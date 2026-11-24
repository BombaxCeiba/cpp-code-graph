/**
 * @module cg.graph.waveflush
 *
 * @brief 波式分解域（值语义域对象）：解析期逐波拆解的驻留槽、账本与
 *        intern 表，及提交期回放的判据。
 *
 * @internal cg.graph.build 以值成员嵌入并再导出本单元。构建族共用的
 *           哈希混合步（mix_hash）唯一定义在此，编排层经导入消费。
 */

export module cg.graph.waveflush;

import std;
import cg.util;
import cg.db;
import cg.clang;
import cg.extract;
import cg.analysis;

export namespace cg::graph
{
    namespace detail
    {
        /** @brief 哈希混合步（黄金比例扰动；组合键哈希的单点实现）。 */
        // ccg-out-param: 哈希种子读改写（seed 进出的标准 combine
        // 形态，非结果承载）
        constexpr void mix_hash(std::size_t& h, std::size_t v) noexcept
        {
            h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }

        /**
         * @brief 波式分解域：解析期逐波把成功 TU 的重型载荷拆解进
         *        "槽 + 账本"并即刻释放原始载荷（内存峰值的收益来源）。
         *
         * @internal 提交事务结构不变：一切库写入仍在主事务内（符号发射、
         *           文件封口 purge、边后置、lambda/站点/包含全部原位），
         *           波内零库写（kill 重做窗口因此天然干净）。开关走
         *           CCG_WAVE_SIZE 环境变量（0=关=单趟；>0=按 jobs 序切波，
         *           波不切同主文件的相邻 TU）。域对象删除拷贝与移动——
         *           fb_key_list_/fb_path_list_ 持有指向同域 intern map 节点
         *           的指针，对象一旦搬移指针即悬垂。
         */
        struct WaveFlush
        {
            WaveFlush() = default;
            WaveFlush(const WaveFlush&) = delete;
            WaveFlush& operator=(const WaveFlush&) = delete;
            WaveFlush(WaveFlush&&) = delete;
            WaveFlush& operator=(WaveFlush&&) = delete;

            /**
             * @brief 符号槽：首现建槽定序（SymId 分配序），末次载荷覆写
             *        （与提交段 SymSlot 的首/末语义同构，载荷从解析产物搬移）。
             */
            struct FlushSymSlot
            {
                cg::extract::SymbolRec
                            rec; // 末次出现载荷（搬移而来，槽序冻结后视图稳定）
                std::string parent_key; // 末次载荷的父记录键（空 = 无父）
            };

            /** @brief 边槽的 TU 贡献（受限裸名按 TU 域分别解析的回放原料）。 */
            struct FlushEdgeContrib
            {
                std::uint32_t tu = 0;          // 账本 TU 序号
                std::int64_t  occurrences = 0; // 该 TU 内的出现折叠和
                std::int64_t  emissions =
                    0; // 该 TU 内的原始发射数（unresolved 口径）
            };

            /**
             * @brief 边槽：字符串键去重的原始边（解析与 SymId/file_id 解析全部
             *        后置到提交事务——届时符号索引才含本批符号）。
             *
             * @internal 等价性根据：(src 键, dst 身份, kind, 站点路径, 行) 到
             *           解析后站点键的映射是单射——字符串键去重与现状的"解析后
             *           键去重"在行集与行序上逐字节等价（跨 TU 同键折叠、受限
             *           裸名按贡献表逐 TU 回放解析）。
             */
            struct FlushEdgeSlot
            {
                std::string             src_key;
                bool                    dst_is_key = false;
                std::string             dst_key;  // is_key 形态的目标键
                cg::extract::CalleeMeta dst_meta; // 延迟形态的解析元数据
                cg::extract::EdgeKind   kind = cg::extract::EdgeKind::Calls;
                std::string             site_file;
                int                     site_line = 0;
                std::uint32_t attrs = 0;       // 首写胜出（与 EdgeSlot 同构）
                std::int64_t  occurrences = 0; // 出现折叠总数
                std::int64_t  emissions =
                    0; // 原始发射总数（unresolved 计数口径）
                std::vector<FlushEdgeContrib>
                    contribs; // 按 TU 出现序（同 TU 合并）
            };

            /**
             * @brief 账本 TU：提交段与解析后清扫块要消费的轻量载荷，加上
             *        keep 集 / 定义文件集（tu_scope 等价重建原料）。
             */
            struct FlushLedgerTu
            {
                std::size_t job = 0; // 分解时的 job 下标（与 jobs 序同序追加）
                const cg::clang::CompileCommand* cmd = nullptr;
                cg::db::FileId                   fid = 0;
                std::string digest; // 条目命令摘要（贡献票的条目身份）
                cg::analysis::TuAnalysis
                    analysis; // 整体搬移（lambda/站点/线索/桥）
                std::vector<cg::extract::IncludeRec>
                    includes; // 包含差与回写原料
                std::vector<cg::extract::ImportRec> imports;
                std::vector<std::uint32_t>
                    keep_keys; // 本 TU 发射的唯一键 intern 号（升序）
                std::vector<std::uint32_t>
                    def_paths; // 本 TU 定义文件路径 intern 号（升序）
                /**
                 * @brief 本 TU 的边槽首现序（提交段回放序）。
                 *
                 * @internal 建槽即 rowid：现状按全局发射序创建 EdgeSlot——受限
                 *           裸名的首次解析成功可能晚于其首次发射，槽序≠发射序
                 *           会漂移 rowid。账本按「TU × 本序」回放严格同构。
                 */
                std::vector<std::uint32_t> edge_seq;
            };

            /**
             * @brief lambda 槽：首现定序、末次载荷覆写（upsert_symbol 冲突走
             *        SYM_REFRESH 刷列——末写胜出）；锚定 TU 经账本索引间接持有
             *        （随陈旧快照核对的 fid 修补联动）。
             */
            struct FlushLamSlot
            {
                cg::analysis::LambdaSymRec rec;
                std::uint32_t              tu = 0; // 末次出现的账本 TU 序号
            };

            bool      fb_active_ = false; // 本运行启用了波式分解
            long long fb_wave_size_ = 0;  // 波大小（TU 数；0 = 关）
            /** @brief keep 集消费游标（账本与 jobs 序同序，单调推进）。 */
            std::size_t               fb_keep_cursor_ = 0;
            std::vector<FlushLamSlot> fb_lams_;
            std::unordered_map<
                std::string,
                std::uint32_t,
                cg::util::string_hash,
                std::equal_to<>>
                fb_lam_of_;
            /** @brief 类型线索槽：首现定序+首写胜出（upsert 冲突忽略不刷列）。
             */
            std::vector<cg::analysis::TypeHintRec> fb_hints_;
            std::unordered_map<std::size_t, std::vector<std::uint32_t>>
                fb_hint_of_;
            /** @brief 调用点槽：首现定序+首写胜出（同上，INSERT 后无刷列）。 */
            std::vector<cg::analysis::CallSiteRec> fb_sites_;
            std::unordered_map<std::size_t, std::vector<std::uint32_t>>
                fb_site_of_;
            /** @brief 波末拆解累计墙钟（微秒；开销观测面）。 */
            double                    t_flush_us_ = 0;
            std::int64_t              fb_waves_ = 0; // 已拆解波数（观测面）
            std::vector<FlushSymSlot> fb_syms_;
            std::unordered_map<
                std::string,
                std::uint32_t,
                cg::util::string_hash,
                std::equal_to<>>
                fb_sym_of_; // 拥有键串；值 = fb_syms_ 下标
            std::vector<FlushEdgeSlot> fb_edges_;
            /** @brief 边槽哈希桶：哈希（槽身份）→
             * 候选槽下标（冲突链上逐槽比对）。
             */
            std::unordered_map<std::size_t, std::vector<std::uint32_t>>
                                       fb_edge_of_;
            std::vector<FlushLedgerTu> fb_ledger_;
            std::unordered_map<
                std::string,
                std::uint32_t,
                cg::util::string_hash,
                std::equal_to<>>
                fb_key_intern_; // 唯一键串 → intern 号
            std::vector<const std::string*>
                fb_key_list_; // intern 号 → 键串（节点稳定）
            std::unordered_map<
                std::string,
                std::uint32_t,
                cg::util::string_hash,
                std::equal_to<>>
                fb_path_intern_;                           // 路径串 → intern 号
            std::vector<const std::string*> fb_path_list_; // intern 号 → 路径串
            /** @brief 文件 → 本批新发射键 intern
             * 号集（覆盖差判定的等价重建输入）。
             */
            std::
                unordered_map<cg::db::FileId, std::unordered_set<std::uint32_t>>
                    fb_fresh_union_;

            /**
             * @brief 唯一键串 → intern 号（首见建号）。
             *
             * @internal 键串拥有权在 fb_key_intern_（map 节点稳定），列表存
             *           节点指针；只 push 不删的纪律保证指针不失效。
             */
            [[nodiscard]]
            std::uint32_t intern_fb_key_(std::string_view s)
            {
                const auto [it, fresh] = fb_key_intern_.try_emplace(
                    std::string{s},
                    static_cast<std::uint32_t>(fb_key_list_.size()));
                if (fresh)
                {
                    fb_key_list_.push_back(&it->first);
                }
                return it->second;
            }

            /**
             * @brief 路径串 → intern 号（首见建号，拥有权结构同
             *        intern_fb_key_）。
             */
            [[nodiscard]]
            std::uint32_t intern_fb_path_(std::string_view s)
            {
                const auto [it, fresh] = fb_path_intern_.try_emplace(
                    std::string{s},
                    static_cast<std::uint32_t>(fb_path_list_.size()));
                if (fresh)
                {
                    fb_path_list_.push_back(&it->first);
                }
                return it->second;
            }

            /**
             * @brief 边身份哈希：字符串键去重的哈希半边。
             *
             * @internal 等价性根据见 FlushEdgeSlot 的说明——(src 键, dst 身份,
             *           kind, 站点路径, 行) 到解析后站点键的映射是单射，字符
             *           串键去重与解析后键去重在行集与行序上逐字节等价。
             */
            [[nodiscard]]
            static auto edge_hash_(const cg::extract::EdgeRec& e) -> std::size_t
            {
                std::size_t h = std::hash<std::string_view>{}(e.src_key);
                detail::mix_hash(h, e.dst.is_key ? 1 : 0);
                detail::mix_hash(h, std::hash<std::string_view>{}(e.dst.key));
                detail::mix_hash(
                    h,
                    std::hash<std::string_view>{}(e.dst.meta.name));
                detail::mix_hash(
                    h,
                    std::hash<std::string_view>{}(e.dst.meta.ns));
                detail::mix_hash(
                    h,
                    std::hash<std::string_view>{}(e.dst.meta.parent));
                detail::mix_hash(
                    h,
                    e.dst.meta.sig_hash.has_value()
                        ? static_cast<std::size_t>(*e.dst.meta.sig_hash)
                        : std::size_t{0});
                detail::mix_hash(h, e.dst.meta.const_method ? 1 : 0);
                detail::mix_hash(h, static_cast<std::size_t>(e.dst.meta.kind));
                detail::mix_hash(h, e.dst.meta.arg_dep ? 1 : 0);
                detail::mix_hash(h, static_cast<std::size_t>(e.kind));
                detail::mix_hash(h, std::hash<std::string_view>{}(e.site_file));
                detail::mix_hash(h, static_cast<std::size_t>(e.site_line));
                return h;
            }

            /**
             * @brief 边身份相等：哈希冲突链上的逐槽比对判据（与 edge_hash_
             *        同键面）。
             */
            [[nodiscard]]
            static auto edge_eq_(
                const FlushEdgeSlot&        s,
                const cg::extract::EdgeRec& e) -> bool
            {
                if (s.src_key != e.src_key || s.dst_is_key != e.dst.is_key
                    || s.kind != e.kind || s.site_file != e.site_file
                    || s.site_line != e.site_line)
                {
                    return false;
                }
                if (e.dst.is_key)
                {
                    return s.dst_key == e.dst.key;
                }
                return s.dst_meta.name == e.dst.meta.name
                       && s.dst_meta.ns == e.dst.meta.ns
                       && s.dst_meta.parent == e.dst.meta.parent
                       && s.dst_meta.sig_hash == e.dst.meta.sig_hash
                       && s.dst_meta.const_method == e.dst.meta.const_method
                       && s.dst_meta.kind == e.dst.meta.kind
                       && s.dst_meta.arg_dep == e.dst.meta.arg_dep;
            }

            /**
             * @brief 单 TU 边拆解：字符串键身份去重入槽，贡献按 TU 记账。
             *
             * @internal 首写胜出建槽（attrs/occurrences 取首值），命中槽累加
             *           出现与发射计数；同 TU 的连续贡献合并进贡献表尾项，
             *           建槽与新 TU 贡献把槽号记入账本 edge_seq（提交段回放
             *           序——建槽即 rowid 的等价性据此保住）。
             */
            void decompose_edges_(
                const cg::extract::TuExtract& ex,
                FlushLedgerTu&                lt,
                std::uint32_t                 tu_idx)
            {
                for (const cg::extract::EdgeRec& e : ex.edges)
                {
                    std::vector<std::uint32_t>& chain =
                        fb_edge_of_[edge_hash_(e)];
                    std::uint32_t slot_idx = 0;
                    bool          found = false;
                    for (const std::uint32_t ci : chain)
                    {
                        if (edge_eq_(fb_edges_[ci], e))
                        {
                            slot_idx = ci;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        slot_idx = static_cast<std::uint32_t>(fb_edges_.size());
                        FlushEdgeSlot& slot = fb_edges_.emplace_back();
                        slot.src_key = e.src_key;
                        slot.dst_is_key = e.dst.is_key;
                        if (e.dst.is_key)
                        {
                            slot.dst_key = e.dst.key;
                        }
                        else
                        {
                            slot.dst_meta = e.dst.meta;
                        }
                        slot.kind = e.kind;
                        slot.site_file = e.site_file;
                        slot.site_line = e.site_line;
                        slot.attrs = e.attrs; // 首写胜出
                        slot.occurrences = e.occurrences;
                        slot.emissions = 1;
                        slot.contribs.push_back(
                            FlushEdgeContrib{
                                .tu = tu_idx,
                                .occurrences = e.occurrences,
                                .emissions = 1});
                        chain.push_back(slot_idx);
                        lt.edge_seq.push_back(slot_idx);
                    }
                    else
                    {
                        FlushEdgeSlot& slot = fb_edges_[slot_idx];
                        slot.occurrences += e.occurrences;
                        slot.emissions += 1;
                        if (!slot.contribs.empty()
                            && slot.contribs.back().tu == tu_idx)
                        {
                            slot.contribs.back().occurrences += e.occurrences;
                            slot.contribs.back().emissions += 1;
                        }
                        else
                        {
                            slot.contribs.push_back(
                                FlushEdgeContrib{
                                    .tu = tu_idx,
                                    .occurrences = e.occurrences,
                                    .emissions = 1});
                            lt.edge_seq.push_back(slot_idx);
                        }
                    }
                }
            }

            /**
             * @brief 单 TU 分析载荷去重入槽（槽+账本的本义补全）。
             *
             * @internal lambda：末写胜出（upsert_symbol 冲突走 SYM_REFRESH 刷
             *           列），锚定 TU 记账本序号（随陈旧快照 fid 修补联动）。
             *           线索/站点：首写胜出（upsert 冲突忽略不刷列）——首现即
             *           首插（rowid 与列载荷同现状），键到库级唯一键单射。
             */
            void decompose_analysis_(FlushLedgerTu& lt, std::uint32_t tu_idx)
            {
                for (cg::analysis::TypeHintRec& h : lt.analysis.hints)
                {
                    std::size_t kh = std::hash<std::string_view>{}(h.owner_key);
                    detail::mix_hash(kh, std::hash<std::string_view>{}(h.file));
                    detail::mix_hash(kh, static_cast<std::size_t>(h.line));
                    detail::mix_hash(kh, static_cast<std::size_t>(h.col));
                    detail::mix_hash(
                        kh,
                        std::hash<std::string_view>{}(h.var_name));
                    detail::mix_hash(
                        kh,
                        static_cast<std::size_t>(static_cast<int>(h.source)));
                    std::vector<std::uint32_t>& chain = fb_hint_of_[kh];
                    bool                        dup = false;
                    for (const std::uint32_t ci : chain)
                    {
                        const cg::analysis::TypeHintRec& e = fb_hints_[ci];
                        if (e.owner_key == h.owner_key && e.file == h.file
                            && e.line == h.line && e.col == h.col
                            && e.var_name == h.var_name && e.source == h.source)
                        {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup)
                    {
                        chain.push_back(
                            static_cast<std::uint32_t>(fb_hints_.size()));
                        fb_hints_.push_back(std::move(h));
                    }
                }
                for (cg::analysis::CallSiteRec& c : lt.analysis.call_sites)
                {
                    std::size_t kh =
                        std::hash<std::string_view>{}(c.caller_key);
                    detail::mix_hash(kh, std::hash<std::string_view>{}(c.file));
                    detail::mix_hash(kh, static_cast<std::size_t>(c.line));
                    detail::mix_hash(kh, static_cast<std::size_t>(c.col));
                    detail::mix_hash(kh, c.is_virtual ? 1 : 0);
                    detail::mix_hash(kh, c.indirect ? 1 : 0);
                    std::vector<std::uint32_t>& chain = fb_site_of_[kh];
                    bool                        dup = false;
                    for (const std::uint32_t ci : chain)
                    {
                        const cg::analysis::CallSiteRec& e = fb_sites_[ci];
                        if (e.caller_key == c.caller_key && e.file == c.file
                            && e.line == c.line && e.col == c.col
                            && e.is_virtual == c.is_virtual
                            && e.indirect == c.indirect)
                        {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup)
                    {
                        chain.push_back(
                            static_cast<std::uint32_t>(fb_sites_.size()));
                        fb_sites_.push_back(std::move(c));
                    }
                }
                for (cg::analysis::LambdaSymRec& l : lt.analysis.lambda_symbols)
                {
                    const auto [it, fresh] = fb_lam_of_.try_emplace(
                        l.key,
                        static_cast<std::uint32_t>(fb_lams_.size()));
                    if (fresh)
                    {
                        fb_lams_.push_back(
                            FlushLamSlot{.rec = std::move(l), .tu = tu_idx});
                    }
                    else
                    {
                        FlushLamSlot& slot = fb_lams_[it->second];
                        slot.rec = std::move(l);
                        slot.tu = tu_idx;
                    }
                }
            }

            /**
             * @brief fresh 提取并集里文件是否仍持有键。
             *
             * @internal 波式分解下并集按 intern 号存储，键经 intern 表回查；
             *           两条通道（内存并集/波并集）读取侧等价。
             */
            [[nodiscard]]
            auto fresh_union_has_(
                const std::unordered_map<
                    cg::db::FileId,
                    std::unordered_set<std::string>>& fresh_union,
                cg::db::FileId                        f,
                const std::string&                    key) const -> bool
            {
                if (!fb_active_)
                {
                    const auto it = fresh_union.find(f);
                    return it != fresh_union.end() && it->second.contains(key);
                }
                const auto it = fb_fresh_union_.find(f);
                if (it == fb_fresh_union_.end())
                {
                    return false;
                }
                const auto kit = fb_key_intern_.find(std::string_view{key});
                return kit != fb_key_intern_.end()
                       && it->second.contains(kit->second);
            }

            /** @brief 复核退路：清空全部波式缓冲（整批重解析前调用）。 */
            void fb_reset_()
            {
                fb_keep_cursor_ = 0;
                std::vector<FlushLamSlot>().swap(fb_lams_);
                std::unordered_map<
                    std::string,
                    std::uint32_t,
                    cg::util::string_hash,
                    std::equal_to<>>()
                    .swap(fb_lam_of_);
                std::vector<cg::analysis::TypeHintRec>().swap(fb_hints_);
                std::unordered_map<std::size_t, std::vector<std::uint32_t>>()
                    .swap(fb_hint_of_);
                std::vector<cg::analysis::CallSiteRec>().swap(fb_sites_);
                std::unordered_map<std::size_t, std::vector<std::uint32_t>>()
                    .swap(fb_site_of_);
                std::vector<FlushSymSlot>().swap(fb_syms_);
                std::unordered_map<
                    std::string,
                    std::uint32_t,
                    cg::util::string_hash,
                    std::equal_to<>>()
                    .swap(fb_sym_of_);
                std::vector<FlushEdgeSlot>().swap(fb_edges_);
                std::unordered_map<std::size_t, std::vector<std::uint32_t>>()
                    .swap(fb_edge_of_);
                std::vector<FlushLedgerTu>().swap(fb_ledger_);
                std::unordered_map<
                    std::string,
                    std::uint32_t,
                    cg::util::string_hash,
                    std::equal_to<>>()
                    .swap(fb_key_intern_);
                std::vector<const std::string*>().swap(fb_key_list_);
                std::unordered_map<
                    std::string,
                    std::uint32_t,
                    cg::util::string_hash,
                    std::equal_to<>>()
                    .swap(fb_path_intern_);
                std::vector<const std::string*>().swap(fb_path_list_);
                std::unordered_map<
                    cg::db::FileId,
                    std::unordered_set<std::uint32_t>>()
                    .swap(fb_fresh_union_);
            }

            /**
             * @brief 复核退路的波式侧联动：清空全部波式缓冲并关闭本运行
             *        的波式开关（整批重解析前调用）。
             */
            void abort_recheck_()
            {
                fb_reset_();
                fb_active_ = false;
            }
        };
    } // namespace detail
} // namespace cg::graph
