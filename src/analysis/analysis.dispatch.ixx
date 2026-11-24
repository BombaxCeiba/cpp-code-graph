/**
 * @module cg.analysis.dispatch
 *
 * @brief 虚调用派发解析：override 家族 ∩ 推断动态类型集 → virtual_dispatch
 *        边 + 候选集 + fallback；绝不把不确定的推断伪装成唯一目标。
 *
 * @internal 本单元持入口 resolve()、结果类型与站点终态结算；
 *           dispatch_detail 的其余自由函数族按职责域分居 graph /
 *           context / statics / dynamics 四个子模块，经本单元
 *           re-export 对消费者可见。
 */
export module cg.analysis.dispatch;

export import cg.analysis.dispatch.graph;
export import cg.analysis.dispatch.context;
export import cg.analysis.dispatch.statics;
export import cg.analysis.dispatch.dynamics;

import std;
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

        /**
         * @brief 站点终态结算：决议位、置信度、fallback_reason 后缀
         *        族、计数、边发射与行落账。
         *
         * @internal resolved 按值接收（调用方的基底装配 prvalue 直接
         *           物化到形参，零拷贝），终态写满后 move 进 out.sites。
         *           接收者槽截断观测（契约见
         *           PropagatedSite::slot_type_truncated 注释）：本站
         *           点动态类型证据可能不完整，如实追加后缀；仅主路
         *           径消费，四条早退路径不经过此处天然不加；既有串
         *           非空才补分号，避免 resolved 且无其他标记时出现
         *           前导分号串。边的属性位随置信度分档：Fallback 档
         *           dispatch_fallback（0.0 标记边），其余
         *           dispatch_inferred（推断边）。
         */
        inline void finalize_virtual_site(
            const PropagatedSite&    site,
            const StaticSiteOutcome& st,
            bool                     has_dynamic,
            ResolvedCallSite         resolved,
            DispatchResult&          out)
        {
            resolved.resolved =
                !st.candidates.empty() && (has_dynamic || st.static_resolved);
            resolved.candidates = st.candidates;
            Confidence site_conf = Confidence::Fallback;
            if (!st.candidates.empty())
            {
                site_conf = st.candidates.front().confidence;
                for (const DispatchCandidate& c : st.candidates)
                {
                    site_conf = better(site_conf, c.confidence);
                }
            }
            resolved.confidence = site_conf;
            if (resolved.resolved)
            {
                resolved.fallback_reason =
                    st.candidates_truncated
                        ? std::string{"candidates_truncated"}
                        : std::string{};
            }
            else
            {
                resolved.fallback_reason = st.fallback_reason;
                if (st.candidates_truncated)
                {
                    resolved.fallback_reason = std::format(
                        "{};candidates_truncated",
                        resolved.fallback_reason);
                }
            }
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
            for (const DispatchCandidate& c : st.edge_targets)
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

        /**
         * @brief 增量运行时站点是否在本次处理面之外。
         *
         * @internal 未受影响站点的行与边保持既有终态：site_affected
         *           掩码越界与掩码位为 0 同义（掩码按受影响站点生长）。
         */
        [[nodiscard]]
        inline bool site_masked_out(
            const PropagationState& state,
            bool                    all_sites,
            std::size_t             idx)
        {
            return state.incremental && !all_sites
                   && (idx >= state.site_affected.size()
                       || state.site_affected[idx] == 0);
        }

        /**
         * @brief 站点解析行的基底装配（各桶共用的恒定字段）。
         *
         * @internal static_type_id 取首个静态类型键的解析值（空静态
         *           面为无值）；receiver_types = 静态类型 ∪ 动态类型
         *           的排序去重并集。
         */
        [[nodiscard]]
        inline auto base_call_site(
            const ResolveContext& ctx,
            const PropagatedSite& site) -> ResolvedCallSite
        {
            ResolvedCallSite resolved;
            resolved.site_id = site.site_id;
            resolved.callee_id = site.callee_id;
            resolved.method_id = site.method_id;
            resolved.method_key = site.method_key;
            resolved.static_type_id =
                site.static_types.empty()
                    ? std::optional<std::int64_t>{}
                    : ctx.id_of_key(site.static_types.front());
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
        }

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
        // extends/overrides 全量预载的内存图（家族/祖先距离等记忆化表
        // 在 ResolveContext）：参考语料 distinct methods=1,235 对 6,777
        // 虚站点（重复率 ~5.8），动态类型同样跨站点重复，缓存把重复闭
        // 包归零。
        const Hierarchy hierarchy{store};
        ResolveContext  ctx{store, state, rows, hierarchy};

        // 站点五桶处理（早退判定链的桶级形态）：热列（site_buckets，装载
        // 物化时产出）可用时直接消费，否则按同一判据现场分桶——两条路径
        // 的桶划分逐字一致，行为等价。等价性：out.sites 终态按 site_id 排
        // 序（site_id 全库唯一，处理序不影响行序）；out.edges 仅 virtual
        // 桶产生，终态 stable_sort 并列键（同 caller/target/file/line）的
        // 相对序只由 virtual 桶内的下标序决定（桶内升序 == 原扫描序），
        // 早退桶不发边互不影响；fallback/resolved 计数按桶累加与逐站点
        // 分支等价。
        std::array<std::vector<std::uint32_t>, 5>      local_buckets;
        std::span<const std::vector<std::uint32_t>, 5> buckets = local_buckets;
        std::size_t                                    bucket_total = 0;
        for (const auto& b : state.site_buckets)
        {
            bucket_total += b.size();
        }
        if (bucket_total == state.sites.size())
        {
            buckets = state.site_buckets;
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
        // 早退四桶：桶级批量（固定 fallback_reason；原因二选一的桶按站点
        // 位读取，判据与原逐站点分支逐字相同）；表序即处理序。
        for (const EarlyBucketSpec& spec : EARLY_BUCKETS)
        {
            for (const std::uint32_t idx : buckets[spec.bucket])
            {
                if (site_masked_out(state, all_sites, idx))
                {
                    continue;
                }
                ResolvedCallSite resolved =
                    base_call_site(ctx, state.sites[idx]);
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
        for (const std::uint32_t idx : buckets[SITE_BUCKET_VIRTUAL])
        {
            const PropagatedSite& site = state.sites[idx];
            if (site_masked_out(state, all_sites, idx))
            {
                continue;
            }

            StaticSiteOutcome st;
            // 桶判据（SITE_BUCKET_VIRTUAL）保证 method_id 有值。
            const std::optional<std::int64_t> method = site.method_id;
            const FamilyInfo& family = ctx.family_of_method(*method);
            const DynamicFace face = dynamic_face_of(ctx, site, family);
            const bool filtered = xeno_exemption_lifts(ctx, site, family, face);
            fill_dynamic_candidates(ctx, site, face, filtered, st);
            narrow_root_lifecycle(ctx, site, family, st);
            const bool face_priority =
                apply_static_face_priority(ctx, site, face, st);
            const bool cast_priority = apply_cast_claim_priority(
                ctx,
                site,
                family,
                method,
                face_priority,
                st);
            const bool has_dynamic =
                !site.dynamic_types.empty() && !face_priority && !cast_priority;
            if (has_dynamic && st.candidates.empty())
            {
                // 有动态类型却没有任何家族成员匹配：如实降级到静态命中
                // 成员的家族级 0.0 标记边，绝不伪造具体目标。
                st.fallback_reason = "family_ambiguous";
                push_candidate(
                    st.edge_targets,
                    ctx,
                    *method,
                    Confidence::Fallback,
                    "family_ambiguous");
            }
            if (has_dynamic && st.edge_targets.empty())
            {
                st.edge_targets = st.candidates;
            }
            else if (!has_dynamic && !st.static_resolved)
            {
                resolve_static_site(ctx, site, family, method, st);
            }
            merge_multi_type_targets(st);
            finalize_virtual_site(
                site,
                st,
                has_dynamic,
                base_call_site(ctx, site),
                out);
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
