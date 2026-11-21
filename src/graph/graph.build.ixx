module;
#include "../db/db.tables.h"

/**
 * @module cg.graph.build
 *
 * @brief 真增量索引器：把编译数据库导入 SQLite（经 cg.db 仓储）。
 *
 * @internal
 * 一次运行：
 *   1. 新鲜度扫描——构建系统语义：先 stat（mtime+size），sha256 作决胜，
 *      逐条目规范化编译命令摘要；重复 TU（相同主文件 + 摘要）共享
 *      一次解析。
 *   2. 脏集扩展——内容变脏/删除的文件传播到其传递包含闭包（includes
 *      表）命中的每个 TU；直接变脏的主文件与内容变脏的头文件一样
 *      播种重写集（主文件自己的站点必须清除+重写，否则一次正文编辑
 *      把编辑前的旧行留下，而阶段 A 已刷新其符号）。
 *   3. 脏 TU 在工人池上解析+提取（每工人一个 CXIndex）；随后第二遍解析
 *      （配置变更域）把编译数据库/包含图/内容变更驱动的种子（抹除/失
 *      败/盘缺/摘要退役文件的前向闭包与包含差目标）的观察者闭包翻位
 *      补解析；失败与盘上缺失的主文件同被抹除者一样播种其前向闭包，
 *      使共享头的出现计数不再统计死 TU。
 *   4. 单一批次事务：阶段 A 符号 upsert（稳定 id）、仅限拥有完整 TU
 *      覆盖的文件的逐文件清除（其出现计数得以精确重建）、阶段 B
 *      三层边解析（被清除符号已退役）、包含、状态、指纹，以及抹除
 *      无当前 TU 可达行的孤儿清扫。
 * 全部写入发生在调用线程、按编译数据库顺序，故无论哪个工人先完成，
 * 存储的行都与串行全量运行完全一致；验收性质：同一树状态下每张表
 * incremental(db) == full-reindex(db)（计时与 meta 除外）。
 *
 * 模块 BMI 失败分类：逐 TU 的命名失败类（module-bmi-missing、
 * bmi-config-mismatch、module-not-found、partition-not-fed、
 * two-stdlib-clash、stale-cascade、module-attachment-conflict——同一
 * 声明两路归属的 MSVC 宽容形状，工程侧修法随失败文本携带）取代聚合
 * 的"N fatal errors"计数器；
 * 每条分类点名确切工件（模块名、pcm 路径、或 ccg 自记键的差异轴），
 * 使失败无需手工重跑 clang 即可行动。
 * cl 方言（MSVC permissive）的方言语义残余另记 msvc-permissive-*
 * 具名类（classify_dialect_diag_failure），与模块分类学正交。
 *
 * db.tables.h 置于全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23
 * 表模型定义在此可见，以合并模块记录的模板特化（完整机理见
 * db.tables.h）。
 */
export module cg.graph.build;

import std;
import cg.util;
import cg.util.cstd;
import cg.util.platform;
import cg.util.instrdigest;
import cg.db;
import cg.clang;
import cg.extract;
import cg.analysis;
import cg.graph.traverse;
import cg.graph.headcache;

export namespace cg::graph
{

    /**
     * @brief 声明在运行中被偷走（提交段事务内心跳匹配零行）。
     *
     * 提交段发现租约已被改写时抛出：~Tx 先行 ROLLBACK，本批一行业务行都
     * 不落库（把"双写者"降级为"干净让位"）。索引路径让它一路成为命令行
     * 失败（消息可行动）；增量持有者（`ccg update` 传入 caller_holder）
     * 捕获它优雅停轮——队列的未来归偷取者。专设类型是为了让调用方能与
     * 普通运行时错误区分捕获，绝不靠消息文本匹配。
     */
    struct ClaimStolenError : std::runtime_error
    {
        explicit ClaimStolenError(const std::string& what)
            : std::runtime_error(what)
        {
        }
    };

    /** @brief 一次索引运行的选项。 */
    struct IndexOptions
    {
        std::vector<std::string> roots;         // 规范化的绝对项目根
        bool                     force = false; // --force/--full：重解析每个 TU
        bool                     progress = true; // stderr 上的逐 TU 进度行
        std::string              compile_db_path; // 非空时记入 meta
        std::string              db_path; // 仅报告用（":memory:" -> 不计大小）
        /**
         * @brief 解析段工人池的期望规模；0 = 自动按核。
         *
         * 统一并发上限的解析段消费点：CLI 侧由 resolve_jobs 合成生效值
         * （--jobs 旗标 > 库配置键 index-jobs > 自动），BMI 自建段经
         * SelfBuildOptions.jobs 吃同一合成值——两段时间上先后发生不叠加，
         * 峰值并发即该值。此处直接构造 IndexOptions 的调用方（测试等）
         * 沿用 0 = 自动（cg::util::pool::auto_worker_count）。
         */
        int  jobs = 0;
        bool dry_run = false; // 只打印计划，不写库
        /**
         * @brief 提交段写锁仲裁（分段认领 + 事务内心跳 + 排队）。
         *
         * 解析段只读、不持锁不参与；协调只发生在提交段（单一大事务）取
         * 写锁之前：claim_update 排队等待 → 提交段事务内每 ~60s 心跳
         * （零行 = 被偷 → 干净 ROLLBACK）→ COMMIT 后释放。默认 none：
         * `ccg update` 复用本引擎时调用方自持协议，绝不能再入。
         */
        enum class Coordination : std::uint8_t
        {
            none,      ///< 不参与（update 复用引擎、内存库、干跑）
            wait,      ///< 排队等待（--wait [ms]，默认 30 分钟上界）
            fail_fast, ///< 有活声明即失败（--no-wait）
        };
        Coordination coordination = Coordination::none;
        /**
         * @brief 调用方自持的声明记号（caller-held 模式；排队租约协议）。
         *
         * 非空时提交段不自行取声明（不排队、不等待、不建 RAII 守卫），
         * 只做两件事：事务内心跳按本记号续租（heartbeat_in_tx 的
         * WHERE holder=? 保证绝不改写偷取者的行）；心跳匹配零行时抛
         * ClaimStolenError 干净回滚。供两类调用方使用：`ccg update` 的
         * 引擎批（持有者循环自己拥有声明，批间 peek / 批后出队是让位点，
         * 批内让位靠这里）；`ccg index`/`ccg msbuild` 的索引尾部（进程
         * 一开始就主动偷取了租约——db 层 steal_update，本字段让提交段
         * 复用同一行而不是再排队认领自己）。空串 = 既有三种 Coordination
         * 语义原样。
         */
        std::string caller_holder;
        /** @brief caller-held 模式的心跳续租时长（毫秒；0 = 自适应公式）。 */
        std::int64_t caller_lease_ms = 0;
        /** @brief wait 模式的排队等待上界（毫秒；超时诚实失败）。 */
        std::int64_t wait_limit_ms = 1'800'000;
        /** @brief 全量 sha256 扫描（保 mtime 的拷贝会击穿 stat 快路径）。 */
        bool paranoid = false;
        /**
         * @brief 外部播种的脏文件（ccg update；已规范化）。
         *
         * 每个种子在新鲜度扫描判定之前强制进入脏集——主文件种子重解析，
         * 非主文件种子视为内容变脏并传播到其包含者闭包——因此即使编辑
         * 保持了 mtime+size，本运行也得出与普通索引运行对同一树状态
         * 完全相同的结论。
         */
        std::vector<std::string> seed_files;
        /** @brief 动态派发分析（schema v10；默认开）。 */
        bool dispatch = true;
        /**
         * @brief 挪时机设计：编辑批只落阶段 A 行 + 按文件粒度删 vd 边，
         *        RTA/类型传播/dispatch/写回整体推迟到独立收敛批
         *        （`ccg update --drain` / UserPromptSubmit 派生）。
         *
         * @internal 仅在 dispatch=true 时生效。脏期间 meta `dispatch_stale`
         * 记滞后文件数、`dispatch_stale_files` 记脏文件集，查询侧
         * DispatchIndex 感知并显式标注，绝不静默旧数据。收敛批（全量
         * dispatch_commit）或任何普通 `ccg index`/非延迟 update 运行都会
         * 清掉这两个键。租约 D 兼容：延迟收敛就是又一个 update 批次，
         * claim/心跳协议原样复用。
         */
        bool defer_dispatch = false;
    };

    /** @brief 一次索引运行的汇总统计。 */
    struct IndexSummary
    {
        std::int64_t tus = 0;
        std::int64_t fresh = 0;
        std::int64_t stale = 0;
        std::int64_t skipped = 0;
        std::int64_t failed = 0;
        std::int64_t purged_files = 0; // 删除的行（已删源/孤儿）
        /**
         * @brief 快路径跳过的可达性清扫批次数。
         *
         * @internal 无删除且无包含行净减少的提交批整场跳过全局清扫
         *           （判定输入=三种删除来源+覆盖差，见 commit_batch 内
         *           sweep_needed 的说明）；有删除的批照走全量清扫，
         *           此处保持 0。供增量测试断言与性能观测。
         */
        std::int64_t reachability_sweeps_skipped = 0;
        /** @brief 运行后整库总量（stats 形态）。 */
        std::int64_t symbols = 0;
        std::int64_t edges = 0;
        std::int64_t includes = 0;
        std::int64_t imports = 0; // 模块导入关系（schema v5）
        /**
         * @brief 本运行的命名失败类（模块 TU 失败分类学；经典解析错误归入
         *        "parse-failed"），计数降序再按名——failed > 0 时摘要
         *        始终展示的诚实聚合。
         */
        std::vector<std::pair<std::string, std::int64_t>> failure_classes;
        /** @brief PCH v2 规划/构建计数（无 PCH 工程全为 0）。 */
        std::int64_t pch_built = 0;
        std::int64_t pch_reused = 0;
        std::int64_t pch_fallbacks = 0;
        std::int64_t pch_replay_degraded = 0;
        std::int64_t pch_unusable = 0;
        /**
         * @brief PCH 文本回退的两种结局分账：fallback_ok=回退后解析成功
         *（含影子失败后文本重试成功），fallback_failed=回退后仍失败（失败
         * 类 pch-fallback-failed，与纯 pch-unusable 不混计）。
         *
         * @internal 只统计本轮真正解析过的回退 TU；规划面总数在
         * pch_fallbacks（增量轮里未被重解析的规划条目不出现在这两格）。
         */
        std::int64_t pch_fallback_ok = 0;
        std::int64_t pch_fallback_failed = 0;
        /** @brief 逐 TU 的 PCH 判定原因（"`<file>`: `<note>`"）。 */
        std::vector<std::string> pch_notes;
        /**
         * @brief cl 方言宽松特化的诊断驱动内存改写计数（cl 方言专属）。
         *
         * @internal clang 对 MSVC 接受的类外成员特化定义报
         * "template specialization requires 'template<>'" 且自带 FixItHint；
         * ccg 按编译器给定的插入点在内存源码缓冲定点插入 "template<> "
         * 后重解析一次（不落盘）。dialect_fixit_tus = 改写后解析转绿的
         * TU；dialect_fixit_failed_tus = 改写并重解析后仍失败（失败类
         * 口径不变，照常归 msvc-permissive-lenient-specialization）。
         * GNU 方言与无 FixIt 的形状零改动，两个计数恒 0。
         */
        std::int64_t dialect_fixit_tus = 0;
        std::int64_t dialect_fixit_failed_tus = 0;
        /** @brief 被改写 TU 的注记（"`<file>`: N fixit edit(s) in M file(s)
         * ..."）。
         */
        std::vector<std::string> dialect_fixit_notes;
        /**
         * @brief ANSI 编码消毒计数（cl 方言专属）。
         *
         * @internal 字符串/字符字面量内系统 ANSI 码页不可解码的字节
         * （如 GBK 未分配码点——MSVC 按原样透传、clang 在宽/Unicode 字面量
         * 上报 "illegal character encoding" 硬错）被改写为八进制转义后
         * 重解析一次（不落盘）。encoding_scrub_tus = 消毒后解析转绿的 TU；
         * encoding_scrub_failed_tus = 消毒并重解析后仍失败（残余场景照常
         * 归 msvc-lenient-source-encoding 失败类）。合法可解码文本、注释、
         * 原始字符串零改动，两个计数恒 0。
         */
        std::int64_t encoding_scrub_tus = 0;
        std::int64_t encoding_scrub_failed_tus = 0;
        /** @brief 被消毒 TU 的注记（"`<file>`: N encoding unit(s) in M file(s)
         * ..."）。 */
        std::vector<std::string> encoding_scrub_notes;
        /**
         * @brief "报错但 AST 完好"的良性错误索引计数（白名单家族，见
         *        benign_error_families）。
         *
         * @internal 全部 Error 级诊断命中白名单且 TU 有效时该 TU 按成功
         *           索引（不进失败类、不计入 failed），此处计数 + 注记
         *           留痕——AST 完好时的硬错误（如引用成员绑定临时对象的
         *           悬垂引用族，无旗标可压）不再令 TU 整只失败。与失败
         *           类分账：TU 无效/走查崩溃/清单外错误照常判失败，不
         *           出现在这两个字段里。
         */
        std::int64_t benign_errored_tus = 0;
        /** @brief 良性索引 TU 的注记（"`<file>`: ignored N benign error(s)
         * [<family>]: <首条拼写>"）。 */
        std::vector<std::string> benign_error_notes;
        /**
         * @brief 系统头来源标注（"auto-detected" 或空）。
         *
         * @internal 外来原生 MSVC cdb 的 vcvars 重建失败时如实标注；正常
         * 自包含 cdb 与无 cl 条目的工程保持为空（零变化门禁）。
         */
        std::string  system_includes;
        std::int64_t unresolved = 0; // 本运行阶段 B 的丢弃数
        /** @brief schema v10 动态派发分析计数。 */
        std::int64_t type_hints = 0;
        std::int64_t call_sites = 0;
        std::int64_t dispatch_edges = 0;
        std::int64_t dispatch_fallback_sites = 0;
        /**
         * @brief 本运行 escalation 判定为真的提交批数（派发全站点重解析腿）。
         *
         * @internal 触发面：传播增量态降级、继承/覆盖/特化边集摘要变化、
         *           `#external` 行集变化、名字桶警报真触发（延迟批经 meta
         *           传递）。> 0 证明该批真的走了派发边整类清空重建（vd 边
         *           删除 + 全站点重解析）——传播侧 rounds/work_items 只量
         *           固定点传播（收敛态语料上 escalation 批也可为 0，实测
         *           rounds=1/work_items=0），幂等门要钉"重解析真发生"须用
         *           本计数。
         */
        std::int64_t dispatch_escalations = 0;
        std::int64_t analysis_failed_tus = 0;
        /** @brief 全局传播固定点迭代轮数/工作量（可观测性；触顶时
         * budget=true）。 */
        std::int64_t analysis_rounds = 0;
        std::int64_t analysis_work_items = 0;
        bool         analysis_budget_exhausted = false;
        /** @brief 外部边界 RTA 写入的 `#external` 行数（root set 线）。 */
        std::int64_t external_rta_hints = 0;
        /**
         * @brief 运行收尾时仍未收敛的派发滞后文件数（defer_dispatch
         * 延迟收敛）。
         *
         * @internal 0 = 派发数据与已提交行一致（全量分析刚跑过或从未延迟）。
         * 与 meta `dispatch_stale` 同源，供 update 渲染与 hook 注入显式标注。
         */
        std::int64_t dispatch_stale_files = 0;
        /**
         * @brief 指令哈希门槛的门控观测计数（只观测不改行为）。
         *
         * @internal 口径与观测实验一致：内容真变脏的文件里，
         * c1_normal_changes = 窄版指令摘要前后相同（普通改动可放行
         * 族）；c1_instr_changes = 摘要变化（指令改动，必须级联）；
         * c1_first_seen = 无基线可比（本运行首次收录，保守归指令侧观察）。
         * 计数同时写 meta 键 c1_normal_changes/c1_instr_changes。
         */
        std::int64_t c1_normal_changes = 0;
        std::int64_t c1_instr_changes = 0;
        std::int64_t c1_first_seen = 0;
        /**
         * @brief 头级缓存的启用判定观测计数（启用门槛：命中率≥40%、漂移=0）。
         *
         * @internal headcache_hits/lookups 命中率口径；headcache_records
         * 本运行落账分区数；headcache_enabled 为false 时全 0（CCG_HEADCACHE=0
         * 或 fb 波式分解在开——两者不兼容，记录面被搬空）。
         */
        std::int64_t             headcache_enabled = 0;
        std::int64_t             headcache_lookups = 0;
        std::int64_t             headcache_hits = 0;
        std::int64_t             headcache_records = 0;
        std::int64_t             edges_by_type[cg::db::edge_type_count] = {};
        double                   elapsed_seconds = 0;
        std::int64_t             db_size_bytes = 0;
        std::vector<std::string> roots;
        std::string              mode;            // "full" | "incremental"
        bool                     dry_run = false; // 仅计划，未写任何东西
        std::int64_t             dirty_bytes = 0; // 计划：待重解析的主文件字节
        /**
         * @brief 提交段租约协调的观测面（IndexOptions::coordination）。
         *
         * coord_waited_ms = claim 排队等待的实际墙钟；coord_holder = 本次
         * 持有的声明记号（含 pid）；coord_drain_pending = 释放时
         * pending_updates 仍非空（索引运行期间的编辑已入队）——调用方据此
         * detach 一个 `ccg update --drain` 收敛。coordination == none 时
         * 保持 0/空/false。
         */
        std::int64_t coord_waited_ms = 0;
        std::string  coord_holder;
        bool         coord_drain_pending = false;
        /**
         * @brief 解析期租约心跳的成功续租次数（0 = 未启用或一跳未成）。
         *
         * @internal caller-held 且非干跑/内存库时，解析段由伴随心跳线程
         *           周期续租（detail::ParseLeaseKeepalive）；此计数进
         *           --json 载荷，供 e2e 与真库实测证明解析期确实有人
         *           在续租（大库解析以十分钟计，租约只有百秒级）。
         */
        std::int64_t parse_lease_beats = 0;
        /**
         * @brief 本运行实际覆盖的规范化路径（`ccg update` 的批后清除集）。
         *
         * 组成：本次实际重解析（含尝试后失败）的主 TU 路径，加上包含
         * 闭包传播触及的每个既有文件（种子、内容脏文件、被传播到的
         * 包含者、抹除/缺失文件）。这是保守集合——只有引擎确确实实
         * 决定处理过的文件才在内；是否可出队还须由调用方叠加"批前
         * 快照哈希 == 当前哈希"的判定。dry-run 为空。
         */
        std::vector<std::string> covered_files;
    };

    namespace detail
    {

        /** @internal cg.extract 的 SymKind/EdgeKind 与播种 id 一一对应。 */
        constexpr auto to_db_kind(cg::extract::SymKind k) -> cg::db::SymbolKind
        {
            return cg::db::symbol_kind_of(
                static_cast<std::int64_t>(static_cast<int>(k)));
        }

        constexpr auto to_db_edge(cg::extract::EdgeKind k) -> cg::db::EdgeType
        {
            return cg::db::edge_type_of(
                static_cast<std::int64_t>(static_cast<int>(k)));
        }

        /**
         * @brief 两个 steady_clock 时间点之间的微秒差。
         *
         * @internal 支撑 -v 的逐阶段计时；steady_clock 读数是纳秒级精度，
         * 每阶段一对读数不构成可测开销。
         */
        [[nodiscard]]
        inline auto phase_us(
            std::chrono::steady_clock::time_point a,
            std::chrono::steady_clock::time_point b) -> double
        {
            return std::chrono::duration<double, std::micro>(b - a).count();
        }

        /**
         * @brief 一个文件的 stat() 身份：构建系统快路径的数据。
         *
         * @internal
         * mtime 存为 std::chrono::file_clock 原生时长自纪元起的原始滴答计数
         * ——一个不透明的、单调可比较的身份，既避开异域 file_clock 纪元的
         * int64 纳秒溢出，也不依赖 to_utc/to_sys 的可用性；该值只与同一平台
         * 写下的指纹比较。
         */
        struct FileStat
        {
            bool         ok = false;
            std::int64_t mtime_ns = 0;
            std::int64_t size = 0;
        };

        [[nodiscard]]
        inline auto stat_file(const std::string& path) -> FileStat
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path  p{path};
            const auto      mtime = fs::last_write_time(p, ec);
            if (ec)
            {
                return {};
            }
            const auto size = fs::file_size(p, ec);
            if (ec)
            {
                return {};
            }
            const std::int64_t ticks = mtime.time_since_epoch().count();
            return {
                .ok = true,
                .mtime_ns = ticks,
                .size = static_cast<std::int64_t>(size)};
        }

        /**
         * @brief 文件字节的 sha256；不可读时返回空串。
         *
         * 调用方把空串视为内容不可验证，从而强制走重解析路径。
         */
        [[nodiscard]]
        inline auto hash_file(const std::string& path) -> std::string
        {
            try
            {
                return cg::util::sha256_file_hex(path);
            }
            catch (const std::exception&)
            {
                return {};
            }
        }

        /**
         * @brief 判定路径是否落在任一根之内（等于根，或以根加目录分隔符
         *        开头）。
         *
         * @internal 与 cg.extract/cg.analysis 的 under_roots 同语义——根集
         * 过滤是收录身份的承重判定，界内界外的结论必须出自单点；空根不
         * 参与判定，根自身计入。
         */
        [[nodiscard]]
        inline auto path_in_roots(
            std::string_view                path,
            const std::vector<std::string>& roots) -> bool
        {
            for (const std::string& root : roots)
            {
                if (root.empty())
                {
                    continue;
                }
                if (path == root
                    || (path.starts_with(root)
                        && (root.back() == '/' || path[root.size()] == '/')))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 判定两个路径拼写是否指同一文件。
         *
         * 两者都存在时由 fs::equivalent 裁定（不区分大小写的文件系统把拼写
         * 折叠为同一文件；POSIX 保持真正不同的文件分离，那些绝不合并）。
         * 存储的拼写已不存在意味着文件在盘上被改拼写了（目录改成不同大小写），
         * 折叠候选是其唯一可能的继任者——一侧已消失，fs::equivalent 无法
         * 作出该判定。
         */
        [[nodiscard]]
        inline auto same_file_spelling(
            const std::string& stored,
            const std::string& incoming) -> bool
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(fs::path{stored}, ec) || ec)
            {
                return true;
            }
            const bool same =
                fs::equivalent(fs::path{stored}, fs::path{incoming}, ec);
            return !ec && same;
        }

        /**
         * @brief 清除折叠到保留行（最小 id 胜出）之上的多余 files 行。
         *
         * @param healed_affected 非空时收进被 CASCADE 清票的符号集（先读后
         *                        删；归零仲裁候选，交给提交段记账块并
         *                        集——票数通道的受影响集纪律与显式撤票
         *                        同款）。
         *
         * @internal
         * 折叠复行是早期大小写漂移运行留下的残骸（一个文件的两种拼写，各带
         * 一份重复的头子图）。包含解析器不区分大小写地匹配，两种拼写永远
         * 双双可达，连 --full 都无法治愈——只有显式合并可以。配对先经
         * same_file_spelling 验证：仅大小写不同的真正独立文件（POSIX）不是
         * 复行，原样保留。库中带伤（行被删除）时返回 true，告知调用方重解析
         * 一切，使幸存行从新提取重写。删除 files 行时外键级联清除其符号/
         * 站点/包含/指纹行。
         */
        inline auto heal_folded_duplicate_files(
            cg::db::GraphStore&         store,
            std::vector<cg::db::SymId>* healed_affected = nullptr) -> bool
        {
            const std::vector<cg::db::FileRow> files = cg::db::all_files(store);
            std::unordered_map<std::string, cg::db::FileRow> first;
            std::vector<cg::db::FileId>                      losers;
            for (const cg::db::FileRow& fr : files)
            {
                const std::string key = cg::util::path_compare_key(fr.path);
                const auto        it = first.find(key);
                if (it == first.end())
                {
                    first.emplace(key, fr);
                    continue;
                }
                if (same_file_spelling(it->second.path, fr.path))
                {
                    losers.push_back(fr.id);
                }
            }
            if (losers.empty())
            {
                return false;
            }
            auto tx = store.begin_tx();
            for (const cg::db::FileId fid : losers)
            {
                // 受影响集先于删除物化（票行随后随文件行 CASCADE 消失，
                // 删除后读不到）。
                if (healed_affected != nullptr)
                {
                    for (const cg::db::SymId sid :
                         store.contributions_of_file(fid))
                    {
                        healed_affected->push_back(sid);
                    }
                }
                // 删除纪律（先删引用方再删被引用方）：先显式清掉该行的符号/
                // 站点/包含/边等引用行，files 行最后删——不依赖外键 CASCADE
                // 兜底，与提交段 purge_file→erase_file 的既有顺序一致。
                store.purge_file(fid, std::span<const cg::db::StrId>{});
                store.erase_file(fid);
            }
            tx.commit();
            return true;
        }

        /**
         * @brief 把符号末日集向子成员级联至不动点。
         *
         * @internal 增量驱动：首轮以调用方给入的全部 id 起查，其后每轮只
         * 把上一轮新并入者交给 child_symbols，直到一轮无新增——每个 id
         * 恰好进一次查询，不再有不动点确认轮的全量重查。级联期间库对本
         * 进程不变，增量闭包与逐轮全量展开严格同集。new_kids 非空时顺带
         * 收集本次新并入的 id（覆盖末日的 grown 增量即用此出参）。
         */
        inline auto cascade_doomed_children(
            cg::db::GraphStore&                store,
            std::unordered_set<cg::db::SymId>& doomed,
            std::vector<cg::db::SymId>         frontier,
            std::vector<cg::db::SymId>*        new_kids = nullptr) -> void
        {
            while (!frontier.empty())
            {
                const std::vector<cg::db::SymId> kids =
                    cg::db::child_symbols(store, frontier);
                frontier.clear();
                for (const cg::db::SymId id : kids)
                {
                    if (doomed.insert(id).second)
                    {
                        frontier.push_back(id);
                        if (new_kids != nullptr)
                        {
                            new_kids->push_back(id);
                        }
                    }
                }
            }
        }

        /**
         * @brief 规范化编译命令摘要。
         *
         * @internal
         * 重放 argv 已经规范化（编译器记号已丢弃、响应文件已展开、包含路径
         * 已绝对化并规范化），故带长度前缀的拼接再 sha256 即编译数据库逐
         * 条目的稳定指纹——这是 compile_commands.json 的结构化差异比对，
         * 能挺过 cmake 任意条目顺序的再生成，并逮住 Release 库换掉 Debug
         * 库的情况。
         */
        [[nodiscard]]
        inline auto argv_digest(std::span<const std::string> argv)
            -> std::string
        {
            std::string buf;
            for (const std::string& tok : argv)
            {
                buf += std::to_string(tok.size());
                buf.push_back(':');
                buf += tok;
                buf.push_back('\n');
            }
            return cg::util::sha256_hex(std::as_bytes(std::span{buf}));
        }

        /**
         * @brief 新鲜度检查所键控的逐条目解析身份：重放 argv 摘要，模块 TU
         *        另加模块覆盖层纪元标记。
         *
         * @internal
         * 导出置空覆盖层（把导出命名空间抹成空白再解析的模块 TU 方案）
         * 在不改变 argv 与文件内容的前提下改变模块 TU 解析的提取结果，故
         * 覆盖层之前的指纹必须恰好失配一次——把纪元折进摘要，使既有数据库
         * 以一次全量模块 TU 重解析完成升级，而不是永远静默保留空的导出
         * 命名空间行。覆盖层语义再次改变时递增该标记。
         */
        [[nodiscard]]
        inline auto parse_digest(const cg::clang::CompileCommand& cmd)
            -> std::string
        {
            std::string buf = argv_digest(cmd.argv);
            if (cg::clang::is_module_tu(cmd))
            {
                buf += "#module-export-overlay:1";
            }
            // 无 PCH 的条目一个字节都不加（零变化门禁）；涉及 PCH 的条目把
            // 规划身份折进来——影子重建/替换/引擎切换必然失配并触发重解析。
            if (!cmd.pch.identity.empty())
            {
                buf += "#pch:" + cmd.pch.identity;
            }
            return cg::util::sha256_hex(std::as_bytes(std::span{buf}));
        }

        /**
         * @brief 一条重放 argv 喂进的包含目录数。
         *
         * GNU 拼写（-I/-isystem/-iquote/-idirafter/-imsvc，独立或联合 -I）
         * 与 cl 拼写（/I、/external:I，独立或联合）都计入。它是包含缺失
         * 解析失败的诚实后缀（"'x.h' file not found (N include dirs fed)"）：
         * 错误或过短的搜索清单就此直接暴露在失败文本里，而不再像被静默
         * 丢弃的包含路径（msbuild 的 INCLUDE 分类缺陷在此前读起来像个谜）。
         *
         * @internal -FI 是强制文件而非目录，不计入。
         */
        [[nodiscard]]
        inline auto include_dir_count(std::span<const std::string> argv)
            -> std::size_t
        {
            std::size_t count = 0;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                if (a == "-I" || a == "-isystem" || a == "-iquote"
                    || a == "-idirafter" || a == "-imsvc" || a == "/I"
                    || a == "/external:I")
                {
                    ++count;
                    if (i + 1 < argv.size())
                    {
                        ++i; // 分写形式：该值 token 不是 flag，须一并跳过
                    }
                    continue;
                }
                if ((a.starts_with("-I") && a.size() > 2)
                    || (a.starts_with("/I") && a.size() > 2)
                    || (a.starts_with("/external:I") && a.size() > 12))
                {
                    ++count; // 连写形式：flag 与值在同一 token
                }
            }
            return count;
        }

        /**
         * @brief 重放 argv 携带的 -std 值（无则空串；最后一个生效，即驱动器
         *        规则）。
         */
        [[nodiscard]]
        inline auto std_of_argv(std::span<const std::string> argv)
            -> std::string
        {
            std::string found;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                if (argv[i] == "-std" && i + 1 < argv.size())
                {
                    found = argv[++i];
                }
                else if (argv[i].starts_with("-std="))
                {
                    found = argv[i].substr(5);
                }
                else if (argv[i].starts_with("/std:"))
                {
                    found = argv[i].substr(5);
                }
            }
            return found;
        }

        /**
         * @brief text 中自 from 起第一个'单引号'区间（诊断以这种方式引用模块
         *        名 / pcm 路径）。
         */
        [[nodiscard]]
        inline auto quoted_span(std::string_view text, std::size_t from)
            -> std::string_view
        {
            const std::size_t open = text.find('\'', from);
            if (open == std::string_view::npos)
            {
                return {};
            }
            const std::size_t close = text.find('\'', open + 1);
            if (close == std::string_view::npos)
            {
                return {};
            }
            return text.substr(open + 1, close - open - 1);
        }

        /**
         * @brief 一个模块 TU 的 BMI 集的喂给前校验（契约的"先校验再喂给"）。
         *
         * 每个喂给的 pcm 必须存在，且库记录的生产者编译器与已加载引擎版本
         * 一致（pcm 格式锁——clang 以静默的 rc=4 拒绝异版本 pcm，故 ccg
         * 拒绝喂给并点名差异轴）。返回分类后的失败文本，喂给安全可解析时
         * 返回 ""。
         *
         * @internal
         * 无喂给的 TU 只有在撰写命名模块导入时才是 BMI 硬失败；头单元降级
         * 与经典重放都能正常解析。失败路径的分类器在解析真正死亡时才点名
         * 类别——快乐路径零开销零误报（路由层已用喂给或预定失败把现实情形
         * 提前拦截）。生产者-引擎版本检查仅对 clang 生产者有意义；
         * 非 clang 的生产者（MSVC 工具链侧）由 msbuild 路由处理。
         */
        [[nodiscard]]
        inline auto validate_bmi_feeds(
            const cg::clang::CompileCommand& cc,
            const std::string&               engine_token) -> std::string
        {
            if (cc.module_feeds.empty())
            {
                return {};
            }
            namespace fs = std::filesystem;
            std::string missing_names;
            std::string missing_pcm;
            std::size_t missing_count = 0;
            for (const cg::clang::ModuleFeed& feed : cc.module_feeds)
            {
                std::error_code ec;
                // 0 字节/半截 pcm 绝不可喂给：它们会让 clang 静默 rc=4，且旧
                // 缓存会永久复用。按缺失处理 → 明年的运行会重建。
                if (fs::is_regular_file(fs::path{feed.pcm}, ec) && !ec
                    && fs::file_size(fs::path{feed.pcm}, ec) > 0 && !ec)
                {
                    continue;
                }
                if (missing_count++ != 0)
                {
                    missing_names += ", ";
                    missing_pcm += ", ";
                }
                missing_names += feed.name;
                missing_pcm += feed.pcm;
            }
            if (missing_count > 0)
            {
                return std::format(
                    "module-bmi-missing: module{}{} ({}) - the recorded BMI "
                    "file(s) are not on disk; rebuild the target (or re-run cmake) "
                    "so the compile db's module flags point at existing BMIs",
                    missing_count == 1 ? " " : "s ",
                    missing_names,
                    missing_pcm);
            }
            const std::string producer =
                cg::clang::compiler_version(cc.compiler, cc.directory);
            if (!producer.empty() && !engine_token.empty()
                && producer != engine_token)
            {
                return std::format(
                    "bmi-config-mismatch [axis: pcm producer version (format "
                    "lock)]: the compile db's compiler '{}' reports clang version "
                    "'{}' but the loaded parse engine is '{}' - pcms cross-version "
                    "are rejected silently (rc=4, no diagnostics), so feeding was "
                    "refused; index with an engine matching the producer or rebuild "
                    "the tree with the engine's toolchain",
                    cc.compiler,
                    producer,
                    engine_token);
            }
            return {};
        }

        /**
         * @brief 为模块喂给 TU 分类硬 CXError_ASTReadError（rc=4：无
         * TU、无诊断）。
         *
         * 从 ccg 自记键点名差异轴：库逐条目记录的生产者版本，与消费者
         * 的 -std 相对产出被喂 pcm 的接口单元的 -std。每个比较的两侧
         * 都是 ccg 自己从编译数据库加载的事实——clang 只在驱动器模式重放中
         * 才点名该轴。非 BMI 拒绝（无模块喂给）时返回空串，保留原始错误文本。
         */
        [[nodiscard]]
        inline auto classify_ast_read_error(
            const cg::clang::CompileCommand&                    cc,
            const std::string&                                  engine_token,
            const std::unordered_map<std::string, std::string>& producer_std_of)
            -> std::string
        {
            if (cc.module_feeds.empty())
            {
                return {};
            }
            const std::string producer =
                cg::clang::compiler_version(cc.compiler, cc.directory);
            if (!producer.empty() && !engine_token.empty()
                && producer != engine_token)
            {
                return std::format(
                    "bmi-config-mismatch [axis: pcm producer version (format "
                    "lock)]: producer '{}' vs loaded engine '{}' - rebuild the "
                    "BMIs with the engine's toolchain or switch the engine",
                    producer,
                    engine_token);
            }
            const std::string consumer_std = std_of_argv(cc.argv);
            if (!consumer_std.empty())
            {
                std::string differing;
                for (const cg::clang::ModuleFeed& feed : cc.module_feeds)
                {
                    const auto it = producer_std_of.find(feed.pcm);
                    if (it == producer_std_of.end() || it->second.empty()
                        || it->second == consumer_std)
                    {
                        continue;
                    }
                    differing += std::format(
                        "[{}: producer -std={}]",
                        feed.name,
                        it->second);
                }
                if (!differing.empty())
                {
                    return std::format(
                        "bmi-config-mismatch [axis: -std]: this TU replays "
                        "-std={} but the interface units that produced its BMIs "
                        "were compiled with {} - rebuild the tree with one "
                        "language standard",
                        consumer_std,
                        differing);
                }
            }
            return std::format(
                "bmi-config-mismatch [axes from ccg's own key: engine version "
                "'{}'; producer '{}'; -std='{}'; {} BMI feeds fed]: clang rejected "
                "a pcm without diagnostics (fingerprinted config axis, "
                "from a pre-measured feasibility matrix); replay the argv "
                "with `{} -fsyntax-only` for "
                "the one-line detail",
                engine_token,
                producer.empty() ? "<unknown>" : producer,
                consumer_std.empty() ? "<none>" : consumer_std,
                cc.module_feeds.size(),
                cc.compiler.empty() ? "clang++" : cc.compiler);
        }

        /**
         * @brief 从首个诊断拼写分类模块 TU 的可恢复错误解析：分类学里高声的
         *        模块失败类（module-not-found、partition-not-fed、
         *        two-stdlib-clash）。
         *
         * 无匹配时返回 ""（保留原始首错）。
         *
         * @internal
         * two-stdlib-clash 的判定：TU 有模块投喂在场（无 pcm 则物理上不
         * 可能两套实体相撞）、错误数达 10 以上（真混合 TU 是几十条起的
         * 重定义风暴，6 是被 FooBiz 六条转换歧义证伪的旧下界）、且
         * 首错含重定义/ODR 歧义特异拼写（reference to '<名>' is
         * ambiguous——conversion/call 的重载解析歧义是方言差异，不含该
         * 前缀）/using 声明目标冲突字样。mix（混用 import std; 的 pcm 与
         * 文本包含的外来标准库头）绝不喂 std.pcm。
         */
        [[nodiscard]]
        inline auto classify_module_diag_failure(
            const std::string&               first,
            std::size_t                      error_count,
            const cg::clang::CompileCommand& cc) -> std::string
        {
            const std::string_view text{first};
            if (text.find("PCM file ") != std::string_view::npos
                && text.find("not found") != std::string_view::npos)
            {
                const std::string_view pcm = quoted_span(text, 0);
                std::string            module;
                for (const cg::clang::ModuleFeed& feed : cc.module_feeds)
                {
                    if (pcm.empty() || feed.pcm == pcm)
                    {
                        module = feed.name;
                        break;
                    }
                }
                return std::format(
                    "module-bmi-missing: module '{}' ({}) - the BMI file is not "
                    "loadable; rebuild the target",
                    module.empty() ? std::string{pcm} : module,
                    pcm.empty() ? std::string{"<unknown pcm>"}
                                : std::string{pcm});
            }
            if (const std::size_t at =
                    text.find("failed to find module file for module '");
                at != std::string_view::npos)
            {
                const std::string_view name = quoted_span(text, at);
                return std::format(
                    "partition-not-fed: module '{}' - the partition's pcm mapping "
                    "was not fed to this parse (partitions need "
                    "-fmodule-file=mod:part=... at EVERY consuming compile)",
                    name);
            }
            if (const std::size_t at = text.find("module '");
                at != std::string_view::npos
                && text.find("' not found", at) != std::string_view::npos)
            {
                const std::string_view name = quoted_span(text, at);
                return std::format(
                    "module-not-found: no BMI mapping was fed for module '{}' "
                    "(check the compile db's module flags / .modmap)",
                    name);
            }
            if (cg::clang::is_module_attachment_conflict(text))
            {
                // 归属冲突：诊断拼写自带模块名，失败文本不重复点名；
                // 工程侧修法（module; 片段 / purview 内单次包含）由
                // 共享的格式化函数携带。
                return cg::clang::module_attachment_conflict_failure(first, {});
            }
            // 前置门：一个模块投喂都没有的 TU 物理上不可能出现两套
            // 标准库实体相撞（真混合 TU 必然带着 std feed 在场）——
            // 无 feeds 时本分支整体放行，诊断落到方言分类器/原始首错。
            //
            // 阈值现状披露：本下界 10 **高于默认诊断上限**（clang.tu 的
            // diag_cap：未设 CCG_DEBUG_DIAGS 时为 8），error_count 是截断
            // 后的计数，默认运行下永远到不了 10——two-stdlib-clash 分类
            // 在默认配置中不可达。这是有意的取舍而非疏漏：真库索引调优
            // 实测大库该分类无真阳性，而拼写判据（redefinition
            // / reference-to-ambiguous / using 冲突）已把误报拦在阈值之前；
            // 阈值抬高只为挡"六条转换歧义"类假风暴（FooBiz 六条
            // 歧义证伪了旧下界 6）。真阳性要具名时经 -v 看完整首错，或设
            // CCG_DEBUG_DIAGS 解除诊断上限（diag_cap=0）后重跑。
            if (!cc.module_feeds.empty() && error_count >= 10)
            {
                // ambiguous 只认 ODR 特异拼写 reference to '<名>' is
                // ambiguous（两套实体同名打架）；conversion/call 的
                // 重载解析歧义（方言差异）不含 "reference to '" 前缀。
                const bool clash =
                    text.find("redefinition") != std::string_view::npos
                    || (text.find("reference to '") != std::string_view::npos
                        && text.find("is ambiguous") != std::string_view::npos)
                    || text.find(
                           "conflicts with target of using "
                           "declaration")
                           != std::string_view::npos;
                if (clash)
                {
                    return "two-stdlib-clash: the TU mixes `import std;` pcms with "
                           "textually included STL headers of a (foreign) stdlib - "
                           "never feed std.pcm to such a TU (a measured "
                           "failure mode)";
                }
            }
            return {};
        }

        /**
         * @brief 从首个诊断拼写分类 cl 方言的 permissive 语义失败：MSVC
         *        （默认 permissive）从不语义检查未实例化的模板体，另有
         *        个别宽松特化规则，clang 则在定义处即检查非依赖代码——
         *        四类拼写（无对象成员调用、lock_guard 构造、省略
         *        template<> 的特化、constexpr 非常量初始化）全部由此家族
         *        产生，cl 编译通过的 TU 在引擎下照样失败（下游归因取证：
         *        方言家族与模块可用性正交）。编码透传宽容（源字面量内
         *        系统 ANSI 码页不可解码字节：MSVC 原样透传、clang 报
         *        "illegal character encoding"）是同门的第五类——消毒层
         *        （内存转义重解析）跑过之后仍残留的坏点由它诚实记名。
         *
         * 与模块分类学正交：模块不可用从不产生这四类拼写，方言失败也不
         * 点名模块工件，故排在模块分类器之后兜底。翻译层为无显式模板模式
         * 旗标的 cl 条目末尾补 /Zc:twoPhase-（MSVC 默认延迟解析的 clang-cl
         * 官方等效拼写，写进 compile_commands.json 可见），未实例化模板体
         * 内的三形状由此在重放侧与真实编译对齐；此处给残余（显式
         * /permissive- 条目、延迟解析射程外的形状）诚实记名——活代码形状
         * （cl 自身也拒）、类模板 static constexpr 成员、声明级特化规则均
         * 不在延迟解析射程内，工程可用 ccg config set clang-compile-flags
         * 显式加码。仅 cl 方言条目参与（argv 带 --driver-mode=cl）；GNU
         * 方言同一拼写保持原始首错。无匹配返回 ""（保留原始首错）。
         */
        [[nodiscard]]
        inline auto classify_dialect_diag_failure(
            const std::string&               first,
            const cg::clang::CompileCommand& cc) -> std::string
        {
            if (std::ranges::find(cc.argv, "--driver-mode=cl") == cc.argv.end())
            {
                return {};
            }
            const std::string_view text{first};
            if (text.find(
                    "call to non-static member function without an object "
                    "argument")
                != std::string_view::npos)
            {
                return std::format(
                    "msvc-permissive-unchecked-member-call: {} - legal under "
                    "MSVC's delayed template parsing (never-instantiated "
                    "template bodies); live-code shapes are rejected by cl "
                    "itself (C2352), recheck the spelling at the caret",
                    first);
            }
            if (text.find(
                    "no matching constructor for initialization of "
                    "'std::lock_guard")
                != std::string_view::npos)
            {
                return std::format(
                    "msvc-permissive-unchecked-ctor: {} - constructing a "
                    "lock_guard from a mismatched argument inside a "
                    "never-instantiated template is legal under MSVC permissive "
                    "semantics",
                    first);
            }
            if (text.find("template specialization requires 'template<>'")
                != std::string_view::npos)
            {
                return std::format(
                    "msvc-permissive-lenient-specialization: {} - MSVC accepts "
                    "out-of-line definitions of class-template members without "
                    "the template<> introducer (lenient specialization rule); "
                    "delayed parsing cannot cover a declaration-level rule",
                    first);
            }
            if (text.find("must be initialized by a constant expression")
                != std::string_view::npos)
            {
                return std::format(
                    "msvc-permissive-unchecked-constexpr: {} - constexpr "
                    "initialized from a runtime value inside a template is "
                    "legal under MSVC permissive semantics; "
                    "-fdelayed-template-parsing covers function-template bodies, "
                    "class-template static constexpr members stay checked at "
                    "the definition",
                    first);
            }
            if (text.find("illegal character encoding")
                != std::string_view::npos)
            {
                return std::format(
                    "msvc-lenient-source-encoding: {} - MSVC passes unmappable "
                    "system-ANSI-code-page sequences in literals through as "
                    "raw bytes while clang decodes the input charset strictly; "
                    "the in-memory encoding scrub (octal escapes) already ran "
                    "once and did not cover the remaining site (e.g. inside a "
                    "raw string) - fix the source bytes or narrow the literal",
                    first);
            }
            return {};
        }

        /**
         * @brief 良性错误类白名单：编译器报 Error 但 AST 完好（引擎照常
         *        产出完整可走查游标树）的错误家族。
         *
         * 命中规则（classify_diag_failure_ 的失败判定前拦截）：TU 有效
         * 且全部 Error 级诊断都命中本清单且在场家族的护栏全过 → 该 TU
         * 按成功索引（不进失败类），逐 TU 注记 + 汇总计数留痕——AST 不
         * 白白扔掉，留痕形式对齐 dialect_fixit/encoding_scrub。
         *
         * 家族分两档：
         * - 窄签名：拼写即指纹（稳定子串或全形正则），放行双条件只有
         *   "签名命中 + TU 有效"。这类错误全部经过镜像对照审计：错误
         *   在场版本与修复版本的各类型声明计数逐项相等（差异为 0 或
         *   修复自带的少量新声明），错误被编译器封在单个表达式节点内，
         *   声明收集与引用遍历不受影响。
         * - 宽签名：同一措辞族里良性与恶性变体共存（恶性变体=类型本体
         *   坏掉后下游连锁报同款错），必须叠家族护栏才可放行：
         *   solo（全错同形态，级联形态在物理上凑不齐"全部同拼写"）、
         *   max_per_tu（家族条数上限，单点病灶与系统性污染的分界）、
         *   distinct_lines（点状性近似：一行多错=语句级坍塌而非每错误
         *   一个恢复表达式的点状损伤）、off_type_decl_lines（错误不落
         *   类型定义行，实体级损伤的定位特征）。
         *
         * @internal 判定用"家族拼写稳定子串"或"全形正则"——诊断含成员
         *           名/对象名等可变段，精确匹配不可行（与
         *           classify_module_diag_failure 的子串分类同风格）。
         *           新增家族只在 benign_error_families 清单追加条目，
         *           护栏按需开位，判定逻辑零改动。保守边界：清单外任何
         *           错误、任何 Fatal 级诊断（包含链中断与"错误太多"风暴
         *           的标志，AST 必然截断）仍走失败路径；TU 无效（NULL，
         *           engine-no-tu）、走查崩溃（engine-visit-crash）永不
         *           适用——绝不能把真失败洗成成功。判定输入是被诊断上限
         *           （默认 8 条）截断后的收集清单，与失败分类同一口径：
         *           max_per_tu 护栏只有在诊断上限解除（环境变量
         *           CCG_DEBUG_DIAGS）时才构成实际约束，默认口径下收集
         *           条数已被截断到 8 以内。
         */
        struct BenignErrorFamily
        {
            std::string_view family; ///< 家族名（注记与摘要点名）
            std::string_view
                needle; ///< 诊断拼写的稳定子串（pattern 为空时启用）
            std::string_view
                     pattern; ///< ECMAScript 全形正则（needle 为空时启用）
            unsigned max_per_tu = 0; ///< 家族内每 TU 错误条数上限（0=不限）
            bool     solo = false; ///< 全错同形态：在场即要求全部错误同属本家族
            bool     distinct_lines =
                false; ///< 点状性近似：本家族错误行两两不同（一行多错=语句级坍塌）
            bool off_type_decl_lines =
                false; ///< 错误行首 token 不得是类型定义关键字
        };

        inline constexpr BenignErrorFamily benign_error_families[] = {
            // 悬垂引用族：引用成员绑定生命周期更短的临时对象。
            // -Wno-everything 压不住的无旗标硬错误，但 -ast-dump 实测
            // 游标树完整产出，索引语义零损。子串同时覆盖 "shorter
            // than the lifetime of the member" 与 "...of the constructed
            // object" 两种尾部变体。
            {
                "dangling-reference-bind",
                "binds to a temporary object whose lifetime",
                {},
            },
            // const 字符指针落差族（窄签名，形状化三要素正则）：把
            // const 修饰的字符串字面量/字符指针传给裸可写字符指针形
            // 参。目标判定只认两种形状——目标本身就是裸字符指针，或
            // 目标是任意名字的别名、其 aka 展开必须是字符指针；不列
            // 举任何具体类型名，业务语料里的任何 typedef 别名一视同
            // 仁。三要素=目标形状 + aka 展开校验 + 来源必须是 const
            // 修饰的字符指针或字符数组；不满足三要素的普通参数错配
            // （如 int 形参收到 std::string）措辞相同也不放行。审计
            // 实证：错误全落在调用实参的 cv 限定转换处，被调函数声明
            // 与全部下游声明完好。
            {
                "const-char-ptr-drop",
                {},
                R"ccg(cannot initialize (?:a parameter|return object) of )ccg"
                R"ccg(type '(?:(?:wchar_t|char) \*'|[^']+' \(aka )ccg"
                R"ccg('(?:wchar_t|char)(?: const)? \*')ccg"
                R"ccg(\)) with an [lr]value of type )ccg"
                R"ccg('const (?:wchar_t|char)(?: \*|\[\d+\])')ccg",
            },
            // 右值 reinterpret_cast 族（窄签名，前缀）：对按值返回的
            // 临时量做 reinterpret_cast 到引用类型（标准要求操作数是
            // 左值，MSVC 沉默放行）。该措辞只在"目标是引用且操作数是
            // 右值"时产生，不含 reinterpret_cast 其余错误形态；每 TU
            // 单发、无连锁，恢复不产生恢复残渣声明。
            {
                "rvalue-reinterpret-cast",
                "reinterpret_cast from rvalue to reference type",
                {},
            },
            // 非 const 左值引用绑临时族（窄签名）：右值临时绑定到非
            // const 左值引用形参/被推断为引用的 auto（MSVC 扩展接受）。
            // 绑定检查在表达式层拒绝，恢复表达式兜住完整调用子树，
            // 成员调用与实参引用逐个保留，调用引用边零损失。
            {
                "nonconst-lvalue-ref-bind-temp",
                "cannot bind to a temporary",
                {},
            },
            // 依赖名缺 template 关键字族（窄签名）：两阶段解析下
            // X<...>::Name<args>(...) 缺 template 关键字被解析成比较
            // 表达式，实例化时语义查找补发此错并按模板恢复。实测每
            // TU 1-2 条且位于公共头内部，声明计数差 0.03% 量级（全部
            // 是有效实例化才有的次生模板特化）。
            {
                "missing-template-keyword",
                "missing 'template' keyword prior to dependent template name",
                {},
            },
            // 表达式层转换二义族（宽签名 + 三闸门）：某 JSON 库的 Value
            // 类型同时提供 Int/Int64 构造，long 实参两者优劣完全一样被
            // 拒。全形
            // 正则锚定头一个词 "conversion from"，与实体级二义
            // （"reference to 'X' is ambiguous"，名字查找层、语句整体
            // 被丢弃、必然级联 use-of-undeclared）在拼写与损伤层级上
            // 双重分野。solo=级联形态物理上凑不齐"全部同拼写"；条数
            // 上限 16=单点病灶（第三方库构造函数集局部特性）与系统性
            // 污染的分界；无 fatal 由 Fatal 级一刀切护栏承担。
            {
                "expr-conversion-ambiguous",
                {},
                R"ccg(^conversion from .* to .* is ambiguous$)ccg",
                16,
                true,
            },
            // std::byte 名字二义族（窄签名）：rpcndr.h 的全局 typedef
            // unsigned char byte 与 std::byte 同时在场且裸用点两者均
            // 可见时报此错。名字锚定有公开出处——rpcndr.h 是 Windows SDK
            // 系统头，byte 撞名属公开平台事实而非语料专名；该措辞形状
            // 与标准库冲突分类无法按文本分野，锚定名字是唯一窄化手段。
            // 实测错误在场版本反而多出全部 std::byte
            // 声明可供索引（宏修复路线对索引是减法），业务声明零变化。
            // 与两套标准库冲突分类的拼写判据（"reference to '" + "is
            // ambiguous"）重叠，但良性判定在前且后者需错误数下界 10
            // （默认诊断上限 8 下不可达），两路不冲突。
            {
                "byte-name-ambiguous",
                "reference to 'byte' is ambiguous",
                {},
            },
            // 类外特化缺引入符族（窄签名）：__declspec(selectany) 静态
            // 成员的类外定义缺 template<> 引入符。该诊断自带 FixIt，
            // 优先走诊断驱动改写（成功则零错误）；改写不适用（无
            // FixIt/非 cl 方言）时残余错误由此家族兜底。错误在场版本
            // 的声明计数反而略多（特化链被完整实例化）。
            {
                "template-spec-introducer",
                "template specialization requires 'template<>'",
                {},
            },
            // selectany 链接性族（窄签名）：selectany 只允许作用于具外
            // 链接的数据项，模板特化静态成员形态被拒。声明层零损伤
            // （+0.034% 为模板实例化链自带），与上一族同源场景。
            {
                "selectany-data-item",
                "'selectany' can only be applied to data items with "
                "external linkage",
                {},
            },
            // 无匹配函数形态族（窄签名，全形正则）：仅匹配
            // "no matching function '名'" 拼写（函数名紧跟引号）。
            // 硬边界：重载决议失败的主形态 "no matching function for
            // call to '名'" 及成员形态 "no matching member function
            // for call to '名'" 绝不进白名单——真致命错误（重载崩塌、
            // SFINAE 失效）与其良性变体同拼写，无法按文本分野，正则
            // 的引号位置恰好把 for-call-to 形态排除在外。
            {
                "no-matching-function-form",
                {},
                R"ccg(no matching function '.*')ccg",
            },
            // 枚举初始化隐式转换族（宽签名）：枚举类枚举器的初始化器
            // 直接用另一枚举类的值（未做 static_cast 到底层整型）。
            // 编译器恢复策略=记诊断、丢弃初始化表达式、保留枚举声明
            // 与全部枚举器（仅缺编译期常量值，索引不消费该值）；五类
            // 声明计数逐项零差，下游以该枚举为形参/成员/variant 模板
            // 实参的 10 处声明全部在场。
            {
                "enum-init-implicit-convert",
                {},
                R"ccg(value of type '.*' is not implicitly convertible to )ccg"
                R"ccg('.*')ccg",
            },
            // 无可行转换族（宽签名 + 五重护栏）：跨库字符串类之间的
            // 隐式转换链被拒（MSVC 经扩展的转换操作符链接受）。同一
            // 措辞在"类型本体坏掉"场景同样出现（模板实例化失败后所有
            // 用到它的初始化连锁报此错），五重护栏分野：TU 有效（分类
            // 器前置门）+ 家族条数 ≤10 + 点状性（每错误一个恢复表达式
            // 的近似=错误行两两不同，一行多错即语句级坍塌）+ 无 fatal
            // 伴生（Fatal 级一刀切护栏）+ 错误不落类型定义行（行首
            // token 不得是 class/struct/enum/union/template/typedef/
            // using）。
            {
                "no-viable-conversion",
                "no viable conversion from ",
                {},
                10,
                false,
                true,
                true,
            },
            // 字面量编码孤儿字节族（窄签名）：GBK 源文件的中文字面量
            // 被按 UTF-8 误读时报此错。只污染字面量内容字节，声明级
            // 索引安全（实测 11/11 函数在场）。签名收窄到 string
            // literal 形态：文件级编码错误（整文件不可解码）是另一种
            // 损伤，不并入本条。
            {
                "illegal-encoding-literal",
                "illegal character encoding in string literal",
                {},
            },
        };

        /**
         * @brief 家族正则的进程级编译缓存（表序对齐）。
         *
         * @internal needle 家族的槽位保持默认构造（空正则，永不交
         *           匹配）；首调一次编译，之后只读共享（const 匹配
         *           线程安全）。良性判定只发生在带错 TU 上（稀有路
         *           径），编译成本一次性。
         */
        [[nodiscard]]
        inline auto benign_family_regex(std::size_t index) -> const std::regex&
        {
            static const std::vector<std::regex> cache = []
            {
                std::vector<std::regex> out(std::size(benign_error_families));
                for (std::size_t k = 0; k < out.size(); ++k)
                {
                    if (!benign_error_families[k].pattern.empty())
                    {
                        out[k] = std::regex(
                            std::string{benign_error_families[k].pattern});
                    }
                }
                return out;
            }();
            return cache[index];
        }

        /** @brief 诊断拼写命中的良性家族名；未命中返回空串。 */
        [[nodiscard]]
        inline auto benign_error_family_of(const std::string& spelling)
            -> std::string_view
        {
            for (std::size_t k = 0; k < std::size(benign_error_families); ++k)
            {
                const BenignErrorFamily& fam = benign_error_families[k];
                if (!fam.needle.empty())
                {
                    if (spelling.find(fam.needle) != std::string::npos)
                    {
                        return fam.family;
                    }
                }
                else if (std::regex_search(spelling, benign_family_regex(k)))
                {
                    return fam.family;
                }
            }
            return {};
        }

        /**
         * @brief 诊断定位行是否以类型定义关键字起始。
         *
         * 宽签名家族的"错误不落类型定义行"护栏：点状表达式错误所在
         * 行的首 token 是被初始化的名字或类型名，而实体级损伤（枚举
         * 器/成员/特化的定义点坏死）落在定义行上——行首 token 是
         * class/struct/enum/union/template/typedef/using 之一。定位
         * 缺失、行号越界或文件读不到时按"在定义行"处理（保守方向：
         * 宁可拒绝良性，不放行存疑 TU）。
         *
         * @internal file_cache 缓存同一次判定内的重复文件整读（每 TU
         *           至多 8 条诊断，最坏 8 次；头文件可达兆级字节）。
         */
        [[nodiscard]]
        inline auto on_type_definition_line(
            const cg::clang::Diagnostic&                  d,
            std::unordered_map<std::string, std::string>& file_cache) -> bool
        {
            if (d.location.file.empty() || d.location.line == 0)
            {
                return true;
            }
            const auto [it, inserted] = file_cache.try_emplace(d.location.file);
            if (inserted)
            {
                it->second = cg::clang::read_source(d.location.file);
            }
            const std::string& src = it->second;
            if (src.empty())
            {
                return true; // 读不到（或物理空文件，不可能承载诊断）：存疑拒绝
            }
            std::size_t at = 0;
            unsigned    seen = 1;
            while (at < src.size() && seen < d.location.line)
            {
                if (src[at] == '\n')
                {
                    ++seen;
                }
                ++at;
            }
            if (seen < d.location.line)
            {
                return true;
            }
            const std::size_t      end = src.find('\n', at);
            const std::string_view line{
                src.data() + at,
                (end == std::string::npos ? src.size() : end) - at};
            const std::size_t b = line.find_first_not_of(" \t\r");
            if (b == std::string_view::npos)
            {
                return false; // 空白行不可能是类型定义行
            }
            const std::size_t      e = line.find_first_of(" \t\r\n<({;:", b);
            const std::string_view word = line.substr(
                b,
                (e == std::string_view::npos ? line.size() : e) - b);
            constexpr std::string_view keywords[] = {
                "class",
                "struct",
                "enum",
                "union",
                "template",
                "typedef",
                "using"};
            for (const std::string_view kw : keywords)
            {
                if (word == kw)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 全部 Error 级诊断是否都命中良性白名单且在场家族护栏
         *        全过。
         *
         * 空清单返回 false（无诊断走正常成功路径，不产生注记）；任何
         * 一条未命中、出现 Fatal 级诊断或任一护栏失败即 false（混合
         * 错误按失败处理）。Fatal 级一刀切：包含链中断与错误风暴的
         * AST 必然截断，良性语义对其不适用（这也承担各家族"无 fatal
         * 伴生"护栏）。
         */
        [[nodiscard]]
        inline auto all_errors_benign(
            const std::vector<cg::clang::Diagnostic>& errors) -> bool
        {
            if (errors.empty())
            {
                return false;
            }
            std::vector<std::string_view> matched;
            matched.reserve(errors.size());
            for (const cg::clang::Diagnostic& d : errors)
            {
                if (d.severity != cg::clang::DiagSeverity::Error)
                {
                    return false;
                }
                const std::string_view fam = benign_error_family_of(d.spelling);
                if (fam.empty())
                {
                    return false;
                }
                matched.push_back(fam);
            }
            std::unordered_map<std::string, std::string> sniff_cache;
            for (const BenignErrorFamily& fam : benign_error_families)
            {
                const std::size_t n = static_cast<std::size_t>(
                    std::ranges::count(matched, fam.family));
                if (n == 0)
                {
                    continue;
                }
                if (fam.solo && n != matched.size())
                {
                    return false; // 全错同形态：混入其他家族即拒
                }
                if (fam.max_per_tu != 0 && n > fam.max_per_tu)
                {
                    return false; // 家族条数越界=系统性污染形态
                }
                if (fam.distinct_lines || fam.off_type_decl_lines)
                {
                    for (std::size_t i = 0; i < errors.size(); ++i)
                    {
                        if (matched[i] != fam.family)
                        {
                            continue;
                        }
                        if (fam.off_type_decl_lines
                            && on_type_definition_line(errors[i], sniff_cache))
                        {
                            return false;
                        }
                        if (!fam.distinct_lines)
                        {
                            continue;
                        }
                        for (std::size_t j = i + 1; j < errors.size(); ++j)
                        {
                            if (matched[j] == fam.family
                                && errors[i].location.line
                                       == errors[j].location.line
                                && errors[i].location.file
                                       == errors[j].location.file)
                            {
                                return false; // 一行多错=语句级坍塌形态
                            }
                        }
                    }
                }
            }
            return true;
        }

        /**
         * @brief 良性注记的家族名汇总：按首现序去重、逗号连接。
         *
         * 单家族 TU 的输出与逐家族直名完全一致（既有注记格式兼容）；
         * 多家族混批（全部命中白名单）时完整点名每个在场家族。
         */
        [[nodiscard]]
        inline auto benign_error_family_summary(
            const std::vector<cg::clang::Diagnostic>& errors) -> std::string
        {
            std::string out;
            for (const cg::clang::Diagnostic& d : errors)
            {
                const std::string_view fam = benign_error_family_of(d.spelling);
                if (fam.empty() || out.find(fam) != std::string::npos)
                {
                    continue;
                }
                if (!out.empty())
                {
                    out += ", ";
                }
                out.append(fam);
            }
            return out;
        }

        /**
         * @brief 判定 TU 的 authored 文本是否携带命名模块导入。
         *
         * 喂给缺失类只适用于它们——头单元导入降级为包含、无喂给也能解析。
         * 只在无喂给 TU 的硬失败路径上运行，绝不进快乐路径。
         *
         * @internal
         * 源文本放具名缓冲 src 而非调用点临时对象：扫描器返回指向该文本
         * 内部的 string_view（悬垂 string_view 的教训，MSVC 调试堆实证）。
         */
        [[nodiscard]]
        inline auto classifies_as_feed_absent(
            const cg::clang::CompileCommand& cc) -> bool
        {
            const std::string src = cg::clang::read_source(cc.file);
            if (src.empty())
            {
                return false;
            }
            for (const auto& site : cg::clang::scan_import_decls(src))
            {
                if (!site.is_header)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 存储失败文本的命名类（模块分类学前缀，经典诊断归
         *        "parse-failed"）——摘要的聚合键。
         *
         * @internal 类清单除模块分类学外还含引擎故障类：解析引擎在遍历中途
         * 自身出错（engine-visit-crash）与防御性无 TU 契约（engine-no-tu），
         * 以及 cl 方言 permissive 语义四类（msvc-permissive- 前缀，见
         * classify_dialect_diag_failure）与 PCH 文本回退后仍失败的
         * pch-fallback-failed（与纯 pch-unusable 分账）。
         */
        [[nodiscard]]
        inline auto failure_class_of(const std::string& failure)
            -> std::string_view
        {
            constexpr std::string_view classes[] = {
                "module-bmi-missing",
                "bmi-config-mismatch",
                "module-not-found",
                "partition-not-fed",
                "two-stdlib-clash",
                "stale-cascade-crash",
                "std-bmi-build-failed",
                "module-build-failed",
                "module-dag-cycle",
                "module-attachment-conflict",
                "module-feeds-absent",
                "self-build-unavailable",
                "engine-visit-crash",
                "engine-no-tu",
                "pch-unusable",
                "pch-fallback-failed",
                "pch-missing",
                "pch-rejected",
                "pch-build-failed",
                "pch-producer-missing",
                "msvc-permissive-unchecked-member-call",
                "msvc-permissive-unchecked-ctor",
                "msvc-permissive-lenient-specialization",
                "msvc-permissive-unchecked-constexpr",
                "msvc-lenient-source-encoding"};
            for (const std::string_view cls : classes)
            {
                if (failure.starts_with(cls))
                {
                    return cls;
                }
            }
            return "parse-failed";
        }

        /**
         * @brief 瞬时失败类：重试有意义的命名失败（其余类在输入不变时必然
         *        以同一原因再失败）。
         *
         * @internal
         * 模块构建/自建失败、引擎走查崩溃与过期 BMI 级联崩溃都可能由一次
         * 短暂的外部状态造成（并发构建、过期工件）；其余模块分类学与
         * parse-failed 视为"输入不变即确定"，只由世界状态变化或 --force
         * 触发重试。
         */
        [[nodiscard]]
        inline auto is_transient_failure_class(std::string_view cls) -> bool
        {
            return cls == "std-bmi-build-failed" || cls == "module-build-failed"
                   || cls == "self-build-unavailable"
                   || cls == "stale-cascade-crash"
                   || cls == "engine-visit-crash" || cls == "pch-build-failed"
                   || cls == "pch-rejected" || cls == "pch-missing";
        }

        /**
         * @brief 当前已加载引擎的版本记号；引擎尚未加载/不可用时为空串。
         *
         * @internal
         * 首次解析前（索引器自己的工人池还没构造第一个 Index）进程内可能
         * 尚无引擎；失败记录写入发生在池运行之后，因此那里能取到真实记号。
         * 空串是"未知"，与另一侧比较时按不相等处理会把失败条目重试一次。
         */
        [[nodiscard]]
        inline auto engine_token_now() -> std::string
        {
            try
            {
                return cg::clang::clang_version_token(
                    cg::clang::libclang_version());
            }
            catch (const std::exception&)
            {
                return {};
            }
        }

        /** @brief 瞬时失败的指数退避：60s·2^(attempts-1)，封顶 30min。 */
        [[nodiscard]]
        inline auto retry_delay_ms(std::int64_t attempts) -> std::int64_t
        {
            const std::int64_t shift =
                std::clamp<std::int64_t>(attempts - 1, 0, 5);
            constexpr std::int64_t cap = 30 * 60 * 1000;
            return std::min<std::int64_t>((60 * 1000) << shift, cap);
        }

        /**
         * @brief 抽取根集摘要：roots 影响 under_roots 过滤，进而影响符号集。
         *
         * @internal 逐元素"长度前缀+冒号+内容+换行"再 sha256 的帧法与
         * argv_digest 完全同一，直接转发——编码规则只有单点可动，存量库
         * 的摘要值逐字节不变。
         */
        [[nodiscard]]
        inline auto roots_digest_of(const std::vector<std::string>& roots)
            -> std::string
        {
            return argv_digest(roots);
        }

        /**
         * @brief 模块 feed 代际摘要：有序 (name, pcm 路径, pcm stat) + 生产者
         *        编译器版本 + module_output 身份。
         *
         * @internal
         * 任一 BMI 重建、消失或生产者版本变化都会改变本摘要，从而让上一次
         * 失败的模块 TU 重试；parse_digest 已含 module-overlay 纪元，无需
         * 重复。
         */
        [[nodiscard]]
        inline auto feed_digest_of(const cg::clang::CompileCommand& cmd)
            -> std::string
        {
            std::string buf;
            for (const cg::clang::ModuleFeed& feed : cmd.module_feeds)
            {
                buf += feed.name;
                buf.push_back('\0');
                buf += cg::util::norm_path(feed.pcm);
                buf.push_back('\0');
                const FileStat st = stat_file(feed.pcm);
                buf += std::to_string(st.mtime_ns);
                buf.push_back(':');
                buf += std::to_string(st.size);
                buf.push_back('\n');
            }
            buf += cg::clang::compiler_version(cmd.compiler, cmd.directory);
            buf.push_back('\0');
            buf += cmd.module_output;
            return cg::util::sha256_hex(std::as_bytes(std::span{buf}));
        }

        /**
         * @brief 追加一条失败依赖记录：`path\tmtime\tsize\n`（size=-1 表示失败
         *        时该依赖不在盘上）。
         */
        inline void encode_dep(
            std::string&     out,
            std::string_view path,
            const FileStat&  st)
        {
            out += path;
            out.push_back('\t');
            out += std::to_string(st.ok ? st.mtime_ns : -1);
            out.push_back('\t');
            out += std::to_string(st.ok ? st.size : -1);
            out.push_back('\n');
        }

        /**
         * @brief 失败记录的依赖闭包是否发生变化（任一条目的 stat 身份/存在性
         *        改变即真）。
         *
         * @internal 失败 TU 的 includes 行在失败时已被清除，不能靠 includes
         * 表传播；deps_json 是失败瞬间快照的闭包路径与 stat 身份。缺失条目
         * 以 -1 记录：依赖重新出现（例如被修好的头文件）也必须触发重试。
         */
        [[nodiscard]]
        inline auto deps_changed(std::string_view deps_json) -> bool
        {
            while (!deps_json.empty())
            {
                const std::size_t      nl = deps_json.find('\n');
                const std::string_view line = deps_json.substr(
                    0,
                    nl == std::string_view::npos ? deps_json.size() : nl);
                if (!line.empty())
                {
                    const std::size_t t1 = line.find('\t');
                    const std::size_t t2 = t1 == std::string_view::npos
                                               ? std::string_view::npos
                                               : line.find('\t', t1 + 1);
                    if (t1 == std::string_view::npos
                        || t2 == std::string_view::npos)
                    {
                        return true; // 损坏记录：宁可重试
                    }
                    const std::string path{line.substr(0, t1)};
                    std::int64_t      mtime = 0;
                    const auto mtime_text = line.substr(t1 + 1, t2 - t1 - 1);
                    const auto size_text = line.substr(t2 + 1);
                    const auto parsed = std::from_chars(
                        mtime_text.data(),
                        mtime_text.data() + mtime_text.size(),
                        mtime);
                    std::int64_t parsed_size = 0;
                    const auto   parsed2 = std::from_chars(
                        size_text.data(),
                        size_text.data() + size_text.size(),
                        parsed_size);
                    if (parsed.ec != std::errc{} || parsed2.ec != std::errc{})
                    {
                        return true;
                    }
                    const FileStat st = stat_file(path);
                    if (!st.ok)
                    {
                        if (mtime != -1)
                        {
                            return true; // 依赖消失了
                        }
                    }
                    else if (
                        mtime == -1 || st.mtime_ns != mtime
                        || st.size != parsed_size)
                    {
                        return true; // 依赖出现或身份变了
                    }
                }
                if (nl == std::string_view::npos)
                {
                    break;
                }
                deps_json.remove_prefix(nl + 1);
            }
            return false;
        }

        /** @brief deps_json 中的依赖路径（每行第一个制表符之前）。 */
        [[nodiscard]]
        inline auto dep_paths(std::string_view deps_json)
            -> std::vector<std::string_view>
        {
            std::vector<std::string_view> out;
            while (!deps_json.empty())
            {
                const std::size_t      nl = deps_json.find('\n');
                const std::string_view line = deps_json.substr(
                    0,
                    nl == std::string_view::npos ? deps_json.size() : nl);
                const std::size_t tab = line.find('\t');
                if (tab != std::string_view::npos && tab != 0)
                {
                    out.push_back(line.substr(0, tab));
                }
                if (nl == std::string_view::npos)
                {
                    break;
                }
                deps_json.remove_prefix(nl + 1);
            }
            return out;
        }

        /**
         * @brief 合并失败记录的依赖闭包：本次快照优先，旧闭包中本次未出现的
         *        路径保留，并按当前 stat 重新编码。
         *
         * @internal 失败 TU 的 includes 行在失败瞬间被清除，因此同一
         * (file_id, cmd_digest) 第二次及以后的失败通常算不出闭包（fresh 为
         * 空）。若直接覆盖，"之后修复被依赖的头文件"将永远无法把该 TU 拉进
         * reached——失败记录退化成永久失败。故：空快照保留旧路径集合；有新增
         * 则取并集（同一路径以本次记录为准）。保留的旧路径按当前 stat 重新
         * 编码：本次重试正是"依赖已变"的结果，记录必须前进到这次失败时的
         * 世界状态，否则旧 stat 会永久失配、让该 TU 每次运行都重试。
         */
        [[nodiscard]]
        inline auto merge_deps(std::string_view prior, std::string_view fresh)
            -> std::string
        {
            if (prior.empty())
            {
                return std::string{fresh};
            }
            std::string                          out;
            std::unordered_set<std::string_view> paths;
            if (!fresh.empty())
            {
                while (!fresh.empty())
                {
                    const std::size_t      nl = fresh.find('\n');
                    const std::string_view line = fresh.substr(
                        0,
                        nl == std::string_view::npos ? fresh.size() : nl);
                    const std::size_t tab = line.find('\t');
                    if (tab != std::string_view::npos && tab != 0
                        && paths.insert(line.substr(0, tab)).second)
                    {
                        out.append(line);
                        out.push_back('\n');
                    }
                    if (nl == std::string_view::npos)
                    {
                        break;
                    }
                    fresh.remove_prefix(nl + 1);
                }
            }
            for (const std::string_view path : dep_paths(prior))
            {
                if (!paths.insert(path).second)
                {
                    continue;
                }
                const std::string dep{path};
                encode_dep(out, dep, stat_file(dep));
            }
            return out;
        }

        /**
         * @brief 孤儿清扫的决策核：给定状态上经包含图无任何编译数据库主文件
         *        可达的每个文件。
         *
         * @internal
         * 由已提交清扫（批次后在事务内回读行）与干跑计划（快照加本运行提取
         * 模拟同一状态）共享，使计划的 purgedFiles 计数按构造等于已提交值，
         * 而非依赖一份会漂移的平行重实现。包含者行不在状态内的链接直接跳过。
         */
        [[nodiscard]]
        inline auto unreachable_files(
            const std::vector<std::pair<cg::db::FileId, std::string>>& files,
            const std::vector<std::pair<cg::db::FileId, std::string>>& links,
            const std::vector<cg::db::FileId>&                         roots)
            -> std::unordered_set<cg::db::FileId>
        {
            IncludeResolver                                 current;
            std::unordered_map<cg::db::FileId, std::string> paths;
            for (const auto& [id, path] : files)
            {
                current.add(path, id);
                paths.emplace(id, path);
            }
            std::unordered_map<cg::db::FileId, std::vector<cg::db::FileId>> fwd;
            for (const auto& [includer, target] : links)
            {
                const auto it = paths.find(includer);
                if (it == paths.end())
                {
                    continue;
                }
                for (const cg::db::FileId f :
                     current.resolve(target, it->second))
                {
                    fwd[includer].push_back(f);
                }
            }
            std::unordered_set<cg::db::FileId> reachable{
                roots.begin(),
                roots.end()};
            std::vector<cg::db::FileId> queue{roots.begin(), roots.end()};
            while (!queue.empty())
            {
                const cg::db::FileId f = queue.back();
                queue.pop_back();
                if (const auto it = fwd.find(f); it != fwd.end())
                {
                    for (const cg::db::FileId t : it->second)
                    {
                        if (reachable.insert(t).second)
                        {
                            queue.push_back(t);
                        }
                    }
                }
            }
            std::unordered_set<cg::db::FileId> out;
            for (const auto& [id, path] : files)
            {
                if (!reachable.contains(id))
                {
                    out.insert(id);
                }
            }
            return out;
        }

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
                const auto  trace_bucket = [&](std::string_view   verdict,
                                               const std::string& bucket_name,
                                               std::size_t        pre_fn_n,
                                               std::size_t        post_fn_n,
                                               std::size_t        pre_fn_tpl_n,
                                               std::size_t        post_fn_tpl_n,
                                               std::size_t        pre_any_n,
                                               std::size_t        post_any_n,
                                               const Entry&       survivor)
                {
                    if (!trace_purge)
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
                };
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
                    // 终态复核闸（假触发否决）：瞬态 doomed
                    // 集把桶清到单候选只是升级的必要条件；桶若在本批终态
                    // （新提取并集 ∪ 重写集内未变文件的完整重建）原样回来，
                    // 升级要抢救的「新可解析调用点」不存在——否决并继续扫
                    // 其它桶。回调缺席（空函数）= 复核关闭，行为与旧版一致。
                    const auto terminal_lift_genuine = [&]() -> bool
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
                    };
                    if (pre_fn >= 2
                        && (post_fn == 1
                            || (post_fn >= 2 && pre_fn_tpl >= 2
                                && post_fn_tpl == 1)))
                    {
                        const bool genuine = terminal_lift_genuine();
                        trace_bucket(
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
                        const bool genuine = terminal_lift_genuine();
                        trace_bucket(
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
             * confidence 记录获胜层级（1.0 / 0.7 / 0.4）。
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
             *   时可选；三层全部沿用同一裁决，域内无候选则按序降级直至
             *   不解析。
             * - soundness 边界：ODR 违例的双外部定义会被同判为孪生——选本
             *   TU 的那份不劣于原先的桶序首个；单一文件内的 static 与匿名
             *   ns 同名同签名共存时二者皆在域内，桶序裁决（同为合法目标）。
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
                double&                                   confidence,
                const std::unordered_set<cg::db::FileId>* tu_scope) const
                -> std::optional<cg::db::SymId>
            {
                const auto it = by_name_.find(meta.name);
                if (it == by_name_.end())
                {
                    return std::nullopt;
                }
                const std::vector<Entry>& cands = it->second;

                const auto kind_ok = [&](const Entry& e)
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
                };

                // 孪生判据：同身份（父类/命名空间/签名/const/函数形类别）在
                // 别的文件另有一份定义。只看未 retire 的候选。仅对域外候选
                // 求值（域内恒可选、tu_scope 为空时短路），最坏 O(bucket)，
                // 与各层主扫同阶。
                const auto twin_elsewhere = [&](const Entry& e)
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
                };
                // TU 域裁决：域内候选恒可选；域外候选仅当无孪生（单一文件
                // 身份 = 外部链接或头文件共享行）时保留——外部函数的跨文件
                // 解析（TU A 调用 lib.cpp 的定义）不能被误伤。
                const auto eligible = [&](const Entry& e)
                {
                    return tu_scope == nullptr || tu_scope->contains(e.file_id)
                           || !twin_elsewhere(e);
                };

                for (const Entry& e : cands)
                {
                    if (retired_.contains(e.id) || !kind_ok(e) || !eligible(e))
                    {
                        continue;
                    }
                    if (e.parent != meta.parent || e.ns != meta.ns
                        || e.sig_hash != meta.sig_hash
                        || e.const_method != meta.const_method)
                    {
                        continue;
                    }
                    confidence = 1.0;
                    return e.id;
                }
                // 重载归属裁决（名字段之前的窄闸）：同名候选里"非模板+
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
                        if (retired_.contains(e.id) || !kind_ok(e)
                            || !eligible(e) || e.parent != meta.parent
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
                        confidence = 0.75;
                        return sole_tmpl;
                    }
                }
                for (const Entry& e : cands)
                {
                    if (retired_.contains(e.id) || !kind_ok(e) || !eligible(e))
                    {
                        continue;
                    }
                    if (e.parent != meta.parent
                        || !cg::analysis::ns_tail_match(e.ns, meta.ns))
                    {
                        continue;
                    }
                    confidence = 0.7;
                    return e.id;
                }
                std::optional<cg::db::SymId> only;
                std::size_t                  hits = 0;
                std::size_t                  template_hits = 0;
                std::optional<cg::db::SymId> sole_template;
                for (const Entry& e : cands)
                {
                    if (retired_.contains(e.id) || !kind_ok(e) || !eligible(e))
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
                    confidence = 0.4;
                    return only;
                }
                if (hits > 1 && sole_template.has_value()
                    && !meta.sig_hash.has_value())
                {
                    confidence = 0.4;
                    return sole_template;
                }
                return std::nullopt;
            }

        private:
            bool                                                loaded_ = false;
            std::unordered_set<cg::db::SymId>                   retired_;
            std::unordered_map<std::string, cg::db::SymId>      by_key_;
            std::unordered_map<std::string, std::vector<Entry>> by_name_;
        };

        /**
         * @brief extends/overrides/specializes 边集摘要（边变化检测的依据）。
         *
         * @internal (src, dst, type) 全序拼接后 sha256：任何一条继承/覆盖/特
         *           化边的增删都翻转摘要。dispatch 家族缓存只在运行内，库内
         *           无从按家族增量——摘要翻转即"家族结构变化"的诚实下界，
         *           触发全站点升级（接口头巨族由 resolve 的家族级缓存消化）。
         *           Specializes 必须在列：dispatch 的 spec_fwd_/spec_down_ 折
         *           算与 propagate 的派生槽都消费特化边，净特化变化而无
         *           extends/overrides 净变化的批若摘要不翻转，会静默保留按
         *           旧折算结构算出的过期派发边（spec-digest-flip 夹具锁死）。
         */
        [[nodiscard]]
        inline auto hierarchy_digest(cg::db::GraphStore& store) -> std::string
        {
            std::vector<std::string> parts;
            for (const cg::db::EdgeType type :
                 {cg::db::EdgeType::Extends,
                  cg::db::EdgeType::Overrides,
                  cg::db::EdgeType::Specializes})
            {
                for (const cg::db::EdgeRow& e :
                     cg::db::edges_of_type(store, type))
                {
                    parts.push_back(
                        std::format(
                            "{}|{}|{}",
                            e.src,
                            e.dst,
                            static_cast<int>(e.type)));
                }
            }
            std::ranges::sort(parts);
            std::string joined;
            for (const std::string& p : parts)
            {
                joined += p;
                joined.push_back('\n');
            }
            return cg::util::sha256_hex(std::as_bytes(std::span{joined}));
        }

        /**
         * @brief `#external` 行集摘要：RTA 输出变化时 dispatch 的候选排序/标注
         *        全局重排，与层级摘要同一升级判据。
         */
        [[nodiscard]]
        inline auto external_digest(cg::db::GraphStore& store) -> std::string
        {
            std::vector<std::string> parts;
            for (const cg::db::TypeHintRow& row : store.all_type_hints())
            {
                if (row.source
                    == static_cast<int>(cg::analysis::HintSource::External))
                {
                    parts.push_back(
                        std::format("{}|{}", row.owner_id, row.type_set));
                }
            }
            std::ranges::sort(parts);
            std::string joined;
            for (const std::string& p : parts)
            {
                joined += p;
                joined.push_back('\n');
            }
            return cg::util::sha256_hex(std::as_bytes(std::span{joined}));
        }

        /**
         * @brief 全局传播固定点 + dispatch 解析 +
         * 写回的提交段序列（唯一实现）。
         *
         * @internal 两个入口共用：普通索引/update 批（Indexer::run 的提交事务
         * 内，阶段 A/B 行已就位）与收敛批（converge_dispatch，编辑批延迟
         * 后的独立触发——延迟收敛就是又一个 update 批次）。环境差异经 ctx
         * 注入：ukey->SymId 与 path->FileId 解析（批内先查内存映射、收敛批直
         * 接查库）、心跳回调（批内是提交事务内的租约心跳；收敛批调用方在长
         * 计算开始前续约，与 update 路径既有批纪律一致）。序列与 meta 契约
         * （analysis_erase/rta/propagate/dispatch/writeback 五计时键 +
         * rounds/work_items/external_rta/budget_exhausted）不变。尾部清
         * dispatch_stale 双键：全量重算完成后派发数据与已提交行必然一致。
         */
        struct DispatchCommitCtx
        {
            cg::db::GraphStore*             store = nullptr;
            const std::vector<std::string>* roots = nullptr;
            IndexSummary*                   sum = nullptr;
            /**
             * @brief 库文件路径（跨提交驻留的库身份键）。
             * @internal 空串（内存库等）禁用驻留——无文件身份的库每次全表读。
             */
            std::string db_path;
            /** @brief unique_key -> sym_id（解析不到 = nullopt，该边跳过）。 */
            std::function<std::optional<cg::db::SymId>(std::string_view)>
                owner_of;
            /** @brief 站点文件路径 -> file_id（收敛批走库查，语义与批内一致）。
             */
            std::function<cg::db::FileId(const std::string&)> file_of;
            /** @brief 长计算段前的心跳钩子（无协调时为空操作）。 */
            std::function<void()> beat;
            // ---- 增量输入：批内捕获（编辑批的 store 阶段就地收集）；
            // 延迟批落盘的 meta 捕获（dispatch_prev_*）在 dispatch_commit 内
            // 与这里取并集。----
            /** @brief 本批脏文件（重写/新增/失败/缺失；收敛批由 stale meta
             * 解析）。 */
            const std::unordered_set<cg::db::FileId>* dirty_files = nullptr;
            /** @brief 脏文件被删站点行的旧 (owner_key, var)
             * 槽键（被删行下游）。 */
            const std::vector<std::pair<std::string, std::string>>* prev_slots =
                nullptr;
            /** @brief 脏文件被删站点行的旧被调用者键（评审红线）。 */
            const std::vector<std::string>* prev_callees = nullptr;
            /** @brief 脏文件已删除符号的 unique_key。 */
            const std::vector<std::string>* doomed_keys = nullptr;
            /** @brief 脏文件已删除符号的非限定名（`fn` 消费者保守匹配）。 */
            const std::vector<std::string>* doomed_names = nullptr;
            /**
             * @brief 名字桶真触发（复核闸放行）的升级腿。
             *
             * @internal 与 hierarchy/external 变化同一既有升级开关：置位
             *           则 dispatch 全站点决议重做+派发边整类清空重写。
             *           延迟批经 meta `dispatch_name_escalation` 跨批传
             *           递（编辑批落盘、收敛批读到即消费清除）。
             */
            bool name_escalation = false;
        };

        /**
         * @brief indirect_calls 边的整库重算
         * （消费分流：实例桥/形参直配/槽）。
         *
         * @internal 三路 join 发 type=9 边，先清后发（正确性优先；边量=间接
         *           点×小目标集）。分辨率两档：exact（目标集唯一）/candidate
         *           （有界多目标每目标一条，GCC speculative 同构）；置信自定
         *           档 0.8/0.7（经验初值）。追不出的站点保留 indirect
         *           降级行（fallback_reason 已在），不硬连——宁漏勿猜。
         */
        inline void indirect_commit(
            const DispatchCommitCtx&              ctx,
            const cg::analysis::PropagationState& state,
            const std::vector<cg::db::IndexRow>*  rows,
            const std::unordered_map<
                std::string,
                cg::db::SymId,
                cg::util::string_hash,
                std::equal_to<>>* id_of_key)
        {
            cg::db::GraphStore& store = *ctx.store;
            store.erase_edges_of_type(cg::db::EdgeType::IndirectCalls);
            // 键预解析（与 dispatch_commit 同一 map）：发射循环内的目标键
            // 查询零库访问；miss 兜底原查询。
            const auto owner_of = [&ctx, id_of_key](std::string_view key)
                -> std::optional<cg::db::SymId>
            {
                if (id_of_key != nullptr)
                {
                    if (const auto it = id_of_key->find(key);
                        it != id_of_key->end())
                    {
                        return it->second;
                    }
                }
                return ctx.owner_of(key);
            };
            const auto emit = [&](cg::db::SymId  src,
                                  cg::db::SymId  dst,
                                  cg::db::FileId file,
                                  int            line,
                                  bool           exact)
            {
                store.upsert_edge(
                    {.src = src,
                     .dst = dst,
                     .type = cg::db::EdgeType::IndirectCalls,
                     .site_file = file,
                     .site_line = line,
                     .attrs = exact ? cg::db::edge_attrs::indirect_exact
                                    : cg::db::edge_attrs::indirect_candidate,
                     .confidence = exact ? 0.8 : 0.7,
                     .occurrences = 1});
            };
            // ---- 路一：实例桥（external_param_args × external_invokes）----
            {
                const std::vector<cg::db::ExternalParamArgRow> args =
                    store.all_external_param_args();
                const std::vector<cg::db::ExternalInvokeRow> invs =
                    store.all_external_invokes();
                std::unordered_map<
                    std::string,
                    std::vector<const cg::db::ExternalParamArgRow*>>
                    by_param;
                for (const auto& a : args)
                {
                    by_param[std::format("{}#{}", a.host_usr, a.param_index)]
                        .push_back(&a);
                }
                // 级联解析：转发占位 "ext#`<host>`#`<param>`" 的真实目标集 =
                // 该 (host,param) 上全部条目目标（可能再嵌 ext#——不动点到
                // 收敛；深度 16 + 访问集防环，环上目标集为空=宁漏勿猜）。
                const auto resolve_targets =
                    [&](const std::string&        key,
                        std::vector<std::string>& out_targets)
                {
                    std::vector<std::pair<std::string, int>> stack;
                    std::unordered_set<std::string>          seen;
                    const auto push_ext = [&](const std::string& tk)
                    {
                        // 形如 ext#`<host_usr>`#`<i>`；host_usr 可含
                        // '#'，取最后 一个 '#' 前的整段。
                        const std::size_t h = tk.rfind('#');
                        if (h == std::string::npos || h <= 4)
                        {
                            return;
                        }
                        const std::string host = tk.substr(4, h - 4);
                        int               pi = -1;
                        if (std::from_chars(
                                tk.data() + h + 1,
                                tk.data() + tk.size(),
                                pi)
                                .ec
                            != std::errc{})
                        {
                            return;
                        }
                        if (!seen.insert(tk).second)
                        {
                            return;
                        }
                        stack.emplace_back(std::format("{}#{}", host, pi), 0);
                    };
                    push_ext(key);
                    for (int iter = 0; iter < 16 && !stack.empty(); ++iter)
                    {
                        auto [pk, depth] = stack.back();
                        stack.pop_back();
                        const auto it = by_param.find(pk);
                        if (it == by_param.end())
                        {
                            continue;
                        }
                        if (depth >= 16)
                        {
                            continue;
                        }
                        for (const auto* a : it->second)
                        {
                            if (a->target_key.starts_with("ext#"))
                            {
                                push_ext(a->target_key);
                            }
                            else if (!a->target_key.empty())
                            {
                                out_targets.push_back(a->target_key);
                            }
                        }
                    }
                };
                // 界外目标文件回查表：invs 每条都可能需要目标符号的所在
                // 文件做 edges.site_file 的第三级 fallback——表只建一次，
                // 循环外物化（调用方已物化符号投影时直接复用，免第二次
                // 全表扫描）。
                std::vector<cg::db::IndexRow> owned_rows;
                if (rows == nullptr)
                {
                    owned_rows = cg::db::all_symbol_index_rows(store);
                    rows = &owned_rows;
                }
                std::unordered_map<cg::db::SymId, cg::db::FileId> sym_file;
                for (const cg::db::IndexRow& row : *rows)
                {
                    sym_file.try_emplace(row.id, row.file_id);
                }
                for (const auto& inv : invs)
                {
                    const auto it = by_param.find(
                        std::format("{}#{}", inv.host_usr, inv.param_index));
                    if (it == by_param.end())
                    {
                        continue;
                    }
                    // 展开转发占位后的（调用点, 目标）对：占位解析出的目标
                    // 继承首个直传调用点（链头发起者）作为 src。
                    struct ResolvedEdge
                    {
                        std::string    caller_key;
                        std::string    target_key;
                        cg::db::FileId site_file = 0;
                        int            site_line = 0;
                    };
                    std::vector<ResolvedEdge> edges;
                    for (const auto* a : it->second)
                    {
                        if (a->target_key.starts_with("ext#"))
                        {
                            std::vector<std::string> sub;
                            resolve_targets(a->target_key, sub);
                            // 沿转发链向上回溯到链头的直传 caller（占位可能
                            // 多层嵌套：每层占位指向的 (host,param) 条目里
                            // caller_key 非空者即该层的直传头）。
                            struct Head
                            {
                                std::string    caller;
                                cg::db::FileId file = 0;
                                int            line = 0;
                            };
                            std::vector<Head> heads;
                            std::string       cur = a->target_key;
                            for (int up = 0; up < 16 && cur.starts_with("ext#");
                                 ++up)
                            {
                                const std::size_t h = cur.rfind('#');
                                if (h == std::string::npos || h <= 4)
                                {
                                    break;
                                }
                                const std::string host = cur.substr(4, h - 4);
                                int               pi = -1;
                                if (std::from_chars(
                                        cur.data() + h + 1,
                                        cur.data() + cur.size(),
                                        pi)
                                        .ec
                                    != std::errc{})
                                {
                                    break;
                                }
                                const auto hit = by_param.find(
                                    std::format("{}#{}", host, pi));
                                if (hit == by_param.end())
                                {
                                    break;
                                }
                                std::string next;
                                bool        found_head = false;
                                for (const auto* hd : hit->second)
                                {
                                    if (!hd->caller_key.empty())
                                    {
                                        heads.push_back(
                                            {hd->caller_key,
                                             static_cast<cg::db::FileId>(
                                                 hd->file_id),
                                             hd->line});
                                        found_head = true;
                                    }
                                    else if (
                                        hd->target_key.starts_with("ext#")
                                        && next.empty())
                                    {
                                        next = hd->target_key;
                                    }
                                }
                                if (found_head || next.empty())
                                {
                                    break;
                                }
                                cur = next;
                            }
                            for (const std::string& t : sub)
                            {
                                for (const Head& head : heads)
                                {
                                    edges.push_back(
                                        {head.caller,
                                         t,
                                         head.file != 0
                                             ? head.file
                                             : static_cast<cg::db::FileId>(
                                                   a->file_id),
                                         head.line != 0 ? head.line : a->line});
                                }
                            }
                        }
                        else if (!a->caller_key.empty())
                        {
                            edges.push_back(
                                {a->caller_key,
                                 a->target_key,
                                 static_cast<cg::db::FileId>(a->file_id),
                                 a->line});
                        }
                    }
                    std::vector<std::string> distinct;
                    for (const auto& e : edges)
                    {
                        distinct.push_back(e.target_key);
                    }
                    std::ranges::sort(distinct);
                    distinct.erase(
                        std::unique(distinct.begin(), distinct.end()),
                        distinct.end());
                    const bool unique_target = distinct.size() == 1;
                    // 界外字段槽场景 heads 可空（make_op 类调用点实参无
                    // 函数目标、无直传 head）→ site_file 与 inv.file_id 双 0，
                    // edges.site_file 的 FK 不收 0（实测同族）——第三级
                    // fallback：挂目标符号的 file（界外 lambda 已挂靠 TU 主
                    // 文件，界内目标即自身文件）；查表 sym_file（循环外建）。
                    for (const auto& e : edges)
                    {
                        const auto src = owner_of(e.caller_key);
                        const auto dst = owner_of(e.target_key);
                        if (!src.has_value() || !dst.has_value())
                        {
                            continue;
                        }
                        // 站点取用户调用点（param_arg 的 file/line）：语义比
                        // 界外 invoke 位更好（边指向用户写的注册行），且
                        // edges.site_file 的 FK 在界外头无行、inv.file_id=0 会
                        // 违规——实测 4 行。
                        cg::db::FileId f =
                            e.site_file != 0
                                ? e.site_file
                                : static_cast<cg::db::FileId>(inv.file_id);
                        if (f == 0)
                        {
                            const auto fit = sym_file.find(*dst);
                            if (fit != sym_file.end() && fit->second != 0)
                            {
                                f = fit->second;
                            }
                            else
                            {
                                // dst 亦无界内锚（不该发生：lambda 挂靠保证非
                                // 0）——宁漏勿猜，弃边不炸库。
                                continue;
                            }
                        }
                        emit(
                            *src,
                            *dst,
                            f,
                            e.site_line != 0 ? e.site_line : inv.line,
                            unique_target);
                    }
                }
            }
            // ---- 槽索引（内存终态；只留含函数/方法目标的槽）----
            std::unordered_map<std::string, std::vector<std::string>>
                slot_types;
            for (const cg::analysis::PropagatedHint& h : state.hints)
            {
                bool has_fn = false;
                for (const std::string& k : h.types)
                {
                    if (k.starts_with("function|") || k.starts_with("method|"))
                    {
                        has_fn = true;
                        break;
                    }
                }
                if (!has_fn)
                {
                    continue;
                }
                slot_types.emplace(
                    cg::util::compose_key(h.owner_key, h.var_name),
                    h.types);
            }
            // ---- 形参直配数据源：按【被调者】索引的 arg_targets ----
            // （"调用 runner 的人传了什么"：key=runner 的 sym_id）
            std::unordered_map<
                cg::db::SymId,
                std::vector<std::vector<std::string>>>
                callee_targets;
            for (auto& [cid, json] : cg::db::arg_target_rows(store))
            {
                callee_targets[cid].push_back(
                    cg::analysis::parse_string_array(json));
            }
            // ---- 路二/三：间接站点消费（形参直配 + 槽）----
            for (const cg::db::IndirectSiteRow& site :
                 cg::db::indirect_site_rows(store))
            {
                ctx.beat();
                std::vector<std::string> targets;
                if (site.callee_param >= 0)
                {
                    // 形参直配：该函数所有调用点的第 N 实参目标并集。
                    const auto it = callee_targets.find(site.caller_id);
                    if (it != callee_targets.end())
                    {
                        for (const std::vector<std::string>& args : it->second)
                        {
                            if (static_cast<std::size_t>(site.callee_param)
                                < args.size())
                            {
                                const std::string& t =
                                    args[static_cast<std::size_t>(
                                        site.callee_param)];
                                if (!t.empty())
                                {
                                    targets.push_back(t);
                                }
                            }
                        }
                    }
                }
                else
                {
                    // 槽消费：字段槽（receiver_owner_key）优先，否则局部变量槽
                    // （caller_key）。类型集里的 function|/method| 键即目标。
                    const std::string owner = !site.receiver_owner_key.empty()
                                                  ? site.receiver_owner_key
                                                  : site.caller_key;
                    const auto        it = slot_types.find(
                        cg::util::compose_key(owner, site.callee_expr));
                    if (it != slot_types.end())
                    {
                        for (const std::string& k : it->second)
                        {
                            if (k.starts_with("function|")
                                || k.starts_with("method|"))
                            {
                                targets.push_back(k);
                            }
                        }
                    }
                }
                if (targets.empty())
                {
                    continue;
                }
                std::ranges::sort(targets);
                targets.erase(
                    std::unique(targets.begin(), targets.end()),
                    targets.end());
                (void)cg::analysis::slot_target_cap_truncate(targets);
                const bool exact = targets.size() == 1;
                for (const std::string& t : targets)
                {
                    const auto dst = owner_of(t);
                    if (!dst.has_value())
                    {
                        continue;
                    }
                    emit(site.caller_id, *dst, site.file_id, site.line, exact);
                }
            }
        }

        /**
         * @brief analysis_slots 终态的跨提交驻留（同进程连续提交段间的 warm
         *        装载免全表读）。
         *
         * @internal 持有权随 update_claim 租约：claim 快照活着且持有者与库身份
         *           都连续时命中（同一 update 持有者的连续编辑批），快照持有
         *           者变化（失效/被夺/新持有者）即弃。analysis_slots 的写入方
         *           唯一是本提交段（整表清空/逐行 upsert/逐键删除三口），驻留
         *           在每次写回后按本批终态同步，下一批的 warm 装载与全表读恒
         *           等。并发写库被 claim 协议互斥（提交事务持写锁，偷 claim 的
         *           BEGIN IMMEDIATE 被 busy 挡死）；协议外的直写属越权场景，
         *           与既有 warm 持久化信任同级。进程级单例：分析提交段单线程，
         *           驻留随进程结束消亡。
         */
        struct AnalysisSlotsResidency
        {
            bool        inited = false;
            std::string db_path;
            std::string holder;
            std::unordered_map<std::string, cg::db::AnalysisSlotRow> rows;
        };

        /**
         * @brief dispatch_commit 的提交后输出（调用方在事务提交成功后消费）。
         *
         * @internal 快照字节在事务内序列化（身份戳 snapshot_seq 已含本批递
         *           增值），落盘必须等 tx.commit() 成功——先库后快照，崩溃窗
         *           口全安全（库提交快照未写则下一批 seq 失配自弃）。空字节
         *           = 本批无快照可写（内存库/装配失败）。
         */
        struct DispatchCommitOut
        {
            std::string           snapshot_bytes;
            std::filesystem::path snapshot_path;
        };

        /**
         * @brief meta 键的按行读取（空行跳过；键缺席产出空表）。
         *
         * dispatch 的四张 meta 行表（prev_slots/prev_callees/doomed_keys/
         * doomed_names）与延迟批落盘的 dispatch_stale_files 全走这里——同一
         * 拆行循环昔日四处内联，改一处忘三处的镜像缺口。
         */
        [[nodiscard]]
        inline auto read_meta_lines(cg::db::GraphStore& store, const char* key)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            const auto               raw = store.meta(key);
            if (!raw.has_value())
            {
                return out;
            }
            for (const std::string_view line : cg::util::split(*raw, '\n'))
            {
                if (!line.empty())
                {
                    out.emplace_back(line);
                }
            }
            return out;
        }

        inline auto dispatch_commit(const DispatchCommitCtx& ctx)
            -> DispatchCommitOut
        {
            cg::db::GraphStore& store = *ctx.store;
            IndexSummary&       sum = *ctx.sum;
            // 进入固定点/派发解析前最后心跳：这两步是提交段里最长的连续计算
            // 段，其内部无法插入心跳点，靠这里的续约 + 自适应初始租约（盖过
            // 2x 实测提交段）覆盖。
            ctx.beat();
            // 驻留取用校验（协议见 AnalysisSlotsResidency）：提交事务已持写锁，
            // 此刻的 claim 快照即本批全程有效的持有者身份。
            static AnalysisSlotsResidency slots_residency;
            const cg::db::ClaimSnapshot   claim_snap = [&store]()
            {
                try
                {
                    return store.claim_snapshot();
                }
                catch (const std::exception&)
                {
                    return cg::db::ClaimSnapshot{};
                }
            }();
            const bool residency_hit =
                !ctx.db_path.empty() && claim_snap.live
                && !claim_snap.holder.empty() && slots_residency.inited
                && slots_residency.db_path == ctx.db_path
                && slots_residency.holder == claim_snap.holder;
            // ---- 增量输入装配：meta 捕获（延迟批累积）∪ ctx
            // 批内捕获。----
            std::unordered_set<cg::db::FileId> dirty_fids;
            if (ctx.dirty_files != nullptr)
            {
                dirty_fids.insert(
                    ctx.dirty_files->begin(),
                    ctx.dirty_files->end());
            }
            std::vector<std::pair<std::string, std::string>> prev_slots;
            if (ctx.prev_slots != nullptr)
            {
                prev_slots = *ctx.prev_slots;
            }
            std::vector<std::string> prev_callees;
            if (ctx.prev_callees != nullptr)
            {
                prev_callees = *ctx.prev_callees;
            }
            std::vector<std::string> doomed_keys;
            if (ctx.doomed_keys != nullptr)
            {
                doomed_keys = *ctx.doomed_keys;
            }
            std::vector<std::string> doomed_names;
            if (ctx.doomed_names != nullptr)
            {
                doomed_names = *ctx.doomed_names;
            }
            const auto read_lines = [&store](const char* key)
            { return read_meta_lines(store, key); };
            for (const std::string& line : read_lines("dispatch_prev_slots"))
            {
                const std::size_t sep = line.find('\x1f');
                if (sep != std::string::npos)
                {
                    prev_slots.emplace_back(
                        line.substr(0, sep),
                        line.substr(sep + 1));
                }
            }
            for (const std::string& key : read_lines("dispatch_prev_callees"))
            {
                prev_callees.push_back(key);
            }
            for (const std::string& key : read_lines("dispatch_doomed_keys"))
            {
                doomed_keys.push_back(key);
            }
            for (const std::string& name : read_lines("dispatch_doomed_names"))
            {
                doomed_names.push_back(name);
            }
            // 边变化检测（评审修正，升级制前提）：extends/overrides/
            // specializes 边集摘要与上次提交比对——变了则家族结构变化，
            // dispatch 全站点升级。
            const std::string hier_now = hierarchy_digest(store);
            bool              hierarchy_changed = false;
            if (const auto prev = store.meta("analysis_hierarchy_digest");
                prev.has_value() && *prev != hier_now)
            {
                hierarchy_changed = true;
            }
            // 外部边界 RTA（root set 线）：扫 SWIG .i/注册表/dlopen 边界并落
            // `#external` 行，必须在 propagate 读入 type_hints 之前——propagate
            // 按契约跳过 source=External 行，dispatch 从表内消费 R(I)（候选
            // 排序+标注，严禁站点级直用）。
            const auto t_rta0 = std::chrono::steady_clock::now();
            sum.external_rta_hints = cg::analysis::run_external_rta(store);
            const auto t_rta1 = std::chrono::steady_clock::now();
            store.set_meta(
                "analysis_rta_us",
                std::to_string(
                    static_cast<std::int64_t>(phase_us(t_rta0, t_rta1))));
            const std::string ext_now = external_digest(store);
            bool              external_changed = false;
            if (const auto prev = store.meta("analysis_external_digest");
                prev.has_value() && *prev != ext_now)
            {
                external_changed = true;
            }
            // 符号投影只读一次、propagate 与 dispatch 共用（分析期不写符号
            // 表，两个阶段各读一遍是纯粹的重复成本）。
            const std::vector<cg::db::IndexRow> analysis_rows =
                cg::db::all_symbol_index_rows(store);
            // 键预解析：投影一次成 map，提交段循环内（边写回 2 键/条、间接
            // 发射 1-2 键/条、受影响站点 1 键/个）的键查询零库访问——逐键
            // 查库在大库是提交段的常数税。键皆来自库内符号，map 必中；兜底
            // 保留原查询以保守等价。
            std::unordered_map<
                std::string,
                cg::db::SymId,
                cg::util::string_hash,
                std::equal_to<>>
                id_of_key;
            id_of_key.reserve(analysis_rows.size() * 2);
            for (const cg::db::IndexRow& row : analysis_rows)
            {
                id_of_key.emplace(row.unique_key, row.id);
            }
            const auto owner_of = [&ctx, &id_of_key](std::string_view key)
                -> std::optional<cg::db::SymId>
            {
                if (const auto it = id_of_key.find(key); it != id_of_key.end())
                {
                    return it->second;
                }
                return ctx.owner_of(key);
            };
            const auto t_prop0 = std::chrono::steady_clock::now();
            const cg::analysis::StubTypeOracle oracle;
            // 增量计划恒装配（资格失败在 run 内部降级，全量路径零分叉）。
            cg::analysis::IncrementalPlan plan;
            plan.active = true;
            plan.dirty_files = &dirty_fids;
            plan.prev_callee_keys = &prev_callees;
            plan.prev_slots = &prev_slots;
            plan.doomed_keys = &doomed_keys;
            plan.doomed_names = &doomed_names;
            plan.hierarchy_changed = hierarchy_changed;
            if (residency_hit)
            {
                // 驻留命中：warm 装载直接采用驻留行集（恒等论证见
                // AnalysisSlotsResidency），免 analysis_slots 全表读。
                plan.warm_preloaded = &slots_residency.rows;
            }
            // ---- 槽侧快照装配：文件库恒开（读侧资格在 run 内部逐项校验，
            // hierarchy_changed 批不尝试恢复；写侧任何成功提交的批都拍快照，
            // 包括降级/全量——终态一律正确）。----
            cg::analysis::snap::SnapshotData   snap_data;
            cg::analysis::snap::RestoreRequest snap_request;
            const bool                         snap_file_ok =
                !ctx.db_path.empty() && ctx.db_path != ":memory:";
            if (snap_file_ok)
            {
                snap_request.path =
                    cg::analysis::snap::snapshot_path(ctx.db_path);
                if (const auto seq = store.meta("snapshot_seq");
                    seq.has_value() && !seq->empty())
                {
                    // meta 值由本工具写（to_string 形态）；atoll 容忍外部篡改的
                    // 非数字串（解析为 0 → seq 失配自弃，不抛异常打断提交段）。
                    snap_request.snapshot_seq =
                        static_cast<std::uint64_t>(std::atoll(seq->c_str()));
                }
                plan.snap_request = &snap_request;
                plan.snap_dump = &snap_data;
            }
            const cg::analysis::PropagationState state =
                cg::analysis::run(store, oracle, &analysis_rows, &plan);
            const auto t_prop1 = std::chrono::steady_clock::now();
            store.set_meta(
                "analysis_propagate_us",
                std::to_string(
                    static_cast<std::int64_t>(phase_us(t_prop0, t_prop1))));
            // 传播段分段归因（微秒）：装载/预解析/增量闭包/四阶段/终态物化，
            // 外加装载段六分细分（符号投影/hints 槽构建/sites 行流/槽索引构建/
            // warm 装载/类型 intern）与四阶段合计——各细分键与性能件收益落点
            // 一一对应（sites 行流对装载列裁剪、槽构建与索引段对槽键整数化、
            // intern 段对类型集 arena 化）。
            for (const auto& [key, us] :
                 std::initializer_list<std::pair<const char*, std::int64_t>>{
                     {"analysis_prop_load_us", state.load_us},
                     {"analysis_prop_resolve_us", state.resolve_us},
                     {"analysis_prop_closure_us", state.closure_us},
                     {"analysis_prop_arg_param_us", state.arg_param_us},
                     {"analysis_prop_param_fanout_us", state.param_fanout_us},
                     {"analysis_prop_writeback_us", state.writeback_us},
                     {"analysis_prop_return_flow_us", state.return_flow_us},
                     {"analysis_prop_finalize_us", state.finalize_us},
                     {"analysis_prop_load_symbols_us", state.load_symbols_us},
                     {"analysis_prop_load_hints_us", state.load_hints_us},
                     {"analysis_prop_load_sites_us", state.load_sites_us},
                     {"analysis_prop_slot_index_us", state.slot_index_us},
                     {"analysis_prop_warm_us", state.warm_us},
                     {"analysis_prop_intern_us", state.intern_us},
                     {"analysis_prop_family_wb_edges",
                      static_cast<std::int64_t>(state.family_wb_edges)},
                     {"analysis_prop_family_pf_edges",
                      static_cast<std::int64_t>(state.family_pf_edges)},
                     // 族边双数字披露：*_edges 是预解析段的通道对数（静
                     // 态面裁剪后零投递的对也计入），*_fed_slots 是实际
                     // 收到族投递的槽数——两者相差一个量级时以槽数为实
                     // 际投递规模口径。
                     {"analysis_prop_family_wb_slots",
                      static_cast<std::int64_t>(state.family_wb_fed_slots)},
                     {"analysis_prop_family_pf_slots",
                      static_cast<std::int64_t>(state.family_pf_fed_slots)},
                     {"analysis_prop_loops_us",
                      state.arg_param_us + state.param_fanout_us
                          + state.writeback_us + state.return_flow_us}})
            {
                store.set_meta(key, std::to_string(us));
            }
            const auto t_disp0 = std::chrono::steady_clock::now();
            // dispatch 升级判定：传播增量态降级（槽表版本失配或暖启动表空）、
            // 继承/覆盖边集变化、`#external` 行集变化、或名字桶警报真触发
            // （复核闸放行；延迟批经 meta 传递，读到即消费）时任一虚站点
            // 的候选/标记都可能变——全站点重解析（家族级缓存在 resolve
            // 内部消化接口头的巨族形态）。
            const bool name_escalation =
                ctx.name_escalation
                || store.meta("dispatch_name_escalation").value_or("") == "1";
            const bool escalation = !state.incremental || hierarchy_changed
                                    || external_changed || name_escalation;
            if (escalation)
            {
                ++sum.dispatch_escalations;
            }
            const cg::analysis::DispatchResult dispatch =
                cg::analysis::resolve(store, state, &analysis_rows, escalation);
            const auto t_disp1 = std::chrono::steady_clock::now();
            store.set_meta(
                "analysis_dispatch_us",
                std::to_string(
                    static_cast<std::int64_t>(phase_us(t_disp0, t_disp1))));
            const auto t_wb0 = std::chrono::steady_clock::now();
            // 派发边清除（同一评审修正的删除半边）：全量或升级（重解析了全
            // 部站点）= 整类清空（既有形态；upsert 冲突会 bump 计数，未删先插
            // 会让 occurrences 翻倍）；纯增量 = 脏文件整文件清（行号漂移的旧边
            // 无站点可指）+ 受影响干净站点按 (caller, file, line) 三元组清。清
            // 除移到写回前——分析段只读 extends/overrides，vd 边的读取方不在
            // 本段。
            const auto t_erase0 = std::chrono::steady_clock::now();
            if (escalation)
            {
                store.erase_edges_of_type(cg::db::EdgeType::VirtualDispatch);
            }
            else
            {
                for (const cg::db::FileId fid : dirty_fids)
                {
                    store.erase_vd_edges_of_file(fid);
                }
                for (std::size_t j = 0; j < state.sites.size(); ++j)
                {
                    if (state.site_affected.empty() || !state.site_affected[j])
                    {
                        continue;
                    }
                    const auto caller = owner_of(state.sites[j].caller_key);
                    if (!caller.has_value())
                    {
                        continue; // 调用者符号已删：其边随 FK 级联消失
                    }
                    store.erase_vd_edges_of_site(
                        *caller,
                        ctx.file_of(state.sites[j].file),
                        state.sites[j].line);
                }
            }
            const auto t_erase1 = std::chrono::steady_clock::now();
            store.set_meta(
                "analysis_erase_us",
                std::to_string(
                    static_cast<std::int64_t>(phase_us(t_erase0, t_erase1))));
            sum.analysis_budget_exhausted =
                sum.analysis_budget_exhausted || state.budget_exhausted;
            sum.analysis_rounds += state.rounds;
            sum.analysis_work_items += state.work_items;
            sum.dispatch_fallback_sites = dispatch.fallback_sites;
            // 持久化最近一次运行的可观测值，供 `ccg stats` 读取。
            store.set_meta(
                "analysis_rounds",
                std::to_string(sum.analysis_rounds));
            store.set_meta(
                "analysis_work_items",
                std::to_string(sum.analysis_work_items));
            store.set_meta(
                "analysis_external_rta",
                std::to_string(sum.external_rta_hints));
            store.set_meta(
                "analysis_budget_exhausted",
                sum.analysis_budget_exhausted ? "1" : "0");
            // 写回 diff 化：refresh_call_site 覆写 10 列，行现值（装载快照）
            // 与解析结果逐列相等时跳过 UPDATE——值基判定，与"无条件写同值"
            // 语义等价；增量批里绝大多数站点输入未变，直收跳过。
            std::unordered_map<std::int64_t, const cg::db::CallSiteRow*>
                prior_of;
            prior_of.reserve(state.source_sites.size() * 2);
            for (const cg::db::CallSiteRow& prior : state.source_sites)
            {
                prior_of.emplace(prior.id, &prior);
            }
            for (const cg::analysis::ResolvedCallSite& site : dispatch.sites)
            {
                ctx.beat();
                cg::db::CallSiteRow row;
                row.id = site.site_id;
                row.callee_id = site.callee_id;
                row.method_id = site.method_id;
                row.method_key = site.method_key;
                row.static_type_id = site.static_type_id;
                row.receiver_types =
                    cg::analysis::json_string_array(site.receiver_types);
                row.callee_resolved = site.callee_resolved;
                row.resolved = site.resolved;
                row.confidence = static_cast<int>(site.confidence);
                row.candidates = cg::analysis::candidates_json(site.candidates);
                row.fallback_reason = site.fallback_reason;
                if (const auto it = prior_of.find(row.id); it != prior_of.end())
                {
                    const cg::db::CallSiteRow& p = *it->second;
                    if (p.callee_id == row.callee_id
                        && p.method_id == row.method_id
                        && p.method_key == row.method_key
                        && p.static_type_id == row.static_type_id
                        && p.receiver_types == row.receiver_types
                        && p.callee_resolved == row.callee_resolved
                        && p.resolved == row.resolved
                        && p.confidence == row.confidence
                        && p.candidates == row.candidates
                        && p.fallback_reason == row.fallback_reason)
                    {
                        continue;
                    }
                }
                store.refresh_call_site(row);
            }
            for (const cg::analysis::DispatchEdgeRec& edge : dispatch.edges)
            {
                ctx.beat();
                const auto src = owner_of(edge.caller_key);
                const auto dst = owner_of(edge.target_key);
                if (!src.has_value() || !dst.has_value())
                {
                    continue;
                }
                store.upsert_edge(
                    {.src = *src,
                     .dst = *dst,
                     .type = cg::db::EdgeType::VirtualDispatch,
                     .site_file = ctx.file_of(edge.site_file),
                     .site_line = edge.site_line,
                     .attrs = edge.attrs,
                     .confidence = edge.confidence,
                     .occurrences = 1});
            }
            // dispatch_edges = 库内当前 vd 边总数（全量/增量同口径；update 的
            // 引擎批+收敛批合并语义（+=）在两形态下都成立）。
            sum.dispatch_edges = cg::db::count_edges_of_type(
                store,
                cg::db::EdgeType::VirtualDispatch);
            // ---- 槽终态落库（增量路径）：降级 = 整表重写；增量 = 值基
            // diff（写 变化行 + 删消失行）。type_set 用与 hints 物化一致的 JSON
            // 形态。 本批 warm 的权威行集（驻留命中时是驻留，否则是装载的
            // plan.warm）。
            // ----
            const std::unordered_map<std::string, cg::db::AnalysisSlotRow>*
                warm_rows = plan.warm_loaded;
            // 增量形态（state.incremental）蕴含 warm 装载成功，warm_rows 必非
            // 空；全量/降级形态不读它。
            // 驻留接管：claim 活着且库身份有效即由本批终态接管（写回是新终态
            // 的唯一权威）；否则弃驻留（下一批全表读）。快照形态的 warm
            // （type_seg 区段引用）不接管——驻留协议以 SQL 行形态为唯一语汇，
            // 跨批由快照路径自持。
            const bool residency_sync = !ctx.db_path.empty() && claim_snap.live
                                        && !claim_snap.holder.empty()
                                        && !plan.warm_from_snap;
            if (residency_sync)
            {
                slots_residency.db_path = ctx.db_path;
                slots_residency.holder = claim_snap.holder;
                slots_residency.inited = true;
                if (!state.incremental)
                {
                    slots_residency.rows.clear();
                }
                else if (!residency_hit && warm_rows != nullptr)
                {
                    // 首建：以本批装载的权威行集为基线（写回循环在其上按终态
                    // 增量更新）。
                    slots_residency.rows = *warm_rows;
                }
            }
            else
            {
                slots_residency.inited = false;
                slots_residency.rows.clear();
                slots_residency.db_path.clear();
                slots_residency.holder.clear();
            }
            if (!state.incremental)
            {
                store.erase_analysis_slots_all();
            }
            std::unordered_set<std::string> live_slot_keys;
            if (state.incremental)
            {
                live_slot_keys.reserve(state.hints.size() * 2);
            }
            for (const cg::analysis::PropagatedHint& h : state.hints)
            {
                const std::string key =
                    cg::util::compose_key(h.owner_key, h.var_name);
                bool write = true;
                if (state.incremental)
                {
                    live_slot_keys.insert(key);
                    if (h.from_warm)
                    {
                        // warm 原值直通（未进脏闭包）：八列与既有行恒等（论证见
                        // PropagatedHint::from_warm），跳过 json
                        // 物化与逐列比对。
                        // 驻留保持基线行（首建基线/上批终态即该原值）。
                        write = false;
                        continue;
                    }
                    const auto wit = warm_rows->find(key);
                    if (wit != warm_rows->end()
                        && wit->second.type_set
                               == cg::analysis::json_string_array(h.types)
                        && wit->second.confidence
                               == static_cast<int>(h.confidence)
                        && wit->second.source == static_cast<int>(h.source)
                        && wit->second.reason == h.reason
                        && wit->second.truncated == h.truncated_here
                        && wit->second.alias_src == h.alias_src
                        && wit->second.ret_src == h.ret_src)
                    {
                        // 八列全等：库行不动，驻留同样保持基线行。
                        write = false;
                    }
                }
                if (write)
                {
                    const std::string type_json =
                        cg::analysis::json_string_array(h.types);
                    store.upsert_analysis_slot(
                        {.owner_key = h.owner_key,
                         .var_name = h.var_name,
                         .type_set = type_json,
                         .confidence = static_cast<int>(h.confidence),
                         .source = static_cast<int>(h.source),
                         .reason = h.reason,
                         .truncated = h.truncated_here,
                         .ver = cg::analysis::analysis_slots_ver,
                         .alias_src = h.alias_src,
                         .ret_src = h.ret_src});
                    if (residency_sync)
                    {
                        cg::db::AnalysisSlotRow& r = slots_residency.rows[key];
                        r.owner_key = h.owner_key;
                        r.var_name = h.var_name;
                        r.type_set = type_json;
                        r.confidence = static_cast<int>(h.confidence);
                        r.source = static_cast<int>(h.source);
                        r.reason = h.reason;
                        r.truncated = h.truncated_here;
                        r.ver = cg::analysis::analysis_slots_ver;
                        r.alias_src = h.alias_src;
                        r.ret_src = h.ret_src;
                    }
                }
            }
            if (state.incremental && warm_rows != nullptr)
            {
                for (const auto& [key, row] : *warm_rows)
                {
                    (void)row;
                    if (live_slot_keys.contains(key))
                    {
                        continue;
                    }
                    const std::size_t sep = key.find('\x1f');
                    store.erase_analysis_slot(
                        key.substr(0, sep),
                        key.substr(sep + 1));
                    if (residency_sync)
                    {
                        slots_residency.rows.erase(key);
                    }
                }
            }
            const auto t_wb1 = std::chrono::steady_clock::now();
            store.set_meta(
                "analysis_writeback_us",
                std::to_string(
                    static_cast<std::int64_t>(phase_us(t_wb0, t_wb1))));
            // 摘要与增量可观测值落盘；延迟标注与捕获 meta 一并清空（全量重算
            // 或增量收敛都把派发数据带回与已提交行一致）。
            store.set_meta("analysis_hierarchy_digest", hier_now);
            store.set_meta("analysis_external_digest", ext_now);
            store.set_meta(
                "analysis_incremental",
                state.incremental ? "1" : "0");
            if (plan.degraded)
            {
                store.set_meta(
                    "analysis_degrade_reason",
                    plan.degrade_reason.substr(0, 200));
            }
            else
            {
                store.erase_meta("analysis_degrade_reason");
            }
            store.set_meta(
                "analysis_closure",
                std::to_string(static_cast<std::int64_t>(plan.closure)));
            store.erase_meta("dispatch_stale");
            store.erase_meta("dispatch_stale_files");
            store.erase_meta("dispatch_prev_slots");
            store.erase_meta("dispatch_prev_callees");
            store.erase_meta("dispatch_doomed_keys");
            store.erase_meta("dispatch_doomed_names");
            store.erase_meta("dispatch_name_escalation");
            // ---- indirect_calls 消费
            // （槽终态写回后跑，槽索引用内存终态）----
            const auto t_ind0 = std::chrono::steady_clock::now();
            indirect_commit(ctx, state, &analysis_rows, &id_of_key);
            const auto t_ind1 = std::chrono::steady_clock::now();
            store.set_meta(
                "analysis_indirect_us",
                std::to_string(
                    static_cast<std::int64_t>(phase_us(t_ind0, t_ind1))));
            // ---- 快照序列化（事务内；落盘等调用方提交成功后）。身份戳
            // snapshot_seq 在本事务内递增——读者失配即弃，编辑批（延迟形态，
            // 不跑 dispatch 不递增）的库改动由指纹基线比对兜住。指纹基线取
            // 本批提交后的全表终态。观测键：恢复 hit/miss 原因、恢复墙钟、
            // 指纹脏文件数、剔除重建槽数、序列化体积。----
            DispatchCommitOut out;
            if (snap_file_ok)
            {
                const std::uint64_t next_seq = snap_request.snapshot_seq + 1;
                store.set_meta("snapshot_seq", std::to_string(next_seq));
                snap_data.snapshot_seq = next_seq;
                snap_data.fingerprints = cg::db::all_fingerprints(store);
                store.set_meta(
                    "analysis_snap_state",
                    plan.snap_hit ? "hit" : ("miss:" + plan.snap_miss_reason));
                store.set_meta(
                    "analysis_snap_restore_us",
                    std::to_string(plan.snap_restore_us));
                store.set_meta(
                    "analysis_snap_dirty_files",
                    std::to_string(
                        static_cast<std::int64_t>(plan.snap_dirty_files)));
                store.set_meta(
                    "analysis_snap_rebuilt_slots",
                    std::to_string(
                        static_cast<std::int64_t>(plan.snap_rebuilt_slots)));
                out.snapshot_bytes = cg::analysis::snap::serialize(snap_data);
                if (!out.snapshot_bytes.empty())
                {
                    out.snapshot_path = snap_request.path;
                    store.set_meta(
                        "analysis_snap_bytes",
                        std::to_string(
                            static_cast<std::int64_t>(
                                out.snapshot_bytes.size())));
                }
                else
                {
                    store.set_meta("analysis_snap_state", "serialize-failed");
                    store.erase_meta("analysis_snap_bytes");
                }
            }
            return out;
        }

        /**
         * @brief 解析段租约心跳（真库实测事故的修复）：伴随线程在独立连接
         *        上周期续租，把"声明仍归本进程"的事实持续证明到解析段结束。
         *
         * 病根：解析段只读、不持锁、耗时以分钟到数十分钟计（大库实测 26 分
         * 钟），而全部既有续约点——提交段事务内心跳、批间 peek、批后出队
         * ——都发生在解析完成**之后**；租约（百秒级）在解析中途过期后，钩
         * 子按"过期可偷"语义派生新写者反偷，索引在提交段第一个心跳点才发
         * 现易主而整场弃权。本类在解析段开始时起一条伴随 jthread，用它
         * **自己独占**的 GraphStore 连接周期执行 renew_update_lease（与
         * peek_pending 同款短 IMMEDIATE 事务形态；绝不触碰主线程的连接与
         * 语句缓存，跨线程共享那些可变状态即数据竞争）；提交段拿写锁之前
         * 由主线程 stop()，续租职责交还事务内的 heartbeat_in_tx。
         *
         * 失败语义：心跳匹配零行 = 租约真被偷，置 stolen 标志后线程自行收
         * 摊；主线程在 stop() 后检查并按 ClaimStolenError 语义优雅让位（干
         * 净退出、队列归窃者）。BUSY/IO 异常吞掉、下一周期重试——BUSY 只说
         * 明锁被别人短暂持有，绝不等于被偷。连接打不开（盘错误等）时线程
         * 静默退出：剩余租约仍由既有心跳点兜底，行为退回修复之前，绝不更
         * 糟。周期等待可被 stop() 即刻唤醒（condition_variable），join 恒快。
         *
         * 首拍时序：每个周期先等待后续租（不在本线程启动瞬间落库）——第
         * 一拍若即刻写库，会在主连接解析段的读快照窗口上推进 WAL，把提
         * 交段的 BEGIN 升级送进忙处理器救不回的快照冲突形态。解析段开头
         * 至多一个周期的无续租由自适应初始租约覆盖。
         */
        class ParseLeaseKeepalive
        {
        public:
            ParseLeaseKeepalive(
                std::string  db_path,
                std::string  holder,
                std::int64_t lease_ms,
                std::int64_t period_ms);
            ~ParseLeaseKeepalive();
            ParseLeaseKeepalive(const ParseLeaseKeepalive&) = delete;
            ParseLeaseKeepalive& operator=(const ParseLeaseKeepalive&) = delete;
            ParseLeaseKeepalive(ParseLeaseKeepalive&&) = delete;
            ParseLeaseKeepalive& operator=(ParseLeaseKeepalive&&) = delete;

            /** @brief 请求停止并 join（幂等；提交段拿写锁前调用）。 */
            void stop();

            /** @brief 心跳已发现租约被偷（stop 之后读取才有定论）。 */
            [[nodiscard]]
            bool stolen() const
            {
                return stolen_.load();
            }

            /** @brief 成功续租次数（观测面：证明解析期确实有人在续租）。 */
            [[nodiscard]]
            std::int64_t beats() const
            {
                return beats_.load();
            }

        private:
            void note_stop_();
            void loop_();

            std::string  db_path_;
            std::string  holder_;
            std::int64_t lease_ms_;
            std::int64_t period_ms_;
            // 声明序=析构逆序的依赖：thread_（join）必须先于它捕获 this 的
            // 同步原语（cv_/mx_）消亡；store_ 挂在线程函数栈上，join 后自然
            // 已随线程退出而关闭。
            std::jthread              thread_;
            std::mutex                mx_;
            std::condition_variable   cv_;
            bool                      stopping_ = false; // 仅在 mx_ 下读写
            std::atomic<bool>         stolen_{false};
            std::atomic<std::int64_t> beats_{0};
        };

        ParseLeaseKeepalive::ParseLeaseKeepalive(
            std::string  db_path,
            std::string  holder,
            std::int64_t lease_ms,
            std::int64_t period_ms)
            : db_path_(std::move(db_path)), holder_(std::move(holder)),
              lease_ms_(lease_ms), period_ms_(period_ms),
              thread_{[this] { loop_(); }}
        {
        }

        ParseLeaseKeepalive::~ParseLeaseKeepalive() { stop(); }

        void ParseLeaseKeepalive::note_stop_()
        {
            {
                const std::lock_guard lock{mx_};
                stopping_ = true;
            }
            cv_.notify_all();
        }

        void ParseLeaseKeepalive::stop()
        {
            note_stop_();
            if (thread_.joinable())
            {
                thread_.join();
            }
        }

        void ParseLeaseKeepalive::loop_()
        {
            // 独占连接：与主线程的 GraphStore 之间零共享（连接句柄、语句缓
            // 存、interner 全是每实例私有）。短 busy——撞锁快速回到周期等
            // 待，绝不钉在锁队列上。
            std::optional<cg::db::GraphStore> store;
            try
            {
                store.emplace(
                    db_path_,
                    cg::db::default_lease_keepalive_busy_ms);
            }
            catch (const std::exception&)
            {
                return; // 打不开就退回修复前行为，既有心跳点兜底
            }
            for (;;)
            {
                // 先等后租：线程启动瞬间就落第一拍会在主连接尚持读快照的
                // 窗口上推进 WAL，把两者送进快照冲突的互撞（接管收尾的追
                // 轮提交段实测被这一拍钉死成无限自旋）。首个周期（至多
                // lease/4、上限 60s）不续租，由自适应初始租约（远大于一个
                // 周期）完全覆盖，语义无损。
                std::unique_lock lock{mx_};
                cv_.wait_for(
                    lock,
                    std::chrono::milliseconds{period_ms_},
                    [this] { return stopping_; });
                if (stopping_)
                {
                    return;
                }
                try
                {
                    if (!store->renew_update_lease(holder_, lease_ms_))
                    {
                        stolen_.store(true);
                        return; // 租约真被偷：收摊，让主线程优雅让位
                    }
                    ++beats_;
                }
                catch (const std::exception&)
                {
                    // BUSY/IO：容忍，下一周期再试（BUSY 绝不等于被偷）。
                }
            }
        }

    } // namespace detail

    /**
     * @brief 驱动一次对已加载编译数据库的完整索引过程。
     */
    class Indexer
    {
    public:
        Indexer(cg::db::GraphStore& store, IndexOptions options)
            : store_(store), opt_(std::move(options))
        {
        }

        /**
         * @brief 逐编译数据库条目的新鲜度计划（pass 1 的产物）。
         *
         * @internal 每条目一行：去重后首个条目持有完整决策，别名条目
         *           （同文件同摘要）复用首条的指针级状态。
         */
        struct MainPlan
        {
            const cg::clang::CompileCommand* cmd = nullptr;
            cg::db::FileId                   fid = 0; // 0 = 尚未索引
            std::string                      digest;
            detail::FileStat                 st;
            bool                             parse = false; // TU 须重提取
            bool        missing = false; // stat 失败：记失败 + 清除
            std::string hash;            // 本运行算得的内容哈希
            /**
             * @brief 非空 = 该条目世界状态未变，按上次失败跳过。
             *
             * @internal 指向 pass 1 前的 failures 快照（快照在其后
             * 不再增长，指针稳定）。failure_skip 不产生 job、不 doom、
             * 不播种前向闭包。
             */
            const cg::db::FailureRow* failure_skip = nullptr;
        };

        /** @brief 一个待重解析 TU 的计划载荷（解析代价代理与失败键）。 */
        struct StaleTu
        {
            const cg::clang::CompileCommand* cmd = nullptr;
            cg::db::FileId                   fid = 0; // 0 = 新
            std::string                      hash;
            std::string                      digest; // 失败记录键的一部分
            std::int64_t   mtime_ns = 0;             // 失败记录里的 stat 身份
            std::uintmax_t size = 0;                 // 主文件字节；解析代价代理
        };

        /** @brief 一个解析作业的多阶段产物（提取、分析、失败与计时）。 */
        struct TuResult
        {
            cg::extract::TuExtract   ex;
            cg::analysis::TuAnalysis analysis;
            std::string              failure;
            bool                     analysis_failed = false;
            double                   parse_us = 0.0;
            double                   visit_us = 0.0;
            std::string dialect_fixit_note;  // 非空 = 发生过诊断驱动改写
            std::string encoding_scrub_note; // 非空 = 发生过编码消毒
            /// 非空 = 错误全为良性白名单家族、TU 照常索引（成功侧注记，
            /// 只在走查成功后写下——失败 TU 绝不携带）。
            std::string benign_error_note;
            /**
             * @brief 失败诊断定位到的文件（绝对、规范化、去重；仅模块 TU
             *        收集）。
             *
             * @internal 失败 TU 的 includes 行无从产生（提取只在零诊断解析后
             * 运行），诊断定位是"哪个依赖文件杀死了本次解析"的直接证词——
             * 失败记录把它们冻结进 deps_json，修复这些文件即触发重试。
             */
            std::vector<std::string> failure_files;
            /** @brief analysis.collect 走查墙钟（微秒；性能观测面）。 */
            double analysis_us = 0.0;
        };

        /**
         * @brief 提交段的共享状态：run 的计划/解析产物与库快照以引用
         *        打包过给 commit_batch（纯搬迁，无所有权转移）。
         *
         * @internal 复核段会重排 jobs/results、重算存活判定并撤销部分
         *           整体清除，故若干成员是可变引用。
         */
        struct CommitState
        {
            IndexSummary&          sum;
            std::vector<MainPlan>& plans;
            const std::map<std::pair<std::string, std::string>, std::size_t>&
                                                dedup;
            std::vector<StaleTu>&               jobs;
            std::vector<TuResult>&              results;
            const std::vector<cg::db::FileRow>& existing_files;
            std::unordered_map<
                std::string,
                cg::db::FileRow,
                cg::util::string_hash,
                std::equal_to<>>&                             row_by_path;
            std::unordered_map<std::string, cg::db::FileRow>& fold_row;
            const std::unordered_map<
                cg::db::FileId,
                std::vector<cg::db::FingerprintRow>>& fp_by_file;
            const std::unordered_map<
                cg::db::FileId,
                std::vector<cg::db::FailureRow>>&                  fail_by_file;
            const std::unordered_map<cg::db::FileId, std::string>& path_of;
            const std::
                unordered_map<cg::db::FileId, std::vector<cg::db::FileId>>& fwd;
            /** @brief pass 1b 抹除集（盘上消失/离开编译库的既有文件）。 */
            const std::unordered_set<cg::db::FileId>& erased;
            /**
             * @brief 快照包含图的原始目标串（包含者 -> 旧包含目标）。
             *
             * @internal 快路径的覆盖差检查输入：重写文件被 purge 的旧
             *           包含行必须被本批回写目标集覆盖，否则照走全量清扫。
             */
            const std::unordered_map<cg::db::FileId, std::vector<std::string>>&
                old_includes;
            /**
             * @brief 主文件条目整组退役（有主指纹的文件当前编译库零条目）。
             *
             * @internal 根集失守信号：头形态主文件离开编译库时不进抹除集
             *           也无人包含，行还在、根已失——在场则不可跳过清扫。
             */
            bool                                   main_entries_retired = false;
            const std::unordered_set<std::string>& main_paths;
            std::unordered_set<cg::db::FileId>&    ok_entries;
            std::unordered_set<cg::db::FileId>&    failed_entries;
            std::unordered_set<cg::db::FileId>&    all_failed_fids;
            std::unordered_set<cg::db::SymId>&     doomed;
            /**
             * @brief 重写集（首遍解析的包含闭包 ∪
             * 配置变更域第二遍种子的闭包）。
             *
             * @internal 行清除面=本批将重解析的观察者闭包。名字桶全塞
             *           与死者社会关系种子已删——分析层判定无权把文件
             *           升格进此面（升格文件的行清除将无重提取回填）。
             */
            const std::unordered_set<cg::db::FileId>& reached;
            /**
             * @brief 终态确证真死的键行（挂在行清除面够不到的文件上）。
             *
             * @internal 复核闸同一判据收集（编辑/模板实例档精确比对）；
             *           提交段显式删行，外键级联清边/线索/站点/子成员。
             */
            const std::vector<cg::db::SymId>& dead_symbol_rows;
            /**
             * @brief 名字桶真触发（复核闸放行）置位的分析层升级开关。
             *
             * @internal 消费点是 commit 段 dispatch 的 escalation 判定
             *           （全站点决议重做+派发边整类清空）；延迟批落 meta
             *           dispatch_name_escalation 跨批传递。
             */
            bool name_lift_escalation = false;
            /**
             * @brief 新鲜度扫描攒下的指纹/哈希载荷（引用 run 局部向量）。
             *
             * @internal 非常量引用：提交段开头的陈旧快照核对发现行已被
             *           并发写者删除时，死 id 的载荷在此整体丢弃（行没了，
             *           旧 id 的指纹无主可挂——复活只救"本批正在写入"的
             *           路径引用，不救快照旁路载荷）。
             */
            std::vector<cg::db::FingerprintRow>&             fp_writes;
            std::unordered_map<cg::db::FileId, std::string>& hash_writes;
            const std::string&                               roots_digest;
            std::function<void()>                            run_pool;
            /**
             * @brief 解析期租约心跳线程（可空；caller-held 时非空）。
             *
             * @internal 提交段拿写锁之前必须先停掉它（事务内的
             *           heartbeat_in_tx 从此接管续租）：独立连接的心跳
             *           在写事务期间只会与自己的写锁互撞空转。
             */
            detail::ParseLeaseKeepalive* keepalive = nullptr;
        };

        /**
         * @brief 执行一次索引过程并返回汇总。
         *
         * @internal
         * 【准备】根保持原样：提取把混合大小写路径重拼为盘上拼写、再恢复
         * 编译数据库自己教授的前缀（大小写漂移契约），使允许清单与锚定
         * 身份按构造一致。教授的拼写 = 每个主文件的祖先目录加运行的根，
         * 按折叠前缀首见（编译数据库顺序）、最长在前。一致重拼的数据库
         * 因此对其主目录下的一切保持自己的拼写，而仅由 -I 旗标携带的拼写
         * （异于库自身文件字段或盘上）每文件折叠为一个身份——唯一键内嵌
         * 路径，一个文件的两种拼写会铸造重复子图。
         *
         * 【状态快照】过期检查所需的一切只读一次：文件行、新鲜度指纹、
         * 已记录的包含图。折叠复行修复在快照之前跑：早期大小写漂移运行
         * 留下的行（一个文件的两种拼写，各带一份重复子图）经不区分大小写
         * 的包含解析器永远可达，普通运行——连 --full 在内——都除不掉，
         * 只有显式合并可以。同批行的折叠视图（首个拼写胜出）用于下方
         * 识别大小写漂移、并在存储阶段把重拼路径折到已索引行上；
         * folded_dups 标记未修复的伤（干跑不能修复），使计划仍反映该修复。
         * 指纹在库中按（文件, 命令摘要）键控：多编译数据库条目的文件每
         * 条目一行。
         *
         * 【大小写漂移】主文件未命中精确拼写映射但折到已索引行上的编译
         * 数据库条目是同一文件的重拼（Windows 文件系统不区分大小写；
         * POSIX 上可能的真正同名折叠文件由同文件检查分离）。条目复用该行
         * 的 id：行 id 即身份，既不复制新行，也不经新拼写的游标路径复制
         * 头子图。重拼本身不使内容过期，故不触发重解析（存储拼写在条目
         * 真正重解析时随存储阶段刷新）；把它升级为全量强制会让任何一次
         * 单文件 update 退化成整语料重析——这正是 PostToolUse 超时的根源。
         * 主文件判定同时按行 id 认这些条目，避免既有行被误当非主文件。
         *
         * 【实时种子】ccg update（实时变更队列）：外部事件点名为已改
         * 的文件，对规范化路径折叠（不区分大小写）——与包含解析器相同的
         * 身份纪律。空种子集（每个普通索引运行）使扫描与之前逐字节相同：
         * 下方各处用法在 seed_fold.empty() 上短路。
         *
         * 【pass 1：TU 新鲜度】逐编译数据库条目（按主文件 + 命令摘要去重）：
         * stat 快路径，stat 变化时 sha256 确认，摘要比较。重解析条件：强制
         * （大小写漂移或未修复折叠伤亦然——每行都须按当前拼写重建）、
         * 外部播种（ccg update 点名了此主文件；stat 快路径可能合法地什么
         * 都看不到——保 mtime 的编辑）、新、不可验证（崩溃运行）、
         * 或先前失败。stat 变化时先 sha256 确认再重解析（checkout 或切分支
         * 以相同内容触碰每个 mtime，解析代价以分钟计；确认在大语料上也只要
         * 秒级）。变脏主文件需记录新哈希（新主文件记首个）——此处一次算好，
         * 存储阶段复用。别名镜像首条目的决策；计数逐条目。计划字节只统计
         * 真正会重解析的 TU：给每个去重拥有者计数会使全跳过计划报出整个
         * 语料的大小（"0 of 6 TUs (~12 KB)"）。
         *
         * 【pass 1b：非主新鲜度】其余被跟踪文件（头）：同一 stat/哈希纪律，
         * 无命令摘要。离开编译数据库的源文件与盘上删除的文件并入抹除集
         * （其行删除，其包含的目标失去一个观察者）。从构建移除（移出构建、
         * EXCLUDE_FROM_BUILD、或不再传入编译数据库）的源文件与盘删除完全
         * 同样地清除；被其他 TU 包含的源文件走普通头处理——可达性清扫
         * 保留它，不得抹除（否则其包含者每次运行都重解析）。快照包含图
         * 先于非主扫描构建：当前某 TU 仍 `#include` 的缺失源文件不算删除
         * ——它是普通非主数据，命运由可达性清扫裁定。无指纹或状态
         * 非 ok 的行为脏——不可验证即重解析。变脏时指纹必须推进到新 stat，
         * 否则下一运行永远重复判脏。外部播种的非主文件（ccg update）：事件
         * 流对其自己的文件是权威——无视 stat 快路径直接内容变脏（保 mtime
         * 的编辑不得漏网）、记录哈希，下方闭包走查重解析每个包含者。
         *
         * 【包含闭包扩展】包含闭包（经已记录 includes 表）触及脏文件的 TU
         * 主文件必须重解析：头变更传播。walked 守卫 BFS 本身；文件可能已在
         * reached 中（更早阶段播过种）而其包含者仍需走查，故已访问检查不得
         * 兼作入队判定。直接变脏的主文件（内容编辑、摘要变化、先前失败、
         * 或强制）与内容变脏的非主文件一样是重写种子：没有这条，pass 1 把
         * 主文件标为过期、阶段 A 刷新其符号，但其编辑前站点行以旧行号存活、
         * 新边/新包含永不写入——指纹随后还把该发散记录为最新。后向走查也
         * 拉入包含该主文件的 TU（罕见，但免费）。强制级覆盖（含大小写漂移/
         * 折叠伤）：每 TU 重解析，故每个被跟踪文件拥有完整批次覆盖——全部
         * 播种使运行就地重写每行，即 --full 等于重建库的 CLI 契约，也是
         * --full 运行能治愈早期（有缺陷或崩溃的）运行遗留行的唯一途径。
         * 已达主文件从跳过转为解析（内容未变；存储哈希仍有效）。
         *
         * 【pass 2：解析+提取】producer_std_of：库为每个模块接口单元记录的
         * -std，按其构建产出的 pcm 键控——喂给这些 pcm 而用不同 -std 的
         * 消费者是分类器在 rc=4 上点名的 bmi-config-mismatch 轴。每个工人
         * 拥有一个 CXIndex 与其领取的每个 TU 的完整解析->提取生命周期
         * （libclang：CXIndex/CXTranslationUnit 绝不跨线程；cg.clang 在调用
         * 线程把每个 CXString 物化为 std::string，离开工人的只有纯 TuExtract
         * 数据）。作业最长先行（LPT）：语料分布重尾，最大 TU 必须最先启动，
         * 否则它独自封顶完工时间（并列时按编译数据库顺序稳定决胜）。池进度
         * 只计池内作业（跳过的 TU 由新鲜度 pass 上报）：计数器必须精确到达
         * 分母，增量运行亦然。工人的 PathCaseCache 是路径大小写探针缓存的
         * 显式继任（原为 canonical_case_spelling 的函数局部 thread_local
         * 状态）：每工人一份跑全程，目录列表在该工人提取的每个 TU 间摊销。
         * 命令合成已知的失败（自构建降级：std BMI 不可建、模块 DAG 环、
         * 扫描洞）逐字上报——不尝试解析，TU 降级到第 1 层。否则先做模块
         * BMI 喂给前校验（模块喂给契约）：拒绝喂给不能解析的 pcm（盘上缺失、
         * 生产者版本 != 引擎）并预先点名类别 + 轴，而不是放任 clang 静默
         * 失败。被分类的 TU 计为失败、同他人一样被清除/标记；运行继续
         * （逐 TU 降级）。engine_token 是失败分类器点名轴所用的 BMI 键的
         * 一半：已加载引擎的版本记号；另一半是 -std 轴，均已从
         * 编译数据库按运行构建——每个比较的两侧都是 ccg 记录的事实。
         * NULL-TU 加固（对任何未来引擎形态的廉价保险）：Index::parse 已经
         * 抛异常而非返回 NULL 翻译单元，但消费侧不得把进程押在该内部契约
         * 上——某解析若仍交回无 TU，在此降级为命名失败，绝不把 NULL
         * CXTranslationUnit 交给任何 libclang 调用。解析带错时先试命名模块
         * 失败类（分类学的高声类点名模块/分区/pcm，使行可行动）；再试
         * 缺头类：点名重放实际喂给了多少包含目录，使过短搜索清单直接可见
         * 于失败文本（不再读作被静默丢弃的包含路径）。CCG_DEBUG_DIAGS 的
         * 输出在队列锁下打印：DIAG 行不得与锁定的进度输出行内交错。
         * 遍历阶段引擎崩溃守卫：单个 TU 的整个游标走查跑在平台硬件故障
         * 守卫之后——libclang 自身的崩溃恢复只武装 PARSE 阶段、从不武装
         * 此走查，走查中 libclang 内部的故障（C API 无法筛掉的输入上的
         * 引擎缺陷）对该 TU 降级为命名的 engine-visit-crash 失败类；池
         * 继续、运行其余部分不受扰。已知故障形态已在各自调用点围栏（声明
         * 类型走查中依赖成员指针的类类型查询自带细粒度守卫），此处是纵深
         * 防御网。恢复跳过提取在途帧的析构——部分结果整体丢弃（仅该一次
         * 提取的分配存活到进程退出）。硬 libclang 失败按模块分类学分类：
         * 模块喂给 TU 上的 rc=4（ASTReadError）是静默 bmi-config-mismatch
         * 类——从 ccg 自记键点名轴；rc=2（Crashed）是过期 BMI 集的崩溃
         * 恢复残渣（过期下游 BMI 引用已删声明是已知成因）。喂给缺失硬失败
         * 类：携带导入但重放完全没带 -fmodule-file 的 TU 以零诊断死 rc=4
         * ——什么都没喂时点名缺失而非原始码。进度打印为尽力而为：抛异常的
         * stderr 写（管道关闭）既不得逃出线程入口（std::terminate）也不得
         * 进入上方 catch（那会把成功解析的 TU 误标为失败）；CCG_DEBUG_DIAGS
         * 的打印刻意留在 try 内——那里的抛出只影响已带解析错误的 TU。
         * 干跑在库持行时也解析脏集：计划必须陈述真实运行将重解析的确切
         * 内容，且包含差种子只能从新提取导出；对空库每个 TU 按构造都是
         * 新工作，计划不解析也已精确。
         *
         * 【解析范围封死】解析范围=包含闭包∪（引擎/配置变更），封死于
         * 此：首遍翻位处理包含闭包；第二遍翻位只处理配置/内容变更驱
         * 动的种子（抹除/失败/盘缺/摘要退役的前向闭包、包含差目标——
         * 编译数据库与包含图的世界变化）。解析完成后的一切分析层判定
         * （悬垂边、覆盖末日、名字桶警报）无权再把文件塞进解析范围——
         * 旧的 existing_files 全塞+翻位+二跑链（曾以 670 TU 重做全库解
         * 析去抢救一个名字桶）与死者社会关系种子（doomed 的边站点/定义
         * 文件）已删。解析重数变化（实测发现）：出现计数按（主文件, 命
         * 令摘要）解析各计一次 bump。已索引主文件的存活条目数变化——
         * 两条发散重复条目之一被丢弃、第二条加入、或最后一条消失而另有
         * TU 仍包含该文件——该主文件整个包含闭包中的站点会永远多计死
         * 解析（或少计新解析）：把重数变化当作被抹除主文件的观察者退出
         * 处理（播前向闭包进第二遍）。批次中带新鲜包含行的文件：逐文件
         * 对已记录行做差；每个新增或移除目标的闭包都改变观察者集合。
         *
         * 【覆盖末日】doomed 集来自已解析主文件的缺失键、抹除/失败/缺失
         * 文件与同文件级联（级联子：父键死子键失配）。覆盖末日的对账域
         * 收死在本批 fresh 提取并集覆盖的文件内——未重解析文件的键缺席
         * fresh 并集是伪前提（它们没有重新提取，不代表死亡），跨文件扩散
         * （死者社会关系经边站点/定义文件引新文件入域）整体删除，跨文件
         * 影响由分析层全站点重做兜住。随后探测清除提升歧义：第 3 层因被
         * 调用者名字匹配多候选而丢弃的调用点，在本运行清除到只剩一个时
         * 变得可解析——但丢弃的站点没留存储行，无行可循。复核闸（终态
         * 三档）放行的真触发不再升级解析，而是置分析层升级开关：commit
         * 段 dispatch 走全站点决议重做+派发边整类清空重写（既有
         * escalation 机制；延迟批经 meta dispatch_name_escalation 跨批传
         * 递）。反方向（新添符号使干净文件的边变得可解析）不做重试——
         * 未解析的 callee 本不入库，无行可循，这是文档化的已知边界。
         *
         * 【计划输出】计划级 purgedFiles：已提交计数来自批次后状态的孤儿
         * 清扫，故计划模拟该状态。文件：快照加每个成功提取点名的文件
         * （运行会创建的行用临时负 id；重拼路径折到其复用的行）。包含
         * 链接：重写覆盖被提及文件的新鲜行、行将被整体清除的文件（失败
         * 与盘缺主文件）无行、其余存行。根：每个计划的主文件。决策核是
         * 清扫自己的助手（unreachable_files），不是平行重实现。
         *
         * 【pass 3：按编译数据库顺序收集】失败标记、成功按库序入批，
         * 故 id 与行序精确匹配串行运行。
         *
         * 【重写规则】文件的站点边/包含只在观察它的每个 TU 都在批次中时
         * （reached = 种子加全部传递包含者——后向闭包把观察者与被观察者
         * 一起访问）、或该文件无先行行时清除并重写。未达文件保留其行：
         * 内容与观察者集合未变，其存储出现计数已等于全量重索引的。
         *
         * 【退役】解析不得落到本运行删除的行上；阶段 A 复活它重新 upsert
         * 的 id（upsert_entry 从 retired_ 抹除）。过期键 doomed 集覆盖已解析
         * 主文件的内容编辑；重写集另退役清除可删行的文件的每个运行前符号
         * ——强制级覆盖（大小写漂移改变每个唯一键内的路径拼写）下即重写
         * 覆盖文件曾有的一切行，死 id 上的解析命中会插入 FK 必拒的边。
         * 保留的行在任何解析运行前于阶段 A 复活。
         *
         * 【单一批次事务】重建专属批量调优：为 upsert
         * 风暴设 64 MB 页缓存，提交后立即恢复；journal/synchronous 保持
         * 打开时的值——批次本就是单事务，没有额外 fsync 可省。页缓存在
         * 每个退出路径恢复：抛异常的批次（upsert 失败、盘错误）不得把
         * 64 MiB 缓存留在连接上过其寿命；恢复在异常经此作用域退栈时也
         * 运行，故其 PRAGMA 的守卫方式与 ~Tx 守卫 ROLLBACK（db.store.ixx）
         * 相同：第二个失败不得逃出析构。BEGIN IMMEDIATE：DEFERRED 批次会
         * 在首个 upsert 处竞速把 WAL 读快照升级为写锁，协调入队（或任何
         * 其他写者）在该窗口内提交会使升级以 busy_timeout 等不到的 BUSY
         * 失败（SQLITE_BUSY_SNAPSHOT）；在 BEGIN 处取写锁使每个并发写者
         * 成为 busy_timeout 可等的普通锁持有者。存储阶段大小写漂移：新
         * 提取把已索引文件拼成不同拼写时复用行（同文件同 id——符号、
         * 站点、包含、指纹保住链接）并刷新存储拼写；新行会在旧旁复制该
         * 文件及其整个子图。阶段 A：整批符号 upsert；逐 TU 键->id 映射
         * （边源是 TU 局部的）、批次级新鲜键集合。对批次联合的逐文件替换，
         * 一次、先于任何边（重）写：只碰拥有完整 TU 覆盖（重写集）或首见
         * 的文件——其余已精确持有全量重索引会产生的行。失败与盘缺 TU：
         * 整体清除其行（同树全量重索引一个都不产）并标记。多摘要语料每
         * 路径共享一个文件行：仅当该文件每个作业都失败时清除才正确——
         * 同文件幸存的 ok 作业（多配置编译数据库在多条记录命令下索引同
         * 一路径）刚在本批重新 upsert 过该文件的符号，照样清除会从边阶段
         * 脚下删掉它们、其 upsert 随即引用悬垂符号 id 并死于外键。部分
         * 失败的文件保留其行：ok 作业的 upsert 正是全量重索引所写，仅失败
         * 作业产出的行留待该文件再无作业成功的运行中老去。降级模块 TU 的
         * 第 1 层导入：从未解析的 TU 仍贡献其撰写的导入——
         * 命令携带的 P1689 扫描 requires，落于行 0 使扫描来源的行可与游标
         * 提取的（带真实行号）区分；其导入行被覆盖层替换掉（std 伞、
         * 头单元包含）的已解析 TU 同样落法——重写移走了它们的导入游标。
         * 盘缺主文件保留其行、保持 Failed；id 被记住，使下方 Ok 状态清扫
         * 不能把从未索引过的缺失主文件翻回 Ok（其行在此创建时进入
         * file_ids），而摘要仍报其失败；内容哈希清除：同树从零全量运行
         * 从未给该文件写过哈希（缺失主文件从不解析），已索引主文件哈希
         * 背后的字节已不存在。阶段 B：解析 + upsert 边、然后包含——只为
         * 重写集内的站点（其余保留其有效行：内容与观察者集合未变，存储行
         * 已等于全量重索引会写的）。模块导入与包含同一逐文件替换纪律
         * （导入主文件拥有其行）。已解析主文件的内容哈希：每个解析被尝试
         * 过的主文件记录其尝试时所依据字节的哈希——含失败解析，因为同树
         * 从零全量运行哈希的恰是那些字节；保留编辑前值会使下一运行的
         * stat 确认对着已不存在的内容比较。第二遍解析加入者（哈希为空）不
         * 碰：内容未变、存储哈希仍立。确保每个被跟踪文件带哈希：stat
         * 确认路径需要参照，无哈希的文件不可验证（checkout 后第一次
         * mtime 变化会重解析每个观察者而非按内容确认）。指纹：每个 TU
         * 主文件带其命令摘要，其余被跟踪文件 ''；别名条目共享拥有者的
         * stat 与摘要；stat 变而内容同的文件刷新 stat 使下一运行走快路径。
         * 退役摘要：库必须精确持有从零全量索引会写的指纹——每个存活
         * 编译数据库条目每主文件一行（多命令下的主文件保多行）、其余被
         * 跟踪文件 ''。变更的命令、丢弃的条目、或缺失主文件留下的
         * （文件, 摘要）行此前的运行从未移除（--full 也不）；修剪它，使
         * 表收敛到全量重索引状态而非累积历史。孤儿清扫：经（现已当前的）
         * includes 表无当前 TU 可达的 files 行抹除——已删源、移出编译
         * 数据库的 TU、无人再包含的头；同树同库的全量重索引一个都不产，
         * 故删除是保等价的删除语义；决策核与干跑计划共享。
         *
         * 【收尾】失败命名类聚合（逐文件存于 files.last_error；摘要使类名
         * 无需 -v 即可见），计数降序、名字升序。库 + WAL 字节：批量提交
         * 后多数页仍在预写日志中（关闭时折入主文件），只算主文件会少计。
         */
        auto run(const std::vector<cg::clang::CompileCommand>& commands)
            -> IndexSummary
        {
            const auto   t0 = std::chrono::steady_clock::now();
            IndexSummary sum;
            // 头级缓存与指令门槛状态逐运行复位（引擎复用时上一批的
            // 缓存实例/合并集/摘要载荷不得泄漏进本批）。
            headcache_.reset();
            head_closure_of_.clear();
            bump_feed_.clear();
            c1_digest_writes_.clear();
            // 引擎身份与根集摘要只算一次：失败条目的跳过判定与模块失败
            // 分类器共用它们（原先 run_pool 内每个波次各算一次）。
            const std::string engine_token = detail::engine_token_now();
            const std::string roots_digest =
                detail::roots_digest_of(opt_.roots);
            cg::extract::ExtractOptions extract_opt;
            extract_opt.roots = opt_.roots;
            {
                std::unordered_map<std::string, bool> taught;
                const auto teach = [&](const std::string& dir)
                {
                    if (dir.empty() || dir == "/" || dir.ends_with(":/"))
                    {
                        return;
                    }
                    if (taught.emplace(cg::util::path_compare_key(dir), true)
                            .second)
                    {
                        extract_opt.spellings.push_back(dir);
                    }
                };
                for (const std::string& root : opt_.roots)
                {
                    teach(root);
                }
                for (const cg::clang::CompileCommand& cmd : commands)
                {
                    const std::string& f = cmd.file;
                    for (std::size_t end = f.find('/');
                         end != std::string::npos;
                         end = f.find('/', end + 1))
                    {
                        teach(f.substr(0, end + 1));
                    }
                }
                std::ranges::sort(
                    extract_opt.spellings,
                    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
                    // 误报：分析器对 libc++ ranges::sort
                    // 内部交换的移动模型污染了比较器元素；此处没有任何从 a 或 b
                    // 的移动
                    [](const std::string& a, const std::string& b)
                    {
                        return a.size() != b.size()
                                   ? a.size() > b.size()
                                   : a < b; // NOLINT(clang-analyzer-cplusplus.Move)
                    });
            }
            collect_opt_.roots = extract_opt.roots;
            collect_opt_.spellings = extract_opt.spellings;
            extract_opt_ = std::move(extract_opt);

            sum.tus = static_cast<std::int64_t>(commands.size());
            sum.roots = opt_.roots;
            sum.dry_run = opt_.dry_run;

            // 批内通道：heal 清票的受影响集只属于本批（同 Indexer 多批
            // 场景防跨批污染）。
            heal_vote_affected_.clear();
            const bool folded_damage = !opt_.dry_run
                                       && detail::heal_folded_duplicate_files(
                                           store_,
                                           &heal_vote_affected_);
            // ===== 解析期租约心跳（真库实测事故的修复）=====
            // caller-held（index 接管后的主批与清队追轮、update 的引擎批）
            // 时，解析段整个只读窗口的租约由伴随线程周期续租；提交段拿写
            // 锁前由 commit_batch 停掉它（事务内心跳接管续租职责）。非
            // caller-held（老排队形态：解析期声明尚不属于本进程）、干跑、
            // 内存库不启用。CCG_TEST_PARSE_KEEPALIVE=0 供测试钉住"禁用即
            // 复现病根"的对照臂。
            std::optional<detail::ParseLeaseKeepalive> parse_keepalive;
            if (!opt_.caller_holder.empty() && !opt_.dry_run
                && opt_.db_path != ":memory:"
                && cg::util::cstd::get_env("CCG_TEST_PARSE_KEEPALIVE")
                       != std::optional<std::string>{"0"})
            {
                const std::int64_t keepalive_lease_ms =
                    opt_.caller_lease_ms > 0 ? opt_.caller_lease_ms
                                             : cg::db::default_update_lease_ms;
                const std::int64_t keepalive_period_ms =
                    std::clamp<std::int64_t>(
                        keepalive_lease_ms / 4,
                        1'000,
                        60'000);
                parse_keepalive.emplace(
                    opt_.db_path,
                    opt_.caller_holder,
                    keepalive_lease_ms,
                    keepalive_period_ms);
            }
            const std::vector<cg::db::FileRow> existing_files =
                cg::db::all_files(store_);
            std::unordered_map<
                std::string,
                cg::db::FileRow,
                cg::util::string_hash,
                std::equal_to<>>
                                                             row_by_path;
            std::unordered_map<std::string, cg::db::FileRow> fold_row;
            bool folded_dups = folded_damage;
            for (const cg::db::FileRow& fr : existing_files)
            {
                row_by_path.emplace(fr.path, fr);
                if (!fold_row.emplace(cg::util::path_compare_key(fr.path), fr)
                         .second)
                {
                    folded_dups = true;
                }
            }
            // 只读路径身份单点：精确拼写先查 row_by_path，未命中按小写折叠
            // 键查 fold_row 并经 same_file_spelling 验证，两轮皆空返回哨兵
            // 0。新行登记属提交事务的写侧（file_id_of），读取侧绝不
            // upsert_file——失败依赖反向边、波式分解、fresh 并集与干跑
            // 模拟共用同一折叠纪律，杜绝大小写判定各自漂移。
            const auto read_fid_of =
                [&](const std::string& path) -> cg::db::FileId
            {
                if (const auto it = row_by_path.find(path);
                    it != row_by_path.end())
                {
                    return it->second.id;
                }
                if (const auto it = fold_row.find(cg::util::ascii_lower(path));
                    it != fold_row.end()
                    && detail::same_file_spelling(it->second.path, path))
                {
                    return it->second.id;
                }
                return cg::db::FileId{0};
            };
            std::unordered_map<
                cg::db::FileId,
                std::vector<cg::db::FingerprintRow>>
                fp_by_file;
            for (cg::db::FingerprintRow& fp : cg::db::all_fingerprints(store_))
            {
                fp_by_file[fp.file_id].push_back(std::move(fp));
            }
            // 失败条目按 (file, cmd_digest) 键控快照：与指纹并列，供 pass 1
            // 逐条目判定"重试还是跳过"。vector 在快照后不再改动，plan 里
            // 保存的指针因此始终有效。
            std::unordered_map<cg::db::FileId, std::vector<cg::db::FailureRow>>
                fail_by_file;
            for (cg::db::FailureRow& failure : cg::db::all_failures(store_))
            {
                fail_by_file[failure.file_id].push_back(std::move(failure));
            }
            const std::vector<cg::db::IncludeLink> include_links =
                cg::db::all_includes(store_);

            std::unordered_map<std::string, cg::db::FileRow> drift_row;
            for (const cg::clang::CompileCommand& cmd : commands)
            {
                if (row_by_path.contains(cmd.file))
                {
                    continue;
                }
                const auto it =
                    fold_row.find(cg::util::path_compare_key(cmd.file));
                if (it != fold_row.end()
                    && detail::same_file_spelling(it->second.path, cmd.file))
                {
                    drift_row.emplace(cmd.file, it->second);
                }
            }
            // 大小写漂移只作身份回退，不再是全量强制的理由：Windows 上仅
            // 大小写不同的拼写（数据库行由包含图以另一拼写建立时很常见）
            // 指同一物理文件，行 id 已足以复用全部既有状态，内容新鲜度仍
            // 由 stat/哈希与种子裁决。曾把任何一条漂移条目升级为 force_all
            // ——一次单文件 ccg update 因此重解析整个语料（实测达数百 TU、
            // 耗时以百秒计），远超 PostToolUse 超时。真正无法就地治愈的只有折叠
            // 复行残骸（folded_dups），那仍由 heal_folded_duplicate_files
            // 触发全量。
            const bool force_all = opt_.force || folded_dups;

            const std::unordered_set<std::string> seed_fold = [&]
            {
                std::unordered_set<std::string> out;
                for (const std::string& s : opt_.seed_files)
                {
                    out.insert(cg::util::path_compare_key(s));
                }
                return out;
            }();
            const auto is_seeded = [&seed_fold](std::string_view path)
            {
                return !seed_fold.empty()
                       && seed_fold.contains(cg::util::path_compare_key(path));
            };
            // 历史失败 TU 的"是否重试"完全由 failures 表逐 (file, digest)
            // 判定（见下方 pass 1）：世界状态不变就跳过，点名它的种子仍
            // 然强制重试。旧的"事件驱动不重试任何失败 TU"是粗粒度的——
            // 它无法区分"同一文件另一条成功的编译条目"，而那正是参考
            // 语料 244s/679 TU 波次的来源。

            sum.mode =
                opt_.force || existing_files.empty() ? "full" : "incremental";
            sym_index_.load(store_);

            std::vector<MainPlan> plans;
            plans.reserve(commands.size());
            std::map<std::pair<std::string, std::string>, std::size_t> dedup;

            for (const cg::clang::CompileCommand& cmd : commands)
            {
                MainPlan p;
                p.cmd = &cmd;
                p.digest = detail::parse_digest(cmd);
                const auto key = std::pair{cmd.file, p.digest};
                if (const auto it = dedup.find(key); it != dedup.end())
                {
                    plans.push_back(std::move(p));
                    continue;
                }
                dedup.emplace(key, plans.size());
                plans.push_back(plan_entry_(
                    cmd,
                    std::move(p),
                    row_by_path,
                    drift_row,
                    fp_by_file,
                    fail_by_file,
                    force_all,
                    seed_fold,
                    engine_token,
                    roots_digest));
            }

            std::int64_t skipped = 0;
            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                const auto key = std::pair{plans[i].cmd->file, plans[i].digest};
                const std::size_t first_index = dedup.at(key);
                if (first_index != i)
                {
                    // 重复编译条目共享第一次的计划与唯一一次解析：计数只由
                    // 首条目贡献，否则 fresh/stale/skipped 会把同一 job 数
                    // 两次（参考语料实测 63 vs 62）。
                    continue;
                }
                const MainPlan& first = plans[first_index];
                if (first.missing)
                {
                    ++sum.failed;
                    // 盘缺 TU 的诚实文案：它不进解析批次（无 worker 行）、
                    // failures 表也不落行（盘缺绝不携带失败记录——文件
                    // 重新出现必须重试），不补这行就只剩无出处的
                    // failed 计数。带上预设降级原因首行（如有）：cdb 列
                    // 了、磁盘上没了的模块 TU 其 self-build-unavailable
                    // 文案（含扫描器 stderr 首行）由此才可见。
                    std::println(
                        std::cerr,
                        "  missing TU (skipped, counted as failed): "
                        "{}{}",
                        first.cmd->file,
                        first.cmd->preordained_failure.empty()
                            ? std::string{}
                            : std::format(
                                  " [{}]",
                                  cg::clang::detail::first_line_of(
                                      first.cmd->preordained_failure)));
                }
                else if (first.parse)
                {
                    if (first.fid == 0)
                    {
                        ++sum.fresh;
                    }
                    else
                    {
                        ++sum.stale;
                    }
                }
                else if (first.failure_skip != nullptr)
                {
                    // 历史失败、世界状态未变：仍如实计入 failed（不是
                    // skipped——这些 TU 本次没有产出），只是不重试。
                    ++sum.failed;
                }
                else
                {
                    ++skipped;
                }
            }

            std::unordered_set<std::string> main_paths;
            for (const cg::clang::CompileCommand& cmd : commands)
            {
                main_paths.insert(cmd.file);
            }
            // 漂移条目的库内拼写与命令拼写不同：按路径文本比较会把那条
            // 既有行误当成"非主文件"（stat/内容脏检查、甚至抹除集）。
            // fid 是身份，故主文件判定同时认计划里解析到的行 id。
            std::unordered_set<cg::db::FileId> main_file_ids;
            for (const MainPlan& p : plans)
            {
                if (p.fid != 0)
                {
                    main_file_ids.insert(p.fid);
                }
            }
            detail::IncludeResolver                         resolver;
            std::unordered_map<cg::db::FileId, std::string> path_of;
            for (const cg::db::FileRow& fr : existing_files)
            {
                resolver.add(fr.path, fr.id);
                path_of.emplace(fr.id, fr.path);
            }
            std::unordered_map<cg::db::FileId, std::vector<cg::db::FileId>>
                rev; // 反向边：目标 -> 包含者（保守解析）
            for (const cg::db::IncludeLink& link : include_links)
            {
                const auto it = path_of.find(link.includer);
                if (it == path_of.end())
                {
                    continue;
                }
                for (const cg::db::FileId target :
                     resolver.resolve(link.target, it->second))
                {
                    rev[target].push_back(link.includer);
                }
            }
            // 失败 TU 的 includes 行在失败时已被清除，靠 includes 表无法
            // 传播其依赖变化；把失败记录里的依赖闭包并入反向边，使依赖
            // 内容变脏/消失时该失败 TU 进入 reached 并强制重试。
            for (const auto& [fid, rows] : fail_by_file)
            {
                for (const cg::db::FailureRow& failure : rows)
                {
                    for (const std::string_view dep :
                         detail::dep_paths(failure.deps_json))
                    {
                        const cg::db::FileId dep_id =
                            read_fid_of(std::string{dep});
                        if (dep_id != 0)
                        {
                            rev[dep_id].push_back(fid);
                        }
                    }
                }
            }

            // import 边并入反向图：import 是 modules 时代的包含——
            // "解析范围=包含闭包"在 modules 语料上的正确读法是 #include
            // ∪ import。编辑模块接口（改导出面/删类型）后，import 它的
            // 消费 TU 在 #include 图上与接口无任何边，不补这层边它们就
            // 不进闭包、不重解析，提取产物就地过期（增量库≠全量库）——
            // 向构建系统的依赖级联看齐：BMI 一变，全部 import 消费者重编。
            //
            // 模块名 → 接口文件的映射由命令面两跳拼出（库内不持久化
            // 模块归属，两条既有机制——CXXModules.json 的 BmiMap 与自建
            // 链的 P1689 provides——都活在路由层，到不了这里；而每条命令
            // 都携带两跳的原材料）：
            //   模块名 --（任一条目的 module_feeds 投喂）--> pcm 绝对路径
            //   pcm   --（接口单元自己的 module_output 产出）--> 接口源文件
            // 边取两路并集：
            //   imports 表的书写 import 行（含降级 TU 的行 0 扫描行）；
            //   命令的投喂面（module_feeds 是解析真正消费过的 BMI 集合，
            //   与构建系统的依赖清单同宽——它还覆盖无书写 import 的模块
            //   实现单元：实现单元经投喂隐式依赖自己的接口，imports 表
            //   对此无行）。
            // 映射缺失（外部预建 BMI 如 std、产出条目不在本编译库）的
            // 模块名跳过并计数，绝不崩：多解析一只 TU 是代价，漏解析一
            // 只 TU 是正确性缺口，宁可前者。
            std::unordered_map<std::string, std::string> interface_of_pcm;
            for (const cg::clang::CompileCommand& cmd : commands)
            {
                if (!cmd.module_output.empty())
                {
                    interface_of_pcm.emplace(cmd.module_output, cmd.file);
                }
            }
            // 名字 → pcm 集合：同名多 pcm（外部投喂与库内自建并存、多配
            // 置同名异源）时任一 pcm 命中产出条目即成边——按名首写胜出会
            // 让命令序在先的外部条目遮蔽库内自建 pcm，整模块的 import 边
            // 静默转 unmapped（消费者不进闭包、增量库≠全量库）。
            std::unordered_map<std::string, std::unordered_set<std::string>>
                pcms_of_module;
            for (const cg::clang::CompileCommand& cmd : commands)
            {
                for (const cg::clang::ModuleFeed& feed : cmd.module_feeds)
                {
                    pcms_of_module[feed.name].insert(feed.pcm);
                }
            }
            std::int64_t import_edges = 0;
            std::int64_t import_unmapped = 0;
            // unmapped 分列（诊断面，和恒等于 import_unmapped）：名字无投
            // 喂 = std/外部预建（正确跳过）；有投喂但无产出条目 = pcm 的
            // 生产者不在本编译库（外部预建投喂）；头单元拼写 = 头路径名
            // 字形态（其闭包由 #include 面承载，此处恒为噪音）。
            std::int64_t import_unmapped_unfed_name = 0;
            std::int64_t import_unmapped_fed_no_iface = 0;
            std::int64_t import_unmapped_header_unit = 0;
            // 接边块的无入边跳过（合法形态计数，使命令拼写漂移导致的边
            // 丢失可观测）：接口行不存在（fresh 主文件，本就要解析）或
            // 自指；消费者无文件行（cmd.file 拼写与 files 表行不齐）。
            std::int64_t import_skip_no_iface_row = 0;
            std::int64_t import_skip_no_consumer_row = 0;
            const auto   add_import_edge =
                [&](const std::string& module_name, cg::db::FileId consumer)
            {
                const auto feed_it = pcms_of_module.find(module_name);
                if (feed_it == pcms_of_module.end())
                {
                    ++import_unmapped;
                    if (cg::clang::detail::looks_like_header_unit(module_name))
                    {
                        ++import_unmapped_header_unit;
                    }
                    else
                    {
                        ++import_unmapped_unfed_name;
                    }
                    return;
                }
                // 集内多个 pcm 都命中产出条目时取字典序最小的接口路径：
                // 集合迭代序不定，边的目标必须跨运行确定。
                const std::string* iface = nullptr;
                for (const std::string& pcm : feed_it->second)
                {
                    if (const auto iface_it = interface_of_pcm.find(pcm);
                        iface_it != interface_of_pcm.end()
                        && (iface == nullptr || iface_it->second < *iface))
                    {
                        iface = &iface_it->second;
                    }
                }
                if (iface == nullptr)
                {
                    ++import_unmapped;
                    ++import_unmapped_fed_no_iface;
                    return;
                }
                const cg::db::FileId target = read_fid_of(*iface);
                if (target == 0 || target == consumer)
                {
                    // 接口行尚不存在（fresh 主文件，本就要解析）或自指
                    // （不该出现的形态）：不入边。
                    ++import_skip_no_iface_row;
                    return;
                }
                rev[target].push_back(consumer);
                ++import_edges;
            };
            for (const cg::db::ImportLink& link : cg::db::all_imports(store_))
            {
                add_import_edge(link.module_name, link.file);
            }
            for (const cg::clang::CompileCommand& cmd : commands)
            {
                const cg::db::FileId consumer = read_fid_of(cmd.file);
                if (consumer == 0)
                {
                    // fresh 主文件：无既有行可过期，本就要解析；但拼写漂
                    // 移（cmd.file ≠ files 表行）的既有文件也落此处——计
                    // 数使其可观测。
                    ++import_skip_no_consumer_row;
                    continue;
                }
                for (const cg::clang::ModuleFeed& feed : cmd.module_feeds)
                {
                    add_import_edge(feed.name, consumer);
                }
            }
            if (cg::util::cstd::get_env("CCG_DEBUG_DIAGS").has_value()
                && (!pcms_of_module.empty() || import_unmapped > 0))
            {
                try
                {
                    std::println(
                        std::cerr,
                        "IMPORT-CLOSURE modules={} interfaces={} edges={} "
                        "unmapped={} unmapped-unfed-name={} "
                        "unmapped-fed-no-iface={} unmapped-header-unit={} "
                        "edge-skip-no-iface-row={} edge-skip-no-consumer-row={}",
                        pcms_of_module.size(),
                        interface_of_pcm.size(),
                        import_edges,
                        import_unmapped,
                        import_unmapped_unfed_name,
                        import_unmapped_fed_no_iface,
                        import_unmapped_header_unit,
                        import_skip_no_iface_row,
                        import_skip_no_consumer_row);
                }
                catch (...)
                {
                }
            }

            std::unordered_set<cg::db::FileId>  content_dirty;
            std::unordered_set<cg::db::FileId>  erased; // 缺失主文件 + 已删文件
            std::vector<cg::db::FingerprintRow> fp_writes;
            std::unordered_map<cg::db::FileId, std::string> hash_writes;

            for (const cg::db::FileRow& fr : existing_files)
            {
                if (main_paths.contains(fr.path)
                    || main_file_ids.contains(fr.id))
                {
                    continue;
                }
                const bool included_somewhere = rev.contains(fr.id);
                if (cg::util::is_cpp_source_file(fr.path)
                    && !included_somewhere)
                {
                    erased.insert(fr.id);
                    continue;
                }
                const detail::FileStat st = detail::stat_file(fr.path);
                if (!st.ok)
                {
                    erased.insert(fr.id);
                    continue;
                }
                const auto note_dirty = [&](const std::string& h)
                {
                    content_dirty.insert(fr.id);
                    hash_writes[fr.id] = h;
                    fp_writes.push_back(
                        {.file_id = fr.id,
                         .mtime_ns = st.mtime_ns,
                         .size = st.size,
                         .cmd_digest = ""});
                };
                if (is_seeded(fr.path))
                {
                    note_dirty(detail::hash_file(fr.path));
                    continue;
                }
                const cg::db::FingerprintRow* fp = nullptr;
                if (const auto fp_it = fp_by_file.find(fr.id);
                    fp_it != fp_by_file.end())
                {
                    for (const cg::db::FingerprintRow& cand : fp_it->second)
                    {
                        if (cand.cmd_digest.empty())
                        {
                            fp = &cand;
                            break;
                        }
                    }
                }
                if (fr.status != cg::util::ParseStatus::Ok || fp == nullptr)
                {
                    note_dirty(detail::hash_file(fr.path));
                    continue;
                }
                const auto mark_refresh = [&]()
                {
                    fp_writes.push_back(
                        {.file_id = fr.id,
                         .mtime_ns = st.mtime_ns,
                         .size = st.size,
                         .cmd_digest = ""});
                };
                if (opt_.paranoid)
                {
                    const std::string h = detail::hash_file(fr.path);
                    if (fr.content_hash.has_value() && *fr.content_hash == h)
                    {
                        if (fp->mtime_ns != st.mtime_ns || fp->size != st.size)
                        {
                            mark_refresh();
                        }
                    }
                    else
                    {
                        note_dirty(h);
                    }
                }
                else if (fp->mtime_ns == st.mtime_ns && fp->size == st.size)
                {
                }
                else
                {
                    const std::string h = detail::hash_file(fr.path);
                    if (fr.content_hash.has_value() && *fr.content_hash == h)
                    {
                        mark_refresh();
                    }
                    else
                    {
                        note_dirty(h);
                    }
                }
            }

            std::unordered_set<cg::db::FileId> reached; // 种子 + 包含者
            std::unordered_set<cg::db::FileId> walked;

            // ===== 头级缓存与指令门槛的公共输入 =====
            // 包含图前向边（includer -> 解析目标）提前构建：下方闭包摘要
            // （缓存键输入）与第二遍解析（配置变更域）的包含差量共用
            // （原先在第一遍解析之后建，头级缓存需要在解析池之前拿到——
            // 同一段代码原样上移，下方原址删除）。
            std::unordered_map<cg::db::FileId, std::vector<cg::db::FileId>> fwd;
            for (const cg::db::IncludeLink& link : include_links)
            {
                const auto it = path_of.find(link.includer);
                if (it == path_of.end())
                {
                    continue;
                }
                for (const cg::db::FileId target :
                     resolver.resolve(link.target, it->second))
                {
                    fwd[link.includer].push_back(target);
                }
            }

            // 指令哈希门槛（门控半）：内容真变脏的文件按窄版指令摘要
            // 分流「普通改动/指令改动」（三变体样本误杀 0 的窄版为产品
            // 接线变体；Rider 结构集对 define 值改动敏感——保守判异只损
            // 收益不损正确性）。本批只观测不改行为：计数进摘要与 meta，
            // 摘要基线落 file_instr_digest 供后续运行比对。基线缺席的非脏
            // 文件顺带补扫（全语料一次 0.237s 实测口径）。
            {
                const bool c1_debug =
                    cg::util::cstd::get_env("CCG_DEBUG_C1").has_value();
                const auto old_digests = store_.file_instr_digest_all();
                const auto scan_into = [&](cg::db::FileId     fid,
                                           const std::string& path,
                                           bool               is_main)
                    -> std::optional<cg::util::instrdigest::InstrDigest>
                {
                    std::optional<cg::util::instrdigest::InstrDigest> fresh =
                        cg::util::instrdigest::digest_of_file(path);
                    if (!fresh.has_value())
                    {
                        return std::nullopt;
                    }
                    c1_digest_writes_.push_back(
                        cg::db::FileInstrDigestRow{
                            .file_id = fid,
                            .d_narrow = fresh->narrow,
                            .d_all = fresh->all,
                            .d_include = fresh->include_only,
                            .n_narrow = fresh->n_narrow,
                            .n_all = fresh->n_all,
                            .n_include = fresh->n_include,
                            .ver = cg::db::head_cache_side_ver});
                    if (c1_debug)
                    {
                        try
                        {
                            std::println(
                                std::cerr,
                                "C1 base file='{}' main={} n_narrow={}",
                                path,
                                is_main ? 1 : 0,
                                fresh->n_narrow);
                        }
                        catch (...)
                        {
                        }
                    }
                    return fresh;
                };
                // 内容变脏的既有文件：分类 + 基线更新。
                for (const cg::db::FileId fid : content_dirty)
                {
                    const auto pit = path_of.find(fid);
                    if (pit == path_of.end())
                    {
                        continue;
                    }
                    const bool is_main = main_paths.contains(pit->second)
                                         || main_file_ids.contains(fid);
                    const auto fresh = scan_into(fid, pit->second, is_main);
                    if (!fresh.has_value())
                    {
                        continue;
                    }
                    const auto  oit = old_digests.find(fid);
                    const char* cls = "normal";
                    if (oit == old_digests.end())
                    {
                        cls = "first-seen";
                        ++sum.c1_first_seen;
                    }
                    else if (oit->second.d_narrow != fresh->narrow)
                    {
                        cls = "instr";
                        ++sum.c1_instr_changes;
                    }
                    else
                    {
                        ++sum.c1_normal_changes;
                    }
                    if (c1_debug)
                    {
                        try
                        {
                            std::println(
                                std::cerr,
                                "C1 dirty file='{}' class={} main={}",
                                pit->second,
                                cls,
                                is_main ? 1 : 0);
                        }
                        catch (...)
                        {
                        }
                    }
                }
                // 基线补种：无行的既有非主文件（首跑全量在此一次扫齐）。
                for (const cg::db::FileRow& fr : existing_files)
                {
                    if (content_dirty.contains(fr.id)
                        || old_digests.contains(fr.id)
                        || main_paths.contains(fr.path)
                        || main_file_ids.contains(fr.id))
                    {
                        continue;
                    }
                    scan_into(fr.id, fr.path, false);
                }
            }

            const auto expand_closure =
                [&](const std::vector<cg::db::FileId>& seed_list)
            {
                std::vector<cg::db::FileId> queue;
                for (const cg::db::FileId f : seed_list)
                {
                    if (walked.insert(f).second)
                    {
                        reached.insert(f);
                        queue.push_back(f);
                    }
                }
                while (!queue.empty())
                {
                    const cg::db::FileId f = queue.back();
                    queue.pop_back();
                    if (const auto it = rev.find(f); it != rev.end())
                    {
                        for (const cg::db::FileId includer : it->second)
                        {
                            if (walked.insert(includer).second)
                            {
                                reached.insert(includer);
                                queue.push_back(includer);
                            }
                        }
                    }
                }
            };
            std::vector<cg::db::FileId> rewrite_seeds;
            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                const auto key = std::pair{plans[i].cmd->file, plans[i].digest};
                if (dedup.at(key) != i || plans[i].missing || !plans[i].parse)
                {
                    continue;
                }
                if (plans[i].fid != 0)
                {
                    rewrite_seeds.push_back(plans[i].fid);
                }
            }
            if (force_all)
            {
                rewrite_seeds.reserve(
                    rewrite_seeds.size() + existing_files.size());
                for (const cg::db::FileRow& fr : existing_files)
                {
                    rewrite_seeds.push_back(fr.id);
                }
            }
            expand_closure(rewrite_seeds);
            expand_closure({content_dirty.begin(), content_dirty.end()});
            expand_closure({erased.begin(), erased.end()});

            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                const auto key = std::pair{plans[i].cmd->file, plans[i].digest};
                if (dedup.at(key) != i || plans[i].missing)
                {
                    continue;
                }
                if (plans[i].fid != 0 && !plans[i].parse
                    && reached.contains(plans[i].fid))
                {
                    plans[i].parse = true;
                    ++sum.stale;
                    --skipped;
                }
            }
            sum.skipped = skipped;

            std::vector<StaleTu>  jobs;
            std::vector<TuResult> results;
            const auto            add_job = [&](MainPlan& p) -> bool
            {
                if (!p.parse || p.missing)
                {
                    return false;
                }
                jobs.push_back(
                    {.cmd = p.cmd,
                     .fid = p.fid,
                     .hash = p.hash,
                     .digest = p.digest,
                     .mtime_ns = p.st.mtime_ns,
                     .size = static_cast<std::uintmax_t>(p.st.size)});
                results.emplace_back();
                return true;
            };
            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                const auto key = std::pair{plans[i].cmd->file, plans[i].digest};
                if (dedup.at(key) == i && add_job(plans[i]))
                {
                    sum.dirty_bytes += plans[i].st.size;
                }
            }

            std::unordered_map<std::string, std::string> producer_std_of;
            for (const cg::clang::CompileCommand& cmd : commands)
            {
                if (!cmd.module_output.empty())
                {
                    producer_std_of.emplace(
                        cmd.module_output,
                        detail::std_of_argv(cmd.argv));
                }
            }
            // ===== 波式分解原型（flush-batch 第二轨）=====
            // 解析期逐波拆解+即时释放：成功 TU 的符号/边进全局去重槽，分析
            // 载荷与包含差原料进账本，原始载荷即刻归还——一切库写入仍在提交
            // 事务（波内零库写，kill 重做窗口天然干净）。开关 CCG_WAVE_SIZE
            // （0=关=现状单趟；实施期换成 旗标>库配置>默认16 贯通链）。
            fb_wave_size_ = cg::util::parse_bounded(
                cg::util::cstd::get_env("CCG_WAVE_SIZE"),
                0);
            fb_active_ = fb_wave_size_ > 0;
            // ===== 头级缓存实例化 =====
            // 启用判定（宁可 miss 的总闸）：默认关、CCG_HEADCACHE=1 显式开
            // ——沙盒实测（启用判定口径：687TU 语料热库两轮）命中率 0.5%（远低于
            // 40% 线）且回放轮出现 1 个空键符号漂移（__print@stdexec 头，
            // 根因未诊断完），宁漏勿错：回放路径在语料尺度证明等价之前不
            // 进默认路径。fb 波式分解在开时同样禁用（记录面会被搬空）。
            const bool headcache_on = cg::util::cstd::get_env("CCG_HEADCACHE")
                                          == std::optional<std::string>{"1"}
                                      && !fb_active_;
            if (headcache_on)
            {
                // 闭包摘要（缓存键的包含闭包输入）：每个既有文件在其联合
                // 包含图前向闭包（含自身）上的「路径 × 内容哈希」并集摘要。
                // 本运行的新哈希（hash_writes）即时叠加；无哈希的文件以
                // 哨兵 '?' 入摘要——闭包内出现未知内容即键失配（miss）。
                std::unordered_map<cg::db::FileId, std::string> hash_now;
                hash_now.reserve(existing_files.size() * 2);
                for (const cg::db::FileRow& fr : existing_files)
                {
                    hash_now.emplace(
                        fr.id,
                        fr.content_hash.has_value() ? *fr.content_hash
                                                    : std::string{"?"});
                }
                for (const auto& [fid, h] : hash_writes)
                {
                    hash_now[fid] = h;
                }
                for (const cg::db::FileRow& fr : existing_files)
                {
                    std::vector<std::pair<std::string, const std::string*>>
                                                       parts;
                    std::unordered_set<cg::db::FileId> seen{fr.id};
                    std::vector<cg::db::FileId>        queue{fr.id};
                    while (!queue.empty())
                    {
                        const cg::db::FileId f = queue.back();
                        queue.pop_back();
                        const auto hit = hash_now.find(f);
                        parts.emplace_back(
                            path_of.at(f),
                            hit != hash_now.end() ? &hit->second : nullptr);
                        if (const auto fit = fwd.find(f); fit != fwd.end())
                        {
                            for (const cg::db::FileId t : fit->second)
                            {
                                if (seen.insert(t).second)
                                {
                                    queue.push_back(t);
                                }
                            }
                        }
                    }
                    std::ranges::sort(
                        parts,
                        [](const auto& a, const auto& b)
                        { return a.first < b.first; });
                    std::string buf;
                    for (const auto& [p, h] : parts)
                    {
                        buf += p;
                        buf += '\t';
                        buf += h != nullptr ? *h : std::string{"?"};
                        buf += '\n';
                    }
                    head_closure_of_.emplace(
                        fr.path,
                        cg::util::sha256_hex(std::as_bytes(std::span{buf})));
                }
                headcache_ = std::make_unique<headcache::HeadCache>(
                    engine_token + "#" + roots_digest,
                    &head_closure_of_);
                headcache_->load(store_, path_of);
                // 主文件锚哈希面：每个去重条目的主文件内容哈希（重解析者
                // 用计划算好的新哈希，未动者回退库存哈希——库存哈希在提交
                // 段保证覆盖每个被跟踪文件）。无哈希（新条目）不参与缓存。
                for (std::size_t i = 0; i < plans.size(); ++i)
                {
                    const auto key =
                        std::pair{plans[i].cmd->file, plans[i].digest};
                    if (dedup.at(key) != i)
                    {
                        continue;
                    }
                    if (!plans[i].hash.empty())
                    {
                        hc_main_hash_of_[plans[i].cmd->file] = plans[i].hash;
                    }
                    else if (plans[i].fid != 0)
                    {
                        const auto rit = row_by_path.find(plans[i].cmd->file);
                        if (rit != row_by_path.end()
                            && rit->second.content_hash.has_value())
                        {
                            hc_main_hash_of_[plans[i].cmd->file] =
                                *rit->second.content_hash;
                        }
                    }
                }
                sum.headcache_enabled = 1;
                if (cg::util::cstd::get_env("CCG_DEBUG_C1").has_value())
                {
                    try
                    {
                        std::println(
                            std::cerr,
                            "HEADCACHE enabled files_with_closure={}",
                            head_closure_of_.size());
                    }
                    catch (...)
                    {
                    }
                }
            }
            const auto flush_decompose_wave = [&](std::size_t a, std::size_t b)
            {
                const auto t_fw0 = std::chrono::steady_clock::now();
                // 裁剪打点的"前"值：拆解释放之前的工作集水位。
                const double ws_before_mb =
                    cg::util::platform::working_set_mb();
                // 波内路径身份只读（read_fid_of）：新行登记属提交事务，
                // 这里绝不 upsert_file。
                const auto intern_key = [&](std::string_view s) -> std::uint32_t
                {
                    const auto [it, fresh] = fb_key_intern_.try_emplace(
                        std::string{s},
                        static_cast<std::uint32_t>(fb_key_list_.size()));
                    if (fresh)
                    {
                        fb_key_list_.push_back(&it->first);
                    }
                    return it->second;
                };
                const auto intern_path =
                    [&](std::string_view s) -> std::uint32_t
                {
                    const auto [it, fresh] = fb_path_intern_.try_emplace(
                        std::string{s},
                        static_cast<std::uint32_t>(fb_path_list_.size()));
                    if (fresh)
                    {
                        fb_path_list_.push_back(&it->first);
                    }
                    return it->second;
                };
                const auto mix = [](std::size_t& h, std::size_t v)
                { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
                // 边身份哈希/相等：字符串键去重的等价根据（见 FlushEdgeSlot
                // 注释——到解析后站点键的映射是单射，行集与行序逐字节等价）。
                const auto edge_hash = [&](const cg::extract::EdgeRec& e)
                {
                    std::size_t h = std::hash<std::string_view>{}(e.src_key);
                    mix(h, e.dst.is_key ? 1 : 0);
                    mix(h, std::hash<std::string_view>{}(e.dst.key));
                    mix(h, std::hash<std::string_view>{}(e.dst.meta.name));
                    mix(h, std::hash<std::string_view>{}(e.dst.meta.ns));
                    mix(h, std::hash<std::string_view>{}(e.dst.meta.parent));
                    mix(h,
                        e.dst.meta.sig_hash.has_value()
                            ? static_cast<std::size_t>(*e.dst.meta.sig_hash)
                            : std::size_t{0});
                    mix(h, e.dst.meta.const_method ? 1 : 0);
                    mix(h, static_cast<std::size_t>(e.dst.meta.kind));
                    mix(h, e.dst.meta.arg_dep ? 1 : 0);
                    mix(h, static_cast<std::size_t>(e.kind));
                    mix(h, std::hash<std::string_view>{}(e.site_file));
                    mix(h, static_cast<std::size_t>(e.site_line));
                    return h;
                };
                const auto edge_eq =
                    [&](const FlushEdgeSlot& s, const cg::extract::EdgeRec& e)
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
                };
                for (std::size_t t = a; t < b; ++t)
                {
                    if (!results[t].failure.empty())
                    {
                        continue; // 失败 TU 不进槽（失败串与计时留在 TuResult）
                    }
                    cg::extract::TuExtract& ex = results[t].ex;
                    fb_ledger_.emplace_back();
                    FlushLedgerTu& lt = fb_ledger_.back();
                    lt.job = t;
                    lt.cmd = jobs[t].cmd;
                    lt.fid = jobs[t].fid;
                    lt.digest = jobs[t].digest;
                    lt.analysis = std::move(results[t].analysis);
                    lt.includes = std::move(ex.includes);
                    lt.imports = std::move(ex.imports);
                    lt.keep_keys.reserve(ex.symbols.size());
                    // 第一遍（只读）：槽序维护 + 父别名解析 + 账本 intern。
                    // 末次载荷先记账后搬移——父别名视图指向本 TU 更早的符号
                    // 串，逐槽搬移会使其失效，故两遍分离。
                    std::unordered_map<
                        std::size_t,
                        std::pair<std::size_t, std::string>>
                        last_upd;
                    {
                        std::unordered_map<std::string, std::string_view>
                            record_key;
                        for (std::size_t i = 0; i < ex.symbols.size(); ++i)
                        {
                            const cg::extract::SymbolRec& s = ex.symbols[i];
                            std::string                   parent_key;
                            if (!s.parent.empty())
                            {
                                const std::string alias = parent_alias_of(s);
                                if (!alias.empty())
                                {
                                    if (const auto it = record_key.find(alias);
                                        it != record_key.end())
                                    {
                                        parent_key = it->second; // 拷贝出视图
                                    }
                                }
                            }
                            const auto [it, fresh] = fb_sym_of_.try_emplace(
                                s.unique_key,
                                static_cast<std::uint32_t>(fb_syms_.size()));
                            if (fresh)
                            {
                                fb_syms_.emplace_back();
                            }
                            const auto [uit, ufirst] =
                                last_upd.try_emplace(it->second);
                            static_cast<void>(ufirst); // 首插与更新同途
                            uit->second.first = i;
                            uit->second.second = std::move(parent_key);
                            if (s.kind == cg::extract::SymKind::Record
                                || s.kind == cg::extract::SymKind::Enumeration)
                            {
                                record_key.insert_or_assign(
                                    record_alias(s),
                                    s.unique_key);
                            }
                            lt.keep_keys.push_back(intern_key(s.unique_key));
                            lt.def_paths.push_back(intern_path(s.file));
                            if (const cg::db::FileId f = read_fid_of(s.file);
                                f != 0)
                            {
                                fb_fresh_union_[f].insert(lt.keep_keys.back());
                            }
                        }
                    }
                    // 第二遍（搬移）：末次载荷进槽。
                    for (auto& [slot_idx, upd] : last_upd)
                    {
                        FlushSymSlot& slot = fb_syms_[slot_idx];
                        slot.rec = std::move(ex.symbols[upd.first]);
                        slot.parent_key = std::move(upd.second);
                    }
                    {
                        std::ranges::sort(lt.keep_keys);
                        lt.keep_keys.erase(
                            std::unique(
                                lt.keep_keys.begin(),
                                lt.keep_keys.end()),
                            lt.keep_keys.end());
                        std::ranges::sort(lt.def_paths);
                        lt.def_paths.erase(
                            std::unique(
                                lt.def_paths.begin(),
                                lt.def_paths.end()),
                            lt.def_paths.end());
                    }
                    // 边：字符串键去重（贡献按 TU 记账）。
                    const std::uint32_t tu_idx =
                        static_cast<std::uint32_t>(fb_ledger_.size() - 1);
                    for (const cg::extract::EdgeRec& e : ex.edges)
                    {
                        std::vector<std::uint32_t>& chain =
                            fb_edge_of_[edge_hash(e)];
                        std::uint32_t slot_idx = 0;
                        bool          found = false;
                        for (const std::uint32_t ci : chain)
                        {
                            if (edge_eq(fb_edges_[ci], e))
                            {
                                slot_idx = ci;
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                        {
                            slot_idx =
                                static_cast<std::uint32_t>(fb_edges_.size());
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
                                slot.contribs.back().occurrences +=
                                    e.occurrences;
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
                    // —— 分析载荷去重（槽+账本的本义补全）——
                    // lambda：末写胜出（upsert_symbol 冲突走 SYM_REFRESH 刷
                    // 列），锚定 TU 记账本序号（随陈旧快照 fid 修补联动）。
                    // 线索/站点：首写胜出（upsert 冲突忽略不刷列）——首现即
                    // 首插（rowid 与列载荷同现状），键到库级唯一键单射。
                    for (cg::analysis::TypeHintRec& h : lt.analysis.hints)
                    {
                        std::size_t kh =
                            std::hash<std::string_view>{}(h.owner_key);
                        mix(kh, std::hash<std::string_view>{}(h.file));
                        mix(kh, static_cast<std::size_t>(h.line));
                        mix(kh, static_cast<std::size_t>(h.col));
                        mix(kh, std::hash<std::string_view>{}(h.var_name));
                        mix(kh,
                            static_cast<std::size_t>(
                                static_cast<int>(h.source)));
                        std::vector<std::uint32_t>& chain = fb_hint_of_[kh];
                        bool                        dup = false;
                        for (const std::uint32_t ci : chain)
                        {
                            const cg::analysis::TypeHintRec& e = fb_hints_[ci];
                            if (e.owner_key == h.owner_key && e.file == h.file
                                && e.line == h.line && e.col == h.col
                                && e.var_name == h.var_name
                                && e.source == h.source)
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
                        mix(kh, std::hash<std::string_view>{}(c.file));
                        mix(kh, static_cast<std::size_t>(c.line));
                        mix(kh, static_cast<std::size_t>(c.col));
                        mix(kh, c.is_virtual ? 1 : 0);
                        mix(kh, c.indirect ? 1 : 0);
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
                    for (cg::analysis::LambdaSymRec& l :
                         lt.analysis.lambda_symbols)
                    {
                        const auto [it, fresh] = fb_lam_of_.try_emplace(
                            l.key,
                            static_cast<std::uint32_t>(fb_lams_.size()));
                        if (fresh)
                        {
                            fb_lams_.push_back(
                                FlushLamSlot{
                                    .rec = std::move(l),
                                    .tu = tu_idx});
                        }
                        else
                        {
                            FlushLamSlot& slot = fb_lams_[it->second];
                            slot.rec = std::move(l);
                            slot.tu = tu_idx;
                        }
                    }
                    // 即时释放：重型原始载荷归还。
                    std::vector<cg::analysis::TypeHintRec>().swap(
                        lt.analysis.hints);
                    std::vector<cg::analysis::CallSiteRec>().swap(
                        lt.analysis.call_sites);
                    std::vector<cg::analysis::LambdaSymRec>().swap(
                        lt.analysis.lambda_symbols);
                    std::vector<cg::extract::SymbolRec>().swap(ex.symbols);
                    std::vector<cg::extract::EdgeRec>().swap(ex.edges);
                    std::vector<std::string>().swap(ex.errors);
                }
                ++fb_waves_;
                // 波尾软裁剪：拆解与释放刚把本波尸体页变垃圾，此刻是
                // 物理内存的天然最低点——把"已释放未归还"的堆高水位页
                // 还回操作系统。每波恰一次；禁在波峰或 TU 粒度调用（会把
                // 活数据一并踢出，热页软缺页重填白付代价）。裁剪耗时计入
                // flush 计时段。
                cg::util::platform::trim_working_set();
                const double ws_after_mb = cg::util::platform::working_set_mb();
                t_flush_us_ += std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - t_fw0)
                                   .count()
                               * 1e6;
                if (opt_.progress)
                {
                    // 无裁剪协同的平台（working_set_mb 恒 -1）隐藏打点。
                    const std::string ws_field =
                        ws_before_mb >= 0.0 ? std::format(
                                                  " ws {:.0f}->{:.0f}MB",
                                                  ws_before_mb,
                                                  ws_after_mb)
                                            : std::string{};
                    std::println(
                        std::cerr,
                        "  flush-wave #{}: TUs [{},{}), sym slots {}, edge slots "
                        "{}, lam/hint/site slots {}/{}/{}, ledger TUs {},{} "
                        "total {:.0f}us",
                        fb_waves_,
                        a,
                        b,
                        fb_syms_.size(),
                        fb_edges_.size(),
                        fb_lams_.size(),
                        fb_hints_.size(),
                        fb_sites_.size(),
                        fb_ledger_.size(),
                        ws_field,
                        t_flush_us_);
                }
            };
            const auto run_pool = [&]()
            {
                if (jobs.empty() || results.size() != jobs.size())
                {
                    return;
                }
                const int worker_n = cg::util::pool::resolve_worker_count(
                    opt_.jobs,
                    jobs.size());
                const std::size_t worker_count =
                    static_cast<std::size_t>(worker_n);

                const std::size_t first = result_base_;
                const std::size_t total = jobs.size() - first;
                // 文本回退的观测计数（工人线程原子累加，收尾并入摘要）：
                // 回退后解析成功与仍失败。
                std::atomic<std::int64_t> pch_fallback_ok{0};
                std::atomic<std::int64_t> pch_fallback_failed{0};
                // 诊断驱动改写的两种结局（同款分账）：改写后解析转绿与
                // 重解析后仍失败（后者照常归 msvc-permissive-lenient-
                // specialization 失败类）。
                std::atomic<std::int64_t> dialect_fixit_ok{0};
                std::atomic<std::int64_t> dialect_fixit_failed{0};
                // 编码消毒的两种结局（同款分账）：消毒后解析转绿与重解析
                // 后仍失败（残余场景归 msvc-lenient-source-encoding 类）。
                std::atomic<std::int64_t> encoding_scrub_ok{0};
                std::atomic<std::int64_t> encoding_scrub_failed{0};
                // "报错但 AST 完好"的良性索引计数（白名单家族）：这些 TU
                // 不进失败类，只有成功端一种结局（失败侧照常走 failed）。
                std::atomic<std::int64_t> benign_errored_ok{0};

                if (opt_.progress)
                {
                    std::println(
                        std::cerr,
                        "  parsing {} stale TUs on {} workers (mode: {})",
                        total,
                        worker_count,
                        sum.mode);
                }

                // 调试输出与进度行的共同互斥：解析尝试内部的调试打印与
                // 完成回调的进度行经同一把锁串行，两路输出不会交错。
                std::mutex stderr_mx;
                // 每工人一份的重型状态（工人号 1 起 → 槽号 wid-1）：工人
                // 线程生命周期内跨任务复用；槽由各工人独占读写，无锁。
                std::vector<std::unique_ptr<cg::clang::Index>> index_slots(
                    worker_count);
                std::vector<std::unique_ptr<cg::util::PathCaseCache>>
                    cache_slots(worker_count);
                // 任务体 → 完成回调的传话（两者在同一工人线程上先后被
                // 池调用）：进度行的工人号与拼好的行主体。
                thread_local int         progress_wid = 0;
                thread_local std::string progress_body;

                // WaveRunner（波式分解原型）：按 jobs 序切波（同主文件的相邻
                // TU 并入同波——批不切文件纪律），波末主线程拆解+释放；关波
                // 时单段整趟（与现状完全一致）。pool_a/pool_b 经引用捕获供
                // 池闭包取段基（pool::run 同步执行，值在段内不变）。
                std::size_t pool_a = first;
                std::size_t pool_b = jobs.size();
                std::size_t wave_no = 0;
                for (std::size_t w0 = first; w0 < jobs.size();)
                {
                    std::size_t w1 =
                        fb_active_
                            ? std::min(
                                  w0 + static_cast<std::size_t>(fb_wave_size_),
                                  jobs.size())
                            : jobs.size();
                    while (w1 < jobs.size()
                           && jobs[w1].cmd->file == jobs[w1 - 1].cmd->file)
                    {
                        ++w1;
                    }
                    pool_a = w0;
                    pool_b = w1;
                    ++wave_no;
                    if (opt_.progress && fb_active_)
                    {
                        std::println(
                            std::cerr,
                            "  wave {}: TUs [{},{})",
                            wave_no,
                            w0,
                            w1);
                    }
                    cg::util::pool::run({
                        .count = pool_b - pool_a,
                        .requested = worker_n,
                        .before =
                            [&](std::size_t a, std::size_t b)
                        {
                            return jobs[pool_a + a].size
                                           != jobs[pool_a + b].size
                                       ? jobs[pool_a + a].size
                                             > jobs[pool_a + b].size
                                       : a < b;
                        },
                        .task =
                            [&, this](std::size_t i, int wid)
                        {
                            std::unique_ptr<cg::clang::Index>& index_slot =
                                index_slots[static_cast<std::size_t>(wid - 1)];
                            if (!index_slot)
                            {
                                index_slot =
                                    std::make_unique<cg::clang::Index>();
                            }
                            std::unique_ptr<
                                cg::util::PathCaseCache>& cache_slot =
                                cache_slots[static_cast<std::size_t>(wid - 1)];
                            if (!cache_slot)
                            {
                                cache_slot =
                                    std::make_unique<cg::util::PathCaseCache>();
                            }
                            const cg::clang::Index&  index = *index_slot;
                            cg::util::PathCaseCache& case_cache = *cache_slot;
                            const StaleTu&           tu = jobs[pool_a + i];
                            TuResult&                out = results[pool_a + i];
                            // 影子 PCH 加载失败（PCH/宏/LangOpts
                            // 硬失败）时，本轮 就地重试规划器预先算好的文本回退
                            // argv；成功则不把 TU
                            // 计入失败。只有真正尝试过解析才重试。
                            const bool pch_shadow =
                                tu.cmd->pch.status
                                    == cg::clang::PchUse::Status::shadow
                                && !tu.cmd->pch_fallback_argv.empty();
                            // 重试阶梯：记录 argv → PCH
                            // 文本回退。档位只向后走，
                            // 触发条件各自守卫（无隐式旗标注入：忠实重放，失败
                            // 如实记名）。单次尝试的完整重放（校验→parse→分类
                            // →守卫化 extract/analysis）在 parse_attempt_。
                            enum class RetryStage
                            {
                                recorded,
                                text_fallback
                            };
                            RetryStage stage = RetryStage::recorded;
                            bool       used_pch_fallback = false;
                            // 影子跳的错误级诊断条数：>0=影子 PCH 装载
                            // 成功、TU 自身诊断失败；0=影子装载本身失败
                            // （无诊断 NULL-TU 或 AST 读异常）——回退
                            // 注记文案按此如实区分。
                            std::size_t shadow_diag_errors = 0;
                            for (;;)
                            {
                                const std::vector<std::string>& argv =
                                    stage == RetryStage::recorded
                                        ? tu.cmd->argv
                                        : tu.cmd->pch_fallback_argv;
                                out.failure.clear();
                                out.ex = {};
                                std::size_t attempt_errors = 0;
                                const bool  did_parse = parse_attempt_(
                                    index,
                                    tu,
                                    argv,
                                    engine_token,
                                    producer_std_of,
                                    case_cache,
                                    stderr_mx,
                                    dialect_fixit_ok,
                                    dialect_fixit_failed,
                                    encoding_scrub_ok,
                                    encoding_scrub_failed,
                                    benign_errored_ok,
                                    out,
                                    attempt_errors);
                                if (stage == RetryStage::recorded)
                                {
                                    shadow_diag_errors = attempt_errors;
                                }
                                if (stage == RetryStage::recorded && pch_shadow
                                    && did_parse && !out.failure.empty())
                                {
                                    used_pch_fallback = true;
                                    stage = RetryStage::text_fallback;
                                    continue; // 影子 PCH 失败：同轮文本回退重试
                                }
                                break;
                            }
                            // 文本回退（规划即回退或影子失败后重试）的两种结局
                            // 分账：解析成功=回退救回；仍失败=pch-fallback-failed
                            // 具名类（不再与纯 pch-unusable 混计）。原因写进
                            // 失败文本（→ failures 表与 files.last_error），
                            // 而不是只留在规划器内。
                            const bool fallback_path =
                                tu.cmd->pch.status
                                    == cg::clang::PchUse::Status::text_fallback
                                || used_pch_fallback;
                            if (out.failure.empty())
                            {
                                if (fallback_path)
                                {
                                    ++pch_fallback_ok;
                                }
                            }
                            else if (fallback_path)
                            {
                                ++pch_fallback_failed;
                                out.failure = std::format(
                                    "pch-fallback-failed: {}",
                                    out.failure);
                                if (tu.cmd->pch.status
                                        == cg::clang::PchUse::Status::
                                            text_fallback
                                    && !tu.cmd->pch.note.empty())
                                {
                                    out.failure += std::format(
                                        " [pch: {}]",
                                        tu.cmd->pch.note);
                                }
                                else if (used_pch_fallback)
                                {
                                    out.failure +=
                                        shadow_diag_errors > 0
                                            ? std::format(
                                                  " [pch: shadow attempt had {} "
                                                  "diagnostic error(s) and the "
                                                  "text fallback also failed]",
                                                  shadow_diag_errors)
                                            : std::string{
                                                  " [pch: shadow PCH load failed "
                                                  "and the text fallback also "
                                                  "failed]"};
                                }
                            }
                            else if (
                                !out.failure.empty()
                                && tu.cmd->pch.status
                                       == cg::clang::PchUse::Status::
                                           replay_degraded
                                && !tu.cmd->pch.note.empty())
                            {
                                out.failure +=
                                    std::format(" [pch: {}]", tu.cmd->pch.note);
                            }
                            // -v 逐 TU 成功行带上 PCH 判定原因（文本回退/被拒/
                            // 构建失败）；失败行的原因已在失败文本里。
                            const std::string pch_suffix =
                                out.failure.empty()
                                        && tu.cmd->pch.status
                                               != cg::clang::PchUse::Status::
                                                   none
                                        && !tu.cmd->pch.note.empty()
                                        && tu.cmd->pch.status
                                               != cg::clang::PchUse::Status::
                                                   shadow
                                    ? std::format(
                                          " [pch: {}]",
                                          tu.cmd->pch.note)
                                    : std::string{};
                            // -v 逐 TU 行注记被改写的 TU（改写细节 +
                            // 重解析结局）
                            const std::string fixit_suffix =
                                out.dialect_fixit_note.empty()
                                    ? std::string{}
                                    : std::format(
                                          " [dialect fixit: {}]",
                                          out.dialect_fixit_note);
                            const std::string scrub_suffix =
                                out.encoding_scrub_note.empty()
                                    ? std::string{}
                                    : std::format(
                                          " [encoding scrub: {}]",
                                          out.encoding_scrub_note);
                            // -v 成功行注记"报错但 AST 完好"的良性索引 TU
                            // （失败行不携带——失败 TU 不写良性注记）。
                            const std::string benign_suffix =
                                out.benign_error_note.empty()
                                    ? std::string{}
                                    : std::format(
                                          " [benign errors: {}]",
                                          out.benign_error_note);
                            // 进度行主体（完成序号由池的完成回调补上）经线程
                            // 局部递给完成回调，在池互斥下打印。
                            progress_wid = wid;
                            if (opt_.progress)
                            {
                                progress_body =
                                    !out.failure.empty()
                                        ? std::format(
                                              "{} (failed: {}){}{}{}",
                                              tu.cmd->file,
                                              out.failure,
                                              pch_suffix,
                                              fixit_suffix,
                                              scrub_suffix)
                                        : std::format(
                                              "{} ({} symbols, {} edges; parse "
                                              "{:.0f}us visit {:.0f}us){}{}{}{}",
                                              tu.cmd->file,
                                              out.ex.symbols.size(),
                                              out.ex.edges.size(),
                                              out.parse_us,
                                              out.visit_us,
                                              pch_suffix,
                                              fixit_suffix,
                                              scrub_suffix,
                                              benign_suffix);
                            }
                        },
                        .on_done =
                            [&](std::size_t done)
                        {
                            if (!opt_.progress)
                            {
                                return;
                            }
                            try
                            {
                                const std::scoped_lock lock{stderr_mx};
                                std::println(
                                    std::cerr,
                                    "  [w{} {}/{}]{} {}",
                                    progress_wid,
                                    done,
                                    pool_b - pool_a,
                                    wave_no != 0 && fb_active_
                                        ? std::format(" wv{}", wave_no)
                                        : std::string{},
                                    progress_body);
                            }
                            catch (...)
                            {
                            }
                        },
                    });
                    if (fb_active_)
                    {
                        flush_decompose_wave(w0, w1);
                    }
                    w0 = w1;
                }

                for (std::size_t i = first; i < results.size(); ++i)
                {
                    t_parse_us_ += results[i].parse_us;
                    t_visit_us_ += results[i].visit_us;
                    t_collect_us_ += results[i].analysis_us;
                }
                sum.pch_fallback_ok = pch_fallback_ok.load();
                sum.pch_fallback_failed = pch_fallback_failed.load();
                // 诊断驱动改写的运行口径聚合（与 text fallback 同款）：计数
                // 两种结局，注记每个被改写的 TU（诊断可观测性）。池分多波
                // 调用时计数累加，注记按波追加。
                sum.dialect_fixit_tus += dialect_fixit_ok.load();
                sum.dialect_fixit_failed_tus += dialect_fixit_failed.load();
                for (std::size_t i = first; i < results.size(); ++i)
                {
                    if (!results[i].dialect_fixit_note.empty()
                        && jobs[i].cmd != nullptr)
                    {
                        sum.dialect_fixit_notes.push_back(
                            std::format(
                                "{}: {}",
                                jobs[i].cmd->file,
                                results[i].dialect_fixit_note));
                    }
                }
                // 编码消毒的运行口径聚合（同款）：计数两种结局，注记每个
                // 被消毒的 TU。
                sum.encoding_scrub_tus += encoding_scrub_ok.load();
                sum.encoding_scrub_failed_tus += encoding_scrub_failed.load();
                for (std::size_t i = first; i < results.size(); ++i)
                {
                    if (!results[i].encoding_scrub_note.empty()
                        && jobs[i].cmd != nullptr)
                    {
                        sum.encoding_scrub_notes.push_back(
                            std::format(
                                "{}: {}",
                                jobs[i].cmd->file,
                                results[i].encoding_scrub_note));
                    }
                }
                // 良性错误索引的运行口径聚合（同款）：计数"报错但 AST
                // 完好"而被照常索引的 TU，注记逐 TU 留痕——与失败类
                // 分账，绝不混入 failed。
                sum.benign_errored_tus += benign_errored_ok.load();
                for (std::size_t i = first; i < results.size(); ++i)
                {
                    if (!results[i].benign_error_note.empty()
                        && jobs[i].cmd != nullptr)
                    {
                        sum.benign_error_notes.push_back(
                            std::format(
                                "{}: {}",
                                jobs[i].cmd->file,
                                results[i].benign_error_note));
                    }
                }
                // 文本回退总数恒等聚合（运行口径）：总数 = 回退解析成功 +
                // 回退仍失败。不再由调用方覆盖为规划器的计划数——两个口径
                // 并到同一行会打架（实测 "333 text fallbacks (155 parse
                // ok, 450 still failed)"，155+450 是运行真相）。
                sum.pch_fallbacks =
                    sum.pch_fallback_ok + sum.pch_fallback_failed;
                // 缓存失效（G）：失败类点名了清单缓存产物时，批次结束
                // 后原子移除对应清单条目并删除产物。下一次运行看到缺失/
                // 尺寸不符即重建，打破“坏缓存永不收敛”。同轮内 PCH 已有
                // 影子→文本回退重试；BMI 的产物重建属于下一轮规划。
                {
                    std::vector<std::string> bad_bmi;
                    std::vector<std::string> bad_pch;
                    for (std::size_t i = first; i < results.size(); ++i)
                    {
                        const std::string& failure = results[i].failure;
                        if (failure.empty())
                        {
                            continue;
                        }
                        const std::string_view cls =
                            detail::failure_class_of(failure);
                        if (cls == "pch-unusable"
                            || cls == "pch-fallback-failed")
                        {
                            if (!jobs[i].cmd->pch.shadow_path.empty())
                            {
                                bad_pch.push_back(jobs[i].cmd->pch.shadow_path);
                            }
                            continue;
                        }
                        if (cls != "bmi-config-mismatch"
                            && cls != "module-bmi-missing")
                        {
                            continue;
                        }
                        for (const cg::clang::ModuleFeed& feed :
                             jobs[i].cmd->module_feeds)
                        {
                            // 只失效本引擎自建缓存；构建树的记录 BMI
                            // 不是 ccg 的产物，删了会破坏用户构建。
                            if (feed.pcm.find("bmi-self-") != std::string::npos)
                            {
                                bad_bmi.push_back(feed.pcm);
                            }
                        }
                    }
                    std::ranges::sort(bad_bmi);
                    {
                        auto [keep, last] = std::ranges::unique(bad_bmi);
                        bad_bmi.erase(keep, last);
                    }
                    std::ranges::sort(bad_pch);
                    {
                        auto [keep, last] = std::ranges::unique(bad_pch);
                        bad_pch.erase(keep, last);
                    }
                    if (!bad_bmi.empty())
                    {
                        static_cast<void>(
                            cg::clang::invalidate_cached_bmi_artifacts(
                                bad_bmi));
                    }
                    if (!bad_pch.empty())
                    {
                        static_cast<void>(
                            cg::clang::invalidate_cached_pch_artifacts(
                                bad_pch));
                    }
                }
                // 头级缓存记录面：本波全部成功 TU 的非主分区落账（fb 分解与缓存
                // 互斥已在上游挡掉；记录只在波间（无在飞走查）进行）。
                if (headcache_)
                {
                    for (std::size_t i = first; i < jobs.size(); ++i)
                    {
                        if (results[i].failure.empty())
                        {
                            const auto hit =
                                hc_main_hash_of_.find(jobs[i].cmd->file);
                            if (hit != hc_main_hash_of_.end())
                            {
                                headcache_->record(
                                    jobs[i].digest,
                                    jobs[i].cmd->file,
                                    hit->second,
                                    results[i].ex);
                            }
                        }
                    }
                }
                result_base_ = results.size();
            };
            // 干跑绝不解析（真干跑契约：任何引擎调用之前短路——已存库上
            // 曾照样把 stale TU 喂进解析池，大仓 27 分钟无输出）。计划的
            // stale/fresh 由主文件指纹与命令摘要判定，闭包扩展随解析消失。
            // 测试注入（生产零开销：无变量即什么都不做）：把解析段拉长到
            // 指定毫秒——配短租约（CCG_TEST_TAKEOVER_LEASE_MS）复现"解析
            // 段远超租约时长"的现场；睡眠前落 marker（内容=持有者记号），
            // 供外部轮询"解析段已开始、租约已在手"。
            if (const long long hold_ms = cg::util::parse_bounded(
                    cg::util::cstd::get_env("CCG_TEST_PARSE_HOLD_MS"),
                    0);
                hold_ms > 0)
            {
                if (const auto marker =
                        cg::util::cstd::get_env("CCG_TEST_PARSE_CLAIM_MARKER"))
                {
                    std::ofstream out{
                        *marker,
                        std::ios::binary | std::ios::trunc};
                    if (out)
                    {
                        out << opt_.caller_holder << "\n";
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{hold_ms});
            }
            if (!opt_.dry_run)
            {
                run_pool();
            }

            std::unordered_set<cg::db::SymId> doomed;
            for (const cg::db::FileId f : erased)
            {
                doom_symbols_of_file_(doomed, f);
            }
            for (const MainPlan& p : plans)
            {
                if (p.missing && p.fid != 0)
                {
                    doom_symbols_of_file_(doomed, p.fid);
                }
            }
            // 同一文件可有多条编译条目（不同摘要）。一条成功、一条失败时，
            // 失败条目不得退役成功条目刚重建的符号：那会把符号末日经边站点
            // 扩散到整语料（参考语料实测 11 个失败条目 → 61179 个符号 → 679
            // TU）。 判定按"文件存在任一存活 Ok 条目"：本 run 成功的 job，或本
            // run 未重试（fresh/skipped）且无失败记录的条目（历史 Ok）。
            std::unordered_set<cg::db::FileId> ok_entries;
            std::unordered_set<cg::db::FileId> failed_entries;
            for (std::size_t i = 0; i < jobs.size(); ++i)
            {
                if (jobs[i].fid == 0)
                {
                    continue;
                }
                if (results[i].failure.empty())
                {
                    ok_entries.insert(jobs[i].fid);
                }
                else
                {
                    failed_entries.insert(jobs[i].fid);
                }
            }
            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                const MainPlan& p = plans[i];
                if (dedup.at(std::pair{p.cmd->file, p.digest}) != i
                    || p.fid == 0 || p.parse)
                {
                    continue; // 重复条目 / 无行 / 已由 job 统计
                }
                if (p.missing)
                {
                    failed_entries.insert(p.fid);
                }
                else if (p.failure_skip != nullptr)
                {
                    failed_entries.insert(p.fid);
                }
                else
                {
                    ok_entries.insert(p.fid); // 历史 Ok，本次 fresh
                }
            }
            for (std::size_t i = 0; i < jobs.size(); ++i)
            {
                if (results[i].failure.empty())
                {
                    // 波式分解：keep 集已 intern 化进账本（升序唯一键号），
                    // 与现状的串集合同判定（幂等回查）。
                    if (fb_active_)
                    {
                        // 账本按 job 序追加，游标同序推进。
                        while (fb_keep_cursor_ < fb_ledger_.size()
                               && fb_ledger_[fb_keep_cursor_].job < i)
                        {
                            ++fb_keep_cursor_;
                        }
                        if (fb_keep_cursor_ < fb_ledger_.size()
                            && fb_ledger_[fb_keep_cursor_].job == i)
                        {
                            const FlushLedgerTu& lt =
                                fb_ledger_[fb_keep_cursor_];
                            for (const cg::db::FileSymbolKey& k :
                                 cg::db::file_symbol_keys(store_, lt.fid))
                            {
                                const std::string& key = store_.str_of(k.key);
                                const auto         kit =
                                    fb_key_intern_.find(std::string_view{key});
                                const bool emitted = kit != fb_key_intern_.end()
                                                     && std::binary_search(
                                                         lt.keep_keys.begin(),
                                                         lt.keep_keys.end(),
                                                         kit->second);
                                if (!emitted)
                                {
                                    doomed.insert(k.id);
                                }
                            }
                            continue;
                        }
                    }
                    std::unordered_set<std::string> fresh;
                    for (const cg::extract::SymbolRec& s :
                         results[i].ex.symbols)
                    {
                        fresh.insert(s.unique_key);
                    }
                    for (const cg::db::FileSymbolKey& k :
                         cg::db::file_symbol_keys(store_, jobs[i].fid))
                    {
                        if (!fresh.contains(store_.str_of(k.key)))
                        {
                            doomed.insert(k.id);
                        }
                    }
                    continue;
                }
                const cg::db::FileId fid = jobs[i].fid;
                if (fid != 0 && !ok_entries.contains(fid))
                {
                    doom_symbols_of_file_(doomed, fid);
                }
            }
            // 全部存活条目都失败、但仍有旧符号的文件（成功条目已移出编译
            // 库）：整体清除并播种闭包。参考语料那 3 个"一条成功一条失败"
            // 的文件不满足此条件（ok_entries 非空），因此不会触发级联。
            std::unordered_set<cg::db::FileId> all_failed_fids;
            std::unordered_set<cg::db::FileId> prune_seed_fids;
            for (const cg::db::FileId fid : failed_entries)
            {
                if (!ok_entries.contains(fid))
                {
                    all_failed_fids.insert(fid);
                }
            }
            for (const cg::db::FileId fid : all_failed_fids)
            {
                bool has_symbols = false;
                for (const cg::db::FileSymbolKey& k :
                     cg::db::file_symbol_keys(store_, fid))
                {
                    has_symbols = true;
                    doomed.insert(k.id);
                }
                if (has_symbols)
                {
                    prune_seed_fids.insert(fid);
                }
            }
            detail::cascade_doomed_children(
                store_,
                doomed,
                std::vector<cg::db::SymId>{doomed.begin(), doomed.end()});
            if (const auto v = cg::util::cstd::get_env("CCG_DEBUG_PURGE");
                v.has_value())
            {
                try
                {
                    std::println(
                        std::cerr,
                        "PURGE doomed_after_cascade={} parse_jobs={}",
                        doomed.size(),
                        jobs.size());
                }
                catch (...)
                {
                }
            }
            // 死者社会关系种子收集（doomed 的边站点/定义文件塞 seeds 引
            // 发解析）已删——跨文件影响由分析层全站点重做兜住。seeds 此
            // 后只收配置/内容变更驱动的种子（观察者退出、包含差），其观
            // 察者闭包经第二遍补解析（解析范围封死规则的合法范围）。
            std::vector<cg::db::FileId> seeds;
            std::unordered_map<cg::db::FileId, std::vector<std::string>>
                old_includes;
            for (const cg::db::IncludeLink& link : include_links)
            {
                old_includes[link.includer].push_back(link.target);
            }
            std::unordered_set<cg::db::FileId> seeded;
            const auto                         seed_closure =
                [&](const std::vector<cg::db::FileId>& roots)
            {
                std::vector<cg::db::FileId> queue;
                for (const cg::db::FileId f : roots)
                {
                    if (seeded.insert(f).second)
                    {
                        seeds.push_back(f);
                        reached.insert(f);
                        queue.push_back(f);
                    }
                }
                while (!queue.empty())
                {
                    const cg::db::FileId f = queue.back();
                    queue.pop_back();
                    if (const auto it = fwd.find(f); it != fwd.end())
                    {
                        for (const cg::db::FileId t : it->second)
                        {
                            if (seeded.insert(t).second)
                            {
                                seeds.push_back(t);
                                reached.insert(t);
                                queue.push_back(t);
                            }
                        }
                    }
                }
            };
            for (const cg::db::FileId f : erased)
            {
                seed_closure({f});
            }
            for (std::size_t i = 0; i < jobs.size(); ++i)
            {
                if (!results[i].failure.empty() && jobs[i].fid != 0)
                {
                    seed_closure({jobs[i].fid});
                }
            }
            // failure_skip 绝不播种前向闭包（那正是 633 TU 波次的来源）；
            // 只有"全部存活条目失败且仍有旧符号"的清除才需要播种，让共享
            // 头的出现计数扣掉死 TU 的那一份 bump。
            for (const cg::db::FileId fid : prune_seed_fids)
            {
                seed_closure({fid});
            }
            for (const MainPlan& p : plans)
            {
                if (p.missing && p.fid != 0)
                {
                    seed_closure({p.fid});
                }
            }
            // 快路径输入：主文件条目整组退役标记（下方 live_entries 块
            // 按"有主指纹而当前零条目"置位）。
            bool main_entries_retired = false;
            // 摘要退役（stored != live）的主文件：它们是旧观察者的退出者
            // 之一，计数增量合并的丢失名单要把它们算上（幻影观察者防误并）。
            std::unordered_set<cg::db::FileId> retired_digest_fids;
            {
                std::unordered_map<cg::db::FileId, std::size_t> live_entries;
                const auto entry_fid = [&](const std::string& path)
                {
                    if (const auto it = row_by_path.find(path);
                        it != row_by_path.end())
                    {
                        return it->second.id;
                    }
                    if (const auto it = drift_row.find(path);
                        it != drift_row.end())
                    {
                        return it->second.id;
                    }
                    return cg::db::FileId{0};
                };
                for (const auto& entry : dedup)
                {
                    const cg::db::FileId fid = entry_fid(entry.first.first);
                    if (fid != 0)
                    {
                        ++live_entries[fid];
                    }
                }
                for (const auto& [fid, rows] : fp_by_file)
                {
                    std::size_t stored = 0;
                    for (const cg::db::FingerprintRow& fp : rows)
                    {
                        stored += fp.cmd_digest.empty() ? 0U : 1U;
                    }
                    // 主文件条目整组退役：根集失守（快路径输入之一，
                    // 见 CommitState::main_entries_retired 的说明）。
                    if (stored > 0 && live_entries[fid] == 0)
                    {
                        main_entries_retired = true;
                    }
                    if (stored != live_entries[fid])
                    {
                        retired_digest_fids.insert(fid);
                        seed_closure({fid});
                    }
                }
            }
            {
                // 观察者集合变化的计数增量合并（形态一载体）：先盘点本运行
                // 「失去观察者」的目标文件（抹除/失败/盘缺/摘要退役的包含
                // 者的旧包含目标）。新增观察者（包含差量 add）的目标若不在
                // 任何丢失名单上且内容未变，无需拉全闭包重解析——新增 TU
                // 的贡献走 occurrences bump-upsert（提交段 site_writable 的
                // bump_feed 补充集），其余观察者的行原样保留。
                //
                // 默认关（CCG_HEADCACHE_INCLUDE_MERGE=1 开启）：增量
                // 等价测试（incr.equiv.*）钉住的全闭包重解析语义在本路径
                // 下有边集差（含 friend_of/uses 的行级出入与 occurrences
                // 口径），等价性未收敛前不进默认路径——宁可漏收益不错语义。
                const bool include_merge_on =
                    cg::util::cstd::get_env("CCG_HEADCACHE_INCLUDE_MERGE")
                    == std::optional<std::string>{"1"};
                std::unordered_map<cg::db::FileId, std::size_t> lost_targets;
                {
                    std::vector<cg::db::FileId> lost_includers{
                        erased.begin(),
                        erased.end()};
                    for (std::size_t i = 0; i < jobs.size(); ++i)
                    {
                        if (!results[i].failure.empty() && jobs[i].fid != 0)
                        {
                            lost_includers.push_back(jobs[i].fid);
                        }
                    }
                    for (const MainPlan& p : plans)
                    {
                        if (p.missing && p.fid != 0)
                        {
                            lost_includers.push_back(p.fid);
                        }
                    }
                    for (const cg::db::FileId fid : all_failed_fids)
                    {
                        lost_includers.push_back(fid);
                    }
                    for (const cg::db::FileId fid : retired_digest_fids)
                    {
                        lost_includers.push_back(fid);
                    }
                    for (const cg::db::FileId fid : lost_includers)
                    {
                        const auto pit = path_of.find(fid);
                        if (pit == path_of.end())
                        {
                            continue;
                        }
                        for (const std::string& t : old_includes[fid])
                        {
                            for (const cg::db::FileId target :
                                 resolver.resolve(t, pit->second))
                            {
                                ++lost_targets[target];
                            }
                        }
                    }
                }
                std::unordered_map<std::string, std::vector<std::string>>
                    fresh_by_path;
                if (fb_active_)
                {
                    // 波式分解：包含差原料在账本（搬运序=现状 jobs 序）。
                    for (const FlushLedgerTu& lt : fb_ledger_)
                    {
                        for (const cg::extract::IncludeRec& inc : lt.includes)
                        {
                            fresh_by_path[inc.file].emplace_back(inc.target);
                        }
                    }
                }
                else
                {
                    for (std::size_t i = 0; i < jobs.size(); ++i)
                    {
                        if (!results[i].failure.empty())
                        {
                            continue;
                        }
                        for (const cg::extract::IncludeRec& inc :
                             results[i].ex.includes)
                        {
                            fresh_by_path[inc.file].emplace_back(inc.target);
                        }
                    }
                }
                for (const auto& [path, targets] : fresh_by_path)
                {
                    const auto           row_it = row_by_path.find(path);
                    const cg::db::FileId fid =
                        row_it == row_by_path.end() ? 0 : row_it->second.id;
                    const bool changed_file = fid == 0 || reached.contains(fid);
                    if (!changed_file)
                    {
                        continue;
                    }
                    std::unordered_set<cg::db::FileId> now_ids;
                    for (const std::string& t : targets)
                    {
                        for (const cg::db::FileId f : resolver.resolve(t, path))
                        {
                            now_ids.insert(f);
                        }
                    }
                    std::unordered_set<cg::db::FileId> old_ids;
                    if (fid != 0)
                    {
                        for (const std::string& t : old_includes[fid])
                        {
                            for (const cg::db::FileId f :
                                 resolver.resolve(t, path))
                            {
                                old_ids.insert(f);
                            }
                        }
                    }
                    for (const cg::db::FileId f : now_ids)
                    {
                        if (!old_ids.contains(f))
                        {
                            const auto pit = path_of.find(f);
                            if (const auto v =
                                    cg::util::cstd::get_env("CCG_DEBUG_PURGE");
                                v.has_value())
                            {
                                try
                                {
                                    std::println(
                                        std::cerr,
                                        "PURGE include_diff add target fid={} "
                                        "path='{}' includer='{}'",
                                        f,
                                        pit != path_of.end() ? pit->second
                                                             : std::string{"?"},
                                        path);
                                }
                                catch (...)
                                {
                                }
                            }
                            // 计数增量合并的安全门（五条全过才放行）：
                            // 目标内容未变、未被抹除、无任何观察者退出、
                            // 尚不在重写集、缓存通道在开。任何一条不过都
                            // 回退既有全闭包重解析（正确性优先）。
                            const bool mergeable =
                                include_merge_on && headcache_ != nullptr
                                && !content_dirty.contains(f)
                                && !erased.contains(f)
                                && !lost_targets.contains(f)
                                && !reached.contains(f) && pit != path_of.end();
                            if (mergeable)
                            {
                                bump_feed_.insert(f);
                                if (cg::util::cstd::get_env("CCG_DEBUG_PURGE")
                                        .has_value())
                                {
                                    try
                                    {
                                        std::println(
                                            std::cerr,
                                            "PURGE include_diff add-merge "
                                            "fid={} path='{}'",
                                            f,
                                            pit->second);
                                    }
                                    catch (...)
                                    {
                                    }
                                }
                                continue;
                            }
                            seed_closure({f});
                        }
                    }
                    for (const cg::db::FileId f : old_ids)
                    {
                        if (!now_ids.contains(f))
                        {
                            if (const auto v =
                                    cg::util::cstd::get_env("CCG_DEBUG_PURGE");
                                v.has_value())
                            {
                                const auto pit = path_of.find(f);
                                try
                                {
                                    std::println(
                                        std::cerr,
                                        "PURGE include_diff drop target fid={} "
                                        "path='{}' includer='{}'",
                                        f,
                                        pit != path_of.end() ? pit->second
                                                             : std::string{"?"},
                                        path);
                                }
                                catch (...)
                                {
                                }
                            }
                            seed_closure({f});
                        }
                    }
                }
            }

            // fresh 提取并集（按文件分组；fb 分解下为成员槽 fb_fresh_union_）
            // ——覆盖末日对账域（本批真正重解析过的文件集）的数据面。
            std::unordered_map<cg::db::FileId, std::unordered_set<std::string>>
                fresh_union;
            if (!fb_active_)
            {
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    if (!results[i].failure.empty())
                    {
                        continue;
                    }
                    for (const cg::extract::SymbolRec& s :
                         results[i].ex.symbols)
                    {
                        const cg::db::FileId f = read_fid_of(s.file);
                        if (f != 0)
                        {
                            fresh_union[f].insert(s.unique_key);
                        }
                    }
                }
            }
            // 波式分解：fresh 并集已 intern 化（分解期身份回查同为只读
            // 单点 read_fid_of）——经 intern 号判定同一成员关系。
            const auto fresh_has = [&](cg::db::FileId f, const std::string& key)
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
            };
            // 对账域收死：覆盖末日只对本批 fresh 提取并集覆盖的文件判
            // 键失配。未重解析文件的键缺席 fresh 并集是伪前提——它们
            // 没有重新提取，不代表死亡（旧机制靠第二遍全库重解析把误
            // 判死者捞回，曾把 66 颗种子滚成 56392 个死者、卷入 761 个
            // 文件）。跨文件扩散（死者社会关系经边站点/定义文件引新文
            // 件入域）整体删除——跨文件影响由分析层全站点重做兜住；同
            // 文件级联（级联子：父键死子键失配）保留。
            std::size_t domain_files = 0;
            {
                std::vector<cg::db::SymId> stale;
                const auto reconcile_file = [&](cg::db::FileId f)
                {
                    for (const cg::db::FileSymbolKey& k :
                         cg::db::file_symbol_keys(store_, f))
                    {
                        if (fresh_has(f, store_.str_of(k.key)))
                        {
                            continue;
                        }
                        if (doomed.insert(k.id).second)
                        {
                            stale.push_back(k.id);
                        }
                    }
                };
                if (!fb_active_)
                {
                    domain_files = fresh_union.size();
                    for (const auto& [f, keys] : fresh_union)
                    {
                        (void)keys;
                        reconcile_file(f);
                    }
                }
                else
                {
                    domain_files = fb_fresh_union_.size();
                    for (const auto& [f, keys] : fb_fresh_union_)
                    {
                        (void)keys;
                        reconcile_file(f);
                    }
                }
                detail::cascade_doomed_children(
                    store_,
                    doomed,
                    std::move(stale));
            }
            if (cg::util::cstd::get_env("CCG_DEBUG_PURGE").has_value())
            {
                try
                {
                    std::println(
                        std::cerr,
                        "PURGE covering_doom domain={} doomed={}",
                        domain_files,
                        doomed.size());
                }
                catch (...)
                {
                }
            }

            // 终态复核闸的数据面：首遍全部成功 TU 的新提取键集（本批已
            // 解析者的事实）——fb 分解下符号已搬进全局槽，keep 集在账本
            // （intern 号 → 字符串）。
            std::unordered_set<std::string> successful_tu_fresh_keys;
            if (fb_active_)
            {
                for (const auto& [key, idx] : fb_key_intern_)
                {
                    (void)idx;
                    successful_tu_fresh_keys.insert(key);
                }
                // fb 槽集是去重并集，直接全收（多收只让复核偏保守否决，
                // 绝不放行假命中——见下方 survives 的两分支）。
            }
            else
            {
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    if (results[i].failure.empty())
                    {
                        for (const cg::extract::SymbolRec& s :
                             results[i].ex.symbols)
                        {
                            successful_tu_fresh_keys.insert(s.unique_key);
                        }
                    }
                }
            }
            // 失败/跳过条目的历史键复活集：重试失败（results 非空 failure）
            // 与 failure_skip 的条目本批没有新提取面，但其旧票按增量契约
            // 默认有效——名字可能还在（修复依赖后键会回来）。终态判定只
            // 看 successful_tu_fresh_keys
            // 会把这些条目上批发射过的键判成缺席死亡
            // （"父被伪死"），锚（Extends/Specializes/父锚）随即绕过票数
            // 直删名字还在的行——与票数通道"失败条目旧票有效"同批自相
            // 矛盾。保守方向：失败者上批投过票的键视为 survives，行留票
            // 数渐进通道裁决（条目真退役/全败时票被整撤，归零照删）。
            std::unordered_set<std::string> failed_entry_revived_keys;
            {
                std::vector<std::pair<cg::db::FileId, std::string_view>>
                    revive_entries;
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    if (!results[i].failure.empty() && jobs[i].fid != 0)
                    {
                        revive_entries.emplace_back(
                            jobs[i].fid,
                            jobs[i].digest);
                    }
                }
                for (std::size_t i = 0; i < plans.size(); ++i)
                {
                    const MainPlan& p = plans[i];
                    if (dedup.at(std::pair{p.cmd->file, p.digest}) == i
                        && p.fid != 0 && p.failure_skip != nullptr)
                    {
                        revive_entries.emplace_back(p.fid, p.digest);
                    }
                }
                for (const auto& [fid, digest] : revive_entries)
                {
                    for (const cg::db::SymId sid :
                         store_.contributions_of_entry(fid, digest))
                    {
                        const auto row = store_.symbol_row(sid);
                        if (row.has_value())
                        {
                            failed_entry_revived_keys.insert(row->unique_key);
                        }
                    }
                }
            }
            // 终态判定三档：
            // ① 本运行编辑过的文件（pass 1b 的内容脏非主文件 + 重解析的
            //    既有主文件）或抹除文件：wave-1 新提取键集精确比对；
            // ② 未编辑文件上的模板实例位符号（is_template）：注册点在使
            //    用处、实体挂在模板定义——模板定义文件变了他就死，快捷
            //    通道对它不成立，退到键集精确比对；
            // ③ 未编辑文件上的非模板符号：文件在重写集内 = 观察者全重
            //    解析 = 完整重建同一符号集（键必回来）。
            // c17/c19 两次全库爆发的触发成员（跨 TU 同名拷贝的构造/函数）
            // 都属③——复核直接否决假触发；incr 的 class-delete 等价测试
            // 的实例符号属②——精确比对放行真提升。
            // 重写集取 reached ∪ seeds：第二遍（配置变更域）种子只进
            // seeds、其观察者闭包在探测之后的 expand_closure 才拉满——
            // 但「种子文件的观察者即将全部重解析」在判定时点已经注定，
            // 终态语义不变。名字桶全塞删除后此域只剩配置变更种子，域外
            // 未编辑文件的键也不再进入 doomed（覆盖末日对账域收死）。
            std::unordered_set<cg::db::FileId> edited_fids = content_dirty;
            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                const auto key = std::pair{plans[i].cmd->file, plans[i].digest};
                if (dedup.at(key) == i && plans[i].parse && !plans[i].missing
                    && plans[i].fid != 0)
                {
                    edited_fids.insert(plans[i].fid);
                }
            }
            std::unordered_set<cg::db::FileId> rewrite_will = reached;
            rewrite_will.insert(seeds.begin(), seeds.end());
            const auto terminal_survives = [&](cg::db::SymId  id,
                                               cg::db::FileId fid,
                                               std::uint32_t  flags) -> bool
            {
                const bool edited =
                    edited_fids.contains(fid) || erased.contains(fid);
                const bool instance_like =
                    (flags & cg::db::sym_flags::is_template) != 0;
                if (!edited && !instance_like)
                {
                    return rewrite_will.contains(fid);
                }
                for (const cg::db::FileSymbolKey& k :
                     cg::db::file_symbol_keys(store_, fid))
                {
                    if (k.id == id
                        && (successful_tu_fresh_keys.contains(
                                store_.str_of(k.key))
                            || failed_entry_revived_keys.contains(
                                store_.str_of(k.key))))
                    {
                        return true;
                    }
                }
                return false;
            };

            // doomed 是判死域不是删除域：部分覆盖头键在 rewrite=reached
            // 行清除面之外的伪死者仍计入 purge_lifts 的 pre/post 候选面
            // （结构上删不到它们的行，见上方终态第三档论证）——警报按保守
            // 面解读：多算警报、不多删行，最坏代价是一次多余的全站点
            // 分析重算（保守方向）。
            const bool lifted_ambiguity = sym_index_.purge_lifts_name_ambiguity(
                doomed,
                terminal_survives);

            // 名字桶真触发（复核闸放行）→ 分析层升级（层级正交化）：解
            // 析范围=包含闭包∪配置变更，分析层判定无权把文件塞进解析范
            // 围——旧的 existing_files 全塞种子链已删。警报的消费是置分
            // 析层升级开关：commit 段 dispatch 走全站点决议重做+派发边
            // 整类清空重写（既有 escalation 机制）。
            const bool name_lift_escalation = lifted_ambiguity;
            // 死实参实例行的显式删除集（层级正交化的行清除半边）：以批内
            // 确死符号为模板实参的实例行（如 StaleStrategy 死后挂在未编
            // 辑模板定义文件上的 StrategyBase<StaleStrategy> 基类实例）挂
            // 在其定义文件上——定义文件未编辑不在行清除面（重写集
            // purge），旧机制靠全塞重解析把它们带进 purge 面，该链已删。
            // 锚点用死符号的 Extends 出边目标（CRTP 基类子句的精确库内
            // 事实）：实参类型不存在 ⟹ 任何 TU（含批外观察者）的新提取
            // 都不再产出该实例键，静态真死、不依赖观察者完备。终态第二
            // 档对未编辑文件实例行的键集缺席判定会误杀注册点在批外 TU
            // 的活实例（dup-entry 实测），宏条件键同理（fanin 实测），
            // 均不采用。
            //
            // 锚面扩宽（行生老死记账制的三锚删除件）：同“键拼写内嵌死名 ⟹ 静态
            // 真死”判据的两个新锚，逐形态实证后引入，与既有 Extends 锚
            // 同前提（只认批内编辑/抹除文件上终态确死的键）：
            // - Specializes 反向锚：确死主模板 P 的 specializes 入边
            //   src=实例行——实例行的键内嵌主模板拼写（特化保留拼写），
            //   而发射实例的 TU 必含模板定义（不可见编不出实例化点），
            //   P 名死 ⟹ 无 TU 能再产出任何 P<...> 键。
            // - 父符号锚：确死父符号（parent_id 库内事实，同文件级联已
            //   有、跨文件原先只进判死域不进删除面）的全部子行——子键
            //   的 scope 内嵌父名，发射子（嵌套或类外定义）的 TU 必见父
            //   完整定义、必发射父，父名死 ⟹ 子键无人能再产出。
            // 引用方向（uses 入边 src、type_alias 目标等）不产生删行：
            // 引用行的键不含被引用名，行只是引用死符号——删边留行（级联
            // 清边由 FK 承载）。锚=速判通道，票数归零是渐进通道，两者共
            // 存互补。
            std::vector<cg::db::SymId> dead_symbol_rows;
            {
                for (const cg::db::SymId id : doomed)
                {
                    const auto row = store_.symbol_row(id);
                    if (!row.has_value())
                    {
                        continue;
                    }
                    const bool edited = edited_fids.contains(row->file_id)
                                        || erased.contains(row->file_id);
                    if (!edited
                        || terminal_survives(id, row->file_id, row->flags))
                    {
                        continue; // 只认批内编辑/抹除文件上终态确死的键
                    }
                    for (const cg::db::EdgeRow& e : store_.edges_of_raw(
                             id,
                             true,
                             cg::db::EdgeType::Extends))
                    {
                        if (doomed.contains(e.dst))
                        {
                            dead_symbol_rows.push_back(e.dst);
                        }
                    }
                    // Specializes 反向锚：P 的实例行（I.specializes→P）。
                    // doomed.contains 门槛与 Extends 锚同构：实例行须在
                    // 判死域（对账域内键缺席）——批外实例行（判死域够不
                    // 到）可能还有失败/未重解析条目的活票（源码里名字还
                    // 在，修复依赖后键会回来），交给票数渐进通道。
                    if (row->flags & cg::db::sym_flags::is_template)
                    {
                        for (const cg::db::EdgeRow& e : store_.edges_of_raw(
                                 id,
                                 false,
                                 cg::db::EdgeType::Specializes))
                        {
                            if (doomed.contains(e.src))
                            {
                                dead_symbol_rows.push_back(e.src);
                            }
                        }
                    }
                    // 父符号锚：确死父的全部子行（跨文件级联子原先只进
                    // 判死域不进删除面，本锚补删除半边；同文件子已被
                    // purge 键失配/归零覆盖，重复收集由 erase_symbols 幂
                    // 等吸收；孙行随 parent_id FK 级联消失）。子键的
                    // scope 内嵌父名、发射子的 TU 必见父完整定义，父名
                    // 死 ⟹ 子键无人能再产出——判据与实参锚同构。
                    const auto one = std::array{id};
                    for (const cg::db::SymId child :
                         cg::db::child_symbols(store_, one))
                    {
                        dead_symbol_rows.push_back(child);
                    }
                }
                if (opt_.dispatch)
                {
                    // 三锚并集去重（Extends 实参/特化实例/子成员的收集
                    // 面天然重叠；dispatch 的键名单随之去重）。
                    std::ranges::sort(dead_symbol_rows);
                    dead_symbol_rows.erase(
                        std::unique(
                            dead_symbol_rows.begin(),
                            dead_symbol_rows.end()),
                        dead_symbol_rows.end());
                    for (const cg::db::SymId id : dead_symbol_rows)
                    {
                        const auto row = store_.symbol_row(id);
                        if (row.has_value())
                        {
                            incr_doomed_keys_.push_back(row->unique_key);
                            incr_doomed_names_.push_back(row->name);
                        }
                    }
                }
            }
            if (const auto v = cg::util::cstd::get_env("CCG_DEBUG_PURGE");
                v.has_value() && !dead_symbol_rows.empty())
            {
                try
                {
                    std::println(
                        std::cerr,
                        "PURGE dead_row_erase count={} (Extends bases of "
                        "batch-dead symbols)",
                        dead_symbol_rows.size());
                }
                catch (...)
                {
                }
            }
            if (const auto v = cg::util::cstd::get_env("CCG_DEBUG_PURGE");
                v.has_value())
            {
                try
                {
                    std::println(
                        std::cerr,
                        "PURGE lifted_ambiguity={} -> analysis escalation "
                        "(full-site dispatch resolve) existing_files={} "
                        "seeds={} reached={}",
                        lifted_ambiguity,
                        existing_files.size(),
                        seeds.size(),
                        reached.size());
                }
                catch (...)
                {
                }
            }

            // 配置变更域的第二遍解析（解析范围封死规则的合法扩张）：seeds
            // 此刻只含
            // 编译库/包含图/内容变更驱动的种子（抹除/失败/盘缺/摘要退役
            // 的前向闭包、包含差目标）——名字桶全塞与死者社会关系种子已
            // 删，分析层判定无权出现在这里。种子的观察者闭包拉满后统一
            // 翻位补解析。
            expand_closure(seeds);
            if (const auto v = cg::util::cstd::get_env("CCG_DEBUG_PURGE");
                v.has_value())
            {
                try
                {
                    std::println(
                        std::cerr,
                        "PURGE after_expand seeds={} reached={}",
                        seeds.size(),
                        reached.size());
                }
                catch (...)
                {
                }
            }
            std::size_t added = 0;
            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                const auto key = std::pair{plans[i].cmd->file, plans[i].digest};
                if (dedup.at(key) != i || plans[i].missing)
                {
                    continue;
                }
                if (plans[i].fid != 0 && !plans[i].parse
                    && reached.contains(plans[i].fid))
                {
                    plans[i].parse = true;
                    ++sum.stale;
                    --sum.skipped;
                    add_job(plans[i]);
                    sum.dirty_bytes +=
                        static_cast<std::int64_t>(plans[i].st.size);
                    ++added;
                }
            }
            if (added > 0 && !opt_.dry_run)
            {
                if (const auto v = cg::util::cstd::get_env("CCG_DEBUG_PURGE");
                    v.has_value())
                {
                    try
                    {
                        std::println(
                            std::cerr,
                            "PURGE second_wave added_jobs={} total_stale={}",
                            added,
                            sum.stale);
                    }
                    catch (...)
                    {
                    }
                }
                run_pool();
            }

            if (opt_.dry_run)
            {
                std::unordered_map<std::string, cg::db::FileId>     sim_id;
                std::vector<std::pair<cg::db::FileId, std::string>> sim_files;
                cg::db::FileId sim_next = -1;
                const auto     sim_id_of = [&](const std::string& path)
                {
                    if (const auto it = sim_id.find(path); it != sim_id.end())
                    {
                        return it->second;
                    }
                    // 库内身份走只读单点；库外路径发放负模拟 id（干跑不落库）。
                    cg::db::FileId id = read_fid_of(path);
                    if (id == 0)
                    {
                        id = sim_next--;
                    }
                    sim_id.emplace(path, id);
                    return id;
                };
                for (const cg::db::FileRow& fr : existing_files)
                {
                    sim_files.emplace_back(fr.id, fr.path);
                    sim_id.emplace(fr.path, fr.id);
                }
                std::unordered_set<cg::db::FileId> mentioned;
                std::unordered_set<cg::db::FileId> links_purged;
                std::vector<std::pair<cg::db::FileId, std::string>> sim_links;
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    if (!results[i].failure.empty())
                    {
                        if (jobs[i].fid != 0)
                        {
                            links_purged.insert(jobs[i].fid);
                        }
                        continue;
                    }
                    for (const cg::extract::SymbolRec& s :
                         results[i].ex.symbols)
                    {
                        mentioned.insert(sim_id_of(s.file));
                    }
                    for (const cg::extract::IncludeRec& inc :
                         results[i].ex.includes)
                    {
                        const cg::db::FileId f = sim_id_of(inc.file);
                        mentioned.insert(f);
                        if (reached.contains(f) || f < 0)
                        {
                            sim_links.emplace_back(f, inc.target);
                        }
                    }
                }
                for (const MainPlan& p : plans)
                {
                    if (p.missing && p.fid != 0)
                    {
                        links_purged.insert(p.fid);
                    }
                }
                for (const cg::db::IncludeLink& link : include_links)
                {
                    if ((mentioned.contains(link.includer)
                         && reached.contains(link.includer))
                        || links_purged.contains(link.includer))
                    {
                        continue;
                    }
                    sim_links.emplace_back(link.includer, link.target);
                }
                std::vector<cg::db::FileId> sim_roots;
                sim_roots.reserve(plans.size());
                for (const MainPlan& p : plans)
                {
                    sim_roots.push_back(
                        p.fid != 0 ? p.fid : sim_id_of(p.cmd->file));
                }
                for (const auto& [path, id] : sim_id)
                {
                    if (id < 0)
                    {
                        sim_files.emplace_back(id, path);
                    }
                }
                sum.purged_files = static_cast<std::int64_t>(
                    detail::unreachable_files(sim_files, sim_links, sim_roots)
                        .size());
                const auto t1 = std::chrono::steady_clock::now();
                sum.elapsed_seconds =
                    std::chrono::duration<double>(t1 - t0).count();
                return sum;
            }

            // ===== 提交段（租约协调 → 提交前新鲜度复核 → 单事务写库 →
            // 释放与记账）整体落在 commit_batch 成员函数 =====
            commit_batch(
                {.sum = sum,
                 .plans = plans,
                 .dedup = dedup,
                 .jobs = jobs,
                 .results = results,
                 .existing_files = existing_files,
                 .row_by_path = row_by_path,
                 .fold_row = fold_row,
                 .fp_by_file = fp_by_file,
                 .fail_by_file = fail_by_file,
                 .path_of = path_of,
                 .fwd = fwd,
                 .erased = erased,
                 .old_includes = old_includes,
                 .main_entries_retired = main_entries_retired,
                 .main_paths = main_paths,
                 .ok_entries = ok_entries,
                 .failed_entries = failed_entries,
                 .all_failed_fids = all_failed_fids,
                 .doomed = doomed,
                 .reached = reached,
                 .dead_symbol_rows = dead_symbol_rows,
                 .name_lift_escalation = name_lift_escalation,
                 .fp_writes = fp_writes,
                 .hash_writes = hash_writes,
                 .roots_digest = roots_digest,
                 .run_pool = run_pool,
                 .keepalive = parse_keepalive.has_value() ? &*parse_keepalive
                                                          : nullptr});

            // ===== 头级缓存与指令摘要旁路落盘（提交事务已结束；缓存行可
            // 丢弃再生，失败静默——绝不影响主流程结果）=====
            if (!opt_.dry_run)
            {
                if (headcache_)
                {
                    std::vector<cg::db::FileId> invalidate{
                        content_dirty.begin(),
                        content_dirty.end()};
                    invalidate.insert(
                        invalidate.end(),
                        erased.begin(),
                        erased.end());
                    const auto resolve =
                        [&](const std::string& p) -> cg::db::FileId
                    {
                        const auto it = row_by_path.find(p);
                        return it == row_by_path.end() ? cg::db::FileId{0}
                                                       : it->second.id;
                    };
                    headcache_->persist(store_, resolve, invalidate);
                    const headcache::Stats hc = headcache_->stats();
                    sum.headcache_lookups = hc.lookups;
                    sum.headcache_hits = hc.hits;
                    sum.headcache_records = hc.records;
                    if (opt_.progress
                        || cg::util::cstd::get_env("CCG_DEBUG_C1").has_value())
                    {
                        try
                        {
                            std::println(
                                std::cerr,
                                "  {}",
                                headcache_->debug_line());
                        }
                        catch (...)
                        {
                        }
                    }
                }
                if (!c1_digest_writes_.empty())
                {
                    store_.file_instr_digest_store(c1_digest_writes_);
                }
                store_.set_meta(
                    "c1_normal_changes",
                    std::to_string(sum.c1_normal_changes));
                store_.set_meta(
                    "c1_instr_changes",
                    std::to_string(sum.c1_instr_changes));
                store_.set_meta(
                    "c1_first_seen",
                    std::to_string(sum.c1_first_seen));
            }

            if (opt_.progress && !unresolved_names_.empty())
            {
                std::vector<std::pair<std::string, std::int64_t>> top(
                    unresolved_names_.begin(),
                    unresolved_names_.end());
                std::ranges::sort(
                    top,
                    [](const auto& a, const auto& b)
                    {
                        return a.second != b.second ? a.second > b.second
                                                    : a.first < b.first;
                    });
                const std::size_t shown = std::min<std::size_t>(top.size(), 10);
                std::println(
                    std::cerr,
                    "  ambiguous callees (top {} of {}):",
                    shown,
                    top.size());
                for (std::size_t i = 0; i < shown; ++i)
                {
                    std::println(
                        std::cerr,
                        "    {:>5}  {}",
                        top[i].second,
                        top[i].first);
                }
            }
            if (opt_.progress && !unknown_names_.empty())
            {
                std::int64_t total = 0;
                for (const auto& [name, n] : unknown_names_)
                {
                    total += n;
                }
                std::vector<std::pair<std::string, std::int64_t>> top(
                    unknown_names_.begin(),
                    unknown_names_.end());
                std::ranges::sort(
                    top,
                    [](const auto& a, const auto& b)
                    {
                        return a.second != b.second ? a.second > b.second
                                                    : a.first < b.first;
                    });
                const std::size_t shown = std::min<std::size_t>(top.size(), 10);
                std::println(
                    std::cerr,
                    "  unknown callees, no project symbol of that name "
                    "(top {} of {}, {} drops):",
                    shown,
                    top.size(),
                    total);
                for (std::size_t i = 0; i < shown; ++i)
                {
                    std::println(
                        std::cerr,
                        "    {:>5}  {}",
                        top[i].second,
                        top[i].first);
                }
            }

            store_.set_meta(
                "last_index_at",
                cg::util::cstd::iso8601_utc_timestamp());
            // 本运行阶段 B 的悬解丢弃计数落盘：口径＝本运行发射后解析失败的
            // 边发射数（全量运行也有——外部/未知被调用者是语料固有，同一棵树
            // 全量与增量的库终态逐行一致，此键只是把 stdout/JSON 的既有数字
            // 变成库里可查的每运行事实，避免跨运行对比时误读）。收敛批不解析
            // 任何 TU，由 converge_dispatch 归零，防止旧值冒充当前态。
            store_.set_meta(
                "edges_unresolved_dropped",
                std::to_string(sum.unresolved));
            if (!opt_.compile_db_path.empty())
            {
                store_.set_meta("compile_db_path", opt_.compile_db_path);
            }
            if (!opt_.roots.empty())
            {
                store_.set_meta("project_root", opt_.roots.front());
            }

            const cg::db::Stats st = cg::db::stats(store_);
            sum.symbols = st.symbols;
            sum.edges = st.edges;
            sum.includes = st.includes;
            sum.imports = st.imports;
            // 计数口径与 `ccg stats` 一致：表内实际行数，而不是写下标次数
            // （头文件在多个 TU 重复分析后由 UNIQUE 约束去重）。
            sum.type_hints = st.type_hints;
            sum.call_sites = st.call_sites;
            {
                std::map<std::string, std::int64_t> classes;
                for (const TuResult& res : results)
                {
                    if (!res.failure.empty())
                    {
                        ++classes[std::string{
                            detail::failure_class_of(res.failure)}];
                    }
                }
                // 失败跳过的条目本 run 没有 job，但仍是失败状态——摘要必须
                // 把它们计入失败类，否则会出现"failed > 0 但类为空"。
                for (std::size_t i = 0; i < plans.size(); ++i)
                {
                    const MainPlan& p = plans[i];
                    if (p.failure_skip == nullptr || p.parse
                        || dedup.at(std::pair{p.cmd->file, p.digest}) != i)
                    {
                        continue;
                    }
                    const std::string& cls = p.failure_skip->failure_class;
                    ++classes[cls.empty() ? "parse-failed" : cls];
                }
                sum.failure_classes.assign(classes.begin(), classes.end());
                if (const auto it = classes.find("pch-unusable");
                    it != classes.end())
                {
                    sum.pch_unusable = it->second;
                }
                std::ranges::sort(
                    sum.failure_classes,
                    [](const auto& a, const auto& b)
                    {
                        return a.second != b.second ? a.second > b.second
                                                    : a.first < b.first;
                    });
            }
            for (int i = 0; i < cg::db::edge_type_count; ++i)
            {
                sum.edges_by_type[i] = st.edges_by_type[i];
            }

            // 批后出队用的覆盖集（ccg update）：实际重解析过的每个主 TU
            // （失败的也算——引擎已处理过它们，保留只会让同一批无限重跑）
            // 加闭包传播触及的既有文件。保守：路径无法确定的（尚未入库的
            // 新头文件）不放入。
            {
                std::unordered_set<std::string> covered;
                covered.reserve(reached.size() + jobs.size());
                for (const cg::db::FileId f : reached)
                {
                    if (const auto it = path_of.find(f); it != path_of.end())
                    {
                        covered.insert(it->second);
                    }
                }
                for (const StaleTu& job : jobs)
                {
                    covered.insert(job.cmd->file);
                }
                // 盘缺主文件不进 jobs（无从解析），但引擎已把它们处理成
                // Failed；不纳入会让同一队列条目永远无法出队、反复重跑。
                for (const MainPlan& plan : plans)
                {
                    if (plan.missing)
                    {
                        covered.insert(plan.cmd->file);
                    }
                }
                sum.covered_files.assign(covered.begin(), covered.end());
                std::ranges::sort(sum.covered_files);
            }

            const auto t1 = std::chrono::steady_clock::now();
            sum.elapsed_seconds =
                std::chrono::duration<double>(t1 - t0).count();
            if (opt_.progress)
            {
                if (fb_waves_ > 0)
                {
                    std::println(
                        std::cerr,
                        "  timing totals: parse {:.0f}us "
                        "visit {:.0f}us collect {:.0f}us flush {} waves "
                        "{:.0f}us storeA {:.0f}us "
                        "purge {:.0f}us storeB {:.0f}us commit {:.0f}us "
                        "sweep {:.0f}us",
                        t_parse_us_,
                        t_visit_us_,
                        t_collect_us_,
                        fb_waves_,
                        t_flush_us_,
                        t_store_a_us_,
                        t_purge_us_,
                        t_store_b_us_,
                        t_commit_us_,
                        t_sweep_us_);
                }
                else
                {
                    std::println(
                        std::cerr,
                        "  timing totals: parse {:.0f}us "
                        "visit {:.0f}us collect {:.0f}us storeA {:.0f}us "
                        "purge {:.0f}us storeB {:.0f}us commit {:.0f}us "
                        "sweep {:.0f}us",
                        t_parse_us_,
                        t_visit_us_,
                        t_collect_us_,
                        t_store_a_us_,
                        t_purge_us_,
                        t_store_b_us_,
                        t_commit_us_,
                        t_sweep_us_);
                }
            }
            if (!opt_.db_path.empty() && opt_.db_path != ":memory:")
            {
                std::int64_t    bytes = 0;
                std::error_code ec;
                if (const auto size =
                        std::filesystem::file_size(opt_.db_path, ec);
                    !ec)
                {
                    bytes += static_cast<std::int64_t>(size);
                }
                ec.clear();
                if (const auto size =
                        std::filesystem::file_size(opt_.db_path + "-wal", ec);
                    !ec)
                {
                    bytes += static_cast<std::int64_t>(size);
                }
                sum.db_size_bytes = bytes;
            }
            return sum;
        }

        /**
         * @brief 提交段：租约协调 → 提交前新鲜度复核 → 单事务写库（阶段
         *        A/B、孤儿清扫、dispatch）→ 释放与记账。
         *
         * @internal 从 run 的同段整体搬迁（行为不变），run 头注释的
         * 【单一批段事务】【计划输出】【重写规则】【退役】与【收尾】
         * 段落描述的逻辑在此落地。可达性清扫与事务内心跳是并发保护的
         * 承重对象，逐字保留。开头把共享状态解包回与 run 内同名的局部
         * 引用，搬迁代码不需要任何改名。状态体按值收（成员皆引用，
         * 拷贝即浅拷贝引用本身）。
         */
        void commit_batch(CommitState st)
        {
            IndexSummary&          sum = st.sum;
            std::vector<MainPlan>& plans = st.plans;
            const std::map<std::pair<std::string, std::string>, std::size_t>&
                                                dedup = st.dedup;
            std::vector<StaleTu>&               jobs = st.jobs;
            std::vector<TuResult>&              results = st.results;
            const std::vector<cg::db::FileRow>& existing_files =
                st.existing_files;
            std::unordered_map<
                std::string,
                cg::db::FileRow,
                cg::util::string_hash,
                std::equal_to<>>& row_by_path = st.row_by_path;
            std::unordered_map<std::string, cg::db::FileRow>& fold_row =
                st.fold_row;
            const std::unordered_map<
                cg::db::FileId,
                std::vector<cg::db::FingerprintRow>>& fp_by_file =
                st.fp_by_file;
            const std::unordered_map<
                cg::db::FileId,
                std::vector<cg::db::FailureRow>>& fail_by_file =
                st.fail_by_file;
            const std::unordered_map<cg::db::FileId, std::string>& path_of =
                st.path_of;
            const std::unordered_map<
                cg::db::FileId,
                std::vector<cg::db::FileId>>&         fwd = st.fwd;
            const std::unordered_set<cg::db::FileId>& erased = st.erased;
            const std::unordered_map<cg::db::FileId, std::vector<std::string>>&
                old_includes = st.old_includes;
            const std::unordered_set<std::string>& main_paths = st.main_paths;
            std::unordered_set<cg::db::FileId>&    ok_entries = st.ok_entries;
            std::unordered_set<cg::db::FileId>&    failed_entries =
                st.failed_entries;
            std::unordered_set<cg::db::FileId>& all_failed_fids =
                st.all_failed_fids;
            std::unordered_set<cg::db::SymId>&        doomed = st.doomed;
            const std::unordered_set<cg::db::FileId>& reached = st.reached;
            const bool name_lift_escalation = st.name_lift_escalation;
            const std::vector<cg::db::FingerprintRow>& fp_writes = st.fp_writes;
            const std::unordered_map<cg::db::FileId, std::string>& hash_writes =
                st.hash_writes;
            const std::string& roots_digest = st.roots_digest;
            const auto&        run_pool = st.run_pool;
            // ===== 提交段租约协调（分段认领 + 事务内心跳 + 排队）=====
            // 解析段只读、不持锁不参与协调；从这里到 release，`ccg index`
            // 走 `ccg update` 的既有协议：排队 claim → 提交前新鲜度复核 →
            // 提交段事务内心跳 → COMMIT 后释放 + 记账。coordination 为
            // none 且调用方未自带声明（内存库；干跑）时整块旁路，行为与
            // 协调引入之前逐字节一致。caller-held（caller_holder 非空）：
            // 调用方在进程级已持有/主动偷取了声明（update 的持有者循环、
            // index 尾部的启动偷取），提交段只事务内心跳 + 被偷让位，
            // 不再认领、不建守卫——释放权在调用方的 RAII 手里。
            const bool caller_held = !opt_.caller_holder.empty();
            const bool coordinate =
                (opt_.coordination != IndexOptions::Coordination::none
                 || caller_held)
                && !opt_.dry_run && opt_.db_path != ":memory:";
            std::optional<cg::db::UpdateClaimGuard> coord_guard;
            std::string                             coord_holder;
            std::int64_t                            coord_lease_ms = 0;
            const auto busy_error = [](const std::exception& e)
            {
                const std::string_view msg = e.what();
                return msg.find("database is locked") != std::string_view::npos;
            };
            if (coordinate)
            {
                const auto knob = [&](const char* key, std::int64_t fallback)
                { return cg::util::parse_bounded(store_.meta(key), fallback); };
                // 租约自适应：有提交段实测记账（index_commit_ms，COMMIT 后
                // 写入）时租约盖过它的两倍；首轮无记账用保守大值（两倍默认
                // 租约）——旧二进制在 WAL 下读不到提交段事务内未提交的心跳，
                // 只能看 claim 时写入的初始 lease_until，竞争下的长提交段
                // 期间全靠它判定 live。
                coord_lease_ms =
                    knob("update_lease_ms", cg::db::default_update_lease_ms);
                const std::int64_t recorded_commit_ms =
                    knob("index_commit_ms", 0);
                coord_lease_ms = std::max(
                    coord_lease_ms,
                    2
                        * (recorded_commit_ms > 0
                               ? recorded_commit_ms
                               : cg::db::default_update_lease_ms));
                if (caller_held)
                {
                    // caller-held：声明在进程级已是我们的（update 的持有者
                    // 循环自持 / index 尾部启动时主动偷取），这里只接线心跳
                    // 与让位语义；busy 超时沿用调用方的武装（偷取循环收尾
                    // 时已恢复默认）。租约时长优先取调用方显式值（update
                    // 用自己的 update_lease_ms 旋钮，钩子的活性判定与它同
                    // 一口径），零值回落本函数的自适应公式。
                    coord_holder = opt_.caller_holder;
                    if (opt_.caller_lease_ms > 0)
                    {
                        coord_lease_ms = opt_.caller_lease_ms;
                    }
                    sum.coord_holder = coord_holder;
                }
                else
                {
                    // 声明记号走 db 层的 claim token 单点（cg.db.schema 的
                    // update_claim_token，与 `ccg update`
                    // 同格式的纯文本协议）： claim
                    // 的持有者语义与进程角色无关，跨二进制（msvc hook 与 clang
                    // index 写同一库）的 pid 探活靠它互通（db.store.ixx 的
                    // holder_pid 只认这个形状）。
                    coord_holder = cg::db::update_claim_token();
                    // 排队等待（默认开，--no-wait 快速失败）：claim 落败不弃权
                    // ——读只读快照、报告排在谁后面、封顶退避重试，到 --wait
                    // 上界诚实失败。等待由这个可观测、可中断的循环承担，而非
                    // 把 busy_timeout 武装到分钟级（那会把进程困在 busy
                    // handler 里，无法心跳也无法报告）。claim_update 的 BEGIN
                    // IMMEDIATE 与持有者的写锁直接相撞：探测事务必须配短 busy
                    // （默认 30 s 会把进程钉在锁队列上静默睡到持有者退场——
                    // 其 RAII 届时已删 claim 行，排队者醒来直接拿锁成功，
                    // waiting 报告与心跳全部失声）；claim 到手后恢复默认 busy
                    // 熬提交段。
                    const bool fail_fast =
                        opt_.coordination
                        == IndexOptions::Coordination::fail_fast;
                    store_.set_busy_timeout_ms(
                        cg::db::default_coord_probe_busy_ms);
                    const auto wait_started = std::chrono::steady_clock::now();
                    const auto wait_deadline =
                        wait_started
                        + std::chrono::milliseconds{opt_.wait_limit_ms};
                    const std::chrono::milliseconds backoff_steps[] = {
                        std::chrono::milliseconds{1000},
                        std::chrono::milliseconds{2000},
                        std::chrono::milliseconds{5000},
                        std::chrono::milliseconds{10000}};
                    std::size_t backoff_i = 0;
                    for (;;)
                    {
                        bool claimed = false;
                        try
                        {
                            claimed = store_.claim_update(
                                coord_lease_ms,
                                coord_holder);
                        }
                        catch (const std::exception& e)
                        {
                            // claim 的 BEGIN IMMEDIATE 排在并发引擎批次后面，
                            // busy 耗尽是"写锁被持有"的常态而非失败：按一次
                            // 落败退避重试；其余异常（盘错误）照常上抛。
                            if (!busy_error(e))
                            {
                                throw;
                            }
                        }
                        if (claimed)
                        {
                            break;
                        }
                        if (fail_fast)
                        {
                            throw std::runtime_error(
                                "the database is held by a live update session "
                                "(--no-wait) - re-run later, or `ccg update "
                                "--drain` to converge the queue first");
                        }
                        cg::db::ClaimSnapshot snap;
                        try
                        {
                            snap = store_.claim_snapshot();
                        }
                        catch (const std::exception&)
                        {
                            // 快照只是诊断：读不到就报一行骨架继续等。
                        }
                        const std::int64_t lease_left_s =
                            std::max<std::int64_t>(
                                0,
                                (snap.lease_until - cg::util::cstd::epoch_ms())
                                    / 1000);
                        std::println(
                            std::cerr,
                            "ccg index: waiting for update holder (pid {}, "
                            "~{}s lease left, queue {} file{})",
                            snap.pid,
                            lease_left_s,
                            snap.pending.size(),
                            snap.pending.size() == 1 ? "" : "s");
                        if (std::chrono::steady_clock::now() >= wait_deadline)
                        {
                            throw std::runtime_error(
                                std::format(
                                    "still waiting for the update holder '{}' after "
                                    "{} ms (--wait limit) - re-run later",
                                    snap.holder,
                                    opt_.wait_limit_ms));
                        }
                        std::this_thread::sleep_for(
                            backoff_steps[std::min(backoff_i, std::size_t{3})]);
                        if (backoff_i < 3)
                        {
                            ++backoff_i;
                        }
                    }
                    sum.coord_waited_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - wait_started)
                            .count();
                    // claim 到手：恢复默认 busy——提交段（拿锁/心跳/写入）是
                    // 自己的领地，长 busy 用于熬引擎批次的尾部竞争。
                    store_.set_busy_timeout_ms(cg::db::default_busy_timeout_ms);
                    sum.coord_holder = coord_holder;
                    coord_guard.emplace(store_, coord_holder);
                }
                // 提交前新鲜度复核：解析段（分钟级）期间落地的编辑会让本
                // 批成果相对磁盘过时。对本批每个主 TU 复核 stat（mtime_ns
                // 与 size——指纹的现成身份字段）：变更槽当场重解析（结果
                // 在内存里替换，提交段拾取新值；同 TU 双记录会让边
                // occurrences 双计，故替换而非追加）；缺失/不可读的降级
                // pending——update 事件流的引擎按缺失清除语义收敛，绝不
                // 把已知过时的成果写进提交段。
                {
                    std::vector<std::size_t> rerun;
                    std::vector<std::string> degrade;
                    for (std::size_t i = 0; i < jobs.size(); ++i)
                    {
                        const detail::FileStat st =
                            detail::stat_file(jobs[i].cmd->file);
                        if (st.ok && st.mtime_ns == jobs[i].mtime_ns
                            && st.size
                                   == static_cast<std::int64_t>(jobs[i].size))
                        {
                            continue; // parse 成果仍新鲜
                        }
                        if (st.ok)
                        {
                            rerun.push_back(i);
                        }
                        else
                        {
                            degrade.push_back(jobs[i].cmd->file);
                        }
                    }
                    if (!rerun.empty() || !degrade.empty())
                    {
                        std::println(
                            std::cerr,
                            "ccg: note: {} file{} changed while "
                            "parsing - re-parsing before commit",
                            rerun.size() + degrade.size(),
                            rerun.size() + degrade.size() == 1 ? "" : "s");
                        if (fb_active_)
                        {
                            // 波式分解的复核退路（罕见：解析期文件被改）：
                            // 载荷已拆解释放，无法按现状"保留未变槽+尾部重
                            // 跑"精细替换——整批重解析（本运行余下关波）。
                            // 终态库行集与现状一致（未变文件同内容重解析产
                            // 出相同载荷）；与现状的排布差异：重跑槽留在原
                            // 位（现状挪尾部），首现序可能不同——SymId 与
                            // rowid 的分配序在该罕见路径可能与现状不同；计
                            // 时类计数翻倍。实施期需按 TU 撤销贡献精细化。
                            std::println(
                                std::cerr,
                                "ccg: note: wave buffers dropped - "
                                "full re-parse without wave release");
                            fb_reset_();
                            for (TuResult& r : results)
                            {
                                r = TuResult{};
                            }
                            result_base_ = 0;
                            fb_active_ = false;
                            for (const std::size_t i : rerun)
                            {
                                const detail::FileStat st =
                                    detail::stat_file(jobs[i].cmd->file);
                                jobs[i].mtime_ns = st.mtime_ns;
                                jobs[i].size = st.size;
                                jobs[i].hash =
                                    detail::hash_file(jobs[i].cmd->file);
                                for (MainPlan& p : plans)
                                {
                                    if (p.parse && !p.missing
                                        && p.cmd->file == jobs[i].cmd->file)
                                    {
                                        p.st = st;
                                        p.hash = jobs[i].hash;
                                    }
                                }
                            }
                            run_pool();
                            ++sum.stale;
                        }
                        else
                        {
                            std::vector<bool> stale(jobs.size(), false);
                            for (const std::size_t i : rerun)
                            {
                                stale[i] = true;
                            }
                            for (std::size_t i = 0; i < jobs.size(); ++i)
                            {
                                if (std::ranges::find(
                                        degrade,
                                        jobs[i].cmd->file)
                                    != degrade.end())
                                {
                                    stale[i] = true; // 同文件其余摘要一并降级
                                }
                            }
                            std::vector<StaleTu>  keep_jobs;
                            std::vector<TuResult> keep_results;
                            keep_jobs.reserve(jobs.size());
                            keep_results.reserve(jobs.size());
                            std::vector<StaleTu> redo_jobs;
                            for (std::size_t i = 0; i < jobs.size(); ++i)
                            {
                                if (!stale[i])
                                {
                                    keep_jobs.push_back(std::move(jobs[i]));
                                    keep_results.push_back(
                                        std::move(results[i]));
                                    continue;
                                }
                                if (std::ranges::find(rerun, i) != rerun.end())
                                {
                                    redo_jobs.push_back(std::move(jobs[i]));
                                }
                                // 降级槽：整体剔除（旧成果描述已不存在的字节）
                            }
                            // 重跑槽统一挪到尾部并重置结果——run_pool 按
                            // result_base_ 基线只跑尾部，中部重置槽不会被执行。
                            const std::size_t fresh_base = keep_jobs.size();
                            for (StaleTu& job : redo_jobs)
                            {
                                const detail::FileStat st =
                                    detail::stat_file(job.cmd->file);
                                job.mtime_ns = st.mtime_ns;
                                job.size = st.size;
                                job.hash = detail::hash_file(job.cmd->file);
                                // 指纹写的是 plan 的 stat 身份：同步刷新，下一
                                // 次运行的 stat 快路径对着复核后的字节。
                                for (MainPlan& p : plans)
                                {
                                    if (p.parse && !p.missing
                                        && p.cmd->file == job.cmd->file)
                                    {
                                        p.st = st;
                                        p.hash = job.hash;
                                    }
                                }
                                keep_jobs.push_back(std::move(job));
                                keep_results.emplace_back();
                            }
                            jobs = std::move(keep_jobs);
                            results = std::move(keep_results);
                            result_base_ = fresh_base;
                            run_pool();
                            ++sum.stale; // 复核触发的重解析计入观感口径
                        }
                        // 重跑可能翻转成败：按最新结果重算"文件存在任一存活
                        // Ok 条目"判定——all_failed 的整体清除（提交段）用的
                        // 正是这个差集，翻成功的文件绝不能按旧判定被清空。
                        // 重跑成功槽的旧符号末日由阶段 A 的复活机制回收
                        // （upsert_entry 从 retired_ 抹除，见 run 头注释）。
                        ok_entries.clear();
                        failed_entries.clear();
                        for (std::size_t i = 0; i < jobs.size(); ++i)
                        {
                            if (jobs[i].fid == 0)
                            {
                                continue;
                            }
                            if (results[i].failure.empty())
                            {
                                ok_entries.insert(jobs[i].fid);
                            }
                            else
                            {
                                failed_entries.insert(jobs[i].fid);
                            }
                        }
                        for (std::size_t i = 0; i < plans.size(); ++i)
                        {
                            const MainPlan& p = plans[i];
                            if (dedup.at(std::pair{p.cmd->file, p.digest}) != i
                                || p.fid == 0 || p.parse)
                            {
                                continue; // 重复条目 / 无行 / 已由 job 统计
                            }
                            if (p.missing || p.failure_skip != nullptr)
                            {
                                failed_entries.insert(p.fid);
                            }
                            else
                            {
                                ok_entries.insert(p.fid); // 历史 Ok，本次 fresh
                            }
                        }
                        std::unordered_set<cg::db::FileId> still_all_failed;
                        for (const cg::db::FileId fid : failed_entries)
                        {
                            if (!ok_entries.contains(fid))
                            {
                                still_all_failed.insert(fid);
                            }
                        }
                        for (auto it = all_failed_fids.begin();
                             it != all_failed_fids.end();)
                        {
                            if (still_all_failed.contains(*it))
                            {
                                ++it;
                                continue;
                            }
                            // 翻成功：撤销整体清除，新符号是全量重索引会写的
                            it = all_failed_fids.erase(it);
                        }
                    }
                    if (!degrade.empty())
                    {
                        store_.enqueue_pending(degrade);
                    }
                }
            }

            struct RunTu
            {
                const cg::clang::CompileCommand* cmd = nullptr;
                cg::db::FileId                   fid = 0;
                std::string                      hash;
                std::string digest; // 条目命令摘要（贡献票的条目身份）
                cg::extract::TuExtract   ex;
                cg::analysis::TuAnalysis analysis;
            };
            std::vector<RunTu> batch;
            // 统一消费视图：波式=账本 TU（搬运序=jobs 序）；否则=批 TU
            // （现状）。四类循环（包含/分析载荷/边域/清扫差）共用。
            struct TuView
            {
                const cg::clang::CompileCommand* cmd = nullptr;
                cg::db::FileId                   fid = 0;
                // 条目命令摘要视图（贡献票的条目身份；指向账本/批 TU 的串）。
                std::string_view                            digest;
                const cg::analysis::TuAnalysis*             analysis = nullptr;
                const std::vector<cg::extract::IncludeRec>* includes = nullptr;
                const std::vector<cg::extract::ImportRec>*  imports = nullptr;
            };
            std::vector<TuView> tu_views;
            if (!fb_active_)
            {
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    if (!results[i].failure.empty())
                    {
                        ++sum.failed;
                        continue;
                    }
                    if (results[i].analysis_failed)
                    {
                        ++sum.analysis_failed_tus;
                    }
                    batch.push_back(
                        RunTu{
                            .cmd = jobs[i].cmd,
                            .fid = jobs[i].fid,
                            .hash = jobs[i].hash,
                            .digest = jobs[i].digest,
                            .ex = std::move(results[i].ex),
                            .analysis = std::move(results[i].analysis)});
                }
                tu_views.reserve(batch.size());
                for (const RunTu& tu : batch)
                {
                    tu_views.push_back(
                        TuView{
                            .cmd = tu.cmd,
                            .fid = tu.fid,
                            .digest = tu.digest,
                            .analysis = &tu.analysis,
                            .includes = &tu.ex.includes,
                            .imports = &tu.ex.imports});
                }
            }
            else
            {
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    if (!results[i].failure.empty())
                    {
                        ++sum.failed;
                        continue;
                    }
                    if (results[i].analysis_failed)
                    {
                        ++sum.analysis_failed_tus;
                    }
                }
                tu_views.reserve(fb_ledger_.size());
                for (const FlushLedgerTu& lt : fb_ledger_)
                {
                    tu_views.push_back(
                        TuView{
                            .cmd = lt.cmd,
                            .fid = lt.fid,
                            .digest = lt.digest,
                            .analysis = &lt.analysis,
                            .includes = &lt.includes,
                            .imports = &lt.imports});
                }
            }

            // ===== 贡献记账（schema v18 行生老病死记账制）=====
            // 本批每个成功条目一张选票集：符号发射面（定义级名字）∪ 边
            // 解析命中的目标面（引用级名字——前置声明+指针/引用使用的浅
            // 引用经 uses 边的延迟解析命中也投活票）。条目重解析后与历史
            // 贡献做差：不再贡献的行撤票、仍贡献的续票；未重解析条目的票
            // 按增量契约默认有效。票数归零的行在 dispatch 装载前删除。
            struct EntryVotes
            {
                const cg::clang::CompileCommand*  cmd = nullptr;
                std::string_view                  digest;
                std::unordered_set<cg::db::SymId> votes;
            };
            std::vector<EntryVotes> entry_votes;
            entry_votes.reserve(tu_views.size());
            for (const TuView& tv : tu_views)
            {
                entry_votes.push_back(
                    EntryVotes{.cmd = tv.cmd, .digest = tv.digest});
            }
            const auto votes_of_tu =
                [&](std::size_t t) -> std::unordered_set<cg::db::SymId>&
            { return entry_votes.at(t).votes; };
            // 归零仲裁候选：本批全部撤票动作触及的符号（行从创建起至少
            // 有一票，“从有票变零票”必经一次撤票，故归零集是其子集）。
            std::unordered_set<cg::db::SymId> retracted_votes;

            std::unordered_set<cg::db::FileId> rewrite = reached;

            for (const cg::db::FileId f : rewrite)
            {
                doom_symbols_of_file_(doomed, f);
            }
            detail::cascade_doomed_children(
                store_,
                doomed,
                std::vector<cg::db::SymId>{doomed.begin(), doomed.end()});
            sym_index_.retire(
                std::vector<cg::db::SymId>{doomed.begin(), doomed.end()});

            if (!batch.empty() || !plans.empty())
            {
                store_.set_bulk_cache(true);
                const struct BulkCacheRestore
                {
                    cg::db::GraphStore& store;
                    ~BulkCacheRestore()
                    {
                        try
                        {
                            store.set_bulk_cache(false);
                        }
                        catch (...)
                        {
                        }
                    }
                } bulk_restore{store_};
                // 测试注入（生产零开销：无环境变量即什么都不做）：在提交事务
                // 开锁之前，把指定路径的 files 行连根删除（等价于真实场景里
                // 并发清扫进程在解析期提交的短删除事务），而解析期拿好的
                // row_by_path 快照原样留着——精确复现"快照引用已删行"的
                // 删-插穿插现场，供陈旧 id 复活兜底的回归钉住"不应外键失败"。
                if (const auto victim = cg::util::cstd::get_env(
                        "CCG_TEST_INDEX_DELETE_BEFORE_COMMIT");
                    victim.has_value() && !victim->empty())
                {
                    // 与库内拼写同一归一化（驱动器盘符小写等），否则快照
                    // 查找被大小写漂移误伤。
                    const auto row =
                        row_by_path.find(cg::util::norm_path(*victim));
                    if (row != row_by_path.end() && row->second.id != 0)
                    {
                        store_.erase_file(row->second.id);
                    }
                }
                // 拿锁段心跳保活：claim 与 BEGIN IMMEDIATE 之间的窗口是
                // 唯一可偷窗口（提交段一旦持写锁，偷 claim 的 BEGIN
                // IMMEDIATE 同样被 busy 挡死）。超长 busy_timeout 会把进程
                // 困在 busy handler 里无法心跳（租约在等待中过期必被偷），
                // 故协调模式用默认 30s 分段：失败即退避、先用 peek_pending
                // 心跳续租约（它自己的 IMMEDIATE 抢到锁的瞬间完成续约）再
                // 重试拿锁，同受 --wait 上界约束。非协调路径保持原状。
                //
                // 解析期心跳功成身退（真库实测事故的修复）：提交段即将拿
                // 写锁，续租职责交还事务内的 heartbeat_in_tx——独立连接的
                // 心跳若继续跑，只会与自己的写锁互撞空转。停机后先问一次
                // 偷取判定：被偷就按既有语义干净让位（本批一行业务行都不
                // 落库），绝不带着死租约进入提交事务白做一场。
                if (st.keepalive != nullptr)
                {
                    st.keepalive->stop();
                    sum.parse_lease_beats = st.keepalive->beats();
                    if (st.keepalive->stolen())
                    {
                        throw ClaimStolenError(
                            "ccg index: claim stolen while parsing (lease "
                            "expired) - nothing written; the thief owns the "
                            "queue; re-run when idle");
                    }
                }
                std::optional<cg::db::GraphStore::Tx> tx_opt;
                if (!coordinate)
                {
                    tx_opt.emplace(store_, true);
                }
                else
                {
                    const std::chrono::milliseconds lock_steps[] = {
                        std::chrono::milliseconds{500},
                        std::chrono::milliseconds{1000},
                        std::chrono::milliseconds{2000},
                        std::chrono::milliseconds{5000}};
                    std::size_t lock_backoff = 0;
                    const auto  lock_deadline =
                        std::chrono::steady_clock::now()
                        + std::chrono::milliseconds{opt_.wait_limit_ms};
                    // 快败分类（永久失败保险丝）：本连接忙超时（30s）在身，
                    // 真锁竞争的 BEGIN 会先在忙处理器里等到接近超时才以
                    // busy 失败（慢败——锁很快会放手，重试有意义）；而本连
                    // 接持旧读快照时的快照冲突，忙处理器按 SQLite 语义不介
                    // 入、瞬间失败（快败）——重试永远无法治愈。连续两次快
                    // 败（含 peek 续租事务的同类失败）即中止自旋、抛可行动
                    // 错误，绝不把 --wait 上界（默认 30 分钟）耗成静默挂死。
                    int        fast_busy_streak = 0;
                    const auto count_fast_busy =
                        [&](std::chrono::milliseconds spent)
                    {
                        fast_busy_streak = spent < std::chrono::seconds{5}
                                               ? fast_busy_streak + 1
                                               : 0;
                        return fast_busy_streak >= 2;
                    };
                    for (;;)
                    {
                        const auto attempt_started =
                            std::chrono::steady_clock::now();
                        try
                        {
                            tx_opt.emplace(store_, true);
                            break;
                        }
                        catch (const std::exception& e)
                        {
                            if (!busy_error(e))
                            {
                                throw;
                            }
                            if (count_fast_busy(
                                    std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now()
                                        - attempt_started)))
                            {
                                throw std::runtime_error(
                                    std::format(
                                        "cannot acquire the write lock: {} "
                                        "consecutive instant lock failures on "
                                        "this connection (stale read snapshot "
                                        "shape; the busy handler never "
                                        "engages); statements still stepping "
                                        "on this connection: [{}]; retrying "
                                        "cannot heal this - re-run the "
                                        "command",
                                        fast_busy_streak,
                                        store_.busy_statements_diag()));
                            }
                        }
                        if (std::chrono::steady_clock::now() >= lock_deadline)
                        {
                            throw std::runtime_error(
                                std::format(
                                    "still waiting for the write lock after {} ms "
                                    "(--wait limit) - a concurrent engine batch "
                                    "holds it; re-run later",
                                    opt_.wait_limit_ms));
                        }
                        std::this_thread::sleep_for(
                            lock_steps[std::min(lock_backoff, std::size_t{3})]);
                        if (lock_backoff < 3)
                        {
                            ++lock_backoff;
                        }
                        const auto peek_started =
                            std::chrono::steady_clock::now();
                        bool renewed = false;
                        try
                        {
                            renewed =
                                !store_
                                     .peek_pending(coord_lease_ms, coord_holder)
                                     .claim_lost;
                        }
                        catch (const std::exception& e)
                        {
                            if (!busy_error(e))
                            {
                                throw;
                            }
                            // 快照冲突的续约事务同样瞬间失败，计入同一连
                            // 败；慢败（真竞争）则清零连败重新观察。
                            if (count_fast_busy(
                                    std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now()
                                        - peek_started)))
                            {
                                throw std::runtime_error(
                                    std::format(
                                        "cannot renew the claim while waiting "
                                        "for the write lock: {} consecutive "
                                        "instant lock failures on this "
                                        "connection (stale read snapshot "
                                        "shape); statements still stepping "
                                        "on this connection: [{}] - re-run "
                                        "the command",
                                        fast_busy_streak,
                                        store_.busy_statements_diag()));
                            }
                            // 续约事务自己也在排队失败：写锁仍被持有，此刻
                            // 同样无人能偷（偷 claim 也需要写锁），继续等。
                            renewed = true;
                        }
                        if (!renewed)
                        {
                            throw ClaimStolenError(
                                "ccg index: claim stolen while waiting for the "
                                "write lock - nothing written; the thief owns "
                                "the queue; re-run when idle");
                        }
                    }
                }
                cg::db::GraphStore::Tx& tx = *tx_opt;
                // 提交段事务内心跳：SQLite 无嵌套事务，peek_pending 的短
                // 事务模式在写事务内不可用，但同事务执行已预编译的
                // CLAIM_HEARTBEAT 合法（heartbeat_in_tx）。按 wall clock 节
                // 流（默认 60s），零行 = 租约已被偷，抛出经 ~Tx 干净
                // ROLLBACK——把"双写者"降级为"干净失败重跑"，绝不改写
                // 窃取者的行（UPDATE 带 WHERE holder=?）。
                std::int64_t beat_interval_ms = cg::util::parse_bounded(
                    cg::util::cstd::get_env("CCG_TEST_COMMIT_HEARTBEAT_MS"),
                    60'000);
                std::optional<std::chrono::steady_clock::time_point> next_beat;
                const auto lease_beat = [&]()
                {
                    if (coord_holder.empty())
                    {
                        return; // 非协调：一次字符串判空的代价
                    }
                    const auto beat_now = std::chrono::steady_clock::now();
                    if (next_beat.has_value() && beat_now < *next_beat)
                    {
                        return;
                    }
                    next_beat =
                        beat_now + std::chrono::milliseconds{beat_interval_ms};
                    if (!store_.heartbeat_in_tx(coord_holder, coord_lease_ms))
                    {
                        // 专设异常类型：增量持有者（caller_holder 的 update）
                        // 捕获它优雅让位（队列保留、退出 0），索引路径则
                        // 按普通失败上抛。消息保持既有文本（e2e 断言锚）。
                        throw ClaimStolenError(
                            "ccg index: claim stolen mid-commit (lease "
                            "expired) - nothing written; the thief owns the "
                            "queue; re-run when idle");
                    }
                };
                lease_beat();
                // 测试注入（生产零开销：无环境变量即什么都不做）：提交段
                // 事务内睡眠，复现"提交段持写锁分钟级"的现场（hook 快速
                // 路径与心跳保活据此成为可断言的事实）；可选在睡眠中途把
                // 声明行改写为伪造偷取者（同连接同事务 UPDATE 合法），钉
                // 住"事务内心跳零行 → 干净 ROLLBACK、库行数不变"。
                if (const long long hold_ms = cg::util::parse_bounded(
                        cg::util::cstd::get_env(
                            "CCG_TEST_INDEX_HOLD_COMMIT_MS"),
                        0);
                    hold_ms > 0)
                {
                    // marker 在写事务内、睡眠之前写：e2e 轮询到它即证明
                    // 提交段已持有写锁（hook 快速路径的判定前提）。
                    if (const auto marker = cg::util::cstd::get_env(
                            "CCG_TEST_INDEX_CLAIM_MARKER"))
                    {
                        std::ofstream out{
                            *marker,
                            std::ios::binary | std::ios::trunc};
                        if (out)
                        {
                            out << coord_holder << "\n";
                        }
                    }
                    if (cg::util::cstd::get_env("CCG_TEST_INDEX_STEAL_CLAIM")
                            .has_value())
                    {
                        // 伪造偷取者带死 pid + 远期租约：本连接的心跳立刻
                        // 匹配零行（让位路径可断言），而下一个真实更新者的
                        // pid 探活把它判死偷回（测试收尾无需手工清行）。
                        store_.exec(
                            "update update_claim set holder = "
                            "'ccg-update:pid:4000000000:t1', lease_until = "
                            "99999999999999 where id = 1");
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{hold_ms});
                    lease_beat();
                }
                const auto        t_store0 = std::chrono::steady_clock::now();
                const std::string now = cg::util::cstd::iso8601_utc_timestamp();

                std::unordered_map<std::string, cg::db::FileId> file_ids;
                std::unordered_set<cg::db::FileId> new_files; // 首次索引运行
                // 快路径输入：大小写漂移折叠重拼次数（重拼改变
                // IncludeResolver
                // 注册表里的路径，既有包含行的解析可能漂移——在场不可跳过）。
                std::size_t fold_renames = 0;
                const auto  file_id_of =
                    [&](const std::string& path) -> cg::db::FileId
                {
                    if (const auto it = file_ids.find(path);
                        it != file_ids.end())
                    {
                        return it->second;
                    }
                    cg::db::FileId id = 0;
                    const auto     row = row_by_path.find(path);
                    if (row != row_by_path.end())
                    {
                        id = row->second.id;
                    }
                    else
                    {
                        const auto fold =
                            fold_row.find(cg::util::ascii_lower(path));
                        if (fold != fold_row.end()
                            && detail::same_file_spelling(
                                fold->second.path,
                                path))
                        {
                            id = fold->second.id;
                            ++fold_renames;
                            store_.set_file_path(id, path);
                            row_by_path.erase(fold->second.path);
                            cg::db::FileRow renamed;
                            renamed.id = id;
                            renamed.path = path;
                            fold->second = renamed;
                            row_by_path.emplace(path, std::move(renamed));
                        }
                        else
                        {
                            id = store_.upsert_file(path);
                            new_files.insert(id);
                            cg::db::FileRow fresh_row;
                            fresh_row.id = id;
                            fresh_row.path = path;
                            row_by_path.emplace(path, std::move(fresh_row));
                            fold_row.try_emplace(
                                cg::util::ascii_lower(path),
                                row_by_path.at(path));
                        }
                    }
                    file_ids.emplace(path, id);
                    return id;
                };

                // ===== 陈旧快照核对（并发删除的插入侧兜底）=====
                // 解析期在事务外拿的 file_id 快照（row_by_path/fold_row/
                // plans/jobs 携带的 fid），在"启动偷取生效但对方在飞批仍在
                // 收尾"的过渡窗口里可能已无对应的 files 行（对方提交段先于
                // 本事务删除了它们）。旧 id 直接流进外键列会以 FOREIGN KEY
                // 整批回滚收场（受控双进程实验 2/2 坐实）；这里在写事务内
                // 用一次在场 id 全集作差：死 id 的路径引用改为复活（重新
                // upsert 拿新 id，语义=首见文件），死 id 的快照旁路载荷
                // （指纹/哈希）整体丢弃。单写者常态=零命中，只剩一次主键
                // 列扫描的常数开销。
                {
                    const std::unordered_set<cg::db::FileId> live_ids =
                        cg::db::all_file_ids(store_);
                    std::unordered_set<cg::db::FileId> dead_ids;
                    for (const auto& [path, row] : row_by_path)
                    {
                        static_cast<void>(path);
                        if (row.id != 0 && !live_ids.contains(row.id))
                        {
                            dead_ids.insert(row.id);
                        }
                    }
                    if (!dead_ids.empty())
                    {
                        std::println(
                            std::cerr,
                            "ccg: note: {} indexed file row{} vanished "
                            "while parsing (concurrent writer) - "
                            "re-registering before commit",
                            dead_ids.size(),
                            dead_ids.size() == 1 ? "" : "s");
                        for (auto& [path, row] : row_by_path)
                        {
                            if (!dead_ids.contains(row.id))
                            {
                                continue;
                            }
                            const cg::db::FileId fresh =
                                store_.upsert_file(path);
                            new_files.insert(fresh);
                            row.id = fresh;
                            row.content_hash.reset(); // 新行无哈希记录
                        }
                        for (auto& [key, row] : fold_row)
                        {
                            static_cast<void>(key);
                            if (!dead_ids.contains(row.id))
                            {
                                continue;
                            }
                            if (const auto it = row_by_path.find(row.path);
                                it != row_by_path.end())
                            {
                                row = it->second;
                            }
                        }
                        for (MainPlan& p : plans)
                        {
                            if (p.fid != 0 && dead_ids.contains(p.fid))
                            {
                                p.fid = file_id_of(p.cmd->file);
                            }
                        }
                        for (StaleTu& j : jobs)
                        {
                            if (j.fid != 0 && dead_ids.contains(j.fid))
                            {
                                j.fid = file_id_of(j.cmd->file);
                            }
                        }
                        for (RunTu& tu : batch)
                        {
                            if (tu.fid != 0 && dead_ids.contains(tu.fid))
                            {
                                tu.fid = file_id_of(tu.cmd->file);
                            }
                        }
                        // 波式分解：账本 TU 的 fid 同步复活（等价于批 TU）。
                        for (FlushLedgerTu& lt : fb_ledger_)
                        {
                            if (lt.fid != 0 && dead_ids.contains(lt.fid))
                            {
                                lt.fid = file_id_of(lt.cmd->file);
                            }
                        }
                        std::erase_if(
                            st.fp_writes,
                            [&](const cg::db::FingerprintRow& fp)
                            { return dead_ids.contains(fp.file_id); });
                        std::erase_if(
                            st.hash_writes,
                            [&](const auto& kv)
                            { return dead_ids.contains(kv.first); });
                    }
                }

                // 外部桥两表的站点 file_id：界内取登记 id，界外（界外头
                // 体内的 invoke/级联转发站点）写 0——不把界外路径登记进
                // files（files 语义=收录文件集；登记会连带 fingerprints
                // FK 违规，实测 18 行）。schema v16 两表 file_id 已无 FK。
                const auto bridge_site_fid =
                    [&](const std::string& path) -> cg::db::FileId
                {
                    return detail::path_in_roots(path, collect_opt_.roots)
                               ? file_id_of(path)
                               : cg::db::FileId{0};
                };

                // 阶段 A 批量去重：逐 TU 发射的同一个 unique_key 收敛为一次
                // upsert——插入序取首次出现（sym_id 与串行运行一致），
                // 列载荷取最后一次发射（与现有 upsert 的末次 refresh 等价）。
                // 头文件声明被每个 TU 重复发射，参考语料实测 1,257,879 次
                // 发射对应 71,430 个唯一键。
                struct SymSlot
                {
                    std::size_t      first_tu = 0;
                    std::size_t      first_sym = 0;
                    std::size_t      last_tu = 0;
                    std::size_t      last_sym = 0;
                    std::string_view parent_key; // 末次载荷的父记录键
                };
                std::vector<SymSlot>                              slots;
                std::unordered_map<std::string_view, std::size_t> slot_of;
                if (!fb_active_)
                {
                    for (std::size_t t = 0; t < batch.size(); ++t)
                    {
                        // 本 TU 的 record 别名 -> unique_key。pre-order 保证父
                        // 记录先于成员出现；映射是 TU 局部的，避免不同文件的
                        // 同名 scope 串父。
                        lease_beat();
                        std::unordered_map<std::string, std::string_view>
                                                                   record_key;
                        const std::vector<cg::extract::SymbolRec>& symbols =
                            batch[t].ex.symbols;
                        for (std::size_t i = 0; i < symbols.size(); ++i)
                        {
                            const cg::extract::SymbolRec& s = symbols[i];
                            std::string_view              parent_key;
                            if (!s.parent.empty())
                            {
                                const std::string alias = parent_alias_of(s);
                                if (!alias.empty())
                                {
                                    if (const auto it = record_key.find(alias);
                                        it != record_key.end())
                                    {
                                        parent_key = it->second;
                                    }
                                }
                            }
                            const auto [it, fresh] =
                                slot_of.try_emplace(s.unique_key, slots.size());
                            if (fresh)
                            {
                                slots.push_back(
                                    SymSlot{
                                        .first_tu = t,
                                        .first_sym = i,
                                        .last_tu = t,
                                        .last_sym = i,
                                        .parent_key = parent_key});
                            }
                            else
                            {
                                SymSlot& slot = slots[it->second];
                                slot.last_tu = t;
                                slot.last_sym = i;
                                slot.parent_key = parent_key;
                            }
                            if (s.kind == cg::extract::SymKind::Record
                                || s.kind == cg::extract::SymKind::Enumeration)
                            {
                                record_key.insert_or_assign(
                                    record_alias(s),
                                    s.unique_key);
                            }
                        }
                    }
                }

                std::unordered_map<std::string_view, cg::db::SymId> key_id;
                key_id.reserve(fb_active_ ? fb_syms_.size() : slots.size());
                std::unordered_map<cg::db::FileId, std::vector<cg::db::StrId>>
                    fresh_keys;
                // 本批符号写入触及的文件（含 PCH 装载域：影子 PCH 的伞头
                // 闭包不产生 include 边，符号是它仍属解析闭包的唯一在库
                // 证据）。可达性清扫把它并入种子——删除刚写入符号的文件
                // 行等于级联清掉本批成果，随后的贡献票发射即引用死符号
                // id 而外键失败。
                std::unordered_set<cg::db::FileId> symbol_sink_files;
                if (!fb_active_)
                {
                    for (const SymSlot& slot : slots)
                    {
                        lease_beat();
                        const cg::extract::SymbolRec& s =
                            batch[slot.last_tu].ex.symbols[slot.last_sym];
                        const cg::db::FileId sfile = file_id_of(s.file);
                        symbol_sink_files.insert(sfile);
                        const bool fn_like = function_like(s.kind);
                        // 同一符号的签名哈希在 upsert 与索引条目两处使用，只算
                        // 一次（util 单点实现，逐位不变）。
                        const std::optional<std::int64_t> sig =
                            fn_like ? std::optional<
                                          std::int64_t>{cg::util::sig_hash_of(
                                          s.param_types)}
                                    : std::nullopt;
                        const std::string param_json =
                            cg::extract::json_string_array(s.param_types);
                        std::optional<cg::db::SymId> parent_id;
                        if (!slot.parent_key.empty())
                        {
                            if (const auto it = key_id.find(slot.parent_key);
                                it != key_id.end())
                            {
                                parent_id = it->second;
                            }
                        }
                        const cg::db::SymId id = store_.upsert_symbol(
                            {.unique_key = s.unique_key,
                             .kind = detail::to_db_kind(s.kind),
                             .name = s.name,
                             .ns = s.ns,
                             .sig_hash = sig,
                             .file_id = sfile,
                             .line = s.line,
                             .col = s.col,
                             .parent_id = parent_id,
                             .flags = s.flags,
                             .param_types =
                                 fn_like ? std::optional<
                                               std::string_view>{param_json}
                                         : std::nullopt,
                             .template_params =
                                 s.template_params.empty()
                                     ? std::nullopt
                                     : std::optional<std::string_view>{
                                           cg::extract::json_string_array(
                                               s.template_params)}});
                        key_id.emplace(s.unique_key, id);
                        fresh_keys[sfile].push_back(
                            store_.intern(s.unique_key));

                        detail::SymbolIndex::Entry entry;
                        entry.id = id;
                        entry.kind = detail::to_db_kind(s.kind);
                        entry.name = s.name;
                        entry.ns = s.ns;
                        entry.parent = s.parent;
                        entry.sig_hash = sig;
                        entry.const_method =
                            (s.flags & cg::extract::sym_flags::is_const) != 0;
                        entry.flags = s.flags;
                        entry.file_id = sfile;
                        sym_index_.upsert_entry(s.unique_key, std::move(entry));
                    }
                }
                else
                {
                    // 波式分解：槽已在波末建好（首现序=SymId 分配序，末次载
                    // 荷已搬移进槽）。发射体与现状逐字段同构；key_id 的视图
                    // 指向槽内串（fb_syms_ 发射期冻结，视图稳定）。
                    for (FlushSymSlot& slot : fb_syms_)
                    {
                        lease_beat();
                        const cg::extract::SymbolRec& s = slot.rec;
                        const cg::db::FileId sfile = file_id_of(s.file);
                        symbol_sink_files.insert(sfile);
                        const bool fn_like = function_like(s.kind);
                        const std::optional<std::int64_t> sig =
                            fn_like ? std::optional<
                                          std::int64_t>{cg::util::sig_hash_of(
                                          s.param_types)}
                                    : std::nullopt;
                        const std::string param_json =
                            cg::extract::json_string_array(s.param_types);
                        std::optional<cg::db::SymId> parent_id;
                        if (!slot.parent_key.empty())
                        {
                            if (const auto it = key_id.find(
                                    std::string_view{slot.parent_key});
                                it != key_id.end())
                            {
                                parent_id = it->second;
                            }
                        }
                        const cg::db::SymId id = store_.upsert_symbol(
                            {.unique_key = s.unique_key,
                             .kind = detail::to_db_kind(s.kind),
                             .name = s.name,
                             .ns = s.ns,
                             .sig_hash = sig,
                             .file_id = sfile,
                             .line = s.line,
                             .col = s.col,
                             .parent_id = parent_id,
                             .flags = s.flags,
                             .param_types =
                                 fn_like ? std::optional<
                                               std::string_view>{param_json}
                                         : std::nullopt,
                             .template_params =
                                 s.template_params.empty()
                                     ? std::nullopt
                                     : std::optional<std::string_view>{
                                           cg::extract::json_string_array(
                                               s.template_params)}});
                        key_id.emplace(std::string_view{s.unique_key}, id);
                        fresh_keys[sfile].push_back(
                            store_.intern(s.unique_key));

                        detail::SymbolIndex::Entry entry;
                        entry.id = id;
                        entry.kind = detail::to_db_kind(s.kind);
                        entry.name = s.name;
                        entry.ns = s.ns;
                        entry.parent = s.parent;
                        entry.sig_hash = sig;
                        entry.const_method =
                            (s.flags & cg::extract::sym_flags::is_const) != 0;
                        entry.flags = s.flags;
                        entry.file_id = sfile;
                        sym_index_.upsert_entry(s.unique_key, std::move(entry));
                    }
                }
                const auto t_a1 = std::chrono::steady_clock::now();

                // 符号面投票：本批每个条目发射的定义级名字各投一票
                // （key_id 已在阶段 A 建满，发射面与 upsert 面一致）。
                for (std::size_t t = 0; t < entry_votes.size(); ++t)
                {
                    lease_beat();
                    std::unordered_set<cg::db::SymId>& v = votes_of_tu(t);
                    if (!fb_active_)
                    {
                        for (const cg::extract::SymbolRec& s :
                             batch[t].ex.symbols)
                        {
                            if (const auto it = key_id.find(s.unique_key);
                                it != key_id.end())
                            {
                                v.insert(it->second);
                            }
                        }
                    }
                    else
                    {
                        // 波式分解：本 TU 的发射键集在账本（intern 号）。
                        for (const std::uint32_t kid : fb_ledger_[t].keep_keys)
                        {
                            const std::string& key = *fb_key_list_[kid];
                            if (const auto it =
                                    key_id.find(std::string_view{key});
                                it != key_id.end())
                            {
                                v.insert(it->second);
                            }
                        }
                    }
                }

                for (const TuView& tu : tu_views)
                {
                    for (const cg::extract::IncludeRec& inc : *tu.includes)
                    {
                        fresh_keys[file_id_of(inc.file)];
                    }
                    for (const cg::extract::ImportRec& im : *tu.imports)
                    {
                        fresh_keys[file_id_of(im.file)];
                    }
                }
                for (const auto& [file, keys] : fresh_keys)
                {
                    lease_beat();
                    if (rewrite.contains(file) || new_files.contains(file))
                    {
                        capture_analysis_purge_(file, keys);
                        store_.purge_file(file, keys);
                    }
                }
                // 全部存活条目都失败的文件整体清除：该文件本次没有任何
                // 成功条目，全量重索引同树也不会为它产出符号。失败条目
                // 的站点/包含行在失败当时就已清除，这里的主要目标是
                // "成功条目已被移出编译库、只剩失败条目"的残留符号。
                // 注意 all_failed_fids 已排除任何拥有存活 Ok 条目的文件
                // ——实测大库中那 3 个"一条成功一条失败"的文件不在此列。
                //
                // 防线（外键提交失败事故）：all_failed_fids 是计划
                // 段按当时已知的成败统计算出的集合，而本 run 的重试翻绿
                // （良性白名单等）发生在解析之后——统计口径一旦没跟上，
                // 这里会把手握本 run 成功发射载荷（符号/线索/站点）的文件
                // 整体清除，随后的线索发射引用被删 owner 即以外键失败拖垮
                // 整个提交事务。以提交段的事内事实做最终裁决：本 run 产出
                // 了载荷的文件绝不走全败清除。载荷身份按发射事实取两路并
                // 集——成功 TU 的主文件（波式=账本 fid、关波=成功 job 的
                // fid），以及 fresh_keys 里键向量非空的文件（头文件定义经
                // 包含者发射：该文件自身条目可能全败，但其符号行是本 run
                // 刚写入的活载荷，清它同样等于删本 run 的产出）。
                std::unordered_set<cg::db::FileId> emitted_fids;
                if (fb_active_)
                {
                    emitted_fids.reserve(fb_ledger_.size());
                    for (const FlushLedgerTu& lt : fb_ledger_)
                    {
                        if (lt.fid != 0)
                        {
                            emitted_fids.insert(lt.fid);
                        }
                    }
                }
                else
                {
                    emitted_fids.reserve(jobs.size());
                    for (std::size_t i = 0; i < jobs.size(); ++i)
                    {
                        if (results[i].failure.empty() && jobs[i].fid != 0)
                        {
                            emitted_fids.insert(jobs[i].fid);
                        }
                    }
                }
                for (const auto& [file, keys] : fresh_keys)
                {
                    // 只认非空键向量：包含/导入差循环也会经 operator[]
                    // 建出空条目，空向量不代表任何发射载荷。
                    if (!keys.empty())
                    {
                        emitted_fids.insert(file);
                    }
                }
                std::size_t kept_emitted_files = 0;
                for (const cg::db::FileId fid : all_failed_fids)
                {
                    if (emitted_fids.contains(fid))
                    {
                        // 本 run 有成功载荷：绝不全败清除。计数并注记——
                        // 该拦截本就是计划段统计口径缺口的兜底，可观测的
                        // 命中数是缺口再现时的第一手线索。
                        ++kept_emitted_files;
                        continue;
                    }
                    // 全败条目终态死亡：整文件撤票（该文件的条目此后不再
                    // 存在），受影响符号进归零仲裁候选。
                    for (const cg::db::SymId sid :
                         store_.retract_file_contributions(fid))
                    {
                        retracted_votes.insert(sid);
                    }
                    capture_analysis_purge_(
                        fid,
                        std::span<const cg::db::StrId>{});
                    store_.purge_file(fid, {});
                }
                if (kept_emitted_files > 0)
                {
                    std::println(
                        std::cerr,
                        "ccg: note: {} all-failed file row{} carried "
                        "this-run payload - purge skipped",
                        kept_emitted_files,
                        kept_emitted_files == 1 ? "" : "s");
                }

                // 死实参实例行显式删除（dispatch 装载之前）：决议表按存
                // 量行构建，幽灵行在场会污染候选计数（实测多发候选边）；
                // 此处删行让全站点/增量决议都与干净库一致。引用方随外键
                // 级联清理（边/线索/站点/子成员 CASCADE，callee/method/
                // static_type 置空）。
                if (!st.dead_symbol_rows.empty())
                {
                    store_.erase_symbols(st.dead_symbol_rows);
                }

                // schema v10 阶段 A 增量：本批 TU 的类型线索与调用点。符号已
                // upsert、逐文件 purge 已跑完，故写入即为该文件的当前事实。
                // key_id 键型即 string_view：零拷贝直查（昔日 std::string{key}
                // 的临时键构造是笔误级写法）。
                const auto analysis_owner_id =
                    [&](std::string_view key) -> std::optional<cg::db::SymId>
                {
                    if (const auto it = key_id.find(key); it != key_id.end())
                    {
                        // 活性预检（外键提交失败事故的防线补
                        // 全）：key_id 在符号发射段填充，其行随后可能被逐
                        // 文件 purge 的 doomed 删除（源不再发射该符号、全
                        // 败清除误伤等）；owner 行缺席即丢弃引用它的派生
                        // 行（线索/站点/契约边），与边发射的
                        // known_symbol_ids 预检同一防线语义（实测事故先例）。
                        return store_.symbol_exists(it->second)
                                   ? std::optional<cg::db::SymId>{it->second}
                                   : std::nullopt;
                    }
                    // 库查分支天然只返回当下存活的行。
                    return cg::db::symbol_id_by_key(store_, key);
                };
                for (std::size_t tv_i = 0; tv_i < tu_views.size(); ++tv_i)
                {
                    const TuView& tu = tu_views[tv_i];
                    lease_beat();
                    if (!fb_active_)
                    {
                        // lambda 符号（function 形态，间接调用的边目标承接）
                        // ——先于本 TU 的站点/边落库（边按 key 查符号）。
                        // 界外体内的 lambda（界外模板/实例桥引入的采集，如
                        // stdexec operation state 的就地 lambda）：真实坐标在
                        // name（operator()@L行:文件）里保全；symbols.file_id 是
                        // NOT NULL+FK，界外路径登记 files 会连带 fingerprints
                        // FK 违规（实测同族）——挂靠本 TU 主文件作锚。
                        for (const cg::analysis::LambdaSymRec& l :
                             tu.analysis->lambda_symbols)
                        {
                            const bool lambda_in_roots = detail::path_in_roots(
                                l.file,
                                collect_opt_.roots);
                            const cg::db::SymId lam_id = store_.upsert_symbol(
                                {.unique_key = l.key,
                                 .kind = cg::db::SymbolKind::Function,
                                 .name = l.name,
                                 .ns = std::string_view{},
                                 .sig_hash = std::nullopt,
                                 .file_id =
                                     lambda_in_roots
                                         ? file_id_of(l.file)
                                         : (tu.fid != 0
                                                ? tu.fid
                                                : file_id_of(tu.cmd->file)),
                                 .line = l.line,
                                 .col = l.col,
                                 .parent_id = std::nullopt,
                                 .flags = cg::db::sym_flags::external_body,
                                 .param_types = std::nullopt,
                                 .template_params = std::nullopt});
                            votes_of_tu(tv_i).insert(lam_id);
                        }
                        for (const cg::analysis::TypeHintRec& hint :
                             tu.analysis->hints)
                        {
                            const auto owner =
                                analysis_owner_id(hint.owner_key);
                            if (!owner.has_value())
                            {
                                continue;
                            }
                            const bool declared =
                                hint.source
                                == cg::analysis::HintSource::Declared;
                            std::vector<std::string> types =
                                declared ? hint.static_types : hint.type_keys;
                            std::ranges::sort(types);
                            types.erase(
                                std::unique(types.begin(), types.end()),
                                types.end());
                            const std::string json =
                                cg::analysis::json_string_array(types);
                            store_.upsert_type_hint(
                                {.owner_id = *owner,
                                 .var_name = hint.var_name,
                                 .file_id = file_id_of(hint.file),
                                 .line = hint.line,
                                 .col = hint.col,
                                 .type_set = json,
                                 .source = static_cast<int>(hint.source),
                                 .confidence =
                                     static_cast<int>(hint.confidence),
                                 .fallback_reason = hint.fallback_reason,
                                 .param_index = hint.param_index,
                                 .related_var = hint.related_var,
                                 .related_key = hint.related_key});
                        }
                        for (const cg::analysis::CallSiteRec& site :
                             tu.analysis->call_sites)
                        {
                            const auto caller =
                                analysis_owner_id(site.caller_key);
                            if (!caller.has_value())
                            {
                                continue;
                            }
                            std::vector<std::string> static_types =
                                site.static_receiver_types;
                            std::ranges::sort(static_types);
                            static_types.erase(
                                std::unique(
                                    static_types.begin(),
                                    static_types.end()),
                                static_types.end());
                            const std::string static_json =
                                cg::analysis::json_string_array(static_types);
                            const std::string args_json =
                                cg::analysis::json_string_array_2d(
                                    site.arg_types);
                            const std::string arg_vars_json =
                                cg::analysis::json_string_array(site.arg_vars);
                            const std::string arg_keys_json =
                                cg::analysis::json_string_array(
                                    site.arg_call_keys);
                            const std::string targets_json =
                                cg::analysis::json_string_array(
                                    site.arg_targets);
                            store_.upsert_call_site(
                                {.caller_id = *caller,
                                 .callee_id = std::nullopt,
                                 .callee_key = std::string_view{},
                                 .callee_name = site.callee.name,
                                 .callee_ns = site.callee.ns,
                                 .callee_parent = site.callee.parent,
                                 .callee_sig = site.callee.sig_hash,
                                 .callee_const = site.callee.const_method,
                                 .method_id = std::nullopt,
                                 .method_key = std::string_view{},
                                 .is_virtual = site.is_virtual,
                                 .qualified = site.qualified,
                                 .indirect = site.indirect,
                                 .receiver_name = site.receiver_name,
                                 .receiver_is_this = site.receiver_is_this,
                                 .receiver_owner_key = site.receiver_owner_key,
                                 .file_id = file_id_of(site.file),
                                 .line = site.line,
                                 .col = site.col,
                                 .arg_types = args_json,
                                 .arg_vars = arg_vars_json,
                                 .arg_call_keys = arg_keys_json,
                                 .arg_targets = targets_json,
                                 .receiver_types = static_json,
                                 .receiver_static = static_json,
                                 .callee_resolved = false,
                                 .resolved = false,
                                 .confidence = 3,
                                 .candidates = std::string_view{},
                                 .fallback_reason = std::string_view{},
                                 .callee_usr = site.callee_usr,
                                 .callee_expr = site.callee_expr,
                                 .callee_param = site.callee_param});
                        }
                    } // !fb_active_：三段重载荷已去重进全局槽，独立发射
                    // 平台回调契约线索边（type=8）：src=注册处函数，
                    // dst=回调函数。upsert 幂等；site 落注册调用点。残留面
                    // （注册消失后的旧行）随槽消费统一治理，诚实清单
                    // 在案。
                    for (const cg::analysis::CallbackRegRec& reg :
                         tu.analysis->callback_regs)
                    {
                        const auto src = analysis_owner_id(reg.caller_key);
                        const auto dst = analysis_owner_id(reg.target_key);
                        if (!src.has_value() || !dst.has_value())
                        {
                            continue;
                        }
                        store_.upsert_edge(
                            {.src = *src,
                             .dst = *dst,
                             .type = cg::db::EdgeType::CallbackReg,
                             .site_file = file_id_of(reg.site_file),
                             .site_line = reg.site_line,
                             .attrs = 0,
                             .confidence = 0.7,
                             .occurrences = 1});
                    }
                    // 槽位基建：透传包装登记（纯登记零消费；per-file 重写
                    // 语义由 file_id 外键的 purge 级联承载，upsert
                    // 即当前事实）。
                    for (const cg::analysis::PassthroughWrapperRec& w :
                         tu.analysis->passthrough_wrappers)
                    {
                        store_.upsert_passthrough_wrapper(
                            {.wrapper_key = w.wrapper_key,
                             .getter_key = w.getter_key,
                             .getter_name = w.getter_name,
                             .file_id = file_id_of(w.file),
                             .line = w.line,
                             .arg_index = w.arg_index});
                    }
                    // 实例桥两侧行（消费 join 在本批 TU 落库完成后统一跑）。
                    for (const cg::analysis::ExternalParamArgRec& a :
                         tu.analysis->external_param_args)
                    {
                        store_.upsert_external_param_arg(
                            {.host_usr = a.host_usr,
                             .param_index = a.param_index,
                             .caller_key = a.caller_key,
                             .target_key = a.target_key,
                             .file_id = bridge_site_fid(a.file),
                             .line = a.line,
                             .col = a.col});
                    }
                    for (const cg::analysis::ExternalInvokeRec& v :
                         tu.analysis->external_invokes)
                    {
                        store_.upsert_external_invoke(
                            {.host_usr = v.host_usr,
                             .param_index = v.param_index,
                             .file_id = bridge_site_fid(v.file),
                             .line = v.line,
                             .col = v.col});
                    }
                }
                if (fb_active_)
                {
                    // 波式分解的三段重载荷发射（全局槽、首现序=首插序；三表
                    // 各自独立，跨表交错不影响 rowid）。lambda 末写胜出、锚定
                    // 取末次出现 TU 的账本条目（fid 修补联动）；线索/站点首
                    // 写胜出。
                    for (const FlushLamSlot& ls : fb_lams_)
                    {
                        lease_beat();
                        const cg::analysis::LambdaSymRec& l = ls.rec;
                        const FlushLedgerTu& anchor = fb_ledger_[ls.tu];
                        const bool           lambda_in_roots =
                            detail::path_in_roots(l.file, collect_opt_.roots);
                        const cg::db::SymId lam_id = store_.upsert_symbol(
                            {.unique_key = l.key,
                             .kind = cg::db::SymbolKind::Function,
                             .name = l.name,
                             .ns = std::string_view{},
                             .sig_hash = std::nullopt,
                             .file_id =
                                 lambda_in_roots
                                     ? file_id_of(l.file)
                                     : (anchor.fid != 0
                                            ? anchor.fid
                                            : file_id_of(anchor.cmd->file)),
                             .line = l.line,
                             .col = l.col,
                             .parent_id = std::nullopt,
                             .flags = cg::db::sym_flags::external_body,
                             .param_types = std::nullopt,
                             .template_params = std::nullopt});
                        votes_of_tu(ls.tu).insert(lam_id);
                    }
                    for (const cg::analysis::TypeHintRec& hint : fb_hints_)
                    {
                        lease_beat();
                        const auto owner = analysis_owner_id(hint.owner_key);
                        if (!owner.has_value())
                        {
                            continue;
                        }
                        const bool declared =
                            hint.source == cg::analysis::HintSource::Declared;
                        std::vector<std::string> types =
                            declared ? hint.static_types : hint.type_keys;
                        std::ranges::sort(types);
                        types.erase(
                            std::unique(types.begin(), types.end()),
                            types.end());
                        const std::string json =
                            cg::analysis::json_string_array(types);
                        store_.upsert_type_hint(
                            {.owner_id = *owner,
                             .var_name = hint.var_name,
                             .file_id = file_id_of(hint.file),
                             .line = hint.line,
                             .col = hint.col,
                             .type_set = json,
                             .source = static_cast<int>(hint.source),
                             .confidence = static_cast<int>(hint.confidence),
                             .fallback_reason = hint.fallback_reason,
                             .param_index = hint.param_index,
                             .related_var = hint.related_var,
                             .related_key = hint.related_key});
                    }
                    for (const cg::analysis::CallSiteRec& site : fb_sites_)
                    {
                        lease_beat();
                        const auto caller = analysis_owner_id(site.caller_key);
                        if (!caller.has_value())
                        {
                            continue;
                        }
                        std::vector<std::string> static_types =
                            site.static_receiver_types;
                        std::ranges::sort(static_types);
                        static_types.erase(
                            std::unique(
                                static_types.begin(),
                                static_types.end()),
                            static_types.end());
                        const std::string static_json =
                            cg::analysis::json_string_array(static_types);
                        const std::string args_json =
                            cg::analysis::json_string_array_2d(site.arg_types);
                        const std::string arg_vars_json =
                            cg::analysis::json_string_array(site.arg_vars);
                        const std::string arg_keys_json =
                            cg::analysis::json_string_array(site.arg_call_keys);
                        const std::string targets_json =
                            cg::analysis::json_string_array(site.arg_targets);
                        store_.upsert_call_site(
                            {.caller_id = *caller,
                             .callee_id = std::nullopt,
                             .callee_key = std::string_view{},
                             .callee_name = site.callee.name,
                             .callee_ns = site.callee.ns,
                             .callee_parent = site.callee.parent,
                             .callee_sig = site.callee.sig_hash,
                             .callee_const = site.callee.const_method,
                             .method_id = std::nullopt,
                             .method_key = std::string_view{},
                             .is_virtual = site.is_virtual,
                             .qualified = site.qualified,
                             .indirect = site.indirect,
                             .receiver_name = site.receiver_name,
                             .receiver_is_this = site.receiver_is_this,
                             .receiver_owner_key = site.receiver_owner_key,
                             .file_id = file_id_of(site.file),
                             .line = site.line,
                             .col = site.col,
                             .arg_types = args_json,
                             .arg_vars = arg_vars_json,
                             .arg_call_keys = arg_keys_json,
                             .arg_targets = targets_json,
                             .receiver_types = static_json,
                             .receiver_static = static_json,
                             .callee_resolved = false,
                             .resolved = false,
                             .confidence = 3,
                             .candidates = std::string_view{},
                             .fallback_reason = std::string_view{},
                             .callee_usr = site.callee_usr,
                             .callee_expr = site.callee_expr,
                             .callee_param = site.callee_param});
                    }
                }
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    if (results[i].failure.empty())
                    {
                        for (const std::string& name :
                             jobs[i].cmd->overlay_imports)
                        {
                            const cg::db::FileId fid =
                                jobs[i].fid != 0
                                    ? jobs[i].fid
                                    : file_id_of(jobs[i].cmd->file);
                            store_.upsert_import(
                                {.file = fid, .module_name = name, .line = 0});
                        }
                        continue;
                    }
                    if (jobs[i].cmd->scan_imports.empty())
                    {
                        continue;
                    }
                    const cg::db::FileId fid =
                        jobs[i].fid != 0 ? jobs[i].fid
                                         : file_id_of(jobs[i].cmd->file);
                    for (const std::string& name : jobs[i].cmd->scan_imports)
                    {
                        store_.upsert_import(
                            {.file = fid, .module_name = name, .line = 0});
                    }
                }
                // 失败条目生命周期：成功 job 删除自己的失败记录；失败 job
                // 写下"本次失败时的世界状态"（内容哈希、引擎、feed、根集、
                // 依赖闭包、瞬时退避）。failure_skip 条目不动记录。
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    lease_beat();
                    const cg::db::FileId fid =
                        jobs[i].fid != 0 ? jobs[i].fid
                                         : file_id_of(jobs[i].cmd->file);
                    if (results[i].failure.empty())
                    {
                        store_.erase_failure(fid, jobs[i].digest);
                        continue;
                    }
                    cg::db::FailureRow failure;
                    failure.file_id = fid;
                    failure.cmd_digest = jobs[i].digest;
                    failure.content_hash = jobs[i].hash;
                    failure.mtime_ns = jobs[i].mtime_ns;
                    failure.size = static_cast<std::int64_t>(jobs[i].size);
                    // 池运行后引擎必已加载：这里取真实记号（首次运行时
                    // run() 开头的 engine_token 可能还是空串）。
                    failure.engine_token = detail::engine_token_now();
                    failure.feed_digest = detail::feed_digest_of(*jobs[i].cmd);
                    failure.roots_digest = roots_digest;
                    failure.failure_class = std::string{
                        detail::failure_class_of(results[i].failure)};
                    failure.failure_text = results[i].failure;
                    std::string deps;
                    if (fid != 0)
                    {
                        // 失败 TU 的 includes 行（旧成功运行时记录的）即将
                        // 被清除；把其传递闭包连同 stat 身份冻结在失败
                        // 记录里，下一次运行据此判断依赖是否变化。
                        std::unordered_set<cg::db::FileId> seen;
                        std::vector<cg::db::FileId>        queue{fid};
                        // 模块 TU 的 BMI 级补充观察集：命令侧注记的模块依赖源
                        // （BMI
                        // 生产失败语境）与解析诊断定位的文件（含头文件）。 BMI
                        // 生产失败的 TU 从未成功解析、库内没有它的 includes
                        // 行，上面的闭包走查对其一无所获——世界快照只能由这两路
                        // 补给。已知文件身份的条目同时入队，让走查顺带展开它们
                        // 自己的包含链（其他 TU 的 includes 行里已有该图）。
                        std::vector<std::string> extra_deps;
                        if (cg::clang::is_module_tu(*jobs[i].cmd))
                        {
                            extra_deps = jobs[i].cmd->bmi_failure_deps;
                            for (const std::string& blamed :
                                 results[i].failure_files)
                            {
                                extra_deps.push_back(blamed);
                            }
                            std::ranges::sort(extra_deps);
                            auto [keep, last] = std::ranges::unique(extra_deps);
                            extra_deps.erase(keep, last);
                        }
                        std::unordered_set<std::string_view> encoded;
                        for (const std::string& extra : extra_deps)
                        {
                            if (const auto row = row_by_path.find(extra);
                                row != row_by_path.end()
                                && seen.insert(row->second.id).second)
                            {
                                queue.push_back(row->second.id);
                            }
                        }
                        std::size_t emitted = 0;
                        while (!queue.empty() && emitted < 4096)
                        {
                            const cg::db::FileId f = queue.back();
                            queue.pop_back();
                            const auto it = fwd.find(f);
                            if (it == fwd.end())
                            {
                                continue;
                            }
                            for (const cg::db::FileId target : it->second)
                            {
                                if (!seen.insert(target).second)
                                {
                                    continue;
                                }
                                queue.push_back(target);
                                if (const auto pit = path_of.find(target);
                                    pit != path_of.end())
                                {
                                    detail::encode_dep(
                                        deps,
                                        pit->second,
                                        detail::stat_file(pit->second));
                                    encoded.insert(
                                        std::string_view{pit->second});
                                    ++emitted;
                                }
                            }
                        }
                        for (const std::string& extra : extra_deps)
                        {
                            if (encoded.insert(std::string_view{extra}).second)
                            {
                                detail::encode_dep(
                                    deps,
                                    extra,
                                    detail::stat_file(extra));
                            }
                        }
                    }
                    const cg::db::FailureRow* prior_failure = nullptr;
                    if (const auto fit = fail_by_file.find(fid);
                        fit != fail_by_file.end())
                    {
                        for (const cg::db::FailureRow& prior : fit->second)
                        {
                            if (prior.cmd_digest == jobs[i].digest)
                            {
                                prior_failure = &prior;
                                break;
                            }
                        }
                    }
                    // 合并而非覆盖：本次依赖闭包通常为空（失败 TU 的
                    // includes 已在上一次失败时清除），空快照会抹掉重试
                    // 所需的依赖身份，让后续修复的头文件再也触发不了重试。
                    failure.deps_json =
                        prior_failure != nullptr
                            ? detail::merge_deps(prior_failure->deps_json, deps)
                            : std::move(deps);
                    std::int64_t attempts = 1;
                    std::string  first_failed_at = now;
                    if (prior_failure != nullptr)
                    {
                        attempts = prior_failure->attempts + 1;
                        if (!prior_failure->first_failed_at.empty())
                        {
                            first_failed_at = prior_failure->first_failed_at;
                        }
                    }
                    failure.attempts = attempts;
                    failure.first_failed_at = std::move(first_failed_at);
                    failure.last_failed_at = now;
                    failure.retry_after =
                        detail::is_transient_failure_class(
                            failure.failure_class)
                            ? cg::util::cstd::epoch_ms()
                                  + detail::retry_delay_ms(attempts)
                            : 0;
                    store_.set_failure(failure);
                }
                std::unordered_set<cg::db::FileId> missing_fids;
                for (std::size_t i = 0; i < plans.size(); ++i)
                {
                    const MainPlan& p = plans[i];
                    if (dedup.at(std::pair{p.cmd->file, p.digest}) != i)
                    {
                        continue; // 重复条目只处理首个（同族守卫）
                    }
                    if (p.missing)
                    {
                        const cg::db::FileId fid =
                            p.fid != 0 ? p.fid : file_id_of(p.cmd->file);
                        missing_fids.insert(fid);
                        // 盘缺条目与全败同族：整文件撤票+归零仲裁候选。
                        for (const cg::db::SymId sid :
                             store_.retract_file_contributions(fid))
                        {
                            retracted_votes.insert(sid);
                        }
                        capture_analysis_purge_(
                            fid,
                            std::span<const cg::db::StrId>{});
                        store_.purge_file(fid, {});
                        store_.set_file_status(
                            fid,
                            cg::util::ParseStatus::Failed,
                            1,
                            "file not found on disk",
                            std::nullopt);
                        store_.clear_file_hash(fid);
                        // 盘缺主文件绝不携带失败记录：文件重新出现时
                        // 必须重试（失败记录会把同一字节判成"世界未变"，
                        // 而其符号已在缺失时被清除）。
                        store_.erase_failure(fid, p.digest);
                    }
                }
                const auto t_purge1 = std::chrono::steady_clock::now();

                // 观察者增量合并的站点放行：重写集/新文件之外，计数增量
                // 合并选中的目标文件也放行（行不清理，upsert 的冲突 bump
                // 语义承接新增观察者的 occurrences；rewrite 优先——目标若
                // 后续进了重写集，按重写语义整体重建，bump 集自然旁路）。
                const auto site_writable = [&](cg::db::FileId f)
                {
                    return rewrite.contains(f) || new_files.contains(f)
                           || bump_feed_.contains(f);
                };
                // 阶段 B 批量去重：同一 (src,dst,type,site_file,line) 站点
                // 被多个 TU 重复观察时只写一行——occurrences 求和，attrs/
                // confidence 首写胜出（与 upsert_edge 的"首插带列、冲突只
                // bump"逐行等价）。解析仍逐发射执行，sum.unresolved /
                // unknown_names_ 的计数语义不变。参考语料实测 1,869,137 次
                // 边发射收敛为 260,734 个唯一站点。
                struct EdgeKey
                {
                    cg::db::SymId    src = 0;
                    cg::db::SymId    dst = 0;
                    cg::db::EdgeType type = cg::db::EdgeType::Calls;
                    cg::db::FileId   site_file = 0;
                    int              site_line = 0;
                    bool             operator==(const EdgeKey&) const = default;
                };
                struct EdgeKeyHash
                {
                    auto operator()(const EdgeKey& k) const noexcept
                        -> std::size_t
                    {
                        std::size_t h = std::hash<std::int64_t>{}(k.src);
                        const auto  mix = [&h](std::size_t v)
                        {
                            h ^=
                                v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                        };
                        mix(std::hash<std::int64_t>{}(k.dst));
                        mix(std::hash<int>{}(static_cast<int>(k.type)));
                        mix(std::hash<std::int64_t>{}(k.site_file));
                        mix(std::hash<int>{}(k.site_line));
                        return h;
                    }
                };
                struct EdgeSlot
                {
                    cg::db::SymId    src = 0;
                    cg::db::SymId    dst = 0;
                    cg::db::EdgeType type = cg::db::EdgeType::Calls;
                    cg::db::FileId   site_file = 0;
                    int              site_line = 0;
                    std::uint32_t    attrs = 0;
                    double           confidence = 1.0;
                    std::int64_t     occurrences = 0;
                };
                std::vector<EdgeSlot> edge_slots;
                std::unordered_map<EdgeKey, std::size_t, EdgeKeyHash>
                    edge_slot_of;
                // 解析后站点键的喂入器（波式分支用；现状循环保持原样不动）：
                // 首喂建槽（attrs/conf 首写胜出），后续只累 occurrences——与
                // 现状 EdgeSlot 语义逐字段同构。
                const auto feed_edge = [&](cg::db::SymId    s,
                                           cg::db::SymId    d,
                                           cg::db::EdgeType ty,
                                           cg::db::FileId   sf,
                                           int              ln,
                                           std::uint32_t    attrs,
                                           double           conf,
                                           std::int64_t     occ)
                {
                    const EdgeKey slot_key{
                        .src = s,
                        .dst = d,
                        .type = ty,
                        .site_file = sf,
                        .site_line = ln};
                    const auto [it, fresh] =
                        edge_slot_of.try_emplace(slot_key, edge_slots.size());
                    if (fresh)
                    {
                        edge_slots.push_back(
                            EdgeSlot{
                                .src = s,
                                .dst = d,
                                .type = ty,
                                .site_file = sf,
                                .site_line = ln,
                                .attrs = attrs,
                                .confidence = conf,
                                .occurrences = occ});
                    }
                    else
                    {
                        edge_slots[it->second].occurrences += occ;
                    }
                };
                if (fb_active_)
                {
                    // 波式分解：按「账本 TU × 槽首现序」回放发射——与现状的
                    // 全局发射序严格同构。建槽即 rowid：受限裸名的首次解析
                    // 成功可能晚于其首次发射，按槽分组喂入会漂移建槽次序
                    // （mid 语料实测：行集相等、行序漂移 22951 行）；每
                    // （槽，TU）解析一次、以该 TU 的折叠 occurrences 一次喂
                    // 入——建槽点=该组首发射，后续喂入只累加，终态与逐发射
                    // 逐字节等价。
                    std::vector<
                        std::optional<std::unordered_set<cg::db::FileId>>>
                               scopes(fb_ledger_.size());
                    const auto scope_of = [&](std::uint32_t t)
                        -> const std::unordered_set<cg::db::FileId>&
                    {
                        // tu_scope 等价重建：本 TU 定义文件全集（波末从其全
                        // 部符号一次性保真采集，非槽位归属）。
                        std::optional<std::unordered_set<cg::db::FileId>>&
                            slot = scopes[t];
                        if (!slot.has_value())
                        {
                            slot.emplace();
                            for (const std::uint32_t pid :
                                 fb_ledger_[t].def_paths)
                            {
                                slot->insert(file_id_of(*fb_path_list_[pid]));
                            }
                        }
                        return *slot;
                    };
                    // 每槽贡献游标：回放序（账本 TU 升序）与贡献序一致，
                    // 逐槽单调推进即命中当前 TU 的贡献。
                    std::vector<std::uint32_t> feed_ci(fb_edges_.size(), 0);
                    for (std::size_t t = 0; t < fb_ledger_.size(); ++t)
                    {
                        lease_beat();
                        for (const std::uint32_t si : fb_ledger_[t].edge_seq)
                        {
                            const FlushEdgeSlot&    es = fb_edges_[si];
                            const FlushEdgeContrib& c =
                                es.contribs[feed_ci[si]];
                            ++feed_ci[si];
                            const cg::db::FileId site =
                                file_id_of(es.site_file);
                            // 站点在未重写文件的边：边行仍在库内
                            // （occurrences 未动），不重发（防翻倍），但
                            // 名字证据真实——目标解析照跑、票照续（差集
                            // 撤票的新票集并入存活边目标面；非可写站点
                            // 的跳过曾把这类活票当批撤走）。
                            const bool writable = site_writable(site);
                            std::unordered_map<
                                std::string_view,
                                cg::db::SymId>::const_iterator src{};
                            if (writable)
                            {
                                src = key_id.find(std::string_view{es.src_key});
                                if (src == key_id.end())
                                {
                                    sum.unresolved += c.emissions;
                                    continue;
                                }
                            }
                            std::optional<cg::db::SymId> dst;
                            double                       confidence = 1.0;
                            if (es.dst_is_key)
                            {
                                dst = sym_index_.by_key(es.dst_key);
                            }
                            else if (
                                sym_index_.name_spans_files(es.dst_meta.name))
                            {
                                dst = sym_index_.resolve(
                                    es.dst_meta,
                                    es.kind,
                                    confidence,
                                    &scope_of(static_cast<std::uint32_t>(t)));
                            }
                            else
                            {
                                dst = sym_index_.resolve(
                                    es.dst_meta,
                                    es.kind,
                                    confidence,
                                    nullptr);
                            }
                            if (!dst.has_value())
                            {
                                if (writable)
                                {
                                    sum.unresolved += c.emissions;
                                    if (opt_.progress)
                                    {
                                        const std::string callee =
                                            es.dst_is_key ? es.dst_key
                                                          : es.dst_meta.name;
                                        if (es.dst_is_key
                                            || sym_index_.knows_name(
                                                es.dst_meta.name))
                                        {
                                            unresolved_names_[callee] +=
                                                c.emissions;
                                        }
                                        else
                                        {
                                            unknown_names_[callee] +=
                                                c.emissions;
                                        }
                                    }
                                }
                                continue;
                            }
                            // 名字面投票：边解析命中的目标符号对本条目
                            // 投一票（浅引用——前置声明+指针/引用使用
                            // ——经 uses 边的延迟解析命中即活票）。非可
                            // 写站点不 feed 边（边行已在库内），解析命
                            // 中仍续票。
                            votes_of_tu(t).insert(*dst);
                            if (!writable)
                            {
                                continue;
                            }
                            feed_edge(
                                src->second,
                                *dst,
                                detail::to_db_edge(es.kind),
                                site,
                                es.site_line,
                                es.attrs,
                                confidence,
                                c.occurrences);
                        }
                    }
                }
                else
                {
                    for (std::size_t t = 0; t < batch.size(); ++t)
                    {
                        lease_beat();
                        // internal-linkage 串线修复：本 TU
                        // 的定义文件全集（主文件
                        // + 本 TU 符号所处的全部头文件）。孪生裸名（跨文件同名
                        // 同身份定义，即 static/匿名 ns 的逐 TU 拷贝）的解析被
                        // 限定在该域内——懒建，只在首个受限裸名出现时付一遍
                        // O(本 TU 符号数) 的构造。
                        std::optional<std::unordered_set<cg::db::FileId>>
                            tu_scope;
                        for (const cg::extract::EdgeRec& e : batch[t].ex.edges)
                        {
                            const cg::db::FileId site = file_id_of(e.site_file);
                            // fb 分支同款：站点在未重写文件的边不重发（边
                            // 行仍在库内），但目标解析照跑、票照续——跳过
                            // 解析曾让差集撤票把头文件站点的浅引用活票当
                            // 批撤走（引用者条目的任何无关重解析都会触发）。
                            const bool writable = site_writable(site);
                            std::unordered_map<
                                std::string_view,
                                cg::db::SymId>::const_iterator src{};
                            if (writable)
                            {
                                src = key_id.find(e.src_key);
                                if (src == key_id.end())
                                {
                                    ++sum.unresolved;
                                    continue;
                                }
                            }
                            std::optional<cg::db::SymId> dst;
                            double                       confidence = 1.0;
                            if (e.dst.is_key)
                            {
                                dst = sym_index_.by_key(e.dst.key);
                            }
                            else if (
                                sym_index_.name_spans_files(e.dst.meta.name))
                            {
                                if (!tu_scope.has_value())
                                {
                                    tu_scope.emplace();
                                    for (const cg::extract::SymbolRec& s :
                                         batch[t].ex.symbols)
                                    {
                                        tu_scope->insert(file_id_of(s.file));
                                    }
                                }
                                dst = sym_index_.resolve(
                                    e.dst.meta,
                                    e.kind,
                                    confidence,
                                    &*tu_scope);
                            }
                            else
                            {
                                dst = sym_index_.resolve(
                                    e.dst.meta,
                                    e.kind,
                                    confidence,
                                    nullptr);
                            }
                            if (!dst.has_value())
                            {
                                if (writable)
                                {
                                    ++sum.unresolved;
                                    if (opt_.progress)
                                    {
                                        const std::string callee =
                                            e.dst.is_key ? e.dst.key
                                                         : e.dst.meta.name;
                                        if (e.dst.is_key
                                            || sym_index_.knows_name(
                                                e.dst.meta.name))
                                        {
                                            ++unresolved_names_[callee];
                                        }
                                        else
                                        {
                                            ++unknown_names_[callee];
                                        }
                                    }
                                }
                                continue;
                            }
                            // 名字面投票（fb 分支同款语义：解析命中即活票，
                            // 非可写站点只投票不 feed 边）。
                            votes_of_tu(t).insert(*dst);
                            if (!writable)
                            {
                                continue;
                            }
                            const EdgeKey slot_key{
                                .src = src->second,
                                .dst = *dst,
                                .type = detail::to_db_edge(e.kind),
                                .site_file = site,
                                .site_line = e.site_line};
                            const auto [it, fresh] = edge_slot_of.try_emplace(
                                slot_key,
                                edge_slots.size());
                            if (fresh)
                            {
                                edge_slots.push_back(
                                    EdgeSlot{
                                        .src = src->second,
                                        .dst = *dst,
                                        .type = detail::to_db_edge(e.kind),
                                        .site_file = site,
                                        .site_line = e.site_line,
                                        .attrs = e.attrs,
                                        .confidence = confidence,
                                        .occurrences = e.occurrences});
                            }
                            else
                            {
                                edge_slots[it->second].occurrences +=
                                    e.occurrences;
                            }
                        }
                    }
                }
                // FK 兜底：边的 src/dst 必须指向当下存活的符号行。成员资格
                // （本轮发射过）不够——符号发射之后还有逐文件 purge / 全败
                // 清除 / 盘缺清除三路删除，发射过又被删的 id 照样撞外键并
                // 拖垮整个事务（实测事故先例只挡了"从未发射"的一类）。这里在
                // 全部 purge 落定之后逐 id 活性确认，与 analysis_owner_id 的
                // symbol_exists 预检同一防线语义；按"unresolved edges
                // dropped"同语义计数跳过。
                std::unordered_set<cg::db::SymId> known_symbol_ids;
                for (const auto& [_, id] : key_id)
                {
                    if (store_.symbol_exists(id))
                    {
                        known_symbol_ids.insert(id);
                    }
                }
                // 白名单只覆盖本批发射的符号；增量批不重解析全部 TU 时，
                // 边的端点还可以是库中存活的存量符号（定义在其他 TU 的主
                // 文件里，resolve 合法解析到它们），白名单一刀切会把这类
                // 跨 TU 边永久误丢。未命中时对该 id 追加一次主键点查复
                // 核：存活即放行，真被 purge 删除的行点查同样为 false、
                // 照旧拦截——原防线语义完整保留；本检查位于全部 purge 落
                // 定之后，时序安全。miss 边每条一次点查，批量级可忽略。
                const auto id_alive = [&](cg::db::SymId id)
                {
                    return known_symbol_ids.contains(id)
                           || store_.symbol_exists(id);
                };
                for (const EdgeSlot& edge : edge_slots)
                {
                    lease_beat();
                    if (!id_alive(edge.src) || !id_alive(edge.dst))
                    {
                        ++sum.unresolved;
                        continue;
                    }
                    store_.upsert_edge(
                        {.src = edge.src,
                         .dst = edge.dst,
                         .type = edge.type,
                         .site_file = edge.site_file,
                         .site_line = edge.site_line,
                         .attrs = edge.attrs,
                         .confidence = edge.confidence,
                         .occurrences = edge.occurrences});
                }

                for (const TuView& tu : tu_views)
                {
                    lease_beat();
                    for (const cg::extract::IncludeRec& inc : *tu.includes)
                    {
                        if (site_writable(file_id_of(inc.file)))
                        {
                            store_.upsert_include(
                                {.file = file_id_of(inc.file),
                                 .target = inc.target,
                                 .line = inc.line,
                                 .angled = inc.angled});
                        }
                    }
                    for (const cg::extract::ImportRec& im : *tu.imports)
                    {
                        if (site_writable(file_id_of(im.file)))
                        {
                            store_.upsert_import(
                                {.file = file_id_of(im.file),
                                 .module_name = im.module_name,
                                 .line = im.line});
                        }
                    }
                }

                std::unordered_set<cg::db::FileId> hashed_now;
                for (const StaleTu& job : jobs)
                {
                    if (job.hash.empty())
                    {
                        continue;
                    }
                    const cg::db::FileId fid =
                        job.fid != 0 ? job.fid : file_id_of(job.cmd->file);
                    store_.set_file_hash(fid, job.hash);
                    hashed_now.insert(fid);
                }
                // files.parse_status 是整文件汇总：该文件只要有任一存活
                // 编译条目 Ok（本 run 成功，或历史 Ok 且本次 fresh），就
                // 是 Ok；只有全部存活条目都失败（含失败跳过）才 Failed。
                // 这样单条目失败不会再把同文件另一条成功条目的状态污染成
                // Failed，也就不会在下一轮触发那条成功条目的重试。
                std::unordered_set<cg::db::FileId>              final_ok;
                std::unordered_set<cg::db::FileId>              status_failed;
                std::unordered_map<cg::db::FileId, std::string> status_text;
                for (std::size_t i = 0; i < jobs.size(); ++i)
                {
                    const cg::db::FileId fid =
                        jobs[i].fid != 0 ? jobs[i].fid
                                         : file_id_of(jobs[i].cmd->file);
                    if (results[i].failure.empty())
                    {
                        final_ok.insert(fid);
                    }
                    else
                    {
                        status_failed.insert(fid);
                        status_text.try_emplace(fid, results[i].failure);
                    }
                }
                for (std::size_t i = 0; i < plans.size(); ++i)
                {
                    const MainPlan& p = plans[i];
                    if (dedup.at(std::pair{p.cmd->file, p.digest}) != i
                        || p.fid == 0 || p.parse || p.missing)
                    {
                        continue;
                    }
                    if (p.failure_skip != nullptr)
                    {
                        status_failed.insert(p.fid);
                        status_text.try_emplace(
                            p.fid,
                            p.failure_skip->failure_text);
                    }
                    else
                    {
                        final_ok.insert(p.fid);
                    }
                }
                for (const auto& [path, id] : file_ids)
                {
                    if (missing_fids.contains(id)
                        || (status_failed.contains(id)
                            && !final_ok.contains(id)))
                    {
                        continue;
                    }
                    store_.set_file_status(
                        id,
                        cg::util::ParseStatus::Ok,
                        0,
                        std::nullopt,
                        now);
                }
                for (const cg::db::FileId fid : status_failed)
                {
                    if (final_ok.contains(fid))
                    {
                        continue;
                    }
                    store_.set_file_status(
                        fid,
                        cg::util::ParseStatus::Failed,
                        1,
                        status_text[fid],
                        std::nullopt);
                }
                for (const auto& [fid, h] : hash_writes)
                {
                    store_.set_file_hash(fid, h);
                }
                for (const auto& [path, id] : file_ids)
                {
                    if (hash_writes.contains(id) || hashed_now.contains(id))
                    {
                        continue;
                    }
                    const auto it = row_by_path.find(path);
                    if (it != row_by_path.end()
                        && it->second.content_hash.has_value())
                    {
                        continue;
                    }
                    const std::string h = detail::hash_file(path);
                    if (!h.empty())
                    {
                        store_.set_file_hash(id, h);
                    }
                }
                const auto t_b1 = std::chrono::steady_clock::now();

                for (std::size_t i = 0; i < plans.size(); ++i)
                {
                    lease_beat();
                    const MainPlan& p = plans[i];
                    if (p.missing
                        || dedup.at(std::pair{p.cmd->file, p.digest}) != i)
                    {
                        continue;
                    }
                    const cg::db::FileId fid =
                        p.fid != 0 ? p.fid : file_id_of(p.cmd->file);
                    store_.set_fingerprint(
                        {.file_id = fid,
                         .mtime_ns = p.st.mtime_ns,
                         .size = p.st.size,
                         .cmd_digest = p.digest});
                }
                for (const cg::db::FingerprintRow& fp : fp_writes)
                {
                    store_.set_fingerprint(fp);
                }
                for (const auto& [path, id] : file_ids)
                {
                    lease_beat();
                    if (fp_by_file.contains(id) || main_paths.contains(path))
                    {
                        continue;
                    }
                    const detail::FileStat st = detail::stat_file(path);
                    if (st.ok)
                    {
                        store_.set_fingerprint(
                            {.file_id = id,
                             .mtime_ns = st.mtime_ns,
                             .size = st.size,
                             .cmd_digest = ""});
                    }
                }

                {
                    std::unordered_set<cg::db::FileId> main_fids;
                    std::unordered_map<cg::db::FileId, std::vector<std::string>>
                        live_digests;
                    for (std::size_t i = 0; i < plans.size(); ++i)
                    {
                        const MainPlan& p = plans[i];
                        if (dedup.at(std::pair{p.cmd->file, p.digest}) != i)
                        {
                            continue;
                        }
                        const cg::db::FileId fid =
                            p.fid != 0 ? p.fid : file_id_of(p.cmd->file);
                        main_fids.insert(fid);
                        if (!p.missing)
                        {
                            live_digests[fid].push_back(p.digest);
                        }
                    }
                    for (const cg::db::FileRow& fr : existing_files)
                    {
                        if (!main_fids.contains(fr.id))
                        {
                            live_digests[fr.id].emplace_back("");
                        }
                    }
                    for (const auto& [fid, rows] : fp_by_file)
                    {
                        const auto keep = live_digests.find(fid);
                        for (const cg::db::FingerprintRow& fp : rows)
                        {
                            if (keep == live_digests.end()
                                || std::ranges::find(
                                       keep->second,
                                       fp.cmd_digest)
                                       == keep->second.end())
                            {
                                store_.erase_fingerprint(fid, fp.cmd_digest);
                                // 条目退役：命令摘要不再存在的条目整撤其
                                // 贡献票（与指纹同寿命），受影响符号进归
                                // 零仲裁候选。
                                for (const cg::db::SymId sid :
                                     store_.retract_entry_contributions(
                                         fid,
                                         fp.cmd_digest))
                                {
                                    retracted_votes.insert(sid);
                                }
                            }
                        }
                    }
                    // 失败记录与指纹同寿命：命令变了、条目删了、文件不再是
                    // 翻译单元 main —— 对应的失败记录必须退役，否则残留的
                    // "世界状态"会让一条已不存在的编译条目永远跳过。
                    for (const auto& [fid, rows] : fail_by_file)
                    {
                        const auto keep = live_digests.find(fid);
                        for (const cg::db::FailureRow& failure : rows)
                        {
                            if (keep == live_digests.end()
                                || std::ranges::find(
                                       keep->second,
                                       failure.cmd_digest)
                                       == keep->second.end())
                            {
                                store_.erase_failure(fid, failure.cmd_digest);
                            }
                        }
                    }
                }

                // 延迟批的脏文件集（规范化路径）：本批行发生替换/新增/
                // 清除/抹除的文件并入 meta dispatch_stale_files。fid 形态的
                // 同一集合供增量消费（非延迟批的 dispatch_commit 直接消费）。
                std::unordered_set<std::string>    dirty_paths;
                std::vector<std::string>           erased_paths;
                std::unordered_set<cg::db::FileId> dirty_fids;
                // 提交后待写的槽侧快照（延迟分支不产生；dispatch 分支在事
                // 务内序列化）。
                detail::DispatchCommitOut commit_out;
                for (const cg::db::FileId f : rewrite)
                {
                    dirty_fids.insert(f);
                }
                for (const cg::db::FileId f : new_files)
                {
                    dirty_fids.insert(f);
                }
                for (const cg::db::FileId f : all_failed_fids)
                {
                    dirty_fids.insert(f);
                }
                for (const cg::db::FileId f : missing_fids)
                {
                    dirty_fids.insert(f);
                }
                // 可达性清扫快路径：本批变更集（事务内已定型，无并发窗口）
                // 证明不可能产生新的不可达文件时，整场清扫（全表读 + 不可达
                // 判定）直接跳过——单文件编辑的增量小批不再付全表读。判定
                // 输入必须覆盖全部三种删除来源，任一在场即照走全量清扫：
                //   ① 变更集的删除/失踪主文件（missing_fids——事件流点名后
                //      stat 落空与编译库失踪同在此落点）；
                //   ② 新鲜度扫描（pass 1b）发现的文件消失/离开编译库
                //      （抹除集 erased）；
                //   ③ 本批清除逻辑产生的行删除：全败/失踪清除（all_failed_
                //      fids 与 missing_fids 的 purge）、主文件条目整组退役
                //      （根集失守）、重写文件的包含行 purge-回写覆盖差。
                // ①③ 的 purge 只在文件仍持有包含行时才构成行删除：常驻失败
                // TU（failure_skip——真实语料的常态）每批都进 all_failed/
                // missing 集合，但其包含行在失败当时已清空，purge 是幂等空
                // 操作——以快照 old_includes 判空区分，快路径不因常驻失败
                // TU 而永久失效。
                // 另两处保守闸：新文件行（new_files）与大小写漂移重拼
                // （fold_renames）都会往 IncludeResolver 的注册表里添/改路径，
                // 既有包含行的解析可能从后缀命中翻转为精确命中而丢边，故在
                // 场一律不跳。等价性：跳过成立时，包含图相对上一批清扫后的
                // 干净态只增不减（边超集、根超集、无行删除、解析输入不变），
                // 原本可达的文件仍全部可达，全量清扫只会得到空集。
                // CCG_TEST_DISABLE_SWEEP_FASTPATH=1 强制全量清扫：等价测试的
                // 对照臂与红绿测量的红形状（默认关闭，生产零行为）。
                const auto t_sweep0 = std::chrono::steady_clock::now();
                const bool sweep_forced =
                    cg::util::cstd::get_env("CCG_TEST_DISABLE_SWEEP_FASTPATH")
                        .has_value();
                const bool sweep_needed = sweep_forced || [&]
                {
                    const auto purge_deletes_includes =
                        [&](const std::unordered_set<cg::db::FileId>& fids)
                    {
                        for (const cg::db::FileId fid : fids)
                        {
                            if (const auto it = old_includes.find(fid);
                                it != old_includes.end() && !it->second.empty())
                            {
                                return true; // purge 将删真包含行
                            }
                        }
                        return false; // 全部幂等空操作：无行删除
                    };
                    if (purge_deletes_includes(missing_fids)
                        || purge_deletes_includes(all_failed_fids)
                        || !erased.empty() || !new_files.empty()
                        || fold_renames != 0 || st.main_entries_retired)
                    {
                        return true;
                    }
                    // 覆盖差：重写集内每个被 purge 的文件（fresh_keys 键
                    // ∩ rewrite），其快照旧包含目标集必须被本批回写目标
                    // 集覆盖（同一文件的本批全部 TU 记录取并集）。旧目标
                    // 缺席即净减少——仅被它包含的头会就此孤儿化。
                    std::unordered_map<
                        cg::db::FileId,
                        std::unordered_set<std::string_view>>
                        fresh_targets;
                    for (const TuView& tu : tu_views)
                    {
                        for (const cg::extract::IncludeRec& inc : *tu.includes)
                        {
                            fresh_targets[file_id_of(inc.file)].insert(
                                std::string_view{inc.target});
                        }
                    }
                    for (const auto& purged : fresh_keys)
                    {
                        const cg::db::FileId purged_file = purged.first;
                        if (!rewrite.contains(purged_file))
                        {
                            continue;
                        }
                        const auto old_it = old_includes.find(purged_file);
                        if (old_it == old_includes.end())
                        {
                            continue; // 本无旧行：无净减少可言
                        }
                        const auto fresh_it = fresh_targets.find(purged_file);
                        if (fresh_it == fresh_targets.end())
                        {
                            return true; // 有旧行、无回写：净减少
                        }
                        for (const std::string& old_target : old_it->second)
                        {
                            if (!fresh_it->second.contains(
                                    std::string_view{old_target}))
                            {
                                return true; // 旧目标未被回写覆盖
                            }
                        }
                    }
                    return false;
                }();
                if (sweep_needed)
                {
                    // PCH 装载域的声明输入种子（先于快照收集，使按需复活
                    // 的行也进图）：影子 PCH 的装载不重放伞头的包含预处
                    // 理，伞头与其包装头在 includes 表上零边——它们是消费
                    // TU 的解析输入（强制包含 / /Yu 声明），不进种子就会被
                    // 误判不可达。已知边界：种子只保一级声明输入与符号
                    // 写入面，伞头闭包里自身无符号发射的二级头（影子装载
                    // 下零 include 边）仍会被清扫——该头无符号无边，行删
                    // 除对查询面零影响；出现依赖文件行存在性的消费方时
                    // 再扩种子面。
                    std::vector<cg::db::FileId> pch_seed_files;
                    for (const TuView& tu : tu_views)
                    {
                        if (!tu.cmd->pch_use)
                        {
                            continue;
                        }
                        for (const std::string& forced :
                             tu.cmd->forced_includes)
                        {
                            pch_seed_files.push_back(file_id_of(forced));
                        }
                        if (!tu.cmd->pch_header.empty())
                        {
                            const std::string abs_header = cg::util::norm_path(
                                (std::filesystem::path{tu.cmd->directory}
                                 / tu.cmd->pch_header)
                                    .lexically_normal()
                                    .generic_string());
                            pch_seed_files.push_back(file_id_of(abs_header));
                        }
                    }
                    std::vector<std::pair<cg::db::FileId, std::string>>
                        cur_files;
                    for (const cg::db::FileRow& fr : cg::db::all_files(store_))
                    {
                        cur_files.emplace_back(fr.id, fr.path);
                    }
                    std::vector<std::pair<cg::db::FileId, std::string>>
                        cur_links;
                    for (const cg::db::IncludeLink& link :
                         cg::db::all_includes(store_))
                    {
                        cur_links.emplace_back(link.includer, link.target);
                    }
                    std::vector<cg::db::FileId> cur_roots;
                    cur_roots.reserve(
                        plans.size() + symbol_sink_files.size()
                        + pch_seed_files.size());
                    for (const MainPlan& p : plans)
                    {
                        cur_roots.push_back(
                            p.fid != 0 ? p.fid : file_id_of(p.cmd->file));
                    }
                    cur_roots.insert(
                        cur_roots.end(),
                        pch_seed_files.begin(),
                        pch_seed_files.end());
                    // 符号写入面并入种子：本批往某文件写了符号，该文件
                    // 就是活的解析产出域（PCH 装载域里符号是伞头闭包仍在
                    // 解析输入中的唯一在库证据）——删除它的行等于级联清
                    // 掉本批刚写的符号，随后的贡献票发射引用死符号 id 而
                    // 外键失败。
                    cur_roots.insert(
                        cur_roots.end(),
                        symbol_sink_files.begin(),
                        symbol_sink_files.end());
                    for (const cg::db::FileId fid : detail::unreachable_files(
                             cur_files,
                             cur_links,
                             cur_roots))
                    {
                        lease_beat();
                        // 行删除即分析输入变化：延迟批把路径记入脏集
                        // （先于 FK 级联取走行）。
                        for (const auto& [cf_id, cf_path] : cur_files)
                        {
                            if (cf_id == fid)
                            {
                                erased_paths.push_back(cf_path);
                                break;
                            }
                        }
                        capture_analysis_purge_(
                            fid,
                            std::span<const cg::db::StrId>{});
                        // 抹除整文件撤票先于行删除（CASCADE 只清票不收集
                        // 受影响集，显式撤票把挂别处、票全来自本文件的行
                        // 带进归零仲裁）。
                        for (const cg::db::SymId sid :
                             store_.retract_file_contributions(fid))
                        {
                            retracted_votes.insert(sid);
                        }
                        store_.erase_file(fid);
                        // 行删除后同步失效路径→id 解析面：缓存若继续命中
                        // 已删 id，后续按该路径发射的外键列就是死引用；
                        // 失效后同一路径再次被需要时按复活语义重新
                        // upsert（file_id_of 的 miss 分支）。
                        for (auto it = file_ids.begin(); it != file_ids.end();)
                        {
                            it = it->second == fid ? file_ids.erase(it) : ++it;
                        }
                        for (auto it = row_by_path.begin();
                             it != row_by_path.end();)
                        {
                            it = it->second.id == fid ? row_by_path.erase(it)
                                                      : ++it;
                        }
                        for (auto it = fold_row.begin(); it != fold_row.end();)
                        {
                            it = it->second.id == fid ? fold_row.erase(it)
                                                      : ++it;
                        }
                        new_files.erase(fid);
                        ++sum.purged_files;
                        if (cg::util::cstd::get_env("CCG_DEBUG_PURGE")
                                .has_value())
                        {
                            // 调试面：逐文件打印清扫删除的路径（外键
                            // 故障定位时钉死"删了谁"的第一手证据）。
                            for (const auto& [dbg_id, dbg_path] : cur_files)
                            {
                                if (dbg_id == fid)
                                {
                                    try
                                    {
                                        std::println(
                                            std::cerr,
                                            "PURGE sweep_unreachable id={} "
                                            "{}",
                                            fid,
                                            dbg_path);
                                    }
                                    catch (...)
                                    {
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                else
                {
                    ++sum.reachability_sweeps_skipped;
                }
                t_sweep_us_ += detail::phase_us(
                    t_sweep0,
                    std::chrono::steady_clock::now());

                // ===== 贡献差集撤票/续票 + 归零删行（schema v18）=====
                // 位置纪律：边发射（名字面投票）与抹除清扫都已完成、
                // dispatch/传播装载尚未开始——票账此刻是本批终态；行删除
                // 遵守"装载前删行"的既有教训（决议表按存量行构建，幽灵
                // 行在场会污染候选计数——与 Extends 锚删除同一防线）。
                {
                    // 记账三段计时（旧票读/差集撤票/新票落库；归零仲裁并入
                    // 差集段）：端到端包络与 store/commit 四分段都圈不出这
                    // 块的独立成本，六键（分析段）也不含它——三段累计器
                    // 落 meta 供增量批与全量批对照（性能纪律的观测面）。
                    const auto  t_cr0 = std::chrono::steady_clock::now();
                    auto        t_cd0 = t_cr0;
                    auto        t_ci0 = t_cr0;
                    std::size_t contrib_entry_i = 0;
                    for (EntryVotes& ev : entry_votes)
                    {
                        lease_beat();
                        // 测试注入（生产零开销：无环境变量即什么都不做）：
                        // 撤票差集循环中途硬崩——崩溃一致性测试的精确现
                        // 场（事务外杀进程 = 部分撤票永不落盘，重启后票账
                        // 必须与崩溃前完全一致）。
                        if (contrib_entry_i == 1
                            && cg::util::cstd::get_env(
                                   "CCG_TEST_CRASH_IN_CONTRIB")
                                   .has_value())
                        {
                            std::abort();
                        }
                        ++contrib_entry_i;
                        // 条目身份的 files 行一律按主文件路径经 file_id_of
                        // 解析（陈旧快照核对的复活映射与“新 TU fid=0”两
                        // 种形态都被覆盖——绝不把死 id 或 0 写进外键）。
                        const cg::db::FileId fid = file_id_of(ev.cmd->file);
                        t_cd0 = std::chrono::steady_clock::now();
                        const std::vector<cg::db::SymId> old_votes =
                            store_.contributions_of_entry(fid, ev.digest);
                        t_ci0 = std::chrono::steady_clock::now();
                        t_contrib_read_us_ += detail::phase_us(t_cd0, t_ci0);
                        for (const cg::db::SymId sid : old_votes)
                        {
                            if (!ev.votes.contains(sid))
                            {
                                store_.retract_contribution(
                                    sid,
                                    fid,
                                    ev.digest);
                                retracted_votes.insert(sid);
                            }
                        }
                        std::vector<cg::db::SymId> fresh_span(
                            ev.votes.begin(),
                            ev.votes.end());
                        t_cd0 = std::chrono::steady_clock::now();
                        t_contrib_diff_us_ += detail::phase_us(t_ci0, t_cd0);
                        store_.contribute_symbols(fid, ev.digest, fresh_span);
                        t_ci0 = std::chrono::steady_clock::now();
                        t_contrib_insert_us_ += detail::phase_us(t_cd0, t_ci0);
                    }
                    // heal 折叠（解析前的独立小事务）删 loser 文件行时
                    // CASCADE 清票但不走受影响集收集——并入归零仲裁候选，
                    // 否则票数虚低的窗口内其它撤票触及同符号会提前归零
                    // 删行（heal_vote_affected_ 在 heal 调用点填）。
                    retracted_votes.insert(
                        heal_vote_affected_.begin(),
                        heal_vote_affected_.end());
                    std::vector<cg::db::SymId> candidates(
                        retracted_votes.begin(),
                        retracted_votes.end());
                    std::ranges::sort(candidates);
                    const std::vector<cg::db::SymId> zeroed =
                        store_.zero_contribution_symbols(candidates);
                    t_contrib_diff_us_ += detail::phase_us(
                        t_ci0,
                        std::chrono::steady_clock::now());
                    if (!zeroed.empty())
                    {
                        if (opt_.dispatch)
                        {
                            for (const cg::db::SymId id : zeroed)
                            {
                                const auto row = store_.symbol_row(id);
                                if (row.has_value())
                                {
                                    incr_doomed_keys_.push_back(
                                        row->unique_key);
                                    incr_doomed_names_.push_back(row->name);
                                }
                            }
                        }
                        store_.erase_symbols(zeroed);
                        if (cg::util::cstd::get_env("CCG_DEBUG_PURGE")
                                .has_value())
                        {
                            try
                            {
                                std::println(
                                    std::cerr,
                                    "PURGE contrib_zero_erase count={} "
                                    "(zero-vote rows)",
                                    zeroed.size());
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                    store_.set_meta(
                        "contrib_read_us",
                        std::to_string(
                            static_cast<std::int64_t>(t_contrib_read_us_)));
                    store_.set_meta(
                        "contrib_diff_us",
                        std::to_string(
                            static_cast<std::int64_t>(t_contrib_diff_us_)));
                    store_.set_meta(
                        "contrib_insert_us",
                        std::to_string(
                            static_cast<std::int64_t>(t_contrib_insert_us_)));
                }

                // schema v10 阶段 B：全局传播固定点 + dispatch。放在 extends/
                // overrides 边与孤儿清扫之后、同一事务提交之前——家族闭包
                // 看见本批边，且本运行将抹除的文件（重命名/移出构建的旧行）
                // 已先消失，绝不参与符号解析或类型传播。
                if (opt_.dispatch && opt_.defer_dispatch)
                {
                    // 挪时机设计（编辑批）：阶段 A 行已就位，这里只按文件
                    // 粒度撤掉脏文件站点上的 vd 边（防旧边对已替换的站点
                    // 顶名存续），RTA/类型传播/dispatch/写回整体推迟到收敛批
                    // （ccg update --drain / UserPromptSubmit 派生）。库旧一
                    // 会：dispatch_stale 记滞后文件数、dispatch_stale_files
                    // 记脏文件集（与既有集合取并，多批编辑累积），查询侧
                    // DispatchIndex 读到即显式标注，绝不静默。
                    lease_beat();
                    const auto mark_dirty = [&](cg::db::FileId f)
                    {
                        store_.erase_vd_edges_of_file(f);
                        if (const auto it = path_of.find(f);
                            it != path_of.end())
                        {
                            dirty_paths.insert(it->second);
                        }
                    };
                    for (const cg::db::FileId f : rewrite)
                    {
                        mark_dirty(f);
                    }
                    for (const cg::db::FileId f : new_files)
                    {
                        mark_dirty(f);
                    }
                    for (const cg::db::FileId f : all_failed_fids)
                    {
                        mark_dirty(f);
                    }
                    for (const cg::db::FileId f : missing_fids)
                    {
                        mark_dirty(f);
                    }
                    for (const std::string& p : erased_paths)
                    {
                        dirty_paths.insert(p);
                    }
                    // 与既有脏集合并（前一批延迟编辑未收敛时累积）。
                    for (const std::string& line : detail::read_meta_lines(
                             store_,
                             "dispatch_stale_files"))
                    {
                        dirty_paths.insert(line);
                    }
                    std::vector<std::string> joined(
                        dirty_paths.begin(),
                        dirty_paths.end());
                    std::ranges::sort(joined);
                    const std::string packed = cg::util::join(joined, "\n");
                    store_.set_meta("dispatch_stale_files", packed);
                    store_.set_meta(
                        "dispatch_stale",
                        std::to_string(
                            static_cast<std::int64_t>(joined.size())));
                    sum.dispatch_stale_files =
                        static_cast<std::int64_t>(joined.size());
                    // 增量捕获（被删行下游见证）落盘给收敛批，与既有 meta
                    // 内容取并（多批延迟编辑累积；收敛批消费后清空）。
                    const auto persist_lines =
                        [&](const char* key, auto&& extra)
                    {
                        std::set<std::string> merged;
                        for (const auto& item : extra)
                        {
                            if constexpr (
                                std::is_same_v<
                                    std::remove_cvref_t<decltype(item)>,
                                    std::pair<std::string, std::string>>)
                            {
                                merged.insert(
                                    cg::util::compose_key(
                                        item.first,
                                        item.second));
                            }
                            else
                            {
                                merged.insert(item);
                            }
                        }
                        if (const auto prev = store_.meta(key);
                            prev.has_value())
                        {
                            std::size_t start = 0;
                            while (start <= prev->size())
                            {
                                const std::size_t end = prev->find('\n', start);
                                const std::string_view line(
                                    prev->data() + start,
                                    (end == std::string::npos ? prev->size()
                                                              : end)
                                        - start);
                                if (!line.empty())
                                {
                                    merged.insert(std::string{line});
                                }
                                if (end == std::string::npos)
                                {
                                    break;
                                }
                                start = end + 1;
                            }
                        }
                        std::string out_packed;
                        for (const std::string& line : merged)
                        {
                            if (!out_packed.empty())
                            {
                                out_packed.push_back('\n');
                            }
                            out_packed += line;
                        }
                        store_.set_meta(key, out_packed);
                    };
                    persist_lines("dispatch_prev_slots", incr_prev_slots_);
                    persist_lines("dispatch_prev_callees", incr_prev_callees_);
                    persist_lines("dispatch_doomed_keys", incr_doomed_keys_);
                    persist_lines("dispatch_doomed_names", incr_doomed_names_);
                    // 名字桶警报的升级腿同样跨批传递：收敛批的
                    // dispatch_commit 读到即升级并消费清除。
                    if (name_lift_escalation)
                    {
                        store_.set_meta("dispatch_name_escalation", "1");
                    }
                }
                else if (opt_.dispatch)
                {
                    commit_out = detail::dispatch_commit(
                        {.store = &store_,
                         .roots = &opt_.roots,
                         .sum = &sum,
                         .db_path = opt_.db_path,
                         .owner_of = analysis_owner_id,
                         .file_of = file_id_of,
                         .beat = lease_beat,
                         .dirty_files = &dirty_fids,
                         .prev_slots = &incr_prev_slots_,
                         .prev_callees = &incr_prev_callees_,
                         .doomed_keys = &incr_doomed_keys_,
                         .doomed_names = &incr_doomed_names_,
                         .name_escalation = name_lift_escalation});
                    sum.dispatch_stale_files = 0;
                }

                tx.commit();
                // 先库后快照：提交成功后原子替换快照文件（tmp+rename 语义，
                // 旧 mmap 读者继续读旧字节）。写失败只丢加速无正确性影响——
                // 下一批 seq 失配自弃快照。
                if (!commit_out.snapshot_bytes.empty())
                {
                    const auto t_snapw0 = std::chrono::steady_clock::now();
                    (void)cg::util::platform::atomic_write_file(
                        commit_out.snapshot_path,
                        std::string_view{commit_out.snapshot_bytes});
                    store_.set_meta(
                        "analysis_snap_write_us",
                        std::to_string(
                            static_cast<std::int64_t>(detail::phase_us(
                                t_snapw0,
                                std::chrono::steady_clock::now()))));
                }
                const auto t_commit1 = std::chrono::steady_clock::now();
                t_store_a_us_ += detail::phase_us(t_store0, t_a1);
                t_purge_us_ += detail::phase_us(t_a1, t_purge1);
                t_store_b_us_ += detail::phase_us(t_purge1, t_b1);
                t_commit_us_ += detail::phase_us(t_b1, t_commit1);
                if (!coord_holder.empty())
                {
                    // 实测提交段墙钟写回 meta：下一次运行的 claim 租约按
                    // max(update_lease_ms, 2x) 自适应（见上方租约计算）。
                    store_.set_meta(
                        "index_commit_ms",
                        std::to_string(
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(t_commit1 - t_store0)
                                .count()));
                }
                if (opt_.progress)
                {
                    std::println(
                        std::cerr,
                        "  store batch: {} TUs (A {:.0f}us, purge "
                        "{:.0f}us, B {:.0f}us, commit {:.0f}us, sweep "
                        "{:.0f}us, contrib read {:.0f}us diff {:.0f}us "
                        "insert {:.0f}us{})",
                        batch.size(),
                        t_store_a_us_,
                        t_purge_us_,
                        t_store_b_us_,
                        t_commit_us_,
                        t_sweep_us_,
                        t_contrib_read_us_,
                        t_contrib_diff_us_,
                        t_contrib_insert_us_,
                        sum.reachability_sweeps_skipped > 0
                            ? std::format(
                                  " [fast-path skipped {}/1 sweep]",
                                  sum.reachability_sweeps_skipped)
                            : std::string{});
                }
            }

            if (coord_guard.has_value())
            {
                // 释放前看一眼队列：索引运行期间的编辑已入队时留给调用方
                // detach `ccg update --drain` 收敛（Stop 钩子默认未装，
                // 事件流之外由索引运行主动收一次尾）。快照失败不阻塞退出。
                try
                {
                    sum.coord_drain_pending =
                        !store_.claim_snapshot().pending.empty();
                }
                catch (const std::exception&)
                {
                    sum.coord_drain_pending = false;
                }
                coord_guard.reset(); // RAII 释放（只删自己的行）
            }
        }

    private:
        /**
         * @brief 把一个文件的全部既有符号并入末日集合（run 的符号末日段
         *        与提交段的重写覆盖共用）。
         */
        void doom_symbols_of_file_(
            std::unordered_set<cg::db::SymId>& doomed,
            cg::db::FileId                     fid) const
        {
            if (fid == 0)
            {
                return;
            }
            for (const cg::db::FileSymbolKey& k :
                 cg::db::file_symbol_keys(store_, fid))
            {
                doomed.insert(k.id);
            }
        }

        /**
         * @brief pass 1 的单条目新鲜度分类：在已填 cmd/digest 的计划上
         *        补齐 fid/stat/parse/missing/hash/failure_skip。
         *
         * @internal 决策链逐层早退：盘缺 → 强制/播种/新条目/不可指纹
         * （一律重试）→ 失败记录世界比对 → 历史正常条目的 stat 快路径
         * 与哈希确认。调用方保证去重先于本函数（重复条目不付 stat 代价）。
         */
        [[nodiscard]]
        MainPlan plan_entry_(
            const cg::clang::CompileCommand& cmd,
            MainPlan                         p,
            const std::unordered_map<
                std::string,
                cg::db::FileRow,
                cg::util::string_hash,
                std::equal_to<>>&                                   row_by_path,
            const std::unordered_map<std::string, cg::db::FileRow>& drift_row,
            const std::unordered_map<
                cg::db::FileId,
                std::vector<cg::db::FingerprintRow>>& fp_by_file,
            const std::unordered_map<
                cg::db::FileId,
                std::vector<cg::db::FailureRow>>&  fail_by_file,
            bool                                   force_all,
            const std::unordered_set<std::string>& seed_fold,
            const std::string&                     engine_token,
            const std::string&                     roots_digest) const
        {
            const auto             row_it = row_by_path.find(cmd.file);
            const cg::db::FileRow* row =
                row_it == row_by_path.end() ? nullptr : &row_it->second;
            // 漂移条目：精确拼写未命中但折叠到同一物理文件（Windows
            // 上仅大小写不同）。行 id 就是身份：复用既有行，不因拼写
            // 差异触发重解析——内容新鲜度仍由 stat/哈希与种子裁决。
            if (row == nullptr)
            {
                if (const auto drift_it = drift_row.find(cmd.file);
                    drift_it != drift_row.end())
                {
                    row = &drift_it->second;
                }
            }
            if (row != nullptr)
            {
                p.fid = row->id;
            }
            p.st = detail::stat_file(cmd.file);
            if (!p.st.ok)
            {
                p.missing = true;
                return p;
            }
            const cg::db::FingerprintRow* fp_row = nullptr;
            if (row != nullptr)
            {
                const auto fp_it = fp_by_file.find(row->id);
                if (fp_it != fp_by_file.end())
                {
                    for (const cg::db::FingerprintRow& cand : fp_it->second)
                    {
                        if (cand.cmd_digest == p.digest)
                        {
                            fp_row = &cand;
                            break;
                        }
                    }
                }
            }

            // 逐条目的失败判定：force/种子/无行/无指纹一律重试；
            // 其余情况只有在该条目没有失败记录、或失败记录里的世界
            // 状态（引擎/feed/根集/内容/依赖闭包）变了、或瞬时类退避
            // 到期时才重试。绝不能再用 files.parse_status != Ok 做判定
            // ——整文件状态是汇总，一条失败条目会因此污染同文件另一条
            // 成功条目，重试失败后经符号末日把整语料拉进重解析。
            const bool seeded =
                !seed_fold.empty()
                && seed_fold.contains(cg::util::path_compare_key(cmd.file));
            if (force_all || seeded || row == nullptr || fp_row == nullptr)
            {
                p.parse = true;
            }
            else
            {
                const cg::db::FailureRow* failure = nullptr;
                if (const auto fit = fail_by_file.find(row->id);
                    fit != fail_by_file.end())
                {
                    for (const cg::db::FailureRow& cand : fit->second)
                    {
                        if (cand.cmd_digest == p.digest)
                        {
                            failure = &cand;
                            break;
                        }
                    }
                }
                if (failure == nullptr)
                {
                    // 无失败记录：该条目历史上是 Ok，走正常新鲜度。
                    if (opt_.paranoid)
                    {
                        p.hash = detail::hash_file(cmd.file);
                        p.parse = !row->content_hash.has_value()
                                  || *row->content_hash != p.hash;
                    }
                    else if (
                        fp_row->mtime_ns == p.st.mtime_ns
                        && fp_row->size == p.st.size)
                    {
                        // stat 快路径：身份未变，世界未动。
                    }
                    else
                    {
                        p.hash = detail::hash_file(cmd.file);
                        if (!row->content_hash.has_value()
                            || *row->content_hash != p.hash)
                        {
                            p.parse = true;
                        }
                    }
                }
                else if (
                    failure->engine_token != engine_token
                    || failure->roots_digest != roots_digest
                    || failure->feed_digest != detail::feed_digest_of(cmd)
                    || detail::deps_changed(failure->deps_json))
                {
                    p.parse = true; // 世界变了
                }
                else
                {
                    // 主文件 "touch 不触发、改内容才触发"：stat 变化后
                    // 用 content_hash 复核，字节未变即视为世界未变（
                    // checkout/branch switch 的 mtime 抖动同理）。依赖
                    // 闭包另按 deps_changed 的 stat 身份判定，见上。
                    const bool content_same =
                        (failure->mtime_ns == p.st.mtime_ns
                         && failure->size == p.st.size)
                        || (!failure->content_hash.empty() &&
                            [&]
                            {
                                p.hash = detail::hash_file(cmd.file);
                                return !p.hash.empty()
                                       && p.hash == failure->content_hash;
                            }());
                    if (!content_same)
                    {
                        p.parse = true;
                    }
                    else if (
                        detail::is_transient_failure_class(
                            failure->failure_class)
                        && cg::util::cstd::epoch_ms() >= failure->retry_after)
                    {
                        p.parse = true; // 瞬时失败退避到期
                    }
                    else
                    {
                        p.failure_skip = failure;
                    }
                }
            }
            if (p.parse && p.hash.empty())
            {
                p.hash = detail::hash_file(cmd.file);
            }
            return p;
        }

        /**
         * @brief 把一次 parse 的错误诊断分类为失败文本。
         *
         * @internal 分类链（每层早退）：模块诊断具名类 → 方言诊断具名类
         *           → "file not found" 补包含目录计数 → 原样首条诊断。
         */
        [[nodiscard]]
        static auto classify_diag_failure_(
            const cg::clang::ParsedTu&       parsed,
            const cg::clang::CompileCommand& cmd,
            const std::vector<std::string>&  argv) -> std::string
        {
            std::string       failure = parsed.errors.front().spelling;
            const std::string classified = detail::classify_module_diag_failure(
                parsed.errors.front().spelling,
                parsed.errors.size(),
                cmd);
            if (!classified.empty())
            {
                return classified;
            }
            const std::string dialect = detail::classify_dialect_diag_failure(
                parsed.errors.front().spelling,
                cmd);
            if (!dialect.empty())
            {
                return dialect;
            }
            if (failure.find("file not found") != std::string::npos)
            {
                failure += std::format(
                    " ({} include dirs fed)",
                    detail::include_dir_count(argv));
            }
            return failure;
        }

        /**
         * @brief 解析一个 TU 的一次尝试（池任务体的单体）。
         *
         * @internal 重试阶梯的档位由调用方选 argv 后整体重入本函数；此处
         *           只做一次忠实重放：preordained/喂给校验 → parse →
         *           诊断分类 → 守卫化 extract/analysis。任何失败只写进
         *           out.failure，绝不抛出（异常就地收编为失败文本）。
         * @param attempt_errors 出参：本次尝试错误级诊断的条数（未发起
         *                      parse、装载失败/引擎异常时为 0）。影子
         *                      PCH 回退的注记文案用它区分触发形态——
         *                      有诊断=影子装载成功但 TU 自身诊断失败；
         *                      零诊断=影子装载本身失败。
         * @return 是否真正发起过 parse（影子 PCH 文本回退的重试门槛）。
         */
        auto parse_attempt_(
            const cg::clang::Index&                             index,
            const StaleTu&                                      tu,
            const std::vector<std::string>&                     argv,
            const std::string&                                  engine_token,
            const std::unordered_map<std::string, std::string>& producer_std_of,
            cg::util::PathCaseCache&                            case_cache,
            std::mutex&                                         stderr_mx,
            std::atomic<std::int64_t>& dialect_fixit_ok,
            std::atomic<std::int64_t>& dialect_fixit_failed,
            std::atomic<std::int64_t>& encoding_scrub_ok,
            std::atomic<std::int64_t>& encoding_scrub_failed,
            std::atomic<std::int64_t>& benign_errored_ok,
            TuResult&                  out,
            std::size_t&               attempt_errors) -> bool
        {
            bool did_parse = false;
            attempt_errors = 0;
            // 调试开关进程内求值一次（每个 TU 的每次尝试都经过这里）；
            // 改环境变量需重启进程才生效。
            static const bool debug_argv =
                cg::util::cstd::get_env("CCG_DEBUG_ARGV").has_value();
            static const bool debug_diags =
                cg::util::cstd::get_env("CCG_DEBUG_DIAGS").has_value();
            try
            {
                out.failure =
                    !tu.cmd->preordained_failure.empty()
                        ? tu.cmd->preordained_failure
                        : detail::validate_bmi_feeds(*tu.cmd, engine_token);
                if (debug_argv)
                {
                    const std::scoped_lock lock{stderr_mx};
                    std::print(std::cerr, "ARGV {}:", tu.cmd->file);
                    for (const std::string& a : tu.cmd->argv)
                    {
                        std::print(std::cerr, " [{}]", a);
                    }
                    std::println(std::cerr);
                }
                if (out.failure.empty())
                {
                    const auto t_parse0 = std::chrono::steady_clock::now();
                    did_parse = true;
                    const cg::clang::ParsedTu parsed = cg::clang::parse(
                        index,
                        tu.cmd->file,
                        argv,
                        cg::clang::is_module_tu(*tu.cmd)
                            && !tu.cmd->overlay.demodule,
                        &tu.cmd->overlay,
                        tu.cmd->pch.status
                            == cg::clang::PchUse::Status::replay_degraded);
                    const auto t_visit0 = std::chrono::steady_clock::now();
                    out.parse_us = detail::phase_us(t_parse0, t_visit0);
                    // 诊断驱动的内存源码改写（cl 方言宽松特化，
                    // 编译器 FixIt 给点、不落盘、一次重解析）：
                    // 注记被改写的 TU，两种结局原子分账。
                    if (parsed.dialect_fixit_applied)
                    {
                        if (parsed.errors.empty())
                        {
                            ++dialect_fixit_ok;
                        }
                        else
                        {
                            ++dialect_fixit_failed;
                        }
                        out.dialect_fixit_note = std::format(
                            "{} fixit edit(s) in {} file(s), "
                            "reparse {}",
                            parsed.dialect_fixit_edits,
                            parsed.dialect_fixit_files.size(),
                            parsed.errors.empty() ? "ok" : "still failed");
                    }
                    // 编码消毒（ANSI 码页坏字节转义、不落盘、
                    // 一次重解析）：同款注记与两种结局分账。
                    if (parsed.encoding_scrub_applied)
                    {
                        if (parsed.errors.empty())
                        {
                            ++encoding_scrub_ok;
                        }
                        else
                        {
                            ++encoding_scrub_failed;
                        }
                        out.encoding_scrub_note = std::format(
                            "{} encoding unit(s) in {} file(s), "
                            "reparse {}",
                            parsed.encoding_scrub_edits,
                            parsed.encoding_scrub_files.size(),
                            parsed.errors.empty() ? "ok" : "still failed");
                    }
                    if (!parsed.tu.valid())
                    {
                        out.failure = std::format(
                            "engine-no-tu: the parse engine "
                            "reported a completed parse of '{}' "
                            "but produced no translation unit "
                            "(defensive contract: a NULL TU is "
                            "never dereferenced); re-index with "
                            "another engine version if this "
                            "persists",
                            tu.cmd->file);
                    }
                    else if (
                        !parsed.errors.empty()
                        && !detail::all_errors_benign(parsed.errors))
                    {
                        attempt_errors = parsed.errors.size();
                        out.failure =
                            classify_diag_failure_(parsed, *tu.cmd, argv);
                        // 模块 TU
                        // 的失败依赖快照补给：诊断定位的文件（含头文件）
                        // 冻结进失败记录的 deps_json——修复它们即可把该 TU 拉回
                        // 重试，无需 --force。主文件自身已有
                        // stat/内容哈希盯守， 跳过不记。
                        if (cg::clang::is_module_tu(*tu.cmd))
                        {
                            std::unordered_set<std::string> blamed;
                            for (const cg::clang::Diagnostic& d : parsed.errors)
                            {
                                if (d.location.file.empty())
                                {
                                    continue;
                                }
                                const std::string blamed_file =
                                    cg::util::norm_path(d.location.file);
                                if (blamed_file != tu.cmd->file
                                    && blamed.insert(blamed_file).second)
                                {
                                    out.failure_files.push_back(
                                        std::move(blamed_file));
                                }
                            }
                        }
                        if (debug_diags)
                        {
                            const std::scoped_lock lock{stderr_mx};
                            for (const cg::clang::Diagnostic& d : parsed.errors)
                            {
                                std::println(
                                    std::cerr,
                                    "  DIAG {}",
                                    d.spelling);
                            }
                        }
                    }
                    else
                    {
                        // 进入此分支的 Error 级诊断（若有）已全部命中良性
                        // 白名单（TU 有效 + AST 完好）：TU 按成功索引。注记
                        // 与计数在走查成功后写下——走查崩溃判
                        // engine-visit-crash 失败，良性计数绝不含失败 TU。
                        const bool benign_errored = !parsed.errors.empty();
                        // 头缓存查询桥：模块 TU 与 PCH 条目不参与（pcm/PCH
                        // 字节不在任何内容哈希内——宁可 miss）。变体键 =
                        // 本 TU 的 parse_digest（argv 已剥主文件 token，
                        // 同旗标 TU 共享）。
                        std::optional<headcache::HeadCache::Bridge> hc_bridge;
                        if (headcache_ && !cg::clang::is_module_tu(*tu.cmd)
                            && tu.cmd->pch.identity.empty())
                        {
                            const auto hit =
                                hc_main_hash_of_.find(tu.cmd->file);
                            if (hit != hc_main_hash_of_.end())
                            {
                                hc_bridge.emplace(headcache_->bridge_for(
                                    tu.digest,
                                    tu.cmd->file,
                                    hit->second));
                            }
                        }
                        const bool visited = cg::util::guard::run_guarded(
                            [&]
                            {
                                out.ex = cg::extract::extract(
                                    parsed,
                                    extract_opt_,
                                    case_cache,
                                    hc_bridge.has_value() ? &*hc_bridge
                                                          : nullptr);
                            });
                        if (visited)
                        {
                            out.visit_us = detail::phase_us(
                                t_visit0,
                                std::chrono::steady_clock::now());
                            if (benign_errored)
                            {
                                ++benign_errored_ok;
                                out.benign_error_note = std::format(
                                    "ignored {} benign error(s) [{}]: {}",
                                    parsed.errors.size(),
                                    detail::benign_error_family_summary(
                                        parsed.errors),
                                    parsed.errors.front().spelling);
                            }
                            // 第二次定向走查：analysis 只依赖
                            // cg.clang/cg.util；崩溃只丢本 TU 的
                            // 分析数据，绝不影响已成功的 TuExtract。
                            const auto t_collect0 =
                                std::chrono::steady_clock::now();
                            const bool analyzed = cg::util::guard::run_guarded(
                                [&]
                                {
                                    out.analysis = cg::analysis::collect(
                                        parsed,
                                        collect_opt_,
                                        case_cache);
                                });
                            out.analysis_us = detail::phase_us(
                                t_collect0,
                                std::chrono::steady_clock::now());
                            if (!analyzed)
                            {
                                out.analysis = {};
                                out.analysis_failed = true;
                            }
                        }
                        else
                        {
                            out.ex = {};
                            out.failure = std::format(
                                "engine-visit-crash: the parse "
                                "engine faulted while walking the "
                                "AST of '{}' (a crash inside "
                                "libclang's cursor traversal - not "
                                "ccg state); the TU is skipped and "
                                "the index run continues - "
                                "re-index with another engine "
                                "version if this persists",
                                tu.cmd->file);
                        }
                    }
                }
            }
            catch (const cg::clang::ParseError& pe)
            {
                if (pe.code() == cg::clang::ParseErrc::Crashed)
                {
                    out.failure = "stale-cascade-crash: libclang recovered "
                                  "from a process crash mid-parse "
                                  "(CXError_Crashed) - a stale downstream BMI "
                                  "referencing removed declarations is the "
                                  "known cause (measured); "
                                  "rebuild the module BMIs";
                }
                else if (pe.code() == cg::clang::ParseErrc::AstReadError)
                {
                    const std::string classified =
                        detail::classify_ast_read_error(
                            *tu.cmd,
                            engine_token,
                            producer_std_of);
                    out.failure = classified.empty() ? std::string{pe.what()}
                                                     : classified;
                }
                else
                {
                    out.failure = pe.what();
                }
                if (tu.cmd->module_feeds.empty()
                    && detail::classifies_as_feed_absent(*tu.cmd))
                {
                    out.failure = std::format(
                        "module-feeds-absent: '{}' authors an "
                        "import but its recorded command carries "
                        "no -fmodule-file feeds (unbuilt "
                        "configuration, or a stripped/missing "
                        ".modmap) - build that configuration or "
                        "index the built one",
                        tu.cmd->file);
                }
            }
            catch (const std::exception& e)
            {
                out.failure = e.what();
            }
            return did_parse;
        }

        /**
         * @brief 供父链接使用的 "scope|name" 别名。
         *
         * @internal 前序走查先于成员压入记录，故该映射是完备的。
         */
        [[nodiscard]]
        static auto record_alias(const cg::extract::SymbolRec& s) -> std::string
        {
            return s.scope.empty() ? s.name
                                   : std::format("{}|{}", s.scope, s.name);
        }

        /**
         * @brief 从本符号的 scope 与父名推导父记录的 "scope|name" 别名
         *        （'scope' 以 "::parent" 结尾）。记录链不一致/无父时返回
         *        空串（不链接）。
         */
        [[nodiscard]]
        static auto parent_alias_of(const cg::extract::SymbolRec& s)
            -> std::string
        {
            if (s.parent.empty())
            {
                return {};
            }
            std::string       parent_scope;
            const std::string tail = "::" + s.parent;
            if (s.scope.ends_with(tail))
            {
                parent_scope = s.scope.substr(0, s.scope.size() - tail.size());
            }
            else if (s.scope == s.parent)
            {
                parent_scope.clear();
            }
            else
            {
                return {};
            }
            return parent_scope.empty()
                       ? s.parent
                       : std::format("{}|{}", parent_scope, s.parent);
        }

        [[nodiscard]]
        static auto function_like(cg::extract::SymKind k) -> bool
        {
            switch (k)
            {
            case cg::extract::SymKind::Function:
            case cg::extract::SymKind::Method:
            case cg::extract::SymKind::Constructor:
            case cg::extract::SymKind::Destructor:
                return true;
            default:
                return false;
            }
        }

        cg::db::GraphStore& store_;
        IndexOptions        opt_;
        /**
         * @brief 根 + 编译数据库自身的目录拼写，每运行构建一次；每个工人
         *        的提取都把身份锚定到它们上。
         */
        cg::extract::ExtractOptions  extract_opt_;
        cg::analysis::CollectOptions collect_opt_;

        /**
         * @brief 增量捕获：本批被 purge/erase 文件的旧边见证（评审
         *        红线"被删行下游"）。
         *
         * @internal store 阶段逐文件累积；非延迟批直接交给 dispatch_commit，
         *           延迟批落 meta（dispatch_prev_*）给收敛批。
         */
        std::vector<std::pair<std::string, std::string>> incr_prev_slots_;
        std::vector<std::string>                         incr_prev_callees_;
        std::vector<std::string>                         incr_doomed_keys_;
        std::vector<std::string>                         incr_doomed_names_;

        /** @brief purge/erase 前捕获该文件的派生数据旧边见证（只读不删）。 */
        void capture_analysis_purge_(
            cg::db::FileId                 fid,
            std::span<const cg::db::StrId> fresh)
        {
            if (!opt_.dispatch)
            {
                return;
            }
            const cg::db::PurgeCapture cap =
                store_.capture_analysis_purge(fid, fresh);
            incr_prev_slots_.insert(
                incr_prev_slots_.end(),
                cap.slots.begin(),
                cap.slots.end());
            incr_prev_callees_.insert(
                incr_prev_callees_.end(),
                cap.callee_keys.begin(),
                cap.callee_keys.end());
            incr_doomed_keys_.insert(
                incr_doomed_keys_.end(),
                cap.doomed_keys.begin(),
                cap.doomed_keys.end());
            incr_doomed_names_.insert(
                incr_doomed_names_.end(),
                cap.doomed_names.begin(),
                cap.doomed_names.end());
        }

        // ===== 波式分解（flush-batch 第二轨原型）=====
        // 解析期逐波把成功 TU 的重型载荷拆解进"槽 + 账本"并即刻释放原始
        // 载荷——内存峰值的收益来源。提交事务结构不变：一切库写入仍在主事
        // 务内（符号发射、文件封口 purge、边后置、lambda/站点/包含全部
        // 原位），波内零库写（kill 重做窗口因此天然干净）。
        //
        // @internal 原型开关走 CCG_WAVE_SIZE 环境变量（0=关=现状单趟；
        //            >0=按 jobs 序切波，波不切同主文件的相邻 TU）。实施
        //            期换成 旗标>库配置>默认16 的贯通链（默认值 16 为既定
        //            口径）。

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
            std::uint32_t           attrs = 0; // 首写胜出（与 EdgeSlot 同构）
            std::int64_t            occurrences = 0; // 出现折叠总数
            std::int64_t emissions = 0; // 原始发射总数（unresolved 计数口径）
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
            std::vector<cg::extract::IncludeRec> includes; // 包含差与回写原料
            std::vector<cg::extract::ImportRec>  imports;
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

        bool      fb_active_ = false; // 本运行启用了波式分解
        long long fb_wave_size_ = 0;  // 波大小（TU 数；0 = 关）
        /** @brief keep 集消费游标（账本与 jobs 序同序，单调推进）。 */
        std::size_t fb_keep_cursor_ = 0;
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
        std::vector<FlushLamSlot> fb_lams_;
        std::unordered_map<
            std::string,
            std::uint32_t,
            cg::util::string_hash,
            std::equal_to<>>
            fb_lam_of_;
        /** @brief 类型线索槽：首现定序+首写胜出（upsert 冲突忽略不刷列）。 */
        std::vector<cg::analysis::TypeHintRec>                      fb_hints_;
        std::unordered_map<std::size_t, std::vector<std::uint32_t>> fb_hint_of_;
        /** @brief 调用点槽：首现定序+首写胜出（同上，INSERT 后无刷列）。 */
        std::vector<cg::analysis::CallSiteRec>                      fb_sites_;
        std::unordered_map<std::size_t, std::vector<std::uint32_t>> fb_site_of_;
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
        /** @brief 边槽哈希桶：哈希（槽身份）→ 候选槽下标（冲突链上逐槽比对）。
         */
        std::unordered_map<std::size_t, std::vector<std::uint32_t>> fb_edge_of_;
        std::vector<FlushLedgerTu>                                  fb_ledger_;
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
                                        fb_path_intern_; // 路径串 → intern 号
        std::vector<const std::string*> fb_path_list_;   // intern 号 → 路径串
        /** @brief 文件 → 本批新发射键 intern 号集（覆盖差判定的等价重建输入）。
         */
        std::unordered_map<cg::db::FileId, std::unordered_set<std::uint32_t>>
            fb_fresh_union_;

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
            std::unordered_map<std::size_t, std::vector<std::uint32_t>>().swap(
                fb_hint_of_);
            std::vector<cg::analysis::CallSiteRec>().swap(fb_sites_);
            std::unordered_map<std::size_t, std::vector<std::uint32_t>>().swap(
                fb_site_of_);
            std::vector<FlushSymSlot>().swap(fb_syms_);
            std::unordered_map<
                std::string,
                std::uint32_t,
                cg::util::string_hash,
                std::equal_to<>>()
                .swap(fb_sym_of_);
            std::vector<FlushEdgeSlot>().swap(fb_edges_);
            std::unordered_map<std::size_t, std::vector<std::uint32_t>>().swap(
                fb_edge_of_);
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

        detail::SymbolIndex                 sym_index_;
        std::map<std::string, std::int64_t> unresolved_names_;
        std::map<std::string, std::int64_t> unknown_names_;
        std::size_t result_base_ = 0; // 已消费的池结果数（计时用）
        // ===== 头级缓存（graph.headcache）=====
        /** @brief 本运行的缓存实例；空 = 关闭（CCG_HEADCACHE=0 / fb 波式
         *        分解在开 / 内存库外的极端形态按启用判定构造）。 */
        std::unique_ptr<headcache::HeadCache> headcache_;
        /** @brief 归属文件 → 闭包摘要（运行期只读快照，缓存键输入）。 */
        std::unordered_map<std::string, std::string> head_closure_of_;
        /** @brief 主文件路径 → 内容哈希（缓存主文件锚输入；无哈希者
         *         不参与缓存——桥不建、记录跳过）。 */
        std::unordered_map<std::string, std::string> hc_main_hash_of_;
        /** @brief 观察者增量合并（包含差量 add 方向）的站点文件集。 */
        std::unordered_set<cg::db::FileId> bump_feed_;
        /** @brief 指令摘要的写回载荷（提交后落 file_instr_digest）。 */
        std::vector<cg::db::FileInstrDigestRow> c1_digest_writes_;
        /** @brief 支撑 -v 计时行的逐阶段墙钟累计（微秒）。 */
        double t_parse_us_ = 0;
        double t_visit_us_ = 0;
        /** @brief analysis.collect 走查累计（微秒；性能观测面）。 */
        double t_collect_us_ = 0;
        double t_store_a_us_ = 0;
        double t_purge_us_ = 0;
        double t_store_b_us_ = 0;
        double t_commit_us_ = 0;
        /** @brief 可达性清扫段墙钟累计（微秒；快路径跳过时近似为零）。 */
        double t_sweep_us_ = 0;
        /** @brief 贡献记账三段墙钟累计（微秒；旧票读/差集撤票+归零仲
         *         裁/新票落库——跨 storeB 尾与 commit 头的独立成本观测
         *         面，值随 meta 落库）。 */
        double t_contrib_read_us_ = 0;
        double t_contrib_diff_us_ = 0;
        double t_contrib_insert_us_ = 0;
        /** @brief heal 折叠删除 loser 文件时被 CASCADE 清票的符号集
         *         （归零仲裁候选的补集通道；先读后删，与显式撤票同纪
         *         律——删除交给 FK，受影响集交给记账块）。 */
        std::vector<cg::db::SymId> heal_vote_affected_;
    };

    /**
     * @brief 收敛批：不解析任何 TU，只重跑类型传播+dispatch 提交段并撤掉
     *        dispatch_stale 标注。
     *
     * @internal 调用方（`ccg update --drain`）已按租约协议持有声明——延迟
     * 收敛就是又一个 update 批次，claim/心跳协议原样复用；长计算开始前的
     * 续约由调用方的 peek_pending 心跳完成（与 update 路径引擎批一致，本
     * 函数内部无协调心跳）。窗口量化（先前性能核查已确认窗口量级，维持 no-op
     * 心跳 不动）：drain 循环每个批间点都以 lease_ms 续约，引擎批的协调租约下
     * 限 max(update_lease_ms, 2*max(index_commit_ms, 默认 300s)) 默认 300
     * 秒级起，本函数整批 8.5-13.5 秒远在窗口内，批内再挂心跳属重复防御
     * （设计复核同结论）；若未来收敛批时长逼近租约量级，再把
     * dispatch_commit 的 beat 接到 claim
     * 心跳上。符号/文件解析走库查（等价于批内内存映射的 miss
     * 兜底路径）：dispatch 边的 caller/target 键与站点文件路径都来自
     * 库内已提交行。空库（无 call_sites）时与普通批一样自然产出空结果并
     * 清掉 stale 标注。dispatch=false 的库不适用（编辑批也不会延迟）。
     */
    [[nodiscard]]
    inline auto converge_dispatch(
        cg::db::GraphStore& store,
        const IndexOptions& opt) -> IndexSummary
    {
        const auto   t0 = std::chrono::steady_clock::now();
        IndexSummary sum;
        sum.mode = "dispatch-converge";
        sum.roots = opt.roots;
        store.set_bulk_cache(true);
        const struct BulkRestore
        {
            cg::db::GraphStore& store;
            ~BulkRestore()
            {
                try
                {
                    store.set_bulk_cache(false);
                }
                catch (...)
                {
                }
            }
        } bulk{store};
        cg::db::GraphStore::Tx tx{store, /*immediate=*/true};
        std::unordered_map<
            std::string,
            cg::db::FileId,
            cg::util::string_hash,
            std::equal_to<>>
            file_of_map;
        file_of_map.reserve(1024);
        for (const cg::db::FileRow& fr : cg::db::all_files(store))
        {
            file_of_map.emplace(fr.path, fr.id);
        }
        // 增量：脏文件集来自延迟批落盘的 dispatch_stale_files（编辑批
        // 已抹除的文件行不再存在——查不到即跳过，其行已随 FK 级联消失）。
        std::unordered_set<cg::db::FileId> dirty_fids;
        for (const std::string& line :
             detail::read_meta_lines(store, "dispatch_stale_files"))
        {
            if (const auto it = file_of_map.find(line); it != file_of_map.end())
            {
                dirty_fids.insert(it->second);
            }
        }
        const detail::DispatchCommitOut commit_out = detail::dispatch_commit(
            {.store = &store,
             .roots = &opt.roots,
             .sum = &sum,
             .db_path = opt.db_path,
             .owner_of = [&store](std::string_view key)
             { return cg::db::symbol_id_by_key(store, key); },
             .file_of =
                 [&store, &file_of_map](const std::string& path)
             {
                 if (const auto it = file_of_map.find(path);
                     it != file_of_map.end())
                 {
                     return it->second;
                 }
                 return store.upsert_file(path);
             },
             .beat = [] {},
             .dirty_files = &dirty_fids});
        tx.commit();
        // 收敛批不解析任何 TU、不发射任何边：悬解丢弃计数归零，防止编辑批
        // 落下的旧值在收敛后冒充当前态（跨运行对比时此键只描述最后一次
        // 真正跑过阶段 B 的运行）。
        store.set_meta("edges_unresolved_dropped", "0");
        // 先库后快照：提交成功后原子替换（协议见 dispatch_commit 的落盘注
        // 释；写失败只丢加速，下一批 seq 失配自弃）。
        if (!commit_out.snapshot_bytes.empty())
        {
            const auto t_snapw0 = std::chrono::steady_clock::now();
            (void)cg::util::platform::atomic_write_file(
                commit_out.snapshot_path,
                std::string_view{commit_out.snapshot_bytes});
            store.set_meta(
                "analysis_snap_write_us",
                std::to_string(
                    static_cast<std::int64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t_snapw0)
                            .count())));
        }
        const cg::db::Stats st = cg::db::stats(store);
        sum.symbols = st.symbols;
        sum.edges = st.edges;
        sum.includes = st.includes;
        sum.imports = st.imports;
        sum.type_hints = st.type_hints;
        sum.call_sites = st.call_sites;
        for (int i = 0; i < cg::db::edge_type_count; ++i)
        {
            sum.edges_by_type[i] = st.edges_by_type[i];
        }
        if (!opt.db_path.empty() && opt.db_path != ":memory:")
        {
            std::int64_t    bytes = 0;
            std::error_code ec;
            if (const auto size = std::filesystem::file_size(opt.db_path, ec);
                !ec)
            {
                bytes += static_cast<std::int64_t>(size);
            }
            ec.clear();
            if (const auto size =
                    std::filesystem::file_size(opt.db_path + "-wal", ec);
                !ec)
            {
                bytes += static_cast<std::int64_t>(size);
            }
            sum.db_size_bytes = bytes;
        }
        sum.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                .count();
        return sum;
    }

} // namespace cg::graph
