/**
 * @module cg.graph.traverse.walk
 *
 * @brief
 * 走查引擎：记录字段用途采集、前沿预取（raw/agg）、贡献合成与风险分级、精确/常规两层
 * BFS。
 */
export module cg.graph.traverse.walk;

import std;
import cg.util;
import cg.db;
import cg.graph.traverse.model;
import cg.graph.traverse.dispatchidx;

export namespace cg::graph
{

    namespace detail
    {

        /** @internal 记录展开段的公共产物（两类预取容器将其搬入自己的
         * fields/field_uses 成员）。 */
        struct RecordFieldUses
        {
            std::unordered_map<cg::db::SymId, std::vector<cg::db::SymId>>
                fields;
            std::unordered_map<cg::db::SymId, std::vector<cg::db::EdgeAggRow>>
                field_uses;
        };

        /**
         * @internal 记录展开：frontier 中的 Record 补上其 FIELD 子成员的
         * USES 边。记录使用的记录挂在其字段成员上（字段拥有 USES 边；记录
         * 与成员的联系是 symbols.parent_id 列而非边），向下影响面对该联系
         * 扩展一跳、仅取 USES 边，使"该记录依赖什么"涵盖其布局（原版
         * get_symbol+members+每字段一次 edges_from 的批量化）。fields 的
         * 段内序 = members 返回序，field_uses 的段内序 = edges_from 逐符号
         * 版返回序。
         */
        [[nodiscard]]
        inline auto collect_record_field_uses(
            cg::db::GraphStore&               store,
            const std::vector<cg::db::SymId>& frontier) -> RecordFieldUses
        {
            RecordFieldUses out;
            std::vector<cg::db::SymId>
                records; // frontier 序（briefs 行序=输入序）
            for (const cg::db::SymbolBrief& b :
                 cg::db::symbol_briefs(store, frontier))
            {
                if (b.kind == cg::db::SymbolKind::Record)
                {
                    records.push_back(b.id);
                }
            }
            std::vector<cg::db::SymId> all_fields; // records 序 × members 序
            for (const cg::db::MemberKindRow& m :
                 cg::db::member_kinds_batch(store, records))
            {
                if (m.kind == cg::db::SymbolKind::Field)
                {
                    out.fields[m.parent].push_back(m.id);
                    all_fields.push_back(m.id);
                }
            }
            for (cg::db::EdgeAggBySym& r :
                 cg::db::edges_from_batch(store, all_fields, USES_TYPES))
            {
                out.field_uses[r.self].push_back(std::move(r.row));
            }
            return out;
        }

        /**
         * @internal 一层前沿的原始边预取。
         *
         * BFS 的逐节点逐类型查询折叠为每类型一整批 IN 查询：of_by_type 与
         * types 数组同下标对应，段内行序与逐节点版 edges_of 的索引自然序逐位
         * 一致（批量函数内已显式排序钉死）。fields/field_uses 的记录展开段
         * 由 collect_record_field_uses 填充（段内序契约同其说明）。
         */
        struct FrontierRaw
        {
            std::vector<
                std::unordered_map<cg::db::SymId, std::vector<cg::db::EdgeRow>>>
                of_by_type;
            std::unordered_map<cg::db::SymId, std::vector<cg::db::SymId>>
                fields;
            std::unordered_map<cg::db::SymId, std::vector<cg::db::EdgeAggRow>>
                field_uses;
        };

        /**
         * @internal 预取一层前沿的静态边。
         *
         * @param types 边类型数组（段序 = 原逐节点外层循环的类型顺序）。
         * @param expand_records 记录展开（bfs_precise 的 Down+impact 形态）。
         */
        [[nodiscard]]
        inline auto prefetch_raw(
            cg::db::GraphStore&               store,
            const std::vector<cg::db::SymId>& frontier,
            bool                              outgoing,
            std::span<const cg::db::EdgeType> types,
            bool                              expand_records) -> FrontierRaw
        {
            FrontierRaw pre;
            pre.of_by_type.resize(types.size());
            for (std::size_t ti = 0; ti < types.size(); ++ti)
            {
                for (cg::db::EdgeRowBySym& r : cg::db::edges_of_batch(
                         store,
                         frontier,
                         outgoing,
                         types[ti]))
                {
                    pre.of_by_type[ti][r.self].push_back(std::move(r.row));
                }
            }
            if (expand_records)
            {
                RecordFieldUses rec =
                    collect_record_field_uses(store, frontier);
                pre.fields = std::move(rec.fields);
                pre.field_uses = std::move(rec.field_uses);
            }
            return pre;
        }

        /**
         * @internal 一层前沿的聚合边预取：Additive/Off 走查的单步邻接
         * 批量化。steps 的段内序与 edges_to/from 逐符号版一致；
         * fields/field_uses 同 FrontierRaw 的记录展开段。
         */
        struct FrontierAgg
        {
            std::unordered_map<cg::db::SymId, std::vector<cg::db::EdgeAggRow>>
                steps;
            std::unordered_map<cg::db::SymId, std::vector<cg::db::SymId>>
                fields;
            std::unordered_map<cg::db::SymId, std::vector<cg::db::EdgeAggRow>>
                field_uses;
        };

        /**
         * @internal 预取一层前沿的聚合邻接。types 与 include_dispatch
         * 决定等价边类型集；expand_records = Down+impact 形态。
         */
        [[nodiscard]]
        inline auto prefetch_agg(
            cg::db::GraphStore&               store,
            const std::vector<cg::db::SymId>& frontier,
            Direction                         dir,
            bool                              impact_mode,
            bool                              include_dispatch,
            bool                              expand_records) -> FrontierAgg
        {
            const std::span<const cg::db::EdgeType> types =
                impact_mode
                    ? (include_dispatch
                           ? std::span<
                                 const cg::db::EdgeType>{IMPACT_DISPATCH_TYPES}
                           : std::span<const cg::db::EdgeType>{IMPACT_TYPES})
                    : (include_dispatch
                           ? std::span<const cg::db::EdgeType>{DISPATCH_TYPES}
                           : std::span<const cg::db::EdgeType>{CALLS_TYPES});
            FrontierAgg pre;
            auto rows = dir == Direction::Up
                            ? cg::db::edges_to_batch(store, frontier, types)
                            : cg::db::edges_from_batch(store, frontier, types);
            for (cg::db::EdgeAggBySym& r : rows)
            {
                pre.steps[r.self].push_back(std::move(r.row));
            }
            if (expand_records)
            {
                RecordFieldUses rec =
                    collect_record_field_uses(store, frontier);
                pre.fields = std::move(rec.fields);
                pre.field_uses = std::move(rec.field_uses);
            }
            return pre;
        }

        /** @internal 状态优先级：证明 > 直接静态 > 家族估算。 */
        [[nodiscard]]
        inline auto status_rank(DispatchStatus s) -> int
        {
            switch (s)
            {
            case DispatchStatus::Inferred:
                return 0;
            case DispatchStatus::None:
                return 1;
            case DispatchStatus::FamilyEstimate:
                return 2;
            }
            return 3;
        }

        /** @internal 同深度内对同一对端的一条候选关系。 */
        struct WalkContribution
        {
            cg::db::SymId    other = 0;
            cg::db::EdgeType via = cg::db::EdgeType::Calls;
            cg::db::SymId    via_id = 0; // 展开来源（当前 frontier 符号）
            cg::db::SymId    origin = 0; // 链途经的种子
            std::int64_t     occurrences = 0;
            DispatchStatus   status = DispatchStatus::None;
            double           confidence = 1.0;
            double           dispatch_confidence = 0.0;
            std::uint32_t    attrs = 0;
            bool             has_dispatch = false;
        };

        /** @internal 同状态候选的显式决胜：置信度、展开来源、边型。 */
        [[nodiscard]]
        inline auto contribution_preferred(
            const WalkContribution& a,
            const WalkContribution& b) -> bool
        {
            if (a.confidence != b.confidence)
            {
                return a.confidence > b.confidence;
            }
            if (a.via_id != b.via_id)
            {
                return a.via_id < b.via_id;
            }
            return static_cast<int>(a.via) < static_cast<int>(b.via);
        }

        /** @internal 把候选并入当前深度的最佳关系表。 */
        inline void consider_contribution(
            std::map<cg::db::SymId, WalkContribution>& best,
            WalkContribution                           c)
        {
            const auto it = best.find(c.other);
            if (it == best.end())
            {
                best.emplace(c.other, c);
                return;
            }
            WalkContribution& cur = it->second;
            if (status_rank(c.status) < status_rank(cur.status))
            {
                cur = c;
                return;
            }
            if (status_rank(c.status) == status_rank(cur.status))
            {
                const std::int64_t total = cur.occurrences + c.occurrences;
                if (contribution_preferred(c, cur))
                {
                    cur = c;
                }
                cur.occurrences = total;
            }
        }

        /**
         * @internal 风险阶梯：score > 120 为 Critical、> 30 为 High、
         * > 8 为 Medium、其余 Low（impact 汇总与 changes 的逐消费者/汇总
         * 共用同一阶梯）。Unknown 前置条件（无直接命中）由调用方判定。
         */
        [[nodiscard]]
        inline auto risk_from_score(std::int64_t score) -> Risk
        {
            if (score > 120)
            {
                return Risk::Critical;
            }
            if (score > 30)
            {
                return Risk::High;
            }
            if (score > 8)
            {
                return Risk::Medium;
            }
            return Risk::Low;
        }

        /**
         * @brief B 精确模式的深度受限 BFS。
         *
         * @internal
         * - 静态 CALLS 边按站点与 `impl_set` 守卫替换（抑制）；
         * - confidence >= 0.7 的 virtual_dispatch
         * 边是**一等**调用关系（不是只用来 过滤），保证“站点目标不在任何 CALLS
         * 家族内”也不漏；
         * - Up 深度 1 的派发目标按方向性 `impl_set(anchor)` 归属：查基类保留
         *   全部 override 调用者，查具体实现不拉入兄弟；
         * - 同一深度按对端合并候选，状态优先级显式确定，绝不依赖 SQLite 顺序。
         */
        [[nodiscard]]
        inline auto bfs_precise(
            cg::db::GraphStore&               store,
            const std::vector<cg::db::SymId>& seeds,
            cg::db::SymId                     anchor,
            Direction                         dir,
            bool                              impact_mode,
            int                               max_depth,
            DispatchMode                      mode,
            const DispatchIndex&              dispatch) -> NeighborsResult
        {
            NeighborsResult out;
            out.family = seeds;
            out.dispatch_mode = mode;
            std::unordered_map<cg::db::SymId, NeighborEntry> reached;
            std::unordered_map<cg::db::SymId, cg::db::SymId> origin;
            std::vector<cg::db::SymId>                       frontier;
            std::unordered_set<cg::db::SymId>                visited;
            frontier.reserve(seeds.size());
            for (const cg::db::SymId s : seeds)
            {
                if (visited.insert(s).second)
                {
                    origin.emplace(s, s);
                    frontier.push_back(s);
                }
            }
            std::ranges::sort(frontier);
            const bool outgoing = dir == Direction::Down;
            const std::span<const cg::db::EdgeType> types =
                impact_mode ? std::span<const cg::db::EdgeType>{IMPACT_TYPES}
                            : std::span<const cg::db::EdgeType>{CALLS_TYPES};
            for (int depth = 1; depth <= max_depth && !frontier.empty();
                 ++depth)
            {
                std::map<cg::db::SymId, WalkContribution> best;
                // 整层静态边一次预取（每类型一批 IN + 记录展开批量），
                // 段序与行序逐字复刻逐节点版（见 prefetch_raw）。
                const FrontierRaw pre = prefetch_raw(
                    store,
                    frontier,
                    outgoing,
                    types,
                    /*expand_records=*/outgoing && impact_mode);
                for (const cg::db::SymId cur : frontier)
                {
                    const cg::db::SymId  cur_origin = origin.at(cur);
                    const DispatchStatus family_status =
                        cur_origin == anchor ? DispatchStatus::None
                                             : DispatchStatus::FamilyEstimate;
                    for (std::size_t ti = 0; ti < types.size(); ++ti)
                    {
                        const cg::db::EdgeType type = types[ti];
                        const auto found = pre.of_by_type[ti].find(cur);
                        if (found == pre.of_by_type[ti].end())
                        {
                            continue; // 逐节点版的空结果：循环体零次
                        }
                        for (const cg::db::EdgeRow& e : found->second)
                        {
                            if (type == cg::db::EdgeType::Calls
                                && dispatch.suppresses_calls(e))
                            {
                                continue;
                            }
                            consider_contribution(
                                best,
                                {.other = outgoing ? e.dst : e.src,
                                 .via = type,
                                 .via_id = cur,
                                 .origin = cur_origin,
                                 .occurrences = e.occurrences,
                                 .status = family_status,
                                 .confidence = e.confidence,
                                 .attrs = e.attrs});
                        }
                    }
                    if (const auto fields_it = pre.fields.find(cur);
                        fields_it != pre.fields.end())
                    {
                        for (const cg::db::SymId field : fields_it->second)
                        {
                            const auto uses_it = pre.field_uses.find(field);
                            if (uses_it == pre.field_uses.end())
                            {
                                continue;
                            }
                            for (const cg::db::EdgeAggRow& e : uses_it->second)
                            {
                                consider_contribution(
                                    best,
                                    {.other = e.other,
                                     .via = cg::db::EdgeType::Uses,
                                     .via_id = cur,
                                     .origin = cur_origin,
                                     .occurrences = e.occurrences_sum,
                                     .status = family_status,
                                     .confidence = e.confidence,
                                     .attrs = e.attrs});
                            }
                        }
                    }
                    // 正面派发边是一等关系。Up 深度 1 只保留属于
                    // impl_set(anchor) 的家族目标（方向性精确归属）。
                    const bool include_dispatch_here =
                        outgoing || depth > 1
                        || dispatch.in_impl_set(anchor, cur);
                    const std::span<const std::size_t> dispatch_list =
                        outgoing ? dispatch.outgoing(cur)
                                 : dispatch.incoming(cur);
                    for (const std::size_t idx : dispatch_list)
                    {
                        const DispatchEdgeRow& d = dispatch.edge(idx);
                        if (d.confidence < kDispatchConfidenceFloor)
                        {
                            continue;
                        }
                        if (!include_dispatch_here)
                        {
                            continue;
                        }
                        consider_contribution(
                            best,
                            {.other = outgoing ? d.dst : d.src,
                             .via = cg::db::EdgeType::VirtualDispatch,
                             .via_id = cur,
                             .origin = cur_origin,
                             .occurrences = 1,
                             .status = DispatchStatus::Inferred,
                             .confidence = d.confidence,
                             .dispatch_confidence = d.confidence,
                             .attrs = d.attrs,
                             .has_dispatch = true});
                    }
                    // Fallback 只给“已经由静态关系到达”的条目加家族估算标注
                    // （0.4 弱推断与 0.0 标记），绝不把它们当成新的调用关系。
                    if (mode == DispatchMode::Fallback)
                    {
                        for (const std::size_t idx : dispatch_list)
                        {
                            const DispatchEdgeRow& d = dispatch.edge(idx);
                            if (d.confidence >= kDispatchConfidenceFloor
                                || !include_dispatch_here)
                            {
                                continue;
                            }
                            const cg::db::SymId other =
                                outgoing ? d.dst : d.src;
                            if (const auto it = best.find(other);
                                it != best.end()
                                && it->second.status == DispatchStatus::None)
                            {
                                it->second.status =
                                    DispatchStatus::FamilyEstimate;
                            }
                        }
                    }
                }
                std::vector<cg::db::SymId> next;
                next.reserve(best.size());
                for (auto& [other, c] : best)
                {
                    if (!visited.insert(other).second)
                    {
                        continue;
                    }
                    NeighborEntry entry{
                        .id = other,
                        .depth = depth,
                        .via = c.via,
                        .via_id = c.via_id,
                        .occurrences = c.occurrences,
                        .family_origin = c.origin == anchor ? 0 : c.origin,
                        .confidence = c.confidence,
                        .attrs = c.attrs,
                        .has_dispatch = c.has_dispatch,
                        .dispatch_confidence = c.dispatch_confidence,
                        .dispatch_attrs = c.attrs,
                        .dispatch_status = c.status};
                    reached.emplace(other, entry);
                    origin.emplace(other, c.origin);
                    next.push_back(other);
                }
                frontier = std::move(next);
                out.truncated = !frontier.empty();
            }
            out.entries.reserve(reached.size());
            for (auto& [id, entry] : reached)
            {
                (void)id;
                out.entries.push_back(entry);
            }
            std::ranges::sort(
                out.entries,
                [](const NeighborEntry& a, const NeighborEntry& b)
                {
                    return a.depth != b.depth ? a.depth < b.depth : a.id < b.id;
                });
            return out;
        }

        /**
         * @brief 种子集（覆盖族，或仅被查询符号）之上的通用深度受限 BFS。
         *
         * 返回以最短深度到达的符号；上限截停仍在生长的边界时 truncated=true。
         *
         * @internal
         * 种子绝不作为条目出现（家族成员是查询的一个侧面而非结果）；每个条目
         * 携带其链途经的种子（family_origin，锚点本身置零）。
         */
        [[nodiscard]]
        inline auto bfs(
            cg::db::GraphStore&               store,
            const std::vector<cg::db::SymId>& seeds,
            cg::db::SymId                     anchor,
            Direction                         dir,
            bool                              impact_mode,
            int                               max_depth,
            DispatchMode                      mode = DispatchMode::Precise,
            const DispatchIndex* dispatch_index = nullptr) -> NeighborsResult
        {
            if (mode == DispatchMode::Precise || mode == DispatchMode::Fallback)
            {
                std::optional<DispatchIndex> owned;
                if (dispatch_index == nullptr)
                {
                    owned.emplace(store);
                    dispatch_index = &*owned;
                }
                NeighborsResult out = bfs_precise(
                    store,
                    seeds,
                    anchor,
                    dir,
                    impact_mode,
                    max_depth,
                    mode,
                    *dispatch_index);
                out.dispatch_stale_files = dispatch_index->stale_files();
                return out;
            }
            const bool include_dispatch = mode == DispatchMode::Additive;
            // Additive 旧模式不构造索引（vd 边直读库），标注从同一 meta 取。
            NeighborsResult out;
            out.family = seeds;
            out.dispatch_mode = mode;
            out.dispatch_stale_files = dispatch_stale_count(store);
            std::unordered_map<cg::db::SymId, NeighborEntry> reached;
            std::unordered_map<cg::db::SymId, cg::db::SymId> origin;
            std::vector<cg::db::SymId>                       frontier;
            std::unordered_set<cg::db::SymId>                visited;
            frontier.reserve(seeds.size());
            for (const cg::db::SymId s : seeds)
            {
                visited.insert(s);
                origin.emplace(s, s);
                frontier.push_back(s);
            }
            for (int depth = 1; depth <= max_depth && !frontier.empty();
                 ++depth)
            {
                std::vector<cg::db::SymId> next;
                // 整层邻接一次预取（单步邻接的批量化）。
                const FrontierAgg pre = prefetch_agg(
                    store,
                    frontier,
                    dir,
                    impact_mode,
                    include_dispatch,
                    /*expand_records=*/dir == Direction::Down && impact_mode);
                for (const cg::db::SymId cur : frontier)
                {
                    // 单步邻接的行集重组：基础段（预取分段）+ 记录字段
                    // USES 追加 + 同一稳定排序，段序与逐节点版逐字一致。
                    std::vector<cg::db::EdgeAggRow> step;
                    if (const auto it = pre.steps.find(cur);
                        it != pre.steps.end())
                    {
                        step = it->second;
                    }
                    if (const auto fields_it = pre.fields.find(cur);
                        fields_it != pre.fields.end())
                    {
                        for (const cg::db::SymId field : fields_it->second)
                        {
                            if (const auto uses_it = pre.field_uses.find(field);
                                uses_it != pre.field_uses.end())
                            {
                                step.insert(
                                    step.end(),
                                    uses_it->second.begin(),
                                    uses_it->second.end());
                            }
                        }
                    }
                    if (include_dispatch)
                    {
                        // 同一 dst 的 CALLS 必须先于 virtual_dispatch 处理，BFS
                        // 才能把 dispatch 信息合并进已有条目而不是丢失。
                        std::ranges::stable_sort(
                            step,
                            [](const cg::db::EdgeAggRow& a,
                               const cg::db::EdgeAggRow& b)
                            {
                                return (a.type
                                        != cg::db::EdgeType::VirtualDispatch)
                                       && (b.type
                                           == cg::db::EdgeType::
                                               VirtualDispatch);
                            });
                    }
                    for (const cg::db::EdgeAggRow& e : step)
                    {
                        const bool is_dispatch =
                            e.type == cg::db::EdgeType::VirtualDispatch;
                        if (visited.insert(e.other).second)
                        {
                            const cg::db::SymId o = origin.at(cur);
                            NeighborEntry       entry{
                                .id = e.other,
                                .depth = depth,
                                .via = e.type,
                                .via_id = cur,
                                .occurrences = e.occurrences_sum,
                                .family_origin = o == anchor ? 0 : o,
                                .confidence = e.confidence,
                                .attrs = e.attrs};
                            if (is_dispatch)
                            {
                                entry.has_dispatch = true;
                                entry.dispatch_confidence = e.confidence;
                                entry.dispatch_attrs = e.attrs;
                                entry.dispatch_status =
                                    e.confidence == 0.0
                                        ? DispatchStatus::FamilyEstimate
                                        : DispatchStatus::Inferred;
                            }
                            reached.emplace(e.other, entry);
                            origin.emplace(e.other, o);
                            next.push_back(e.other);
                            continue;
                        }
                        // 同一对端已由 CALLS 先到达：把 dispatch 的置信度/属性
                        // 合并进既有条目，绝不因去重丢掉新信息。
                        if (is_dispatch)
                        {
                            const auto it = reached.find(e.other);
                            if (it != reached.end())
                            {
                                if (!it->second.has_dispatch)
                                {
                                    it->second.has_dispatch = true;
                                    it->second.dispatch_confidence =
                                        e.confidence;
                                    it->second.dispatch_attrs = e.attrs;
                                    it->second.dispatch_status =
                                        e.confidence == 0.0
                                            ? DispatchStatus::FamilyEstimate
                                            : DispatchStatus::Inferred;
                                }
                                else
                                {
                                    it->second.dispatch_confidence = std::max(
                                        it->second.dispatch_confidence,
                                        e.confidence);
                                    it->second.dispatch_attrs |= e.attrs;
                                }
                            }
                        }
                    }
                }
                frontier = std::move(next);
                out.truncated = !frontier.empty();
            }
            out.entries.reserve(reached.size());
            for (auto& [id, entry] : reached)
            {
                out.entries.push_back(entry);
            }
            std::ranges::sort(
                out.entries,
                [](const NeighborEntry& a, const NeighborEntry& b)
                {
                    return a.depth != b.depth ? a.depth < b.depth : a.id < b.id;
                });
            return out;
        }

    } // namespace detail
} // namespace cg::graph
