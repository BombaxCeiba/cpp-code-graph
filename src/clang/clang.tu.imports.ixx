

/**
 * @module cg.clang.tu.imports
 *
 * @brief import 声明域：文本扫描定位 import 声明点、叠加重写为源文件形态。
 */
export module cg.clang.tu.imports;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.tu.lex;

export namespace cg::clang
{

    /**
     * @brief scan_import_decls 定位到的一条书写 import 声明。
     *
     * [begin, end) 跨度覆盖可选的前导 `export` 关键字、总在终结 ';'
     * 之后一格结束。头单元 import 携带其拼写（`"h"` / `<h>`）与去引号
     * 的名字；命名模块 import 携带模块名（"std"、"cg.util"、"mp:core"）。
     * starts_line 报告该位置的首 token（有 rider 时的 `export`，否则
     * `import`）是否为其物理行的首个非空白 token——改写后的预处理指令
     * 唯一能占据的位置（行中的 '#' 不是指令）。
     */
    struct ImportDeclSite
    {
        std::size_t      begin = 0;
        std::size_t      end = 0;
        bool             is_header = false;
        bool             starts_line = false; // 位置首 token 开启其行
        std::string_view name;
        std::string_view header_spell; // 含引号/尖括号，仅头单元
    };

    namespace detail
    {

        /**
         * @brief scan_import_decls 的扫描回调：export rider 记号 + import
         *        声明定界。
         *
         * 口径差异全部内聚于此：预处理逻辑行整行吞掉（on_hash）、非标识符
         * 代码字符放弃挂起的 export 记号（on_other_char）、`import` 关键字
         * 解析至其终结 ';'（on_identifier——定界成功时返回 site.end 让核心
         * 从声明终点继续，婉拒时返回 nullopt 从关键字之后恢复）。
         */
        struct ImportScanHooks
        {
            static constexpr std::size_t none = std::string_view::npos;

            std::string_view             text;
            std::vector<ImportDeclSite>* sites;
            std::size_t                  export_at =
                none; // 仍有资格骑下一个 'import' 的 'export'
            bool export_starts_line = false; // 该 'export' 处的 line_start

            /**
             * @brief 跳过空白与注释（import 声明文法的 token 分隔）。
             *
             * 仅未闭合块注释返回 none。
             */
            [[nodiscard]]
            auto skip_gap(std::size_t k) const -> std::size_t
            {
                for (;;)
                {
                    while (k < text.size() && is_space_char(text[k]))
                    {
                        ++k;
                    }
                    if (k + 1 < text.size() && text[k] == '/'
                        && text[k + 1] == '/')
                    {
                        k += 2;
                        while (k < text.size() && text[k] != '\n')
                        {
                            k += text[k] == '\\' && k + 1 < text.size() ? 2 : 1;
                        }
                        continue;
                    }
                    if (k + 1 < text.size() && text[k] == '/'
                        && text[k + 1] == '*')
                    {
                        const std::size_t shut = text.find("*/", k + 2);
                        if (shut == none)
                        {
                            return none;
                        }
                        k = shut + 2;
                        continue;
                    }
                    return k;
                }
            }

            void on_newline() {}

            auto on_hash(std::size_t i) -> std::optional<std::size_t>
            {
                std::size_t k = i + 1;
                while (k < text.size() && text[k] != '\n')
                {
                    k += text[k] == '\\' && k + 1 < text.size() ? 2 : 1;
                }
                return k;
            }

            void on_other_char() { export_at = none; }

            auto on_identifier(std::size_t i, std::size_t j, bool opens_line)
                -> std::optional<std::size_t>
            {
                const std::string_view word{text.data() + i, j - i};
                if (word != "import")
                {
                    if (word == "export")
                    {
                        export_at = i;
                        export_starts_line = opens_line;
                    }
                    else
                    {
                        export_at = none;
                    }
                    return std::nullopt;
                }
                ImportDeclSite site;
                site.begin = export_at != none ? export_at : i;
                site.starts_line =
                    export_at != none ? export_starts_line : opens_line;
                bool        ok = true;
                std::size_t k = skip_gap(j);
                if (k == none)
                {
                    ok = false;
                }
                if (ok && k + 1 < text.size() && text[k] == '['
                    && text[k + 1] == '[')
                {
                    const std::size_t shut = text.find("]]", k + 2);
                    if (shut == none)
                    {
                        ok = false;
                    }
                    else
                    {
                        k = skip_gap(shut + 2);
                        ok = k != none;
                    }
                }
                if (ok)
                {
                    if (k < text.size() && text[k] == '"')
                    {
                        std::size_t m = k + 1;
                        while (m < text.size() && text[m] != '"'
                               && text[m] != '\n')
                        {
                            m += text[m] == '\\' && m + 1 < text.size() ? 2 : 1;
                        }
                        if (m < text.size() && text[m] == '"')
                        {
                            site.is_header = true;
                            site.header_spell = text.substr(k, m - k + 1);
                            site.name = text.substr(k + 1, m - k - 1);
                            k = m + 1;
                        }
                        else
                        {
                            ok = false;
                        }
                    }
                    else if (k < text.size() && text[k] == '<')
                    {
                        const std::size_t shut = text.find('>', k + 1);
                        if (shut != none)
                        {
                            site.is_header = true;
                            site.header_spell = text.substr(k, shut - k + 1);
                            site.name = text.substr(k + 1, shut - k - 1);
                            k = shut + 1;
                        }
                        else
                        {
                            ok = false;
                        }
                    }
                    else if (k < text.size() && is_ident_char(text[k]))
                    {
                        std::size_t m = k;
                        while (m < text.size()
                               && (is_ident_char(text[m]) || text[m] == '.'
                                   || text[m] == ':'))
                        {
                            ++m;
                        }
                        site.name = text.substr(k, m - k);
                        k = m;
                    }
                    else
                    {
                        ok = false;
                    }
                }
                if (ok)
                {
                    k = skip_gap(k);
                    ok = k != none && k < text.size() && text[k] == ';';
                }
                if (ok)
                {
                    site.end = k + 1;
                    sites->push_back(site);
                    export_at = none;
                    return site.end;
                }
                export_at = none; // 婉拒：声明保持原样，从关键字之后恢复
                return std::nullopt;
            }
        };

    } // namespace detail

    /**
     * @brief 书写 import 声明的词法扫描。
     *
     * 与 export 覆盖共享 detail::lex_scan 词法核心（代码/注释/字符串/
     * 原始字符串/字符/拼接状态机），故注释、字面量、原始字符串或预处理
     * 指令行内的 `import` 绝不匹配。每个 `import` 关键字（可带仅隔
     * 空白/注释的可选 `export` rider）解析至其终结 ';'；扫描器无法自信
     * 定界的声明（无 ';'、名字后是垃圾）被婉拒并保持原样。多行声明、
     * 尾注释、';' 前空白、同一物理行多个 import 都已处理——这些 import
     * 映射的投喂可能已被抹除（伞头降级），形状触发的婉拒绝不能滞留
     * 无映射的 import。
     *
     * @internal
     * 本侧口径差异（逻辑行整行吞掉、export 记号、声明定界）见
     * detail::ImportScanHooks：skip_gap 跳过空白与注释（import 声明文法
     * 的 token 分隔），仅未闭合块注释返回 none；位置首 token 决定改写后
     * 的预处理指令能否合法落在 site.begin——`export` 开启位置时携带
     * rider 的行位；可选属性说明符序列 [[ … ]] 被跳过；被消费的关键字
     * 是行 token：同物理行其后的任何内容在行中开启其位置。婉拒时：
     * 机器从关键字之后立刻恢复；声明保持原样。
     */
    [[nodiscard]]
    auto scan_import_decls(std::string_view text) -> std::vector<ImportDeclSite>
    {
        std::vector<ImportDeclSite> sites;
        detail::ImportScanHooks     hooks{.text = text, .sites = &sites};
        detail::lex_scan(text, hooks);
        return sites;
    }

    /**
     * @brief import 声明改写覆盖。
     *
     * 构建于 scan_import_decls 之上，扫描器能定界的每种声明形状都被
     * 处理：多行 import、尾注释、';' 前空白、同一物理行多个 import、
     * 被注释隔开的 `export`/`import`。每次替换都精确保留书写行数（被
     * 替换跨度内的换行存活；指令整体落在跨度首行、续行同宽消隐），
     * 改写之下的抽取位置保持在书写行上。
     *
     * 各模式的语义：
     * - std_umbrella：`import std;` / `import std.compat;`（允许可选
     *   `export` rider）变为 `#include <__msvc_all_public_headers.hpp>`
     *   ——MSVC 地混合 TU 的降级层。TU 文本地保留整个 STL（工具链
     *   include 目录骑在解析 include 路径上），而非让 std pcm 与被包含
     *   的头冲突。
     * - dropped_modules（伞头模式）：投喂被抹除（其闭包绑着 std）的
     *   命名模块 import 同宽消隐——无映射的 import 会让本该存活的降级
     *   解析硬失败。
     * - header_unit_includes：`import "hdr";` / `import <hdr>;` 变为对应
     *   的 `#include`（v1 策略：头单元降级为文本包含；module-map 形式
     *   上游仍是实验性）。
     * - demodule：整条模块声明行同宽消隐（仅有头单元 import 的接口
     *   降级为经典解析）。
     *
     * @internal
     * 两个产出 `#include` 的通道仅在位置首 token 开启其物理行时发出指令
     * （ImportDeclSite::starts_line）：不是行首非空白 token 的 '#' 不是
     * 预处理指令，故行中位置（`import win; import std;`、
     * `void g(); import "cfg.h";`）改为同宽消隐——干净的降级，同行的
     * 邻居与列不动。发出还额外要求 import 跨度之后的行尾只剩空白/注释：
     * 长大的指令占据其整个物理行，预处理器会把任何其他跟随 token 作为
     * "`#include` 指令末尾的多余 token"静默丢弃——因此跟随代码也同宽消隐。
     * 被消隐的 import 仍经返回结构的 replaced 上报。replaced 按文件
     * 序收集被改写的 import 名（"std"、"std.compat"、被丢弃的模块、或头
     * 拼写），使索引器能为不再携带 import 游标的行落扫描通道的 import
     * 行。无匹配时返回原文。实现细节：前向遍历判定并收集（名字按文件
     * 序落位），编辑按逆序应用使更早的偏移保持有效；替换整体落在跨度
     * 首行、跨度内每个换行存活、续行同宽消隐——书写行数精确保留；跨
     * 度后的列在替换变长时会移位（指令必须连续），但非注释跟随 token
     * 不止移位——预处理器会把它作为"`#include` 指令末尾的多余 token"
     * 删除，这正是发出被行尾惰性检查门控的原因：只有空白与注释可能
     * 移位到长大指令的后面。demodule 分支对 `out` 取视图而非
     * std::string::substr（后者返回临时 std::string、绑定到视图会悬垂）；
     * 同宽消隐使 TU 降级为经典解析、下方每个书写行列保持精确。
     */
    struct ImportOverlay
    {
        std::string              text;     ///< 改写后文本；无匹配时与输入相同
        std::vector<std::string> replaced; ///< 被改写的 import 名（文件序）
    };

    [[nodiscard]]
    auto import_rewrite_overlay(
        std::string_view             src,
        bool                         std_umbrella,
        bool                         header_unit_includes,
        bool                         demodule = false,
        std::span<const std::string> dropped_modules = {}) -> ImportOverlay
    {
        if (!std_umbrella && !header_unit_includes && !demodule
            && dropped_modules.empty())
        {
            return ImportOverlay{.text = std::string{src}};
        }
        std::string              out{src};
        std::vector<std::string> replaced_names;
        if (std_umbrella || header_unit_includes || !dropped_modules.empty())
        {
            struct Edit
            {
                std::size_t begin;
                std::size_t end;
                std::string replacement;
            };
            std::vector<Edit> edits;
            for (const ImportDeclSite& site : scan_import_decls(out))
            {
                std::string       replacement;
                std::string       replaced_name;
                bool              acted = false;
                const std::size_t span_nl = out.find('\n', site.begin);
                const bool        directive_legal =
                    site.starts_line
                    && ((span_nl != std::string_view::npos
                         && span_nl < site.end)
                        || detail::rest_of_line_is_inert(out, site.end));
                if (site.is_header)
                {
                    if (header_unit_includes)
                    {
                        if (directive_legal)
                        {
                            replacement =
                                std::format("#include {}", site.header_spell);
                        }
                        replaced_name = std::string{site.name};
                        acted = true;
                    }
                }
                else if (
                    std_umbrella
                    && (site.name == "std" || site.name == "std.compat"))
                {
                    if (directive_legal)
                    {
                        replacement =
                            "#include <__msvc_all_public_headers.hpp>";
                    }
                    replaced_name = std::string{site.name};
                    acted = true;
                }
                else if (
                    std_umbrella
                    && std::ranges::find(dropped_modules, site.name)
                           != dropped_modules.end())
                {
                    replaced_name = std::string{site.name};
                    acted = true;
                }
                if (acted)
                {
                    edits.push_back(
                        {.begin = site.begin,
                         .end = site.end,
                         .replacement = std::move(replacement)});
                    if (!replaced_name.empty())
                    {
                        replaced_names.push_back(std::move(replaced_name));
                    }
                }
            }
            for (const Edit& edit : std::views::reverse(edits))
            {
                const std::size_t span = edit.end - edit.begin;
                std::size_t       first_nl = out.find('\n', edit.begin);
                if (first_nl == std::string_view::npos || first_nl >= edit.end)
                {
                    first_nl = std::string_view::npos;
                }
                std::string       segment;
                const std::size_t first_len = first_nl == std::string_view::npos
                                                  ? span
                                                  : first_nl - edit.begin;
                segment = edit.replacement;
                if (segment.size() < first_len)
                {
                    segment.append(first_len - segment.size(), ' ');
                }
                std::size_t at = first_nl;
                while (at != std::string_view::npos && at < edit.end)
                {
                    std::size_t next = out.find('\n', at + 1);
                    if (next == std::string_view::npos || next >= edit.end)
                    {
                        next = edit.end;
                    }
                    segment += '\n';
                    segment.append(next - (at + 1), ' ');
                    at = next;
                }
                out.replace(edit.begin, span, segment);
            }
        }
        if (demodule)
        {
            std::string blanked;
            blanked.reserve(out.size());
            const std::string_view text{out};
            std::size_t            at = 0;
            while (at <= text.size())
            {
                const std::size_t      end = text.find('\n', at);
                const std::string_view line = text.substr(
                    at,
                    end == std::string_view::npos ? std::string_view::npos
                                                  : end - at);
                std::size_t lead = 0;
                while (lead < line.size()
                       && (line[lead] == ' ' || line[lead] == '\t'
                           || line[lead] == '\r'))
                {
                    ++lead;
                }
                const std::string_view rest = line.substr(lead);
                if (rest.starts_with("export module ")
                    || rest.starts_with("export\tmodule ")
                    || rest.starts_with("module ")
                    || rest.starts_with("module\t")
                    || rest.starts_with("module;"))
                {
                    blanked.append(line.size(), ' ');
                }
                else
                {
                    blanked.append(line);
                }
                if (end == std::string_view::npos)
                {
                    break;
                }
                blanked.push_back('\n');
                at = end + 1;
            }
            out = std::move(blanked);
        }
        ImportOverlay result;
        result.text = std::move(out);
        result.replaced = std::move(replaced_names);
        return result;
    }
} // namespace cg::clang
