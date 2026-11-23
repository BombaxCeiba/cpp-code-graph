/**
 * @module cg.extract.visitor
 *
 * @brief 单个翻译单元 -> TuExtract。对整棵游标树（含被包含头）的一次
 *        先序走查，构建仅定义的符号记录、带延迟解析 callee 元数据的
 *        call/extends/overrides/type_alias/friend_of/specializes 边记录、
 *        以及 token 扫描的 include 记录。
 *
 * 一切以索引根允许清单为闸（仅项目文件；系统头不产出节点、边、
 * includes）。纯函数：无数据库，除 cg.clang 已持有的内容与身份规范化
 * 所需的路径拼写探测（目录列举，有缓存）外无 I/O。
 *
 * @internal
 * 走查机理：本文件自驱递归。children() 回调在处理完每个游标后返回
 * false（不下潜），每个需要子树的分支显式调用 walk_children——因此
 * 每个游标恰好被访问一次。每个符号的 scope、parent 与命名空间来自
 * 游标的语义父节点、从不来自走查位置：类外成员定义（.cpp 里的
 * Circle::area）没有可继承的词法记录上下文。
 */
export module cg.extract.visitor;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang;
import cg.extract.model;

export namespace cg::extract
{

    /**
     * @brief 抽取选项。
     *
     * @internal
     * roots（规范化绝对路径）：文件在其下即算项目代码。编译数据库加载器
     * 的根过滤器用相同的段边界语义；这些根归图构建器所有，抽取只是接收
     * 它们。
     *
     * spellings：当前编译数据库自己使用的目录拼写（每个 main 文件的祖先
     * 目录，加运行的根）：身份路径在磁盘大小写规范化之后锚回这些拼写，
     * 因此统一重拼写的数据库保持其拼写（大小写漂移契约），而同一数据库
     * 内部的混合拼写坍缩到数据库自己的拼写（见下方 anchored_at_spelling）。
     * 最长前缀优先。
     */
    struct ExtractOptions
    {
        std::vector<std::string> roots;
        std::vector<std::string> spellings;
    };

    namespace detail
    {

        /**
         * @internal
         * cg.clang 再导出的全局作用域 C 名字经 import 可达，但不是那里的命名
         * 空间成员；从全局作用域取别名（模块调研探针 m2 的测试在本工具链上
         * 证得此形态）。
         */
        using ::CXCursor;
        using ::CXCursorKind;

        [[nodiscard]]
        inline auto is_record_kind(CXCursorKind k) -> bool
        {
            switch (k)
            {
            case CXCursor_ClassDecl:
            case CXCursor_StructDecl:
            case CXCursor_UnionDecl:
            case CXCursor_ClassTemplate:
            case CXCursor_ClassTemplatePartialSpecialization:
                return true;
            default:
                return false;
            }
        }

        /**
         * @brief 作用域链种类：记录加枚举（枚举常量经其枚举进入作用域；枚举
         *        不是合法的 extends 目标，因此不在 is_record_kind 里）。
         */
        [[nodiscard]]
        inline auto is_scope_kind(CXCursorKind k) -> bool
        {
            return is_record_kind(k) || k == CXCursor_EnumDecl;
        }

        [[nodiscard]]
        inline auto is_template_param_kind(CXCursorKind k) -> bool
        {
            switch (k)
            {
            case CXCursor_TemplateTypeParameter:
            case CXCursor_NonTypeTemplateParameter:
            case CXCursor_TemplateTemplateParameter:
                return true;
            default:
                return false;
            }
        }

        /** @brief 路径段边界的根测试（与 cg.clang.compile_db 同语义）。 */
        [[nodiscard]]
        inline auto under_roots(
            const std::string&    file,
            const ExtractOptions& opt) -> bool
        {
            if (file.empty())
            {
                return false;
            }
            for (const std::string& root : opt.roots)
            {
                if (root.empty())
                {
                    continue;
                }
                if (file == root)
                {
                    return true;
                }
                if (file.starts_with(root)
                    && (root.back() == '/' || file[root.size()] == '/'))
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief 两字符在 ASCII
         * 大小写映射下折叠是否相等（与图构建器的文件身份用同一折叠）。 */
        [[nodiscard]]
        inline auto fold_eq(char a, char b) -> bool
        {
            return a == b
                   || cg::util::ascii_lower_char(a)
                          == cg::util::ascii_lower_char(b);
        }

        /**
         * @brief 用当前编译数据库自己的拼写，重写磁盘规范化路径的最长匹配
         *        已授前缀。
         *
         * @internal
         * 两个契约共享这一接缝：-I 拼写与文件字段（或磁盘）不一致的编译数据
         * 库，坍缩到每文件一个身份（unique key 内嵌路径，一个文件的两种拼写
         * 会铸造出重复子图）；统一重拼写的数据库则保持其自己的拼写（大小写
         * 漂移契约）——数据库 main 文件目录正是统一重拼写
         * 改写的东西，恢复它们让两者兼得。数据库从未授过的拼写（系统路径、
         * 纯头文件目录）保持磁盘大小写。
         *
         * 段边界判定与 under_roots 同规则：已授前缀是整个路径、以分隔符结尾、
         * 或其后一个字符是分隔符。
         */
        [[nodiscard]]
        inline auto anchored_at_spelling(
            std::string           canonical,
            const ExtractOptions& opt) -> std::string
        {
            for (const std::string& taught : opt.spellings)
            {
                if (taught.empty() || canonical.size() < taught.size())
                {
                    continue;
                }
                bool under = true;
                for (std::size_t i = 0; i < taught.size() && under; ++i)
                {
                    under = fold_eq(taught[i], canonical[i]);
                }
                if (under
                    && (canonical.size() == taught.size()
                        || taught.back() == '/'
                        || canonical[taught.size()] == '/'))
                {
                    canonical.replace(0, taught.size(), taught);
                    return canonical;
                }
            }
            return canonical;
        }

        /**
         * @brief 一个游标位置的身份：折叠点段、每段重写为其磁盘大小写、再
         *        恢复最长的数据库已授前缀。
         *
         * @internal
         * 纯函数；Walker 按原始拼写缓存走查结果，磁盘探测缓存是调用方持有的
         * PathCaseCache（每工人一个）。
         */
        [[nodiscard]]
        inline auto normalized(
            const cg::clang::SourceLocation& loc,
            const ExtractOptions&            opt,
            cg::util::PathCaseCache&         case_cache) -> std::string
        {
            return anchored_at_spelling(
                case_cache.canonical_case_spelling(
                    cg::util::collapse_dot_segments(loc.file)),
                opt);
        }

        /**
         * @brief 游标的命名空间路径：仅语义父命名空间，最外层在前
         *        （"app::inner"）。链上的记录与枚举被跳过（成员的命名空间是
         *        其包围记录的命名空间）。匿名命名空间不贡献内容。
         */
        [[nodiscard]]
        inline auto ns_path_of(const CXCursor& c) -> std::string
        {
            std::vector<std::string> parts;
            CXCursor                 p = cg::clang::semantic_parent(c);
            while (!cg::clang::is_null(p))
            {
                const CXCursorKind pk = cg::clang::kind(p);
                if (pk == CXCursor_Namespace)
                {
                    const std::string name = cg::clang::spelling(p);
                    if (!name.empty())
                    {
                        parts.push_back(name);
                    }
                }
                else if (!is_scope_kind(pk))
                {
                    break; // 函数局部作用域终止上行
                }
                p = cg::clang::semantic_parent(p);
            }
            std::ranges::reverse(parts);
            return cg::util::join(parts, "::");
        }

        /**
         * @brief 直接声明在 `c` 上的模板参数名（仅直接子游标：扫描回调从不
         *        下潜）。
         */
        [[nodiscard]]
        inline auto template_params_of(const CXCursor& c)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            cg::clang::children(
                c,
                [&](const CXCursor& ch)
                {
                    if (is_template_param_kind(cg::clang::kind(ch)))
                    {
                        out.push_back(cg::clang::spelling(ch));
                    }
                    return false; // 仅直接子游标
                });
            return out;
        }

        /**
         * @brief 特化游标的显示名：libclang 的拼写对偏特化丢弃 "`<...>`" 实参
         *        （只返回 "Box"），因此实参从模板实参 C API 重新接上。空白被
         *        剥除以保持跨打印机细节级别的稳定性。
         *
         * @internal
         * clang 把依赖实参打印为
         * "type-parameter-`<decl>`-`<index>`"；按声明的参数
         * 名（"T"）重新接上。该 token 形如
         * "type-parameter-`<depth>`-`<index>`"，在第二个数字之后结束、先于任何尾部
         * 修饰（*、&、const……）——那些属于实参而非名字。
         */
        [[nodiscard]]
        inline auto specialization_name(
            const CXCursor&    c,
            const std::string& spelling) -> std::string
        {
            const int argc = cg::clang::num_template_arguments(c);
            if (argc <= 0)
            {
                return spelling;
            }
            const std::vector<std::string> params = template_params_of(c);
            const auto                     substitute = [&](std::string arg)
            {
                for (std::size_t pos = arg.find("type-parameter-");
                     pos != std::string::npos;
                     pos = arg.find("type-parameter-", pos))
                {
                    const std::size_t num_start = pos + 15;
                    std::size_t       num_end = num_start;
                    while (num_end < arg.size() && arg[num_end] >= '0'
                           && arg[num_end] <= '9')
                    {
                        ++num_end;
                    }
                    if (num_end < arg.size() && arg[num_end] == '-')
                    {
                        const auto depth = static_cast<unsigned>(
                            std::atoi(arg.c_str() + num_start));
                        std::size_t end = num_end + 1;
                        while (end < arg.size() && arg[end] >= '0'
                               && arg[end] <= '9')
                        {
                            ++end;
                        }
                        if (depth < params.size())
                        {
                            arg.replace(pos, end - pos, params[depth]);
                        }
                        else
                        {
                            pos = end;
                        }
                    }
                    else
                    {
                        pos = num_start;
                    }
                }
                return arg;
            };
            std::string out = spelling;
            out += '<';
            for (int i = 0; i < argc; ++i)
            {
                out += substitute(
                    cg::clang::template_argument_type_spelling(c, i));
            }
            out += '>';
            std::erase_if(
                out,
                [](char ch) { return ch == ' ' || ch == '	'; });
            return out;
        }

        /**
         * @brief 声明游标的显示名：就是拼写，唯独模板特化重新接上 "`<...>`"
         * 实参。
         *
         * @internal
         * 一切构建名字或 scope 组件的路径都经过它，让记录节点与其成员的
         * scope 保持一致（"shape::Box<T*>::get"）。拼名覆盖两类特化形态：显式
         * 偏特化（ClassTemplatePartialSpecialization）与特化实例 decl（显式全
         * 特化/隐式实例化的 StructDecl 系——spelling 是裸模板名，实参可取才
         * 拼）。主模板本体（ClassTemplate）的实参列表是声明形参不是实参，
         * 不进此分支，既有键零变化。
         */
        [[nodiscard]]
        inline auto display_name_of(const CXCursor& c) -> std::string
        {
            std::string        spelling = cg::clang::spelling(c);
            const CXCursorKind k = cg::clang::kind(c);
            if (k == CXCursor_ClassTemplatePartialSpecialization)
            {
                return specialization_name(c, spelling);
            }
            if ((k == CXCursor_StructDecl || k == CXCursor_ClassDecl
                 || k == CXCursor_UnionDecl)
                && cg::clang::num_template_arguments(c) > 0
                && spelling.find('<') == std::string::npos)
            {
                return specialization_name(c, spelling);
            }
            return spelling;
        }

        /**
         * @brief 特化 decl 的实参列表是否含模板形参引用（依赖形态基类判据）。
         *
         * @internal
         * 实参的原始 spelling 含 "type-parameter-`<d>`-`<i>`"（canonical
         * 形态）——
         * 与派发管线的零误报判据同源。此形态无从对齐到具体实例符号，extends
         * 指主模板本尊 + is_template_base 标注（探针定论）。
         */
        [[nodiscard]]
        inline auto has_dependent_template_argument(const CXCursor& c) -> bool
        {
            for (const std::string& arg : template_params_of(c))
            {
                if (arg.find("type-parameter-") != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 游标的 scope：命名空间路径加包围记录名（"ns"、
         *        "render::Canvas"、"shape::Box<T*>"）。从语义父推导，因此对
         *        类外定义同样正确。
         */
        [[nodiscard]]
        inline auto scope_of(const CXCursor& c) -> std::string
        {
            std::vector<std::string> parts;
            CXCursor                 p = cg::clang::semantic_parent(c);
            while (!cg::clang::is_null(p))
            {
                const CXCursorKind k = cg::clang::kind(p);
                if (k == CXCursor_Namespace)
                {
                    const std::string name = cg::clang::spelling(p);
                    if (!name.empty())
                    {
                        parts.push_back(name);
                    }
                }
                else if (is_scope_kind(k))
                {
                    parts.push_back(display_name_of(p));
                }
                else
                {
                    break;
                }
                p = cg::clang::semantic_parent(p);
            }
            std::ranges::reverse(parts);
            return cg::util::join(parts, "::");
        }

        /** @brief 游标的包围记录名（语义父不是记录时为 ''）。 */
        [[nodiscard]]
        inline auto parent_record_name(const CXCursor& c) -> std::string
        {
            const CXCursor p = cg::clang::semantic_parent(c);
            if (cg::clang::is_null(p) || !is_scope_kind(cg::clang::kind(p)))
            {
                return {};
            }
            return display_name_of(p);
        }

        /**
         * @brief 字符串/字符字面量剥除扫描器。
         *
         * @internal 文本判据的多个扫描循环共用同一套转义与闭合语义：
         *           反斜杠吞掉字面量内的下一字符；引号按种类开启/闭合
         *           对应字面量（字符串内的单引号、字符字面量内的双引
         *           号都是普通内容）。字面量内的字符对宿主扫描不可见，
         *           宿主先判 inside() 再把字面量内字符交给 consume()。
         */
        struct LiteralScan
        {
            bool str = false; // 处于字符串字面量内
            bool chr = false; // 处于字符字面量内
            bool esc = false; // 前一字符是字面量内的转义反斜杠

            /** @brief 是否处于任一字面量内。 */
            [[nodiscard]]
            auto inside() const -> bool
            {
                return str || chr;
            }

            /** @brief 字面量内的字符消费（前置条件 inside()）。 */
            auto consume(char ch) -> void
            {
                if (esc)
                {
                    esc = false;
                }
                else if (ch == '\\')
                {
                    esc = true;
                }
                else if (str && ch == '"')
                {
                    str = false;
                }
                else if (chr && ch == '\'')
                {
                    chr = false;
                }
            }

            /**
             * @brief 字面量外的开引号：ch 为引号则开启对应字面量并
             *        返回 true。
             *
             * @internal 宿主扫描在开引号字符上自行收口（词边界判定
             *           等），再调这里；非引号字符返回 false、状态
             *           不动。
             */
            auto open(char ch) -> bool
            {
                if (ch == '"')
                {
                    str = true;
                    return true;
                }
                if (ch == '\'')
                {
                    chr = true;
                    return true;
                }
                return false;
            }
        };

        /**
         * @brief constexpr 检测：AST 重打印文本的声明头里找说明符。
         *
         * @internal
         * 本 libclang 构建没有 clang_Cursor_isConstexpr 谓词（类型限定符、
         * 存储类、求值结果族都与说明符正交），只能看文本——但看的必须是
         * 编译器重建的文本（clang_getCursorPrettyPrinted），不是源文件
         * 原文：打印走语义树，宏已展开。旧的前缀直读源文本判据有两个实
         * 测缺陷——说明符由宏生成（#define CX constexpr）时前缀里只有宏
         * 别名，漏报；名字由宏生成时前缀区间不可信，旧回退把整声明
         * extent 词法化，函数体内的 constexpr 局部量污染判定，非
         * constexpr 函数误报。
         *
         * 三道精化缺一不可：先剥字符串/字符字面量（字面量内容恰含
         * "constexpr" 字样不是说明符），再把说明符之前的两类花括号组按
         * 配对整组跳过（见下），最后截到函数体起点（函数定义的打印含
         * 整个函数体——体内局部量的声明头不得污染本符号；分号是纯声明
         * 尾的等价截断点）。词匹配按标识符边界分词（is_constexpr_x 之类
         * 的标识符不命中）。单遍扫描、零中间拷贝，也彻底不经
         * clang_tokenize——它每次调用都从文件顶部重新词法化，对多定义
         * 文件是平方级（一个 SWIG 生成的翻译单元曾把 68 秒访问时间的
         * 99% 花在 6.6 千次这样的扫描上）。
         *
         * 说明符之前的两类花括号不是函数体，按语境配对跳过：requires
         * 词之后的首个花括号组是复合要求的体（`requires requires
         * { ... }` 的体在说明符之前，stdexec 生态 asioexec/
         * completion_token.hpp 的 query/prefer/require/dispatch/post/
         * defer 六处为实测同型形态）；尖括号深度大于零（模板形参表）内
         * 的花括号组是类类型非类型模板参数的花括号默认值
         * （`T V = T{}`）。跳过标记一次性——下一个花括号回到截断语义；
         * 组内字符串照剥、嵌套花括号计数。尖括号计数在运算符名
         * （operator<、operator<<）上会失衡，失衡的退化方向只是"函数体
         * 被整组跳过"：配对跳过不读组内内容，体内局部量同样不可见，
         * 误配至多漏报、不产生体内污染误报。
         *
         * 例外分支——显式 defaulted 成员：打印器对 "= default" 分支结构
         * 性丢弃 constexpr 说明符（各 PrintingPolicy 开关下均如此；
         * "= delete" 与常规函数保留），打印头判据对
         * `constexpr f() = default` 形态漏报。defaulted 声明的说明符
         * 只能回到源文本前缀直读（文件缓冲字节扫描，零 tokenize，无
         * 病态面；词匹配前剥注释与字符串/字符字面量——前缀起点含前导
         * 属性，属性实参字符串恰含说明符本词不是说明符）。前缀不可得
         * （宏生成的 defaulted）按未写处理——已披露的漏报盲区，不得引
         * tokenize 回退（病态面拆除是本判据的核心收益）。另一已披露
         * 残余盲区：打印器对属性实参里带转义引号的字符串会丢转义
         * （"a\"constexpr\"b" 打印成 "a"constexpr"b"，字符串边界在打印
         * 层已坏）——文本判据的固有税，语料零命中。
         */
        [[nodiscard]]
        inline auto is_constexpr_decl(const CXCursor& c) -> bool
        {
            const std::string printed = cg::clang::pretty_printed_decl(c);
            const std::size_t size = printed.size();
            LiteralScan       lit;
            // 说明符之前的两类花括号语境：requires 词之后的首个花括号组
            // 是复合要求的体；尖括号深度大于零（模板形参表）内的花括号
            // 组是类类型 NTTP 的花括号默认值。两者都不是函数体。
            bool        after_requires = false;
            int         angle_depth = 0;
            std::string word;
            const auto  word_hits = [&word]
            { return word == "constexpr" || word == "consteval"; };
            // 词边界收口：说明符命中即真；requires 词开启一次性跳过语境。
            const auto word_end = [&]()
            {
                if (word_hits())
                {
                    return true;
                }
                if (word == "requires")
                {
                    after_requires = true;
                }
                word.clear();
                return false;
            };
            std::size_t i = 0;
            while (i < size)
            {
                const char ch = printed[i];
                if (lit.inside())
                {
                    lit.consume(ch);
                    ++i;
                    continue;
                }
                if (ch == '"' || ch == '\'')
                {
                    if (word_end())
                    {
                        return true;
                    }
                    lit.open(ch);
                    ++i;
                    continue;
                }
                if (ch == '{')
                {
                    if (!after_requires && angle_depth == 0)
                    {
                        break; // 函数体起点：截断
                    }
                    if (word_end())
                    {
                        return true;
                    }
                    // requirement 体 / 花括号默认值：配对整组跳过（组内
                    // 字符串照剥、嵌套花括号计数；组不闭合按截断处理）。
                    int         depth = 1;
                    LiteralScan group_lit;
                    while (++i < size)
                    {
                        const char g = printed[i];
                        if (group_lit.inside())
                        {
                            group_lit.consume(g);
                            continue;
                        }
                        if (group_lit.open(g))
                        {
                            continue;
                        }
                        if (g == '{')
                        {
                            ++depth;
                        }
                        else if (g == '}' && --depth == 0)
                        {
                            break;
                        }
                    }
                    after_requires = false;
                    word.clear();
                    ++i;
                    continue;
                }
                if (ch == ';')
                {
                    break;
                }
                if (ch == '<')
                {
                    if (word_end())
                    {
                        return true;
                    }
                    ++angle_depth;
                    ++i;
                    continue;
                }
                if (ch == '>')
                {
                    if (word_end())
                    {
                        return true;
                    }
                    if (angle_depth > 0)
                    {
                        --angle_depth;
                    }
                    ++i;
                    continue;
                }
                const bool ident = (ch >= 'a' && ch <= 'z')
                                   || (ch >= 'A' && ch <= 'Z')
                                   || (ch >= '0' && ch <= '9') || ch == '_';
                if (ident)
                {
                    word.push_back(ch);
                }
                else if (word_end())
                {
                    return true;
                }
                ++i;
            }
            if (word_hits())
            {
                return true;
            }
            if (!cg::clang::is_defaulted_method(c))
            {
                return false;
            }
            // defaulted 打印丢说明符：回到源前缀直读（词匹配前剥注释与
            // 字符串/字符字面量）。
            const std::optional<std::string> prefix =
                cg::clang::specifier_prefix_text(c);
            if (!prefix.has_value())
            {
                return false;
            }
            bool        line_comment = false;
            bool        block_comment = false;
            LiteralScan prefix_lit;
            std::string prefix_word;
            const auto  prefix_hits = [&prefix_word]
            {
                return prefix_word == "constexpr" || prefix_word == "consteval";
            };
            for (std::size_t j = 0; j < prefix->size(); ++j)
            {
                const char ch = (*prefix)[j];
                if (prefix_lit.inside())
                {
                    prefix_lit.consume(ch);
                    continue;
                }
                if (line_comment)
                {
                    if (ch == '\n')
                    {
                        line_comment = false;
                    }
                    continue;
                }
                if (block_comment)
                {
                    if (ch == '*' && j + 1 < prefix->size()
                        && (*prefix)[j + 1] == '/')
                    {
                        block_comment = false;
                        ++j;
                    }
                    continue;
                }
                if (ch == '/' && j + 1 < prefix->size())
                {
                    if ((*prefix)[j + 1] == '/')
                    {
                        line_comment = true;
                        ++j;
                        continue;
                    }
                    if ((*prefix)[j + 1] == '*')
                    {
                        block_comment = true;
                        ++j;
                        continue;
                    }
                }
                if (ch == '"' || ch == '\'')
                {
                    if (prefix_hits())
                    {
                        return true;
                    }
                    prefix_word.clear();
                    prefix_lit.open(ch);
                    continue;
                }
                const bool ident = (ch >= 'a' && ch <= 'z')
                                   || (ch >= 'A' && ch <= 'Z')
                                   || (ch >= '0' && ch <= '9') || ch == '_';
                if (ident)
                {
                    prefix_word.push_back(ch);
                }
                else if (prefix_hits())
                {
                    return true;
                }
                else
                {
                    prefix_word.clear();
                }
            }
            return prefix_hits();
        }

        /**
         * @brief 函数形游标（定义、声明或模板模式——三者携带同一签名）的
         *        延迟解析元数据。
         */
        [[nodiscard]]
        inline auto callee_meta(const CXCursor& fn) -> CalleeMeta
        {
            CalleeMeta m;
            m.name = cg::clang::spelling(fn);
            m.ns = ns_path_of(fn);
            m.parent = parent_record_name(fn);
            m.sig_hash = sig_hash_of(cg::clang::arg_type_spellings(fn));
            m.const_method = cg::clang::is_const_method(fn);
            m.kind = m.parent.empty() ? SymKind::Function : SymKind::Method;
            return m;
        }

        /**
         * @brief 对永远不可能是项目符号的别名的目标返回 true：内建类型拼写
         *        （"double"、"unsigned long long"）与标准命名空间。
         *
         * @internal 多词内建按 token 覆盖（"unsigned long long" 命中
         * "unsigned" token）。
         */
        [[nodiscard]]
        inline auto is_external_alias_target(std::string_view target) -> bool
        {
            if (target.starts_with("std::"))
            {
                return true;
            }
            if (target.find("::") != std::string_view::npos)
            {
                return false; // 限定在别处：交给解析裁决
            }
            static constexpr std::string_view BUILTINS[] = {
                "void",
                "bool",
                "char",
                "wchar_t",
                "char8_t",
                "char16_t",
                "char32_t",
                "int",
                "long",
                "short",
                "float",
                "double",
                "signed",
                "unsigned",
                "auto",
                "_Bool"};
            for (const std::string_view word : cg::util::split(target, ' '))
            {
                for (const std::string_view builtin : BUILTINS)
                {
                    if (word == builtin)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /**
         * @brief 走查上下文：树形知道而游标不知道的两个事实——哪个记录拥有
         *        当前子树（基说明符需要；访问记录的子游标时 ctx 已设好）、
         *        哪个函数拥有它（调用点需要；访问函数体时设置）。
         */
        struct Ctx
        {
            std::string parent_key; // 包围记录的 unique_key（无则 ''）
            std::string fn_key;     // 包围函数的 unique_key（无则 ''）
        };

        struct Walker
        {
            const ExtractOptions&    opt;
            cg::util::PathCaseCache& case_cache; // 调用方持有，每工人一个
            TuExtract                out;
            std::unordered_set<std::string> keys_seen;
            std::unordered_set<std::string> dependent_sites_seen;

            /**
             * @brief 头缓存查询桥（nullptr = 缓存关闭，逐字节走旧路径）。
             *
             * @internal 非空时 visit() 对每个游标先做归属文件判定：非主
             * 文件且缓存命中 → 整分区回放一次、后续该文件子树全部跳过。
             * 游标按词法序走查且产物归属与游标位置同文件（类外定义、宏
             * 展开游标都在各自词法位置成游标），故「首遇该文件的游标处
             * 回放全量分区 + 跳过该文件全部子树」与逐游标走查产出同一
             * 行集与同序发射。
             */
            const HeadCacheBridge* bridge_ = nullptr;

            /**
             * @internal 归属文件的三态缓存（原始拼写 → 状态+规范化路径），
             * 每游标一次查表：0=未决（触发一次定位+缓存查询），1=已回放
             * （跳过），2=主文件/未命中（照常走查）。normed 兼作当前发射
             * 区标注（符号的发射区=发射游标所在文件，可与归属文件不同）。
             */
            struct FileState
            {
                std::string   normed;
                unsigned char state = 0;
            };
            std::unordered_map<std::string, FileState> file_skip_;
            /** @internal 当前发射区（最近一次 visit 解析的游标文件）。 */
            std::string cur_region_;

            /** @internal 分区回放：缓存产物按记录序原样并入本 TU 抽取。 */
            void replay_partition(const HeadPartition& p)
            {
                for (std::size_t i = 0; i < p.symbols.size(); ++i)
                {
                    if (keys_seen.insert(p.symbols[i].unique_key).second)
                    {
                        out.symbols.push_back(p.symbols[i]);
                        out.symbol_regions.push_back(
                            i < p.symbol_regions.size() ? p.symbol_regions[i]
                                                        : p.file);
                    }
                }
                for (const EdgeRec& e : p.edges)
                {
                    out.edges.push_back(e);
                }
                for (const IncludeRec& i : p.includes)
                {
                    out.includes.push_back(i);
                }
                out.external_dropped += p.external_dropped;
                out.operator_dropped += p.operator_dropped;
                auto& d = out.dropped_by_file[p.file];
                d.first += p.external_dropped;
                d.second += p.operator_dropped;
            }

            /**
             * @internal 卫生计数器的头缓存按文件拆账（缓存关闭时只走聚合
             * 计数，零额外开销）：external=false 记 external_dropped。
             */
            void note_drop(const cg::clang::SourceLocation& loc, bool oper)
            {
                if (oper)
                {
                    ++out.operator_dropped;
                }
                else
                {
                    ++out.external_dropped;
                }
                if (bridge_ != nullptr)
                {
                    auto& d = out.dropped_by_file[norm(loc)];
                    if (oper)
                    {
                        ++d.second;
                    }
                    else
                    {
                        ++d.first;
                    }
                }
            }

            void note_drop(const std::string& file, bool oper)
            {
                if (oper)
                {
                    ++out.operator_dropped;
                }
                else
                {
                    ++out.external_dropped;
                }
                if (bridge_ != nullptr)
                {
                    auto& d = out.dropped_by_file[file];
                    if (oper)
                    {
                        ++d.second;
                    }
                    else
                    {
                        ++d.first;
                    }
                }
            }

            /**
             * @brief 头缓存子树跳过判定：命中已回放文件返回 true。
             *
             * 首遇的非主文件先查头缓存——命中即回放分区并入 visited_files，
             * 未命中仅登记 visited_files（记录面需要零产出文件的名字）。
             */
            bool skip_cached_file(const CXCursor& c)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               raw{loc.file};
                const auto [it, fresh] = file_skip_.try_emplace(raw);
                if (fresh)
                {
                    it->second.normed = raw.empty() ? std::string{} : norm(loc);
                    if (raw.empty() || it->second.normed == out.main_file
                        || bridge_ == nullptr)
                    {
                        it->second.state = 2;
                    }
                    else
                    {
                        const HeadPartition* hit =
                            bridge_->lookup(it->second.normed);
                        if (hit != nullptr)
                        {
                            out.visited_files.push_back(it->second.normed);
                            replay_partition(*hit);
                            it->second.state = 1;
                        }
                        else
                        {
                            out.visited_files.push_back(it->second.normed);
                            it->second.state = 2;
                        }
                    }
                }
                cur_region_ = it->second.normed;
                return it->second.state == 1;
            }

            /**
             * @brief 已解析调用站点的首次目击：调用游标的列加上已发射边记录
             *        的下标，让后续同键目击把书写重复折叠进 `occurrences`
             *        而不重复发射。
             */
            struct SiteSight
            {
                unsigned    col = 0;
                std::size_t edge = 0;
            };
            std::unordered_map<std::string, SiteSight> call_sites;

            explicit Walker(
                const ExtractOptions&    o,
                cg::util::PathCaseCache& cases)
                : opt(o), case_cache(cases)
            {
            }

            /**
             * @brief 游标位置的磁盘规范化、数据库拼写锚定身份（走查的唯一
             *        拼写接缝——见上方 normalized()）。
             *
             * @internal 按原始拼写缓存：一个翻译单元会重访每个头很多次。
             */
            [[nodiscard]]
            std::string norm(const cg::clang::SourceLocation& loc)
            {
                const std::string raw{loc.file};
                if (const auto hit = path_cache_.find(raw);
                    hit != path_cache_.end())
                {
                    return hit->second;
                }
                std::string normed = normalized(loc, opt, case_cache);
                path_cache_.emplace(raw, normed);
                return normed;
            }

            std::unordered_map<std::string, std::string> path_cache_;

            /**
             * @brief 只下潜 `c` 的直接子游标；clang 从不自动递归（回调处理后
             *        返回 false），因此每个游标每次走查恰好访问一次。
             */
            void walk_children(const CXCursor& c, const Ctx& ctx)
            {
                cg::clang::children(
                    c,
                    [&](const CXCursor& ch)
                    {
                        visit(ch, ctx);
                        return false; // visit() 在需要处显式下潜
                    });
            }

            /**
             * @brief 推入一个符号，除非其键已在本翻译单元产出过；两种情况都
             *        返回键（边端点可能引用它）。
             */
            std::string push_symbol(SymbolRec s)
            {
                s.unique_key = make_unique_key(s);
                const std::string key = s.unique_key;
                if (keys_seen.insert(key).second)
                {
                    out.symbols.push_back(std::move(s));
                    if (bridge_ != nullptr)
                    {
                        out.symbol_regions.push_back(cur_region_);
                    }
                }
                return key;
            }

            /**
             * @brief 符号发射的公共骨架：kind 与 ns/scope/parent/位置四件套。
             *
             * @internal
             * 九个发射点（record/function/field/var/enum/enum_constant/
             * alias/实例基类登记/friend 探针）的公共字段填充收口于此；name 留给
             * 调用方（记录用 display_name_of、其余用 spelling——两者的名字口径
             * 不同），种类特有字段（flags/param_types 等）同样由调用方补齐。
             */
            [[nodiscard]]
            auto base_symbol(
                const CXCursor&                  c,
                SymKind                          kind,
                const cg::clang::SourceLocation& loc,
                const std::string&               file) -> SymbolRec
            {
                SymbolRec s;
                s.kind = kind;
                s.ns = ns_path_of(c);
                s.scope = scope_of(c);
                s.parent = parent_record_name(c);
                s.file = file;
                s.line = static_cast<int>(loc.line);
                s.col = static_cast<int>(loc.column);
                return s;
            }

            /**
             * @brief 按游标种类分派。
             *
             * @internal
             * ParmDecl 分支：参数的声明类型引用外围函数拥有的记录（USES）；
             * 默认实参表达式继续走查以捕获调用。default 分支（命名空间、
             * 语句、表达式、模板参数）：自身无符号，但其子树重要。
             */
            void visit(const CXCursor& c, const Ctx& ctx)
            {
                if (bridge_ != nullptr && skip_cached_file(c))
                {
                    return; // 该文件分区已回放，子树整体跳过
                }
                const CXCursorKind k = cg::clang::kind(c);
                switch (k)
                {
                case CXCursor_InclusionDirective:
                    include_of(c);
                    return;
                case CXCursor_ModuleImportDecl:
                    import_of(c);
                    return;
                case CXCursor_ClassDecl:
                case CXCursor_StructDecl:
                case CXCursor_UnionDecl:
                case CXCursor_ClassTemplate:
                case CXCursor_ClassTemplatePartialSpecialization:
                    record_of(c, ctx);
                    return;
                case CXCursor_FunctionDecl:
                case CXCursor_FunctionTemplate:
                case CXCursor_CXXMethod:
                case CXCursor_Constructor:
                case CXCursor_Destructor:
                    function_of(c, ctx);
                    return;
                case CXCursor_FieldDecl:
                    field_of(c, ctx);
                    return;
                case CXCursor_ParmDecl:
                    if (!ctx.fn_key.empty())
                    {
                        uses_edges_of(
                            c,
                            ctx.fn_key,
                            edge_attrs::uses_site_param);
                    }
                    walk_children(c, ctx);
                    return;
                case CXCursor_VarDecl:
                    var_of(c, ctx);
                    return;
                case CXCursor_EnumDecl:
                    enum_of(c, ctx);
                    return;
                case CXCursor_EnumConstantDecl:
                    enum_constant_of(c, ctx);
                    return;
                case CXCursor_TypedefDecl:
                case CXCursor_TypeAliasDecl:
                    alias_of(c, ctx);
                    return;
                case CXCursor_FriendDecl:
                    friend_of(c, ctx);
                    return;
                case CXCursor_CXXBaseSpecifier:
                    base_spec_of(c, ctx);
                    return;
                case CXCursor_CallExpr:
                case CXCursor_MemberRefExpr:
                    call_of(c, ctx);
                    walk_children(c, ctx);
                    return;
                default:
                    walk_children(c, ctx);
                    return;
                }
            }

            /**
             * @brief 记录类游标 -> SymbolRec + 特化边 + 子树走查。
             *
             * @internal
             * 类模板的完整特化以普通 Class/StructDecl 形态出现、带非空的
             * specialized-template 游标。隐式实例化在每个使用点物化（实测大库
             * 显示 IidHolder`<T>` 记录按接口堆积）——而 libclang 无法把它们与
             * 显式特化区分开。两者都折叠到主模板的节点（DESIGN 的"仅主模板"
             * 存储立场）——但仅当主模板确实【定义】在项目内：仅声明的主模板
             * （实测大库的 `template<typename T> struct IidHolder;`
             * 模式）不可能
             * 被实例化，因此每个特化游标都是用户书写的显式代码、保留自己的
             * 节点。主模板在根之外（std::hash`<T>` 等）的特化也保留。特化 ->
             * 主模板边（clang_getSpecializedCursorTemplate；模板策略选项 1 加
             * 带守卫的选项 3 探针）：主模板必须是根内已定义的记录——仅声明的
             * 主模板没有节点，specializes 边会键落空。主模板游标对两处用途
             * （折叠检查 + specializes 边）只取一次：
             * clang_getSpecializedCursorTemplate 是走查中较贵的游标调用之一。
             */
            void record_of(const CXCursor& c, const Ctx& ctx)
            {
                static const bool DEBUG =
                    cg::util::cstd::get_env("CCG_DEBUG_RECS").has_value();
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (DEBUG)
                {
                    std::println(
                        std::cerr,
                        "REC kind={} name=[{}] file=[{}] roots={}",
                        static_cast<int>(cg::clang::kind(c)),
                        cg::clang::spelling(c),
                        file,
                        under_roots(file, opt) ? 1 : 0);
                }
                if (!under_roots(file, opt))
                {
                    return; // 外部定义不产节点
                }
                if (!cg::clang::is_definition(c))
                {
                    return; // 前向声明：无成员可访问
                }

                const CXCursorKind k = cg::clang::kind(c);

                // 显式全特化（template<> struct X`<int>` 顶层可见的 StructDecl
                // 系 + prim 非空）不再折叠：登记符号（名带实参，与偏特化同形
                // 态）+ 发 Specializes + 成员遍历——与显式偏特化同路。隐式实
                // 例化不进顶层遍历，不受此影响（探针定论）。
                const CXCursor prim = cg::clang::specialized_template(c);

                const std::string rec_name = display_name_of(c);
                SymbolRec s = base_symbol(c, SymKind::Record, loc, file);
                s.name = rec_name;
                if (k == CXCursor_StructDecl)
                {
                    s.flags |= sym_flags::is_struct;
                }
                if (k == CXCursor_UnionDecl)
                {
                    s.flags |= sym_flags::is_union;
                }
                if (k == CXCursor_ClassTemplate)
                {
                    s.flags |= sym_flags::is_template;
                    s.template_params = template_params_of(c);
                }
                if (k == CXCursor_ClassTemplatePartialSpecialization)
                {
                    s.flags |=
                        sym_flags::is_template | sym_flags::is_partial_spec;
                    s.template_params = template_params_of(c);
                }
                // 显式全特化/特化实例 decl（StructDecl 系 + prim 非空）：模板
                // 实例语义（无 is_partial_spec——非声明级偏特化）。
                if ((k == CXCursor_StructDecl || k == CXCursor_ClassDecl
                     || k == CXCursor_UnionDecl)
                    && !cg::clang::is_null(prim))
                {
                    s.flags |= sym_flags::is_template;
                }
                // C++ final 语法位：final 记录在 libclang 的直接子游标里暴露
                // CXXFinalAttr（非 final 记录没有该子游标——探针实证），子游标
                // 存在性即源码硬证据。消费方是 dispatch 静态决议的 final 并集
                // 口径（语法位不依赖 extends 图完备性）。attr 无子树，回调两
                // 个方向都返回 false（不下探，只扫兄弟）。
                cg::clang::children(
                    c,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch) == CXCursor_CXXFinalAttr)
                        {
                            s.flags |= sym_flags::is_final;
                        }
                        return false;
                    });

                const std::string key = push_symbol(std::move(s));

                const CXCursor& primary = prim;
                if (!cg::clang::is_null(primary) && !key.empty())
                {
                    const CXCursor    pdef = cg::clang::definition_of(primary);
                    const std::string pfile =
                        cg::clang::is_null(pdef)
                            ? std::string{}
                            : norm(cg::clang::location(pdef));
                    if (!cg::clang::is_null(pdef) && under_roots(pfile, opt)
                        && is_record_kind(cg::clang::kind(pdef)))
                    {
                        EdgeRec e;
                        e.src_key = key;
                        e.dst.key = make_record_key(
                            scope_of(pdef),
                            display_name_of(pdef),
                            pfile);
                        e.dst.is_key = true;
                        e.kind = EdgeKind::Specializes;
                        e.site_file = file;
                        e.site_line = static_cast<int>(loc.line);
                        const int argc = cg::clang::num_template_arguments(c);
                        e.attrs =
                            static_cast<std::uint32_t>(std::clamp(argc, 0, 15))
                            << edge_attrs::template_arg_shift;
                        out.edges.push_back(std::move(e));
                    }
                }

                Ctx inner = ctx;
                inner.parent_key = key;
                inner.fn_key.clear(); // 类作用域不在函数体内
                walk_children(c, inner);
            }

            /**
             * @brief 函数形游标 -> SymbolRec + overrides 边 + 返回类型 USES +
             *        函数体走查。
             *
             * @internal
             * overrides：libclang 解析基类链（含 ~D -> ~B）。返回类型 USES：
             * 构造/析构函数没有结果类型——类型走查对它们无产出；参数与局部量
             * 在走查下方各自的游标处处理。
             */
            void function_of(const CXCursor& c, const Ctx& ctx)
            {
                const CXCursorKind k = cg::clang::kind(c);
                const bool         pure =
                    (k == CXCursor_CXXMethod) && cg::clang::is_pure_virtual(c);
                const bool defaulted = cg::clang::is_defaulted_method(c);
                if (!cg::clang::is_definition(c) && !pure && !defaulted)
                {
                    return; // 仅声明（纯虚函数是例外）
                }

                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }

                SymKind kind;
                switch (k)
                {
                case CXCursor_Constructor:
                    kind = SymKind::Constructor;
                    break;
                case CXCursor_Destructor:
                    kind = SymKind::Destructor;
                    break;
                case CXCursor_CXXMethod:
                    kind = SymKind::Method;
                    break;
                default: // FunctionDecl / FunctionTemplate
                    kind = parent_record_name(c).empty() ? SymKind::Function
                                                         : SymKind::Method;
                    break;
                }
                SymbolRec s = base_symbol(c, kind, loc, file);
                s.name = cg::clang::spelling(c);
                s.param_types = cg::clang::arg_type_spellings(c);
                s.const_method = cg::clang::is_const_method(c);
                if (s.const_method)
                {
                    s.flags |= sym_flags::is_const;
                }
                if (cg::clang::is_virtual_method(c))
                {
                    s.flags |= sym_flags::is_virtual;
                }
                if (pure)
                {
                    s.flags |= sym_flags::is_pure_virtual;
                }
                if (cg::clang::is_static_method(c))
                {
                    s.flags |= sym_flags::is_static;
                }
                if (k == CXCursor_FunctionTemplate)
                {
                    s.flags |= sym_flags::is_template;
                    s.template_params = template_params_of(c);
                }
                if (is_constexpr_decl(c))
                {
                    s.flags |= sym_flags::is_constexpr;
                }
                // 方法级 C++ final：与 record 侧同一判据、宿主换成方法——
                // CXXMethod/Destructor 的直接子游标含 CXXFinalAttr 即源码硬
                // 证据（final 只允许修饰虚函数，自由函数/构造天然无此游标；
                // 类外定义的定义游标保留该位，库内符号取自定义处）。方法符
                // 号与 record 符号共用 is_final 位（双宿主语义见 sym_flags 处
                // 注释），消费方是 dispatch 静态决议的方法级 final 接力判定。
                // attr 无子树，回调两个方向都返回 false（不下探，只扫兄弟）。
                if (k == CXCursor_CXXMethod || k == CXCursor_Destructor)
                {
                    cg::clang::children(
                        c,
                        [&](const CXCursor& ch)
                        {
                            if (cg::clang::kind(ch) == CXCursor_CXXFinalAttr)
                            {
                                s.flags |= sym_flags::is_final;
                            }
                            return false;
                        });
                }

                const std::string key = push_symbol(std::move(s));

                if ((k == CXCursor_CXXMethod || k == CXCursor_Destructor)
                    && !key.empty())
                {
                    for (const CXCursor& o : cg::clang::overridden(c))
                    {
                        if (!under_roots(norm(cg::clang::location(o)), opt))
                        {
                            continue; // 外部基类（如 std streambuf 的方法）
                        }
                        EdgeRec e;
                        e.src_key = key;
                        e.dst.meta = callee_meta(o);
                        e.kind = EdgeKind::Overrides;
                        e.site_file = file;
                        e.site_line = static_cast<int>(loc.line);
                        out.edges.push_back(std::move(e));
                    }
                }

                uses_edges_of(c, key, edge_attrs::uses_site_return);

                Ctx inner = ctx;
                inner.fn_key = key;
                walk_children(c, inner);
            }

            void field_of(const CXCursor& c, const Ctx& /*ctx*/)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                SymbolRec s = base_symbol(c, SymKind::Field, loc, file);
                s.name = cg::clang::spelling(c);
                const std::string key = push_symbol(std::move(s));
                uses_edges_of(c, key, edge_attrs::uses_site_field);
            }

            /**
             * @brief 变量游标 -> SymbolRec（仅命名空间/类作用域）+ USES。
             *
             * @internal
             * 仅命名空间/类作用域——局部量与参数不作为符号保留（存储经济），
             * 但局部量的声明类型仍引用外围
             * 函数拥有的记录（USES），其初始化器仍可含调用。走查上下文是
             * 唯一可靠的判别器：fn_key 恰好在函数体内被设置。
             */
            void var_of(const CXCursor& c, const Ctx& ctx)
            {
                if (!ctx.fn_key.empty())
                {
                    uses_edges_of(c, ctx.fn_key, edge_attrs::uses_site_local);
                    walk_children(c, ctx);
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                SymbolRec s = base_symbol(c, SymKind::Variable, loc, file);
                s.name = cg::clang::spelling(c);
                if (is_constexpr_decl(c))
                {
                    s.flags |= sym_flags::is_constexpr;
                }
                const std::string key = push_symbol(std::move(s));
                uses_edges_of(c, key, edge_attrs::uses_site_variable);
            }

            void enum_of(const CXCursor& c, const Ctx& ctx)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt) || !cg::clang::is_definition(c))
                {
                    return;
                }
                const std::string enum_name = cg::clang::spelling(c);
                SymbolRec s = base_symbol(c, SymKind::Enumeration, loc, file);
                s.name = enum_name;
                const std::string key = push_symbol(std::move(s));

                Ctx inner = ctx;
                inner.parent_key = key;
                walk_children(c, inner);
            }

            void enum_constant_of(const CXCursor& c, const Ctx& /*ctx*/)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                SymbolRec s = base_symbol(c, SymKind::EnumConstant, loc, file);
                s.name = cg::clang::spelling(c);
                push_symbol(std::move(s));
            }

            /**
             * @brief 别名游标 -> SymbolRec + type_alias 边。
             *
             * @internal
             * 目标稍后按裸名解析（通常是另一个记录；底层类型拼写是非限定的）。
             * 指向非项目类型（内建、std::）的别名永远无法解析：在此丢弃，让
             * 未解析边指标反映真正缺失的项目目标、而非设计使然的外部目标。
             */
            void alias_of(const CXCursor& c, const Ctx& /*ctx*/)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                SymbolRec s = base_symbol(c, SymKind::TypeAlias, loc, file);
                s.name = cg::clang::spelling(c);
                const std::string key = push_symbol(std::move(s));

                if (key.empty())
                {
                    return;
                }
                std::string target =
                    cg::util::trim(cg::clang::underlying_type_spelling(c));
                if (target.empty())
                {
                    return;
                }
                if (is_external_alias_target(target))
                {
                    note_drop(file, false);
                    return;
                }
                EdgeRec e;
                e.src_key = key;
                e.dst.meta.name = std::move(target);
                e.dst.meta.kind = SymKind::Record;
                e.kind = EdgeKind::TypeAlias;
                e.site_file = file;
                e.site_line = static_cast<int>(loc.line);
                out.edges.push_back(std::move(e));
            }

            /**
             * @brief 在项目允许清单与主模板折叠策略下，把一个记录声明游标
             *        （来自声明类型走查）解析为 USES 边终点。
             *
             * @internal
             * 与 extends 相同的存储立场：根内已定义主模板的特化（隐式或显式）
             * 落到主模板的节点，因为那是符号侧铸造的唯一节点。外部记录（std、
             * 系统头）完全不产边——std 类型是文档化的 USES 非目标（其定义在
             * 索引根之外）；作为外部模板实参嵌套的项目类型仍会浮出，因为类型
             * 走查会递归进实参。定义在本翻译单元不可见的项目前向声明延迟到
             * 名字解析（定义活在另一个翻译单元的解析里）。特化：折叠到主
             * 模板，绝不信任特化游标自己的位置（隐式实例化携带使用点位置，
             * 会键落空）。
             */
            [[nodiscard]]
            bool uses_dst_of(const CXCursor& decl, EdgeDst& dst)
            {
                const CXCursor prim = cg::clang::specialized_template(decl);
                if (!cg::clang::is_null(prim))
                {
                    const CXCursor pdef = cg::clang::definition_of(prim);
                    if (cg::clang::is_null(pdef)
                        || !is_record_kind(cg::clang::kind(pdef))
                        || !under_roots(norm(cg::clang::location(pdef)), opt))
                    {
                        return false; // 外部/未定义主模板：无项目节点
                    }
                    dst.key = make_record_key(
                        scope_of(pdef),
                        display_name_of(pdef),
                        norm(cg::clang::location(pdef)));
                    dst.is_key = true;
                    return true;
                }
                const CXCursor def = cg::clang::definition_of(decl);
                if (!cg::clang::is_null(def)
                    && is_record_kind(cg::clang::kind(def))
                    && under_roots(norm(cg::clang::location(def)), opt))
                {
                    dst.key = make_record_key(
                        scope_of(def),
                        display_name_of(def),
                        norm(cg::clang::location(def)));
                    dst.is_key = true;
                    return true;
                }
                if (is_record_kind(cg::clang::kind(decl))
                    && under_roots(norm(cg::clang::location(decl)), opt))
                {
                    dst.meta = CalleeMeta{};
                    dst.meta.name = display_name_of(decl);
                    dst.meta.ns = ns_path_of(decl);
                    dst.meta.parent = parent_record_name(decl);
                    dst.meta.kind = SymKind::Record;
                    dst.is_key = false;
                    return true; // 项目前向声明：按名延迟
                }
                return false; // 外部类型（std/系统）：不产 USES 边
            }

            /**
             * @brief 为 `c` 的声明类型引用的每个项目记录发射一条 USES 边
             *        （字段、参数、返回类型、局部量、命名空间作用域变量）。
             *
             * @internal
             * src_key 是拥有该声明的符号——字段或变量本身；参数、返回类型与
             * 局部量则是外围函数。站点身份是声明游标的位置，因此同一行上多个
             * 同类型参数的出现次数折叠与书写的调用重复完全一致。字段的声明
             * 类型引用记录：字段拥有 USES 边，因此记录级影响面会列出字段本身。
             */
            void uses_edges_of(
                const CXCursor&    c,
                const std::string& src_key,
                std::uint32_t      site_bits)
            {
                if (src_key.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                // 环境开关函数级求值一次（record_of 同款写法）：热路径上每条
                // uses 边一次系统调用是纯重复付费。
                static const bool DEBUG =
                    cg::util::cstd::get_env("CCG_DEBUG_USES").has_value();
                for (const CXCursor& decl :
                     cg::clang::declared_type_record_decls(c))
                {
                    EdgeDst dst;
                    if (DEBUG)
                    {
                        std::println(
                            std::cerr,
                            "USES site {}:{} owner=[{}] type=[{}]"
                            " target=[{}] -> {}",
                            file,
                            loc.line,
                            src_key,
                            cg::clang::type_spelling(c),
                            cg::clang::spelling(decl),
                            uses_dst_of(decl, dst) ? "edge" : "drop");
                    }
                    if (!uses_dst_of(decl, dst))
                    {
                        continue;
                    }
                    EdgeRec e;
                    e.src_key = src_key;
                    e.dst = dst;
                    e.kind = EdgeKind::Uses;
                    e.site_file = file;
                    e.site_line = static_cast<int>(loc.line);
                    e.attrs = site_bits;
                    out.edges.push_back(std::move(e));
                }
            }

            /**
             * @brief 基说明符游标 -> extends 边。
             *
             * @internal
             * 特化基类（referenced 即特化 decl，探针实证 display/USR
             * 带实参）
             * 不再折叠回主模板：实例符号从使用点登记（隐式实例化不进顶层遍
             * 历，显式特化另有声明处登记，push_symbol 按 key 去重），extends
             * 直指实例——CRTP 家族的 overrides/派发消费由此接通。两条例外保
             * 留旧语义：roots 判定走归约链（特化定义或主模板定义任一在根内
             * 即收边，两边都界外仍丢）；依赖形态基类（模板形参作实参）无从
             * 对齐具体实例，指主模板本尊 + is_template_base 标注（可溯源不误
             * 指实例）。
             */
            void base_spec_of(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.parent_key.empty())
                {
                    return;
                }
                const CXCursor base = cg::clang::referenced(c);
                if (cg::clang::is_null(base))
                {
                    return;
                }
                CXCursor def = cg::clang::definition_of(base);
                if (cg::clang::is_null(def))
                {
                    def = base;
                }
                if (!is_record_kind(cg::clang::kind(def)))
                {
                    return;
                }
                CXCursor prim = cg::clang::specialized_template(def);
                if (!cg::clang::is_null(prim))
                {
                    const CXCursor pdef = cg::clang::definition_of(prim);
                    if (!cg::clang::is_null(pdef)
                        && is_record_kind(cg::clang::kind(pdef)))
                    {
                        prim = pdef;
                    }
                }
                // roots 判定保留归约链：特化定义或主模板定义任一在根内即收。
                const std::string bfile = norm(cg::clang::location(def));
                bool              under = under_roots(bfile, opt);
                if (!under && !cg::clang::is_null(prim))
                {
                    under = under_roots(norm(cg::clang::location(prim)), opt);
                }
                if (!under)
                {
                    note_drop(cg::clang::location(c), false);
                    return;
                }
                // 依赖形态：基说明符的 referenced 是主模板本尊（ClassTemplate，
                // 探针定论——模板形参作实参时拿不到特化 decl）-> 指本尊 + 标注。
                // 本尊就是 def 本身（prim 为 null：specialized_template(本尊)
                // 无值），直用 def，勿经 prim。建议级接线：kind
                // 判据之外， 实参含 dependent（如 Base`<T>`::type 的 typedef
                // 基）同样无实例可 指，走同一路径（原 kind-only
                // 判据漏此形态——helper 从死码转正）。
                if (cg::clang::kind(def) == CXCursor_ClassTemplate
                    || (!cg::clang::is_null(prim)
                        && has_dependent_template_argument(def)))
                {
                    // 依赖形态一律指本尊：def 本身是本尊（ClassTemplate）时直用
                    // def；def 是实参含 dependent 的特化 decl 时指 prim。
                    const CXCursor& target =
                        cg::clang::kind(def) == CXCursor_ClassTemplate ? def
                                                                       : prim;
                    const std::string tfile = norm(cg::clang::location(target));
                    const cg::clang::SourceLocation site =
                        cg::clang::location(c);
                    EdgeRec e;
                    e.src_key = ctx.parent_key;
                    e.dst.key = make_record_key(
                        scope_of(target),
                        display_name_of(target),
                        tfile);
                    e.dst.is_key = true;
                    e.kind = EdgeKind::Extends;
                    e.site_file = norm(site);
                    e.site_line = static_cast<int>(site.line);
                    e.attrs |= edge_attrs::is_template_base;
                    apply_base_attrs(c, e);
                    out.edges.push_back(std::move(e));
                    return;
                }
                // 实例基类：登记实例符号（与显式偏特化声明处登记的 key 同形
                // 态天然去重；隐式实例化只在此登记），边直指实例；顺带补发
                // Specializes→本尊（建议级补齐：隐式实例路径此前不发，与
                // 顶层显式形态不一致）。
                const cg::clang::SourceLocation site = cg::clang::location(c);
                std::string                     bkey;
                if (!cg::clang::is_null(prim))
                {
                    const cg::clang::SourceLocation bloc =
                        cg::clang::location(def);
                    SymbolRec bs =
                        base_symbol(def, SymKind::Record, bloc, bfile);
                    bs.name = display_name_of(def);
                    bs.flags = sym_flags::is_template;
                    bkey = push_symbol(std::move(bs));
                    const std::string pfile = norm(cg::clang::location(prim));
                    if (under_roots(pfile, opt))
                    {
                        EdgeRec sp;
                        sp.src_key = bkey;
                        sp.dst.key = make_record_key(
                            scope_of(prim),
                            display_name_of(prim),
                            pfile);
                        sp.dst.is_key = true;
                        sp.kind = EdgeKind::Specializes;
                        sp.site_file = norm(site);
                        sp.site_line = static_cast<int>(site.line);
                        out.edges.push_back(std::move(sp));
                    }
                }
                else
                {
                    bkey = make_record_key(
                        scope_of(def),
                        display_name_of(def),
                        bfile);
                }
                EdgeRec e;
                e.src_key = ctx.parent_key;
                e.dst.key = std::move(bkey);
                e.dst.is_key = true;
                e.kind = EdgeKind::Extends;
                e.site_file = norm(site);
                e.site_line = static_cast<int>(site.line);
                apply_base_attrs(c, e);
                out.edges.push_back(std::move(e));
            }

            /** @brief 基说明符的访问级别与虚继承位（两条路径共用）。 */
            void apply_base_attrs(const CXCursor& c, EdgeRec& e)
            {
                switch (cg::clang::access_of(c))
                {
                case cg::clang::Access::Protected:
                    e.attrs |= edge_attrs::access_protected;
                    break;
                case cg::clang::Access::Private:
                    e.attrs |= edge_attrs::access_private;
                    break;
                default:
                    break;
                }
                if (cg::clang::is_virtual_base(c))
                {
                    e.attrs |= edge_attrs::is_virtual_base;
                }
            }

            /**
             * @brief 友元游标 -> friend_of 边。
             *
             * @internal
             * src 必须是节点：仅当友元的定义在本翻译单元可见时发射（延迟 src
             * 的边不在记录集里）。
             */
            void friend_of(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.parent_key.empty())
                {
                    return;
                }
                const CXCursor friend_ent = cg::clang::referenced(c);
                if (cg::clang::is_null(friend_ent))
                {
                    return;
                }
                CXCursor def = cg::clang::definition_of(friend_ent);
                if (cg::clang::is_null(def))
                {
                    return;
                }
                const std::string ffile = norm(cg::clang::location(def));
                if (!under_roots(ffile, opt))
                {
                    return;
                }
                const cg::clang::SourceLocation site = cg::clang::location(c);
                if (!under_roots(norm(site), opt))
                {
                    return;
                }
                SymbolRec probe = base_symbol(
                    def,
                    is_record_kind(cg::clang::kind(def)) ? SymKind::Record
                                                         : SymKind::Function,
                    cg::clang::location(def),
                    ffile);
                probe.name = cg::clang::spelling(def);
                // 位置不进 key 的语义在此显式固定（1:1）：探针只求键，
                // 不承载定位。
                probe.line = 1;
                probe.col = 1;
                probe.param_types = cg::clang::arg_type_spellings(def);
                probe.const_method = cg::clang::is_const_method(def);
                EdgeRec e;
                e.src_key = make_unique_key(probe);
                e.dst.key = ctx.parent_key;
                e.dst.is_key = true;
                e.kind = EdgeKind::FriendOf;
                e.site_file = norm(site);
                e.site_line = static_cast<int>(site.line);
                out.edges.push_back(std::move(e));
            }

            /**
             * @brief 调用表达式游标 -> calls 边（含出现次数折叠）。
             *
             * @internal
             * 项目允许清单作用在 callee 自己的声明位置（std 容器、运行时函数
             * 等都活在根之外）。实例化的成员/函数模板映射回其主模式：调用落在
             * 模板自己的符号上（DESIGN 第 6 节）。站点身份：外围函数、callee
             * 身份（名字、重载签名、父级）、文件、行。成员调用以 CALL_EXPR
             * 包裹同一 callee 的 MEMBER_REF_EXPR 出现；两者描述同一次书写调用，
             * 因此重复的识别必须与那对区分开：只有同一行上【不同】列的
             * CALL_EXPR 才是第二次书写调用（"return get() + get();"）。
             */
            void call_of(const CXCursor& c, const Ctx& ctx)
            {
                CXCursor ref = cg::clang::referenced(c);
                if (cg::clang::is_null(ref))
                {
                    dependent_call_of(c, ctx);
                    return;
                }
                const CXCursorKind ref_kind = cg::clang::kind(ref);
                if (ref_kind != CXCursor_CXXMethod
                    && ref_kind != CXCursor_FunctionDecl
                    && ref_kind != CXCursor_FunctionTemplate)
                {
                    return; // 字段、构造函数、值：不是调用边
                }

                const std::string name = cg::clang::spelling(ref);
                if (name.starts_with("operator"))
                {
                    note_drop(cg::clang::location(c), true);
                    return;
                }
                for (const std::string_view prefix : {"_M_", "__", "_X"})
                {
                    if (name.starts_with(prefix))
                    {
                        note_drop(cg::clang::location(c), true);
                        return;
                    }
                }

                if (!under_roots(norm(cg::clang::location(ref)), opt))
                {
                    note_drop(cg::clang::location(c), false);
                    return;
                }

                const CXCursor primary = cg::clang::specialized_template(ref);
                if (!cg::clang::is_null(primary))
                {
                    ref = primary;
                }

                if (ctx.fn_key.empty())
                {
                    return; // 无外围函数（初始化器、NSDMI 等）
                }

                const cg::clang::SourceLocation site = cg::clang::location(c);
                const std::string site_file = norm(site); // 下方用到两次
                CalleeMeta        meta = callee_meta(ref);
                // 重载归属：任一实参 canonical 含 type-parameter（依赖）时
                // 置 arg_dep——同名"非模板+模板"并存时解析侧选模板版。
                {
                    std::vector<CXCursor> arg_kids;
                    cg::clang::children(
                        c,
                        [&](const CXCursor& ch)
                        {
                            arg_kids.push_back(ch);
                            return false;
                        });
                    // 首子是被调用表达式，实参从下标 1 起（fill_arg_records
                    // 的 i=1 起步口径）。
                    for (std::size_t ai = 1; ai < arg_kids.size(); ++ai)
                    {
                        const std::string cs =
                            cg::clang::canonical_type_spelling(arg_kids[ai]);
                        if (cs.find("type-parameter-") != std::string::npos)
                        {
                            meta.arg_dep = true;
                            break;
                        }
                    }
                }
                const std::string site_key = std::format(
                    "{}|{}|{}|{}|{}|{}",
                    ctx.fn_key,
                    meta.name,
                    meta.sig_hash.value_or(0),
                    meta.parent,
                    site_file,
                    site.line);
                const auto [it, fresh] =
                    call_sites.emplace(site_key, SiteSight{});
                if (fresh)
                {
                    it->second.col = site.column;
                    EdgeRec e;
                    e.src_key = ctx.fn_key;
                    e.dst.meta = meta;
                    e.kind = EdgeKind::Calls;
                    e.site_file = site_file;
                    e.site_line = static_cast<int>(site.line);
                    if (cg::clang::kind(ref) == CXCursor_CXXMethod)
                    {
                        e.attrs |= edge_attrs::dispatch_member;
                        if (cg::clang::is_virtual_method(ref))
                        {
                            e.attrs |= edge_attrs::dispatch_virtual;
                        }
                    }
                    out.edges.push_back(std::move(e));
                    it->second.edge = out.edges.size() - 1;
                    return;
                }
                if (cg::clang::kind(c) == CXCursor_CallExpr
                    && site.column != it->second.col)
                {
                    ++out.edges[it->second.edge].occurrences;
                }
            }

            /**
             * @brief clang 无法解析 callee 的调用——模板模式体内的类型依赖
             *        调用（对模板类型的 value 做 "return unwrap(value);"）。
             *
             * @internal
             * libclang 的 referenced() 在那里为 null；调用自身的拼写仍点名
             * callee，由解析器的层级（最可能是裸名）裁决是否有项目符号匹配。
             * 名字推导的主规则：callee 是调用第一个 "(" 之前的最后一个标识符，
             * 向前跳过一整组 "`<...>`" 模板实参（"drift`<double>`(...)" 取
             * "drift" 而非 "double"）。这覆盖 libclang 不暴露的 callee 表达式
             * （接收者是唯一直接子游标的依赖成员调用：否则 "self.decay()"
             * 会命名成 "self"）。回退规则：第一个引用形态的直接子游标（clang
             * 把 callee 表达式放在实参之前）。成员表达式先携带接收者
             * （"self.blend"——成员是最后一个标识符 token）；普通引用只携带
             * 名字（"unwrap"——第一个 token）。更晚的子游标是实参：绝不让他们
             * 命名 callee。模板内的转型表达式以 CALL_EXPR 形态出现、token 推导
             * 的名字是转型关键字（"static_cast`<T>`(x)"——参考语料实测 1 万+）。
             * 它们不是 callee。
             */
            void dependent_call_of(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation site = cg::clang::location(c);
                const std::string site_file = norm(site); // 下方用到两次
                if (!under_roots(site_file, opt))
                {
                    return;
                }
                std::string name = cg::clang::spelling(c);
                const auto  is_identifier = [](std::string_view tok)
                {
                    if (tok.empty())
                    {
                        return false;
                    }
                    const char first = tok[0];
                    return (first >= 'a' && first <= 'z')
                           || (first >= 'A' && first <= 'Z') || first == '_'
                           || first == '~';
                };
                if (name.empty())
                {
                    const std::vector<std::string> toks =
                        cg::clang::token_spellings(c);
                    std::size_t paren = toks.size();
                    for (std::size_t i = 0; i < toks.size(); ++i)
                    {
                        if (toks[i] == "(")
                        {
                            paren = i;
                            break;
                        }
                    }
                    if (paren > 0)
                    {
                        std::ptrdiff_t i =
                            static_cast<std::ptrdiff_t>(paren) - 1;
                        bool ok = true;
                        if (i >= 0 && toks[i] == ">")
                        {
                            int depth = 0;
                            while (i >= 0)
                            {
                                if (toks[i] == ">")
                                {
                                    ++depth;
                                }
                                else if (toks[i] == "<")
                                {
                                    --depth;
                                    if (depth == 0)
                                    {
                                        break;
                                    }
                                }
                                --i;
                            }
                            if (i < 0 || depth != 0)
                            {
                                ok = false; // 不配对：不是模板实参
                            }
                            else
                            {
                                --i;
                            }
                        }
                        if (ok && i >= 0 && is_identifier(toks[i]))
                        {
                            name = toks[i];
                        }
                    }
                }
                if (name.empty())
                {
                    const auto token_name =
                        [&is_identifier](const CXCursor& ref, bool last)
                    {
                        std::string found;
                        for (const std::string& tok :
                             cg::clang::token_spellings(ref))
                        {
                            if (is_identifier(tok))
                            {
                                found = tok;
                                if (!last)
                                {
                                    break; // 第一个标识符获胜
                                }
                            }
                        }
                        return found; // `last` 时最后一个标识符获胜
                    };
                    cg::clang::children(
                        c,
                        [&](const CXCursor& ch)
                        {
                            if (!name.empty())
                            {
                                return false;
                            }
                            const CXCursorKind ck = cg::clang::kind(ch);
                            const bool         member_like =
                                ck == CXCursor_MemberRefExpr
                                || ck == CXCursor_TemplateRef;
                            const bool value_like =
                                ck == CXCursor_OverloadedDeclRef
                                || ck == CXCursor_DeclRefExpr;
                            if (!member_like && !value_like)
                            {
                                return false;
                            }
                            std::string candidate = cg::clang::spelling(ch);
                            if (candidate.empty())
                            {
                                candidate = cg::clang::display_name(ch);
                            }
                            if (candidate.empty())
                            {
                                candidate = token_name(ch, member_like);
                            }
                            if (!candidate.empty())
                            {
                                name = std::move(candidate);
                            }
                            return false; // 仅直接子游标
                        });
                }
                if (name.empty())
                {
                    return;
                }
                const char first = name[0];
                const bool identifier_like = (first >= 'a' && first <= 'z')
                                             || (first >= 'A' && first <= 'Z')
                                             || first == '_' || first == '~';
                if (!identifier_like)
                {
                    return; // 编译器合成的拼写是噪声
                }
                if (name.starts_with("operator"))
                {
                    note_drop(site_file, true);
                    return;
                }
                if (name == "static_cast" || name == "dynamic_cast"
                    || name == "const_cast" || name == "reinterpret_cast")
                {
                    return;
                }
                const std::string site_key = std::format(
                    "{}|{}|{}|{}",
                    ctx.fn_key,
                    name,
                    site_file,
                    site.line);
                if (!dependent_sites_seen.insert(site_key).second)
                {
                    return;
                }
                EdgeRec e;
                e.src_key = ctx.fn_key;
                e.dst.meta.name = name;
                e.dst.meta.kind = SymKind::Function;
                // 重载归属：模板上下文的依赖调用，实参几乎必为依赖类型
                // （本函数的进入条件即 referenced 为 null 的模板体调用）——
                // arg_dep 置位后，解析侧在同名"非模板+模板"并存时选模板版。
                {
                    std::vector<CXCursor> arg_kids;
                    cg::clang::children(
                        c,
                        [&](const CXCursor& ch)
                        {
                            arg_kids.push_back(ch);
                            return false;
                        });
                    for (std::size_t ai = 1; ai < arg_kids.size(); ++ai)
                    {
                        const std::string cs =
                            cg::clang::canonical_type_spelling(arg_kids[ai]);
                        if (cs.find("type-parameter-") != std::string::npos)
                        {
                            e.dst.meta.arg_dep = true;
                            break;
                        }
                    }
                }
                e.kind = EdgeKind::Calls;
                e.site_file = site_file;
                e.site_line = static_cast<int>(site.line);
                out.edges.push_back(std::move(e));
            }

            /**
             * @brief include 指令游标 -> IncludeRec。
             *
             * @internal
             * token 扫描（INCLUSION_DIRECTIVE 元数据在部分 libclang 构建上不可
             * 靠）：拼接 include 关键字之后的每个 token；"`<...>`"
             * 为尖括号形式，
             * "\"...\"" 为引号形式。
             */
            void include_of(const CXCursor& c)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }

                std::string text;
                bool        past_keyword = false;
                for (const std::string& tok : cg::clang::token_spellings(c))
                {
                    if (!past_keyword)
                    {
                        if (tok == "include" || tok == "include_next"
                            || tok == "import")
                        {
                            past_keyword = true;
                        }
                        continue;
                    }
                    text += tok;
                }
                IncludeRec rec;
                rec.file = file;
                rec.line = static_cast<int>(loc.line);
                if (text.starts_with('<'))
                {
                    rec.angled = true;
                    rec.target = text.substr(1);
                    if (rec.target.ends_with('>'))
                    {
                        rec.target.pop_back();
                    }
                }
                else if (text.starts_with('"'))
                {
                    rec.target = text.substr(1);
                    if (rec.target.ends_with('"'))
                    {
                        rec.target.pop_back();
                    }
                }
                else
                {
                    rec.target = std::move(text);
                }
                if (!rec.target.empty())
                {
                    out.includes.push_back(std::move(rec));
                }
            }

            /**
             * @brief 命名模块翻译单元的一条 `import `<name>`;` 指令。
             *
             * @internal
             * 该光标仅在 import 对已喂入的 BMI 解析成功时存在（两层
             * 扫描契约；解析退化的翻译单元的模块需求由第一层源文本扫描提供），
             * 其拼写按原文携带被导入名（"std"、"cg.util"、"cg.util.json"；
             * 外部语料可能仍拼出分区导入，按原文记录）。按两层扫描契约做
             * main 文件过滤：只有翻译单元自身书写的 import 成为关系，BMI
             * 再导出的一切不算。
             */
            void import_of(const CXCursor& c)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (file != out.main_file)
                {
                    return; // 非本翻译单元 main 文件所书写
                }
                std::string name = cg::clang::spelling(c);
                if (name.empty())
                {
                    name = cg::clang::display_name(c);
                }
                if (name.empty())
                {
                    return;
                }
                ImportRec rec;
                rec.file = file;
                rec.module_name = std::move(name);
                rec.line = static_cast<int>(loc.line);
                out.imports.push_back(std::move(rec));
            }
        };

    } // namespace detail

    /**
     * @brief 抽取一个已解析的翻译单元。纯函数：同样输入、同样记录（按树
     *        序；case_cache 只备忘磁盘探测，从不改变结果）。错误原样拷入
     *        结果。
     *
     * @internal
     * case_cache 是调用方持有的每工人 PathCaseCache——整个运行期间每个
     * 抽取工人一个实例，而非每翻译单元一个。
     *
     * head_bridge（头缓存查询桥，可空）非空时启用子树跳过与分区回放：命中的
     * 非主文件不再逐游标走查，缓存分区按记录序回放（详见 Walker::bridge_
     * 注释）。桥由调用方保证本 TU 解析期内的指针稳定性，实现自带并发
     * 安全。
     */
    [[nodiscard]]
    inline auto extract(
        const cg::clang::ParsedTu& parsed,
        const ExtractOptions&      opt,
        cg::util::PathCaseCache&   case_cache,
        const HeadCacheBridge*     head_bridge = nullptr) -> TuExtract
    {
        detail::Walker w{opt, case_cache};
        w.bridge_ = head_bridge;
        w.out.main_file = detail::anchored_at_spelling(
            case_cache.canonical_case_spelling(
                cg::util::norm_path(parsed.tu.file())),
            opt);
        for (const cg::clang::Diagnostic& d : parsed.errors)
        {
            w.out.errors.push_back(d.spelling);
        }
        w.walk_children(parsed.tu.cursor(), detail::Ctx{});
        return std::move(w.out);
    }

} // namespace cg::extract
