module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.parse.usage
 *
 * @brief 使用说明域：顶层用法、按命令的用法文本、命令词与命令名互查。
 */
export module cg.cli.parse.usage;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.graph;
import cg.cli.parse.model;

export namespace cg::cli
{

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
} // namespace cg::cli
