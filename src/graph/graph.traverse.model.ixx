/**
 * @module cg.graph.traverse.model
 *
 * @brief
 * 走查域的导出类型：方向/风险/派发策略枚举、邻域/影响/轨迹/闭包/模块图/上下文/变更/环的结果载荷。
 */
export module cg.graph.traverse.model;

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
} // namespace cg::graph
