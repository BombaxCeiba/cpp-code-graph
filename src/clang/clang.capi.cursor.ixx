module;
#include <clang-c/Index.h> // 第三方头文件，仅限全局模块片段

/**
 * @module cg.clang.capi.cursor
 *
 * @brief 游标查询域：kind/拼写/判定小函数群、参数与覆写链、token
 * 拼写与说明符前缀（含 token 卫生闸）。
 */
export module cg.clang.capi.cursor;

import std;
import cg.clang.loader;
import cg.util; // 伞形模块；此处用的是 cg.util.guard（崩溃防护）
import cg.clang.capi.api;
import cg.clang.capi.model;

export {
    using ::CX_BinaryOperatorKind;
    using ::CXCursor;
    using ::CXCursorKind;
}

export namespace cg::clang
{

    /**
     * @name 游标门面
     * 基于 const CXCursor& 的自由函数；cg.extract 绝不直接调 clang_*
     * 函数。
     */
    /** @{ */
    [[nodiscard]]
    auto kind(const CXCursor& c) noexcept -> CXCursorKind
    {
        return detail::api().clang_getCursorKind(c);
    }

    [[nodiscard]]
    auto kind_spelling(CXCursorKind k) -> std::string
    {
        return Str{detail::api().clang_getCursorKindSpelling(k)}.std_str();
    }

    [[nodiscard]]
    auto spelling(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getCursorSpelling(c)}.std_str();
    }

    [[nodiscard]]
    auto display_name(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getCursorDisplayName(c)}.std_str();
    }

    [[nodiscard]]
    auto is_definition(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_isCursorDefinition(c) != 0;
    }

    [[nodiscard]]
    auto is_const_method(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isConst(c) != 0;
    }

    [[nodiscard]]
    auto is_virtual_method(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isVirtual(c) != 0;
    }

    [[nodiscard]]
    auto is_pure_virtual(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isPureVirtual(c) != 0;
    }

    [[nodiscard]]
    auto is_static_method(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isStatic(c) != 0;
    }

    /**
     * @brief 显式默认（"= default"）成员。
     *
     * @internal clang 逐 TU 惰性补全类内默认定义，故仅
     * clang_isCursorDefinition 会漏掉只包含头文件的 TU 中的它们。
     */
    [[nodiscard]]
    auto is_defaulted_method(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isDefaulted(c) != 0;
    }

    [[nodiscard]]
    auto is_destructor(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_getCursorKind(c) == CXCursor_Destructor;
    }

    /** @brief 成员/基说明符游标的访问性（其他游标为 Invalid）。 */
    [[nodiscard]]
    auto access_of(const CXCursor& c) noexcept -> Access
    {
        return static_cast<Access>(
            detail::api().clang_getCXXAccessSpecifier(c));
    }

    /**
     * @brief CXXBaseSpecifier 游标的虚基位。
     *
     * @internal C API 仅部分绑定提供；此处一次调用即得——非基游标静默
     * 降级返回 false。
     */
    [[nodiscard]]
    auto is_virtual_base(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_isVirtualBase(c) != 0;
    }

    /**
     * @brief c 所指（或所声明）实体的定义游标；本翻译单元内不可达任何
     * 定义时为空游标。
     */
    [[nodiscard]]
    auto definition_of(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getCursorDefinition(c);
    }

    /**
     * @brief 两游标是否同一实体（逐字段等价比较）。
     *
     * @internal clang_equalCursors 是 dllimport 符号（本模块不链接导入库，
     * 动态加载其余 API），等价的字段比较：kind/xdata/data 三元组相等即同
     * 一游标（clang-c/Index.h 的官方实现语义）。
     */
    [[nodiscard]]
    inline auto equal_cursors(const CXCursor& a, const CXCursor& b) noexcept
        -> bool
    {
        if (a.kind != b.kind || a.xdata != b.xdata)
        {
            return false;
        }
        for (int i = 0; i < 3; ++i)
        {
            if (a.data[i] != b.data[i])
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 函数型游标的参数类型拼写（按序）。
     *
     * 非函数游标与无参函数都返回空；调用方无法仅凭返回值区分这两种
     * 情形（用游标 kind 判断）。
     *
     * @internal clang_getNumArgTypes 对一切非函数类型（记录、依赖/未解析
     * 类型、null 类型）回答 -1，而把 size_t 重解释的 -1 拿去 reserve 会
     * 抛 std::length_error——reserve 前先防护。
     */
    [[nodiscard]]
    auto arg_type_spellings(const CXCursor& c) -> std::vector<std::string>
    {
        const CXType             t = detail::api().clang_getCursorType(c);
        const int                n = detail::api().clang_getNumArgTypes(t);
        std::vector<std::string> out;
        if (n > 0)
        {
            out.reserve(static_cast<std::size_t>(n));
        }
        for (int i = 0; i < n; ++i)
        {
            out.push_back(
                Str{detail::api().clang_getTypeSpelling(
                        detail::api().clang_getArgType(t, i))}
                    .std_str());
        }
        return out;
    }

    /**
     * @brief 此（虚）成员函数游标覆写的方法，直接基优先。
     *
     * 非虚函数或无基声明该签名时为空。
     *
     * @internal libclang 解析整条继承链，含 ~D -> ~B 映射。
     */
    [[nodiscard]]
    auto overridden(const CXCursor& c) -> std::vector<CXCursor>
    {
        CXCursor* cursors = nullptr;
        unsigned  count = 0;
        detail::api().clang_getOverriddenCursors(c, &cursors, &count);
        std::vector<CXCursor> out;
        if (static_cast<std::int64_t>(count) > 0)
        {
            out.reserve(count);
        }
        for (unsigned i = 0; i < count; ++i)
        {
            out.push_back(cursors[i]);
        }
        detail::api().clang_disposeOverriddenCursors(cursors);
        return out;
    }

    /** @brief 特化化游标的模板实参数（非特化游标为 -1）。 */
    [[nodiscard]]
    auto num_template_arguments(const CXCursor& c) noexcept -> int
    {
        return detail::api().clang_Cursor_getNumTemplateArguments(c);
    }

    /** @brief 特化游标第 i 个模板实参类型的拼写（非类型/值实参与坏下标
     *  为 ""）。 */
    [[nodiscard]]
    auto template_argument_type_spelling(const CXCursor& c, int i)
        -> std::string
    {
        if (i < 0 || i >= detail::api().clang_Cursor_getNumTemplateArguments(c))
        {
            return {};
        }
        return Str{
            detail::api().clang_getTypeSpelling(
                detail::api().clang_Cursor_getTemplateArgumentType(c, i))}
            .std_str();
    }

    /**
     * @brief 二元运算游标的运算符码（非二元运算游标为 CX_BO_Invalid）。
     *
     * @internal opcode 是 Sema 建树时定在 AST 节点上的权威判定，不经
     * 源码词法化，宏展开造成的 extent 错位碰不到它——赋值判定（原
     * "'=' 出现在圆括号深度零"的 token 判据判的就是它）一律走这里。
     * 绑定旧名 clang_Cursor_getBinaryOpcode：新名
     * clang_getCursorBinaryOperatorKind 需 LLVM 21 起，旧名自 LLVM 3.6
     * 起全程在列且引擎实现即新名的转发。
     */
    [[nodiscard]]
    auto binary_opcode(const CXCursor& c) noexcept -> CX_BinaryOperatorKind
    {
        return detail::api().clang_Cursor_getBinaryOpcode(c);
    }

    namespace detail
    {

        /**
         * @brief 单语句级 token 扫描的 spelling 区间上限（字节）。
         *
         * @internal token_spellings 的全部消费点都是单表达式/单语句/单
         * 声明内的首符与形态判定，正常形态几十到几百字节；4KB 比实测
         * 最长的正常语句高一个数量级、比宏错位形态（数万字节，见
         * token_range_sane）低一个数量级，两侧都有安全边距。与
         * writes_through_deref 的 4096 节点上限同数量级（同为"防病态
         * 生成物"的界）。
         */
        constexpr unsigned max_statement_token_span = 4096;

        /** @brief 一处源位置的 spelling 落点（所属文件 + 文件内字节偏移）。 */
        struct SpellingEnd
        {
            CXFile   file = nullptr;
            unsigned off = 0;
        };

        [[nodiscard]]
        auto spelling_end(const CXSourceLocation loc) -> SpellingEnd
        {
            SpellingEnd out;
            api().clang_getSpellingLocation(
                loc,
                &out.file,
                nullptr,
                nullptr,
                &out.off);
            return out;
        }

        /**
         * @brief extent 两端的 spelling 位置是否构成同文件内、跨度受
         *        单语句上限约束的有效正向区间。
         *
         * @internal 跨文件区间（宏边界两端各在一处）clang_tokenize 本就
         *           产不出 token；同文件倒置区间只能词法出一个 token
         *           且必属错位垃圾——两者对任何合法扫描都拿不到可用
         *           token，拒绝是零代价的卫生边界。跨度上限那半边：
         *           clang_tokenize 按 range 两端的 spelling 偏移决定词
         *           法化区间，宏展开生成的表达式（SWIG PYTHON 后端把
         *           桥接 API 生成 #define，调用点全部经宏展开）其
         *           extent 两端各落在相距数万字节的不同宏定义处，被词
         *           法化的是两个定义点之间的无关源码——单次调用产出
         *           数千至上万 token、数十毫秒，且 token 内容与本游标
         *           无关，任何判定消费它都是误报。跨度越界即拒绝
         *           tokenize（调用方拿空 token 序列，与"宁漏勿误"的
         *           判定纪律一致）。
         */
        [[nodiscard]]
        auto token_range_sane(const CXSourceRange& range) -> bool
        {
            const SpellingEnd begin =
                spelling_end(api().clang_getRangeStart(range));
            const SpellingEnd end =
                spelling_end(api().clang_getRangeEnd(range));
            return begin.file != nullptr && end.file == begin.file
                   && begin.off <= end.off
                   && end.off - begin.off <= max_statement_token_span;
        }

    }

    /**
     * @brief 覆盖游标 extent 的原始 token 拼写（语句级卫生闸内）。
     *
     * @internal 用于对预处理指令与单表达式/单语句做 token 扫描——部分
     * libclang 构建上 INCLUSION_DIRECTIVE 元数据不可靠。extent 两端
     * spelling 位置错位（宏展开形态）或跨度超过单语句语义上限时返回
     * 空：错位区间的 token 是别处宏定义的源码，消费它只会误报（闸的
     * 语义论证见 detail::token_range_sane）。
     */
    [[nodiscard]]
    auto token_spellings(const CXCursor& c) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        const CXTranslationUnit  tu =
            detail::api().clang_Cursor_getTranslationUnit(c);
        if (tu == nullptr)
        {
            return out;
        }
        const CXSourceRange range = detail::api().clang_getCursorExtent(c);
        if (!detail::token_range_sane(range))
        {
            return out;
        }
        CXToken* tokens = nullptr;
        unsigned count = 0;
        detail::api().clang_tokenize(tu, range, &tokens, &count);
        for (unsigned i = 0; i < count; ++i)
        {
            out.push_back(
                Str{detail::api().clang_getTokenSpelling(tu, tokens[i])}
                    .std_str());
        }
        detail::api().clang_disposeTokens(tu, tokens, count);
        return out;
    }

    /**
     * @brief 被引用名在源码里是否显式写了嵌套名限定符
     *        （ns:: / Base:: / ::）。
     *
     * 判据是纯 AST 位置信息：带 WantQualifier 标志的引用名范围会把
     * nested-name-specifier 并入首段，与裸名范围不等即限定符在场。
     *
     * @internal 与 token 判据（extent 词法化找 "::"）语义同面且互补：
     * 本判据不经词法化，不受宏展开的 spelling 错位影响（token 侧在错位
     * 形态下被卫生闸拒绝而漏报），实参限定天然排除（只看被引用名自身）。
     * 非引用形态（指针间接调用等）两范围同为兜底值或 null，返回 false
     * 与"未写限定"同面。已知缺口：依赖重载集（UnresolvedLookupExpr）
     * 取不到限定符位置，两范围相同恒返回 false——由调用方在被调子式的
     * 限定符子游标（NamespaceRef）判据互补。
     */
    [[nodiscard]]
    auto reference_name_includes_qualifier(const CXCursor& c) -> bool
    {
        const CXSourceRange bare =
            detail::api().clang_getCursorReferenceNameRange(c, 0, 0);
        const CXSourceRange with_qualifier =
            detail::api().clang_getCursorReferenceNameRange(
                c,
                CXNameRange_WantQualifier | CXNameRange_WantSinglePiece,
                0);
        if (detail::api().clang_Range_isNull(bare)
            || detail::api().clang_Range_isNull(with_qualifier))
        {
            return false;
        }
        return !detail::api().clang_equalRanges(bare, with_qualifier);
    }

    /**
     * @brief 声明游标的 AST 重打印文本（编译器按语义重建，宏已展开）。
     *
     * 非声明形态返回空串。释放义务（CXString）由 Str 封装履行。
     *
     * @internal 与源文本的关键差别：打印走的是语义 AST，说明符以展开后
     * 的形态输出——`#define CXP constexpr` 生成的说明符在打印文本里就是
     * constexpr 本词，而源文件前缀里只有宏别名。函数**定义**的打印含整
     * 个函数体，只应消费声明头的判据必须自行截断（见 extract 侧
     * is_constexpr_decl 的截体与剥字符串两道精化）。打印策略传空指针：
     * 接口文档明确此时用默认策略（与 clang_getCursorPrintingPolicy 同源
     * 构造，均取自 TU 上下文的默认 PrintingPolicy），可免一次策略堆分配
     * 与释放。
     */
    [[nodiscard]]
    auto pretty_printed_decl(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getCursorPrettyPrinted(c, nullptr)}
            .std_str();
    }

    /**
     * @brief 声明前导说明符段的原始源文本（extent 起点 到 游标名字
     * 位置 的文件字节直读）。
     *
     * 范围不能当作同文件纯文本信任时（extent 与名字在不同文件、缓冲
     * 不可用、或名字拼写不在其文件位置——即宏生成的声明）返回
     * nullopt。
     *
     * @internal
     * 消费者是显式 defaulted 成员的说明符检测：打印器对 "= default"
     * 分支结构性丢弃 constexpr 说明符（实测各 PrintingPolicy 开关下
     * 均如此；"= delete" 与常规函数保留），此路只对 defaulted 形态
     * 直读源文本兜底。直读是文件缓冲上的字节扫描，零 tokenize——
     * clang_tokenize 每次调用都从文件头重新词法化，对多定义文件是
     * 平方级（SWIG 生成的翻译单元曾把 68 秒访问时间的 99% 花在
     * 6.6 千次这样的扫描上），绝不做它的回退。名字必须恰好写在
     * 它的文件位置所指处：拼写在别处（#define 内）意味着声明由宏
     * 生成，原始字节范围证明不了任何事。
     */
    [[nodiscard]]
    auto specifier_prefix_text(const CXCursor& c) -> std::optional<std::string>
    {
        const CXTranslationUnit tu =
            detail::api().clang_Cursor_getTranslationUnit(c);
        if (tu == nullptr)
        {
            return std::nullopt;
        }
        const CXSourceRange extent = detail::api().clang_getCursorExtent(c);
        CXFile              begin_file = nullptr, name_file = nullptr;
        unsigned            begin_off = 0, name_off = 0;
        detail::api().clang_getFileLocation(
            detail::api().clang_getRangeStart(extent),
            &begin_file,
            nullptr,
            nullptr,
            &begin_off);
        detail::api().clang_getFileLocation(
            detail::api().clang_getCursorLocation(c),
            &name_file,
            nullptr,
            nullptr,
            &name_off);
        if (begin_file == nullptr || begin_file != name_file
            || name_off < begin_off)
        {
            return std::nullopt;
        }
        CXFile   spell_file = nullptr;
        unsigned spell_off = 0;
        detail::api().clang_getSpellingLocation(
            detail::api().clang_getCursorLocation(c),
            &spell_file,
            nullptr,
            nullptr,
            &spell_off);
        if (spell_file != begin_file || spell_off != name_off)
        {
            return std::nullopt;
        }
        std::size_t size = 0;
        const char* data =
            detail::api().clang_getFileContents(tu, begin_file, &size);
        if (data == nullptr || begin_off >= size)
        {
            return std::nullopt;
        }
        const std::size_t end = std::min<std::size_t>(name_off, size);
        return std::string{data + begin_off, end - begin_off};
    }

    [[nodiscard]]
    auto semantic_parent(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getCursorSemanticParent(c);
    }

    [[nodiscard]]
    auto lexical_parent(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getCursorLexicalParent(c);
    }

    [[nodiscard]]
    auto referenced(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getCursorReferenced(c);
    }

    /**
     * @brief 此游标特化自的主模板（若有）；否则空游标（见 is_null）。
     *
     * @internal c 是模板实例化（非模板本尊）时返回来源模板声明——隐式/
     * 显式实例化的函数与记录体随使用方 TU 展开在 AST 上，referenced 游标
     * 即实例 decl；本尊（模板定义处）返回空。是"界外但可见"与"界外且
     * 不可见"的分野点。
     */
    [[nodiscard]]
    auto specialized_template(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getSpecializedCursorTemplate(c);
    }

    /**
     * @brief 游标的 USR（统一符号解析名）；无 USR 或垃圾游标为空串。
     *
     * @internal 同一声明的 USR 在同一 TU 内稳定，是跨访问形态（表达式引用
     * 与声明游标）比对字段/变量身份的依据。
     */
    [[nodiscard]]
    auto usr(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getCursorUSR(c)}.std_str();
    }

    /**
     * @brief 空游标判定：C API 的 null 与值初始化的空游标都视为空。
     *
     * @internal `clang_Cursor_isNull` 只把 `CXCursor_NoDeclFound(71)` 当作
     * null；值初始化的 `CXCursor{}` 其 `kind == 0`（不是任何有效游标
     * 种类），此前会被误判为非空，导致 analysis.collect 的
     * `referenced_callable` 对 VarDecl/FieldDecl 返回的空游标被当成真实
     * 声明。kind==0 在 libclang 中永远不是有效游标，两个约定一起接受。
     */
    [[nodiscard]]
    auto is_null(const CXCursor& c) noexcept -> bool
    {
        return c.kind == 0 || detail::api().clang_Cursor_isNull(c) != 0;
    }

    [[nodiscard]]
    auto location(const CXCursor& c) -> SourceLocation
    {
        return detail::file_location(detail::api().clang_getCursorLocation(c));
    }

    [[nodiscard]]
    auto extent(const CXCursor& c) -> SourceExtent
    {
        const CXSourceRange range = detail::api().clang_getCursorExtent(c);
        return SourceExtent{
            .begin =
                detail::file_location(detail::api().clang_getRangeStart(range)),
            .end =
                detail::file_location(detail::api().clang_getRangeEnd(range))};
    }

    /**
     * @brief 游标类型的拼写（函数游标得到的是完整函数类型，如
     * "int (ns::Widget &)"）。
     */
    [[nodiscard]]
    auto type_spelling(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getTypeSpelling(
                       detail::api().clang_getCursorType(c))}
            .std_str();
    }

    /** @brief 函数型游标的返回类型拼写。 */
    [[nodiscard]]
    auto result_type_spelling(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getTypeSpelling(
                       detail::api().clang_getCursorResultType(c))}
            .std_str();
    }

    /** @brief typedef 底层类型的拼写（非 typedef 游标为 ""）。 */
    [[nodiscard]]
    auto underlying_type_spelling(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getTypeSpelling(
                       detail::api().clang_getTypedefDeclUnderlyingType(c))}
            .std_str();
    }
    /** @} */

} // namespace cg::clang
