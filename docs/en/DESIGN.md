# cpp-code-graph v1 — Architecture and Design

cpp-code-graph (executable `ccg.exe`) parses a C++ codebase through libclang,
extracts a semantic graph (symbols + edges), stores it economically in SQLite,
and answers code-intelligence queries from the command line. v1 targets a
reference corpus of similar scale (about 660 TUs, same clang64 toolchain)
with full-fidelity parsing and storage economy as first-class requirements.

Scope of v1: index, query, context, callers, callees, impact, trace, stats.
Schema v11 adds static virtual-dispatch inference (`cg.analysis`): interface
pointer/reference calls are resolved to concrete implementations when local
type evidence supports it, and downgraded honestly (candidate set +
confidence + fallback) when it does not. Query commands use that evidence by
default (precise mode): confidence >= 0.7 evidence (1.0 / 0.7) replaces the
override-family over-approximation at the call site; 0.4 weak inference and
0.0 markers are not evidence, and results without proven evidence are
labelled family estimates. `--no-dispatch` is the byte-identical static
upper-bound view.
Out of scope (recorded for later, do not build): FTS/process/community
overlays, PDG/taint, detect-changes, incremental watch mode, MCP/serve.

## 1. Module DAG and layering

100% named modules, C++23, `import std;` everywhere. Every project source file
is a module unit with extension `.ixx`. Third-party headers appear ONLY inside
global module fragments (GMF), each owned by exactly one module (single-owner
rule, see §10). A header that contains only macros and no declarations is not
bound by this rule: macros cannot be exported across modules, so every unit
that uses one includes it in its own global module fragment without creating
duplicate declarations; the name-tag macro header
`sqlpp23/core/name/create_name_tag.h` currently appears in the global module
fragments of the four `cg.db.tables` submodules and of `cg.msbuild.tables`.
Module interfaces stay small; the bulk lives in small
dotted-name submodules under each umbrella (former partitions; the
dotted-name module refactor dissolved the partition syntax).

```
                 app.main  (src/app/main.ixx, export main)
                     |
                  cg.cli  (src/cli/cli.ixx)
                 /     \
           cg.graph    cg.db* (sqlpp23/sqlite3 imported directly, see below)
          /   |    \
   cg.extract cg.db cg.util
        |
    cg.clang  (libclang only via cg.clang)
        |
    cg.util   (yyjson only via cg.util.json)
```
Strict layering: a module may import only modules in layers strictly below it
in the table. No cycles; the build system enforces it de facto because module
BMIs encode the import graph.

The two sqlpp23 modules (`sqlpp23.core`, `sqlpp23.sqlite3`) have no wrapper;
the units that use them import them directly: in the database layer the four
`cg.db.tables` submodules, `cg.db.store` and the five `cg.db.query`
submodules; in the `cg.msbuild` layer `cg.msbuild.tables`, `cg.msbuild.wrap`
and `cg.msbuild.pipeline.wraplog`; plus `cg.analysis.external` (one delete
statement on the connection). `sqlite3.h` is included in the global module
fragments of `cg.db.store`, `cg.msbuild.wrap` and
`cg.msbuild.pipeline.wraplog` (and of the schema-mirror test unit).

| Module | File(s) | Imports (project) | GMF third-party includes | Responsibility |
|---|---|---|---|---|
| `cg.util` | `src/util/util.ixx` (+ `cg.util.json` = `src/util/util.json.ixx`, `cg.util.platform` = `src/util/util.platform.ixx`, re-exported by the umbrella) | — | `cpp_yyjson.hpp` (in `cg.util.json` only, with the assert shim of §10.2); platform headers (`io.h`/`windows.h` or `unistd.h`, in `cg.util.platform` only) | version string; FNV-1a 64; qualified-name split/join; path normalization (forward slashes, lowercase drive); `PathCaseCache` + on-disk case re-spelling; in-memory `Interner` cache; minimal JSON read/write facade over yyjson-cpp; `cg.util.platform` console-vs-pipe stdin probe (the one place OS APIs may appear) |
| `cg.db` | `src/db/db.ixx` + `cg.db.schema` (`db.schema.ixx`), `cg.db.store` (`db.store.ixx`), `cg.db.query` (`db.query.ixx`) | — | `sqlite3.h` | SQLite via sqlpp23: open/PRAGMAs/DDL, string interning table, upserts (files/symbols/edges/includes/imports), per-file purge, all lookups and traversal-shaped queries, stats. Knows nothing of libclang |
| `cg.db.tables` | `src/db/db.tables.ixx` (entry point; declares nothing itself and only re-exports the four submodules below; not re-exported by the umbrella `cg.db`) + `cg.db.tables.core` (`db.tables.core.ixx`), `cg.db.tables.graph` (`db.tables.graph.ixx`), `cg.db.tables.sites` (`db.tables.sites.ixx`), `cg.db.tables.analysis` (`db.tables.analysis.ixx`) | — | `sqlpp23/core/name/create_name_tag.h` (macro-only) | sqlpp23 table models (namespace `cg::db::tab`), hand-written shape, tables do not reference each other: `core` holds metadata and strings, the kind/edge-type dictionaries, a read-only model of the sqlite catalog table, file registry, fingerprints, failure entries and realtime coordination, plus every alias tag used for aggregates and joins; `graph` holds the symbols/edges/includes/imports tables; `sites` holds type hints and call sites; `analysis` holds propagation slots, passthrough wrappers, external parameter args and external invokes, and symbol contributions. Units that name a table model `import cg.db.tables;` themselves |
| `cg.clang` | `src/clang/clang.ixx` + `cg.clang.loader`, `cg.clang.capi` (`clang.capi.ixx`), `cg.clang.driver`, `cg.clang.tu` (`clang.tu.ixx`), `cg.clang.compile_db` (`clang.compile_db.ixx`), `cg.clang.bmi` | `cg.util` | `clang-c/Index.h` (in `cg.clang.capi` only) | `cg.clang.capi` re-exports CX types + RAII (`Str`, `Index`, `Tu`) + thin cursor facade (kind/spelling/location/children/…); `cg.clang.tu` parse + diagnostics; `cg.clang.compile_db` reads compile_commands.json, tokenizes command strings, builds per-TU argv |
| `cg.extract` | `src/extract/extract.ixx` + `cg.extract.model` (`extract.model.ixx`), `cg.extract.visitor` (`extract.visitor.ixx`) | `cg.clang`, `cg.util` | — | `cg.extract.model` plain record types (`SymbolRec`, `EdgeRec`, `IncludeRec`, `ImportRec`, `TuExtract`) + THE `unique_key` builder (single source of truth, §4); `cg.extract.visitor` walks one TU cursor tree and produces records (named-module imports from `CXCursor_ModuleImportDecl`, main-file filter). Pure: no DB, no I/O |
| `cg.analysis` | `src/analysis/analysis.ixx` + `cg.analysis.model` (`analysis.model.ixx`), `cg.analysis.collect` (`analysis.collect.ixx`), `cg.analysis.points_to` (`analysis.points_to.ixx`), `cg.analysis.propagate` (`analysis.propagate.ixx`), `cg.analysis.dispatch` (`analysis.dispatch.ixx`) | `cg.clang`, `cg.db`, `cg.util` | — | Dynamic-dispatch analysis (schema v11). `model`: pure data + confidence ladder + the byte-identical `unique_key` builders (analysis cannot import `cg.extract`); `collect`: a second directed AST walk producing `TuAnalysis` (L0 clues: declaration, `new T`, aggregates, `make_unique/shared`, casts, `&obj`, arguments; pure data, no cursors leave the worker); `points_to`: the `TypeOracle` seam (stub today, whole-program pointer/alias analysis replaces only this module); `propagate`: L1 intra-function alias/return flow + L2 global worklist fixed point over persisted hints/call sites (bounded, deterministic); `dispatch`: override-family ∩ dynamic type set → `virtual_dispatch` edges + candidate set + honest fallback. **Dependency fence (design ruling): analysis → db/clang only; graph/cli → analysis; extract/db/clang must never import analysis** (enforced by `tests/portability_scan.cmake`) |
| `cg.graph` | `src/graph/graph.ixx` + `cg.graph.build` (`graph.build.ixx`), `cg.graph.traverse` (`graph.traverse.ixx`) | `cg.extract`, `cg.analysis`, `cg.db`, `cg.util` | — | `cg.graph.build` batch import (two-phase: symbols first, then edge resolution cascade), true-incremental indexing (fingerprint stale check, include-closure dirty expansion, dangling-edge re-resolution, coverage-complete per-file replacement, module-BMI feed validation with named failure classes — SCHEMA.md §7); `cg.graph.traverse` BFS/shortest-path algorithms for callers/callees/impact/trace, change-set propagation (changes), include-cycle check (SCC over the resolved include graph; the IncludeResolver lives here and `cg.graph.build` imports the module so both sides share one resolution rule set) |
| `cg.cli` | `src/cli/cli.ixx` (umbrella: run_main, re-exports the whole family) + `cg.cli.parse` (`cli.parse.ixx`), `cg.cli.render` (`cli.render.ixx`), `cg.cli.runners` (`cli.runners.ixx`, shared spine: engine prep, command-list resolution, index tail, store-open and module-unit scan plumbing), `cg.cli.commands` (`cli.commands.ixx`: query runners + dispatch, which imports the four runner modules - the fan-in point), and the runner modules `cg.cli.index`/`cg.cli.update`/`cg.cli.msbuild`/`cg.cli.config` (each owns its entry function and its private helpers inline) | `cg.graph`, `cg.db`, `cg.util`, (`cg.clang`, `cg.msbuild` in the runner/spine modules only) | — | `cg.cli.parse` argv → `Command` variant + usage/exit-2 errors; `cg.cli.render` text and JSON output shapes (JSON via `cg.util.json`); `cg.cli.runners` the shared spine; `cg.cli.commands` query runners + dispatch; the four runner modules own their entries inline (former implementation units - all interfaces since the dotted-name module refactor retired the BMI-less split). Layering is acyclic: commands → runner modules → runners → parse/render; the umbrella re-exports everything so `import cg.cli;` is unchanged |
| `app.main` | `src/app/main.ixx` | `cg.cli` | — | `export int main()` (proven pattern; clang's `-Wmain-attached-to-named-module` is benign noise). Top-level try/catch, exit-code mapping |
| `cg.test.harness` | `tests/test_harness.ixx` | `cg.util` | — | tiny assert/count harness (`CG_CHECK`, `cg::test::run(name, fn)` registry); no external test framework is in the allowed third-party set |

Split rule: when any single `.ixx` would exceed ~300 lines, split it into a new
dotted-name submodule (`cg.util` -> `cg.util.json`); the umbrella interface
file re-exports submodules with `export import cg.util.json;` and declares
nothing bulky itself. (Historically these were module partitions; the
dotted-name module refactor dissolved the partition syntax into plain
dotted-name modules, so the umbrella `import cg.util;` surface is unchanged.)

### CMake target structure

- `cmake/CcgUtils.cmake` — helpers (module file-set adder, module-target
  finalizer, DLL copy, test registration; code in §8.5).
- Object library `cg_core` — owns ONE `PUBLIC FILE_SET CXX_MODULES` file set
  containing every `src/**/*.ixx`, `SYSTEM` off, links the third-party set.
  Object libraries with PUBLIC CXX_MODULES consumed by executables are a
  proven shape on this machine (sqlpp23's own `sqlpp23::sqlite3_module` is
  exactly this). If that ever misbehaves, the documented fallback is
  per-executable file sets listing the needed `.ixx` files directly (a shape
  likewise validated end-to-end).
- Executables `ccg`, `cg_test_util`, `cg_test_db`, `cg_test_clang`,
  `cg_test_extract`, `cg_test_graph`, `cg_test_cli` — each links `cg_core`
  and (for tests) `test_harness.ixx` from the tests file set.

## 2. Data flow

```
compile_commands.json
        |  cg.clang.compile_db  (JSON via cg.util.json; tokenize "command",
        |                       keep argv[1:], drop "-c" and "-o <path>")
        v
 per-TU: cg.clang.tu parse(argv, CXTranslationUnit_DetailedPreprocessingRecord)
        |                       (+ diagnostics, severity >= error collected)
        v
 cg.extract.visitor        one TuExtract = {symbols, edges, includes, errors}
        |                       (records carry structured callee metadata,
        |                        unique_key built by the ONE shared function)
        v
 cg.graph.build            freshness: stat (mtime+size) fast path, sha256
        |                       confirm on stat change, per-entry command
        |                       digest (SCHEMA.md §7); dirty-set expansion
        |                       through the includes graph; parallel
        |                       parse+extract of dirty TUs (worker pool)
        |                  phase A: upsert ALL symbols of the whole batch
        |                       (stable sym_id via ON CONFLICT unique_key),
        |                       build in-memory name index
        |                  phase B: resolve + upsert edges (3-tier cascade, §5),
        |                       upsert includes, per-file purge for re-index
        |                       (coverage-complete files only)
        v
 SQLite (cg.db)  --- cg.graph.traverse <--- cg.cli.commands <--- render
```
Extraction is a pure function `(argv) -> TuExtract`; only `cg.graph.build`
talks to the DB. That seam is what keeps a future multi-threaded index
(§7) and unit tests cheap.

## 3. Error handling

- Errors are exceptions internally (`std::runtime_error` subtypes; no custom
  exception hierarchy in v1 — boring beats clever). Module interfaces are
  exception-neutral; nothing catches-and-swallows across layers.
- `app.main` catches `std::exception` last: prints `ccg: error: <what>` to
  stderr, exit 1. Usage errors (unknown command/flag, missing operand) are
  raised by `cg.cli.parse` as `UsageError`, printed with the usage text,
  exit 2.
- Recoverable per-TU failures never abort an index run: a TU that fails to
  parse is recorded `parse_status = failed` with `error_count` and
  `last_error` in `files`, its (possibly partial) extraction is discarded,
  and the run continues; the summary reports failed TUs; the run still exits
  0 (the DB is usable). This mirrors the reference projects' behavior.
- CLI honesty contract: a payload with
  a non-empty `error` or `partial:true` exits 1; `truncated:true` alone exits
  0 (counts/risk are still computed over the full set); listing caps never
  cap the reported counts.

## 4. Symbol identity and resolution (extract + graph.build)

- `unique_key` (format measured on reference corpora, byte-identical on both
  node and edge side because ONE function in `cg.extract.model` builds it):
  `kind|namespace|name|defining_file_path[|p<sig_hash>][|c]`
  (`|c` marks const methods; the param suffix only for function-like kinds).
  It is interned in the `str` table; `symbols.unique_key_id` is UNIQUE, so
  sym_id is stable across re-indexes — per-file replacement depends on it.
- Two-phase import (§2) exists because TU A's call target lives in TU B;
  resolving against a complete symbol index removes TU ordering sensitivity.
- CALLS resolution cascade (3 tiers, measured on reference corpora;
  `confidence` on the edge records which tier won: 1.0 / 0.7 / 0.4):
  1. name + parent class + `sig_hash` exact (overload-correct);
  2. name + parent-class with namespace-TAIL exact match (never substring —
     `Base` must not match `Database`);
  3. bare unique name in the project allowlist.
  Unresolved after tier 3 → edge dropped, counted in the index summary
  (`unresolved edges: N`). Nothing unresolved is stored in v1.
- Call-edge hygiene (measured on the reference projects): skip `operator` overload calls,
  compiler-internal prefixes (`_M_`, `__`, `_X`), stdlib container
  methods; gate callee by project-path allowlist (symbols whose defining file
  is under an indexed root).
- Caller resolution: enclosing function via `semantic_parent` chain first,
  smallest-enclosing line-range map second (MEMBER_REF_EXPR's chain can skip
  the method — measured on the reference projects). The line-range map is
  built per TU during the same walk.
- Definitions-only nodes (`is_definition()`), except pure-virtual methods
  (declaration-only by nature) — prevents .h decl + .cpp def double nodes.
  Cross-TU duplication (every TU re-parses shared headers) collapses in the
  per-batch in-memory dedup by unique_key, then again in the UPSERT.
- Namespace is NOT a node kind (a deliberate deviation from the reference
  projects):
  symbols carry `ns_id` (interned full namespace path). Namespace nodes would
 add a node class and membership edges for zero v1 query value; namespace-tail
  matching in tier 2 works on the interned string. Record membership is
  likewise NOT an edge: `symbols.parent_id` (an economy twist measured on
  the reference projects).
  Edges mean semantic relations (calls/extends/overrides/alias/friend), not
  scoping.

## 5. Edge semantics

| type | src → dst | notes |
|---|---|---|
| `calls` | caller fn → callee fn | CALL_EXPR.referenced PLUS MEMBER_REF_EXPR.referenced (filtered to function/method kinds; closes the iface/proxy gap). One row per distinct site (`UNIQUE(src,dst,type,site_file,site_line)`; evidence from reference corpora: collapsing sites lost 7/7 call sites in one function); `occurrences` counts re-observations of the same site across TUs |
| `extends` | derived record → base record | from CXXBaseSpecifier children; attrs bits: access (public/protected/private), is_virtual |
| `overrides` | method → base virtual method | via virtual-signature match along extends chains; destructor name mapping `~D`→`~B` |
| `type_alias` | typedef/alias → target record | target often external → tier-3 style resolve, drop if unresolved |
| `friend_of` | friend fn/class → granting record | |
| `specializes` | explicit/partial specialization → primary template | only if the §6 probe proves clean; otherwise not emitted in v1 |
| `uses` | declaring symbol → referenced record | declared-type references: the field's own type, a function's parameter/return types, and locals/namespace-scope variables in its body (the function owns those). Canonicalized: cv/ref/pointer/array stripped, typedefs resolved, specializations folded onto the primary template, template arguments walked (std::vector<Widget> → Widget). External records (std, system headers) create no edge; incomplete project types defer by name. attrs bits 0-2: site flavor (field/param/return/local/variable). Powers record-level impact (§5 note in CLI.md) |

File relationships are NOT edges (files are not symbols): DEFINES is implied
by `symbols.file_id`; includes live in their own tiny `includes` table
(a shape measured on reference corpora) which also powers `check`-style cycle queries later.

## 6. Template instantiation strategy

Chosen: **option 1 — primary templates only** (a policy validated on an
earlier code-analysis project, which shipped exactly this and deleted its
instantiation extractor as dead code):

- The template's own cursor IS the node; `flags.is_template` plus
  `template_params` (JSON, from TEMPLATE_TYPE_PARAMETER children);
  partial specializations flagged `is_partial_spec`.
- Implicit specializations (`Foo<int>`) produce no cursors in a libclang
  walk — synthesizing nodes from TYPE_REF spellings (option 2) is proven
  fruitless by the reference project; a full instantiation graph (option 4)
  violates storage economy. Both rejected.
- Guarded probe (during M3): measure `clang_getSpecializedCursorTemplate` on
  the reference corpus; if it resolves cleanly for explicit instantiations and
  partial specializations, emit `specializes` edges with the argument count
  as an edge attr — still zero synthesized nodes. If noisy or slow: drop it
  and record the deviation. Rationale: zero-risk declaration fidelity now,
  one cheap C-API call for the only instantiation relation
  libclang can actually see.

## 7. Threading stance

Indexing parses in parallel, stores serially:

- a worker pool (`std::jthread`, `--jobs N`, default auto =
  `min(cores-2, 12)`) runs the pure per-TU extraction; each worker owns its
  own `CXIndex` and `CXTranslationUnit` lifecycle (libclang: neither may
  cross threads), and every `CXString` is materialized into `std::string`
  on the owning thread inside `cg.clang`/`cg.extract`, so only plain
  `TuExtract` data leaves a worker;
- jobs are handed out longest-first (main-file size) — the corpus
  distribution is heavy-tailed and the biggest TU must start first;
- all DB writes happen on the calling thread after the pool joins, in
  compile-db order, inside the single batched transaction — the stored rows
  are identical to a serial run for any worker count (a ctest pins this);
- SQLite is compiled threadsafe=1 (verified) and opened WAL; a future
  multi-reader query path needs no schema change.

## 8. Third-party wiring (3rdParty/CMakeLists.txt)

Reference-project function style (per-package `_cpm_add_*` / `_config_*` functions,
STATUS messages, static minimal configs, pins), CPM form. The top-level
CMakeLists does, in order: `cmake_minimum_required(3.28)` → the
version-conditional `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` gate (§10.1) →
`project(cpp-code-graph LANGUAGES C CXX)` (C needed by yyjson) → C++23 globals
→ CPM bootstrap (cache at `.cpm-cache`, already warm) → `add_subdirectory(3rdParty)`
→ `add_subdirectory(src)` → `include(CTest)` + tests.

```cmake
# 3rdParty/CMakeLists.txt — sketch; the full proven recipes live in the
# committed 3rdParty/CMakeLists.txt itself
set(GITHUB_MIRROR_URL "https://github.com" CACHE STRING "GitHub prefix override")

# System libs resolve through the ADAPTIVE discovery policy (section 8.6):
# compiler-derived root -> standard find_* search -> CCG_*_ROOT override
# (which wins when set) -> FATAL_ERROR naming the override. Sketch:

function(_config_sqlite3)               # system sqlite3 STATIC (user decision)
    # Raw file name beats find_package(SQLite3): the find module resolves
    # libsqlite3.dll.a on this MinGW-style toolchain (probe-measured).
    set(CCG_SQLITE3_ROOT "" CACHE PATH "sqlite3 prefix override")
    set(CG_SQLITE3_ROOT "" CACHE PATH "deprecated alias of CCG_SQLITE3_ROOT")
    # override set -> search ONLY under it; else CCG_TOOLCHAIN_ROOT (derived
    # from CMAKE_CXX_COMPILER) with NO_DEFAULT_PATH, else plain find_*.
    # Still not found -> FATAL_ERROR naming CCG_SQLITE3_ROOT.
    # sqlpp23's own find_package(SQLite3) picks these cache entries up:
    set(SQLite3_INCLUDE_DIR "${SQLITE3_INCLUDE_DIR}" CACHE PATH "" FORCE)
    set(SQLite3_LIBRARY "${SQLITE3_STATIC_LIBRARY}" CACHE FILEPATH "" FORCE)
    add_library(cg_sqlite3 INTERFACE); add_library(cg::sqlite3 ALIAS cg_sqlite3)
    target_include_directories(cg_sqlite3 INTERFACE "${SQLITE3_INCLUDE_DIR}")
    target_link_libraries(cg_sqlite3 INTERFACE "${SQLITE3_STATIC_LIBRARY}")
endfunction()

function(_config_libclang)              # system libclang via import lib (NOT libclang.a!)
    set(CCG_LLVM_ROOT "" CACHE PATH "LLVM/libclang prefix override")
    set(CG_LIBCLANG_ROOT "" CACHE PATH "deprecated alias of CCG_LLVM_ROOT")
    # same chain; import lib candidates libclang.dll.a (MSYS2) / libclang.lib
    # (LLVM layout); CCG_LIBCLANG_BIN (bin dir of the found prefix) feeds the
    # test PATH prepend and the opt-in DLL-copy closure.
    add_library(cg_libclang INTERFACE); add_library(cg::libclang ALIAS cg_libclang)
    target_include_directories(cg_libclang INTERFACE "${LIBCLANG_INCLUDE_DIR}")
    target_link_libraries(cg_libclang INTERFACE "${LIBCLANG_IMPLIB}")
endfunction()

# CPM packages — call order matters (pfr target must exist before cpp_yyjson
# validates CPPYYJSON_PFR_TARGET). Pins and OPTIONS verbatim below
# (all proven GREEN on this machine):
#   pfr        boostorg/pfr            boost-1.92.0                       -> Boost::pfr
#   yyjson     ibireme/yyjson          1f934d2aaee3e2d016fc1f271b6a01388581dcbf (static, tests/doc/misc/fuzzer OFF)
#   cpp_yyjson BombaxCeiba/yyjson-cpp  2ed9d20e485a5040e64165b8990ed0682dce458c (GIT_SUBMODULES "",
#              CPPYYJSON_PFR_TARGET=Boost::pfr, CPPYYJSON_USE_STD_FORMAT=ON, test/bench OFF)
#   sqlpp23    rbock/sqlpp23           7e8c9a7fde5a7bcc6a1cf694c29a4e9a886ffeb6
#              (BUILD_SQLITE3_CONNECTOR=ON, BUILD_WITH_MODULES=ON -> import sqlpp23.core / sqlpp23.sqlite3)
# All: GIT_SHALLOW TRUE, SYSTEM YES, EXCLUDE_FROM_ALL YES, URLs "${GITHUB_MIRROR_URL}/<org>/<repo>.git"

_config_sqlite3()
_config_libclang()
_cpm_add_pfr()
_cpm_add_yyjson()
_cpm_add_yyjson_cpp()
_cpm_add_sqlpp23()
```
Consumption rules (single-owner, GMF include sites):

- `cg_core` links `cg::sqlite3`, `cg::libclang`, `sqlpp23::sqlite3_module`
  (drags `sqlpp23::core_module`), `cpp_yyjson::cpp_yyjson`.
- sqlpp23 is imported DIRECTLY (`import sqlpp23.core; import sqlpp23.sqlite3;`)
  — proven on this toolchain; no wrapper .ixx needed. Raw DDL/PRAGMAs go
  through the connection call operator `db(R"(...)")`; hand-written schema
  structs use the macro-only header in the GMF (exactly sqlpp23's own
  module-test pattern). Gotchas that are LAW (probe-hit): `std::int64_t` not
  `int64_t`; select results stored non-const; `order_by(col.asc())`.
- sqlite3.h is included in the global module fragments of three units,
  `cg.db.store`, `cg.msbuild.wrap` and `cg.msbuild.pipeline.wraplog` (and of
  the schema-mirror test unit). Macros do not cross module boundaries, so a
  unit that uses the SQLITE_OPEN_* flag macros has to include it itself;
  `cg.db.store` additionally calls sqlite3 C API functions directly (binding
  and running prepared statements, statement tracing), `cg.msbuild.wrap`
  additionally runs raw SQL statements and calls `sqlite3_busy_timeout`, and
  `cg.msbuild.pipeline.wraplog` only calls `sqlite3_busy_timeout`. The only
  flags used under src are `SQLITE_OPEN_READWRITE` and
  `SQLITE_OPEN_CREATE`.
- yyjson-cpp is reachable only through `cg.util.json`'s facade.
- libclang is reachable only through `cg.clang.capi` exports.
- SQLite 3.54.0 amalgamation is a DOCUMENTED upgrade path only — do not
  wire it in v1.

### 8.5 cmake/CcgUtils.cmake

```cmake
function(ccg_add_modules target base_dir)          # append .ixx files as one PUBLIC module file set
    target_sources(${target} PUBLIC FILE_SET CXX_MODULES
                   BASE_DIRS ${base_dir} FILES ${ARGN})
endfunction()

function(ccg_module_target target)                 # C++23 + import std, once per module-owning target
    set_target_properties(${target} PROPERTIES CXX_MODULE_STD ON)
    target_compile_features(${target} PUBLIC cxx_std_23)
endfunction()

# Toolchain derivation (section 8.6 step 1) — computed at include time:
get_filename_component(CCG_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(CCG_TOOLCHAIN_ROOT "${CCG_TOOLCHAIN_BIN}" DIRECTORY)

function(ccg_add_test name)                        # test exe: links cg_core + harness, PATH for runtime DLLs
    add_executable(${name} ${ARGN} tests/test_harness.ixx)
    ccg_add_modules(${name} "${CMAKE_SOURCE_DIR}" ${ARGN} tests/test_harness.ixx)
    # ... link cg_core; then prepend the DERIVED bin dirs (libclang bin +
    # compiler bin, normally the same directory):
    #   ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CCG_LIBCLANG_BIN};..."
endfunction()

option(CCG_COPY_RUNTIME_DLLS "Copy toolchain runtime DLLs beside built executables" OFF)
# ccg_toolchain_dlls(out) globs the closure from the resolved LLVM prefix
# (libclang.dll / libc++.dll by name, libLLVM-* / libxml2-* / libffi-* /
# libiconv-* by pattern; missing entries skip with a STATUS line).
# ccg_copy_dlls_after_build(target dll...) — proven POST_BUILD copy_if_different loop,
# used only when CCG_COPY_RUNTIME_DLLS=ON.
```
(Exact bodies live in the repo; the shapes above are the proven ones.)

### 8.6 Toolchain portability (discovery policy)

No build input or shipped binary contains a machine-absolute toolchain path
(pinned by the `ccg_portability_scan` ctest). Every toolchain location is
resolved through the same four-step policy:

1. **Derive from the compiler actually used.** Configure time:
   `get_filename_component(... ${CMAKE_CXX_COMPILER} DIRECTORY)` twice —
   the parent of the compiler's `bin/` is the toolchain root
   (`CCG_TOOLCHAIN_ROOT` in cmake/CcgUtils.cmake). This works for any
   MSYS2-style prefix (clang64/ucrt64/mingw64) and standard LLVM layouts;
   CMake resolves a PATH-named compiler (`"CMAKE_CXX_COMPILER": "clang++"`
   in CMakePresets.json) to its full path during `project()`. Run time:
   the directory of the loaded `libclang.dll` (Windows module API; the
   `windows.h` include lives in the global module fragment of
   `src/clang/clang.driver.ixx`), then a PATH search.
2. **Standard search fallback.** Configure time: plain `find_path`/
   `find_library`/`find_file` (no `NO_DEFAULT_PATH`) when step 1 gave no
   hit. Run time: first `clang++`/`clang` on `PATH`.
3. **Overrides win.** Configure time: cache variables `CCG_LLVM_ROOT`,
   `CCG_SQLITE3_ROOT` (empty default; when set ONLY they are searched —
   no silent fallback to a different installation; `CG_LIBCLANG_ROOT` /
   `CG_SQLITE3_ROOT` keep working as deprecated aliases). Run time: env
   `CCG_LLVM_ROOT` or `ccg index --llvm-root DIR` — these locate the
   DRIVER, never raw include paths.
4. **Loud failure.** Still unresolved → actionable error naming the
   override variables (configure: `message(FATAL_ERROR)`; run time: a
   thrown error naming `CCG_LLVM_ROOT` / `--llvm-root`). Never a hardcoded
   absolute path, never silent success with a wrong guess.

Run-time include knowledge comes from **interrogating the driver**, not
from literals: `clang++ -print-resource-dir` for the resource dir, and the
stderr block between `#include <...> search starts here:` / `End of search
list.` of `clang++ -E -v -x c++ -` (and `-x c`) for the default include
search order of THIS installation. That reported list feeds
`fallback_argv()` (verbatim, in reported order) and the `driver_supplement`
implicit-include injection (minus the resource-include entry, which the
`-resource-dir` flag carries). The result is cached for the process
lifetime.

**Machine-local compiler escape hatch.** CMakePresets.json resolves the
compilers by PATH name. A machine that needs an explicit pin creates its
own `CMakeUserPresets.json` next to the project root (never committed;
CMake merges it automatically) overriding just the compiler cache
variables:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "clang-dev-local",
      "inherits": "dev",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "C:/opt/LLVM/bin/clang.exe",
        "CMAKE_CXX_COMPILER": "C:/opt/LLVM/bin/clang++.exe"
      }
    }
  ]
}
```
`cmake --preset clang-dev-local` then configures with that toolchain; the
configure-time discovery follows it automatically (step 1 derives the root
from the pinned compiler), and `CCG_LLVM_ROOT`/`CCG_SQLITE3_ROOT` remain
available when headers/libs live elsewhere.

### 8.7 AddressSanitizer switch (CCG_USE_ASAN, cmake/CcgAsan.cmake)

The sanitizer is a reference-style build option (the use-ASAN idiom), never
a preset flag; no flag is ever written into a cache `CMAKE_*_FLAGS` entry
or the toolchain. One three-state cache STRING is resolved once at
configure time:

| `CCG_USE_ASAN` | meaning |
|---|---|
| `''` (default) | AUTO — enabled iff one of `CCG_ASAN_CONFIGS` is being built. Single-config: `CMAKE_BUILD_TYPE` compared case-insensitively; multi-config: any of the generator's configuration set. |
| `ON` | force enabled (still only for the `CCG_ASAN_CONFIGS` configurations). |
| `OFF` | force disabled (the `clang-dev` preset pins this: the fast iteration tree stays sanitizer-free; `cmake --preset clang-dev -DCCG_USE_ASAN=ON` overrides the pin). |

`CCG_ASAN_CONFIGS` (default `Debug`) is the instrumented set and the
documented escape hatch: `cmake -G "Ninja Multi-Config" -DCCG_ASAN_CONFIGS="Debug;Release"`
also instruments Release; a forced `CCG_USE_ASAN=ON` alone never widens the
set (a multi-config ON tree still instruments only Debug unless the list is
extended, and a single-config ON tree whose build type is not listed gets a
loud WARNING instead of a silently uninstrumented "sanitized" build).

Compile-flag transport: the
flags are APPENDED to the per-config `CMAKE_CXX_FLAGS_<CONFIG>` variables
for exactly the `CCG_ASAN_CONFIGS` entries while the switch is active —
the configure-time scope keeps them out of every cache entry, preset and
toolchain. They originally rode `$<$<CONFIG:Debug>:...>` genexes on the
project targets, but CMake 4.4's synthesized per-consumer module-BMI
targets (`<provider>@synth_N`, built for `std.ixx` and the sqlpp23 module
interfaces consumed across directories) capture the consuming target's
`COMPILE_OPTIONS` in a configuration-less context where the genex
mis-evaluates TRUE, leaking `/fsanitize=address` onto 6 synthesized BMI
rules in the Release/RelWithDebInfo ninja files (measured). The generator expands the flag VARIABLES per
configuration even inside synthesized rules, restoring the guarantee:

```cmake
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer")
target_link_options(t   PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)  # clang executables
```

so in a multi-config tree (Ninja Multi-Config) ONLY the listed
configurations are instrumented and Release artifacts contain no `__asan`
symbols. Scope side effect of the variable transport: every C++ target of
the tree is compiled with the flags (third-party C++ included; C targets
like the sqlite/yyjson amalgamations are untouched) — deeper coverage than
the old project-targets-only scope, and those module BMIs were already
instrumented through the synthesized-rule leak. `ccg_module_target()`
(cmake/CcgUtils.cmake) still calls `ccg_asan_target()` for every project
target for the LINK side: clang executables get the sanitizer at link time
plus the runtime-DLL copy below; MSVC executables need nothing per target.
Presets carry no sanitizer flags at all: the `clang-dev-asan`
preset pins no switch and lets AUTO instrument the Debug configuration of
its Ninja Multi-Config tree (`cmake --build --preset clang-dev-asan` builds
that configuration); `clang-dev` pins `CCG_USE_ASAN=OFF` (a switch state,
not a flag — flags come only from the option logic above).

Runtime DLL: instrumented executables link the ASAN runtime dynamically
(`libclang_rt.asan_dynamic-x86_64.dll` on this ABI). It is discovered
through the section 8.6 policy with the compiler first: the compiler's bin
dir (MSYS2-style prefixes ship it there), then
`lib/clang/<version>/lib/windows|bin` under the compiler prefix, then the
`CCG_LLVM_ROOT` override prefix as an ADDITIONAL root — the override can
never shadow the compiler's own runtime, because the runtime ABI must match
the compiler that instruments the code. A missing DLL is a WARNING, never
fatal (PATH remains the dev fallback). When found, it is copied POST_BUILD
beside every instrumented executable (same `copy_if_different` mechanism as
`ccg_copy_dlls_after_build`, config-guarded with the same `$<CONFIG:...>`
genex + `COMMAND_EXPAND_LISTS`, so building a clean configuration of a
multi-config tree copies nothing).

**Known third-party interaction (justified mitigation, probe-measured).**
Under ASAN, libclang's SEH crash recovery intercepts the ASAN
runtime's benign first-chance access violations (its lazy shadow-page
commits) and misreports them as `CXError_Crashed` ("libclang: crash detected
during parsing") — an artifact of the uninstrumented `libclang.dll` running
inside an instrumented process, not a project memory error (no ASAN report
is ever produced; cdb shows the swallowed exception is the ASAN runtime's
own shadow write, and the same parse succeeds with recovery off).
Instrumented test runs therefore set
`LIBCLANG_DISABLE_CRASH_RECOVERY=set:1` via the test
`ENVIRONMENT_MODIFICATION` (cmake/CcgUtils.cmake `ccg_test_path_mods`,
applied only while `CCG_USE_ASAN` is active); ASAN's own exception handler
still processes those AVs.

**Generator limitation (probe-measured).** The multi-config case
proven for this project is `Ninja Multi-Config`, matching the Ninja-family
constraint of the `import std` modules build. A plain Visual Studio
generator probe (`cmake -G "Visual Studio 17 2022"`) fails on this machine
before any module question, with
`Generator "Visual Studio 17 2022" could not find any instance of Visual
Studio.` — no VS instance is installed; the Ninja family remains the only
supported generator family here regardless.

### 8.8 clang-tidy gate (`tidy` target, cmake/CcgTidy.cmake)

The check set lives in `.clang-tidy` at the source root, adapted from a peer
project's configuration (the only adaptation: `HeaderFilterRegex` matches this
tree's `cpp-code-graph/(src|tests)/` instead of that project's source dirs —
third-party and run-history/build trees never match). One command runs it over every project
module unit (`src/**/*.ixx`, `tests/*.ixx`) through the build tree's
`compile_commands.json`:

```
cmake --build --preset clang-dev --target tidy
```
The target first rebuilds every module-owning target (`cg_core`, `ccg`, the
test executables — tracked by `ccg_module_target` as the `CCG_MODULE_TARGETS`
global property) because clang-tidy reads the BMIs those targets produce when
it parses the module units; the runner (cmake/ccg-tidy.cmake) then invokes one
clang-tidy with `-p <build dir>` and FAILS the target on any project-code
finding (clang-tidy itself exits 0 for plain warnings), with a per-check
summary. clang-tidy 23 parses named-module units natively in this mode: the
compile database's per-unit `@<unit>.modmap` response files carry the
`-x c++-module` + `-fmodule-file=` flags, so `import std;`, project modules
and sqlpp23 module BMIs all resolve (`-extra-arg=-Wno-unused-command-line-
argument` suppresses the driver noise `-fmodule-output` makes in syntax-only
mode). Dependency re-analysis is deduplicated by the runner.

Binary discovery follows the section 8.6 policy in the ASAN-runtime variant:
the compiler's own bin dir first, default search, then the `CCG_LLVM_ROOT`
override's bin as an additional hint — the compiler prefix always wins
because BMIs are version-locked (a clang-tidy from another LLVM build cannot
read this compiler's .pcm files); pre-seeding `CCG_CLANG_TIDY_EXECUTABLE`
skips the search. Runs are SERIAL by design: this toolchain's clang-tidy
(23.1.3) has no native `-j` worker option (probed: rejected with "Did you
mean '-h'?"), and the shipped `run-clang-tidy` wrapper would add a python
dependency to the gate.

## 9. DLL runtime strategy and the per-db LLVM engine

Proven facts: any real C++ exe needs exactly
`libc++.dll` (1.55 MB, the msys2 clang64 bin dir) beyond OS components;
sqlite3 is linked STATIC (zero sqlite DLL imports — verified at the
import-table level). **ccg.exe does not import libclang at all**
(design ruling): the LLVM engine — the libclang whose PARSING produced
a database — is a per-db worldview and is RUNTIME-LOADED (cg.clang.loader:
`LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)` on Windows /
`dlopen(RTLD_NOW|RTLD_LOCAL)` on POSIX) through the clang-c dispatch table
in cg.clang.capi (zero direct clang-c calls anywhere else; the import
table of every exe verifies the absence).

- Engine resolution order: the db's `llvm-path` config (`ccg config set
  llvm-path <prefix-or-libclang-file>`), then `--llvm-root`, then
  `$CCG_LLVM_ROOT`, then default discovery — the clang driver on PATH
  names the toolchain, whose root provides `libclang.dll`/`libclang.so`
  (beside the driver and under `bin/` or `lib/`; PATH entries may carry
  a trailing separator or dot). A configured root that resolves
  to nothing, or a library that fails to load, is an actionable error
  naming the knobs — never a silent fallback to another engine.
  `--llvm-root` is a SESSION override that applies only when the db pins
  no engine: a db's `llvm-path` always outranks the flag, and a flag
  that loses to the pin is announced with a note (a habit flag must not
  silently re-worldview a deliberately pinned db).
- The supplement driver follows the engine (one worldview = one
  installation): driver resolution anchors on the LOADED library's
  location first — its directory, then the installation prefix for the
  `bin/` and `lib/` layouts — and only then falls back to
  `$CCG_LLVM_ROOT` and PATH (fallbacks that exist for engine-only
  layouts, e.g. a lone `libclang.dll` with no driver beside it). The
  `--llvm-root` override steers the driver only when the flag actually
  selected the engine; the supplement never silently describes a
  different installation than the one parsing. Two ordering invariants
  keep that true on every path (audited): the engine is
  resolved and loaded BEFORE the first driver consumer in the run —
  including the self-build's per-TU baseline vectors, which are built
  from the pinned/flag-selected engine's own driver — and the
  driver cache in cg.clang.driver is keyed to
  `loaded_clang_library_path()`, so an engine switch at rest (one
  process indexing several dbs, e.g. `ccg msbuild --all`) re-resolves
  the driver instead of serving the previous engine's supplement.
- Engine identity sits alongside the argv digest and the TU-set delta as
  the THIRD PILLAR of index freshness: every index run records the
  resolved library path + the loaded engine's version string in meta
  (`llvm_engine_path`, `llvm_version`); a run whose resolved engine
  differs in EITHER value force-fulls (worldview change). A pre-feature
  db has no recorded identity — unknown engine — so the first run after
  the upgrade re-parses once, then records.
- Lifecycle: the first `cg.clang::Index` (or the explicit CLI load)
  resolves + loads; the handle stays mapped for the PROCESS lifetime —
  deliberately never freed, because libclang's static initializers
  install process-global crash-recovery state (a vectored SEH handler on
  Windows) that FreeLibrary/dlclose does not retract: a measured
  load/unload/reload cycle segfaults the next parse with crash recovery
  enabled and is green only with `LIBCLANG_DISABLE_CRASH_RECOVERY=1`.
  Switching engines mid-process is refused while Index objects are alive;
  a switch at rest keeps the previous handle mapped (its handler entry
  keeps pointing into mapped code, so multiplicity is harmless).
- Strategy for the REST of the runtime: **PATH for dev, opt-in copy for
  packaging.**
  - Dev builds, ctest, and workflow shells rely on the toolchain bin dir
    (e.g. the msys2 clang64 bin dir) already being on PATH (verified in the
    workflow environment). Test registration additionally prepends it via
    `ENVIRONMENT_MODIFICATION` so ctest is robust regardless of the
    invoking shell — this also feeds default engine discovery.
  - `CCG_COPY_RUNTIME_DLLS=ON` (default OFF) enables the POST_BUILD
    `copy_if_different` of ccg's OWN runtime (`libc++.dll` and friends)
    beside each exe for a shippable, standalone `ccg.exe` — never on by
    default. The former ~194 MiB libclang.dll + libLLVM-* closure is
    deliberately NOT copied anymore: the engine is resolved per database,
    never through files beside the executable.
  - The ASAN runtime DLL is the one standing exception (section 8.7):
    whenever `CCG_USE_ASAN` instruments a build, the ~0.6 MiB
    `libclang_rt.asan_dynamic-x86_64.dll` is always copied beside every
    instrumented executable (config-guarded), because without it the
    loader kills the process at startup before PATH-based lookup can help.
  - sqlite3 stays static in both modes (user decision + economy).
  - Escape hatch kept in the docs, not wired: `-static-libstdc++`
    collapses libc++ into the exe if a fully self-contained artifact is
    ever wanted.

### Crash dump guard (process-wide)

The first line of `main` (before any ccg initialization) reads
`CCG_CRASH_DUMP_DIR` once: only a non-empty value arms the full crash
callback set — zero config means zero behavior change; with the variable
unset, crash behavior is bit-for-bit identical to an uninstrumented
build. Arming includes: dbghelp preloading (loading it at crash time
crosses the loader lock with a possibly corrupt heap), a resident dump
thread, `SetUnhandledExceptionFilter` (the primary interception layer),
`signal(SIGABRT)` plus `std::set_terminate` (the only exit of the abort
family and the terminate double-peg), and `SetThreadStackGuarantee` on
the main thread; MSVC additionally hooks the pure-virtual and UCRT
invalid-parameter handlers (the latter defaults to fail-fast when not
hooked). The crash-shape matrix passes on both builds, MSVC and clang64
(the LLVM-mingw toolchain produces a plain Win32 binary with no POSIX
emulation layer) — see the ccg_crashdump_matrix gate under tests/.
The matrix coverage includes the MSVC Debug configuration: the Debug
CRT's `abort()` raises a modal message box by default, so the installer
must clear both the `_CALL_REPORTFAULT` and `_WRITE_ABORT_MSG` behavior
bits for abort to reach the SIGABRT handler silently; for the same
reason the invalid-parameter validation first emits an `_CRT_ASSERT`
report that defaults to a modal box and must be rerouted to a
non-modal channel.

- **Why a dedicated dump thread**: at stack overflow the crashing thread
  is left with a gutted stack — probes measured that writing the dump
  in place on the crashing thread (including the dbghelp-preload and
  stack-guarantee-reserve rescues) left nothing but 0-byte files five
  runs in a row; switching to "the crashing thread signals one event, a
  resident thread with an intact stack writes the dump" turned that
  into ten-for-ten — the same architecture as CrashPad/Breakpad. The
  crashing thread's handler is pared down to a few system calls (an
  atomic gate + `SetEvent`); a 30-second ack ceiling prevents zombie
  processes if the write ever wedges; concurrent crashes converge on
  the atomic gate — a multi-thread same-second crash produces exactly
  one dump (measured across a ten-run multiav race). The process then
  terminates via `TerminateProcess` with the original exception code,
  so exit-code semantics match the uninstrumented build.
- **Interceptable surface (honestly bounded, never oversold)**: caught —
  access violations, stack overflow, heap corruption (0xC0000374),
  uncaught C++ exceptions (0xE06D7363), abort, pure virtual calls, UCRT
  invalid parameter (requires the handler; without it the shape belongs
  to fail-fast); not caught — the fail-fast family (`__fastfail` /
  `RaiseFailFastException`, MSVC stack-cookie check failures), which by
  Windows design bypass every user-mode callback (measured: even a
  vectored exception handler cannot see them), exit codes
  0xC0000409/0xC0000602. Exit-code note: most shapes terminate with the
  original system exception code, but pure virtual calls and UCRT
  invalid-parameter have no system exception code of their own — those
  two terminate with distinguishable application-defined codes,
  0xE0000002 (pure virtual) / 0xE0000003 (invalid parameter).
- **Boundary with the existing recovery layers**: first-chance hardware
  faults swallowed inside cg.util.guard's run_guarded frames never reach
  the process-wide filter (no false dumps); stack overflow is not on
  that whitelist and propagates through (correctly dumped); libclang's
  vectored recovery layer swallows exceptions only inside PARSE contexts
  and passes them through outside — zero conflict.
- **Complement to system-level WER**: WER LocalDumps takes the kernel
  path and can capture the fail-fast family (the part user mode cannot);
  this mechanism captures everything else without depending on machine
  configuration. The two coexist without conflict: this mechanism
  terminates with the original exception code after writing the dump —
  the exception has been handled, so WER never engages for the same
  crash.
- **Crash-time write discipline**: handlers and the write path use only
  system APIs plus static/stack buffers (the CRT heap may already be
  corrupted by the heap-corruption shape; std::string/malloc are
  banned); the dump file is named `ccg-<pid>-<yyyymmdd>-<hhmmss>.dmp`
  (pid + second-resolution timestamp keeps concurrent processes from
  colliding); the directory is created level by level; a write failure
  reports GetLastError on stderr.
- **ASAN builds**: the ASAN interceptor reports and terminates before
  this mechanism ever sees the fault — under ASAN this mechanism most
  likely collects nothing; ASAN's own report is the diagnostic material.

## 10. Rulings and deviations

1. **CMake is 4.4.4, not 4.2.1** (probe-measured). The import-std gate UUID
   must be chosen version-conditionally BEFORE `project()`:

   ```cmake
   if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.3)
       set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
   else()
       set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
   endif()
   ```

   The `(experimental)` configure warning that follows is expected noise.
   Per-target `CXX_MODULE_STD ON` + explicit `CMAKE_CXX_STANDARD 23` +
   `CMAKE_CXX_STANDARD_REQUIRED ON` remain mandatory.

2. **assert shim in `cg.util.json`**: `cpp_yyjson.hpp` uses `assert` ~70x
   without including `<cassert>`, and `import std;` cannot export macros. In
   the GMF of `util.json.ixx`, before the include, put

   ```cpp
   #ifndef assert
   #define assert(expr) ((void)0)   // no-op, matches NDEBUG builds
   #endif
   ```

   Third-party asserts stay disabled (release-equivalent behavior); NO
   `#include <assert.h>` anywhere in project code.

3. **Test fixtures are data, not sources.** Files under `tests/fixtures/**`
   are libclang parse INPUTS (classic `#include`-based C++ so they parse
   without `-fmodules`), never compiled by our build, never imported. The
   all-`.ixx` rule applies to code compiled into the project. Fixtures are
   committed as ordinary data files.

4. **`export int main()` inside `app.main`** is the proven pattern for a
   module-only executable (benign `-Wmain-attached-to-named-module`).

5. **No git, no writes outside the project tree**; `make`, if ever needed, is
   the msys2 usr/bin `make.exe` (still not on Git Bash PATH — measured twice).

6. **Libclang parse baseline** (per-TU baseline when a TU has no usable
   entry): `-std=c++23` plus the include search list the clang driver
   reports for this installation (`-E -v` stderr interrogation,
   section 8.6; discovered and cached at run time — never a baked path);
   resource dir from `-print-resource-dir`, added to replays only when a
   tree needs it. Primary strategy is verbatim compile_commands replay.
   This baseline is NOT a directory fallback scan: `ccg index` requires a
   compile database and errors out when none exists (C++ has no
   meaningful simple fallback; a wrong graph is worse than a missing
   one). The baseline only serves units routed away from replay in the
   self-build (e.g. cl-route module units) and the PCH shadow path.

7. **Per-file unique extraction is CLOSED — never plan it again.** A hybrid
   "light count pass over every TU, then extract symbols/edges once per
   unique file" was measured on 14 real TUs from four projects (same parse
   options, `DetailedPreprocessingRecord | KeepGoing`, 3 warm repeats +
   1 cold pass) and rejected. Measured cost of a full extraction simulation
   relative to `parse + extract`: 5.0% and 6.7% on two real reference
   corpora, 8.1% on ccg's own tree. The light pass itself costs 28% of that extraction warm
   and 33% cold while producing no graph data, so the best-case net saving
   is ≈ 0.64×E, i.e. 3–5% of index wall — paid for by changing
   `occurrences` semantics, whose correctness under conditional compilation
   cannot be proven. Parse is 92–95% of wall; effort belongs there (PCH
   support), not here. Module-heavy repos (parse served from BMIs, extraction
   52% of wall) are the one exception measured, and they do not justify the
   semantic risk either. If extraction work is ever revisited, the only
   admissible form is a `(file, offset)` cache for USR / spelling /
   type-spelling lookups that leaves occurrence counting untouched.

## 11. Testing strategy

- One test executable per layer (`cg_test_*`), each a module-only exe with
  `export int main()` driving `cg.test.harness`. ctest via presets.
- Pure layers (util, db with `:memory:`, extract on fixtures, graph on seeded
  stores, cli parse/render) are unit-tested directly; the end-to-end path is
  tested by ctest `add_test` invocations of the real `ccg.exe` (argument
  lists, `PASS_REGULAR_EXPRESSION`, `WILL_FAIL` for exit-1 cases).
- Fixture mini-project (M3, classic C++ data files): a small header/source
  tree exercising every v1 node kind and edge type, including a cross-file
  call requiring tier-1 resolution, an overload pair, a virtual override, a
  template + partial specialization, and an include cycle kept in a SEPARATE
  fixture directory so normal fixtures stay acyclic.
- The reference corpus (read-only!) is the M3/M4 scale smoke: index a subset
  (e.g. one directory's TUs) manually; full ~660-TU indexing is an optional
  soak, never a CI gate.

## 12. Storage economy principles (full detail in SCHEMA.md)

Every repeated byte is interned (`str` table): names, qualified keys,
namespaces, file paths, include targets. Structured facts live in typed
columns, never JSON (JSON only for genuinely sparse per-kind payloads:
`param_types`, `template_params`). Enums are tiny INTEGER ids backed by
seeded lookup tables. Scoping relations (membership, namespace) are columns,
not edges. One edge row per distinct call site, deduplicated across TUs by
UNIQUE + occurrence counter. Row-count and byte budget for a 100k-LOC
codebase: see SCHEMA.md §6.
