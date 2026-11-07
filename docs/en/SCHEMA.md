# cpp-code-graph v1 — SQLite Schema (ccg.db)

One file, one database, versioned by a `meta` row. All DDL below is executed
verbatim through the sqlpp23 connection call operator (`db(R"(...)")`) at
first open; `:memory:` databases get the identical treatment (tests).

## 1. Connection PRAGMAs (every open)

```sql
PRAGMA busy_timeout = 30000;    -- per-connection; FIRST statement of every open (it
                                -- must cover the WAL pragma and the bootstrap
                                -- writes too): SQLite's factory default
                                -- is 0 - a contended write fails instantly - so
                                -- every ccg writer queues behind concurrent lock
                                -- holders (realtime coordination transactions,
                                -- engine batches, external holders) instead of
                                -- dying "database is locked"; `ccg update` raises
                                -- it further (update-busy-timeout-ms)
PRAGMA journal_mode = WAL;      -- persistent property of the file; a no-op (returns
                                -- "memory") for :memory: databases
PRAGMA synchronous = NORMAL;    -- WAL-safe compromise; index writes are batched anyway
PRAGMA foreign_keys = ON;       -- per-connection; MUST be set on every open
PRAGMA temp_store = MEMORY;
```

`journal_mode`/`synchronous` are set once at open; `foreign_keys` and
`busy_timeout` are per-connection and re-armed by `GraphStore` on every
open (`busy_timeout` 30 s = the schema constant `default_busy_timeout_ms`;
the sqlpp23 sqlite3 connector has no timeout knob of its own, so the PRAGMA
is the only arming point).

## 2. Complete DDL

```sql
-- --------------------------------------------------------------- meta ----
-- Schema/versioning + index bookkeeping. Tiny, low-cardinality, read often:
-- plain TEXT key/value, not interned (interning 20 rows would cost more
-- than it saves), WITHOUT ROWID so the table IS its primary-key index.
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;
-- Seeded rows:
--   schema_version = <current schema version (14)>  (bumped on incompatible
--                                 DDL change OR an extraction change that
--                                 alters rows for unchanged sources; see
--                                 section 4)
--   app_version    = <cg.util version string>
--   last_index_at = ISO-8601 UTC
--   project_root   = first indexed root, normalized forward-slash form
--   compile_db_path= path of the compile_commands.json that produced this db
--   llvm_path       = per-db LLVM engine config (`ccg config set
--                     llvm-path`); ABSENT = default resolution (the NULL
--                     side of the setting - $CCG_LLVM_ROOT, then PATH
--                     discovery; `ccg config unset llvm-path`)
--   llvm_engine_path= normalized path of the library that produced this db
--                     (recorded at every index run)
--   llvm_version    = version string of that engine (clang_getClangVersion
--                     at load time). Engine identity (path + version) is
--                     the third freshness pillar: an index run whose
--                     resolved engine differs from the recorded one in
--                     EITHER value force-fulls the database (worldview
--                     change; no migration involved - the keys simply did
--                     not exist before this feature, which itself counts
--                     as "unknown engine" against a non-empty db)

-- ---------------------------------------------------------------- str ----
-- THE string interning dictionary. Every repeated byte in the graph lives
-- here exactly once and is referenced by INTEGER id everywhere else.
CREATE TABLE str (
    id  INTEGER PRIMARY KEY,             -- rowid alias; ids never reused
    s   TEXT NOT NULL
);
CREATE UNIQUE INDEX str_s ON str(s);     -- upsert conflict target + exact lookup

-- ------------------------------------------------- kind lookup tables ----
-- ~16 rows total, seeded at creation. Makes SQL/JSON output self-describing
-- (stats joins, debugging) for a negligible, one-time cost.
CREATE TABLE symbol_kinds (
    kind_id INTEGER PRIMARY KEY,
    name    TEXT NOT NULL UNIQUE
);
INSERT INTO symbol_kinds (kind_id, name) VALUES
    (0, 'record'), (1, 'enum'), (2, 'enum_constant'),
    (3, 'function'), (4, 'method'), (5, 'constructor'), (6, 'destructor'),
    (7, 'field'), (8, 'variable'), (9, 'type_alias');

CREATE TABLE edge_types (
    type_id INTEGER PRIMARY KEY,
    name    TEXT NOT NULL UNIQUE
);
INSERT INTO edge_types (type_id, name) VALUES
    (0, 'calls'), (1, 'extends'), (2, 'overrides'),
    (3, 'type_alias'), (4, 'friend_of'), (5, 'specializes'),
    (6, 'uses'), (7, 'virtual_dispatch'), (8, 'callback_reg');

-- --------------------------------------------------------------- files ----
-- One row per source/header file ever touched by the indexer. The path text
-- itself is interned (str); only the id is stored here.
CREATE TABLE files (
    file_id      INTEGER PRIMARY KEY,
    path_id      INTEGER NOT NULL UNIQUE REFERENCES str(id),  -- normalized abs path
    content_hash TEXT,                     -- SHA-256 hex (64 chars) of file bytes;
                                           -- NULL until the file has been hashed
    parse_status INTEGER NOT NULL DEFAULT 0,  -- 0 pending, 1 ok, 2 partial, 3 failed
    error_count  INTEGER NOT NULL DEFAULT 0,  -- libclang diagnostics severity>=error
    last_error   TEXT,                       -- one diagnostic string, inlined
                                           -- (single occurrence by construction)
    indexed_at   TEXT                        -- ISO-8601 of the last successful pass
);
CREATE INDEX files_pending ON files(parse_status) WHERE parse_status <> 1;

-- -------------------------------------------------------- fingerprints ----
-- Build-system freshness identity (schema v2, section 7): stat first
-- (mtime + size), sha256 as tiebreaker, per-TU compile-command digest.
-- One row PER (file, command digest): a file with several compile-db
-- entries (mixed configs in one db) carries one row per entry, each
-- checked against its own entry, so the duplicates stay fresh instead of
-- fighting over a single slot. mtime_ns holds std::chrono::file_clock's
-- native tick count - an opaque, platform-stable identity (the value only
-- ever compares against rows written on the same machine). cmd_digest is
-- the sha256 of the canonicalized replay argv (compiler token dropped,
-- response files expanded, include paths absolute); '' on non-main files.
-- A missing row means unverifiable, which means stale.
CREATE TABLE fingerprints (
    file_id    INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    mtime_ns   INTEGER NOT NULL,
    size       INTEGER NOT NULL,
    cmd_digest TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (file_id, cmd_digest)
);

-- ------------------------------------------------------------ failures ----
-- PER-ENTRY failure identity (schema v7). Keyed by (file, cmd_digest), NOT
-- by file: one file may carry several compile-db entries (mixed configs)
-- and one entry failing must not poison a sibling that succeeds. The row
-- freezes the world state under which the attempt failed: main-file content
-- hash + stat, engine version token, module-feed generation (pcm stat +
-- producer compiler version + module output), extraction root set, and the
-- pre-failure include closure (`path\tmtime\tsize`, size -1 = absent).
-- The indexer retries the entry only when one of those changed (or --force
-- / an update seed names it, or a transient class's retry_after elapsed);
-- otherwise it skips without creating parse work, without dooming the
-- file's symbols and without seeding the include closure. A missing main
-- file never carries a failure row (reappearance must retry). Rows live and
-- die with their fingerprint rows.
CREATE TABLE failures (
    file_id         INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    cmd_digest      TEXT    NOT NULL,
    content_hash    TEXT    NOT NULL DEFAULT '',
    mtime_ns        INTEGER NOT NULL DEFAULT 0,
    size            INTEGER NOT NULL DEFAULT 0,
    engine_token    TEXT    NOT NULL DEFAULT '',
    feed_digest     TEXT    NOT NULL DEFAULT '',
    roots_digest    TEXT    NOT NULL DEFAULT '',
    failure_class   TEXT    NOT NULL DEFAULT '',
    failure_text    TEXT    NOT NULL DEFAULT '',
    deps_json       TEXT    NOT NULL DEFAULT '',
    attempts        INTEGER NOT NULL DEFAULT 1,
    first_failed_at TEXT    NOT NULL DEFAULT '',
    last_failed_at  TEXT    NOT NULL DEFAULT '',
    retry_after     INTEGER NOT NULL DEFAULT 0,  -- unix ms; transient only
    PRIMARY KEY (file_id, cmd_digest)
) WITHOUT ROWID;
CREATE INDEX failures_retry ON failures(retry_after) WHERE retry_after > 0;

-- ------------------------------------------------------------- symbols ----
-- Every definition-level symbol (and pure-virtual declarations). Scoping is
-- carried by COLUMNS, not edges: parent_id = enclosing record, ns_id =
-- interned namespace PATH string ('' at global scope) — no namespace nodes,
-- no HAS_METHOD/HAS_FIELD edges (DESIGN.md §4; saves one node class and one
-- edge class with zero query loss).
CREATE TABLE symbols (
    sym_id        INTEGER PRIMARY KEY,
    unique_key_id INTEGER NOT NULL UNIQUE REFERENCES str(id),
                  -- 'kind|ns|name|defining_file[|p<sig_hash>][|c]', built by the
                  -- ONE shared function in cg.extract.model. UNIQUE + UPSERT keep
                  -- sym_id STABLE across re-indexes (incremental replacement §7).
    kind_id       INTEGER NOT NULL REFERENCES symbol_kinds(kind_id),
    name_id       INTEGER NOT NULL REFERENCES str(id),   -- unqualified name
    ns_id         INTEGER NOT NULL REFERENCES str(id),   -- full namespace path ''
    sig_hash      INTEGER,          -- signed-64 reinterpretation of FNV-1a64 over
                                    -- the canonical parameter-type list. NULL for
                                    -- non-function-like kinds. Overload identity
                                    -- without storing signature text.
    file_id       INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    line          INTEGER NOT NULL, -- 1-based definition location
    col           INTEGER NOT NULL, -- 1-based
    parent_id     INTEGER REFERENCES symbols(sym_id) ON DELETE CASCADE,
                                    -- enclosing record for methods/fields/ctors/
                                    -- dtors/nested types; NULL otherwise
    flags         INTEGER NOT NULL DEFAULT 0,  -- bitfield, see §3
    param_types   TEXT,             -- JSON array of canonical type spellings;
                                    -- function-like kinds ONLY (resolution tier 1)
    template_params TEXT            -- JSON array; templates ONLY (sparse)
);
CREATE INDEX symbols_name   ON symbols(name_id);              -- query tier 1/2
CREATE INDEX symbols_file   ON symbols(file_id);              -- per-file purge
CREATE INDEX symbols_parent ON symbols(parent_id);            -- context / members

-- --------------------------------------------------------------- edges ----
-- Semantic relations between symbols. One row per DISTINCT SITE
-- UNIQUE(src,dst,type,site_file,site_line) — the reference project proved that
-- collapsing call sites into one (src,dst) row destroyed real data (7 call
-- sites of HandleError in one function collapsed to 1). 'first-seen' semantics
-- = the site columns themselves; occurrences counts how often the SAME site
-- was re-observed (other TUs including the same header, re-index passes).
CREATE TABLE edges (
    edge_id      INTEGER PRIMARY KEY,
    src_id       INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    dst_id       INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    type_id      INTEGER NOT NULL REFERENCES edge_types(type_id),
    site_file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
                 -- the file OWNING the reference site (call site, base-clause
                 -- line...). Edges are purged/re-extracted by this column.
    site_line    INTEGER NOT NULL,  -- 1-based line of the reference
    attrs        INTEGER NOT NULL DEFAULT 0,   -- bitfield, see §3
    confidence   REAL NOT NULL DEFAULT 1.0,    -- resolution tier that created
                 -- the edge: 1.0 exact / 0.7 ns-tail / 0.4 bare-name
    occurrences  INTEGER NOT NULL DEFAULT 1,   -- UPSERT ... occurrences =
                 -- occurrences + 1 (excluded) on conflict
);
CREATE UNIQUE INDEX edges_site ON edges(src_id, dst_id, type_id, site_file_id, site_line);
-- edges_src was dropped in schema v7: edges_from already uses the
-- edges_site UNIQUE index's leftmost prefix (src_id), and every edge write
-- pays for one less B-tree. edges_dst stays (callers / upstream).
CREATE INDEX edges_dst ON edges(dst_id, type_id);   -- callers / upstream (impact)
CREATE INDEX edges_own ON edges(site_file_id);      -- per-file purge
CREATE INDEX edges_dispatch ON edges(type_id) WHERE type_id = 7;

-- ------------------------------------------------------- dynamic dispatch ----
-- schema v11: derived analysis facts. Both tables are pure caches of
-- cg.analysis output and are rewritten per owning file (type_hints /
-- call_sites) or globally (virtual_dispatch edges) on every run that
-- rewrites anything. They never carry unique facts: --force rebuilds them.
CREATE TABLE type_hints (                 -- L0 fact or L2 param/return slot
    hint_id         INTEGER PRIMARY KEY,
    owner_id        INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    var_name        TEXT    NOT NULL DEFAULT '',  -- '#ret' = return slot
    file_id         INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    line            INTEGER NOT NULL,
    col             INTEGER NOT NULL,
    type_set        TEXT    NOT NULL DEFAULT '[]', -- JSON array of record unique_keys
    source          INTEGER NOT NULL,   -- HintSource (see cg.analysis.model)
    confidence      INTEGER NOT NULL,   -- 0 high / 1 medium / 2 low / 3 fallback
    fallback_reason TEXT    NOT NULL DEFAULT '',
    param_index     INTEGER NOT NULL DEFAULT -1,
    related_var     TEXT    NOT NULL DEFAULT '', -- LocalCopy source
    related_key     TEXT    NOT NULL DEFAULT '', -- ReturnFlow callee unique_key
    UNIQUE(owner_id, file_id, line, col, var_name, source)
);
CREATE INDEX type_hints_owner ON type_hints(owner_id);
CREATE INDEX type_hints_file  ON type_hints(file_id);
CREATE INDEX type_hints_owner_var ON type_hints(owner_id, var_name);

CREATE TABLE call_sites (                 -- every project call site
    site_id         INTEGER PRIMARY KEY,
    caller_id       INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    callee_id       INTEGER REFERENCES symbols(sym_id) ON DELETE SET NULL,
    callee_key      TEXT    NOT NULL DEFAULT '',
    callee_name     TEXT    NOT NULL DEFAULT '',
    callee_ns       TEXT    NOT NULL DEFAULT '',
    callee_parent   TEXT    NOT NULL DEFAULT '',
    callee_sig      INTEGER,
    callee_const    INTEGER NOT NULL DEFAULT 0,
    method_id       INTEGER REFERENCES symbols(sym_id) ON DELETE SET NULL,
    method_key      TEXT    NOT NULL DEFAULT '',
    is_virtual      INTEGER NOT NULL DEFAULT 0,
    qualified       INTEGER NOT NULL DEFAULT 0,  -- p->Base::f()/Base::f(): not dynamic dispatch
    indirect        INTEGER NOT NULL DEFAULT 0,  -- function/member pointer or dependent callee
    receiver_name   TEXT    NOT NULL DEFAULT '', -- variable, or field name when receiver_is_this
    receiver_is_this INTEGER NOT NULL DEFAULT 0, -- receiver_name is an implicit this->field
    receiver_owner_key TEXT NOT NULL DEFAULT '', -- declaring record of a field receiver
    static_type_id  INTEGER REFERENCES symbols(sym_id) ON DELETE SET NULL,
    file_id         INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    line            INTEGER NOT NULL,
    col             INTEGER NOT NULL,
    arg_types       TEXT    NOT NULL DEFAULT '[]', -- JSON array of arrays (dynamic clues only)
    arg_vars        TEXT    NOT NULL DEFAULT '[]', -- per-arg local/parameter names
    arg_call_keys   TEXT    NOT NULL DEFAULT '[]', -- per-arg @fn|... return-slot descriptors
    receiver_types  TEXT    NOT NULL DEFAULT '[]', -- static + dynamic keys
    -- resolved is DISPATCH resolution (virtual only); callee_resolved is the
    -- independent "the called symbol itself resolved" fact. Non-virtual and
    -- indirect sites therefore have callee_resolved=1/0 with resolved=0.
    resolved        INTEGER NOT NULL DEFAULT 0,
    callee_resolved INTEGER NOT NULL DEFAULT 0,
    confidence      INTEGER NOT NULL DEFAULT 3,
    candidates      TEXT    NOT NULL DEFAULT '[]', -- JSON [{target,confidence,reason}], capped at 64
    fallback_reason TEXT    NOT NULL DEFAULT '',
    -- indirect participates in the identity: a direct inner call and the
    -- outer indirect call through its result share one source position.
    UNIQUE(caller_id, file_id, line, col, callee_key, is_virtual, indirect)
);
CREATE INDEX call_sites_caller ON call_sites(caller_id);
CREATE INDEX call_sites_callee ON call_sites(callee_id);
CREATE INDEX call_sites_method ON call_sites(method_id);
CREATE INDEX call_sites_file   ON call_sites(file_id);
CREATE INDEX call_sites_open   ON call_sites(resolved) WHERE resolved = 0;

-- virtual_dispatch edges (type_id = 7): one row per (caller, concrete
-- target, site). Multiple reachable inferred targets produce multiple rows
-- at the same site — never a single guessed winner. attrs carries
-- dispatch_member|dispatch_virtual|dispatch_inferred (0x0B); family-level
-- fallbacks add dispatch_fallback (0x10) and confidence 0.0.
--
-- Unresolved virtual sites emit exactly ONE family-level fallback marker
-- (dst = the statically resolved callee), not one row per candidate; the
-- full (capped, at most 64) candidate set lives in call_sites.candidates
-- and truncation is spelled out in call_sites.fallback_reason
-- ("...;candidates_truncated"). This keeps a 3,000-member override family
-- to one edge instead of thousands.
--
-- fallback_reason vocabulary: no_dynamic_type_evidence (static-declaration
-- subtree narrowing appends ";static_narrowed"; a subtree over 256 nodes or
-- 16 levels falls back to the full family with ";subtree_overflow"),
-- family_ambiguous, indirect_call, not_virtual, qualified_call,
-- no_callee_symbol; ";candidates_truncated" and ";slot_type_truncated" (the
-- receiver slot's type set was cut by the 32-type propagation cap; consumer
-- note: on low-band multi-candidate (0.4) sites the surviving type set is
-- noise-heavy — may fan-out / family write-back unions can flood a slot
-- with near-whole-library types, so pair the marker with receiver-scope or
-- static-reachability filtering) append
-- as suffixes when applicable. Candidate reasons add static_unique_override
-- (every static receiver type is final-or-concrete and the family has
-- exactly one non-pure implementation: High 1.0, resolved=1), the
-- ";receiver_scope_narrowed" suffix (COM root lifecycle methods
-- AddRef/Release/QueryInterface under the full-family disclosure form:
-- candidates narrowed to the receiver static type's extends descendant
-- closure; members outside it are runtime-impossible targets for the site,
-- and an all-filtered set falls back to the full disclosure), and the
-- "_nearest_converged" suffix (all dynamic types resolved and their nearest
-- overrides converge to one implementation: lifted to Medium 0.7).
--
-- Indirect calls (function pointers / member pointers / dependent callees)
-- emit no dispatch edge; their call_sites row has indirect=1 and
-- fallback_reason='indirect_call'.
--
-- Read-side semantics (query commands, default precise mode): an edge with
-- confidence >= 0.7 (1.0 / 0.7) is evidence; at that (caller, site_file, site_line) the
-- proven target replaces the statically resolved family over-approximation
-- (guarded by the calls edge's virtual bit and the target's implementation
-- set). confidence = 0.4 is a weak inference below the default evidence
-- threshold: it names no dynamic target and does not replace the family
-- approximation. confidence = 0.0 is a family marker, never evidence: it
-- suppresses nothing, adds no fan-in, never participates in path finding, and
-- is shown only with --dispatch-fallback. User-visible states are
-- dispatchStatus = "inferred" | "family_estimate" | "none"; only inferred
-- entries carry dispatchConfidence. --no-dispatch is the byte-identical
-- static view (no virtual_dispatch rows at all).

-- ------------------------------------------------------- analysis_slots ----
-- schema v12: L2 fixed-point terminal state per propagation slot
-- (owner_sym, var_name) — the warm-start base for incremental analysis
-- (A/D plan). Written by the analysis commit segment (value-diffed when the
-- run went incremental, full rewrite when it degraded to the full path);
-- ver is a code-side constant (analysis_slots_ver): any row with a stale
-- ver makes the next run discard the whole table and recompute from
-- scratch. owner_sym stores the symbol unique_key TEXT (not the id): ids
-- churn when a dirty file's symbols are purged and re-created, keys don't.
-- alias_src / ret_src persist the L1/L2c propagation source slot keys the
-- last run resolved — the witness that lets the next run detect a vanished
-- or re-targeted source edge (deletion propagation follows the OLD edge
-- by design). Rows for slots whose hint rows vanished are
-- deleted by the reconcile pass on every analysis commit.
CREATE TABLE analysis_slots (
    owner_sym  TEXT    NOT NULL,
    var_name   TEXT    NOT NULL,
    type_set   TEXT    NOT NULL DEFAULT '[]', -- JSON array, canonical sorted
    confidence INTEGER NOT NULL,   -- terminal slot confidence (0..3)
    source     INTEGER NOT NULL,   -- HintSource of the terminal label
    reason     TEXT    NOT NULL DEFAULT '',
    truncated  INTEGER NOT NULL DEFAULT 0,  -- slot-level k_max_types taint
    ver        INTEGER NOT NULL,
    alias_src  TEXT    NOT NULL DEFAULT '', -- last-resolved L1 source slot key
    ret_src    TEXT    NOT NULL DEFAULT '', -- last-resolved L2c source slot key
    PRIMARY KEY (owner_sym, var_name)
) WITHOUT ROWID;

-- --------------------------------------------------- passthrough_wrappers --
-- schema v13 (R1 slot infra): registry of passthrough wrapper templates —
-- a class template C<T...> owning a zero-parameter member function whose
-- return type references an own template parameter (atomic<T>::load /
-- optional<T>::value / shared_ptr<T>::get / any custom wrapper: zero-name
-- structural judgement, canonical type-parameter- marker). Pure registry:
-- the unwrap consumer (getter call sites folding the parameter record into
-- the receiver slot) is wave-3; this table itself has zero readers.
-- Keys are weak TEXT references (call_sites.callee_key form); file_id
-- carries the per-file rewrite semantics via the purge cascade.
CREATE TABLE passthrough_wrappers (
    wrapper_key TEXT    NOT NULL,  -- primary template record unique_key
    getter_key  TEXT    NOT NULL,  -- passthrough getter function-form key
    getter_name TEXT    NOT NULL,
    file_id     INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    line        INTEGER NOT NULL,
    arg_index   INTEGER NOT NULL,  -- own template parameter index returned
    PRIMARY KEY (wrapper_key, getter_key)
) WITHOUT ROWID;

-- ------------------------------------------------------------- includes ----
-- File→file #include graph, CSG-proven separate table (files are not symbols;
-- DEFINES needs no table at all — it IS symbols.file_id). Target is stored as
-- interned RAW text as written ('foo/bar.hpp' or 'vector'); angled-ness is a
-- column, because INCLUSION_DIRECTIVE.included_file is unreliable on some
-- libclang builds (reference D.11: token-scan the directive instead).
CREATE TABLE includes (
    file_id   INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE, -- includer
    target_id INTEGER NOT NULL REFERENCES str(id),   -- interned target text
    line      INTEGER NOT NULL,
    angled    INTEGER NOT NULL,     -- 1: #include <...>   0: #include "..."
    UNIQUE (file_id, target_id, line)
);
CREATE INDEX includes_target ON includes(target_id);  -- reverse: who includes X

-- -------------------------------------------------------------- imports ----
-- File→module named-import graph (schema v5, the module twin of includes):
-- one row per `import <name>;` / `export import <name>;` directive authored
-- in a module TU's MAIN file. Extracted from CXCursor_ModuleImportDecl with
-- a main-file filter, so a row exists exactly when the build fed the BMI and
-- the import resolved (tier-2); module_name is the cursor spelling verbatim
-- ("std", "cg.util", "cg.util.json"; a foreign corpus's partition imports
-- keep their "mod:part" spelling, stored verbatim).
CREATE TABLE imports (
    file_id   INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE, -- importer
    module_id INTEGER NOT NULL REFERENCES str(id),   -- interned module name
    line      INTEGER NOT NULL,
    UNIQUE (file_id, module_id, line)
);
CREATE INDEX imports_module ON imports(module_id);   -- reverse: who imports M

-- ----------------------------------------------------- realtime coord ----
-- `ccg update` coordination (schema v6, REALTIME.md): the DATABASE is
-- the coordinator — no lock files, no PID heuristics. pending_updates is
-- the change queue (one row per normalized path; the TEXT PRIMARY KEY
-- deduplicates concurrent enqueues of the same file). update_claim is the
-- singleton exclusive claim: lease_until is unix epoch MILLISECONDS read
-- from the machine's own wall clock (std::chrono::system_clock; the skew
-- consequences are spelled out in REALTIME.md "Clock source and
-- skew"); a
-- claim whose lease has expired is stealable by the next updater, which is
-- the whole crash self-heal. Every coordination step is one SHORT
-- transaction (enqueue plain, claim/take BEGIN IMMEDIATE + busy_timeout);
-- the engine work between them runs with no coordination transaction open.
CREATE TABLE pending_updates (
    path TEXT PRIMARY KEY
) WITHOUT ROWID;
CREATE TABLE update_claim (
    id          INTEGER PRIMARY KEY CHECK (id = 1),  -- singleton row
    holder      TEXT NOT NULL,     -- diagnostic identity (pid + tick)
    lease_until INTEGER NOT NULL   -- unix epoch ms; expired = stealable
);

-- v1 adds nothing else. (No FTS table: search tiers run over str/symbols —
-- measured fine at our scale; FTS5 is a documented LATER addition.)
```

## 3. Bitfield dictionaries (documented contract, not tables)

`symbols.flags`:

| bit | meaning |
|---|---|
| 0x01 | is_struct (kind record) |
| 0x02 | is_union (kind record) |
| 0x04 | is_template (primary template) |
| 0x08 | is_partial_specialization |
| 0x10 | is_virtual (methods) |
| 0x20 | is_pure_virtual |
| 0x40 | is_static (methods/variables) |
| 0x80 | is_constexpr |
| 0x100 | is_const (methods — also the `\|c` unique_key suffix) |
| 0x200 | external_body (symbol body lives outside the indexed roots) |
| 0x400 | is_final (records — C++ final, evidence is the CXXFinalAttr child cursor) |

`edges.attrs` for `extends`: bits 0-1 access (0 public, 1 protected, 2
private), bit 2 is_virtual. For `calls`: bits 0-2 dispatch (0 direct, 1
member, 2 virtual-dispatch suspected), bit 3 operator-filtered-out never
reaches storage. For `specializes` (if the probe lands): bits 0-3 hold the
template argument count. For `uses`: bits 0-2 hold the declaration-site
flavor that produced the edge (0 field, 1 param, 2 return, 3 local,
4 variable); when a parameter and the return type reference the same record
on one line the site folds (occurrences counts the repeats) and attrs keeps
the first flavor in walk order (return, then params, then locals).

## 4. Versioning and migration

Current version: **14**. `meta.schema_version` is checked on every open.

- **No cross-version migration is maintained while the project is
  unreleased.** A fresh db is created at the current version in one step;
  an existing db whose `schema_version` does not match — older, newer, or
  missing — is refused with a clear error advising `ccg index --force`
  (which recreates the file).
- Bump the version on incompatible DDL change or an extraction change
  that alters stored rows for unchanged sources. Post-release, evaluate
  an auto-migration for purely additive changes; until then the recreate
  path is the only one.

## 5. Write-path shapes (all via sqlpp23, probe-proven)

```sql
-- intern (prepared once):  INSERT INTO str(s) VALUES(?) ON CONFLICT(s)
--                          DO NOTHING RETURNING id;   (+ in-memory cache:
--                          std::unordered_map<std::string, i64>; the map makes
--                          repeat interns free and the RETURNING covers cold ones)
-- symbol upsert:           INSERT INTO symbols(...) VALUES(...)
--                          ON CONFLICT(unique_key_id) DO UPDATE SET
--                          file_id=excluded.file_id, line=excluded.line,
--                          col=excluded.col, flags=excluded.flags, ...
--                          RETURNING sym_id;
-- edge upsert (prepared, split - a single INSERT..DO UPDATE..RETURNING
--                          cannot be prepared on the sqlpp23 pin and its
--                          per-row re-parse dominated the store phase):
--                          INSERT INTO edges(...) VALUES(...)
--                          ON CONFLICT(src_id,dst_id,type_id,site_file_id,site_line)
--                          DO NOTHING;            -- 0 affected rows = conflict
--                          SELECT edge_id FROM edges WHERE <site key>;
--                          UPDATE edges SET occurrences = occurrences + ?
--                          WHERE edge_id = ?;
-- per-file purge (re-index): DELETE FROM edges      WHERE site_file_id = ?;
--                            DELETE FROM includes   WHERE file_id = ?;
--                            DELETE FROM symbols    WHERE file_id = ?
--                              AND unique_key_id NOT IN (<fresh batch ids>);
--                            (FK ON DELETE CASCADE removes the purged symbols'
--                             incoming/outgoing edges automatically)
-- batch classify (avoid per-row SELECTs): SELECT unique_key_id FROM symbols
--                                          WHERE file_id = ?;  -- once per file
```

Prepared statements are created once per `GraphStore` and reused; the whole
import of one TU batch runs inside a single transaction (`BEGIN` … `COMMIT`
via the connection), which is also the biggest performance lever SQLite
offers for bulk loads.

**Batch dedup before the statement storm (v7).** Every TU re-emits every
declaration in its include closure, so the raw write stream carries a large
constant factor of identical rows. Stage A folds the batch by `unique_key`
in memory: insert order = first appearance (so `sym_id`s match a serial
run), column payload = last emission (identical to the prepared upsert's
last refresh). Stage B folds by `(src,dst,type,site_file,site_line)`:
`occurrences` sum, `attrs`/`confidence` first-writer-wins (identical to the
INSERT-DO-NOTHING + `occurrences = occurrences + ?` shape above). Only the
folded rows touch SQLite; measured on the reference corpus this is ~17.6x fewer
symbol statements and ~7.2x fewer edge statements.

**Failure records (v7).** A failed (file, digest) entry writes one
`failures` row (insert-do-nothing + keyed update, mirroring fingerprints);
a succeeding job deletes its row, and digest retirement prunes rows exactly
like `fingerprints`. Those rows drive the pass-1 skip decision in §7.

## 6. Size-economy analysis (100k LOC codebase)

Assumptions (calibrated against the reference corpus shape): ~300 TUs,
~900 distinct files (headers + sources), ~18k definition symbols
(~8k free functions, ~4k methods/ctors/dtors, ~1.5k records, ~2.5k fields,
~1k enums+enumerators, ~1k variables/aliases), ~50k edge sites (~45k calls,
~1.5k extends, ~1.5k overrides, ~2k alias/friend), ~15k include rows.

What is INTERNED (stored once in `str`, referenced by id):

| content | distinct ≈ | avg bytes | why |
|---|---|---|---|
| unqualified names | 12k | 18 | repeated across TUs/namespaces/edges |
| unique_keys | 18k | 45 | referenced by symbols + in-memory dedup |
| namespace paths | 300 | 14 | massively repeated |
| file paths | 900 | 70 | the single biggest win: each path otherwise appears on every symbol/edge/site it owns — 20-60x repetition |
| include targets | 3k | 24 | repeated per includer |

Dictionary ≈ 34k rows × ~40 B payload + index overhead ≈ **2.0 MB**.

What is INLINED (never interned — justified per column):

- `sig_hash`: a 64-bit integer REPLACES the signature text on every
  function-like symbol (~13k × ~45 B of param text → 8 B each; full
  `param_types` JSON kept only where resolution needs it, ~13k × ~30 B).
- `line`/`col`/`flags`/`attrs`/`occurrences`/`confidence`: fixed-size
  integers; interning integers is meaningless.
- `content_hash`: unique per file by construction — interning would only add
  a row per file.
- `meta.value`, `last_error`, the two JSON columns: single-occurrence or
  sparse-by-nature.

Row budgets and totals (per-row figures include SQLite varint header + page
slack; treat them as order-of-magnitude budgets, to be re-measured against
a large reference corpus and recorded here):

| table | rows | ≈ bytes/row | ≈ total |
|---|---|---|---|
| str | 34k | 60 | 2.0 MB |
| symbols | 18k | 75 (+30 JSON on ~13k) | 1.8 MB |
| edges | 50k | 50 | 2.5 MB |
| includes | 15k | 30 | 0.5 MB |
| files | 900 | 110 | 0.1 MB |
| indices (5 symbol + 3 edge + 2 str + 1 include) | — | — | ~2.5 MB |
| **total ccg.db** | | | **~9-10 MB** |

Non-interned counterfactual: file paths inlined on symbols/edges/sites
(~70k references × 70 B = 4.9 MB vs 0.06 MB of ids + 0.06 MB dictionary
entries) and signatures inlined twice (symbol + resolution) would land around
15-17 MB — the interning design saves ~40% of bytes, but the second-order
effect matters more: INTEGER ids make every hot query (name → symbols,
symbol → edges) an indexed-integer join instead of a TEXT scan, and dedup by
construction is what keeps re-indexing from double-storing anything.

Edge economy: one row per SITE (not per observation) bounded by real call
sites in the code; the occurrences counter folds re-observations of the same
site (every TU that includes a header re-sees its calls) into an integer
instead of a row — on header-heavy corpora this is the difference between
50k and 200k+ edge rows.

## 7. Incremental re-index policy (per-file replacement + include closure)

`ccg index` against an existing db defaults to INCREMENTAL; `--full` (or a
fresh db) re-parses everything. Acceptance property: for any sequence of
edits, incremental(db) equals a from-scratch full re-index of the final tree
state on every table (timing/meta columns exempt); the ctest suite pins this
with normalized, storage-order-id-free comparisons on the sample2 fixture.

### Freshness: build-system semantics (fingerprints table)

Per tracked file, with mtime/size from stat and the command digest per
compile-db entry:

1. FAST PATH — stat identity (mtime + size) unchanged AND command digest
   unchanged AND `parse_status = ok` → skip. O(stat), no hashing.
2. STAT CHANGED — confirm with sha256 against `files.content_hash` before
   re-parsing: a checkout or branch switch touches every mtime with
   identical content, and re-parsing the corpus costs minutes while the
   confirm costs seconds. Content same → skip, refresh the stat. In other
   words an mtime-only `touch` never triggers a re-parse; changing the
   file's CONTENT does. The same rule governs a failure-skipped TU's main
   file: its stat moving is re-confirmed by `failures.content_hash`, so a
   touch keeps the failure skip and only a content edit forces a retry.
3. COMMAND DIGEST CHANGED — stale regardless of content: this is the
   structured diff of compile_commands.json (a Release db over a Debug one
   must re-parse even though every hash matches). The digest is taken over
   the canonicalized replay argv, so cmake regenerating the db in arbitrary
   entry order changes nothing. Rows for RETIRED digests (changed command,
   dropped entry) are pruned on every index run, so the table always holds
   exactly the fingerprints a from-scratch full index would write.
4. `--paranoid` — skip the stat shortcut and hash every file; closes the
   mtime-preserving-copy edge. No fingerprint row (crashed run) or non-ok
   status also means stale.
5. Duplicate TUs (identical main file + identical digest) share one parse.
6. CASE DRIFT — an entry whose main file misses the exact stored spelling
   but folds onto it (ASCII case-fold, confirmed the same file via
   `std::filesystem::equivalent`, so genuinely distinct case-differing
   files on POSIX stay distinct) reuses the file row: the stored spelling
   is refreshed and the run re-parses and rewrites every tracked file,
   because unique keys embed the path text — the state a fresh full index
   of the re-spelled compile db produces. Without this a re-spelled db
   duplicated every file row it touched (and, through the new spellings'
   cursor paths, every header subgraph). Folded-duplicate rows left by a
   pre-fix run are healed (merged, lowest id keeps the row) at the start of
   the next run — including `--full`.
7. PREVIOUS FAILURE (v7) — a per-entry `failures` row replaces the old
   "files.parse_status != ok" retry gate. The entry retries when there is
   no row (historical Ok or never attempted), when its engine token, module
   feed generation, extraction roots or recorded dependency closure changed,
   when the main file's stat/hash differs from the recorded failed attempt,
   when a transient class's `retry_after` has elapsed, or when the entry is
   seeded / `--force`. Otherwise it is a FAILURE SKIP: it counts as failed,
   produces no parse job, does not doom the file's symbols, and does not
   seed the include closure. The recorded dependency closure is MERGED,
   never overwritten: a retried failure usually computes an empty closure
   because the failed TU's `includes` rows were cleared on the previous
   failure, so an empty snapshot keeps the prior paths (and a fresh
   snapshot is unioned, same path taking the fresh stat). Retained paths
   are re-encoded from their current stat, so the record advances to the
   world of this retry instead of re-triggering forever; a later fix to a
   retained header still pulls the TU into `reached` and retries it.
   Dependency paths are compared by stat: an mtime-only touch of a retained
   dependency DOES trigger one retry (the new stat is then recorded).
   Content-hash rechecking applies to the MAIN file only: touching a main
   file without changing its bytes never retries it.
   The doom decision is per FILE — a file with
   any live Ok entry (a successful sibling digest this run, or a fresh
   historical-Ok entry) keeps its symbols; only a file whose every live
   entry failed is purged. `files.parse_status` is likewise a whole-file
   aggregate (Ok if any live entry is Ok). This is what stops one broken
   compile-db entry from escalating into a corpus-wide re-parse (reference
   corpus: 11 failed TUs, 3 of them with a successful sibling, previously
   679 TUs / 244 s of no-op work).

### Dirty set and expansion

Direct-dirty = mains the freshness check flags + every tracked file whose
content changed (headers are statted and hash-confirmed the same way) +
files deleted from disk + source files that left the compile db (TU-set
delta: removed from the build, EXCLUDE_FROM_BUILD, a compile db no longer
passed). A directly-dirty main is a REWRITE seed exactly like a
content-dirty header: its own site rows (edges, includes) must purge and
rewrite, or a body edit leaves pre-edit line numbers behind while the
symbol upserts refresh. The set expands over the recorded `includes` table:
any TU whose transitive include closure hits a dirty file is dirty (include
targets resolve by includer-directory join, then conservative path-suffix
match; a target matching several files resolves to all of them). In force
mode every tracked file is a rewrite seed, which is what makes `--full`
equivalent to rebuilding the db in place.

### Replacement, coverage, and occurrence counters

Occurrence counters count OBSERVERS — TUs whose closure contains the site's
file — so a file's site edges are purged and rewritten only when its TU
coverage is COMPLETE: the backward expansion reaches every observer of
everything it seeds, so every file in the rewrite set has all its observers
in the batch (or is brand new). Unreached files keep their rows untouched;
their content and observer set are unchanged, so the stored rows already
equal what a full re-index would write. A second wave re-parses TUs whose
edges referenced symbols being purged (dangling-edge re-resolution: sites in
otherwise clean files are rewritten exactly as a full re-index re-resolves
them; purged symbols are retired from the resolver so nothing new lands on
a row that dies this run) and pulls in observer-set changes from include
edits, new TUs, and dropped TUs. Failed and disk-missing mains seed their
forward include closure exactly like dropped entries: everything they used
to observe loses their occurrence bump, so the remaining observers re-parse
and the counters stop counting the dead TU. The same observer-exit seeding
covers a PARSE-MULTIPLICITY change of a main: occurrence counters count one
bump per (main, command digest) parse, so when a main's live entry count
changes — one of two divergent duplicate entries dropped (every surviving
digest still matches, so without this the run reports skipped-all and the
dropped parse stays a phantom observer forever), a second divergent entry
added, or the last entry gone while another TU still includes the file —
the main's forward closure re-covers and the counters rebuild from the live
entry set only. A pure digest replacement (same entry count) is excluded:
the parse count is unchanged and the main's own re-parse already covers its
sites. The whole wave is read-only, so
`--dry-run` executes it (parsing the dirty set) to report the exact plan.

Deletion semantics: files absent from disk, sources absent from the compile
db, and headers nothing includes anymore are erased by an orphan sweep
(forward reachability from the current TU mains over the includes table) —
a full re-index of the same tree + db would create none of those rows. A TU
that fails to re-parse is recorded failed and its previous rows are purged,
matching a fresh index that would produce none; its files row still records
the content hash of the bytes the parse was attempted against, exactly what
a fresh full run would hash. The resolver retires every pre-run symbol of
rewrite-covered files (phase A revives whatever it re-upserts), so no edge
ever resolves onto a row the same transaction deletes.

Purge-lifted ambiguity: purging a symbol whose bare name had several
tier-3 candidates and is left with exactly one (or with a sole primary
template) can make previously AMBIGUOUS-DROPPED call sites resolvable —
and a dropped site leaves no row a dangling-edge wave could follow, so the
sites may sit in any clean file. The purge set this probe sees is the FULL
coverage purge: stale keys of rewrite-covered files (headers included — a
dead class removed from a header defines the F3 class just as a main does)
plus erased/failed/missing files and parent-cascade children. When it lifts
a name this way
the run escalates to full re-parse coverage (every tracked file joins the
rewrite set, exactly the seeding `--full` uses), the only state whose
re-resolution equals a full re-index's.

Path identity: extracted paths are re-spelled to each segment's on-disk
case and then the run's root prefix is restored (case-drift rule 6 above:
a UNIFORMLY re-spelled tree still converges to the current spelling), so a
compile db whose `-I` spellings disagree in case with its file fields yields
one identity per physical file — unique keys embed the path, and two
spellings would mint a duplicated subgraph that not even `--full` could
heal (both spellings always reappear). Case-SENSITIVE filesystems keep
distinct same-fold files distinct: a directory holding two entries that
differ only in case leaves both spellings verbatim.

Deletion also touches the hash bookkeeping: a previously-indexed main that
goes missing from disk keeps its (failed) row but loses its content hash —
a fresh full run of that tree never hashed the file, and the bytes behind
the old hash no longer exist.

Known boundary (documented): edges from clean files that could NEWLY resolve
against symbols ADDED this run are not retried — unresolved callees are not
stored, so there is nothing to retry against; the equivalence suite's
add-file case therefore adds an independent TU. (The REMOVAL direction —
a purge lifting an ambiguity — is covered by the escalation above.)

## 8. Query support shapes

- Tier lookup (CLI.md §matching): `symbols_name`; the qualified/namespace
  filtering itself runs in memory over the resolved rows (the v6
  `symbols_ns` index had no query consumer and was dropped in v7).
  Substring tier = `str.s LIKE '%x%'` joined to symbols (str is small);
  fuzzy tier = candidates from the substring tier scored in-process
  (case/separator-insensitive similarity) — no FTS needed at this scale.
- callers/callees: `edges_dst` for upstream; downstream uses the
  `edges_site` UNIQUE index's leftmost `src_id` prefix (the separate
  `edges_src` index was dropped in v7 — it was never chosen by the
  planner), aggregated `MIN(site_line)`, `SUM(occurrences)` per (src,dst).
- impact BFS: repeated callers/callees queries per frontier; depth-limited;
  visited-set in memory.
- trace: bidirectional BFS over the same neighbor queries.
- include cycles (LATER `check`): recursive CTE over `includes` joined to
  `files`+`str` restricted to project-internal, non-angled targets.
- stats: GROUP BY joins against the seeded kind/type lookup tables.
