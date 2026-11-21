/**
 * @module cg.analysis.dispatch
 *
 * @brief 虚调用派发解析：override 家族 ∩ 推断动态类型集 → virtual_dispatch
 *        边 + 候选集 + fallback；绝不把不确定的推断伪装成唯一目标。
 *
 * @internal 家族闭包在 overrides 边上做——全量预载进内存邻接图
 * （dispatch_detail::Hierarchy），BFS 不再逐步查库（不导入
 * cg.graph.traverse，避免反向依赖）。nearest-override 用 extends 图的
 * 最短距离：动态类型
 * 自身覆盖优先，其次最近基类覆盖；同距离多候选全部保留。无动态证据
 * 时按家族级 fallback 发 0.0 置信边（attrs 带 dispatch_fallback）。
 * db.tables.h 置于全局模块片段：本单元调用 cg.db 的查询函数，MSVC
 * 要求 sqlpp23 表模型定义在此可见，以合并模块记录的模板特化（完整
 * 机理见 db.tables.h）。
 */
module;
#include "../db/db.tables.h"

export module cg.analysis.dispatch;

import std;
import cg.util;
import cg.db;
import cg.analysis.model;
import cg.analysis.propagate;

export namespace cg::analysis
{

    /** @brief dispatch 解析结果：边 + 回填的调用点解析行。 */
    struct DispatchResult
    {
        std::vector<DispatchEdgeRec>  edges;
        std::vector<ResolvedCallSite> sites;
        std::int64_t                  fallback_sites = 0;
        std::int64_t                  resolved_sites = 0;
    };

    namespace dispatch_detail
    {

        /**
         * @brief 每站点候选集上限。
         *
         * @internal 巨型覆盖族（真实大库上单站点可达数千个目标）的候选 JSON
         * 必须封顶；截断在 call_sites.fallback_reason 中显式标记，绝不静默。
         */
        inline constexpr std::size_t MAX_CANDIDATES = 64;

        /**
         * @brief COM 根层生命周期协议方法名（AddRef / Release /
         *        QueryInterface）。
         *
         * @internal 这三个方法在接口生态库上的 override 族是全库接口
         *           实现类全体（所有接口都 extends 根协议），nearest
         *           收敛护栏在该巨族上恒不收敛——动态类型集一旦被 may
         *           扇投/族写回的假类型污染，站点就整族披露（含接收者
         *           静态类型后代闭包之外的运行时不可能目标）。名字判
         *           据是生态协议约定（协议根接口在库外，无结构锚点可
         *           用），只用于启用接收者可达过滤，不产生任何候选——
         *           判据失效的最坏后果是过滤不启用，回到既有整族披
         *           露。
         */
        inline constexpr std::array<std::string_view, 3> ROOT_LIFECYCLE_METHODS{
            "AddRef",
            "Release",
            "QueryInterface"};

        /** @brief 方法所在记录（parent_id）的 unique_key；无父为空。 */
        [[nodiscard]]
        inline auto owner_key_of(
            const std::unordered_map<std::int64_t, const cg::db::IndexRow*>&
                         by_id,
            std::int64_t id) -> std::string
        {
            const auto it = by_id.find(id);
            if (it == by_id.end() || !it->second->parent_id.has_value())
            {
                return {};
            }
            const auto p = by_id.find(*it->second->parent_id);
            return p == by_id.end() ? std::string{} : p->second->unique_key;
        }

        /**
         * @brief 覆盖家族的一次性预解析缓存（成员表 + 每成员派生量）。
         *
         * @internal 家族只按 method 构建一次（family_of 记忆化），构建时同步
         *           预解析每个成员的 owner id 与 pure 虚位，与成员表下标对齐；
         *           巨族（参考语料单族 3,425 成员）在 nearest 双循环与候选扫描
         *           中按站点重复进入，循环内逐成员做 owner 键字符串构造 +
         *           字符串哈希解析是纯重复成本——缓存后全部走整数索引/哈希快
         *           查，语义与逐次解析完全一致（符号集在解析期只读不变）。
         *           owner_of 供按成员 id 的散点查询（候选 stable_partition
         * 等）， 未命中时调用方回退逐次解析链保底。
         */
        struct FamilyInfo
        {
            /** @brief 家族成员表（override_family 原序，id 升序）。 */
            std::vector<std::int64_t> members;
            /** @brief members 的非纯子序列（保持原序）。 */
            std::vector<std::int64_t> non_pure_members;
            /** @brief 成员 id -> owner id 的散点快查。 */
            std::unordered_map<std::int64_t, std::optional<std::int64_t>>
                owner_of;
            /**
             * @brief 非纯成员 owner 域的稠密位图（符号 id 位，整数位运算）。
             * @internal nearest 覆盖查找的成员资格面：成员命中 ⟺ 其 owner 在
             *           动态类型的祖先距离表中——位图测试替代逐成员 map 查找，
             *           双循环从“全体成员 × 动态类型”降为“动态类型的祖先数 ×
             *           动态类型”（巨族 3,425 成员时祖先侧通常个位数）。位域
             *           惰性增长到本族最大 owner id。
             */
            propagate_detail::SymBits non_pure_owner_bits;
            /**
             * @brief owner id -> 该记录的全部非纯成员下标（位图命中后展开）。
             * @internal 与 non_pure_owner_bits 同步登记（同 owner 多成员时一
             *           并收集，与逐成员扫描的收集集合恒等）。
             */
            std::unordered_map<std::int64_t, std::vector<std::uint32_t>>
                owner_slots;
        };

        /**
         * @brief 静态收窄子树的规模上限（参考语料 105/6271 站点
         *        单子树 >256 节点，巨型接口必须封顶）。
         */
        inline constexpr std::size_t MAX_SUBTREE_NODES = 256;
        /** @brief 子树 BFS 深度上限（与祖先距离上溯上限同源；触顶并入
         * overflow）。 */
        inline constexpr int MAX_SUBTREE_DEPTH = 16;

        /** @brief extends 后代子树（含各根自身，去重并集）与溢出标记。 */
        struct Subtree
        {
            std::unordered_set<std::int64_t> ids;
            bool                             overflow = false;
        };

        /**
         * @brief extends/overrides 全量预载的内存邻接图。
         *
         * @internal 两次 edges_of_type 读入全部 extends/overrides
         * 边（参考语料规模 合计约 6.6k 行）建双向邻接，替代逐站点逐层
         * edges_from/edges_to 小 SQL 的 BFS（6,777 虚站点 ×
         * 每层每节点一条查询，量级 338 万条）。 邻接向量按 id 排序，BFS
         * 展开序不依赖 SQLite 行序——所有查询结果 （家族按 id
         * 排序、距离取各节点首次到达的最小层、子树只消费成员
         * 资格与溢出布尔）均与展开序无关，同一输入同一字节序的契约保持。
         * 边语义与 db 层一致：extends 边 src=派生、dst=基类；overrides 家族
         * 是双向传递闭包。
         */
        class Hierarchy
        {
        public:
            explicit Hierarchy(cg::db::GraphStore& store)
            {
                for (const cg::db::EdgeRow& e :
                     cg::db::edges_of_type(store, cg::db::EdgeType::Overrides))
                {
                    override_fwd_[e.src].push_back(e.dst);
                    override_bwd_[e.dst].push_back(e.src);
                }
                for (const cg::db::EdgeRow& e :
                     cg::db::edges_of_type(store, cg::db::EdgeType::Extends))
                {
                    extends_up_[e.src].push_back(e.dst);
                    extends_down_[e.dst].push_back(e.src);
                }
                // 特化折算邻接（主模板 -> 特化行）：CRTP 模板基形态的
                // extends 边挂在主模板行、特化行无 extends 出边（实现类
                // `class Impl : Base<Impl>` 的直接基是特化行）——只走
                // extends 边的后代闭包对 IDL 生成模式结构性失明。特化
                // 行实例化主模板、继承其全部 extends 基类，后代子树把特
                // 化行折算为结构派生（over-近似，sound 方向：子树变宽只
                // 放宽可达判据、少砍候选）。descendant_subtree 与
                // ancestor_distances（对偶的上行折算）消费；final 判定
                // （has_descendant）保持纯 extends 语义不变（特化不是继
                // 承派生，final 语义不得放宽）。
                for (const cg::db::EdgeRow& e : cg::db::edges_of_type(
                         store,
                         cg::db::EdgeType::Specializes))
                {
                    spec_down_[e.dst].push_back(e.src);
                    spec_fwd_[e.src].push_back(e.dst);
                }
                const auto sort_adj = [](auto& table)
                {
                    for (auto& [id, adj] : table)
                    {
                        std::ranges::sort(adj);
                    }
                };
                sort_adj(override_fwd_);
                sort_adj(override_bwd_);
                sort_adj(extends_up_);
                sort_adj(extends_down_);
                sort_adj(spec_down_);
                sort_adj(spec_fwd_);
            }

            /** @brief 方法的覆盖家族（自身 + overrides 双向传递闭包，按 id
             * 排序）。 */
            [[nodiscard]]
            auto override_family(std::int64_t sym) const
                -> std::vector<std::int64_t>
            {
                std::vector<std::int64_t>        family{sym};
                std::unordered_set<std::int64_t> visited{sym};
                std::vector<std::int64_t>        frontier{sym};
                const auto expand = [&](const auto&                table,
                                        std::int64_t               cur,
                                        std::vector<std::int64_t>& next)
                {
                    const auto it = table.find(cur);
                    if (it == table.end())
                    {
                        return;
                    }
                    for (const std::int64_t other : it->second)
                    {
                        if (visited.insert(other).second)
                        {
                            family.push_back(other);
                            next.push_back(other);
                        }
                    }
                };
                while (!frontier.empty())
                {
                    std::vector<std::int64_t> next;
                    for (const std::int64_t cur : frontier)
                    {
                        expand(override_fwd_, cur, next);
                        expand(override_bwd_, cur, next);
                    }
                    frontier = std::move(next);
                }
                std::ranges::sort(family);
                return family;
            }

            /**
             * @brief 记录类型到其基类的距离（含自身 0），BFS 直到预算上限。
             *
             * @internal 上行方向并行走 extends 与 specializes 正向
             *           （特化行 -> 主模板，spec_fwd_）：CRTP 模板基形态
             *           的 extends 边挂在主模板行上，特化行无 extends 出
             *           边——只走 extends 的祖先链到不了方法宿主所在的
             *           主模板行，nearest_overrides 对该形态结构性零交
             *           集（family_ambiguous 站点中 CRTP 模板主/实例链
             *           断裂带的根因）。特化行实例化
             *           主模板、继承其全部 extends 基类，把特化行折算为
             *           主模板的结构基类（与 descendant_subtree 的下行
             *           折算互为对偶方向）。主模板行上的方法体代表全部
             *           实例化的共同实现（采集层的既有宿主模型），距离
             *           折算不引入家族外目标。
             */
            [[nodiscard]]
            auto ancestor_distances(std::int64_t record) const
                -> std::map<std::int64_t, int>
            {
                std::map<std::int64_t, int> dist;
                std::vector<std::int64_t>   frontier{record};
                dist.emplace(record, 0);
                for (int d = 1; d <= MAX_SUBTREE_DEPTH && !frontier.empty();
                     ++d)
                {
                    std::vector<std::int64_t> next;
                    for (const std::int64_t cur : frontier)
                    {
                        for (const auto* table : {&extends_up_, &spec_fwd_})
                        {
                            const auto it = table->find(cur);
                            if (it == table->end())
                            {
                                continue;
                            }
                            for (const std::int64_t base : it->second)
                            {
                                if (dist.emplace(base, d).second)
                                {
                                    next.push_back(base);
                                }
                            }
                        }
                    }
                    frontier = std::move(next);
                }
                return dist;
            }

            /**
             * @brief 多根 extends 后代子树并集（含 specializes 特化折
             *        算），节点/深度封顶。
             *
             * @internal BFS 沿 extends 边向下（extends_down_ 的对端是派生
             * 类）+ 沿 specializes 反向把主模板的特化行折算为结构派生
             * （spec_down_；机理见构造函数注释）——CRTP 模板基形态的真
             * 实现经 基类 -> 主模板 -> 特化行 -> 实现类 进入子树，接收
             * 者可达判据不再把真实现当闭包外剔除。去重节点数超过
             * MAX_SUBTREE_NODES、或第 MAX_SUBTREE_DEPTH 层边界仍在生长时
             * 置 overflow——两种截断都使候选集不完备，调用方必须回退全
             * 家族并在 fallback_reason 标记，绝不静默丢弃深层目标。
             */
            [[nodiscard]]
            auto descendant_subtree(
                const std::vector<std::int64_t>& roots) const -> Subtree
            {
                Subtree                   out;
                std::vector<std::int64_t> frontier;
                for (const std::int64_t root : roots)
                {
                    if (out.ids.insert(root).second)
                    {
                        frontier.push_back(root);
                    }
                }
                for (int d = 1; d <= MAX_SUBTREE_DEPTH && !frontier.empty();
                     ++d)
                {
                    std::vector<std::int64_t> next;
                    for (const std::int64_t cur : frontier)
                    {
                        for (const auto* table : {&extends_down_, &spec_down_})
                        {
                            const auto it = table->find(cur);
                            if (it == table->end())
                            {
                                continue;
                            }
                            for (const std::int64_t derived : it->second)
                            {
                                if (out.ids.insert(derived).second)
                                {
                                    next.push_back(derived);
                                    if (out.ids.size() > MAX_SUBTREE_NODES)
                                    {
                                        out.overflow = true;
                                        return out;
                                    }
                                }
                            }
                        }
                    }
                    frontier = std::move(next);
                    if (d == MAX_SUBTREE_DEPTH && !frontier.empty())
                    {
                        out.overflow = true;
                    }
                }
                return out;
            }

            /**
             * @brief 记录在 extends 图内是否有（任意）后代。
             *
             * @internal final 判定的图内口径之一（另一口径是 symbols.flags 的
             * is_final 语法位）：extends 缺边时“无 extends 入边”会漏看派生
             * （under-近似），语法 final 位是源码硬证据（实测：final 记录
             * 在 libclang 子游标里暴露 CXXFinalAttr）。两个口径并集消费（见
             * resolve() 的静态决议注释）。
             */
            [[nodiscard]]
            auto has_descendant(std::int64_t record) const -> bool
            {
                const auto it = extends_down_.find(record);
                return it != extends_down_.end() && !it->second.empty();
            }

            /** @brief 记录的 extends 基类邻接（无出边为 nullptr，只读
             *  视图；折算缺边守门的上行链走查用）。 */
            [[nodiscard]]
            auto extends_bases(std::int64_t record) const
                -> const std::vector<std::int64_t>*
            {
                const auto it = extends_up_.find(record);
                return it == extends_up_.end() ? nullptr : &it->second;
            }

            /** @brief 记录是否有 specializes 出边（特化行 -> 主模板；
             *  折算缺边守门的实例行判据用）。 */
            [[nodiscard]]
            auto has_specializes_out(std::int64_t record) const -> bool
            {
                const auto it = spec_fwd_.find(record);
                return it != spec_fwd_.end() && !it->second.empty();
            }

        private:
            /** overrides 正向（src=覆盖者 -> dst=被覆盖者）。 */
            std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                override_fwd_;
            /** overrides 反向。 */
            std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                override_bwd_;
            /** extends 上行（src=派生 -> dst=基类）。 */
            std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                extends_up_;
            /** extends 下行（基类 -> 派生）。 */
            std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                extends_down_;
            /** specializes 下行（主模板 -> 特化行；结构派生折算面，
             *  仅 descendant_subtree 消费）。 */
            std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                spec_down_;
            /** specializes 正向（特化行 -> 主模板；结构基类折算面，
             *  仅 ancestor_distances 消费）。 */
            std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                spec_fwd_;
        };

        /**
         * @brief 早退桶的原因选择形态（EarlyBucketSpec.reason 用）。
         *
         * @internal kFixed = 恒定串；kQualifiedOrText / kResolvedOrText =
         *           原因二选一的桶（判据按站点位读取，与原逐站点分支逐字
         *           相同）。
         */
        enum class EarlyReason : std::uint8_t
        {
            kFixed,
            kQualifiedOrText,
            kResolvedOrText,
        };

        /** @brief 早退桶的表驱动规格（resolve 四桶批量处理的单一事实源）。 */
        struct EarlyBucketSpec
        {
            /** @brief 站点五桶号（classify_site 的下标语义）。 */
            std::size_t bucket;
            /** @brief 原因选择形态。 */
            EarlyReason reason;
            /** @brief kFixed 的原因串 / 二选一形态的否侧串。 */
            const char* text;
            /** @brief 计入 dispatch fallback 计数（no_method 桶）。 */
            bool count_fallback;
        };

        /**
         * @brief 早退四桶表：indirect / not_virtual / qualified / no_method。
         *
         * @internal indirect：函数指针/成员指针/依赖调用——没有家族可解析，
         *           但绝不能不声不响（验收要求原因可读）；间接调用不是虚
         *           派发站点，不计入 dispatch fallback 计数（原因仍完整落在
         *           call_sites.fallback_reason）。not_virtual：非虚调用不做
         *           dispatch（callee_resolved 另行如实上报），限定形态原因
         *           取 qualified_call。no_method：虚被调用者本身未解析
         *           （method_id 为空）与“有被调用者但没有动态类型证据”是
         *           两回事，原因必须可区分。
         */
        inline constexpr std::array<EarlyBucketSpec, 4> EARLY_BUCKETS{{
            {SITE_BUCKET_INDIRECT, EarlyReason::kFixed, "indirect_call", false},
            {SITE_BUCKET_NOT_VIRTUAL,
             EarlyReason::kQualifiedOrText,
             "not_virtual",
             false},
            {SITE_BUCKET_QUALIFIED,
             EarlyReason::kFixed,
             "qualified_call",
             false},
            {SITE_BUCKET_NO_METHOD,
             EarlyReason::kResolvedOrText,
             "no_callee_symbol",
             true},
        }};

    } // namespace dispatch_detail

    /**
     * @brief 对传播状态里的每个调用点产出 dispatch 解析与边。
     *
     * @param store 图数据库句柄（读取符号投影与调用点行）。
     * @param state 传播阶段产出的状态（槽提示、候选与站点集合）。
     * @param shared_rows 预载的符号投影（与 propagate::run 共用一次
     *                    all_symbol_index_rows 全表读；分析期不写符号表，
     *                    两个阶段各读一遍是重复成本）。nullptr 时自行读取。
     * @param all_sites 强制全站点解析（增量形态的升级回退：extends/
     *                  overrides 边集或 `#external` 行集相对上次提交变化时，
     *                  家族/展示面全局重排，任何虚站点都可能变）。增量运行
     *                  默认按 state.site_affected 掩码只解析受影响站点，
     *                  未受影响站点的行与边完全不动（与全量重算逐字节一致
     *                  的前提正是增量等价性本身）。
     */
    [[nodiscard]]
    inline auto resolve(
        cg::db::GraphStore&                  store,
        const PropagationState&              state,
        const std::vector<cg::db::IndexRow>* shared_rows = nullptr,
        bool all_sites = false) -> DispatchResult
    {
        using namespace dispatch_detail;
        DispatchResult out;

        std::vector<cg::db::IndexRow> owned_rows;
        if (shared_rows == nullptr)
        {
            owned_rows = cg::db::all_symbol_index_rows(store);
            shared_rows = &owned_rows;
        }
        const std::vector<cg::db::IndexRow>& rows = *shared_rows;
        std::unordered_map<std::int64_t, const cg::db::IndexRow*> by_id;
        by_id.reserve(rows.size());
        // 透明哈希：调用方持有 string_view 键，直查免逐次构造临时键。
        std::unordered_map<
            std::string,
            std::int64_t,
            cg::util::string_hash,
            std::equal_to<>>
            by_key;
        by_key.reserve(rows.size());
        for (const cg::db::IndexRow& row : rows)
        {
            by_id.emplace(row.id, &row);
            by_key.emplace(row.unique_key, row.id);
        }
        // extends/overrides 全量预载的内存图 + 家族/祖先距离记忆化：
        // 参考语料 distinct methods=1,235 对 6,777 虚站点（重复率 ~5.8），
        // 动态类型同样跨站点重复，缓存把重复闭包归零。
        const Hierarchy                              hierarchy{store};
        std::unordered_map<std::int64_t, FamilyInfo> family_of;
        std::unordered_map<std::int64_t, std::map<std::int64_t, int>> dist_of;
        const auto id_of_key =
            [&](std::string_view key) -> std::optional<std::int64_t>
        {
            const auto it = by_key.find(key);
            if (it == by_key.end())
            {
                return std::nullopt;
            }
            return it->second;
        };
        // 家族成员的 owner 散点快查：命中走缓存（整数哈希），未命中（成员
        // 不在本站点家族缓存——理论不发生，同键多符号的解析还原形态）回
        // 退逐次解析链，判据与缓存前逐字相同。
        const auto owner_of_member =
            [&](const FamilyInfo& fam,
                std::int64_t      member) -> std::optional<std::int64_t>
        {
            const auto it = fam.owner_of.find(member);
            if (it != fam.owner_of.end())
            {
                return it->second;
            }
            return id_of_key(owner_key_of(by_id, member));
        };

        // 前置声明键兜底索引：key 剥文件段（kind|ns|name 三段）-> 候选 id
        // 集。IDL 生态里接口的前置声明形态（聚合头集中前置声明等）不落
        // 符号行，其 key 在符号域 miss——但 ODR 下同名同命名空间同种类记录是
        // 同一实体，定义行承载 extends 闭包。首个 miss 键触发惰性构建（全
        // 投影一次扫描）；三段多候选（多 TU 各挂一行等形态）不猜（宁漏）。
        std::optional<
            std::unordered_map<std::string, std::vector<std::int64_t>>>
                   trimmed_key_ids;
        const auto id_of_static_key =
            [&](const std::string& key) -> std::optional<std::int64_t>
        {
            if (const auto direct = id_of_key(key);
                direct.has_value() && by_id.contains(*direct))
            {
                return direct;
            }
            if (!trimmed_key_ids.has_value())
            {
                trimmed_key_ids.emplace();
                trimmed_key_ids->reserve(rows.size() * 2);
                for (const cg::db::IndexRow& row : rows)
                {
                    const std::string_view k{row.unique_key};
                    const auto             last = k.rfind('|');
                    if (last == std::string_view::npos)
                    {
                        continue;
                    }
                    (*trimmed_key_ids)[std::string{k.substr(0, last)}]
                        .push_back(row.id);
                }
            }
            const std::string_view k{key};
            const auto             last = k.rfind('|');
            if (last == std::string_view::npos)
            {
                return std::nullopt;
            }
            const auto it =
                trimmed_key_ids->find(std::string{k.substr(0, last)});
            if (it == trimmed_key_ids->end() || it->second.size() != 1)
            {
                return std::nullopt; // 三段多候选：不猜
            }
            return it->second.front();
        };

        // 接收者声明面回退的槽级索引：state.hints 的
        // (owner_key, var_name) -> 行。首个命中站点时一次构建——行在槽化
        // 装载后 (owner, var) 唯一（同一槽一行），构建序不影响查询结果；
        // 万级站点只有少数触发本查询，建表成本一次摊销。
        std::optional<std::unordered_map<std::string, const PropagatedHint*>>
                   hint_by_slot;
        const auto declared_receiver_roots =
            [&](const PropagatedSite& site) -> const PropagatedHint*
        {
            if (!hint_by_slot.has_value())
            {
                hint_by_slot.emplace();
                hint_by_slot->reserve(state.hints.size() * 2);
                for (const PropagatedHint& h : state.hints)
                {
                    hint_by_slot->try_emplace(
                        cg::util::compose_key(h.owner_key, h.var_name),
                        &h);
                }
            }
            const auto at =
                [&](const std::string& owner) -> const PropagatedHint*
            {
                const auto it = hint_by_slot->find(
                    cg::util::compose_key(owner, site.receiver_name));
                return it == hint_by_slot->end() ? nullptr : it->second;
            };
            // 与传播期接收者槽定位同序同停（字段声明记录域 → 调用方函数域
            // → 隐式 this 的记录域）：行存在即停、不跨域续找——同名变量的
            // 声明面只认首个定位域（与槽消费同一变量），宁漏勿猜。
            if (!site.receiver_owner_key.empty())
            {
                if (const PropagatedHint* h = at(site.receiver_owner_key))
                {
                    return h;
                }
            }
            if (const PropagatedHint* h = at(site.caller_key))
            {
                return h;
            }
            if (site.receiver_is_this && !site.caller_owner_key.empty())
            {
                if (const PropagatedHint* h = at(site.caller_owner_key))
                {
                    return h;
                }
            }
            return nullptr;
        };

        // 直接成员含 pure virtual 方法的记录（“具体类”判定的图内口径：
        // 只看直接声明，继承的 pure 不追——偏宽的方向被静态决议的唯一
        // 实现守卫兜住）。
        std::unordered_set<std::int64_t> has_pure_member;
        for (const cg::db::IndexRow& row : rows)
        {
            if ((row.flags & cg::db::sym_flags::is_pure_virtual) != 0
                && row.parent_id.has_value())
            {
                has_pure_member.insert(*row.parent_id);
            }
        }

        // 外部边界 RTA 索引（root set 线）：接口 id -> (R(I) 成员 id 集,
        // 边界 reason)。消费契约——可达 ≠ 此站点调它：只做候选排序与
        // fallback_reason 标注；站点级置信严禁，绝不经本索引发边或抬档。
        struct ExternalReach
        {
            std::unordered_set<std::int64_t> ids;
            std::string                      reason;
        };
        std::unordered_map<std::int64_t, ExternalReach> external_of;
        {
            // WHERE 投影直取 R(I) 边界行：行集与
            // all_type_hints 全表读后内存过滤 source==External 且
            // var_name==external_slot 的旧行集逐行相等，免整表物化。
            const std::vector<cg::db::TypeHintRow> hint_rows =
                store.type_hints_of_source(
                    static_cast<std::int64_t>(HintSource::External),
                    external_slot);
            for (const cg::db::TypeHintRow& row : hint_rows)
            {
                ExternalReach& reach = external_of[row.owner_id];
                reach.reason = row.fallback_reason;
                for (const std::string& key : parse_string_array(row.type_set))
                {
                    if (const auto id = id_of_key(key); id.has_value())
                    {
                        reach.ids.insert(*id);
                    }
                }
            }
        }

        // 站点五桶处理（早退判定链的桶级形态）：热列（site_buckets，装载
        // 物化时产出）可用时直接消费，否则按同一判据现场分桶——两条路径
        // 的桶划分逐字一致，行为等价。等价性：out.sites 终态按 site_id 排
        // 序（site_id 全库唯一，处理序不影响行序）；out.edges 仅 virtual
        // 桶产生，终态 stable_sort 并列键（同 caller/target/file/line）的
        // 相对序只由 virtual 桶内的下标序决定（桶内升序 == 原扫描序），
        // 早退桶不发边互不影响；fallback/resolved 计数按桶累加与逐站点
        // 分支等价。
        std::array<std::vector<std::uint32_t>, 5>        local_buckets;
        const std::array<std::vector<std::uint32_t>, 5>* buckets_ptr =
            &local_buckets;
        std::size_t bucket_total = 0;
        for (const auto& b : state.site_buckets)
        {
            bucket_total += b.size();
        }
        if (bucket_total == state.sites.size())
        {
            buckets_ptr = &state.site_buckets;
        }
        else
        {
            // 附加视图缺失（外部手工构造的 state）：现场分桶兜底——判据经
            // classify_site 单点，与装载物化侧同源。
            for (std::size_t idx = 0; idx < state.sites.size(); ++idx)
            {
                local_buckets[classify_site(state.sites[idx])].push_back(
                    static_cast<std::uint32_t>(idx));
            }
        }
        const auto masked_out = [&](std::size_t idx)
        {
            // 未受影响站点：行与边保持既有终态。
            return state.incremental && !all_sites
                   && (idx >= state.site_affected.size()
                       || state.site_affected[idx] == 0);
        };
        const auto make_base = [&](std::size_t idx)
        {
            const PropagatedSite& site = state.sites[idx];
            ResolvedCallSite      resolved;
            resolved.site_id = site.site_id;
            resolved.callee_id = site.callee_id;
            resolved.method_id = site.method_id;
            resolved.method_key = site.method_key;
            resolved.static_type_id =
                site.static_types.empty()
                    ? std::optional<std::int64_t>{}
                    : id_of_key(site.static_types.front());
            std::vector<std::string> receiver_types = site.static_types;
            for (const std::string& t : site.dynamic_types)
            {
                receiver_types.push_back(t);
            }
            std::ranges::sort(receiver_types);
            receiver_types.erase(
                std::unique(receiver_types.begin(), receiver_types.end()),
                receiver_types.end());
            resolved.receiver_types = std::move(receiver_types);
            resolved.callee_resolved = site.callee_resolved;
            return resolved;
        };

        // ---- 提级助手：家族构建 / 最近覆盖查找 / 静态侧决议 ----

        // 家族首建时同步预解析派生量（成员 owner id / 非纯子序列 / 散点
        // 快查表 / 非纯 owner 位图与下标展开表），后续站点与覆盖查找全部
        // 整数索引/位运算。
        const auto family_of_method =
            [&](std::int64_t method) -> const FamilyInfo&
        {
            auto [fam_it, fam_fresh] = family_of.try_emplace(method);
            if (fam_fresh)
            {
                // 家族首建时同步预解析派生量（成员 owner id / 非纯子序列 /
                // 散点快查表 / 非纯 owner 位图与下标展开表），后续站点与
                // 覆盖查找全部整数索引/位运算。
                FamilyInfo& fam = fam_it->second;
                fam.members = hierarchy.override_family(method);
                fam.owner_of.reserve(fam.members.size() * 2);
                for (std::size_t k = 0; k < fam.members.size(); ++k)
                {
                    const std::int64_t                m = fam.members[k];
                    const std::optional<std::int64_t> owner =
                        id_of_key(owner_key_of(by_id, m));
                    const auto mit = by_id.find(m);
                    const bool pure_flag =
                        mit != by_id.end()
                        && (mit->second->flags
                            & cg::db::sym_flags::is_pure_virtual)
                               != 0;
                    fam.owner_of.emplace(m, owner);
                    if (!pure_flag)
                    {
                        fam.non_pure_members.push_back(m);
                        if (owner.has_value())
                        {
                            const std::int64_t o = *owner;
                            fam.non_pure_owner_bits.set(o);
                            fam.owner_slots[o].push_back(
                                static_cast<std::uint32_t>(k));
                        }
                    }
                }
            }
            return fam_it->second;
        };

        // 逐动态类型求自己的最近覆盖，再按 target 取并集——不能用一个
        // 全局最小距离把“一个动态类型有覆盖、另一个无覆盖”压成单一猜测。
        // 家族零交集的缺席类型（键已解析、祖先链上无家族非纯宿主）
        // 记入 absent_ids_out：全部参与且并集后收敛是唯一决议的前置
        // 条件（宁漏勿猜：缺席就不猜）；缺席类型 id 逐个透出（而非仅
        // 计数），供调用方按接收者声明面分类异族噪声与合法缺席（key
        // 解析失败在调用方单独计数，不经过本表）。
        // 查找形态：遍历 dyn 的祖先距离表（规模 = 祖先数，通常个位
        // 数），家族侧用非纯 owner 位图做成员资格测试（位命中 ⟺ 该
        // 记录有家族非纯成员且是 dyn 的祖先），命中祖先经 owner_slots
        // 展开到全部同记录非纯成员——与逐成员 map 查找的收集集合恒
        // 等（成员命中 ⟺ owner 在距离表中；best 取命中祖先距离的最
        // 小值同构），巨族成员数不再是循环量。
        const auto nearest_overrides =
            [&](const FamilyInfo&                family,
                const std::vector<std::int64_t>& types,
                std::vector<std::int64_t>&       absent_ids_out)
            -> std::map<std::int64_t, int>
        {
            std::map<std::int64_t, int> found; // member id -> distance
            for (const std::int64_t dyn : types)
            {
                auto [dist_it, dist_fresh] = dist_of.try_emplace(dyn);
                if (dist_fresh)
                {
                    dist_it->second = hierarchy.ancestor_distances(dyn);
                }
                const std::map<std::int64_t, int>& distances = dist_it->second;
                int best = std::numeric_limits<int>::max();
                for (const auto& [anc, d] : distances)
                {
                    if (family.non_pure_owner_bits.test(anc) && d < best)
                    {
                        best = d;
                    }
                }
                if (best == std::numeric_limits<int>::max())
                {
                    absent_ids_out.push_back(dyn);
                    continue;
                }
                for (const auto& [anc, d] : distances)
                {
                    if (d != best || !family.non_pure_owner_bits.test(anc))
                    {
                        continue;
                    }
                    const auto sit = family.owner_slots.find(anc);
                    if (sit == family.owner_slots.end())
                    {
                        continue;
                    }
                    for (const std::uint32_t k : sit->second)
                    {
                        const auto [pos, fresh] =
                            found.emplace(family.members[k], best);
                        if (!fresh)
                        {
                            pos->second = std::min(pos->second, best);
                        }
                    }
                }
            }
            return found;
        };

        // 接收者声明面排除域（异族豁免的证明面）：全部静态根的 extends
        // 后代闭包并集（descendant_subtree 折算含特化行，与接收者可达
        // 判据同构）。C++ 静态类型系统下，虚调用点接收者的动态类型只能
        // 是某静态根自身或其后代——落在闭包之外的槽类型是可证噪声
        // （并槽路径灌入的界外实现等），不必计入封口缺席。立界失败形
        // 态一律返回 nullptr（调用方保持全缺席阻断，宁漏勿猜）：静态类
        // 型为空、任一键经三段兜底仍解析失败、闭包节点/深度溢出（证
        // 明面不完备时不可主张排除）。模板壳根不跳过——其折算闭包覆
        // 盖全部实例化派生，剔除会收窄排除面（豁免变宽是危险方向）。
        // 按原始键序列 memo：同接收者声明面的多站点共享一次闭包展开。
        std::unordered_map<std::string, std::shared_ptr<const Subtree>>
                   xeno_scope_of;
        const auto xeno_exclusion_scope =
            [&](const std::vector<std::string>& static_types) -> const Subtree*
        {
            std::string memo_key;
            for (const std::string& t : static_types)
            {
                memo_key += t;
                memo_key.push_back('\x1f');
            }
            const auto cached = xeno_scope_of.find(memo_key);
            if (cached != xeno_scope_of.end())
            {
                return cached->second.get();
            }
            std::shared_ptr<const Subtree> scope; // null = 不可立界
            std::vector<std::int64_t>      roots;
            bool                           ok = !static_types.empty();
            for (const std::string& t : static_types)
            {
                const auto sid = id_of_static_key(t);
                if (!sid.has_value())
                {
                    ok = false;
                    break;
                }
                roots.push_back(*sid);
            }
            if (ok)
            {
                Subtree sub = hierarchy.descendant_subtree(roots);
                if (!sub.overflow)
                {
                    scope = std::make_shared<const Subtree>(std::move(sub));
                }
            }
            const Subtree* raw = scope.get();
            xeno_scope_of.emplace(std::move(memo_key), std::move(scope));
            return raw;
        };

        // 折算缺边守门（fold gap）：模板主模板的下行折算闭包经 specializes
        // 边把特化行折算为结构派生——实例行若是本家族主模板的实例、却缺
        // specializes 出边（主模板无定义等形态），其整棵 extends 子树对闭
        // 包结构性失明，“闭包内唯一命中”不可再证。判据（与探针模拟同口径）：
        // 记录行（kind=Record）的键含 '<'（实例形态名）且无 specializes 出
        // 边。缺口行集合一次惰性构建；每起点记录的 extends 上行链（16 层
        // 封顶、只查途经基类）全决议期 memo 缓存。
        std::optional<std::unordered_set<std::int64_t>> fold_gap_rows;
        std::unordered_map<std::int64_t, bool>          fold_gap_chain_of;
        const auto chain_hits_fold_gap = [&](std::int64_t owner) -> bool
        {
            if (!fold_gap_rows.has_value())
            {
                fold_gap_rows.emplace();
                for (const cg::db::IndexRow& row : rows)
                {
                    if (row.kind == cg::db::SymbolKind::Record
                        && row.unique_key.find('<') != std::string::npos
                        && !hierarchy.has_specializes_out(row.id))
                    {
                        fold_gap_rows->insert(row.id);
                    }
                }
            }
            const auto cached = fold_gap_chain_of.find(owner);
            if (cached != fold_gap_chain_of.end())
            {
                return cached->second;
            }
            bool                             gap = false;
            std::unordered_set<std::int64_t> seen{owner};
            std::vector<std::int64_t>        frontier{owner};
            for (int hop = 0;
                 hop < MAX_SUBTREE_DEPTH && !frontier.empty() && !gap;
                 ++hop)
            {
                std::vector<std::int64_t> next;
                for (const std::int64_t cur : frontier)
                {
                    const auto* bases = hierarchy.extends_bases(cur);
                    if (bases == nullptr)
                    {
                        continue;
                    }
                    for (const std::int64_t base : *bases)
                    {
                        if (!seen.insert(base).second)
                        {
                            continue;
                        }
                        if (fold_gap_rows->contains(base))
                        {
                            gap = true;
                            break;
                        }
                        next.push_back(base);
                    }
                    if (gap)
                    {
                        break;
                    }
                }
                frontier = std::move(next);
            }
            fold_gap_chain_of.emplace(owner, gap);
            return gap;
        };

        // 宿主抽象直连的闭包完备性守卫（closure gap）：overrides 家族
        // 闭包以"同签名方法行"为锚——接口经模板中间层继承且中间层不
        // 重声明该方法时，模板实现类（依赖基语境）的 override 边在采
        // 集层结构性缺失，其方法行落不进闭包，"全库唯一非纯实现"成为
        // 观测假象（真库对照实锤：接口 56 个模板实现类的同名行全部闭包
        // 外，闭包内仅剩一个非模板直接实现）。判据（结构可达版，比同
        // 名计数失衡更准）：全库与被调方法同名、闭包外、可作覆盖行的
        // 方法行（带虚位，或宿主带模板位——模板定义语境下虚位与
        // override 边同源丢失，真库对照实证模板实现类方法行只带 const 位）
        // 中，任一行的宿主落在"家族成员宿主 ∪ 被调方法宿主"的折算后
        // 代子树内（extends 下行 + specializes 特化折算）即缺边实锤
        // ——C++ 语义下结构可达 + 同名同虚（同一命名作用域内由 extends
        // 链保证同签名覆盖关系）的行本应在闭包里。子树溢出时证明面不
        // 完备，同样阻断（宁漏勿错）。按方法 memo；同名方法行索引首
        // 个触发守卫的站点一次构建。
        std::optional<
            std::unordered_map<std::string, std::vector<std::int64_t>>>
                                               method_rows_by_name;
        std::unordered_map<std::int64_t, bool> closure_gap_of;
        const auto family_closure_gap = [&](std::int64_t      method,
                                            const FamilyInfo& fam) -> bool
        {
            const auto [gap_it, gap_fresh] = closure_gap_of.try_emplace(method);
            if (!gap_fresh)
            {
                return gap_it->second;
            }
            bool gap = false;
            do
            {
                const auto mrow = by_id.find(method);
                if (mrow == by_id.end())
                {
                    break; // 行缺席：无判据可用，不立缺边
                }
                if (!method_rows_by_name.has_value())
                {
                    method_rows_by_name.emplace();
                    for (const cg::db::IndexRow& row : rows)
                    {
                        if (row.kind == cg::db::SymbolKind::Method)
                        {
                            (*method_rows_by_name)[row.name].push_back(row.id);
                        }
                    }
                }
                const auto name_it =
                    method_rows_by_name->find(mrow->second->name);
                if (name_it == method_rows_by_name->end())
                {
                    break;
                }
                // 成员资格面：闭包成员集合（含纯虚成员）与全部成员宿主。
                std::unordered_set<std::int64_t> member_ids(
                    fam.members.begin(),
                    fam.members.end());
                std::vector<std::int64_t> owners;
                for (const std::int64_t member : fam.members)
                {
                    const auto owner = owner_of_member(fam, member);
                    if (owner.has_value())
                    {
                        owners.push_back(*owner);
                    }
                }
                if (mrow->second->parent_id.has_value())
                {
                    owners.push_back(*mrow->second->parent_id);
                }
                if (owners.empty())
                {
                    break;
                }
                const Subtree reach = hierarchy.descendant_subtree(owners);
                if (reach.overflow)
                {
                    gap = true; // 证明面不完备：保守阻断
                    break;
                }
                for (const std::int64_t peer : name_it->second)
                {
                    if (member_ids.contains(peer))
                    {
                        continue;
                    }
                    const auto prow = by_id.find(peer);
                    if (prow == by_id.end()
                        || !prow->second->parent_id.has_value())
                    {
                        continue;
                    }
                    const bool peer_virtual =
                        (prow->second->flags & cg::db::sym_flags::is_virtual)
                        != 0;
                    bool host_template = false;
                    if (const auto hrow = by_id.find(*prow->second->parent_id);
                        hrow != by_id.end())
                    {
                        host_template = (hrow->second->flags
                                         & cg::db::sym_flags::is_template)
                                        != 0;
                    }
                    if (!peer_virtual && !host_template)
                    {
                        continue; // 非虚同名（隐藏形态）：不是覆盖行
                    }
                    if (reach.ids.contains(*prow->second->parent_id))
                    {
                        gap = true;
                        break;
                    }
                }
            } while (false);
            gap_it->second = gap;
            return gap;
        };

        // 静态侧决议（无动态证据站点）：填充候选面/家族标记边/原因与
        // 决议位；两级 final 直连在成功点早退（其后只剩 no-op 块）。
        const auto resolve_static_site =
            [&](const PropagatedSite&              site,
                const FamilyInfo&                  family,
                const std::optional<std::int64_t>& method,
                std::vector<DispatchCandidate>&    candidates,
                std::vector<DispatchCandidate>&    edge_targets,
                std::string&                       fallback_reason,
                bool&                              static_resolved,
                bool&                              candidates_truncated)
        {
            // ── 静态侧判定（无动态证据的站点）────────────────────────
            //
            // 第一优先：final/具体类 + 家族唯一非纯实现 → 直接决议。
            // 定义：每个接收者静态类型 S 都满足 (a) final：语法 final 位
            // （sym_flags::is_final，源码硬证据）或 extends 图内无后代
            // （图内近似，缺边时漏看派生；C++ final 语义下动态类型只能
            // 是 S 自身）；或 (b) 具体类：S 的直接成员中无 is_pure_virtual
            // （接口/抽象类排除，继承的 pure 不追——偏宽方向由下方唯一
            // 实现守卫兜住）；且 override 家族中恰有 1 个非 pure 实现 m。
            // soundness：动态类型 ∈ {S} ∪ descendants(S)（final 时恰为
            // {S}），虚调用解析到动态类型的最近覆盖且它必非纯虚，而全
            // 家族唯一的非纯成员就是 m——别无选择。静态可证语义按项目
            // 惯例给 High(1.0)（与 DirectNew 同档）。
            // 例外清单（触发失败时逐档下落，绝不伪装唯一目标）：
            // static_types 任一 key 解析失败；家族非纯实现数 != 1；
            // 家族/extends 闭包不完备（漏 overrides 或 extends 边会漏
            // 真实目标——extends 缺边误判 final 时若派生覆盖在家族内
            // 会使非纯数 >= 2 而不触发，只有两类边同时缺失才可能错，
            // 与 nearest-override 0.7 档同源的已声明风险，方向只漏不
            // 假）。
            // 非纯成员表来自家族缓存（构建时与 pure 位同步预筛）。
            const std::vector<std::int64_t>& non_pure_family =
                family.non_pure_members;
            // 静态根清洗：键经三段兜底解析（前置声明键 miss 时剥文件段
            // 唯一匹配，与接收者可达过滤同款先例）；模板壳根（采集期
            // is_template 位的主模板行）跳过——壳是剥 operator-> 后的
            // 语法包装残留，真正的 pointee 静态面已由采集层展开为独立
            // 键，壳自身与目标家族通常零交集，留在根集只会以缺席拖垮
            // 封口（最近覆盖并集已收敛、final/唯一守卫齐备的站点被壳
            // 假缺席压档）。全壳（清洗后无根）不立界：空洞证明面不构
            // 成 sealed 判定，回退既有展示面路径。
            std::vector<std::int64_t> static_roots;
            bool                      roots_ok = !site.static_types.empty();
            for (const std::string& key : site.static_types)
            {
                const auto sid = id_of_static_key(key);
                if (!sid.has_value())
                {
                    roots_ok = false;
                    break;
                }
                const auto row = by_id.find(*sid);
                if (row != by_id.end()
                    && (row->second->flags & cg::db::sym_flags::is_template)
                           != 0)
                {
                    continue; // 模板壳主模板键：无语义证明面，不参与
                }
                static_roots.push_back(*sid);
            }
            roots_ok = roots_ok && !static_roots.empty();
            // 接收者缺位回退（隐式 this）：裸成员调用（QueryInterface
            // 尾部的 AddRef() 形态）没有接收者表达式，receiver_name 与
            // static_types 双空。隐式 this 的静态类型就是调用方所属记
            // 录——按它补唯一的静态根。防误伤四条：接收者在场未解析位
            // （receiver_is_this 的 name 空扩展义）整档不接管——接收者
            // 表达式写在了源码里（出参解引用 `(*pp_out)->AddRef()`、下
            // 标访问、链式调用等）但采集层没解析出名时，接收者对象的动
            // 态类型与调用方所属类没有任何结构关联，拿调用方类当静态根
            // 是"同家族错对象"直连（对抗轮钉死三例），维持披露等真证据；
            // 模板主模板记录不进静态根（不同实例化的动态类型互异，主模
            // 板代表不了任何一族实例——下方专用分支按折算闭包单独决议，
            // 绝不与 has_descendant 纯 extends 口径的 final 判定混流）；
            // 调用方不是成员函数（自由函数/lambda 的所属记录为空）天然
            // 不触发；static_types 已有值时不覆盖（有真接收者证据）。模
            // 板记录不靠拼写或 canonical 判定，直接消费采集期落库的
            // sym_flags::is_template 位。
            bool                        implicit_this_root = false;
            std::optional<std::int64_t> implicit_this_template_id;
            if (!roots_ok && site.static_types.empty()
                && site.receiver_name.empty() && !site.receiver_is_this
                && !site.caller_owner_key.empty())
            {
                const auto sid = id_of_key(site.caller_owner_key);
                if (sid.has_value())
                {
                    const auto row = by_id.find(*sid);
                    if (row != by_id.end())
                    {
                        if ((row->second->flags
                             & cg::db::sym_flags::is_template)
                            == 0)
                        {
                            static_roots.push_back(*sid);
                            roots_ok = true;
                            implicit_this_root = true;
                        }
                        else
                        {
                            implicit_this_template_id = *sid;
                        }
                    }
                }
            }
            // 接收者声明面回退：有名接收者但站点静态
            // 类型空——采集面拿不到声明记录 key（界外/宏产物等形态）
            // 而同定位域的声明行在库时，用接收者槽的声明面类型补静态
            // 根（槽级 static_types = 同域 Declared 行 type_set 的并集，
            // 即变量的声明记录类型）。与隐式 this 回退互斥（receiver_
            // name 空/非空）。防误伤：声明面为空（无 Declared 证据）或
            // 任一 key 解析失败时不补（宁漏勿猜）；补根后仍走下方全部
            // 既有判据（sealed/家族=1/nearest 唯一化与降档原样），多
            // 候选不猜。
            bool receiver_declared_root = false;
            if (!roots_ok && site.static_types.empty()
                && !site.receiver_name.empty())
            {
                const PropagatedHint* declared = declared_receiver_roots(site);
                if (declared != nullptr && !declared->static_types.empty())
                {
                    bool roots_all = true;
                    for (const std::string& key : declared->static_types)
                    {
                        const auto sid = id_of_key(key);
                        if (!sid.has_value())
                        {
                            roots_all = false;
                            break;
                        }
                        static_roots.push_back(*sid);
                    }
                    if (roots_all && !static_roots.empty())
                    {
                        roots_ok = true;
                        receiver_declared_root = true;
                    }
                    else
                    {
                        static_roots.clear();
                    }
                }
            }
            // 语法 final ∪ extends 无后代的并集口径（两个证据方向互补：
            // 语法位不依赖图完备性，图覆盖源码外起点的 extends）。
            const auto is_final_record = [&](std::int64_t sid)
            {
                const auto row = by_id.find(sid);
                return row != by_id.end()
                       && (row->second->flags & cg::db::sym_flags::is_final)
                              != 0;
            };
            bool all_sealed = roots_ok;
            bool all_final = roots_ok;
            if (roots_ok)
            {
                for (const std::int64_t sid : static_roots)
                {
                    if (!hierarchy.has_descendant(sid)
                        || !has_pure_member.contains(sid))
                    {
                        // sealed（final 或具体类）成立，继续看 final。
                    }
                    else
                    {
                        all_sealed = false;
                    }
                    if (!is_final_record(sid) && hierarchy.has_descendant(sid))
                    {
                        all_final = false;
                    }
                    if (!all_sealed && !all_final)
                    {
                        break;
                    }
                }
            }
            if (all_sealed && non_pure_family.size() == 1)
            {
                static_resolved = true;
                candidates.push_back(
                    {.target_key =
                         by_id.at(non_pure_family.front())->unique_key,
                     .target_qualified =
                         by_id.at(non_pure_family.front())->name,
                     .confidence = Confidence::High,
                     .reason =
                         receiver_declared_root ? "receiver_declared_unique"
                         : implicit_this_root   ? "implicit_this_unique"
                                                : "static_unique_override"});
                edge_targets = candidates;
                return; // 第一优先已决议：其后各块对决议站点恒 no-op
            }
            else if (roots_ok)
            {
                // 第 1.5 优先（进入条件放宽为有可用静态根；类级与方法
                // 级 final 证据在此两级判定）：先按 nearest-override 语
                // 义求各根最近覆盖的并集（两级判定共享，只算一次）。
                // 类级 all_final：全部静态根都是 final 记录（语法位或
                // 图内无后代）→ 动态类型恰为各根自身（库内家族=1 的
                // 收窄面），不必要求全家族唯一实现——并集恰一个目标
                // 时静态可证唯一（High(1.0)，与第一优先同档；reason
                // 区分隐式 this 回退与显式静态类型两种来源）。
                // 方法级接力（类级证据缺失时）：并集恰一个成员且该成
                // 员符号自带方法级 final 语法位（sym_flags::is_final 位
                // 的 method 宿主侧）→ 该成员是各接收者静态链上的最近
                // 覆盖，final 由编译器强制封死其下的一切真实覆盖（源
                // 码语法硬证据，与 extends/overrides 图完备性无关——
                // 图缺边只导致目标缺席不发，不可能误指别的成员）→ 动
                // 态类型的最近覆盖恒为该成员，直连 High(1.0)（reason=
                // static_final_method 系列，隐式 this/声明面回退来源按
                // 第一优先惯例区分命名）。类级与方法级证据的直连目标
                // 恒同：两规则是封印证据的并集而非竞争关系。
                // 同距离多覆盖（多继承菱形）或任一根在家族内无覆盖
                // （absent）一律不猜：发 Low 展示候选并保持 0.0 家族
                // 标记边（宁漏勿猜）。
                std::vector<std::int64_t>         static_absent_ids;
                const std::map<std::int64_t, int> static_nearest =
                    nearest_overrides(family, static_roots, static_absent_ids);
                const bool static_unique =
                    static_absent_ids.empty() && static_nearest.size() == 1;
                const auto member_row =
                    static_unique ? by_id.find(static_nearest.begin()->first)
                                  : by_id.end();
                const bool member_final =
                    static_unique && member_row != by_id.end()
                    && (member_row->second->flags & cg::db::sym_flags::is_final)
                           != 0;
                if (member_row != by_id.end() && (all_final || member_final))
                {
                    static_resolved = true;
                    const char* reason = nullptr;
                    if (all_final)
                    {
                        reason = receiver_declared_root
                                     ? "receiver_declared_unique"
                                 : implicit_this_root ? "implicit_this_unique"
                                                      : "static_final_unique";
                    }
                    else
                    {
                        reason = receiver_declared_root
                                     ? "receiver_declared_final_method"
                                 : implicit_this_root
                                     ? "implicit_this_final_method"
                                     : "static_final_method";
                    }
                    candidates.push_back(
                        {.target_key = member_row->second->unique_key,
                         .target_qualified = member_row->second->name,
                         .confidence = Confidence::High,
                         .reason = reason});
                    edge_targets = candidates;
                    return; // 直连已决议：其后各块恒 no-op
                }
                else if (all_final)
                {
                    for (const auto& [member, dist] : static_nearest)
                    {
                        (void)dist;
                        if (candidates.size() >= MAX_CANDIDATES)
                        {
                            candidates_truncated = true;
                            break;
                        }
                        candidates.push_back(
                            {.target_key = by_id.at(member)->unique_key,
                             .target_qualified = by_id.at(member)->name,
                             .confidence = Confidence::Low,
                             .reason = "no_dynamic_type_evidence"});
                    }
                    if (!candidates.empty())
                    {
                        fallback_reason = "no_dynamic_type_evidence;"
                                          "static_narrowed";
                    }
                }
                else
                {
                    // 第二优先：DTA 静态声明子树收窄：候选面 = 家族 ∩
                    // (各静态类型 extends 后代子树的并集)，排除 method
                    // 自身；空集 守卫回退全家族；子树溢出（>256 节点或 16
                    // 层未收 敛）同样回退全家族并标 subtree_overflow。收窄只
                    // 改展示面：候选置信 Low(0.4)、fallback_reason 标
                    // static_narrowed，不发推断边——0.4 档不构成派发
                    // 证据，0.0 家族标记边保持不变。
                    const Subtree subtree =
                        hierarchy.descendant_subtree(static_roots);
                    if (!subtree.overflow)
                    {
                        for (const std::int64_t member : non_pure_family)
                        {
                            if (member == *method)
                            {
                                continue;
                            }
                            const auto owner = owner_of_member(family, member);
                            if (!owner.has_value()
                                || !subtree.ids.contains(*owner))
                            {
                                continue;
                            }
                            if (candidates.size() >= MAX_CANDIDATES)
                            {
                                candidates_truncated = true;
                                break;
                            }
                            candidates.push_back(
                                {.target_key = by_id.at(member)->unique_key,
                                 .target_qualified = by_id.at(member)->name,
                                 .confidence = Confidence::Low,
                                 .reason = "no_dynamic_type_evidence"});
                        }
                        if (!candidates.empty())
                        {
                            fallback_reason =
                                "no_dynamic_type_evidence;static_narrowed";
                        }
                    }
                    else
                    {
                        fallback_reason =
                            "no_dynamic_type_evidence;subtree_overflow";
                    }
                }
            }
            // ── 隐式 this 的模板主模板专用分支 ────────────────────────
            //
            // CRTP 模板基（派生类 : AcmeBase<派生类>）方法体内
            // 的裸虚调用：调用方所属记录是主模板行（方法宿主全部挂在主模
            // 板，实例行不重复挂成员），主模板行代表不了任何一族实例的动
            // 态类型——第一/1.5 优先的静态根路径不适用（has_descendant 是
            // 纯 extends 口径，看不见特化行后代，主模板会被误判 final 而
            // 假阳性直连）。专用判据改用 descendant_subtree 的折算闭包
            // （extends 下行 + specializes 下行复合）：候选面 = 家族非纯
            // 成员中 owner 落在 [主模板] 折算闭包内的那些——闭包把主模板
            // 的全部实例化行及其 extends 派生折算为结构可达，特化侧不覆
            // 盖时闭包内唯一非纯成员就是主模板自己的共享实现。
            // 恰 1 且闭包无溢出 → High(1.0) 直连（与 implicit_this_unique
            // 对称）；多于 1 → Low(0.4) 披露命中集（收窄只改展示面，0.0
            // 家族标记边保持）；0 或溢出 → 维持家族级 fallback 原状。
            // 唯一命中另加折算缺边守门：家族闭包外非纯成员的 owner 走
            // extends 上行链，途经缺口实例行（键含 '<' 且无 specializes
            // 出边——该行若是本主模板的实例，其子树成员进不了折算闭包）
            // 时不可再证唯一，降 0.4 披露（fold_gap）。守门后直连的残余
            // 风险与第一优先同构：只有 extends 与 specializes 两类边同
            // 时缺失才可能误连。
            if (implicit_this_template_id.has_value() && !static_resolved
                && candidates.empty())
            {
                const Subtree subtree =
                    hierarchy.descendant_subtree({*implicit_this_template_id});
                std::vector<std::int64_t> hits;
                if (!subtree.overflow)
                {
                    for (const std::int64_t member : non_pure_family)
                    {
                        const auto owner = owner_of_member(family, member);
                        if (owner.has_value() && subtree.ids.contains(*owner))
                        {
                            hits.push_back(member);
                        }
                    }
                }
                if (subtree.overflow)
                {
                    fallback_reason =
                        "no_dynamic_type_evidence;subtree_overflow";
                }
                else if (hits.size() == 1)
                {
                    bool fold_gap = false;
                    for (const std::int64_t member : non_pure_family)
                    {
                        const auto owner = owner_of_member(family, member);
                        if (!owner.has_value() || subtree.ids.contains(*owner))
                        {
                            continue;
                        }
                        if (chain_hits_fold_gap(*owner))
                        {
                            fold_gap = true;
                            break;
                        }
                    }
                    if (!fold_gap)
                    {
                        static_resolved = true;
                        candidates.push_back(
                            {.target_key = by_id.at(hits.front())->unique_key,
                             .target_qualified = by_id.at(hits.front())->name,
                             .confidence = Confidence::High,
                             .reason = "implicit_this_template_unique"});
                        edge_targets = candidates;
                        return; // 折算闭包唯一命中已决议
                    }
                    // 缺口在案：唯一性不可证——降 0.4 披露，绝不直连。
                    candidates.push_back(
                        {.target_key = by_id.at(hits.front())->unique_key,
                         .target_qualified = by_id.at(hits.front())->name,
                         .confidence = Confidence::Low,
                         .reason = "no_dynamic_type_evidence"});
                    fallback_reason = "no_dynamic_type_evidence;"
                                      "implicit_this_template_fold_gap";
                }
                else if (!hits.empty())
                {
                    for (const std::int64_t member : hits)
                    {
                        if (candidates.size() >= MAX_CANDIDATES)
                        {
                            candidates_truncated = true;
                            break;
                        }
                        candidates.push_back(
                            {.target_key = by_id.at(member)->unique_key,
                             .target_qualified = by_id.at(member)->name,
                             .confidence = Confidence::Low,
                             .reason = "no_dynamic_type_evidence"});
                    }
                    fallback_reason = "no_dynamic_type_evidence;"
                                      "implicit_this_template_narrowed";
                }
            }
            // ── 宿主抽象直连：method 纯虚 × 全库唯一非纯 override ──
            //
            // 对象存在性论证：被调方法自带纯虚语法位 ⟹ 其宿主抽象、
            // 不可能实例化 ⟹ 任何运行时动态类型必为可实例化的后代且
            // 其最近覆盖非纯（覆盖带纯虚位会使宿主同样抽象），而全库
            // 家族恰有一个非纯成员——别无选择，静态可证 High(1.0)。
            // 纯虚位是采集落库的源码语法硬证据，兼作数据质量闸：声明
            // 符号缺纯虚位（默认体/override 二选一静态不可分）的形态
            // 不触发，宁漏勿错。外部边界守卫：接收者静态类型命中
            // `#external` 行（用户显式声明实现可达集在库外）时不开闸
            // ——全库唯一只是库内观测，边界接口的实现在索引外是既有
            // 知识而非缺边近似，直连会与可达性纪律冲突。置于隐式 this
            // 模板分支之后：CRTP 主模板调用方先走折算闭包专用判定；
            // 此前各优先已填的展示候选被直连覆盖（展示面不是决议面）。
            // 两道结构守卫（复验补强，缺一即挡、绝不伪装唯
            // 一）：(1) 闭包完备性——模板中间层缺边形态（family_closure_
            // gap）下"全库唯一"是闭包观测假象，直连即同家族错对象；
            // (2) 接收者间接形态——下标元素（"#elem:" 槽名）与接收者
            // 表达式在场未解析（name 空 + 在场位）形态上接收者对象与
            // 调用方/库内唯一实现无结构关联论证可用，历史误连簇全部
            // 落在这两种形态上，短期排除回披露等真证据。挡下时
            // fallback_reason 如实披露挡因（绝不静默）。
            bool receiver_external = false;
            for (const std::string& key : site.static_types)
            {
                const auto sid = id_of_key(key);
                if (sid.has_value() && external_of.contains(*sid))
                {
                    receiver_external = true;
                    break;
                }
            }
            const bool receiver_indirect_form =
                site.receiver_name.starts_with("#elem:")
                || (site.receiver_name.empty() && site.receiver_is_this);
            if (!static_resolved && !receiver_external && method.has_value()
                && non_pure_family.size() == 1)
            {
                const auto mrow = by_id.find(*method);
                if (mrow != by_id.end()
                    && (mrow->second->flags
                        & cg::db::sym_flags::is_pure_virtual)
                           != 0)
                {
                    const bool closure_gap =
                        family_closure_gap(*method, family);
                    if (receiver_indirect_form || closure_gap)
                    {
                        // 守卫挡下：维持披露，挡因如实追加（已有展示
                        // 面披露串不覆盖，绝不静默）。
                        const std::string_view guard_tag =
                            closure_gap ? "family_closure_gap"
                                        : "receiver_indirect_form";
                        fallback_reason =
                            fallback_reason.empty()
                                ? std::format(
                                      "no_dynamic_type_evidence;{}",
                                      guard_tag)
                                : std::format(
                                      "{};{}",
                                      fallback_reason,
                                      guard_tag);
                    }
                    else
                    {
                        static_resolved = true;
                        candidates.clear();
                        edge_targets.clear();
                        candidates.push_back(
                            {.target_key =
                                 by_id.at(non_pure_family.front())->unique_key,
                             .target_qualified =
                                 by_id.at(non_pure_family.front())->name,
                             .confidence = Confidence::High,
                             .reason = "family_unique_override"});
                        edge_targets = candidates;
                    }
                }
            }
            if (!static_resolved && candidates.empty())
            {
                // 家族级 fallback：每站点只发一条 0.0 标记边（目标是静态
                // 命中的成员），全部候选留在 call_sites.candidates。这样
                // 未解析站点在巨型族上不会爆炸成一条边/候选。
                for (const std::int64_t member : non_pure_family)
                {
                    if (member == *method)
                    {
                        continue;
                    }
                    if (candidates.size() >= MAX_CANDIDATES)
                    {
                        candidates_truncated = true;
                        break;
                    }
                    candidates.push_back(
                        {.target_key = by_id.at(member)->unique_key,
                         .target_qualified = by_id.at(member)->name,
                         .confidence = Confidence::Fallback,
                         .reason = "no_dynamic_type_evidence"});
                }
                if (fallback_reason.empty())
                {
                    fallback_reason = "no_dynamic_type_evidence";
                }
            }
            if (!static_resolved)
            {
                edge_targets.push_back(
                    {.target_key = by_id.at(*method)->unique_key,
                     .target_qualified = by_id.at(*method)->name,
                     .confidence = Confidence::Fallback,
                     .reason = "no_dynamic_type_evidence"});
            }

            // ── 外部边界 RTA（排序 + 标注，不砍候选不发边）──────────
            //
            // 激活 = 接收者静态类型命中 `#external` 行（SWIG 导出/
            // director/插件注册表/dlopen 边界接口）。可达 ≠ 此站点调
            // 它：hit>1 只把命中 R(I) 的候选稳定排前；hit==1 只展示唯一
            // 候选（Low 展示档）也不发边（0.0 家族标记边保持不变）；
            // hit==0 是"无库内实例化实例"的诚实信号——插件 DLL 实现在
            // 索引外（逐点位语料实证 3/30），保留全家族并标注实现可能
            // 索引外，严禁发空候选。站点级置信严禁：本块不动
            // edge_targets、不抬任何候选置信。
            const ExternalReach* reach = nullptr;
            for (const std::string& key : site.static_types)
            {
                const auto it = external_of.find(id_of_key(key).value_or(-1));
                if (it != external_of.end())
                {
                    reach = &it->second;
                    break;
                }
            }
            if (reach != nullptr && !static_resolved)
            {
                std::vector<std::int64_t> hit_members;
                for (const std::int64_t member : non_pure_family)
                {
                    const auto owner = owner_of_member(family, member);
                    if (owner.has_value() && reach->ids.contains(*owner))
                    {
                        hit_members.push_back(member);
                    }
                }
                if (!hit_members.empty())
                {
                    std::ranges::stable_partition(
                        candidates,
                        [&](const DispatchCandidate& c)
                        {
                            const auto member = id_of_key(c.target_key);
                            return member.has_value()
                                   && reach->ids.contains(
                                       owner_of_member(family, *member)
                                           .value_or(-1));
                        });
                    if (hit_members.size() == 1)
                    {
                        // 只展示唯一候选：可达性证据不足以发边
                        // （"可达≠此站点调它"），收缩展示面到该成员。
                        candidates.clear();
                        candidates.push_back(
                            {.target_key =
                                 by_id.at(hit_members.front())->unique_key,
                             .target_qualified =
                                 by_id.at(hit_members.front())->name,
                             .confidence = Confidence::Low,
                             .reason = "external_rta_narrowed"});
                    }
                    if (!fallback_reason.empty())
                    {
                        fallback_reason += ';';
                    }
                    fallback_reason =
                        std::format("{}external_rta_narrowed", fallback_reason);
                }
                else
                {
                    // hit==0：实现可能索引外，保全家族并如实标注。
                    if (!fallback_reason.empty())
                    {
                        fallback_reason += ';';
                    }
                    fallback_reason = std::format(
                        "{}external_no_instantiation",
                        fallback_reason);
                }
            }
        };

        // 早退四桶：桶级批量（固定 fallback_reason；原因二选一的桶按站点
        // 位读取，判据与原逐站点分支逐字相同）；表序即处理序。
        for (const EarlyBucketSpec& spec : EARLY_BUCKETS)
        {
            for (const std::uint32_t idx : (*buckets_ptr)[spec.bucket])
            {
                if (masked_out(idx))
                {
                    continue;
                }
                ResolvedCallSite resolved = make_base(idx);
                resolved.resolved = false;
                resolved.confidence = Confidence::Fallback;
                switch (spec.reason)
                {
                case EarlyReason::kFixed:
                    resolved.fallback_reason = spec.text;
                    break;
                case EarlyReason::kQualifiedOrText:
                    resolved.fallback_reason = state.sites[idx].qualified
                                                   ? "qualified_call"
                                                   : spec.text;
                    break;
                case EarlyReason::kResolvedOrText:
                    resolved.fallback_reason = state.sites[idx].callee_resolved
                                                   ? "no_dynamic_type_evidence"
                                                   : spec.text;
                    break;
                }
                out.sites.push_back(std::move(resolved));
                if (spec.count_fallback)
                {
                    ++out.fallback_sites;
                }
            }
        }
        // virtual 桶：主路径（桶判据保证 indirect/非虚/限定/无 method 的
        // 早退全部不在桶内）。
        for (const std::uint32_t idx : (*buckets_ptr)[SITE_BUCKET_VIRTUAL])
        {
            const PropagatedSite& site = state.sites[idx];
            if (masked_out(idx))
            {
                continue;
            }
            ResolvedCallSite resolved = make_base(idx);

            std::vector<DispatchCandidate> candidates; // call_sites.candidates
            std::vector<DispatchCandidate> edge_targets; // virtual_dispatch 边
            Confidence                     site_conf = Confidence::Fallback;
            std::string                    fallback_reason;
            bool                           candidates_truncated = false;
            // final/具体类静态决议是否触发（!has_dynamic 分支内设置）：
            // resolved 与 1.0 推断边以它为准，而不是 has_dynamic。
            bool static_resolved = false;

            // 桶判据（SITE_BUCKET_VIRTUAL）保证 method_id 有值。
            const std::optional<std::int64_t> method = site.method_id;

            const FamilyInfo& family = family_of_method(*method);
            // 缺席细分两桶：key 解析失败（数据质量可疑，恒阻断，不参与
            // 任何豁免）与家族零交集（键已解析，逐类型透出供异族分类）。
            std::size_t               key_miss = 0;
            std::vector<std::int64_t> dynamic_ids;
            dynamic_ids.reserve(site.dynamic_types.size());
            for (const std::string& key : site.dynamic_types)
            {
                const auto dyn = id_of_key(key);
                if (!dyn.has_value())
                {
                    ++key_miss;
                    continue;
                }
                dynamic_ids.push_back(*dyn);
            }
            std::vector<std::int64_t> absent_ids;       // 家族零交集的缺席类型
            const std::map<std::int64_t, int> nearest = // member id -> distance
                nearest_overrides(family, dynamic_ids, absent_ids);
            const std::size_t absent = key_miss + absent_ids.size();
            // 最近覆盖收敛不降级：全部动态类型都参与解析
            // （absent==0）且并集后恰一个最近覆盖——多类型不等于必然模糊，
            // 收敛到同一实现时按单覆盖同档给 Medium(0.7)。封口用槽级
            // dynamic_types.size()<32（截断代理），不用全局 budget_exhausted
            // 阻断（实测大库的 meta=1 由别的槽触发，会清零收益且与单类型档
            // 待遇不一致）。恰 32 个未截断类型的站点被保守放弃：漏报而非
            // 误报，符合“绝不把不确定的推断伪装成唯一目标”的纪律方向。
            const bool nearest_converged = absent == 0 && nearest.size() == 1
                                           && site.dynamic_types.size() < 32;
            // ── 异族豁免封口：缺席全部可证噪声时的条件提档 ─────────────
            //
            // 四闸合取（按真库锚证，任一不满足维持弱带 0.4）：
            // (a) 异族豁免——缺席类型全部落在接收者声明面排除域之外
            //     （extends 后代闭包可证无关；接口名缺席是合法声明面
            //     的合法缺席——其正解是家族闭包展开后参与收敛，保持
            //     阻断），key miss 恒阻断；
            // (b) 截断硬闸——槽截断观测位（含传染）为真时，槽类型集
            //     不完整的站点上“唯一收敛”可能是假象，除非家族全库
            //     唯一（唯一守卫不依赖槽观测完整性：任何可实例化动态
            //     类型的最近覆盖必为那个唯一非纯成员）；
            // (c) final 封口——唯一目标的方法级或宿主类级 final 语法
            //     位封死其下一切真实覆盖（漏观测类型只能是 final 目标
            //     的覆盖者，不可能存在），无 final 时同样要求家族全库
            //     唯一兜底。
            // 档位复用 Medium(0.7)（查询侧消费面与既有收敛档等价），
            // reason 后缀 nearest_converged_filtered 区分披露来源。
            bool nearest_converged_filtered = false;
            if (!absent_ids.empty() && key_miss == 0 && nearest.size() == 1)
            {
                const bool family_unique = family.non_pure_members.size() == 1;
                if (family_unique || !site.slot_type_truncated)
                {
                    std::size_t    legit_absent = absent_ids.size();
                    const Subtree* excl =
                        xeno_exclusion_scope(site.static_types);
                    if (excl != nullptr)
                    {
                        legit_absent = 0;
                        for (const std::int64_t absent_type : absent_ids)
                        {
                            if (excl->ids.contains(absent_type))
                            {
                                ++legit_absent;
                            }
                        }
                    }
                    if (legit_absent == 0 || family_unique)
                    {
                        const std::int64_t target = nearest.begin()->first;
                        const auto         trow = by_id.find(target);
                        bool member_sealed = trow != by_id.end()
                                             && (trow->second->flags
                                                 & cg::db::sym_flags::is_final)
                                                    != 0;
                        if (!member_sealed)
                        {
                            const auto owner = owner_of_member(family, target);
                            if (owner.has_value())
                            {
                                const auto orow = by_id.find(*owner);
                                member_sealed =
                                    orow != by_id.end()
                                    && (orow->second->flags
                                        & cg::db::sym_flags::is_final)
                                           != 0;
                            }
                        }
                        nearest_converged_filtered =
                            member_sealed || family_unique;
                    }
                }
            }
            Confidence  edge_conf = worse(site.evidence, site.resolution);
            std::string edge_reason =
                site.reason.empty() ? std::string{"no_dynamic_type_evidence"}
                                    : site.reason;
            if (nearest_converged)
            {
                edge_conf = better(edge_conf, Confidence::Medium);
                edge_reason = std::format("{}_nearest_converged", edge_reason);
            }
            else if (nearest_converged_filtered)
            {
                edge_conf = better(edge_conf, Confidence::Medium);
                edge_reason =
                    std::format("{}_nearest_converged_filtered", edge_reason);
            }

            for (const auto& [member, dist] : nearest)
            {
                (void)dist;
                if (candidates.size() >= MAX_CANDIDATES)
                {
                    candidates_truncated = true;
                    break;
                }
                candidates.push_back(
                    {.target_key = by_id.at(member)->unique_key,
                     .target_qualified = by_id.at(member)->name,
                     .confidence = edge_conf,
                     .reason = edge_reason});
            }

            // COM 根层巨族的接收者可达过滤：AddRef/Release/QueryInterface
            // 的 override 族是全库接口实现全体（见 ROOT_LIFECYCLE_METHODS
            // 注释），nearest 收敛护栏在巨族上恒不收敛；动态类型集被 may
            // 扇投/族写回的假类型污染时，多候选里混有接收者静态类型
            // extends 后代闭包之外的运行时不可能目标。整族披露形态（多
            // 候选）下按接收者静态可达闭包收窄：静态根经三段键兜底解析
            // （前置声明键 miss 时剥文件段唯一匹配，模板壳根无语义面、
            // 跳过）且子树不溢出才立界；全滤空回退全候选（可达判据自身
            // 可能有 extends 缺边盲区，宁保持披露）；收窄发生时候选
            // reason 追加后缀披露（绝不静默）。已收敛唯一
            // （nearest_converged）或单候选站点不启用——收敛护栏已工作，
            // 不添乱。
            if (candidates.size() > 1
                && std::ranges::find(
                       ROOT_LIFECYCLE_METHODS,
                       std::string_view{site.callee.name})
                       != ROOT_LIFECYCLE_METHODS.end())
            {
                std::vector<std::int64_t> recv_roots;
                bool                      roots_ok = !site.static_types.empty();
                for (const std::string& key : site.static_types)
                {
                    const auto sid = id_of_static_key(key);
                    if (!sid.has_value())
                    {
                        roots_ok = false;
                        break;
                    }
                    const auto row = by_id.find(*sid);
                    if (row != by_id.end()
                        && (row->second->flags & cg::db::sym_flags::is_template)
                               != 0)
                    {
                        continue; // 模板壳主模板键：无语义面，不参与证明
                    }
                    recv_roots.push_back(*sid);
                }
                if (roots_ok && !recv_roots.empty())
                {
                    const Subtree scope =
                        hierarchy.descendant_subtree(recv_roots);
                    if (!scope.overflow)
                    {
                        std::vector<DispatchCandidate> scoped;
                        scoped.reserve(candidates.size());
                        for (const DispatchCandidate& c : candidates)
                        {
                            const auto member = id_of_key(c.target_key);
                            const auto owner =
                                member.has_value()
                                    ? owner_of_member(family, *member)
                                    : std::optional<std::int64_t>{};
                            if (owner.has_value() && scope.ids.contains(*owner))
                            {
                                scoped.push_back(c);
                            }
                        }
                        if (!scoped.empty()
                            && scoped.size() < candidates.size())
                        {
                            for (DispatchCandidate& c : scoped)
                            {
                                c.reason += ";receiver_scope_narrowed";
                            }
                            candidates = std::move(scoped);
                        }
                    }
                }
            }

            // ── 出参族并集弱带 × 密封静态面的静态优先 ─────────────────
            //
            // 出参族并集（out_param_family）是"装填接口各实现写进同位
            // 出参槽的类型并集"——may 超集证据（槽档位 Low）；其静态
            // 面裁剪带对称守卫（全批面外整体不裁，防闭包缺边饿死真决
            // 议），污染路径就藏在守卫的放行面里：装填族从不写接收者
            // 声明类型时，整批界外类型照投，槽动态证据与接收者的
            // final/具体类声明面正面冲突（真库对照实锤：声明 final 类的直
            // 连被并集收敛的界外实现压档错连）。冲突裁决：密封静态面
            // （final 语法位或图内无后代 + 直接成员无纯虚的具体类口
            // 径，与 resolve_static_site 第一优先同判据）是源码硬证据，
            // 槽并集是传播近似——静态优先。三条件合取（宁漏勿猜）：
            // 站点动态证据携带族并集弱带标记（槽 reason 含
            // out_param_family——Medium 直连/direct_new 等强证据形态
            // 不抢班）；静态根全部解析（三段兜底 + 壳清洗，与静态决议
            // 同款）且全部密封；动态类型与声明面折算后代闭包求交为空
            // （全部是可证界外污染——含任一面内类型或键解析失败时守
            // 卫不动）。触发即弃动态侧已填候选，走静态决议全阶梯
            // （final 直连或诚实披露）。
            bool static_face_priority = false;
            if (!site.dynamic_types.empty()
                && site.reason.find("out_param_family") != std::string::npos
                && !site.static_types.empty())
            {
                std::vector<std::int64_t> priority_roots;
                bool                      priority_ok = true;
                for (const std::string& key : site.static_types)
                {
                    const auto sid = id_of_static_key(key);
                    if (!sid.has_value())
                    {
                        priority_ok = false;
                        break;
                    }
                    const auto row = by_id.find(*sid);
                    if (row != by_id.end()
                        && (row->second->flags & cg::db::sym_flags::is_template)
                               != 0)
                    {
                        continue; // 模板壳主模板键：无语义证明面
                    }
                    priority_roots.push_back(*sid);
                }
                priority_ok = priority_ok && !priority_roots.empty();
                if (priority_ok)
                {
                    bool all_sealed = true;
                    for (const std::int64_t sid : priority_roots)
                    {
                        if (hierarchy.has_descendant(sid)
                            && has_pure_member.contains(sid))
                        {
                            all_sealed = false;
                            break;
                        }
                    }
                    if (all_sealed)
                    {
                        const Subtree face =
                            hierarchy.descendant_subtree(priority_roots);
                        if (!face.overflow && key_miss == 0)
                        {
                            bool face_hit = false;
                            for (const std::int64_t dyn : dynamic_ids)
                            {
                                if (face.ids.contains(dyn))
                                {
                                    face_hit = true;
                                    break;
                                }
                            }
                            static_face_priority = !face_hit;
                        }
                    }
                }
            }
            if (static_face_priority)
            {
                // 撤销动态侧 nearest 候选与截断标记：静态决议接管全部
                // 展示面与决议位。
                candidates.clear();
                candidates_truncated = false;
            }
            // ── 转型声称弱带 × 静态硬证明的静态优先 ──────────────────
            //
            // 声称槽（reason 词根 cast_target）的动
            // 态证据是程序员的转型声称（Low 档弱带），静态侧能硬证唯一
            // （final/具体类/宿主抽象 直连 High(1.0)）时静态优先——声称不
            // 得把静态直连压成 0.7。静态不决议（static_narrowed/外部边界
            // 关闸等形态）时维持声称候选：声称面对无证据带是纯增益。与
            // 出参族并集的 static_face_priority 同构（弱带动态 × 硬证明
            // 静态 → 静态接管），但触发域不同（槽 reason 词根判定，声称
            // 槽绝无 out_param_family 后缀，两门互斥）。
            bool cast_claim_static_priority = false;
            if (!site.dynamic_types.empty() && !static_face_priority
                && site.reason.starts_with("cast_target"))
            {
                std::vector<DispatchCandidate> shadow_candidates;
                std::vector<DispatchCandidate> shadow_edges;
                std::string                   shadow_fallback;
                bool                          shadow_resolved = false;
                bool                          shadow_truncated = false;
                resolve_static_site(
                    site,
                    family,
                    method,
                    shadow_candidates,
                    shadow_edges,
                    shadow_fallback,
                    shadow_resolved,
                    shadow_truncated);
                if (shadow_resolved)
                {
                    // 静态直连接管展示面与决议位；声称候选整批让位。
                    candidates = std::move(shadow_candidates);
                    edge_targets = std::move(shadow_edges);
                    fallback_reason = std::move(shadow_fallback);
                    candidates_truncated = shadow_truncated;
                    static_resolved = true;
                    cast_claim_static_priority = true;
                }
            }
            const bool has_dynamic = !site.dynamic_types.empty()
                                     && !static_face_priority
                                     && !cast_claim_static_priority;
            if (has_dynamic && candidates.empty())
            {
                // 有动态类型却没有任何家族成员匹配：如实降级到静态命中
                // 成员的家族级 0.0 标记边，绝不伪造具体目标。
                fallback_reason = "family_ambiguous";
                edge_targets.push_back(
                    {.target_key = by_id.at(*method)->unique_key,
                     .target_qualified = by_id.at(*method)->name,
                     .confidence = Confidence::Fallback,
                     .reason = "family_ambiguous"});
            }
            if (has_dynamic && edge_targets.empty())
            {
                edge_targets = candidates;
            }
            else if (!has_dynamic && !static_resolved)
            {
                resolve_static_site(
                    site,
                    family,
                    method,
                    candidates,
                    edge_targets,
                    fallback_reason,
                    static_resolved,
                    candidates_truncated);
            }

            // 多类型路径映射到同一 target：合并为一条，置信度取最高。
            std::ranges::sort(
                edge_targets,
                [](const DispatchCandidate& a, const DispatchCandidate& b)
                { return a.target_key < b.target_key; });
            std::vector<DispatchCandidate> merged;
            for (const DispatchCandidate& c : edge_targets)
            {
                if (!merged.empty() && merged.back().target_key == c.target_key)
                {
                    if (c.confidence < merged.back().confidence)
                    {
                        merged.back().confidence = c.confidence;
                        merged.back().reason = c.reason;
                    }
                    continue;
                }
                merged.push_back(c);
            }
            edge_targets = std::move(merged);

            resolved.resolved =
                !candidates.empty() && (has_dynamic || static_resolved);
            resolved.candidates = candidates;
            if (candidates.empty())
            {
                site_conf = Confidence::Fallback;
            }
            else
            {
                site_conf = candidates.front().confidence;
                for (const DispatchCandidate& c : candidates)
                {
                    site_conf = better(site_conf, c.confidence);
                }
            }
            resolved.confidence = site_conf;
            if (resolved.resolved)
            {
                resolved.fallback_reason =
                    candidates_truncated ? std::string{"candidates_truncated"}
                                         : std::string{};
            }
            else
            {
                resolved.fallback_reason = fallback_reason;
                if (candidates_truncated)
                {
                    resolved.fallback_reason = std::format(
                        "{};candidates_truncated",
                        resolved.fallback_reason);
                }
            }
            // 接收者槽截断观测（契约见 PropagatedSite::slot_type_truncated
            // 注释）：本站点动态类型证据可能不完整，如实追加后缀；仅主路
            // 径消费，四条早退路径不经过此处天然不加；既有串非空才补分号，
            // 避免 resolved 且无其他标记时出现前导分号串。
            if (site.slot_type_truncated)
            {
                if (!resolved.fallback_reason.empty())
                {
                    resolved.fallback_reason += ';';
                }
                resolved.fallback_reason = std::format(
                    "{}slot_type_truncated",
                    resolved.fallback_reason);
            }
            if (resolved.resolved)
            {
                ++out.resolved_sites;
            }
            else
            {
                ++out.fallback_sites;
            }

            for (const DispatchCandidate& c : edge_targets)
            {
                std::uint32_t attrs = cg::db::edge_attrs::dispatch_member
                                      | cg::db::edge_attrs::dispatch_virtual;
                if (c.confidence == Confidence::Fallback)
                {
                    attrs |= cg::db::edge_attrs::dispatch_fallback;
                }
                else
                {
                    attrs |= cg::db::edge_attrs::dispatch_inferred;
                }
                out.edges.push_back(
                    {.caller_key = site.caller_key,
                     .target_key = c.target_key,
                     .site_file = site.file,
                     .site_line = site.line,
                     .attrs = attrs,
                     .confidence = confidence_value(c.confidence),
                     .reason = c.reason});
            }
            out.sites.push_back(std::move(resolved));
        }

        // 确定性：同一输入同一字节序。
        std::ranges::stable_sort(
            out.edges,
            [](const DispatchEdgeRec& a, const DispatchEdgeRec& b)
            {
                return std::tie(
                           a.caller_key,
                           a.target_key,
                           a.site_file,
                           a.site_line)
                       < std::tie(
                           b.caller_key,
                           b.target_key,
                           b.site_file,
                           b.site_line);
            });
        std::ranges::stable_sort(
            out.sites,
            [](const ResolvedCallSite& a, const ResolvedCallSite& b)
            { return a.site_id < b.site_id; });
        return out;
    }

} // namespace cg::analysis
