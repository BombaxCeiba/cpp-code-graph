/**
 * @module cg.analysis.dispatch.statics
 *
 * @brief 虚调用派发解析的静态决议域：根清洗与两级回退、final/唯一
 *        直连阶梯、隐式 this 模板折算闭包、宿主抽象直连、家族级
 *        fallback 带与外部边界 RTA 标注。
 *
 * @internal 静态侧决议入口 resolve_static_site 按六段优先阶梯短路，
 *           每个成功档位早退，其后各段对决议站点恒 no-op；逐档判据
 *           与 reason 串语义见各函数注释。db.tables.h 置于全局模块
 *           片段：MSVC 要求 sqlpp23 表模型定义在此可见，以合并模块
 *           记录的模板特化（完整机理见 db.tables.h）。
 */
module;
#include "../db/db.tables.h"

export module cg.analysis.dispatch.statics;

import std;
import cg.db;
import cg.analysis.model;
import cg.analysis.propagate;
import cg.analysis.dispatch.graph;
import cg.analysis.dispatch.context;

export namespace cg::analysis
{

    namespace dispatch_detail
    {

        /**
         * @brief 静态根清洗结果（根清洗 + 两个回退 + 密封/final 判定
         *        的产物）。
         */
        struct StaticRoots
        {
            /** @brief 清洗后的静态根 id 集（模板壳已剔除）。 */
            std::vector<std::int64_t> roots;
            /** @brief 是否立界成功（有可用静态根）。 */
            bool ok = false;
            /** @brief 全部根 sealed（final 或具体类）。 */
            bool all_sealed = false;
            /** @brief 全部根 final（语法位或图内无后代）。 */
            bool all_final = false;
            /** @brief 根来自隐式 this 回退。 */
            bool implicit_this_root = false;
            /** @brief 隐式 this 回退命中模板主模板行（不进根集）。 */
            std::optional<std::int64_t> implicit_this_template_id;
            /** @brief 根来自接收者声明面回退。 */
            bool receiver_declared_root = false;
        };

        /**
         * @brief 站点出参聚集合（展示候选 / 推断边目标 / 原因串与截
         *        断位 / 决议位）。
         *
         * @internal static_resolved 置位表示静态硬证明接管决议位：
         *           resolved 与 1.0 推断边以它为准，而不是
         *           has_dynamic。
         */
        struct StaticSiteOutcome
        {
            /** @brief 展示候选（call_sites.candidates）。 */
            std::vector<DispatchCandidate> candidates;
            /** @brief 推断边目标（virtual_dispatch 边）。 */
            std::vector<DispatchCandidate> edge_targets;
            /** @brief 原因串（未决议站点的逐段披露面）。 */
            std::string fallback_reason;
            /** @brief 静态硬证明已接管决议位。 */
            bool static_resolved = false;
            /** @brief 候选集触顶 MAX_CANDIDATES。 */
            bool candidates_truncated = false;
        };

        /**
         * @brief 按符号 id 压入一条候选：键与全限定名取自同一行。
         *
         * @internal 阶梯各段的候选行恒以 (unique_key, name) 成对取自
         *           by_id 的同一行；收口单点后 id 只查一次表。
         */
        inline void push_candidate(
            std::vector<DispatchCandidate>& out,
            const ResolveContext&           ctx,
            std::int64_t                    id,
            Confidence                      confidence,
            std::string_view                reason)
        {
            const cg::db::IndexRow& row = *ctx.by_id.at(id);
            out.push_back(
                {.target_key = row.unique_key,
                 .target_qualified = row.name,
                 .confidence = confidence,
                 .reason = std::string{reason}});
        }

        /**
         * @brief 静态根清洗、两级回退与密封/final 判定。
         *
         * @internal 静态根清洗：键经三段兜底解析（前置声明键 miss 时
         *           剥文件段唯一匹配，与接收者可达过滤同款先例）；模
         *           板壳根（采集期 is_template 位的主模板行）跳过——
         *           壳是剥 operator-> 后的语法包装残留，真正的
         *           pointee 静态面已由采集层展开为独立键，壳自身与目
         *           标家族通常零交集，留在根集只会以缺席拖垮封口（最
         *           近覆盖并集已收敛、final/唯一守卫齐备的站点被壳假
         *           缺席压档）。全壳（清洗后无根）不立界：空洞证明面
         *           不构成 sealed 判定，回退既有展示面路径。
         *           隐式 this 回退：接收者缺位（裸成员调用，
         *           QueryInterface 尾部的 AddRef() 形态）没有接收者表
         *           达式，receiver_name 与 static_types 双空。隐式
         *           this 的静态类型就是调用方所属记录——按它补唯一的
         *           静态根。防误伤四条：接收者在场未解析位
         *           （receiver_is_this 的 name 空扩展义）整档不接管
         *           ——接收者表达式写在了源码里（出参解引用
         *           `(*pp_out)->AddRef()`、下标访问、链式调用等）但
         *           采集层没解析出名时，接收者对象的动态类型与调用
         *           方所属类没有任何结构关联，拿调用方类当静态根是
         *           "同家族错对象"直连，维持披露等真证据；模板主模板
         *           记录不进静态根（不同实例化的动态类型互异，主模
         *           板代表不了任何一族实例——后续折算闭包分支单独决
         *           议，绝不与 has_descendant 纯 extends 口径的 final
         *           判定混流）；调用方不是成员函数（自由函数/lambda
         *           的所属记录为空）天然不触发；static_types 已有值
         *           时不覆盖（有真接收者证据）。模板记录不靠拼写或
         *           canonical 判定，直接消费采集期落库的
         *           sym_flags::is_template 位。
         *           接收者声明面回退：有名接收者但站点静态类型空——
         *           采集面拿不到声明记录 key（界外/宏产物等形态）而
         *           同定位域的声明行在库时，用接收者槽的声明面类型补
         *           静态根（槽级 static_types = 同域 Declared 行
         *           type_set 的并集，即变量的声明记录类型）。与隐式
         *           this 回退互斥（receiver_name 空/非空）。防误伤：
         *           声明面为空（无 Declared 证据）或任一 key 解析失
         *           败时不补（宁漏勿猜）；补根后仍走全部既有判据
         *           （sealed/家族=1/nearest 唯一化与降档原样），多候
         *           选不猜。
         */
        [[nodiscard]]
        inline auto resolve_static_roots(
            ResolveContext&       ctx,
            const PropagatedSite& site) -> StaticRoots
        {
            StaticRoots roots;
            roots.ok = !site.static_types.empty();
            for (const std::string& key : site.static_types)
            {
                const auto sid = ctx.id_of_static_key(key);
                if (!sid.has_value())
                {
                    roots.ok = false;
                    break;
                }
                const auto row = ctx.by_id.find(*sid);
                if (row != ctx.by_id.end()
                    && (row->second->flags & cg::db::sym_flags::is_template)
                           != 0)
                {
                    continue; // 模板壳主模板键：无语义证明面，不参与
                }
                roots.roots.push_back(*sid);
            }
            roots.ok = roots.ok && !roots.roots.empty();
            if (!roots.ok && site.static_types.empty()
                && site.receiver_name.empty() && !site.receiver_is_this
                && !site.caller_owner_key.empty())
            {
                const auto sid = ctx.id_of_key(site.caller_owner_key);
                if (sid.has_value())
                {
                    const auto row = ctx.by_id.find(*sid);
                    if (row != ctx.by_id.end())
                    {
                        if ((row->second->flags
                             & cg::db::sym_flags::is_template)
                            == 0)
                        {
                            roots.roots.push_back(*sid);
                            roots.ok = true;
                            roots.implicit_this_root = true;
                        }
                        else
                        {
                            roots.implicit_this_template_id = *sid;
                        }
                    }
                }
            }
            if (!roots.ok && site.static_types.empty()
                && !site.receiver_name.empty())
            {
                const PropagatedHint* declared =
                    ctx.declared_receiver_roots(site);
                if (declared != nullptr && !declared->static_types.empty())
                {
                    bool roots_all = true;
                    for (const std::string& key : declared->static_types)
                    {
                        const auto sid = ctx.id_of_key(key);
                        if (!sid.has_value())
                        {
                            roots_all = false;
                            break;
                        }
                        roots.roots.push_back(*sid);
                    }
                    if (roots_all && !roots.roots.empty())
                    {
                        roots.ok = true;
                        roots.receiver_declared_root = true;
                    }
                    else
                    {
                        roots.roots.clear();
                    }
                }
            }
            // 语法 final ∪ extends 无后代的并集口径（两个证据方向互
            // 补：语法位不依赖图完备性，图覆盖源码外起点的 extends）。
            roots.all_sealed = roots.ok;
            roots.all_final = roots.ok;
            if (roots.ok)
            {
                for (const std::int64_t sid : roots.roots)
                {
                    // sealed（final 或具体类）成立时不翻位，继续看
                    // final；final 判定独立累加。
                    if (ctx.hierarchy.has_descendant(sid)
                        && ctx.has_pure_member.contains(sid))
                    {
                        roots.all_sealed = false;
                    }
                    if (!ctx.is_final_record(sid)
                        && ctx.hierarchy.has_descendant(sid))
                    {
                        roots.all_final = false;
                    }
                    if (!roots.all_sealed && !roots.all_final)
                    {
                        break;
                    }
                }
            }
            return roots;
        }

        /**
         * @brief 第一优先：final/具体类 + 家族唯一非纯实现 → 直接
         *        决议。
         *
         * @internal 定义：每个接收者静态类型 S 都满足 (a) final：语法
         *           final 位（sym_flags::is_final，源码硬证据）或
         *           extends 图内无后代（图内近似，缺边时漏看派生；
         *           C++ final 语义下动态类型只能是 S 自身）；或 (b)
         *           具体类：S 的直接成员中无 is_pure_virtual（接口/
         *           抽象类排除，继承的 pure 不追——偏宽方向由下方唯
         *           一实现守卫兜住）；且 override 家族中恰有 1 个非
         *           pure 实现 m。soundness：动态类型 ∈ {S} ∪
         *           descendants(S)（final 时恰为 {S}），虚调用解析到
         *           动态类型的最近覆盖且它必非纯虚，而全家族唯一的非
         *           纯成员就是 m——别无选择。静态可证语义按项目惯例
         *           给 High(1.0)（与 DirectNew 同档）。
         *           例外清单（触发失败时逐档下落，绝不伪装唯一目
         *           标）：static_types 任一 key 解析失败；家族非纯实
         *           现数 != 1；家族/extends 闭包不完备（漏 overrides
         *           或 extends 边会漏真实目标——extends 缺边误判
         *           final 时若派生覆盖在家族内会使非纯数 >= 2 而不触
         *           发，只有两类边同时缺失才可能错，与
         *           nearest-override 0.7 档同源的已声明风险，方向只
         *           漏不假）。非纯成员表来自家族缓存（构建时与 pure
         *           位同步预筛）。reason 按根来源区分：声明面回退
         *           receiver_declared_unique、隐式 this 回退
         *           implicit_this_unique、显式静态类型
         *           static_unique_override。
         *
         * @return 已决议（其后各段对决议站点恒 no-op）。
         */
        [[nodiscard]]
        inline bool try_unique_override(
            ResolveContext&    ctx,
            const FamilyInfo&  family,
            const StaticRoots& roots,
            StaticSiteOutcome& out)
        {
            if (!roots.all_sealed || family.non_pure_members.size() != 1)
            {
                return false;
            }
            out.static_resolved = true;
            push_candidate(
                out.candidates,
                ctx,
                family.non_pure_members.front(),
                Confidence::High,
                roots.receiver_declared_root ? "receiver_declared_unique"
                : roots.implicit_this_root   ? "implicit_this_unique"
                                             : "static_unique_override");
            out.edge_targets = out.candidates;
            return true;
        }

        /**
         * @brief 第 1.5 优先：nearest 并集 × 类级/方法级 final 证据；
         *        不满足则 Low 披露或 DTA 子树收窄。
         *
         * @internal 进入条件放宽为有可用静态根；类级与方法级 final
         *           证据在此两级判定：先按 nearest-override 语义求各
         *           根最近覆盖的并集（两级判定共享，只算一次）。类级
         *           all_final：全部静态根都是 final 记录（语法位或图
         *           内无后代）→ 动态类型恰为各根自身（库内家族=1 的
         *           收窄面），不必要求全家族唯一实现——并集恰一个目
         *           标时静态可证唯一（High(1.0)，与第一优先同档；
         *           reason 区分隐式 this 回退与显式静态类型两种来
         *           源）。方法级接力（类级证据缺失时）：并集恰一个成
         *           员且该成员符号自带方法级 final 语法位
         *           （sym_flags::is_final 位的 method 宿主侧）→ 该成
         *           员是各接收者静态链上的最近覆盖，final 由编译器强
         *           制封死其下的一切真实覆盖（源码语法硬证据，与
         *           extends/overrides 图完备性无关——图缺边只导致目
         *           标缺席不发，不可能误指别的成员）→ 动态类型的最近
         *           覆盖恒为该成员，直连 High(1.0)（reason=
         *           static_final_method 系列，隐式 this/声明面回退来
         *           源按第一优先惯例区分命名）。类级与方法级证据的直
         *           连目标恒同：两规则是封印证据的并集而非竞争关系。
         *           同距离多覆盖（多继承菱形）或任一根在家族内无覆盖
         *           （absent）一律不猜：发 Low 展示候选并保持 0.0 家
         *           族标记边（宁漏勿猜）。
         *           第二优先（两级 final 证据都不满足时）：DTA 静态
         *           声明子树收窄：候选面 = 家族 ∩ (各静态类型 extends
         *           后代子树的并集)，排除 method 自身；空集守卫回退全
         *           家族；子树溢出（>256 节点或 16 层未收敛）同样回退
         *           全家族并标 subtree_overflow。收窄只改展示面：候选
         *           置信 Low(0.4)、fallback_reason 标 static_narrowed，
         *           不发推断边——0.4 档不构成派发证据，0.0 家族标记
         *           边保持不变。
         *
         * @return 已决议（直连早退；其后各段对决议站点恒 no-op）。
         */
        [[nodiscard]]
        inline bool try_final_ladder(
            ResolveContext&                    ctx,
            const FamilyInfo&                  family,
            const std::optional<std::int64_t>& method,
            const StaticRoots&                 roots,
            StaticSiteOutcome&                 out)
        {
            if (!roots.ok)
            {
                return false;
            }
            const NearestUnions static_unions = nearest_overrides(
                family,
                roots.roots,
                ctx.hierarchy,
                ctx.dist_of);
            const auto& static_nearest = static_unions.nearest;
            const bool  static_unique =
                static_unions.absent_ids.empty() && static_nearest.size() == 1;
            const auto member_row =
                static_unique ? ctx.by_id.find(static_nearest.begin()->first)
                              : ctx.by_id.end();
            const bool member_final =
                static_unique && member_row != ctx.by_id.end()
                && (member_row->second->flags & cg::db::sym_flags::is_final)
                       != 0;
            if (member_row != ctx.by_id.end()
                && (roots.all_final || member_final))
            {
                out.static_resolved = true;
                const char* reason = nullptr;
                if (roots.all_final)
                {
                    reason = roots.receiver_declared_root
                                 ? "receiver_declared_unique"
                             : roots.implicit_this_root ? "implicit_this_unique"
                                                        : "static_final_unique";
                }
                else
                {
                    reason = roots.receiver_declared_root
                                 ? "receiver_declared_final_method"
                             : roots.implicit_this_root
                                 ? "implicit_this_final_method"
                                 : "static_final_method";
                }
                push_candidate(
                    out.candidates,
                    ctx,
                    static_nearest.begin()->first,
                    Confidence::High,
                    reason);
                out.edge_targets = out.candidates;
                return true;
            }
            if (roots.all_final)
            {
                for (const auto& [member, dist] : static_nearest)
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
                        Confidence::Low,
                        "no_dynamic_type_evidence");
                }
                if (!out.candidates.empty())
                {
                    out.fallback_reason =
                        "no_dynamic_type_evidence;static_narrowed";
                }
                return false;
            }
            const Subtree subtree =
                ctx.hierarchy.descendant_subtree(roots.roots);
            if (!subtree.overflow)
            {
                for (const std::int64_t member : family.non_pure_members)
                {
                    if (member == *method)
                    {
                        continue;
                    }
                    const auto owner = ctx.owner_of_member(family, member);
                    if (!owner.has_value() || !subtree.ids.contains(*owner))
                    {
                        continue;
                    }
                    if (out.candidates.size() >= MAX_CANDIDATES)
                    {
                        out.candidates_truncated = true;
                        break;
                    }
                    push_candidate(
                        out.candidates,
                        ctx,
                        member,
                        Confidence::Low,
                        "no_dynamic_type_evidence");
                }
                if (!out.candidates.empty())
                {
                    out.fallback_reason =
                        "no_dynamic_type_evidence;static_narrowed";
                }
            }
            else
            {
                out.fallback_reason =
                    "no_dynamic_type_evidence;subtree_overflow";
            }
            return false;
        }

        /**
         * @brief 隐式 this 的模板主模板专用分支（折算闭包决议）。
         *
         * @internal CRTP 模板基（派生类 : AcmeBase<派生类>）方法体内
         *           的裸虚调用：调用方所属记录是主模板行（方法宿主全
         *           部挂在主模板，实例行不重复挂成员），主模板行代表
         *           不了任何一族实例的动态类型——第一/1.5 优先的静态
         *           根路径不适用（has_descendant 是纯 extends 口径，
         *           看不见特化行后代，主模板会被误判 final 而假阳性
         *           直连）。专用判据改用 descendant_subtree 的折算闭
         *           包（extends 下行 + specializes 下行复合）：候选面
         *           = 家族非纯成员中 owner 落在 [主模板] 折算闭包内的
         *           那些——闭包把主模板的全部实例化行及其 extends 派
         *           生折算为结构可达，特化侧不覆盖时闭包内唯一非纯成
         *           员就是主模板自己的共享实现。恰 1 且闭包无溢出 →
         *           High(1.0) 直连（与 implicit_this_unique 对称）；
         *           多于 1 → Low(0.4) 披露命中集（收窄只改展示面，
         *           0.0 家族标记边保持）；0 或溢出 → 维持家族级
         *           fallback 原状。唯一命中另加折算缺边守门：家族闭
         *           包外非纯成员的 owner 走 extends 上行链，途经缺口
         *           实例行（键含 '<' 且无 specializes 出边——该行若是
         *           本主模板的实例，其子树成员进不了折算闭包）时不
         *           可再证唯一，降 0.4 披露（fold_gap）。守门后直连的
         *           残余风险与第一优先同构：只有 extends 与
         *           specializes 两类边同时缺失才可能误连。
         */
        inline void try_implicit_this_template(
            ResolveContext&    ctx,
            const FamilyInfo&  family,
            const StaticRoots& roots,
            StaticSiteOutcome& out)
        {
            if (!roots.implicit_this_template_id.has_value()
                || out.static_resolved || !out.candidates.empty())
            {
                return;
            }
            const Subtree subtree = ctx.hierarchy.descendant_subtree(
                {*roots.implicit_this_template_id});
            std::vector<std::int64_t> hits;
            if (!subtree.overflow)
            {
                for (const std::int64_t member : family.non_pure_members)
                {
                    const auto owner = ctx.owner_of_member(family, member);
                    if (owner.has_value() && subtree.ids.contains(*owner))
                    {
                        hits.push_back(member);
                    }
                }
            }
            if (subtree.overflow)
            {
                out.fallback_reason =
                    "no_dynamic_type_evidence;subtree_overflow";
            }
            else if (hits.size() == 1)
            {
                bool fold_gap = false;
                for (const std::int64_t member : family.non_pure_members)
                {
                    const auto owner = ctx.owner_of_member(family, member);
                    if (!owner.has_value() || subtree.ids.contains(*owner))
                    {
                        continue;
                    }
                    if (ctx.chain_hits_fold_gap(*owner))
                    {
                        fold_gap = true;
                        break;
                    }
                }
                if (!fold_gap)
                {
                    out.static_resolved = true;
                    push_candidate(
                        out.candidates,
                        ctx,
                        hits.front(),
                        Confidence::High,
                        "implicit_this_template_unique");
                    out.edge_targets = out.candidates;
                    return; // 折算闭包唯一命中已决议
                }
                // 缺口在案：唯一性不可证——降 0.4 披露，绝不直连。
                push_candidate(
                    out.candidates,
                    ctx,
                    hits.front(),
                    Confidence::Low,
                    "no_dynamic_type_evidence");
                out.fallback_reason = "no_dynamic_type_evidence;"
                                      "implicit_this_template_fold_gap";
            }
            else if (!hits.empty())
            {
                for (const std::int64_t member : hits)
                {
                    if (out.candidates.size() >= MAX_CANDIDATES)
                    {
                        out.candidates_truncated = true;
                        break;
                    }
                    push_candidate(
                        out.candidates,
                        ctx,
                        member,
                        Confidence::Low,
                        "no_dynamic_type_evidence");
                }
                out.fallback_reason = "no_dynamic_type_evidence;"
                                      "implicit_this_template_narrowed";
            }
        }

        /**
         * @brief 宿主抽象直连：method 纯虚 × 全库唯一非纯 override。
         *
         * @internal 对象存在性论证：被调方法自带纯虚语法位 ⟹ 其宿
         *           主抽象、不可能实例化 ⟹ 任何运行时动态类型必为可
         *           实例化的后代且其最近覆盖非纯（覆盖带纯虚位会使
         *           宿主同样抽象），而全库家族恰有一个非纯成员——别
         *           无选择，静态可证 High(1.0)。纯虚位是采集落库的
         *           源码语法硬证据，兼作数据质量闸：声明符号缺纯虚位
         *           （默认体/override 二选一静态不可分）的形态不触发，
         *           宁漏勿错。外部边界守卫：接收者静态类型命中
         *           `#external` 行（用户显式声明实现可达集在库外）时
         *           不开闸——全库唯一只是库内观测，边界接口的实现在
         *           索引外是既有知识而非缺边近似，直连会与可达性纪
         *           律冲突。置于隐式 this 模板分支之后：CRTP 主模板
         *           调用方先走折算闭包专用判定；此前各优先已填的展
         *           示候选被直连覆盖（展示面不是决议面）。两道结构守
         *           卫（缺一即挡、绝不伪装唯一）：(1) 闭包完备性——
         *           模板中间层缺边形态（family_closure_gap）下"全库
         *           唯一"是闭包观测假象，直连即同家族错对象；(2) 接
         *           收者间接形态——下标元素（"#elem:" 槽名）与接收者
         *           表达式在场未解析（name 空 + 在场位）形态上接收者
         *           对象与调用方/库内唯一实现无结构关联论证可用，这
         *           两种形态上发生过整簇误连，排除后回披露等真证据。
         *           挡下时 fallback_reason 如实披露挡因（绝不静默）。
         */
        inline void try_abstract_host_unique(
            ResolveContext&                    ctx,
            const PropagatedSite&              site,
            const FamilyInfo&                  family,
            const std::optional<std::int64_t>& method,
            StaticSiteOutcome&                 out)
        {
            bool receiver_external = false;
            for (const std::string& key : site.static_types)
            {
                const auto sid = ctx.id_of_key(key);
                if (sid.has_value() && ctx.external_of.contains(*sid))
                {
                    receiver_external = true;
                    break;
                }
            }
            const bool receiver_indirect_form =
                site.receiver_name.starts_with("#elem:")
                || (site.receiver_name.empty() && site.receiver_is_this);
            if (out.static_resolved || receiver_external || !method.has_value()
                || family.non_pure_members.size() != 1)
            {
                return;
            }
            const auto mrow = ctx.by_id.find(*method);
            if (mrow == ctx.by_id.end()
                || (mrow->second->flags & cg::db::sym_flags::is_pure_virtual)
                       == 0)
            {
                return;
            }
            const bool closure_gap = ctx.family_closure_gap(*method, family);
            if (receiver_indirect_form || closure_gap)
            {
                // 守卫挡下：维持披露，挡因如实追加（已有展示面披露串
                // 不覆盖，绝不静默）。
                const std::string_view guard_tag =
                    closure_gap ? "family_closure_gap"
                                : "receiver_indirect_form";
                out.fallback_reason =
                    out.fallback_reason.empty()
                        ? std::format("no_dynamic_type_evidence;{}", guard_tag)
                        : std::format("{};{}", out.fallback_reason, guard_tag);
                return;
            }
            out.static_resolved = true;
            out.candidates.clear();
            out.edge_targets.clear();
            push_candidate(
                out.candidates,
                ctx,
                family.non_pure_members.front(),
                Confidence::High,
                "family_unique_override");
            out.edge_targets = out.candidates;
        }

        /**
         * @brief 家族级 fallback 带：全家族披露 + 静态命中成员的 0.0
         *        标记边。
         *
         * @internal 每站点只发一条 0.0 标记边（目标是静态命中的成
         *           员），全部候选留在 call_sites.candidates。这样未
         *           解析站点在巨型族上不会爆炸成一条边/候选。
         */
        inline void fallback_family_band(
            ResolveContext&                    ctx,
            const FamilyInfo&                  family,
            const std::optional<std::int64_t>& method,
            StaticSiteOutcome&                 out)
        {
            if (!out.static_resolved && out.candidates.empty())
            {
                for (const std::int64_t member : family.non_pure_members)
                {
                    if (member == *method)
                    {
                        continue;
                    }
                    if (out.candidates.size() >= MAX_CANDIDATES)
                    {
                        out.candidates_truncated = true;
                        break;
                    }
                    push_candidate(
                        out.candidates,
                        ctx,
                        member,
                        Confidence::Fallback,
                        "no_dynamic_type_evidence");
                }
                if (out.fallback_reason.empty())
                {
                    out.fallback_reason = "no_dynamic_type_evidence";
                }
            }
            if (!out.static_resolved)
            {
                push_candidate(
                    out.edge_targets,
                    ctx,
                    *method,
                    Confidence::Fallback,
                    "no_dynamic_type_evidence");
            }
        }

        /**
         * @brief 外部边界 RTA（排序 + 标注，不砍候选不发边）。
         *
         * @internal 激活 = 接收者静态类型命中 `#external` 行（SWIG
         *           导出/director/插件注册表/dlopen 边界接口）。可达
         *           ≠ 此站点调它：hit>1 只把命中 R(I) 的候选稳定排
         *           前；hit==1 只展示唯一候选（Low 展示档）也不发边
         *           （0.0 家族标记边保持不变）；hit==0 是"无库内实
         *           例化实例"的诚实信号——插件 DLL 实现在索引外（抽
         *           样 30 个点位中 3 例），保留全家族并标注实现可能
         *           索引外，严禁发空候选。站点级置信严禁：本段不动
         *           edge_targets、不抬任何候选置信。
         */
        inline void apply_external_rta(
            ResolveContext&       ctx,
            const PropagatedSite& site,
            const FamilyInfo&     family,
            StaticSiteOutcome&    out)
        {
            const ExternalReach* reach = nullptr;
            for (const std::string& key : site.static_types)
            {
                const auto it =
                    ctx.external_of.find(ctx.id_of_key(key).value_or(-1));
                if (it != ctx.external_of.end())
                {
                    reach = &it->second;
                    break;
                }
            }
            if (reach == nullptr || out.static_resolved)
            {
                return;
            }
            std::vector<std::int64_t> hit_members;
            for (const std::int64_t member : family.non_pure_members)
            {
                const auto owner = ctx.owner_of_member(family, member);
                if (owner.has_value() && reach->ids.contains(*owner))
                {
                    hit_members.push_back(member);
                }
            }
            if (hit_members.empty())
            {
                // hit==0：实现可能索引外，保全家族并如实标注。
                if (!out.fallback_reason.empty())
                {
                    out.fallback_reason += ';';
                }
                out.fallback_reason = std::format(
                    "{}external_no_instantiation",
                    out.fallback_reason);
                return;
            }
            std::ranges::stable_partition(
                out.candidates,
                [&](const DispatchCandidate& c)
                {
                    const auto member = ctx.id_of_key(c.target_key);
                    return member.has_value()
                           && reach->ids.contains(
                               ctx.owner_of_member(family, *member)
                                   .value_or(-1));
                });
            if (hit_members.size() == 1)
            {
                // 只展示唯一候选：可达性证据不足以发边（"可达≠此站
                // 点调它"），收缩展示面到该成员。
                out.candidates.clear();
                push_candidate(
                    out.candidates,
                    ctx,
                    hit_members.front(),
                    Confidence::Low,
                    "external_rta_narrowed");
            }
            if (!out.fallback_reason.empty())
            {
                out.fallback_reason += ';';
            }
            out.fallback_reason =
                std::format("{}external_rta_narrowed", out.fallback_reason);
        }

        /**
         * @brief 静态侧决议入口：六段优先阶梯按序短路。
         *
         * @internal 无动态证据（或弱带动态证据被静态硬证明接管）的
         *           虚调用站点在此决议，产出展示候选与推断边：根清
         *           洗与两级回退 → 第一优先直连 → 1.5 优先（final
         *           阶梯 / Low 披露 / DTA 收窄）→ 隐式 this 模板折
         *           算闭包 → 宿主抽象直连 → 家族级 fallback 带 → 外
         *           部边界 RTA 标注。每个成功档位早退（其后各段对决
         *           议站点恒 no-op）。
         */
        inline void resolve_static_site(
            ResolveContext&                    ctx,
            const PropagatedSite&              site,
            const FamilyInfo&                  family,
            const std::optional<std::int64_t>& method,
            StaticSiteOutcome&                 out)
        {
            const StaticRoots roots = resolve_static_roots(ctx, site);
            if (try_unique_override(ctx, family, roots, out))
            {
                return;
            }
            if (try_final_ladder(ctx, family, method, roots, out))
            {
                return;
            }
            try_implicit_this_template(ctx, family, roots, out);
            try_abstract_host_unique(ctx, site, family, method, out);
            fallback_family_band(ctx, family, method, out);
            apply_external_rta(ctx, site, family, out);
        }

    } // namespace dispatch_detail

} // namespace cg::analysis
