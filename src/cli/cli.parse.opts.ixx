/**
 * @module cg.cli.parse.opts
 *
 * @brief 旗标校验域：符号种类/深度/配置 token 解析、默认输出目录、AllowedOpts
 * 与按命令的白名单。
 */
export module cg.cli.parse.opts;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.graph;
import cg.cli.parse.model;

export namespace cg::cli
{

    /** @brief --kind 词 -> SymbolKind（未知返回空）。 */
    [[nodiscard]]
    auto parse_kind(std::string_view word) -> std::optional<cg::db::SymbolKind>
    {
        using K = cg::db::SymbolKind;
        static constexpr std::pair<std::string_view, K> KINDS[] = {
            {"record", K::Record},
            {"enum", K::Enumeration},
            {"enum_constant", K::EnumConstant},
            {"function", K::Function},
            {"method", K::Method},
            {"constructor", K::Constructor},
            {"destructor", K::Destructor},
            {"field", K::Field},
            {"variable", K::Variable},
            {"type_alias", K::TypeAlias},
        };
        for (const auto& [name, kind] : KINDS)
        {
            if (name == word)
            {
                return kind;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 深度文本 -> [1, 10] 内的整数；非法返回空。
     *
     * @internal 数字累加并在超上限时提前钳制：荒谬的数字串饱和而非使
     * 有符号 int 溢出。
     */
    [[nodiscard]]
    auto parse_depth(std::string_view text) -> std::optional<int>
    {
        if (text.empty()
            || !std::ranges::all_of(
                text,
                [](char c) { return c >= '0' && c <= '9'; }))
        {
            return std::nullopt;
        }
        unsigned long long value = 0;
        for (const char c : text)
        {
            value = value * 10 + static_cast<unsigned long long>(c - '0');
            if (value > 10ull)
            {
                break;
            }
        }
        if (value > 10)
        {
            return std::nullopt;
        }
        return static_cast<int>(value);
    }

    /**
     * @brief 配置名记号会进入目录名（`<文件夹>`/`<配置>`/），故必须是单个
     *        路径安全的路径段：非空、无空白、不含 Windows 文件名禁用的
     *        字符（外加 MSBuild 中已用于分隔配对的 '|'）。
     */
    [[nodiscard]]
    auto valid_config_token(std::string_view value) -> bool
    {
        if (value.empty())
        {
            return false;
        }
        return std::ranges::none_of(
            value,
            [](char c)
            {
                return c == ' ' || c == '\t' || c == '/' || c == '\\'
                       || c == ':' || c == '*' || c == '?' || c == '"'
                       || c == '<' || c == '>' || c == '|'
                       || static_cast<unsigned char>(c) < 0x20;
            });
    }

    /** @brief 默认产出文件夹：$CCG_DB 或 ./.ccg（二者皆指文件夹）。 */
    [[nodiscard]]
    auto default_output_dir() -> std::string
    {
        if (const auto env = cg::util::cstd::get_env("CCG_DB"))
        {
            return *env;
        }
        return "./.ccg";
    }

    /**
     * @brief 每个命令接受哪些选项（各命令用法 synopsis 行的一一镜像）。
     */
    struct AllowedOpts
    {
        bool force = false;
        bool full = false;
        bool dry_run = false;
        bool paranoid = false;
        bool depth = false;
        bool kind = false;
        bool file = false;
        bool direction = false;
        bool no_merge = false;
        bool dispatch = false;
        bool top = false;          // callers/callees/impact：--top N 截断
        bool exclude_glob = false; // 结果端路径排除：--exclude-glob G
        bool implements = false;   // query：--implements 派生树模式
        bool compile_commands = false;
        bool jobs = false;
        bool llvm_root = false;
        bool clang_compile_flags = false; // index/update/msbuild：注入旗标
        bool from_file = false;
        bool cycles = false;
        bool max = false;
        bool config = false; // --config（布局配置名；msbuild 另有自己的语义）
        bool msbuild_config = false;
        bool msbuild_platform = false;
        bool msbuild_all = false;
        bool msbuild_export_only = false;
        bool msbuild_list = false;
        bool msbuild_record = false;
        bool msbuild_full_build = false;
        bool msbuild_allow_failure = false;
        bool msbuild_args = false;
        bool msbuild_cl = false;
        bool msbuild_exe = false;
        bool drain = false;
        bool wait = false;       // index/msbuild：--wait/--no-wait（租约排队）
        bool hook_scope = false; // hook：--user / --project
        bool graph_symbol = false;  // graph：--symbol
        bool graph_modules = false; // graph：--modules
        bool graph_format = false;  // graph：--format
        bool graph_output = false;  // graph：--output
        bool min_weight = false;    // graph：--min-weight
    };

    /** @brief 逐命令的选项允许集。 */
    [[nodiscard]]
    auto allowed_for(Invocation::Cmd cmd) -> AllowedOpts
    {
        switch (cmd)
        {
        case Invocation::Cmd::Index:
            return {
                .force = true,
                .full = true,
                .dry_run = true,
                .paranoid = true,
                .compile_commands = true,
                .jobs = true,
                .llvm_root = true,
                .clang_compile_flags = true,
                .config = true,
                .wait = true};
        case Invocation::Cmd::Update:
            return {
                .compile_commands = true,
                .jobs = true,
                .llvm_root = true,
                .clang_compile_flags = true,
                .config = true,
                .drain = true};
        case Invocation::Cmd::Msbuild:
            return {
                .force = true,
                .compile_commands = true,
                .jobs = true,
                .clang_compile_flags = true,
                .msbuild_config = true,
                .msbuild_platform = true,
                .msbuild_all = true,
                .msbuild_export_only = true,
                .msbuild_list = true,
                .msbuild_record = true,
                .msbuild_full_build = true,
                .msbuild_allow_failure = true,
                .msbuild_args = true,
                .msbuild_cl = true,
                .msbuild_exe = true,
                .wait = true};
        case Invocation::Cmd::Hook:
            return {.force = true, .config = true, .hook_scope = true};
        case Invocation::Cmd::Config:
            return {.config = true};
        case Invocation::Cmd::Query:
            return {
                .kind = true,
                .file = true,
                .implements = true,
                .config = true};
        case Invocation::Cmd::Members:
            return {.kind = true, .file = true, .config = true};
        case Invocation::Cmd::Trace:
            return {
                .kind = true,
                .file = true,
                .dispatch = true,
                .config = true};
        case Invocation::Cmd::Context:
            return {
                .kind = true,
                .file = true,
                .no_merge = true,
                .dispatch = true,
                .exclude_glob = true,
                .config = true};
        case Invocation::Cmd::Callers:
        case Invocation::Cmd::Callees:
            return {
                .depth = true,
                .kind = true,
                .file = true,
                .no_merge = true,
                .dispatch = true,
                .top = true,
                .exclude_glob = true,
                .config = true};
        case Invocation::Cmd::Impact:
            return {
                .depth = true,
                .kind = true,
                .file = true,
                .direction = true,
                .no_merge = true,
                .dispatch = true,
                .top = true,
                .exclude_glob = true,
                .config = true};
        case Invocation::Cmd::Changes:
            return {
                .depth = true,
                .dispatch = true,
                .from_file = true,
                .config = true};
        case Invocation::Cmd::Check:
            return {.cycles = true, .max = true, .config = true};
        case Invocation::Cmd::Stats:
            return {.config = true};
        case Invocation::Cmd::Graph:
            return {
                .depth = true,
                .kind = true,
                .file = true,
                .config = true,
                .graph_symbol = true,
                .graph_modules = true,
                .graph_format = true,
                .graph_output = true,
                .min_weight = true};
        default:
            return {};
        }
    }
} // namespace cg::cli
