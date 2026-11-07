/**
 * @module cg.clang.compile_db
 *
 * @brief 面向 TU 重放的 compile_commands.json 加载：经 cg.util.json
 * 门面解析 JSON、按 MSVC CRT 规则切分 Windows "command" 字符串、产出
 * 逐文件的就绪 argv 向量供 Index::parse 使用。
 *
 * 索引主策略是记录命令行的逐字重放。
 *
 * @internal
 * 刻意不用 C API 的 CXCompilationDatabase——根过滤与条目整形保持在
 * 项目代码中。
 */
export module cg.clang.compile_db;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.driver;

export namespace cg::clang
{

    /**
     * @brief 条目重放携带的一条命名模块 BMI 投喂。
     *
     * 在 `modmap` 响应文件（或裸命令行）展开时以结构化形式捕获：模块名
     * 加绝对 pcm 路径。
     */
    struct ModuleFeed
    {
        std::string name; // 被导入的模块名（"std"、"cg.util.json"）
        std::string pcm;  // 绝对、规范化 BMI 路径
    };

    /**
     * @brief 解析期模块覆盖旋钮。
     *
     * @internal
     * MSVC 地混合 TU（import std + 文本 STL include）绝不可投喂 std.pcm：
     * pcm 导出的 std:: 模板与文本包含的 STL 定义同时物化并冲突
     * （'is_same_v' 重定义之类的 22 条诊断风暴、rc=0）——此类 TU 按规则
     * 以工具链伞头文件替换 `import std;` 行解析，并丢弃传递闭包含 std 的
     * 一切投喂。头单元降级为文本 include（头单元的
     * name=key 投喂形式总是失败"not known to be a header unit"，唯一可行
     * 的 path-only 急切形式仍属实验性——v1 策略降级）。
     */
    struct ModuleOverlay
    {
        /**
         * @brief 把 `import std;` / `import std.compat;` 行替换为
         * `#include` `<__msvc_all_public_headers.hpp>`（单行——保留书写行号；
         * 工具链 include 目录经 -imsvc 已在解析 include 路径上）。
         */
        bool std_umbrella = false;
        /** @brief 把 `import "hdr";` / `import <hdr>;` 改写为对应的
         *  `#include`。 */
        bool header_unit_includes = false;
        /**
         * @brief 把头单元 import 无法活在 purview 内（未书写全局模块
         *  片段）的接口单元降级为经典 TU：模块声明同宽消隐、解析去掉
         *  -x c++-module。行列保持精确。
         */
        bool demodule = false;
        /**
         * @brief 伞头降级下被消隐 import 行的命名模块。
         *
         * 其投喂已被抹除（投喂会传递加载 std.pcm、撞上文本 STL 头），
         * 留下无映射的书写 import 会让本该存活的解析硬失败。import 关系
         * 经 overlay_imports 的行 0 行存活。
         */
        std::vector<std::string> dropped_modules;
    };

    /**
     * @brief 一条条目的 PCH 消费/回退判定（由 cg.clang.pch 的规划器填写）。
     *
     * @internal 无 PCH 标记的条目保持 `none` 且 identity 为空——parse_digest
     * 不追加任何后缀，保证不用 PCH 的工程与 PCH 支持落地前逐字节一致。
     */
    struct PchUse
    {
        enum class Status : std::uint8_t
        {
            none,
            shadow,          // 消费本引擎构建的 DPR 影子 PCH
            text_fallback,   // PCH 旗标已剥除，走纯文本解析
            replay_degraded, // 显式 opt-in：消费记录 PCH、关闭 DPR（图不完整）
            unusable,        // 文本回退也不可行（仅诊断分类）
        };
        Status status = Status::none;
        /** @brief 折入 parse_digest 的身份；none 时为空。 */
        std::string identity;
        /** @brief 人类可读的原因/失败类（摘要与逐 TU note）。 */
        std::string note;
        /**
         * @brief 实际注入的缓存影子路径（仅 status==shadow）。
         *
         * @internal 下游 pch-unusable 失效用它定位 slot；文本回退/重放
         * 路径为空。
         */
        std::string shadow_path;
    };

    /**
     * @brief 一条数据库条目，重整形供重放。
     */
    struct CompileCommand
    {
        std::string directory; // 绝对、规范化（加载时解析所得）
        std::string file;      // 绝对、规范化
        /**
         * @brief 仅编译器开关：argv[0] 丢弃、"-c" 丢弃、"-o `<path>`"
         * （分写或连写）丢弃。
         *
         * @internal libclang 反正会忽略它们；丢弃使重放输入统一。
         */
        std::vector<std::string> argv;
        /**
         * @brief 数据库记录的编译器可执行文件原样拼写（argv[0]）。
         *
         * @internal 它是本条目投喂或写出的任何 BMI 的生产者身份；保持
         * 原样（可能相对；运行时按 directory 解析）。
         */
        std::string compiler;
        /**
         * @brief 本条目构建产出的 BMI（"-fmodule-output="，解析为绝对；
         * 条目不是模块接口单元时为空串）。
         *
         * @internal ccg 自身 BMI 配置键的生产者一半。
         */
        std::string module_output;
        /**
         * @brief 本条目投喂的命名模块 BMI（"-fmodule-file=NAME=path"，
         * 解析为绝对；经典 TU 为空）。
         *
         * @internal 投喂同时保留在 argv 中——本字段是供校验与分类的
         * 结构化孪生。
         */
        std::vector<ModuleFeed> module_feeds;
        /**
         * @brief 首个模块依赖映射响应文件（"@…modmap"，解析为绝对；无则
         * 空串）：姊妹 CXXModules.json 的锚点。
         */
        std::string module_map;
        /**
         * @brief 一级 P1689 扫描所得 TU 的直接模块 requires。
         *
         * @internal 无二级解析而降级的 TU 仍由此列表落其书写 import
         * （行 0 的 import 行）；扫描从未覆盖的条目为空。
         */
        std::vector<std::string> scan_imports;
        /**
         * @brief 命令合成阶段已预知的失败（自建降级：std BMI 不可构建、
         * 模块 DAG 环、扫描洞）：索引器逐字上报此文本而不解析该 TU。
         */
        std::string preordained_failure;
        /**
         * @brief BMI 级失败的重试观察路径（绝对、规范化、排序去重）。
         *
         * @internal 自建链对模块单元的 BMI 生产失败（自身接口不可用，或
         * 依赖模块失败而投喂被拒）时，把该单元 P1689 requires 闭包里每个
         * 接口依赖的源路径附着在命令上。索引器的失败记录把它们连同 stat
         * 身份冻结进 deps_json——失败 TU 自己的 includes 行无从产生（解析
         * 未发生或未成功），世界快照必须由命令侧补给，依赖源被修复后
         * deps_changed 才拉得回重试。空 = 无 BMI 级失败语境；本字段不参与
         * 任何摘要（cmd/feed）的计算。
         */
        std::vector<std::string> bmi_failure_deps;
        /** @brief 本 TU 解析的覆盖旋钮（见 ModuleOverlay）。 */
        ModuleOverlay overlay;
        /**
         * @brief 覆盖改写了其书写行的 import 名（伞头下的 std/std.compat、
         * include 改写下的头单元）。
         *
         * @internal 即使解析成功，索引器也把它们落成扫描通道的行 0
         * import 行（改写抹掉了它们的 import 游标）。
         */
        std::vector<std::string> overlay_imports;
        /**
         * @brief 条目是否携带 PCH 标记（消费或生产）。
         *
         * @internal 条目级门控：只有它为 true 的条目才走 PCH 归一化
         * （-Xclang 原子对、路径按 directory 绝对化、cl /Yu /Yc /Fp 守卫、
         * -include 转发化）。其余条目的 argv 逐字节保持历史形状。
         */
        bool pch_involved = false;
        /**
         * @brief loader 推断出的 cl 方言（`/Yc` `/Yu` `/Fp` 或
         * `--driver-mode=cl`）。
         *
         * @internal 重放与 producer 跳过判定共用的唯一信号：外来 MSVC cdb
         * 的 argv 是 `cl.exe /c /I ... /Ycpch.h /Fp...`，没有
         * `--driver-mode=cl`，只看该旗标会把记录误判成 GNU。非 PCH 条目
         * 保持 false（不影响历史 argv/摘要）。
         */
        bool cl_mode = false;
        /**
         * @brief 系统头来源已降级为 clang-cl 自动探测。
         *
         * @internal 外来原生 MSVC cdb 的 cl 路径没有工具集线索（如裸
         * `cl.exe`），或带线索但 vcvars 重建 INCLUDE 失败时为 true（原始
         * 命令已带 `/X` 时自动探测被关掉，故两种情况下都不标注）。摘要与
         * JSON 据此如实输出 `system-includes: auto-detected`，绝不静默
         * 假装精确。无 cl 条目的工程恒为 false（零变化门禁）。
         */
        bool system_includes_auto = false;
        /** @brief 记录的生产者输出（-o 或 /Fp），绝对。 */
        std::string pch_output;
        /** @brief 消费者引用的 PCH（-include-pch 或 /Fp），绝对。 */
        std::string pch_path;
        /** @brief /Yc 或 /Yu 的头拼写（原样；裸 /Yu 为空）。 */
        std::string pch_header;
        /** @brief 本条目是 PCH 生产者（-x c++-header+emit-pch 或 /Yc）。 */
        bool pch_create = false;
        /** @brief 本条目消费 PCH（-include-pch 或 /Yu）。 */
        bool pch_use = false;
        /** @brief 强制包含（-include / -FI），绝对。 */
        std::vector<std::string> forced_includes;
        /** @brief 规划器判定（见 PchUse）。 */
        PchUse pch;
        /**
         * @brief 文本回退 argv（影子 PCH 加载失败时解析器就地重试）。
         *
         * @internal 只有 pch_use 条目可能非空；不在 parse_digest 中。
         */
        std::vector<std::string> pch_fallback_argv;
    };

    /**
     * @brief 条目是否描述命名模块单元：模块味文件扩展名或重放 argv 中
     * 显式的 "-x c++-module"。
     *
     * @internal 扩展判定走 util 的分类单点（ASCII 折叠后缀——大写 .IXX
     * 同样是模块单元）。
     */
    [[nodiscard]]
    auto is_module_tu(const CompileCommand& cc) -> bool
    {
        if (cg::util::is_module_unit_file(cc.file))
        {
            return true;
        }
        for (std::size_t i = 0; i + 1 < cc.argv.size(); ++i)
        {
            if (cc.argv[i] == "-x"
                && (cc.argv[i + 1] == "c++-module"
                    || cc.argv[i + 1] == "cpp-module"))
            {
                return true;
            }
        }
        return false;
    }

    namespace detail
    {
        /**
         * @brief 带路径段边界的绝对路径前缀测试。
         *
         * @internal 根不得匹配仅共享前缀的更长姊妹路径。模块内部使用，不导出。
         */
        [[nodiscard]]
        auto path_under(std::string_view path, std::string_view root) -> bool
        {
            if (root.empty())
            {
                return path.empty();
            }
            if (path == root)
            {
                return true;
            }
            if (!path.starts_with(root))
            {
                return false;
            }
            return root.back() == '/' || path[root.size()] == '/';
        }

        /**
         * @brief PCH 标记 token 测试（消费或生产）。
         *
         * @internal 只认旗标本身，不认路径值——`-include-pch` 的直接与
         * `-Xclang` 转发拼写都表现为独立 token，故词法层即可发现。`/Yc` /
         * `/Yu` / `/Fp` 是 cl 的 PCH 旗标；`-emit-pch` 是 GNU 生产旗标。
         */
        [[nodiscard]]
        auto pch_marker_token(std::string_view t) -> bool
        {
            return t == "-include-pch" || t.starts_with("-include-pch=")
                   || t == "-emit-pch" || t.starts_with("/Yc")
                   || t.starts_with("/Yu") || t.starts_with("/Fp");
        }

        /**
         * @brief C/C++ 翻译单元扩展名测试（大小写不敏感）。
         *
         * @internal 判定收编至 util 的扩展分类单点（is_tu_input_file——集合
         * 与折叠规则只此一处）：cl 的分写 `/Yu`/`/Yc` 值只有在不是源文件时
         * 才是头名，否则会吞掉下一条 TU。
         */
        [[nodiscard]]
        inline auto has_source_ext_token(std::string_view token) -> bool
        {
            return cg::util::is_tu_input_file(token);
        }

        /**
         * @brief "下一 token 是值"守卫族（分写旗标的吞值判定，clang 层单点）。
         *
         * 适配层/装载层/PCH/BMI 四处剥离与重放循环曾各自内联这套判定，口径
         * 漂移即吞值或漏吞。判据是三型（刻意不成单函数——各型的旗标世界与
         * 排除面不同）：
         * 1. cl 型——cl 世界旗标以 '/' 或 '-' 开头（cl 的路径值带盘符，
         *    不以 '/' 开头），下一 token 不是旗标即为值（输出路径类）；
         * 2. cl 头值型——在 cl 型之上再排除源文件扩展（PCH/模块类旗标：值
         *    是头名或产物路径，源文件是解析/重放的输入，绝不吞）；
         * 3. gnu 型——gnu 世界旗标只以 '-' 开头，下一 token 不以 '-' 开头
         *    即为值（-include 类；'/' 开头的路径在 gnu 世界是值）。
         */
        ///@{
        /** @brief token 是否 cl 世界旗标（'/' 或 '-' 开头）。 */
        [[nodiscard]]
        inline auto is_cl_flag_token(std::string_view token) -> bool
        {
            return token.starts_with('/') || token.starts_with('-');
        }

        /** @brief token 是否 gnu 世界旗标（'-' 开头）。 */
        [[nodiscard]]
        inline auto is_gnu_flag_token(std::string_view token) -> bool
        {
            return token.starts_with('-');
        }

        /** @brief cl 型：位置 i 的下一 token
         * 存在且不是旗标（输出路径类分写值）。 */
        [[nodiscard]]
        inline auto cl_value_follows(
            std::span<const std::string> argv,
            std::size_t                  i) -> bool
        {
            return i + 1 < argv.size() && !is_cl_flag_token(argv[i + 1]);
        }

        /** @brief cl 头值型：cl 型守卫且下一 token
         * 不是源文件（PCH/模块类分写值）。 */
        [[nodiscard]]
        inline auto cl_header_value_follows(
            std::span<const std::string> argv,
            std::size_t                  i) -> bool
        {
            return cl_value_follows(argv, i)
                   && !has_source_ext_token(argv[i + 1]);
        }

        /** @brief gnu 型：位置 i 的下一 token 存在且不以 '-' 开头（-include
         * 类分写值）。 */
        [[nodiscard]]
        inline auto gnu_value_follows(
            std::span<const std::string> argv,
            std::size_t                  i) -> bool
        {
            return i + 1 < argv.size() && !is_gnu_flag_token(argv[i + 1]);
        }
        ///@}

        /** @brief 编译器 token 的基名（去目录、小写、去 .exe）。 */
        [[nodiscard]]
        inline auto compiler_basename(std::string_view compiler) -> std::string
        {
            std::string base =
                cg::util::ascii_lower(cg::util::path_basename(compiler));
            if (base.ends_with(".exe"))
            {
                base.resize(base.size() - 4);
            }
            return base;
        }

        /**
         * @brief 编译器是否 cl 家族（cl/clang-cl）。
         *
         * @internal 只有这两个名字才注入 cl 方言；clang/clang++ 条目逐字节
         * 不变（硬门禁）。
         */
        [[nodiscard]]
        inline auto compiler_is_cl_family(std::string_view compiler) -> bool
        {
            const std::string base = compiler_basename(compiler);
            return base == "cl" || base == "clang-cl";
        }

        /** @brief 编译器是否原生 MSVC cl（而非 clang-cl）。 */
        [[nodiscard]]
        inline auto compiler_is_native_cl(std::string_view compiler) -> bool
        {
            return compiler_basename(compiler) == "cl";
        }

        /**
         * @brief token 是否为 cl 的 IgnoreStandardIncludePath（`/X`）。
         *
         * @internal cl 接受 '/' 与 '-' 前缀且该选项大小写不敏感。clang-cl 只
         * 认规范 `/X` 与 `-X`（实测 `/x` 被当作输入文件），因此共享判定接受
         * `/X`、`/x`、`-X`；`-x` 只在确知是原生 cl 时才按 /X 对待（在
         * clang-cl 里它是语言选项，不能改写）。
         */
        [[nodiscard]]
        inline auto is_ignore_standard_include(std::string_view token) -> bool
        {
            if (token.size() != 2)
            {
                return false;
            }
            if (token[0] == '/')
            {
                return token[1] == 'X' || token[1] == 'x';
            }
            return token[0] == '-' && token[1] == 'X';
        }

        [[nodiscard]]
        auto has_pch_marker(std::span<const std::string> tokens) -> bool
        {
            for (const std::string& t : tokens)
            {
                if (pch_marker_token(t))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 隐式 PCH 探测：`-include`（含 `-Xclang -include` 转发）指向的
         *        头文件旁存在 `<header>.pch` / `<header>.gch`。
         *
         * @internal clang 驱动器只为顶层 `-include` 注入隐式 PCH（`-###` 实测：
         * `-Xclang -include` 不会），故命中后 PCH 感知解析把该旗标改写为
         * `-Xclang -include -Xclang <abs>` 即可绕开自动加载。相对值按条目目录
         * （记录的 CWD）解析。源码首个 `#include` 不触发 clang 的隐式加载
         * （实测 `-###` 无 `-include-pch`），无需探测源文件。
         */
        [[nodiscard]]
        auto implicit_pch_via_include(
            std::span<const std::string> tokens,
            const std::string&           dir) -> bool
        {
            namespace fs = std::filesystem;
            const auto probe = [&](std::string_view raw)
            {
                if (raw.empty())
                {
                    return false;
                }
                std::string path{raw};
                if (fs::path{path}.is_relative())
                {
                    path = (fs::path{dir} / path)
                               .lexically_normal()
                               .generic_string();
                }
                std::error_code ec;
                return fs::is_regular_file(fs::path{path + ".pch"}, ec)
                       || fs::is_regular_file(fs::path{path + ".gch"}, ec);
            };
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const std::string& t = tokens[i];
                if (t == "-Xclang" && i + 3 < tokens.size()
                    && tokens[i + 1] == "-include"
                    && tokens[i + 2] == "-Xclang")
                {
                    if (probe(tokens[i + 3]))
                    {
                        return true;
                    }
                    i += 3;
                    continue;
                }
                if (t == "-include" && gnu_value_follows(tokens, i))
                {
                    if (probe(tokens[i + 1]))
                    {
                        return true;
                    }
                    ++i;
                }
            }
            return false;
        }

        /**
         * @brief 响应文件里是否出现 PCH 标记（只读探测；不改变展开语义）。
         *
         * @internal 必须在展开前决定条目是否走 PCH 归一化，而 cl 的
         * `AdditionalOptions` 可能整体记录在 @file 中。探测只读文件、按同一
         * tokenizer 切分并递归一层；读不到的文件忽略（展开阶段会按原语义
         * 丢弃）。
         */
        [[nodiscard]]
        auto rsp_has_pch_marker(
            std::span<const std::string> tokens,
            const std::string&           dir,
            std::size_t                  depth = 0) -> bool
        {
            namespace fs = std::filesystem;
            if (depth > 16)
            {
                return false;
            }
            for (const std::string& t : tokens)
            {
                if (!t.starts_with('@') || t.size() < 2)
                {
                    continue;
                }
                std::string rsp = t.substr(1);
                if (fs::path{rsp}.is_relative())
                {
                    rsp = (fs::path{dir} / rsp)
                              .lexically_normal()
                              .generic_string();
                }
                const auto rsp_text = cg::util::read_text_file(rsp);
                if (!rsp_text.has_value())
                {
                    continue;
                }
                const std::vector<std::string> inner =
                    cg::util::tokenize_command(*rsp_text);
                if (has_pch_marker(inner)
                    || rsp_has_pch_marker(inner, dir, depth + 1))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 模板模式旗标是否已在条目 argv 里显式声明。
         *
         * @internal 与 cg.msbuild.translate 的同名判定同一规则（cl 前缀
         * '/' 与 '-' 皆认、该族选项大小写不敏感，按小写全量拼写比对）：
         * /permissive- 或 /Zc:twoPhase（严格）与 /Zc:twoPhase-（延迟）任一
         * 在场即已声明。
         */
        [[nodiscard]]
        inline auto template_mode_declared(
            const std::vector<std::string>& tokens) -> bool
        {
            for (const std::string& t : tokens)
            {
                const std::string low = cg::util::ascii_lower(t);
                if (low == "/permissive-" || low == "-permissive-"
                    || low == "/zc:twophase" || low == "-zc:twophase"
                    || low == "/zc:twophase-" || low == "-zc:twophase-")
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 原地修剪宏定义值两端的空白。
         *
         * 工程文件里手滑的 "NAME= VALUE"（MSBuild 原样透传、MSVC 与
         * clang 的预处理器都宽容值的前导空白）在此归一为 "NAME=VALUE"。
         * 两段式（/D 与值分立）与连写（/DNAME=..）两种形态都处理，'/'
         * 与 '-' 前缀等价、大小写不敏感；只修剪值两端的空白，字符串字
         * 面量内部的空白（"a b"）原样保留。
         *
         * @internal cdb 载入与 pch sidecar 载入必须共用本函数：影子
         * PCH 烙进的宏值与消费者命令行（cdb 已归一化）须按字节一致，
         * clang 装载 PCH 按字节比对宏值，任一侧带原始空白即拒载、整组
         * TU 降级文本重放。
         */
        auto normalize_macro_values(std::vector<std::string>& tokens) -> void
        {
            for (std::size_t di = 0; di < tokens.size(); ++di)
            {
                std::string& t = tokens[di];
                const bool   two_token = t.size() == 2
                                         && (t[0] == '/' || t[0] == '-')
                                         && (t[1] == 'D' || t[1] == 'd');
                const std::string_view flag =
                    two_token ? std::string_view{} : std::string_view{t};
                if (!two_token
                    && !(
                        flag.size() > 2 && (flag[0] == '/' || flag[0] == '-')
                        && (flag[1] == 'D' || flag[1] == 'd')
                        && flag.contains('=')))
                {
                    continue;
                }
                std::string& def =
                    two_token && di + 1 < tokens.size() ? tokens[di + 1] : t;
                const std::size_t eq = def.find('=');
                if (eq == std::string::npos || eq + 1 >= def.size())
                {
                    continue;
                }
                std::string_view value{
                    def.c_str() + eq + 1,
                    def.size() - eq - 1};
                const std::size_t front = value.find_first_not_of(" \t");
                const std::size_t back = value.find_last_not_of(" \t");
                if (front == std::string_view::npos
                    || (front == 0 && back + 1 == value.size()))
                {
                    continue;
                }
                def = def.substr(0, eq + 1)
                      + std::string{value.substr(front, back - front + 1)};
            }
        }

        /**
         * @brief 旁车消费者记录的识别面。
         *
         * @internal pch.json consumers 一条记录中参与消费者身份裁决的
         * 两个字段：与 producer 的关联键（绝对 /Fp）和 /Yu 伞头拼写。
         * file 是数据源组织信息（文件归属），不进裁决。sidecar 的解析
         * 结构留在 cg.clang.pch，识别面以本结构过界，两模块不互相依赖。
         */
        struct SidecarPchFacts
        {
            std::string pch_key;    // 绝对 /Fp（与 producer 的关联键）
            std::string pch_header; // /Yu 的头拼写（原样）
        };

        /**
         * @brief PCH 消费者身份的最终判定。
         *
         * @internal sidecar_sourced 是来源注记：判定取自旁车记录为
         * true（调用方据此补 pch_involved），取自 cdb 令牌或判非消费
         * 者为 false。
         */
        struct PchConsumerVerdict
        {
            bool        use = false;
            bool        sidecar_sourced = false;
            std::string path;
            std::string header;
        };

        /**
         * @brief PCH 消费者身份的唯一裁决点（cdb 令牌识别 vs pch.json
         * 旁车记录）。
         *
         * 输入两路各自的原始识别：cdb 侧是 loader 产出的结构化字段
         * （pch_use/pch_path/pch_header——装载层只做令牌提取，不裁决）；
         * 旁车侧是调用方按文件归属查得的记录（无记录传 nullptr）。
         * 输出最终判定，规划器的两处消费（producer 引用收集、消费者
         * 改写）都必须经本函数，不再各自拼合并。
         *
         * 优先级（显式写死）：旁车为准，cdb 令牌补充——cdb 未判成消
         * 费者而旁车有该文件记录时以旁车为准；cdb 令牌已判成消费者的
         * 条目保持 cdb 结果，旁车不改写。工程管线里两源互斥：MSBuild
         * 适配器写的 cdb 剥离 /Yc /Yu /Fp（旁车是该场景唯一来源），
         * GNU 风格 cdb 没有旁车；两源同时命中同一 TU 只见于手工拼造
         * 的输入，此时保持 cdb 侧判定与历史合并行为逐条一致。
         */
        [[nodiscard]]
        auto resolve_pch_consumer(
            const CompileCommand&  cmd,
            const SidecarPchFacts* sidecar) -> PchConsumerVerdict
        {
            if (cmd.pch_use)
            {
                return PchConsumerVerdict{
                    .use = true,
                    .sidecar_sourced = false,
                    .path = cmd.pch_path,
                    .header = cmd.pch_header};
            }
            if (sidecar != nullptr)
            {
                return PchConsumerVerdict{
                    .use = true,
                    .sidecar_sourced = true,
                    .path = sidecar->pch_key,
                    .header = sidecar->pch_header};
            }
            return PchConsumerVerdict{};
        }
    } // namespace detail

    /**
     * @brief 加载一份 compile_commands.json。
     *
     * 解析出的绝对 "file" 不在任一 root_filters 之下的条目被丢弃；空 span
     * 保留全部。相对 "directory" 字段按数据库文件所在目录解析；相对
     * "file" 字段按其条目的 directory 解析。畸形条目（无 "file"、无
     * "command"/"arguments"）跳过而非致命。文件不可读或不是 JSON 数组时
     * 抛 std::runtime_error。
     *
     * @internal
     * 条目整形要点：两种形状（"command" 单条 Windows 命令串 / "arguments"
     * 已切分 argv）都保留 argv[1:]，编译器 token（argv[0]）单独留作 BMI
     * 生产者身份。"@file" 响应 token 必须展开（CMake 4.4 以此写模块单元的
     * 模块依赖映射）：驱动器把它们拼进命令行、libclang 不能——且它们携带
     * 的 flag（-fmodule-file=NAME=path.pcm）恰是命名模块 TU 能解析的命脉。
     * 展开一层即可（嵌套 @ 文件不存在）。"-x `<language>`" 的值不是路径、
     * 原样保留；"-fmodule-output=" 只命名构建会写的 BMI——从重放 argv
     * 丢弃但记录进 module_output。非解析参数剥离："-c" 及其后的输入文件
     * （杂散位置路径会令 libclang 以 CXError_ASTReadError 失败）、"-o"
     * 及其路径、连写 "-o`<path>`"、"@file"（未展开的 @token 会被当作第二个
     * 输入文件）。带路径值的 flag（"-I"、"-isystem"…）按条目目录解析：
     * libclang 按进程工作目录解释相对 include 路径，那不是记录的编译目录。
     * 裸命令行上的模块 flag 与 rsp 孪生同法捕获进结构化字段。CMake 的
     * 合成重扫条目（@synth_N）携带 "--precompile"：它命令驱动器产出 BMI，
     * libclang 做不到（rc=4）——索引的是接口单元的解析，该 flag 是构建
     * 模式而非解析输入。连写 "-fmodule-file=NAME=path" 是 CMake/Ninja 记录
     * 的形状；从展开 @modmap 拼来的 token 已解析过、须去重。条目自身的
     * 主文件规范化一次（合成重扫条目以裸位置参数命名输入、无 -c，杂散
     * 位置输入即两个输入文件的失败——恰好丢弃该 token、他者不动）。
     * note_feed 消化的 joined_value 形如 "NAME=path"，调用方随后解析路径
     * 并把 .pcm 修正为解析后的拼写。
     * file 先对 directory 解析、再二者规范化入库：norm_path 的输出绝不可
     * 再喂给文件系统拼接（且其自身必须幂等）。
     */
    [[nodiscard]]
    auto load(
        std::string_view             db_path,
        std::span<const std::string> root_filters)
        -> std::vector<CompileCommand>
    {
        namespace fs = std::filesystem;
        namespace json = cg::util::json;

        const std::optional<std::string> db_text =
            cg::util::read_text_file(std::string{db_path});
        if (!db_text.has_value())
        {
            throw std::runtime_error(
                std::format("cannot open compile database: {}", db_path));
        }

        const fs::path db_dir =
            fs::path{std::string_view{db_path}}.parent_path();
        const std::string db_dir_str = db_dir.generic_string();

        const json::Document  doc = json::Document::parse(*db_text);
        const json::ValueView root = doc.root();
        if (!root.is_array())
        {
            throw std::runtime_error(
                std::format(
                    "compile database is not a JSON array: {}",
                    db_path));
        }

        std::vector<CompileCommand> out;
        const std::size_t           count = root.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto entry_view = root.at(i);
            if (!entry_view || !entry_view->is_object())
            {
                continue;
            }
            const json::ValueView entry = *entry_view;

            const auto field =
                [&entry](std::string_view key) -> std::optional<std::string>
            {
                if (const auto v = entry.get(key); v && v->is_string())
                {
                    return std::string{*v->as_string()};
                }
                return std::nullopt;
            };

            const std::optional<std::string> file_field = field("file");
            if (!file_field)
            {
                continue;
            }

            std::string              compiler;
            std::vector<std::string> tokens;
            if (const auto args = entry.get("arguments");
                args && args->is_array())
            {
                if (const auto v = args->at(0); v && v->is_string())
                {
                    compiler = std::string{*v->as_string()};
                }
                const std::size_t n = args->size();
                for (std::size_t j = 1; j < n; ++j)
                {
                    if (const auto v = args->at(j); v && v->is_string())
                    {
                        tokens.emplace_back(*v->as_string());
                    }
                }
            }
            else if (const auto cmd = field("command"))
            {
                tokens = cg::util::tokenize_command(*cmd);
                if (!tokens.empty())
                {
                    compiler = std::move(tokens.front());
                    tokens.erase(tokens.begin());
                }
            }
            else
            {
                continue;
            }

            std::string dir = field("directory").value_or(".");
            if (fs::path{dir}.is_relative())
            {
                dir = (fs::path{db_dir_str} / dir)
                          .lexically_normal()
                          .generic_string();
            }
            while (dir.size() > 3 && dir.back() == '/')
            {
                dir.pop_back(); // lexically_normal 保留尾部分隔符
            }

            const auto resolve_arg = [&](const std::string& value)
            {
                if (fs::path{value}.is_absolute())
                {
                    return cg::util::norm_path(value);
                }
                return cg::util::norm_path((fs::path{dir} / value)
                                               .lexically_normal()
                                               .generic_string());
            };
            const std::optional<std::string> output_field = field("output");
            // 条目级 PCH 门控：先看命令行，再看响应文件内容。只有涉及 PCH
            // 的条目才允许后续归一化触碰 argv（零变化门禁）。
            const bool pch_hint = detail::has_pch_marker(tokens)
                                  || detail::rsp_has_pch_marker(tokens, dir);
            CompileCommand cc;
            cc.compiler = std::move(compiler);
            const auto note_feed = [&](std::string_view joined_value)
            {
                const std::size_t eq = joined_value.find('=');
                if (eq == std::string_view::npos || eq == 0
                    || eq + 1 >= joined_value.size())
                {
                    return;
                }
                cc.module_feeds.push_back(
                    ModuleFeed{
                        .name = std::string{joined_value.substr(0, eq)},
                        .pcm = std::string{joined_value.substr(eq + 1)}});
            };
            const auto expand_response_files = [&](std::vector<std::string> in)
            {
                std::vector<std::string> out;
                const auto resolve_module_arg = [&](std::string arg)
                {
                    const auto fix_path_suffix = [&](std::string_view prefix)
                    {
                        if (!arg.starts_with(prefix))
                        {
                            return false;
                        }
                        std::string tail = arg.substr(prefix.size());
                        if (const std::size_t eq = tail.find('=');
                            eq != std::string::npos)
                        {
                            note_feed(tail);
                            tail = tail.substr(0, eq + 1)
                                   + resolve_arg(tail.substr(eq + 1));
                            cc.module_feeds.back().pcm = tail.substr(eq + 1);
                        }
                        else
                        {
                            tail = resolve_arg(tail);
                        }
                        arg = std::string{prefix} + tail;
                        return true;
                    };
                    if (fix_path_suffix("-fmodule-file="))
                    {
                        return arg;
                    }
                    if (!arg.empty() && arg[0] != '-')
                    {
                        return resolve_arg(arg);
                    }
                    return arg;
                };
                for (std::string& t : in)
                {
                    if (!t.starts_with('@') || t.size() < 2)
                    {
                        out.push_back(std::move(t));
                        continue;
                    }
                    std::string rsp = t.substr(1);
                    if (fs::path{rsp}.is_relative())
                    {
                        rsp = (fs::path{dir} / rsp)
                                  .lexically_normal()
                                  .generic_string();
                    }
                    if (cc.module_map.empty())
                    {
                        cc.module_map = cg::util::norm_path(rsp);
                    }
                    const auto rsp_text = cg::util::read_text_file(rsp);
                    if (!rsp_text.has_value())
                    {
                        continue;
                    }
                    bool rsp_after_x = false;
                    for (std::string& a : cg::util::tokenize_command(*rsp_text))
                    {
                        if (pch_hint)
                        {
                            // PCH 条目：响应文件 token 逐字进入主循环，由
                            // PCH 感知解析器按 directory 绝对化路径值；此处
                            // 绝不把 cl 的 `/D` 值误当路径。模块产物旗标仍
                            // 按结构化字段吞掉。
                            if (a.starts_with("-fmodule-output="))
                            {
                                cc.module_output =
                                    resolve_arg(a.substr(a.find('=') + 1));
                                continue;
                            }
                            out.push_back(std::move(a));
                            continue;
                        }
                        if (a == "-x")
                        {
                            rsp_after_x = true;
                            out.push_back(std::move(a));
                            continue;
                        }
                        if (a.starts_with("-fmodule-output="))
                        {
                            cc.module_output =
                                resolve_arg(a.substr(a.find('=') + 1));
                            continue; // 构建产物，非解析输入
                        }
                        if (rsp_after_x)
                        {
                            rsp_after_x = false;
                            out.push_back(std::move(a)); // -x 的语言值
                            continue;
                        }
                        out.push_back(resolve_module_arg(std::move(a)));
                    }
                }
                return out;
            };
            tokens = expand_response_files(std::move(tokens));

            // ---- 外来 MSVC 条目的方言与系统头补齐 ----
            // 原生 cdb（cl.exe /c ...）没有 --driver-mode=cl 时，重放会走
            // GNU 方言、静默用 libc++ 建图。按编译器基名注入（放在编译器
            // token 之后，即 argv[1] 位置）；clang/clang++ 条目绝不触碰。
            const bool cl_family = detail::compiler_is_cl_family(cc.compiler);
            if (cl_family
                && std::ranges::find(tokens, "--driver-mode=cl")
                       == tokens.end())
            {
                tokens.insert(tokens.begin(), "--driver-mode=cl");
            }
            if (cl_family)
            {
                // clang-cl 只认规范 `/X` 与 `-X`：把 cl 的 `/x` 规范化，
                // 并让重复拼写只留一个。`-x` 不在此处理——在 clang-cl 里
                // 它是语言选项，只有原生 cl 才按 /X 对待（见下）。
                bool saw_ignore = false;
                if (std::ranges::any_of(
                        tokens,
                        detail::is_ignore_standard_include))
                {
                    std::vector<std::string> canonical;
                    canonical.reserve(tokens.size());
                    for (const std::string& t : tokens)
                    {
                        if (detail::is_ignore_standard_include(t))
                        {
                            if (!saw_ignore)
                            {
                                canonical.emplace_back("/X");
                            }
                            saw_ignore = true;
                            continue;
                        }
                        canonical.push_back(t);
                    }
                    tokens = std::move(canonical);
                }
            }
            // 外来原生 cl 条目的系统头补齐（第 3 层）：按 cl.exe 路径反推
            // 工具集版本 → vcvars 捕获完整有序 INCLUDE → 与 ccg msbuild
            // 第 1 层同形物化（/X + -imsvc + 兼容版本）。两件事必须解耦：
            //  - 兼容版本只依赖编译器路径，原始命令带不带 /X 都要追加，
            //    否则 /X（IgnoreStandardIncludePath）的 TU 会静默走错
            //    `#if _MSC_VER` 分支；
            //  - INCLUDE 物化才是 /X 条件性的：原始命令已带 /X（任意合法
            //    拼写）时 cl 本会忽略 INCLUDE，不得追加。
            if (cl_family && detail::compiler_is_native_cl(cc.compiler))
            {
                // 原生 cl 的 `-x` 就是 /X（cl 选项前缀不敏感）；clang-cl
                // 会把它当语言选项，必须规范化为 `/X`。共享拼写已在上面
                // 处理，这里只剩 `-x` 与去重。
                bool ignores_include =
                    std::ranges::find(tokens, "/X") != tokens.end();
                if (std::ranges::find(tokens, "-x") != tokens.end())
                {
                    ignores_include = true;
                    bool                     have_x = false;
                    std::vector<std::string> canonical;
                    canonical.reserve(tokens.size());
                    for (const std::string& t : tokens)
                    {
                        if (t == "-x")
                        {
                            if (!have_x)
                            {
                                canonical.emplace_back("/X");
                            }
                            have_x = true;
                            continue;
                        }
                        if (t == "/X")
                        {
                            if (have_x)
                            {
                                continue;
                            }
                            have_x = true;
                        }
                        canonical.push_back(t);
                    }
                    tokens = std::move(canonical);
                }
                const MsvcToolsetPath toolset =
                    parse_msvc_toolset_path(cc.compiler);
                if (toolset.version.empty())
                {
                    // 连工具集线索都没有：只做方言修正，并让自动探测状态
                    // 可见（/X 时自动探测已被关掉，无需标注）。
                    if (!ignores_include)
                    {
                        cc.system_includes_auto = true;
                    }
                }
                else
                {
                    if (const std::string compat =
                            msvc_compat_version(toolset.version);
                        !compat.empty())
                    {
                        tokens.push_back(
                            std::format(
                                "-fms-compatibility-version={}",
                                compat));
                    }
                    if (!ignores_include)
                    {
                        const MsvcIncludeCapture captured =
                            capture_msvc_include_env(cc.compiler, dir);
                        if (captured.ok)
                        {
                            tokens.emplace_back("/X");
                            // INCLUDE 是 ';' 分隔的完整有序集合；空段跳过。
                            for (const std::string_view item :
                                 cg::util::split(captured.include_env, ';'))
                            {
                                if (!item.empty())
                                {
                                    tokens.emplace_back("-imsvc");
                                    tokens.emplace_back(
                                        cg::util::to_forward_slashes(item));
                                }
                            }
                        }
                        else
                        {
                            cc.system_includes_auto = true;
                        }
                    }
                }
            }
            // 模板模式等效旗标：仅原生 cl.exe 著条目——MSVC 默认（未显式
            // 声明 /permissive- 或 /Zc:twoPhase*）是延迟解析，/Zc:twoPhase-
            // 是该语义在 clang-cl 里的官方拼写（-fdelayed-template-parsing
            // 的别名）。clang-cl 自著条目忠实保留其自身语义不加（ccg
            // msbuild 翻译产物已在生成 cdb 时落旗标，同样不再补）。补在
            // 装载期的内存副本（外来 cdb 文件绝不改写），db 记录的 argv
            // 可见本旗标。
            if (cl_family && detail::compiler_is_native_cl(cc.compiler)
                && !detail::template_mode_declared(tokens))
            {
                tokens.emplace_back("/Zc:twoPhase-");
            }

            // 宏定义值的空白兜底（语义见 detail::normalize_macro_values）：
            // 重放 argv 与 digest 因此得到确定性的干净形态，任何来源的
            // 编译数据库（msbuild 采集/手写/外部生成）都受益。
            detail::normalize_macro_values(tokens);

            const auto is_include_flag = [](std::string_view t)
            {
                return t == "-I" || t == "-isystem" || t == "-iquote"
                       || t == "-idirafter" || t == "-include";
            };
            const auto is_joined_include = [](std::string_view t)
            {
                return (t.starts_with("-I") && t.size() > 2)
                       || (t.starts_with("-iquote") && t.size() > 7);
            };
            const std::string main_norm = cg::util::norm_path(
                [&]
                {
                    fs::path p{*file_field};
                    if (p.is_relative())
                    {
                        p = fs::path{dir} / p;
                    }
                    return p.lexically_normal().generic_string();
                }());
            const bool pch_involved =
                pch_hint || detail::has_pch_marker(tokens)
                || detail::implicit_pch_via_include(tokens, dir);
            cc.pch_involved = pch_involved;
            // 下游 PCH 感知分支与普通分支共享的 token 段（@file /
            // -fmodule-output= / -fmodule-file= / include 旗标）。分写与连写
            // argv 形态在 helper 内原样保留——cmd_digest 指纹依赖命令行形态，
            // 统一拼写即全库判脏。
            const auto note_rsp_token = [&](std::string_view t)
            {
                if (cc.module_map.empty() && t.size() >= 2)
                {
                    std::string rsp{t.substr(1)};
                    if (fs::path{rsp}.is_relative())
                    {
                        rsp = (fs::path{dir} / rsp)
                                  .lexically_normal()
                                  .generic_string();
                    }
                    cc.module_map = cg::util::norm_path(rsp);
                }
            };
            const auto note_module_output_flag = [&](std::string_view t)
            {
                cc.module_output =
                    resolve_arg(std::string{t.substr(t.find('=') + 1)});
            };
            // -fmodule-file=NAME=path：名与路径进结构化 feeds（重放去重），
            // argv 重写解析后的路径段；名或路径为空时返回 false（token 不
            // 消费，落穿后续分支，与原两分支逐字一致）。
            const auto try_module_file_flag = [&](const std::string& t) -> bool
            {
                if (!t.starts_with("-fmodule-file="))
                {
                    return false;
                }
                const std::size_t eq1 = t.find('=');          // flag 之后
                const std::size_t eq2 = t.find('=', eq1 + 1); // 名|路径
                if (eq2 == std::string::npos || eq2 <= eq1 + 1
                    || eq2 + 1 >= t.size())
                {
                    return false;
                }
                const std::string name = t.substr(eq1 + 1, eq2 - eq1 - 1);
                const std::string pcm = resolve_arg(t.substr(eq2 + 1));
                const bool        seen = std::ranges::any_of(
                    cc.module_feeds,
                    [&](const ModuleFeed& feed)
                    { return feed.name == name && feed.pcm == pcm; });
                if (!seen)
                {
                    cc.module_feeds.push_back(
                        ModuleFeed{.name = name, .pcm = pcm});
                }
                cc.argv.push_back(t.substr(0, eq2 + 1) + pcm);
                return true;
            };
            // 分写 include 旗标：值 token（如有）一并消费并按条目目录绝对化。
            const auto push_include_token =
                [&](const std::string& t, std::size_t& j)
            {
                cc.argv.push_back(t);
                if (j + 1 < tokens.size())
                {
                    cc.argv.push_back(resolve_arg(tokens[++j]));
                }
            };
            const auto push_joined_include = [&](const std::string& t)
            { cc.argv.push_back(t.substr(0, 2) + resolve_arg(t.substr(2))); };
            if (pch_involved)
            {
                // ---- PCH 感知解析 ----
                // 唯一目标：把构建系统记录的 PCH 语义完整搬进结构化字段，并
                // 让消费者 argv 对 libclang 可重放。`-Xclang <arg>` 是原子
                // 转发对；`-Xclang -include -Xclang V` 是两级转发，V 是路径
                // 值、按条目目录绝对化。绝不把转发 token 当顶层旗标消费。
                const bool cl_mode =
                    std::ranges::find(tokens, "--driver-mode=cl")
                        != tokens.end()
                    || std::ranges::any_of(
                        tokens,
                        [](const std::string& a)
                        {
                            return a.starts_with("/Yc") || a.starts_with("/Yu")
                                   || a.starts_with("/Fp");
                        })
                    // cc.compiler 是 argv[0] 的独立副本（原 token 已
                    // 移入此处）。
                    || detail::compiler_is_cl_family(cc.compiler);
                cc.cl_mode = cl_mode;
                const auto take =
                    [&](std::size_t& k) -> std::optional<std::string>
                {
                    if (k + 1 >= tokens.size())
                    {
                        return std::nullopt;
                    }
                    ++k;
                    return tokens[k];
                };
                const auto pchish = [](std::string_view p)
                { return p.ends_with(".pch") || p.ends_with(".gch"); };
                bool        emit_pch = false;
                bool        header_mode = false;
                bool        yc_set = false;
                std::string out_flag;
                std::string fp_path;
                std::size_t j = 0;
                while (j < tokens.size())
                {
                    const std::string t = tokens[j];
                    if (t == "-Xclang" && j + 1 < tokens.size())
                    {
                        const std::string v = tokens[j + 1];
                        if (v == "-include" || v == "-include-pch")
                        {
                            cc.argv.push_back("-Xclang");
                            cc.argv.push_back(v);
                            j += 2;
                            std::string raw;
                            if (j < tokens.size() && tokens[j] == "-Xclang"
                                && j + 1 < tokens.size())
                            {
                                raw = tokens[j + 1];
                                j += 2;
                            }
                            else if (
                                j < tokens.size()
                                && !detail::is_gnu_flag_token(tokens[j]))
                            {
                                raw = tokens[j];
                                ++j;
                            }
                            const std::string abs =
                                raw.empty() ? std::string{} : resolve_arg(raw);
                            if (!abs.empty())
                            {
                                cc.argv.push_back("-Xclang");
                                cc.argv.push_back(abs);
                            }
                            if (v == "-include-pch")
                            {
                                cc.pch_use = true;
                                cc.pch_path = abs;
                            }
                            else
                            {
                                cc.forced_includes.push_back(abs);
                            }
                            continue;
                        }
                        if (v == "-emit-pch")
                        {
                            emit_pch = true;
                        }
                        cc.argv.push_back("-Xclang");
                        cc.argv.push_back(v);
                        j += 2;
                        continue;
                    }
                    if (t == "-x" && j + 1 < tokens.size())
                    {
                        if (tokens[j + 1] == "c++-header"
                            || tokens[j + 1] == "c-header")
                        {
                            header_mode = true;
                        }
                        cc.argv.push_back(t);
                        cc.argv.push_back(tokens[j + 1]);
                        j += 2;
                        continue;
                    }
                    if (t == "-emit-pch")
                    {
                        emit_pch = true;
                        cc.argv.push_back(t);
                        ++j;
                        continue;
                    }
                    if (t == "-include")
                    {
                        std::string raw;
                        if (const auto v = take(j))
                        {
                            raw = *v;
                        }
                        if (!raw.empty())
                        {
                            const std::string abs = resolve_arg(raw);
                            cc.forced_includes.push_back(abs);
                            // 转发形式绕过 clang 对同名 .pch 的隐式加载
                            // （隐式 PCH 与 DPR 硬冲突，实测 rc=4）。
                            cc.argv.push_back("-Xclang");
                            cc.argv.push_back("-include");
                            cc.argv.push_back("-Xclang");
                            cc.argv.push_back(abs);
                        }
                        ++j;
                        continue;
                    }
                    if (t == "-include-pch" || t.starts_with("-include-pch="))
                    {
                        std::string raw;
                        if (t == "-include-pch")
                        {
                            if (const auto v = take(j))
                            {
                                raw = *v;
                            }
                        }
                        else
                        {
                            raw = t.substr(
                                std::string_view{"-include-pch="}.size());
                        }
                        cc.pch_use = true;
                        cc.pch_path =
                            raw.empty() ? std::string{} : resolve_arg(raw);
                        cc.argv.push_back("-Xclang");
                        cc.argv.push_back("-include-pch");
                        if (!cc.pch_path.empty())
                        {
                            cc.argv.push_back("-Xclang");
                            cc.argv.push_back(cc.pch_path);
                        }
                        ++j;
                        continue;
                    }
                    if (t == "-o")
                    {
                        std::string raw;
                        if (const auto v = take(j))
                        {
                            raw = *v;
                        }
                        if (!raw.empty())
                        {
                            out_flag = resolve_arg(raw);
                        }
                        ++j;
                        continue;
                    }
                    if (t.starts_with("-o") && t.size() > 2)
                    {
                        out_flag = resolve_arg(t.substr(2));
                        ++j;
                        continue;
                    }
                    if (t == "-c")
                    {
                        if (detail::gnu_value_follows(tokens, j))
                        {
                            ++j;
                        }
                        ++j;
                        continue;
                    }
                    if (t == "--precompile")
                    {
                        ++j;
                        continue;
                    }
                    if (t.starts_with('@'))
                    {
                        note_rsp_token(t);
                        ++j;
                        continue;
                    }
                    if (t.starts_with("-fmodule-output="))
                    {
                        note_module_output_flag(t);
                        ++j;
                        continue;
                    }
                    if (try_module_file_flag(t))
                    {
                        ++j;
                        continue;
                    }
                    if (cl_mode)
                    {
                        if (t == "/c")
                        {
                            ++j;
                            continue;
                        }
                        if (!detail::is_cl_flag_token(t)
                            && resolve_arg(t) == main_norm)
                        {
                            ++j; // 条目自身的输入：解析器另行传入
                            continue;
                        }
                        const auto cl_sep = [&]()
                        {
                            return t == "/Fp" || t == "/Fo" || t == "/Fd"
                                   || t == "/I" || t == "/external:I"
                                   || t == "/FI" || t == "/D" || t == "/U"
                                   || t == "/FU" || t == "/AI" || t == "/arch"
                                   || t == "/source-charset"
                                   || t == "/execution-charset"
                                   || t == "/validate-charset";
                        };
                        if (cl_sep())
                        {
                            std::string value;
                            bool        have = false;
                            if (detail::cl_value_follows(tokens, j))
                            {
                                value = tokens[j + 1];
                                have = true;
                                ++j;
                            }
                            if (have)
                            {
                                if (t == "/Fp")
                                {
                                    fp_path = resolve_arg(value);
                                }
                                else if (t == "/Fo" || t == "/Fd")
                                {
                                    // 构建产物：重定向由影子构建负责。
                                }
                                else if (t == "/FI")
                                {
                                    const std::string abs = resolve_arg(value);
                                    cc.forced_includes.push_back(abs);
                                    cc.argv.push_back(t);
                                    cc.argv.push_back(abs);
                                }
                                else if (t == "/I" || t == "/external:I")
                                {
                                    cc.argv.push_back(t);
                                    cc.argv.push_back(resolve_arg(value));
                                }
                                else
                                {
                                    cc.argv.push_back(t);
                                    cc.argv.push_back(value);
                                }
                            }
                            ++j;
                            continue;
                        }
                        const auto joined =
                            [&](std::string_view prefix, std::string& out)
                        {
                            if (!t.starts_with(prefix)
                                || t.size() <= prefix.size())
                            {
                                return false;
                            }
                            out = t.substr(prefix.size());
                            return true;
                        };
                        std::string suffix;
                        if (joined("/external:I", suffix)
                            || joined("/FI", suffix))
                        {
                            const std::string abs = resolve_arg(suffix);
                            if (t.starts_with("/FI"))
                            {
                                cc.forced_includes.push_back(abs);
                            }
                            cc.argv.push_back(
                                std::format(
                                    "{}{}",
                                    t.starts_with("/FI") ? "/FI"
                                                         : "/external:I",
                                    abs));
                            ++j;
                            continue;
                        }
                        if (joined("/I", suffix))
                        {
                            cc.argv.push_back(
                                std::format("/I{}", resolve_arg(suffix)));
                            ++j;
                            continue;
                        }
                        if (joined("/Fp", suffix))
                        {
                            fp_path = resolve_arg(suffix);
                            ++j;
                            continue;
                        }
                        if (joined("/Fo", suffix) || joined("/Fd", suffix))
                        {
                            ++j;
                            continue;
                        }
                        if (joined("/Yc", suffix))
                        {
                            yc_set = true;
                            cc.pch_header = suffix;
                            cc.argv.push_back(t);
                            ++j;
                            continue;
                        }
                        if (joined("/Yu", suffix))
                        {
                            cc.pch_use = true;
                            cc.pch_header = suffix;
                            ++j;
                            continue;
                        }
                        if (t == "/Yc" || t == "/Yu")
                        {
                            // 分写值守卫（cl 头值型）：源文件扩展名绝不能被
                            // 吞（v1 的 AdditionalOptions=/Yu 把下一条 TU 吃
                            // 掉的 bug）。
                            std::string value;
                            if (detail::cl_header_value_follows(tokens, j))
                            {
                                value = tokens[j + 1];
                                ++j;
                            }
                            if (t == "/Yc")
                            {
                                yc_set = true;
                                cc.pch_header = value;
                                cc.argv.push_back(t);
                            }
                            else
                            {
                                cc.pch_use = true;
                                cc.pch_header = value;
                            }
                            ++j;
                            continue;
                        }
                        if (t == "/showIncludes" || t == "/MP" || t == "/Gm"
                            || t == "/GL" || t == "/analyze" || t == "/WX"
                            || t == "/FS")
                        {
                            ++j;
                            continue;
                        }
                        cc.argv.push_back(t);
                        ++j;
                        continue;
                    }
                    if (is_include_flag(t))
                    {
                        push_include_token(t, j);
                        ++j;
                        continue;
                    }
                    if (is_joined_include(t))
                    {
                        push_joined_include(t);
                        ++j;
                        continue;
                    }
                    if (!detail::is_gnu_flag_token(t)
                        && resolve_arg(t) == main_norm)
                    {
                        ++j;
                        continue;
                    }
                    if (t.starts_with("-Werror"))
                    {
                        ++j; // 告警不影响 AST 形状，影子构建不应假失败
                        continue;
                    }
                    cc.argv.push_back(t);
                    ++j;
                }
                if (cl_mode)
                {
                    if (yc_set)
                    {
                        cc.pch_create = true;
                        cc.pch_output = fp_path;
                    }
                    else if (!fp_path.empty())
                    {
                        cc.pch_path = fp_path;
                        cc.pch_use = true;
                    }
                }
                else
                {
                    cc.pch_output = out_flag;
                    cc.pch_create =
                        header_mode && (emit_pch || pchish(out_flag));
                }
                if (cc.pch_output.empty() && output_field.has_value())
                {
                    cc.pch_output = resolve_arg(*output_field);
                }
            }
            else
                for (std::size_t j = 0; j < tokens.size(); ++j)
                {
                    const std::string& t = tokens[j];
                    if (t == "-c")
                    {
                        if (detail::gnu_value_follows(tokens, j))
                        {
                            ++j;
                        }
                        continue;
                    }
                    if (t == "-o")
                    {
                        ++j;
                        continue;
                    }
                    if (t.starts_with("-o") && t.size() > 2)
                    {
                        continue; // 连写形式 "-opath"
                    }
                    if (t == "--precompile")
                    {
                        continue;
                    }
                    if (!detail::is_gnu_flag_token(t)
                        && resolve_arg(t) == main_norm)
                    {
                        continue; // 条目自身的输入，位置拼写
                    }
                    if (t.starts_with('@'))
                    {
                        note_rsp_token(t);
                        continue; // 驱动器响应文件（如 @…modmap）
                    }
                    if (t.starts_with("-fmodule-output="))
                    {
                        note_module_output_flag(t);
                        continue; // 构建产物，非解析输入
                    }
                    if (try_module_file_flag(t))
                    {
                        continue;
                    }
                    if (is_include_flag(t))
                    {
                        push_include_token(t, j);
                        continue;
                    }
                    if (is_joined_include(t))
                    {
                        push_joined_include(t);
                        continue;
                    }
                    cc.argv.push_back(t);
                }

            fs::path file_path{*file_field};
            if (file_path.is_relative())
            {
                file_path = fs::path{dir} / file_path;
            }

            cc.directory = cg::util::norm_path(dir);
            cc.file = cg::util::norm_path(
                file_path.lexically_normal().generic_string());

            bool keep = root_filters.empty();
            for (const std::string& filter : root_filters)
            {
                if (detail::path_under(cc.file, cg::util::norm_path(filter)))
                {
                    keep = true;
                    break;
                }
            }
            if (keep)
            {
                out.push_back(std::move(cc));
            }
        }
        return out;
    }

    /**
     * @brief 按文件查找命令：先精确规范化路径匹配，再带路径段边界的后缀
     * 匹配（以不同绝对前缀记录的数据库仍能命中）。
     *
     * @return 无匹配时为 nullptr。
     */
    [[nodiscard]]
    auto find_command(
        const std::vector<CompileCommand>& commands,
        std::string_view                   file) -> const CompileCommand*
    {
        const std::string needle = cg::util::norm_path(file);
        for (const CompileCommand& cc : commands)
        {
            if (cc.file == needle)
            {
                return &cc;
            }
        }
        for (const CompileCommand& cc : commands)
        {
            if (cc.file.size() > needle.size() && cc.file.ends_with(needle)
                && cc.file[cc.file.size() - needle.size() - 1] == '/')
            {
                return &cc;
            }
        }
        return nullptr;
    }

    /**
     * @brief 一个构建目标的命名模块 BMI 映射。
     *
     * CMake 4.4+ 在目标的 .modmap 响应文件旁写 CXXModules.json，携带权威
     * 的模块名到 BMI 路径对应（"modules" 对象）、逐模块 requires 闭包
     * （"usages"）与查找拼写（"references"）。
     *
     * @internal
     * .modmap 展开之后的第二条消费通道：为 rsp 未携带投喂的模块 TU 供给
     * feeds，并为失败分类提供 pcm 到模块名的反查。usages 是消费编译必须
     * 投喂的精确集合（契约维度：整模块再导出链下，消费者自身的
     * requires 只列直接 import，能解析的是传递闭包）。
     */
    struct BmiMap
    {
        std::vector<ModuleFeed> feeds; // 模块名 -> 绝对 bmi 路径
        /** @brief feeds 的反向视图（绝对 pcm 路径 -> 模块名）。 */
        std::unordered_map<std::string, std::string> module_of_pcm;
        /** @brief 逐模块的 P1689 requires 闭包（"usages"），即上述契约
         *  下消费编译应投喂的精确集合。 */
        std::unordered_map<std::string, std::vector<std::string>> usages;
    };

    namespace detail
    {

        /**
         * @brief 读一份 CXXModules.json。
         *
         * @internal 任何问题（文件缺失、坏 JSON、意外形状）都返回空 BmiMap：
         * 此映射是加速器，绝不是闸门。CMake 为真实目标写绝对 bmi 路径，但其
         * 合成重扫目标（@synth_N）的路径相对构建根——相对路径锚定在 JSON
         * 自身目录上，绝不相对进程 cwd 解析。
         */
        [[nodiscard]]
        auto read_bmi_map(const std::string& json_path) -> BmiMap
        {
            namespace fs = std::filesystem;
            namespace json = cg::util::json;
            BmiMap          out;
            std::error_code ec;
            if (!fs::is_regular_file(fs::path{json_path}, ec))
            {
                return out;
            }
            try
            {
                const auto text = cg::util::read_text_file(json_path);
                if (!text.has_value())
                {
                    return out;
                }
                const json::Document  doc = json::Document::parse(*text);
                const json::ValueView root = doc.root();
                if (!root.is_object())
                {
                    return out;
                }
                const auto modules = root.get("modules");
                if (!modules || !modules->is_object())
                {
                    return out;
                }
                modules->for_each_member(
                    [&](std::string_view name, const json::ValueView& entry)
                    {
                        if (name.empty() || !entry.is_object())
                        {
                            return;
                        }
                        const auto bmi = entry.get("bmi");
                        if (!bmi || !bmi->is_string())
                        {
                            return;
                        }
                        ModuleFeed feed;
                        feed.name = std::string{name};
                        fs::path pcm{std::string{*bmi->as_string()}};
                        if (pcm.is_relative())
                        {
                            pcm = fs::path{json_path}.parent_path() / pcm;
                        }
                        feed.pcm = cg::util::norm_path(
                            pcm.lexically_normal().generic_string());
                        if (feed.pcm.empty())
                        {
                            return;
                        }
                        out.module_of_pcm.emplace(feed.pcm, feed.name);
                        out.feeds.push_back(std::move(feed));
                    });
                const auto usages = root.get("usages");
                if (usages && usages->is_object())
                {
                    usages->for_each_member(
                        [&](std::string_view name, const json::ValueView& list)
                        {
                            if (name.empty() || !list.is_array())
                            {
                                return;
                            }
                            std::vector<std::string>& closure =
                                out.usages[std::string{name}];
                            for (std::size_t i = 0; i < list.size(); ++i)
                            {
                                if (const auto v = list.at(i);
                                    v && v->is_string())
                                {
                                    closure.emplace_back(*v->as_string());
                                }
                            }
                        });
                }
            }
            catch (const std::exception&)
            {
                return BmiMap{};
            }
            return out;
        }

        /**
         * @brief 逐路径缓存。
         *
         * @internal 一个目标的全部模块条目共享同一份姊妹 CXXModules.json，
         * 且 load() 在测试中可能反复运行。
         */
        auto bmi_map_cache() -> std::unordered_map<std::string, BmiMap>&
        {
            static std::unordered_map<std::string, BmiMap> cache;
            return cache;
        }

    } // namespace detail

    /**
     * @brief 加载锚定在条目 .modmap 响应文件目录上的 BMI 映射。
     *
     * 先目标自身的 CXXModules.json，再同一 CMakeFiles/ 层级下每个姊妹
     * 目标的（提供 `std` 的 `cmake_cxx_std` 工具目标位于真实目标旁）。
     * 映射中已有的名字（来自本目标或更早姊妹）跳过——绝不在以不同 pcm
     * 构建同一模块名的两个目标之间猜测，目标自身的映射胜出。无解析时
     * 返回空映射；按锚目录缓存；线程安全。
     *
     * @internal
     * 确定性顺序：合并保留每模块名首个见到的映射，而目录迭代顺序未定，
     * 故先收集再排序。同名同 pcm 是无操作；同名不同 pcm 跳过。
     */
    [[nodiscard]]
    auto bmi_map_of(const CompileCommand& cc) -> const BmiMap&
    {
        namespace fs = std::filesystem;
        static std::mutex      mutex;
        const std::scoped_lock lock{mutex};
        auto&                  cache = detail::bmi_map_cache();
        if (cc.module_map.empty())
        {
            if (const auto it = cache.find("__none__"); it != cache.end())
            {
                return it->second;
            }
            return cache.emplace("__none__", BmiMap{}).first->second;
        }
        const std::string anchor_key = "merged:"
                                       + fs::path{cc.module_map}
                                             .parent_path()
                                             .lexically_normal()
                                             .generic_string();
        if (const auto it = cache.find(anchor_key); it != cache.end())
        {
            return it->second;
        }
        BmiMap merged = detail::read_bmi_map(
            (fs::path{cc.module_map}.parent_path() / "CXXModules.json")
                .lexically_normal()
                .generic_string());
        std::error_code ec;
        const fs::path  siblings =
            fs::path{cc.module_map}.parent_path().parent_path();
        if (fs::is_directory(siblings, ec))
        {
            std::vector<fs::path> dirs;
            for (const fs::directory_entry& entry :
                 fs::directory_iterator(siblings, ec))
            {
                if (entry.is_directory(ec))
                {
                    dirs.push_back(entry.path());
                }
            }
            std::ranges::sort(dirs);
            for (const fs::path& dir : dirs)
            {
                const BmiMap other =
                    detail::read_bmi_map((dir / "CXXModules.json")
                                             .lexically_normal()
                                             .generic_string());
                for (const ModuleFeed& feed : other.feeds)
                {
                    if (merged.module_of_pcm.contains(feed.pcm))
                    {
                        continue;
                    }
                    const bool name_conflict = std::ranges::any_of(
                        merged.feeds,
                        [&](const ModuleFeed& own)
                        { return own.name == feed.name; });
                    if (name_conflict)
                    {
                        continue;
                    }
                    merged.module_of_pcm.emplace(feed.pcm, feed.name);
                    merged.feeds.push_back(feed);
                }
            }
        }
        return cache.emplace(anchor_key, std::move(merged)).first->second;
    }

    /**
     * @brief 为重放 argv 未携带投喂的模块接口单元补 -fmodule-file。
     *
     * （被剥离或生成器过期的 .modmap 响应文件。）条目自身的
     * -fmodule-output 命名它产出的 BMI，目标的 CXXModules.json 把该 pcm
     * 反查回模块名并携带其 P1689 requires 闭包——恰是消费编译需要的投喂
     * 集。只投喂磁盘上存在且**非空**的 pcm（0 字节/半截产物是失败构建的
     * 残骸，投喂它只会让消费者以同样的错误失败）。返回获得投喂的条目数
     * （0 = modmap 通道已覆盖全部，即 CMake 的常态）。
     *
     * @internal
     * 不做超额投喂：libclang 会急切校验被投喂的 pcm，而姊妹/合成目标可能
     * 携带以不同方式构建的 BMI。绝不投喂单元自己的接口 pcm；合成重扫
     * 目标（@synth_N）与从未构建的可选目标不得以指向缺失文件的映射毒化
     * 解析。
     */
    auto supply_module_feeds(std::vector<CompileCommand>& commands)
        -> std::size_t
    {
        namespace fs = std::filesystem;
        std::size_t augmented = 0;
        for (CompileCommand& cc : commands)
        {
            if (!is_module_tu(cc) || !cc.module_feeds.empty()
                || cc.module_map.empty() || cc.module_output.empty())
            {
                continue;
            }
            const BmiMap& map = bmi_map_of(cc);
            const auto    own = map.module_of_pcm.find(cc.module_output);
            if (own == map.module_of_pcm.end())
            {
                continue;
            }
            const auto req = map.usages.find(own->second);
            if (req == map.usages.end())
            {
                continue;
            }
            bool fed_any = false;
            for (const std::string& name : req->second)
            {
                const auto feed_it = std::ranges::find_if(
                    map.feeds,
                    [&](const ModuleFeed& feed) { return feed.name == name; });
                if (feed_it == map.feeds.end()
                    || feed_it->pcm == cc.module_output)
                {
                    continue; // 绝不投喂单元自己的接口
                }
                std::error_code ec;
                if (!fs::is_regular_file(fs::path{feed_it->pcm}, ec) || ec)
                {
                    continue;
                }
                const auto feed_size =
                    fs::file_size(fs::path{feed_it->pcm}, ec);
                if (ec || feed_size == 0)
                {
                    continue; // 0 字节/半截 BMI 不得被投喂（复用）
                }
                cc.argv.push_back(
                    std::format(
                        "-fmodule-file={}={}",
                        feed_it->name,
                        feed_it->pcm));
                cc.module_feeds.push_back(*feed_it);
                fed_any = true;
            }
            if (fed_any)
            {
                ++augmented;
            }
        }
        return augmented;
    }

    /**
     * @brief 单个 TU 无可用条目时的基准旗标。
     *
     * -std=c++23 加 clang 驱动器为本安装报告的 include 搜索列表
     * （cg.clang.driver 讯问所得、按报告顺序）——绝不是烤死的路径。
     * 这与目录级兜底扫描无关：`ccg index` 只接受编译数据库，本函数只
     * 服务自建管线中没有记录权威的单元（如 cl 路由的模块单元）。
     *
     * @internal
     * $CCG_FALLBACK_FLAGS（';' 分隔的裸 argv token，如 "-isystem;
     * `<vendor>`/include;-DUSE_STD_FORMAT"）给这些合成向量追加自带依赖所需
     * 的 include 根与定义——自建管线的扫描、生产与解析全都骑同一份列表。
     * 讯问不到驱动器时响亮失败：错误的静默猜测比可行动错误更糟。
     */
    [[nodiscard]]
    auto fallback_argv() -> std::vector<std::string>
    {
        const DriverInfo& info = driver_info();
        if (info.driver.empty() || info.cxx_include_dirs.empty())
        {
            throw std::runtime_error(
                "ccg: cannot build fallback compiler flags: no clang driver "
                "found or it reported no include search list (looked beside "
                "the loaded LLVM engine library and under its installation "
                "prefix, then via CCG_LLVM_ROOT, then on PATH). Set "
                "CCG_LLVM_ROOT, pass `ccg index --llvm-root <prefix>`, or "
                "configure `ccg config set llvm-path <prefix>` to name the "
                "LLVM installation containing clang++/clang.");
        }
        std::vector<std::string> argv{"-std=c++23"};
        for (const std::string& dir : info.cxx_include_dirs)
        {
            argv.emplace_back("-I");
            argv.push_back(dir);
        }
        if (const auto env = cg::util::cstd::get_env("CCG_FALLBACK_FLAGS"))
        {
            std::stringstream stream{*env};
            std::string       token;
            while (std::getline(stream, token, ';'))
            {
                if (!token.empty())
                {
                    argv.push_back(std::move(token));
                }
            }
        }
        return argv;
    }

} // namespace cg::clang
