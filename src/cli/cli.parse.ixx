module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.parse
 *
 * @brief argv -> Invocation：CLI 规格文档 第 2 节完整命令矩阵的解析。
 *
 * 共享旗标、逐命令校验（未知选项 / 缺操作数 / 坏值 -> UsageError ->
 * 退出 2）、逐命令默认值、以及随退出 2（或 help）打印的用法文本。
 *
 * @internal
 * db.tables.h 置于全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23
 * 表模型定义在此可见，以合并模块记录的模板特化（完整机理见
 * db.tables.h）。
 */
export module cg.cli.parse;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.graph;

export namespace cg::cli
{

    /**
     * @brief 用法错误：携带原因与随后打印的用法块（用法
     *        错误走 stderr、退出 2）。
     */
    class UsageError : public std::runtime_error
    {
    public:
        UsageError(std::string_view why, std::string_view usage)
            : std::runtime_error(std::string{why}), usage_(usage)
        {
        }
        [[nodiscard]]
        auto usage() const noexcept -> const std::string&
        {
            return usage_;
        }

    private:
        std::string usage_;
    };

    /** @brief 校验完毕后命令运行器所需的一切。 */
    struct Invocation
    {
        enum class Cmd : std::uint8_t
        {
            Help,
            Version,
            Index,
            Update,
            Msbuild,
            Config,
            Hook,
            Query,
            Context,
            Callers,
            Callees,
            Impact,
            Trace,
            Stats,
            Changes,
            Check,
            Graph,
            Members,
        };
        Cmd                      cmd = Cmd::Help;
        std::vector<std::string> positional; // index 根/符号名/changes 变更文件
        /**
         * @brief 产出文件夹（-o、$CCG_DB 或 ./.ccg），解析前的拼写。
         *
         * 布局规则：-o 只许指定文件夹；每配置一个子目录
         * `<folder>`/`<config>`/，ccg.db 与派生物同居其中。db_path 由布局解析
         * （cg.cli.layout）在分派期填入最终的库文件路径。
         */
        std::string output_dir;
        std::string config_name; // --config（"" = 发现规则自动选）
        std::string db_path;     // 解析后的库文件（<folder>/<config>/ccg.db）
        std::string layout_root; // 解析后的产出文件夹（hook 子进程转发用）
        std::string compile_db;  // index：显式 --compile-commands
        std::string llvm_root;   // index：显式 --llvm-root 前缀
        std::string from_file;   // changes：--from-file 清单路径
        /**
         * index/update/msbuild：--clang-compile-flags（可重复）累计的注入
         * 旗标 token（按出现序拼接，后写覆盖先写——驱动器同名旗标后者胜）。
         *
         * 重放层把它们追加到每条重放 argv 的末尾；非空时压制库配置键
         * clang-compile-flags（命令行本次 > 配置键 > 无）。
         */
        std::vector<std::string> clang_compile_flags;
        bool                     json = false;
        bool                     verbose = false;
        bool force = false;        // index / hook uninstall（--force）
        bool full = false;         // 仅 index（--full）
        bool dry_run = false;      // 仅 index
        bool paranoid = false;     // 仅 index
        bool cycles = false;       // check：--cycles 模式
        bool hook_user = false;    // hook：目标 ~/.claude/settings.json
        bool hook_project = false; // hook：目标 ./.claude/settings.json
        /**
         * index/update/msbuild：--jobs 并发上限旗标（1-64）。
         *
         * 0 = 未传旗标：生效值经 cg::cli::detail::resolve_jobs 合成——
         * 旗标 > 库配置键 index-jobs（`ccg config set`，0 = 显式回自动）
         * > 自动按核（clamp(cores-2, 1, 12)）。生效值统管两段工人池
         * （BMI 自建段与 TU 解析段）。
         */
        int                  jobs = 0;
        int                  depth = 1;     // 逐命令默认值在此施加
        int                  list_cap = 20; // check：列出的环簇上限
        cg::graph::Direction direction = cg::graph::Direction::Up;
        bool                 no_merge = false; // 符号命令：关闭覆盖族合并
        /**
         * @brief 查询侧的动态派发证据策略。
         *
         * @internal 默认 Precise（B 精确模式）：confidence >= 0.7（1.0/0.7）
         * 的 virtual_dispatch 证据替换调用点上的家族过近似；0.4 弱推断与
         * 0.0 标记不算证据，按整个 override 家族估算。
         * `--no-dispatch` 回到与引入 dispatch 之前逐字节一致的纯静态视图，
         * `--dispatch-fallback` 额外显示 0.0 家族标记。
         */
        cg::graph::DispatchMode dispatch_mode =
            cg::graph::DispatchMode::Precise;
        std::optional<cg::db::SymbolKind> kind;
        std::optional<std::string>        file_substring;
        /**
         * @brief callers/callees/impact 的 --top N：每个结果区至多列出 N 条
         *        （默认无限制；截断处以 "… and N more" 行标明，truncated 实
         *        算为 true，计数仍覆盖全部找到的条目）。
         */
        std::optional<int> top;
        /**
         * @brief query 专属：--implements 模式（positional 当接口/基类名，
         *        列派生树；类型继承=extends 边，与虚函数 override 家族无关）。
         */
        bool implements = false;
        /**
         * @brief --exclude-glob G（可重复）：查询结果端的路径排除——定义
         *        路径命中任一 glob 的条目从结果中剔除（索引侧全量收录不动，
         *        这正是"全量收录 + 用户主动过滤"的配套面）。
         */
        std::vector<std::string> exclude_globs;
        /** @brief 仅 msbuild（MSBuild 管线规格）：`ccg msbuild` 的选项集。 */
        std::string msbuild_config;      // --config C（""=注册表默认/Debug）
        std::string msbuild_platform;    // --platform P（""=x64）
        std::string msbuild_args;        // --msbuild-args S，逐字 MSBuild 尾参
        std::string cl_path;             // --cl PATH，跳过 cl 发现
        std::string msbuild_path;        // --msbuild PATH，跳过 vswhere
        bool        msbuild_all = false; // --all：全部声明的 C|P 对
        bool        msbuild_export_only = false; // --export-only：cdb 后即停
        bool        msbuild_list = false;        // --list：打印注册表并退出
        /** @brief --record：显式走记录管线（wrap 捕获的对照真值臂——
         * 测试/验证基础设施身份的入口；与 --full-build 互斥）。默认
         * （无旗标）= 求值通道：对解决方案做工程求值、ccg 侧组装 argv，
         * 不跑构建、不桩化工具。 */
        bool msbuild_record = false;
        /** @brief --full-build：真编（构建图真跑、cl 真编译，金标准对账
         * 逃生口；与 --record 互斥）。 */
        bool msbuild_full_build = false;
        /** @brief --allow-failure：接受 MSBuild 非零退出码仍落盘捕获数据
         *  （部署断言/测试兜底检查对沙盒产物不友好的场景；仅对跑构建的
         * 通道有意义——--record/--full-build，默认求值通道无 MSBuild
         * 退出码，解析层组合拒绝）。 */
        bool msbuild_allow_failure = false;
        bool db_explicit = false;     // 见过 -o
        bool config_explicit = false; // 见过 --config（index 必给）
        bool drain = false; // update：--drain——无自有文件也处理待办队列
        /**
         * @brief index/msbuild 的提交段租约排队（事务内心跳方案）。
         *
         * @internal 默认排队等待（上界 wait_limit_ms）；--wait=&lt;ms&gt; 收紧
         * 上界；--no-wait 快速失败。解析层只记偏好，协调发生在 Indexer 的
         * 提交段（IndexOptions::Coordination）。
         */
        bool         no_wait = false;
        std::int64_t wait_limit_ms = 1'800'000; // 默认 30 分钟排队上界
        bool wait_cap_seen = false; // 见过 --wait=<ms>（与 --no-wait 互斥判定）
        std::string graph_symbol;   // graph：--symbol 入口符号名
        bool        graph_modules = false; // graph：--modules 模式
        std::string graph_format = "dot";  // graph：--format dot|json
        std::string output_file;           // graph：--output 载荷落盘路径
        int         min_weight = 1;        // graph --modules：聚合边权重下限
    };

    /**
     * @brief ccg changes 的一个变更文件条目，已对数据库解析。
     *
     * 携带喂入时的拼写、解析到时的库路径文本、文件是否已从盘上消失
     * （删除——其符号在库中存活到下次索引运行）、以及该文件定义的符号数。
     */
    struct ChangedFile
    {
        std::string  input;           // 喂入时的精确拼写（规范化前）
        std::string  path;            // 库路径文本；未知时 ""
        bool         known = false;   // 已解析到 files 行
        bool         deleted = false; // 行已知、文件盘上缺失
        std::int64_t symbols = 0;
    };

    /** @brief 顶层用法文本。 */
    [[nodiscard]]
    auto top_usage() -> std::string_view
    {
        return "usage: ccg <command> [arguments] [options]\n"
               "\n"
               "commands:\n"
               "  index    build or update the database from a "
               "compile_commands.json\n"
               "  update   realtime refresh of changed files (db-coordinated queue)\n"
               "  msbuild  index an MSBuild solution (Windows)\n"
               "  config   per-database settings (LLVM engine path)\n"
               "  hook     manage the Claude Code integration (install/uninstall)\n"
               "  query    find symbols by name\n"
               "  context  360-degree symbol view\n"
               "  members  list a record's methods and fields\n"
               "  callers  who calls this symbol\n"
               "  callees  what this symbol calls\n"
               "  impact   blast radius of changing a symbol\n"
               "  trace    shortest call path between two symbols\n"
               "  changes  affected consumers of the files you changed\n"
               "  check    db sanity checks (include cycles with --cycles)\n"
               "  graph    export the call graph (Graphviz dot or JSON)\n"
               "  stats    database summary\n"
               "\n"
               "shared options:\n"
               "  -o DIR          output FOLDER: one directory per\n"
               "                  configuration inside, each holding ccg.db\n"
               "                  (default $CCG_DB or ./.ccg)\n"
               "  --config NAME   select (or, for index, create) the\n"
               "                  configuration <folder>/<config>/; with exactly\n"
               "                  one configuration present, query commands may\n"
               "                  omit it and the single one is auto-selected\n"
               "  --json          machine-readable output on stdout\n"
               "  -v, --verbose   progress chatter to stderr\n"
               "  --version       print version and exit\n"
               "  --help, -h      print this help and exit\n"
               "\n"
               "symbol commands take --kind K (record function method constructor\n"
               "destructor field variable enum enum_constant type_alias) and\n"
               "--file SUBSTR to narrow matching. callers/callees/impact also take\n"
               "--top N (list at most N entries per result section; totals keep\n"
               "counting everything) and --exclude-glob G (drop result entries "
               "whose\n"
               "defining path matches G, e.g. build/** or _deps/*; repeatable);\n"
               "context takes --exclude-glob too. The packaged docs (beside\n"
               "the binary) carry the full command reference.\n";
    }

    /** @brief 逐命令的用法文本。 */
    [[nodiscard]]
    auto usage_for(Invocation::Cmd cmd) -> std::string_view
    {
        switch (cmd)
        {
        case Invocation::Cmd::Index:
            return "usage: ccg index [<paths>...] --config <name> "
                   "[--compile-commands F] [--llvm-root DIR] "
                   "[--clang-compile-flags F] [--full] [--dry-run] "
                   "[--paranoid] [--jobs N] [--wait[=<ms>]] [--no-wait] "
                   "[-o DIR] [-v] [--json]\n"
                   "\n"
                   "  <paths>               project root directories (omit on a\n"
                   "                        re-run: the recorded profile restores\n"
                   "                        them)\n"
                   "  --config <name>       REQUIRED: names the configuration\n"
                   "                        directory <folder>/<name>/ holding\n"
                   "                        ccg.db, the BMI cache and profile.json\n"
                   "  --compile-commands F  explicit compile_commands.json path\n"
                   "  --llvm-root DIR       LLVM prefix for the clang++ driver and\n"
                   "                        the engine when no llvm-path is\n"
                   "                        configured (a configured llvm-path\n"
                   "                        wins; default: $CCG_LLVM_ROOT, else\n"
                   "                        discovered)\n"
                   "  --clang-compile-flags F\n"
                   "                        extra clang flags appended to EVERY\n"
                   "                        replayed argv (repeatable; later\n"
                   "                        occurrences win). Not written into\n"
                   "                        compile_commands.json; changing them\n"
                   "                        re-parses. Per-database default via\n"
                   "                        `ccg config set clang-compile-flags`;\n"
                   "                        the command line wins for that run\n"
                   "  --full                re-index everything in place (the\n"
                   "                        default against a fresh db)\n"
                   "  --dry-run             print the plan (dirty/skipped TU\n"
                   "                        counts, estimated work) without writing\n"
                   "  --paranoid            ignore the mtime fast path and hash\n"
                   "                        every file (catches mtime-preserving\n"
                   "                        copies)\n"
                   "  --force               legacy alias of --full that also\n"
                   "                        recreates the db on schema version\n"
                   "                        mismatch\n"
                   "  --jobs N              parallel workers for BOTH the module\n"
                   "                        BMI build and the TU parse pool,\n"
                   "                        1-64 (default: the index-jobs config\n"
                   "                        key, else auto min(cores-2, 12); the\n"
                   "                        flag wins for this run)\n"
                   "  --wait[=<ms>]         cap how long index may queue for the\n"
                   "                        write lock while taking over the\n"
                   "                        update claim (DEFAULT 30 min; index\n"
                   "                        STEALS the claim at startup - the\n"
                   "                        holder yields at its next heartbeat)\n"
                   "  --no-wait             fail fast when a live claim holds the\n"
                   "                        db instead of taking it over\n"
                   "\n"
                   "  The first run records a profile (roots, compile db, engine\n"
                   "  identity, argv) inside the configuration directory; a bare\n"
                   "  `ccg index --config <name>` re-run restores every input\n"
                   "  from it (command-line values win and update the profile).\n"
                   "  A compile db that carries MULTIPLE build configurations\n"
                   "  splits: one configuration directory per internal config,\n"
                   "  one db each (no exception); the split directories are named\n"
                   "  after the internal configurations.\n"
                   "\n"
                   "  Against an existing db, index defaults to INCREMENTAL:\n"
                   "  only TUs whose main file or transitive include closure\n"
                   "  changed (or whose compile command digest changed) re-parse.\n";
        case Invocation::Cmd::Update:
            return "usage: ccg update [<file>...] [--drain] [--compile-commands F]\n"
                   "                   [--llvm-root DIR] "
                   "[--clang-compile-flags F]\n"
                   "                   [--jobs N] [--config NAME]\n"
                   "                   [-o DIR] [-v] [--json]\n"
                   "\n"
                   "  <file>...     changed source files (absolute, or relative\n"
                   "                to the working directory)\n"
                   "  --drain       process the pending queue until empty even\n"
                   "                with no own files (Stop-hook tail pattern)\n"
                   "  --compile-commands F  override the compile database the db\n"
                   "                recorded, for this run only\n"
                   "  --llvm-root DIR  session engine override (a configured\n"
                   "                llvm-path wins, like index)\n"
                   "  --clang-compile-flags F\n"
                   "                extra clang flags appended to every replayed\n"
                   "                argv this run (repeatable; the per-database\n"
                   "                clang-compile-flags config key applies when\n"
                   "                the flag is absent)\n"
                   "  --jobs N      parallel workers for the module BMI build\n"
                   "                and the parse pool, 1-64 (same precedence as\n"
                   "                index: the flag wins, else the index-jobs\n"
                   "                config key, else auto)\n"
                   "  --config NAME / -o DIR  database discovery like every\n"
                   "                query command (single configuration under the\n"
                   "                folder auto-selects)\n"
                   "\n"
                   "  One-shot realtime refresh, no daemon, the DATABASE is the\n"
                   "  coordinator: own files are queued in the db's\n"
                   "  pending_updates table and the singleton claim row is taken\n"
                   "  in one short transaction; the holder drains the queue\n"
                   "  batch by batch through the ordinary incremental engine\n"
                   "  (dirty set = the queued files + their include closure). A\n"
                   "  concurrent invocation queues its files and exits 0\n"
                   "  immediately (the holder drains). A crashed holder leaves\n"
                   "  no stale state: its claim lease expires and the next\n"
                   "  updater steals it. Knobs: `ccg config set update-lease-ms\n"
                   "  N` / `update-busy-timeout-ms N`. The database must already\n"
                   "  exist (run `ccg index` first). Editor auto-refresh\n"
                   "  wiring: `ccg hook install --project`.\n";
        case Invocation::Cmd::Msbuild:
            return "usage: ccg msbuild <path.sln> [--config C]\n"
                   "                   [--platform P] [--all] [--export-only]\n"
                   "                   [--record] [--full-build]\n"
                   "                   [--msbuild-args S] [--cl PATH]\n"
                   "                   [--msbuild PATH] [-o DIR] "
                   "[--compile-commands F]\n"
                   "                   [--clang-compile-flags F] [--jobs N]\n"
                   "                   [--force] [--wait[=<ms>]] [--no-wait]\n"
                   "                   [-v] [--json]\n"
                   "       ccg msbuild --list [<path>]\n"
                   "\n"
                   "  Extract compile commands from an MSBuild solution and\n"
                   "  index one configuration directory per\n"
                   "  Configuration|Platform at <layout>/<C>-<P>/ (db inside as\n"
                   "  ccg.db; layout = -o DIR, else <root>/.ccg). The DEFAULT\n"
                   "  channel is project evaluation: ccg loads the VS-shipped\n"
                   "  Microsoft.Build engine in-process, evaluates every member\n"
                   "  project, enumerates ClCompile items with their final\n"
                   "  metadata and composes the cl argv itself - no build, no\n"
                   "  tool stubs, no sandbox, which is what makes refreshes\n"
                   "  seconds-fast. Evaluation expects a warm tree (at least\n"
                   "  one successful build so generated sources exist on\n"
                   "  disk); a cold checkout reports missing sources without\n"
                   "  blocking. Build-running channels are opt-in:\n"
                   "  --record (wrapped capture, test/verification\n"
                   "  infrastructure) and --full-build (real compilation).\n"
                   "\n"
                   "  <path>              .sln solution file; .vcxproj and\n"
                   "                      directory inputs are rejected\n"
                   "                      (single-project input not supported)\n"
                   "  --config C          configuration to evaluate (default:\n"
                   "                      the registry default, else Debug)\n"
                   "  --platform P        platform to evaluate (default x64)\n"
                   "  --all               every Configuration|Platform the input\n"
                   "                      declares, one db each\n"
                   "  --export-only       stop after writing compile_commands.json\n"
                   "                      (clangd/cl-tidy usable as-is)\n"
                   "  --record            run the wrapped-build capture pipeline\n"
                   "                      (test/verification infrastructure): the\n"
                   "                      build graph runs with ccg.exe standing\n"
                   "                      in for the compile-tool family -\n"
                   "                      CustomBuild/midl generators execute for\n"
                   "                      real (keeping the TU set complete),\n"
                   "                      compile tools are stubbed (cl/link/lib/\n"
                   "                      rc/mt record their argv and write\n"
                   "                      nothing; post-build steps failing on\n"
                   "                      missing products are expected; MASM\n"
                   "                      tools resolve ml64 by absolute path\n"
                   "                      and run for real); MSBuild outputs go\n"
                   "                      to a sandbox under the configuration\n"
                   "                      dir and writes escaping into the\n"
                   "                      project tree are reported as\n"
                   "                      sandbox-leak warnings. The recorded cl\n"
                   "                      invocations are the ground-truth arm\n"
                   "                      the default evaluation channel is\n"
                   "                      compared against\n"
                   "  --full-build        real compilation (the wrapped capture\n"
                   "                      runs with every cl really executing);\n"
                   "                      gold-standard reconciliation escape\n"
                   "                      hatch. Mutually exclusive with --record\n"
                   "  --allow-failure     accept a non-zero MSBuild exit code and\n"
                   "                      still translate the capture (deploy\n"
                   "                      assertions and other sandbox-hostile\n"
                   "                      custom targets); requires --record or\n"
                   "                      --full-build - the evaluation channel\n"
                   "                      runs no MSBuild build\n"
                   "  --msbuild-args S    extra MSBuild arguments, spliced verbatim\n"
                   "  --cl PATH           explicit cl.exe (skips discovery)\n"
                   "  --msbuild PATH      explicit MSBuild.exe (skips vswhere)\n"
                   "  -o DIR              layout folder override (default\n"
                   "                      <root>/.ccg; works with --all too)\n"
                   "  --compile-commands F  override the generated cdb path\n"
                   "  --clang-compile-flags F\n"
                   "                      extra clang flags appended to every\n"
                   "                      replayed argv (repeatable; not written\n"
                   "                      into compile_commands.json; the\n"
                   "                      per-database clang-compile-flags config\n"
                   "                      key applies when the flag is absent)\n"
                   "  --jobs N            parallel workers for the module BMI\n"
                   "                      build and the parse pool, 1-64 (same\n"
                   "                      precedence as index; per-database\n"
                   "                      default via the index-jobs config key)\n"
                   "  --force             recreate the db on schema version\n"
                   "                      mismatch - now the msbuild DEFAULT on\n"
                   "                      mismatch; the flag stays as the explicit\n"
                   "                      spelling (like index)\n"
                   "  --list              print the per-configuration registry\n"
                   "                      with per-run mode (always at\n"
                   "                      <root>/.ccg/msbuild.json)\n"
                   "\n"
                   "  Exit codes: 0 success (failed TUs allowed, like index),\n"
                   "  1 runtime error (no MSBuild, empty capture/composition),\n"
                   "  2 usage. Evaluation and capture failures preserve the\n"
                   "  previous corpus: cdb/library/registry untouched, exit 1.\n"
                   "  Windows-only by design: --list and the registry work\n"
                   "  everywhere, extraction reports not-supported elsewhere.\n";
        case Invocation::Cmd::Config:
            return "usage: ccg config get [key] [--config NAME] [-o DIR] [--json]\n"
                   "       ccg config set <key> <value> [--config NAME] [-o DIR]\n"
                   "       ccg config unset <key> [--config NAME] [-o DIR]\n"
                   "\n"
                   "  Per-database configuration. One database is parsed by\n"
                   "  exactly one LLVM engine (its parse worldview); the engine\n"
                   "  identity is recorded with the results it produced.\n"
                   "  Database discovery like every query command (-o folder,\n"
                   "  $CCG_DB, single configuration auto-selects).\n"
                   "\n"
                   "  get [key]          print the configuration (one key or all);\n"
                   "                     names the effective engine resolution\n"
                   "  set <key> <value>  llvm-path: the LLVM installation to parse\n"
                   "                     with - an LLVM prefix or the libclang\n"
                   "                     library file itself\n"
                   "  unset <key>        clear back to default resolution\n"
                   "                     ($CCG_LLVM_ROOT, then PATH discovery)\n"
                   "\n"
                   "  Keys: llvm-path (engine); update-lease-ms and\n"
                   "  update-busy-timeout-ms (`ccg update` coordination, defaults\n"
                   "  300000 and 600000); index-jobs\n"
                   "  (worker cap for the module-BMI build and the parse pool of\n"
                   "  index/update/msbuild, 0 = auto, 1-64; the --jobs flag wins\n"
                   "  for its run); clang-compile-\n"
                   "  flags (extra clang flags appended to every replayed argv by\n"
                   "  index/update/msbuild; the --clang-compile-flags command\n"
                   "  line wins for that run). Changing or\n"
                   "  clearing llvm-path prints a rebuild advisory: every result\n"
                   "  in this database was produced by the previous engine - run\n"
                   "  `ccg index --full --config <name>` (a plain re-run also\n"
                   "  detects the change and forces full).\n"
                   "\n"
                   "  Exit 0 ok, 1 runtime error (bad path, missing db for get),\n"
                   "  2 usage.\n";
        case Invocation::Cmd::Hook:
            return "usage: ccg hook install --project|--user [--json]\n"
                   "       ccg hook uninstall --project|--user [--force] [--json]\n"
                   "       ccg hook status --project|--user [--json]\n"
                   "\n"
                   "  Manages ccg's entries in a Claude Code settings.json:\n"
                   "  a PostToolUse hook refreshes the database after every\n"
                   "  Edit/Write/MultiEdit/Update and reports\n"
                   "  the file's blast radius; a SessionStart hook reports the\n"
                   "  database status; a\n"
                   "  PreToolUse hook answers every Grep with the target\n"
                   "  symbol's graph facts (definition, same-name rivals, top\n"
                   "  callers, dispatch tiers; silent when the pattern names no\n"
                   "  indexed symbol). Entries\n"
                   "  are merged element-wise and stay recognizable - existing\n"
                   "  user entries, comments, and formatting are preserved, and\n"
                   "  uninstall removes exactly ccg's own entries (default is a\n"
                   "  dry run; --force applies).\n"
                   "\n"
                   "  --project  target ./.claude/settings.json (this repo)\n"
                   "  --user     target the home directory's settings.json\n"
                   "\n"
                   "  Commands referencing scripts derive them from THIS\n"
                   "  executable's location (the .ccg directory a build or\n"
                   "  release ships beside it), so nothing portable is baked\n"
                   "  into the settings file. The internal actions `hook\n"
                   "  posttooluse` / `hook sessionstart` / `hook pretooluse`\n"
                   "  are the stdin-JSON\n"
                   "  entry points those scripts forward to; they locate the\n"
                   "  database like every query command ($CCG_DB > .ccg/\n"
                   "  discovery; single configuration auto-selects) and stay\n"
                   "  silent when none is found.\n";
        case Invocation::Cmd::Query:
            return "usage: ccg query <name> [--kind K] [--file SUBSTR] "
                   "[--implements] [--config NAME] [-o DIR] [--json]\n"
                   "\n"
                   "  --implements treats <name> as an interface/base and lists the\n"
                   "  types deriving from it (extends edges, direct first, depth\n"
                   "  column marks each level; transitive included)\n";
        case Invocation::Cmd::Context:
            return "usage: ccg context <sym> [--kind K] [--file SUBSTR] "
                   "[--exclude-glob G] [--no-merge] "
                   "[--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] "
                   "[--json]\n"
                   "\n"
                   "  --no-merge  skip override-family merging (query the symbol\n"
                   "              alone; virtual methods otherwise merge their\n"
                   "              override family's edges, each row marked)\n"
                   "  --exclude-glob G  drop edges whose far symbol is defined in\n"
                   "                     a path matching G (e.g. build/** or\n"
                   "                     _deps/*; repeatable)\n"
                   "  --no-dispatch      pure static view (byte-identical to the\n"
                   "                     pre-dispatch default; the conservative\n"
                   "                     upper bound)\n"
                   "  --dispatch-fallback  also show 0.0 family marker edges as\n"
                   "                     family estimates (does not change which\n"
                   "                     symbols are considered)\n";
        case Invocation::Cmd::Members:
            return "usage: ccg members <record> [--kind K] [--file SUBSTR] "
                   "[--config NAME] [-o DIR] [--json]\n"
                   "\n"
                   "  Lists the direct members of a record - methods, constructors,\n"
                   "  destructors, fields, nested types - with file:line, in\n"
                   "  declaration order. --kind/--file narrow the listing like\n"
                   "  every symbol command.\n"
                   "\n"
                   "  Exit 0 with an empty listing for a record without members;\n"
                   "  1 when the symbol is not a record or cannot be resolved.\n";
        case Invocation::Cmd::Callers:
            return "usage: ccg callers <sym> [--depth N] [--top N] [--kind K] "
                   "[--file SUBSTR] [--exclude-glob G] [--no-merge] "
                   "[--no-dispatch|--dispatch-fallback] [--config NAME] "
                   "[-o DIR] [--json]\n"
                   "\n"
                   "  --depth N   transitive depth 1-10 (default 1)\n"
                   "  --top N     list at most N entries (default unlimited; a\n"
                   "              '... and N more' line marks the cut, totals and\n"
                   "              counts keep covering every entry found)\n"
                   "  --exclude-glob G  drop entries whose defining path matches G\n"
                   "                     (e.g. build/** or _deps/*; repeatable;\n"
                   "                     totals follow the filtered view)\n"
                   "  --no-merge  skip override-family merging (virtual methods\n"
                   "              otherwise merge dispatch sites across the family)\n"
                   "  --no-dispatch      pure static view (the conservative upper\n"
                   "                     bound; byte-identical to the old default)\n"
                   "  --dispatch-fallback  also show 0.0 family marker edges as\n"
                   "                     family estimates\n"
                   "\n"
                   "  By default a call site with confidence >= 0.7 dynamic\n"
                   "  evidence (1.0/0.7) is attributed to the proven dispatch\n"
                   "  target instead of the whole override family. A 0.4 weak\n"
                   "  inference or a 0.0 marker is not evidence: the result stays\n"
                   "  a family estimate, marked [dispatch unknown · family\n"
                   "  estimate] (dynamic type unproven; estimated from the whole\n"
                   "  override family, may include non-target implementations).\n";
        case Invocation::Cmd::Callees:
            return "usage: ccg callees <sym> [--depth N] [--top N] [--kind K] "
                   "[--file SUBSTR] [--exclude-glob G] [--no-merge] "
                   "[--no-dispatch|--dispatch-fallback] [--config NAME] "
                   "[-o DIR] [--json]\n"
                   "\n"
                   "  --depth N   transitive depth 1-10 (default 1)\n"
                   "  --top N     list at most N entries (default unlimited; a\n"
                   "              '... and N more' line marks the cut, totals and\n"
                   "              counts keep covering every entry found)\n"
                   "  --exclude-glob G  drop entries whose defining path matches G\n"
                   "                     (e.g. build/** or _deps/*; repeatable;\n"
                   "                     totals follow the filtered view)\n"
                   "  --no-merge  skip override-family merging (virtual methods\n"
                   "              otherwise merge dispatch sites across the family)\n"
                   "  --no-dispatch      pure static view (the conservative upper\n"
                   "                     bound; byte-identical to the old default)\n"
                   "  --dispatch-fallback  also show 0.0 family marker edges as\n"
                   "                     family estimates\n";
        case Invocation::Cmd::Impact:
            return "usage: ccg impact <sym> [--direction up|down] [--depth N] "
                   "[--top N] [--exclude-glob G] [--no-merge] "
                   "[--no-dispatch|--dispatch-fallback] [--config NAME] "
                   "[-o DIR] [--json]\n"
                   "\n"
                   "  --direction  up = what depends on this (default), down = what\n"
                   "               this depends on\n"
                   "  --depth N    1-10 (default 3)\n"
                   "  --top N      at most N entries per depth bucket (default\n"
                   "               unlimited; '... and N more' lines mark the cut,\n"
                   "               impacted/direct/transitive keep counting\n"
                   "               everything found)\n"
                   "  --exclude-glob G  drop entries whose defining path matches G\n"
                   "                     (e.g. build/** or _deps/*; repeatable;\n"
                   "                     impacted counts follow the filtered view,\n"
                   "                     risk/score stay at the unfiltered upper\n"
                   "                     bound)\n"
                   "  --no-merge   skip override-family merging (virtual methods\n"
                   "               otherwise report the whole family's blast "
                   "radius)\n"
                   "  --no-dispatch      pure static view (the conservative upper\n"
                   "                     bound; byte-identical to the old default)\n"
                   "  --dispatch-fallback  also show 0.0 family marker edges as\n"
                   "                     family estimates\n"
                   "\n"
                   "  The risk score uses the precise dispatch result; family\n"
                   "  estimates never raise the band.\n";
        case Invocation::Cmd::Trace:
            return "usage: ccg trace <a> <b> [--kind K] [--file SUBSTR] "
                   "[--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] "
                   "[--json]\n"
                   "\n"
                   "  --no-dispatch      follow only static calls edges\n"
                   "  --dispatch-fallback  accepted for symmetry; 0.0 family "
                   "markers\n"
                   "                     never participate in path finding\n";
        case Invocation::Cmd::Changes:
            return "usage: ccg changes [<file>...] [--from-file LIST] [--depth N] "
                   "[--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] "
                   "[--json]\n"
                   "\n"
                   "  <file>          changed file paths (absolute, or relative to\n"
                   "                  the working directory; '-' reads stdin)\n"
                   "  --from-file LIST  read one path per line from LIST ('-' =\n"
                   "                  stdin); paths are the union with <file>,\n"
                   "                  deduplicated after normalization\n"
                   "  --depth N       upstream propagation depth 1-10 (default 3)\n"
                   "  --no-dispatch   pure static propagation (the conservative\n"
                   "                  upper bound)\n"
                   "  --dispatch-fallback  accepted for symmetry; 0.0 family "
                   "markers\n"
                   "                  never add consumers\n"
                   "\n"
                   "  With no <file> and no --from-file, and stdin piped, the path\n"
                   "  list is read from stdin (git diff --name-only | ccg changes).\n"
                   "  ccg never invokes any version-control tool; you feed the\n"
                   "  changed-file list. Files are mapped to every symbol they\n"
                   "  define (deleted files keep their symbols in the db until the\n"
                   "  next index run); consumers propagate upstream over calls,\n"
                   "  extends, uses, and override families. Exit 1 when nothing is\n"
                   "  found (no known files, or no consumers).\n";
        case Invocation::Cmd::Check:
            return "usage: ccg check --cycles [--max N] [--config NAME] [-o DIR] "
                   "[--json]\n"
                   "\n"
                   "  --cycles  detect include-graph cycles from the includes table\n"
                   "  --max N   cycle groups listed, 1-1000 (default 20; the\n"
                   "            totals always cover every group found)\n"
                   "\n"
                   "  Exit 0 = no cycles, 3 = cycles found (usable as a gate),\n"
                   "  1 = runtime error, 2 = usage error.\n";
        case Invocation::Cmd::Graph:
            return "usage: ccg graph (--symbol <sym> | --modules) [--depth N]\n"
                   "                 [--min-weight N] [--kind K] [--file SUBSTR]\n"
                   "                 [--format dot|json] [--output FILE] "
                   "[--config NAME] [-o DIR] [--json]\n"
                   "\n"
                   "  Export the call graph as Graphviz dot (default) or JSON.\n"
                   "\n"
                   "  --symbol <sym>  call-graph expansion from an entry symbol\n"
                   "                  over calls edges (what it calls), BFS;\n"
                   "                  --depth N is 0 (unlimited) or 1-10 (default "
                   "2)\n"
                   "  --modules       module-level aggregate: symbols grouped by\n"
                   "                  their defining file (label = file stem),\n"
                   "                  each distinct symbol-level calls pair counts\n"
                   "                  one edge weight; same-file calls never appear\n"
                   "  --min-weight N  --modules only: drop aggregate edges below N\n"
                   "                  pairs (default 1 = keep everything; 3 reads\n"
                   "                  well at repo scale)\n"
                   "  --kind K /      --symbol: narrow the entry resolution;\n"
                   "  --file SUBSTR   --modules: scope which files participate\n"
                   "  --format F      dot (default) or json; --json implies json\n"
                   "  --output FILE   write the payload to FILE instead of stdout\n"
                   "                  (a one-line note goes to stderr)\n"
                   "\n"
                   "  Exit 0 success (an entry with no callees still exports a\n"
                   "  single-node graph); 1 unknown/ambiguous symbol or missing\n"
                   "  db; 2 usage.\n";
        case Invocation::Cmd::Stats:
            return "usage: ccg stats [--config NAME] [-o DIR] [--json]\n";
        default:
            return top_usage();
        }
    }

    /** @brief 命令名 -> 枚举（未知返回空）。 */
    [[nodiscard]]
    auto command_of(std::string_view word) -> std::optional<Invocation::Cmd>
    {
        static constexpr std::pair<std::string_view, Invocation::Cmd> WORDS[] =
            {
                {"index", Invocation::Cmd::Index},
                {"update", Invocation::Cmd::Update},
                {"msbuild", Invocation::Cmd::Msbuild},
                {"config", Invocation::Cmd::Config},
                {"hook", Invocation::Cmd::Hook},
                {"query", Invocation::Cmd::Query},
                {"context", Invocation::Cmd::Context},
                {"members", Invocation::Cmd::Members},
                {"callers", Invocation::Cmd::Callers},
                {"callees", Invocation::Cmd::Callees},
                {"impact", Invocation::Cmd::Impact},
                {"trace", Invocation::Cmd::Trace},
                {"stats", Invocation::Cmd::Stats},
                {"changes", Invocation::Cmd::Changes},
                {"check", Invocation::Cmd::Check},
                {"graph", Invocation::Cmd::Graph},
        };
        for (const auto& [name, cmd] : WORDS)
        {
            if (name == word)
            {
                return cmd;
            }
        }
        return std::nullopt;
    }

    /** @brief 枚举 -> 命令名。 */
    [[nodiscard]]
    auto name_of(Invocation::Cmd cmd) -> std::string_view
    {
        switch (cmd)
        {
        case Invocation::Cmd::Index:
            return "index";
        case Invocation::Cmd::Update:
            return "update";
        case Invocation::Cmd::Msbuild:
            return "msbuild";
        case Invocation::Cmd::Config:
            return "config";
        case Invocation::Cmd::Hook:
            return "hook";
        case Invocation::Cmd::Query:
            return "query";
        case Invocation::Cmd::Context:
            return "context";
        case Invocation::Cmd::Members:
            return "members";
        case Invocation::Cmd::Callers:
            return "callers";
        case Invocation::Cmd::Callees:
            return "callees";
        case Invocation::Cmd::Impact:
            return "impact";
        case Invocation::Cmd::Trace:
            return "trace";
        case Invocation::Cmd::Stats:
            return "stats";
        case Invocation::Cmd::Changes:
            return "changes";
        case Invocation::Cmd::Check:
            return "check";
        case Invocation::Cmd::Graph:
            return "graph";
        default:
            return "help";
        }
    }

    /** @brief --kind 词 -> SymbolKind（未知返回空）。 */
    [[nodiscard]]
    auto parse_kind(std::string_view word) -> std::optional<cg::db::SymbolKind>
    {
        using K = cg::db::SymbolKind;
        static constexpr std::pair<std::string_view, K> KINDS[] = {
            {"record", K::Record},
            {"enum", K::Enumeration},
            {"enum_constant", K::EnumConstant},
            {"function", K::Function},
            {"method", K::Method},
            {"constructor", K::Constructor},
            {"destructor", K::Destructor},
            {"field", K::Field},
            {"variable", K::Variable},
            {"type_alias", K::TypeAlias},
        };
        for (const auto& [name, kind] : KINDS)
        {
            if (name == word)
            {
                return kind;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 深度文本 -> [1, 10] 内的整数；非法返回空。
     *
     * @internal 数字累加并在超上限时提前钳制：荒谬的数字串饱和而非使
     * 有符号 int 溢出。
     */
    [[nodiscard]]
    auto parse_depth(std::string_view text) -> std::optional<int>
    {
        if (text.empty()
            || !std::ranges::all_of(
                text,
                [](char c) { return c >= '0' && c <= '9'; }))
        {
            return std::nullopt;
        }
        unsigned long long value = 0;
        for (const char c : text)
        {
            value = value * 10 + static_cast<unsigned long long>(c - '0');
            if (value > 10ull)
            {
                break;
            }
        }
        if (value > 10)
        {
            return std::nullopt;
        }
        return static_cast<int>(value);
    }

    /**
     * @brief 配置名记号会进入目录名（`<文件夹>`/`<配置>`/），故必须是单个
     *        路径安全的路径段：非空、无空白、不含 Windows 文件名禁用的
     *        字符（外加 MSBuild 中已用于分隔配对的 '|'）。
     */
    [[nodiscard]]
    auto valid_config_token(std::string_view value) -> bool
    {
        if (value.empty())
        {
            return false;
        }
        return std::ranges::none_of(
            value,
            [](char c)
            {
                return c == ' ' || c == '\t' || c == '/' || c == '\\'
                       || c == ':' || c == '*' || c == '?' || c == '"'
                       || c == '<' || c == '>' || c == '|'
                       || static_cast<unsigned char>(c) < 0x20;
            });
    }

    /** @brief 默认产出文件夹：$CCG_DB 或 ./.ccg（二者皆指文件夹）。 */
    [[nodiscard]]
    auto default_output_dir() -> std::string
    {
        if (const auto env = cg::util::cstd::get_env("CCG_DB"))
        {
            return *env;
        }
        return "./.ccg";
    }

    /**
     * @brief 每个命令接受哪些选项（各命令用法 synopsis 行的一一镜像）。
     */
    struct AllowedOpts
    {
        bool force = false;
        bool full = false;
        bool dry_run = false;
        bool paranoid = false;
        bool depth = false;
        bool kind = false;
        bool file = false;
        bool direction = false;
        bool no_merge = false;
        bool dispatch = false;
        bool top = false;          // callers/callees/impact：--top N 截断
        bool exclude_glob = false; // 结果端路径排除：--exclude-glob G
        bool implements = false;   // query：--implements 派生树模式
        bool compile_commands = false;
        bool jobs = false;
        bool llvm_root = false;
        bool clang_compile_flags = false; // index/update/msbuild：注入旗标
        bool from_file = false;
        bool cycles = false;
        bool max = false;
        bool config = false; // --config（布局配置名；msbuild 另有自己的语义）
        bool msbuild_config = false;
        bool msbuild_platform = false;
        bool msbuild_all = false;
        bool msbuild_export_only = false;
        bool msbuild_list = false;
        bool msbuild_record = false;
        bool msbuild_full_build = false;
        bool msbuild_allow_failure = false;
        bool msbuild_args = false;
        bool msbuild_cl = false;
        bool msbuild_exe = false;
        bool drain = false;
        bool wait = false;       // index/msbuild：--wait/--no-wait（租约排队）
        bool hook_scope = false; // hook：--user / --project
        bool graph_symbol = false;  // graph：--symbol
        bool graph_modules = false; // graph：--modules
        bool graph_format = false;  // graph：--format
        bool graph_output = false;  // graph：--output
        bool min_weight = false;    // graph：--min-weight
    };

    /** @brief 逐命令的选项允许集。 */
    [[nodiscard]]
    auto allowed_for(Invocation::Cmd cmd) -> AllowedOpts
    {
        switch (cmd)
        {
        case Invocation::Cmd::Index:
            return {
                .force = true,
                .full = true,
                .dry_run = true,
                .paranoid = true,
                .compile_commands = true,
                .jobs = true,
                .llvm_root = true,
                .clang_compile_flags = true,
                .config = true,
                .wait = true};
        case Invocation::Cmd::Update:
            return {
                .compile_commands = true,
                .jobs = true,
                .llvm_root = true,
                .clang_compile_flags = true,
                .config = true,
                .drain = true};
        case Invocation::Cmd::Msbuild:
            return {
                .force = true,
                .compile_commands = true,
                .jobs = true,
                .clang_compile_flags = true,
                .msbuild_config = true,
                .msbuild_platform = true,
                .msbuild_all = true,
                .msbuild_export_only = true,
                .msbuild_list = true,
                .msbuild_record = true,
                .msbuild_full_build = true,
                .msbuild_allow_failure = true,
                .msbuild_args = true,
                .msbuild_cl = true,
                .msbuild_exe = true,
                .wait = true};
        case Invocation::Cmd::Hook:
            return {.force = true, .config = true, .hook_scope = true};
        case Invocation::Cmd::Config:
            return {.config = true};
        case Invocation::Cmd::Query:
            return {
                .kind = true,
                .file = true,
                .implements = true,
                .config = true};
        case Invocation::Cmd::Members:
            return {.kind = true, .file = true, .config = true};
        case Invocation::Cmd::Trace:
            return {
                .kind = true,
                .file = true,
                .dispatch = true,
                .config = true};
        case Invocation::Cmd::Context:
            return {
                .kind = true,
                .file = true,
                .no_merge = true,
                .dispatch = true,
                .exclude_glob = true,
                .config = true};
        case Invocation::Cmd::Callers:
        case Invocation::Cmd::Callees:
            return {
                .depth = true,
                .kind = true,
                .file = true,
                .no_merge = true,
                .dispatch = true,
                .top = true,
                .exclude_glob = true,
                .config = true};
        case Invocation::Cmd::Impact:
            return {
                .depth = true,
                .kind = true,
                .file = true,
                .direction = true,
                .no_merge = true,
                .dispatch = true,
                .top = true,
                .exclude_glob = true,
                .config = true};
        case Invocation::Cmd::Changes:
            return {
                .depth = true,
                .dispatch = true,
                .from_file = true,
                .config = true};
        case Invocation::Cmd::Check:
            return {.cycles = true, .max = true, .config = true};
        case Invocation::Cmd::Stats:
            return {.config = true};
        case Invocation::Cmd::Graph:
            return {
                .depth = true,
                .kind = true,
                .file = true,
                .config = true,
                .graph_symbol = true,
                .graph_modules = true,
                .graph_format = true,
                .graph_output = true,
                .min_weight = true};
        default:
            return {};
        }
    }

    /**
     * @brief 解析不含 argv[0] 的 argv；任何契约违例抛 UsageError。
     *
     * @internal
     * 操作数边界：trace 恰两个符号（先查——trace 也接受符号）、index 零或
     * 多个根（零 = 档案复跑，运行期无档案时再报可行动错误）、query 类恰一
     * 个、changes 零或多个文件（零 = 管道时读 stdin）、stats 与 check 无。
     * "--opt=value" 拆分使两种拼写统一生效。数字选项（--max、--jobs）同样
     * 数字累加 + 超上限提前钳制（荒谬数字串饱和而非溢出）。操作数计数逐命
     * 令校验。ccg check 需要其模式旗标，在解析期校验使用法错误胜过后续任何
     * 运行时错误（如库缺失）。ccg update 无文件又无 --drain 时无可刷新、无
     * 队列可收敛——解析期拒绝并给出 drain 提示。ccg index 的 --config 永不
     * 可省、-o 只许文件夹（文件路径解析期响亮报错）。
     * ccg graph 双模式互斥（--symbol 与 --modules 恰其一），--depth 只属
     * 入口展开（默认 2：调研实证深度 3 的入口图过大）、--min-weight 只属
     * 模块聚合，错配在解析期拒绝。
     * ccg msbuild 组合规则：--list 是纯注册表读；--all
     * 扇出到每个声明配对；两者都拒绝会点名或引导单次构建的选项；操作数仅
     * --list 可选（此时以 cwd 为注册表根）。逐命令深度默认：impact 与
     * changes 默认比直接邻域走得更远（两者默认深度 3；callers/callees
     * 直接邻域为 1）。
     */
    [[nodiscard]]
    auto parse_args(std::span<const std::string> args) -> Invocation
    {
        if (args.empty())
        {
            throw UsageError{"missing command", top_usage()};
        }

        Invocation inv;
        inv.output_dir = default_output_dir();

        const std::string& first = args.front();
        if (first == "--version")
        {
            inv.cmd = Invocation::Cmd::Version;
            return inv;
        }
        if (first == "help")
        {
            inv.cmd = Invocation::Cmd::Help;
            if (args.size() > 2)
            {
                throw UsageError{
                    "help takes at most one command name",
                    top_usage()};
            }
            if (args.size() == 2)
            {
                if (!command_of(args[1]).has_value())
                {
                    throw UsageError{
                        std::format("unknown command '{}'", args[1]),
                        top_usage()};
                }
                inv.positional.push_back(args[1]);
            }
            return inv;
        }
        if (first == "--help" || first == "-h")
        {
            if (args.size() > 1)
            {
                throw UsageError{"--help takes no arguments", top_usage()};
            }
            inv.cmd = Invocation::Cmd::Help;
            return inv;
        }
        const auto cmd = command_of(first);
        if (!cmd.has_value())
        {
            if (first.starts_with('-'))
            {
                throw UsageError{
                    "unknown option '" + first
                        + "' (a command must come first)",
                    top_usage()};
            }
            throw UsageError{
                std::format("unknown command '{}'", first),
                top_usage()};
        }
        inv.cmd = *cmd;

        const AllowedOpts      allowed = allowed_for(*cmd);
        const std::string_view usage = usage_for(*cmd);
        bool                   depth_seen = false;

        const bool wants_symbol = *cmd == Invocation::Cmd::Query
                                  || *cmd == Invocation::Cmd::Context
                                  || *cmd == Invocation::Cmd::Members
                                  || *cmd == Invocation::Cmd::Callers
                                  || *cmd == Invocation::Cmd::Callees
                                  || *cmd == Invocation::Cmd::Impact
                                  || *cmd == Invocation::Cmd::Trace;

        const std::size_t min_operands =
            *cmd == Invocation::Cmd::Trace ? 2
            : (wants_symbol || *cmd == Invocation::Cmd::Config
               || *cmd == Invocation::Cmd::Hook)
                ? 1
                : 0; // index 根可缺（档案复跑）
        const std::size_t max_operands =
            (*cmd == Invocation::Cmd::Index || *cmd == Invocation::Cmd::Changes
             || *cmd == Invocation::Cmd::Update)
                ? 1000
                : (*cmd == Invocation::Cmd::Msbuild
                       ? 1
                       : (*cmd == Invocation::Cmd::Config
                              ? 3
                              : (*cmd == Invocation::Cmd::Hook
                                     ? 1
                                     : min_operands)));

        bool saw_no_dispatch = false;
        bool saw_dispatch_fallback = false;
        // `ccg config` 的值操作数可以以 '-' 开头（clang-compile-flags 的
        // 旗标文本，如 "-DFOO -isystem `<dir>`"，从真实 shell 引号喂入仍是
        // 一个 '-' 前缀 token）。config 的选项面极小（-o/--config/--json/
        // -v/-h），不在其列的 '-' 前缀 token 按操作数对待——否则该键的
        // 值永远到不了运行器。
        const bool dash_value_is_operand = *cmd == Invocation::Cmd::Config;
        const auto config_known_option = [](std::string_view name)
        {
            constexpr std::string_view known[] =
                {"-o", "--config", "--json", "-v", "--verbose", "-h", "--help"};
            for (const std::string_view k : known)
            {
                if (name == k)
                {
                    return true;
                }
            }
            return false;
        };
        for (std::size_t i = 1; i < args.size(); ++i)
        {
            const std::string& token = args[i];
            if (!token.starts_with('-') || token == "-")
            {
                inv.positional.push_back(token);
                continue;
            }
            if (dash_value_is_operand)
            {
                const std::size_t      eq = token.find('=');
                const std::string_view probe =
                    std::string_view{token}.substr(0, eq);
                if (!config_known_option(
                        eq == std::string::npos ? std::string_view{token}
                                                : probe))
                {
                    inv.positional.push_back(token);
                    continue;
                }
            }

            std::string name = token;
            std::string inline_value;
            bool        has_inline = false;
            if (const std::size_t eq = token.find('='); eq != std::string::npos)
            {
                name = token.substr(0, eq);
                inline_value = token.substr(eq + 1);
                has_inline = true;
            }
            const auto take_value = [&]() -> std::string
            {
                if (has_inline)
                {
                    return inline_value;
                }
                if (i + 1 >= args.size())
                {
                    throw UsageError{
                        std::format("option '{}' needs a value", name),
                        usage};
                }
                return args[++i];
            };
            const auto reject_for_command = [&]()
            {
                throw UsageError{
                    std::format(
                        "option '{}' is not valid for command '{}'",
                        name,
                        name_of(inv.cmd)),
                    usage};
            };

            if (name == "-h" || name == "--help")
            {
                inv.cmd = Invocation::Cmd::Help;
                inv.positional.clear();
                inv.positional.emplace_back(name_of(*cmd));
                return inv;
            }
            if (name == "-o")
            {
                const std::string v = take_value();
                // -o 只许产出文件夹；文件路径响亮报错。
                std::error_code             fec;
                const std::filesystem::path probe{v};
                if (v.ends_with(".db") || v == ":memory:"
                    || (std::filesystem::is_regular_file(probe, fec) && !fec))
                {
                    throw UsageError{
                        "-o takes the output FOLDER (default ./.ccg): "
                        "configurations live inside it as <config>/ccg.db - you "
                        "passed the file path '"
                            + v
                            + "'; use `-o <folder> --config <name>` (query side: "
                              "`-o <folder>` auto-selects the single configuration)",
                        usage};
                }
                inv.output_dir = v;
                inv.db_explicit = true;
            }
            else if (name == "--json")
            {
                inv.json = true;
            }
            else if (name == "-v" || name == "--verbose")
            {
                inv.verbose = true;
            }
            else if (name == "--force")
            {
                if (!allowed.force)
                {
                    reject_for_command();
                }
                inv.force = true;
            }
            else if (name == "--full")
            {
                if (!allowed.full)
                {
                    reject_for_command();
                }
                inv.full = true;
            }
            else if (name == "--dry-run")
            {
                if (!allowed.dry_run)
                {
                    reject_for_command();
                }
                inv.dry_run = true;
            }
            else if (name == "--paranoid")
            {
                if (!allowed.paranoid)
                {
                    reject_for_command();
                }
                inv.paranoid = true;
            }
            else if (name == "--depth")
            {
                if (!allowed.depth)
                {
                    reject_for_command();
                }
                const auto d = parse_depth(take_value());
                const bool graph_unlimited =
                    *cmd == Invocation::Cmd::Graph && d.has_value() && *d == 0;
                if (!d.has_value() || (*d == 0 && !graph_unlimited))
                {
                    throw UsageError{
                        *cmd == Invocation::Cmd::Graph
                            ? "--depth must be 0 (unlimited) or an integer "
                              "between 1 and 10"
                            : "--depth must be an integer between 1 and 10",
                        usage};
                }
                inv.depth = *d;
                depth_seen = true;
            }
            else if (name == "--kind")
            {
                if (!allowed.kind)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                const auto        k = parse_kind(v);
                if (!k.has_value())
                {
                    throw UsageError{
                        "unknown kind '" + v
                            + "' (record function method constructor "
                              "destructor field variable enum "
                              "enum_constant type_alias)",
                        usage};
                }
                inv.kind = k;
            }
            else if (name == "--file")
            {
                if (!allowed.file)
                {
                    reject_for_command();
                }
                inv.file_substring = take_value();
            }
            else if (name == "--top")
            {
                if (!allowed.top)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty()
                    || !std::ranges::all_of(
                        v,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw UsageError{
                        "--top must be an integer between 1 and "
                        "100000",
                        usage};
                }
                unsigned long long value = 0;
                for (const char c : v)
                {
                    value =
                        value * 10 + static_cast<unsigned long long>(c - '0');
                    if (value > 100000ull)
                    {
                        break;
                    }
                }
                if (value < 1 || value > 100000)
                {
                    throw UsageError{
                        "--top must be an integer between 1 and "
                        "100000",
                        usage};
                }
                inv.top = static_cast<int>(value);
            }
            else if (name == "--exclude-glob")
            {
                if (!allowed.exclude_glob)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{
                        "--exclude-glob needs a glob pattern "
                        "(e.g. build/** or _deps/*; '**' crosses "
                        "'/', '*' stays within one segment)",
                        usage};
                }
                inv.exclude_globs.push_back(v);
            }
            else if (name == "--direction")
            {
                if (!allowed.direction)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v == "up")
                {
                    inv.direction = cg::graph::Direction::Up;
                }
                else if (v == "down")
                {
                    inv.direction = cg::graph::Direction::Down;
                }
                else
                {
                    throw UsageError{"--direction must be up or down", usage};
                }
            }
            else if (name == "--no-merge")
            {
                if (!allowed.no_merge)
                {
                    reject_for_command();
                }
                inv.no_merge = true;
            }
            else if (name == "--implements")
            {
                if (!allowed.implements)
                {
                    reject_for_command();
                }
                inv.implements = true;
            }
            else if (name == "--no-dispatch")
            {
                if (!allowed.dispatch)
                {
                    reject_for_command();
                }
                saw_no_dispatch = true;
                inv.dispatch_mode = cg::graph::DispatchMode::Off;
            }
            else if (name == "--dispatch-fallback")
            {
                if (!allowed.dispatch)
                {
                    reject_for_command();
                }
                saw_dispatch_fallback = true;
                inv.dispatch_mode = cg::graph::DispatchMode::Fallback;
            }
            else if (name == "--from-file")
            {
                if (!allowed.from_file)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{
                        "--from-file needs a path ('-' reads stdin)",
                        usage};
                }
                inv.from_file = v;
            }
            else if (name == "--cycles")
            {
                if (!allowed.cycles)
                {
                    reject_for_command();
                }
                inv.cycles = true;
            }
            else if (name == "--max")
            {
                if (!allowed.max)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty()
                    || !std::ranges::all_of(
                        v,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw UsageError{
                        "--max must be an integer between 1 and 1000",
                        usage};
                }
                unsigned long long value = 0;
                for (const char c : v)
                {
                    value =
                        value * 10 + static_cast<unsigned long long>(c - '0');
                    if (value > 1000ull)
                    {
                        break;
                    }
                }
                if (value < 1 || value > 1000)
                {
                    throw UsageError{
                        "--max must be an integer between 1 and 1000",
                        usage};
                }
                inv.list_cap = static_cast<int>(value);
            }
            else if (name == "--compile-commands")
            {
                if (!allowed.compile_commands)
                {
                    reject_for_command();
                }
                inv.compile_db = take_value();
            }
            else if (name == "--config")
            {
                const std::string v = take_value();
                if (!valid_config_token(v))
                {
                    throw UsageError{
                        "--config must be a path-safe name (no whitespace or "
                        "'/\\:*?\"<>|' - it becomes a configuration DIRECTORY "
                        "name under the -o folder)",
                        usage};
                }
                if (allowed.config)
                {
                    // 布局配置名：index 必给，查询侧可选。
                    inv.config_name = v;
                    inv.config_explicit = true;
                }
                else if (allowed.msbuild_config)
                {
                    inv.msbuild_config = v;
                }
                else
                {
                    reject_for_command();
                }
            }
            else if (name == "--platform")
            {
                if (!allowed.msbuild_platform)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (!valid_config_token(v))
                {
                    throw UsageError{
                        "--platform must be a path-safe name (no whitespace or "
                        "'/\\:*?\"<>|' - it becomes part of the db file name)",
                        usage};
                }
                inv.msbuild_platform = v;
            }
            else if (name == "--all")
            {
                if (!allowed.msbuild_all)
                {
                    reject_for_command();
                }
                inv.msbuild_all = true;
            }
            else if (name == "--export-only")
            {
                if (!allowed.msbuild_export_only)
                {
                    reject_for_command();
                }
                inv.msbuild_export_only = true;
            }
            else if (name == "--list")
            {
                if (!allowed.msbuild_list)
                {
                    reject_for_command();
                }
                inv.msbuild_list = true;
            }
            else if (name == "--record")
            {
                if (!allowed.msbuild_record)
                {
                    reject_for_command();
                }
                inv.msbuild_record = true;
            }
            else if (name == "--full-build")
            {
                if (!allowed.msbuild_full_build)
                {
                    reject_for_command();
                }
                inv.msbuild_full_build = true;
            }
            else if (name == "--allow-failure")
            {
                if (!allowed.msbuild_allow_failure)
                {
                    reject_for_command();
                }
                inv.msbuild_allow_failure = true;
            }
            else if (name == "--msbuild-args")
            {
                if (!allowed.msbuild_args)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--msbuild-args needs a value", usage};
                }
                inv.msbuild_args = v;
            }
            else if (name == "--cl")
            {
                if (!allowed.msbuild_cl)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--cl needs a path", usage};
                }
                inv.cl_path = v;
            }
            else if (name == "--msbuild")
            {
                if (!allowed.msbuild_exe)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--msbuild needs a path", usage};
                }
                inv.msbuild_path = v;
            }
            else if (name == "--llvm-root")
            {
                if (!allowed.llvm_root)
                {
                    reject_for_command();
                }
                inv.llvm_root = take_value();
            }
            else if (name == "--clang-compile-flags")
            {
                if (!allowed.clang_compile_flags)
                {
                    reject_for_command();
                }
                const std::string              v = take_value();
                const std::vector<std::string> tokens =
                    cg::util::tokenize_command(v);
                if (tokens.empty())
                {
                    throw UsageError{
                        "--clang-compile-flags needs flags (e.g. "
                        "\"-DFOO -isystem <vendor>/include\"; repeatable, "
                        "later occurrences override earlier ones)",
                        usage};
                }
                inv.clang_compile_flags.insert(
                    inv.clang_compile_flags.end(),
                    tokens.begin(),
                    tokens.end());
            }
            else if (name == "--drain")
            {
                if (!allowed.drain)
                {
                    reject_for_command();
                }
                inv.drain = true;
            }
            else if (name == "--symbol")
            {
                if (!allowed.graph_symbol)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--symbol needs a symbol name", usage};
                }
                inv.graph_symbol = v;
            }
            else if (name == "--modules")
            {
                if (!allowed.graph_modules)
                {
                    reject_for_command();
                }
                inv.graph_modules = true;
            }
            else if (name == "--format")
            {
                if (!allowed.graph_format)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v != "dot" && v != "json")
                {
                    throw UsageError{"--format must be dot or json", usage};
                }
                inv.graph_format = v;
            }
            else if (name == "--output")
            {
                if (!allowed.graph_output)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty())
                {
                    throw UsageError{"--output needs a path", usage};
                }
                inv.output_file = v;
            }
            else if (name == "--min-weight")
            {
                if (!allowed.min_weight)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty()
                    || !std::ranges::all_of(
                        v,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw UsageError{
                        "--min-weight must be an integer between 1 "
                        "and 100000",
                        usage};
                }
                unsigned long long value = 0;
                for (const char c : v)
                {
                    value =
                        value * 10 + static_cast<unsigned long long>(c - '0');
                    if (value > 100000ull)
                    {
                        break;
                    }
                }
                if (value < 1 || value > 100000)
                {
                    throw UsageError{
                        "--min-weight must be an integer between 1 "
                        "and 100000",
                        usage};
                }
                inv.min_weight = static_cast<int>(value);
            }
            else if (name == "--user")
            {
                if (!allowed.hook_scope)
                {
                    reject_for_command();
                }
                inv.hook_user = true;
            }
            else if (name == "--project")
            {
                if (!allowed.hook_scope)
                {
                    reject_for_command();
                }
                inv.hook_project = true;
            }
            else if (name == "--jobs")
            {
                if (!allowed.jobs)
                {
                    reject_for_command();
                }
                const std::string v = take_value();
                if (v.empty()
                    || !std::ranges::all_of(
                        v,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw UsageError{
                        "--jobs must be an integer between 1 and 64",
                        usage};
                }
                unsigned long long value = 0;
                for (const char c : v)
                {
                    value =
                        value * 10 + static_cast<unsigned long long>(c - '0');
                    if (value > 64ull)
                    {
                        break;
                    }
                }
                if (value < 1 || value > 64)
                {
                    throw UsageError{
                        "--jobs must be an integer between 1 and 64",
                        usage};
                }
                inv.jobs = static_cast<int>(value);
            }
            else if (name == "--wait")
            {
                if (!allowed.wait)
                {
                    reject_for_command();
                }
                // 排队等待默认开：--wait 本身不动默认上界，收紧上界走
                // --wait=`<ms>` 内联形式（空格形式会与 index 的根目录操作数
                // 歧义，故不支持）。
                if (inv.no_wait)
                {
                    throw UsageError{
                        "--wait and --no-wait are mutually "
                        "exclusive (the run either queues behind a "
                        "live update session or fails fast)",
                        usage};
                }
                if (has_inline)
                {
                    if (inline_value.empty()
                        || !std::ranges::all_of(
                            inline_value,
                            [](char c) { return c >= '0' && c <= '9'; }))
                    {
                        throw UsageError{
                            "--wait=<ms> must be milliseconds (e.g. --wait=5000)",
                            usage};
                    }
                    unsigned long long value = 0;
                    for (const char c : inline_value)
                    {
                        value = value * 10
                                + static_cast<unsigned long long>(c - '0');
                        if (value > 86'400'000ull)
                        { // 24 h：荒谬上界即钳制
                            break;
                        }
                    }
                    if (value < 1 || value > 86'400'000ull)
                    {
                        throw UsageError{
                            "--wait=<ms> must be between 1 ms and 24 h",
                            usage};
                    }
                    inv.wait_limit_ms = static_cast<std::int64_t>(value);
                    inv.wait_cap_seen = true;
                }
            }
            else if (name == "--no-wait")
            {
                if (!allowed.wait)
                {
                    reject_for_command();
                }
                if (inv.wait_cap_seen)
                {
                    throw UsageError{
                        "--wait and --no-wait are mutually "
                        "exclusive (the run either queues behind a "
                        "live update session or fails fast)",
                        usage};
                }
                inv.no_wait = true;
            }
            else
            {
                throw UsageError{
                    std::format("unknown option '{}'", name),
                    usage};
            }
        }

        if (inv.positional.size() < min_operands)
        {
            throw UsageError{
                *cmd == Invocation::Cmd::Trace
                    ? std::string{"trace needs two symbol names"}
                    : (*cmd == Invocation::Cmd::Index
                           ? std::string{"missing project root directory"}
                           : (*cmd == Invocation::Cmd::Config
                                  ? std::string{"config needs an action: get, "
                                                "set, or unset"}
                                  : (*cmd == Invocation::Cmd::Hook
                                         ? std::
                                               string{"hook needs an action: install, "
                                                      "uninstall, or status"}
                                         : std::
                                               string{"missing symbol name"}))),
                usage};
        }
        if (inv.positional.size() > max_operands)
        {
            throw UsageError{
                std::format("unexpected argument '{}'", inv.positional.back()),
                usage};
        }

        if (*cmd == Invocation::Cmd::Check && !inv.cycles)
        {
            throw UsageError{
                "check needs a mode; the only one today is --cycles",
                usage};
        }

        if (*cmd == Invocation::Cmd::Update && inv.positional.empty()
            && !inv.drain)
        {
            throw UsageError{
                "update needs file arguments or --drain (a bare queue drain is "
                "`ccg update --drain`)",
                usage};
        }

        if (*cmd == Invocation::Cmd::Index && !inv.config_explicit)
        {
            throw UsageError{
                "index requires --config <name> (it names the configuration "
                "directory <folder>/<name>/ that holds ccg.db, the BMI cache "
                "and the recorded profile; never omitted)",
                usage};
        }

        if (*cmd == Invocation::Cmd::Msbuild)
        {
            if (inv.msbuild_list)
            {
                if (inv.msbuild_all || inv.msbuild_export_only
                    || inv.msbuild_record || inv.msbuild_full_build
                    || inv.msbuild_allow_failure
                    || !inv.msbuild_config.empty()
                    || !inv.msbuild_platform.empty()
                    || !inv.msbuild_args.empty() || !inv.cl_path.empty()
                    || !inv.msbuild_path.empty() || inv.force || inv.db_explicit
                    || !inv.compile_db.empty())
                {
                    throw UsageError{
                        "--list only prints the registry; build "
                        "options do not apply to it",
                        usage};
                }
            }
            else
            {
                if (inv.positional.empty())
                {
                    throw UsageError{
                        "msbuild needs a solution file (.sln) "
                        "(or --list)",
                        usage};
                }
                if (inv.msbuild_record && inv.msbuild_full_build)
                {
                    throw UsageError{
                        "--record (wrapped-build capture) and --full-build "
                        "(real compilation) are different capture channels; "
                        "the default with neither flag is project "
                        "evaluation",
                        usage};
                }
                if (inv.msbuild_allow_failure && !inv.msbuild_record
                    && !inv.msbuild_full_build)
                {
                    throw UsageError{
                        "--allow-failure tolerates a non-zero MSBuild exit "
                        "code; the default evaluation channel runs no "
                        "MSBuild build (combine it with --record or "
                        "--full-build)",
                        usage};
                }
                if (inv.msbuild_all
                    && (!inv.msbuild_config.empty()
                        || !inv.msbuild_platform.empty()))
                {
                    throw UsageError{
                        "--all builds every declared "
                        "Configuration|Platform; --config/--platform "
                        "select exactly one",
                        usage};
                }
                if (inv.msbuild_all && !inv.compile_db.empty())
                {
                    throw UsageError{
                        "--all writes one compile_commands.json per "
                        "configuration; --compile-commands cannot "
                        "name them all",
                        usage};
                }
            }
        }

        if (*cmd == Invocation::Cmd::Graph)
        {
            const bool symbol_mode = !inv.graph_symbol.empty();
            if (symbol_mode == inv.graph_modules)
            {
                throw UsageError{
                    "graph needs exactly one mode: --symbol <sym> or --modules",
                    usage};
            }
            if (!symbol_mode && depth_seen)
            {
                throw UsageError{
                    "--depth expands an entry symbol; it does not "
                    "apply to --modules",
                    usage};
            }
            if (symbol_mode && inv.min_weight != 1)
            {
                throw UsageError{
                    "--min-weight filters module aggregate edges; "
                    "it does not apply to --symbol",
                    usage};
            }
            if (!depth_seen)
            {
                inv.depth = 2;
            }
        }

        if ((*cmd == Invocation::Cmd::Impact
             || *cmd == Invocation::Cmd::Changes)
            && !depth_seen)
        {
            inv.depth = 3;
        }
        if (saw_no_dispatch && saw_dispatch_fallback)
        {
            throw UsageError{
                "--no-dispatch (pure static view) and --dispatch-fallback "
                "(precise + 0.0 markers) are mutually exclusive",
                usage};
        }
        return inv;
    }

} // namespace cg::cli
