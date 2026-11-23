

/**
 * @module cg.clang.tu.scrub
 *
 * @brief 源改写域：宽松模板特化重写、八进制转义、源编码清洗与非法编码重写。
 */
export module cg.clang.tu.scrub;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.tu.lex;
import cg.clang.tu.argv;

export namespace cg::clang
{

    namespace detail
    {

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
                            { return same_fixit(*seen, f); }))
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

    } // namespace detail

    namespace detail
    {

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
         * @return 消毒后文本与编码单元计数；无编辑时文本与输入逐字节相同
         */
        struct ScrubOutcome
        {
            std::string text;      ///< 消毒后的文本；无编辑时与输入逐字节相同
            std::size_t edits = 0; ///< 消毒的编码单元数（0 = 无改动）
        };

        [[nodiscard]]
        auto scrub_source_encoding(std::string_view src) -> ScrubOutcome
        {
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
            std::size_t       edits = 0;
            const std::size_t n = src.size();
            std::size_t       i = 0;
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
                        if (is_ident_char(src[i]))
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
                                ++edits;
                                break;
                            }
                        }
                        append_octal_escape(out, c); // 孤立高字节（不成对）
                        ++i;
                        ++edits;
                    }
                    break;
                }
                }
            }
            ScrubOutcome result;
            result.text = std::move(out);
            result.edits = edits;
            return result;
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
                ScrubOutcome scrubbed = scrub_source_encoding(buf);
                if (scrubbed.edits == 0)
                {
                    continue; // 诊断指向却扫不出：零动作（绝不发明编辑）
                }
                applied += scrubbed.edits;
                std::erase_if(
                    overlay,
                    [&](const UnsavedFile& u) { return u.filename == name; });
                overlay.push_back(
                    UnsavedFile{
                        .filename = name,
                        .contents = std::move(scrubbed.text)});
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

        /**
         * @brief 单次引擎解析尝试；ParseError 收编进首失败记忆。
         *
         * @param first_failure 首失败异常记忆（编码消毒重试阶梯共享：
         *                      仅首个失败被记录，终局原样上抛）。
         */
        [[nodiscard]]
        auto parse_once(
            ParsedTu&                       out,
            const Index&                    index,
            const std::string&              file,
            const std::vector<std::string>& replay,
            const std::vector<UnsavedFile>& overlay,
            bool                            detailed,
            bool                            skip_instantiations,
            std::exception_ptr&             first_failure) -> bool
        {
            try
            {
                out.tu = index.parse(
                    file,
                    replay,
                    overlay,
                    detailed,
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
        }

    } // namespace detail
} // namespace cg::clang
