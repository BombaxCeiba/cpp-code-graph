/**
 * @module cg.clang.tu
 *
 * @brief 翻译单元级 API：单次调用解析、返回 RAII 句柄加严重度
 * >=error 的诊断，以及一切走查器都需要的主文件游标过滤器（包含头
 * 内的游标位置绝不能成为项目符号——探针验证过的过滤模式）。
 */
export module cg.clang.tu;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;

export namespace cg::clang
{

    /**
     * @brief 支撑内建头（stddef.h 等）的工具链资源目录。
     *
     * 重放 argv 自带 -resource-dir 时不使用。每进程一次、从 clang 驱动器
     * 自身发现（`-print-resource-dir`，经 cg.clang.driver）；对已发现
     * 工具链布局（`<驱动器前缀>`/lib/clang/ 下的版本目录通配）的目录扫描
     * 仅是无法应答的
     * 驱动器的兜底。什么都解析不到时为 ""，重放保持原样记录。
     */
    [[nodiscard]]
    auto builtin_resource_dir() -> const std::string&
    {
        static const std::string CACHED = []
        {
            const DriverInfo& info = driver_info();
            if (!info.resource_dir.empty())
            {
                return info.resource_dir;
            }
            if (info.driver.empty())
            {
                return std::string{};
            }
            namespace fs = std::filesystem;
            const fs::path prefix =
                fs::path{info.driver}.parent_path().parent_path();
            const fs::path  base = prefix / "lib" / "clang";
            std::error_code ec;
            for (const fs::directory_entry& version :
                 fs::directory_iterator(base, ec))
            {
                if (!version.is_directory(ec))
                {
                    continue;
                }
                if (fs::is_regular_file(
                        version.path() / "include" / "stddef.h",
                        ec))
                {
                    return cg::util::norm_path(version.path().generic_string());
                }
            }
            return std::string{};
        }();
        return CACHED;
    }

    namespace detail
    {

        /** @brief 可延续 C++ 标识符 token 的字符。 */
        [[nodiscard]]
        auto is_ident_char(char c) -> bool
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9') || c == '_';
        }

        /**
         * @brief C++ 词法器视为 token 分隔的空白（含换行：
         * `export\nmodule m;` 是一条模块声明）。
         */
        [[nodiscard]]
        auto is_space_char(char c) -> bool
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v'
                   || c == '\f';
        }

        /**
         * @brief text 中 from 起及其后下一个标识符 token 的拼写。
         *
         * 跳过空白与注释。nullopt = 后面无有意义内容（EOF 或未闭合注释）；
         * 空值 = 后面是 meaningful 的非标识符（如 `export {` 的 `{`）。
         *
         * @internal
         * 用于判定 `export` 关键字是骑在 `export module`/`export import` 上
         * （绝不消隐）还是前置于声明（消隐）。行注释的拼接续行处理：只跳过
         * 反斜杠本身，让换行处理自然结束或拼接它。
         */
        [[nodiscard]]
        auto next_identifier(std::string_view text, std::size_t from)
            -> std::optional<std::string_view>
        {
            const std::size_t n = text.size();
            std::size_t       k = from;
            while (k < n)
            {
                const char c = text[k];
                if (is_space_char(c))
                {
                    ++k;
                    continue;
                }
                if (c == '/' && k + 1 < n && text[k + 1] == '/')
                {
                    k += 2;
                    while (k < n && text[k] != '\n')
                    {
                        k += text[k] == '\\' ? 2 : 1;
                    }
                    continue;
                }
                if (c == '/' && k + 1 < n && text[k + 1] == '*')
                {
                    const std::size_t close = text.find("*/", k + 2);
                    if (close == std::string_view::npos)
                    {
                        return std::nullopt;
                    }
                    k = close + 2;
                    continue;
                }
                break;
            }
            if (k >= n)
            {
                return std::nullopt;
            }
            if (!is_ident_char(text[k]))
            {
                return std::string_view{};
            }
            std::size_t m = k;
            while (m < n && is_ident_char(text[m]))
            {
                ++m;
            }
            return text.substr(k, m - k);
        }

        /**
         * @brief from 到其物理行尾之间是否全为空白和/或注释。
         *
         * 这是长大的 `#include` 指令唯一能容忍的行尾。
         *
         * @internal
         * 出现任何其他跟随 token 都会被预处理器作为"`#include` 指令末尾的
         * 多余 token"静默丢弃（一条永远到不了严重度>=error 诊断收集的告警），
         * 故发出改写的指令前必须先证明这一点。跨换行的块注释会让指令活过
         * 多行，按非惰性计（保守：消隐）。
         */
        [[nodiscard]]
        auto rest_of_line_is_inert(std::string_view text, std::size_t from)
            -> bool
        {
            const std::size_t n = text.size();
            std::size_t       i = from;
            while (i < n)
            {
                const char c = text[i];
                if (c == '\n')
                {
                    return true;
                }
                if (is_space_char(c))
                {
                    ++i;
                    continue;
                }
                if (c == '/' && i + 1 < n && text[i + 1] == '/')
                {
                    return true; // 注释一路到换行
                }
                if (c == '/' && i + 1 < n && text[i + 1] == '*')
                {
                    const std::size_t shut = text.find("*/", i + 2);
                    if (shut == std::string_view::npos)
                    {
                        return false;
                    }
                    for (std::size_t k = i; k < shut + 2; ++k)
                    {
                        if (text[k] == '\n')
                        {
                            return false; // 指令会溢出行外
                        }
                    }
                    i = shut + 2;
                    continue;
                }
                return false; // 指令行上有真实的跟随代码
            }
            return true; // 输入结束即行结束
        }

        /**
         * @brief 词法扫描共享核心：代码/行注释/块注释/字符串/字符/原始字符串
         *        六状态状态机 + 行首追踪 + 动作回调。
         *
         * export_overlay 与 scan_import_decls 的同源扫描核心：状态转移、
         * 行拼接（反斜杠 + 换行）、原始字符串 opener 识别与 line_start
         * （上个换行以来仅空白/注释）维护全部内聚于此；两个调用方的口径
         * 差异经回调显式保留。
         *
         * 回调协议（Hooks 按需实现，无动作的写空函数体）：
         * - on_newline()：物理换行处（Code/Line/Str/Chr 状态的换行字符）。
         *   export 侧借此结束 directive 追踪；import 侧无动作。块注释与
         *   原始字符串内的换行不触发（预处理逻辑行尚未结束）。
         * - on_hash(i)：行首 '#'。返回 nullopt = 核心逐字符继续（export 侧
         *   借此开启 directive 追踪——指令行内的 export 交给预处理器展开，
         *   绝不消隐）；返回位置 = 整条预处理逻辑行（含拼接）跳到该处，
         *   换行字符留给核心统一处理（import 侧：指令行内不可能有 import
         *   声明，且 '#import `<x.tlb>`' 绝不能匹配）。
         * - on_identifier(i, j, opens_line)：标识符 token [i, j)（原始字符串
         *   前缀已被核心消化，绝不抵达此处；找不到左括号的前缀同样回落到
         *   此处）。opens_line 是该 token 之前的 line_start 值。返回
         *   nullopt = 核心从 j 继续；返回位置 = 从该处继续（import 侧声明
         *   定界成功后跳到终结 ';' 之后一格）。
         * - on_other_char()：标识符与 '#' 之外的代码字符（含未构成拼接的
         *   反斜杠）。import 侧借此放弃挂起的 export 记号。
         *
         * @internal
         * 状态机细节：拼接的行注释在下一行继续；块注释内的换行不构成行首
         * （其块注释结束符尚在其后）；未终结的字面量在行尾恢复。原始字符串
         * opener（R"(、u8R"(、LR"( …）对扫描只是标识符加引号——识别后走
         * 原始字面量路径。编码消毒（scrub_source_encoding）不走此核心：
         * 它是逐字节重写器（GBK 双字节配对、unsigned 口径、原始字符串经
         * 标识符尾巴滑窗识别），口径差异以独立实现显式保留。
         */
        template <typename Hooks>
        auto lex_scan(std::string_view text, Hooks& hooks) -> void
        {
            const std::size_t n = text.size();
            enum class St : std::uint8_t
            {
                Code,
                Line,
                Block,
                Str,
                Chr,
                Raw
            };
            St          st = St::Code;
            std::size_t i = 0;
            bool        line_start = true; // 上个换行以来仅空白
            std::string raw_tail; // 结束原始字符串的 ")" + 定界符 + "\""

            while (i < n)
            {
                const char c = text[i];
                switch (st)
                {
                case St::Line:
                    if (c == '\\')
                    { // 拼接的行注释续到下一行
                        if (i + 1 < n && text[i + 1] == '\n')
                        {
                            i += 2;
                            continue;
                        }
                        if (i + 2 < n && text[i + 1] == '\r'
                            && text[i + 2] == '\n')
                        {
                            i += 3;
                            continue;
                        }
                    }
                    if (c == '\n')
                    {
                        st = St::Code;
                        line_start = true;
                        hooks.on_newline();
                    }
                    ++i;
                    continue;
                case St::Block:
                    if (c == '*' && i + 1 < n && text[i + 1] == '/')
                    {
                        st = St::Code;
                        i += 2;
                        continue;
                    }
                    ++i; // 块内换行不算行首：其后还有块注释结束符
                    continue;
                case St::Str:
                case St::Chr:
                {
                    const char close = st == St::Str ? '"' : '\'';
                    if (c == '\\')
                    {
                        i += 2;
                        continue;
                    }
                    if (c == close)
                    {
                        st = St::Code;
                    }
                    if (c == '\n')
                    { // 未终结字面量：行尾恢复
                        st = St::Code;
                        line_start = true;
                        hooks.on_newline();
                    }
                    ++i;
                    continue;
                }
                case St::Raw:
                    if (text.compare(i, raw_tail.size(), raw_tail) == 0)
                    {
                        st = St::Code;
                        i += raw_tail.size();
                        continue;
                    }
                    ++i;
                    continue;
                case St::Code:
                    if (c == '\\')
                    { // 行拼接：同一逻辑行继续
                        if (i + 1 < n && text[i + 1] == '\n')
                        {
                            i += 2;
                            continue;
                        }
                        if (i + 2 < n && text[i + 1] == '\r'
                            && text[i + 2] == '\n')
                        {
                            i += 3;
                            continue;
                        }
                    }
                    if (c == '\n')
                    {
                        line_start = true;
                        hooks.on_newline();
                        ++i;
                        continue;
                    }
                    if (is_space_char(c))
                    {
                        ++i;
                        continue;
                    }
                    if (c == '/' && i + 1 < n && text[i + 1] == '/')
                    {
                        st = St::Line;
                        i += 2;
                        continue;
                    }
                    if (c == '/' && i + 1 < n && text[i + 1] == '*')
                    {
                        st = St::Block;
                        line_start = false;
                        i += 2;
                        continue;
                    }
                    if (c == '"')
                    {
                        st = St::Str;
                        line_start = false;
                        ++i;
                        continue;
                    }
                    if (c == '\'')
                    {
                        st = St::Chr;
                        line_start = false;
                        ++i;
                        continue;
                    }
                    if (c == '#' && line_start)
                    {
                        if (const std::optional<std::size_t> skip =
                                hooks.on_hash(i))
                        {
                            i = *skip; // 指令整行吞掉；换行留给下轮统一收尾
                        }
                        else
                        {
                            line_start = false;
                            ++i;
                        }
                        continue;
                    }
                    if (is_ident_char(c))
                    {
                        std::size_t j = i;
                        while (j < n && is_ident_char(text[j]))
                        {
                            ++j;
                        }
                        const std::string_view word{text.data() + i, j - i};
                        if (j < n && text[j] == '"'
                            && (word == "R" || word == "LR" || word == "uR"
                                || word == "UR" || word == "u8R"))
                        {
                            const std::size_t open = text.find('(', j);
                            if (open != std::string_view::npos)
                            {
                                raw_tail = ")"
                                           + std::string{text.substr(
                                               j + 1,
                                               open - (j + 1))}
                                           + "\"";
                                st = St::Raw;
                                line_start = false;
                                i = open + 1;
                                continue;
                            }
                        }
                        const bool opens_line = line_start;
                        line_start = false;
                        if (const std::optional<std::size_t> resume =
                                hooks.on_identifier(i, j, opens_line))
                        {
                            i = *resume; // 声明被整段消费：从其终点继续
                        }
                        else
                        {
                            i = j;
                        }
                        continue;
                    }
                    hooks.on_other_char();
                    line_start = false;
                    ++i;
                    continue;
                }
            }
        }

        /**
         * @brief export_overlay 的扫描回调：预处理指令行追踪 + export 消隐
         *        动作。
         */
        struct ExportOverlayHooks
        {
            std::string* out; // 就地空格消隐的落点（与扫描视图同源）
            bool         directive = false; // 处于 # 引入的逻辑行内

            void on_newline() { directive = false; }

            auto on_hash(std::size_t) -> std::optional<std::size_t>
            {
                directive = true;
                return std::nullopt;
            }

            auto on_identifier(std::size_t i, std::size_t j, bool)
                -> std::optional<std::size_t>
            {
                const std::string_view word{out->data() + i, j - i};
                if (word == "export" && !directive)
                {
                    const std::optional<std::string_view> next =
                        next_identifier(
                            std::string_view{out->data(), out->size()},
                            j);
                    const bool keep = !next.has_value() || next->empty()
                                      || *next == "module" || *next == "import";
                    if (!keep)
                    {
                        for (std::size_t b = i; b < j; ++b)
                        {
                            (*out)[b] = ' ';
                        }
                    }
                }
                return std::nullopt;
            }

            void on_other_char() {}
        };

    } // namespace detail

    /**
     * @brief 模块翻译单元的同宽 export 消隐覆盖。
     *
     * libclang 把 `export namespace X { … }`、`export { … }` 与逐声明
     * `export` 之下的一切包进一个 ExportDecl 游标，并把它呈现为单一的
     * 无可访问子节点的 UnexposedDecl（上游缺口）——这些文件无论解析多
     * 干净都产出零符号。把六个关键字字符替换为空格在剥掉包裹的同时
     * 保住每个行列，使抽取的位置与磁盘文件精确一致。
     *
     * @internal
     * 绝不消隐：`export module`/`export import`（模块身份与再导出语义
     * 骑在其上；跟随 token 跨空白、换行与注释识别）、`export { … }`
     * （命名空间作用域的裸花括号块是非良构 C++——消隐会破坏解析）、
     * 字符串与字符字面量（含原始字符串）、注释、仅"包含"export 字样的
     * 标识符、以及预处理指令行（`#define E export` 必须继续展开）。
     * 调用方仅在模块模式解析上启用（cg::clang::is_module_tu）；经典 TU
     * 绝不见此覆盖。保留不消隐的判定：EOF（无可解锁）、
     * `module`/`import`（身份/再导出语义）、任何非标识符跟随者——尤其
     * `export { … }`，命名空间作用域的裸花括号块是非良构 C++，消隐直接
     * 破坏解析（实测：clang 应答 "expected unqualified-id"）。导出块保持
     * 包裹（其符号仍隐藏）——语法安全的子集 {export namespace、逐声明
     * export …} 才是解锁产出量的部分。词法状态机与
     * scan_import_decls 共享 detail::lex_scan；本侧差异（directive 追踪
     * 与消隐动作）见 detail::ExportOverlayHooks。
     */
    [[nodiscard]]
    auto export_overlay(std::string_view src) -> std::string
    {
        std::string                out{src};
        detail::ExportOverlayHooks hooks{.out = &out};
        detail::lex_scan(std::string_view{out.data(), out.size()}, hooks);
        return out;
    }

    /**
     * @brief 整读一个文件为二进制文本；读不到时返回 ""。
     *
     * @internal 文件消失时解析自身会响亮失败——覆盖是可选项。实现收编进
     * cg::util::read_text_file（"" 与 nullopt 的口径差异在此折叠）。
     */
    [[nodiscard]]
    auto read_source(const std::string& file) -> std::string
    {
        return cg::util::read_text_file(file).value_or(std::string{});
    }

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
     * 被消隐的 import 仍经 `replaced` 上报。`replaced`（非空时）按文件
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
    [[nodiscard]]
    auto import_rewrite_overlay(
        std::string_view             src,
        bool                         std_umbrella,
        bool                         header_unit_includes,
        bool                         demodule = false,
        std::vector<std::string>*    replaced = nullptr,
        std::span<const std::string> dropped_modules = {}) -> std::string
    {
        if (!std_umbrella && !header_unit_includes && !demodule
            && dropped_modules.empty())
        {
            return std::string{src};
        }
        std::string out{src};
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
                    if (replaced != nullptr && !replaced_name.empty())
                    {
                        replaced->push_back(std::move(replaced_name));
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
        return out;
    }

    namespace detail
    {
        [[nodiscard]]
        auto is_cxx_tu(std::string_view file, std::span<const std::string> argv)
            -> bool
        {
            for (const std::string& a : argv)
            {
                if (a.starts_with("-std=c++") || a.starts_with("-std=gnu++"))
                {
                    return true;
                }
            }
            constexpr std::string_view cxx_ext[] =
                {".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".ipp"};
            for (const std::string_view ext : cxx_ext)
            {
                if (file.ends_with(ext))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 某个 -I/-isystem/-iquote/-idirafter 条目（分写或连写 -I 形式）
         * 是否精确解析到 dir。
         *
         * @internal 此处子串测试是错的：记录的 "-I `<toolchain>`/include/
         * `<vendor>/<product>`"（恰好住在该前缀下的无关产品树）不得满足系统
         * "`<toolchain>`/include" 根而压制补充项（参考语料实测：每个这样的 TU
         * 随后都在 Windows.h / memcmp 上失败）。
         */
        [[nodiscard]]
        auto argv_has_include_dir(
            std::span<const std::string> argv,
            std::string_view             dir) -> bool
        {
            namespace fs = std::filesystem;
            const auto equal_path = [](std::string_view a, std::string_view b)
            { return cg::util::ascii_iequals(a, b); };
            const std::string want = [&]
            {
                std::error_code ec;
                return fs::path{std::string{dir}}
                    .lexically_normal()
                    .generic_string();
            }();
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                std::optional<std::string> value;
                if ((argv[i] == "-I" || argv[i] == "-isystem"
                     || argv[i] == "-iquote" || argv[i] == "-idirafter")
                    && i + 1 < argv.size())
                {
                    value = argv[++i];
                }
                else if (argv[i].starts_with("-I") && argv[i].size() > 2)
                {
                    value = argv[i].substr(2);
                }
                if (!value.has_value())
                {
                    continue;
                }
                std::error_code ec;
                if (equal_path(
                        fs::path{*value}.lexically_normal().generic_string(),
                        want))
                {
                    return true;
                }
            }
            return false;
        }

    } // namespace detail

    /**
     * @brief 记录的编译器驱动器隐式添加、而 libclang 不会添加的编译器
     * 开关：资源目录加驱动器自身的系统 include 根，按驱动器报告的顺序
     * （cg.clang.driver 讯问；本工具链上 libc++ 在前、其后是 mingw-w64
     * 的 C 集）。
     *
     * 资源目录的 INCLUDE 条目被跳过——-resource-dir flag 已覆盖；argv
     * 已有的 flag 绝不重复。
     *
     * @internal
     * 经参考语料重放冒烟运行发现。cl 模式门控：clang-cl 模式的重放绝不
     * 可被追加驱动器讯问来的 GNU 系统 include 根——clang-cl 把 -isystem
     * 当 GNU 选项忽略、其值变成孤儿位置输入、解析以
     * CXError_ASTReadError 失败（实证 v3/v4/v5）；改为把这些目录预置成
     * -I 又会让 libc++ 遮蔽 MSVC STL 而解析失败（v6）。clang-cl 自行
     * 找到 MSVC 工具链（实证 s7），故什么都不需要补；上面的
     * -resource-dir 不设门控——cl 模式也接受它（实证 v2_res）。
     * 自带 stdlib 布线的 TU 携带 -nostdinc++ 加自己的 -isystem stdlib 集
     * （libstdc++ 布线：3×-isystem + -nostdinc++ 是唯一无泄漏的接线）：
     * 它们自供 C++ 标准库，引擎自己的 stdlib 根（对 C++ 报告但对 C 不
     * 报告的那些目录）绝不可追加其后——libc++ 也带 stddef.h/__config，
     * libc++ 接线的引擎上 libstdc++ 布线的解析会经乘积撞上两个 stdlib
     * （实测 10 错误冲突；原生 gnu 布线引擎不受影响：其根已在 argv 上、
     * 在下方去重）。C 根保留：它们不是 stdlib、布线依赖它们。仅当 C
     * 报告确切给出 stdlib 专属集时跳过；空 C 报告保持历史追加行为。
     */
    [[nodiscard]]
    auto driver_supplement(
        const std::string&              file,
        const std::vector<std::string>& argv) -> std::vector<std::string>
    {
        const DriverInfo& info = driver_info();
        if (info.driver.empty())
        {
            throw std::runtime_error(
                "ccg: no clang driver executable found (looked beside the "
                "loaded LLVM engine library and under its installation prefix, "
                "then via CCG_LLVM_ROOT, then on PATH). Set CCG_LLVM_ROOT, pass "
                "`ccg index --llvm-root <prefix>`, or configure `ccg config set "
                "llvm-path <prefix>` to name the LLVM installation containing "
                "clang++/clang.");
        }
        std::vector<std::string> extra;
        if (!builtin_resource_dir().empty()
            && std::ranges::find(argv, "-resource-dir") == argv.end())
        {
            extra.emplace_back("-resource-dir");
            extra.push_back(builtin_resource_dir());
        }
        const bool cl_mode =
            std::ranges::find(argv, "--driver-mode=cl") != argv.end();
        if (!cl_mode)
        {
            const std::string resource_include =
                info.resource_dir.empty()
                    ? std::string{}
                    : cg::util::norm_path(
                          (std::filesystem::path{info.resource_dir} / "include")
                              .generic_string());
            const bool cxx_tu = detail::is_cxx_tu(file, argv);
            const std::vector<std::string>& reported =
                cxx_tu ? info.cxx_include_dirs : info.c_include_dirs;
            const bool rewired_stdlib =
                cxx_tu && !info.c_include_dirs.empty()
                && std::ranges::find(argv, "-nostdinc++") != argv.end();
            for (const std::string& dir : reported)
            {
                if (!resource_include.empty() && dir == resource_include)
                {
                    continue; // 由上面的 -resource-dir flag 承载
                }
                if (rewired_stdlib
                    && std::ranges::find(info.c_include_dirs, dir)
                           == info.c_include_dirs.end())
                {
                    continue; // 引擎的 stdlib 根，被 TU 的 -nostdinc++ 布线排除
                }
                if (!detail::argv_has_include_dir(argv, dir))
                {
                    extra.emplace_back("-isystem");
                    extra.push_back(dir);
                }
            }
        }
        return extra;
    }

    /**
     * @brief 解析一个翻译单元的结果。
     *
     * @internal parse() 返回时 `tu` 总是有效：可恢复诊断在 `errors` 中，
     * 部分恢复的游标树仍可走查。dialect_fixit_* 三字段记录诊断驱动的
     * 内存源码改写（见 detail::rewrite_lenient_specializations）是否
     * 发生：applied 表示确有一次重解析；改写后 errors 即重解析的诊断
     * （一次重解析上限，绝不二轮）。encoding_scrub_* 三字段同构，记录
     * ANSI 编码消毒（见 detail::rewrite_illegal_encoding；字符串/字符
     * 字面量内系统 ANSI 码页不可解码的字节转八进制转义后重解析一次，
     * 消毒先于 FixIt 改写执行，两者的内存基座链式叠加）。
     */
    struct ParsedTu
    {
        Tu                      tu;
        std::vector<Diagnostic> errors;
        bool                    dialect_fixit_applied = false;
        std::int64_t dialect_fixit_edits = 0;         // 实际应用的 FixIt 条数
        std::vector<std::string> dialect_fixit_files; // 被改写的文件（排序）
        bool                     encoding_scrub_applied = false;
        std::int64_t             encoding_scrub_edits = 0; // 消毒的编码单元数
        std::vector<std::string> encoding_scrub_files; // 被消毒的文件（排序）
    };

    namespace detail
    {

        /**
         * @brief argv 里的目标三元组（--target=<t> 连写与 -target/--target
         * 分写形态，与装载层 argv 值解析同款逐 token 扫描）。
         *
         * @internal 后写覆盖（驱动器语义：靠后的 -target 生效）。分写
         * 守卫：下一 token 以 '-' 开头视为旗标而非值（三元组永不以 '-'
         * 开头），裸开关绝不吞它。精确 token 匹配使 -target-cpu 之类的
         * 同前缀旗标不误配。
         */
        [[nodiscard]]
        auto argv_target_triple(const std::vector<std::string>& argv)
            -> std::string
        {
            std::string found;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& t = argv[i];
                if ((t == "-target" || t == "--target") && i + 1 < argv.size()
                    && !argv[i + 1].starts_with('-'))
                {
                    found = argv[++i];
                }
                else if (
                    t.starts_with("--target=") || t.starts_with("-target="))
                {
                    found = t.substr(t.find('=') + 1);
                }
            }
            return found;
        }

        /**
         * @brief 实例化跳过（TU_Prefix 形态）对该 argv 是否生效。
         *
         * @internal 环境变量值与 argv 都由调用方递入（纯函数，可单测）：
         * - CCG_SKIP_INSTANTIATIONS 精确值 "1" 才开（默认关=解析行为
         *   逐字节不变；与 CCG_HEADCACHE 同一显式开形态）。
         * - cl 方言条目（argv 含 --driver-mode=cl，装载/翻译层保证 cl
         *   家族条目带该旗标）永久禁用：原生 cl 条目装载期补
         *   /Zc:twoPhase-、clang-cl 在 MSVC target 低于 C++20 时驱动
         *   默认开延迟模板解析——延迟语料下 TU_Prefix 形态不 parse
         *   模板 pattern 体（LateParsed 实例化被转移且不再解析），模板
         *   体内符号与依赖调用边全丢，开关对该方言无效。
         * - 显式 -fdelayed-template-parsing（GNU 方言拼写）机制同上，
         *   一并禁用。
         * - 目标三元组为 *-windows-msvc 的 GNU 拼写条目一并禁用：驱动器
         *   对该目标在低于 C++20 时自动注入 -fdelayed-template-parsing
         *   （注入发生在 cc1 命令行装配期，argv 层不可见）。判据与标准
         *   版本无关——凡 msvc 三元组即禁用（免解析 -std 版本序，误禁
         *   方向安全：保真优先）；mingw 的 *-windows-gnu 三元组无此注入，
         *   不受影响。
         */
        [[nodiscard]]
        auto instantiation_skip_allowed(
            const std::optional<std::string>& env_value,
            const std::vector<std::string>&   argv) -> bool
        {
            if (env_value != std::optional<std::string>{"1"})
            {
                return false;
            }
            const std::string          triple = argv_target_triple(argv);
            constexpr std::string_view msvc_suffix{"-windows-msvc"};
            const bool                 msvc_target =
                triple.size() >= msvc_suffix.size()
                && cg::util::ascii_iequals(
                    std::string_view{triple}.substr(
                        triple.size() - msvc_suffix.size()),
                    msvc_suffix);
            return !msvc_target
                   && std::ranges::none_of(
                       argv,
                       [](const std::string& t)
                       {
                           return t == "--driver-mode=cl"
                                  || t == "-fdelayed-template-parsing";
                       });
        }

        /**
         * @brief 诊断驱动的内存源码改写：cl 方言的宽松特化家族。
         *
         * 形态：MSVC 接受、clang 报 "template specialization requires
         * 'template<>'" 的类外成员特化定义（如 `__declspec(selectany)
         * Widget Singleton<T>::mutex_;`）。clang 该诊断自带
         * FixItHint（在声明头插入 "template<> "，插入点与文本全由编译器给出）。
         *
         * 触发条件三合一，缺一零动作：仅 cl 方言（argv 含 --driver-mode=cl，
         * GNU 方言零改动）；首轮解析的诊断里含该拼写且该诊断带 FixIt
         * （绝不自己发明插入点）；FixIt 缺失时零动作。改写在内存源码缓冲按
         * FixIt 定点插入（unsaved buffer 管线，磁盘文件绝不修改），重解析
         * 一次；一次重解析后仍失败则如实保留新诊断（失败口径由调用方分类，
         * 不做二轮）。重解析硬失败（ParseError）时保留首轮 TU 与诊断。
         *
         * @internal 缓冲基座是覆盖感知的：FixIt 所在文件若已有内存覆盖
         * （export 消隐等），偏移以覆盖后的文本为准，改写叠在其上；否则
         * 从磁盘整读。同一文件的多条 FixIt 按偏移降序应用（先改后段、先段
         * 偏移保持有效），与已应用编辑重叠或越界的条目被丢弃。被改写文件
         * 的原覆盖条目被替换，未触及文件的覆盖原样保留。
         */
        void rewrite_lenient_specializations(
            ParsedTu&                       out,
            const Index&                    index,
            const std::string&              file,
            const std::vector<std::string>& argv,
            const std::vector<std::string>& replay,
            const std::vector<UnsavedFile>& overlay,
            bool                            detailed,
            std::size_t                     diag_cap)
        {
            if (std::ranges::find(argv, "--driver-mode=cl") == argv.end())
            {
                return; // 仅 cl 方言；GNU 方言零改动
            }
            constexpr std::string_view shape =
                "template specialization requires";
            std::vector<const FixIt*> edits;
            const auto same_edit = [](const FixIt& a, const FixIt& b)
            {
                return a.file == b.file && a.begin == b.begin && a.end == b.end
                       && a.replacement == b.replacement;
            };
            for (const Diagnostic& d : out.errors)
            {
                if (d.spelling.find(shape) == std::string::npos)
                {
                    continue;
                }
                for (const FixIt& f : d.fixits)
                {
                    if (std::ranges::none_of(
                            edits,
                            [&](const FixIt* seen)
                            { return same_edit(*seen, f); }))
                    {
                        edits.push_back(&f); // 同一位置绝不重复插入
                    }
                }
            }
            if (edits.empty())
            {
                return; // 无 FixIt：零动作（绝不发明插入点）
            }
            std::unordered_map<std::string, std::vector<const FixIt*>> by_file;
            for (const FixIt* f : edits)
            {
                by_file[f->file].push_back(f);
            }
            std::vector<UnsavedFile> covers = overlay;
            std::vector<std::string> touched;
            std::size_t              applied = 0;
            for (auto& [name, list] : by_file)
            {
                std::ranges::sort(
                    list,
                    {},
                    [](const FixIt* f) { return f->begin; });
                std::string buf;
                for (const UnsavedFile& u : overlay)
                {
                    if (u.filename == name)
                    {
                        buf = u.contents;
                        break;
                    }
                }
                if (buf.empty())
                {
                    buf = read_source(name); // 磁盘基座（该文件尚无覆盖）
                }
                if (buf.empty())
                {
                    continue; // 读不到：该文件的 FixIt 放弃
                }
                std::size_t low = std::numeric_limits<std::size_t>::max();
                std::size_t used = 0;
                for (auto it = list.rbegin(); it != list.rend(); ++it)
                {
                    const FixIt& f = **it;
                    if (f.end > buf.size() || f.end > low)
                    {
                        continue; // 越界 / 与已应用编辑重叠：丢弃该条
                    }
                    buf.replace(f.begin, f.end - f.begin, f.replacement);
                    low = f.begin;
                    ++used;
                }
                if (used == 0)
                {
                    continue;
                }
                applied += used;
                std::erase_if(
                    covers,
                    [&](const UnsavedFile& u) { return u.filename == name; });
                covers.push_back(
                    UnsavedFile{.filename = name, .contents = std::move(buf)});
                touched.push_back(name);
            }
            if (applied == 0)
            {
                return;
            }
            try
            {
                // 与首轮同一 replay 上独立求值同一纯判定（方言门内一致）。
                Tu retried = index.parse(
                    file,
                    replay,
                    covers,
                    detailed,
                    instantiation_skip_allowed(
                        cg::util::cstd::get_env("CCG_SKIP_INSTANTIATIONS"),
                        replay));
                out.tu = std::move(retried);
                out.errors = out.tu.diagnostics(DiagSeverity::Error, diag_cap);
            }
            catch (const ParseError&)
            {
                // 重解析硬失败：保留首轮 TU 与诊断，按原失败口径如实记录
            }
            out.dialect_fixit_applied = true;
            out.dialect_fixit_edits = static_cast<std::int64_t>(applied);
            std::ranges::sort(touched);
            out.dialect_fixit_files = std::move(touched);
        }

        /**
         * @brief 把单个字节追加为三数字八进制转义（如 0xF5 -> "\365"）。
         *
         * @internal 八进制而非十六进制：十六进制转义贪婪消费后续十六进制
         * 字符，"\xF5""\xFB" 相邻必须拆串，而八进制转义至多三数字、天然
         * 不与后随转义粘连，且首字符（0-3）限定单字节值域。
         */
        void append_octal_escape(std::string& out, unsigned char byte)
        {
            out.push_back('\\');
            out.push_back(static_cast<char>('0' + ((byte >> 6) & 0x7)));
            out.push_back(static_cast<char>('0' + ((byte >> 3) & 0x7)));
            out.push_back(static_cast<char>('0' + (byte & 0x7)));
        }

        /**
         * @brief 源码字节的 ANSI 编码消毒：字符串/字符字面量内的一切高位
         *        字节（>= 0x80，按编码单元）改写为八进制转义，其余区域逐
         *        字节保留。
         *
         * 背景：MSVC 对源文件里的系统 ANSI 码页序列（含未分配码点，如 GBK
         * 的 0xF5 0xFB）在字面量中按原样透传、编译通过；clang 家族按输入字
         * 符集严格解码，宽/Unicode 字面量遇无法映射的序列上报 "illegal
         * character encoding" 硬错误、整 TU 失败。消毒把字面量内容的高位
         * 字节变为等值转义，解析即可通过。
         *
         * @internal 为何一律转义而不查"合法对"：引擎的码页分配表与系统
         * CP936 不一致（Windows API 判 0xF5FB 可解码、clang 的表判非法，
         * 实测分歧），任何一侧的表都无法充当引擎口径；而字面量内容的
         * 确切字节值对索引语义零影响（符号/边/包含都不读字符串内容），
         * 全量转义是唯一口径自洽且无表依赖的做法。消毒只在诊断已点名
         * 该 TU（或引擎硬失败重试）时运行，正常文件零改动。GBK 双字节
         * 结构区（次字节 0x40-0xFE 且非 0x7F）构成一个编码单元、两字节
         * 同转——拆开会让次高位字节与后随字节错误配对。
         *
         * 词法边界：行注释、块注释、原始字符串内的字节一律不动（注释无
         * 害；原始字符串无转义语法，改写只会破坏内容；宽原始字面量的
         * 残余坏点由失败分类器记名）。普通字符串/字符字面量内反斜杠转义
         * 跳两字节（已写就的转义原样保留）。
         *
         * @param src 源文本（字节精确）
         * @param edits 可选出参：消毒的编码单元数（0 = 无改动，返回值与
         *              src 逐字节相同）
         * @return 消毒后的文本；无编辑时与输入逐字节相同
         */
        [[nodiscard]]
        auto scrub_source_encoding(
            std::string_view src,
            std::size_t*     edits = nullptr) -> std::string
        {
            if (edits != nullptr)
            {
                *edits = 0;
            }
            enum class St
            {
                normal,        // 代码区
                line_comment,  // // 行注释
                block_comment, /* 块注释 */
                raw,           // R"delim( ... )delim" 原始字符串
                str,           // 普通字符串字面量
                chr            // 字符字面量
            };
            St          st = St::normal;
            std::string raw_delim; // 原始字符串的闭合定界段（) 之后至 " 之前）
            std::string tail;      // 代码区最近标识符尾巴（原始串前缀识别）
            std::string out;
            out.reserve(src.size());
            const std::size_t n = src.size();
            std::size_t       i = 0;
            const auto        is_ident = [](unsigned char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || (c >= '0' && c <= '9') || c == '_';
            };
            while (i < n)
            {
                const unsigned char c = static_cast<unsigned char>(src[i]);
                switch (st)
                {
                case St::normal:
                    if (c == '/' && i + 1 < n && src[i + 1] == '/')
                    {
                        st = St::line_comment;
                        out.push_back(src[i]);
                        ++i;
                        tail.clear();
                    }
                    else if (c == '/' && i + 1 < n && src[i + 1] == '*')
                    {
                        st = St::block_comment;
                        out.push_back(src[i]);
                        ++i;
                        tail.clear();
                    }
                    else if (c == '"')
                    {
                        const bool raw = tail == "R" || tail == "LR"
                                         || tail == "uR" || tail == "UR"
                                         || tail == "u8R";
                        if (raw)
                        {
                            // 收集开引号与左括号之间的定界符
                            std::size_t k = i + 1;
                            std::string delim;
                            while (k < n && src[k] != '(' && src[k] != '"'
                                   && delim.size() < 16)
                            {
                                delim.push_back(src[k]);
                                ++k;
                            }
                            if (k < n && src[k] == '(')
                            {
                                raw_delim = std::move(delim);
                                st = St::raw;
                            }
                            else
                            {
                                st = St::str; // 畸形前缀：按普通串防御处理
                            }
                        }
                        else
                        {
                            st = St::str;
                        }
                        out.push_back(src[i]);
                        ++i;
                        tail.clear();
                    }
                    else if (c == '\'')
                    {
                        st = St::chr;
                        out.push_back(src[i]);
                        ++i;
                        tail.clear();
                    }
                    else
                    {
                        out.push_back(src[i]);
                        ++i;
                        if (is_ident(c))
                        {
                            if (tail.size() >= 3)
                            {
                                tail.erase(0, 1);
                            }
                            tail.push_back(static_cast<char>(c));
                        }
                        else
                        {
                            tail.clear();
                        }
                    }
                    break;
                case St::line_comment:
                    if (c == '\n')
                    {
                        st = St::normal;
                    }
                    out.push_back(src[i]);
                    ++i;
                    break;
                case St::block_comment:
                    if (c == '*' && i + 1 < n && src[i + 1] == '/')
                    {
                        out.append(src.substr(i, 2));
                        i += 2;
                        st = St::normal;
                    }
                    else
                    {
                        out.push_back(src[i]);
                        ++i;
                    }
                    break;
                case St::raw:
                    if (c == ')' && i + 1 + raw_delim.size() < n
                        && src.compare(i + 1, raw_delim.size(), raw_delim) == 0
                        && src[i + 1 + raw_delim.size()] == '"')
                    {
                        const std::size_t close =
                            i + 1 + raw_delim.size(); // 闭引号位置
                        out.append(src.substr(i, close - i + 1));
                        i = close + 1;
                        st = St::normal;
                    }
                    else
                    {
                        out.push_back(src[i]);
                        ++i;
                    }
                    break;
                case St::str:
                case St::chr:
                {
                    const char close = st == St::str ? '"' : '\'';
                    if (c == close)
                    {
                        st = St::normal;
                        out.push_back(src[i]);
                        ++i;
                    }
                    else if (c == '\\' && i + 1 < n)
                    {
                        out.append(src.substr(i, 2)); // 已有转义原样保留
                        i += 2;
                    }
                    else if (c == '\n')
                    {
                        st = St::normal; // 未闭合字面量：防御性回代码区
                        out.push_back(src[i]);
                        ++i;
                    }
                    else if (c < 0x80)
                    {
                        out.push_back(src[i]);
                        ++i;
                    }
                    else
                    {
                        // 高字节：一律转义（口径说明见函数文档）
                        if (i + 1 < n)
                        {
                            const unsigned char c2 =
                                static_cast<unsigned char>(src[i + 1]);
                            const bool pair_shape =
                                c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F;
                            if (pair_shape)
                            {
                                // 编码单元整体转义：拆开会错配出新码位
                                append_octal_escape(out, c);
                                append_octal_escape(out, c2);
                                i += 2;
                                if (edits != nullptr)
                                {
                                    ++*edits;
                                }
                                break;
                            }
                        }
                        append_octal_escape(out, c); // 孤立高字节（不成对）
                        ++i;
                        if (edits != nullptr)
                        {
                            ++*edits;
                        }
                    }
                    break;
                }
                }
            }
            return out;
        }

        /**
         * @brief 诊断驱动的编码消毒：字面量内 ANSI 不可解码字节转义后重解
         *        析一次（不落盘）。
         *
         * 触发条件三合一，缺一零动作：仅 cl 方言（argv 含 --driver-mode=cl，
         * 与宽松特化改写同门）；系统 ANSI 代码页可用（UTF-8 系统与非
         * Windows 恒零动作——那里的源解码口径不同，消毒无从谈起）；首轮
         * 诊断含 "illegal character encoding"（MSVC 透传宽容、clang 硬错
         * 的宽/Unicode 字面量坏字节）。改写只发生在内存缓冲（覆盖感知：
         * 已有覆盖条目做基座、改写后替换该条目），重解析一次；一次重解析
         * 后仍失败则如实保留新诊断（残余场景由失败分类器记名），重解析
         * 硬失败（ParseError）保留首轮。诊断出现的每个文件（主文件或被含
         * 头）独立消毒；@p overlay 是活基座（进出参数），消毒更新它供后续
         * 诊断驱动改写链式叠加。
         */
        void rewrite_illegal_encoding(
            ParsedTu&                       out,
            const Index&                    index,
            const std::string&              file,
            const std::vector<std::string>& argv,
            const std::vector<std::string>& replay,
            std::vector<UnsavedFile>&       overlay,
            bool                            detailed,
            std::size_t                     diag_cap)
        {
            if (std::ranges::find(argv, "--driver-mode=cl") == argv.end())
            {
                return; // 仅 cl 方言；GNU 方言零改动
            }
            if (cg::util::platform::ansi_input_charset().empty())
            {
                return; // 无系统 ANSI 码页：消毒口径不成立
            }
            std::vector<std::string> targets;
            for (const Diagnostic& d : out.errors)
            {
                if (d.spelling.find("illegal character encoding")
                        == std::string::npos
                    || d.location.file.empty())
                {
                    continue;
                }
                if (std::ranges::find(targets, d.location.file)
                    == targets.end())
                {
                    targets.push_back(d.location.file);
                }
            }
            if (targets.empty())
            {
                return;
            }
            std::size_t              applied = 0;
            std::vector<std::string> touched;
            for (const std::string& name : targets)
            {
                std::string buf;
                for (const UnsavedFile& u : overlay)
                {
                    if (u.filename == name)
                    {
                        buf = u.contents;
                        break;
                    }
                }
                if (buf.empty())
                {
                    buf = read_source(name); // 磁盘基座（该文件尚无覆盖）
                }
                if (buf.empty())
                {
                    continue; // 读不到：该文件的消毒放弃
                }
                std::size_t edits = 0;
                std::string scrubbed = scrub_source_encoding(buf, &edits);
                if (edits == 0)
                {
                    continue; // 诊断指向却扫不出：零动作（绝不发明编辑）
                }
                applied += edits;
                std::erase_if(
                    overlay,
                    [&](const UnsavedFile& u) { return u.filename == name; });
                overlay.push_back(
                    UnsavedFile{
                        .filename = name,
                        .contents = std::move(scrubbed)});
                touched.push_back(name);
            }
            if (applied == 0)
            {
                return;
            }
            try
            {
                // 与首轮同一 replay 上独立求值同一纯判定（方言门内一致）。
                Tu retried = index.parse(
                    file,
                    replay,
                    overlay,
                    detailed,
                    instantiation_skip_allowed(
                        cg::util::cstd::get_env("CCG_SKIP_INSTANTIATIONS"),
                        replay));
                out.tu = std::move(retried);
                out.errors = out.tu.diagnostics(DiagSeverity::Error, diag_cap);
            }
            catch (const ParseError&)
            {
                // 重解析硬失败：保留首轮 TU 与诊断，按原失败口径如实记录
            }
            out.encoding_scrub_applied = true;
            out.encoding_scrub_edits = static_cast<std::int64_t>(applied);
            std::ranges::sort(touched);
            out.encoding_scrub_files = std::move(touched);
        }

    } // namespace detail

    /**
     * @brief 以 argv 编译器开关（无 argv[0]）解析 file（绝对或相对进程
     * 工作目录）。
     *
     * 可恢复诊断绝不抛出；仅硬索引/解析失败抛 std::runtime_error。追加
     * driver_supplement，使真实编译数据库条目的解析与其记录的编译器所
     * 见完全一致。
     *
     * @internal
     * `module_tu`（编译数据库条目的 cg::clang::is_module_tu）把上述 export
     * 消隐覆盖作为 unsaved-file 覆盖应用：libclang 把 `export namespace`/
     * `export {}`/逐声明 `export` 下的一切藏进不可访问的 UnexposedDecl，
     * 没有覆盖时这些文件产出零符号。经典 TU 走默认值、按字节精确解析。
     * export 消隐只骑模块模式解析；import 改写（混合 TU 伞头、头单元
     * 降级）也可以命中经典消费者 TU，故不被 module_tu 门控。诊断收集
     * 上限 8 条——只有第一条错误拼写会进失败报告；CCG_DEBUG_DIAGS 选择
     * 回完整列表。首轮诊断命中 "illegal character encoding"（宽/Unicode
     * 字面量内的系统 ANSI 码页坏字节）时，经 detail::rewrite_illegal_
     * encoding 把字面量内不可解码字节转八进制转义后重解析一次（不落
     * 盘、一次上限、仅 cl 方言）；首轮诊断命中 cl 方言宽松特化家族且带
     * FixIt 时，经 detail::rewrite_lenient_specializations 做一次诊断
     * 驱动的内存源码改写并重解析（不落盘、一次上限），其基座为消毒后
     * 的覆盖表（两机制链式叠加、各自一次重解析上限）。
     */
    [[nodiscard]]
    auto parse(
        const Index&                    index,
        const std::string&              file,
        const std::vector<std::string>& argv,
        bool                            module_tu = false,
        const ModuleOverlay*            module_overlay = nullptr,
        bool                            degraded = false) -> ParsedTu
    {
        ParsedTu                 out;
        std::vector<UnsavedFile> overlay;
        const bool rewrite = module_overlay != nullptr
                             && (module_overlay->std_umbrella
                                 || module_overlay->header_unit_includes
                                 || module_overlay->demodule
                                 || !module_overlay->dropped_modules.empty());
        if (module_tu || rewrite)
        {
            const std::string src = read_source(file);
            if (!src.empty())
            {
                std::string blanked = module_tu ? export_overlay(src) : src;
                if (rewrite)
                {
                    blanked = import_rewrite_overlay(
                        blanked,
                        module_overlay->std_umbrella,
                        module_overlay->header_unit_includes,
                        module_overlay->demodule,
                        nullptr,
                        module_overlay->dropped_modules);
                }
                if (blanked != src)
                {
                    overlay.push_back(
                        UnsavedFile{
                            .filename = file,
                            .contents = std::move(blanked)});
                }
            }
        }
        std::vector<std::string> extra = driver_supplement(file, argv);
        // 记录 argv 之后的告警策略（绝不写回 cc.argv / parse_digest 喂进的
        // 摘要，否则每个指纹都会因引擎侧策略而失配）：
        // -Wno-nonportable-include-path / -Wno-nonportable-system-include-path
        // 压掉 include 路径大小写告警；在 -Werror 下这些告警会升级成错误，
        // 让本可恢复的 TU 整只失败（参考语料 11 个 TU 的实测根因之一）。两个
        // 开关在 GNU 与 --driver-mode=cl 两种方言下都被 libclang 的选项
        // 解析器接受，故不需要方言分支。-Wno-unknown-warning-option 放在
        // 二者之后：旧 clang 不认识 nonportable 开关时会发 unknown warning
        // option，-Werror 下那本身又是一条错误；该开关让未知 -Wno-* 静默。
        extra.emplace_back("-Wno-nonportable-include-path");
        extra.emplace_back("-Wno-nonportable-system-include-path");
        extra.emplace_back("-Wno-unknown-warning-option");
        // non-pod-varargs 是 clang 的 DefaultError（与 -Werror 无关）：非平凡
        // 类型传 varargs 的日志宏在 MSVC 下完全合法（ATL/日志上抛形态），
        // 索引语境只需把错误升级降回警告——警告本体保留（诊断只收集
        // Error 级，符号提取不受影响），两个方言都接受该开关。
        extra.emplace_back("-Wno-error=non-pod-varargs");
        // cl 方言在末尾追加 /WX-（后写胜出）：/WX 会把 libclang 解析入口
        // 固定追加的 -fno-spell-checking 触发的 unknown-argument 驱动警告
        // 提升成错误，压掉一层后下一层（argument unused during
        // compilation: '/c'）还会冒——逐警告 -Wno-* 打地鼠不可取，一次性
        // 解除全部升级。无 /WX 时 /WX- 是 no-op；GNU 方言不接受 /WX-，故
        // 严格门控在 cl 方言（loader/翻译层已保证 cl 条目带
        // --driver-mode=cl）。
        if (std::ranges::find(argv, "--driver-mode=cl") != argv.end())
        {
            extra.emplace_back("/WX-");
        }
        std::vector<std::string> replay = argv;
        replay.insert(replay.end(), extra.begin(), extra.end());
        // 引擎解析 + 编码感知的硬失败重试：宽/Unicode 字面量内的系统
        // ANSI 坏字节让 libclang 直接拒绝建 TU（ParseError，无诊断可看），
        // 此时盲扫主文件——字面量内确有坏点才消毒重试一次，否则原样
        // 重抛（其余硬失败口径不变）。诊断驱动的残余消毒（TU 建立、诊断
        // 点名被含头）见 detail::rewrite_illegal_encoding。
        // 实例化跳过形态按本 replay 判定一次：首轮与编码消毒重试共用；
        // 下方两个 detail 改写器的重试在同一 replay 上独立求值同一
        // 纯函数，形态必然一致。
        const bool skip_instantiations = detail::instantiation_skip_allowed(
            cg::util::cstd::get_env("CCG_SKIP_INSTANTIATIONS"),
            replay);
        std::exception_ptr first_failure;
        const auto         engine_parse = [&]() -> bool
        {
            try
            {
                out.tu = index.parse(
                    file,
                    replay,
                    overlay,
                    !degraded,
                    skip_instantiations);
                return true;
            }
            catch (const ParseError&)
            {
                if (!first_failure)
                {
                    first_failure = std::current_exception();
                }
                return false;
            }
        };
        bool parsed_ok = engine_parse();
        if (!parsed_ok
            && std::ranges::find(argv, "--driver-mode=cl") != argv.end()
            && !cg::util::platform::ansi_input_charset().empty())
        {
            std::string buf;
            for (const UnsavedFile& u : overlay)
            {
                if (u.filename == file)
                {
                    buf = u.contents;
                    break;
                }
            }
            if (buf.empty())
            {
                buf = read_source(file);
            }
            std::size_t edits = 0;
            std::string scrubbed = detail::scrub_source_encoding(buf, &edits);
            if (edits > 0)
            {
                std::erase_if(
                    overlay,
                    [&](const UnsavedFile& u) { return u.filename == file; });
                overlay.push_back(
                    UnsavedFile{
                        .filename = file,
                        .contents = std::move(scrubbed)});
                parsed_ok = engine_parse();
                if (parsed_ok)
                {
                    out.encoding_scrub_applied = true;
                    out.encoding_scrub_edits = static_cast<std::int64_t>(edits);
                    out.encoding_scrub_files = {file};
                }
            }
        }
        if (!parsed_ok)
        {
            std::rethrow_exception(first_failure); // 原始异常原样上抛
        }
        // 诊断上限开关进程内求值一次（每次 parse 都经过这里）；
        // 改环境变量需重启进程才生效。
        static const std::size_t diag_cap =
            cg::util::cstd::get_env("CCG_DEBUG_DIAGS").has_value() ? 0 : 8;
        out.errors = out.tu.diagnostics(DiagSeverity::Error, diag_cap, true);
        // 活基座：编码消毒（如有）就地更新覆盖表，随后的 FixIt 改写以
        // 最新基座链式叠加——编辑偏移始终对齐 clang 眼中的缓冲。
        std::vector<UnsavedFile> base = std::move(overlay);
        detail::rewrite_illegal_encoding(
            out,
            index,
            file,
            argv,
            replay,
            base,
            !degraded,
            diag_cap);
        detail::rewrite_lenient_specializations(
            out,
            index,
            file,
            argv,
            replay,
            base,
            !degraded,
            diag_cap);
        return out;
    }

    /**
     * @brief cursor 是否位于 tu 的主文件（而非被包含的头）内。
     *
     * @internal 比较经 norm_path：clang 按传入 parse() 的原样报告主文件
     * 路径，分隔符/大小写可能不同。
     */
    [[nodiscard]]
    auto in_main_file(const Tu& tu, const CXCursor& cursor) -> bool
    {
        const SourceLocation loc = location(cursor);
        if (loc.file.empty())
        {
            return false;
        }
        return cg::util::norm_path(loc.file) == cg::util::norm_path(tu.file());
    }

} // namespace cg::clang
