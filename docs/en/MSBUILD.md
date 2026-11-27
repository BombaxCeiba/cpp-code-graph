# MSBUILD.md — `ccg msbuild`: indexing Visual Studio / MSBuild solutions

`ccg msbuild` turns a `.sln` into indexed ccg databases in one
command: it derives the compile command of every translation unit, writes
them into a `compile_commands.json`, and feeds it through the ordinary
`ccg index` pipeline. Windows-only by design (see Cross-platform).

There are three channels, dispatched by flag:

- **Evaluate mode (the default)**: no build runs at all. ccg loads the
  VS-shipped `Microsoft.Build.dll` through its in-process C# object host
  (the `ccg-cshost51.dll` shim) and *evaluates* each member project —
  the same evaluation MSBuild itself performs before building — to read
  the final `ClCompile` items and their metadata (37 fields: defines,
  include dirs, language standard, PCH settings, module outputs, ...).
  The argv is then *composed* ccg-side from that metadata, following the
  MSBuild CL task's own switch order. This is seconds for typical
  solutions (the design anchor: 4.5 s for a 59-project / 1655-TU
  solution) and produces no build products, no sandbox, no stubs. See
  section 2a.
- **Record mode (`--record`, the test/verification facility)**: the full
  build graph executes for real (dependency scheduling, `CustomBuild`
  and `midl` generators actually run — that is what keeps the TU set
  complete), but the compile-class tools are stubbed — `cl`, `link`,
  `lib`, `rc`, `mt`, `bscmake`, `xdcmake` record their full argv
  (RSP-expanded) and write nothing at all (a zero-disk record run: stub
  products have no consumer, and writing them would clobber real
  PDB/lib/obj/PCH files sitting on hardcoded paths). Record mode is how
  the wrapped-capture layer is regression-tested and how the
  ground-truth comparison arm is produced (same fixture, both channels,
  diff the two compile databases — the record arm is what MSBuild
  *actually* invokes; the evaluate arm is what ccg *derives*). See
  section 2b.
- **`--full-build`**: real compilation (the historical pre-record
  default) — every cl really runs; use it as the gold-standard
  cross-check when you need the real compile's verdict. See section 2b.

## 1. One database FILE per configuration (the model)

One MSBuild `Configuration|Platform` = one `compile_commands.json` = ONE ccg
configuration directory. `Debug|x64` becomes the stem `Debug-x64` (`|` is
illegal in a Windows file name) and the stem IS the configuration
directory name (the centralized layout; `-o DIR` overrides the
layout folder, the registry stays anchored at the project root):

```
<project-root>/                root = directory of the .sln
  .ccg/                        layout folder (default; -o DIR overrides)
    msbuild.json               registry (cross-config discovery, section 4)
    Debug-x64/                 one configuration directory per pair
      ccg.db                   the configuration's database
      compile_commands.json    clangd/cl-tidy usable as-is
      bmi-self-.../            the configuration's BMI cache subtree
      capture.db               SQLite capture table (raw cl invocations;
                               record mode only)
    Release-x64/...
    msbuild/Debug-x64/         transient extraction area (record mode: stub
                               tools, the CCG_CLWRAP_LOG anchor path;
                               evaluate mode: the refresh snapshot below)
    msbuild/capture-<stem>.lock  per-config capture mutex (all channels)
```

Why per-config DATABASES instead of a config column: the graph of one project's
configurations is ~90-100% identical, and only `#ifdef`-gated definitions
differ. A shared db would make every common symbol an ambiguous
multi-match for `query context callers callees impact trace` (the tier-1
ambiguity rule prints candidates and exits 1), silently merge identical
call sites across configurations, and let a Release import ride over stale
Debug flags. Separate files make each db trivially, provably correct per
configuration — and independently deletable or VACUUMable. The schema and
engine are untouched by this feature.

Each db is self-describing via its `meta` table (`project_root`,
`compile_db_path`); the registry below is a convenience for listing and
defaults, never load-bearing for queries. `ccg query add -o
<root>/.ccg --config Debug-x64` works directly on any per-config db (a
layout folder holding exactly one configuration auto-selects it, per the
shared discovery rules).

## 2. The pipeline (what one run does)

Steps 1-3 (input, MSBuild and per-project cl resolution) are shared by
all channels; the channel dispatch happens at step 4 — evaluate (the
default) replaces the wrapped build with project evaluation + ccg-side
composition (section 2a), while `--record`/`--full-build` run the wrapped
`-t:Rebuild` exactly as historically (section 2b). Steps 5-7 (translate,
index, registry) are shared: both channels' invocations flow through the
SAME translation layer, which is what makes the two compile databases
directly comparable.

1. **Resolve the input**: a `.sln` file only. A `.vcxproj` or directory
   operand is rejected with an actionable usage error — pass the solution
   file; single-project input is not supported yet (exit 2).
2. **Resolve MSBuild**: explicit `--msbuild PATH` or `$CCG_MSBUILD_EXE`,
   else `vswhere.exe -latest -products * -requires
   Microsoft.Component.MSBuild -find MSBuild/**/Bin/MSBuild.exe`
   (vswhere looked up under `Program Files (x86)`, with a PATH fallback).
   Not found → exit 1 with the override hint.
3. **Resolve the real cl.exe per project**: explicit `--cl PATH`, else
   `MSBuild <input> -getProperty:VCToolsInstallDir -nologo
   -p:Configuration=C -p:Platform=P` joined with
   `bin/HostX86/<target>/cl.exe` (x64/Win32→x86/ARM64 mapping). The
   configuration pair is REQUIRED in the query: `VCToolsVersion` /
   `PlatformToolset` pins live in per-configuration PropertyGroups, and
   without `-p:Configuration`/`-p:Platform` every Condition misses and
   the resolution silently falls back to the default toolset (a project
   pinned to 14.44 resolved to 14.51 — the reported "wrong cl.exe
   version" root cause). A `.sln` refuses `-getProperty` outright, so
   ccg enumerates the member `.vcxproj` projects and runs the same
   config-aware query per member (each member's `ActiveCfg` from
   `ProjectConfigurationPlatforms`), producing a per-project cl map; the
   vswhere "highest toolset wins" fallback is the last resort per
   project, and verbose/registry report the resolution source. Extra
   `--msbuild-args` ride along after the configuration properties, so an
   explicit `/p:` still overrides.
4. **Extract** — the channel dispatch point. In evaluate mode (the
   default) this step is the evaluation session + composer of section
   2a; the rest of this step describes the build channels
   (`--record`/`--full-build`, section 2b): the wrapped `-t:Rebuild`
   runs with the child environment carrying `CCG_CLWRAP_LOG=<capture
   log>`, `CCG_CLWRAP_REAL_CL=<primary cl>` and — for multi-project
   solutions — `CCG_CLWRAP_CL`, a `<project dir>	<cl>` prefix map the
   wrapper consults on EVERY invocation, picking the cl by cwd (longest
   prefix) so a mixed-toolset solution compiles (or, in record mode,
   records) each TU with its own project's compiler. The
   `CCG_CLWRAP_LOG` value is only the trigger contract and derivation
   anchor: the captured data itself lands in
   `<layout>/<C>-<P>/capture.db` (a SQLite `capture` table, WAL +
   busy-timeout, one row per wrapped invocation with `{argv, cwd,
   INCLUDE, real_cl}` — wide command line, unicode-exact, `@rsp`
   expanded; error rows carry a `message` instead). `-t:Rebuild` is the
   default so the capture covers every TU (a plain Build only records
   re-invoked cl calls).
5. **Translate** to `compile_commands.json` (arguments
   shape) — the same layer for both channels (record: the captured log;
   evaluate: the composed invocations). Flags are kept VERBATIM in cl
   mode: entries begin
   `clang-cl --driver-mode=cl` followed by the recorded cl flags, with
   only the proven-necessary edits — `/I /external:I /FI` values
   absolutized against the recorded working directory, `/Fo /Fd /Fp /Yc
   /Yu` and the two hard-fail spellings `/Gf` `/u-` dropped, batched
   sources split into one entry per TU, and non-C/C++ invocations
   (rc/midl/...) filtered out. A separate `/D` define is SPLICED into one
   token (`"/D", "NDEBUG"` → `/DNDEBUG`): clang response files split on
   whitespace, and two tokens would read as two arguments. Two
   semantic-equivalence appends (each visible in the generated cdb; entries
   that already declare the option stay verbatim): an entry whose captured
   cl argv declares no template mode (`/permissive-`, `/Zc:twoPhase`,
   `/Zc:twoPhase-`; case-insensitive) gets `/Zc:twoPhase-` at the tail —
   clang-cl's official spelling of MSVC-default delayed template parsing
   (`-fdelayed-template-parsing`'s formal alias) — and an entry that
   declares no source encoding (no `/utf-8`, no `/source-charset`) gets
   `-finput-charset=<system ANSI codepage>` (probed at run time — Windows
   `GetACP` mapped to its charset name, e.g. GBK for CP936; never
   hardcoded), because cl reads BOM-less sources in the ANSI codepage
   while clang defaults to UTF-8. BOM-carrying files are unaffected by
   that flag (the BOM decides; pinned by the four-quadrant
   GBK/UTF-8 x BOM/no-BOM fixtures in `tests/clang.test.ixx`).
   The captured `INCLUDE` environment is
   **materialized in full**: every non-empty entry (project dirs from the
   `<IncludePath>` property / VC++ Include Directories, the MSVC toolset,
   `VC/Auxiliary/VS`, the Windows SDK and NETFXSDK) is appended after
   every recorded flag as `-imsvc DIR` pairs in captured order, and `/X`
   is inserted. `/X` makes the argv the ONLY source of system headers:
   clang-cl stops auto-discovering a toolset and ignores the indexing
   shell's `INCLUDE`. The per-TU captured `INCLUDE` toolset directory
   (`.../VC/Tools/MSVC/<ver>/...`) yields
   `-fms-compatibility-version=19.<minor>.<build>`, falling back to the
   invocation's `real_cl` path when `INCLUDE` has no toolset directory.
   This is what makes a mixed-toolset `.sln` right per TU: the wrapped
   `real_cl` is one value for the whole build (the highest toolset), only
   each project's `INCLUDE` reveals its real headers. If the original cl
   command already had `/X` (IgnoreStandardIncludePath; `/x`, `-X` and
   `-x` are cl's same option), cl itself ignored `INCLUDE`, so those dirs
   are NOT added back; the spelling is canonicalized to `/X` because
   clang-cl only recognizes that spelling.
   The loader drops the `clang-cl` token and replays the rest natively.
6. **Index** through the exact `ccg index` code path (same store, same
   incremental engine, same summary output), with the project root as the
   index root and `<layout>/<C>-<P>/ccg.db` as the db. `--export-only`
   stops after step 5.
7. **Update the registry** (section 4). `--all` repeats 2-6 for every
   `Configuration|Platform` the input declares.

### 2a. Evaluate mode (the default)

The default channel never runs MSBuild's build graph. What runs is the
*evaluation* MSBuild performs before any target executes, driven from
inside the ccg process:

- **The engine host**: the evaluation engine is the VS-shipped
  `Microsoft.Build.dll` (a .NET Framework 4.7.2 assembly), loaded in
  process through ccg's own C# object host (`ccg-cshost51.dll`, the
  shim also serving the Select-String fast path; loaded on the CLR 4
  line, which every Windows 10 1903+ machine carries as .NET Framework
  4.8). ccg creates one `MsbuildEval` session object per configuration
  and pulls one event per member project through it — no subprocess per
  project, no compile, no stubs, no sandbox, no capture db. The
  assembly directory is the `MSBuild.exe` directory found by the usual
  discovery (step 2); `VCTargetsPath` is derived from the VS install
  root (the highest `vNNN` directory under `MSBuild/Microsoft/VC/` that
  carries both guard props, existence-verified, trailing backslash).
- **Filters**: projects whose `Build.0` participation bit in the `.sln`
  is off are skipped entirely (a solution-config not building a project
  produces no corpus for it — correct, not a gap), and `ClCompile`
  items with a truthy `ExcludedFromBuild` are skipped per item.
- **The composer**: each remaining item becomes one invocation — argv[0]
  is that project's cl (the per-project resolution of step 3), the
  flags are composed from the item's evaluated metadata (37 fields)
  following the MSBuild CL task's own switch order:
  `PreprocessorDefinitions` → ordered `/D`s, `AdditionalIncludeDirectories`
  → ordered `/I`s, `LanguageStandard` → `/std:`, the
  `PrecompiledHeader` triple → `/Yc`/`/Yu` + `/Fp`,
  `ModuleOutputFile` → `/ifcOutput <value>` (paired-form, per the
  single-source `cg.util.msvcswitch` list), `ObjectFileName` → `/Fo`,
  `AdditionalOptions` verbatim, and so on. Unknown/unmapped metadata
  fields are DISCLOSED (verbose `compose: field X unmapped, skipped`)
  and never invented into flags the CL task would not emit. A `.ixx`
  interface unit gets `/interface` (explicit `CompileAs` module, or
  `Default` + `ModulesSupported` + the extension); each module unit is
  a single-pass single entry — the record channel's second scan pass
  has no counterpart here by construction.
- **INCLUDE**: the evaluate channel reads the ccg process's own
  `INCLUDE` for the `-imsvc` materialization. A VS developer prompt (or
  an exported `INCLUDE`) gives the full system set; an empty INCLUDE
  composes the corpus without `-imsvc` (the run says so, and indexing
  reports per-TU include failures — the record channel differs there:
  its wrapper captures the VS-initialized INCLUDE of the MSBuild
  child). Run ccg from a VS developer prompt for full fidelity.
- **Warm-checkout precondition**: evaluation sees every ClCompile item
  the project *declares* — including generated module units that are
  explicit project entries — but the files must exist on disk for
  indexing to parse them. A cold checkout (generators never ran) is
  disclosed up front (`evaluated N TUs, M sources missing on disk —
  build once so generators produce them`); build the project once, then
  index. Undeclared build-time generated TUs (a CustomBuild step
  emitting a `.cpp` nobody lists) are outside the evaluate corpus by
  construction — the record channel sees them (its generators really
  run); that is one of the documented uses of `--record`.
- **Failures are classified, never silent**: a project-level evaluation
  error fails that project only (the rest of the solution proceeds; the
  failed projects are disclosed and counted in `--json` as
  `failed_projects`); a missing import (MSB4019) degrades to
  ignore-missing-imports evaluation with a `evaluated with missing
  imports ignored: <project>` note; channel-level failures — the
  shim/CLR unavailable, an evaluation error code, a watchdog timeout
  (`CCG_EVAL_TIMEOUT_MS`, default 300 s — the anchor is 4.5 s for 59
  projects, so 66x headroom), or the whole solution failing — abort
  the refresh keeping the previous corpus (no cdb written, db
  untouched, registry not updated, exit 1). Nothing ever falls back to
  another channel automatically: the production path is evaluate only;
  `--record` is an explicit user action.
- **The refresh snapshot (the R11 hash gate)**: the tail of every
  successful evaluate capture writes
  `<root>/.ccg/msbuild/<stem>/snapshot.json` — the identity of the
  inputs that produced the corpus: the sha256 of the `.sln`, every
  member `.vcxproj`, and every project's import closure (the
  evaluation produces it for free), plus the `INCLUDE` hash, the
  MSBuild/cl paths, the derived VCTargetsPath, and the ccg version.
  `ccg update`'s project-file linkage reads it before re-capturing: all
  inputs unchanged ⇒ the event is absorbed
  (`"projectRefresh": "skipped-unchanged"`, no capture spawned); any
  mismatch ⇒ the real refresh. The snapshot is written only on success
  — failure-keep-old keeps the snapshot too.

### 2b. Record mode (--record) and --full-build

Record mode is the explicit `--record` channel — the test/verification
facility. It reuses the exact extract pipeline of step 4 but changes
what the wrapped tools do after logging:

- The stub tool family: ccg.exe is hard-linked (copied cross-volume) into
  `<root>/.ccg/msbuild/<C>-<P>/stub-tools/` under the tool names —
  `cl.exe`, `clang-cl.exe` (ClangCL toolsets), `link.exe`, `lib.exe`,
  `rc.exe`, `mt.exe`, `ml64.exe`, `ml.exe`, `armasm.exe`, `marmasm.exe`,
  `bscmake.exe`, `xdcmake.exe`. MSBuild is pointed at them via the
  `ToolPath` properties (`-p:CLToolPath=…` etc. — `CLToolExe` is
  deliberately NOT pinned so the toolset's own default name, `cl.exe` or
  `clang-cl.exe`, survives), and the stub directory is prepended to the
  child `PATH` (a tool that resolves by bare name lands on the stub). One
  known gap on current MSVC: the MASM targets resolve `ml64.exe` by an
  absolute toolset path, so the PATH prepend does not intercept it and
  `ml64` runs FOR REAL — its `.obj` is an MSBuild-managed product and
  lands in the sandbox like the generators' outputs (harmless; a real
  `.asm` never touches the project tree). Each stub records argv and
  writes NOTHING (the
  zero-disk contract): stub products have no consumer inside a record run
  (link/lib/mt are stubs too; `midl` and the generators are real and
  unrelated to them), and writing them would overwrite real PDB/lib/obj/PCH
  files sitting on hardcoded paths (observed LNK1201/C1041 in the user's
  next real build). Downstream MSBuild steps that need those products to
  exist (deployment copies, timestamp checks) fail LOUDLY instead — that
  failure is the expected record-mode behavior (see the post-build note
  below), not something to paper over with placeholder files.
- `midl.exe` and CustomBuild/CustomBuildStep/pre-/post-build events run
  FOR REAL. midl's generated headers/`.c` files are compile inputs —
  stubbing them would break parsing — and the generators are what keep
  the TU set complete (a design-time enumeration cannot see TUs that
  only materialize during the build).
- **Sandbox**: `-p:IntDir`/`-p:OutDir` redirect all MSBuild-managed
  products to `<layout>/<C>-<P>/msbuild-sandbox/{int,out}/`. The real
  project tree is snapshotted before and scanned after the run; any NEW
  file outside the sandbox (and outside `.ccg`) is reported as a named
  `sandbox-leak` warning on stderr and in `--json` (`sandbox_leaks`
  list) — never silently dropped. Files that were MERELY overwritten in
  place (a post-build copy onto an existing file — no new file appears)
  count as leaks too. Generator products that land outside the sandbox
  are EXPECTED, not leaks: midl runs for real (its generated headers are
  compile inputs) and CustomBuild generators write their declared outputs
  into the tree — CustomBuild-declared outputs with SOURCE extensions
  are listed as notes (they are index input), and the rest (a declared
  non-source output, midl's `.tlb` / `_i.c` / `_p.c` / `dlldata.c` / the
  generated header beside its `.idl`) collapses into ONE info line,
  `out-of-sandbox generator products: N (expected)` (JSON:
  `generator_products`). Truly unexpected writes stay per-file leak
  warnings. On top of the tree diff, a canary cross-checks
  every output path parsed from the captured argv (all five families —
  `/Fd`, `/PDB`, lib `/OUT`, `/Fo`, `/Fp`): a path outside the sandbox
  and `.ccg` that is new or was overwritten relative to the pre-run
  snapshot (paths outside the solution root fall back to a
  modified-time-in-run-window test) is reported the same way. Under the
  zero-disk contract the canary should NEVER fire — it exists to call out
  a broken no-op the moment it happens.
  Hardcoded escapes (a literal
  `/Fo<projectdir>/hardobj/` in AdditionalOptions, a `Link`
  `OutputFile` outside `$(OutDir)`, a CustomBuild step writing into the
  source tree) are exactly what this catches. A project's own
  `<OutDir>`/`<IntDir>` properties lose to the command-line `/p:`
  overrides by MSBuild's precedence — that is the intended sandbox
  behavior, and MSBuild itself warns (MSB8012) when a `Link OutputFile`
  contradicts it.
- **Post-build step failures are classified, not fatal-looking**: when
  MSBuild exits 1 with MSB3073/exec-class errors (VCEnd/PostBuildEvent
  copy or verify steps), the run says so as a note — expected in record
  mode: the stubs write no products, so steps that consume them find
  nothing to copy or verify; the captured compile commands are still
  translated and indexed. The exit-code semantics are unchanged. The
  noisy error LINES themselves are filtered out of the console tee
  (MSB3073 / `exited with code` lines, buffered and counted) and
  summarized in ONE line — `post-build/exec failure lines on empty
  record-mode products suppressed: N (expected in record mode)` — so a
  big solution no longer scrolls thousands of expected failures; the
  full text is still accumulated and the classification above still
  sees it. `--full-build` tees everything verbatim. No MSBuild property
  is injected for this.
- **Non-exec failures abort the refresh and keep the previous corpus
  (record mode)**: in RECORD mode any OTHER nonzero MSBuild exit
  (malformed project XML, a missing import, MSBuild itself crashing)
  means the evaluation/orchestration layer never ran properly — the
  stubs write no products, so a record-mode build has no legitimate
  build-time failure — and the capture cannot be trusted. ccg does
  not write the new cdb, does not touch the db, does not update the
  registry entry for that config, prints
  `project refresh aborted; database keeps the previous corpus`, and
  exits 1. The db always stays at its last complete good state — there
  is never a half-refreshed corpus. This is also the contract the
  `ccg update` project-file linkage relies on (see REALTIME.md):
  a failed triggered re-capture retains the queued line so the next
  edit retries it. `--full-build` keeps the historical honesty
  contract for ALL its failures (the wrapper records before
  forwarding, so even a failing real compile leaves a complete
  capture: still translated and indexed).
- **Captures of the same config are mutually exclusive**:
  `<root>/.ccg/msbuild/capture-<stem>.lock` (pid + timestamp,
  atomically created) is held for the whole capture/translate/index/
  registry window of one config. A second capture of the same config
  fails loudly (`another capture is running ... retry when it finishes`,
  exit 1); a lock left by a dead or crashed holder is reclaimed
  automatically (pid liveness probe + timestamp staleness). The
  `ccg update` linkage only PROBES this lock (read-only) before
  spawning the capture — the lock itself is owned by the capture.
- **Toolchain discovery is cached in the registry**: a capture with no
  explicit `--msbuild`/`--cl`/`--msbuild-args` reuses the registry's
  recorded MSBuild/cl paths when they still exist on disk (sparing the
  per-project `-getProperty` seconds); any miss (path gone, registry
  entry absent, explicit overrides present) re-runs the full discovery.
  Verbose prints `toolchain discovery reused from the registry`.
- Record mode cannot detect compile errors — no cl ever runs. The
  honesty contract shifts to the loader (parse failures surface as
  `failed:` exactly like `ccg index`). Use `--full-build` when you need
  the real compile's verdict.

`--full-build` is byte-for-byte the historical behavior: only `CLToolExe`/
`CLToolPath` point at ccg.exe, every cl invocation is forwarded to the real
compiler (selected through the same per-project prefix map — mixed-toolset
solutions now compile each project with its own cl), no sandbox, no leak
scan. `--full-build` costs a real `-t:Rebuild`.

### Exit codes

| code | meaning |
|---|---|
| 0 | success — failed TUs allowed (matches `ccg index`); the db is usable |
| 1 | runtime error — MSBuild/cl not found, empty capture, bad registry, non-exec build failure, evaluate-channel failure (undeducible inputs / watchdog timeout / E3 — refresh aborted, previous corpus kept), another capture holds the lock |
| 2 | usage error — unknown option, missing/bad `--platform`, `--all` combined with `-o/--config/--compile-commands`, `--list` combined with build options, `--record` combined with `--full-build`, no project found |

### Cross-platform (Windows-only by design)

The MSBuild feature is Windows-only BY DESIGN. The translator, the
registry format, and the CLI parsing are platform-neutral and compile
everywhere; ALL Windows APIs (vswhere and registry access) live inside
`src/msbuild/` behind `_WIN32` guards (spawning child processes and reading
the command line go through the platform unit) with the
non-Windows path returning the unsupported error. On a
non-Windows machine, `ccg msbuild --list` (a pure registry read) works and
every extracting invocation fails cleanly with
`ccg msbuild: not supported on this platform` (exit 1) — never a compile
error or a silent no-op.

## 3. Usage

```
ccg msbuild <path.sln> [--config C] [--platform P]
            [--all] [--export-only] [--record] [--full-build]
            [--msbuild-args "extra /p:..."]
            [--cl PATH] [--msbuild PATH] [-o PATH] [--compile-commands F]
            [--force] [-v] [--json]
ccg msbuild --list [<path>]
```

- `--config C` / `--platform P` — which pair to build. Defaults: the
  registry's `default` entry, else `Debug` / `x64`. Values become part of
  a file name, so whitespace and `/\:*?"<>|` are rejected (exit 2).
- `--all` — every `Configuration|Platform` the solution declares (`.sln`
  SolutionConfigurationPlatforms), one db each, built sequentially. Not
  combinable with `--config`,
  `--platform`, `-o`, or `--compile-commands` (they cannot name N files).
- `--export-only` — stop after writing `compile_commands.json`; prints
  the path (text) or `{"config","platform","mode","compile_commands",
  "tus","failed_projects","sandbox_root","sandbox_leaks"}` (JSON; the
  `failed_projects` field is evaluate-mode only, the sandbox fields are
  record-mode only). The file is directly usable by clangd or cl-tidy.
- `--record` — run the wrapped-build capture channel explicitly
  (section 2b): the test/verification facility (wrap-layer regression,
  ground-truth comparison arm, manual forensics of what MSBuild really
  invokes). Mutually exclusive with `--full-build`.
- `--full-build` — real compilation (section 2b): every cl really runs
  (and MSBuild's real compile verdict surfaces). Evaluate is the
  default; `--record` and `--full-build` are the two explicit build
  channels.
- `--msbuild-args S` — extra MSBuild arguments spliced VERBATIM at the
  end of the command line (your quoting survives).
- `--cl PATH` / `--msbuild PATH` — skip the corresponding discovery.
- `--clang-compile-flags F` — extra clang flags appended to every replayed
  argv at index time (repeatable; the generated compile_commands.json is
  not modified — see CLI.md `ccg index`); the per-database
  `clang-compile-flags` config key applies when the flag is absent.
- `-o DIR` — output FOLDER (the layout folder; the configuration
  directory `<Config>-<Platform>/` with its `ccg.db` lives inside).
- `--force` — explicit spelling of recreating the db on schema-version
  mismatch; the msbuild route RECREATES BY DEFAULT there now (an upgrade
  no longer needs a manual `--force`).
- `--json` — machine output; with `--all`, one summary object per
  configuration, one per line, in build order.
- `--list` — print the registry (section 4); the operand (default: the
  current directory) only locates the registry root. Missing registry →
  exit 1 (first-run hint), never an empty table.

With `-v`, each configuration prints its resolved plan line (db/cdb/log
paths, MSBuild and the per-project cl paths with their resolution) to
stderr before building.

## 4. The registry (`<root>/.ccg/msbuild.json`)

Written by `ccg msbuild` only; hand-editable; absence = first-run
behavior. Paths are stored relative to the project root where possible.

```json
{
  "version": 1,
  "generator": "msbuild",
  "default": "Debug-x64",
  "configs": [
    {
      "name": "Debug-x64",
      "configuration": "Debug",
      "platform": "x64",
      "solution": "DemoSln.sln",
      "db": ".ccg/Debug-x64/ccg.db",
      "compile_commands": ".ccg/Debug-x64/compile_commands.json",
      "msbuild": "C:/.../MSBuild.exe",
      "cl": "C:/.../HostX86/x64/cl.exe",
      "cl_resolution": "project-property",
      "cl_projects": [ {"project": "projA",
                        "cl": "C:/.../14.51.../cl.exe",
                        "resolution": "project-property",
                        "dir": "C:/src/DemoSln/projA",
                        "active_cfg": "Debug|x64",
                        "in_build": true},
                       {"project": "projB",
                        "cl": "C:/.../14.44.../cl.exe",
                        "resolution": "project-property",
                        "dir": "C:/src/DemoSln/projB",
                        "active_cfg": "Debug|Win32",
                        "in_build": false} ],
      "mode": "evaluate",
      "extracted_at": "2026-10-12T12:00:00Z",
      "tus": 5, "failed": 1
    }
  ]
}
```

A run replaces (or appends) its configuration's entry and keeps the
others; `default` points at the first configuration of the most recent
run that is still listed. `cl` is the primary project's compiler;
`cl_resolution` says how it was found (`override` | `project-property` |
`vswhere-fallback` — the fallback never consulted the project's pins);
`cl_projects` carries the per-project map for `.sln` inputs
(mixed-toolset solutions differ per member; each entry's `resolution`
uses the same vocabulary as `cl_resolution`, so the per-project honesty
survives restarts); each entry also discloses `dir` (the project
directory, absolute forward-slash path), `active_cfg` (the member's
ActiveCfg mapping under this solution configuration, e.g.
`Debug|x64`), and `in_build` (the Build.0 participation flag —
`false` means the solution loads it but does not build it under this
configuration; the values come from the same sln parse that filters
members at extraction time). These three keys are written only when
known: older entries lack them (read back as unknown, never as
`false`), and `.vcxproj` inputs carry no ActiveCfg/Build.0 concept,
so they omit them too. `mode` is `evaluate` (the default channel),
`record`, or
`full-build` (older registries without the field read as their
historical channel). All new fields are additive: older registries parse
unchanged. A corrupt registry is a loud error, never
silently ignored. `ccg msbuild` with no `--config` resolves `default`
through the entry table (configuration + platform fields), falling back
to splitting the stem at the last `-`.

## 5. Failure modes

| failure | behavior |
|---|---|
| MSBuild / vswhere not found | `MSBuild not found (Visual Studio + C++ workload required; pass --msbuild PATH or set CCG_MSBUILD_EXE)`, exit 1 |
| cl resolution fails | error listing what was tried (`-getProperty:VCToolsInstallDir` + HostX86 join, vswhere find) with the `--cl` override hint, exit 1 |
| `.vcxproj` / directory input | actionable usage error asking for the solution file (single-project input not supported yet), exit 2 |
| evaluation inputs not derivable (evaluate) | guidance error (`cannot derive the VS installation root ...` / no VC targets directory, candidates listed) + failure-keep-old, exit 1 |
| evaluation channel failure (evaluate) | shim/CLR unavailable (E1), channel error code or bad event line, watchdog timeout (`CCG_EVAL_TIMEOUT_MS`, default 300 s), or every member project failed (E3) → `project refresh aborted; database keeps the previous corpus`, nothing written (cdb/db/registry/snapshot), exit 1 — never a silent channel switch |
| some member projects fail to evaluate (E4) | the failed projects are disclosed per line (`eval project failure: <name> (<error>)`), counted in `--json` `failed_projects`, and the successful projects' commands are still composed and indexed (exit 0) |
| missing import (evaluate, E5) | degraded evaluation with `IgnoreMissingImports` (`evaluated with missing imports ignored: <project>` note), full corpus still composed |
| the build fails (compile errors, `--full-build`) | still translate the captured log — the wrapper logs every invocation BEFORE forwarding, so even TUs whose cl run failed are captured — index what parses, exit 0, and SURFACE `MSBuild exited N for <config>` on stderr |
| record mode compile errors | none can be detected — no cl runs; the loader surfaces unparseable TUs as `failed:` (exit 0, like `ccg index`) |
| sandbox-leak (record mode) | every NEW or overwritten file in the project tree outside the sandbox is a named `ccg msbuild: sandbox-leak: <path>` warning (stderr + `--json` `sandbox_leaks`); generator products (CustomBuild-declared outputs, midl's tlb/generated headers) are expected — source-class outputs are notes and the rest collapses into the one-line `out-of-sandbox generator products: N (expected)` info (JSON `generator_products`); captured-argv output paths outside the sandbox/`.ccg` are canary-checked the same way (theoretically never fires under the zero-disk contract); the run still exits 0 — the leak report is the product |
| log with 0 cl invocations (build channels) | hard error naming the capture db in full: `no compiler invocations captured for <config> (capture db: <full path>; log: <CCG_CLWRAP_LOG value>) — was the project already up to date? (-t:Rebuild is used by default; check --msbuild-args)`, exit 1; the evaluate counterpart names the member count (`no ClCompile items composed for <config> ...`) |
| capture-log error rows | a wrapped invocation that could not expand an `@rsp` or spawn cl writes an error row (kind=`error`, `message` column) in `capture.db`; the run reports each on stderr and indexes what was captured |
| project sets CLToolExe/CLToolPath | the run's `/p:` globals win for that invocation only; ccg prints a note when it sees the property in the project file |
| schema_version mismatch on an existing per-config db | recreated by DEFAULT (a note names the old version); `--force` remains the explicit spelling |

### Evaluation-channel error codes (`rc`)

The evaluate-channel failure row above aborts with `evaluation failed
for <config>: <error>`, and that `<error>` can carry bare numbers —
`Begin failed rc=-13 payload=...`, `NextEvent failed rc=-14 payload=...`,
`MsbuildEval session open failed, rc=-30 detail=...`. Those
`rc` values come from the C# host channel that carries the evaluation
session (`ccg-cshost51.dll` on the CLR 4 line): `0` is success, a
positive value is a handle token, and every negative value is one of
the codes below. Codes `-1`..`-20` are returned by the shim itself;
`-30`/`-31` are host-side supplements (the engine line could not be
loaded at all, or the request never fit the channel) — the range split
keeps attribution unambiguous. The list is complete; the same message
also carries the diagnostic text (`payload=` / `detail=`), which names
the concrete cause.

| rc | constant | meaning | typical trigger |
|---|---|---|---|
| `-1` | `ErrChannel` | channel exception (shared memory / struct bridge) | the transport itself failed — the per-call shared-memory mapping could not be created or mapped, or the in-process CLR invocation failed before the shim code ran; resource pressure, not a payload problem |
| `-2` | `ErrPayload` | command payload invalid (bad JSON, missing argument, length out of range) | the JSON ccg sent failed to parse or lacked a required key — in practice a ccg executable and a shim dll from different versions sharing one layout |
| `-3` | `ErrUnknownClass` | `ObjCreate` class name unknown | the creation request named a class this shim does not implement — again a mixed-version layout |
| `-4` | `ErrConstruct` | session construction threw (the exception text rides back in the same message) | `MsbuildEval` could not be built — typically the resolved MSBuild directory lacks the VS-shipped `Microsoft.Build` assemblies |
| `-5` | `ErrTableFull` | shim handle table full | a defensive backstop: the table keys entries by generation-bearing tokens (multiple generations coexist per slot), so 1024 is just the slot-index bit width, not a cap on concurrent objects; it fires only after every one of the 1024 slots' generation counters has been pushed into the overflow/wrap-out-of-positive-token range (millions of register-teardown cycles per slot) — not reachable in normal operation, and seeing it means an abnormal shim-side handle-table state, not load |
| `-6` | `ErrAbi` | struct-bridge ABI mismatch (size/version check) | 7-line only (the Select-String face); the evaluate channel is pinned to the 51 line and never reports this code |
| `-10` | `ErrBadHandle` | handle value malformed (not a number / outside int range) | protocol misuse or a mixed-version layout |
| `-11` | `ErrStaleHandle` | handle already invalid (torn down, or never existed) | a call after the session was torn down (reference count hit zero — an old token is permanently invalidated, never reused), or on a session the watchdog already abandoned |
| `-12` | `ErrUnknownMethod` | method name unknown or signature off-protocol | the requested method does not exist on the session object — a mixed-version layout |
| `-13` | `ErrMethodFailure` | the session method threw (diagnostic text rides back in the same message) | a session-protocol violation (an empty/malformed `Begin` request, `NextEvent` past the summary event) or an unexpected shim-internal error; ONE project failing to evaluate is not this code — that is an event and surfaces as the per-project E4 disclosure |
| `-14` | `ErrOverflow` | result exceeds the channel-view capacity of the handle's tier | one project's event payload does not fit the evaluation session's 4 MB result view (the large tier is sized for hundreds of TUs × 37 fields) |
| `-20` | `ErrNotOnLine` | class not hosted by this line | a `MsbuildEval` creation reached the 7 line (CoreCLR cannot load the net472 MSBuild assemblies); the evaluate channel never routes there — the code exists to make mis-routing fail loudly |
| `-30` | `LineUnavailable` | host side: the target line's engine is unavailable | the 51-line CLR host could not be loaded — the E1 shape; the `detail=` text names the failing step (shim dll missing beside the executable, CLR 4 failing to start) and the remedy |
| `-31` | `CmdTooLarge` | host side: the command payload exceeds the channel tier's capacity | the `Begin` request — it carries the full member-project list — exceeded the evaluation session's 64K-character command area; a solution with an enormous project count |

### Failure-class triage map (loader side)

Record mode cannot detect compile errors (no cl runs) — the loader's
`failed:` TU classes are the honesty channel. The remedy map lives in
CLI.md (section *Failure-class → remedy map*); the MSBuild-relevant
cells, with this route's automatic alignments already in place:

| failure shape / class | remedy |
|---|---|
| varargs shape (`cannot pass object of non-trivial type ... through variadic function`) | `--clang-compile-flags "-Wno-non-pod-varargs"` (proven effective on a large MSVC codebase) |
| source-encoding garbage on GBK/Shift-JIS/... sources | ALREADY ALIGNED on this route (`-finput-charset=<system ANSI codepage>` appended to entries that declare no encoding; BOM files unaffected). A plain compile db needs the manual route: convert to UTF-8 (BOM preferred) or inject `-finput-charset=<codepage>` |
| delayed-template-parsing residual (`msvc-permissive-unchecked-member-call`, `msvc-permissive-unchecked-ctor`) | `--clang-compile-flags "-fdelayed-template-parsing"` (the translation layer already appends `/Zc:twoPhase-`; this covers the residual shapes) |
| `msvc-permissive-unchecked-constexpr` (constexpr bound to a dllimport/runtime value in a template) | **no switch exists — fix the source** |
| `msvc-permissive-lenient-specialization` (class-template member definition missing `template<>`) | ccg rewrites it automatically (cl dialect, diagnostic-driven via the compiler's own fixit, in-memory only — never touches disk; see CLI.md) |
| lambda-capture shadowing shape (MSVC binds the outer entity where clang binds the capture) | **no switch exists — fix the source** |

## 6. Known limitations and side effects

- **tlog rebuild-once**: the wrapper builds rename MSBuild's incremental
  tracking files (`CL.command.1.tlog` etc. — named after the tool the
  tracker launched), so your NEXT normal build of that project rebuilds
  once. Harmless and not fixable from outside MSBuild.
- **Rebuild clobbers obj/bin outputs (only `--full-build`)**: record mode
  redirects every MSBuild-managed product into the sandbox
  (`<layout>/<C>-<P>/msbuild-sandbox/`); the project's real tree only
  ever sees leaks (reported). `--full-build` runs a real `-t:Rebuild`
  and regenerates the project's own `build/` outputs — build in a
  scratch copy when that matters.
- **`VCToolsVersion` pins must be full four-part versions**
  (`14.44.35207`, not `14.44`): current MSBuild fails a short pin with
  MSB8070 — the project does not build at all, under ccg or standalone.
  This machine-independent MSBuild behavior is a likely source of "I
  pinned the toolset but the build / version sensing is confused"
  reports.
- **compatibility version build segment**: `-fms-compatibility-version`
  takes `<minor>.<build>` from the TOOLSET DIRECTORY number
  (`14.44.35207` → `19.44.35207`), which can differ from the cl binary's
  own `_MSC_FULL_VER` build segment (e.g. directory `35207` vs binary
  `194435229`). clang gates on major.minor, so only the
  `_MSC_FULL_VER` macro's build segment is affected; the discrepancy is
  documented rather than papered over.
- **non-ASCII TU paths**: extraction is unicode-clean end to end (wide
  argv, UTF-8 logs), but ccg's INGEST of a TU whose path contains
  non-ASCII currently fails (the JSON reader lacks `\uXXXX` decode and
  narrow `fs::path` goes through the ANSI codepage). Such a TU is
  reported as `failed` with exit 0 — honest, deterministic, and the
  subject of its own future fix.
- **capture log name carries a token** (`clwrap-<ns>.jsonl`): MSBuild's
  file tracker records the log in the project's write-tlogs, so a fixed
  name would be deleted by a later project's Clean MID-BUILD on
  multi-project solutions. The token lives on in the `CCG_CLWRAP_LOG`
  value (the derivation anchor) and in `capture.db`'s `token` column —
  the reader filters by the current run's token, so stale rows can never
  leak into a translation; the capture db itself is pruned at the start
  of each run.
- **`-t:Rebuild` is the build-channel default**: full TU coverage needs
  the complete build graph; record mode makes that cheap (no compile
  pass — only the graph walk plus generators). Under `--full-build` it
  is a real full compile. A plain-Build mode (capturing only re-invoked
  TUs) remains a later optimization. The default evaluate channel needs
  no build at all — its TU coverage is the evaluated `ClCompile` set.
- **System-header self-containment**: an entry written by `ccg msbuild`
  carries `/X` plus the complete `INCLUDE` as ordered `-imsvc`
  pairs (record: captured from the MSBuild child; evaluate: read
  directly from the ccg process — run from a VS developer prompt for
  the full system set). Indexing that cdb therefore no longer depends
  on the shell's `INCLUDE`/`CPATH` or on clang-cl auto-discovery: pure
  shell, vcvars shell and a captured-name-only environment produce the
  same graph (the evaluate channel's cold-shell entries carry no
  `-imsvc` and say so — see 2a). A TU that still fails on a missing
  include reports `(N include dirs fed)` in its failure text, so a
  short search list is visible instead of mysterious.
- **Foreign MSVC cdb coverage boundary** (hand-written or third-party
  `compile_commands.json` with a native `cl.exe` entry): three cases.
  1. *Rebuild* — when the compiler token is an absolute path containing
     `VC\Tools\MSVC\<x.y.z>\`, ccg derives the toolset and target
     architecture, locates the VS install (the cl path's own install root,
     else `vswhere`), runs `VC\Auxiliary\Build\vcvars<arch>.bat
     -vcvars_ver=<x.y.z>`, and materializes the captured `INCLUDE` exactly
     like the ccg-produced path (`/X` + `-imsvc` + compatibility version).
     The compatibility version is appended from the cl path even when the
     original command already carried `/X` — otherwise `#if _MSC_VER`
     branches silently used clang-cl's default. Only the INCLUDE
     materialization is conditional on the command not having `/X`.
     The capture is cached per (toolset, architecture) and bounded to
     10 s; it is skipped entirely for projects without cl entries.
  2. *Dialect-only* — a bare `cl.exe`/`clang-cl` with no toolset clue gets
     `--driver-mode=cl` injected (so the replay no longer silently uses
     libc++), and native `cl.exe` entries are reported as
     `system-includes: auto-detected` because their headers still come
     from clang-cl discovery.
  3. *Auto-detect fallback* — when a clue exists but the vcvars rebuild
     fails, the summary and `--json` output also say
     `system-includes: auto-detected` (JSON: `systemIncludes`). ccg never
     pretends the argv is self-contained when it is not.
- **PCH closure order under cl**: the textual closure follows the real
  driver order `/I → CPATH → resource dir → -imsvc → INCLUDE`. A
  self-contained argv (contains `/X`, `/x` or `-X` — cl's same
  IgnoreStandardIncludePath option) uses only argv directories plus the
  clang resource dir; it never reads the indexing process's
  `INCLUDE`/`CPATH` and never appends the auto-discovered toolset chain.
  This keeps `stddef.h`/`stdint.h`/`vadefs.h` resolving to the resource
  headers rather than a same-named fake in `-imsvc`.
