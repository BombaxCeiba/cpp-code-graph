module;
// 本单元直接调用 cg.msbuild 的 capture 读写（capture_db_insert /
// read_wrap_log 等导出内联函数，其 sqlpp23 语句构造在调用方单元实例
// 化表模板）：MSVC 要求表模型定义在本单元的全局模块片段可见，否则
// IFC 读取器在跨模块合并模板实例时崩溃（与 src/db/db.tables.h 同一
// 探针矩阵结论，机理详见 capture.tables.h 注释）。
#include "../src/msbuild/capture.tables.h"

export module cg.test.msbuild;

// MSBuild 层的单元 + golden 闸门：cl 模式编译命令
// 适配器（cg.msbuild.translate）、wrap 模式
// 纯件（引号处理、JSON 转义——进程级闸门在
// tests/msbuild-wrap.cmake），以及管线辅助（配置枚举、
// cl 拼接、输出解析、注册表往返；活发现为手工
// 记录）、求值通道（发现链文本规则、会话驱动、
// 刷新快照存取与哈希门判定）。golden 数据是已入库的
// 研究 clwrap 日志副本与由其再生的 cl 模式
// 编译命令；比较是纯文本、跨平台。
// CCG_MSBUILD_FIXTURE_DIR 由构建注入。

import std;
import cg.util;
import cg.util.cstd;
import cg.clang;
import cg.msbuild;
import cg.msbuild.compose;
import cg.cli.parse;
import cg.cli.msbuild;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace mb = cg::msbuild;
    namespace ev = cg::msbuild::eval;
    namespace cc = cg::clang;
    namespace json = cg::util::json;

    constexpr std::string_view fixture_dir = CCG_MSBUILD_FIXTURE_DIR;

    [[nodiscard]]
    auto fixture(std::string_view name) -> std::string
    {
        return std::string{fixture_dir} + "/" + std::string{name};
    }

    [[nodiscard]]
    auto read_file(const std::string& path) -> std::string
    {
        std::ifstream in{std::filesystem::path{path}, std::ios::binary};
        if (!in)
        {
            throw std::runtime_error("cannot open fixture: " + path);
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    /** @brief 往已存在的目录写文本文件（手写夹具源文件落地）。 */
    void put_text(const std::filesystem::path& p, std::string_view text)
    {
        std::ofstream out{p, std::ios::binary};
        out << text;
    }

    // 把一条 clwrap JSONL 日志解析成适配器输入调用。
    [[nodiscard]]
    auto load_log(const std::string& path) -> std::vector<mb::ClInvocation>
    {
        std::vector<mb::ClInvocation> out;
        std::istringstream            lines{read_file(path)};
        std::string                   line;
        while (std::getline(lines, line))
        {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos)
            {
                continue;
            }
            const json::Document  doc = json::Document::parse(line);
            const json::ValueView root = doc.root();
            const auto            argv_view = root.get("argv");
            if (!argv_view || !argv_view->is_array())
            {
                continue;
            }
            mb::ClInvocation  inv;
            const std::size_t n = argv_view->size();
            for (std::size_t i = 0; i < n; ++i)
            {
                if (const auto v = argv_view->at(i); v && v->is_string())
                {
                    inv.argv_utf8.emplace_back(*v->as_string());
                }
            }
            if (const auto cwd = root.get("cwd"); cwd && cwd->is_string())
            {
                inv.cwd = std::string{*cwd->as_string()};
            }
            if (const auto inc = root.get("INCLUDE"); inc && inc->is_string())
            {
                inv.include_env = std::string{*inc->as_string()};
            }
            if (const auto cl = root.get("real_cl"); cl && cl->is_string())
            {
                inv.real_cl = std::string{*cl->as_string()};
            }
            out.push_back(std::move(inv));
        }
        return out;
    }

    // 解析 golden compile_commands.json（arguments 形态）。
    // 入库语料（expected/ 下的 jsonl 与 json）提交前必须把
    // 真实仓库根路径归一化为 X:/ccg-fake 占位根，不得携带
    // 机器路径。
    [[nodiscard]]
    auto load_golden(const std::string& path) -> std::vector<mb::CdbEntry>
    {
        const json::Document  doc = json::Document::parse(read_file(path));
        const json::ValueView root = doc.root();
        if (!root.is_array())
        {
            throw std::runtime_error("golden is not a JSON array: " + path);
        }
        std::vector<mb::CdbEntry> out;
        const std::size_t         count = root.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto entry_view = root.at(i);
            if (!entry_view || !entry_view->is_object())
            {
                continue;
            }
            const json::ValueView entry = *entry_view;
            mb::CdbEntry          e;
            if (const auto v = entry.get("directory"); v && v->is_string())
            {
                e.directory = std::string{*v->as_string()};
            }
            if (const auto v = entry.get("file"); v && v->is_string())
            {
                e.file = std::string{*v->as_string()};
            }
            if (const auto args = entry.get("arguments");
                args && args->is_array())
            {
                const std::size_t n = args->size();
                for (std::size_t j = 0; j < n; ++j)
                {
                    if (const auto v = args->at(j); v && v->is_string())
                    {
                        e.arguments.emplace_back(*v->as_string());
                    }
                }
            }
            out.push_back(std::move(e));
        }
        return out;
    }

    [[nodiscard]]
    auto adapt_log(const std::string& log_path) -> std::vector<mb::CdbEntry>
    {
        std::vector<mb::CdbEntry> out;
        for (const mb::ClInvocation& inv : load_log(log_path))
        {
            std::vector<mb::CdbEntry> entries = mb::adapt_cl(inv);
            out.insert(
                out.end(),
                std::make_move_iterator(entries.begin()),
                std::make_move_iterator(entries.end()));
        }
        return out;
    }

    // 适配输出对 golden 的有序逐字段比较。
    void check_entries(
        R&                               r,
        const std::vector<mb::CdbEntry>& got,
        const std::vector<mb::CdbEntry>& want,
        std::string_view                 label)
    {
        cg::test::check(
            r,
            got.size() == want.size(),
            std::format(
                "{}: entry count {} == {}",
                label,
                got.size(),
                want.size()));
        const std::size_t n = std::min(got.size(), want.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            cg::test::check(
                r,
                got[i].directory == want[i].directory,
                std::format(
                    "{}: entry {} directory '{}' == '{}'",
                    label,
                    i,
                    got[i].directory,
                    want[i].directory));
            cg::test::check(
                r,
                got[i].file == want[i].file,
                std::format(
                    "{}: entry {} file '{}' == '{}'",
                    label,
                    i,
                    got[i].file,
                    want[i].file));
            cg::test::check(
                r,
                got[i].arguments == want[i].arguments,
                std::format(
                    "{}: entry {} argv differs:\n  got:  {}\n  want: {}",
                    label,
                    i,
                    [&]
                    {
                        std::string s;
                        for (const std::string& a : got[i].arguments)
                        {
                            s += a + ' ';
                        }
                        return s;
                    }(),
                    [&]
                    {
                        std::string s;
                        for (const std::string& a : want[i].arguments)
                        {
                            s += a + ' ';
                        }
                        return s;
                    }()));
        }
    }

    [[nodiscard]]
    auto contains(const std::vector<std::string>& argv, std::string_view token)
        -> bool
    {
        return std::ranges::find(argv, token) != argv.end();
    }

    [[nodiscard]]
    auto has_prefix_token(
        const std::vector<std::string>& argv,
        std::string_view                prefix) -> bool
    {
        return std::ranges::any_of(
            argv,
            [&](const std::string& t) { return t.starts_with(prefix); });
    }

    // 系统代码页不是 UTF-8 的机器上适配器会追加
    // -finput-charset=<宿主 ANSI 代码页>（MSVC 源编码对齐；声明
    // 过的条目保持原样）。该令牌是本机相关的：golden 保持
    // 平台中立，所以比较先把它从适配输出剥掉，
    // 再断言宿主字符集已知时每条恰带一个。
    [[nodiscard]]
    auto strip_local_input_charset(R& r, std::vector<mb::CdbEntry> entries)
        -> std::vector<mb::CdbEntry>
    {
        const std::string local = cg::util::platform::ansi_input_charset();
        for (mb::CdbEntry& e : entries)
        {
            const std::size_t hits = std::erase_if(
                e.arguments,
                [](const std::string& t)
                { return t.starts_with("-finput-charset="); });
            cg::test::check(
                r,
                hits == (local.empty() ? 0u : 1u),
                std::format(
                    "one -finput-charset append per "
                    "charset-declaring entry (host '{}', "
                    "hits {})",
                    local.empty() ? std::string{"none"} : local,
                    hits));
        }
        return entries;
    }

    // ---------------------------------------------------------------------------

    // 手工构造调用上 5.2 编辑的形态：注入、原样
    // 保留、产物/PCH 丢弃、include 绝对化（拼接与分开）、
    // 批拆成每 TU 一条。
    void adapt_shape(R& r)
    {
        mb::ClInvocation inv;
        inv.cwd = "E:/proj";
        inv.argv_utf8 = {
            "cl.exe",
            "/c",
            "/Iinclude",
            "/I",
            "src",
            "/std:c++20",
            "/EHsc",
            "/GR-",
            "/Foobj/",
            "/Fdpdb",
            "/Fppch.pch",
            "/Ycpch.h",
            "/Yupch.h",
            "/FIforce.h",
            "/external:Iext",
            "/Gd",
            "/TP",
            "src\\main.cpp",
            "src\\util.cpp",
            "notes.txt",
        };
        const std::vector<mb::CdbEntry> out = mb::adapt_cl(inv);

        cg::test::check(
            r,
            out.size() == 2,
            std::format("batch split: 2 entries, got {}", out.size()));
        if (out.size() != 2)
        {
            return;
        }
        for (const mb::CdbEntry& e : out)
        {
            cg::test::check(
                r,
                e.directory == "E:/proj",
                "directory passthrough");
            cg::test::check(
                r,
                e.arguments.size() >= 2 && e.arguments[0] == "clang-cl"
                    && e.arguments[1] == "--driver-mode=cl",
                "clang-cl + --driver-mode=cl injected as the first tokens");
            cg::test::check(
                r,
                contains(e.arguments, "/std:c++20"),
                "/std:c++20 kept verbatim");
            cg::test::check(
                r,
                contains(e.arguments, "/EHsc"),
                "/EHsc kept verbatim");
            cg::test::check(
                r,
                contains(e.arguments, "/GR-"),
                "/GR- kept verbatim");
            cg::test::check(
                r,
                contains(e.arguments, "/c"),
                "/c kept verbatim (harmless in cl mode)");
            cg::test::check(
                r,
                contains(e.arguments, "/Gd") && contains(e.arguments, "/TP"),
                "/Gd /TP kept verbatim");
            cg::test::check(
                r,
                contains(e.arguments, "/IE:/proj/include"),
                "joined /I absolutized against cwd");
            cg::test::check(
                r,
                contains(e.arguments, "/I")
                    && contains(e.arguments, "E:/proj/src"),
                "separate /I kept two-token with absolute value");
            cg::test::check(
                r,
                contains(e.arguments, "/FIE:/proj/force.h"),
                "joined /FI preserved (t2) and absolutized");
            cg::test::check(
                r,
                contains(e.arguments, "/external:IE:/proj/ext"),
                "joined /external:I absolutized");
            for (const std::string_view drop :
                 {"/Fo", "/Fd", "/Fp", "/Yc", "/Yu"})
            {
                cg::test::check(
                    r,
                    !has_prefix_token(e.arguments, drop),
                    std::format("no {}* survives", drop));
            }
            cg::test::check(
                r,
                !contains(e.arguments, "src\\main.cpp")
                    && !contains(e.arguments, "src\\util.cpp")
                    && !contains(e.arguments, "notes.txt"),
                "positional inputs do not survive in argv");
        }
        cg::test::check(
            r,
            out[0].file == "E:/proj/src/main.cpp",
            "first TU of the batch, separators normalized");
        cg::test::check(
            r,
            out[1].file == "E:/proj/src/util.cpp",
            "second TU of the batch");
        cg::test::check(
            r,
            out[0].arguments == out[1].arguments,
            "batched TUs share identical flags");
        cg::test::check(
            r,
            !contains(out[0].arguments, "notes.txt"),
            "non-C/C++ positional dropped (orphan-input defense)");
    }

    // 硬失败黑名单区分大小写：/Gf /u- 在 cl 模式下
    // 实测 rc=4 被丢弃；合法兄弟 /GF /u 保持原样。
    void adapt_blacklist_case(R& r)
    {
        mb::ClInvocation inv;
        inv.cwd = "E:/proj";
        inv.argv_utf8 =
            {"cl.exe", "/Gf", "/u-", "/GF", "/u", "/GF-", "src\\main.cpp"};
        const std::vector<mb::CdbEntry> out = mb::adapt_cl(inv);
        cg::test::check(r, out.size() == 1, "one TU entry");
        if (out.size() != 1)
        {
            return;
        }
        cg::test::check(
            r,
            !contains(out[0].arguments, "/Gf"),
            "blacklist /Gf dropped (g4: rc=4)");
        cg::test::check(
            r,
            !contains(out[0].arguments, "/u-"),
            "blacklist /u- dropped (z8: rc=4)");
        cg::test::check(
            r,
            contains(out[0].arguments, "/GF"),
            "valid /GF kept (case is significant)");
        cg::test::check(
            r,
            contains(out[0].arguments, "/u"),
            "valid /u kept (case is significant)");
        cg::test::check(
            r,
            contains(out[0].arguments, "/GF-"),
            "valid /GF- kept");
    }

    /// 以 E:/proj 为工作目录的 cl 适配（测试侧单点）。
    [[nodiscard]]
    auto adapt_cl_argv(const std::vector<std::string>& argv)
        -> std::vector<mb::CdbEntry>
    {
        mb::ClInvocation inv;
        inv.cwd = "E:/proj";
        inv.argv_utf8 = argv;
        return mb::adapt_cl(inv);
    }

    // 模板模式等效旗标：MSVC 默认（延迟解析）翻译为 /Zc:twoPhase- 补在
    // 条目末尾、写进 compile_commands.json 可见；工程显式表态过（严格或
    // 延迟）的条目原样保留绝不代言；大小写不敏感。
    void adapt_two_phase_equivalence(R& r)
    {
        const std::vector<mb::CdbEntry> plain =
            adapt_cl_argv({"cl.exe", "/c", "src\\main.cpp"});
        cg::test::check(
            r,
            plain.size() == 1 && !plain.empty()
                && contains(plain[0].arguments, "/Zc:twoPhase-"),
            "undeclared template mode gets /Zc:twoPhase- appended "
            "(visible in the cdb entry)");
        if (!plain.empty())
        {
            cg::test::check(
                r,
                plain[0].arguments.back() == "/Zc:twoPhase-",
                "the equivalence flag is the LAST argument");
        }
        const std::vector<mb::CdbEntry> strict1 =
            adapt_cl_argv({"cl.exe", "/c", "/permissive-", "src\\main.cpp"});
        cg::test::check(
            r,
            strict1.size() == 1
                && !contains(strict1[0].arguments, "/Zc:twoPhase-"),
            "explicit /permissive- is translated verbatim, no "
            "equivalence append");
        const std::vector<mb::CdbEntry> strict2 =
            adapt_cl_argv({"cl.exe", "/c", "/Zc:twoPhase", "src\\main.cpp"});
        cg::test::check(
            r,
            strict2.size() == 1
                && !contains(strict2[0].arguments, "/Zc:twoPhase-"),
            "explicit /Zc:twoPhase stays strict, no append");
        const std::vector<mb::CdbEntry> declared =
            adapt_cl_argv({"cl.exe", "/c", "/Zc:twoPhase-", "src\\main.cpp"});
        if (declared.size() == 1)
        {
            std::size_t n = 0;
            for (const std::string& a : declared[0].arguments)
            {
                if (a == "/Zc:twoPhase-")
                {
                    ++n;
                }
            }
            cg::test::check(
                r,
                n == 1,
                "explicit /Zc:twoPhase- is kept exactly once "
                "(no duplicate append)");
        }
        const std::vector<mb::CdbEntry> ci =
            adapt_cl_argv({"cl.exe", "/c", "/PERMISSIVE-", "src\\main.cpp"});
        cg::test::check(
            r,
            ci.size() == 1 && !contains(ci[0].arguments, "/Zc:twoPhase-"),
            "case-insensitive: /PERMISSIVE- counts as declared");
    }

    // 非 C/C++ 调用绝不抵达 libclang：工具基名过滤、无
    // C/C++ 位置实参、空 argv。
    void adapt_non_cxx(R& r)
    {
        mb::ClInvocation rc;
        rc.cwd = "E:/proj";
        rc.argv_utf8 = {"rc.exe", "/fo", "res.res", "app.rc"};
        cg::test::check(
            r,
            mb::adapt_cl(rc).empty(),
            "rc.exe tool basename drops the invocation");

        mb::ClInvocation midl;
        midl.cwd = "E:/proj";
        midl.argv_utf8 = {"C:/tools/midl.exe", "/iid", "obj.idl"};
        cg::test::check(
            r,
            mb::adapt_cl(midl).empty(),
            "midl.exe tool basename drops the invocation");

        mb::ClInvocation no_tu;
        no_tu.cwd = "E:/proj";
        no_tu.argv_utf8 = {"cl.exe", "/c", "/Iinc", "app.rc", "app.idl"};
        cg::test::check(
            r,
            mb::adapt_cl(no_tu).empty(),
            "cl invocation without a C/C++ TU drops the entry");

        mb::ClInvocation empty;
        empty.cwd = "E:/proj";
        cg::test::check(
            r,
            mb::adapt_cl(empty).empty(),
            "empty argv adapts to zero entries");

        mb::ClInvocation trailing_flag;
        trailing_flag.cwd = "E:/proj";
        trailing_flag.argv_utf8 = {"cl.exe", "/I", "src\\main.cpp"};
        const std::vector<mb::CdbEntry> out = mb::adapt_cl(trailing_flag);
        // "/I" 把源文件当它的值吞掉：无条目、但不崩也
        // 无孤儿位置实参。
        cg::test::check(
            r,
            out.empty(),
            "separate /I at argv end consumes its value token");
    }

    // 响应文件防御性展开（BOM 自检、嵌套 @、
    // CRT 引号规则）；任何 '@' 令牌都不幸存进条目。
    void adapt_rsp(R& r)
    {
        mb::ClInvocation inv;
        inv.cwd = fixture("rsp");
        inv.argv_utf8 = {"cl.exe", "@test.rsp", "@test_utf16.rsp"};
        const std::vector<mb::CdbEntry> out = mb::adapt_cl(inv);

        cg::test::check(
            r,
            out.size() == 1,
            std::format("one TU from the rsp, got {}", out.size()));
        if (out.size() != 1)
        {
            return;
        }
        cg::test::check(
            r,
            out[0].file == fixture("rsp/src/main.cpp"),
            "rsp positional becomes the entry file");
        const std::vector<std::string>& argv = out[0].arguments;
        cg::test::check(
            r,
            std::ranges::none_of(
                argv,
                [](const std::string& t) { return t.starts_with('@'); }),
            "no @ token survives expansion");
        cg::test::check(
            r,
            contains(argv, "/DSPACE DEF=2"),
            "quoted rsp define survives as one joined token (CRT "
            "rules; /D + value is spliced into a single token)");
        cg::test::check(
            r,
            contains(argv, "/DNESTED=1"),
            "nested response file expanded recursively");
        cg::test::check(
            r,
            contains(argv, "/DUTF16=1"),
            "UTF-16LE BOM response file decoded");
        cg::test::check(r, contains(argv, "/c"), "rsp flags carried over");
        cg::test::check(
            r,
            contains(argv, "/I" + fixture("expected")),
            "relative rsp include absolutized against the invocation cwd");
        cg::test::check(
            r,
            !has_prefix_token(argv, "/Fo"),
            "rsp artifact /Fo dropped after expansion");

        // 读不了的响应文件：大声失败，绝不静默丢弃。
        mb::ClInvocation missing;
        missing.cwd = fixture("rsp");
        missing.argv_utf8 = {"cl.exe", "@does-not-exist.rsp", "src\\main.cpp"};
        bool threw = false;
        try
        {
            static_cast<void>(mb::adapt_cl(missing));
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        cg::test::check(
            r,
            threw,
            "missing response file throws instead of dropping");
    }

    // golden（Debug）：适配器对已入库研究日志是确定性的
    // ——每 TU 的 argv 逐条等于 golden。
    void clwrap_log_debug_golden(R& r)
    {
        const std::vector<mb::CdbEntry> got = strip_local_input_charset(
            r,
            adapt_log(fixture("expected/clwrap-Debug.jsonl")));
        const std::vector<mb::CdbEntry> want =
            load_golden(fixture("expected/compile_commands-Debug-clmode.json"));
        check_entries(r, got, want, "clwrap-log Debug golden");
        bool ndebug = false;
        for (const mb::CdbEntry& e : got)
        {
            ndebug = ndebug || contains(e.arguments, "NDEBUG");
        }
        cg::test::check(r, !ndebug, "Debug golden carries no NDEBUG define");
    }

    // golden（Release）：Release 日志上同样确定性；它唯一
    // 的定义差异 NDEBUG 在场。
    void clwrap_log_release_golden(R& r)
    {
        const std::vector<mb::CdbEntry> got = strip_local_input_charset(
            r,
            adapt_log(fixture("expected/clwrap-Release.jsonl")));
        const std::vector<mb::CdbEntry> want = load_golden(
            fixture("expected/compile_commands-Release-clmode.json"));
        check_entries(r, got, want, "clwrap-log Release golden");
        bool ndebug = false;
        for (const mb::CdbEntry& e : got)
        {
            ndebug = ndebug || contains(e.arguments, "/DNDEBUG");
        }
        cg::test::check(r, ndebug, "Release golden carries NDEBUG (/DNDEBUG)");
    }

    // 双工程 golden：双工程解决方案日志逐字保留各工程的 /std
    // （mathlib c++17、msbdemo c++20）横跨批条目。
    void clwrap_log_sln_std(R& r)
    {
        const std::vector<mb::CdbEntry> got = strip_local_input_charset(
            r,
            adapt_log(fixture("expected/clwrap-sln-Debug.jsonl")));
        const std::vector<mb::CdbEntry> want = load_golden(
            fixture("expected/compile_commands-sln-Debug-clmode.json"));
        check_entries(r, got, want, "clwrap-log sln golden");

        std::size_t mathlib = 0;
        std::size_t msbdemo = 0;
        for (const mb::CdbEntry& e : got)
        {
            const bool is_math = e.file.find("mathlib") != std::string::npos;
            const bool is_demo = e.file.find("msbdemo") != std::string::npos;
            cg::test::check(
                r,
                is_math || is_demo,
                "every sln entry belongs to a known project");
            if (is_math)
            {
                ++mathlib;
                cg::test::check(
                    r,
                    contains(e.arguments, "/std:c++17"),
                    "mathlib TU keeps /std:c++17");
            }
            if (is_demo)
            {
                ++msbdemo;
                cg::test::check(
                    r,
                    contains(e.arguments, "/std:c++20"),
                    "msbdemo TU keeps /std:c++20");
            }
        }
        cg::test::check(
            r,
            mathlib >= 1 && msbdemo >= 1,
            std::format(
                "both projects present (mathlib {}, "
                "msbdemo {})",
                mathlib,
                msbdemo));
    }

    // 捕获到的 INCLUDE 环境按捕获顺序完整物化：
    // 工具集、VS Aux、Windows SDK 与 NETFXSDK 根不再被丢
    // （丢掉它们会让 argv 不自含：重放静默用
    // clang-cl 自动发现）。插入 `/X` 让进程 INCLUDE 与
    // 自动发现无法掺和。原有 `/X` 意味着 cl 无视
    // INCLUDE——适配器绝不能把那些目录加回来。兼容
    /// 仅工具集/SDK 的 INCLUDE 捕获串（MSVC 14.51 + Win10 SDK 形态）。
    constexpr std::string_view k_toolset_include_env =
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\"
        "Tools\\MSVC\\14.51.36231\\include;;C:\\Program Files\\Microsoft "
        "Visual Studio\\18\\Community\\VC\\Tools\\MSVC\\"
        "14.51.36231\\atlmfc\\include;C:\\Program Files\\Microsoft Visual "
        "Studio\\18\\Community\\VC\\Auxiliary\\VS\\include;C:\\Program "
        "Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\ucrt;C:\\"
        "Program Files (x86)\\Windows Kits\\10\\Include\\10.0.26100.0\\um;"
        "C:\\Program Files (x86)\\Windows Kits\\NETFXSDK\\4.8\\Include\\um";

    /// 混合 INCLUDE 串（工具集/SDK + 工程专属目录三形态）。
    [[nodiscard]]
    auto mixed_include_env() -> std::string
    {
        return std::string{k_toolset_include_env}
               + R"(;my include;E:\vs\DIA SDK\\include;C:/my msvc/include)";
    }

    // 版本来自每次调用的 real_cl 路径。
    void adapt_include_env(R& r)
    {
        constexpr std::string_view toolset_only = k_toolset_include_env;

        // 仅工具集/SDK 的 INCLUDE：每条都物化；`/X` 让
        // argv 自含。输出必须不同于无 INCLUDE 的情形。
        mb::ClInvocation with_env;
        with_env.cwd = "E:/proj";
        with_env.argv_utf8 = {"cl.exe", "/c", "/std:c++20", "src\\main.cpp"};
        with_env.include_env = std::string{toolset_only};
        with_env.real_cl =
            "C:/Program Files/Microsoft Visual Studio/18/Community/VC/"
            "Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe";
        mb::ClInvocation without_env = with_env;
        without_env.include_env.clear();
        const std::vector<mb::CdbEntry> a = mb::adapt_cl(with_env);
        const std::vector<mb::CdbEntry> b = mb::adapt_cl(without_env);
        cg::test::check(r, a.size() == 1 && b.size() == 1, "one TU entry each");
        if (a.size() == 1 && b.size() == 1)
        {
            cg::test::check(
                r,
                a[0].arguments != b[0].arguments,
                "captured INCLUDE changes the argv (self-contained)");
            cg::test::check(
                r,
                contains(a[0].arguments, "/X"),
                "/X is inserted for a self-contained argv");
            cg::test::check(
                r,
                contains(
                    a[0].arguments,
                    "-fms-compatibility-version=19.51.36231"),
                "full four-part toolset version becomes 19.minor.build");
            std::vector<std::string> all_fed;
            for (std::size_t i = 1; i < a[0].arguments.size(); ++i)
            {
                if (a[0].arguments[i - 1] == "-imsvc")
                {
                    all_fed.push_back(a[0].arguments[i]);
                }
            }
            cg::test::check(
                r,
                all_fed.size() == 6,
                std::format(
                    "all 6 toolset/SDK roots materialized, got {}",
                    all_fed.size()));
            cg::test::check(
                r,
                !all_fed.empty() && all_fed[0].ends_with("14.51.36231/include")
                    && all_fed.back().ends_with("NETFXSDK/4.8/Include/um"),
                "captured order preserved (toolset first, NETFXSDK last)");
        }

        // 混合 INCLUDE：每条按捕获顺序、绝对化，
        // 包括工程专属的。
        mb::ClInvocation mixed;
        mixed.cwd = "E:/proj";
        mixed.argv_utf8 = {"cl.exe", "/c", "/I", "src", "src\\main.cpp"};
        mixed.real_cl = with_env.real_cl;
        mixed.include_env = mixed_include_env();
        const std::vector<mb::CdbEntry> out = mb::adapt_cl(mixed);
        cg::test::check(r, out.size() == 1, "one TU entry (mixed)");
        if (out.size() != 1)
        {
            return;
        }
        const std::vector<std::string>& argv = out[0].arguments;
        std::vector<std::string>        fed;
        for (std::size_t i = 1; i < argv.size(); ++i)
        {
            if (argv[i - 1] == "-imsvc")
            {
                fed.emplace_back(argv[i]);
            }
        }
        const std::vector<std::string> want = {
            "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/"
            "MSVC/14.51.36231/include",
            "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/"
            "MSVC/14.51.36231/atlmfc/include",
            "C:/Program Files/Microsoft Visual Studio/18/Community/VC/"
            "Auxiliary/VS/include",
            "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/ucrt",
            "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um",
            "C:/Program Files (x86)/Windows Kits/NETFXSDK/4.8/Include/um",
            "E:/proj/my include",
            "E:/vs/DIA SDK/include",
            "C:/my msvc/include"};
        cg::test::check(
            r,
            fed == want,
            std::format(
                "all dirs fed in order: got [{}], want [{}]",
                [&]
                {
                    std::string s;
                    for (const std::string& f : fed)
                    {
                        s += f + "; ";
                    }
                    return s;
                }(),
                [&]
                {
                    std::string s;
                    for (const std::string& w : want)
                    {
                        s += w + "; ";
                    }
                    return s;
                }()));
        cg::test::check(
            r,
            !has_prefix_token(argv, "/imsvc"),
            "space form only - the /imsvc: colon spelling never appears");
        const std::size_t first_imsvc = [&]
        {
            const auto it = std::ranges::find(argv, "-imsvc");
            return static_cast<std::size_t>(it - argv.begin());
        }();
        const std::size_t last_flag = [&]
        {
            std::size_t at = 1;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                if (argv[i] == "/Zc:twoPhase-")
                {
                    continue; // ccg 的末尾等效旗标补齐，不是记录旗标
                }
                if (argv[i].starts_with('/') || argv[i].starts_with("--"))
                {
                    at = i;
                }
            }
            return at;
        }();
        cg::test::check(
            r,
            first_imsvc > last_flag,
            "-imsvc pairs sit after every recorded flag (the "
            "trailing /Zc:twoPhase- equivalence append excluded)");
        cg::test::check(
            r,
            argv.back() == "/Zc:twoPhase-",
            "undeclared template mode: the equivalence flag is "
            "the final argument");
    }

    // INCLUDE 与 /X 抑制、多工具集版本与 INCLUDE 对 .sln 级 real_cl 的
    // 胜出、批内共享目录：全部对着同一捕获串形态。
    void adapt_include_env_suppression_and_toolsets(R& r)
    {
        constexpr std::string_view toolset_only = k_toolset_include_env;
        mb::ClInvocation           with_env;
        with_env.cwd = "E:/proj";
        with_env.argv_utf8 = {"cl.exe", "/c", "/std:c++20", "src\\main.cpp"};
        with_env.include_env = std::string{toolset_only};
        with_env.real_cl =
            "C:/Program Files/Microsoft Visual Studio/18/Community/VC/"
            "Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe";

        // 原有 `/X`：cl 早已无视 INCLUDE，所以任何目录都不得回来
        // （且原旗标不重复）。
        mb::ClInvocation ignores;
        ignores.cwd = "E:/proj";
        ignores.argv_utf8 = {"cl.exe", "/c", "/X", "src\\main.cpp"};
        ignores.include_env = std::string{toolset_only};
        ignores.real_cl = with_env.real_cl;
        const std::vector<mb::CdbEntry> original_x = mb::adapt_cl(ignores);
        cg::test::check(
            r,
            original_x.size() == 1,
            "one TU entry (original /X)");
        if (original_x.size() == 1)
        {
            const std::vector<std::string>& xargv = original_x[0].arguments;
            cg::test::check(
                r,
                !contains(xargv, "-imsvc"),
                "original /X: captured INCLUDE is NOT re-introduced");
            cg::test::check(
                r,
                std::ranges::count(xargv, "/X") == 1,
                "original /X kept exactly once");
            cg::test::check(
                r,
                contains(xargv, "-fms-compatibility-version=19.51.36231"),
                "compatibility version is still pinned under /X");
        }

        // cl 接受同一选项的 `/x`、`-X` 与 `-x`（大小写不敏感、
        // '-' 前缀）。全都必须抑制 INCLUDE 重物化
        // 且不得多出第二个 /X。
        for (const char* spelling : {"/x", "-X", "-x"})
        {
            mb::ClInvocation variant;
            variant.cwd = "E:/proj";
            variant.argv_utf8 = {"cl.exe", "/c", spelling, "src\\main.cpp"};
            variant.include_env = std::string{toolset_only};
            variant.real_cl = with_env.real_cl;
            const std::vector<mb::CdbEntry> adapted = mb::adapt_cl(variant);
            cg::test::check(
                r,
                adapted.size() == 1,
                std::string{"one TU entry (original "} + spelling + ")");
            if (adapted.size() == 1)
            {
                const std::vector<std::string>& vargv = adapted[0].arguments;
                cg::test::check(
                    r,
                    !contains(vargv, "-imsvc"),
                    std::string{spelling}
                        + ": captured INCLUDE is NOT re-introduced");
                cg::test::check(
                    r,
                    std::ranges::count(vargv, "/X") == 1
                        && !contains(vargv, spelling),
                    std::string{spelling}
                        + ": canonicalized to a single /X for replay");
                cg::test::check(
                    r,
                    contains(vargv, "-fms-compatibility-version=19.51.36231"),
                    std::string{spelling}
                        + ": compatibility version still pinned");
            }
        }

        // 混合工具集：两次调用、两个兼容值。每 TU 的
        // INCLUDE 是权威（.sln 级 real_cl 对两者
        // 相同）。
        mb::ClInvocation newer = with_env;
        mb::ClInvocation older = with_env;
        older.real_cl =
            "C:/Program Files/Microsoft Visual Studio/18/Community/VC/"
            "Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe";
        older.include_env = [&]
        {
            std::string       env{toolset_only};
            const std::string from = "14.51.36231";
            const std::string to = "14.44.35207";
            for (std::size_t at = env.find(from); at != std::string::npos;
                 at = env.find(from, at + to.size()))
            {
                env.replace(at, from.size(), to);
            }
            return env;
        }();
        const std::vector<mb::CdbEntry> a_new = mb::adapt_cl(newer);
        const std::vector<mb::CdbEntry> a_old = mb::adapt_cl(older);
        cg::test::check(
            r,
            a_new.size() == 1 && a_old.size() == 1,
            "two invocations adapt");
        if (a_new.size() == 1 && a_old.size() == 1)
        {
            cg::test::check(
                r,
                contains(
                    a_new[0].arguments,
                    "-fms-compatibility-version=19.51.36231")
                    && contains(
                        a_old[0].arguments,
                        "-fms-compatibility-version=19.44.35207"),
                "mixed toolset: per-invocation compatibility versions");
        }

        // .sln 级 real_cl 对每个工程都是最高工具集；
        // 意见不合时每 TU 的 INCLUDE 必须胜（本闸门前
        // projB 的 14.44 头文件一直被 19.51 解析）。
        mb::ClInvocation conflict = with_env;
        conflict.real_cl =
            "C:/Program Files/Microsoft Visual Studio/18/Community/VC/"
            "Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe";
        conflict.include_env = older.include_env;
        const std::vector<mb::CdbEntry> cdb_conflict = mb::adapt_cl(conflict);
        cg::test::check(
            r,
            cdb_conflict.size() == 1
                && contains(
                    cdb_conflict[0].arguments,
                    "-fms-compatibility-version=19.44.35207")
                && !contains(
                    cdb_conflict[0].arguments,
                    "-fms-compatibility-version=19.51.36231"),
            "INCLUDE-derived toolset wins over the .sln-wide real_cl");

        // INCLUDE 与 real_cl 都无工具集线索：无兼容
        // 旗标（CLI 会发 verbose 提示，纯适配器不会）。
        mb::ClInvocation unknown = with_env;
        unknown.real_cl = "cl.exe";
        unknown.include_env.clear();
        const std::vector<mb::CdbEntry> no_compat = mb::adapt_cl(unknown);
        cg::test::check(
            r,
            no_compat.size() == 1,
            "unparseable real_cl still adapts");
        if (no_compat.size() == 1)
        {
            cg::test::check(
                r,
                !has_prefix_token(
                    no_compat[0].arguments,
                    "-fms-compatibility-version="),
                "no compatibility flag when the path has no toolset version");
        }

        // real_cl 单独不可用时 INCLUDE 仍钉住版本。
        mb::ClInvocation include_only = with_env;
        include_only.real_cl = "cl.exe";
        const std::vector<mb::CdbEntry> from_include =
            mb::adapt_cl(include_only);
        cg::test::check(
            r,
            from_include.size() == 1
                && contains(
                    from_include[0].arguments,
                    "-fms-compatibility-version=19.51.36231"),
            "INCLUDE toolset version is used when real_cl has no clue");

        // 批内 TU 共享同一批 INCLUDE 推导目录。
        mb::ClInvocation batched;
        batched.cwd = "E:/proj";
        batched.argv_utf8 =
            {"cl.exe", "/c", "/I", "src", "src\\main.cpp", R"(src\util.cpp)"};
        batched.real_cl = with_env.real_cl;
        batched.include_env = mixed_include_env();
        const std::vector<mb::CdbEntry> batch = mb::adapt_cl(batched);
        cg::test::check(
            r,
            batch.size() == 2 && batch[0].arguments == batch[1].arguments,
            "batched TUs share the INCLUDE-derived dirs");
    }

    // 适配器是纯函数：输入相同，输出全同。
    void adapt_determinism(R& r)
    {
        const std::vector<mb::ClInvocation> log =
            load_log(fixture("expected/clwrap-Debug.jsonl"));
        for (const mb::ClInvocation& inv : log)
        {
            const std::vector<mb::CdbEntry> a = mb::adapt_cl(inv);
            const std::vector<mb::CdbEntry> b = mb::adapt_cl(inv);
            cg::test::check(r, a.size() == b.size(), "stable entry count");
            for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
            {
                cg::test::check(
                    r,
                    a[i].file == b[i].file && a[i].directory == b[i].directory
                        && a[i].arguments == b[i].arguments,
                    "identical output on re-run");
            }
        }
    }

    // ------------------------------------------------------ 日志 +
    // cdb ---

    // read_wrap_log: capture.db（SQLite capture 表）是唯一数据源。好坏行
    // 混合：合法 invocation 行带 argv/cwd/INCLUDE/real_cl 完整往返，error
    // 行单独成列，argv_json 损坏的坏行计数并继续（坏数据是信号不是异
    // 常）；CCG_CLWRAP_LOG 的 jsonl 旧拼写经与写侧同一函数推导出 db 位
    // 置；缺库读作空捕获（调用方上报零捕获）。
    void pipeline_wrap_log(R& r)
    {
        namespace fs = std::filesystem;
        const fs::path root =
            fs::temp_directory_path() / "ccg-msbuild-log-test";
        std::error_code ec;
        fs::remove_all(root, ec);
        // 真实布局形态：`<root>`/.ccg/msbuild/`<C>`-`<P>`/clwrap-`<token>`.jsonl
        // 的 env 值推导到
        // `<root>`/.ccg/`<C>`-`<P>`/capture.db——写读两侧唯一真相。
        const fs::path cfg_dir = root / ".ccg" / "Debug-x64";
        const fs::path env_dir = root / ".ccg" / "msbuild" / "Debug-x64";
        fs::create_directories(cfg_dir, ec);
        fs::create_directories(env_dir, ec);
        const std::string db = (cfg_dir / "capture.db").string();

        mb::detail::CaptureRow good;
        good.argv_json = R"(["clwrap-Debug.jsonl","/c","src\\main.cpp"])";
        good.cwd = "E:\\p";
        good.include_env = "E:\\inc";
        good.real_cl = "E:\\vs\\bin\\cl.exe";
        cg::test::check(
            r,
            mb::detail::capture_db_insert(db, "Debug.jsonl", good),
            "invocation row inserts");
        mb::detail::CaptureRow err;
        err.is_error = true;
        err.message = "ccg wrap: something failed";
        cg::test::check(
            r,
            mb::detail::capture_db_insert(db, "Debug.jsonl", err),
            "error row inserts");
        mb::detail::CaptureRow bad;
        bad.argv_json = "not json at all";
        bad.cwd = "E:\\q";
        cg::test::check(
            r,
            mb::detail::capture_db_insert(db, "Debug.jsonl", bad),
            "corrupt row inserts (bad data is a signal, not fatal)");

        // jsonl env 值形态：经 capture_db_of_log_path 落到同一 capture.db。
        const mb::WrapLog log =
            mb::read_wrap_log((env_dir / "clwrap-Debug.jsonl").string());
        cg::test::check(
            r,
            log.invocations.size() == 1,
            std::format(
                "one invocation survives, got {}",
                log.invocations.size()));
        if (!log.invocations.empty())
        {
            const mb::ClInvocation& inv = log.invocations.front();
            cg::test::check(
                r,
                inv.argv_utf8.size() == 3
                    && inv.argv_utf8[0].find("clwrap") != std::string::npos,
                "argv[0] is the wrapper token");
            cg::test::check(r, inv.cwd == "E:\\p", "cwd round-trips verbatim");
            cg::test::check(
                r,
                inv.include_env == "E:\\inc",
                "INCLUDE env captured");
            cg::test::check(
                r,
                inv.real_cl.find("cl.exe") != std::string::npos,
                "real_cl rides along per invocation");
        }
        cg::test::check(
            r,
            log.errors.size() == 1
                && log.errors[0].find("something failed") != std::string::npos,
            "error row surfaces as data, not an exception");
        cg::test::check(
            r,
            log.bad_rows == 1,
            "corrupt argv_json counted and skipped, never thrown");

        // db 直接路径同库同读（读侧兼容直传 capture.db 的调用面）。
        const mb::WrapLog direct = mb::read_wrap_log(db);
        cg::test::check(
            r,
            direct.invocations.size() == 1 && direct.errors.size() == 1
                && direct.bad_rows == 1,
            "direct db path reads the same capture");

        // 逐 token 过滤（纵深防御）：prune 因句柄占用/ACL 删不掉旧库时，
        // 非空 token 只读本运行的行，上一轮的行绝不混入翻译；空 token
        // （默认）仍读全表。db_path 一并带出（零捕获报错指路用）。
        mb::detail::CaptureRow stale;
        stale.argv_json = R"(["clwrap-Old.jsonl","/c","old.cpp"])";
        stale.cwd = "E:\\old";
        cg::test::check(
            r,
            mb::detail::capture_db_insert(db, "Old.jsonl", stale),
            "stale-token row inserts");
        const std::string env_log = (env_dir / "clwrap-Debug.jsonl").string();
        const mb::WrapLog mine = mb::read_wrap_log(env_log, "Debug.jsonl");
        cg::test::check(
            r,
            mine.invocations.size() == 1,
            std::format(
                "token filter isolates this run's rows, "
                "got {}",
                mine.invocations.size()));
        cg::test::check(
            r,
            mine.db_path == mb::detail::capture_db_of_log_path(env_log),
            "db path rides along for zero-capture diagnostics");
        const mb::WrapLog every = mb::read_wrap_log(db);
        cg::test::check(
            r,
            every.invocations.size() == 2,
            "empty token still reads the whole table");
        if (every.invocations.size() == 2)
        {
            cg::test::check(
                r,
                every.invocations.back().include_env.empty(),
                "unset INCLUDE persists as NULL and reads back as empty");
        }

        cg::test::check(
            r,
            mb::read_wrap_log((root / "absent" / "absent.jsonl").string())
                .invocations.empty(),
            "missing db reads as empty (caller reports the capture)");
        fs::remove_all(root, ec);
    }

    // write_compile_commands 产出的恰是加载器的 arguments 形态，
    // 原始 UTF-8 路径与带引号 argv 经解析往返幸存。
    void pipeline_write_cdb(R& r)
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "ccg-msbuild-cdb-test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        // 写入者必须自建嵌套缓存目录（首跑布局）。
        const std::string path =
            (dir / ".ccg" / "msbuild" / "Debug-x64" / "compile_commands.json")
                .string();

        mb::CdbEntry e;
        e.directory = "E:/proj root";
        // 非 ASCII 路径字节拼成转义：源文件保持纯 ASCII
        // 而字符串携带真 UTF-8（scale.cpp 已知限制
        // 形态）。
        e.file = "E:/proj root/src-h\xc3\xbc"
                 "n\xc3\xaf"
                 "code/scale.cpp";
        e.arguments =
            {"clang-cl", "--driver-mode=cl", "/DSPACE=1", "/DQUOTED=\"a b\""};
        mb::write_compile_commands(path, {e});
        cg::test::check(
            r,
            fs::exists(fs::path{path}),
            "nested cdb path created and written");
        cg::test::check(
            r,
            !fs::exists(fs::path{path + ".tmp"}),
            "atomic replace leaves no .tmp");

        const json::Document  doc = json::Document::parse(read_file(path));
        const json::ValueView root = doc.root();
        cg::test::check(
            r,
            root.is_array() && root.size() == 1,
            "top-level array with one entry");
        if (root.is_array() && root.size() == 1)
        {
            const auto entry = root.at(0);
            cg::test::check(
                r,
                entry
                    && entry->get("directory")->as_string()
                           == std::string_view{"E:/proj root"}
                    && entry->get("file")->as_string()->find("scale.cpp")
                           != std::string_view::npos,
                "directory/file survive with spaces and UTF-8");
            const auto args = entry->get("arguments");
            cg::test::check(
                r,
                args && args->is_array() && args->size() == 4
                    && *args->at(0)->as_string() == std::string_view{"clang-cl"}
                    && *args->at(3)->as_string()
                           == std::string_view{"/DQUOTED=\"a b\""},
                "arguments array round-trips quotes verbatim");
        }
        fs::remove_all(dir, ec);
    }

    // MSBuild PCH 全链（不依赖 MSBuild 进程）：wrap-log 形状的 ClInvocation
    // 流程 classify_cl_pch/adapt_cl → write_compile_commands +
    // write_pch_sidecar → cg.clang 加载 sidecar → plan_pch 构建影子并改写
    // consumer。覆盖 sidecar 的 argv/cdb_sha256/producers/consumers
    // 写盘与消费两端； MSBuild 驱动本身（-t:Rebuild 捕获）不在本测试范围。
    void pipeline_pch_sidecar_chain(R& r)
    {
        namespace fs = std::filesystem;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "MSBuild PCH chain skipped: no clang driver");
            return;
        }
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path dir =
            fs::temp_directory_path()
            / ("ccg-msbuild-pch-chain-" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string root = cg::util::norm_path(dir.generic_string());
        put_text(dir / "pch.h", "#pragma once\nstruct MsbPch { int v; };\n");
        put_text(dir / "pch.cpp", "#include \"pch.h\"\n");
        put_text(
            dir / "main.cpp",
            "#include \"pch.h\"\nint main(){MsbPch p{1};return p.v;}\n");

        // --- 捕获形状（wrap log 的 invocations[] 元素）---
        mb::ClInvocation prod;
        prod.cwd = root;
        prod.argv_utf8 = {
            "clang-cl",
            "--driver-mode=cl",
            "/c",
            "/I",
            root,
            "/Ycpch.h",
            "/Fp" + root + "/app.pch",
            root + "/pch.cpp"};
        mb::ClInvocation cons;
        cons.cwd = root;
        cons.argv_utf8 = {
            "clang-cl",
            "--driver-mode=cl",
            "/c",
            "/I",
            root,
            "/Yupch.h",
            "/Fp" + root + "/app.pch",
            root + "/main.cpp"};

        // 与 cli.msbuild 相同的分类/适配/记录构造路径。
        const mb::PchClassification pcls = mb::classify_cl_pch(prod);
        const mb::PchClassification ccls = mb::classify_cl_pch(cons);
        cg::test::check(
            r,
            pcls.is_producer && !pcls.sources.empty() && !pcls.pch_key.empty(),
            "captured /Yc invocation classifies as a producer");
        cg::test::check(
            r,
            ccls.pch_use && ccls.pch_key == pcls.pch_key,
            "captured /Yu invocation classifies as a consumer");

        std::vector<mb::CdbEntry>          entries;
        std::vector<mb::PchProducerRecord> producers;
        std::vector<mb::PchConsumerRecord> consumers;
        mb::PchProducerRecord              prec;
        prec.pch_key = pcls.pch_key;
        prec.source = mb::detail::to_abs(pcls.sources.front(), prod.cwd);
        prec.umbrella = pcls.pch_header;
        prec.cwd = root;
        for (const mb::CdbEntry& e : mb::adapt_cl(prod))
        {
            entries.push_back(e);
            if (e.file == prec.source)
            {
                prec.argv = e.arguments;
            }
        }
        prec.argv.push_back("/Yc" + pcls.pch_header);
        prec.argv.push_back("/Fp" + prec.pch_key);
        std::string canonical;
        for (const std::string& a : prec.argv)
        {
            canonical += a + "\n";
        }
        canonical += prec.cwd;
        prec.command_digest =
            cg::util::sha256_hex(std::as_bytes(std::span{canonical}));
        producers.push_back(prec);
        for (const mb::CdbEntry& e : mb::adapt_cl(cons))
        {
            entries.push_back(e);
            mb::PchConsumerRecord rec;
            rec.file = e.file;
            rec.pch_key = ccls.pch_key;
            rec.pch_header = ccls.pch_header;
            std::string ccanonical = e.directory + "\n";
            for (const std::string& a : e.arguments)
            {
                ccanonical += a + "\n";
            }
            rec.command_digest =
                cg::util::sha256_hex(std::as_bytes(std::span{ccanonical}));
            consumers.push_back(rec);
        }

        const std::string cdb = root + "/compile_commands.json";
        const std::string sidecar = root + "/pch.json";
        mb::write_compile_commands(cdb, entries);
        mb::write_pch_sidecar(
            sidecar,
            cdb,
            "Debug",
            "x64",
            producers,
            consumers,
            {});
        cg::test::check(
            r,
            fs::is_regular_file(sidecar, ec) && fs::is_regular_file(cdb, ec),
            "sidecar and cdb are written together");
        const json::Document  doc = json::Document::parse(read_file(sidecar));
        const json::ValueView sroot = doc.root();
        const auto            sha = sroot.get("cdb_sha256");
        const auto            plist = sroot.get("producers");
        const auto            clist = sroot.get("consumers");
        cg::test::check(
            r,
            sha && sha->is_string()
                && *sha->as_string() == cg::util::sha256_file_hex(cdb) && plist
                && plist->is_array() && plist->size() == 1 && clist
                && clist->is_array() && clist->size() == 1,
            "pch.json carries cdb_sha256 and non-empty "
            "producers/consumers");

        // --- 消费端：cg.clang 加载 cdb + sidecar 并规划影子 ---
        const auto     db = cc::load(cdb, {});
        cc::PchOptions opt;
        opt.ccg_config_dir = root + "/.ccg/Debug";
        opt.compile_db_path = cdb;
        opt.sidecar_path = sidecar;
        const cc::PchPlanResult first = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            first.built == 1 && first.text_fallback == 0,
            "sidecar-driven plan builds the shadow (not text "
            "fallback)");
        bool shadow = false;
        for (const cc::CompileCommand& c : first.commands)
        {
            if (c.file == root + "/main.cpp")
            {
                shadow = c.pch.status == cc::PchUse::Status::shadow;
            }
        }
        cg::test::check(
            r,
            shadow,
            "consumer consumes the shadow built from the sidecar");
        const cc::PchPlanResult second = cc::plan_pch(db, db, opt);
        cg::test::check(
            r,
            second.reused == 1 && second.built == 0,
            "second plan reuses the sidecar-built shadow");
        fs::remove_all(dir, ec);
    }

    // ------------------------------------------------------------- wrap
    // 包装 ----

    // ASCII 宽令牌的窄拷贝（仅往返夹具；真实
    // wrap 路径全程保持宽字符）。
    [[nodiscard]]
    auto narrow(const std::wstring& wide) -> std::string
    {
        return std::string{wide.begin(), wide.end()};
    }

    // 命令行引号规则是 MSVC CRT 分词器的精确逆，覆盖
    // wrap 模式可转发的每种令牌形态。
    void wrap_quote_roundtrip(R& r)
    {
        const std::vector<std::wstring> tokens = {
            L"",
            L"plain",
            L"a b",
            L"he said \"hi\"",
            L"C:\\path with space\\",
            L"a\\b\\\"c",
            L"tab\there",
            L"/DQUOTED=a b",
            L"trailing\\",
            L"\\\"leading quote"};
        for (const std::wstring& token : tokens)
        {
            const std::wstring quoted =
                cg::util::platform::quote_for_command_line(token);
            const std::vector<std::string> reparsed =
                cg::util::tokenize_command(narrow(quoted));
            cg::test::check(
                r,
                reparsed.size() == 1 && reparsed[0] == narrow(token),
                std::format(
                    "quote round-trip: '{}' -> '{}' -> '{}'",
                    narrow(token),
                    narrow(quoted),
                    reparsed.empty() ? std::string{"<none>"} : reparsed[0]));
        }
    }

    // 门面写出的 argv JSON 经真实解析器往返，
    // 原始 UTF-8 不转义地穿过，并保留旧 jsonl 时代
    // 手写器的 \u00XX 控制字节形态（clwrap.c 形态）。
    void wrap_json_escape(R& r)
    {
        const std::string    value = std::string{"a\"b\\c\nd\x01"
                                                 "\te "
                                                 "\xC3\xBC"
                                                 "n\xC3\xAF"
                                                 "code-\xCE\xA9"};
        const std::string    text = mb::detail::argv_to_json({value, "plain"});
        const json::Document doc = json::Document::parse(text);
        const auto           parsed = doc.root();
        cg::test::check(
            r,
            parsed.is_array() && parsed.size() == 2,
            "argv json parses as a two-element array");
        const auto first = parsed.at(0);
        cg::test::check(
            r,
            first && first->is_string(),
            "escaped element parses as a JSON string");
        if (first && first->is_string())
        {
            cg::test::check(
                r,
                *first->as_string() == value,
                "escaped value round-trips through the parser");
        }
        cg::test::check(
            r,
            text.find("\\u0001") != std::string::npos,
            "control chars escape as \\u00XX");
        cg::test::check(
            r,
            text.find("xC3") == std::string::npos, // no double escaping
            "multi-byte UTF-8 passes through raw");
        cg::test::check(
            r,
            mb::detail::argv_to_json({"plain"}) == "[\"plain\"]",
            "single plain element keeps the compact array shape");
        cg::test::check(
            r,
            mb::detail::argv_to_json({}) == "[]",
            "empty argv serializes as an empty array");
    }

    // ---------------------------------------------------------
    /// argv 里是否存在（旗标, 值）相邻对。
    [[nodiscard]]
    auto argv_has_flag_value(
        const std::vector<std::string>& argv,
        std::string_view                flag,
        std::string_view                value) -> bool
    {
        for (std::size_t i = 0; i + 1 < argv.size(); ++i)
        {
            if (argv[i] == flag && argv[i + 1] == value)
            {
                return true;
            }
        }
        return false;
    }

    // 管线 -------

    /// 配置枚举的 Debug/Release|x64 两态断言。
    void expect_debug_release_configs(
        R&                                     rr,
        const std::vector<mb::ConfigPlatform>& got,
        std::string_view                       label)
    {
        cg::test::check(
            rr,
            got.size() == 2,
            std::format("{}: 2 configs, got {}", label, got.size()));
        if (got.size() == 2)
        {
            cg::test::check(
                rr,
                got[0].configuration == "Debug" && got[0].platform == "x64",
                std::format("{}: first is Debug|x64", label));
            cg::test::check(
                rr,
                got[1].configuration == "Release" && got[1].platform == "x64",
                std::format("{}: second is Release|x64", label));
        }
    }

    // 已入库研究工作区上的配置枚举：
    // sln 与每个 vcxproj 都恰好声明 {Debug|x64, Release|x64}，
    // 按声明顺序、去重。
    void pipeline_enum_configs(R& r)
    {
        expect_debug_release_configs(
            r,
            mb::enumerate_configs(fixture("DemoSln.sln")),
            "sln");
        expect_debug_release_configs(
            r,
            mb::enumerate_configs(fixture("msbdemo/msbdemo.vcxproj")),
            "msbdemo vcxproj");
        expect_debug_release_configs(
            r,
            mb::enumerate_configs(fixture("mathlib/mathlib/mathlib.vcxproj")),
            "mathlib vcxproj");

        cg::test::check(
            r,
            mb::enumerate_configs(fixture("no-such.sln")).empty(),
            "missing input enumerates nothing");
        cg::test::check(
            r,
            mb::enumerate_configs(fixture("rsp/test.rsp")).empty(),
            "non-project input enumerates nothing");
        cg::test::check(
            r,
            mb::config_stem(
                mb::ConfigPlatform{.configuration = "Debug", .platform = "x64"})
                == "Debug-x64",
            "config stem maps | to -");
    }

    // cl.exe 拼接：每个目标都是 HostX86 宿主（binlog 事实），
    // 平台映射 x64/Win32/x86/ARM64、反斜杠 + 尾分隔符
    // 容忍、正斜杠输出。
    void pipeline_cl_join(R& r)
    {
        cg::test::check(
            r,
            mb::cl_under_tools_dir(R"(C:\VCT\14.51.36231\)", "x64")
                == "C:/VCT/14.51.36231/bin/HostX86/x64/cl.exe",
            "x64 join with backslashes and trailing separator");
        cg::test::check(
            r,
            mb::cl_under_tools_dir("C:/VCT", "Win32")
                == "C:/VCT/bin/HostX86/x86/cl.exe",
            "Win32 maps to the x86 target");
        cg::test::check(
            r,
            mb::cl_under_tools_dir("C:/VCT/", "x86")
                == "C:/VCT/bin/HostX86/x86/cl.exe",
            "x86 maps to the x86 target");
        cg::test::check(
            r,
            mb::cl_under_tools_dir(R"(C:\VCT\)", "ARM64")
                == "C:/VCT/bin/HostX86/ARM64/cl.exe",
            "ARM64 maps to the ARM64 target");
        cg::test::check(
            r,
            mb::cl_under_tools_dir("C:/VCT", "arm")
                == "C:/VCT/bin/HostX86/arm/cl.exe",
            "unknown platform passes through as its own target");
    }

    // -getProperty 解析穿过求值噪声取最后一条路径形态行；
    // cl 横幅解析产出工具集 major.minor。
    void pipeline_parse_helpers(R& r)
    {
        cg::test::check(
            r,
            mb::parse_last_path_line(
                R"(Determining projects...
E:\tools\msvc\
note: something
E:\tools\msvc
)") == R"(E:\tools\msvc)",
            "last path line wins over noise");
        cg::test::check(
            r,
            mb::parse_last_path_line(
                R"(noise only
no path here
still nothing
)")
                .empty(),
            "no path line -> empty");
        cg::test::check(
            r,
            mb::parse_last_path_line("").empty(),
            "empty output -> empty");
        cg::test::check(
            r,
            mb::parse_last_path_line("E:/a\nE:/b\nE:/a") == "E:/a",
            "duplicate lines: the last occurrence wins");
        cg::test::check(
            r,
            mb::parse_last_path_line(R"(\\server\share\tools)")
                == R"(\\server\share\tools)",
            "UNC paths are path-shaped");
    }

    // 常规 Rebuild 实参向量。带空格的值拼成
    // 不带引号：property_token 从不内嵌引号，run_extract
    // 的命令行组装对整个令牌做 CRT 引号。（旧的内嵌
    // 引号拼写把引号漏进 MSBuild 属性值 ->
    // 带空格路径报 MSB4184；证伪了"计划风险：给带空格的
    // /p: 值加引号"。）
    void pipeline_extract_args(R& r)
    {
        mb::ExtractParams p;
        p.input = "E:/proj/DemoSln.sln";
        p.configuration = "Debug";
        p.platform = "x64";
        p.cl_tool_exe = "ccg.exe";
        p.cl_tool_path = "E:/tools/a b/ccg";
        const std::vector<std::string> args = mb::build_extract_args(p);
        const std::vector<std::string> want = {
            "E:/proj/DemoSln.sln",
            "-t:Rebuild",
            "-p:Configuration=Debug",
            "-p:Platform=x64",
            "-p:CLToolExe=ccg.exe",
            "-p:CLToolPath=E:/tools/a b/ccg",
            "-m:1",
            "-v:m",
            "-nologo"};
        cg::test::check(r, args == want, "stock extract argument vector");
    }

    // 工具集版本查找的单一实现钉子（行为变更波）：斜杠规范化→整串折叠
    // 小写→三连段 /vc/tools/msvc/ 标记→版本段严格数字校验。本组用例同时
    // 钉住两个调用面——translate 适配器（compat_version_of 经 INCLUDE 与
    // cl 路径）与 pipeline 的 vswhere 兜底挑选（经共用函数取版本键）。
    // 纯文本，所有平台可查。
    //
    /// 版本四段是否等于期望的逐段值。
    [[nodiscard]]
    auto version_list_is(
        const std::vector<int>&    got,
        std::initializer_list<int> want) -> bool
    {
        return got == std::vector<int>{want};
    }

    void pipeline_msvc_version(R& r)
    {
        // ① 全小写钉子（统一生效回归钉）：合并前 pipeline 侧大小写敏感
        //    标记对此形态返回空，是本次行为变更修掉的功能缺失面。
        cg::test::check(
            r,
            version_list_is(
                mb::msvc_toolset_version_of(
                    R"(c:\program files\microsoft visual studio\18\community\vc\tools\msvc\14.51.36231\bin\hostx64\x64\cl.exe)"),
                {14, 51, 36231}),
            "fully-lowercase path: folded marker lookup resolves the version");
        // ② 混合大小写（原生反斜杠/正斜杠两个拼写族）。
        cg::test::check(
            r,
            version_list_is(
                mb::msvc_toolset_version_of(
                    R"(C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\HostX86\x64\cl.exe)"),
                {14, 51, 36231}),
            "backslash path: version stops at the next separator");
        cg::test::check(
            r,
            version_list_is(
                mb::msvc_toolset_version_of(
                    "C:/VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/ARM64/cl.exe"),
                {14, 44, 35207}),
            "forward-slash path: same key");
        // ③ 真实磁盘上的混合拼写：段内大小写交错（tools 小写、Hostx64
        //    混合）也必须命中折叠查找。
        cg::test::check(
            r,
            version_list_is(
                mb::msvc_toolset_version_of(
                    "C:/Program Files/Microsoft Visual "
                    "Studio/18/Community/vc/tools/MSVC/"
                    "14.51.36231/Bin/Hostx64/x64/cl.exe"),
                {14, 51, 36231}),
            "mixed-case segments (hostx64 spelling) resolve the version");
        // ④ 敌意目录：路径里恰有 MSVC 字样但不在 /vc/tools/msvc/ 三连段
        //    位置——必须返回空，绝不产出 {0} 假版本混入 vswhere 的
        //    "最高版本胜出"比较（旧单段实现会取到 VS18 段）。
        cg::test::check(
            r,
            mb::msvc_toolset_version_of("D:/MSVC/VS18/14.51.36231/bin/cl.exe")
                .empty(),
            "hostile MSVC-named directory yields no version key");
        cg::test::check(
            r,
            mb::msvc_toolset_version_of("C:/VS/no-toolset/cl.exe").empty(),
            "no /vc/tools/msvc/ marker -> empty key");
        // ⑤ 非数字版本段（latest 目录）：返回空。
        cg::test::check(
            r,
            mb::msvc_toolset_version_of(
                "C:/VC/Tools/MSVC/latest/bin/Hostx64/x64/cl.exe")
                .empty(),
            "non-numeric version segment (latest) -> empty key");
        // ⑥ 尾空段（"14."）：返回空（旧宽松 atoi 会解析出 {14,0}）。
        cg::test::check(
            r,
            mb::msvc_toolset_version_of(
                "C:/VC/Tools/MSVC/14./bin/Hostx64/x64/cl.exe")
                .empty(),
            "trailing empty segment (14.) -> empty key");
        // ⑦⑧ 版本段严格校验（atoi 归 0 防线）：部分数字+字母的段两调用
        //    面同判非法；translate 侧调用面（compat_version_of）在同一
        //    输入上取同一版本。
        cg::test::check(
            r,
            mb::msvc_toolset_version_of(
                "C:/VC/Tools/MSVC/14.29.x/bin/Hostx64/x64/cl.exe")
                .empty(),
            "partially numeric segment (14.29.x) -> empty key, no {0} filler");
        cg::test::check(
            r,
            mb::msvc_toolset_version_of("").empty(),
            "empty path -> empty key");
        cg::test::check(
            r,
            mb::detail::compat_version_of(
                R"(c:\program files\microsoft visual studio\18\community\vc\tools\msvc\14.51.36231\bin\hostx64\x64\cl.exe)")
                == "19.51.36231",
            "translate caller (compat_version_of): lowercase path maps to "
            "19.minor.build");
        cg::test::check(
            r,
            mb::detail::compat_version_of("D:/MSVC/VS18/14.51.36231/bin/cl.exe")
                .empty(),
            "translate caller: hostile directory yields no compat version");
        // ⑦ INCLUDE 串直测：混合条目里首个工具集条目胜出；小写前置条目
        //    也取到；无工具集条目时为空（版本号数组直出，兼容版本映射
        //    不再经字符串往返）。
        cg::test::check(
            r,
            version_list_is(
                mb::detail::include_env_toolset_version(
                    "D:/SDK10/Include/1.2.3/ucrt;"
                    "c:/program files/vc/tools/msvc/14.44.35207/include;"
                    "E:/VS/VC/Tools/MSVC/14.51.36231/atlmfc/include"),
                {14, 44, 35207}),
            "INCLUDE scan: first toolset entry wins over a later one");
        cg::test::check(
            r,
            mb::detail::include_env_toolset_version("D:/plain/include;E:/other")
                .empty(),
            "INCLUDE scan: no toolset entry -> empty");
    }

    // 注册表存/取往返：每个字段幸存、原子替换
    // 不留 .tmp、第二次存是替换、缺文件 -> nullopt、坏
    // 文件 -> 大声运行时错误。
    void pipeline_registry_roundtrip(R& r)
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "ccg-msbuild-reg-test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const std::string path = (dir / "msbuild.json").string();

        mb::Registry reg;
        reg.default_config = "Debug-x64";
        mb::RegistryEntry debug;
        debug.name = "Debug-x64";
        debug.configuration = "Debug";
        debug.platform = "x64";
        debug.solution = "DemoSln.sln";
        debug.db = ".ccg/Debug-x64.db";
        debug.compile_commands = ".ccg/msbuild/Debug-x64/compile_commands.json";
        debug.msbuild = "E:/VS/MSBuild/Current/Bin/MSBuild.exe";
        debug.cl = "E:/VS/VC/Tools/MSVC/14.51.36231/bin/HostX86/x64/cl.exe";
        debug.extracted_at = "2026-10-12T00:00:00Z";
        debug.tus = 3;
        debug.failed = 0;
        mb::RegistryEntry release = debug;
        release.name = "Release-x64";
        release.configuration = "Release";
        release.db = ".ccg/Release-x64.db";
        release.compile_commands =
            ".ccg/msbuild/Release-x64/compile_commands.json";
        release.tus = 4;
        release.failed = 1;
        reg.entries = {debug, release};

        mb::save_registry(path, reg);
        cg::test::check(
            r,
            !fs::exists(fs::path{path + ".tmp"}),
            "atomic replace leaves no .tmp staging file");
        const std::optional<mb::Registry> loaded = mb::load_registry(path);
        cg::test::check(r, loaded.has_value(), "registry loads after save");
        if (loaded)
        {
            cg::test::check(r, loaded->version == 1, "version round-trips");
            cg::test::check(
                r,
                loaded->generator == "msbuild",
                "generator round-trips");
            cg::test::check(
                r,
                loaded->default_config == "Debug-x64",
                "default round-trips");
            cg::test::check(
                r,
                loaded->entries.size() == 2,
                "entry count round-trips");
            if (loaded->entries.size() == 2)
            {
                const mb::RegistryEntry& a = loaded->entries[0];
                cg::test::check(
                    r,
                    a.name == debug.name
                        && a.configuration == debug.configuration
                        && a.platform == debug.platform
                        && a.solution == debug.solution && a.db == debug.db
                        && a.compile_commands == debug.compile_commands
                        && a.msbuild == debug.msbuild && a.cl == debug.cl
                        && a.extracted_at == debug.extracted_at
                        && a.tus == debug.tus && a.failed == debug.failed,
                    "Debug entry fields round-trip");
                cg::test::check(
                    r,
                    loaded->entries[1].db == ".ccg/Release-x64.db"
                        && loaded->entries[1].tus == 4
                        && loaded->entries[1].failed == 1,
                    "Release entry fields round-trip");
            }
        }

        // 原位替换（第二次存）与缺文件行为。
        reg.entries.pop_back();
        mb::save_registry(path, reg);
        const std::optional<mb::Registry> replaced = mb::load_registry(path);
        cg::test::check(
            r,
            replaced && replaced->entries.size() == 1,
            "second save replaces the registry");
        cg::test::check(
            r,
            !mb::load_registry((dir / "absent.json").string()).has_value(),
            "missing registry file -> nullopt (first-run behavior)");

        // 坏文件要大声，绝不静默忽略。
        std::ofstream bad{fs::path{path}, std::ios::binary | std::ios::trunc};
        bad << "{not json";
        bad.close();
        bool threw = false;
        try
        {
            static_cast<void>(mb::load_registry(path));
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        cg::test::check(r, threw, "corrupt registry throws");
        fs::remove_all(dir, ec);
    }

    // 裸 /Yu 绝不吞掉后面的源文件；生产者 TU 上的
    // /Yc /Fp 被分类（后者胜）且不改变
    // cdb 形态。
    void adapt_pch_guards(R& r)
    {
        mb::ClInvocation bare;
        bare.cwd = "E:/proj";
        bare.argv_utf8 = {"cl.exe", "/c", "/Yu", "src\\main.cpp"};
        const std::vector<mb::CdbEntry> out = mb::adapt_cl(bare);
        cg::test::check(r, out.size() == 1, "bare /Yu keeps the following TU");
        if (out.size() == 1)
        {
            cg::test::check(
                r,
                out[0].file == "E:/proj/src/main.cpp",
                "the source after bare /Yu is the entry file");
            cg::test::check(
                r,
                !contains(out[0].arguments, "/Yu"),
                "bare /Yu is stripped from the cdb");
        }

        mb::ClInvocation macro;
        macro.cwd = "E:/proj";
        macro.argv_utf8 =
            {"cl.exe", "/c", "/Yu", "/D", "FOO=1", "src\\main.cpp"};
        const std::vector<mb::CdbEntry> mout = mb::adapt_cl(macro);
        cg::test::check(
            r,
            mout.size() == 1 && contains(mout[0].arguments, "/DFOO=1"),
            "bare /Yu does not swallow the next /D macro (the "
            "define itself arrives spliced, /DFOO=1)");

        mb::ClInvocation producer;
        producer.cwd = "E:/proj";
        producer.argv_utf8 = {
            "cl.exe",
            "/c",
            "/Yc",
            "pch.h",
            "/Fp",
            "build\\app.pch",
            "src\\pch.cpp",
            "/Fpbuild\\later.pch"};
        const mb::PchClassification cls = mb::classify_cl_pch(producer);
        cg::test::check(
            r,
            cls.is_producer && !cls.pch_use,
            "/Yc classifies the invocation as a producer");
        cg::test::check(
            r,
            cls.pch_header == "pch.h",
            "/Yc header spelling is preserved");
        cg::test::check(
            r,
            cls.pch_key == "E:/proj/build/later.pch",
            "the last /Fp wins and resolves against cwd");
        cg::test::check(
            r,
            cls.sources.size() == 1 && cls.sources[0] == "src\\pch.cpp",
            "producer TU is captured for the sidecar");

        mb::ClInvocation consumer;
        consumer.cwd = "E:/proj";
        consumer.argv_utf8 = {
            "cl.exe",
            "/c",
            "/Yu",
            "pch.h",
            "/Fp",
            "build\\app.pch",
            "src\\main.cpp"};
        const mb::PchClassification ccls = mb::classify_cl_pch(consumer);
        cg::test::check(
            r,
            !ccls.is_producer && ccls.pch_use,
            "/Yu + /Fp classifies a consumer");
        cg::test::check(
            r,
            ccls.pch_key == "E:/proj/build/app.pch",
            "consumer /Fp resolves absolute");
    }

    // 与记录 /I 重复的 INCLUDE 目录不得物化为 -imsvc：同一目录 user（/I）
    // 与 system（-imsvc）双身份时 clang 去重保留 system 位置，/I 的前置搜
    // 索优先级丢失（同名 stdafx.h 被后面的目录抢先解析）。连写与分写两种
    // /I 形态、大小写不敏感比对都要覆盖；不重复的目录照常物化、顺序不变。
    void adapt_include_env_i_priority(R& r)
    {
        mb::ClInvocation inv;
        inv.cwd = "E:/proj";
        inv.argv_utf8 = {
            "cl.exe",
            "/c",
            "/ID:/work/Suite/MainViewApp",
            "/I",
            "D:/work/Suite/Common",
            "src\\main.cpp"};
        inv.include_env =
            "D:\\work\\Suite\\Core;d:/work/suite/mainviewapp;D:\\work\\Suite\\"
            "MsTools;D:/WORK/SUITE/Common";
        const std::vector<mb::CdbEntry> out = mb::adapt_cl(inv);
        cg::test::check(r, out.size() == 1, "one TU entry (/I priority)");
        if (out.size() != 1)
        {
            return;
        }
        const std::vector<std::string>& argv = out[0].arguments;
        std::vector<std::string>        fed;
        for (std::size_t i = 1; i < argv.size(); ++i)
        {
            if (argv[i - 1] == "-imsvc")
            {
                fed.push_back(argv[i]);
            }
        }
        const std::vector<std::string> want = {
            "D:/work/Suite/Core",
            "D:/work/Suite/MsTools"};
        cg::test::check(
            r,
            fed == want,
            std::format(
                "only non-/I-duplicated dirs materialize: got [{}], "
                "want [{}]",
                [&]
                {
                    std::string s;
                    for (const std::string& f : fed)
                    {
                        s += f + "; ";
                    }
                    return s;
                }(),
                [&]
                {
                    std::string s;
                    for (const std::string& w : want)
                    {
                        s += w + "; ";
                    }
                    return s;
                }()));
        cg::test::check(
            r,
            contains(argv, "/ID:/work/Suite/MainViewApp"),
            "the joined /I entry stays in the argv verbatim");
        const auto i_at = std::ranges::find(argv, "/I");
        cg::test::check(
            r,
            i_at != argv.end() && i_at + 1 != argv.end()
                && *(i_at + 1) == "D:/work/Suite/Common",
            "the separate /I entry stays in the argv verbatim");
    }

    // MSVC 模块带值开关（ifcOutput/scanDependencies/sourceDependencies/
    // ifcSearchDir/reference/exportHeader，名单出自 cg.util.msvcswitch 单点
    // 清单）：MSBuild 实发形态是「开关 值」相邻两 token——cdb 忠实成对保留
    // （开关名与值 token 都进 argv、值逐字不绝对化），冒号连写形态整 token
    // 保留，末 token 裸开关不误吞；不带这些开关的普通 TU argv 逐字节不变。
    // 消费端（喂 clang 前）的成对剥除由剥离层单点负责，本用例末段的等价
    // 守卫钉死：剥除后的 argv 与不带六项开关的调用逐 token 相等——引擎
    // 看到的命令不变，变的只是 cdb 的忠实度。
    void adapt_module_valued_switches(R& r)
    {
        // 扫描趟实录形态（.ixx 双趟之一）：/ifcOutput 带产物值、
        // /scanDependencies 带目录值。
        mb::ClInvocation scan;
        scan.cwd = "E:/proj";
        scan.argv_utf8 = {
            "cl.exe",
            "/c",
            "/std:c++20",
            "/interface",
            "/ifcOutput",
            "E:\\proj\\int\\mod.ifc",
            "/scanDependencies",
            "E:\\proj\\int\\",
            "/TP",
            "mod.ixx"};
        const std::vector<mb::CdbEntry> s_out = mb::adapt_cl(scan);
        cg::test::check(
            r,
            s_out.size() == 1,
            "scan-pass invocation yields one entry");
        if (s_out.size() == 1)
        {
            const std::vector<std::string>& argv = s_out[0].arguments;
            cg::test::check(
                r,
                argv_has_flag_value(
                    argv,
                    "/ifcOutput",
                    "E:\\proj\\int\\mod.ifc"),
                "/ifcOutput keeps its .ifc value token verbatim "
                "(recorded spelling, no absolutize)");
            cg::test::check(
                r,
                argv_has_flag_value(
                    argv,
                    "/scanDependencies",
                    "E:\\proj\\int\\"),
                "/scanDependencies keeps its directory value token");
            cg::test::check(
                r,
                std::ranges::find(argv, "/interface") != argv.end(),
                "the single-token /interface flag survives as before");
        }

        // 编译趟与消费 TU：/sourceDependencies 目录值、/ifcSearchDir 目录值、
        // /reference 的 name=path 值、/exportHeader 的头路径值全部成对在场。
        mb::ClInvocation consumer;
        consumer.cwd = "E:/proj";
        consumer.argv_utf8 = {
            "cl.exe",
            "/c",
            "/std:c++20",
            "/sourceDependencies",
            "E:\\proj\\int\\",
            "/ifcSearchDir",
            "E:\\proj\\ifc",
            "/reference",
            "mod=E:/proj/int/mod.ifc",
            "/exportHeader",
            "E:\\proj\\inc\\units.h",
            "use.cpp"};
        const std::vector<mb::CdbEntry> c_out = mb::adapt_cl(consumer);
        cg::test::check(
            r,
            c_out.size() == 1,
            "consumer invocation yields one entry");
        if (c_out.size() == 1)
        {
            const std::vector<std::string>& argv = c_out[0].arguments;
            cg::test::check(
                r,
                argv_has_flag_value(
                    argv,
                    "/sourceDependencies",
                    "E:\\proj\\int\\"),
                "/sourceDependencies keeps its directory value token");
            cg::test::check(
                r,
                argv_has_flag_value(argv, "/ifcSearchDir", "E:\\proj\\ifc"),
                "/ifcSearchDir keeps its directory value token");
            cg::test::check(
                r,
                argv_has_flag_value(
                    argv,
                    "/reference",
                    "mod=E:/proj/int/mod.ifc"),
                "/reference keeps its name=path value token");
            cg::test::check(
                r,
                argv_has_flag_value(
                    argv,
                    "/exportHeader",
                    "E:\\proj\\inc\\units.h"),
                "/exportHeader keeps its header value token");
        }

        // 冒号连写形态：整 token（开关与值同体）逐字保留，不拆不译。
        mb::ClInvocation joined;
        joined.cwd = "E:/proj";
        joined
            .argv_utf8 = {"cl.exe", "/c", "/ifcOutput:C:/b/mod.ifc", "mod.ixx"};
        const std::vector<mb::CdbEntry> j_out = mb::adapt_cl(joined);
        cg::test::check(
            r,
            j_out.size() == 1
                && std::ranges::find(
                       j_out[0].arguments,
                       "/ifcOutput:C:/b/mod.ifc")
                       != j_out[0].arguments.end(),
            "the colon-joined spelling stays in argv as one verbatim token");

        // 末 token 裸开关：i+1<n 不成立，绝误吞（与 /U 族一致），裸拼写
        // 原样进 cdb（防御面：剥除层对裸形态同样剥除开关本体）；透明追加
        // 旗标（/X 等）照常缀后，裸开关未必是 argv 最后一个 token。
        mb::ClInvocation bare_tail;
        bare_tail.cwd = "E:/proj";
        bare_tail.argv_utf8 = {"cl.exe", "/c", "mod.ixx", "/ifcOutput"};
        const std::vector<mb::CdbEntry> b_out = mb::adapt_cl(bare_tail);
        cg::test::check(
            r,
            b_out.size() == 1
                && std::ranges::find(b_out[0].arguments, "/ifcOutput")
                       != b_out[0].arguments.end(),
            "a trailing bare /ifcOutput stays in argv without swallowing");

        // 同名前缀不误伤：/ifcOutputX 不是 /ifcOutput——不配对、不吞值，
        // 后随 token 按原规则处理（值形态的孤儿 token 仍按非源输入防御
        // 丢弃，与修复前一致）。
        mb::ClInvocation prefix_noise;
        prefix_noise.cwd = "E:/proj";
        prefix_noise.argv_utf8 = {
            "cl.exe",
            "/c",
            "/ifcOutputX",
            "E:/lookalike/orphan.ifc",
            "/std:c++20",
            "mod.ixx"};
        const std::vector<mb::CdbEntry> n_out = mb::adapt_cl(prefix_noise);
        cg::test::check(
            r,
            n_out.size() == 1
                && std::ranges::find(n_out[0].arguments, "/ifcOutputX")
                       != n_out[0].arguments.end()
                && std::ranges::find(
                       n_out[0].arguments,
                       "E:/lookalike/orphan.ifc")
                       == n_out[0].arguments.end()
                && std::ranges::find(n_out[0].arguments, "/std:c++20")
                       != n_out[0].arguments.end(),
            "a same-prefix lookalike flag neither pairs nor swallows");

        // 引擎等价守卫（关键不变量）：带六项开关的调用经翻译层（成对保留）
        // 再经剥离层（成对剥除）后，argv 与同一调用完全不带六项开关的翻译
        // 产物逐 token 相等——引擎看到的命令逐字节不变。
        mb::ClInvocation with_switches;
        with_switches.cwd = "E:/proj";
        with_switches.argv_utf8 = {
            "cl.exe",
            "/c",
            "/std:c++20",
            "/TP",
            "/interface",
            "/ifcOutput",
            "E:/proj/int/mod.ifc",
            "/scanDependencies",
            "E:/proj/int/",
            "mod.ixx"};
        mb::ClInvocation without_switches = with_switches;
        without_switches.argv_utf8.erase(
            without_switches.argv_utf8.begin() + 4,
            without_switches.argv_utf8.end() - 1);
        const std::vector<mb::CdbEntry> w_out = mb::adapt_cl(with_switches);
        const std::vector<mb::CdbEntry> o_out = mb::adapt_cl(without_switches);
        cg::test::check(
            r,
            w_out.size() == 1 && o_out.size() == 1,
            "both guard invocations yield one entry");
        if (w_out.size() == 1 && o_out.size() == 1)
        {
            // 剥离层吃完整 arguments（含编译器 token；喂引擎时 arguments[0]
            // 由加载器丢弃——剥离不碰它），等价比较因此是整体逐 token。
            const cc::detail::MsvcModuleStrip stripped =
                cc::detail::strip_msvc_module_switches(w_out[0].arguments);
            cg::test::check(
                r,
                stripped.stripped,
                "the strip layer recognizes the preserved pairs");
            cg::test::check(
                r,
                stripped.argv == o_out[0].arguments,
                "translate-then-strip equals the switch-free translation "
                "(engine argv byte-identical)");
        }
    }

    const cg::test::Registrar reg_shape{"msbuild.adapt_shape", &adapt_shape};
    const cg::test::Registrar reg_pch_guards{
        "msbuild.adapt_pch_guards",
        &adapt_pch_guards};
    const cg::test::Registrar reg_blacklist{
        "msbuild.adapt_blacklist_case",
        &adapt_blacklist_case};
    const cg::test::Registrar reg_two_phase{
        "msbuild.adapt_two_phase_equivalence",
        &adapt_two_phase_equivalence};
    const cg::test::Registrar reg_non_cxx{
        "msbuild.adapt_non_cxx",
        &adapt_non_cxx};
    const cg::test::Registrar reg_rsp{"msbuild.adapt_rsp", &adapt_rsp};
    const cg::test::Registrar reg_clwrap_log_debug{
        "msbuild.clwrap_log_debug_golden",
        &clwrap_log_debug_golden};
    const cg::test::Registrar reg_clwrap_log_release{
        "msbuild.clwrap_log_release_golden",
        &clwrap_log_release_golden};
    const cg::test::Registrar reg_clwrap_log_sln{
        "msbuild.clwrap_log_sln_std",
        &clwrap_log_sln_std};
    const cg::test::Registrar reg_determinism{
        "msbuild.adapt_determinism",
        &adapt_determinism};
    const cg::test::Registrar reg_wrap_quote{
        "msbuild.wrap_quote_roundtrip",
        &wrap_quote_roundtrip};
    const cg::test::Registrar reg_wrap_json{
        "msbuild.wrap_json_escape",
        &wrap_json_escape};
    const cg::test::Registrar reg_enum{
        "msbuild.pipeline_enum_configs",
        &pipeline_enum_configs};
    const cg::test::Registrar reg_cl_join{
        "msbuild.pipeline_cl_join",
        &pipeline_cl_join};
    const cg::test::Registrar reg_parse{
        "msbuild.pipeline_parse_helpers",
        &pipeline_parse_helpers};
    const cg::test::Registrar reg_extract_args{
        "msbuild.pipeline_extract_args",
        &pipeline_extract_args};
    const cg::test::Registrar reg_registry{
        "msbuild.pipeline_registry_roundtrip",
        &pipeline_registry_roundtrip};
    const cg::test::Registrar reg_wrap_log{
        "msbuild.pipeline_wrap_log",
        &pipeline_wrap_log};
    const cg::test::Registrar reg_write_cdb{
        "msbuild.pipeline_write_cdb",
        &pipeline_write_cdb};
    const cg::test::Registrar reg_pch_chain{
        "msbuild.pipeline_pch_sidecar_chain",
        &pipeline_pch_sidecar_chain};
    const cg::test::Registrar reg_include_env{
        "msbuild.adapt_include_env",
        &adapt_include_env};
    const cg::test::Registrar reg_adapt_env_supp{
        "msbuild.adapt_include_env_suppression_and_toolsets",
        &adapt_include_env_suppression_and_toolsets};
    const cg::test::Registrar reg_include_env_i{
        "msbuild.adapt_include_env_i_priority",
        &adapt_include_env_i_priority};
    const cg::test::Registrar reg_module_switches{
        "msbuild.adapt_module_valued_switches",
        &adapt_module_valued_switches};
    const cg::test::Registrar reg_msvc_version{
        "msbuild.pipeline_msvc_version",
        &pipeline_msvc_version};

    // 记录模式桩角色：被包装映像的基名决定包装器
    // 替身是哪个工具（桩目录里的家族名副本）；
    // ccg.exe 自身（全构建 CLToolExe 形态）保持"未知"——
    // 泄漏金丝雀跳过其 argv（零磁盘桩反正什么都不写）。
    void record_stub_roles(R& r)
    {
        using mb::detail::StubRole;
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("cl.exe") == StubRole::cl,
            "cl.exe -> cl");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("clang-cl.exe") == StubRole::cl,
            "clang-cl.exe -> cl (same stub semantics)");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("link.exe") == StubRole::link,
            "link.exe");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("lib.exe") == StubRole::lib,
            "lib.exe");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("rc.exe") == StubRole::rc,
            "rc.exe");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("mt.exe") == StubRole::mt,
            "mt.exe");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("ml64.exe") == StubRole::masm,
            "ml64.exe");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("ml.exe") == StubRole::masm,
            "ml.exe");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("marmasm.exe") == StubRole::masm,
            "marmasm.exe");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("bscmake.exe")
                == StubRole::bscmake,
            "bscmake");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("xdcmake.exe")
                == StubRole::xdcmake,
            "xdcmake");
        cg::test::check(
            r,
            mb::detail::stub_role_of_basename("ccg.exe") == StubRole::unknown,
            "ccg.exe -> unknown (full-build wrap, treated as cl)");
        bool all_family = true;
        for (const std::string_view name : mb::detail::stub_tool_family)
        {
            if (mb::detail::stub_role_of_basename(name) == StubRole::unknown)
            {
                all_family = false;
            }
        }
        cg::test::check(r, all_family, "every family name maps to a role");
    }
    // 按工具的声明输出解析（泄漏金丝雀的输入面）：桩在
    // 零磁盘契约下什么都不写，所以每个工具家族
    // 唯一要查的是捕获 argv 字面上声明了
    // 哪些输出路径——无按源 .obj、无 vc`<ver>`.pdb 或 .ilk 默认名。
    /** 从字面 argv 收声明输出（cwd 锚定输出路径）。 */
    [[nodiscard]]
    auto declared_outputs_of(
        const std::string&                 cwd,
        std::initializer_list<std::string> argv) -> std::vector<std::string>
    {
        mb::WrapLog      capture;
        mb::ClInvocation inv;
        inv.cwd = cwd;
        inv.argv_utf8 = std::vector<std::string>{argv};
        capture.invocations.push_back(std::move(inv));
        return mb::capture_declared_outputs(capture);
    }

    void record_declared_outputs(R& r)
    {
        const std::string cwd = "E:/proj";
        {
            // 零写盘语义：替身不再推导任何缺省名（逐源 .obj、vc`<ver>`.pdb、
            // .ilk 全部退役），只收 argv 里字面写出的输出旗标值——分写 /Fo
            // 目录形式收的是目录本身（金丝雀盯它的修改时间）。
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"cl.exe",
                 "/c",
                 "/Fo",
                 "x64/Debug/",
                 "/Yc",
                 "pch.h",
                 "src/main.cpp",
                 "src/util.cpp"});
            cg::test::check(
                r,
                v == std::vector<std::string>{"E:/proj/x64/Debug"},
                "cl separate /Fo dir form declares the directory itself");
        }
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"cl.exe",
                 "/c",
                 "/Fox64/Debug/app.obj",
                 "/Fdx64/Debug/",
                 "src/main.cpp"});
            const std::vector<std::string> want = {
                "E:/proj/x64/Debug/app.obj",
                "E:/proj/x64/Debug"};
            cg::test::check(
                r,
                v == want,
                "cl exact /Fo + /Fd dir form (both literal)");
        }
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"cl.exe", "/c", "/Fpsolid.pch", "/Ycui.h", "src/a.cpp"});
            cg::test::check(
                r,
                v
                    == std::vector<std::string>{"E:/proj/"
                                                "solid.pch"},
                "/Yc header is an input, not a declared output");
        }
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"link.exe",
                 "/OUT:out/app.exe",
                 "/PDB:out/app.pdb",
                 "/IMPLIB:out/app.lib",
                 "/MAP:out/app.map",
                 "a.obj"});
            const std::vector<std::string> want = {
                "E:/proj/out/app.exe",
                "E:/proj/out/app.pdb",
                "E:/proj/out/app.lib",
                "E:/proj/out/app.map"};
            cg::test::check(
                r,
                v == want,
                "link OUT/PDB/IMPLIB/MAP (no derived .ilk)");
        }
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"lib.exe", "/OUT:out/libsub.lib", "/NOLOGO", "a.obj"});
            cg::test::check(
                r,
                v
                    == std::vector<std::string>{"E:/proj/out/"
                                                "libsub.lib"},
                "lib /OUT");
        }
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"rc.exe", "/nologo", "/fo", "x64/Debug/app.res", "app.rc"});
            cg::test::check(
                r,
                v == std::vector<std::string>{"E:/proj/x64/Debug/app.res"},
                "rc /fo (separate form)");
        }
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"ml64.exe", "/c", "/Foint/mul.obj", "/Tamul.asm"});
            cg::test::check(
                r,
                v
                    == std::vector<std::string>{"E:/proj/int/"
                                                "mul.obj"},
                "masm /Fo");
        }
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"mt.exe", "/manifest", "/outputresource:out/app.exe;#1"});
            cg::test::check(
                r,
                v
                    == std::vector<std::string>{"E:/proj/out/"
                                                "app.exe"},
                "mt /outputresource strips ;#id");
        }
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"bscmake.exe", "/o", "out/app.bsc", "a.sbr"});
            cg::test::check(
                r,
                v
                    == std::vector<std::string>{"E:/proj/out/"
                                                "app.bsc"},
                "bscmake /o");
        }
        // 带引号旗标值（响应文件拼写）让目录形态
        //    判定保持诚实：引号被剥、无按源推导。
        {
            const std::vector<std::string> v = declared_outputs_of(
                cwd,
                {"cl.exe", "/c", "/Fo\"E:/proj/int/\"", "src/a.cpp"});
            cg::test::check(
                r,
                v == std::vector<std::string>{"E:/proj/int"},
                "quoted /Fo value is the dir itself, unquoted");
        }
        // 多次调用去重：同一路径声明两次只报一次。
        {
            mb::WrapLog capture;
            for (const std::string_view exe : {"cl.exe", "cl.exe"})
            {
                mb::ClInvocation inv;
                inv.cwd = cwd;
                inv.argv_utf8 =
                    {std::string{exe}, "/c", "/Foint/a.obj", "src/a.cpp"};
                capture.invocations.push_back(std::move(inv));
            }
            cg::test::check(
                r,
                mb::capture_declared_outputs(capture)
                    == std::vector<std::string>{"E:/proj/int/a.obj"},
                "declared outputs dedup across invocations");
        }
    }

    // 泄漏金丝雀（no-op 语义的看门）：argv 声明的输出路径在沙盒与 .ccg 之
    // 外发生新建/改写才报告——真树既有文件零变化、缺失文件不被创建、沙盒
    // 内的路径一律豁免；树外路径退回运行窗口 mtime 判定（陈旧文件不误报）。
    void record_output_canary(R& r)
    {
        namespace fs = std::filesystem;
        const fs::path  dir = fs::temp_directory_path() / "ccg-msbuild-canary";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "proj", ec);
        fs::create_directories(dir / "sandbox/out", ec);
        fs::create_directories(dir / "outside", ec);
        // 真树既有文件（跑前存在）：保持不变 = 不报；被改写 = 报。
        const fs::path keep = dir / "proj" / "keep.pdb";
        const fs::path touch = dir / "proj" / "touch.obj";
        {
            std::ofstream o{keep, std::ios::binary};
            o << "old";
        }
        {
            std::ofstream o{touch, std::ios::binary};
            o << "old";
        }
        const std::string root =
            (dir / "proj").lexically_normal().generic_string();
        const std::vector<mb::TreeFileState> before =
            mb::snapshot_tree_states(root, {});
        const fs::file_time_type run_start = fs::file_time_type::clock::now();
        // 改写既有文件（字节数与修改时间都变）。
        {
            std::ofstream o{touch, std::ios::binary | std::ios::app};
            o << "more";
        }
        // 本轮新建的树外文件：修改时间落在运行窗口内 = 报。
        const fs::path outside = dir / "outside" / "hard.pdb";
        {
            std::ofstream o{outside, std::ios::binary};
            o << "fresh";
        }
        // 运行窗口之前的陈旧树外文件：静默存在不是泄漏。
        const fs::path stale = dir / "outside" / "stale.pdb";
        {
            std::ofstream o{stale, std::ios::binary};
            o << "stale";
        }
        fs::last_write_time(stale, run_start - std::chrono::seconds{60}, ec);
        // 沙盒内新建：豁免。
        const fs::path in_sandbox = dir / "sandbox" / "out" / "a.res";
        {
            std::ofstream o{in_sandbox, std::ios::binary};
            o << "x";
        }
        const std::vector<std::string> leaks = mb::scan_declared_output_leaks(
            {keep.generic_string(),
             touch.generic_string(),
             (dir / "proj" / "missing" / "absent.obj").generic_string(),
             outside.generic_string(),
             stale.generic_string(),
             in_sandbox.generic_string()},
            before,
            root,
            {(dir / "sandbox").generic_string(), root + "/.ccg"},
            run_start,
            {});
        bool touch_leak = false;
        bool outside_leak = false;
        bool extra = false;
        for (const std::string& l : leaks)
        {
            if (l == touch.generic_string())
            {
                touch_leak = true;
            }
            else if (l == outside.generic_string())
            {
                outside_leak = true;
            }
            else
            {
                extra = true;
            }
        }
        cg::test::check(
            r,
            touch_leak && outside_leak && !extra,
            "canary reports only the overwritten tree file and "
            "the run-window write outside the root");
        // 已报告路径（树差集先行命中）不双报。
        const std::vector<std::string> again = mb::scan_declared_output_leaks(
            {touch.generic_string()},
            before,
            root,
            {},
            run_start,
            {[&]
             {
                 std::string l = touch.generic_string();
                 for (char& c : l)
                 {
                     c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                              : c;
                 }
                 return l;
             }()});
        cg::test::check(
            r,
            again.empty(),
            "already-reported paths are not double-reported");
        fs::remove_all(dir, ec);
    }
    // CCG_CLWRAP_CL 前缀映射选择：最长 cwd 前缀胜（大小写与
    // 分隔符不敏感），落空返回 ""（调用方回退到
    // 全局 real_cl）。
    void record_cl_map_selection(R& r)
    {
        const std::string map =
            "E:/sln/projA\tC:/VS/14.44/cl.exe\n"
            "E:/sln/projA/nested\tC:/VS/14.44/nested/cl.exe\n"
            "E:/sln/projB\tC:/VS/14.51/cl.exe\n";
        cg::test::check(
            r,
            mb::detail::select_cl_by_cwd(map, "E:/sln/projA/nested/x.cpp.dir")
                == "C:/VS/14.44/nested/cl.exe",
            "longest prefix wins");
        cg::test::check(
            r,
            mb::detail::select_cl_by_cwd(map, "E:/sln/projA")
                == "C:/VS/14.44/cl.exe",
            "exact dir match");
        cg::test::check(
            r,
            mb::detail::select_cl_by_cwd(map, "E:\\sln\\projB\\sub")
                == "C:/VS/14.51/cl.exe",
            "backslash cwd matches forward-slash key");
        cg::test::check(
            r,
            mb::detail::select_cl_by_cwd(map, "E:/other").empty(),
            "no match -> empty (global fallback)");
        cg::test::check(
            r,
            mb::detail::select_cl_by_cwd(map, "E:/sln/projAB").empty(),
            "prefix must end at a path segment boundary");
    }

    // 记录模式 MSBuild 实参向量：工具路径家族 + 沙盒
    // IntDir/OutDir 覆盖集；full-build 与历史形态逐字节一致。
    //
    void record_extract_args(R& r)
    {
        // 刻意带空格的工程路径：注入的 /p: 值（桩
        // 目录 + 沙盒 IntDir/OutDir）继承空格，令牌
        // 必须保持无引号（引号会漏进 MSBuild 属性值
        // -> MSB4184；空格由命令行组装对
        // 整个令牌做 CRT 引号处理）。
        mb::ExtractParams p;
        p.input = "E:/proj/my demo/app.sln";
        p.configuration = "Debug";
        p.platform = "x64";
        p.cl_tool_exe = "ccg.exe";
        p.cl_tool_path = "E:/tools/ccg";
        p.mode = mb::ExtractMode::Record;
        p.stub_tool_dir = "E:/proj/my demo/.ccg/msbuild/Debug-x64/stub-tools";
        p.int_dir = "E:/proj/my demo/.ccg/Debug-x64/msbuild-sandbox/int/";
        p.out_dir = "E:/proj/my demo/.ccg/Debug-x64/msbuild-sandbox/out/";
        const std::vector<std::string> args = mb::build_extract_args(p);
        const std::string              joined = [&]
        {
            std::string s;
            for (const std::string& a : args)
            {
                s += a + " ";
            }
            return s;
        }();
        for (const std::string_view want :
             {"-p:CLToolPath=E:/proj/my demo/.ccg/msbuild/Debug-x64/stub-tools",
              "-p:LinkToolPath=",
              "-p:LibToolPath=",
              "-p:RCToolPath=",
              "-p:MtToolPath=",
              "-p:BSCMakeToolPath=",
              "-p:XDCMakeToolPath=",
              "-p:IntDir=E:/proj/my demo/.ccg/Debug-x64/msbuild-sandbox/int/",
              "-p:OutDir=E:/proj/my demo/.ccg/Debug-x64/msbuild-sandbox/out/"})
        {
            cg::test::check(
                r,
                joined.find(want) != std::string::npos,
                std::string{"record args carry "} + std::string{want});
        }
        cg::test::check(
            r,
            joined.find('"') == std::string::npos,
            "record args never embed quotes in /p: tokens");
        cg::test::check(
            r,
            contains(
                args,
                "-p:IntDir=E:/proj/my demo/.ccg/Debug-x64/"
                "msbuild-sandbox/int/"),
            "spaced IntDir token survives as ONE argv element");
        cg::test::check(
            r,
            joined.find("-p:CLToolExe=") == std::string::npos,
            "record mode does not pin CLToolExe (toolset default "
            "names cl.exe / clang-cl.exe must survive)");
        // full-build 与历史向量逐字节一致。
        p.mode = mb::ExtractMode::FullBuild;
        const std::vector<std::string> full = mb::build_extract_args(p);
        const std::vector<std::string> want = {
            "E:/proj/my demo/app.sln",
            "-t:Rebuild",
            "-p:Configuration=Debug",
            "-p:Platform=x64",
            "-p:CLToolExe=ccg.exe",
            "-p:CLToolPath=E:/tools/ccg",
            "-m:1",
            "-v:m",
            "-nologo"};
        cg::test::check(
            r,
            full == want,
            "full-build argument vector unchanged");
    }
    // 沙盒泄漏分类：白名单外的新文件是泄漏；
    // 生成器声明的源输出是 generated_sources；声明
    // 白名单解析覆盖 $(ProjectDir)/%(RootDir)%(Directory) 拼写
    // 并跳过不可展开的宏。
    void record_sandbox_scan(R& r)
    {
        namespace fs = std::filesystem;
        const fs::path dir =
            fs::temp_directory_path() / "ccg-msbuild-sandbox-test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "proj", ec);
        const fs::path vcx = dir / "proj" / "app.vcxproj";
        {
            std::ofstream out{vcx, std::ios::binary};
            out << "<Project>\n"
                << " <ItemGroup>\n"
                << "  <CustomBuild Include=\"gen.tpl\">\n"
                << "   <Command>cmd /c copy gen.tpl gen.cpp</Command>\n"
                << "   <Outputs>%(RootDir)%(Directory)gen.cpp</Outputs>\n"
                << "  </CustomBuild>\n"
                << "  <CustomBuild Include=\"pack.tpl\">\n"
                << "   <Command>cmd /c copy pack.tpl gen.bin</Command>\n"
                << "   <Outputs>%(RootDir)%(Directory)gen.bin</Outputs>\n"
                << "  </CustomBuild>\n"
                << "  <CustomBuild Include=\"other.tpl\">\n"
                << "   <Command>echo hi</Command>\n"
                << "   "
                   "<Outputs>$(MSBuildProjectDirectory)\\leak\\leaked.txt</Outputs>\n"
                << "  </CustomBuild>\n"
                << " </ItemGroup>\n"
                << "</Project>\n";
        }
        const std::string project_dir =
            (dir / "proj").lexically_normal().generic_string();
        const std::vector<std::string> declared =
            mb::generator_declared_source_outputs(vcx.generic_string());
        bool has_gen = false;
        for (const std::string& d : declared)
        {
            if (d == project_dir + "/gen.cpp")
            {
                has_gen = true;
            }
        }
        cg::test::check(
            r,
            has_gen,
            "%(RootDir)%(Directory) outputs expand to the item dir");
        // MIDL 头文件形态检查要求 .idl 与 .h 同在
        // （分类时读取目录级证据）。
        {
            fs::create_directories(dir / "proj" / "ifaces", ec);
            std::ofstream idl{
                dir / "proj" / "ifaces" / "obj.idl",
                std::ios::binary};
            idl << "interface IObj {};\n";
        }
        const mb::SandboxDiff diff = mb::classify_new_tree_files(
            project_dir,
            {"gen.cpp",
             "gen.bin",
             "ifaces/obj.tlb",
             "ifaces/obj_i.c",
             "ifaces/obj.h",
             "hardobj/adv.obj",
             "hard.exe",
             "leak/leaked.txt"},
            declared);
        cg::test::check(
            r,
            diff.generated_sources.size() == 1
                && diff.generated_sources[0] == "gen.cpp",
            "declared generated source is whitelisted");
        bool bin = false, tlb = false, ic = false, hdr = false;
        for (const std::string& g : diff.generator_products)
        {
            bin = bin || g == "gen.bin";
            tlb = tlb || g == "ifaces/obj.tlb";
            ic = ic || g == "ifaces/obj_i.c";
            hdr = hdr || g == "ifaces/obj.h";
        }
        cg::test::check(
            r,
            bin && tlb && ic && hdr,
            "declared non-source CustomBuild output and MIDL "
            "products (tlb / _i.c / generated header beside its "
            ".idl) classify as expected generator products, not "
            "leaks");
        bool obj = false, exe = false, txt = false;
        for (const std::string& l : diff.leaks)
        {
            obj = obj || l == "hardobj/adv.obj";
            exe = exe || l == "hard.exe";
            txt = txt || l == "leak/leaked.txt";
        }
        cg::test::check(
            r,
            obj && exe && txt,
            "hardcoded cl /Fo, link OutputFile and CustomBuild "
            "escapes all classify as leaks");
        // 无 .idl 兄弟的同名 .h 证明不了是 MIDL 输出：
        // 它保持泄漏（形态白名单绝不放宽泄漏面）。
        const mb::SandboxDiff plain =
            mb::classify_new_tree_files(project_dir, {"notes/Hdr.h"}, declared);
        cg::test::check(
            r,
            plain.generator_products.empty() && plain.leaks.size() == 1
                && plain.leaks[0] == "notes/Hdr.h",
            "a .h without a sibling .idl stays a leak");
        fs::remove_all(dir, ec);
    }

    // 记录模式噪声过滤使用的 MSB3073/VCEnd/exec 行形态
    // （run_extract 从控制台转印中压掉这些行并做摘要）。
    void record_post_build_line_shape(R& r)
    {
        cg::test::check(
            r,
            mb::detail::is_post_build_failure_line(
                "C:/.../Microsoft.CppCommon.targets(166,5): error MSB3073: The "
                "command \"copy x y\" exited with code 1. [app.vcxproj]"),
            "MSB3073 wrapper line matches");
        cg::test::check(
            r,
            mb::detail::is_post_build_failure_line(
                "VCEnd : error : The command \"xcopy a b\" exited "
                "with code 2."),
            "VCEnd exec line matches");
        cg::test::check(
            r,
            !mb::detail::is_post_build_failure_line(
                "main.cpp(12): error C2065: 'x': undeclared identifier"),
            "a compile diagnostic is not a post-build failure line");
        cg::test::check(
            r,
            !mb::detail::is_post_build_failure_line("Build succeeded."),
            "an ordinary build line passes through");
    }

    // /D 拼接：分开的 define/value 令牌合成一个 /DVALUE 令牌（clang
    // 响应文件按空白拆分——两个令牌会被读成两个
    // 实参）。已拼接拼写与尾部裸 /D 保持原样。
    void adapt_define_joined(R& r)
    {
        mb::ClInvocation inv;
        inv.cwd = "E:/proj";
        inv.argv_utf8 = {
            "cl.exe",
            "/c",
            "/D",
            "FOO=1",
            "/D",
            "_UNICODE",
            "/DBAR=2",
            "src\\main.cpp",
            "/D"};
        const std::vector<mb::CdbEntry> out = mb::adapt_cl(inv);
        cg::test::check(r, out.size() == 1, "one entry");
        if (out.size() != 1)
        {
            return;
        }
        const std::vector<std::string>& argv = out[0].arguments;
        cg::test::check(
            r,
            contains(argv, "/DFOO=1") && contains(argv, "/D_UNICODE"),
            "separate /D + value splice into one token each");
        cg::test::check(
            r,
            contains(argv, "/DBAR=2"),
            "already-joined /D stays verbatim");
        cg::test::check(
            r,
            contains(argv, "/D"),
            "a trailing bare /D with no value token stays verbatim");
    }

    // MSVC 源编码对齐：捕获 argv 未声明
    // /utf-8 与 /source-charset 的条目获得 -finput-charset=<宿主 ANSI 代码页>
    // 追加（带 BOM 的文件不受该旗标影响；该引擎
    // 行为由 clang 侧夹具钉住）。声明过的条目
    // 保持原样。
    /** argv 里 -finput-charset= 旗标的条数。 */
    [[nodiscard]]
    auto input_charset_flag_count(const std::vector<std::string>& argv)
        -> std::size_t
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            argv,
            [](const std::string& t)
            { return t.starts_with("-finput-charset="); }));
    }

    /** 带追加旗标的 cl 条目适配（E:/proj 工作目录形态）。 */
    [[nodiscard]]
    auto charset_probe_entry(const std::vector<std::string>& extra)
        -> std::vector<mb::CdbEntry>
    {
        mb::ClInvocation inv;
        inv.cwd = "E:/proj";
        inv.argv_utf8 = {"cl.exe", "/c"};
        inv.argv_utf8.insert(inv.argv_utf8.end(), extra.begin(), extra.end());
        inv.argv_utf8.push_back("src\\main.cpp");
        return mb::adapt_cl(inv);
    }

    void adapt_input_charset(R& r)
    {
        const std::string local = cg::util::platform::ansi_input_charset();
        {
            const std::vector<mb::CdbEntry> out = charset_probe_entry({});
            cg::test::check(r, out.size() == 1, "plain entry adapts");
            if (out.size() == 1)
            {
                cg::test::check(
                    r,
                    input_charset_flag_count(out[0].arguments)
                        == (local.empty() ? 0u : 1u),
                    std::format(
                        "undeclared entry carries the host charset "
                        "append (host '{}')",
                        local.empty() ? std::string{"none"} : local));
            }
        }
        {
            const std::vector<mb::CdbEntry> out =
                charset_probe_entry({"/utf-8"});
            cg::test::check(
                r,
                out.size() == 1
                    && input_charset_flag_count(out[0].arguments) == 0,
                "/utf-8 declared - no append");
        }
        {
            const std::vector<mb::CdbEntry> out =
                charset_probe_entry({"/source-charset:GBK"});
            cg::test::check(
                r,
                out.size() == 1
                    && input_charset_flag_count(out[0].arguments) == 0,
                "/source-charset:VALUE declared - no append");
        }
        {
            const std::vector<mb::CdbEntry> out =
                charset_probe_entry({"/source-charset", "GBK"});
            cg::test::check(
                r,
                out.size() == 1
                    && input_charset_flag_count(out[0].arguments) == 0,
                "separate /source-charset VALUE declared - no append");
        }
    }

    // 工具集根推导（工具集线索来源）对 cl.exe 路径
    // 与物化 include 目录都成立。
    void record_toolset_root(R& r)
    {
        cg::test::check(
            r,
            mb::msvc_toolset_root_of(
                "C:/VS/VC/Tools/MSVC/14.44.35207/bin/HostX86/x64/cl.exe")
                == "C:/VS/VC/Tools/MSVC/14.44.35207",
            "toolset root of a cl path");
        cg::test::check(
            r,
            mb::msvc_toolset_root_of("C:/VS/VC/Tools/MSVC/14.44.35207/include")
                == "C:/VS/VC/Tools/MSVC/14.44.35207",
            "toolset root of an include dir");
        cg::test::check(
            r,
            mb::msvc_toolset_root_of("C:/VS/include").empty(),
            "no toolset segment -> empty");
    }

    // .sln 成员枚举 + ActiveCfg 映射与 Build.0 参与位（每工程 cl
    // 解析与注册表披露的共用载体）跑在已入库混合夹具与手造 sln 上。
    void record_sln_projects(R& r)
    {
        const std::vector<mb::SlnProject> projects =
            mb::enumerate_sln_projects(fixture("mixed/mixed.sln"), "Debug|x64");
        cg::test::check(
            r,
            projects.size() == 2,
            "mixed.sln has two vcxproj members");
        bool a = false, b = false;
        for (const mb::SlnProject& p : projects)
        {
            if (p.name == "projA")
            {
                a = p.path.ends_with("/projA/projA.vcxproj")
                    && p.active_cfg == "Debug|x64" && p.build;
            }
            if (p.name == "projB")
            {
                b = p.path.ends_with("/projB/projB.vcxproj")
                    && p.active_cfg == "Debug|x64" && p.build;
            }
        }
        cg::test::check(
            r,
            a && b,
            "members resolved with ActiveCfg mapped and Build.0 set");

        // 手造 sln：删 Build.0 行——成员在场、ActiveCfg 照常解析、参与位
        // false（该配置下解决方案加载它但不编它）。纯文本解析，无进程。
        const cg::test::ScratchDir scratch{
            std::filesystem::temp_directory_path() / "ccg-sln-nobuild"};
        const char* sln_no_build =
            "Microsoft Visual Studio Solution File, Format Version 12.00\n"
            "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = "
            "\"projA\", \"projA\\projA.vcxproj\", "
            "\"{AAAAAAAA-1111-2222-3333-444444444444}\"\n"
            "EndProject\n"
            "Global\n"
            "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n"
            "\t\t{AAAAAAAA-1111-2222-3333-444444444444}.Debug|x64.ActiveCfg "
            "= Debug|x64\n"
            "\tEndGlobalSection\n"
            "EndGlobal\n";
        scratch.put(scratch.base / "no-build.sln", sln_no_build);
        const std::vector<mb::SlnProject> no_build = mb::enumerate_sln_projects(
            (scratch.base / "no-build.sln").generic_string(),
            "Debug|x64");
        cg::test::check(
            r,
            no_build.size() == 1 && no_build[0].name == "projA"
                && no_build[0].active_cfg == "Debug|x64" && !no_build[0].build,
            "member without Build.0 stays listed with in-build flag false");
    }

    const cg::test::Registrar reg_stub_roles{
        "msbuild.record_stub_roles",
        &record_stub_roles};
    const cg::test::Registrar reg_declared_outputs{
        "msbuild.record_declared_outputs",
        &record_declared_outputs};
    const cg::test::Registrar reg_output_canary{
        "msbuild.record_output_canary",
        &record_output_canary};
    const cg::test::Registrar reg_cl_map{
        "msbuild.record_cl_map_selection",
        &record_cl_map_selection};
    const cg::test::Registrar reg_record_args{
        "msbuild.record_extract_args",
        &record_extract_args};
    const cg::test::Registrar reg_sandbox{
        "msbuild.record_sandbox_scan",
        &record_sandbox_scan};
    const cg::test::Registrar reg_post_build_line{
        "msbuild.record_post_build_line_shape",
        &record_post_build_line_shape};
    const cg::test::Registrar reg_define_joined{
        "msbuild.adapt_define_joined",
        &adapt_define_joined};
    const cg::test::Registrar reg_input_charset{
        "msbuild.adapt_input_charset",
        &adapt_input_charset};
    const cg::test::Registrar reg_toolset_root{
        "msbuild.record_toolset_root",
        &record_toolset_root};
    const cg::test::Registrar reg_sln_projects{
        "msbuild.record_sln_projects",
        &record_sln_projects};

    // ------------------------------------------- 输入解析 / 环境 / 注册表
    // ------

    /** 操作数解析一次调用的多态结局。 */
    struct ResolveOutcome
    {
        bool                                ok = false;
        bool                                threw_usage = false;
        bool                                threw_runtime = false;
        std::string                         message;
        std::string                         usage;
        std::pair<std::string, std::string> value;
    };

    /** 解析操作数并按异常形态归档结局。 */
    [[nodiscard]]
    auto try_resolve_msbuild_input(const std::string& operand, bool list_mode)
        -> ResolveOutcome
    {
        ResolveOutcome o;
        try
        {
            o.value =
                cg::cli::detail::resolve_msbuild_input(operand, list_mode);
            o.ok = true;
        }
        catch (const cg::cli::UsageError& e)
        {
            o.threw_usage = true;
            o.message = e.what();
            o.usage = e.usage();
        }
        catch (const std::runtime_error& e)
        {
            o.threw_runtime = true;
            o.message = e.what();
        }
        return o;
    }

    // ccg msbuild 操作数解析矩阵：extract 模式只接受 .sln
    // （扩展名匹配大小写不敏感）；.vcxproj、目录与其它文件
    // 输入是 UsageError（退出码 2、可操作信息 +
    // 用法块）；操作数路径缺失是普通运行时错误（退出码 1）；
    // --list 是纯注册表读——同一批操作数改为定位注册表
    // 根（目录本身就是根，文件只贡献父
    // 目录）且 extract 模式的拒绝不适用。
    void cli_resolve_msbuild_input(R& r)
    {
        namespace fs = std::filesystem;
        using Outcome = ResolveOutcome;

        // extract 模式：已入库 .sln 被接受为（输入，根）。
        const std::string sln = fixture("DemoSln.sln");
        const fs::path    abs_sln = fs::absolute(fs::path{sln});
        const Outcome     got = try_resolve_msbuild_input(sln, false);
        cg::test::check(
            r,
            got.ok,
            std::format(
                "fixture .sln resolves, threw '{}' instead",
                got.message));
        if (got.ok)
        {
            const std::string& input = got.value.first;
            cg::test::check(
                r,
                fs::path{input}.is_absolute()
                    && input.ends_with("/DemoSln.sln"),
                std::format("input is the absolute .sln, got '{}'", input));
            cg::test::check(
                r,
                got.value.second
                    == cg::util::norm_path(abs_sln.parent_path()
                                               .lexically_normal()
                                               .generic_string()),
                std::format(
                    "root is the .sln directory, got '{}'",
                    got.value.second));
            cg::test::check(
                r,
                fs::exists(fs::path{input}),
                "resolved input points back at a real file");
        }

        // 扩展名匹配大小写不敏感（MIXED.SLN 也接受）。
        const fs::path dir =
            fs::temp_directory_path() / "ccg-msbuild-input-test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        {
            std::ofstream out{
                dir / "MIXED.SLN",
                std::ios::binary | std::ios::trunc};
            out << "Microsoft Visual Studio Solution File\n";
        }
        const Outcome upper =
            try_resolve_msbuild_input((dir / "MIXED.SLN").string(), false);
        cg::test::check(
            r,
            upper.ok && upper.value.first.ends_with("/MIXED.SLN"),
            ".sln extension match is case-insensitive");

        // extract 模式拒绝是带可操作信息的 UsageError；
        // 操作数路径缺失是运行时错误，绝非用法错误。
        const Outcome vcx = try_resolve_msbuild_input(
            fixture("msbdemo/msbdemo.vcxproj"),
            false);
        cg::test::check(
            r,
            vcx.threw_usage
                && vcx.message.find("single-project") != std::string::npos,
            "single .vcxproj input rejected as a usage error");
        const Outcome directory =
            try_resolve_msbuild_input(fixture("rsp"), false);
        cg::test::check(
            r,
            directory.threw_usage
                && directory.message.find("directory input")
                       != std::string::npos,
            "directory input rejected as a usage error");
        const Outcome notsln =
            try_resolve_msbuild_input(fixture("rsp/test.rsp"), false);
        cg::test::check(
            r,
            notsln.threw_usage
                && notsln.message.find(".sln") != std::string::npos,
            "non-.sln file input rejected as a usage error");
        cg::test::check(
            r,
            vcx.threw_usage && !vcx.usage.empty(),
            "usage error carries the usage block (exit 2 shape)");
        const Outcome absent =
            try_resolve_msbuild_input(fixture("no-such.sln"), false);
        cg::test::check(
            r,
            absent.threw_runtime && !absent.threw_usage
                && absent.message.find("not found") != std::string::npos,
            "missing operand path is a runtime error, not a usage error");

        // --list 双语义：同操作数、注册表根含义；
        // .vcxproj 拒绝不适用（--list 不发生构建）。
        const Outcome list_dir =
            try_resolve_msbuild_input(fixture("rsp"), true);
        cg::test::check(
            r,
            list_dir.ok && list_dir.value.first.empty()
                && list_dir.value.second
                       == cg::util::norm_path(
                           fs::absolute(fs::path{fixture("rsp")})
                               .lexically_normal()
                               .generic_string()),
            "--list on a directory: root is the directory itself");
        const Outcome list_file = try_resolve_msbuild_input(sln, true);
        cg::test::check(
            r,
            list_file.ok && list_file.value.first.empty()
                && list_file.value.second == got.value.second,
            "--list on a file: root is the file's parent directory");
        const Outcome list_vcx =
            try_resolve_msbuild_input(fixture("msbdemo/msbdemo.vcxproj"), true);
        cg::test::check(
            r,
            list_vcx.ok && list_vcx.value.first.empty()
                && list_vcx.value.second.ends_with("/msbdemo"),
            "--list accepts a .vcxproj operand (registry read only)");
        fs::remove_all(dir, ec);
    }
    const cg::test::Registrar reg_resolve_input{
        "msbuild.cli_resolve_msbuild_input",
        &cli_resolve_msbuild_input};

#if defined(_WIN32)
    /** 环境块到 (名, 值) 条目表的走查（畸形条目处停止）。 */
    [[nodiscard]]
    auto env_block_entries(const std::wstring& block)
        -> std::vector<std::pair<std::wstring, std::wstring>>
    {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        std::size_t                                        at = 0;
        while (at < block.size() && block[at] != L'\0')
        {
            const std::size_t eq = block.find(L'=', at);
            const std::size_t end = block.find(L'\0', at);
            if (eq == std::wstring::npos || eq > end)
            {
                break; // 畸形条目：停止走查（永不自建）
            }
            out.emplace_back(
                block.substr(at, eq - at),
                block.substr(eq + 1, end - eq - 1));
            at = end + 1;
        }
        return out;
    }

    /** 宽串转小写（ASCII 段）。 */
    [[nodiscard]]
    auto wide_lower(std::wstring s) -> std::wstring
    {
        std::ranges::transform(
            s,
            s.begin(),
            [](wchar_t c)
            {
                return c >= L'A' && c <= L'Z'
                           ? static_cast<wchar_t>(c - L'A' + L'a')
                           : c;
            });
        return s;
    }

    /** 条目表里指定名（小写比对）的条数。 */
    [[nodiscard]]
    auto env_entries_named(
        const std::vector<std::pair<std::wstring, std::wstring>>& entries,
        const wchar_t* name) -> std::size_t
    {
        std::size_t n = 0;
        for (const auto& [entry_name, value] : entries)
        {
            static_cast<void>(value);
            if (wide_lower(entry_name) == name)
            {
                ++n;
            }
        }
        return n;
    }

    // build_child_env：子环境块 = 当前块减去
    // 移除清单减去被覆盖名，再加上新覆盖；名字
    // 匹配大小写不敏感（Windows 环境名语义——进程
    // 块里 PATH 按设置时的拼写存在，大小写敏感比较
    // 会让两种拼写并存）；块以双 NUL 结尾、覆盖按
    // 实参顺序追加到尾部、移除只修剪子
    // 块（ccg 进程环境不动）。
    void pipeline_child_env(R& r)
    {

        // 基线块：双 NUL 结尾、按实际拼写恰好携带
        // 进程 PATH 一次。
        const std::wstring baseline = mb::detail::build_child_env({}, {});
        cg::test::check(
            r,
            baseline.size() >= 2 && baseline[baseline.size() - 1] == L'\0'
                && baseline[baseline.size() - 2] == L'\0',
            "env block is double-NUL terminated");
        const std::vector<std::pair<std::wstring, std::wstring>> base =
            env_block_entries(baseline);
        const std::size_t base_paths = env_entries_named(base, L"path");
        if (base_paths != 1)
        {
            cg::test::check(
                r,
                true,
                "child-env checks skipped: block PATH count != 1");
            return;
        }
        std::wstring path_name;
        for (const auto& [entry_name, value] : base)
        {
            static_cast<void>(value);
            if (wide_lower(entry_name) == L"path")
            {
                path_name = entry_name;
            }
        }

        // 大小写不敏感覆盖：翻转大小写的拼写仍替换
        // 既有条目（大小写敏感比较会两个都留）。
        std::wstring flipped;
        for (const wchar_t c : path_name)
        {
            flipped.push_back(
                c >= L'A' && c <= L'Z'
                    ? static_cast<wchar_t>(c - L'A' + L'a')
                    : (c >= L'a' && c <= L'z'
                           ? static_cast<wchar_t>(c - L'a' + L'A')
                           : c));
        }
        const std::wstring overridden =
            mb::detail::build_child_env({{flipped, L"ccg-child-env-stub"}}, {});
        const auto over = env_block_entries(overridden);
        cg::test::check(
            r,
            env_entries_named(over, L"path") == 1,
            "case-flipped override still replaces PATH (one entry)");
        bool stubbed = false;
        for (const auto& [entry_name, value] : over)
        {
            if (wide_lower(entry_name) == L"path")
            {
                stubbed = value == L"ccg-child-env-stub";
            }
        }
        cg::test::check(
            r,
            stubbed,
            "the override value wins under any spelling");

        // 大小写不敏感移除：翻转拼写修剪该条目。
        const std::wstring removed = mb::detail::build_child_env({}, {flipped});
        cg::test::check(
            r,
            env_entries_named(env_block_entries(removed), L"path") == 0,
            "case-flipped remove name still prunes PATH");
        // 移除只修剪子块；进程环境保住 PATH。
        cg::test::check(
            r,
            cg::util::cstd::get_env("PATH").has_value(),
            "remove affects only the child block, not the process");

        // 覆盖按实参顺序追加到尾部。
        const std::wstring tail = mb::detail::build_child_env(
            {{L"ccg_child_env_one", L"1"}, {L"CCG_CHILD_ENV_TWO", L"2"}},
            {});
        const auto tail_entries = env_block_entries(tail);
        cg::test::check(
            r,
            tail_entries.size() >= 2
                && tail_entries[tail_entries.size() - 2].first
                       == L"ccg_child_env_one"
                && tail_entries[tail_entries.size() - 2].second == L"1"
                && tail_entries[tail_entries.size() - 1].first
                       == L"CCG_CHILD_ENV_TWO"
                && tail_entries[tail_entries.size() - 1].second == L"2",
            "overrides append at the block tail in argument order");
    }
    const cg::test::Registrar reg_child_env{
        "msbuild.pipeline_child_env",
        &pipeline_child_env};
#endif // defined(_WIN32)

    // 每工程 cl 记录的注册表往返：cl_projects 的每个
    // 字段（project / cl / resolution，含诚实的 "" 空形态；披露键
    // dir / active_cfg / in_build——in_build 只在确定知道时写键，false
    // 如实持久化，未知保持键缺席）加条目级 cl_resolution 经存 + 取
    // 幸存，且序列化文本按每记录拼写携带数组（加载侧的空字符串回退
    // 绝不能伪造绿灯）。字段出现之前的注册表（无 cl_projects /
    // cl_resolution / mode / 披露键）仍可加载：缺失字段读回为空、
    // 绝不抛。
    void pipeline_registry_cl_projects(R& r)
    {
        namespace fs = std::filesystem;
        const fs::path dir =
            fs::temp_directory_path() / "ccg-msbuild-reg-clproj";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const std::string path = (dir / "msbuild.json").string();

        mb::Registry reg;
        reg.default_config = "Debug-x64";
        mb::RegistryEntry e;
        e.name = "Debug-x64";
        e.configuration = "Debug";
        e.platform = "x64";
        e.solution = "DemoSln.sln";
        e.db = ".ccg/Debug-x64.db";
        e.compile_commands = ".ccg/msbuild/Debug-x64/compile_commands.json";
        e.msbuild = "E:/VS/MSBuild/Current/Bin/MSBuild.exe";
        e.cl = "C:/VS/14.51/bin/HostX86/x64/cl.exe";
        e.cl_resolution = "vswhere-fallback";
        e.mode = "record";
        e.extracted_at = "2026-10-20T00:00:00Z";
        e.tus = 3;
        e.failed = 0;
        e.cl_projects = {
            mb::ProjectClEntry{
                .project = "projA",
                .cl = "C:/VS/14.44/bin/HostX86/x64/cl.exe",
                .resolution = "override",
                .dir = "E:/src/DemoSln/projA",
                .active_cfg = "Debug|x64",
                .in_build = true},
            mb::ProjectClEntry{
                .project = "projB",
                .cl = "C:/VS/14.51/bin/HostX86/x64/cl.exe",
                .resolution = "project-property",
                .dir = "E:/src/DemoSln/projB",
                .active_cfg = "Debug|Win32",
                .in_build = false},
            mb::ProjectClEntry{.project = "projC", .cl = "", .resolution = ""},
        };
        reg.entries = {e};

        mb::save_registry(path, reg);
        const std::optional<mb::Registry> loaded = mb::load_registry(path);
        cg::test::check(
            r,
            loaded.has_value() && loaded->entries.size() == 1,
            "registry with cl_projects loads as one entry");
        if (loaded && loaded->entries.size() == 1)
        {
            const mb::RegistryEntry& back = loaded->entries.front();
            cg::test::check(
                r,
                back.cl_resolution == "vswhere-fallback",
                "entry-level cl_resolution round-trips");
            const std::size_t n = std::min<std::size_t>(
                back.cl_projects.size(),
                e.cl_projects.size());
            cg::test::check(
                r,
                back.cl_projects.size() == 3,
                std::format(
                    "3 per-project records, got {}",
                    back.cl_projects.size()));
            for (std::size_t i = 0; i < n; ++i)
            {
                cg::test::check(
                    r,
                    back.cl_projects[i].project == e.cl_projects[i].project
                        && back.cl_projects[i].cl == e.cl_projects[i].cl
                        && back.cl_projects[i].resolution
                               == e.cl_projects[i].resolution
                        && back.cl_projects[i].dir == e.cl_projects[i].dir
                        && back.cl_projects[i].active_cfg
                               == e.cl_projects[i].active_cfg
                        && back.cl_projects[i].in_build
                               == e.cl_projects[i].in_build,
                    std::format(
                        "cl_projects[{}] {} round-trips (project/cl/"
                        "resolution/dir/active_cfg/in_build)",
                        i,
                        e.cl_projects[i].project));
            }
        }

        // 序列化文本携带数组 + 每记录拼写。
        const json::Document  doc = json::Document::parse(read_file(path));
        const json::ValueView root = doc.root();
        const auto            configs = root.get("configs");
        const auto cps = configs && configs->is_array() && configs->size() == 1
                             ? configs->at(0)->get("cl_projects")
                             : std::optional<json::ValueView>{};
        cg::test::check(
            r,
            cps && cps->is_array() && cps->size() == 3,
            "cl_projects serialized as a 3-element array");
        if (cps && cps->is_array() && cps->size() == 3)
        {
            const auto r0 = cps->at(0)->get("resolution");
            const auto c2 = cps->at(2)->get("cl");
            cg::test::check(
                r,
                r0 && r0->is_string()
                    && *r0->as_string() == std::string_view{"override"} && c2
                    && c2->is_string() && c2->as_string()->empty(),
                "per-record resolution spelling and empty cl persisted");
            // 披露键：有值逐键写出（false 如实写 false），未知（projC）
            // 键缺席——序列化文本与读回口径一致。
            const auto        d0 = cps->at(0)->get("dir");
            const auto        a0 = cps->at(0)->get("active_cfg");
            const auto        b0 = cps->at(0)->get("in_build");
            const auto        b1 = cps->at(1)->get("in_build");
            const auto        d2 = cps->at(2)->get("dir");
            const auto        a2 = cps->at(2)->get("active_cfg");
            const auto        b2 = cps->at(2)->get("in_build");
            const std::string text = read_file(path);
            cg::test::check(
                r,
                d0 && d0->is_string()
                    && *d0->as_string()
                           == std::string_view{"E:/src/DemoSln/projA"}
                    && a0 && a0->is_string()
                    && *a0->as_string() == std::string_view{"Debug|x64"} && b0
                    && b0->is_bool() && *b0->as_bool() && b1 && b1->is_bool()
                    && !*b1->as_bool(),
                "disclosure keys dir/active_cfg/in_build persisted "
                "(false spelled as false)");
            cg::test::check(
                r,
                !d2.has_value() && !a2.has_value() && !b2.has_value()
                    && text.find("\"projC\"") != std::string::npos
                    && text.find("in_build") != std::string::npos,
                "unknown disclosure facts keep keys absent (projC)");
        }

        // 字段前注册表（无 cl_projects / cl_resolution / mode）：宽松
        // 加载——缺失字段回来为空、已记录字段完好。
        {
            std::ofstream old{
                fs::path{path},
                std::ios::binary | std::ios::trunc};
            old << R"({"version":1,"generator":"msbuild","default":"Debug-x64",)"
                << R"("configs":[{"name":"Debug-x64","configuration":"Debug",)"
                << R"("platform":"x64","solution":"DemoSln.sln",)"
                << R"("db":".ccg/Debug-x64.db",)"
                << R"("compile_commands":".ccg/msbuild/Debug-x64/compile_commands.json",)"
                << R"("msbuild":"E:/VS/MSBuild.exe","cl":"E:/VS/cl.exe",)"
                << R"("extracted_at":"2026-01-01T00:00:00Z","tus":2,"failed":0}]})";
        }
        const std::optional<mb::Registry> old = mb::load_registry(path);
        cg::test::check(
            r,
            old.has_value() && old->entries.size() == 1,
            "old-format registry still loads as one entry");
        if (old && old->entries.size() == 1)
        {
            const mb::RegistryEntry& back = old->entries.front();
            cg::test::check(
                r,
                back.cl_projects.empty() && back.cl_resolution.empty()
                    && back.mode.empty(),
                "absent cl fields read back empty (lenient)");
            cg::test::check(
                r,
                back.db == ".ccg/Debug-x64.db" && back.cl == "E:/VS/cl.exe"
                    && back.tus == 2,
                "old-format entry keeps its recorded fields");
        }

        // cl_projects 内的非对象元素被跳过，绝不致命；披露键类型不符
        // （in_build 字符串形态）按未知处理，绝不臆造。
        {
            std::ofstream holes{
                fs::path{path},
                std::ios::binary | std::ios::trunc};
            holes << R"({"version":1,"generator":"msbuild","default":"d",)"
                  << R"("configs":[{"name":"d","configuration":"Debug",)"
                  << R"("platform":"x64","cl":"c:/cl.exe",)"
                  << R"("cl_projects":[null,"noise",)"
                  << R"({"project":"p","cl":"c:/p.exe",)"
                  << R"("resolution":"override",)"
                  << R"("in_build":"maybe"}]}]})";
        }
        const std::optional<mb::Registry> holey = mb::load_registry(path);
        cg::test::check(
            r,
            holey.has_value() && holey->entries.size() == 1
                && holey->entries[0].cl_projects.size() == 1
                && holey->entries[0].cl_projects[0].project == "p"
                && holey->entries[0].cl_projects[0].resolution == "override",
            "non-object cl_projects elements are skipped");
        if (holey && !holey->entries.empty()
            && !holey->entries[0].cl_projects.empty())
        {
            cg::test::check(
                r,
                !holey->entries[0].cl_projects[0].in_build.has_value(),
                "non-bool in_build reads as unknown, not false");
        }
        fs::remove_all(dir, ec);
    }
    const cg::test::Registrar reg_registry_cl_projects{
        "msbuild.pipeline_registry_cl_projects",
        &pipeline_registry_cl_projects};

    // project_refresh_target_for：决定排队的工程文件行是否
    // 触发重捕获、以及为哪个配置
    // 触发的注册表反查。内存注册表上的纯函数——无文件、
    // 无进程。
    void pipeline_refresh_target_cases(R& r)
    {
        cg::msbuild::Registry      reg;
        cg::msbuild::RegistryEntry debug;
        debug.name = "Debug-x64";
        debug.configuration = "Debug";
        debug.platform = "x64";
        debug.solution = "DemoSln.sln";
        reg.entries.push_back(debug);
        cg::msbuild::RegistryEntry release;
        release.name = "Release-x64";
        release.configuration = "Release";
        release.platform = "x64";
        release.solution = "DemoSln.sln";
        reg.entries.push_back(release);
        const std::string root = "x:/ccg-fake/work/proj";

        // 精确解决方案匹配（注册表相对拼写在工程
        // 根下解析；两侧大小写不敏感）。
        auto t = cg::msbuild::project_refresh_target_for(
            reg,
            root,
            "Debug-x64",
            "X:/ccg-fake/work/proj/demosln.sln");
        cg::test::check(r, t.has_value(), "solution line resolves");
        if (t.has_value())
        {
            cg::test::check(
                r,
                t->stem == "Debug-x64" && t->configuration == "Debug"
                    && t->platform == "x64",
                "target carries the matching entry's config");
            cg::test::check(
                r,
                t->solution == "x:/ccg-fake/work/proj/DemoSln.sln",
                "solution is resolved to an absolute forward-slash path "
                "(lowercased drive, original file casing kept)");
        }

        // 工程根下任何工程文件触发该配置的条目。
        t = cg::msbuild::project_refresh_target_for(
            reg,
            root,
            "Release-x64",
            "x:/ccg-fake/work/proj/msbdemo/dir.props");
        cg::test::check(
            r,
            t.has_value() && t->stem == "Release-x64",
            "props under the root map to the db's own config");

        // 根本身不算匹配（需要正经的前缀边界）。
        cg::test::check(
            r,
            !cg::msbuild::project_refresh_target_for(
                 reg,
                 root,
                 "Debug-x64",
                 "x:/ccg-fake/work/projish/x.props")
                 .has_value(),
            "sibling directory sharing the name prefix does not match");

        // 根之外且非解决方案：无联动。
        cg::test::check(
            r,
            !cg::msbuild::project_refresh_target_for(
                 reg,
                 root,
                 "Debug-x64",
                 "x:/ccg-fake/elsewhere/shared.props")
                 .has_value(),
            "root-external props file does not trigger");

        // 未知配置名（该库不是 msbuild 配置）：无联动。
        cg::test::check(
            r,
            !cg::msbuild::project_refresh_target_for(
                 reg,
                 root,
                 "fallback",
                 "x:/ccg-fake/work/proj/msbdemo/a.vcxproj")
                 .has_value(),
            "config without a registry entry does not trigger");

        // 绝对注册表拼写（解决方案在工程根外）在
        // 行恰为该解决方案时仍解析。
        cg::msbuild::Registry      abs_reg;
        cg::msbuild::RegistryEntry abs_entry;
        abs_entry.name = "Debug-x64";
        abs_entry.configuration = "Debug";
        abs_entry.platform = "x64";
        abs_entry.solution = "x:/ccg-fake/elsewhere/DemoSln.sln";
        abs_reg.entries.push_back(abs_entry);
        t = cg::msbuild::project_refresh_target_for(
            abs_reg,
            root,
            "Debug-x64",
            "x:/ccg-fake/elsewhere/DemoSln.sln");
        cg::test::check(
            r,
            t.has_value() && t->stem == "Debug-x64",
            "absolute registry spelling matches the solution line");
        cg::test::check(
            r,
            !cg::msbuild::project_refresh_target_for(
                 abs_reg,
                 root,
                 "Debug-x64",
                 "x:/ccg-fake/elsewhere/other.props")
                 .has_value(),
            "absolute-spelling entry does not widen to root-external props");

        // 防御：空配置名 / 根 / 行永不匹配。
        cg::test::check(
            r,
            !cg::msbuild::project_refresh_target_for(
                 reg,
                 root,
                 "",
                 "x:/ccg-fake/work/proj/a.sln")
                    .has_value()
                && !cg::msbuild::project_refresh_target_for(
                        reg,
                        "",
                        "Debug-x64",
                        "x:/ccg-fake/work/proj/a.sln")
                        .has_value()
                && !cg::msbuild::project_refresh_target_for(
                        reg,
                        root,
                        "Debug-x64",
                        "")
                        .has_value(),
            "empty inputs are rejected");
    }

    const cg::test::Registrar reg_refresh_target{
        "msbuild.pipeline_refresh_target",
        &pipeline_refresh_target_cases};

    // ------------------------------------------------------------------
    // 求值通道（MsbuildEval 对象过桥拿结构化元数据）：发现链文本规则
    // 跨平台恒跑；会话验证钉 Windows（51 线 CLR 宿主），本机无 VS 时
    // 打说明行跳过（宁漏方向——CI 环境不含 VS 时不红）。
    // ------------------------------------------------------------------

    /** 求值会话的环境闸：本机有 MSBuild（发现链锚）。 */
    [[nodiscard]]
    auto eval_env_ready() -> std::string
    {
        return mb::find_msbuild("");
    }

    /** TU 的 file/abs 字段归一（小写、正斜杠）供跨形态断言比较。 */
    [[nodiscard]]
    auto norm_slash(std::string s) -> std::string
    {
        std::ranges::replace(s, '\\', '/');
        return cg::util::ascii_lower(s);
    }

    /** fs::path 的 utf-8 文本形态（路径经 find/msbuild/求值三处流转时
     *  保持同一编码口径）。 */
    [[nodiscard]]
    auto path_utf8(const std::filesystem::path& p) -> std::string
    {
        return cg::util::platform::narrow_utf8(p.wstring());
    }

    /** 对 DemoSln 的一个成员工程做关键字段深度断言（工具集/标准/
     *  defines/TU 清单）。 */
    void expect_project_fields(
        R&                                 r,
        const ev::EvalProjectOut&          p,
        std::string_view                   toolset,
        std::string_view                   lang_std,
        std::string_view                   define_needle,
        std::initializer_list<const char*> tu_files)
    {
        cg::test::check(
            r,
            p.error.empty(),
            std::format("{} 求值无错误: {}", p.name, p.error));
        cg::test::check(
            r,
            p.platform_toolset == toolset,
            std::format(
                "{} 工具集 '{}' vs '{}'",
                p.name,
                p.platform_toolset,
                toolset));
        cg::test::check(
            r,
            p.language_standard == lang_std,
            std::format(
                "{} 语言标准 '{}' vs '{}'",
                p.name,
                p.language_standard,
                lang_std));
        if (!define_needle.empty())
        {
            cg::test::check(
                r,
                p.defines.find(define_needle) != std::string::npos,
                std::format(
                    "{} defines 含 '{}'（实值 '{}'）",
                    p.name,
                    define_needle,
                    p.defines));
        }
        if (tu_files.size() > 0)
        {
            cg::test::check(
                r,
                p.tu_count == tu_files.size()
                    && p.tus.size() == tu_files.size(),
                std::format(
                    "{} TU 计数 {} 条（期望 {}）",
                    p.name,
                    p.tus.size(),
                    tu_files.size()));
            std::set<std::string> want;
            std::set<std::string> got;
            for (const char* f : tu_files)
            {
                want.insert(cg::util::ascii_lower(f));
            }
            for (const ev::EvalTu& tu : p.tus)
            {
                got.insert(norm_slash(tu.file));
                cg::test::check(
                    r,
                    !tu.abs.empty(),
                    std::format("TU '{}' 绝对路径非空", tu.file));
            }
            cg::test::check(
                r,
                got == want,
                std::format("{} TU 清单对齐", p.name));
        }
        cg::test::check(
            r,
            p.raw_json.find("cl_item_definition") != std::string::npos
                && p.raw_json.find("import_closure") != std::string::npos,
            std::format("{} 原始载荷含 37 字段面与 import 闭包", p.name));
    }

    /** 发现链推导的纯文本规则（跨平台）。 */
    void eval_discovery_text_cases(R& r)
    {
        cg::test::check(
            r,
            ev::vs_version_of("x:/ccg-fake/vs/18/Community") == "18.0",
            "主版本段 + .0");
        cg::test::check(
            r,
            ev::vs_version_of("x:/ccg-fake/vs/2022/BuildTools") == "2022.0",
            "版本段非主版本号形态时按规则原样拼段（已知局限记档）");
        cg::test::check(
            r,
            ev::vs_version_of("x:/ccg-fake/vsroot") == "",
            "无版本段的根拒绝");
        cg::test::check(
            r,
            ev::vs_version_of("") == "" && ev::vc_targets_dir_of("") == "",
            "空输入拒绝");
        cg::test::check(
            r,
            ev::vc_targets_dir_of("x:/ccg-fake/nowhere") == "",
            "不存在的 VS 根无 VC targets 候选");
        cg::test::check(
            r,
            !ev::build_evaluate_request(
                 "",
                 fixture("DemoSln.sln"),
                 "Debug",
                 "x64")
                    .has_value()
                && !ev::build_evaluate_request(
                        "x:/ccg-fake/msbuild.exe",
                        "",
                        "Debug",
                        "x64")
                        .has_value(),
            "空 msbuild/sln 输入拒绝");
    }

    /** 刷新快照：构建（输入集合并/去重/规范化/缺盘记空）、存取往返
     *  与哈希门判定矩阵（纯文件面，跨平台——不依赖 51 线桥）。 */
    /** 快照哈希门是否判变（非空原因即变）。 */
    [[nodiscard]]
    auto snapshot_mismatches(
        const ev::RefreshSnapshot& s,
        const std::string&         msbuild,
        const std::string&         cl,
        const std::string&         vc_targets) -> bool
    {
        return !ev::refresh_snapshot_unchanged(s, msbuild, cl, vc_targets)
                    .empty();
    }

    /** 刷新快照里指定路径的文件条目（未命中给空）。 */
    [[nodiscard]]
    auto snapshot_file_of(const ev::RefreshSnapshot& snap, const std::string& p)
        -> const ev::RefreshSnapshotFile*
    {
        const auto it = std::ranges::find(
            snap.files,
            cg::util::norm_path(p),
            &ev::RefreshSnapshotFile::path);
        return it == snap.files.end() ? nullptr : &*it;
    }

    /** 文本内容的 sha256（十六进制）。 */
    [[nodiscard]]
    auto content_sha256(std::string_view text) -> std::string
    {
        return cg::util::sha256_hex(std::as_bytes(std::span{text}));
    }

    void eval_snapshot_cases(R& r)
    {
        namespace fs = std::filesystem;
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path() / "ccg-eval-snapshot"};
        const std::string sln = path_utf8(scratch.base / "demo.sln");
        const std::string va = path_utf8(scratch.base / "a/a.vcxproj");
        const std::string vb = path_utf8(scratch.base / "b/b.vcxproj");
        const std::string shared = path_utf8(scratch.base / "shared.props");
        const std::string gone = path_utf8(scratch.base / "gone.props");
        scratch.put(scratch.base / "demo.sln", "sln-bytes");
        scratch.put(scratch.base / "a/a.vcxproj", "a-bytes");
        scratch.put(scratch.base / "b/b.vcxproj", "b-bytes");
        scratch.put(scratch.base / "shared.props", "shared-bytes");
        // 注册表身份面占位（门只校验"路径在盘上"，不执行）。
        const std::string fake_msbuild =
            path_utf8(scratch.base / "MSBuild.exe");
        const std::string fake_cl = path_utf8(scratch.base / "cl.exe");
        scratch.put(scratch.base / "MSBuild.exe", "msbuild-bytes");
        scratch.put(scratch.base / "cl.exe", "cl-bytes");

        ev::EvalRequest req;
        req.solution = sln;
        req.vc_targets = "x:/ccg-fake/VC/v170\\";
        ev::EvalProjectIn in_a;
        in_a.vcxproj = va;
        ev::EvalProjectIn in_b;
        in_b.vcxproj = vb;
        req.projects = {in_a, in_b};

        ev::EvalProjectOut out_a;
        ev::EvalProjectOut out_b;
        // 闭包形态实证：正斜杠/反斜杠同文件（去重）、"?" 占位（跳过）、
        // 缺盘文件（哈希记 ""，两侧同缺即稳定）。
        std::string back_slash = shared;
        std::ranges::replace(back_slash, '/', '\\');
        out_a.import_closure = {shared, back_slash, "?", gone};
        out_b.import_closure = {shared};

        const ev::RefreshSnapshot snap = ev::build_refresh_snapshot(
            fake_msbuild,
            fake_cl,
            req,
            {out_a, out_b});

        // 输入集 = sln + 两 vcxproj + shared（两工程闭包去重后一条）+
        // gone = 5 条。
        cg::test::check(
            r,
            snap.files.size() == 5,
            std::format(
                "输入集恰 5 条（sln/两 vcxproj/shared 去重/gone），实值 {}",
                snap.files.size()));
        if (snap.files.size() == 5)
        {
            cg::test::check(
                r,
                std::ranges::is_sorted(
                    snap.files,
                    {},
                    &ev::RefreshSnapshotFile::path),
                "文件条目按路径字典序（确定性输出）");
        }
        if (const ev::RefreshSnapshotFile* f = snapshot_file_of(snap, shared))
        {
            cg::test::check(
                r,
                f->sha256 == content_sha256("shared-bytes"),
                "shared.props 哈希与内容对齐");
            cg::test::check(
                r,
                f->path.find('\\') == std::string::npos,
                "闭包反斜杠形态规范化为正斜杠");
        }
        else
        {
            cg::test::check(r, false, "shared.props 在快照输入集内");
        }
        if (const ev::RefreshSnapshotFile* f = snapshot_file_of(snap, gone))
        {
            cg::test::check(r, f->sha256.empty(), "缺盘闭包文件哈希记空串");
        }
        else
        {
            cg::test::check(r, false, "缺盘闭包文件仍入输入集");
        }
        if (const ev::RefreshSnapshotFile* f = snapshot_file_of(snap, sln))
        {
            cg::test::check(
                r,
                f->sha256 == content_sha256("sln-bytes"),
                "sln 哈希与内容对齐");
        }
        const std::string live_include =
            cg::util::cstd::get_env("INCLUDE").value_or("");
        cg::test::check(
            r,
            snap.env.msbuild == cg::util::norm_path(fake_msbuild)
                && snap.env.cl == cg::util::norm_path(fake_cl)
                && snap.env.vc_targets == req.vc_targets
                && snap.env.include == content_sha256(live_include),
            "环境身份四元组齐备（INCLUDE 为当前进程哈希）");
        cg::test::check(
            r,
            snap.ccg_version == cg::util::version,
            "ccg 版本入快照");
        cg::test::check(r, !snap.written_at.empty(), "写出时间戳非空");

        // 路径单点与存取往返。
        const std::string snapshot_path =
            ev::refresh_snapshot_path(path_utf8(scratch.base), "Debug-x64");
        cg::test::check(
            r,
            snapshot_path.find("/.ccg/msbuild/Debug-x64/snapshot.json")
                != std::string::npos,
            std::format("快照路径与捕获日志同区: {}", snapshot_path));
        ev::save_refresh_snapshot(snapshot_path, snap);
        cg::test::check(
            r,
            !fs::exists(fs::path{snapshot_path + ".tmp"}),
            "原子替换不留 .tmp 暂存");
        const auto loaded = ev::load_refresh_snapshot(snapshot_path);
        bool       roundtrip = loaded.has_value();
        if (loaded)
        {
            roundtrip = roundtrip && loaded->files.size() == snap.files.size();
            for (std::size_t i = 0; roundtrip && i < snap.files.size(); ++i)
            {
                roundtrip = loaded->files[i].path == snap.files[i].path
                            && loaded->files[i].sha256 == snap.files[i].sha256;
            }
            roundtrip = roundtrip && loaded->env.include == snap.env.include
                        && loaded->env.msbuild == snap.env.msbuild
                        && loaded->env.cl == snap.env.cl
                        && loaded->env.vc_targets == snap.env.vc_targets
                        && loaded->ccg_version == snap.ccg_version
                        && loaded->written_at == snap.written_at;
        }
        cg::test::check(r, roundtrip, "存取往返逐字段幸存");

        // 哈希门判定矩阵：全等 -> 空；各失配 -> 非空人读原因。
        cg::test::check(
            r,
            ev::refresh_snapshot_unchanged(
                snap,
                fake_msbuild,
                fake_cl,
                req.vc_targets)
                .empty(),
            "全等判定（注册表路径在盘 + 推导一致）");
        scratch.put(scratch.base / "a/a.vcxproj", "a-bytes-2");
        cg::test::check(
            r,
            snapshot_mismatches(snap, fake_msbuild, fake_cl, req.vc_targets),
            "成员 vcxproj 内容变 -> 判变");
        scratch.put(scratch.base / "a/a.vcxproj", "a-bytes");
        fs::remove(scratch.base / "shared.props", scratch.ec);
        cg::test::check(
            r,
            snapshot_mismatches(snap, fake_msbuild, fake_cl, req.vc_targets),
            "闭包 props 被删 -> 判变（非空记录 vs 缺盘）");
        scratch.put(scratch.base / "shared.props", "shared-bytes");
        ev::RefreshSnapshot bump = snap;
        bump.ccg_version = "0.0.0-sentinel";
        cg::test::check(
            r,
            snapshot_mismatches(bump, fake_msbuild, fake_cl, req.vc_targets),
            "ccg 版本变 -> 判变");
        bump = snap;
        bump.env.include = "deadbeef";
        cg::test::check(
            r,
            snapshot_mismatches(bump, fake_msbuild, fake_cl, req.vc_targets),
            "INCLUDE 哈希变 -> 判变");
        bump = snap;
        bump.env.msbuild = "x:/ccg-fake/other/MSBuild.exe";
        cg::test::check(
            r,
            !ev::refresh_snapshot_unchanged(
                 bump,
                 fake_msbuild,
                 fake_cl,
                 req.vc_targets)
                 .empty(),
            "注册表 msbuild 与快照不一致 -> 判变");
        cg::test::check(
            r,
            !ev::refresh_snapshot_unchanged(
                 snap,
                 path_utf8(scratch.base / "vanished-MSBuild.exe"),
                 fake_cl,
                 req.vc_targets)
                 .empty(),
            "注册表 msbuild 路径缺盘（发现缓存将失效）-> 判变");
        cg::test::check(
            r,
            !ev::refresh_snapshot_unchanged(
                 snap,
                 fake_msbuild,
                 path_utf8(scratch.base / "vanished-cl.exe"),
                 req.vc_targets)
                 .empty(),
            "注册表 cl 路径缺盘 -> 判变");
        cg::test::check(
            r,
            !ev::refresh_snapshot_unchanged(
                 snap,
                 fake_msbuild,
                 fake_cl,
                 req.vc_targets + "x")
                 .empty(),
            "VCTargetsPath 推导变 -> 判变");
        bump = snap;
        bump.files.clear();
        cg::test::check(
            r,
            snapshot_mismatches(bump, fake_msbuild, fake_cl, req.vc_targets),
            "零文件条目（形坏）-> 判变");
        scratch.put(
            scratch.base / ".ccg/msbuild/Debug-x64/snapshot.json",
            "{not json");
        cg::test::check(
            r,
            !ev::load_refresh_snapshot(snapshot_path).has_value(),
            "坏快照行 -> nullopt（读侧按无快照处理）");
    }

    /** DemoSln 定向验证：两工程求值过桥，TU 清单与关键字段逐项断言。 */
    void eval_demosln_cases(R& r)
    {
#if defined(_WIN32)
        const std::string msbuild = eval_env_ready();
        if (msbuild.empty())
        {
            std::println("eval_demosln: skip, no MSBuild on machine");
            return;
        }
        const auto req = ev::build_evaluate_request(
            msbuild,
            fixture("DemoSln.sln"),
            "Debug",
            "x64");
        cg::test::check(r, req.has_value(), "DemoSln 请求构建成功");
        if (!req.has_value())
        {
            return;
        }
        cg::test::check(
            r,
            !req->assembly_dir.empty() && !req->vs_root.empty()
                && !req->vs_version.empty(),
            std::format(
                "发现链推导齐备: dir={} root={} ver={}",
                req->assembly_dir,
                req->vs_root,
                req->vs_version));
        cg::test::check(
            r,
            !req->vc_targets.empty() && req->vc_targets.back() == '\\',
            std::format("VCTargetsPath 尾反斜杠硬约束: '{}'", req->vc_targets));
        cg::test::check(
            r,
            req->projects.size() == 2 && req->projects[0].name == "mathlib"
                && req->projects[1].name == "msbdemo",
            "成员清单两工程且保序");
        cg::test::check(
            r,
            req->projects[0].build && req->projects[1].build,
            "DemoSln 两成员都有 Build.0 参与位");

        const ev::EvalOutcome out = ev::evaluate(*req);
        cg::test::check(
            r,
            out.completed,
            std::format("会话拉到 summary: error={}", out.error));
        cg::test::check(
            r,
            out.ok_projects == 2 && out.failed_projects == 0,
            std::format(
                "summary 计数 ok={} failed={}",
                out.ok_projects,
                out.failed_projects));
        cg::test::check(
            r,
            out.mismatches == 0,
            std::format("回放自检零失配（{} 次校验）", out.tu_field_checks));
        if (out.projects.size() != 2)
        {
            return;
        }

        // mathlib：静态库、stdcpp17、MATHLIB_SCALE=3、2 TU（含 ü 名目录）。
        expect_project_fields(
            r,
            out.projects[0],
            "v145",
            "stdcpp17",
            "MATHLIB_SCALE=3",
            {"multiply.cpp", "src-ünikode/scale.cpp"});
        // 绝对路径两重验证：求值出的 abs 能在磁盘上命中真实文件（ü 名
        // 目录段若在过桥时乱码，路径必然落空）且落在工程目录内。
        bool math_abs_ok = true;
        for (const ev::EvalTu& tu : out.projects[0].tus)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(
                    std::filesystem::path{
                        cg::util::platform::widen_utf8(tu.abs)},
                    ec))
            {
                math_abs_ok = false;
            }
            if (norm_slash(tu.abs).find("/mathlib/mathlib/")
                == std::string::npos)
            {
                math_abs_ok = false;
            }
        }
        cg::test::check(
            r,
            math_abs_ok,
            "mathlib TU 绝对路径命中磁盘文件（ü 名目录不乱码）");

        // msbdemo：应用工程、stdcpp20、DEMO_DEFINE=42（Debug 无 NDEBUG）、
        // 3 TU。
        expect_project_fields(
            r,
            out.projects[1],
            "v145",
            "stdcpp20",
            "DEMO_DEFINE=42",
            {"src/main.cpp", "src/probe.cpp", "src/util.cpp"});
        cg::test::check(
            r,
            out.projects[1].defines.find("NDEBUG") == std::string::npos,
            std::format(
                "Debug 配置 defines 不含 NDEBUG（实值 '{}'）",
                out.projects[1].defines));
        cg::test::check(
            r,
            !out.projects[0].degraded_missing_imports
                && !out.projects[1].degraded_missing_imports,
            "两工程都走完整 import 求值（无降级腿）");
#else
        (void)r;
#endif
    }

    /** 夹具解决方案广度验证：msbdemo / modmsb / pchp 三条会话轻断言。 */
    void eval_matrix_cases(R& r)
    {
#if defined(_WIN32)
        const std::string msbuild = eval_env_ready();
        if (msbuild.empty())
        {
            std::println("eval_matrix: skip, no MSBuild on machine");
            return;
        }
        struct Row
        {
            std::string_view sln;
            std::string_view name;
            std::size_t      tus;
            std::string_view lang_std;
        };
        const Row rows[] = {
            {"msbdemo/msbdemo.sln", "msbdemo", 3, "stdcpp20"},
            {"modmsb/modmsb.sln", "g16mod", 2, "stdcpp20"},
            {"pchp/pchp.sln", "pchp", 2, "stdcpp20"},
        };
        for (const Row& row : rows)
        {
            const auto req = ev::build_evaluate_request(
                msbuild,
                fixture(row.sln),
                "Debug",
                "x64");
            cg::test::check(
                r,
                req.has_value(),
                std::format("{} 请求构建成功", row.name));
            if (!req.has_value())
            {
                continue;
            }
            const ev::EvalOutcome out = ev::evaluate(*req);
            cg::test::check(
                r,
                out.completed && out.failed_projects == 0
                    && out.ok_projects == 1,
                std::format(
                    "{} 会话完成且单工程成功: error={}",
                    row.name,
                    out.error));
            cg::test::check(
                r,
                out.mismatches == 0,
                std::format("{} 回放自检零失配", row.name));
            if (out.projects.size() == 1)
            {
                // 清单参数传空 = 跳过 TU 逐条对齐，仅断言关键字段；
                // 计数由下方独立断言承载。
                expect_project_fields(
                    r,
                    out.projects[0],
                    "v145",
                    row.lang_std,
                    "",
                    {});
                cg::test::check(
                    r,
                    out.projects[0].tu_count == row.tus
                        && out.projects[0].tus.size() == row.tus,
                    std::format(
                        "{} TU 计数 {}（期望 {}）",
                        row.name,
                        out.projects[0].tus.size(),
                        row.tus));
            }
        }
        // modmsb 的模块 TU 形态：.ixx 与 .cpp 各一条（纯求值单趟形态）。
        const auto mod_req = ev::build_evaluate_request(
            msbuild,
            fixture("modmsb/modmsb.sln"),
            "Debug",
            "x64");
        if (mod_req.has_value())
        {
            const ev::EvalOutcome out = ev::evaluate(*mod_req);
            if (out.projects.size() == 1)
            {
                std::set<std::string> files;
                for (const ev::EvalTu& tu : out.projects[0].tus)
                {
                    files.insert(norm_slash(tu.file));
                }
                cg::test::check(
                    r,
                    files.count("mod.ixx") == 1 && files.count("use.cpp") == 1,
                    std::format(
                        "g16mod 模块 TU 清单含 mod.ixx 与 use.cpp: 实值"
                        " {} 条",
                        files.size()));
            }
        }
#else
        (void)r;
#endif
    }

    /** Build.0 参与位与 ActiveCfg 映射的成员清单口径（手造 sln 文本）。 */
    void eval_build_flag_cases(R& r)
    {
#if defined(_WIN32)
        const std::string msbuild = eval_env_ready();
        if (msbuild.empty())
        {
            std::println("eval_build_flags: skip, no MSBuild on machine");
            return;
        }
        const cg::test::ScratchDir scratch{
            std::filesystem::temp_directory_path() / "ccg-eval-b0"};
        const std::string text = read_file(fixture("DemoSln.sln"));

        // 形态一：删全部 Build.0 行——成员在场、参与位 false。
        std::string no_build;
        {
            std::istringstream lines{text};
            std::string        line;
            while (std::getline(lines, line))
            {
                cg::util::strip_cr(line);
                if (line.find(".Build.0 = ") == std::string::npos)
                {
                    no_build += line;
                    no_build += '\n';
                }
            }
        }
        scratch.put(scratch.base / "DemoNoBuild.sln", no_build);
        const std::string written =
            read_file(path_utf8(scratch.base / "DemoNoBuild.sln"));
        cg::test::check(
            r,
            written.find("Project(\"") != std::string::npos
                && written.find(".ActiveCfg = ") != std::string::npos,
            std::format("回读手造 sln 完整（{} 字节）", written.size()));
        const auto req_no_build = ev::build_evaluate_request(
            msbuild,
            path_utf8(scratch.base / "DemoNoBuild.sln"),
            "Debug",
            "x64");
        cg::test::check(
            r,
            req_no_build.has_value() && req_no_build->projects.size() == 2,
            std::format(
                "无 Build.0 行时成员仍在清单（has={} n={} 路径={}）",
                req_no_build.has_value(),
                req_no_build.has_value() ? req_no_build->projects.size() : 0,
                path_utf8(scratch.base / "DemoNoBuild.sln")));
        if (req_no_build.has_value() && req_no_build->projects.size() == 2)
        {
            cg::test::check(
                r,
                !req_no_build->projects[0].build
                    && !req_no_build->projects[1].build,
                "无 Build.0 行时参与位 false");
        }

        // 形态二：删 mathlib 的全部配置映射行——无 ActiveCfg 的成员
        // 不进求值清单。
        std::string no_map;
        {
            std::istringstream lines{text};
            std::string        line;
            while (std::getline(lines, line))
            {
                cg::util::strip_cr(line);
                if (line.find("AA12B7C3-4D55-4E86-9A17-987654321000")
                    == std::string::npos)
                {
                    no_map += line;
                    no_map += '\n';
                }
            }
        }
        scratch.put(scratch.base / "DemoNoMap.sln", no_map);
        const auto req_no_map = ev::build_evaluate_request(
            msbuild,
            path_utf8(scratch.base / "DemoNoMap.sln"),
            "Debug",
            "x64");
        cg::test::check(
            r,
            req_no_map.has_value() && req_no_map->projects.size() == 1
                && req_no_map->projects[0].name == "msbdemo",
            "无 ActiveCfg 映射的成员被跳过");
#else
        (void)r;
#endif
    }

    /** 看门狗：1 毫秒预算下会话超时放弃（timed_out 形态、不完成）。 */
    void eval_watchdog_cases(R& r)
    {
#if defined(_WIN32)
        const std::string msbuild = eval_env_ready();
        if (msbuild.empty())
        {
            std::println("eval_watchdog: skip, no MSBuild on machine");
            return;
        }
        const auto req = ev::build_evaluate_request(
            msbuild,
            fixture("DemoSln.sln"),
            "Debug",
            "x64");
        cg::test::check(r, req.has_value(), "看门狗用例请求构建成功");
        if (!req.has_value())
        {
            return;
        }
        cg::test::EnvScope watchdog{"CCG_EVAL_TIMEOUT_MS"};
        watchdog.set("1");
        const ev::EvalOutcome out = ev::evaluate(*req);
        cg::test::check(
            r,
            out.timed_out && !out.completed,
            std::format(
                "1ms 预算超时放弃: timed_out={} completed={}",
                out.timed_out,
                out.completed));
        cg::test::check(
            r,
            out.error.find("timed out") != std::string::npos,
            std::format("超时诊断文本在场: {}", out.error));
#else
        (void)r;
#endif
    }

    const cg::test::Registrar reg_eval_discovery{
        "msbuild.eval_discovery_text",
        &eval_discovery_text_cases};
    const cg::test::Registrar reg_eval_demosln{
        "msbuild.eval_demosln",
        &eval_demosln_cases};
    const cg::test::Registrar reg_eval_matrix{
        "msbuild.eval_matrix",
        &eval_matrix_cases};
    const cg::test::Registrar reg_eval_build_flags{
        "msbuild.eval_build_flags",
        &eval_build_flag_cases};
    const cg::test::Registrar reg_eval_watchdog{
        "msbuild.eval_watchdog",
        &eval_watchdog_cases};
    const cg::test::Registrar reg_eval_snapshot{
        "msbuild.eval_snapshot",
        &eval_snapshot_cases};

    // ------------------------------------------------------------------
    // 组装器（cg.msbuild.compose）：合成载荷的字段级单测（跨平台纯函数）
    // + 夹具驱动的双通道对账（求值组装 argv vs 记录通道真值投影，
    // Windows + 本机 VS 时跑，差异面按对照判据显式声明）。
    // ------------------------------------------------------------------

    namespace cp = cg::msbuild::compose;

    /** 合成一个工程求值产物（raw_json 由 json 门面构造）。 */
    [[nodiscard]]
    auto make_project(std::string_view name, json::Object root)
        -> ev::EvalProjectOut
    {
        ev::EvalProjectOut p;
        p.name = std::string{name};
        p.build_in_solution = true;
        p.raw_json = json::stringify(std::move(root));
        return p;
    }

    /** 工程载荷骨架：top_props + cl_item_definition + tus。 */
    [[nodiscard]]
    auto compose_skeleton(
        std::string_view dir,
        std::string_view int_dir,
        json::Object     itemdef,
        json::Array      tus) -> json::Object
    {
        json::Object top;
        top.set("MSBuildProjectDirectory", std::string{dir});
        top.set("IntDir", std::string{int_dir});
        top.set("PlatformToolset", "v145");
        json::Object root;
        root.set("name", std::string{"synthetic"});
        root.set("top_props", std::move(top));
        root.set("cl_item_definition", std::move(itemdef));
        root.set("tus", std::move(tus));
        return root;
    }

    /** PCH 三态之一的 argv 断言（/Yc /Yu 连写头名 + /Fp 连写）。 */
    void compose_pch_case(
        R&                                r,
        const std::vector<mb::ProjectCl>& cls,
        std::string_view                  use,
        std::string_view                  want_switch)
    {
        json::Object itemdef3;
        itemdef3.set("PrecompiledHeader", std::string{use});
        itemdef3.set("PrecompiledHeaderFile", "pch.h");
        itemdef3.set("PrecompiledHeaderOutputFile", "X:/fake/int/pch.pch");
        itemdef3.set("WarningLevel", "Level3");
        json::Object tu3;
        tu3.set("file", "main.cpp");
        tu3.set("abs", "X:/fake/proj/main.cpp");
        json::Array tus3;
        tus3.add(std::move(tu3));
        std::vector<ev::EvalProjectOut> ps;
        ps.push_back(make_project(
            "synthetic",
            compose_skeleton(
                "X:/fake/proj",
                "X:/fake/int/",
                std::move(itemdef3),
                std::move(tus3))));
        const std::vector<mb::ClInvocation> o3 =
            cp::compose_invocations(ps, cls);
        const std::vector<std::string> want3 = {
            "X:/fake/cl.exe",
            "/W3",
            std::string{want_switch},
            "/FpX:/fake/int/pch.pch",
            "/external:W3",
            "/TP",
            "X:/fake/proj/main.cpp",
        };
        cg::test::check(
            r,
            o3.size() == 1 && o3[0].argv_utf8 == want3,
            std::format("PCH '{}' 的 argv 相等", use));
    }

    /** 模块单元判定单分支的 argv 断言（/interface 与 /ifcOutput 拼名）。 */
    void compose_module_case(
        R&                                r,
        const std::vector<mb::ProjectCl>& cls,
        std::string_view                  compile_as,
        std::string_view                  file,
        std::string_view                  want_lang_mode,
        bool                              want_interface,
        bool                              want_module_output,
        std::string_view                  want_fo)
    {
        json::Object itemdef5;
        itemdef5.set("CompileAs", std::string{compile_as});
        itemdef5.set("ObjectFileName", "X:/fake/int/");
        itemdef5.set("ModuleOutputFile", "X:/fake/int/");
        itemdef5.set("AdditionalBMIDirectories", "bmidir");
        itemdef5.set("AdditionalModuleDependencies", "core=core.ifc");
        itemdef5.set("TranslateIncludes", "true");
        itemdef5.set("WarningLevel", "Level3");
        itemdef5.set("LanguageStandard", "stdcpp20");
        itemdef5.set("CallingConvention", "Cdecl");
        json::Object tu5;
        tu5.set("file", std::string{file});
        tu5.set("abs", std::string{"X:/fake/proj/"} + std::string{file});
        json::Array tus5;
        tus5.add(std::move(tu5));
        std::vector<ev::EvalProjectOut> ps;
        ps.push_back(make_project(
            "synthetic",
            compose_skeleton(
                "X:/fake/proj",
                "X:/fake/int/",
                std::move(itemdef5),
                std::move(tus5))));
        const std::vector<mb::ClInvocation> o5 =
            cp::compose_invocations(ps, cls);
        std::vector<std::string> want5 = {
            "X:/fake/cl.exe",
            "/ifcSearchDir",
            "bmidir",
            "/reference",
            "core=core.ifc",
            "/translateInclude",
            "/W3",
            "/std:c++20",
        };
        if (want_module_output)
        {
            want5.emplace_back("/ifcOutput");
            want5.emplace_back(
                std::string{"X:/fake/int/"} + std::string{file} + ".ifc");
        }
        want5.push_back(std::string{want_fo});
        want5.emplace_back("/external:W3");
        want5.emplace_back("/Gd");
        if (want_interface)
        {
            want5.push_back("/interface");
        }
        want5.push_back(std::string{want_lang_mode});
        want5.push_back(std::string{"X:/fake/proj/"} + std::string{file});
        cg::test::check(
            r,
            o5.size() == 1 && o5[0].argv_utf8 == want5,
            std::format(
                "模块单元判定分支 CompileAs='{}' file='{}' 的 "
                "argv 相等（实得 {} 项）",
                compile_as,
                file,
                o5.size() == 1 ? o5[0].argv_utf8.size() : 0));
    }

    /** coverfields 全量正例的字段装载（40 键逐值设置）。 */
    [[nodiscard]]
    auto make_coverfields_itemdef() -> json::Object
    {
        json::Object itemdef;
        itemdef.set("PreprocessorDefinitions", "COV_A=1;COV_B");
        itemdef.set("UndefinePreprocessorDefinitions", "OLD_THING");
        itemdef.set("AdditionalIncludeDirectories", "sub;inc");
        itemdef.set("ExternalIncludePath", "extinc");
        itemdef.set("ForcedIncludeFiles", "force.h");
        itemdef.set("AdditionalOptions", "/Zc:__cplusplus /extra");
        itemdef.set("LanguageStandard", "stdcpp17");
        itemdef.set("LanguageStandard_C", "Default");
        itemdef.set("PrecompiledHeader", "NotUsing");
        itemdef.set("PrecompiledHeaderFile", "stdafx.h");
        itemdef.set("PrecompiledHeaderOutputFile", "X:/fake/int/x.pch");
        itemdef.set("CompileAs", "Default");
        itemdef.set("ConformanceMode", "true");
        itemdef.set("RuntimeLibrary", "MultiThreaded");
        itemdef.set("WarningLevel", "Level4");
        itemdef.set("ExternalWarningLevel", "Level1");
        itemdef.set("DisableSpecificWarnings", "4267;4996");
        itemdef.set("TreatWarningAsError", "true");
        itemdef.set("MultiProcessorCompilation", "true");
        itemdef.set("DebugInformationFormat", "OldStyle");
        itemdef.set("ExceptionHandling", "Async");
        itemdef.set("FunctionLevelLinking", "true");
        itemdef.set("IntrinsicFunctions", "true");
        itemdef.set("StringPooling", "true");
        itemdef.set("MinimalRebuild", "true");
        itemdef.set("BrowseInformation", "true");
        itemdef.set("TreatWChar_tAsBuiltInType", "false");
        itemdef.set("RuntimeTypeInfo", "true");
        itemdef.set("DisableLanguageExtensions", "false");
        itemdef.set("CallingConvention", "VectorCall");
        itemdef.set("ScanSourceForModuleDependencies", "true");
        itemdef.set("TranslateIncludes", "false");
        itemdef.set("AdditionalModuleDependencies", "");
        itemdef.set("AdditionalBMIDirectories", "");
        itemdef.set("ModuleOutputFile", "");
        itemdef.set("ObjectFileName", "X:/fake/int/");
        itemdef.set("ProgramDataBaseFileName", "X:/fake/int/vc.pdb");
        return itemdef;
    }

    /** 字段全量合成载荷的 argv 精确断言（拼写/序/连分写/否定形——
     *  期望值 = 记录通道对 coverfields 夹具的 wrap 捕获真值投影）。 */
    void compose_field_cases(R& r)
    {
        json::Object itemdef = make_coverfields_itemdef();

        json::Object tu;
        tu.set("file", "cov.cpp");
        tu.set("abs", "X:/fake/proj/cov.cpp");
        json::Array tus;
        tus.add(std::move(tu));

        std::vector<ev::EvalProjectOut> projects;
        projects.push_back(make_project(
            "synthetic",
            compose_skeleton(
                "X:/fake/proj",
                "X:/fake/int/",
                std::move(itemdef),
                std::move(tus))));
        std::vector<mb::ProjectCl> cls;
        cls.push_back(
            mb::ProjectCl{
                .project = "synthetic",
                .dir = "X:/fake",
                .cl = "X:/fake/cl.exe",
                .resolution = "override"});

        cp::ComposeDiary   diary;
        cp::ComposeOptions opts;
        opts.include_env = std::string{"X:/fake/inc1;X:/fake/inc2"};
        const std::vector<mb::ClInvocation> out =
            cp::compose_invocations(projects, cls, opts, &diary);
        cg::test::check(
            r,
            out.size() == 1,
            std::format("全量载荷组装 1 条（实得 {}）", out.size()));
        if (out.size() != 1)
        {
            return;
        }

        const std::vector<std::string> want = {
            "X:/fake/cl.exe",
            "/Isub",
            "/Iinc",
            "/Z7",
            "/W4",
            "/WX",
            "/MP",
            "/Oi",
            "/D",
            "COV_A=1",
            "/D",
            "COV_B",
            "/UOLD_THING",
            "/GF",
            "/Gm",
            "/EHa",
            "/MT",
            "/Gy",
            "/Zc:wchar_t-",
            "/GR",
            "/std:c++17",
            "/permissive-",
            "/FoX:/fake/int/",
            "/FdX:/fake/int/vc.pdb",
            "/FRX:/fake/int/",
            "/external:W1",
            "/Gv",
            "/TP",
            "/wd4267",
            "/wd4996",
            "/FIforce.h",
            "/Zc:__cplusplus",
            "/extra",
            "X:/fake/proj/cov.cpp",
        };
        cg::test::check(
            r,
            out[0].argv_utf8 == want,
            std::format(
                "全量字段 argv 逐 token 相等（实得 {} 项，期望 {} 项）:\n"
                "  got:  {}\n  want: {}",
                out[0].argv_utf8.size(),
                want.size(),
                [&]
                {
                    std::string s;
                    for (const auto& a : out[0].argv_utf8)
                        s += a + " ";
                    return s;
                }(),
                [&]
                {
                    std::string s;
                    for (const auto& a : want)
                        s += a + " ";
                    return s;
                }()));
        cg::test::check(
            r,
            out[0].cwd == "X:/fake/proj" && out[0].real_cl == "X:/fake/cl.exe"
                && out[0].include_env == "X:/fake/inc1;X:/fake/inc2",
            "cwd / real_cl / include_env 字段承接");
        bool scan_note = false;
        bool extinc_note = false;
        for (const std::string& n : diary.notes)
        {
            if (n.find("ScanSourceForModuleDependencies") != std::string::npos)
            {
                scan_note = true;
            }
            if (n.find("ExternalIncludePath") != std::string::npos)
            {
                extinc_note = true;
            }
        }
        cg::test::check(
            r,
            scan_note && extinc_note,
            std::format(
                "已知不进命令行的两字段披露（scan={} external_include={}）",
                scan_note,
                extinc_note));

        // 否定形 + 空值默认档（记录通道 false 探针与 coverc 真值投影）。
        {
            json::Object itemdef2;
            itemdef2.set("FunctionLevelLinking", "false");
            itemdef2.set("StringPooling", "false");
            itemdef2.set("RuntimeTypeInfo", "false");
            itemdef2.set("TreatWarningAsError", "false");
            itemdef2.set("MinimalRebuild", "false");
            itemdef2.set("TreatWChar_tAsBuiltInType", "false");
            itemdef2.set("IntrinsicFunctions", "false");
            itemdef2.set("BrowseInformation", "false");
            itemdef2.set("DisableLanguageExtensions", "true");
            itemdef2.set("WarningLevel", "");
            itemdef2.set("ExternalWarningLevel", "");
            itemdef2.set("ExceptionHandling", "Sync");
            itemdef2.set("RuntimeLibrary", "MultiThreadedDll");
            itemdef2.set("LanguageStandard", "Default");
            itemdef2.set("CallingConvention", "Cdecl");
            itemdef2.set("PrecompiledHeader", "NotUsing");
            itemdef2.set("CompileAs", "Default");
            itemdef2.set("ConformanceMode", "Default");
            itemdef2.set("DebugInformationFormat", "ProgramDatabase");
            itemdef2.set("DisableSpecificWarnings", "");
            itemdef2.set("ForcedIncludeFiles", "");
            itemdef2.set("AdditionalOptions", "");
            itemdef2.set("PreprocessorDefinitions", "");
            itemdef2.set("UndefinePreprocessorDefinitions", "");
            itemdef2.set("AdditionalIncludeDirectories", "");
            itemdef2.set("MultiProcessorCompilation", "");
            json::Object tu2;
            tu2.set("file", "cov.cpp");
            tu2.set("abs", "X:/fake/proj/cov.cpp");
            json::Array tus2;
            tus2.add(std::move(tu2));
            std::vector<ev::EvalProjectOut> ps;
            ps.push_back(make_project(
                "synthetic",
                compose_skeleton(
                    "X:/fake/proj",
                    "X:/fake/int/",
                    std::move(itemdef2),
                    std::move(tus2))));
            const std::vector<mb::ClInvocation> o2 =
                cp::compose_invocations(ps, cls);
            const std::vector<std::string> want2 = {
                "X:/fake/cl.exe",
                "/Zi",
                "/W1",
                "/WX-",
                "/GF-",
                "/Gm-",
                "/EHsc",
                "/MD",
                "/Gy-",
                "/Za",
                "/Zc:wchar_t-",
                "/GR-",
                "/external:W1",
                "/Gd",
                "/TP",
                "X:/fake/proj/cov.cpp",
            };
            cg::test::check(
                r,
                o2.size() == 1 && o2[0].argv_utf8 == want2,
                std::format(
                    "否定形与空值默认档 argv 相等（实得 {} 项）",
                    o2.size() == 1 ? o2[0].argv_utf8.size() : 0));
        }

        // PCH 三态（pchp 真值投影：/Yc /Yu 连写头名 + /Fp 连写）。
        {
            compose_pch_case(r, cls, "Create", "/Ycpch.h");
            compose_pch_case(r, cls, "Use", "/Yupch.h");
            // NotUsing：/Yc /Yu /Fp 都不出现。
            {
                json::Object itemdef4;
                itemdef4.set("PrecompiledHeader", "NotUsing");
                itemdef4.set("PrecompiledHeaderFile", "pch.h");
                itemdef4.set(
                    "PrecompiledHeaderOutputFile",
                    "X:/fake/int/pch.pch");
                json::Object tu4;
                tu4.set("file", "main.cpp");
                tu4.set("abs", "X:/fake/proj/main.cpp");
                json::Array tus4;
                tus4.add(std::move(tu4));
                std::vector<ev::EvalProjectOut> ps;
                ps.push_back(make_project(
                    "synthetic",
                    compose_skeleton(
                        "X:/fake/proj",
                        "X:/fake/int/",
                        std::move(itemdef4),
                        std::move(tus4))));
                const std::vector<mb::ClInvocation> o4 =
                    cp::compose_invocations(ps, cls);
                cg::test::check(
                    r,
                    o4.size() == 1
                        && std::ranges::none_of(
                            o4[0].argv_utf8,
                            [](const std::string& t)
                            {
                                return t.starts_with("/Y")
                                       || t.starts_with("/Fp");
                            }),
                    "NotUsing 不发 PCH 族开关");
            }
        }

        // 模块单元判定三分支与 /interface 产物拼名（modmsb / covermod
        // 真值投影）。
        {
            // 显式模块：/interface + /TP 垫尾 + 产物拼全名（工程感知，
            // /ifcOutput 带 .ifc 文件名）。
            compose_module_case(
                r,
                cls,
                "CompileAsCppModule",
                "mod.ixx",
                "/TP",
                true,
                true,
                "/FoX:/fake/int/mod.ixx.obj");
            // Default + .ixx 扩展名（扩展名分支）同形态。
            compose_module_case(
                r,
                cls,
                "Default",
                "mod.ixx",
                "/TP",
                true,
                true,
                "/FoX:/fake/int/mod.ixx.obj");
            // Default + .cpp：无模块 TU 的工程不感知——/ifcOutput 不发、
            // 产物目录原样、/TP 正常位。
            compose_module_case(
                r,
                cls,
                "Default",
                "use.cpp",
                "/TP",
                false,
                false,
                "/FoX:/fake/int/");
            // Default + .c：/TC。
            compose_module_case(
                r,
                cls,
                "Default",
                "cov.c",
                "/TC",
                false,
                false,
                "/FoX:/fake/int/");
            // 显式 CompileAsCpp：/TP 正常位（工程仍不感知）。
            compose_module_case(
                r,
                cls,
                "CompileAsCpp",
                "use.cpp",
                "/TP",
                false,
                false,
                "/FoX:/fake/int/");
        }
    }

    /** TU 级覆盖、过滤四态与披露、未映射值不代言、字段登记表快照。 */
    void compose_filter_and_override_cases(R& r)
    {
        std::vector<mb::ProjectCl> cls;
        cls.push_back(
            mb::ProjectCl{
                .project = "synthetic",
                .dir = "X:/fake",
                .cl = "X:/fake/cl.exe",
                .resolution = "override"});

        // TU 级 diff 覆盖 itemdef 基线（求值 diff 存储的回放）。
        {
            json::Object itemdef6;
            itemdef6.set("PreprocessorDefinitions", "BASE=1");
            itemdef6.set("WarningLevel", "Level3");
            json::Object tu6;
            tu6.set("file", "over.cpp");
            tu6.set("abs", "X:/fake/proj/over.cpp");
            tu6.set("defines", "OVERRIDE=2");
            tu6.set("warning_level", "Level1");
            json::Array tus6;
            tus6.add(std::move(tu6));
            std::vector<ev::EvalProjectOut> ps;
            ps.push_back(make_project(
                "synthetic",
                compose_skeleton(
                    "X:/fake/proj",
                    "X:/fake/int/",
                    std::move(itemdef6),
                    std::move(tus6))));
            const std::vector<mb::ClInvocation> o6 = cp::compose_invocations(
                ps,
                cls,
                cp::ComposeOptions{std::string{"INC"}});
            const std::vector<std::string> want6 = {
                "X:/fake/cl.exe",
                "/W1",
                "/D",
                "OVERRIDE=2",
                // external 档继承的是回放终值（diff 覆盖后的 Level1），
                // 不是 itemdef 基线——继承语义按 TU 终值走。
                "/external:W1",
                "/TP",
                "X:/fake/proj/over.cpp",
            };
            cg::test::check(
                r,
                o6.size() == 1 && o6[0].argv_utf8 == want6,
                "TU diff 覆盖 itemdef 基线（defines 与 warning_level）");
        }

        // 过滤四态与披露（error 工程 / 参与位 / TU 排除 / 无 cl）。
        {
            json::Object empty_def;
            json::Object tu7;
            tu7.set("file", "a.cpp");
            tu7.set("abs", "X:/fake/proj/a.cpp");
            json::Object tu8;
            tu8.set("file", "b.cpp");
            tu8.set("abs", "X:/fake/proj/b.cpp");
            tu8.set("excluded_from_build", "true");
            json::Array tus7;
            tus7.add(std::move(tu7));
            tus7.add(std::move(tu8));
            std::vector<ev::EvalProjectOut> ps;
            ps.push_back(make_project(
                "okproj",
                compose_skeleton(
                    "X:/fake/proj",
                    "X:/fake/int/",
                    std::move(empty_def),
                    std::move(tus7))));
            ev::EvalProjectOut failed_proj;
            failed_proj.name = "failedproj";
            failed_proj.error = "InvalidProjectFileException: boom";
            failed_proj.build_in_solution = true;
            ps.push_back(std::move(failed_proj));
            ev::EvalProjectOut unbound;
            unbound.name = "okproj";
            unbound.build_in_solution = false;
            unbound.raw_json = ps[0].raw_json;
            ps.push_back(std::move(unbound));

            std::vector<mb::ProjectCl>          no_cls; // 空清单：无 cl 匹配
            cp::ComposeDiary                    diary2;
            const std::vector<mb::ClInvocation> o7 =
                cp::compose_invocations(ps, no_cls, {}, &diary2);
            cg::test::check(
                r,
                o7.empty(),
                std::format(
                    "error/无 cl/参与位假全跳过（实得 {} 条）",
                    o7.size()));
            bool skip_error = false;
            bool skip_cl = false;
            for (const std::string& n : diary2.notes)
            {
                if (n.find("eval error") != std::string::npos)
                {
                    skip_error = true;
                }
                if (n.find("no cl resolved") != std::string::npos)
                {
                    skip_cl = true;
                }
            }
            cg::test::check(
                r,
                skip_error && skip_cl,
                "error 工程与无 cl 工程披露行在场");

            // 正常 cl 下 ExcludedFromBuild 的 TU 级过滤。
            cp::ComposeDiary                diary3;
            std::vector<ev::EvalProjectOut> ps2;
            json::Object                    ed8;
            json::Object                    t9;
            t9.set("file", "a.cpp");
            t9.set("abs", "X:/fake/proj/a.cpp");
            json::Object t10;
            t10.set("file", "b.cpp");
            t10.set("abs", "X:/fake/proj/b.cpp");
            t10.set("excluded_from_build", "true");
            json::Array ta;
            ta.add(std::move(t9));
            ta.add(std::move(t10));
            ps2.push_back(make_project(
                "synthetic",
                compose_skeleton(
                    "X:/fake/proj",
                    "X:/fake/int/",
                    std::move(ed8),
                    std::move(ta))));
            const std::vector<mb::ClInvocation> o8 =
                cp::compose_invocations(ps2, cls, {}, &diary3);
            bool excluded_note = false;
            for (const std::string& n : diary3.notes)
            {
                if (n.find("ExcludedFromBuild") != std::string::npos)
                {
                    excluded_note = true;
                }
            }
            cg::test::check(
                r,
                o8.size() == 1 && excluded_note
                    && (!o8.empty()
                        && o8[0].argv_utf8.back() == "X:/fake/proj/a.cpp"),
                "TU 级 ExcludedFromBuild 过滤且披露");
        }

        // 未映射值不代言（未知枚举/未知布尔拼写只披露）。
        {
            json::Object itemdef9;
            itemdef9.set("WarningLevel", "LevelNine");
            itemdef9.set("CompileAs", "CompileAsRust");
            itemdef9.set("RuntimeLibrary", "DebugFast");
            itemdef9.set("MinimalRebuild", "maybe");
            json::Object t11;
            t11.set("file", "x.cpp");
            t11.set("abs", "X:/fake/proj/x.cpp");
            json::Array t11s;
            t11s.add(std::move(t11));
            std::vector<ev::EvalProjectOut> ps;
            ps.push_back(make_project(
                "synthetic",
                compose_skeleton(
                    "X:/fake/proj",
                    "X:/fake/int/",
                    std::move(itemdef9),
                    std::move(t11s))));
            cp::ComposeDiary                    diary4;
            const std::vector<mb::ClInvocation> o9 =
                cp::compose_invocations(ps, cls, {}, &diary4);
            std::size_t unmapped = 0;
            for (const std::string& n : diary4.notes)
            {
                if (n.find("unmapped, skipped") != std::string::npos)
                {
                    ++unmapped;
                }
            }
            cg::test::check(
                r,
                o9.size() == 1 && unmapped == 4,
                std::format(
                    "四个未知值各披露一行（实得 {} 行）且条目仍在",
                    unmapped));
            const std::vector<std::string> want9 = {
                "X:/fake/cl.exe",
                "/external:W1",
                "X:/fake/proj/x.cpp",
            };
            cg::test::check(
                r,
                o9.size() == 1 && o9[0].argv_utf8 == want9,
                std::format(
                    "未知值的开关全部不在场（实得 {} 项）",
                    o9.size() == 1 ? o9[0].argv_utf8.size() : 0));
        }

        // 字段面登记表快照（37 项、与垫片侧平行数组同步）。
        cg::test::check(
            r,
            std::size(cp::tu_fields) == 37
                && cp::tu_fields[0].msb_key == "PreprocessorDefinitions"
                && cp::tu_fields[36].msb_key == "ProgramDataBaseFileName"
                && cp::tu_fields[34].tu_key == "module_output",
            "tu_fields 登记表 37 项且首尾键无漂移");
    }

    /**
     * 对照投影：把 record 臂（wrap 捕获真值）与 evaluate 臂（组装器
     * 产物）的 argv 投到同一可比面。差异面显式声明（与设计稿六维
     * 对齐判据同源）：
     * - CL task 属性默认值族（/c /JMC /nologo /diagnostics:column
     *   /sdl /sdl- /Od /O2 /RTC1 /GS /fp:precise /Zc:forScope
     *   /Zc:inline /FC /errorReport:queue /FS）——props 链与 ToolSwitch
     *   默认注入，求值元数据面不可见，组装器不补造；
     * - 扫描趟开关族（/scanDependencies /sourceDependencies 与值）——
     *   纯求值无扫描趟，record 侧滤除；
     * - 产物路径值（/Fo /Fd /Fp /FR /ifcOutput 的值）——record 侧是
     *   沙箱 IntDir 映射、evaluate 侧是求值终值，路径不可比、形态可比
     *   （目录尾 vs 文件名）；
     * - /reference 的值——targets 执行期做路径绝对化（Fixup），求值面
     *   拿原始相对形态，投影取 '=' 前的模块名；
     * - 位置源文件与编译器 token——调用方先剥掉（TU 匹配按 basename）。
     */
    [[nodiscard]]
    auto project_argv(const std::vector<std::string>& argv)
        -> std::vector<std::string>
    {
        static const std::set<std::string> default_family = {
            "/c",
            "/JMC",
            "/nologo",
            "/diagnostics:column",
            "/sdl",
            "/sdl-",
            "/Od",
            "/O2",
            "/RTC1",
            "/GS",
            "/fp:precise",
            "/Zc:forScope",
            "/Zc:inline",
            "/FC",
            "/errorReport:queue",
            "/FS",
        };
        std::vector<std::string> out;
        for (std::size_t i = 0; i < argv.size(); ++i)
        {
            const std::string& t = argv[i];
            if (default_family.contains(t))
            {
                continue;
            }
            if (t == "/scanDependencies" || t == "/sourceDependencies")
            {
                ++i; // 开关与值一起滤除
                continue;
            }
            if (t == "/ifcOutput")
            {
                const std::string& v = argv[++i];
                out.emplace_back("/ifcOutput");
                out.push_back(
                    v.size() >= 4 && v.ends_with(".ifc") ? "<ifcOutput:file>"
                                                         : "<ifcOutput:dir>");
                continue;
            }
            if (t == "/reference")
            {
                const std::string& v = argv[++i];
                const std::size_t  eq = v.find('=');
                out.emplace_back("/reference");
                out.push_back("<reference:" + v.substr(0, eq) + ">");
                continue;
            }
            if (t.starts_with("/Fo"))
            {
                out.push_back(
                    t.size() > 3 && t.back() != '/' && t.back() != '\\'
                        ? "<Fo:file>"
                        : "<Fo:dir>");
                continue;
            }
            if (t.starts_with("/Fd"))
            {
                out.push_back("<Fd>");
                continue;
            }
            if (t.starts_with("/Fp"))
            {
                out.push_back("<Fp>");
                continue;
            }
            if (t.starts_with("/FR"))
            {
                out.push_back("<FR>");
                continue;
            }
            out.push_back(t);
        }
        return out;
    }

    /** record 真值投影的 golden 表：TU basename → 期望投影 token 列。 */
    [[nodiscard]]
    auto crosscheck_golden(std::string_view fixture, std::string_view tu)
        -> std::vector<std::string>
    {
        const std::map<std::string, std::vector<std::string>> table = {
            {"DemoSln|multiply.cpp",
             {"/I.",
              "/Zi",
              "/W3",
              "/WX-",
              "/D",
              "MATHLIB_SCALE=3",
              "/Gm-",
              "/EHsc",
              "/MDd",
              "/Zc:wchar_t",
              "/std:c++17",
              "<Fo:dir>",
              "<Fd>",
              "/external:W3",
              "/Gd",
              "/TP"}},
            {"DemoSln|scale.cpp",
             {"/I.",
              "/Zi",
              "/W3",
              "/WX-",
              "/D",
              "MATHLIB_SCALE=3",
              "/Gm-",
              "/EHsc",
              "/MDd",
              "/Zc:wchar_t",
              "/std:c++17",
              "<Fo:dir>",
              "<Fd>",
              "/external:W3",
              "/Gd",
              "/TP"}},
            {"DemoSln|main.cpp",
             {"/Iinclude",    "/Isrc",        "/Zi",      "/W3",
              "/WX-",         "/MP",          "/D",       "DEMO_DEFINE=42",
              "/D",           "_UNICODE",     "/D",       "UNICODE",
              "/Gm-",         "/EHsc",        "/MDd",     "/Zc:wchar_t",
              "/std:c++20",   "/permissive-", "<Fo:dir>", "<Fd>",
              "/external:W3", "/Gd",          "/TP"}},
            {"DemoSln|probe.cpp",
             {"/Iinclude",    "/Isrc",        "/Zi",      "/W3",
              "/WX-",         "/MP",          "/D",       "DEMO_DEFINE=42",
              "/D",           "_UNICODE",     "/D",       "UNICODE",
              "/Gm-",         "/EHsc",        "/MDd",     "/Zc:wchar_t",
              "/std:c++20",   "/permissive-", "<Fo:dir>", "<Fd>",
              "/external:W3", "/Gd",          "/TP"}},
            {"DemoSln|util.cpp",
             {"/Iinclude",    "/Isrc",        "/Zi",      "/W3",
              "/WX-",         "/MP",          "/D",       "DEMO_DEFINE=42",
              "/D",           "_UNICODE",     "/D",       "UNICODE",
              "/Gm-",         "/EHsc",        "/MDd",     "/Zc:wchar_t",
              "/std:c++20",   "/permissive-", "<Fo:dir>", "<Fd>",
              "/external:W3", "/Gd",          "/TP"}},
            {"modmsb|mod.ixx",
             {"/Zi",
              "/W3",
              "/WX-",
              "/Gm-",
              "/EHsc",
              "/MD",
              "/Zc:wchar_t",
              "/std:c++20",
              "/ifcOutput",
              "<ifcOutput:file>",
              "<Fo:file>",
              "<Fd>",
              "/external:W3",
              "/Gd",
              "/interface",
              "/TP"}},
            {"modmsb|use.cpp",
             {"/Zi",
              "/W3",
              "/WX-",
              "/Gm-",
              "/EHsc",
              "/MD",
              "/Zc:wchar_t",
              "/std:c++20",
              "/ifcOutput",
              "<ifcOutput:dir>",
              "<Fo:dir>",
              "<Fd>",
              "/external:W3",
              "/Gd",
              "/TP"}},
            {"pchp|pch.cpp",
             {"/ZI",          "/W3",          "/WX-",        "/D",
              "_UNICODE",     "/D",           "UNICODE",     "/Gm-",
              "/EHsc",        "/MDd",         "/Zc:wchar_t", "/std:c++20",
              "/permissive-", "/Ycpch.h",     "<Fp>",        "<Fo:dir>",
              "<Fd>",         "/external:W3", "/Gd",         "/TP"}},
            {"pchp|main.cpp",
             {"/ZI",          "/W3",          "/WX-",        "/D",
              "_UNICODE",     "/D",           "UNICODE",     "/Gm-",
              "/EHsc",        "/MDd",         "/Zc:wchar_t", "/std:c++20",
              "/permissive-", "/Yupch.h",     "<Fp>",        "<Fo:dir>",
              "<Fd>",         "/external:W3", "/Gd",         "/TP"}},
            {"cover|cov.cpp",
             {"/Isub",        "/Iinc",        "/Z7",   "/W4",
              "/WX",          "/MP",          "/Oi",   "/D",
              "COV_A=1",      "/D",           "COV_B", "/UOLD_THING",
              "/GF",          "/Gm",          "/EHa",  "/MT",
              "/Gy",          "/Zc:wchar_t-", "/GR",   "/std:c++17",
              "/permissive-", "<Fo:dir>",     "<Fd>",  "<FR>",
              "/external:W1", "/Gv",          "/TP",   "/wd4267",
              "/wd4996",      "/FIforce.h"}},
            {"cover|cov.c",
             {"/Zi",
              "/W1",
              "/WX-",
              "/Gm-",
              "/EHsc",
              "/MD",
              "/Za",
              "/Zc:wchar_t",
              "/std:c17",
              "<Fo:dir>",
              "<Fd>",
              "/external:W1",
              "/Gd",
              "/TC"}},
            {"cover|mod.ixx",
             {"/ifcSearchDir",
              "bmidir",
              "/reference",
              "<reference:core>",
              "/translateInclude",
              "/Zi",
              "/W1",
              "/WX-",
              "/Gm-",
              "/EHsc",
              "/MD",
              "/Zc:wchar_t",
              "/std:c++20",
              "/ifcOutput",
              "<ifcOutput:file>",
              "<Fo:file>",
              "<Fd>",
              "/external:W1",
              "/Gd",
              "/interface",
              "/TP"}},
            {"cover|use.cpp",
             {"/ifcSearchDir",
              "bmidir",
              "/reference",
              "<reference:core>",
              "/translateInclude",
              "/Zi",
              "/W1",
              "/WX-",
              "/Gm-",
              "/EHsc",
              "/MD",
              "/Zc:wchar_t",
              "/std:c++20",
              "/ifcOutput",
              "<ifcOutput:dir>",
              "<Fo:dir>",
              "<Fd>",
              "/external:W1",
              "/Gd",
              "/TP"}},
        };
        const auto it =
            table.find(std::string{fixture} + "|" + std::string{tu});
        return it != table.end() ? it->second : std::vector<std::string>{};
    }

    /**
     * 双通道对账自证：同一夹具上求值 + 组装的 argv 投影 vs 记录通道
     * 真值投影（golden 来自 record 的 wrap 捕获，机器为 VS 18 / v145
     * 工具集实测）。无 VS 的环境打说明行跳过。
     */
    void compose_crosscheck_cases(R& r)
    {
#if defined(_WIN32)
        const std::string msbuild = eval_env_ready();
        if (msbuild.empty())
        {
            std::println("compose_crosscheck: skip, no MSBuild on machine");
            return;
        }
        struct Row
        {
            std::string_view fixture;
            std::string_view sln;
        };
        const Row rows[] = {
            {"DemoSln", "DemoSln.sln"},
            {"modmsb", "modmsb/modmsb.sln"},
            {"pchp", "pchp/pchp.sln"},
            {"cover", "compose/cover.sln"},
        };
        for (const Row& row : rows)
        {
            const auto req = ev::build_evaluate_request(
                msbuild,
                fixture(row.sln),
                "Debug",
                "x64");
            cg::test::check(
                r,
                req.has_value(),
                std::format("{} 对照请求构建成功", row.fixture));
            if (!req.has_value())
            {
                continue;
            }
            const ev::EvalOutcome out = ev::evaluate(*req);
            cg::test::check(
                r,
                out.completed && out.failed_projects == 0
                    && out.mismatches == 0,
                std::format(
                    "{} 求值会话全绿: error={} ok={} failed={}",
                    row.fixture,
                    out.error,
                    out.ok_projects,
                    out.failed_projects));
            if (!out.completed)
            {
                continue;
            }
            const std::vector<mb::ProjectCl> cls = mb::resolve_project_cls(
                msbuild,
                fixture(row.sln),
                "Debug",
                "x64",
                "");
            const std::vector<mb::ClInvocation> composed =
                cp::compose_invocations(out.projects, cls);

            // TU 集对账（组装条数 == golden 键数；逐条匹配）。
            std::map<std::string, const mb::ClInvocation*> by_base;
            for (const mb::ClInvocation& inv : composed)
            {
                const std::string src = inv.argv_utf8.back();
                const std::size_t slash = src.find_last_of("/\\");
                by_base[cg::util::ascii_lower(
                    slash == std::string::npos ? src : src.substr(slash + 1))] =
                    &inv;
            }
            std::size_t                           matched = 0;
            static const std::vector<std::string> probe_files = {
                "multiply.cpp",
                "scale.cpp",
                "main.cpp",
                "probe.cpp",
                "util.cpp",
                "mod.ixx",
                "use.cpp",
                "pch.cpp",
                "cov.cpp",
                "cov.c",
            };
            for (const std::string& base : probe_files)
            {
                const std::vector<std::string> want =
                    crosscheck_golden(row.fixture, base);
                if (want.empty())
                {
                    continue;
                }
                const auto it = by_base.find(base);
                if (it == by_base.end())
                {
                    cg::test::check(
                        r,
                        false,
                        std::format(
                            "{} 的 TU '{}' 缺组装条目",
                            row.fixture,
                            base));
                    continue;
                }
                const mb::ClInvocation&            inv = *it->second;
                const std::span<const std::string> middle{
                    inv.argv_utf8.begin() + 1,
                    inv.argv_utf8.end() - 1};
                const std::vector<std::string> got =
                    project_argv({middle.begin(), middle.end()});
                cg::test::check(
                    r,
                    got == want,
                    std::format(
                        "{} / {} 对照投影逐 token 相等:\n"
                        "  got:  {}\n  want: {}",
                        row.fixture,
                        base,
                        [&]
                        {
                            std::string s;
                            for (const auto& a : got)
                                s += a + " ";
                            return s;
                        }(),
                        [&]
                        {
                            std::string s;
                            for (const auto& a : want)
                                s += a + " ";
                            return s;
                        }()));
                ++matched;
            }
            cg::test::check(
                r,
                matched > 0,
                std::format("{} 至少匹配一个 TU 的 golden", row.fixture));
        }
#else
        (void)r;
#endif
    }

    const cg::test::Registrar reg_compose_fields{
        "msbuild.compose_fields",
        &compose_field_cases};
    const cg::test::Registrar reg_compose_filter{
        "msbuild.compose_filter_and_override",
        &compose_filter_and_override_cases};
    const cg::test::Registrar reg_compose_crosscheck{
        "msbuild.compose_crosscheck",
        &compose_crosscheck_cases};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }
