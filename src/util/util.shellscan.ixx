/**
 * @module cg.util.shellscan
 *
 * @brief shell 命令的"字符串查找意图"提取层（PreToolUse 四工具监听）：
 *        Bash 命令经 tree-sitter 官方 Bash 语法
 *        解析后识别其中的 rg 调用并产出结构化查找请求；PowerShell
 *        命令经进程内 C# 对象宿主（cg.util.cshost）产出同构请求；Search
 *        工具载荷按候选字段直读。三方共用本模块定义的请求结构。
 *
 * 纪律硬线（宁漏勿猜）：任何动态成分（$ 展开、命令替换、变量引用）、
 * 未知旗标、反选语义、控制字符、超长模式一律放弃并携带原因；解析层
 * 是纯内存操作，除 PowerShell 桥接经引擎宿主外零输入输出。
 *
 * @internal
 * Bash 侧按 tree-sitter 路线：词法/
 * 切分/识别三层由语法树天然承担（heredoc 正文不构成 command 节点、
 * 注释内引号天然免疫、残缺输入产 ERROR 节点），rg 的旗标表解析与
 * 宁漏勿猜判定仍自守（用例矩阵 62 条的口径）。PowerShell 侧只做
 * 协议解析（shim 产 JSON），引擎路由与降级链全部在 cg.util.cshost。
 */

export module cg.util.shellscan;

import std;
import cg.util.treesit;
import cg.util.json;
import cg.util.cshost;

export namespace cg::util::shellscan
{

    /** @brief 查找引擎来源（请求溯源，供诊断与日志）。 */
    enum class Engine : std::uint8_t
    {
        Rg,           ///< Bash 命令中的 ripgrep 调用
        SelectString, ///< PowerShell 的 Select-String / sls 调用
        SearchField,  ///< Search 工具载荷的字段直读
    };

    /** @brief 路径提示形态：无 / 读上游管道（路径语义化为 stdin）/ cd 之后。 */
    enum class PathKind : std::uint8_t
    {
        None,
        StdinPipe,
        AfterCd,
    };

    /**
     * @brief 单条结构化查找请求（结构化查找请求的 C++ 形态）。
     *
     * patterns 允许多值（rg -e a -e b、-Pattern a,b）；single 标记引擎侧
     * 的"恰好一个查找意图"（提取层按 pattern 数量派生）。下游归一化闸门
     * 对非 fixed 的多 pattern 请求按交替分支同款护栏消费（OR 语义与顶层
     * 交替同构的"多词并查"）；引擎显式写 single=false 的单 pattern 请求
     * 仍按多意图拒——那是引擎对意图数的显式否决，不是数量派生产物。
     */
    struct ShellRequest
    {
        Engine                   engine = Engine::Rg;
        std::vector<std::string> patterns;
        bool                     single = true;
        bool fixed = false; ///< rg -F / Select-String -SimpleMatch
        std::vector<std::string> paths;
        std::vector<std::string> globs; ///< 仅 rg -g 收集，下游暂不消费
        std::vector<std::string> types; ///< 仅 rg -t 收集，下游暂不消费
        PathKind                 path_kind = PathKind::None;
        std::string              cwd_hint; ///< 仅 AfterCd 且 cd 目标静态时有值
    };

    /** @brief 一次提取的完整产出：请求列表加至多一个整体放弃原因。 */
    struct ShellExtractOutcome
    {
        std::vector<ShellRequest> requests;
        std::string               skip; ///< 首个识别为查找但被硬线拒绝的原因
        std::string skip_all;           ///< 整串放弃原因（伪 rg/别名遮蔽语境）
    };

    /**
     * @brief Bash 命令的加载前触发词预筛（微秒级成本闸门）。
     *
     * 字节级大小写不敏感扫描：rg 与 sls 要求词边界（前后字节不是
     * 字母/数字/下划线/连字符），select-string 按子串命中（含连字符
     * 本身即天然边界）。预筛只挡成本不判语义——假命中最多白付一次
     * tree-sitter 解析，假漏只会安静跳过，两个方向都安全。
     */
    [[nodiscard]]
    auto bash_mentions_search(std::string_view command) -> bool;

    /**
     * @brief 从一条 Bash 命令文本提取全部 rg 查找请求。
     *
     * 内嵌的 powershell/pwsh -Command 静态串会递归进 PowerShell 引擎
     * 宿主（引擎不可用时该单元静默跳过）。输入与输出均按字节处理
     * （UTF-8 多字节序列不含 ASCII 引号/元字符字节）。
     */
    [[nodiscard]]
    auto extract_bash_queries(std::string_view command) -> ShellExtractOutcome;

    /**
     * @brief 从一条 PowerShell 命令文本提取全部 Select-String 查找请求。
     *
     * 解析由进程内真引擎承担（引擎级正确性，与真实 PowerShell 一致）；
     * 引擎不可用或加载前预筛未命中时返回空请求（静默方向）。
     */
    [[nodiscard]]
    auto extract_ps_queries(std::string_view command) -> ShellExtractOutcome;

    /**
     * @brief Search 工具载荷的字段直读适配。
     *
     * Search 工具未在本环境注册（三渠道核实），按官方契约防御式
     * 实现：依次尝试 query/pattern/q 三个候选字段名，取第一个存在的
     * 字符串值作为查找文本；全部缺失或非字符串时静默放弃。可选的
     * path 字段（与 Grep 工具同形）作为单条路径提示带上。
     */
    [[nodiscard]]
    auto search_tool_queries(const json::ValueView& tool_input)
        -> ShellExtractOutcome;

} // namespace cg::util::shellscan

// ---------------------------------------------------------------------------
// 内部实现：Bash 侧 tree-sitter 遍历与 rg 参数解析。
// ---------------------------------------------------------------------------
namespace cg::util::shellscan
{

    namespace
    {

        namespace ts = cg::util::treesit;

        /// word 级动态成分节点（展开使词义运行期才可知）。
        constexpr std::string_view WORD_DYNAMIC[] = {
            "simple_expansion",
            "command_substitution",
            "process_substitution",
            "string_expansion",
            "expansion",
            "var_assignment",
        };

        /// string 内嵌动态成分节点（比 word 级少 var_assignment）。
        constexpr std::string_view STRING_DYNAMIC[] = {
            "simple_expansion",
            "command_substitution",
            "expansion",
            "string_expansion",
            "process_substitution",
        };

        /// command 子节点里不构成参数的形态（重定向/注释/环境变量前缀）。
        constexpr std::string_view NON_ARGUMENT[] = {
            "file_redirect",
            "comment",
            "heredoc_redirect",
            "variable_assignment",
        };

        /// Bash 里可反调 PowerShell 的宿主命令名。
        constexpr std::string_view PS_BRIDGE_HOSTS[] = {
            "powershell",
            "pwsh",
            "powershell.exe",
            "pwsh.exe",
            "powershell_ise.exe",
        };

        /// 反调旗标（带静态命令串参数）。
        constexpr std::string_view PS_BRIDGE_FLAGS[] = {
            "-command",
            "-c",
            "--command"};

        /// rg 15.x 无值短选项（u 可堆叠：-uu）。
        constexpr std::string_view RG_FLAG_SHORT =
            "icslwsSnhHIFvVLoqzaZUpPuU0m-";

        /// rg 15.x 带值短选项。
        constexpr std::string_view RG_VALUE_SHORT = "efgtTABCMjE";

        /// rg 15.x 无值长选项。
        constexpr std::string_view RG_FLAG_LONG[] = {
            "count",
            "files-with-matches",
            "files-without-match",
            "word-regexp",
            "ignore-case",
            "case-sensitive",
            "smart-case",
            "fixed-strings",
            "no-heading",
            "heading",
            "line-number",
            "no-line-number",
            "column",
            "with-filename",
            "no-filename",
            "null",
            "text",
            "hidden",
            "no-hidden",
            "no-ignore",
            "no-ignore-vcs",
            "no-ignore-parent",
            "no-ignore-global",
            "invert-match",
            "only-matching",
            "quiet",
            "stats",
            "mmap",
            "no-mmap",
            "follow",
            "no-follow",
            "multiline",
            "multiline-dotall",
            "pcre2",
            "json",
            "pretty",
            "crlf",
            "no-unicode",
            "unicode",
            "byte-offset",
            "passthru",
            "replace",
            "trim",
            "block-buffered",
            "line-buffered",
            "sortr",
            "messages",
            "no-messages",
            "binary",
            "no-require-git",
            "require-git",
            "auto-hybrid-regex",
            "auto-regex",
            "no-pcre2",
        };

        /// rg 15.x 带值长选项。
        constexpr std::string_view RG_VALUE_LONG[] = {
            "regexp",
            "file",
            "glob",
            "type",
            "type-not",
            "type-list",
            "context",
            "before-context",
            "after-context",
            "max-count",
            "max-columns-preview",
            "max-columns",
            "max-depth",
            "max-filesize",
            "threads",
            "encoding",
            "engine",
            "pre",
            "pre-glob",
            "sort",
            "colors",
            "color",
            "separator",
            "field-context-separator",
            "hostname-bin",
            "username-bin",
            "dfa-size-limit",
            "regex-size-limit",
            "query-word",
            "queries",
            "seeders",
        };

        /// 打印即退出的元选项（无查找发生）。
        constexpr std::string_view RG_META_NOOP[] = {"version", "help"};

        /// 无 pattern 的列文件模式。
        constexpr std::string_view RG_FILES_MODE[] = {"files", "type-list"};

        template <std::size_t N>
        [[nodiscard]]
        auto in_table(
            std::string_view needle,
            const std::string_view (&table)[N]) -> bool
        {
            return std::ranges::contains(table, needle);
        }

        [[nodiscard]]
        auto in_short_set(char c, std::string_view set) -> bool
        {
            return set.find(c) != std::string_view::npos;
        }

        /// 是否存在使谓词为真的直接子节点（门面遍历助手的短路形态，本模块
        /// 内部使用；含匿名子节点，与树结构取证口径一致）。
        template <typename Fn>
        [[nodiscard]]
        auto any_child(const ts::Node& node, Fn&& fn) -> bool
        {
            const std::uint32_t count = node.child_count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (fn(node.child(i)))
                {
                    return true;
                }
            }
            return false;
        }

        /// ASCII 小写化（多字节 UTF-8 序列的首字节高位为 1，不会命中 a-z）。
        [[nodiscard]]
        auto ascii_lower(std::string_view text) -> std::string
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text)
            {
                out.push_back(
                    static_cast<char>(
                        c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
            }
            return out;
        }

        /**
         * @brief 节点的静态文本值（去引号）；含动态成分时返回 nullopt。
         *
         * raw_string/string 去首尾引号；word 与
         * number 全文；拼接（concatenation/string_array）递归；其余类型视为
         * 动态。ansii_c_string 的前缀是 $' 两字符（[1:-1] 切法会残留 $，
         * 故取 [2:-1]），且不做 ANSI-C 转义解释——保留原文
         * 让下游标识符链判定自行拒绝，方向仍是宁漏勿猜。
         */
        [[nodiscard]]
        auto cooked_text(const ts::Node& node, std::string_view source)
            -> std::optional<std::string>
        {
            const std::string_view type = node.type();
            const std::string_view text = node.text(source);
            if (type == "raw_string")
            {
                if (text.size() < 2)
                {
                    return std::string{};
                }
                return std::string{text.substr(1, text.size() - 2)};
            }
            if (type == "string" || type == "ansii_c_string")
            {
                const std::size_t prefix = type == "ansii_c_string" ? 2 : 1;
                const bool        dynamic = any_child(
                    node,
                    [](const ts::Node& child)
                    { return in_table(child.type(), STRING_DYNAMIC); });
                if (dynamic)
                {
                    return std::nullopt;
                }
                if (text.size() < prefix + 1)
                {
                    return std::string{};
                }
                return std::string{
                    text.substr(prefix, text.size() - prefix - 1)};
            }
            if (type == "concatenation" || type == "string_array")
            {
                std::string joined;
                const bool  dynamic = any_child(
                    node,
                    [&](const ts::Node& child)
                    {
                        if (child.type() == "comment")
                        {
                            return false;
                        }
                        const auto piece = cooked_text(child, source);
                        if (!piece.has_value())
                        {
                            return true;
                        }
                        joined += *piece;
                        return false;
                    });
                if (dynamic)
                {
                    return std::nullopt;
                }
                return joined;
            }
            if (type == "word" || type == "number")
            {
                const bool dynamic = any_child(
                    node,
                    [](const ts::Node& child)
                    { return in_table(child.type(), WORD_DYNAMIC); });
                if (dynamic)
                {
                    return std::nullopt;
                }
                return std::string{text};
            }
            return std::nullopt;
        }

        /// 先序收集全部 command 节点。
        void collect_commands(const ts::Node& node, std::vector<ts::Node>& out)
        {
            if (node.is_null())
            {
                return;
            }
            if (node.type() == "command")
            {
                out.push_back(node);
            }
            ts::for_each_child(
                node,
                [&](const ts::Node& child) { collect_commands(child, out); });
        }

        /// 沿父链找最近的规定类型祖先（找不到返回空节点）。
        [[nodiscard]]
        auto ancestor_of_type(const ts::Node& node, std::string_view type)
            -> ts::Node
        {
            for (ts::Node p = node.parent(); !p.is_null(); p = p.parent())
            {
                if (p.type() == type)
                {
                    return p;
                }
            }
            return ts::Node{};
        }

        /// command 在管道中的位置语义（first/middle/last/only）。
        [[nodiscard]]
        auto pipeline_position(const ts::Node& cmd) -> std::string_view
        {
            ts::Node p = cmd.parent();
            while (!p.is_null() && p.type() != "pipeline" && p.type() != "list")
            {
                p = p.parent();
            }
            if (p.is_null() || p.type() != "pipeline")
            {
                return "only";
            }
            std::vector<ts::Node> siblings;
            ts::for_each_child(
                p,
                [&](const ts::Node& child)
                {
                    if (child.type() == "command")
                    {
                        siblings.push_back(child);
                    }
                });
            std::size_t index = 0;
            bool        found = false;
            for (std::size_t i = 0; i < siblings.size(); ++i)
            {
                if (siblings[i] == cmd)
                {
                    index = i;
                    found = true;
                    break;
                }
            }
            if (!found || siblings.size() == 1)
            {
                return "only";
            }
            if (index == 0)
            {
                return "first";
            }
            if (index == siblings.size() - 1)
            {
                return "last";
            }
            return "middle";
        }

        /// command 的 command_name 子节点文本（无则空视图）。
        [[nodiscard]]
        auto command_name_text(const ts::Node& cmd, std::string_view source)
            -> std::string_view
        {
            const std::uint32_t count = cmd.named_child_count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const ts::Node child = cmd.named_child(i);
                if (child.type() == "command_name")
                {
                    return child.text(source);
                }
            }
            return {};
        }

        /// function_definition 是否定义了名为 rg 的函数（伪 rg 语境之一）。
        [[nodiscard]]
        auto function_named_rg(const ts::Node& root, std::string_view source)
            -> bool
        {
            return any_child(
                root,
                [&](const ts::Node& def)
                {
                    if (def.type() != "function_definition")
                    {
                        return false;
                    }
                    // 函数名是 field "name"，门面无 field 查询：按语法事实，
                    // 名字节点是其第一个 word 子节点。
                    const std::uint32_t n = def.named_child_count();
                    for (std::uint32_t i = 0; i < n; ++i)
                    {
                        const ts::Node child = def.named_child(i);
                        if (child.type() == "word")
                        {
                            return child.text(source) == "rg";
                        }
                    }
                    return false;
                });
        }

        /// 同串里 alias rg=... 重定义（伪 rg 语境之二）。
        [[nodiscard]]
        auto alias_shadow_rg(
            const std::vector<ts::Node>& commands,
            std::string_view             source) -> bool
        {
            for (const ts::Node& cmd : commands)
            {
                if (command_name_text(cmd, source) != "alias")
                {
                    continue;
                }
                const std::uint32_t n = cmd.named_child_count();
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    const ts::Node child = cmd.named_child(i);
                    if (child.type() == "command_name")
                    {
                        continue;
                    }
                    if (in_table(child.type(), NON_ARGUMENT))
                    {
                        continue;
                    }
                    const auto value = cooked_text(child, source);
                    if (value.has_value() && value->starts_with("rg="))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// parse_rg 的产出：reason 为空即成功，request 字段才有意义。
        struct RgParseOutcome
        {
            ShellRequest     request;
            std::string_view reason;
        };

        /**
         * @brief rg 的参数解析。
         *
         * argv 是已去掉命令名的静态参数串。放弃原因与矩阵口径逐字一致
         * （files-mode/meta-noop/pattern-from-file/invert/regex-dialect/
         * missing-value/no-pattern/control-char/too-long/unknown-flag）。
         */
        [[nodiscard]]
        auto parse_rg(const std::vector<std::string>& argv) -> RgParseOutcome
        {
            std::vector<std::string> patterns;
            std::vector<std::string> paths;
            std::vector<std::string> globs;
            std::vector<std::string> types;
            std::vector<std::string> positional;
            bool explicit_pattern = false; // -e/--regexp 出现过（与顺序无关）
            bool fixed = false;
            bool opts_done = false;

            const auto fail = [](std::string_view why) -> RgParseOutcome
            {
                return RgParseOutcome{.request = ShellRequest{}, .reason = why};
            };

            // 取等号后附着值，或吞下一个参数词。
            const auto take_value =
                [&](bool             has_eq,
                    std::string_view attached,
                    std::size_t&     i) -> std::optional<std::string>
            {
                if (has_eq)
                {
                    return std::string{attached};
                }
                if (i + 1 >= argv.size())
                {
                    return std::nullopt; // missing-value 由调用方落 reason
                }
                return argv[++i];
            };

            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                if (!opts_done && a == "--")
                {
                    opts_done = true;
                    continue;
                }
                if (!opts_done && a.starts_with("--"))
                {
                    const std::string_view body = std::string_view{a}.substr(2);
                    const std::size_t      eq = body.find('=');
                    const bool has_eq = eq != std::string_view::npos;
                    const std::string_view name =
                        has_eq ? body.substr(0, eq) : body;
                    const std::string_view attached =
                        has_eq ? body.substr(eq + 1) : std::string_view{};
                    if (in_table(name, RG_FILES_MODE))
                    {
                        return fail("files-mode");
                    }
                    if (in_table(name, RG_META_NOOP))
                    {
                        return fail("meta-noop");
                    }
                    if (name == "regexp")
                    {
                        const auto value = take_value(has_eq, attached, i);
                        if (!value.has_value())
                        {
                            return fail("missing-value");
                        }
                        explicit_pattern = true;
                        patterns.push_back(std::move(*value));
                    }
                    else if (name == "file")
                    {
                        return fail("pattern-from-file");
                    }
                    else if (name == "glob")
                    {
                        const auto value = take_value(has_eq, attached, i);
                        if (!value.has_value())
                        {
                            return fail("missing-value");
                        }
                        globs.push_back(std::move(*value));
                    }
                    else if (name == "type" || name == "type-not")
                    {
                        const auto value = take_value(has_eq, attached, i);
                        if (!value.has_value())
                        {
                            return fail("missing-value");
                        }
                        if (name == "type")
                        {
                            types.push_back(std::move(*value));
                        }
                    }
                    else if (name == "invert-match")
                    {
                        return fail("invert");
                    }
                    else if (name == "fixed-strings")
                    {
                        fixed = true;
                    }
                    else if (name == "pcre2")
                    {
                        return fail("regex-dialect");
                    }
                    else if (in_table(name, RG_FLAG_LONG))
                    {
                        // 已知无值长选项：忽略。
                    }
                    else if (in_table(name, RG_VALUE_LONG))
                    {
                        if (!has_eq)
                        {
                            if (i + 1 >= argv.size())
                            {
                                return fail("missing-value");
                            }
                            ++i; // 吞掉值
                        }
                    }
                    else
                    {
                        return fail("unknown-flag");
                    }
                    continue;
                }
                if (!opts_done && a.starts_with("-") && a != "-")
                {
                    bool cluster_done = false;
                    for (std::size_t j = 1; j < a.size() && !cluster_done;)
                    {
                        const char c = a[j];
                        if (c == 'e' || c == 'g' || c == 't' || c == 'T')
                        {
                            // 带值短选项：簇内附着余量或分离的下一个词。
                            const std::string_view rest =
                                std::string_view{a}.substr(j + 1);
                            std::string value;
                            if (!rest.empty())
                            {
                                value.assign(rest);
                            }
                            else
                            {
                                if (i + 1 >= argv.size())
                                {
                                    return fail("missing-value");
                                }
                                value = argv[++i];
                            }
                            if (c == 'e')
                            {
                                explicit_pattern = true;
                                patterns.push_back(std::move(value));
                            }
                            else if (c == 'g')
                            {
                                globs.push_back(std::move(value));
                            }
                            else if (c == 't')
                            {
                                types.push_back(std::move(value));
                            }
                            cluster_done = true;
                        }
                        else if (c == 'f')
                        {
                            return fail("pattern-from-file");
                        }
                        else if (c == 'v')
                        {
                            return fail("invert");
                        }
                        else if (c == 'P')
                        {
                            return fail("regex-dialect");
                        }
                        else if (c == 'F')
                        {
                            fixed = true;
                            ++j;
                        }
                        else if (in_short_set(c, RG_VALUE_SHORT))
                        {
                            // 通用带值短选项：附着余量或吞下一个词。
                            if (j + 1 < a.size())
                            {
                                cluster_done = true;
                            }
                            else
                            {
                                if (i + 1 >= argv.size())
                                {
                                    return fail("missing-value");
                                }
                                ++i;
                                cluster_done = true;
                            }
                        }
                        else if (in_short_set(c, RG_FLAG_SHORT))
                        {
                            ++j;
                        }
                        else
                        {
                            return fail("unknown-flag");
                        }
                    }
                    continue;
                }
                // 位置参数先暂存：是否当路径取决于 -e/--regexp 是否出现，
                // 而它与位置参数的先后顺序无关（rg 真实语义）。
                positional.push_back(a);
            }

            if (!positional.empty())
            {
                if (explicit_pattern)
                {
                    paths.insert(
                        paths.end(),
                        std::make_move_iterator(positional.begin()),
                        std::make_move_iterator(positional.end()));
                }
                else
                {
                    patterns.push_back(std::move(positional.front()));
                    paths.insert(
                        paths.end(),
                        std::make_move_iterator(positional.begin() + 1),
                        std::make_move_iterator(positional.end()));
                }
            }

            if (patterns.empty())
            {
                return fail("no-pattern");
            }
            for (const std::string& p : patterns)
            {
                for (const char ch : p)
                {
                    if (static_cast<unsigned char>(ch) < 32
                        || static_cast<unsigned char>(ch) == 127)
                    {
                        return fail("control-char");
                    }
                }
                if (p.size() > 256)
                {
                    return fail("too-long");
                }
            }
            ShellRequest request;
            request.engine = Engine::Rg;
            request.patterns = std::move(patterns);
            request.single = request.patterns.size() == 1;
            request.fixed = fixed;
            request.paths = std::move(paths);
            request.globs = std::move(globs);
            request.types = std::move(types);
            request.path_kind = PathKind::None;
            return RgParseOutcome{.request = std::move(request), .reason = {}};
        }

        /**
         * @brief rg 单元之前最近的字面 cd 目标（cwd 提示）。
         *
         * 在同一 list/program 内按序找
         * 本命令之前的命令，倒序第一个 cd 带静态目标即提示；遇到 pushd 则
         * 放弃（目录栈语义不可知）。
         */
        [[nodiscard]]
        auto cd_hint_before(const ts::Node& cmd, std::string_view source)
            -> std::optional<std::string>
        {
            ts::Node p = cmd.parent();
            while (!p.is_null() && p.type() != "list" && p.type() != "program")
            {
                p = p.parent();
            }
            if (p.is_null())
            {
                return std::nullopt;
            }
            std::vector<ts::Node> commands;
            collect_commands(p, commands);
            std::size_t index = commands.size();
            for (std::size_t i = 0; i < commands.size(); ++i)
            {
                if (commands[i] == cmd)
                {
                    index = i;
                    break;
                }
            }
            for (std::size_t back = index; back > 0; --back)
            {
                const ts::Node&        prev = commands[back - 1];
                const std::string_view name = command_name_text(prev, source);
                if (name == "pushd")
                {
                    return std::nullopt;
                }
                if (name == "cd")
                {
                    // 第一个参数（跳过 command_name 与非参数形态）。
                    const std::uint32_t n = prev.named_child_count();
                    bool                took_name = false;
                    for (std::uint32_t i = 0; i < n; ++i)
                    {
                        const ts::Node child = prev.named_child(i);
                        if (child.type() == "command_name")
                        {
                            took_name = true;
                            continue;
                        }
                        if (!took_name || in_table(child.type(), NON_ARGUMENT))
                        {
                            continue;
                        }
                        const auto value = cooked_text(child, source);
                        if (value.has_value())
                        {
                            return value;
                        }
                        return std::nullopt; // cd 目标动态：提示放弃
                    }
                }
            }
            return std::nullopt;
        }

        /// 命令的静态参数表（跳过 command_name 与非参数形态）；任一参数动态
        /// 时 whole_dynamic 置真。
        struct CommandArgs
        {
            std::vector<std::string> args;
            bool                     whole_dynamic = false;
        };

        [[nodiscard]]
        auto command_args(const ts::Node& cmd, std::string_view source)
            -> CommandArgs
        {
            CommandArgs         out;
            const std::uint32_t n = cmd.named_child_count();
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const ts::Node child = cmd.named_child(i);
                if (child.type() == "command_name"
                    || in_table(child.type(), NON_ARGUMENT))
                {
                    continue;
                }
                const auto value = cooked_text(child, source);
                if (!value.has_value())
                {
                    out.whole_dynamic = true;
                    return out;
                }
                out.args.push_back(std::move(*value));
            }
            return out;
        }

        /// Bash 命令里反调 PowerShell 的桥接提取（引擎不可用时静默空手）。
        [[nodiscard]]
        auto ps_bridge_queries(const std::vector<std::string>& args)
            -> ShellExtractOutcome
        {
            ShellExtractOutcome empty;
            for (std::size_t k = 0; k < args.size(); ++k)
            {
                const std::string flag = ascii_lower(args[k]);
                if (!in_table(flag, PS_BRIDGE_FLAGS))
                {
                    continue;
                }
                if (k + 1 < args.size())
                {
                    return extract_ps_queries(args[k + 1]);
                }
            }
            return empty;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Bash 提取主流程。
    // ---------------------------------------------------------------------------

    auto bash_mentions_search(std::string_view command) -> bool
    {
        if (command.size() < 2)
        {
            return false;
        }
        // 首字节小写化后才进三联比对，均摊每字节一次比较，微秒级走完。
        const auto lower_at = [&](std::size_t at, std::string_view needle)
        {
            if (at + needle.size() > command.size())
            {
                return false;
            }
            for (std::size_t k = 0; k < needle.size(); ++k)
            {
                char c = command[at + k];
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
                if (c != needle[k])
                {
                    return false;
                }
            }
            return true;
        };
        const auto word_byte = [](char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9') || c == '_' || c == '-';
        };
        const auto word_bounded = [&](std::size_t at, std::size_t len)
        {
            const bool left_ok = at == 0 || !word_byte(command[at - 1]);
            const bool right_ok =
                at + len >= command.size() || !word_byte(command[at + len]);
            return left_ok && right_ok;
        };
        for (std::size_t i = 0; i + 2 <= command.size(); ++i)
        {
            char c = command[i];
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c == 'r')
            {
                if (lower_at(i, "rg") && word_bounded(i, 2))
                {
                    return true;
                }
            }
            else if (c == 's')
            {
                if (lower_at(i, "sls") && word_bounded(i, 3))
                {
                    return true;
                }
                if (lower_at(i, "select-string"))
                {
                    return true;
                }
            }
        }
        return false;
    }

    auto extract_bash_queries(std::string_view command) -> ShellExtractOutcome
    {
        static const ts::Parser parser{ts::bash_language()};
        const ts::Tree          tree = parser.parse(command);
        const ts::Node          root = tree.root_node();

        ShellExtractOutcome outcome;

        if (root.is_null() || root.has_error())
        {
            // 残缺输入的树级闸门（矩阵口径：残缺引号等一切语法错误）。
            outcome.skip = "unterminated-quote";
            return outcome;
        }

        std::vector<ts::Node> commands;
        collect_commands(root, commands);

        if (function_named_rg(root, command)
            || alias_shadow_rg(commands, command))
        {
            outcome.skip_all = "alias-shadow";
            return outcome;
        }

        for (const ts::Node& cmd : commands)
        {
            // 命令替换内的 rg 意义嵌在别的命令的参数里：宁漏勿猜跳过。
            if (!ancestor_of_type(cmd, "command_substitution").is_null())
            {
                continue;
            }
            std::string_view head = command_name_text(cmd, command);
            CommandArgs      parsed = command_args(cmd, command);

            if (head == "command" || head == "env")
            {
                // 剥一层 command/env 包装：仅当紧跟的字面参数恰是 rg。
                if (!parsed.args.empty() && parsed.args.front() == "rg")
                {
                    parsed.args.erase(parsed.args.begin());
                    head = "rg";
                }
                else
                {
                    continue;
                }
            }

            if (in_table(head, PS_BRIDGE_HOSTS))
            {
                ShellExtractOutcome bridged = ps_bridge_queries(parsed.args);
                outcome.requests.insert(
                    outcome.requests.end(),
                    bridged.requests.begin(),
                    bridged.requests.end());
                continue;
            }

            if (head != "rg")
            {
                continue;
            }

            if (parsed.whole_dynamic)
            {
                if (outcome.requests.empty())
                {
                    outcome.skip = "dynamic-argument";
                    return outcome;
                }
                continue;
            }

            auto parsed_rg = parse_rg(parsed.args);
            if (!parsed_rg.reason.empty())
            {
                if (outcome.requests.empty())
                {
                    outcome.skip = std::string{parsed_rg.reason};
                    return outcome;
                }
                continue;
            }

            const std::string_view pos = pipeline_position(cmd);
            if (pos == "middle" || pos == "last")
            {
                parsed_rg.request.path_kind = PathKind::StdinPipe;
            }
            if (const auto hint = cd_hint_before(cmd, command);
                hint.has_value())
            {
                parsed_rg.request.path_kind = PathKind::AfterCd;
                parsed_rg.request.cwd_hint = *hint;
            }
            outcome.requests.push_back(std::move(parsed_rg.request));
        }
        return outcome;
    }

    // ---------------------------------------------------------------------------
    // PowerShell 协议解析与 Search 直读。
    // ---------------------------------------------------------------------------

    namespace
    {

        /// shim JSON 的 path_kind 字符串到枚举。
        [[nodiscard]]
        auto parse_path_kind(std::string_view text) -> PathKind
        {
            if (text == "stdin-pipe")
            {
                return PathKind::StdinPipe;
            }
            if (text == "after-cd")
            {
                return PathKind::AfterCd;
            }
            return PathKind::None;
        }

        [[nodiscard]]
        auto strings_from_array(const json::ValueView& array)
            -> std::optional<std::vector<std::string>>
        {
            std::vector<std::string> out;
            const std::size_t        count = array.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto element = array.at(i);
                if (!element.has_value() || !element->is_string())
                {
                    return std::nullopt;
                }
                out.emplace_back(*element->as_string());
            }
            return out;
        }

        [[nodiscard]]
        auto request_from_json(const json::ValueView& obj)
            -> std::optional<ShellRequest>
        {
            ShellRequest request;
            if (const auto engine = obj.get("engine");
                engine.has_value() && engine->is_string())
            {
                if (*engine->as_string() == "select-string")
                {
                    request.engine = Engine::SelectString;
                }
            }
            if (const auto patterns = obj.get("patterns");
                patterns.has_value() && patterns->is_array())
            {
                request.patterns = strings_from_array(*patterns).value_or({});
            }
            if (const auto single = obj.get("single");
                single.has_value() && single->is_bool())
            {
                request.single = *single->as_bool();
            }
            else
            {
                request.single = request.patterns.size() == 1;
            }
            if (const auto fixed = obj.get("fixed");
                fixed.has_value() && fixed->is_bool())
            {
                request.fixed = *fixed->as_bool();
            }
            if (const auto paths = obj.get("paths");
                paths.has_value() && paths->is_array())
            {
                request.paths = strings_from_array(*paths).value_or({});
            }
            if (const auto globs = obj.get("globs");
                globs.has_value() && globs->is_array())
            {
                request.globs = strings_from_array(*globs).value_or({});
            }
            if (const auto types = obj.get("types");
                types.has_value() && types->is_array())
            {
                request.types = strings_from_array(*types).value_or({});
            }
            if (const auto kind = obj.get("path_kind");
                kind.has_value() && kind->is_string())
            {
                request.path_kind = parse_path_kind(*kind->as_string());
            }
            if (const auto hint = obj.get("cwd_hint");
                hint.has_value() && hint->is_string())
            {
                request.cwd_hint = std::string{*hint->as_string()};
            }
            return request;
        }

    } // namespace

    auto extract_ps_queries(std::string_view command) -> ShellExtractOutcome
    {
        ShellExtractOutcome outcome;
        const auto          payload = cshost::parse_via_engine(command);
        if (!payload.has_value())
        {
            // 引擎不可用 / 预筛未命中 / 通道失败：静默空手（宁漏）。
            return outcome;
        }
        try
        {
            const json::Document  doc = json::Document::parse(*payload);
            const json::ValueView root = doc.root();
            if (!root.is_object())
            {
                return outcome;
            }
            if (const auto requests = root.get("requests");
                requests.has_value() && requests->is_array())
            {
                const std::size_t count = requests->size();
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (const auto element = requests->at(i);
                        element.has_value())
                    {
                        if (const auto request = request_from_json(*element);
                            request.has_value())
                        {
                            outcome.requests.push_back(std::move(*request));
                        }
                    }
                }
            }
            if (const auto skip = root.get("skip");
                skip.has_value() && skip->is_string())
            {
                outcome.skip = std::string{*skip->as_string()};
            }
            if (const auto skip_all = root.get("skip_all");
                skip_all.has_value() && skip_all->is_string())
            {
                outcome.skip_all = std::string{*skip_all->as_string()};
            }
        }
        catch (const std::exception&)
        {
            // 协议解析失败按宁漏处理：空手退出。
            return ShellExtractOutcome{};
        }
        return outcome;
    }

    auto search_tool_queries(const json::ValueView& tool_input)
        -> ShellExtractOutcome
    {
        ShellExtractOutcome outcome;
        if (!tool_input.is_object())
        {
            return outcome;
        }
        // 防御式三候选字段：取第一个存在的字符串值。
        constexpr std::string_view CANDIDATES[] = {"query", "pattern", "q"};
        for (const std::string_view key : CANDIDATES)
        {
            const auto field = tool_input.get(key);
            if (!field.has_value() || !field->is_string())
            {
                continue;
            }
            const std::string_view text = *field->as_string();
            if (text.empty())
            {
                outcome.skip = "no-pattern";
                return outcome;
            }
            if (text.size() > 256)
            {
                outcome.skip = "too-long";
                return outcome;
            }
            for (const char ch : text)
            {
                if (static_cast<unsigned char>(ch) < 32
                    || static_cast<unsigned char>(ch) == 127)
                {
                    outcome.skip = "control-char";
                    return outcome;
                }
            }
            ShellRequest request;
            request.engine = Engine::SearchField;
            request.patterns.emplace_back(text);
            request.single = true;
            // 可选的 path 字段（与 Grep 工具同形）作为单条路径提示带上，
            // 交归一化闸门与 shell 臂同款把关（通配即拒）。
            if (const auto hint = tool_input.get("path");
                hint.has_value() && hint->is_string())
            {
                const std::string_view dir = *hint->as_string();
                if (!dir.empty())
                {
                    request.paths.emplace_back(dir);
                }
            }
            outcome.requests.push_back(std::move(request));
            return outcome;
        }
        return outcome;
    }

} // namespace cg::util::shellscan
