module;
#include "../src/analysis/symbits.kernels.h"
#include "../src/db/db.tables.h"

export module cg.test.analysis;

// 动态派发分析（schema v11）：类型线索采集、函数内传播、
// 全局不动点、三个具名 demo 夹具，以及两次全新
// 索引之间字节级一致的确定性。

import std;
import cg.util;
import cg.util.platform;
import cg.clang;
import cg.db;
import cg.graph;
import cg.analysis;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace db = cg::db;

    constexpr std::string_view fixture_root = CCG_DISPATCH_FIXTURE_DIR;

    [[nodiscard]]
    auto fixture(std::string_view name) -> std::string
    {
        return cg::util::norm_path(
            std::string{fixture_root} + "/" + std::string{name});
    }

    [[nodiscard]]
    auto index_fixture(cg::db::GraphStore& store, std::string_view name)
        -> cg::graph::IndexSummary
    {
        const std::string       root = fixture(name);
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.force = true;
        opt.progress = false;
        opt.compile_db_path = root + "/compile_commands.json";
        cg::graph::Indexer indexer{store, std::move(opt)};
        return indexer.run(
            cg::clang::load(root + "/compile_commands.json", {}));
    }

    struct DEdge
    {
        cg::db::SymId src = 0;
        cg::db::SymId dst = 0;
        double        confidence = 0.0;
        std::uint32_t attrs = 0;
        int           line = 0;
        std::string   src_name;
        std::string   dst_name;
    };

    [[nodiscard]]
    auto dispatch_edges(cg::db::GraphStore& store) -> std::vector<DEdge>
    {
        std::vector<DEdge> out;
        for (const cg::db::EdgeRow& e : cg::db::all_edges(store))
        {
            if (e.type != cg::db::EdgeType::VirtualDispatch)
            {
                continue;
            }
            const auto src = cg::db::get_symbol(store, e.src);
            const auto dst = cg::db::get_symbol(store, e.dst);
            out.push_back(
                {.src = e.src,
                 .dst = e.dst,
                 .confidence = e.confidence,
                 .attrs = e.attrs,
                 .line = e.site_line,
                 .src_name = src ? src->qualified_name : "?",
                 .dst_name = dst ? dst->qualified_name : "?"});
        }
        return out;
    }

    [[nodiscard]]
    auto find_edge(
        const std::vector<DEdge>& edges,
        std::string_view          src,
        std::string_view          dst) -> const DEdge*
    {
        for (const DEdge& e : edges)
        {
            if (e.src_name == src && e.dst_name == dst)
            {
                return &e;
            }
        }
        return nullptr;
    }

    /**
     * @brief 观察站的结构判据助手：caller 全部派发推断边的置信上限。
     *
     * @internal 与 reason 字面匹配互补的负形总闸——任何分支（既有或
     *           未来新增）在观察站上产出 High(1.0) 直连边都会被抓，
     *           reason 换路径迁移不再穿透断言。
     */
    [[nodiscard]]
    auto top_inferred_confidence(
        const std::vector<DEdge>& edges,
        std::string_view          src) -> double
    {
        double top = 0.0;
        for (const DEdge& e : edges)
        {
            if (e.src_name == src && e.confidence > top)
            {
                top = e.confidence;
            }
        }
        return top;
    }

    [[nodiscard]]
    auto hint_has(
        cg::db::GraphStore& store,
        int                 source,
        std::string_view    needle) -> bool
    {
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            if (row.source == source
                && row.type_set.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    auto site_of(
        cg::db::GraphStore& store,
        std::string_view    caller,
        std::string_view    callee,
        int                 line) -> std::optional<cg::db::CallSiteRow>
    {
        for (const cg::db::CallSiteRow& row : store.all_call_sites())
        {
            const auto c = cg::db::get_symbol(store, row.caller_id);
            if (c && c->qualified_name == caller && row.callee_name == callee
                && row.line == line)
            {
                return row;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]]
    auto site_named(
        cg::db::GraphStore& store,
        std::string_view    caller,
        std::string_view    callee) -> std::optional<cg::db::CallSiteRow>
    {
        for (const cg::db::CallSiteRow& row : store.all_call_sites())
        {
            const auto c = cg::db::get_symbol(store, row.caller_id);
            if (c && c->qualified_name == caller && row.callee_name == callee)
            {
                return row;
            }
        }
        return std::nullopt;
    }

    // --------------------------------------------------------- 采集层线索 ----

    void l0_direct_new(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l0");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge*             e = find_edge(edges, "probes", "Impl::f");
        cg::test::check(r, e != nullptr, "direct new reaches Impl::f");
        if (e != nullptr)
        {
            cg::test::check(
                r,
                e->confidence == 1.0,
                "direct new is high confidence (1.0)");
            cg::test::check(
                r,
                (e->attrs & cg::db::edge_attrs::dispatch_inferred) != 0,
                "inferred dispatch attrs");
            cg::test::check(
                r,
                (e->attrs & cg::db::edge_attrs::dispatch_fallback) == 0,
                "not a fallback edge");
        }
        cg::test::check(
            r,
            hint_has(
                store,
                static_cast<int>(cg::analysis::HintSource::DirectNew),
                "Impl"),
            "direct_new type hint records Impl");
    }

    void l0_cast_down(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l0");
        const std::vector<DEdge> edges = dispatch_edges(store);
        bool                     medium = false;
        for (const DEdge& e : edges)
        {
            if (e.src_name == "probes" && e.dst_name == "Impl::f"
                && e.confidence == 0.7)
            {
                medium = true;
            }
        }
        cg::test::check(
            r,
            medium,
            "static_cast<Impl*> clue yields a medium (0.7) edge");
        cg::test::check(
            r,
            hint_has(
                store,
                static_cast<int>(cg::analysis::HintSource::CastDown),
                "Impl"),
            "cast_down hint records the cast target");
        bool low_downcast = false;
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            if (row.source
                    == static_cast<int>(cg::analysis::HintSource::CastDown)
                && row.var_name == "d" && row.confidence == 2)
            {
                low_downcast = true;
            }
        }
        cg::test::check(
            r,
            low_downcast,
            "provable downcast target starts as a low candidate");
    }

    void l0_other_sources(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l0");
        cg::test::check(
            r,
            hint_has(
                store,
                static_cast<int>(cg::analysis::HintSource::MakeUnique),
                "Impl"),
            "make_unique<T> clue records T");
        cg::test::check(
            r,
            hint_has(
                store,
                static_cast<int>(cg::analysis::HintSource::DirectAggregate),
                "Obj"),
            "aggregate initialization clue records Obj");
        cg::test::check(
            r,
            hint_has(
                store,
                static_cast<int>(cg::analysis::HintSource::AddressOfObject),
                "Obj"),
            "&object clue records the object type");
    }

    // --------------------------------------------------------- 函数内流
    // --------

    void l1_alias(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l1");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge* e = find_edge(dw_edges, "local_alias", "Impl::f");
        cg::test::check(r, e != nullptr, "alias propagates to Impl::f");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "single local alias is medium (0.7)");
    }

    void l1_conditional(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l1");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge*             a = find_edge(edges, "conditional", "Impl::f");
        const DEdge* b = find_edge(edges, "conditional", "Other::f");
        cg::test::check(
            r,
            a != nullptr && b != nullptr,
            "conditional yields both dynamic candidates");
        cg::test::check(
            r,
            a != nullptr && a->confidence == 0.4 && b != nullptr
                && b->confidence == 0.4,
            "multiple candidates are low confidence (0.4)");
        // 按 caller+callee 定位该函数体内唯一的 a->f() 调用点，不与夹具
        // 绝对行号耦合（conditional 内的调用点随夹具内容增删会移位）。
        const auto site = site_named(store, "conditional", "f");
        cg::test::check(
            r,
            site.has_value()
                && site->candidates.find("conditional_merge")
                       != std::string::npos,
            "conditional multi-type slot reports conditional_merge");
    }

    void l1_return_flow(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l1");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge* e = find_edge(dw_edges, "from_return", "Impl::f");
        cg::test::check(r, e != nullptr, "return-slot flow reaches Impl::f");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "return flow is medium (0.7)");
    }

    // ------------------------------------------------------------ 全局流
    // --------

    void l2_param_flow(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l2");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "take1", "Impl1::f");
        cg::test::check(r, e != nullptr, "L2 argument -> parameter flow");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "single caller parameter flow is medium (0.7)");
    }

    void l2_return_flow(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l2");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "main", "Impl1::f");
        cg::test::check(
            r,
            e != nullptr,
            "L2 cross-TU return flow reaches Impl1::f");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "cross-TU return flow is medium (0.7)");
    }

    void l2_multi_caller(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "l2");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge*             a = find_edge(edges, "take2", "Impl1::f");
        const DEdge*             b = find_edge(edges, "take2", "Impl2::f");
        cg::test::check(
            r,
            a != nullptr && b != nullptr,
            "two callers with different dynamic types keep both");
        cg::test::check(
            r,
            a != nullptr && b != nullptr && a->confidence == 0.4
                && b->confidence == 0.4,
            "multiple dynamic types are low confidence (0.4)");
        // 调用点本身必须公布它的候选集。
        const auto site = site_of(store, "take2", "f", 7);
        cg::test::check(
            r,
            site.has_value() && site->resolved,
            "take2 call site is resolved");
        cg::test::check(
            r,
            site.has_value()
                && site->candidates.find("|Impl1|f|") != std::string::npos
                && site->candidates.find("|Impl2|f|") != std::string::npos,
            "call_sites.candidates keeps both targets");
    }

    [[nodiscard]]
    auto site_for(
        cg::db::GraphStore& store,
        std::string_view    caller,
        std::string_view    callee,
        bool                want_virtual) -> std::optional<cg::db::CallSiteRow>
    {
        for (const cg::db::CallSiteRow& row : store.all_call_sites())
        {
            const auto c = cg::db::get_symbol(store, row.caller_id);
            if (c && c->qualified_name == caller && row.callee_name == callee
                && row.is_virtual == want_virtual)
            {
                return row;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]]
    auto symbol_id(cg::db::GraphStore& store, std::string_view qualified)
        -> cg::db::SymId
    {
        const auto found = cg::db::find_symbols(store, qualified, {});
        return found.matches.empty() ? 0 : found.matches.front().id;
    }

    /** @brief 动态证据必须只来自对象创建/转型/取地址，绝不来自静态容器元素。 */
    void container_element_is_not_dynamic(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "rangefor");
        const std::vector<DEdge> edges = dispatch_edges(store);
        bool                     inferred = false;
        const DEdge*             marker = nullptr;
        for (const DEdge& e : edges)
        {
            if (e.confidence > 0.0)
            {
                inferred = true;
            }
            if (e.src_name == "main" && e.dst_name == "Base::f")
            {
                marker = &e;
            }
        }
        cg::test::check(
            r,
            !inferred,
            "range-for element static type never becomes inferred evidence");
        cg::test::check(
            r,
            marker != nullptr && marker->confidence == 0.0
                && (marker->attrs & cg::db::edge_attrs::dispatch_fallback) != 0
                && (marker->attrs & cg::db::edge_attrs::dispatch_inferred) == 0,
            "range-for yields one family-level fallback marker");
        const auto site = site_for(store, "main", "f", true);
        cg::test::check(
            r,
            site.has_value()
                && site->candidates.find("|D1|f|") != std::string::npos
                && site->candidates.find("|D2|f|") != std::string::npos,
            "range-for candidate set keeps both implementations");
    }

    /** @brief `set(make())` 的实参经 #ret 槽传播，不把声明返回类型当证据。 */
    void call_argument_return_slot(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "callarg");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge*             e = find_edge(edges, "set", "Impl::f");
        cg::test::check(
            r,
            e != nullptr,
            "set(make()) resolves through the callee return slot");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "return-slot argument flow is medium (0.7)");
        for (const DEdge& candidate : edges)
        {
            cg::test::check(
                r,
                !(candidate.src_name == "set" && candidate.dst_name == "I::f"
                  && candidate.confidence > 0.0),
                "declaration return type I* is never a medium dynamic target");
        }
    }

    /** @brief 变量实参传播（schema v11 的 call_sites.arg_vars）。 */
    void argument_variable_flow(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "vararg");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "set", "Impl::f");
        cg::test::check(
            r,
            e != nullptr,
            "set(p) propagates p's dynamic type across TUs");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "variable argument flow is medium (0.7)");
    }

    /** @brief 跨 TU 返回值 -> 局部变量 -> 实参 -> 形参 的完整链。 */
    void return_value_via_parameter(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "retflow2");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "set", "Impl::f");
        cg::test::check(
            r,
            e != nullptr,
            "make() -> q -> set(q) crosses TUs to Impl::f");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "cross-TU return/variable flow is medium (0.7)");
    }

    /** @brief 每个动态类型独立求最近覆盖：混合 override 不丢基类候选。 */
    void mixed_override_nearest(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "mixedoverride");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge*             d1 = find_edge(edges, "use", "D1::f");
        const DEdge*             base = find_edge(edges, "use", "Base::f");
        cg::test::check(
            r,
            d1 != nullptr,
            "overriding dynamic type reaches D1::f");
        cg::test::check(
            r,
            base != nullptr,
            "non-overriding dynamic type reaches Base::f (no lost candidate)");
        cg::test::check(
            r,
            d1 != nullptr && base != nullptr && d1->confidence == 0.4
                && base->confidence == 0.4,
            "both mixed-override candidates are low confidence (0.4)");
    }

    /** @brief 函数指针/成员指针调用必须留行并给出可读 fallback 原因。 */
    void indirect_call_is_reported(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "fptr");
        std::optional<cg::db::CallSiteRow> indirect;
        for (const cg::db::CallSiteRow& row : store.all_call_sites())
        {
            const auto c = cg::db::get_symbol(store, row.caller_id);
            if (c && c->qualified_name == "g" && row.indirect)
            {
                indirect = row;
            }
        }
        cg::test::check(
            r,
            indirect.has_value(),
            "member-pointer call leaves a call_sites row");
        cg::test::check(
            r,
            indirect.has_value() && !indirect->resolved
                && !indirect->callee_resolved
                && indirect->fallback_reason == "indirect_call",
            "indirect call reports reason=indirect_call");
        for (const DEdge& e : dispatch_edges(store))
        {
            cg::test::check(
                r,
                e.src_name != "g",
                "indirect call never fabricates a dispatch target");
        }
    }

    /** @brief 智能指针接收者剥掉 operator-> 后绑定到 make_unique 线索。 */
    void smart_pointer_receiver(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "smart");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "main", "Impl::f");
        cg::test::check(
            r,
            e != nullptr,
            "unique_ptr receiver resolves to Impl::f");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "make_unique evidence is medium (0.7)");
    }

    /** @brief 字段赋值：this->field 由记录所有者槽解析。 */
    void field_assignment_flow(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "field");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "S::run", "Impl::f");
        cg::test::check(
            r,
            e != nullptr,
            "field assignment in bind() reaches run()'s call site");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "field flow is medium (0.7)");
        const auto site = site_for(store, "S::run", "f", true);
        cg::test::check(
            r,
            site.has_value() && site->receiver_is_this
                && site->receiver_name == "p",
            "implicit this->field receiver is reported as the field");
    }

    /**
     * @brief 函数体字段工厂赋值：标准库智能指针成员在成员函数体内被
     *        make_unique 赋值（标准库壳不满足指针式壳直属 T* 字段判据），
     *        模板实参 pointee 证据进字段槽，另一成员函数的虚站点决议。
     */
    void field_factory_assignment(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "field-factory");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "S::run", "Impl::f");
        cg::test::check(
            r,
            e != nullptr,
            "setup()'s make_unique field write reaches run()'s site");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "factory field flow is medium (0.7)");
        const DEdge* mp = find_edge(dw_edges, "Multi::run", "Impl::f");
        const DEdge* mq = find_edge(dw_edges, "Multi::run", "Impl2::f");
        cg::test::check(
            r,
            mp != nullptr && mq != nullptr && mp->confidence == 0.7
                && mq->confidence == 0.7,
            "two factory-assigned fields keep separate slot evidence");
        const auto ambi = site_for(store, "Ambi::run", "f", true);
        bool       ambi_dual_low = false;
        if (ambi.has_value())
        {
            // 多候选族口径：两条 low 档候选并列（make_unique_multiple），
            // 决议不得单选升 medium。
            const std::string& text = ambi->candidates;
            std::size_t        hits = 0;
            for (std::size_t pos = 0;
                 (pos = text.find("make_unique_multiple", pos))
                 != std::string::npos;
                 pos += 21)
            {
                ++hits;
            }
            ambi_dual_low = hits == 2 && ambi->confidence >= 2;
        }
        cg::test::check(
            r,
            ambi_dual_low,
            "dual factory assignment merges to two low candidates, "
            "never a medium single pick");
        const auto null_site = site_for(store, "Null::run", "f", true);
        cg::test::check(
            r,
            null_site.has_value() && !null_site->resolved
                && (null_site->receiver_types.empty()
                    || null_site->receiver_types == "[]"),
            "non-factory RHS (nullptr) yields no fabricated evidence");
        const DEdge* local = find_edge(dw_edges, "local_late", "Impl::f");
        cg::test::check(
            r,
            local != nullptr && local->confidence == 0.7,
            "late-declared local unique_ptr assignment is covered too");
        const DEdge* shared = find_edge(dw_edges, "Shared::run", "Impl::f");
        cg::test::check(
            r,
            shared != nullptr && shared->confidence == 0.7,
            "make_shared field factory assignment is covered equally");
    }

    /** @brief 巨型覆盖族：fallback 每站点一条边，候选集封顶且显式标记。 */
    void fallback_is_family_level(R& r)
    {
        db::GraphStore                store{":memory:"};
        const cg::graph::IndexSummary sum = index_fixture(store, "wide");
        const std::vector<DEdge>      edges = dispatch_edges(store);
        cg::test::check(
            r,
            edges.size() == 1,
            "one fallback marker edge for one unresolved site");
        if (!edges.empty())
        {
            cg::test::check(
                r,
                edges.front().confidence == 0.0
                    && (edges.front().attrs
                        & cg::db::edge_attrs::dispatch_fallback)
                           != 0,
                "fallback marker is 0.0 with dispatch_fallback");
        }
        const auto  site = site_for(store, "use", "f", true);
        std::size_t candidate_count = 0;
        if (site.has_value())
        {
            const std::string& text = site->candidates;
            for (std::size_t pos = 0;
                 (pos = text.find("\"target\"", pos)) != std::string::npos;
                 pos += 8)
            {
                ++candidate_count;
            }
        }
        cg::test::check(
            r,
            site.has_value() && !site->resolved,
            "wide family site stays unresolved");
        cg::test::check(
            r,
            candidate_count > 1 && candidate_count <= 64,
            "candidate set keeps a bounded multiplicity (<= 64)");
        cg::test::check(
            r,
            site.has_value()
                && site->fallback_reason.find("candidates_truncated")
                       != std::string::npos,
            "candidate truncation is explicit in fallback_reason");
        cg::test::check(
            r,
            sum.analysis_rounds >= 1,
            "fixed-point rounds are observable");
        const auto rounds_meta = store.meta("analysis_rounds");
        const auto work_meta = store.meta("analysis_work_items");
        cg::test::check(
            r,
            rounds_meta.has_value() && std::stoll(*rounds_meta) >= 1
                && work_meta.has_value(),
            "fixed-point counters are persisted for ccg stats");
    }

    /** @brief call_sites.resolved 是 dispatch 解析；callee_resolved 单独上报。
     */
    void call_site_resolution_split(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "callarg");
        const auto virtual_site = site_for(store, "set", "f", true);
        cg::test::check(
            r,
            virtual_site.has_value() && virtual_site->resolved
                && virtual_site->callee_resolved,
            "resolved virtual site reports both resolutions");
        const auto plain = site_for(store, "main", "set", false);
        cg::test::check(
            r,
            plain.has_value() && !plain->resolved && plain->callee_resolved
                && plain->fallback_reason == "not_virtual",
            "non-virtual call has callee_resolved=1, dispatch resolved=0");
    }

    /** @brief --dispatch 走查把同目标的 CALLS 与 dispatch 合并，不丢信息。 */
    void traverse_merges_dispatch(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "rangefor");
        const cg::db::SymId main_id = symbol_id(store, "main");
        cg::test::check(r, main_id != 0, "main symbol present");
        const cg::graph::NeighborsResult plain = cg::graph::callees(
            store,
            main_id,
            1,
            true,
            cg::graph::DispatchMode::Off);
        const cg::graph::NeighborsResult dispatched = cg::graph::callees(
            store,
            main_id,
            1,
            true,
            cg::graph::DispatchMode::Additive);
        const auto find_entry =
            [](const cg::graph::NeighborsResult& result,
               std::string_view                  name,
               cg::db::GraphStore& st) -> const cg::graph::NeighborEntry*
        {
            for (const auto& entry : result.entries)
            {
                const auto row = cg::db::get_symbol(st, entry.id);
                if (row && row->qualified_name == name)
                {
                    return &entry;
                }
            }
            return nullptr;
        };
        const auto* plain_entry = find_entry(plain, "Base::f", store);
        const auto* merged = find_entry(dispatched, "Base::f", store);
        cg::test::check(
            r,
            plain_entry != nullptr && !plain_entry->has_dispatch,
            "default walk keeps dispatch out");
        cg::test::check(
            r,
            merged != nullptr && merged->has_dispatch
                && merged->dispatch_confidence == 0.0
                && (merged->dispatch_attrs
                    & cg::db::edge_attrs::dispatch_fallback)
                       != 0,
            "--dispatch merge keeps confidence/fallback on the CALLS destination");
        const cg::graph::ContextView view = cg::graph::context(
            store,
            main_id,
            false,
            cg::graph::DispatchMode::Additive);
        bool context_dispatch = false;
        for (const auto& edge : view.outgoing)
        {
            if (edge.edge.type == cg::db::EdgeType::VirtualDispatch
                && edge.other.qualified_name == "Base::f"
                && (edge.edge.attrs & cg::db::edge_attrs::dispatch_fallback)
                       != 0)
            {
                context_dispatch = true;
            }
        }
        cg::test::check(
            r,
            context_dispatch,
            "context --dispatch exposes fallback attrs");
    }

    /**
     * @brief 默认 B 精确模式：0.4 双实现候选可见、兄弟目标不互拉、
     *        基类仍保留全部 override 调用者，Off 回到纯静态视图。
     */
    void precise_dispatch_default(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "demo3");
        const cg::db::SymId base = symbol_id(store, "Base::f");
        const cg::db::SymId d1 = symbol_id(store, "D1::f");
        const cg::db::SymId d2 = symbol_id(store, "D2::f");
        cg::test::check(
            r,
            base != 0 && d1 != 0 && d2 != 0,
            "demo3 symbols present");
        const auto entry_for =
            [](const cg::graph::NeighborsResult& result,
               cg::db::SymId id) -> const cg::graph::NeighborEntry*
        {
            for (const auto& entry : result.entries)
            {
                if (entry.id == id)
                {
                    return &entry;
                }
            }
            return nullptr;
        };
        const cg::db::SymId use = symbol_id(store, "use");
        cg::test::check(r, use != 0, "use symbol present");

        // 具体实现 D1 / D2：唯一调用者 use 的 0.4 候选低于 0.7 证据阈值，
        // 默认按整个 override 家族估算，不冒充具体动态目标。
        for (const cg::db::SymId impl : {d1, d2})
        {
            const cg::graph::NeighborsResult precise =
                cg::graph::callers(store, impl, 1);
            const auto* e = entry_for(precise, use);
            cg::test::check(
                r,
                e != nullptr
                    && e->dispatch_status
                           == cg::graph::DispatchStatus::FamilyEstimate
                    && !e->has_dispatch,
                "0.4 weak inference stays a family estimate");
            const cg::graph::NeighborsResult off = cg::graph::callers(
                store,
                impl,
                1,
                true,
                cg::graph::DispatchMode::Off);
            // Off 是家族合并的保守上界：use 仍会经 Base 的静态调用出现，
            // 但没有派发证据，状态必须是 none。
            const auto* off_entry = entry_for(off, use);
            cg::test::check(
                r,
                off_entry != nullptr
                    && off_entry->dispatch_status
                           == cg::graph::DispatchStatus::None
                    && !off_entry->has_dispatch,
                "Off keeps the family upper bound without dispatch evidence");
        }

        // 基类：0.4 弱推断不替换静态调用关系；use 仍是调用者，但
        // 不携带派发证据（不是 inferred）。
        const cg::graph::NeighborsResult base_callers =
            cg::graph::callers(store, base, 1);
        const auto* base_entry = entry_for(base_callers, use);
        cg::test::check(
            r,
            base_entry != nullptr
                && base_entry->dispatch_status
                       == cg::graph::DispatchStatus::None
                && !base_entry->has_dispatch,
            "base query keeps the static caller without a 0.4 inferred target");

        // trace：main -> use 是静态 calls，use -> D1 只有 0.4 弱推断；
        // 低于 0.7 阈值，默认不参与寻路。
        const cg::db::SymId          main_id = symbol_id(store, "main");
        const cg::graph::TraceResult precise_trace =
            cg::graph::trace(store, main_id, d1, 10);
        cg::test::check(
            r,
            !precise_trace.found,
            "trace does not follow a 0.4 weak-inference hop");
        bool saw_dispatch_hop = false;
        for (const cg::graph::TraceHop& hop : precise_trace.hops)
        {
            if (hop.via == cg::db::EdgeType::VirtualDispatch)
            {
                saw_dispatch_hop = true;
            }
        }
        cg::test::check(r, !saw_dispatch_hop, "trace has no 0.4 dispatch hop");
        const cg::graph::TraceResult off_trace = cg::graph::trace(
            store,
            main_id,
            d1,
            10,
            cg::graph::DispatchMode::Off);
        cg::test::check(r, !off_trace.found, "Off trace has no dispatch paths");
    }

    /** @brief changes 默认把正面派发调用者算作消费者，Off 不包含。 */
    void changes_include_dispatch_callers(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "demo2");
        const cg::db::SymId impl = symbol_id(store, "Impl::f");
        const cg::db::SymId set = symbol_id(store, "set");
        cg::test::check(r, impl != 0 && set != 0, "demo2 symbols present");
        const std::array<cg::db::SymId, 1> seeds{impl};
        const cg::graph::ChangesResult     precise =
            cg::graph::changes(store, seeds, 2);
        bool saw_set = false;
        for (const cg::graph::ChangesEntry& e : precise.consumers)
        {
            if (e.id == set
                && e.dispatch_status == cg::graph::DispatchStatus::Inferred)
            {
                saw_set = true;
            }
        }
        cg::test::check(
            r,
            saw_set,
            "changes lists the cross-TU dispatch caller");
        const cg::graph::ChangesResult off =
            cg::graph::changes(store, seeds, 2, cg::graph::DispatchMode::Off);
        bool off_set = false;
        bool off_set_inferred = false;
        for (const cg::graph::ChangesEntry& e : off.consumers)
        {
            if (e.id == set)
            {
                off_set = true;
                off_set_inferred =
                    e.dispatch_status == cg::graph::DispatchStatus::Inferred;
            }
        }
        // Off 仍可经 override 家族在更深的静态路径上看到 set，但绝不带
        // dispatch 证据；精确模式把它提升到深度 1 的 inferred。
        cg::test::check(
            r,
            off_set,
            "Off changes still reaches set via the override family");
        cg::test::check(
            r,
            !off_set_inferred,
            "Off changes never reports a dispatch-inferred caller");
        bool precise_shallow = false;
        for (const cg::graph::ChangesEntry& e : precise.consumers)
        {
            if (e.id == set && e.depth == 1
                && e.dispatch_status == cg::graph::DispatchStatus::Inferred)
            {
                precise_shallow = true;
            }
        }
        cg::test::check(
            r,
            precise_shallow,
            "precise changes finds the dispatch caller at depth 1");
    }

    /** @brief 同一语料两次默认查询逐字段一致（状态与置信度确定）。 */
    void precise_dispatch_deterministic(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "demo3");
        const auto dump = [&store]()
        {
            std::string out;
            for (const cg::db::SymId id :
                 {symbol_id(store, "Base::f"),
                  symbol_id(store, "D1::f"),
                  symbol_id(store, "D2::f")})
            {
                const cg::graph::NeighborsResult res =
                    cg::graph::callers(store, id, 2);
                for (const auto& e : res.entries)
                {
                    out += std::format(
                        "{}|{}|{}|{}|{:.2f}|{}\n",
                        id,
                        e.id,
                        e.depth,
                        static_cast<int>(e.dispatch_status),
                        e.dispatch_confidence,
                        e.family_origin);
                }
            }
            return out;
        };
        cg::test::check(
            r,
            dump() == dump(),
            "two precise walks are byte-identical");
    }

    /** @brief 普通函数指针三种形态都必须留间接调用行，绝不静默。 */
    void function_pointer_variants(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "fptr_variants");
        std::map<std::string, cg::db::CallSiteRow> indirect;
        for (const cg::db::CallSiteRow& row : store.all_call_sites())
        {
            if (!row.indirect)
            {
                continue;
            }
            const auto c = cg::db::get_symbol(store, row.caller_id);
            if (c)
            {
                indirect.emplace(c->qualified_name, row);
            }
        }
        const auto local = indirect.find("main");
        const auto param = indirect.find("use_param");
        const auto member = indirect.find("S::run");
        cg::test::check(
            r,
            local != indirect.end(),
            "local function-pointer call leaves a call_sites row");
        cg::test::check(
            r,
            param != indirect.end(),
            "function-pointer parameter call leaves a row");
        cg::test::check(
            r,
            member != indirect.end(),
            "field function-pointer call leaves a row");
        for (const auto& [name, row] : indirect)
        {
            (void)name;
            cg::test::check(
                r,
                row.fallback_reason == "indirect_call" && !row.resolved
                    && !row.callee_resolved,
                "each function-pointer site reports indirect_call");
        }
        cg::test::check(
            r,
            dispatch_edges(store).empty(),
            "function-pointer calls never fabricate dispatch edges");
    }

    /**
     * @brief 被调用表达式本身是调用（容器下标 / 嵌套调用）时必须留间接行。
     *
     * @internal 对抗类：`a[0]()`、`v[0]()`（operator[]）与 `get()()` 的外层
     * 调用都调用内层表达式的结果，绝不能沿用内层被调用者，也不能静默。
     * 真正的运算符调用（`make_vec()[0]`）不受影响。
     */
    void nested_callee_indirect(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "fptr_container");
        const auto rows_for = [&](std::string_view caller)
        {
            std::vector<cg::db::CallSiteRow> out;
            for (const cg::db::CallSiteRow& row : store.all_call_sites())
            {
                const auto c = cg::db::get_symbol(store, row.caller_id);
                if (c && c->qualified_name == caller)
                {
                    out.push_back(row);
                }
            }
            return out;
        };
        for (const std::string_view caller :
             {"use_array", "use_vector", "use_nested"})
        {
            std::size_t indirect = 0;
            for (const cg::db::CallSiteRow& row : rows_for(caller))
            {
                if (!row.indirect)
                {
                    continue;
                }
                ++indirect;
                cg::test::check(
                    r,
                    row.fallback_reason == "indirect_call" && !row.resolved
                        && !row.callee_resolved,
                    "nested callee reports reason=indirect_call");
            }
            cg::test::check(
                r,
                indirect == 1,
                "nested callee leaves exactly one indirect row");
        }
        bool direct_get = false;
        for (const cg::db::CallSiteRow& row : rows_for("use_nested"))
        {
            if (!row.indirect && row.callee_name == "get")
            {
                direct_get = true;
            }
        }
        cg::test::check(
            r,
            direct_get,
            "inner get() keeps its own direct call_sites row");
        for (const cg::db::CallSiteRow& row : rows_for("use_operator"))
        {
            cg::test::check(
                r,
                !row.indirect,
                "genuine operator call is not an indirect call");
        }
        cg::test::check(
            r,
            dispatch_edges(store).empty(),
            "nested callee never fabricates dispatch edges");
        for (const cg::db::EdgeRow& e : cg::db::all_edges(store))
        {
            if (e.type != cg::db::EdgeType::Calls)
            {
                continue;
            }
            const auto src = cg::db::get_symbol(store, e.src);
            const auto dst = cg::db::get_symbol(store, e.dst);
            if (!src
                || (src->qualified_name != "use_array"
                    && src->qualified_name != "use_vector"
                    && src->qualified_name != "use_nested"))
            {
                continue;
            }
            cg::test::check(
                r,
                !(dst && dst->qualified_name == "g"),
                "nested callee never fabricates a call edge to g");
        }
    }

    /** @brief static_cast 包裹的指针调用/变量不得把转型目标当动态证据。 */
    void cast_wrapped_evidence(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "cast_wrap");
        const std::vector<DEdge> edges = dispatch_edges(store);
        for (const std::string_view caller : {"set_arg", "set_var", "use_init"})
        {
            const DEdge* e = find_edge(edges, caller, "Impl::f");
            cg::test::check(
                r,
                e != nullptr,
                "cast-wrapped value reaches Impl::f");
            cg::test::check(
                r,
                e != nullptr && e->confidence == 0.7,
                "cast-wrapped evidence is medium (0.7)");
        }
        for (const DEdge& e : edges)
        {
            cg::test::check(
                r,
                !(e.dst_name == "I::f" && e.confidence > 0.0),
                "cast target type is never positive dynamic evidence");
        }
    }

    /** @brief C 风格转换保留操作数证据、绝不采纳转换目标（SWIG 生成代码形态）。
     */
    void cstyle_cast_evidence(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "cstyle_cast");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge* e = find_edge(edges, "use_new_assign", "Impl::f");
        cg::test::check(
            r,
            e != nullptr,
            "C-style cast keeps the operand new-Impl evidence");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "cast-wrapped operand evidence is medium (0.7)");
        for (const DEdge& edge : edges)
        {
            cg::test::check(
                r,
                !(edge.dst_name == "I::f" && edge.confidence > 0.0),
                "C-style cast target is never positive dynamic evidence");
        }
    }

    /** @brief 依赖接收者的成员调用不建具名 calls 边（幻觉边修正）。 */
    void dependent_member_calls(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "depcall");
        bool phantom_kept = false; // 具体接收者的同名调用不受守卫影响
        bool helper_kept = false;  // 模板类隐式 this 调用保留（守卫不误杀）
        bool run_kept = false;     // 具体接收者调模板类方法保留
        for (const cg::db::CallSiteRow& row : store.all_call_sites())
        {
            const auto        caller = cg::db::get_symbol(store, row.caller_id);
            const std::string caller_name =
                caller ? caller->qualified_name : "?";
            if (row.callee_name == "error_code")
            {
                // 同名成员在 TU 内可见（幻觉条件）：依赖接收者的模板 body
                // 不得产出具名行；唯一合法行是 main 里的具体接收者调用。
                cg::test::check(
                    r,
                    caller_name == "main",
                    "dependent-receiver error_code never leaves a named site");
            }
            if (row.callee_name == "go"
                && caller_name.find("run") != std::string::npos)
            {
                cg::test::check(
                    r,
                    false,
                    "dependent receiver t.go() leaves no call site");
            }
            if (caller_name == "main" && row.callee_name == "error_code")
            {
                phantom_kept = true;
            }
            if (row.callee_name == "helper")
            {
                helper_kept = true;
            }
            if (caller_name == "use_all" && row.callee_name == "run")
            {
                run_kept = true;
            }
        }
        cg::test::check(
            r,
            phantom_kept,
            "concrete-receiver Phantom::error_code call is kept");
        cg::test::check(
            r,
            helper_kept,
            "implicit-this helper() inside the class template is "
            "kept (guard must not overfire)");
        cg::test::check(
            r,
            run_kept,
            "concrete-receiver w.run(c) keeps its call site");
        cg::test::check(
            r,
            site_named(store, "use_all", "go").has_value(),
            "concrete-receiver c.go() keeps its call site");
    }

    /** @brief 字段流三形态：显式 this、字段=调用、obj.field 接收者。 */
    void field_flow_forms(R& r)
    {
        {
            db::GraphStore store{":memory:"};
            (void)index_fixture(store, "this_assign");
            const std::vector<DEdge> dw_edges = dispatch_edges(store);
            const DEdge* e = find_edge(dw_edges, "S::run", "Impl::f");
            cg::test::check(
                r,
                e != nullptr && e->confidence == 0.7,
                "explicit this->p = new Impl reaches run()");
        }
        {
            db::GraphStore store{":memory:"};
            (void)index_fixture(store, "field_call");
            const std::vector<DEdge> dw_edges = dispatch_edges(store);
            const DEdge* e = find_edge(dw_edges, "S::run", "Impl::f");
            cg::test::check(
                r,
                e != nullptr && e->confidence == 0.7,
                "p = make() records a return-slot field flow");
        }
        {
            db::GraphStore store{":memory:"};
            (void)index_fixture(store, "obj_field");
            const std::vector<DEdge> edges = dispatch_edges(store);
            const DEdge* run = find_edge(edges, "S::run", "Impl::f");
            const DEdge* direct = find_edge(edges, "main", "Impl::f");
            cg::test::check(
                r,
                run != nullptr && run->confidence == 0.7,
                "obj.field assignment reaches run()'s site");
            cg::test::check(
                r,
                direct != nullptr && direct->confidence == 0.7,
                "s.p->f() resolves the field slot by object record");
            const auto site = site_named(store, "main", "f");
            cg::test::check(
                r,
                site.has_value() && !site->receiver_is_this
                    && site->receiver_name == "p"
                    && !site->receiver_owner_key.empty(),
                "explicit object field receiver carries its owner record");
        }
    }

    /** @brief q = p; 参与函数内别名传播。 */
    void local_reassignment_alias(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "realias");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "main", "Impl::f");
        cg::test::check(r, e != nullptr, "q = p alias reaches Impl::f");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "local reassignment alias is medium (0.7)");
    }

    /** @brief 显式接口类型的 unique_ptr/shared_ptr 初始化保留实现证据。 */
    void interface_smart_pointers(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "smart_interface");
        const std::vector<DEdge> edges = dispatch_edges(store);
        for (const std::string_view caller :
             {"use_unique", "use_shared", "use_new"})
        {
            const DEdge* e = find_edge(edges, caller, "Impl::f");
            cg::test::check(
                r,
                e != nullptr,
                "interface smart pointer resolves to Impl::f");
            cg::test::check(
                r,
                e != nullptr && e->confidence >= 0.7,
                "smart pointer evidence is at least medium");
        }
        for (const DEdge& e : edges)
        {
            cg::test::check(
                r,
                e.dst_name != "I::f" || e.confidence == 0.0,
                "interface type is never dynamic evidence");
        }
    }

    /** @brief std::move/std::forward 实参按恒等包装传播。 */
    void identity_wrapper_arguments(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "move_arg");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge* moved = find_edge(edges, "take_move", "Impl::f");
        const DEdge* forwarded = find_edge(edges, "take_forward", "Impl::f");
        cg::test::check(
            r,
            moved != nullptr && moved->confidence == 0.7,
            "std::move argument propagates the variable slot");
        cg::test::check(
            r,
            forwarded != nullptr && forwarded->confidence == 0.7,
            "std::forward argument propagates the variable slot");
        const auto site = site_named(store, "main", "take_move");
        cg::test::check(
            r,
            site.has_value()
                && site->arg_vars.find("\"p\"") != std::string::npos,
            "std::move is stripped to its first argument variable");
    }

    /** @brief 比较运算不是赋值：不得产生假的 LocalCopy 别名。 */
    void comparison_is_not_assignment(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "compare_guard");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge*             impl = find_edge(edges, "main", "Impl::f");
        bool                     other = false;
        for (const DEdge& e : edges)
        {
            if (e.src_name == "main" && e.dst_name == "Other::f")
            {
                other = true;
            }
        }
        cg::test::check(
            r,
            impl != nullptr && impl->confidence >= 0.7,
            "p keeps its own dynamic type");
        cg::test::check(r, !other, "p == q comparison never aliases q into p");
    }

    /** @brief 多类型槽的 fallback 原因按来源具体化。 */
    void multiple_reason_specificity(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "reason_multi");
        const auto field = site_named(store, "S::run", "f");
        const auto ret = site_named(store, "use_return", "f");
        cg::test::check(
            r,
            field.has_value()
                && field->candidates.find("field_flow_multiple")
                       != std::string::npos,
            "field multi-write slot reports field_flow_multiple");
        cg::test::check(
            r,
            ret.has_value()
                && ret->candidates.find("return_flow_multiple")
                       != std::string::npos,
            "return multi-type slot reports return_flow_multiple");
    }

    /** @brief 虚被调用者本身未解析时用 no_callee_symbol，不与无动态证据混同。
     */
    void unresolved_virtual_callee_reason(R& r)
    {
        db::GraphStore       store{":memory:"};
        const cg::db::FileId file = store.upsert_file("e:/synthetic/ghost.cpp");
        const cg::db::SymId  caller = store.upsert_symbol(
            {.unique_key = "function||caller|e:/synthetic/ghost.cpp",
             .kind = cg::db::SymbolKind::Function,
             .name = "caller",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = file,
             .line = 1,
             .col = 1});
        store.upsert_call_site(
            {.caller_id = caller,
             .callee_key = std::string_view{},
             .callee_name = "ghost",
             .is_virtual = true,
             .file_id = file,
             .line = 2,
             .col = 3});
        const cg::analysis::StubTypeOracle   oracle;
        const cg::analysis::PropagationState state =
            cg::analysis::run(store, oracle);
        const cg::analysis::DispatchResult dispatch =
            cg::analysis::resolve(store, state);
        bool found = false;
        for (const cg::analysis::ResolvedCallSite& site : dispatch.sites)
        {
            if (!site.callee_resolved && !site.resolved
                && site.fallback_reason == "no_callee_symbol")
            {
                found = true;
            }
        }
        cg::test::check(
            r,
            found,
            "unresolved virtual callee reports no_callee_symbol");
    }

    /**
     * @brief upsert_call_site 对 indirect 站点回查承诺的固化用例。
     *
     * @internal 回查 WHERE 含 indirect 第七条件（与唯一键七列一致）；漏绑
     *           该参数时参数零初始化为 0，indirect=1 的行（函数指针调用）
     *           回查失配返回 0——"重复 upsert 返回既有 id"的承诺对这类行
     *           被打破。本例同时覆盖首插与撞键两步：两步都必须回查到同一
     *           个 siteId。
     */
    void upsert_call_site_indirect_requery(R& r)
    {
        db::GraphStore       store{":memory:"};
        const cg::db::FileId file = store.upsert_file("e:/synthetic/fptr.cpp");
        const cg::db::SymId  caller = store.upsert_symbol(
            {.unique_key = "function||caller|e:/synthetic/fptr.cpp",
             .kind = cg::db::SymbolKind::Function,
             .name = "caller",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = file,
             .line = 1,
             .col = 1});
        const cg::db::CallSiteWrite site{
            .caller_id = caller,
            .callee_name = "fp",
            .is_virtual = false,
            .indirect = true,
            .file_id = file,
            .line = 2,
            .col = 3};
        const std::int64_t first = store.upsert_call_site(site);
        cg::test::check(
            r,
            first > 0,
            "an inserted indirect site re-queries its own id");
        const std::int64_t again = store.upsert_call_site(site);
        cg::test::check(
            r,
            again == first,
            "a duplicate indirect upsert re-queries the same id");
    }

    // ----------------------------------- 派发词汇表 -------------------------

    /**
     * @brief 静态决议（static_unique_override）：final/具体类 + 家族唯一
     *        非纯实现给 1.0 且站点 resolved；多实现守卫必须拒绝决议。
     * @internal 验收时建议固化的用例。攻击函数不接收 &obj 实参：
     *           取址证据会把站点翻到动态路径（0.7），掩盖静态分支。
     */
    void static_unique_override_resolution(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "staticfinal");
        cg::test::check(r, sum.failed == 0, "staticfinal indexes cleanly");
        const std::vector<DEdge> edges = dispatch_edges(store);
        // 形态 1：pure 接口 + 叶类唯一覆盖 -> 1.0 决议。
        const DEdge* pure = find_edge(edges, "probe_pure_single", "LeafA2b::h");
        cg::test::check(
            r,
            pure != nullptr && pure->confidence == 1.0,
            "leaf + unique override resolves to 1.0");
        const auto pure_site = site_for(store, "probe_pure_single", "h", true);
        cg::test::check(
            r,
            pure_site.has_value() && pure_site->resolved
                && pure_site->candidates.find("static_unique_override")
                       != std::string::npos
                && pure_site->candidates.find("\"confidence\":\"high\"")
                       != std::string::npos,
            "resolved site carries reason=static_unique_override at high");
        // 形态 2：唯一非纯实现是被继承的基类方法（目标 = method 自身）。
        const DEdge* self_impl =
            find_edge(edges, "probe_self_impl", "BaseA2::g");
        cg::test::check(
            r,
            self_impl != nullptr && self_impl->confidence == 1.0,
            "inherited unique implementation resolves the method itself (1.0)");
        const auto self_site = site_for(store, "probe_self_impl", "g", true);
        cg::test::check(
            r,
            self_site.has_value() && self_site->resolved
                && self_site->candidates.find("static_unique_override")
                       != std::string::npos,
            "method-self form also reports static_unique_override");
        // 守卫（件一语义更新）：叶子记录 + 多实现家族不再一律拒绝——
        // 接收者静态类型是叶子（extends 无后代）时动态类型只能是它自身，
        // nearest-override 唯一收敛即静态可证（1.0，reason=
        // static_final_unique）。旧的"多实现守卫必须拒绝"只对非叶子接收
        // 者成立（见形态 4）。
        const DEdge* multi = find_edge(edges, "probe_final_multi", "Final3::f");
        cg::test::check(
            r,
            multi != nullptr && multi->confidence == 1.0,
            "leaf receiver with multi-impl family resolves at 1.0");
        const auto multi_site = site_for(store, "probe_final_multi", "f", true);
        cg::test::check(
            r,
            multi_site.has_value() && multi_site->resolved
                && multi_site->candidates.find("static_final_unique")
                       != std::string::npos,
            "leaf receiver carries reason=static_final_unique");
        // 形态 4（新守卫）：非叶子接收者 + 多实现家族——无任何 final
        // 证据，静态决议（旧守卫与新路径）都必须拒绝，只留 0.0 家族
        // 标记边（DTA 收窄的 0.4 展示候选不发边）。
        const DEdge* mid = find_edge(edges, "probe_mid_multi", "MidA1::f");
        cg::test::check(
            r,
            mid == nullptr || mid->confidence == 0.0,
            "non-leaf receiver with multi-impl family never gets a 1.0 edge");
        const auto mid_site = site_for(store, "probe_mid_multi", "f", true);
        cg::test::check(
            r,
            mid_site.has_value() && !mid_site->resolved,
            "non-leaf receiver stays unresolved (unique-impl guard intact)");
    }

    /**
     * @brief 方法级 final 静态直连（static_final_method）：开放类（类级
     *        证据缺失）+ 被调方法自带 final 语法位时最近覆盖唯一即直连
     *        1.0；只 override 不 final、声明/定义分离、多根不猜三守卫
     *        全部各就各位。
     *
     * @internal 验收用例。类级 all_final 与方法级接力两级
     *           证据的直连目标恒同——夹具用"开放类 + 图内后代 + 直接纯
     *           虚面"隔离出纯方法级证据面（类级语法位为 0、图内无后代
     *           近似也失效、第一优先 sealed 口径被纯虚面挡住）。攻击函
     *           数不接收 &obj 实参、用裸空指针，防取址证据把站点翻到动
     *           态路径掩盖静态分支。形态 8 的多根并集用接收者改写（增量
     *           stale-merge 形态）后重跑派发，仿 static_types_union 用例。
     */
    void static_final_method_resolution(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "staticfinal");
        cg::test::check(r, sum.failed == 0, "staticfinal indexes cleanly");
        const std::vector<DEdge> edges = dispatch_edges(store);
        // 形态 5 具名接收者：开放类 + 方法 final + 图内后代 → 直连 1.0，
        // reason 为 static_final_method。
        const DEdge* named =
            find_edge(edges, "probe_final_method_named", "OpenProbe::release");
        cg::test::check(
            r,
            named != nullptr && named->confidence == 1.0,
            "open class + final method resolves to 1.0");
        const auto named_site =
            site_for(store, "probe_final_method_named", "release", true);
        cg::test::check(
            r,
            named_site.has_value() && named_site->resolved
                && named_site->candidates.find("static_final_method")
                       != std::string::npos
                && named_site->candidates.find("\"confidence\":\"high\"")
                       != std::string::npos,
            "named receiver carries reason=static_final_method at high");
        // 形态 5 隐式 this：同判定，来源按第一优先惯例区分命名。
        const DEdge* self =
            find_edge(edges, "OpenProbe::query", "OpenProbe::add_ref");
        cg::test::check(
            r,
            self != nullptr && self->confidence == 1.0,
            "implicit-this final-method call resolves to 1.0");
        const auto self_site =
            site_for(store, "OpenProbe::query", "add_ref", true);
        cg::test::check(
            r,
            self_site.has_value() && self_site->resolved
                && self_site->candidates.find("implicit_this_final_method")
                       != std::string::npos,
            "implicit-this source is named implicit_this_final_method");
        // 形态 6 守卫：只 override 不 final——两级 final 判定都不得触发。
        const auto guard_site =
            site_for(store, "probe_final_method_guard", "release", true);
        cg::test::check(
            r,
            guard_site.has_value() && !guard_site->resolved
                && guard_site->candidates.find("static_final_method")
                       == std::string::npos
                && guard_site->candidates.find("static_final_unique")
                       == std::string::npos,
            "override-only method is never final-resolved");
        // 形态 7：声明/定义分离的 final 位随定义游标保留，直连照发。
        const DEdge* split =
            find_edge(edges, "probe_final_method_split", "SplitHost::add_ref");
        cg::test::check(
            r,
            split != nullptr && split->confidence == 1.0,
            "out-of-line final definition keeps the final bit");
        const auto split_site =
            site_for(store, "probe_final_method_split", "add_ref", true);
        cg::test::check(
            r,
            split_site.has_value() && split_site->resolved
                && split_site->candidates.find("static_final_method")
                       != std::string::npos,
            "split declaration/definition resolves via static_final_method");
        // 形态 8：双根各自最近覆盖不同 final 方法 → 并集两成员不猜（接
        // 收者静态面改写为双根并集——union 接收者/多路径初始化形态，
        // 重跑派发断言）。
        const cg::db::SymId ra = symbol_id(store, "RootAProbe");
        const cg::db::SymId rb = symbol_id(store, "RootBProbe");
        auto union_row = site_for(store, "probe_final_method_union", "m", true);
        const auto ra_row = cg::db::get_symbol(store, ra);
        const auto rb_row = cg::db::get_symbol(store, rb);
        cg::test::check(
            r,
            union_row.has_value() && ra_row.has_value() && rb_row.has_value(),
            "form-8 roots and site present");
        if (union_row.has_value() && ra_row.has_value() && rb_row.has_value())
        {
            union_row->receiver_static = cg::analysis::json_string_array(
                std::vector<std::string>{
                    ra_row->unique_key,
                    rb_row->unique_key});
            store.exec(
                std::format(
                    "UPDATE call_sites SET receiver_static='{}' "
                    "WHERE site_id={}",
                    union_row->receiver_static,
                    union_row->id));
            const cg::analysis::StubTypeOracle   oracle;
            const cg::analysis::PropagationState state =
                cg::analysis::run(store, oracle);
            const cg::analysis::DispatchResult dispatch =
                cg::analysis::resolve(store, state);
            const cg::analysis::ResolvedCallSite* union_site = nullptr;
            for (const cg::analysis::ResolvedCallSite& s : dispatch.sites)
            {
                if (s.site_id == union_row->id)
                {
                    union_site = &s;
                }
            }
            cg::test::check(
                r,
                union_site != nullptr,
                "re-run resolves the rewritten union site");
            bool has_ra = false;
            bool has_rb = false;
            bool any_final_reason = false;
            if (union_site != nullptr)
            {
                for (const cg::analysis::DispatchCandidate& c :
                     union_site->candidates)
                {
                    if (c.target_key.find("|RootAProbe|m|")
                        != std::string::npos)
                    {
                        has_ra = true;
                    }
                    if (c.target_key.find("|RootBProbe|m|")
                        != std::string::npos)
                    {
                        has_rb = true;
                    }
                    any_final_reason =
                        any_final_reason
                        || c.reason.find("final") != std::string::npos;
                }
            }
            cg::test::check(
                r,
                union_site != nullptr && !union_site->resolved
                    && !any_final_reason,
                "two diverging final methods never guess one target");
            cg::test::check(
                r,
                has_ra && has_rb,
                "union keeps both final members as display-only");
        }
    }

    /**
     * @brief DTA 静态收窄（static_narrowed）：接口静态类型不决议，候选 =
     *        家族∩子树、置信 0.4 只进 call_sites 不发边；子树外实现与
     *        method 自身都被排除。
     */
    void static_narrowed_candidates(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "staticnarrow");
        cg::test::check(r, sum.failed == 0, "staticnarrow indexes cleanly");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const auto iface = site_for(store, "probe_iface", "f", true);
        cg::test::check(
            r,
            iface.has_value() && !iface->resolved
                && iface->fallback_reason
                       == "no_dynamic_type_evidence;static_narrowed",
            "interface static type reports static_narrowed (exact reason)");
        cg::test::check(
            r,
            iface.has_value()
                && iface->candidates.find("|MidB|f|") != std::string::npos
                && iface->candidates.find("|XB|f|") != std::string::npos
                && iface->candidates.find("|XJ|f|") == std::string::npos,
            "candidates = family cap subtree (sibling implementation stays out)");
        cg::test::check(
            r,
            iface.has_value()
                && iface->candidates.find("\"confidence\":\"low\"")
                       != std::string::npos,
            "narrowed candidates are display-only low (0.4)");
        // 0.4 候选不进 edges：站点只保留一条 0.0 家族标记边。
        const DEdge* marker = find_edge(edges, "probe_iface", "IB::f");
        cg::test::check(
            r,
            marker != nullptr && marker->confidence == 0.0
                && (marker->attrs & cg::db::edge_attrs::dispatch_fallback) != 0,
            "narrowing keeps the single 0.0 family marker edge");
        for (const DEdge& e : edges)
        {
            cg::test::check(
                r,
                !(e.src_name == "probe_iface" && e.confidence > 0.0),
                "0.4 narrowed candidates never become dispatch edges");
        }
        // 中间类静态类型：method 自身（MidB::f）被排除，候选恰 XB::f。
        const auto mid = site_for(store, "probe_mid", "f", true);
        cg::test::check(
            r,
            mid.has_value()
                && mid->candidates.find("|XB|f|") != std::string::npos
                && mid->candidates.find("|MidB|f|") == std::string::npos
                && mid->fallback_reason
                       == "no_dynamic_type_evidence;static_narrowed",
            "mid-type narrowing excludes the method itself");
        // 兄弟接口对照：JB 子树内只有 XJ::f，MidB/XB 不混入。
        const auto sibling = site_for(store, "probe_sibling", "f", true);
        cg::test::check(
            r,
            sibling.has_value()
                && sibling->candidates.find("|XJ|f|") != std::string::npos
                && sibling->candidates.find("|MidB|f|") == std::string::npos,
            "sibling subtree keeps only its own implementation");
    }

    /**
     * @brief 多静态类型并集：dispatch 必须对全部静态根取子树并集，
     *        不能只看 front()（评审修正的回归钉）。
     *
     * @internal 把 probe_union 站点的 receiver_static（装载判证列）改写
     *           成两个叶类——union 接收者/多路径初始化的静态面形态，直接
     *           固化该输入形态后重跑 run()+resolve() 检查 DispatchResult。
     */
    void static_types_union(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "staticunion");
        const cg::db::SymId l1 = symbol_id(store, "L1");
        const cg::db::SymId l2 = symbol_id(store, "L2");
        cg::test::check(r, l1 != 0 && l2 != 0, "staticunion leaves present");
        auto row = site_for(store, "probe_union", "f", true);
        cg::test::check(r, row.has_value(), "probe_union site present");
        const auto l1_row = cg::db::get_symbol(store, l1);
        const auto l2_row = cg::db::get_symbol(store, l2);
        cg::test::check(
            r,
            l1_row.has_value() && l2_row.has_value(),
            "both leaf record keys resolve");
        if (!row.has_value() || !l1_row.has_value() || !l2_row.has_value())
        {
            return;
        }
        row->receiver_static = cg::analysis::json_string_array(
            std::vector<std::string>{l1_row->unique_key, l2_row->unique_key});
        store.exec(
            std::format(
                "UPDATE call_sites SET receiver_static='{}' "
                "WHERE site_id={}",
                row->receiver_static,
                row->id));
        const cg::analysis::StubTypeOracle   oracle;
        const cg::analysis::PropagationState state =
            cg::analysis::run(store, oracle);
        const cg::analysis::DispatchResult dispatch =
            cg::analysis::resolve(store, state);
        const cg::analysis::ResolvedCallSite* site = nullptr;
        for (const cg::analysis::ResolvedCallSite& s : dispatch.sites)
        {
            if (s.site_id == row->id)
            {
                site = &s;
            }
        }
        cg::test::check(
            r,
            site != nullptr,
            "re-run resolves the rewritten site");
        bool has_l1 = false;
        bool has_l2 = false;
        bool all_low = true;
        if (site != nullptr)
        {
            for (const cg::analysis::DispatchCandidate& c : site->candidates)
            {
                if (c.target_key.find("|L1|f|") != std::string::npos)
                {
                    has_l1 = true;
                }
                if (c.target_key.find("|L2|f|") != std::string::npos)
                {
                    has_l2 = true;
                }
                all_low =
                    all_low && c.confidence == cg::analysis::Confidence::Low;
            }
        }
        cg::test::check(
            r,
            has_l1 && has_l2,
            "multi static types union covers BOTH subtrees");
        cg::test::check(r, all_low, "union candidates stay display-only low");
        cg::test::check(
            r,
            site != nullptr
                && site->fallback_reason
                       == "no_dynamic_type_evidence;static_narrowed",
            "union narrowing reports static_narrowed");
    }

    /** @brief 子树溢出守卫：>256 节点的静态子树回退全家族并显式标
     *         subtree_overflow（附 candidates_truncated），绝不静默丢目标。 */
    void subtree_overflow_fallback(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "subtreeof");
        cg::test::check(r, sum.failed == 0, "subtreeof indexes cleanly");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const auto site = site_for(store, "probe_overflow", "f", true);
        cg::test::check(
            r,
            site.has_value()
                && site->fallback_reason
                       == "no_dynamic_type_evidence;subtree_overflow;"
                          "candidates_truncated",
            "oversized subtree is explicit in fallback_reason");
        cg::test::check(
            r,
            site.has_value() && !site->resolved,
            "overflow site stays unresolved");
        std::size_t candidate_count = 0;
        if (site.has_value())
        {
            const std::string& text = site->candidates;
            for (std::size_t pos = 0;
                 (pos = text.find("\"target\"", pos)) != std::string::npos;
                 pos += 8)
            {
                ++candidate_count;
            }
        }
        cg::test::check(
            r,
            candidate_count == 64,
            "overflow falls back to the capped whole family (64)");
        cg::test::check(
            r,
            edges.size() == 1 && edges.front().confidence == 0.0
                && (edges.front().attrs & cg::db::edge_attrs::dispatch_fallback)
                       != 0,
            "overflow keeps exactly one 0.0 family marker edge");
    }

    /**
     * @brief nearest 收敛不降级（_nearest_converged）：单动态类型保持
     *        >=0.7 且 reason 带后缀；两类型同收敛 0.4 升 0.7（基底保留）；
     *        分歧对照保持 0.4 且绝不声称收敛。
     */
    void nearest_converged_upgrade(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "converge");
        cg::test::check(r, sum.failed == 0, "converge indexes cleanly");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge* single = find_edge(edges, "probe_single", "NB::w");
        cg::test::check(
            r,
            single != nullptr && single->confidence >= 0.7,
            "single dynamic type keeps >= 0.7 through convergence");
        const auto single_site = site_for(store, "probe_single", "w", true);
        cg::test::check(
            r,
            single_site.has_value()
                && single_site->candidates.find("direct_new_nearest_converged")
                       != std::string::npos,
            "converged candidate reason carries the _nearest_converged suffix");
        const DEdge* conv = find_edge(edges, "probe_converge", "NB::w");
        cg::test::check(
            r,
            conv != nullptr && conv->confidence == 0.7,
            "converging multi-type site upgrades 0.4 -> 0.7");
        const auto conv_site = site_for(store, "probe_converge", "w", true);
        cg::test::check(
            r,
            conv_site.has_value()
                && conv_site->candidates.find(
                       "conditional_merge_nearest_converged")
                       != std::string::npos,
            "upgraded reason keeps its base (conditional_merge) plus suffix");
        const DEdge* d1 = find_edge(edges, "probe_diverge", "S1::v");
        const DEdge* d2 = find_edge(edges, "probe_diverge", "S2::v");
        cg::test::check(
            r,
            d1 != nullptr && d2 != nullptr && d1->confidence == 0.4
                && d2->confidence == 0.4,
            "diverging overrides stay 0.4");
        const auto div_site = site_for(store, "probe_diverge", "v", true);
        cg::test::check(
            r,
            div_site.has_value()
                && div_site->candidates.find("_nearest_converged")
                       == std::string::npos,
            "diverging site never claims convergence");
    }

    /**
     * @brief 槽类型集截断回归：40 个动态类型经全局实参流进接收者形参槽，
     *        被 MAX_TYPES=32
     *        截断后边恰 32 条、resolved 站点的 fallback_reason 如实追加
     *        slot_type_truncated 且无前导分号。
     */
    void slot_type_truncated_regression(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "truncated");
        cg::test::check(r, sum.failed == 0, "truncated indexes cleanly");
        const std::vector<DEdge> edges = dispatch_edges(store);
        cg::test::check(
            r,
            edges.size() == 32,
            "the receiver slot keeps exactly 32 types");
        cg::test::check(
            r,
            find_edge(edges, "run", "D00::step") != nullptr
                && find_edge(edges, "run", "D31::step") != nullptr
                && find_edge(edges, "run", "D32::step") == nullptr
                && find_edge(edges, "run", "D39::step") == nullptr,
            "truncation keeps the first 32 sorted types (D00..D31)");
        const auto site = site_for(store, "run", "step", true);
        cg::test::check(
            r,
            site.has_value() && site->resolved
                && site->fallback_reason == "slot_type_truncated",
            "resolved site appends slot_type_truncated without a stray ';'");
    }

    // --------------------------------- 出参包装判据 / 取址方法判据
    // ------------

    // 结构性判据（collect_detail::is_outparam_wrapper_class /
    // is_addressof_method）尚未接入 collect 主路径；这些测试
    // 直接在夹具光标上评测它们以钉死其正确性。

    namespace criteria_detail
    {

        /** @brief TU 内全部同名 ClassTemplate（含前向声明与定义、含 std）。 */
        [[nodiscard]]
        auto class_templates_named(
            const cg::clang::Tu& tu,
            std::string_view     name) -> std::vector<CXCursor>
        {
            std::vector<CXCursor>                out;
            std::function<void(const CXCursor&)> walk = [&](const CXCursor& c)
            {
                const CXCursorKind k = cg::clang::kind(c);
                if (k == CXCursor_ClassTemplate
                    && cg::clang::spelling(c) == name)
                {
                    out.push_back(c);
                }
                if (k == CXCursor_Namespace || k == CXCursor_ClassTemplate
                    || k == CXCursor_StructDecl || k == CXCursor_ClassDecl
                    || k == CXCursor_LinkageSpec)
                {
                    cg::clang::children(
                        c,
                        [&](const CXCursor& ch)
                        {
                            walk(ch);
                            return true;
                        });
                }
            };
            cg::clang::children(
                tu.cursor(),
                [&](const CXCursor& c)
                {
                    walk(c);
                    return true;
                });
            return out;
        }

        /** @brief TU 内全部同名非模板记录（StructDecl/ClassDecl）。 */
        [[nodiscard]]
        auto records_named(const cg::clang::Tu& tu, std::string_view name)
            -> std::vector<CXCursor>
        {
            std::vector<CXCursor> out;
            cg::clang::children(
                tu.cursor(),
                [&](const CXCursor& c)
                {
                    if ((cg::clang::kind(c) == CXCursor_StructDecl
                         || cg::clang::kind(c) == CXCursor_ClassDecl)
                        && cg::clang::spelling(c) == name)
                    {
                        out.push_back(c);
                    }
                    return true;
                });
            return out;
        }

        /** @brief 记录/类模板直属成员里第一个同名方法/构造（未找到为空游标）。
         */
        [[nodiscard]]
        auto member_of(const CXCursor& cls, std::string_view name) -> CXCursor
        {
            CXCursor found{};
            cg::clang::children(
                cls,
                [&](const CXCursor& c)
                {
                    const CXCursorKind k = cg::clang::kind(c);
                    if ((k == CXCursor_CXXMethod
                         || k == CXCursor_FunctionTemplate
                         || k == CXCursor_Constructor)
                        && cg::clang::spelling(c) == name)
                    {
                        found = c;
                    }
                    return false; // 只扫直属成员，不下钻方法体
                });
            return found;
        }

    } // namespace criteria_detail

    /** @brief 出参包装判据：正例命中带槽位映射，对抗形态与 libc++ 全拒。 */
    void outparam_wrapper_criterion(R& r)
    {
        const cg::clang::Index    index;
        const cg::clang::ParsedTu parsed = cg::clang::parse(
            index,
            fixture("heur/heur.cpp"),
            cg::clang::fallback_argv());
        cg::test::check(
            r,
            parsed.tu.valid() && parsed.errors.empty(),
            "heuristics fixture parses clean");
        using cg::analysis::collect_detail::Walker;
        const auto only = [&](std::string_view name) -> CXCursor
        {
            const std::vector<CXCursor> found =
                criteria_detail::class_templates_named(parsed.tu, name);
            cg::test::check(
                r,
                found.size() == 1,
                std::string{name} + " has exactly one template");
            return found.empty() ? CXCursor{} : found.front();
        };

        const auto hit =
            Walker::is_outparam_wrapper_class(only("HeurOutParam"));
        cg::test::check(
            r,
            hit.has_value(),
            "AcmeOutPtr-shaped wrapper hits the structural criterion");
        if (hit)
        {
            cg::test::check(
                r,
                hit->template_index == 0 && hit->ctor_param_pos == 0
                    && hit->field == "pp_out_" && hit->writeback == "Set",
                "hit reports the slot mapping (index/pos/field/method)");
        }
        const auto writer =
            Walker::is_outparam_wrapper_class(only("HeurSlotWriter"));
        cg::test::check(
            r,
            writer.has_value() && writer->field == "slot_"
                && writer->writeback == "Assign",
            "minimal slot writer hits with field and write-back");

        // 对抗形态：构造绑定、字段与回写各守卫逐一拦截，binds>1 整体
        // no-op，全部宁漏勿误。
        for (const char* miss :
             {"HeurNoField",
              "HeurStaticSet",
              "HeurPtrHolder",
              "HeurMatrix",
              "HeurListPush",
              "HeurMemberTmplCtor",
              "HeurIndexedSlot",
              "HeurBindMethod",
              "HeurCompareNotAssign",
              "HeurTwoBind",
              "HeurSwapFields"})
        {
            const std::vector<CXCursor> found =
                criteria_detail::class_templates_named(parsed.tu, miss);
            cg::test::check(
                r,
                !found.empty(),
                std::string{miss} + " template is present");
            for (const CXCursor& c : found)
            {
                cg::test::check(
                    r,
                    !Walker::is_outparam_wrapper_class(c).has_value(),
                    std::string{miss}
                        + " is rejected (prefer miss over false hit)");
            }
        }
        // libc++ 智能指针不命中（判据零名字参与，靠结构排除）。
        for (const char* smart : {"unique_ptr", "shared_ptr", "weak_ptr"})
        {
            const std::vector<CXCursor> found =
                criteria_detail::class_templates_named(parsed.tu, smart);
            cg::test::check(
                r,
                !found.empty(),
                std::string{"std::"} + smart + " template is visible");
            for (const CXCursor& c : found)
            {
                cg::test::check(
                    r,
                    !Walker::is_outparam_wrapper_class(c).has_value(),
                    std::string{"std::"} + smart + " never hits the criterion");
            }
        }
    }

    /** @brief 取址方法判据：四条件合取 + 成员链比对三态。 */
    void addressof_method_criterion(R& r)
    {
        const cg::clang::Index    index;
        const cg::clang::ParsedTu parsed = cg::clang::parse(
            index,
            fixture("heur/heur.cpp"),
            cg::clang::fallback_argv());
        cg::test::check(
            r,
            parsed.tu.valid() && parsed.errors.empty(),
            "heuristics fixture parses clean");
        using cg::analysis::collect_detail::AddressofChain;
        using cg::analysis::collect_detail::Walker;
        const auto only = [&](std::string_view name) -> CXCursor
        {
            const std::vector<CXCursor> found =
                criteria_detail::class_templates_named(parsed.tu, name);
            cg::test::check(
                r,
                found.size() == 1,
                std::string{name} + " has exactly one template");
            return found.empty() ? CXCursor{} : found.front();
        };
        const auto verdict = [&](std::string_view cls, std::string_view method)
        {
            return Walker::is_addressof_method(
                criteria_detail::member_of(only(cls), method));
        };
        const auto tie = [&](const char* cls, const char* m)
        {
            const auto v = verdict(cls, m);
            cg::test::check(
                r,
                v.has_value() && v->chain == AddressofChain::Tie,
                std::string{cls} + "::" + m
                    + " passes C1-C4 and ties on the arrow slot");
        };
        const auto reject = [&](const char* cls, const char* m)
        {
            const auto v = verdict(cls, m);
            cg::test::check(
                r,
                v.has_value() && v->chain == AddressofChain::Reject,
                std::string{cls} + "::" + m
                    + " is rejected by the member-chain compare");
        };
        const auto degrade = [&](const char* cls, const char* m)
        {
            const auto v = verdict(cls, m);
            cg::test::check(
                r,
                v.has_value() && v->chain == AddressofChain::Uncertain,
                std::string{cls} + "::" + m
                    + " degrades to uncertain (never a tie)");
        };
        const auto inactive = [&](const char* cls, const char* m)
        {
            const auto v = verdict(cls, m);
            cg::test::check(
                r,
                !v.has_value(),
                std::string{cls} + "::" + m + " fails C1-C4");
        };

        // 真取址形态（SamplePtr::Put）：同槽 tie，typedef 返回同槽也 tie。
        tie("SlotPtr", "put");
        tie("Cache", "slot");
        tie("Cache", "alias_slot");
        // 成员链比对拒：指向别处的可解析异槽。
        reject("Cache", "spare");    // 内部数组 spare_ 对箭头槽 cur_
        reject("TwoSlot", "put");    // 同声明异实例：链基段 a_ 对 b_
        reject("StaticSlot", "put"); // 静态成员槽 &g_（VarDecl 对 FieldDecl）
        // 成员链比对降级：解析失败（漏收方向，安全）。
        degrade("SameChain", "put");   // 伪 tie 守卫：两侧 InvalidFile 不判 tie
        degrade("PtrIndirect", "put"); // 经指针间接（s_->cur_ 的隐式解引用）
        degrade("ArrSlot", "put");     // 数组下标槽
        degrade("NoBody", "put");      // 无体
        degrade("MultiRet", "put");    // 多 return 无单一链
        degrade("ForwardPut", "put");  // 转发调用（b_.grab()）
        // 四条件各拒收面。
        inactive("Q", "probe_out");      // 成员模板重编号撞拼写
        inactive("ConstArrow", "put");   // const 箭头拼写不等
        inactive("VoidPut", "put_void"); // void** inner 非 TP
        inactive("HeurOutParam", "Put"); // 单指针返回（Get 形态）
        // 非模板指针式类（无 type-parameter 标记）。
        const std::vector<CXCursor> conc =
            criteria_detail::records_named(parsed.tu, "ConcPtr");
        cg::test::check(r, conc.size() == 1, "ConcPtr record is present");
        for (const CXCursor& c : conc)
        {
            cg::test::check(
                r,
                !Walker::is_addressof_method(
                     criteria_detail::member_of(c, "put"))
                     .has_value(),
                "non-template pointer-style class fails C2");
        }
        // libc++ 智能指针的 operator->
        // 不是取址方法（单指针结果，双层指针条件挡）。
        bool saw_smart_arrow = false;
        for (const char* smart : {"unique_ptr", "shared_ptr", "weak_ptr"})
        {
            for (const CXCursor& c :
                 criteria_detail::class_templates_named(parsed.tu, smart))
            {
                const CXCursor arrow =
                    criteria_detail::member_of(c, "operator->");
                if (cg::clang::is_null(arrow))
                {
                    continue;
                }
                saw_smart_arrow = true;
                cg::test::check(
                    r,
                    !Walker::is_addressof_method(arrow).has_value(),
                    std::string{"std::"} + smart
                        + "::operator-> is not an address-of method");
            }
        }
        cg::test::check(
            r,
            saw_smart_arrow,
            "at least one std smart-pointer operator-> was inspected");
    }

    // ------------------------------------------------------------ demo 演示
    // -----

    void demo_single_implementation(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "demo1");
        cg::test::check(r, sum.failed == 0, "demo1 indexes cleanly");
        const std::vector<DEdge> edges = dispatch_edges(store);
        cg::test::check(
            r,
            edges.size() == 1,
            "demo1 has exactly one dispatch edge");
        const DEdge* e = find_edge(edges, "main", "Impl::f");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 1.0,
            "demo1 resolves I* p = new Impl to Impl::f (1.0)");
        // CALLS 语义保持原样：main -> I::f 这条边仍然存在。
        bool calls_base = false;
        for (const cg::db::EdgeRow& edge : cg::db::all_edges(store))
        {
            if (edge.type != cg::db::EdgeType::Calls)
            {
                continue;
            }
            const auto src = cg::db::get_symbol(store, edge.src);
            const auto dst = cg::db::get_symbol(store, edge.dst);
            if (src && dst && src->qualified_name == "main"
                && dst->qualified_name == "I::f")
            {
                calls_base = true;
            }
        }
        cg::test::check(
            r,
            calls_base,
            "CALLS main -> I::f is unchanged by dispatch");
    }

    void demo_cross_tu_setter(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "demo2");
        cg::test::check(r, sum.failed == 0, "demo2 indexes cleanly");
        const std::vector<DEdge> dw_edges = dispatch_edges(store);
        const DEdge*             e = find_edge(dw_edges, "set", "Impl::f");
        cg::test::check(
            r,
            e != nullptr,
            "cross-TU setter resolves p->f to Impl::f");
        cg::test::check(
            r,
            e != nullptr && e->confidence == 0.7,
            "cross-TU setter is medium (0.7)");
    }

    void demo_two_implementations(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "demo3");
        cg::test::check(r, sum.failed == 0, "demo3 indexes cleanly");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge*             a = find_edge(edges, "use", "D1::f");
        const DEdge*             b = find_edge(edges, "use", "D2::f");
        cg::test::check(
            r,
            a != nullptr && b != nullptr,
            "two reachable implementations yield two edges");
        cg::test::check(
            r,
            a != nullptr && b != nullptr && a->confidence == 0.4
                && b->confidence == 0.4,
            "dual implementation candidates are low (0.4)");
        cg::test::check(
            r,
            a != nullptr && b != nullptr && a->line == b->line,
            "both edges share the call site line");
        // 按 caller+callee 定位 use 体内唯一的 b->f() 调用点，行号实参
        // 会随夹具增删漂移，这里不与绝对行号耦合。
        const auto site = site_named(store, "use", "f");
        cg::test::check(
            r,
            site.has_value() && site->resolved
                && site->candidates.find("|D1|f|") != std::string::npos
                && site->candidates.find("|D2|f|") != std::string::npos,
            "the site reports both candidates (no single-target lie)");
    }

    // --------------------------------------------- 地址取用证据 --------------

    /**
     * @brief 取址证据回归钉：&Class::method / &free_fn 落 #addr 行，消费侧
     *        address_taken_hints 按 unique_key 精确文本命中。
     *
     * 契约：source = 6（HintSource::AddressOfObject 的
     * 整数值）、related_key = 被取址符号的 unique_key 文本、var_name =
     * "#addr" 保留槽名（propagate 按槽名跳过）；&obj 对象取址行
     * related_key 恒空，天然不与 #addr 行混淆。
     */
    void addr_taken_evidence(R& r)
    {
        db::GraphStore store{":memory:"};
        (void)index_fixture(store, "addr_taken");

        const auto key_of = [&](std::string_view qualified)
        {
            const auto found = cg::db::find_symbols(store, qualified, {});
            return found.matches.empty() ? std::string{}
                                         : found.matches.front().unique_key;
        };
        const std::string method_key = key_of("Handler::on_event");
        const std::string fn_key = key_of("free_step");
        cg::test::check(
            r,
            !method_key.empty(),
            "fixture yields the Handler::on_event symbol");
        cg::test::check(
            r,
            !fn_key.empty(),
            "fixture yields the free_step symbol");

        // 生产侧：#addr 行恰两行（方法取址 + 自由函数取址），related_key
        // 逐字等于符号表 unique_key；&obj 形态绝不产生 #addr 行。
        const cg::db::SymId owner = symbol_id(store, "register_all");
        std::size_t         slot_rows = 0;
        bool                method_row = false;
        bool                fn_row = false;
        bool                obj_row_empty_key = false;
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            if (row.source
                != static_cast<int>(cg::analysis::HintSource::AddressOfObject))
            {
                continue;
            }
            if (row.var_name == "#addr")
            {
                ++slot_rows;
                if (row.related_key == method_key)
                {
                    method_row = row.owner_id == owner;
                }
                if (row.related_key == fn_key)
                {
                    fn_row = row.owner_id == owner;
                }
            }
            else if (row.related_key.empty())
            {
                obj_row_empty_key = true; // &h / &ph 对象取址行（老形态）
            }
        }
        cg::test::check(
            r,
            slot_rows == 2,
            "exactly two #addr rows (method + free function)");
        cg::test::check(
            r,
            method_row,
            "&Handler::on_event pins the method's unique_key text "
            "with the taking function as owner");
        cg::test::check(
            r,
            fn_row,
            "&free_step pins the function's unique_key text");
        cg::test::check(
            r,
            obj_row_empty_key,
            "&obj rows keep an empty related_key (never #addr)");

        // 消费侧：db::address_taken_hints 按 unique_key 精确文本命中 #addr 行。
        const std::vector<std::string_view> keys{method_key};
        const auto consumed = cg::db::address_taken_hints(store, keys);
        bool       consumed_method = false;
        for (const cg::db::TypeHintRow& row : consumed)
        {
            consumed_method =
                consumed_method
                || (row.var_name == "#addr" && row.related_key == method_key);
        }
        cg::test::check(
            r,
            consumed_method,
            "address_taken_hints returns the #addr row by "
            "unique_key");
    }

    // ------------------------------------------------------- 确定性
    // -----------

    [[nodiscard]]
    auto analysis_dump(cg::db::GraphStore& store) -> std::string
    {
        std::string out;
        for (const std::string& row : cg::db::dump_edges(store))
        {
            out += row;
            out += '\n';
        }
        for (const cg::db::TypeHintRow& h : store.all_type_hints())
        {
            out += std::format(
                "hint|{}|{}|{}|{}|{}|{}|{}\n",
                h.id,
                h.owner_id,
                h.var_name,
                h.line,
                h.col,
                h.source,
                h.type_set);
        }
        for (const cg::db::CallSiteRow& s : store.all_call_sites())
        {
            out += std::format(
                "site|{}|{}|{}|{}|{}|{}|{}|{}\n",
                s.id,
                s.caller_id,
                s.line,
                s.col,
                s.is_virtual,
                s.resolved,
                s.confidence,
                s.candidates);
        }
        return out;
    }

    void fixed_point_determinism(R& r)
    {
        db::GraphStore a{":memory:"};
        db::GraphStore b{":memory:"};
        (void)index_fixture(a, "demo2");
        (void)index_fixture(b, "demo2");
        cg::test::check(
            r,
            analysis_dump(a) == analysis_dump(b),
            "two fresh runs are byte-identical (hints/sites/edges)");
    }

    // ------------------------------------------------- RTA + elem ------------

    /** @brief 接口名 -> 其 "`#external`" 行（无行为 nullopt）。 */
    [[nodiscard]]
    auto external_row_of(cg::db::GraphStore& store, std::string_view iface)
        -> std::optional<cg::db::TypeHintRow>
    {
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            if (row.source
                    != static_cast<int>(cg::analysis::HintSource::External)
                || row.var_name != "#external")
            {
                continue;
            }
            const auto sym = cg::db::get_symbol(store, row.owner_id);
            if (sym && sym->name == iface)
            {
                return row;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 外部边界 RTA（root set 线）：四类边界的 `#external` 行与 R(I)
     *        内容、候选排序/收缩 + external_rta_narrowed 标注、hit==1 只展示
     *        不发边、hit==0 索引外标注保全家族、非边界接口零污染
     *        （可达性绝不抬升站点置信——全库零推断边）。
     */
    void external_rta_boundaries(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "externalrta");
        cg::test::check(r, sum.failed == 0, "externalrta indexes cleanly");
        cg::test::check(
            r,
            sum.external_rta_hints == 3,
            "external scan writes exactly three #external rows");

        const auto reason = [&](std::string_view iface) -> std::string
        {
            const auto row = external_row_of(store, iface);
            return row.has_value() ? row->fallback_reason : std::string{};
        };
        cg::test::check(
            r,
            reason("IExt") == "dlopen_boundary",
            "loader-chain interface marked dlopen_boundary");
        cg::test::check(
            r,
            reason("IPlain") == "director",
            "director interface without instantiations still marked");
        cg::test::check(
            r,
            reason("IDir") == "director",
            "SwigDirector_* base interface marked director");
        cg::test::check(
            r,
            reason("IReg").empty(),
            "register* heuristic removed: no boundary row");
        cg::test::check(
            r,
            reason("INoSig").empty(),
            "unsigned loader cast emits no row (chain broken)");
        cg::test::check(
            r,
            reason("INeg").empty(),
            "non-boundary interface gets no #external row");

        const auto anchored = external_row_of(store, "IExt");
        cg::test::check(
            r,
            anchored.has_value()
                && (anchored->related_var.starts_with("dlopen@main.cpp:")
                    || anchored->related_var.starts_with(
                        "GetProcAddress@main.cpp:")),
            "dlopen row carries the loader call-site anchor");

        // R(I) = 后代闭包 ∩ I_all：命中实例化的实现、排除未实例化实现。
        const auto reach = external_row_of(store, "IExt");
        cg::test::check(
            r,
            reach.has_value()
                && reach->type_set.find("|ExtImpl|") != std::string::npos
                && reach->type_set.find("|ExtOther|") == std::string::npos,
            "R(I) keeps the instantiated implementation and drops the rest");
        const auto plain = external_row_of(store, "IPlain");
        cg::test::check(
            r,
            plain.has_value() && plain->type_set == "[]",
            "no library instantiation yields an empty reach set");

        // hit==1：候选收缩到唯一命中 + external_rta_narrowed 标注，
        // 仍不 resolved、绝无推断边（0.0 家族标记边保留）。
        const auto site = site_for(store, "probe_export", "use", true);
        cg::test::check(
            r,
            site.has_value() && !site->resolved
                && site->fallback_reason
                       == "no_dynamic_type_evidence;static_narrowed;external_rta_narrowed",
            "hit==1 site carries the exact narrowed reason chain");
        cg::test::check(
            r,
            site.has_value()
                && site->candidates.find("|ExtImpl|use|") != std::string::npos
                && site->candidates.find("|ExtOther|use|") == std::string::npos
                && site->candidates.find("\"confidence\":\"low\"")
                       != std::string::npos,
            "hit==1 candidates collapse to the single reachable implementation");
        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge* marker = find_edge(edges, "probe_export", "IExt::use");
        cg::test::check(
            r,
            marker != nullptr && marker->confidence == 0.0
                && (marker->attrs & cg::db::edge_attrs::dispatch_fallback) != 0,
            "hit==1 keeps the single 0.0 family marker edge");
        for (const DEdge& e : edges)
        {
            cg::test::check(
                r,
                !(e.src_name == "probe_export" && e.confidence > 0.0),
                "reachability never becomes an inferred edge (C3)");
        }

        // hit==0：无库内实例化实例——保全家族并标注实现可能索引外。
        const auto plain_site = site_for(store, "probe_plain", "q", true);
        cg::test::check(
            r,
            plain_site.has_value()
                && plain_site->fallback_reason
                       == "no_dynamic_type_evidence;static_narrowed;"
                          "external_no_instantiation",
            "hit==0 site reports implementation-may-be-outside-index");
        cg::test::check(
            r,
            plain_site.has_value()
                && plain_site->candidates.find("|PlainImpl|q|")
                       != std::string::npos,
            "hit==0 keeps the whole family as candidates");

        // director/dlopen 边界的站点同享收缩语义。
        const auto dir_site = site_for(store, "probe_director", "step", true);
        cg::test::check(
            r,
            dir_site.has_value()
                && dir_site->fallback_reason.find("external_rta_narrowed")
                       != std::string::npos
                && dir_site->candidates.find("|DirImpl|step|")
                       != std::string::npos,
            "director interface site narrows to the library implementation");
        const auto plug_site = site_for(store, "probe_export", "use", true);
        cg::test::check(
            r,
            plug_site.has_value()
                && plug_site->fallback_reason.find("external_rta_narrowed")
                       != std::string::npos
                && plug_site->candidates.find("|ExtImpl|use|")
                       != std::string::npos,
            "dlopen-boundary interface site narrows likewise");

        // 非边界接口对照：无 external_* 词、候选为普通静态收窄。
        const auto neg_site = site_for(store, "probe_neg", "neg", true);
        cg::test::check(
            r,
            neg_site.has_value()
                && neg_site->fallback_reason.find("external")
                       == std::string::npos,
            "non-boundary interface site stays free of external markers");

        // 总闸：可达性绝不能制造推断边。计数收窄到三个边界接口消费
        // 方（宿主抽象直连使无边界行的单实现负例接口（INeg/INoSig）
        // 合法直连 High，全库计数不再恒零；边界接口自身家族均多实现，
        // 直连与守卫都不触发）。
        std::int64_t inferred = 0;
        for (const DEdge& e : edges)
        {
            if ((e.src_name == "probe_export" || e.src_name == "probe_plain"
                 || e.src_name == "probe_director")
                && e.confidence > 0.0)
            {
                ++inferred;
            }
        }
        cg::test::check(
            r,
            inferred == 0,
            "boundary-interface sites keep zero inferred edges (C3)");
        // 宿主抽象直连的正例落点：无边界行的单实现接口由纯虚位 +
        // 全库唯一非纯 override 直连（与 RTA 可达性无关的证据面）。
        const DEdge* neg_direct = find_edge(edges, "probe_neg", "NegImpl::neg");
        cg::test::check(
            r,
            neg_direct != nullptr && neg_direct->confidence == 1.0,
            "a non-boundary single-impl interface resolves at 1.0");
        const auto neg_direct_site = site_for(store, "probe_neg", "neg", true);
        cg::test::check(
            r,
            neg_direct_site.has_value() && neg_direct_site->resolved
                && neg_direct_site->candidates.find("family_unique_override")
                       != std::string::npos,
            "the abstract-host direct link carries family_unique_override");
    }

    /**
     * @brief dlopen 零激活修复：加载系 API 原型声明在索引界外
     *        （系统头形态，../externalout-api/ext_api.h）时，调用点仍落
     *        call_sites（dlopen 配对的唯一输入源），锚点照常发行 `#external`
     *        行；站点 callee 永不按名解析（无假 calls 边）。此前界外声明被
     *        collect 的 under_roots 守卫整点丢弃——真实语料（Win32 系统头）
     *        全库 API 站点为 0，夹具把声明写在 TU 内掩盖了缺口。
     */
    void external_outside_api(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "externalout");
        cg::test::check(r, sum.failed == 0, "externalout indexes cleanly");

        // 站点落库：界外 dlopen 调用点存在、名字如实、绝不解析成符号。
        const auto site = site_for(store, "run_out", "dlopen", false);
        cg::test::check(
            r,
            site.has_value(),
            "out-of-index dlopen site lands in call_sites");
        cg::test::check(
            r,
            site.has_value() && !site->resolved,
            "out-of-index API site never resolves to a library symbol");
        const auto proc = site_for(store, "run_out", "GetProcAddress", false);
        cg::test::check(
            r,
            proc.has_value(),
            "out-of-index GetProcAddress site lands in call_sites");

        // 配对照常发行：IExtOut 标 dlopen_boundary，锚点指向本文件调用点。
        const auto row = external_row_of(store, "IExtOut");
        cg::test::check(
            r,
            row.has_value() && row->fallback_reason == "dlopen_boundary",
            "out-of-index loader chain still claims the boundary");
        cg::test::check(
            r,
            row.has_value()
                && (row->related_var.starts_with("dlopen@main.cpp:")
                    || row->related_var.starts_with(
                        "GetProcAddress@main.cpp:")),
            "anchor still names the loader call site in the indexed TU");

        // 边界铁律：界外站点绝不产生 calls 边（全库零推断边）。
        const std::vector<DEdge> edges = dispatch_edges(store);
        std::int64_t             inferred = 0;
        for (const DEdge& e : edges)
        {
            if (e.confidence > 0.0)
            {
                ++inferred;
            }
        }
        cg::test::check(
            r,
            inferred == 0,
            "out-of-index API sites add zero inferred edges");
    }

    /**
     * @brief 容器元素槽：声明期 Declared 元素行（静态面）、写集递归
     *        保级（new 直接 High → 0.7）、多写入并集 Low（0.4×2）、拷贝
     *        构造/拷贝赋值/assign 区间拷贝三条局部拷贝流、字段容器跨方法
     *        并集、智能指针壳元素剥壳（operator-> 单一判据）、数组负例。
     */
    void container_elem_flow(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, "containerelem");
        cg::test::check(r, sum.failed == 0, "containerelem indexes cleanly");

        bool elem_declared = false;
        bool elem_link = false;
        bool elem_copy_flow = false;
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            if (row.var_name == "#elem:sinks"
                && row.source
                       == static_cast<int>(cg::analysis::HintSource::Declared)
                && row.type_set.find("|Sink|") != std::string::npos)
            {
                elem_declared = true;
            }
            if (row.source
                    == static_cast<int>(
                        cg::analysis::HintSource::ContainerElemFlow)
                && row.var_name == "s" && row.related_var == "#elem:sinks")
            {
                elem_link = true;
            }
            if (row.source
                    == static_cast<int>(
                        cg::analysis::HintSource::ContainerElemFlow)
                && row.var_name == "#elem:dst"
                && row.related_var.find("#elem:src") != std::string::npos)
            {
                elem_copy_flow = true;
            }
        }
        cg::test::check(
            r,
            elem_declared,
            "declaration-time #elem row is Declared and static-only");
        cg::test::check(
            r,
            elem_link,
            "range-for link row points at the container elem slot");
        cg::test::check(
            r,
            elem_copy_flow,
            "copy-construct emits an elem-slot copy flow");

        const std::vector<DEdge> edges = dispatch_edges(store);
        const DEdge* single = find_edge(edges, "probe_loop", "FileSink::flow");
        cg::test::check(
            r,
            single != nullptr && single->confidence == 0.7,
            "write-then-range-for reaches 0.7 through the elem slot");
        const DEdge* copy = find_edge(edges, "probe_copy", "MemSink::flow");
        cg::test::check(
            r,
            copy != nullptr && copy->confidence == 0.7,
            "copy-constructed container keeps the source evidence");
        const DEdge* assign_copy =
            find_edge(edges, "probe_copy_assign", "FileSink::flow");
        cg::test::check(
            r,
            assign_copy != nullptr && assign_copy->confidence == 0.7,
            "copy-assigned container keeps the source evidence");

        const DEdge* multi_a =
            find_edge(edges, "probe_multi", "FileSink::flow");
        const DEdge* multi_b = find_edge(edges, "probe_multi", "MemSink::flow");
        cg::test::check(
            r,
            multi_a != nullptr && multi_b != nullptr
                && multi_a->confidence == 0.4 && multi_b->confidence == 0.4,
            "two writes merge into an honest low union (0.4 x2)");

        // 字段容器：fill/drain_field 两处写入并集 → Low；快照 assign 拷贝流
        // 继承同一并集。
        const DEdge* field_a =
            find_edge(edges, "SinkHost::drain_field", "FileSink::flow");
        const DEdge* field_b =
            find_edge(edges, "SinkHost::drain_field", "MemSink::flow");
        cg::test::check(
            r,
            field_a != nullptr && field_b != nullptr
                && field_a->confidence == 0.4 && field_b->confidence == 0.4,
            "field container merges writes across methods (0.4 union)");
        const DEdge* snap_a =
            find_edge(edges, "SinkHost::drain_snapshot", "FileSink::flow");
        const DEdge* snap_b =
            find_edge(edges, "SinkHost::drain_snapshot", "MemSink::flow");
        cg::test::check(
            r,
            snap_a != nullptr && snap_b != nullptr && snap_a->confidence == 0.4
                && snap_b->confidence == 0.4,
            "assign(begin, end) snapshot inherits the field elem slot");

        // 智能指针壳元素：剥壳后 push_back 的 new 直达元素槽。
        const DEdge* shell = find_edge(edges, "probe_shell", "FileSink::flow");
        cg::test::check(
            r,
            shell != nullptr && shell->confidence == 0.7,
            "smart-pointer element peels to the pointee evidence");

        // 数组负例：不是 std 容器，无元素槽、无链接行，站点保持诚实回退
        // （接口静态类型多实现 → 静态收窄附加词，与
        // analysis.dispatch.static_narrowed 的 reason 口径一致）。
        const auto arr_site = site_for(store, "probe_array", "flow", true);
        cg::test::check(
            r,
            arr_site.has_value() && !arr_site->resolved
                && arr_site->fallback_reason
                       == "no_dynamic_type_evidence;static_narrowed",
            "plain-array range-for keeps its honest fallback (exact reason)");
        for (const DEdge& e : edges)
        {
            cg::test::check(
                r,
                !(e.src_name == "probe_array" && e.confidence > 0.0),
                "plain-array range-for never gets an inferred edge");
        }
    }

} // namespace

const cg::test::Registrar reg_l0_direct{
    "analysis.l0.direct_new",
    &l0_direct_new};
const cg::test::Registrar reg_l0_cast{"analysis.l0.cast_down", &l0_cast_down};
const cg::test::Registrar reg_l0_other{
    "analysis.l0.other_sources",
    &l0_other_sources};
const cg::test::Registrar reg_l1_alias{"analysis.l1.alias", &l1_alias};
const cg::test::Registrar reg_l1_cond{
    "analysis.l1.conditional",
    &l1_conditional};
const cg::test::Registrar reg_l1_ret{
    "analysis.l1.return_flow",
    &l1_return_flow};
const cg::test::Registrar reg_l2_param{
    "analysis.l2.param_flow",
    &l2_param_flow};
const cg::test::Registrar reg_l2_ret{
    "analysis.l2.return_flow",
    &l2_return_flow};
const cg::test::Registrar reg_l2_multi{
    "analysis.l2.multi_caller",
    &l2_multi_caller};
const cg::test::Registrar reg_rangefor{
    "analysis.container_element_not_dynamic",
    &container_element_is_not_dynamic};
const cg::test::Registrar reg_callarg{
    "analysis.l2.call_argument_return_slot",
    &call_argument_return_slot};
const cg::test::Registrar reg_vararg{
    "analysis.l2.argument_variable_flow",
    &argument_variable_flow};
const cg::test::Registrar reg_retvar{
    "analysis.l2.return_via_parameter",
    &return_value_via_parameter};
const cg::test::Registrar reg_mixed{
    "analysis.dispatch.mixed_override_nearest",
    &mixed_override_nearest};
const cg::test::Registrar reg_indirect{
    "analysis.dispatch.indirect_call",
    &indirect_call_is_reported};
const cg::test::Registrar reg_smart{
    "analysis.dispatch.smart_pointer",
    &smart_pointer_receiver};
const cg::test::Registrar reg_field{
    "analysis.l1.field_assignment",
    &field_assignment_flow};
const cg::test::Registrar reg_field_factory{
    "analysis.l1.field_factory_assignment",
    &field_factory_assignment};
const cg::test::Registrar reg_budget{
    "analysis.dispatch.family_level_fallback",
    &fallback_is_family_level};
const cg::test::Registrar reg_resolution{
    "analysis.call_sites.resolution_split",
    &call_site_resolution_split};
const cg::test::Registrar reg_traverse{
    "analysis.traverse.dispatch_merge",
    &traverse_merges_dispatch};
const cg::test::Registrar reg_precise{
    "analysis.traverse.precise_default",
    &precise_dispatch_default};
const cg::test::Registrar reg_changes_dispatch{
    "analysis.changes.dispatch_callers",
    &changes_include_dispatch_callers};
const cg::test::Registrar reg_precise_det{
    "analysis.traverse.precise_deterministic",
    &precise_dispatch_deterministic};
const cg::test::Registrar reg_demo1{
    "analysis.demo.single_implementation",
    &demo_single_implementation};
const cg::test::Registrar reg_demo2{
    "analysis.demo.cross_tu_setter",
    &demo_cross_tu_setter};
const cg::test::Registrar reg_demo3{
    "analysis.demo.two_implementations",
    &demo_two_implementations};
const cg::test::Registrar reg_det{
    "analysis.determinism",
    &fixed_point_determinism};
const cg::test::Registrar reg_fptr_variants{
    "analysis.indirect.function_pointer_variants",
    &function_pointer_variants};
const cg::test::Registrar reg_nested_callee{
    "analysis.indirect.nested_callee",
    &nested_callee_indirect};
const cg::test::Registrar reg_cast_wrap{
    "analysis.cast.wrapped_evidence",
    &cast_wrapped_evidence};
const cg::test::Registrar reg_cstyle_cast{
    "analysis.cast.cstyle_operand_evidence",
    &cstyle_cast_evidence};
const cg::test::Registrar reg_depcall{
    "analysis.calls.dependent_receiver",
    &dependent_member_calls};
const cg::test::Registrar reg_field_forms{
    "analysis.field.flow_forms",
    &field_flow_forms};
const cg::test::Registrar reg_realias{
    "analysis.l1.reassignment_alias",
    &local_reassignment_alias};
const cg::test::Registrar reg_smart_interface{
    "analysis.smart.interface_types",
    &interface_smart_pointers};
const cg::test::Registrar reg_identity_args{
    "analysis.l2.identity_wrappers",
    &identity_wrapper_arguments};
const cg::test::Registrar reg_compare_guard{
    "analysis.l1.comparison_not_assignment",
    &comparison_is_not_assignment};
const cg::test::Registrar reg_multi_reason{
    "analysis.dispatch.multiple_reason",
    &multiple_reason_specificity};
const cg::test::Registrar reg_no_callee{
    "analysis.dispatch.no_callee_symbol",
    &unresolved_virtual_callee_reason};
const cg::test::Registrar reg_indirect_requery{
    "analysis.db.upsert_call_site_indirect_requery",
    &upsert_call_site_indirect_requery};
const cg::test::Registrar reg_outparam_criterion{
    "analysis.criteria.outparam_wrapper",
    &outparam_wrapper_criterion};
const cg::test::Registrar reg_addressof_criterion{
    "analysis.criteria.addressof_method",
    &addressof_method_criterion};
const cg::test::Registrar reg_static_unique{
    "analysis.dispatch.static_unique_override",
    &static_unique_override_resolution};
const cg::test::Registrar reg_static_final_method{
    "analysis.dispatch.static_final_method",
    &static_final_method_resolution};
const cg::test::Registrar reg_static_narrowed{
    "analysis.dispatch.static_narrowed",
    &static_narrowed_candidates};
const cg::test::Registrar reg_static_union{
    "analysis.dispatch.static_types_union",
    &static_types_union};
const cg::test::Registrar reg_subtree_overflow{
    "analysis.dispatch.subtree_overflow",
    &subtree_overflow_fallback};
const cg::test::Registrar reg_nearest_converged{
    "analysis.dispatch.nearest_converged",
    &nearest_converged_upgrade};
const cg::test::Registrar reg_slot_truncated{
    "analysis.dispatch.slot_type_truncated",
    &slot_type_truncated_regression};
const cg::test::Registrar reg_addr_taken{
    "analysis.addr_taken.evidence",
    &addr_taken_evidence};
const cg::test::Registrar reg_external_rta{
    "analysis.external.rta",
    &external_rta_boundaries};
const cg::test::Registrar reg_external_outside{
    "analysis.external.outside_api",
    &external_outside_api};
const cg::test::Registrar reg_container_elem{
    "analysis.container.elem_flow",
    &container_elem_flow};

// ------------------------------------------------ 槽位基础设施 (v13) ------

void passthrough_registry(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "passthrough");
    const std::vector<cg::db::PassthroughWrapperRow> rows =
        store.all_passthrough_wrappers();

    const auto has =
        [&](std::string_view wrapper, std::string_view getter, int arg)
    {
        for (const auto& w : rows)
        {
            if (w.wrapper_key.find(wrapper) != std::string::npos
                && w.getter_name == getter && w.arg_index == arg)
            {
                return true;
            }
        }
        return false;
    };
    const auto absent = [&](std::string_view wrapper)
    {
        for (const auto& w : rows)
        {
            if (w.wrapper_key.find(wrapper) != std::string::npos)
            {
                return false;
            }
        }
        return true;
    };

    cg::test::check(
        r,
        has("WrapAtomic", "load", 0),
        "value-form getter (atomic::load shape)");
    cg::test::check(
        r,
        has("WrapOpt", "value", 0),
        "value-form getter (optional::value shape)");
    cg::test::check(
        r,
        has("WrapPtr", "get", 0),
        "pointer-form getter (shared_ptr::get shape)");
    cg::test::check(
        r,
        has("WrapDeref", "operator->", 0),
        "operator-> getter (SamplePtr shape)");
    cg::test::check(
        r,
        has("WrapDeref", "get", 0),
        "dual getter registers both entries");
    cg::test::check(
        r,
        has("WrapTwo", "pick", 1),
        "second own parameter indexed");
    cg::test::check(r, absent("NotTemplate"), "non-template rejected");
    cg::test::check(r, absent("NoGetter"), "non-parameter return rejected");
    cg::test::check(r, absent("WithArgs"), "parameterized getter rejected");
}

void slot_target_cap_bounds(R& r)
{
    std::vector<std::string> v;
    for (int i = 0; i < static_cast<int>(cg::analysis::SLOT_TARGET_CAP); ++i)
    {
        v.push_back("function|ns|f" + std::to_string(i) + "|a.cc");
    }
    cg::test::check(
        r,
        !cg::analysis::slot_target_cap_truncate(v),
        "at-cap set does not truncate");
    // 重复元素先去重：56 份同键仍是一行，不触发截断。
    std::vector<std::string> dup(
        cg::analysis::SLOT_TARGET_CAP,
        "method|ns|C::m|a.cc");
    cg::test::check(
        r,
        !cg::analysis::slot_target_cap_truncate(dup),
        "duplicates dedup before cap check");
    cg::test::check(r, dup.size() == 1, "dedup collapses identical keys");
    v.push_back("function|ns|zz|a.cc");
    cg::test::check(
        r,
        cg::analysis::slot_target_cap_truncate(v),
        "over-cap set truncates");
    cg::test::check(
        r,
        v.size() == cg::analysis::SLOT_TARGET_CAP,
        "capped at 56");
    const bool has_tail =
        std::find(v.begin(), v.end(), "function|ns|zz|a.cc") == v.end();
    cg::test::check(r, has_tail, "sorted truncation drops the last key");
    const auto again = cg::analysis::slot_target_cap_truncate(v);
    cg::test::check(
        r,
        !again && v.size() == cg::analysis::SLOT_TARGET_CAP,
        "truncate is idempotent");
}

void slot_mixed_key_roundtrip(R& r)
{
    db::GraphStore store{":memory:"};
    // 混合命名空间目标集（record|/function|/method| 完整 unique_key）：
    // 落库读回逐字节一致——"能流能存"的验收面（语义解析另测）。
    const std::vector<std::string> mixed = {
        "function|ns|handler|a.cc",
        "method|ns|C::run|a.cc",
        "record|ns|Impl|a.h"};
    const std::string json = cg::analysis::json_string_array(mixed);
    store.upsert_analysis_slot(
        {.owner_key = "function|ns|probe|a.cc",
         .var_name = "slot1",
         .type_set = json,
         .source = 7,
         .reason = "r1_probe",
         .ver = 1});
    const std::vector<cg::db::AnalysisSlotRow> rows =
        store.all_analysis_slots();
    cg::test::check(r, rows.size() == 1, "one slot row round-trips");
    if (rows.size() == 1)
    {
        cg::test::check(
            r,
            rows.front().type_set == json,
            "mixed-namespace type_set byte-identical");
        cg::test::check(
            r,
            rows.front().type_set.find("function|ns|handler|a.cc")
                != std::string::npos,
            "function key survives storage");
    }
}

const cg::test::Registrar reg_passthrough_registry{
    "analysis.slot.passthrough_registry",
    &passthrough_registry};

// ------------------------------------------------ 透传壳闸 ----------------

/** @brief 断链 1-4 六形态：透传壳闸消费接线后的推断边。 */
void passthrough_gate_consumption(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "ptgate");
    const std::vector<DEdge> edges = dispatch_edges(store);
    const auto               conf_edge = [&](std::string_view caller,
                                             std::string_view dst,
                                             double           want,
                                             std::string_view label)
    {
        const DEdge* e = find_edge(edges, caller, dst);
        cg::test::check(r, e != nullptr, "edge exists");
        cg::test::check(r, e != nullptr && e->confidence == want, label);
        return e;
    };
    // 壳字段站点（use()）的边归属方法而非驱动函数：caller 是 use。
    conf_edge(
        "HolderCtor::use",
        "ImplA::virt",
        0.7,
        "ctor-init penetration is medium (0.7)");
    conf_edge(
        "assign_penetration",
        "ImplA::virt",
        0.7,
        "operator=(T*) store penetration is medium (0.7)");
    conf_edge(
        "HolderMove::use",
        "ImplB::virt",
        0.7,
        "shell move transport is medium (0.7)");
    conf_edge(
        "getter_chain_receiver",
        "ImplA::virt",
        1.0,
        "chained getter receiver keeps DirectNew grade (1.0)");
    conf_edge(
        "getter_value_slot",
        "ImplA::virt",
        0.7,
        "getter value slot alias is medium (0.7)");
    conf_edge(
        "plain_getter_chain",
        "ImplB::virt",
        0.7,
        "plain-class getter body chase is medium (0.7)");
}

/** @brief 负例一：字段槽并集多实现不发唯一边（宁漏勿猜）。 */
void passthrough_gate_negative_multi(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "ptgate");
    const std::vector<DEdge> edges = dispatch_edges(store);
    const DEdge* a = find_edge(edges, "HolderMulti::use", "ImplA::virt");
    const DEdge* b = find_edge(edges, "HolderMulti::use", "ImplB::virt");
    cg::test::check(
        r,
        a != nullptr && b != nullptr,
        "union slot keeps both candidates");
    cg::test::check(
        r,
        a != nullptr && b != nullptr && a->confidence == 0.4
            && b->confidence == 0.4,
        "multi pointee stays low (0.4), never a unique edge");
    for (const DEdge& e : edges)
    {
        if (e.src_name == "HolderMulti::use")
        {
            cg::test::check(
                r,
                e.confidence <= 0.4,
                "no medium/high edge escapes the ambiguous pool");
        }
    }
}

/** @brief 负例二：无证据的壳站点只发家族级 0.0 标记边。 */
void passthrough_gate_negative_empty(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "ptgate");
    const std::vector<DEdge> edges = dispatch_edges(store);
    bool                     saw_inferred = false;
    for (const DEdge& e : edges)
    {
        if (e.src_name != "negative_empty_site")
        {
            continue;
        }
        if ((e.attrs & cg::db::edge_attrs::dispatch_inferred) != 0)
        {
            saw_inferred = true;
        }
        cg::test::check(
            r,
            e.confidence == 0.0,
            "evidence-less shell site emits only 0.0");
    }
    cg::test::check(
        r,
        !saw_inferred,
        "no inferred dispatch edge without pointee evidence");
    const auto site = site_named(store, "negative_empty_site", "virt");
    cg::test::check(
        r,
        site.has_value() && !site->resolved
            && site->fallback_reason == "no_dynamic_type_evidence",
        "empty shell site reports no_dynamic_type_evidence");
}

/** @brief 采集面：壳字段写侧穿透的 FieldFlow 行与 getter 值槽别名。 */
void passthrough_gate_collect_rows(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "ptgate");
    bool ctor_alias = false;
    bool getter_value_alias = false;
    bool put_value_alias = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        if (row.source == static_cast<int>(cg::analysis::HintSource::FieldFlow)
            && row.fallback_reason == "ctor_init_flow"
            && row.related_var.starts_with("@fnv|"))
        {
            ctor_alias = true;
        }
        if (row.source == static_cast<int>(cg::analysis::HintSource::ReturnFlow)
            && row.fallback_reason == "getter_pointee"
            && !row.related_var.empty())
        {
            getter_value_alias = true;
            // 取址形态（T** 返回的 Put 族）不得按透传值消费。
            if (row.var_name == "pp")
            {
                put_value_alias = true;
            }
        }
    }
    cg::test::check(
        r,
        ctor_alias,
        "ctor init emits FieldFlow row with @fnv alias");
    cg::test::check(
        r,
        getter_value_alias,
        "getter value slot emits receiver alias row");
    cg::test::check(
        r,
        !put_value_alias,
        "address-form getter (T**) never aliases as passthrough");
}

const cg::test::Registrar reg_ptw_gate_consumption{
    "analysis.slot.passthrough_gate_consumption",
    &passthrough_gate_consumption};
const cg::test::Registrar reg_ptw_gate_negative_multi{
    "analysis.slot.passthrough_gate_negative_multi",
    &passthrough_gate_negative_multi};
const cg::test::Registrar reg_ptw_gate_negative_empty{
    "analysis.slot.passthrough_gate_negative_empty",
    &passthrough_gate_negative_empty};
const cg::test::Registrar reg_ptw_gate_collect_rows{
    "analysis.slot.passthrough_gate_collect_rows",
    &passthrough_gate_collect_rows};

// --------------------------------------------- 模板继承 --------------------

namespace
{
    constexpr cg::db::EdgeType k_test_ext[] = {cg::db::EdgeType::Extends};
    constexpr cg::db::EdgeType k_test_spec[] = {cg::db::EdgeType::Specializes};
    constexpr cg::db::EdgeType k_test_cb[] = {cg::db::EdgeType::CallbackReg};
} // namespace

/** @internal 全库符号行做成的 SymId -> unique_key 映射（边收集共用）。 */
[[nodiscard]]
auto symbol_keys(cg::db::GraphStore& store)
    -> std::unordered_map<cg::db::SymId, std::string>
{
    std::unordered_map<cg::db::SymId, std::string> out;
    for (const cg::db::IndexRow& row : cg::db::all_symbol_index_rows(store))
    {
        out.emplace(row.id, row.unique_key);
    }
    return out;
}

/** @internal extends 边 (src 名 -> dst key 文本, attrs) 收集帮手。 */
[[nodiscard]]
auto extends_pairs(cg::db::GraphStore& store)
    -> std::vector<std::tuple<std::string, std::string, std::uint32_t>>
{
    std::vector<std::tuple<std::string, std::string, std::uint32_t>> out;
    const auto key_of = symbol_keys(store);
    for (const auto& [id, key] : key_of)
    {
        for (const cg::db::EdgeAggRow& e :
             cg::db::edges_from(store, id, k_test_ext))
        {
            const auto it = key_of.find(e.other);
            if (it != key_of.end())
            {
                out.emplace_back(key, it->second, e.attrs);
            }
        }
    }
    std::ranges::sort(out);
    return out;
}

/** @internal 一条收集到的 indirect_calls 边（src/dst 均为 unique_key）。 */
struct IEdge
{
    std::string   src;
    std::string   dst;
    std::uint32_t attrs = 0;
};

/** @internal indirect_calls 边 (src key, dst key, attrs) 收集帮手。 */
[[nodiscard]]
auto indirect_edges(cg::db::GraphStore& store) -> std::vector<IEdge>
{
    constexpr cg::db::EdgeType k_indirect[] = {cg::db::EdgeType::IndirectCalls};
    const auto                 key_of = symbol_keys(store);
    std::vector<IEdge>         out;
    for (const auto& [id, key] : key_of)
    {
        for (const cg::db::EdgeAggRow& e :
             cg::db::edges_from(store, id, k_indirect))
        {
            const auto it = key_of.find(e.other);
            if (it != key_of.end())
            {
                out.push_back({key, it->second, e.attrs});
            }
        }
    }
    return out;
}

/** @internal 按 src/dst 子串与属性位在收集结果中查一条 indirect 边。 */
[[nodiscard]]
auto has_indirect_edge(
    const std::vector<IEdge>& edges,
    std::string_view          from,
    std::string_view          to,
    std::uint32_t             attrs_mask) -> bool
{
    for (const IEdge& e : edges)
    {
        if (e.src.find(from) != std::string::npos
            && e.dst.find(to) != std::string::npos
            && (e.attrs & attrs_mask) != 0)
        {
            return true;
        }
    }
    return false;
}

void tmpl_inherit_edges(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "tmpl_inherit");
    const auto pairs = extends_pairs(store);

    const auto has_edge = [&](std::string_view src, std::string_view dst_frag)
    {
        for (const auto& [s, d, a] : pairs)
        {
            if (s.find(src) != std::string::npos
                && d.find(dst_frag) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    };

    // 三形态：extends 直指实例（不再折叠回主模板）。
    cg::test::check(
        r,
        has_edge("|Derived", "Base<Derived>"),
        "CRTP extends points at specialization");
    cg::test::check(
        r,
        has_edge("|UsesSpec", "Multi<int>"),
        "explicit specialization base");
    cg::test::check(
        r,
        has_edge("|User", "traits<"),
        "partial specialization instance base");
    // dependent 基类：指主模板本尊 + is_template_base 位（0x08）。
    bool dependent_ok = false;
    for (const auto& [s, d, a] : pairs)
    {
        if (s.find("|Host") != std::string::npos
            && d.find("|Base|") != std::string::npos && (a & 0x08) != 0)
        {
            dependent_ok = true;
        }
    }
    cg::test::check(
        r,
        dependent_ok,
        "dependent base -> primary + is_template_base");
    // 非模板基红线：Plain 继承不受影响。
    cg::test::check(
        r,
        has_edge("|P2", "|Plain|"),
        "non-template base unchanged");
    // 实例符号登记：隐式实例（使用点）与显式全特化（声明处）都在。
    bool has_crtp_sym = false;
    bool has_full_spec_sym = false;
    for (const cg::db::IndexRow& row : cg::db::all_symbol_index_rows(store))
    {
        if (row.unique_key.find("Base<Derived>") != std::string::npos)
        {
            has_crtp_sym = true;
        }
        if (row.unique_key.find("Multi<int>") != std::string::npos)
        {
            has_full_spec_sym = true;
        }
    }
    cg::test::check(r, has_crtp_sym, "implicit instance symbol registered");
    cg::test::check(
        r,
        has_full_spec_sym,
        "explicit full specialization symbol registered");
    // 显式全特化发 specializes 指本尊。
    bool spec_edge = false;
    {
        const auto key_of = symbol_keys(store);
        for (const auto& [id, key] : key_of)
        {
            if (key.find("Multi<int>") == std::string::npos)
            {
                continue;
            }
            for (const cg::db::EdgeAggRow& e :
                 cg::db::edges_from(store, id, k_test_spec))
            {
                const auto it = key_of.find(e.other);
                if (it != key_of.end()
                    && it->second.find("|Multi|") != std::string::npos)
                {
                    spec_edge = true;
                }
            }
        }
    }
    cg::test::check(
        r,
        spec_edge,
        "explicit specialization -> specializes primary");
}

const cg::test::Registrar reg_tmpl_inherit{
    "analysis.tmpl.inherit_edges",
    &tmpl_inherit_edges};

// --------------------------------------- 特化摘要翻转（增量升级判据） -----
// hierarchy_digest 必须覆盖 Specializes：净特化边变化且 extends/overrides
// 边集不变的增量批，摘要仍须翻转（否则 dispatch 全站点升级不触发，库内
// 保留按旧折算结构算出的过期派发边）。先红后绿：digest 类型清单缺
// Specializes 时两次摘要相同，本套件红。
void spec_digest_flip(R& r)
{
    const cg::test::ScratchDir scratch{
        std::filesystem::path{"ccg-analysis-scratch"} / "spec-digest-flip"};
    const std::filesystem::path tree = scratch.base / "tree";
    std::filesystem::create_directories(tree);
    std::error_code ec;
    std::filesystem::copy_file(
        std::filesystem::path{fixture("spec-digest-flip")} / "main.cpp",
        tree / "main.cpp",
        ec);
    cg::test::check(r, !ec, "spec-digest-flip corpus seeded");
    {
        std::ofstream cdb{tree / "compile_commands.json"};
        cdb << "[\n  {\n    \"directory\": \".\",\n    \"file\": "
               "\"main.cpp\",\n    \"command\": \"c++ -std=c++23 -I . -I "
               "X:/ccg-fake/toolchain/include/c++/v1 -I "
               "X:/ccg-fake/toolchain/include -c main.cpp -o main.o\"\n  "
               "}\n]\n";
    }

    const auto digest_and_edges = [](db::GraphStore&             store,
                                     std::optional<std::string>& digest,
                                     std::int64_t&               extends_n,
                                     std::int64_t&               overrides_n,
                                     std::int64_t&               specializes_n)
    {
        digest = store.meta("analysis_hierarchy_digest");
        const db::Stats st = db::stats(store);
        extends_n =
            st.edges_by_type[static_cast<std::size_t>(db::EdgeType::Extends)];
        overrides_n =
            st.edges_by_type[static_cast<std::size_t>(db::EdgeType::Overrides)];
        specializes_n = st.edges_by_type[static_cast<std::size_t>(
            db::EdgeType::Specializes)];
    };

    const std::filesystem::path db_file = scratch.base / "spec.db";
    std::optional<std::string>  digest_full;
    std::int64_t                extends_full = 0;
    std::int64_t                overrides_full = 0;
    std::int64_t                specializes_full = 0;
    {
        db::GraphStore          store{db_file.string()};
        cg::graph::IndexOptions opt;
        opt.roots = {cg::util::norm_path(tree.generic_string())};
        opt.progress = false;
        opt.compile_db_path = (tree / "compile_commands.json").string();
        opt.db_path = db_file.string();
        cg::graph::Indexer            indexer{store, std::move(opt)};
        const cg::graph::IndexSummary first = indexer.run(
            cg::clang::load((tree / "compile_commands.json").string(), {}));
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "spec-digest-flip baseline indexes full and clean");
        digest_and_edges(
            store,
            digest_full,
            extends_full,
            overrides_full,
            specializes_full);
        cg::test::check(
            r,
            digest_full.has_value() && specializes_full > 0,
            std::format(
                "baseline digest present and specialization edge emitted "
                "(specializes {})",
                specializes_full));
    }

    // 编辑：追加一个显式全特化（Box<char> 无基类——extends/overrides 边
    // 集不变，净增 specializes 边）。
    {
        std::ofstream edit{tree / "main.cpp", std::ios::app};
        edit << "\n// digest-flip edit: new explicit specialization; "
                "extends/overrides stay untouched.\ntemplate <>\nstruct "
                "Box<char>\n{\n    int describe() const { return 3; }\n};\n"
                "namespace {\nint use_char_box()\n{\n    Box<char> box;\n   "
                " return box.describe();\n}\n}  // namespace\n";
    }

    std::optional<std::string> digest_inc;
    std::int64_t               extends_inc = 0;
    std::int64_t               overrides_inc = 0;
    std::int64_t               specializes_inc = 0;
    {
        db::GraphStore          store{db_file.string()};
        cg::graph::IndexOptions opt;
        opt.roots = {cg::util::norm_path(tree.generic_string())};
        opt.progress = false;
        opt.compile_db_path = (tree / "compile_commands.json").string();
        opt.db_path = db_file.string();
        opt.seed_files = {cg::util::norm_path((tree / "main.cpp").string())};
        cg::graph::Indexer            indexer{store, std::move(opt)};
        const cg::graph::IndexSummary second = indexer.run(
            cg::clang::load((tree / "compile_commands.json").string(), {}));
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            std::format(
                "spec-digest-flip edit batch runs incremental and clean "
                "(mode {}, failed {})",
                second.mode,
                second.failed));
        digest_and_edges(
            store,
            digest_inc,
            extends_inc,
            overrides_inc,
            specializes_inc);
    }

    // 语料形态自证：extends/overrides 边集逐数不变、特化边净增。
    cg::test::check(
        r,
        extends_inc == extends_full && overrides_inc == overrides_full,
        std::format(
            "edit keeps extends/overrides counts ({}/{} -> {}/{})",
            extends_full,
            overrides_full,
            extends_inc,
            overrides_inc));
    cg::test::check(
        r,
        specializes_inc > specializes_full,
        std::format(
            "edit adds a net specialization edge ({} -> {})",
            specializes_full,
            specializes_inc));
    // 红绿核心：特化净变且 extends/overrides 不变时摘要必须翻转。
    cg::test::check(
        r,
        digest_full.has_value() && digest_inc.has_value()
            && *digest_full != *digest_inc,
        "hierarchy digest flips on net specialization change");
}

const cg::test::Registrar reg_spec_digest_flip{
    "analysis.dispatch.spec_digest_flip",
    &spec_digest_flip};

// --------------------------------------------- 回调契约 --------------------

void callback_reg_edges(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "callback_reg");
    const auto key_of = symbol_keys(store);
    // 收集全部 callback_reg 边的 (target 名) 集合。
    std::set<std::string> targets;
    for (const auto& [id, key] : key_of)
    {
        for (const cg::db::EdgeAggRow& e :
             cg::db::edges_from(store, id, k_test_cb))
        {
            const auto it = key_of.find(e.other);
            if (it != key_of.end())
            {
                targets.insert(it->second);
            }
        }
    }
    const auto registered = [&](std::string_view name)
    {
        for (const std::string& t : targets)
        {
            if (t.find(name) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    };
    cg::test::check(
        r,
        registered("|begin_fn"),
        "_beginthread slot 1 addr-of target");
    cg::test::check(
        r,
        registered("|thread_fn"),
        "_beginthreadex/CreateThread slot 3 implicit target");
    cg::test::check(r, registered("|cmp"), "qsort slot 4 target");
    cg::test::check(r, registered("|enum_fn"), "EnumWindows slot 1");
    cg::test::check(r, registered("|hook_fn"), "SetWindowsHookExW slot 2");
    cg::test::check(
        r,
        !registered("|unused_cb"),
        "non-list API gets no contract edge");
    cg::test::check(
        r,
        targets.size() == 5,
        "five distinct callback targets (thread_fn deduped by upsert)");
}

const cg::test::Registrar reg_callback_reg{
    "analysis.callback.reg_edges",
    &callback_reg_edges};

// --------------------------------------------- 间接调用 --------------------

void indirect_instance_flow(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "indirect3");
    cg::test::check(r, sum.failed == 0, "indirect3 indexes cleanly");
    const std::vector<IEdge> edges = indirect_edges(store);
    // 实例桥：use_algo 的 &cb 经界外 algo 实例体的 f(1) 调用点连边。
    // use_algo_multi 给同 host+param 第二目标 -> 该组全部降 candidate。
    cg::test::check(
        r,
        has_indirect_edge(
            edges,
            "|use_algo",
            "|cb",
            cg::db::edge_attrs::indirect_candidate),
        "instance bridge connects use_algo -> cb (candidate: "
        "shared instance has a second target)");
    cg::test::check(
        r,
        has_indirect_edge(
            edges,
            "|use_algo_multi",
            "|cb2",
            cg::db::edge_attrs::indirect_candidate),
        "second caller bridged too");
    // param 1 桥（algo2 的 f(a)）。
    cg::test::check(
        r,
        has_indirect_edge(
            edges,
            "|use_algo2",
            "|cb",
            cg::db::edge_attrs::indirect_exact),
        "algo2 param-1 bridge connects (unique target)");
    // lambda 桥：实参 lambda 登记为符号并成为边目标。
    cg::test::check(
        r,
        has_indirect_edge(
            edges,
            "|use_lambda",
            "operator()@L",
            cg::db::edge_attrs::indirect_exact),
        "lambda argument becomes a bridged target (unique)");
    // 形态 1：局部变量槽（唯一目标 -> exact）。
    cg::test::check(
        r,
        has_indirect_edge(
            edges,
            "|local_var_call",
            "|target_fn",
            cg::db::edge_attrs::indirect_exact),
        "local var slot resolves exactly");
    // 形态 2：形参直配（runner 的 fp() <- call_runner 的 &target_fn）。
    cg::test::check(
        r,
        has_indirect_edge(
            edges,
            "|runner",
            "|target_fn",
            cg::db::edge_attrs::indirect_exact),
        "param passthrough resolves exactly");
    // 形态 4：字段槽（Task::exec）。
    cg::test::check(
        r,
        has_indirect_edge(
            edges,
            "|fire",
            "|target_fn2",
            cg::db::edge_attrs::indirect_exact),
        "field slot (__execute form) resolves exactly");
    // 反例：via_var 的 q 是界外实例（不可连目标），无硬连边。
    bool via_var_edge = false;
    for (const IEdge& e : edges)
    {
        if (e.src.find("|via_var") != std::string::npos)
        {
            via_var_edge = true;
        }
    }
    cg::test::check(
        r,
        !via_var_edge,
        "out-of-roots instance via variable stays unconnected");
}

// --------------------------------------------- 界外字段槽 -----------------

void external_field_slot_flow(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "indirect4");
    cg::test::check(r, sum.failed == 0, "indirect4 indexes cleanly");
    const std::vector<IEdge> edges = indirect_edges(store);
    // 界外字段槽端到端：def（界外构造模板体内 execute_ = 就地 lambda）
    // 与 use（界外 run 循环内 task->execute_(tid)）都发生在界外实例
    // 体内——消费 join 走 "fld#<FieldDecl USR>" 占位。边的 src 按
    // 已验收的链头语义（"沿转发链回溯到链头直传 caller"）=
    // 发起整个链的入口函数（main），不是界外 use 宿主。
    cg::test::check(
        r,
        has_indirect_edge(
            edges,
            "main",
            "operator()@L",
            cg::db::edge_attrs::indirect_exact),
        "external field slot bridges main -> in-place lambda");
    // 反例：def 侧宿主（r8_make_op 实例）不是调用点——其自身无
    // indirect 出边（边在 use 侧）。
    bool make_op_src = false;
    for (const IEdge& e : edges)
    {
        if (e.src.find("r8_make_op") != std::string::npos)
        {
            make_op_src = true;
        }
    }
    cg::test::check(
        r,
        !make_op_src,
        "def-side host emits no edge (edge lives on use side)");
}

const cg::test::Registrar reg_indirect_instance_flow{
    "analysis.indirect.instance_flow",
    &indirect_instance_flow};

// 归因定案：界外字段槽的"CLI 零落"=双重根因——(a) 实例化体里 lambda→
// 函数指针隐式转换被显式化（转换 CallExpr 三层包裹，探针实证），
// function_target_key 剥不到（def 零产生）；(b) def 修复后消费侧
// heads 可空 → edges.site_file=0 触发 FK（整事务回滚）。修复=转换
// 层宽度受限下探 + 界外 lambda 符号挂靠 TU 主文件（flags 0x200）+
// 边站点三级 fallback（用户行→invoke→目标符号文件）。
const cg::test::Registrar reg_external_field_slot{
    "analysis.external.field_slot",
    &external_field_slot_flow};

const cg::test::Registrar reg_slot_target_cap{
    "analysis.slot.target_cap",
    &slot_target_cap_bounds};
const cg::test::Registrar reg_slot_mixed_roundtrip{
    "analysis.slot.mixed_key_roundtrip",
    &slot_mixed_key_roundtrip};

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }

// ------------------------------------------ 重载归属裁决 ------------------

// 同名"非模板+模板"并存（IsFailed 形态）：模板上下文的依赖调用
// （clang 解析到模板版但隐式实例符号不入库）经名字段兜底会命中入库
// 序第一个（非模板版）——实测定性的"错连"在夹具上可复现；随后的
// arg_dep（实参 canonical 含 type-parameter）让解析侧在恰一模板候选
// 时选模板版（conf 0.75）。具体实参调用不受影响（第 1 段 sig_hash
// 精确命中）。注：实测大库的 Acme::IsFailed 4378 条经查实参是具体类型
// （qi_result=QueryInterface 的返回值），连接本就正确——先前诊断的
// "模板版 0 边"是缺口误判，本机制是防未来真实错连的护栏，pristine 上按
// 宁缺原序不触发（行为正确）。
void overload_arg_dep_resolution(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "overload");
    cg::test::check(r, sum.failed == 0, "overload indexes cleanly");
    const db::FindResult found =
        db::find_symbols(store, "IsFailed", db::SymbolFilter{});
    cg::test::check(
        r,
        found.matches.size() == 2,
        "both IsFailed overloads are symbols");
    std::optional<cg::db::SymId> plain, tmpl;
    for (const db::SymbolRow& m : found.matches)
    {
        if ((m.flags & db::sym_flags::is_template) != 0)
        {
            tmpl = m.id;
        }
        else
        {
            plain = m.id;
        }
    }
    cg::test::check(
        r,
        plain.has_value() && tmpl.has_value(),
        "one plain + one template overload");
    const auto sym_of = [&](std::string_view q) -> cg::db::SymId
    {
        const db::FindResult fr = db::find_symbols(
            store,
            q,
            db::SymbolFilter{},
            /*merge_lower_tiers=*/false);
        return fr.matches.size() == 1 ? fr.matches.front().id
                                      : cg::db::SymId{0};
    };
    const cg::db::SymId host_t = sym_of("host_t");
    const cg::db::SymId host_c = sym_of("host_c");
    cg::test::check(r, host_t != 0 && host_c != 0, "both hosts are symbols");
    if (host_t && host_c && plain && tmpl)
    {
        bool t_to_tmpl = false, t_to_plain = false;
        for (const db::EdgeRow& e :
             db::edges_of(store, host_t, true, db::EdgeType::Calls))
        {
            if (e.dst == *tmpl)
            {
                t_to_tmpl = true;
            }
            if (e.dst == *plain)
            {
                t_to_plain = true;
            }
        }
        cg::test::check(
            r,
            t_to_tmpl,
            "dependent-arg call resolves to the template "
            "overload");
        cg::test::check(
            r,
            !t_to_plain,
            "dependent-arg call no longer lands on the plain "
            "overload");
        bool c_to_plain = false;
        for (const db::EdgeRow& e :
             db::edges_of(store, host_c, true, db::EdgeType::Calls))
        {
            if (e.dst == *plain)
            {
                c_to_plain = true;
            }
        }
        cg::test::check(
            r,
            c_to_plain,
            "concrete-arg call still resolves to the plain "
            "overload");
    }
}

const cg::test::Registrar reg_overload_arg_dep{
    "analysis.overload.arg_dep",
    &overload_arg_dep_resolution};

// ------------------------------------------------------------ 件 1-4 -------

/**
 * @brief 件一：接收者缺位（裸 this）回退调用方所属记录 + 叶子记录的
 *        nearest-override 静态决议；模板记录调用方必须拒收。
 */
void implicit_this_resolution(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "dispatch-evidence");
    cg::test::check(r, sum.failed == 0, "dispatch-evidence indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);
    // 正例：QueryInterface 尾部裸 AddRef()——无接收者表达式，静态根
    // 回退到 DirectorLeaf（叶子），家族多实现经 nearest 收敛到自身
    // 覆盖，1.0 + reason=implicit_this_unique。
    const DEdge* qi = find_edge(
        edges,
        "DirectorLeaf::QueryInterface",
        "DirectorLeaf::AddRef");
    cg::test::check(
        r,
        qi != nullptr && qi->confidence == 1.0,
        "bare-this AddRef resolves at 1.0 via caller record");
    const auto qi_site =
        site_named(store, "DirectorLeaf::QueryInterface", "AddRef");
    cg::test::check(
        r,
        qi_site.has_value() && qi_site->resolved
            && qi_site->candidates.find("implicit_this_unique")
                   != std::string::npos,
        "bare-this site carries reason=implicit_this_unique");
    // 负例：模板记录（TplCaller）成员函数内的裸 this——实例化间的动态
    // 类型互异，回退必须拒收（只允许 0.0 家族标记边）。
    const DEdge* tpl = find_edge(edges, "TplCaller::call", "TplDerived::virt");
    cg::test::check(
        r,
        tpl == nullptr || tpl->confidence == 0.0,
        "template-record implicit this never yields an "
        "inferred edge");
}

/** @brief 件三：透明转发范式（零名字白名单）识别自定义转发助手。 */
void transparent_forwarder(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "dispatch-evidence");
    const std::vector<DEdge> edges = dispatch_edges(store);
    // 自定义右值转发器 pipe(p) 与 std::move(p) 同款：实参槽直通。虚
    // 站点在 take_pipe 体内（边归属方法而非驱动函数，与透传壳字段
    // 站点同款归属口径）。
    const DEdge* piped = find_edge(edges, "take_pipe", "FwdImpl::f");
    cg::test::check(
        r,
        piped != nullptr && piped->confidence == 0.7,
        "custom rvalue forwarder transports the argument slot "
        "(0.7)");
    // 左值转发器 hand_off(x)：调用点的实参变量被剥出。
    const auto hand = site_named(store, "lvalue_forwarder_probe", "hand_off");
    cg::test::check(
        r,
        hand.has_value() && hand->arg_vars.find("\"x\"") != std::string::npos,
        "custom lvalue forwarder strips to its argument variable");
    // 负例：按值返回的转换函数不是转发器——嵌套实参的调用表达式形态
    // 必须保留（外层站点的实参槽不得被提取成内层变量名）。
    const auto val =
        site_named(store, "value_cast_negative", "consume_by_value");
    cg::test::check(
        r,
        val.has_value() && val->arg_vars.find("\"v\"") == std::string::npos,
        "value-returning cast is refused (no argument slot)");
}

/** @brief 件二：同一出参槽的多写回源并集（首个写点不再吞掉后续源）。 */
void writeback_union(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "dispatch-evidence");
    const std::vector<DEdge> edges = dispatch_edges(store);
    // fill_two_writes 有两个 *pp = ... 写点（a / b）：出参槽的行级多源
    // 并集必须同时保留 OutA 与 OutB；消费站点是多类型站点，按"置信度
    // 取低者"纪律降档发两条 0.4 边（宁降档不吞值，绝不伪装唯一）。
    const DEdge* a = find_edge(edges, "consume_two_writes", "OutA::g");
    const DEdge* b = find_edge(edges, "consume_two_writes", "OutB::g");
    cg::test::check(
        r,
        a != nullptr && a->confidence == 0.4,
        "first write-back source stays in the union (0.4)");
    cg::test::check(
        r,
        b != nullptr && b->confidence == 0.4,
        "second write-back source joins the union (0.4)");
}

/** @brief 虚族并集写回：直连槽无行的虚装填以 override 族同位槽并集回传。 */
void virtual_family_writeback(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "virtual-family-writeback");
    cg::test::check(
        r,
        sum.failed == 0,
        "virtual-family-writeback indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);
    // 手写多实现族：filler->Fill(&r) 的被调解析到 IVFiller::Fill 声明
    // （无函数体无写回行），族内 AppleFiller/BananaFiller 的同位槽各带
    // 实现类——r->Name() 站点拿 [Apple, Banana] 并集，may 超集按 0.4
    // 档发两条候选边（不伪装唯一）。
    const DEdge* apple = find_edge(edges, "consume_two_impls", "Apple::Name");
    const DEdge* banana = find_edge(edges, "consume_two_impls", "Banana::Name");
    cg::test::check(
        r,
        apple != nullptr && apple->confidence == 0.4,
        "virtual family union yields the Apple candidate (0.4)");
    cg::test::check(
        r,
        banana != nullptr && banana->confidence == 0.4,
        "virtual family union yields the Banana candidate (0.4)");
    // 披露来源：族并集边的 reason 标注 out_param_family（多类型站点的
    // 槽 reason 经 multiple_reason 展平）。
    const auto two = site_named(store, "consume_two_impls", "Name");
    cg::test::check(
        r,
        two.has_value()
            && two->candidates.find("out_param_family") != std::string::npos,
        "family write-back is disclosed via reason=out_param_family");
    // 生成代码同机制：stub 体内 impl->Fill(&out) 直转真实实现并就近消
    // 费——与手写同一查询视野救活，不按生成代码降级。
    const DEdge* stub_apple =
        find_edge(edges, "stub_consume_and_use", "Apple::Name");
    const DEdge* stub_banana =
        find_edge(edges, "stub_consume_and_use", "Banana::Name");
    cg::test::check(
        r,
        stub_apple != nullptr && stub_apple->confidence == 0.4,
        "stub passthrough reaches the Apple candidate (0.4)");
    cg::test::check(
        r,
        stub_banana != nullptr && stub_banana->confidence == 0.4,
        "stub passthrough reaches the Banana candidate (0.4)");
    // 单实现族：并集唯一实现类，决议端 nearest 唯一收敛升 0.7（族边本
    // 身不提档，档位来自既有收敛护栏）。
    const DEdge* single =
        find_edge(edges, "consume_single_impl", "Apple::Name");
    cg::test::check(
        r,
        single != nullptr && single->confidence == 0.7,
        "single-implementation family converges to one target (0.7)");
    // 负例一：族内全部成员都无写回行（证据不足不连）——站点不决议、
    // 不发任何派发边（静态侧子树收窄的展示候选不构成派发证据，是既有
    // 保守披露），绝不连出错误目标。
    const auto none = site_named(store, "consume_no_write", "Name");
    cg::test::check(
        r,
        none.has_value() && !none->resolved,
        "no write-back rows in the family means no resolution");
    cg::test::check(
        r,
        find_edge(edges, "consume_no_write", "Apple::Name") == nullptr
            && find_edge(edges, "consume_no_write", "Banana::Name") == nullptr,
        "no write-back rows in the family means no dispatch edge");
    // 负例二：跨族隔离——具体类直连装填只回传本函数写回类型，其它
    // 家族的实现类不得混入候选。
    const DEdge* red = find_edge(edges, "consume_unrelated", "RedColor::Hue");
    const auto   unrelated = site_named(store, "consume_unrelated", "Hue");
    cg::test::check(
        r,
        red != nullptr,
        "direct write-back keeps resolving unrelated families");
    cg::test::check(
        r,
        unrelated.has_value()
            && unrelated->candidates.find("Apple") == std::string::npos
            && unrelated->candidates.find("Banana") == std::string::npos,
        "other families never leak into direct write-back candidates");
    // 静态面裁剪：QI 快速路径形态——出参静态类型是具体类时，族并集
    // [Apple, Banana] 被静态声明面（Apple 及其后代）裁到 [Apple]，静
    // 态可证的唯一性不被巨族 may 集稀释（Banana 不进候选）。
    const auto clipped = site_named(store, "consume_static_clip", "Name");
    cg::test::check(
        r,
        clipped.has_value() && clipped->resolved
            && clipped->candidates.find("Banana") == std::string::npos,
        "concrete out-param clips the family union to its static face");
    // 壳槽静态面裁剪：装填实参经 TPtr<Apple>::Put()（取址槽形态定位壳
    // 变量槽），壳槽静态面 = 主模板键 + pointee 实参键（Apple）——主模
    // 板键不是语义类型面（跳过、由实参键立界），族并集 [Apple, Banana]
    // 被裁到 [Apple]，壳槽不再是 may 并集的无界扩散口（唯一收敛 0.7，
    // Banana 不进候选也不发边）。
    const DEdge* shell = find_edge(edges, "consume_shell_clip", "Apple::Name");
    cg::test::check(
        r,
        shell != nullptr && shell->confidence == 0.7,
        "shell slot clips the family union via its pointee face (0.7)");
    cg::test::check(
        r,
        find_edge(edges, "consume_shell_clip", "Banana::Name") == nullptr,
        "out-of-face type never leaks into the shell slot");
    const auto shell_site = site_named(store, "consume_shell_clip", "Name");
    cg::test::check(
        r,
        shell_site.has_value() && shell_site->resolved
            && shell_site->candidates.find("Banana") == std::string::npos,
        "shell slot stays unique after the semantic-face clip");
    // 多级继承链：链根与中间层均为纯虚声明（族内一层邻居无写回行），
    // 完整双向闭包必须跨过中间层到达叶子实现的写回行——闭包退化成
    // "自身 + 直接邻居"一层时链根站点拿不到任何证据（漏救），此处
    // 必须发两条 0.4 候选边。
    const DEdge* chain_apple =
        find_edge(edges, "consume_chain_root", "Apple::Name");
    const DEdge* chain_banana =
        find_edge(edges, "consume_chain_root", "Banana::Name");
    cg::test::check(
        r,
        chain_apple != nullptr && chain_apple->confidence == 0.4,
        "multi-level chain closure reaches the leaf write-back (Apple)");
    cg::test::check(
        r,
        chain_banana != nullptr && chain_banana->confidence == 0.4,
        "multi-level chain closure reaches the leaf write-back (Banana)");
    // 链中间层消费（一层即达叶子）：完整闭包不得回退一层可达形态。
    const DEdge* mid_apple =
        find_edge(edges, "consume_chain_mid", "Apple::Name");
    const DEdge* mid_banana =
        find_edge(edges, "consume_chain_mid", "Banana::Name");
    cg::test::check(
        r,
        mid_apple != nullptr && mid_apple->confidence == 0.4,
        "mid-level entry keeps its direct candidates after the fix");
    cg::test::check(
        r,
        mid_banana != nullptr && mid_banana->confidence == 0.4,
        "mid-level entry keeps its direct candidates after the fix");
    // CRTP 模板特化基形态：实现类 KiwiImpl 经特化行 TFruitBase<KiwiImpl>
    // 继承接口（extends 边挂在主模板行上、特化行无 extends 出边）——
    // 后代闭包只走 extends 边时 allowed 面不含真实现。混合批 [ProxyFruit
    // （直连继承）, KiwiImpl（特化路径）] 是部分盲区形态：批内有面内类
    // 型时"全批面外整体不裁"的守卫不触发，闭包必须把特化行经
    // specializes 反向折算为结构派生，KiwiImpl 才不被裁掉饿死。
    const DEdge* kiwi =
        find_edge(edges, "consume_crtp_shell", "KiwiImpl::Kind");
    cg::test::check(
        r,
        kiwi != nullptr && kiwi->confidence == 0.4,
        "CRTP specialization-base implementation stays in the family "
        "face (0.4)");
    const DEdge* proxy =
        find_edge(edges, "consume_crtp_shell", "ProxyFruit::Kind");
    cg::test::check(
        r,
        proxy != nullptr && proxy->confidence == 0.4,
        "direct-extender sibling keeps its candidate (0.4)");
    const auto crtp = site_named(store, "consume_crtp_shell", "Kind");
    cg::test::check(
        r,
        crtp.has_value() && crtp->resolved,
        "CRTP shell slot stays resolved with both implementations");
}

/** @brief 虚族扇投入参：直连槽不存在的虚调用以 override 族同位槽收实参批。 */
void virtual_family_param_fanout(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "virtual-family-param-fanout");
    cg::test::check(
        r,
        sum.failed == 0,
        "virtual-family-param-fanout indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);
    // StringImpl 代理穿透形态：proxy_keep 经接口指针调用（被调解析到
    // IStore::Keep 声明，无体无槽），payload 的动态并集 [PayloadA,
    // PayloadB] 扇投进族内每个有体 Keep 的 #p0 槽——两个实现体内的
    // p->Name() 站点各得双候选，may 超集按 0.4 档发候选边（不伪装唯一）。
    const DEdge* a_pa = find_edge(edges, "StoreA::Keep", "PayloadA::Name");
    const DEdge* a_pb = find_edge(edges, "StoreA::Keep", "PayloadB::Name");
    const DEdge* b_pa = find_edge(edges, "StoreB::Keep", "PayloadA::Name");
    const DEdge* b_pb = find_edge(edges, "StoreB::Keep", "PayloadB::Name");
    cg::test::check(
        r,
        a_pa != nullptr && a_pa->confidence == 0.4,
        "param fan-out yields the StoreA/PayloadA candidate (0.4)");
    cg::test::check(
        r,
        a_pb != nullptr && a_pb->confidence == 0.4,
        "param fan-out yields the StoreA/PayloadB candidate (0.4)");
    cg::test::check(
        r,
        b_pa != nullptr && b_pa->confidence == 0.4,
        "param fan-out yields the StoreB/PayloadA candidate (0.4)");
    cg::test::check(
        r,
        b_pb != nullptr && b_pb->confidence == 0.4,
        "param fan-out yields the StoreB/PayloadB candidate (0.4)");
    // 披露来源：被扇投槽的 reason 记名 param_flow_family（族并集救活
    // 面与直连流在披露层可区分）。
    const auto fed = site_named(store, "StoreA::Keep", "Name");
    cg::test::check(
        r,
        fed.has_value()
            && fed->candidates.find("param_flow_family") != std::string::npos,
        "param fan-out is disclosed via reason=param_flow_family");
    // 负例一：具体类接收者直连不扇投——NegaA 只进 NegaStoreA 的 #p0
    // （单候选唯一收敛 0.7），兄弟实现 NegaStoreB 体内站点不得拿到
    // NegaA 证据（已唯一解析站点走精确通道，不被 may 并集污染）。
    const DEdge* direct = find_edge(edges, "NegaStoreA::Keep", "NegaA::Tag");
    cg::test::check(
        r,
        direct != nullptr && direct->confidence == 0.7,
        "concrete receiver keeps the direct precise channel (0.7)");
    // 兄弟站点的证据面照旧不收扇投（静态决议启用前无任何 NegaA::Tag 边）：其
    // 无动态证据站点按宿主抽象直连（Nega::Tag 纯虚 × 全库唯一非纯
    // override），High 直连与扇投污染是两个不相干的面——污染守恒由
    // 下方 reason 断言钉住（无 param_flow_family 披露）。
    const DEdge* sibling = find_edge(edges, "NegaStoreB::Keep", "NegaA::Tag");
    cg::test::check(
        r,
        sibling != nullptr && sibling->confidence == 1.0,
        "the sibling site resolves via the abstract-host link (1.0)");
    const auto sibling_site = site_named(store, "NegaStoreB::Keep", "Tag");
    cg::test::check(
        r,
        sibling_site.has_value() && sibling_site->resolved
            && sibling_site->candidates.find("param_flow_family")
                   == std::string::npos
            && sibling_site->candidates.find("family_unique_override")
                   != std::string::npos,
        "the sibling site is abstract-host resolved, never fan-out fed");
    // 负例二：族内无体成员（声明无定义）无槽不投——DeclStoreA 体内
    // 没有任何站点证据，也不影响有体兄弟 DeclStoreB 照收扇投（单次
    // 调用单候选，唯一收敛 0.7）。
    cg::test::check(
        r,
        find_edge(edges, "DeclStoreA::Keep", "PayloadA::Name") == nullptr
            && find_edge(edges, "DeclStoreA::Keep", "PayloadB::Name")
                   == nullptr,
        "bodyless family member receives no fan-out");
    const DEdge* decl = find_edge(edges, "DeclStoreB::Keep", "PayloadA::Name");
    cg::test::check(
        r,
        decl != nullptr && decl->confidence == 0.7,
        "bodied sibling still receives the fan-out (0.7)");
    // 负例三：跨族隔离——不相关接口的实现类不得混入 Keep 族的候选。
    const auto isolated = site_named(store, "StoreA::Keep", "Name");
    cg::test::check(
        r,
        isolated.has_value()
            && isolated->candidates.find("RedColor") == std::string::npos,
        "unrelated families never leak into fan-out candidates");
    // 多级继承链：链根与中间层均为纯虚声明（族内一层邻居无体无槽），
    // 完整双向闭包必须跨过中间层到达叶子实现的形参槽——闭包退化成
    // "自身 + 直接邻居"一层时链根站点拿不到任何证据（漏救）。
    const DEdge* chain_pa =
        find_edge(edges, "ChainStoreA::Keep", "PayloadA::Name");
    const DEdge* chain_pb =
        find_edge(edges, "ChainStoreB::Keep", "PayloadB::Name");
    cg::test::check(
        r,
        chain_pa != nullptr && chain_pa->confidence == 0.4,
        "multi-level chain closure reaches the leaf param slot (A)");
    cg::test::check(
        r,
        chain_pb != nullptr && chain_pb->confidence == 0.4,
        "multi-level chain closure reaches the leaf param slot (B)");
}

/** @brief 件四：壳 move 字段贯穿（std::move 包壳变量 → 字段槽）；非
 *        字段 LHS 不得写字段槽。 */
void shell_move_field(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "dispatch-evidence");
    const std::vector<DEdge> edges = dispatch_edges(store);
    // h.f_ = std::move(s)：壳变量的 pointee 集搬进字段槽，字段站点
    // （HolderMoveField::use 的 f_->f()）获得动态证据。
    const DEdge* moved = find_edge(edges, "HolderMoveField::use", "FwdImpl::f");
    cg::test::check(
        r,
        moved != nullptr && moved->confidence == 0.7,
        "shell-move field transport reaches the field site "
        "(0.7)");
    // 负例：局部变量 LHS（b = std::move(a)）不是字段贯穿——字段槽的
    // FieldFlow 行必须只有写侧穿透那一条（非字段 LHS 一条都不加）。
    std::size_t field_flow_rows = 0;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        if (row.source == static_cast<int>(cg::analysis::HintSource::FieldFlow)
            && row.var_name == "f_")
        {
            ++field_flow_rows;
        }
    }
    cg::test::check(
        r,
        field_flow_rows == 1,
        "non-field LHS adds no FieldFlow row");
}

const cg::test::Registrar reg_implicit_this{
    "analysis.implicit_this",
    &implicit_this_resolution};
const cg::test::Registrar reg_transparent_forwarder{
    "analysis.transparent_forwarder",
    &transparent_forwarder};
const cg::test::Registrar reg_writeback_union{
    "analysis.writeback_union",
    &writeback_union};
const cg::test::Registrar reg_virtual_family_writeback{
    "analysis.virtual_family_writeback",
    &virtual_family_writeback};
const cg::test::Registrar reg_virtual_family_param_fanout{
    "analysis.virtual_family_param_fanout",
    &virtual_family_param_fanout};

/**
 * @brief COM 根层巨族（AddRef/Release/QueryInterface）的接收者可达过
 *        滤：动态类型集被污染时多候选里接收者静态类型后代闭包之外的
 *        实现是运行时不可能目标，整族披露形态下被剔除。
 * @internal 负例一：接口静态根（闭包=全体实现）判据空转不改任何候选；
 *           负例二：全部候选不可达（闭包剔空）回退全候选——可达判据
 *           自身可能有 extends 缺边盲区，宁保持披露；单候选/已收敛唯
 *           一站点不启用过滤（收敛护栏已工作，不添乱）。CRTP 模板特
 *           化基形态：可达闭包把特化行经 specializes 反向折算为结构
 *           派生，特化路径真实现不被当闭包外误砍（部分盲区：全滤空
 *           回退只救全剔空形态）。
 */
void root_family_receiver_scope(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "recv-scope");
    cg::test::check(r, sum.failed == 0, "recv-scope indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);
    // 污染形态：IPic* 接收者槽的动态集混入闭包外实现（C 风格转型保留
    // 操作数证据）——Release 多候选里 ToneA::Release 对 IPic* 接收者是
    // 运行时不可能目标，可达过滤剔除（边与候选双面）；PicA 同在闭包
    // 内保留，may 档 0.4 不伪装唯一。
    const DEdge* pic = find_edge(edges, "polluted_release", "PicA::Release");
    cg::test::check(
        r,
        pic != nullptr && pic->confidence == 0.4,
        "reachable candidate survives the receiver-scope clip (0.4)");
    cg::test::check(
        r,
        find_edge(edges, "polluted_release", "ToneA::Release") == nullptr,
        "out-of-scope implementation never gets an edge");
    const auto polluted = site_named(store, "polluted_release", "Release");
    cg::test::check(
        r,
        polluted.has_value() && polluted->resolved
            && polluted->candidates.find("ToneA") == std::string::npos
            && polluted->candidates.find("receiver_scope_narrowed")
                   != std::string::npos,
        "the clip is disclosed via reason=receiver_scope_narrowed");
    // 负例一：根协议静态根（闭包 = 全体实现）——判据空转，全部候选
    // 原样保留、披露后缀不出现（判据无贡献时不改写披露面）。
    const DEdge* root_pic = find_edge(edges, "root_release", "PicA::Release");
    const DEdge* root_tone = find_edge(edges, "root_release", "ToneA::Release");
    cg::test::check(
        r,
        root_pic != nullptr && root_pic->confidence == 0.4
            && root_tone != nullptr && root_tone->confidence == 0.4,
        "root-protocol receiver keeps its full-family disclosure");
    const auto rooted = site_named(store, "root_release", "Release");
    cg::test::check(
        r,
        rooted.has_value()
            && rooted->candidates.find("receiver_scope_narrowed")
                   == std::string::npos,
        "no-op scope check adds no disclosure suffix");
    // 负例二：全部候选不可达（IPic* 静态根收到两个 Tone 实现）——剔空
    // 回退全候选，两条候选边原样保留（宁保持披露）。
    cg::test::check(
        r,
        find_edge(edges, "orphan_release", "ToneA::Release") != nullptr
            && find_edge(edges, "orphan_release", "ToneB::Release") != nullptr,
        "fully-out-of-scope candidates fall back to the full set");
    // 单候选形态：唯一实现收敛 0.7（nearest_converged 既有路径），过
    // 滤不启用、档位与 reason 原样。
    const DEdge* single = find_edge(edges, "single_release", "PicA::Release");
    cg::test::check(
        r,
        single != nullptr && single->confidence == 0.7,
        "converged single candidate keeps its 0.7 channel untouched");
    // CRTP 模板特化基形态：GearImpl 经特化行 TGearBase<GearImpl> 继承
    // IGear（extends 边挂在主模板行上）——可达闭包折算特化行后真实现
    // 留在界内；部分盲区形态（动态集 = 界内直连实现 DirectGear + 界外
    // ToneA + 特化路径 GearImpl）下全滤空回退救不了被误砍的 GearImpl，
    // 只有 specializes 反向折算能保住它；界外 ToneA 照常被剔。
    const DEdge* gear = find_edge(edges, "crtp_release", "GearImpl::Release");
    cg::test::check(
        r,
        gear != nullptr && gear->confidence == 0.4,
        "specialization-base implementation stays reachable (0.4)");
    const DEdge* direct_gear =
        find_edge(edges, "crtp_release", "DirectGear::Release");
    cg::test::check(
        r,
        direct_gear != nullptr && direct_gear->confidence == 0.4,
        "direct extender keeps its narrowed candidate (0.4)");
    cg::test::check(
        r,
        find_edge(edges, "crtp_release", "ToneA::Release") == nullptr,
        "out-of-scope implementation is still narrowed away");
    const auto crtp = site_named(store, "crtp_release", "Release");
    cg::test::check(
        r,
        crtp.has_value() && crtp->resolved
            && crtp->candidates.find("receiver_scope_narrowed")
                   != std::string::npos
            && crtp->candidates.find("ToneA") == std::string::npos,
        "CRTP receiver scope keeps the real implementation in scope");
}

const cg::test::Registrar reg_root_family_receiver_scope{
    "analysis.root_family_receiver_scope",
    &root_family_receiver_scope};

/**
 * @brief receiver_static 列的分离语义：parse 首写接收者的纯声明静态
 *        类型，dispatch 写回只扩 receiver_types（静态∪动态披露并集），
 *        静态判证列在整轮提交后保持纯净。
 * @internal 一列双载是温启动批边数膨胀的根因。整轮 index（parse 首写
 *           + dispatch 写回）之后静态列必须仍只含声明面：写回若把
 *           并集覆写进静态列，Polluted 站点的动态类型（PicA/ToneA 形参
 *           槽污染面）就会在场，本例红；parse 漏写静态列，IPic 缺席，
 *           同红。root_release 对照站（静态根 IRoot）同理两列分离。
 */
void receiver_static_column_purity(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "recv-scope");
    cg::test::check(r, sum.failed == 0, "recv-scope indexes cleanly");
    const auto polluted = site_named(store, "polluted_release", "Release");
    cg::test::check(r, polluted.has_value(), "polluted_release site present");
    if (polluted.has_value())
    {
        cg::test::check(
            r,
            polluted->receiver_static.find("|IPic|") != std::string::npos
                && polluted->receiver_static.find("PicA") == std::string::npos
                && polluted->receiver_static.find("ToneA") == std::string::npos,
            "receiver_static keeps the declared-only face after the "
            "dispatch write-back");
        cg::test::check(
            r,
            polluted->receiver_types.find("|IPic|") != std::string::npos
                && polluted->receiver_types.find("|PicA|") != std::string::npos
                && polluted->receiver_types.find("|ToneA|")
                       != std::string::npos,
            "receiver_types carries the static-union-dynamic disclosure");
    }
    const auto rooted = site_named(store, "root_release", "Release");
    cg::test::check(
        r,
        rooted.has_value()
            && rooted->receiver_static.find("|IRoot|") != std::string::npos
            && rooted->receiver_static.find("|PicA|") == std::string::npos,
        "root-protocol site keeps its static column declared-only");
}

const cg::test::Registrar reg_receiver_static_purity{
    "analysis.receiver_static_column_purity",
    &receiver_static_column_purity};

/**
 * @brief 派发装载的静态判证面读 receiver_static 列：披露列
 *        receiver_types 被篡改成罩住全族的宽并集后重跑，接收者可达
 *        收窄行为必须纹丝不动。
 * @internal 篡改模拟温启动库上轮写回的更宽并集（修复前披露列被装载
 *           反读当静态面：闭包罩住全部候选，scoped==candidates，收窄
 *           分支不触发，ToneA 假边整族披露）。静态列保持 [IPic] 时
 *           ToneA 必须仍被剔出候选。
 */
void dispatch_static_face_reads_static_column(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "recv-scope");
    cg::test::check(r, sum.failed == 0, "recv-scope indexes cleanly");
    const auto polluted = site_named(store, "polluted_release", "Release");
    cg::test::check(r, polluted.has_value(), "polluted_release site present");
    if (!polluted.has_value())
    {
        return;
    }
    // 全族宽并集（IRoot 生态全体实现 + 接口），拼进披露列——闭包罩住
    // Release 族的全部候选（PicA/PicB/ToneA/ToneB）。
    std::vector<std::string> family;
    for (const std::string_view name :
         {"IRoot", "IPic", "PicA", "PicB", "ITone", "ToneA", "ToneB"})
    {
        const cg::db::SymId id = symbol_id(store, name);
        const auto          row = cg::db::get_symbol(store, id);
        if (row.has_value())
        {
            family.push_back(row->unique_key);
        }
    }
    cg::test::check(
        r,
        family.size() == 7,
        "the whole IRoot family resolves to keys");
    store.exec(
        std::format(
            "UPDATE call_sites SET receiver_types='{}' WHERE site_id={}",
            cg::analysis::json_string_array(family),
            polluted->id));
    const cg::analysis::StubTypeOracle   oracle;
    const cg::analysis::PropagationState state =
        cg::analysis::run(store, oracle);
    const cg::analysis::DispatchResult dispatch =
        cg::analysis::resolve(store, state);
    const cg::analysis::ResolvedCallSite* site = nullptr;
    for (const cg::analysis::ResolvedCallSite& s : dispatch.sites)
    {
        if (s.site_id == polluted->id)
        {
            site = &s;
        }
    }
    bool has_tone = false;
    bool narrowed = false;
    if (site != nullptr)
    {
        for (const cg::analysis::DispatchCandidate& c : site->candidates)
        {
            has_tone =
                has_tone || c.target_key.find("|ToneA|") != std::string::npos;
            narrowed = narrowed
                       || c.reason.find("receiver_scope_narrowed")
                              != std::string::npos;
        }
    }
    cg::test::check(
        r,
        site != nullptr && site->resolved && !has_tone && narrowed,
        "the static face stays clean no matter how wide the disclosure "
        "column is");
}

const cg::test::Registrar reg_static_face_source{
    "analysis.dispatch_static_face_reads_static_column",
    &dispatch_static_face_reads_static_column};
const cg::test::Registrar reg_shell_move_field{
    "analysis.shell_move_field",
    &shell_move_field};

// ------------------------------------------------- 派发侧接收面小件 --

/**
 * @brief 壳剥 pointee 实参：接收者声明类型是透传壳类
 *        模板实例时，壳的 pointee 实参记录 key 追加进静态类型——叶子
 *        实参经家族唯一覆盖唯一决议；界外壳（标准库）自身 key 为空、
 *        实参界内时由实参独自承载静态面。
 * @internal 负例一：壳实参接口有多实现——补根后走既有降档路径不猜
 *           （收窄候选不发推断边）。负例二：非类型形参前置的壳（own
 *           形参下标域拒绝，定位不到可剥的实参位）不得追加。
 */
void recvshell_pointee_resolution(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "recvshell");
    cg::test::check(r, sum.failed == 0, "recvshell indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);
    // 正例：界内壳 + 叶子实参 → 静态类型同时携带壳与实参两键，家族
    // 唯一非纯覆盖唯一决议（1.0）。
    const DEdge* leaf = find_edge(edges, "shell_leaf", "Leaf::f");
    cg::test::check(
        r,
        leaf != nullptr && leaf->confidence == 1.0,
        "own shell with leaf pointee resolves at 1.0");
    const auto leaf_site = site_named(store, "shell_leaf", "f");
    cg::test::check(
        r,
        leaf_site.has_value()
            && leaf_site->receiver_types.find("OwnShell") != std::string::npos
            && leaf_site->receiver_types.find("Leaf") != std::string::npos,
        "shell site carries both shell and pointee keys");
    // 形态：界外壳（标准库）——壳 key 界外为空；本 libclang 对界外主模
    // 板的 pattern/定义游标均不暴露成员，透传判据无从取数（漏收方向），
    // 站点由工厂返回槽的动态证据决议（既有机制，不依赖剥壳）。
    const DEdge* std_leaf = find_edge(edges, "std_shell_leaf", "Leaf::f");
    cg::test::check(
        r,
        std_leaf != nullptr && std_leaf->confidence >= 0.7,
        "std shell leaf pointee resolves via factory evidence");
    const auto std_param = site_named(store, "std_shell_param", "f");
    cg::test::check(
        r,
        std_param.has_value() && !std_param->resolved
            && std_param->receiver_types.find("Leaf") == std::string::npos,
        "std shell param is never unwrapped (external pattern invisible)");
    // 形态：字段壳（隐式 this 字段接收者）同款剥壳决议。
    const DEdge* held = find_edge(edges, "Holder::use", "Leaf::f");
    cg::test::check(
        r,
        held != nullptr && held->confidence == 1.0,
        "shell field receiver resolves at 1.0");
    // 形态：壳 + 构造动态证据（new Impl2a 直证据）——剥壳键在场（静
    // 态面三键），决议由既有动态路径给出 1.0。
    const DEdge* multi = find_edge(edges, "shell_multi", "Impl2a::g");
    cg::test::check(
        r,
        multi != nullptr && multi->confidence == 1.0,
        "shell with construction evidence resolves at 1.0");
    const auto multi_site = site_named(store, "shell_multi", "g");
    cg::test::check(
        r,
        multi_site.has_value()
            && multi_site->receiver_types.find("IAbst2") != std::string::npos,
        "unwrapped interface key joins the shell site static face");
    // 负例：壳形参无构造/工厂证据——壳自身键作动态证据与家族无交集，
    // 剥壳键在场仍降档不猜：无推断边、站点不 resolved。
    const DEdge* multi_param =
        find_edge(edges, "shell_multi_param", "Impl2a::g");
    cg::test::check(
        r,
        multi_param == nullptr || multi_param->confidence == 0.0,
        "multi-impl shell param never yields an inferred edge");
    const auto mp_site = site_named(store, "shell_multi_param", "g");
    cg::test::check(
        r,
        mp_site.has_value() && !mp_site->resolved
            && mp_site->receiver_types.find("IAbst2") != std::string::npos,
        "multi-impl shell param stays unresolved with the unwrapped key");
    // 负例：非类型形参前置的壳——own 形参下标域拒绝，剥壳必须跳过
    // （静态面不得混入实参键）。
    const auto mixed_site = site_named(store, "shell_mixed", "g");
    cg::test::check(
        r,
        mixed_site.has_value()
            && mixed_site->receiver_types.find("IAbst2") == std::string::npos,
        "non-type-param-first shell is never unwrapped");
}

/**
 * @brief 接收者声明面回退：有名接收者但站点静态类型
 *        空——同定位域的声明行在库时用其声明面类型补静态根，叶子声明
 *        经家族唯一覆盖唯一决议（reason=receiver_declared_unique）。
 * @internal 站点静态面缺口用库行清空模拟（界外/宏产物形态的采集缺
 *           口在夹具内无法自然复现，type_hints 声明行即库内事实）。
 *           负例：多实现接口声明（局部与形参两域）补根后仍降档收窄
 *           不猜（不 resolved）。
 */
void receiver_declared_root(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "recvshell");
    cg::test::check(r, sum.failed == 0, "recvshell indexes cleanly");
    const std::optional<cg::db::CallSiteRow> leaf_site =
        site_named(store, "declared_leaf_host", "f");
    const std::optional<cg::db::CallSiteRow> multi_site =
        site_named(store, "declared_multi_host", "g");
    const std::optional<cg::db::CallSiteRow> param_site =
        site_named(store, "declared_param_host", "g");
    cg::test::check(
        r,
        leaf_site.has_value() && multi_site.has_value()
            && param_site.has_value(),
        "declared-root hosts produce their call sites");
    if (!leaf_site.has_value() || !multi_site.has_value()
        || !param_site.has_value())
    {
        return;
    }
    // 模拟采集面静态缺口：清空三个宿主站点的 receiver_static（装载判证
    // 列），声明行（type_hints 的 Declared 行）保持原样。
    store.exec(
        std::format(
            "UPDATE call_sites SET receiver_static='[]' WHERE site_id IN "
            "({},{},{})",
            leaf_site->id,
            multi_site->id,
            param_site->id));
    const cg::analysis::StubTypeOracle   oracle;
    const cg::analysis::PropagationState state =
        cg::analysis::run(store, oracle);
    const cg::analysis::DispatchResult dispatch =
        cg::analysis::resolve(store, state);
    const auto by_id =
        [&](std::int64_t id) -> const cg::analysis::ResolvedCallSite*
    {
        for (const cg::analysis::ResolvedCallSite& s : dispatch.sites)
        {
            if (s.site_id == id)
            {
                return &s;
            }
        }
        return nullptr;
    };
    // 正例：叶子声明（Declared 行静态面=[Leaf]）补根 → 家族唯一覆盖
    // 唯一决议，reason 区分声明面来源。
    const cg::analysis::ResolvedCallSite* leaf = by_id(leaf_site->id);
    cg::test::check(
        r,
        leaf != nullptr && leaf->resolved && leaf->candidates.size() == 1
            && leaf->candidates.front().reason == "receiver_declared_unique",
        "declared leaf root resolves uniquely via receiver_declared_"
        "unique");
    // 负例：多实现接口声明补根后仍降档不猜（不 resolved，不发推断边）。
    const cg::analysis::ResolvedCallSite* multi = by_id(multi_site->id);
    cg::test::check(
        r,
        multi != nullptr && !multi->resolved,
        "multi-impl declared root stays unresolved (no "
        "guessing)");
    // 负例（形参域）：形参声明行同样带静态面（Declared 行 type_set 非
    // 空）且定位域落在调用方函数域——补根照常发生，但接口多实现降档
    // 收窄不猜（不 resolved，fallback 仍是动态证据缺失档）。
    const cg::analysis::ResolvedCallSite* param = by_id(param_site->id);
    cg::test::check(
        r,
        param != nullptr && !param->resolved
            && param->fallback_reason.find("no_dynamic_type_evidence")
                   != std::string::npos,
        "parameter declaration carries no static face to "
        "backfill");
}

const cg::test::Registrar reg_recvshell_pointee{
    "analysis.recvshell.pointee",
    &recvshell_pointee_resolution};
const cg::test::Registrar reg_receiver_declared_root{
    "analysis.recvshell.declared_root",
    &receiver_declared_root};

/**
 * @brief PropagatedHint 的逐字段相等谓词。
 *
 * @internal warm 预装载与快照往返两个等价测试共用；结构新增字段时
 *           须同步此清单，漏改只会静默弱化等价校验。
 */
[[nodiscard]]
auto propagated_hint_equal(
    const cg::analysis::PropagatedHint& x,
    const cg::analysis::PropagatedHint& y) -> bool
{
    return x.owner_key == y.owner_key && x.var_name == y.var_name
           && x.types == y.types && x.static_types == y.static_types
           && x.confidence == y.confidence && x.source == y.source
           && x.reason == y.reason && x.truncated_here == y.truncated_here
           && x.alias_src == y.alias_src && x.ret_src == y.ret_src
           && x.from_warm == y.from_warm;
}

/**
 * @brief 跨提交驻留的 warm 预装载路径与全表读路径等价。
 *
 * @internal 同一库同一脏集跑两次增量 run：第一次全表装载
 *           （warm_preloaded 为空，IncrementalPlan.warm 填充），把装载
 *           到的 warm 行集当作驻留传入第二次（模拟提交段驻留命中——
 *           驻留与库行恒等的前提由其同步协议背书，此处固化该前提验
 *           证传播侧双路径等价）。两次的传播终态必须逐项相等。
 */
void warm_preloaded_equivalence(R& r)
{
    db::GraphStore                store{":memory:"};
    const cg::graph::IndexSummary sum =
        index_fixture(store, "dispatch-evidence");
    cg::test::check(r, sum.failed == 0, "dispatch-evidence indexes cleanly");
    // 脏集 = 库里任一文件（行脏种子触发闭包重算与 warm 回填混合）。
    const std::vector<cg::db::FileRow>  files = cg::db::all_files(store);
    const std::optional<cg::db::FileId> dirty_fid =
        files.empty() ? std::optional<cg::db::FileId>{}
                      : std::optional<cg::db::FileId>{files.front().id};
    cg::test::check(r, dirty_fid.has_value(), "fixture has a tracked file");
    if (!dirty_fid.has_value())
    {
        return;
    }
    std::unordered_set<cg::db::FileId> dirty{*dirty_fid};
    const cg::analysis::StubTypeOracle oracle;
    // 第一次：全表读路径。
    cg::analysis::IncrementalPlan plan_a;
    plan_a.active = true;
    plan_a.dirty_files = &dirty;
    const cg::analysis::PropagationState state_a =
        cg::analysis::run(store, oracle, nullptr, &plan_a);
    cg::test::check(
        r,
        plan_a.warm_loaded == &plan_a.warm,
        "table-scan run exposes its own warm map");
    cg::test::check(
        r,
        state_a.incremental && !plan_a.degraded,
        "table-scan incremental run converges");
    // 第二次：同一输入，warm 以预装载（驻留形态）提供。
    cg::analysis::IncrementalPlan plan_b;
    plan_b.active = true;
    plan_b.dirty_files = &dirty;
    plan_b.warm_preloaded = plan_a.warm_loaded;
    const cg::analysis::PropagationState state_b =
        cg::analysis::run(store, oracle, nullptr, &plan_b);
    cg::test::check(
        r,
        plan_b.warm_loaded == plan_a.warm_loaded,
        "preloaded run adopts the residency view");
    cg::test::check(
        r,
        state_b.incremental && !plan_b.degraded,
        "preloaded incremental run converges");
    // 终态逐项相等（hints 全字段、观测计数、增量标记）。
    bool hints_equal = state_a.hints.size() == state_b.hints.size()
                       && state_a.sites.size() == state_b.sites.size()
                       && state_a.rounds == state_b.rounds
                       && state_a.work_items == state_b.work_items;
    if (hints_equal)
    {
        for (std::size_t i = 0; i < state_a.hints.size(); ++i)
        {
            const cg::analysis::PropagatedHint& x = state_a.hints[i];
            const cg::analysis::PropagatedHint& y = state_b.hints[i];
            if (!propagated_hint_equal(x, y))
            {
                hints_equal = false;
                break;
            }
        }
    }
    cg::test::check(
        r,
        hints_equal,
        "preloaded warm path matches the table-scan path");
}

const cg::test::Registrar reg_warm_preloaded_equivalence{
    "analysis.warm_preloaded_equivalence",
    &warm_preloaded_equivalence};

/**
 * @brief 槽侧快照的往返等价：全表装载路径装配快照落盘，恢复路径（无
 *        脏批与指纹脏批两形态）与全表路径的传播终态逐项相等。
 *
 * @internal 三段：基准全量增量（装配 dump）→ 无脏恢复（零脏集，全快
 *           照，槽序与装载序一致，含轮数全等）→ 指纹脏批恢复（改一条
 *           指纹触发该文件槽整槽重建；行内容未变故终态仍逐项相等，
 *           但槽序=保留序+重建序，轮数不断言）。负例（seq 失配/坏头
 *           弃快照走全表）在快照格式往返 case 内覆盖。
 */
void snapshot_roundtrip_equivalence(R& r)
{
    db::GraphStore                store{":memory:"};
    const cg::graph::IndexSummary sum =
        index_fixture(store, "dispatch-evidence");
    cg::test::check(r, sum.failed == 0, "dispatch-evidence indexes cleanly");
    const std::vector<cg::db::FileRow> files = cg::db::all_files(store);
    cg::test::check(r, !files.empty(), "fixture has tracked files");
    if (files.empty())
    {
        return;
    }
    const cg::analysis::StubTypeOracle oracle;
    std::string                        first_diff_;
    // 逐下标比对（同槽序路径用：无脏恢复与失配回退的槽序都与装载序一
    // 致）；键序比对（脏批恢复路径用：剔除重建槽挪到槽序尾部，集合语
    // 义等价）。
    const auto hints_equal_in_order =
        [&](const cg::analysis::PropagationState& a,
            const cg::analysis::PropagationState& b)
    {
        if (a.hints.size() != b.hints.size()
            || a.sites.size() != b.sites.size())
        {
            first_diff_.assign(
                std::format(
                    "counts hints {} vs {} sites {} vs {}",
                    a.hints.size(),
                    b.hints.size(),
                    a.sites.size(),
                    b.sites.size()));
            return false;
        }
        for (std::size_t i = 0; i < a.hints.size(); ++i)
        {
            if (!propagated_hint_equal(a.hints[i], b.hints[i]))
            {
                first_diff_.assign(
                    std::format(
                        "slot#{} {}::{} types{} [{}] vs [{}] static{}conf{}src{}"
                        "reason{}trunc{}alias{}ret{}warm{}",
                        i,
                        a.hints[i].owner_key,
                        a.hints[i].var_name,
                        a.hints[i].types == b.hints[i].types ? "=" : "!",
                        a.hints[i].types.size(),
                        b.hints[i].types.size(),
                        a.hints[i].static_types == b.hints[i].static_types
                            ? "="
                            : "!",
                        a.hints[i].confidence == b.hints[i].confidence ? "="
                                                                       : "!",
                        a.hints[i].source == b.hints[i].source ? "=" : "!",
                        a.hints[i].reason == b.hints[i].reason ? "=" : "!",
                        a.hints[i].truncated_here == b.hints[i].truncated_here
                            ? "="
                            : "!",
                        a.hints[i].alias_src == b.hints[i].alias_src ? "="
                                                                     : "!",
                        a.hints[i].ret_src == b.hints[i].ret_src ? "=" : "!",
                        a.hints[i].from_warm == b.hints[i].from_warm ? "="
                                                                     : "!"));
                return false;
            }
        }
        return true;
    };
    const auto hints_equal_by_key = [&](const cg::analysis::PropagationState& a,
                                        const cg::analysis::PropagationState& b)
    {
        if (a.hints.size() != b.hints.size()
            || a.sites.size() != b.sites.size())
        {
            return false;
        }
        const auto by_key = [](const cg::analysis::PropagationState& s)
        {
            std::vector<const cg::analysis::PropagatedHint*> v;
            v.reserve(s.hints.size());
            for (const cg::analysis::PropagatedHint& h : s.hints)
            {
                v.push_back(&h);
            }
            std::ranges::sort(
                v,
                [](const cg::analysis::PropagatedHint* x,
                   const cg::analysis::PropagatedHint* y)
                {
                    return std::tie(x->owner_key, x->var_name)
                           < std::tie(y->owner_key, y->var_name);
                });
            return v;
        };
        const auto va = by_key(a);
        const auto vb = by_key(b);
        for (std::size_t i = 0; i < va.size(); ++i)
        {
            if (!propagated_hint_equal(*va[i], *vb[i]))
            {
                return false;
            }
        }
        return true;
    };

    // 基准：全表装载 + 快照装配（模拟 dispatch_commit 的写侧语义，
    // seq=1、指纹基线=提交后的全表终态）。
    std::unordered_set<cg::db::FileId> no_dirty;
    cg::analysis::IncrementalPlan      plan_a;
    plan_a.active = true;
    plan_a.dirty_files = &no_dirty;
    cg::analysis::snap::SnapshotData dump;
    plan_a.snap_dump = &dump;
    const cg::analysis::PropagationState state_a =
        cg::analysis::run(store, oracle, nullptr, &plan_a);
    cg::test::check(
        r,
        state_a.incremental && !plan_a.degraded,
        "baseline incremental run converges");
    dump.snapshot_seq = 1;
    dump.fingerprints = cg::db::all_fingerprints(store);
    const std::string bytes = cg::analysis::snap::serialize(dump);
    cg::test::check(r, !bytes.empty(), "snapshot serializes");
    if (bytes.empty())
    {
        return;
    }
    const std::filesystem::path snap_path =
        cg::util::platform::unique_temp_path(
            std::filesystem::temp_directory_path() / "ccg-snap-roundtrip.snap");
    cg::test::check(
        r,
        cg::util::platform::atomic_write_file(snap_path, bytes),
        "snapshot file written atomically");
    cg::analysis::snap::RestoreRequest req;
    req.path = snap_path;
    req.snapshot_seq = 1;

    // 路径一：无脏恢复（零脏集全快照）。
    cg::analysis::IncrementalPlan plan_b;
    plan_b.active = true;
    plan_b.dirty_files = &no_dirty;
    plan_b.snap_request = &req;
    const cg::analysis::PropagationState state_b =
        cg::analysis::run(store, oracle, nullptr, &plan_b);
    cg::test::check(
        r,
        plan_b.snap_hit,
        "clean restore hits (miss=" + plan_b.snap_miss_reason + ")");
    cg::test::check(
        r,
        plan_b.warm_from_snap,
        "restored warm carries segment form");
    cg::test::check(
        r,
        state_b.incremental && !plan_b.degraded,
        "clean restored run converges");
    bool equal_b = hints_equal_in_order(state_a, state_b);
    cg::test::check(
        r,
        equal_b,
        "clean restored run matches the table-scan path"
            + (equal_b ? std::string{} : " | " + first_diff_));
    cg::test::check(
        r,
        state_a.rounds == state_b.rounds,
        std::format(
            "rounds: table={} restored={}",
            state_a.rounds,
            state_b.rounds));
    cg::test::check(
        r,
        state_a.work_items == state_b.work_items,
        std::format(
            "work: table={} restored={}",
            state_a.work_items,
            state_b.work_items));

    // 路径二：指纹脏批（改一条指纹 mtime → 该文件槽整槽重建；行内容
    // 未变，终态仍与基准逐项相等）。
    const cg::db::FileId touched = files.front().id;
    bool                 fp_touched = false;
    for (const cg::db::FingerprintRow& fp : cg::db::all_fingerprints(store))
    {
        if (fp.file_id == touched)
        {
            cg::db::FingerprintRow mutated = fp;
            mutated.mtime_ns += 1;
            store.set_fingerprint(mutated);
            fp_touched = true;
            break;
        }
    }
    cg::test::check(r, fp_touched, "touched one fingerprint row");
    cg::analysis::IncrementalPlan plan_c;
    plan_c.active = true;
    plan_c.dirty_files = &no_dirty;
    plan_c.snap_request = &req;
    const cg::analysis::PropagationState state_c =
        cg::analysis::run(store, oracle, nullptr, &plan_c);
    cg::test::check(r, plan_c.snap_hit, "delta restore hits");
    cg::test::check(
        r,
        plan_c.snap_dirty_files == 1,
        "fingerprint delta yields exactly one dirty file");
    cg::test::check(
        r,
        plan_c.snap_rebuilt_slots > 0,
        "dirty file slots are rebuilt from SQL");
    cg::test::check(
        r,
        state_c.incremental && !plan_c.degraded,
        "delta restored run converges");
    cg::test::check(
        r,
        hints_equal_by_key(state_a, state_c),
        "delta restored run matches the table-scan path");

    // 负例：seq 失配 → 弃快照走全表，行为零分叉（终态仍与基准相等）。
    cg::analysis::snap::RestoreRequest stale_req;
    stale_req.path = snap_path;
    stale_req.snapshot_seq = 2; // 库侧 seq 已前进
    cg::analysis::IncrementalPlan plan_d;
    plan_d.active = true;
    plan_d.dirty_files = &no_dirty;
    plan_d.snap_request = &stale_req;
    const cg::analysis::PropagationState state_d =
        cg::analysis::run(store, oracle, nullptr, &plan_d);
    cg::test::check(
        r,
        !plan_d.snap_hit && plan_d.snap_miss_reason == "seq",
        "stale seq rejects the snapshot");
    cg::test::check(
        r,
        hints_equal_in_order(state_a, state_d),
        "rejected snapshot falls back to the table-scan path");
    std::error_code ec;
    std::filesystem::remove(snap_path, ec);
}

const cg::test::Registrar reg_snapshot_roundtrip{
    "analysis.snapshot_roundtrip_equivalence",
    &snapshot_roundtrip_equivalence};

/**
 * @brief 快照格式层的往返与失效：序列化字段逐项还原、指纹对称差正确、
 *        坏头/坏段弃快照。
 */
void snapshot_format_roundtrip(R& r)
{
    cg::analysis::snap::SnapshotData data;
    data.snapshot_seq = 7;
    data.slots_ver =
        static_cast<std::uint32_t>(cg::analysis::analysis_slots_ver);
    data.schema_version = static_cast<std::uint32_t>(cg::db::schema_version);
    data.type_of_id = {"", "A", "B", "method|X"};
    data.arena_init = {0, 1, 2, 1, 3};
    data.arena_warm = {2, 3};
    data.var_domain = {{"#ret", 0}, {"x", 1}, {"p", 2}};
    data.next_var_id = 3;
    cg::analysis::snap::SlotRec slot;
    slot.owner_key = "function|f";
    slot.var_name = "x";
    slot.init_off = 1;
    slot.init_len = 2;
    slot.confidence = 1;
    slot.source = 8;
    slot.reason = "param_flow";
    slot.related_var = "@fld|C|f";
    slot.related_key = "";
    slot.param_index = -1;
    slot.has_outparam_flow = true;
    slot.derived_seed = true;
    slot.warm_off = 0;
    slot.warm_len = 2;
    slot.warm_confidence = 2;
    slot.warm_source = 9;
    slot.warm_reason = "param_flow_multiple";
    slot.alias_src = "function|g\x1fp";
    slot.ret_src = "";
    slot.truncated = false;
    slot.static_types = {"A"};
    slot.files = {3, 5};
    data.slots = {slot};
    data.row_alias = {{0, "@fld|C|f"}};
    data.fingerprints = {
        {.file_id = 3, .mtime_ns = 11, .size = 5, .cmd_digest = "d1"},
        {.file_id = 5, .mtime_ns = 22, .size = 6, .cmd_digest = ""}};
    const std::string bytes = cg::analysis::snap::serialize(data);
    cg::test::check(r, !bytes.empty(), "format case serializes");
    if (bytes.empty())
    {
        return;
    }
    const std::filesystem::path path = cg::util::platform::unique_temp_path(
        std::filesystem::temp_directory_path() / "ccg-snap-format.snap");
    cg::test::check(
        r,
        cg::util::platform::atomic_write_file(path, bytes),
        "format case file written");

    std::string miss;
    auto        restored = cg::analysis::snap::restore(
        {.path = path, .snapshot_seq = data.snapshot_seq},
        data.slots_ver,
        data.schema_version,
        data.fingerprints,
        miss);
    cg::test::check(
        r,
        restored.has_value() && miss.empty(),
        "format case restores cleanly");
    if (restored.has_value())
    {
        const cg::analysis::snap::SnapshotData& d = restored->data;
        bool ok = d.snapshot_seq == 7 && d.next_var_id == 3
                  && d.var_domain.size() == 3 && d.type_of_id == data.type_of_id
                  && d.arena_init == data.arena_init
                  && d.arena_warm == data.arena_warm && d.slots.size() == 1
                  && d.row_alias == data.row_alias;
        const cg::analysis::snap::SlotRec& s = d.slots.front();
        ok = ok && s.owner_key == slot.owner_key && s.var_name == "x"
             && s.init_off == 1 && s.init_len == 2 && s.confidence == 1
             && s.reason == "param_flow" && s.related_var == "@fld|C|f"
             && s.has_outparam_flow && s.derived_seed && s.warm_off == 0
             && s.warm_len == 2 && s.warm_confidence == 2
             && s.warm_reason == "param_flow_multiple"
             && s.alias_src == slot.alias_src
             && s.static_types == slot.static_types && s.files == slot.files;
        ok = ok && restored->dirty_files.empty();
        cg::test::check(r, ok, "format roundtrip preserves every field");
        // 指纹对称差：改动一条/新增一条/删除一条。
        std::vector<cg::db::FingerprintRow> mutated = data.fingerprints;
        mutated[0].mtime_ns += 1;
        auto r2 = cg::analysis::snap::restore(
            {.path = path, .snapshot_seq = data.snapshot_seq},
            data.slots_ver,
            data.schema_version,
            mutated,
            miss);
        cg::test::check(
            r,
            r2.has_value() && r2->dirty_files.size() == 1
                && r2->dirty_files.contains(3),
            "changed fingerprint marks its file dirty");
        mutated.push_back(
            {.file_id = 9, .mtime_ns = 1, .size = 1, .cmd_digest = "d9"});
        auto r3 = cg::analysis::snap::restore(
            {.path = path, .snapshot_seq = data.snapshot_seq},
            data.slots_ver,
            data.schema_version,
            mutated,
            miss);
        cg::test::check(
            r,
            r3.has_value() && r3->dirty_files.size() == 2
                && r3->dirty_files.contains(3) && r3->dirty_files.contains(9),
            "changed+added fingerprints mark both files dirty");
        std::vector<cg::db::FingerprintRow> shrunk{data.fingerprints.back()};
        auto                                r4 = cg::analysis::snap::restore(
            {.path = path, .snapshot_seq = data.snapshot_seq},
            data.slots_ver,
            data.schema_version,
            shrunk,
            miss);
        cg::test::check(
            r,
            r4.has_value() && r4->dirty_files.size() == 1
                && r4->dirty_files.contains(3),
            "removed fingerprint marks its file dirty");
    }
    // 坏头（翻转 magic）→ 弃。
    {
        std::string corrupt = bytes;
        corrupt[0] = 'X';
        const auto bad = cg::util::platform::unique_temp_path(
            std::filesystem::temp_directory_path() / "ccg-snap-corrupt.snap");
        (void)cg::util::platform::atomic_write_file(bad, corrupt);
        auto r5 = cg::analysis::snap::restore(
            {.path = bad, .snapshot_seq = data.snapshot_seq},
            data.slots_ver,
            data.schema_version,
            data.fingerprints,
            miss);
        cg::test::check(
            r,
            !r5.has_value() && miss == "magic",
            "corrupt magic rejects the snapshot");
        std::error_code ec;
        std::filesystem::remove(bad, ec);
    }
    // 坏段目录：无符号回绕绕过"和上界"检查（offset/bytes 均 64 位无符
    // 号）。目录区不受 header_crc/data_crc 任何一方覆盖，这道边界检查
    // 是唯一防线，两种回绕形态都必须在此被拒（seg-dir），而不是放行后
    // 拿越界 span 去算 CRC（野指针读，硬件故障）。布局常量与 restore 一
    // 致：头部 96 字节，段目录条目 32 字节（kind/reserved 各 4 字节后是
    // offset、bytes），目录共 10 条。
    for (int form = 0; form < 2; ++form)
    {
        std::string       corrupt = bytes;
        const std::size_t offset_field = 96 + 8;
        const std::size_t bytes_field = offset_field + 8;
        // 形态零：offset 接近 2^64（16 对齐、不低于数据区起点），和回绕
        // 为小值；形态一：offset 取合法的数据区起点 416（=96+10*32），
        // bytes 巨大使和回绕为 32（不低于 tail_offset 的检查被骗过）。
        std::uint64_t bad_offset = 0xFFFFFFFFFFFFFF00ULL;
        std::uint64_t bad_bytes = 0x100ULL;
        if (form == 1)
        {
            bad_offset = 416;
            bad_bytes = 0ULL - 416ULL + 32ULL;
        }
        std::memcpy(
            corrupt.data() + offset_field,
            &bad_offset,
            sizeof(bad_offset));
        std::memcpy(
            corrupt.data() + bytes_field,
            &bad_bytes,
            sizeof(bad_bytes));
        const auto bad = cg::util::platform::unique_temp_path(
            std::filesystem::temp_directory_path() / "ccg-snap-wrap.snap");
        (void)cg::util::platform::atomic_write_file(bad, corrupt);
        auto wrapped = cg::analysis::snap::restore(
            {.path = bad, .snapshot_seq = data.snapshot_seq},
            data.slots_ver,
            data.schema_version,
            data.fingerprints,
            miss);
        cg::test::check(
            r,
            !wrapped.has_value() && miss == "seg-dir",
            "a wraparound segment entry is rejected");
        std::error_code ec2;
        std::filesystem::remove(bad, ec2);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

const cg::test::Registrar reg_snapshot_format{
    "analysis.snapshot_format_roundtrip",
    &snapshot_format_roundtrip};

/**
 * @brief decode_method_ref 对畸形 related_key 的宽容契约。
 *
 * @internal sig_hash 段非数字（外部改写/跨版本残留/位腐）不得把异常抛
 *           出解码层：该轮分析必须整体存活（对齐 graph.build 对 meta 值
 *           atoll 的宽容先例——解析失败退化为无 sig_hash 形态）。
 */
void decode_method_ref_tolerates_malformed_sig(R& r)
{
    bool                    threw = false;
    cg::analysis::MethodRef m;
    try
    {
        m = cg::analysis::decode_method_ref("@fn|f|ns|parent|not-a-number|1");
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    cg::test::check(
        r,
        !threw,
        "a malformed sig_hash never throws out of decode");
    cg::test::check(
        r,
        !m.sig_hash.has_value(),
        "a malformed sig_hash degrades to the no-hash form");
    const cg::analysis::MethodRef ok =
        cg::analysis::decode_method_ref("@fn|f|ns|parent|-42|0");
    cg::test::check(
        r,
        ok.sig_hash.has_value() && *ok.sig_hash == -42,
        "a numeric sig_hash still round-trips");
}

const cg::test::Registrar reg_decode_ref{
    "analysis.decode_method_ref_tolerant",
    &decode_method_ref_tolerates_malformed_sig};

/**
 * @brief 批量位运算三档（标量基线/AVX2/AVX-512）的同语义等价。
 *
 * @internal 三档或原语必须逐位同语义：本机可执行的每一档对同一
 *           (dst, src, count) 的输出与标量基线逐字比对（word 数组的
 *           逐元素相等即逐位相等）；长度矩阵覆盖 0、单 word、跨 word
 *           与两种向量块边界（4 word 与 8 word）的两侧；模式覆盖全 0、
 *           全 1、伪随机与稀疏位。档位门：探测未确认的档不执行（旧
 *           机器上执行向量指令会非法指令异常），因此只支持标量的机器
 *           上向量档检查合法为空、分派路径仍与标量 oracle 全量比对。
 *           另验证分派档位与探测能力快照的映射一致（探测一次定型的
 *           契约）与 SymBits::or_with 分派路径的端到端等价。缓冲统一
 *           走 propagate_detail 的对齐存储（AlignedWords，与 SymBits
 *           生产形态同型），满足向量档对齐版指令的基址 64 字节对齐
 *           契约（见 symbits.kernels.h 的 OrWordsFn 说明）。
 */
void symbits_tier_bit_equivalence(R& r)
{
    using cg::analysis::propagate_detail::BitsTier;
    namespace pd = cg::analysis::propagate_detail;

    // 档位一致性：分派档位 = 能力快照映射（CPUID/XCR0 探测 -> 档位）。
    const BitsTier                          tier = pd::bits_active_tier();
    const cg::util::platform::CpuBitOpsCaps caps =
        cg::util::platform::cpu_bitops_caps();
    const BitsTier expected = caps.avx512 ? BitsTier::Avx512
                              : caps.avx2 ? BitsTier::Avx2
                                          : BitsTier::Scalar;
    cg::test::check(
        r,
        tier == expected,
        "dispatch tier matches the probed capability caps");
    const std::string_view tier_label = pd::bits_tier_name(tier);
    cg::test::check(
        r,
        tier_label == "scalar" || tier_label == "avx2"
            || tier_label == "avx512",
        "tier name is one of the three known tiers");

    // 逐位等价矩阵：模式 kind 0=全 0、1=全 1、2=伪随机稠密、3=稀疏位。
    std::mt19937_64   rng{0x5EED5EED5EED5EEDULL};
    const std::size_t lengths[] = {0,  1,  2,  3,  4,  5,  7,   8,   9,   15,
                                   16, 31, 32, 63, 64, 65, 127, 128, 129, 137};
    const auto        make_words = [&](std::size_t n, unsigned kind)
    {
        pd::AlignedWords v(n, 0);
        for (std::size_t i = 0; i < n; ++i)
        {
            if (kind == 1)
            {
                v[i] = ~0ULL;
            }
            else if (kind == 2)
            {
                v[i] = rng();
            }
            else if (kind == 3)
            {
                v[i] = (1ULL << (rng() % 64)) | (1ULL << (rng() % 64));
            }
        }
        return v;
    };
    bool all_equal = true;
    bool avx2_ran = false;
    bool avx512_ran = false;
    for (const std::size_t n : lengths)
    {
        for (unsigned dst_kind = 0; dst_kind < 4; ++dst_kind)
        {
            for (unsigned src_kind = 0; src_kind < 4; ++src_kind)
            {
                const pd::AlignedWords dst = make_words(n, dst_kind);
                const pd::AlignedWords src = make_words(n, src_kind);
                pd::AlignedWords       out_scalar = dst;
                pd::or_words_scalar(out_scalar.data(), src.data(), n);
                if (tier >= BitsTier::Avx2)
                {
                    pd::AlignedWords out_avx2 = dst;
                    pd::or_words_avx2(out_avx2.data(), src.data(), n);
                    avx2_ran = true;
                    if (out_avx2 != out_scalar)
                    {
                        all_equal = false;
                    }
                }
                if (tier == BitsTier::Avx512)
                {
                    pd::AlignedWords out_avx512 = dst;
                    pd::or_words_avx512(out_avx512.data(), src.data(), n);
                    avx512_ran = true;
                    if (out_avx512 != out_scalar)
                    {
                        all_equal = false;
                    }
                }
                // 分派路径（SymBits::or_with）与标量 oracle 等价。
                pd::SymBits a;
                a.words = dst;
                pd::SymBits b;
                b.words = src;
                a.or_with(b);
                if (a.words != out_scalar)
                {
                    all_equal = false;
                }
            }
        }
    }
    cg::test::check(
        r,
        all_equal,
        "every executable tier matches the scalar oracle "
        "bit-for-bit");
    // 可执行事实：向量档恰在探测确认时执行（标量机器上合法为 false）。
    cg::test::check(
        r,
        avx2_ran == (tier >= BitsTier::Avx2),
        "avx2 tier ran exactly when supported");
    cg::test::check(
        r,
        avx512_ran == (tier == BitsTier::Avx512),
        "avx512 tier ran exactly when supported");
}

/**
 * @brief lambda 体内同名局部遮蔽的跨域记名闸。
 *
 * @internal 首版语义把 lambda 体语句按外层函数语境采集；同名遮蔽时
 *           编译器把引用解析到 lambda 内声明，裸名记名会把内层证据
 *           错连进外层槽。负例（ctor 初始化与 handler 流两夹具各一）：
 *           外层形参槽
 *           （shadow_host / handler_host 的 ctx）不得出现遮蔽局部的
 *           类型证据（Declared / DirectNew 行），遮蔽调用点的实参名
 *           记录为空；正例：捕获引用照常落外层域（实参名记录不回
 *           退），lambda 体内调用点仍按外层调用方归属采集（首版语
 *           义保持）。
 */
void lambda_shadow_cross_domain_gate(R& r)
{
    struct GateCase
    {
        std::string_view fixture;
        std::string_view host;
        std::string_view sink;
        std::string_view shadow_type;
    };
    for (const GateCase& cs :
         {GateCase{"ctorinit", "shadow_host", "take_dev", "LocalDev"},
          GateCase{"handlerflow", "handler_host", "use_svc", "OtherSvc"}})
    {
        db::GraphStore store{":memory:"};
        const auto     sum = index_fixture(store, cs.fixture);
        cg::test::check(
            r,
            sum.failed == 0,
            "lambda shadow fixture indexes cleanly");
        // 遮蔽负例：外层 ctx 槽不得出现遮蔽局部的任何类型证据。
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            const auto owner = cg::db::get_symbol(store, row.owner_id);
            if (owner && owner->qualified_name == cs.host
                && row.var_name == "ctx")
            {
                cg::test::check(
                    r,
                    row.type_set.find(cs.shadow_type) == std::string::npos,
                    "outer ctx slot never receives shadow-local evidence");
            }
        }
        // 调用点：sink 的两个调用点都在（首版语义归属外层调用方）；
        // 捕获点实参名记 ctx（正例不回退），遮蔽点实参名为空（断链）。
        std::size_t sites = 0;
        std::size_t named = 0;
        for (const cg::db::CallSiteRow& row : store.all_call_sites())
        {
            const auto caller = cg::db::get_symbol(store, row.caller_id);
            if (!caller || caller->qualified_name != cs.host
                || row.callee_name != cs.sink)
            {
                continue;
            }
            ++sites;
            if (row.arg_vars.find("ctx") != std::string::npos)
            {
                ++named;
            }
        }
        cg::test::check(
            r,
            sites == 2,
            "both sink call sites stay collected under the "
            "outer caller");
        cg::test::check(
            r,
            named == 1,
            "capture call keeps the outer argument name");
        cg::test::check(
            r,
            sites - named == 1,
            "shadowed call records no argument name");
    }
}

const cg::test::Registrar reg_lambda_shadow_gate{
    "analysis.lambda.shadow_domain_gate",
    &lambda_shadow_cross_domain_gate};

// ------------------------------------------------ ctor-init 字段采集臂 ----

/**
 * @brief ctor 初始化列表普通字段（非壳）采集臂。
 *
 * @internal 四正例：裸指针/引用成员/const+花括号/遮蔽同名（related_var
 *           必须落在形参域 @fnv，绝不落字段槽 @fld）；ctor+函数体双写
 *           走行级多源边并集（双候选 0.4 低置信，不发唯一边）。四负例：
 *           基类/委托（TypeRef 形态天然排除）、数组/字面量 RHS（三通道
 *           全空不发）、NSDMI（不经 ctor 配对路径）、多实参值字段（整
 *           体 RHS 类型证据，不按参拆出别名）。
 */
void ctorinit_plain_field_arm(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "ctorinit");
    cg::test::check(r, sum.failed == 0, "ctor-init fixture indexes cleanly");
    const auto owner_of = [&](const cg::db::TypeHintRow& row)
    {
        const auto s = cg::db::get_symbol(store, row.owner_id);
        return s ? s->qualified_name : std::string{};
    };
    // 正例：alias 行逐 (记录, 字段) 断言——related_var 带 @fnv（构造函
    // 数域）且类型通道为空（纯传播边）。
    const struct AliasCase
    {
        std::string_view owner;
        std::string_view var;
        std::string_view label;
    } alias_cases[] = {
        {"Holder", "dev_", "plain pointer field (fixture base)"},
        {"PlainHolder", "f_", "plain pointer field"},
        {"RefHolder", "r_", "reference member"},
        {"ConstBraceHolder", "c_", "const member with braces"},
        {"ShadowHolder", "image_", "shadowed name resolves to ParmDecl"},
        {"DualWriteHolder", "f_", "dual-write ctor source"},
        {"DualWriteHolder", "g_", "dual-write body source"},
        {"CtorDerived", "f_", "field after base init still pairs"},
    };
    for (const AliasCase& cs : alias_cases)
    {
        bool hit = false;
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            if (row.source
                    != static_cast<int>(cg::analysis::HintSource::FieldFlow)
                || row.fallback_reason != "ctor_init_flow"
                || owner_of(row) != cs.owner || row.var_name != cs.var)
            {
                continue;
            }
            hit = true;
            cg::test::check(
                r,
                row.related_var.starts_with("@fnv|"),
                "plain ctor-init alias carries the ctor function domain");
            cg::test::check(
                r,
                !row.related_var.starts_with("@fld|"),
                "shadowed-name RHS never resolves to the field "
                "slot itself");
            cg::test::check(
                r,
                row.type_set.empty() || row.type_set == "[]",
                "pure alias row keeps the type channel empty");
        }
        cg::test::check(r, hit, cs.label);
    }
    // 遮蔽同名的域细节：related_var 的名字段恰是形参拼写。
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        if (row.fallback_reason == "ctor_init_flow"
            && owner_of(row) == "ShadowHolder" && row.var_name == "image_")
        {
            cg::test::check(
                r,
                row.related_var.size() >= 6
                    && row.related_var.substr(row.related_var.size() - 6)
                           == "image_",
                "shadowed alias names the parameter, not the field");
        }
    }
    // 负例：数组/字面量 RHS、NSDMI 不发任何 ctor_init_flow 行。
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        if (row.fallback_reason != "ctor_init_flow")
        {
            continue;
        }
        const std::string owner = owner_of(row);
        cg::test::check(
            r,
            !(owner == "OpaqueHolder"
              && (row.var_name == "arr_" || row.var_name == "n_")),
            "array/literal RHS emits nothing");
        cg::test::check(
            r,
            !(owner == "NsdmiHolder" && row.var_name == "nsd_"),
            "NSDMI never goes through the ctor pairing");
    }
    // 负例（多实参值字段）：整体 RHS 的类型证据行存在，但绝不按参拆出
    // 别名（related_var 恒空）。
    bool multi_type_row = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        if (row.fallback_reason == "ctor_init_flow"
            && owner_of(row) == "MultiArgHolder" && row.var_name == "v_")
        {
            multi_type_row = true;
            cg::test::check(
                r,
                row.related_var.empty(),
                "multi-arg value field is never split per "
                "argument");
            cg::test::check(
                r,
                row.type_set.find("PairVal") != std::string::npos,
                "whole-RHS type evidence survives");
        }
    }
    cg::test::check(
        r,
        multi_type_row,
        "multi-arg value field emits one whole-RHS row");
    // 派发边：单源字段站点 Medium 连通；双写字段并集双候选低置信。
    const std::vector<DEdge> edges = dispatch_edges(store);
    const auto               conf_edge = [&](std::string_view caller,
                                             std::string_view dst,
                                             double           want,
                                             std::string_view label)
    {
        const DEdge* e = find_edge(edges, caller, dst);
        cg::test::check(r, e != nullptr, label);
        cg::test::check(r, e != nullptr && e->confidence == want, label);
    };
    conf_edge(
        "PlainHolder::use",
        "RealDev::poke",
        0.7,
        "plain pointer ctor-init connects at medium");
    conf_edge(
        "RefHolder::use",
        "RealDev::poke",
        0.7,
        "reference member ctor-init connects at medium");
    conf_edge(
        "ConstBraceHolder::use",
        "RealDev::poke",
        0.7,
        "const/brace ctor-init connects at medium");
    conf_edge(
        "ShadowHolder::use",
        "RealDev::poke",
        0.7,
        "shadowed-name ctor-init connects at medium");
    conf_edge(
        "DualWriteHolder::use",
        "RealDev::poke",
        0.4,
        "dual-write keeps candidate one low");
    conf_edge(
        "DualWriteHolder::use",
        "LocalDev::poke",
        0.4,
        "dual-write keeps candidate two low");
    for (const DEdge& e : edges)
    {
        if (e.src_name == "DualWriteHolder::use")
        {
            cg::test::check(
                r,
                e.confidence <= 0.4,
                "no medium/high edge escapes the dual-write union pool");
        }
    }
}

const cg::test::Registrar reg_ctorinit_plain_arm{
    "analysis.ctorinit.plain_field_arm",
    &ctorinit_plain_field_arm};

// --------------------------------------------- handler-flow 解引用穿透 ----

/**
 * @brief 解引用实参穿透记名（`*p` 剥壳，槽锚点直达操作数）。
 *
 * @internal 两正例：lambda 捕获形参解引用作直接调用实参（HandleSvc 与
 *           第二 sink SinkRef2 的 IService& 形参同链穿透，裸名归外层
 *           函数域，HandleSvc::ctx 参数槽取双 fake 并集、派发边恰两
 *           fake 实现低置信、不含接口类型；一级 record 指针的解引用在
 *           四判据下恒绑定引用/值形参，类型上不可能绑定裸指针形参，
 *           故第二 sink 用引用形参）；四负例：二级指针/非 record
 *           pointee/函数指针解引用调用不产生对象槽证据、值切片实参
 *           不记名（callee 值形参闸死，派生类型不得混入值形参槽）。
 */
void handlerflow_deref_penetration(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "handlerflow");
    cg::test::check(r, sum.failed == 0, "handler-flow fixture indexes cleanly");
    // 调用点实参名：正例记操作数名，负例维持空名。
    bool handle_named = false;
    bool sinkptr_named = false;
    bool sinkval_empty = false;
    bool sinkint_empty = false;
    bool sinkintref_empty = false;
    bool fnptr_indirect = false;
    for (const cg::db::CallSiteRow& row : store.all_call_sites())
    {
        const auto caller = cg::db::get_symbol(store, row.caller_id);
        if (!caller || caller->qualified_name != "handler_flow_host")
        {
            continue;
        }
        if (row.callee_name == "HandleSvc")
        {
            handle_named = row.arg_vars.find("\"ctx\"") != std::string::npos;
        }
        else if (row.callee_name == "SinkRef2")
        {
            sinkptr_named = row.arg_vars.find("\"ctx\"") != std::string::npos;
        }
        else if (row.callee_name == "SinkVal")
        {
            sinkval_empty = row.arg_vars.find("dp") == std::string::npos;
        }
        else if (row.callee_name == "SinkIntRef")
        {
            sinkintref_empty = row.arg_vars.find("pp") == std::string::npos;
        }
        else if (row.callee_name == "SinkInt")
        {
            sinkint_empty = row.arg_vars.find("ip") == std::string::npos;
        }
        else if (row.indirect)
        {
            fnptr_indirect = true;
        }
    }
    cg::test::check(
        r,
        handle_named,
        "deref argument into a record-reference parameter "
        "records the operand name");
    cg::test::check(
        r,
        sinkptr_named,
        "second sink on the same chain records the operand "
        "name too");
    cg::test::check(
        r,
        sinkval_empty,
        "value-slice argument records no operand name");
    cg::test::check(
        r,
        sinkintref_empty,
        "double dereference records no operand name");
    cg::test::check(
        r,
        sinkint_empty,
        "non-record pointee records no operand name");
    cg::test::check(
        r,
        fnptr_indirect,
        "function-pointer dereference call stays indirect");
    // 参数槽终态：HandleSvc::ctx 取双 fake 并集；切片闸保 SinkVal::b 恒
    // 无派生类型。
    bool handle_slot = false;
    bool sinkref2_slot = false;
    bool sinkval_slot = false;
    for (const cg::db::AnalysisSlotRow& row : store.all_analysis_slots())
    {
        if (row.owner_key.find("|HandleSvc|") != std::string::npos
            && row.var_name == "ctx")
        {
            handle_slot = true;
            cg::test::check(
                r,
                row.type_set.find("FakeSvc") != std::string::npos
                    && row.type_set.find("OtherSvc") != std::string::npos,
                "handler ctx slot unions both fakes");
            cg::test::check(
                r,
                row.type_set.find("IService") == std::string::npos,
                "interface type never enters the slot");
        }
        if (row.owner_key.find("|SinkRef2|") != std::string::npos
            && row.var_name == "s")
        {
            sinkref2_slot = true;
            cg::test::check(
                r,
                row.type_set.find("FakeSvc") != std::string::npos
                    && row.type_set.find("OtherSvc") != std::string::npos,
                "second sink slot unions both fakes");
        }
        if (row.owner_key.find("|SinkVal|") != std::string::npos
            && row.var_name == "b")
        {
            sinkval_slot = true;
            cg::test::check(
                r,
                row.type_set.find("SinkDerived") == std::string::npos,
                "value-slice gate keeps the derived type out");
        }
    }
    cg::test::check(r, handle_slot, "handler ctx slot exists");
    cg::test::check(r, sinkref2_slot, "second sink slot exists");
    cg::test::check(r, sinkval_slot, "sink value slot exists");
    // 派发边：HandleSvc 体内虚调用恰达两 fake 实现，低置信；无其它
    // 非 0 边。
    const std::vector<DEdge> edges = dispatch_edges(store);
    const DEdge* fake = find_edge(edges, "HandleSvc", "FakeSvc::call");
    const DEdge* other = find_edge(edges, "HandleSvc", "OtherSvc::call");
    cg::test::check(
        r,
        fake != nullptr && other != nullptr,
        "deref penetration reaches both fake implementations");
    cg::test::check(
        r,
        fake != nullptr && fake->confidence == 0.4 && other != nullptr
            && other->confidence == 0.4,
        "multi-candidate param flow stays low (0.4)");
    for (const DEdge& e : edges)
    {
        if (e.src_name == "HandleSvc" && e.confidence > 0.0)
        {
            cg::test::check(
                r,
                e.dst_name == "FakeSvc::call" || e.dst_name == "OtherSvc::call",
                "no non-marker edge escapes the fake pair");
        }
    }
}

const cg::test::Registrar reg_handlerflow_deref{
    "analysis.handlerflow.deref_penetration",
    &handlerflow_deref_penetration};

// --------------------------------------------- attach-shell 工厂穿透 ----

/**
 * @brief 静态方法壳工厂穿透（`类::Attach(T*) 返回壳` 形态）。
 *
 * @internal 六种消费位正例：局部初始化/链式直调/operator=
 *           存储/返回槽/ctor-init 字段写/字段
 *           operator= 写——T* 位实参的 pointee 证据穿透进壳槽并接
 *           通虚派发（单目标 ImplFoo::Virt，Medium 链 0.7）；多参恰一
 *           T* 工厂同享。负例矩阵（双 T* 位、无 T* 位、
 *           非静态方法、无 operator->、非模板壳、自由工厂、值
 *           形参、返回非壳）：判据拒绝后负例槽零实现类混入（形状闸
 *           整档不发）；libc++ 静态方法返回 own 形参标量的干扰源不触
 *           发。
 */
void attachshell_factory_penetration(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "attachshell");
    cg::test::check(r, sum.failed == 0, "attach-shell fixture indexes cleanly");
    const auto owner_of = [&](const cg::db::TypeHintRow& row)
    {
        const auto s = cg::db::get_symbol(store, row.owner_id);
        return s ? s->qualified_name : std::string{};
    };
    // 穿透行形态：related_var 恰是 T* 位实参（局部/返回槽直名，字段写
    // 侧 @fnv 域别名）——判据命中的直接物证。
    bool c1_row = false;
    bool b1_row = false;
    bool c3_row = false;
    bool c4_row = false;
    bool c5_row = false;
    bool c6_row = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        const std::string owner = owner_of(row);
        if (owner == "attach_shell_host" && row.var_name == "s")
        {
            c1_row = row.related_var == "raw";
        }
        if (owner == "attach_shell_host" && row.var_name == "u")
        {
            b1_row = row.related_var == "raw";
        }
        if (owner == "attach_shell_host" && row.var_name == "t")
        {
            // 写侧路径的别名自带 @fnv 域编码（shell_store_rhs 归一），
            // 名字段以 |raw 收尾。
            c3_row = row.related_var.starts_with("@fnv|")
                     && row.related_var.ends_with("|raw");
        }
        if (owner == "ret_shell" && row.var_name == "#ret")
        {
            c4_row = row.related_var == "p";
        }
        if (owner == "Holder" && row.var_name == "f_")
        {
            if (row.fallback_reason == "ctor_init_flow")
            {
                c5_row = row.related_var.starts_with("@fnv|")
                         && row.related_var.ends_with("|p");
            }
            if (row.fallback_reason == "field_flow")
            {
                c6_row = row.related_var.starts_with("@fnv|")
                         && row.related_var.ends_with("|p");
            }
        }
    }
    cg::test::check(r, c1_row, "C1 local init aliases the factory argument");
    cg::test::check(
        r,
        b1_row,
        "multi-argument factory aliases its single T* argument");
    cg::test::check(
        r,
        c3_row,
        "C3 operator= store aliases the factory argument");
    cg::test::check(r, c4_row, "C4 return slot aliases the factory argument");
    cg::test::check(
        r,
        c5_row,
        "C5 ctor-init field store carries the ctor domain");
    cg::test::check(
        r,
        c6_row,
        "C6 field operator= store carries the writer domain");
    // 链式直调站点：接收者名穿透为工厂实参名。
    bool c2_receiver = false;
    for (const cg::db::CallSiteRow& row : store.all_call_sites())
    {
        const auto caller = cg::db::get_symbol(store, row.caller_id);
        if (caller && caller->qualified_name == "attach_shell_host"
            && row.callee_name == "Virt" && row.receiver_name == "raw")
        {
            c2_receiver = true;
        }
    }
    cg::test::check(
        r,
        c2_receiver,
        "C2 chained factory receiver resolves to the argument");
    // 槽终态：正例槽拿实现类；负例槽零实现类混入。
    const auto slot_has_impl =
        [&](std::string_view domain, std::string_view var)
    {
        for (const cg::db::AnalysisSlotRow& row : store.all_analysis_slots())
        {
            if (row.owner_key.find(domain) != std::string::npos
                && row.var_name == var)
            {
                return row.type_set.find("ImplFoo") != std::string::npos;
            }
        }
        return false;
    };
    cg::test::check(
        r,
        slot_has_impl("attach_shell_host", "s"),
        "C1 slot reaches the implementation");
    cg::test::check(
        r,
        slot_has_impl("attach_shell_host", "t"),
        "C3 slot reaches the implementation");
    cg::test::check(
        r,
        slot_has_impl("attach_shell_host", "u"),
        "B1 slot reaches the implementation");
    cg::test::check(
        r,
        slot_has_impl("ret_shell", "#ret"),
        "C4 return slot reaches the implementation");
    cg::test::check(
        r,
        slot_has_impl("Holder", "f_"),
        "C5/C6 field slot reaches the implementation");
    for (const std::string_view neg : {"tp", "fo", "fv", "cloned"})
    {
        cg::test::check(
            r,
            !slot_has_impl("attach_shell_host", neg),
            "negative factory slot stays free of the "
            "implementation");
    }
    // B3 free factory：调用点本身绝不被工厂形态穿透（无实参直名行）；
    // 工厂体内的壳构造穿透 + 返回流是既有机制，不在本断言面。
    bool mb_penetrated = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        if (owner_of(row) == "attach_shell_host" && row.var_name == "mb"
            && row.related_var == "raw")
        {
            mb_penetrated = true;
        }
    }
    cg::test::check(
        r,
        !mb_penetrated,
        "free factory call site never takes the penetration");
    // 派发边：六消费位汇成单目标实现；host 的全部虚边不得指向别处。
    const std::vector<DEdge> edges = dispatch_edges(store);
    const DEdge*             host_edge =
        find_edge(edges, "attach_shell_host", "ImplFoo::Virt");
    cg::test::check(
        r,
        host_edge != nullptr,
        "factory penetration connects the virtual dispatch");
    cg::test::check(
        r,
        host_edge != nullptr && host_edge->confidence == 0.7,
        "single-source factory chain stays at medium");
    cg::test::check(
        r,
        find_edge(edges, "Holder::use", "ImplFoo::Virt") != nullptr,
        "ctor-init/field store chains reach the dispatch");
    for (const DEdge& e : edges)
    {
        if (e.src_name == "attach_shell_host" && e.confidence > 0.0)
        {
            cg::test::check(
                r,
                e.dst_name == "ImplFoo::Virt",
                "no edge escapes the sole implementation");
        }
    }
}

const cg::test::Registrar reg_attachshell_factory{
    "analysis.attachshell.factory_penetration",
    &attachshell_factory_penetration};

const cg::test::Registrar reg_symbits_tier_bit_equivalence{
    "analysis.symbits_tier_bit_equivalence",
    &symbits_tier_bit_equivalence};

// --------------------------------------------- tmplFold 上行折算 ----------

/**
 * @brief CRTP 特化行 -> 主模板行的上行折算（ancestor_distances 并行
 *        extends 与 specializes 正向）。
 *
 * @internal CRTP 模板基形态（class Impl : Base<Impl>）的 extends 边挂在
 *           主模板行、特化行无 extends 出边——只走 extends 的祖先链到
 *           特化行即死，nearest_overrides 对方法宿主在主模板行的家族
 *           结构性零交集（family_ambiguous 站点中 CRTP
 * 模板主/实例链断裂带的根因）。四形态断言： 动态路径唯一默认实现（famamb ->
 * direct_new 收敛 1.0）、静 态路径 final 接收者（absent ->
 * static_final_unique，且绝不 指向家族内兄弟覆盖）、两级模板链（折算与 extends
 * 混合跳跃 三步达宿主）、跨家族隔离护栏（同名方法互不串，零误连）。
 */
void crtp_template_fold_upfold(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "crtp-template-fold");
    cg::test::check(r, sum.failed == 0, "crtp-template-fold indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 形态 A：dyn=AcmeGadget 的距离表折算后含主模板行（唯一步），唯一
    // 默认实现收敛——direct_new 强证据保 1.0。
    const auto gadget = site_for(store, "probe_gadget_dynamic", "tick", true);
    cg::test::check(
        r,
        gadget.has_value() && gadget->resolved
            && gadget->candidates.find("direct_new_nearest_converged")
                   != std::string::npos,
        "default-impl-on-primary converges after the upward fold");
    const DEdge* gadget_edge = nullptr;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "probe_gadget_dynamic" && e.confidence == 1.0)
        {
            gadget_edge = &e;
        }
    }
    cg::test::check(
        r,
        gadget_edge != nullptr
            && gadget_edge->dst_name.find("tick") != std::string::npos,
        "folded dynamic site carries a single 1.0 edge at the primary");

    // 形态 B：final 具体类无成员，最近覆盖只能落主模板行——折算前
    // absent 不直连，折算后 static_final_unique；唯一性护栏：绝不指向
    // 家族内兄弟覆盖 AcmePlainUser::cost。
    const auto sealed = site_for(store, "probe_sealed_static", "cost", true);
    cg::test::check(
        r,
        sealed.has_value() && sealed->resolved
            && sealed->candidates.find("static_final_unique")
                   != std::string::npos,
        "final CRTP receiver resolves via static_final_unique");
    const DEdge* sealed_edge = nullptr;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "probe_sealed_static" && e.confidence == 1.0)
        {
            sealed_edge = &e;
        }
    }
    cg::test::check(
        r,
        sealed_edge != nullptr
            && sealed_edge->dst_name.find("cost") != std::string::npos
            && sealed_edge->dst_name.find("AcmePlainUser") == std::string::npos,
        "static final direct never points at the sibling override");

    // 形态 E：两级模板链连跳三步（特化行 -> 主模板 -> dependent 基类
    // 主模板行）才到 step 宿主——折算与 extends 混合链路可达。
    const auto deep = site_for(store, "probe_deep_chain", "step", true);
    cg::test::check(
        r,
        deep.has_value() && deep->resolved
            && deep->candidates.find("_nearest_converged") != std::string::npos,
        "two-level template chain reaches the primary host");
    bool deep_edge = false;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "probe_deep_chain" && e.confidence == 1.0
            && e.dst_name.find("step") != std::string::npos)
        {
            deep_edge = true;
        }
    }
    cg::test::check(
        r,
        deep_edge,
        "two-level chain site carries a 1.0 edge at the primary");

    // 形态 C：跨家族隔离——同名 tag 方法、独立 CRTP 家族，AcmeBeta 的
    // 候选面绝不混入 AcmeAlpha 家族成员（零误连纪律）。
    const auto cross = site_for(store, "probe_cross_family", "tag", true);
    cg::test::check(
        r,
        cross.has_value() && cross->resolved
            && cross->candidates.find("AcmeAlpha") == std::string::npos,
        "same-name sibling family never leaks into the candidate face");
    bool no_leak = true;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "probe_cross_family"
            && e.dst_name.find("AcmeAlpha") != std::string::npos)
        {
            no_leak = false;
        }
    }
    cg::test::check(
        r,
        no_leak,
        "no dispatch edge from the isolated family crosses over");
}

const cg::test::Registrar reg_crtp_template_fold_upfold{
    "analysis.dispatch.crtp_template_fold_upfold",
    &crtp_template_fold_upfold};

// ------------------------------------------------ implicitThis 臂 ----------

/**
 * @brief 隐式 this 穿透模板语境：CRTP 主模板行方法体内的裸虚调用按
 *        descendant_subtree 折算闭包决议。
 *
 * @internal 调用方所属记录带 is_template 位时不再整档跳过，进专用分
 *           支（绕过 has_descendant 纯 extends 口径的 final 判定——主
 *           模板看不见特化行后代会假阳性直连）。三变体：特化侧不覆盖
 *           时闭包内唯一非纯成员（主模板自己的共享实现）→ High(1.0)
 *           直连 reason=implicit_this_template_unique，闭包外同根接
 *           口实现绝不入候选（零误连）；特化侧覆盖 → 闭包内多命中 →
 *           Low 披露 reason=implicit_this_template_narrowed（不猜，
 *           绝不发推断边）；折算缺边守门 → 主模板只声明不定型使实例行
 *           缺 specializes 出边，唯一命中降 0.4 披露 reason 带
 *           fold_gap（缺口在案不直连）。
 */
void implicit_this_template_crtp(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "implicit-this-crtp");
    cg::test::check(r, sum.failed == 0, "implicit-this-crtp indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 变体一：唯一命中直连——1.0 边落在主模板自己的 AddRef 上。
    const auto uniq =
        site_for(store, "AcmePartBase::QueryInterface", "AddRef", true);
    cg::test::check(
        r,
        uniq.has_value() && uniq->resolved
            && uniq->candidates.find("implicit_this_template_unique")
                   != std::string::npos,
        "unique fold hit direct-connects with "
        "implicit_this_template_unique");
    const DEdge* uniq_edge = nullptr;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "AcmePartBase::QueryInterface" && e.confidence == 1.0)
        {
            uniq_edge = &e;
        }
    }
    cg::test::check(
        r,
        uniq_edge != nullptr
            && uniq_edge->dst_name.find("AcmePartBase::AddRef")
                   != std::string::npos,
        "unique fold hit carries a single 1.0 edge at the primary");
    // 零误连护栏：闭包外实现（AcmeForeignPart）不进候选面、不进边。
    cg::test::check(
        r,
        uniq.has_value()
            && uniq->candidates.find("AcmeForeignPart") == std::string::npos,
        "out-of-closure sibling never enters the unique-hit face");
    bool no_foreign_edge = true;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "AcmePartBase::QueryInterface"
            && e.dst_name.find("AcmeForeignPart") != std::string::npos)
        {
            no_foreign_edge = false;
        }
    }
    cg::test::check(
        r,
        no_foreign_edge,
        "no dispatch edge crosses to the out-of-closure sibling");

    // 变体二：特化侧覆盖——闭包内两命中（主模板行 + 显式特化行的
    // AddRef），披露不猜。
    const auto multi =
        site_for(store, "AcmeProxyBase::QueryInterface", "AddRef", true);
    cg::test::check(
        r,
        multi.has_value() && !multi->resolved
            && multi->fallback_reason.find("implicit_this_template_narrowed")
                   != std::string::npos
            && multi->candidates.find("AcmeProxyBase<int>")
                   != std::string::npos,
        "specialization-side override stays a narrowed disclosure");
    bool multi_inferred = false;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "AcmeProxyBase::QueryInterface" && e.confidence > 0.0)
        {
            multi_inferred = true;
        }
    }
    cg::test::check(
        r,
        !multi_inferred,
        "multi-hit template site never emits an inferred edge");

    // 变体三：折算缺边守门——唯一命中降 0.4 披露，绝不直连。
    const auto gated = site_for(store, "AcmeGateBase::open", "alive", true);
    cg::test::check(
        r,
        gated.has_value() && !gated->resolved
            && gated->fallback_reason.find("implicit_this_template_fold_gap")
                   != std::string::npos,
        "fold-gap in the family downgrades the unique hit to disclosure");
    bool gated_inferred = false;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "AcmeGateBase::open" && e.confidence > 0.0)
        {
            gated_inferred = true;
        }
    }
    cg::test::check(
        r,
        !gated_inferred,
        "fold-gap site never emits an inferred edge");
}

const cg::test::Registrar reg_implicit_this_template{
    "analysis.dispatch.implicit_this_template",
    &implicit_this_template_crtp};

// ---------------------------------------------------- 写穿性三闸 ----------

/**
 * @brief 闸一：返回槽壳透传（`return 壳变量;` 的拷贝/move 构造形态）。
 *
 * @internal 构造穿透臂对无 T* 形参位的拷贝/move 构造此前整档 no-op，
 *           返回槽零行、消费方 return_flow 边建而值空（断链在中继第一
 *           跳）。拷贝源别名边把返回槽接到源壳变量槽（pointee 语义），
 *           上游工厂证据经函数内传播并入；花括号壳构造单元素穿透。断言面：
 *           中继返回的别名行、消费方收敛唯一实现、局部拷贝同语义、花
 *           括号构造穿透且壳类型不入槽、两实现并流保持披露 may（无唯
 *           一边）、非模板壳维持既有聚合路径。
 */
void return_shell_passthrough(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "return-shell-passthrough");
    cg::test::check(
        r,
        sum.failed == 0,
        "return-shell-passthrough indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 中继工厂：#ret 上的拷贝源别名行（槽级单值边）。
    bool relay_alias = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        const auto owner = cg::db::get_symbol(store, row.owner_id);
        if (owner && owner->qualified_name == "RelaySession"
            && row.var_name == "#ret"
            && row.source
                   == static_cast<int>(cg::analysis::HintSource::LocalCopy)
            && row.related_var == "ctx"
            && row.fallback_reason == "shell_pointee_flow")
        {
            relay_alias = true;
        }
    }
    cg::test::check(
        r,
        relay_alias,
        "return of a shell local emits the copy-source alias row on #ret");

    // 消费方：中继链收敛到唯一实现（0.7 档以上推断边）。
    const DEdge* relay_edge = nullptr;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "ConsumeRelay" && e.confidence >= 0.7)
        {
            relay_edge = &e;
        }
    }
    cg::test::check(
        r,
        relay_edge != nullptr
            && relay_edge->dst_name.find("AcmeSession") != std::string::npos,
        "relay consumer resolves through the shell copy alias");

    // 局部拷贝（拷贝初始化形态）同语义。
    bool copy_edge = false;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "UseLocalCopy" && e.confidence >= 0.7
            && e.dst_name.find("AcmeSession") != std::string::npos)
        {
            copy_edge = true;
        }
    }
    cg::test::check(
        r,
        copy_edge,
        "copy-initialization of a shell carries the same alias semantics");

    // 花括号壳构造：单元素穿透（壳类型绝不如槽），保持 High 直证。
    const DEdge* brace_edge = nullptr;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "ConsumeBrace" && e.confidence >= 0.9)
        {
            brace_edge = &e;
        }
    }
    cg::test::check(
        r,
        brace_edge != nullptr
            && brace_edge->dst_name.find("AcmeSession") != std::string::npos,
        "brace construction of a shell penetrates the single element");
    bool brace_shell_type = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        const auto owner = cg::db::get_symbol(store, row.owner_id);
        if (owner && owner->qualified_name == "ConsumeBrace"
            && row.var_name == "s"
            && row.source
                   != static_cast<int>(cg::analysis::HintSource::Declared)
            && row.type_set.find("AcmeHandle") != std::string::npos)
        {
            brace_shell_type = true;
        }
    }
    cg::test::check(
        r,
        !brace_shell_type,
        "the shell's own static type never enters the pointee slot");

    // 两实现并流：披露 may，双方候选俱在，绝不发唯一边。
    const auto amb = site_for(store, "ConsumeAmbiguous", "Id", true);
    cg::test::check(
        r,
        amb.has_value()
            && amb->candidates.find("AcmeSession") != std::string::npos
            && amb->candidates.find("AcmeAltSession") != std::string::npos,
        "two-implementation merge keeps both candidates disclosed");
    bool amb_unique = false;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "ConsumeAmbiguous" && e.confidence >= 0.7)
        {
            amb_unique = true;
        }
    }
    cg::test::check(
        r,
        !amb_unique,
        "the ambiguous merge never fabricates a unique dispatch edge");

    // 非模板壳：模板判据外，既有聚合路径原样（不崩、不变更）。
    bool plain_legacy = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        const auto owner = cg::db::get_symbol(store, row.owner_id);
        if (owner && owner->qualified_name == "RelayPlain"
            && row.var_name == "#ret"
            && row.source
                   == static_cast<int>(
                       cg::analysis::HintSource::DirectAggregate))
        {
            plain_legacy = true;
        }
    }
    cg::test::check(
        r,
        plain_legacy,
        "non-template shells keep the legacy aggregate path");
}

const cg::test::Registrar reg_return_shell_passthrough{
    "analysis.dispatch.return_shell_passthrough",
    &return_shell_passthrough};

/**
 * @brief 闸二：出参写回补采（工具写回 f(值, 出参槽) 与出参壳取址写回
 *        `*v.Put() = e` 两形态）。
 *
 * @internal 工厂把 `*pp = 值` 委托给中转函数时，工厂自己的出参形参槽
 *           零行、调用方写回触发永不点火；出参包装壳经取址方法解引用
 *           写回时同病。签名方向闸（形参双剥恰两层且最深 pointee 是记
 *           录）+ 值配对闸（值 pointee 记录派生自或等于出参 pointee）
 *           + 值源三形态（壳工厂模板实参直取/透传接收者/一级指针源）。
 *           断言面：工具写回行落工厂出参槽、两形态消费方均收敛唯一、
 *           非派生值的配对闸拒绝（无行、消费方保持 fallback）。
 */
void out_param_write_back(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "out-param-write-back");
    cg::test::check(r, sum.failed == 0, "out-param-write-back indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 工具写回行：工厂出参槽上的 out_param_tool 行（值源 = 壳工厂模板
    // 实参直取）。
    bool tool_row = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        const auto owner = cg::db::get_symbol(store, row.owner_id);
        if (owner && owner->qualified_name == "AcmeCreateSvc"
            && row.var_name == "pp_out"
            && row.source
                   == static_cast<int>(cg::analysis::HintSource::OutParamFlow)
            && row.fallback_reason == "out_param_tool"
            && row.type_set.find("AcmeSvc") != std::string::npos)
        {
            tool_row = true;
        }
    }
    cg::test::check(
        r,
        tool_row,
        "the delegated write lands on the factory's out-param slot");

    // 工具写回形态消费方：写回触发点火，收敛唯一实现。
    const DEdge* tool_edge = nullptr;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "ConsumeTool" && e.confidence >= 0.7)
        {
            tool_edge = &e;
        }
    }
    cg::test::check(
        r,
        tool_edge != nullptr
            && tool_edge->dst_name.find("AcmeSvc") != std::string::npos,
        "tool write-back consumer resolves to the unique implementation");

    // 取址写回行：绑定出参槽上的 out_param_flow 行（related 边到
    // DirectNew 局部）。
    bool put_row = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        const auto owner = cg::db::get_symbol(store, row.owner_id);
        if (owner && owner->qualified_name == "AcmeCreateSvcOut"
            && row.var_name == "pp_out"
            && row.source
                   == static_cast<int>(cg::analysis::HintSource::OutParamFlow)
            && row.fallback_reason == "out_param_flow"
            && row.related_var == "impl")
        {
            put_row = true;
        }
    }
    cg::test::check(
        r,
        put_row,
        "the address-of write through the bound shell reaches the slot");

    const DEdge* put_edge = nullptr;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "ConsumeOutBox" && e.confidence >= 0.7)
        {
            put_edge = &e;
        }
    }
    cg::test::check(
        r,
        put_edge != nullptr
            && put_edge->dst_name.find("AcmeSvc") != std::string::npos,
        "address-of write-back consumer resolves to the implementation");

    // 非派生值：配对闸拒绝，无补采行（槽证据层守恒）。
    bool bad_row = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        const auto owner = cg::db::get_symbol(store, row.owner_id);
        if (owner && owner->qualified_name == "AcmeCreateUnrelated"
            && row.var_name == "pp_out"
            && row.fallback_reason == "out_param_tool")
        {
            bad_row = true;
        }
    }
    cg::test::check(
        r,
        !bad_row,
        "a value outside the out pointee's hierarchy never locks the slot");
    // 消费方站点：拒绝配对后无任何动态证据，静态决议启用前维持 fallback 带；
    // 宿主抽象直连后按全库唯一非纯 override 直连（Work 家族唯一
    // 实现 AcmeSvc，接口纯虚——与槽证据无关的家族级证明面）。
    const auto bad = site_for(store, "ConsumeUnrelated", "Work", true);
    cg::test::check(
        r,
        bad.has_value() && bad->resolved
            && bad->candidates.find("family_unique_override")
                   != std::string::npos,
        "the rejected pairing still resolves via the library-unique override");

    // 三级指针负形（方向闸深度半边）：out-out 形态（槽收槽）即使另一位
    // 实参是派生记录值也必须被深度闸拒绝——零 out_param_tool 证据行落
    // 三级槽。
    bool deep_row = false;
    for (const cg::db::TypeHintRow& row : store.all_type_hints())
    {
        const auto owner = cg::db::get_symbol(store, row.owner_id);
        if (owner && owner->qualified_name == "AcmeCreateDeep"
            && row.fallback_reason == "out_param_tool")
        {
            deep_row = true;
        }
    }
    cg::test::check(
        r,
        !deep_row,
        "a triple-level pointer slot never receives a tool write row");
}

const cg::test::Registrar reg_out_param_write_back{
    "analysis.dispatch.out_param_write_back",
    &out_param_write_back};

/**
 * @brief 接收者在场守卫：隐式 this 回退对三类在源码里写了接收者表达式
 *        的形态绝不接管调用方所属类；真裸 this 的回退语义保持。
 *
 * @internal 出参解引用（`(*pp_out)->AddRef()`）解析到出参槽并收敛到写
 *           入的实现；链式 getter（`ref.get().AddRef()`，非虚中间环节
 *           对象子结构性不可达）置在场未解析位、站点维持披露；裸数组
 *           下标（`slots[i]->AddRef()`）无元素行时同样维持披露。三形
 *           态站点都不得出现指向调用方类自身 override 的推断边（同家
 *           族错对象直连）；对照的裸 this 调用保持 1.0 直连
 *           （reason=implicit_this_unique）。
 */
void recvfail_guard(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "recvfail-guard");
    cg::test::check(r, sum.failed == 0, "recvfail-guard indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 形态一（出参解引用）：接收者名解析到出参槽，写回行类型化槽内
    // 容，动态证据收敛到写入的实现。
    const DEdge* deref = find_edge(
        edges,
        "AcmeHostWidget::ProduceJson",
        "AcmeJsonWidget::AddRef");
    cg::test::check(
        r,
        deref != nullptr && deref->confidence >= 0.7,
        "out-param dereference resolves to the stored implementation");
    const DEdge* deref_wrong = find_edge(
        edges,
        "AcmeHostWidget::ProduceJson",
        "AcmeHostWidget::AddRef");
    cg::test::check(
        r,
        deref_wrong == nullptr,
        "out-param dereference never adopts the caller's own override");
    // 结构判据（reason 无关）：观察站的推断边置信上限 0.7——任何
    // High(1.0) 直连（无论换什么 reason 路径：隐式 this 回退、宿主
    // 抽象直连、未来新增分支）都是把动态收敛站伪装成唯一目标，一律
    // 违例。字面 reason 匹配对"换路径迁移"结构性失明（真库对照实锤：守
    // 卫挡掉 implicit_this 后同一站点被 family_unique_override 重新
    // 接管仍 High）。
    const double deref_top =
        top_inferred_confidence(edges, "AcmeHostWidget::ProduceJson");
    cg::test::check(
        r,
        deref_top <= 0.7,
        "out-param dereference site never carries a high-confidence "
        "inferred edge, whatever the reason path");

    // 形态二（链式 getter）：非虚中间环节不可达，接收者双空 + 在场位
    // ——回退整档不接管，站点维持披露（无推断边、原因如实）。
    const DEdge* chain_wrong =
        find_edge(edges, "AcmeHostWidget::ChainCall", "AcmeHostWidget::AddRef");
    cg::test::check(
        r,
        chain_wrong == nullptr,
        "chained-getter site never adopts the caller's own override");
    const auto chain_site =
        site_named(store, "AcmeHostWidget::ChainCall", "AddRef");
    cg::test::check(
        r,
        chain_site.has_value() && !chain_site->resolved
            && chain_site->fallback_reason.find("no_dynamic_type_evidence")
                   != std::string::npos,
        "chained-getter site stays disclosed with the honest reason");
    cg::test::check(
        r,
        top_inferred_confidence(edges, "AcmeHostWidget::ChainCall") < 0.95,
        "chained-getter site stays free of high-confidence inferred "
        "edges, whatever the reason path");

    // 形态三（裸数组下标）：接收者是数组元素，无元素行时维持披露。
    const DEdge* sub_wrong =
        find_edge(edges, "SubscriptCall", "AcmeHostWidget::AddRef");
    cg::test::check(
        r,
        sub_wrong == nullptr,
        "subscript site never adopts the caller's own override");
    cg::test::check(
        r,
        top_inferred_confidence(edges, "SubscriptCall") < 0.95,
        "subscript site stays free of high-confidence inferred edges, "
        "whatever the reason path");

    // 对照（真裸 this）：无接收者表达式的调用保持隐式 this 直连。
    const DEdge* solo =
        find_edge(edges, "AcmeSoloHost::Run", "AcmeSoloHost::Poke");
    cg::test::check(
        r,
        solo != nullptr && solo->confidence == 1.0,
        "bare-this control keeps the direct resolution");
    const auto solo_site = site_named(store, "AcmeSoloHost::Run", "Poke");
    cg::test::check(
        r,
        solo_site.has_value() && solo_site->resolved
            && solo_site->candidates.find("implicit_this_unique")
                   != std::string::npos,
        "bare-this control still carries reason=implicit_this_unique");
}

const cg::test::Registrar reg_recvfail_guard{
    "analysis.dispatch.recvfail_guard",
    &recvfail_guard};

/**
 * @brief 闸三：iid 锁形状（QI(令牌, 出参位) 与成员模板 As 形态）——
 *        行发射熔断守恒。
 *
 * @internal iid 令牌与出参方向位/As 成员模板的判据在采集层保留，但两
 *           形态的证据行发射在真库（687 TU 全量）上触发未定位的写库
 *           段段错误（隔离实验：发射关全量成功、发射开稳定崩溃），按
 *           "拿不准的形态宁可不开闸"纪律整体熔断（见 iid_lock_call 的
 *           熔断注释）。本测试断言熔断守恒：全库零 qi 证据行、负形依
 *           旧闭合、索引零失败（判定路径仍在跑、不崩）。消费方站点在
 *           熔断下无动态证据，由静态决议接管（宿主抽象直连按
 *           家族级证明面 High 直连、final 具体类接收者走既有第一优
 *           先）——直连不消费槽证据，与证据行熔断不冲突。
 */
void iid_lock_write_through(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "iid-lock-write-through");
    cg::test::check(
        r,
        sum.failed == 0,
        "iid-lock-write-through indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    const auto row_of = [&](std::string_view owner,
                            std::string_view var,
                            std::string_view reason) -> bool
    {
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            const auto own = cg::db::get_symbol(store, row.owner_id);
            if (own && own->qualified_name == owner && row.var_name == var
                && row.fallback_reason == reason)
            {
                return true;
            }
        }
        return false;
    };

    // 熔断守恒：三种形态全部零证据行。
    cg::test::check(
        r,
        !row_of("ConsumeQI", "p_backend", "qi_iid_lock"),
        "the QI out slot stays row-free under the fused emission");
    cg::test::check(
        r,
        !row_of("ConsumeWideQI", "p_wide", "qi_iid_lock"),
        "the wide QI out slot stays row-free under the fused emission");
    cg::test::check(
        r,
        !row_of("ConsumeAs", "backend", "qi_iid_ret"),
        "the As result slot stays row-free under the fused emission");

    // 单实现接口：熔断下零证据行（上方三条守恒断言），消费方站点不再
    // 有动态证据——宿主抽象直连按家族级证明面（Mat 纯虚 × 全库唯一
    // 非纯 override）High 直连；这与 iid 证据行熔断不冲突（直连不消费
    // 槽证据），误连面为空（全库唯一实现是唯一可能目标）。
    const auto qi = site_for(store, "ConsumeQI", "Mat", true);
    cg::test::check(
        r,
        qi.has_value() && qi->resolved
            && qi->candidates.find("family_unique_override")
                   != std::string::npos
            && qi->candidates.find("\"confidence\":\"high\"")
                   != std::string::npos,
        "the QI consumer resolves via the library-unique override at high");
    bool qi_unique = false;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "ConsumeQI" && e.confidence == 1.0
            && e.dst_name.find("CpuBackend") != std::string::npos)
        {
            qi_unique = true;
        }
    }
    cg::test::check(r, qi_unique, "the direct link edge lands at exactly 1.0");

    // 双实现接口：无唯一边（熔断下同样不产唯一边）。
    bool wide_unique = false;
    for (const DEdge& e : edges)
    {
        if (e.src_name == "ConsumeWideQI" && e.confidence >= 0.7
            && e.dst_name.find("Wide") != std::string::npos)
        {
            wide_unique = true;
        }
    }
    cg::test::check(
        r,
        !wide_unique,
        "a two-implementation family never yields a unique edge");

    // As 形态消费方：结果槽无锁（无证据行），接口指针接收者按宿主
    // 抽象直连；具体类接收者的对照站（ConsumeAsConcrete）走既有第一
    // 优先 sealed 根直连。
    const auto as = site_for(store, "ConsumeAs", "Mat", true);
    cg::test::check(
        r,
        as.has_value() && as->resolved
            && as->candidates.find("family_unique_override")
                   != std::string::npos,
        "the As consumer resolves via the library-unique override");
    const auto as_conc = site_for(store, "ConsumeAsConcrete", "Mat", true);
    cg::test::check(
        r,
        as_conc.has_value() && as_conc->resolved
            && as_conc->candidates.find("static_unique_override")
                   != std::string::npos,
        "the concrete receiver resolves via the sealed static root");

    // 负形：非令牌实参与具体类模板实参闸门依旧闭合。
    cg::test::check(
        r,
        !row_of("ConsumeNoToken", "p_backend", "qi_iid_lock"),
        "a non-token argument keeps the QI gate closed");
    cg::test::check(
        r,
        !row_of("ConsumeAsConcrete", "backend", "qi_iid_ret"),
        "a concrete template argument keeps the As gate closed");
    const auto no_tok = site_for(store, "ConsumeNoToken", "Mat", true);
    cg::test::check(
        r,
        no_tok.has_value() && no_tok->resolved
            && no_tok->candidates.find("family_unique_override")
                   != std::string::npos,
        "the no-token consumer resolves via the library-unique override");
}

const cg::test::Registrar reg_iid_lock_write_through{
    "analysis.dispatch.iid_lock_write_through",
    &iid_lock_write_through};

/**
 * @brief 异族豁免封口：缺席细分三态（异族豁免提档 / 接口名阻断 /
 *        final 封口缺失阻断）。
 *
 * @internal 缺席细分语义：key 解析失败与家族零交集分开计数；家族零
 *           交集的缺席类型逐个对照接收者声明面排除域（extends 后代
 *           闭包并集）分类——闭包外是可证噪声（豁免出封口条件），
 *           闭包内的接口名是合法声明面的合法缺席（其正解是家族闭包
 *           展开后参与收敛，保持阻断）。四闸合取的后两闸：槽截断观
 *           测位与 final 封口（唯一目标的方法级/宿主类级 final 语法
 *           位，或家族全库唯一非纯成员旁路）。污染手段与既有夹具同
 *           款：C 风格转换保留操作数证据（界外类型灌入形参槽）、
 *           static_cast 向下转型把接口名记为合法动态候选。
 */
void unrelated_absence_final_seal_gates(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum =
        index_fixture(store, "dispatch-unrelated-absence-final-seal");
    cg::test::check(
        r,
        sum.failed == 0,
        "dispatch-unrelated-absence-final-seal indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 态一：异族缺席豁免 + 类级 final 封口 -> 提档 medium（0.7）。
    const DEdge* xeno = find_edge(edges, "render", "AcmeGlyph::Render");
    cg::test::check(
        r,
        xeno != nullptr && xeno->confidence == 0.7,
        "xeno-exempt convergence promotes to medium (0.7)");
    const auto xeno_site = site_for(store, "render", "Render", true);
    cg::test::check(
        r,
        xeno_site.has_value() && xeno_site->resolved
            && xeno_site->candidates.find("nearest_converged_filtered")
                   != std::string::npos,
        "promoted site carries reason=nearest_converged_filtered");
    // 家族第二实现不得混入（final 封口封死的是槽观测外类型，不是披露
    // 候选——nearest 并集唯一由异族豁免保证）。
    cg::test::check(
        r,
        xeno_site.has_value()
            && xeno_site->candidates.find("|AcmePlainGlyph|Render|")
                   == std::string::npos,
        "the off-slot sibling implementation stays out of candidates");

    // 态四：异族豁免成立但 final 封口缺失（家族多实现、槽内目标无
    // final 位）-> 保持弱带 0.4，不伪装唯一。
    const DEdge* no_final = find_edge(edges, "size", "AcmeWidePane::Size");
    cg::test::check(
        r,
        no_final != nullptr && no_final->confidence == 0.4,
        "no-final unique convergence stays low (0.4)");
    const auto no_final_site = site_for(store, "size", "Size", true);
    cg::test::check(
        r,
        no_final_site.has_value()
            && no_final_site->candidates.find("nearest_converged_filtered")
                   == std::string::npos,
        "the unsealed target never reports nearest_converged_filtered");

    // 态二：接口名缺席（抽象中间接口是接收者声明面的后代）-> 阻断，
    // 保持 0.4（接口是合法声明面，家族闭包展开前不得豁免；家族另加
    // 第二实现使其非唯一，全库唯一旁路同样不可达，归因精确）。
    const DEdge* iface = find_edge(edges, "area", "AcmeDisc::Area");
    cg::test::check(
        r,
        iface != nullptr && iface->confidence == 0.4,
        "interface-name absence keeps the low band (0.4)");
    const auto iface_site = site_for(store, "area", "Area", true);
    cg::test::check(
        r,
        iface_site.has_value()
            && iface_site->candidates.find("nearest_converged_filtered")
                   == std::string::npos,
        "an interface-name absence is never exempted");
    cg::test::check(
        r,
        iface_site.has_value()
            && iface_site->candidates.find("|AcmeSquare|Area|")
                   == std::string::npos,
        "the off-slot sibling implementation stays out of the low band");
}

const cg::test::Registrar reg_unrelated_absence_final_seal_gates{
    "analysis.dispatch.unrelated_absence_final_seal_gates",
    &unrelated_absence_final_seal_gates};

/**
 * @brief 截断观测硬闸：槽类型集写入截断时唯一收敛不得提档；家族
 *        全库唯一非纯实现是唯一旁路。
 *
 * @internal 截断保留排序后前 32 个类型——真实现命名排字典序最前确保
 *           保留；33 原始类型（1 真实现 + 32 异族填充）触发全局实参→
 *           形参截断点。硬闸臂：全部缺席可证异族、nearest 唯一、final
 *           封口齐备，唯独槽截断观测位为真且家族非唯一——四闸合取下
 *           仍不得提档（截断槽的“唯一”可能是观测假象）。旁路臂：同
 *           形态截断但家族全库唯一——唯一守卫不依赖槽观测完整性，提
 *           档照发。
 */
void truncation_gate_unique_bypass_gates(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum =
        index_fixture(store, "dispatch-truncation-gate-unique-bypass");
    cg::test::check(
        r,
        sum.failed == 0,
        "dispatch-truncation-gate-unique-bypass indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 硬闸臂：截断观测位为真 + 家族非唯一 -> 不提档。
    const DEdge* gate = find_edge(edges, "gate", "Acme00Solo::Pass");
    cg::test::check(
        r,
        gate != nullptr && gate->confidence == 0.4,
        "a truncated slot with a non-unique family stays low (0.4)");
    const auto gate_site = site_for(store, "gate", "Pass", true);
    cg::test::check(
        r,
        gate_site.has_value()
            && gate_site->fallback_reason.find("slot_type_truncated")
                   != std::string::npos,
        "the truncation observation stays disclosed");
    cg::test::check(
        r,
        gate_site.has_value()
            && gate_site->candidates.find("nearest_converged_filtered")
                   == std::string::npos,
        "a truncated slot never reports nearest_converged_filtered");

    // 旁路臂：同样截断，但 Mark 家族全库唯一非纯 -> 提档 medium。
    const DEdge* mark = find_edge(edges, "mark", "Acme00Mark::Mark");
    cg::test::check(
        r,
        mark != nullptr && mark->confidence == 0.7,
        "a library-unique family bypasses the truncation gate (0.7)");
    const auto mark_site = site_for(store, "mark", "Mark", true);
    cg::test::check(
        r,
        mark_site.has_value() && mark_site->resolved
            && mark_site->candidates.find("nearest_converged_filtered")
                   != std::string::npos,
        "the unique-family site carries nearest_converged_filtered");
}

const cg::test::Registrar reg_truncation_gate_unique_bypass_gates{
    "analysis.dispatch.truncation_gate_unique_bypass_gates",
    &truncation_gate_unique_bypass_gates};

/**
 * @brief 静态决议补齐：模板壳根清洗（壳主模板行不再以假缺席拖垮
 *        最近覆盖封口）与宿主抽象直连（方法纯虚 × 全库唯一非纯
 *        override）。
 *
 * @internal 壳清洗臂：剥 operator-> 调用的接收者静态面 = 壳主模板行 +
 *           模板实参，壳与目标家族零交集——清洗前壳根以缺席拖垮 1.5
 *           优先的封口（final 密封根明明收敛唯一仍压档），清洗后走
 *           既有 static_final_unique 直连（family 非唯一，靠 final 类
 *           级证据）。宿主抽象臂：被调方法自带纯虚语法位 ⟹ 宿主不可
 *           实例化 ⟹ 任何动态类型的最近覆盖必非纯，全库唯一非纯成员
 *           即唯一目标（对象存在性论证，纯虚位兼作数据质量闸）。
 */
void tmpl_shell_pure_host_direct(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "dispatch-tmpl-shell-pure-host");
    cg::test::check(
        r,
        sum.failed == 0,
        "dispatch-tmpl-shell-pure-host indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 壳清洗臂：final 密封根直连（既有 1.5 优先，壳根不再假缺席）。
    const DEdge* box = find_edge(edges, "drive_box", "AcmeMarkImpl::AddMark");
    cg::test::check(
        r,
        box != nullptr && box->confidence == 1.0,
        "the shell-washed sealed root resolves to 1.0");
    const auto box_site = site_for(store, "drive_box", "AddMark", true);
    cg::test::check(
        r,
        box_site.has_value() && box_site->resolved
            && box_site->candidates.find("static_final_unique")
                   != std::string::npos,
        "the shell root no longer drags the final gate down");

    // 宿主抽象臂：纯虚声明 + 全库唯一 override 直连。
    const DEdge* toll = find_edge(edges, "drive_toll", "AcmeTollLane::Toll");
    cg::test::check(
        r,
        toll != nullptr && toll->confidence == 1.0,
        "pure-virtual host with a unique override resolves to 1.0");
    const auto toll_site = site_for(store, "drive_toll", "Toll", true);
    cg::test::check(
        r,
        toll_site.has_value() && toll_site->resolved
            && toll_site->candidates.find("family_unique_override")
                   != std::string::npos
            && toll_site->candidates.find("\"confidence\":\"high\"")
                   != std::string::npos,
        "the abstract-host site carries reason=family_unique_override at high");
}

const cg::test::Registrar reg_tmpl_shell_pure_host_direct{
    "analysis.dispatch.tmpl_shell_pure_host_direct",
    &tmpl_shell_pure_host_direct};

/**
 * @brief 宿主抽象直连的闭包完备性双守卫：模板中间层缺边形态
 *        （family_closure_gap）与接收者间接形态（receiver_indirect_
 *        form）。
 *
 * @internal 真库对照实锤的复刻：接口经模板中间层继承且中间层不重声明该
 *           方法时，模板实现类的 override 边结构性缺失，闭包内"全库
 *           唯一非纯实现"是观测假象。守卫臂一（有名接口接收者）：唯
 *           一非纯是直接实现，但运行时目标集横跨被隐藏的模板实现，
 *           闭包缺边守卫必须挡下直连（披露挡因）。守卫臂二（下标元
 *           素接收者）：即使闭包完备，间接形态的接收者短期排除（结
 *           构判据断言不依赖 reason）。对照臂：无中间层的真唯一实现
 *           家族保持宿主抽象直连（守卫零误伤）。
 */
void family_closure_gap_gates(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "dispatch-family-closure-gap");
    cg::test::check(
        r,
        sum.failed == 0,
        "dispatch-family-closure-gap indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 守卫臂一（闭包缺边 × 有名接收者）：无 High 直连（结构判据，
    // reason 无关——换路径迁移不再穿透），挡因如实披露。
    const auto one_site = site_for(store, "describe_one", "Describe", true);
    cg::test::check(
        r,
        one_site.has_value() && !one_site->resolved
            && one_site->fallback_reason.find("family_closure_gap")
                   != std::string::npos,
        "the closure-gap site stays disclosed with the gap reason");
    cg::test::check(
        r,
        top_inferred_confidence(edges, "describe_one") < 0.95,
        "the closure-gap site never carries a high-confidence inferred "
        "edge, whatever the reason path");

    // 守卫臂二（间接接收者）：下标元素形态同样脱离 High 直连。
    cg::test::check(
        r,
        top_inferred_confidence(edges, "describe_all") < 0.95,
        "the indirect-receiver site never carries a high-confidence "
        "inferred edge, whatever the reason path");

    // 对照臂（闭包完备的真唯一家族）：宿主抽象直连保持（守卫零误伤）。
    const DEdge* stamp = find_edge(edges, "stamp_one", "AcmeStampLeaf::Stamp");
    cg::test::check(
        r,
        stamp != nullptr && stamp->confidence == 1.0,
        "a genuinely unique closed family keeps the direct link (1.0)");
    const auto stamp_site = site_for(store, "stamp_one", "Stamp", true);
    cg::test::check(
        r,
        stamp_site.has_value() && stamp_site->resolved
            && stamp_site->candidates.find("family_unique_override")
                   != std::string::npos,
        "the closed-form control still resolves via family_unique_override");

    // 间接接收者臂的挡因披露：两条守卫挡因之一如实出现（闭包缺边
    // 与间接形态同时命中时以缺边为主因）。
    const auto all_site = site_for(store, "describe_all", "Describe", true);
    cg::test::check(
        r,
        all_site.has_value() && !all_site->resolved
            && (all_site->fallback_reason.find("family_closure_gap")
                    != std::string::npos
                || all_site->fallback_reason.find("receiver_indirect_form")
                       != std::string::npos),
        "the indirect-receiver site stays disclosed with a guard reason");
}

const cg::test::Registrar reg_family_closure_gap_gates{
    "analysis.dispatch.family_closure_gap_gates",
    &family_closure_gap_gates};

/**
 * @brief 密封静态面对 may 级动态证据的静态优先（终态语义面）。
 *
 * @internal 真库对照实锤的夹具复刻边界：真库污染路径（键取出参的族并集
 *           直投接收者槽）依赖写回/裁剪/槽流的组合形态，夹具内可造
 *           的最接近形态下污染批被写回裁剪裁光（零投递）——守卫臂
 *           因此同时覆盖两条等价终态：零投递时静态侧本来直连、投递
 *           发生时静态优先守卫压回，断言统一钉在"终态 = 静态直连正
 *           确目标 + reason=static_final_unique"；守卫对真污染路径的
 *           咬合由 DAS 重索引复验背书（真站唯一可复刻环境）。对照
 *           臂：批部分面内（含声明类型）时动态带保持多候选披露，静
 *           态直连不抢班。
 */
void static_face_priority(R& r)
{
    db::GraphStore store{":memory:"};
    const auto     sum = index_fixture(store, "dispatch-static-face-priority");
    cg::test::check(
        r,
        sum.failed == 0,
        "dispatch-static-face-priority indexes cleanly");
    const std::vector<DEdge> edges = dispatch_edges(store);

    // 守卫臂：密封静态面 × 全批面外并集——静态直连夺回（1.0）。
    const DEdge* direct =
        find_edge(edges, "disconnect_off_face", "AcmeSessionObj::Release");
    cg::test::check(
        r,
        direct != nullptr && direct->confidence == 1.0,
        "the sealed static face outranks the off-face union band (1.0)");
    const auto off_site =
        site_for(store, "disconnect_off_face", "Release", true);
    cg::test::check(
        r,
        off_site.has_value() && off_site->resolved
            && off_site->candidates.find("static_final_unique")
                   != std::string::npos,
        "the guarded site resolves via static_final_unique");
    cg::test::check(
        r,
        off_site.has_value()
            && off_site->candidates.find("out_param_family")
                   == std::string::npos,
        "the off-face union no longer feeds the resolution");

    // 对照臂：部分面内批（含声明类型）——守卫不抢班，站点留在动态
    // 带（多候选 0.4 披露 + 生命周期可达收窄，候选含声明类型），绝不
    // 被静态直连替换。
    const DEdge* conv =
        find_edge(edges, "disconnect_mix_face", "AcmeSessionObj::Release");
    cg::test::check(
        r,
        conv != nullptr && conv->confidence == 0.4,
        "an in-face dynamic band keeps its disclosed channel (0.4)");
    const auto mix_site =
        site_for(store, "disconnect_mix_face", "Release", true);
    cg::test::check(
        r,
        mix_site.has_value()
            && mix_site->candidates.find("static_final_unique")
                   == std::string::npos
            && mix_site->candidates.find("out_param_family")
                   != std::string::npos,
        "the partially in-face site stays on the dynamic band");
}

const cg::test::Registrar reg_static_face_priority{
    "analysis.dispatch.static_face_priority",
    &static_face_priority};

/**
 * @brief call_sites.qualified 的限定判据行为锚点。
 *
 * @internal 夹具 qualified-ast：普通调用的限定判定走引用名范围差
 * ∪ 被调子式 NamespaceRef 子游标——宏展开错位形态（extent 两端
 * spelling 落在宏定义与调用行、跨度超 token 卫生闸上限）不再因闸
 * 拒绝而漏报，显式普通类型模板实参（产出 TypeRef 子游标但无限定
 * 符）不误报；构造调用保持 token 判据（限定符挂在外层
 * CXXFunctionalCastExpr，内层构造表达式不可达）。每形态一个独立
 * caller，断言按 (caller, callee, is_virtual) 定位。
 */
void qualified_call_ast_criterion(R& r)
{
    db::GraphStore store{":memory:"};
    (void)index_fixture(store, "qualified-ast");
    struct Want
    {
        std::string_view caller;
        std::string_view callee;
        bool             virt;
        bool             qualified;
    };
    const Want wants[] = {
        {"a_qual_member", "vstep", true, true},
        {"b_unqual_member", "vstep", true, false},
        {"c_qual_ns", "make", false, true},
        {"d_qual_global", "vfree", false, true},
        {"e_plain", "vfree", false, false},
        {"f_arg_qual", "vstep", true, false},
        {"f_arg_qual", "make", false, true},
        {"g_macro_qual", "vstep", true, true},
        {"h_tmpl_ns_arg", "vqual_t", false, true},
        {"i_tmpl_plain_arg", "vqual_t", false, false},
        {"j_ctor_qual", "T", false, true},
        {"k_ctor_plain", "PlainS", false, false},
    };
    for (const Want& w : wants)
    {
        const auto site = site_for(store, w.caller, w.callee, w.virt);
        cg::test::check(
            r,
            site.has_value() && site->qualified == w.qualified,
            std::string{w.caller} + " -> " + std::string{w.callee}
                + " qualified expected " + (w.qualified ? "1" : "0"));
    }
}

const cg::test::Registrar reg_qualified_call_ast{
    "analysis.call_site.qualified_ast_criterion",
    &qualified_call_ast_criterion};
