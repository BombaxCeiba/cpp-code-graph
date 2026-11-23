

/**
 * @module cg.clang.tu.lex
 *
 * @brief
 * 源文本词法域：内置资源目录、标识符扫描器与惰性行判定、导出叠加的词法重写。
 */
export module cg.clang.tu.lex;

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
} // namespace cg::clang
