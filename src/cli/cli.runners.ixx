module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.runners
 *
 * @brief CLI 运行器的共享索引脊柱：引擎准备、命令清单解析（编译数据
 *        库加载 / 模块自构建路由）、cl 模式模块路由、以及
 *        `ccg index` / `ccg update` / `ccg msbuild` 共享的索引尾部。
 *
 * @internal
 * 曾按分区语法拆分（cg.cli:runners）；分区语法退役后无 BMI 实现单元
 * 一并退役，拆分时从 cli.index.cpp 吸收了定义。消费者：cg.cli.index /
 * cg.cli.update / cg.cli.msbuild。定义为接口驻留（inline）——把运行器
 * 函数体留在无 BMI 实现单元里的 C2801 时代要求已过时：规避补丁对决定性的
 * std BMI 宿主做了影子补丁（MSVC 移植注记），而"仅声明"的安排只是为
 * 绕开该缺陷的体量敏感性。db.tables.h 置于全局模块片段：本单元导入
 * cg.db，MSVC 要求 sqlpp23 表模型定义在此可见，以合并模块记录的模板
 * 特化（完整机理见 db.tables.h）。
 */
export module cg.cli.runners;

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
        void open_store_create(
            const Invocation&                  inv,
            std::optional<cg::db::GraphStore>& out)
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
            const auto recreate = [&]()
            {
                std::error_code ec;
                (void)fs::remove(fs::path{path}, ec);
                (void)fs::remove(fs::path{path + "-wal"}, ec);
                (void)fs::remove(fs::path{path + "-shm"}, ec);
                out.emplace(path);
            };
            try
            {
                out.emplace(path);
                return;
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
                recreate();
            }
            catch (const std::exception&)
            {
                if (!inv.force || path == ":memory:")
                {
                    throw;
                }
                recreate();
            }
        }

        /**
         * @brief 读侧命令预先拒绝缺失文件（"对不存在/未打开的库
         *        做 Stats -> 错误、退出 1"；同一礼节适用于每个读命令）。
         */
        void open_store_read(
            const Invocation&                  inv,
            std::optional<cg::db::GraphStore>& out)
        {
            if (inv.db_path != ":memory:" && !fs::exists(fs::path{inv.db_path}))
            {
                throw std::runtime_error(
                    std::format(
                        "cannot open database '{}': file not found",
                        inv.db_path));
            }
            out.emplace(inv.db_path);
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

        /**
         * @brief cl 模式路由（`ccg index` 对 cl 模式编译数据库与
         *        `ccg msbuild` 共享）。
         *
         * MSVC 构建的 .ifc BMI 无法供 clang 引擎使用，故 cl 模式库的模块单元
         * 离开重放、改走自构建（clang-cl 生产 + 解析）。与 db 路线同机制
         * （"有 db 用记录 argv"原则）：模块单元记录的编译命令作为扫描/
         * BMI/解析三处的权威 argv 传入（SelfBuildOptions::recorded +
         * db_mode）， 其中的 MSVC
         * 模块开关（/interface、/ifcOutput、/reference…）由自建
         * 链的剥离层剥除，/reference 名单与扫描器结果取并集注入影子 pcm——
         * 记录命令里本来就有工程 /I，项目 include 随记录 argv自动在场。
         * 工具集钉定轴、msvc-stl std 供应链、-fms-compatibility-version 物化
         * 等既有 cl 适配保留复用；合成路径（cl_base_argv/cl_parse_argv）保留
         * 给无记录条目的单元与 std 生产作兜底。经典条目按 msbuild 波的行为
         * 原样保留并就地获得喂给（重放 argv 同样过剥离层）。先加载引擎：自
         * 构建的生产者锁与驱动器锚定都要读已加载引擎的身份（引擎先行的
         * 顺序不变式）。
         *
         * @internal
         * 模块 TU 记录的宏定义塑造其头文件（合成的解析必须复现它们，
         * 静默随行）。链的语言轴取自记录的 /std: 值（精确匹配纪律：经典
         * 条目逐字重放它们，故 pcm 必须以同一 std 构建）；最高记录值胜出，
         * 模块需要 >=c++20，故无任何 /std:（或低于 c++20）的库以 c++20 垫底。
         * 同一文件的多条库条目（两个目标建同一单元）：首条记录命令为权威
         * （与 db 路线同裁决——重复重放会把同一文件解析两遍）。无可路由单元
         * 时恢复被移出的经典条目。
         */
        inline void route_cl_module_tus(
            const Invocation&                       inv,
            const std::vector<std::string>&         roots,
            std::vector<cg::clang::CompileCommand>& commands,
            EnginePrepared&                         engine,
            const std::string&                      msvc_toolset_hint = {})
        {
            const bool any_cl = std::ranges::any_of(
                commands,
                [](const auto& c)
                {
                    return std::ranges::find(c.argv, "--driver-mode=cl")
                           != c.argv.end();
                });
            if (!any_cl)
            {
                return;
            }
            std::vector<std::string>                         module_files;
            std::map<std::string, cg::clang::CompileCommand> recorded;
            std::vector<cg::clang::CompileCommand>           classic;
            std::vector<std::string>                         cl_dirs;
            std::vector<std::string>                         cl_defines;
            for (cg::clang::CompileCommand& c : commands)
            {
                if (cg::clang::is_module_tu(c))
                {
                    const std::string norm = cg::util::norm_path(c.file);
                    if (std::ranges::find(module_files, norm)
                        == module_files.end())
                    {
                        module_files.push_back(norm);
                        recorded.emplace(norm, c);
                    }
                    for (std::size_t i = 0; i + 1 < c.argv.size(); ++i)
                    {
                        if (c.argv[i] == "/D"
                            && std::ranges::find(cl_defines, c.argv[i + 1])
                                   == cl_defines.end())
                        {
                            cl_defines.push_back(c.argv[i + 1]);
                        }
                    }
                    continue;
                }
                for (std::size_t i = 0; i + 1 < c.argv.size(); ++i)
                {
                    if ((c.argv[i] == "-imsvc" || c.argv[i] == "/I")
                        && std::ranges::find(cl_dirs, c.argv[i + 1])
                               == cl_dirs.end())
                    {
                        cl_dirs.push_back(c.argv[i + 1]);
                    }
                }
                classic.push_back(std::move(c));
            }
            if (module_files.empty())
            {
                commands = std::move(classic);
                return;
            }
            engine = prepare_index_engine(inv, read_db_engine_pin(inv));
            cg::clang::SelfBuildOptions self_build;
            self_build.root = roots.front();
            self_build.ccg_config_dir = layout::config_dir_of_db(inv.db_path);
            self_build.force = inv.force || inv.full;
            self_build.dry_run = inv.dry_run;
            self_build.cl_mode = true;
            self_build.cl_include_dirs = std::move(cl_dirs);
            self_build.cl_defines = std::move(cl_defines);
            self_build.msvc_toolset_hint = msvc_toolset_hint;
            self_build.db_mode = true;
            self_build.db_config_label = "cl";
            self_build.jobs = resolve_jobs(inv);
            for (auto& [file, rec] : recorded)
            {
                self_build.recorded.emplace(file, std::move(rec));
            }
            std::string recorded_std = "c++20";
            const auto  note_std = [&recorded_std](const std::string& a)
            {
                if (!a.starts_with("/std:c++"))
                {
                    return;
                }
                const std::string value = "c++" + a.substr(8);
                if (value == "c++latest")
                {
                    recorded_std = "c++2d";
                }
                else if (value > recorded_std && value >= "c++20")
                {
                    recorded_std = value;
                }
            };
            for (const cg::clang::CompileCommand& c : classic)
            {
                for (const std::string& a : c.argv)
                {
                    note_std(a);
                }
            }
            for (const auto& [file, rec] : self_build.recorded)
            {
                for (const std::string& a : rec.argv)
                {
                    note_std(a);
                }
            }
            self_build.std_value = recorded_std;
            cg::clang::SelfBuildResult built = cg::clang::self_build_modules(
                self_build,
                module_files,
                classic);
            for (const std::string& note : built.notes)
            {
                std::println(std::cerr, "ccg: note: {}", note);
            }
            commands = std::move(classic);
            commands.insert(
                commands.end(),
                built.commands.begin(),
                built.commands.end());
        }

        /**
         * @brief 编译数据库模块路由，clang/gcc 方言（数据库路线）。
         *
         * 记录命令（响应文件展开之后）不含模块喂给的模块单元（或携带导入的
         * 经典 TU）无法逐字重放（多配置生成器记录每个配置的命令，而盘上只有
         * 已构建配置的 .modmap rsp 文件与 BMI；导入随后遇不到 BMI，libclang
         * 以零诊断死 rc=4）。此类单元离开重放、改走自构建，以记录的 argv 为
         * 生产/解析权威（统一规则：模块的 BMI 生产命令就是其权威命令——取
         * 其记录的 argv；无记录条目的单元才用基线旗标向量）。
         * 喂给齐备的条目（已构建配置）留在逐字重放上，无撰写导入的经典 TU
         * 永不被碰。在 cl 方言路由之后运行：那条路线留下的按构造即非
         * cl 方言。每个构建配置一次自构建（按记录 .modmap 路径的
         * ".dir/`<Config>`/" 签名）：各配置记录的 argv 不同，其 BMI 变体分别
         * 生产与缓存、绝不喂给另一配置的消费者。
         *
         * @internal
         * 逐配置的记录命令权威覆盖库中全部模块单元——不只是被路由的无喂给
         * 者：一次 pass 构建其单元的依赖闭包，而本身带喂给重放的依赖（已构建
         * 配置自己的条目）在无喂给兄弟导入它时仍在此产出其 pcm——用它的记录
         * argv，绝不用基线向量。前置拷贝：下方分区会把条目移出 commands。
         * 路由条目按构建配置签名与记录语言选项向量（PCM LangOpts 白名单，
         * cg::clang::pcm_langopt_vector）分组：同配置的单元可能记录不同的
         * 白名单语言选项，而一个 std pcm 恰好服务一种 LangOpts 形状——每个
         * （配置, 向量）组跑自己的自构建 pass 与自己的 std pcm 变体。全同
         * 向量的配置坍缩为统一旗标世界的单 pass；跨组 pcm 导入作为命名的
         * module-build-failed 行失败而非静默混形状；失败组的 std 供给只降级
         * 该组（每组一 pass，组间绝不共享状态）。变体计数护栏：健康的树记录
         * 少量向量（每配置一个乘以每个真实旗标分叉一个——CMake 构建被其单一
         * 合成 std 目标锁成全同）。散布众多向量的库几乎总是陈旧或手改的，
         * 而每个向量花一次完整 std pcm 构建加一个缓存槽；仅劝告性提示——
         * 绝不阻塞，只诚实陈述代价。同配置同文件的两条库条目（两个目标建
         * 同一单元）：首条记录命令为
         * 权威；重复重放会把同一文件解析两遍——丢弃（索引器自身的
         * （文件, 摘要）去重同理）。链的语言轴取自该配置命令记录的 -std 值
         * （std pcm 与无权威单元的基线对齐它）；最高记录值胜出、以 c++20
         * 垫底（模块语法最低），gnu++ 拼写映射到 c++ 孪生。经典条目被就地
         * 增强（喂给追加进其记录 argv）。
         */
        inline void route_db_module_tus(
            const Invocation&                       inv,
            const std::vector<std::string>&         roots,
            std::vector<cg::clang::CompileCommand>& commands,
            EnginePrepared&                         engine)
        {
            std::vector<cg::clang::CompileCommand> keep;
            std::vector<cg::clang::CompileCommand> routed;
            std::map<
                std::string,
                std::map<std::string, cg::clang::CompileCommand>>
                recorded_by_config;
            for (const cg::clang::CompileCommand& c : commands)
            {
                if (cg::clang::is_module_tu(c))
                {
                    recorded_by_config[cg::clang::db_config_signature(c)]
                        .emplace(cg::util::norm_path(c.file), c);
                }
            }
            for (cg::clang::CompileCommand& c : commands)
            {
                const bool cl = std::ranges::find(c.argv, "--driver-mode=cl")
                                != c.argv.end();
                const bool moduleish = cg::clang::is_module_tu(c)
                                       || cg::clang::text_has_import(c.file);
                if (!cl && c.module_feeds.empty() && moduleish)
                {
                    routed.push_back(std::move(c));
                }
                else
                {
                    keep.push_back(std::move(c));
                }
            }
            commands = std::move(keep);
            if (routed.empty())
            {
                return;
            }
            engine = prepare_index_engine(inv, read_db_engine_pin(inv));

            struct RouteGroup
            {
                std::string              config;   // 构建配置签名
                std::vector<std::string> langopts; // 本组的白名单向量
                std::vector<cg::clang::CompileCommand> entries;
            };
            const auto group_label =
                [](const std::string&              config,
                   const std::vector<std::string>& langopts)
            {
                if (langopts.empty())
                {
                    return config;
                }
                std::string joined;
                for (const std::string& flag : langopts)
                {
                    joined += joined.empty() ? flag : " " + flag;
                }
                return std::format("{}~langopt({})", config, joined);
            };
            std::map<std::string, RouteGroup> groups; // 键：复合标签
            for (cg::clang::CompileCommand& c : routed)
            {
                const std::string config = cg::clang::db_config_signature(c);
                std::vector<std::string> langopts =
                    cg::clang::pcm_langopt_vector(c.argv);
                const std::string label = group_label(config, langopts);
                RouteGroup&       g = groups[label];
                g.config = config;
                g.langopts = std::move(langopts);
                g.entries.push_back(std::move(c));
            }
            if (groups.size() > 8)
            {
                std::println(
                    std::cerr,
                    "ccg: note: {} std pcm variants (language-option "
                    "groups) in this compile db - each variant builds and "
                    "caches its own std pcm; expected only a few, so check "
                    "for a stale or split-flags database",
                    groups.size());
            }

            std::size_t routed_count = routed.size();
            for (auto& [label, group] : groups)
            {
                std::vector<std::string>               module_files;
                std::vector<cg::clang::CompileCommand> classic;
                cg::clang::SelfBuildOptions            self_build;
                for (cg::clang::CompileCommand& c : group.entries)
                {
                    if (cg::clang::is_module_tu(c))
                    {
                        const std::string norm = cg::util::norm_path(c.file);
                        if (std::ranges::find(module_files, norm)
                            == module_files.end())
                        {
                            module_files.push_back(norm);
                            continue;
                        }
                        --routed_count;
                        continue;
                    }
                    classic.push_back(std::move(c));
                }
                if (const auto rit = recorded_by_config.find(group.config);
                    rit != recorded_by_config.end())
                {
                    for (const auto& [file, cc] : rit->second)
                    {
                        self_build.recorded.emplace(file, cc);
                    }
                }
                self_build.db_config_label = label;
                self_build.langopt_axes = group.langopts;
                if (module_files.empty() && classic.empty())
                {
                    continue;
                }
                self_build.root = roots.front();
                self_build.ccg_config_dir =
                    layout::config_dir_of_db(inv.db_path);
                self_build.force = inv.force || inv.full;
                self_build.dry_run = inv.dry_run;
                self_build.db_mode = true;
                self_build.jobs = resolve_jobs(inv);
                std::string recorded_std = "c++20";
                const auto  note_std = [&recorded_std](const std::string& a)
                {
                    std::string value;
                    if (a.starts_with("-std=c++"))
                    {
                        value = "c++" + a.substr(8);
                    }
                    else if (a.starts_with("-std=gnu++"))
                    {
                        value = "c++" + a.substr(10);
                    }
                    else
                    {
                        return;
                    }
                    if (value > recorded_std && value >= "c++20")
                    {
                        recorded_std = value;
                    }
                };
                for (const cg::clang::CompileCommand& c : classic)
                {
                    for (const std::string& a : c.argv)
                    {
                        note_std(a);
                    }
                }
                for (const auto& [file, rec] : self_build.recorded)
                {
                    for (const std::string& a : rec.argv)
                    {
                        note_std(a);
                    }
                }
                self_build.std_value = recorded_std;

                cg::clang::SelfBuildResult built =
                    cg::clang::self_build_modules(
                        self_build,
                        module_files,
                        classic);
                for (const std::string& note : built.notes)
                {
                    std::println(std::cerr, "ccg: note: {}", note);
                }
                commands.insert(
                    commands.end(),
                    built.commands.begin(),
                    built.commands.end());
                commands.insert(
                    commands.end(),
                    std::make_move_iterator(classic.begin()),
                    std::make_move_iterator(classic.end()));
            }
            if (routed_count > 0)
            {
                std::println(
                    std::cerr,
                    "ccg: note: routed {} module/importing TU{} without "
                    "recorded BMI feeds through the self-build (compile-db "
                    "route, recorded argv authority)",
                    routed_count,
                    routed_count == 1 ? "" : "s");
            }
        }

        /**
         * @brief 解析（并缓存）驱动器知识，使缺失驱动器是一个可行动的错误
         *        而非每 TU 一个失败。
         *
         * @internal
         * 已加载引擎把驱动器发现锚定在做解析的安装上；后续运行在静置时换
         * 引擎，缓存自行重新键。
         */
        inline void require_driver()
        {
            if (cg::clang::driver_info().driver.empty())
            {
                throw std::runtime_error(
                    "ccg: no clang driver executable found (looked beside the "
                    "loaded LLVM engine library and under its installation "
                    "prefix, then via CCG_LLVM_ROOT, then on PATH). Set "
                    "CCG_LLVM_ROOT, pass `ccg index --llvm-root <prefix>`, or "
                    "configure `ccg config set llvm-path <prefix>` to name the "
                    "LLVM installation containing clang++/clang.");
            }
        }

        // 前置声明（定义在下方）。
        [[nodiscard]]
        inline auto resolve_loaded_commands(
            const Invocation&                             inv,
            const std::vector<std::string>&               roots,
            std::vector<cg::clang::CompileCommand>        commands,
            const std::vector<cg::clang::CompileCommand>& coverage,
            const std::string& db_file) -> ResolvedCommands;

        /**
         * @brief 缺失编译数据库时的可行动错误文案。
         *
         * 两行之外的每条提示都是一条可执行的修复路径：让构建自己导出数据库、
         * 用 MSBuild 适配器索引解决方案、或显式点名数据库文件。
         */
        [[nodiscard]]
        inline auto missing_compile_db_message(
            const std::vector<std::string>& roots) -> std::string
        {
            const std::string root =
                roots.empty() ? std::string{"<root>"} : roots.front();
            return std::format(
                "no compile_commands.json found under {}\n"
                "  hint: have your build export one (CMake: "
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON)\n"
                "  hint: or index a Visual Studio solution with `ccg msbuild`\n"
                "  hint: or pass the database explicitly with "
                "`--compile-commands <file>`",
                root);
        }

        /**
         * @fn cg::cli::detail::resolve_loaded_commands
         * @brief 已加载命令清单的解析（index 拆分路线与 update 过滤后共用）。
         *
         * coverage 是 PCH 发现的额外基准：拆分路线只喂本配置的分片，update
         * 用全量清单做发现。MSBuild 没提到的文件一概不管（孤儿、条件排除、
         * 他配置的文件不扫不提——模块单元只能经编译数据库条目索引，缺条目
         * 的单元不构成 ccg 的行动项）。
         */
        [[nodiscard]]
        inline auto resolve_loaded_commands(
            const Invocation&                             inv,
            const std::vector<std::string>&               roots,
            std::vector<cg::clang::CompileCommand>        commands,
            const std::vector<cg::clang::CompileCommand>& coverage,
            const std::string& db_file) -> ResolvedCommands
        {
            ResolvedCommands out;
            out.commands = std::move(commands);
            // 旗标注入先于一切路由：记录 argv 携带注入旗标进入 cl/db 两条模块
            // 路由（BMI 生产与消费者解析同旗标世界）与 PCH 规划。
            apply_clang_compile_flags(inv, out.commands);
            route_cl_module_tus(inv, roots, out.commands, out.engine);
            route_db_module_tus(inv, roots, out.commands, out.engine);

            // PCH 影子规划：entry 级门控在任何 PCH 标记/ sidecar 存在时才进入。
            // update 用全量 coverage 做发现，且禁止 purge。
            const auto pch_data = [](const cg::clang::CompileCommand& c)
            { return c.pch_involved || c.pch_create || c.pch_use; };
            const std::string sidecar =
                db_file.empty()
                    ? std::string{}
                    : cg::util::norm_path(
                          (fs::path{db_file}.parent_path() / "pch.json")
                              .lexically_normal()
                              .generic_string());
            const bool pch_possible =
                std::ranges::any_of(out.commands, pch_data)
                || std::ranges::any_of(coverage, pch_data)
                || (!sidecar.empty() && fs::is_regular_file(fs::path{sidecar}));
            if (pch_possible)
            {
                // 影子必须由当前 libclang 的同源驱动器构建：先准备引擎，使
                // driver_info() 锚定在做解析的安装上。
                if (!out.engine.valid)
                {
                    out.engine =
                        prepare_index_engine(inv, read_db_engine_pin(inv));
                }
                cg::clang::PchOptions pch_opt;
                pch_opt.ccg_config_dir = layout::config_dir_of_db(inv.db_path);
                pch_opt.compile_db_path = db_file;
                pch_opt.sidecar_path = sidecar;
                // `--full` 是索引全量，不是影子缓存强制作废；只有显式 --force
                // 才重建影子（否则内容相同的重建会让全部消费者无谓重解析）。
                pch_opt.force = inv.force;
                pch_opt.dry_run = inv.dry_run;
                pch_opt.purge = inv.cmd == Invocation::Cmd::Index
                                && (inv.force || inv.full);
                out.pch = cg::clang::plan_pch(
                    std::move(out.commands),
                    coverage,
                    pch_opt);
                out.commands = std::move(out.pch.commands);
                out.pch.commands.clear();
                out.pch.commands.shrink_to_fit();
                for (const std::string& note : out.pch.notes)
                {
                    std::println(std::cerr, "ccg: note: {}", note);
                }
            }
            // 喂给恢复收尾（原公共尾部；模块单元计数提示已随功能退役——
            // MSBuild 没提的文件一概不管）。
            if (const std::size_t fed =
                    cg::clang::supply_module_feeds(out.commands);
                fed > 0)
            {
                std::println(
                    std::cerr,
                    "ccg: note: restored BMI feeds for {} module TU{} from "
                    "CXXModules.json (the .modmap channel was empty)",
                    fed,
                    fed == 1 ? "" : "s");
            }
            return out;
        }

        /**
         * @brief 按库记录的内部构建配置签名过滤命令清单（拆分库的 update 面）。
         *
         * 拆分语义下一库一配置：库 meta 的 build_config 记录其签名，
         * update 重载全量编译数据库时只保留本配置的条目（无签名条目随行——
         * 无法归属的条目对各配置都对）。空签名（单配置库）不过滤。
         */
        [[nodiscard]]
        inline auto filter_by_build_config(
            std::vector<cg::clang::CompileCommand> commands,
            const std::string&                     build_config)
            -> std::vector<cg::clang::CompileCommand>
        {
            if (build_config.empty())
            {
                return commands;
            }
            std::erase_if(
                commands,
                [&](const cg::clang::CompileCommand& c)
                {
                    const std::string sig = cg::clang::db_config_signature(c);
                    return !sig.empty() && sig != build_config;
                });
            return commands;
        }

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

            std::optional<cg::db::GraphStore> store;
            open_store_create(inv, store);

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
                // 析构之前完成自己的释放（异常路径由守卫析构兜底）。
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
