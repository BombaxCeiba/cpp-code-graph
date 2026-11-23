module;
// std::system（vcvars 手工复核块需要）经全局模块片段包含。
#include <cstdlib>
// 卸载引擎副本需要平台加载器接口（GetModuleHandleExW/FreeLibrary/
// GetModuleHandleW）。两个防御宏挡掉 min/max 宏与冗余声明；
// rpcndr.h 随 windows.h 进入并会把 interface 定义为宏，本单元
// 有以 interface 为名的形参与成员访问，紧随包含之后撤销该宏。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef interface

export module cg.test.clang;

// cg.clang 的单元测试：Windows 命令分词、编译数据库
// 加载/查找/回退 argv、带诊断的 TU 解析、光标门面
// 查询、主文件过滤、被引用者解析——全部对着
// tests/fixtures/clang 下已提交的夹具（解析输入数据，不是
// 被编译的源码）。CCG_FIXTURE_DIR 由构建注入。

import std;
import cg.util;
import cg.util.cstd;
import cg.clang;
import cg.graph;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace cc = cg::clang;

    constexpr std::string_view fixture_dir = CCG_FIXTURE_DIR;

    [[nodiscard]]
    auto fixture(std::string_view name) -> std::string
    {
        return std::string{fixture_dir} + "/" + std::string{name};
    }

    /**
     * @brief JSON 字符串字面量引用（反斜杠与双引号转义后加引号）。
     *
     * @internal 手拼 compile_commands.json 的用例统一走这里——此前 21 份
     * 逐字拷贝的转义 lambda 散在各用例里，漏转义一处就生成坏库文本。
     */
    [[nodiscard]]
    auto q(std::string_view s) -> std::string
    {
        std::string out = "\"";
        for (const char c : s)
        {
            if (c == '\\' || c == '"')
            {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return out + "\"";
    }

    // 整个 TU 的前序光标转储；过滤 lambda 可返回 false 剪枝。
    template <typename Pred>
    [[nodiscard]]
    auto collect(const cc::Tu& tu, Pred&& keep, bool main_only)
        -> std::vector<CXCursor>
    {
        std::vector<CXCursor> out;
        cc::children(
            tu.cursor(),
            [&](const CXCursor& c)
            {
                if ((!main_only || cc::in_main_file(tu, c)) && keep(c))
                {
                    out.push_back(c);
                }
                return true;
            });
        return out;
    }

    [[nodiscard]]
    auto by_kind_spelling(
        CXCursorKind                 k,
        std::string_view             name,
        const std::vector<CXCursor>& cursors) -> const CXCursor*
    {
        for (const CXCursor& c : cursors)
        {
            if (cc::kind(c) == k && cc::spelling(c) == name)
            {
                return &c;
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // 临时工程夹具（cdb 手拼与 PCH/闭包用例共用；文件级 q 负责 JSON 引用）。

    namespace
    {

        /**
         * @brief 临时工程夹具：ScratchDir + 规范化根路径。
         *
         * @internal 生命周期三件套（唯一名/自清理/put 建父目录）在
         * cg::test::ScratchDir；root 是 norm_path 后的正斜杠绝对路径，
         * compile_commands.json 手拼与路径断言共用同一拼写。
         */
        struct TempProj : cg::test::ScratchDir
        {
            std::string root;

            explicit TempProj(std::string_view tag)
                : ScratchDir(
                      std::filesystem::temp_directory_path()
                      / (std::string{tag}
                         + std::to_string(
                             std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count())))
            {
                root = cg::util::norm_path(base.generic_string());
            }
        };

        /**
         * @brief 在本机 VS 安装里找一个真实 cl.exe（测试用，绝不烤死路径）。
         *
         * 优先 Hostx64/x64（x64 目标）；找不到时返回 ""，相关用例跳过。
         */
        [[nodiscard]]
        auto find_test_cl() -> std::string
        {
            namespace fs = std::filesystem;
            std::vector<std::string> roots;
            for (const char* name : {"ProgramFiles", "ProgramFiles(x86)"})
            {
                if (const auto value = cg::util::cstd::get_env(name);
                    value && !value->empty())
                {
                    roots.push_back(*value);
                }
            }
            // Git Bash / ctest 子进程可能没有 ProgramFiles 环境变量；补标准位置
            // （仅测试发现，运行时代码绝不烤死路径）。
            roots.push_back("C:/Program Files");
            roots.push_back("C:/Program Files (x86)");
            std::string best;
            for (const std::string& pf : roots)
            {
                std::error_code ec;
                const fs::path  vs{pf + "/Microsoft Visual Studio"};
                if (!fs::is_directory(vs, ec))
                {
                    continue;
                }
                for (const fs::directory_entry& release :
                     fs::directory_iterator(vs, ec))
                {
                    if (!release.is_directory(ec))
                    {
                        continue;
                    }
                    for (const fs::directory_entry& edition :
                         fs::directory_iterator(release.path(), ec))
                    {
                        if (!edition.is_directory(ec))
                        {
                            continue;
                        }
                        const fs::path tools = edition.path() / "VC/Tools/MSVC";
                        if (!fs::is_directory(tools, ec))
                        {
                            continue;
                        }
                        for (const fs::directory_entry& version :
                             fs::directory_iterator(tools, ec))
                        {
                            if (!version.is_directory(ec))
                            {
                                continue;
                            }
                            const fs::path cl =
                                version.path() / "bin/Hostx64/x64/cl.exe";
                            if (fs::is_regular_file(cl, ec))
                            {
                                const std::string candidate =
                                    cg::util::norm_path(cl.generic_string());
                                if (candidate > best)
                                {
                                    best = candidate;
                                }
                            }
                        }
                    }
                }
            }
            return best;
        }

        /**
         * @brief 手拼一份 cl 方言的 compile_commands.json（directory/file/
         *        arguments 三件全展开，JSON 引用走文件级 q）。
         */
        [[nodiscard]]
        auto make_cl_db(
            const TempProj&                 proj,
            std::string_view                cl,
            const std::vector<std::string>& files) -> std::string
        {
            std::string text{"["};
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                if (i != 0)
                {
                    text += ",";
                }
                text += "{\"directory\":" + q(proj.root) + ",\"file\":"
                        + q(proj.root + "/" + std::string{files[i]})
                        + ",\"arguments\":[" + q(std::string{cl})
                        + ",\"/c\",\"/I\"," + q(proj.root);
                text += "," + q(std::string{files[i]}) + "]}";
            }
            text += "]";
            proj.put(proj.base / "compile_commands.json", text);
            return cg::util::norm_path(
                (proj.base / "compile_commands.json").generic_string());
        }

    } // namespace

    // ---------------------------------------------------------------------------

    void tokenize_rules(R& r)
    {
        using cg::util::tokenize_command;
        const auto quoted =
            tokenize_command("c++ -I \"../inc with space\" -c a.cpp -o a.o");
        cg::test::check(
            r,
            quoted.size() == 7,
            "plain command splits into 7 tokens");
        cg::test::check(
            r,
            quoted[2] == "../inc with space",
            "quoted include path is one token, quotes removed");

        const auto escaped = tokenize_command(R"("a\"b c" d)");
        cg::test::check(
            r,
            escaped.size() == 2 && escaped[0] == "a\"b c" && escaped[1] == "d",
            "2n+1 backslashes before a quote yield a literal quote");

        const auto collapsing = tokenize_command(R"(a=\\"b c\\" d)");
        cg::test::check(
            r,
            collapsing.size() == 2 && collapsing[0] == "a=\\b c\\"
                && collapsing[1] == "d",
            "2n backslashes before a quote yield n, quote toggles");

        const auto plain_bs = tokenize_command(R"(C:\\path\\ file)");
        cg::test::check(
            r,
            plain_bs.size() == 2 && plain_bs[0] == R"(C:\\path\\)"
                && plain_bs[1] == "file",
            "backslashes not followed by a quote stay literal");

        cg::test::check(
            r,
            tokenize_command("").empty(),
            "empty input, no tokens");
        cg::test::check(
            r,
            tokenize_command("   ").empty(),
            "only whitespace, no tokens");
        const auto trimmed = tokenize_command("  -std=c++23  ");
        cg::test::check(
            r,
            trimmed.size() == 1 && trimmed[0] == "-std=c++23",
            "surrounding whitespace is dropped");
    }

    void compile_db_load(R& r)
    {
        const auto db = cc::load(fixture("compile_commands.json"), {});
        cg::test::check(r, db.size() == 2, "both fixture entries load");

        if (db.size() == 2)
        {
            const std::string main_path =
                cg::util::norm_path(fixture("baseline_main.cpp"));
            const std::string broken_path =
                cg::util::norm_path(fixture("broken.cpp"));
            cg::test::check(
                r,
                db[0].file == main_path,
                "entry file resolves absolute against its directory");
            cg::test::check(
                r,
                db[0].directory == cg::util::norm_path(fixture_dir),
                "relative directory resolves against the db location");
            cg::test::check(r, db[1].file == broken_path, "second entry file");

            cg::test::check(
                r,
                db[0].argv.size() == 3 && db[0].argv[0] == "-std=c++23"
                    && db[0].argv[1] == "-I"
                    && db[0].argv[2]
                           == cg::util::norm_path(
                               (std::filesystem::path{fixture_dir} / ".."
                                / "fixtures" / "clang" / "inc with space")
                                   .lexically_normal()
                                   .generic_string()),
                "argv drops argv[0]/-c/-o; relative -I resolves "
                "against the entry directory");
            cg::test::check(
                r,
                db[1].argv.size() == 2 && db[1].argv[0] == "-std=c++23"
                    && db[1].argv[1] == "-DCCG_FIXTURE=1",
                "second entry argv shape");
        }

        const cc::CompileCommand* hit = cc::find_command(
            db,
            cg::util::norm_path(fixture("baseline_main.cpp")));
        cg::test::check(
            r,
            hit != nullptr && hit->file == db[0].file,
            "find_command hits on the exact path");
        const cc::CompileCommand* suffix =
            cc::find_command(db, "baseline_main.cpp");
        cg::test::check(
            r,
            suffix == &db[0],
            "find_command hits on a path-segment suffix");
        const cc::CompileCommand* near_miss =
            cc::find_command(db, "aseline_main.cpp");
        cg::test::check(
            r,
            near_miss == nullptr,
            "suffix match respects segment boundaries");
        cg::test::check(
            r,
            cc::find_command(db, "no_such.cpp") == nullptr,
            "miss returns nullptr");

        // 根过滤。
        const std::vector<std::string> keep_roots{std::string{fixture_dir}};
        const auto                     roots_all =
            cc::load(fixture("compile_commands.json"), keep_roots);
        cg::test::check(
            r,
            roots_all.size() == 2,
            "root filter covering the fixture dir keeps both");
        const std::vector<std::string> drop_roots{fixture("nowhere")};
        const auto                     roots_none =
            cc::load(fixture("compile_commands.json"), drop_roots);
        cg::test::check(
            r,
            roots_none.empty(),
            "non-matching root filter drops everything");
        const std::vector<std::string> boundary_roots{
            fixture("clang") + "/baseline"};
        const auto roots_boundary =
            cc::load(fixture("compile_commands.json"), boundary_roots);
        cg::test::check(
            r,
            roots_boundary.empty(),
            "root prefix without a segment boundary does not match");
    }

    // 模块条目捕获契约：
    /// 夹具目录内相对路径的规范化拼接。
    [[nodiscard]]
    auto path_under(const std::string& base, std::string_view rel)
        -> std::string
    {
        return cg::util::norm_path((std::filesystem::path{base} / rel)
                                       .lexically_normal()
                                       .generic_string());
    }

    // 编译器令牌保留为 BMI 生产者身份，@modmap 展开
    // 记录产出的 BMI（module_output）与喂入的 BMI
    // （module_feeds，绝对路径），modmap 路径锚定 BMI 映射，
    // 纯命令行的模块旗标以同样方式捕获。
    void compile_db_module_feeds(R& r)
    {
        const auto db = cc::load(fixture("mod_commands.json"), {});
        cg::test::check(r, db.size() == 2, "both module fixture entries load");
        if (db.size() != 2)
        {
            return;
        }
        const std::string fix = cg::util::norm_path(fixture_dir);

        cg::test::check(
            r,
            db[0].compiler == "clang++.exe",
            "compiler token (argv[0]) is kept aside");
        cg::test::check(
            r,
            db[0].module_output == path_under(fix, "build/mod_out.pcm"),
            "-fmodule-output= from the rsp is recorded absolute");
        cg::test::check(
            r,
            db[0].module_map == path_under(fix, "mod_rsp.modmap"),
            "the modmap response file path is recorded");
        cg::test::check(
            r,
            db[0].module_feeds.size() == 2
                && db[0].module_feeds[0].name == "std"
                && db[0].module_feeds[0].pcm == path_under(fix, "build/std.pcm")
                && db[0].module_feeds[1].name == "cg.util"
                && db[0].module_feeds[1].pcm
                       == path_under(fix, "build/cg.util.pcm"),
            "rsp -fmodule-file=NAME=path feeds capture name + "
            "absolute pcm");
        bool out_dropped = true;
        bool x_present = false;
        bool feed_resolved = false;
        for (std::size_t i = 0; i < db[0].argv.size(); ++i)
        {
            if (db[0].argv[i].starts_with("-fmodule-output="))
            {
                out_dropped = false;
            }
            if (i + 1 < db[0].argv.size() && db[0].argv[i] == "-x"
                && db[0].argv[i + 1] == "c++-module")
            {
                x_present = true;
            }
            if (db[0].argv[i]
                == "-fmodule-file=std=" + path_under(fix, "build/std.pcm"))
            {
                feed_resolved = true;
            }
        }
        cg::test::check(
            r,
            out_dropped,
            "-fmodule-output= stays out of the replay argv");
        cg::test::check(r, x_present, "-x c++-module from the rsp replays");
        cg::test::check(
            r,
            feed_resolved,
            "feed flags replay with resolved absolute paths");
        cg::test::check(
            r,
            cc::is_module_tu(db[0]),
            "rsp -x c++-module marks the entry a module TU");

        cg::test::check(
            r,
            db[1].compiler == "clang++.exe" && db[1].module_output.empty()
                && db[1].module_map.empty() && db[1].module_feeds.size() == 1
                && db[1].module_feeds[0].name == "std"
                && db[1].module_feeds[0].pcm
                       == path_under(fix, "build/std.pcm"),
            "plain-command-line module flags capture identically");
        cg::test::check(
            r,
            cc::is_module_tu(db[1]) == false,
            "a classic TU with a module feed flag is not itself a module unit");
    }

    // BMI 生产者检查两侧使用的可比版本令牌。
    void version_token_extraction(R& r)
    {
        cg::test::check(
            r,
            cc::clang_version_token(
                "clang version 23.1.3 (https://github.com/msys2/MINGW-packages "
                "067886dba977a6b4c97dfedfb25286b5eda55620)")
                == "23.1.3",
            "driver --version first line yields the token");
        cg::test::check(
            r,
            cc::clang_version_token("clang version 22.1.3\nTarget: ...")
                == "22.1.3",
            "token ends at the newline");
        cg::test::check(
            r,
            cc::clang_version_token("clang version ").empty(),
            "no token after the marker");
        cg::test::check(
            r,
            cc::clang_version_token(
                "gcc.exe (Rev3, Built by MSYS2 project) 15.2.0")
                .empty(),
            "non-clang version lines yield no token");
    }

    // ------------------------------------------------- bmi 层
    // ----------

    /**
     * @brief 读入测试夹具全文（二进制口径；缺失时得空串）。
     *
     * @internal 读取走 cg::util::read_text_file 单点，夹具路径由 fixture
     * 拼出；调用方解析空文本自然失败，无需区分缺失与空文件。
     */
    [[nodiscard]]
    auto read_fixture(std::string_view name) -> std::string
    {
        return cg::util::read_text_file(fixture(name)).value_or(std::string{});
    }

    /**
     * @brief 读入 BMI 清单并解析（缺失/坏文本得 nullopt）。
     *
     * @internal self-build 用例反复核对 manifest.json（初轮/失效后/
     * 重建后）；读取走 cg::util::read_text_file 单点，坏文本由解析层
     * 归为 nullopt。
     */
    [[nodiscard]]
    auto read_bmi_manifest(const std::filesystem::path& path)
        -> std::optional<cc::BmiManifest>
    {
        const std::optional<std::string> text =
            cg::util::read_text_file(path.generic_string());
        if (!text.has_value())
        {
            return std::nullopt;
        }
        return cc::detail::manifest_from_json(*text);
    }

    /**
     * @brief 读入 PCH 影子清单并解析（缺失/坏文本得 nullopt）。
     *
     * @internal 影子规划用例核对 manifest.json 的槽位与摘要；读取走
     * cg::util::read_text_file 单点，坏文本由解析层归为 nullopt。
     */
    [[nodiscard]]
    auto read_pch_manifest(const std::filesystem::path& path)
        -> std::optional<cc::detail::PchManifest>
    {
        const std::optional<std::string> text =
            cg::util::read_text_file(path.generic_string());
        if (!text.has_value())
        {
            return std::nullopt;
        }
        return cc::detail::pch_manifest_from_json(*text);
    }

    void bmi_p1689_parse(R& r)
    {
        // demo 树上的真实 clang-scan-deps 输出（revision 0，rules
        // 形态）：主单元 + 两个分区 + 一个 .cpp 消费者。
        const std::vector<cc::ScanRule> rules =
            cc::detail::parse_p1689(read_fixture("p1689_scan.json"));
        cg::test::check(r, rules.size() == 4, "four rules parse");
        std::map<std::string, std::vector<std::string>> requires_of;
        for (const cc::ScanRule& rule : rules)
        {
            for (const cc::ScanProvide& p : rule.provides)
            {
                std::vector<std::string> names;
                names.reserve(rule.requires_names.size());
                for (const cc::ScanRequire& rq : rule.requires_names)
                {
                    names.push_back(rq.name);
                }
                requires_of.emplace(p.name, names);
            }
        }
        cg::test::check(
            r,
            requires_of.contains("demo") && requires_of.contains("demo:core")
                && requires_of.contains("demo:math"),
            "partitions are their own provides rules");
        cg::test::check(
            r,
            requires_of["demo"]
                == std::vector<std::string>({"demo:core", "demo:math", "std"}),
            "primary's requires carries partitions + std");
        // 消费者规则没有 provides；其 requires 点名主单元。
        bool consumer = false;
        for (const cc::ScanRule& rule : rules)
        {
            if (rule.provides.empty() && rule.requires_names.size() == 2
                && rule.requires_names[0].name == "demo")
            {
                consumer = true;
            }
        }
        cg::test::check(r, consumer, "consumer rule parses with its requires");
        // revision 1 形态（translation-units）解析结果一致。
        const std::string v1 =
            R"({"revision":1,"translation-units":[{"file":"a.ixx",
        "provides":[{"logical-name":"a","is-interface":true,"source-path":"a.ixx"}],
        "requires":[{"logical-name":"std"}]}]})";
        const std::vector<cc::ScanRule> v1_rules = cc::detail::parse_p1689(v1);
        cg::test::check(
            r,
            v1_rules.size() == 1 && v1_rules[0].provides[0].name == "a",
            "revision 1 translation-units shape tolerated");
        // 坏文本降级为空（绝不轻信）。
        cg::test::check(
            r,
            cc::detail::parse_p1689("{not json").empty(),
            "malformed scan output yields no rules");
        cg::test::check(
            r,
            cc::detail::parse_p1689("null").empty(),
            "non-object scan output yields no rules");
    }

    void bmi_modules_json_parse(R& r)
    {
        // libc++ 形态：相对源路径 + 声明的系统 include 目录。
        const cc::detail::ModulesJson libcxx = cc::detail::parse_modules_json(
            read_fixture("libcxx_modules.json"),
            "E:/fake/clang64/lib");
        cg::test::check(
            r,
            libcxx.std_path == "e:/fake/clang64/share/libc++/v1/std.cppm"
                && libcxx.compat_path
                       == "e:/fake/clang64/share/libc++/v1/std.compat.cppm",
            "libc++ json resolves std/compat against its dir");
        cg::test::check(
            r,
            libcxx.wiring
                == std::vector<std::string>(
                    {"e:/fake/clang64/share/libc++/v1"}),
            "declared system-include-directories become wiring");
        // libstdc++ 形态：相对源路径，无本地实参。
        const cc::detail::ModulesJson gnu = cc::detail::parse_modules_json(
            read_fixture("libstdcxx_modules.json"),
            "E:/fake/ucrt64/lib");
        cg::test::check(
            r,
            gnu.std_path == "e:/fake/ucrt64/include/c++/16.2.0/bits/std.cc",
            "libstdc++ json resolves bits/std.cc");
        cg::test::check(
            r,
            gnu.wiring.empty(),
            "libstdc++ json declares no wiring");
        // 缺失/陌生名字与坏文本保持为空。
        cg::test::check(
            r,
            cc::detail::parse_modules_json("{}", "x").std_path.empty(),
            "empty json yields no std source");
        cg::test::check(
            r,
            cc::detail::parse_modules_json("[1,2", "x").std_path.empty(),
            "malformed json yields no std source (never trusted)");
    }

    void bmi_helper_rules(R& r)
    {
        // pcm 名字清理：分区与冲突。
        cg::test::check(
            r,
            cc::detail::sanitize_pcm_name("cg.util") == "cg.util",
            "plain names stay untouched");
        const std::string part = cc::detail::sanitize_pcm_name("demo:core");
        cg::test::check(
            r,
            part.starts_with("demo_core-") && part.size() == 18,
            "colon becomes '_' plus an 8-hex name hash");
        cg::test::check(
            r,
            cc::detail::sanitize_pcm_name("demo:core")
                == cc::detail::sanitize_pcm_name("demo:core"),
            "sanitization is deterministic");
        cg::test::check(
            r,
            cc::detail::sanitize_pcm_name("demo_core")
                != cc::detail::sanitize_pcm_name("demo:core"),
            "a literal underscore name cannot collide");
        // MSVC 工具集下限（14.44）。
        cg::test::check(
            r,
            cc::detail::toolset_at_floor("14.44.35207"),
            "14.44 passes the floor");
        cg::test::check(
            r,
            cc::detail::toolset_at_floor("14.51.36231"),
            "14.51 passes the floor");
        cg::test::check(
            r,
            !cc::detail::toolset_at_floor("14.38.33130"),
            "14.38 is below the floor");
        cg::test::check(
            r,
            !cc::detail::toolset_at_floor("14"),
            "unparseable versions fail the floor");
        // 从驱动报告的 include 清单提取工具集 + SDK。
        const auto [toolset, version] = cc::detail::msvc_toolset_from_report(
            {"C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/"
             "MSVC/14.51.36231/include",
             "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/"
             "ucrt"});
        cg::test::check(
            r,
            version == "14.51.36231",
            "toolset version parsed from the include root");
        cg::test::check(
            r,
            toolset.ends_with("MSVC/14.51.36231"),
            "toolset dir is the include root's parent");
        cg::test::check(
            r,
            cc::detail::windows_sdk_from_report(
                {"C:/Program Files (x86)/Windows Kits/10/Include/"
                 "10.0.26100.0/ucrt"})
                == "10.0.26100.0",
            "Windows SDK version parsed from the kits root");
        // 失败文本优先取第一条诊断行。
        cg::test::check(
            r,
            cc::detail::first_line_of("note: here\nfile.cpp:3:2: error: broken")
                == "file.cpp:3:2: error: broken",
            "error lines win over earlier noise");
        // 数据库路由的配置签名（所记录 .modmap 路径的
        // ".dir/`<Config>`/" 判别子；多配置库按配置逐趟路由）。
        // 合成的 posix 拼写：该函数是纯字符串手术。
        {
            cc::CompileCommand cc;
            cg::test::check(
                r,
                cc::db_config_signature(cc).empty(),
                "no modmap -> empty signature");
            cc.module_map = "/b/src/CMakeFiles/cg_core.dir/Release/util/"
                            "util.ixx.obj.modmap";
            cg::test::check(
                r,
                cc::db_config_signature(cc) == "Release",
                "CMake per-config modmap yields the config name");
            cc.module_map = "/b/src/CMakeFiles/cg_core.dir/Debug/x.modmap";
            cg::test::check(
                r,
                cc::db_config_signature(cc) == "Debug",
                "Debug config parsed");
            cc.module_map = "/b/x.obj.modmap";
            cg::test::check(
                r,
                cc::db_config_signature(cc).empty(),
                "no .dir/ segment -> empty signature");
            cc.module_map = "/b/CMakeFiles/cg_core.dir//x.modmap";
            cg::test::check(
                r,
                cc::db_config_signature(cc).empty(),
                "empty config segment declines (not a config)");
        }
        // text_has_import 预过滤（导出给路由层 +
        // 索引器的喂入缺失守卫）。
        {
            const std::filesystem::path tmp =
                std::filesystem::temp_directory_path()
                / ("ccg-bmi-import-"
                   + std::to_string(
                       std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())
                   + ".ixx");
            {
                std::ofstream of{tmp, std::ios::binary};
                of << "// import std;\nmodule;\nexport module t;\n";
            }
            cg::test::check(
                r,
                !cc::text_has_import(tmp.generic_string()),
                "commented import does not count");
            {
                std::ofstream of{tmp, std::ios::binary};
                of << "module;\nexport module t;\nexport import cg.util;\n";
            }
            cg::test::check(
                r,
                cc::text_has_import(tmp.generic_string()),
                "export import counts as an import line");
            cg::test::check(
                r,
                !cc::text_has_import("/definitely/not/here.ixx"),
                "unreadable file reports no import (parse will fail loudly)");
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
    }

    void bmi_manifest_roundtrip(R& r)
    {
        // 清单 json 往返 + stat/哈希新鲜度纪律。
        cc::BmiManifest m;
        m.engine = "clang version 23.1.3 (test)";
        m.engine_key = "0123456789abcdef";
        m.config_key = "cfg";
        m.toolset_id = "libc++:E:/fake";
        cc::BmiEntry e;
        e.name = "demo:core";
        e.source = "E:/does/not/exist.ixx";
        e.hash = "deadbeef";
        e.mtime = 17;
        e.size = 42;
        e.command = "cmd";
        e.requires_names = {"std"};
        e.pcm = "cfg/demo_core-1fd90a99.pcm";
        e.pcm_size = 123456;
        e.config = "Release";
        m.entries.push_back(e);
        const std::string text = cc::detail::manifest_to_json(m);
        const std::optional<cc::BmiManifest> back =
            cc::detail::manifest_from_json(text);
        cg::test::check(r, back.has_value(), "manifest json parses back");
        cg::test::check(
            r,
            back->engine == m.engine && back->engine_key == m.engine_key
                && back->config_key == m.config_key
                && back->toolset_id == m.toolset_id,
            "identity fields survive the round-trip");
        cg::test::check(
            r,
            back->entries.size() == 1 && back->entries[0].name == e.name
                && back->entries[0].hash == e.hash
                && back->entries[0].mtime == e.mtime
                && back->entries[0].size == e.size
                && back->entries[0].command == e.command
                && back->entries[0].pcm == e.pcm
                && back->entries[0].pcm_size == e.pcm_size
                && back->entries[0].config == e.config
                && back->entries[0].requires_names
                       == std::vector<std::string>({"std"}),
            "entry fields survive the round-trip");
        // 没有配置字段的清单（数据库路由前的 schema）解析
        // 回来是 ""——结转逻辑把它们当同配置的遗留
        // 清除而非结转。
        {
            const std::optional<cc::BmiManifest> legacy =
                cc::detail::manifest_from_json(
                    "{\"engine\":\"e\",\"engine_key\":\"k\",\"config_key\":\"c\","
                    "\"entries\":[{\"name\":\"n\",\"source\":\"s\",\"hash\":\"h\","
                    "\"command\":\"c\",\"pcm\":\"p.pcm\"}]}");
            cg::test::check(
                r,
                legacy.has_value() && legacy->entries[0].config.empty()
                    && legacy->entries[0].pcm_size == 0,
                "legacy entry without config/pcm_size parses "
                "conservatively (size 0 forces rebuild)");
        }
        cg::test::check(
            r,
            !cc::detail::manifest_from_json("{bad").has_value(),
            "malformed manifest degrades to absent");

        // 新鲜度：先走 stat 快路径，漂移时哈希确认。
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path()
            / ("ccg-bmi-test-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())
               + ".txt");
        {
            std::ofstream of{tmp, std::ios::binary};
            of << "body";
        }
        cc::BmiEntry live;
        live.source = tmp.generic_string();
        live.hash = cg::util::sha256_file_hex(tmp.generic_string());
        const cc::detail::BmiStat st = cc::detail::stat_source(live.source);
        live.mtime = st.mtime;
        live.size = st.size;
        cg::test::check(
            r,
            cc::detail::check_source(live.source, &live).matches_prior,
            "stat fast path confirms unchanged content");
        // 字节相同、stat 身份更新：哈希确认命中。
        cc::BmiEntry touched = live;
        touched.mtime += 1;
        touched.size += 1;
        cg::test::check(
            r,
            cc::detail::check_source(live.source, &touched).matches_prior,
            "hash confirms content after stat drift");
        // 字节不同：不匹配。
        {
            std::ofstream of{tmp, std::ios::binary};
            of << "body2";
        }
        cg::test::check(
            r,
            !cc::detail::check_source(live.source, &live).matches_prior,
            "changed content does not match the recorded hash");
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }

    // 复用门：0 字节/半截/尺寸不符/头部损坏/旧清单（无 pcm_size）一律
    // 不可复用。
    void bmi_cached_pcm_size_gate(R& r)
    {
        namespace fs = std::filesystem;
        TempProj       proj{"ccg-bmi-size-"};
        const fs::path pcm = proj.base / "m.pcm";
        {
            std::ofstream out{pcm, std::ios::binary};
            out << "CPCHabc";
        }
        const std::string path = pcm.generic_string();
        cg::test::check(
            r,
            cc::detail::cached_pcm_size(path, 7) == 7,
            "matching recorded size with a CPCH header is reusable");
        cg::test::check(
            r,
            cc::detail::cached_pcm_size(path, 0) == -1,
            "a legacy entry without pcm_size rebuilds conservatively");
        cg::test::check(
            r,
            cc::detail::cached_pcm_size(path, 8) == -1,
            "size mismatch is rejected (half-written pcm)");
        {
            // 尺寸与清单一致但头部被覆盖：下游会以 bmi-config-mismatch
            // 逐 TU 失败，必须在复用门就判为不可加载。
            std::ofstream out{pcm, std::ios::binary | std::ios::trunc};
            out << "XXXXabc";
        }
        cg::test::check(
            r,
            cc::detail::cached_pcm_size(path, 7) == -1,
            "a same-size pcm with a corrupted header is rejected");
        {
            std::ofstream out{pcm, std::ios::binary | std::ios::trunc};
        }
        cg::test::check(
            r,
            cc::detail::cached_pcm_size(path, 0) == -1
                && cc::detail::cached_pcm_size(path, 3) == -1,
            "0-byte pcm is never reusable");
        cg::test::check(
            r,
            cc::detail::artifact_size(path) == 0,
            "artifact_size reports an empty artifact as 0");
    }

    /// 读文件全文（缺失时给空串）。
    [[nodiscard]]
    auto read_file_or_empty(const std::filesystem::path& p) -> std::string
    {
        return cg::util::read_text_file(p.generic_string())
            .value_or(std::string{});
    }

    // donor 校验：0 字节/尺寸不符/头部损坏的 donor 绝不复制，目标保持原样。
    void bmi_copy_donor_verified(R& r)
    {
        namespace fs = std::filesystem;
        TempProj        proj{"ccg-bmi-donor-"};
        std::error_code ec;
        const fs::path  donor = proj.base / "donor.pcm";
        const fs::path  target = proj.base / "cfga" / "m.pcm";
        {
            std::ofstream out{donor, std::ios::binary};
            out << "CPCHgood";
        }
        const std::string donor_path = donor.generic_string();
        const std::string target_path = target.generic_string();
        cg::test::check(
            r,
            cc::detail::copy_donor_verified(donor_path, target_path, 8),
            "a sized donor with a CPCH header copies");
        cg::test::check(
            r,
            fs::exists(target, ec) && read_file_or_empty(target) == "CPCHgood",
            "the copied target carries the donor bytes");

        // 0 字节 donor：拒绝，目标不存在。
        const fs::path empty_donor = proj.base / "empty.pcm";
        {
            std::ofstream out{empty_donor, std::ios::binary};
        }
        const fs::path empty_target = proj.base / "cfgb" / "m.pcm";
        cg::test::check(
            r,
            !cc::detail::copy_donor_verified(
                empty_donor.generic_string(),
                empty_target.generic_string(),
                0),
            "a 0-byte donor is never copied");
        cg::test::check(
            r,
            !fs::exists(empty_target, ec),
            "the rejected 0-byte donor leaves no target");

        // 头部损坏（尺寸与台账一致）：拒绝，目标不存在。
        const fs::path corrupt_donor = proj.base / "corrupt.pcm";
        {
            std::ofstream out{corrupt_donor, std::ios::binary};
            out << "XXXXgood";
        }
        const fs::path corrupt_target = proj.base / "cfgd" / "m.pcm";
        cg::test::check(
            r,
            !cc::detail::copy_donor_verified(
                corrupt_donor.generic_string(),
                corrupt_target.generic_string(),
                8),
            "a same-size donor with a corrupted header is rejected");
        cg::test::check(
            r,
            !fs::exists(corrupt_target, ec),
            "the rejected corrupted donor leaves no target");

        // 尺寸不符：拒绝，且旧目标原样保留。
        const fs::path stale = proj.base / "cfgc" / "m.pcm";
        fs::create_directories(stale.parent_path(), ec);
        {
            std::ofstream out{stale, std::ios::binary};
            out << "old";
        }
        cg::test::check(
            r,
            !cc::detail::copy_donor_verified(
                donor_path,
                stale.generic_string(),
                9),
            "a donor whose size disagrees with the ledger is rejected");
        cg::test::check(
            r,
            fs::exists(stale, ec) && read_file_or_empty(stale) == "old",
            "the failed verification keeps the old target");
        // 复制失败/尺寸不符时必须清理临时文件。
        std::size_t leftovers = 0;
        for (const fs::directory_entry& e :
             fs::recursive_directory_iterator(proj.base, ec))
        {
            if (e.path().filename().generic_string().ends_with(".tmp"))
            {
                ++leftovers;
            }
        }
        cg::test::check(r, leftovers == 0, "donor verification leaves no tmp");
    }

    /// 清单条目的合成构造（尺寸恒 1 的最小行）。
    [[nodiscard]]
    auto make_bmi_entry(
        std::string_view name,
        std::string_view command,
        std::string_view pcm) -> cc::BmiEntry
    {
        cc::BmiEntry e;
        e.name = name;
        e.command = command;
        e.pcm = pcm;
        e.pcm_size = 1;
        return e;
    }

    // 并发写者不丢其它配置条目：写前并集并集是“不丢”的核心。
    void bmi_manifest_merge_preserves_entries(R& r)
    {
        cc::BmiManifest mine;
        mine.entries.push_back(make_bmi_entry("a", "cmd-a2", "cfga/a.pcm"));
        cc::BmiManifest disk; // 另一个进程刚写下的清单
        disk.entries.push_back(make_bmi_entry("a", "cmd-a2", "cfga/a.pcm"));
        disk.entries.push_back(make_bmi_entry("b", "cmd-b", "cfgb/b.pcm"));
        const std::size_t merged =
            cc::detail::merge_manifest_entries(mine, disk);
        cg::test::check(
            r,
            merged == 1 && mine.entries.size() == 2,
            "merge unions the other config's entry exactly once");
        cg::test::check(
            r,
            mine.entries[1].name == "b",
            "the sibling config entry survives the merge");
        cc::BmiManifest fresh_target;
        fresh_target.entries.push_back(
            make_bmi_entry("a", "cmd-a2", "cfga/a.pcm"));
        const std::unordered_set<std::string> dropped{
            cc::detail::bmi_entry_key("b", "cmd-b")};
        const std::size_t again =
            cc::detail::merge_manifest_entries(fresh_target, disk, dropped);
        cg::test::check(
            r,
            again == 0 && fresh_target.entries.size() == 1,
            "an explicitly dropped key is never resurrected");
    }

    // 失败条目失效：条目原子移除、产物删除，兄弟配置条目原样保留。
    void bmi_invalidate_cached_artifact(R& r)
    {
        namespace fs = std::filesystem;
        TempProj        proj{"ccg-bmi-inval-"};
        std::error_code ec;
        const fs::path  engine_dir =
            proj.base / "bmi-self-test" / "0123456789abcdef";
        fs::create_directories(engine_dir / "cfga" / "", ec);
        fs::create_directories(engine_dir / "cfgb", ec);
        const fs::path bad = engine_dir / "cfga" / "a.pcm";
        const fs::path good = engine_dir / "cfgb" / "b.pcm";
        {
            std::ofstream out{bad, std::ios::binary};
            out << "bad-bytes";
            std::ofstream out2{good, std::ios::binary};
            out2 << "good";
        }
        cc::BmiManifest m;
        m.engine = "engine";
        m.engine_key = "0123456789abcdef";
        m.config_key = "cfga";
        for (const auto& [name, rel, size] :
             {std::tuple{"a", std::string{"cfga/a.pcm"}, 9},
              std::tuple{"b", std::string{"cfgb/b.pcm"}, 4}})
        {
            cc::BmiEntry e;
            e.name = name;
            e.source = "s";
            e.hash = "h";
            e.command = "c";
            e.pcm = rel;
            e.pcm_size = size;
            m.entries.push_back(e);
        }
        {
            std::ofstream out{engine_dir / "manifest.json", std::ios::binary};
            out << cc::detail::manifest_to_json(m);
        }
        const std::size_t removed =
            cc::invalidate_cached_bmi_artifacts({bad.generic_string()});
        cg::test::check(r, removed == 1, "exactly one manifest entry removed");
        cg::test::check(
            r,
            !fs::exists(bad, ec),
            "failed cached pcm artifact is deleted");
        cg::test::check(
            r,
            fs::exists(good, ec),
            "sibling config artifact survives invalidation");
        const std::string manifest_text =
            cg::util::read_text_file(
                (engine_dir / "manifest.json").generic_string())
                .value_or(std::string{});
        const auto back = cc::detail::manifest_from_json(manifest_text);
        cg::test::check(
            r,
            back.has_value() && back->entries.size() == 1
                && back->entries[0].name == "b",
            std::format(
                "manifest is rewritten atomically and still "
                "valid JSON (entries={}, text={})",
                back ? back->entries.size() : 0,
                manifest_text));
    }

    // PCH 失败条目失效：slot 目录删除、清单原子重写。
    void pch_invalidate_cached_artifact(R& r)
    {
        namespace fs = std::filesystem;
        TempProj        proj{"ccg-pch-inval-"};
        std::error_code ec;
        const fs::path  engine_dir =
            proj.base / "cfg" / "pch" / "0123456789abcdef";
        fs::create_directories(engine_dir / "slota", ec);
        fs::create_directories(engine_dir / "slotb", ec);
        const fs::path bad = engine_dir / "slota" / "shadow.pch";
        const fs::path good = engine_dir / "slotb" / "shadow.pch";
        {
            std::ofstream out{bad, std::ios::binary};
            out << "bad";
            std::ofstream out2{good, std::ios::binary};
            out2 << "good";
        }
        cc::detail::PchManifest m;
        m.engine = "engine";
        m.engine_key = "0123456789abcdef";
        for (const auto& [slot, hash] :
             {std::pair{std::string{"slota"}, std::string{"ha"}},
              std::pair{std::string{"slotb"}, std::string{"hb"}}})
        {
            cc::detail::PchEntry e;
            e.slot_key = slot;
            e.pch_key = "pk";
            e.pch_hash = hash;
            e.pch_size = 3;
            m.entries.push_back(e);
        }
        {
            std::ofstream out{engine_dir / "manifest.json", std::ios::binary};
            out << cc::detail::pch_manifest_to_json(m);
        }
        const std::size_t removed =
            cc::invalidate_cached_pch_artifacts({bad.generic_string()});
        cg::test::check(r, removed == 1, "exactly one PCH entry removed");
        cg::test::check(
            r,
            !fs::exists(engine_dir / "slota", ec),
            "unusable shadow slot directory is deleted");
        cg::test::check(r, fs::exists(good, ec), "other shadow slots survive");
        const auto back = read_pch_manifest(engine_dir / "manifest.json");
        cg::test::check(
            r,
            back.has_value() && back->entries.size() == 1
                && back->entries[0].slot_key == "slotb",
            "PCH manifest is rewritten atomically and stays valid");
    }

    // 固定 tmp 名并发互踩：唯一 tmp + 原子替换下，最终清单永远是合法
    // JSON（一个完整写入者的版本），且临时文件不残留。
    void pch_manifest_concurrent_atomic_write(R& r)
    {
        namespace fs = std::filesystem;
        TempProj                  proj{"ccg-pch-atomic-"};
        std::error_code           ec;
        const fs::path            path = proj.base / "manifest.json";
        std::vector<std::jthread> writers;
        for (int t = 0; t < 3; ++t)
        {
            writers.emplace_back(
                [&, t]
                {
                    for (int i = 0; i < 25; ++i)
                    {
                        cc::detail::PchManifest m;
                        m.engine = "engine";
                        m.engine_key = "k";
                        cc::detail::PchEntry e;
                        e.slot_key = std::format("slot{}-{}", t, i);
                        e.pch_key = "pk";
                        e.pch_size = 1;
                        m.entries.push_back(e);
                        (void)cg::util::platform::atomic_write_file(
                            path,
                            cc::detail::pch_manifest_to_json(m));
                    }
                });
        }
        writers.clear();
        const auto text = cc::detail::read_text(path.generic_string());
        cg::test::check(r, text.has_value(), "concurrent writes leave a file");
        const auto parsed =
            text ? cc::detail::pch_manifest_from_json(*text) : std::nullopt;
        cg::test::check(
            r,
            parsed.has_value() && parsed->entries.size() == 1,
            "the final manifest is one writer's complete valid JSON");
        std::size_t leftovers = 0;
        for (const fs::directory_entry& e :
             fs::directory_iterator(proj.base, ec))
        {
            if (e.path().filename().generic_string().ends_with(".tmp"))
            {
                ++leftovers;
            }
        }
        cg::test::check(
            r,
            leftovers == 0,
            "unique tmp names are renamed away, none left behind");
    }

    // 硬门禁：0 字节 pcm + 完好清单 → 下一次自建运行必须重建成功，
    // 清单的 pcm_size 随之更新。donor/复用门都不得放行空产物。
    void bmi_zero_byte_pcm_rebuilds(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "zero-byte rebuild skipped: no driver");
            return;
        }
        TempProj        proj{"ccg-bmi-zero-"};
        std::error_code ec;
        fs::create_directories(proj.base / "proj", ec);
        fs::create_directories(proj.base / "cfg", ec);
        {
            std::ofstream a{proj.base / "proj" / "a.ixx", std::ios::binary};
            a << "export module a;\nexport int fa(){return 1;}\n";
            std::ofstream b{proj.base / "proj" / "b.ixx", std::ios::binary};
            b << "export module b;\nimport a;\nexport int fb(){return fa()+1;}\n";
        }
        const std::string root =
            cg::util::norm_path((proj.base / "proj").generic_string());
        const std::vector<std::string> units{
            cg::util::norm_path(
                (proj.base / "proj" / "a.ixx").generic_string()),
            cg::util::norm_path(
                (proj.base / "proj" / "b.ixx").generic_string())};
        cc::SelfBuildOptions opt;
        opt.root = root;
        opt.ccg_config_dir =
            cg::util::norm_path((proj.base / "cfg").generic_string());
        opt.std_value = "c++20";
        std::vector<cc::CompileCommand> classic;
        const cc::SelfBuildResult       first =
            cc::self_build_modules(opt, units, classic);
        cg::test::check(
            r,
            first.built >= 2 && first.degraded == 0,
            "first self-build produces the module BMIs");
        const fs::path manifest_path =
            fs::path{first.cache_dir} / "manifest.json";
        const auto initial = read_bmi_manifest(manifest_path);
        cg::test::check(r, initial.has_value(), "self-build writes a manifest");
        if (!initial.has_value())
        {
            return;
        }
        std::string  a_pcm;
        std::int64_t a_size = 0;
        for (const cc::BmiEntry& e : initial->entries)
        {
            if (e.name == "a")
            {
                a_pcm = cg::util::norm_path(
                    (fs::path{first.cache_dir} / fs::path{e.pcm})
                        .lexically_normal()
                        .generic_string());
                a_size = e.pcm_size;
            }
        }
        cg::test::check(
            r,
            !a_pcm.empty() && a_size > 0
                && fs::file_size(fs::path{a_pcm}, ec)
                       == static_cast<std::uintmax_t>(a_size),
            "manifest records the produced pcm size");
        {
            std::ofstream trunc{a_pcm, std::ios::binary | std::ios::trunc};
        }
        std::vector<cc::CompileCommand> classic2;
        const cc::SelfBuildResult       second =
            cc::self_build_modules(opt, units, classic2);
        cg::test::check(
            r,
            second.built >= 1 && second.degraded == 0,
            "a 0-byte pcm forces a rebuild in the next run");
        const std::int64_t rebuilt = cc::detail::artifact_size(a_pcm);
        cg::test::check(r, rebuilt > 0, "the rebuilt pcm is non-empty");
        const auto   after = read_bmi_manifest(manifest_path);
        std::int64_t recorded = 0;
        if (after.has_value())
        {
            for (const cc::BmiEntry& e : after->entries)
            {
                if (e.name == "a")
                {
                    recorded = e.pcm_size;
                }
            }
        }
        cg::test::check(
            r,
            recorded == rebuilt,
            "manifest pcm_size follows the rebuilt artifact");

        // 同轮收敛（G 的 BMI 分支）：尺寸与清单一致但头部被覆盖的 pcm
        // 必须在解析前判为不可加载，并在同一次 self_build 内重建——不是
        // 首轮 failed=1、第二次运行才恢复。
        {
            std::fstream bad{
                a_pcm,
                std::ios::binary | std::ios::in | std::ios::out};
            bad.seekp(0);
            bad.write("XXXX", 4);
        }
        cg::test::check(
            r,
            fs::file_size(fs::path{a_pcm}, ec)
                == static_cast<std::uintmax_t>(rebuilt),
            "the corrupted pcm keeps the recorded size");
        std::vector<cc::CompileCommand> classic3;
        const cc::SelfBuildResult       third =
            cc::self_build_modules(opt, units, classic3);
        cg::test::check(
            r,
            third.built >= 1 && third.degraded == 0,
            "a same-size header-corrupted pcm rebuilds in the same "
            "run (no second-run convergence)");
        cg::test::check(
            r,
            cc::detail::pcm_header_ok(a_pcm),
            "the rebuilt pcm has a loadable CPCH header again");
    }

    // PCH 保真搬运：自建合成命令必须携带记录条目的 pch_use/pch_path/
    // pch_header/pch_involved 四字段（否则路由吞噬让模块单元的 PCH 语义
    // 在规划前死亡）；不带 PCH 标记的记录保持默认（零扰动门禁）；
    // 配套项：模块单元重放 argv 的文本强制包含剥除并逐文件记名。
    void module_pch_field_carry(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "field carry skipped: no driver");
            return;
        }
        TempProj        proj{"ccg-pch-carry-"};
        std::error_code ec;
        fs::create_directories(proj.base / "proj", ec);
        fs::create_directories(proj.base / "cfg", ec);
        {
            std::ofstream a{proj.base / "proj" / "a.ixx", std::ios::binary};
            a << "export module a;\nexport int fa(){return 1;}\n";
            std::ofstream b{proj.base / "proj" / "b.ixx", std::ios::binary};
            b << "export module b;\nexport int fb(){return 2;}\n";
            // 强制包含的伞头必须真实存在：否则扫描器先于剥离点失败，单
            // 元走 preordained 降级，配套项根本不会到达（真实语料的
            // cmake_pch.hxx 也在盘上）。
            std::ofstream u{proj.base / "proj" / "u.h", std::ios::binary};
            u << "// comment-only umbrella, hard error before module decl\n";
        }
        const std::string a_norm = cg::util::norm_path(
            (proj.base / "proj" / "a.ixx").generic_string());
        const std::string b_norm = cg::util::norm_path(
            (proj.base / "proj" / "b.ixx").generic_string());
        const std::string root =
            cg::util::norm_path((proj.base / "proj").generic_string());
        cc::SelfBuildOptions opt;
        opt.root = root;
        opt.ccg_config_dir =
            cg::util::norm_path((proj.base / "cfg").generic_string());
        opt.std_value = "c++20";
        opt.db_mode = true;
        // a：/Yu+/Fp 消费者形态（gnu 方言路由等价的记录权威形状）；b：干
        // 净记录（真实工程最常见的无任何 PCH 标记形态）。
        cc::CompileCommand rec_a;
        rec_a.directory = root;
        rec_a.file = a_norm;
        rec_a.argv = {"-std=c++20", "-I", root, "a.ixx"};
        rec_a.pch_use = true;
        rec_a.pch_path = "E:/phantom/stdafx.pch";
        rec_a.pch_header = "stdafx.h";
        rec_a.pch_involved = true;
        opt.recorded.emplace(a_norm, std::move(rec_a));
        cc::CompileCommand rec_b;
        rec_b.directory = root;
        rec_b.file = b_norm;
        rec_b.argv = {"-std=c++20", "-I", root, "b.ixx"};
        opt.recorded.emplace(b_norm, std::move(rec_b));
        std::vector<cc::CompileCommand> classic;
        const cc::SelfBuildResult       built =
            cc::self_build_modules(opt, {a_norm, b_norm}, classic);
        const cc::CompileCommand* cmd_a = nullptr;
        const cc::CompileCommand* cmd_b = nullptr;
        for (const cc::CompileCommand& c : built.commands)
        {
            if (c.file == a_norm)
            {
                cmd_a = &c;
            }
            else if (c.file == b_norm)
            {
                cmd_b = &c;
            }
        }
        cg::test::check(
            r,
            cmd_a != nullptr && cmd_b != nullptr,
            "the self-build synthesizes commands for both units");
        if (cmd_a != nullptr)
        {
            cg::test::check(
                r,
                cmd_a->pch_use && cmd_a->pch_involved,
                "the synthetic command carries pch_use and "
                "pch_involved from the recorded entry");
            cg::test::check(
                r,
                cmd_a->pch_path == "E:/phantom/stdafx.pch"
                    && cmd_a->pch_header == "stdafx.h",
                "the synthetic command carries pch_path and "
                "pch_header verbatim");
            cg::test::check(
                r,
                !cmd_a->pch_create,
                "pch_create is never carried (a module unit is not "
                "a PCH producer)");
        }
        if (cmd_b != nullptr)
        {
            cg::test::check(
                r,
                !cmd_b->pch_use && !cmd_b->pch_involved
                    && cmd_b->pch_path.empty() && cmd_b->pch_header.empty(),
                "a record without PCH marks keeps the synthetic "
                "defaults (zero perturbation)");
        }

        // 配套项：转发形态文本强制包含剥除 + 逐文件记名（注释-only 伞头同
        // 样先于 module 声明注入，是硬错误）。
        cc::CompileCommand rec_c;
        rec_c.directory = root;
        rec_c.file = b_norm;
        rec_c.argv = {
            "-std=c++20",
            "-I",
            root,
            "-Xclang",
            "-include",
            "-Xclang",
            cg::util::norm_path((proj.base / "proj" / "u.h").generic_string()),
            "b.ixx"};
        opt.recorded.clear();
        opt.recorded.emplace(b_norm, std::move(rec_c));
        cc::SelfBuildOptions            opt_c = opt;
        std::vector<cc::CompileCommand> classic_c;
        const cc::SelfBuildResult       built_c =
            cc::self_build_modules(opt_c, {b_norm}, classic_c);
        const cc::CompileCommand* cmd_c = nullptr;
        for (const cc::CompileCommand& c : built_c.commands)
        {
            if (c.file == b_norm)
            {
                cmd_c = &c;
            }
        }
        cg::test::check(
            r,
            cmd_c != nullptr,
            "the forced-include unit still synthesizes its command");
        if (cmd_c != nullptr)
        {
            const auto hit = std::ranges::find_if(
                cmd_c->argv,
                [](const std::string& a) { return a == "-include"; });
            cg::test::check(
                r,
                hit == cmd_c->argv.end(),
                "the forwarded text include is stripped from the "
                "module unit's replay argv");
        }
        bool named = false;
        for (const std::string& note : built_c.notes)
        {
            if (note.contains(b_norm) && note.contains("forced include"))
            {
                named = true;
            }
        }
        cg::test::check(
            r,
            named,
            "the strip is named per file in the self-build notes, "
            "never silent");
    }

    // 硬门禁：一次 --force 索引（配置 B）不得删除另一配置（A）的产物
    // 目录——即使 A 的 mtime 已超过 1 小时宽限期。清单必须同时保留两套
    // 条目（含兄弟配置）。
    void bmi_cross_config_cleanup_preserved(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "cross-config cleanup skipped: no driver");
            return;
        }
        TempProj        proj{"ccg-bmi-cfgkeep-"};
        std::error_code ec;
        fs::create_directories(proj.base / "proj", ec);
        fs::create_directories(proj.base / "cfg", ec);
        {
            std::ofstream a{proj.base / "proj" / "a.ixx", std::ios::binary};
            a << "export module a;\nexport int fa(){return 1;}\n";
            std::ofstream b{proj.base / "proj" / "b.ixx", std::ios::binary};
            b << "export module b;\nimport a;\nexport int fb(){return fa()+1;}\n";
        }
        const std::string root =
            cg::util::norm_path((proj.base / "proj").generic_string());
        const std::vector<std::string> units{
            cg::util::norm_path(
                (proj.base / "proj" / "a.ixx").generic_string()),
            cg::util::norm_path(
                (proj.base / "proj" / "b.ixx").generic_string())};
        cc::SelfBuildOptions opt_a;
        opt_a.root = root;
        opt_a.ccg_config_dir =
            cg::util::norm_path((proj.base / "cfg").generic_string());
        opt_a.std_value = "c++20";
        std::vector<cc::CompileCommand> classic_a;
        const cc::SelfBuildResult       a =
            cc::self_build_modules(opt_a, units, classic_a);
        cg::test::check(
            r,
            a.built >= 2 && a.degraded == 0,
            "config A self-build produces its BMIs");
        const fs::path manifest_path = fs::path{a.cache_dir} / "manifest.json";
        const auto     first = read_bmi_manifest(manifest_path);
        std::string    config_a;
        std::vector<fs::path> artifacts_a;
        if (first.has_value())
        {
            config_a = first->config_key;
            for (const cc::BmiEntry& e : first->entries)
            {
                if (!config_a.empty() && e.pcm.starts_with(config_a + "/"))
                {
                    artifacts_a.push_back(
                        fs::path{a.cache_dir} / fs::path{e.pcm});
                }
            }
        }
        cg::test::check(
            r,
            !config_a.empty() && !artifacts_a.empty(),
            "config A manifest names its config directory");
        // 把 A 的产物做旧到宽限期之外：--force 下的清理若越界就会删它们。
        const auto old =
            fs::file_time_type::clock::now() - std::chrono::hours{2};
        for (const fs::path& p : artifacts_a)
        {
            fs::last_write_time(p, old, ec);
        }

        // 配置 B：不同 -std 级别 => 不同 config_key/命令摘要，--force 触发
        // 清理路径。必须用真实不同的标准级别（c++20 vs c++23），不能用
        // 别名拼写：cl 方言把 c++2a 与 c++20 归一成同一个 /std 旗标
        // （cl_std_of），别名在两种方言下不构成跨编译器的配置差异轴。
        cc::SelfBuildOptions opt_b = opt_a;
        opt_b.std_value = "c++23";
        opt_b.force = true;
        std::vector<cc::CompileCommand> classic_b;
        const cc::SelfBuildResult       b =
            cc::self_build_modules(opt_b, units, classic_b);
        cg::test::check(
            r,
            b.built >= 1 && b.degraded == 0,
            "config B force self-build succeeds");

        bool kept_a = !artifacts_a.empty();
        for (const fs::path& p : artifacts_a)
        {
            kept_a = kept_a && fs::is_regular_file(p, ec);
        }
        cg::test::check(
            r,
            kept_a,
            "config B's --force cleanup never deletes config A "
            "artifacts (even past the grace period)");

        const auto after = read_bmi_manifest(manifest_path);
        cg::test::check(r, after.has_value(), "manifest stays valid JSON");
        if (after.has_value())
        {
            bool has_a = false;
            bool has_b = false;
            for (const cc::BmiEntry& e : after->entries)
            {
                has_a = has_a || e.pcm.starts_with(config_a + "/");
                has_b = has_b
                        || (!after->config_key.empty()
                            && e.pcm.starts_with(after->config_key + "/"));
            }
            cg::test::check(
                r,
                has_a && has_b,
                "manifest carries both configs' entries after the "
                "forced run");
        }
    }

    /// 文本的换行计数。
    [[nodiscard]]
    auto line_count_of(std::string_view text) -> std::size_t
    {
        return static_cast<std::size_t>(std::ranges::count(text, '\n'));
    }

    /// 文本中子串出现次数（允许重叠）。
    [[nodiscard]]
    auto substring_count(std::string_view text, std::string_view needle)
        -> std::size_t
    {
        std::size_t hits = 0;
        for (std::size_t at = text.find(needle); at != std::string_view::npos;
             at = text.find(needle, at + 1))
        {
            ++hits;
        }
        return hits;
    }

    /// 文本的第 n 行（0 起；越界给空视图）。
    [[nodiscard]]
    auto nth_line_of(std::string_view text, std::size_t n) -> std::string_view
    {
        std::size_t at = 0;
        std::size_t seen = 0;
        while (at <= text.size())
        {
            const std::size_t      end = text.find('\n', at);
            const std::string_view line = text.substr(
                at,
                end == std::string_view::npos ? std::string_view::npos
                                              : end - at);
            if (seen == n)
            {
                return line;
            }
            ++seen;
            if (end == std::string_view::npos)
            {
                break;
            }
            at = end + 1;
        }
        return std::string_view{};
    }

    // ------------------------------------------------- cl 方言规则
    // ---------

    void m4_cl_dialect_rules(R& r)
    {
        // /std: 映射：cl 模式里不存在
        // /std:c++23——只被诊断为一个无害的未用实参警告，
        // 而 TU 静默跌回 c++14 默认，模块语法
        // 在那里死掉。该映射绝不能产出它。
        const auto s20 = cc::detail::cl_std_of("c++20");
        cg::test::check(
            r,
            s20.first == "/std:c++20" && s20.second == "c++20",
            "c++20 maps to the exact cl spelling");
        const auto s23 = cc::detail::cl_std_of("c++23");
        cg::test::check(
            r,
            s23.first == "/std:c++latest" && s23.second == "c++2d",
            "c++23 maps to c++latest (never /std:c++23)");
        const auto sld = cc::detail::cl_std_of("c++latest");
        cg::test::check(
            r,
            sld.first == "/std:c++latest",
            "c++latest maps through");
        cg::test::check(
            r,
            s20.first != "/std:c++23" && s23.first != "/std:c++23",
            "the invalid cl spelling is never emitted");

        // 头单元检测。
        cg::test::check(
            r,
            cc::detail::looks_like_header_unit("test.h"),
            "plain header name detected");
        cg::test::check(
            r,
            cc::detail::looks_like_header_unit("\"vec.h\""),
            "quoted header name detected");
        cg::test::check(
            r,
            cc::detail::looks_like_header_unit("<vector>"),
            "angled header name detected");
        cg::test::check(
            r,
            !cc::detail::looks_like_header_unit("std"),
            "named module std is not a header unit");
        cg::test::check(
            r,
            !cc::detail::looks_like_header_unit("mp:core"),
            "partition names are not header units");
        cg::test::check(
            r,
            !cc::detail::looks_like_header_unit("calc"),
            "module names without extensions are not header units");

        // 从手写文本提取头单元 import 行（P1689 扫描从不
        // 把它们报进 requires——在研究样本上实测）；
        // 含多行声明（词法扫描器）。
        const std::vector<std::string> hus =
            cc::detail::text_header_unit_imports(
                std::string{fixture("hu_units.ixx")});
        cg::test::check(
            r,
            hus.size() == 3 && hus[0] == "local.h" && hus[1] == "angled.hpp"
                && hus[2] == "multi-line.h",
            "header-unit import lines detected in file order");

        // 混合 TU 检测器：`#include` 解析进钉住的根内
        // （never-feed-std.pcm 规则的触发器）。
        const std::vector<std::string> roots{
            std::string{fixture_dir} + "/stlroot"};
        cg::test::check(
            r,
            cc::detail::text_has_stl_include(
                std::string{fixture("stl_inc.cpp")},
                roots),
            "STL include under a pinned root detected");
        cg::test::check(
            r,
            !cc::detail::text_has_stl_include(
                std::string{fixture("plain_inc.cpp")},
                roots),
            "project-only includes do not trigger the detector");
    }

    // import 重写覆盖层的形态面：伞头、头单元、去模块化；行数恒等；
    // 同行跟随、行中点位、被丢弃模块的同宽抹除与名字收集。
    void import_rewrite_overlay_cases(R& r)
    {
        // import 重写覆盖层：伞头、头单元、去模块化。
        const std::string       src = read_fixture("hu_units.ixx");
        const cc::ImportOverlay res =
            cc::import_rewrite_overlay(src, true, true, false);
        const std::string& out = res.text;
        cg::test::check(
            r,
            res.replaced.size() == 6,
            "std + std.compat + export-std + 3 header units replaced");
        cg::test::check(
            r,
            out.find("#include <__msvc_all_public_headers.hpp>")
                != std::string::npos,
            "umbrella include substituted for import std");
        cg::test::check(
            r,
            out.find("#include \"local.h\"") != std::string::npos,
            "quoted header unit becomes an include");
        cg::test::check(
            r,
            out.find("#include \"multi-line.h\"") != std::string::npos,
            "multi-line header unit becomes an include on its first line");
        cg::test::check(
            r,
            out.find("import hu.other;") != std::string::npos,
            "named-module imports stay verbatim");
        cg::test::check(
            r,
            line_count_of(src) == line_count_of(out),
            "line count preserved");

        // 重写在伞下必须处理的 import 声明形态：
        // ';' 前的空白、尾随注释、
        // 同一物理行多个 import、`export` 独占一行，以及
        // 被丢弃的模块（喂入已抹除）按同宽抹除而非留着不映射。
        const std::string              mix = "#include <vector>\n"
                                             "import std ;\n"
                                             "import std; // trailing comment\n"
                                             "import std; import util;\n"
                                             "export\nimport std;\n"
                                             "import\n  std.compat;\n"
                                             "int body() { return 0; }\n";
        const std::vector<std::string> dropped{"util"};
        const cc::ImportOverlay        mix_res =
            cc::import_rewrite_overlay(mix, true, false, false, dropped);
        const std::string& mixed = mix_res.text;
        cg::test::check(
            r,
            line_count_of(mix) == line_count_of(mixed),
            "shape rewrite preserves the line count");
        cg::test::check(
            r,
            substring_count(mixed, "#include <__msvc_all_public_headers.hpp>")
                == 4,
            "std/std.compat imports become the umbrella include "
            "except the one with a follower import on its line "
            "(that tail is not inert, so it blanks - the "
            "util site blanks as a dropped module anyway)");
        cg::test::check(
            r,
            mixed.find("import") == std::string::npos,
            "no unmapped import survives the umbrella rewrite");
        cg::test::check(
            r,
            mixed.find("// trailing comment") != std::string::npos,
            "a trailing comment behind the rewrite survives");
        cg::test::check(
            r,
            mixed.find("int body()") != std::string::npos,
            "code below the rewrites stays untouched");
        cg::test::check(
            r,
            mix_res.replaced.size() == 6 && mix_res.replaced[3] == "util"
                && mix_res.replaced[5] == "std.compat",
            "rewritten names collect in file order incl. dropped");

        // 不是本行第一个非空白令牌的 '#' 不是
        // 预处理指令。行中 import 点位做同宽
        // 抹除（干净降级：不长出指令、不挪列、
        // 同行邻居不动）且仍收集其名字。
        const std::vector<cc::ImportDeclSite> sites = cc::scan_import_decls(
            "import win; import std;\nvoid g(); import \"cfg.h\";\n");
        cg::test::check(
            r,
            sites.size() == 3 && sites[0].starts_line && !sites[1].starts_line
                && !sites[2].starts_line,
            "scanner reports whether each site opens its line");
        const std::string       mid = "import win; import std;\n"
                                      "#include <string>\n"
                                      "void g(); import \"cfg.h\";\n"
                                      "int body() { return 0; }\n";
        const cc::ImportOverlay mid_res =
            cc::import_rewrite_overlay(mid, true, true, false);
        const std::string& mid_out = mid_res.text;
        cg::test::check(
            r,
            line_count_of(mid) == line_count_of(mid_out),
            "mid-line rewrite preserves the line count");
        cg::test::check(
            r,
            mid_out.find("#include <__msvc_all_public_headers.hpp>")
                    == std::string::npos
                && mid_out.find("#include \"cfg.h\"") == std::string::npos,
            "no directive grows out of a mid-line import site");
        cg::test::check(
            r,
            mid_out.substr(0, mid_out.find('\n'))
                == "import win;" + std::string(12, ' '),
            "the mid-line std import blanks same-width");
        cg::test::check(
            r,
            mid_out.find("import win;") != std::string::npos
                && mid_out.find("void g();") != std::string::npos
                && mid_out.find("#include <string>") != std::string::npos,
            "same-line neighbors and authored directives stay put");
        cg::test::check(
            r,
            mid_res.replaced.size() == 2 && mid_res.replaced[0] == "std"
                && mid_res.replaced[1] == "cfg.h",
            "mid-line rewrites still collect their names");
        const std::string rider = cc::import_rewrite_overlay(
                                      "void f(); export import std;\n",
                                      true,
                                      false,
                                      false)
                                      .text;
        cg::test::check(
            r,
            rider.find('#') == std::string::npos
                && rider.find("void f();") != std::string::npos,
            "a site behind a mid-line export rider blanks too");

        // 长成的 `#include` 指令占据其整个物理行——
        // 预处理器把任何非注释的跟随令牌静默丢弃
        // 报成 "extra tokens at end of `#include` directive"（仅警告，绝不
        // 到达 severity>=error 收集）。行首点位只有当
        // import 区段之后的本行剩余部分
        // 全是空白/注释时才能发出；跟随代码回退到同宽
        // 抹除（两种情况下 import 行都经 `replaced` 幸存）。
        const std::string       tail_src = read_fixture("follower_header.cpp");
        const cc::ImportOverlay tail_res =
            cc::import_rewrite_overlay(tail_src, false, true, false);
        const std::string& tail_out = tail_res.text;
        cg::test::check(
            r,
            line_count_of(tail_src) == line_count_of(tail_out),
            "follower header rewrite preserves the line count");
        cg::test::check(
            r,
            nth_line_of(tail_out, 4).size() == nth_line_of(tail_src, 4).size()
                && nth_line_of(tail_out, 4).find('#') == std::string_view::npos
                && nth_line_of(tail_out, 4).find("import")
                       == std::string_view::npos
                && nth_line_of(tail_out, 4).find("int follower_impl()")
                       != std::string_view::npos,
            "follower code after a line-start header import blanks "
            "same-width");
        cg::test::check(
            r,
            nth_line_of(tail_out, 5).starts_with("#include \"follower_cfg.h\"")
                && nth_line_of(tail_out, 5).find("// comment-only follower")
                       != std::string_view::npos,
            "whitespace/comment-only tail keeps the grown directive");
        cg::test::check(
            r,
            tail_res.replaced.size() == 2
                && tail_res.replaced[0] == "follower_cfg.h"
                && tail_res.replaced[1] == "follower_cfg.h",
            "both header sites still collect their import names");
        const std::string       umb_src = read_fixture("follower_umbrella.cpp");
        const cc::ImportOverlay umb_res =
            cc::import_rewrite_overlay(umb_src, true, false, false);
        const std::string& umb_out = umb_res.text;
        cg::test::check(
            r,
            line_count_of(umb_src) == line_count_of(umb_out),
            "follower umbrella rewrite preserves the line count");
        cg::test::check(
            r,
            nth_line_of(umb_out, 4).size() == nth_line_of(umb_src, 4).size()
                && nth_line_of(umb_out, 4).find('#') == std::string_view::npos
                && nth_line_of(umb_out, 4).find("import")
                       == std::string_view::npos
                && nth_line_of(umb_out, 4).find("int umbrella_follow()")
                       != std::string_view::npos,
            "follower code after a line-start std import blanks "
            "same-width");
        cg::test::check(
            r,
            nth_line_of(umb_out, 5)
                    .starts_with("#include <__msvc_all_public_headers.hpp>")
                && nth_line_of(umb_out, 5).find("// comment-only follower")
                       != std::string_view::npos,
            "whitespace/comment-only tail keeps the umbrella directive");
        cg::test::check(
            r,
            umb_res.replaced.size() == 2 && umb_res.replaced[0] == "std"
                && umb_res.replaced[1] == "std",
            "both std sites still collect their import names");

        // 注释/字符串/指令里的 `import` 绝不匹配。
        const std::string       decoy = "// import std;\n"
                                        "const char* s = \"import std;\";\n"
                                        "#import <nope.tlb>\n"
                                        "int keep() { return 1; }\n";
        const cc::ImportOverlay decoy_res =
            cc::import_rewrite_overlay(decoy, true, false, false);
        const std::string& decoy_out = decoy_res.text;
        cg::test::check(
            r,
            decoy_res.replaced.empty()
                && decoy_out.find("import std;") != std::string::npos
                && decoy_out == decoy,
            "commented/quoted/directive imports stay verbatim");

        // 去模块化：模块声明同宽抹除。
        const std::string demod =
            cc::import_rewrite_overlay(src, false, false, true).text;
        cg::test::check(
            r,
            demod.find("export module hu.fixture;") == std::string::npos,
            "module declaration blanked under demodule");
        cg::test::check(
            r,
            line_count_of(src) == line_count_of(demod),
            "demodule preserves the line count");
        cg::test::check(
            r,
            demod.find("int body()") != std::string::npos,
            "demodule keeps the body text");
    }

    // 剥离层（cl cdb 记录 argv 权威的承重件）：MSVC 模块开关在三个消费点
    // （扫描/BMI 生产/重放）之前剥除——单 token（/interface
    // /internalPartition）、两段式连值（"/ifcOutput `<值>`"）与冒号连写
    // （"/ifcOutput:值"）三种拼写都兼容，'/' 与 '-' 前缀、ASCII 大小写不
    // 敏感；/reference 的名字记账进名单（"name=path" 取名、裸 path 取文件
    // 主干推断）；其余 token 逐字保留，gnu 方言命令零触碰（stripped 恒
    // 为 false）。
    void msvc_module_switch_strip(R& r)
    {
        using cc::detail::MsvcModuleStrip;
        // cl 方言模块单元的记录形状（审计夹具同形）：/interface 单 token、
        // /ifcOutput 与 /scanDependencies 两段式。
        const std::vector<std::string> cl_mod = {
            "--driver-mode=cl",
            "/c",
            "/EHsc",
            "/Zc:inline",
            "/std:c++20",
            "/I",
            "E:/proj/sub",
            "/interface",
            "/ifcOutput",
            "E:/b/m1.ifc",
            "/scanDependencies",
            "E:/b/",
            "m1.ixx"};
        const MsvcModuleStrip s1 =
            cc::detail::strip_msvc_module_switches(cl_mod);
        const std::vector<std::string> want1 = {
            "--driver-mode=cl",
            "/c",
            "/EHsc",
            "/Zc:inline",
            "/std:c++20",
            "/I",
            "E:/proj/sub",
            "m1.ixx"};
        cg::test::check(
            r,
            s1.argv == want1,
            "module switches stripped, everything else verbatim");
        cg::test::check(
            r,
            s1.stripped && s1.references.empty(),
            "interface unit strip records no /reference names");

        // 冒号连写 + 两段式混排 + /internalPartition + /reference 三种值
        // 形态（name=path、连写 name=path、裸 path 推主干）。带点模块名
        // （std.core、m7a.one 形态）的 name 部分原样记账——绝不当扩展名截
        // 主干（带点模块名夹具实证过的回归点）。
        const std::vector<std::string> joined = {
            "/internalPartition",
            "/ifcOutput:E:/b/p.ifc",
            "/scanDependencies:E:/b/",
            "/ifcSearchDir",
            "E:/b/ifc",
            "/reference",
            "m1=E:/b/m1.ifc",
            "/reference:m2=E:/b/m2.ifc",
            "/reference",
            "std.core=E:/b/sc.ifc",
            "/reference",
            "E:/b/justpath.ifc"};
        const MsvcModuleStrip s2 =
            cc::detail::strip_msvc_module_switches(joined);
        cg::test::check(
            r,
            s2.argv.empty(),
            "colon-joined and two-segment valued forms all strip");
        cg::test::check(
            r,
            s2.stripped
                && s2.references
                       == std::vector<
                           std::string>{"m1", "m2", "std.core", "justpath"},
            "/reference ledger collects names, never .ifc paths");

        // 前缀 '-' 与大小写放宽（手写 cdb 的健壮性）。
        const std::vector<std::string> loose = {
            "/INTERFACE",
            "-ifcoutput",
            "X"};
        const MsvcModuleStrip s3 =
            cc::detail::strip_msvc_module_switches(loose);
        cg::test::check(
            r,
            s3.argv.empty() && s3.stripped,
            "'-' prefix and case-insensitive spellings strip");

        // gnu 方言零触碰：没有任何开关命中时 argv 逐字节恒等。
        const std::vector<std::string> gnu = {
            "-std=c++20",
            "-I",
            "/usr/include",
            "-fmodule-file=std=/x.pcm",
            "-o",
            "out.o",
            "a.cppm"};
        const MsvcModuleStrip s4 = cc::detail::strip_msvc_module_switches(gnu);
        cg::test::check(
            r,
            s4.argv == gnu && !s4.stripped,
            "gnu-dialect argv passes through byte-identical");

        // 尾部裸 /reference（值缺席）：token 剥除、名单不记账。
        const MsvcModuleStrip s5 = cc::detail::strip_msvc_module_switches(
            {"/reference", "/std:c++20"});
        cg::test::check(
            r,
            s5.argv == std::vector<std::string>{"/std:c++20"}
                && s5.references.empty(),
            "trailing bare /reference strips without a name");

        // 值缺席的裸旗标（翻译层成对保留落地前的历史 cdb 形态；手写 cdb
        // 仍是活的防御面）：/ifcOutput 与 /scanDependencies 裸露、后随
        // 旗标绝不被当值吞掉。裸 /ifcOutput 若穿透到 BMI 自建，会与追加的
        // 输出参数构成双输出冲突（cannot specify -o when generating multiple
        // output files），此形态必须剥除。
        const std::vector<std::string> bare = {
            "/c",
            "/ifcOutput",
            "/scanDependencies",
            "/interface",
            "/std:c++20"};
        const MsvcModuleStrip s6 = cc::detail::strip_msvc_module_switches(bare);
        cg::test::check(
            r,
            s6.argv == std::vector<std::string>{"/c", "/std:c++20"},
            "value-less bare switches strip without eating the "
            "next flag");
        cg::test::check(
            r,
            s6.stripped && s6.references.empty(),
            "bare switch strip marks the argv as stripped");

        // 真实记录形态（真实 MSVC 工程语料实录，MSVC 14.51：MSBuild
        // 给 .ixx 发扫描/编译两趟调用，cdb 每文件双条；翻译层修复后两趟
        // 均为两段式带值，此夹具钉的是修复前的历史形态——防御面同 s6）：
        // 裸 /ifcOutput 值缺席、后随裸 /sourceDependencies（编译趟；扫描趟
        // 是裸 /scanDependencies，s6 已覆盖）、/TP 在场——模块开关全剥、
        // /TP 与 /X 原样保留，重放只剩单个输入文件。
        const std::vector<std::string> compile_pass = {
            "--driver-mode=cl",
            "/c",
            "/EHsc",
            "/std:c++20",
            "/interface",
            "/ifcOutput",
            "/sourceDependencies",
            "/TP",
            "/X",
            "-imsvc",
            "C:/VC/include",
            "mod.ixx"};
        const MsvcModuleStrip s7 =
            cc::detail::strip_msvc_module_switches(compile_pass);
        cg::test::check(
            r,
            s7.argv
                == std::vector<
                    std::
                        string>{"--driver-mode=cl", "/c", "/EHsc", "/std:c++20", "/TP", "/X", "-imsvc", "C:/VC/include", "mod.ixx"},
            "the compile-pass recording (bare /ifcOutput + bare "
            "/sourceDependencies + /TP) strips to a single-input "
            "replay");

        // /exportHeader（头文件单元产物开关）与 /sourceDependencies 的三种
        // 拼写（两段式、冒号连写、裸）全部剥除；裸形态后随旗标绝不吞。
        const std::vector<std::string> exports = {
            "/exportHeader",
            "E:/b/h.h",
            "/sourceDependencies:E:/b/d.json",
            "/exportHeader:E:/b/h2.h",
            "/exportHeader",
            "/TP",
            "h.ixx"};
        const MsvcModuleStrip s8a =
            cc::detail::strip_msvc_module_switches(exports);
        cg::test::check(
            r,
            s8a.argv == std::vector<std::string>{"/TP", "h.ixx"},
            "all three spellings of /exportHeader and "
            "/sourceDependencies strip with their values");
    }

    // 剥离层的等价与单一事实源面：修复前后三种 cdb 拼写剥除产物逐字节
    // 一致、同名前缀不误伤、共享清单在 classify 与 strip 两个消费面
    // 上的全部拼写归类正确。
    void msvc_module_strip_ledger_invariants(R& r)
    {
        using cc::detail::MsvcModuleStrip;
        // 修复形态等价守卫（翻译层成对保留落地后的关键不变量）：cdb 中
        // 六项为两段式带值（或冒号连写）——剥离层成对剥除后，引擎 argv 与
        // 修复前裸拼写形态（值被丢的历史 cdb）的剥除产物逐字节一致。引擎
        // 看到的命令不变，变的只是 cdb 的忠实度。
        const std::vector<std::string> pre_fix_form = {
            "--driver-mode=cl",
            "/c",
            "/EHsc",
            "/std:c++20",
            "/interface",
            "/ifcOutput",
            "/sourceDependencies",
            "/TP",
            "/X",
            "-imsvc",
            "C:/VC/include"};
        const std::vector<std::string> post_fix_form = {
            "--driver-mode=cl",
            "/c",
            "/EHsc",
            "/std:c++20",
            "/interface",
            "/ifcOutput",
            "E:/proj/int/mod.ifc",
            "/sourceDependencies",
            "E:/proj/int/",
            "/TP",
            "/X",
            "-imsvc",
            "C:/VC/include"};
        const std::vector<std::string> joined_fix_form = {
            "--driver-mode=cl",
            "/c",
            "/EHsc",
            "/std:c++20",
            "/interface",
            "/ifcOutput:E:/proj/int/mod.ifc",
            "/sourceDependencies:E:/proj/int/",
            "/TP",
            "/X",
            "-imsvc",
            "C:/VC/include"};
        const std::vector<std::string> engine_want = {
            "--driver-mode=cl",
            "/c",
            "/EHsc",
            "/std:c++20",
            "/TP",
            "/X",
            "-imsvc",
            "C:/VC/include"};
        const MsvcModuleStrip g1 =
            cc::detail::strip_msvc_module_switches(pre_fix_form);
        const MsvcModuleStrip g2 =
            cc::detail::strip_msvc_module_switches(post_fix_form);
        const MsvcModuleStrip g3 =
            cc::detail::strip_msvc_module_switches(joined_fix_form);
        cg::test::check(
            r,
            g1.argv == engine_want && g2.argv == engine_want
                && g3.argv == engine_want,
            "bare (pre-fix), two-segment (post-fix) and colon-joined cdb "
            "forms all strip to the byte-identical engine argv");
        cg::test::check(
            r,
            g1.stripped && g2.stripped && g3.stripped,
            "all three cdb forms are recognized as carrying module switches");

        // 同名前缀不误伤（消费端剥离的直接单测）：/ifcOutputX、/interface2
        // 这类恰以清单拼写为前缀的旗标不是模块开关——开关与后随 token 都
        // 逐字保留，绝不剥一半留一半、也绝不误吞。
        const std::vector<std::string> near_miss =
            {"/ifcOutputX", "E:/not/a/value.ifc", "/interface2", "/std:c++20"};
        const MsvcModuleStrip g4 =
            cc::detail::strip_msvc_module_switches(near_miss);
        cg::test::check(
            r,
            g4.argv == near_miss && !g4.stripped,
            "prefix lookalike flags pass through with their next tokens "
            "untouched");

        // 单一事实源锁死（曾有的结构性缺口：剥离层与 PCH 影子重放各自维
        // 护清单，/sourceDependencies 只在 PCH 侧被认识）：两个消费面
        // 经同一个 classify 读同一份共享清单——对清单全部规范拼写（含
        // 大小写与前缀放宽形态）断言归类正确、strip 一律剥除。
        const std::vector<std::string_view> singles(
            std::begin(cc::detail::msvc_module_single_switches),
            std::end(cc::detail::msvc_module_single_switches));
        const std::vector<std::string_view> valued(
            std::begin(cc::detail::msvc_module_valued_switches),
            std::end(cc::detail::msvc_module_valued_switches));
        cg::test::check(
            r,
            singles.size() == 2
                && std::ranges::find(singles, "interface") != singles.end()
                && std::ranges::find(singles, "internalPartition")
                       != singles.end(),
            "the shared single-switch ledger holds the interface unit markers");
        cg::test::check(
            r,
            valued.size() == 6
                && std::ranges::find(valued, "ifcOutput") != valued.end()
                && std::ranges::find(valued, "scanDependencies") != valued.end()
                && std::ranges::find(valued, "sourceDependencies")
                       != valued.end()
                && std::ranges::find(valued, "exportHeader") != valued.end()
                && std::ranges::find(valued, "ifcSearchDir") != valued.end()
                && std::ranges::find(valued, "reference") != valued.end(),
            "the shared valued-switch ledger covers the full MSVC module "
            "switch family (sourceDependencies and exportHeader included)");
        bool classify_ok = true;
        bool strip_all = true;
        for (const std::string_view f : singles)
        {
            for (const std::string& tok :
                 {std::format("/{}", f), std::format("-{}", f)})
            {
                const cc::detail::MsvcSwitchMatch m =
                    cc::detail::msvc_module_switch_classify(tok);
                classify_ok =
                    classify_ok
                    && m.kind == cc::detail::MsvcModuleSwitchKind::single;
                strip_all = strip_all
                            && cc::detail::strip_msvc_module_switches({tok})
                                   .argv.empty();
            }
        }
        for (const std::string_view f : valued)
        {
            for (const std::string& tok :
                 {std::format("/{}", f),
                  std::format("-{}", f),
                  std::format("/{}:E:/b/x", f),
                  std::format("/{}UPPER", f)})
            {
                const cc::detail::MsvcSwitchMatch m =
                    cc::detail::msvc_module_switch_classify(tok);
                const bool tok_is_noise = tok.ends_with("UPPER");
                classify_ok =
                    classify_ok
                    && (tok_is_noise
                            ? m.kind == cc::detail::MsvcModuleSwitchKind::none
                            : m.kind == cc::detail::MsvcModuleSwitchKind::valued
                                  && (!tok.contains(':')
                                      || !m.joined_value.empty()));
                if (!tok_is_noise)
                {
                    strip_all = strip_all
                                && cc::detail::strip_msvc_module_switches({tok})
                                       .argv.empty();
                }
            }
        }
        cg::test::check(
            r,
            classify_ok,
            "classify reads the shared ledger prefix/case-loose "
            "and rejects near-miss noise");
        cg::test::check(
            r,
            strip_all,
            "every ledger switch strips from argv in every spelling (both "
            "consumers share the one ledger)");
    }

    // 配套项（相邻发现）：模块单元重放 argv 的文本强制包含剥离——转发对
    // "-Xclang -include -Xclang 值"、裸 "-include 值"/"-include=值"、cl 方言
    // "/FI 值"/"/FI值"（含前缀 '-' 与大小写放宽）全部剥除并记名；序列化
    // PCH 通道 "-Xclang -include-pch"、/Fp 与 -imacros 绝不碰。
    void module_forced_include_strip(R& r)
    {
        using cc::detail::ForcedIncludeStrip;
        using cc::detail::strip_module_text_forced_includes;

        // 装载层改写出的转发形态（gnu 方言绕开隐式 .pch 加载的形状）。
        const ForcedIncludeStrip fwd = strip_module_text_forced_includes(
            {"-std=c++20",
             "-Xclang",
             "-include",
             "-Xclang",
             "E:/h/pch.hxx",
             "-fmodule-file=m=E:/c/m.pcm"});
        cg::test::check(
            r,
            fwd.argv
                == std::vector<
                    std::string>{"-std=c++20", "-fmodule-file=m=E:/c/m.pcm"},
            "the forwarded -include pair strips out of the replay argv");
        cg::test::check(
            r,
            fwd.removed == std::vector<std::string>{"E:/h/pch.hxx"},
            "the forwarded include value is recorded by path");

        // 裸 gnu 形态（未走 PCH 感知解析的记录条目原样保留的形状）。
        const ForcedIncludeStrip raw = strip_module_text_forced_includes(
            {"-include",
             "E:/h/f.h",
             "-include=g.h",
             "a.cppm",
             "-imacros",
             "E:/h/m.h"});
        cg::test::check(
            r,
            raw.argv
                == std::vector<std::string>{"a.cppm", "-imacros", "E:/h/m.h"},
            "bare -include and -include= strip, -imacros untouched");
        cg::test::check(
            r,
            raw.removed == std::vector<std::string>{"E:/h/f.h", "g.h"},
            "both bare spellings record their values");

        // cl 方言 /FI：两段式、连写、前缀 '-'（连写须精确大写 FI——
        // 小写 "-fi..." 落在 gnu -f 族命名空间，如 -finput-charset）。
        const ForcedIncludeStrip fi = strip_module_text_forced_includes(
            {"/FI",
             "E:/h/f.h",
             "/FIstdin2.h",
             "-FIE:/h/f3.h",
             "-finput-charset=GBK",
             "/Fpobj/x.pch",
             "/std:c++20"});
        cg::test::check(
            r,
            fi.argv
                == std::vector<
                    std::
                        string>{"-finput-charset=GBK", "/Fpobj/x.pch", "/std:c++20"},
            "/FI two-segment and joined forms strip, -finput-charset "
            "and /Fp survive");
        cg::test::check(
            r,
            fi.removed
                == std::vector<
                    std::string>{"E:/h/f.h", "stdin2.h", "E:/h/f3.h"},
            "each /FI spelling records its value");

        // 序列化 PCH 通道不是文本强制包含：-include-pch 三元组逐字节保留。
        const ForcedIncludeStrip pch = strip_module_text_forced_includes(
            {"-Xclang",
             "-include-pch",
             "-Xclang",
             "E:/c/shadow.pch",
             "-std=c++20"});
        cg::test::check(
            r,
            pch.argv.size() == 5 && pch.removed.empty(),
            "the -include-pch triple passes through untouched");

        // 值缺席的裸旗标（剥离层按孤儿输入丢弃值后的真实形状）：token 剥
        // 除、后随旗标绝不吞、名单不记空名。
        const ForcedIncludeStrip bare = strip_module_text_forced_includes(
            {"/FI", "/std:c++20", "-include", "-x", "c++-module", "m.ixx"});
        cg::test::check(
            r,
            bare.argv
                == std::vector<
                    std::string>{"/std:c++20", "-x", "c++-module", "m.ixx"},
            "value-less bare flags strip without eating the next "
            "flag");
        cg::test::check(
            r,
            bare.removed.empty(),
            "value-less strips record no names");

        // 无命中时逐字节恒等（零扰动门禁）。
        const std::vector<std::string> clean =
            {"-std=c++20", "-I", "E:/p", "-x", "c++-module", "m.ixx"};
        const ForcedIncludeStrip same =
            strip_module_text_forced_includes(clean);
        cg::test::check(
            r,
            same.argv == clean && same.removed.empty(),
            "argv without forced includes passes through "
            "byte-identical");
    }

    /// token 是否以大小写不敏感的 .exe 结尾（引擎可执行路径的形状）。
    [[nodiscard]]
    auto exe_shaped_token(const std::string& a) -> bool
    {
        return a.size() >= 4
               && cg::util::ascii_iequals(a.substr(a.size() - 4), ".exe");
    }

    /// 参数向量里是否没有任何引擎可执行路径 token。
    [[nodiscard]]
    auto argv_exe_free(const std::vector<std::string>& v) -> bool
    {
        return std::ranges::none_of(v, exe_shaped_token);
    }

    // 拼装层断言（逃逸缺陷修复的承重件）：接口 BMI 重放命令的参数向量组装
    // ——产物绝不携带可执行路径 token（引擎路径由 run_tool 首参携带，命令
    // 行首 token 由 run_process 拼装"exe" + 参数；参数向量里再出现一份引
    // 擎 .exe 是真实存在的文件、被 clang 当作额外输入，与记录 argv 的 /TP
    // 相遇即双输入，追加的 -clang:-o 报 cannot specify -o when generating
    // multiple output files——逃逸缺陷的真凶形状）。记录权威/方言合成/依赖喂
    // 给三段拼接逐 token 断言。
    void bmi_replay_argv_gate(R& r)
    {
        using cc::detail::bmi_replay_argv;
        using cc::detail::MsvcModuleStrip;

        const std::vector<std::pair<std::string, std::string>> feeds = {
            {"std", "E:/c/bmi/std.pcm"}};

        // 记录权威（cl 方言，真实 MSVC 工程语料实录剥离后的真实形态）：
        // 已带 /std:c++20 时不重复补；/TP 与 /X 原样保留（方言旗标，不是
        // 模块开关）；依赖以 -fmodule-file=名=pcm 追加。
        MsvcModuleStrip recorded;
        recorded.argv = {
            "--driver-mode=cl",
            "/c",
            "/EHsc",
            "/std:c++20",
            "/Zc:inline",
            "/TP",
            "/X",
            "-imsvc",
            "C:/VC/include",
            "/Zc:twoPhase-"};
        const std::vector<std::string> cl_replay = bmi_replay_argv(
            &recorded,
            true,
            "/std:c++20",
            "c++20",
            {},
            {},
            {},
            feeds);
        cg::test::check(
            r,
            cl_replay
                == std::vector<
                    std::
                        string>{"--driver-mode=cl", "/c", "/EHsc", "/std:c++20", "/Zc:inline", "/TP", "/X", "-imsvc", "C:/VC/include", "/Zc:twoPhase-", "-x", "c++-module", "--precompile-reduced-bmi", "-fmodule-file=std=E:/c/bmi/std.pcm"},
            "recorded-argv authority replays verbatim with the "
            "module suffix and dep feeds");
        cg::test::check(
            r,
            argv_exe_free(cl_replay),
            "the replay argv carries no executable-path token "
            "(the engine path lives in run_tool's exe slot)");

        // 记录权威缺席 -std 时按方言兜底补一份（cl 补 /std: 拼写）。
        MsvcModuleStrip no_std;
        no_std.argv = {"--driver-mode=cl", "/c", "/TP", "m.ixx"};
        const std::vector<std::string> topped = bmi_replay_argv(
            &no_std,
            true,
            "/std:c++20",
            "c++20",
            {},
            {},
            {},
            {});
        cg::test::check(
            r,
            topped.back() == "--precompile-reduced-bmi"
                && std::ranges::find(topped, "/std:c++20") != topped.end(),
            "a recorded argv without a std switch gets the "
            "dialect default appended");
        cg::test::check(
            r,
            argv_exe_free(topped),
            "the topped argv stays exe-free");

        // cl 方言合成基线（无记录权威）：driver-mode/EHsc/std 先行压入，
        // 基线中的重复项跳过。
        const std::vector<std::string> cl_base = {
            "--driver-mode=cl",
            "/EHsc",
            "/std:c++20",
            "-imsvc",
            "C:/VC/include"};
        const std::vector<std::string> cl_synth = bmi_replay_argv(
            nullptr,
            true,
            "/std:c++20",
            "c++20",
            {},
            cl_base,
            {},
            feeds);
        cg::test::check(
            r,
            cl_synth
                == std::vector<
                    std::
                        string>{"--driver-mode=cl", "/EHsc", "/std:c++20", "-imsvc", "C:/VC/include", "-x", "c++-module", "--precompile-reduced-bmi", "-fmodule-file=std=E:/c/bmi/std.pcm"},
            "the synthesized cl baseline de-duplicates its own "
            "leading trio");
        cg::test::check(
            r,
            argv_exe_free(cl_synth),
            "the synthesized cl argv is "
            "exe-free");

        // gnu 方言合成基线：-std= 与语言轴先行，基线里的 -std= 跳过。
        const std::vector<std::string> axes = {"-fms-extensions"};
        const std::vector<std::string> gnu_base = {"-std=c++17", "-I", "E:/r"};
        const std::vector<std::string> gnu_synth = bmi_replay_argv(
            nullptr,
            false,
            "/std:c++20",
            "c++20",
            axes,
            {},
            gnu_base,
            {});
        cg::test::check(
            r,
            gnu_synth
                == std::vector<
                    std::
                        string>{"-std=c++20", "-fms-extensions", "-I", "E:/r", "-x", "c++-module", "--precompile-reduced-bmi"},
            "the synthesized gnu baseline keeps one -std only");
        cg::test::check(
            r,
            argv_exe_free(gnu_synth),
            "the synthesized gnu argv is "
            "exe-free");
    }

    void fallback_shape(R& r)
    {
        // 只断言行为契约（驱动报告是事实来源——
        // 绝不与机器字面量做字符串比较）：语言开关
        // 打头，且每个发现的 include 目录都按驱动报告的
        // 精确顺序在磁盘上存在。
        const auto argv = cc::fallback_argv();
        cg::test::check(
            r,
            !argv.empty() && argv[0] == "-std=c++23",
            "fallback argv starts with -std=c++23");
        std::vector<std::string> dirs;
        dirs.reserve(argv.size() / 2);
        for (std::size_t i = 1; i + 1 < argv.size(); i += 2)
        {
            cg::test::check(
                r,
                argv[i] == "-I",
                "fallback include entries use -I");
            dirs.push_back(argv[i + 1]);
        }
        cg::test::check(
            r,
            !dirs.empty(),
            "fallback carries a non-empty include set");
        bool all_exist = !dirs.empty();
        for (const std::string& dir : dirs)
        {
            all_exist =
                all_exist
                && std::filesystem::is_directory(std::filesystem::path{dir});
        }
        cg::test::check(
            r,
            all_exist,
            "every fallback include dir exists on disk");
        const cc::DriverInfo& info = cc::driver_info();
        cg::test::check(
            r,
            dirs == info.cxx_include_dirs,
            "fallback include order matches the driver report");
    }

    void cl_mode_supplement_gate(R& r)
    {
        // cl 方言闸门：重放 argv 带 --driver-mode=cl 时
        // 驱动补充绝不能追加任何 -isystem 的 GNU include
        // 根（clang-cl 无视 -isystem，其值变成孤儿
        // 位置实参，解析以 CXError_ASTReadError 失败，
        // 多轮实测已证）。闸门盯的是精确旗标令牌：不带
        // 该开关的 cl 风格旗标仍得到 GNU 补充。
        const std::vector<std::string> cl_argv = {
            "--driver-mode=cl",
            "/c",
            "/std:c++20"};
        const std::vector<std::string> cl_extra =
            cc::driver_supplement("x.cpp", cl_argv);
        bool cl_has_isystem = false;
        for (const std::string& token : cl_extra)
        {
            cl_has_isystem = cl_has_isystem || token == "-isystem";
        }
        cg::test::check(
            r,
            !cl_has_isystem,
            "cl-mode supplement appends no -isystem token");
        bool cl_flag_after_gate = false;
        for (std::size_t i = 0; i + 1 < cl_extra.size(); ++i)
        {
            cl_flag_after_gate =
                cl_flag_after_gate || (cl_extra[i] == "-resource-dir");
        }
        cg::test::check(
            r,
            cl_flag_after_gate,
            "-resource-dir still appended in cl mode (v2_res)");

        const std::vector<std::string> gnu_argv = {"-std=c++20"};
        const std::vector<std::string> gnu_extra =
            cc::driver_supplement("x.cpp", gnu_argv);
        bool gnu_has_isystem = false;
        for (const std::string& token : gnu_extra)
        {
            gnu_has_isystem = gnu_has_isystem || token == "-isystem";
        }
        cg::test::check(
            r,
            gnu_has_isystem,
            "GNU-mode supplement still appends -isystem roots");

        const std::vector<std::string> cl_no_switch = {"/c", "/std:c++20"};
        const std::vector<std::string> cl_no_switch_extra =
            cc::driver_supplement("x.cpp", cl_no_switch);
        bool no_switch_isystem = false;
        for (const std::string& token : cl_no_switch_extra)
        {
            no_switch_isystem = no_switch_isystem || token == "-isystem";
        }
        cg::test::check(
            r,
            no_switch_isystem,
            "cl flags without the switch keep the GNU supplement");
    }

    /// 补充参数里 -isystem 旗标后的取值列表。
    [[nodiscard]]
    auto isystem_values_of(const std::vector<std::string>& extra)
        -> std::vector<std::string>
    {
        std::vector<std::string> values;
        for (std::size_t i = 0; i + 1 < extra.size(); i += 2)
        {
            if (extra[i] == "-isystem")
            {
                values.push_back(extra[i + 1]);
            }
        }
        return values;
    }

    // B 路接线闸门（接线修复）：带 -nostdinc++ 的 TU
    // 自带 C++ 标准库 include，所以补充绝不能在其后
    // 再追加引擎自己的标准库根（libc++ 也带
    // stddef.h/__config——libc++ 接线的引擎上做 libstdc++ 接线的解析
    // 会经产品撞上两个标准库）。引擎的 C 根保留
    // （它们不是标准库）；不带 -nostdinc++ 的 TU 保全集。
    void supplement_stdlib_rewire_gate(R& r)
    {
        const cc::DriverInfo&    info = cc::driver_info();
        std::vector<std::string> stdlib_roots; // C++ 有报、C 没有
        for (const std::string& dir : info.cxx_include_dirs)
        {
            if (std::ranges::find(info.c_include_dirs, dir)
                == info.c_include_dirs.end())
            {
                stdlib_roots.push_back(dir);
            }
        }
        const std::vector<std::string> wired = {
            "-std=c++20",
            "-nostdinc++",
            "-isystem",
            "E:/ccg-test/libstdcxx",
            "-isystem",
            "E:/ccg-test/libstdcxx/target"};
        const std::vector<std::string> wired_extra =
            cc::driver_supplement("x.cpp", wired);
        const std::vector<std::string> wired_values =
            isystem_values_of(wired_extra);
        bool leaked = false;
        for (const std::string& root : stdlib_roots)
        {
            leaked =
                leaked
                || std::ranges::find(wired_values, root) != wired_values.end();
        }
        cg::test::check(
            r,
            !leaked,
            "-nostdinc++ TU gets no engine stdlib root appended");
        if (!info.c_include_dirs.empty())
        {
            bool c_root_kept = false;
            for (const std::string& c : info.c_include_dirs)
            {
                c_root_kept =
                    c_root_kept
                    || std::ranges::find(wired_values, c) != wired_values.end();
            }
            cg::test::check(
                r,
                c_root_kept,
                "-nostdinc++ TU still gets the C roots");
        }

        // 不带 -nostdinc++ 时引擎集合保持完整（经典
        // 补充契约文档记载的历史行为）。
        const std::vector<std::string> plain = {"-std=c++20"};
        const std::vector<std::string> plain_extra =
            cc::driver_supplement("x.cpp", plain);
        const std::vector<std::string> plain_values =
            isystem_values_of(plain_extra);
        bool stdlib_present = stdlib_roots.empty();
        for (const std::string& root : stdlib_roots)
        {
            stdlib_present =
                stdlib_present
                || std::ranges::find(plain_values, root) != plain_values.end();
        }
        cg::test::check(
            r,
            stdlib_present,
            "a plain TU keeps the engine stdlib supplement");
    }

    // 并发排空回归：驱动分区的进程运行器并发排空
    // stdout 与 stderr。洪水桩先向 stderr 写 128 KiB（远超
    // 任何匿名管道缓冲），再写仅有的一行 stdout，然后
    // 以 0 退出——正是模板错误级联导致 BMI 构建失败的
    // 形态。先 stdout 的顺序排空在此死锁（桩
    // 阻塞在写 stderr、父进程阻塞在读 stdout）：
    // 本测试能跑完就是断言；ctest 超时就是旧缺陷的
    // 失败形态。
    void driver_concurrent_drain(R& r)
    {
#ifdef CCG_FLOODSTUB_EXE
        cg::test::check(
            r,
            cc::compiler_version(CCG_FLOODSTUB_EXE, "").empty(),
            "flooded --version probe completes with no version "
            "token (no pipe deadlock)");
#else
        cg::test::check(r, true, "flood stub absent on this platform (skip)");
#endif
    }

    void driver_discovery(R& r)
    {
        // 运行期发现契约：能找到驱动（在被加载的
        // libclang.dll 旁边，或在 PATH 上），问询产出资源目录与
        // 条目真实存在的非空 include 清单，且显式根
        // 覆盖优先——包括它点名的前缀里没有驱动时
        // 大声失败，而不是静默回退到发现链。
        const cc::DriverInfo& info = cc::driver_info();
        cg::test::check(
            r,
            !info.driver.empty(),
            "a clang driver executable is discovered");
        if (info.driver.empty())
        {
            return;
        }
        cg::test::check(
            r,
            std::filesystem::is_regular_file(
                std::filesystem::path{info.driver}),
            "discovered driver exists on disk");
        cg::test::check(
            r,
            !info.resource_dir.empty(),
            "driver reports a resource dir");
        cg::test::check(
            r,
            std::filesystem::is_directory(
                std::filesystem::path{info.resource_dir}),
            "reported resource dir exists");
        cg::test::check(
            r,
            !info.cxx_include_dirs.empty() && !info.c_include_dirs.empty(),
            "driver reports non-empty C++ and C include sets");
        bool all_exist = true;
        for (const std::string& dir : info.cxx_include_dirs)
        {
            all_exist =
                all_exist
                && std::filesystem::is_directory(std::filesystem::path{dir});
        }
        for (const std::string& dir : info.c_include_dirs)
        {
            all_exist =
                all_exist
                && std::filesystem::is_directory(std::filesystem::path{dir});
        }
        cg::test::check(
            r,
            all_exist,
            "every reported include dir exists on disk");

        // 指向真实工具链的显式覆盖在工具链内解析。
        const std::string    bin_dir = std::filesystem::path{info.driver}
                                           .parent_path()
                                           .lexically_normal()
                                           .generic_string();
        const cc::DriverInfo real = cc::resolve_driver(bin_dir);
        cg::test::check(
            r,
            !real.driver.empty() && !real.cxx_include_dirs.empty(),
            "explicit root with a driver resolves");
        // 点名空前缀的显式覆盖失败——覆盖已接管；
        // 绝不静默回退到发现链。
        const cc::DriverInfo bogus =
            cc::resolve_driver(bin_dir + "/definitely-no-driver-here");
        cg::test::check(
            r,
            bogus.driver.empty() && bogus.cxx_include_dirs.empty(),
            "explicit override without a driver fails, no fallback");
    }

    void parse_and_walk_baseline(R& r)
    {
        const auto db = cc::load(fixture("compile_commands.json"), {});
        const cc::CompileCommand* entry = cc::find_command(
            db,
            cg::util::norm_path(fixture("baseline_main.cpp")));
        cg::test::check(
            r,
            entry != nullptr,
            "database has the main-file entry");
        if (entry == nullptr)
        {
            return;
        }

        const cc::Index index;
        cg::test::check(r, index.valid(), "index initializes");
        const cc::ParsedTu parsed = cc::parse(index, entry->file, entry->argv);
        cg::test::check(r, parsed.tu.valid(), "parse produces a valid TU");
        cg::test::check(
            r,
            parsed.errors.empty(),
            "no diagnostics at severity>=error");

        const auto all =
            collect(parsed.tu, [](const CXCursor&) { return true; }, false);
        cg::test::check(r, !all.empty(), "cursor tree is walkable");

        // 头文件侧声明（未过滤遍历）。
        const CXCursor* widget =
            by_kind_spelling(CXCursor_ClassDecl, "Widget", all);
        cg::test::check(r, widget != nullptr, "class Widget found");
        const CXCursor* typedefed =
            by_kind_spelling(CXCursor_TypedefDecl, "BaseWidget", all);
        cg::test::check(r, typedefed != nullptr, "typedef BaseWidget found");
        if (typedefed != nullptr)
        {
            cg::test::check(
                r,
                cc::underlying_type_spelling(*typedefed) == "WidgetBase",
                "typedef underlying type spelling");
        }

        // 非函数光标的 clang_getNumArgTypes 返回 -1：
        // 拼写辅助函数必须回空，而不是把 -1 重解释成
        // size_t 去保留（守卫之前 std::length_error 杀死过整个
        // TU；在参考语料上发现）。
        if (widget != nullptr)
        {
            cg::test::check(
                r,
                cc::arg_type_spellings(*widget).empty(),
                "arg_type_spellings on a record cursor is empty");
        }

        // 光标向量必须活得比 by_kind_spelling 返回的
        // 指针久（它按指针递出元素）：把 collect()
        // 结果绑定到具名局部变量，绝不传临时对象。
        const auto declarations = collect(
            parsed.tu,
            [](const CXCursor& c) { return !cc::is_definition(c); },
            false);
        const CXCursor* base_weight =
            by_kind_spelling(CXCursor_CXXMethod, "weight", declarations);
        if (base_weight != nullptr)
        {
            cg::test::check(
                r,
                cc::is_virtual_method(*base_weight)
                    && cc::is_const_method(*base_weight),
                "WidgetBase::weight decl is virtual and const");
        }
        else
        {
            cg::test::check(r, false, "WidgetBase::weight declaration found");
        }

        const auto pure_virtuals = collect(
            parsed.tu,
            [](const CXCursor& c) { return cc::is_pure_virtual(c); },
            false);
        const CXCursor* pure =
            by_kind_spelling(CXCursor_CXXMethod, "describe", pure_virtuals);
        cg::test::check(r, pure != nullptr, "pure virtual describe found");

        const CXCursor* dtor =
            by_kind_spelling(CXCursor_Destructor, "~WidgetBase", all);
        cg::test::check(
            r,
            dtor != nullptr && cc::is_destructor(*dtor),
            "destructor detected via kind + is_destructor");

        // 主文件定义。存在两个 weight() 定义（WidgetBase 的
        // 与 Widget 的）；按语义父选中 Widget 的。
        const auto own =
            collect(parsed.tu, [](const CXCursor&) { return true; }, true);
        const CXCursor* weight_def = nullptr;
        for (const CXCursor& c : own)
        {
            if (cc::kind(c) == CXCursor_CXXMethod && cc::spelling(c) == "weight"
                && cc::is_definition(c)
                && cc::spelling(cc::semantic_parent(c)) == "Widget")
            {
                weight_def = &c;
            }
        }
        cg::test::check(
            r,
            weight_def != nullptr,
            "Widget::weight definition in main file");
        const CXCursor* weigh_def =
            by_kind_spelling(CXCursor_FunctionDecl, "weigh", own);
        cg::test::check(
            r,
            weigh_def != nullptr,
            "free function weigh definition found");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FunctionDecl, "run", own) != nullptr,
            "free function run definition found");

        if (weight_def != nullptr)
        {
            cg::test::check(
                r,
                cc::is_definition(*weight_def),
                "weight in main file is a definition");
            const CXCursor parent = cc::semantic_parent(*weight_def);
            cg::test::check(
                r,
                cc::spelling(parent) == "Widget"
                    && cc::kind(parent) == CXCursor_ClassDecl,
                "semantic parent of the method is the class");
            const CXCursor lexical = cc::lexical_parent(*weight_def);
            cg::test::check(
                r,
                cc::spelling(lexical) == "baseline"
                    && cc::kind(lexical) == CXCursor_Namespace,
                "lexical parent of an out-of-line def is the namespace");

            const cc::SourceLocation loc = cc::location(*weight_def);
            cg::test::check(
                r,
                cg::util::norm_path(loc.file)
                    == cg::util::norm_path(entry->file),
                "location reports the main file");
            cg::test::check(
                r,
                loc.line >= 1 && loc.column >= 1,
                "location line/column are sane 1-based values");

            const cc::SourceExtent body = cc::extent(*weight_def);
            cg::test::check(
                r,
                body.begin.line <= body.end.line,
                "extent begin precedes end");
        }

        if (weigh_def != nullptr)
        {
            cg::test::check(
                r,
                cc::result_type_spelling(*weigh_def) == "int",
                "result type of weigh is int");
            const std::string full_type = cc::type_spelling(*weigh_def);
            cg::test::check(
                r,
                full_type.find("int (") != std::string::npos
                    && full_type.find("Widget &") != std::string::npos,
                "full type spelling of a function cursor");
            cg::test::check(
                r,
                cc::kind_spelling(cc::kind(*weigh_def)) == "FunctionDecl",
                "kind spelling for a function cursor");
            cg::test::check(
                r,
                cc::display_name(*weigh_def).find("weigh") != std::string::npos,
                "display name contains the unqualified name");
        }

        const CXCursor* reserve_def = nullptr;
        for (const CXCursor& c : own)
        {
            if (cc::kind(c) == CXCursor_CXXMethod
                && cc::spelling(c) == "reserve" && cc::is_definition(c))
            {
                reserve_def = &c;
            }
        }
        if (reserve_def != nullptr)
        {
            cg::test::check(
                r,
                cc::is_static_method(*reserve_def),
                "static method predicate");
        }
        else
        {
            cg::test::check(r, false, "Widget::reserve definition found");
        }

        // 经调用解析被引用者。
        bool method_call_resolved = false;
        bool free_call_resolved = false;
        for (const CXCursor& c : own)
        {
            const CXCursorKind k = cc::kind(c);
            if (k != CXCursor_CallExpr && k != CXCursor_MemberRefExpr)
            {
                continue;
            }
            const CXCursor ref = cc::referenced(c);
            if (cc::is_null(ref))
            {
                continue;
            }
            if (cc::spelling(ref) == "weight"
                && cc::spelling(cc::semantic_parent(ref)) == "Widget")
            {
                method_call_resolved = true;
            }
            if (cc::spelling(ref) == "weigh"
                && cc::kind(cc::semantic_parent(ref)) == CXCursor_Namespace)
            {
                free_call_resolved = true;
            }
        }
        cg::test::check(
            r,
            method_call_resolved,
            "referenced() resolves w.weight() to Widget::weight");
        cg::test::check(
            r,
            free_call_resolved,
            "referenced() resolves weigh(w) to the free function");

        cg::test::check(
            r,
            cc::is_null(cc::specialized_template(*weigh_def)),
            "specialized_template is null for a non-template");
    }

    void parse_broken_reports_errors(R& r)
    {
        const auto db = cc::load(fixture("compile_commands.json"), {});
        const cc::CompileCommand* entry =
            cc::find_command(db, cg::util::norm_path(fixture("broken.cpp")));
        cg::test::check(
            r,
            entry != nullptr,
            "database has the broken-file entry");
        if (entry == nullptr)
        {
            return;
        }

        const cc::Index    index;
        const cc::ParsedTu parsed = cc::parse(index, entry->file, entry->argv);
        cg::test::check(
            r,
            !parsed.errors.empty(),
            "syntax error surfaces as severity>=error diagnostics");
        if (!parsed.errors.empty())
        {
            const cc::Diagnostic& d = parsed.errors.front();
            cg::test::check(
                r,
                d.severity >= cc::DiagSeverity::Error,
                "collected diagnostic is at least error severity");
            cg::test::check(
                r,
                !d.spelling.empty(),
                "diagnostic spelling present");
            cg::test::check(
                r,
                cg::util::norm_path(d.location.file).ends_with("broken.cpp")
                    && d.location.line >= 1,
                "diagnostic carries file:line");
        }
        // 部分恢复的语法树上不崩。
        std::size_t walked = 0;
        cc::children(
            parsed.tu.cursor(),
            [&walked](const CXCursor&)
            {
                ++walked;
                return true;
            });
        cg::test::check(r, walked > 0, "recovered cursor tree is walkable");
    }

    void header_only_fallback_parse(R& r)
    {
        const cc::Index    index;
        const cc::ParsedTu parsed =
            cc::parse(index, fixture("baseline.hpp"), cc::fallback_argv());
        cg::test::check(
            r,
            parsed.tu.valid() && parsed.errors.empty(),
            "header-only TU parses clean with the fallback argv");
        const auto all =
            collect(parsed.tu, [](const CXCursor&) { return true; }, false);
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_ClassDecl, "Widget", all) != nullptr,
            "class Widget found in the header-only parse");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_TypedefDecl, "BaseWidget", all)
                != nullptr,
            "typedef found in the header-only parse");

        const std::string version = cc::libclang_version();
        cg::test::check(
            r,
            version.find("clang") != std::string::npos,
            "libclang version string available");
    }

    /**
     * @brief 宏错位 extent 的 token 卫生闸与二元运算 opcode 门面。
     *
     * @internal SWIG PYTHON 后端把桥接 API 生成 #define，宏调用表达式的
     * extent 两端 spelling 各钉在不同宏定义处：clang_tokenize 按两端
     * spelling 偏移词法化，会扫过两个定义点之间的全部源码（真库单翻译
     * 单元曾把 330 秒烧在这上面）。夹具把 CONV 与 FLAG_KEEP 两个宏定义
     * 隔开约 5KB 复现错位形态：宏调用表达式的 begin 落在 CONV 定义处、
     * end 落在 FLAG_KEEP 定义处，正序但相距 5.4KB（真 TU 慢样本的同款
     * 坐标形态）。断言闸拒绝这次 tokenize、对正常语句（含 900 字节的
     * 长初始化列表）不误伤、赋值判定经 opcode（Sema 建树的权威判定，
     * 不经源码词法化）与 token 判定在健康形态下互证。另设一条约 5KB
     * 的合法巨型初始化表：span 闸对它按设计静默归空（token 级子判定
     * 宁漏勿误），断言钉住该边界行为——token 为空且解析产物面不崩。
     */
    void tokenize_macro_extent_gate(R& r)
    {
        // 夹具 tok_gate_macros.cpp：两个宏定义间垫 130 个填充函数（约
        // 5.4KB）；尾部先是一个 450 元素的扁平初始化表（约 900 字节的
        // 单语句，验证长语句不被闸误伤），再是一条 3000 槽、约 5KB 的
        // 合法巨型初始化表（验证合法超长单语句被 span 闸静默归空）。
        const cc::Index    index;
        const cc::ParsedTu parsed = cc::parse(
            index,
            fixture("tok_gate_macros.cpp"),
            cc::fallback_argv());
        cg::test::check(
            r,
            parsed.tu.valid() && parsed.errors.empty(),
            "macro-mismatch gate fixture parses clean");
        if (!parsed.tu.valid())
        {
            return;
        }

        const auto all =
            collect(parsed.tu, [](const CXCursor&) { return true; }, false);

        std::size_t assign_live = 0;         // 赋值 opcode 且拿到真 token
        bool        assign_token_eq = false; // 赋值的真 token 里有 '='
        bool        add_assign = false;      // '+=' 有独立 opcode
        bool        compare = false;         // '==' 有独立 opcode
        std::size_t calls = 0;
        std::size_t calls_gated = 0; // 宏调用表达式拿空 token
        std::size_t max_tokens = 0;  // 单游标 token 数的峰值
        for (const CXCursor& c : all)
        {
            const std::vector<std::string> toks = cc::token_spellings(c);
            if (toks.size() > max_tokens)
            {
                max_tokens = toks.size();
            }
            const CX_BinaryOperatorKind op = cc::binary_opcode(c);
            if (op == CX_BO_Assign)
            {
                if (!toks.empty())
                {
                    ++assign_live;
                }
                assign_token_eq = assign_token_eq
                                  || std::ranges::find(toks, "=") != toks.end();
            }
            else if (op == CX_BO_AddAssign)
            {
                add_assign = true;
            }
            else if (op == CX_BO_EQ)
            {
                compare = true;
            }
            if (cc::kind(c) == CXCursor_CallExpr)
            {
                ++calls;
                if (toks.empty())
                {
                    ++calls_gated;
                }
            }
        }
        cg::test::check(
            r,
            calls == 1 && calls_gated == 1,
            "macro-mismatch call expression yields no tokens (span gate)");
        cg::test::check(
            r,
            assign_live >= 1,
            "plain assignments keep their tokens");
        cg::test::check(
            r,
            assign_token_eq,
            "healthy assignments still show '=' to the token scan");
        cg::test::check(
            r,
            max_tokens >= 800,
            "the 900-byte plain initializer still tokenizes in full");
        const CXCursor* huge =
            by_kind_spelling(CXCursor_VarDecl, "huge_table", all);
        cg::test::check(
            r,
            huge != nullptr,
            "the oversized legal initializer table is present in the fixture");
        if (huge != nullptr)
        {
            cg::test::check(
                r,
                cc::token_spellings(*huge).empty(),
                "a legal >4KB single statement is span-gated: its tokens"
                " come back empty without breaking the parse");
        }
        cg::test::check(
            r,
            max_tokens <= 1024,
            "no cursor tokenizes beyond statement scale");
        cg::test::check(
            r,
            add_assign && compare,
            "opcode distinguishes += and == from =");
    }

    // --------------------------------------------------- export
    // 覆盖层 ------

    // export 抹除覆盖层的文本级契约：对模块辖域内的 `export` 关键字同宽抹除，
    // 绝不碰 `export module`/`export import`，绝不进字面量、注释、
    // 指令或更长的标识符内部。
    void export_overlay_rules(R& r)
    {

        // 永远同宽（保列保行正是全部意义）。
        const std::pair<std::string_view, std::string_view> keep_or_blank[] = {
            {"export module m;", "export module m;"},
            {"export import m.part;", "export import m.part;"},
            {"export import std;", "export import std;"},
            {"export\nmodule m;", "export\nmodule m;"},
            {"export /* c */ import m;", "export /* c */ import m;"},
            {"export namespace n { int f(); }",
             "       namespace n { int f(); }"},
            {"export { int g(); }", "export { int g(); }"},
            {"export auto f();", "       auto f();"},
            {"export template <class T> struct S {};",
             "       template <class T> struct S {};"},
            {"int exported_var;", "int exported_var;"},
            {"int my_export;", "int my_export;"},
            {"// export namespace\nint x;", "// export namespace\nint x;"},
            {"/* export */ int x;", "/* export */ int x;"},
            {"const char* s = \"export namespace\";",
             "const char* s = \"export namespace\";"},
            {"char c = 'x'; char d = '\"';", "char c = 'x'; char d = '\"';"},
            {"#define E export\nint y;", "#define E export\nint y;"},
            {"export module m;\nexport namespace n { int f(); }",
             "export module m;\n       namespace n { int f(); }"},
        };
        for (const auto& [src, want] : keep_or_blank)
        {
            const std::string got = cc::export_overlay(src);
            cg::test::check(
                r,
                got == want,
                "overlay: '" + std::string{src} + "' -> '" + got + "' (want '"
                    + std::string{want} + "')");
            cg::test::check(
                r,
                got.size() == src.size(),
                "overlay preserves width for '" + std::string{src} + "'");
        }

        // 末尾后面无物的 `export` 保持原样。
        cg::test::check(
            r,
            cc::export_overlay("int x;\nexport") == "int x;\nexport",
            "export at EOF is not blanked");

        // 幂等：抹两次等于抹一次。
        const std::string mixed =
            "export module m;\nexport namespace a {\nexport int f();\n}\n"
            "// export\nexport { int g(); }\n";
        cg::test::check(
            r,
            cc::export_overlay(cc::export_overlay(mixed))
                == cc::export_overlay(mixed),
            "overlay is idempotent");

        // 原始字符串里的标记词 `export` 完好无损。
        const std::string raw = "auto r = R\"x(export namespace)x\";";
        cg::test::check(
            r,
            cc::export_overlay(raw) == raw,
            "export inside a raw string");
    }

    // 迷你夹具上的解析级证明：覆盖层生效后，
    // `export namespace`/`export {}`/逐声明 `export` 之下的
    // 声明以普通光标浮出（libclang 的 ExportDecl 空缺，
    // 此处是绕过而非修复）。
    void export_overlay_parse(R& r)
    {
        const std::string              file = fixture("mini.ixx");
        const cc::Index                index;
        const std::vector<std::string> argv{"-std=c++23", "-x", "c++-module"};

        const cc::ParsedTu parsed = cc::parse(index, file, argv, true);
        cg::test::check(r, parsed.tu.valid(), "module fixture parses");
        cg::test::check(
            r,
            parsed.errors.empty(),
            "module fixture parses clean under the overlay");

        const auto all =
            collect(parsed.tu, [](const CXCursor&) { return true; }, true);
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_Namespace, "n", all) != nullptr,
            "export namespace n is visible");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FunctionDecl, "visible_function", all)
                != nullptr,
            "function inside export namespace is visible");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_StructDecl, "VisibleStruct", all)
                != nullptr,
            "struct inside export namespace is visible");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FieldDecl, "field", all) != nullptr,
            "field inside export namespace is visible");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FunctionDecl, "single_export", all)
                != nullptr,
            "per-declaration export is visible");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FunctionDecl, "classic_function", all)
                != nullptr,
            "plain namespace content stays visible");

        // 位置必须与磁盘文件一致：覆盖层同宽，
        // 所以 `visible_function` 报告其手写行（在 export
        // 命名空间包装之外）。行号按锚串从夹具文本动态
        // 定位（锚串前的换行数加一），夹具头部注释增删
        // 不再牵动断言常量。
        const std::string mini_src = read_fixture("mini.ixx");
        const std::size_t anchor = mini_src.find("int visible_function()");
        const std::size_t authored_line =
            anchor == std::string::npos
                ? 0
                : static_cast<std::size_t>(std::count(
                      mini_src.begin(),
                      mini_src.begin() + static_cast<std::ptrdiff_t>(anchor),
                      '\n'))
                      + 1;
        const CXCursor* f =
            by_kind_spelling(CXCursor_FunctionDecl, "visible_function", all);
        if (f != nullptr)
        {
            const cc::SourceLocation loc = cc::location(*f);
            cg::test::check(
                r,
                authored_line != 0 && loc.line == authored_line
                    && loc.column >= 1,
                "overlay keeps authored line/column");
        }

        // 经典 TU 闸门：同一文件不带模块旗标解析走
        // 字节精确路径（覆盖层绝不在模块模式外触发）。
        const cc::ParsedTu classic = cc::parse(index, file, argv, false);
        cg::test::check(
            r,
            classic.tu.valid() && classic.errors.empty(),
            "module_tu=false parses the file byte-exact");
    }

    // 解析级证明：与 import 同一物理行的跟随代码
    // 在重写后活着进入被索引的解析。修复前，长成的
    // `#include` 让预处理器丢掉跟随令牌（"extra tokens
    // at end of `#include` directive"，仅警告）：TU 解析干净，
    // 跟随者却从索引中静默消失。仅注释的
    // 跟随行保留指令，所以头文件/伞头内容
    // 仍被拉入——既有的正确行为得以保留。
    void same_line_follower_parse(R& r)
    {
        const cc::Index index;

        // 头单元通道（经典 TU）：跟随函数必须
        // 被索引，且 follower_cfg.h 的内容必须经仅注释行上
        // 发出的指令抵达。
        cc::ModuleOverlay hdr;
        hdr.header_unit_includes = true;
        const cc::ParsedTu hp = cc::parse(
            index,
            fixture("follower_header.cpp"),
            {"-std=c++23"},
            false,
            &hdr);
        cg::test::check(
            r,
            hp.tu.valid() && hp.errors.empty(),
            "header-channel fixture parses clean under the overlay");
        const auto hc =
            collect(hp.tu, [](const CXCursor&) { return true; }, false);
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FunctionDecl, "follower_impl", hc)
                != nullptr,
            "same-line follower function is indexed (header "
            "channel)");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FunctionDecl, "follower_cfg_value", hc)
                != nullptr,
            "comment-only follower line still emits the include");

        // 伞孪生（MSVC 工具链混合 TU 形态）：假伞头
        // 经 -I 搭上解析 include 路径。
        cc::ModuleOverlay umb;
        umb.std_umbrella = true;
        const cc::ParsedTu up = cc::parse(
            index,
            fixture("follower_umbrella.cpp"),
            {"-std=c++23", "-I", fixture("umbrella_root")},
            false,
            &umb);
        cg::test::check(
            r,
            up.tu.valid() && up.errors.empty(),
            "umbrella fixture parses clean under the overlay");
        const auto uc =
            collect(up.tu, [](const CXCursor&) { return true; }, false);
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FunctionDecl, "umbrella_follow", uc)
                != nullptr,
            "same-line follower function is indexed (umbrella "
            "twin)");
        cg::test::check(
            r,
            by_kind_spelling(CXCursor_FunctionDecl, "umbrella_marker", uc)
                != nullptr,
            "comment-only follower line still emits the umbrella "
            "include");
    }

    // --------------------------------------------------------- 引擎配置
    // --------- 按库的 LLVM 引擎解析：对假
    // 根的优先级矩阵（空标记文件——解析只探测
    // 存在性，从不加载），外加已加载引擎的身份面。

    /// 建出假引擎条目文件（含父目录）并断言创建成功。
    void touch_fake_entry(R& r, const std::filesystem::path& p)
    {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out{p, std::ios::binary};
        cg::test::check(
            r,
            out.good(),
            std::string{"fake engine entry created: "} + p.generic_string());
    }

    /// 建出假驱动条目文件（含父目录）并断言创建成功。
    void touch_fake_driver_entry(R& r, const std::filesystem::path& p)
    {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out{p, std::ios::binary};
        cg::test::check(
            r,
            out.good(),
            std::string{"fake driver entry created: "} + p.generic_string());
    }

    /// 四路输入的驱动链解析（只取驱动路径）。
    [[nodiscard]]
    auto driver_chain_of(
        const std::string& override_root,
        const std::string& engine_library,
        const std::string& env_root,
        const std::string& path_env) -> std::string
    {
        return cc::resolve_driver_chain_from(
                   override_root,
                   engine_library,
                   env_root,
                   path_env)
            .driver;
    }

    /// 目录内驱动文件的规范化路径。
    [[nodiscard]]
    auto driver_path_in(const std::string& driver, const std::string& dir)
        -> std::string
    {
        return (std::filesystem::path{dir} / driver)
            .lexically_normal()
            .generic_string();
    }

    /// 解析抛出的 runtime_error 文本是否含给定针串（不抛给假）。
    [[nodiscard]]
    auto resolve_throws_with(
        const std::string& configured,
        const std::string& env,
        const std::string& paths,
        std::string_view   needle) -> bool
    {
        try
        {
            (void)cc::engine::resolve_library_from(configured, env, paths);
        }
        catch (const std::runtime_error& e)
        {
            return std::string_view{e.what()}.find(needle)
                   != std::string_view::npos;
        }
        return false;
    }

    void engine_resolution_matrix(R& r)
    {
        namespace fs = std::filesystem;
        TempProj          proj{"ccg-engine-"};
        std::error_code   ec;
        const std::string lib{cc::engine::library_file_name};
        const std::string driver{cc::engine::discovery_driver_names[0]};

        // rootA：库在 bin/ 下的前缀（msys2/VS 布局）。
        const std::string root_a = (proj.base / "a").generic_string();
        touch_fake_entry(r, proj.base / "a" / "bin" / driver);
        touch_fake_entry(r, proj.base / "a" / "bin" / lib);
        // rootB：库只在 lib/ 下的前缀（已批准设计的
        // root/lib 拼写）。
        const std::string root_b = (proj.base / "b").generic_string();
        touch_fake_entry(r, proj.base / "b" / "lib" / lib);
        // rootC：发现根——PATH 上的 clang 驱动，库在
        // 其旁边。
        const std::string dir_c = (proj.base / "c" / "bin").generic_string();
        touch_fake_entry(r, proj.base / "c" / "bin" / driver);
        touch_fake_entry(r, proj.base / "c" / "bin" / lib);
        // rootD：工具链根里哪里都没有库的驱动。
        const std::string dir_d = (proj.base / "d" / "bin").generic_string();
        touch_fake_entry(r, proj.base / "d" / "bin" / driver);
        // 一个配置成文件（库本身，任意名字）的根和一个空目录。
        const std::string direct =
            (proj.base / "direct" / "myclang.dll").generic_string();
        touch_fake_entry(r, proj.base / "direct" / "myclang.dll");
        const std::string root_empty = (proj.base / "empty").generic_string();
        fs::create_directories(fs::path{root_empty}, ec);

        const std::string& path_c = dir_c;
        const auto         throws_with = [&](const std::string& configured,
                                             const std::string& env,
                                             const std::string& paths,
                                             std::string_view   needle)
        {
            try
            {
                (void)cc::engine::resolve_library_from(configured, env, paths);
            }
            catch (const std::runtime_error& e)
            {
                return std::string_view{e.what()}.find(needle)
                       != std::string_view::npos;
            }
            return false;
        };

        // r1：配置成文件者胜过环境变量与发现。
        cg::test::check(
            r,
            cc::engine::resolve_library_from(direct, root_b, path_c)
                == cg::util::norm_path(direct),
            "configured library file wins over env and discovery");
        // r2：配置的前缀经 bin/ 解析。
        cg::test::check(
            r,
            cc::engine::resolve_library_from(root_a, root_b, path_c)
                == cg::util::norm_path((fs::path{root_a} / "bin" / lib)
                                           .lexically_normal()
                                           .generic_string()),
            "configured prefix resolves bin/ spelling");
        // r2b：lib/ 拼写也行。
        cg::test::check(
            r,
            cc::engine::resolve_library_from(root_b, "", "")
                == cg::util::norm_path((fs::path{root_b} / "lib" / lib)
                                           .lexically_normal()
                                           .generic_string()),
            "configured prefix resolves lib/ spelling");
        // r3：环境变量胜过 PATH 发现。
        cg::test::check(
            r,
            cc::engine::resolve_library_from("", root_b, path_c)
                == cg::util::norm_path((fs::path{root_b} / "lib" / lib)
                                           .lexically_normal()
                                           .generic_string()),
            "CCG_LLVM_ROOT root beats PATH discovery");
        // r4：默认发现在喂入的 PATH 上找到工具链。
        cg::test::check(
            r,
            cc::engine::resolve_library_from("", "", path_c)
                == cg::util::norm_path((fs::path{dir_c} / lib)
                                           .lexically_normal()
                                           .generic_string()),
            "default discovery resolves beside the PATH driver");
        // r4b/r4c：PATH 条目合法地携带尾分隔符或
        // 点号；工具链根必须从折叠后的条目推导——
        // parent_path() 保留空尾元素，未修剪的
        // "E:/x/bin/" 会把根本身推导成 "E:/x/bin"，
        // 邻驱探测会去 bin/bin 找而落空。
        cg::test::check(
            r,
            cc::engine::resolve_library_from("", "", dir_c + "/")
                == cg::util::norm_path((fs::path{dir_c} / lib)
                                           .lexically_normal()
                                           .generic_string()),
            "trailing-separator PATH entry resolves beside the driver");
        cg::test::check(
            r,
            cc::engine::resolve_library_from("", "", dir_c + "/.")
                == cg::util::norm_path((fs::path{dir_c} / lib)
                                           .lexically_normal()
                                           .generic_string()),
            "dot-suffixed PATH entry resolves beside the driver");
        // r5：配置的根没有库是可操作的错误。
        cg::test::check(
            r,
            resolve_throws_with(root_empty, "", "", root_empty)
                && resolve_throws_with(root_empty, "", "", "llvm-path"),
            "configured root without a library fails, naming the "
            "root and the llvm-path knob");
        // r6：哪里都没有 -> 点名每个旋钮的可操作错误。
        cg::test::check(
            r,
            resolve_throws_with("", "", "", "CCG_LLVM_ROOT")
                && resolve_throws_with("", "", "", "llvm-path"),
            "no engine at all fails, naming env and config knobs");
        // r7：PATH 上的驱动而其根没有库时确定性
        // 失败（绝不静默滑到另一个
        // 工具链）。
        cg::test::check(
            r,
            resolve_throws_with("", "", dir_d, "driver"),
            "driver without a library fails naming the driver");
    }

    void engine_load_identity(R& r)
    {
        namespace fs = std::filesystem;
        // 构造 Index 会经默认链路取得进程引擎
        // （$CCG_LLVM_ROOT，然后 PATH 发现）。
        const cc::Index index;
        cg::test::check(r, index.valid(), "Index acquires the runtime engine");

        const std::string loaded = cc::loaded_clang_library_path();
        std::error_code   ec;
        cg::test::check(
            r,
            !loaded.empty() && fs::is_regular_file(loaded, ec),
            "loaded engine path is recorded and exists");
        const std::string version = cc::libclang_version();
        cg::test::check(
            r,
            version.starts_with("clang version"),
            "engine version string captured at load");

        // 拼写成已加载文件的配置根解析到它自身。
        cg::test::check(
            r,
            cc::engine::resolve_library(loaded) == cg::util::norm_path(loaded),
            "the loaded engine file resolves to itself as a "
            "configured root");
        // 加载已加载的路径是幂等的（不抛、不切换）。
        bool idempotent = true;
        try
        {
            cc::load_clang_library(loaded);
        }
        catch (const std::exception&)
        {
            idempotent = false;
        }
        cg::test::check(r, idempotent, "loading the loaded engine is a no-op");
        // Index 存活期间切换到另一个库会被拒绝
        // （一次解析会话 = 一个世界观）。探测目标必须存在，
        // 拒绝才是关于切换而非关于加载。
        const fs::path other = fs::temp_directory_path()
                               / ("ccg-switch-probe-"
                                  + std::to_string(
                                      std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
        {
            std::error_code mec;
            fs::create_directories(other.parent_path(), mec);
            std::ofstream out{other, std::ios::binary};
        }
        bool refused = false;
        try
        {
            cc::load_clang_library(other.generic_string());
        }
        catch (const std::runtime_error&)
        {
            refused = true;
        }
        cg::test::check(
            r,
            refused,
            "switching the engine under a live Index is refused");
        std::error_code rec;
        fs::remove(other, rec);
    }

    // -------------------------------------------------------- 驱动链核心
    // 补充驱动必须跟随引擎而非环境
    // （按库世界观）：纯链核心对着假
    // 安装探测——标记文件，因为选择只探测
    // 存在性。假赢家无法被问询（对标记文件
    // CreateProcess 会失败），产出的是空补充清单而非错误——
    // 此处只断言解析出的驱动路径。
    void driver_chain_matrix(R& r)
    {
        namespace fs = std::filesystem;
        TempProj          proj{"ccg-driver-"};
        std::error_code   ec;
        const std::string driver{cc::engine::discovery_driver_names[0]};
        const std::string lib{cc::engine::library_file_name};

        // eBin：引擎库在驱动旁（bin/ 布局）。
        const std::string ebin = (proj.base / "eBin" / "bin").generic_string();
        touch_fake_driver_entry(r, proj.base / "eBin" / "bin" / driver);
        touch_fake_driver_entry(r, proj.base / "eBin" / "bin" / lib);
        // eLib：库在 lib/ 下、驱动在 bin/ 下（lib/ 布局）。
        const std::string elib = (proj.base / "eLib").generic_string();
        touch_fake_driver_entry(r, proj.base / "eLib" / "lib" / lib);
        touch_fake_driver_entry(r, proj.base / "eLib" / "bin" / driver);
        // eRoot：库文件在前缀根、驱动在 bin/ 下。
        const std::string eroot = (proj.base / "eRoot").generic_string();
        touch_fake_driver_entry(r, proj.base / "eRoot" / "myclang.dll");
        touch_fake_driver_entry(r, proj.base / "eRoot" / "bin" / driver);
        // eNone：仅引擎布局——哪都没有驱动的库。
        const std::string enone =
            (proj.base / "eNone" / "bin").generic_string();
        touch_fake_driver_entry(r, proj.base / "eNone" / "bin" / lib);
        // envP：环境可能点名的另一个工具链根。
        const std::string envp = (proj.base / "envP").generic_string();
        touch_fake_driver_entry(r, proj.base / "envP" / "bin" / driver);
        // pathP：经 PATH 找到的工具链。
        const std::string pathp =
            (proj.base / "pathP" / "bin").generic_string();
        touch_fake_driver_entry(r, proj.base / "pathP" / "bin" / driver);

        // d1：引擎自己的驱动压过环境根与 PATH。
        cg::test::check(
            r,
            driver_chain_of("", ebin + "/" + lib, envp, pathp)
                == driver_path_in(driver, ebin),
            "bin-layout engine wins over env root and PATH");
        // d2：lib/ 布局经安装前缀解析。
        cg::test::check(
            r,
            driver_chain_of("", elib + "/lib/" + lib, envp, pathp)
                == driver_path_in(driver, elib + "/bin"),
            "lib-layout engine resolves through its prefix");
        // d3：前缀根里的库文件找到 root/bin。
        cg::test::check(
            r,
            driver_chain_of("", eroot + "/myclang.dll", "", "")
                == driver_path_in(driver, eroot + "/bin"),
            "root-layout engine finds its own bin/");
        // d4/d5：仅引擎布局（没有自己的驱动）回退到
        // 环境根，再到 PATH。
        cg::test::check(
            r,
            driver_chain_of("", enone + "/" + lib, envp, pathp)
                == driver_path_in(driver, envp + "/bin"),
            "driverless engine falls back to the env root");
        cg::test::check(
            r,
            driver_chain_of("", enone + "/" + lib, "", pathp)
                == driver_path_in(driver, pathp),
            "driverless engine without env falls back to PATH");
        // d6：覆盖获胜且是唯一来源；引擎库的
        // 文件拼写锚定在文件所在目录。
        cg::test::check(
            r,
            driver_chain_of(ebin + "/" + lib, elib + "/lib/" + lib, envp, pathp)
                == driver_path_in(driver, ebin),
            "override as the library file wins and anchors");
        // d7：点名不到驱动的覆盖失败——不静默回退。
        cg::test::check(
            r,
            driver_chain_of(
                (proj.base / "nope").generic_string(),
                ebin + "/" + lib,
                envp,
                pathp)
                .empty(),
            "override without a driver fails, no fallback");
        // d8：未加载引擎——环境根生效，再到 PATH。
        cg::test::check(
            r,
            driver_chain_of("", "", envp, pathp)
                == driver_path_in(driver, envp + "/bin"),
            "no engine loaded: the env root applies");
        cg::test::check(
            r,
            driver_chain_of("", "", "", pathp) == driver_path_in(driver, pathp),
            "no engine, no env: PATH applies");
    }

    [[nodiscard]]
    auto dir_files_gone(const std::filesystem::path& dir) -> bool;

    // 驱动缓存重定键回归（缓存键=已加载引擎）：引擎在
    // 同一进程的两个索引尾巴之间静默切换（按库世界观，
    // 例如 `ccg msbuild --all`）；为前一引擎建的驱动缓存
    // 必须丢弃，否则新引擎用旧补充解析。
    // 切换时加载真实库的副本，旁边放一个驱动形状的
    // 标记文件：代码相同、安装锚点不同，重定键
    // 因此只能经解析出的驱动路径观察到。
    void driver_cache_rekeys_on_engine_switch(R& r)
    {
        namespace fs = std::filesystem;
        // 启动清扫：%TEMP% 下历史轮残留的 ccg-rekey-* 目录在此
        // 尽力删除（remove_all、失败静默）。残留多数是 OS 装载器
        // 句柄散尽延迟留下的空壳，正常可删；仍处删除挂起的删不
        // 掉，留给系统回收。只计数注记、不进断言——目的是每轮
        // 启动即净，空壳不再跨轮累积。
        {
            std::error_code sweep_ec;
            std::size_t     swept = 0;
            std::size_t     unlinked = 0;
            for (const fs::directory_entry& entry :
                 fs::directory_iterator{fs::temp_directory_path(), sweep_ec})
            {
                std::error_code kind_ec;
                if (!entry.is_directory(kind_ec)
                    || entry.path().filename().string().rfind("ccg-rekey-", 0)
                           != 0)
                {
                    continue;
                }
                std::error_code rm_ec;
                fs::remove_all(entry.path(), rm_ec);
                ++swept;
                if (!rm_ec)
                {
                    ++unlinked;
                }
            }
            std::println(
                "rekey-sweep: {} leftover ccg-rekey-* dir(s) swept, {} "
                "removed",
                swept,
                unlinked);
        }
        // 引擎由之前跑过的任一测试加载（某个 Index，或
        // 此处的显式加载——对已加载路径幂等）。
        cc::load_clang_library(cc::engine::resolve_library(""));
        const std::string original = cc::loaded_clang_library_path();
        cg::test::check(r, !original.empty(), "an engine is loaded");

        const std::string d1 = cc::driver_info().driver;
        cg::test::check(
            r,
            !d1.empty(),
            "driver resolved for the loaded engine");

        // 副本与临时目录的生存期收束进下述作用域：块尾 TempProj
        // 析构删除目录，块外紧跟文件级残留断言。Windows 锁死已加
        // 载 DLL 的文件，副本引擎不卸载则该删除必然静默失败，每
        // 轮泄漏一个 84MB 副本文件——文件级残留断言是该回归的保
        // 险丝（目录空壳/删除挂起的容忍语义见块外注释）。
        fs::path temp_base;
        {
            TempProj        proj{"ccg-rekey-"};
            std::error_code ec;
            temp_base = proj.base;
            const std::string driver_name{
                cc::engine::discovery_driver_names[0]};
            const fs::path lib_copy =
                proj.base / std::string{cc::engine::library_file_name};
            fs::copy_file(
                fs::path{original},
                lib_copy,
                fs::copy_options::overwrite_existing,
                ec);
            // 副本旁的驱动形状标记：选择只探测
            // 存在性，所以副本的安装解析到它。
            {
                std::ofstream out{proj.base / driver_name, std::ios::binary};
                cg::test::check(r, out.good(), "marker driver created");
            }

            bool switched = true;
            try
            {
                cc::load_clang_library(lib_copy.generic_string());
            }
            catch (const std::exception&)
            {
                switched = false; // 副本的依赖此处不可解析
            }
            if (switched)
            {
                const std::string d2 = cc::driver_info().driver;
                cg::test::check(
                    r,
                    cg::util::norm_path(d2)
                        == cg::util::norm_path(
                            (proj.base / driver_name).generic_string()),
                    "driver cache re-keys onto the switched engine");
                // 回到真实安装：缓存再次重定键，进程留在真实
                // 驱动上供后续测试使用。切回必须先于副本卸载——
                // 进程级引擎状态（当前引擎路径与无锁分发表）在
                // 切回完成前仍指向副本代码，先卸载会留下一个
                // 门面调用跳进未映射内存的窗口。
                cc::load_clang_library(original);
                cg::test::check(
                    r,
                    cc::driver_info().driver == d1,
                    "switching back restores the original driver");
                // 卸载副本引擎模块，解除 Windows 对副本文件的
                // 加载锁，TempProj 析构才删得掉目录。安全前提：
                // 副本生存期内只有引擎加载路径触达过它——版本
                // 查询与崩溃恢复开关，从未创建 Index 或翻译单元
                // （引擎切换本身要求 Index 计数归零才放行），副
                // 本内无存活 clang-c 对象；进程引擎此刻已锚回真
                // 实安装。副本恰被引擎加载一次，取句柄不增引用、
                // FreeLibrary 一次即解除映射。
                HMODULE copy_module = nullptr;
                cg::test::check(
                    r,
                    GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        fs::path{lib_copy.generic_string()}.wstring().c_str(),
                        &copy_module)
                            != 0
                        && copy_module != nullptr,
                    "the copy engine module stays mapped before unload");
                if (copy_module != nullptr)
                {
                    FreeLibrary(copy_module);
                }
                cg::test::check(
                    r,
                    GetModuleHandleW(
                        fs::path{lib_copy.generic_string()}.wstring().c_str())
                        == nullptr,
                    "the copy engine module is unloaded");
            }
            else
            {
                cg::test::check(
                    r,
                    cc::driver_info().driver == d1,
                    "a failed switch leaves the original cache intact");
                // 加载失败的副本从未驻留进程（加载入口在符号解
                // 析失败时已自行释放句柄），无需卸载，直接落入
                // 块尾的目录删除与块外断言。
            }
        }
        // 删除断言收窄为文件级：真正的回归信号是 84MB 引擎副本或
        // 驱动标记任一真实存活（泄漏级失败）。目录本身不再要求消
        // 失，删除挂起也不追——clang64×msys2 引擎组合下 OS 装载器
        // 解除副本映射后，句柄散尽没有可靠时限：目录与文件都可能
        // 长时间处于删除挂起（已标记删除、枚举仍可见、再打不开），
        // 纯等待等不来。故判定按"能否打开"逐条目探测：目录不存
        // 在 → 过；目录打不开（枚举抛错）→ 挂起，视同已删 → 过；
        // 条目打不开（ifstream 失败，delete-pending 的表现）→
        // 删除传播中，容忍；任何条目打得开 → 文件真实存活，红。
        // 轮询在外层吸收删除传播的亚秒级瞬态，全挂起/已删首轮即
        // 通过、不再空耗。
        int gone_retries = 0;
        while (gone_retries < 5 && !dir_files_gone(temp_base))
        {
            ++gone_retries;
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        cg::test::check(
            r,
            dir_files_gone(temp_base),
            std::format(
                "copy engine files leaked (leak-level failure): {} still "
                "has live entries after unload (delete-pending entries "
                "and empty/delete-pending dirs are tolerated; waited {} "
                "retries)",
                temp_base.string(),
                gone_retries));
        // 断言已判文件级干净，这里再尽力把空壳/挂起目录本身删
        // 掉：句柄此刻若已散尽，%TEMP% 当轮即全净；仍挂起则留给
        // 下一轮的启动清扫。成败静默，不影响上面的断言。
        {
            std::error_code rm_ec;
            fs::remove_all(temp_base, rm_ec);
        }
    }

    /// 目录内是否已无打得开的条目（目录消失/枚举失败/删除挂起均算过）。
    [[nodiscard]]
    auto dir_files_gone(const std::filesystem::path& dir) -> bool
    {
        namespace fs = std::filesystem;
        std::error_code exists_ec;
        if (!fs::exists(dir, exists_ec))
        {
            return true;
        }
        try
        {
            for (const fs::directory_entry& entry : fs::directory_iterator{dir})
            {
                std::ifstream probe{entry.path(), std::ios::binary};
                if (probe.is_open())
                {
                    return false;
                }
            }
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return true;
        }
        return true;
    }

    // PCH 加载器规范化：`-Xclang` 原子对、`-include-pch` /
    // `-include` 相对条目目录的路径解析、cl `/Yu` 裸
    // 守卫，以及原封不动的非 PCH 条目（零改动闸门）。
    void compile_db_pch_load(R& r)
    {
        namespace fs = std::filesystem;
        TempProj proj{"ccg-pch-load-"};
        proj.put(proj.base / "pch.h", "#pragma once\n#define PCH_TOKEN 7\n");
        proj.put(proj.base / "pch.cxx", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){return 0;}\n");
        const std::string db_text = std::format(
            "[{{\"directory\":{0},\"file\":{1},\"arguments\":[\"clang++\","
            "\"-I\",{0},\"-x\",\"c++-header\",\"-Xclang\",\"-emit-pch\","
            "\"-Xclang\",\"-include\",\"-Xclang\",\"pch.h\",\"-o\",\"pch.pch\","
            "\"-c\",{1}],\"output\":{2}}},"
            "{{\"directory\":{0},\"file\":{3},\"arguments\":[\"clang++\","
            "\"-include-pch\",\"pch.pch\",\"-include\",\"pch.h\",\"-c\",{3}]}},"
            "{{\"directory\":{0},\"file\":{4},\"arguments\":[\"clang-cl\","
            "\"--driver-mode=cl\",\"/Yu\",{4}]}},"
            "{{\"directory\":{0},\"file\":{3},\"arguments\":[\"clang++\","
            "\"-I\",\".\",\"-c\",{3}]}}]",
            q(proj.root),
            q(proj.root + "/pch.cxx"),
            q(proj.root + "/pch.pch"),
            q(proj.root + "/main.cpp"),
            q(proj.root + "/bare.cpp"));
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        cg::test::check(r, db.size() == 4, "PCH fixture loads four entries");
        if (db.size() == 4)
        {
            cg::test::check(
                r,
                db[0].pch_create && !db[0].pch_use,
                "GNU producer is classified from the record");
            cg::test::check(
                r,
                db[0].pch_output == proj.root + "/pch.pch",
                "producer -o path is recorded absolute");
            cg::test::check(
                r,
                db[1].pch_use && db[1].pch_path == proj.root + "/pch.pch",
                "consumer -include-pch resolves against directory");
            const bool fed_pch =
                std::ranges::find(db[1].argv, "-include-pch")
                    != db[1].argv.end()
                && std::ranges::find(db[1].argv, proj.root + "/pch.pch")
                       != db[1].argv.end();
            const bool forwarded_include =
                std::ranges::find(db[1].argv, "-include") != db[1].argv.end()
                && std::ranges::find(db[1].argv, proj.root + "/pch.h")
                       != db[1].argv.end();
            cg::test::check(
                r,
                fed_pch && forwarded_include,
                "-Xclang forwarding stays atomic and -include is "
                "rewritten to the forwarded absolute form");
            cg::test::check(
                r,
                db[2].pch_use && db[2].file == proj.root + "/bare.cpp",
                "bare /Yu does not swallow the source file");
            const bool yu_gone =
                std::ranges::find(db[2].argv, "/Yu") == db[2].argv.end();
            cg::test::check(r, yu_gone, "bare /Yu is stripped for replay");
            cg::test::check(
                r,
                !db[3].pch_involved && db[3].argv.size() == 2
                    && db[3].argv[0] == "-I"
                    && db[3].argv[1].starts_with(proj.root),
                "non-PCH entry keeps the legacy normalized argv");
        }
    }

    // 影子 PCH 端到端：重放记录的生产者构建 DPR 影子，
    // 消费者被重写、第二次计划复用它、改头文件
    // 使其失效。无驱动可用时跳过构建检查。
    void pch_shadow_build(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "shadow PCH build skipped: no clang driver");
            return;
        }
        TempProj        proj{"ccg-pch-build-"};
        std::error_code ec;
        proj.put(
            proj.base / "common.h",
            "#pragma once\n#define PCH_TOKEN 7\nstruct PchPass { int v; };\n");
        proj.put(proj.base / "pch.cxx", "#include \"common.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"common.h\"\nint main(){PchPass p{1};return p.v;}\n");
        const std::string db_text = std::format(
            "[{{\"directory\":{0},\"file\":{1},\"arguments\":[\"clang++\","
            "\"-I\",{0},\"-x\",\"c++-header\",\"-Xclang\",\"-emit-pch\","
            "\"-Xclang\",\"-include\",\"-Xclang\",\"common.h\",\"-o\","
            "\"common.pch\",\"-c\",{1}],\"output\":{2}}},"
            "{{\"directory\":{0},\"file\":{3},\"arguments\":[\"clang++\","
            "\"-I\",{0},\"-include-pch\",\"common.pch\",\"-include\","
            "\"common.h\",\"-c\",{3}]}}]",
            q(proj.root),
            q(proj.root + "/pch.cxx"),
            q(proj.root + "/common.pch"),
            q(proj.root + "/main.cpp"));
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        cg::test::check(r, db.size() == 2, "shadow fixture loads two entries");
        if (db.size() != 2)
        {
            return;
        }
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult first = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            first.built == 1 && first.consumers == 1,
            "recorded producer builds one shadow for one consumer");
        const cc::CompileCommand* consumer = nullptr;
        for (const cc::CompileCommand& c : first.commands)
        {
            if (c.file == proj.root + "/main.cpp")
            {
                consumer = &c;
            }
        }
        cg::test::check(
            r,
            consumer != nullptr
                && consumer->pch.status == cc::PchUse::Status::shadow
                && consumer->pch.identity != "text",
            "consumer is rewritten to consume the shadow");
        bool shadow_on_disk = false;
        if (consumer != nullptr)
        {
            const std::string shadow = [&]
            {
                for (std::size_t i = 0; i + 3 < consumer->argv.size(); ++i)
                {
                    if (consumer->argv[i] == "-Xclang"
                        && consumer->argv[i + 1] == "-include-pch")
                    {
                        return consumer->argv[i + 3];
                    }
                }
                return std::string{};
            }();
            shadow_on_disk = !shadow.empty() && fs::is_regular_file(shadow, ec);
            cg::test::check(
                r,
                shadow_on_disk,
                "shadow PCH exists at the injected absolute path");
        }
        const cc::PchPlanResult second = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            second.reused == 1 && second.built == 0,
            "second plan reuses the shadow (no rebuild)");
        proj.put(
            proj.base / "common.h",
            "#pragma once\n#define PCH_TOKEN 9\nstruct PchPass { int v; };\n");
        const cc::PchPlanResult third = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            third.built == 1 && third.reused == 0,
            "header edit invalidates the shadow and rebuilds");
        // 强制重建但字节相同：消费者身份必须不变（mtime 不参与身份），
        // 否则一次内容等价的重建会让所有消费者无谓重解析。
        std::string third_identity;
        for (const cc::CompileCommand& c : third.commands)
        {
            if (c.file == proj.root + "/main.cpp")
            {
                third_identity = c.pch.identity;
            }
        }
        opt.force = true;
        const cc::PchPlanResult forced = cc::plan_pch(db, db, opt);
        std::string             forced_identity;
        for (const cc::CompileCommand& c : forced.commands)
        {
            if (c.file == proj.root + "/main.cpp")
            {
                forced_identity = c.pch.identity;
            }
        }
        cg::test::check(
            r,
            forced.built == 1 && !third_identity.empty()
                && forced_identity == third_identity,
            "content-identical forced rebuild keeps the consumer "
            "identity (mtime is not identity)");
    }

    /// compile_commands 条目的 JSON 文本（cl.exe 直调形态）。
    [[nodiscard]]
    auto cl_cdb_item(const std::string& root, const std::string& file)
        -> std::string
    {
        return "{\"directory\":" + q(root) + ",\"file\":" + q(file)
               + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(root)
               + ",\"/EHsc\",\"/std:c++20\"," + q(file) + "]}";
    }

    /// PCH 计划里指定文件消费的影子路径（未找到给空串）。
    [[nodiscard]]
    auto shadow_path_of(const std::string& root, const cc::PchPlanResult& plan)
        -> std::string
    {
        std::string path;
        for (const cc::CompileCommand& c : plan.commands)
        {
            if (c.file != root + "/main.cpp")
            {
                continue;
            }
            for (std::size_t i = 0; i + 3 < c.argv.size(); ++i)
            {
                if (c.argv[i] == "-Xclang" && c.argv[i + 1] == "-include-pch")
                {
                    path = c.argv[i + 3];
                }
            }
        }
        return path;
    }

    /// PCH 计划里指定文件的消费者身份（缺席给空串）。
    [[nodiscard]]
    auto pch_consumer_identity(
        const std::string&       file,
        const cc::PchPlanResult& plan) -> std::string
    {
        for (const cc::CompileCommand& c : plan.commands)
        {
            if (c.file == file)
            {
                return c.pch.identity;
            }
        }
        return std::string{};
    }

    // 只经 CPATH 可达的头文件必须进入闭包：搜索路径来自环境而非构建记录。
    // 同样的“同尺寸 + 恢复 mtime”编辑必须让闭包哈希改变、影子重建，消费
    // 者身份随之变化——否则图会静默保留旧字段。
    void pch_closure_env_search(R& r)
    {
        namespace fs = std::filesystem;
        const cc::DriverInfo& dinfo = cc::driver_info();
        if (dinfo.driver.empty())
        {
            cg::test::check(
                r,
                true,
                "CPATH closure test skipped: no clang driver");
            return;
        }
        TempProj          proj{"ccg-pch-cpath-"};
        std::error_code   ec;
        const std::string envdir = proj.root + "/envinc";
        proj.put(
            proj.base / "envinc" / "envdep.h",
            "#pragma once\nstruct EnvPass { int e; };\n");
        proj.put(proj.base / "pch.h", "#pragma once\n#include <envdep.h>\n");
        proj.put(proj.base / "pch.cxx", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){EnvPass p{1};return p.e;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cxx")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-x\",\"c++-header\",\"-Xclang\",\"-emit-pch\","
              "\"-Xclang\",\"-include\",\"-Xclang\",\"pch.h\",\"-o\","
              "\"common.pch\",\"-c\","
            + q(proj.root + "/pch.cxx") + "],\"output\":"
            + q(proj.root + "/common.pch") + "},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-include-pch\",\"common.pch\",\"-include\",\"pch.h\","
              "\"-c\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        if (db.size() != 2)
        {
            cg::test::check(
                r,
                false,
                "CPATH closure fixture loads two entries");
            return;
        }
        const cg::test::EnvScope cpath_guard{"CPATH"};
        cpath_guard.set(envdir);
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult first = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            first.built == 1 && first.reused == 0,
            "CPATH-only include resolves into the closure and "
            "builds a shadow");
        const cc::PchPlanResult second = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            second.reused == 1 && second.built == 0,
            "CPATH-only closure is stable for an unchanged rerun");
        const fs::path dep = proj.base / "envinc" / "envdep.h";
        const auto     dep_mtime = fs::last_write_time(dep, ec);
        std::string    dep_text = cg::util::read_text_file(dep.generic_string())
                                      .value_or(std::string{});
        const std::size_t field = dep_text.find("int e;");
        if (field != std::string::npos)
        {
            dep_text.replace(field, 6, "int q;");
        }
        {
            std::ofstream out{dep, std::ios::binary | std::ios::trunc};
            out << dep_text;
        }
        fs::last_write_time(dep, dep_mtime, ec);
        const cc::PchPlanResult third = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            third.built == 1 && third.reused == 0
                && pch_consumer_identity(proj.root + "/main.cpp", third)
                       != pch_consumer_identity(proj.root + "/main.cpp", second)
                && !pch_consumer_identity(proj.root + "/main.cpp", third)
                        .empty(),
            "same-size mtime-restored CPATH header edit rebuilds the "
            "shadow and changes the consumer identity");
    }

    // 隐式 PCH：`-include X` 且 X 旁有 X.pch/.gch → 条目必须视为
    // PCH-involved，并把 -include 改写为 `-Xclang -include` 转发形式
    // （否则 libclang 自动加载非 DPR PCH → rc=4）。没有 producer 记录时
    // 规划器文本回退，且原因在逐 TU note 中可见。
    void pch_implicit_include(R& r)
    {
        namespace fs = std::filesystem;
        TempProj proj{"ccg-pch-imp-"};
        proj.put(
            proj.base / "common.h",
            "#pragma once\nstruct Imp { int v; };\n");
        // 内容无所谓：探测只看兄弟文件存在，规划器不消费它。
        proj.put(proj.base / "common.h.pch", "stale non-dpr pch\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"common.h\"\nint main(){Imp i{1};return i.v;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root) + ",\"file\":"
            + q(proj.root + "/main.cpp") + ",\"arguments\":[\"clang++\",\"-I\","
            + q(proj.root) + ",\"-include\"," + q(proj.root + "/common.h")
            + ",\"-c\"," + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        cg::test::check(r, db.size() == 1, "implicit PCH fixture loads");
        if (db.size() == 1)
        {
            cg::test::check(
                r,
                db[0].pch_involved && !db[0].pch_use && !db[0].pch_create,
                "sibling .pch beside -include marks the entry "
                "PCH-involved");
            bool forwarded = false;
            for (std::size_t i = 0; i + 3 < db[0].argv.size(); ++i)
            {
                if (db[0].argv[i] == "-Xclang"
                    && db[0].argv[i + 1] == "-include"
                    && db[0].argv[i + 2] == "-Xclang"
                    && db[0].argv[i + 3] == proj.root + "/common.h")
                {
                    forwarded = true;
                }
            }
            cg::test::check(
                r,
                forwarded,
                "-include is rewritten to the atomic forwarding "
                "form (bypasses implicit PCH load)");
        }
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            plan.text_fallback == 1 && plan.built == 0 && plan.reused == 0,
            "implicit PCH without a recorded producer is text "
            "fallback");
        cg::test::check(
            r,
            !plan.tu_notes.empty()
                && plan.tu_notes.front().find("implicit PCH")
                       != std::string::npos,
            "implicit PCH fallback reason is visible per TU");
        for (const cc::CompileCommand& c : plan.commands)
        {
            if (c.file == proj.root + "/main.cpp")
            {
                cg::test::check(
                    r,
                    c.pch.status == cc::PchUse::Status::text_fallback
                        && c.pch.note.find("implicit PCH") != std::string::npos,
                    "implicit PCH consumer is classified "
                    "text_fallback with a note");
                // 实解析：改写后的 argv 必须 0 诊断且图含伞头里的符号
                // （非 DPR 兄弟 .pch 绝不能被隐式加载）。
                const cc::Index    index;
                const cc::ParsedTu parsed = cc::parse(index, c.file, c.argv);
                bool               umbrella_symbol = false;
                cc::children(
                    parsed.tu.cursor(),
                    [&](const CXCursor& cur)
                    {
                        if (cc::kind(cur) == CXCursor_StructDecl
                            && cc::spelling(cur) == "Imp")
                        {
                            umbrella_symbol = true;
                        }
                        return true;
                    });
                cg::test::check(
                    r,
                    parsed.errors.empty() && umbrella_symbol,
                    "implicit-PCH bypass parses cleanly and keeps "
                    "the umbrella header's symbols");
            }
        }
    }

    // sidecar producer 的 `/FI` 强制包含必须进入闭包：修改被强制包含的
    // 头必须使影子失效重建（否则陈旧影子被无限期复用）。
    void pch_sidecar_fi_closure(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "sidecar /FI closure skipped: no driver");
            return;
        }
        TempProj proj{"ccg-pch-fi-"};
        proj.put(
            proj.base / "pch.h",
            "#pragma once\nstruct PchThing { int v; };\n");
        proj.put(proj.base / "forced.h", "#pragma once\n#define FORCED_A 1\n");
        proj.put(proj.base / "pch.cpp", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){PchThing t{FORCED_A};return t.v;}\n");
        const std::string cdb_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cpp")
            + ",\"arguments\":[\"clang-cl\",\"--driver-mode=cl\",\"/c\","
              "\"/I\","
            + q(proj.root) + ",\"/FI\"," + q(proj.root + "/forced.h") + ","
            + q(proj.root + "/pch.cpp") + "]},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"clang-cl\",\"--driver-mode=cl\",\"/c\","
              "\"/I\","
            + q(proj.root) + ",\"/FI\"," + q(proj.root + "/forced.h") + ","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", cdb_text);
        const std::string cdb_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const std::string digest = cg::util::sha256_hex(
            std::as_bytes(
                std::span<const char>{cdb_text.data(), cdb_text.size()}));
        const std::string side_text =
            "{\"version\":1,\"cdb_sha256\":\"" + digest
            + "\",\"producers\":[{\"pch_key\":" + q(proj.root + "/app.pch")
            + ",\"source\":" + q(proj.root + "/pch.cpp")
            + ",\"umbrella\":\"pch.h\",\"umbrella_path\":\"\",\"cwd\":"
            + q(proj.root)
            + ",\"argv\":[\"clang-cl\",\"--driver-mode=cl\",\"/c\","
              "\"/I\","
            + q(proj.root) + ",\"/FI\"," + q(proj.root + "/forced.h")
            + ",\"/Ycpch.h\",\"/Fp" + proj.root + "/app.pch\","
            + q(proj.root + "/pch.cpp")
            + "],\"command_digest\":\"x\"}],\"consumers\":[{\"file\":"
            + q(proj.root + "/main.cpp")
            + ",\"pch_key\":" + q(proj.root + "/app.pch")
            + ",\"pch_header\":\"pch.h\",\"command_digest\":\"y\"}]}";
        proj.put(proj.base / "pch.json", side_text);
        const auto     db = cc::load(cdb_path, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = cdb_path;
        opt.sidecar_path =
            cg::util::norm_path((proj.base / "pch.json").generic_string());
        const cc::PchPlanResult first = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            first.built == 1,
            "sidecar producer with /FI builds one shadow");
        const cc::PchPlanResult second = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            second.reused == 1 && second.built == 0,
            "unchanged /FI closure reuses the shadow");
        proj.put(
            proj.base / "forced.h",
            "#pragma once\n#define FORCED_A 2\n#define FORCED_B 1\n");
        const cc::PchPlanResult third = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            third.built == 1 && third.reused == 0,
            "editing the /FI forced header invalidates the shadow");
    }

    // MSBuild 旁车 argv 是 /Yc 命令的逐字记录，宏值可能带原始空白
    // （"/DNTDDI_VERSION= 0x06030000"，等号后手滑空格）。生产者命令行
    // 必须过与 cdb 载入相同的宏值归一化：否则 producer_digest 按原始
    // 字节计算，同一命令的带空格/不带空格记录分裂成两个槽位，影子
    // 烙进的宏值与消费者命令行（cdb 已修剪）按字节比对即拒载。两种
    // 记录规划出的 producer_digest 与槽位必须完全一致。
    /// PCH 旁车 JSON 全文（单生产者/单消费者，argv 里嵌给定宏 token）。
    [[nodiscard]]
    auto pch_sidecar_text(
        const std::string& root,
        const std::string& digest,
        const std::string& macro_token) -> std::string
    {
        return "{\"version\":1,\"cdb_sha256\":\"" + digest
               + "\",\"producers\":[{\"pch_key\":" + q(root + "/app.pch")
               + ",\"source\":" + q(root + "/pch.cpp")
               + ",\"umbrella\":\"pch.h\",\"umbrella_path\":\"\",\"cwd\":"
               + q(root)
               + ",\"argv\":[\"clang-cl\",\"--driver-mode=cl\",\"/c\","
                 "\"/I\","
               + q(root) + ",\"" + macro_token + "\",\"/Ycpch.h\",\"/Fp" + root
               + "/app.pch\"," + q(root + "/pch.cpp")
               + "],\"command_digest\":\"x\"}],\"consumers\":[{\"file\":"
               + q(root + "/main.cpp") + ",\"pch_key\":" + q(root + "/app.pch")
               + ",\"pch_header\":\"pch.h\",\"command_digest\":\"y\"}]}";
    }

    void pch_sidecar_macro_space_digest(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "sidecar macro-space skipped: no driver");
            return;
        }
        TempProj proj{"ccg-pch-macrosp-"};
        proj.put(
            proj.base / "pch.h",
            "#pragma once\nstruct PchThing { int v; };\n");
        proj.put(proj.base / "pch.cpp", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){PchThing t{1};return t.v;}\n");
        const std::string cdb_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cpp")
            + ",\"arguments\":[\"clang-cl\",\"--driver-mode=cl\",\"/c\","
              "\"/I\","
            + q(proj.root) + ",\"/DNTDDI_VERSION=0x06030000\","
            + q(proj.root + "/pch.cpp") + "]},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"clang-cl\",\"--driver-mode=cl\",\"/c\","
              "\"/I\","
            + q(proj.root) + ",\"/DNTDDI_VERSION=0x06030000\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", cdb_text);
        const std::string cdb_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const std::string digest = cg::util::sha256_hex(
            std::as_bytes(
                std::span<const char>{cdb_text.data(), cdb_text.size()}));
        // 旁车与 cdb 记录的是同一条命令：cdb 条目是已修剪形态，旁车
        // argv 逐字透传宏 token（实害形态 = 等号后带空格）。
        proj.put(
            proj.base / "pch.json",
            pch_sidecar_text(proj.root, digest, "/DNTDDI_VERSION= 0x06030000"));
        const auto     db = cc::load(cdb_path, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = cdb_path;
        opt.sidecar_path =
            cg::util::norm_path((proj.base / "pch.json").generic_string());
        const cc::PchPlanResult first = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            first.built == 1,
            "sidecar producer with a spaced macro value builds one shadow");
        const fs::path manifest_path =
            fs::path{first.cache_dir} / "manifest.json";
        const auto spaced = read_pch_manifest(manifest_path);
        cg::test::check(
            r,
            spaced.has_value() && spaced->entries.size() == 1,
            "the spaced-macro sidecar leaves exactly one manifest entry");
        // 同一命令换成不带空格的宏记录：两次记录仅宏值空白之差，规划
        // 出的 producer_digest 与槽位必须一致，第二次直接复用第一次的
        // 影子（归一化缺失时 digest 分裂成两个槽位、影子必然重建）。
        proj.put(
            proj.base / "pch.json",
            pch_sidecar_text(proj.root, digest, "/DNTDDI_VERSION=0x06030000"));
        const cc::PchPlanResult second = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            second.reused == 1 && second.built == 0,
            "the trimmed-macro sidecar reuses the spaced-macro shadow");
        const auto trimmed = read_pch_manifest(manifest_path);
        cg::test::check(
            r,
            trimmed.has_value() && trimmed->entries.size() == 1
                && spaced.has_value()
                && spaced->entries[0].producer_digest
                       == trimmed->entries[0].producer_digest
                && spaced->entries[0].slot_key == trimmed->entries[0].slot_key,
            "spaced and trimmed macro spellings share one producer_digest "
            "and slot");
    }

    // 闭包不可解析：宏形态 `#include INC_HEADER` 隐藏了真实依赖。该
    // producer 每轮强制重建（绝不复用），note 如实可见；影子解析与文本
    // 基线的 AST（主文件声明集）一致。
    /// 解析 argv 后收集主文件声明集（排除预处理游标；有序返回）。
    [[nodiscard]]
    auto main_file_decls(
        const cc::Index&                index,
        const std::string&              file,
        const std::vector<std::string>& argv) -> std::vector<std::string>
    {
        const cc::ParsedTu       parsed = cc::parse(index, file, argv);
        std::vector<std::string> out;
        cc::children(
            parsed.tu.cursor(),
            [&](const CXCursor& c)
            {
                const CXCursorKind k = cc::kind(c);
                const bool         preprocessing =
                    k == CXCursor_InclusionDirective
                    || k == CXCursor_MacroDefinition
                    || k == CXCursor_MacroExpansion
                    || k == CXCursor_PreprocessingDirective;
                if (cc::in_main_file(parsed.tu, c) && !preprocessing)
                {
                    const std::string spelling = cc::spelling(c);
                    if (!spelling.empty())
                    {
                        out.push_back(
                            std::to_string(static_cast<int>(k)) + ":"
                            + spelling);
                    }
                }
                return true;
            });
        std::ranges::sort(out);
        return out;
    }

    void pch_macro_include_closure(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "macro closure skipped: no driver");
            return;
        }
        TempProj proj{"ccg-pch-macro-"};
        proj.put(
            proj.base / "common.h",
            "#pragma once\nstruct MacroPass { int v; };\n");
        proj.put(
            proj.base / "pch.cxx",
            "#define INC_HEADER \"common.h\"\n#include INC_HEADER\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"common.h\"\nint main(){MacroPass p{1};return p.v;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cxx")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-x\",\"c++-header\",\"-Xclang\",\"-emit-pch\","
              "\"-Xclang\",\"-include\",\"-Xclang\",\"common.h\",\"-o\","
              "\"common.pch\",\"-c\","
            + q(proj.root + "/pch.cxx") + "],\"output\":"
            + q(proj.root + "/common.pch") + "},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-include-pch\",\"common.pch\",\"-include\",\"common.h\","
              "\"-c\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto     db = cc::load(db_path, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult first = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            first.built == 1 && first.reused == 0,
            "macro-#include producer builds a shadow first time");
        const cc::PchPlanResult second = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            second.reused == 1 && second.built == 0,
            "macro-#include closure comes from the build's own "
            "preprocessing; an unchanged rerun reuses the shadow");
        proj.put(
            proj.base / "common.h",
            "#pragma once\nstruct MacroPass { int v; };\n"
            "struct MacroExtra { int w; };\n");
        const cc::PchPlanResult third = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            third.built == 1,
            "editing a header reached through a macro forces a "
            "rebuild");
        // 图等价：影子解析与文本回退解析的主文件声明集必须一致。
        const cc::Index index;
        for (const cc::CompileCommand& c : third.commands)
        {
            if (c.file != proj.root + "/main.cpp")
            {
                continue;
            }
            const std::vector<std::string> shadow_decls =
                main_file_decls(index, proj.root + "/main.cpp", c.argv);
            const std::vector<std::string> text_decls = main_file_decls(
                index,
                proj.root + "/main.cpp",
                c.pch_fallback_argv);
            cg::test::check(
                r,
                c.pch.status == cc::PchUse::Status::shadow
                    && shadow_decls == text_decls && !shadow_decls.empty(),
                "shadow AST equals the text baseline for the "
                "macro-include producer");
        }
    }

    // 重放 cwd：记录的 directory 必须传给子进程（lpCurrentDirectory），
    // 相对 -imacros 才能解析；进程级 chdir 会与线程池竞态。
    void pch_replay_cwd(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "replay cwd skipped: no driver");
            return;
        }
        TempProj          proj{"ccg-pch-cwd-"};
        std::error_code   ec;
        const std::string proj_dir =
            cg::util::norm_path((proj.base / "proj").generic_string());
        const std::string dir = cg::util::norm_path(
            (proj.base / "proj" / "build").generic_string());
        proj.put(
            proj.base / "proj" / "forced_macros.h",
            "#pragma once\n#define FORCED_FROM_MACROS 1\n");
        proj.put(
            proj.base / "proj" / "pch.h",
            "#pragma once\nstruct CwdPass { int v; };\n");
        // 无 -imacros 时该 TU 直接报错：影子能否落盘就是 cwd 是否正确的
        // 判据。
        proj.put(
            proj.base / "proj" / "pch.cxx",
            "#if !defined(FORCED_FROM_MACROS)\n"
            "#error forced macros not visible\n"
            "#endif\n");
        proj.put(
            proj.base / "proj" / "main.cpp",
            "#include \"pch.h\"\nint main(){CwdPass p;return p.v;}\n");
        // 记录目录是 proj/build；相对 -imacros ../forced_macros.h 只有
        // 在子进程 cwd=记录目录时才能解析。
        const std::string db_text =
            "[{\"directory\":" + q(dir)
            + ",\"file\":" + q(proj_dir + "/pch.cxx")
            + ",\"arguments\":[\"clang++\",\"-I\",\"..\",\"-imacros\","
              "\"../forced_macros.h\",\"-x\",\"c++-header\","
              "\"-Xclang\",\"-emit-pch\",\"-Xclang\",\"-include\","
              "\"-Xclang\",\"../pch.h\",\"-o\",\"pch.pch\",\"-c\","
              "\"../pch.cxx\"],\"output\":"
            + q(dir + "/pch.pch") + "},{\"directory\":" + q(dir)
            + ",\"file\":" + q(proj_dir + "/main.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\",\"..\",\"-include-pch\","
              "\"pch.pch\",\"-include\",\"../pch.h\",\"-c\","
              "\"../main.cpp\"]}]";
        proj.put(
            proj.base / "proj" / "build" / "compile_commands.json",
            db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "proj" / "build" / "compile_commands.json")
                .generic_string());
        const auto     db = cc::load(db_path, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = cg::util::norm_path(
            (proj.base / ".ccg" / "Debug").generic_string());
        opt.compile_db_path = db_path;
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            plan.built == 1 && plan.text_fallback == 0,
            "relative -imacros resolves from the recorded "
            "directory (child cwd), not the ccg process cwd");
    }

    // 跨配置 donor：同 engine_key 的第二个配置从兄弟配置复制影子，不重编。
    void pch_cross_config_donor(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "cross-config donor skipped: no driver");
            return;
        }
        TempProj        proj{"ccg-pch-donor-"};
        std::error_code ec;
        proj.put(
            proj.base / "common.h",
            "#pragma once\nstruct DonorPass { int v; };\n");
        proj.put(proj.base / "pch.cxx", "#include \"common.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"common.h\"\nint main(){DonorPass p{1};return p.v;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cxx")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-x\",\"c++-header\",\"-Xclang\",\"-emit-pch\","
              "\"-Xclang\",\"-include\",\"-Xclang\",\"common.h\",\"-o\","
              "\"common.pch\",\"-c\","
            + q(proj.root + "/pch.cxx") + "],\"output\":"
            + q(proj.root + "/common.pch") + "},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-include-pch\",\"common.pch\",\"-include\",\"common.h\","
              "\"-c\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto     db = cc::load(db_path, {});
        cc::PchOptions opt;
        opt.compile_db_path = db_path;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        const cc::PchPlanResult first = cc::plan_pch(db, db, opt);
        cg::test::check(r, first.built == 1, "first config builds the shadow");
        opt.ccg_config_dir = proj.root + "/.ccg/Release";
        const cc::PchPlanResult second = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            second.built == 0 && second.reused == 1 && second.donated == 1,
            "second config copies the shadow from the sibling "
            "config (0 built / 1 donor)");
        const std::string first_shadow = shadow_path_of(proj.root, first);
        const std::string second_shadow = shadow_path_of(proj.root, second);
        cg::test::check(
            r,
            !first_shadow.empty() && fs::is_regular_file(second_shadow, ec)
                && cg::clang::detail::hash_source(first_shadow)
                       == cg::clang::detail::hash_source(second_shadow),
            "donated shadow exists and is byte-identical to the "
            "source config's shadow");
        // 台账记录 donor 来源。
        std::string     donor_field;
        std::error_code mec;
        for (fs::recursive_directory_iterator it{proj.base / ".ccg", mec}, end;
             !mec && it != end;
             it.increment(mec))
        {
            if (it->is_regular_file(mec)
                && it->path().filename() == "manifest.json")
            {
                const std::string text =
                    cg::util::read_text_file(it->path().generic_string())
                        .value_or(std::string{});
                const std::size_t key = text.find("\"donor\"");
                if (key == std::string::npos)
                {
                    continue;
                }
                const std::size_t colon = text.find(':', key);
                const std::size_t open = colon == std::string::npos
                                             ? colon
                                             : text.find('"', colon + 1);
                const std::size_t close =
                    open == std::string::npos ? open : text.find('"', open + 1);
                if (open != std::string::npos && close != std::string::npos
                    && close > open + 1)
                {
                    donor_field = text;
                }
            }
        }
        cg::test::check(
            r,
            !donor_field.empty(),
            "manifest records the donor shadow path");
    }

    // 外来 raw cl cdb 的 `/Yc` producer：loader 丢弃 `/c`，重放必须补回，
    // 否则 clang-cl 走编译+链接、影子永不落盘。
    void pch_raw_cl_producer(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "raw cl producer skipped: no driver");
            return;
        }
        TempProj        proj{"ccg-pch-cl-"};
        std::error_code ec;
        proj.put(
            proj.base / "pch.h",
            "#pragma once\nstruct ClPass { int v; };\n");
        proj.put(proj.base / "pch.cpp", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){ClPass p{1};return p.v;}\n");
        // 外来 raw MSVC cdb：argv[0]=cl.exe，没有 --driver-mode=cl，只靠
        // /Yc //Yu //Fp 推断方言。输出类旗标 /P /Fi /FA /Fa 必须被剥离，
        // 否则影子构建只做预处理/写构建树。
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(proj.root)
            + ",\"/Ycpch.h\",\"/Fp" + proj.root
            + "/app.pch\",\"/P\","
              "\"/Fi"
            + proj.root
            + "/preprocessed-pollution.i\",\"/FA\","
              "\"/Fa"
            + proj.root + "/listing-pollution.asm\","
            + q(proj.root + "/pch.cpp") + "]},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(proj.root)
            + ",\"/Yupch.h\",\"/Fp" + proj.root + "/app.pch\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        cg::test::check(
            r,
            db.size() == 2 && db[0].pch_create && db[0].cl_mode,
            "raw MSVC cdb is classified as cl without "
            "--driver-mode=cl");
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            plan.built == 1 && plan.text_fallback == 0,
            "raw cl /Yc producer replays with /c and builds the "
            "shadow");
        for (const cc::CompileCommand& c : plan.commands)
        {
            if (c.file == proj.root + "/main.cpp")
            {
                cg::test::check(
                    r,
                    c.pch.status == cc::PchUse::Status::shadow,
                    "raw cl consumer consumes the shadow");
            }
        }
        cg::test::check(
            r,
            !fs::exists(proj.base / "preprocessed-pollution.i", ec)
                && !fs::exists(proj.base / "listing-pollution.asm", ec),
            "cl output flags (/P /Fi /FA /Fa) are stripped from "
            "replay");
    }

    // 外场语料之解析旗标：非平凡类型传 varargs 的 PCH 头（ATL/日志宏
    // 形态）在 MSVC 下完全合法，clang 却把 -Wnon-pod-varargs 定为
    // DefaultError——与 /WX 无关，影子构建直接失败，消费者文本回退解析同
    // 一个头再失败（pch-unusable 双死）。索引语境只降级错误升级
    // （-Wno-error=non-pod-varargs 注入 parse 与 replay 两路），producer
    // 影子必须正常落盘、消费者吃影子。
    void pch_nonpod_varargs_producer(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "non-pod varargs producer skipped: no driver");
            return;
        }
        TempProj proj{"ccg-pch-nopod-"};
        // 取证夹具形态：非平凡析构的类型经 variadic
        // 日志宏上抛；__FUNCTIONW__ 兜底宏只为聚焦 non-pod 语义（clang-cl
        // 不识别该 MSVC 宏）。
        proj.put(
            proj.base / "pch.h",
            "#pragma once\n"
            "#ifndef __FUNCTIONW__\n"
            "#define __FUNCTIONW__ L\"fallback_fn\"\n"
            "#endif\n"
            "struct SimWideString {\n"
            "    wchar_t* p;\n"
            "    SimWideString();\n"
            "    ~SimWideString();\n"
            "};\n"
            "void app_log_info(const wchar_t* fmt, ...);\n"
            "#define APP_LOG_INFO(...) app_log_info(__VA_ARGS__)\n"
            "inline void log_uplink() {\n"
            "    SimWideString str;\n"
            "    APP_LOG_INFO(L\"[%s]report\", __FUNCTIONW__, str);\n"
            "}\n");
        proj.put(proj.base / "pch.cpp", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){return 0;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(proj.root)
            + ",\"/EHsc\",\"/std:c++17\",\"/Fp" + proj.root + "/app.pch\","
            + "\"/Ycpch.h\"," + q(proj.root + "/pch.cpp") + "]},{\"directory\":"
            + q(proj.root) + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(proj.root)
            + ",\"/EHsc\",\"/std:c++17\",\"/Fp" + proj.root + "/app.pch\","
            + "\"/Yupch.h\"," + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        cg::test::check(
            r,
            db.size() == 2 && db[0].pch_create && db[0].cl_mode,
            "non-pod fixture is classified as a raw cl producer");
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            plan.built == 1 && plan.text_fallback == 0 && plan.unusable == 0,
            "non-pod varargs producer builds the shadow (no text "
            "fallback / unusable)");
        for (const cc::CompileCommand& c : plan.commands)
        {
            if (c.file == proj.root + "/main.cpp")
            {
                cg::test::check(
                    r,
                    c.pch.status == cc::PchUse::Status::shadow,
                    "non-pod consumer consumes the shadow");
            }
        }
    }

    // /WX 的 cl 方言 TU：libclang 解析入口固定追加的 -fno-spell-checking 在
    // clang-cl 下发 unknown-argument 驱动警告，/WX 把它提升成错误；逐警告
    // 压制是打地鼠（压掉 unknown-argument 后下一层 argument unused '/c'
    // 还会冒）。cl 方言解析路径末尾追加 /WX-（后写胜出）一次根治：合法
    // TU 解析零 Error 级诊断（等价索引输出的 failed: 0）。夹具自包含、不
    // include 任何头，不依赖本机 MSVC STL。
    void parse_wx_cl_dialect_downgraded(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "/WX cl parse test skipped: no driver");
            return;
        }
        TempProj proj{"ccg-wx-cl-"};
        proj.put(proj.base / "wxfn.cpp", "int wxfn() { return 42; }\n");
        // 外来 raw MSVC cdb（/WX 保留在主解析 argv 上，loader 补
        // --driver-mode=cl）——外场语料中 18 个 TU parse-failed 的最小复现形态。
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/wxfn.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(proj.root)
            + ",\"/EHsc\",\"/std:c++17\",\"/WX\"," + q(proj.root + "/wxfn.cpp")
            + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto                db = cc::load(db_path, {});
        const cc::CompileCommand* entry =
            cc::find_command(db, cg::util::norm_path(proj.root + "/wxfn.cpp"));
        // cl_mode 是 PCH 分支专属信号（非 PCH 条目恒 false，见
        // CompileCommand::cl_mode 的字段文档）；非 PCH 的 raw cl 条目其
        // 方言证据是 loader 补插进 argv 的 --driver-mode=cl token。
        bool cl_token = false;
        if (entry != nullptr)
        {
            for (const std::string& a : entry->argv)
            {
                if (a == "--driver-mode=cl")
                {
                    cl_token = true;
                    break;
                }
            }
        }
        cg::test::check(r, cl_token, "/WX fixture loads as a cl-dialect entry");
        if (entry == nullptr)
        {
            return;
        }
        const cc::Index index;
        cg::test::check(
            r,
            index.valid(),
            "index initializes for the /WX parse");
        const cc::ParsedTu parsed = cc::parse(index, entry->file, entry->argv);
        cg::test::check(
            r,
            parsed.tu.valid() && parsed.errors.empty(),
            "cl-dialect /WX TU parses with zero error diagnostics "
            "(/WX- downgrade; the 'failed: 0' contract)");
    }

    // 注入方言门禁：GNU（非 cl）、无 PCH 的工程，经 plan 后 argv 逐字节不
    // 变，cl 专属的 /WX-、--driver-mode=cl 与 -fdelayed-template-parsing
    // 对（含其告警静默）绝不混入；parse 注入的
    // -Wno-error=non-pod-varargs 双方言安全，GNU 解析零 Error 级诊断。
    void gnu_nopch_argv_injection_gate(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "GNU no-PCH gate skipped: no driver");
            return;
        }
        TempProj proj{"ccg-gnu-gate-"};
        proj.put(proj.base / "main.cpp", "int main(){return 0;}\n");
        const std::string db_text = "[{\"directory\":" + q(proj.root)
                                    + ",\"file\":" + q(proj.root + "/main.cpp")
                                    + ",\"arguments\":[\"clang++\",\"-I\","
                                    + q(proj.root) + ",\"-std=c++20\",\"-c\","
                                    + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto                db = cc::load(db_path, {});
        const cc::CompileCommand* entry =
            cc::find_command(db, cg::util::norm_path(proj.root + "/main.cpp"));
        cg::test::check(
            r,
            entry != nullptr && !entry->cl_mode,
            "GNU fixture loads as a non-cl entry");
        if (entry == nullptr)
        {
            return;
        }
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        bool                    argv_unchanged =
            plan.commands.size() == 1 && plan.commands[0].argv == entry->argv;
        cg::test::check(
            r,
            argv_unchanged,
            "GNU no-PCH entry keeps its argv byte-for-byte through "
            "plan_pch");
        bool leaked_cl_flag = false;
        for (const std::string& a : entry->argv)
        {
            leaked_cl_flag = leaked_cl_flag || a == "/WX-"
                             || a == "--driver-mode=cl"
                             || a == "-fdelayed-template-parsing"
                             || a == "-Wno-delayed-template-parsing-in-cxx20";
        }
        cg::test::check(
            r,
            !leaked_cl_flag,
            "no cl-dialect-only flag (/WX-, --driver-mode=cl, "
            "-fdelayed-template-parsing pair) leaks into a GNU "
            "entry");
        const cc::Index index;
        cg::test::check(
            r,
            index.valid(),
            "index initializes for the GNU parse");
        const cc::ParsedTu parsed = cc::parse(index, entry->file, entry->argv);
        cg::test::check(
            r,
            parsed.tu.valid() && parsed.errors.empty(),
            "GNU TU parses with zero error diagnostics (the "
            "-Wno-error=non-pod-varargs downgrade is "
            "dialect-safe)");
    }

    // cl 方言 permissive 语义（MSVC 从不语义检查未实例化的模板体，另有
    // 宽松特化规则）的方言形状：parse 对 cl 条目注入
    // -fdelayed-template-parsing 后，死模板体内三形状（无对象成员调用、
    // 构造实参不符、函数模板体 constexpr 非常量初始化——下游取证的
    // dead-template 夹具形态）零 Error 解析；延迟解析射程外的两形状（类
    // 模板 static constexpr 成员、省略 template<> 的类模板成员定义）仍
    // 失败且首错拼写原样保留，交给失败分类器记 msvc-permissive-* 具名
    // 类；GNU 方言同一拼写不记（方言门控），非方言拼写保留原始首错。
    void cl_dialect_dtp_dead_template_shapes(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "cl dialect dtp shapes skipped: no driver");
            return;
        }
        TempProj proj{"ccg-cl-dtp-"};
        // 可救三形状（-fdelayed-template-parsing 实测转绿）。
        proj.put(
            proj.base / "x323_tpl.cpp",
            "struct Engine { void Refresh(); };\n"
            "template <typename T> int DeadTpl() { Engine::Refresh(); "
            "return 0; }\n"
            "int main() { return 0; }\n");
        proj.put(
            proj.base / "x135_tpl.cpp",
            "struct Mutex {};\n"
            "struct Str {};\n"
            "template <class M> struct Guard {\n"
            "    explicit Guard(M&);\n"
            "    Guard(const Guard&) = delete;\n"
            "};\n"
            "template <typename T> int DeadLock(Str& s) { Guard<Mutex> "
            "lk(s); return 0; }\n"
            "int main() { return 0; }\n");
        proj.put(
            proj.base / "x32_fntpl.cpp",
            "int runtime_val = 5;\n"
            "template <typename T> int DeadFn() {\n"
            "    constexpr int k = runtime_val;\n"
            "    return k;\n"
            "}\n"
            "int main() { return 0; }\n");
        // 延迟解析射程外的两形状（仍失败，拼写原样保留供分类器记名）。
        proj.put(
            proj.base / "x105_memberdef.cpp",
            "template <typename T> struct Box { void Seal(); };\n"
            "void Box<int>::Seal() {}\n"
            "int main() { return 0; }\n");
        proj.put(
            proj.base / "x32_clsmember.cpp",
            "int runtime_val = 5;\n"
            "template <typename T> struct Holder {\n"
            "    static constexpr int k = runtime_val;\n"
            "};\n"
            "int main() { return 0; }\n");
        proj.put(proj.base / "gnu_main.cpp", "int main(){return 0;}\n");
        const std::string gnu_item =
            "{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/gnu_main.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-std=c++20\",\"-c\"," + q(proj.root + "/gnu_main.cpp") + "]}";
        const std::string db_text =
            "[" + cl_cdb_item(proj.root, proj.root + "/x323_tpl.cpp") + ","
            + cl_cdb_item(proj.root, proj.root + "/x135_tpl.cpp") + ","
            + cl_cdb_item(proj.root, proj.root + "/x32_fntpl.cpp") + ","
            + cl_cdb_item(proj.root, proj.root + "/x105_memberdef.cpp") + ","
            + cl_cdb_item(proj.root, proj.root + "/x32_clsmember.cpp") + ","
            + gnu_item + "]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto                db = cc::load(db_path, {});
        const cc::CompileCommand* x323 = cc::find_command(
            db,
            cg::util::norm_path(proj.root + "/x323_tpl.cpp"));
        const cc::CompileCommand* x135 = cc::find_command(
            db,
            cg::util::norm_path(proj.root + "/x135_tpl.cpp"));
        const cc::CompileCommand* x32fn = cc::find_command(
            db,
            cg::util::norm_path(proj.root + "/x32_fntpl.cpp"));
        const cc::CompileCommand* x105 = cc::find_command(
            db,
            cg::util::norm_path(proj.root + "/x105_memberdef.cpp"));
        const cc::CompileCommand* x32cls = cc::find_command(
            db,
            cg::util::norm_path(proj.root + "/x32_clsmember.cpp"));
        const cc::CompileCommand* gnu = cc::find_command(
            db,
            cg::util::norm_path(proj.root + "/gnu_main.cpp"));
        if (x323 == nullptr || x135 == nullptr || x32fn == nullptr
            || x105 == nullptr || x32cls == nullptr || gnu == nullptr)
        {
            cg::test::check(r, false, "all dtp fixture entries must load");
            return;
        }
        const cc::Index index;
        cg::test::check(
            r,
            index.valid(),
            "index initializes for the dtp parses");
        const struct
        {
            const cc::CompileCommand* cmd;
            const char*               name;
        } saved[] = {
            {x323, "member call"},
            {x135, "lock-guard-style ctor"},
            {x32fn, "fn-template constexpr"}};
        for (const auto& s : saved)
        {
            const cc::ParsedTu parsed =
                cc::parse(index, s.cmd->file, s.cmd->argv);
            cg::test::check(
                r,
                parsed.tu.valid() && parsed.errors.empty(),
                std::string{"cl dead-template shape ("} + s.name
                    + ") parses clean via /Zc:twoPhase- equivalence");
        }
        const cc::ParsedTu spec = cc::parse(index, x105->file, x105->argv);
        cg::test::check(
            r,
            spec.tu.valid() && spec.errors.empty() && spec.dialect_fixit_applied
                && spec.dialect_fixit_edits >= 1,
            "x105 lenient-specialization shape is auto-fixed by the "
            "compiler fixit (in-memory rewrite, one reparse; the "
            "specialized member becomes a definition)");
        // 改写仍失败时的分类口径用字面拼写钉（改写路径之上，残余同类
        // 诊断照常归 msvc-permissive-lenient-specialization）。
        const std::string spec_first =
            "template specialization requires 'template<>'";
        const std::string spec_class =
            cg::graph::detail::classify_dialect_diag_failure(spec_first, *x105);
        cg::test::check(
            r,
            spec_class.starts_with("msvc-permissive-lenient-specialization:"),
            "x105 spelling classifies into the named dialect class");
        cg::test::check(
            r,
            cg::graph::detail::failure_class_of(spec_class)
                == "msvc-permissive-lenient-specialization",
            "the dialect class aggregates under its own name (not "
            "parse-failed)");
        const cc::ParsedTu cls_const =
            cc::parse(index, x32cls->file, x32cls->argv);
        const std::string cls_first = cls_const.errors.empty()
                                          ? std::string{}
                                          : cls_const.errors.front().spelling;
        cg::test::check(
            r,
            cls_const.tu.valid() && !cls_const.errors.empty()
                && cls_first.find(
                       "must be initialized by a constant "
                       "expression")
                       != std::string::npos,
            "class-template static constexpr member still fails "
            "with its exact spelling (dtp does not cover it)");
        cg::test::check(
            r,
            !cls_const.dialect_fixit_applied,
            "no-fixit shape (constexpr member) never triggers the "
            "fixit rewrite (no invented insertion points)");
        cg::test::check(
            r,
            cg::graph::detail::classify_dialect_diag_failure(cls_first, *x32cls)
                .starts_with("msvc-permissive-unchecked-constexpr:"),
            "b12-shape spelling classifies into the named dialect "
            "class");
        cg::test::check(
            r,
            cg::graph::detail::classify_dialect_diag_failure(
                "call to non-static member function without an "
                "object argument",
                *x323)
                .starts_with("msvc-permissive-unchecked-member-call:"),
            "x323 canonical spelling maps to its named class");
        cg::test::check(
            r,
            cg::graph::detail::classify_dialect_diag_failure(
                "no matching constructor for initialization of "
                "'std::lock_guard<std::mutex>'",
                *x135)
                .starts_with("msvc-permissive-unchecked-ctor:"),
            "x135 canonical spelling maps to its named class");
        cg::test::check(
            r,
            cg::graph::detail::classify_dialect_diag_failure(spec_first, *gnu)
                .empty(),
            "GNU-dialect entry never books an msvc-permissive "
            "class (dialect gate)");
        cg::test::check(
            r,
            cg::graph::detail::classify_dialect_diag_failure(
                "use of undeclared identifier 'kLiveKey'",
                *x323)
                .empty(),
            "non-dialect spelling keeps the raw first error");
    }

    // 诊断驱动的内存源码改写（cl 方言宽松特化家族）：MSVC 接受、clang 报
    // "template specialization requires 'template<>'" 的类外成员特化，其
    // 诊断自带 FixItHint（声明头插入 "template<> "，插入点与文本全部由
    // 编译器给出）。触发三合一钉板：cl 方言（GNU 同形文件零改动、仍失
    // 败）、首轮诊断含目标拼写、该诊断带 FixIt（constexpr 形状无 FixIt
    // 零动作，见上 cls_const 断言）。改写只发生在内存源码缓冲（磁盘逐
    // 字节不变），主文件与被包含头一并改写，多条 FixIt 一批应用后仅一
    // 次重解析；改写后的初始化形特化成为定义（is_definition）。
    void cl_lenient_specialization_fixit_rewrite(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "lenient-specialization fixit test skipped: no driver");
            return;
        }
        TempProj proj{"ccg-cl-fixit-"};
        // 被包含头一处（声明形：selectany 静态成员的类外特化定义）。
        proj.put(
            proj.base / "spec_inc.h",
            "#pragma once\n"
            "struct DemoLock {};\n"
            "template <class T> struct DemoSgl {\n"
            "    static DemoLock mutex_;\n"
            "    static T* value_;\n"
            "};\n"
            "template <class T> DemoLock DemoSgl<T>::mutex_;\n"
            "__declspec(selectany) DemoLock DemoSgl<int>::mutex_;\n");
        // 主文件两处：声明形（A`<int>`::v，改写后仍为声明）与初始化形
        // （A`<long>`::v，改写后成为定义）。
        proj.put(
            proj.base / "spec_main.cpp",
            "#include \"spec_inc.h\"\n"
            "template <class T> struct A { static int v; };\n"
            "template <class T> int A<T>::v = 0;\n"
            "__declspec(selectany) int A<int>::v;\n"
            "__declspec(selectany) int A<long>::v = 3;\n"
            "int main() { return 0; }\n");
        // GNU 门禁用同形文件（clang GNU 方言同样拒绝该形状）。
        proj.put(
            proj.base / "spec_gnu.cpp",
            "#include \"spec_inc.h\"\n"
            "template <class T> struct A { static int v; };\n"
            "template <class T> int A<T>::v = 0;\n"
            "__declspec(selectany) int A<int>::v;\n"
            "__declspec(selectany) int A<long>::v = 3;\n"
            "int main() { return 0; }\n");
        const std::string gnu_item =
            "{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/spec_gnu.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-std=c++20\",\"-c\"," + q(proj.root + "/spec_gnu.cpp") + "]}";
        proj.put(
            proj.base / "compile_commands.json",
            "[" + cl_cdb_item(proj.root, proj.root + "/spec_main.cpp") + ","
                + gnu_item + "]");
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto                db = cc::load(db_path, {});
        const cc::CompileCommand* cl_entry = cc::find_command(
            db,
            cg::util::norm_path(proj.root + "/spec_main.cpp"));
        const cc::CompileCommand* gnu_entry = cc::find_command(
            db,
            cg::util::norm_path(proj.root + "/spec_gnu.cpp"));
        cg::test::check(
            r,
            cl_entry != nullptr && gnu_entry != nullptr,
            "fixit fixture entries load");
        if (cl_entry == nullptr || gnu_entry == nullptr)
        {
            return;
        }
        const std::string main_disk_before = cc::read_source(cl_entry->file);
        const std::string inc_disk_before =
            cc::read_source(proj.root + "/spec_inc.h");
        const cc::Index index;
        cg::test::check(
            r,
            index.valid(),
            "index initializes for the fixit rewrite parse");
        // cl 方言：触发三合一命中，三条 FixIt（主文件两条 + 头一条）一批
        // 应用，一次重解析后零 Error 级诊断。
        const cc::ParsedTu parsed =
            cc::parse(index, cl_entry->file, cl_entry->argv);
        cg::test::check(
            r,
            parsed.tu.valid() && parsed.errors.empty()
                && parsed.dialect_fixit_applied,
            "cl lenient-specialization TU is fixed via the compiler "
            "fixit (one reparse, zero error diagnostics)");
        cg::test::check(
            r,
            parsed.dialect_fixit_edits == 3,
            "all three fixits (two in the main file, one in the "
            "included header) applied in a single batch");
        cg::test::check(
            r,
            parsed.dialect_fixit_files.size() == 2,
            "both rewritten files (main + included header) are "
            "annotated");
        // 磁盘逐字节不变：改写只活在 unsaved 缓冲里。
        cg::test::check(
            r,
            cc::read_source(cl_entry->file) == main_disk_before
                && cc::read_source(proj.root + "/spec_inc.h")
                       == inc_disk_before,
            "on-disk sources are byte-identical after the rewrite");
        // 初始化形特化成为定义；声明形保住符号（恢复机制原本就入图）。
        const auto vars = collect(
            parsed.tu,
            [](const CXCursor& c)
            {
                return cc::kind(c) == CXCursor_VarDecl
                       && cc::spelling(c) == "v";
            },
            true);
        bool decl_kept = false;
        bool init_now_definition = false;
        for (const CXCursor& c : vars)
        {
            const cc::SourceLocation loc = cc::location(c);
            if (loc.line == 4)
            {
                decl_kept = true;
            }
            if (loc.line == 5 && cc::is_definition(c))
            {
                init_now_definition = true;
            }
        }
        cg::test::check(
            r,
            decl_kept,
            "declaration-form specialization symbol survives");
        cg::test::check(
            r,
            init_now_definition,
            "initialized specialization is a definition after the "
            "fixit rewrite");
        const auto header_vars = collect(
            parsed.tu,
            [](const CXCursor& c)
            {
                return cc::kind(c) == CXCursor_VarDecl
                       && cc::spelling(c) == "mutex_";
            },
            false);
        bool header_mutex = false;
        for (const CXCursor& c : header_vars)
        {
            if (cc::location(c).line == 8
                && cg::util::norm_path(cc::location(c).file)
                       == cg::util::norm_path(proj.root + "/spec_inc.h"))
            {
                header_mutex = true;
            }
        }
        cg::test::check(
            r,
            header_mutex,
            "header-side specialization is present in the rewritten "
            "parse");
        // GNU 门禁：同形文件在 GNU 方言下零改动、照样失败（拼写原样）。
        const cc::ParsedTu gnu =
            cc::parse(index, gnu_entry->file, gnu_entry->argv);
        cg::test::check(
            r,
            gnu.tu.valid() && !gnu.errors.empty() && !gnu.dialect_fixit_applied
                && gnu.errors.front().spelling.find(
                       "template specialization requires")
                       != std::string::npos,
            "GNU-dialect entry never rewrites (dialect gate; same "
            "engine, same spelling, no fixit)");
    }

    // 装载层的模板模式等效旗标（仅原生 cl.exe 著条目）：未声明模板模式
    // 的条目 argv 末尾补 /Zc:twoPhase-（MSVC 默认延迟解析的 clang-cl 官
    // 方等效拼写）；显式声明过（/permissive- 或 /Zc:twoPhase 系列）的原
    /// 路径分隔符的原生拼写（'/' 全部换 '\'）。
    [[nodiscard]]
    auto native_path_spelling(std::string s) -> std::string
    {
        for (char& c : s)
        {
            if (c == '/')
            {
                c = '\\';
            }
        }
        return s;
    }

    /// 构建树的文件清单快照（相对路径有序，排除缓存目录）。
    [[nodiscard]]
    auto build_tree_snapshot(const std::filesystem::path& base)
        -> std::vector<std::string>
    {
        namespace fs = std::filesystem;
        std::vector<std::string> out;
        std::error_code          sec;
        for (fs::recursive_directory_iterator it{base, sec}, end;
             !sec && it != end;
             it.increment(sec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec))
            {
                continue;
            }
            const std::string rel =
                fs::relative(it->path(), base, fec).generic_string();
            if (fec || rel.starts_with(".pchcache/"))
            {
                continue;
            }
            out.push_back(rel);
        }
        std::ranges::sort(out);
        return out;
    }

    /// 条目的 argv 是否含给定旗标。
    [[nodiscard]]
    auto command_has_flag(const cc::CompileCommand* c, std::string_view flag)
        -> bool
    {
        return c != nullptr
               && std::ranges::find(c->argv, flag) != c->argv.end();
    }

    // 样保留；clang-cl 自著条目忠实其自身语义不加。
    void loader_two_phase_equivalence(R& r)
    {
        namespace fs = std::filesystem;
        TempProj proj{"ccg-ltp2-"};
        proj.put(proj.base / "plain.cpp", "int main() { return 0; }\n");
        proj.put(proj.base / "strict.cpp", "int main() { return 0; }\n");
        proj.put(proj.base / "clangcl.cpp", "int main() { return 0; }\n");
        proj.put(
            proj.base / "compile_commands.json",
            "[{\"directory\":" + q(proj.root)
                + ",\"file\":" + q(proj.root + "/plain.cpp")
                + ",\"arguments\":[\"cl.exe\",\"/c\","
                + q(proj.root + "/plain.cpp") + "]},{\"directory\":"
                + q(proj.root) + ",\"file\":" + q(proj.root + "/strict.cpp")
                + ",\"arguments\":[\"cl.exe\",\"/c\",\"/permissive-\","
                + q(proj.root + "/strict.cpp") + "]},{\"directory\":"
                + q(proj.root) + ",\"file\":" + q(proj.root + "/clangcl.cpp")
                + ",\"arguments\":[\"clang-cl\",\"--driver-mode=cl\",\"/c\","
                + q(proj.root + "/clangcl.cpp") + "]}]");
        const auto db = cc::load(
            cg::util::norm_path(
                (proj.base / "compile_commands.json").generic_string()),
            {});
        cg::test::check(
            r,
            command_has_flag(
                cc::find_command(
                    db,
                    cg::util::norm_path(proj.root + "/plain.cpp")),
                "/Zc:twoPhase-"),
            "native cl entry without a declared template mode gets "
            "/Zc:twoPhase- appended (visible in the db argv)");
        cg::test::check(
            r,
            !command_has_flag(
                cc::find_command(
                    db,
                    cg::util::norm_path(proj.root + "/strict.cpp")),
                "/Zc:twoPhase-"),
            "explicit /permissive- stays verbatim, no append");
        cg::test::check(
            r,
            !command_has_flag(
                cc::find_command(
                    db,
                    cg::util::norm_path(proj.root + "/clangcl.cpp")),
                "/Zc:twoPhase-"),
            "clang-cl authored entry keeps its own semantics "
            "(no append)");
    }

    // cl 方言的系统搜索链必须来自 `--driver-mode=cl` 的讯问：GNU 链
    // （libc++）与 cl 链（MSVC STL/Windows Kits 或记录的 INCLUDE）是两套
    // 完全不同的头文件集合。用错链会让闭包哈希另一套头文件。
    void pch_cl_system_dirs(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "cl system dirs skipped: no driver");
            return;
        }
        const auto& cl = cc::driver_include_dirs(true, true);
        const auto& gnu = cc::driver_include_dirs(true, false);
        if (cl.empty())
        {
            cg::test::check(
                r,
                true,
                "cl system dirs skipped: empty cl-mode search list");
            return;
        }
        // 链隔离断言的判别前提是工具链口味：gnu 方言解析到 libc++（如
        // msys2 clang64）时两条链必须不同；官方 LLVM（MSVC ABI 驱动）在
        // 两种 driver-mode 下都解析 MSVC STL，cl==gnu 是设计内重合，此时
        // 判别断言不适用（下方闭包检查仍然完整执行）。
        const bool gnu_resolves_msvc_stl = [&]
        {
            for (const std::string& d : gnu)
            {
                if (d.find("Microsoft Visual Studio") != std::string::npos
                    || d.find("/MSVC/") != std::string::npos
                    || d.find("\\MSVC\\") != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }();
        cg::test::check(
            r,
            cl != gnu || gnu_resolves_msvc_stl,
            "cl-mode system chain differs from the GNU chain");
        TempProj        proj{"ccg-pch-clstd-"};
        std::error_code ec;
        proj.put(
            proj.base / "pch.h",
            "#pragma once\n#include <type_traits>\nstruct ClStd { int v; };\n");
        proj.put(proj.base / "pch.cpp", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){ClStd p{1};return p.v;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(proj.root)
            + ",\"/Ycpch.h\",\"/Fp" + proj.root + "/app.pch\","
            + q(proj.root + "/pch.cpp") + "]},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(proj.root)
            + ",\"/Yupch.h\",\"/Fp" + proj.root + "/app.pch\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto     db = cc::load(db_path, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        std::string             manifest_text;
        for (const auto& e :
             fs::recursive_directory_iterator(opt.ccg_config_dir, ec))
        {
            if (e.is_regular_file() && e.path().filename() == "manifest.json")
            {
                manifest_text =
                    cg::util::read_text_file(e.path().generic_string())
                        .value_or(std::string{});
            }
        }
        bool from_cl_chain = false;
        for (const std::string& d : cl)
        {
            if (d.empty() || std::ranges::find(gnu, d) != gnu.end())
            {
                continue;
            }
            if (manifest_text.find(d) != std::string::npos)
            {
                from_cl_chain = true;
            }
        }
        cg::test::check(
            r,
            plan.built == 1 && (from_cl_chain || gnu_resolves_msvc_stl),
            "cl closure resolves through the cl-mode system chain "
            "(built="
                + std::to_string(plan.built)
                + " cl_dirs=" + std::to_string(cl.size()) + " manifest_bytes="
                + std::to_string(manifest_text.size()) + ")");
        const cc::PchPlanResult again = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            again.built == 0 && again.reused == 1,
            "cl shadow with #include_next chains is reusable "
            "(reused="
                + std::to_string(again.reused) + ")");
    }

    // CPLUS_INCLUDE_PATH / C_INCLUDE_PATH 只对对应语言生效（CPATH 中立的
    // 语义由 pch_closure_env_search 覆盖）。不裁剪会让 C++ TU 的闭包哈希
    // 一个真实编译根本不会读的 C 头文件目录。
    void pch_closure_env_language(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "env language test skipped: no clang driver");
            return;
        }
        TempProj          proj{"ccg-pch-envlang-"};
        std::error_code   ec;
        const std::string cdir = proj.root + "/cdir";
        proj.put(
            proj.base / "cdir" / "envdep.h",
            "#pragma once\nstruct EnvLangPass { int e; };\n");
        proj.put(proj.base / "pch.h", "#pragma once\n#include <envdep.h>\n");
        proj.put(proj.base / "pch.cxx", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){EnvLangPass p{1};return p.e;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cxx")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-x\",\"c++-header\",\"-Xclang\",\"-emit-pch\","
              "\"-Xclang\",\"-include\",\"-Xclang\",\"pch.h\",\"-o\","
              "\"common.pch\",\"-c\","
            + q(proj.root + "/pch.cxx") + "],\"output\":"
            + q(proj.root + "/common.pch") + "},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-include-pch\",\"common.pch\",\"-include\",\"pch.h\","
              "\"-c\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        if (db.empty())
        {
            cg::test::check(
                r,
                false,
                "env language fixture loads one producer");
            return;
        }
        const cg::test::EnvScope cpath_guard{"CPATH"};
        const cg::test::EnvScope cinc_guard{"C_INCLUDE_PATH"};
        const cg::test::EnvScope cplus_guard{"CPLUS_INCLUDE_PATH"};
        cc::PchOptions           opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        cpath_guard.set("");
        cplus_guard.set("");
        cinc_guard.set(cdir);
        const cc::PchPlanResult c_only = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            c_only.built == 0,
            "C_INCLUDE_PATH alone is not searched for a C++ TU "
            "(shadow build fails on the unresolved include; got built="
                + std::to_string(c_only.built) + ")");
        cinc_guard.set("");
        cplus_guard.set(cdir);
        const cc::PchPlanResult cxx = cc::plan_pch(db, db, opt);
        const bool              cxx_first = cxx.built >= 1;
        const cc::PchPlanResult cxx2 = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            cxx_first && cxx2.reused >= 1,
            "CPLUS_INCLUDE_PATH is searched for a C++ TU and the "
            "closure stays stable (built="
                + std::to_string(cxx.built)
                + " reused=" + std::to_string(cxx2.reused) + ")");
    }

    // 影子重放必须只读：记录 argv 里的输出副作用旗标不得写进构建树。
    void pch_replay_no_tree_pollution(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "replay pollution check skipped: no driver");
            return;
        }
        TempProj        proj{"ccg-pch-side-"};
        std::error_code ec;
        proj.put(
            proj.base / "common.h",
            "#pragma once\nstruct SidePass { int v; };\n");
        proj.put(proj.base / "pch.cxx", "#include \"common.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"common.h\"\nint main(){SidePass p{1};return p.v;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cxx")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-x\",\"c++-header\",\"-Xclang\",\"-emit-pch\","
              "\"-Xclang\",\"-include\",\"-Xclang\",\"common.h\","
              "\"-MJ\","
            + q(proj.root + "/mj-pollution.json")
            + ",\"--serialize-diagnostics\","
            + q(proj.root + "/sd-pollution.bin")
            + ",\"-ftime-trace\","
              "\"-save-temps=obj\",\"-fdump-record-layouts\",\"-o\","
              "\"common.pch\",\"-c\","
            + q(proj.root + "/pch.cxx") + "],\"output\":"
            + q(proj.root + "/common.pch") + "},{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-include-pch\",\"common.pch\",\"-include\",\"common.h\","
              "\"-c\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        // 构建树快照：排除缓存目录（.pchcache）后，影子构建前后文件集合
        // 必须逐项相同——不只断言两个已知污染文件名不存在。
        const std::vector<std::string> before = build_tree_snapshot(proj.base);
        cc::PchOptions                 opt;
        opt.ccg_config_dir = proj.root + "/.pchcache/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult        plan = cc::plan_pch(db, db, opt);
        const std::vector<std::string> after = build_tree_snapshot(proj.base);
        cg::test::check(
            r,
            plan.built == 1,
            "shadow with side-effect flags still builds");
        cg::test::check(
            r,
            !fs::exists(proj.base / "mj-pollution.json", ec)
                && !fs::exists(proj.base / "sd-pollution.bin", ec),
            "shadow replay does not write output side-effect "
            "files into the build tree");
        cg::test::check(
            r,
            before == after,
            "the build tree file set is unchanged by the shadow "
            "build (full snapshot, cache excluded)");
    }

    // replay-degraded 是独立状态/计数，不得伪装成“文本回退”。
    void pch_replay_degraded_count(R& r)
    {
        namespace fs = std::filesystem;
        TempProj proj{"ccg-pch-deg-"};
        proj.put(
            proj.base / "common.h",
            "#pragma once\nstruct Deg { int v; };\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"common.h\"\nint main(){Deg d{1};return d.v;}\n");
        proj.put(proj.base / "recorded.pch", "not a usable pch\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-include-pch\"," + q(proj.root + "/recorded.pch") + ",\"-c\","
            + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto     db = cc::load(db_path, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        opt.replay_degraded = true;
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            plan.replay_degraded == 1 && plan.text_fallback == 0,
            "replay-degraded has its own count (not a text "
            "fallback)");
        for (const cc::CompileCommand& c : plan.commands)
        {
            if (c.file == proj.root + "/main.cpp")
            {
                cg::test::check(
                    r,
                    c.pch.status == cc::PchUse::Status::replay_degraded,
                    "replay-degraded consumer keeps its status");
            }
        }
    }

    // 零变化门禁：无 PCH 标记的条目经 plan_pch 后 argv 逐字节不变，且
    // 不产生缓存目录/清单/note。
    void pch_no_data_zero_change(R& r)
    {
        namespace fs = std::filesystem;
        TempProj        proj{"ccg-pch-zero-"};
        std::error_code ec;
        proj.put(proj.base / "a.cpp", "int a(){return 1;}\n");
        proj.put(proj.base / "b.cpp", "int b(){return 2;}\n");
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/a.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\",\"./\",\"-DVAL=1\","
              "\"-std=c++20\",\"-c\",\""
            + proj.root
            + "/a.cpp\"]},"
              "{\"directory\":"
            + q(proj.root) + ",\"file\":" + q(proj.root + "/b.cpp")
            + ",\"arguments\":[\"clang++\",\"-O2\",\"-Wall\",\"-c\","
              "\""
            + proj.root + "/b.cpp\"]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto     db = cc::load(db_path, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        bool                    identical = plan.commands.size() == db.size();
        for (std::size_t i = 0; identical && i < db.size(); ++i)
        {
            identical = plan.commands[i].file == db[i].file
                        && plan.commands[i].directory == db[i].directory
                        && plan.commands[i].argv == db[i].argv;
        }
        cg::test::check(
            r,
            identical,
            "non-PCH entries keep their argv byte-for-byte through "
            "plan_pch (zero-change gate)");
        cg::test::check(
            r,
            plan.notes.empty() && plan.tu_notes.empty()
                && plan.cache_dir.empty() && plan.built == 0 && plan.reused == 0
                && !fs::exists(proj.base / ".ccg" / "Debug", ec),
            "non-PCH project creates no pch dir/manifest/note");
    }

    // 无 PCH 工程旁落无效/陈旧 sidecar：静默忽略，绝不创建 pch 目录/
    // 清单，也不打印 note（零变化门禁）。
    void pch_no_data_no_cache_dir(R& r)
    {
        namespace fs = std::filesystem;
        TempProj        proj{"ccg-pch-nodata-"};
        std::error_code ec;
        proj.put(proj.base / "main.cpp", "int main(){return 0;}\n");
        const std::string cdb_text =
            "[{\"directory\":" + q(proj.root) + ",\"file\":"
            + q(proj.root + "/main.cpp") + ",\"arguments\":[\"clang++\",\"-I\","
            + q(proj.root) + ",\"-c\"," + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", cdb_text);
        proj.put(
            proj.base / "pch.json",
            "{\"version\":1,\"cdb_sha256\":\"deadbeef\","
            "\"producers\":[],\"consumers\":[]}");
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto     db = cc::load(db_path, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = proj.root + "/.ccg/Debug";
        opt.compile_db_path = db_path;
        opt.sidecar_path =
            cg::util::norm_path((proj.base / "pch.json").generic_string());
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            plan.cache_dir.empty() && plan.built == 0 && plan.reused == 0,
            "stale sidecar in a no-PCH project plans nothing");
        cg::test::check(
            r,
            !fs::exists(proj.base / ".ccg" / "Debug" / "pch", ec)
                && !fs::exists(proj.base / ".ccg" / "Debug", ec),
            "stale sidecar creates no pch cache directory and no "
            "empty manifest");
        cg::test::check(
            r,
            plan.notes.empty() && plan.tu_notes.empty(),
            "stale sidecar is silently ignored (no PCH trace in "
            "notes)");
        // 别字 sidecar（截断 JSON）同样静默。
        proj.put(proj.base / "pch.json", "{\"version\":1,\"producers\":[");
        const cc::PchPlanResult broken = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            broken.cache_dir.empty() && broken.notes.empty()
                && broken.tu_notes.empty()
                && !fs::exists(proj.base / ".ccg" / "Debug", ec),
            "malformed sidecar is ignored with no PCH trace");
    }

    // ---------------------------------------------------------------------------
    // 系统头自包含改造（系统头自包含 + /X + 完整兼容版本）的回归。

    // cl.exe 路径解析：版本、目标架构与 vcvars 脚本选择；兼容版本映射。
    void msvc_toolset_path_rules(R& r)
    {
        const cc::MsvcToolsetPath x64 = cc::parse_msvc_toolset_path(
            "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\"
            "Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x64\\cl.exe");
        cg::test::check(
            r,
            x64.version == "14.44.35207",
            "toolset version parsed from the cl path");
        cg::test::check(
            r,
            x64.target_arch == "x64",
            "target arch parsed (Hostx64/x64 -> x64)");
        cg::test::check(
            r,
            x64.vs_root.ends_with("/Community"),
            "VS install root precedes VC/Tools/MSVC");
        cg::test::check(
            r,
            x64.vcvars_bat.ends_with(
                "/VC/Auxiliary/Build/"
                "vcvars64.bat"),
            "x64 selects vcvars64.bat");
        const cc::MsvcToolsetPath x86 = cc::parse_msvc_toolset_path(
            "D:/VS/VC/Tools/MSVC/14.38.33130/bin/Hostx64/x86/cl.exe");
        cg::test::check(
            r,
            x86.target_arch == "x86"
                && x86.vcvars_bat.ends_with("vcvars32.bat"),
            "x86 target selects vcvars32.bat");
        const cc::MsvcToolsetPath arm64 = cc::parse_msvc_toolset_path(
            "D:/VS/VC/Tools/MSVC/14.38.33130/bin/Hostx64/arm64/cl.exe");
        cg::test::check(
            r,
            arm64.target_arch == "arm64"
                && arm64.vcvars_bat.ends_with("vcvarsarm64.bat"),
            "arm64 target selects vcvarsarm64.bat");
        cg::test::check(
            r,
            cc::parse_msvc_toolset_path("clang-cl").version.empty(),
            "no toolset clue for a bare clang-cl");
        cg::test::check(
            r,
            cc::parse_msvc_toolset_path("D:/VS/VC/Tools/MSVC/14.44/bin/cl.exe")
                .version.empty(),
            "two-part version is not a toolset clue");
        cg::test::check(
            r,
            cc::msvc_compat_version("14.51.36231") == "19.51.36231",
            "compat version maps 14.minor.build -> 19.minor.build");
        cg::test::check(
            r,
            cc::msvc_compat_version("14.51").empty(),
            "compat version requires three parts");
    }

    // 原生 MSVC cdb（cl.exe /c ...）载入后必须自带 cl 方言；clang/clang++
    // 条目逐字节不变。arguments 与 command 两种形状都覆盖。
    void compile_db_native_cl_dialect(R& r)
    {
        TempProj proj{"ccg-native-cl-"};
        proj.put(proj.base / "a.cpp", "int a(){return 1;}\n");
        proj.put(proj.base / "b.cpp", "int b(){return 2;}\n");
        proj.put(proj.base / "c.cpp", "int c(){return 3;}\n");
        proj.put(proj.base / "d.cpp", "int d(){return 4;}\n");
        proj.put(proj.base / "e.cpp", "int e(){return 5;}\n");
        proj.put(proj.base / "f.cpp", "int f(){return 6;}\n");
        // 带工具集的 cl 路径：无需 vcvars 即可推导版本
        // （下方原始 /X 拼写完全抑制 INCLUDE 捕获）。
        const std::string toolset_cl =
            proj.root + "/VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe";
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/a.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(proj.root)
            + ",\"a.cpp\"]},"
              "{\"directory\":"
            + q(proj.root) + ",\"file\":" + q(proj.root + "/b.cpp")
            + ",\"command\":" + q("cl.exe /c /I " + proj.root + " b.cpp")
            + "},"
              "{\"directory\":"
            + q(proj.root) + ",\"file\":" + q(proj.root + "/c.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(proj.root)
            + ",\"-c\",\"c.cpp\"]},"
              "{\"directory\":"
            + q(proj.root) + ",\"file\":" + q(proj.root + "/d.cpp")
            + ",\"arguments\":[" + q(toolset_cl) + ",\"/c\",\"-X\",\"/I\","
            + q(proj.root)
            + ",\"d.cpp\"]},"
              "{\"directory\":"
            + q(proj.root) + ",\"file\":" + q(proj.root + "/e.cpp")
            + ",\"arguments\":[" + q(toolset_cl) + ",\"/c\",\"/x\",\"/I\","
            + q(proj.root)
            + ",\"e.cpp\"]},"
              "{\"directory\":"
            + q(proj.root) + ",\"file\":" + q(proj.root + "/f.cpp")
            + ",\"arguments\":[" + q(toolset_cl) + ",\"/c\",\"-x\",\"/I\","
            + q(proj.root) + ",\"f.cpp\"]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});
        cg::test::check(
            r,
            db.size() == 6,
            "native cl fixture loads six entries");
        const auto* a = cc::find_command(db, proj.root + "/a.cpp");
        const auto* b = cc::find_command(db, proj.root + "/b.cpp");
        const auto* c = cc::find_command(db, proj.root + "/c.cpp");
        const auto* d = cc::find_command(db, proj.root + "/d.cpp");
        const auto* e = cc::find_command(db, proj.root + "/e.cpp");
        const auto* f = cc::find_command(db, proj.root + "/f.cpp");
        if (a != nullptr)
        {
            cg::test::check(
                r,
                !a->argv.empty() && a->argv.front() == "--driver-mode=cl",
                "arguments form: --driver-mode=cl injected after the compiler");
            cg::test::check(
                r,
                a->system_includes_auto,
                "bare cl.exe without a toolset clue is honestly marked "
                "auto-detected");
        }
        if (b != nullptr)
        {
            cg::test::check(
                r,
                !b->argv.empty() && b->argv.front() == "--driver-mode=cl",
                "command form: --driver-mode=cl injected after the compiler");
            cg::test::check(
                r,
                b->system_includes_auto,
                "command form: no toolset clue -> auto-detected");
        }
        if (c != nullptr)
        {
            cg::test::check(
                r,
                std::ranges::find(c->argv, "--driver-mode=cl") == c->argv.end(),
                "clang++ entry is untouched (zero-change gate)");
            cg::test::check(
                r,
                !c->system_includes_auto,
                "clang++ entry is not marked auto-detected");
        }
        // 原始 `-X` / `/x`（IgnoreStandardIncludePath）：不捕获
        // INCLUDE，但兼容版本仍来自 cl
        // 路径。解耦之前整块被跳过，这些
        // TU 静默用 clang-cl 默认 _MSC_VER。
        for (const auto& [cmd, spelling] : std::initializer_list<
                 std::pair<const cc::CompileCommand*, std::string_view>>{
                 {d, "-X"},
                 {e, "/x"},
                 {f, "-x"}})
        {
            if (cmd == nullptr)
            {
                continue;
            }
            cg::test::check(
                r,
                std::ranges::find(cmd->argv, "--driver-mode=cl")
                    != cmd->argv.end(),
                std::string{spelling} + ": cl dialect is still injected");
            cg::test::check(
                r,
                std::ranges::find(
                    cmd->argv,
                    "-fms-compatibility-version=19.44.35207")
                    != cmd->argv.end(),
                std::string{spelling}
                    + ": compat version still pinned (decoupled from /X)");
            cg::test::check(
                r,
                std::ranges::find(cmd->argv, "-imsvc") == cmd->argv.end(),
                std::string{spelling}
                    + ": INCLUDE is not re-materialized (cl ignores it)");
            cg::test::check(
                r,
                std::ranges::find(cmd->argv, "/X") != cmd->argv.end()
                    && std::ranges::find(cmd->argv, spelling)
                           == cmd->argv.end(),
                std::string{spelling}
                    + ": canonicalized to /X (clang-cl only knows that spelling)");
            cg::test::check(
                r,
                !cmd->system_includes_auto,
                std::string{spelling}
                    + ": /X means no auto-detection to report");
        }
    }

    // 外来原生 MSVC cdb：cl 路径 -> vcvars INCLUDE -> /X + -imsvc + 兼容
    // 版本；`<yvals_core.h>` 可解析、`<__config>` 不可。无法重建时如实降级。
    void compile_db_foreign_cl_rebuild(R& r)
    {
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "foreign cl rebuild skipped: no driver");
            return;
        }
        const std::string cl = find_test_cl();
        if (cl.empty())
        {
            cg::test::check(
                r,
                true,
                "foreign cl rebuild skipped: no MSVC toolset found");
            return;
        }
        const cc::MsvcIncludeCapture captured =
            cc::capture_msvc_include_env(cl);
        if (!captured.ok)
        {
            cg::test::check(
                r,
                true,
                "foreign cl rebuild skipped: vcvars capture failed ("
                    + captured.reason + ")");
            return;
        }
        TempProj proj{"ccg-foreign-cl-"};
        proj.put(
            proj.base / "msvc.cpp",
            "#include <yvals_core.h>\nint msvc_probe = 1;\n");
        proj.put(
            proj.base / "libcxx.cpp",
            "#include <__config>\nint libcxx_probe = 1;\n");
        // 独立复核：手工在 vcvars shell 里 `set INCLUDE` 的结果必须与
        // capture_msvc_include_env 逐字一致（不同脚本、同一环境来源）。
        const cc::MsvcToolsetPath toolset = cc::parse_msvc_toolset_path(cl);
        if (!toolset.vcvars_bat.empty())
        {
            const std::filesystem::path manual =
                proj.base / "manual-include.cmd";
            const std::filesystem::path manual_out =
                proj.base / "manual-include.txt";
            {
                std::ofstream out{manual, std::ios::binary | std::ios::trunc};
                out << "@echo off\r\ncall \""
                    << native_path_spelling(toolset.vcvars_bat)
                    << "\" -vcvars_ver=" << toolset.version
                    << " >nul 2>&1\r\nset INCLUDE > \""
                    << native_path_spelling(manual_out.string()) << "\"\r\n";
            }
            const std::string manual_cmd =
                std::string{"cmd.exe /d /s /c \""}
                + native_path_spelling(manual.string()) + "\"";
            const int         manual_rc = std::system(manual_cmd.c_str());
            const std::string manual_text =
                cg::util::read_text_file(manual_out.generic_string())
                    .value_or(std::string{});
            const std::size_t eq = manual_text.find("INCLUDE=");
            std::string       manual_include;
            if (eq != std::string::npos)
            {
                const std::size_t nl = manual_text.find_first_of("\r\n", eq);
                manual_include = manual_text.substr(
                    eq + 8,
                    nl == std::string::npos ? std::string::npos : nl - eq - 8);
            }
            cg::test::check(
                r,
                manual_rc == 0 && manual_include == captured.include_env,
                "vcvars capture equals a manual `set INCLUDE` in a vcvars "
                "shell");
        }

        // 辅助来源：CMakeCache 的目标 SDK 必须与 vcvars 重建结果一致
        // （';' 分隔的整串做路径比较键折叠：分隔符不在折叠字符集内）。
        {
            const std::string lower =
                cg::util::path_compare_key(captured.include_env);
            std::string sdk;
            if (const std::size_t kits = lower.find("windows kits");
                kits != std::string::npos)
            {
                if (const std::size_t at = lower.find("10.0.", kits);
                    at != std::string::npos)
                {
                    std::size_t end = at;
                    while (end < lower.size()
                           && ((lower[end] >= '0' && lower[end] <= '9')
                               || lower[end] == '.'))
                    {
                        ++end;
                    }
                    sdk = lower.substr(at, end - at);
                }
            }
            if (!sdk.empty())
            {
                proj.put(
                    proj.base / "CMakeCache.txt",
                    "CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION:STRING=" + sdk
                        + "\n");
            }
        }

        const std::string db_path = make_cl_db(proj, cl, {"msvc.cpp"});
        const auto        db = cc::load(db_path, {});
        cg::test::check(r, db.size() == 1, "foreign cdb loads one entry");
        if (db.size() != 1)
        {
            return;
        }
        const cc::CompileCommand& cmd = db.front();
        cg::test::check(
            r,
            std::ranges::find(cmd.argv, "--driver-mode=cl") != cmd.argv.end(),
            "native cdb gains the cl dialect");
        cg::test::check(
            r,
            std::ranges::find(cmd.argv, "/X") != cmd.argv.end(),
            "foreign cl entry becomes self-contained (/X)");
        bool        has_compat = false;
        std::size_t imsvc = 0;
        for (const std::string& a : cmd.argv)
        {
            if (a.starts_with("-fms-compatibility-version="))
            {
                has_compat = true;
                const cc::MsvcToolsetPath cl_toolset =
                    cc::parse_msvc_toolset_path(cl);
                cg::test::check(
                    r,
                    a
                        == "-fms-compatibility-version="
                               + cc::msvc_compat_version(cl_toolset.version),
                    "compat version matches the cl path toolset");
            }
            if (a == "-imsvc")
            {
                ++imsvc;
            }
        }
        cg::test::check(r, has_compat, "compat version materialized");
        std::size_t want_dirs = 0;
        {
            std::string_view env{captured.include_env};
            while (!env.empty())
            {
                const std::size_t      semi = env.find(';');
                const std::string_view item = env.substr(
                    0,
                    semi == std::string_view::npos ? std::string_view::npos
                                                   : semi);
                if (!item.empty())
                {
                    ++want_dirs;
                }
                if (semi == std::string_view::npos)
                {
                    break;
                }
                env.remove_prefix(semi + 1);
            }
        }
        cg::test::check(
            r,
            imsvc == want_dirs,
            std::format(
                "all vcvars INCLUDE dirs materialized "
                "(got {}, want {})",
                imsvc,
                want_dirs));
        cg::test::check(
            r,
            !cmd.system_includes_auto,
            "successful rebuild is not marked auto-detected");

        // MSVC 专属头可解析，libc++ 专属头不可（/X 后只剩物化目录）。
        const std::string msvc_db = make_cl_db(proj, cl, {"msvc.cpp"});
        const auto        msvc_loaded = cc::load(msvc_db, {});
        const cc::Index   index;
        if (!msvc_loaded.empty())
        {
            const cc::ParsedTu parsed = cc::parse(
                index,
                msvc_loaded.front().file,
                msvc_loaded.front().argv);
            cg::test::check(
                r,
                parsed.errors.empty(),
                "MSVC-only <yvals_core.h> resolves after rebuild");
        }
        proj.put(proj.base / "compile_commands.json", "[]");
        const std::string libcxx_db = make_cl_db(proj, cl, {"libcxx.cpp"});
        const auto        libcxx_loaded = cc::load(libcxx_db, {});
        if (!libcxx_loaded.empty())
        {
            const cc::ParsedTu parsed = cc::parse(
                index,
                libcxx_loaded.front().file,
                libcxx_loaded.front().argv);
            cg::test::check(
                r,
                !parsed.errors.empty(),
                "libc++-only <__config> fails under /X (no silent "
                "libc++ fallback)");
        }

        // vswhere/vcvars 都失败（未安装的工具集版本）→ 自动探测 + 标注。
        TempProj missing{"ccg-foreign-cl-missing-"};
        missing.put(missing.base / "x.cpp", "int x(){return 1;}\n");
        const std::string fake_cl =
            missing.root
            + "/VS/VC/Tools/MSVC/14.99.99999/bin/Hostx64/x64/cl.exe";
        const std::string missing_db = make_cl_db(missing, fake_cl, {"x.cpp"});
        const auto        missing_loaded = cc::load(missing_db, {});
        cg::test::check(
            r,
            missing_loaded.size() == 1,
            "unrebuildable cl entry still loads");
        if (missing_loaded.size() == 1)
        {
            cg::test::check(
                r,
                missing_loaded.front().system_includes_auto,
                "failed rebuild is reported as system-includes "
                "auto-detected");
            cg::test::check(
                r,
                std::ranges::find(missing_loaded.front().argv, "/X")
                    == missing_loaded.front().argv.end(),
                "failed rebuild adds no /X (auto detection stays in charge)");
            cg::test::check(
                r,
                !missing_loaded.front().argv.empty()
                    && missing_loaded.front().argv.front()
                           == "--driver-mode=cl",
                "failed rebuild still fixes the dialect");
        }
    }

    // vcvars 子进程 10 秒硬上限：一个挂死的 vcvars 绝不能挂死索引；
    // 超时按普通失败走降级（自动探测）并如实标注。此处用假 VS 根 +
    // 唯一工具集版本（14.98.99999）隔离缓存，脚本只对该完整版本挂起，
    // 两段回退版本立即失败（总耗时约 10 秒）。
    void msvc_vcvars_timeout_degrades(R& r)
    {
        namespace fs = std::filesystem;
        TempProj       proj{"ccg-vcvars-timeout-"};
        const fs::path fake_vs = proj.base / "FakeVS";
        fs::create_directories(fake_vs / "VC/Auxiliary/Build", proj.ec);
        const fs::path vcvars = fake_vs / "VC/Auxiliary/Build/vcvars64.bat";
        {
            std::ofstream out{vcvars, std::ios::binary | std::ios::trunc};
            out << "@echo off\r\n"
                   "if \"%1\"==\"-vcvars_ver=14.98.99999\" goto hang\r\n"
                   "exit /b 1\r\n"
                   ":hang\r\n"
                   ":loop\r\n"
                   "goto loop\r\n";
        }
        const std::string cl_path = cg::util::norm_path(
            (fake_vs / "VC/Tools/MSVC/14.98.99999/bin/Hostx64/x64/cl.exe")
                .generic_string());
        const cc::MsvcIncludeCapture captured =
            cc::capture_msvc_include_env(cl_path);
        cg::test::check(
            r,
            !captured.ok,
            "hung vcvars is reported as a failed capture");
        cg::test::check(
            r,
            !captured.reason.empty(),
            "hung vcvars records a diagnostic reason");

        // 端到端：加载器不等待第二个 10s（缓存已存失败），保持自动探测
        // 并标注，且不做“精确”的 /X 物化。
        proj.put(proj.base / "x.cpp", "int x(){return 1;}\n");
        const std::string db_path = make_cl_db(proj, cl_path, {"x.cpp"});
        const auto        db = cc::load(db_path, {});
        cg::test::check(r, db.size() == 1, "timed-out cl entry still loads");
        if (db.size() == 1)
        {
            cg::test::check(
                r,
                db.front().system_includes_auto,
                "timed-out rebuild is reported as auto-detected");
            cg::test::check(
                r,
                std::ranges::find(db.front().argv, "/X")
                    == db.front().argv.end(),
                "timed-out rebuild adds no /X");
            cg::test::check(
                r,
                !db.front().argv.empty()
                    && db.front().argv.front() == "--driver-mode=cl",
                "timed-out rebuild still fixes the dialect");
        }
    }

    // 自包含 cl argv 的闭包顺序：resource dir 的 stddef.h 必须胜过
    // `-imsvc` 里的同名假头（旧顺序导致的陈旧影子复用入口）。
    void pch_closure_self_contained_order(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "closure order skipped: no driver");
            return;
        }
        const std::string resource = cg::util::norm_path(
            (fs::path{cc::driver_info().resource_dir} / "include")
                .lexically_normal()
                .generic_string());
        if (resource.empty()
            || !fs::is_regular_file(fs::path{resource + "/stddef.h"}))
        {
            cg::test::check(
                r,
                true,
                "closure order skipped: no resource stddef.h");
            return;
        }
        TempProj       proj{"ccg-pch-order-"};
        const fs::path fake = proj.base / "fakesys";
        fs::create_directories(fake, proj.ec);
        proj.put(fake / "stddef.h", "#define FAKE_STDDEF_HEADER 1\n");
        // resource-only 头：没有 include_next 链，直接证明 resource 先于
        // -imsvc 搜索（stddef.h 的 include_next 在文本扫描下仍会被跟随，
        // 因此假 stddef.h 可能作为其链尾出现——这不是遮蔽）。
        proj.put(fake / "__stddef_size_t.h", "#define FAKE_SIZE_T 1\n");
        proj.put(
            proj.base / "pch.h",
            "#pragma once\n#include <stddef.h>\n"
            "#include <__stddef_size_t.h>\n"
            "struct Order { int v; };\n");
        proj.put(proj.base / "pch.cpp", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){Order o{1};return o.v;}\n");
        const std::string fake_dir = cg::util::norm_path(fake.generic_string());
        cg::test::check(
            r,
            !cc::detail::is_self_contained_cl(
                {"clang-cl", "--driver-mode=cl", "/I", fake_dir}),
            "argv without IgnoreStandardIncludePath is not self-contained");
        // `/x` 与 `-X` 是同 `/X` 的 cl 选项；闭包必须把
        // 它们都当自包含（否则会把进程的
        // INCLUDE/CPATH/自动发现重新混进 cl 已隔离的 argv）。
        // 加载器把它们规范化成 `/X`，那是唯一
        // 被 clang-cl 接受的拼写。（`-x` 是 clang-cl 的语言选项，
        // 此处不当 /X 处理；原生 cl 条目由
        // 加载器在抵达规划器前规范化。）
        std::size_t case_index = 0;
        for (const char* spelling : {"/X", "-X", "/x"})
        {
            const std::string common =
                "\"clang-cl\",\"--driver-mode=cl\",\"/c\",\"/I\","
                + q(proj.root) + ",\"" + spelling + "\",\"-imsvc\","
                + q(fake_dir);
            const std::string db_text =
                "[{\"directory\":" + q(proj.root)
                + ",\"file\":" + q(proj.root + "/pch.cpp") + ",\"arguments\":["
                + common + ",\"/Ycpch.h\",\"/Fp" + proj.root + "/app.pch\","
                + q(proj.root + "/pch.cpp")
                + "]},"
                  "{\"directory\":"
                + q(proj.root) + ",\"file\":" + q(proj.root + "/main.cpp")
                + ",\"arguments\":[" + common + ",\"/Yupch.h\",\"/Fp"
                + proj.root + "/app.pch\"," + q(proj.root + "/main.cpp")
                + "]}]";
            proj.put(proj.base / "compile_commands.json", db_text);
            const std::string db_path = cg::util::norm_path(
                (proj.base / "compile_commands.json").generic_string());
            const auto     db = cc::load(db_path, {});
            cc::PchOptions opt;
            // 跨配置捐赠只扫配置目录的兄弟：CaseN 挂在 .ccg
            // 之下会让三个拼写互为兄弟，后续轮直接收编前轮
            // 影子（built=0）；CaseN/.ccg 让各轮目录互不为
            // 兄弟，每个拼写独立走一次自包含闭包构建。
            opt.ccg_config_dir = cg::util::norm_path(
                (proj.base / ("Case" + std::to_string(case_index++)) / ".ccg")
                    .generic_string());
            opt.compile_db_path = db_path;
            const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
            std::string             manifest_text;
            for (const auto& e :
                 fs::recursive_directory_iterator(opt.ccg_config_dir, proj.ec))
            {
                if (e.is_regular_file()
                    && e.path().filename() == "manifest.json")
                {
                    manifest_text =
                        cg::util::read_text_file(e.path().generic_string())
                            .value_or(std::string{});
                }
            }
            cg::test::check(
                r,
                plan.built >= 1,
                std::string{spelling}
                    + ": self-contained closure resolves (built="
                    + std::to_string(plan.built) + ")");
            cg::test::check(
                r,
                manifest_text.find(resource + "/stddef.h") != std::string::npos,
                std::string{spelling}
                    + ": closure uses the clang resource stddef.h");
            cg::test::check(
                r,
                manifest_text.find(fake_dir + "/__stddef_size_t.h")
                    == std::string::npos,
                std::string{spelling}
                    + ": closure excludes the -imsvc fake resource-only header");
        }
    }

    // 三环境一致性：自包含 argv 的闭包身份在 纯 shell / 进程 INCLUDE=垃圾 /
    // 进程 INCLUDE=捕获值 三种环境下完全相同。
    /// 自包含命令表的 PCH 闭包身份（main.cpp 条目；缺席给空串）。
    [[nodiscard]]
    auto pch_closure_identity_of(
        const std::string&                     config_dir,
        const std::string&                     db_path,
        const std::vector<cc::CompileCommand>& db,
        const std::string&                     main_file) -> std::string
    {
        cc::PchOptions opt;
        opt.ccg_config_dir = config_dir;
        opt.compile_db_path = db_path;
        opt.force = true; // 前一轮的缓存不得掩盖环境差异
        const cc::PchPlanResult plan = cc::plan_pch(db, db, opt);
        for (const cc::CompileCommand& c : plan.commands)
        {
            if (c.file == main_file)
            {
                return c.pch.identity;
            }
        }
        return std::string{};
    }

    void pch_system_include_env_invariance(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(r, true, "env invariance skipped: no driver");
            return;
        }
        const std::string            cl = find_test_cl();
        const cc::MsvcIncludeCapture captured =
            cl.empty() ? cc::MsvcIncludeCapture{}
                       : cc::capture_msvc_include_env(cl);
        if (!captured.ok)
        {
            cg::test::check(
                r,
                true,
                "env invariance skipped: no MSVC INCLUDE captured");
            return;
        }
        TempProj proj{"ccg-pch-envinv-"};
        proj.put(
            proj.base / "pch.h",
            "#pragma once\n#include <type_traits>\nstruct Inv { int v; };\n");
        proj.put(proj.base / "pch.cpp", "#include \"pch.h\"\n");
        proj.put(
            proj.base / "main.cpp",
            "#include \"pch.h\"\nint main(){Inv i{1};return i.v;}\n");
        std::string flags = "\"/X\"";
        {
            std::string_view env{captured.include_env};
            while (!env.empty())
            {
                const std::size_t      semi = env.find(';');
                const std::string_view item = env.substr(
                    0,
                    semi == std::string_view::npos ? std::string_view::npos
                                                   : semi);
                if (!item.empty())
                {
                    flags += ",\"-imsvc\"," + q(std::string{item});
                }
                if (semi == std::string_view::npos)
                {
                    break;
                }
                env.remove_prefix(semi + 1);
            }
        }
        const std::string common =
            "\"clang-cl\",\"--driver-mode=cl\",\"/c\",\"/I\"," + q(proj.root)
            + "," + flags;
        const std::string db_text =
            "[{\"directory\":" + q(proj.root)
            + ",\"file\":" + q(proj.root + "/pch.cpp") + ",\"arguments\":["
            + common + ",\"/Ycpch.h\",\"/Fp" + proj.root + "/app.pch\","
            + q(proj.root + "/pch.cpp")
            + "]},"
              "{\"directory\":"
            + q(proj.root) + ",\"file\":" + q(proj.root + "/main.cpp")
            + ",\"arguments\":[" + common + ",\"/Yupch.h\",\"/Fp" + proj.root
            + "/app.pch\"," + q(proj.root + "/main.cpp") + "]}]";
        proj.put(proj.base / "compile_commands.json", db_text);
        const std::string db_path = cg::util::norm_path(
            (proj.base / "compile_commands.json").generic_string());
        const auto db = cc::load(db_path, {});

        const cg::test::EnvScope include_guard{"INCLUDE"};
        const std::string        config_dir = proj.root + "/.ccg/Debug";
        const std::string        main_file = proj.root + "/main.cpp";
        include_guard.restore();
        const std::string pure_shell =
            pch_closure_identity_of(config_dir, db_path, db, main_file);
        include_guard.set(proj.root + "/bogus-include");
        const std::string bogus_env =
            pch_closure_identity_of(config_dir, db_path, db, main_file);
        include_guard.set(captured.include_env);
        const std::string captured_env =
            pch_closure_identity_of(config_dir, db_path, db, main_file);
        include_guard.restore();
        cg::test::check(
            r,
            !pure_shell.empty(),
            "env invariance: closure identity is non-empty");
        cg::test::check(
            r,
            pure_shell == bogus_env,
            "process INCLUDE cannot change a self-contained closure");
        cg::test::check(
            r,
            pure_shell == captured_env,
            "same closure under pure shell / captured INCLUDE");
    }

    const cg::test::Registrar reg_tokenize{
        "clang.tokenize_command",
        &tokenize_rules};
    const cg::test::Registrar reg_msvc_path{
        "clang.msvc_toolset_path_rules",
        &msvc_toolset_path_rules};
    const cg::test::Registrar reg_native_cl{
        "clang.compile_db_native_cl_dialect",
        &compile_db_native_cl_dialect};
    const cg::test::Registrar reg_foreign_cl{
        "clang.compile_db_foreign_cl_rebuild",
        &compile_db_foreign_cl_rebuild};
    const cg::test::Registrar reg_vcvars_timeout{
        "clang.msvc_vcvars_timeout_degrades",
        &msvc_vcvars_timeout_degrades};
    const cg::test::Registrar reg_pch_order{
        "clang.pch_closure_self_contained_order",
        &pch_closure_self_contained_order};
    const cg::test::Registrar reg_pch_invariance{
        "clang.pch_system_include_env_invariance",
        &pch_system_include_env_invariance};
    const cg::test::Registrar reg_engine_matrix{
        "clang.engine_resolution_matrix",
        &engine_resolution_matrix};
    const cg::test::Registrar reg_engine_identity{
        "clang.engine_load_identity",
        &engine_load_identity};
    const cg::test::Registrar reg_driver_chain{
        "clang.driver_chain_matrix",
        &driver_chain_matrix};
    const cg::test::Registrar reg_driver_rekey{
        "clang.driver_cache_rekey",
        &driver_cache_rekeys_on_engine_switch};
    const cg::test::Registrar reg_db{"clang.compile_db", &compile_db_load};
    const cg::test::Registrar reg_tok_gate{
        "clang.tokenize_gate",
        &tokenize_macro_extent_gate};
    const cg::test::Registrar reg_db_pch{
        "clang.compile_db_pch",
        &compile_db_pch_load};
    const cg::test::Registrar reg_pch_shadow{
        "clang.pch_shadow_build",
        &pch_shadow_build};
    const cg::test::Registrar reg_pch_cpath{
        "clang.pch_closure_env_search",
        &pch_closure_env_search};
    const cg::test::Registrar reg_pch_implicit{
        "clang.pch_implicit_include",
        &pch_implicit_include};
    const cg::test::Registrar reg_pch_fi{
        "clang.pch_sidecar_fi_closure",
        &pch_sidecar_fi_closure};
    const cg::test::Registrar reg_pch_macrosp{
        "clang.pch_sidecar_macro_space_digest",
        &pch_sidecar_macro_space_digest};
    const cg::test::Registrar reg_pch_macro{
        "clang.pch_macro_include_closure",
        &pch_macro_include_closure};
    const cg::test::Registrar reg_pch_cwd{
        "clang.pch_replay_cwd",
        &pch_replay_cwd};
    const cg::test::Registrar reg_pch_donor{
        "clang.pch_cross_config_donor",
        &pch_cross_config_donor};
    const cg::test::Registrar reg_pch_raw_cl{
        "clang.pch_raw_cl_producer",
        &pch_raw_cl_producer};
    const cg::test::Registrar reg_pch_nopod{
        "clang.pch_nonpod_varargs_producer",
        &pch_nonpod_varargs_producer};
    const cg::test::Registrar reg_wx_cl{
        "clang.parse_wx_cl_dialect_downgraded",
        &parse_wx_cl_dialect_downgraded};
    const cg::test::Registrar reg_gnu_gate{
        "clang.gnu_nopch_argv_injection_gate",
        &gnu_nopch_argv_injection_gate};
    const cg::test::Registrar reg_cl_dtp{
        "clang.cl_dialect_dtp_dead_template_shapes",
        &cl_dialect_dtp_dead_template_shapes};
    const cg::test::Registrar reg_cl_fixit{
        "clang.cl_lenient_specialization_fixit_rewrite",
        &cl_lenient_specialization_fixit_rewrite};
    const cg::test::Registrar reg_loader_2ph{
        "clang.loader_two_phase_equivalence",
        &loader_two_phase_equivalence};
    const cg::test::Registrar reg_pch_clsys{
        "clang.pch_cl_system_dirs",
        &pch_cl_system_dirs};
    const cg::test::Registrar reg_pch_envlang{
        "clang.pch_closure_env_language",
        &pch_closure_env_language};
    const cg::test::Registrar reg_pch_side{
        "clang.pch_replay_no_tree_pollution",
        &pch_replay_no_tree_pollution};
    const cg::test::Registrar reg_pch_deg{
        "clang.pch_replay_degraded_count",
        &pch_replay_degraded_count};
    const cg::test::Registrar reg_pch_nodata{
        "clang.pch_no_data_no_cache_dir",
        &pch_no_data_no_cache_dir};
    const cg::test::Registrar reg_pch_zero{
        "clang.pch_no_data_zero_change",
        &pch_no_data_zero_change};
    const cg::test::Registrar reg_moddb{
        "clang.compile_db_module_feeds",
        &compile_db_module_feeds};
    const cg::test::Registrar reg_vers{
        "clang.version_token",
        &version_token_extraction};
    const cg::test::Registrar reg_bmi_p1689{
        "clang.bmi_p1689_parse",
        &bmi_p1689_parse};
    const cg::test::Registrar reg_bmi_modjson{
        "clang.bmi_modules_json",
        &bmi_modules_json_parse};
    const cg::test::Registrar reg_bmi_helpers{
        "clang.bmi_helpers",
        &bmi_helper_rules};
    const cg::test::Registrar reg_bmi_manifest{
        "clang.bmi_manifest",
        &bmi_manifest_roundtrip};
    const cg::test::Registrar reg_bmi_size{
        "clang.bmi_cached_pcm_size",
        &bmi_cached_pcm_size_gate};
    const cg::test::Registrar reg_bmi_donor{
        "clang.bmi_copy_donor_verified",
        &bmi_copy_donor_verified};
    const cg::test::Registrar reg_bmi_merge{
        "clang.bmi_manifest_merge",
        &bmi_manifest_merge_preserves_entries};
    const cg::test::Registrar reg_bmi_inval{
        "clang.bmi_invalidate_artifact",
        &bmi_invalidate_cached_artifact};
    const cg::test::Registrar reg_bmi_zero{
        "clang.bmi_zero_byte_pcm",
        &bmi_zero_byte_pcm_rebuilds};
    const cg::test::Registrar reg_pch_carry{
        "clang.module_pch_field_carry",
        &module_pch_field_carry};
    const cg::test::Registrar reg_bmi_cfgkeep{
        "clang.bmi_cross_config_cleanup",
        &bmi_cross_config_cleanup_preserved};
    const cg::test::Registrar reg_pch_inval{
        "clang.pch_invalidate_artifact",
        &pch_invalidate_cached_artifact};
    const cg::test::Registrar reg_pch_atomic{
        "clang.pch_manifest_concurrent",
        &pch_manifest_concurrent_atomic_write};
    const cg::test::Registrar reg_overlay{
        "clang.export_overlay",
        &export_overlay_rules};
    const cg::test::Registrar reg_overlay_parse{
        "clang.export_overlay_parse",
        &export_overlay_parse};
    const cg::test::Registrar reg_same_line_follower{
        "clang.same_line_follower",
        &same_line_follower_parse};
    const cg::test::Registrar reg_fallback{
        "clang.fallback_argv",
        &fallback_shape};
    const cg::test::Registrar reg_m4cl{
        "clang.m4_cl_dialect",
        &m4_cl_dialect_rules};
    const cg::test::Registrar reg_import_overlay{
        "clang.import_rewrite_overlay",
        &import_rewrite_overlay_cases};
    const cg::test::Registrar reg_msvc_strip{
        "clang.msvc_module_switch_strip",
        &msvc_module_switch_strip};
    const cg::test::Registrar reg_msvc_ledger{
        "clang.msvc_module_strip_ledger_invariants",
        &msvc_module_strip_ledger_invariants};
    const cg::test::Registrar reg_fi_strip{
        "clang.module_forced_include_strip",
        &module_forced_include_strip};
    const cg::test::Registrar reg_bmi_argv{
        "clang.bmi_replay_argv",
        &bmi_replay_argv_gate};
    const cg::test::Registrar reg_clmode{
        "clang.cl_mode_supplement_gate",
        &cl_mode_supplement_gate};
    const cg::test::Registrar reg_rewire{
        "clang.supplement_stdlib_rewire_gate",
        &supplement_stdlib_rewire_gate};
    const cg::test::Registrar reg_driver{
        "clang.driver_discovery",
        &driver_discovery};
    const cg::test::Registrar reg_drain{
        "clang.driver_concurrent_drain",
        &driver_concurrent_drain};
    const cg::test::Registrar reg_baseline{
        "clang.parse_and_walk",
        &parse_and_walk_baseline};
    const cg::test::Registrar reg_broken{
        "clang.diagnostics",
        &parse_broken_reports_errors};

    // ---------------------------------------------------------------------------
    // 源编码四象限（GBK/UTF-8 交叉 有/无 BOM）：翻译层
    // 给未声明编码的 cl 方言条目追加 -finput-charset=<宿主 ANSI 代码页>。
    // 该旗标只管无 BOM 的字节流——
    // 带 BOM 的文件由其 BOM 决定。字节精确的夹具（不依赖
    // 宿主代码页）钉住全部四格。
    /// 写一个带给定 BOM 与中文字节的源文件（返回其路径）。
    [[nodiscard]]
    auto write_charset_fixture(
        const std::filesystem::path& dir,
        const std::string&           name,
        const std::string&           bom,
        const std::string&           zh_bytes) -> std::filesystem::path
    {
        const std::filesystem::path p = dir / name;
        std::ofstream               of{p, std::ios::binary | std::ios::trunc};
        of << bom << "// comment " << zh_bytes << "\n"
           << "const char* kZh = \"" << zh_bytes << "\";\n"
           << "int zh_fn();\n";
        return p;
    }

    /// 以回退 argv（可选带 -finput-charset=GBK）解析文件。
    [[nodiscard]]
    auto parse_with_charset(
        const cc::Index&             index,
        const std::filesystem::path& file,
        bool                         with_charset_flag) -> cc::ParsedTu
    {
        std::vector<std::string> argv = cc::fallback_argv();
        if (with_charset_flag)
        {
            argv.emplace_back("-finput-charset=GBK");
        }
        return cc::parse(index, file.generic_string(), argv);
    }

    /// 解析单元里首个字符串字面量的拼写。
    [[nodiscard]]
    auto first_string_literal(const cc::ParsedTu& parsed) -> std::string
    {
        std::string out;
        cc::children(
            parsed.tu.cursor(),
            [&out](const CXCursor& c)
            {
                if (cc::kind(c) == CXCursor_StringLiteral)
                {
                    out = cc::spelling(c);
                    return false;
                }
                return true;
            });
        return out;
    }

    void source_charset_quadrants(R& r)
    {
        // "中文" as GBK bytes (D6 D0 CE D4) and as UTF-8 bytes.
        const std::string zh_gbk = std::string{"\xD6\xD0\xCE\xD4", 4};
        const std::string zh_utf8 = std::string{"\xE4\xB8\xAD\xE6\x96\x87", 6};
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "ccg-clang-charset";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path gbk_nobom =
            write_charset_fixture(dir, "gbk_nobom.cpp", "", zh_gbk);
        const std::filesystem::path utf8_bom =
            write_charset_fixture(dir, "utf8_bom.cpp", "\xEF\xBB\xBF", zh_utf8);
        const std::filesystem::path gbk_bom =
            write_charset_fixture(dir, "gbk_bom.cpp", "\xEF\xBB\xBF", zh_gbk);

        const cc::Index index;

        // 引擎能力探针：构建时没带字符集转换的
        // libclang（今天的 msys2/clang64）拒绝每一个非 UTF-8 字符集值，
        // 并给出诚实的 "invalid value" 诊断。四象限随之钉住
        // 诚实失败形态而非全绿形态；有转换能力
        // 的引擎（官方 LLVM 构建）钉住完整绿矩阵。
        const auto engine_converts = [&]
        {
            const std::filesystem::path ascii_probe = dir / "probe.cpp";
            {
                std::ofstream of{
                    ascii_probe,
                    std::ios::binary | std::ios::trunc};
                of << "int probe_fn();\n";
            }
            std::vector<std::string> argv = cc::fallback_argv();
            argv.emplace_back("-finput-charset=GBK");
            const cc::ParsedTu parsed =
                cc::parse(index, ascii_probe.generic_string(), argv);
            for (const cc::Diagnostic& d : parsed.errors)
            {
                if (d.spelling.find("invalid value") != std::string::npos
                    && d.spelling.find("-finput-charset") != std::string::npos)
                {
                    return false;
                }
            }
            return true;
        }();

        if (engine_converts)
        {
            // 象限 1：GBK 字节、无 BOM、charset=GBK -> 解码成功；
            // 中文跑进字面量拼写（实测大库语料形态：
            // 旗标正是让这些文件变绿的东西）。
            const cc::ParsedTu parsed =
                parse_with_charset(index, gbk_nobom, true);
            const std::string lit = first_string_literal(parsed);
            cg::test::check(
                r,
                parsed.errors.empty() && lit.find(zh_utf8) != std::string::npos,
                "GBK no-BOM with -finput-charset=GBK parses clean and decodes "
                "the CJK literal");
            // 没有该旗标同一文件失败（默认读取器是
            // UTF-8）：追加是必要的，不是装饰。
            const cc::ParsedTu bare =
                parse_with_charset(index, gbk_nobom, false);
            cg::test::check(
                r,
                !bare.errors.empty(),
                "GBK no-BOM without a charset flag reports source "
                "decoding errors");
            // 象限 3：UTF-8 + BOM、charset=GBK -> BOM 获胜，文件
            // 保持正确（旗标不得损坏带 BOM 的文件）。
            {
                const cc::ParsedTu bom =
                    parse_with_charset(index, utf8_bom, true);
                const std::string bom_lit = first_string_literal(bom);
                cg::test::check(
                    r,
                    bom.errors.empty()
                        && bom_lit.find(zh_utf8) != std::string::npos,
                    "UTF-8 BOM file is unaffected by -finput-charset=GBK "
                    "(BOM wins)");
            }
            // 象限 4：GBK 字节前缀 UTF-8 BOM、charset=GBK ->
            // 仍按 UTF-8 读（非法字节错误）。若旗标能压过
            // BOM，此文件会按 GBK 解码成功——这个错误
            // 正是 BOM 优先的证据。
            {
                const cc::ParsedTu bom =
                    parse_with_charset(index, gbk_bom, true);
                cg::test::check(
                    r,
                    !bom.errors.empty(),
                    "BOM-prefixed GBK bytes stay on the BOM's "
                    "reading even with -finput-charset=GBK (BOM "
                    "precedence)");
            }
        }
        else
        {
            // 无转换引擎：每个带旗标解析都恰好携带
            // 该具名 invalid-value 诊断（可归因的诚实失败——
            // 绝不静默吞掉），无旗标象限展示原始
            // UTF-8 读取器行为（BOM 文件干净、GBK 字节被标记）。
            const cc::ParsedTu flagged =
                parse_with_charset(index, gbk_nobom, true);
            bool named_reject = false;
            for (const cc::Diagnostic& d : flagged.errors)
            {
                named_reject =
                    named_reject
                    || (d.spelling.find("invalid value") != std::string::npos
                        && d.spelling.find("-finput-charset")
                               != std::string::npos);
            }
            cg::test::check(
                r,
                named_reject,
                "conversion-less engine rejects the charset value with the "
                "named invalid-value diagnostic (honest failure, no silent "
                "ignore)");
            const cc::ParsedTu bom_clean =
                parse_with_charset(index, utf8_bom, false);
            cg::test::check(
                r,
                bom_clean.errors.empty(),
                "UTF-8 BOM file parses clean without any flag");
            const cc::ParsedTu gbk_bare =
                parse_with_charset(index, gbk_nobom, false);
            const std::string bare_lit = first_string_literal(gbk_bare);
            cg::test::check(
                r,
                bare_lit.find(zh_utf8) == std::string::npos,
                "GBK bytes without a flag never decode to the CJK text (the "
                "raw reader keeps the bytes verbatim; only a supported "
                "charset flag decodes them)");
        }
        std::filesystem::remove_all(dir, ec);
    }

    const cg::test::Registrar reg_charset{
        "clang.source_charset_quadrants",
        &source_charset_quadrants};

    // std 伞覆盖层闸门：覆盖层（import-std 行重写为
    // 工具链伞头 + 丢弃 std 绑定喂入）只在该 TU 的
    // P1689 规则真的涉及时才合法。仅仅
    // 文本 include STL 的 TU（stdafx 形态——整个库经
    // 头文件、零 import 行）绝不能进伞层：
    // 重写无物可替换，而喂入丢弃早已
    // 破坏解析（外场反馈形态：不 import std 的模块单元
    // 丢失 std 名字）。
    /// 记录的 cl 形态 argv（带翻译层追加的 -finput-charset=GBK）。
    [[nodiscard]]
    auto recorded_cl_argv(const std::filesystem::path& p)
        -> std::vector<std::string>
    {
        return std::vector<std::string>{
            "--driver-mode=cl",
            "/c",
            "/std:c++20",
            "-fms-compatibility",
            "--target=x86_64-pc-windows-msvc",
            "-finput-charset=GBK",
            p.string()};
    }

    /// 模块名的 P1689 require 行。
    [[nodiscard]]
    auto require_of(std::string_view n) -> cc::ScanRequire
    {
        return cc::ScanRequire{.name = std::string{n}};
    }

    void umbrella_requires_std_gate(R& r)
    {
        std::unordered_map<std::string, cc::detail::BmiNode> nodes;
        nodes.emplace("std", cc::detail::BmiNode{.name = "std"});
        nodes.emplace(
            "uses_std",
            cc::detail::BmiNode{.name = "uses_std", .direct = {"std"}});
        nodes.emplace("text_only", cc::detail::BmiNode{.name = "text_only"});
        nodes.emplace("a", cc::detail::BmiNode{.name = "a", .direct = {"b"}});
        nodes.emplace("b", cc::detail::BmiNode{.name = "b", .direct = {"std"}});
        nodes.emplace(
            "chain",
            cc::detail::BmiNode{.name = "chain", .direct = {"text_only"}});
        std::unordered_map<std::string, std::vector<std::string>> memo;
        cg::test::check(
            r,
            cc::detail::requires_std({require_of("std")}, nodes, memo),
            "a direct `import std` requires opens the gate");
        cg::test::check(
            r,
            cc::detail::requires_std({require_of("std.compat")}, nodes, memo),
            "`import std.compat` opens the gate too");
        cg::test::check(
            r,
            cc::detail::requires_std({require_of("a")}, nodes, memo),
            "a dependency chain a -> b -> std opens the gate "
            "(feeding the pcm transitively loads std.pcm)");
        cg::test::check(
            r,
            !cc::detail::requires_std({}, nodes, memo),
            "an empty P1689 rule (the stdafx text-include shape, "
            "zero imports) never opens the gate");
        cg::test::check(
            r,
            !cc::detail::requires_std({require_of("text_only")}, nodes, memo),
            "a named module whose closure never touches std keeps "
            "the gate closed");
        cg::test::check(
            r,
            !cc::detail::requires_std({require_of("chain")}, nodes, memo),
            "a chain onto std-free modules stays closed");
        cg::test::check(
            r,
            !cc::detail::requires_std(
                {require_of(R"("local.h")")},
                nodes,
                memo),
            "header-unit requires (a header path spelling) never "
            "open the gate");
        cg::test::check(
            r,
            !cc::detail::requires_std(
                {require_of(R"("local.h")"), require_of("text_only")},
                nodes,
                memo),
            "mixed requires keep the gate closed when no leg "
            "reaches std");
    }

    // 字面量字节 ANSI 编码擦洗：字符串/字符字面量内
    // 无法映射的系统代码页字节（MSVC 按原始字节放行，
    // clang 对宽/Unicode 字面量报错——"illegal character encoding"）
    // 在内存里重写成人进制转义，一次重解析。先纯函数
    // 四象限，再走真实引擎的红->绿路径。
    void encoding_scrub_literals(R& r)
    {
        const bool gbk_system =
            cg::util::platform::ansi_input_charset() == "GBK";
        // 不可映射对 0xF5 0xFB 与合法对 0xB4 0xED（GBK 的
        // "cuo"，"error code" 的首字）是代码页事实；
        // 只在成立处断言。
        const std::string bad_pair = "\xF5\xFB";
        const std::string good_pair = "\xB4\xED";
        // ---- 纯函数四象限（与代码页无关）--------------
        const std::string clean = "int ok() { return 0; }\n";
        {
            const cc::detail::ScrubOutcome res =
                cc::detail::scrub_source_encoding(clean);
            cg::test::check(
                r,
                res.text == clean && res.edits == 0,
                "a clean file is returned byte-identical with zero "
                "edits");
        }
        const std::string comment_src =
            "// comment \xF5\xFB bytes\r\nint ok() { return 0; }\r\n";
        {
            const cc::detail::ScrubOutcome res =
                cc::detail::scrub_source_encoding(comment_src);
            cg::test::check(
                r,
                res.text == comment_src && res.edits == 0,
                "bytes inside a line comment are never touched (clang "
                "does not decode them)");
        }
        const std::string block_src =
            "/* block \xF5\xFB */\r\nint ok() { return 0; }\r\n";
        {
            const cc::detail::ScrubOutcome res =
                cc::detail::scrub_source_encoding(block_src);
            cg::test::check(
                r,
                res.text == block_src && res.edits == 0,
                "bytes inside a block comment are never touched");
        }
        const std::string raw_src =
            "const wchar_t* s = LR\"x(\xF5\xFB)x\";\r\nint ok() { return 0; "
            "}\r\n";
        {
            const cc::detail::ScrubOutcome res =
                cc::detail::scrub_source_encoding(raw_src);
            cg::test::check(
                r,
                res.text == raw_src && res.edits == 0,
                "bytes inside a raw string are never touched (no "
                "escape grammar exists there; a rewrite would only "
                "corrupt the content)");
        }
        const std::string esc_src = "const char* s = \"keep \\xF5 tail\";\r\n";
        {
            const cc::detail::ScrubOutcome res =
                cc::detail::scrub_source_encoding(esc_src);
            cg::test::check(
                r,
                res.text == esc_src && res.edits == 0,
                "an authored escape sequence survives verbatim (the "
                "backslash pair is consumed atomically)");
        }
        // ---- 纯函数四象限（GBK 代码页事实）------------
        if (gbk_system)
        {
            const std::string wide =
                "const wchar_t* s = L\"a\xF5\xFB b\xB4\xED c\";\r\n";
            const cc::detail::ScrubOutcome wide_res =
                cc::detail::scrub_source_encoding(wide);
            const std::string& out = wide_res.text;
            cg::test::check(
                r,
                out.find("L\"a\\365\\373 b") != std::string::npos,
                "the unmappable pair becomes two octal escapes "
                "inside the literal");
            cg::test::check(
                r,
                out.find(good_pair) == std::string::npos
                    && out.find(" b\\264\\355 c") != std::string::npos,
                "a legal GBK pair inside a scrubbed literal "
                "escapes too (engine code-page tables diverge "
                "from the system CP936 - one self-consistent "
                "criterion, and literal contents carry no "
                "indexing semantics)");
            cg::test::check(
                r,
                wide_res.edits == 2,
                "both encoding units were scrubbed");
            const std::string narrow = "const char* s = \"\xF5\xFB\";\r\n";
            const std::string nout =
                cc::detail::scrub_source_encoding(narrow).text;
            cg::test::check(
                r,
                nout.find("\"\\365\\373\"") != std::string::npos,
                "narrow literals scrub on the same rule (one "
                "machine criterion, no wide/narrow split)");
            const std::string solo = "const char* s = \"\xF5\";\r\n";
            const std::string sout =
                cc::detail::scrub_source_encoding(solo).text;
            cg::test::check(
                r,
                sout.find("\"\\365\"") != std::string::npos,
                "a lone high byte (next byte cannot close a pair) "
                "escapes as a single unit");
            const std::string chr = "const wchar_t c = L'\xF5\xFB';\r\n";
            const std::string cout_ =
                cc::detail::scrub_source_encoding(chr).text;
            cg::test::check(
                r,
                cout_.find("L'\\365\\373'") != std::string::npos,
                "character literals scrub on the same rule");
            const std::string two = "const char* a = \"\xF5\xFB\";\r\n"
                                    "const char* b = \"\xF5\xFB\";\r\n";
            const std::size_t two_edits =
                cc::detail::scrub_source_encoding(two).edits;
            cg::test::check(
                r,
                two_edits == 2,
                "two offending sites count as two encoding units");
        }
        // ---- 引擎四象限（GBK 代码页事实，真实解析）----
        if (gbk_system)
        {
            namespace fs = std::filesystem;
            TempProj       enc{"ccg-encscrub-"};
            const fs::path bad = enc.base / "bad_wide.cpp";
            enc.put(
                bad,
                "const wchar_t* s = L\"Sample Text 01234\xF5\xFB\";\r\n"
                "int bad_wide() { return 0; }\r\n");
            const fs::path good = enc.base / "clean_gbk.cpp";
            enc.put(
                good,
                "const char* s = \"\xB4\xED\xCE\xF3\";\r\n"
                "int clean_gbk() { return 0; }\r\n");
            const fs::path raw = enc.base / "raw_residual.cpp";
            enc.put(
                raw,
                "const wchar_t* s = LR\"x(\xF5\xFB)x\";\r\n"
                "int raw_residual() { return 0; }\r\n");
            const cc::Index index;
            // 记录的 cl 形态带 -finput-charset=GBK（翻译层
            // 追加的）；引擎前端尊重它
            // （cl 驱动包装器只会警告）——这正是外场
            // 反馈的管线形态。
            // 不可映射的宽字面量：要么内存八进制转义擦洗
            // 让解析变绿（引擎尊重 argv 形态——
            // 真实管线上已证），要么引擎拒绝该 TU
            // 原始硬失败传播（ParseError；
            // 引擎形态分歧下的诚实失败）。两种结局都是
            // 契约；管线级运行钉住绿路径。
            bool engine_green = false;
            bool engine_threw = false;
            try
            {
                const cc::ParsedTu red_green =
                    cc::parse(index, bad.string(), recorded_cl_argv(bad));
                engine_green = red_green.encoding_scrub_applied
                               && red_green.errors.empty();
            }
            catch (const cc::ParseError&)
            {
                engine_threw = true;
            }
            cg::test::check(
                r,
                engine_green || engine_threw,
                "the bad wide literal ends honestly: scrubbed "
                "green (one reparse) or the original hard "
                "failure propagated (never a silent pass, never "
                "a crash)");
            bool no_scrub_trigger = false;
            try
            {
                const cc::ParsedTu no_scrub =
                    cc::parse(index, good.string(), recorded_cl_argv(good));
                no_scrub_trigger = no_scrub.encoding_scrub_applied;
            }
            catch (const cc::ParseError&)
            {
                // 引擎形态拒绝（argv 分歧，管线已证
                // 那里是绿的）：两种情况下都无物被擦洗。
            }
            cg::test::check(
                r,
                !no_scrub_trigger,
                "a file with only legal GBK pairs never triggers "
                "the scrub");
            // 原始字符串残留点：擦洗跳过原始字面量（没有
            // 转义语法），所以引擎仍拒绝该 TU，
            // 原始硬失败传播（诚实，绝不静默
            // 通过）——分类器在下游点名它。
            bool raw_threw = false;
            try
            {
                (void)cc::parse(index, raw.string(), recorded_cl_argv(raw));
            }
            catch (const cc::ParseError&)
            {
                raw_threw = true;
            }
            cg::test::check(
                r,
                raw_threw,
                "a raw-string residual site keeps the original "
                "hard failure (the scrub never invents edits "
                "inside a raw literal)");
            bool gnu_threw = false;
            try
            {
                (void)cc::parse(
                    index,
                    bad.string(),
                    {"-std=c++20", "-c", bad.string()});
            }
            catch (const cc::ParseError&)
            {
                gnu_threw = true;
            }
            cg::test::check(
                r,
                gnu_threw,
                "the GNU dialect never triggers the scrub retry "
                "(dialect gate, same rule as the fixit rewrite; "
                "the UTF-8 reader rejects the bytes on its own)");
        }
        // ---- 失败分类象限 ------------------------------
        cc::CompileCommand cl_cmd;
        cl_cmd.file = "z:\\x\\bad.cpp";
        cl_cmd.argv = {"--driver-mode=cl", "/c"};
        cc::CompileCommand gnu_cmd;
        gnu_cmd.file = "z:\\x\\bad.cpp";
        gnu_cmd.argv = {"-std=c++20", "-c"};
        const std::string spelling =
            "illegal character encoding in string literal";
        const std::string classified =
            cg::graph::detail::classify_dialect_diag_failure(spelling, cl_cmd);
        cg::test::check(
            r,
            classified.starts_with("msvc-lenient-source-encoding:"),
            "the residual spelling classifies into the named "
            "encoding class");
        cg::test::check(
            r,
            cg::graph::detail::failure_class_of(classified)
                == "msvc-lenient-source-encoding",
            "the encoding class aggregates under its own name "
            "(not parse-failed)");
        cg::test::check(
            r,
            cg::graph::detail::classify_dialect_diag_failure(spelling, gnu_cmd)
                .empty(),
            "the GNU dialect never books the encoding class "
            "(dialect gate)");
    }

    // 模块归属冲突诊断：一个声明同时归属
    // 全局模块与具名模块（生成头的
    // 双腿 include 形态：`export { #include ... }` 对模块；
    // 片段 / 第二个生成头重新声明同一接口
    // 名）——MSVC 的模块实现容忍该形态，clang
    // 以 "declaration of 'X' ... follows declaration in ..." 拒绝。
    // 三种拼写（全局先、模块先、两个具名模块）、
    // 终端折行包裹与近似诊断全部钉死；
    // 失败文本逐字携带两条项目侧修法。
    void module_attachment_conflict_diagnosis(R& r)
    {
        using cg::graph::detail::classify_module_diag_failure;
        const std::string global_first =
            "gen_idl.h(112,5): error: declaration of 'IWidget' in the global "
            "module follows declaration in module Demo.Api";
        const std::string module_first =
            "def_purview.h(3,8): error: declaration of 'Dup' in module "
            "Demo.Core follows declaration in the global module";
        const std::string two_named =
            "a.h(1,1): error: declaration of 'Thing' in module Demo.B follows "
            "declaration in module Demo.A";
        const std::string wrapped =
            "gen_idl.h(112,5): error: declaration of 'IWidget' in the global "
            "module\n    follows declaration in module Demo.Api";
        cg::test::check(
            r,
            cc::is_module_attachment_conflict(global_first),
            "global-module-last spelling detects");
        cg::test::check(
            r,
            cc::is_module_attachment_conflict(module_first),
            "global-module-first spelling detects");
        cg::test::check(
            r,
            cc::is_module_attachment_conflict(two_named),
            "two named modules spelling detects");
        cg::test::check(
            r,
            cc::is_module_attachment_conflict(wrapped),
            "terminal-wrapped fold still detects (anchors are "
            "line-break independent)");
        cg::test::check(
            r,
            !cc::is_module_attachment_conflict(
                "def_purview.h(3,8): error: redefinition of 'Dup'"),
            "a plain redefinition (same attachment, e.g. GMF header vs "
            "purview header both defining) never matches");
        cg::test::check(
            r,
            !cc::is_module_attachment_conflict(
                "fatal error: 'fwd_gmf.h' file not found"),
            "unrelated first errors never match");
        cg::test::check(
            r,
            !cc::is_module_attachment_conflict(""),
            "empty text never matches");
        const std::string named =
            cc::module_attachment_conflict_failure(module_first, "Demo.Core");
        cg::test::check(
            r,
            named.starts_with("module-attachment-conflict:"),
            "the named failure text leads with its class");
        cg::test::check(
            r,
            named.find("module 'Demo.Core' vs") != std::string::npos,
            "a known module name is named in the text");
        cg::test::check(
            r,
            named.find("`module;` global fragment") != std::string::npos
                && named.find("`export using`") != std::string::npos
                && named.find("exactly once inside the purview")
                       != std::string::npos,
            "both project-side fixes ride along (module; fragment + export "
            "using; single include inside the purview)");
        const std::string anonymous =
            cc::module_attachment_conflict_failure(global_first, {});
        cg::test::check(
            r,
            anonymous.find(" vs ") == std::string::npos,
            "an empty module name keeps the text unnamed");
        cc::CompileCommand cmd;
        cmd.argv = {"clang-cl", "--driver-mode=cl", "/c"};
        const std::string classified =
            classify_module_diag_failure(module_first, 1, cmd);
        cg::test::check(
            r,
            classified.starts_with("module-attachment-conflict:"),
            "the consumer-side classifier books the named class");
        cg::test::check(
            r,
            cg::graph::detail::failure_class_of(classified)
                == "module-attachment-conflict",
            "the class aggregates under its own name (not parse-failed)");
        cg::test::check(
            r,
            !cg::graph::detail::is_transient_failure_class(
                "module-attachment-conflict"),
            "the conflict is input-determined: retrying the same sources "
            "cannot fix it (no transient class)");
    }

    const cg::test::Registrar reg_gate{
        "clang.umbrella_requires_std_gate",
        &umbrella_requires_std_gate};

    const cg::test::Registrar reg_conflict{
        "clang.module_attachment_conflict_diagnosis",
        &module_attachment_conflict_diagnosis};

    // two-stdlib-clash 判据的三道收紧（FooBiz 误判回归）：无投喂
    // 在场的 TU 物理上不可能两套实体相撞（前置门，判据入口处）；ambiguous
    // 只认 ODR 特异拼写 reference to '<名>' is ambiguous——conversion/
    // call 的重载解析歧义是方言差异，不含该前缀；阈值 6→10（真混合 TU 是
    // 几十条起的重定义风暴，旧下界 6 被 FooBiz 恰好六条转换歧义
    // 证伪）。三条独立成立，任一条单独也拦得住原案。
    void two_stdlib_clash_gating(R& r)
    {
        using cg::graph::detail::classify_module_diag_failure;
        // FooBiz 原案：无任何 pcm 投喂、恰好六条 conversion 歧义。
        cc::CompileCommand no_feeds;
        no_feeds.argv = {"clang-cl", "--driver-mode=cl", "/c"};
        const std::string conversion =
            "FooBiz.cpp(73,12): error: conversion from 'const LONG' "
            "(aka 'const long') to 'Value' is ambiguous";
        cg::test::check(
            r,
            classify_module_diag_failure(conversion, 6, no_feeds).empty(),
            "no module feeds in play: the conversion-ambiguous overload "
            "failure never books two-stdlib-clash (the FooBiz false "
            "positive)");
        cg::test::check(
            r,
            classify_module_diag_failure(
                "type_traits(314): error: reference to "
                "'integral_constant' is ambiguous",
                22,
                no_feeds)
                .empty(),
            "the feed gate leads the branch: even the ODR spelling with a "
            "diagnostic storm stays unclassified when no pcm was fed");
        cg::test::check(
            r,
            classify_module_diag_failure(
                "xutility(231): error: redefinition of '_Char_traits'",
                22,
                no_feeds)
                .empty(),
            "a redefinition storm with no feeds also falls through (two "
            "standard libraries cannot clash without a pcm in play)");
        // 真阳性保命：被喂了 std 的 TU + ODR 特异拼写 + 风暴级错误数。
        cc::CompileCommand fed;
        fed.argv = {"clang-cl", "--driver-mode=cl", "/c"};
        fed.module_feeds.push_back(
            cc::ModuleFeed{.name = "std", .pcm = "build/std.pcm"});
        cg::test::check(
            r,
            classify_module_diag_failure(
                "type_traits(314): error: reference to "
                "'integral_constant' is ambiguous",
                22,
                fed)
                .starts_with("two-stdlib-clash:"),
            "a fed TU with the ODR spelling and a diagnostic storm still "
            "books the clash class (true positive survives)");
        cg::test::check(
            r,
            classify_module_diag_failure(
                "xutility(231): error: redefinition of '_Char_traits'",
                22,
                fed)
                .starts_with("two-stdlib-clash:"),
            "the redefinition spelling keeps its slot for fed TUs");
        cg::test::check(
            r,
            classify_module_diag_failure(conversion, 22, fed).empty(),
            "conversion-ambiguous stays out even for a fed TU with many "
            "errors (overload resolution is a dialect matter, not an ODR "
            "clash)");
        cg::test::check(
            r,
            classify_module_diag_failure(
                "type_traits(314): error: reference to "
                "'integral_constant' is ambiguous",
                6,
                fed)
                .empty(),
            "six errors never reach the threshold of ten (the storm shape "
            "of a real mixed TU starts in the dozens)");
        cg::test::check(
            r,
            cg::graph::detail::failure_class_of(conversion) == "parse-failed",
            "the conversion failure aggregates under parse-failed (a "
            "classic dialect diagnostic, not a module class)");
    }

    const cg::test::Registrar reg_two_stdlib{
        "clang.two_stdlib_clash_gating",
        &two_stdlib_clash_gating};

    /// 向节点表加一个 BMI 节点（默认接口单元形态）。
    void add_bmi_node(
        std::unordered_map<std::string, cc::detail::BmiNode>& nodes,
        std::string                                           name,
        std::vector<std::string>                              direct,
        bool                                                  interface = true)
    {
        cc::detail::BmiNode n;
        n.name = name;
        n.source = name + ".ixx";
        n.interface = interface;
        n.direct = std::move(direct);
        nodes.emplace(name, std::move(n));
    }

    // Kahn 分层（BMI 层内并行构建的拓扑依据）：宽图同层、逐依赖加深、
    // 层内名字序、环上节点落选任何层、std 伪节点不进度数——纯函数用例，
    // 无引擎、无文件系统。
    void bmi_topo_layers(R& r)
    {
        std::unordered_map<std::string, cc::detail::BmiNode> nodes;
        // 宽深混合 DAG：L0{a,b,c,s}（s 只依赖 std 伪节点，度数为零）；
        // L1{d,e}（d←a,b；e←b,c）；L2{f}（f←d,e）；环 g↔h 落选。
        add_bmi_node(nodes, "a", {});
        add_bmi_node(nodes, "b", {});
        add_bmi_node(nodes, "c", {});
        add_bmi_node(nodes, "d", {"a", "b"});
        add_bmi_node(nodes, "e", {"b", "c"});
        add_bmi_node(nodes, "f", {"d", "e"});
        add_bmi_node(nodes, "g", {"h"});
        add_bmi_node(nodes, "h", {"g"});
        add_bmi_node(nodes, "std", {}, false);
        add_bmi_node(nodes, "s", {"std"});
        const std::vector<std::string>
            names{"a", "b", "c", "d", "e", "f", "g", "h", "s"};
        const std::vector<std::vector<std::string>> layers =
            cc::detail::topo_layers(nodes, names);
        cg::test::check(
            r,
            layers.size() == 3,
            "the mixed DAG cuts into exactly three layers");
        if (layers.size() == 3)
        {
            const std::vector<std::string> want0{"a", "b", "c", "s"};
            const std::vector<std::string> want1{"d", "e"};
            const std::vector<std::string> want2{"f"};
            cg::test::check(
                r,
                layers[0] == want0,
                "layer 0 holds the zero-degree interfaces in name order (the "
                "std pseudo-node never counts as a dependency, so s joins "
                "layer 0)");
            cg::test::check(
                r,
                layers[1] == want1,
                "layer 1 holds d/e in name order");
            cg::test::check(r, layers[2] == want2, "layer 2 holds f alone");
        }
        std::unordered_set<std::string> flat;
        for (const std::vector<std::string>& layer : layers)
        {
            flat.insert(layer.begin(), layer.end());
        }
        cg::test::check(
            r,
            !flat.contains("g") && !flat.contains("h"),
            "cycle members never join any layer (the caller marks "
            "them module-dag-cycle via the flattened set)");
        cg::test::check(
            r,
            flat.contains("s"),
            "an interface depending only on the std pseudo-node "
            "still layers normally");

        // 深链形态：层数=链长、每层恰一节点（层宽 1 是并行收益封顶的
        // 结构性形态，分层不得把链上节点并进同层）。
        std::unordered_map<std::string, cc::detail::BmiNode> chain;
        cc::detail::BmiNode                                  c0;
        c0.name = "c0";
        c0.source = "c0.ixx";
        c0.interface = true;
        chain.emplace("c0", c0);
        for (int i = 1; i < 4; ++i)
        {
            cc::detail::BmiNode n;
            n.name = "c" + std::to_string(i);
            n.source = n.name + ".ixx";
            n.interface = true;
            n.direct = {"c" + std::to_string(i - 1)};
            chain.emplace(n.name, std::move(n));
        }
        const std::vector<std::string> chain_names{"c0", "c1", "c2", "c3"};
        const std::vector<std::vector<std::string>> chain_layers =
            cc::detail::topo_layers(chain, chain_names);
        cg::test::check(
            r,
            chain_layers.size() == 4,
            "a pure chain layers one node per layer");
        bool chain_ok = chain_layers.size() == 4;
        for (std::size_t i = 0; chain_ok && i < chain_layers.size(); ++i)
        {
            chain_ok = chain_layers[i].size() == 1
                       && chain_layers[i].front() == chain_names[i];
        }
        cg::test::check(
            r,
            chain_ok,
            "each chain layer holds exactly its own node in "
            "dependency order");
    }

    // 实例化跳过开关（CCG_SKIP_INSTANTIATIONS，TU_Prefix 形态）的纯
    // 判定门：默认关、精确值 "1" 开、cl 方言、显式延迟模板解析语料与
    // *-windows-msvc 三元组条目永久禁用（延迟语料下 TU_Prefix 不
    // parse 模板 pattern 体）。
    void instantiation_skip_gate(R& r)
    {
        using Opt = std::optional<std::string>;
        const std::vector<std::string> gnu_argv{
            "-x",
            "c++",
            "-std=c++20",
            "-DFOO=1"};

        cg::test::check(
            r,
            !cc::detail::instantiation_skip_allowed(std::nullopt, gnu_argv),
            "an unset CCG_SKIP_INSTANTIATIONS keeps the complete form");
        cg::test::check(
            r,
            !cc::detail::instantiation_skip_allowed(Opt{"0"}, gnu_argv),
            "CCG_SKIP_INSTANTIATIONS=0 keeps the complete form");
        cg::test::check(
            r,
            !cc::detail::instantiation_skip_allowed(Opt{"true"}, gnu_argv),
            "only the exact value 1 enables the skip form");
        cg::test::check(
            r,
            cc::detail::instantiation_skip_allowed(Opt{"1"}, gnu_argv),
            "CCG_SKIP_INSTANTIATIONS=1 on a GNU-dialect argv enables the "
            "skip form");

        std::vector<std::string> cl_argv = gnu_argv;
        cl_argv.emplace_back("--driver-mode=cl");
        cg::test::check(
            r,
            !cc::detail::instantiation_skip_allowed(Opt{"1"}, cl_argv),
            "the cl dialect is permanently excluded even with the switch "
            "on (late-parsed template bodies would never parse)");

        std::vector<std::string> delayed_on_argv = gnu_argv;
        delayed_on_argv.emplace_back("-fdelayed-template-parsing");
        cg::test::check(
            r,
            !cc::detail::instantiation_skip_allowed(Opt{"1"}, delayed_on_argv),
            "an explicit -fdelayed-template-parsing argv is excluded");

        std::vector<std::string> delayed_off_argv = gnu_argv;
        delayed_off_argv.emplace_back("-fno-delayed-template-parsing");
        cg::test::check(
            r,
            cc::detail::instantiation_skip_allowed(Opt{"1"}, delayed_off_argv),
            "an explicit -fno-delayed-template-parsing argv stays "
            "eligible (eager parsing is the safe direction)");

        // GNU 拼写 + *-windows-msvc 三元组：驱动器在低于 C++20 时自动
        // 注入 -fdelayed-template-parsing（cc1 装配期，argv 层不可见），
        // 门必须按三元组放行前拦下（保守判据：与标准版本无关）。
        std::vector<std::string> msvc_joined_argv{
            "-x",
            "c++",
            "-std=c++17",
            "--target=x86_64-pc-windows-msvc"};
        cg::test::check(
            r,
            !cc::detail::instantiation_skip_allowed(Opt{"1"}, msvc_joined_argv),
            "a joined --target=*-windows-msvc argv is excluded (the "
            "driver injects delayed parsing invisible at the argv layer)");

        std::vector<std::string> msvc_split_argv{
            "-x",
            "c++",
            "-std=c++17",
            "-target",
            "x86_64-pc-windows-msvc"};
        cg::test::check(
            r,
            !cc::detail::instantiation_skip_allowed(Opt{"1"}, msvc_split_argv),
            "a split -target <msvc-triple> argv is excluded like the "
            "joined spelling");

        std::vector<std::string> msvc_modern_argv{
            "-x",
            "c++",
            "-std=c++23",
            "--target=aarch64-pc-windows-msvc"};
        cg::test::check(
            r,
            !cc::detail::instantiation_skip_allowed(Opt{"1"}, msvc_modern_argv),
            "an msvc-target argv is excluded regardless of the standard "
            "version (conservative criterion, no -std parsing)");

        std::vector<std::string> mingw_argv{
            "-x",
            "c++",
            "-std=c++17",
            "--target=x86_64-w64-windows-gnu"};
        cg::test::check(
            r,
            cc::detail::instantiation_skip_allowed(Opt{"1"}, mingw_argv),
            "a mingw *-windows-gnu target triple has no driver-injected "
            "delayed parsing and stays eligible");

        std::vector<std::string> lookalike_argv{
            "-x",
            "c++",
            "-std=c++20",
            "-target-cpu=x86-64"};
        cg::test::check(
            r,
            cc::detail::instantiation_skip_allowed(Opt{"1"}, lookalike_argv),
            "lookalike flags such as -target-cpu do not satisfy the "
            "exact -target token match");
    }

    const cg::test::Registrar reg_topo{
        "clang.bmi_topo_layers",
        &bmi_topo_layers};

    const cg::test::Registrar reg_scrub{
        "clang.encoding_scrub_literals",
        &encoding_scrub_literals};

    const cg::test::Registrar reg_header{
        "clang.header_fallback",
        &header_only_fallback_parse};

    const cg::test::Registrar reg_skip_gate{
        "clang.instantiation_skip_gate",
        &instantiation_skip_gate};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }
