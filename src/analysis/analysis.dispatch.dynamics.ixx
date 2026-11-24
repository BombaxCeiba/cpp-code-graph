/**
 * @module cg.analysis.dispatch.dynamics
 *
 * @brief 虚调用派发解析的动态侧证据域：动态类型最近覆盖、异族豁免
 *        提档、COM 根层收窄、弱带静态优先接管与多目标合并。
 *
 * @internal 动态侧证据（槽提示的动态类型集）在此折算为展示候选与
 *           置信档；弱带动态证据（出参族并集、转型声称）与静态硬
 *           证明冲突时静态优先。逐档判据见各函数注释。
 */
export module cg.analysis.dispatch.dynamics;

import std;
import cg.db;
import cg.analysis.model;
import cg.analysis.propagate;
import cg.analysis.dispatch.graph;
import cg.analysis.dispatch.context;
import cg.analysis.dispatch.statics;

export namespace cg::analysis
{

    namespace dispatch_detail
    {

        /** @brief 动态侧最近覆盖的事实面（并集、缺席与收敛位）。 */
        struct DynamicFace
        {
            /** @brief 解析失败的动态类型键数（数据质量可疑，恒阻断）。 */
            std::size_t key_miss = 0;
            /** @brief 解析成功的动态类型 id。 */
            std::vector<std::int64_t> dynamic_ids;
            /**
             * @brief 家族零交集的缺席类型（键已解析）。
             * @internal 逐个透出（而非仅计数），供异族豁免按接收者
             *           声明面分类异族噪声与合法缺席。
             */
            std::vector<std::int64_t> absent_ids;
            /** @brief member id -> 最近覆盖距离。 */
            std::map<std::int64_t, int> nearest;
            /** @brief 全部参与且并集唯一（槽级截断代理 <32 封口）。 */
            bool nearest_converged = false;
        };

        /**
         * @brief 缺席细分两桶并求最近覆盖并集。
         *
         * @internal key 解析失败（数据质量可疑，恒阻断，不参与任何
         *           豁免）与家族零交集（键已解析，逐类型透出供异族
         *           分类）。最近覆盖收敛不降级：全部动态类型都参与
         *           解析（absent==0）且并集后恰一个最近覆盖——多类
         *           型不等于必然模糊，收敛到同一实现时按单覆盖同档
         *           给 Medium(0.7)。封口用槽级 dynamic_types.size()<32
         *           （截断代理），不用全局 budget_exhausted 阻断（大
         *           库的 meta=1 由别的槽触发，会清零收益且与单类型档
         *           待遇不一致）。恰 32 个未截断类型的站点被保守放
         *           弃：漏报而非误报，符合"绝不把不确定的推断伪装成
         *           唯一目标"的纪律方向。
         */
        [[nodiscard]]
        inline auto dynamic_face_of(
            ResolveContext&       ctx,
            const PropagatedSite& site,
            const FamilyInfo&     family) -> DynamicFace
        {
            DynamicFace face;
            face.dynamic_ids.reserve(site.dynamic_types.size());
            for (const std::string& key : site.dynamic_types)
            {
                const auto dyn = ctx.id_of_key(key);
                if (!dyn.has_value())
                {
                    ++face.key_miss;
                    continue;
                }
                face.dynamic_ids.push_back(*dyn);
            }
            const NearestUnions unions = nearest_overrides(
                family,
                face.dynamic_ids,
                ctx.hierarchy,
                ctx.dist_of);
            face.absent_ids = std::move(unions.absent_ids);
            face.nearest = std::move(unions.nearest);
            const std::size_t absent = face.key_miss + face.absent_ids.size();
            face.nearest_converged = absent == 0 && face.nearest.size() == 1
                                     && site.dynamic_types.size() < 32;
            return face;
        }

        /**
         * @brief 异族豁免封口：缺席全部可证噪声时的条件提档。
         *
         * @internal 四闸合取（任一不满足维持弱带 0.4）：(a) 异族豁
         *           免——缺席类型全部落在接收者声明面排除域之外
         *           （extends 后代闭包可证无关；接口名缺席是合法声
         *           明面的合法缺席——其正解是家族闭包展开后参与收
         *           敛，保持阻断），key miss 恒阻断；(b) 截断硬闸
         *           ——槽截断观测位（含传染）为真时，槽类型集不完
         *           整的站点上"唯一收敛"可能是假象，除非家族全库唯
         *           一（唯一守卫不依赖槽观测完整性：任何可实例化动
         *           态类型的最近覆盖必为那个唯一非纯成员）；(c)
         *           final 封口——唯一目标的方法级或宿主类级 final
         *           语法位封死其下一切真实覆盖（漏观测类型只能是
         *           final 目标的覆盖者，不可能存在），无 final 时同
         *           样要求家族全库唯一兜底。档位复用 Medium(0.7)
         *           （查询侧消费面与既有收敛档等价），reason 后缀
         *           nearest_converged_filtered 区分披露来源。
         *
         * @return 是否提档（nearest_converged_filtered）。
         */
        [[nodiscard]]
        inline bool xeno_exemption_lifts(
            ResolveContext&       ctx,
            const PropagatedSite& site,
            const FamilyInfo&     family,
            const DynamicFace&    face)
        {
            if (face.absent_ids.empty() || face.key_miss != 0
                || face.nearest.size() != 1)
            {
                return false;
            }
            const bool family_unique = family.non_pure_members.size() == 1;
            if (!family_unique && site.slot_type_truncated)
            {
                return false;
            }
            std::size_t    legit_absent = face.absent_ids.size();
            const Subtree* excl = ctx.xeno_exclusion_scope(site.static_types);
            if (excl != nullptr)
            {
                legit_absent = 0;
                for (const std::int64_t absent_type : face.absent_ids)
                {
                    if (excl->ids.contains(absent_type))
                    {
                        ++legit_absent;
                    }
                }
            }
            if (legit_absent != 0 && !family_unique)
            {
                return false;
            }
            const std::int64_t target = face.nearest.begin()->first;
            const auto         trow = ctx.by_id.find(target);
            bool               member_sealed =
                trow != ctx.by_id.end()
                && (trow->second->flags & cg::db::sym_flags::is_final) != 0;
            if (!member_sealed)
            {
                const auto owner = ctx.owner_of_member(family, target);
                if (owner.has_value())
                {
                    const auto orow = ctx.by_id.find(*owner);
                    member_sealed =
                        orow != ctx.by_id.end()
                        && (orow->second->flags & cg::db::sym_flags::is_final)
                               != 0;
                }
            }
            return member_sealed || family_unique;
        }

        /**
         * @brief 用最近覆盖并集填充展示候选（边置信与 reason 前缀在
         *        此定档）。
         *
         * @internal 边置信 = min(站点证据, 站点决议) 再与收敛提档取
         *           高；reason 空时以 no_dynamic_type_evidence 兜底，
         *           收敛/过滤档追加后缀区分披露来源。候选封顶
         *           MAX_CANDIDATES，截断置位绝不静默。
         */
        inline void fill_dynamic_candidates(
            ResolveContext&       ctx,
            const PropagatedSite& site,
            const DynamicFace&    face,
            bool                  filtered,
            StaticSiteOutcome&    out)
        {
            Confidence  edge_conf = worse(site.evidence, site.resolution);
            std::string edge_reason =
                site.reason.empty() ? std::string{"no_dynamic_type_evidence"}
                                    : site.reason;
            if (face.nearest_converged)
            {
                edge_conf = better(edge_conf, Confidence::Medium);
                edge_reason = std::format("{}_nearest_converged", edge_reason);
            }
            else if (filtered)
            {
                edge_conf = better(edge_conf, Confidence::Medium);
                edge_reason =
                    std::format("{}_nearest_converged_filtered", edge_reason);
            }
            for (const auto& [member, dist] : face.nearest)
            {
                (void)dist;
                if (out.candidates.size() >= MAX_CANDIDATES)
                {
                    out.candidates_truncated = true;
                    break;
                }
                push_candidate(
                    out.candidates,
                    ctx,
                    member,
                    edge_conf,
                    edge_reason);
            }
        }

        /**
         * @brief COM 根层巨族的接收者可达过滤。
         *
         * @internal AddRef/Release/QueryInterface 的 override 族是
         *           全库接口实现全体（见 ROOT_LIFECYCLE_METHODS 注
         *           释），nearest 收敛护栏在巨族上恒不收敛；动态类型
         *           集被 may 扇投/族写回的假类型污染时，多候选里混
         *           有接收者静态类型 extends 后代闭包之外的运行时不
         *           可能目标。整族披露形态（多候选）下按接收者静态
         *           可达闭包收窄：静态根经三段键兜底解析（前置声明
         *           键 miss 时剥文件段唯一匹配，模板壳根无语义面、
         *           跳过）且子树不溢出才立界；全滤空回退全候选（可
         *           达判据自身可能有 extends 缺边盲区，宁保持披
         *           露）；收窄发生时候选 reason 追加后缀披露（绝不
         *           静默）。已收敛唯一（nearest_converged）或单候选
         *           站点不启用——收敛护栏已工作，不添乱。
         */
        inline void narrow_root_lifecycle(
            ResolveContext&       ctx,
            const PropagatedSite& site,
            const FamilyInfo&     family,
            StaticSiteOutcome&    out)
        {
            if (out.candidates.size() <= 1
                || std::ranges::find(
                       ROOT_LIFECYCLE_METHODS,
                       std::string_view{site.callee.name})
                       == ROOT_LIFECYCLE_METHODS.end())
            {
                return;
            }
            std::vector<std::int64_t> recv_roots;
            bool                      roots_ok = !site.static_types.empty();
            for (const std::string& key : site.static_types)
            {
                const auto sid = ctx.id_of_static_key(key);
                if (!sid.has_value())
                {
                    roots_ok = false;
                    break;
                }
                const auto row = ctx.by_id.find(*sid);
                if (row != ctx.by_id.end()
                    && (row->second->flags & cg::db::sym_flags::is_template)
                           != 0)
                {
                    continue; // 模板壳主模板键：无语义面，不参与证明
                }
                recv_roots.push_back(*sid);
            }
            if (!roots_ok || recv_roots.empty())
            {
                return;
            }
            const Subtree scope = ctx.hierarchy.descendant_subtree(recv_roots);
            if (scope.overflow)
            {
                return;
            }
            std::vector<DispatchCandidate> scoped;
            scoped.reserve(out.candidates.size());
            for (const DispatchCandidate& c : out.candidates)
            {
                const auto member = ctx.id_of_key(c.target_key);
                const auto owner = member.has_value()
                                       ? ctx.owner_of_member(family, *member)
                                       : std::optional<std::int64_t>{};
                if (owner.has_value() && scope.ids.contains(*owner))
                {
                    scoped.push_back(c);
                }
            }
            if (scoped.empty() || scoped.size() == out.candidates.size())
            {
                return;
            }
            for (DispatchCandidate& c : scoped)
            {
                c.reason += ";receiver_scope_narrowed";
            }
            out.candidates = std::move(scoped);
        }

        /**
         * @brief 出参族并集弱带 × 密封静态面的静态优先判定。
         *
         * @internal 出参族并集（out_param_family）是"装填接口各实现
         *           写进同位出参槽的类型并集"——may 超集证据（槽档
         *           位 Low）；其静态面裁剪带对称守卫（全批面外整体
         *           不裁，防闭包缺边饿死真决议），污染路径就藏在守
         *           卫的放行面里：装填族从不写接收者声明类型时，整
         *           批界外类型照投，槽动态证据与接收者的 final/具体
         *           类声明面正面冲突（声明 final 类的直连会被并集收
         *           敛的界外实现压档错连）。冲突的取舍：密封静态面
         *           （final 语法位或图内无后代 + 直接成员无纯虚的
         *           具体类口径，与静态决议第一优先同判据）是源码硬
         *           证据，槽并集是传播近似——静态优先。三条件合取
         *           （宁漏勿猜）：站点动态证据携带族并集弱带标记
         *           （槽 reason 含 out_param_family——Medium 直连/
         *           direct_new 等强证据形态不抢班）；静态根全部解析
         *           （三段兜底 + 壳清洗，与静态决议同款）且全部密
         *           封；动态类型与声明面折算后代闭包求交为空（全部
         *           是可证界外污染——含任一面内类型或键解析失败时
         *           守卫不动）。触发即弃动态侧已填候选，走静态决议
         *           全阶梯（final 直连或诚实披露）。
         *
         * @return 是否静态优先接管（接管时动态候选已清空）。
         */
        [[nodiscard]]
        inline bool apply_static_face_priority(
            ResolveContext&       ctx,
            const PropagatedSite& site,
            const DynamicFace&    face,
            StaticSiteOutcome&    out)
        {
            bool priority = false;
            if (!site.dynamic_types.empty()
                && site.reason.find("out_param_family") != std::string::npos
                && !site.static_types.empty())
            {
                std::vector<std::int64_t> priority_roots;
                bool                      priority_ok = true;
                for (const std::string& key : site.static_types)
                {
                    const auto sid = ctx.id_of_static_key(key);
                    if (!sid.has_value())
                    {
                        priority_ok = false;
                        break;
                    }
                    const auto row = ctx.by_id.find(*sid);
                    if (row != ctx.by_id.end()
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
                        if (ctx.hierarchy.has_descendant(sid)
                            && ctx.has_pure_member.contains(sid))
                        {
                            all_sealed = false;
                            break;
                        }
                    }
                    if (all_sealed)
                    {
                        const Subtree face_tree =
                            ctx.hierarchy.descendant_subtree(priority_roots);
                        if (!face_tree.overflow && face.key_miss == 0)
                        {
                            bool face_hit = false;
                            for (const std::int64_t dyn : face.dynamic_ids)
                            {
                                if (face_tree.ids.contains(dyn))
                                {
                                    face_hit = true;
                                    break;
                                }
                            }
                            priority = !face_hit;
                        }
                    }
                }
            }
            if (priority)
            {
                // 撤销动态侧 nearest 候选与截断标记：静态决议接管全部
                // 展示面与决议位。
                out.candidates.clear();
                out.candidates_truncated = false;
            }
            return priority;
        }

        /**
         * @brief 转型声称弱带 × 静态硬证明的静态优先判定。
         *
         * @internal 声称槽（reason 词根 cast_target）的动态证据是程
         *           序员的转型声称（Low 档弱带），静态侧能硬证唯一
         *           （final/具体类/宿主抽象直连 High(1.0)）时静态优
         *           先——声称不得把静态直连压成 0.7。静态不决议
         *           （static_narrowed/外部边界关闸等形态）时维持声称
         *           候选：声称面对无证据带是纯增益。与出参族并集的
         *           static_face_priority 同构（弱带动态 × 硬证明静
         *           态 → 静态接管），但触发域不同（槽 reason 词根判
         *           定，声称槽绝无 out_param_family 后缀，两门互
         *           斥）。
         *
         * @return 是否静态直连接管（接管时出参聚集合整体替换为影子
         *         决议结果）。
         */
        [[nodiscard]]
        inline bool apply_cast_claim_priority(
            ResolveContext&                    ctx,
            const PropagatedSite&              site,
            const FamilyInfo&                  family,
            const std::optional<std::int64_t>& method,
            bool                               face_priority,
            StaticSiteOutcome&                 out)
        {
            if (site.dynamic_types.empty() || face_priority
                || !site.reason.starts_with("cast_target"))
            {
                return false;
            }
            StaticSiteOutcome shadow;
            resolve_static_site(ctx, site, family, method, shadow);
            if (!shadow.static_resolved)
            {
                return false;
            }
            // 静态直连接管展示面与决议位；声称候选整批让位。
            out = std::move(shadow);
            return true;
        }

        /**
         * @brief 多类型路径映射到同一 target 的合并：排序去重，置信
         *        度取最高。
         *
         * @internal 同 target 多条候选并一条——置信度取最高（枚举值
         *           小者为高），reason 随胜出置信度走。
         */
        inline void merge_multi_type_targets(StaticSiteOutcome& out)
        {
            std::ranges::sort(
                out.edge_targets,
                [](const DispatchCandidate& a, const DispatchCandidate& b)
                { return a.target_key < b.target_key; });
            std::vector<DispatchCandidate> merged;
            for (const DispatchCandidate& c : out.edge_targets)
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
            out.edge_targets = std::move(merged);
        }

    } // namespace dispatch_detail

} // namespace cg::analysis
