/**
 * @module cg.analysis.dispatch.context
 *
 * @brief 虚调用派发解析的上下文域：resolve 全程共享的符号投影索引、
 *        惰性记忆化缓存、槽级声明面查找与守卫索引。
 *
 * @internal 只读半边（by_id/by_key/has_pure_member/external_of）构造
 *           时一次装配；记忆化半边是各查找的惰性缓存——家族表、祖先
 *           距离、前置声明键兜底索引、槽级声明面索引、排除域闭包、
 *           折算缺边两表、同名方法行索引。同一 resolve 调用内跨站点
 *           共享：首查构建、重复查询复用（符号集在解析期只读不变，
 *           缓存不失效）。
 */
export module cg.analysis.dispatch.context;

import std;
import cg.util;
import cg.db;
import cg.analysis.model;
import cg.analysis.propagate;
import cg.analysis.dispatch.graph;

export namespace cg::analysis
{

    namespace dispatch_detail
    {

        /**
         * @brief 外部边界 RTA 索引行（root set 线）。
         *
         * @internal 消费契约——可达 ≠ 此站点调它：只做候选排序与
         *           fallback_reason 标注；站点级置信严禁，绝不经本
         *           索引发边或抬档。
         */
        struct ExternalReach
        {
            /** @brief R(I) 成员 id 集（可达类型投影）。 */
            std::unordered_set<std::int64_t> ids;
            /** @brief 边界 reason（`#external` 行的披露串）。 */
            std::string reason;
        };

        /**
         * @brief resolve 全程共享的投影索引与记忆化上下文。
         *
         * @internal 只读半边（by_id/by_key/has_pure_member/external_of）
         *           构造时一次装配；记忆化半边是各查找的惰性缓存——
         *           家族表、祖先距离、前置声明键兜底索引、槽级声明面
         *           索引、排除域闭包、折算缺边两表、同名方法行索引。
         *           同一 resolve 调用内跨站点共享：首查构建、重复查
         *           询复用，与原内嵌 lambda 的闭包共享语义逐项相同
         *           （符号集在解析期只读不变，缓存不失效）。
         */
        struct ResolveContext
        {
            ResolveContext(
                cg::db::GraphStore&                  store,
                const PropagationState&              state_in,
                const std::vector<cg::db::IndexRow>& rows_in,
                const Hierarchy&                     hierarchy_in);

            /** @brief 符号投影行（与 propagate::run 共用的全表读）。 */
            const std::vector<cg::db::IndexRow>& rows;
            /** @brief extends/overrides/specializes 全量预载的内存图。 */
            const Hierarchy& hierarchy;
            /** @brief 传播状态（槽级声明面回退读 hints）。 */
            const PropagationState& state;

            /** @brief id -> 行投影。 */
            std::unordered_map<std::int64_t, const cg::db::IndexRow*> by_id;
            /**
             * @brief unique_key -> id 投影（透明哈希）。
             * @internal 调用方持有 string_view 键，直查免逐次构造临
             *           时键。
             */
            std::unordered_map<
                std::string,
                std::int64_t,
                cg::util::string_hash,
                std::equal_to<>>
                by_key;
            /**
             * @brief 直接成员含 pure virtual 方法的记录集。
             * @internal "具体类"判定的图内口径：只看直接声明，继承的
             *           pure 不追——偏宽的方向被静态决议的唯一实现守
             *           卫兜住。
             */
            std::unordered_set<std::int64_t> has_pure_member;
            /** @brief 外部边界 RTA 索引（接口 id -> 可达成员集）。 */
            std::unordered_map<std::int64_t, ExternalReach> external_of;

            /** @brief method -> 覆盖家族（family_of_method 回填）。 */
            std::unordered_map<std::int64_t, FamilyInfo> family_of;
            /** @brief 动态类型 -> 祖先距离表（nearest 查找回填）。 */
            std::unordered_map<std::int64_t, std::map<std::int64_t, int>>
                dist_of;
            /** @brief 前置声明键兜底索引（首个 miss 键触发构建）。 */
            std::optional<
                std::unordered_map<std::string, std::vector<std::int64_t>>>
                trimmed_key_ids;
            /** @brief 槽级声明面索引（首个命中站点触发构建）。 */
            std::optional<
                std::unordered_map<std::string, const PropagatedHint*>>
                hint_by_slot;
            /** @brief 排除域闭包 memo（键 = 原始静态键序列）。 */
            std::unordered_map<std::string, std::shared_ptr<const Subtree>>
                xeno_scope_of;
            /** @brief 折算缺边行集（首个守卫查询触发构建）。 */
            std::optional<std::unordered_set<std::int64_t>> fold_gap_rows;
            /** @brief 起点记录 -> 折算缺边命中 memo。 */
            std::unordered_map<std::int64_t, bool> fold_gap_chain_of;
            /** @brief 同名方法行索引（首个闭包守卫查询触发构建）。 */
            std::optional<
                std::unordered_map<std::string, std::vector<std::int64_t>>>
                method_rows_by_name;
            /** @brief method -> 闭包缺边 memo。 */
            std::unordered_map<std::int64_t, bool> closure_gap_of;

            /** @brief unique_key 直查。 */
            [[nodiscard]]
            auto id_of_key(std::string_view key) const
                -> std::optional<std::int64_t>
            {
                const auto it = by_key.find(key);
                if (it == by_key.end())
                {
                    return std::nullopt;
                }
                return it->second;
            }

            /**
             * @brief 家族成员的 owner 散点快查。
             *
             * @internal 命中走缓存（整数哈希），未命中（成员不在本
             *           站点家族缓存——理论不发生，同键多符号的解析
             *           还原形态）回退逐次解析链，判据与缓存前逐字
             *           相同。
             */
            [[nodiscard]]
            auto owner_of_member(const FamilyInfo& fam, std::int64_t member)
                const -> std::optional<std::int64_t>
            {
                const auto it = fam.owner_of.find(member);
                if (it != fam.owner_of.end())
                {
                    return it->second;
                }
                return id_of_key(owner_key_of(by_id, member));
            }

            /** @brief 记录行带语法 final 位。 */
            [[nodiscard]]
            bool is_final_record(std::int64_t sid) const
            {
                const auto row = by_id.find(sid);
                return row != by_id.end()
                       && (row->second->flags & cg::db::sym_flags::is_final)
                              != 0;
            }

            /** @brief 静态侧键解析（直查 + 前置声明键兜底）。 */
            [[nodiscard]]
            auto id_of_static_key(const std::string& key)
                -> std::optional<std::int64_t>;

            /** @brief 接收者声明面回退（槽级索引查找）。 */
            [[nodiscard]]
            auto declared_receiver_roots(const PropagatedSite& site)
                -> const PropagatedHint*;

            /** @brief 覆盖家族（含派生量预解析）的记忆化构建。 */
            [[nodiscard]]
            auto family_of_method(std::int64_t method) -> const FamilyInfo&;

            /** @brief 接收者声明面排除域（异族豁免的证明面）。 */
            [[nodiscard]]
            auto xeno_exclusion_scope(
                const std::vector<std::string>& static_types) -> const Subtree*;

            /** @brief owner 的 extends 上行链是否途经折算缺口实例行。 */
            [[nodiscard]]
            bool chain_hits_fold_gap(std::int64_t owner);

            /** @brief 宿主抽象直连的闭包完备性守卫。 */
            [[nodiscard]]
            bool family_closure_gap(std::int64_t method, const FamilyInfo& fam);
        };

        /**
         * @brief 投影与只读索引的一次性装配。
         *
         * @internal WHERE 投影直取 R(I) 边界行：行集与
         *           all_type_hints 全表读后内存过滤 source==External
         *           且 var_name==external_slot 的旧行集逐行相等，免
         *           整表物化。
         */
        inline ResolveContext::ResolveContext(
            cg::db::GraphStore&                  store,
            const PropagationState&              state_in,
            const std::vector<cg::db::IndexRow>& rows_in,
            const Hierarchy&                     hierarchy_in)
            : rows{rows_in}, hierarchy{hierarchy_in}, state{state_in}
        {
            by_id.reserve(rows.size());
            by_key.reserve(rows.size());
            for (const cg::db::IndexRow& row : rows)
            {
                by_id.emplace(row.id, &row);
                by_key.emplace(row.unique_key, row.id);
            }
            for (const cg::db::IndexRow& row : rows)
            {
                if ((row.flags & cg::db::sym_flags::is_pure_virtual) != 0
                    && row.parent_id.has_value())
                {
                    has_pure_member.insert(*row.parent_id);
                }
            }
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

        /**
         * @brief 静态侧键解析：直查失败时剥文件段做前置声明键兜底。
         *
         * @internal 兜底索引：key 剥文件段（kind|ns|name 三段）-> 候选
         *           id 集。IDL 生态里接口的前置声明形态（聚合头集中
         *           前置声明等）不落符号行，其 key 在符号域 miss——但
         *           ODR 下同名同命名空间同种类记录是同一实体，定义行
         *           承载 extends 闭包。首个 miss 键触发惰性构建（全
         *           投影一次扫描）；三段多候选（多 TU 各挂一行等形
         *           态）不猜（宁漏）。
         */
        inline auto ResolveContext::id_of_static_key(const std::string& key)
            -> std::optional<std::int64_t>
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
        }

        /**
         * @brief 槽级声明面查找：按定位域顺序取首个命中行。
         *
         * @internal 查找序与传播期接收者槽定位同序同停（字段声明记录
         *           域 → 调用方函数域 → 隐式 this 的记录域）：行存在
         *           即停、不跨域续找——同名变量的声明面只认首个定位
         *           域（与槽消费同一变量），宁漏勿猜。
         */
        [[nodiscard]]
        inline auto first_declared_hint(
            const std::unordered_map<std::string, const PropagatedHint*>&
                                  by_slot,
            const PropagatedSite& site) -> const PropagatedHint*
        {
            if (!site.receiver_owner_key.empty())
            {
                const auto it = by_slot.find(
                    cg::util::compose_key(
                        site.receiver_owner_key,
                        site.receiver_name));
                if (it != by_slot.end())
                {
                    return it->second;
                }
            }
            const auto caller = by_slot.find(
                cg::util::compose_key(site.caller_key, site.receiver_name));
            if (caller != by_slot.end())
            {
                return caller->second;
            }
            if (site.receiver_is_this && !site.caller_owner_key.empty())
            {
                const auto it = by_slot.find(
                    cg::util::compose_key(
                        site.caller_owner_key,
                        site.receiver_name));
                if (it != by_slot.end())
                {
                    return it->second;
                }
            }
            return nullptr;
        }

        /**
         * @brief 接收者声明面回退：槽级 (owner_key, var_name) 查找。
         *
         * @internal state.hints 的 (owner_key, var_name) -> 行索引在首
         *           个命中站点时一次构建——行在槽化装载后 (owner,
         *           var) 唯一（同一槽一行），构建序不影响查询结果；
         *           万级站点只有少数触发本查询，建表成本一次摊销。
         */
        inline auto ResolveContext::declared_receiver_roots(
            const PropagatedSite& site) -> const PropagatedHint*
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
            return first_declared_hint(*hint_by_slot, site);
        }

        /**
         * @brief method -> 覆盖家族（含派生量预解析）的记忆化构建。
         *
         * @internal 家族只按 method 构建一次，构建时同步预解析派生量
         *           （成员 owner id / 非纯子序列 / 散点快查表 / 非纯
         *           owner 位图与下标展开表），后续站点与覆盖查找全部
         *           整数索引/位运算——巨族（参考语料单族 3,425 成员）
         *           在 nearest 双循环与候选扫描中按站点重复进入，循
         *           环内逐成员做 owner 键字符串构造 + 字符串哈希解析
         *           是纯重复成本，缓存后全部走整数索引/哈希快查，语
         *           义与逐次解析完全一致（符号集在解析期只读不变）。
         */
        inline auto ResolveContext::family_of_method(std::int64_t method)
            -> const FamilyInfo&
        {
            auto [fam_it, fam_fresh] = family_of.try_emplace(method);
            if (fam_fresh)
            {
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
        }

        /**
         * @brief 接收者声明面排除域（异族豁免的证明面）。
         *
         * @internal 全部静态根的 extends 后代闭包并集
         *           （descendant_subtree 折算含特化行，与接收者可达
         *           判据同构）。C++ 静态类型系统下，虚调用点接收者的
         *           动态类型只能是某静态根自身或其后代——落在闭包之
         *           外的槽类型是可证噪声（并槽路径灌入的界外实现
         *           等），不必计入封口缺席。立界失败形态一律返回
         *           nullptr（调用方保持全缺席阻断，宁漏勿猜）：静态
         *           类型为空、任一键经三段兜底仍解析失败、闭包节点/
         *           深度溢出（证明面不完备时不可主张排除）。模板壳根
         *           不跳过——其折算闭包覆盖全部实例化派生，剔除会收
         *           窄排除面（豁免变宽是危险方向）。按原始键序列
         *           memo：同接收者声明面的多站点共享一次闭包展开。
         */
        inline auto ResolveContext::xeno_exclusion_scope(
            const std::vector<std::string>& static_types) -> const Subtree*
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
        }

        /**
         * @brief 折算缺边守门（fold gap）：owner 的 extends 上行链是
         *        否途经折算缺口实例行。
         *
         * @internal 模板主模板的下行折算闭包经 specializes 边把特化
         *           行折算为结构派生——实例行若是本家族主模板的实
         *           例、却缺 specializes 出边（主模板无定义等形态），
         *           其整棵 extends 子树对闭包结构性失明，"闭包内唯一
         *           命中"不可再证。判据：记录行（kind=Record）的键
         *           含 '<'（实例形态名）且无 specializes 出边。缺口
         *           行集合一次惰性构建；每起点记录的 extends 上行链
         *           （16 层封顶、只查途经基类）全决议期 memo 缓存。
         */
        inline bool ResolveContext::chain_hits_fold_gap(std::int64_t owner)
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
        }

        /**
         * @brief 宿主抽象直连的闭包完备性守卫（closure gap）。
         *
         * @internal overrides 家族闭包以"同签名方法行"为锚——接口经
         *           模板中间层继承且中间层不重声明该方法时，模板实
         *           现类（依赖基语境）的 override 边在采集层结构性
         *           缺失，其方法行落不进闭包，"全库唯一非纯实现"成
         *           为观测假象（接口 56 个模板实现类的同名行全部闭
         *           包外、闭包内仅剩一个非模板直接实现的观测形
         *           态）。
         *           判据（结构可达版，比同名计数失衡更准）：全库与
         *           被调方法同名、闭包外、可作覆盖行的方法行（带虚
         *           位，或宿主带模板位——模板定义语境下虚位与
         *           override 边同源丢失，模板实现类方法行只带 const
         *           位）中，任一行的宿主落在"家族成员宿主 ∪ 被调方
         *           法宿主"的折算后代子树内（extends 下行 +
         *           specializes 特化折算）即证明存在缺边——C++ 语义下结
         *           构可达 + 同名同虚（同一命名作用域内由 extends 链
         *           保证同签名覆盖关系）的行本应在闭包里。子树溢出
         *           时证明面不完备，同样阻断（宁漏勿错）。按方法
         *           memo；同名方法行索引首个触发守卫的站点一次构建。
         */
        inline bool ResolveContext::family_closure_gap(
            std::int64_t      method,
            const FamilyInfo& fam)
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
        }

    } // namespace dispatch_detail

} // namespace cg::analysis
