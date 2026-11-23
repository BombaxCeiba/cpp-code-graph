module;
#include <clang-c/Index.h> // 第三方头文件，仅限全局模块片段

/**
 * @module cg.clang.capi.type
 *
 * @brief 类型判定域：声明类型记录走查、TypeClass 分类与逐层剥型、canonical
 * 拼写与指针/引用判定、children 收集。
 */
export module cg.clang.capi.type;

import std;
import cg.clang.loader;
import cg.util; // 伞形模块；此处用的是 cg.util.guard（崩溃防护）
import cg.clang.capi.api;
import cg.clang.capi.model;
import cg.clang.capi.cursor;

export {
    using ::CX_BinaryOperatorKind;
    using ::CXCursor;
    using ::CXCursorKind;
}

export namespace cg::clang
{

    namespace detail
    {

        /**
         * @brief 声明类型记录走查的一个递归步。
         *
         * 下穿 cv/引用/指针/数组/typedef 糖，收集记录声明游标，并跟随模板
         * 实参类型，使模板特化内嵌套的项目类型（std::vector`<Widget>` ->
         * Widget）也浮现。依赖类型（模板参数）与非记录类型终止该分支。
         *
         * @internal
         * 有界：深度上限防病态糖链，记录上限防深嵌套实参表。模板特化类型
         * 在此 libclang 构建上以 Unexposed"按书写"形态浮现
         * （std::vector`<Widget>`、Box`<int>`）：书写的模板实参保持可查、规范
         * 类型是 Record 节点（声明 + 完整实参表、含默认实参）——两者都走，
         * USR 去重吸收重叠；但绝不递归进另一个 Unexposed 的规范类型（依赖
         * 名规范化为自身，那不会终止）。'int Widget::*' /
         * 'void (Widget::*)(Widget&)'：类类型命名拥有式记录；指针所指（数据
         * 或函数类型）携带自身引用、按任何声明类型一样走查。
         * clang_Type_getClassType 是抵达该类类型的唯一 C API 通路，但
         * libclang（23.x 上观察到）在成员指针为依赖时于其内部解引用
         * NULL——即未实例化模板模式中的 'void (U::*)(void)' 参数形状，其
         * 类半边是模板参数上的嵌名说明符。C API 无法在调用前检测该依赖性
         * （彼处指针所指是普通非依赖类型，也没有类型级查询暴露类半边的
         * 形状），故该调用在硬件错误防护下运行而非被丢弃：一个依赖模式
         * 损失的恰是这一条拥有式记录边（记录在那里反正未解析），而每个
         * 非依赖成员指针保住它的 USES 边——此走查为之存在的铸边。引擎在
         * 被防护的错误后继续服务（探针验证：同一 TU 上稍后的非依赖类
         * 类型查询正常回答），走查与访问的其余部分不受扰。回调形状：
         * std::function<Gadget()> 与 std::function<void(Widget&)> 把函数类型
         * 暴露为模板实参，函数指针/typedef 类型直接内嵌它；记录在结果类型
         * 与每个参数类型中浮现。
         */
        void record_decls_of_type(
            const CXType&                    t,
            int                              depth,
            std::vector<CXCursor>&           out,
            std::unordered_set<std::string>& seen)
        {
            constexpr int         MAX_DEPTH = 8;
            constexpr std::size_t MAX_RECORDS = 32;
            if (depth > MAX_DEPTH || out.size() >= MAX_RECORDS)
            {
                return;
            }
            switch (t.kind)
            {
            case CXType_Pointer:
            case CXType_LValueReference:
            case CXType_RValueReference:
                record_decls_of_type(
                    api().clang_getPointeeType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            case CXType_ConstantArray:
            case CXType_IncompleteArray:
            case CXType_VariableArray:
            case CXType_DependentSizedArray:
                record_decls_of_type(
                    api().clang_getElementType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            case CXType_Elaborated:
                record_decls_of_type(
                    api().clang_Type_getNamedType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            case CXType_Typedef:
            case CXType_Auto:
            {
                const CXType canon = api().clang_getCanonicalType(t);
                if (canon.kind != t.kind)
                { // 未推导的 auto 规范化为自身
                    record_decls_of_type(canon, depth + 1, out, seen);
                }
                return;
            }
            case CXType_Attributed:
                record_decls_of_type(
                    api().clang_Type_getModifiedType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            case CXType_Record:
            {
                const CXCursor decl = api().clang_getTypeDeclaration(t);
                if (!api().clang_Cursor_isNull(decl))
                {
                    const std::string usr =
                        Str{api().clang_getCursorUSR(decl)}.std_str();
                    if (!usr.empty() && seen.insert(usr).second)
                    {
                        out.push_back(decl);
                    }
                }
                const int argc = api().clang_Type_getNumTemplateArguments(t);
                for (int i = 0; i < argc && out.size() < MAX_RECORDS; ++i)
                {
                    record_decls_of_type(
                        api().clang_Type_getTemplateArgumentAsType(t, i),
                        depth + 1,
                        out,
                        seen);
                }
                return;
            }
            case CXType_Unexposed:
            {
                const int argc = api().clang_Type_getNumTemplateArguments(t);
                for (int i = 0; i < argc && out.size() < MAX_RECORDS; ++i)
                {
                    record_decls_of_type(
                        api().clang_Type_getTemplateArgumentAsType(t, i),
                        depth + 1,
                        out,
                        seen);
                }
                const CXType canon = api().clang_getCanonicalType(t);
                if (canon.kind == CXType_Record)
                {
                    record_decls_of_type(canon, depth + 1, out, seen);
                }
                return;
            }
            case CXType_MemberPointer:
            {
                CXType cls{};
                if (cg::util::guard::run_guarded(
                        [&] { cls = api().clang_Type_getClassType(t); }))
                {
                    record_decls_of_type(cls, depth + 1, out, seen);
                }
                record_decls_of_type(
                    api().clang_getPointeeType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            }
            case CXType_FunctionProto:
            case CXType_FunctionNoProto:
            {
                record_decls_of_type(
                    api().clang_getResultType(t),
                    depth + 1,
                    out,
                    seen);
                const int argc = api().clang_getNumArgTypes(t);
                for (int i = 0; i < argc && out.size() < MAX_RECORDS; ++i)
                {
                    record_decls_of_type(
                        api().clang_getArgType(t, static_cast<unsigned>(i)),
                        depth + 1,
                        out,
                        seen);
                }
                return;
            }
            default:
                return; // 内建、枚举、依赖名等
            }
        }

    } // namespace detail

    /**
     * @brief c 的声明类型引用的每个记录的声明游标（USES 边目标）。
     *
     * 函数型游标取结果类型，字段/变量/参数取声明类型。下穿途中剥掉
     * cv/引用/指针/数组包装（Widget*、const Widget& 与 Widget[4] 都引用
     * Widget），typedef 经规范类型解析，模板实参类型递归走查（Box<
     * Widget> 字段同时引用 Box 与 Widget），成员指针引用其类类型
     * （int Widget::* 引用 Widget），函数类型经结果 + 参数走查
     * （std::function<Gadget()> 与接受 Widget& 的回调引用 Gadget/Widget）。
     * 特化游标原样返回；把它们折叠到主模板是调用方的策略（它持有项目
     * 允许清单）。顺序为类型走查序、按 USR 去重。
     *
     * @internal 函数型游标取结果类型（构造/析构与非函数上为 Invalid，
     * 它们改取自身声明类型——构造函数的游标类型是其函数类型，走查无
     * 所获）。
     */
    [[nodiscard]]
    auto declared_type_record_decls(const CXCursor& c) -> std::vector<CXCursor>
    {
        std::vector<CXCursor> out;
        CXType                t = detail::api().clang_getCursorResultType(c);
        if (t.kind == CXType_Invalid)
        {
            t = detail::api().clang_getCursorType(c);
        }
        std::unordered_set<std::string> seen;
        detail::record_decls_of_type(t, 0, out, seen);
        return out;
    }

    /**
     * @brief 类型形状的粗分类（cg.analysis 的类型线索门面）。
     *
     * @internal 不导出裸 CXType：调用方只需要形状判别，具体类型查询走下方
     * 返回游标的包装。值刻意按 clang-c 的常用族分组，非逐 kind 镜像。
     */
    enum class TypeClass : std::uint8_t
    {
        Invalid = 0,
        Pointer,
        LValueReference,
        RValueReference,
        Record,
        Enumeration,
        Typedef,
        Auto,
        Elaborated,
        Unexposed,
        Dependent,
        MemberPointer,
        Array,
        Function,
        Builtin,
        Other,
    };

    namespace detail
    {

        auto classify_type(CXType t) -> TypeClass
        {
            switch (t.kind)
            {
            case CXType_Invalid:
                return TypeClass::Invalid;
            case CXType_Pointer:
                return TypeClass::Pointer;
            case CXType_LValueReference:
                return TypeClass::LValueReference;
            case CXType_RValueReference:
                return TypeClass::RValueReference;
            case CXType_Record:
                return TypeClass::Record;
            case CXType_Enum:
                return TypeClass::Enumeration;
            case CXType_Typedef:
                return TypeClass::Typedef;
            case CXType_Auto:
                return TypeClass::Auto;
            case CXType_Elaborated:
                return TypeClass::Elaborated;
            case CXType_Unexposed:
                return TypeClass::Unexposed;
            case CXType_Dependent:
                return TypeClass::Dependent;
            case CXType_MemberPointer:
                return TypeClass::MemberPointer;
            case CXType_ConstantArray:
            case CXType_IncompleteArray:
            case CXType_VariableArray:
            case CXType_DependentSizedArray:
                return TypeClass::Array;
            case CXType_FunctionProto:
            case CXType_FunctionNoProto:
                return TypeClass::Function;
            default:
                return CXType_FirstBuiltin <= t.kind
                               && t.kind <= CXType_LastBuiltin
                           ? TypeClass::Builtin
                           : TypeClass::Other;
            }
        }

        /**
         * @brief 剥掉指针/引用/elaborated/typedef/attributed 糖，停在记录/枚举
         *        或第一个不可解的 kind 上。
         *
         * @internal 绝不调用 clang_Type_getClassType（依赖成员指针上的已知崩溃
         * 面）；这里只需要记录声明。
         */
        auto peel_type(CXType t) -> CXType
        {
            for (int depth = 0; depth < 8; ++depth)
            {
                switch (t.kind)
                {
                case CXType_Pointer:
                case CXType_LValueReference:
                case CXType_RValueReference:
                    t = api().clang_getPointeeType(t);
                    break;
                case CXType_Elaborated:
                    t = api().clang_Type_getNamedType(t);
                    break;
                case CXType_Attributed:
                    t = api().clang_Type_getModifiedType(t);
                    break;
                case CXType_Typedef:
                case CXType_Auto:
                {
                    const CXType canon = api().clang_getCanonicalType(t);
                    if (canon.kind == t.kind || canon.kind == CXType_Invalid)
                    {
                        return t;
                    }
                    t = canon;
                    break;
                }
                case CXType_Unexposed:
                {
                    const CXType canon = api().clang_getCanonicalType(t);
                    if (canon.kind == CXType_Unexposed
                        || canon.kind == CXType_Invalid)
                    {
                        return t;
                    }
                    t = canon;
                    break;
                }
                default:
                    return t;
                }
            }
            return t;
        }

        auto declaration_of_type(CXType t) -> CXCursor
        {
            t = peel_type(t);
            if (t.kind != CXType_Record && t.kind != CXType_Enum)
            {
                return CXCursor{};
            }
            return api().clang_getTypeDeclaration(t);
        }

        /**
         * @brief 类型先规范再剥 layers 层指针后的拼写。
         *
         * @internal 只经 CXType_Pointer 下探（引用/数组/Invalid 中途终止）；
         * pointee 的 canonical 仍是 canonical，故无需逐层再规范化。模板模式体
         * 内的模板形参在此打印为
         * "type-parameter-`<depth>`-`<index>`"（canonical 无 identifier
         * 的结构性输出，C++ 标识符不含 '-' 不可伪造），调用方以此做
         * 零名字参与的形状判别。
         */
        auto canonical_peeled_spelling(CXType t, int layers)
            -> std::optional<std::string>
        {
            if (layers < 0 || t.kind == CXType_Invalid)
            {
                return std::nullopt;
            }
            CXType cur = api().clang_getCanonicalType(t);
            if (cur.kind == CXType_Invalid)
            {
                return std::nullopt;
            }
            for (int i = 0; i < layers; ++i)
            {
                if (cur.kind != CXType_Pointer)
                {
                    return std::nullopt;
                }
                cur = api().clang_getPointeeType(cur);
                if (cur.kind == CXType_Invalid)
                {
                    return std::nullopt;
                }
            }
            return Str{api().clang_getTypeSpelling(cur)}.std_str();
        }

    } // namespace detail

    /**
     * @brief 表达式静态类型的 canonical 拼写（不剥层）。
     *
     * @internal 重载归属判定：模板上下文的调用点实参（T/T&/T* 等）
     * canonical 含 "type-parameter-"（依赖类型，C++ 标识符不含 '-' 不可
     * 伪造——与既有判据族同源）；无效类型返回空串。
     */
    [[nodiscard]]
    auto canonical_type_spelling(const CXCursor& c) -> std::string
    {
        CXType t = detail::api().clang_getCursorType(c);
        if (t.kind == CXType_Invalid)
        {
            return {};
        }
        t = detail::api().clang_getCanonicalType(t);
        if (t.kind == CXType_Invalid)
        {
            return {};
        }
        return Str{detail::api().clang_getTypeSpelling(t)}.std_str();
    }

    /** @brief 游标声明类型的规范形状（剥 typedef/auto）。 */
    [[nodiscard]]
    auto canonical_type_class(const CXCursor& c) -> TypeClass
    {
        const CXType t = detail::api().clang_getCursorType(c);
        return detail::classify_type(detail::api().clang_getCanonicalType(t));
    }

    /**
     * @brief 函数型游标结果类型的形状（不剥 typedef——依赖名的引用在
     *        canonical 化时会折叠成 Unexposed，raw kind 才保得住引用位）。
     *
     * @internal 非函数游标为 Invalid。消费方：透明转发函数判据的引用
     *           返回证据（模板转发器的 `T&&` raw kind 是 RValueReference，
     *           表达式侧的 cast 类型则已被依赖糖折叠，不可用）。
     */
    [[nodiscard]]
    auto result_type_class(const CXCursor& c) -> TypeClass
    {
        return detail::classify_type(
            detail::api().clang_getCursorResultType(c));
    }

    /**
     * @brief 游标声明类型的规范拼写，先剥 layers 层指针。
     *
     * 层数不足（中途遇到非指针/Invalid）为 nullopt；返回的拼写来自规范
     * 类型，模板形参呈现为 "type-parameter-`<depth>`-`<index>`" 标记
     * （detail::canonical_peeled_spelling 的判别语义）。
     */
    [[nodiscard]]
    auto canonical_pointee_spelling(const CXCursor& c, int layers)
        -> std::optional<std::string>
    {
        return detail::canonical_peeled_spelling(
            detail::api().clang_getCursorType(c),
            layers);
    }

    /** @brief 函数型游标结果类型的同款查询（非函数为 nullopt）。 */
    [[nodiscard]]
    auto canonical_result_pointee_spelling(const CXCursor& c, int layers)
        -> std::optional<std::string>
    {
        return detail::canonical_peeled_spelling(
            detail::api().clang_getCursorResultType(c),
            layers);
    }

    /** @brief 游标类型是否为指针或引用。 */
    [[nodiscard]]
    auto is_pointer_or_reference(const CXCursor& c) -> bool
    {
        const CXType t = detail::api().clang_getCursorType(c);
        return t.kind == CXType_Pointer || t.kind == CXType_LValueReference
               || t.kind == CXType_RValueReference;
    }

    /**
     * @brief 函数游标的结果类型是否为指针/引用。
     *
     * @internal 表达式游标（尤其 CXXOperatorCallExpr）的 `clang_getCursorType`
     * 在 libclang 里可能报成绑定成员函数类型而不是调用结果；判定“调用
     * 返回指针/引用”必须看被调用函数的 result type，否则容器解引用
     * （`operator*`）的静态元素类型会被误当成动态证据。
     */
    [[nodiscard]]
    auto result_is_pointer_or_reference(const CXCursor& c) -> bool
    {
        const CXType t = detail::api().clang_getCursorResultType(c);
        return t.kind == CXType_Pointer || t.kind == CXType_LValueReference
               || t.kind == CXType_RValueReference;
    }

    /** @brief 游标声明类型（剥糖后）的记录/枚举声明游标；否则空游标。 */
    [[nodiscard]]
    auto type_declaration(const CXCursor& c) -> CXCursor
    {
        return detail::declaration_of_type(
            detail::api().clang_getCursorType(c));
    }

    /** @brief 游标类型所指记录/枚举的声明游标；非指针/引用为空游标。 */
    [[nodiscard]]
    auto pointee_declaration(const CXCursor& c) -> CXCursor
    {
        const CXType t = detail::api().clang_getCursorType(c);
        if (t.kind != CXType_Pointer && t.kind != CXType_LValueReference
            && t.kind != CXType_RValueReference)
        {
            return CXCursor{};
        }
        return detail::declaration_of_type(
            detail::api().clang_getPointeeType(t));
    }

    /** @brief 函数型游标返回类型（剥糖后）的记录/枚举声明；否则空游标。 */
    [[nodiscard]]
    auto result_type_declaration(const CXCursor& c) -> CXCursor
    {
        return detail::declaration_of_type(
            detail::api().clang_getCursorResultType(c));
    }

    /**
     * @brief 游标类型的第 i 个模板实参（剥糖后）的记录/枚举声明；
     *        越界或无记录实参时为空游标。
     */
    [[nodiscard]]
    auto type_template_arg_declaration(const CXCursor& c, int i) -> CXCursor
    {
        if (i < 0)
        {
            return CXCursor{};
        }
        CXType t = detail::api().clang_getCursorType(c);
        int    argc = detail::api().clang_Type_getNumTemplateArguments(t);
        if (argc <= i)
        {
            t = detail::api().clang_getCanonicalType(t);
            argc = detail::api().clang_Type_getNumTemplateArguments(t);
        }
        if (argc <= i)
        {
            return CXCursor{};
        }
        return detail::declaration_of_type(
            detail::api().clang_Type_getTemplateArgumentAsType(
                t,
                static_cast<unsigned>(i)));
    }

    /**
     * @brief 访问 parent 的每个直接子（fn 返回 true 时含其整个子树）。
     *
     * 这是 cg.extract 赖以构建的递归走查原语。
     */
    void children(
        const CXCursor&                             parent,
        const std::function<bool(const CXCursor&)>& fn)
    {
        detail::VisitCtx ctx{&fn};
        detail::api().clang_visitChildren(parent, detail::visitor, &ctx);
    }
} // namespace cg::clang
