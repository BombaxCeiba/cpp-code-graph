module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.runners.engine
 *
 * @brief
 * 引擎与存储准备域：审计守卫、库打开（建/读）、编译旗标解析与应用、引擎预备、并发度解析、引擎与库同步。
 */
export module cg.cli.runners.engine;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.clang;
import cg.graph;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.layout;

export namespace cg::cli
{

    namespace detail
    {

        namespace json = cg::util::json;

        /**
         * @brief 一次索引运行解析所经由的引擎世界观。
         *
         * 在运行的第一个驱动器消费者之前解析并加载（模块自构建、干跑计划、
         * 索引器的补充都要询问驱动器，而驱动器锚定在已加载引擎上——钉/旗标
         * 优先级届时必须已定）。
         *
         * @internal
         * db_engine 是作出该选择时所依据的库钉，故存储打开后读到的权威钉
         * 能检测变化并重新准备。
         */
        struct EnginePrepared
        {
            bool        valid = false;
            std::string db_engine; // 选择所用的库 llvm-path 钉（"" = 无）
            std::string path;      // 已解析并加载的引擎库路径
            std::string version;   // 已加载引擎的 clang_getClangVersion
        };

        /** @internal 位置参数拼单行 argv 摘要：首元素直拼，其余前补空格。 */
        auto join_argv(const std::vector<std::string>& parts) -> std::string
        {
            std::string joined;
            for (const std::string& a : parts)
            {
                joined += joined.empty() ? a : std::format(" {}", a);
            }
            return joined;
        }

        /**
         * @brief 一次 update/index 运行的 run-start/run-finish 审计对。
         *
         * hook 派生子进程的 stderr 走自动删除的临时文件——detach 后台的
         * 运行对所有人（含 agent）都是黑洞；本守卫把"何时启动、跑了多久、
         * 结果如何"落进 coord_events，事后 `ccg hook status --json` 与
         * `ccg update --json` 一查便知。
         *
         * @internal 量控：每次运行恰好一对（非高频事件，老化随表）。正常
         * 收尾由调用方显式 finish（携带结果数据）；未显式收尾的退栈路径
         * （异常上抛、测试注入通道）由析构兜底记 reason="exit"（无结果
         * 数据）；进程被杀/断电连析构都不跑——那种悬挂 run-start 由下一
         * 个接管者在 claim_update 的 pid 探活判死后经
         * abandon_dangling_runs_tx_ 补记 reason=abandoned。dry-run（真
         * 干跑短路，只读不写库）与 :memory: 库不记。actor_holder 用运行
         * 者的 claim 记号——`ccg update --json` 的会话过滤
         * （actor/other == holder）因此天然包含这对事件。审计写失败静默。
         */
        struct RunAuditGuard
        {
            RunAuditGuard(
                cg::db::GraphStore& store,
                std::string         holder,
                std::string         role,
                std::string         db,
                std::string         argv)
                : store_(&store), holder_(std::move(holder)),
                  role_(std::move(role)), db_(std::move(db)),
                  argv_(std::move(argv))
            {
                // argv 摘要按 UTF-8 码点边界截断（detail 会经读侧进 JSON，
                // 硬切多字节序列会让 yyjson 写出整体失败）。
                if (argv_.size() > 160)
                {
                    std::size_t cut = 160;
                    while (cut > 0
                           && cg::util::utf8_is_continuation(argv_[cut]))
                    {
                        --cut;
                    }
                    argv_.resize(cut == 0 ? 160 : cut);
                }
                started_ = std::chrono::steady_clock::now();
                json::Object payload;
                payload.set("db", db_);
                payload.set("role", role_);
                payload.set("argv", argv_);
                (void)store_->log_coord_event(
                    cg::db::CoordEventInput{
                        .event = cg::db::CoordEvent::RunStart,
                        .actor_holder = holder_,
                        .actor_pid = cg::util::platform::current_pid(),
                        .reason = role_,
                        .detail = json::stringify(std::move(payload)),
                    });
            }
            /**
             * @brief 位置参数形构造：argv 摘要由位置参数以单空格拼成。
             *
             * @internal 首元素无前导空格、空清单拼出空串（口径与调用方
             * 此前的手拼逐字一致）。
             */
            RunAuditGuard(
                cg::db::GraphStore&             store,
                std::string                     holder,
                std::string                     role,
                std::string                     db,
                const std::vector<std::string>& argv_parts)
                : RunAuditGuard(
                      store,
                      std::move(holder),
                      std::move(role),
                      std::move(db),
                      join_argv(argv_parts))
            {
            }
            RunAuditGuard(const RunAuditGuard&) = delete;
            RunAuditGuard& operator=(const RunAuditGuard&) = delete;

            /** @brief 显式收尾：reason ∈ done/queued/stolen（结果数据随行）。
             */
            void finish(
                std::string_view reason,
                int              exit_code,
                std::int64_t     files,
                std::string_view summary)
            {
                if (done_ || store_ == nullptr)
                {
                    return;
                }
                done_ = true;
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started_)
                        .count();
                json::Object payload;
                payload.set("durationMs", ms);
                payload.set("files", files);
                payload.set("exit", exit_code);
                payload.set("summary", summary);
                (void)store_->log_coord_event(
                    cg::db::CoordEventInput{
                        .event = cg::db::CoordEvent::RunFinish,
                        .actor_holder = holder_,
                        .actor_pid = cg::util::platform::current_pid(),
                        .reason = reason,
                        .detail = json::stringify(std::move(payload)),
                    });
            }

            ~RunAuditGuard()
            {
                // 兜底：异常上抛等未显式收尾的退栈（进程被杀不会到这里，
                // 由接管者补记 abandoned）。
                finish("exit", 0, 0, "");
            }

        private:
            cg::db::GraphStore*                   store_ = nullptr;
            std::string                           holder_;
            std::string                           role_;
            std::string                           db_;
            std::string                           argv_;
            std::chrono::steady_clock::time_point started_{};
            bool                                  done_ = false;
        };

        /**
         * @brief `ccg index` 与 `ccg update` 共享的命令清单解析结果：编译
         *        数据库加载加 cl 模式/数据库路线的模块自构建路由。
         */
        struct ResolvedCommands
        {
            std::vector<cg::clang::CompileCommand> commands;
            EnginePrepared                         engine;
            /** @brief PCH 影子规划计数/注记（无 PCH 工程全为零/空）。 */
            cg::clang::PchPlanResult pch;
        };
        namespace fs = std::filesystem;

        /**
         * @brief 打开（必要时创建）索引数据库到 out。
         *
         * schema_version 不匹配的库：`ccg msbuild` 默认重建文件（升级后无需
         * 手动 --force；记录的配置档案与 msbuild 注册表不受影响，重建的库由
         * 本次运行全量重写），其余命令沿用既有契约——版本不匹配的库被拒绝
         * 打开并提示 --force（项目未发布，不维护跨版本迁移），带 --force 才
         * 重建。其它打开失败仅在 --force 下重建（历史
         * 行为）。父目录创建为尽力而为——打开失败时自会报告。
         */
        [[nodiscard]]
        auto open_store_create(const Invocation& inv) -> cg::db::GraphStore
        {
            const std::string& path = inv.db_path;
            if (path != ":memory:")
            {
                std::error_code ec;
                const fs::path  parent = fs::path{path}.parent_path();
                if (!parent.empty())
                {
                    fs::create_directories(parent, ec);
                }
            }
            const auto recreate = [&]() -> cg::db::GraphStore
            {
                std::error_code ec;
                (void)fs::remove(fs::path{path}, ec);
                (void)fs::remove(fs::path{path + "-wal"}, ec);
                (void)fs::remove(fs::path{path + "-shm"}, ec);
                return cg::db::GraphStore{path};
            };
            try
            {
                return cg::db::GraphStore{path};
            }
            catch (const cg::db::SchemaMismatchError& e)
            {
                if (path == ":memory:"
                    || !(inv.force || inv.cmd == Invocation::Cmd::Msbuild))
                {
                    throw;
                }
                std::println(
                    std::cerr,
                    "ccg: note: {} - recreating {} (the msbuild route rebuilds "
                    "by default on schema version mismatch; --force remains the "
                    "explicit spelling)",
                    e.what(),
                    path);
                return recreate();
            }
            catch (const std::exception&)
            {
                if (!inv.force || path == ":memory:")
                {
                    throw;
                }
                return recreate();
            }
        }

        /**
         * @brief 读侧命令预先拒绝缺失文件（"对不存在/未打开的库
         *        做 Stats -> 错误、退出 1"；同一礼节适用于每个读命令）。
         */
        [[nodiscard]]
        auto open_store_read(const Invocation& inv) -> cg::db::GraphStore
        {
            if (inv.db_path != ":memory:" && !fs::exists(fs::path{inv.db_path}))
            {
                throw std::runtime_error(
                    std::format(
                        "cannot open database '{}': file not found",
                        inv.db_path));
            }
            return cg::db::GraphStore{inv.db_path};
        }

        /**
         * @brief 本次运行生效的注入旗标：命令行 --clang-compile-flags（可重复，
         *        解析层已切成 token、按出现序拼接，后写覆盖先写）优先，库配置
         *        键 clang-compile-flags（`ccg config set`，空格切分）次之，
         *        都没有则无注入。
         *
         * @internal
         * 库不存在（首次 index）或打不开（schema 失配等）时读作无注入——权威
         * 的打开路径自会报告后者；注入只服务重放层，绝不在此制造新失败面。
         */
        [[nodiscard]]
        inline auto resolve_clang_compile_flags(const Invocation& inv)
            -> std::vector<std::string>
        {
            if (!inv.clang_compile_flags.empty())
            {
                return inv.clang_compile_flags;
            }
            if (inv.db_path.empty() || inv.db_path == ":memory:")
            {
                return {};
            }
            std::error_code ec;
            if (!fs::is_regular_file(fs::path{inv.db_path}, ec) || ec)
            {
                return {};
            }
            try
            {
                cg::db::GraphStore store{inv.db_path};
                const auto         raw = store.meta("clang_compile_flags");
                if (!raw.has_value() || raw->empty())
                {
                    return {};
                }
                return cg::util::tokenize_command(*raw);
            }
            catch (const std::exception&)
            {
                return {};
            }
        }

        /**
         * @brief 重放层旗标注入：把生效旗标追加到每条重放 argv（含 PCH 文本
         *        回退副本）的末尾——不写 compile_commands.json（ccg msbuild
         *        重新生成 cdb 时配置不丢），注入只活在本次运行；argv 变化自然
         *        折进 parse_digest，旗标一改即全量重解析。有记录 argv 与无
         *        记录 argv（路由合成的基线）的 TU 一律生效。
         */
        inline void apply_clang_compile_flags(
            const Invocation&                       inv,
            std::vector<cg::clang::CompileCommand>& commands)
        {
            const std::vector<std::string> flags =
                resolve_clang_compile_flags(inv);
            if (flags.empty() || commands.empty())
            {
                return;
            }
            std::string joined;
            for (const std::string& flag : flags)
            {
                joined += joined.empty() ? flag : " " + flag;
            }
            for (cg::clang::CompileCommand& c : commands)
            {
                c.argv.insert(c.argv.end(), flags.begin(), flags.end());
                if (!c.pch_fallback_argv.empty())
                {
                    c.pch_fallback_argv.insert(
                        c.pch_fallback_argv.end(),
                        flags.begin(),
                        flags.end());
                }
            }
            std::println(
                std::cerr,
                "ccg: note: appending injected clang flags to every "
                "replayed argv: {}",
                joined);
        }

        /**
         * @brief 按文档化优先级（库 llvm_path > --llvm-root > $CCG_LLVM_ROOT >
         *        PATH 发现）解析并加载解析引擎。
         *
         * 旗标输给库钉时在 stderr 宣告并清除其驱动器覆盖。
         *
         * @internal
         * 对已加载路径，加载是幂等的；换用别的引擎只发生在静置时（无存活
         * Index 对象），驱动器缓存自行重新键到新引擎（cg.clang.driver）。
         * 库钉同样拥有驱动器：旗标既未选中引擎，也不得引导补充链——清除
         * 覆盖，让驱动器发现锚定在已加载引擎、然后环境变量、然后 PATH。
         */
        [[nodiscard]]
        inline auto prepare_index_engine(
            const Invocation&  inv,
            const std::string& db_engine) -> EnginePrepared
        {
            if (!inv.llvm_root.empty())
            {
                cg::clang::set_driver_root_override(inv.llvm_root);
            }
            if (!db_engine.empty())
            {
                if (!inv.llvm_root.empty())
                {
                    const std::string flag_norm = cg::util::norm_path(
                        fs::path{inv.llvm_root}
                            .lexically_normal()
                            .generic_string());
                    if (flag_norm != db_engine)
                    {
                        std::println(
                            std::cerr,
                            "ccg: note: the database's llvm-path ({}) takes "
                            "precedence over --llvm-root - using the configured "
                            "engine (change it with `ccg config set llvm-path` or "
                            "unset it to let --llvm-root apply)",
                            db_engine);
                    }
                }
                cg::clang::set_driver_root_override("");
            }
            const std::string engine_configured =
                !db_engine.empty() ? db_engine : inv.llvm_root;
            const std::string engine_path =
                cg::clang::engine::resolve_library(engine_configured);
            cg::clang::load_clang_library(engine_path);
            return {
                .valid = true,
                .db_engine = db_engine,
                .path = engine_path,
                .version = cg::clang::libclang_version()};
        }

        /**
         * @brief 不创建文件地窥探数据库的 llvm-path 钉。
         *
         * @internal
         * 缺失或内存库无钉；打不开的库读作无钉（下方权威的打开会报告它——
         * 或 --force 重建它，重建也抹掉钉；该分歧由调用方重新准备来处理）。
         */
        [[nodiscard]]
        inline auto read_db_engine_pin(const Invocation& inv) -> std::string
        {
            if (inv.db_path.empty() || inv.db_path == ":memory:")
            {
                return {};
            }
            std::error_code ec;
            if (!fs::is_regular_file(fs::path{inv.db_path}, ec))
            {
                return {};
            }
            try
            {
                cg::db::GraphStore store{inv.db_path};
                return store.meta("llvm_path").value_or(std::string{});
            }
            catch (const std::exception&)
            {
                return {};
            }
        }

        /**
         * @brief 本次运行生效的并发上限：--jobs 旗标 > 库配置键 index-jobs >
         *        自动按核。
         *
         * 统一上限的覆盖链单点。同一返回值喂给两段工人池——BMI 自建段
         * （SelfBuildOptions.jobs，self_build 组装点）与 TU 解析段
         * （IndexOptions.jobs，索引尾部）；两段时间上先后发生不叠加，峰值并发
         * 即该值（消费侧各自再经 cg::util::pool::resolve_worker_count 按作业数
         * 截断：解析段按全量作业数、BMI 段按层宽）。
         *
         * @param open_store 调用方已打开的库连接（直接读 meta，免二次开连
         *                  接；GraphStore::meta 非常量限定，故取可变指针——
         *                  只读使用）；空指针时自带只读连接窥探——命令路由
         *                  阶段（route_cl_module_tus / route_db_module_tus
         *                  的 self_build 组装点）正式 store 尚未打开，而
         *                  update 与索引尾部调用点库已在手。
         *
         * @internal
         * 读侧对 meta 值做与 `ccg config set index-jobs` 同一的严格校验（全串
         * 纯数字、1-64 生效；0 = 显式回自动）——手改库写出的畸形值按未配置
         * 处理，绝不制造新失败面（库缺失/打不开同理，权威打开路径自会报告）。
         * 自动档解析为 cg::util::pool::auto_worker_count（单一事实源，
         * clamp(cores-2, 1, 12)）。
         */
        [[nodiscard]]
        inline auto resolve_jobs(
            const Invocation&   inv,
            cg::db::GraphStore* open_store = nullptr) -> int
        {
            if (inv.jobs > 0)
            {
                return inv.jobs;
            }
            const auto configured = [&]() -> std::optional<std::string>
            {
                if (open_store != nullptr)
                {
                    return open_store->meta("index_jobs");
                }
                if (inv.db_path.empty() || inv.db_path == ":memory:")
                {
                    return std::nullopt;
                }
                std::error_code ec;
                if (!fs::is_regular_file(fs::path{inv.db_path}, ec))
                {
                    return std::nullopt;
                }
                try
                {
                    cg::db::GraphStore store{inv.db_path};
                    return store.meta("index_jobs");
                }
                catch (const std::exception&)
                {
                    return std::nullopt;
                }
            }();
            if (configured.has_value())
            {
                const std::string& raw = *configured;
                // 1-64 至多两位数字：长度先挡住溢出与荒谬值，再全串纯数字，
                // 最后区间判定（0 与畸形一律落到自动档）。
                if (!raw.empty() && raw.size() <= 2
                    && std::ranges::all_of(
                        raw,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    int value = raw[0] - '0';
                    if (raw.size() == 2)
                    {
                        value = value * 10 + (raw[1] - '0');
                    }
                    if (value >= 1 && value <= 64)
                    {
                        return value;
                    }
                }
            }
            return cg::util::pool::auto_worker_count();
        }

        /**
         * @brief 库打开后的引擎身份同步：权威钉复检 + 跨引擎改版检测。
         *
         * `ccg index` 与 `ccg update` 的索引尾部共用（昔日两份逐字相同的复
         * 制，改一处忘另一处的高危点）。两步：先用库里权威的 llvm-path 钉复
         * 准备引擎（库钉变了即世界观变了）；再比对上次索引时记录的引擎身份
         * ——库有内容而记录的引擎路径/版本与本次不一致时，返回"强制全量"
         * （引擎变化 = 世界观变化，增量捷径一律不作数）。空库不触发改版
         * （无旧世界观可比）。
         *
         * @return (同步后的引擎, 是否须强制全量重索引)。
         *
         * @internal 存在性探测用 has_files 单行探针（stats 的四表分组计数对
         *           每次编辑都跑的路径是纯浪费）；note 输出措辞两侧调用点
         *           原样保留。
         */
        [[nodiscard]]
        inline auto sync_engine_with_store(
            cg::db::GraphStore& store,
            const Invocation&   inv,
            EnginePrepared      engine) -> std::pair<EnginePrepared, bool>
        {
            const std::string db_engine =
                store.meta("llvm_path").value_or(std::string{});
            if (db_engine != engine.db_engine)
            {
                engine = prepare_index_engine(inv, db_engine);
            }
            bool engine_forced_full = false;
            {
                const auto recorded_path = store.meta("llvm_engine_path");
                const auto recorded_version = store.meta("llvm_version");
                const bool db_has_content = cg::db::has_files(store);
                if (db_has_content
                    && (!recorded_path.has_value()
                        || !recorded_version.has_value()
                        || *recorded_path != engine.path
                        || *recorded_version != engine.version))
                {
                    engine_forced_full = true;
                    std::println(
                        std::cerr,
                        "ccg: note: LLVM engine changed since this database was "
                        "last indexed (recorded: {}, {}; now: {}, {}) - engine "
                        "change = worldview change, forcing a FULL re-index (no "
                        "incremental shortcuts)",
                        recorded_path.value_or(std::string{"<none>"}),
                        recorded_version.value_or(std::string{"<unknown>"}),
                        engine.path,
                        engine.version);
                }
            }
            return {std::move(engine), engine_forced_full};
        }

    } // namespace detail
} // namespace cg::cli
