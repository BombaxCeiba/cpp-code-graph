/**
 * @module cg.analysis.dispatch.graph
 *
 * @brief 虚调用派发解析的图与家族域：extends/overrides/specializes
 *        全量预载的内存邻接图、覆盖家族闭包与最近覆盖并集查找。
 *
 * @internal 家族闭包在 overrides 边上做——全量预载进内存邻接图
 *           （dispatch_detail::Hierarchy），BFS 不再逐步查库（不导入
 *           cg.graph.traverse，避免反向依赖）。nearest-override 用
 *           extends 图的最短距离：动态类型自身覆盖优先，其次最近
 *           基类覆盖；同距离多候选全部保留。db.tables.h 置于全局
 *           模块片段：本单元调用 cg.db 的查询函数，MSVC 要求
 *           sqlpp23 表模型定义在此可见，以合并模块记录的模板特化
 *           （完整机理见 db.tables.h）。
 */
module;
#include "../db/db.tables.h"

export module cg.analysis.dispatch.graph;

import std;
import cg.db;
import cg.analysis.propagate;

export namespace cg::analysis
{

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
         * @brief 邻接表的逐键值向量排序。
         *
         * @internal BFS 展开序不依赖 SQLite 行序（详见 Hierarchy 类注
         *           释），装表后统一排序一次达成。
         */
        inline void sort_adjacency(
            std::unordered_map<std::int64_t, std::vector<std::int64_t>>& table)
        {
            for (auto& [id, adj] : table)
            {
                std::ranges::sort(adj);
            }
        }

        /**
         * @brief 覆盖家族闭包的单侧单点展开（overrides 一翼）。
         *
         * @internal 首见节点同时进家族表与下层 frontier；visited 是闭
         *           包成员资格的唯一事实源，已见节点不再入队。
         */
        inline void expand_family_side(
            const std::unordered_map<std::int64_t, std::vector<std::int64_t>>&
                                              table,
            std::int64_t                      cur,
            std::unordered_set<std::int64_t>& visited,
            std::vector<std::int64_t>&        family,
            std::vector<std::int64_t>&        next)
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
        }

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
                sort_adjacency(override_fwd_);
                sort_adjacency(override_bwd_);
                sort_adjacency(extends_up_);
                sort_adjacency(extends_down_);
                sort_adjacency(spec_down_);
                sort_adjacency(spec_fwd_);
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
                while (!frontier.empty())
                {
                    std::vector<std::int64_t> next;
                    for (const std::int64_t cur : frontier)
                    {
                        expand_family_side(
                            override_fwd_,
                            cur,
                            visited,
                            family,
                            next);
                        expand_family_side(
                            override_bwd_,
                            cur,
                            visited,
                            family,
                            next);
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

        /** @brief nearest 查找的产物面（最近覆盖并集与缺席类型）。 */
        struct NearestUnions
        {
            /** @brief member id -> 最近覆盖距离（按 target 取并集）。 */
            std::map<std::int64_t, int> nearest;
            /** @brief 家族零交集的缺席类型 id（键已解析）。 */
            std::vector<std::int64_t> absent_ids;
        };

        /**
         * @brief 逐动态类型求自己的最近覆盖，再按 target 取并集。
         *
         * @internal 不能用一个全局最小距离把“一个动态类型有覆盖、
         *           另一个无覆盖”压成单一猜测。家族零交集的缺席类型
         *           （键已解析、祖先链上无家族非纯宿主）记入返回值
         *           的 absent_ids：全部参与且并集后收敛是唯一决议的
         *           前置条件（宁漏勿猜：缺席就不猜）；缺席类型 id 逐
         *           个透出（而非仅计数），供调用方按接收者声明面分类
         *           异族噪声与合法缺席（key 解析失败在调用方单独计
         *           数，不经过本表）。查找形态：遍历 dyn 的祖先距离
         *           表（规模 = 祖先数，通常个位数），家族侧用非纯
         *           owner 位图做成员资格测试（位命中 ⟺ 该记录有家族
         *           非纯成员且是 dyn 的祖先），命中祖先经 owner_slots
         *           展开到全部同记录非纯成员——与逐成员 map 查找的
         *           收集集合恒等（成员命中 ⟺ owner 在距离表中；best
         *           取命中祖先距离的最小值同构），巨族成员数不再是
         *           循环量。dist_memo 为跨调用共享的祖先距离缓存
         *           （首查回填，重复类型复用）。
         */
        [[nodiscard]]
        inline auto nearest_overrides(
            const FamilyInfo&                family,
            const std::vector<std::int64_t>& types,
            const Hierarchy&                 hierarchy,
            std::unordered_map<std::int64_t, std::map<std::int64_t, int>>&
                dist_memo) -> NearestUnions
        {
            NearestUnions out;
            for (const std::int64_t dyn : types)
            {
                auto [dist_it, dist_fresh] = dist_memo.try_emplace(dyn);
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
                    out.absent_ids.push_back(dyn);
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
                            out.nearest.emplace(family.members[k], best);
                        if (!fresh)
                        {
                            pos->second = std::min(pos->second, best);
                        }
                    }
                }
            }
            return out;
        }

    } // namespace dispatch_detail

} // namespace cg::analysis
