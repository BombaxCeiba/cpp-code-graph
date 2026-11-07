# cpp-code-graph v1 — SQLite 模式（ccg.db）

一个文件即一个数据库，由一行 `meta` 记录其版本。下面全部 DDL 在首次打开时
通过 sqlpp23 连接的调用运算符（`db(R"(...)")`）逐字执行；`:memory:` 数据库
（测试）得到完全相同的处理。

## 1. 连接 PRAGMA（每次打开时）

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

`journal_mode`/`synchronous` 在打开时设置一次；`foreign_keys` 与
`busy_timeout` 是逐连接设置，由 `GraphStore` 在每次打开时重新设置
（`busy_timeout` 30 秒 = 模式常量 `default_busy_timeout_ms`；sqlpp23 的
sqlite3 连接器自身没有超时旋钮，所以这条 PRAGMA 是唯一的设置点）。

## 2. 完整 DDL

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

## 3. 位域字典（成文契约，不是表）

`symbols.flags`：

| 位 | 含义 |
|---|---|
| 0x01 | is_struct（kind 为 record） |
| 0x02 | is_union（kind 为 record） |
| 0x04 | is_template（主模板） |
| 0x08 | is_partial_specialization（偏特化） |
| 0x10 | is_virtual（方法） |
| 0x20 | is_pure_virtual（纯虚） |
| 0x40 | is_static（方法/变量） |
| 0x80 | is_constexpr |
| 0x100 | is_const（方法 —— 同时也是 `\|c` 这个 unique_key 后缀） |
| 0x200 | external_body（符号体位于被索引根之外） |
| 0x400 | is_final（record —— C++ 的 final，证据是 CXXFinalAttr 子游标） |

`edges.attrs` 对 `extends`：位 0-1 为访问级别（0 public、1 protected、
2 private），位 2 为 is_virtual。对 `calls`：位 0-2 为派发方式
（0 直接调用、1 成员调用、2 疑似虚派发），位 3（被运算符过滤器排除的
标记）永远不会写入存储。对 `specializes`（若该探针落地）：位 0-3 存
模板实参个数。对 `uses`：位 0-2 存产生这条边的声明点类别（0 字段、
1 形参、2 返回、3 局部变量、4 变量）；当同一行上一个参数与返回类型
引用同一个 record 时，该站点折叠（occurrences 计重复次数），attrs
按遍历顺序保留第一个类别（先是返回，再是形参，最后是局部变量）。

## 4. 版本管理与迁移

当前版本：**14**。每次打开都会检查 `meta.schema_version`。

- **项目尚未发布期间不维护任何跨版本迁移。** 全新数据库一步以当前
  版本创建；`schema_version` 不匹配的既有数据库——无论偏旧、偏新还是
  缺失——都会被拒绝，并给出明确报错，建议执行 `ccg index --force`
  （该命令会重建文件）。
- 出现不兼容的 DDL 变更，或某次抽取变更会在源文件未变的情况下改动
  已存储的行时，提升版本号。发布之后，再评估对纯增量变更做自动迁移；
  在那之前重建是唯一路径。

## 5. 写入路径形态（全部经 sqlpp23，探针实证）

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

预编译语句每个 `GraphStore` 只创建一次并复用；一个 TU 批次的整个导入
在单个事务内运行（经连接执行 `BEGIN` … `COMMIT`），这也是 SQLite 为
批量装载提供的最大性能杠杆。

**语句风暴之前的批量去重（v7）。** 每个 TU 都会重新产出其 include 闭包
中的每个声明，因此原始写入流带着一个巨大的常数因子——大量完全相同的
行。阶段 A 在内存中按 `unique_key` 折叠批次：插入顺序 = 首次出现顺序
（因此 `sym_id` 与串行运行一致），列负载 = 最后一次产出（与预编译
upsert 的最后一次刷新一致）。阶段 B 按
`(src,dst,type,site_file,site_line)` 折叠：`occurrences` 求和，
`attrs`/`confidence` 先写者胜（与上面 INSERT-DO-NOTHING +
`occurrences = occurrences + ?` 的形态一致）。只有折叠后的行才碰
SQLite；在参考语料上实测，符号语句减少约 17.6 倍，边语句减少约
7.2 倍。

**失败记录（v7）。** 一个失败的 (file, digest) 条目写入一行 `failures`
（insert-do-nothing + 按键更新，与 fingerprints 如出一辙）；成功的作业
删除自己的行，摘要退役时对这些行的修剪与 `fingerprints` 完全一致。
这些行驱动 §7 中第一轮（pass-1）的跳过决策。

## 6. 体积经济性分析（100k LOC 代码库）

假设（按参考语料的形状校准）：约 300 个 TU，约 900 个不同文件（头文件 +
源文件），约 18k 个定义级符号（约 8k 个自由函数、约 4k 个方法/构造/
析构、约 1.5k 个 record、约 2.5k 个字段、约 1k 个枚举+枚举项、约 1k
个变量/别名），约 50k 个边站点（约 45k 个 calls、约 1.5k 个 extends、
约 1.5k 个 overrides、约 2k 个 alias/friend），约 15k 行 include。

被驻留（INTERNED）的内容（在 `str` 中只存一次，按 id 引用）：

| 内容 | 去重后 ≈ | 平均字节 | 原因 |
|---|---|---|---|
| 非限定名 | 12k | 18 | 跨 TU/命名空间/边重复 |
| unique_keys | 18k | 45 | 被 symbols 引用 + 内存去重 |
| 命名空间路径 | 300 | 14 | 大规模重复 |
| 文件路径 | 900 | 70 | 单项最大的收益：否则每条路径会出现在它拥有的每个符号/边/站点上 —— 20-60 倍重复 |
| include 目标 | 3k | 24 | 按包含者重复 |

字典 ≈ 34k 行 × 约 40 B 负载 + 索引开销 ≈ **2.0 MB**。

被内联（INLINED）的内容（从不驻留 —— 逐列说明理由）：

- `sig_hash`：一个 64 位整数取代每个函数类符号上的签名文本（约 13k ×
  约 45 B 的参数文本 → 各 8 B；完整的 `param_types` JSON 只在解析需要
  的地方保留，约 13k × 约 30 B）。
- `line`/`col`/`flags`/`attrs`/`occurrences`/`confidence`：定长整数；
  对整数做驻留毫无意义。
- `content_hash`：按构造方式天然每文件唯一 —— 驻留只会为每个文件多加
  一行。
- `meta.value`、`last_error`、两个 JSON 列：天然单次出现或稀疏。

行数预算与总量（单行数字含 SQLite varint 头 + 页内余隙；把它们当作
数量级预算看待，将在大型参考语料上重新实测并更新此处数字）：

| 表 | 行数 | ≈ 字节/行 | ≈ 总量 |
|---|---|---|---|
| str | 34k | 60 | 2.0 MB |
| symbols | 18k | 75（约 13k 行另有 +30 JSON） | 1.8 MB |
| edges | 50k | 50 | 2.5 MB |
| includes | 15k | 30 | 0.5 MB |
| files | 900 | 110 | 0.1 MB |
| 索引（5 个符号 + 3 个边 + 2 个 str + 1 个 include） | — | — | ~2.5 MB |
| **ccg.db 总计** | | | **~9-10 MB** |

不做驻留的反事实：文件路径内联在符号/边/站点上（约 70k 个引用 ×
70 B = 4.9 MB，对比 0.06 MB 的 id + 0.06 MB 的字典条目），签名内联
两份（符号 + 解析），总量会落在 15-17 MB 附近 —— 驻留设计节省约
40% 的字节，但二阶效应更重要：INTEGER id 让每个热查询（名字 → 符号，
符号 → 边）都变成索引整数连接而非 TEXT 扫描，且按构造去重让重建
索引绝不会重复存储任何东西。

边的经济性：每个站点（SITE）一行（而非每次观察一行），上限由代码中的
真实调用点决定；occurrences 计数器把同一站点的重复观察（每个包含某
头文件的 TU 都会重新看到其中的调用）折叠成一个整数而非一行 —— 在
头文件占比高的语料上，这就是 5 万行与 20 万多行边记录的差距。

## 7. 增量重建索引策略（按文件替换 + include 闭包）

对既有数据库执行 `ccg index` 默认为增量（INCREMENTAL）；`--full`
（或全新数据库）重新解析一切。验收性质：对任意编辑序列，
incremental(db) 与对最终树状态从零开始的完整重建索引在每个表上都
相等（计时与 meta 列豁免）；ctest 套件在 sample2 夹具上用归一化的、
剔除存储顺序 id 的比较钉死这一性质。

### 新鲜度：构建系统语义（fingerprints 表）

对每个被跟踪文件，取 stat 的 mtime/size 与每个 compile-db 条目的
命令摘要：

1. 快速路径（FAST PATH）—— stat 同一性（mtime + size）未变且命令摘要
   未变且 `parse_status = ok` → 跳过。O(stat)，不做哈希。
2. stat 变化（STAT CHANGED）—— 重新解析前先用 sha256 对照
   `files.content_hash` 确认：一次 checkout 或分支切换会改动所有
   mtime 而内容不变，重新解析整个语料要花几分钟而确认只需几秒。
   内容相同 → 跳过，刷新 stat。换句话说，仅改 mtime 的 `touch`
   永远不会触发重新解析；改动文件的内容才会。同一规则也管辖被失败
   跳过的 TU 的主文件：它的 stat 变动会用 `failures.content_hash`
   重新确认，因此 touch 保持失败跳过，只有内容编辑才强制重试。
3. 命令摘要变化（COMMAND DIGEST CHANGED）—— 无论内容如何都算过期：
   这是 compile_commands.json 的结构化差异（Release 库覆盖 Debug 库时
   即使所有哈希都匹配也必须重新解析）。摘要取自规范化后的重放 argv，
   因此 cmake 以任意条目顺序重新生成数据库不会改变任何东西。退役
   （RETIRED）摘要（命令改变、条目删除）的行在每次索引运行时都被
   修剪，所以该表始终恰好持有从零完整索引会写入的那些指纹。
4. `--paranoid` —— 跳过 stat 捷径，对每个文件做哈希；堵住保留 mtime
   的复制这条边路。没有指纹行（运行中途崩溃）或状态非 ok 同样意味着
   过期。
5. 重复 TU（主文件相同 + 摘要相同）共享一次解析。
6. 大小写漂移（CASE DRIFT）—— 某条目的主文件对不上存储的精确拼写、
   但折叠后命中它（ASCII 大小写折叠，并经
   `std::filesystem::equivalent` 确认为同一文件，因此 POSIX 上真正
   互异的大小写互异文件仍然保持互异）时复用该文件行：存储的拼写被
   刷新，本次运行会对每个被跟踪文件重新解析并重写，因为唯一键内嵌
   路径文本 —— 这正是对重新拼写后的 compile db 做全新完整索引会得到
   的状态。没有这条规则，重新拼写的数据库会复制它碰到的每个文件行
   （并经由新拼写的游标路径复制每个头文件子图）。修复前运行留下的
   折叠重复行会在下一次运行开始时被治愈（合并，最小 id 保留该行）
   —— 包括 `--full`。
7. 此前失败（PREVIOUS FAILURE，v7）—— 逐条目的 `failures` 行取代了
   旧的 "files.parse_status != ok" 重试闸门。以下情况该条目重试：没有
   行（历史 Ok 或从未尝试）、其引擎令牌、模块供给代、抽取根集合或
   记录的依赖闭包发生变化、主文件的 stat/哈希与记录的失败尝试不一致、
   某个瞬时类别的 `retry_after` 已到期，或该条目被播种 / 使用
   `--force`。否则就是失败跳过（FAILURE SKIP）：它计为失败、不产生
   解析作业、不判该文件的符号死刑、也不播种 include 闭包。记录的
   依赖闭包是合并（MERGED）而非覆盖：一次重试的失败通常算出空闭包，
   因为失败 TU 的 `includes` 行在上次失败时已被清空，所以空快照保留
   原有路径（新快照则做并集，同一路径取新 stat）。保留的路径按其当前
   stat 重新编码，因此记录随本次重试前进到当前世界，而不是永远重复
   触发；之后对某个保留头文件的修复仍会把该 TU 拉进 `reached` 并重试
   它。依赖路径按 stat 比较：仅 touch 某个保留依赖的 mtime 确实会触发
   一次重试（随后记录新 stat）。内容哈希复查只针对主文件：不改字节
   仅 touch 主文件永远不会重试。判死刑的决定是按文件（FILE）粒度 ——
   任何存在存活 Ok 条目（本轮成功的兄弟摘要，或全新的历史 Ok 条目）
   的文件都保留其符号；只有所有存活条目都失败的文件才被清除。
   `files.parse_status` 同样是整文件聚合（任一存活条目为 Ok 即为 Ok）。
   正是这条规则阻止了一条损坏的 compile-db 条目升级成全语料重新解析
   （参考语料：11 个失败 TU，其中 3 个有成功的兄弟条目，此前是 679
   个 TU / 244 秒的空转）。

### 脏集合与扩张

直接脏（Direct-dirty）= 新鲜度检查标记的主文件 + 内容发生变化的每个
被跟踪文件（头文件同样经过 stat 与哈希确认）+ 从磁盘删除的文件 +
离开 compile db 的源文件（TU 集合增量：从构建中移除、
EXCLUDE_FROM_BUILD、不再传入 compile db）。直接脏的主文件与内容脏的
头文件一样是重写（REWRITE）种子：它自己的站点行（edges、includes）
必须清除并重写，否则一次函数体编辑会在符号 upsert 刷新时留下编辑前
的行号。该集合沿记录的 `includes` 表扩张：任何传递 include 闭包碰到
脏文件的 TU 都是脏的（include 目标先按包含者目录连接解析，再做保守的
路径后缀匹配；一个目标匹配多个文件时解析到全部）。force 模式下每个
被跟踪文件都是重写种子，这正是 `--full` 等价于原地重建数据库的原因。

### 替换、覆盖与出现计数器

出现计数器数的是观察者（OBSERVERS）—— 闭包包含该站点文件的那些 TU
—— 因此一个文件的站点边只有在其 TU 覆盖完整（COMPLETE）时才被清除
并重写：反向扩张会到达它播种的一切的每个观察者，所以重写集合中的
每个文件的所有观察者都在本批里（或者它是全新文件）。未到达的文件
保持其行不动；它们的内容与观察者集合都没变，所以已存储的行已经等于
完整重建索引会写入的内容。第二波重新解析那些边引用了正被清除的符号
的 TU（悬空边重解析：否则干净的文件中的站点被重写，与完整重建索引
重新解析它们的方式完全一致；被清除的符号从解析器退役，因此不会有
新内容落到本轮要删的行上），并吸收来自 include 编辑、新增 TU 与删除
TU 的观察者集合变化。失败与磁盘缺失的主文件像被删除的条目一样播种
其前向 include 闭包：它们曾经观察过的一切都失去它们的出现增量，于是
其余观察者重新解析，计数器也不再数这个死 TU。同样的观察者退出播种
也覆盖主文件解析重数（PARSE-MULTIPLICITY）变化：出现计数器按每个
(main, command digest) 解析计一次增量，因此当主文件的存活条目数变化
时 —— 两条分叉重复条目中的一条被删除（每条幸存摘要仍然匹配，所以
没有这条规则时运行会报告全部跳过，被删的解析永远成为一个幽灵
观察者）、新增第二条分叉条目、或最后一个条目消失而另一个 TU 仍包含
该文件 —— 主文件的前向闭包重新覆盖，计数器只从存活条目集重建。纯
摘要替换（条目数不变）被排除在外：解析次数没变，主文件自身的重新
解析已覆盖其站点。整波是只读的，所以 `--dry-run` 会执行它（解析脏
集合）以报告精确计划。

删除语义：磁盘上不存在的文件、compile db 中不存在的源文件、以及不再
被任何内容 include 的头文件，由孤儿清扫（沿 includes 表从当前 TU
主文件做前向可达）抹除 —— 对同一棵树 + 数据库做完整重建索引不会创建
任何这样的行。重新解析失败的 TU 被记录为失败并清除其先前的行，与
全新索引不会产出任何行一致；它的 files 行仍记录解析时所针对字节的
content hash，恰是全新完整运行会哈希的值。解析器把重写覆盖文件的
每个运行前符号退役（阶段 A 会复活它重新 upsert 的一切），因此不会有
任何边解析到同一事务要删除的行上。

清除消歧：清除一个裸名原有多个 tier-3 候选、清除后只剩一个（或只剩
唯一主模板）的符号，可以让此前因歧义被丢弃（AMBIGUOUS-DROPPED）的
调用点变得可解析 —— 而被丢弃的站点不会留下任何可供悬空边波次跟随的
行，所以这些站点可能位于任何干净文件中。该探针看到的清除集合是完整
（FULL）覆盖清除：重写覆盖文件的过期键（包括头文件 —— 从头文件删除
一个死类与从主文件删除一样会定义 F3 类）加上被抹除/失败/缺失的文件
与父级级联的子项。当它以这种方式消除了某个名字的歧义，本次运行升级
为完整重解析覆盖（每个被跟踪文件加入重写集合，正是 `--full` 所用的
播种方式）—— 唯一一种重解析结果等同于完整重建索引的状态。

路径同一性：抽取出的路径被逐段重新拼成磁盘上的实际大小写，然后恢复
本次运行的根前缀（上文大小写漂移规则 6：被统一重新拼写的树仍会收敛
到当前拼写），因此 `-I` 拼写与文件字段大小写不一致的 compile db 对
每个物理文件只产生一个同一性 —— 唯一键内嵌路径，两种拼写会铸造出一
个连 `--full` 都无法治愈的重复子图（两种拼写总会再次出现）。大小写
敏感（Case-SENSITIVE）文件系统保持折叠后相同的互异文件互异：一个
目录持有两个仅大小写不同的条目时，两种拼写都原样保留。

删除也牵动哈希簿记：一个此前已索引、随后从磁盘消失的主文件保留其
（失败的）行但失去其 content hash —— 对那棵树做全新完整运行从未哈希
过该文件，旧哈希背后的字节也已不存在。

已知边界（已有文档记录）：来自干净文件、本可对本次运行新增（ADDED）
符号重新解析的边不会被重试 —— 未解析的被调方不入库，因此没有可重试
的对象；等价性套件的加文件用例因此新增一个独立 TU。（删除方向 ——
清除消除歧义 —— 由上面的升级机制覆盖。）

## 8. 查询支撑形态

- 分层查找（CLI.md §matching）：`symbols_name`；限定名/命名空间过滤
  本身在内存中对已解析的行进行（v6 的 `symbols_ns` 索引没有查询消费
  者，v7 中已删除）。子串层 = `str.s LIKE '%x%'` 连接到 symbols
  （str 很小）；模糊层 = 从子串层取候选，在进程内打分（大小写/分隔符
  不敏感的相似度）—— 这个规模不需要 FTS。
- callers/callees：上游用 `edges_dst`；下游用 `edges_site` UNIQUE 索引
  最左的 `src_id` 前缀（单独的 `edges_src` 索引已在 v7 删除 —— 计划器
  从未选过它），按 (src,dst) 聚合 `MIN(site_line)`、
  `SUM(occurrences)`。
- impact 广度优先搜索：对每个前沿重复 callers/callees 查询；有深度
  限制；已访问集在内存中。
- trace：用同样的邻居查询做双向广度优先搜索。
- include 环（将来的 `check`）：对 `includes` 的递归 CTE，连接
  `files`+`str`，限定为项目内部、非尖括号目标。
- stats：对预置的 kind/type 查找表做 GROUP BY 连接。
