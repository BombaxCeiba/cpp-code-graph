/**
 * @module cg.analysis.propagate
 *
 * @brief 类型集传播：函数内（别名/拷贝/返回槽）与全局固定点
 *        （跨函数、跨 TU 的实参 -> 形参、返回值 -> 调用点）。
 *
 * @internal 输入是 db 中持久化的 type_hints/call_sites（schema v10）；
 * 输出是纯内存的传播状态（每调用点的接收者动态类型集 + 每个线索的
 * 最终类型集），由 analysis.dispatch 消费。依赖方向：analysis →
 * db/model。固定点带迭代上限与确定性顺序；触顶如实标记
 * budget_exhausted，绝不静默截断成一个猜测。MAX_TYPES 的类型集
 * 截断另有槽级 truncated_here 观测并随拷贝路径传染（定义与边界见
 * propagate_detail::HintSlot）。
 *
 * 求值形态（semi-naive）：类型集 intern 成按字典序赋号的小整数
 * （整数序 == 字符串序，排序/去重/截断与字符串形态逐字节等价），
 * 轮内四阶段（函数内别名 / 实参→形参 / 出参写回 / 返回值→调用点）
 * 保持全扫版的阶段与扫描顺序，用脏
 * 队列只处理"自上次处理后输入有变化"的槽/站点；被跳过的访问是可
 * 证明的 no-op，固定点与全扫版一致。db.tables.h 置于全局
 * 模块片段：本单元调用 cg.db 的查询函数，MSVC 要求 sqlpp23 表模型
 * 定义在此可见，以合并模块记录的模板特化（完整机理见 db.tables.h）。
 */
module;
#include <cassert>
#include <cstdint>
#include <new>

#include "../db/db.tables.h"
#include "symbits.kernels.h"

export module cg.analysis.propagate;

import std;
import cg.util;
import cg.util.platform;
import cg.analysis.model;
import cg.analysis.points_to;
import cg.analysis.snapshot;
import cg.db;

export namespace cg::analysis
{

    /** @brief 传播后的一个线索槽。 */
    struct PropagatedHint
    {
        std::string              owner_key;
        std::string              var_name;
        std::vector<std::string> types;
        std::vector<std::string> static_types;
        Confidence               confidence = Confidence::Fallback;
        HintSource               source = HintSource::Declared;
        std::string              reason;
        /** @brief 槽级截断观测的透出（三要素定义见
         *         propagate_detail::HintSlot::truncated_here）。 */
        bool truncated_here = false;
        /**
         * @brief 本次运行解析出的函数内别名源槽键（"" = 无别名边）。
         * @internal 落 analysis_slots.alias_src：下一轮增量据此检测
         *           "源槽漂移/消失"（删除传播的见证）。
         */
        std::string alias_src;
        /** @brief 同上，返回值传播源槽键（analysis_slots.ret_src）。 */
        std::string ret_src;
        /**
         * @brief 本槽终态是否为增量 warm 原值直通（未进脏闭包重算）。
         * @internal 写回侧消费契约：true 时 type_set/confidence/source/
         *           reason/truncated/alias_src/ret_src 八列与 analysis_slots
         *           既有行恒等，写回可跳过 json 物化与逐列比对直接免写。
         *           恒等论证：前五列由 warm 回填直拷或经保序 intern 往返
         *           （warm.type_set 是本管线 json_string_array(排序串集)
         *           写入的规范形态，parse 后重物化恒等）；alias_src/ret_src
         *           由增量种子段的漂移检查背书（src 键漂移恒种子进闭包，
         *           闭包外必与 warm 行相等）。全量路径恒 false，行为不变。
         */
        bool from_warm = false;
    };

    /** @brief 传播后的一个调用点。 */
    struct PropagatedSite
    {
        std::int64_t site_id = 0;
        std::string  caller_key;
        /** @brief 调用者所属记录（`this->field` 接收者的槽所有者）。 */
        std::string caller_owner_key;
        bool        is_virtual = false;
        bool        qualified = false;
        bool        indirect = false;
        std::string receiver_name;
        /**
         * @brief 接收者形态位（双态，与 CallSiteRec 同义）：name 非空 =
         *        经 `this->field` 取出；name 空 + 位 true = 接收者表达式
         *        在场但采集层未解析（决议端隐式 this 回退见位即不接管）。
         */
        bool                                  receiver_is_this = false;
        std::string                           receiver_owner_key;
        std::string                           file;
        std::int64_t                          file_id = 0;
        int                                   line = 0;
        std::vector<std::string>              static_types;
        std::vector<std::string>              dynamic_types;
        std::vector<std::vector<std::string>> arg_types;
        std::vector<std::string>              arg_vars;
        std::vector<std::string>              arg_call_keys;
        std::optional<std::int64_t>           callee_id;
        std::string                           callee_key;
        MethodRef                             callee;
        bool                                  callee_resolved = false;
        std::optional<std::int64_t>           method_id;
        std::string                           method_key;
        Confidence                            resolution = Confidence::Fallback;
        Confidence                            evidence = Confidence::Fallback;
        std::string                           reason;
        /**
         * @brief 接收者槽的截断观测透出（HintSlot::truncated_here）：
         *        本站点的动态类型证据可能不完整（截断或传染）。
         * @internal dispatch 主路径消费契约：在 fallback_reason 追加
         *           ";slot_type_truncated"（模仿 ";candidates_truncated"
         *           的后缀拼接，既有串非空才加分号）；早退路径（indirect/
         *           not_virtual/qualified/no_callee_symbol）不加，避免破坏
         *           测试对 fallback_reason 的精确相等断言。
         */
        bool slot_type_truncated = false;
    };

    /** @brief 一次传播的完整状态。 */
    struct PropagationState
    {
        std::vector<PropagatedHint> hints;
        std::vector<PropagatedSite> sites;
        /**
         * @brief 装载期的 call_sites 原始行（site_id -> 回写前的现值）。
         * @internal 写回 diff 化的比对基准：dispatch 解析结果与行现值逐
         *           列相等时跳过 refresh_call_site（值基判定，无语义风险）。
         */
        std::vector<cg::db::CallSiteRow> source_sites;
        int                              rounds = 0;
        bool                             budget_exhausted = false;
        std::int64_t                     work_items = 0;
        /**
         * @brief 本次运行是否走增量路径（增量 = warm start + 脏闭包重算）。
         * @internal false（含一切降级形态）时与全量路径逐行为同一段代码；
         *           true 时 dispatch 按下面 site_affected 的掩码只解析受
         *           影响站点。
         */
        bool incremental = false;
        /**
         * @brief 与 sites 对齐的受影响标记（incremental 时有效）。
         * @internal affected = 站点在脏文件内 ∪ 跨文件扇入命中 ∪ 实参源
         *           漂移 ∪ 接收者槽在脏闭包内；同一 (caller, file, line)
         *           三元组按组扩张（边删除以三元组为键，同行多站点必须
         *           一起重解析）。
         */
        std::vector<char> site_affected;
        /**
         * @brief 站点五桶下标表（dispatch 消费的附加视图；sites AoS 保留）。
         * @internal 桶序即 dispatch::resolve 的早退判定链（indirect →
         *           not_virtual → qualified → no_method → virtual），桶内
         *           下标升序（稳定下标映射，不重排 AoS），与 sites 同下标
         *           域。装载物化时同步产出；dispatch 据此把逐站点早退分支
         *           换成桶级批量处理（等价性见 resolve 内注释）。
         */
        std::array<std::vector<std::uint32_t>, 5> site_buckets;
        /**
         * @brief 分段计时（微秒）：装载（表读入+warm+类型 intern）、边静态
         *        预解析、增量闭包/warm 回填、传播四阶段、终态物化。
         * @internal 传播段归因观测面：写入 meta（analysis_prop_*_us），
         *           `ccg stats` 读取；用于校准装载列裁剪/槽键整数化/类型集
         *           arena 化各件的预估收益（各段只在对应件上受益）。
         */
        std::int64_t load_us = 0;
        std::int64_t resolve_us = 0;
        std::int64_t closure_us = 0;
        std::int64_t arg_param_us = 0;
        std::int64_t param_fanout_us = 0;
        std::int64_t writeback_us = 0;
        std::int64_t return_flow_us = 0;
        std::int64_t finalize_us = 0;
        /**
         * @brief 虚族并集写回边数（直连槽无行时沿 overrides 族回传的边）。
         * @internal 观测面：写入 meta（analysis_prop_family_wb_edges），
         *           `ccg stats` 读取；族查询命中间数与救活面规模的直接
         *           观测，索引轮调试输出同源。口径：预解析段建边处计数
         *           （通道对数）——静态面裁剪后零投递的对也计入；实际收
         *           到族写回的槽数见 family_wb_fed_slots（双数字披露）。
         */
        std::size_t family_wb_edges = 0;
        /**
         * @brief 虚族扇投入参边数（直连形参槽不存在时沿 overrides 族投
         *        递的实参→形参边）。
         * @internal 观测面：写入 meta（analysis_prop_family_pf_edges），
         *           `ccg stats` 读取；族扇投命中面规模的直接观测，索引
         *           轮调试输出同源。口径：预解析段建边处计数（通道对
         *           数）——静态面裁剪后零投递的对也计入；实际被扇投的
         *           槽数见 family_pf_fed_slots（双数字披露）。
         */
        std::size_t family_pf_edges = 0;
        /**
         * @brief 实际收到虚族并集写回的槽数（终态物化段统计）。
         * @internal 观测面：写入 meta（analysis_prop_family_wb_slots）。
         *           与 family_wb_edges（通道对数）构成双数字披露：边数
         *           含静态面裁剪后零投递的通道对，槽数是实际投递规模。
         */
        std::size_t family_wb_fed_slots = 0;
        /**
         * @brief 实际收到入参族扇投的槽数（终态物化段统计）。
         * @internal 观测面：写入 meta（analysis_prop_family_pf_slots）。
         *           与 family_pf_edges（通道对数）构成双数字披露：边数
         *           含静态面裁剪后零投递的通道对，槽数是实际投递规模。
         */
        std::size_t family_pf_fed_slots = 0;
        /**
         * @brief 装载段细分计时（微秒）：符号投影装载与解析索引构建 /
         *        hints 行装载与槽构建（含 oracle 收敛）/ call_sites 行装载
         *        与站点构建（含文件路径解析）/ 槽索引与别名辅助表构建 /
         *        增量 warm 行装载 / 类型串 intern 与回填。
         * @internal 装载段逐件归因的细粒度观测面（与 load_us 同口径细分，
         *           六段之和 == load_us）：六段分别对应装载列裁剪（sites 行
         *           流与文件路径解析）、槽键整数化（槽构建与索引段）与类型
         *           集 arena 化（intern 段）各性能件的收益落点，写入 meta
         *           （analysis_prop_load_symbols_us 等，`ccg stats` 读取）。
         */
        std::int64_t load_symbols_us = 0;
        std::int64_t load_hints_us = 0;
        std::int64_t load_sites_us = 0;
        std::int64_t slot_index_us = 0;
        std::int64_t warm_us = 0;
        std::int64_t intern_us = 0;
    };

    /**
     * @brief 站点五桶号（PropagationState::site_buckets 的下标语义）。
     *
     * @internal 与 dispatch::resolve 的早退判定链同序：indirect →
     *           not_virtual → qualified → no_method → virtual。
     */
    inline constexpr std::size_t SITE_BUCKET_INDIRECT = 0;
    inline constexpr std::size_t SITE_BUCKET_NOT_VIRTUAL = 1;
    inline constexpr std::size_t SITE_BUCKET_QUALIFIED = 2;
    inline constexpr std::size_t SITE_BUCKET_NO_METHOD = 3;
    inline constexpr std::size_t SITE_BUCKET_VIRTUAL = 4;

    /**
     * @brief 站点的五桶分类（本函数是判据链的单一事实源）。
     *
     * 装载物化（propagate 终态填充 site_buckets）与 dispatch 的现场分桶
     * 兜底共用本判据——既往两处三元链逐字镜像仅靠注释互锚，漂移即两路
     * 分桶不一致（桶序与等价性论证见 PropagationState::site_buckets）。
     */
    [[nodiscard]]
    inline auto classify_site(const PropagatedSite& s) -> std::size_t
    {
        return s.indirect                 ? SITE_BUCKET_INDIRECT
               : !s.is_virtual            ? SITE_BUCKET_NOT_VIRTUAL
               : s.qualified              ? SITE_BUCKET_QUALIFIED
               : !s.method_id.has_value() ? SITE_BUCKET_NO_METHOD
                                          : SITE_BUCKET_VIRTUAL;
    }

    /**
     * @brief analysis_slots 行版本戳（导出形：写回侧与装载侧共用）。
     *
     * @internal 槽表语义/形状变更时递增；装载时任一行失配即整体降级
     *           全量（ver 失配整体重算兜底）。ver=2：写回行
     *           （OutParamFlow/FieldFlow）的 related_var 从槽级单值改为
     *           行级多源并集边——旧库槽终态缺失这些并集值，必须整体重算。
     *           ver=3：写回边的直连失败回退引入虚族并集（overrides 族内
     *           同位置形参槽的类型并集回传，reason=out_param_family）——
     *           旧库槽终态缺失族并集值，必须整体重算。ver=4：实参→形参
     *           流的直连失败回退引入虚族扇投（被调解析到无体声明时实参
     *           类型沿 overrides 族并入每个有体 override 的同位置形参
     *           槽）——旧库槽终态缺失扇投值，必须整体重算。ver=5：族边
     *           静态面裁剪（family_face_of）把模板壳主模板键从
     *           "整体不裁"改为"跳过、由壳 pointee 实参键立界"——旧库槽
     *           终态缺失新裁剪效果（壳槽 may 并集更宽），必须整体重算。
     *           ver=6：后代闭包折算 specializes 特化边（CRTP 模板基的真
     *           实现进 allowed 面）+ 族边裁剪全批面外对称守卫（整体不
     *           裁）——旧库槽终态缺失折算与守卫效果，必须整体重算。
     *           ver=7：写回族并集的成员门槛从"带写回行"放宽为"有体成员
     *           槽"（转发出参形态的实现贡献槽终态）；形参补发 Declared
     *           静态面行（形参槽立族边裁剪的证明界）——旧库槽终态缺失
     *           放宽后的族并集值与形参静态面，必须整体重算。
     *           ver=8：写穿性三闸（壳拷贝/move 构造的 pointee 别名边 /
     *           工具写回与出参壳取址写回的补采行 / iid 锁的接口级类型行
     *           与 face 声明面）——旧库槽终态缺失别名边、补采行与接口
     *           键值，必须整体重算。iid 行的派生闭包展开在真库实测触
     *           发未定位崩溃后整体熔断（见行流段的熔断注释）。
     */
    inline constexpr int analysis_slots_ver = 8;

    /**
     * @brief 增量计划：调用方装配的脏集与旧边见证 + 运行后的判定。
     *
     * @internal 调用方（dispatch_commit）填输入侧；run() 填 degraded/closure
     *           并把装载的 warm 行留在 warm 成员里供槽表 diff 写回。任何
     *           资格检查失败（ver 失配、warm 缺行、闭包超预算）都只把
     *           degraded 置位——run() 随即按与全量完全相同的路径执行，
     *           语义零分叉（fold_dups → force_all 同范式）。
     */
    struct IncrementalPlan
    {
        /** @brief 调用方是否尝试增量（false = 直接全量，run 不读其余字段）。 */
        bool active = false;
        /** @brief 脏文件集（阶段 A 重写/新增/失败/缺失/抹除的全部 fid）。 */
        const std::unordered_set<cg::db::FileId>* dirty_files = nullptr;
        /** @brief 脏文件被删站点的旧被调用者键（capture_analysis_purge）。 */
        const std::vector<std::string>* prev_callee_keys = nullptr;
        /** @brief 脏文件被删线索行涉及的旧槽键 (owner_key, var_name)。 */
        const std::vector<std::pair<std::string, std::string>>* prev_slots =
            nullptr;
        /** @brief 脏文件已删除符号的 unique_key（外部扇入站点的旧指向）。 */
        const std::vector<std::string>* doomed_keys = nullptr;
        /** @brief 脏文件已删除符号的非限定名（`fn` 消费者的保守匹配面）。 */
        const std::vector<std::string>* doomed_names = nullptr;
        /** @brief extends/overrides 边集相对上次提交是否变化（摘要判定）。 */
        bool hierarchy_changed = false;
        /** @brief 脏闭包预算（槽+站点数）：超限降级全量。 */
        std::size_t closure_budget = 16'000;
        // ---- 输出侧（run() 填）----
        bool degraded = false;
        /** @brief 降级原因（可观测性；空 = 未降级）。 */
        std::string degrade_reason;
        std::size_t closure = 0;
        /**
         * @brief 预装载的 warm 行（跨提交驻留命中时由调用方提供）。
         * @internal 与全表 all_analysis_slots 读出的行集恒等的前提是调用
         *           方的驻留协议（analysis_slots 的写入方唯一是提交段，
         *           驻留在每次写回后按本批终态同步；持有权随 update_claim
         *           租约——claim 快照的持有者变化即弃）。nullptr = 无驻留，
         *           自行全表读（既有路径）。
         */
        const std::unordered_map<std::string, cg::db::AnalysisSlotRow>*
            warm_preloaded = nullptr;
        /**
         * @brief 本批 warm 的实际来源视图（run 填）：驻留命中时指向
         *        warm_preloaded，否则指向内部 warm 成员。
         * @internal 写回 diff 与增量删除消费它（代替直接读 warm 成员——
         *           两条装载路径下都指向本批权威行集）。
         */
        const std::unordered_map<std::string, cg::db::AnalysisSlotRow>*
            warm_loaded = nullptr;
        /**
         * @brief 本批 warm 是否来自快照恢复（run 填；写侧消费）。
         * @internal 快照形态的行带 type_seg 区段引用且 type_set 未物化，
         *           驻留协议以 SQL 行形态为唯一语汇——写侧据此禁用驻留
         *           接管（否则跨批 type_seg 引用悬空），跨批由快照路径
         *           自持。
         */
        bool warm_from_snap = false;
        /**
         * @brief 装载成功的 warm 行（键 = owner 0x1F 分隔的 var）；写回 diff
         * 的基准。
         * @internal 仅无驻留路径填充；有驻留时本成员保持空，消费经
         *           warm_loaded。
         */
        std::unordered_map<std::string, cg::db::AnalysisSlotRow> warm;
        // ---- 快照恢复（读侧）与快照落盘（写侧）：均由 dispatch_commit
        // 装配；失败/未装配不影响增量资格——快照是装载优化，资格判定
        // （ver/warm-empty/闭包预算）与其正交。----
        /**
         * @brief 快照恢复请求（nullptr = 不尝试；路径与库内 snapshot_seq
         *        当前值由调用方装配）。
         * @internal hierarchy_changed 时调用方不装（extends 边变化使
         *           out_param_this 槽的装载期派生展开过时，必须全量装载）。
         */
        const snap::RestoreRequest* snap_request = nullptr;
        /**
         * @brief 快照装配目标（nullptr = 本批不落盘；run 在装载段收尾与
         *        finalize 各拍一次平面态，序列化与写盘由调用方在事务提交
         *        成功后完成）。
         */
        snap::SnapshotData* snap_dump = nullptr;
        /** @brief 快照恢复观测（run 填；miss 时 hit 为假、原因入串）。 */
        bool         snap_hit = false;
        std::string  snap_miss_reason;
        std::int64_t snap_restore_us = 0;
        /** @brief 指纹比对出的脏文件数（快照写后库行变化过的文件）。 */
        std::size_t snap_dirty_files = 0;
        /** @brief 快照恢复后剔除重建的槽数（半脏/符号消失槽）。 */
        std::size_t snap_rebuilt_slots = 0;
    };

    namespace propagate_detail
    {

        /** @brief 数据库符号之上的分层解析（1.0/0.7/0.4 三档与 graph.build
         * 索引期解析同判据，0.7 档的 ns 尾匹配共用
         * cg::analysis::ns_tail_match 单一实现）。 */
        class SymbolLookup
        {
        public:
            explicit SymbolLookup(const std::vector<cg::db::IndexRow>& rows)
            {
                by_id_.reserve(rows.size());
                by_key_.reserve(rows.size());
                for (const cg::db::IndexRow& row : rows)
                {
                    by_id_.emplace(row.id, &row);
                    by_key_.emplace(row.unique_key, row.id);
                    by_name_[row.name].push_back(&row);
                }
            }

            [[nodiscard]]
            auto key_of(std::int64_t id) const -> std::string
            {
                const auto it = by_id_.find(id);
                return it == by_id_.end() ? std::string{}
                                          : it->second->unique_key;
            }

            /** @brief 符号所在文件（增量脏集判定；未知 id 返回 nullopt）。 */
            [[nodiscard]]
            auto file_of(std::int64_t id) const -> std::optional<cg::db::FileId>
            {
                const auto it = by_id_.find(id);
                if (it == by_id_.end())
                {
                    return std::nullopt;
                }
                return it->second->file_id;
            }

            [[nodiscard]]
            auto owner_key_of(std::int64_t id) const -> std::string
            {
                const auto it = by_id_.find(id);
                if (it == by_id_.end() || !it->second->parent_id.has_value())
                {
                    return {};
                }
                return key_of(*it->second->parent_id);
            }

            [[nodiscard]]
            auto by_key(std::string_view key) const
                -> std::optional<std::int64_t>
            {
                const auto it = by_key_.find(key);
                if (it == by_key_.end())
                {
                    return std::nullopt;
                }
                return it->second;
            }

            /**
             * @brief 符号是否带采集期 is_template 位（未知 id 恒 false）。
             *
             * @internal 出参壳（AcmeOutPtr 等）的声明静态类型是模板记录本
             *           身——模板记录不是出参位的语义类型面（语义面是模板
             *           实参），消费方据此放弃模板静态面的裁剪证明。
             */
            [[nodiscard]]
            bool is_template_symbol(std::int64_t id) const
            {
                const auto it = by_id_.find(id);
                return it != by_id_.end()
                       && (it->second->flags & cg::db::sym_flags::is_template)
                              != 0;
            }

            /**
             * @brief 符号是否带采集期 is_pure_virtual 位（未知 id 恒
             *        false）。
             *
             * @internal 纯虚声明的形参数据行照常发射（静态披露面），但
             *           声明无函数体——实参→形参流投进声明形参槽是传播
             *           死胡同（体内站点才消费形参槽，声明无体内站点）。
             *           直连目标与虚族扇投目标都必须把纯虚声明槽排除，
             *           虚调用解析到纯虚声明时直连判定视同失败、改走族
             *           扇投。
             */
            [[nodiscard]]
            bool is_pure_virtual_symbol(std::int64_t id) const
            {
                const auto it = by_id_.find(id);
                return it != by_id_.end()
                       && (it->second->flags
                           & cg::db::sym_flags::is_pure_virtual)
                              != 0;
            }

            /**
             * @brief 三层级联：名字+父+命名空间+签名（1.0）、名字+父+命名空间
             *        尾（0.7）、项目内裸名唯一/唯一主模板（0.4）。
             */
            [[nodiscard]]
            auto resolve(const MethodRef& meta, Confidence& conf) const
                -> std::optional<std::int64_t>
            {
                const auto it = by_name_.find(meta.name);
                if (it == by_name_.end())
                {
                    return std::nullopt;
                }
                const auto function_like = [](cg::db::SymbolKind k)
                {
                    return k == cg::db::SymbolKind::Function
                           || k == cg::db::SymbolKind::Method
                           || k == cg::db::SymbolKind::Constructor
                           || k == cg::db::SymbolKind::Destructor;
                };
                for (const cg::db::IndexRow* e : it->second)
                {
                    if (!function_like(e->kind) || e->parent != meta.parent
                        || e->ns != meta.ns || e->sig_hash != meta.sig_hash
                        || (e->flags & cg::db::sym_flags::is_const) != 0
                               != meta.const_method)
                    {
                        continue;
                    }
                    conf = Confidence::High;
                    return e->id;
                }
                for (const cg::db::IndexRow* e : it->second)
                {
                    if (!function_like(e->kind) || e->parent != meta.parent
                        || !ns_tail_match(e->ns, meta.ns))
                    {
                        continue;
                    }
                    conf = Confidence::Medium;
                    return e->id;
                }
                std::optional<std::int64_t> only;
                std::size_t                 hits = 0;
                for (const cg::db::IndexRow* e : it->second)
                {
                    if (!function_like(e->kind))
                    {
                        continue;
                    }
                    ++hits;
                    if (hits == 1)
                    {
                        only = e->id;
                    }
                    if (hits > 8)
                    {
                        break;
                    }
                }
                if (hits == 1)
                {
                    conf = Confidence::Low;
                    return only;
                }
                return std::nullopt;
            }

        private:
            std::unordered_map<std::int64_t, const cg::db::IndexRow*> by_id_;
            // 透明哈希：by_key(string_view) 的热路径直查免临时键拷贝。
            std::unordered_map<
                std::string,
                std::int64_t,
                cg::util::string_hash,
                std::equal_to<>>
                by_key_;
            std::
                unordered_map<std::string, std::vector<const cg::db::IndexRow*>>
                    by_name_;
        };

        /**
         * @brief 槽类型集 arena：单一连续缓冲，每槽一个 (offset,len) 区段。
         *
         * @internal 写路径是尾追加 + 保序归并（见 run() 内 add_types）：新集落
         *           尾部新区段，旧区段成死区，由分代压缩回收（活元素 < 半仓时
         *           按槽下标序紧凑重排——区段内容不变仅位置变，一切语义只读
         *           区段内容，压缩等价）。区段只以 (off,len) 下标寻址：追加
         *           扩容（realloc）后裸指针失效而下标恒有效。
         *
         *           区段级驻留（相同集合共享同一段）：append_dedup 先查驻留
         *           表，arena 内已有内容相同的区段时直接复用其 (off,len)，
         *           不追加。非空类型集重复度高（槽终态大量同集——单元素集
         *           占比高的形态下 10 倍量级），驻留把"每槽一份区段"收敛为
         *           "每 distinct 集一份"，arena 与分代压缩后的常驻体积按重复
         *           度等比缩小；add_types 的并集结果命中已有区段时旧区段立即
         *           失效成死区、零追加。驻留表键是区段内容的哈希，条目存
         *           (off,len)——内容访问经 buf 实时进行（buf 扩容只换底层指
         *           针，off/len 键不失效）；哈希碰撞链由内容相等比较收口，
         *           相同 (off,len) 不会重复登记（每段只 emplace 一次）。一切
         *           语义只读区段内容，共享仅使多个 (off,len) 指向同一处，
         *           行为等价。
         */
        struct TypeArena
        {
            std::vector<std::int32_t> buf;
            /**
             * @brief 区段驻留表：内容哈希 -> (off,len) 链。
             * @internal 全量重建点与分代压缩同步（压缩重建后旧行全部作废）。
             */
            std::unordered_map<
                std::uint64_t,
                std::vector<std::pair<std::uint32_t, std::uint32_t>>>
                dedup;

            /** @brief 区段内容的折叠哈希（splitmix64 逐元素终混 + 长度收口）。
             */
            [[nodiscard]]
            static auto span_hash(std::span<const std::int32_t> ids)
                -> std::uint64_t
            {
                std::uint64_t z =
                    0x9e3779b97f4a7c15ULL
                    ^ (static_cast<std::uint64_t>(ids.size()) << 32);
                for (const std::int32_t t : ids)
                {
                    z ^= static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(t));
                    z *= 0xff51afd7ed558ccdULL;
                    z ^= z >> 33;
                }
                return z;
            }

            /** @brief 尾追加一段 id（返回新段 offset；不查驻留表）。 */
            [[nodiscard]]
            auto append(std::span<const std::int32_t> ids) -> std::uint32_t
            {
                const auto off = static_cast<std::uint32_t>(buf.size());
                buf.insert(buf.end(), ids.begin(), ids.end());
                return off;
            }

            /**
             * @brief 带驻留的追加：arena 已有内容相同的区段时复用，否则追加
             *        并登记。
             * @return (off, len)。空区间恒 (0,0)（空集视图与任何 off 等价）。
             */
            [[nodiscard]]
            auto append_dedup(std::span<const std::int32_t> ids)
                -> std::pair<std::uint32_t, std::uint32_t>
            {
                if (ids.empty())
                {
                    return {0, 0};
                }
                const std::uint64_t h = span_hash(ids);
                if (const auto it = dedup.find(h); it != dedup.end())
                {
                    for (const auto& [off, len] : it->second)
                    {
                        if (len == ids.size()
                            && std::equal(
                                buf.begin() + off,
                                buf.begin() + off + len,
                                ids.begin(),
                                ids.end()))
                        {
                            return {off, len};
                        }
                    }
                }
                const auto off = append(ids);
                dedup[h].emplace_back(
                    off,
                    static_cast<std::uint32_t>(ids.size()));
                return {off, static_cast<std::uint32_t>(ids.size())};
            }

            /** @brief 区段视图（只读；buf 任何变更后需重取）。 */
            [[nodiscard]]
            auto view(std::uint32_t off, std::uint32_t len) const
                -> std::span<const std::int32_t>
            {
                return std::span<const std::int32_t>{
                    buf.data() + off,
                    static_cast<std::size_t>(len)};
            }
        };

        /**
         * @brief 本进程的批量位运算档位（首次调用探测一次，此后固定）。
         *
         * @internal 探测在平台守卫模块 cg.util.platform（CPUID 与 XCR0 双重
         *           确认，非 x86 恒标量）；此处用函数内静态量把探测收敛成
         *           进程内一次。档位选择与输入数据无关，函数指针在
         *           bits_or_words 里同理一次定型——热路径每次只做一次指针
         *           读取，探测成本不进循环。
         */
        [[nodiscard]]
        inline BitsTier bits_active_tier()
        {
            static const BitsTier tier = []
            {
                const cg::util::platform::CpuBitOpsCaps caps =
                    cg::util::platform::cpu_bitops_caps();
                if (caps.avx512)
                {
                    return BitsTier::Avx512;
                }
                if (caps.avx2)
                {
                    return BitsTier::Avx2;
                }
                return BitsTier::Scalar;
            }();
            return tier;
        }

        /**
         * @brief 按位或原语在本进程的固定函数指针（标量/AVX2/AVX-512 三档
         *        之一，探测一次后不变）。
         *
         * @internal 三档语义逐位相同（等价测试逐字比对），档位差异对任何
         *           消费方不可观测；向量档翻译单元整体以本档指令集编译，
         *           详见 symbits.kernels.h 的分 TU 说明。
         */
        [[nodiscard]]
        inline OrWordsFn bits_or_words()
        {
            static const OrWordsFn fn = []
            {
                switch (bits_active_tier())
                {
                case BitsTier::Avx512:
                    return &or_words_avx512;
                case BitsTier::Avx2:
                    return &or_words_avx2;
                case BitsTier::Scalar:
                    break;
                }
                return &or_words_scalar;
            }();
            return fn;
        }

        /**
         * @brief SymBits 位图存储的分配器：64 字节对齐的 word 数组。
         *
         * @internal 向量档内核的主循环以对齐版指令（vmovdqa/vmovdqa64 一族）
         *           整块读写 SymBits 存储，基址必须真对齐：64 字节覆盖
         *           AVX-512 的 512 位整块（AVX2 的 256 位由 64 整除自动
         *           满足），非对齐地址走对齐版指令是硬件异常。实现走
         *           C++17 带对齐的 operator new/delete（std::align_val_t
         *           重载，与未对齐版本严格配对），MSVC 与 clang64 libc++
         *           双工具链同源码可用。allocate 内的对齐断言是存储契约
         *           的守卫：将来改动存储/分配路径破坏对齐时调试构建
         *           第一时间失败，而不是等向量内核在运行期崩。
         */
        template <typename T>
        struct AlignedWordAllocator
        {
            using value_type = T;

            constexpr AlignedWordAllocator() noexcept = default;
            template <typename U>
            constexpr AlignedWordAllocator(
                const AlignedWordAllocator<U>&) noexcept
            {
            }

            [[nodiscard]]
            T* allocate(std::size_t n)
            {
                if (n > static_cast<std::size_t>(-1) / sizeof(T))
                {
                    throw std::bad_array_new_length{};
                }
                T* p = static_cast<T*>(
                    ::operator new(n * sizeof(T), std::align_val_t{64}));
                assert(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
                return p;
            }

            void deallocate(T* p, std::size_t) noexcept
            {
                ::operator delete(p, std::align_val_t{64});
            }

            template <typename U>
            bool operator==(const AlignedWordAllocator<U>&) const noexcept
            {
                return true;
            }
        };

        /** @brief SymBits 的存储形态：64 字节对齐分配的 word 数组（对齐
         *         契约与断言守卫见 AlignedWordAllocator）。 */
        using AlignedWords =
            std::vector<std::uint64_t, AlignedWordAllocator<std::uint64_t>>;

        /**
         * @brief 符号 id 域上的稠密位图（word 数组，整数位运算）。
         *
         * @internal 位域是符号 id（全库唯一、上界有限：按参考语料规模 76,285
         * 符号 约 9.5KB/图，千级图合计 ~10MB 量级）。set 惰性增长；test 在
         *           已置位域之外恒 false；each_set 按 id 升序回调（word 内低位
         *           扫描天然升序，消费面的物化顺序因此确定）。批量并集
         *           （or_with）经 bits_or_words 的三档分派执行：标量基线与
         *           AVX2/AVX-512 向量档逐位同语义，档位对结果零影响。
         */
        struct SymBits
        {
            /** @brief 位图存储（64 字节对齐分配，对齐版向量内核的整块
             *         读写前提；契约见 AlignedWordAllocator）。 */
            AlignedWords words;

            /** @brief 置位（惰性增长到位所在 word）。 */
            void set(std::int64_t id)
            {
                const std::size_t w = static_cast<std::size_t>(id) / 64;
                if (words.size() <= w)
                {
                    words.resize(w + 1, 0);
                }
                words[w] |= 1ULL << (static_cast<unsigned>(id) % 64);
            }

            /** @brief 成员资格测试（未增长到的 word 恒 false）。 */
            [[nodiscard]]
            bool test(std::int64_t id) const
            {
                const std::size_t w = static_cast<std::size_t>(id) / 64;
                return w < words.size()
                       && (words[w]
                           & (1ULL << (static_cast<unsigned>(id) % 64)))
                              != 0;
            }

            /**
             * @brief 就地按位或（增长到对方的域宽）。
             *
             * @internal word 级循环经三档分派的原语执行；本机不支持向量档时
             *           分派即标量，行为与逐 word 循环逐位一致。
             */
            void or_with(const SymBits& other)
            {
                if (words.size() < other.words.size())
                {
                    words.resize(other.words.size(), 0);
                }
                if (other.words.empty())
                {
                    return;
                }
                bits_or_words()(
                    words.data(),
                    other.words.data(),
                    other.words.size());
            }

            /**
             * @brief 依 id 升序对每个置位 id 调用 fn（word 内逐低位清位扫描）。
             */
            template <typename F>
            void each_set(F&& fn) const
            {
                for (std::size_t i = 0; i < words.size(); ++i)
                {
                    std::uint64_t w = words[i];
                    while (w != 0)
                    {
                        const int b = std::countr_zero(w);
                        fn(static_cast<std::int64_t>(
                            i * 64 + static_cast<std::size_t>(b)));
                        w &= w - 1;
                    }
                }
            }
        };

        struct HintSlot
        {
            std::string owner_key;
            std::string var_name;
            /**
             * @brief 槽键整数分量：owner 规范 id 与 var intern id。
             * @internal 随槽首见与字符串字段同步填充（见 SlotKey 的等价性
             *           论证）；仅供内存索引（槽索引/形参表/增量种子）使用，
             *           导出面消费字符串字段不变。
             */
            std::int64_t owner_id = 0;
            std::int32_t var_id = 0;
            /**
             * @brief 槽类型集的区段表示：类型集 arena 的 (offset,len)。
             * @internal 槽不再各自持有堆块（槽各一个 vector 的分配/释放常数
             *           与写入时整体 sort 的常数一并消除）。区段内容是有序去重
             *           的 intern id（按类型串字典序赋号，整数比较与字符串比较
             *           同序——排序、去重与 MAX_TYPES 截断保留的子集和字符串
             *           形态逐字节一致；id 0 保留给空串，批内空串参与计数与截
             *           断前缀，写入侧跳过，与字符串形态的 `if (t.empty())
             *           continue` 对齐）。装载期先在字符串暂存上跑 oracle，最终
             *           统一 intern 落 arena（见 run() 内类型表与 add_types）。
             */
            std::uint32_t            type_off = 0;
            std::uint32_t            type_len = 0;
            std::vector<std::string> static_types;
            Confidence               confidence = Confidence::Fallback;
            HintSource               source = HintSource::Declared;
            std::string              reason;
            std::string              related_var;
            std::string              related_key;
            int                      param_index = -1;
            /**
             * @brief 槽级类型集截断观测（MAX_TYPES）。
             * @internal 三要素——定义：本槽类型集写入时被 MAX_TYPES 截断
             *           （函数内别名拷贝与实参→形参传播两处 resize 点置位），
             *           或经拷贝路径从已标记源槽传染（函数内别名、实参→
             *           形参传播的 caller 槽与 arg_call_keys 解析的返回槽、
             *           返回值→调用点槽）；
             *           soundness 边界：只置位不复位，标记是过近似——被截断
             *           丢弃的类型可能已由其它路径补齐时仍标记（宁可多报不
             *           完整、不漏报），collect
             * 直收的实参类型（site.arg_types） 无槽载体，只能靠实参→形参
             * 传播的截断点本身覆盖、无法经槽传染； fallback 清单：独立
             * bool，绝不写进 reason（final 阶段 会被 multiple_reason
             * 整串覆盖），经 PropagatedHint 与
             * PropagatedSite::slot_type_truncated 透出，由 dispatch 主路径追加
             * ";slot_type_truncated" 后缀（非空才加分号）。
             */
            bool truncated_here = false;
            /**
             * @brief 本槽存在 OutParamFlow 行（取址实参回填的行存在性门槛）。
             * @internal 定义：装载期任一 source=OutParamFlow 的行命中本槽即置
             *           位（与槽置信度/来源标签的到达顺序无关）；消费方：全局传播的
             *           出参回填把它当唯一库内/外门槛（库外实现的函数无行，
             *           天然不回填）。
             */
            bool has_outparam_flow = false;
        };

        [[nodiscard]]
        inline auto hint_slot_key(std::string_view owner, std::string_view var)
            -> std::string
        {
            return cg::util::compose_key(owner, var);
        }

        /**
         * @brief 槽键的整数形态：owner 规范 id × var 域 intern id。
         *
         * @internal 与字符串键形态（hint_slot_key 拼接）的等价性：owner 分量取
         *           unique_key 的规范 id（同键多符号按首见收敛），槽装载侧用
         *           行内 owner_id 直接规范化、查询侧按键串解析——两侧落到同一
         *           规范 id 当且仅当字符串形态两侧相等；var 分量在槽装载期
         *           intern（外加 "#ret" 预热与形参 "#p{n}" 注册），槽域封闭后
         *           查询 miss 即无槽，与字符串 find miss 同义。热路径（装载去
         *           重 / 槽索引 / 形参查找 / 别名解析）的键构造与比较全部整数
         *           化，消掉每次的字符串拼接分配与第二次字符串哈希；导出面
         *           （owner_key/var_name 字符串字段、analysis_slots 持久化键、
         *           增量 warm 行键）保持字符串形态不变。
         */
        struct SlotKey
        {
            std::int64_t owner = 0;
            std::int32_t var = 0;
            bool         operator==(const SlotKey& other) const = default;
        };

        /** @brief SlotKey 的整数混合哈希（splitmix64 终混，位分布均匀）。 */
        struct SlotKeyHash
        {
            [[nodiscard]]
            std::size_t operator()(const SlotKey& k) const noexcept
            {
                std::uint64_t z = (static_cast<std::uint64_t>(k.owner) << 32)
                                  ^ (static_cast<std::uint64_t>(
                                         static_cast<std::uint32_t>(k.var))
                                     | 0x9e3779b97f4a7c15ULL);
                z ^= z >> 33;
                z *= 0xff51afd7ed558ccdULL;
                z ^= z >> 33;
                return static_cast<std::size_t>(z);
            }
        };

        /** @brief string_view 透明哈希/相等（unordered heterogeneous 查找）。
         */
        struct SvHash
        {
            using is_transparent = void;
            [[nodiscard]]
            std::size_t operator()(std::string_view s) const noexcept
            {
                return std::hash<std::string_view>{}(s);
            }
        };
        struct SvEq
        {
            using is_transparent = void;
            [[nodiscard]]
            bool operator()(std::string_view a, std::string_view b)
                const noexcept
            {
                return a == b;
            }
        };
        using VarDomain =
            std::unordered_map<std::string, std::int32_t, SvHash, SvEq>;

        /**
         * @brief intern 类型表的空串 id（恒为 0：字典序赋号时空串最小）。
         *
         * @internal 批内的空串参与计数与截断前缀（与字符串形态一致），但写入
         * 侧跳过——见 run() 内 add_types 与类型表构建。
         */
        inline constexpr std::int32_t EMPTY_TYPE_ID = 0;

        /** @brief 槽行版本戳（定义见 cg::analysis::analysis_slots_ver）。 */
        inline constexpr int SLOT_VER = analysis_slots_ver;

        /**
         * @brief 多类型槽的 fallback 原因：按类型集来源区分，不一律写成
         *        param_flow_multiple。
         */
        [[nodiscard]]
        inline auto multiple_reason(
            HintSource         source,
            const std::string& original) -> std::string
        {
            switch (source)
            {
            case HintSource::ParamFlow:
                // 多类型实参流吞掉槽 reason 里的族扇投后缀时补回——
                // 类型构成里有扇投来源（;param_flow_family）是披露面信息，
                // 与多类型措辞并存（出参族词根经下方 default 分支天然保
                // 留，此处对齐）。
                return original.find("param_flow_family") != std::string::npos
                           ? "param_flow_multiple;param_flow_family"
                           : "param_flow_multiple";
            case HintSource::FieldFlow:
                return "field_flow_multiple";
            case HintSource::ReturnFlow:
                return "return_flow_multiple";
            case HintSource::LocalCopy:
                return "local_copy_multiple";
            case HintSource::DirectAggregate:
                // 条件表达式合流是 DirectAggregate 的多类型来源。
                return "conditional_merge";
            default:
                break;
            }
            if (original.empty())
            {
                return "multiple_dynamic_types";
            }
            return std::format("{}_multiple", original);
        }

        /**
         * @brief 实参预解析的“无槽”哨兵值（ArgFlow 三个落点字段用）。
         */
        constexpr std::size_t NPOS = static_cast<std::size_t>(-1);

        /**
         * @brief 站点单个实参的静态预解析结果（preresolve_edges 填充）。
         *
         * @internal 槽/站点集合在传播期不变，实参的三个落点（形参槽/实参
         *           变量槽/项目内调用的返回槽）各解析一次（全扫版每轮重复
         *           解析的成本消解）。
         */
        struct ArgFlow
        {
            std::uint32_t arg = 0;
            std::size_t   target = NPOS;   // 形参槽（无则 NPOS）
            std::size_t   arg_slot = NPOS; // 实参变量槽
            std::size_t   ret_slot = NPOS; // 实参 = 项目内调用时的返回槽
            /**
             * @brief 虚族扇投的族内目标槽（直连槽不存在时沿 overrides
             *        边取族内每个有体 override 的同位置形参槽）。
             * @internal 直连命中的站点不填（接收者已可唯一时并集只稀释
             *           决议唯一性）；空 = 不扇投（非虚调用/全库无该形参
             *           位/族内无有体成员）。批源（arg_slot/ret_slot/直收
             *           批）与直连共享，投递语义见不动点 kA 段。
             */
            std::vector<std::uint32_t> family_targets;
        };

        /**
         * @brief 站点的一条出参写回边（被调形参槽 -> 调用方实参槽）。
         *
         * @internal family=true 为虚族并集边：直连槽无写回行时沿 overrides
         *           族取同位置槽的回退（回传档位 Low、reason
         *           =out_param_family，与直连边的 Medium/out_param_flow
         *           在披露层可区分）。
         */
        struct WbEdge
        {
            std::uint32_t param = 0;
            std::uint32_t dst = 0;
            bool          family = false;
        };

        /**
         * @brief run() 的共享上下文：装载/闭包/不动点/终态各段的全部中间态。
         *
         * @internal 巨型函数拆分的函数群载体——run() 只保留段序编排与分段计
         *           时聚合，各段按既有段落注释边界提为成员函数（成员名与拆
         *           分前的局部变量同名，段体逐字搬迁）。段序与计时打点位置
         *           和拆分前一致：六个装载子段之和恒等于 load_us。
         */
        struct RunCtx
        {
            // ---- 输入与结果 ----
            /** @brief 当前连接（同一事务内可见本批已写入的行）。 */
            cg::db::GraphStore& store;
            /** @brief 类型 oracle 接缝。 */
            const TypeOracle& oracle;
            /** @brief 预载符号投影（owned_rows 归 run 持有，此处只借针）。 */
            const std::vector<cg::db::IndexRow>* shared_rows;
            /** @brief 增量计划（nullptr/active=false = 全量路径）。 */
            IncrementalPlan* plan;
            /** @brief 符号域分层解析索引。 */
            const SymbolLookup& lookup;
            /** @brief 结果累积器（run 终态移出返回）。 */
            PropagationState out;

            // ---- 装载行集与增量输入 ----
            /** @brief hints 行（快照路径=脏文件重读集，否则全表）。 */
            std::vector<cg::db::TypeHintRow> hint_rows;
            /** @brief call_sites 行（列裁剪投影；终态移入 out.source_sites）。
             */
            std::vector<cg::db::CallSiteRow> site_rows;
            /** @brief 增量脏文件集（非增量批为空）。 */
            std::unordered_set<cg::db::FileId> dirty_fids;
            /** @brief 本批是否增量候选（plan 非空且 active）。 */
            bool incr_candidate = false;
            /** @brief 槽有线索行位于脏文件（与 slots 下标对齐）。 */
            std::vector<char> slot_row_dirty;
            /** @brief 槽带 out_param_this 派生行（与 slots 下标对齐）。 */
            std::vector<char> slot_derived_seed;

            // ---- QI this 派生闭包与符号规范域 ----
            /** @brief extends 下行邻接（首个派生行触发惰性装载）。 */
            std::optional<
                std::unordered_map<std::int64_t, std::vector<std::int64_t>>>
                extends_down;
            /** @brief 根 -> 后代闭包位图的缓存。 */
            std::unordered_map<std::int64_t, SymBits> derived_bits_of;
            /**
             * @brief overrides 边双向邻接（键值均已规范化到符号规范 id）。
             *
             * @internal 首个虚族查询触发惰性装载（语料无虚装填站点时零成
             *           本）；装载时把边端符号 id 换算成规范 id，与 param_of
             *           的槽键域一致（同键多符号的边端直查会假 miss 漏成
             *           员）。边端键悬空（符号已删）跳过——宁漏勿错。
             */
            std::optional<std::pair<
                std::unordered_map<std::int64_t, std::vector<std::int64_t>>,
                std::unordered_map<std::int64_t, std::vector<std::int64_t>>>>
                overrides_adj;
            /** @brief 方法规范 id -> 覆盖族成员规范 id（含自身；记忆化）。 */
            std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                virtual_family_of;
            /**
             * @brief (方法规范 id × 形参域 id) -> 族内同位置形参槽下标。
             *
             * @internal 只收录有体成员的同位置槽（槽存在即有体——形参数
             *           据行只在函数体采集时发射）；成员自己的写回行不再是
             *           门槛：转发出参形态（体内把指针转发给工厂函数、无
             *           `*pp=` 直写行）的实现类同样要向族并集贡献其槽终态
             *           （转发链的证据已在该槽内，may 并集语义不区分证据
             *           的获取路径）。空集也缓存（miss 与空族的重复查询零
             *           成本）。
             */
            std::unordered_map<SlotKey, std::vector<std::uint32_t>, SlotKeyHash>
                virtual_family_params_of;
            /**
             * @brief (方法规范 id × 形参域 id) -> 族内有体成员的同位置形
             *        参槽下标。
             *
             * @internal 与 virtual_family_params_of 的门槛差异：只要求槽存在
             *           （形参数据行只在函数体采集时发射，槽存在即有
             *           体），不带写回行门槛——入参扇投要的是能把实参证
             *           据投进去的每个有体实现，无论该实现是否写穿出参。
             *           空集也缓存（miss 与空族的重复查询零成本）。
             */
            std::unordered_map<SlotKey, std::vector<std::uint32_t>, SlotKeyHash>
                virtual_family_body_params_of;
            /** @brief 虚族并集写回边计数（观测面，run 终态转 out）。 */
            std::size_t family_wb_edges = 0;
            /** @brief 虚族扇投入参边计数（观测面，run 终态转 out）。 */
            std::size_t family_pf_edges = 0;
            /** @brief 实收族写回槽数（终态物化段累加，run 终态转 out）。 */
            std::size_t family_wb_fed_slots = 0;
            /** @brief 实收扇投槽数（终态物化段累加，run 终态转 out）。 */
            std::size_t family_pf_fed_slots = 0;
            /**
             * @brief 槽的族并集静态裁剪面（与 slots 对齐）。
             *
             * @internal allowed = 槽静态声明类型自身 ∪ 各类型的 extends
             *           后代闭包（含 specializes 特化折算，见
             *           derived_closure_bits）。QI/GetBase 类巨族的 may
             *           并集注入静态类型已可证的具体类出参（如 QI 快速
             *           路径的 T** 出参）会把静态可证的唯一决议稀释成巨
             *           族披露——族边投递前按此面裁剪；接口静态面（后代
             *           = 全部实现类）天然不裁，虚族救活主场景零损失。
             *           裁剪只收紧可证界外（宁漏方向），静态面任一非模
             *           板类型解析失败即整体不裁；模板记录（出参壳的主
             *           模板）不是语义类型面——其语义面是模板实参 T（采
             *           集侧已把壳 pointee 实参键追加进静态面），壳键跳
             *           过、由实参键立界，剥壳后为空的纯壳面整体不裁。
             *           闭包残缺（缺边形态）时真实现会被整批判界外——
             *           面外裁剪配有对称守卫（见 family_clip_batch），
             *           全批面外的槽整体不裁，宁可漏投不饿死真决议。
             */
            struct FamilyClip
            {
                bool    init = false; // 惰性构建标记
                bool    clip = false; // false = 不裁（无静态面/解析失败）
                SymBits allowed{};
            };
            std::vector<FamilyClip> slot_family_clip;
            /** @brief 族边裁剪过滤的复用缓冲（热路径免逐边分配）。 */
            std::vector<std::int32_t> family_clip_scratch;
            /**
             * @brief 槽收到过族并集投递的标记（与 slots 对齐）。
             *
             * @internal 槽标签遵循"最高档来源"语义（add_types 同档/低档不
             *           改写），族边把类型并进被更高档标签占位的槽时来源
             *           不可见——终态物化对被投递槽的 reason 追加
             *           out_param_family 后缀，披露层据此区分族并集救活
             *           面（类型构成事实与标签档位解耦）。
             */
            std::vector<char> slot_family_fed;
            /**
             * @brief 槽收到过入参族扇投的标记（与 slots 对齐）。
             *
             * @internal 与 slot_family_fed（出参族写回）分记：两个方向
             *           的族边都以 Low 档并入，槽标签遵循最高档来源语义
             *           时扇投来源不可见——终态物化对被扇投槽的 reason
             *           追加 param_flow_family 后缀，披露层据此区分入参
             *           扇投救活面。
             */
            std::vector<char> slot_family_pf_fed;
            /** @brief unique_key -> 规范符号 id（键指向 shared_rows 行内串）。
             */
            std::unordered_map<std::string_view, std::int64_t> key_to_canon;
            /** @brief 任意符号 id -> 同键规范 id（同键多符号首见收敛）。 */
            std::unordered_map<std::int64_t, std::int64_t> canon_of_id;

            // ---- 快照恢复（读侧）与 var 域 ----
            /** @brief 快照恢复结果（nullopt = miss/未尝试）。 */
            std::optional<snap::RestoreResult> snap;
            /** @brief 快照 miss 原因（"off" = 未装配请求）。 */
            std::string snap_miss;
            /** @brief var 域 intern（槽装载期封闭；"#ret" 预热）。 */
            VarDomain var_of;
            /** @brief var 域下一 id（快照路径从快照值续号）。 */
            std::int32_t next_var_id = 0;
            /** @brief 返回槽名 "#ret" 的 var 域 id。 */
            std::int32_t ret_var_id = 0;

            // ---- 槽域装载态 ----
            /** @brief 传播槽（owner×var 唯一，行流合并）。 */
            std::vector<HintSlot> slots;
            /** @brief 槽类型集 arena（区段语义与分代压缩见 TypeArena）。 */
            TypeArena arena;
            /** @brief 装载期字符串类型集暂存（oracle 在字符串形态上跑）。 */
            std::vector<std::vector<std::string>> pending_types;
            /**
             * @brief 转型声称缓冲（与 slots 下标对齐）。
             * @internal CastTarget 行的类型串只入缓冲不进 pending——声称绝
             *           不参与传播（不建边、不流动），传播终态槽动态集为空
             *           时才整批填入（finalize_hints），有更强证据的槽不受
             *           影响。快照保留槽的行流跳过对声称行豁免（闭包卷入
             *           复位重算后缓冲须有注入源），行集由装载侧补装。
             */
            std::vector<std::vector<std::string>> slot_cast_claims;
            /** @brief 声称根 -> 物化声称面（派生闭包串集）的装载期缓存。 */
            std::unordered_map<std::string, std::vector<std::string>>
                claim_face_of;
            /** @brief 装载期行级别名引用（槽下标, related_var）。 */
            std::vector<std::pair<std::size_t, std::string>> pending_row_alias;
            /** @brief 槽的源文件全集（与 slots 对齐；半脏判定与快照段用）。 */
            std::vector<std::vector<cg::db::FileId>> slot_file_list;
            /** @brief 槽键 -> 槽下标（装载去重）。 */
            std::unordered_map<SlotKey, std::size_t, SlotKeyHash> slot_of;
            /** @brief 快照保留槽的初始区段（与 slots 下标对齐）。 */
            std::vector<std::pair<std::uint32_t, std::uint32_t>> snap_init_segs;
            /** @brief 快照保留槽标记（行流命中保留槽时跳过，防边重复登记）。 */
            std::vector<char> slot_from_snap;

            // ---- 站点域与槽索引 ----
            /** @brief 传播站点（装载装配 + 终态填充动态类型）。 */
            std::vector<PropagatedSite> sites;
            /** @brief 站点行位于脏文件（与 sites 下标对齐）。 */
            std::vector<char> site_row_dirty;
            /** @brief 行内旧被调用者键（翻转见证，callee_key 无 FK 存活）。
             */
            std::vector<std::string> site_row_callee_key;
            /** @brief 形参槽索引：owner 规范 id × "#p{n}" -> 槽下标。 */
            std::unordered_map<SlotKey, std::size_t, SlotKeyHash> param_of;
            /** @brief 形参序号 -> "#p{n}" 的 var 域 id（与 param_of
             * 同步注册）。 */
            std::unordered_map<int, std::int32_t> param_var_of;
            /** @brief 槽键 -> 槽下标（主索引）。 */
            std::unordered_map<SlotKey, std::size_t, SlotKeyHash> slot_index;
            /** @brief add_types 的归并暂存（arena
             * 外稳定缓冲，跨投递复用容量）。 */
            std::vector<std::int32_t> merge_scratch;

            // ---- warm 装载与类型 intern 域 ----
            /** @brief 上次提交段槽终态行（三来源：驻留/快照/全表读）。 */
            const std::unordered_map<std::string, cg::db::AnalysisSlotRow>*
                warm = nullptr;
            /** @brief warm 是否来自快照恢复（行带 type_seg 区段引用）。 */
            bool warm_from_snap = false;
            /** @brief intern id -> 类型串（字典序赋号，id 序 == 字符串序）。 */
            std::vector<std::string> type_of_id;
            /** @brief 每站点每实参的类型 id 集（仅已解析站点参与传播）。 */
            std::vector<std::vector<std::vector<std::int32_t>>> arg_ids;
            /** @brief 类型串 -> intern id。 */
            std::unordered_map<std::string, std::int32_t> id_of_type;
            /** @brief 快照类型域旧 id -> 恢复域新 id（保序重映射）。 */
            std::vector<std::int32_t> snap_id_remap;

            // ---- 传播边静态预解析 ----
            /** @brief 每站点的实参落点表（target/arg_slot/ret_slot）。 */
            std::vector<std::vector<ArgFlow>> site_args;
            /** @brief 每站点的出参写回边（直连边 + 虚族并集边）。 */
            std::vector<std::vector<WbEdge>> site_wb;
            /** @brief 槽 -> 函数内依赖扇出（别名/行级边的下游槽）。 */
            std::vector<std::vector<std::uint32_t>> l1_deps;
            /** @brief 槽 -> 以它为实参源的站点（实参→形参扇出）。 */
            std::vector<std::vector<std::uint32_t>> a_src_sites;
            /** @brief 形参槽 ->
             * 喂它的站点（出参写回扇出；增量时兼复位再供料）。 */
            std::vector<std::vector<std::uint32_t>> b_param_sites;
            /** @brief 返回槽 -> 消费它的槽（返回值→调用点扇出）。 */
            std::vector<std::vector<std::uint32_t>> c_consumers;
            /** @brief 增量专用：形参槽 -> 喂它的站点（仅候选时填充）。 */
            std::vector<std::vector<std::uint32_t>> a_target_sites;
            /** @brief owner 键 -> 槽下标列表（翻档旧目标定位；仅候选时填充）。
             */
            std::unordered_map<std::string, std::vector<std::uint32_t>>
                slots_of_owner;
            /** @brief 槽 -> 槽级别名源槽（单值，装载序第一条）。 */
            std::vector<std::optional<std::size_t>> l1_src_of;
            /** @brief 槽 -> 返回消费源槽（单值）。 */
            std::vector<std::optional<std::size_t>> c_src_of;
            /** @brief 槽 -> 行级多源边源列表（写回/壳字段写行）。 */
            std::vector<std::vector<std::uint32_t>> row_alias_of;
            /** @brief 槽 -> 行级死边见证 rel（解析失败的 related_var）。 */
            std::vector<std::string> row_alias_dead_rel;

            // ---- 增量闭包与不动点 ----
            /** @brief 槽在脏闭包内（增量重算面；finalize 与掩码消费）。 */
            std::vector<char> slot_in_closure;
            /** @brief 站点 due（闭包内待处理；类 A/类 B 见种子段注释）。 */
            std::vector<char> site_due;
            /** @brief 增量是否成立（闭包构建后定；finalize 与 site_affected
             * 消费）。 */
            bool incr_applied = false;

            // ---- 分段计时打点（run 聚合；六装载段之和恒等于 load_us）----
            std::chrono::steady_clock::time_point t_symbols;
            std::chrono::steady_clock::time_point t_hints;
            std::chrono::steady_clock::time_point t_sites;
            std::chrono::steady_clock::time_point t_slotidx;
            std::chrono::steady_clock::time_point t_warm;
            std::chrono::steady_clock::time_point t_load1;
            std::chrono::steady_clock::time_point t_final0;

            explicit RunCtx(
                cg::db::GraphStore&                  s,
                const TypeOracle&                    o,
                const std::vector<cg::db::IndexRow>* rows,
                IncrementalPlan*                     p,
                const SymbolLookup&                  l)
                : store{s}, oracle{o}, shared_rows{rows}, plan{p}, lookup{l}
            {
            }

            /**
             * @brief 装载站点行与增量输入标记，构建派生闭包机制与符号规范域。
             */
            void init_symbol_domain()
            {
                // 装载走列裁剪投影（sites 去 callee_usr/expr/param 与
                // arg_targets 四列——固定点与写回比对均不消费）。hints
                // 的列裁剪形态按装载路
                // 径二选一：快照恢复成功时只重读脏文件行（含半脏槽的干净半边文件），
                // 其余槽侧状态全部从快照平面段恢复；任何失败回退全表读。
                site_rows = store.load_call_sites_analysis();
                // 增量输入：脏文件集 + 装载期并行标记（与 slots/sites
                // 下标对齐）。
                if (plan != nullptr && plan->active
                    && plan->dirty_files != nullptr)
                {
                    dirty_fids.insert(
                        plan->dirty_files->begin(),
                        plan->dirty_files->end());
                }
                incr_candidate = plan != nullptr && plan->active;
                // 槽键整数化的两个域（等价性见 SlotKey）：key_to_canon 把
                // unique_key 映到规范 id（键是 string_view，指向 shared_rows
                // 行内串——行集在 run 全程稳定），canon_of_id 把任意符号 id
                // 映到同键规范 id（同键
                // 多符号首见收敛，与字符串键形态的合并行为一致）；var_of 是 var
                // 域 intern（槽装载期封闭；"#ret"
                // 预热——返回槽名可能不出现在任何槽行）。
                key_to_canon.reserve(shared_rows->size() * 2);
                canon_of_id.reserve(shared_rows->size() * 2);
                for (const cg::db::IndexRow& row : *shared_rows)
                {
                    const auto [kit, kfresh] = key_to_canon.try_emplace(
                        std::string_view{row.unique_key},
                        row.id);
                    canon_of_id.emplace(row.id, kit->second);
                }
            }

            /**
             * @brief QI this 证据类型的派生闭包位图（extends
             * 向下取后代，带缓存）。
             */
            [[nodiscard]]
            const SymBits* derived_closure_bits(std::string_view key)
            {
                constexpr std::size_t MAX_CLOSURE_NODES = 256;
                constexpr int         MAX_CLOSURE_DEPTH = 16;
                // QI this 证据的派生闭包（extends 边向下取后代记录）。动态类型
                // ∈ {所在类及其派生}：collect 只发 [所在类]
                // 单键（under-近似），此处 展开后成 over-近似（sound
                // 方向）。截断只少不多：缺候选 = 保守
                // 漏报，不会引入家族外假目标（dispatch 侧家族交拦截面不变）。
                //
                // 位图缓存形态：extends 下行图一次读入内存（首个派生行触发惰性
                // 初始化——语料无该形态行时零成本）；每根的后代闭包按符号 id
                // 位图
                // 化并缓存，行的多类型闭包先按位或合成并集再一次物化。集合与逐
                // 节点 SQL 形态恒等：BFS 同层全展开（同层邻居全部入集），第 d
                // 层 结束时的已见集仅由图结构决定，与邻接序/SQL
                // 行序无关；节点/深度
                // 上限的检查在层间，截断点的已见集同样与序无关。物化改 id 升序
                // 输出（SQL 形态是层序）——消费点装载后统一 sort+unique，终集合
                // 相同即等价。根自身不入集（SQL 形态 out 只含展开的后代）。
                //
                // 特化折算（CRTP 模板基形态）：采集把模板基的 extends 边记在
                // 主模板行上，特化行（实现类 `class Impl : Base<Impl>` 的直
                // 接基）无 extends 出边——只走 extends 边的后代闭包对 IDL 生
                // 成模式结构性失明（闭包只剩跨语言壳，永不含真实现，实测全体
                // 特化行 specializes 边齐全）。特化行实例化主模板、继承其全
                // 部 extends 基类，把 specializes 反向边（主模板 -> 特化行）
                // 并入下行邻接后，闭包按
                // 基类 -> 主模板 -> 特化行 -> 实现类折算出真实现（over-近
                // 似，sound 方向：面变宽只放宽裁剪与披露，不引入新误连）。
                // 空位图单例（根解析 miss 的返回值）保持函数内 static：模块
                // 接口单元里 namespace 域 inline const（动态初始化）变量不发射
                // 定义，跨模块消费方链接 LNK2001（探针实测，勿再上提）。
                static const SymBits EMPTY_BITS{};
                const auto           root = lookup.by_key(key);
                if (!root.has_value())
                {
                    return &EMPTY_BITS;
                }
                if (const auto cached = derived_bits_of.find(*root);
                    cached != derived_bits_of.end())
                {
                    return &cached->second;
                }
                if (!extends_down.has_value())
                {
                    std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                        down;
                    for (const cg::db::EdgeRow& e : cg::db::edges_of_type(
                             store,
                             cg::db::EdgeType::Extends))
                    {
                        down[e.dst].push_back(e.src); // 基类 -> 派生
                    }
                    for (const cg::db::EdgeRow& e : cg::db::edges_of_type(
                             store,
                             cg::db::EdgeType::Specializes))
                    {
                        // 主模板 -> 特化行（结构派生同构，见上方折算说明）。
                        down[e.dst].push_back(e.src);
                    }
                    for (auto& [id, adj] : down)
                    {
                        std::ranges::sort(adj); // 确定性（集合已证与序无关）
                    }
                    extends_down.emplace(std::move(down));
                }
                SymBits                          bits;
                std::unordered_set<std::int64_t> seen{*root};
                std::vector<std::int64_t>        frontier{*root};
                for (int d = 0; d < MAX_CLOSURE_DEPTH && !frontier.empty()
                                && seen.size() <= MAX_CLOSURE_NODES;
                     ++d)
                {
                    std::vector<std::int64_t> next;
                    for (const std::int64_t cur : frontier)
                    {
                        const auto it = extends_down->find(cur);
                        if (it == extends_down->end())
                        {
                            continue;
                        }
                        for (const std::int64_t derived : it->second)
                        {
                            if (!seen.insert(derived).second)
                            {
                                continue;
                            }
                            next.push_back(derived);
                        }
                    }
                    frontier = std::move(next);
                }
                for (const std::int64_t id : seen)
                {
                    if (id != *root)
                    {
                        bits.set(id);
                    }
                }
                return &derived_bits_of.emplace(*root, std::move(bits))
                            .first->second;
            }

            /**
             * @brief 方法的覆盖族成员（自身 + overrides 双向传递闭包）。
             *
             * @internal 与 dispatch 端 Hierarchy::override_family 同语义：
             *           虚调用的被调方法解析到接口/基类声明时，写回证据在
             *           各 override 实现的形参槽上——族并集沿 overrides 边
             *           取全体成员，绝不顺调用链跨函数取值（误连边界）。
             *           逐方法记忆化；成员按规范 id 升序输出（确定性）。
             */
            [[nodiscard]]
            const std::vector<std::int64_t>* virtual_family_members(
                std::int64_t canon)
            {
                if (const auto cached = virtual_family_of.find(canon);
                    cached != virtual_family_of.end())
                {
                    return &cached->second;
                }
                if (!overrides_adj.has_value())
                {
                    std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                        fwd; // override 方法 -> 被覆盖的基方法
                    std::unordered_map<std::int64_t, std::vector<std::int64_t>>
                        bwd; // 基方法 -> override
                    for (const cg::db::EdgeRow& e : cg::db::edges_of_type(
                             store,
                             cg::db::EdgeType::Overrides))
                    {
                        const auto s = canon_of_id.find(e.src);
                        const auto d = canon_of_id.find(e.dst);
                        if (s == canon_of_id.end() || d == canon_of_id.end())
                        {
                            continue; // 边端符号已删（悬空键）——宁漏勿错
                        }
                        fwd[s->second].push_back(d->second);
                        bwd[d->second].push_back(s->second);
                    }
                    const auto sort_adj = [](auto& table)
                    {
                        for (auto& [id, adj] : table)
                        {
                            std::ranges::sort(adj);
                        }
                    };
                    sort_adj(fwd);
                    sort_adj(bwd);
                    overrides_adj.emplace(std::move(fwd), std::move(bwd));
                }
                std::vector<std::int64_t>        family{canon};
                std::unordered_set<std::int64_t> seen{canon};
                std::vector<std::int64_t>        frontier{canon};
                while (!frontier.empty())
                {
                    // 新成员只入 next：范围 for 迭代 frontier 期间对同一
                    // vector 做 push_back 属未定义行为（重分配使缓存的
                    // 结束迭代器悬空），且逐轮换防才能展开多级链的全
                    // 闭包——与 derived_closure_bits 的分层 BFS 同范式。
                    std::vector<std::int64_t> next;
                    for (const std::int64_t cur : frontier)
                    {
                        for (const auto* table :
                             {&overrides_adj->first, &overrides_adj->second})
                        {
                            const auto it = table->find(cur);
                            if (it == table->end())
                            {
                                continue;
                            }
                            for (const std::int64_t other : it->second)
                            {
                                if (!seen.insert(other).second)
                                {
                                    continue;
                                }
                                family.push_back(other);
                                next.push_back(other);
                            }
                        }
                    }
                    frontier = std::move(next);
                }
                std::ranges::sort(family);
                return &virtual_family_of.emplace(canon, std::move(family))
                            .first->second;
            }

            /**
             * @brief 虚族并集的 pi 对位形参槽（族内全部有体成员槽）。
             *
             * @internal 直连槽无写回行时的回退查询源：族成员的 override
             *           实现形参序与声明一致（C++ 覆写契约），同位置槽直查
             *           param_of；槽存在即有体（形参数据行只在函数体采集时
             *           发射），无体声明成员与库外实现天然无槽不收录。成
             *           员自身的写回行不是门槛——转发出参形态（体内无
             *           `*pp=` 直写、指针转发给工厂/静态 Create）的实现类
             *           槽终态里有转发链带来的真类型，may 并集不区分证据
             *           的获取路径（旧门槛把这类真实现挡在族并集外，真类
             *           型只能靠污染并集捎带到达——污染被静态面裁剪后站点
             *           即饿死）。空槽成员照常收录：投递段按源槽空集跳过，
             *           与无行成员零差别。逐 (方法 × 形参位) 记忆化。
             */
            [[nodiscard]]
            const std::vector<std::uint32_t>* virtual_family_param_slots(
                std::int64_t canon,
                std::int32_t pv)
            {
                const SlotKey key{canon, pv};
                if (const auto cached = virtual_family_params_of.find(key);
                    cached != virtual_family_params_of.end())
                {
                    return &cached->second;
                }
                std::vector<std::uint32_t>       member_slots;
                const std::vector<std::int64_t>& family =
                    *virtual_family_members(canon);
                for (const std::int64_t member : family)
                {
                    if (member == canon)
                    {
                        continue; // 自身槽即直连槽，直连判定已失败
                    }
                    const auto it = param_of.find(SlotKey{member, pv});
                    if (it == param_of.end())
                    {
                        continue;
                    }
                    member_slots.push_back(
                        static_cast<std::uint32_t>(it->second));
                }
                std::ranges::sort(member_slots); // 确定性（param_of 值唯一）
                return &virtual_family_params_of
                            .emplace(key, std::move(member_slots))
                            .first->second;
            }

            /**
             * @brief 虚族扇投的 pi 对位形参槽（族内全部有体成员）。
             *
             * @internal 实参→形参流直连失败（被调解析到接口/基类声明，
             *           声明无函数体无槽）时的扇投目标集：族成员的
             *           override 实现形参序与声明一致（C++ 覆写契约），
             *           同位置槽直查 param_of；槽存在即有函数体（形参数
             *           据行只在函数体采集时发射），无体声明成员天然无槽
             *           不投。与写回族查询（virtual_family_param_slots）的门槛
             *           差异：入参方向只要能投证据就投，不要求写回行。
             *           逐 (方法 × 形参位) 记忆化。
             */
            [[nodiscard]]
            const std::vector<std::uint32_t>* virtual_family_body_param_slots(
                std::int64_t canon,
                std::int32_t pv)
            {
                const SlotKey key{canon, pv};
                if (const auto cached = virtual_family_body_params_of.find(key);
                    cached != virtual_family_body_params_of.end())
                {
                    return &cached->second;
                }
                std::vector<std::uint32_t>       member_slots;
                const std::vector<std::int64_t>& family =
                    *virtual_family_members(canon);
                for (const std::int64_t member : family)
                {
                    if (member == canon)
                    {
                        continue; // 自身槽即直连槽，直连判定已失败
                    }
                    if (lookup.is_pure_virtual_symbol(member))
                    {
                        // 纯虚声明成员（多级链的中间层）的形参行照常发射
                        // 但槽是传播死胡同——扇投只进有体实现。
                        continue;
                    }
                    const auto it = param_of.find(SlotKey{member, pv});
                    if (it == param_of.end())
                    {
                        continue;
                    }
                    member_slots.push_back(
                        static_cast<std::uint32_t>(it->second));
                }
                std::ranges::sort(member_slots); // 确定性（param_of 值唯一）
                return &virtual_family_body_params_of
                            .emplace(key, std::move(member_slots))
                            .first->second;
            }

            /**
             * @brief 族并集批类型对实参槽静态面的判定三态（面惰性构建）。
             *
             * @internal 两类不可解析的处置方向相反：静态面类型解析失败
             *           = 证明界立不起来，整体不裁（保守保持 may）；批类型
             *           解析失败 = 决议端同样解析不了它（id_of_key miss 计
             *           absent、家族距离无从参与），进槽只会阻断收敛——
             *           直接不投递（它不构成任何可消费的证据）。allowed
             *           面构建一次全批共用。面外（kClipped）与面内
             *           （kAllowed）分开回报：全批面外时消费方的对称守卫
             *           整体不裁（闭包残缺形态下真实现整批判界外，零投
             *           递会饿死唯一决议——宁漏勿饿）。
             */
            enum class FamilyFace : std::uint8_t
            {
                kUnresolvable, // 键不在符号域：决议端无法消费
                kAllowed,      // 面内（或面未立）：投递
                kClipped,      // 面外：裁剪（全批面外时由守卫回捞）
            };

            [[nodiscard]]
            FamilyFace family_face_of(std::uint32_t dst, std::int32_t type_id)
            {
                FamilyClip& c = slot_family_clip[dst];
                if (!c.init)
                {
                    c.init = true;
                    if (!slots[dst].static_types.empty())
                    {
                        bool    resolvable = true;
                        bool    face_any = false;
                        SymBits bits;
                        for (const std::string& s : slots[dst].static_types)
                        {
                            const auto sid = canon_of_key(s);
                            if (!sid.has_value())
                            {
                                // 键不在符号域 = 证明界立不起来，整体不裁
                                // （保守保持 may）。
                                resolvable = false;
                                break;
                            }
                            if (lookup.is_template_symbol(*sid))
                            {
                                // 模板记录（壳主模板）不是出参位的语义类
                                // 型面——其语义面是模板实参 T（采集侧已把
                                // 壳 pointee 实参键追加进静态面）：壳键自
                                // 身跳过、由实参键立界，壳槽的族并集投递
                                // 不再无界扩散；纯壳面（无实参键，旧库/
                                // 剥不出实参）剥壳后为空，按空面整体不裁。
                                continue;
                            }
                            bits.set(*sid); // 闭包不含根，自身补位
                            bits.or_with(*derived_closure_bits(s));
                            face_any = true;
                        }
                        if (resolvable && face_any)
                        {
                            c.clip = true;
                            c.allowed = std::move(bits);
                        }
                    }
                }
                const auto sid =
                    canon_of_key(type_of_id[static_cast<std::size_t>(type_id)]);
                if (!sid.has_value())
                {
                    return FamilyFace::kUnresolvable;
                }
                if (!c.clip)
                {
                    return FamilyFace::kAllowed;
                }
                return c.allowed.test(*sid) ? FamilyFace::kAllowed
                                            : FamilyFace::kClipped;
            }

            /**
             * @brief 族并集批按目标槽静态面过滤（结果落
             *        family_clip_scratch，空 = 零投递）。
             *
             * @internal 面内类型在前、面外（可解析）类型暂存其后：批内
             *           有任一面内类型时维持精确裁剪（截到面内前缀）。
             *           guard=true（扇投通道，批=实参证据并集）时全批面
             *           外整体不裁——allowed 面是闭包近似，缺边形态（模
             *           板特化基等未折算面）会把真实现整批判界外，零投
             *           递饿死唯一决议比 may 漏投代价高（宁漏勿饿的对称
             *           守卫，与"解析失败整体不裁"同一方向）；guard=false
             *           （写回通道）保持严格裁剪——写回边按族成员逐边
             *           投递、批是成员单类型集，逐边启用守卫会使任何界
             *           外成员类型都整边放行，裁剪退化为不裁（静态面保
             *           证的唯一决议被 may 并集稀释），写回侧的饿死形态
             *           由闭包折算（specializes 反向边）根治。全批不可
             *           解析时照旧零投递。
             */
            void family_clip_batch(
                std::uint32_t                 dst,
                std::span<const std::int32_t> batch,
                bool                          guard)
            {
                family_clip_scratch.clear();
                family_clip_scratch.reserve(batch.size());
                std::size_t kept = 0;
                for (const std::int32_t t : batch)
                {
                    switch (family_face_of(dst, t))
                    {
                    case FamilyFace::kAllowed:
                        family_clip_scratch.push_back(t);
                        ++kept;
                        break;
                    case FamilyFace::kClipped:
                        family_clip_scratch.push_back(t);
                        break;
                    case FamilyFace::kUnresolvable:
                        break; // 决议端无法消费——不投递
                    }
                }
                if (kept != 0)
                {
                    family_clip_scratch.resize(kept);
                }
                else if (!guard)
                {
                    family_clip_scratch.clear();
                }
            }

            /** @brief unique_key -> 规范符号 id（miss 返回 nullopt）。 */
            [[nodiscard]]
            std::optional<std::int64_t> canon_of_key(std::string_view key)
            {
                const auto it = key_to_canon.find(key);
                if (it == key_to_canon.end())
                {
                    return std::nullopt;
                }
                return it->second;
            }

            /**
             * @brief 槽侧快照恢复尝试（读侧）与 var 域初始化。
             *
             * @internal 成功则槽域/双 arena/类型域全部从平面段恢复，hints 只
             *           重读脏文件行；失败回退既有全表读（零分叉）。资格与
             *           增量资格正交：非增量批不尝试（全量重算无热装载可省），
             *           hierarchy_changed 的批不尝试。var 域整段恢复（id 保持
             *           快照值），"#ret" 预热命中返回快照 id，全量路径按原样
             *           从 0 赋号。
             */
            void try_restore_snapshot()
            {
                // ---- 槽侧快照恢复尝试（读侧）：成功则槽域/双
                // arena/类型域全部从 平面段恢复，hints
                // 只重读脏文件行；失败回退既有全表读（零分叉）。
                // 资格与增量资格正交：非增量批不尝试（全量重算无热装载可省），
                // hierarchy_changed 的批不尝试（out_param_this
                // 槽的装载期派生展开 已过时，必须从当前 extends
                // 全量装载）。----
                snap_miss = "off";
                const auto t_snap0 = std::chrono::steady_clock::now();
                if (plan != nullptr && plan->snap_request != nullptr)
                {
                    if (plan->active && !plan->hierarchy_changed)
                    {
                        snap = snap::restore(
                            *plan->snap_request,
                            SLOT_VER,
                            cg::db::schema_version,
                            cg::db::all_fingerprints(store),
                            snap_miss);
                    }
                    else
                    {
                        snap_miss = plan->active ? "hierarchy" : "inactive";
                    }
                }
                const auto t_snap1 = std::chrono::steady_clock::now();
                if (plan != nullptr)
                {
                    plan->snap_hit = snap.has_value();
                    plan->snap_miss_reason = snap_miss;
                    plan->snap_restore_us =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            t_snap1 - t_snap0)
                            .count();
                    plan->snap_dirty_files =
                        snap.has_value() ? snap->dirty_files.size() : 0;
                }
                if (snap.has_value())
                {
                    // var 域整段恢复（id 保持快照值；脏文件的新变量经
                    // intern_var 追加新号，与全量装载的域构造同构）。
                    var_of.reserve(snap->data.var_domain.size() * 2);
                    for (auto& [name, id] : snap->data.var_domain)
                    {
                        var_of.emplace(std::move(name), id);
                    }
                    next_var_id = snap->data.next_var_id;
                }
                // 返回槽名预热：快照域必含
                // #ret（装载时预热过，域段原样），命中返 回快照
                // id；全量路径按原样从 0 赋号。
                ret_var_id = [&]
                {
                    const auto [it, fresh] = var_of.try_emplace(
                        std::string{return_slot},
                        next_var_id);
                    if (fresh)
                    {
                        ++next_var_id;
                    }
                    return it->second;
                }();
            }

            /** @brief var 域 intern（首见赋新号，此后恒等返回）。 */
            [[nodiscard]]
            std::int32_t intern_var(std::string_view name)
            {
                const auto [it, fresh] =
                    var_of.try_emplace(std::string{name}, next_var_id);
                if (fresh)
                {
                    ++next_var_id;
                }
                return it->second;
            }

            /**
             * @brief 快照保留槽装填 + 脏文件行集装配（miss 则全表读）。
             *
             * @internal 半脏规则：槽源文件与指纹脏集相交即整槽剔除重建，
             *           重建行集扩到该槽全部源文件；owner 符号已从当前符号
             *           域消失的槽同样剔除。
             */
            void load_hint_rows()
            {
                // 槽类型集的单一连续缓冲：每槽一个 (type_off,type_len)
                // 区段（区段 语义与分代压缩见 TypeArena）。
                // 装载期的字符串类型集暂存（与 slots 下标对齐）：oracle
                // 在字符串形态 上运行，装载收尾统一 intern 进 HintSlot.types。
                // 装载期收集的行级别名引用（槽下标,
                // related_var）：写回行/壳字段写 行的 RHS
                // 变量。槽索引建立后解析成 row_alias_of 多源边。
                // 槽的源文件索引（与 slots 对齐；行流循环逐行收集）：槽行的
                // file_id 全集。装载期脏标记（slot_row_dirty）只用当前行，快照
                // delta 需要 每槽的完整文件面（半脏判定与下一轮快照的段内容）。
                // ---- 快照保留槽装填 +
                // 脏文件行集装配。半脏规则：槽源文件与指纹
                // 脏集相交即整槽剔除重建，重建行集扩到该槽全部源文件（槽行可跨
                // 文件，只重读脏半边会缺行错算）；owner
                // 符号已从当前符号域消失的 槽同样剔除（其源文件必在脏集，SQL
                // 重建路径同样建不出该槽）。----
                if (snap.has_value())
                {
                    snap::SnapshotData&                sd = snap->data;
                    std::unordered_set<cg::db::FileId> reread =
                        snap->dirty_files;
                    for (const snap::SlotRec& s : sd.slots)
                    {
                        for (const cg::db::FileId f : s.files)
                        {
                            if (snap->dirty_files.contains(f))
                            {
                                reread.insert(s.files.begin(), s.files.end());
                                break;
                            }
                        }
                    }
                    hint_rows = store.load_type_hints_analysis_files(
                        std::vector<cg::db::FileId>{
                            reread.begin(),
                            reread.end()});
                    // 保留槽的转型声称行补装：声称填充是 finalize 的单向注
                    // 入（不是传播边），增量闭包把保留槽复位到初始值重算后，
                    // 缓冲没有注入源就会整槽丢失（与全量重建分叉）。全量表
                    // 读路径天然覆盖；快照路径只重读脏文件侧，干净文件的声
                    // 称行在此补入（文件在重读集内的已随文件投影装载，不重
                    // 复）；这些行只可能命中保留槽——新行必在脏文件，其槽已
                    // 整槽剔除重建，行随重读集装载。
                    for (const cg::db::TypeHintRow& crow :
                         store.load_type_hints_of_source(
                             static_cast<std::int64_t>(HintSource::CastTarget)))
                    {
                        if (!reread.contains(crow.file_id))
                        {
                            hint_rows.push_back(crow);
                        }
                    }
                    slots.reserve(sd.slots.size() + hint_rows.size());
                    slot_of.reserve(sd.slots.size() * 2);
                    pending_types.reserve(sd.slots.size() + hint_rows.size());
                    slot_cast_claims.reserve(
                        sd.slots.size() + hint_rows.size());
                    slot_row_dirty.reserve(sd.slots.size() + hint_rows.size());
                    slot_derived_seed.reserve(
                        sd.slots.size() + hint_rows.size());
                    slot_file_list.reserve(sd.slots.size() + hint_rows.size());
                    snap_init_segs.reserve(sd.slots.size());
                    constexpr std::uint32_t SNAP_DROP =
                        std::numeric_limits<std::uint32_t>::max();
                    std::vector<std::uint32_t> new_of_snap(
                        sd.slots.size(),
                        SNAP_DROP);
                    std::size_t kept = 0;
                    for (std::size_t si = 0; si < sd.slots.size(); ++si)
                    {
                        const snap::SlotRec& s = sd.slots[si];
                        bool                 keep = true;
                        for (const cg::db::FileId f : s.files)
                        {
                            if (snap->dirty_files.contains(f))
                            {
                                keep = false;
                                break;
                            }
                        }
                        if (keep)
                        {
                            // owner 规范化：快照存键串（符号行 id
                            // 在文件重写后不稳 定，规范 id
                            // 随投影首见序漂移），经当前符号域重解析。
                            const auto oid = canon_of_key(s.owner_key);
                            if (!oid.has_value())
                            {
                                keep = false; // 符号已删：与全量装载（行被 FK
                                              // 级联 删除后建不出槽）行为一致
                            }
                            else
                            {
                                HintSlot slot;
                                slot.owner_key = s.owner_key;
                                slot.var_name = s.var_name;
                                slot.owner_id = *oid;
                                slot.var_id = intern_var(s.var_name);
                                slot.param_index = s.param_index;
                                slot.related_var = s.related_var;
                                slot.related_key = s.related_key;
                                slot.confidence = static_cast<Confidence>(
                                    std::clamp(s.confidence, 0, 3));
                                slot.source = source_of(s.source);
                                slot.reason = s.reason;
                                slot.has_outparam_flow = s.has_outparam_flow;
                                slot.static_types = s.static_types;
                                slots.push_back(std::move(slot));
                                // 保留槽的初始集已在快照 arena（oracle
                                // 已跑形态），
                                // 无字符串暂存；区段在类型域保序重映射后回填（见
                                // intern 段）。snap_init_segs 按 slots
                                // 下标对齐（保留
                                // 槽在行流重建槽之前连续装填，前缀即对齐面）。
                                pending_types.emplace_back();
                                slot_cast_claims.emplace_back();
                                slot_row_dirty.push_back(0);
                                slot_derived_seed.push_back(
                                    s.derived_seed ? 1 : 0);
                                slot_file_list.push_back(s.files);
                                snap_init_segs.emplace_back(
                                    s.init_off,
                                    s.init_len);
                                new_of_snap[si] = static_cast<std::uint32_t>(
                                    slots.size() - 1);
                                slot_of.emplace(
                                    SlotKey{*oid, slots.back().var_id},
                                    slots.size() - 1);
                                ++kept;
                            }
                        }
                    }
                    if (plan != nullptr)
                    {
                        plan->snap_rebuilt_slots = sd.slots.size() - kept;
                    }
                    // 行级别名边 remap：剔除槽的边丢弃（其槽经行流重建时重新收
                    // 集——重建行集已含该槽全部行）。
                    pending_row_alias.reserve(sd.row_alias.size());
                    for (const auto& [slot_idx, rel] : sd.row_alias)
                    {
                        if (new_of_snap[slot_idx] != SNAP_DROP)
                        {
                            pending_row_alias.emplace_back(
                                static_cast<std::size_t>(new_of_snap[slot_idx]),
                                rel);
                        }
                    }
                }
                else
                {
                    hint_rows = store.load_type_hints_analysis();
                    var_of.reserve(hint_rows.size() + 16);
                    // 全量装载的容量预留：槽与逐槽并行 vector 均按行数上界
                    // 预留（行流合并使槽数 <= 行数；快照分支在上文已按
                    // 保留槽+重读行双份预留）。
                    slot_row_dirty.reserve(hint_rows.size());
                    slot_derived_seed.reserve(hint_rows.size());
                    slot_file_list.reserve(hint_rows.size());
                }
                // 保留槽标记（与 slots 对齐）：快照恢复装填的槽为 1，行流新建为
                // 0。
                // 行流的行命中保留槽时跳过——重读集含半脏槽的干净半边文件，其上
                // 的完全干净槽（已从快照装填）会被行流重复命中；行级合并虽幂等，
                // 行级别名边会重复登记（边表非集合语义，重复项污染观测计数）。
                slot_from_snap.reserve(slots.capacity());
                if (snap.has_value())
                {
                    slot_from_snap.assign(slots.size(), 1);
                }
            }

            /**
             * @brief hints 行流装载（槽构建/合并）与装载收尾的 oracle 收敛。
             */
            void stream_hint_rows()
            {
                for (const cg::db::TypeHintRow& row : hint_rows)
                {
                    if (row.var_name == address_taken_slot)
                    {
                        continue; // &method
                                  // 取址证据行：仅供查询簇消费，不建传播槽
                    }
                    if (row.source == static_cast<int>(HintSource::External)
                        || row.var_name == external_slot)
                    {
                        // 外部边界 RTA 行：接口级可达集，站点级直用严禁——
                        // 双护栏之二（其一是 '`#external`'
                        // 槽名永不匹配接收者/实参
                        // 变量名）；不建传播槽，消费方是 dispatch 的展示面与
                        // cli 标注（全量读表，不走槽）。
                        continue;
                    }
                    const std::string owner = lookup.key_of(row.owner_id);
                    if (owner.empty())
                    {
                        continue;
                    }
                    const SlotKey skey{
                        canon_of_id.at(row.owner_id),
                        intern_var(row.var_name)};
                    auto [it, fresh] = slot_of.try_emplace(skey, slots.size());
                    if (fresh)
                    {
                        HintSlot slot;
                        slot.owner_key = owner;
                        slot.var_name = row.var_name;
                        slot.owner_id = skey.owner;
                        slot.var_id = skey.var;
                        slot.param_index = row.param_index;
                        slot.related_var = row.related_var;
                        slot.related_key = row.related_key;
                        slot.confidence = static_cast<Confidence>(
                            std::clamp(row.confidence, 0, 3));
                        slot.source = source_of(row.source);
                        slot.reason = row.fallback_reason;
                        slots.push_back(std::move(slot));
                        pending_types.emplace_back();
                        slot_cast_claims.emplace_back();
                        slot_row_dirty.push_back(0);
                        slot_derived_seed.push_back(0);
                        slot_file_list.emplace_back();
                        slot_from_snap.push_back(0);
                    }
                    // 保留槽的转型声称行是行流跳过的唯一例外：闭包卷入的
                    // 保留槽被复位到初始值重算后，声称缓冲必须有注入源（全
                    // 量路径行流天然装载）；其余行仍跳过（初始态已在快照，
                    // 重复应用会重复登记行级别名边）。保留槽的声称行只入缓
                    // 冲：文件清单/脏标记不动（快照的槽文件面已含声称行文
                    // 件，重复登记污染观测面）。
                    const bool retained = !fresh
                                          && slot_from_snap[it->second] != 0;
                    if (retained
                        && row.source
                               != static_cast<int>(HintSource::CastTarget))
                    {
                        continue; // 保留槽：初始态已在快照，行流不重复应用
                    }
                    if (!retained)
                    {
                        slot_file_list[it->second].push_back(row.file_id);
                        if (incr_candidate && dirty_fids.contains(row.file_id))
                        {
                            slot_row_dirty[it->second] = 1;
                        }
                    }
                    if (row.source
                        == static_cast<int>(HintSource::CastTarget))
                    {
                        // 转型声称行：只入缓冲——不发
                        // 传播边、不吸收槽标签（置信度/来源/reason 不动），
                        // 终态空槽判定在 finalize_hints；脏标记与文件清单
                        // 照常（声称行所在文件重写时槽必须进脏闭包重建）。
                        // 缓冲面 = 声称面 ∪ 派生闭包（运行时类型 ∈ 面内任
                        // 一记录；接口声称展开成实现族并集，与 qi iid 锁/
                        // 出参族的展开语义同款，闭包位图沿
                        // derived_closure_bits 的既有缓存与特化折算）——在
                        // 缓冲期展开使闭包串随缓冲进入 intern 类型域
                        // （finalize 期动态扩域会破坏快照域一致性）。巨族声
                        // 称（如 IDasBase 全库实现族）按根去重缓存物化面，
                        // 千行同根不再逐行重算。
                        std::vector<std::string>& claims =
                            slot_cast_claims[it->second];
                        for (const std::string& t :
                             parse_string_array(row.type_set))
                        {
                            cg::util::push_unique(claims, t);
                            auto fit = claim_face_of.find(t);
                            if (fit == claim_face_of.end())
                            {
                                std::vector<std::string> face;
                                derived_closure_bits(t)->each_set(
                                    [&](std::int64_t id)
                                    {
                                        const std::string derived =
                                            lookup.key_of(id);
                                        if (!derived.empty())
                                        {
                                            face.push_back(derived);
                                        }
                                    });
                                std::ranges::sort(face);
                                face.erase(
                                    std::unique(face.begin(), face.end()),
                                    face.end());
                                fit = claim_face_of
                                         .emplace(t, std::move(face))
                                         .first;
                            }
                            for (const std::string& d : fit->second)
                            {
                                cg::util::push_unique(claims, d);
                            }
                        }
                        continue;
                    }
                    HintSlot&                 slot = slots[it->second];
                    std::vector<std::string>& pending =
                        pending_types[it->second];
                    // 派生闭包种子行（槽终态含闭包展开值的增量重建标记）：
                    // out_param_this（QI this 写回）与 qi iid 锁两形态（接口
                    // 级类型入槽、装载期展开成实现族并集）。
                    if ((row.source
                             == static_cast<int>(HintSource::OutParamFlow)
                         && row.fallback_reason == "out_param_this")
                        || row.fallback_reason == "qi_iid_lock"
                        || row.fallback_reason == "qi_iid_ret")
                    {
                        slot_derived_seed[it->second] = 1;
                    }
                    if (row.source
                        == static_cast<int>(HintSource::OutParamFlow))
                    {
                        slot.has_outparam_flow = true;
                    }
                    // 行级多源边收集（OutParamFlow 写回行 + FieldFlow
                    // 壳字段写行）： related_var 指向 RHS 变量。槽级
                    // related_var 单值会让多写回源
                    // （多写点/多函数写同一出参或字段）只有装载序第一条建边，其余
                    // 源的并集被吞（参考
                    // 语料实测：出参槽/字段槽只留先到值）。行级解析
                    // 与消费在槽索引建立之后（见 row_alias_of 构建段）。
                    if ((row.source
                             == static_cast<int>(HintSource::OutParamFlow)
                         || row.source
                                == static_cast<int>(HintSource::FieldFlow))
                        && !row.related_var.empty())
                    {
                        pending_row_alias.emplace_back(
                            it->second,
                            row.related_var);
                    }
                    const std::vector<std::string> parsed =
                        parse_string_array(row.type_set);
                    if (row.source == static_cast<int>(HintSource::Declared))
                    {
                        for (const std::string& t : parsed)
                        {
                            slot.static_types.push_back(t);
                        }
                    }
                    else
                    {
                        for (const std::string& t : parsed)
                        {
                            pending.push_back(t);
                        }
                        // 派生闭包展开行：接口级类型（QI this 写回 / iid 锁
                        // 的出参槽与 As 结果槽）展开成实现族并集——实现族
                        // 恰一时决议端收敛唯一、多于一站 may（qi_iid_lock /
                        // qi_iid_ret 与 out_param_this 同一并集物化路径）。
                        // qi 两形态的展开熔断：真库（687 TU 全量）实测该物
                        // 化路径在写库段触发未定位的段错误（exit 139，五次
                        // 复现；隔离实验三角定位到展开物化本身——发射关/
                        // 仅 face 行/展开关三种形态全部成功，仅展开开崩溃；
                        // 接口闭包规模实测最大 25，非规模因素），按"拿不准
                        // 的形态宁可不开闸"纪律整体禁用至根因定位：接口键
                        // 原样入槽、face 声明面裁剪照常生效，iid 锁的收敛
                        // 收益（qi 展开与 As 调用形态的射程）暂缓。
                        constexpr bool QI_EXPANSION_FUSED = true;
                        bool           qi_expand = !QI_EXPANSION_FUSED;
                        if (((row.source
                                  == static_cast<int>(HintSource::OutParamFlow)
                              && row.fallback_reason == "out_param_this")
                             || row.fallback_reason == "qi_iid_ret"
                             || row.fallback_reason == "qi_iid_lock")
                            && qi_expand)
                        {
                            // 行级并集：parsed
                            // 各类型的后代闭包位图按位或，一次物 化（id 升序 ->
                            // key_of 过滤空键）——与逐类型逐后代追
                            // 加等价（终集合相同，pending 之后统一
                            // sort+unique）。
                            SymBits row_bits;
                            for (const std::string& t : parsed)
                            {
                                row_bits.or_with(*derived_closure_bits(t));
                            }
                            row_bits.each_set(
                                [&](std::int64_t id)
                                {
                                    const std::string derived =
                                        lookup.key_of(id);
                                    if (!derived.empty())
                                    {
                                        pending.push_back(std::move(derived));
                                    }
                                });
                        }
                    }
                    if (slot.related_var.empty())
                    {
                        slot.related_var = row.related_var;
                    }
                    if (slot.related_key.empty())
                    {
                        slot.related_key = row.related_key;
                    }
                    if (row.param_index >= 0)
                    {
                        slot.param_index = row.param_index;
                    }
                    if (static_cast<Confidence>(
                            std::clamp(row.confidence, 0, 3))
                        < slot.confidence)
                    {
                        slot.confidence = static_cast<Confidence>(
                            std::clamp(row.confidence, 0, 3));
                        slot.source = source_of(row.source);
                        slot.reason = row.fallback_reason;
                    }
                }
                for (std::size_t i = 0; i < slots.size(); ++i)
                {
                    HintSlot&                 slot = slots[i];
                    std::vector<std::string>& types = pending_types[i];
                    std::ranges::sort(types);
                    types.erase(
                        std::unique(types.begin(), types.end()),
                        types.end());
                    std::ranges::sort(slot.static_types);
                    slot.static_types.erase(
                        std::unique(
                            slot.static_types.begin(),
                            slot.static_types.end()),
                        slot.static_types.end());
                    if (!types.empty())
                    {
                        TypeHintRec probe;
                        probe.owner_key = slot.owner_key;
                        probe.var_name = slot.var_name;
                        probe.type_keys = types;
                        probe.static_types = slot.static_types;
                        probe.source = slot.source;
                        probe.confidence = slot.confidence;
                        types = oracle.points_to(probe);
                    }
                }
                t_hints = std::chrono::steady_clock::now();
            }

            /** @brief call_sites 行装载成站点（callee 解析与行内旧键见证）。 */
            void build_sites()
            {
                sites.reserve(site_rows.size());
                // 文件路径整表一次物化：逐站点单键查询在万级站点上是每行一条
                // SQL 的重复成本；files
                // 表在分析期只读，映射与逐行查询结果一致。
                const std::unordered_map<cg::db::FileId, std::string>
                    file_paths = cg::db::file_path_map(store);
                // 与 sites 对齐的行内旧值（翻转见证）：callee_key
                // 是文本列，无
                // FK，被调用者符号被删/被改键后仍保留上次收敛写入的旧键——解析
                // 翻转（新键 ≠ 旧键）即扇入站点的判据。
                for (const cg::db::CallSiteRow& row : site_rows)
                {
                    const std::string caller = lookup.key_of(row.caller_id);
                    if (caller.empty())
                    {
                        continue;
                    }
                    PropagatedSite site;
                    site.site_id = row.id;
                    site.caller_key = caller;
                    site.caller_owner_key = lookup.owner_key_of(row.caller_id);
                    site.is_virtual = row.is_virtual;
                    site.qualified = row.qualified;
                    site.indirect = row.indirect;
                    site.receiver_name = row.receiver_name;
                    site.receiver_is_this = row.receiver_is_this;
                    site.receiver_owner_key = row.receiver_owner_key;
                    site.file_id = row.file_id;
                    if (const auto fit = file_paths.find(row.file_id);
                        fit != file_paths.end())
                    {
                        site.file = fit->second;
                    }
                    site.line = row.line;
                    // 静态判证面只认 parse 专写的声明列：receiver_types 是
                    // dispatch 写回的静态∪动态并集（披露面），读它会把上轮
                    // 动态类型混进静态根集，接收者可达收窄/封口判定整族失强
                    // （同名库二次全量重算边数膨胀的根因）。
                    site.static_types = parse_string_array(row.receiver_static);
                    site.arg_types = parse_string_array_2d(row.arg_types);
                    site.arg_vars = parse_string_array(row.arg_vars);
                    site.arg_call_keys = parse_string_array(row.arg_call_keys);
                    site.callee.name = row.callee_name;
                    site.callee.ns = row.callee_ns;
                    site.callee.parent = row.callee_parent;
                    site.callee.sig_hash = row.callee_sig;
                    site.callee.const_method = row.callee_const;
                    site.callee_key = row.callee_key;
                    Confidence res = Confidence::Fallback;
                    // 加载系 API 名单命中（collect 侧界外放行的 Win32/POSIX
                    // 加载器 站点，或 TU
                    // 内前置声明的同形态）：按名解析一律短路——界外 callee
                    // 本无库内符号，短路防 API 名撞库内同名定义产生假 calls
                    // 边（宁漏勿猜）；站点证据价值在 external 配对，不在边。
                    if (!site.callee.name.empty()
                        && !is_dlopen_api(site.callee.name)
                        && callback_param_slot(site.callee.name) == 0)
                    {
                        site.callee_id = lookup.resolve(site.callee, res);
                        if (site.callee_id.has_value())
                        {
                            site.callee_key = lookup.key_of(*site.callee_id);
                        }
                    }
                    site.callee_resolved = site.callee_id.has_value();
                    site.resolution = res;
                    if (site.is_virtual && site.callee_id.has_value())
                    {
                        site.method_id = site.callee_id;
                        site.method_key = site.callee_key;
                    }
                    site_row_dirty.push_back(
                        incr_candidate && dirty_fids.contains(row.file_id) ? 1
                                                                           : 0);
                    site_row_callee_key.push_back(row.callee_key);
                    sites.push_back(std::move(site));
                }
                t_sites = std::chrono::steady_clock::now();
            }

            /** @brief 形参槽/槽索引构建（整数键域，等价性见 SlotKey 注释）。 */
            void build_slot_index()
            {
                // 形参槽：owner + param_index -> 槽下标；返回槽：owner ->
                // 槽下标。 别名注册与槽 source 解耦：param_index 只有 ParamFlow
                // 行携带（写回 行恒 -1），OutParamFlow 行置信更低时会覆盖槽的
                // source 标签，但形 参身份由序号证明，#p{i} 寻址不受影响（取址
                // 实参依赖此别名触达写回槽）。 形参序号 -> "#p{n}" 的 var 域
                // id（与 param_of 的键值同步注册）：
                // 站点实参解析按序号直查，替代逐实参格式化串构造。
                for (std::size_t i = 0; i < slots.size(); ++i)
                {
                    const HintSlot& slot = slots[i];
                    if (slot.param_index >= 0)
                    {
                        const std::int32_t pv =
                            intern_var(std::format("#p{}", slot.param_index));
                        param_var_of.try_emplace(slot.param_index, pv);
                        param_of.emplace(SlotKey{slot.owner_id, pv}, i);
                    }
                }
                for (std::size_t i = 0; i < slots.size(); ++i)
                {
                    slot_index.emplace(
                        SlotKey{slots[i].owner_id, slots[i].var_id},
                        i);
                }
                // add_types 的归并暂存（arena 外的稳定缓冲，跨投递复用容量）：
                // 归并输出不落 arena，append_dedup
                // 命中既有区段时零追加——批/现集 区段即使与 arena
                // 内部重合也只在归并期间被读，追加扩容不影响
                // scratch，旧形态的自环下标换算防护因此不再需要。
                t_slotidx = std::chrono::steady_clock::now();
            }

            /**
             * @brief related 键的宿主规范化（返回槽宿主/@fn 解析，其余原样）。
             */
            [[nodiscard]]
            std::string related_owner_key(const std::string& related)
            {
                if (related.empty())
                {
                    return related;
                }
                // "related 即返回槽宿主" 判定走整数键（owner 解析 + #ret 域
                // id）： 键串解析失败 = 无此宿主 = 无该返回槽，与字符串
                // contains 同义。
                const auto rid = canon_of_key(related);
                if (rid.has_value()
                    && slot_index.contains(SlotKey{*rid, ret_var_id}))
                {
                    return related;
                }
                if (related.starts_with("@fn|"))
                {
                    Confidence conf = Confidence::Fallback;
                    if (const auto id =
                            lookup.resolve(decode_method_ref(related), conf);
                        id.has_value())
                    {
                        return lookup.key_of(*id);
                    }
                }
                return related;
            }

            /** @brief owner×var 的槽查找（var 域封闭，miss 即无槽）。 */
            [[nodiscard]]
            std::optional<std::size_t> find_slot(
                std::string_view owner,
                std::string_view var)
            {
                // owner 规范化 + var 域查找（var 域已封闭：miss 即无槽，与
                // 字符串键 find miss 同义；owner 键串不在符号域 =
                // 无槽以此为宿主）。
                const auto oid = canon_of_key(owner);
                const auto vit = var_of.find(var);
                if (!oid.has_value() || vit == var_of.end())
                {
                    return std::nullopt;
                }
                const auto it = slot_index.find(SlotKey{*oid, vit->second});
                if (it == slot_index.end())
                {
                    return std::nullopt;
                }
                return it->second;
            }

            /**
             * @brief 站点实参槽解析（裸名在调用方域，@fld
             * 按声明域）。
             */
            [[nodiscard]]
            std::optional<std::size_t> arg_slot_of(
                const PropagatedSite& site,
                std::size_t           i)
            {
                // 实参槽解析：裸变量名在调用方函数域；@fld
                // 描述符 按声明记录域（字段实参与取址槽接收者同一路径）。
                if (i >= site.arg_vars.size() || site.arg_vars[i].empty())
                {
                    return std::nullopt;
                }
                const std::string& v = site.arg_vars[i];
                if (v.starts_with(field_slot_prefix))
                {
                    const auto field = decode_field_slot(v);
                    if (!field.has_value())
                    {
                        return std::nullopt;
                    }
                    return find_slot(field->first, field->second);
                }
                return find_slot(site.caller_key, v);
            }

            /**
             * @brief 类型集投递（保序并集 +
             * 置信度取低者；命中既有区段零追加）。
             *
             * @return 是否发生变更（新增类型或置信度下探/标签改写）。
             */
            [[nodiscard]]
            bool add_types(
                HintSlot&                     slot,
                std::span<const std::int32_t> types,
                Confidence                    conf,
                HintSource                    source,
                std::string_view              reason)
            {
                // 并集语义（与 binary_search miss + push_back + sort + unique
                // 等价）：先数批中不在现集的元素，有新增才把并集双指针归并到
                // 暂存（保序去重，空串 id 写入侧跳过），经区段驻留落 arena
                // （命中同内容既有区段直接复用）；零新增不写入（区段不动）。
                std::size_t n_new = 0;
                {
                    const std::span<const std::int32_t> cur =
                        arena.view(slot.type_off, slot.type_len);
                    for (const std::int32_t t : types)
                    {
                        if (t == EMPTY_TYPE_ID)
                        {
                            continue; // 空串 id：批内参与计数，写入侧跳过
                        }
                        if (!std::ranges::binary_search(cur, t))
                        {
                            ++n_new;
                        }
                    }
                }
                bool changed = false;
                if (n_new > 0)
                {
                    const std::span<const std::int32_t> cur =
                        arena.view(slot.type_off, slot.type_len);
                    merge_scratch.clear();
                    merge_scratch.reserve(cur.size() + n_new);
                    const std::int32_t*       p = cur.data();
                    const std::int32_t* const p_end = p + cur.size();
                    const std::int32_t*       q = types.data();
                    const std::int32_t* const q_end = q + types.size();
                    while (p != p_end || q != q_end)
                    {
                        if (q != q_end && *q == EMPTY_TYPE_ID)
                        {
                            ++q;
                            continue;
                        }
                        const bool has_p = p != p_end;
                        const bool has_q = q != q_end;
                        if (has_q && (!has_p || *q < *p))
                        {
                            merge_scratch.push_back(*q++);
                        }
                        else if (has_p)
                        {
                            merge_scratch.push_back(*p++);
                            if (has_q && *q == merge_scratch.back())
                            {
                                ++q; // 两集同值：取现集一份，批侧跳过
                            }
                        }
                        else
                        {
                            break; // 双尽（仅剩空 id 批元素）：计数已排除
                        }
                    }
                    const auto [off, len] = arena.append_dedup(
                        std::span<const std::int32_t>{
                            merge_scratch.data(),
                            merge_scratch.size()});
                    slot.type_off = off;
                    slot.type_len = len;
                    changed = true;
                }
                if (!types.empty() && conf < slot.confidence)
                {
                    slot.confidence = conf;
                    slot.source = source;
                    slot.reason = std::string{reason};
                    changed = true;
                }
                return changed;
            }

            /**
             * @brief 增量 warm start：装载上次提交段的槽终态。
             *
             * @internal 三来源优先级：驻留（同进程，免任何读）> 快照（跨进程
             *           平面段）> 全表读；ver 失配或槽存在而 warm 空表时整体
             *           降级全量。
             */
            void load_warm_start()
            {
                // ---- 增量 warm start：装载上次提交段的槽终态。ver
                // 失配、或槽存在 而 warm
                // 表为空（库从未收敛）时整体降级全量。三来源优先级：驻留
                // （同进程，免任何读）> 快照（跨进程平面段）>
                // 全表读。快照形态的 行带 type_seg（终态 arena 区段）而
                // type_set 为空——回填与比对 的双形态契约见
                // AnalysisSlotRow::type_seg。ver/warm-empty 检查与
                // 全表读路径同款。----
                if (incr_candidate && !plan->degraded)
                {
                    if (plan->warm_preloaded != nullptr)
                    {
                        for (const auto& [key, row] : *plan->warm_preloaded)
                        {
                            if (row.ver != SLOT_VER)
                            {
                                plan->degraded = true;
                                plan->degrade_reason = "slot-ver";
                                break;
                            }
                        }
                        if (!plan->degraded)
                        {
                            warm = plan->warm_preloaded;
                            plan->warm_loaded = warm;
                        }
                    }
                    else if (snap.has_value())
                    {
                        // 快照恢复：warm 行集 = 快照终态全量（含被剔除槽的行——
                        // warm 是写回 diff
                        // 的基准，必须与"上批全表读"恒等）。类型
                        // 集以区段引用承载（id 与快照域同域，restore 已自检），
                        // ver 恒等由头部 slots_ver 校验背书。
                        plan->warm.reserve(snap->data.slots.size() * 2);
                        for (const snap::SlotRec& s : snap->data.slots)
                        {
                            cg::db::AnalysisSlotRow row;
                            row.owner_key = s.owner_key;
                            row.var_name = s.var_name;
                            row.confidence = s.warm_confidence;
                            row.source = s.warm_source;
                            row.reason = s.warm_reason;
                            row.truncated = s.truncated;
                            row.ver = SLOT_VER;
                            row.alias_src = s.alias_src;
                            row.ret_src = s.ret_src;
                            row.type_seg =
                                std::pair<std::uint32_t, std::uint32_t>{
                                    s.warm_off,
                                    s.warm_len};
                            plan->warm.emplace(
                                hint_slot_key(row.owner_key, row.var_name),
                                std::move(row));
                        }
                        warm = &plan->warm;
                        plan->warm_loaded = warm;
                        warm_from_snap = true;
                        plan->warm_from_snap = true;
                    }
                    else
                    {
                        for (cg::db::AnalysisSlotRow& row :
                             store.all_analysis_slots())
                        {
                            if (row.ver != SLOT_VER)
                            {
                                plan->degraded = true;
                                plan->degrade_reason = "slot-ver";
                                break;
                            }
                            plan->warm.emplace(
                                hint_slot_key(row.owner_key, row.var_name),
                                std::move(row));
                        }
                        if (!plan->degraded)
                        {
                            warm = &plan->warm;
                            plan->warm_loaded = warm;
                        }
                    }
                    if (!plan->degraded && warm != nullptr && warm->empty()
                        && !slots.empty())
                    {
                        plan->degraded = true;
                        plan->degrade_reason = "warm-empty";
                    }
                }
                t_warm = std::chrono::steady_clock::now();
            }

            /**
             * @brief 类型集 intern：全库类型串按字典序赋号并回填槽初始区段。
             *
             * @internal 快照路径：类型域 = 快照域 ∪ 新串（有序并）+ 保序重
             *           映射；实参静态批同步 intern 成 id 形态副本（仅已解
             *           析站点参与传播）。
             */
            void intern_type_sets()
            {
                // ---- 类型集 intern：全库类型串（槽终态 + 实参静态批 + warm
                // 终态） 按字典序赋号，整数序 ==
                // 字符串序——排序、去重与截断保留的子集和
                // 字符串形态逐字节等价；空串最小，恒落 id 0（见
                // EMPTY_TYPE_ID）。 快照路径：类型域 = 快照域（有序）∪
                // 新串（脏槽 + 实参静态批）， 有序合并并构建保序重映射（旧 id →
                // 新 id 严格递增），两个 arena 段重映射后序性质保持；warm
                // 的串已在快照域，整段跳过 JSON 解析。 ----
                {
                    std::unordered_set<std::string> distinct;
                    distinct.insert(std::string{});
                    for (const std::vector<std::string>& ts : pending_types)
                    {
                        distinct.insert(ts.begin(), ts.end());
                    }
                    // 转型声称串参与赋号（终态空槽填充需要 id 形态）。
                    for (const std::vector<std::string>& ts : slot_cast_claims)
                    {
                        distinct.insert(ts.begin(), ts.end());
                    }
                    for (const PropagatedSite& site : sites)
                    {
                        if (!site.callee_id.has_value())
                        {
                            continue;
                        }
                        for (const std::vector<std::string>& row :
                             site.arg_types)
                        {
                            distinct.insert(row.begin(), row.end());
                        }
                    }
                    // warm 槽的类型串也参与赋号（闭包外槽回填 intern
                    // 表示需要它们）。
                    if (incr_candidate && !plan->degraded && !warm_from_snap)
                    {
                        for (const auto& [key, row] : *warm)
                        {
                            for (const std::string& t :
                                 parse_string_array(row.type_set))
                            {
                                distinct.insert(t);
                            }
                        }
                    }
                    if (snap.has_value())
                    {
                        // 恢复域 = 快照域 ∪
                        // 新串（有序并）。快照域含已从库中消失的
                        // 串（脏文件删除形态）是常态——内部编号有洞，对消费面不可
                        // 观测（域是字典序超集，id 序 == 字符串序的性质保持）。
                        std::vector<std::string> extra(
                            distinct.begin(),
                            distinct.end());
                        std::ranges::sort(extra);
                        extra.erase(
                            std::unique(extra.begin(), extra.end()),
                            extra.end());
                        const std::vector<std::string>& old =
                            snap->data.type_of_id;
                        type_of_id.reserve(old.size() + extra.size());
                        snap_id_remap.assign(old.size(), 0);
                        std::size_t a = 0;
                        std::size_t b = 0;
                        while (a < old.size() || b < extra.size())
                        {
                            if (b >= extra.size())
                            {
                                snap_id_remap[a] = static_cast<std::int32_t>(
                                    type_of_id.size());
                                type_of_id.push_back(old[a]);
                                ++a;
                            }
                            else if (a >= old.size() || extra[b] < old[a])
                            {
                                type_of_id.push_back(extra[b]);
                                ++b;
                            }
                            else
                            {
                                snap_id_remap[a] = static_cast<std::int32_t>(
                                    type_of_id.size());
                                type_of_id.push_back(old[a]);
                                ++a;
                                if (extra[b] == old[a - 1])
                                {
                                    ++b; // 同串：快照侧映射后跳过新串侧
                                }
                            }
                        }
                    }
                    else
                    {
                        type_of_id.assign(distinct.begin(), distinct.end());
                        std::ranges::sort(type_of_id);
                    }
                    id_of_type.reserve(type_of_id.size() * 2);
                    for (std::size_t i = 0; i < type_of_id.size(); ++i)
                    {
                        id_of_type.emplace(
                            type_of_id[i],
                            static_cast<std::int32_t>(i));
                    }
                    if (snap.has_value())
                    {
                        // 快照初始 arena
                        // 整段载入（元素重映射到恢复域），保留槽区段
                        // 直指（区段内容即装载期收尾的 oracle
                        // 后初始集）；区段驻留
                        // 表按保留槽区段重建（与全量装载的逐槽登记同构）。
                        std::vector<std::int32_t>& init_buf =
                            snap->data.arena_init;
                        for (std::int32_t& v : init_buf)
                        {
                            v = snap_id_remap[static_cast<std::size_t>(v)];
                        }
                        arena.buf = std::move(init_buf);
                        const auto rededup =
                            [&](std::uint32_t off, std::uint32_t len)
                        {
                            if (len == 0)
                            {
                                return;
                            }
                            const std::uint64_t h =
                                TypeArena::span_hash(arena.view(off, len));
                            auto& chain = arena.dedup[h];
                            for (const auto& [o, l] : chain)
                            {
                                if (o == off && l == len)
                                {
                                    return; // 共享区段只登记一次
                                }
                            }
                            chain.emplace_back(off, len);
                        };
                        for (std::size_t i = 0; i < slots.size(); ++i)
                        {
                            if (slot_from_snap.empty()
                                || slot_from_snap[i] == 0)
                            {
                                continue;
                            }
                            const auto [off, len] = snap_init_segs[i];
                            slots[i].type_off = off;
                            slots[i].type_len = len;
                            rededup(off, len);
                        }
                    }
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        if (!slot_from_snap.empty() && slot_from_snap[i] != 0)
                        {
                            continue; // 快照保留槽：初始区段已回填
                        }
                        // 初始槽区段经驻留落
                        // arena：同集槽共享区段（区段级驻留的
                        // 收益主体——非空类型集重复度高），arena 体积按 distinct
                        // 集合计。
                        const std::vector<std::int32_t> ids =
                            intern_ids(pending_types[i]);
                        const auto [off, len] = arena.append_dedup(
                            std::span<const std::int32_t>{
                                ids.data(),
                                ids.size()});
                        slots[i].type_off = off;
                        slots[i].type_len = len;
                    }
                    pending_types.clear();
                    pending_types.shrink_to_fit();
                    // 实参静态批的 intern
                    // 副本（仅被调用者已解析的站点参与传播）。
                    arg_ids.resize(sites.size());
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        if (!sites[j].callee_id.has_value())
                        {
                            continue;
                        }
                        arg_ids[j].reserve(sites[j].arg_types.size());
                        for (const std::vector<std::string>& row :
                             sites[j].arg_types)
                        {
                            arg_ids[j].push_back(intern_ids(row));
                        }
                    }
                }
            }

            /** @brief 类型串集 -> 有序去重的 intern id 集（warm 终态回填用）。
             */
            [[nodiscard]]
            std::vector<std::int32_t> intern_ids(
                const std::vector<std::string>& row)
            {
                std::vector<std::int32_t> ids;
                ids.reserve(row.size());
                for (const std::string& t : row)
                {
                    ids.push_back(id_of_type.at(t));
                }
                std::ranges::sort(ids);
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                return ids;
            }

            /**
             * @brief 快照初始态捕获（写侧）：装载收尾时刻的槽平面态。
             *
             * @internal buf 在终态预拼接之前拍照（拼接段不属于初始态）；
             *           序列化与指纹基线/seq 由调用方在提交事务内补齐。
             */
            void capture_snapshot_initial()
            {
                // ---- 快照初始态捕获（写侧）：装载收尾时刻的槽平面态——oracle
                // 已 跑后的初始区段、槽标量、var
                // 域、类型域、行级别名边、槽源文件索 引、静态类型集。buf
                // 在终态预拼接之前拍照（拼接段不属于初始态）。
                // 序列化与指纹基线/seq 由调用方在提交事务内补齐，落盘在事务提交
                // 成功之后（先库后快照）。----
                if (plan != nullptr && plan->snap_dump != nullptr)
                {
                    snap::SnapshotData& sd = *plan->snap_dump;
                    sd.slots_ver = static_cast<std::uint32_t>(SLOT_VER);
                    sd.schema_version =
                        static_cast<std::uint32_t>(cg::db::schema_version);
                    sd.arena_init = arena.buf;
                    sd.var_domain.reserve(var_of.size());
                    for (const auto& [name, id] : var_of)
                    {
                        sd.var_domain.emplace_back(name, id);
                    }
                    sd.next_var_id = next_var_id;
                    sd.type_of_id = type_of_id;
                    sd.row_alias.reserve(pending_row_alias.size());
                    for (const auto& [slot, rel] : pending_row_alias)
                    {
                        sd.row_alias.emplace_back(
                            static_cast<std::uint32_t>(slot),
                            rel);
                    }
                    sd.slots.resize(slots.size());
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        const HintSlot& slot = slots[i];
                        snap::SlotRec&  r = sd.slots[i];
                        r.owner_key = slot.owner_key;
                        r.var_name = slot.var_name;
                        r.init_off = slot.type_off;
                        r.init_len = slot.type_len;
                        r.confidence = static_cast<int>(slot.confidence);
                        r.source = static_cast<int>(slot.source);
                        r.reason = slot.reason;
                        r.related_var = slot.related_var;
                        r.related_key = slot.related_key;
                        r.param_index = slot.param_index;
                        r.has_outparam_flow = slot.has_outparam_flow;
                        r.derived_seed = slot_derived_seed[i] != 0;
                        r.static_types = slot.static_types;
                        auto& files = slot_file_list[i];
                        std::ranges::sort(files);
                        files.erase(
                            std::unique(files.begin(), files.end()),
                            files.end());
                        r.files = std::move(files);
                    }
                }
            }

            /**
             * @brief 快照终态 arena 预拼接：warm 区段重映射后一次性追加进
             * arena。
             *
             * @internal warm 行的 type_seg 平移到拼接基址，回填点免逐槽 JSON
             *           解析 + intern + append；降级批的拼接段成为死区，分代
             *           压缩兜底回收。
             */
            void preconcat_warm_arena()
            {
                // ---- 快照终态 arena 预拼接：warm 行的区段（恢复域 id
                // 重映射后） 一次性追加进 arena，warm 行的 type_seg
                // 平移到拼接基址——下方 warm 回填点直接平移区段，免逐槽 JSON
                // 解析 + intern + append（装
                // 载税的大头之一）。驻留表按终态区段重建（回填后的活区段与传播期
                // add_types
                // 的驻留查询同构）。降级批同样安全：拼接段成为死区，槽
                // 区段引用不受影响，分代压缩兜底回收。 ----
                if (warm_from_snap && incr_candidate && !plan->degraded)
                {
                    std::vector<std::int32_t>& wb = snap->data.arena_warm;
                    for (std::int32_t& v : wb)
                    {
                        v = snap_id_remap[static_cast<std::size_t>(v)];
                    }
                    const std::uint32_t wbase =
                        static_cast<std::uint32_t>(arena.buf.size());
                    arena.buf.insert(arena.buf.end(), wb.begin(), wb.end());
                    const auto dedup_warm =
                        [&](std::uint32_t off, std::uint32_t len)
                    {
                        if (len == 0)
                        {
                            return;
                        }
                        const std::uint64_t h =
                            TypeArena::span_hash(arena.view(off, len));
                        auto& chain = arena.dedup[h];
                        for (const auto& [o, l] : chain)
                        {
                            if (o == off && l == len)
                            {
                                return;
                            }
                        }
                        chain.emplace_back(off, len);
                    };
                    for (auto& [key, row] : plan->warm)
                    {
                        if (!row.type_seg.has_value())
                        {
                            continue;
                        }
                        row.type_seg->first += wbase;
                        dedup_warm(row.type_seg->first, row.type_seg->second);
                    }
                }
            }

            /**
             * @brief
             * 传播边静态预解析：别名源/形参目标/写回边/返回消费边各解析一次。
             *
             * @internal 行级多源边在此解析成 (dst, src) 并入 l1_deps 扇出；
             *           增量专用的喂料站点表与 owner 槽表仅候选时填充。
             */
            void preresolve_edges()
            {
                // ---- 传播边静态预解析：槽/站点集合在传播期不变，别名源、形参
                // 目标、写回边、返回消费边各解析一次（全扫版每轮重复解析）。
                // ----
                // 别名引用（related_var）到源槽的解析：@elem/@fld/@gvar/@fnv
                // 前缀按
                // 编码携带的域（字段槽按声明记录域、容器元素槽按局部函数域或声明
                // 记录域、全局变量槽按变量记录域、函数域变量槽按写函数域），裸名
                // 按 owner
                // 域。槽级别名（单值）与行级多源边（写回/壳字段写行）共用。
                site_args.resize(sites.size());
                site_wb.resize(sites.size());
                slot_family_fed.assign(slots.size(), 0);
                slot_family_pf_fed.assign(slots.size(), 0);
                slot_family_clip.assign(slots.size(), FamilyClip{});
                l1_deps.resize(slots.size());
                a_src_sites.resize(slots.size());
                b_param_sites.resize(slots.size());
                c_consumers.resize(slots.size());
                // 增量专用：形参槽 ->
                // 喂它的站点（闭包槽复位后必须由这些站点 重新供料）与 owner ->
                // 槽列表（翻档旧目标定位）。仅候选时填充。
                a_target_sites.resize(slots.size());
                l1_src_of.resize(slots.size());
                c_src_of.resize(slots.size());
                for (std::size_t i = 0; i < slots.size(); ++i)
                {
                    l1_src_of[i] = alias_src_of(slots[i]);
                    if (l1_src_of[i].has_value())
                    {
                        l1_deps[*l1_src_of[i]].push_back(
                            static_cast<std::uint32_t>(i));
                    }
                    if (!slots[i].related_key.empty())
                    {
                        if (const auto src = find_slot(
                                related_owner_key(slots[i].related_key),
                                std::string{return_slot});
                            src.has_value())
                        {
                            c_src_of[i] = src;
                            c_consumers[*src].push_back(
                                static_cast<std::uint32_t>(i));
                        }
                    }
                    if (incr_candidate)
                    {
                        slots_of_owner[slots[i].owner_key].push_back(
                            static_cast<std::uint32_t>(i));
                    }
                }
                // 行级多源边解析（收集点见装载循环）：写回行/壳字段写行的每条
                // related_var 独立解析成 (dst, src) 边，排序去重后登记进
                // row_alias_of（dst 视角的源列表）与 l1_deps（src
                // 视角的依赖扇出， fan_out 直接复用）。并集语义经函数内阶段的
                // add_types 天然成立； 置信度取低者也是 add_types 既有语义（新
                // conf 低于现值才改写标签
                // ——宁降档不吞值）。槽级单值边（alias_src_of，装载序第一条）与
                // 行级边重复时 add_types 幂等去重。解析失败（源槽不存在——RHS 无
                // 证据行是常态）不建边，但记 row_alias_dead 供增量种子判定（见
                // 行级死边的删失传播见证）。
                row_alias_of.resize(slots.size());
                row_alias_dead_rel.resize(slots.size());
                {
                    std::vector<std::pair<std::size_t, std::size_t>> edges;
                    for (const auto& [dst, rel] : pending_row_alias)
                    {
                        if (const auto src =
                                resolve_alias_slot(slots[dst].owner_key, rel);
                            src.has_value())
                        {
                            edges.emplace_back(dst, *src);
                        }
                        else if (row_alias_dead_rel[dst].empty())
                        {
                            row_alias_dead_rel[dst] = rel;
                        }
                    }
                    std::ranges::sort(edges);
                    edges.erase(
                        std::unique(edges.begin(), edges.end()),
                        edges.end());
                    for (const auto& [dst, src] : edges)
                    {
                        row_alias_of[dst].push_back(
                            static_cast<std::uint32_t>(src));
                        l1_deps[src].push_back(static_cast<std::uint32_t>(dst));
                    }
                }
                pending_row_alias.clear();
                pending_row_alias.shrink_to_fit();
                for (std::size_t j = 0; j < sites.size(); ++j)
                {
                    const PropagatedSite& site = sites[j];
                    if (!site.callee_id.has_value())
                    {
                        continue;
                    }
                    const std::int64_t callee_canon =
                        canon_of_id.at(*site.callee_id);
                    for (std::size_t i = 0; i < site.arg_types.size(); ++i)
                    {
                        ArgFlow f;
                        f.arg = static_cast<std::uint32_t>(i);
                        // 形参槽查找走整数键：序号 -> "#p{n}" 域 id（miss
                        // 即无该形参 槽），宿主用被调用者规范
                        // id——判据与键串拼接形态等价。
                        const auto pv = param_var_of.find(static_cast<int>(i));
                        const auto target =
                            pv != param_var_of.end()
                                ? param_of.find(
                                      SlotKey{callee_canon, pv->second})
                                : param_of.end();
                        if (target != param_of.end()
                            && !lookup.is_pure_virtual_symbol(callee_canon))
                        {
                            f.target = target->second;
                            if (incr_candidate)
                            {
                                a_target_sites[f.target].push_back(
                                    static_cast<std::uint32_t>(j));
                            }
                        }
                        else if (site.is_virtual && pv != param_var_of.end())
                        {
                            // 入参虚族扇投：被调解析到接口/基类声明（纯虚
                            // 声明无函数体——其形参数据行照常发射但槽是传
                            // 播死胡同，直连判定视同失败；非纯虚的无体声明
                            // 本就无槽）时，实参的类型证据沿 overrides 边
                            // 扇投进族内每个有体 override 的同位置形参槽
                            // ——may 语义超集（该 override 未必被此调用点
                            // 派发到），档位与静态面裁剪交投递点，绝不提
                            // 档。直连命中的站点不扇投：接收者已可唯一定
                            // 位时并集只稀释决议唯一性。非虚目标（被调唯一
                            // 无需族）与全库无 #p{i} 槽（pv miss）不开阀；
                            // 被调键串未解析的站点整站跳过（无声明锚点，
                            // 族无从取）。
                            for (const std::uint32_t dst :
                                 *virtual_family_body_param_slots(
                                     callee_canon,
                                     pv->second))
                            {
                                f.family_targets.push_back(dst);
                                ++family_pf_edges;
                                if (incr_candidate)
                                {
                                    a_target_sites[dst].push_back(
                                        static_cast<std::uint32_t>(j));
                                }
                            }
                        }
                        if (const auto s = arg_slot_of(site, i); s.has_value())
                        {
                            f.arg_slot = *s;
                        }
                        // 实参是项目内调用（set(make())）：走被调用者的返回槽，
                        // 绝不把声明返回类型当动态证据。
                        if (i < site.arg_call_keys.size()
                            && !site.arg_call_keys[i].empty())
                        {
                            const std::string owner =
                                related_owner_key(site.arg_call_keys[i]);
                            if (const auto s =
                                    find_slot(owner, std::string{return_slot});
                                s.has_value())
                            {
                                f.ret_slot = *s;
                            }
                        }
                        if (f.arg_slot != NPOS)
                        {
                            a_src_sites[f.arg_slot].push_back(
                                static_cast<std::uint32_t>(j));
                        }
                        if (f.ret_slot != NPOS)
                        {
                            a_src_sites[f.ret_slot].push_back(
                                static_cast<std::uint32_t>(j));
                        }
                        site_args[j].push_back(f);
                    }
                    // 出参写回边（取址实参，修正版触发）：直连触发 = callee
                    // #p{i} 槽存在 OutParamFlow 行（红线：行存在性是唯一
                    // 库内/外门槛，库外实现天然无行）且实参变量/字段域可解析
                    // （&x / 裸字段 / X.Put() 取址槽三形态由 collect 记入
                    // arg_vars）。虚调用的直连槽常解析到接口/基类声明——声明
                    // 无函数体天然无行，此时回退到虚族并集：沿 overrides 边
                    // 取实现族全部有体成员的同位置槽为源槽并集回传（成员自身
                    // 写回行不是门槛——转发出参形态的真实现靠槽终态贡献真类
                    // 型；族成员只取自己的 #p{i} 槽，绝不顺调用链跨函数取
                    // 值）；族并集是 may 语义超集，多类型交决议端既有
                    // family/nearest 护栏消费，决不在此提档。直连成功的站点
                    // 不叠加族边（接收者已可直连定位时并集只稀释决议唯一
                    // 性）。
                    for (std::size_t i = 0; i < site.arg_vars.size(); ++i)
                    {
                        const auto dst = arg_slot_of(site, i);
                        if (!dst.has_value())
                        {
                            continue;
                        }
                        const auto pv = param_var_of.find(static_cast<int>(i));
                        const auto target =
                            pv != param_var_of.end()
                                ? param_of.find(
                                      SlotKey{callee_canon, pv->second})
                                : param_of.end();
                        if (target != param_of.end()
                            && slots[target->second].has_outparam_flow)
                        {
                            site_wb[j].push_back(
                                WbEdge{
                                    static_cast<std::uint32_t>(target->second),
                                    static_cast<std::uint32_t>(*dst),
                                    false});
                            b_param_sites[target->second].push_back(
                                static_cast<std::uint32_t>(j));
                            continue;
                        }
                        if (!site.is_virtual || pv == param_var_of.end())
                        {
                            continue; // 非虚目标唯一无需族；全库无 #p{i} 槽
                        }
                        for (const std::uint32_t src :
                             *virtual_family_param_slots(
                                 callee_canon,
                                 pv->second))
                        {
                            site_wb[j].push_back(
                                WbEdge{
                                    src,
                                    static_cast<std::uint32_t>(*dst),
                                    true});
                            b_param_sites[src].push_back(
                                static_cast<std::uint32_t>(j));
                            ++family_wb_edges;
                        }
                    }
                }
            }

            /**
             * @brief 别名引用（related_var）到源槽的解析。
             *
             * @internal @elem/@fld/@gvar/@fnv 前缀按编码携带的域，裸名按
             *           owner 域；槽级别名（单值）与行级多源边共用。
             */
            [[nodiscard]]
            std::optional<std::size_t> resolve_alias_slot(
                const std::string& owner,
                const std::string& rel)
            {
                if (rel.empty())
                {
                    return std::nullopt;
                }
                if (rel.starts_with(container_elem_slot_prefix))
                {
                    const auto elem = decode_container_elem_slot(rel);
                    if (elem.has_value())
                    {
                        return find_slot(
                            elem->first.empty() ? owner : elem->first,
                            elem->second);
                    }
                }
                else if (rel.starts_with(field_slot_prefix))
                {
                    const auto field = decode_field_slot(rel);
                    if (field.has_value())
                    {
                        return find_slot(field->first, field->second);
                    }
                }
                else if (rel.starts_with(global_slot_prefix))
                {
                    // 全局变量槽引用（@gvar|<variable||name|file>|`<名>`）
                    // ——按最后 '|' 切分，边界语义与前缀槽解码单点统一。
                    const auto global = decode_global_slot(rel);
                    if (global.has_value())
                    {
                        return find_slot(global->first, global->second);
                    }
                }
                else if (rel.starts_with(fn_var_slot_prefix))
                {
                    // 函数域变量槽引用（@fnv|<fn unique_key>|`<名>`）——壳
                    // 字段写侧穿透的跨域别名源（写函数的形参/局部），同款经
                    // 前缀槽解码单点。
                    const auto fn_var = decode_fn_var_slot(rel);
                    if (fn_var.has_value())
                    {
                        return find_slot(fn_var->first, fn_var->second);
                    }
                }
                else
                {
                    return find_slot(owner, rel);
                }
                return std::nullopt;
            }

            /** @brief 槽级别名源槽（related_var 单值形态）。 */
            [[nodiscard]]
            std::optional<std::size_t> alias_src_of(const HintSlot& slot)
            {
                return resolve_alias_slot(slot.owner_key, slot.related_var);
            }

            /**
             * @brief 增量路径：种子构造 + 脏闭包 + warm 回填。
             *
             * @internal 种子四类（脏文件槽/脏站点与跨文件扇入落点/被删行
             *           下游/旧边 src 键漂移见证）；闭包 = 种子沿新图前向可
             *           达，闭包槽复位重算、闭包外槽沿用 warm 终态；预算超
             *           限只置 degraded，下方初始化退回全量形态。
             */
            void build_incremental_closure()
            {
                // ---- 增量路径：种子构造 + 脏闭包 + warm
                // 回填。种子四类：脏文件槽（行/owner 在
                // D）、脏站点与跨文件扇
                // 入站点的实参槽/形参落点、被删行下游（capture
                // 的旧槽键/旧被调用者 键/doomed 键 + 持久化 src
                // 键漂移见证）。闭包 = 种子沿新图前向
                // 可达（槽→别名/消费/站点源/喂料站点，站点→形参目标/写回目标）；
                // 闭包槽复位到初始值重算（删失只能靠复位传播），闭包外槽沿用
                // warm 终态。预算超限只置
                // degraded——下方初始化退回全量形态。----
                slot_in_closure.assign(slots.size(), 0);
                site_due.assign(sites.size(), 0);
                if (incr_candidate && !plan->degraded)
                {
                    std::unordered_set<std::string_view> prev_callee_set;
                    if (plan->prev_callee_keys != nullptr)
                    {
                        prev_callee_set.insert(
                            plan->prev_callee_keys->begin(),
                            plan->prev_callee_keys->end());
                    }
                    // 旧槽键集合走整数键：owner/var
                    // 解析失败的旧键直接丢弃——失败
                    // 意味着该宿主/变量已无任何存活槽，contains 对任何槽键恒
                    // false （与字符串形态"拼出来也永不等于任何槽键"同义）。
                    std::unordered_set<SlotKey, SlotKeyHash> prev_slot_set;
                    if (plan->prev_slots != nullptr)
                    {
                        for (const auto& [owner, var] : *plan->prev_slots)
                        {
                            const auto oid = canon_of_key(owner);
                            const auto vit = var_of.find(var);
                            if (oid.has_value() && vit != var_of.end())
                            {
                                prev_slot_set.insert(
                                    SlotKey{*oid, vit->second});
                            }
                        }
                    }
                    const auto slot_key_of = [](const HintSlot& slot)
                    { return hint_slot_key(slot.owner_key, slot.var_name); };
                    // 槽键的整数形态（与 slot_key_of 同一键判定的整数版；
                    // 集合命中判定用它，warm 表查找仍用字符串键——warm
                    // 行是持久化态）。
                    const auto slot_key_id = [](const HintSlot& slot)
                    { return SlotKey{slot.owner_id, slot.var_id}; };
                    const auto seed_slot = [&](std::size_t i)
                    { slot_in_closure[i] = 1; };
                    std::size_t n_r1 = 0, n_r4 = 0, n_r5 = 0;
                    std::unordered_set<std::string_view> doomed_set;
                    if (plan->doomed_keys != nullptr)
                    {
                        doomed_set.insert(
                            plan->doomed_keys->begin(),
                            plan->doomed_keys->end());
                    }
                    std::unordered_set<std::string_view> doomed_name_set;
                    if (plan->doomed_names != nullptr)
                    {
                        doomed_name_set.insert(
                            plan->doomed_names->begin(),
                            plan->doomed_names->end());
                    }
                    // 保守集：源槽/源符号的消失有 D
                    // 根源（被删符号键或名、被删行
                    // 捕获）——历史投递可能只送了重复类型却降过置信度/改写过原因，
                    // 不能用"类型集相等"证明无害，必须种子。
                    const auto doomed_pair =
                        [&](std::string_view owner, std::string_view var)
                    {
                        if (doomed_set.contains(owner)
                            || prev_callee_set.contains(owner))
                        {
                            return true;
                        }
                        const auto oid = canon_of_key(owner);
                        const auto vit = var_of.find(var);
                        return oid.has_value() && vit != var_of.end()
                               && prev_slot_set.contains(
                                   SlotKey{*oid, vit->second});
                    };
                    // 槽种子 + 覆盖检查（非脏槽必须有 warm 行，否则不可重建）。
                    for (std::size_t i = 0; i < slots.size() && !plan->degraded;
                         ++i)
                    {
                        const HintSlot&   slot = slots[i];
                        const std::string key = slot_key_of(slot);
                        bool              seeded = false;
                        // 行在脏文件，或 owner
                        // 符号在脏文件（重写即输入变化）。
                        if (slot_row_dirty[i])
                        {
                            seeded = true;
                            ++n_r1;
                        }
                        else if (
                            const auto oid = lookup.by_key(slot.owner_key);
                            oid.has_value())
                        {
                            if (const auto of = lookup.file_of(*oid);
                                of.has_value() && dirty_fids.contains(*of))
                            {
                                seeded = true;
                                ++n_r1;
                            }
                        }
                        // 被删行下游（捕获的旧槽键 / 旧被调用者的全部槽）。
                        if (!seeded
                            && (prev_slot_set.contains(slot_key_id(slot))
                                || prev_callee_set.contains(
                                    std::string_view{slot.owner_key})))
                        {
                            seeded = true;
                            ++n_r4;
                        }
                        // 旧边见证。死边（别名/返回源槽消失）只看 D 根源——
                        // 源符号/源行干净（槽本就不存在：外部函数、值返回函数无
                        // #ret 槽、从未有证据的字段）=
                        // 两次运行同样死，从未投递， 出局；源在
                        // D（被删符号键/名、被删行捕获、脏文件重写后
                        // 槽可能消失）=
                        // 边曾可能投递过类型或降过置信度，保守种子。 src
                        // 键漂移（含无源变有源）无法便宜证明无害，恒为种子。
                        if (!seeded)
                        {
                            // 死边判定用
                            // rel：槽级别名（单值）优先，否则行级死边
                            // （写回/壳字段写行的 related_var
                            // 解析失败——多源口径
                            // 下槽级可能活着而某条行级边已死，删失传播按那条
                            // rel 判 D 根源）。
                            const std::string& alias_rel =
                                !slot.related_var.empty()
                                    ? slot.related_var
                                    : row_alias_dead_rel[i];
                            const bool dead_alias =
                                (!slot.related_var.empty()
                                 && !l1_src_of[i].has_value())
                                || (slot.related_var.empty()
                                    && !row_alias_dead_rel[i].empty());
                            const bool dead_ret = !slot.related_key.empty()
                                                  && !c_src_of[i].has_value();
                            if (dead_alias || dead_ret)
                            {
                                bool d_rooted = false;
                                if (dead_ret)
                                {
                                    if (slot.related_key.starts_with("@fn|"))
                                    {
                                        const MethodRef ref =
                                            decode_method_ref(slot.related_key);
                                        d_rooted = doomed_name_set.contains(
                                            std::string_view{ref.name});
                                        if (!d_rooted)
                                        {
                                            Confidence conf =
                                                Confidence::Fallback;
                                            if (const auto id =
                                                    lookup.resolve(ref, conf);
                                                id.has_value())
                                            {
                                                if (const auto ff =
                                                        lookup.file_of(*id);
                                                    ff.has_value()
                                                    && dirty_fids.contains(*ff))
                                                {
                                                    d_rooted = true;
                                                }
                                            }
                                        }
                                    }
                                    else
                                    {
                                        d_rooted = doomed_set.contains(
                                                       std::string_view{
                                                           slot.related_key})
                                                   || prev_callee_set.contains(
                                                       std::string_view{
                                                           slot.related_key});
                                        if (!d_rooted)
                                        {
                                            if (const auto id = lookup.by_key(
                                                    slot.related_key);
                                                id.has_value())
                                            {
                                                if (const auto ff =
                                                        lookup.file_of(*id);
                                                    ff.has_value()
                                                    && dirty_fids.contains(*ff))
                                                {
                                                    d_rooted = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                else if (
                                    alias_rel.starts_with(field_slot_prefix))
                                {
                                    const auto fld =
                                        decode_field_slot(alias_rel);
                                    if (fld.has_value())
                                    {
                                        d_rooted = doomed_pair(
                                            fld->first,
                                            fld->second);
                                        if (!d_rooted)
                                        {
                                            if (const auto id =
                                                    lookup.by_key(fld->first);
                                                id.has_value())
                                            {
                                                if (const auto ff =
                                                        lookup.file_of(*id);
                                                    ff.has_value()
                                                    && dirty_fids.contains(*ff))
                                                {
                                                    d_rooted = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                else if (
                                    alias_rel.starts_with(
                                        container_elem_slot_prefix))
                                {
                                    const auto elem =
                                        decode_container_elem_slot(alias_rel);
                                    if (elem.has_value())
                                    {
                                        d_rooted = doomed_pair(
                                            elem->first.empty()
                                                ? std::
                                                      string_view{slot.owner_key}
                                                : std::string_view{elem->first},
                                            elem->second);
                                    }
                                }
                                else if (
                                    alias_rel.starts_with(fn_var_slot_prefix))
                                {
                                    // @fnv 别名源的 D
                                    // 根源（写函数被删/其行被
                                    // 捕获/写函数在脏文件）——与 @fld
                                    // 分支同构，解码
                                    // 经前缀槽单点（边界语义统一）。
                                    if (const auto fn_var =
                                            decode_fn_var_slot(alias_rel);
                                        fn_var.has_value())
                                    {
                                        const std::string_view fkey =
                                            fn_var->first;
                                        d_rooted =
                                            doomed_set.contains(fkey)
                                            || prev_callee_set.contains(fkey)
                                            || doomed_pair(
                                                fkey,
                                                fn_var->second);
                                        if (!d_rooted)
                                        {
                                            if (const auto id =
                                                    lookup.by_key(fkey);
                                                id.has_value())
                                            {
                                                if (const auto ff =
                                                        lookup.file_of(*id);
                                                    ff.has_value()
                                                    && dirty_fids.contains(*ff))
                                                {
                                                    d_rooted = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                if (d_rooted)
                                {
                                    seeded = true;
                                    ++n_r5;
                                }
                            }
                        }
                        if (!seeded)
                        {
                            const auto wit = warm->find(key);
                            if (wit == warm->end())
                            {
                                if (!slot_row_dirty[i])
                                {
                                    plan->degraded = true; // 覆盖缺口：整体降级
                                    plan->degrade_reason =
                                        "warm-coverage:" + key;
                                }
                                seeded = true; // 新槽（行必在 D）：按种子重算
                            }
                            else
                            {
                                const std::string cur_alias =
                                    l1_src_of[i].has_value()
                                        ? slot_key_of(slots[*l1_src_of[i]])
                                        : std::string{};
                                const std::string cur_ret =
                                    c_src_of[i].has_value()
                                        ? slot_key_of(slots[*c_src_of[i]])
                                        : std::string{};
                                if (wit->second.alias_src != cur_alias
                                    || wit->second.ret_src != cur_ret)
                                {
                                    seeded = true;
                                    ++n_r5;
                                }
                            }
                        }
                        // 层级变化：out_param_this 的派生闭包在装载期按 extends
                        // 展开。
                        if (!seeded && plan->hierarchy_changed
                            && slot_derived_seed[i])
                        {
                            seeded = true;
                        }
                        if (seeded)
                        {
                            seed_slot(i);
                        }
                    }
                    // 站点分类（输入变化 = 类 A，闭包内传播；仅复位槽的再供料 =
                    // 类 B，due
                    // 但不传播——馈料站点的其余目标输入未变，处理是可证 明
                    // no-op，不并入闭包，否则热点形参槽的反向可达把闭包炸成整
                    // 个调用图）：
                    // - 类 A：行在脏文件 / 解析翻转（本次解析键 ≠
                    // 行内旧键文本——
                    //   callee_key 无 FK，被调用者删除也存活，跨文件扇入
                    //   形态同样覆盖） / 实参源漂移（@fld 字段槽或 @fn
                    //   返回槽解析失败 = 旧边消失）。
                    std::vector<char> site_propagates(sites.size(), 0);
                    std::vector<char> site_flip(sites.size(), 0);
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        if (site_row_dirty[j])
                        {
                            site_due[j] = 1;
                            site_propagates[j] = 1;
                            continue;
                        }
                        const PropagatedSite& site = sites[j];
                        if (!site_row_callee_key[j].empty()
                            && site.callee_key != site_row_callee_key[j])
                        {
                            site_flip[j] = 1; // 解析翻转：新旧目标都要进闭包
                        }
                        if (site_flip[j])
                        {
                            site_due[j] = 1;
                            site_propagates[j] = 1;
                        }
                        for (const ArgFlow& f : site_args[j])
                        {
                            if (f.arg < site.arg_vars.size()
                                && site.arg_vars[f.arg].starts_with(
                                    field_slot_prefix)
                                && f.arg_slot == NPOS)
                            {
                                // @fld 实参：字段槽消失须有 D
                                // 根源（被删记录/被删行）
                                // 才是漂移；从未有过槽的字段（值语义，无证据行）是
                                // 常态，不是漂移。
                                const auto fld =
                                    decode_field_slot(site.arg_vars[f.arg]);
                                if (fld.has_value()
                                    && doomed_pair(fld->first, fld->second))
                                {
                                    site_due[j] = 1;
                                    site_propagates[j] = 1;
                                    break;
                                }
                            }
                            if (f.arg < site.arg_call_keys.size()
                                && !site.arg_call_keys[f.arg].empty()
                                && f.ret_slot == NPOS)
                            {
                                // @fn
                                // 实参：被调用函数返回槽缺失分两类——值返回函数
                                // 从无 #ret 槽（常态，非漂移）与曾有后消失（D
                                // 根源： 被删函数名命中 / 其 #ret 行被捕获 /
                                // 函数在脏文件）。
                                const MethodRef ref = decode_method_ref(
                                    site.arg_call_keys[f.arg]);
                                bool vanished = doomed_name_set.contains(
                                    std::string_view{ref.name});
                                if (!vanished)
                                {
                                    Confidence conf = Confidence::Fallback;
                                    if (const auto id =
                                            lookup.resolve(ref, conf);
                                        id.has_value())
                                    {
                                        // "#ret 曾存在"判定走整数键（宿主规范
                                        // id × 返回槽域
                                        // id），与键串拼接形态同义。
                                        const auto cit = canon_of_id.find(*id);
                                        vanished = cit != canon_of_id.end()
                                                   && prev_slot_set.contains(
                                                       SlotKey{
                                                           cit->second,
                                                           ret_var_id});
                                        if (!vanished)
                                        {
                                            if (const auto ff =
                                                    lookup.file_of(*id);
                                                ff.has_value()
                                                && dirty_fids.contains(*ff))
                                            {
                                                vanished = true;
                                            }
                                        }
                                    }
                                }
                                if (vanished)
                                {
                                    site_due[j] = 1;
                                    site_propagates[j] = 1;
                                    break;
                                }
                            }
                        }
                    }
                    // 站点驱动的槽种子：类 A
                    // 站点的实参槽与形参落点、写回目标、翻
                    // 转旧被调用者（仍存活）的全部槽。
                    for (std::size_t j = 0; j < sites.size() && !plan->degraded;
                         ++j)
                    {
                        if (!site_due[j] || !site_propagates[j])
                        {
                            continue;
                        }
                        for (const ArgFlow& f : site_args[j])
                        {
                            if (f.target != NPOS)
                            {
                                seed_slot(f.target);
                            }
                            for (const std::uint32_t dst : f.family_targets)
                            {
                                seed_slot(dst);
                            }
                            if (f.arg_slot != NPOS)
                            {
                                seed_slot(f.arg_slot);
                            }
                        }
                        for (const WbEdge& wb : site_wb[j])
                        {
                            seed_slot(wb.dst);
                        }
                        if (site_flip[j] && !site_row_callee_key[j].empty())
                        {
                            if (const auto it =
                                    slots_of_owner.find(site_row_callee_key[j]);
                                it != slots_of_owner.end())
                            {
                                for (const std::uint32_t i : it->second)
                                {
                                    seed_slot(i);
                                }
                            }
                        }
                    }
                    // 闭包
                    // BFS（槽/站点交替出队；馈料站点不展开），预算超限降级全量。
                    if (!plan->degraded)
                    {
                        // bfs 条目：(槽, 下标) / (站点, 下标)；site_propagates
                        // 区分 类 A（展开目标）与类 B（只处理，不展开）。
                        std::vector<std::pair<bool, std::size_t>> bfs;
                        std::size_t                               visited = 0;
                        for (std::size_t i = 0; i < slots.size(); ++i)
                        {
                            if (slot_in_closure[i])
                            {
                                bfs.emplace_back(false, i);
                                ++visited;
                            }
                        }
                        for (std::size_t j = 0; j < sites.size(); ++j)
                        {
                            if (site_due[j])
                            {
                                bfs.emplace_back(true, j);
                                ++visited;
                            }
                        }
                        std::size_t qi = 0;
                        while (qi < bfs.size())
                        {
                            if (visited > plan->closure_budget)
                            {
                                plan->degraded = true;
                                std::size_t seed_slots = 0;
                                std::size_t due_a = 0;
                                for (std::size_t i = 0; i < slots.size(); ++i)
                                {
                                    seed_slots += slot_in_closure[i];
                                }
                                for (std::size_t k = 0; k < sites.size(); ++k)
                                {
                                    due_a += site_due[k] && site_propagates[k];
                                }
                                plan->degrade_reason = std::format(
                                    "closure-budget visited={} seeds={} classA={} "
                                    "r1={} r4={} r5={}",
                                    visited,
                                    seed_slots,
                                    due_a,
                                    n_r1,
                                    n_r4,
                                    n_r5);
                                break;
                            }
                            const auto [is_site, idx] = bfs[qi++];
                            if (is_site)
                            {
                                if (!site_propagates[idx])
                                {
                                    continue; // 类 B 馈料站点：再供料，不传播
                                }
                                for (const ArgFlow& f : site_args[idx])
                                {
                                    if (f.target != NPOS
                                        && !slot_in_closure[f.target])
                                    {
                                        slot_in_closure[f.target] = 1;
                                        ++visited;
                                        bfs.emplace_back(false, f.target);
                                    }
                                    for (const std::uint32_t dst :
                                         f.family_targets)
                                    {
                                        if (!slot_in_closure[dst])
                                        {
                                            slot_in_closure[dst] = 1;
                                            ++visited;
                                            bfs.emplace_back(false, dst);
                                        }
                                    }
                                }
                                for (const WbEdge& wb : site_wb[idx])
                                {
                                    if (!slot_in_closure[wb.dst])
                                    {
                                        slot_in_closure[wb.dst] = 1;
                                        ++visited;
                                        bfs.emplace_back(false, wb.dst);
                                    }
                                }
                            }
                            else
                            {
                                const auto visit_slot = [&](std::uint32_t d)
                                {
                                    if (!slot_in_closure[d])
                                    {
                                        slot_in_closure[d] = 1;
                                        ++visited;
                                        bfs.emplace_back(false, d);
                                    }
                                };
                                for (const std::uint32_t d : l1_deps[idx])
                                {
                                    visit_slot(d);
                                }
                                for (const std::uint32_t d : c_consumers[idx])
                                {
                                    visit_slot(d);
                                }
                                const auto visit_site =
                                    [&](std::uint32_t j, bool propagates)
                                {
                                    if (!site_due[j])
                                    {
                                        site_due[j] = 1;
                                        if (propagates)
                                        {
                                            site_propagates[j] = 1;
                                        }
                                        ++visited;
                                        bfs.emplace_back(true, j);
                                    }
                                    else if (propagates && !site_propagates[j])
                                    {
                                        site_propagates[j] = 1;
                                        // 升级为类 A：补入队以展开其目标。
                                        bfs.emplace_back(true, j);
                                    }
                                };
                                for (const std::uint32_t j : a_src_sites[idx])
                                {
                                    visit_site(j, true);
                                }
                                for (const std::uint32_t j : b_param_sites[idx])
                                {
                                    visit_site(j, true);
                                }
                                for (const std::uint32_t j :
                                     a_target_sites[idx])
                                {
                                    visit_site(j, false);
                                }
                            }
                        }
                        if (!plan->degraded)
                        {
                            plan->closure = visited;
                            // 回填分两遍：先校验后改写——任何降级决不发生在已改写
                            // 之后（全量模式绝不能带着 warm 残值跑：add_types
                            // 只增，
                            // 高于初始值的起点会固定在错误上界）。种子循环已对未
                            // 种子槽做过同一校验，这里按闭包终态复核。
                            bool covered = true;
                            for (std::size_t i = 0; i < slots.size(); ++i)
                            {
                                if (!slot_in_closure[i]
                                    && !warm->contains(slot_key_of(slots[i])))
                                {
                                    covered = false;
                                    break;
                                }
                            }
                            if (!covered)
                            {
                                plan->degraded = true;
                                plan->degrade_reason = "apply-coverage";
                            }
                            else
                            {
                                for (std::size_t i = 0; i < slots.size(); ++i)
                                {
                                    if (slot_in_closure[i])
                                    {
                                        continue;
                                    }
                                    const auto wit =
                                        warm->find(slot_key_of(slots[i]));
                                    if (wit == warm->end())
                                    {
                                        continue; // 校验已排除；防御性保持初始值
                                    }
                                    HintSlot& slot = slots[i];
                                    if (wit->second.type_seg.has_value())
                                    {
                                        // 快照形态：终态区段已在
                                        // arena（预拼接段）， 直接平移——与 JSON
                                        // 解析 + intern + append 恒等（同域
                                        // id、id 序 == 字符串序，区段相
                                        // 等当且仅当物化串集相等）。
                                        slot.type_off =
                                            wit->second.type_seg->first;
                                        slot.type_len =
                                            wit->second.type_seg->second;
                                    }
                                    else
                                    {
                                        const std::vector<std::int32_t>
                                            warm_ids =
                                                intern_ids(parse_string_array(
                                                    wit->second.type_set));
                                        const auto [woff, wlen] =
                                            arena.append_dedup(
                                                std::span<const std::int32_t>{
                                                    warm_ids.data(),
                                                    warm_ids.size()});
                                        slot.type_off = woff;
                                        slot.type_len = wlen;
                                    }
                                    slot.confidence =
                                        static_cast<Confidence>(std::clamp(
                                            wit->second.confidence,
                                            0,
                                            3));
                                    slot.source = source_of(wit->second.source);
                                    slot.reason = wit->second.reason;
                                    slot.truncated_here = wit->second.truncated;
                                }
                            }
                        }
                    }
                }
            }

            /**
             * @brief semi-naive 脏队列不动点（Datalog semi-naive 求值同型）。
             *
             * @internal 四阶段（函数内别名 / 实参→形参 / 出参写回 /
             *           返回值→调用点）保持全扫版的轮内阶段顺序与扫描
             *           顺序，只跳过“自上次处理后输入无变化”的项；同轮同
             *           阶段的就地可见性用位置规则复刻。轮间做分代压缩。
             */
            void run_fixed_point()
            {
                // ---- semi-naive 脏队列（Datalog semi-naive 求值同型）：四阶段
                // （函数内别名 / 实参→形参 / 出参写回 /
                // 返回值→调用点）
                // 保持全扫版的轮内阶段顺序与扫描顺序，只跳过"自上次处理后输入
                // 无变化"的项——被跳过的访问是可证明的 no-op（批内容未变时
                // add_types 必 false、置信度只降不升）。同轮同阶段的就地可见性
                // 用位置规则复刻：变更扇出落在当前扫描位之后的项本轮处理、之前
                // 的下一轮处理，与全扫版逐槽逐站点的串行语义等价。 ----
                constexpr int          MAX_TYPES = 32;
                constexpr int          MAX_ROUNDS = 256;
                constexpr std::int64_t MAX_WORK = 2'000'000;
                std::int64_t           work = 0;
                auto t_return_flow_0 = std::chrono::steady_clock::now();
                /**
                 * @brief 传播四阶段的队列位面：实参→形参（arg_param）、
                 *         入参反传族并集扇投（param_fanout）、写回
                 *         （writeback）、返回值→调用点（return_flow）。
                 */
                enum class Phase : std::uint8_t
                {
                    kArgParam,
                    kParamFanout,
                    kWriteback,
                    kReturnFlow
                };
                std::vector<char> arg_param_due(slots.size(), 0),
                    arg_param_next(slots.size(), 0);
                std::vector<char> param_fanout_due(sites.size(), 0),
                    param_fanout_next(sites.size(), 0);
                std::vector<char> writeback_due(sites.size(), 0),
                    writeback_next(sites.size(), 0);
                std::vector<char> return_flow_due(slots.size(), 0),
                    return_flow_next(slots.size(), 0);
                // 增量成立时 due 集 = 闭包（槽）∪
                // 直配站点；否则全量形态（有源槽/
                // 已解析站点全部入队）——被跳过的访问是可证明 no-op 的既有论证在
                // warm 回填后同样成立（闭包外槽的输入与其旧终态一致）。
                incr_applied =
                    incr_candidate && plan != nullptr && !plan->degraded;
                if (incr_applied)
                {
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        if (slot_in_closure[i])
                        {
                            if (l1_src_of[i].has_value()
                                || !row_alias_of[i].empty())
                            {
                                arg_param_due[i] = 1;
                            }
                            if (c_src_of[i].has_value())
                            {
                                return_flow_due[i] = 1;
                            }
                        }
                    }
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        if (site_due[j] && sites[j].callee_id.has_value())
                        {
                            param_fanout_due[j] = 1;
                            if (!sites[j].arg_vars.empty())
                            {
                                writeback_due[j] = 1;
                            }
                        }
                    }
                }
                else
                {
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        if (l1_src_of[i].has_value()
                            || !row_alias_of[i].empty())
                        {
                            arg_param_due[i] = 1;
                        }
                        if (c_src_of[i].has_value())
                        {
                            return_flow_due[i] = 1;
                        }
                    }
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        if (sites[j].callee_id.has_value())
                        {
                            param_fanout_due[j] = 1;
                            if (!sites[j].arg_vars.empty())
                            {
                                writeback_due[j] = 1;
                            }
                        }
                    }
                }
                out.incremental = incr_applied;
                // 槽 s 变更（新增类型 / 置信度下探 /
                // 截断标记首置）时的下游扇出； cur/pos
                // 描述当前执行的阶段与扫描位（同阶段同轮的位置规则）。
                const auto fan_out =
                    [&](std::size_t s, Phase cur, std::size_t pos)
                {
                    for (const std::uint32_t d : l1_deps[s])
                    {
                        (cur == Phase::kArgParam && d > pos
                             ? arg_param_due[d]
                             : arg_param_next[d]) = 1;
                    }
                    for (const std::uint32_t j : a_src_sites[s])
                    {
                        (cur == Phase::kParamFanout && j > pos
                             ? param_fanout_due[j]
                             : param_fanout_next[j]) = 1;
                    }
                    for (const std::uint32_t j : b_param_sites[s])
                    {
                        (cur == Phase::kWriteback && j > pos
                             ? writeback_due[j]
                             : writeback_next[j]) = 1;
                    }
                    for (const std::uint32_t d : c_consumers[s])
                    {
                        (cur == Phase::kReturnFlow && d > pos
                             ? return_flow_due[d]
                             : return_flow_next[d]) = 1;
                    }
                };
                for (int round = 0; round < MAX_ROUNDS; ++round)
                {
                    ++out.rounds;
                    bool changed = false;
                    // 上一轮返回值→调用点阶段尾段（含循环退出判定前的收尾扫描）
                    // 在此累计。
                    out.return_flow_us +=
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t_return_flow_0)
                            .count();
                    const auto t_arg_param_0 = std::chrono::steady_clock::now();
                    // 函数内阶段：同函数别名/拷贝。
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        arg_param_due[i] |= arg_param_next[i];
                        arg_param_next[i] = 0;
                    }
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        if (!arg_param_due[i])
                        {
                            continue;
                        }
                        arg_param_due[i] = 0;
                        HintSlot& slot = slots[i];
                        // 源列表：槽级别名（单值）+
                        // 行级多源边（写回/壳字段写行，
                        // 并集口径）。逐源拷贝并集；截断、taint 传染对每个源
                        // 同款处理；任一源带来新类型或首置截断标记即扇出。
                        std::vector<std::size_t> sources;
                        if (l1_src_of[i].has_value())
                        {
                            sources.push_back(*l1_src_of[i]);
                        }
                        for (const std::uint32_t s : row_alias_of[i])
                        {
                            sources.push_back(s);
                        }
                        bool       touched = false;
                        const bool taint_before = slot.truncated_here;
                        for (const std::size_t src : sources)
                        {
                            std::span<const std::int32_t> types = arena.view(
                                slots[src].type_off,
                                slots[src].type_len);
                            std::vector<std::int32_t> cap;
                            if (types.size()
                                > static_cast<std::size_t>(MAX_TYPES))
                            {
                                cap.assign(
                                    types.begin(),
                                    types.begin() + MAX_TYPES);
                                types = cap;
                                out.budget_exhausted = true;
                                slot.truncated_here = true;
                            }
                            // taint 传染：源槽不完整则拷贝结果同样不完整（#ret
                            // 经 此路径从被 return 的局部变量槽继承标记）。
                            slot.truncated_here |= slots[src].truncated_here;
                            const bool did = add_types(
                                slot,
                                types,
                                Confidence::Medium,
                                HintSource::LocalCopy,
                                "local_assignment_single");
                            changed |= did;
                            touched |= did;
                            ++work;
                        }
                        if (touched || (!taint_before && slot.truncated_here))
                        {
                            fan_out(i, Phase::kArgParam, i);
                        }
                    }
                    // 实参 -> 形参阶段。
                    out.arg_param_us +=
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t_arg_param_0)
                            .count();
                    const auto t_param_fanout_0 =
                        std::chrono::steady_clock::now();
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        param_fanout_due[j] |= param_fanout_next[j];
                        param_fanout_next[j] = 0;
                    }
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        if (!param_fanout_due[j])
                        {
                            continue;
                        }
                        param_fanout_due[j] = 0;
                        for (const ArgFlow& f : site_args[j])
                        {
                            std::span<const std::int32_t> batch;
                            // 实际采用的槽源是否携带截断标记（传染用）。
                            bool source_tainted = false;
                            if (!arg_ids[j][f.arg].empty())
                            {
                                batch = arg_ids[j][f.arg];
                            }
                            else
                            {
                                if (f.arg_slot != NPOS)
                                {
                                    batch = arena.view(
                                        slots[f.arg_slot].type_off,
                                        slots[f.arg_slot].type_len);
                                    source_tainted =
                                        slots[f.arg_slot].truncated_here;
                                }
                                if (batch.empty() && f.ret_slot != NPOS)
                                {
                                    batch = arena.view(
                                        slots[f.ret_slot].type_off,
                                        slots[f.ret_slot].type_len);
                                    source_tainted =
                                        slots[f.ret_slot].truncated_here;
                                }
                            }
                            if (batch.empty())
                            {
                                continue;
                            }
                            bool                      types_truncated = false;
                            std::vector<std::int32_t> cap;
                            if (batch.size()
                                > static_cast<std::size_t>(MAX_TYPES))
                            {
                                cap.assign(
                                    batch.begin(),
                                    batch.begin() + MAX_TYPES);
                                batch = cap;
                                out.budget_exhausted = true;
                                types_truncated = true;
                            }
                            if (f.target == NPOS)
                            {
                                if (f.family_targets.empty())
                                {
                                    continue;
                                }
                                // 虚族扇投投递（边构建见预解析段）：与直连
                                // 共享同一实参批（直收类型/实参变量槽/返回
                                // 槽三落点与截断、taint 传染同款），may 语
                                // 义 Low 档不提档（档位语义对齐
                                // param_flow_multiple 的多候选档）；批按各
                                // 目标槽的静态声明面裁剪（构建与缓存见
                                // family_face_of/family_clip_batch）——形
                                // 参声明类型是编译期硬约束，静态界面外的
                                // 类型对该 override 是不可能收到的假证
                                // 据，宁漏不投；面是闭包近似，全批面外
                                // 时整体不裁（对称守卫，防闭包缺边饿死真
                                // 决议）。全批不可解析的目标零投递（通道
                                // 在、证据不在——不置 fed 标记）。
                                for (const std::uint32_t dst : f.family_targets)
                                {
                                    family_clip_batch(dst, batch, true);
                                    if (family_clip_scratch.empty())
                                    {
                                        continue;
                                    }
                                    slot_family_pf_fed[dst] = 1;
                                    HintSlot&  dst_slot = slots[dst];
                                    const bool taint_before =
                                        dst_slot.truncated_here;
                                    dst_slot.truncated_here |=
                                        types_truncated || source_tainted;
                                    const bool did = add_types(
                                        dst_slot,
                                        family_clip_scratch,
                                        Confidence::Low,
                                        HintSource::ParamFlow,
                                        "param_flow_family");
                                    changed |= did;
                                    ++work;
                                    if (did
                                        || (!taint_before
                                            && dst_slot.truncated_here))
                                    {
                                        fan_out(dst, Phase::kParamFanout, j);
                                    }
                                }
                                continue;
                            }
                            // 截断与 taint 都落到目标形参槽：直收实参
                            // （site.arg_types）无槽载体，只能靠此截断点覆盖；
                            // 槽源实参按实际采用的源（caller 槽或返回槽）传染。
                            HintSlot&  target_slot = slots[f.target];
                            const bool taint_before =
                                target_slot.truncated_here;
                            target_slot.truncated_here |=
                                types_truncated || source_tainted;
                            const Confidence  conf = batch.size() > 1
                                                         ? Confidence::Low
                                                         : Confidence::Medium;
                            const std::string reason =
                                batch.size() > 1 ? "param_flow_multiple"
                                                 : "param_flow_single";
                            const bool did = add_types(
                                target_slot,
                                batch,
                                conf,
                                HintSource::ParamFlow,
                                reason);
                            changed |= did;
                            ++work;
                            if (did
                                || (!taint_before
                                    && target_slot.truncated_here))
                            {
                                fan_out(f.target, Phase::kParamFanout, j);
                            }
                        }
                    }
                    // 出参写回阶段：写回 ->
                    // 调用方实参槽。写回槽内容（并集）回填给实参 槽：单类型
                    // Medium（救活 0.7 档的证据来源），多类型由收尾 cap
                    // Low。参数按值传指针仍同指向物，回填语义一致；递归自传
                    // （f(p) 里再 f(p)）经 add_types 去重自然不动点。
                    // 形参形状闸不在此重复：OutParamFlow 行本身就来自 `*pp =
                    // e` 或壳 Set 写 穿，行存在即证明该形参被写穿。
                    out.param_fanout_us +=
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t_param_fanout_0)
                            .count();
                    const auto t_writeback_0 = std::chrono::steady_clock::now();
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        writeback_due[j] |= writeback_next[j];
                        writeback_next[j] = 0;
                    }
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        if (!writeback_due[j])
                        {
                            continue;
                        }
                        writeback_due[j] = 0;
                        for (const WbEdge& wb : site_wb[j])
                        {
                            const HintSlot& src_slot = slots[wb.param];
                            if (src_slot.type_len == 0)
                            {
                                continue;
                            }
                            // 族并集批按实参槽静态声明面裁剪（构建与缓存见
                            // family_face_of/family_clip_batch）：巨族 may 集
                            // 里静态可证界外的类型不投递，防稀释唯一决议；
                            // 面是闭包近似，全批面外时整体不裁（对称守
                            // 卫，防闭包缺边饿死真决议）；全批不可解析则
                            // 本边零投递（通道在、证据不在——不置 fed 标记）。
                            std::span<const std::int32_t> batch = arena.view(
                                src_slot.type_off,
                                src_slot.type_len);
                            if (wb.family)
                            {
                                family_clip_batch(wb.dst, batch, false);
                                if (family_clip_scratch.empty())
                                {
                                    continue;
                                }
                                batch = family_clip_scratch;
                                slot_family_fed[wb.dst] = 1;
                            }
                            // 直接传源槽区段（免拷贝）：add_types 把 arena
                            // 内批换算 成下标形态，追加扩容后重取；dst==param
                            // 自回填时批区段 与现集区段重合，自并集新增数为 0
                            // 恒 no-op——与旧形态 "先拷贝 guard"等价且更省。
                            // 族并集边不升 Medium：直连行才是 0.7 档证据来
                            // 源，族并集是 may 超集，档位交决议端收敛护栏。
                            const bool taint_before =
                                slots[wb.dst].truncated_here;
                            slots[wb.dst].truncated_here |=
                                src_slot.truncated_here;
                            const bool did = add_types(
                                slots[wb.dst],
                                batch,
                                wb.family ? Confidence::Low
                                          : Confidence::Medium,
                                HintSource::OutParamFlow,
                                wb.family ? "out_param_family"
                                          : "out_param_flow");
                            changed |= did;
                            ++work;
                            if (did
                                || (!taint_before
                                    && slots[wb.dst].truncated_here))
                            {
                                fan_out(wb.dst, Phase::kWriteback, j);
                            }
                        }
                    }
                    // 返回值 -> 调用点阶段。
                    out.writeback_us +=
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t_writeback_0)
                            .count();
                    t_return_flow_0 = std::chrono::steady_clock::now();
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        return_flow_due[i] |= return_flow_next[i];
                        return_flow_next[i] = 0;
                    }
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        if (!return_flow_due[i])
                        {
                            continue;
                        }
                        return_flow_due[i] = 0;
                        const std::size_t src = *c_src_of[i];
                        // 该路径不截断（全量直传），但源返回槽不完整时目标
                        // 槽同样不完整。
                        const bool taint_before = slots[i].truncated_here;
                        slots[i].truncated_here |= slots[src].truncated_here;
                        const bool did = add_types(
                            slots[i],
                            arena.view(
                                slots[src].type_off,
                                slots[src].type_len),
                            Confidence::Medium,
                            HintSource::ReturnFlow,
                            "return_flow");
                        changed |= did;
                        ++work;
                        if (did || (!taint_before && slots[i].truncated_here))
                        {
                            fan_out(i, Phase::kReturnFlow, i);
                        }
                    }
                    out.work_items = work;
                    // 分代压缩：追加写留下的死区段过半时重建 arena——每槽活区段
                    // 经驻留追加到新仓（同集槽共享区段，压缩后体积 = distinct
                    // 集合
                    // 计），旧仓与旧驻留表整体作废。区段内容不变仅位置变，一切语
                    // 义只读区段内容，压缩等价；压缩点在轮与轮之间，无存活区段视
                    // 图引用 arena。
                    {
                        std::size_t live = 0;
                        for (const HintSlot& s : slots)
                        {
                            live += s.type_len;
                        }
                        if (live * 2 < arena.buf.size())
                        {
                            TypeArena compact;
                            compact.buf.reserve(arena.buf.size() / 2);
                            for (HintSlot& s : slots)
                            {
                                const auto [off, len] = compact.append_dedup(
                                    arena.view(s.type_off, s.type_len));
                                s.type_off = off;
                                s.type_len = len;
                            }
                            arena = std::move(compact);
                        }
                    }
                    if (!changed)
                    {
                        break;
                    }
                    if (work > MAX_WORK)
                    {
                        out.budget_exhausted = true;
                        break;
                    }
                    if (round + 1 == MAX_ROUNDS)
                    {
                        out.budget_exhausted = true;
                    }
                }
                out.return_flow_us +=
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t_return_flow_0)
                        .count();
            }

            /** @brief intern id 区段的字符串终态物化（对外契约
             * vector<string>）。 */
            [[nodiscard]]
            std::vector<std::string> types_to_strings(
                std::span<const std::int32_t> ids)
            {
                std::vector<std::string> out;
                out.reserve(ids.size());
                for (const std::int32_t id : ids)
                {
                    out.push_back(type_of_id[static_cast<std::size_t>(id)]);
                }
                return out;
            }

            /**
             * @brief 槽终态物化：置信度收口（多候选 cap low/无类型 fallback）
             *        与 PropagatedHint 写回八列。
             */
            void finalize_hints()
            {
                // 转型声称填充：接收者指针来源未知的
                // reinterpret_cast/static_cast/C 样式转型，其目标类型是程序
                // 员的类型声称，直接接受为槽的类型证据——但仅当传播终态该槽
                // 动态集为空（工厂/直连/入参扇投等更强证据在场时声称弃用，
                // 有证据站点行为不变）。缓冲面已在装载期展开为声称面 ∪ 派生
                // 闭包；此处经 oracle 过滤后，界外根键（符号域解析失败，入
                // 动态集只会制造 key miss 阻断收敛）剔除再入集。填充不建传
                // 播边（声称只在槽本位生效），槽标签如实改写为声称来源
                // （reason=cast_target）；多声称并集多候选时由既有 cap-to-low
                // 纪律接管。
                for (std::size_t i = 0; i < slots.size(); ++i)
                {
                    if (slot_cast_claims.empty()
                        || slot_cast_claims[i].empty()
                        || slots[i].type_len != 0)
                    {
                        continue;
                    }
                    TypeHintRec probe;
                    probe.owner_key = slots[i].owner_key;
                    probe.var_name = slots[i].var_name;
                    probe.type_keys = slot_cast_claims[i];
                    probe.static_types = slots[i].static_types;
                    probe.source = HintSource::CastTarget;
                    probe.confidence = Confidence::Low;
                    std::vector<std::string> claimed =
                        oracle.points_to(probe);
                    std::erase_if(
                        claimed,
                        [this](const std::string& t)
                        {
                            return !lookup.by_key(t).has_value();
                        });
                    if (claimed.empty())
                    {
                        continue;
                    }
                    const std::vector<std::int32_t> ids =
                        intern_ids(claimed);
                    if (add_types(
                            slots[i],
                            ids,
                            Confidence::Low,
                            HintSource::CastTarget,
                            source_name(HintSource::CastTarget)))
                    {
                        // 类型集全部来自声称：标签与置信如实归位（别名行
                        // 的 Medium 空流不构成证据，不得顶替声称披露）。
                        slots[i].confidence = Confidence::Low;
                        slots[i].source = HintSource::CastTarget;
                        slots[i].reason =
                            std::string{
                                source_name(HintSource::CastTarget)};
                    }
                }
                // 每个槽的最终置信度：多候选一律 cap 到 low；无类型 =
                // fallback。
                for (std::size_t i = 0; i < slots.size(); ++i)
                {
                    HintSlot& slot = slots[i];
                    if (slot_family_fed[i] != 0)
                    {
                        ++family_wb_fed_slots;
                    }
                    if (slot_family_pf_fed[i] != 0)
                    {
                        ++family_pf_fed_slots;
                    }
                    if (slot.type_len == 0)
                    {
                        slot.confidence = Confidence::Fallback;
                    }
                    else if (slot.type_len > 1)
                    {
                        slot.confidence = Confidence::Low;
                    }
                    if (slot_family_fed[i] != 0
                        && slot.reason.find("out_param_family")
                               == std::string::npos)
                    {
                        // 族并集投递的披露后缀（槽标签档位语义不动）：类型构
                        // 成里有族并集来源时在 reason 记名，空槽直赋避免前导
                        // 分号。
                        slot.reason = slot.reason.empty()
                                          ? std::string{"out_param_family"}
                                          : slot.reason + ";out_param_family";
                    }
                    if (slot_family_pf_fed[i] != 0
                        && slot.reason.find("param_flow_family")
                               == std::string::npos)
                    {
                        // 入参族扇投的披露后缀（与出参族并集分记）：类型构
                        // 成里有扇投来源时在 reason 记名，语义同上。
                        slot.reason = slot.reason.empty()
                                          ? std::string{"param_flow_family"}
                                          : slot.reason + ";param_flow_family";
                    }
                    PropagatedHint h;
                    h.owner_key = slot.owner_key;
                    h.var_name = slot.var_name;
                    h.types = types_to_strings(
                        arena.view(slot.type_off, slot.type_len));
                    h.from_warm = incr_applied && !slot_in_closure[i];
                    h.static_types = slot.static_types;
                    h.confidence = slot.confidence;
                    h.source = slot.source;
                    h.reason = slot.reason;
                    h.truncated_here = slot.truncated_here;
                    h.alias_src = l1_src_of[i].has_value()
                                      ? hint_slot_key(
                                            slots[*l1_src_of[i]].owner_key,
                                            slots[*l1_src_of[i]].var_name)
                                      : std::string{};
                    h.ret_src = c_src_of[i].has_value()
                                    ? hint_slot_key(
                                          slots[*c_src_of[i]].owner_key,
                                          slots[*c_src_of[i]].var_name)
                                    : std::string{};
                    out.hints.push_back(std::move(h));
                }
            }

            /**
             * @brief 快照终态捕获（写侧）：finalize 循环后的槽终态平面态。
             *
             * @internal 区段取传播后的 arena（分代压缩同步过区段下标），与
             *           初始态捕获同下标对齐；落盘由调用方在事务提交成功后
             *           完成（先库后快照）。
             */
            void capture_snapshot_final()
            {
                // ---- 快照终态捕获（写侧）：finalize
                // 循环后的槽终态——区段取传播 后的
                // arena（分代压缩同步过区段下标），标量与 PropagatedHint（写
                // 回八列）同源。槽序与初始态捕获一致（传播不增删槽），按下标对齐
                // 更新；arena buf 与区段在同一时刻拍照，区段引用恒在界内。----
                if (plan != nullptr && plan->snap_dump != nullptr
                    && plan->snap_dump->slots.size() == slots.size())
                {
                    snap::SnapshotData& sd = *plan->snap_dump;
                    sd.arena_warm = arena.buf;
                    for (std::size_t i = 0; i < slots.size(); ++i)
                    {
                        const PropagatedHint& h = out.hints[i];
                        snap::SlotRec&        r = sd.slots[i];
                        r.warm_off = slots[i].type_off;
                        r.warm_len = slots[i].type_len;
                        r.warm_confidence = static_cast<int>(h.confidence);
                        r.warm_source = static_cast<int>(h.source);
                        r.warm_reason = h.reason;
                        r.alias_src = h.alias_src;
                        r.ret_src = h.ret_src;
                        r.truncated = h.truncated_here;
                    }
                }
            }

            /**
             * @brief 站点终态：五桶分类/接收者动态类型解析/增量受影响掩码。
             *
             * @internal 掩码按 (caller, file, line) 三元组扩张（边删除以三
             *           元组为键，同行多站点必须一起重解析）。
             */
            void finalize_sites()
            {
                // 增量受影响站点（dispatch 掩码）：脏/扇入/源漂移站点 +
                // 接收者槽 在闭包内的站点；按 (caller, file, line)
                // 三元组扩张——边删除以三 元组为键，同行多站点必须一起重解析。
                std::vector<char> affected(sites.size(), 0);
                for (std::size_t j = 0; j < sites.size(); ++j)
                {
                    if (!incr_applied)
                    {
                        affected[j] = 1;
                        continue;
                    }
                    if (site_due[j])
                    {
                        affected[j] = 1;
                    }
                }

                for (std::size_t j = 0; j < sites.size(); ++j)
                {
                    PropagatedSite& site = sites[j];
                    // 站点五桶分类（附加视图，消费方与桶序等价性见
                    // PropagationState::site_buckets）：判据经 classify_site
                    // 单点， 与 dispatch 的现场分桶兜底同源，桶内下标升序。
                    out.site_buckets[classify_site(site)].push_back(
                        static_cast<std::uint32_t>(j));
                    site.evidence = Confidence::Fallback;
                    HintSource receiver_source = HintSource::Declared;
                    if (!site.receiver_name.empty())
                    {
                        std::optional<std::size_t> src;
                        // obj.field：字段线索以声明该字段的记录为所有者。
                        if (!site.receiver_owner_key.empty())
                        {
                            src = find_slot(
                                site.receiver_owner_key,
                                site.receiver_name);
                        }
                        if (!src.has_value())
                        {
                            src =
                                find_slot(site.caller_key, site.receiver_name);
                        }
                        // this->field：字段线索以记录为所有者。
                        if (!src.has_value() && site.receiver_is_this
                            && !site.caller_owner_key.empty())
                        {
                            src = find_slot(
                                site.caller_owner_key,
                                site.receiver_name);
                        }
                        if (src.has_value())
                        {
                            site.dynamic_types = types_to_strings(arena.view(
                                slots[*src].type_off,
                                slots[*src].type_len));
                            site.evidence = slots[*src].confidence;
                            site.reason = slots[*src].reason;
                            receiver_source = slots[*src].source;
                            site.slot_type_truncated =
                                slots[*src].truncated_here;
                            if (incr_applied && slot_in_closure[*src])
                            {
                                affected[j] = 1; // 接收者槽在脏闭包内
                            }
                        }
                    }
                    if (site.dynamic_types.size() > 1)
                    {
                        // 多类型槽按来源给出具体原因，绝不一律写成
                        // param_flow_multiple。
                        site.evidence = Confidence::Low;
                        site.reason =
                            multiple_reason(receiver_source, site.reason);
                    }
                    out.sites.push_back(std::move(site));
                }
                if (incr_applied)
                {
                    // (caller, file, line)
                    // 三元组扩张：边删除按三元组整组清空，同行
                    // 的未受影响站点必须一并重解析以重发自己的边。
                    std::unordered_map<std::string, std::vector<std::size_t>>
                        by_triple;
                    by_triple.reserve(sites.size() * 2);
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        by_triple[std::format(
                                      "{}\x1f{}\x1f{}",
                                      sites[j].caller_key,
                                      sites[j].file_id,
                                      sites[j].line)]
                            .push_back(j);
                    }
                    std::vector<std::size_t> pending;
                    for (std::size_t j = 0; j < sites.size(); ++j)
                    {
                        if (affected[j])
                        {
                            pending.push_back(j);
                        }
                    }
                    for (const std::size_t j : pending)
                    {
                        for (const std::size_t sibling : by_triple[std::format(
                                 "{}\x1f{}\x1f{}",
                                 sites[j].caller_key,
                                 sites[j].file_id,
                                 sites[j].line)])
                        {
                            affected[sibling] = 1;
                        }
                    }
                    out.site_affected = std::move(affected);
                }
                out.source_sites = std::move(site_rows);
            }
        };

    } // namespace propagate_detail

    /**
     * @brief 读取整库 hints/call_sites，跑函数内与全局固定点传播。
     *
     * @param store 当前连接（同一事务内可见本批已写入的 hints/call_sites
     *              与阶段 B 已写入的 overrides/extends 边）。
     * @param oracle 类型 oracle 接缝（当前 stub）。
     * @param shared_rows 预载的符号投影（与 dispatch::resolve 共用一次
     *                    all_symbol_index_rows 全表读；分析期不写符号表，
     *                    两个阶段各读一遍是重复成本）。nullptr 时自行读取。
     * @param plan 增量计划（nullptr / active=false = 全量路径）。
     *             增量 = 装载 analysis_slots 终态做 warm start + 脏闭包
     *             （种子 = 脏文件槽/脏站点实参槽/跨文件扇入实参槽/被删行
     *             下游）重算；任何资格失败只置 plan->degraded，执行与全量
     *             路径零分叉。等价性论证：传播规则在排序类型集的单调
     *             （并置 + 置信度只降）半格上收敛到唯一最小不动点，闭包外
     *             槽的全部入边（含按持久化 src 键与捕获见证复原的旧边）
     *             均不受任何种子影响，warm 值即新系统不动点值。
     */
    [[nodiscard]]
    inline auto run(
        cg::db::GraphStore&                  store,
        const TypeOracle&                    oracle,
        const std::vector<cg::db::IndexRow>* shared_rows = nullptr,
        IncrementalPlan*                     plan = nullptr) -> PropagationState

    {
        using namespace propagate_detail;
        const auto t_load0 = std::chrono::steady_clock::now();
        std::vector<cg::db::IndexRow> owned_rows;
        if (shared_rows == nullptr)
        {
            owned_rows = cg::db::all_symbol_index_rows(store);
            shared_rows = &owned_rows;
        }
        const SymbolLookup lookup{*shared_rows};
        RunCtx             ctx{store, oracle, shared_rows, plan, lookup};
        ctx.t_symbols = std::chrono::steady_clock::now();
        ctx.init_symbol_domain();
        ctx.try_restore_snapshot();
        ctx.load_hint_rows();
        ctx.stream_hint_rows();
        ctx.build_sites();
        ctx.build_slot_index();
        ctx.load_warm_start();
        ctx.intern_type_sets();
        ctx.capture_snapshot_initial();
        ctx.preconcat_warm_arena();
        ctx.t_load1 = std::chrono::steady_clock::now();
        ctx.out.load_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              ctx.t_load1 - t_load0)
                              .count();
        // 装载段六分计时（共用同一终点，六段之和严格等于 load_us）。
        const auto seg_us = [](auto t0, auto t1)
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       t1 - t0)
                .count();
        };
        ctx.out.load_symbols_us = seg_us(t_load0, ctx.t_symbols);
        ctx.out.load_hints_us = seg_us(ctx.t_symbols, ctx.t_hints);
        ctx.out.load_sites_us = seg_us(ctx.t_hints, ctx.t_sites);
        ctx.out.slot_index_us = seg_us(ctx.t_sites, ctx.t_slotidx);
        ctx.out.warm_us = seg_us(ctx.t_slotidx, ctx.t_warm);
        ctx.out.intern_us = seg_us(ctx.t_warm, ctx.t_load1);
        const auto t_resolve0 = std::chrono::steady_clock::now();
        ctx.preresolve_edges();
        ctx.out.resolve_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_resolve0)
                .count();
        const auto t_closure0 = std::chrono::steady_clock::now();
        ctx.build_incremental_closure();
        ctx.out.closure_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_closure0)
                .count();
        ctx.run_fixed_point();
        ctx.t_final0 = std::chrono::steady_clock::now();
        ctx.finalize_hints();
        ctx.capture_snapshot_final();
        ctx.finalize_sites();
        ctx.out.family_wb_edges = ctx.family_wb_edges;
        ctx.out.family_pf_edges = ctx.family_pf_edges;
        ctx.out.family_wb_fed_slots = ctx.family_wb_fed_slots;
        ctx.out.family_pf_fed_slots = ctx.family_pf_fed_slots;
        ctx.out.finalize_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - ctx.t_final0)
                .count();
        return std::move(ctx.out);
    }

} // namespace cg::analysis
