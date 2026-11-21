/**
 * @module cg.analysis.collect
 *
 * @brief 从 libclang AST 收集动态派发分析所需的采集层事实：每个函数内的
 *        类型线索（声明类型、new/聚合初始化、make_unique/make_shared、
 *        转型、取地址、同函数别名、实参/返回槽）与每个项目内调用点
 *        （接收者名字、静态类型、实参类型）。
 *
 * @internal 只依赖 cg.clang/cg.util/cg.analysis.model。产出的 TuAnalysis
 * 是纯数据（无 CXCursor），与 TuExtract 并列流入串行存储阶段；worker
 * 线程安全由“每个 worker 一个 PathCaseCache”保证。
 */
export module cg.analysis.collect;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang;
import cg.analysis.model;

export namespace cg::analysis
{

    /** @brief 收集选项：项目根与编译数据库教授的路径拼写。 */
    struct CollectOptions
    {
        std::vector<std::string> roots;
        std::vector<std::string> spellings;
        /**
         * @brief 生成区目录前缀（cdb 子树，路径小写规范化形态）；空 = 不降级。
         *
         * @internal 预留：透传包装登记在该子树内跳过（防生成代码膨胀：
         *           生成区符号海量而查询价值趋零，登记收益不抵膨胀）；由 cli
         * 层从 profile 的 compile_commands 路径推导后传入，本环默认空 =
         * 全登记。
         */
        std::string generated_root;
    };

    namespace collect_detail
    {

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

        [[nodiscard]]
        inline auto is_scope_kind(CXCursorKind k) -> bool
        {
            return is_record_kind(k) || k == CXCursor_EnumDecl;
        }

        /**
         * @brief std 容器名单（已批第三类：与 dlopen 系统名单同级的工具链
         *        常量）。容器元素槽的激活面：变量声明类型是本表内类模板
         *        特化且元素类型可解析为项目内记录，才建 "#elem:" 槽。
         */
        inline constexpr std::string_view STD_CONTAINERS[] = {
            "vector",
            "list",
            "forward_list",
            "deque",
            "set",
            "multiset",
            "map",
            "multimap",
            "unordered_set",
            "unordered_multiset",
            "unordered_map",
            "unordered_multimap",
            "queue",
            "stack",
            "priority_queue",
            "array",
            "flat_set",
            "flat_multiset",
            "flat_map",
            "flat_multimap"};

        /** @brief 容器写集方法表（写期动态证据的采集面）。 */
        inline constexpr std::string_view CONTAINER_WRITES[] = {
            "push_back",
            "emplace_back",
            "push_front",
            "push",
            "insert",
            "emplace",
            "emplace_hint",
            "assign"};

        /** @brief 迭代器工厂方法名（assign 区间拷贝形态的识别面）。 */
        inline constexpr std::string_view ITERATOR_FACTORIES[] = {
            "begin",
            "cbegin",
            "rbegin",
            "crbegin",
            "end",
            "cend",
            "rend",
            "crend"};

        [[nodiscard]]
        inline auto fold_eq(char a, char b) -> bool
        {
            return a == b
                   || cg::util::ascii_lower_char(a)
                          == cg::util::ascii_lower_char(b);
        }

        [[nodiscard]]
        inline auto under_roots(
            const std::string&    file,
            const CollectOptions& opt) -> bool
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

        [[nodiscard]]
        inline auto anchored_at_spelling(
            std::string           canonical,
            const CollectOptions& opt) -> std::string
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
                    break;
                }
                p = cg::clang::semantic_parent(p);
            }
            std::ranges::reverse(parts);
            std::string out;
            for (const std::string& part : parts)
            {
                if (!out.empty())
                {
                    out += "::";
                }
                out += part;
            }
            return out;
        }

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
                    parts.push_back(cg::clang::spelling(p));
                }
                else
                {
                    break;
                }
                p = cg::clang::semantic_parent(p);
            }
            std::ranges::reverse(parts);
            std::string out;
            for (const std::string& part : parts)
            {
                if (!out.empty())
                {
                    out += "::";
                }
                out += part;
            }
            return out;
        }

        [[nodiscard]]
        inline auto parent_record_name(const CXCursor& c) -> std::string
        {
            const CXCursor p = cg::clang::semantic_parent(c);
            if (cg::clang::is_null(p) || !is_scope_kind(cg::clang::kind(p)))
            {
                return {};
            }
            return cg::clang::spelling(p);
        }

        /** @brief 一个表达式的直接子游标列表。 */
        [[nodiscard]]
        inline auto direct_children(const CXCursor& c) -> std::vector<CXCursor>
        {
            std::vector<CXCursor> out;
            cg::clang::children(
                c,
                [&](const CXCursor& ch)
                {
                    out.push_back(ch);
                    return false;
                });
            return out;
        }

        /**
         * @brief 红形状开关：物化子节点列表（对照与红绿测量用）。
         *
         * @internal CCG_TEST_COLLECT_MATERIALIZE=1 时 for_each_child 退回
         *           direct_children 物化形态，与改造前逐位等价；函数级 static
         *           求值一次（改环境变量需重启进程）。默认关闭，生产零行为。
         */
        [[nodiscard]]
        inline bool children_materialized_for_test()
        {
            static const bool on =
                cg::util::cstd::get_env("CCG_TEST_COLLECT_MATERIALIZE")
                    .has_value();
            return on;
        }

        /**
         * @brief 子节点是否非空（回调式，零物化）。
         *
         * @internal 与 direct_children().empty() 等价：引擎子链追逐一遍即出，
         *           省掉堆列表。空探针调用点专用。
         */
        [[nodiscard]]
        inline auto has_direct_child(const CXCursor& c) -> bool
        {
            bool any = false;
            cg::clang::children(
                c,
                [&](const CXCursor&)
                {
                    any = true;
                    return false;
                });
            return any;
        }

        /**
         * @brief 回调式流式子节点遍历（"单遍顺序看"的调用点专用）。
         *
         * @internal 与 direct_children + 逐元素循环等价（子节点序即引擎遍历
         *           序），只是省掉每节点一次的堆列表物化——分配器开销与搬运
         *           带宽归零，引擎侧子链追逐两种方式相同。需要随机访问（下标
         *           /首尾/内部搜索）或"要看两遍/找到即停"的调用点保持
         *           direct_children 列表现状（宽口径保留，逐调用点核对过）。
         *           回调返回 false 与范本
         *           （extract.visitor walk_children / capi children）一致：只下
         *           潜一层，调用点需要时自行再调。异常穿越蹦床裸转发——与
         *           抽取侧同险同命。CCG_TEST_COLLECT_MATERIALIZE=1 时退回物化
         *           形态（对照与红绿测量的红形状开关；改环境变量需重启进程）。
         */
        template <typename Fn>
        inline void for_each_child(const CXCursor& c, Fn&& fn)
        {
            if (children_materialized_for_test())
            {
                for (const CXCursor& ch : direct_children(c))
                {
                    fn(ch);
                }
                return;
            }
            cg::clang::children(
                c,
                [&](const CXCursor& ch)
                {
                    fn(ch);
                    return false;
                });
        }

        /**
         * @brief 函数游标的 kind 拼写（unique_key 的第一段）。
         */
        [[nodiscard]]
        inline auto function_kind_of(const CXCursor& c) -> std::string_view
        {
            switch (cg::clang::kind(c))
            {
            case CXCursor_Constructor:
                return "constructor";
            case CXCursor_Destructor:
                return "destructor";
            case CXCursor_CXXMethod:
                return "method";
            default:
                return parent_record_name(c).empty() ? "function" : "method";
            }
        }

        /** @brief 函数形游标的 unique_key（与 cg.extract 逐字节一致）。 */
        [[nodiscard]]
        inline auto function_key_of(const CXCursor& c, const std::string& file)
            -> std::string
        {
            const std::vector<std::string> params =
                cg::clang::arg_type_spellings(c);
            return make_function_key(
                function_kind_of(c),
                scope_of(c),
                cg::clang::spelling(c),
                file,
                sig_hash_of(params),
                cg::clang::is_const_method(c));
        }

    } // namespace collect_detail

    /**
     * @brief 收集一个已解析翻译单元的采集层事实。调用方必须在 crash guard
     *        之后调用（与 extract 同一纪律）。
     */
    [[nodiscard]]
    auto collect(
        const cg::clang::ParsedTu& parsed,
        const CollectOptions&      opt,
        cg::util::PathCaseCache&   case_cache) -> TuAnalysis;

    namespace collect_detail
    {

        struct Ctx
        {
            std::string fn_key;
            /** @brief 当前函数所属记录（字段流线索的所有者）；自由函数为空。 */
            std::string record_key;
            int         param_index = 0;
            /**
             * @brief 当前函数的声明游标（形参直配消费用；lambda 体内等
             *        借用外层函数语境时指向外层）。
             */
            CXCursor fn_decl{};
        };

        /**
         * @brief 出参包装类判据的命中描述（槽位映射供派发管线消费）。
         */
        struct OutparamWrapper
        {
            /** @brief 绑定的 own 模板形参下标 i（canonical
             * type-parameter-0-`<i>`）。 */
            int template_index = -1;
            /** @brief 绑定构造的第 pos 个形参 = 调用点第 pos 个实参（T** 槽）。
             */
            int ctor_param_pos = -1;
            /** @brief 存储字段名。 */
            std::string field;
            /** @brief 写回方法名。 */
            std::string writeback;
        };

        /** @brief 成员链比对判据的结论。 */
        enum class AddressofChain : std::uint8_t
        {
            Tie = 0,       // 两链字段声明全等（放行）
            Reject = 1,    // 存在可解析的不等段（拒）
            Uncertain = 2, // 任一段解析失败（降级；两侧同失败绝不判 tie）
        };

        /** @brief 壳直属 T* 存储字段位（own 形参下标 + 字段名）。 */
        struct ShellTpField
        {
            int         tp_index = -1;
            std::string field_name;
        };

        /** @brief 取址方法判据结果：四条件通过（有值）时携带成员链比对结论。
         */
        struct AddressofMethod
        {
            AddressofChain chain = AddressofChain::Uncertain;
        };

        /**
         * @brief 函数内出参壳绑定（自定义包装句柄形态：构造登记出参槽，Set
         * 写回）。
         *
         * @internal key = (fn_key, 壳变量名)；value 的 owner_key/var_name 是被
         * 绑定的 T** 槽（函数域变量或字段槽），writeback 是出参包装类判据
         * 识别出的写回方法名（出参写回需变量一致与方法名双闸校验）。
         */
        struct OutparamBinding
        {
            std::string owner_key;
            std::string var_name;
            std::string writeback;
        };

        /** @brief 成员链的一段：字段/变量声明身份（或未解析占位）。 */
        struct ChainSegment
        {
            bool resolved = false;
            /** @brief FieldDecl 为 true；VarDecl（静态成员/局部静态）为 false。
             */
            bool        is_field = false;
            std::string usr;
        };

        /** @brief return 表达式的成员访问链（基表达式在前）。 */
        struct MemberChain
        {
            std::vector<ChainSegment> segments;
            /** @brief 每段解析且终止于隐式 this（判 tie 的必要条件）。 */
            bool complete = false;
        };

        /** @brief 单 TU 界外实例化 invoke 采集上限（门控；超限停止并外露）。
         */
        inline constexpr std::size_t EXT_INVOKE_BUDGET = 2000;

        struct Walker
        {
            const CollectOptions&                        opt;
            cg::util::PathCaseCache&                     case_cache;
            TuAnalysis                                   out;
            std::unordered_map<std::string, std::string> path_cache_;
            std::unordered_set<std::string>              hint_seen_;
            /** @brief 透传 getter 登记（getter_key）的 TU 内去重。 */
            std::unordered_set<std::string> passthrough_seen_;
            /** @brief lambda 符号登记（key）的 TU 内去重。 */
            std::unordered_set<std::string> lambda_seen_;
            /**
             * @brief 当前采集语境的函数体游标（lambda 跨域记名闸基准）。
             *
             * @internal lambda 体内语句按首版语义借用外层函数语境采集，本成员
             *           恒指向当前 visit 语境所属的函数体游标（进入函数体时设
             *           置、离开时还原，局部类方法等嵌套函数形不会串味）；命
             *           名空间级初始化递归时为空游标，闸失效（保持既有语义）。
             *           闸定义见 cross_fn_body_decl。
             */
            CXCursor host_fn_{};
            /**
             * @brief 单 TU 界外实例化体 invoke 采集计数（门控）。
             *
             * @internal 初始保守上限 2000/TU；超限停止采集并在 TuAnalysis 层
             *           留 truncated 标记（预算耗尽是可观测事实，不静默）。
             */
            std::size_t ext_invoke_count_ = 0;
            bool        ext_invoke_capped_ = false;
            /** @brief 已扫描的实例 host（TU
             * 内去重：同实例多调用点只扫体一次）。 */
            std::unordered_set<std::string> ext_host_seen_;
            // 结构判据缓存（usr 键）：判据要遍历类成员/方法体，按唯一游标身份
            // 记一次结论；Walker 与 TU 同生命周期，天然按 TU 隔离。
            std::unordered_map<std::string, std::optional<OutparamWrapper>>
                                                  wrapper_cache_;
            std::unordered_map<std::string, bool> shell_cache_;
            /** @brief 壳直属 T* 存储字段缓存（pattern usr -> 字段位）。 */
            std::unordered_map<std::string, std::optional<ShellTpField>>
                shell_field_cache_;
            std::unordered_map<std::string, std::optional<AddressofMethod>>
                addressof_cache_;
            /**
             * @brief 透明转发函数判据缓存（函数 usr -> 透明形参位）。
             *
             * @internal 判据要进函数体数 return 与 cast 形态，按唯一游标身份
             * 记一次结论（判据定义见 transparent_forwarder_param）；与其它
             * 判据缓存同生命周期（TU 内有效，标准库/项目助手每 TU 首次调用
             * 解析一次）。
             */
            std::unordered_map<std::string, std::optional<int>>
                forwarder_cache_;
            /**
             * @brief 透传壳 pointee 实参位缓存（主模板 usr -> 实参位）。
             *
             * @internal 判据要遍历主模板全部零参方法的返回 canonical，按唯一
             *           游标身份记一次结论（判据定义见 passthrough_pointee_
             *           index）；与其它结构判据缓存同生命周期（TU 内有效，
             *           标准库/项目壳每 TU 首次调用解析一次）。
             */
            std::unordered_map<std::string, std::optional<int>>
                passthrough_pointee_cache_;
            /** @brief 断链 4 体追源缓存（方法 usr -> 透传 getter 对象游标）。
             */
            std::unordered_map<std::string, CXCursor> plain_getter_cache_;
            /** @brief 函数内出参壳绑定表：(fn_key, 壳变量名) -> 出参槽。 */
            std::unordered_map<std::string, OutparamBinding> outparam_bindings_;
            /**
             * @brief 声明期容器元素槽登记表：(owner_key, 容器名)。
             *
             * @internal 容器元素槽闸门：写集/拷贝流/range-for
             * 链接行只在容器先于 使用点完成声明期元素登记后才发（同函数/同 TU
             * 内声明先于使用， C++ 语义保证；跨 TU 的字段声明不在本 TU 时
             * record key 为空、 登记失败、整链 no-op——宁漏勿误）。
             */
            std::unordered_set<std::string> elem_slots_;

            explicit Walker(
                const CollectOptions&    o,
                cg::util::PathCaseCache& cases)
                : opt(o), case_cache(cases)
            {
            }

            [[nodiscard]]
            auto norm(const cg::clang::SourceLocation& loc) -> std::string
            {
                const std::string raw{loc.file};
                if (const auto hit = path_cache_.find(raw);
                    hit != path_cache_.end())
                {
                    return hit->second;
                }
                std::string result = anchored_at_spelling(
                    case_cache.canonical_case_spelling(
                        cg::util::collapse_dot_segments(loc.file)),
                    opt);
                path_cache_.emplace(raw, result);
                return result;
            }

            /** @brief 记录声明游标 -> record unique_key（项目外为空）。 */
            [[nodiscard]]
            auto record_key_of_decl(const CXCursor& decl) -> std::string
            {
                if (cg::clang::is_null(decl)
                    || !is_record_kind(cg::clang::kind(decl)))
                {
                    return {};
                }
                const CXCursor primary = cg::clang::specialized_template(decl);
                CXCursor       use = decl;
                if (!cg::clang::is_null(primary))
                {
                    const CXCursor def = cg::clang::definition_of(primary);
                    if (!cg::clang::is_null(def)
                        && is_record_kind(cg::clang::kind(def))
                        && under_roots(norm(cg::clang::location(def)), opt))
                    {
                        use = def;
                    }
                }
                const std::string file = norm(cg::clang::location(use));
                if (!under_roots(file, opt))
                {
                    return {};
                }
                return make_record_key(
                    scope_of(use),
                    cg::clang::spelling(use),
                    file);
            }

            /** @brief 游标声明类型（剥糖）的记录 key；非记录为空。 */
            [[nodiscard]]
            auto declared_type_key(const CXCursor& c) -> std::string
            {
                return record_key_of_decl(cg::clang::type_declaration(c));
            }

            /**
             * @brief 剥掉 libclang
             * 的透明表达式包装（UnexposedExpr/ParenExpr）。
             *
             * @internal 本 libclang 把 `new Impl` 初始化式报为 UnexposedExpr
             * （其子才是实际表达式）；不剥会让采集层的 new/转型线索全部丢失。
             */
            [[nodiscard]]
            static auto unwrap_expr(const CXCursor& e) -> CXCursor
            {
                CXCursor cur = e;
                for (int depth = 0; depth < 16; ++depth)
                {
                    const CXCursorKind k = cg::clang::kind(cur);
                    if (k != CXCursor_UnexposedExpr && k != CXCursor_ParenExpr)
                    {
                        return cur;
                    }
                    const std::vector<CXCursor> kids = direct_children(cur);
                    if (kids.size() != 1)
                    {
                        return cur;
                    }
                    cur = kids.front();
                }
                return cur;
            }

            /**
             * @brief 被调子式是否显式写了嵌套名限定符
             *        （ns:: / Base:: / ::）。
             *
             * @internal 双判据并集，与被调 extent 词法化找 "::" 的 token
             * 判据在标准样本集上逐位等价：引用名范围差覆盖成员限定
             * （p->Base::f() / Base::f()）、全局限定（::fn）与命名空间
             * 限定；被调子式 direct children 的 NamespaceRef 覆盖依赖
             * 重载集（范围差对该形态失效）。TypeRef/TemplateRef 不计入
             * 子游标判据：显式模板实参对任意类型实参都产出 TypeRef
             * （take<PlainT>(x) 无限定却带 TypeRef），计入即误报；带
             * 命名空间的模板实参（take<ns::T>(x)）由 NamespaceRef 命中，
             * 与 token 判据同值。token 判据的两类既有漏报——宏展开错位
             * 被 token 卫生闸拒绝、接收者带括号形态（(&d)->Base::f()）
             * 的首个 '(' 提前截断——都被本判据从 AST 结构读出真值。
             */
            [[nodiscard]]
            static auto qualified_callee(const CXCursor& callee) -> bool
            {
                if (cg::clang::reference_name_includes_qualifier(callee))
                {
                    return true;
                }
                for (const CXCursor& kid : direct_children(callee))
                {
                    if (cg::clang::kind(kid) == CXCursor_NamespaceRef)
                    {
                        return true;
                    }
                }
                return false;
            }

            /** @brief 表达式是否类型引用子（转型目标/模板名，不是操作数）。 */
            [[nodiscard]]
            static auto is_type_ref(const CXCursor& c) -> bool
            {
                const CXCursorKind k = cg::clang::kind(c);
                return k == CXCursor_TypeRef || k == CXCursor_TemplateRef
                       || k == CXCursor_NamespaceRef
                       || k == CXCursor_OverloadCandidate;
            }

            /** @brief 显式转型的第一个真实操作数子表达式；没有则空游标。 */
            [[nodiscard]]
            static auto cast_operand(const CXCursor& cast) -> CXCursor
            {
                CXCursor found{};
                for_each_child(
                    cast,
                    [&](const CXCursor& kid)
                    {
                        if (cg::clang::is_null(found) && !is_type_ref(kid))
                        {
                            found = kid;
                        }
                    });
                return found;
            }

            /** @brief
             * 是否显式转型表达式（static/dynamic/functional/const/reinterpret）。
             */
            [[nodiscard]]
            static auto is_cast_expr(CXCursorKind k) -> bool
            {
                return k == CXCursor_CXXStaticCastExpr
                       || k == CXCursor_CXXDynamicCastExpr
                       || k == CXCursor_CXXFunctionalCastExpr
                       || k == CXCursor_CXXConstCastExpr
                       || k == CXCursor_CXXReinterpretCastExpr ||
                       // C 风格转型（(IFn)p / (void*)&f——逃逸集形态的主
                       // 流包裹层）补入；此前 strip_casts 对其整层不剥。
                       k == CXCursor_CStyleCastExpr;
            }

            /**
             * @brief 剥掉透明包装与显式转型，返回被转型的值表达式。
             *
             * @internal 用于 `set(static_cast<I*>(p))` / `q =
             * static_cast<I*>(make())`
             * 的传播：转型目标类型是静态信息，真实动态证据在操作数一侧。
             * 只用于“传播边”（ref_var_name / call_result_key /
             * hint_from_expr）， 不用于把目标类型当作证据。
             */
            [[nodiscard]]
            static auto strip_casts(const CXCursor& e) -> CXCursor
            {
                CXCursor cur = e;
                for (int depth = 0; depth < 16; ++depth)
                {
                    cur = unwrap_expr(cur);
                    if (!is_cast_expr(cg::clang::kind(cur)))
                    {
                        return cur;
                    }
                    const CXCursor operand = cast_operand(cur);
                    if (cg::clang::is_null(operand))
                    {
                        return cur;
                    }
                    cur = operand;
                }
                return cur;
            }

            /** @brief derived 的基类闭包中是否包含 base（记录 key 相等）。 */
            [[nodiscard]]
            auto derives_from(
                const CXCursor& derived,
                const CXCursor& base,
                int             depth) -> bool
            {
                if (depth > 8 || cg::clang::is_null(derived)
                    || cg::clang::is_null(base))
                {
                    return false;
                }
                const std::string derived_key = record_key_of_decl(derived);
                const std::string base_key = record_key_of_decl(base);
                if (derived_key.empty() || base_key.empty())
                {
                    return false; // 项目外类型无法证明，宁可不猜
                }
                if (derived_key == base_key)
                {
                    return true;
                }
                bool found = false;
                for_each_child(
                    derived,
                    [&](const CXCursor& kid)
                    {
                        if (found
                            || cg::clang::kind(kid)
                                   != CXCursor_CXXBaseSpecifier)
                        {
                            return;
                        }
                        const CXCursor base_decl =
                            cg::clang::type_declaration(kid);
                        if (derives_from(base_decl, base, depth + 1))
                        {
                            found = true;
                        }
                    });
                return found;
            }

            /**
             * @brief 转型是否可证明为向下转型（目标记录派生自操作数静态记录）。
             *
             * @internal 只有这种情形才允许把转型目标保留为 Low 候选；上转型/
             * 同类型转型的目标类型绝不能成为动态证据。
             */
            [[nodiscard]]
            auto is_provable_downcast(const CXCursor& cast) -> bool
            {
                const CXCursor operand = cast_operand(cast);
                if (cg::clang::is_null(operand))
                {
                    return false;
                }
                const CXCursor target = cg::clang::type_declaration(cast);
                const CXCursor source =
                    cg::clang::type_declaration(unwrap_expr(operand));
                if (cg::clang::is_null(target) || cg::clang::is_null(source))
                {
                    return false;
                }
                const std::string target_key = record_key_of_decl(target);
                const std::string source_key = record_key_of_decl(source);
                if (target_key.empty() || source_key.empty()
                    || target_key == source_key)
                {
                    return false;
                }
                return derives_from(target, source, 0);
            }

            /**
             * @brief cast_claim_target 的廉价入口闸：原始 kind 是三种转型
             *        或透明包装（Unexposed/Paren）时才可能有声称。
             *
             * @internal 全库百万级赋值/接收者表达式的早退判定前移——一次
             *           kind 读取替代 unwrap 往返；unwrap 只剥 Unexposed/
             *           Paren 两类包装，其余 kind 经 unwrap 后必非转型，
             *           闸上跳过与函数内判空逐字节同值。
             */
            [[nodiscard]]
            static auto cast_claim_shape(CXCursorKind k) -> bool
            {
                switch (k)
                {
                case CXCursor_CXXStaticCastExpr:
                case CXCursor_CXXReinterpretCastExpr:
                case CXCursor_CStyleCastExpr:
                case CXCursor_UnexposedExpr:
                case CXCursor_ParenExpr:
                    return true;
                default:
                    return false;
                }
            }

            /**
             * @brief 转型声称目标：指针转型在操作数
             *        无任何动态证据时，目标记录 key 直接接受为槽的类型声称。
             *
             * @internal 形态边界（AST 判据，不做文本猜测）：仅
             *           reinterpret_cast / static_cast / C 样式三种指针/引用
             *           转型——dynamic_cast 自带运行时下发语义（既有 CastDown
             *           判据维持）、const_cast 不改类型、值转型走 expr_type_keys
             *           既有值档；操作数携带动态证据（new/值记录等，与
             *           hint_from_expr 转型臂的 operand_has_evidence 同一口
             *           径）时指针来源已知，返回空——既有 CastDown/传播语义
             *           逐字节保持。语义担保方是程序员的转型声称：声称行只落
             *           库不建边，槽终态无任何动态证据时才由传播侧填入（更强
             *           证据在场绝不覆盖）。
             */
            [[nodiscard]]
            auto cast_claim_target(const CXCursor& expr) -> std::string
            {
                const CXCursor     cur = unwrap_expr(expr);
                const CXCursorKind k = cg::clang::kind(cur);
                if (k != CXCursor_CXXStaticCastExpr
                    && k != CXCursor_CXXReinterpretCastExpr
                    && k != CXCursor_CStyleCastExpr)
                {
                    return {};
                }
                if (!cg::clang::is_pointer_or_reference(cur))
                {
                    return {}; // 值转型由 expr_type_keys 既有值档承载
                }
                const CXCursor operand = cast_operand(cur);
                if (cg::clang::is_null(operand))
                {
                    return {};
                }
                if (!expr_type_keys(strip_casts(cur)).empty())
                {
                    return {}; // 指针来源已知：既有转型证据语义不动
                }
                return record_key_of_decl(cg::clang::type_declaration(cur));
            }

            /**
             * @brief 构造调用是否显式智能指针类型（unique_ptr/shared_ptr）。
             *
             * @internal 两名是 std 公开类型（已批名单），按全词匹配：子串
             * 形态会误中名字里恰好内嵌该子串的任意自定义类型。拼写中它们
             * 必以独立词出现（前邻 `::`、后邻 `<`），全词不改变既批命中。
             */
            [[nodiscard]]
            static auto is_smart_pointer_construction(const CXCursor& e) -> bool
            {
                const std::string ty = cg::clang::type_spelling(e);
                const auto        has_word = [&ty](std::string_view word)
                {
                    const auto ident = [](char c)
                    {
                        return std::isalnum(static_cast<unsigned char>(c))
                               || c == '_';
                    };
                    for (std::size_t pos = ty.find(word);
                         pos != std::string::npos;
                         pos = ty.find(word, pos + 1))
                    {
                        if ((pos == 0 || !ident(ty[pos - 1]))
                            && (pos + word.size() == ty.size()
                                || !ident(ty[pos + word.size()])))
                        {
                            return true;
                        }
                    }
                    return false;
                };
                return has_word("unique_ptr") || has_word("shared_ptr");
            }

            // ============ 出参包装类/取址方法结构判据（零项目名参与）
            // ============
            // 以下判据函数按派发可行性实现：出参包装类
            // 四判据、取址方法判据四条件 + 成员链比对。判据只依赖
            // canonical 类型的 type-parameter- 标记（依据：C++ 标识符
            // 不含 '-'，不可伪造，零误报）。消费方已接线：
            // is_outparam_wrapper_class 驱动出参壳绑定
            // （bind_outparam_shell / outparam_writeback_call），
            // is_addressof_method 驱动取址槽实参（fill_arg_records）。

            /** @brief InvalidFile 系垃圾游标判定（specialized 对 pattern 成员
             *         会返回 InvalidFile 非空垃圾，is_null 不覆盖该区间）。 */
            [[nodiscard]]
            static auto is_invalid_cursor(const CXCursor& c) -> bool
            {
                const CXCursorKind k = cg::clang::kind(c);
                return k >= CXCursor_FirstInvalid && k <= CXCursor_LastInvalid;
            }

            /**
             * @brief "type-parameter-0-`<i>`" 核心拼写 -> own 下标 i；否则 -1。
             *
             * @internal 深度非 0（成员模板形参）与下标越界都拒；own_count 是类
             * 主模板直接 TemplateTypeParameter 子节点数（按序即下标）。
             */
            [[nodiscard]]
            static auto own_tp_index(const std::string& spelling, int own_count)
                -> int
            {
                constexpr std::string_view PREFIX = "type-parameter-0-";
                if (!spelling.starts_with(PREFIX))
                {
                    return -1;
                }
                int index = 0;
                for (const char ch :
                     std::string_view{spelling}.substr(PREFIX.size()))
                {
                    if (ch < '0' || ch > '9' || index > 8)
                    {
                        return -1; // 非数字尾巴（const 限定/依赖糖）拒
                    }
                    index = index * 10 + (ch - '0');
                }
                return index < own_count ? index : -1;
            }

            /**
             * @brief 写回证明：`*<field> = <值>` 形态的赋值在方法体内存在。
             *
             * @internal 赋值按 AST opcode 判定（CX_BO_Assign；'=='/'+=' 是
             * 别的 opcode）；LHS 必须是解引用（UnaryOperator '*'），解引用
             * 目标优先按 FieldDecl USR 比对，引用解析失败时按字段名（规
             * 格口径）。索引槽 `slot[i] = v`（LHS 是 ArraySubscript）与静
             * 态写回因此被拒——宁漏勿误。
             */
            [[nodiscard]]
            static auto writes_through_deref(
                const CXCursor& method,
                const CXCursor& field_decl) -> bool
            {
                const std::string field_usr = cg::clang::usr(field_decl);
                const std::string field_name = cg::clang::spelling(field_decl);
                // 有界 DFS：方法体内全部赋值
                // BinaryOperator（节点上限防病态体）。
                std::vector<CXCursor> stack = direct_children(method);
                for (int budget = 4096; budget > 0 && !stack.empty(); --budget)
                {
                    const CXCursor c = stack.back();
                    stack.pop_back();
                    if (cg::clang::kind(c) == CXCursor_BinaryOperator)
                    {
                        if (cg::clang::binary_opcode(c) == CX_BO_Assign)
                        {
                            const std::vector<CXCursor> kids =
                                direct_children(c);
                            if (kids.empty())
                            {
                                continue;
                            }
                            const CXCursor lhs = unwrap_expr(kids.front());
                            const std::vector<std::string> lhs_tokens =
                                cg::clang::token_spellings(lhs);
                            const std::vector<CXCursor> lhs_kids =
                                direct_children(lhs);
                            if (cg::clang::kind(lhs) == CXCursor_UnaryOperator
                                && !lhs_tokens.empty()
                                && lhs_tokens.front() == "*"
                                && !lhs_kids.empty())
                            {
                                const CXCursor target =
                                    unwrap_expr(lhs_kids.front());
                                const CXCursor ref =
                                    cg::clang::referenced(target);
                                if (cg::clang::kind(ref) == CXCursor_FieldDecl)
                                {
                                    if (cg::clang::usr(ref) == field_usr)
                                    {
                                        return true;
                                    }
                                }
                                else if (
                                    cg::clang::spelling(target) == field_name)
                                {
                                    return true;
                                }
                            }
                        }
                    }
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { stack.push_back(ch); });
                }
                return false;
            }

            /**
             * @brief 出参包装类结构判据（替代对具体包装类型的点名）。
             *
             * 输入 ClassTemplate 主模板游标。own(i) := C 的直接
             * TemplateTypeParameter 子节点按序（深度 0；成员模板形参深度 1 天然
             * 排除）。dp(i) := canonical 为
             * Pointer(Pointer("type-parameter-0-`<i>`")) 且 i ∈ own；sp(i)
             * 同理一层。命中 = 四条合取：绑定构造——∃构造函数某形参
             * dp(i)，记形参序号 pos；存储字段——index 匹配的 dp 字段候选恰 1
             * 个（同一 i；静态成员 是 VarDecl 天然排除）；写回方法——
             * ∃非静态方法恰 1 形参 canonical 为 sp(i)（同一 i，
             *      方法名任意）；
             *   写回证明——写回方法体内存在 `*<存储字段> = <值>` 赋值
             *      （writes_through_deref）。
             * 多个 dp 绑定形参（binds>1）时 no-op 返回空；dp 字段候选 >1 时
             * 同理 no-op（宁漏勿误，两侧对称设防）。
             *
             * @internal soundness 边界：误收面由写回证明封杀（无写穿即非出
             * 参）；双 dp 字段候选的"绑 A 写 B"错位形态（构造绑
             * slot_、写回写 other_，四条判据机械全过但绑定槽从未被写）由存储
             * 字段唯一性封杀——首个匹配选法无法证明绑定目标，整类 no-op。
             * 明确 fallback（不命中，漏收方向）：绑定走 bind() 方法而非构造、非
             * 模板包装类、索引槽写回 slot[i]=v、写回形参 T&/智能指针、深度 ≥1
             * 绑定形参（成员模板构造，canonical 撞拼写被 own 下标拒）、双 dp 字
             * 段（含合法的同槽双字段，无法区分时一并放弃）。
             *
             * @return 命中时给出槽位映射（template_index/ctor_param_pos
             * 供调用点 实参对位）；未命中为空。
             */
            /**
             * @brief 透传包装判据（槽位基建，透传 getter 形态）：类模板
             *        C`<T...>` 存在零参成员函数且返回类型直接引用 own 模板形参
             *        （T / T* / T& / T**，canonical 含 type-parameter-0-）→
             *        该类是透传包装，该方法是透传 getter。
             *
             * @internal 与 is_outparam_wrapper_class 同判据族（零项目名参与，
             *           canonical type-parameter- 标记不可伪造）。覆盖
             *           atomic`<T>`::load / optional`<T>`::value /
             * shared_ptr`<T>`::get / `Wrapper`<T>`
             * 及任意自定义包装。嵌套别名形态（inner`<T>`:: type
             * 的间接透传）明确不收——漏收方向。消费侧（类型集 解包把 arg record
             * 并入 receiver 槽）由透传闸挂接。
             *
             * @return 命中时给出 getter 的 own 形参下标；未命中为空。
             */
            [[nodiscard]]
            static auto is_passthrough_getter(
                const CXCursor& method,
                int             own_count,
                int             max_layers = 2) -> int
            {
                int parm_count = 0;
                for_each_child(
                    method,
                    [&](const CXCursor& p)
                    {
                        if (cg::clang::kind(p) == CXCursor_ParmDecl)
                        {
                            ++parm_count;
                        }
                    });
                if (parm_count != 0)
                {
                    return -1;
                }
                // 值形态 T：结果类型 canonical 全串即 type-parameter-0-`<i>`。
                if (const auto core =
                        cg::clang::canonical_result_pointee_spelling(method, 0))
                {
                    const int i = own_tp_index(*core, own_count);
                    if (i >= 0)
                    {
                        return i;
                    }
                }
                // 指针/引用形态：结果类型剥 1-2 层后前缀匹配。
                for (int layers = 1; layers <= max_layers; ++layers)
                {
                    if (const auto core =
                            cg::clang::canonical_result_pointee_spelling(
                                method,
                                layers))
                    {
                        const int i = own_tp_index(*core, own_count);
                        if (i >= 0)
                        {
                            return i;
                        }
                    }
                }
                return -1;
            }

            [[nodiscard]]
            static auto is_outparam_wrapper_class(const CXCursor& cls)
                -> std::optional<OutparamWrapper>
            {
                if (cg::clang::kind(cls) != CXCursor_ClassTemplate)
                {
                    return std::nullopt;
                }
                int own_count = 0;
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch)
                            == CXCursor_TemplateTypeParameter)
                        {
                            ++own_count;
                        }
                    });
                if (own_count == 0)
                {
                    return std::nullopt;
                }
                struct Binding
                {
                    int index;
                    int pos;
                };
                struct FieldShape
                {
                    int      index;
                    CXCursor decl;
                };
                struct WritebackShape
                {
                    int      index;
                    CXCursor decl;
                };
                std::vector<Binding>        bindings;
                std::vector<FieldShape>     fields;
                std::vector<WritebackShape> writebacks;
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        const CXCursorKind k = cg::clang::kind(ch);
                        if (k == CXCursor_FieldDecl)
                        {
                            if (const auto core =
                                    cg::clang::canonical_pointee_spelling(
                                        ch,
                                        2))
                            {
                                const int i = own_tp_index(*core, own_count);
                                if (i >= 0)
                                {
                                    fields.push_back({i, ch});
                                }
                            }
                            return;
                        }
                        if (k == CXCursor_Constructor)
                        {
                            int pos = -1;
                            for_each_child(
                                ch,
                                [&](const CXCursor& p)
                                {
                                    if (cg::clang::kind(p) != CXCursor_ParmDecl)
                                    {
                                        return;
                                    }
                                    ++pos;
                                    if (const auto core = cg::clang::
                                            canonical_pointee_spelling(p, 2))
                                    {
                                        const int i =
                                            own_tp_index(*core, own_count);
                                        if (i >= 0)
                                        {
                                            bindings.push_back({i, pos});
                                        }
                                    }
                                });
                            return;
                        }
                        if (k == CXCursor_CXXMethod
                            && !cg::clang::is_static_method(ch))
                        {
                            int parm_count = 0;
                            int wb_index = -1;
                            for_each_child(
                                ch,
                                [&](const CXCursor& p)
                                {
                                    if (cg::clang::kind(p) != CXCursor_ParmDecl)
                                    {
                                        return;
                                    }
                                    ++parm_count;
                                    if (wb_index < 0)
                                    {
                                        if (const auto core = cg::clang::
                                                canonical_pointee_spelling(
                                                    p,
                                                    1))
                                        {
                                            wb_index =
                                                own_tp_index(*core, own_count);
                                        }
                                    }
                                });
                            if (parm_count == 1 && wb_index >= 0)
                            {
                                writebacks.push_back({wb_index, ch});
                            }
                        }
                    });
                if (bindings.size() != 1)
                {
                    return std::nullopt; // 无绑定，或多 T** 绑定形参：no-op
                }
                const Binding bind = bindings.front();
                const auto    field_matches = [&](const FieldShape& f)
                { return f.index == bind.index; };
                // 存储字段唯一性：index 匹配的 dp 字段候选必须恰 1
                // 个。双候选时首 个匹配选法无法证明构造绑定的就是它（构造绑
                // slot_ 写 other_，机械过四条判据但绑定槽从未被写）——与 binds>1
                // 对称 的宁漏勿误，整类 no-op。
                if (std::ranges::count_if(fields, field_matches) != 1)
                {
                    return std::nullopt; // 无字段，或多 dp 字段候选
                }
                const auto field = std::ranges::find_if(fields, field_matches);
                const auto writeback = std::ranges::find_if(
                    writebacks,
                    [&](const WritebackShape& w)
                    {
                        return w.index == bind.index
                               && writes_through_deref(w.decl, field->decl);
                    });
                if (writeback == writebacks.end())
                {
                    return std::nullopt; // 无写回方法，或写回证明不过
                }
                OutparamWrapper out;
                out.template_index = bind.index;
                out.ctor_param_pos = bind.pos;
                out.field = cg::clang::spelling(field->decl);
                out.writeback = cg::clang::spelling(writeback->decl);
                return out;
            }

            /**
             * @brief 抽取方法 return 表达式的成员访问链（成员链比对的一侧）。
             *
             * @internal 归一
             * &F/F/*F（至多剥一层前缀取址/解引用）；MemberRefExpr
             * 逐段下推基表达式，段身份 = referenced 的 FieldDecl
             * USR（InvalidFile
             * 等垃圾游标记未解析段，不终止下推——同声明异实例要靠更深的段区
             * 分）；DeclRefExpr 到 VarDecl 记静态成员段（类静态/局部静态）；
             * 基表达式类型是指针（s_->cur_ 的隐式解引用）、数组下标、转发调用、
             * nullptr、多 return、无体都产生未解析段或不完整链。终止：隐式
             * this（无基子）或 CXXThisExpr。
             */
            [[nodiscard]]
            static auto member_chain_of(const CXCursor& method) -> MemberChain
            {
                MemberChain out;
                if (!cg::clang::is_definition(method))
                {
                    return out; // 无体：链不可解析（降级）
                }
                std::vector<CXCursor> returns;
                std::vector<CXCursor> stack = direct_children(method);
                while (!stack.empty() && returns.size() <= 2)
                {
                    const CXCursor c = stack.back();
                    stack.pop_back();
                    if (cg::clang::kind(c) == CXCursor_ReturnStmt)
                    {
                        returns.push_back(c);
                        continue;
                    }
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { stack.push_back(ch); });
                }
                if (returns.size() != 1)
                {
                    return out; // 无 return / 多 return：无法证明单一链
                }
                const std::vector<CXCursor> ret_kids =
                    direct_children(returns.front());
                if (ret_kids.empty())
                {
                    return out;
                }
                CXCursor e = unwrap_expr(ret_kids.front());
                if (cg::clang::kind(e) == CXCursor_UnaryOperator)
                {
                    const std::vector<std::string> toks =
                        cg::clang::token_spellings(e);
                    if (!toks.empty()
                        && (toks.front() == "&" || toks.front() == "*"))
                    {
                        const std::vector<CXCursor> uk = direct_children(e);
                        if (!uk.empty())
                        {
                            e = unwrap_expr(uk.front());
                        }
                    }
                }
                std::vector<ChainSegment> leaf_first;
                bool                      terminal = false;
                for (int depth = 0; depth < 8 && !terminal; ++depth)
                {
                    e = unwrap_expr(e);
                    const CXCursorKind k = cg::clang::kind(e);
                    if (k == CXCursor_MemberRefExpr)
                    {
                        const CXCursor ref = cg::clang::referenced(e);
                        if (cg::clang::kind(ref) == CXCursor_FieldDecl)
                        {
                            leaf_first.push_back(
                                {true, true, cg::clang::usr(ref)});
                        }
                        else
                        {
                            leaf_first.push_back({}); // InvalidFile：未解析段
                        }
                        const std::vector<CXCursor> kids = direct_children(e);
                        if (kids.empty())
                        {
                            terminal = true; // 隐式 this 成员访问无基子
                            break;
                        }
                        const CXCursor base = kids.front();
                        if (cg::clang::canonical_type_class(base)
                            == cg::clang::TypeClass::Pointer)
                        {
                            leaf_first.push_back(
                                {}); // 经指针基（s_->f）：解析失败
                            break;
                        }
                        e = base;
                        continue;
                    }
                    if (k == CXCursor_DeclRefExpr)
                    {
                        const CXCursor ref = cg::clang::referenced(e);
                        if (cg::clang::kind(ref) == CXCursor_VarDecl)
                        {
                            leaf_first.push_back(
                                {true, false, cg::clang::usr(ref)});
                            terminal = true;
                        }
                        else
                        {
                            leaf_first.push_back({});
                        }
                        break;
                    }
                    if (k == CXCursor_CXXThisExpr)
                    {
                        terminal = true;
                        break;
                    }
                    leaf_first.push_back({}); // 数组下标/转发调用/nullptr/…
                    break;
                }
                std::ranges::reverse(leaf_first);
                out.complete =
                    terminal
                    && std::ranges::all_of(
                        leaf_first,
                        [](const ChainSegment& s) { return s.resolved; });
                out.segments = std::move(leaf_first);
                return out;
            }

            /**
             * @brief 完整成员链比对：operator-> 与取址方法各自 return 链。
             *
             * @internal 基表达式在前逐段比较：两段都可解析且不等 -> Reject（可
             * 证明的异槽，如 Cache::spare 的 spare_ 对 cur_、同声明异实例的
             * a_ 对 b_）；任何未解析段（单侧或双侧）-> Uncertain（宁可降级不冒
             * 充 tie——两侧同 InvalidFile 绝不判 tie）；两链同长、每段全等且都
             * 完整到达隐式 this -> Tie。一侧结束另一侧还有段：较短侧完整且剩
             * 余段可解析 = 深度不等 -> Reject，否则 Uncertain。
             */
            [[nodiscard]]
            static auto compare_member_chains(
                const CXCursor& lhs_method,
                const CXCursor& rhs_method) -> AddressofChain
            {
                const MemberChain lhs = member_chain_of(lhs_method);
                const MemberChain rhs = member_chain_of(rhs_method);
                for (std::size_t i = 0;; ++i)
                {
                    const bool l_end = i >= lhs.segments.size();
                    const bool r_end = i >= rhs.segments.size();
                    if (l_end && r_end)
                    {
                        return lhs.complete && rhs.complete
                                   ? AddressofChain::Tie
                                   : AddressofChain::Uncertain;
                    }
                    if (l_end || r_end)
                    {
                        const MemberChain& shorter = l_end ? lhs : rhs;
                        if (!shorter.complete)
                        {
                            return AddressofChain::Uncertain;
                        }
                        const ChainSegment& rest =
                            l_end ? rhs.segments[i] : lhs.segments[i];
                        return rest.resolved ? AddressofChain::Reject
                                             : AddressofChain::Uncertain;
                    }
                    const ChainSegment& a = lhs.segments[i];
                    const ChainSegment& b = rhs.segments[i];
                    if (!a.resolved || !b.resolved)
                    {
                        return AddressofChain::Uncertain;
                    }
                    if (a.usr != b.usr || a.is_field != b.is_field)
                    {
                        return AddressofChain::Reject;
                    }
                }
            }

            /**
             * @brief 取址语义方法判据（X.M() 的 M 返回 "&operator->
             * 槽位"）。
             *
             * 输入方法游标（调用点的 referenced 或模板 pattern 声明均可；特化
             * 游标先经 specialized 归一到 pattern——pattern 成员对 specialized
             * 回答 InvalidFile 垃圾，按无效回退原游标）。四条合取（首条保证两
             * 端同处 pattern 主语境，编号一致）：
             *   方法非模板——pattern 的 kind 必须是
             * CXXMethod（FunctionTemplate 直拒——成员模板形参 U 会被重编号为
             * type-parameter-0-0 与类 形参 T 撞拼写，实测假阳性）；
             * 所属类指针式——语义父类（特化先归一到类模板
             * pattern）有 operator->，其结果 canonical 为 Pointer 且 pointee
             * spelling 含 "type-parameter-"（记拼写 P）；方法结果 canonical
             * 为双层指针 Pointer(Pointer(...))；最内 pointee 拼写 ==
             * P 字符串相等。 通过后用 compare_member_chains 比对
             * operator-> 与方法的 return 成员链。
             *
             * @internal 已知 FN（漏收方向，不产边）：operator-> 在基类而方法在
             * 派生类（类 pattern 查找无基类回溯）；const T* operator-> 与
             * T** 并存（最内拼写比对挡，'const type-parameter-0-0' !=
             * 'type-parameter-0-0'）；void** 返回（最内拼写比对挡，inner 是
             * 'void'）；非模板指针式类（所属类指针式条件挡）。
             * 消费端守卫与置信分层属派发管线，不在本函数。
             *
             * @return 四条件通过时携带成员链比对结论；否则为空（不激活）。
             */
            [[nodiscard]]
            static auto is_addressof_method(const CXCursor& method)
                -> std::optional<AddressofMethod>
            {
                CXCursor pm = cg::clang::specialized_template(method);
                if (is_invalid_cursor(pm) || cg::clang::is_null(pm))
                {
                    pm = method; // pattern 游标：specialized 只回垃圾
                }
                if (cg::clang::kind(pm) != CXCursor_CXXMethod)
                {
                    return std::nullopt; // 方法非模板条件拒
                }
                CXCursor cls = cg::clang::semantic_parent(pm);
                if (cg::clang::is_null(cls) || is_invalid_cursor(cls)
                    || !is_record_kind(cg::clang::kind(cls)))
                {
                    return std::nullopt;
                }
                const CXCursor primary = cg::clang::specialized_template(cls);
                if (!cg::clang::is_null(primary) && !is_invalid_cursor(primary)
                    && is_record_kind(cg::clang::kind(primary)))
                {
                    cls = primary; // 特化归一到类模板 pattern
                }
                // 类 pattern 直属的 operator->（无基类回溯，已知 FN）。
                CXCursor    arrow{};
                std::string p_spelling;
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        if (!p_spelling.empty())
                        {
                            return; // 已命中：找到即停（其余子节点只做守卫判空）
                        }
                        const CXCursorKind k = cg::clang::kind(ch);
                        if ((k != CXCursor_CXXMethod
                             && k != CXCursor_FunctionTemplate)
                            || cg::clang::spelling(ch) != "operator->")
                        {
                            return;
                        }
                        if (const auto pointee =
                                cg::clang::canonical_result_pointee_spelling(
                                    ch,
                                    1);
                            pointee
                            && pointee->find("type-parameter-")
                                   != std::string::npos)
                        {
                            arrow = ch;
                            p_spelling = *pointee;
                        }
                    });
                if (p_spelling.empty())
                {
                    return std::nullopt; // 所属类非指针式
                }
                const auto inner =
                    cg::clang::canonical_result_pointee_spelling(pm, 2);
                if (!inner || *inner != p_spelling)
                {
                    return std::nullopt; // 结果非双层指针，或最内拼写不匹配
                }
                AddressofMethod out;
                out.chain = compare_member_chains(arrow, pm);
                return out;
            }

            /** @brief 取址方法判据的 usr 缓存包装（消费方取址实参解析用）。 */
            [[nodiscard]]
            auto addressof_method(const CXCursor& m)
                -> std::optional<AddressofMethod>
            {
                const std::string key = cg::clang::usr(m);
                auto              it = addressof_cache_.find(key);
                if (it == addressof_cache_.end())
                {
                    it = addressof_cache_.emplace(key, is_addressof_method(m))
                             .first;
                }
                return it->second;
            }

            /** @brief 方法/构造归一到模板 pattern 成员（特化 -> pattern）。 */
            [[nodiscard]]
            static auto pattern_function_of(const CXCursor& fn) -> CXCursor
            {
                CXCursor pm = cg::clang::specialized_template(fn);
                if (is_invalid_cursor(pm) || cg::clang::is_null(pm))
                {
                    return fn;
                }
                return pm;
            }

            /**
             * @brief 方法/构造所属的类模板 pattern；非类模板语境为空。
             *
             * @internal 特化成员先经 specialized 归一（pattern 成员对
             * specialized 回答 InvalidFile 垃圾，is_addressof_method
             * 同款导航）；语义父类再
             * 归一到主模板。非模板类返回空（单一判据的语料只覆盖类模板）。
             */
            [[nodiscard]]
            static auto pattern_class_of(const CXCursor& fn) -> CXCursor
            {
                CXCursor cls =
                    cg::clang::semantic_parent(pattern_function_of(fn));
                if (cg::clang::is_null(cls) || is_invalid_cursor(cls)
                    || !is_record_kind(cg::clang::kind(cls)))
                {
                    return CXCursor{};
                }
                const CXCursor primary = cg::clang::specialized_template(cls);
                if (!cg::clang::is_null(primary) && !is_invalid_cursor(primary)
                    && cg::clang::kind(primary) == CXCursor_ClassTemplate)
                {
                    cls = primary;
                }
                return cg::clang::kind(cls) == CXCursor_ClassTemplate
                           ? cls
                           : CXCursor{};
            }

            /**
             * @brief 指针式类判据（容器元素槽的单一判据 +
             * 存储面守卫，零名字参与）。
             *
             * 定义：类模板 pattern 直属
             * operator->（CXXMethod/FunctionTemplate） 的结果 canonical
             * 剥一层拼写含 "type-parameter-"（记指针式），且 存在直属字段
             * canonical 剥一层恰为 own 模板形参下标（T* 存储面）。 soundness
             * 边界：libc++ 三智能指针满足；容器无 operator-> 不满足；
             * 出参包装（T** 字段剥一层是 "type-parameter-0-0 *"，own 下标拒）
             * 不满足。fallback（漏收方向）：operator-> 在基类、存储经嵌套壳
             * （TwoSlot 形态）、非模板包装类。
             */
            [[nodiscard]]
            static auto is_pointer_shell_class(const CXCursor& cls) -> bool
            {
                if (cg::clang::kind(cls) != CXCursor_ClassTemplate)
                {
                    return false;
                }
                int own_count = 0;
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch)
                            == CXCursor_TemplateTypeParameter)
                        {
                            ++own_count;
                        }
                    });
                if (own_count == 0)
                {
                    return false;
                }
                bool arrow = false;
                bool stored = false;
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        const CXCursorKind k = cg::clang::kind(ch);
                        if (k == CXCursor_FieldDecl)
                        {
                            if (const auto core =
                                    cg::clang::canonical_pointee_spelling(
                                        ch,
                                        1))
                            {
                                stored = stored
                                         || own_tp_index(*core, own_count) >= 0;
                            }
                        }
                        else if (
                            (k == CXCursor_CXXMethod
                             || k == CXCursor_FunctionTemplate)
                            && cg::clang::spelling(ch) == "operator->")
                        {
                            if (const auto pointee = cg::clang::
                                    canonical_result_pointee_spelling(ch, 1);
                                pointee
                                && pointee->find("type-parameter-")
                                       != std::string::npos)
                            {
                                arrow = true;
                            }
                        }
                    });
                return arrow && stored;
            }

            /** @brief 指针式类判据的 usr 缓存包装。 */
            [[nodiscard]]
            auto is_pointer_shell_cached(const CXCursor& cls) -> bool
            {
                const std::string key = cg::clang::usr(cls);
                auto              it = shell_cache_.find(key);
                if (it == shell_cache_.end())
                {
                    it = shell_cache_.emplace(key, is_pointer_shell_class(cls))
                             .first;
                }
                return it->second;
            }

            /** @brief 记录声明游标的 pattern 归一（特化 -> 主模板）。 */
            [[nodiscard]]
            static auto normalize_record_decl(const CXCursor& decl) -> CXCursor
            {
                const CXCursor primary = cg::clang::specialized_template(decl);
                if (cg::clang::is_null(primary) || is_invalid_cursor(primary)
                    || !is_record_kind(cg::clang::kind(primary)))
                {
                    return decl;
                }
                return primary;
            }

            /**
             * @brief 指针式壳 pattern 的直属 T* 存储字段（恰一个 own 形参
             *        下标命中的字段；多候选 no-op）。
             *
             * @internal 与 is_pointer_shell_class 的存储面守卫同一判据源
             *           （canonical 剥一层 = own 模板形参下标）；多 T* 字段
             *           无法证明唯一存储位，整类不发（宁漏勿猜）。结果按
             *           pattern usr 缓存。tp_index 供壳工厂模板实参直取的
             *           pointee 位对齐。
             */
            [[nodiscard]]
            auto shell_tp_field_slot(const CXCursor& pattern)
                -> std::optional<ShellTpField>
            {
                if (cg::clang::is_null(pattern) || is_invalid_cursor(pattern))
                {
                    return std::nullopt;
                }
                const std::string key = cg::clang::usr(pattern);
                if (const auto it = shell_field_cache_.find(key);
                    it != shell_field_cache_.end())
                {
                    return it->second;
                }
                int own_count = 0;
                for_each_child(
                    pattern,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch)
                            == CXCursor_TemplateTypeParameter)
                        {
                            ++own_count;
                        }
                    });
                std::optional<ShellTpField> hit;
                bool                        ambiguous = false;
                if (own_count > 0)
                {
                    for_each_child(
                        pattern,
                        [&](const CXCursor& ch)
                        {
                            if (ambiguous
                                || cg::clang::kind(ch) != CXCursor_FieldDecl)
                            {
                                return;
                            }
                            const auto core =
                                cg::clang::canonical_pointee_spelling(ch, 1);
                            if (!core || own_tp_index(*core, own_count) < 0)
                            {
                                return;
                            }
                            if (hit.has_value())
                            {
                                ambiguous = true; // 多 T* 字段：不唯一
                                hit = std::nullopt;
                                return;
                            }
                            hit = ShellTpField{
                                own_tp_index(*core, own_count),
                                std::string{cg::clang::spelling(ch)}};
                        });
                }
                shell_field_cache_.emplace(key, hit);
                return hit;
            }

            /**
             * @brief 记录派生判定（derived == base 或沿基类链可达 base，
             *        有界 BFS）。
             *
             * @internal 两侧先经 normalize_record_decl 归一（模板特化基比
             *           对主模板）。深度/宽度封顶（缺候选方向的保守近似），
             *           不可解析基（依赖糖）跳过。出参写回值配对的证明闸
             *           专用——零名字参与。
             */
            [[nodiscard]]
            static auto record_derives_from(
                const CXCursor& derived,
                const CXCursor& base) -> bool
            {
                const CXCursor        want = normalize_record_decl(base);
                std::vector<CXCursor> frontier{derived};
                std::unordered_set<std::string> seen{cg::clang::usr(derived)};
                for (int depth = 0;
                     depth < 6 && !frontier.empty() && seen.size() <= 64;
                     ++depth)
                {
                    for (const CXCursor& cur : frontier)
                    {
                        if (cg::clang::equal_cursors(
                                normalize_record_decl(cur),
                                want))
                        {
                            return true;
                        }
                    }
                    std::vector<CXCursor> next;
                    for (const CXCursor& cur : frontier)
                    {
                        for_each_child(
                            cur,
                            [&](const CXCursor& ch)
                            {
                                if (cg::clang::kind(ch)
                                    != CXCursor_CXXBaseSpecifier)
                                {
                                    return;
                                }
                                const CXCursor base_decl =
                                    cg::clang::type_declaration(ch);
                                if (cg::clang::is_null(base_decl)
                                    || is_invalid_cursor(base_decl))
                                {
                                    return;
                                }
                                if (seen.insert(cg::clang::usr(base_decl))
                                        .second)
                                {
                                    next.push_back(base_decl);
                                }
                            });
                    }
                    frontier = std::move(next);
                }
                return false;
            }

            /**
             * @brief 壳拷贝/move 构造的 pointee 别名边（返回槽壳透传）。
             *
             * @internal `return 壳变量;` / `Box b = a;` 在 AST 是拷贝/move
             *           构造调用（无 T* 形参位，构造穿透臂整档 no-op）——
             *           此前 #ret 槽零行，返回链断在中继第一跳（消费方
             *           return_flow 边建而值空）。源操作数解到槽引用（局部/
             *           形参记函数域裸名、字段记 @fld 描述符）且自身静态类型
             *           命中指针式壳判据时，槽发 LocalCopy 别名行到源壳变量
             *           槽（pointee 语义，与构造穿透及工厂赋值臂同惯例）——
             *           源槽自身的证据链（return_flow 中继 / 出参写回终态）
             *           经函数内传播并入。绝不发壳自身静态类型；源不可寻址（跨函
             *           数体声明等）不发（宁漏勿猜）。
             */
            void shell_copy_source_alias(
                const CXCursor&    expr,
                const CXCursor&    ctor,
                const std::string& owner,
                const std::string& var)
            {
                const std::vector<CXCursor> args = ctor_arguments(expr, ctor);
                if (args.size() != 1)
                {
                    return; // 多源构造无唯一别名面
                }
                const CXCursor     src = unwrap_expr(strip_casts(args.front()));
                const CXCursorKind sk = cg::clang::kind(src);
                if ((sk != CXCursor_DeclRefExpr && sk != CXCursor_MemberRefExpr)
                    || !typed_pointer_shell(src))
                {
                    return;
                }
                const auto slot = slot_ref_of(src);
                if (!slot.has_value() || slot->var_name.empty())
                {
                    return;
                }
                const CXCursor src_decl = cg::clang::referenced(src);
                if (cg::clang::is_null(src_decl)
                    || cross_fn_body_decl(src_decl))
                {
                    return;
                }
                TypeHintRec hint =
                    make_hint(owner, var, cg::clang::location(args.front()));
                hint.source = HintSource::LocalCopy;
                hint.confidence = Confidence::Medium;
                hint.related_var =
                    slot->owner_key.empty()
                        ? slot->var_name
                        : encode_field_slot(slot->owner_key, slot->var_name);
                hint.fallback_reason = "shell_pointee_flow";
                emit_hint(std::move(hint));
            }

            /**
             * @brief 透传壳的 pointee 实参位（按声明在该实例上的游标判定）。
             *
             * @internal 接收者静态类型是透传壳类的站点（壳类模板实例作接收者
             *           声明类型）：壳的 pointee 家族才是虚派发的真实动态面。
             *           判据与透传 getter 同族——主模板上存在零参方法且返回
             *           类型 canonical 引用 own 模板形参（值/一层指针收紧：T**
             *           出参地址形态不剥，与消费侧透传剥壳同一口径）；全部命中
             *           方法必须收敛到同一 own 形参下标（多 getter 各指不同形参
             *           时无法证明唯一 pointee，整类放弃——宁漏勿猜）。own 下标
             *           域（直接 TemplateTypeParameter 子序）保证该位是类型实参
             *           位：非类型形参前置的壳（下标域拒绝）与值实参壳都定位不
             *           到可剥的实参位。主模板不要求界内（结构判据与代码位置无
             *           关，标准库壳同享）。结果按主模板 usr 缓存。
             * @return 实参位下标；非透传壳/非类模板/多下标不收敛为空。
             */
            [[nodiscard]]
            auto passthrough_pointee_index(const CXCursor& typed)
                -> std::optional<int>
            {
                const CXCursor decl = cg::clang::type_declaration(typed);
                if (cg::clang::is_null(decl) || is_invalid_cursor(decl))
                {
                    return std::nullopt;
                }
                const CXCursor primary = cg::clang::specialized_template(decl);
                if (cg::clang::is_null(primary) || is_invalid_cursor(primary)
                    || cg::clang::kind(primary) != CXCursor_ClassTemplate)
                {
                    return std::nullopt;
                }
                const std::string key = cg::clang::usr(primary);
                if (const auto it = passthrough_pointee_cache_.find(key);
                    it != passthrough_pointee_cache_.end())
                {
                    return it->second;
                }
                int own_count = 0;
                for_each_child(
                    primary,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch)
                            == CXCursor_TemplateTypeParameter)
                        {
                            ++own_count;
                        }
                    });
                std::optional<int> arg;
                bool               diverged = false;
                if (own_count > 0)
                {
                    for_each_child(
                        primary,
                        [&](const CXCursor& ch)
                        {
                            if (diverged
                                || cg::clang::kind(ch) != CXCursor_CXXMethod)
                            {
                                return;
                            }
                            const int hit =
                                is_passthrough_getter(ch, own_count, 1);
                            if (hit < 0)
                            {
                                return;
                            }
                            if (arg.has_value() && *arg != hit)
                            {
                                arg = std::nullopt; // 多下标不收敛：整类放弃
                                diverged = true;
                                return;
                            }
                            arg = hit;
                        });
                }
                passthrough_pointee_cache_.emplace(key, arg);
                return arg;
            }

            /**
             * @brief 透传壳声明类型的 pointee 记录
             * key（供接收者静态类型追加）。
             *
             * @internal 主模板命中透传判据且实参位是界内记录时返回实参记录
             *           key；实参非记录（标量/指针/界外函数类型）或判据不收敛
             *           时返回空——调用方不追加，站点行为与剥壳前一致（宁漏
             *           勿猜）。
             */
            [[nodiscard]]
            auto shell_pointee_key(const CXCursor& typed) -> std::string
            {
                const std::optional<int> arg = passthrough_pointee_index(typed);
                if (!arg.has_value())
                {
                    return {};
                }
                const CXCursor elem =
                    cg::clang::type_template_arg_declaration(typed, *arg);
                if (cg::clang::is_null(elem) || is_invalid_cursor(elem))
                {
                    return {};
                }
                return record_key_of_decl(elem);
            }

            /**
             * @brief 恰一个"T* 形参"（canonical 剥一层恰为 own 模板形参下标）的
             *        形参序号；无或多个为 -1。
             *
             * @internal 在 pattern 函数上判定（特化成员的形参 canonical 已是具
             * 体类型，type-parameter- 标记只在 pattern 语境可见——与取址判据的
             * 方法非模板条件同源）；
             * 序号与调用点实参按位对齐（构造与成员调用的 ParmDecl
             * 序即实参序）。
             */
            [[nodiscard]]
            static auto single_tp_pointer_param(
                const CXCursor& fn,
                const CXCursor& cls) -> int
            {
                int own_count = 0;
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch)
                            == CXCursor_TemplateTypeParameter)
                        {
                            ++own_count;
                        }
                    });
                int  found = -1;
                int  pos = -1;
                bool ambiguous = false;
                for_each_child(
                    fn,
                    [&](const CXCursor& p)
                    {
                        if (cg::clang::kind(p) != CXCursor_ParmDecl)
                        {
                            return;
                        }
                        ++pos;
                        if (const auto core =
                                cg::clang::canonical_pointee_spelling(p, 1);
                            core && own_tp_index(*core, own_count) >= 0)
                        {
                            if (found >= 0)
                            {
                                ambiguous =
                                    true; // 多个 T* 形参：无法证明唯一槽
                                return;
                            }
                            found = pos;
                        }
                    });
                return ambiguous ? -1 : found;
            }

            /**
             * @brief 变量的声明类型是否 std 容器表内类模板特化（名单法）。
             *
             * @internal 特化声明拼写可能带模板实参（剥 '<' 尾巴）；先归一到
             * 主模板名再查表。项目内同名自定义类会被误判——名单法的既定
             * 已批边界（与 dlopen 系统名单同类），最坏多一个空元素槽行。
             */
            [[nodiscard]]
            auto is_std_container(const CXCursor& var) -> bool
            {
                const CXCursor decl = cg::clang::type_declaration(var);
                if (cg::clang::is_null(decl)
                    || !is_record_kind(cg::clang::kind(decl)))
                {
                    return false;
                }
                std::string    name = cg::clang::spelling(decl);
                const CXCursor primary = cg::clang::specialized_template(decl);
                if (!cg::clang::is_null(primary) && !is_invalid_cursor(primary)
                    && is_record_kind(cg::clang::kind(primary)))
                {
                    name = cg::clang::spelling(primary);
                }
                const std::size_t lt = name.find('<');
                if (lt != std::string::npos)
                {
                    name.resize(lt);
                }
                return std::ranges::find(STD_CONTAINERS, name)
                       != std::end(STD_CONTAINERS);
            }

            /**
             * @brief 容器变量的元素记录 key（声明期，只进 static_types）。
             *
             * @internal 元素类型 = 第 0 模板实参剥糖（declaration_of_type 剥
             * 指针/引用/typedef：vector<I*> 直接给 I 的声明）。智能指针元素
             * 剥壳用单一判据（零名字红线）：元素类模板 pattern 直属
             * operator-> 的结果 canonical 剥一层含 "type-parameter-" 标记
             * （canonical_result_pointee_spelling；libc++ shared_ptr 的
             * '__remove_extent(type-parameter-0-0) *' 与自研壳的
             * 'type-parameter-0-0 *' 同判命中，容器无 operator-> 天然排除），
             * 元素记录 = 壳特化的第 0 模板实参。fallback（漏收方向）：元素是
             * 嵌套容器（std 外记录，key 空）、int 等非记录、壳 pattern 不可见
             * （系统头过滤）。
             */
            [[nodiscard]]
            auto container_elem_key(const CXCursor& var) -> std::string
            {
                const CXCursor elem =
                    cg::clang::type_template_arg_declaration(var, 0);
                if (cg::clang::is_null(elem)
                    || !is_record_kind(cg::clang::kind(elem)))
                {
                    return {};
                }
                const CXCursor pattern = cg::clang::specialized_template(elem);
                if (!cg::clang::is_null(pattern) && !is_invalid_cursor(pattern)
                    && cg::clang::kind(pattern) == CXCursor_ClassTemplate)
                {
                    bool        shell_hit = false;
                    std::string shell_key;
                    for_each_child(
                        pattern,
                        [&](const CXCursor& ch)
                        {
                            if (shell_hit)
                            {
                                return; // 已命中：找到即停
                            }
                            const CXCursorKind k = cg::clang::kind(ch);
                            if ((k != CXCursor_CXXMethod
                                 && k != CXCursor_FunctionTemplate)
                                || cg::clang::spelling(ch) != "operator->")
                            {
                                return;
                            }
                            if (const auto pointee = cg::clang::
                                    canonical_result_pointee_spelling(ch, 1);
                                pointee
                                && pointee->find("type-parameter-")
                                       != std::string::npos)
                            {
                                const CXCursor inner =
                                    cg::clang::type_template_arg_declaration(
                                        elem,
                                        0);
                                shell_key = cg::clang::is_null(inner)
                                                ? std::string{}
                                                : record_key_of_decl(inner);
                                shell_hit = true;
                            }
                        });
                    if (shell_hit)
                    {
                        return shell_key;
                    }
                }
                return record_key_of_decl(elem);
            }

            /** @brief 元素槽登记
             * key（声明期闸门：写集/拷贝流/链接行据此放行）。 */
            [[nodiscard]]
            static auto elem_slot_key(
                std::string_view owner,
                std::string_view var) -> std::string
            {
                return cg::util::compose_key(owner, var);
            }

            /**
             * @brief 声明期容器元素槽：发 Declared 元素行（static_types = 元素
             *        记录 key，type_keys 恒空——声明期元素类型只进静态面）并
             *        登记闸门。
             *
             * @internal gate_only：元素是函数指针/std::function（无记录 key、
             * 静态面无内容可发）时只登记闸门——目标集完全来自动态写入
             * （下标存函数 / push_back 函数），与"元素是记录"的静态声明互补。
             */
            void register_container_elem(
                const std::string&               owner,
                const std::string&               var,
                const std::string&               elem_key,
                const cg::clang::SourceLocation& loc,
                bool                             gate_only = false)
            {
                if (owner.empty() || var.empty()
                    || (elem_key.empty() && !gate_only))
                {
                    return;
                }
                if (!elem_key.empty())
                {
                    TypeHintRec hint = make_hint(
                        owner,
                        std::string{container_elem_slot_prefix} + var,
                        loc);
                    hint.static_types = {elem_key};
                    emit_hint(std::move(hint));
                }
                elem_slots_.insert(elem_slot_key(owner, var));
            }

            /**
             * @brief 容器元素是否可调用形态（函数指针 / std::function）。
             *
             * @internal 判据走声明类型拼写（contains）："(*)" 覆盖裸函数指针
             * 元素（vector<void(*)()>），"function<" 覆盖 std::function 元素
             * （拼写含模板形态）。拼写判据在此是闸门放行（只多放行证据流、
             * 不产类型），宁可稍宽。
             */
            [[nodiscard]]
            static auto container_elem_is_callable(const CXCursor& var) -> bool
            {
                const std::string sp = cg::clang::type_spelling(var);
                return sp.find("(*)") != std::string::npos
                       || sp.find("function<") != std::string::npos;
            }

            [[nodiscard]]
            auto elem_slot_registered(
                std::string_view owner,
                std::string_view var) const -> bool
            {
                return elem_slots_.contains(elem_slot_key(owner, var));
            }

            /**
             * @brief 构造调用的实参序列（跳过类型/模板引用与自引用伪影）。
             *
             * @internal 构造折叠 CallExpr 的直接子恰是实参（构造折叠探针
             * 实证：Holder h(&d, Wrap(d), d2) 的直接子恰好三个实参）；普通
             * 函数调用"首子=被调用表达式"的约定在此不适用。防御性跳过引用
             * 本构造的子游标。
             */
            [[nodiscard]]
            static auto ctor_arguments(
                const CXCursor& expr,
                const CXCursor& ctor) -> std::vector<CXCursor>
            {
                std::vector<CXCursor> args;
                const std::string     ctor_usr = cg::clang::usr(ctor);
                for_each_child(
                    expr,
                    [&](const CXCursor& kid)
                    {
                        if (is_type_ref(kid))
                        {
                            return;
                        }
                        const CXCursor kid_ref =
                            cg::clang::referenced(unwrap_expr(kid));
                        if (!cg::clang::is_null(kid_ref)
                            && cg::clang::usr(kid_ref) == ctor_usr)
                        {
                            return;
                        }
                        args.push_back(kid);
                    });
                return args;
            }

            /** @brief 表达式值可能携带的动态记录类型集（采集层）。 */
            [[nodiscard]]
            auto expr_type_keys(const CXCursor& e) -> std::vector<std::string>
            {
                const CXCursor unwrapped = unwrap_expr(e);
                if (cg::clang::kind(unwrapped) != cg::clang::kind(e))
                {
                    return expr_type_keys(unwrapped);
                }
                const CXCursorKind       k = cg::clang::kind(e);
                std::vector<std::string> keys;
                const auto               add = [&](const std::string& key)
                {
                    if (!key.empty())
                    {
                        cg::util::push_unique(keys, key);
                    }
                };
                switch (k)
                {
                case CXCursor_CXXNewExpr:
                    add(record_key_of_decl(cg::clang::pointee_declaration(e)));
                    return keys;
                case CXCursor_InitListExpr:
                    add(record_key_of_decl(cg::clang::type_declaration(e)));
                    return keys;
                case CXCursor_CXXStaticCastExpr:
                case CXCursor_CXXDynamicCastExpr:
                case CXCursor_CXXFunctionalCastExpr:
                case CXCursor_CXXConstCastExpr:
                case CXCursor_CXXReinterpretCastExpr:
                case CXCursor_CStyleCastExpr:
                {
                    // 转型：先保留操作数自身的动态证据（上转型不得把 Derived
                    // 缩窄成 Base）。操作数是值类型时目标类型就是新值类型；
                    // 指针/引用转型绝不拿目标类型当动态证据——只有可证明的
                    // 向下转型（目标记录派生自操作数静态记录）才保留 Low 目标。
                    const CXCursor operand = cast_operand(e);
                    if (!cg::clang::is_null(operand))
                    {
                        for (const std::string& key : expr_type_keys(operand))
                        {
                            add(key);
                        }
                    }
                    if (k == CXCursor_CStyleCastExpr)
                    {
                        // C 风格转换此前不在 case 列表，整类表达式无证据（SWIG
                        // 形态诊断：SWIG 生成代码统一写
                        // `(Impl*)x`，站点评述全断）。 定义：与具名 cast
                        // 同享"先保留操作数证据"前半段； soundness 边界：C
                        // 风格语义是 static/reinterpret/const
                        // 的并集，目标类型语义不可证明，绝不能当动态证据补入
                        // ——连具名 cast 的"可证明向下转型 Low 目标"采纳也不给，
                        // 只增不减，误报方向安全。fallback 清单（漏收方向）：C
                        // 风格可证明 downcast 不产 Low 目标候选，需要目标证据
                        // 的站点应改用具名 cast。
                        return keys;
                    }
                    if (!keys.empty())
                    {
                        return keys;
                    }
                    const bool pointer_cast =
                        !cg::clang::is_null(operand)
                        && (cg::clang::is_pointer_or_reference(operand)
                            || cg::clang::is_pointer_or_reference(e));
                    if (pointer_cast)
                    {
                        if (is_provable_downcast(e))
                        {
                            add(record_key_of_decl(
                                cg::clang::type_declaration(e)));
                        }
                        return keys;
                    }
                    add(record_key_of_decl(cg::clang::type_declaration(e)));
                    return keys;
                }
                case CXCursor_UnaryOperator:
                {
                    const std::vector<std::string> toks =
                        cg::clang::token_spellings(e);
                    if (toks.empty() || toks.front() != "&")
                    {
                        return keys;
                    }
                    const std::vector<CXCursor> kids = direct_children(e);
                    if (kids.empty())
                    {
                        return keys;
                    }
                    const CXCursor operand = kids.front();
                    if (cg::clang::kind(operand) != CXCursor_DeclRefExpr)
                    {
                        return keys; // &*p、&ref、&arr[i] 只给静态类型
                    }
                    const CXCursor var = cg::clang::referenced(operand);
                    if (cg::clang::is_null(var))
                    {
                        return keys;
                    }
                    if (cg::clang::canonical_type_class(var)
                        == cg::clang::TypeClass::Record)
                    {
                        add(record_key_of_decl(
                            cg::clang::type_declaration(var)));
                    }
                    return keys;
                }
                case CXCursor_DeclRefExpr:
                {
                    const CXCursor var = cg::clang::referenced(e);
                    if (cg::clang::is_null(var))
                    {
                        return keys;
                    }
                    if (cg::clang::canonical_type_class(var)
                        == cg::clang::TypeClass::Record)
                    {
                        add(record_key_of_decl(
                            cg::clang::type_declaration(var)));
                    }
                    return keys;
                }
                case CXCursor_ConditionalOperator:
                {
                    const std::vector<CXCursor> kids = direct_children(e);
                    for (std::size_t i = 1; i < kids.size(); ++i)
                    {
                        for (const std::string& key : expr_type_keys(kids[i]))
                        {
                            add(key);
                        }
                    }
                    return keys;
                }
                case CXCursor_CallExpr:
                {
                    // 调用结果：只有按值返回记录时动态类型才是返回记录。指针/
                    // 引用返回（含容器解引用 `operator*`/`operator[]`）只给静态
                    // 类型——type_declaration 会剥指针，绝不能拿它当动态证据。
                    const CXCursor ref = cg::clang::referenced(e);
                    if (!cg::clang::is_null(ref))
                    {
                        if (cg::clang::spelling(ref).starts_with("operator"))
                        {
                            return keys; // 运算符调用（容器遍历/智能指针包装）
                        }
                        if (cg::clang::result_is_pointer_or_reference(ref))
                        {
                            return keys;
                        }
                    }
                    else if (cg::clang::is_pointer_or_reference(e))
                    {
                        return keys;
                    }
                    add(record_key_of_decl(cg::clang::type_declaration(e)));
                    return keys;
                }
                default:
                    return keys;
                }
            }

            /** @brief 赋值 LHS 指向的字段：字段名 + 声明该字段的记录 key。 */
            struct FieldTarget
            {
                std::string name;
                std::string owner_key;
            };

            /**
             * @brief LHS 是否为字段写入（`this->field` / 隐式 this /
             * `obj.field`）。
             *
             * @internal 字段槽的所有者是声明该字段的记录，与访问方式无关：
             * 显式 this、隐式 this、显式对象一律按 FieldDecl 的语义父记录
             * 归槽，`obj.field` 因此能与 `obj.field->f()` 的接收者对同一槽。
             */
            [[nodiscard]]
            auto field_target_of(const CXCursor& e) -> FieldTarget
            {
                const CXCursor cur = unwrap_expr(e);
                if (cg::clang::kind(cur) != CXCursor_MemberRefExpr)
                {
                    return {};
                }
                const CXCursor field = cg::clang::referenced(cur);
                if (cg::clang::is_null(field))
                {
                    return {};
                }
                const std::string owner =
                    record_key_of_decl(cg::clang::semantic_parent(field));
                return {.name = cg::clang::spelling(cur), .owner_key = owner};
            }

            /**
             * @brief 容器下标写（形态 3 注册表）：lhs[k] = &fn / lhs[k] =
             * fn。
             *
             * @internal LHS 是界外 operator[] 调用，容器元素槽（记录元素或
             * gate_only 可调用元素）的动态写入证据：函数 key 进
             * "#elem:`<容器>`" 槽。经变量中转的 RHS 不透（宁漏勿猜）。
             * @return LHS 是否为下标写形态——形态命中即终结本语句处理（无论
             *         是否发射：下标写不是指针/字段线索，不落下方任何路径）。
             */
            bool consume_container_index_write(
                const CXCursor&              c,
                const std::vector<CXCursor>& kids,
                const CXCursor&              rhs,
                const Ctx&                   ctx)
            {
                const CXCursor sub_lhs = unwrap_expr(kids[0]);
                if (cg::clang::kind(sub_lhs) != CXCursor_CallExpr)
                {
                    return false;
                }
                const CXCursor op = cg::clang::referenced(sub_lhs);
                if (cg::clang::is_null(op)
                    || cg::clang::kind(op) != CXCursor_CXXMethod
                    || cg::clang::spelling(op) != "operator[]")
                {
                    return false;
                }
                const std::vector<CXCursor> sk = direct_children(sub_lhs);
                if (sk.empty() || ctx.fn_key.empty())
                {
                    return true;
                }
                const auto        slot = slot_ref_of(unwrap_expr(sk.front()));
                const std::string fn_key_of_rhs = function_target_key(rhs);
                if (!slot.has_value() || slot->var_name.empty()
                    || fn_key_of_rhs.empty())
                {
                    return true;
                }
                // 域解析——字段域 → DeclRef 全局变量域（跨函数注册表）
                // → 函数域兜底。
                const std::string owner =
                    !slot->owner_key.empty() ? slot->owner_key : [&]
                {
                    const std::string g = resolve_slot_owner(sk.front());
                    return g.empty() ? ctx.fn_key : g;
                }();
                if (!elem_slot_registered(owner, slot->var_name))
                {
                    return true;
                }
                const cg::clang::SourceLocation sloc = cg::clang::location(c);
                const std::string               sfile = norm(sloc);
                if (!under_roots(sfile, opt))
                {
                    return true;
                }
                TypeHintRec hint = make_hint(
                    owner,
                    std::string{container_elem_slot_prefix} + slot->var_name,
                    sloc);
                hint.type_keys = {fn_key_of_rhs};
                hint.source = HintSource::ContainerElemFlow;
                hint.confidence = Confidence::High;
                hint.fallback_reason = "elem_index_write";
                emit_hint(std::move(hint));
                return true;
            }

            /**
             * @brief 件三（逃逸集·写侧）：裸数组/指针下标写。
             *
             * @internal slot[i] = <函数目标>（含 void* 转型形态）——目标离开
             * 具名槽体系进入逃逸面。锚点 = 下标基名（域：全局优先、函数域
             * 兜底），Low 置信 escaped_store；读侧（indirect_call 的下标锚
             * 点）按同锚点 join 出逃逸候选边。宁漏勿猜：经变量中转的 RHS
             * 不透。
             * @return LHS 是否为下标写形态——形态命中即终结本语句处理（下标
             *         写不走字段/局部槽路径）。
             */
            bool consume_escaped_store(
                const CXCursor&              c,
                const std::vector<CXCursor>& kids,
                const CXCursor&              rhs,
                const Ctx&                   ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return false;
                }
                const CXCursor lhs_arr = unwrap_expr(strip_casts(kids[0]));
                if (cg::clang::kind(lhs_arr) != CXCursor_ArraySubscriptExpr)
                {
                    return false;
                }
                const std::string fn_key_esc =
                    function_target_key(strip_casts(rhs));
                const std::vector<CXCursor> bk = direct_children(lhs_arr);
                if (!bk.empty() && !fn_key_esc.empty())
                {
                    const CXCursor    base = unwrap_expr(bk.front());
                    const std::string nm = ref_var_name(base);
                    if (!nm.empty())
                    {
                        const std::string gk_esc = resolve_slot_owner(base);
                        const cg::clang::SourceLocation eloc =
                            cg::clang::location(c);
                        TypeHintRec esc_hint = make_hint(
                            gk_esc.empty() ? ctx.fn_key : gk_esc,
                            nm,
                            eloc);
                        esc_hint.type_keys = {fn_key_esc};
                        esc_hint.source = HintSource::LocalCopy;
                        esc_hint.fallback_reason = "escaped_store";
                        emit_hint(std::move(esc_hint));
                    }
                }
                return true;
            }

            /**
             * @brief LHS 解引用 = 出参写回（*pp = e / *obj.pp_field = e）。
             *
             * @internal 目标槽取解引用下的变量/字段（数组下标/再解引用不是槽，
             * 宁漏勿误）；RHS 是 static_cast<I*>(this) 的 QI 形态发 [所在类]
             * 单键 Low 证据（动态类型 ∈ {所在类及其派生} 的 under-近似；派生
             * 闭包由 propagate 按 extends 边展开成 over-近似）。
             * @return LHS 是否为赋值语义的 "*" 解引用形态——形态命中即终结本
             *         语句处理；非赋值语义的解引用 LHS（*p += … 之类）返回
             *         false，落回下文并因 LHS 非 DeclRefExpr/MemberRefExpr 自
             *         然不产线索。
             */
            bool consume_outparam_writeback(
                const CXCursor&              c,
                const std::vector<CXCursor>& kids,
                const CXCursor&              rhs,
                std::vector<std::string>     keys,
                const std::string&           related_var,
                const std::string&           related_key,
                const Ctx&                   ctx)
            {
                const CXCursor deref_lhs = unwrap_expr(kids[0]);
                if (cg::clang::kind(deref_lhs) != CXCursor_UnaryOperator)
                {
                    return false;
                }
                const std::vector<std::string> lhs_tokens =
                    cg::clang::token_spellings(deref_lhs);
                const std::vector<CXCursor> lhs_kids =
                    direct_children(deref_lhs);
                if (lhs_tokens.empty() || lhs_tokens.front() != "*"
                    || lhs_kids.empty())
                {
                    return false;
                }
                const auto slot = slot_ref_of(unwrap_expr(lhs_kids.front()));
                if (!slot.has_value() || slot->var_name.empty())
                {
                    // `*v.Put() = e`：解引用目标是取址方法判据返回的
                    // T**，接收者 v 命中本函数构造期登记的出参壳绑定表时，
                    // 写回行补采到绑定的出参槽（与 v.Set(e) 同语义）。判据与
                    // 发射见 outparam_addressof_writeback；形态命中与否都终
                    // 结本语句（调用结果不是槽）。
                    outparam_addressof_writeback(
                        unwrap_expr(lhs_kids.front()),
                        keys,
                        related_var,
                        related_key,
                        ctx);
                    return true;
                }
                Confidence       conf = Confidence::Medium;
                std::string_view reason = "out_param_flow";
                if (cg::clang::kind(strip_casts(rhs)) == CXCursor_CXXThisExpr
                    && !ctx.record_key.empty())
                {
                    keys.assign(1, ctx.record_key);
                    conf = Confidence::Low;
                    reason = "out_param_this";
                }
                const cg::clang::SourceLocation dloc = cg::clang::location(c);
                const std::string               dfile = norm(dloc);
                if (!ctx.fn_key.empty() && under_roots(dfile, opt))
                {
                    TypeHintRec hint = make_hint(
                        slot->owner_key.empty() ? ctx.fn_key : slot->owner_key,
                        slot->var_name,
                        dloc);
                    hint.type_keys = keys;
                    hint.source = HintSource::OutParamFlow;
                    hint.confidence = conf;
                    hint.fallback_reason = std::string{reason};
                    if (keys.empty())
                    {
                        hint.related_var = related_var;
                        hint.related_key = related_key;
                    }
                    emit_hint(std::move(hint));
                }
                return true;
            }

            /**
             * @brief `*v.M() = e` 形态的出参壳写回补采（M 命中取址方法判
             *        据，v 命中本函数构造期绑定表）。
             *
             * @internal 工厂体经出参包装壳中转写回（v 以 T** 出参构造、
             *           写侧经 *v.Put() = e 解引用取址方法）此前整层无采
             *           集——写回行落在壳自己的局部上，出参槽（工厂形参）
             *           零行、调用方写回触发永不点火。判据纯结构三闸：
             *           解引用操作数是被调为 CXXMethod 的调用且该方法命中
             *           取址方法判据（所属 pattern 有 operator->、返回
             *           canonical 双剥与 operator-> 单剥同 own 形参拼写）；
             *           接收者是函数域局部（绑定表键域）；接收者名命中
             *           bind_outparam_shell 登记的绑定。命中发 OutParamFlow
             *           行到绑定槽（值证据三通道由 assignment 预计算），
             *           与 v.Set(e) 写回臂同一落点、并集语义。
             */
            void outparam_addressof_writeback(
                const CXCursor&                 operand,
                const std::vector<std::string>& keys,
                const std::string&              related_var,
                const std::string&              related_key,
                const Ctx&                      ctx)
            {
                if (outparam_bindings_.empty() || ctx.fn_key.empty())
                {
                    return;
                }
                if (cg::clang::kind(operand) != CXCursor_CallExpr)
                {
                    return;
                }
                const CXCursor m = cg::clang::referenced(operand);
                if (cg::clang::is_null(m)
                    || cg::clang::kind(m) != CXCursor_CXXMethod)
                {
                    return;
                }
                const auto hit = addressof_method(m);
                if (!hit.has_value() || hit->chain == AddressofChain::Reject)
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(operand);
                if (kids.empty())
                {
                    return;
                }
                const CXCursor callee_expr = unwrap_expr(kids.front());
                if (cg::clang::kind(callee_expr) != CXCursor_MemberRefExpr)
                {
                    return;
                }
                const std::vector<CXCursor> member_kids =
                    direct_children(callee_expr);
                if (member_kids.empty())
                {
                    return;
                }
                const ReceiverInfo ri = describe_receiver(member_kids.front());
                if (ri.name.empty() || ri.name == "this"
                    || !ri.owner_key.empty())
                {
                    return; // 绑定表是函数域局部形态；字段/隐式 this 不采
                }
                const auto it = outparam_bindings_.find(
                    cg::util::compose_key(ctx.fn_key, ri.name));
                if (it == outparam_bindings_.end())
                {
                    return;
                }
                const cg::clang::SourceLocation loc =
                    cg::clang::location(operand);
                const std::string file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                TypeHintRec hint =
                    make_hint(it->second.owner_key, it->second.var_name, loc);
                hint.type_keys = keys;
                hint.source = HintSource::OutParamFlow;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = "out_param_flow";
                if (keys.empty())
                {
                    hint.related_var = related_var;
                    hint.related_key = related_key;
                }
                emit_hint(std::move(hint));
            }

            /**
             * @brief 赋值语句：字段写入记为 FieldFlow，局部重赋值记为
             * LocalCopy。
             *
             * @internal RHS 没有可直接推出的动态类型时，仍记录 related_var /
             * related_key（指针变量别名、返回指针的调用），由传播阶段（函数内/
             * 全局）解析；绝
             * 不把静态类型当动态证据。字段线索 owner 是记录，由接收者在
             * propagate 中按记录槽查找。多写入在槽级合并取并集。
             */
            void assignment(const CXCursor& c, const Ctx& ctx)
            {
                // AST 判定赋值：LHS/RHS 是 BinaryOperator 的前两个表达式子，
                // 不能用 token 下标（`this->p = ...` 的 "=" 不在第 1 位）。
                // 运算符按 opcode 判（'=='/'<=' 是别的 opcode，圆括号内嵌
                // 的赋值属于子表达式自己的 BinaryOperator）——opcode 是
                // Sema 建树的权威判定，不经源码词法化，宏展开的 extent
                // 错位碰不到它。
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.size() < 2)
                {
                    return;
                }
                if (cg::clang::binary_opcode(c) != CX_BO_Assign)
                {
                    return;
                }
                const CXCursor rhs = kids[1];
                // 转型声称目标：RHS 三通道（类型键/
                // 别名/返回槽）全空时仍可能是转型声称形态（如
                // `arg1 = reinterpret_cast<I*>(argp1)` 的 SWIG 解包、
                // `p = (I*)opaque();`），声称行独立于三通道发射——槽终态为空
                // 才由传播侧填入。入口闸前移：非转型/非包装 RHS 一次 kind
                // 读取即早退，不进判定函数。
                std::string cast_claim;
                if (cast_claim_shape(cg::clang::kind(rhs)))
                {
                    cast_claim = cast_claim_target(rhs);
                }
                std::string    related_var = ref_var_name(rhs);
                // 断链 1（赋值侧）：RHS 是透传 getter 调用（*pp = p.Get() /
                // x.f = q.Get()）时槽引用取 getter 接收者——值 = 壳实例槽的
                // pointee（getter 的 pattern #ret 恒空，@fn 返回边是死路）。
                if (related_var.empty())
                {
                    const CXCursor obj = passthrough_getter_object(
                        unwrap_expr(strip_casts(rhs)));
                    if (!cg::clang::is_null(obj))
                    {
                        const ReceiverInfo ri = describe_receiver(obj);
                        if (!ri.name.empty() && ri.name != "this")
                        {
                            related_var =
                                ri.owner_key.empty()
                                    ? ri.name
                                    : encode_field_slot(ri.owner_key, ri.name);
                        }
                    }
                }
                // RHS 是裸全局变量引用时拷贝流编码携带全局域（裸名在
                // propagate 的同域解析找不到 variable||name|file 全局行）；
                // 已带域的编码（@fld / #elem）与取址形态不重编码。
                {
                    const CXCursor rhs_ref = unwrap_expr(strip_casts(rhs));
                    if (cg::clang::kind(rhs_ref) == CXCursor_DeclRefExpr
                        && !related_var.empty()
                        && related_var.find('|') == std::string::npos
                        && related_var.find(':') == std::string::npos)
                    {
                        const std::string gk_raw = resolve_slot_owner(rhs_ref);
                        if (!gk_raw.empty())
                        {
                            related_var =
                                encode_global_slot(gk_raw, related_var);
                        }
                    }
                }
                const std::string        related_key = call_result_key(rhs);
                std::vector<std::string> keys = expr_type_keys(rhs);
                // 取址 seed（形态 1/4 存储）：q = &f / this->cb = &f
                // 时把函数 目标 key 作为槽的类型集元素（泛化允许函数 key
                // 流动），下游 indirect 消费按槽取目标。RHS
                // 取值表达式不透（经变量中转是槽 传播的活，宁漏勿猜）。
                if (keys.empty() && !ctx.fn_key.empty())
                {
                    const std::string fn_target = function_target_key(rhs);
                    if (!fn_target.empty())
                    {
                        keys.assign(1, fn_target);
                    }
                }
                // 容器下标写（形态 3 注册表）：判据与发射见
                // consume_container_index_write；形态命中即终结本语句（下标写
                // 不是指针/字段线索，不落下方任何路径）。
                if (consume_container_index_write(c, kids, rhs, ctx))
                {
                    return;
                }
                // 件三（逃逸集·写侧）：判据与发射见 consume_escaped_store；
                // 形态命中即终结本语句（下标写不走字段/局部槽路径）。
                if (consume_escaped_store(c, kids, rhs, ctx))
                {
                    return;
                }
                // LHS 解引用 = 出参写回（*pp = e / *obj.pp_field =
                // e）。 判据与发射见 consume_outparam_writeback；"*"
                // 赋值形态命中即 终结本语句，非赋值语义（*p += …
                // 之类）落回下文并因 LHS 非 DeclRefExpr/MemberRefExpr
                // 自然不产线索。
                if (consume_outparam_writeback(
                        c,
                        kids,
                        rhs,
                        keys,
                        related_var,
                        related_key,
                        ctx))
                {
                    return;
                }
                if (cast_claim.empty() && keys.empty() && related_var.empty()
                    && related_key.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                const auto emit_cast_claim = [&](const std::string& owner,
                                                 const std::string& var)
                {
                    if (cast_claim.empty() || owner.empty() || var.empty())
                    {
                        return;
                    }
                    TypeHintRec hint = make_hint(owner, var, loc);
                    hint.type_keys.assign(1, cast_claim);
                    hint.source = HintSource::CastTarget;
                    hint.confidence = Confidence::Low;
                    hint.fallback_reason =
                        std::string{source_name(HintSource::CastTarget)};
                    emit_hint(std::move(hint));
                };
                const FieldTarget field = field_target_of(kids[0]);
                if (!field.name.empty() && !field.owner_key.empty())
                {
                    TypeHintRec hint =
                        make_hint(field.owner_key, field.name, loc);
                    hint.type_keys = keys;
                    hint.source = HintSource::FieldFlow;
                    hint.confidence = Confidence::Medium;
                    hint.fallback_reason = "field_flow";
                    if (keys.empty())
                    {
                        hint.related_var = related_var;
                        hint.related_key = related_key;
                    }
                    emit_hint(std::move(hint));
                    emit_cast_claim(field.owner_key, field.name);
                    return;
                }
                const CXCursor lhs = unwrap_expr(kids[0]);
                if (ctx.fn_key.empty()
                    || cg::clang::kind(lhs) != CXCursor_DeclRefExpr)
                {
                    return;
                }
                const CXCursor var = cg::clang::referenced(lhs);
                if (cross_fn_body_decl(var))
                {
                    return;
                }
                const std::string name = cg::clang::spelling(var);
                if (name.empty())
                {
                    return;
                }
                emit_cast_claim(ctx.fn_key, name);
                // 容器拷贝赋值（容器元素局部拷贝流）：`b = a;` 时元素槽随对象
                // 一起继承源元素槽；仅同域变量/字段源（返回容器不走元素流，
                // 返回槽类型是容器对象而非元素）。容器对象的静态模板键不是
                // 元素证据（与 hint_from_expr 对 operator 调用的原则一致），
                // 元素流独立于 keys 通道发射。
                if (related_key.empty() && !related_var.empty()
                    && elem_slot_registered(ctx.fn_key, name))
                {
                    TypeHintRec hint = make_hint(
                        ctx.fn_key,
                        std::string{container_elem_slot_prefix} + name,
                        loc);
                    hint.source = HintSource::ContainerElemFlow;
                    hint.confidence = Confidence::Medium;
                    hint.fallback_reason = "container_elem_flow";
                    hint.related_var = encode_container_elem_slot(related_var);
                    emit_hint(std::move(hint));
                }
                if (keys.empty())
                {
                    // 指针别名 `q = p;` 或返回槽 `p = make();`：只记录传播边，
                    // 动态类型由传播（函数内/全局）从源槽取得。
                    TypeHintRec hint = make_hint(ctx.fn_key, name, loc);
                    hint.source = related_key.empty() ? HintSource::LocalCopy
                                                      : HintSource::ReturnFlow;
                    hint.confidence = Confidence::Medium;
                    hint.fallback_reason = related_key.empty()
                                               ? "local_assignment_single"
                                               : "return_flow";
                    hint.related_var = related_var;
                    hint.related_key = related_key;
                    emit_hint(std::move(hint));
                    return;
                }
                TypeHintRec hint = make_hint(ctx.fn_key, name, loc);
                hint.type_keys = keys;
                hint.source = HintSource::LocalCopy;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = "local_assignment_single";
                emit_hint(std::move(hint));
            }

            /**
             * @brief 字段声明：记录其声明类型（owner = 记录，不是函数）。
             */
            void field_decl(const CXCursor& c)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                const std::string owner =
                    record_key_of_decl(cg::clang::semantic_parent(c));
                const std::string var = cg::clang::spelling(c);
                if (owner.empty() || var.empty())
                {
                    return;
                }
                const std::string static_key = declared_type_key(c);
                // 声明类型是壳类模板实例时追加 pointee 实参记录 key（槽静
                // 态面的语义面：壳主模板键解析不出证明界，消费侧
                // （family_type_allowed）按实参键立界、壳键跳过）。判据与
                // describe_receiver 的接收者剥壳同源（透传判据收敛，零名字
                // 参与）；界外壳由实参独自承载静态面。
                const std::string shell_pointee = shell_pointee_key(c);
                TypeHintRec       hint = make_hint(owner, var, loc);
                if (!static_key.empty())
                {
                    hint.static_types.push_back(static_key);
                }
                if (!shell_pointee.empty())
                {
                    hint.static_types.push_back(shell_pointee);
                }
                emit_hint(std::move(hint));
                // 声明期容器元素槽：字段容器以声明记录为槽所有者；
                // 函数指针/std::function 元素 gate_only（同局部变量侧）。
                if (is_std_container(c))
                {
                    const std::string ek = container_elem_key(c);
                    register_container_elem(
                        owner,
                        var,
                        ek,
                        loc,
                        ek.empty() && container_elem_is_callable(c));
                }
            }

            /** @brief 传播边可解析的槽引用：变量名或"字段名+域"。 */
            struct SlotRef
            {
                /** @brief 字段槽的所属记录 key；空 = 所在函数域（调用方补
                 * fn_key）。 */
                std::string owner_key;
                std::string var_name;
            };

            /**
             * @brief 声明所属的最近函数体游标（语义父级链上溯至函数族 kind）。
             *
             * @internal 局部变量/形参一步即达其函数；lambda 体内声明的上溯止
             *           于其调用运算符（CXXMethod），局部类内声明经类声明再一
             *           步到达。链上无函数族宿主（全局/成员/命名空间级声明）
             *           或上溯越界（深嵌套防御上限 8）返回空游标——调用方把
             *           空游标视为"无函数宿主"，不属跨函数体形态。
             */
            [[nodiscard]]
            static auto owning_function_of(const CXCursor& decl) -> CXCursor
            {
                CXCursor cur = cg::clang::semantic_parent(decl);
                for (int depth = 0; depth < 8 && !cg::clang::is_null(cur)
                                    && !is_invalid_cursor(cur);
                     ++depth)
                {
                    switch (cg::clang::kind(cur))
                    {
                    case CXCursor_FunctionDecl:
                    case CXCursor_FunctionTemplate:
                    case CXCursor_CXXMethod:
                    case CXCursor_Constructor:
                    case CXCursor_Destructor:
                    case CXCursor_ConversionFunction:
                        return cur;
                    default:
                        cur = cg::clang::semantic_parent(cur);
                        break;
                    }
                }
                return CXCursor{};
            }

            /**
             * @brief lambda 跨域记名闸：声明的所属函数体与采集语境函数不是同
             *        一实体时拒绝记名（断链，漏优于错）。
             *
             * @internal 首版语义把 lambda 体内语句按外层函数语境采集：捕获引
             *           用解析回外层声明，所属函数与语境一致，照常记名；同名
             *           遮蔽形态（lambda 体内声明局部/形参遮蔽外层名字）的引
             *           用解析到 lambda 内声明，所属函数是其调用运算符，与语
             *           境函数不同——记名会把内层证据错连进外层槽（裸名同域
             *           join）。判据全 AST 形态（语义父级链 + 游标同一实体比
             *           较），零项目命名。语境关闭（host_fn_ 空，命名空间级
             *           初始化）与无函数宿主声明（全局/成员）不属跨域形态，
             *           放行保持既有语义。消费面：slot_ref_of（ref_var_name
             *           委托）、赋值 LHS 直取名、describe_receiver 接收者名、
             *           local_var/parameter 声明口共五处记名点；describe_
             *           receiver 处只断名字 join，接收者声明的静态类型面照常
             *           保留——静态面是该声明的正确证据，不是错连源。
             */
            [[nodiscard]]
            auto cross_fn_body_decl(const CXCursor& decl) -> bool
            {
                if (cg::clang::is_null(host_fn_) || is_invalid_cursor(host_fn_))
                {
                    return false;
                }
                const CXCursor owner = owning_function_of(decl);
                if (cg::clang::is_null(owner) || is_invalid_cursor(owner))
                {
                    return false;
                }
                return !cg::clang::equal_cursors(owner, host_fn_);
            }

            /**
             * @brief 游标类型指向的一级 record 判定（deref 穿透判据）。
             *
             * @internal 类型是指向 record 的指针/引用且 pointee 声明是
             * Struct/Class（二级指针 pointee 是指针、int* pointee 是内建、函数
             * 指针 pointee 是函数声明，全部天然排除）。判定纯类型形状，零项目
             * 命名。
             */
            [[nodiscard]]
            static auto pointee_is_record(const CXCursor& c) -> bool
            {
                const CXCursor decl = cg::clang::pointee_declaration(c);
                if (cg::clang::is_null(decl) || is_invalid_cursor(decl))
                {
                    return false;
                }
                const CXCursorKind dk = cg::clang::kind(decl);
                return dk == CXCursor_StructDecl || dk == CXCursor_ClassDecl;
            }

            /**
             * @brief callee 第 ordinal 形参是否为 record 引用/指针（deref 穿透
             *        的值切片闸）。
             *
             * @internal deref 实参传值形参时对象切片、动态类型恒为声明类型，
             * 把操作数槽的派生集灌进值形参槽即伪造证据——形参类型必须是指向
             * record 的引用/指针才放行。callee 经 referenced_callable 归一（构
             * 造放行、运算符/析构拒绝）；间接调用与模板 pattern 的 dependent 形
             * 参拿不到 record pointee，一律不放行（宁漏勿猜）。
             */
            [[nodiscard]]
            static auto callee_param_takes_record_ref_or_ptr(
                const CXCursor& call,
                const int       ordinal) -> bool
            {
                const CXCursor callee = referenced_callable(call);
                if (cg::clang::is_null(callee) || is_invalid_cursor(callee))
                {
                    return false;
                }
                int  seen = 0;
                bool verdict = false;
                bool hit = false;
                for_each_child(
                    callee,
                    [&](const CXCursor& ch)
                    {
                        if (hit || cg::clang::kind(ch) != CXCursor_ParmDecl)
                        {
                            return;
                        }
                        if (seen == ordinal)
                        {
                            verdict = cg::clang::is_pointer_or_reference(ch)
                                      && pointee_is_record(ch);
                            hit = true; // 序号到位（找到即停）
                            return;
                        }
                        ++seen;
                    });
                return verdict;
            }

            /**
             * @brief 表达式指向的变量/字段槽（穿透显式转型与一层取址；解引用
             *        为授权形态）。
             *
             * @internal "实参变量/字段域可解析"（出参写回与取址实参共同依赖）
             * 的底座：
             * `x` / `&x` -> 所在函数域的变量名；`this->f` / `obj.f` / `&obj.f`
             * -> 字段名 + 声明记录域（字段槽以声明记录为所有者，与
             * field_target_of 同口径）。解引用 `*p` 是双形态的第二臂，默认关
             * 闭（既有消费面零变化）：授权开启时要求唯一操作数指向 record 的
             * 一级指针（pointee_is_record；二级指针/内建 pointee/函数指针全部
             * 拒绝），穿透后与 `x` 同槽——`*p`（p:T* 槽）求值=指向对象本身，
             * 传给 record 引用/指针形参后新槽语义=同一对象类型集，与 `&x` 同
             * 槽论证对称。callee 形参引用/指针闸不在本函数（无 callee 语境），
             * 由 fill_arg_records 的 deref 分支把关（值切片防线）。其余形态
             * （再解引用、下标、调用结果）为空——&obj 的对象侧证据仍由
             * AddressOfObject 线索承载。DeclRef 目标声明跨函数体（lambda 体内
             * 声明遮蔽外层名字）时返回空——cross_fn_body_decl 闸。
             */
            [[nodiscard]]
            auto slot_ref_of(const CXCursor& e, const bool deref = false)
                -> std::optional<SlotRef>
            {
                CXCursor cur = strip_casts(e);
                if (cg::clang::kind(cur) == CXCursor_UnaryOperator)
                {
                    const std::vector<std::string> toks =
                        cg::clang::token_spellings(cur);
                    if (toks.empty()
                        || (toks.front() != "&" && toks.front() != "*"))
                    {
                        return std::nullopt;
                    }
                    const std::vector<CXCursor> kids = direct_children(cur);
                    if (kids.size() != 1)
                    {
                        return std::nullopt;
                    }
                    if (toks.front() == "*"
                        && (!deref || !pointee_is_record(kids.front())))
                    {
                        return std::nullopt;
                    }
                    cur = unwrap_expr(kids.front());
                }
                const CXCursorKind k = cg::clang::kind(cur);
                if (k == CXCursor_DeclRefExpr)
                {
                    const CXCursor     var = cg::clang::referenced(cur);
                    const CXCursorKind vk = cg::clang::kind(var);
                    if (cg::clang::is_null(var)
                        || (vk != CXCursor_VarDecl && vk != CXCursor_ParmDecl))
                    {
                        return std::nullopt; // 函数/枚举等非变量引用不是槽
                    }
                    if (cross_fn_body_decl(var))
                    {
                        return std::nullopt;
                    }
                    return SlotRef{
                        .owner_key = {},
                        .var_name = cg::clang::spelling(var)};
                }
                if (k == CXCursor_MemberRefExpr)
                {
                    const CXCursor field = cg::clang::referenced(cur);
                    if (cg::clang::is_null(field)
                        || cg::clang::kind(field) != CXCursor_FieldDecl)
                    {
                        return std::nullopt;
                    }
                    return SlotRef{
                        .owner_key = record_key_of_decl(
                            cg::clang::semantic_parent(field)),
                        .var_name = cg::clang::spelling(cur)};
                }
                return std::nullopt;
            }

            /**
             * @brief 全局/命名空间级变量声明 -> 跨函数槽域 key。
             *
             * @internal 全局容器（handlers["k"]=&f 注册表形态）的元素槽
             * 域。局部容器槽以函数域为 owner（跨不了函数）；全局变量的槽域
             * 用 `variable||name|file`（与 extract 的全局变量符号 key 同源形
             * 态——消费端只作 map 键用，不需要符号解析，但保持同源便于对账）。
             * 局部/形参（语义父是函数）返回空 = 仍走函数域。
             */
            [[nodiscard]]
            auto global_var_key_of(const CXCursor& var) -> std::string
            {
                const CXCursor     parent = cg::clang::semantic_parent(var);
                const CXCursorKind pk = cg::clang::kind(parent);
                if (pk == CXCursor_FunctionDecl || pk == CXCursor_CXXMethod
                    || pk == CXCursor_Constructor || pk == CXCursor_Destructor
                    || pk == CXCursor_FunctionTemplate)
                {
                    return {};
                }
                const std::string nm = cg::clang::spelling(var);
                if (nm.empty())
                {
                    return {};
                }
                return std::format(
                    "variable||{}|{}",
                    nm,
                    norm(cg::clang::location(var)));
            }

            /**
             * @brief 槽引用的解析域：字段域优先，其次 DeclRef 全局变量
             *        域，最后函数域兜底。
             */
            [[nodiscard]]
            auto resolve_slot_owner(const CXCursor& expr) -> std::string
            {
                const CXCursor deref = unwrap_expr(expr);
                if (cg::clang::kind(deref) == CXCursor_DeclRefExpr)
                {
                    const CXCursor var = cg::clang::referenced(deref);
                    if (!cg::clang::is_null(var)
                        && cg::clang::kind(var) == CXCursor_VarDecl)
                    {
                        const std::string gk = global_var_key_of(var);
                        if (!gk.empty())
                        {
                            return gk;
                        }
                    }
                }
                return {};
            }

            /**
             * @brief 传播边目标的名字：裸变量名原样；字段实参记"字段名+域"
             *        （行为修正：此前只认
             * DeclRefExpr，字段实参记空名）。
             *
             * @internal `&x` 剥一层取址后与 `x` 同槽（槽对象语义：指向物相同，
             * 上下文不敏感并集方向一致）；字段返回 @fld
             * 描述符（decode_field_slot 解码），由传播阶段（函数内/全局）
             * 按记录域解析。`set(static_cast<I*>(p))` 的实参 传播目标是 p
             * 而不是转型目标类型。
             */
            [[nodiscard]]
            auto ref_var_name(const CXCursor& e) -> std::string
            {
                const auto slot = slot_ref_of(e);
                if (!slot.has_value() || slot->var_name.empty())
                {
                    return {};
                }
                if (slot->owner_key.empty())
                {
                    return slot->var_name;
                }
                return encode_field_slot(slot->owner_key, slot->var_name);
            }

            /** @brief
             * 被调用函数/方法的引用（构造函数调用计入；析构/运算符除外）。 */
            [[nodiscard]]
            static auto referenced_callable(const CXCursor& call) -> CXCursor
            {
                CXCursor ref = cg::clang::referenced(call);
                if (cg::clang::is_null(ref))
                {
                    return ref;
                }
                const CXCursorKind k = cg::clang::kind(ref);
                // Constructor 放行（构造注入半边：构造调用进 call_sites 供全局
                // 传播的出参回填消费）；实参 >=1 的收录门槛在 call_site
                // 侧。析构与 ConversionFunction 维持不收录。
                if (k != CXCursor_CXXMethod && k != CXCursor_FunctionDecl
                    && k != CXCursor_FunctionTemplate
                    && k != CXCursor_Constructor)
                {
                    return CXCursor{};
                }
                const std::string name = cg::clang::spelling(ref);
                if (name.starts_with("operator") || name.starts_with("_M_")
                    || name.starts_with("__") || name.starts_with("_X"))
                {
                    return CXCursor{};
                }
                const CXCursor primary = cg::clang::specialized_template(ref);
                return cg::clang::is_null(primary) ? ref : primary;
            }

            /** @brief 函数引用 -> MethodRef。 */
            /**
             * @brief 平台回调注册点 -> 契约线索记录（callback_regs）。
             *
             * @internal 回调位实参两种直接形态收：取址（&handler）与函数名
             * 隐式转指针（handler）。目标函数的定义处（界内才有库内符号）
             * 经 function_key_of 拼键——与 extract 符号逐字节一致。宁漏勿猜：
             * lambda、经变量/字段中转的注册形态不记（槽传播接管）；目标
             * 定义界外（回调在别的库里）也无处可连，如实跳过。
             */
            void callback_reg_of(
                const Ctx&                       ctx,
                const std::vector<CXCursor>&     kids,
                int                              cb_slot,
                const std::string&               site_file,
                const cg::clang::SourceLocation& site)
            {
                if (ctx.fn_key.empty()
                    || static_cast<std::size_t>(cb_slot) >= kids.size())
                {
                    return;
                }
                CXCursor arg =
                    strip_casts(kids[static_cast<std::size_t>(cb_slot)]);
                // 剥取址一元运算符（&handler -> DeclRef handler）。
                if (cg::clang::kind(arg) == CXCursor_UnaryOperator)
                {
                    const std::vector<CXCursor> sub = direct_children(arg);
                    if (sub.size() == 1)
                    {
                        arg = strip_casts(sub.front());
                    }
                    else
                    {
                        return;
                    }
                }
                if (cg::clang::kind(arg) != CXCursor_DeclRefExpr)
                {
                    return; // lambda/中转变量：宁漏勿猜
                }
                const CXCursor target = cg::clang::referenced(arg);
                if (cg::clang::is_null(target))
                {
                    return;
                }
                const CXCursorKind tk = cg::clang::kind(target);
                if (tk != CXCursor_FunctionDecl && tk != CXCursor_CXXMethod
                    && tk != CXCursor_FunctionTemplate)
                {
                    return; // 非函数目标（对象/变量）：不是本路径语义
                }
                const CXCursor def = [&]
                {
                    const CXCursor d = cg::clang::definition_of(target);
                    return cg::clang::is_null(d) ? target : d;
                }();
                const std::string dfile = norm(cg::clang::location(def));
                if (!under_roots(dfile, opt))
                {
                    return; // 回调定义界外：库内无符号可连
                }
                CallbackRegRec reg;
                reg.caller_key = ctx.fn_key;
                reg.target_key = function_key_of(def, dfile);
                reg.site_file = site_file;
                reg.site_line = static_cast<int>(site.line);
                out.callback_regs.push_back(std::move(reg));
            }

            [[nodiscard]]
            static auto method_ref_of(const CXCursor& fn) -> MethodRef
            {
                MethodRef m;
                m.name = cg::clang::spelling(fn);
                m.ns = ns_path_of(fn);
                m.parent = parent_record_name(fn);
                m.sig_hash = sig_hash_of(cg::clang::arg_type_spellings(fn));
                m.const_method = cg::clang::is_const_method(fn);
                return m;
            }

            /** @brief 是否运算符调用（容器遍历/智能指针包装，不是动态派发）。
             */
            [[nodiscard]]
            static auto is_operator_call(const CXCursor& e) -> bool
            {
                const CXCursor ref = cg::clang::referenced(e);
                return !cg::clang::is_null(ref)
                       && cg::clang::spelling(ref).starts_with("operator");
            }

            /**
             * @brief 实参表达式若是项目内函数调用，返回其延迟返回槽描述符。
             *
             * @internal `set(make())` 的实参不能把 `make` 的声明返回类型当作
             * 动态证据（那是静态类型）；它只能经 `make` 的 `#ret` 槽在全局
             * 固定点传播。这里记录“调用的是谁”，由 propagate
             * 在整库符号表上解析。 传播。这里记录“调用的是谁”，由 propagate
             * 在整库符号表上解析。
             */
            [[nodiscard]]
            auto call_result_key(const CXCursor& e) -> std::string
            {
                // 穿透显式转型：`static_cast<I*>(make())` 的返回槽是 make 的
                // #ret，不是转型目标类型。
                const CXCursor cur = strip_casts(e);
                if (cg::clang::kind(cur) != CXCursor_CallExpr
                    || is_operator_call(cur))
                {
                    return {};
                }
                const CXCursor ref = referenced_callable(cur);
                if (cg::clang::is_null(ref)
                    || cg::clang::kind(ref) == CXCursor_Constructor)
                {
                    return {}; // 构造无返回槽；其证据走实参/初始化路径
                }
                if (!under_roots(norm(cg::clang::location(ref)), opt))
                {
                    return {};
                }
                return encode_method_ref(method_ref_of(ref));
            }

            /** @brief 接收者表达式抽取结果。 */
            struct ReceiverInfo
            {
                std::string name;
                bool        via_this = false;
                /** @brief 字段接收者的声明记录 key（`obj.field` 必需）。 */
                std::string              owner_key;
                std::vector<std::string> static_types;
            };

            /**
             * @brief 透传 getter 调用的对象表达式；非透传 getter 形态为空游标。
             *
             * @internal 判据与登记（passthrough_wrapper_decl / is_passthrough_
             * getter）同源（零项目名参与，canonical type-parameter- 标记不可伪
             * 造）：被调用方法归一到类模板 pattern 后命中零参 + 返回 own 模板
             * 形参。对象表达式取被调用成员表达式的基子（隐式 this 无子 = 空
             * 游标，由调用方放弃——宁漏勿猜）。
             */
            [[nodiscard]]
            static auto passthrough_getter_object(const CXCursor& call)
                -> CXCursor
            {
                if (cg::clang::kind(call) != CXCursor_CallExpr)
                {
                    return CXCursor{};
                }
                const CXCursor ref = cg::clang::referenced(call);
                if (cg::clang::is_null(ref))
                {
                    return CXCursor{};
                }
                const CXCursor fn = pattern_function_of(ref);
                if (cg::clang::kind(fn) != CXCursor_CXXMethod)
                {
                    return CXCursor{};
                }
                const CXCursor cls = pattern_class_of(fn);
                if (cg::clang::is_null(cls)
                    || cg::clang::kind(cls) != CXCursor_ClassTemplate)
                {
                    return CXCursor{};
                }
                int own_count = 0;
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch)
                            == CXCursor_TemplateTypeParameter)
                        {
                            ++own_count;
                        }
                    });
                // 消费侧收紧到值/一层指针（T、T*）：返回 T** 的取址形态（此
                // 类方法族，登记侧照收）语义是"出参槽地址"不是透传值——值槽/链式接
                // 收者按透传消费会把 pointee 类型灌进出参地址槽（污染）；出参
                // 面已有 addressof/取址实参专门路径。
                if (own_count == 0
                    || is_passthrough_getter(fn, own_count, 1) < 0)
                {
                    return CXCursor{};
                }
                CXCursor found{};
                bool     resolved = false;
                for_each_child(
                    call,
                    [&](const CXCursor& kid)
                    {
                        if (resolved)
                        {
                            return; // 已定论（找到即停）
                        }
                        const CXCursor inner = unwrap_expr(kid);
                        if (cg::clang::kind(inner) != CXCursor_MemberRefExpr)
                        {
                            return;
                        }
                        const CXCursor member = cg::clang::referenced(inner);
                        if (cg::clang::is_null(member)
                            || cg::clang::kind(member) != CXCursor_CXXMethod
                            || cg::clang::spelling(member)
                                   != cg::clang::spelling(fn))
                        {
                            return;
                        }
                        const std::vector<CXCursor> member_kids =
                            direct_children(inner);
                        if (member_kids.empty())
                        {
                            resolved =
                                true; // 隐式 this：无对象可寻址（结果空）
                            return;
                        }
                        found = unwrap_expr(member_kids.front());
                        resolved = true;
                    });
                return found;
            }

            /**
             * @brief 递归找函数体的唯一 ReturnStmt 值表达式；多/零 return 或值
             *        缺失为假（>1 提前停）。
             */
            [[nodiscard]]
            static auto sole_return_value(
                const CXCursor& node,
                CXCursor*       value,
                int*            count) -> bool
            {
                bool ok = true;
                for_each_child(
                    node,
                    [&](const CXCursor& ch)
                    {
                        if (!ok)
                        {
                            return; // 已判多 return：短路（找到即停的递归形态）
                        }
                        if (cg::clang::kind(ch) == CXCursor_ReturnStmt)
                        {
                            if (++(*count) > 1)
                            {
                                ok = false;
                                return;
                            }
                            // 逐子覆盖：最终 *value = return 的最后一个直接子。
                            for_each_child(
                                ch,
                                [&](const CXCursor& rc) { *value = rc; });
                            return;
                        }
                        if (!sole_return_value(ch, value, count))
                        {
                            ok = false;
                        }
                    });
                return ok;
            }

            /**
             * @brief 断链 4（最小口径）：普通类 getter 的函数体追源。
             *
             * @internal `Get() const { return p_impl_.Get(); }` 形——非模板壳类
             * 的透传链不在 passthrough 表（判据限类模板）。追源边界（宁漏勿
             * 猜）：被调用方法零参、非虚（虚体可能被覆盖，体不代表全部目标）、
             * 定义在本 TU 可见、函数体（递归计，lambda 内 return 计入）恰一条
             * ReturnStmt、return 表达式剥壳后恰是"透传 getter 调用"→ 接收者 =
             * 该 getter 的对象（即壳字段，其槽类型集按 pointee 语义消费）。usr
             * 记忆化防重复体扫描。
             */
            [[nodiscard]]
            auto plain_getter_field_receiver(const CXCursor& call, int depth)
                -> ReceiverInfo
            {
                if (depth >= 4)
                {
                    return {};
                }
                const CXCursor ref = cg::clang::referenced(call);
                if (cg::clang::is_null(ref)
                    || cg::clang::kind(ref) != CXCursor_CXXMethod
                    || cg::clang::is_virtual_method(ref)
                    || cg::clang::is_static_method(ref))
                {
                    return {};
                }
                bool has_parm = false;
                for_each_child(
                    ref,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch) == CXCursor_ParmDecl)
                        {
                            has_parm = true;
                        }
                    });
                if (has_parm)
                {
                    return {}; // getter 不带参
                }
                const std::string key = cg::clang::usr(ref);
                if (key.empty())
                {
                    return {};
                }
                const auto it = plain_getter_cache_.find(key);
                if (it != plain_getter_cache_.end())
                {
                    return cg::clang::is_null(it->second)
                               ? ReceiverInfo{}
                               : describe_receiver(it->second, depth + 1);
                }
                const auto cache_miss = [&]() -> ReceiverInfo
                {
                    plain_getter_cache_.emplace(key, CXCursor{});
                    return {};
                };
                const CXCursor def = cg::clang::definition_of(ref);
                if (cg::clang::is_null(def) || is_invalid_cursor(def))
                {
                    return cache_miss();
                }
                CXCursor value{};
                int      returns = 0;
                if (!sole_return_value(def, &value, &returns))
                {
                    return cache_miss();
                }
                const CXCursor obj =
                    passthrough_getter_object(unwrap_expr(strip_casts(value)));
                if (cg::clang::is_null(obj))
                {
                    return cache_miss();
                }
                plain_getter_cache_.emplace(key, obj);
                return describe_receiver(obj, depth + 1);
            }

            /**
             * @brief 抽取调用接收者：剥掉智能指针 `operator->`/`operator*`
             * 包装、 隐式转型与 this->字段。
             *
             * @internal `p->f()` 的接收者是
             * `p`；`sp->f()`（unique_ptr）的接收者 是 `sp`（跳过
             * operator->）；`this->p->f()` 的接收者是字段 `p` 且标记
             * via_this，供字段流按记录所有者查找线索。
             */
            [[nodiscard]]
            auto describe_receiver(CXCursor r, int depth = 0) -> ReceiverInfo
            {
                ReceiverInfo info;
                r = unwrap_expr(r);
                switch (cg::clang::kind(r))
                {
                case CXCursor_DeclRefExpr:
                {
                    const CXCursor var = cg::clang::referenced(r);
                    if (cg::clang::is_null(var))
                    {
                        return info;
                    }
                    const bool cross_body = cross_fn_body_decl(var);
                    info.name =
                        cross_body ? std::string{} : cg::clang::spelling(var);
                    const std::string key =
                        record_key_of_decl(cg::clang::type_declaration(var));
                    if (!key.empty())
                    {
                        info.static_types.push_back(key);
                    }
                    // 声明类型是透传壳类模板实例时追加 pointee 实参记录 key
                    // （壳自身的 key 保留：界外壳 key 为空、实参界内时静态类型
                    // 由实参独自承载）。判据零名字（结构判定），标准库/自定义壳
                    // 同享；无可剥实参时行为与剥壳前一致。
                    const std::string pointee = shell_pointee_key(var);
                    if (!pointee.empty())
                    {
                        info.static_types.push_back(pointee);
                    }
                    return info;
                }
                case CXCursor_CXXThisExpr:
                {
                    info.name = "this";
                    const std::string key =
                        record_key_of_decl(cg::clang::type_declaration(r));
                    if (!key.empty())
                    {
                        info.static_types.push_back(key);
                        info.owner_key = key;
                    }
                    return info;
                }
                case CXCursor_UnaryOperator:
                {
                    // 出参解引用（`(*pp_out)->AddRef()`）：接收者 = 解引用
                    // 下的槽（写入 *pp_out 的对象即该槽的内容，槽类型集
                    // 按出参写回臂的 pointee 语义与接收者动态面天然对
                    // 齐）。token 首符 "*" 才是解引用（++/--/取址等其它一
                    // 元形态不碰）；结果整体透传内层（pp_out 的名字、字
                    // 段域与 via_this 原样），静态面不新产——操作数是双
                    // 指针槽，无 record 声明可取，与一级指针接收者
                    // （`p->f()` 的 DeclRefExpr 分支）同口径留空。
                    const std::vector<std::string> toks =
                        cg::clang::token_spellings(r);
                    if (toks.empty() || toks.front() != "*" || depth >= 4)
                    {
                        return info;
                    }
                    const std::vector<CXCursor> kids = direct_children(r);
                    if (kids.empty())
                    {
                        return info;
                    }
                    const ReceiverInfo inner =
                        describe_receiver(kids.front(), depth + 1);
                    if (inner.name.empty() || inner.name == "this")
                    {
                        return info;
                    }
                    return inner;
                }
                case CXCursor_ArraySubscriptExpr:
                {
                    // 容器元素下标（`items_[i]->AddRef()`）：接收者 = 容器
                    // 的元素，槽名 = 元素槽前缀 + 容器名（与声明期
                    // register_container_elem / 写回臂 elem_index_write 的
                    // 槽命名同源对齐，find_slot 按名直配元素槽）。owner 域
                    // 与 via_this 透传容器基表达式（字段容器归声明记录，
                    // 局部容器归函数域）；静态面不新产（元素声明类型经元
                    // 素槽的 Declared 行进入声明面回退，表达式的元素类型
                    // 多为指针，无 record 声明可取）。
                    if (depth >= 4)
                    {
                        return info;
                    }
                    const std::vector<CXCursor> kids = direct_children(r);
                    if (kids.empty())
                    {
                        return info;
                    }
                    const ReceiverInfo base =
                        describe_receiver(kids.front(), depth + 1);
                    if (base.name.empty() || base.name == "this"
                        || base.name.starts_with(container_elem_slot_prefix))
                    {
                        return info;
                    }
                    info.name =
                        std::string{container_elem_slot_prefix} + base.name;
                    info.via_this = base.via_this;
                    info.owner_key = base.owner_key;
                    return info;
                }
                case CXCursor_MemberRefExpr:
                {
                    // this->p / obj.p：值语义的接收者是字段本身。
                    const std::vector<CXCursor> kids = direct_children(r);
                    info.name = cg::clang::spelling(r);
                    // 本 libclang 对隐式 this 成员访问不暴露基表达式（无子）；
                    // 只有显式对象访问才有子。无子 => this->field。
                    if (kids.empty())
                    {
                        info.via_this = true;
                    }
                    else
                    {
                        const ReceiverInfo object =
                            describe_receiver(kids.front());
                        info.via_this =
                            object.name == "this" || object.via_this;
                    }
                    // 字段槽的所属记录来自 FieldDecl 的语义父，显式对象访问
                    // （obj.field）也能据此找到槽。
                    const CXCursor field = cg::clang::referenced(r);
                    if (!cg::clang::is_null(field))
                    {
                        info.owner_key = record_key_of_decl(
                            cg::clang::semantic_parent(field));
                    }
                    const std::string key =
                        record_key_of_decl(cg::clang::type_declaration(r));
                    if (!key.empty())
                    {
                        info.static_types.push_back(key);
                    }
                    // 字段类型是透传壳时同款追加 pointee 实参（隐式 this 字段
                    // 与显式对象字段的静态面同源，剥壳判据与变量分支一致）。
                    const std::string pointee = shell_pointee_key(r);
                    if (!pointee.empty())
                    {
                        info.static_types.push_back(pointee);
                    }
                    return info;
                }
                case CXCursor_CallExpr:
                {
                    const CXCursor    ref = cg::clang::referenced(r);
                    const std::string callee = cg::clang::is_null(ref)
                                                   ? std::string{}
                                                   : cg::clang::spelling(ref);
                    // 容器下标（`items_[i]->AddRef()`）：接收者语义是容器
                    // 的元素不是容器本身（容器槽类型集是容器对象），槽名
                    // 包元素前缀（与声明期 register_container_elem / 写回
                    // 臂 elem_index_write 的槽命名对齐，find_slot 按名直配
                    // 元素槽）。静态面不新产：元素声明类型经元素槽的
                    // Declared 行进入声明面回退。
                    const bool elem_index = callee == "operator[]";
                    if (callee == "operator->" || callee == "operator*"
                        || elem_index)
                    {
                        // libclang 的 CXXOperatorCallExpr 子顺序与 AST dump
                        // 不同：
                        // 对象表达式在前、运算符函数引用在后；跳过函数引用，
                        // 取第一个真正描述对象/变量的子表达式。
                        bool resolved = false;
                        for_each_child(
                            r,
                            [&](const CXCursor& kid)
                            {
                                if (resolved)
                                {
                                    return; // 已取得接收者（找到即停）
                                }
                                const CXCursor unwrapped_kid = unwrap_expr(kid);
                                const CXCursor kid_ref =
                                    cg::clang::referenced(unwrapped_kid);
                                if (!cg::clang::is_null(kid_ref)
                                    && cg::clang::spelling(kid_ref).starts_with(
                                        "operator"))
                                {
                                    return;
                                }
                                if (cg::clang::kind(unwrapped_kid)
                                        == CXCursor_DeclRefExpr
                                    && !cg::clang::is_null(kid_ref)
                                    && (cg::clang::kind(kid_ref)
                                            == CXCursor_CXXMethod
                                        || cg::clang::kind(kid_ref)
                                               == CXCursor_FunctionDecl
                                        || cg::clang::kind(kid_ref)
                                               == CXCursor_FunctionTemplate))
                                {
                                    return; // 运算符函数引用
                                }
                                ReceiverInfo object =
                                    describe_receiver(kid, depth + 1);
                                if (!object.name.empty())
                                {
                                    info = object;
                                    resolved = true;
                                }
                            });
                        if (elem_index && resolved && info.name != "this"
                            && !info.name.starts_with(
                                container_elem_slot_prefix))
                        {
                            info.name = std::string{container_elem_slot_prefix}
                                        + info.name;
                        }
                        return info;
                    }
                    // 断链 1/3：透传 getter 链式接收者（p.Get()->Virt()）——
                    // getter 返回 own 模板形参，值即壳实例槽的 pointee，接收者
                    // 语义 = 壳实例（其槽类型集按构造穿透惯例即 pointee
                    // 集）。解到 对象表达式递归解读。
                    if (depth < 4 && !cg::clang::is_null(ref)
                        && cg::clang::kind(ref) == CXCursor_CXXMethod)
                    {
                        const CXCursor obj = passthrough_getter_object(r);
                        if (!cg::clang::is_null(obj))
                        {
                            const ReceiverInfo inner =
                                describe_receiver(obj, depth + 1);
                            if (!inner.name.empty() && inner.name != "this")
                            {
                                return inner;
                            }
                            return info;
                        }
                        // 断链 4（最小口径）：非模板壳透传链——函数体恰一条
                        // return 且 return 的是壳字段上的透传 getter 调用时，接
                        // 收者 = 该壳字段（plain_getter_field_receiver 内含全部
                        // 边界）。
                        const ReceiverInfo chased =
                            plain_getter_field_receiver(r, depth);
                        if (!chased.name.empty() && chased.name != "this")
                        {
                            return chased;
                        }
                        // 断链 5：静态方法壳工厂链式接收者（`类::Attach(p)->
                        // Virt()`）——工厂返回壳、T* 位实参才是 pointee 载体，
                        // 链式接收者语义 = 工厂 T* 位实参的接收者（与透传
                        // getter 方向相反：getter 是 壳.方法() 返回 T*，工厂是
                        // 类::方法(T*) 返回壳）。
                        const CXCursor factory_arg =
                            shell_factory_pointee_argument(r, ref);
                        if (!cg::clang::is_null(factory_arg))
                        {
                            const ReceiverInfo inner =
                                describe_receiver(factory_arg, depth + 1);
                            if (!inner.name.empty() && inner.name != "this")
                            {
                                return inner;
                            }
                            return info;
                        }
                    }
                    return info;
                }
                default:
                    return info;
                }
            }

            /**
             * @brief 类型线索发射的公共骨架（位置三件套 + Declared/Low
             * 默认档）。
             *
             * @internal 各发射点的指定初始化器样板收口于此：file/line/col 恒取
             * 自同一个 SourceLocation，source/confidence/fallback_reason 的
             * 传播默认（Declared/Low/"declared"）在此一次定档，差异字段
             * （type_keys/static_types/related_* 等）由调用方在返回值上覆写后
             * 再 emit_hint；norm 自带 memo 缓存，重复取 file 无额外代价。
             */
            [[nodiscard]]
            auto make_hint(
                const std::string&               owner_key,
                const std::string&               var_name,
                const cg::clang::SourceLocation& loc) -> TypeHintRec
            {
                TypeHintRec hint;
                hint.owner_key = owner_key;
                hint.var_name = var_name;
                hint.file = norm(loc);
                hint.line = static_cast<int>(loc.line);
                hint.col = static_cast<int>(loc.column);
                hint.source = HintSource::Declared;
                hint.confidence = Confidence::Low;
                hint.fallback_reason = "declared";
                return hint;
            }

            void emit_hint(TypeHintRec hint)
            {
                if (hint.owner_key.empty() || hint.var_name.empty())
                {
                    return;
                }
                std::ranges::sort(hint.type_keys);
                hint.type_keys.erase(
                    std::unique(hint.type_keys.begin(), hint.type_keys.end()),
                    hint.type_keys.end());
                std::ranges::sort(hint.static_types);
                hint.static_types.erase(
                    std::unique(
                        hint.static_types.begin(),
                        hint.static_types.end()),
                    hint.static_types.end());
                const bool declared_static = hint.source == HintSource::Declared
                                             && !hint.static_types.empty();
                const bool param_slot = hint.source == HintSource::ParamFlow;
                if (!declared_static && !param_slot && hint.type_keys.empty()
                    && hint.related_var.empty() && hint.related_key.empty())
                {
                    return; // 没有传播价值
                }
                const std::string key = std::format(
                    "{}|{}|{}|{}|{}|{}",
                    hint.owner_key,
                    hint.var_name,
                    hint.file,
                    hint.line,
                    hint.col,
                    static_cast<int>(hint.source));
                if (hint_seen_.insert(key).second)
                {
                    out.hints.push_back(std::move(hint));
                }
            }

            /**
             * @brief 出参包装壳构造登记绑定（自定义包装句柄`<T>` v(pp)
             * 形态）。
             *
             * @internal 出参包装类结构判据（is_outparam_wrapper_class，零名字）
             * 识别类；ctor_param_pos 指出 T** 槽实参位。绑定表按 (fn_key,
             * 壳变量名) 记 {出参槽, 写回方法名}；写回在 call_site
             * 侧按同名接收者
             * + 写回方法名双校验触发。花括号初始化（InitListExpr 包装不折叠成
             * Constructor 引用的 CallExpr）与 bind() 方法绑定不在本形态——
             * fallback 清单，宁漏勿误。
             */
            [[nodiscard]]
            auto bind_outparam_shell(
                const CXCursor&    expr,
                const CXCursor&    ctor,
                const std::string& fn_key,
                const std::string& var) -> bool
            {
                if (fn_key.empty() || var.empty()
                    || var == std::string{return_slot})
                {
                    return false;
                }
                const CXCursor cls = pattern_class_of(ctor);
                if (cg::clang::is_null(cls))
                {
                    return false;
                }
                const std::string key = cg::clang::usr(cls);
                auto              it = wrapper_cache_.find(key);
                if (it == wrapper_cache_.end())
                {
                    it = wrapper_cache_
                             .emplace(key, is_outparam_wrapper_class(cls))
                             .first;
                }
                if (!it->second.has_value())
                {
                    return false;
                }
                const OutparamWrapper&      w = *it->second;
                const std::vector<CXCursor> args = ctor_arguments(expr, ctor);
                if (static_cast<int>(args.size()) <= w.ctor_param_pos)
                {
                    return false;
                }
                const auto slot = slot_ref_of(args[w.ctor_param_pos]);
                if (!slot.has_value() || slot->var_name.empty())
                {
                    return false;
                }
                OutparamBinding binding;
                binding.owner_key =
                    slot->owner_key.empty() ? fn_key : slot->owner_key;
                binding.var_name = slot->var_name;
                binding.writeback = w.writeback;
                outparam_bindings_.emplace(
                    cg::util::compose_key(fn_key, var),
                    std::move(binding));
                return true;
            }

            /**
             * @brief 指针式壳构造穿透的 pointee 实参；不适格为空游标。
             *
             * @internal 单一判据（is_pointer_shell_class）+ 构造恰一个 T* 形参
             * （single_tp_pointer_param，pattern 语境）双闸；实参按位对齐取回。
             * 穿透 = 把该实参交给 hint_from_expr 递归，证据级别随实参形态保级。
             */
            [[nodiscard]]
            auto shell_pointee_argument(
                const CXCursor& expr,
                const CXCursor& ctor) -> CXCursor
            {
                const CXCursor cls = pattern_class_of(ctor);
                if (cg::clang::is_null(cls) || !is_pointer_shell_cached(cls))
                {
                    return CXCursor{};
                }
                const int pos =
                    single_tp_pointer_param(pattern_function_of(ctor), cls);
                if (pos < 0)
                {
                    return CXCursor{};
                }
                const std::vector<CXCursor> args = ctor_arguments(expr, ctor);
                return pos < static_cast<int>(args.size()) ? args[pos]
                                                           : CXCursor{};
            }

            /**
             * @brief 断链 5 判据：静态方法壳工厂调用的 pointee 实参；不适格空。
             *
             * @internal 五闸全 AST 形态（零名字）：①被调是静态 CXXMethod（构造
             *           走构造穿透既有路径；free
             * 函数工厂的所属类经语义父非记录出
             *           局，非静态成员返回壳不具工厂语义拒）；②所属 pattern 类
             *           是指针式壳（is_pointer_shell_class 单一判据，非模板壳与
             *           无 operator-> 类出局）；③恰一个 T* 形参位
             *           （single_tp_pointer_param，pattern 语境——多 T* 位无法
             *           证明唯一 pointee 槽、值形参非指针均拒）；④返回 own 模板
             *           形参壳：返回类型声明经 specialized_template
             * 归一到主模板 后与 pattern
             * 类同一实体（判定锚点必须在调用点实例方法 上取返回类型——pattern
             * 声明语境的返回类型会退化为当前
             *           实例化记录而误拒；返回标量/异类记录天然出局）；⑤实参按
             *           位对齐：跳过首子函数引用与类型引用（与 ctor_arguments
             *           同一防御——首子函数引用的引用目标即本方法、usr 相同被
             *           跳过）后取第 pos 个实参。穿透语义与构造穿透同款：T*
             * 位实参 即 pointee 直接证据，级别随实参形态保级（new 保持
             *           DirectNew High）。
             */
            [[nodiscard]]
            auto shell_factory_pointee_argument(
                const CXCursor& expr,
                const CXCursor& ref) -> CXCursor
            {
                if (cg::clang::kind(expr) != CXCursor_CallExpr
                    || cg::clang::is_null(ref)
                    || cg::clang::kind(ref) != CXCursor_CXXMethod
                    || !cg::clang::is_static_method(ref))
                {
                    return CXCursor{};
                }
                const CXCursor cls = pattern_class_of(ref);
                if (cg::clang::is_null(cls) || !is_pointer_shell_cached(cls))
                {
                    return CXCursor{};
                }
                const int pos =
                    single_tp_pointer_param(pattern_function_of(ref), cls);
                if (pos < 0)
                {
                    return CXCursor{};
                }
                const CXCursor ret_decl =
                    cg::clang::result_type_declaration(ref);
                if (cg::clang::is_null(ret_decl) || is_invalid_cursor(ret_decl))
                {
                    return CXCursor{};
                }
                const CXCursor ret_pattern =
                    cg::clang::specialized_template(ret_decl);
                if (cg::clang::is_null(ret_pattern)
                    || !cg::clang::equal_cursors(ret_pattern, cls))
                {
                    return CXCursor{};
                }
                const std::vector<CXCursor> args = ctor_arguments(expr, ref);
                return pos < static_cast<int>(args.size()) ? args[pos]
                                                           : CXCursor{};
            }

            /**
             * @brief 壳静态方法形状闸（断链 5 的阻断面）。
             *
             * @internal 被调是静态 CXXMethod 且所属 pattern 类是指针式壳——工厂
             *           判据细节（恰一 T* 位/返回 own 壳）不过时，值槽落点仍是
             *           壳槽（pointee 语义），"按值返回记录"兜底与函数体返回值
             *           追源都可能把壳类型灌进壳槽，整档不发（宁漏勿猜，与
             *           构造穿透版的壳阻断同款）；非模板壳、free
             * 函数与界外静态方 法不属本闸（既有路径照走）。
             */
            [[nodiscard]]
            auto is_shell_static_factory_shape(const CXCursor& ref) -> bool
            {
                if (cg::clang::is_null(ref)
                    || cg::clang::kind(ref) != CXCursor_CXXMethod
                    || !cg::clang::is_static_method(ref))
                {
                    return false;
                }
                const CXCursor cls = pattern_class_of(ref);
                return !cg::clang::is_null(cls) && is_pointer_shell_cached(cls);
            }

            /**
             * @brief 壳转型/构造剥壳判定：表达式声明类型（剥糖）的记录是
             *        "指针式壳"——其 pattern 类有 operator-> 且 canonical 结果
             *        剥一层含 type-parameter-（与 container_elem_key 的元素
             *        剥壳同一判据，零名字参与）。
             */
            [[nodiscard]]
            bool casts_to_pointer_shell(const CXCursor& expr)
            {
                const CXCursor inst = cg::clang::type_declaration(expr);
                if (cg::clang::is_null(inst) || is_invalid_cursor(inst))
                {
                    return false;
                }
                const CXCursor pattern = cg::clang::specialized_template(inst);
                if (cg::clang::is_null(pattern) || is_invalid_cursor(pattern)
                    || cg::clang::kind(pattern) != CXCursor_ClassTemplate)
                {
                    return false;
                }
                bool shell = false;
                for_each_child(
                    pattern,
                    [&](const CXCursor& ch)
                    {
                        if (shell)
                        {
                            return; // 已命中（找到即停）
                        }
                        const CXCursorKind k = cg::clang::kind(ch);
                        if ((k != CXCursor_CXXMethod
                             && k != CXCursor_FunctionTemplate)
                            || cg::clang::spelling(ch) != "operator->")
                        {
                            return;
                        }
                        if (const auto pointee =
                                cg::clang::canonical_result_pointee_spelling(
                                    ch,
                                    1);
                            pointee
                            && pointee->find("type-parameter-")
                                   != std::string::npos)
                        {
                            shell = true;
                        }
                    });
                return shell;
            }

            /**
             * @brief iid 令牌调用的第 0 模板实参记录；非令牌形态为空游标。
             *
             * @internal 令牌半闸（零名字）：实参是恰零值实参的模板（函数
             *           模板或其实例）调用、恰一个显式类型模板实参且解到
             *           记录。调用游标的结果类型不带模板实参面，显式实参
             *           的唯一结构载体是被调表达式子树里的 TypeRef 子（调
             *           用层与被调表达式层合计恰一）；值实计数只看调用层
             *           （恰被调表达式一个）。接口性/形参位/同接口配对由
             *           各消费闸补齐。
             */
            [[nodiscard]]
            static auto iid_token_template_record(const CXCursor& arg)
                -> CXCursor
            {
                const CXCursor core = unwrap_expr(strip_casts(arg));
                if (cg::clang::kind(core) != CXCursor_CallExpr)
                {
                    return CXCursor{};
                }
                const CXCursor callee = cg::clang::referenced(core);
                if (cg::clang::is_null(callee))
                {
                    return CXCursor{};
                }
                const CXCursorKind ck = cg::clang::kind(callee);
                if (ck != CXCursor_FunctionTemplate
                    && (ck != CXCursor_FunctionDecl
                        || cg::clang::is_null(
                            cg::clang::specialized_template(callee))))
                {
                    return CXCursor{};
                }
                const std::vector<CXCursor> kids = direct_children(core);
                if (kids.empty())
                {
                    return CXCursor{};
                }
                std::size_t           non_type = 0;
                std::vector<CXCursor> type_refs;
                const auto collect_type_ref = [&](const CXCursor& parent)
                {
                    for (const CXCursor& kid : direct_children(parent))
                    {
                        if (cg::clang::kind(kid) == CXCursor_TypeRef)
                        {
                            type_refs.push_back(kid);
                        }
                    }
                };
                for (const CXCursor& kid : kids)
                {
                    if (cg::clang::kind(kid) == CXCursor_TypeRef)
                    {
                        type_refs.push_back(kid);
                        continue;
                    }
                    if (!is_type_ref(kid))
                    {
                        ++non_type;
                    }
                }
                collect_type_ref(unwrap_expr(kids.front()));
                if (non_type != 1 || type_refs.size() != 1)
                {
                    return CXCursor{}; // 恰被调表达式（零值实参）+ 单类型实参
                }
                const CXCursor rec = cg::clang::referenced(type_refs.front());
                if (cg::clang::is_null(rec) || is_invalid_cursor(rec)
                    || !is_record_kind(cg::clang::kind(rec)))
                {
                    return CXCursor{};
                }
                return rec;
            }

            /**
             * @brief iid 锁成员模板调用的接口记录（recv.As`<IFace>`(
             *        IidOf`<IFace>`()) 家族）；不适格为空游标。
             *
             * @internal 四闸纯结构（零名字）：被调是 CXXMethod；调用恰一个
             *           显式类型模板实参（TypeRef 子）且是含纯虚直属成员
             *           的记录（接口性）；被调结果类型一级剥层后与该接口
             *           同记录（pattern 归一比对）；存在模板实参同为该接
             *           口的 iid 令牌实参（QI 契约锚点——结果由接口身份
             *           限定）。命中返回调用点实参游标（record_key_of_decl
             *           负责域归一）。
             */
            [[nodiscard]]
            auto iid_locked_call_result(const CXCursor& call) -> CXCursor
            {
                const CXCursor ref = cg::clang::referenced(call);
                if (cg::clang::is_null(ref)
                    || cg::clang::kind(ref) != CXCursor_CXXMethod)
                {
                    return CXCursor{};
                }
                // 恰一个显式类型模板实参（TypeRef 子；成员模板调用的实
                // 参子可挂调用层或被调成员表达式层，两处合计恰一）。
                std::vector<CXCursor> kids = direct_children(call);
                std::vector<CXCursor> type_refs;
                const auto collect_type_ref = [&](const CXCursor& parent)
                {
                    for (const CXCursor& kid : direct_children(parent))
                    {
                        if (cg::clang::kind(kid) == CXCursor_TypeRef)
                        {
                            type_refs.push_back(kid);
                        }
                    }
                };
                collect_type_ref(call);
                if (!kids.empty())
                {
                    collect_type_ref(unwrap_expr(kids.front()));
                }
                if (type_refs.size() != 1)
                {
                    return CXCursor{};
                }
                const CXCursor iface = type_refs.front();
                const CXCursor iface_decl = cg::clang::referenced(iface);
                if (cg::clang::is_null(iface_decl)
                    || is_invalid_cursor(iface_decl)
                    || !is_record_kind(cg::clang::kind(iface_decl))
                    || !has_pure_virtual_member(iface_decl))
                {
                    return CXCursor{};
                }
                if (!cg::clang::result_is_pointer_or_reference(ref))
                {
                    return CXCursor{};
                }
                const CXCursor ret = cg::clang::result_type_declaration(ref);
                if (cg::clang::is_null(ret) || is_invalid_cursor(ret)
                    || !cg::clang::equal_cursors(
                        normalize_record_decl(ret),
                        normalize_record_decl(iface_decl)))
                {
                    return CXCursor{};
                }
                for (std::size_t i = 1; i < kids.size(); ++i)
                {
                    if (cg::clang::kind(kids[i]) == CXCursor_TypeRef)
                    {
                        continue;
                    }
                    const CXCursor token = iid_token_template_record(kids[i]);
                    if (cg::clang::is_null(token) || is_invalid_cursor(token))
                    {
                        continue;
                    }
                    if (cg::clang::equal_cursors(
                            normalize_record_decl(token),
                            normalize_record_decl(iface_decl)))
                    {
                        return iface_decl;
                    }
                }
                return CXCursor{};
            }

            /**
             * @brief 从初始化/返回表达式推导线索；返回是否产出了动态线索。
             */
            void hint_from_expr(
                const CXCursor&    expr,
                const std::string& owner,
                const std::string& var,
                const std::string& file,
                int                line,
                int                col)
            {
                const CXCursorKind outer_kind = cg::clang::kind(expr);
                const CXCursor     inner = unwrap_expr(expr);
                if (cg::clang::kind(inner) != outer_kind)
                {
                    hint_from_expr(inner, owner, var, file, line, col);
                    return;
                }
                // 转型声称行：与既有转型臂/别名行并行
                // 落库（行键含 source，天然不冲突），类型只在传播终态槽为空时
                // 填入——有更强证据的站点行为不变。入口闸前移：非转型/非包装
                // 表达式一次 kind 读取即早退。
                if (cast_claim_shape(outer_kind))
                {
                    if (const std::string claim = cast_claim_target(expr);
                        !claim.empty())
                    {
                        TypeHintRec cast_hint;
                        cast_hint.owner_key = owner;
                        cast_hint.var_name = var;
                        cast_hint.file = file;
                        cast_hint.line = line;
                        cast_hint.col = col;
                        cast_hint.type_keys.assign(1, claim);
                        cast_hint.source = HintSource::CastTarget;
                        cast_hint.confidence = Confidence::Low;
                        cast_hint.fallback_reason =
                            std::string{source_name(HintSource::CastTarget)};
                        emit_hint(std::move(cast_hint));
                    }
                }
                const CXCursorKind k = outer_kind;
                TypeHintRec        hint;
                hint.owner_key = owner;
                hint.var_name = var;
                hint.file = file;
                hint.line = line;
                hint.col = col;
                hint.static_types = expr_type_keys(expr);
                auto with_source = [&](HintSource s, Confidence c)
                {
                    hint.source = s;
                    hint.confidence = c;
                    if (hint.fallback_reason.empty())
                    {
                        hint.fallback_reason = std::string{source_name(s)};
                    }
                    emit_hint(hint);
                };
                switch (k)
                {
                case CXCursor_CXXNewExpr:
                    hint.type_keys = expr_type_keys(expr);
                    with_source(HintSource::DirectNew, Confidence::High);
                    return;
                case CXCursor_InitListExpr:
                {
                    // 指针式壳的花括号构造（Box b = {p}; / return {p};）：落点
                    // 槽的消费语义是 pointee（与构造穿透同款惯例），壳自身静
                    // 态类型绝不能当动态证据——单非类型子穿透递归保级，其余
                    // （空/多元素）整档 no-op，阻断 DirectAggregate 兜底把壳
                    // 类型灌进壳槽（宁漏勿猜）。
                    if (typed_pointer_shell(expr))
                    {
                        std::vector<CXCursor> real;
                        for (const CXCursor& op : direct_children(expr))
                        {
                            if (!is_type_ref(op))
                            {
                                real.push_back(op);
                            }
                        }
                        if (real.size() == 1)
                        {
                            hint_from_expr(
                                unwrap_expr(strip_casts(real.front())),
                                owner,
                                var,
                                file,
                                line,
                                col);
                        }
                        return;
                    }
                    hint.type_keys = expr_type_keys(expr);
                    with_source(HintSource::DirectAggregate, Confidence::High);
                    return;
                }
                case CXCursor_ConditionalOperator:
                    // 分支合流：各分支动态类型的并集；多候选在传播阶段统一
                    // cap 到 low，绝不挑一个分支。原因保留合流来源，避免
                    // 多类型时被误标成 param_flow_multiple。
                    hint.type_keys = expr_type_keys(expr);
                    hint.fallback_reason = "conditional_merge";
                    with_source(
                        HintSource::DirectAggregate,
                        Confidence::Medium);
                    return;
                case CXCursor_CXXStaticCastExpr:
                case CXCursor_CXXDynamicCastExpr:
                case CXCursor_CXXFunctionalCastExpr:
                case CXCursor_CXXConstCastExpr:
                case CXCursor_CXXReinterpretCastExpr:
                {
                    // 指针式壳转型/构造（Box`<P>`(raw) 等函数式转型形态）：目标
                    // pattern 的 operator-> 含 type-parameter-（与构造穿透
                    // 同一判据）——
                    // 壳对象不是动态类型证据，穿透唯一操作数递归保级。
                    if (casts_to_pointer_shell(expr))
                    {
                        std::vector<CXCursor> real;
                        for (const CXCursor& op : direct_children(expr))
                        {
                            if (!is_type_ref(op))
                            {
                                real.push_back(op);
                            }
                        }
                        if (real.size() == 1)
                        {
                            hint_from_expr(
                                real.front(),
                                owner,
                                var,
                                file,
                                line,
                                col);
                        }
                        return;
                    }
                    // 转型目标只有在可证明的向下转型时才由 expr_type_keys
                    // 保留；其余情形只记录操作数的传播边（变量别名 / 返回槽），
                    // 由传播（函数内/全局）从真实动态证据取得类型。
                    hint.type_keys = expr_type_keys(expr);
                    hint.related_var = ref_var_name(expr);
                    hint.related_key = call_result_key(expr);
                    if (hint.related_var.empty() && hint.related_key.empty()
                        && hint.type_keys.empty())
                    {
                        return;
                    }
                    // 操作数自身有动态证据才是 Medium；只有可证明向下转型的
                    // 目标类型（或仅一条传播边）一律从 Low 起，绝不抬成
                    // Medium。
                    const bool operand_has_evidence =
                        !expr_type_keys(strip_casts(expr)).empty();
                    with_source(
                        HintSource::CastDown,
                        operand_has_evidence ? Confidence::Medium
                                             : Confidence::Low);
                    return;
                }
                case CXCursor_UnaryOperator:
                {
                    const std::vector<std::string> toks =
                        cg::clang::token_spellings(expr);
                    if (!toks.empty() && toks.front() == "&")
                    {
                        hint.type_keys = expr_type_keys(expr);
                        with_source(
                            HintSource::AddressOfObject,
                            Confidence::High);
                        return;
                    }
                    break;
                }
                case CXCursor_DeclRefExpr:
                {
                    hint.related_var = ref_var_name(expr);
                    if (!hint.related_var.empty())
                    {
                        with_source(HintSource::LocalCopy, Confidence::Medium);
                        return;
                    }
                    break;
                }
                case CXCursor_CallExpr:
                {
                    // 运算符调用绝不提供对象创建证据：range-for 绑定与智能指针
                    // 接收者是 `operator*`/`operator->`
                    // 调用，其静态元素类型不是 动态类型。
                    if (const CXCursor any_ref = cg::clang::referenced(expr);
                        !cg::clang::is_null(any_ref)
                        && cg::clang::spelling(any_ref).starts_with("operator"))
                    {
                        return;
                    }
                    // iid 锁形态的成员模板调用（recv.As<IFace>(IidOf<IFace>())
                    // 家族）：单显式类型模板实参是接口记录、存在同接口的 iid
                    // 令牌实参（QI 契约锚点）、结果类型是该接口一级指针——
                    // 结果槽落接口级类型（实现族闭包由 propagate 展开，实现
                    // 族多于一时决议端停 may）。判据纯结构（零名字），命中整
                    // 档终结（下方返回边/记录兜底都不适用）。
                    if (const CXCursor iface = iid_locked_call_result(expr);
                        !cg::clang::is_null(iface))
                    {
                        // As 形态同款熔断（见 iid_lock_call 的熔断注释）：
                        // 判据命中整档终结，证据行不发射。
                        // const std::string iface_key =
                        //     record_key_of_decl(iface);
                        // if (!iface_key.empty())
                        // {
                        //     hint.type_keys = {iface_key};
                        //     hint.fallback_reason = "qi_iid_ret";
                        //     with_source(
                        //         HintSource::ReturnFlow,
                        //         Confidence::Medium);
                        // }
                        return;
                    }
                    // 断链 5（值槽/返回槽）：静态方法壳工厂穿透——`类::方法(T*)`
                    // 返回壳的工厂形态与构造穿透方向互补（构造是
                    // `壳(实参)`、工厂是 `类::方法(实参)`），T* 位实参才是
                    // pointee 证据，命中即递归保级；壳上静态方法而判据细节不过
                    // （多 T* 位/无 T* 位/返回非 own 壳）时整档不发：值槽落点是
                    // 壳槽（pointee 语义），下方"按值返回记录"兜底与函数体返回
                    // 值追源都会把壳类型灌进壳槽（与构造版壳阻断同款，宁漏勿
                    // 猜）。
                    if (const CXCursor factory_ref =
                            cg::clang::referenced(expr);
                        !cg::clang::is_null(factory_ref)
                        && cg::clang::kind(factory_ref) == CXCursor_CXXMethod
                        && cg::clang::is_static_method(factory_ref))
                    {
                        const CXCursor source =
                            shell_factory_pointee_argument(expr, factory_ref);
                        if (!cg::clang::is_null(source))
                        {
                            hint_from_expr(source, owner, var, file, line, col);
                            return;
                        }
                        if (is_shell_static_factory_shape(factory_ref))
                        {
                            return;
                        }
                    }
                    // 构造折叠（Constructor 引用的 CallExpr）三档：
                    // 出参包装壳（结构判据命中）构造只登记函数内绑定表
                    //       （pp 槽），不把壳当证据，维持既有 DirectAggregate
                    //       兜底（Set 写回在 call_site 侧接线）；
                    // 指针式壳（operator-> 剥一层 canonical 含
                    //       type-parameter- 的单一判据 + 直属 T* 字段存储面）的
                    //       构造穿透——恰一个 T* 形参位的实参才是 pointee 证据，
                    //       递归保级（new 保持 DirectNew High
                    //       等实参证据置信）；
                    // 既有名义档：unique_ptr/shared_ptr 拼写（libc++ 成员模板
                    //       构造深度 1，不满足穿透判据的 own
                    //       下标判据，靠名字兜底）。
                    if (const CXCursor any_ref = cg::clang::referenced(expr);
                        !cg::clang::is_null(any_ref)
                        && cg::clang::kind(any_ref) == CXCursor_Constructor)
                    {
                        if (bind_outparam_shell(expr, any_ref, owner, var))
                        {
                            // 绑定已登记，落 DirectAggregate 兜底。
                        }
                        else if (is_smart_pointer_construction(expr))
                        {
                            const CXCursor operand = cast_operand(expr);
                            if (!cg::clang::is_null(operand))
                            {
                                hint_from_expr(
                                    operand,
                                    owner,
                                    var,
                                    file,
                                    line,
                                    col);
                            }
                            return;
                        }
                        else
                        {
                            const CXCursor source =
                                shell_pointee_argument(expr, any_ref);
                            if (!cg::clang::is_null(source))
                            {
                                hint_from_expr(
                                    source,
                                    owner,
                                    var,
                                    file,
                                    line,
                                    col);
                                return;
                            }
                            // 壳构造无 T* 形参位（默认/拷贝/move 构造）：落点槽
                            // 的消费语义是 pointee（与构造穿透同款
                            // 惯例），壳自身的静态类型 绝不能当 pointee
                            // 动态证据——整档 no-op（宁漏勿
                            // 猜），阻断下方"按值返回记录"的 DirectAggregate 兜
                            // 底把壳类型灌进壳槽。拷贝/move 构造的单源操作数
                            // 发 pointee 别名行到源壳变量槽（返回槽壳透传，
                            // 断链在中继第一跳的补边）。
                            const CXCursor shell_cls =
                                pattern_class_of(any_ref);
                            if (!cg::clang::is_null(shell_cls)
                                && is_pointer_shell_cached(shell_cls))
                            {
                                shell_copy_source_alias(
                                    expr,
                                    any_ref,
                                    owner,
                                    var);
                                return;
                            }
                        }
                    }
                    const CXCursor ref = referenced_callable(expr);
                    if (!cg::clang::is_null(ref)
                        && cg::clang::kind(ref) != CXCursor_Constructor)
                    {
                        const std::string callee = cg::clang::spelling(ref);
                        if (callee == "make_unique" || callee == "make_shared")
                        {
                            const CXCursor arg0 =
                                cg::clang::type_template_arg_declaration(
                                    expr,
                                    0);
                            const std::string key = record_key_of_decl(arg0);
                            if (!key.empty())
                            {
                                hint.type_keys.push_back(key);
                                with_source(
                                    callee == "make_unique"
                                        ? HintSource::MakeUnique
                                        : HintSource::MakeShared,
                                    Confidence::Medium);
                                return;
                            }
                        }
                        // 断链 1（值槽）：透传 getter 调用的值槽（auto r =
                        // p.Get() / return p.Get()）——返回值即壳实例槽的
                        // pointee，别名 到接收者槽（函数内传播拷贝），不把
                        // getter 模式的 #ret（pattern
                        // 字段别名，恒空）当传播边。
                        if (cg::clang::kind(ref) == CXCursor_CXXMethod)
                        {
                            const CXCursor obj =
                                passthrough_getter_object(expr);
                            if (!cg::clang::is_null(obj))
                            {
                                const ReceiverInfo ri = describe_receiver(obj);
                                if (!ri.name.empty() && ri.name != "this")
                                {
                                    hint.related_var = ri.owner_key.empty()
                                                           ? ri.name
                                                           : encode_field_slot(
                                                                 ri.owner_key,
                                                                 ri.name);
                                    hint.related_key = {};
                                    hint.source = HintSource::ReturnFlow;
                                    hint.confidence = Confidence::Medium;
                                    hint.fallback_reason = "getter_pointee";
                                    emit_hint(hint);
                                    return;
                                }
                                return; // 接收者不可寻址（隐式 this）：不发
                            }
                        }
                        // 跨 TU 的声明/定义：本 TU 只看得见声明，定义在另一个
                        // TU 里，因此 related_key 存延迟解析描述符（由
                        // propagate 在整库符号表上解析），而不是内嵌文件位置的
                        // unique_key。
                        const std::string callee_file =
                            norm(cg::clang::location(ref));
                        if (under_roots(callee_file, opt))
                        {
                            hint.related_key =
                                encode_method_ref(method_ref_of(ref));
                            hint.source = HintSource::ReturnFlow;
                            hint.confidence = Confidence::Medium;
                            hint.fallback_reason = std::string{
                                source_name(HintSource::ReturnFlow)};
                            emit_hint(hint);
                            return;
                        }
                    }
                    // 构造函数调用（C API 无 CXXConstructExpr 种类）或记录按值
                    // 返回：静态类型就是动态类型。
                    hint.type_keys = expr_type_keys(expr);
                    if (!hint.type_keys.empty()
                        && !cg::clang::is_pointer_or_reference(expr))
                    {
                        with_source(
                            HintSource::DirectAggregate,
                            Confidence::High);
                        return;
                    }
                    break;
                }
                default:
                    break;
                }
                // 无动态证据：不产线索（静态类型仍由 Declared 线索承载）。
            }

            /**
             * @brief
             * 声明的初始化表达式：最后一个表达式子（跳过类型/属性引用）。
             *
             * @internal local_var 的三处初始化消费（fn_target 种子 / 容器元素
             *           继承 / 拷贝流与 hint_from_expr）共用同一“倒序跳过
             *           TypeRef/TemplateRef/NamespaceRef/AnnotateAttr/
             *           OverloadCandidate，取首个表达式子”的骨架。
             */
            [[nodiscard]]
            std::optional<CXCursor> init_expr_of(const CXCursor& c)
            {
                const std::vector<CXCursor> kids = direct_children(c);
                for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                {
                    const CXCursorKind k = cg::clang::kind(*it);
                    if (k == CXCursor_TypeRef || k == CXCursor_TemplateRef
                        || k == CXCursor_NamespaceRef
                        || k == CXCursor_AnnotateAttr
                        || k == CXCursor_OverloadCandidate)
                    {
                        continue;
                    }
                    return *it;
                }
                return std::nullopt;
            }

            /**
             * @brief 容器下标取（形态 3）：auto h = c[k]; 的元素槽继承发射。
             *
             * @internal init 是界外 operator[] 调用且源容器元素槽已注册：局部
             *           函数指针槽继承容器元素槽（函数内引用传播，同容器拷贝流
             *           范式——目标集沿 related_var 流动）。
             */
            void emit_elem_index_read(
                const CXCursor&                  init,
                const Ctx&                       ctx,
                const std::string&               var,
                const cg::clang::SourceLocation& loc)
            {
                const CXCursor op = cg::clang::referenced(init);
                if (cg::clang::is_null(op)
                    || cg::clang::kind(op) != CXCursor_CXXMethod
                    || cg::clang::spelling(op) != "operator[]")
                {
                    return;
                }
                const std::vector<CXCursor> sk = direct_children(init);
                if (sk.empty())
                {
                    return;
                }
                // 源容器域解析——局部（函数域）与全局（variable|| 域）两查；
                // 全局源的 related 编码带域（@fld|`<域>`|`<名>`，decode
                // 按携带域 解析——跨函数 join 的锚）。
                const auto sref = slot_ref_of(unwrap_expr(sk.front()));
                if (!sref.has_value() || sref->var_name.empty())
                {
                    return;
                }
                const std::string  gk = resolve_slot_owner(sk.front());
                const std::string& src = sref->var_name;
                const std::string  domain =
                    !gk.empty() ? gk
                                : (!sref->owner_key.empty() ? sref->owner_key
                                                            : ctx.fn_key);
                const bool local = domain == ctx.fn_key;
                if (!elem_slot_registered(domain, src))
                {
                    return;
                }
                TypeHintRec hint = make_hint(ctx.fn_key, var, loc);
                hint.source = HintSource::ContainerElemFlow;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = "elem_index_read";
                hint.related_var = encode_container_elem_slot(
                    local ? std::string_view{src}
                          : std::string_view{
                                std::format("@fld|{}|{}", domain, src)});
                emit_hint(std::move(hint));
            }

            void local_var(const CXCursor& c, const Ctx& ctx)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                const std::string var = cg::clang::spelling(c);
                if (var.empty())
                {
                    return;
                }
                if (cross_fn_body_decl(c))
                {
                    return;
                }
                if (ctx.fn_key.empty())
                {
                    // 全局/命名空间级 VarDecl（TU 顶层 visit 无函数域）——
                    // 函数域路径（declared hint/def/取出继承）不适用，但全局
                    // 容器元素槽在此注册（跨函数注册表形态：reg 写/fire 读的
                    // 槽基础）。owner 用 variable||name|file（global_var_key_of
                    // 对局部形态返回空、此处必为全局）。
                    if (is_std_container(c))
                    {
                        const std::string gk = global_var_key_of(c);
                        const std::string ek = container_elem_key(c);
                        if (!gk.empty())
                        {
                            register_container_elem(
                                gk,
                                var,
                                ek,
                                loc,
                                ek.empty() && container_elem_is_callable(c));
                        }
                    }
                    // 全局 MPtr/函数指针变量初始化（gp = &C::m 形态）——
                    // 目标行落全局域自身槽（局部形态的 def 在下方 fn_target 分
                    // 支，全局无函数域走此对称路径），供字段/局部拷贝流经
                    // @gvar 编码跨域 join。
                    {
                        const std::string gk = global_var_key_of(c);
                        if (!gk.empty())
                        {
                            const std::vector<CXCursor> iks =
                                direct_children(c);
                            for (auto it = iks.rbegin(); it != iks.rend(); ++it)
                            {
                                const CXCursorKind ik = cg::clang::kind(*it);
                                if (ik == CXCursor_TypeRef
                                    || ik == CXCursor_TemplateRef)
                                {
                                    continue;
                                }
                                const std::string fn_target =
                                    function_target_key(*it);
                                if (!fn_target.empty())
                                {
                                    TypeHintRec hint = make_hint(gk, var, loc);
                                    hint.type_keys = {fn_target};
                                    hint.source = HintSource::AddressOfObject;
                                    hint.confidence = Confidence::Medium;
                                    hint.fallback_reason = "global_init";
                                    emit_hint(std::move(hint));
                                }
                                break;
                            }
                        }
                    }
                    return;
                }
                const std::string static_key = declared_type_key(c);
                // 局部壳变量（TPtr<T> box; 形态）追加 pointee 实参 key——
                // 界外壳自身 key 为空时由实参独自承载静态面（与
                // describe_receiver 的接收者剥壳同口径）。
                const std::string shell_pointee = shell_pointee_key(c);
                if (!static_key.empty() || !shell_pointee.empty())
                {
                    TypeHintRec hint = make_hint(ctx.fn_key, var, loc);
                    if (!static_key.empty())
                    {
                        hint.static_types.push_back(static_key);
                    }
                    if (!shell_pointee.empty())
                    {
                        hint.static_types.push_back(shell_pointee);
                    }
                    emit_hint(std::move(hint));
                }
                // def 事件（形态 1）：初始化即函数目标（&f / 裸函数名 /
                // lambda）——局部函数指针槽的直达证据（赋值形态在
                // assignment() 的 fn_target 分支，此处覆盖 `T (*p)() = &f;`
                // 的声明初始化同型）。source 复用 AddressOfObject（取址证据
                // 同族），fallback_reason 区分。
                if (!ctx.fn_key.empty())
                {
                    if (const auto init = init_expr_of(c))
                    {
                        const std::string fn_target = function_target_key(
                            identity_wrapper_operand(*init));
                        if (!fn_target.empty())
                        {
                            TypeHintRec hint = make_hint(ctx.fn_key, var, loc);
                            hint.type_keys = {fn_target};
                            hint.source = HintSource::AddressOfObject;
                            hint.confidence = Confidence::High;
                            hint.fallback_reason = "fn_seed_init";
                            emit_hint(std::move(hint));
                        }
                    }
                }
                // 声明期容器元素槽：std 容器 + 元素可解析才登记；元素
                // 类型只进 static_types（动态证据一律来自写集/拷贝流）；元素
                // 是函数指针/std::function 时无静态 key，gate_only 登记闸门
                // （目标集来自下标写入/写集的动态证据）。
                if (is_std_container(c))
                {
                    const std::string ek = container_elem_key(c);
                    register_container_elem(
                        ctx.fn_key,
                        var,
                        ek,
                        loc,
                        ek.empty() && container_elem_is_callable(c));
                }
                // 容器下标取（形态 3）：auto h = c[k]; ——判据与发射见
                // emit_elem_index_read。
                if (const auto init_raw = init_expr_of(c))
                {
                    const CXCursor init = unwrap_expr(*init_raw);
                    if (cg::clang::kind(init) == CXCursor_CallExpr)
                    {
                        emit_elem_index_read(init, ctx, var, loc);
                    }
                }
                // 初始化表达式：最后一个表达式子（跳过类型/属性引用，见
                // init_expr_of）。
                if (const auto init = init_expr_of(c))
                {
                    // 拷贝构造/auto
                    // 拷贝（容器元素局部拷贝流）：源是容器变量时，
                    // 局部元素槽继承源元素槽（函数内传播经 "#elem:"
                    // 引用拷贝）。
                    if (elem_slot_registered(ctx.fn_key, var))
                    {
                        CXCursor src_expr = *init;
                        // 拷贝构造形态（T dst = src）的 init 是构造调用包装：
                        // 裸引用判空时穿透其唯一非类型子（源容器引用）再判；
                        // 对 DeclRefExpr/初始化列表天然 no-op。
                        if (ref_var_name(src_expr).empty())
                        {
                            std::vector<CXCursor> real;
                            for (const CXCursor& ck : direct_children(src_expr))
                            {
                                if (!is_type_ref(ck))
                                {
                                    real.push_back(ck);
                                }
                            }
                            if (real.size() == 1)
                            {
                                src_expr = unwrap_expr(real.front());
                            }
                        }
                        const std::string src = ref_var_name(src_expr);
                        if (!src.empty() && call_result_key(src_expr).empty())
                        {
                            TypeHintRec hint = make_hint(
                                ctx.fn_key,
                                std::string{container_elem_slot_prefix} + var,
                                loc);
                            hint.source = HintSource::ContainerElemFlow;
                            hint.confidence = Confidence::Medium;
                            hint.fallback_reason = "container_elem_flow";
                            hint.related_var = encode_container_elem_slot(src);
                            emit_hint(std::move(hint));
                        }
                    }
                    hint_from_expr(
                        *init,
                        ctx.fn_key,
                        var,
                        file,
                        static_cast<int>(loc.line),
                        static_cast<int>(loc.column));
                }
            }

            void parameter(const CXCursor& c, Ctx& ctx)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt) || ctx.fn_key.empty())
                {
                    ++ctx.param_index;
                    return;
                }
                if (cross_fn_body_decl(c))
                {
                    return;
                }
                const std::string var = cg::clang::spelling(c);
                if (!var.empty())
                {
                    const std::string static_key = declared_type_key(c);
                    // 壳类模板形参（AcmePtr<T>/AcmeOutPtr<T> 出参壳同构
                    // 形态）的静态面追加 pointee 实参 key：槽静态面由此可
                    // 立证明界（壳主模板键由消费侧跳过），出参写回/扇投
                    // 的 may 并集不再经壳槽无界扩散。判据与 field_decl 同
                    // 源；界外壳由实参独自承载静态面。
                    const std::string shell_pointee = shell_pointee_key(c);
                    TypeHintRec       hint = make_hint(ctx.fn_key, var, loc);
                    if (!static_key.empty())
                    {
                        hint.static_types.push_back(static_key);
                    }
                    if (!shell_pointee.empty())
                    {
                        hint.static_types.push_back(shell_pointee);
                    }
                    hint.source = HintSource::ParamFlow;
                    hint.confidence = Confidence::Medium;
                    hint.fallback_reason = "param_flow_single";
                    hint.param_index = ctx.param_index;
                    emit_hint(std::move(hint));
                    // 形参声明行（Declared，静态面载体）：ParamFlow 行的
                    // static_types 落库时被丢弃（写侧只持久化 Declared 行
                    // 的静态面），补发一行与局部变量/字段同机制的声明行，
                    // 传播侧形参槽才立得起族边裁剪的静态面——无面的形参
                    // 槽对 may 族并集不设防，唯一决议被并集稀释（0.7 降
                    // 0.4）、真实现类型与污染类型同批进出。两行以 source
                    // 区分（UNIQUE 键含 source），互不覆写。
                    if (!static_key.empty() || !shell_pointee.empty())
                    {
                        TypeHintRec decl = make_hint(ctx.fn_key, var, loc);
                        if (!static_key.empty())
                        {
                            decl.static_types.push_back(static_key);
                        }
                        if (!shell_pointee.empty())
                        {
                            decl.static_types.push_back(shell_pointee);
                        }
                        emit_hint(std::move(decl));
                    }
                    // 容器形参的元素槽：source 用 Declared（静态面载体；
                    // param_index 恒 -1——"#elem:" 槽不做 #p{i} 实参寻址），
                    // 跨函数的元素动态证据不在本层范围。
                    if (is_std_container(c))
                    {
                        register_container_elem(
                            ctx.fn_key,
                            var,
                            container_elem_key(c),
                            loc);
                    }
                }
                ++ctx.param_index;
            }

            void return_stmt(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                hint_from_expr(
                    kids.front(),
                    ctx.fn_key,
                    std::string{return_slot},
                    norm(loc),
                    static_cast<int>(loc.line),
                    static_cast<int>(loc.column));
            }

            /** @brief 调用是否经由成员指针（`(p->*pmf)()` / `(o.*pmf)()`）。 */
            [[nodiscard]]
            static auto is_member_pointer_call(const CXCursor& call) -> bool
            {
                const std::vector<CXCursor> kids = direct_children(call);
                if (kids.empty())
                {
                    return false;
                }
                const CXCursor callee = unwrap_expr(kids.front());
                if (cg::clang::kind(callee) != CXCursor_BinaryOperator)
                {
                    return false;
                }
                for (const std::string& tok :
                     cg::clang::token_spellings(callee))
                {
                    if (tok == "->*" || tok == ".*")
                    {
                        return true;
                    }
                }
                return false;
            }

            /**
             * @brief 成员调用的接收者类型是否为裸模板形参（dependent）。
             *
             * @internal 定义：被调用表达式为 MemberRefExpr，且其基表达式的
             * canonical 类型剥掉 cv 限定与指针/引用修饰后整串就是
             * "type-parameter-`<d>`-`<i>`"（裸形参：T / T* / const
             * T&），或拼写为
             * "<dependent type>"（canonical 无从展开的依赖表达式）。
             * soundness 边界：裸形参接收者的成员集未知，成员名查找不可信——
             * libclang 对这类 dependent 成员形态的 referenced 可能幻觉解析到
             * TU 内任意同名成员（实测案例：某模板成员函数
             * body 的 t.error_code 被连到 sol2 的 dump_error::error_code，
             * 污染 down 方向影响面）。成型类模板（W`<T>`&、std::vector`<T>`&、
             * this 的 W`<T>`*）成员集已知、解析可靠，不在拦截面（判据必须是
             * 裸形参，contains 型判定会误杀模板类的 this-> 与类对象调用）。
             * 判据只看基表达式类型、不看 referenced，引擎间行为一致。
             * fallback 清单（漏收方向）：模板体内裸形参接收者的真实调用不建
             * 边——真实边由各实例化点的调用（接收者类型具体）承载；限定名
             * 依赖调用（UnresolvedLookupExpr 形态）不在本守卫内，维持既有
             * indirect 降级。
             */
            [[nodiscard]]
            static auto receiver_type_is_dependent(const CXCursor& call) -> bool
            {
                const std::vector<CXCursor> kids = direct_children(call);
                if (kids.empty())
                {
                    return false;
                }
                const CXCursor callee = unwrap_expr(kids.front());
                if (cg::clang::kind(callee) != CXCursor_MemberRefExpr)
                {
                    return false;
                }
                const std::vector<CXCursor> member_kids =
                    direct_children(callee);
                if (member_kids.empty())
                {
                    return false; // 隐式 this：本类 pattern 方法可正常解析
                }
                const CXCursor base = member_kids.front();
                if (const auto core =
                        cg::clang::canonical_pointee_spelling(base, 0))
                {
                    std::string_view sp = *core;
                    if (sp == "<dependent type>")
                    {
                        return true; // canonical 无从展开的依赖表达式
                    }
                    for (std::string_view qualifier : {"const ", "volatile "})
                    {
                        while (sp.starts_with(qualifier))
                        {
                            sp.remove_prefix(qualifier.size());
                        }
                    }
                    while (!sp.empty()
                           && (sp.back() == '*' || sp.back() == '&'
                               || sp.back() == ' '))
                    {
                        sp.remove_suffix(1);
                    }
                    return sp.starts_with("type-parameter-");
                }
                return cg::clang::type_spelling(base) == "<dependent type>";
            }

            /**
             * @brief 本游标是否是真正的运算符调用（`v[0]` / `fn(1)`）。
             *
             * @internal libclang 不暴露独立的 CXXOperatorCallExpr kind，它与
             * 函数指针调用同为 CXCursor_CallExpr。真正运算符调用的直接子游标
             * 里含有指向同一 operator 成员的 DeclRefExpr（对象表达式之后）；
             * 而被调用表达式是内层调用时（`a[0]()`），直接子游标是那个内层
             * CallExpr——不能仅凭传播到外层的 operator 名字判定外层是运算符
             * 调用。
             */
            [[nodiscard]]
            static auto is_operator_invocation(
                const CXCursor& call,
                const CXCursor& ref) -> bool
            {
                if (cg::clang::is_null(ref)
                    || cg::clang::kind(ref) != CXCursor_CXXMethod)
                {
                    return false;
                }
                const std::string name = cg::clang::spelling(ref);
                if (!name.starts_with("operator"))
                {
                    return false;
                }
                // operator() 是函数对象调用（std::function/lambda/仿函数
                // 的调用形态），恰是用户间接调用——不属"运算符不是间接调用"
                // 的拦截面，放行给 indirect 路径（槽锚点提取在
                // indirect_call）。
                if (name == "operator()")
                {
                    return false;
                }
                bool hit = false;
                for_each_child(
                    call,
                    [&](const CXCursor& kid)
                    {
                        if (hit)
                        {
                            return; // 已命中（找到即停）
                        }
                        const CXCursor inner = unwrap_expr(kid);
                        if (cg::clang::kind(inner) != CXCursor_DeclRefExpr)
                        {
                            return;
                        }
                        const CXCursor operator_ref =
                            cg::clang::referenced(inner);
                        if (!cg::clang::is_null(operator_ref)
                            && cg::clang::kind(operator_ref)
                                   == CXCursor_CXXMethod
                            && cg::clang::spelling(operator_ref) == name)
                        {
                            hit = true;
                        }
                    });
                return hit;
            }

            /**
             * @brief 被调用表达式本身是一次调用（`a[0]()` / `get()()`）。
             *
             * @internal libclang 把被调用表达式（operator[] 的
             * CXXOperatorCallExpr 或普通嵌套 CallExpr）放在直接子游标首位。
             * 外层调用的目标由该表达式的求值结果（函数指针/可调用对象）
             * 决定，不能把内层被调用者当成外层的直接 callee。
             */

            /**
             * @brief operator= 调用形态的函数目标写入（件一）。
             *
             * @internal `h.on_key_empty = &f` 在 std::function 字段上是
             * operator= 调用（非裸 BinaryOperator 赋值），此前被
             * is_operator_invocation 拦截面整体丢弃（def 侧缺口）。此处补
             * def：被写对象（首子，剥壳）经 field_target_of 提取字段域，值
             * （最后一个非类型子，剥壳）经 function_target_key 提取函数目标
             * （取址/裸名/lambda 同判），落库形态与裸字段赋值（assignment()
             * 的 FieldFlow）逐字段一致。拷贝语义（dst=src 的槽继承）不在此
             * （container_copy_assign 处理）；RHS 非函数目标（对象拷贝等）
             * 不发——宁漏勿猜。
             */
            /**
             * @brief 断链 2：壳槽写入 RHS 的 pointee 证据解析（ctor 初始化列表
             *        与 operator= 调用两条 AST 路径共用）。
             *
             * @internal RHS 归一到槽别名或待递归表达式：花括号/裸壳构造暴露为
             * 壳构造调用——T* 位实参（shell_pointee_argument）是 pointee 直接证
             * 据，无 T* 位（壳 move/拷贝构造）时恰一实参即源壳（壳到壳搬运：
             * 源槽类型集已是 pointee
             * 语义，与构造穿透同惯例）；std::move/forward 剥壳；
             * DeclRef/MemberRef 落槽别名（裸名由调用方 @fnv 编码携带写函数域，
             * @fld/@gvar 自带域）；其余（new 等）交 hint_from_expr 递归保级。
             * 返回 (别名或空串, 别名空时的 hint_from_expr 输入)。
             */
            [[nodiscard]]
            auto shell_store_rhs(const CXCursor& init, const Ctx& ctx)
                -> std::pair<std::string, CXCursor>
            {
                CXCursor rhs = unwrap_expr(strip_casts(init));
                if (cg::clang::kind(rhs) == CXCursor_InitListExpr)
                {
                    std::vector<CXCursor> real;
                    for (const CXCursor& ck : direct_children(rhs))
                    {
                        if (!is_type_ref(ck))
                        {
                            real.push_back(ck);
                        }
                    }
                    if (real.size() == 1)
                    {
                        rhs = unwrap_expr(strip_casts(real.front()));
                    }
                }
                if (cg::clang::kind(rhs) == CXCursor_CallExpr)
                {
                    const CXCursor rr = cg::clang::referenced(rhs);
                    if (!cg::clang::is_null(rr)
                        && cg::clang::kind(rr) == CXCursor_Constructor)
                    {
                        const CXCursor pointee =
                            shell_pointee_argument(rhs, rr);
                        if (!cg::clang::is_null(pointee))
                        {
                            rhs = unwrap_expr(strip_casts(pointee));
                        }
                        else
                        {
                            // 壳 move/拷贝构造（f_{std::move(s)} 暴露为壳构造调
                            // 用）：恰一实参即源壳，槽到槽搬运。
                            const std::vector<CXCursor> args =
                                ctor_arguments(rhs, rr);
                            std::vector<CXCursor> real;
                            for (const CXCursor& a : args)
                            {
                                if (!is_type_ref(a))
                                {
                                    real.push_back(a);
                                }
                            }
                            if (real.size() != 1)
                            {
                                return {{}, CXCursor{}}; // 零/多实参：不猜
                            }
                            rhs = unwrap_expr(strip_casts(real.front()));
                        }
                    }
                    else if (
                        !cg::clang::is_null(rr)
                        && cg::clang::kind(rr) == CXCursor_CXXMethod
                        && cg::clang::is_static_method(rr))
                    {
                        // 断链 5（写侧）：静态方法壳工厂（`p = 类::Attach(arg)`
                        // 与 ctor-init `f_{类::Attach(arg)}`）——T* 位实参即
                        // pointee 证据，折叠成实参表达式走下方既有归一（别名或
                        // hint_from_expr 保级）；判据不过时 RHS 保持工厂调用原
                        // 样：ref_var_name 取不到名字，hint_from_expr 侧形状闸
                        // 再拦（双闸收敛到不发）。
                        const CXCursor pointee =
                            shell_factory_pointee_argument(rhs, rr);
                        if (!cg::clang::is_null(pointee))
                        {
                            rhs = unwrap_expr(strip_casts(pointee));
                        }
                    }
                }
                rhs = unwrap_expr(strip_casts(identity_wrapper_operand(rhs)));
                const std::string alias = ref_var_name(rhs);
                if (!alias.empty())
                {
                    if (alias.find('|') == std::string::npos)
                    {
                        // 裸名是写函数域的形参/局部——@fnv 编码携带域。
                        return {
                            encode_fn_var_slot(ctx.fn_key, alias),
                            CXCursor{}};
                    }
                    return {alias, CXCursor{}};
                }
                return {{}, rhs};
            }

            /** @brief 壳类型闸：表达式/声明的类型记录 pattern 是指针式壳。 */
            [[nodiscard]]
            auto typed_pointer_shell(const CXCursor& c) -> bool
            {
                const CXCursor inst = cg::clang::type_declaration(c);
                if (cg::clang::is_null(inst) || is_invalid_cursor(inst))
                {
                    return false;
                }
                const CXCursor pattern = cg::clang::specialized_template(inst);
                if (cg::clang::is_null(pattern) || is_invalid_cursor(pattern)
                    || cg::clang::kind(pattern) != CXCursor_ClassTemplate)
                {
                    return false;
                }
                return is_pointer_shell_cached(pattern);
            }

            /**
             * @brief 断链 2a：ctor
             * 初始化列表的成员字段写侧穿透（壳/普通分流）。
             *
             * @internal Holder(IFace* p) : f_{p} / f_{std::move(s)}——字段静态类
             * 型命中指针式壳（typed_pointer_shell，与构造穿透同一判据）时，RHS
             * 的 pointee 证据落 FieldFlow 行（槽类型集语义 =
             * pointee）；字段非壳走
             * 普通字段臂（ctor_plain_field_init，槽类型集语义 =
             * 值本身）。libclang 把初始化列表暴露为"引用字段的 MemberRef 游标 +
             * 同级后随初始化表达 式"（实测：引用/const
             * 指针/裸指针/花括号/遮蔽同名同形态，值字
             * 段空圆括号实体化为默认构造调用），配对在 function() 的构造函数子
             * 循环里完成；基类/委托初始化是 TypeRef 形态天然不入本臂。
             */
            void ctor_shell_field_init(
                const CXCursor& field,
                const CXCursor& init,
                const Ctx&      ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const std::string owner =
                    record_key_of_decl(cg::clang::semantic_parent(field));
                const std::string var = cg::clang::spelling(field);
                if (owner.empty() || var.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(init);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                if (!typed_pointer_shell(field))
                {
                    ctor_plain_field_init(owner, var, loc, file, init, ctx);
                    return;
                }
                const auto [alias, hint_expr] = shell_store_rhs(init, ctx);
                if (!alias.empty())
                {
                    TypeHintRec hint = make_hint(owner, var, loc);
                    hint.source = HintSource::FieldFlow;
                    hint.confidence = Confidence::Medium;
                    hint.fallback_reason = "ctor_init_flow";
                    hint.related_var = alias;
                    emit_hint(std::move(hint));
                    return;
                }
                if (!cg::clang::is_null(hint_expr))
                {
                    hint_from_expr(
                        hint_expr,
                        owner,
                        var,
                        file,
                        static_cast<int>(loc.line),
                        static_cast<int>(loc.column));
                }
            }

            /**
             * @brief ctor 初始化列表普通字段（非壳）的写侧采集臂。
             *
             * @internal 与函数体字段写（assignment）逐字段同语义：RHS 三件套
             * （expr_type_keys / ref_var_name / call_result_key），类型键非空时
             * 只发类型证据，否则发传播边；related_var 裸名以 @fnv 编码携带构造
             * 函数域（字段槽 owner 是记录，裸名在记录域找不到形参/局部行），全
             * 局变量源镜像赋值侧改 @gvar 编码，字段源 @fld 描述符原样。花
             * 括号单元素剥一层与圆括号同路（实测同形态）；数组/多元素花括
             * 号、字面量、取址下标等不透 RHS 三通道全空不发（宁漏勿猜）；多实
             * 参值字段以整个构造调用表达式为 RHS，不按参拆分；同名遮蔽 RHS 的
             * 引用按 clang_getCursorReferenced 解析到 ParmDecl（按拼写配对会把
             * 字段自身错当源，探针实测），lambda 域遮蔽由 slot_ref_of 的跨域闸
             * 拦截。类内默认初始化器（NSDMI）是 FieldDecl 子表达式形态，不经
             * 本臂（范围锁定，探针已证形态）。
             */
            void ctor_plain_field_init(
                const std::string&               owner,
                const std::string&               var,
                const cg::clang::SourceLocation& loc,
                const std::string&               file,
                const CXCursor&                  init,
                const Ctx&                       ctx)
            {
                CXCursor rhs = unwrap_expr(strip_casts(init));
                if (cg::clang::kind(rhs) == CXCursor_InitListExpr)
                {
                    std::vector<CXCursor> real;
                    for (const CXCursor& ck : direct_children(rhs))
                    {
                        if (!is_type_ref(ck))
                        {
                            real.push_back(ck);
                        }
                    }
                    if (real.size() == 1)
                    {
                        rhs = unwrap_expr(strip_casts(real.front()));
                    }
                }
                const std::vector<std::string> keys = expr_type_keys(rhs);
                std::string                    related_var;
                const std::string              alias = ref_var_name(rhs);
                if (!alias.empty())
                {
                    if (alias.find('|') != std::string::npos)
                    {
                        related_var = alias; // @fld 等已带域编码原样
                    }
                    else
                    {
                        // 同款：全局变量源 @gvar（variable||name|file 域）；
                        // 其余裸名是构造函数域形参/局部——@fnv 携带写函数域。
                        const CXCursor rhs_ref = unwrap_expr(strip_casts(rhs));
                        const std::string gk =
                            cg::clang::kind(rhs_ref) == CXCursor_DeclRefExpr
                                ? resolve_slot_owner(rhs_ref)
                                : std::string{};
                        related_var =
                            !gk.empty() ? encode_global_slot(gk, alias)
                                        : encode_fn_var_slot(ctx.fn_key, alias);
                    }
                }
                const std::string related_key = call_result_key(rhs);
                if (keys.empty() && related_var.empty() && related_key.empty())
                {
                    return;
                }
                TypeHintRec hint = make_hint(owner, var, loc);
                hint.type_keys = keys;
                hint.source = HintSource::FieldFlow;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = "ctor_init_flow";
                if (keys.empty())
                {
                    hint.related_var = related_var;
                    hint.related_key = related_key;
                }
                emit_hint(std::move(hint));
            }

            /**
             * @brief operator= 被写槽载体的 (owner, var) 解析。
             *
             * @internal LHS 两种槽载体形态：字段（owner = 字段所属记录的
             * key，槽由字段流按记录域查找消费）与局部/形参变量（owner =
             * 写函数 key）。其余（数组下标、非字段成员等）返回空对。
             */
            [[nodiscard]]
            auto assign_slot_target(const CXCursor& lhs, const Ctx& ctx)
                -> std::pair<std::string, std::string>
            {
                const CXCursor lref = cg::clang::referenced(lhs);
                if (cg::clang::kind(lhs) == CXCursor_MemberRefExpr)
                {
                    if (cg::clang::is_null(lref)
                        || cg::clang::kind(lref) != CXCursor_FieldDecl)
                    {
                        return {};
                    }
                    return {
                        record_key_of_decl(cg::clang::semantic_parent(lref)),
                        cg::clang::spelling(lhs)};
                }
                if (cg::clang::is_null(lref)
                    || (cg::clang::kind(lref) != CXCursor_VarDecl
                        && cg::clang::kind(lref) != CXCursor_ParmDecl))
                {
                    return {};
                }
                return {ctx.fn_key, cg::clang::spelling(lref)};
            }

            /**
             * @brief 断链 2b：operator= 调用形态的壳槽写侧穿透。
             *
             * @internal p_impl_ = {p} / p_impl_ = std::move(p_impl) 在 AST 是
             * operator= 调用（非 BinaryOperator，assignment() 天然不见），此前
             * 整层无采集。LHS 是槽载体（字段/局部变量）且静态类型命中指针式壳
             * 时 RHS 的 pointee 证据落槽；LHS 非壳（宿主类整体赋值，如
             * Holder::operator=(T*)）不采——字段写入由运算符体内的同形态穿透覆
             * 盖（体在另一 TU 亦然，@fnv 跨域别名 + 实参→形参传播天然跨 TU）。
             */
            void operator_assign_shell_store(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const CXCursor ref = cg::clang::referenced(c);
                if (cg::clang::is_null(ref)
                    || cg::clang::spelling(ref) != "operator=")
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.size() < 2)
                {
                    return;
                }
                const CXCursor     lhs = unwrap_expr(strip_casts(kids.front()));
                const CXCursorKind lk = cg::clang::kind(lhs);
                if ((lk != CXCursor_MemberRefExpr && lk != CXCursor_DeclRefExpr)
                    || !typed_pointer_shell(lhs))
                {
                    return;
                }
                const auto [owner, var] = assign_slot_target(lhs, ctx);
                if (owner.empty() || var.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                const bool is_field = lk == CXCursor_MemberRefExpr;
                const auto [alias, hint_expr] =
                    shell_store_rhs(kids.back(), ctx);
                if (!alias.empty())
                {
                    TypeHintRec hint = make_hint(owner, var, loc);
                    hint.source = is_field ? HintSource::FieldFlow
                                           : HintSource::LocalCopy;
                    hint.confidence = Confidence::Medium;
                    hint.fallback_reason =
                        is_field ? "field_flow" : "local_assignment_single";
                    hint.related_var = alias;
                    emit_hint(std::move(hint));
                    return;
                }
                if (!cg::clang::is_null(hint_expr))
                {
                    hint_from_expr(
                        hint_expr,
                        owner,
                        var,
                        file,
                        static_cast<int>(loc.line),
                        static_cast<int>(loc.column));
                }
            }

            /**
             * @brief 函数体 operator= 的工厂赋值臂：模板特化槽载体被标准
             *        工厂（make_unique/make_shared）赋值时，模板实参即
             *        pointee 证据落槽。
             *
             * @internal 标准库智能指针的存储面（MSVC 经 compressed_pair
             * 嵌套、libc++ 经匿名 struct 展开）不满足指针式壳的直属 T*
             * 字段判据，成员函数体内 `p_ = std::make_unique<T>()` 是
             * operator= 调用形态（裸 BinaryOperator 赋值臂天然不见），
             * 此前整层无采集——函数体字段工厂赋值的类型证据全部落空
             * （夹具 SetUp 形态的成片 fallback）。判据纯结构双闸：LHS
             * 槽载体（字段/局部/形参）静态类型是类模板特化；RHS 被调是
             * make_unique/make_shared 且第 0 模板实参可解为记录。赋值
             * 两端类型兼容由语言保证（工厂返回的同族壳是唯一可赋来源），
             * 槽类型集语义 = pointee（与构造穿透同惯例）。LHS 命中指针
             * 式壳判据时本臂跳过（壳穿透臂已覆盖同形态，避免重复行）；
             * 双闸任一不满足不发（宁漏勿猜）。
             */
            void operator_assign_factory_store(
                const CXCursor& c,
                const Ctx&      ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const CXCursor ref = cg::clang::referenced(c);
                if (cg::clang::is_null(ref)
                    || cg::clang::spelling(ref) != "operator=")
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.size() < 2)
                {
                    return;
                }
                const CXCursor     lhs = unwrap_expr(strip_casts(kids.front()));
                const CXCursorKind lk = cg::clang::kind(lhs);
                if (lk != CXCursor_MemberRefExpr && lk != CXCursor_DeclRefExpr)
                {
                    return;
                }
                if (typed_pointer_shell(lhs))
                {
                    return; // 壳穿透臂已覆盖，避免重复行
                }
                const CXCursor inst = cg::clang::type_declaration(lhs);
                if (cg::clang::is_null(inst) || is_invalid_cursor(inst))
                {
                    return;
                }
                const CXCursor pattern = cg::clang::specialized_template(inst);
                if (cg::clang::is_null(pattern) || is_invalid_cursor(pattern)
                    || cg::clang::kind(pattern) != CXCursor_ClassTemplate)
                {
                    return;
                }
                const CXCursor rhs = unwrap_expr(kids.back());
                const CXCursor callee = referenced_callable(rhs);
                if (cg::clang::is_null(callee)
                    || cg::clang::kind(callee) == CXCursor_Constructor)
                {
                    return;
                }
                const std::string factory = cg::clang::spelling(callee);
                if (factory != "make_unique" && factory != "make_shared")
                {
                    return;
                }
                const CXCursor arg0 =
                    cg::clang::type_template_arg_declaration(rhs, 0);
                const std::string key = record_key_of_decl(arg0);
                if (key.empty())
                {
                    return;
                }
                const auto [owner, var] = assign_slot_target(lhs, ctx);
                if (owner.empty() || var.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                TypeHintRec hint = make_hint(owner, var, loc);
                hint.type_keys.assign(1, key);
                hint.source = factory == "make_unique" ? HintSource::MakeUnique
                                                       : HintSource::MakeShared;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = std::string{source_name(hint.source)};
                emit_hint(std::move(hint));
            }

            void operator_assign_fn_target(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const CXCursor ref = cg::clang::referenced(c);
                if (cg::clang::is_null(ref)
                    || cg::clang::spelling(ref) != "operator=")
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.size() < 2)
                {
                    return;
                }
                const std::string fn_target =
                    function_target_key(unwrap_expr(kids.back()));
                if (fn_target.empty())
                {
                    return;
                }
                const FieldTarget field =
                    field_target_of(unwrap_expr(kids.front()));
                if (field.name.empty() || field.owner_key.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                TypeHintRec hint = make_hint(field.owner_key, field.name, loc);
                hint.type_keys = {fn_target};
                hint.source = HintSource::FieldFlow;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = "field_flow";
                emit_hint(std::move(hint));
            }

            /**
             * @brief 界外仿函数/CPO 对象调用桥（推倒重做）。
             *
             * @internal 实测：`my_inc(work)` 的 referenced
             * 直接给 operator() 的实例化特化（带 USR，定义体随 include 可见）
             * ——host 无需构造，此前"记录内按实参数找重载 + referenced=
             * VarDecl 触发"整环绕远且永不命中（仿函数调用形态 referenced 恒为
             * operator() 方法）。界内仿函数（operator() 语义父在根内）走具名
             * 路径（槽/def 事件），此处跳过。实参 = 调用子序列剔除：callee 子
             * （引用==op）、CPO 对象子（引用是 VarDecl 且其类型声明界外）、
             * 类型引用子。
             */
            void cpo_operator_bridge(
                const CXCursor& call,
                const CXCursor& op,
                const Ctx&      ctx)
            {
                const CXCursor rec = cg::clang::semantic_parent(op);
                if (cg::clang::is_null(rec)
                    || !is_record_kind(cg::clang::kind(rec)))
                {
                    return;
                }
                const std::string rec_file = norm(cg::clang::location(rec));
                if (under_roots(rec_file, opt))
                {
                    return; // 界内仿函数：具名路径（槽/def 事件）处理
                }
                const std::string host_usr = cg::clang::usr(op);
                if (host_usr.empty())
                {
                    return;
                }
                external_instance_body(op, ctx.fn_key);
                const cg::clang::SourceLocation site =
                    cg::clang::location(call);
                const std::string site_file = norm(site);
                int               idx = 0;
                bool              receiver_seen = false;
                for_each_child(
                    call,
                    [&](const CXCursor& kid)
                    {
                        if (is_type_ref(kid))
                        {
                            return;
                        }
                        const CXCursor r =
                            cg::clang::referenced(unwrap_expr(kid));
                        if (!cg::clang::is_null(r)
                            && cg::clang::equal_cursors(r, op))
                        {
                            return; // callee 子（operator() 声明引用）
                        }
                        if (!receiver_seen)
                        {
                            // 首个非 callee 子是接收者位（CXXOperatorCallExpr
                            // 的 隐式对象表达式）：CPO 对象本身（或
                            // this）——不计
                            // 实参。实测（stdexec 语料）：stdexec::then 的
                            // begin 实参也是界外类型变量，不能按"类型界外"泛化
                            // 跳过，只跳位置。
                            receiver_seen = true;
                            return;
                        }
                        const std::string target =
                            function_target_key(unwrap_expr(kid));
                        if (!target.empty())
                        {
                            out.external_param_args.push_back(
                                {.host_usr = host_usr,
                                 .param_index = idx,
                                 .caller_key = ctx.fn_key,
                                 .target_key = target,
                                 .file = site_file,
                                 .line = static_cast<int>(site.line),
                                 .col = static_cast<int>(site.column)});
                        }
                        ++idx;
                    });
            }
            [[nodiscard]]
            static auto callee_is_call(const CXCursor& call) -> bool
            {
                const std::vector<CXCursor> kids = direct_children(call);
                if (kids.empty())
                {
                    return false;
                }
                return cg::clang::kind(unwrap_expr(kids.front()))
                       == CXCursor_CallExpr;
            }

            /**
             * @brief 件三（逃逸集·读侧）：被调表达式剥转型后是裸下标取元。
             *
             * @internal ((IFn)slot[0])(1) 形态——锚点 = 下标基名（全局域优
             *           先），与写侧 escaped_store 同锚点 join；域经
             *           receiver_owner_key 携带。
             */
            void escaped_read_anchor(
                CallSiteRec&    rec,
                const CXCursor& first_kid)
            {
                const CXCursor bare = strip_casts(first_kid);
                if (cg::clang::kind(bare) != CXCursor_ArraySubscriptExpr)
                {
                    return;
                }
                const std::vector<CXCursor> bk = direct_children(bare);
                if (bk.empty())
                {
                    return;
                }
                const CXCursor    base = unwrap_expr(bk.front());
                const std::string nm = ref_var_name(base);
                if (nm.empty())
                {
                    return;
                }
                rec.callee_expr = nm;
                const std::string gk_esc = resolve_slot_owner(base);
                if (!gk_esc.empty())
                {
                    rec.receiver_owner_key = gk_esc;
                }
            }

            /**
             * @brief 容器下标直接调用（形态 3）：handlers[k]()。
             *
             * @internal 被调表达式是界外 operator[] 调用，callee_expr 取
             *           "#elem:<容器变量名>"（消费侧按该键查容器元素槽的目标
             *           集）。全局容器跨函数域——写侧（声明注册/动态写入）
             *           落在 variable||name|file 全局域，读侧此前只查函数域导
             *           致 join 落空。域序列化：全局域优先、函数域兜底；全局
             *           域命中时 owner 记入 receiver_owner_key（消费侧 owner 选
             *           择优先它）。
             * @return 被调表达式是否为 operator[] 调用形态——形态命中即发射
             *         站点行并终结调用方处理。
             */
            bool try_container_elem_call(
                CallSiteRec&       rec,
                const CXCursor&    callee,
                const std::string& fn_key)
            {
                if (cg::clang::kind(callee) != CXCursor_CallExpr)
                {
                    return false;
                }
                const CXCursor op = cg::clang::referenced(callee);
                if (cg::clang::is_null(op)
                    || cg::clang::kind(op) != CXCursor_CXXMethod
                    || cg::clang::spelling(op) != "operator[]")
                {
                    return false;
                }
                const std::vector<CXCursor> sk = direct_children(callee);
                if (!sk.empty())
                {
                    const CXCursor    src_expr = unwrap_expr(sk.front());
                    const std::string src = ref_var_name(src_expr);
                    if (!src.empty())
                    {
                        const std::string gk = resolve_slot_owner(src_expr);
                        if (!gk.empty() && elem_slot_registered(gk, src))
                        {
                            rec.callee_expr =
                                std::string{container_elem_slot_prefix} + src;
                            rec.receiver_owner_key = gk;
                        }
                        else if (elem_slot_registered(fn_key, src))
                        {
                            rec.callee_expr =
                                std::string{container_elem_slot_prefix} + src;
                        }
                    }
                }
                out.call_sites.push_back(std::move(rec));
                return true;
            }

            /**
             * @brief 成员函数指针调用：(obj.*pmf)() / (this->*pmf)()。
             *
             * @internal 被调表达式经 ParenExpr 剥到 BO_PtrMemD/I，槽锚点 = 右
             *           操作数（成员引用=字段槽 / DeclRef=局部变量）。
             */
            void resolve_ptr_member_call(
                CallSiteRec&    rec,
                const CXCursor& callee)
            {
                if (cg::clang::kind(callee) != CXCursor_BinaryOperator)
                {
                    return;
                }
                const std::vector<std::string> ptoks =
                    cg::clang::token_spellings(callee);
                const bool ptr_mem =
                    std::ranges::find(ptoks, "->*") != ptoks.end()
                    || std::ranges::find(ptoks, ".*") != ptoks.end();
                if (!ptr_mem)
                {
                    return;
                }
                const std::vector<CXCursor> bk = direct_children(callee);
                if (bk.size() < 2)
                {
                    return;
                }
                const CXCursor member = unwrap_expr(bk.back());
                if (cg::clang::kind(member) == CXCursor_MemberRefExpr)
                {
                    rec.callee_expr = cg::clang::spelling(member);
                    const CXCursor field = cg::clang::referenced(member);
                    if (!cg::clang::is_null(field)
                        && cg::clang::kind(field) == CXCursor_FieldDecl)
                    {
                        rec.receiver_owner_key = record_key_of_decl(
                            cg::clang::semantic_parent(field));
                    }
                }
                else if (cg::clang::kind(member) == CXCursor_DeclRefExpr)
                {
                    rec.callee_expr = cg::clang::spelling(member);
                }
            }

            /**
             * @brief
             * 有名被调表达式（字段函数指针/全局函数指针变量）的锚点解析。
             *
             * @internal MemberRefExpr：槽消费需要字段所属记录（Task）作
             *           owner——与 describe_receiver 的 MemberRefExpr 分支同一
             *           来源（FieldDecl 语义父）；被调表达式是字段而非形参，
             *           callee 清空防下方 DeclRef 形参判定误触发。DeclRefExpr：
             *           被调是全局函数指针/MPtr 变量时槽在
             *           variable||name|file 全局域，owner 记入
             *           receiver_owner_key（消费侧优先它）；局部变量维持函数域
             *           （caller_key）。
             */
            void resolve_named_callee(CallSiteRec& rec, CXCursor& callee)
            {
                if (cg::clang::kind(callee) == CXCursor_MemberRefExpr)
                {
                    rec.callee_expr = cg::clang::spelling(callee);
                    const CXCursor field = cg::clang::referenced(callee);
                    if (!cg::clang::is_null(field)
                        && cg::clang::kind(field) == CXCursor_FieldDecl)
                    {
                        rec.receiver_owner_key = record_key_of_decl(
                            cg::clang::semantic_parent(field));
                    }
                    const std::vector<CXCursor> mk = direct_children(callee);
                    if (!mk.empty())
                    {
                        callee = unwrap_expr(mk.front());
                    }
                    callee = CXCursor{};
                }
                else if (cg::clang::kind(callee) == CXCursor_DeclRefExpr)
                {
                    rec.callee_expr = cg::clang::spelling(callee);
                    const std::string gk_callee = resolve_slot_owner(callee);
                    if (!gk_callee.empty())
                    {
                        rec.receiver_owner_key = gk_callee;
                    }
                }
            }

            /**
             * @brief 被调是本函数形参时的形参序号（callee_param 槽锚点）。
             */
            void resolve_callee_param(
                CallSiteRec&    rec,
                const CXCursor& callee,
                const CXCursor& fn_decl)
            {
                if (cg::clang::kind(callee) != CXCursor_DeclRefExpr
                    || cg::clang::is_null(fn_decl))
                {
                    return;
                }
                const CXCursor ref = cg::clang::referenced(callee);
                if (cg::clang::kind(ref) != CXCursor_ParmDecl)
                {
                    return;
                }
                // 找到即停的守卫省略：equal_cursors 对同一 ParmDecl 只命中一次
                // （重复扫描只多付一次比较，无行为差异）。
                int ordinal = 0;
                for_each_child(
                    fn_decl,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch) == CXCursor_ParmDecl)
                        {
                            if (cg::clang::equal_cursors(ch, ref))
                            {
                                rec.callee_param = ordinal;
                                return;
                            }
                            ++ordinal;
                        }
                    });
            }

            /**
             * @brief 落一条间接调用行（函数指针/成员指针/依赖被调用者）。
             *
             * @internal 验收要求这些场景不得静默：没有可解析的家族，也要让
             * call_sites 留下 `indirect_call` 降级原因。
             */
            void indirect_call(
                const CXCursor&                  call,
                const Ctx&                       ctx,
                const std::string&               file,
                const cg::clang::SourceLocation& site)
            {
                CallSiteRec rec;
                rec.caller_key = ctx.fn_key;
                rec.indirect = true;
                rec.is_virtual = false;
                rec.qualified = false;
                rec.file = file;
                rec.line = static_cast<int>(site.line);
                rec.col = static_cast<int>(site.column);
                // 被调用表达式名与形参判定（消费直配的槽锚点）。
                const std::vector<CXCursor> kids = direct_children(call);
                if (!kids.empty())
                {
                    CXCursor callee = unwrap_expr(kids.front());
                    // 件三（逃逸集·读侧）：判据见 escaped_read_anchor。
                    escaped_read_anchor(rec, kids.front());
                    // 容器下标直接调用（形态 3）：判据与发射见
                    // try_container_elem_call；形态命中即发射站点行并返回。
                    if (try_container_elem_call(rec, callee, ctx.fn_key))
                    {
                        return;
                    }
                    // 成员函数指针调用：判据见 resolve_ptr_member_call。
                    resolve_ptr_member_call(rec, callee);
                    // 有名被调（字段/全局函数指针变量）锚点：见
                    // resolve_named_callee。
                    resolve_named_callee(rec, callee);
                    // 被调是本函数形参时的序号：见 resolve_callee_param。
                    resolve_callee_param(rec, callee, ctx.fn_decl);
                }
                out.call_sites.push_back(std::move(rec));
            }

            /**
             * @brief 透明转发函数范式判据（全 AST 形态，零名字白名单）。
             *
             * @internal 定义：函数体恰一条 return，return 表达式是对某形参的
             * 引用转换（cast），且函数返回类型是引用——返回的引用与形参实参
             * 指称同一对象，其一切调用点可视同实参本身（transparent
             * forwarder）。覆盖面自动含标准库 move/forward/as_const 与项目自
             * 定义转发助手——其本质就是"函数体内 static_cast 到
             * 引用"，不按名字枚举。
             * 边界（宁漏勿猜，逐条拒收）：定义不可见（纯声明）不判；body 除
             * return 外还有其它语句（StaticAssert 与只含类型别名的 DeclStmt
             * 放行——`using U = ...; return static_cast<U&&>(t);` 是标准库
             * move 的书写形态，纯别名无运行期效果；含变量声明的 DeclStmt 拒）
             * 不判；多 return 不判；return 无/多表达式不判；cast 形态限
             * CXXStaticCastExpr/CXXConstCastExpr（引用绑定语义；C 风格与
             * reinterpret_cast 拒——后者是位重解释，值同一性无从保证）；返回
             * 类型非引用（按值转换返回拷贝）不判——引用证据取函数返回类型的
             * raw kind（依赖名的引用经 canonical 化会折叠成 Unexposed，表达式
             * 侧 cast 类型同样被依赖糖折叠，二者都不可靠，实测
             * `T&&` 返回的 raw kind 稳定为 RValueReference）；操作数不是本函
             * 数形参（返回字段/全局的 getter 不是转发器）不判。结论按函数
             * usr 记忆化（forwarder_cache_，TU 内一次）。
             *
             * @return 透明形参位（0 基）；非转发器返回 nullopt。
             */
            [[nodiscard]]
            auto transparent_forwarder_param(const CXCursor& fn)
                -> std::optional<int>
            {
                const std::string key = cg::clang::usr(fn);
                if (key.empty())
                {
                    return std::nullopt;
                }
                const auto cached = forwarder_cache_.find(key);
                if (cached != forwarder_cache_.end())
                {
                    return cached->second;
                }
                std::optional<int> verdict;
                CXCursor           def = cg::clang::definition_of(fn);
                if (cg::clang::is_null(def))
                {
                    // 隐式实例化的引用（模板特化实例 decl）无定义：上溯主模板
                    // （标准库/项目助手的主模板定义在头文件内联可见），按主模板
                    // 的 body 判据（形参序以主模板为准）。
                    const CXCursor prim = cg::clang::specialized_template(fn);
                    if (!cg::clang::is_null(prim))
                    {
                        def = cg::clang::definition_of(prim);
                    }
                }
                if (!cg::clang::is_null(def))
                {
                    const std::vector<CXCursor> def_kids = direct_children(def);
                    const CXCursor*             body = nullptr;
                    for (const CXCursor& ch : def_kids)
                    {
                        if (cg::clang::kind(ch) == CXCursor_CompoundStmt)
                        {
                            body = &ch;
                            break;
                        }
                    }
                    if (body != nullptr)
                    {
                        verdict = forwarder_body_param(def, def_kids, *body);
                    }
                }
                forwarder_cache_.emplace(key, verdict);
                return verdict;
            }

            /**
             * @brief 透明转发判据的函数体部分（transparent_forwarder_param 的
             *        实现）。
             *
             * @internal def 是函数定义游标（返回类型证据从它取）；def_kids 是
             * 其直接子（形参序从中数）；body 是其 CompoundStmt。逐层判据：
             * 恰一 return（其它语句仅放行 StaticAssert 与纯类型别名
             * DeclStmt）→ 恰一 return 表达式 → static/const cast → 返回类型
             * 是引用 → 操作数剥后是本函数形参的 DeclRef。
             */
            [[nodiscard]]
            static auto forwarder_body_param(
                const CXCursor&              def,
                const std::vector<CXCursor>& def_kids,
                const CXCursor&              body) -> std::optional<int>
            {
                const std::vector<CXCursor> body_kids = direct_children(body);
                const CXCursor*             ret = nullptr;
                for (const CXCursor& ch : body_kids)
                {
                    const CXCursorKind ck = cg::clang::kind(ch);
                    if (ck == CXCursor_ReturnStmt)
                    {
                        if (ret != nullptr)
                        {
                            return std::nullopt; // 多条 return：不判
                        }
                        ret = &ch;
                        continue;
                    }
                    if (ck == CXCursor_StaticAssert)
                    {
                        continue; // 声明级子，无运行期效果
                    }
                    if (ck == CXCursor_DeclStmt)
                    {
                        // 纯类型别名声明（using/typedef）放行——标准库 move 的
                        // `using U = remove_reference_t`<T>`;` 书写形态；含变量
                        // 声明（初始化有副作用）拒收。
                        const std::vector<CXCursor> decl_kids =
                            direct_children(ch);
                        bool alias_only = !decl_kids.empty();
                        for (const CXCursor& dk : decl_kids)
                        {
                            const CXCursorKind dkk = cg::clang::kind(dk);
                            if (dkk != CXCursor_TypeAliasDecl
                                && dkk != CXCursor_TypedefDecl)
                            {
                                alias_only = false;
                                break;
                            }
                        }
                        if (alias_only)
                        {
                            continue;
                        }
                    }
                    return std::nullopt; // 还有别的语句：不判
                }
                if (ret == nullptr)
                {
                    return std::nullopt;
                }
                const std::vector<CXCursor> ret_kids = direct_children(*ret);
                const CXCursor*             value = nullptr;
                for (const CXCursor& ch : ret_kids)
                {
                    if (is_type_ref(ch))
                    {
                        continue;
                    }
                    if (value != nullptr)
                    {
                        return std::nullopt; // 多表达式：不判
                    }
                    value = &ch;
                }
                if (value == nullptr)
                {
                    return std::nullopt;
                }
                const CXCursorKind vk = cg::clang::kind(*value);
                if (vk != CXCursor_CXXStaticCastExpr
                    && vk != CXCursor_CXXConstCastExpr)
                {
                    return std::nullopt;
                }
                // 引用证据取函数返回类型的 raw kind（依赖名引用的 canonical
                // 与 cast 表达式类型都会折叠，唯一稳定证据是 raw 返回类型）。
                const cg::clang::TypeClass rc =
                    cg::clang::result_type_class(def);
                if (rc != cg::clang::TypeClass::LValueReference
                    && rc != cg::clang::TypeClass::RValueReference)
                {
                    return std::nullopt; // 按值返回：转发出去的是拷贝
                }
                const std::vector<CXCursor> cast_kids = direct_children(*value);
                const CXCursor*             operand = nullptr;
                for (const CXCursor& ch : cast_kids)
                {
                    if (is_type_ref(ch))
                    {
                        continue;
                    }
                    if (operand != nullptr)
                    {
                        return std::nullopt; // 多操作数：不判
                    }
                    operand = &ch;
                }
                if (operand == nullptr)
                {
                    return std::nullopt;
                }
                const CXCursor inner = unwrap_expr(*operand);
                if (cg::clang::kind(inner) != CXCursor_DeclRefExpr)
                {
                    return std::nullopt;
                }
                const CXCursor parm = cg::clang::referenced(inner);
                if (cg::clang::kind(parm) != CXCursor_ParmDecl)
                {
                    return std::nullopt; // 返回字段/全局：不是转发器
                }
                int ordinal = 0;
                for (const CXCursor& ch : def_kids)
                {
                    if (cg::clang::kind(ch) == CXCursor_ParmDecl)
                    {
                        if (cg::clang::equal_cursors(ch, parm))
                        {
                            return ordinal;
                        }
                        ++ordinal;
                    }
                }
                return std::nullopt; // 形参不来自本函数定义：不判
            }

            /**
             * @brief 透明转发调用视同实参本身：e 是透明转发器调用时返回其透明
             *        形参位的实参，否则原样返回 e。
             *
             * @internal 范式判据见 transparent_forwarder_param（零名字——标准
             * 库 move/forward 与自定义助手同判）。按“调用结果”处理转发调用会
             * 丢掉实参的变量名/返回槽；剥一层后实参交给 arg_vars/return-slot。
             * 实参位不足（默认参数/包展开形态）按原表达式返回，宁漏勿猜。
             */
            [[nodiscard]]
            auto identity_wrapper_operand(const CXCursor& e) -> CXCursor
            {
                const CXCursor cur = unwrap_expr(e);
                if (cg::clang::kind(cur) != CXCursor_CallExpr)
                {
                    return e;
                }
                const CXCursor ref = cg::clang::referenced(cur);
                if (cg::clang::is_null(ref))
                {
                    return e;
                }
                const CXCursorKind rk = cg::clang::kind(ref);
                if (rk != CXCursor_FunctionDecl
                    && rk != CXCursor_FunctionTemplate
                    && rk != CXCursor_CXXMethod)
                {
                    return e;
                }
                const std::optional<int> fwd = transparent_forwarder_param(ref);
                if (!fwd.has_value())
                {
                    return e;
                }
                CXCursor found{};
                bool     hit = false;
                int      seen = 0;
                for_each_child(
                    cur,
                    [&](const CXCursor& kid)
                    {
                        if (hit)
                        {
                            return; // 已取到透明位实参（找到即停）
                        }
                        if (is_type_ref(kid))
                        {
                            return;
                        }
                        const CXCursor inner = unwrap_expr(kid);
                        const CXCursor kid_ref = cg::clang::referenced(inner);
                        if (!cg::clang::is_null(kid_ref))
                        {
                            const CXCursorKind kk = cg::clang::kind(kid_ref);
                            if (kk == CXCursor_FunctionDecl
                                || kk == CXCursor_FunctionTemplate
                                || kk == CXCursor_CXXMethod
                                || kk == CXCursor_Constructor
                                || kk == CXCursor_ConversionFunction)
                            {
                                return; // 被调用者引用，不是实参
                            }
                        }
                        if (seen == *fwd)
                        {
                            found = kid;
                            hit = true;
                            return;
                        }
                        ++seen;
                    });
                return hit ? found : e;
            }

            void call_site(const CXCursor& c, const Ctx& ctx)
            {
                const cg::clang::SourceLocation site = cg::clang::location(c);
                const std::string               site_file = norm(site);
                if (!under_roots(site_file, opt))
                {
                    return;
                }
                // 构造调用（Constructor 放行）：直接子全是实参，不走下方"首子=
                // 被调用表达式"的形态判定（is_operator_invocation /
                // callee_is_call 在构造上调用的假设不成立）；实参 >=1
                // 才收录，目录不跳过（既有口径：全量收录；call_sites
                // 增量由验收对账观测）。
                if (const CXCursor raw = cg::clang::referenced(c);
                    !cg::clang::is_null(raw)
                    && cg::clang::kind(raw) == CXCursor_Constructor)
                {
                    constructor_call_site(c, raw, ctx, site, site_file);
                    return;
                }
                if (is_operator_invocation(c, cg::clang::referenced(c)))
                {
                    // 运算符调用本身不是用户间接调用；容器拷贝赋值
                    // （dst = src）的元素槽继承在此拦截处理。
                    // operator= 上的函数目标写入（std::function 字段
                    // `h.fn = &f` 的调用形态 def——裸字段赋值走 assignment()
                    // 的 FieldFlow，此处补调用形态，与容器拷贝不互斥）。
                    operator_assign_fn_target(c, ctx);
                    // 断链 2b：壳槽的 operator= 写侧穿透（p_impl_ = {p} 形）。
                    operator_assign_shell_store(c, ctx);
                    // 工厂赋值臂：模板特化槽载体被 make_unique/make_shared
                    // 赋值（函数体字段工厂赋值，标准库智能指针不满足壳
                    // 判据的形态）。
                    operator_assign_factory_store(c, ctx);
                    container_copy_assign(c, ctx);
                    return;
                }
                if (callee_is_call(c))
                {
                    // 被调用表达式是内层调用/容器下标：外层调用的是它的结果
                    // （函数指针/可调用对象），不得沿用内层被调用者。
                    indirect_call(c, ctx, site_file, site);
                    return;
                }
                if (is_member_pointer_call(c))
                {
                    indirect_call(c, ctx, site_file, site);
                    return;
                }
                if (receiver_type_is_dependent(c))
                {
                    // 依赖接收者的成员调用：referenced 解析不可信（幻觉边），
                    // 具名 calls 边一律不建；receiver 静态类型为空，indirect
                    // 降级行对 dispatch 也无候选可解析，一并省去（宁漏勿误）。
                    return;
                }
                if (container_write_site(c, ctx))
                {
                    // std 容器写集调用（push_back/insert/assign…）：证据已按
                    // "#elem:" 槽发行；方法定义在系统头，本就不是用户间接调用。
                    return;
                }
                const CXCursor ref = referenced_callable(c);
                if (cg::clang::is_null(ref))
                {
                    const CXCursor any_ref = cg::clang::referenced(c);
                    if (!cg::clang::is_null(any_ref))
                    {
                        const std::string  name = cg::clang::spelling(any_ref);
                        const CXCursorKind raw_kind = cg::clang::kind(any_ref);
                        if (name.starts_with("operator"))
                        {
                            if (name == "operator()")
                            {
                                // 函数对象调用（std::function/lambda/仿函
                                // 数）——operator() 恰是用户间接调用，落
                                // indirect 行（槽锚点在 indirect_call 提取）。
                                // 界外形态（CPO 全局对象/stdexec 定制
                                // 点）叠加 operator() 桥（体采集+实参登记，
                                // 见 cpo_operator_bridge）；界内形态桥内自判
                                // 跳过。
                                cpo_operator_bridge(c, any_ref, ctx);
                                indirect_call(c, ctx, site_file, site);
                            }
                            return; // 其余运算符调用不是用户间接调用
                        }
                        // 构造/析构与被名字过滤掉的内部函数：不是间接调用。
                        if (raw_kind == CXCursor_Constructor
                            || raw_kind == CXCursor_Destructor
                            || raw_kind == CXCursor_ConversionFunction
                            || raw_kind == CXCursor_CXXMethod
                            || raw_kind == CXCursor_FunctionDecl
                            || raw_kind == CXCursor_FunctionTemplate)
                        {
                            return;
                        }
                    }
                    indirect_call(c, ctx, site_file, site);
                    return;
                }
                const std::string ref_file = norm(cg::clang::location(ref));
                // 界外 callee（声明在系统头/未索引 TU）：命中加载系 API 名单才
                // 落站点——这是 dlopen 调用点证据配对的唯一输入源。真实语料的
                // Win32 API 声明在界外（此前此守卫整点丢弃，API 锚点永无输入、
                // dlopen 分支零激活；夹具把声明写在 TU 内掩盖了缺口）。站点照实
                // 落 call_sites，callee 永不解析成库内符号（propagate 侧同名单
                // 短路按名解析，防 API 名撞库内符号），不产生 calls 边；其余界
                // 外调用照旧不收录（量级与语义都不该进库）。
                const bool        external_callee = !under_roots(ref_file, opt);
                const std::string ref_name = cg::clang::spelling(ref);
                // 双名单界外放行：加载系（dlopen 证据配对）与平台回调（注册
                // 契约线索边）。其余界外调用照旧不收录。站点是"契约/证据"不
                // 是调用，callee（API 本身）永不解析。
                const bool loader_site = is_dlopen_api(ref_name);
                const int  cb_slot =
                    loader_site ? 0 : callback_param_slot(ref_name);
                // 第三类放行：界外模板实例化（std::sort/thread 类）——站点
                // 落库带 callee_usr（实例桥锚点），实参目标写 external_param_
                // args，与实例体内采集的 invokes 按 (host_usr, param) join。
                // 判定与锚点必须在【原始实例 decl】上做：referenced_callable
                // 会把实例归约回主模板（:1895 的既有语义），而主模板的
                // specialized_template 返回 null，在此判定必假（探针
                // 实证：clang 的 CallExpr referenced 给的就是实例）。
                const CXCursor raw_callee = cg::clang::referenced(c);
                const bool tmpl_instance_site = external_callee && cb_slot == 0
                                                && [&]
                {
                    if (cg::clang::is_null(raw_callee))
                    {
                        return false;
                    }
                    const CXCursorKind rk = cg::clang::kind(raw_callee);
                    // 方法实例（stdexec CPO 的 operator()、算法对象调用）
                    // 与函数实例同列；构造实例走 constructor 分支单列。
                    if (rk != CXCursor_FunctionDecl && rk != CXCursor_CXXMethod)
                    {
                        return false;
                    }
                    const CXCursor tmpl =
                        cg::clang::specialized_template(raw_callee);
                    return !cg::clang::is_null(tmpl)
                           && !is_invalid_cursor(tmpl);
                }();
                if (external_callee && !loader_site && cb_slot == 0
                    && !tmpl_instance_site)
                {
                    // 函数对象调用（std::function 字段/变量/形参、lambda 闭包
                    // 变量）：operator() 定义在界外头，但语义是间接调用点——
                    // 首子（implicit object 表达式）经 indirect_call 的
                    // MemberRef/DeclRef 分支提取槽锚点（字段名+owner / 变量
                    // 名；DeclRef 形参判定顺带覆盖 std::function 形参直传形
                    // 态），交消费侧按槽解析。界内仿函数不受影响（external_
                    // callee 判定在，走具名路径）。
                    if (cg::clang::kind(ref) == CXCursor_CXXMethod
                        && ref_name == "operator()" && has_direct_child(c))
                    {
                        indirect_call(c, ctx, site_file, site);
                    }
                    return;
                }

                CallSiteRec rec;
                rec.caller_key = ctx.fn_key;
                rec.callee = method_ref_of(ref);
                const CXCursorKind ref_kind = cg::clang::kind(ref);
                rec.is_virtual = ref_kind == CXCursor_CXXMethod
                                 && cg::clang::is_virtual_method(ref);
                // 限定调用：被调子式的 AST 判据（qualified_callee）；实参里
                // 的命名空间限定（p->f(qns::make())）判据只看被调子式，
                // 天然排除。构造调用不走本判据（限定符挂在外层
                // CXXFunctionalCastExpr，采集面不可达），见 constructor_call_
                // site 的 token 判据。
                const std::vector<CXCursor> kids = direct_children(c);
                rec.qualified = !kids.empty()
                                && qualified_callee(unwrap_expr(kids.front()));
                rec.file = site_file;
                rec.line = static_cast<int>(site.line);
                rec.col = static_cast<int>(site.column);

                // 接收者：成员调用的第一子是 MemberRefExpr；describe_receiver
                // 负责剥智能指针 operator-> 与 this->field。
                if (!kids.empty())
                {
                    const CXCursor callee_expr = unwrap_expr(kids.front());
                    if (cg::clang::kind(callee_expr) == CXCursor_MemberRefExpr)
                    {
                        // MemberRefExpr 本身是
                        // `object.f`；接收者是它的对象子式， 不是成员名。
                        const std::vector<CXCursor> member_kids =
                            direct_children(callee_expr);
                        if (!member_kids.empty())
                        {
                            const ReceiverInfo info =
                                describe_receiver(member_kids.front());
                            rec.receiver_name = info.name;
                            rec.receiver_is_this = info.via_this;
                            rec.receiver_owner_key = info.owner_key;
                            rec.static_receiver_types = info.static_types;
                            if (info.name.empty())
                            {
                                // 接收者在场未解析位（receiver_is_this 的
                                // name 空扩展义）：接收者表达式显式写在源
                                // 码里（出参解引用/下标/链式 getter 等）但
                                // describe_receiver 没解出槽名——本 libclang
                                // 只对虚调用的被调表达式暴露接收者基子，
                                // 非虚中间环节（`x.get().Virt()` 的 get）
                                // 的对象子结构性不可达。此时接收者对象的
                                // 动态类型与调用方所属类没有任何结构关
                                // 联，决议端的隐式 this 回退见此位整档不
                                // 接管（维持披露），绝不拿调用方类当静态
                                // 根误直连。裸调用（真隐式 this）的被调表
                                // 达式无基子（member_kids 空），不进本块
                                // 天然不置位，隐式 this 回退语义保持。
                                rec.receiver_is_this = true;
                            }
                        }
                    }
                }
                for (std::size_t i = 1; i < kids.size(); ++i)
                {
                    fill_arg_records(
                        rec,
                        c,
                        identity_wrapper_operand(kids[i]),
                        static_cast<int>(i - 1));
                }
                outparam_writeback_call(rec, c, ctx);
                shell_attach_call(rec, c, ref, ctx);
                // 工具写回（f(值, 出参槽)）与 iid 锁（QI(令牌, &out)）的
                // 写回行补采：判据与发射见各函数。
                tool_writeback_call(rec, c, ctx);
                iid_lock_call(rec, c, ctx);
                rec.api_callback = cb_slot > 0;
                if (rec.api_callback)
                {
                    callback_reg_of(ctx, kids, cb_slot, site_file, site);
                }
                if (tmpl_instance_site)
                {
                    rec.callee_usr = cg::clang::usr(raw_callee);
                    // 隐式实例化的函数体不在 TU 顶层遍历（探针定论），从调用
                    // 点的实例 decl 直接进入体内采集 invoke。
                    external_instance_body(raw_callee, ctx.fn_key);
                    // 实参→实例形参的桥（默认参数/实参多于形参时宁漏勿猜：
                    // 只对显式给出的实参位直配）。
                    int param_count = 0;
                    for_each_child(
                        raw_callee,
                        [&](const CXCursor& ch)
                        {
                            if (cg::clang::kind(ch) == CXCursor_ParmDecl)
                            {
                                ++param_count;
                            }
                        });
                    for (std::size_t i = 1; i < kids.size(); ++i)
                    {
                        const int ordinal = static_cast<int>(i - 1);
                        if (ordinal >= param_count)
                        {
                            break;
                        }
                        const std::string target = function_target_key(
                            identity_wrapper_operand(kids[i]));
                        if (target.empty())
                        {
                            continue;
                        }
                        out.external_param_args.push_back(
                            {.host_usr = rec.callee_usr,
                             .param_index = ordinal,
                             .caller_key = ctx.fn_key,
                             .target_key = target,
                             .file = site_file,
                             .line = rec.line,
                             .col = rec.col});
                    }
                }
                out.call_sites.push_back(std::move(rec));
            }

            /**
             * @brief 构造调用点（实参 >=1；无接收者、恒非虚）。
             *
             * @internal callee 用 Constructor 引用自身（method_ref_of 的
             * sig_hash 来自其形参拼写）；解析后 SymbolLookup 的 function_like
             * 含 Constructor，全局实参->形参传播与出参回填（callee 写 *pp
             * 的构造注入 形态）天然可用。
             */
            void constructor_call_site(
                const CXCursor&                  c,
                const CXCursor&                  ref,
                const Ctx&                       ctx,
                const cg::clang::SourceLocation& site,
                const std::string&               site_file)
            {
                const std::string ref_file = norm(cg::clang::location(ref));
                if (!under_roots(ref_file, opt))
                {
                    // 界外模板构造实例（std::thread t(fn)/std::function`<F>`
                    // f=&g 类）：构造体随用户 TU 实例化，体内对形参的调用可见——
                    // 与 call_site 的 tmpl_instance_site 同款桥（thread 构造把
                    // callable 转发到 _State 存储后由桥函数调用的穿透起点）。
                    // 判定：构造的 referenced 是特化实例（specialized 非
                    // null）。
                    const CXCursor tmpl = cg::clang::specialized_template(ref);
                    if (cg::clang::is_null(tmpl) || is_invalid_cursor(tmpl))
                    {
                        return;
                    }
                    // 构造体所在的宿主是类模板实例；体内 invoke 采集用宿主
                    // 构造的 USR 作 join 锚（与 external_param_args 对齐）。
                    const std::string host_usr = cg::clang::usr(ref);
                    if (host_usr.empty())
                    {
                        return;
                    }
                    const std::vector<CXCursor> cargs = ctor_arguments(c, ref);
                    std::vector<CXCursor>       fn_args;
                    for (const CXCursor& a : cargs)
                    {
                        if (!is_type_ref(a))
                        {
                            fn_args.push_back(a);
                        }
                    }
                    if (fn_args.empty())
                    {
                        return;
                    }
                    // 实参目标桥（只对函数目标实参位直配；与 call_site 同款
                    // 宁漏勿猜边界）。
                    bool any_target = false;
                    for (std::size_t i = 0; i < fn_args.size(); ++i)
                    {
                        const std::string target = function_target_key(
                            identity_wrapper_operand(fn_args[i]));
                        if (target.empty())
                        {
                            continue;
                        }
                        any_target = true;
                        out.external_param_args.push_back(
                            {.host_usr = host_usr,
                             .param_index = static_cast<int>(i),
                             .caller_key = ctx.fn_key,
                             .target_key = target,
                             .file = site_file,
                             .line = static_cast<int>(site.line),
                             .col = static_cast<int>(site.column)});
                    }
                    // 体内采集：构造体是 CXXConstructor（external_instance_body
                    // 的 ParmDecl 枚举与 CallExpr 扫描对构造体同样成立）。
                    if (any_target)
                    {
                        external_instance_body(ref, ctx.fn_key);
                    }
                    return;
                }
                const std::vector<CXCursor> args = ctor_arguments(c, ref);
                if (args.empty())
                {
                    return; // 实参 >=1 门槛（无参构造不收录）
                }
                CallSiteRec rec;
                rec.caller_key = ctx.fn_key;
                rec.callee = method_ref_of(ref);
                rec.is_virtual = false;
                // 限定判定保持 token 判据（第一个 '(' 之前找 "::"）：构造
                // 调用的限定符游标挂在外层 CXXFunctionalCastExpr 的
                // children 上，内层 CXXConstructExpr 不可达（C API 无父
                // 游标链），AST 判据在此面失效；token 判据是唯一覆盖
                // （含 ::T(a) 全局限定——限定符为全局时不产任何子游标，
                // AST 任何判据的盲区）。
                for (const std::string& tok : cg::clang::token_spellings(c))
                {
                    if (tok == "(")
                    {
                        break;
                    }
                    if (tok == "::")
                    {
                        rec.qualified = true;
                        break;
                    }
                }
                rec.file = site_file;
                rec.line = static_cast<int>(site.line);
                rec.col = static_cast<int>(site.column);
                for (std::size_t ai = 0; ai < args.size(); ++ai)
                {
                    fill_arg_records(
                        rec,
                        c,
                        identity_wrapper_operand(args[ai]),
                        static_cast<int>(ai));
                }
                out.call_sites.push_back(std::move(rec));
            }

            /**
             * @brief 实参只记槽锚点：四个平行数组同位写入空类型集、槽名、
             *        空调用键、空目标。
             *
             * @internal 槽名编码与 ref_var_name 同规则——owner_key 为空用
             * 裸名，非空用 @fld 字段描述符（encode_field_slot）。
             */
            void push_slot_anchor(
                CallSiteRec&       rec,
                const std::string& var,
                const std::string& owner_key)
            {
                rec.arg_types.push_back({});
                rec.arg_vars.push_back(
                    owner_key.empty() ? var
                                      : encode_field_slot(owner_key, var));
                rec.arg_call_keys.push_back({});
                rec.arg_targets.push_back({});
            }

            /**
             * @brief
             * 单个实参的采集层记录：类型集、槽名（含取址槽形态）、返回槽。
             *
             * @internal 取址实参前置：实参是 X.M(...) 且 M 命中取址方法判据
             * （is_addressof_method；成员链比对 Reject
             * 的对抗形态不激活）时，arg_vars 记 接收者 X 的槽名（变量名或 @fld
             * 字段描述符）——该实参的语义是 "X 槽的地址"，交 propagate
             * 的回填触发（callee #p{i} 槽存在 OutParamFlow
             * 行才回填）。取址形态不记返回槽（取址方法的 #ret
             * 不是值来源）也不记类型（取址结果无记录语义）。
             */
            void fill_arg_records(
                CallSiteRec&    rec,
                const CXCursor& call,
                const CXCursor& arg,
                const int       ordinal)
            {
                const CXCursor arg_core = strip_casts(arg);
                if (cg::clang::kind(arg_core) == CXCursor_CallExpr)
                {
                    const CXCursor m = cg::clang::referenced(arg_core);
                    if (!cg::clang::is_null(m)
                        && cg::clang::kind(m) == CXCursor_CXXMethod
                        && cg::clang::canonical_result_pointee_spelling(m, 2)
                               .has_value())
                    { // 前置廉价闸（结果能剥两层指针）
                        const auto hit = addressof_method(m);
                        if (hit.has_value()
                            && hit->chain != AddressofChain::Reject)
                        {
                            const std::vector<CXCursor> kids =
                                direct_children(arg_core);
                            if (!kids.empty())
                            {
                                const CXCursor callee_expr =
                                    unwrap_expr(kids.front());
                                if (cg::clang::kind(callee_expr)
                                    == CXCursor_MemberRefExpr)
                                {
                                    const std::vector<CXCursor> member_kids =
                                        direct_children(callee_expr);
                                    if (!member_kids.empty())
                                    {
                                        const ReceiverInfo info =
                                            describe_receiver(
                                                member_kids.front());
                                        if (!info.name.empty()
                                            && info.name != "this")
                                        {
                                            push_slot_anchor(
                                                rec,
                                                info.name,
                                                info.owner_key);
                                            return;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // 断链 1（实参侧）：透传 getter 调用实参（sink(p.Get()) /
                // Equals(rhs.Get())）——实参值 = getter 接收者壳实例槽的
                // pointee 集，arg_vars 记接收者槽（变量名或 @fld 描述符），不
                // 记 @fn 返回槽（pattern #ret 恒空）也不记静态类型。
                if (cg::clang::kind(arg_core) == CXCursor_CallExpr)
                {
                    const CXCursor obj = passthrough_getter_object(arg_core);
                    if (!cg::clang::is_null(obj))
                    {
                        const ReceiverInfo info = describe_receiver(obj);
                        if (!info.name.empty() && info.name != "this")
                        {
                            push_slot_anchor(rec, info.name, info.owner_key);
                            return;
                        }
                    }
                }
                // 断链（handler-flow 解引用穿透）：解引用实参穿透记名——`*p`（p
                // 指向 record 的 一级指针）作直接调用第 ordinal 实参，且 callee
                // 第 ordinal 形 参是 record
                // 引用/指针（值形参=切片，闸死）时，arg_vars 记操作
                // 数槽名（变量裸名 / @fld 字段描述符）。lambda 捕获引用解析回外
                // 层形参、按首版语义归调用点所在函数域；其余通道留空——该实参
                // 的证据只能沿槽传播，不产生直接类型（与透传 getter 分支同型的
                // "只记锚点"约定）。callee 无 ParmDecl（间接调用/模板实例）或任
                // 一判据不过时维持现状空名（宁漏勿猜）。
                if (cg::clang::kind(arg_core) == CXCursor_UnaryOperator)
                {
                    const std::vector<std::string> toks =
                        cg::clang::token_spellings(arg_core);
                    if (!toks.empty() && toks.front() == "*"
                        && callee_param_takes_record_ref_or_ptr(call, ordinal))
                    {
                        const auto slot = slot_ref_of(arg, true);
                        if (slot.has_value() && !slot->var_name.empty())
                        {
                            push_slot_anchor(
                                rec,
                                slot->var_name,
                                slot->owner_key);
                            return;
                        }
                    }
                }
                rec.arg_types.push_back(expr_type_keys(arg));
                rec.arg_vars.push_back(ref_var_name(arg));
                rec.arg_call_keys.push_back(call_result_key(arg));
                rec.arg_targets.push_back(function_target_key(arg));
            }

            /**
             * @brief 出参壳写回（v.Set(e) -> 出参槽的 OutParamFlow 行）。
             *
             * @internal
             * 变量一致判据：接收者名必须命中本函数构造期登记的绑定表，
             * 且被调用方法名等于出参包装判据的写回方法名；e 的证据沿用
             * expr_type_keys / ref_var_name / call_result_key
             * 三件套（无直接类型
             * 时留传播边，由传播（函数内/全局）从源槽取得）。
             */
            void outparam_writeback_call(
                const CallSiteRec& rec,
                const CXCursor&    c,
                const Ctx&         ctx)
            {
                if (outparam_bindings_.empty() || rec.receiver_name.empty()
                    || ctx.fn_key.empty())
                {
                    return;
                }
                const auto it = outparam_bindings_.find(
                    cg::util::compose_key(ctx.fn_key, rec.receiver_name));
                if (it == outparam_bindings_.end()
                    || rec.callee.name != it->second.writeback)
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.size() < 2)
                {
                    return; // 写回方法恰 1 形参，首实参即写回值
                }
                const CXCursor value = identity_wrapper_operand(kids[1]);
                std::vector<std::string> keys = expr_type_keys(value);
                const std::string        related_var = ref_var_name(value);
                const std::string        related_key = call_result_key(value);
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                TypeHintRec hint =
                    make_hint(it->second.owner_key, it->second.var_name, loc);
                hint.type_keys = keys;
                hint.source = HintSource::OutParamFlow;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = "out_param_flow";
                if (keys.empty())
                {
                    hint.related_var = related_var;
                    hint.related_key = related_key;
                }
                emit_hint(std::move(hint));
            }

            /**
             * @brief 指针式壳的 Attach 形写回（v.Attach(e) -> v 槽
             * pointee）。
             *
             * @internal 判据：被调用方法（pattern 归一，非静态
             * CXXMethod）恰一个 T* 形参，且其所属类模板 pattern
             * 是指针式壳——该位实参即 pointee 证据，交给 hint_from_expr
             * 递归保级；接收者槽：字段以声明记录为
             * 所有者（receiver_owner_key），局部变量以函数为所有者。非存储型
             * T* 方法（Sink::Write 反例）的残余风险 = 接收者槽多一条 Medium 候
             * 选，只增不减方向，与参考语料 Attach/智能指针 reset 家族收益权衡后
             * 收录（判据面：operator-> + T* 存储字段已挡容器）。
             */
            void shell_attach_call(
                const CallSiteRec& rec,
                const CXCursor&    c,
                const CXCursor&    ref,
                const Ctx&         ctx)
            {
                if (rec.receiver_name.empty() || ctx.fn_key.empty())
                {
                    return;
                }
                const CXCursor pattern_fn = pattern_function_of(ref);
                if (cg::clang::kind(pattern_fn) != CXCursor_CXXMethod
                    || cg::clang::is_static_method(pattern_fn))
                {
                    return;
                }
                const CXCursor cls = pattern_class_of(ref);
                if (cg::clang::is_null(cls) || !is_pointer_shell_cached(cls))
                {
                    return;
                }
                const int pos = single_tp_pointer_param(pattern_fn, cls);
                if (pos < 0)
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (static_cast<std::size_t>(pos) + 1 >= kids.size())
                {
                    return;
                }
                const CXCursor value = identity_wrapper_operand(kids[1 + pos]);
                const std::string owner = rec.receiver_owner_key.empty()
                                              ? ctx.fn_key
                                              : rec.receiver_owner_key;
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                hint_from_expr(
                    value,
                    owner,
                    rec.receiver_name,
                    norm(loc),
                    static_cast<int>(loc.line),
                    static_cast<int>(loc.column));
            }

            /** @brief 函数型游标的形参序列（按声明序）。 */
            [[nodiscard]]
            static auto param_decls_of(const CXCursor& fn)
                -> std::vector<CXCursor>
            {
                std::vector<CXCursor> params;
                for_each_child(
                    fn,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch) == CXCursor_ParmDecl)
                        {
                            params.push_back(ch);
                        }
                    });
                return params;
            }

            /** @brief 记录直属成员是否含纯虚方法（接口性判据，零名字）。 */
            [[nodiscard]]
            static auto has_pure_virtual_member(const CXCursor& record) -> bool
            {
                bool pure = false;
                for_each_child(
                    record,
                    [&](const CXCursor& ch)
                    {
                        if (pure)
                        {
                            return;
                        }
                        if (cg::clang::kind(ch) == CXCursor_CXXMethod
                            && cg::clang::is_pure_virtual(ch))
                        {
                            pure = true;
                        }
                    });
                return pure;
            }

            /**
             * @brief 形参是否 P** / P*& 出参方向（canonical 恰两层可剥指针
             *        且最深 pointee 是记录）。
             *
             * @internal 签名方向闸：两层剥得开、第三层剥不开定深度；
             *           pointee_declaration 剥到底取记录声明。值形参/一级
             *           指针/三级指针/内建 pointee 全部拒绝（试点实测：无
             *           闸 33% 误判、双指针出参方向闸后 0）。
             */
            [[nodiscard]]
            static auto param_out_direction_record(const CXCursor& p)
                -> CXCursor
            {
                if (!cg::clang::canonical_pointee_spelling(p, 2).has_value()
                    || cg::clang::canonical_pointee_spelling(p, 3).has_value())
                {
                    return CXCursor{};
                }
                const CXCursor rec = cg::clang::pointee_declaration(p);
                if (cg::clang::is_null(rec) || is_invalid_cursor(rec)
                    || !is_record_kind(cg::clang::kind(rec)))
                {
                    return CXCursor{};
                }
                return rec;
            }

            /**
             * @brief 出参写回值实参的 pointee 证据（写回行补采的值源）。
             *
             * @internal 三形态：①壳工厂模板调用——模板实参直取（工厂体在
             *           另一 TU / 模板形参依赖时 #ret 恒空，调用点的具体实
             *           参才是可靠值源），pointee 位取壳直属 T* 字段的 own
             *           形参下标；②透传 getter/Detach 调用——接收者槽别名
             *           （返回值 = 接收者壳实例槽的 pointee）；③指向记录的
             *           一级指针变量/字段——related 边到源槽。record 字段供
             *           配对闸（派生证明）；取不到记录的形态不发（宁漏勿
             *           猜）。
             */
            struct OutValueEvidence
            {
                std::vector<std::string> keys;
                std::string              related_var;
                std::string              related_key;
                CXCursor                 record{};
            };

            [[nodiscard]]
            auto outparam_value_evidence(const CXCursor& value)
                -> OutValueEvidence
            {
                OutValueEvidence   evidence;
                const CXCursor     core = unwrap_expr(strip_casts(value));
                const CXCursorKind k = cg::clang::kind(core);
                if (k == CXCursor_CallExpr)
                {
                    if (typed_pointer_shell(core))
                    {
                        const CXCursor pattern =
                            cg::clang::specialized_template(
                                cg::clang::type_declaration(core));
                        const auto tp = shell_tp_field_slot(pattern);
                        if (!tp.has_value())
                        {
                            return evidence;
                        }
                        const CXCursor arg =
                            cg::clang::type_template_arg_declaration(
                                core,
                                tp->tp_index);
                        if (cg::clang::is_null(arg) || is_invalid_cursor(arg)
                            || !is_record_kind(cg::clang::kind(arg)))
                        {
                            return evidence;
                        }
                        evidence.keys.push_back(record_key_of_decl(arg));
                        evidence.record = arg;
                        return evidence;
                    }
                    const CXCursor obj = passthrough_getter_object(core);
                    if (!cg::clang::is_null(obj))
                    {
                        const ReceiverInfo ri = describe_receiver(obj);
                        if (!ri.name.empty() && ri.name != "this")
                        {
                            const auto idx = passthrough_pointee_index(obj);
                            if (idx.has_value())
                            {
                                const CXCursor arg =
                                    cg::clang::type_template_arg_declaration(
                                        obj,
                                        *idx);
                                if (!cg::clang::is_null(arg)
                                    && !is_invalid_cursor(arg)
                                    && is_record_kind(cg::clang::kind(arg)))
                                {
                                    evidence.related_var =
                                        ri.owner_key.empty()
                                            ? ri.name
                                            : encode_field_slot(
                                                  ri.owner_key,
                                                  ri.name);
                                    evidence.record = arg;
                                }
                            }
                        }
                    }
                    return evidence;
                }
                if (k == CXCursor_CXXNewExpr)
                {
                    evidence.keys = expr_type_keys(core);
                    evidence.record = cg::clang::pointee_declaration(core);
                    return evidence;
                }
                if (k == CXCursor_DeclRefExpr || k == CXCursor_MemberRefExpr)
                {
                    const CXCursor decl = cg::clang::referenced(core);
                    if (cg::clang::is_null(decl))
                    {
                        return evidence;
                    }
                    const CXCursorKind dk = cg::clang::kind(decl);
                    if ((dk != CXCursor_VarDecl && dk != CXCursor_ParmDecl
                         && dk != CXCursor_FieldDecl)
                        || !cg::clang::is_pointer_or_reference(decl))
                    {
                        return evidence;
                    }
                    const CXCursor rec = cg::clang::pointee_declaration(decl);
                    if (cg::clang::is_null(rec) || is_invalid_cursor(rec)
                        || !is_record_kind(cg::clang::kind(rec)))
                    {
                        return evidence;
                    }
                    evidence.related_var = ref_var_name(core);
                    evidence.record = rec;
                    return evidence;
                }
                return evidence;
            }

            /**
             * @brief 工具写回调用（f(值, 出参槽) 形态）的写回行补采。
             *
             * @internal 工厂体把出参写回经中转函数（写发生在中转体内、工
             *           厂自己的出参形参槽零行——调用方写回触发永不点火）
             *           的形态：在调用点直接补采 OutParamFlow 行到出参槽实
             *           参的槽。判据纯结构四闸：被调可解析且形参有 P**
             *           出参方向位（签名方向闸）；该位实参槽可寻址（裸名/&x/取址方
             *           法锚点）；另一位实参有 pointee 记录证据（壳工厂模
             *           板实参直取/透传接收者/一级指针源）；值记录派生自或
             *           等于出参 pointee（record_derives_from——赋值兼容证
             *           明值属于这个出参位）。任一闸不过不发（宁漏勿猜）。
             */
            void tool_writeback_call(
                const CallSiteRec& rec,
                const CXCursor&    c,
                const Ctx&         ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const CXCursor raw = cg::clang::referenced(c);
                if (cg::clang::is_null(raw)
                    || (cg::clang::kind(raw) != CXCursor_FunctionDecl
                        && cg::clang::kind(raw) != CXCursor_CXXMethod))
                {
                    return;
                }
                const std::string nm = cg::clang::spelling(raw);
                if (nm.starts_with("operator") || nm.starts_with("__")
                    || nm.starts_with("_M_") || nm.starts_with("_X"))
                {
                    return;
                }
                const std::vector<CXCursor> params = param_decls_of(raw);
                if (params.empty())
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.size() < 3)
                {
                    return; // 值与出参两位至少三子（被调表达式 + 两实参）
                }
                for (std::size_t k = 0;
                     k < params.size() && k + 1 < kids.size();
                     ++k)
                {
                    const CXCursor out_rec =
                        param_out_direction_record(params[k]);
                    if (cg::clang::is_null(out_rec))
                    {
                        continue;
                    }
                    // 出参位实参的槽锚点：fill_arg_records 已按位登记（裸
                    // 变量名 / @fld 字段描述符 / 取址方法接收者）。
                    if (k >= rec.arg_vars.size() || rec.arg_vars[k].empty())
                    {
                        continue;
                    }
                    const std::string& anchor = rec.arg_vars[k];
                    std::string        target_owner = ctx.fn_key;
                    std::string        target_var = anchor;
                    if (anchor.starts_with(field_slot_prefix))
                    {
                        const auto field = decode_field_slot(anchor);
                        if (field.has_value())
                        {
                            target_owner = field->first;
                            target_var = field->second;
                        }
                    }
                    // 值实参扫描：另位实参的 pointee 记录派生自出参记录。
                    for (std::size_t m = 0;
                         m < params.size() && m + 1 < kids.size();
                         ++m)
                    {
                        if (m == k)
                        {
                            continue;
                        }
                        const OutValueEvidence ev = outparam_value_evidence(
                            identity_wrapper_operand(kids[m + 1]));
                        if (cg::clang::is_null(ev.record)
                            || is_invalid_cursor(ev.record)
                            || !record_derives_from(ev.record, out_rec))
                        {
                            continue;
                        }
                        const cg::clang::SourceLocation loc =
                            cg::clang::location(c);
                        const std::string file = norm(loc);
                        if (!under_roots(file, opt))
                        {
                            return;
                        }
                        TypeHintRec hint =
                            make_hint(target_owner, target_var, loc);
                        hint.type_keys = ev.keys;
                        hint.source = HintSource::OutParamFlow;
                        hint.confidence = Confidence::Medium;
                        hint.fallback_reason = "out_param_tool";
                        if (ev.keys.empty())
                        {
                            hint.related_var = ev.related_var;
                        }
                        emit_hint(std::move(hint));
                        break; // 一位多值只并一行（槽级并集语义）
                    }
                }
            }

            /**
             * @brief iid 令牌实参（零值实参的模板调用、第 0 模板实参是记
             *        录、对应形参是记录值/引用位）。
             *
             * @internal 身份描述符经值/const 引用传递的结构面（iid 契约
             *           锚点）：零名字参与。
             */
            [[nodiscard]]
            auto is_iid_token_of(const CXCursor& arg, const CXCursor& param)
                -> CXCursor
            {
                const CXCursor iface = iid_token_template_record(arg);
                if (cg::clang::is_null(iface) || is_invalid_cursor(iface)
                    || !has_pure_virtual_member(iface))
                {
                    return CXCursor{};
                }
                // iid 位形参是记录值/引用（身份描述符类型）。
                const CXCursor by_ref = cg::clang::pointee_declaration(param);
                const CXCursor param_rec =
                    cg::clang::is_null(by_ref)
                        ? cg::clang::type_declaration(param)
                        : by_ref;
                if (cg::clang::is_null(param_rec)
                    || is_invalid_cursor(param_rec)
                    || !is_record_kind(cg::clang::kind(param_rec)))
                {
                    return CXCursor{};
                }
                return iface;
            }

            /**
             * @brief iid 锁调用（QI(令牌, &out) 形态）的出参槽接口级证据。
             *
             * @internal 模板实参直取为出参槽接口级类型（QI 契约：写入对象
             *           必是实现该接口的某个类）——接口键入槽后由 propagate
             *           的派生闭包展开成实现族并集，实现族恰一时决议端收敛
             *           唯一、多实现停 may（零误连纪律）。判据纯结构：iid
             *           令牌位（零值实参模板调用 + 接口性第 0 模板实参 +
             *           记录值/引用形参）与出参方向位（双剥恰两层、
             *           最深 pointee 不限记录——void 双指针的 QI 出参位在
             *           射程）分立扫描；出参位实参槽可寻址才发。
             */
            void iid_lock_call(
                const CallSiteRec& rec,
                const CXCursor&    c,
                const Ctx&         ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const CXCursor raw = cg::clang::referenced(c);
                if (cg::clang::is_null(raw)
                    || (cg::clang::kind(raw) != CXCursor_FunctionDecl
                        && cg::clang::kind(raw) != CXCursor_CXXMethod))
                {
                    return;
                }
                const std::vector<CXCursor> params = param_decls_of(raw);
                const std::vector<CXCursor> kids = direct_children(c);
                if (params.empty() || kids.size() < 3)
                {
                    return;
                }
                // iid 令牌位扫描：命中即得接口记录。
                CXCursor iface{};
                for (std::size_t i = 0;
                     i < params.size() && i + 1 < kids.size();
                     ++i)
                {
                    iface = is_iid_token_of(
                        identity_wrapper_operand(kids[i + 1]),
                        params[i]);
                    if (!cg::clang::is_null(iface))
                    {
                        break;
                    }
                }
                if (cg::clang::is_null(iface))
                {
                    return;
                }
                const std::string iface_key = record_key_of_decl(iface);
                if (iface_key.empty())
                {
                    return; // 界外接口：槽证据无处落
                }
                // 出参方向位：双剥成功即可（void** 不限记录）。
                for (std::size_t j = 0;
                     j < params.size() && j + 1 < kids.size();
                     ++j)
                {
                    if (!cg::clang::canonical_pointee_spelling(params[j], 2)
                             .has_value()
                        || cg::clang::canonical_pointee_spelling(params[j], 3)
                               .has_value())
                    {
                        continue;
                    }
                    if (j >= rec.arg_vars.size())
                    {
                        continue;
                    }
                    std::string target_owner;
                    std::string target_var;
                    if (!rec.arg_vars[j].empty())
                    {
                        const std::string& anchor = rec.arg_vars[j];
                        target_owner = ctx.fn_key;
                        target_var = anchor;
                        if (anchor.starts_with(field_slot_prefix))
                        {
                            const auto field = decode_field_slot(anchor);
                            if (field.has_value())
                            {
                                target_owner = field->first;
                                target_var = field->second;
                            }
                        }
                    }
                    else
                    {
                        // 取址槽回退锚（壳上的 void** 出参位方法，如经
                        // reinterpret_cast 转发 T** 的形态——取址方法判据天然拒
                        // 绝、arg_vars 为空）：零值实参成员调用、结果
                        // canonical 双剥可解、接收者可寻址时锚定接收者槽。
                        // 语义与 X.Put() 同源（接收者槽的地址传入出参位）。
                        const CXCursor arg =
                            j + 1 < kids.size()
                                ? unwrap_expr(strip_casts(kids[j + 1]))
                                : CXCursor{};
                        if (cg::clang::kind(arg) != CXCursor_CallExpr)
                        {
                            continue;
                        }
                        const CXCursor m = cg::clang::referenced(arg);
                        if (cg::clang::is_null(m)
                            || cg::clang::kind(m) != CXCursor_CXXMethod
                            || !cg::clang::canonical_result_pointee_spelling(
                                    m,
                                    2)
                                    .has_value())
                        {
                            continue;
                        }
                        std::size_t non_type = 0;
                        for (const CXCursor& kid : direct_children(arg))
                        {
                            if (!is_type_ref(kid))
                            {
                                ++non_type;
                            }
                        }
                        if (non_type != 1)
                        {
                            continue; // 恰被调表达式一个非类型子（零值实参）
                        }
                        const std::vector<CXCursor> mk = direct_children(arg);
                        if (mk.empty()
                            || cg::clang::kind(unwrap_expr(mk.front()))
                                   != CXCursor_MemberRefExpr)
                        {
                            continue;
                        }
                        const std::vector<CXCursor> member_kids =
                            direct_children(unwrap_expr(mk.front()));
                        if (member_kids.empty())
                        {
                            continue;
                        }
                        const ReceiverInfo ri =
                            describe_receiver(member_kids.front());
                        if (ri.name.empty() || ri.name == "this")
                        {
                            continue;
                        }
                        target_owner =
                            ri.owner_key.empty() ? ctx.fn_key : ri.owner_key;
                        target_var = ri.name;
                    }
                    if (target_var.empty())
                    {
                        continue;
                    }
                    const cg::clang::SourceLocation loc =
                        cg::clang::location(c);
                    const std::string file = norm(loc);
                    if (!under_roots(file, opt))
                    {
                        return;
                    }
                    // 声明面伴随行：iid 把出参槽的可证静态面锁到接口级——
                    // 族并集写回按此面裁剪（QI 巨族的 may 并集被界外裁掉，
                    // 只留接口实现族；实现族 >1 时多候选停 may）。
                    // iid 锁行发射熔断：真库（687 TU 全量）实测本函数与
                    // As 形态发射的行令传播后写库段稳定段错误（exit 139，
                    // 五次复现；发射全关的对照全量成功——隔离实验三角定
                    // 位到发射层，接口闭包实测最大 25 排除规模因素，根因
                    // 待定位）。按"拿不准的形态宁可不开闸"纪律熔断：判定
                    // 判据保留（闸门逻辑零改动），两行不发射，qi 展开与
                    // As 调用形态（recv.As<IFace>(IidOf<IFace>())）的射程
                    // 暂缓。根因定位后解除本熔断。
                    TypeHintRec face = make_hint(target_owner, target_var, loc);
                    face.static_types = {iface_key};
                    // emit_hint(std::move(face));
                    TypeHintRec hint = make_hint(target_owner, target_var, loc);
                    hint.type_keys = {iface_key};
                    hint.source = HintSource::OutParamFlow;
                    hint.confidence = Confidence::Medium;
                    hint.fallback_reason = "qi_iid_lock";
                    // emit_hint(std::move(hint));
                }
            }

            /**
             * @brief 容器写集：push_back/insert/assign 等调用把值实参的
             *        动态证据递归保级地落进 "#elem:`<容器>`" 槽。
             *
             * @internal 声明期闸门（elem_slots_）在先，最坏 no-op。std 容器
             * 方法定义在系统头（libc++/libstdc++/MSVC STL），其 referenced
             * 的 ref 永远过不了 call_site 对被调用者的 under_roots 守卫——
             * 故本函数由 call_site 在该守卫之前调用，接收者与被调用者名自行
             * 组装（不依赖 CallSiteRec），命中（callee 名在写集表且接收者槽
             * 已登记）即拦截并返回 true：容器方法调用不是用户间接调用。未
             * 命中（项目内同名方法等）返回 false 交回正常调用点路径。assign
             * 的区间拷贝形态（assign(src.begin(), src.end())）识别为元素槽
             * 拷贝流——源容器取自迭代器工厂调用的对象表达式；assign(n, v)
             * 填充形态取值实参 1。insert 按实参数区分关联容器（1 实参，值
             * 位 0）与顺序容器（2 实参，值位 1）双形态；迭代器区间插入
             * （>2 实参）不处理。值证据经 hint_from_expr 递归保级（new 保持
             * DirectNew High；智能指针壳构造经穿透判据/名义档剥壳）。
             */
            [[nodiscard]]
            bool container_write_site(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return false;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.empty())
                {
                    return false;
                }
                const CXCursor callee_expr = unwrap_expr(kids.front());
                if (cg::clang::kind(callee_expr) != CXCursor_MemberRefExpr)
                {
                    return false;
                }
                const std::string callee = cg::clang::spelling(callee_expr);
                if (std::ranges::find(CONTAINER_WRITES, callee)
                    == std::end(CONTAINER_WRITES))
                {
                    return false;
                }
                const std::vector<CXCursor> member_kids =
                    direct_children(callee_expr);
                if (member_kids.empty())
                {
                    return false;
                }
                const ReceiverInfo info =
                    describe_receiver(member_kids.front());
                if (info.name.empty())
                {
                    return false;
                }
                const std::string owner =
                    info.owner_key.empty() ? ctx.fn_key : info.owner_key;
                if (!elem_slot_registered(owner, info.name))
                {
                    return false;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return true; // 根外站点：仍拦截（不建调用点），不发证据
                }
                const std::string slot =
                    std::string{container_elem_slot_prefix} + info.name;
                // 成员调用：首子是被调用表达式，实参从 1 起。
                std::vector<CXCursor> args;
                for (std::size_t i = 1; i < kids.size(); ++i)
                {
                    if (!is_type_ref(kids[i]))
                    {
                        args.push_back(kids[i]);
                    }
                }
                const auto emit_copy_flow = [&](const std::string& src)
                {
                    TypeHintRec hint = make_hint(owner, slot, loc);
                    hint.source = HintSource::ContainerElemFlow;
                    hint.confidence = Confidence::Medium;
                    hint.fallback_reason = "container_elem_flow";
                    hint.related_var = encode_container_elem_slot(src);
                    emit_hint(std::move(hint));
                };
                if (callee == "assign")
                {
                    if (args.size() != 2)
                    {
                        return true; // assign(il) / 0 实参：无元素证据形态
                    }
                    const CXCursor head = unwrap_expr(args[0]);
                    if (cg::clang::kind(head) == CXCursor_CallExpr)
                    {
                        const CXCursor m = cg::clang::referenced(head);
                        if (!cg::clang::is_null(m)
                            && std::ranges::find(
                                   ITERATOR_FACTORIES,
                                   cg::clang::spelling(m))
                                   != std::end(ITERATOR_FACTORIES))
                        {
                            // 区间拷贝：源容器 = 迭代器工厂调用的对象表达式。
                            const std::vector<CXCursor> mkids =
                                direct_children(head);
                            if (!mkids.empty())
                            {
                                const CXCursor iter_expr =
                                    unwrap_expr(mkids.front());
                                if (cg::clang::kind(iter_expr)
                                    == CXCursor_MemberRefExpr)
                                {
                                    const std::vector<CXCursor> obj_kids =
                                        direct_children(iter_expr);
                                    if (!obj_kids.empty())
                                    {
                                        const std::string src =
                                            ref_var_name(obj_kids.front());
                                        if (!src.empty())
                                        {
                                            emit_copy_flow(src);
                                        }
                                    }
                                }
                            }
                            return true;
                        }
                    }
                    // 填充形态 assign(n, v)：值实参位 1。
                    hint_from_expr(
                        args[1],
                        owner,
                        slot,
                        file,
                        static_cast<int>(loc.line),
                        static_cast<int>(loc.column));
                    return true;
                }
                std::size_t value_pos = 0;
                if (callee == "insert")
                {
                    if (args.size() == 1)
                    {
                        value_pos = 0;
                    }
                    else if (args.size() == 2)
                    {
                        value_pos = 1;
                    }
                    else
                    {
                        return true; // insert(first, last)：无值语义
                    }
                }
                else if (callee == "emplace_hint")
                {
                    value_pos = 1;
                }
                if (value_pos < args.size())
                {
                    hint_from_expr(
                        args[value_pos],
                        owner,
                        slot,
                        file,
                        static_cast<int>(loc.line),
                        static_cast<int>(loc.column));
                }
                return true;
            }

            /**
             * @brief 容器拷贝赋值（`dst = src;`）：operator= 是运算符调用，
             *        被 is_operator_invocation 滤出用户调用面——本函数在该滤点
             *        被调用，元素槽继承源元素槽（函数内传播经 "#elem:"
             * 引用拷贝）。
             *
             * @internal CXXOperatorCallExpr 的 libclang 形态是 CallExpr 直接子
             * [operator 引用, 接收者, 实参]；成员调用形态首子是 MemberRefExpr
             * （接收者取其对象子）。仅同域变量/字段源（返回容器不走元素流，
             * 返回槽类型是容器对象而非元素）。声明期闸门在先，未登记 no-op。
             */
            void container_copy_assign(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const CXCursor m = cg::clang::referenced(c);
                if (cg::clang::is_null(m)
                    || cg::clang::kind(m) != CXCursor_CXXMethod
                    || cg::clang::spelling(m) != "operator=")
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.size() < 2)
                {
                    return;
                }
                // 子游标按源码位置排序（libclang 契约）：`dst = src` 的 callee
                // 引用（operator=，位于 '=' 处）可能夹在接收者与实参之间——
                // 按内容识别而非位置：排除引用 operator= 的子游标，其余恰两
                // 个依源位置为接收者与实参。
                std::vector<CXCursor> operands;
                for (const CXCursor& kid : kids)
                {
                    const CXCursor     inner = unwrap_expr(kid);
                    const CXCursorKind ik = cg::clang::kind(inner);
                    if (ik == CXCursor_DeclRefExpr
                        || ik == CXCursor_MemberRefExpr)
                    {
                        const CXCursor r = cg::clang::referenced(inner);
                        if (!cg::clang::is_null(r)
                            && (cg::clang::kind(r) == CXCursor_CXXMethod
                                || cg::clang::kind(r)
                                       == CXCursor_FunctionTemplate)
                            && cg::clang::spelling(r) == "operator=")
                        {
                            continue; // callee 引用，不是操作数
                        }
                    }
                    operands.push_back(inner);
                }
                if (operands.size() != 2)
                {
                    return;
                }
                const CXCursor lhs = operands.front();
                const CXCursor rhs = operands.back();
                std::string    recv;
                std::string    recv_owner;
                if (cg::clang::kind(lhs) == CXCursor_MemberRefExpr)
                {
                    const std::vector<CXCursor> mk = direct_children(lhs);
                    if (mk.empty())
                    {
                        return;
                    }
                    const ReceiverInfo info = describe_receiver(mk.front());
                    recv = info.name;
                    recv_owner = info.owner_key;
                }
                else
                {
                    recv = ref_var_name(lhs);
                }
                if (recv.empty())
                {
                    return;
                }
                const std::string owner =
                    recv_owner.empty() ? ctx.fn_key : recv_owner;
                if (!elem_slot_registered(owner, recv))
                {
                    return;
                }
                const std::string src = ref_var_name(rhs);
                if (src.empty() || !call_result_key(rhs).empty())
                {
                    return; // 初始化列表/返回容器等：无元素流形态
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                TypeHintRec hint = make_hint(
                    owner,
                    std::string{container_elem_slot_prefix} + recv,
                    loc);
                hint.source = HintSource::ContainerElemFlow;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = "container_elem_flow";
                hint.related_var = encode_container_elem_slot(src);
                emit_hint(std::move(hint));
            }

            /**
             * @brief 容器元素 range-for 链接行：CXCursor_CXXForRangeStmt
             * 直接子恰 三个（range 声明 / range 初始化式 / body，fac-llvm
             * 源码级核实；kind 225 是独立语句种类，永不命中 ForStmt case——
             *        预研 v1 的核心教训）。
             *
             * @internal 链接行 var_name = 循环变量、related_var =
             * "#elem:`<容器引用>`"（裸容器名或 @fld 字段描述符），由 propagate
             * 的函数内阶段解析拷贝——循环变量槽获得元素槽的写集类型，range-for
             * 位点的
             * 虚调用接收者因此拿到动态证据。容器必须先完成声明期登记（局部/
             * 形参在函数域、字段在记录域），未登记不发行。
             */
            void for_range_stmt(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(c);
                if (kids.size() != 3
                    || cg::clang::kind(kids[0]) != CXCursor_VarDecl)
                {
                    return;
                }
                const std::string var = cg::clang::spelling(kids[0]);
                const std::string src = ref_var_name(kids[1]);
                if (var.empty() || src.empty())
                {
                    return;
                }
                bool registered = false;
                if (src.starts_with(field_slot_prefix))
                {
                    const auto field = decode_field_slot(src);
                    registered =
                        field.has_value()
                        && elem_slot_registered(field->first, field->second);
                }
                else
                {
                    registered = elem_slot_registered(ctx.fn_key, src);
                }
                if (!registered)
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                TypeHintRec hint = make_hint(ctx.fn_key, var, loc);
                hint.source = HintSource::ContainerElemFlow;
                hint.confidence = Confidence::Medium;
                hint.fallback_reason = "container_elem_flow";
                hint.related_var = encode_container_elem_slot(src);
                emit_hint(std::move(hint));
            }

            /**
             * @brief 取址边采集：&Class::method / &free_fn 记取址证据行。
             *
             * @internal 数据契约与消费方（cg::db::address_taken_hints + cli
             * 标注） 对齐：source = AddressOfObject(6) 且 related_key =
             * 被取址符号的 unique_key 文本（&obj 对象行 related_key
             * 恒空，天然区分），var_name 用 #addr 保留槽名（propagate
             * 按槽名跳过）。unique_key 优先按定义 游标计算（与 cg.extract
             * 逐字节一致）；本 TU 只见声明/特化游标时可 能与符号表 key
             * 不匹配——消费方按精确文本匹配，查不到就空（宁漏 勿误，契约声明的
             * fallback）。取址对象必须是方法/函数引用 （DeclRefExpr 或无基
             * MemberRefExpr，穿透 UnexposedExpr）； &obj / &*p / &arr[i]
             * 不在采集面。AST 廉价闸在前（操作数形态 + 引用种类），"&" token
             * 确认最后做，避免对全部一元表达式花 token 化
             * 成本。渲染展示不在本簇。
             */
            /**
             * @brief 函数形 ParmDecl 的稳定 key（定义优先，取址 seed 同源）。
             */
            [[nodiscard]]
            auto function_decl_key(const CXCursor& fn) -> std::string
            {
                CXCursor       key_src = fn;
                const CXCursor def = cg::clang::definition_of(fn);
                if (!cg::clang::is_null(def) && !is_invalid_cursor(def))
                {
                    key_src = def;
                }
                return function_key_of(
                    key_src,
                    norm(cg::clang::location(key_src)));
            }

            /**
             * @brief lambda 表达式的 operator() 符号登记（TU 内去重）。
             *
             * @internal key 由位置唯一化（`operator()@L<file>:<line>:<col>`），
             *           签名哈希 0——它只作 indirect 目标标识，不参与签名匹配。
             */
            [[nodiscard]]
            auto lambda_key_of(const cg::clang::SourceLocation& loc)
                -> std::string
            {
                const std::string file = norm(loc);
                const std::string name =
                    std::format("operator()@L{}:{}", file, loc.line);
                const std::string key =
                    make_function_key("function", "", name, file, 0, false);
                if (lambda_seen_.insert(key).second)
                {
                    out.lambda_symbols.push_back(
                        {.key = key,
                         .name = name,
                         .file = file,
                         .line = static_cast<int>(loc.line),
                         .col = static_cast<int>(loc.column)});
                }
                return key;
            }

            /**
             * @brief 表达式的函数目标 key（def 事件提取）。
             *
             * @internal 三形态：`&fn`（UO_AddrOf 包 DeclRef 函数）、裸函数名
             *           （DeclRef 函数 = 隐式转指针）、LambdaExpr（登记后取其
             *           operator() key）。经变量中转/取值不透——那是槽消费的活
             *           （宁漏勿猜）。函数定义在界外时返回空（无处可连）。
             */
            [[nodiscard]]
            auto function_target_key(const CXCursor& expr) -> std::string
            {
                CXCursor e = unwrap_expr(expr);
                if (cg::clang::kind(e) == CXCursor_LambdaExpr)
                {
                    return lambda_key_of(cg::clang::location(e));
                }
                // 实例化体的 lambda→函数指针隐式转换被显式化
                // （UnexposedExpr"operator T(*)(...)" > CallExpr > MemberRef >
                // … > LambdaExpr，实测；模板定义体里是裸
                // LambdaExpr，两种形态并存）。宽度受限下探：只允许转换
                // 形态节点（转换 CallExpr/MemberRef/Paren/Unexposed）垫在
                // LambdaExpr 之上，别处不动——宁漏勿猜边界不变。
                {
                    CXCursor cur = e;
                    for (int depth = 0; depth < 6; ++depth)
                    {
                        if (cg::clang::kind(cur) == CXCursor_LambdaExpr)
                        {
                            return lambda_key_of(cg::clang::location(cur));
                        }
                        const CXCursorKind          ck = cg::clang::kind(cur);
                        const std::vector<CXCursor> kids = direct_children(cur);
                        if (kids.empty())
                        {
                            break;
                        }
                        CXCursor next = kids.front();
                        if (ck == CXCursor_CallExpr
                            && cg::clang::spelling(cur).starts_with("operator"))
                        {
                            // 转换函数调用：唯一操作数在首子（成员形态首子是
                            // 转换函数 MemberRef，跳过取第二子）。
                            if (cg::clang::kind(next) == CXCursor_MemberRefExpr
                                && kids.size() > 1)
                            {
                                next = kids[1];
                            }
                        }
                        else if (
                            ck == CXCursor_MemberRefExpr
                            || ck == CXCursor_UnexposedExpr
                            || ck == CXCursor_ParenExpr)
                        {
                            // 转换链垫层。
                        }
                        else
                        {
                            break;
                        }
                        cur = unwrap_expr(next);
                    }
                }
                if (cg::clang::kind(e) == CXCursor_UnaryOperator)
                {
                    const std::vector<CXCursor>    kids = direct_children(e);
                    const std::vector<std::string> toks =
                        cg::clang::token_spellings(e);
                    if (toks.empty() || toks.front() != "&" || kids.empty())
                    {
                        return {};
                    }
                    e = unwrap_expr(kids.front());
                }
                if (cg::clang::kind(e) != CXCursor_DeclRefExpr)
                {
                    return {};
                }
                const CXCursor     ref = cg::clang::referenced(e);
                const CXCursorKind rk = cg::clang::kind(ref);
                if (rk != CXCursor_CXXMethod && rk != CXCursor_FunctionDecl
                    && rk != CXCursor_FunctionTemplate)
                {
                    return {};
                }
                const std::string def_file =
                    norm(cg::clang::location(cg::clang::definition_of(ref)));
                if (!under_roots(def_file, opt)
                    || (def_file.empty()
                        && !under_roots(norm(cg::clang::location(ref)), opt)))
                {
                    return {};
                }
                return function_decl_key(ref);
            }

            void address_of_evidence(const CXCursor& c, const Ctx& ctx)
            {
                if (ctx.fn_key.empty())
                {
                    return;
                }
                CXCursor target{};
                bool     found = false;
                for_each_child(
                    c,
                    [&](const CXCursor& kid)
                    {
                        if (found || is_type_ref(kid))
                        {
                            return;
                        }
                        target = kid; // 首个非类型子（找到即停）
                        found = true;
                    });
                if (cg::clang::is_null(target))
                {
                    return;
                }
                target = unwrap_expr(target);
                const CXCursorKind tk = cg::clang::kind(target);
                if (tk != CXCursor_DeclRefExpr && tk != CXCursor_MemberRefExpr)
                {
                    return;
                }
                const CXCursor     ref = cg::clang::referenced(target);
                const CXCursorKind rk = cg::clang::kind(ref);
                if (rk != CXCursor_CXXMethod && rk != CXCursor_FunctionDecl
                    && rk != CXCursor_FunctionTemplate)
                {
                    return; // VarDecl（&obj）/未知引用不采集
                }
                const std::vector<std::string> toks =
                    cg::clang::token_spellings(c);
                if (toks.empty() || toks.front() != "&")
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    return;
                }
                TypeHintRec hint =
                    make_hint(ctx.fn_key, std::string{address_taken_slot}, loc);
                hint.source = HintSource::AddressOfObject;
                hint.confidence = Confidence::High;
                hint.fallback_reason = "address_of_method";
                hint.related_key = function_decl_key(ref);
                emit_hint(std::move(hint));
            }

            void function(const CXCursor& c)
            {
                const cg::clang::SourceLocation loc = cg::clang::location(c);
                const std::string               file = norm(loc);
                if (!under_roots(file, opt))
                {
                    // 界外模板实例化体：定义不在 roots 但随本 TU 实例化（
                    // specialized_template 非 null = 实例非本尊）。轻量采集
                    // 只记"体内对形参的调用"（泛型算法把形参当可调用物），不
                    // 收符号不收其它结构——std::sort/thread 的 comp(a,b) 类
                    // 回调契约点由此进入实例桥。
                    if (!cg::clang::is_definition(c))
                    {
                        return;
                    }
                    const CXCursor tmpl = cg::clang::specialized_template(c);
                    if (cg::clang::is_null(tmpl) || is_invalid_cursor(tmpl))
                    {
                        return;
                    }
                    external_instance_body(c);
                    return;
                }
                const bool pure = cg::clang::kind(c) == CXCursor_CXXMethod
                                  && cg::clang::is_pure_virtual(c);
                const bool defaulted = cg::clang::is_defaulted_method(c);
                if (!cg::clang::is_definition(c) && !pure && !defaulted)
                {
                    return;
                }
                Ctx inner;
                inner.fn_key = function_key_of(c, file);
                inner.record_key =
                    record_key_of_decl(cg::clang::semantic_parent(c));
                inner.param_index = 0;
                inner.fn_decl = c;
                const CXCursor prev_host_fn = host_fn_;
                host_fn_ = c;
                const std::vector<CXCursor> fn_kids = direct_children(c);
                // 断链 2a：构造函数的成员初始化列表——libclang 把每个初始化项
                // 暴露为"引用字段的 MemberRef 游标 + 同级后随的初始化表达式"
                // （实测），MemberRef 标记配对后随首个非类型子表达式后仍
                // 照常 visit（初始化表达式内的构造调用点等既有行为不变）。
                const bool is_ctor = cg::clang::kind(c) == CXCursor_Constructor;
                for (std::size_t i = 0; i < fn_kids.size(); ++i)
                {
                    if (is_ctor
                        && cg::clang::kind(fn_kids[i]) == CXCursor_MemberRef)
                    {
                        const CXCursor fld = cg::clang::referenced(fn_kids[i]);
                        if (!cg::clang::is_null(fld)
                            && cg::clang::kind(fld) == CXCursor_FieldDecl)
                        {
                            for (std::size_t j = i + 1; j < fn_kids.size(); ++j)
                            {
                                if (is_type_ref(fn_kids[j]))
                                {
                                    continue;
                                }
                                ctor_shell_field_init(fld, fn_kids[j], inner);
                                break;
                            }
                        }
                    }
                    visit(fn_kids[i], inner);
                }
                host_fn_ = prev_host_fn;
            }

            /**
             * @brief 界外模板实例化体的轻量采集（件一）。
             *
             * @internal 只找"callee 引用本函数形参"的调用点（重载调用
             * comp(a,b) 的 callee 是 DeclRef(形参)；operator 形态
             * comp.operator() 的 callee 是 MemberRef、base 是 DeclRef(形参)）。
             * 形参序号用 ParmDecl 在函数 children 里的出现序（与调用实参对位
             * 同一序系）。门控：单 TU 2000 条，超限置 capped 停止（如实外露）。
             */
            void external_instance_body(
                const CXCursor&    fn,
                const std::string& entry_caller = {})
            {
                const std::string host_usr = cg::clang::usr(fn);
                if (host_usr.empty() || !ext_host_seen_.insert(host_usr).second)
                {
                    return;
                }
                std::unordered_map<std::string, int> params;
                int                                  ordinal = 0;
                for_each_child(
                    fn,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch) == CXCursor_ParmDecl)
                        {
                            params.emplace(cg::clang::spelling(ch), ordinal++);
                        }
                    });
                if (params.empty())
                {
                    return;
                }
                // 只递归可含语句的节点（不落 hint/call_site 副作用：这里的
                // visit 使用空 fn_key 的 Ctx，一切要求 fn_key 的采集自动
                // no-op； 唯一可能触达的是 VarDecl/表达式递归本身——零落库）。
                Ctx bare;
                for_each_child(
                    fn,
                    [&](const CXCursor& ch)
                    {
                        collect_param_invokes(
                            ch,
                            host_usr,
                            params,
                            entry_caller);
                    });
            }

            /**
             * @brief 件一 def：界外体内 `param->fnfield = <函数目标>` 的
             *        字段槽登记（host="fld#"+FieldDecl USR，见 collect_param_
             *        invokes 的 @internal 注释）。
             *
             * @internal 接收者限定为形参（base 剥壳后 DeclRef→ParmDecl）——
             * 局部变量接收者的字段链不可靠，宁漏勿猜。RHS 经 function_target_
             * key 提取（lambda 无界外守卫直接编码；界外普通函数目标会被守卫
             * 拒收——其库内无符号，连边也无 SymId，属正确降级）。
             */
            void ext_field_def(
                const CXCursor&                             node,
                const std::unordered_map<std::string, int>& params,
                const std::string&                          entry_caller)
            {
                const std::vector<CXCursor> kids = direct_children(node);
                if (kids.size() != 2)
                {
                    return;
                }
                if (cg::clang::binary_opcode(node) != CX_BO_Assign)
                {
                    return;
                }
                const CXCursor lhs = unwrap_expr(kids.front());
                if (cg::clang::kind(lhs) != CXCursor_MemberRefExpr)
                {
                    return;
                }
                const CXCursor fld = cg::clang::referenced(lhs);
                if (cg::clang::kind(fld) != CXCursor_FieldDecl)
                {
                    return;
                }
                const std::vector<CXCursor> mk = direct_children(lhs);
                if (mk.empty())
                {
                    return;
                }
                const CXCursor base = unwrap_expr(mk.front());
                if (cg::clang::kind(base) != CXCursor_DeclRefExpr
                    || cg::clang::kind(cg::clang::referenced(base))
                           != CXCursor_ParmDecl)
                {
                    return;
                }
                if (params.find(
                        cg::clang::spelling(cg::clang::referenced(base)))
                    == params.end())
                {
                    return;
                }
                const std::string fld_usr = cg::clang::usr(fld);
                if (fld_usr.empty())
                {
                    return;
                }
                const std::string target = function_target_key(kids.back());
                if (target.empty())
                {
                    return; // RHS 非函数目标/界外普通函数——宁漏勿猜
                }
                const cg::clang::SourceLocation loc = cg::clang::location(node);
                const std::string               site_file = norm(loc);
                const int site_line = static_cast<int>(loc.line);
                const int site_col = static_cast<int>(loc.column);
                out.external_param_args.push_back(
                    {.host_usr = std::format("fld#{}", fld_usr),
                     .param_index = 0,
                     .caller_key = entry_caller,
                     .target_key = target,
                     .file = site_file,
                     .line = site_line,
                     .col = site_col});
                if (++ext_invoke_count_ > EXT_INVOKE_BUDGET)
                {
                    ext_invoke_capped_ = true;
                    out.ext_invoke_capped = true;
                }
            }

            /**
             * @brief 件一 use：界外体内 `param->fnfield(...)` 的调用点登记
             *        （invoke + "接收者形参槽=字段槽" 占位转发）。返回 true
             *        表示按字段调用处理完毕（不再走 base 剥壳的 DeclRef
             * 路径）。
             */
            [[nodiscard]]
            bool ext_field_use(
                const CXCursor&                             call,
                const CXCursor&                             callee,
                const std::string&                          host_usr,
                const std::unordered_map<std::string, int>& params)
            {
                const CXCursor fld = cg::clang::referenced(callee);
                if (cg::clang::kind(fld) != CXCursor_FieldDecl)
                {
                    return false; // 普通成员函数调用（CXXMethod）——原路径
                }
                const std::vector<CXCursor> mk = direct_children(callee);
                if (mk.empty())
                {
                    return true; // 字段引用但无 base——吞掉（非可采形态）
                }
                const CXCursor base = unwrap_expr(mk.front());
                if (cg::clang::kind(base) != CXCursor_DeclRefExpr
                    || cg::clang::kind(cg::clang::referenced(base))
                           != CXCursor_ParmDecl)
                {
                    return true; // 接收者非形参（局部变量/链式）——宁漏勿猜
                }
                const auto it = params.find(
                    cg::clang::spelling(cg::clang::referenced(base)));
                if (it == params.end())
                {
                    return true;
                }
                const std::string fld_usr = cg::clang::usr(fld);
                if (fld_usr.empty())
                {
                    return true;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(call);
                const std::string               site_file = norm(loc);
                const int site_line = static_cast<int>(loc.line);
                const int site_col = static_cast<int>(loc.column);
                out.external_invokes.push_back(
                    {.host_usr = host_usr,
                     .param_index = it->second,
                     .file = site_file,
                     .line = site_line,
                     .col = site_col});
                out.external_param_args.push_back(
                    {.host_usr = host_usr,
                     .param_index = it->second,
                     .caller_key = {},
                     .target_key = std::format("ext#fld#{}#0", fld_usr),
                     .file = site_file,
                     .line = site_line,
                     .col = site_col});
                if (++ext_invoke_count_ > EXT_INVOKE_BUDGET)
                {
                    ext_invoke_capped_ = true;
                    out.ext_invoke_capped = true;
                }
                return true;
            }

            /**
             * @brief 递归扫描实例化体，落"对形参的调用点"（external_invokes）
             *        与"形参转发到下层实例化体"的级联桥（必修一/二），
             *        以及界外字段槽的 def/use（件一）。
             *
             * @internal 级联（libc++ 实证形态）：sort 体内 `__sort_impl(first,
             * last, __comp)`——callee 是下层界外实例 decl（DeclRef 指向实例），
             * 实参是对本 host 形参的直接 DeclRef（无 forward 包裹；有包裹时
             * 剥壳后仍是 DeclRef）。此时：a) 记转发 ExternalParamArgRec
             * {host=下层 usr, param=下层形参位,
             * target="ext#`<本host>`#`<i>`"}； b) 对下层实例递归
             * external_instance_body（体内再采 invoke/再 级联）。target 的
             * "ext#" 前缀占位由消费侧（indirect_commit）
             * 不动点解析（graph.build.ixx）。
             *
             * 界外字段槽（stdexec 实测形态：__execute 字段的存与调都在界外
             * 实例体内）——编码完全复用 ext# 占位机制，消费侧零改动：
             * a) def（形参字段的函数指针字段被赋函数目标，如
             *    `op->__execute = [](...){...}`）：param_args{host="fld#"+
             *    FieldDecl USR, param=0, caller=entry_caller, target=函数键}；
             * b) use（形参的函数指针字段被调用，如 `task->__execute(t)`）：
             *    invoke{host=本 host, param=接收者形参序} + param_args{同
             *    host/param, target="ext#fld#`<usr>`#0"}——接收者形参槽的目标
             *    集即该字段槽的目标集。FieldDecl USR 用 @/字母数字（无 '#'，
             *    rfind('#') 解析安全）。RHS 非函数目标（一元 + 转指针等深
             *    形态）不采——宁漏勿猜。
             */
            void collect_param_invokes(
                const CXCursor&                             node,
                const std::string&                          host_usr,
                const std::unordered_map<std::string, int>& params,
                const std::string&                          entry_caller = {})
            {
                for_each_child(
                    node,
                    [&](const CXCursor& ch)
                    {
                        collect_param_invokes(
                            ch,
                            host_usr,
                            params,
                            entry_caller);
                    });
                // 件一 def：BinaryOperator 赋值——LHS 是形参的函数指针字段
                // （接收者为形参，含 "->" 与 "." 两形态）、RHS 剥出函数目标。
                if (cg::clang::kind(node) == CXCursor_BinaryOperator)
                {
                    if (!ext_invoke_capped_)
                    {
                        ext_field_def(node, params, entry_caller);
                    }
                    return;
                }
                if (cg::clang::kind(node) != CXCursor_CallExpr)
                {
                    return;
                }
                if (ext_invoke_capped_)
                {
                    return;
                }
                const std::vector<CXCursor> kids = direct_children(node);
                if (kids.empty())
                {
                    return;
                }
                // 被调用表达式（首子）剥到 DeclRef：重载调用形态。
                CXCursor callee = unwrap_expr(kids.front());
                // 界外字段槽 use：callee 是 MemberRef 且 referenced 是
                // FieldDecl （函数指针字段调用，如
                // task->__execute(task,tid)）——先于 "comp.operator()" 的 base
                // 剥壳分支处理。
                if (cg::clang::kind(callee) == CXCursor_MemberRefExpr)
                {
                    if (ext_field_use(node, callee, host_usr, params))
                    {
                        return;
                    }
                    const std::vector<CXCursor> mk = direct_children(callee);
                    if (!mk.empty())
                    {
                        callee = unwrap_expr(mk.front());
                    }
                }
                if (cg::clang::kind(callee) != CXCursor_DeclRefExpr)
                {
                    return;
                }
                const CXCursor          ref = cg::clang::referenced(callee);
                const enum CXCursorKind ref_kind = cg::clang::kind(ref);
                if (ref_kind == CXCursor_ParmDecl)
                {
                    const auto it = params.find(cg::clang::spelling(ref));
                    if (it == params.end())
                    {
                        return;
                    }
                    const cg::clang::SourceLocation loc =
                        cg::clang::location(node);
                    out.external_invokes.push_back(
                        {.host_usr = host_usr,
                         .param_index = it->second,
                         .file = norm(loc),
                         .line = static_cast<int>(loc.line),
                         .col = static_cast<int>(loc.column)});
                    if (++ext_invoke_count_ > EXT_INVOKE_BUDGET)
                    {
                        ext_invoke_capped_ = true;
                        out.ext_invoke_capped = true;
                    }
                    return;
                }
                // 级联：callee 是另一个界外模板实例（函数/方法），某实参是本
                // host 的形参 → 记转发并递归下层体内采集。
                if (ref_kind != CXCursor_FunctionDecl
                    && ref_kind != CXCursor_CXXMethod)
                {
                    return;
                }
                // 级联判据：callee 是界外头文件里的函数/方法定义即可。不查
                // specialized_template——本 libclang 对部分实例形态返回垃圾
                // （见 :1060 既有注释），而"界外 + 有定义 + 有形参转发"三重
                // 条件已足够窄（转发本身即守卫，宁漏勿猜）。
                const cg::clang::SourceLocation sub_loc =
                    cg::clang::location(ref);
                const std::string sub_file = norm(sub_loc);
                if (under_roots(sub_file, opt))
                {
                    return;
                }
                if (!cg::clang::is_definition(ref))
                {
                    return;
                }
                const std::string sub_usr = cg::clang::usr(ref);
                if (sub_usr.empty())
                {
                    return;
                }
                const cg::clang::SourceLocation loc = cg::clang::location(node);
                const std::string               site_file = norm(loc);
                const int site_line = static_cast<int>(loc.line);
                const int site_col = static_cast<int>(loc.column);
                bool      any_forwarded = false;
                for (std::size_t i = 1; i < kids.size(); ++i)
                {
                    const int ordinal = static_cast<int>(i - 1);
                    CXCursor  arg = unwrap_expr(kids[i]);
                    // forward/move 包裹：实参是对单参调用、其内层 DeclRef 是
                    // 形参时剥到内层（libc++ 的 std::move(__comp) 形态）。
                    if (cg::clang::kind(arg) == CXCursor_CallExpr)
                    {
                        const std::vector<CXCursor> ak = direct_children(arg);
                        if (ak.size() == 2)
                        {
                            const CXCursor inner = unwrap_expr(ak.back());
                            if (cg::clang::kind(inner) == CXCursor_DeclRefExpr
                                && cg::clang::kind(cg::clang::referenced(inner))
                                       == CXCursor_ParmDecl)
                            {
                                arg = inner;
                            }
                        }
                    }
                    if (cg::clang::kind(arg) != CXCursor_DeclRefExpr)
                    {
                        continue;
                    }
                    const CXCursor aref = cg::clang::referenced(arg);
                    if (cg::clang::kind(aref) != CXCursor_ParmDecl)
                    {
                        continue;
                    }
                    const auto it = params.find(cg::clang::spelling(aref));
                    if (it == params.end())
                    {
                        continue;
                    }
                    out.external_param_args.push_back(
                        {.host_usr = sub_usr,
                         .param_index = ordinal,
                         .caller_key = {},
                         .target_key =
                             std::format("ext#{}#{}", host_usr, it->second),
                         .file = site_file,
                         .line = site_line,
                         .col = site_col});
                    any_forwarded = true;
                }
                // 体内递归与占位转发解绑——callee 是界外实例定义即递归
                // （stdexec 深链构造用的是就地 lambda/复杂实参而非形参直传，
                // "实参必须形参"的窄判据只约束占位（防误连），不应挡住体内
                // invoke/def 采集（field 槽/就地 lambda 目标都在体内深处）。
                // 爆炸面由 ext_host_seen_（每 host 一次）与 budget 兜底。
                external_instance_body(ref, entry_caller);
            }

            /**
             * @brief 透传包装登记：类模板命中透传 getter 判据时登记
             *        wrapper/getter/实参下标（消费侧透传闸挂接，本登记零消费）。
             */
            void passthrough_wrapper_decl(const CXCursor& cls)
            {
                const std::string file = norm(cg::clang::location(cls));
                if (!under_roots(file, opt))
                {
                    return;
                }
                if (!opt.generated_root.empty()
                    && file.starts_with(opt.generated_root))
                {
                    return;
                }
                int own_count = 0;
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch)
                            == CXCursor_TemplateTypeParameter)
                        {
                            ++own_count;
                        }
                    });
                if (own_count == 0)
                {
                    return;
                }
                const std::string wrapper_key = make_record_key(
                    scope_of(cls),
                    cg::clang::spelling(cls),
                    file);
                for_each_child(
                    cls,
                    [&](const CXCursor& ch)
                    {
                        if (cg::clang::kind(ch) != CXCursor_CXXMethod)
                        {
                            return;
                        }
                        const int arg = is_passthrough_getter(ch, own_count);
                        if (arg < 0)
                        {
                            return;
                        }
                        const cg::clang::SourceLocation loc =
                            cg::clang::location(ch);
                        const std::string mfile = norm(loc);
                        const std::string getter_key =
                            function_key_of(ch, mfile);
                        if (!passthrough_seen_.insert(getter_key).second)
                        {
                            return;
                        }
                        out.passthrough_wrappers.push_back(
                            {.wrapper_key = wrapper_key,
                             .getter_key = getter_key,
                             .getter_name = cg::clang::spelling(ch),
                             .file = mfile,
                             .line = static_cast<int>(loc.line),
                             .arg_index = arg});
                    });
            }

            void visit(const CXCursor& c, Ctx& ctx)
            {
                const CXCursorKind k = cg::clang::kind(c);
                switch (k)
                {
                case CXCursor_FunctionDecl:
                case CXCursor_FunctionTemplate:
                case CXCursor_CXXMethod:
                case CXCursor_Constructor:
                case CXCursor_Destructor:
                    function(c);
                    return;
                case CXCursor_VarDecl:
                    local_var(c, ctx);
                    // 初始化表达式内的调用仍需收集。
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { visit(ch, ctx); });
                    return;
                case CXCursor_ParmDecl:
                    parameter(c, ctx);
                    return;
                case CXCursor_FieldDecl:
                    field_decl(c);
                    return;
                case CXCursor_ClassTemplate:
                    passthrough_wrapper_decl(c);
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { visit(ch, ctx); });
                    return;
                case CXCursor_BinaryOperator:
                    assignment(c, ctx);
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { visit(ch, ctx); });
                    return;
                case CXCursor_UnaryOperator:
                    address_of_evidence(c, ctx);
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { visit(ch, ctx); });
                    return;
                case CXCursor_ReturnStmt:
                    return_stmt(c, ctx);
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { visit(ch, ctx); });
                    return;
                case CXCursor_CXXForRangeStmt:
                {
                    // range-for 是独立语句种类（kind 225）：落在 default 会静默
                    // 递归、链接行永不发射（预研 v1 的 no-op 教训），必须显式
                    // 接管。循环变量（首子 VarDecl）不递归——其动态证据一律由
                    // 链接行继承，值绑定的拷贝构造形态（aggregate/静态行）不是
                    // 元素证据，混入会把唯一的元素证据稀释成槽级并集 Low。
                    for_range_stmt(c, ctx);
                    const std::vector<CXCursor> range_kids = direct_children(c);
                    for (std::size_t i = 1; i < range_kids.size(); ++i)
                    {
                        visit(range_kids[i], ctx);
                    }
                    return;
                }
                case CXCursor_CallExpr:
                    call_site(c, ctx);
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { visit(ch, ctx); });
                    return;
                case CXCursor_LambdaExpr:
                {
                    // lambda 登记（def 事件）：体内语句仍按外层函数语境采集
                    // （首版语义：lambda 体的调用点算外层函数的调用点）。
                    const cg::clang::SourceLocation loc =
                        cg::clang::location(c);
                    if (under_roots(norm(loc), opt))
                    {
                        (void)lambda_key_of(loc);
                    }
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { visit(ch, ctx); });
                    return;
                }
                default:
                    for_each_child(
                        c,
                        [&](const CXCursor& ch) { visit(ch, ctx); });
                    return;
                }
            }

            void run(const CXCursor& root)
            {
                Ctx ctx;
                for_each_child(
                    root,
                    [&](const CXCursor& ch) { visit(ch, ctx); });
                std::ranges::stable_sort(
                    out.hints,
                    [](const TypeHintRec& a, const TypeHintRec& b)
                    {
                        return std::tie(
                                   a.owner_key,
                                   a.var_name,
                                   a.file,
                                   a.line,
                                   a.col,
                                   a.source)
                               < std::tie(
                                   b.owner_key,
                                   b.var_name,
                                   b.file,
                                   b.line,
                                   b.col,
                                   b.source);
                    });
                std::ranges::stable_sort(
                    out.call_sites,
                    [](const CallSiteRec& a, const CallSiteRec& b)
                    {
                        return std::tie(
                                   a.caller_key,
                                   a.file,
                                   a.line,
                                   a.col,
                                   a.callee.name)
                               < std::tie(
                                   b.caller_key,
                                   b.file,
                                   b.line,
                                   b.col,
                                   b.callee.name);
                    });
                std::ranges::stable_sort(
                    out.passthrough_wrappers,
                    [](const PassthroughWrapperRec& a,
                       const PassthroughWrapperRec& b)
                    {
                        return std::tie(a.wrapper_key, a.getter_key)
                               < std::tie(b.wrapper_key, b.getter_key);
                    });
            }
        };

    } // namespace collect_detail

    inline auto collect(
        const cg::clang::ParsedTu& parsed,
        const CollectOptions&      opt,
        cg::util::PathCaseCache&   case_cache) -> TuAnalysis
    {
        collect_detail::Walker walker{opt, case_cache};
        if (parsed.tu.valid())
        {
            walker.run(parsed.tu.cursor());
        }
        walker.out.main_file = parsed.tu.file();
        return std::move(walker.out);
    }

} // namespace cg::analysis
