module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.runners.tail
 *
 * @brief 共享索引主干尾部 run_index_tail：命令消费、解析池驱动、汇总与收尾。
 */
export module cg.cli.runners.tail;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.clang;
import cg.graph;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.layout;
import cg.cli.runners.engine;
import cg.cli.runners.route;
import cg.cli.runners.resolve;

export namespace cg::cli
{

    namespace detail
    {

        /**
         * @brief `ccg index` 与 `ccg msbuild` 共享的索引尾部（msbuild
         *        运行器复用此接线而非重复之）。
         *
         * 引擎准备、干跑计划、存储打开/创建（schema 不匹配时 --force 重建）、
         * 索引器运行、关闭后尺寸测量、文本/JSON 摘要。commands 已对 db_file
         * 加载完毕。engine 携带调用方的准备（run_index 在模块路由的基线
         * 旗标之前解析引擎，使合成旗标描述将做解析的引擎）；默认构造的实参——
         * msbuild 运行器的情形——在此对不创建库而窥探到的钉做准备。
         *
         * @internal
         * LLVM 引擎解析（逐库解析世界观，既有口径）先于本尾部
         * 一切驱动器触点：优先级为库的 llvm_path 配置（`ccg config set
         * llvm-path`）> --llvm-root（会话覆盖；仅当库未钉引擎时生效）>
         * $CCG_LLVM_ROOT > PATH 发现。
         * 习惯性的 --llvm-root 不得静默改写刻意钉住的库的世界观，故配置胜出、
         * 落败旗标被宣告。引擎先于驱动器解析加载，使驱动器发现（下方干跑
         * 计划、索引器补充）偏向解析所经由的同一安装——调用方传入其准备时，
         * run_index 的模块路由已在该引擎下跑过。--dry-run 在一切引擎触点之前
         * 短路（不加载 libclang、不解析驱动器、不跑模块自建/影子 PCH 规划）：
         * 只读已有库计算逐条目新鲜度计划，毫秒级返回；缺失/打不开的库用内存
         * 替身（全部 TU 报为新工作），绝不创建、绝不写入。
         * 从打开的存储读权威钉：准备是对存储存在前窥探的钉做的；分歧（文件
         * 被 --force 重建、或被并发重钉）按真钉重新准备。与已记录引擎身份的
         * 路径或版本差强制全量重索引——引擎变化 = 世界观变化，无增量捷径
         * （argv 摘要与 TU 集差量之外的第三根新鲜度支柱）。引擎身份随其产出
         * 的结果一并记录；下一运行恰与这些值比较。先关闭再量尺寸：sqlite
         * 在关闭时把 WAL 折入主文件，故其后量到的才是真实的静置库大小
         * （摘要的关闭前数字数的是未折 WAL 页）。
         */
        inline auto run_index_tail(
            const Invocation&                             inv,
            std::vector<std::string>                      roots,
            const std::string&                            db_file,
            const std::vector<cg::clang::CompileCommand>& commands,
            EnginePrepared                                engine,
            const cg::clang::PchPlanResult*               pch = nullptr)
            -> cg::graph::IndexSummary
        {
            // 跨库并发时"看到一个 ccg.exe 不知道它在更新谁"的失认问题：
            // 启动即亮明目标库（index/msbuild 两条路径都汇到本尾部；stderr
            // 不进注入信封——钩子派生路径的 stderr 一律经
            // capture_exec_timed 的临时文件）。内存库（测试替身）除外。
            if (inv.db_path != ":memory:")
            {
                std::println(
                    std::cerr,
                    "ccg index: target database: {}",
                    inv.db_path);
            }
            // 外来 cl cdb 的 vcvars 重建失败时如实标注来源；自包含 cdb 与
            // 无 cl 条目的工程保持空（零变化门禁）。
            const auto note_system_includes = [&commands](auto& summary)
            {
                if (std::ranges::any_of(
                        commands,
                        [](const cg::clang::CompileCommand& c)
                        { return c.system_includes_auto; }))
                {
                    summary.system_includes = "auto-detected";
                }
            };
            if (inv.dry_run)
            {
                // 真干跑短路：任何引擎调用、驱动器解析与 PCH-BMI 规划之前返回
                // ——不加载 libclang、不跑模块自建扫描、不建影子 PCH（实测这些
                // 在大仓上要几十分钟）。计划 = 已有库的逐条目新鲜度判定（打开
                // 只读，绝不写）；库不存在或打不开（schema 不匹配等——真跑路
                // 径自会重建/报错）时用内存替身，全部 TU 报为新工作。毫秒级，
                // 无引擎环境也能出计划。
                std::optional<cg::db::GraphStore> existing;
                if (inv.db_path != ":memory:"
                    && fs::exists(fs::path{inv.db_path}))
                {
                    try
                    {
                        existing.emplace(inv.db_path);
                    }
                    catch (const std::exception&)
                    {
                        existing.reset();
                    }
                }
                cg::graph::IndexOptions opt;
                opt.roots = std::move(roots);
                opt.progress = inv.verbose;
                opt.compile_db_path = db_file;
                opt.db_path = inv.db_path;
                opt.dry_run = true;
                opt.force = inv.force || inv.full;
                cg::graph::IndexSummary sum = [&]
                {
                    if (existing.has_value())
                    {
                        cg::graph::Indexer planner{*existing, std::move(opt)};
                        return planner.run(commands);
                    }
                    cg::db::GraphStore blank{":memory:"};
                    cg::graph::Indexer planner{blank, std::move(opt)};
                    return planner.run(commands);
                }();
                note_system_includes(sum);
                if (pch != nullptr)
                {
                    sum.pch_built = static_cast<std::int64_t>(pch->built);
                    sum.pch_reused = static_cast<std::int64_t>(pch->reused);
                    sum.pch_fallbacks =
                        static_cast<std::int64_t>(pch->text_fallback);
                    sum.pch_replay_degraded =
                        static_cast<std::int64_t>(pch->replay_degraded);
                    sum.pch_unusable = std::max(
                        sum.pch_unusable,
                        static_cast<std::int64_t>(pch->unusable));
                    sum.pch_notes = pch->tu_notes;
                }
                if (inv.json)
                {
                    std::println("{}", render::index_json(sum));
                }
                else
                {
                    std::print("{}", render::index_text(sum, inv.db_path));
                }
                return sum;
            }
            if (!engine.valid)
            {
                engine = prepare_index_engine(inv, read_db_engine_pin(inv));
            }

            // 装箱进 unique_ptr 以支持收尾处的显式关库（量库文件大小前
            // 关闭连接，wal 合并落盘后主库文件大小才稳定）；GraphStore
            // 删除拷贝移动，不走 make_unique 的完美转发（prvalue 失去
            // 省略资格）——花括号直接初始化保证原地形化。
            std::unique_ptr<cg::db::GraphStore> store{
                new cg::db::GraphStore{open_store_create(inv)}};

            auto [synced_engine, engine_forced_full] =
                sync_engine_with_store(*store, inv, std::move(engine));
            engine = std::move(synced_engine);
            require_driver();

            // ===== 启动接管租约（写命令主动偷取；租约仲裁协议）=====
            // `ccg index` / `ccg msbuild` 是重型权威写者：开写库之后立刻把
            // update_claim 单例行无条件改写为自己（db 层 steal_update——与
            // 既有"过期可偷"同款写，只是触发方变为主动）。常驻增量持有者在
            // 下一个心跳点（批间 peek / 批内事务心跳 / 批后出队）优雅让位，
            // 多写者并存（真库第一杀手 BUSY 与删-插穿插 FOREIGN KEY 的共同
            // 温床）从此按设计消解。RAII 守卫兜底异常路径的释放；租约足够
            // 盖过整场运行（自适应公式），持有期间钩子事件只入队不派生——
            // 队列由本函数收尾的清队循环就地消化。
            std::optional<cg::db::UpdateClaimGuard> takeover_guard;
            std::optional<RunAuditGuard>            run_audit;
            std::string                             takeover_holder;
            std::int64_t                            takeover_lease_ms = 0;
            if (inv.db_path != ":memory:")
            {
                const auto knob = [&](const char* key, std::int64_t fallback)
                { return cg::util::parse_bounded(store->meta(key), fallback); };
                takeover_lease_ms =
                    knob("update_lease_ms", cg::db::default_update_lease_ms);
                const std::int64_t recorded_commit_ms =
                    knob("index_commit_ms", 0);
                takeover_lease_ms = std::max(
                    takeover_lease_ms,
                    2
                        * (recorded_commit_ms > 0
                               ? recorded_commit_ms
                               : cg::db::default_update_lease_ms));
                // 测试注入（生产零开销：无变量即原值）：把接管租约压到秒级，
                // 配 CCG_TEST_PARSE_HOLD_MS 复现"解析段远超租约时长"的现场
                // （真库实测事故的病根形态）。
                takeover_lease_ms = cg::util::parse_bounded(
                    cg::util::cstd::get_env("CCG_TEST_TAKEOVER_LEASE_MS"),
                    takeover_lease_ms);
                // --no-wait 保留快速失败语义：用户明确要求不打扰活持有者时，
                // 宁可失败也不偷（默认路径照偷不误）。
                if (inv.no_wait && store->claim_snapshot().live)
                {
                    throw std::runtime_error(
                        "the database is held by a live update session (--no-wait) "
                        "- re-run later, or `ccg update --drain` to converge the "
                        "queue first");
                }
                takeover_holder = cg::db::update_claim_token();
                const auto busy_error = [](const std::exception& e)
                {
                    const std::string_view msg = e.what();
                    return msg.find("database is locked")
                           != std::string_view::npos;
                };
                // 短探测 + 封顶退避（与提交段既有排队形态同款）：偷取事务的
                // BEGIN IMMEDIATE 会撞上对方在飞批的写锁，busy 耗尽按一次落败
                // 退避重试，--wait 上界内诚实失败。
                store->set_busy_timeout_ms(cg::db::default_coord_probe_busy_ms);
                const auto wait_started = std::chrono::steady_clock::now();
                const auto wait_deadline =
                    wait_started + std::chrono::milliseconds{inv.wait_limit_ms};
                const std::chrono::milliseconds backoff_steps[] = {
                    std::chrono::milliseconds{1000},
                    std::chrono::milliseconds{2000},
                    std::chrono::milliseconds{5000},
                    std::chrono::milliseconds{10000}};
                std::size_t backoff_i = 0;
                std::optional<std::pair<std::string, std::int64_t>> displaced;
                for (;;)
                {
                    try
                    {
                        displaced = store->steal_update(
                            takeover_lease_ms,
                            takeover_holder,
                            std::format(
                                R"({{"takeoverLeaseMs":{},"commitMsBase":{}}})",
                                takeover_lease_ms,
                                recorded_commit_ms));
                        break;
                    }
                    catch (const std::exception& e)
                    {
                        if (!busy_error(e))
                        {
                            throw;
                        }
                    }
                    cg::db::ClaimSnapshot snap;
                    try
                    {
                        snap = store->claim_snapshot();
                    }
                    catch (const std::exception&)
                    {
                        // 快照只是诊断：读不到就报一行骨架继续等。
                    }
                    std::println(
                        std::cerr,
                        "ccg index: waiting to take over the update claim "
                        "(pid {}, queue {} file{})",
                        snap.pid,
                        snap.pending.size(),
                        snap.pending.size() == 1 ? "" : "s");
                    if (std::chrono::steady_clock::now() >= wait_deadline)
                    {
                        throw std::runtime_error(
                            std::format(
                                "still waiting to take over the update claim after {} ms "
                                "(--wait limit) - the concurrent writer holds the lock; "
                                "re-run later",
                                inv.wait_limit_ms));
                    }
                    std::this_thread::sleep_for(
                        backoff_steps[std::min(backoff_i, std::size_t{3})]);
                    if (backoff_i < 3)
                    {
                        ++backoff_i;
                    }
                }
                store->set_busy_timeout_ms(cg::db::default_busy_timeout_ms);
                takeover_guard.emplace(*store, takeover_holder);
                // run-start 放在接管完成之后记：持有者在飞批持有写锁时，
                // 任何同步审计写（30s 默认 busy）都会先把锁熬穿——偷取的
                // 探测退避循环（"waiting to take over"）就再也不会经历
                // 等待（e2e 实测）。ts 口径因此是"接管完成时刻"，等待
                // 接管的耗时不计入 durationMs；argv 摘要取位置参数（守卫
                // 内部拼接并按 UTF-8 码点边界截断）。
                run_audit.emplace(
                    *store,
                    takeover_holder,
                    inv.cmd == Invocation::Cmd::Msbuild ? "msbuild" : "index",
                    inv.db_path,
                    inv.positional);
                // 只在顶替了"当时仍活"的声明时出注记（过期/空行的接管是普通
                // 自愈，不值得打扰）。
                const std::int64_t now_ms = cg::util::cstd::epoch_ms();
                if (displaced.has_value() && displaced->second > now_ms)
                {
                    std::println(
                        std::cerr,
                        "ccg: note: took over the update lease from '{}' "
                        "- its holder yields at the next heartbeat",
                        displaced->first);
                }
            }

            cg::graph::IndexOptions opt;
            // 收尾清队还要用根集（opt 会整体 move 进索引器）。
            const std::vector<std::string> drain_roots = roots;
            opt.roots = std::move(roots);
            opt.force = inv.force || inv.full || engine_forced_full;
            opt.progress = inv.verbose;
            opt.compile_db_path = db_file;
            opt.db_path = inv.db_path;
            opt.jobs = resolve_jobs(inv, &*store);
            opt.dry_run = inv.dry_run;
            opt.paranoid = inv.paranoid;
            // 提交段复用进程级接管的声明（caller-held）：不再排队认领自己，
            // 只在提交事务内心跳续期、被偷时干净让位。`ccg update` 复用本
            // 尾部之外的引擎路径自持协议，不经过这里。
            opt.caller_holder = takeover_holder;
            opt.caller_lease_ms = takeover_lease_ms;
            opt.wait_limit_ms = inv.wait_limit_ms;
            cg::graph::Indexer      indexer{*store, std::move(opt)};
            cg::graph::IndexSummary sum = indexer.run(commands);
            note_system_includes(sum);
            if (pch != nullptr)
            {
                sum.pch_built = static_cast<std::int64_t>(pch->built);
                sum.pch_reused = static_cast<std::int64_t>(pch->reused);
                // text fallbacks 总数不再取规划器的计划口径：索引器按运行口径
                // 恒等聚合（总数 = 回退解析成功 + 回退仍失败），否则计划数与运
                // 行数两个口径会在同一行打架（实测 333 vs 155+450）。
                sum.pch_replay_degraded =
                    static_cast<std::int64_t>(pch->replay_degraded);
                sum.pch_unusable = std::max(
                    sum.pch_unusable,
                    static_cast<std::int64_t>(pch->unusable));
                sum.pch_notes = pch->tu_notes;
            }
            if (!inv.dry_run)
            {
                store->set_meta("llvm_engine_path", engine.path);
                store->set_meta("llvm_version", engine.version);
            }
            if (inv.db_path != ":memory:")
            {
                // ===== 收尾清队（队列租约协议）：索引期间入队的事件就地追轮
                // =====
                // 启动偷取后钩子事件只入队不派生，队列的未来归本进程：主流程
                // 提交完毕、库还开着，就地做增量追轮（与 update 的引擎批完全
                // 同款：种子强制变脏、快速批只落阶段 A）。上限双限（轮数 +
                // 时间预算，库级旋钮 index_drain_rounds / index_drain_budget_ms
                // ），超限诚实退出并把残余交给常规机制（后台 --drain）。输出
                // 三段式：检测提示 + 全量文件列表（不截断——有什么列什么）+
                // 收尾结果行。
                bool         drain_stolen = false;
                std::size_t  drain_leftover = 0;
                std::int64_t drain_rounds = 0;
                if (takeover_guard.has_value())
                {
                    const auto drain_knob = [&](const char*  env_name,
                                                const char*  meta_key,
                                                std::int64_t fallback)
                    {
                        return cg::util::parse_bounded(
                            cg::util::cstd::get_env(env_name),
                            cg::util::parse_bounded(
                                store->meta(meta_key),
                                fallback));
                    };
                    // 轮数上限的显式 0 = 本次运行禁用追轮（测试/脚本形态；
                    // parse_bounded 的"非正回退默认"语义对 0 有意保留给 meta
                    // 键）。
                    const std::int64_t max_rounds = [&]
                    {
                        if (cg::util::cstd::get_env(
                                "CCG_TEST_INDEX_DRAIN_ROUNDS")
                            == std::optional<std::string>{"0"})
                        {
                            return std::int64_t{0};
                        }
                        return drain_knob(
                            "CCG_TEST_INDEX_DRAIN_ROUNDS",
                            "index_drain_rounds",
                            8);
                    }();
                    const std::int64_t budget_ms = drain_knob(
                        "CCG_TEST_INDEX_DRAIN_BUDGET_MS",
                        "index_drain_budget_ms",
                        300'000);
                    const auto fold = [](std::string_view path)
                    { return cg::util::path_compare_key(path); };
                    std::unordered_set<std::string> tracked_fold;
                    for (const cg::clang::CompileCommand& c : commands)
                    {
                        tracked_fold.insert(fold(c.file));
                    }
                    for (const cg::db::FileRow& fr : cg::db::all_files(*store))
                    {
                        tracked_fold.insert(fold(fr.path));
                    }
                    const auto content_hash =
                        [](const std::string& path) -> std::string
                    {
                        try
                        {
                            return cg::util::sha256_file_hex(path);
                        }
                        catch (const std::exception&)
                        {
                            return {};
                        }
                    };
                    const auto drain_started = std::chrono::steady_clock::now();
                    try
                    {
                        for (;;)
                        {
                            if (drain_rounds >= max_rounds
                                || std::chrono::duration_cast<
                                       std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now()
                                       - drain_started)
                                           .count()
                                       >= budget_ms)
                            {
                                break;
                            }
                            const cg::db::PendingPeek peek =
                                store->peek_pending(
                                    takeover_lease_ms,
                                    takeover_holder);
                            if (peek.claim_lost)
                            {
                                drain_stolen = true;
                                break;
                            }
                            if (peek.files.empty())
                            {
                                break;
                            }
                            std::println(
                                std::cerr,
                                "ccg: note: {} file{} changed while indexing - "
                                "re-processing (drain round {}):",
                                peek.files.size(),
                                peek.files.size() == 1 ? "" : "s",
                                drain_rounds + 1);
                            for (const std::string& f : peek.files)
                            {
                                std::println(std::cerr, "  {}", f);
                            }
                            std::vector<std::string> tu_files;
                            std::vector<std::string> project_files;
                            for (const std::string& f : peek.files)
                            {
                                (cg::util::is_project_file(f) ? project_files
                                                              : tu_files)
                                    .push_back(f);
                            }
                            if (!tu_files.empty())
                            {
                                std::unordered_map<std::string, std::string>
                                    snapshot;
                                for (const std::string& f : tu_files)
                                {
                                    if (tracked_fold.contains(fold(f)))
                                    {
                                        snapshot.emplace(f, content_hash(f));
                                    }
                                }
                                if (const std::ptrdiff_t untracked =
                                        std::ranges::count_if(
                                            tu_files,
                                            [&](const std::string& f)
                                            {
                                                return !tracked_fold.contains(
                                                    fold(f));
                                            });
                                    untracked > 0)
                                {
                                    std::println(
                                        std::cerr,
                                        "ccg: note: {} queued file{} not tracked by "
                                        "the database or the compile commands - "
                                        "nothing to refresh for them",
                                        untracked,
                                        untracked == 1 ? "" : "s");
                                }
                                cg::graph::IndexOptions dopt;
                                dopt.roots = drain_roots;
                                dopt.progress = inv.verbose;
                                dopt.compile_db_path = db_file;
                                dopt.db_path = inv.db_path;
                                dopt.jobs = resolve_jobs(inv, &*store);
                                dopt.seed_files = tu_files;
                                dopt.defer_dispatch = true;
                                dopt.caller_holder = takeover_holder;
                                dopt.caller_lease_ms = takeover_lease_ms;
                                dopt.wait_limit_ms = inv.wait_limit_ms;
                                cg::graph::Indexer drainer{
                                    *store,
                                    std::move(dopt)};
                                const cg::graph::IndexSummary dsum =
                                    drainer.run(commands);
                                store->set_meta(
                                    "llvm_engine_path",
                                    engine.path);
                                store->set_meta("llvm_version", engine.version);
                                std::unordered_set<std::string> covered_fold;
                                covered_fold.reserve(dsum.covered_files.size());
                                for (const std::string& path :
                                     dsum.covered_files)
                                {
                                    covered_fold.insert(fold(path));
                                }
                                std::vector<cg::db::PendingClearInput>
                                    clear_inputs;
                                clear_inputs.reserve(tu_files.size());
                                for (const std::string& f : tu_files)
                                {
                                    cg::db::PendingClearInput input;
                                    input.path = f;
                                    const auto snap = snapshot.find(f);
                                    if (snap == snapshot.end())
                                    {
                                        input.tracked = false;
                                    }
                                    else
                                    {
                                        input.tracked = true;
                                        input.snapshot_hash = snap->second;
                                        input.covered =
                                            covered_fold.contains(fold(f));
                                    }
                                    clear_inputs.push_back(std::move(input));
                                }
                                store->clear_covered(
                                    clear_inputs,
                                    content_hash,
                                    takeover_holder,
                                    takeover_lease_ms);
                                ++drain_rounds;
                            }
                            if (!project_files.empty())
                            {
                                // 项目文件行（.sln/.vcxproj/...）改的是语料来源，
                                // 其重采集联动（msbuild 注册表反查）是 update
                                // 的 领地：留给常规机制，如实计入剩余。
                                break;
                            }
                        }
                    }
                    catch (const cg::graph::ClaimStolenError&)
                    {
                        drain_stolen = true;
                    }
                    if (!drain_stolen)
                    {
                        const cg::db::PendingPeek fin = store->peek_pending(
                            takeover_lease_ms,
                            takeover_holder);
                        if (!fin.claim_lost)
                        {
                            drain_leftover = fin.files.size();
                        }
                    }
                    if (drain_stolen)
                    {
                        std::println(
                            std::cerr,
                            "ccg: note: index drain stopped - the claim "
                            "was stolen; the new holder owns the remaining "
                            "events");
                    }
                    else if (drain_leftover > 0)
                    {
                        std::println(
                            std::cerr,
                            "ccg: note: index drain stopped after {} round{} - {} "
                            "event{} still queued (round/time budget or non-TU "
                            "lines); a background `ccg update --drain` will "
                            "converge them",
                            drain_rounds,
                            drain_rounds == 1 ? "" : "s",
                            drain_leftover,
                            drain_leftover == 1 ? "" : "s");
                    }
                    else if (drain_rounds > 0)
                    {
                        std::println(
                            std::cerr,
                            "ccg: note: index drain finished: {} round{}, "
                            "queue empty",
                            drain_rounds,
                            drain_rounds == 1 ? "" : "s");
                    }
                    if (drain_leftover > 0)
                    {
                        // 常规机制兜底：detach 派生、零等待零 kill，后台 update
                        // 自己跑完（stdout/stderr 进 delete-on-close 临时文件，
                        // 与 hook 的 detach 同款平台机制）。
                        const fs::path ccg =
                            cg::util::platform::executable_path();
                        if (!ccg.empty())
                        {
                            std::vector<std::string> drain_args{
                                "update",
                                "--drain",
                                "--json",
                                "-o",
                                inv.layout_root.empty() ? inv.output_dir
                                                        : inv.layout_root,
                                "--config",
                                inv.config_name};
                            (void)cg::util::platform::capture_exec_timed(
                                ccg,
                                drain_args,
                                std::chrono::milliseconds{0});
                        }
                    }
                }
                // run-finish 赶在关库之前显式收尾（带结果数据）；让位收场
                // 用 stolen 点名，被杀/异常由守卫析构与接管者补记兜底。
                if (run_audit.has_value())
                {
                    run_audit->finish(
                        drain_stolen ? "stolen" : "done",
                        /*exit_code=*/0,
                        sum.tus,
                        std::format(
                            "{} mode, {} round{} drained",
                            sum.mode.empty() ? "full" : sum.mode,
                            drain_rounds,
                            drain_rounds == 1 ? "" : "s"));
                    run_audit.reset();
                }
                // 先释放声明再关库：守卫持有连接引用，让它赶在 GraphStore
                // 析构之前完成自己的释放（异常路径由守卫析构兜底）。连接
                // 赶在量库文件大小之前关闭（wal 合并落盘后主库文件大小
                // 才稳定）。
                takeover_guard.reset();
                store.reset();
                std::error_code ec;
                if (const auto size = fs::file_size(fs::path{inv.db_path}, ec);
                    !ec)
                {
                    sum.db_size_bytes = static_cast<std::int64_t>(size);
                }
            }
            if (inv.json)
            {
                std::println("{}", render::index_json(sum));
            }
            else
            {
                std::print("{}", render::index_text(sum, inv.db_path));
            }
            return sum;
        }

    } // namespace detail
} // namespace cg::cli
