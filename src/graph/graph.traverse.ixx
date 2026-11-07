module;
#include "../db/db.tables.h"

/**
 * @module cg.graph.traverse
 *
 * @brief cg.db 之上的读侧图算法：带最短深度标注的 BFS 调用者/被调用者、
 *        带风险阶梯的影响面（直接/传递计数加受影响者的扇入）、最短调用
 *        路径 trace、上下文分组装配——以上均支持虚方法的覆盖族合并
 *        ——变更集传播（ccg changes）、包含图环检测（ccg check
 *        --cycles），以及调用闭包与模块级聚合图（ccg graph 导出）。
 *
 * @internal
 * 纯查询：唯一写入方是 cg.graph.build，它导入本模块以复用包含解析器
 * （两侧共用同一套规则，环检测使用其中的严格无歧义子集）。覆盖族 =
 * 被查询方法本身加上经 overrides 边连通的全部方法。db.tables.h 置于
 * 全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23 表模型定义在此
 * 可见，以合并模块记录的模板特化（完整机理见 db.tables.h）。
 */
export module cg.graph.traverse;

import std;
import cg.util;
import cg.db;

export namespace cg::graph
{

    /** @brief 走查方向：Up 沿入边（谁依赖我），Down 沿出边（我依赖谁）。 */
    enum class Direction : std::uint8_t
    {
        Up,
        Down
    };

    /** @brief 风险分级（阶梯定义见 impact）。 */
    enum class Risk : std::uint8_t
    {
        Low,
        Medium,
        High,
        Critical,
        Unknown
    };

    /**
     * @brief 查询侧对动态派发（virtual_dispatch）证据的策略。
     *
     * - Off：纯静态视图；与引入 dispatch 之前的默认输出逐字节等价
     *   （CLI `--no-dispatch`）。
     * - Precise：默认。confidence >= 0.7 的派发证据替换调用点上的静态家族
     *   过近似；无证据的家族合并按 family estimate 标注。
     * - Fallback：Precise 加上显示 confidence == 0 的家族标记（标记只做
     *   展示与状态标注，不改变归属集合）。
     * - Additive：旧的 `--dispatch` 行为，静态与派发两条路径并存；仅作
     *   诊断/实验退路。
     */
    enum class DispatchMode : std::uint8_t
    {
        Off,
        Precise,
        Fallback,
        Additive
    };

    /**
     * @brief 默认派发证据阈值：confidence >= 0.7 的 virtual_dispatch 才算证据。
     *
     * @internal 唯一事实源。查询侧判定与 CLI 的 JSON `threshold` 字段都引用
     * 此常量；0.4 是弱推断、0.0 是家族级标记，二者都不作为具体目标证据。
     */
    inline constexpr double kDispatchConfidenceFloor = 0.7;

    /**
     * @brief 一条查询结果的派发证据状态。
     *
     * `Inferred` = 有 confidence >= 0.7 的 virtual_dispatch
     * 证据；`FamilyEstimate` = 动态类型无法证明，按整个 override
     * 家族估算（可能包含并非实际目标 的实现）；`None` =
     * 直接静态关系，没有派发不确定性。
     */
    enum class DispatchStatus : std::uint8_t
    {
        None,
        Inferred,
        FamilyEstimate
    };

    /** @brief 邻域走查到达的一个符号，处于其最短深度。 */
    struct NeighborEntry
    {
        cg::db::SymId    id = 0;
        int              depth = 0;
        cg::db::EdgeType via = cg::db::EdgeType::Calls;
        cg::db::SymId    via_id = 0; // 展开来源符号（'via' 边的起点）
        std::int64_t     occurrences = 0;
        /**
         * @brief 覆盖族合并：本链途经的被查询方法家族成员
         *        （0 = 被查询符号本身，或合并已关闭）。
         */
        cg::db::SymId family_origin = 0;
        /** @brief 该条目的证据置信度（派发证据时为派发置信度）。 */
        double confidence = 1.0;
        /** @brief 该条目的属性（dispatch_inferred/fallback 可见）。 */
        std::uint32_t attrs = 0;
        /**
         * @brief 该条目是否由 confidence >= 0.7 的 virtual_dispatch 边证明。
         *
         * @internal Additive 旧模式仍用它表达“同目标同时存在 CALLS 与派发”；
         * Precise 下它为 true 时 dispatch_status 必为 Inferred。
         */
        bool          has_dispatch = false;
        double        dispatch_confidence = 0.0;
        std::uint32_t dispatch_attrs = 0;
        /** @brief 派发证据状态（Off 模式恒为 None）。 */
        DispatchStatus dispatch_status = DispatchStatus::None;
    };

    /** @brief 邻域走查结果。 */
    struct NeighborsResult
    {
        std::vector<NeighborEntry> entries; // 按深度排序，再按 id
        bool truncated = false;             // 深度上限截停了仍在生长的边界
        /**
         * @brief 走查的种子覆盖族（合并关闭或符号无家族时仅含被查询符号）。
         */
        std::vector<cg::db::SymId> family;
        /** @brief 本次走查使用的派发策略（Off 时不渲染任何派发字段）。 */
        DispatchMode dispatch_mode = DispatchMode::Off;
        /**
         * @brief 派发数据滞后于索引的文件数（延迟收敛；>0 时渲染层
         *        必须显式标注，绝不静默）。
         */
        std::int64_t dispatch_stale_files = 0;
    };

    /** @brief 影响面的一个深度层（第 1 层 = 直接）。 */
    struct ImpactLevel
    {
        int                        depth = 0;
        std::vector<cg::db::SymId> ids;
    };

    /** @brief 影响面结果：分层 id 集合与风险阶梯摘要。 */
    struct ImpactResult
    {
        std::vector<ImpactLevel> by_depth;     // 第 1 层 = 直接
        std::int64_t             impacted = 0; // 去重后的受影响符号总数
        std::int64_t             direct = 0;
        std::int64_t             transitive = 0; // impacted - direct
        std::int64_t             fan_in = 0;     // 直接命中者的入边来源数之和
        std::int64_t risk_score = 0; // 3*direct + transitive + fan_in
        Risk         risk = Risk::Unknown;
        bool         truncated = false;
        std::vector<cg::db::SymId> family; // 覆盖族种子（合并视图）
        /**
         * @brief 每个受影响符号：其链途经的家族成员（缺省 = 被查询符号
         *        本身 / 未合并）。
         */
        std::unordered_map<cg::db::SymId, cg::db::SymId> family_of;
        /**
         * @brief 经 confidence >= 0.7 的 virtual_dispatch
         * 边证明的符号及其置信度 （Off 模式为空）。
         */
        std::unordered_map<cg::db::SymId, double>        dispatch_confidence;
        std::unordered_map<cg::db::SymId, std::uint32_t> dispatch_attrs;
        /** @brief 每个受影响符号的派发证据状态（Off 模式为空）。 */
        std::unordered_map<cg::db::SymId, DispatchStatus> dispatch_status;
        /** @brief 本次影响面使用的派发策略。 */
        DispatchMode dispatch_mode = DispatchMode::Off;
        /** @brief 派发数据滞后于索引的文件数（延迟收敛；渲染层须标注）。 */
        std::int64_t dispatch_stale_files = 0;
    };

    /** @brief 调用路径上的一跳：目标符号 + 进入它的边证据。 */
    struct TraceHop
    {
        cg::db::SymId    id = 0;
        cg::db::EdgeType via = cg::db::EdgeType::Calls;
        DispatchStatus   dispatch_status = DispatchStatus::None;
        double           confidence = 1.0; // 派发证据时为派发置信度
        cg::db::FileId   site_file = 0;
        int              site_line = 0;
    };

    /** @brief 最短调用路径查询结果。 */
    struct TraceResult
    {
        bool                       found = false;
        std::vector<cg::db::SymId> chain; // 找到时为 [from, ..., to]
        /**
         * @brief 与 chain 对齐的逐跳证据；Off 模式为空（保持旧输出路径）。
         */
        std::vector<TraceHop> hops;
        DispatchMode          dispatch_mode = DispatchMode::Off;
        bool                  truncated = false; // 上限截停时边界仍在生长
        /** @brief 派发数据滞后于索引的文件数（延迟收敛；渲染层须标注）。 */
        std::int64_t dispatch_stale_files = 0;
    };

    /** @brief 调用闭包图（ccg graph --symbol）的一个节点：calls 出边 BFS
     * 到达，处于最短深度。 */
    struct CallNode
    {
        cg::db::SymId id = 0;
        int           depth = 0; // 0 = 入口符号自身
    };

    /** @brief 调用闭包内的一条聚合调用边：(src, dst) 的 occurrences 之和。 */
    struct CallClosureEdge
    {
        cg::db::SymId src = 0;
        cg::db::SymId dst = 0;
        std::int64_t  occurrences = 0;
    };

    /** @brief 入口符号的调用闭包（ccg graph --symbol 的数据面）。 */
    struct CallClosure
    {
        std::vector<CallNode> nodes; // 含入口（depth 0）；按 (depth, id) 排序
        std::vector<CallClosureEdge>
             edges;             // 两端均在闭包内的 calls 边，按 (src, dst) 排序
        bool truncated = false; // 深度上限截停了仍在生长的边界
    };

    /** @brief 模块级聚合图（ccg graph
     * --modules）的一条边：跨模块的符号级调用对数。
     */
    struct ModuleGraphEdge
    {
        std::string  from;
        std::string  to;
        std::int64_t weight = 0;
    };

    /** @brief 模块级调用聚合图：模块 = 定义文件名词干，同文件调用不出图。 */
    struct ModuleGraph
    {
        std::vector<std::string> modules; // 保留边的端点，字典序（无孤儿节点）
        std::vector<ModuleGraphEdge> edges; // 按 (from, to) 排序
        std::int64_t                 edges_total =
            0; // 权重过滤前的跨模块聚合边总数（计数从不上限截断）
    };

    /** @brief 上下文视图中的一条边：对端符号已物化，附带边行。 */
    struct ContextEdge
    {
        cg::db::SymbolRow other;
        cg::db::EdgeRow   edge;
        cg::db::SymId     family_origin = 0; // 贡献此边的家族成员（0 = 自身）
        DispatchStatus    dispatch_status = DispatchStatus::None;
    };

    /** @brief 360 度视图：符号本身加全部出入边。 */
    struct ContextView
    {
        cg::db::SymbolRow          symbol;
        std::vector<ContextEdge>   incoming; // 按类型分组，calls 在前
        std::vector<ContextEdge>   outgoing;
        std::vector<cg::db::SymId> family; // 覆盖族（合并视图）
        DispatchMode               dispatch_mode = DispatchMode::Off;
        /** @brief 派发数据滞后于索引的文件数（延迟收敛；渲染层须标注）。 */
        std::int64_t dispatch_stale_files = 0;
    };

    /**
     * @brief 变更集（ccg changes）的一个消费者：从变更文件中定义的符号沿
     *        上游到达的符号，处于其在全部种子上的最短深度。
     */
    struct ChangesEntry
    {
        cg::db::SymId id = 0;
        int           depth = 0;  // 1 = 变更文件符号的直接消费者
        cg::db::SymId seed = 0;   // 链的起点（变更文件符号）
        cg::db::SymId via_id = 0; // 上一跳（深度 1 时即种子本身）
        std::int64_t  fan_in = 0; // 去重入边来源数（calls/extends/uses）
        Risk          risk = Risk::Unknown; // (直接?3:1) + fan_in 的分级
        /** @brief 到达该消费者的一跳证据状态。 */
        DispatchStatus   dispatch_status = DispatchStatus::None;
        cg::db::EdgeType via_type = cg::db::EdgeType::Calls;
        double           dispatch_confidence = 0.0; // 仅 Inferred 有意义
    };

    /** @brief 变更集传播结果。 */
    struct ChangesResult
    {
        std::vector<cg::db::SymId> seeds;     // 变更文件中定义的符号
        std::vector<ChangesEntry>  consumers; // 按 (depth, id) 排序
        std::int64_t               direct = 0;
        std::int64_t               transitive = 0; // consumers - direct
        std::int64_t               fan_in = 0;     // 直接消费者的扇入之和
        std::int64_t risk_score = 0; // 3*direct + transitive + fan_in
        Risk         risk = Risk::Unknown;
        bool         truncated = false; // 深度上限截停了仍在生长的边界
        /** @brief 本次传播使用的派发策略。 */
        DispatchMode dispatch_mode = DispatchMode::Off;
        /** @brief 派发数据滞后于索引的文件数（延迟收敛；渲染层须标注）。 */
        std::int64_t dispatch_stale_files = 0;
    };

    /**
     * @brief 包含图（ccg check --cycles）的一个环簇：强连通分量
     *        （规模 > 1，或自包含文件）及其内部包含边。
     */
    struct CycleGroup
    {
        std::vector<cg::db::FileId> files; // 按路径文本排序
        std::vector<std::pair<cg::db::FileId, cg::db::FileId>> edges;
    };

    /** @brief 环检测结果：*Total 计数覆盖全部簇，不受返回上限影响。 */
    struct CyclesResult
    {
        std::int64_t groups_total = 0;     // 找到的环簇数（从不上限截断）
        std::int64_t files_total = 0;      // 处于环中的文件数，全部簇（无上限）
        std::int64_t edges_total = 0;      // 簇内边数，全部簇
        std::int64_t files_scanned = 0;    // 构建图所用的文件行数
        std::int64_t unresolved_links = 0; // 严格模式下不匹配任何文件的目标
        /**
         * @brief 拼写匹配到多个文件的目标：没有真实包含搜索路径便无法
         *        判定，故不贡献环边（门禁绝不捏造边）但如实上报、绝不丢弃。
         */
        std::int64_t            ambiguous_links = 0;
        std::vector<CycleGroup> groups;            // 上限为 cap，确定性顺序
        bool                    truncated = false; // 实际簇数超过返回上限
    };

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
                const auto sort_edges =
                    [this](std::vector<std::size_t>& list, bool by_dst)
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
                };
                for (auto& [id, list] : by_src_)
                {
                    (void)id;
                    sort_edges(list, /*by_dst=*/true);
                }
                for (auto& [id, list] : by_dst_)
                {
                    (void)id;
                    sort_edges(list, /*by_dst=*/false);
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

        /**
         * @brief 文件表之上的包含目标解析器。
         *
         * 引号目标先对包含者目录解析，再按路径后缀解析；匹配多个文件的目标
         * 解析到其全部（对包含图过近似只多花一次冗余解析，欠近似则会静默漏掉
         * 脏 TU）。查找大小写不敏感：文件系统（在 Windows 上）如此，而存储的
         * 拼写来自各 TU 各自的包含搜索，两侧差异必须折掉。
         *
         * @internal
         * 增量引擎（cg.graph.build）的包含解析即走本类 resolve()。包含环检测
         * 额外使用 resolve_strict()——同一规则集的无歧义子集——因为上报的环
         * 必须真实，不能是保守扇出的伪影（见 resolve_strict 的说明）。
         */
        class IncludeResolver
        {
        public:
            void add(const std::string& path, cg::db::FileId fid)
            {
                const std::string lower = lowercase(cg::util::norm_path(path));
                by_path_[lower].push_back(fid);
                for (std::size_t i = lower.find('/'); i != std::string::npos;
                     i = lower.find('/', i + 1))
                {
                    by_suffix_[lower.substr(i + 1)].push_back(fid);
                }
            }

            /**
             * @brief 解析一条包含目标（保守版）。
             *
             * @return 全部候选文件 id；目标为空或无命中时为空集。
             *
             * @internal
             * 后缀回退：全路径未命中（定位文件的是包含搜索路径而非包含者
             * 目录）时，依次尝试拼接路径在 '/' 边界上的每个后缀；匹配多个
             * 文件的目标解析到其全部（过近似只多花冗余解析）。
             */
            [[nodiscard]]
            auto resolve(
                std::string_view target,
                std::string_view includer_path) const
                -> std::vector<cg::db::FileId>
            {
                namespace fs = std::filesystem;
                if (target.empty())
                {
                    return {};
                }
                fs::path t{std::string_view{target}};
                if (t.is_relative())
                {
                    fs::path base{std::string_view{includer_path}};
                    base.remove_filename();
                    t = base / t;
                }
                const std::string full = lowercase(
                    cg::util::norm_path(t.lexically_normal().generic_string()));
                if (const auto it = by_path_.find(full); it != by_path_.end())
                {
                    return it->second;
                }
                std::vector<cg::db::FileId> out;
                for (std::size_t i = full.find('/'); i != std::string::npos;
                     i = full.find('/', i + 1))
                {
                    const std::string suffix = full.substr(i + 1);
                    if (suffix.empty())
                    {
                        continue;
                    }
                    if (const auto it = by_suffix_.find(suffix);
                        it != by_suffix_.end())
                    {
                        for (const cg::db::FileId fid : it->second)
                        {
                            cg::util::push_unique(out, fid);
                        }
                    }
                }
                return out;
            }

            /**
             * @brief resolve() 的严格孪生，服务环检测（CI 门禁）。
             *
             * 判定顺序：引号相对目标先对包含者目录（编译器对引号包含先搜当前
             * 目录，该处的精确命中即无歧义事实）；否则以完整包含拼写作路径
             * 后缀——定位文件的是包含搜索路径而非包含者目录——仅当恰好一个
             * 已索引文件携带该后缀时接受。
             *
             * @internal
             * resolve() 会把多匹配后缀扇出到每个候选：对新鲜度检查是对的
             * （冗余解析廉价），对门禁是错的——捏造的边会变成任何源码编辑
             * 都无法消除的幻影环。此处绝不尝试更短的后缀（裸 "logger.h"
             * 拼写可匹配末段恰好折叠为同名的无关文件）。
             */
            struct StrictTarget
            {
                enum class Kind : std::uint8_t
                {
                    None,
                    Unique,
                    Ambiguous
                } kind = Kind::None;
                cg::db::FileId file = 0; // Kind::Unique 时有效
            };

            /**
             * @brief 以严格子集规则解析一条包含目标。
             *
             * @return 无命中为 Kind::None；唯一命中为 Kind::Unique 并携带文件；
             *         多命中为 Kind::Ambiguous（不贡献边）。
             *
             * @internal 对拼写本身做包含搜索路径仿真（完整拼写作后缀键）。
             */
            [[nodiscard]]
            auto resolve_strict(
                std::string_view target,
                std::string_view includer_path,
                bool             quoted) const -> StrictTarget
            {
                namespace fs = std::filesystem;
                if (target.empty())
                {
                    return {};
                }
                fs::path t{std::string_view{target}};
                if (t.is_absolute())
                {
                    const std::string full =
                        lowercase(cg::util::norm_path(t.generic_string()));
                    const auto it = by_path_.find(full);
                    return it == by_path_.end() || it->second.size() != 1
                               ? StrictTarget{}
                               : StrictTarget{
                                     .kind = StrictTarget::Kind::Unique,
                                     .file = it->second.front()};
                }
                if (quoted)
                {
                    fs::path base{std::string_view{includer_path}};
                    base.remove_filename();
                    const std::string joined = lowercase(
                        cg::util::norm_path(
                            (base / t).lexically_normal().generic_string()));
                    const auto it = by_path_.find(joined);
                    if (it != by_path_.end() && it->second.size() == 1)
                    {
                        return {
                            .kind = StrictTarget::Kind::Unique,
                            .file = it->second.front()};
                    }
                }
                const std::string spell = lowercase(
                    cg::util::norm_path(t.lexically_normal().generic_string()));
                const auto it = by_suffix_.find(spell);
                if (it == by_suffix_.end() || it->second.empty())
                {
                    return {};
                }
                return it->second.size() == 1
               ? StrictTarget{.kind = StrictTarget::Kind::Unique,
                              .file = it->second.front()}
               : StrictTarget{.kind = StrictTarget::Kind::Ambiguous};
            }

        private:
            [[nodiscard]]
            static auto lowercase(std::string s) -> std::string
            {
                return cg::util::ascii_lower(s);
            }

            std::unordered_map<std::string, std::vector<cg::db::FileId>>
                by_path_;
            std::unordered_map<std::string, std::vector<cg::db::FileId>>
                by_suffix_;
        };

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
        // 入/出边的共享物化：对端符号查库后连同边行与调用点给定的证据
        // 状态推入对应容器；状态推导（随模式不同）留在各调用点。
        const auto incoming_symbol = [&](const cg::db::EdgeRow& e,
                                         cg::db::SymId          origin,
                                         DispatchStatus         status)
        {
            if (auto other = cg::db::get_symbol(store, e.src))
            {
                out.incoming.push_back(
                    {.other = std::move(*other),
                     .edge = e,
                     .family_origin = origin,
                     .dispatch_status = status});
            }
        };
        const auto outgoing_symbol = [&](const cg::db::EdgeRow& e,
                                         cg::db::SymId          origin,
                                         DispatchStatus         status)
        {
            if (auto other = cg::db::get_symbol(store, e.dst))
            {
                out.outgoing.push_back(
                    {.other = std::move(*other),
                     .edge = e,
                     .family_origin = origin,
                     .dispatch_status = status});
            }
        };
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
                        incoming_symbol(e, origin, status);
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
                        outgoing_symbol(e, origin, status);
                    }
                }
            }
            return out;
        }

        // Precise / Fallback。
        detail::DispatchIndex dispatch{store};
        out.dispatch_stale_files = dispatch.stale_files();
        // 派发行集的公共排序：首键入边取 src、出边取 dst，站点坐标
        // （文件，行）作次键与末键。
        const auto site_less =
            [](const cg::db::EdgeRow& a, const cg::db::EdgeRow& b, bool by_dst)
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
        };
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
                        { return site_less(a, b, /*by_dst=*/false); });
                    for (const cg::db::EdgeRow& e : in_rows)
                    {
                        if (e.confidence >= kDispatchConfidenceFloor)
                        {
                            if (include_incoming)
                            {
                                incoming_symbol(
                                    e,
                                    origin,
                                    DispatchStatus::Inferred);
                            }
                        }
                        else if (
                            mode == DispatchMode::Fallback && include_incoming)
                        {
                            incoming_symbol(
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
                        { return site_less(a, b, /*by_dst=*/true); });
                    for (const cg::db::EdgeRow& e : out_rows)
                    {
                        if (e.confidence >= kDispatchConfidenceFloor)
                        {
                            outgoing_symbol(
                                e,
                                origin,
                                DispatchStatus::Inferred);
                        }
                        else if (mode == DispatchMode::Fallback)
                        {
                            outgoing_symbol(
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
                    incoming_symbol(e, origin, status);
                }
                for (const cg::db::EdgeRow& e :
                     cg::db::edges_of(store, member, /*outgoing=*/true, type))
                {
                    if (type == cg::db::EdgeType::Calls
                        && dispatch.suppresses_calls(e))
                    {
                        continue;
                    }
                    outgoing_symbol(e, origin, status);
                }
            }
        }
        return out;
    }

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
     * id 每库稳定，但路径才是人读的标识）。path_text 中的 MISSING_TEXT
     * 占位实际不可达（每个节点都来自 files 表），仅保住引用返回的
     * 完整性；包含者行已消失的链接直接跳过，不作为环输入。
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
        const auto node_of = [&](cg::db::FileId fid) -> int
        {
            if (const auto found = index_of.find(fid); found != index_of.end())
            {
                return found->second;
            }
            const int idx = static_cast<int>(ids.size());
            ids.push_back(fid);
            index_of.emplace(fid, idx);
            return idx;
        };
        std::vector<std::pair<int, int>> edges_idx;
        edges_idx.reserve(links.size());
        for (const auto& [src, dst] : links)
        {
            edges_idx.emplace_back(node_of(src), node_of(dst));
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

        const auto path_text = [&paths](cg::db::FileId f) -> const std::string&
        {
            static const std::string MISSING_TEXT = "?";
            const auto               it = paths.find(f);
            return it == paths.end() ? MISSING_TEXT : it->second;
        };
        const auto path_less = [&](cg::db::FileId a, cg::db::FileId b)
        {
            const std::string& sa = path_text(a);
            const std::string& sb = path_text(b);
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
