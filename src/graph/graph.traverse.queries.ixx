/**
 * @module cg.graph.traverse.queries
 *
 * @brief
 * 导出查询函数族：override_family、callers/callees、扇入、impact、trace、调用闭包、模块标签与模块图。
 */
export module cg.graph.traverse.queries;

import std;
import cg.util;
import cg.db;
import cg.graph.traverse.model;
import cg.graph.traverse.dispatchidx;
import cg.graph.traverse.walk;

export namespace cg::graph
{

    /**
     * @brief 方法的覆盖族：自身加上经 overrides 边连通的全部方法。
     *
     * 向上（它覆写的虚函数）与向下（覆写它的方法）皆传递；按 id 排序。
     * 非虚或无家族的符号仅返回 {sym}。
     *
     * @internal overrides 边方向为 派生 -> 基：出边到达它覆写的方法，
     * 入边到达覆写它的方法。
     */
    [[nodiscard]]
    inline auto override_family(cg::db::GraphStore& store, cg::db::SymId sym)
        -> std::vector<cg::db::SymId>
    {
        std::vector<cg::db::SymId>        family{sym};
        std::unordered_set<cg::db::SymId> visited{sym};
        std::vector<cg::db::SymId>        frontier{sym};
        while (!frontier.empty())
        {
            std::vector<cg::db::SymId> next;
            // 整层 overrides 出边与入边各一批 IN 查询（逐节点版的
            // 两条 edges_from/edges_to 折叠成两批）。
            const std::vector<cg::db::EdgeAggBySym> from_rows =
                cg::db::edges_from_batch(
                    store,
                    frontier,
                    detail::OVERRIDE_TYPES);
            const std::vector<cg::db::EdgeAggBySym> to_rows =
                cg::db::edges_to_batch(store, frontier, detail::OVERRIDE_TYPES);
            for (const cg::db::SymId cur : frontier)
            {
                for (const auto* rows : {&from_rows, &to_rows})
                {
                    const auto [begin, end] = std::ranges::equal_range(
                        *rows,
                        cur,
                        {},
                        &cg::db::EdgeAggBySym::self);
                    for (const auto& hit : std::ranges::subrange{begin, end})
                    {
                        const cg::db::EdgeAggRow& e = hit.row;
                        if (visited.insert(e.other).second)
                        {
                            family.push_back(e.other);
                            next.push_back(e.other);
                        }
                    }
                }
            }
            frontier = std::move(next);
        }
        std::ranges::sort(family);
        return family;
    }

    /**
     * @brief 符号的传递调用者。
     *
     * max_depth 1 = 直接；深度钳制到 [1, 10]。
     * @param store 图数据库句柄（符号与边的读取）。
     * @param sym 起点符号键（传递调用者从这里出发）。
     * @param max_depth 深度钳制（1 = 直接调用者；上限 10）。
     * @param mode 派发证据模式（Precise 默认；--no-dispatch
     * 时的字节等价视图）。
     * @param merge_family 默认开：虚方法的走查以其整个覆盖族为种子，使任何
     *                    覆写的派发点调用者也作为被查询方法的调用者浮现
     *                    （合并行携带 family_origin）；传 false 仅查询符号
     *                    本身。
     */
    [[nodiscard]]
    inline auto callers(
        cg::db::GraphStore& store,
        cg::db::SymId       sym,
        int                 max_depth = 1,
        bool                merge_family = true,
        DispatchMode        mode = DispatchMode::Precise) -> NeighborsResult
    {
        const std::vector<cg::db::SymId> seeds =
            merge_family ? override_family(store, sym)
                         : std::vector<cg::db::SymId>{sym};
        return detail::bfs(
            store,
            seeds,
            sym,
            Direction::Up,
            /*impact_mode=*/false,
            std::clamp(max_depth, 1, 10),
            mode);
    }

    /** @brief 符号的传递被调用者；参数语义同 callers（Direction::Down）。 */
    [[nodiscard]]
    inline auto callees(
        cg::db::GraphStore& store,
        cg::db::SymId       sym,
        int                 max_depth = 1,
        bool                merge_family = true,
        DispatchMode        mode = DispatchMode::Precise) -> NeighborsResult
    {
        const std::vector<cg::db::SymId> seeds =
            merge_family ? override_family(store, sym)
                         : std::vector<cg::db::SymId>{sym};
        return detail::bfs(
            store,
            seeds,
            sym,
            Direction::Down,
            /*impact_mode=*/false,
            std::clamp(max_depth, 1, 10),
            mode);
    }

    /**
     * @brief 一个符号在影响关系（calls、extends、uses）上的去重入边来源数：
     *        其扇入。
     *
     * 记录类型经字段、参数、返回值与子类化被引用而非被调用，三种关系
     * 均计入。
     */
    [[nodiscard]]
    inline auto fan_in_of(
        cg::db::GraphStore&          store,
        cg::db::SymId                id,
        const detail::DispatchIndex* dispatch = nullptr) -> std::int64_t
    {
        std::unordered_set<cg::db::SymId> sources;
        for (const cg::db::EdgeAggRow& e :
             cg::db::edges_to(store, id, detail::IMPACT_TYPES))
        {
            sources.insert(e.other);
        }
        // B 精确模式：正面派发入边是真实调用者，计入扇入；0.0 标记不计。
        if (dispatch != nullptr)
        {
            for (const std::size_t idx : dispatch->incoming(id))
            {
                const detail::DispatchEdgeRow& d = dispatch->edge(idx);
                if (d.confidence >= kDispatchConfidenceFloor)
                {
                    sources.insert(d.src);
                }
            }
        }
        return static_cast<std::int64_t>(sources.size());
    }

    /**
     * @brief 一批符号的扇入之和：fan_in_of 的批量化。
     *
     * @internal 每符号的去重来源集合构造与 fan_in_of 逐字相同（edges_to
     * 聚合一批 + 派发正面入边并入）；只取集合大小，无顺序依赖。
     */
    [[nodiscard]]
    inline auto fan_in_sum(
        cg::db::GraphStore&            store,
        std::span<const cg::db::SymId> ids,
        const detail::DispatchIndex*   dispatch = nullptr) -> std::int64_t
    {
        std::unordered_map<cg::db::SymId, std::unordered_set<cg::db::SymId>>
            sources;
        for (const cg::db::EdgeAggBySym& r :
             cg::db::edges_to_batch(store, ids, detail::IMPACT_TYPES))
        {
            sources[r.self].insert(r.row.other);
        }
        if (dispatch != nullptr)
        {
            for (const cg::db::SymId id : ids)
            {
                for (const std::size_t idx : dispatch->incoming(id))
                {
                    const detail::DispatchEdgeRow& d = dispatch->edge(idx);
                    if (d.confidence >= kDispatchConfidenceFloor)
                    {
                        sources[id].insert(d.src);
                    }
                }
            }
        }
        std::int64_t sum = 0;
        for (const cg::db::SymId id : ids)
        {
            if (const auto it = sources.find(id); it != sources.end())
            {
                sum += static_cast<std::int64_t>(it->second.size());
            }
        }
        return sum;
    }

    /**
     * @brief 影响面。
     *
     * up = 传递调用者、派生类型、声明类型引用本符号的符号（USES：字段、
     * 参数、返回值、局部变量、变量）；down = 传递被调用者、基类、使用的
     * 记录。虚方法在 merge_family（默认开）时以整个覆盖族为种子——得到的
     * 是抽象的影响面而非单一实现的影响面。风险阶梯：
     *
     *     score = 3*direct + transitive + fan_in
     *       direct     = 深度 1 的受影响数
     *       transitive = impacted - direct
     *       fan_in     = 直接受影响符号的去重入边（calls/extends/uses）
     *                    来源数之和
     *     LOW <= 8 < MEDIUM <= 30 < HIGH <= 120 < CRITICAL
     *
     * 直接影响为零的上向走查保持 UNKNOWN——没有边不是安全的证据。
     */
    [[nodiscard]]
    inline auto impact(
        cg::db::GraphStore& store,
        cg::db::SymId       sym,
        Direction           dir,
        int                 max_depth = 3,
        bool                merge_family = true,
        DispatchMode        mode = DispatchMode::Precise) -> ImpactResult
    {
        const int                        depth = std::clamp(max_depth, 1, 10);
        const std::vector<cg::db::SymId> seeds =
            merge_family ? override_family(store, sym)
                         : std::vector<cg::db::SymId>{sym};
        std::optional<detail::DispatchIndex> dispatch;
        const bool                           precise =
            mode == DispatchMode::Precise || mode == DispatchMode::Fallback;
        if (precise)
        {
            dispatch.emplace(store);
        }
        const NeighborsResult walked = detail::bfs(
            store,
            seeds,
            sym,
            dir,
            /*impact_mode=*/true,
            depth,
            mode,
            dispatch ? &*dispatch : nullptr);

        ImpactResult out;
        out.family = walked.family;
        out.truncated = walked.truncated;
        out.dispatch_mode = mode;
        out.dispatch_stale_files = walked.dispatch_stale_files;
        if (mode == DispatchMode::Additive)
        {
            for (const NeighborEntry& e : walked.entries)
            {
                if (e.has_dispatch)
                {
                    out.dispatch_confidence.emplace(
                        e.id,
                        e.dispatch_confidence);
                    out.dispatch_attrs.emplace(e.id, e.dispatch_attrs);
                    out.dispatch_status.emplace(e.id, e.dispatch_status);
                }
            }
        }
        else if (mode != DispatchMode::Off)
        {
            for (const NeighborEntry& e : walked.entries)
            {
                out.dispatch_status.emplace(e.id, e.dispatch_status);
                if (e.dispatch_status == DispatchStatus::Inferred)
                {
                    out.dispatch_confidence.emplace(
                        e.id,
                        e.dispatch_confidence);
                    out.dispatch_attrs.emplace(e.id, e.dispatch_attrs);
                }
            }
        }
        out.impacted = static_cast<std::int64_t>(walked.entries.size());
        int                        level = 1;
        std::vector<cg::db::SymId> ids;
        for (const NeighborEntry& e : walked.entries)
        {
            if (e.depth != level)
            {
                out.by_depth.push_back({.depth = level, .ids = std::move(ids)});
                ids.clear();
                level = e.depth;
            }
            ids.push_back(e.id);
            if (e.family_origin != 0)
            {
                out.family_of.emplace(e.id, e.family_origin);
            }
        }
        if (!ids.empty())
        {
            out.by_depth.push_back({.depth = level, .ids = std::move(ids)});
        }
        out.direct =
            out.by_depth.empty()
                ? 0
                : static_cast<std::int64_t>(out.by_depth.front().ids.size());
        out.transitive = out.impacted - out.direct;
        if (!out.by_depth.empty())
        {
            // 直接受影响者的扇入求和批量化（每符号一条 edges_to 折叠
            // 成一批）。
            out.fan_in += fan_in_sum(
                store,
                out.by_depth.front().ids,
                dispatch ? &*dispatch : nullptr);
        }
        out.risk_score = 3 * out.direct + out.transitive + out.fan_in;
        if (dir == Direction::Up && out.direct == 0)
        {
            out.risk = Risk::Unknown;
        }
        else
        {
            out.risk = detail::risk_from_score(out.risk_score);
        }
        return out;
    }

    /**
     * @brief from 到 to 的最短有向调用路径（仅 calls 边，前向）。
     *
     * max_depth 钳制到 [1, 10]。
     */
    [[nodiscard]]
    inline auto trace(
        cg::db::GraphStore& store,
        cg::db::SymId       from,
        cg::db::SymId       to,
        int                 max_depth = 10,
        DispatchMode        mode = DispatchMode::Precise) -> TraceResult
    {
        const int   depth = std::clamp(max_depth, 1, 10);
        TraceResult out;
        out.dispatch_mode = mode;
        if (from == to)
        {
            out.found = true;
            out.chain.push_back(from);
            if (mode != DispatchMode::Off)
            {
                out.hops.push_back({.id = from});
            }
            return out;
        }

        if (mode != DispatchMode::Off)
        {
            // B 精确寻路：正面派发边是一等邻接；0.0 标记永不参与寻路，
            // --dispatch-fallback 也不得伪造路径。
            detail::DispatchIndex dispatch{store};
            out.dispatch_stale_files = dispatch.stale_files();
            std::unordered_map<cg::db::SymId, cg::db::SymId> parent;
            std::unordered_map<cg::db::SymId, TraceHop>      parent_hop;
            std::vector<cg::db::SymId>                       frontier{from};
            std::unordered_set<cg::db::SymId>                visited{from};
            struct Candidate
            {
                cg::db::SymId    dst = 0;
                cg::db::EdgeType via = cg::db::EdgeType::Calls;
                DispatchStatus   status = DispatchStatus::None;
                double           confidence = 1.0;
                cg::db::FileId   site_file = 0;
                int              site_line = 0;
                bool             dispatch_first = false;
            };
            bool found = false;
            for (int d = 0; d < depth && !frontier.empty() && !found; ++d)
            {
                std::vector<cg::db::SymId> next;
                // 整层静态 CALLS 边一批 IN 查询（逐节点版折叠）。
                const std::vector<cg::db::EdgeRowBySym> calls_rows =
                    cg::db::edges_of_batch(
                        store,
                        frontier,
                        /*outgoing=*/true,
                        cg::db::EdgeType::Calls);
                for (const cg::db::SymId cur : frontier)
                {
                    std::vector<Candidate> candidates;
                    for (const std::size_t idx : dispatch.outgoing(cur))
                    {
                        const detail::DispatchEdgeRow& e = dispatch.edge(idx);
                        if (e.confidence < kDispatchConfidenceFloor)
                        {
                            continue;
                        }
                        candidates.push_back(
                            {.dst = e.dst,
                             .via = cg::db::EdgeType::VirtualDispatch,
                             .status = DispatchStatus::Inferred,
                             .confidence = e.confidence,
                             .site_file = e.site_file,
                             .site_line = e.site_line,
                             .dispatch_first = true});
                    }
                    {
                        const auto [begin, end] = std::ranges::equal_range(
                            calls_rows,
                            cur,
                            {},
                            &cg::db::EdgeRowBySym::self);
                        for (const auto& hit :
                             std::ranges::subrange{begin, end})
                        {
                            const cg::db::EdgeRow& e = hit.row;
                            if (dispatch.suppresses_calls(e))
                            {
                                continue;
                            }
                            candidates.push_back(
                                {.dst = e.dst,
                                 .via = cg::db::EdgeType::Calls,
                                 .status = DispatchStatus::None,
                                 .confidence = e.confidence,
                                 .site_file = e.site_file,
                                 .site_line = e.site_line});
                        }
                    }
                    std::ranges::sort(
                        candidates,
                        [](const Candidate& a, const Candidate& b)
                        {
                            if (a.dispatch_first != b.dispatch_first)
                            {
                                return a.dispatch_first;
                            }
                            if (a.dst != b.dst)
                            {
                                return a.dst < b.dst;
                            }
                            if (a.site_file != b.site_file)
                            {
                                return a.site_file < b.site_file;
                            }
                            return a.site_line < b.site_line;
                        });
                    for (const Candidate& c : candidates)
                    {
                        if (visited.contains(c.dst))
                        {
                            continue;
                        }
                        visited.insert(c.dst);
                        parent.emplace(c.dst, cur);
                        parent_hop.emplace(
                            c.dst,
                            TraceHop{
                                .id = c.dst,
                                .via = c.via,
                                .dispatch_status = c.status,
                                .confidence = c.confidence,
                                .site_file = c.site_file,
                                .site_line = c.site_line});
                        if (c.dst == to)
                        {
                            found = true;
                            break;
                        }
                        next.push_back(c.dst);
                    }
                    if (found)
                    {
                        break;
                    }
                }
                frontier = std::move(next);
            }
            if (!found)
            {
                out.truncated = !frontier.empty();
                return out;
            }
            for (cg::db::SymId cur = to;; cur = parent.at(cur))
            {
                out.chain.push_back(cur);
                if (cur == from)
                {
                    break;
                }
            }
            std::ranges::reverse(out.chain);
            out.hops.reserve(out.chain.size());
            out.hops.push_back({.id = from});
            for (std::size_t i = 1; i < out.chain.size(); ++i)
            {
                out.hops.push_back(parent_hop.at(out.chain[i]));
            }
            out.found = true;
            out.truncated = false;
            return out;
        }

        std::unordered_map<cg::db::SymId, cg::db::SymId> parent;
        std::vector<cg::db::SymId>                       frontier{from};
        std::unordered_set<cg::db::SymId>                visited{from};
        bool                                             found = false;
        for (int d = 0; d < depth && !frontier.empty() && !found; ++d)
        {
            std::vector<cg::db::SymId> next;
            // 整层出边聚合一批 IN 查询（逐节点版折叠）。
            const std::vector<cg::db::EdgeAggBySym> rows =
                cg::db::edges_from_batch(store, frontier, detail::CALLS_TYPES);
            for (const cg::db::SymId cur : frontier)
            {
                const auto [begin, end] = std::ranges::equal_range(
                    rows,
                    cur,
                    {},
                    &cg::db::EdgeAggBySym::self);
                for (const auto& hit : std::ranges::subrange{begin, end})
                {
                    const cg::db::EdgeAggRow& e = hit.row;
                    if (visited.contains(e.other))
                    {
                        continue;
                    }
                    visited.insert(e.other);
                    parent.emplace(e.other, cur);
                    if (e.other == to)
                    {
                        found = true;
                        break;
                    }
                    next.push_back(e.other);
                }
                if (found)
                {
                    break;
                }
            }
            frontier = std::move(next);
        }
        if (!found)
        {
            out.truncated = !frontier.empty();
            return out;
        }
        out.truncated = false;
        for (cg::db::SymId cur = to;; cur = parent.at(cur))
        {
            out.chain.push_back(cur);
            if (cur == from)
            {
                break;
            }
        }
        std::ranges::reverse(out.chain);
        out.found = true;
        return out;
    }

    /**
     * @brief 入口符号的调用闭包（ccg graph --symbol）。
     *
     * 沿 calls 出边做深度受限 BFS；闭包内两端的全部 calls 边聚合为
     * (src, dst) 行——BFS 树边之外的交叉边（先经别路到达的对端）同样保留，
     * 图是真实展开而非树。不合并覆盖族：图是单符号视角的忠实导出。
     * max_depth 钳制到 [1, 10]。
     */
    [[nodiscard]]
    inline auto call_closure(
        cg::db::GraphStore& store,
        cg::db::SymId       entry,
        int                 max_depth) -> CallClosure
    {
        // depth <= 0 = 不设上限：BFS 直到 frontier 为空（ccg graph --depth
        // 0）。
        const int   depth = max_depth <= 0 ? 0 : std::clamp(max_depth, 1, 10);
        const bool  unlimited = max_depth <= 0;
        CallClosure out;
        std::unordered_map<cg::db::SymId, int> level;
        level.emplace(entry, 0);
        std::vector<cg::db::SymId> frontier{entry};
        for (int d = 1; (unlimited || d <= depth) && !frontier.empty(); ++d)
        {
            std::vector<cg::db::SymId> next;
            for (const cg::db::SymId cur : frontier)
            {
                for (const cg::db::EdgeAggRow& e :
                     cg::db::edges_from(store, cur, detail::CALLS_TYPES))
                {
                    if (level.emplace(e.other, d).second)
                    {
                        next.push_back(e.other);
                    }
                }
            }
            frontier = std::move(next);
            out.truncated = !unlimited && !frontier.empty();
        }

        out.nodes.reserve(level.size());
        for (const auto& [id, d] : level)
        {
            out.nodes.push_back({.id = id, .depth = d});
        }
        std::ranges::sort(
            out.nodes,
            [](const CallNode& a, const CallNode& b)
            { return a.depth != b.depth ? a.depth < b.depth : a.id < b.id; });

        std::map<std::pair<cg::db::SymId, cg::db::SymId>, std::int64_t> agg;
        for (const auto& [id, d] : level)
        {
            for (const cg::db::EdgeAggRow& e :
                 cg::db::edges_from(store, id, detail::CALLS_TYPES))
            {
                if (level.contains(e.other))
                {
                    agg[{id, e.other}] += e.occurrences_sum;
                }
            }
        }
        out.edges.reserve(agg.size());
        for (const auto& [pair, occurrences] : agg)
        {
            out.edges.push_back(
                {.src = pair.first,
                 .dst = pair.second,
                 .occurrences = occurrences});
        }
        return out;
    }

    /**
     * @brief 模块标签：定义文件的名词干（去最后一个扩展名）。
     *
     * 现行布局下文件名即模块名（cli.commands.ixx -> cli.commands）；对任意
     * 项目它是"按定义文件聚合"的中性代理标签。
     */
    [[nodiscard]]
    inline auto module_label_of(const std::string& path) -> std::string
    {
        const std::string      generic = cg::util::norm_path(path);
        const std::string_view name = cg::util::path_basename(generic);
        const std::size_t      dot = name.find_last_of('.');
        return dot == std::string_view::npos || dot == 0
                   ? std::string{name}
                   : std::string{name.substr(0, dot)};
    }

    /**
     * @brief 模块级调用聚合图（ccg graph --modules）。
     *
     * 模块 = 符号定义文件的词干；符号级 calls 边先按 (src, dst) 去重、再按
     * 模块对计数为权重——同文件调用不出图。file_substring 非空时仅路径包含
     * 它的文件参与（收窄到自家代码、滤掉依赖缓存）。min_weight 丢弃权重
     * 不足的聚合边，模块节点集随之收缩为保留边的端点（无孤儿节点）；
     * edges_total 始终报告过滤前的聚合边总数（计数不受过滤影响）。
     */
    [[nodiscard]]
    inline auto module_graph(
        cg::db::GraphStore& store,
        std::string_view    file_substring,
        int                 min_weight) -> ModuleGraph
    {
        ModuleGraph                                     out;
        std::unordered_map<cg::db::FileId, std::string> module_of_file;
        for (const cg::db::FileRow& f : cg::db::all_files(store))
        {
            if (!file_substring.empty()
                && f.path.find(file_substring) == std::string::npos)
            {
                continue;
            }
            module_of_file.emplace(f.id, module_label_of(f.path));
        }

        std::unordered_map<cg::db::SymId, const std::string*> module_of_sym;
        for (const cg::db::IndexRow& row : cg::db::all_symbol_index_rows(store))
        {
            if (const auto it = module_of_file.find(row.file_id);
                it != module_of_file.end())
            {
                module_of_sym.emplace(row.id, &it->second);
            }
        }

        std::set<std::pair<cg::db::SymId, cg::db::SymId>> pairs;
        for (const cg::db::EdgeRow& e : cg::db::all_edges(store))
        {
            if (e.type == cg::db::EdgeType::Calls
                && module_of_sym.contains(e.src)
                && module_of_sym.contains(e.dst))
            {
                pairs.emplace(e.src, e.dst);
            }
        }
        std::map<std::pair<std::string, std::string>, std::int64_t> agg;
        for (const auto& [src, dst] : pairs)
        {
            const std::string& from = *module_of_sym.at(src);
            const std::string& to = *module_of_sym.at(dst);
            if (from != to)
            {
                agg[{from, to}] += 1;
            }
        }
        out.edges_total = static_cast<std::int64_t>(agg.size());

        std::set<std::string> kept_modules;
        for (const auto& [pair, weight] : agg)
        {
            if (weight < min_weight)
            {
                continue;
            }
            out.edges.push_back(
                {.from = pair.first, .to = pair.second, .weight = weight});
            kept_modules.insert(pair.first);
            kept_modules.insert(pair.second);
        }
        out.modules.assign(kept_modules.begin(), kept_modules.end());
        return out;
    }
} // namespace cg::graph
