/**
 * @module cg.graph.traverse.dispatchidx
 *
 * @brief 走查域的边类型常量与派发证据装载：DispatchIndex
 * 及其行/键/目标/证据结构。
 */
export module cg.graph.traverse.dispatchidx;

import std;
import cg.util;
import cg.db;
import cg.graph.traverse.model;

export namespace cg::graph
{

    namespace detail
    {

        /** @internal 边类型集合一律静态数组：绝不用悬垂初始化列表上的临时
         * span。 */
        inline constexpr cg::db::EdgeType CALLS_TYPES[] = {
            cg::db::EdgeType::Calls};
        inline constexpr cg::db::EdgeType IMPACT_TYPES[] = {
            cg::db::EdgeType::Calls,
            cg::db::EdgeType::Extends,
            cg::db::EdgeType::Uses};
        inline constexpr cg::db::EdgeType OVERRIDE_TYPES[] = {
            cg::db::EdgeType::Overrides};
        inline constexpr cg::db::EdgeType USES_TYPES[] = {
            cg::db::EdgeType::Uses};
        inline constexpr cg::db::EdgeType CHANGES_UP_TYPES[] = {
            cg::db::EdgeType::Calls,
            cg::db::EdgeType::Extends,
            cg::db::EdgeType::Uses};
        /**
         * @internal Additive 旧模式（诊断退路）的类型集：静态边与
         * virtual_dispatch 并存；默认 Precise 走按站点的独立路径。
         */
        inline constexpr cg::db::EdgeType DISPATCH_TYPES[] = {
            cg::db::EdgeType::Calls,
            cg::db::EdgeType::VirtualDispatch};
        inline constexpr cg::db::EdgeType IMPACT_DISPATCH_TYPES[] = {
            cg::db::EdgeType::Calls,
            cg::db::EdgeType::Extends,
            cg::db::EdgeType::Uses,
            cg::db::EdgeType::VirtualDispatch};

        /** @internal 继承链上溯深度上限（与 analysis.dispatch
         * 的祖先距离一致）。 */
        inline constexpr int OVERRIDE_ANCESTOR_LIMIT = 16;

        /**
         * @internal 延迟收敛：库级派发滞后文件数（meta dispatch_stale）。
         * DispatchIndex 构造点与不构造索引、直接从库读 vd 边的路径
         * （Additive 模式）都经本函数取同一标注。
         */
        [[nodiscard]]
        inline auto dispatch_stale_count(cg::db::GraphStore& store)
            -> std::int64_t
        {
            return cg::util::parse_bounded(store.meta("dispatch_stale"), 0);
        }

        /** @internal 查询侧读取的一条 virtual_dispatch 边。 */
        struct DispatchEdgeRow
        {
            cg::db::SymId  src = 0;
            cg::db::SymId  dst = 0;
            cg::db::FileId site_file = 0;
            int            site_line = 0;
            double         confidence = 0.0;
            std::uint32_t  attrs = 0;
        };

        /** @internal 调用点键：caller + 文件 + 行（edges 表无 column）。 */
        struct DispatchSiteKey
        {
            cg::db::SymId  caller = 0;
            cg::db::FileId file = 0;
            int            line = 0;
            auto           operator<=>(const DispatchSiteKey&) const = default;
        };

        /** @internal 某调用点上的一个正面派发目标。 */
        struct DispatchTargetRef
        {
            cg::db::SymId target = 0;
            double        confidence = 0.0;
            std::uint32_t attrs = 0;
        };

        /** @internal 某调用点上的派发证据汇总。 */
        struct DispatchSiteEvidence
        {
            std::vector<DispatchTargetRef> positive; // confidence > floor
        };

        /**
         * @brief 查询侧派发证据索引：一次扫描全部 virtual_dispatch 边。
         *
         * @internal
         * - 调用点键 `(caller, file, line)` 用于“该调用点有没有证据”与静态
         *   CALLS 边的替换判定；
         * - 按目标/来源的索引用于把正面派发边作为一等调用关系加入 BFS；
         * - `ancestors` 缓存 override 祖先闭包（派生 -> 基，深度上限 16），
         *   供方向性实现集 `impl_set` 判定。纯虚成员不算实现。
         *
         * 低于置信下限（含 0.0 家族标记）的边不进入 positive——Fallback 模式
         * 只按站点有无 positive 证据做状态标注，绝不改变归属集合。
         */
        class DispatchIndex
        {
        public:
            explicit DispatchIndex(cg::db::GraphStore& store) : store_(&store)
            {
                // 延迟收敛的诚实性闸（设计评审修正）：构造点即感知滞后，
                // 消费方（BFS/trace/hook 注入/CLI 渲染）都从这里读标注，
                // 绝不静默吃旧派发数据。meta 缺席/非法 = 0（与已收敛同义）。
                stale_files_ = dispatch_stale_count(store);
                for (const cg::db::EdgeRow& e : cg::db::edges_of_type(
                         store,
                         cg::db::EdgeType::VirtualDispatch))
                {
                    const std::size_t idx = edges_.size();
                    edges_.push_back(
                        {.src = e.src,
                         .dst = e.dst,
                         .site_file = e.site_file,
                         .site_line = e.site_line,
                         .confidence = e.confidence,
                         .attrs = e.attrs});
                    by_src_[e.src].push_back(idx);
                    by_dst_[e.dst].push_back(idx);
                    DispatchSiteEvidence& ev = sites_[{
                        .caller = e.src,
                        .file = e.site_file,
                        .line = e.site_line}];
                    if (e.confidence >= kDispatchConfidenceFloor)
                    {
                        ev.positive.push_back(
                            {.target = e.dst,
                             .confidence = e.confidence,
                             .attrs = e.attrs});
                    }
                }
                for (auto& [key, ev] : sites_)
                {
                    std::ranges::sort(
                        ev.positive,
                        [](const DispatchTargetRef& a,
                           const DispatchTargetRef& b)
                        {
                            if (a.target != b.target)
                            {
                                return a.target < b.target;
                            }
                            return a.confidence > b.confidence;
                        });
                }
                for (auto& [id, list] : by_src_)
                {
                    (void)id;
                    sort_edge_slots_(list, /*by_dst=*/true);
                }
                for (auto& [id, list] : by_dst_)
                {
                    (void)id;
                    sort_edge_slots_(list, /*by_dst=*/false);
                }
            }

            [[nodiscard]]
            auto evidence_at(
                cg::db::SymId  caller,
                cg::db::FileId file,
                int            line) const -> const DispatchSiteEvidence&
            {
                static const DispatchSiteEvidence EMPTY{};
                const auto                        it =
                    sites_.find({.caller = caller, .file = file, .line = line});
                return it == sites_.end() ? EMPTY : it->second;
            }

            [[nodiscard]]
            auto outgoing(cg::db::SymId src) const
                -> std::span<const std::size_t>
            {
                return lookup(by_src_, src);
            }

            [[nodiscard]]
            auto incoming(cg::db::SymId dst) const
                -> std::span<const std::size_t>
            {
                return lookup(by_dst_, dst);
            }

            [[nodiscard]]
            auto edge(std::size_t idx) const -> const DispatchEdgeRow&
            {
                return edges_[idx];
            }

            /**
             * @brief 派发数据滞后于索引的文件数（延迟收敛；>0 = 本索引
             *        吃到的是编辑前派发事实，消费方必须标注）。
             */
            [[nodiscard]]
            auto stale_files() const -> std::int64_t
            {
                return stale_files_;
            }

            /** @brief 纯虚符号没有可执行实现。 */
            [[nodiscard]]
            auto is_pure(cg::db::SymId id) const -> bool
            {
                if (const auto it = pure_.find(id); it != pure_.end())
                {
                    return it->second;
                }
                bool pure = false;
                if (const auto row = cg::db::get_symbol(*store_, id))
                {
                    pure =
                        (row->flags & cg::db::sym_flags::is_pure_virtual) != 0;
                }
                pure_.emplace(id, pure);
                return pure;
            }

            /**
             * @brief T 的传递 override 祖先（派生 -> 基；不含 T 自身）。
             */
            [[nodiscard]]
            auto ancestors(cg::db::SymId id) const
                -> const std::unordered_set<cg::db::SymId>&
            {
                if (const auto it = ancestors_.find(id); it != ancestors_.end())
                {
                    return it->second;
                }
                std::unordered_set<cg::db::SymId> out;
                std::vector<cg::db::SymId>        frontier{id};
                for (int depth = 0;
                     depth < OVERRIDE_ANCESTOR_LIMIT && !frontier.empty();
                     ++depth)
                {
                    std::vector<cg::db::SymId> next;
                    // 整层 overrides 出边一批 IN 查询（逐节点版折叠）；
                    // 结果是集合，无顺序依赖。
                    const std::vector<cg::db::EdgeAggBySym> rows =
                        cg::db::edges_from_batch(
                            *store_,
                            frontier,
                            OVERRIDE_TYPES);
                    for (const cg::db::SymId cur : frontier)
                    {
                        const auto [begin, end] = std::ranges::equal_range(
                            rows,
                            cur,
                            {},
                            &cg::db::EdgeAggBySym::self);
                        for (const auto& hit :
                             std::ranges::subrange{begin, end})
                        {
                            const cg::db::EdgeAggRow& e = hit.row;
                            if (e.other != id && out.insert(e.other).second)
                            {
                                next.push_back(e.other);
                            }
                        }
                    }
                    frontier = std::move(next);
                }
                return ancestors_.emplace(id, std::move(out)).first->second;
            }

            /**
             * @brief candidate ∈ impl_set(owner) ⇔ candidate == owner
             * 且非纯虚， 或 owner 在 candidate 的 override 祖先链上。
             */
            [[nodiscard]]
            auto in_impl_set(cg::db::SymId owner, cg::db::SymId candidate) const
                -> bool
            {
                if (candidate == owner)
                {
                    return !is_pure(owner);
                }
                return ancestors(candidate).contains(owner);
            }

            /**
             * @brief 该静态 CALLS 边是否被同一调用点上的正面派发证据替换。
             *
             * 守卫：CALLS 边必须标记为虚调用，且证据目标至少有一个属于
             * `impl_set(CALLS.dst)`——同一行的非虚调用或另一家族的调用不被误删。
             */
            [[nodiscard]]
            auto suppresses_calls(const cg::db::EdgeRow& e) const -> bool
            {
                if (e.type != cg::db::EdgeType::Calls)
                {
                    return false;
                }
                if ((e.attrs & cg::db::edge_attrs::dispatch_virtual) == 0)
                {
                    return false;
                }
                for (const DispatchTargetRef& t :
                     evidence_at(e.src, e.site_file, e.site_line).positive)
                {
                    if (in_impl_set(e.dst, t.target))
                    {
                        return true;
                    }
                }
                return false;
            }

        private:
            [[nodiscard]]
            static auto lookup(
                const std::map<cg::db::SymId, std::vector<std::size_t>>& index,
                cg::db::SymId id) -> std::span<const std::size_t>
            {
                const auto it = index.find(id);
                if (it == index.end())
                {
                    return {};
                }
                return std::span<const std::size_t>{it->second};
            }

            /**
             * @brief 就地排序一组边槽：首键 by_dst ? dst : src，置信度降序，
             *        站点（文件，行）作次键与末键。
             */
            void sort_edge_slots_(std::vector<std::size_t>& list, bool by_dst)
            {
                std::ranges::sort(
                    list,
                    [this, by_dst](std::size_t a, std::size_t b)
                    {
                        const DispatchEdgeRow& x = edges_[a];
                        const DispatchEdgeRow& y = edges_[b];
                        const cg::db::SymId    kx = by_dst ? x.dst : x.src;
                        const cg::db::SymId    ky = by_dst ? y.dst : y.src;
                        if (kx != ky)
                        {
                            return kx < ky;
                        }
                        if (x.confidence != y.confidence)
                        {
                            return x.confidence > y.confidence;
                        }
                        if (x.site_file != y.site_file)
                        {
                            return x.site_file < y.site_file;
                        }
                        return x.site_line < y.site_line;
                    });
            }

            cg::db::GraphStore*                               store_;
            std::vector<DispatchEdgeRow>                      edges_;
            std::map<DispatchSiteKey, DispatchSiteEvidence>   sites_;
            std::map<cg::db::SymId, std::vector<std::size_t>> by_src_;
            std::map<cg::db::SymId, std::vector<std::size_t>> by_dst_;
            /** @brief 构造时读到的 dispatch_stale meta（滞后文件数）。 */
            std::int64_t stale_files_ = 0;
            mutable std::map<cg::db::SymId, std::unordered_set<cg::db::SymId>>
                                                  ancestors_;
            mutable std::map<cg::db::SymId, bool> pure_;
        };

    } // namespace detail
} // namespace cg::graph
