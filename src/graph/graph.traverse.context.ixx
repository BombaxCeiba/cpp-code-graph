/**
 * @module cg.graph.traverse.context
 *
 * @brief 上下文走查：入边/出边推送与站点排序辅助、context 主函数。
 */
export module cg.graph.traverse.context;

import std;
import cg.util;
import cg.db;
import cg.graph.traverse.model;
import cg.graph.traverse.dispatchidx;
import cg.graph.traverse.queries;

export namespace cg::graph
{

    namespace detail
    {

        /**
         * @brief 入边物化：对端符号（src）查库后连同边行与调用点给定的
         *        证据状态推入 out.incoming。
         *
         * @internal 状态推导（随模式不同）留在各调用点。
         */
        auto context_push_incoming(
            cg::db::GraphStore&    store,
            ContextView&           out,
            const cg::db::EdgeRow& e,
            cg::db::SymId          origin,
            DispatchStatus         status) -> void
        {
            if (auto other = cg::db::get_symbol(store, e.src))
            {
                out.incoming.push_back(
                    {.other = std::move(*other),
                     .edge = e,
                     .family_origin = origin,
                     .dispatch_status = status});
            }
        }

        /**
         * @brief 出边物化：对端符号（dst）查库后连同边行与调用点给定的
         *        证据状态推入 out.outgoing。
         *
         * @internal 状态推导（随模式不同）留在各调用点。
         */
        auto context_push_outgoing(
            cg::db::GraphStore&    store,
            ContextView&           out,
            const cg::db::EdgeRow& e,
            cg::db::SymId          origin,
            DispatchStatus         status) -> void
        {
            if (auto other = cg::db::get_symbol(store, e.dst))
            {
                out.outgoing.push_back(
                    {.other = std::move(*other),
                     .edge = e,
                     .family_origin = origin,
                     .dispatch_status = status});
            }
        }

        /**
         * @brief 派发行集的公共排序：首键入边取 src、出边取 dst，站点坐标
         *        （文件，行）作次键与末键。
         */
        [[nodiscard]]
        auto dispatch_rows_site_less(
            const cg::db::EdgeRow& a,
            const cg::db::EdgeRow& b,
            bool                   by_dst) -> bool
        {
            const cg::db::SymId ka = by_dst ? a.dst : a.src;
            const cg::db::SymId kb = by_dst ? b.dst : b.src;
            if (ka != kb)
            {
                return ka < kb;
            }
            if (a.site_file != b.site_file)
            {
                return a.site_file < b.site_file;
            }
            return a.site_line < b.site_line;
        }

    } // namespace detail

    /**
     * @brief 360 度视图：符号本身加全部出入边，对端已物化（按类型分组是
     *        渲染器的职责）。
     *
     * merge_family（默认开）时并入覆盖族的边，每行标注贡献它的成员。
     *
     * @internal
     * 类型主序、成员次序（锚点在前）：边按类型分组到达，与单符号视图
     * 一致，按类型分桶的渲染器得以每类型保有一桶、锚点自己的行打头。
     */
    [[nodiscard]]
    inline auto context(
        cg::db::GraphStore& store,
        cg::db::SymId       sym,
        bool                merge_family = true,
        DispatchMode        mode = DispatchMode::Precise) -> ContextView
    {
        ContextView out;
        out.dispatch_mode = mode;
        if (auto row = cg::db::get_symbol(store, sym))
        {
            out.symbol = std::move(*row);
        }
        const std::vector<cg::db::SymId> family =
            merge_family ? override_family(store, sym)
                         : std::vector<cg::db::SymId>{sym};
        out.family = family;
        std::vector<cg::db::SymId> members{sym};
        members.reserve(family.size());
        for (const cg::db::SymId m : family)
        {
            if (m != sym)
            {
                members.push_back(m);
            }
        }
        // 入/出边的共享物化收在 detail::context_push_incoming/outgoing；
        // 状态推导（随模式不同）留在各调用点。
        if (mode == DispatchMode::Off || mode == DispatchMode::Additive)
        {
            const bool include_dispatch = mode == DispatchMode::Additive;
            if (include_dispatch)
            {
                // Additive 旧模式不构造索引（vd 边直读库），标注从同一 meta
                // 取。
                out.dispatch_stale_files = detail::dispatch_stale_count(store);
            }
            for (int t = 0; t < cg::db::edge_type_count; ++t)
            {
                const auto type = cg::db::edge_type_of(t);
                if (!include_dispatch
                    && type == cg::db::EdgeType::VirtualDispatch)
                {
                    continue;
                }
                for (const cg::db::SymId member : members)
                {
                    const cg::db::SymId origin = member == sym ? 0 : member;
                    for (const cg::db::EdgeRow& e : cg::db::edges_of(
                             store,
                             member,
                             /*outgoing=*/false,
                             type))
                    {
                        DispatchStatus status = DispatchStatus::None;
                        if (type == cg::db::EdgeType::VirtualDispatch)
                        {
                            status = e.confidence == 0.0
                                         ? DispatchStatus::FamilyEstimate
                                         : DispatchStatus::Inferred;
                        }
                        else if (origin != 0)
                        {
                            status = DispatchStatus::FamilyEstimate;
                        }
                        detail::context_push_incoming(
                            store,
                            out,
                            e,
                            origin,
                            status);
                    }
                    for (const cg::db::EdgeRow& e : cg::db::edges_of(
                             store,
                             member,
                             /*outgoing=*/true,
                             type))
                    {
                        DispatchStatus status = DispatchStatus::None;
                        if (type == cg::db::EdgeType::VirtualDispatch)
                        {
                            status = e.confidence == 0.0
                                         ? DispatchStatus::FamilyEstimate
                                         : DispatchStatus::Inferred;
                        }
                        else if (origin != 0)
                        {
                            status = DispatchStatus::FamilyEstimate;
                        }
                        detail::context_push_outgoing(
                            store,
                            out,
                            e,
                            origin,
                            status);
                    }
                }
            }
            return out;
        }

        // Precise / Fallback。
        detail::DispatchIndex dispatch{store};
        out.dispatch_stale_files = dispatch.stale_files();
        // 派发行集的公共排序收在 detail::dispatch_rows_site_less。
        for (int t = 0; t < cg::db::edge_type_count; ++t)
        {
            const auto type = cg::db::edge_type_of(t);
            if (type == cg::db::EdgeType::VirtualDispatch)
            {
                for (const cg::db::SymId member : members)
                {
                    const cg::db::SymId origin = member == sym ? 0 : member;
                    const bool          include_incoming =
                        dispatch.in_impl_set(sym, member);
                    std::vector<cg::db::EdgeRow> in_rows = cg::db::edges_of(
                        store,
                        member,
                        /*outgoing=*/false,
                        type);
                    std::ranges::sort(
                        in_rows,
                        [&](const cg::db::EdgeRow& a, const cg::db::EdgeRow& b)
                        {
                            return detail::dispatch_rows_site_less(
                                a,
                                b,
                                /*by_dst=*/false);
                        });
                    for (const cg::db::EdgeRow& e : in_rows)
                    {
                        if (e.confidence >= kDispatchConfidenceFloor)
                        {
                            if (include_incoming)
                            {
                                detail::context_push_incoming(
                                    store,
                                    out,
                                    e,
                                    origin,
                                    DispatchStatus::Inferred);
                            }
                        }
                        else if (
                            mode == DispatchMode::Fallback && include_incoming)
                        {
                            detail::context_push_incoming(
                                store,
                                out,
                                e,
                                origin,
                                DispatchStatus::FamilyEstimate);
                        }
                    }
                    std::vector<cg::db::EdgeRow> out_rows = cg::db::edges_of(
                        store,
                        member,
                        /*outgoing=*/true,
                        type);
                    std::ranges::sort(
                        out_rows,
                        [&](const cg::db::EdgeRow& a, const cg::db::EdgeRow& b)
                        {
                            return detail::dispatch_rows_site_less(
                                a,
                                b,
                                /*by_dst=*/true);
                        });
                    for (const cg::db::EdgeRow& e : out_rows)
                    {
                        if (e.confidence >= kDispatchConfidenceFloor)
                        {
                            detail::context_push_outgoing(
                                store,
                                out,
                                e,
                                origin,
                                DispatchStatus::Inferred);
                        }
                        else if (mode == DispatchMode::Fallback)
                        {
                            detail::context_push_outgoing(
                                store,
                                out,
                                e,
                                origin,
                                DispatchStatus::FamilyEstimate);
                        }
                    }
                }
                continue;
            }
            for (const cg::db::SymId member : members)
            {
                const cg::db::SymId  origin = member == sym ? 0 : member;
                const DispatchStatus status =
                    origin == 0 ? DispatchStatus::None
                                : DispatchStatus::FamilyEstimate;
                for (const cg::db::EdgeRow& e :
                     cg::db::edges_of(store, member, /*outgoing=*/false, type))
                {
                    if (type == cg::db::EdgeType::Calls
                        && dispatch.suppresses_calls(e))
                    {
                        continue;
                    }
                    detail::context_push_incoming(
                        store,
                        out,
                        e,
                        origin,
                        status);
                }
                for (const cg::db::EdgeRow& e :
                     cg::db::edges_of(store, member, /*outgoing=*/true, type))
                {
                    if (type == cg::db::EdgeType::Calls
                        && dispatch.suppresses_calls(e))
                    {
                        continue;
                    }
                    detail::context_push_outgoing(
                        store,
                        out,
                        e,
                        origin,
                        status);
                }
            }
        }
        return out;
    }
} // namespace cg::graph
