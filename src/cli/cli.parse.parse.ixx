/**
 * @module cg.cli.parse.parse
 *
 * @brief argv 解析主链 parse_args：命令分派、旗标循环与取值、错误装配。
 */
export module cg.cli.parse.parse;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.graph;
import cg.cli.parse.model;
import cg.cli.parse.usage;
import cg.cli.parse.opts;

namespace
{
    /**
     * @brief 让 std::format 一族在本单元的第一次实例化落在这段常量的初始化上。
     *
     * @internal clang 23.1.5 配 libc++ 时，本单元去掉这段常量，前端就在实例化
     *           std::format 一族的格式串检查模板（basic_format_string 的构造函
     *           数与 __handles_）时访问违例，重编五次结果相同。崩的位置是
     *           std::format 一族的一次普通调用（std::format 或 std::println），
     *           各单元的实参形态互不相同（单个 const char*、std::string、整数的
     *           引用等），有的在 lambda 里。
     *
     *           同项目里另有 15 个单元同样导入 std、同样直接或间接导入 sqlpp23
     *           的模块，不带这段常量能编过。与是否向 std::cerr 打印无关（不打印
     *           的单元也崩），与本单元全局模块片段里有没有标准库头文本也无关
     *           （带 `<cstdio>` 的单元也崩）。哪些单元会触发，规律没有查清，这
     *           里只记实测到的事实。
     *
     *           这段常量抢在一切使用之前，把 std::format 一族在本单元的第一次实
     *           例化固定成一次最简单的直接使用。在这套工具链上，别的单元若出现
     *           同类崩溃（调用栈里有 basic_format_string 与 __handles_），按同
     *           样的写法加一段；升级 clang 之后可以逐个单元去掉试编，不崩就删。
     */
    [[maybe_unused]]
    inline const std::string FORMAT_WARMUP_TEXT =
        std::format("{} {} {} {} {}", "a", std::string("b"), 1, 2.5, 'c');
} // namespace

export namespace cg::cli
{

    /**
     * @brief 解析不含 argv[0] 的 argv；任何契约违例抛 UsageError。
     *
     * @internal
     * 操作数边界：trace 恰两个符号（先查——trace 也接受符号）、index 零或
     * 多个根（零 = 档案复跑，运行期无档案时再报可行动错误）、query 类恰一
     * 个、changes 零或多个文件（零 = 管道时读 stdin）、stats 与 check 无。
     * "--opt=value" 拆分使两种拼写统一生效。数字选项（--max、--jobs）同样
     * 数字累加 + 超上限提前钳制（荒谬数字串饱和而非溢出）。操作数计数逐命
     * 令校验。ccg check 需要其模式旗标，在解析期校验使用法错误胜过后续任何
     * 运行时错误（如库缺失）。ccg update 无文件又无 --drain 时无可刷新、无
     * 队列可收敛——解析期拒绝并给出 drain 提示。ccg index 的 --config 永不
     * 可省、-o 只许文件夹（文件路径解析期响亮报错）。
     * ccg graph 双模式互斥（--symbol 与 --modules 恰其一），--depth 只属
     * 入口展开（默认 2：调研实证深度 3 的入口图过大）、--min-weight 只属
     * 模块聚合，错配在解析期拒绝。
     * ccg msbuild 组合规则：--list 是纯注册表读；--all
     * 扇出到每个声明配对；两者都拒绝会点名或引导单次构建的选项；操作数仅
     * --list 可选（此时以 cwd 为注册表根）。逐命令深度默认：impact 与
     * changes 默认比直接邻域走得更远（两者默认深度 3；callers/callees
     * 直接邻域为 1）。
     */
    [[nodiscard]]
    auto parse_args(std::span<const std::string> args) -> Invocation
    {
        if (args.empty())
        {
            throw UsageError{"missing command", top_usage()};
        }

        Invocation inv;
        inv.output_dir = default_output_dir();

        const std::string& first = args.front();
        if (first == "--version")
        {
            inv.cmd = Invocation::Cmd::Version;
            return inv;
        }
        if (first == "help")
        {
            inv.cmd = Invocation::Cmd::Help;
            if (args.size() > 2)
            {
                throw UsageError{
                    "help takes at most one command name",
                    top_usage()};
            }
            if (args.size() == 2)
            {
                if (!command_of(args[1]).has_value())
                {
                    throw UsageError{
                        std::format("unknown command '{}'", args[1]),
                        top_usage()};
                }
                inv.positional.push_back(args[1]);
            }
            return inv;
        }
        if (first == "--help" || first == "-h")
        {
            if (args.size() > 1)
            {
                throw UsageError{"--help takes no arguments", top_usage()};
            }
            inv.cmd = Invocation::Cmd::Help;
            return inv;
        }
        const auto cmd = command_of(first);
        if (!cmd.has_value())
        {
            if (first.starts_with('-'))
            {
                throw UsageError{
                    "unknown option '" + first
                        + "' (a command must come first)",
                    top_usage()};
            }
            throw UsageError{
                std::format("unknown command '{}'", first),
                top_usage()};
        }
        inv.cmd = *cmd;

        const AllowedOpts      allowed = allowed_for(*cmd);
        const std::string_view usage = usage_for(*cmd);
        bool                   depth_seen = false;

        const bool wants_symbol = *cmd == Invocation::Cmd::Query
                                  || *cmd == Invocation::Cmd::Context
                                  || *cmd == Invocation::Cmd::Members
                                  || *cmd == Invocation::Cmd::Callers
                                  || *cmd == Invocation::Cmd::Callees
                                  || *cmd == Invocation::Cmd::Impact
                                  || *cmd == Invocation::Cmd::Trace;

        const std::size_t min_operands =
            *cmd == Invocation::Cmd::Trace ? 2
            : (wants_symbol || *cmd == Invocation::Cmd::Config
               || *cmd == Invocation::Cmd::Hook)
                ? 1
                : 0; // index 根可缺（档案复跑）
        const std::size_t max_operands =
            (*cmd == Invocation::Cmd::Index || *cmd == Invocation::Cmd::Changes
             || *cmd == Invocation::Cmd::Update)
                ? 1000
                : (*cmd == Invocation::Cmd::Msbuild
                       ? 1
                       : (*cmd == Invocation::Cmd::Config
                              ? 3
                              : (*cmd == Invocation::Cmd::Hook
                                     ? 1
                                     : min_operands)));

        bool saw_no_dispatch = false;
        bool saw_dispatch_fallback = false;
        // `ccg config` 的值操作数可以以 '-' 开头（clang-compile-flags 的
        // 旗标文本，如 "-DFOO -isystem `<dir>`"，从真实 shell 引号喂入仍是
        // 一个 '-' 前缀 token）。config 的选项面极小（-o/--config/--json/
        // -v/-h），不在其列的 '-' 前缀 token 按操作数对待——否则该键的
        // 值永远到不了运行器。
        const bool dash_value_is_operand = *cmd == Invocation::Cmd::Config;
        const auto config_known_option = [](std::string_view name)
        {
            constexpr std::string_view known[] =
                {"-o", "--config", "--json", "-v", "--verbose", "-h", "--help"};
            for (const std::string_view k : known)
            {
                if (name == k)
                {
                    return true;
                }
            }
            return false;
        };
        for (std::size_t i = 1; i < args.size(); ++i)
        {
            const std::string& token = args[i];
            if (!token.starts_with('-') || token == "-")
            {
                inv.positional.push_back(token);
                continue;
            }
            if (dash_value_is_operand)
            {
                const std::size_t      eq = token.find('=');
                const std::string_view probe =
                    std::string_view{token}.substr(0, eq);
                if (!config_known_option(
                        eq == std::string::npos ? std::string_view{token}
                                                : probe))
                {
                    inv.positional.push_back(token);
                    continue;
                }
            }

            std::string name = token;
            std::string inline_value;
            bool        has_inline = false;
            if (const std::size_t eq = token.find('='); eq != std::string::npos)
            {
                name = token.substr(0, eq);
                inline_value = token.substr(eq + 1);
                has_inline = true;
            }
            const auto take_value = [&]() -> std::string
            {
                if (has_inline)
                {
                    return inline_value;
                }
                if (i + 1 >= args.size())
                {
                    throw UsageError{
                        std::format("option '{}' needs a value", name),
                        usage};
                }
                return args[++i];
            };
            const auto reject_for_command = [&]()
            {
                throw UsageError{
                    std::format(
                        "option '{}' is not valid for command '{}'",
                        name,
                        name_of(inv.cmd)),
                    usage};
            };

            if (name == "-h" || name == "--help")
            {
                inv.cmd = Invocation::Cmd::Help;
                inv.positional.clear();
                inv.positional.emplace_back(name_of(*cmd));
                return inv;
            }
            if (name == "-o")
            {
                const std::string v = take_value();
                // -o 只许产出文件夹；文件路径响亮报错。
                std::error_code             fec;
                const std::filesystem::path probe{v};
                if (v.ends_with(".db") || v == ":memory:"
                    || (std::filesystem::is_regular_file(probe, fec) && !fec))
                {
                    throw UsageError{
                        "-o takes the output FOLDER (default ./.ccg): "
                        "configurations live inside it as <config>/ccg.db - you "
                        "passed the file path '"
                            + v
                            + "'; use `-o <folder> --config <name>` (query side: "
                              "`-o <folder>` auto-selects the single configuration)",
                        usage};
                }
                inv.output_dir = v;
                inv.db_explicit = true;
            }
            else if (name == "--json")
            {
                inv.json = true;
            }
            else if (name == "-v" || name == "--verbose")
            {
                inv.verbose = true;
            }
            else if (name == "--force")
            {
                if (!allowed.force)
                {
                    reject_for_command();
                }
                inv.force = true;
            }
            else if (name == "--full")
            {
                if (!allowed.full)
                {
                    reject_for_command();
                }
                inv.full = true;
            }
            else if (name == "--dry-run")
            {
                if (!allowed.dry_run)
                {
                    reject_for_command();
                }
                inv.dry_run = true;
            }
            else if (name == "--paranoid")
            {
                if (!allowed.paranoid)
                {
                    reject_for_command();
                }
                inv.paranoid = true;
            }
            else if (name == "--depth")
            {
                if (!allowed.depth)
                {
                    reject_for_command();
                }
                const auto d = parse_depth(take_value());
                const bool graph_unlimited =
                    *cmd == Invocation::Cmd::Graph && d.has_value() && *d == 0;
                if (!d.has_value() || (*d == 0 && !graph_unlimited))
                {
                    throw UsageError{
                        *cmd == Invocation::Cmd::Graph
                            ? "--depth must be 0 (unlimited) or an integer "
                              "between 1 and 10"
                            : "--depth must be an integer between 1 and 10",
                        usage};
                }
                inv.depth = *d;
                depth_seen = true;
            }
            else if (name == "--kind")
            {
                if (!allowed.kind)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                const auto        k = parse_kind(v);
                if (!k.has_value())
                {
                    throw UsageError{
                        "unknown kind '" + v
                            + "' (record function method constructor "
                              "destructor field variable enum "
                              "enum_constant type_alias)",
                        usage};
                }
                inv.kind = k;
            }
            else if (name == "--file")
            {
                if (!allowed.file)
                {
                    reject_for_command();
                }
                inv.file_substring = take_value();
            }
            else if (name == "--top")
            {
                if (!allowed.top)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty()
                    || !std::ranges::all_of(
                        v,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw UsageError{
                        "--top must be an integer between 1 and "
                        "100000",
                        usage};
                }
                unsigned long long value = 0;
                for (const char c : v)
                {
                    value =
                        value * 10 + static_cast<unsigned long long>(c - '0');
                    if (value > 100000ull)
                    {
                        break;
                    }
                }
                if (value < 1 || value > 100000)
                {
                    throw UsageError{
                        "--top must be an integer between 1 and "
                        "100000",
                        usage};
                }
                inv.top = static_cast<int>(value);
            }
            else if (name == "--exclude-glob")
            {
                if (!allowed.exclude_glob)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{
                        "--exclude-glob needs a glob pattern "
                        "(e.g. build/** or _deps/*; '**' crosses "
                        "'/', '*' stays within one segment)",
                        usage};
                }
                inv.exclude_globs.push_back(v);
            }
            else if (name == "--direction")
            {
                if (!allowed.direction)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v == "up")
                {
                    inv.direction = cg::graph::Direction::Up;
                }
                else if (v == "down")
                {
                    inv.direction = cg::graph::Direction::Down;
                }
                else
                {
                    throw UsageError{"--direction must be up or down", usage};
                }
            }
            else if (name == "--no-merge")
            {
                if (!allowed.no_merge)
                {
                    reject_for_command();
                }
                inv.no_merge = true;
            }
            else if (name == "--implements")
            {
                if (!allowed.implements)
                {
                    reject_for_command();
                }
                inv.implements = true;
            }
            else if (name == "--no-dispatch")
            {
                if (!allowed.dispatch)
                {
                    reject_for_command();
                }
                saw_no_dispatch = true;
                inv.dispatch_mode = cg::graph::DispatchMode::Off;
            }
            else if (name == "--dispatch-fallback")
            {
                if (!allowed.dispatch)
                {
                    reject_for_command();
                }
                saw_dispatch_fallback = true;
                inv.dispatch_mode = cg::graph::DispatchMode::Fallback;
            }
            else if (name == "--from-file")
            {
                if (!allowed.from_file)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{
                        "--from-file needs a path ('-' reads stdin)",
                        usage};
                }
                inv.from_file = v;
            }
            else if (name == "--cycles")
            {
                if (!allowed.cycles)
                {
                    reject_for_command();
                }
                inv.cycles = true;
            }
            else if (name == "--max")
            {
                if (!allowed.max)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty()
                    || !std::ranges::all_of(
                        v,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw UsageError{
                        "--max must be an integer between 1 and 1000",
                        usage};
                }
                unsigned long long value = 0;
                for (const char c : v)
                {
                    value =
                        value * 10 + static_cast<unsigned long long>(c - '0');
                    if (value > 1000ull)
                    {
                        break;
                    }
                }
                if (value < 1 || value > 1000)
                {
                    throw UsageError{
                        "--max must be an integer between 1 and 1000",
                        usage};
                }
                inv.list_cap = static_cast<int>(value);
            }
            else if (name == "--compile-commands")
            {
                if (!allowed.compile_commands)
                {
                    reject_for_command();
                }
                inv.compile_db = take_value();
            }
            else if (name == "--config")
            {
                const std::string v = take_value();
                if (!valid_config_token(v))
                {
                    throw UsageError{
                        "--config must be a path-safe name (no whitespace or "
                        "'/\\:*?\"<>|' - it becomes a configuration DIRECTORY "
                        "name under the -o folder)",
                        usage};
                }
                if (allowed.config)
                {
                    // 布局配置名：index 必给，查询侧可选。
                    inv.config_name = v;
                    inv.config_explicit = true;
                }
                else if (allowed.msbuild_config)
                {
                    inv.msbuild_config = v;
                }
                else
                {
                    reject_for_command();
                }
            }
            else if (name == "--platform")
            {
                if (!allowed.msbuild_platform)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (!valid_config_token(v))
                {
                    throw UsageError{
                        "--platform must be a path-safe name (no whitespace or "
                        "'/\\:*?\"<>|' - it becomes part of the db file name)",
                        usage};
                }
                inv.msbuild_platform = v;
            }
            else if (name == "--all")
            {
                if (!allowed.msbuild_all)
                {
                    reject_for_command();
                }
                inv.msbuild_all = true;
            }
            else if (name == "--export-only")
            {
                if (!allowed.msbuild_export_only)
                {
                    reject_for_command();
                }
                inv.msbuild_export_only = true;
            }
            else if (name == "--list")
            {
                if (!allowed.msbuild_list)
                {
                    reject_for_command();
                }
                inv.msbuild_list = true;
            }
            else if (name == "--record")
            {
                if (!allowed.msbuild_record)
                {
                    reject_for_command();
                }
                inv.msbuild_record = true;
            }
            else if (name == "--full-build")
            {
                if (!allowed.msbuild_full_build)
                {
                    reject_for_command();
                }
                inv.msbuild_full_build = true;
            }
            else if (name == "--allow-failure")
            {
                if (!allowed.msbuild_allow_failure)
                {
                    reject_for_command();
                }
                inv.msbuild_allow_failure = true;
            }
            else if (name == "--msbuild-args")
            {
                if (!allowed.msbuild_args)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--msbuild-args needs a value", usage};
                }
                inv.msbuild_args = v;
            }
            else if (name == "--cl")
            {
                if (!allowed.msbuild_cl)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--cl needs a path", usage};
                }
                inv.cl_path = v;
            }
            else if (name == "--msbuild")
            {
                if (!allowed.msbuild_exe)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--msbuild needs a path", usage};
                }
                inv.msbuild_path = v;
            }
            else if (name == "--llvm-root")
            {
                if (!allowed.llvm_root)
                {
                    reject_for_command();
                }
                inv.llvm_root = take_value();
            }
            else if (name == "--clang-compile-flags")
            {
                if (!allowed.clang_compile_flags)
                {
                    reject_for_command();
                }
                const std::string              v = take_value();
                const std::vector<std::string> tokens =
                    cg::util::tokenize_command(v);
                if (tokens.empty())
                {
                    throw UsageError{
                        "--clang-compile-flags needs flags (e.g. "
                        "\"-DFOO -isystem <vendor>/include\"; repeatable, "
                        "later occurrences override earlier ones)",
                        usage};
                }
                inv.clang_compile_flags.insert(
                    inv.clang_compile_flags.end(),
                    tokens.begin(),
                    tokens.end());
            }
            else if (name == "--drain")
            {
                if (!allowed.drain)
                {
                    reject_for_command();
                }
                inv.drain = true;
            }
            else if (name == "--symbol")
            {
                if (!allowed.graph_symbol)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--symbol needs a symbol name", usage};
                }
                inv.graph_symbol = v;
            }
            else if (name == "--modules")
            {
                if (!allowed.graph_modules)
                {
                    reject_for_command();
                }
                inv.graph_modules = true;
            }
            else if (name == "--format")
            {
                if (!allowed.graph_format)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v != "dot" && v != "json")
                {
                    throw UsageError{"--format must be dot or json", usage};
                }
                inv.graph_format = v;
            }
            else if (name == "--output")
            {
                if (!allowed.graph_output)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--output needs a path", usage};
                }
                inv.output_file = v;
            }
            else if (name == "--min-weight")
            {
                if (!allowed.min_weight)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty()
                    || !std::ranges::all_of(
                        v,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw UsageError{
                        "--min-weight must be an integer between 1 "
                        "and 100000",
                        usage};
                }
                unsigned long long value = 0;
                for (const char c : v)
                {
                    value =
                        value * 10 + static_cast<unsigned long long>(c - '0');
                    if (value > 100000ull)
                    {
                        break;
                    }
                }
                if (value < 1 || value > 100000)
                {
                    throw UsageError{
                        "--min-weight must be an integer between 1 "
                        "and 100000",
                        usage};
                }
                inv.min_weight = static_cast<int>(value);
            }
            else if (name == "--user")
            {
                if (!allowed.hook_scope)
                {
                    reject_for_command();
                }
                inv.hook_user = true;
            }
            else if (name == "--project")
            {
                if (!allowed.hook_scope)
                {
                    reject_for_command();
                }
                inv.hook_project = true;
            }
            else if (name == "--jobs")
            {
                if (!allowed.jobs)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty()
                    || !std::ranges::all_of(
                        v,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw UsageError{
                        "--jobs must be an integer between 1 and 64",
                        usage};
                }
                unsigned long long value = 0;
                for (const char c : v)
                {
                    value =
                        value * 10 + static_cast<unsigned long long>(c - '0');
                    if (value > 64ull)
                    {
                        break;
                    }
                }
                if (value < 1 || value > 64)
                {
                    throw UsageError{
                        "--jobs must be an integer between 1 and 64",
                        usage};
                }
                inv.jobs = static_cast<int>(value);
            }
            else if (name == "--wait")
            {
                if (!allowed.wait)
                {
                    reject_for_command();
                }
                // 排队等待默认开：--wait 本身不动默认上界，收紧上界走
                // --wait=`<ms>` 内联形式（空格形式会与 index 的根目录操作数
                // 歧义，故不支持）。
                if (inv.no_wait)
                {
                    throw UsageError{
                        "--wait and --no-wait are mutually "
                        "exclusive (the run either queues behind a "
                        "live update session or fails fast)",
                        usage};
                }
                if (has_inline)
                {
                    if (inline_value.empty()
                        || !std::ranges::all_of(
                            inline_value,
                            [](char c) { return c >= '0' && c <= '9'; }))
                    {
                        throw UsageError{
                            "--wait=<ms> must be milliseconds (e.g. --wait=5000)",
                            usage};
                    }
                    unsigned long long value = 0;
                    for (const char c : inline_value)
                    {
                        value = value * 10
                                + static_cast<unsigned long long>(c - '0');
                        if (value > 86'400'000ull)
                        { // 24 h：荒谬上界即钳制
                            break;
                        }
                    }
                    if (value < 1 || value > 86'400'000ull)
                    {
                        throw UsageError{
                            "--wait=<ms> must be between 1 ms and 24 h",
                            usage};
                    }
                    inv.wait_limit_ms = static_cast<std::int64_t>(value);
                    inv.wait_cap_seen = true;
                }
            }
            else if (name == "--no-wait")
            {
                if (!allowed.wait)
                {
                    reject_for_command();
                }
                if (inv.wait_cap_seen)
                {
                    throw UsageError{
                        "--wait and --no-wait are mutually "
                        "exclusive (the run either queues behind a "
                        "live update session or fails fast)",
                        usage};
                }
                inv.no_wait = true;
            }
            else
            {
                throw UsageError{
                    std::format("unknown option '{}'", name),
                    usage};
            }
        }

        if (inv.positional.size() < min_operands)
        {
            throw UsageError{
                *cmd == Invocation::Cmd::Trace
                    ? std::string{"trace needs two symbol names"}
                    : (*cmd == Invocation::Cmd::Index
                           ? std::string{"missing project root directory"}
                           : (*cmd == Invocation::Cmd::Config
                                  ? std::string{"config needs an action: get, "
                                                "set, or unset"}
                                  : (*cmd == Invocation::Cmd::Hook
                                         ? std::
                                               string{"hook needs an action: install, "
                                                      "uninstall, or status"}
                                         : std::
                                               string{"missing symbol name"}))),
                usage};
        }
        if (inv.positional.size() > max_operands)
        {
            throw UsageError{
                std::format("unexpected argument '{}'", inv.positional.back()),
                usage};
        }

        if (*cmd == Invocation::Cmd::Check && !inv.cycles)
        {
            throw UsageError{
                "check needs a mode; the only one today is --cycles",
                usage};
        }

        if (*cmd == Invocation::Cmd::Update && inv.positional.empty()
            && !inv.drain)
        {
            throw UsageError{
                "update needs file arguments or --drain (a bare queue drain is "
                "`ccg update --drain`)",
                usage};
        }

        if (*cmd == Invocation::Cmd::Index && !inv.config_explicit)
        {
            throw UsageError{
                "index requires --config <name> (it names the configuration "
                "directory <folder>/<name>/ that holds ccg.db, the BMI cache "
                "and the recorded profile; never omitted)",
                usage};
        }

        if (*cmd == Invocation::Cmd::Msbuild)
        {
            if (inv.msbuild_list)
            {
                if (inv.msbuild_all || inv.msbuild_export_only
                    || inv.msbuild_record || inv.msbuild_full_build
                    || inv.msbuild_allow_failure || !inv.msbuild_config.empty()
                    || !inv.msbuild_platform.empty()
                    || !inv.msbuild_args.empty() || !inv.cl_path.empty()
                    || !inv.msbuild_path.empty() || inv.force || inv.db_explicit
                    || !inv.compile_db.empty())
                {
                    throw UsageError{
                        "--list only prints the registry; build "
                        "options do not apply to it",
                        usage};
                }
            }
            else
            {
                if (inv.positional.empty())
                {
                    throw UsageError{
                        "msbuild needs a solution file (.sln) "
                        "(or --list)",
                        usage};
                }
                if (inv.msbuild_record && inv.msbuild_full_build)
                {
                    throw UsageError{
                        "--record (wrapped-build capture) and --full-build "
                        "(real compilation) are different capture channels; "
                        "the default with neither flag is project "
                        "evaluation",
                        usage};
                }
                if (inv.msbuild_allow_failure && !inv.msbuild_record
                    && !inv.msbuild_full_build)
                {
                    throw UsageError{
                        "--allow-failure tolerates a non-zero MSBuild exit "
                        "code; the default evaluation channel runs no "
                        "MSBuild build (combine it with --record or "
                        "--full-build)",
                        usage};
                }
                if (inv.msbuild_all
                    && (!inv.msbuild_config.empty()
                        || !inv.msbuild_platform.empty()))
                {
                    throw UsageError{
                        "--all builds every declared "
                        "Configuration|Platform; --config/--platform "
                        "select exactly one",
                        usage};
                }
                if (inv.msbuild_all && !inv.compile_db.empty())
                {
                    throw UsageError{
                        "--all writes one compile_commands.json per "
                        "configuration; --compile-commands cannot "
                        "name them all",
                        usage};
                }
            }
        }

        if (*cmd == Invocation::Cmd::Graph)
        {
            const bool symbol_mode = !inv.graph_symbol.empty();
            if (symbol_mode == inv.graph_modules)
            {
                throw UsageError{
                    "graph needs exactly one mode: --symbol <sym> or --modules",
                    usage};
            }
            if (!symbol_mode && depth_seen)
            {
                throw UsageError{
                    "--depth expands an entry symbol; it does not "
                    "apply to --modules",
                    usage};
            }
            if (symbol_mode && inv.min_weight != 1)
            {
                throw UsageError{
                    "--min-weight filters module aggregate edges; "
                    "it does not apply to --symbol",
                    usage};
            }
            if (!depth_seen)
            {
                inv.depth = 2;
            }
        }

        if ((*cmd == Invocation::Cmd::Impact
             || *cmd == Invocation::Cmd::Changes)
            && !depth_seen)
        {
            inv.depth = 3;
        }
        if (saw_no_dispatch && saw_dispatch_fallback)
        {
            throw UsageError{
                "--no-dispatch (pure static view) and --dispatch-fallback "
                "(precise + 0.0 markers) are mutually exclusive",
                usage};
        }
        return inv;
    }
} // namespace cg::cli
