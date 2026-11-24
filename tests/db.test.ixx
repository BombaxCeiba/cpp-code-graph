export module cg.test.db;

// cg.db 存储层的单元测试，全部跑在 ":memory:" 上，除非某个用例
// 需要重开语义（schema 版本守卫用临时文件）。
// 覆盖面清单：schema 自举、版本守卫、字符串驻留
// 同一性、幂等符号 upsert 且 id 稳定、边出现次数
// 计数、按文件清除且重抽取 id 稳定、分层查找、
// edges_to 聚合、stats，以及事务提交/回滚。

import std;
import cg.util;
import cg.db;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    using cg::db::EdgeType;
    using cg::db::EdgeWrite;
    using cg::db::FindResult;
    using cg::db::GraphStore;
    using cg::db::IncludeWrite;
    using cg::db::SymbolFilter;
    using cg::db::SymbolKind;
    using cg::db::SymbolWrite;

    // 一个两符号迷你夹具：一个文件里的 caller 调用另一个文件里的 callee。
    struct Mini
    {
        GraphStore     store{":memory:"};
        cg::db::FileId fa = 0;
        cg::db::FileId fb = 0;
        cg::db::SymId  caller = 0;
        cg::db::SymId  callee = 0;

        Mini()
        {
            fa = store.upsert_file("e:/src/a.cpp");
            fb = store.upsert_file("e:/src/b.cpp");
            caller = store.upsert_symbol(
                {.unique_key = "function||caller|e:/src/a.cpp",
                 .kind = SymbolKind::Function,
                 .name = "caller",
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = fa,
                 .line = 1,
                 .col = 5});
            callee = store.upsert_symbol(
                {.unique_key = "function||callee|e:/src/b.cpp",
                 .kind = SymbolKind::Function,
                 .name = "callee",
                 .ns = "shape",
                 .sig_hash = 7,
                 .file_id = fb,
                 .line = 2,
                 .col = 6,
                 .parent_id = std::nullopt});
        }
    };

    auto
    edge(cg::db::SymId src, cg::db::SymId dst, cg::db::FileId site, int line)
        -> EdgeWrite
    {
        return {
            .src = src,
            .dst = dst,
            .type = EdgeType::Calls,
            .site_file = site,
            .site_line = line};
    }

    /**
     * @brief 删除文件库主文件及其 -wal/-shm 边车（文件不存在时静默）。
     *
     * @internal 上一次崩溃运行的残留不得毒化本轮（WAL 边车还带着旧
     * 事务）；文件库用例在开头清残留、结尾清本轮各调一次。
     */
    void wipe_db(const std::string& p)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove(fs::path{p}, ec);
        fs::remove(p + "-wal", ec);
        fs::remove(p + "-shm", ec);
    }

    void schema_bootstrap(R& r)
    {
        GraphStore store{":memory:"};
        cg::test::check(
            r,
            store.meta("schema_version").value_or("")
                == std::to_string(cg::db::schema_version),
            "fresh db seeds the current schema_version");
        cg::test::check(
            r,
            store.meta("app_version").value_or("")
                == std::string{cg::util::version},
            "fresh db seeds app_version from cg.util");
        const auto st = cg::db::stats(store);
        cg::test::check(
            r,
            st.files == 0 && st.symbols == 0 && st.edges == 0
                && st.includes == 0 && st.strings == 0,
            "fresh db has empty counters");
        // 同一个 :memory: 库第二次打开仍保留版本行。
        cg::test::check(
            r,
            store.meta("schema_version").has_value(),
            "meta readable after bootstrap");
    }

    // 实时协调语义（排队租约 + 事务内心跳 + 偷取仲裁）的单元级验证：FILE
    // 数据库上用两个 连接分别扮演两个 `ccg update` 进程：claim 独占、enqueue
    // 去重 + 跨连接可见、take 时的 lease 续租、持有者范围的 release、lease
    // 过期抢走（崩溃自愈），以及 claim 被抢后再 take 触发 claim_lost。
    //
    void realtime_coordination(R& r)
    {
        namespace fs = std::filesystem;
        const auto        path = fs::path{"ccg-db-coordination-test.db"};
        const std::string p{path.string()};
        wipe_db(p);

        const std::string a_path = "e:/coord/a.cpp";
        const std::string b_path = "e:/coord/b.cpp";
        const std::string c_path = "e:/coord/c.cpp";
        {
            GraphStore creator{p}; // 自举，然后两个'进程'再打开
        }
        GraphStore holder{p};
        GraphStore other{p};
        // 两个'进程'都像 run_update 那样在任何协调事务之前
        // 设置好 busy timeout（实时协调协议的等锁纪律）。
        holder.set_busy_timeout_ms(60'000);
        other.set_busy_timeout_ms(60'000);

        // CLAIM：lease 存活期间独占；输家能看到
        // 持有者的身份。
        cg::test::check(
            r,
            holder.claim_update(60'000, "H1"),
            "claim succeeds on an unclaimed db");
        cg::test::check(
            r,
            !other.claim_update(60'000, "O1"),
            "a live claim rejects the second claimant");
        {
            const auto row = other.update_claim_holder();
            cg::test::check(
                r,
                row.has_value() && row->first == "H1",
                "the claim row names the live holder");
        }

        // ENQUEUE：同一路径两次只有一行（PRIMARY KEY 去重），跨连接
        // 可见；PEEK 返回队列但不消费它。
        holder.enqueue_pending(
            std::vector<std::string>{a_path, a_path, b_path});
        other.enqueue_pending(
            std::vector<std::string>{b_path}); // '另一'进程的并发重复
        const cg::db::PendingPeek peek1 = holder.peek_pending(60'000, "H1");
        cg::test::check(
            r,
            !peek1.claim_lost && peek1.files.size() == 2
                && peek1.files[0] == a_path && peek1.files[1] == b_path,
            "enqueue dedupes by path and peek returns the whole "
            "queue");
        // peek 是读操作：同样的行会反复返回，直到 clear_covered 判定
        // 它们已处理完。
        const cg::db::PendingPeek peek_repeat =
            holder.peek_pending(60'000, "H1");
        cg::test::check(
            r,
            !peek_repeat.claim_lost && peek_repeat.files.size() == 2,
            "peek does not consume the queue");
        // 行出队前，索引记录的哈希必须与决策哈希一致
        // （edit→revert 残留守卫）。
        holder.set_file_hash(holder.upsert_file(a_path), "h1");
        holder.set_file_hash(holder.upsert_file(b_path), "h1");
        holder.set_file_hash(holder.upsert_file(c_path), "h1");
        const auto stable_hash = [](const std::string&)
        { return std::string{"h1"}; };
        const std::vector<cg::db::PendingClearInput> consumed{
            {.path = a_path,
             .snapshot_hash = "h1",
             .tracked = true,
             .covered = true},
            {.path = b_path,
             .snapshot_hash = "h1",
             .tracked = true,
             .covered = true}};
        cg::test::check(
            r,
            holder.clear_covered(consumed, stable_hash, "H1", 60'000) == 2,
            "clear_covered deletes covered entries whose hash is "
            "unchanged");
        const cg::db::PendingPeek peek2 = holder.peek_pending(60'000, "H1");
        cg::test::check(
            r,
            !peek2.claim_lost && peek2.files.empty(),
            "a peek after clear_covered comes back empty");

        // PEEK 为 lease 续租心跳：另一连接期间入队的行会被拾起，
        // 且 claim 保持独占存活。
        other.enqueue_pending(std::vector<std::string>{c_path});
        const cg::db::PendingPeek peek3 = holder.peek_pending(60'000, "H1");
        cg::test::check(
            r,
            !peek3.claim_lost && peek3.files.size() == 1,
            "peek picks up rows enqueued by another connection");
        cg::test::check(
            r,
            !other.claim_update(60'000, "O1"),
            "the peek's lease heartbeat keeps the claim live");
        holder.clear_covered(
            std::vector<cg::db::PendingClearInput>{
                {.path = c_path,
                 .snapshot_hash = "h1",
                 .tracked = true,
                 .covered = true}},
            stable_hash,
            "H1",
            60'000);

        // RELEASE 限定持有者：错误持有者的 release 是空操作。
        cg::test::check(
            r,
            !other.release_update("O1"),
            "release by a non-holder changes nothing");
        cg::test::check(
            r,
            holder.release_update("H1"),
            "the holder's release drops the claim");
        cg::test::check(
            r,
            !other.update_claim_holder().has_value(),
            "no claim row remains after release");
        cg::test::check(
            r,
            other.claim_update(60'000, "O1"),
            "the claim is takeable again after release");
        cg::test::check(
            r,
            other.release_update("O1"),
            "release the second holder for the steal phase");

        // LEASE 过期 = 崩溃自愈：零时长 lease 在授予时即过期，
        // 下一个更新者直接抢走——无需锁文件、无需 PID 检查。
        cg::test::check(
            r,
            holder.claim_update(0, "H2"),
            "a zero-lease claim is granted (and instantly stale)");
        cg::test::check(
            r,
            other.claim_update(60'000, "O2"),
            "an expired lease is stolen by the next updater");
        {
            const auto row = holder.update_claim_holder();
            cg::test::check(
                r,
                row.has_value() && row->first == "O2",
                "the steal rewrote the holder identity");
        }

        // CLAIM LOST：过期持有者的 peek 拒绝读抢夺者的
        // 队列（其心跳匹配到零行）。
        other.enqueue_pending(std::vector<std::string>{c_path});
        const cg::db::PendingPeek stolen = holder.peek_pending(60'000, "H2");
        cg::test::check(
            r,
            stolen.claim_lost && stolen.files.empty(),
            "a peek under a stolen claim reports claim_lost and "
            "reads nothing");
        const cg::db::PendingPeek owner = other.peek_pending(60'000, "O2");
        cg::test::check(
            r,
            !owner.claim_lost && owner.files.size() == 1,
            "the thief owns the queue the stale holder left");
        other.clear_covered(
            std::vector<cg::db::PendingClearInput>{
                {.path = c_path,
                 .snapshot_hash = "h1",
                 .tracked = true,
                 .covered = true}},
            stable_hash,
            "O2",
            60'000);
        cg::test::check(r, other.release_update("O2"), "final cleanup release");
        cg::test::check(
            r,
            other.peek_pending(60'000, "O2").claim_lost,
            "peeking after releasing reports claim_lost (no claim "
            "to heartbeat)");

        // 孤儿 LEASE = 持有者进程死在守卫释放该行之前。
        // token 内嵌 pid：pid 存活则 lease 保持权威，
        // pid 已死则下一个更新者立即接管，哪怕
        // lease_until 还远在未来。
        const std::string live_token = std::format(
            "ccg-update:pid:{}:t1",
            cg::util::platform::current_pid());
        cg::test::check(
            r,
            holder.claim_update(60'000, live_token),
            "a claim whose token names a live pid is granted");
        cg::test::check(
            r,
            !other.claim_update(60'000, "probe"),
            "a live pid keeps the claim exclusive");
        cg::test::check(
            r,
            holder.release_update(live_token),
            "release the live-pid claim for the orphan phase");
        const std::string dead_token = "ccg-update:pid:4000000000:t1";
        cg::test::check(
            r,
            holder.claim_update(600'000, dead_token),
            "a claim with a dead holder pid is granted (stale on "
            "arrival)");
        cg::test::check(
            r,
            other.claim_update(60'000, "O3"),
            "a live lease whose holder pid is dead is taken over "
            "immediately");
        {
            const auto row = holder.update_claim_holder();
            cg::test::check(
                r,
                row.has_value() && row->first == "O3",
                "the orphan takeover rewrote the holder identity");
        }
        cg::test::check(
            r,
            other.release_update("O3"),
            "final cleanup after the orphan takeover");

        wipe_db(p);
    }

    // 主动偷取（写命令接管租约）：steal_update 连活声明也照改写，
    // 返回被顶替的旧行供诊断；让位方随后的心跳/释放全部扑空，
    // 绝不碰偷取者的行。
    void update_steal_active(R& r)
    {
        namespace fs = std::filesystem;
        const auto        path = fs::path{"ccg-db-steal-test.db"};
        const std::string p = path.generic_string();
        wipe_db(p);
        {
            GraphStore creator{p};
        }
        GraphStore holder{p};
        GraphStore writer{p};
        holder.set_busy_timeout_ms(60'000);
        writer.set_busy_timeout_ms(60'000);

        // 空库上的偷取：无旧行可报，声明直接落笔。
        const auto none = writer.steal_update(60'000, "W1");
        cg::test::check(
            r,
            !none.has_value(),
            "stealing from an unclaimed db reports no displaced row");
        {
            const auto row = writer.update_claim_holder();
            cg::test::check(
                r,
                row.has_value() && row->first == "W1",
                "the steal wrote the writer's holder identity");
        }
        cg::test::check(
            r,
            writer.release_update("W1"),
            "release the stolen claim for the live-steal phase");

        // 活声明也照偷（与 claim_update 的关键差别）：返回旧行身份，
        // 被偷者的 peek 心跳立刻 claim_lost，释放匹配零行。
        cg::test::check(
            r,
            holder.claim_update(600'000, "LIVE"),
            "the live holder claims first");
        holder.enqueue_pending(std::vector<std::string>{"e:/steal/a.cpp"});
        const auto displaced = writer.steal_update(60'000, "W2");
        cg::test::check(
            r,
            displaced.has_value() && displaced->first == "LIVE"
                && displaced->second > 0,
            "stealing a live claim reports the displaced holder");
        {
            const auto row = holder.update_claim_holder();
            cg::test::check(
                r,
                row.has_value() && row->first == "W2",
                "the live claim was rewritten to the writer");
        }
        const cg::db::PendingPeek stolen_peek =
            holder.peek_pending(60'000, "LIVE");
        cg::test::check(
            r,
            stolen_peek.claim_lost && stolen_peek.files.empty(),
            "the displaced holder's heartbeat reports claim_lost");
        cg::test::check(
            r,
            !holder.release_update("LIVE"),
            "the displaced holder cannot release the thief's row");
        // 队列的未来归偷取者：它照常 peek 与释放。
        const cg::db::PendingPeek owner_peek =
            writer.peek_pending(60'000, "W2");
        cg::test::check(
            r,
            !owner_peek.claim_lost && owner_peek.files.size() == 1,
            "the thief owns the queue the holder left behind");
        cg::test::check(
            r,
            writer.release_update("W2"),
            "the thief releases normally");

        wipe_db(p);
    }

    // coord_events 协调事件审计（协调审计设计的落地）：
    // 写入/读回、steal 与偷取同事务、expire/pid-dead 的接管审计、独立
    // 短事务的 rollback 审计在外层业务回滚后幸存、老化（时间窗 + 行数
    // 封顶）。
    void coord_events_audit(R& r)
    {
        namespace fs = std::filesystem;
        const auto        path = fs::path{"ccg-db-coord-test.db"};
        const std::string p = path.generic_string();
        wipe_db(p);
        {
            GraphStore creator{p};
        }
        GraphStore store{p};
        store.set_busy_timeout_ms(60'000);

        // 写入/读回 roundtrip（enqueue-reject：钩子拒收入队的审计面）。
        cg::test::check(
            r,
            store.log_coord_event(
                cg::db::CoordEventInput{
                    .event = cg::db::CoordEvent::EnqueueReject,
                    .actor_holder = "ccg-hook:pid:4242",
                    .reason = "not-in-corpus",
                    .detail = R"({"config":"main","file":"e:/x.cpp"})"}),
            "log_coord_event writes an enqueue-reject row");
        {
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 1 && rows.front().event == "enqueue-reject"
                    && rows.front().actor_holder == "ccg-hook:pid:4242"
                    && rows.front().reason == "not-in-corpus"
                    && rows.front().ts_ms > 0,
                "coord_events_since reads the row back with its fields");
            const auto counts = store.coord_event_counts_since(0);
            cg::test::check(
                r,
                counts.size() == 1 && counts.front().first == "enqueue-reject"
                    && counts.front().second == 1,
                "coord_event_counts_since groups by event name");
        }

        // steal 与偷取同事务：活声明被顶替时一行 steal（reason/对方记号）。
        cg::test::check(
            r,
            store.claim_update(600'000, "LIVE"),
            "the live holder claims for the steal audit");
        const auto displaced = store.steal_update(
            60'000,
            "THIEF",
            R"({"takeoverLeaseMs":60000,"commitMsBase":421263})");
        cg::test::check(
            r,
            displaced.has_value() && displaced->first == "LIVE",
            "the steal displaces the live holder");
        {
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 2 && rows.back().event == "steal"
                    && rows.back().actor_holder == "THIEF"
                    && rows.back().other_holder == "LIVE"
                    && rows.back().reason == "index-takeover",
                "stealing a live claim logs a steal row in the same "
                "transaction");
        }
        // 空行偷取（普通自愈）不写 steal。
        cg::test::check(
            r,
            store.release_update("THIEF"),
            "release the thief for the no-row steal phase");
        (void)store.steal_update(60'000, "W-EMPTY");
        {
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 2,
                "stealing an absent claim writes no steal row");
        }

        // 过期接管：lease_until 落在过去，接管写 orphan-recover
        // （reason=lease-expired）。
        cg::test::check(
            r,
            store.release_update("W-EMPTY"),
            "release before the expire phase");
        (void)store.claim_update(-600'000, "OLD");
        cg::test::check(
            r,
            store.claim_update(60'000, "NEW"),
            "claiming over an expired lease succeeds");
        {
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 3 && rows.back().event == "orphan-recover"
                    && rows.back().other_holder == "OLD"
                    && rows.back().reason == "lease-expired",
                "taking over an expired lease logs orphan-recover/lease-"
                "expired");
        }

        // pid 探活判死的接管：伪造死 pid 记号 + 远期租约（e2e 同款注入
        // 形态），接管写 orphan-recover（reason=pid-dead）。
        store.exec(
            "update update_claim set holder = "
            "'ccg-update:pid:4000000000:t1', lease_until = 99999999999999 "
            "where id = 1");
        cg::test::check(
            r,
            store.claim_update(60'000, "NEW2"),
            "claiming over a dead-pid lease succeeds");
        {
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 4 && rows.back().reason == "pid-dead"
                    && rows.back().other_pid == 4'000'000'000,
                "taking over a dead-pid lease logs orphan-recover/pid-dead "
                "with the dead pid");
        }

        // 回滚不丢审计：外层业务事务回滚后，独立短事务的 rollback 审计
        // 仍然落盘（detach 子进程 stderr 蒸发时库内证据幸存的机制本身）。
        {
            GraphStore::Tx doomed{store, /*immediate=*/true};
            store.exec(
                "insert into pending_updates(path) values('e:/gone.cpp')");
            doomed.rollback();
        }
        cg::test::check(
            r,
            store.log_coord_event(
                cg::db::CoordEventInput{
                    .event = cg::db::CoordEvent::Rollback,
                    .actor_holder = "YIELDER",
                    .reason = "mid-batch-heartbeat",
                    .detail = R"({"batchTu":3,"queue":5})"}),
            "the rollback audit write after the rolled-back business tx");
        {
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 5 && rows.back().event == "rollback"
                    && rows.back().actor_holder == "YIELDER",
                "the rollback row survives the unrelated rolled-back "
                "transaction");
        }

        // 被杀运行的补记：悬挂 run-start（死 pid，无配对 finish）由下一
        // 个接管者在 pid 探活判死后补记 run-finish/abandoned；补记使悬挂
        // 判定自愈——再接管不再重复补。
        store.log_coord_event(
            cg::db::CoordEventInput{
                .event = cg::db::CoordEvent::RunStart,
                .actor_holder = "ccg-update:pid:4000000000:t5",
                .actor_pid = 4'000'000'000,
                .reason = "update",
                .detail = R"({"db":"e:/x/ccg.db","role":"update"})"});
        store.exec(
            "update update_claim set holder = "
            "'ccg-update:pid:4000000000:t5', lease_until = 99999999999999 "
            "where id = 1");
        cg::test::check(
            r,
            store.claim_update(60'000, "NEW3"),
            "claiming over the dangling run's dead pid succeeds");
        {
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 8 && rows.back().event == "run-finish"
                    && rows.back().reason == "abandoned"
                    && rows.back().actor_pid == 4'000'000'000
                    && rows.back().other_holder == "NEW3",
                "the dangling run-start gets an abandoned run-finish from "
                "the discoverer");
        }
        store.exec(
            "update update_claim set holder = "
            "'ccg-update:pid:4000000000:t5', lease_until = 99999999999999 "
            "where id = 1");
        cg::test::check(
            r,
            store.claim_update(60'000, "NEW4"),
            "a second takeover over the same dead pid succeeds");
        {
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 9
                    && std::ranges::count_if(
                           rows,
                           [](const cg::db::CoordEventRow& row)
                           { return row.reason == "abandoned"; })
                           == 1,
                "the healed dangling check does not double-abandon");
        }
        cg::test::check(
            r,
            store.release_update("NEW4"),
            "release before the aging phase");

        // 老化：时间窗删除（远古行清掉、新鲜行保留）+ 行数封顶（新者
        // 保留）。远古行经 exec 直插（写入路径总是盖当前时间戳）。
        store.exec(
            "insert into coord_events(ts_ms,event) values(1000,'steal')");
        const std::size_t aged = store.coord_events_age(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::hours{24})
                .count(),
            10'000);
        {
            const auto rows = store.coord_events_since(0);
            const bool ancient_gone = std::ranges::find(
                                          rows,
                                          std::int64_t{1000},
                                          &cg::db::CoordEventRow::ts_ms)
                                      == rows.end();
            cg::test::check(
                r,
                aged == 1 && ancient_gone && rows.size() == 9,
                "coord_events_age drops rows older than the keep window");
        }
        for (int i = 0; i < 8; ++i)
        {
            store.exec(
                std::format(
                    "insert into coord_events(ts_ms,event) values({},'steal')",
                    2'000'000'000'000 + i));
        }
        (void)store.coord_events_age(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::hours{24})
                .count(),
            10);
        {
            // 17 行压到 10：最老的 7 行被裁，ts 升序的首行因此是最新的
            // 业务行（abandoned 补记）。
            const auto rows = store.coord_events_since(0);
            cg::test::check(
                r,
                rows.size() == 10 && rows.front().reason == "abandoned",
                "coord_events_age caps total rows keeping the newest");
        }

        wipe_db(p);
    }

    // 解析期租约心跳（真库实测事故的修复）：renew_update_lease 在自足
    // 短事务内续自己的租约——只动自己那行，异名记号零行（被偷语义），
    // 续租时长如实改写（与 claim/steal 同语义，不取 max）。
    void update_renew_lease(R& r)
    {
        namespace fs = std::filesystem;
        const auto        path = fs::path{"ccg-db-renew-test.db"};
        const std::string p = path.generic_string();
        wipe_db(p);
        {
            GraphStore creator{p};
        }
        GraphStore store{p};
        store.set_busy_timeout_ms(60'000);

        // 未声明时续租=匹配零行：绝不落笔新行（被偷语义，不是认领）。
        cg::test::check(
            r,
            !store.renew_update_lease("K1", 60'000),
            "renewing an unclaimed db reports stolen, never writes");
        cg::test::check(
            r,
            !store.update_claim_holder().has_value(),
            "the failed renew left no claim row behind");

        // 自有声明续租成功，lease_until 按本次时长如实前移。
        cg::test::check(
            r,
            store.claim_update(600'000, "K1"),
            "the holder claims first");
        const std::int64_t before = store.update_claim_holder()->second;
        cg::test::check(
            r,
            store.renew_update_lease("K1", 600'000),
            "the holder renews its own lease");
        const std::int64_t after = store.update_claim_holder()->second;
        cg::test::check(
            r,
            after >= before,
            "the renewal moved lease_until forward");
        // 更短的续租时长如实改写（心跳语义=覆盖，不取 max）：调用方全权
        // 决定时长，库侧绝不自作主张放大。
        cg::test::check(
            r,
            store.renew_update_lease("K1", 1'000),
            "a shorter renew also succeeds");
        const std::int64_t shortened = store.update_claim_holder()->second;
        cg::test::check(
            r,
            shortened < after,
            "the renewal writes the given duration verbatim");

        // 被偷之后：原持有者续租零行，偷取者照常续。
        GraphStore thief{p};
        thief.set_busy_timeout_ms(60'000);
        cg::test::check(
            r,
            thief.steal_update(60'000, "K2").has_value(),
            "the thief steals the live claim");
        cg::test::check(
            r,
            !store.renew_update_lease("K1", 60'000),
            "the displaced holder's renew reports stolen");
        cg::test::check(
            r,
            thief.renew_update_lease("K2", 60'000),
            "the thief renews normally");
        cg::test::check(
            r,
            thief.release_update("K2"),
            "the thief releases normally");

        wipe_db(p);
    }

    // 批尾出队策略（实时变更队列协调协议）：未 peek 的行留在队列里；
    // clear_covered 只清除两类条目：未被跟踪的，或被覆盖且
    // 内容哈希未变的。批运行期间被编辑的文件保留
    // 其标记（哈希不同）；批没碰到的文件同样保留。
    void pending_clear_covered(R& r)
    {
        GraphStore store{":memory:"};
        store.set_busy_timeout_ms(60'000);
        cg::test::check(
            r,
            store.claim_update(60'000, "C1"),
            "claim for the clear_covered case");
        const std::string unchanged = "e:/batch/unchanged.cpp";
        const std::string edited = "e:/batch/edited.cpp";
        const std::string uncovered = "e:/batch/uncovered.hpp";
        const std::string untracked = "e:/batch/untracked.txt";
        store.enqueue_pending(
            std::vector<std::string>{unchanged, edited, uncovered, untracked});
        const cg::db::PendingPeek peek = store.peek_pending(60'000, "C1");
        cg::test::check(
            r,
            !peek.claim_lost && peek.files.size() == 4,
            "all four entries are queued before the batch");

        // files.content_hash 是索引自己声明的它解析过的字节。
        // 行出队前必须与决策哈希一致
        // （见下方 edit→revert 用例）；按此播种。
        const cg::db::FileId unchanged_id = store.upsert_file(unchanged);
        store.set_file_hash(unchanged_id, "aa");
        const cg::db::FileId edited_id = store.upsert_file(edited);
        store.set_file_hash(edited_id, "bb");
        const cg::db::FileId uncovered_id = store.upsert_file(uncovered);
        store.set_file_hash(uncovered_id, "cc");

        // 当前哈希在 clear_covered 的 IMMEDIATE
        // 事务里重算；这个映射代表磁盘上的字节。
        const auto hashes = [&](const std::string& path) -> std::string
        {
            if (path == unchanged)
            {
                return "aa";
            }
            if (path == edited)
            {
                return "bb";
            }
            if (path == uncovered)
            {
                return "cc";
            }
            return "";
        };
        const std::vector<cg::db::PendingClearInput> decisions{
            // 被覆盖且未变的文件 -> 清除
            {.path = unchanged,
             .snapshot_hash = "aa",
             .tracked = true,
             .covered = true},
            // 批运行期间被编辑（哈希变了）-> 保留
            {.path = edited,
             .snapshot_hash = "aa",
             .tracked = true,
             .covered = true},
            // 本批从未触碰 -> 保留
            {.path = uncovered,
             .snapshot_hash = "aa",
             .tracked = true,
             .covered = false},
            // 不在库/编译数据库里 -> 清除（永远无物可刷新）
            {.path = untracked,
             .snapshot_hash = "",
             .tracked = false,
             .covered = false}};
        cg::test::check(
            r,
            store.clear_covered(decisions, hashes, "C1", 60'000) == 2,
            "clear_covered removes only the unchanged covered and "
            "untracked entries");
        const cg::db::PendingPeek rest = store.peek_pending(60'000, "C1");
        cg::test::check(
            r,
            !rest.claim_lost && rest.files.size() == 2
                && rest.files[0] == edited && rest.files[1] == uncovered,
            "batch-edited and uncovered entries keep their dirty "
            "markers");
        cg::test::check(
            r,
            store.clear_covered(
                std::vector<cg::db::PendingClearInput>{},
                hashes,
                "C1",
                60'000)
                == 0,
            "clear_covered on an empty batch is a no-op");

        // TOCTOU 窗口：编辑落在决策哈希之后、DELETE 之前
        // 的修改曾经会丢失（它并发的入队被同一条
        // 语句删掉）。删除后的校验必须察觉变化的字节
        // 并立即把路径重新入队。
        const std::string racing = "e:/batch/racing.cpp";
        store.enqueue_pending(std::vector<std::string>{racing});
        const cg::db::FileId racing_id = store.upsert_file(racing);
        store.set_file_hash(racing_id, "aa");
        std::size_t racing_reads = 0;
        const auto  racing_hash = [&racing_reads](const std::string&)
        {
            // 第一次调用 = 决策读取（与快照一致），第二次调用 =
            // 删除后校验（并发编辑已落地）。
            return racing_reads++ == 0 ? std::string{"aa"} : std::string{"bb"};
        };
        cg::test::check(
            r,
            store.clear_covered(
                std::vector<cg::db::PendingClearInput>{
                    {.path = racing,
                     .snapshot_hash = "aa",
                     .tracked = true,
                     .covered = true}},
                racing_hash,
                "C1",
                60'000)
                == 1,
            "the racing entry is deleted before the post-delete recheck");
        const cg::db::PendingPeek raced = store.peek_pending(60'000, "C1");
        cg::test::check(
            r,
            !raced.claim_lost && raced.files.size() == 3
                && raced.files[1] == racing,
            "a post-delete hash change re-enqueues the path");

        // edit→revert 残留：字节回到了批前状态，
        // 快照与决策哈希一致，但引擎索引进去的是
        // 中间态字节（files.content_hash = "h2"）。删除该行
        // 会留下一个不描述磁盘上文件的索引。
        const std::string reverted = "e:/batch/reverted.cpp";
        store.enqueue_pending(std::vector<std::string>{reverted});
        const cg::db::FileId reverted_id = store.upsert_file(reverted);
        store.set_file_hash(reverted_id, "h2");
        const auto reverted_hash = [](const std::string&)
        { return std::string{"h"}; };
        cg::test::check(
            r,
            store.clear_covered(
                std::vector<cg::db::PendingClearInput>{
                    {.path = reverted,
                     .snapshot_hash = "h",
                     .tracked = true,
                     .covered = true}},
                reverted_hash,
                "C1",
                60'000)
                == 0,
            "an edit reverted to the batch snapshot keeps its queue row while "
            "the index hash disagrees");
        cg::test::check(
            r,
            store.peek_pending(60'000, "C1").files.size() == 4,
            "the reverted entry is still queued");

        // 被抢的 claim 不能让老持有者清掉新持有者的
        // 队列：clear_covered 内的心跳匹配到零行。
        const auto empty_clear = store.clear_covered(
            std::vector<cg::db::PendingClearInput>{
                {.path = racing,
                 .snapshot_hash = "bb",
                 .tracked = true,
                 .covered = true}},
            hashes,
            "not-the-holder",
            60'000);
        cg::test::check(
            r,
            empty_clear == 0,
            "a stolen claim refuses to dequeue");
        cg::test::check(
            r,
            store.peek_pending(60'000, "C1").files.size() == 4,
            "the refused dequeue leaves the queue intact");

        const auto final_hashes = [&](const std::string& path) -> std::string
        {
            if (path == racing)
            {
                return "bb";
            }
            if (path == reverted)
            {
                return "h";
            }
            return hashes(path);
        };
        // 下一批正确完成了竞态重索引与回退：存储的
        // 哈希现在与决策哈希一致。
        store.set_file_hash(racing_id, "bb");
        store.set_file_hash(reverted_id, "h");
        store.clear_covered(
            std::vector<cg::db::PendingClearInput>{
                {.path = edited,
                 .snapshot_hash = "bb",
                 .tracked = true,
                 .covered = true},
                {.path = uncovered,
                 .snapshot_hash = "cc",
                 .tracked = true,
                 .covered = true},
                {.path = racing,
                 .snapshot_hash = "bb",
                 .tracked = true,
                 .covered = true},
                {.path = reverted,
                 .snapshot_hash = "h",
                 .tracked = true,
                 .covered = true}},
            final_hashes,
            "C1",
            60'000);
        cg::test::check(
            r,
            store.peek_pending(60'000, "C1").files.empty(),
            "a second batch clears the retained markers");
        cg::test::check(
            r,
            store.release_update("C1"),
            "release the clear_covered claim");
    }

    void schema_version_guard(R& r)
    {
        namespace fs = std::filesystem;
        // 相对路径：ctest 从构建树运行本可执行文件，所以临时
        // 库（及其 WAL 边车文件）永不离开工程目录。
        const auto        path = fs::path{"ccg-db-guard-test.db"};
        const std::string p{path.string()};
        wipe_db(p); // 旧运行残留不得遮蔽本测试

        {
            GraphStore store{p};
            cg::test::check(
                r,
                store.meta("schema_version").value_or("")
                    == std::to_string(cg::db::schema_version),
                "file db bootstraps like :memory:");
            store.set_meta(
                "schema_version",
                "99"); // tamper, as an old db would
        }
        bool threw = false;
        try
        {
            GraphStore store{p};
        }
        catch (const std::runtime_error& e)
        {
            threw = true;
            const std::string_view msg = e.what();
            cg::test::check(
                r,
                msg.find("schema version mismatch") != std::string_view::npos,
                "mismatch error names the problem");
            cg::test::check(
                r,
                msg.find("--force") != std::string_view::npos,
                "mismatch error advises --force");
        }
        cg::test::check(r, threw, "tampered schema_version refuses to open");

        // 旧版本戳同样拒绝：未发布期间不维护跨版本迁移，任何不符都要求
        // 用 --force 重建（而非在打开时逐级升级）。
        wipe_db(p);
        {
            GraphStore store{p};
            store.set_meta(
                "schema_version",
                std::to_string(cg::db::schema_version - 1));
        }
        threw = false;
        try
        {
            GraphStore store{p};
        }
        catch (const std::runtime_error& e)
        {
            threw = true;
            const std::string_view msg = e.what();
            cg::test::check(
                r,
                msg.find("schema version mismatch") != std::string_view::npos,
                "older-version error names the problem");
            cg::test::check(
                r,
                msg.find("--force") != std::string_view::npos,
                "older-version error advises --force");
        }
        cg::test::check(r, threw, "older schema_version refuses to open");

        // 清理（WAL 边车文件通常在 close 后已消失；忽略错误）
        wipe_db(p);
    }

    void interning_identity(R& r)
    {
        GraphStore store{":memory:"};
        const auto a1 = store.intern("alpha");
        const auto a2 = store.intern("alpha");
        const auto b = store.intern("beta");
        cg::test::check(r, a1 == a2, "same string interns to the same id");
        cg::test::check(r, a1 != b, "distinct strings get distinct ids");
        cg::test::check(
            r,
            store.find_str("alpha").value_or(-1) == a1,
            "find_str resolves an interned string");
        cg::test::check(
            r,
            !store.find_str("omega").has_value(),
            "find_str misses unknown strings");
        cg::test::check(
            r,
            store.str_of(a1) == "alpha" && store.str_of(b) == "beta",
            "str_of round-trips ids to text");
        const auto g = store.intern("gamma");
        const auto g2 = store.intern("gamma");
        cg::test::check(r, g == g2, "later interns stay stable");
        const auto st = cg::db::stats(store);
        cg::test::check(
            r,
            st.strings == 3,
            "str table gained exactly one row per unique string");
    }

    void file_upsert_and_status(R& r)
    {
        GraphStore store{":memory:"};
        const auto f1 = store.upsert_file("e:/src/a.cpp");
        const auto f2 = store.upsert_file("e:/src/a.cpp");
        const auto other = store.upsert_file("e:/src/b.cpp");
        cg::test::check(r, f1 == f2, "same path upserts to the same file_id");
        cg::test::check(
            r,
            f1 != other,
            "different paths get different file_ids");

        const auto row = store.file(f1);
        cg::test::check(
            r,
            row.has_value() && row->path == "e:/src/a.cpp",
            "file row resolves path text");
        cg::test::check(
            r,
            row->status == cg::util::ParseStatus::Pending,
            "fresh file starts pending");
        cg::test::check(
            r,
            !store.file(other + 1000).has_value(),
            "unknown file id yields no row");

        store.set_file_hash(f1, "ab64");
        store.set_file_status(
            f1,
            cg::util::ParseStatus::Ok,
            0,
            std::nullopt,
            "2026-10-11T00:00:00Z");
        const auto ok = store.file(f1);
        cg::test::check(
            r,
            ok->content_hash.value_or("") == "ab64",
            "content hash round-trips");
        cg::test::check(
            r,
            ok->status == cg::util::ParseStatus::Ok && ok->error_count == 0
                && !ok->last_error.has_value(),
            "ok status round-trips");
        store.set_file_status(
            f1,
            cg::util::ParseStatus::Failed,
            2,
            "boom",
            std::nullopt);
        const auto failed = store.file(f1);
        cg::test::check(
            r,
            failed->status == cg::util::ParseStatus::Failed
                && failed->error_count == 2
                && failed->last_error.value_or("") == "boom",
            "failed status with diagnostics round-trips");
    }

    void symbol_upsert_idempotent(R& r)
    {
        Mini       m;
        const auto again = m.store.upsert_symbol(
            {.unique_key = "function||caller|e:/src/a.cpp",
             .kind = SymbolKind::Function,
             .name = "caller",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = m.fa,
             .line = 42,
             .col = 9});
        cg::test::check(
            r,
            again == m.caller,
            "same unique_key returns the SAME sym_id");
        cg::test::check(
            r,
            cg::db::symbol_count(m.store) == 2,
            "re-upsert keeps exactly one row");
        const auto row = cg::db::get_symbol(m.store, m.caller);
        cg::test::check(
            r,
            row.has_value() && row->line == 42 && row->col == 9,
            "location refreshed on conflict");

        cg::test::check(
            r,
            row->name == "caller" && row->ns == ""
                && row->file == "e:/src/a.cpp",
            "row strings resolve");
        const auto callee_row = cg::db::get_symbol(m.store, m.callee);
        cg::test::check(
            r,
            callee_row->ns == "shape"
                && callee_row->qualified_name == "shape::callee"
                && callee_row->sig_hash.value_or(-1) == 7,
            "namespace path and sig_hash round-trip");

        // 重载对：同名、不同 sig_hash -> 不同符号。
        const auto o1 = m.store.upsert_symbol(
            {.unique_key = "function||scale|int|e:/src/a.cpp",
             .kind = SymbolKind::Function,
             .name = "scale",
             .ns = "",
             .sig_hash = 1,
             .file_id = m.fa,
             .line = 3,
             .col = 1});
        const auto o2 = m.store.upsert_symbol(
            {.unique_key = "function||scale|double|e:/src/a.cpp",
             .kind = SymbolKind::Function,
             .name = "scale",
             .ns = "",
             .sig_hash = 2,
             .file_id = m.fa,
             .line = 4,
             .col = 1,
             .parent_id = std::nullopt});
        cg::test::check(r, o1 != o2, "overloads stay distinct via unique_key");
    }

    void qualified_names_and_members(R& r)
    {
        Mini       m;
        const auto canvas = m.store.upsert_symbol(
            {.unique_key = "record|render|Canvas|e:/src/canvas.hpp",
             .kind = SymbolKind::Record,
             .name = "Canvas",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fb,
             .line = 5,
             .col = 1,
             .flags = cg::db::sym_flags::is_struct});
        const auto draw = m.store.upsert_symbol(
            {.unique_key = "method|render|draw|e:/src/canvas.hpp|p9",
             .kind = SymbolKind::Method,
             .name = "draw",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fb,
             .line = 6,
             .col = 9,
             .parent_id = canvas});
        cg::test::check(
            r,
            m.store.qualified_name(canvas) == "render::Canvas",
            "record qualified name is ns::name");
        cg::test::check(
            r,
            m.store.qualified_name(draw) == "render::Canvas::draw",
            "member qualified name chains through the parent");
        const auto row = cg::db::get_symbol(m.store, draw);
        cg::test::check(
            r,
            row->parent == "Canvas" && row->parent_id.value_or(0) == canvas,
            "parent id and name resolve");

        const auto kids = cg::db::members(m.store, canvas);
        cg::test::check(
            r,
            kids.size() == 1 && kids[0].id == draw,
            "members lists record children");
        cg::test::check(
            r,
            cg::db::members(m.store, draw).empty(),
            "a method has no members");

        // extends 关系：Circle -> Canvas
        const auto circle = m.store.upsert_symbol(
            {.unique_key = "record|render|Circle|e:/src/circle.hpp",
             .kind = SymbolKind::Record,
             .name = "Circle",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fa,
             .line = 1,
             .col = 1});
        m.store.upsert_edge(
            {.src = circle,
             .dst = canvas,
             .type = EdgeType::Extends,
             .site_file = m.fa,
             .site_line = 1,
             .attrs = cg::db::edge_attrs::is_virtual_base});
        const auto b = cg::db::bases(m.store, circle);
        cg::test::check(
            r,
            b.size() == 1 && b[0].dst == canvas
                && b[0].attrs == cg::db::edge_attrs::is_virtual_base,
            "bases returns extends edges with attrs");
        const auto d = cg::db::deriving(m.store, canvas);
        cg::test::check(
            r,
            d.size() == 1 && d[0].src == circle,
            "deriving returns reverse extends edges");
    }

    void edge_occurrences(R& r)
    {
        Mini       m;
        const auto e1 = m.store.upsert_edge(edge(m.caller, m.callee, m.fa, 10));
        const auto e2 = m.store.upsert_edge(edge(m.caller, m.callee, m.fa, 10));
        cg::test::check(
            r,
            e1 == e2,
            "identical site re-upsert returns the same edge_id");
        cg::test::check(
            r,
            cg::db::edge_count(m.store) == 1,
            "identical site yields a single row");

        const auto e3 = m.store.upsert_edge(edge(m.caller, m.callee, m.fa, 20));
        cg::test::check(
            r,
            e3 != e1,
            "a different site line is a different row");
        cg::test::check(
            r,
            cg::db::edge_count(m.store) == 2,
            "distinct sites are distinct rows");

        // 原始行保留按出现点计数：重复观察的点是 2，新点是 1。
        //
        const auto raw_calls =
            cg::db::edges_of(m.store, m.caller, true, EdgeType::Calls);
        cg::test::check(r, raw_calls.size() == 2, "raw rows stay per site");
        std::int64_t occ_at_10 = 0, occ_at_20 = 0;
        for (const auto& row : raw_calls)
        {
            if (row.site_line == 10)
            {
                occ_at_10 = row.occurrences;
            }
            if (row.site_line == 20)
            {
                occ_at_20 = row.occurrences;
            }
        }
        cg::test::check(
            r,
            occ_at_10 == 2 && occ_at_20 == 1,
            "identical site re-upsert counts 2 occurrences");

        const auto out = cg::db::edges_from(m.store, m.caller, {});
        cg::test::check(r, out.size() == 1, "edges_from aggregates per pair");
        cg::test::check(
            r,
            out[0].other == m.callee && out[0].occurrences_sum == 3
                && out[0].min_site_line == 10,
            "aggregation sums occurrences and keeps MIN(site_line)");
        const auto out_calls =
            cg::db::edges_from(m.store, m.caller, std::array{EdgeType::Calls});
        cg::test::check(r, out_calls.size() == 1, "type filter keeps calls");
        const auto out_extends = cg::db::edges_from(
            m.store,
            m.caller,
            std::array{EdgeType::Extends});
        cg::test::check(
            r,
            out_extends.empty(),
            "type filter drops other types");

        const auto in = cg::db::edges_to(m.store, m.callee, {});
        cg::test::check(
            r,
            in.size() == 1 && in[0].other == m.caller
                && in[0].occurrences_sum == 3 && in[0].min_site_line == 10,
            "edges_to mirrors edges_from");

        // confidence 经 edges_of 往返
        m.store.upsert_edge(
            {.src = m.caller,
             .dst = m.callee,
             .type = EdgeType::Overrides,
             .site_file = m.fa,
             .site_line = 5,
             .confidence = 0.7});
        const auto raw =
            cg::db::edges_of(m.store, m.callee, false, EdgeType::Overrides);
        cg::test::check(
            r,
            raw.size() == 1 && raw[0].confidence == 0.7
                && raw[0].occurrences == 1,
            "raw edge rows carry confidence and occurrences");

        // 手写的重复折叠进写入（同行被调方重复）：
        // occurrences = N 在新行落 N、冲突时加 N，所以
        // 同行折叠与跨 TU 折叠可以组合。
        EdgeWrite folded = edge(m.caller, m.callee, m.fa, 30);
        folded.occurrences = 2;
        m.store.upsert_edge(folded);
        m.store.upsert_edge(folded);
        std::int64_t occ_at_30 = 0;
        for (const auto& row :
             cg::db::edges_of(m.store, m.caller, true, EdgeType::Calls))
        {
            if (row.site_line == 30)
            {
                occ_at_30 = row.occurrences;
            }
        }
        cg::test::check(
            r,
            occ_at_30 == 4,
            "folded occurrences set on insert and add on conflict");
    }

    void include_upsert(R& r)
    {
        Mini m;
        m.store.upsert_include(
            {.file = m.fa, .target = "vector", .line = 1, .angled = true});
        m.store.upsert_include(
            {.file = m.fa, .target = "vector", .line = 1, .angled = true});
        m.store.upsert_include(
            {.file = m.fa, .target = "circle.hpp", .line = 2, .angled = false});
        const auto st = cg::db::stats(m.store);
        cg::test::check(
            r,
            st.includes == 2,
            "duplicate include upserts collapse; distinct targets stay");
        cg::test::check(r, st.includes_angled == 1, "angled flag counted");
    }

    // 模块 import 关系（schema v5）：includes 的具名模块孪生——
    // 按 (file, module, line) 幂等、模块文本驻留、purge_file
    // 用该文件的新集合替换它们、erase_file 级联删除。
    void import_upsert(R& r)
    {
        Mini m;
        m.store.upsert_import({.file = m.fa, .module_name = "std", .line = 3});
        m.store.upsert_import({.file = m.fa, .module_name = "std", .line = 3});
        m.store.upsert_import(
            {.file = m.fa, .module_name = "cg.util.json", .line = 4});
        m.store.upsert_import({.file = m.fb, .module_name = "std", .line = 1});
        const auto st = cg::db::stats(m.store);
        cg::test::check(
            r,
            st.imports == 3,
            "duplicate import upserts collapse; distinct modules/"
            "files stay");

        const auto links = cg::db::all_imports(m.store);
        cg::test::check(
            r,
            links.size() == 3,
            "all_imports resolves module text");
        bool saw_partition = false;
        for (const cg::db::ImportLink& link : links)
        {
            if (link.file == m.fa && link.module_name == "cg.util.json"
                && link.line == 4)
            {
                saw_partition = true;
            }
        }
        cg::test::check(
            r,
            saw_partition,
            "partition names intern verbatim (colon spelling)");

        // 按文件替换：清空 a.fa 不保留任何行，然后重新加回一条。
        m.store.purge_file(m.fa);
        cg::test::check(
            r,
            cg::db::stats(m.store).imports == 1,
            "purge_file removes the file's import rows");
        m.store.upsert_import(
            {.file = m.fa, .module_name = "cg.util", .line = 2});
        const auto dump = cg::db::dump_imports(m.store);
        cg::test::check(
            r,
            dump.size() == 2
                && std::ranges::find(dump, "e:/src/a.cpp|cg.util|2")
                       != dump.end(),
            "dump_imports spells canonical per-row text");

        // 整文件删除会级联。
        m.store.erase_file(m.fb);
        cg::test::check(
            r,
            cg::db::stats(m.store).imports == 1,
            "erase_file cascades import rows away");
    }

    void purge_replaces_files(R& r)
    {
        Mini m;
        // canvas（文件 b）带方法 draw；circle（文件 a）extends canvas；
        // caller（文件 a）在文件 a 的一处调用 canvas；文件 a include 了 x.hpp。
        const auto canvas = m.store.upsert_symbol(
            {.unique_key = "record|render|Canvas|e:/src/b.cpp",
             .kind = SymbolKind::Record,
             .name = "Canvas",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fb,
             .line = 5,
             .col = 1});
        const auto draw = m.store.upsert_symbol(
            {.unique_key = "method|render|draw|e:/src/a.cpp|p1",
             .kind = SymbolKind::Method,
             .name = "draw",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fa,
             .line = 7,
             .col = 5,
             .parent_id = canvas});
        const auto gone = m.store.upsert_symbol(
            {.unique_key = "method|render|obsolete|e:/src/a.cpp",
             .kind = SymbolKind::Method,
             .name = "obsolete",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fa,
             .line = 9,
             .col = 5});
        m.store.upsert_edge(edge(m.caller, draw, m.fa, 12));
        m.store.upsert_edge(edge(m.caller, gone, m.fa, 13));
        m.store.upsert_include(
            {.file = m.fa, .target = "x.hpp", .line = 1, .angled = false});
        cg::test::check(
            r,
            cg::db::symbol_count(m.store) == 5
                && cg::db::edge_count(m.store) == 2,
            "seeded purge fixture");
        // 贡献票 seed：文件 a 的条目给本批发射的三个符号各投一票。
        {
            const auto seeded_votes = std::array{m.caller, draw, gone};
            m.store.contribute_symbols(m.fa, "digest-a", seeded_votes);
            cg::test::check(
                r,
                m.store.contributions_of_entry(m.fa, "digest-a").size() == 3,
                "seeded votes registered");
        }

        // 重抽取文件 a：站点属主边/包含随 purge 清理；符号行的生死自
        // schema v18 起由贡献票数仲裁——键失配（obsolete）不再被 purge
        // 直接删除，撤票+归零（与图构建器同一记账流程）才删行。
        {
            auto tx = m.store.begin_tx();
            m.store.purge_file(m.fa);
            // 记账流程的 store 原语直接演练：该文件条目重解析后的新票
            // 集不含 obsolete——差集撤票，归零仲裁判死，删行。
            const auto old_votes =
                m.store.contributions_of_entry(m.fa, "digest-a");
            std::vector<cg::db::SymId> retracted;
            for (const cg::db::SymId sid : old_votes)
            {
                m.store.retract_contribution(sid, m.fa, "digest-a");
                retracted.push_back(sid);
            }
            const auto fresh = std::array{m.caller, draw};
            m.store.contribute_symbols(m.fa, "digest-a", fresh);
            const auto zeroed = m.store.zero_contribution_symbols(retracted);
            cg::test::check(
                r,
                zeroed.size() == 1 && zeroed.front() == gone,
                "zero-vote arbitration picks exactly the retracted "
                "symbol that lost all votes");
            m.store.erase_symbols(zeroed);
            tx.commit();
        }
        cg::test::check(
            r,
            cg::db::symbol_count(m.store) == 4,
            "purge plus zero-vote erase removes only disappeared symbols");
        cg::test::check(
            r,
            !cg::db::get_symbol(m.store, gone).has_value(),
            "obsolete symbol gone after its votes hit zero");
        cg::test::check(
            r,
            cg::db::get_symbol(m.store, m.caller).has_value(),
            "re-extracted caller survives purge");
        const auto draw_row = cg::db::get_symbol(m.store, draw);
        cg::test::check(
            r,
            draw_row.has_value() && draw_row->id == draw,
            "re-extracted symbol keeps its sym_id");
        cg::test::check(
            r,
            cg::db::edge_count(m.store) == 0,
            "site-owned edges and gone-symbol edges are purged");
        const auto st = cg::db::stats(m.store);
        cg::test::check(r, st.includes == 0, "file includes purged");

        // 方法以新位置重新 upsert 且保持 id 不变。
        const auto redraw = m.store.upsert_symbol(
            {.unique_key = "method|render|draw|e:/src/a.cpp|p1",
             .kind = SymbolKind::Method,
             .name = "draw",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fa,
             .line = 8,
             .col = 5,
             .parent_id = canvas});
        cg::test::check(
            r,
            redraw == draw,
            "post-purge re-insert reuses the stable id");
    }

    void find_tiers(R& r)
    {
        Mini       m;
        const auto legacy = m.store.upsert_file("e:/src/legacy.cpp");
        const auto canvas = m.store.upsert_symbol(
            {.unique_key = "record|render|Canvas|e:/src/canvas.hpp",
             .kind = SymbolKind::Record,
             .name = "Canvas",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fb,
             .line = 5,
             .col = 1});
        const auto draw = m.store.upsert_symbol(
            {.unique_key = "method|render|draw|e:/src/canvas.hpp|p1",
             .kind = SymbolKind::Method,
             .name = "draw",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fb,
             .line = 6,
             .col = 9,
             .parent_id = canvas});
        const auto draw_all = m.store.upsert_symbol(
            {.unique_key = "function|render|draw_all|e:/src/draw.cpp",
             .kind = SymbolKind::Function,
             .name = "draw_all",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fa,
             .line = 12,
             .col = 6});
        const auto global_draw = m.store.upsert_symbol(
            {.unique_key = "function||draw|e:/src/legacy.cpp",
             .kind = SymbolKind::Function,
             .name = "draw",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = legacy,
             .line = 3,
             .col = 1});

        // tier 1 by unqualified name（分层语义断言走短路档：合并档会把
        // 子串层的 draw_all 并入命中集）
        auto res = cg::db::find_symbols(
            m.store,
            "draw",
            SymbolFilter{.kind = std::nullopt, .file_substring = std::nullopt},
            /*merge_lower_tiers=*/false);
        cg::test::check(
            r,
            res.tier == 1 && res.matches.size() == 2,
            "exact unqualified name hits tier 1 (both draws)");
        // tier 1：按完整限定名
        res = cg::db::find_symbols(
            m.store,
            "render::Canvas::draw",
            SymbolFilter{.kind = std::nullopt, .file_substring = std::nullopt});
        cg::test::check(
            r,
            res.tier == 1 && res.matches.size() == 1
                && res.matches[0].id == draw,
            "full qualified name hits tier 1");
        // tier 1：按 unique key（高级用户的逃生门）
        res = cg::db::find_symbols(
            m.store,
            "function|render|draw_all|e:/src/draw.cpp",
            SymbolFilter{.kind = std::nullopt, .file_substring = std::nullopt});
        cg::test::check(
            r,
            res.tier == 1 && res.matches.size() == 1
                && res.matches[0].id == draw_all,
            "exact unique key hits tier 1");
        // tier 2：限定名后缀
        res = cg::db::find_symbols(
            m.store,
            "Canvas::draw",
            SymbolFilter{.kind = std::nullopt, .file_substring = std::nullopt});
        cg::test::check(
            r,
            res.tier == 2 && res.matches.size() == 1
                && res.matches[0].id == draw,
            "qualified suffix hits tier 2");
        // tier 3：子串
        res = cg::db::find_symbols(
            m.store,
            "raw_al",
            SymbolFilter{.kind = std::nullopt, .file_substring = std::nullopt});
        cg::test::check(
            r,
            res.tier == 3 && res.matches.size() == 1
                && res.matches[0].id == draw_all,
            "substring hits tier 3");
        // 处处未命中
        res = cg::db::find_symbols(
            m.store,
            "zzzz",
            SymbolFilter{.kind = std::nullopt, .file_substring = std::nullopt});
        cg::test::check(
            r,
            res.tier == 0 && res.matches.empty(),
            "no hit in any tier reports tier 0");
        // 过滤器收窄 tier-1 命中
        SymbolFilter only_functions{
            .kind = SymbolKind::Function,
            .file_substring = std::nullopt};
        res = cg::db::find_symbols(
            m.store,
            "draw",
            only_functions,
            /*merge_lower_tiers=*/false);
        cg::test::check(
            r,
            res.tier == 1 && res.matches.size() == 1
                && res.matches[0].id == global_draw,
            "kind filter narrows tier-1 matches");
        SymbolFilter by_file{.file_substring = "legacy"};
        res = cg::db::find_symbols(m.store, "draw", by_file);
        cg::test::check(
            r,
            res.tier == 1 && res.matches.size() == 1,
            "file-substring filter narrows tier-1 matches");
    }

    void edges_to_aggregation(R& r)
    {
        Mini       m;
        const auto helper = m.store.upsert_symbol(
            {.unique_key = "function||helper|e:/src/a.cpp",
             .kind = SymbolKind::Function,
             .name = "helper",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = m.fa,
             .line = 2,
             .col = 1});
        // main 调用 pi 两次（两个出现点）、helper 一次；main 还调用 helper。
        m.store.upsert_edge(edge(m.caller, m.callee, m.fa, 3));
        m.store.upsert_edge(edge(m.caller, m.callee, m.fa, 7));
        m.store.upsert_edge(edge(helper, m.callee, m.fa, 11));
        m.store.upsert_edge(edge(m.caller, helper, m.fa, 5));

        const auto callers = cg::db::edges_to(m.store, m.callee, {});
        cg::test::check(
            r,
            callers.size() == 2,
            "two distinct callers aggregate to two rows");
        std::int64_t main_sum = 0, helper_sum = 0;
        for (const auto& row : callers)
        {
            if (row.other == m.caller)
            {
                main_sum = row.occurrences_sum;
            }
            if (row.other == helper)
            {
                helper_sum = row.occurrences_sum;
            }
        }
        cg::test::check(r, main_sum == 2, "caller's two sites sum to 2");
        cg::test::check(r, helper_sum == 1, "single-site caller sums to 1");
        cg::test::check(
            r,
            std::ranges::all_of(
                callers,
                [](const auto& row) { return row.type == EdgeType::Calls; }),
            "aggregated rows carry the edge type");
    }

    void stats_matches_seeded_data(R& r)
    {
        Mini       m;
        const auto canvas = m.store.upsert_symbol(
            {.unique_key = "record|render|Canvas|e:/src/b.cpp",
             .kind = SymbolKind::Record,
             .name = "Canvas",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fb,
             .line = 5,
             .col = 1});
        m.store.upsert_symbol(
            {.unique_key = "method|render|draw|e:/src/b.cpp|p1",
             .kind = SymbolKind::Method,
             .name = "draw",
             .ns = "render",
             .sig_hash = std::nullopt,
             .file_id = m.fb,
             .line = 6,
             .col = 5,
             .parent_id = canvas});
        m.store.upsert_edge(edge(m.caller, m.callee, m.fa, 1));
        m.store.upsert_edge(
            {.src = m.caller,
             .dst = canvas,
             .type = EdgeType::Extends,
             .site_file = m.fa,
             .site_line = 2});
        m.store.upsert_include(
            {.file = m.fa, .target = "vector", .line = 1, .angled = true});
        m.store.upsert_include(
            {.file = m.fa, .target = "b.cpp", .line = 2, .angled = false});
        m.store.set_file_status(
            m.fa,
            cg::util::ParseStatus::Ok,
            0,
            std::nullopt,
            "2026-10-11T01:02:03Z");
        m.store.set_file_status(
            m.fb,
            cg::util::ParseStatus::Failed,
            1,
            "diag",
            std::nullopt);

        const auto st = cg::db::stats(m.store);
        cg::test::check(
            r,
            st.files == 2 && st.files_ok == 1 && st.files_failed == 1
                && st.files_pending == 0,
            "file statuses counted");
        cg::test::check(
            r,
            st.symbols == 4
                && st.symbols_by_kind[cg::db::to_id(SymbolKind::Function)] == 2
                && st.symbols_by_kind[cg::db::to_id(SymbolKind::Record)] == 1
                && st.symbols_by_kind[cg::db::to_id(SymbolKind::Method)] == 1,
            "symbols counted per kind");
        cg::test::check(
            r,
            st.edges == 2
                && st.edges_by_type[cg::db::to_id(EdgeType::Calls)] == 1
                && st.edges_by_type[cg::db::to_id(EdgeType::Extends)] == 1,
            "edges counted per type");
        cg::test::check(
            r,
            st.includes == 2 && st.includes_angled == 1,
            "includes counted");
        // 字符串：4 个符号 + 2 个路径驻留的 unique_key/name/ns 文本
        cg::test::check(r, st.strings >= 8, "interned strings counted");
        cg::test::check(
            r,
            st.app_version == std::string{cg::util::version},
            "stats carries app_version");
    }

    // meta 篡改容错（行为变更波）：analysis_rounds/analysis_work_items 是
    // 机器来源（写入侧恒为 to_string 纯数字），被外部 sqlite 改脏后 stats
    // 必须归 0 显示而非整单失败——此前 stoll 抛 invalid_argument，实测
    // 整条 ccg stats 退出 1、连 files/symbols 都不输出，与同屏
    // analysis_snap_* 的宽容显示（atoll 归 0）自相矛盾。
    void stats_survives_tampered_meta(R& r)
    {
        Mini m;
        m.store.set_meta("analysis_rounds", "abc");
        m.store.set_meta("analysis_work_items", "xyz");
        bool          threw = false;
        cg::db::Stats st{};
        try
        {
            st = cg::db::stats(m.store);
        }
        catch (...)
        {
            threw = true;
        }
        cg::test::check(r, !threw, "stats survives tampered machine meta");
        cg::test::check(
            r,
            st.analysis_rounds == 0 && st.analysis_work_items == 0,
            "tampered analysis meta reads as 0");
        cg::test::check(
            r,
            st.files == 2 && st.symbols == 2,
            "the rest of stats still populated");
        // 对照组：合法值照常透出。
        m.store.set_meta("analysis_rounds", "7");
        m.store.set_meta("analysis_work_items", "123");
        const auto ok = cg::db::stats(m.store);
        cg::test::check(
            r,
            ok.analysis_rounds == 7 && ok.analysis_work_items == 123,
            "well-formed analysis meta round-trips");
    }

    void transactions_commit_and_rollback(R& r)
    {
        GraphStore store{":memory:"};
        const auto f = store.upsert_file("e:/src/tx.cpp");

        { // 提交持久化
            auto tx = store.begin_tx();
            store.upsert_symbol(
                {.unique_key = "function||keep|e:/src/tx.cpp",
                 .kind = SymbolKind::Function,
                 .name = "keep",
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = f,
                 .line = 1,
                 .col = 1});
            tx.commit();
        }
        cg::test::check(
            r,
            cg::db::symbol_count(store) == 1,
            "committed work persists");

        { // 显式回滚丢弃
            auto tx = store.begin_tx();
            store.upsert_symbol(
                {.unique_key = "function||discard|e:/src/tx.cpp",
                 .kind = SymbolKind::Function,
                 .name = "discard",
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = f,
                 .line = 2,
                 .col = 1});
            tx.rollback();
        }
        cg::test::check(
            r,
            cg::db::symbol_count(store) == 1,
            "rolled-back work discarded");

        { // RAII：销毁未完成事务即回滚
            const auto tx = store.begin_tx();
            store.upsert_symbol(
                {.unique_key = "function||auto|e:/src/tx.cpp",
                 .kind = SymbolKind::Function,
                 .name = "auto",
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = f,
                 .line = 3,
                 .col = 1});
        }
        cg::test::check(
            r,
            cg::db::symbol_count(store) == 1,
            "unfinished tx auto-rolls-back on destruction");

        { // 单事务内成批（导入形态）
            auto tx = store.begin_tx();
            for (int i = 0; i < 50; ++i)
            {
                store.upsert_symbol(
                    {.unique_key =
                         std::format("function||f{}|e:/src/tx.cpp", i),
                     .kind = SymbolKind::Function,
                     .name = std::format("f{}", i),
                     .ns = "",
                     .sig_hash = std::nullopt,
                     .file_id = f,
                     .line = i,
                     .col = 1});
            }
            tx.commit();
        }
        cg::test::check(
            r,
            cg::db::symbol_count(store) == 51,
            "a 50-row batch commits atomically");
    }

    /// 命中面 #external 行的装载（行/列与载荷按用例注入）。
    void upsert_external_hit(
        Mini&            m,
        int              line,
        int              col,
        std::string_view type_set,
        int              confidence,
        std::string_view fallback)
    {
        m.store.upsert_type_hint(
            {.owner_id = m.caller,
             .var_name = "#external",
             .file_id = m.fa,
             .line = line,
             .col = col,
             .type_set = type_set,
             .source = 12,
             .confidence = confidence,
             .fallback_reason = fallback,
             .param_index = -1});
    }

    /// 干扰面邻值行的装载（槽名/来源/位置按用例注入）。
    void upsert_near_row(Mini& m, std::string_view var, int source, int line)
    {
        m.store.upsert_type_hint(
            {.owner_id = m.caller,
             .var_name = var,
             .file_id = m.fa,
             .line = line,
             .col = 5,
             .type_set = R"(["record|Near"])",
             .source = source});
    }

    void type_hint_source_projection(R& r)
    {
        Mini m;
        // 命中面：四个 (owner,file,line,col) 组合的 #external 行，type_set/
        // confidence/fallback 等载荷各不相同（行内容全字段参与等价比对）。
        upsert_external_hit(m, 10, 1, R"(["record|Device"])", 3, "");
        upsert_external_hit(
            m,
            11,
            2,
            R"(["record|Screen"])",
            2,
            "single-candidate");
        m.store.upsert_type_hint(
            {.owner_id = m.callee,
             .var_name = "#external",
             .file_id = m.fb,
             .line = 20,
             .col = 3,
             .type_set = R"(["record|Speaker"])",
             .source = 12});
        m.store.upsert_type_hint(
            {.owner_id = m.callee,
             .var_name = "#external",
             .file_id = m.fb,
             .line = 21,
             .col = 4,
             .type_set = R"(["record|Box"])",
             .source = 12,
             .confidence = 1,
             .fallback_reason = "wide-shape"});

        // 干扰面：槽名/来源的邻值边界行。C++ 写路径 source 一律按
        // int64 绑定，INTEGER 亲和列的 TEXT 数值形态不可达（仅 raw SQL
        // 才能注入，另行由 SQL 层比对覆盖），这里覆盖字符串侧的全部
        // 分叉形态：大小写、首/尾空格、嵌入 NUL、前后缀邻居、来源
        // 邻值 11/13、以及另一个真实槽名。
        const std::string embedded_nul =
            std::string{"#external"} + '\0' + std::string{"extra"};
        upsert_near_row(m, "#External", 12, 30);  // 大小写
        upsert_near_row(m, " #external", 12, 31); // 前导空格
        upsert_near_row(m, "#external ", 12, 32); // 尾随空格
        upsert_near_row(
            m,
            embedded_nul,
            12,
            33); // 嵌入 NUL（sqlite 按字节长度存取）
        upsert_near_row(m, "#externa", 12, 34);   // 前缀截断邻居
        upsert_near_row(m, "#externall", 12, 35); // 后缀扩展邻居
        upsert_near_row(m, "#external", 11, 36);  // 来源下邻值
        upsert_near_row(m, "#external", 13, 37);  // 来源上邻值
        upsert_near_row(m, "#ret", 12, 38);       // 另一个真实槽名

        const auto projected = m.store.type_hints_of_source(12, "#external");
        cg::test::check(
            r,
            projected.size() == 4,
            std::format(
                "projection returns exactly the four #external rows (got {})",
                projected.size()));

        // 全读过滤参照臂：all_type_hints 后按同谓词内存过滤（旧行集形）。
        std::vector<cg::db::TypeHintRow> filtered;
        for (const cg::db::TypeHintRow& row : m.store.all_type_hints())
        {
            if (row.source == 12 && row.var_name == "#external")
            {
                filtered.push_back(row);
            }
        }

        const auto spell = [](const cg::db::TypeHintRow& row)
        {
            return std::format(
                "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                row.id,
                row.owner_id,
                row.var_name,
                row.file_id,
                row.line,
                row.col,
                row.type_set,
                row.source,
                row.confidence,
                row.fallback_reason,
                row.param_index,
                row.related_var,
                row.related_key);
        };
        const auto same =
            std::ranges::equal(projected, filtered, {}, spell, spell);
        std::string first_diff;
        if (!same)
        {
            for (std::size_t i = 0;
                 i < std::max(projected.size(), filtered.size());
                 ++i)
            {
                const std::string a =
                    i < projected.size() ? spell(projected[i]) : "<none>";
                const std::string b =
                    i < filtered.size() ? spell(filtered[i]) : "<none>";
                if (a != b)
                {
                    first_diff = std::format("row {}: [{}] vs [{}]", i, a, b);
                    break;
                }
            }
        }
        cg::test::check(
            r,
            same,
            std::format(
                "WHERE projection rows == full-read filtered rows ({})",
                first_diff));
    }

    const cg::test::Registrar reg_schema{
        "db.schema_bootstrap",
        &schema_bootstrap};
    const cg::test::Registrar reg_coord{
        "db.realtime_coordination",
        &realtime_coordination};
    const cg::test::Registrar reg_steal{
        "db.update_steal_active",
        &update_steal_active};
    const cg::test::Registrar reg_coord_audit{
        "db.coord_events_audit",
        &coord_events_audit};
    const cg::test::Registrar reg_renew{
        "db.update_renew_lease",
        &update_renew_lease};
    const cg::test::Registrar reg_clear{
        "db.pending_clear_covered",
        &pending_clear_covered};
    const cg::test::Registrar reg_guard{
        "db.schema_version_guard",
        &schema_version_guard};
    const cg::test::Registrar reg_intern{
        "db.interning_identity",
        &interning_identity};
    const cg::test::Registrar reg_files{
        "db.file_upsert_and_status",
        &file_upsert_and_status};
    const cg::test::Registrar reg_symbols{
        "db.symbol_upsert_idempotent",
        &symbol_upsert_idempotent};
    const cg::test::Registrar reg_qualified{
        "db.qualified_names_and_members",
        &qualified_names_and_members};
    const cg::test::Registrar reg_edges{
        "db.edge_occurrences",
        &edge_occurrences};
    const cg::test::Registrar reg_includes{
        "db.include_upsert",
        &include_upsert};
    const cg::test::Registrar reg_imports{"db.import_upsert", &import_upsert};
    const cg::test::Registrar reg_purge{
        "db.purge_replaces_files",
        &purge_replaces_files};
    const cg::test::Registrar reg_find{"db.find_tiers", &find_tiers};
    const cg::test::Registrar reg_agg{
        "db.edges_to_aggregation",
        &edges_to_aggregation};
    const cg::test::Registrar reg_stats{
        "db.stats_matches_seeded_data",
        &stats_matches_seeded_data};
    const cg::test::Registrar reg_stats_meta{
        "db.stats_survives_tampered_meta",
        &stats_survives_tampered_meta};
    const cg::test::Registrar reg_tx{
        "db.transactions_commit_and_rollback",
        &transactions_commit_and_rollback};
    const cg::test::Registrar reg_hint_projection{
        "db.type_hint_source_projection",
        &type_hint_source_projection};

    void meta_erase_roundtrip(cg::test::Result& r)
    {
        GraphStore store{":memory:"};
        store.set_meta("llvm_path", "E:/some/llvm");
        const auto before = store.meta("llvm_path");
        cg::test::check(
            r,
            before.has_value() && *before == "E:/some/llvm",
            "meta row written");
        store.erase_meta("llvm_path");
        cg::test::check(
            r,
            !store.meta("llvm_path").has_value(),
            "erase_meta removes the row (absent = NULL = default "
            "engine resolution)");
        store.erase_meta("llvm_path");
        cg::test::check(
            r,
            !store.meta("llvm_path").has_value(),
            "erasing an absent key is a no-op");
    }

    const cg::test::Registrar reg_meta_erase{
        "db.meta_erase",
        &meta_erase_roundtrip};

    // 提交阶段的 lease 心跳（事务内心跳方案：心跳与提交同事务）：全量
    // 索引运行的提交阶段是一个大写事务，而 SQLite
    // 没有嵌套事务，所以 peek_pending 心跳模式无法在
    // 其内部运行——heartbeat_in_tx 改为在 CALLER 已打开的
    // 事务里执行预编译的 CLAIM_HEARTBEAT。零行（claim 被抢）必须
    // 读作"干净中止"；健康的续租不得扰动同一事务里
    // 已写入的批数据行。
    void commit_lease_heartbeat(R& r)
    {
        namespace fs = std::filesystem;
        const auto        path = fs::path{"ccg-db-heartbeat-test.db"};
        const std::string p{path.string()};
        wipe_db(p);
        {
            GraphStore creator{p};
        }
        GraphStore holder{p};
        GraphStore thief{p};
        holder.set_busy_timeout_ms(60'000);
        thief.set_busy_timeout_ms(60'000);

        cg::test::check(
            r,
            holder.claim_update(60'000, "H1"),
            "claim for the in-tx heartbeat case");
        const std::int64_t lease_before = holder.update_claim_holder()->second;

        // 续租 + 批写入 + 提交在一个事务里：心跳
        // 既不能破坏提交，也不能让 lease 未续。
        {
            auto tx = holder.begin_tx(/*immediate=*/true);
            cg::test::check(
                r,
                holder.heartbeat_in_tx("H1", 120'000),
                "an in-tx heartbeat renews the own claim");
            cg::test::check(
                r,
                !holder.heartbeat_in_tx("not-the-holder", 120'000),
                "an in-tx heartbeat under a foreign holder matches "
                "zero rows and touches nothing");
            const cg::db::FileId f = holder.upsert_file("e:/beat/a.cpp");
            holder.set_file_hash(f, "aa");
            tx.commit();
        }
        const auto row = holder.update_claim_holder();
        cg::test::check(
            r,
            row.has_value() && row->first == "H1" && row->second > lease_before,
            "the committed heartbeat extended the lease and the "
            "batch rows landed");

        // 中途被抢：抢夺者改写了单例行，原持有者的
        // 事务内心跳匹配零行——提交阶段
        // 的正确反应是干净的 ROLLBACK（什么都没写）。
        cg::test::check(
            r,
            holder.release_update("H1"),
            "release the live claim for the steal phase");
        cg::test::check(
            r,
            holder.claim_update(0, "H2"),
            "a zero-lease claim is granted (instantly stale)");
        cg::test::check(
            r,
            thief.claim_update(60'000, "T1"),
            "the thief takes the expired claim");
        {
            auto tx = holder.begin_tx(true);
            (void)holder.upsert_file("e:/beat/c.cpp");
            cg::test::check(
                r,
                !holder.heartbeat_in_tx("H2", 120'000),
                "an in-tx heartbeat under a stolen claim matches "
                "zero rows");
            tx.rollback();
        }
        const auto stolen_row = holder.update_claim_holder();
        cg::test::check(
            r,
            stolen_row.has_value() && stolen_row->first == "T1",
            "the rolled-back heartbeat left the thief's row intact");
        cg::test::check(
            r,
            !holder.find_file("e:/beat/c.cpp").has_value(),
            "the rolled-back rows are gone (all-or-nothing commit "
            "stage semantics)");
        cg::test::check(r, thief.release_update("T1"), "final cleanup release");

        wipe_db(p);
    }

    const cg::test::Registrar reg_beat{
        "db.commit_lease_heartbeat",
        &commit_lease_heartbeat};

    // 批量轻量投影：同一批 id 的七字段投影与逐符号完整物化逐位一致；
    // 行序跟随输入 id 序；批量越过单块 IN 上限（分块路径也被覆盖）。
    void symbol_briefs_projection(R& r)
    {
        Mini                       mini;
        GraphStore&                store = mini.store;
        std::vector<cg::db::SymId> ids{mini.caller, mini.callee};

        // 深父链（method 挂在 record 下）与同名重载（不同文件、同限定名）。
        const cg::db::FileId fc = store.upsert_file("e:/src/c.cpp");
        const cg::db::SymId  record = store.upsert_symbol(
            {.unique_key = "record|outer|Rec|e:/src/c.cpp",
             .kind = SymbolKind::Record,
             .name = "Rec",
             .ns = "outer",
             .sig_hash = std::nullopt,
             .file_id = fc,
             .line = 10,
             .col = 7});
        const cg::db::SymId method = store.upsert_symbol(
            {.unique_key = "method|outer::Rec|ov|e:/src/c.cpp|1",
             .kind = SymbolKind::Method,
             .name = "ov",
             .ns = "outer",
             .sig_hash = 11,
             .file_id = fc,
             .line = 11,
             .col = 8,
             .parent_id = record});
        const cg::db::FileId fd = store.upsert_file("e:/src/d.cpp");
        const cg::db::SymId  overload = store.upsert_symbol(
            {.unique_key = "method|outer::Rec|ov|e:/src/d.cpp|2",
             .kind = SymbolKind::Method,
             .name = "ov",
             .ns = "outer",
             .sig_hash = std::nullopt,
             .file_id = fd,
             .line = 3,
             .col = 4,
             .parent_id = record});
        ids.push_back(record);
        ids.push_back(method);
        ids.push_back(overload);

        // 批量主体：越过 BRIEF_CHUNK=400 的单块上限，覆盖分块路径。
        const cg::db::FileId fb2 = store.upsert_file("e:/src/bulk.cpp");
        for (int i = 0; i < 450; ++i)
        {
            ids.push_back(store.upsert_symbol(
                {.unique_key =
                     std::format("function||bulk_{}|e:/src/bulk.cpp", i),
                 .kind = SymbolKind::Function,
                 .name = std::format("bulk_{}", i),
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = fb2,
                 .line = 100 + i,
                 .col = 1}));
        }

        const std::vector<cg::db::SymbolBrief> briefs =
            cg::db::symbol_briefs(store, ids);
        cg::test::check(
            r,
            briefs.size() == ids.size(),
            "every id projects to a brief row");
        bool order_ok = true;
        bool fields_ok = true;
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            order_ok = order_ok && briefs[i].id == ids[i];
            const auto full = cg::db::get_symbol(store, ids[i]);
            if (!full.has_value())
            {
                fields_ok = false;
                break;
            }
            fields_ok = fields_ok && briefs[i].kind == full->kind
                        && briefs[i].name == full->name
                        && briefs[i].unique_key == full->unique_key
                        && briefs[i].file == full->file
                        && briefs[i].line == full->line
                        && briefs[i].col == full->col;
        }
        cg::test::check(r, order_ok, "brief rows follow the input id order");
        cg::test::check(
            r,
            fields_ok,
            "brief seven fields match the full symbol_row fields");

        // 缺失 id（已删除/不存在）被跳过，不破坏其余行与顺序。
        const std::vector<cg::db::SymId> with_gap{
            ids[0],
            999'999'999LL,
            ids[2]};
        const std::vector<cg::db::SymbolBrief> gapped =
            cg::db::symbol_briefs(store, with_gap);
        cg::test::check(
            r,
            gapped.size() == 2 && gapped[0].id == ids[0]
                && gapped[1].id == ids[2],
            "missing ids are skipped in place");
        // 空批零开销路径。
        cg::test::check(
            r,
            cg::db::symbol_briefs(store, {}).empty(),
            "empty batch projects to nothing");
    }

    const cg::test::Registrar reg_briefs{
        "db.symbol_briefs_projection",
        &symbol_briefs_projection};

    // 路径缓存：命中直答、set_file_path/erase_file 失效、与 file() 全行
    // 视图一致。
    void file_path_cache(R& r)
    {
        GraphStore           store{":memory:"};
        const cg::db::FileId f1 = store.upsert_file("e:/cache/one.cpp");
        const cg::db::FileId f2 = store.upsert_file("e:/cache/two.cpp");
        cg::test::check(
            r,
            store.file_path(f1).value_or("") == "e:/cache/one.cpp"
                && store.file_path(f2).value_or("") == "e:/cache/two.cpp",
            "file_path resolves and caches fresh rows");
        store.set_file_path(f1, "e:/cache/ONE.cpp");
        cg::test::check(
            r,
            store.file_path(f1).value_or("") == "e:/cache/ONE.cpp",
            "set_file_path invalidates the cached path");
        const auto row = store.file(f1);
        cg::test::check(
            r,
            row.has_value() && row->path == "e:/cache/ONE.cpp",
            "the full file() view agrees after the rewrite");
        cg::test::check(
            r,
            store.file_path(f2).value_or("") == "e:/cache/two.cpp",
            "unrelated cache entries survive a path rewrite");
        store.erase_file(f2);
        cg::test::check(
            r,
            !store.file_path(f2).has_value(),
            "erase_file invalidates the cache (no dangling hit)");
        cg::test::check(
            r,
            !store.file(999'999'999LL).has_value(),
            "a missing file id resolves to nullopt");
    }

    const cg::test::Registrar reg_file_path_cache{
        "db.file_path_cache_invalidation",
        &file_path_cache};

    // erase_symbols 的外键级联钉子：一个被删符号牵动的全部引用形态
    // ——edges（src/dst CASCADE）、type_hints（owner CASCADE）、
    // 子成员（parent_id CASCADE，跨文件也删）、call_sites 的
    // caller CASCADE 与 callee/method/static_type SET NULL——逐表断言。
    // 该删除面是层级正交化批的行清除半边（模板死实例行显式删除），
    // 此前仅经 incr.equiv 间接覆盖，此处补 store 层直接单测。
    void erase_symbols_cascade_forms(R& r)
    {
        GraphStore           store{":memory:"};
        const cg::db::FileId fmain = store.upsert_file("e:/src/main.cpp");
        const cg::db::FileId fhdr = store.upsert_file("e:/src/logic.hpp");

        const cg::db::SymId victim = store.upsert_symbol(
            {.unique_key = "function||victim|e:/src/main.cpp",
             .kind = SymbolKind::Function,
             .name = "victim",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = fmain,
             .line = 1,
             .col = 5});
        const cg::db::SymId bystander = store.upsert_symbol(
            {.unique_key = "function||bystander|e:/src/main.cpp",
             .kind = SymbolKind::Function,
             .name = "bystander",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = fmain,
             .line = 9,
             .col = 5});
        // 子成员挂在另一文件：parent_id 级联跨文件生效（与拥有文件无关）。
        const cg::db::SymId child = store.upsert_symbol(
            {.unique_key = "method|victim|emit|e:/src/logic.hpp",
             .kind = SymbolKind::Method,
             .name = "emit",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = fhdr,
             .line = 3,
             .col = 9,
             .parent_id = victim});

        store.upsert_edge(edge(bystander, victim, fmain, 10));
        store.upsert_type_hint(
            {.owner_id = victim,
             .var_name = "slot",
             .file_id = fmain,
             .line = 11,
             .col = 13,
             .type_set = "[\"victim*\"]",
             .source = 0});
        // 站点 A：callee/method/static_type 三列全指 victim（SET NULL 面）。
        store.upsert_call_site(
            {.caller_id = bystander,
             .callee_id = victim,
             .callee_key = "function||victim",
             .callee_name = "victim",
             .method_id = victim,
             .method_key = "method|victim|emit",
             .static_type_id = victim,
             .file_id = fmain,
             .line = 20,
             .col = 9});
        // 站点 B：caller 是 victim（caller CASCADE 面）。
        store.upsert_call_site(
            {.caller_id = victim,
             .callee_id = bystander,
             .callee_key = "function||bystander",
             .callee_name = "bystander",
             .file_id = fmain,
             .line = 30,
             .col = 9});

        const auto pre = cg::db::stats(store);
        cg::test::check(
            r,
            pre.symbols == 3 && pre.edges == 1 && pre.type_hints == 1
                && pre.call_sites == 2,
            std::format(
                "seed counts before erase (symbols {}, edges {}, hints {}, "
                "sites {})",
                pre.symbols,
                pre.edges,
                pre.type_hints,
                pre.call_sites));
        if (pre.symbols != 3 || pre.edges != 1 || pre.type_hints != 1
            || pre.call_sites != 2)
        {
            return; // 播种失败时后续断言只会级联误报
        }

        const std::vector<cg::db::SymId> victims{victim};
        store.erase_symbols(victims);

        const std::vector<cg::db::FullSymbolRow> syms =
            cg::db::all_symbol_rows(store);
        bool victim_gone = true;
        bool child_gone = true;
        bool byst_alive = false;
        for (const cg::db::FullSymbolRow& s : syms)
        {
            victim_gone = victim_gone && s.id != victim;
            child_gone = child_gone && s.id != child;
            byst_alive = byst_alive || s.id == bystander;
        }
        cg::test::check(
            r,
            victim_gone && child_gone && byst_alive,
            std::format(
                "the erased row and its child die, the bystander survives "
                "({} rows left)",
                syms.size()));
        cg::test::check(
            r,
            cg::db::all_edges(store).empty(),
            "edges referencing the erased symbol cascade away");
        cg::test::check(
            r,
            cg::db::all_type_hints(store).empty(),
            "type hints owned by the erased symbol cascade away");
        const std::vector<cg::db::CallSiteRow> sites =
            cg::db::all_call_sites(store);
        cg::test::check(
            r,
            sites.size() == 1,
            std::format(
                "only the SET-NULL site survives the caller-cascade site "
                "({} sites left)",
                sites.size()));
        if (sites.size() == 1)
        {
            cg::test::check(
                r,
                sites[0].caller_id == bystander
                    && !sites[0].callee_id.has_value()
                    && !sites[0].method_id.has_value()
                    && !sites[0].static_type_id.has_value(),
                "callee/method/static_type flip to NULL, caller untouched");
        }

        // 行不存在时幂等空操作（重复删除不炸不误删）。
        store.erase_symbols(victims);
        cg::test::check(
            r,
            cg::db::all_symbol_rows(store).size() == 1,
            "re-erasing a missing row is a no-op");
    }

    const cg::test::Registrar reg_erase_symbols_cascade{
        "db.erase_symbols_cascade_forms",
        &erase_symbols_cascade_forms};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }
