/**
 * @module cg.cli.update
 *
 * @brief `ccg update` 实时刷新运行器。
 *
 * @internal
 * 曾为实现单元；模块布局扩展改为接口模块——无 BMI 单元触发 C2801 的规避
 * 手法随之退役（cmake/CcgC2801Workaround.cmake 持有该修复）。
 */
export module cg.cli.update;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.clang;
import cg.graph;
import cg.msbuild;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.runners;
import cg.cli.layout;

namespace
{
    /**
     * @brief 让 std::format 一族在本单元的第一次实例化落在这段常量的初始化上。
     *
     * @internal clang 23.1.5 配 libc++ 时，本单元去掉这段常量，前端就在实例化
     *           std::format 一族的格式串检查模板（basic_format_string 的构造函
     *           数与 __handles_）时访问违例，重编五次结果相同。崩的位置是
     *           std::format 一族的一次普通调用（std::format 或 std::println），
     *           各单元的实参形态互不相同（单个 const char*、std::string、整数的
     *           引用等），有的在 lambda 里。
     *
     *           同项目里另有 15 个单元同样导入 std、同样直接或间接导入 sqlpp23
     *           的模块，不带这段常量能编过。与是否向 std::cerr 打印无关（不打印
     *           的单元也崩），与本单元全局模块片段里有没有标准库头文本也无关
     *           （带 `<cstdio>` 的单元也崩）。哪些单元会触发，规律没有查清，这
     *           里只记实测到的事实。
     *
     *           这段常量抢在一切使用之前，把 std::format 一族在本单元的第一次实
     *           例化固定成一次最简单的直接使用。在这套工具链上，别的单元若出现
     *           同类崩溃（调用栈里有 basic_format_string 与 __handles_），按同
     *           样的写法加一段；升级 clang 之后可以逐个单元去掉试编，不崩就删。
     */
    [[maybe_unused]]
    inline const std::string FORMAT_WARMUP_TEXT =
        std::format("{} {} {} {} {}", "a", std::string("b"), 1, 2.5, 'c');
} // namespace

export namespace cg::cli
{

    namespace detail
    {
        namespace fs = std::filesystem;
        namespace json = cg::util::json;

        /**
         * @brief 写入声明行的持有者身份记号。
         *
         * @internal
         * `ccg update` 的库协调协议（排队租约 + 事务内心跳 +
         * 偷取仲裁）：数据库自身是协调者。 pending_updates
         * 是变更队列（每行一个规范化路径，PRIMARY KEY 把并发
         * 入队去重），update_claim 单例行（持有者 + Unix 纪元毫秒的租约到期）
         * 是独占声明。没有锁文件、没有 PID 启发式：租约已过期的声明会被下一
         * 个更新者偷走，故崩溃的持有者在下一个事件上自愈。每个协调步骤都是
         * 一个短 SQLite 事务（连接上武装了 busy_timeout，并发引擎批次被等走
         * 而非失败）；其间的引擎工作走普通增量路径、不开任何协调事务。
         * 一次性刷新流程：自己的文件入队 pending_updates，随后在一个短
         * IMMEDIATE 事务里取声明（他处有活声明则立即退出 0——持有者的排空
         * 循环会覆盖队列；过期声明被偷走，即崩溃自愈）。持有者循环重复
         * 「短 IMMEDIATE 事务：租约心跳 + peek 整个队列（不删除）」，并把
         * 每批经普通增量引擎以外部种子处理；批后只清除本次覆盖且内容哈希
         * 与批前快照一致的条目（见下），直到 peek 为空；随后释放声明、
         * 退出 0。落在最后一次空 peek 与释放之间的队列项是任何无守护队列
         * 的普通尾巴：下一个事件或 Stop 钩子的 --drain 会收敛它。等价契约：
         * 批次经普通增量引擎以外部种子运行，故静置的数据库等于 `ccg index`
         * 对同一树状态的结论（种子文件被强制变脏，即使编辑保持了
         * mtime+size——信号是事件而非 stat）。
         *
         * 不丢更新的出队规则（硬约束）：peek 返回后先给每个被跟踪条目记下
         * 内容哈希快照；引擎批次结束后，仅当路径在本批 IndexSummary::
         * covered_files 中（本次重解析的主 TU 或包含闭包传播触及）、且当前
         * 哈希仍等于快照哈希时才删除该队列行。当前哈希由 clear_covered 在
         * 其 IMMEDIATE 事务内重算（不在事务外预计算），删除后还会再核一次
         * 并在哈希已变时立即重新入队——判定与 DELETE 之间落地的编辑不会
         * 被删掉的队列行吞没。批运行期间的新编辑会改变哈希（或已由主键
         * 折叠进同一行），该条目保留给下一批——宁可多跑一次，绝不丢更新。不在
         * db/compile db 中的未跟踪条目永远无事可做： 按既有 note
         * 报告后直接从队列删除，避免永久堆积。身份记号含 pid （给人读）与一个
         * steady_clock
         * 滴答（使单进程的两次更新——测试、钩子风暴——持有不同声明）；仅作
         * 诊断——活性由租约时间戳裁定，从不由该字符串裁定。
         */
        using cg::db::update_claim_token;

        /**
         * @brief RAII 声明释放——已下沉 db 层（cg::db::UpdateClaimGuard，
         *        db.store.ixx；全量索引的提交段租约复用同一协议），此处仅引用。
         */
        using cg::db::UpdateClaimGuard;

        /**
         * @brief 规范化更新文件清单，与 ccg changes 完全一致（绝对化 +
         *        词典序规范化 + norm_path），按喂入顺序去重。
         */
        [[nodiscard]]
        auto normalize_update_files(const std::vector<std::string>& raw_files)
            -> std::vector<std::string>
        {
            std::vector<std::string>        out;
            std::unordered_set<std::string> seen;
            for (const std::string& raw : raw_files)
            {
                std::error_code ec;
                fs::path        abs = fs::absolute(fs::path{raw}, ec);
                if (ec)
                {
                    abs = fs::path{raw};
                }
                const std::string norm = cg::util::norm_path(
                    abs.lexically_normal().generic_string());
                if (seen.insert(norm).second)
                {
                    out.push_back(norm);
                }
            }
            return out;
        }

        /**
         * @brief 项目文件行重采集联动的参数面。
         *
         * @internal run_update 传给重采集管线各段的公共参数。content_hash
         * 复用宿主的哈希回调（与 TU 批的出队判定同一单点）；claim_lost 是
         * 引用出参——静默窗内声明被偷时置位，宿主的 peek 循环据此停轮，
         * 把队列交给偷取者。
         */
        struct ProjectRefreshArgs
        {
            cg::db::GraphStore& store;
            const Invocation&   inv;
            const std::string&  holder;
            std::int64_t        lease_ms;
            std::int64_t        refresh_debounce_ms;
            bool&               claim_lost;

            /** @brief 出队判定与哈希回调共用的内容哈希单点。 */
            const std::function<std::string(const std::string&)>& content_hash;
        };

        /**
         * @brief 无条件出队一组队列行（tracked=false 通道）。
         *
         * @internal 服务两类行：成功采集后的项目行（信号已被一次完整重
         * 采集消费）与联动无处可去的行（留着只会让 peek 永远返回它们）。
         * 与 TU 批的覆盖判定无关——那些行走 clear_covered 的快照复核
         * 通道。
         */
        auto dequeue_untracked(
            const ProjectRefreshArgs&       args,
            const std::vector<std::string>& which) -> void
        {
            std::vector<cg::db::PendingClearInput> clear;
            clear.reserve(which.size());
            for (const std::string& f : which)
            {
                clear.push_back(
                    cg::db::PendingClearInput{.path = f, .tracked = false});
            }
            args.store.clear_covered(
                clear,
                args.content_hash,
                args.holder,
                args.lease_ms);
        }

        /**
         * @brief 失败镜像重入队：把派生前已出队的项目行原样放回队列。
         *
         * @internal 保住"下一次事件重试"的联动语义——行在派生前删除
         * （见 spawn_capture_and_settle 的事务次序），失败路径把它们送
         * 回来。入队失败（库锁死等超时）时行已不可恢复地离开队列：如实
         * 告警，用户下一次保存该文件即自愈。
         */
        auto requeue_project_lines(
            const ProjectRefreshArgs&       args,
            const std::vector<std::string>& lines) -> void
        {
            try
            {
                args.store.enqueue_pending(lines);
            }
            catch (const std::exception&)
            {
                std::println(
                    std::cerr,
                    "ccg update: could not re-queue the project line{} after "
                    "the failed refresh",
                    lines.size() == 1 ? "" : "s");
            }
        }

        /**
         * @brief 把 peek 出的项目文件行映射到应重采集的配置目标。
         *
         * @internal 逐行调用 project_refresh_target_for（联动判定的唯一
         * 收口）：命中的行合并进一次重采集（同一配置），未命中的行（根
         * 外属性文件等）由调用方按未跟踪出队。目标取首个命中——多行命中
         * 同一配置目录时一次采集覆盖全部。
         *
         * @param registry 已加载的注册表。
         * @param project_root 项目根（正斜杠、规范化）。
         * @param inv 调用面（配置名参与注册表条目匹配）。
         * @param lines peek 出的项目文件行。
         * @param matched 命中的行（出参，按输入序）。
         * @param unmatched 未命中的行（出参，按输入序）。
         *
         * @return 命中时给出目标配置；全部未命中时为 nullopt。
         */
        [[nodiscard]]
        auto match_refresh_lines(
            const cg::msbuild::Registry&    registry,
            const std::string&              project_root,
            const Invocation&               inv,
            const std::vector<std::string>& lines,
            std::vector<std::string>&       matched,
            std::vector<std::string>&       unmatched)
            -> std::optional<cg::msbuild::ProjectRefreshTarget>
        {
            std::optional<cg::msbuild::ProjectRefreshTarget> target;
            for (const std::string& line : lines)
            {
                auto candidate = cg::msbuild::project_refresh_target_for(
                    registry,
                    project_root,
                    inv.config_name,
                    line);
                if (candidate.has_value())
                {
                    matched.push_back(line);
                    if (!target.has_value())
                    {
                        target = std::move(candidate);
                    }
                }
                else
                {
                    unmatched.push_back(line);
                }
            }
            return target;
        }

        /**
         * @brief 静默窗去抖：等队列静默或到达硬上限后再发起重采集。
         *
         * @internal 期间的新行入队会重置窗口（IDE 连发保存在窗口内合并
         * 进同一次采集），租约照常心跳；30 秒硬上限后照发（宁可多采集，
         * 绝不丢事件）。心跳 peek 发现声明被偷时置 args.claim_lost 并返
         * 回 false——队列归偷取者，调用方以 "yielded" 让外层停轮。
         *
         * @return true = 窗口已过，可继续联动；false = 声明被偷，须让位。
         */
        [[nodiscard]]
        auto wait_quiet_window(
            const ProjectRefreshArgs&       args,
            const std::vector<std::string>& lines) -> bool
        {
            const auto window_start = std::chrono::steady_clock::now();
            auto       quiet_since = window_start;
            std::vector<std::string> queue_face = lines;
            for (;;)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - window_start >= std::chrono::seconds{30}
                    || now - quiet_since >= std::chrono::milliseconds{
                           args.refresh_debounce_ms})
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
                const cg::db::PendingPeek beat =
                    args.store.peek_pending(args.lease_ms, args.holder);
                if (beat.claim_lost)
                {
                    std::println(
                        std::cerr,
                        "ccg update: claim stolen during the refresh "
                        "debounce - stopping; the new holder continues");
                    args.claim_lost = true;
                    return false;
                }
                if (beat.files != queue_face)
                {
                    quiet_since = now;
                    queue_face = beat.files;
                }
            }
            return true;
        }

        /**
         * @brief 哈希门：求值输入快照与当前输入全等则本次事件可被现有
         *        语料吸收。
         *
         * @internal 与求值通道成对，插在静默窗之后、capture 锁探测之前：
         * 重采前先问"求值输入变没变"。快照由成功求值采集的尾部写回
         * （import 闭包是求值腿的免费产物）；读侧全等 = 本次事件已被现有
         * 语料覆盖——吸收行、跳过重采（IDE 保存未落实质变化是联动事件的
         * 大多数形态）。任何不等（无快照/文件哈希变/环境身份变/坏行）按
         * "变"处理，走既有采集联动。msbuild/cl 的"当前值"口径 = 下一轮
         * 采集的发现缓存会复用的注册表路径——与采集侧同一决策面，不会出
         * 现"门说变、采集却复用旧路径"的门-采集失配循环。
         *
         * @return true = 全等（调用方吸收行并跳过重采）；false = 有差异
         *         或无快照（继续既有采集联动）。
         */
        [[nodiscard]]
        auto refresh_snapshot_absorbs(
            const ProjectRefreshArgs&                args,
            const cg::msbuild::Registry&             registry,
            const std::string&                       project_root,
            const cg::msbuild::ProjectRefreshTarget& target) -> bool
        {
            const auto snapshot = cg::msbuild::eval::load_refresh_snapshot(
                cg::msbuild::eval::refresh_snapshot_path(
                    project_root,
                    target.stem));
            if (!snapshot.has_value())
            {
                return false;
            }
            const auto entry = std::ranges::find(
                registry.entries,
                target.stem,
                &cg::msbuild::RegistryEntry::name);
            if (entry == registry.entries.end())
            {
                return false;
            }
            const std::string mismatch =
                cg::msbuild::eval::refresh_snapshot_unchanged(
                    *snapshot,
                    entry->msbuild,
                    entry->cl,
                    cg::msbuild::eval::vc_targets_dir_of(
                        cg::msbuild::eval::vs_root_of(entry->msbuild)));
            if (mismatch.empty())
            {
                if (args.inv.verbose)
                {
                    std::println(
                        std::cerr,
                        "ccg update: project refresh skipped - "
                        "evaluation inputs unchanged since the last capture "
                        "({} file{} watched)",
                        snapshot->files.size(),
                        snapshot->files.size() == 1 ? "" : "s");
                }
                return true;
            }
            if (args.inv.verbose)
            {
                std::println(
                    std::cerr,
                    "ccg update: refresh snapshot mismatch - {}",
                    mismatch);
            }
            return false;
        }

        /** @brief 派生采集的联动结论。 */
        struct CaptureSettle
        {
            /** @brief 联动结论串："busy" / "failed" / "captured"。 */
            std::string outcome;
            /** @brief 成功采集置位：语料与跟踪集需调用方重载。 */
            bool corpus_stale = false;
        };

        /**
         * @brief 派生同步采集子进程并按结局分账（出队 → 让位 → 派生）。
         *
         * @internal 事务次序（不可重排）：①触发即出队（宁可重入队也不丢
         * 事件）——项目行在派生前删除、任何失败路径原样入队回来。两个理
         * 由：采集子进程的索引尾看到非空队列会派生自己的 --drain 收敛进
         * 程（coord_drain_pending），那个进程会抢走声明并对同一行再跑一
         * 轮幂等采集——空队列让每个事件恰好一轮采集；子进程退出后不再
         * 需要重取声明（声明已让位，外层 peek 以 claim_lost 自然停轮，
         * 队列的未来归下一个事件）。出队到派生之间的进程崩溃是毫秒级残
         * 余窗口（与既有"读哈希 → DELETE"窗口同级），用户再保存一次即
         * 自愈。②让位声明——采集子进程的索引提交段按 update_claim 排队
         * 等待（Coordination::wait 默认开），持有者不放手必然互等；守卫
         * 仍然在场：析构时的二次释放按持有者匹配，删不到别人的行。③派生
         * 同步采集（同步等待、超时放手不杀）——成功与否由退出码判定，失
         * 败保旧由 msbuild 侧兜底（非 exec 类失败不写 cdb、库一行不动、
         * 退出 1），本侧只决定队列行的去留。
         *
         * @param matched 命中的项目行（在本函数内出队，失败时重入队）。
         *
         * @return 联动结论："busy"（未启动/超时让行，行已重入队）、
         *         "failed"（退出码非零，行已重入队、库保旧）、
         *         "captured"（成功，行已消费，声明已让位；corpus_stale
         *         置位，语料与跟踪集的重载由调用方执行——那是宿主的语
         *         料重载设施）。
         */
        [[nodiscard]]
        auto spawn_capture_and_settle(
            const ProjectRefreshArgs&                args,
            const cg::msbuild::ProjectRefreshTarget& target,
            const std::vector<std::string>&          matched) -> CaptureSettle
        {
            dequeue_untracked(args, matched);
            args.store.release_update(args.holder);

            const fs::path ccg = cg::util::platform::executable_path();
            const std::vector<std::string> capture_args{
                "msbuild",
                target.solution,
                "--config",
                target.configuration,
                "--platform",
                target.platform,
                "-o",
                args.inv.layout_root};
            const auto refresh_start = std::chrono::steady_clock::now();
            const cg::util::platform::TimedExecResult child =
                cg::util::platform::capture_exec_timed(
                    ccg,
                    capture_args,
                    std::chrono::minutes{30});
            const auto refresh_secs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - refresh_start)
                    .count();
            if (!child.started)
            {
                requeue_project_lines(args, matched);
                std::println(
                    std::cerr,
                    "ccg update: could not spawn the capture subprocess - the "
                    "queued project line{} stay{} for the next event",
                    matched.size() == 1 ? "" : "s",
                    matched.size() == 1 ? "s" : "");
                return {.outcome = "busy"};
            }
            if (args.inv.verbose || child.exit_code != 0)
            {
                std::println(
                    std::cerr,
                    "ccg update: project refresh (ccg msbuild {} --config {} "
                    "--platform {}) exited {} in {} ms",
                    target.solution,
                    target.configuration,
                    target.platform,
                    child.exit_code,
                    refresh_secs);
            }
            if (child.timed_out)
            {
                requeue_project_lines(args, matched);
                std::println(
                    std::cerr,
                    "ccg update: project refresh still running after 30 min "
                    "- detached; the queued line{} stay{} for the next event",
                    matched.size() == 1 ? "" : "s",
                    matched.size() == 1 ? "s" : "");
                return {.outcome = "busy"};
            }
            if (child.exit_code != 0)
            {
                requeue_project_lines(args, matched);
                std::println(
                    std::cerr,
                    "ccg update: project refresh failed (exit {}); the "
                    "database keeps the previous corpus - the queued line{} "
                    "stay{} for the next event",
                    child.exit_code,
                    matched.size() == 1 ? "" : "s",
                    matched.size() == 1 ? "s" : "");
                return {.outcome = "failed"};
            }

            // 成功：新 TU 已由子进程的索引尾增量入库。声明已让位——不再
            // 重取，外层 peek 以 claim_lost 停轮，把队列的未来（窗口内的
            // TU 编辑等）交给下一个事件/持有者。语料重载由调用方按
            // corpus_stale 执行：若下一轮 peek 恰好抢回声明，未跟踪判定
            // 必须基于新语料。
            std::println(
                std::cerr,
                "ccg update: project files re-captured - corpus refreshed "
                "({} file{}, {} ms)",
                matched.size(),
                matched.size() == 1 ? "" : "s",
                refresh_secs);
            return {.outcome = "captured", .corpus_stale = true};
        }

        /** @brief 项目联动管线的结论。 */
        struct ProjectRefreshOutcome
        {
            /** @brief 联动结论串（六值协议，见 refresh_project_files）。 */
            std::string outcome;
            /** @brief 成功采集置位：语料与跟踪集需调用方重载。 */
            bool corpus_stale = false;
        };

        /**
         * @brief 项目文件行的重采集联动管线（编排入口）。
         *
         * @internal 注册表反查（平台中立门）→ 静默窗去抖 → 哈希门（求值
         * 输入全等即吸收）→ capture 锁探测 → 触发即出队 → 让位声明 →
         * 派生 ccg msbuild（其内部自带 run_index_tail 增量索引与失败保
         * 旧）；失败把行重新入队。返回联动结论（六值逐字是协议，JSON/
         * 注入面按串分类）：
         * - "captured"：重采集完成，行已消费；声明已让位，外层 peek 以
         *   claim_lost 停轮；corpus_stale 置位，调用方据此重载语料与
         *   跟踪面；
         * - "skipped-unchanged"：哈希门吸收——快照与当前输入全等，行出
         *   队、不派生采集，声明未让；
         * - "untracked"：联动无处可去，行按未跟踪出队；
         * - "busy"：行保留待下一事件（互斥锁活/子进程未启动/超时让行）；
         * - "failed"：采集失败，行重新入队，库保旧态；
         * - "yielded"：静默窗内声明被偷，claim_lost 已置，外层停轮。
         *
         * @param lines peek 出的项目文件行。
         *
         * @return 联动结论（六值之一，见上）。
         */
        [[nodiscard]]
        auto refresh_project_files(
            const ProjectRefreshArgs&       args,
            const std::vector<std::string>& lines) -> ProjectRefreshOutcome
        {
            // 注册表反查的第一权威是库 meta 的 project_root（msbuild 索
            // 引运行写入 sln 所在目录）；无根/无注册表 = 联动无处可去，
            // 按未跟踪出队避免队列永久堆积（非 Windows 天然走此分支——
            // 注册表从不存在，项目文件事件不触发任何采集）。
            const auto        recorded_root = args.store.meta("project_root");
            const std::string project_root =
                recorded_root.has_value() ? cg::util::norm_path(*recorded_root)
                                          : std::string{};
            if (project_root.empty())
            {
                std::println(
                    std::cerr,
                    "ccg: note: {} project file{} seen but the database "
                    "records no project root - treated as untracked (nothing "
                    "to refresh)",
                    lines.size(),
                    lines.size() == 1 ? "" : "s");
                dequeue_untracked(args, lines);
                return {.outcome = "untracked"};
            }
            const std::string registry_path =
                project_root + "/.ccg/msbuild.json";
            const auto registry = cg::msbuild::load_registry(registry_path);
            if (!registry.has_value())
            {
                std::println(
                    std::cerr,
                    "ccg: note: {} project file{} seen but no msbuild "
                    "registry exists at {} - treated as untracked (nothing "
                    "to refresh)",
                    lines.size(),
                    lines.size() == 1 ? "" : "s",
                    registry_path);
                dequeue_untracked(args, lines);
                return {.outcome = "untracked"};
            }
            std::vector<std::string> matched;
            std::vector<std::string> unmatched;
            const auto               target = match_refresh_lines(
                *registry,
                project_root,
                args.inv,
                lines,
                matched,
                unmatched);
            if (!unmatched.empty())
            {
                std::println(
                    std::cerr,
                    "ccg: note: {} project file{} outside the registered "
                    "solution tree - treated as untracked",
                    unmatched.size(),
                    unmatched.size() == 1 ? "" : "s");
                dequeue_untracked(args, unmatched);
            }
            if (!target.has_value())
            {
                return {.outcome = "untracked"};
            }
            if (args.inv.verbose)
            {
                std::println(
                    std::cerr,
                    "ccg update: project file{} -> re-capture {} ({}|{})",
                    matched.size() == 1 ? "" : "s",
                    target->stem,
                    target->configuration,
                    target->platform);
            }
            if (!wait_quiet_window(args, lines))
            {
                return {.outcome = "yielded"};
            }
            if (refresh_snapshot_absorbs(
                    args,
                    *registry,
                    project_root,
                    *target))
            {
                dequeue_untracked(args, matched);
                return {.outcome = "skipped-unchanged"};
            }

            // capture 互斥只读探测：真锁由采集执行方（ccg msbuild）获取
            // 并持有到翻译/索引/注册表全部完成；这里只问"是否有人在跑"。
            const std::string capture_lock_path = project_root
                                                  + "/.ccg/msbuild/capture-"
                                                  + target->stem + ".lock";
            if (cg::util::platform::file_lock_held_by_live_process(
                    fs::path{capture_lock_path},
                    900'000))
            {
                std::println(
                    std::cerr,
                    "ccg update: project refresh deferred - another capture "
                    "is running for {} (lock {} is live); the queued line{} "
                    "stays for the next event",
                    target->stem,
                    capture_lock_path,
                    matched.size() == 1 ? "" : "s");
                return {.outcome = "busy"};
            }
            const CaptureSettle settled =
                spawn_capture_and_settle(args, *target, matched);
            return {
                .outcome = std::move(settled.outcome),
                .corpus_stale = settled.corpus_stale};
        }

        /**
         * @brief `ccg update` 运行器。
         *
         * @internal
         * 一条连接管所有事：下方的短协调事务与引擎自己的批次事务在其上交错
         * （WAL 加武装的 busy timeout 使并发写者按序等待而非失败）。协调旋钮
         * （逐库 meta，`ccg config set`；schema 默认）：租约须超过最坏情况的
         * 单个批次（循环在批次间心跳），busy timeout 须等得过持有写锁的并发
         * 引擎批次。入队：一个短事务（INSERT .. DO NOTHING——同路径的并发
         * 入队是同一工作，PRIMARY KEY 折叠它）；busy timeout 覆盖并发引擎
         * 批次。取声明：一个短 IMMEDIATE 事务；落败不是等待——持有者把队列
         * 排空到空为止，该工作无论如何都会发生——毫秒之后、在获胜的进程里。
         * 持有者路径的语料身份来自数据库 meta（上次索引运行记录的编译数据
         * 库与项目根）；--compile-commands 只为本次运行覆盖编译数据库。无
         * 记录根（meta 记账之前的库、或内存草稿库）时从编译数据库目录（否则
         * 库自身目录）推导根。没有记录编译数据库的库与 `ccg index` 同样报错：
         * 更新无从知道语料旗标，绝不扫描兜底。引擎解析与索引尾部完全一致：
         * resolve_loaded_commands 为经典语料返回未准备引擎（只有模块路由顺路
         * 准备一个），故在此按文档化优先级（库钉 >
         * --llvm-root > $CCG_LLVM_ROOT > PATH）加载——在驱动器首次被询问
         * 之前，与 run_index_tail 相同。引擎纪律同索引尾部：开放存储的权威
         * 钉胜出，引擎身份变化强制全覆盖（世界观变化 = 无增量捷径）。折叠的
         * 被跟踪文件视图用于未跟踪种子提示（既非编译数据库主文件也非已索引
         * 文件的种子只能是尚无人包含的全新文件或笔误——点名它；这类条目在
         * 批后直接出队）。持有者循环（租约心跳循环）：每次迭代一个短
         * IMMEDIATE 事务（租约心跳 + peek 全部 pending_updates，不删除），
         * 随后在任何协调事务之外跑引擎批次；首次 peek 折入自己的文件——它们
         * 与任何人的入队方式相同，故恰有一条刷新路径。批次结束后的
         * clear_covered 是唯一出队点：覆盖集 ∩ 快照哈希未变。租约运行中途
         * 到期被偷时立即停：偷取者拥有队列，已落地的批次是幂等引擎运行。
         * 守卫在每个退栈路径释放声明（仅自己的行）。
         * 项目文件联动（实时变更队列协调）：peek 出的 .sln/.vcxproj/.props
         * 等行不走 TU 解析——经注册表反查（库 meta project_root +
         * .ccg/msbuild.json，在场才联动，天然平台中立）触发该配置的重采集：
         * 静默窗去抖（msbuild_refresh_debounce_ms）→ capture 锁探测 → 触发即
         * 出队 → 让位声明 → 派生 ccg msbuild（内部自带增量索引尾与失败保旧，
         * 空队列使它不再派生自己的收敛 drain）；失败把行重新入队，下一个事
         * 件重试。旧库永远是上一个完整好态，绝无半库。
         */
        inline auto run_update(const Invocation& inv) -> int
        {
            if (inv.db_path.empty())
            {
                throw std::runtime_error(
                    "ccg update needs a resolved database (dispatch resolves the "
                    "layout; a memory database cannot be coordinated across "
                    "processes)");
            }
            std::error_code ec;
            if (!fs::is_regular_file(fs::path{inv.db_path}, ec) || ec)
            {
                throw std::runtime_error(
                    std::format(
                        "cannot update database '{}': file not found (run `ccg index` "
                        "first; update refreshes an indexed database)",
                        inv.db_path));
            }
            // 跨库并发时"看到一个 ccg.exe 不知道它在更新谁"的失认问题：
            // 启动即亮明目标库（stderr 不进注入信封——钩子派生路径的
            // stderr 一律经 capture_exec_timed 的临时文件，见派生点注释）。
            std::println(
                std::cerr,
                "ccg update: target database: {}",
                inv.db_path);
            const std::vector<std::string> own =
                normalize_update_files(inv.positional);

            cg::db::GraphStore store = open_store_create(inv);

            const auto knob = [&](const char* key, std::int64_t fallback)
            { return cg::util::parse_bounded(store.meta(key), fallback); };
            const std::int64_t lease_ms =
                knob("update_lease_ms", cg::db::default_update_lease_ms);
            store.set_busy_timeout_ms(knob(
                "update_busy_timeout_ms",
                cg::db::default_update_busy_timeout_ms));
            // 项目文件联动的静默窗（见 refresh_project_files 的静默窗段）。
            const std::int64_t refresh_debounce_ms = knob(
                "msbuild_refresh_debounce_ms",
                cg::db::default_msbuild_refresh_debounce_ms);

            if (!own.empty())
            {
                store.enqueue_pending(own);
            }

            const std::string holder = update_claim_token();
            // 本进程协调事件的读侧下界（--json 的 coordEvents 只报本会话
            // 触发的 steal/rollback/orphan-recover/run-*）。
            const std::int64_t coord_since_ms = cg::util::cstd::epoch_ms();
            // run-start（每次运行一条；hook 派生与 detach 续跑的同一进程
            // 也在此记账）；actor 用 claim 记号使 update --json 的会话过滤
            // 天然包含这对事件。
            detail::RunAuditGuard
                run_audit{store, holder, "update", inv.db_path, inv.positional};
            if (!store.claim_update(lease_ms, holder))
            {
                std::println(
                    std::cerr,
                    "ccg update: a live claim holds the database - {} "
                    "file{} queued (the holder drains the queue)",
                    own.size(),
                    own.size() == 1 ? "" : "s");
                if (inv.json)
                {
                    json::Object o;
                    o.set("queued", static_cast<std::int64_t>(own.size()));
                    o.set("claimed", false);
                    std::println("{}", json::stringify(std::move(o)));
                }
                run_audit.finish(
                    "queued",
                    0,
                    static_cast<std::int64_t>(own.size()),
                    "a live claim holds the database");
                return 0;
            }
            const UpdateClaimGuard claim_guard{store, holder};

            // 测试注入（生产零开销：无环境变量即什么都不做）：仅持声明睡 N
            // 毫秒、绝不碰写锁——复现"活租约持有者无在飞批"的现场，让写命令
            // 的启动接管（偷取后排队的队列形态）与钩子派生判定成为可断言的
            // 事实，而不是靠撞引擎批的时序。标记文件供测试轮询确认声明已到手。
            if (const long long claim_ms = cg::util::parse_bounded(
                    cg::util::cstd::get_env("CCG_TEST_CLAIM_HOLD_MS"),
                    0);
                claim_ms > 0)
            {
                if (const auto marker =
                        cg::util::cstd::get_env("CCG_TEST_CLAIM_MARKER"))
                {
                    std::ofstream out{
                        *marker,
                        std::ios::binary | std::ios::trunc};
                    if (out)
                    {
                        out << "claimed\n";
                    }
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds{claim_ms});
                return 0;
            }

            // 测试注入（生产零开销：无环境变量即什么都不做）：持有一个真实的
            // 写事务，精确复现"批次正持有 SQLite 写锁"的现场，让钩子快速路径
            // 的 150 ms 忙超时与 journal 旁路成为可断言的事实，而不是靠运气
            // 撞上引擎批处理的时序。标记文件供测试轮询确认声明已到手。
            if (const long long hold_ms = cg::util::parse_bounded(
                    cg::util::cstd::get_env("CCG_TEST_HOLD_WRITE_LOCK_MS"),
                    0);
                hold_ms > 0)
            {
                cg::db::GraphStore::Tx writer{store, /*immediate=*/true};
                if (const auto marker =
                        cg::util::cstd::get_env("CCG_TEST_CLAIM_MARKER"))
                {
                    std::ofstream out{
                        *marker,
                        std::ios::binary | std::ios::trunc};
                    if (out)
                    {
                        out << "locked\n";
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{hold_ms});
                writer.rollback();
                return 0;
            }
            if (const auto marker =
                    cg::util::cstd::get_env("CCG_TEST_CLAIM_MARKER"))
            {
                std::ofstream out{*marker, std::ios::binary | std::ios::trunc};
                if (out)
                {
                    out << "claimed\n";
                }
            }

            std::string              db_file = inv.compile_db;
            std::vector<std::string> roots;
            if (db_file.empty())
            {
                db_file = store.meta("compile_db_path").value_or(std::string{});
            }
            if (const auto root = store.meta("project_root"))
            {
                roots.push_back(*root);
            }
            if (roots.empty())
            {
                // meta 记账以前的库：配置档案（若有）是根的下一权威；再落空才退
                // 回锚推导（编译数据库目录，否则库的配置目录——布局改版后后
                // 者是布局文件夹、离项目根最远，故仅作末位兜底）。
                const auto profile =
                    layout::load_profile(layout::config_dir_of_db(inv.db_path));
                if (profile.has_value() && !profile->roots.empty())
                {
                    roots = profile->roots;
                }
                else
                {
                    const std::string anchor =
                        !db_file.empty() ? db_file : inv.db_path;
                    roots.push_back(
                        cg::util::norm_path(
                            fs::path{anchor}
                                .parent_path()
                                .lexically_normal()
                                .generic_string()));
                }
            }
            if (db_file.empty())
            {
                throw std::runtime_error(missing_compile_db_message(roots));
            }
            if (!fs::is_regular_file(fs::path{db_file}, ec) || ec)
            {
                throw std::runtime_error(
                    std::format(
                        "the compile database recorded for '{}' ({}) is missing - re-run "
                        "`ccg index` (or pass --compile-commands) so the refresh can see "
                        "the corpus flags",
                        inv.db_path,
                        db_file));
            }

            // 拆分库只刷新自己的配置：库 meta 的 build_config 签名过滤
            // 全量编译数据库，其它配置的条目绝不混进本库。
            const std::string build_config =
                store.meta("build_config").value_or(std::string{});
            ResolvedCommands resolved;
            // 语料加载：项目文件联动会在持有者中途重写 compile_commands.json
            // （新 TU 进、移除 TU 出），本段必须可重跑——首次加载与每次成功
            // 重采集后各跑一遍。引擎身份不在重跑面内：解析世界观在运行中途不
            // 变，重载只换命令面。
            const auto load_corpus = [&]()
            {
                std::vector<cg::clang::CompileCommand> loaded =
                    cg::clang::load(db_file, roots);
                std::vector<cg::clang::CompileCommand> coverage = loaded;
                ResolvedCommands fresh = resolve_loaded_commands(
                    inv,
                    roots,
                    filter_by_build_config(std::move(loaded), build_config),
                    coverage,
                    db_file);
                if (fresh.commands.empty())
                {
                    throw std::runtime_error(
                        std::format(
                            "no compile database entries to update under {} (the "
                            "recorded corpus is empty - re-run `ccg index`)",
                            roots.front()));
                }
                resolved = std::move(fresh);
            };
            load_corpus();

            EnginePrepared engine = resolved.engine;
            if (!engine.valid)
            {
                engine = prepare_index_engine(inv, read_db_engine_pin(inv));
            }

            auto [synced_engine, engine_forced_full] =
                sync_engine_with_store(store, inv, std::move(engine));
            engine = std::move(synced_engine);
            require_driver();

            // 折叠比较走 util 的比较规范化单点（输入已是存储规范化路径，
            // 分隔符折叠为空操作，集合语义不变）。
            const auto fold = [](std::string_view path)
            { return cg::util::path_compare_key(path); };
            // 折叠的被跟踪文件视图与语料同寿：重采集换掉命令面后一并重建。
            std::unordered_set<std::string> tracked_fold;
            const auto                      rebuild_tracked = [&]()
            {
                tracked_fold.clear();
                for (const cg::clang::CompileCommand& c : resolved.commands)
                {
                    tracked_fold.insert(fold(c.file));
                }
                for (const cg::db::FileRow& fr : cg::db::all_files(store))
                {
                    tracked_fold.insert(fold(fr.path));
                }
            };
            rebuild_tracked();

            const auto content_hash = [](const std::string& path) -> std::string
            {
                try
                {
                    return cg::util::sha256_file_hex(path);
                }
                catch (const std::exception&)
                {
                    // 缺失/不可读：记为 EMPTY。批前与批后都读不到即视为"未
                    // 再变化"（删除事件本身已被引擎处理）；若是批后消失或
                    // 批后才出现，空值会与快照不同而保留。
                    return {};
                }
            };

            cg::graph::IndexSummary last;
            std::int64_t            runs = 0;
            bool                    claim_lost = false;
            // 重捕获后的重查轮标记（文件级门第二段）：仅该轮的未跟踪
            // 出队写 enqueue-reject 审计（detail 带 recheck 标记）；一轮
            // 批后清零。
            bool recheck_after_capture = false;
            // 本轮联动结论（JSON/注入消费）："captured"（重采集+增量索引进库）、
            // "skipped-unchanged"（哈希门吸收：输入与上次采集全等，行出队、
            // 不派生采集）、"busy"（互斥锁活/子进程未启动/超时让行，行保留
            // 待下一事件）、"failed"（采集失败保旧，行保留待下一事件）。空
            // = 本轮没触发。
            std::string project_refresh_state;

            // 项目文件行重采集联动的参数面（管线本体见 detail 的
            // refresh_project_files 函数族）：一次构造、循环内复用；哈希
            // 回调与 TU 批的出队判定同一单点，claim_lost 为引用出参。
            const std::function<std::string(const std::string&)> refresh_hash =
                content_hash;
            const ProjectRefreshArgs refresh_args{
                store,
                inv,
                holder,
                lease_ms,
                refresh_debounce_ms,
                claim_lost,
                refresh_hash};

            for (;;)
            {
                const cg::db::PendingPeek peek =
                    store.peek_pending(lease_ms, holder);
                if (peek.claim_lost)
                {
                    std::println(
                        std::cerr,
                        "ccg update: claim stolen (lease expired) - "
                        "stopping; the new holder continues the queue");
                    claim_lost = true;
                    break;
                }
                if (peek.files.empty())
                {
                    break;
                }
                // 分类：项目文件行（.vcxproj/.sln/.props/...）改的是"语料从哪
                // 来"，不进 TU 解析批——交给重采集联动；TU 行照旧走增量引擎
                // 批。两类行可能同时在场（保存项目文件后紧跟保存源文件），TU
                // 批先跑，联动收尾后 peek 循环把窗口内的后续编辑收敛掉。
                std::vector<std::string> tu_files;
                std::vector<std::string> project_files;
                for (const std::string& f : peek.files)
                {
                    (cg::util::is_project_file(f) ? project_files : tu_files)
                        .push_back(f);
                }
                if (!tu_files.empty())
                {
                    if (inv.verbose)
                    {
                        std::println(
                            std::cerr,
                            "ccg update: refreshing {} file{}",
                            tu_files.size(),
                            tu_files.size() == 1 ? "" : "s");
                    }
                    // 批前快照：只对真正会被引擎处理的（被跟踪）路径记内容哈
                    // 希。未跟踪条目无论如何都从队列删除，无需付哈希成本。
                    std::unordered_map<std::string, std::string> snapshot;
                    for (const std::string& f : tu_files)
                    {
                        if (tracked_fold.contains(fold(f)))
                        {
                            snapshot.emplace(f, content_hash(f));
                        }
                    }
                    if (const std::ptrdiff_t untracked = std::ranges::count_if(
                            tu_files,
                            [&](const std::string& f)
                            { return !tracked_fold.contains(fold(f)); });
                        untracked > 0)
                    {
                        std::println(
                            std::cerr,
                            "ccg: note: {} seeded file{} not tracked by the database "
                            "or the compile commands - nothing to refresh for them",
                            untracked,
                            untracked == 1 ? "" : "s");
                    }
                    cg::graph::IndexOptions opt;
                    opt.roots = roots;
                    opt.force = engine_forced_full;
                    opt.progress = inv.verbose;
                    opt.compile_db_path = db_file;
                    opt.db_path = inv.db_path;
                    // 覆盖链同 index：--jobs 旗标（update 已放开）> 库配置
                    // index_jobs > 自动按核；持有者连接已在手，直接读 meta。
                    opt.jobs = resolve_jobs(inv, &store);
                    opt.seed_files = tu_files;
                    // 批内让位点（租约被偷即让位）：引擎批的提交段按本持有者的
                    // 记号做事务内心跳——声明被偷（写命令启动接管等）时抛
                    // ClaimStolenError 干净回滚，本循环据此优雅让位，绝不与
                    // 偷取者并存写库。批间点（peek_pending）与批后点
                    // （clear_covered）的让位是既有设计，此处补齐的是批内点。
                    opt.caller_holder = holder;
                    opt.caller_lease_ms = lease_ms;
                    // 挪时机设计：带文件参数的快速批只做阶段 A + 按文件删 vd 边
                    // （毫秒-秒级返回，编辑体感即阶段 A）；--drain 的批次同样快
                    // 速，队列排空后统一跑一次收敛批（见循环尾部）。RTA/类型
                    // 传播/dispatch 在快速批里不跑，meta dispatch_stale
                    // 显式记录滞后。
                    opt.defer_dispatch = !inv.drain;
                    cg::graph::Indexer indexer{store, std::move(opt)};
                    try
                    {
                        last = indexer.run(resolved.commands);
                    }
                    catch (const cg::graph::ClaimStolenError&)
                    {
                        // 引擎批提交段发现声明被偷：整批已干净回滚，队列行原样
                        // 保留（clear_covered 未跑），偷取者拥有队列的未来。
                        std::println(
                            std::cerr,
                            "ccg update: claim stolen mid-batch - "
                            "stopping; the thief owns the queue");
                        // rollback 审计在回滚**之后**新开短事务写（回滚救不
                        // 回它没包含的东西，独立事务的审计天然幸存——"回滚
                        // 也不丢审计"）。对方记号补一次只读快照（=偷取者）；
                        // 任何失败静默：审计绝不把优雅让位变成异常退出。
                        try
                        {
                            std::string                 thief;
                            std::int64_t                thief_pid = 0;
                            const cg::db::ClaimSnapshot now_snap =
                                store.claim_snapshot();
                            if (now_snap.has_claim)
                            {
                                thief = now_snap.holder;
                                thief_pid = now_snap.pid;
                            }
                            (void)store.log_coord_event(
                                cg::db::CoordEventInput{
                                    .event = cg::db::CoordEvent::Rollback,
                                    .actor_holder = holder,
                                    .other_holder = thief,
                                    .actor_pid =
                                        cg::util::platform::current_pid(),
                                    .other_pid = thief_pid,
                                    .reason = "mid-batch-heartbeat",
                                    .detail =
                                        [&]
                                    {
                                        json::Object o;
                                        o.set(
                                            "batchTu",
                                            static_cast<std::int64_t>(
                                                tu_files.size()));
                                        o.set(
                                            "queue",
                                            static_cast<std::int64_t>(
                                                peek.files.size()));
                                        return json::stringify(std::move(o));
                                    }(),
                                });
                        }
                        catch (const std::exception&)
                        {
                        }
                        claim_lost = true;
                        break;
                    }
                    store.set_meta("llvm_engine_path", engine.path);
                    store.set_meta("llvm_version", engine.version);

                    // 批后出队：覆盖集 ∩ 快照哈希未变。未跟踪条目按既有 note
                    // 处理并从队列删除（否则 peek 永远返回它们）。
                    std::unordered_set<std::string> covered_fold;
                    covered_fold.reserve(last.covered_files.size());
                    for (const std::string& path : last.covered_files)
                    {
                        covered_fold.insert(fold(path));
                    }
                    std::vector<cg::db::PendingClearInput> clear_inputs;
                    clear_inputs.reserve(tu_files.size());
                    for (const std::string& f : tu_files)
                    {
                        cg::db::PendingClearInput input;
                        input.path = f;
                        const auto snap = snapshot.find(f);
                        if (snap == snapshot.end())
                        {
                            input.tracked = false; // 未跟踪：直接出队
                            if (recheck_after_capture)
                            {
                                // 重查轮的拒收：按新 cdb 仍不在语料面
                                // （enqueue-reject 审计，detail 带重查标记
                                // 与库路径——误投当场留痕）。
                                (void)store.log_coord_event(
                                    cg::db::CoordEventInput{
                                        .event =
                                            cg::db::CoordEvent::EnqueueReject,
                                        .actor_holder = std::format(
                                            "ccg-update:pid:{}",
                                            cg::util::platform::current_pid()),
                                        .reason = "not-in-corpus",
                                        .detail =
                                            [&]
                                        {
                                            json::Object o;
                                            o.set("recheck", true);
                                            o.set("path", f);
                                            return json::stringify(
                                                std::move(o));
                                        }(),
                                    });
                            }
                            clear_inputs.push_back(std::move(input));
                            continue;
                        }
                        input.tracked = true;
                        input.snapshot_hash = snap->second;
                        input.covered = covered_fold.contains(fold(f));
                        clear_inputs.push_back(std::move(input));
                    }
                    recheck_after_capture = false;
                    // 当前哈希不再在事务外预计算：clear_covered 在自己的
                    // IMMEDIATE 事务里按需重算，并在删除后复核、必要时立即重
                    // 新入队，从而封住"读哈希 → DELETE"之间的编辑窗口。心跳也
                    // 放在同一事务里：出队必须发生在仍持有租约的期间。
                    store.clear_covered(
                        clear_inputs,
                        content_hash,
                        holder,
                        lease_ms);
                    ++runs;
                }
                if (!project_files.empty())
                {
                    const detail::ProjectRefreshOutcome project_refresh =
                        refresh_project_files(refresh_args, project_files);
                    project_refresh_state = project_refresh.outcome;
                    // 行仍在队列（busy/failed 重新入队）时停轮：继续 peek 只会
                    // 对同一行再触发一轮采集；声明已让出（yielded/captured 的
                    // 让位路径）时下一个 peek 自然以 claim_lost 停轮。所有停轮
                    // 路径都把收敛交给下一个事件（宁可多跑一次，绝不丢更新）。
                    if (project_refresh_state == "busy"
                        || project_refresh_state == "failed"
                        || project_refresh_state == "yielded")
                    {
                        break;
                    }
                    if (project_refresh_state == "captured")
                    {
                        // 采集成功换掉了命令面（corpus_stale）：语料与跟
                        // 踪集一并重载，重查轮的未跟踪判定基于新 cdb。
                        // 文件级门的第二段（重捕获后重查）：重采集成功后
                        // 声明已让位给采集子进程的索引尾；窗口期内入队的
                        // TU 行此刻必须按**新 cdb** 重判——新入工程的文件
                        // 此时已被 tracked_fold 认识（照常增量），仍不在
                        // 语料面的行在重查轮里拒收出队（enqueue-reject
                        // 审计），不再留给永不消化的队列。排队事件等重
                        // 捕获收尾的排序依赖就是这条重取声明的原子裁决：
                        // 抢回声明者获得重查权，抢不到者（下一个事件/
                        // 持有者）按同一语料结论处理。
                        if (project_refresh.corpus_stale)
                        {
                            load_corpus();
                            rebuild_tracked();
                        }
                        recheck_after_capture =
                            store.claim_update(lease_ms, holder);
                        if (!recheck_after_capture)
                        {
                            break;
                        }
                    }
                }
            }

            // 收敛批：--drain 排空队列后，若快速批留下 dispatch_stale 标注
            // 则重跑一次完整 RTA/类型传播/dispatch（延迟收敛就是又一个 update
            // 批次： 同一声明、同一租约协议，只是这一批不解析
            // TU）。收敛中的新编辑仍
            // 在队列里留给下一个事件——这里只把已提交行带回一致。声明被偷时
            // 绝不收敛：窃取者拥有队列与本库的下一步。
            if (inv.drain && !claim_lost)
            {
                std::int64_t stale_dispatch = 0;
                try
                {
                    stale_dispatch = cg::util::parse_bounded(
                        store.meta("dispatch_stale"),
                        0);
                }
                catch (const std::exception&)
                {
                }
                if (stale_dispatch > 0)
                {
                    if (inv.verbose)
                    {
                        std::println(
                            std::cerr,
                            "ccg update: converging deferred dispatch "
                            "analysis ({} file{} stale)",
                            stale_dispatch,
                            stale_dispatch == 1 ? "" : "s");
                    }
                    cg::graph::IndexOptions conv_opt;
                    conv_opt.roots = roots;
                    conv_opt.progress = inv.verbose;
                    conv_opt.db_path = inv.db_path;
                    const cg::graph::IndexSummary conv =
                        cg::graph::converge_dispatch(store, conv_opt);
                    if (runs == 0)
                    {
                        last = conv;
                        runs = 1; // 纯收敛也是一次可报告的运行
                    }
                    else
                    {
                        last.dispatch_edges += conv.dispatch_edges;
                        last.dispatch_fallback_sites =
                            conv.dispatch_fallback_sites;
                        last.analysis_rounds += conv.analysis_rounds;
                        last.analysis_work_items += conv.analysis_work_items;
                        last.external_rta_hints = conv.external_rta_hints;
                        last.analysis_budget_exhausted =
                            last.analysis_budget_exhausted
                            || conv.analysis_budget_exhausted;
                        last.dispatch_stale_files = conv.dispatch_stale_files;
                    }
                }
            }

            // run-finish 赶在输出之前显式收尾（带结果数据）——coordEvents
            // 数组因此能带上本会话的这对事件；被偷让位用 stolen 点名。
            run_audit.finish(
                claim_lost ? "stolen" : "done",
                0,
                runs > 0 ? static_cast<std::int64_t>(last.covered_files.size())
                         : 0,
                std::format(
                    "{} run{}, claim_lost={}",
                    runs,
                    runs == 1 ? "" : "s",
                    claim_lost));

            if (inv.json)
            {
                // 本会话协调事件（steal/rollback/orphan-recover，按本进程
                // 的 holder 记号过滤；enqueue-reject 的 actor 是钩子记号，
                // 属 hook status --json 的计数面）。
                json::Array coord;
                std::size_t coord_count = 0;
                try
                {
                    for (const cg::db::CoordEventRow& row :
                         store.coord_events_since(coord_since_ms))
                    {
                        if (row.actor_holder != holder
                            && row.other_holder != holder)
                        {
                            continue;
                        }
                        ++coord_count;
                        json::Object e;
                        e.set("event", row.event);
                        e.set("actorHolder", row.actor_holder);
                        e.set("otherHolder", row.other_holder);
                        e.set("reason", row.reason);
                        e.set("tsMs", row.ts_ms);
                        e.set("detail", row.detail);
                        coord.add(std::move(e));
                    }
                }
                catch (const std::exception&)
                {
                }
                if (runs > 0)
                {
                    std::println(
                        "{}",
                        render::index_json(
                            last,
                            project_refresh_state.empty()
                                ? std::nullopt
                                : std::optional<
                                      std::pair<std::string, std::string>>(
                                      std::pair<std::string, std::string>{
                                          "projectRefresh",
                                          project_refresh_state}),
                            std::make_optional(
                                std::make_pair(
                                    "coordEvents",
                                    std::move(coord)))));
                }
                else
                {
                    json::Object o;
                    o.set("updated", static_cast<std::int64_t>(0));
                    o.set("queued", static_cast<std::int64_t>(0));
                    if (!project_refresh_state.empty())
                    {
                        o.set("projectRefresh", project_refresh_state);
                    }
                    if (coord_count > 0)
                    {
                        o.set("coordEvents", std::move(coord));
                    }
                    std::println("{}", json::stringify(std::move(o)));
                }
            }
            else if (runs > 0)
            {
                std::print("{}", render::index_text(last, inv.db_path));
            }
            else if (!project_refresh_state.empty())
            {
                // 纯联动轮（本批没有 TU 行）：结论行代替索引摘要（细节各行
                // 已在联动内部按状态打印到 stderr）。
            }
            else
            {
                std::println(
                    std::cerr,
                    "ccg update: nothing to do (queue empty)");
            }
            return 0;
        }

    } // namespace detail

} // namespace cg::cli
