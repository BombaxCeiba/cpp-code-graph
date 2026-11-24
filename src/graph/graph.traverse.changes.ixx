/**
 * @module cg.graph.traverse.changes
 *
 * @brief 变更面走查：changes 常规/精确两步、环节点与路径文本、include_cycles。
 */
export module cg.graph.traverse.changes;

import std;
import cg.util;
import cg.db;
import cg.graph.traverse.model;
import cg.graph.traverse.dispatchidx;
import cg.graph.traverse.walk;
import cg.graph.traverse.include;
import cg.graph.traverse.queries;

export namespace cg::graph
{

    namespace detail
    {

        /**
         * @brief 变更集传播的一步上游走查：calls/extends/uses 上的依赖者，
         *        加上双向的覆盖族。
         *
         * 修改虚方法的契约会波及它覆写的方法与全部覆写它的方法，故经任何
         * 家族成员的派发点调用者也作为消费者浮现。
         */
        [[nodiscard]]
        inline auto changes_step(cg::db::GraphStore& store, cg::db::SymId id)
            -> std::vector<cg::db::EdgeAggRow>
        {
            std::vector<cg::db::EdgeAggRow> out =
                cg::db::edges_to(store, id, CHANGES_UP_TYPES);
            for (const cg::db::EdgeAggRow& e :
                 cg::db::edges_from(store, id, OVERRIDE_TYPES))
            {
                out.push_back(e);
            }
            for (const cg::db::EdgeAggRow& e :
                 cg::db::edges_to(store, id, OVERRIDE_TYPES))
            {
                out.push_back(e);
            }
            return out;
        }

        /** @internal changes 里除 calls 外的静态上游类型（保持旧聚合语义）。 */
        inline constexpr cg::db::EdgeType CHANGES_SOFT_TYPES[] = {
            cg::db::EdgeType::Extends,
            cg::db::EdgeType::Uses};

        /**
         * @brief changes 的 B 精确一步：calls 按站点抑制 + 正面派发入边。
         *
         * @internal
         * - 静态 CALLS 边应用与 Up 走查相同的站点守卫；被替换的引用不再计入。
         * - 正面派发入边 C -> cur 是一等消费者关系（状态 inferred）；0.0 标记
         *   永不加入成员集合。
         * - override 双向跳仍保留契约传播，标 family estimate。
         */
        [[nodiscard]]
        inline auto changes_step_precise(
            cg::db::GraphStore&  store,
            cg::db::SymId        id,
            const DispatchIndex& dispatch) -> std::vector<WalkContribution>
        {
            std::vector<WalkContribution> out;
            for (const cg::db::EdgeRow& e : cg::db::edges_of(
                     store,
                     id,
                     /*outgoing=*/false,
                     cg::db::EdgeType::Calls))
            {
                if (dispatch.suppresses_calls(e))
                {
                    continue;
                }
                out.push_back(
                    {.other = e.src,
                     .via = cg::db::EdgeType::Calls,
                     .via_id = id,
                     .occurrences = e.occurrences,
                     .status = DispatchStatus::None,
                     .confidence = e.confidence,
                     .attrs = e.attrs});
            }
            for (const cg::db::EdgeAggRow& e :
                 cg::db::edges_to(store, id, CHANGES_SOFT_TYPES))
            {
                out.push_back(
                    {.other = e.other,
                     .via = e.type,
                     .via_id = id,
                     .occurrences = e.occurrences_sum,
                     .status = DispatchStatus::None,
                     .confidence = e.confidence,
                     .attrs = e.attrs});
            }
            for (const cg::db::EdgeAggRow& e :
                 cg::db::edges_from(store, id, OVERRIDE_TYPES))
            {
                out.push_back(
                    {.other = e.other,
                     .via = cg::db::EdgeType::Overrides,
                     .via_id = id,
                     .occurrences = e.occurrences_sum,
                     .status = DispatchStatus::FamilyEstimate,
                     .confidence = e.confidence,
                     .attrs = e.attrs});
            }
            for (const cg::db::EdgeAggRow& e :
                 cg::db::edges_to(store, id, OVERRIDE_TYPES))
            {
                out.push_back(
                    {.other = e.other,
                     .via = cg::db::EdgeType::Overrides,
                     .via_id = id,
                     .occurrences = e.occurrences_sum,
                     .status = DispatchStatus::FamilyEstimate,
                     .confidence = e.confidence,
                     .attrs = e.attrs});
            }
            for (const std::size_t idx : dispatch.incoming(id))
            {
                const DispatchEdgeRow& e = dispatch.edge(idx);
                if (e.confidence < kDispatchConfidenceFloor)
                {
                    continue;
                }
                out.push_back(
                    {.other = e.src,
                     .via = cg::db::EdgeType::VirtualDispatch,
                     .via_id = id,
                     .occurrences = 1,
                     .status = DispatchStatus::Inferred,
                     .confidence = e.confidence,
                     .dispatch_confidence = e.confidence,
                     .attrs = e.attrs,
                     .has_dispatch = true});
            }
            return out;
        }

        /**
         * @brief 环节点稠密索引：首次见到的文件追加进 ids 并登记
         *        index_of，此后直取既有编号。
         */
        auto cycle_node_of(
            std::vector<cg::db::FileId>&             ids,
            std::unordered_map<cg::db::FileId, int>& index_of,
            cg::db::FileId                           fid) -> int
        {
            if (const auto found = index_of.find(fid); found != index_of.end())
            {
                return found->second;
            }
            const int idx = static_cast<int>(ids.size());
            ids.push_back(fid);
            index_of.emplace(fid, idx);
            return idx;
        }

        /**
         * @brief 文件 id 到路径文本的只读映射；缺席时给占位文本
         *        （保住引用返回的完整性）。
         */
        auto cycle_path_text(
            const std::unordered_map<cg::db::FileId, std::string>& paths,
            cg::db::FileId f) -> const std::string&
        {
            static const std::string MISSING_TEXT = "?";
            const auto               it = paths.find(f);
            return it == paths.end() ? MISSING_TEXT : it->second;
        }

    } // namespace detail

    /**
     * @brief 变更集影响面（ccg changes）。
     *
     * 变更文件中定义的每个符号都是种子；走查沿上游进行：calls（调用者）、
     * extends（派生类型）、uses（声明类型引用变更记录的字段/参数/返回值/
     * 局部变量/变量）、overrides（双向——变更虚方法的覆盖族）。消费者按
     * 其在全部种子上的最短深度列出；种子本身是变更的侧面，绝不作为条目。
     * 每消费者风险与 impact 同阶梯，按
     * score = (深度 1 ? 3 : 1) + fan_in 分级；摘要风险按
     * 3*direct + transitive + fan_in、以直接消费者的扇入计。max_depth
     * 钳制到 [1, 10]。
     */
    [[nodiscard]]
    inline auto changes(
        cg::db::GraphStore&            store,
        std::span<const cg::db::SymId> changed,
        int                            max_depth = 3,
        DispatchMode mode = DispatchMode::Precise) -> ChangesResult
    {
        const int     depth = std::clamp(max_depth, 1, 10);
        ChangesResult out;
        out.dispatch_mode = mode;
        std::unordered_set<cg::db::SymId> seed_set;
        for (const cg::db::SymId s : changed)
        {
            if (seed_set.insert(s).second)
            {
                out.seeds.push_back(s);
            }
        }

        std::unordered_map<cg::db::SymId, ChangesEntry>  reached;
        std::unordered_map<cg::db::SymId, cg::db::SymId> origin;
        std::vector<cg::db::SymId>                       frontier{out.seeds};
        std::unordered_set<cg::db::SymId>                visited{seed_set};
        for (const cg::db::SymId s : out.seeds)
        {
            origin.emplace(s, s);
        }
        std::optional<detail::DispatchIndex> dispatch;
        const bool                           precise =
            mode == DispatchMode::Precise || mode == DispatchMode::Fallback;
        if (precise)
        {
            dispatch.emplace(store);
            out.dispatch_stale_files = dispatch->stale_files();
        }
        else if (mode == DispatchMode::Additive)
        {
            // Additive 旧模式不构造索引（vd 边直读库），标注从同一 meta 取。
            out.dispatch_stale_files = detail::dispatch_stale_count(store);
        }
        for (int d = 1; d <= depth && !frontier.empty(); ++d)
        {
            std::vector<cg::db::SymId> next;
            if (!precise)
            {
                for (const cg::db::SymId cur : frontier)
                {
                    for (const cg::db::EdgeAggRow& e :
                         detail::changes_step(store, cur))
                    {
                        if (visited.insert(e.other).second)
                        {
                            const cg::db::SymId seed = origin.at(cur);
                            reached.emplace(
                                e.other,
                                ChangesEntry{
                                    .id = e.other,
                                    .depth = d,
                                    .seed = seed,
                                    .via_id = cur});
                            origin.emplace(e.other, seed);
                            next.push_back(e.other);
                        }
                    }
                }
            }
            else
            {
                std::map<cg::db::SymId, detail::WalkContribution> best;
                for (const cg::db::SymId cur : frontier)
                {
                    for (const detail::WalkContribution& c :
                         detail::changes_step_precise(store, cur, *dispatch))
                    {
                        detail::consider_contribution(best, c);
                    }
                }
                for (auto& [other, c] : best)
                {
                    if (!visited.insert(other).second)
                    {
                        continue;
                    }
                    const cg::db::SymId seed = origin.at(c.via_id);
                    reached.emplace(
                        other,
                        ChangesEntry{
                            .id = other,
                            .depth = d,
                            .seed = seed,
                            .via_id = c.via_id,
                            .dispatch_status = c.status,
                            .via_type = c.via,
                            .dispatch_confidence = c.dispatch_confidence});
                    origin.emplace(other, seed);
                    next.push_back(other);
                }
            }
            frontier = std::move(next);
            out.truncated = !frontier.empty();
        }

        out.consumers.reserve(reached.size());
        for (auto& [id, entry] : reached)
        {
            entry.fan_in = fan_in_of(store, id, precise ? &*dispatch : nullptr);
            const std::int64_t score =
                (entry.depth == 1 ? 3 : 1) + entry.fan_in;
            entry.risk = detail::risk_from_score(score);
            out.consumers.push_back(entry);
        }
        std::ranges::sort(
            out.consumers,
            [](const ChangesEntry& a, const ChangesEntry& b)
            { return a.depth != b.depth ? a.depth < b.depth : a.id < b.id; });

        for (const ChangesEntry& e : out.consumers)
        {
            if (e.depth == 1)
            {
                ++out.direct;
                out.fan_in += e.fan_in;
            }
        }
        out.transitive =
            static_cast<std::int64_t>(out.consumers.size()) - out.direct;
        out.risk_score = 3 * out.direct + out.transitive + out.fan_in;
        if (out.direct == 0)
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
     * @brief 包含图环检测（ccg check --cycles）。
     *
     * 以增量引擎规则集的严格解析子集构建已解析包含图
     * （IncludeResolver::resolve_strict：仅精确路径与唯一完整拼写后缀
     * 匹配——环必须真实，可能解析到多个文件的包含不贡献边、计入歧义），
     * 返回每个规模大于一文件的强连通分量或自包含文件及其内部包含边。
     * 簇按确定性顺序排列（字典序最小成员路径）；cap 限制返回数量——
     * *Total 计数始终覆盖找到的全部簇，截断与否皆然（列表上限
     * 从不截断上报的计数）。
     *
     * @internal
     * 已解析包含边存入 std::set（去重且迭代顺序确定），links 以排序顺序
     * 迭代故每个邻接表天然有序；仅对被链接的文件建稠密节点索引（孤立
     * 文件不可能参与环）。SCC 用迭代式 Tarjan（显式帧栈替代递归——病态
     * 包含图可深于默认栈）。物化簇时按路径文本排序（跨运行稳定；文件
     * id 每库稳定，但路径才是人读的标识）。cycle_path_text 中的
     * MISSING_TEXT 占位实际不可达（每个节点都来自 files 表），仅保住
     * 引用返回的完整性；包含者行已消失的链接直接跳过，不作为环输入。
     */
    [[nodiscard]]
    inline auto include_cycles(cg::db::GraphStore& store, std::size_t cap)
        -> CyclesResult
    {
        CyclesResult                       out;
        const std::vector<cg::db::FileRow> files = cg::db::all_files(store);
        out.files_scanned = static_cast<std::int64_t>(files.size());

        detail::IncludeResolver                         resolver;
        std::unordered_map<cg::db::FileId, std::string> paths;
        paths.reserve(files.size() * 2);
        for (const cg::db::FileRow& fr : files)
        {
            resolver.add(fr.path, fr.id);
            paths.emplace(fr.id, fr.path);
        }

        std::set<std::pair<cg::db::FileId, cg::db::FileId>> links;
        for (const cg::db::IncludeLink& link : cg::db::all_includes(store))
        {
            const auto it = paths.find(link.includer);
            if (it == paths.end())
            {
                continue;
            }
            const detail::IncludeResolver::StrictTarget t =
                resolver.resolve_strict(
                    link.target,
                    it->second,
                    /*quoted=*/!link.angled);
            switch (t.kind)
            {
            case detail::IncludeResolver::StrictTarget::Kind::Unique:
                links.emplace(link.includer, t.file);
                break;
            case detail::IncludeResolver::StrictTarget::Kind::Ambiguous:
                ++out.ambiguous_links;
                break;
            case detail::IncludeResolver::StrictTarget::Kind::None:
            default:
                ++out.unresolved_links;
                break;
            }
        }

        std::vector<cg::db::FileId>             ids;
        std::unordered_map<cg::db::FileId, int> index_of;
        std::vector<std::pair<int, int>>        edges_idx;
        edges_idx.reserve(links.size());
        for (const auto& [src, dst] : links)
        {
            edges_idx.emplace_back(
                detail::cycle_node_of(ids, index_of, src),
                detail::cycle_node_of(ids, index_of, dst));
        }
        const int                     n = static_cast<int>(ids.size());
        std::vector<std::vector<int>> adj(static_cast<std::size_t>(n));
        for (const auto& [src, dst] : edges_idx)
        {
            adj[static_cast<std::size_t>(src)].push_back(dst);
        }
        std::vector<int>  order(static_cast<std::size_t>(n), -1);
        std::vector<int>  low(static_cast<std::size_t>(n), 0);
        std::vector<char> on_stack(static_cast<std::size_t>(n), 0);
        std::vector<int>  scc_stack;
        struct Frame
        {
            int         v;
            std::size_t next = 0;
        };
        std::vector<Frame>            call;
        int                           counter = 0;
        std::vector<std::vector<int>> raw_groups;
        for (int root = 0; root < n; ++root)
        {
            if (order[root] != -1)
            {
                continue;
            }
            order[root] = low[static_cast<std::size_t>(root)] = counter++;
            scc_stack.push_back(root);
            on_stack[static_cast<std::size_t>(root)] = 1;
            call.push_back({.v = root});
            while (!call.empty())
            {
                Frame& f = call.back();
                if (f.next < adj[static_cast<std::size_t>(f.v)].size())
                {
                    const int w = adj[static_cast<std::size_t>(f.v)][f.next++];
                    if (order[w] == -1)
                    {
                        order[w] = low[static_cast<std::size_t>(w)] = counter++;
                        scc_stack.push_back(w);
                        on_stack[static_cast<std::size_t>(w)] = 1;
                        call.push_back({.v = w});
                    }
                    else if (on_stack[static_cast<std::size_t>(w)] != 0)
                    {
                        low[static_cast<std::size_t>(f.v)] = std::min(
                            low[static_cast<std::size_t>(f.v)],
                            order[w]);
                    }
                    continue;
                }
                const int v = f.v;
                call.pop_back();
                if (!call.empty())
                {
                    low[static_cast<std::size_t>(call.back().v)] = std::min(
                        low[static_cast<std::size_t>(call.back().v)],
                        low[static_cast<std::size_t>(v)]);
                }
                if (low[static_cast<std::size_t>(v)]
                    != order[static_cast<std::size_t>(v)])
                {
                    continue;
                }
                std::vector<int> comp;
                for (;;)
                {
                    const int w = scc_stack.back();
                    scc_stack.pop_back();
                    on_stack[static_cast<std::size_t>(w)] = 0;
                    comp.push_back(w);
                    if (w == v)
                    {
                        break;
                    }
                }
                const bool self_loop =
                    std::ranges::contains(adj[static_cast<std::size_t>(v)], v);
                if (comp.size() > 1 || self_loop)
                {
                    raw_groups.push_back(std::move(comp));
                }
            }
        }

        const auto path_less = [&](cg::db::FileId a, cg::db::FileId b)
        {
            const std::string& sa = detail::cycle_path_text(paths, a);
            const std::string& sb = detail::cycle_path_text(paths, b);
            return sa != sb ? sa < sb : a < b;
        };
        std::vector<CycleGroup> all;
        for (const std::vector<int>& comp : raw_groups)
        {
            CycleGroup g;
            g.files.reserve(comp.size());
            for (const int v : comp)
            {
                g.files.push_back(ids[static_cast<std::size_t>(v)]);
            }
            std::ranges::sort(g.files, path_less);
            std::unordered_set<cg::db::FileId> member;
            member.reserve(comp.size());
            for (const int v : comp)
            {
                member.insert(ids[static_cast<std::size_t>(v)]);
            }
            for (const auto& [src, dst] : links)
            {
                if (member.contains(src) && member.contains(dst))
                {
                    g.edges.emplace_back(src, dst);
                }
            }
            std::ranges::sort(
                g.edges,
                [&](const auto& a, const auto& b)
                {
                    if (a.first != b.first)
                    {
                        return path_less(a.first, b.first);
                    }
                    return path_less(a.second, b.second);
                });
            out.groups_total += 1;
            out.files_total += static_cast<std::int64_t>(g.files.size());
            out.edges_total += static_cast<std::int64_t>(g.edges.size());
            all.push_back(std::move(g));
        }
        std::ranges::sort(
            all,
            [&](const CycleGroup& a, const CycleGroup& b)
            { return path_less(a.files.front(), b.files.front()); });
        const std::size_t take = std::min(cap, all.size());
        out.groups.assign(
            all.begin(),
            all.begin() + static_cast<std::ptrdiff_t>(take));
        out.truncated = take < all.size();
        return out;
    }
} // namespace cg::graph
