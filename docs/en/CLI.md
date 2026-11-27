# cpp-code-graph v1 — CLI Reference (`ccg.exe`)

## 1. Synopsis and shared rules

```
ccg <command> [arguments] [options]
```

| shared option | meaning |
|---|---|
| `-o DIR` | output FOLDER. One subdirectory per configuration lives inside it, each holding its own `ccg.db` plus derivatives (BMI cache, profile). Default: `$CCG_DB` if set, else `./.ccg`. Passing a FILE path is a loud usage error |
| `--config NAME` | select (or, for `index`, create) the configuration `<folder>/<name>/`. Required for `ccg index` (never omittable). Query-side commands may omit it when the folder holds exactly ONE configuration — the single one auto-selects; with several, the command exits 1 listing the candidates. `$CCG_DB` may point at the folder OR directly at one configuration directory |
| `--json` | machine output (valid JSON on stdout) — accepted by every command that prints results, including `index` and `stats` |
| `-v, --verbose` | progress chatter to stderr (never stdout) |
| `--version` | print version, exit 0 |
| `--help`, `ccg help [cmd]` | usage, exit 0 |

Symbol-accepting commands (`query context callers callees impact trace
members`) also take disambiguators: `--file SUBSTR` (filter by defining
path substring) and `--kind K` (`record function method constructor
destructor field variable enum enum_constant type_alias`). The
family-aware commands (`context callers callees impact`) additionally take
`--no-merge` to disable override-family merging (§2).

Result shaping on the query line: `context callers callees impact` take
`--exclude-glob G` (repeatable; drop result entries whose far symbol is
defined in a matching path — rules below), and `callers callees impact`
take `--top N` (listing cap; a `... and N more` line marks the cut and the
reported counts keep covering everything found).

### Exit codes

| code | meaning |
|---|---|
| 0 | success — including `truncated` results (counts are still complete) |
| 1 | not found / no path / ambiguous target / runtime error / `partial` result / `changes`: nothing found |
| 2 | usage error (unknown command or option, missing operand, bad value) |
| 3 | `check --cycles`: cycles FOUND (distinct gate code; 0 = clean) |

Adopted honesty contract: `--json` payloads carry `error` (string) and
`partial`/`truncated` (bool) fields when applicable; `error` non-empty or
`partial:true` forces exit 1; `truncated:true` alone does not. Listing caps
(`--top N` on `callers`/`callees`/`impact`, and `check --cycles --max`)
never cap the reported counts; `truncated` reports `yes` only where a
listing cap actually cut the output (a live `--depth` frontier is a
separate flag — `deeperEdgesExist` — see the callers section).

### Docs and PATH

Usage and error texts reference `docs/*.md` files that live BESIDE the
binary — build trees copy them next to the executable, `cmake --install`
ships them in the flat release layout (INSTALL.md). To make a bare
`ccg` callable from any directory, run the shim installer beside the
binary: `ccg-install.cmd` on Windows, `ccg-install.sh` under bash
(INSTALL.md section 2).

### Symbol name matching (commands taking `<sym>`)

Matching runs in three tiers, evaluated left to right:

1. **exact** — the argument equals the symbol's unqualified name
   (`Circle`), or its full qualified name (`shape::Circle`,
   `render::Canvas::draw`), or its full unique key
   (`draw|shape|src/...|30` — power-user escape hatch, exact text).
   Case-sensitive.
2. **qualified suffix** — the argument matches the tail of the qualified
   name across `::` boundaries (`Canvas::draw` matches
   `render::Canvas::draw`; `shape::Circle` matches `a::shape::Circle`).
   Case-sensitive.
3. **substring** — ASCII case-INSENSITIVE substring of name or qualified
   name (`circ` → `Circle`, `CircleCollider`; a lowercase `task` finds
   both `Pretask` and `TaskRecord`).

All three tiers are always evaluated and their hits are deduplicated and
merged (tier-exclusivity is gone): `ccg query` lists every hit and reports
which tiers contributed (`3 matches (tier: exact)`,
`5 matches (tier: exact + substring)`). Symbol-target resolution
(`context callers callees impact trace members`) first looks at the EXACT
subset — hits whose name, qualified name, or unique key equals the
argument: exactly one → resolved directly (substring noise never turns a
unique exact hit into an ambiguity report); more than one → the command
prints the candidate list (one `kind  qualified_name  file:line` per line;
JSON gets a `candidates` array) and exits 1; none → every merged hit
participates in the ambiguity decision.

When tiers 1-3 all come up empty, tier-4 **fuzzy suggestions** run:
case-insensitive, separator-insensitive similarity (`_` and `::` stripped)
over a LIKE-narrowed candidate pool, ranked by edit distance on the
argument's last segment, best 5 kept (`no symbol matching 'drawx' (did you
mean: draw?)`). Tier-4 hits are suggestions only — they never resolve a
target on their own.

Overload note: v1 matches by NAME, not by parameter list (`draw(int)` is not
resolvable). `--kind` separates overloads only when their kinds differ
(method vs function); overloads of the SAME kind in the SAME file (e.g. two
free functions next to each other) cannot be told apart in v1 — `--file`
does not help there because both candidates match the same path substring.
Signature-aware matching is a documented LATER item (the schema already
carries `sig_hash`).

### Path filters (`--exclude-glob G`)

`context callers callees impact` drop result entries whose FAR symbol
(the other end of the edge, or the impacted entry itself) is defined in a
path matching `G`. The filter is result-side only — nothing is removed
from the database, and reported totals follow the filtered view. `G` may
be repeated; any match excludes. Syntax:

- `*` matches any run WITHIN one path segment (never crosses `/`);
- `?` matches exactly one non-`/` character;
- `**` crosses segment boundaries — both as a prefix (`build/**`) and in
  the MIDDLE of a pattern (`src/**/test.cpp` matches `src/a/b/test.cpp`
  and `src/test.cpp` alike); a bare `**` matches anything;
- matching is ASCII case-insensitive (paths are compared with forward
  slashes; the drive letter's case is not stable on Windows);
- a pattern starting with `/` anchors to the path start; otherwise an
  implicit `**/` prefix is prepended (`_deps/*` matches at any depth);
- there is no character-class support (`[...]` compares literally); an
  empty pattern is a usage error; paths matching nothing are simply never
  dropped.

## 2. Commands

### ccg index — build or update the database

```
ccg index [<paths>...] --config <name> [--compile-commands F]
                   [--llvm-root DIR] [--clang-compile-flags F] [-o DIR]
                   [--full] [--dry-run]
                   [--paranoid] [--force] [--jobs N]
                   [--wait[=<ms>]] [--no-wait] [-v]
```

- `<paths>` — one or more project ROOT directories (omit on a re-run:
  the recorded profile restores them). Only TUs whose main file lies under
  one of the roots are indexed (project allowlist for edge gating comes
  from the same roots).
- `--config <name>` — REQUIRED, never omittable. Names the configuration
  directory `<folder>/<name>/` that holds `ccg.db`, the BMI cache subtree
  and `profile.json`. The first successful run records a profile (roots,
  compile-commands path, engine identity, effective argv); a bare
  `ccg index --config <name>` re-run restores every input from it, and
  command-line values win over the profile and update it.
- **multi-config compile db splitting** — a
  compile_commands.json that carries MULTIPLE internal build
  configurations (the `.dir/<Config>/` signature of the recorded
  commands; multi-config generators such as Ninja Multi-Config record
  every configuration) is split at index time into one configuration
  directory per internal config, one db each — no exception. The split
  directories are named after the internal configurations (e.g.
  `.ccg/Debug/`, `.ccg/Release/`), the passed `--config` name applies to
  single-configuration databases, and each split db records its build
  signature in meta (`ccg update` filters the reloaded compile db by it,
  so sibling configurations never mix into one db). A configuration that
  left the compile db simply stops being refreshed — its directory stays
  on disk (stale quarantine) until the user removes it. The split
  discriminant is one FILE recurring under different signatures (single-
  config generators put source subpaths after `.dir/`, which must not be
  mistaken for configurations); a multi-config db whose configurations
  index disjoint sources therefore passes it — when the unsplit
  signatures still look like configuration names (Debug/Release/...), the
  run stays single-configuration and says so with an advisory note on
  stderr (index each configuration from its own compile db to split).
- `--compile-commands F` — path to a compile_commands.json. **A compile
  database is a prerequisite**: C++ has no meaningful simple fallback, so
  `ccg index` never guesses a corpus (a wrong graph is more dangerous than
  a missing one). If omitted, each root is searched for
  `compile_commands.json` (root itself, then `root/build`, then one level
  of subdirectories); still not found → exit 2 with an actionable error
  naming the three fixes (export it from the build — CMake:
  `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`; index a Visual Studio solution
  with `ccg msbuild`; or pass `--compile-commands <file>`).
- **module BMI self-build (compile-db route)** — module units
  (`.ixx/.cppm/...`) that the compile database routes away from verbatim
  replay are indexed through the self-build: ccg discovers the std module
  source for the engine's
  land (libc++ `std.cppm` via the driver's own `libc++.modules.json`,
  libstdc++ `bits/std.cc` via the driver or `g++`, the MSVC STL
  `modules/std.ixx` via the toolset the driver's include list
  auto-detects — floor 14.44; the clang-cl production vector for that
  land arrives with the M4 wave, until then it degrades with a named
  class), builds `std` + `std.compat` once per (engine, config,
  toolset), P1689-scans every unit with `clang-scan-deps` (beside the
  driver), builds the interface BMI DAG topologically with explicit
  `-fmodule-file=<dep>=<pcm>` mappings (partitions by their `mod:part`
  spelling) as REDUCED BMIs, and parses every TU feeding the transitive
  closure of its imports. The cache lives under
  `<folder>/<config>/bmi-self-<platform>/<engine_key>/` (the ccg
  configuration directory — NTFS
  compression requested at directory creation, best-effort and never a
  correctness dependency) as ONE `manifest.json` ledger at the engine
  level plus one `<config_key>/` SUBDIRECTORY per build configuration,
  each holding `std.pcm`, `std.compat.pcm` and the interface pcms under
  their PURE module names (no fingerprint suffixes — the configuration
  dimension is the subdirectory; the manifest records each pcm's
  provenance as `"<config_key>/<module>.pcm"` plus source content hash,
  production command digest, direct requires, and engine + toolset
  identity): later runs stat-check then hash-confirm each interface,
  reuse unchanged pcms, cascade rebuilds only downstream of edits, purge
  modules that left the DAG, retire a whole configuration by deleting
  its subdirectory when no db entry feeds it anymore, and wipe the
  entire engine directory when the engine version changes or a
  pre-rework flat-pcm layout is detected (`--force`/`--full` rebuild
  everything). A failed std
  or interface BMI build never stops the run: the affected TUs degrade
  to tier 1 with named failure classes (`std-bmi-build-failed`,
  `module-build-failed`, `module-dag-cycle`, `self-build-unavailable`)
  and still land their authored imports from the P1689 scan requires
  (imports rows at line 0 = scan-sourced). Known boundary (honest
  bookkeeping, a future wave): partition units themselves (`module
  m:part;` producers) are not yet self-build producers — the P1689 scan
  does report `provides m:part (partition)`, but the self-build DAG
  registers no provider node under the partition name, the partition TU
  parses as a classic unit (no `-x c++-module`, no module output), and
  host TUs importing `:part` report `module-not-found 'm:part'`
  (observed identical under the gnu dialect — a dialect-independent,
  pre-existing gap, not a regression of the module waves).

- **MSVC/MSBuild module land (module wave M4)** — compile databases in cl
  dialect (the `ccg msbuild` adapter's shape, `--driver-mode=cl` entries)
  route their module units through the self-build in the clang-cl dialect:
  `std.pcm` is produced from the toolset's own `modules/std.ixx`
  (`-clang:-o` BMI naming, `-imsvc DIR` SPACE-form include roots — the
  colon form is silently dropped and falls back to the newest toolset's
  headers), the P1689 scan runs over cl-spelled entries (the in-command
  `--driver-mode=cl` flag, with a `clang-cl.exe` shim fallback inside
  `.ccg/`), interfaces build cl-mode bottom-up, and consumers parse with
  the pinned `--target=x86_64-pc-windows-msvc` plus the 3 axes
  (`-Xclang -fexternc-nounwind`, `-fstack-protector-strong`,
  `-fms-compatibility`). The toolset is resolved from the RECORDED
  include dirs first, then the msbuild pipeline's own cl discovery (a
  `-p:VCToolsVersion` pin flows through), then the newest installed
  toolset — toolset + SDK identity is part of the pcm cache key, so a
  toolset change wipes and rebuilds. The language axis follows the
  recorded `/std:` values (floored at c++20; `/std:c++23` does not exist
  in cl mode and is never emitted). Mixed TUs (`import std` + textual STL
  includes) never see `std.pcm` — the import line is overlaid with the
  toolset umbrella header `__msvc_all_public_headers.hpp` (the
  redefinition clash is avoided by rule); header-unit imports
  (`import "hdr";`) degrade to textual includes, and a purview
  header-unit-only interface additionally degrades to a classic parse
  (module declaration blanked same-width). `CCG_MSVC_GNU_VECTOR=1`
  switches consumer parses to the documented gnu-mode 4-flag fallback
  vector over the SAME cl-built pcms. Classic cl entries keep their
  verbatim replay (msbuild wave behavior) and only gain feeds. The
  toolset floor is 14.44 — older toolsets degrade with the named
  `std-bmi-build-failed` class. Environment knobs:
  `$CCG_FALLBACK_FLAGS` — extra raw argv tokens (`;`-separated:
  `-isystem;<dir>;-DNAME=VALUE`) appended to the per-TU baseline vectors
  of units without recorded authority, self-build production and scan
  alike, for vendored dependency roots and build-shaped defines;
  `$CCG_STD_SOURCE` / `$CCG_STD_COMPAT_SOURCE` — explicit std module
  interface sources overriding land discovery; `$CCG_GXX` — explicit
  `g++` for the libstdc++ discovery channel.
- **module BMI authority over a compile db (db route)** — the unified
  routing rule: a module's BMI production command is its AUTHORITATIVE
  command — the compile db's recorded argv for every unit it records;
  the per-TU baseline flags only serve units without a recorded entry.
  Two routes realize it: the cl route above and the db route here. For a
  clang/gcc dialect compile db, entries whose recorded command — after
  `@modmap`
  response-file expansion — still carries no `-fmodule-file` feeds
  (multi-config generators such as Ninja Multi-Config record EVERY
  configuration's command into `compile_commands.json`, but only the
  built configuration's `.modmap` rsp files and BMIs exist on disk)
  replay verbatim into a silent `rc=4` hard-fail, so they route through
  the self-build instead: each module interface produces its pcm with
  its RECORDED argv plus the dependency-closure `-fmodule-file` feeds
  and the emit-interface additions (`-x c++-module`,
  `--precompile-reduced-bmi`, `-o`), the consumer parses with the same
  recorded argv plus its import-closure feeds, and the std module comes
  from the engine's own land (route-B discovery) built at the recorded
  `-std` axis — a deliberate fidelity trade-off documented as
  engine-consistency-first: the build's own `std.pcm` exists only for
  built configurations, and a pcm of a foreign flavor or dialect is
  never fed (an msvc-stl-land engine degrades std with the named class
  rather than mixing dialects). One self-build pass runs per build
  configuration (the `.dir/<Config>/` signature of the recorded modmap
  path): each configuration's recorded argv differs, so its pcms live in
  their own `<config_key>/` subdirectory — the manifest keeps one entry
  per (module, production-command digest), the digest covering the
  recorded argv AND compiler token, so an edited db command (a changed
  macro or flag) rebuilds exactly that configuration's module (same pure
  file name, overwritten) and cascades downstream while sibling
  configurations reuse; a configuration that left the db is retired
  wholesale (its subdirectory reclaimed). Entries whose feeds
  ARE present (the built configuration) stay on verbatim replay, and
  classic TUs without authored imports are never rerouted. A feed-less
  import-bearing TU that still reaches a parse (a routing residual)
  fails with the named `module-feeds-absent` class instead of a bare
  `error code 4`.
- `--llvm-root DIR` — explicit LLVM prefix holding the `clang++`/`clang`
  driver used for the per-TU baseline flags (DESIGN.md section 10.6) and
  implicit-include discovery — and,
  for this invocation only, also the parse ENGINE when the database has
  no `llvm-path` configured (a configured `llvm-path` always wins over
  the flag, and the lost flag is announced with a note on stderr).
  Default: the driver anchored on the loaded engine library (its
  directory, then its installation prefix), else `$CCG_LLVM_ROOT` if
  set, else the first one on `PATH`. If no driver can be located at
  all, `ccg index` fails with an error naming `CCG_LLVM_ROOT` /
  `--llvm-root` / `llvm-path` (never a guessed baked-in path).
- `--clang-compile-flags F` — extra clang flags appended to the TAIL of
  every replayed argv (quote-aware splitting, e.g.
  `--clang-compile-flags "-DFOO -isystem <vendor>/include"`; repeatable —
  later occurrences override earlier ones, driver last-flag-wins). The
  injection happens at the REPLAY layer only: compile_commands.json is
  never rewritten (regenerating the cdb via `ccg msbuild` keeps the
  configuration), recorded-argv TUs and synthesized-baseline TUs are both
  covered, and the appended flags fold into the per-entry argv digest —
  changing them re-parses the database on the next run. A per-database
  default can be set with `ccg config set clang-compile-flags "..."`;
  priority is command line (this run) > config key > none. Typical use:
  replay flags MSVC accepts but the recorded clang argv lacks (the
  `$CCG_FALLBACK_FLAGS` escape hatch was fallback-TU only and never
  reached recorded argv). `ccg update` and `ccg msbuild` take the same
  flag. Standard troubleshooting path for MSVC-vs-clang template
  divergence (failure classes `msvc-permissive-*`): set
  `ccg config set clang-compile-flags "-fdelayed-template-parsing"`,
  then re-run `ccg index --full` — necessary because clang's delayed
  parsing only approximates MSVC's permissive default, so residual
  shapes fail honestly and need the explicit flag. One residual family
  needs no flag at all: the lenient-specialization shape
  (`msvc-permissive-lenient-specialization`) is rewritten in memory
  from the compiler's own fixit on cl-dialect entries (see the remedy
  map below), so those TUs turn green automatically.
- **LLVM engine (per-db parse worldview)** — `ccg` does not link libclang;
  the engine (`libclang.dll` / `libclang.so`) is loaded at run time, one
  worldview per database. Resolution order: the database's `llvm-path`
  config (`ccg config set llvm-path`), then `--llvm-root`, then
  `$CCG_LLVM_ROOT`, then default discovery — the clang driver found on
  `PATH` names the toolchain, and its root provides the library (beside
  the driver and under `lib/`). The path and the version string the
  loaded engine reports are recorded in the db at every index run; when
  either differs from what is recorded — a different engine, an upgraded
  toolchain, or even the same path whose content changed — the run
  automatically switches to FULL re-indexing with a notice (no
  incremental shortcuts across worldviews). A missing or unloadable
  engine is an actionable error naming the knobs (exit 1), never a silent
  fallback to another engine.
- `--full` — re-index everything in place (also the implicit mode against a
  fresh db). Per-file replacement makes a `--full` run equivalent to
  rebuilding the db: every tracked file is rewrite-eligible, so the run also
  heals rows an earlier (buggy or crashed) run may have left behind.
- `--dry-run` — print the plan (mode, total/dirty TU counts, estimated
  re-parse work in main-file bytes — only the TUs that will actually
  re-parse, never skipped ones — purged-file estimate) without writing, and
  return in milliseconds: the run short-circuits BEFORE any engine load,
  driver resolution, module self-build scan or shadow-PCH planning (those
  cost tens of minutes on large corpora). Nothing is parsed — the plan is
  the per-entry freshness verdict against the existing db (stat mtime+size,
  sha256 confirm, compile-command digest); a no-op db stays a stat-only
  check, and an edited main still shows up as exactly one stale TU. Because
  nothing parses, the include-delta expansion and the purged-file estimate
  come from the RECORDED include graph (an include you abandon this edit
  surfaces in the next real run's sweep). A missing db is never created; an
  existing one is only read (a db that fails to open — schema mismatch —
  is planned against an in-memory stand-in, i.e. everything reads as new
  work).
- `--paranoid` — ignore the mtime fast path and hash every file (catches
  mtime-preserving copies; slower, O(bytes) instead of O(stat)).
- `--force` — legacy alias of `--full` that also recreates the db file when
  its `schema_version` does not match (§SCHEMA 4).
- `--jobs N` — parallel workers for BOTH the module-BMI build and the TU
  parse pool, 1–64 (one cap governs both stages; they never overlap, so
  peak concurrency is N). Default: the per-database `index-jobs` config
  key, else `auto` = `min(cores-2, 12)`; the flag wins for its run
  (`ccg update`/`ccg msbuild` take the same flag with the same
  precedence). In the BMI build the per-layer worker count is additionally
  capped by the layer width. Stale TUs are handed to the parse workers
  longest-first (by main-file size); each worker owns its own libclang
  index. Worker count does not change the result: the stored rows are
  identical for any N.
- `--wait[=<ms>]` / `--no-wait` — startup lease takeover
  (**REALTIME.md** §4). Right after opening the db for writing,
  the run STEALS the same `update_claim` the update flow uses (a live
  holder is displaced; it yields gracefully at its next heartbeat —
  no second writer survives). Stealing needs the write lock: while a
  concurrent batch holds it, one `waiting to take over the update claim
  (pid …, queue …)` line per retry (1/2/5/10 s capped backoff) is
  printed, capped at 30 min; `--wait=<ms>` tightens the cap. Inside
  the commit transaction the claim is heartbeated every 60 s; a claim
  stolen mid-commit rolls the whole stage back cleanly
  (`nothing written`). After the commit, events that hooks enqueued
  during indexing are converged IN PROCESS by bounded catch-up rounds
  (three-part output: detection note, the full untruncated file list,
  and a result line naming the rounds done and any events left; knobs
  `index-drain-rounds` / `index-drain-budget-ms`, defaults 8 /
  300000 ms); leftovers (round/time budget, project-file lines) are
  handed to the regular mechanism — one detached
  `ccg update --drain`. `--no-wait` keeps the fail-fast behavior
  (exit 1 instead of taking a live holder over, pointer to
  `ccg update --drain`). `ccg msbuild` shares both flags and the
  whole takeover/drain tail.

Behavior: against an EXISTING db, `ccg index` defaults to INCREMENTAL —
build-system freshness first (stat mtime+size; sha256 confirm on stat
change; per-entry compile-command digest, so a Release db over a Debug one
re-parses instead of silently skipping), then the dirty set expands through
the recorded include graph: any TU whose transitive include closure hits a
changed/deleted file re-parses, a directly-edited TU main rewrites its own
site rows (edges, includes, line numbers), TUs whose edges referenced purged
symbols re-resolve, failed TUs drop their observer contribution from
shared-header occurrence counters, and rows no current TU reaches (deleted
sources, dropped compile-db entries, abandoned headers) are purged.
Duplicate compile-db entries (same main file + same command) share one
parse. Incremental runs are row-equivalent to a from-scratch re-index of the
same tree state (SCHEMA §7; ctest-pinned). Failed TUs are recorded
(`parse_status=failed`, their previous rows purged) and never abort the run.
Opening a db whose `schema_version` does not match refuses with an error
advising `ccg index --force` (no cross-version migration is maintained
while the project is unreleased; §SCHEMA 4). With `-v`, per-TU progress lines carry
the worker id (`[w2 41/660]`) and may interleave.

Text summary to stdout (exit 0 even with failed TUs — the db is usable; the
first line states the mode):

```
$ ccg index . --compile-commands build/compile_commands.json
indexing 660 TUs under E:/src/widget (db: .ccg/main/ccg.db, mode: incremental)
  fresh: 0  stale: 12  skipped: 648  failed: 0
  symbols: 18411  edges: 50237 (calls 44891, extends 1523, overrides 1502, ...)
  includes: 15203  imports: 135
  unresolved edges dropped: 311
  elapsed: 41.3s   db size: 9.4 MB
```

`imports` counts the named-module import relations (schema v5): one row per
`import <name>;` authored in a module TU that parsed with its build's BMI
feeds (`@...modmap` response files and `CXXModules.json` BMI maps are both
consumed). When any TU fails, a `failure classes:` line summarizes the
NAMED per-TU classes instead of an opaque count —
`module-bmi-missing` (a fed BMI is not on disk; names the module + pcm),
`bmi-config-mismatch` (clang rejected a pcm silently; names the differing
axis of ccg's own recorded key — pcm producer version vs loaded engine, or
consumer vs interface-unit `-std`), `module-not-found`, `partition-not-fed`,
`two-stdlib-clash`, `stale-cascade-crash`, `module-feeds-absent` (the TU
authors an import but its recorded command carries no feeds at all — an
unbuilt configuration or a stripped/missing `.modmap`), plus the
self-build degrade classes (`std-bmi-build-failed`, `module-build-failed`,
`module-dag-cycle`, `self-build-unavailable`) — and `parse-failed` for
classic diagnostics. Module-TU parses run with an in-memory export-blanking overlay
(same-width `export` blanking that never touches `export module`/
`export import`/`export { ... }` blocks or literals, comments and
directives): libclang hides everything behind an `export namespace`/
per-declaration `export` wrapper in an unvisitable cursor, so without the
overlay those files yield zero symbols. The overlay is parse-only — the
on-disk file is never modified — and classic (non-module) TUs never see
it.

**Failure-class → remedy map.** When TUs fail, the class names below are
the triage entry points (the `--clang-compile-flags` mechanism — command
line or `ccg config set clang-compile-flags` — injects flags into every
replayed argv; MSBuild-route entries already carry the automatic
alignments from MSBUILD.md section 2 step 5):

| failure shape / class | remedy |
|---|---|
| varargs shape (`cannot pass object of non-trivial type ... through variadic function`; classic cl accepts it) | `--clang-compile-flags "-Wno-non-pod-varargs"` (proven effective on a large MSVC codebase) |
| source-encoding garbage (invalid-UTF-8 / mojibake diagnostics on GBK/Shift-JIS/... sources) | the MSBuild route is aligned automatically (`-finput-charset=<system ANSI codepage>` appended to entries that declare no encoding; BOM files unaffected); for a plain compile db, either convert the source to UTF-8 (BOM preferred) or inject `-finput-charset=<your codepage>` yourself |
| delayed-template-parsing residual (`msvc-permissive-unchecked-member-call`, `msvc-permissive-unchecked-ctor`) | the flag-injection triage flow above: `-fdelayed-template-parsing` via `--clang-compile-flags` |
| `msvc-permissive-unchecked-constexpr` (a `constexpr` bound to a dllimport/runtime value inside a template) | **no switch exists — fix the source** (clang checks class-template `static constexpr` members at the definition; delayed parsing cannot cover a declaration-level rule; the offending `constexpr` macro (e.g. `HEADER_WIDGET`) heals every use at once with a one-line constexpr→const change) |
| `msvc-permissive-lenient-specialization` (out-of-line class-template member definition missing the `template<>` introducer) | ccg rewrites it automatically (cl dialect only, diagnostic-driven via the compiler's own fixit, in-memory — the on-disk file is never touched; the summary line reports `dialect rewrites: N TU(s) fixed via compiler fixit`). Residual occurrences that still fail after the single reparse keep this class |
| lambda init-capture shape (MSVC binds the outer entity where clang binds the capture) | **no switch; tolerate + info** (error recovery is good — missing edges stay local with no pollution; init-capture shape, 2 occurrences repo-wide in the audited corpus) |

Debug environment switches (internal diagnostics; evaluated once per
process — set them before launching `ccg`. A run with any of them on is a
diagnostic run, not a performance benchmark. Windows note: environment
variable lookup there is case-insensitive, so case variants — e.g. an
all-lowercase spelling — take effect too; set just one spelling):

| switch | effect |
|---|---|
| `CCG_DEBUG_DIAGS=1` | prints every error-level diagnostic of every parse attempt to stderr — including the shadow-PCH attempt whose diagnostics an in-run text-fallback retry would otherwise replace (a TU that degrades with no DIAG lines is the NULL-TU/load-failure class, not a code error) |
| `CCG_DEBUG_ARGV=1` | prints the full replay argv of every parse attempt (the PCH-injected form and the fallback-stripped form each get their own block) |
| `CCG_PCH_DEBUG=1` | prints the full producer replay command of every shadow-PCH build |
| `CCG_CRASH_DUMP_DIR=<dir>` | Windows only. When set, `ccg` installs its crash callbacks at the very start of startup: if the process crashes, a full memory dump (all thread stacks, the heap, the handle table, and module information) is written into that directory as `ccg-<pid>-<yyyymmdd>-<hhmmss>.dmp`; a missing directory is created level by level, and a write failure reports the error code on stderr and terminates with the original crash exit code (exit codes are never swallowed; pure virtual calls and UCRT invalid-parameter have no system exception code of their own, so those two shapes exit with distinguishable application-defined codes 0xE0000002/0xE0000003). When unset, nothing is installed and behavior is exactly as before. Crash shapes that CAN be caught: access violations, stack overflow, heap corruption, uncaught C++ exceptions, `abort`, pure virtual calls (plus UCRT invalid-parameter on MSVC). Shapes that CANNOT (by Windows design they bypass every user-mode callback on the way to kernel termination): the fail-fast family — `__fastfail`-class instructions and stack-cookie (GS) check failures — for those the system-level WER LocalDumps is the only net (see DESIGN.md). Inert on POSIX (crashes go through the system core dump) |

`--dry-run` prints the plan instead:

```
$ ccg index . --dry-run
indexing 660 TUs under E:/src/widget (db: .ccg/main/ccg.db, mode: incremental, dry run)
  fresh: 0  stale: 12  skipped: 648  failed: 0
  plan: 12 of 660 TUs to re-parse (~318.2 KB main-file bytes), purged files: 1
  elapsed: 2.1s
```

`--json` emits the same fields as an object (`mode`, `dryRun`, and — for dry
runs — `dirtyBytes` instead of the graph totals).

### ccg update — realtime refresh of changed files (db-coordinated queue)

```
ccg update [<file>...] [--drain] [--compile-commands F] [--llvm-root DIR]
           [--clang-compile-flags F] [--jobs N]
           [-o DIR] [--config NAME] [-v] [--json]
```

A ONE-SHOT refresh command for editor-hook/agent-driven indexing — **no
daemon, ever, and no lock files**. It feeds the given files into the
ordinary incremental engine as externally seeded dirty files; the
resting database equals what `ccg index` would conclude for the same
tree state (the seed, not the stat, is the signal — an edit that
preserved mtime+size still re-parses, because the event said the file
changed).

- `<file>...` — changed source files (mains or headers; a seeded header
  propagates to every TU whose include closure reaches it). Files the
  database and the compile commands do not know are reported on stderr
  and otherwise ignored (a brand-new header nothing includes yet is
  invisible to the graph by construction).
- **Project files** (`.sln`/`.vcxproj`/`.vcxitems`/`.props`/`.targets`/
  `.filters`) are classified separately: they never take the TU parse
  path. When the db's project root has an msbuild registry and the line
  is the registered solution or lives under that root, the holder first
  consults the **hash gate** (R11): the refresh snapshot written by the
  last successful evaluate capture (`.ccg/msbuild/<stem>/snapshot.json`
  — the sha256 of the sln, every member vcxproj and every import
  closure, plus the INCLUDE hash, the MSBuild/cl paths, VCTargetsPath
  and the ccg version) is re-derived; ALL inputs unchanged ⇒ the event
  is absorbed without spawning anything. Otherwise the holder triggers
  a re-capture of THIS db's config (the default evaluate channel —
  debounced via `msbuild-refresh-debounce-ms`, default 3000;
  mutex-probed; failures keep the previous corpus and retain the line —
  see **REALTIME.md** §6 for the full protocol), and the capture's own
  incremental tail indexes whatever TUs actually changed. `--json`
  reports the outcome as `"projectRefresh": "captured" |
  "skipped-unchanged" | "busy" | "failed" | "untracked"`.
  Without a registry the line is dequeued as untracked (no pile-up; on
  non-Windows machines project events therefore never trigger anything).
- `--drain` — process the pending queue until empty even with no own
  files; the Stop-hook tail-convergence flag.
- `--compile-commands F` — override the compile database the db
  recorded, for this run only (the recorded file going missing is a
  runtime error: the refresh cannot see the corpus flags).
- `--clang-compile-flags F` — replay-layer flag injection for this run,
  same semantics as `ccg index` (repeatable, appended to every replayed
  argv tail; when absent, the database's `clang-compile-flags` config
  key applies).
- `--jobs N` — parallel workers for the module-BMI build and the parse
  pool, 1–64, same precedence as `ccg index`: the flag wins for this
  run, else the database's `index-jobs` config key, else auto.
- The database must already exist (`ccg index` first); roots, compile
  db, and the LLVM engine pin are read from the recorded index run's
  metadata, with the same precedence rules as `ccg index` (an engine
  identity change forces full coverage, worldview change). A database
  that records NO compile database is an error (exit 1) — update never
  scans for sources; re-run `ccg index` with a compile database.

Concurrency protocol (per database; **the database itself is the
coordinator** — schema v6 tables `pending_updates` + `update_claim`):

1. **ENQUEUE** — one short transaction inserts the own normalized paths
   into `pending_updates` (the `path` PRIMARY KEY deduplicates a
   concurrent enqueue of the same file). The connection's
   `busy_timeout` (`update-busy-timeout-ms`, default 600000) makes the
   enqueue wait out a concurrent index batch instead of failing.
2. **CLAIM** — one short `BEGIN IMMEDIATE` transaction on the
   `update_claim` singleton. No row, an EXPIRED lease, or a holder
   process proven dead (`update-lease-ms`, default 300000; the holder
   token embeds its pid): this process becomes the holder. A LIVE lease:
   exit 0 immediately — the holder's drain loop covers the queued work.
3. **HOLDER LOOP** — repeat { one short IMMEDIATE transaction: refresh
   the lease + **peek** the whole queue (a read — nothing is deleted);
   record each tracked path's content hash; process the batch through
   the ordinary incremental engine with no coordination transaction
   open; then **clear covered** — delete only entries that are untracked
   or that this batch covered with an unchanged content hash } until a
   peek is empty; release the claim (own row only) and exit 0. Edits
   that land while the batch runs keep their markers for the next batch
   (no lost updates). Each batch is ONE incremental run (summary shaped
   exactly like `ccg index`'s; `--json` likewise, and a lost claim
   reports `{"queued":N,"claimed":false}` instead).

Delivery is at-least-once and idempotent (re-seeding an
already-current file rewrites identical rows). A crashed holder leaves
NOTHING behind to clean: its lease expires and the next updater steals
the claim automatically. Work queued by a run that then died sits in
the queue until the next event or a `--drain` — the ordinary tail of
any queue without a daemon. `ccg index` joins the same protocol around
its commit stage only (queueing `--wait`/`--no-wait` flags, § *ccg
index*; **REALTIME.md** §4): while a batch runs, an index run
waits behind the holder instead of colliding, and while an index commit
stage holds the claim, editor hooks take the fast path exactly as they
do behind an update batch. Editor/agent hook wiring (PostToolUse
spawn + optional Stop-hook `--drain`) and the sample script:
**REALTIME.md**.

### ccg msbuild — index an MSBuild solution (Windows)

```
ccg msbuild <path.sln> [--config C] [--platform P]
            [--all] [--export-only] [--record] [--full-build]
            [--msbuild-args S] [--cl PATH]
            [--msbuild PATH] [-o DIR] [--compile-commands F]
            [--clang-compile-flags F] [--jobs N] [--force]
            [-v] [--json]
ccg msbuild --list [<path>]
```

One command from a Visual Studio solution to indexed databases.
The DEFAULT channel is EVALUATE: no build runs — ccg evaluates each
member project through its in-process C# object host (the VS-shipped
`Microsoft.Build.dll`), reads the final `ClCompile` items and metadata,
and composes the argv itself (seconds for typical solutions; no sandbox,
no stubs, no capture db; run from a VS developer prompt for the full
`-imsvc` set). `--record` runs the wrapped-build capture channel — the
TEST/VERIFICATION facility: the build graph executes, generators run
for real, compile tools are stubbed (no source is actually compiled,
stubs write nothing — a zero-disk record run; MSBuild-managed outputs go
to a sandbox, escapes into the project tree are reported as
`sandbox-leak` warnings, and generator products outside the sandbox
collapse into one `out-of-sandbox generator products: N (expected)`
info line). Both channels' invocations flow through the same
translation layer into a
clangd-usable `compile_commands.json`, and that feeds the ordinary index
pipeline — ONE DATABASE FILE per configuration at
one configuration directory per pair at
`<layout>/<Config>-<Platform>/` holding `ccg.db` plus its
`compile_commands.json` (layout = `-o DIR`, else `<root>/.ccg`), with a
tiny `.ccg/msbuild.json` registry at the project root describing the set
(the cross-configuration discovery entry point; the registry's `mode`
column reads `evaluate`/`record`/`full-build`). Query any of them with
`-o <folder> --config <stem>` (`ccg query add -o .ccg --config Debug-x64`).
A successful evaluate capture also writes the refresh snapshot
(`.ccg/msbuild/<stem>/snapshot.json`) that `ccg update`'s project-file
hash gate consumes (`skipped-unchanged` absorption).

- `--config`/`--platform` — pair to build (default: the registry default,
  else `Debug`/`x64`); values must be path-safe (they name the db file).
- `--all` — every Configuration|Platform the input declares, one db each
  (rejects `-o`, `--config`, `--platform`, `--compile-commands`).
- `--export-only` — stop after writing the compile_commands.json (prints
  its path; `--json` → `{"config","platform","mode","compile_commands",
  "tus","failed_projects","sandbox_root","sandbox_leaks"}` —
  `failed_projects` is evaluate-only, the sandbox fields are
  record-mode only).
- `--record` — the wrapped-build capture channel explicitly (see
  MSBUILD.md §2b): wrap-layer regression runs, the ground-truth
  comparison arm, and manual forensics of what MSBuild really invokes.
  Mutually exclusive with `--full-build`.
- `--full-build` — real compilation: every cl
  really runs, MSBuild's compile verdict surfaces (`MSBuild exited N`),
  no sandbox. Use it as the gold-standard cross-check (record-mode and
  full-build captures are byte-identical after translation).
- `--msbuild-args S` — extra MSBuild arguments spliced verbatim;
  `--cl PATH`/`--msbuild PATH` skip the corresponding discovery.
- `--clang-compile-flags F` — replay-layer flag injection, same
  semantics as `ccg index` (appended to every replayed argv; the
  generated compile_commands.json is not modified). A schema-mismatched
  db is RECREATED by default here (no manual `--force` needed after a
  ccg upgrade).
- `--jobs N` — parallel workers for the module-BMI build and the parse
  pool, 1–64, same precedence as `ccg index` (flag > the database's
  `index-jobs` config key > auto); per-database default via
  `ccg config set index-jobs`.
- `--list` — print the registry; the operand (default: the current
  directory) only locates the registry root. Missing registry → error,
  exit 1.
- Exit codes: 0 success (failed TUs allowed, like `index` — a failing
  build still indexes what was captured and surfaces `MSBuild exited N`);
  1 runtime (no MSBuild, empty capture); 2 usage.

Windows-only by design: parsing, translation, and the registry are
platform-neutral, but extraction needs MSBuild — elsewhere `--list` works
and everything else fails cleanly with `not supported on this platform`.
Full pipeline, registry format, failure modes, and caveats (tlog
rebuild-once, non-ASCII TU-path ingest limitation): **MSBUILD.md**.

### ccg config — per-database settings (engine + update coordination)

```
ccg config get [key] [-o DIR] [--config NAME] [--json]
ccg config set <key> <value> [-o DIR] [--config NAME]
ccg config unset <key> [-o DIR] [--config NAME]
```

One database is parsed by exactly one LLVM engine — its parse worldview.
The engine identity (resolved library path + version string) is recorded
alongside the results it produced and participates in index freshness
beside the compile-command digest and the TU-set delta.

- `get [key]` — print the configuration. Without a key, prints every key
  plus the engine lines, SEPARATELY: the db-RECORDED engine (the
  authoritative identity that produced the corpus — `recorded engine`) and
  the CURRENT environment resolution (the stored `llvm-path` — or the
  discovery chain when unset — the library a fresh resolution would load,
  and the version string of the engine once loaded). When the two differ
  (e.g. PATH discovery now finds 22.1.8 over the recorded 23.1.3) a loud
  warning says what a bare `ccg index` would do: load the new engine and
  force a FULL re-index. `--json` → `{"llvm-path","recordedEngine",
  "recordedVersion","engine","version","engineMismatch"}` plus the two
  update knobs and `index-jobs` below when getting everything; a broken
  engine is reported
  inline — reading the config never depends on a loadable engine, and a db
  never indexed shows `(not indexed yet)` for the recorded line. Missing
  db → exit 1.
- `set llvm-path <value>` — the LLVM installation to parse with: either
  an LLVM prefix (the library is looked up directly under it, under
  `bin/`, and under `lib/`) or the libclang library file itself. The path
  must exist (exit 1 otherwise). The value is stored normalized in the
  db's meta table. `unset`-equivalent spelling of the same engine is
  detected (resolving to the same library records no change).
- `unset llvm-path` (alias `clear`) — remove the setting; resolution
  falls back to `$CCG_LLVM_ROOT`, then PATH discovery.
- `set update-lease-ms N` / `set update-busy-timeout-ms N` — the `ccg
  update` coordination knobs (REALTIME.md): the claim lease
  lifetime without a heartbeat (must exceed the worst-case ONE batch;
  default 300000) and how long update coordination transactions wait
  out a concurrent write lock (default 600000; every ccg connection
  already arms a 30 s default busy timeout at open — SCHEMA.md section
  1 — so this knob only needs to cover minutes-long engine batches).
  Positive integers only
  (exit 1 otherwise); `unset` restores the default.
- `set index-jobs N` — the unified worker cap: parallel workers for the
  module-BMI build and the TU parse pool of `index`/`update`/`msbuild`
  (the two stages never overlap, so peak concurrency is N; the BMI
  build additionally caps each layer by its width). `0` = explicit back
  to auto, `1`–`64` effective — strict integer semantics (exit 1
  otherwise). Default 0 = auto by cores (`min(cores-2, 12)`); a
  `--jobs N` command line wins for its run. Changing it never
  re-parses anything: the worker count does not change the stored rows.
- `set msbuild-refresh-debounce-ms N` — the quiet window the update
  holder waits after seeing a queued project file before spawning the
  re-capture (default 3000; new queue lines during the window reset it).
  IDE save-storms on `.vcxproj`/`.props` collapse into one capture;
  see **REALTIME.md** §6.
- `set clang-compile-flags "F…"` — extra clang flags appended to every
  replayed argv by `index`/`update`/`msbuild` (space-separated,
  quote-aware). The generated compile_commands.json files are never
  modified; a `--clang-compile-flags` command line wins for its run.
  Setting/changing it re-parses the database on the next index run (the
  flags enter the per-entry argv digest).

**Advisory:** changing or clearing `llvm-path` prints a rebuild notice —
the setting is global for the whole database, and every result in it was
produced by the previous engine. Run `ccg index --full` (or just
`ccg index`: the engine-identity check force-fulls the next run by
itself).

Exit codes: 0 ok, 1 runtime error (nonexistent path, missing db for
`get`/`unset`), 2 usage (unknown action/key, missing operands).

### ccg hook — manage the Claude Code integration

```
ccg hook install --project|--user [--json]
ccg hook uninstall --project|--user [--force] [--json]
ccg hook status --project|--user [--json]
```

Registers or removes ccg's entries in a Claude Code `settings.json`:
a `PostToolUse` hook (matcher `Edit|Write|MultiEdit|Update`)
refreshes the
database through `ccg update` after every edit and answers with a
one-line blast-radius summary; a `SessionStart` hook answers with a
one-line database status; a `UserPromptSubmit` hook reports the
conclusions of edits that piled up while a refresh batch was running;
a `PreToolUse` hook (matcher `Grep|Bash|PowerShell|Search|Read|Glob`)
answers every string-search intent that names an indexed symbol with that
symbol's graph facts before the search runs — a Grep `pattern`, a
`rg`/`Select-String` command in Bash or PowerShell (a microsecond
trigger-word prescreen drops non-search commands before any parsing
cost), or a Search tool payload field. Read and Glob carry no search
intent at all: they are delivery-only carriers — the handler skips
intent extraction and the symbol query entirely and runs only the
backlog-delivery check described below. Every silent exit of the
PreToolUse handler (zero hits, an extraction gate declining, a deadline,
or any Read/Glob call) opportunistically delivers the hook digest
backlog through the same `hook_digest_flush(Opportunistic)` the next
PostToolUse edit uses — no journal directory, a contended lock, a live
claim lease, an unconverged queue, or an empty backlog all return at
zero cost, and no child process is ever spawned. This is what makes the
last batch of a batch-editing session reach the model even after it
switches to pure querying/reading with no further edits and no user
prompt; a hit that injects symbol facts does not piggyback (the
1400-byte injection envelope stays unmixed, the backlog waits for the
next silent exit).
All respond in the
`{hookSpecificOutput:{hookEventName, additionalContext}}` shape and are
silent (zero output, exit 0) whenever there is nothing to say — no
database in the working context, non-corpus file, non-edit tool, a
batch that is still running, or a search that names no indexed symbol.

**Grep-time injection (V7.1).** The PreToolUse hook strips the Grep
pattern down to a bare identifier (an `A::b` chain allowed; anchors,
`\b`, lookaround guards, and call-shape tails like `f\(` are tolerated)
and looks it up in the symbol table through the query-side fast path,
read-only. A pattern with top-level alternations (`pi|area`) is split at
depth-zero `|` and each branch goes through the same identifier gate and
noise-word filter (at most 8 branches; a `-F` literal pipe is never
split); multiple `rg -e`/`-Pattern` values are the same OR shape and
merge the same way. Branches that survive load their candidates together
(total hits capped at 5 — over the cap the hook answers with a one-line
count disclosure instead of a list), deduplicate by symbol, and render a
multi-term candidate list under the same 1400-byte budget with truncate
disclosure; a single surviving (or singly-hitting) branch degrades to
the exact single-chain answer below. A pattern with metacharacters but
no surviving branch can take a **restricted regex path** when the
`CCG_PRETOOLUSE_REGEX` switch allows it (on by default) and the shape
passes the guardrails (pattern length ≤ 64 and not pure decoration; the
PCRE2 engine bounds every single match by a step limit, so quantifier
forms such as `(a*)*` are admitted, a name that hits the limit counts
as a non-match, and the exits disclose how many such names there are;
deadline probes before and during every scan). The
path first scans the whole table with full-match semantics; when that
finds nothing and the pattern cannot match the empty string, it scans
again with partial-match semantics (a fragment inside the name) while
`CCG_PRETOOLUSE_REGEX_PARTIAL` allows it. Either layer branches on the
number of matching names: one name takes the single-chain answer,
two to five take the multi-term candidate list, and more
than five answer with a **graded envelope** — the hit total, the
five lexicographically smallest names that can be re-queried
verbatim, and one narrowing hint (the
over-limit exit of the multi-term path itself stays a one-line count
disclosure). The envelope's list only takes names that resolve again
when re-queried verbatim: operator names carrying regex
metacharacters and descriptive long names past the length limit take
no slot, and when there is no such name no name is listed. When some
names were left undecided by the step limit, every injection exit
discloses their count (right after the total in the envelope's first
line, as a trailing line elsewhere); zero hits in both layers stay
silent as before. Bash and PowerShell commands funnel their extracted
`rg`/`Select-String` pattern through this same gate: a multi-request
or wildcard-path command stays silent, and a `-F`/`-SimpleMatch`
literal only passes when it is itself a bare identifier chain (a
fixed multi-pattern command stays silent too). A
**unique** hit (bare
name with exactly one symbol, or a qualified chain that resolves to
one) injects five bounded lines: the identity line (qualified name,
parameter list, kind, parent chain), the definition coordinate, a
same-name disambiguation line (top 3 rivals with their coordinates —
"hits at those coords belong to other symbols"), the top-3 callers
with their highest-frequency call site (a zero-caller symbol gets
explicit "answer with no callers" wording instead of a defensive
guess), and the dispatch line (virtual-site count, resolved count,
implementation candidates by confidence tier). A **multi** hit (bare
name with several same-name symbols, an overload family) does not
guess a target: it injects a candidate list — the callable same-name
symbols ranked by references (top 5, `(+N more)` tail) with their
definitions, plus a hint to re-grep with a qualified name (example =
the head candidate) or a path filter. A Grep `path` input
pre-filters candidates by definition location and can collapse a list
into the five-line form. Both formats obey the 1400-byte budget with
their fixed degrade rules.

Runtime environment switch of the Grep-time injection (hook runtime,
not the index diagnostics table above; read once per process and cached
— the hook is a fresh process per tool event, so this is at most one
parse per event, and repeated checks on the hot path are pure memory
reads):

| switch | effect |
|---|---|
| `CCG_PRETOOLUSE_REGEX=1` | enables the restricted regex path of the PreToolUse injection: a metacharacter pattern with no surviving identifier branch (and not all-noise branches) is matched against the symbol-name table with PCRE2 full-match (interpreter tier, step-limited per name — a hit on the limit counts as a non-match and the exits disclose how many such names there are; quantified groups, lazy and possessive quantifiers, and the `(?i)` inline flag are accepted, while a pattern the engine cannot compile stays silent), one full-table scan per event, guarded by pattern length ≤ 64, name-length skip, and deadline probes before and during the scan |
| `CCG_PRETOOLUSE_REGEX=0` | disables the regex path (such patterns stay silent) |
| `CCG_PRETOOLUSE_REGEX_PARTIAL=1` | enables the second layer of the restricted regex path: when the full-match scan finds nothing and the pattern cannot match the empty string, the table is scanned once more with partial-match semantics (the pattern covers a fragment of a name, under the same step limit and deadline probes) and the hits take the same three exits |
| `CCG_PRETOOLUSE_REGEX_PARTIAL=0` | disables the second layer (the path stops after the full-match scan) |
| unset or any other value | either switch falls back to its built-in default (both currently enabled — the closed-book paired evaluation found the regex path harmless, so it ships on), documented here and in REALTIME.md |

The first switch gates only the regex path — alternation splitting and
multi-pattern lookup are always on. The second gates only the second
layer and has no effect while the first is off. Windows note:
environment variable lookup is case-insensitive there, so a
differently-cased variant (for example an all-lowercase spelling)
takes effect as well — set exactly one spelling and do not export
case variants.

**Fast path.** When an edit arrives while another `ccg update` holds a
live claim lease, the PostToolUse hook does not spawn a refresh and does
not query the impact surface: it enqueues the file into
`pending_updates` (primary-key deduplicated) and writes one journal
entry under `<ccg.db dir>/hook-digest/`, returning immediately (well
under a second). If SQLite's single writer is busy (the batch holds the
write lock), the enqueue falls back to a `need_drain` journal entry that
is merged into the queue once the batch converges — it never claims a
file was queued when it was not.

**Batch conclusion.** Once no live claim remains and
`pending_updates` is empty, the next `UserPromptSubmit` (or the next
PostToolUse edit, any silent exit of the PreToolUse handler — including
every Read and Glob call — or SessionStart as a crash fallback) claims the
journal entries, recomputes each file's impact surface against the
*refreshed* database (symbol, direct/transitive consumers, risk grade,
top consumers), and injects one merged `additionalContext` capped at
1200 bytes. Duplicate paths, symbols, and rendered summaries are
collapsed; injected entries are deleted and never replayed. A crash
between claim and send loses one round rather than ever double
reporting. Each delivery via a PreToolUse silent exit leaves a
`flushed-pretooluse-*` marker in the journal directory (aged out after
24 h); `ccg hook status --json` reports their count as
`digest.preToolUseFlushes`.

- `--project` targets `./.claude/settings.json` (run from the target
  repository's root); `--user` targets the home directory's
  `~/.claude/settings.json`. Exactly one is required.
- The edit is element-wise and span-preserving: user entries, comments,
  key order, and formatting outside ccg's own entries survive
  byte-for-byte; ccg's entries are recognized by their command (the
  `.ccg/hooks/ccg-posttooluse` / `ccg-sessionstart` /
  `ccg-userpromptsubmit` / `ccg-pretooluse` script names, or the
  `ccg hook posttooluse` / `ccg hook sessionstart` /
  `ccg hook userpromptsubmit` / `ccg hook pretooluse`
  spellings the plugin channel writes). A
  file that cannot be parsed is refused, never
  rewritten. Re-running `install` converges (own entries are replaced,
  not duplicated).
- **Upgrade path for an existing installation** — e.g. after the
  PreToolUse matcher grew from `Grep` to `Grep|Bash|PowerShell|Search`:
  re-run `ccg hook install` (own entries are recognized by their
  command, whatever matcher they carry, and rewritten to the current
  spec), then RESTART the Claude Code session — the host snapshots
  hooks at session start, so an entry changed mid-session does not
  take effect until relaunch (or a review in the `/hooks` menu).
  Both steps are required: without the re-install the settings keep
  the old matcher, and without the restart the freshly written
  matcher is not loaded.
- Commands reference trampoline scripts derived from THIS executable's
  location — the `.ccg` directory the build's POST_BUILD step (or a
  release package) places beside it; a missing script aborts
  installation rather than registering a dead path. Scripts locate ccg
  by `$CCG_BIN`, then the release layout beside themselves, then PATH.
- `uninstall` defaults to a dry run (`--force` applies) and removes
  exactly ccg's own command objects; emptied matcher entries, emptied
  event arrays, and an emptied `hooks` object are pruned with them (zero
  residue), neighbors untouched.
- The internal actions `ccg hook posttooluse` /
  `ccg hook sessionstart` / `ccg hook userpromptsubmit` /
  `ccg hook pretooluse` are the
  stdin-JSON entry points those scripts forward to (they take no scope
  flags; `-o DIR`/`$CCG_DB`/`--config` select the database like
  everywhere else). `ccg hook status --json` additionally reports a
  `digest: {dir, pending, claimed, needDrain}` summary for operators.

Exit codes: 0 ok (including "nothing to uninstall"), 1 runtime error
(missing script, unreadable/writable-but-refused settings), 2 usage
(missing/unknown action, missing or doubled scope).

### ccg query — find symbols

```
ccg query <name> [--kind K] [--file SUBSTR] [--implements] [--json]
```

`--implements` flips the lookup: `<name>` is treated as an interface or
base type and the command lists every type deriving from it along
extends edges (direct first, transitive included, a `depth` column
marking each level; `--json` mirrors it as an `interface`/`deriving`
payload). This is type-inheritance retrieval — virtual override
families are a different axis (see `impact`'s override-family merge).

```
$ ccg query draw
kind      qualified name                    location
method    render::Canvas::draw              src/render/canvas.hpp:42
method    render::Canvas::draw              src/render/canvas.hpp:58
function  shape::draw_all                   src/draw.cpp:12
3 matches (tier: exact)
```

`--json`:

```json
{"query":"draw","tier":"exact","matches":[
 {"uid":1041,"kind":"method","name":"draw","qualified_name":"render::Canvas::draw",
  "file":"src/render/canvas.hpp","line":42,"col":9,"parent":"render::Canvas","matchTier":"exact"},
 {"uid":1042,"kind":"method","name":"draw","qualified_name":"render::Canvas::draw",
  "file":"src/render/canvas.hpp","line":58,"col":9,"parent":"render::Canvas","matchTier":"exact"},
 {"uid":2210,"kind":"function","name":"draw_all","qualified_name":"shape::draw_all",
  "file":"src/draw.cpp","line":12,"col":6,"parent":null,"matchTier":"exact"}]}
```

Zero matches in all tiers → `no symbol matching 'drawx' (did you mean: draw?)`,
exit 1 (fuzzy suggestions). Ambiguous exact multi-hit → candidate listing,
exit 1 (§1). Tiers never exclude one another: an exact hit and a substring
hit both appear, with the tier line naming every contributing tier; in JSON
every entry carries its own `matchTier` (`exact` / `qualified suffix` /
`substring`) while the top-level `tier` names the shallowest contributing
tier.

### ccg context — 360-degree symbol view

```
ccg context <sym> [--kind K] [--file SUBSTR] [--exclude-glob G] [--no-merge] [--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] [--json]
```

`--exclude-glob G` (repeatable) drops edges whose far symbol is defined in
a matching path (§1 path-filter rules); the db is untouched.

Prints the symbol block (kind, qualified name, definition location, parent,
flags, template/param info), then incoming and outgoing edges grouped by
edge type, ONE LINE PER RECORDED EDGE ROW: a far symbol reached at several
sites appears once per site row (the raw `edges` rows — folding repeats
into one occurrences-counted line is a LATER item), and each line carries
that edge's first use-site coordinate (file:line).

```
$ ccg context shape::Circle
symbol  record shape::Circle
        src/shapes/circle.hpp:18  parent: shape::Shape (extends)
        template  no   methods 6   fields 3

incoming
  calls        shape::draw_all                src/draw.cpp:14
  calls        shape::area_all                src/draw.cpp:27
  used by      render::Canvas::add            src/render/canvas.hpp:21
  extends from shape::Ellipse                 src/shapes/ellipse.hpp:9

outgoing
  extends      shape::Shape                   src/shapes/shape.hpp:11
  calls        shape::pi                      src/shapes/circle.hpp:31
  type_alias   shape::CircleRef               src/shapes/circle.hpp:24
```

`uses` edges are the type-usage relations (below). Inbound rows print as
`used by`.

`--json` shape: `{"symbol":{...},"incoming":{"calls":[...],"extends":[...]},"outgoing":{...}}`
with each edge entry `{uid, kind, qualified_name, file, line, occurrences, confidence}`
— `file`/`line` are the edge's USE-SITE coordinates, the same source the
text line prints, not the far symbol's definition location (both come from
the site row; a missing site yields `"?"`) — plus `familyOrigin` on merged
rows, below, `dispatchStatus`, below, and a top-level `dispatch` summary
object in the default mode.

#### Override-family merging (context / callers / callees / impact / trace / changes)

Querying a virtual method merges its OVERRIDE FAMILY by default: the method
plus everything connected through `overrides` edges (the virtuals it
overrides and every override of it, transitively). Every merged row is
marked with its origin — `[family: ns::Class::method]` in text output,
`"familyOrigin": "ns::Class::method"` in JSON — and `--no-merge` queries
the symbol alone. Non-virtual or family-less symbols are unaffected.
Output with a merged family also lists the members (text: an
`override family merged` line; JSON: a `family` array).

#### Dynamic dispatch evidence (default on)

Schema v11 stores `virtual_dispatch` edges derived by static type analysis:
a virtual call through an interface pointer/reference is resolved to the
concrete override(s) reachable from the inferred dynamic type set. Queries
use this evidence **by default** (precise mode, B semantics):

- a call site with confidence >= 0.7 evidence (1.0 / 0.7) is attributed to the proven
  dispatch target instead of the family over-approximation. The static
  `calls` row is replaced, not duplicated. Concrete queries no longer pull
  in callers that dispatch to a sibling override; base/interface queries
  still include every override caller because the evidence target belongs
  to the queried symbol's implementation set;
- a call site without evidence keeps the static relation and, when it was
  reached only through a family member, is marked as a **family estimate**;
- positive dispatch edges are first-class call relations (not merely a
  filter), so a target that has no static `calls` edge in its family is
  still found;
- multiple inferred implementations appear as multiple rows at the same
  site — the tool never picks a single guessed target;
- an unresolved site emits ONE family-level 0.0 marker edge (target = the
  statically resolved callee); its capped candidate set lives in
  `call_sites.candidates` and truncation is marked in `fallback_reason`;
- indirect calls (function pointers / member pointers) emit no dispatch
  edge and are reported in `call_sites` with `fallback_reason=indirect_call`.

Markers and machine-readable status (the only user-visible vocabulary):

| evidence | text | JSON `dispatchStatus` |
|---|---|---|
| proven target, confidence 1.0 / 0.7 | `[dispatch 1.00]` / `[dispatch 0.70]` | `"inferred"` (plus `dispatchConfidence`) |
| no proven dynamic evidence (including a 0.4 weak inference only); whole override family estimate | `[dispatch unknown · family estimate]` | `"family_estimate"` |
| direct static relation, no dispatch uncertainty | no marker | `"none"` |

When any family estimate is present, the text header adds one
self-explanatory line:

```
family estimate: dynamic type unproven; estimated from the whole override family, may include non-target implementations
```

followed by the counts:

```
dispatch: N inferred, M family estimate, K static (--no-dispatch for the static view)
```

JSON carries the same counts as a top-level object:
`"dispatch":{"mode":"precise","threshold":0.7,"inferred":N,"familyEstimate":M,"static":K}`.
Only `inferred` entries carry `dispatchConfidence`; a family estimate
never exposes a numeric confidence (0.0 is a marker, not a measurement).

Confidence values are `1.0` (high: direct local evidence such as
`new Impl`), `0.7` (medium: single-type L1/L2 propagation), `0.4` (low:
multiple dynamic candidates, ambiguous resolution) and `0.0` (marker:
no dynamic evidence — never treated as evidence and never shown as a
measured confidence). The default evidence threshold is **0.7**: only
1.0 / 0.7 replace the family approximation. A 0.4 weak inference does
not name a dynamic target; the site stays a family estimate. 0.0 markers,
including the 0.4 sub-threshold rows, never add call relations.

#### Dispatch resolution vocabulary (`call_sites`)

The CLI surfaces only the ladder above; the finer-grained *why* lives in
the `call_sites` table (`fallback_reason`, `candidates[].reason` in the
candidates JSON) for programmatic audits over the db:

| token | meaning |
|---|---|
| `static_unique_override` | no dynamic evidence, but every receiver static type is graph-final or a concrete class AND the override family holds exactly one non-pure implementation — resolved 1.0 on static proof alone (candidate `reason`) |
| `static_narrowed` | `fallback_reason` suffix: candidates narrowed to family ∩ (the UNION of all static types' descendant subtrees), shown at 0.4 for display only — never a dispatch edge |
| `subtree_overflow` | the static-type subtree exceeded 256 nodes (or depth 16); the whole family is kept instead of a lossy narrowing, and the suffix says so |
| `receiver_scope_narrowed` | candidate `reason` suffix: under the full-family disclosure form of the COM root lifecycle methods (AddRef/Release/QueryInterface), candidates are narrowed to the receiver static type's extends descendant closure — members outside the closure are runtime-impossible targets for the site (guards against dilution when the dynamic type set is polluted by may fan-out / family write-back of fake types); when every candidate falls outside the closure the full set is restored |
| `..._nearest_converged` | several dynamic types whose nearest overrides all converge onto the same implementation — the site is upgraded to 0.7 while the `reason` keeps its base (`conditional_merge_nearest_converged`, `direct_new_nearest_converged`, ...) |
| `slot_type_truncated` | `fallback_reason` suffix: the receiver's type set was cut at the 32-type slot cap, so the dynamic evidence may be incomplete. Consumer note: when this marker stacks on a low-band multi-candidate (0.4) site, the candidate set is noise-heavy — the type set may have been flooded with near-whole-library types by may fan-out / family write-back unions; consumers should filter or down-rank via `receiver_scope_narrowed` / static reachability |
| `candidates_truncated` | the candidate listing was capped (64 entries) |

Slot-truncation observability is SQL-only — there is **no**
`analysis_truncated_slots` meta key (the analysis observability meta keys
are `analysis_rounds` / `analysis_work_items` / `analysis_budget_exhausted`
plus the `analysis_prop_*` timing/count series). Count affected sites with:

```
SELECT COUNT(*) FROM call_sites
 WHERE fallback_reason LIKE '%slot_type_truncated%';
```

Family-edge counting uses a two-number scheme (meta keys):
`analysis_prop_family_pf_edges` / `analysis_prop_family_wb_edges` count
**channel pairs opened at the pre-resolution stage** — pairs that deliver
nothing after the static-face clip are included; the number of slots that
actually received a family delivery is `analysis_prop_family_pf_slots` /
`analysis_prop_family_wb_slots` (finalized-stage count). The two can
differ by an order of magnitude (channel open, evidence absent); use the
slot counts when estimating the real delivery scale.

C-style cast note (erratum): an earlier field report claimed SWIG-generated
code uniformly writes C-style casts. The audited snapshot actually writes
`reinterpret_cast`; the CStyleCast handling (keep the operand's evidence,
never adopt the cast target) is defensive coverage, not a fix for an
observed form.

#### Dispatch CLI switches (context / callers / callees / impact / trace / changes)

- default — precise mode as described above.
- `--no-dispatch` — pure static view. This is the conservative upper bound
  (whole override families) and is byte-identical to the pre-dispatch
  default output; use it when you need to prove you did not miss a caller.
- `--dispatch-fallback` — precise mode plus sub-threshold markers (0.4 weak
  inference and 0.0 family markers), displayed as `family_estimate`. For
  `callers` / `callees` / `impact` it only annotates rows that a static
  relation already reached; `context` additionally lists sub-threshold
  edges. In every command the membership set is unchanged — markers never
  add or remove symbols. Mutually exclusive with `--no-dispatch`.

`trace` follows positive dispatch edges as first-class call hops (0.0
markers never participate in path finding). `changes` adds positive
dispatch callers as consumers (status `inferred`); override-family
propagation remains a family estimate. Residual over-approximation:
`changes` still propagates through override edges in both directions, so a
sibling's family-estimate consumers can appear when changing a concrete
implementation.

### ccg members — record member listing (own + inherited)

```
ccg members <record> [--kind K] [--file SUBSTR] [--config NAME] [-o DIR] [--json]
```

Lists the members of a record — methods, constructors,
destructors, fields, nested types — with `file:line`, in declaration
order (no need to re-read the header to answer "what does this struct
have?"). Direct members come first (`own`); members inherited along the
extends chain follow under `inherited from <base>` group headers (BFS
order, cycle-guarded, depth cap 8; `--json` carries `origin` per member).
An override appears once in `own` and once under its base — different
symbols, shown as-is.

```
$ ccg members shape::Circle
record   shape::Circle  src/shapes/circle.hpp:18
         6 members (3 methods, 2 fields)
kind         qualified name                   location
method       shape::Circle::area              src/shapes/circle.hpp:30
...
```

Count line: `N members (M methods, F fields)` where N counts EVERY direct
member — nested records, enums, enum constants and type aliases are
included in N but in neither M nor F, so `N = M + F` does NOT hold when
the record declares nested types. `--kind`/`--file` narrow the listing
(and the counts) like every symbol command.

Exit codes: 0 with an EMPTY listing for a record without members (that is
a valid answer); 1 when the argument does not resolve or resolves to a
non-record (the error names the actual kind and points at `--kind
record`).

`--json`: `{"record":{...},"total":6,"methods":3,"fields":2,"members":[
{uid,kind,name,qualified_name,file,line,col,parent},...]}`.

### ccg callers / ccg callees — call relations (transitive with --depth)

```
ccg callers <sym> [--depth N] [--top N] [--kind K] [--file SUBSTR] [--exclude-glob G] [--no-merge] [--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] [--json]
ccg callees <sym> [--depth N] [--top N] ...same options...
```

`--depth N` (default 1, max 10): 1 = direct callers; 2 = callers of callers;
each entry is annotated with its BFS depth. Output is a tree by depth:

`--top N`: list at most N entries (default unlimited). A
`... and N more (raise --top to list them)` line marks the cut; the
`N callers total` line and every other count keep covering all entries
found, and `truncated` reports `yes` exactly at such a cut.

`--exclude-glob G` (repeatable): drop entries whose defining path matches
`G` (§1 path-filter rules — `build/**`, `_deps/*`, mid-pattern `**` all
work); totals follow the filtered view.

Coordinates: the `file:line` printed for each entry is the FIRST CALL SITE
on the edge between the entry and its expansion source (the target itself
at depth 1, the `via` symbol deeper in) — the same call-site coordinate
vocabulary `context` uses, NOT the entry's definition line. When that edge
has no recorded site the entry's definition location is printed as an
honest fallback; `xN` marks N occurrences of the site.

```
$ ccg callers shape::Circle::area --depth 2
shape::Circle::area  (method, src/shapes/circle.hpp:30)
  depth 1
    shape::Circle::draw           src/shapes/circle.hpp:33
    shape::area_all               src/draw.cpp:27
  depth 2
    app::main                    src/app/main.cpp:18   (via shape::area_all)
    shape::render_all            src/draw.cpp:40       (via shape::area_all)
5 callers total (truncated: no)
```

Virtual methods merge their override family by default (rows marked with
`[family: ...]`; see context). `--no-merge` restores the single-symbol
view. Positive dispatch evidence replaces the family over-approximation at
that call site; rows with no evidence are marked as family estimates. Use
`--no-dispatch` for the static upper bound.

Depth > 1 can revisit a symbol via several paths; each symbol is listed once
at its SHORTEST depth, with one `via` origin. Cycles terminate the walk
(visited set). `truncated` is true only when the listing was actually cut
by `--top` (depth expansion never sets it); a live `--depth` frontier is
reported separately as `deeper edges exist: yes
(listed entries are complete; raise --depth to expand)` (text) or
`deeperEdgesExist` (JSON), and neither flag affects the exit code. Direct
callers of nothing → `no callers found — dead-code candidate unless one of
three invisible paths applies: indirect calls through function/member
pointers (never indexed); virtual dispatch to a sibling override (target is
virtual: yes/no, use --no-dispatch for the static upper bound); address
taken at N site(s) below` (+ the address-taken evidence lines when any),
exit 1. `--json` mirrors this as a `no_callers_reason` object with the
three blind-spot booleans (`pointer_calls_invisible`,
`sibling_dispatch_possible`, `member_address_taken`).

`callees` with an empty result deliberately keeps the plain error form
(`ccg: error: no callees found`, exit 1) instead of the dead-code note —
a zero on the callees side usually means the query picked the wrong
symbol or the body lives in an unindexed file, which is a query problem
rather than an answer; `callers`-side zero stays a normal (exit-1)
answer because dead code is a finding.

Address-taken annotation (`callers` / `callees` / `impact`): when the
queried symbol (or a member of its merged override family) has had its
address taken — `&Class::method`, `&free_fn` — the output ends with one
evidence line:

```
address-taken: &ns::sym appears at src/wire.cpp:57 (+2 more sites); calls through function or member pointers are not visible
```

This explains WHY such uses never show up as callers: the evidence row
only proves the address was taken somewhere; it is never turned into a
call edge. JSON carries the sites as `"addressTaken":[{"file":...,
"line":...},...]` (omitted when empty).

### ccg impact — blast radius

```
ccg impact <sym> [--direction up|down] [--depth N] [--top N] [--exclude-glob G] [--no-merge] [--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] [--json]
```

- `--direction up` (default): what depends on this symbol — transitive
  callers, types that extend it, and symbols whose declared types reference
  it (`uses` edges). `down`: what this symbol depends on — transitive
  callees, bases, and used records (a record's down-walk also includes the
  records its FIELDS use: the layout dependency).
- `--depth N` default 3, max 10.
- `--top N`: at most N entries PER DEPTH BUCKET (default unlimited);
  `... and N more` lines mark each cut while `impacted` / `direct` /
  `transitive` keep counting everything found; `truncated` is `yes`
  exactly when a bucket was cut.
- `--exclude-glob G` (repeatable): drop impacted entries whose defining
  path matches `G` (§1 path-filter rules); impacted/impacted sub-counts
  follow the filtered view.
- `--no-merge`: query the symbol alone; virtual methods otherwise report
  the whole override family's blast radius (rows marked, see context).
- The risk score is computed from the precise dispatch result; family
  estimates never raise the band. `--no-dispatch` computes the static
  upper-bound score instead.
- Coordinates: each listed entry prints the first call site of its
  in-chain one-hop edge (the same call-site vocabulary as
  `callers`/`callees`/`context`), falling back to the entry's definition
  line when no site is recorded. The address-taken annotation (above)
  applies here too.

```
$ ccg impact shape::Circle::area --direction up --depth 3 --top 2
target   method shape::Circle::area  src/shapes/circle.hpp:30
risk     HIGH
score    47 (3*direct 2 + transitive 35 + fan-in 6)
impacted 37 (direct: 2, at depth 2: 9, at depth 3: 26)
  depth 1
    method   shape::Circle::draw      src/shapes/circle.hpp:33
    function shape::area_all          src/draw.cpp:27
  depth 2
    function shape::render_all        src/draw.cpp:40
    function app::main                src/app/main.cpp:18
    ... and 7 more at depth 2 (raise --top to list them)
  depth 3
    ... and 26 more at depth 3 (raise --top to list them)
truncated: yes
```

#### USES edges (type usage)

A `uses` edge is emitted for every project record referenced by a declared
type: a FIELD's type, a function's PARAMETER types and RETURN type, and the
LOCAL variables and namespace-scope VARIABLES in its body — after
canonicalization. Canonicalization strips cv/ref/pointer/array wrappers
(`const Widget&`, `Widget*`, `Widget[4]` all reference `Widget`), resolves
typedefs to their underlying record, folds template specializations onto
the PRIMARY template's node (`Box<int>` references `Box`), recurses into
template arguments (`std::vector<Widget>` references `Widget`), follows
member pointers to their class type (`int Widget::*` references `Widget`),
and walks function types through result + parameters
(`std::function<Gadget()>` references `Gadget`; a callback parameter
`std::function<void(const Widget&)>` references `Widget`).
Records from outside the indexed roots — the standard library, system
headers — never create USES edges (their own references are dropped
silently; only project records inside their arguments surface). An
incomplete project type (forward-declared, defined in another TU) resolves
deferred by name. When several declarations reference the same record on
one line (a parameter and the return type), the site folds into one edge
with `occurrences` counting the repeats; `edges.attrs` bits 0-2 record the
site flavor (field / param / return / local / variable).

This is what makes RECORD-level impact real: `ccg impact <record>` lists
the USES-referencing fields and functions as direct impact, where v1
records only traversed `extends`.

#### Risk model

```
score = 3 * direct + transitive + fan_in
  direct     = depth-1 impacted count
  transitive = impacted - direct          (depth 2+)
  fan_in     = distinct inbound-edge sources (calls / extends / uses)
               summed over the directly impacted symbols
```

A direct consumer weighs three times a far transitive one; fan-in captures
blast amplification through well-connected consumers. Bands:

| risk | rule |
|---|---|
| UNKNOWN | direction `up` and zero resolved direct impact — absence of edges is not evidence of safety |
| LOW | score ≤ 8 |
| MEDIUM | 9 – 30 |
| HIGH | 31 – 120 |
| CRITICAL | score > 120 |

The banding is deterministic: the same db state always yields the same
score and band for a symbol.

`--json`: `{"target":{...},"direction":"up","depth":3,"impactedCount":37,"direct":2,"transitive":35,"fanIn":6,"riskScore":47,"byDepthCounts":[2,9,26],"byDepth":[{"depth":1,"symbols":[...]},...],"risk":"HIGH","truncated":false,"deeperEdgesExist":true}`
(plus `"family":[...]` when the override family was merged).

### ccg trace — shortest call path between two symbols

```
ccg trace <a> <b> [--kind K] [--file SUBSTR] [--no-dispatch|--dispatch-fallback] [--json]
```

BFS over `calls` edges plus positive `virtual_dispatch` edges (direction
a→b). Proven dispatch hops are labelled `--dispatch-->`; sub-threshold (0.4/0.0) markers
never participate in path finding. `--no-dispatch` follows static calls
only (the pre-dispatch behavior). Prints the hop chain or reports
unreachable.

```
$ ccg trace app::main shape::pi
found: app::main -> shape::pi  (3 hops)
  app::main            src/app/main.cpp:18
    --calls--> shape::render_all      src/draw.cpp:40
    --calls--> shape::Circle::draw    src/shapes/circle.hpp:33
    --calls--> shape::pi              src/shapes/math.hpp:7
```

Unreachable: `no path from app::main to shape::pi within 10 hops`, exit 1.
Either endpoint unresolved → error + exit 1. The disambiguators apply to
BOTH endpoints: a `--kind`/`--file` that does not match an endpoint's own
kind/defining file makes that endpoint unresolved (`no symbol matching ...`),
so when the two endpoints live in different kinds or files, disambiguate
with longer qualified names instead. `--json`:
`{"from":{...},"to":{...},"found":true,"length":3,"hops":[{...}],"truncated":false}`.

### ccg changes — affected consumers of the files you changed

```
ccg changes [<file>...] [--from-file LIST] [--depth N] [--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] [--json]
```

ccg is VCS-agnostic: it NEVER invokes git or any other version-control
tool. You find the changed files (git diff, IDE, whatever) and FEED the
list; ccg maps it onto the graph.

- `<file>` — changed file paths, absolute or relative to the working
  directory. `-` as an operand reads the list from stdin.
- `--from-file LIST` — one path per line from LIST (`-` = stdin); blank
  lines are skipped, a fully-quoted line (`"a b.hpp"`) loses its quotes,
  and a trailing `\r` (CRLF pipes) is stripped. Paths are the UNION with
  `<file>`, deduplicated after normalization.
- stdin — with no `<file>` and no `--from-file`, and stdin piped
  (`git diff --name-only | ccg changes`), the list is read from stdin. An
  interactive invocation (real console on stdin) is a usage error instead
  of a hang. Note the NUL device counts as a console under `_isatty`; ccg
  probes with `GetConsoleMode`, so `ccg changes < NUL` reads an empty list.
- `--depth N` — upstream propagation depth 1–10, default 3.
- `--no-dispatch` — static propagation only (the conservative upper bound).
  `--dispatch-fallback` is accepted for symmetry; 0.0 markers never add
  consumers.

**Path handling.** Each fed path is normalized exactly like the indexer
(absolute against the working directory, lexically normal, backslashes →
forward slashes, drive letter lowercased), then resolved to a `files` row:
exact text, then case-insensitive full path (deleted files keep their db
spelling — there is no on-disk case to canonicalize from), then a UNIQUE
case-insensitive path-suffix match (lists fed relative to another working
directory). Ambiguous or unmatched paths are reported in the output as
`unknown` / `"status":"unknown"` — never silently dropped.

**Mapping and propagation.** Each resolved file maps to EVERY symbol
defined in it (v1 is file-granularity, conservative-correct; a symbol
belongs to its DEFINITION file, so out-of-line methods count under their
`.cpp`). Deleted files keep their rows and symbols in the db until the
next `ccg index` run purges them — run `changes` against the pre-reindex
db to catch a deletion. Consumers propagate upstream over `calls`
(callers), `extends` (deriving types), `uses` (fields, parameters,
returns, locals, variables whose declared types reference a changed
record), and `overrides` in BOTH directions — the override family of a
changed virtual method are direct consumers. Positive `virtual_dispatch`
callers are first-class consumers (`dispatchStatus: "inferred"`); the 0.0
family markers never add consumers. Override-family propagation remains a
family estimate (`dispatchStatus: "family_estimate"`), so changing a
concrete override can still list a sibling's family-estimate consumers.
Consumers are listed at their SHORTEST depth over all seeds; symbols
defined in the changed files are the change itself and never appear as
consumers.

**Risk.** Per consumer: `score = (depth 1 ? 3 : 1) + fan_in` where fan_in
is the consumer's distinct inbound sources over calls/extends/uses; bands
use the same ladder as impact (LOW ≤ 8 < MEDIUM ≤ 30 < HIGH ≤ 120 <
CRITICAL). The summary risk uses impact's formula
`3*direct + transitive + fan_in` (fan-in summed over direct consumers);
UNKNOWN when there are no direct consumers.

**Exit codes.** 0 = consumers found; 1 = nothing found (no known files OR
no consumers — the payload is still printed in both text and JSON so the
unknown-path table is visible); 2 = usage error.

```
$ ccg changes src/shapes/circle.hpp src/gone.cpp
changes  2 files fed (known 1, deleted 1, unknown 0)  depth 3
  src/shapes/circle.hpp                      present  3 symbols
  src/gone.cpp                               deleted  2 symbols
consumers 6 (direct 4, transitive 2)
risk     MEDIUM
score    16 (3*direct 4 + transitive 2 + fan-in 2)
  depth 1
    field       render::Canvas::circles_     src/render/canvas.hpp:20  risk LOW
    method      render::Canvas::total_area   src/render/canvas.cpp:13  risk LOW
    ...
  depth 2
    function   app::area_all                 src/app/main.cpp:9   (via render::Canvas::total_area)  risk LOW
truncated: no
deeper edges exist: yes (listed entries are complete; raise --depth to expand)
```

`truncated` stays false for these walks (no listing cap — everything found
is listed); `deeper edges exist: yes` / JSON `deeperEdgesExist:true` mean the
walk was still expanding when `--depth` ran out (the next layer may or may
not add NEW consumers). `--json`:
`{"depth":3,"files":[{"input","path","status","symbols"}],"seeds":N,"consumers":[{"uid","kind","qualified_name","file","line","depth","viaSeed","fanIn","risk"}],"direct","transitive","fanIn","riskScore","risk","truncated","deeperEdgesExist"}`
with `path:null` and `"status":"unknown"` for unmatched inputs.

### ccg check --cycles — include-graph cycle detection

```
ccg check --cycles [--max N] [--config NAME] [-o DIR] [--json]
```

Builds the include graph from the recorded `includes` table and reports
every cycle cluster: a strongly-connected component larger than one file,
or a single file that includes itself. Each cluster lists its member
files and its internal include edges, so a two-file ring prints both
directions and a self-including header prints its self edge. Diamonds
(shared bottom, no back edge) are NOT cycles.

Target resolution is the STRICT unambiguous subset of the incremental
engine's rule set (§index — the engine fans a multi-match include out to
every candidate, which is right for freshness and wrong for a gate): a
quoted relative target resolves against the includer's directory first
(the current directory wins for quoted includes, exactly as compilers
search it), else the FULL include spelling as a case-insensitive path
suffix — accepted only when exactly ONE indexed file carries it. An
include whose spelling matches several files (say `logger.h` folding onto
unrelated `*/Logger.h` files) or none contributes NO cycle edge — a
fabricated edge would surface as a phantom cycle involving files no
source edit can fix — and is counted instead: `unresolvedIncludes` for
no match, `ambiguousIncludes` for multi-match (text output prints an
`ambiguous includes N` line when N > 0).

- `--max N` — cycle groups LISTED, 1–1000, default 20. The `*Total`
  counters always cover every group found; only the listing is capped.
- Exit codes: **0 = no cycles**, **3 = cycles found** (usable directly as
  a CI gate), 1 = runtime error (missing db), 2 = usage error (`ccg check`
  without `--cycles` — the only mode today).

```
$ ccg check --cycles
include cycles   1 group (2 files, 2 edges)
cycle 1 (2 files)
  e:/src/lib/a.hpp -> e:/src/lib/b.hpp
  e:/src/lib/b.hpp -> e:/src/lib/a.hpp
truncated: no (1 of 1 groups shown)

$ ccg check --cycles
include cycles   none (48 files scanned)
```

`--json`: `{"cycles":true,"groupsTotal":1,"filesInCycles":2,"edgesInCycles":2,"filesScanned":48,"unresolvedIncludes":0,"ambiguousIncludes":0,"truncated":false,"groups":[{"files":["..."],"edges":[{"from":"...","to":"..."}]}]`}
(`unresolvedIncludes` counts include targets strictly matching no file row;
`ambiguousIncludes` counts targets matching several).

### ccg graph — call-graph export (dot / JSON)

```
ccg graph (--symbol <sym> | --modules) [--depth N] [--min-weight N]
          [--kind K] [--file SUBSTR] [--format dot|json] [--output FILE]
          [--config NAME] [-o DIR] [--json]
```

Exports the call graph for humans (Graphviz dot, the default) and agents
(JSON). Two mutually exclusive modes:

- `--symbol <sym>` — call-graph expansion from an entry symbol over `calls`
  edges (what it calls), BFS depth-capped by `--depth N` (default 2, 1-10;
  `--depth 0` = unlimited, BFS until the frontier is empty; 2 is the
  readable sweet spot, 3 already explodes at repo scale). The closure keeps
  every `calls` edge whose both ends are inside it, so cross edges survive
  — the export is the real expansion, not a spanning tree.
  `--kind`/`--file` narrow the entry resolution like every symbol command.
- `--modules` — module-level aggregate. A module is the defining file's
  stem (`cli.parse.ixx` → `cli.parse`); each distinct symbol-level `calls`
  pair counts one edge weight; same-file calls never appear. `--min-weight
  N` (default 1 = keep everything; 3 reads well at repo scale) drops weak
  aggregate edges — nodes shrink to the kept edges' endpoints, but
  `edgesTotal` in JSON always reports the unfiltered count. `--file SUBSTR`
  scopes which files participate (e.g. `--file src/` keeps the project's
  own modules and leaves the dependency cache out).

```
$ ccg graph --symbol cg::cli::dispatch --depth 2
digraph ccg_call_graph {
  rankdir=LR;
  graph [label="call closure of cg::cli::dispatch at depth 2 (source: calls edges)"];
  node [shape=box, style="rounded,filled", fillcolor="#e8f0fe"];
  edge [color="#5f6368"];
  "cg::cli::dispatch" [fillcolor="#ffd166"];
  "cg::cli::top_usage";
  "cg::cli::usage_for";
  "cg::cli::command_of";
  "cg::cli::detail::run_query";
  ...
  "cg::cli::dispatch" -> "cg::cli::detail::run_query";
  ...
}
```

- `--format F`: `dot` (default) or `json`; the shared `--json` flag
  implies json (query-family parity).
- `--output FILE` writes the payload to a file instead of stdout; stdout
  stays clean and a one-line note goes to stderr. Node and edge labels are
  dot-escaped (quotes/backslashes; UTF-8 passes through untouched, so
  non-ASCII identifiers are safe).

`--json` (`--symbol`): `{"target":{...},"depth":2,"nodeCount":109,"edgeCount":225,"nodes":[{"uid":..,"kind":"function","qualified_name":"...","file":"...","line":12,"depth":1},...],"edges":[{"from":"...","to":"...","fromUid":7,"toUid":9,"occurrences":2},...],"truncated":false,"deeperEdgesExist":true}`.
`--json` (`--modules`): `{"moduleCount":29,"edgeCount":49,"edgesTotal":108,"minWeight":3,"modules":["cli.commands",...],"edges":[{"from":"cli.commands","to":"cli.render","weight":22},...]}`.

Exit codes follow the family with one export-specific note: 0 success —
an entry with no callees still exports a single-node graph (an export is a
faithful snapshot, not a lookup); 1 unknown/ambiguous symbol, missing db,
or unwritable `--output`; 2 usage (no mode, both modes, `--depth` with
`--modules`, `--min-weight` with `--symbol`, bad `--format`).

### ccg stats — database summary

```
ccg stats [--json]
```

```
$ ccg stats
db               .ccg/main/ccg.db (schema 1, app 0.1.0)
indexed at       2026-10-11T09:41:02Z   root E:/src/widget
files            898 total: 874 ok, 0 partial, 22 failed, 2 pending
symbols          18411
                   method 6102   function 5988   field 2511   record 1531
                   constructor 902  destructor 601  enum 401  enum_constant 288
                   variable 201  type_alias 86
edges            50237
                   calls 44891  extends 1523  overrides 1502  type_alias 1502
                   friend_of 780  specializes 139  uses 8541
includes         15203 (angled 11022, quoted 4181)
imports          135
strings          33902 interned (~1.9 MB)
db size          9.4 MB   wal 0.2 MB
```

`--json` mirrors the same fields (`{"db":{...},"files":{...},"symbols":{"total":N,"byKind":{...}},"edges":{...},"includes":{...},"strings":{...}}`).
Stats on a nonexistent/unopened db → error, exit 1.

## 3. Error output (uniform)

Text mode: `ccg: error: <message>` on stderr for exit 1; usage errors print
the command's usage block on stderr with exit 2. JSON mode (`--json`):
errors go to STDOUT as `{"error":"<message>","partial":false}` (plus
`"candidates":[...]` for ambiguous symbol targets) so callers can parse
everything from one stream; exit codes unchanged.

## 4. LATER (explicitly not in v1)

`search`/`query` concept ranking (BM25/FTS), `serve`/`mcp`, `watch`,
signature-aware symbol matching, multi-root registries, import-cycle
detection (once module support lands; `check --cycles` then grows an
`--imports` mode over the import graph). Do not build these in v1; the
schema and CLI shape leave room for each.
