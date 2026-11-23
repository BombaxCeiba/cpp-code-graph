/**
 * @module cg.util.instrdigest
 *
 * @brief 指令哈希门槛的预处理指令摘要扫描器：对单个源文件字节流产出
 *        三变体（窄版/全指令/仅 include）的序敏感 sha256 摘要与计数。
 *
 * 判定语义（判定口径与实验验证用的外置 Python 探针 polib.digest_variants
 * 逐条对齐，本模块为其 C++ 内置化）：
 * - 逐行扫描；行首（剥两侧 ASCII 空白后）为 '#' 的行是指令行。
 * - 指令名 = '#' 后首个 [A-Za-z0-9_] 串，ASCII 小写折叠；空名（孤立
 *   '#'/'##'）不参与任何变体。
 * - 行内注释剥离：'//' 起截断；同行完整包含的 '\/\*...\*\/' 段以一个
 *   空格替换（跨行块注释不处理——与探针同为单行近似，样本误杀 0）。
 * - 空白规范化：任意空白串折叠为单个空格。
 * - 摘要 = 保留行（规范化文本）按 '\n' 连接后 sha256；计数 = 保留行数。
 *
 * 变体边界（三变体在 82 前后文本对上判定一致、误杀均为 0；include-only
 * 对「#define 值改动改变包含者宏表」形态必然误杀，故窄版为产品接线变体，
 * 其余两变体保留为观测对照）：
 * - narrow  ：include/define/undef/if/ifdef/ifndef/elif/elifdef/elifndef/
 *             else/endif（Rider 结构集）——对 define 值改动敏感（保守判异，
 *             只损收益不损正确性）。
 * - all     ：全部指令行（另含 pragma/line/error/warning 等）。
 * - include ：仅 include 行。
 */
export module cg.util.instrdigest;

import std;
import cg.util;

export namespace cg::util::instrdigest
{

    /** @brief 三变体摘要与计数（字段语义与验证探针 JSON 的 d_ 系与 n_ 系
     *        对齐）。 */
    struct InstrDigest
    {
        std::string  narrow;       ///< 窄版（产品接线变体）摘要
        std::string  all;          ///< 全指令摘要
        std::string  include_only; ///< 仅 include 摘要
        std::int64_t n_narrow = 0;
        std::int64_t n_all = 0;
        std::int64_t n_include = 0;
    };

    /** @brief 窄版变体收录的指令名集合判定（Rider 结构集）。 */
    [[nodiscard]]
    inline auto is_narrow_directive(std::string_view name) -> bool
    {
        static constexpr std::string_view SET[] = {
            "include",
            "define",
            "undef",
            "if",
            "ifdef",
            "ifndef",
            "elif",
            "elifdef",
            "elifndef",
            "else",
            "endif",
        };
        for (const std::string_view n : SET)
        {
            if (name == n)
            {
                return true;
            }
        }
        return false;
    }
}

namespace cg::util::instrdigest
{
    namespace detail
    {
        /** @brief 三变体的保留行累积器（all 恒收、narrow 与 include
         * 按指令名）。 */
        struct VariantLines
        {
            std::string narrow;
            std::string all;
            std::string include_only;

            void append(std::string_view name, std::string_view line)
            {
                all += line;
                all += '\n';
                if (is_narrow_directive(name))
                {
                    narrow += line;
                    narrow += '\n';
                }
                if (name == "include")
                {
                    include_only += line;
                    include_only += '\n';
                }
            }
        };
    } // namespace detail
} // namespace cg::util::instrdigest

export namespace cg::util::instrdigest
{

    /**
     * @brief 对文件字节流算三变体摘要。
     *
     * 字节级处理（编码不可知）：指令名与空白判定都在 ASCII 域内，非 ASCII
     * 字节原样进摘要文本。与探针的 utf-8/gbk 解码在摘要值上不逐位一致，
     * 但同侧自洽（摘要只与本实现自己的历史值比对），分类语义一致。
     */
    [[nodiscard]]
    inline auto digest_of(std::span<const std::byte> bytes) -> InstrDigest
    {
        const char*          data = reinterpret_cast<const char*>(bytes.data());
        const std::size_t    size = bytes.size();
        detail::VariantLines kept;
        std::size_t          i = 0;
        while (i <= size)
        {
            std::size_t end = i;
            while (end < size && data[end] != '\n' && data[end] != '\r')
            {
                ++end;
            }
            std::string_view raw{data + i, end - i};
            i = end + 1;
            if (end >= size && raw.empty())
            {
                break;
            }
            // 两侧空白剥离
            while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t'))
            {
                raw.remove_prefix(1);
            }
            while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t'))
            {
                raw.remove_suffix(1);
            }
            if (raw.empty() || raw.front() != '#')
            {
                continue;
            }
            raw.remove_prefix(1);
            while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t'))
            {
                raw.remove_prefix(1);
            }
            // 指令名
            std::size_t n = 0;
            while (n < raw.size())
            {
                const char c = raw[n];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '_')
                {
                    ++n;
                }
                else
                {
                    break;
                }
            }
            if (n == 0)
            {
                continue; // 孤立 '#'/'##'：任何变体都不收录
            }
            std::string name{raw.substr(0, n)};
            for (char& c : name)
            {
                c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            }
            // 行内注释剥离 + 空白规范化
            std::string line;
            line.reserve(raw.size());
            std::size_t p = n;
            bool        in_line_comment = false;
            while (p < raw.size())
            {
                char c = raw[p];
                if (in_line_comment)
                {
                    ++p;
                    continue;
                }
                if (c == '/' && p + 1 < raw.size() && raw[p + 1] == '/')
                {
                    break;
                }
                if (c == '/' && p + 1 < raw.size() && raw[p + 1] == '*')
                {
                    const std::size_t close = raw.find("*/", p + 2);
                    if (close == std::string_view::npos)
                    {
                        break; // 跨行块注释：截断（单行近似）
                    }
                    line += ' ';
                    p = close + 2;
                    continue;
                }
                if (c == ' ' || c == '\t' || c == '\v' || c == '\f')
                {
                    if (!line.empty() && line.back() != ' ')
                    {
                        line += ' ';
                    }
                    ++p;
                    continue;
                }
                line += c;
                ++p;
            }
            while (!line.empty() && line.back() == ' ')
            {
                line.pop_back();
            }
            kept.append(name, line);
        }
        InstrDigest out;
        out.narrow = sha256_hex(std::as_bytes(std::span{kept.narrow}));
        out.all = sha256_hex(std::as_bytes(std::span{kept.all}));
        out.include_only =
            sha256_hex(std::as_bytes(std::span{kept.include_only}));
        return out;
    }

    /** @brief 便利重载：从文件路径读取并算摘要；不可读返回 nullopt。 */
    [[nodiscard]]
    inline auto digest_of_file(const std::string& path)
        -> std::optional<InstrDigest>
    {
        const std::optional<std::string> bytes = read_text_file(path);
        if (!bytes.has_value())
        {
            return std::nullopt;
        }
        return digest_of(std::as_bytes(std::span{*bytes}));
    }

} // namespace cg::util::instrdigest
