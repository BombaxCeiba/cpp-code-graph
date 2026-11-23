module;
#include "../db/db.tables.h"

/**
 * @module cg.graph.symledger
 *
 * @brief 符号与仲裁账目域（值语义域对象）：symbols 表之上的内存解析
 *        索引、未解析名观测与 heal 清票受影响集。
 *
 * @internal db.tables.h 置于全局模块片段：本单元导入 cg.db，MSVC 要求
 *           sqlpp23 表模型定义在此可见（同 cg.graph.build 的纪律）。
 *           cg.graph.build 以值成员嵌入并再导出本单元。
 */

export module cg.graph.symledger;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.extract;
import cg.analysis;

export namespace cg::graph
{
    namespace detail
    {
        /**
         * @brief 整个 symbols 表之上的内存解析索引，在每个 TU 的阶段 A 之后
         *        增长，使后续 TU 能对早先 TU 解析（两阶段导入）。
         */
        class SymbolIndex
        {
        public:
            struct Entry
            {
                cg::db::SymId               id = 0;
                cg::db::SymbolKind          kind = cg::db::SymbolKind::Function;
                std::string                 name;
                std::string                 ns;
                std::string                 parent;
                std::optional<std::int64_t> sig_hash;
                bool                        const_method = false;
                std::uint32_t               flags = 0;
                cg::db::FileId              file_id =
                    0; // 定义所在文件（TU 域判据，见 resolve）
            };

            /** @brief resolve 的产物：符号 id 与获胜层级的置信度。 */
            struct ResolvedCallee
            {
                std::optional<cg::db::SymId> id;
                /** @brief 获胜层级（1.0 / 0.75 / 0.7 / 0.4）。 */
                double confidence = 1.0;
            };

            void load(cg::db::GraphStore& store)
            {
                if (loaded_)
                {
                    return;
                }
                for (const cg::db::IndexRow& row :
                     cg::db::all_symbol_index_rows(store))
                {
                    if (by_key_.contains(row.unique_key))
                    {
                        continue;
                    }
                    Entry e;
                    e.id = row.id;
                    e.kind = row.kind;
                    e.name = row.name;
                    e.ns = row.ns;
                    e.parent = row.parent;
                    e.sig_hash = row.sig_hash;
                    e.const_method =
                        (row.flags & cg::db::sym_flags::is_const) != 0;
                    e.flags = row.flags;
                    e.file_id = row.file_id;
                    upsert_entry(row.unique_key, std::move(e));
                }
                loaded_ = true;
            }

            /**
             * @brief 阶段 A upsert 之后刷新一个条目（id 稳定，事实更新）。
             *
             * @internal 唯一键只在此并入 by_key_ 映射，不驻留 Entry——每个符号
             *           背一份键字符串拷贝在全库符号量级是纯白存的内存。
             */
            void upsert_entry(const std::string& key, Entry e)
            {
                retired_.erase(e.id);
                by_key_[key] = e.id;
                std::vector<Entry>& bucket = by_name_[e.name];
                for (Entry& existing : bucket)
                {
                    if (existing.id == e.id)
                    {
                        existing = std::move(e);
                        return;
                    }
                }
                bucket.push_back(std::move(e));
            }

            /**
             * @brief 标记本运行清除的符号：阶段 B 不得把新边解析到逐文件清除
             *        将删除的行上（全量重索引也会看到它们消失）。
             *
             * 阶段 A 重新 upsert 的 id 自动复活。
             */
            void retire(std::span<const cg::db::SymId> ids)
            {
                retired_.insert(ids.begin(), ids.end());
            }

            /**
             * @brief 清除提升的第 3 层歧义：本运行清除某符号后，其裸名从运行前
             *        的多候选变为恰剩一个（或多候选中剩唯一主模板——无签名
             *        依赖调用在第 3 层的落点）时为真。
             *
             * @internal
             * 此类名字的调用点在多余候选存活期间被作为歧义丢弃，现在可能解析
             * 成功——且可能在语料任何位置，因为丢弃的站点不留任何存储行供
             * 悬垂边波跟踪。按类别细分计数（函数形候选供 calls/overrides 的
             * 第 3 层类别过滤；全类别计数覆盖 type_alias 过滤的超集，过近似
             * 安全），使清除后名字仍歧义（无解析变化）或清空（无物可解析）的
             * 情形不升级。
             */
            [[nodiscard]]
            auto purge_lifts_name_ambiguity(
                const std::unordered_set<cg::db::SymId>& purged,
                const std::function<
                    bool(cg::db::SymId, cg::db::FileId, std::uint32_t)>&
                    terminal_survives = nullptr) const -> bool
            {
                if (purged.empty())
                {
                    return false;
                }
                // CCG_DEBUG_PURGE=1：升级判定观测（爆发归因与头级缓存启用判定共
                // 用）。命中桶逐个点名（触发名字与清除前后的候选计数），
                // 触碰桶与清除符号数汇总——无论命中与否都打，便于把
                // 「不升级的爆发」与「升级」区分开。
                const bool trace_purge =
                    cg::util::cstd::get_env("CCG_DEBUG_PURGE").has_value();
                std::size_t touched_buckets = 0;
                for (const auto& [name, bucket] : by_name_)
                {
                    std::size_t  pre_fn = 0;
                    std::size_t  post_fn = 0;
                    std::size_t  pre_fn_tpl = 0;
                    std::size_t  post_fn_tpl = 0;
                    std::size_t  pre_any = 0;
                    std::size_t  post_any = 0;
                    bool         touches = false;
                    const Entry* survivor = nullptr;
                    for (const Entry& e : bucket)
                    {
                        const bool dead = purged.contains(e.id);
                        touches = touches || dead;
                        ++pre_any;
                        if (!dead)
                        {
                            ++post_any;
                            survivor = &e;
                        }
                        if (!cg::db::is_callable(e.kind))
                        {
                            continue;
                        }
                        ++pre_fn;
                        if (!dead)
                        {
                            ++post_fn;
                        }
                        if ((e.flags & cg::db::sym_flags::is_template) != 0)
                        {
                            ++pre_fn_tpl;
                            if (!dead)
                            {
                                ++post_fn_tpl;
                            }
                        }
                    }
                    if (!touches || post_any == 0)
                    {
                        continue;
                    }
                    ++touched_buckets;
                    if (pre_fn >= 2
                        && (post_fn == 1
                            || (post_fn >= 2 && pre_fn_tpl >= 2
                                && post_fn_tpl == 1)))
                    {
                        const bool genuine = terminal_lift_genuine(
                            terminal_survives,
                            bucket,
                            purged);
                        trace_purge_bucket(
                            trace_purge,
                            genuine ? "HIT-fn" : "HIT-fn-VETOED",
                            name,
                            pre_fn,
                            post_fn,
                            pre_fn_tpl,
                            post_fn_tpl,
                            pre_any,
                            post_any,
                            survivor != nullptr ? *survivor : Entry{});
                        if (genuine)
                        {
                            return true;
                        }
                        continue;
                    }
                    if (pre_any >= 2 && post_any == 1)
                    {
                        const bool genuine = terminal_lift_genuine(
                            terminal_survives,
                            bucket,
                            purged);
                        trace_purge_bucket(
                            trace_purge,
                            genuine ? "HIT-any" : "HIT-any-VETOED",
                            name,
                            pre_fn,
                            post_fn,
                            pre_fn_tpl,
                            post_fn_tpl,
                            pre_any,
                            post_any,
                            survivor != nullptr ? *survivor : Entry{});
                        if (genuine)
                        {
                            return true;
                        }
                        continue;
                    }
                }
                if (trace_purge)
                {
                    try
                    {
                        std::println(
                            std::cerr,
                            "PURGE LIFT miss touched_buckets={} "
                            "purged={}",
                            touched_buckets,
                            purged.size());
                    }
                    catch (...)
                    {
                    }
                }
                return false;
            }

            [[nodiscard]]
            auto by_key(const std::string& key) const
                -> std::optional<cg::db::SymId>
            {
                if (const auto it = by_key_.find(key); it != by_key_.end())
                {
                    if (!retired_.contains(it->second))
                    {
                        return it->second;
                    }
                }
                return std::nullopt;
            }

            /**
             * @brief 是否至少一个已知符号携带此裸名——让导入器区分"项目里没有
             *        此名"（外部/未知被调用者，依赖调用噪声类）与"名字存在但
             *        无唯一匹配"（诚实歧义）。
             */
            [[nodiscard]]
            auto knows_name(const std::string& name) const -> bool
            {
                return by_name_.contains(name);
            }

            /**
             * @brief 是否至少存在一个会在 TU 域下受限的裸名——桶内（跳过
             *        retired）出现 ≥2 个互异的定义文件。调用方用它懒建
             *        TU 文件域，免得为每个 TU 白付一遍构造。
             */
            [[nodiscard]]
            auto name_spans_files(const std::string& name) const -> bool
            {
                const auto it = by_name_.find(name);
                if (it == by_name_.end() || it->second.size() < 2)
                {
                    return false;
                }
                std::optional<cg::db::FileId> seen;
                for (const Entry& e : it->second)
                {
                    if (retired_.contains(e.id))
                    {
                        continue;
                    }
                    if (!seen.has_value())
                    {
                        seen = e.file_id;
                    }
                    else if (*seen != e.file_id)
                    {
                        return true;
                    }
                }
                return false;
            }

            /**
             * @brief 三层级联解析一个被调用者元数据到符号 id。
             *
             * 返回值携带获胜层级的置信度（1.0 / 0.75 / 0.7 / 0.4）。
             *
             * @internal
             * 类别过滤：calls/overrides 仅函数形候选；type_alias 取记录与类型
             * 别名；uses 恒为记录——延迟 USES 目标（项目前置声明）的规范化
             * 遍历已剥掉 typedef，终点必是记录。
             * 层级：第 1 层名字 + 父类 + 命名空间 + 签名全精确（重载正确、
             * const 方法感知）；第 2 层名字 + 父类、命名空间尾精确；第 3 层
             * 项目内裸名唯一（歧义 = 不解析，计数超 8 即足证歧义而提前截断）。
             * 无签名的依赖调用（模板体、未实例化）在同名候选中取唯一主模板
             * 作既定落点（"调用落在模板的符号上"）。
             *
             * TU 域（internal-linkage 串线修复，早期反馈）：
             * - 定义：身份 = (父类, 命名空间, 签名, const)（第 1 层等价类）。
             *   同一身份在 ≥2 个文件各有定义行 = "跨 TU 孪生"。symbols 表只存
             *   定义（visitor 以 is_definition 门控），而 ODR 禁止外部链接实体
             *   多处定义，因此孪生必是 static/匿名命名空间函数（或记录）的
             *   逐 TU 拷贝——FooBiz 在 SampleStore.cpp 与
             *   SampleScheduler.cpp 各一份的形状。孪生候选仅在定义文件属于
             *   发射这条边的 TU（tu_scope，本 TU 符号定义文件的全集，含头）
             *   时可选；三层全部沿用同一判定，域内无候选则按序降级直至
             *   不解析。
             * - soundness 边界：ODR 违例的双外部定义会被同判为孪生——选本
             *   TU 的那份不劣于原先的桶序首个；单一文件内的 static 与匿名
             *   ns 同名同签名共存时二者皆在域内，桶序判定（同为合法目标）。
             *   单文件 internal-linkage 符号（项目里唯一同名定义）无孪生可
             *   侦测，仍按全局解析——提取侧无链接性旗标，图合并侧不可分；
             *   完整修复需 visitor 记 clang_getCursorLinkage，超出本簇文件
             *   边界。
             * - fallback 清单：域内无孪生候选 → 维持不解析（计入
             *   unresolved/ambiguous，knows_name 区分口径不变）；tu_scope 为
             *   nullptr（裸名无跨文件定义）→ 行为与旧版逐字节一致；同文件
             *   内的调用照常解析（站点文件天然在域内）。
             */
            [[nodiscard]]
            auto resolve(
                const cg::extract::CalleeMeta&            meta,
                cg::extract::EdgeKind                     edge_kind,
                const std::unordered_set<cg::db::FileId>* tu_scope) const
                -> ResolvedCallee
            {
                const auto it = by_name_.find(meta.name);
                if (it == by_name_.end())
                {
                    return ResolvedCallee{};
                }
                const std::vector<Entry>& cands = it->second;

                for (const Entry& e : cands)
                {
                    if (retired_.contains(e.id) || !kind_matches(edge_kind, e)
                        || !tu_scope_eligible(tu_scope, cands, e))
                    {
                        continue;
                    }
                    if (e.parent != meta.parent || e.ns != meta.ns
                        || e.sig_hash != meta.sig_hash
                        || e.const_method != meta.const_method)
                    {
                        continue;
                    }
                    return ResolvedCallee{.id = e.id, .confidence = 1.0};
                }
                // 重载归属判定（名字段之前的窄闸）：同名候选里"非模板+
                // 模板"并存、调用点实参是依赖类型（模板上下文——clang 已把
                // 调用解析到模板版但隐式实例符号不入库）时，选唯一模板版。
                // 判不动（多模板/无模板/无依赖标志）走原名字段——宁缺原序
                // 不硬改（防先入库非模板版吃掉全部调用归属的实测形态）。
                if (meta.arg_dep)
                {
                    std::optional<cg::db::SymId> sole_tmpl;
                    int                          tmpl_count = 0;
                    for (const Entry& e : cands)
                    {
                        if (retired_.contains(e.id)
                            || !kind_matches(edge_kind, e)
                            || !tu_scope_eligible(tu_scope, cands, e)
                            || e.parent != meta.parent
                            || !cg::analysis::ns_tail_match(e.ns, meta.ns))
                        {
                            continue;
                        }
                        if ((e.flags & cg::db::sym_flags::is_template) != 0)
                        {
                            ++tmpl_count;
                            if (tmpl_count == 1)
                            {
                                sole_tmpl = e.id;
                            }
                        }
                    }
                    if (tmpl_count == 1)
                    {
                        return ResolvedCallee{
                            .id = sole_tmpl,
                            .confidence = 0.75};
                    }
                }
                for (const Entry& e : cands)
                {
                    if (retired_.contains(e.id) || !kind_matches(edge_kind, e)
                        || !tu_scope_eligible(tu_scope, cands, e))
                    {
                        continue;
                    }
                    if (e.parent != meta.parent
                        || !cg::analysis::ns_tail_match(e.ns, meta.ns))
                    {
                        continue;
                    }
                    return ResolvedCallee{.id = e.id, .confidence = 0.7};
                }
                std::optional<cg::db::SymId> only;
                std::size_t                  hits = 0;
                std::size_t                  template_hits = 0;
                std::optional<cg::db::SymId> sole_template;
                for (const Entry& e : cands)
                {
                    if (retired_.contains(e.id) || !kind_matches(edge_kind, e)
                        || !tu_scope_eligible(tu_scope, cands, e))
                    {
                        continue;
                    }
                    ++hits;
                    if (hits == 1)
                    {
                        only = e.id;
                    }
                    if ((e.flags & cg::db::sym_flags::is_template) != 0)
                    {
                        ++template_hits;
                        if (template_hits == 1)
                        {
                            sole_template = e.id;
                        }
                    }
                    if (hits > 8)
                    {
                        break;
                    }
                }
                if (hits == 1)
                {
                    return ResolvedCallee{.id = only, .confidence = 0.4};
                }
                if (hits > 1 && sole_template.has_value()
                    && !meta.sig_hash.has_value())
                {
                    return ResolvedCallee{
                        .id = sole_template,
                        .confidence = 0.4};
                }
                return ResolvedCallee{};
            }

        private:
            bool                                                loaded_ = false;
            std::unordered_set<cg::db::SymId>                   retired_;
            std::unordered_map<std::string, cg::db::SymId>      by_key_;
            std::unordered_map<std::string, std::vector<Entry>> by_name_;

            /**
             * @brief 边类别对候选符号类别的过滤。
             *
             * @internal calls/overrides 仅函数形候选；type_alias 取记录与
             *           类型别名；uses 恒为记录——延迟 USES 目标（项目前
             *           置声明）的规范化遍历已剥掉 typedef，终点必是记录。
             */
            [[nodiscard]]
            static auto kind_matches(
                cg::extract::EdgeKind edge_kind,
                const Entry&          e) -> bool
            {
                switch (edge_kind)
                {
                case cg::extract::EdgeKind::Calls:
                case cg::extract::EdgeKind::Overrides:
                    return cg::db::is_callable(e.kind);
                case cg::extract::EdgeKind::TypeAlias:
                    return e.kind == cg::db::SymbolKind::Record
                           || e.kind == cg::db::SymbolKind::TypeAlias;
                case cg::extract::EdgeKind::Uses:
                    return e.kind == cg::db::SymbolKind::Record;
                default:
                    return true;
                }
            }

            /**
             * @brief 孪生判据：同身份（父类/命名空间/签名/const/函数形
             *        类别）在别的文件另有一份定义。
             *
             * @internal 只看未 retire 的候选。仅对域外候选求值（域内恒
             *           可选、tu_scope 为空时短路），最坏 O(bucket)，与
             *           各层主扫同阶。
             */
            [[nodiscard]]
            auto twin_elsewhere(const std::vector<Entry>& cands, const Entry& e)
                const -> bool
            {
                for (const Entry& o : cands)
                {
                    if (o.id == e.id || retired_.contains(o.id)
                        || o.file_id == e.file_id)
                    {
                        continue;
                    }
                    if (o.parent == e.parent && o.ns == e.ns
                        && o.sig_hash == e.sig_hash
                        && o.const_method == e.const_method
                        && cg::db::is_callable(o.kind)
                               == cg::db::is_callable(e.kind))
                    {
                        return true;
                    }
                }
                return false;
            }

            /**
             * @brief TU 域判定：域内候选恒可选；域外候选仅当无孪生（单一
             *        文件身份 = 外部链接或头文件共享行）时保留。
             *
             * @internal 外部函数的跨文件解析（TU A 调用 lib.cpp 的定义）
             *           不能被误伤。
             */
            [[nodiscard]]
            auto tu_scope_eligible(
                const std::unordered_set<cg::db::FileId>* tu_scope,
                const std::vector<Entry>&                 cands,
                const Entry&                              e) const -> bool
            {
                return tu_scope == nullptr || tu_scope->contains(e.file_id)
                       || !twin_elsewhere(cands, e);
            }

            /**
             * @brief 清除升级判定的逐桶观测行。
             *
             * @internal 命中桶逐个点名（触发名字与清除前后的候选计数），
             *           CCG_DEBUG_PURGE=1 时才打印（爆发归因与头级缓存
             *           启用判定共用的观测面）。
             */
            static void trace_purge_bucket(
                bool               trace,
                std::string_view   verdict,
                const std::string& bucket_name,
                std::size_t        pre_fn_n,
                std::size_t        post_fn_n,
                std::size_t        pre_fn_tpl_n,
                std::size_t        post_fn_tpl_n,
                std::size_t        pre_any_n,
                std::size_t        post_any_n,
                const Entry&       survivor)
            {
                if (!trace)
                {
                    return;
                }
                try
                {
                    std::println(
                        std::cerr,
                        "PURGE LIFT {} name='{}' pre_fn={} post_fn={} "
                        "pre_tpl={} post_tpl={} pre_any={} post_any={} "
                        "survivor_id={} survivor_file={} survivor_sig={}",
                        verdict,
                        bucket_name,
                        pre_fn_n,
                        post_fn_n,
                        pre_fn_tpl_n,
                        post_fn_tpl_n,
                        pre_any_n,
                        post_any_n,
                        survivor.id,
                        survivor.file_id,
                        survivor.sig_hash.has_value()
                            ? std::format("{}", *survivor.sig_hash)
                            : std::string{"-"});
                }
                catch (...)
                {
                }
            }

            /**
             * @brief 终态复核闸（假触发否决）：桶内被清成员在终态是否
             *        有真死亡者。
             *
             * @internal 瞬态 doomed 集把桶清到单候选只是升级的必要条件；
             *           桶若在本批终态（新提取并集 ∪ 重写集内未变文件的
             *           完整重建）原样回来，升级要抢救的「新可解析调用
             *           点」不存在。回调缺席（空函数）= 复核关闭。
             */
            [[nodiscard]]
            static auto terminal_lift_genuine(
                const std::function<
                    bool(cg::db::SymId, cg::db::FileId, std::uint32_t)>&
                                                         terminal_survives,
                const std::vector<Entry>&                bucket,
                const std::unordered_set<cg::db::SymId>& purged) -> bool
            {
                if (terminal_survives == nullptr)
                {
                    return true;
                }
                for (const Entry& e : bucket)
                {
                    if (purged.contains(e.id)
                        && !terminal_survives(e.id, e.file_id, e.flags))
                    {
                        return true; // 有真死亡成员：桶真会缩小
                    }
                }
                return false;
            }
        };

        /**
         * @brief 符号与仲裁账目域：内存符号解析索引、未解析名观测与
         *        heal 清票受影响集。
         *
         * @internal 解析池侧的预载与复核查询、提交侧的 upsert 维护共享
         *           同一实例；heal 受影响集先读后删，与显式撤票同纪律。
         */
        struct SymLedger
        {
            SymLedger() = default;
            SymLedger(const SymLedger&) = delete;
            SymLedger& operator=(const SymLedger&) = delete;
            SymLedger(SymLedger&&) = delete;
            SymLedger& operator=(SymLedger&&) = delete;

            SymbolIndex                         sym_index_;
            std::map<std::string, std::int64_t> unresolved_names_;
            std::map<std::string, std::int64_t> unknown_names_;
            /** @brief heal 折叠删除 loser 文件时被 CASCADE 清票的符号集
             *         （归零仲裁候选的补集通道；先读后删，与显式撤票同纪
             *         律——删除交给 FK，受影响集交给记账块）。 */
            std::vector<cg::db::SymId> heal_vote_affected_;
        };
    } // namespace detail
} // namespace cg::graph
