# cpp-code-graph — Realtime updates (`ccg update` + editor hooks)

This document explains the one-shot realtime refresh machinery: the
`ccg update` command semantics, the database-coordinated queue/claim
protocol, and how to wire an editor or agent (Claude Code is used as the
worked example) so the index tracks every save — with **no daemon ever
running** and **no lock files anywhere**.

Command reference: `CLI.md` section *ccg update*.

## 1. Why one-shot and why the db is the coordinator

Every refresh is a short-lived process doing a bounded amount of work
(usually one or two TUs). Daemons add a second lifecycle to debug, a
socket/pipe protocol to version, and a stale-state failure mode. File
locks add another failure mode of their own: a crashed holder leaves a
stale marker that only a human deletes. Instead:

- The **event stream is the queue**: whoever notices a file change runs
  `ccg update <file>`.
- The **database itself is the coordination substrate** (schema v6):
  the `pending_updates` table holds the change queue, and the
  `update_claim` singleton row (holder + lease expiry) is the exclusive
  claim. SQLite's transaction atomicity replaces every custom lock
  primitive.
- A run that loses the claim race does not wait: its files are already
  queued (the enqueue is its own committed transaction) and the process
  exits 0 immediately. The holder drains the queue until empty, so the
  work still happens — milliseconds later, in the process that won.

## 2. Protocol

```
ccg update a.cpp b.hpp
```

1. **ENQUEUE** — one short transaction inserts the own (normalized,
   deduplicated) file paths into `pending_updates`
   (`INSERT .. DO NOTHING`; the table's `path` PRIMARY KEY folds a
   concurrent enqueue of the same file). The connection's SQLite
   `busy_timeout` is armed (`update-busy-timeout-ms`, default 600000),
   so a concurrent long index batch holding the write lock is waited
   out, never failed — and because the editor hook spawns the update
   detached, nothing user-visible ever blocks.
2. **CLAIM** — one short `BEGIN IMMEDIATE` transaction reads the
   `update_claim` row:
   - **No row, the lease expired, or the holder process is gone** → this
     process writes itself in as holder with
     `lease_until = now + update-lease-ms` (default 300000) and proceeds
     to step 3. An expired lease being stolen IS the crash self-heal: no
     stale-lock files. The holder token embeds its pid, so a claim whose
     holder was killed before its destructor could run (for example a
     host killing an over-time hook) is taken over immediately instead
     of stalling the queue until the lease runs out. The pid probe only
     ever says "dead" when that is certain; tokens without a pid fall
     back to pure lease timing.
   - **A live claim exists** → exit 0 immediately. The work is covered:
     the holder's loop drains the queue until empty (this process's
     enqueue included), or a later event converges it.
3. **HOLDER LOOP** — repeat:
   1. one short `BEGIN IMMEDIATE` transaction: refresh the lease
      (heartbeat) **and** `PEEK` the whole queue (`SELECT` all paths,
      **no `DELETE`**). If the heartbeat matches no row, the claim was
      stolen (own lease expired mid-run): stop, the thief owns the queue.
   2. record a **batch snapshot**: for every queued path that the
      database/compile database tracks, the file's current content hash
      (missing/unreadable = empty hash). Untracked paths need no hash.
   3. process the peeked files **outside any coordination transaction**
      through the ordinary incremental engine
      (`IndexOptions::seed_files`): each seeded file is forced into the
      dirty set — a seeded main re-parses, a seeded header is
      content-dirty and every TU whose include closure reaches it
      re-parses. The engine's own batch transactions are untouched.
   4. **clear covered** (one short `BEGIN IMMEDIATE` transaction):
      delete a queue row only when it is untracked (not in the database
      and not in the compile commands — nothing will ever refresh it),
      or when it is in this batch's coverage set **and** its current
      content hash still equals the batch snapshot hash **and** the
      index's own recorded `files.content_hash` is absent or equals that
      same hash. The last condition covers the edit→revert residual:
      bytes back to the pre-batch state make snapshot and current hash
      agree again while the engine may have recorded the intermediate
      state. An absent index hash (file purged mid-batch, or never
      hashed) is not evidence of staleness — keeping the row forever
      would make the holder loop never converge.
      Coverage set = every main TU the engine actually re-parsed
      (including attempts that failed or found the file missing) plus
      every existing file the include closure propagated to. Everything
      else stays queued.

      This is the no-lost-update rule: an edit that lands **while a
      batch runs** changes the hash (or is folded into the same row by
      the path primary key) and keeps the marker for the next batch —
      prefer one extra batch over a lost update. The user-facing
      semantics: edit a common header H twice (second during the first
      compile) and also edit `C.cpp` that includes H; when the batch
      triggered by the second event finishes (it re-parsed C), C's
      marker is cleared if its bytes did not change again, with no
      extra compile.
   Until a peek comes back empty; then **release** the claim (one short
   transaction deleting *own* row — `WHERE holder = ?`, so a stolen
   claim is never deleted by its expired predecessor) and exit 0.

   Equivalence invariant: the batches run through the ORDINARY
   incremental engine with external seeds, so the resting database
   equals what a normal `ccg index` run would conclude for the same
   tree state — the seed only adds certainty (an edit that preserved
   mtime+size still re-parses, because the event said the file
   changed).

`ccg update --drain` (no files) claims and loops even with no own
files — the Stop-hook tail-convergence flag: the last events of a
session are folded in when the session ends.

Crash behavior:

- A crash **before claiming** loses nothing: the enqueue is committed;
  the next event or `--drain` processes it.
- A crash **while holding**: the lease expires
  (`update-lease-ms` later) and the next updater steals the claim —
  automatic, no operator action. Work queued when that happens sits in
  `pending_updates` until the next event or `--drain` (the ordinary
  tail of any queue without a daemon).
- The lease must exceed the worst-case ONE batch (the loop heartbeats
  between batches, so only a single over-long batch can expire it).
  Both knobs are per-database config:

  | config key | default | meaning |
  |---|---|---|
  | `update-lease-ms` | `300000` (5 min) | claim lifetime without a heartbeat |
  | `update-busy-timeout-ms` | `600000` (10 min) | how long coordination transactions wait out a concurrent write lock |

  Set with `ccg config set update-lease-ms 120000` (the v6 schema
  creates the two tables at bootstrap).

Every `ccg` connection — `index`, `msbuild`, `config`, `query` — arms a
30 s default `busy_timeout` as the first statement of every open
(SCHEMA.md section 1), so ordinary commands wait out the coordination
transactions above (milliseconds each) and short external locks instead
of failing with "database is locked"; the `update-busy-timeout-ms` knob
exists because the update process must additionally out-wait a
minutes-long engine batch. The asymmetry only ever pointed one way
before: waiting longer.

### Clock source and skew

Lease arithmetic (`lease_until`, heartbeat refreshes, expiry checks) uses
unix epoch milliseconds read from the machine's own wall clock
(`std::chrono::system_clock` in `GraphStore`). Every claimant is a
process on the machine that owns the database file, so the protocol
needs no shared clock infrastructure — but wall clocks jump (NTP
corrections, manual changes), and each direction has a defined, safe
outcome:

- **Forward jump** (now leaps ahead): a live claim's `lease_until` can
  read as already expired, so the next updater steals it early. This is
  the safe direction — engine runs are idempotent (re-running the
  incremental engine on the same tree converges to the same rows), and
  heartbeat/release match on the holder string, so the original holder
  sees `claim_lost` on its next peek and stops without touching the
  thief's claim or queue. The worst case is duplicated work, never
  corruption.
- **Backward jump** (now falls behind): an expired lease can read as
  still live, which delays the steal until the clock catches back up (or
  the next event fires after it). Queued work still drains as events
  arrive — no deadlock beyond the skew itself.

Do not place the database on a share written to by hosts with
unsynchronized clocks: cross-machine skew adds directly to these
windows, and the holder match cannot protect a claim whose lease is
being judged by a foreign clock.

## 3. Wiring an editor/agent hook (sample)

The sample script `scripts/ccg-update-hook.py` is a Claude Code
PostToolUse hook: it reads the hook JSON from stdin, filters
non-corpus files, and spawns `ccg update` **detached**, so the hook
itself returns in milliseconds while the refresh happens in the
background.

**This repository ships the sample only — but the CLI now wires hooks
for you:** `ccg hook install --project` (or `--user`) merges the
PostToolUse / SessionStart / UserPromptSubmit / PreToolUse entries
below into a
Claude Code `settings.json` element-wise (own entries recognizable,
uninstall clean; CLI.md § *ccg hook*). The manual shape remains
for other editors and for understanding what gets written. Note that
the installer does **not** register a `Stop` hook — the block below is
an optional manual tail-convergence configuration for other clients:

`~/.claude/settings.json` (or a project `.claude/settings.json`):

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write|MultiEdit|Update",
        "hooks": [
          {
            "type": "command",
            "command": "python E:/path/to/cpp-code-graph/scripts/ccg-update-hook.py"
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "ccg update --drain"
          }
        ]
      }
    ]
  }
}
```

How the hooks cooperate:

- **PostToolUse** fires after every file-editing tool call. The script
  filters by `tool_name` and the corpus extensions, then spawns
  `ccg update <file>` detached. If a refresh is already running, the
  built-in handler takes the fast path instead: it enqueues the file
  into `pending_updates` and records one journal entry, with no spawn
  and no impact query — the hook never blocks the editor.
- **UserPromptSubmit** fires before the user's next input. When no
  refresh is running and the queue is empty, it claims the journal
  entries accumulated during the batch, recomputes their impact
  surfaces against the refreshed database, and injects one merged,
  bounded `additionalContext`; it is silent (zero stdout) while a
  batch still runs. The next PostToolUse uses the same flush
  opportunistically, and so does every silent exit of the PreToolUse
  handler (zero hits, gate declines, deadlines — and every Read/Glob
  call, which are delivery-only carriers): in an unattended session
  the agent's own queries deliver the last batch once it stops
  editing. SessionStart only sweeps crash/session
  leftovers.
- **Stop** (manual/optional, not installed by `ccg hook install`)
  fires when the agent finishes its turn. `ccg update --drain` claims
  (or exits 0 if held — the holder drains) and folds in whatever tail
  events landed after the last PostToolUse drain started.
- **PreToolUse** (matcher `Grep|Bash|PowerShell|Search|Read|Glob`) is the
  read-only fourth event: it watches every search-shaped tool call —
  a Grep pattern, an `rg`/`Select-String` command in Bash or
  PowerShell (a microsecond trigger-word prescreen drops non-search
  commands before any parsing cost), or a Search tool payload —
  injects the target symbol's graph facts before the search runs and
  never touches the database's write path (section 3.2). Read and Glob
  are delivery-only carriers (no intent extraction, no symbol query),
  and every silent exit of the handler opportunistically delivers the
  digest backlog — in an unattended session (no user prompt, no
  further edits) the last batch reaches the model through these query
  and read events. Upgrading an
  older installation to the widened matcher takes a re-run of `ccg
  hook install` plus a session restart (CLI.md § *ccg hook*,
  upgrade path).

### 3.1 Hook digest: conclusions of edits that landed during a batch

While a batch holds the SQLite write lock, an edit cannot insert into
`pending_updates` without waiting for the writer. The fast path bounds
that wait (150 ms), and when it loses it records
`need_drain = true` in `<ccg.db dir>/hook-digest/pending-*.json`.
Once the claim is released and `pending_updates` is empty, the flush:

1. takes a consumer-only lock (`flush.lock`), so concurrent triggers
   cannot each emit half a report. An orphan is reclaimed when its pid
   is dead, its timestamp is older than the stale window, or (empty or
   truncated by a crash between create and first write) its file mtime
   exceeds that window; on Windows an empty but readable lock proves
   the creator is gone and is reclaimed immediately;
2. merges `need_drain` paths back into `pending_updates` (idempotent)
   and drains; until the queue is empty it stays silent — reporting an
   index that is still catching up would be wrong;
3. renames journal entries to `claimed-*` (atomic claim, exactly one
   consumer wins), recomputes each file's impact surface, deduplicates
   by path / symbol uid / rendered summary (summary dedup is keyed by
   the normalized path, so distinct files never collapse into one
   conclusion), and assembles one message capped at 1200 bytes (ending
   in `等 N 项` when truncated);
4. emits, then deletes the claimed files. A crash after the claim but
   before the emit loses that round's conclusion; claimed leftovers are
   never replayed, only aged out (one hour). Pending entries age out
   after 24 hours; corrupt entries are dropped and reported on stderr.

The journal stores only the path and edit span — never impact numbers
captured at edit time — so the report always reflects the database as
of batch completion.

### 3.2 Grep-time injection (PreToolUse, I1/V7.1)

The fourth installed event, **PreToolUse** (matcher
`Grep|Bash|PowerShell|Search|Read|Glob`), fires before every
search-shaped tool call and works the opposite direction of the digest:
it does not refresh anything, it reads. Four arms feed one shared gate — a Grep
`pattern` is read directly; a Bash `command` goes through a
tree-sitter-parsed `rg` extraction behind a trigger-word prescreen
(non-search commands never pay parse cost); a PowerShell `command` is
parsed by an in-process real engine (`Select-String`/`sls`; only
search-looking commands pay the engine load); a Search payload field
is read directly.
Each arm reduces its payload to a {patterns, path hint} lookup request
(a single pattern, or several when the command is one `rg -e a -e b` /
`-Pattern a,b` OR-shape request), and the rules below then apply
uniformly to all four — anything still ambiguous (multiple search
units, multiple or wildcard paths, a `-F`/`-SimpleMatch` literal that
is not itself a bare identifier chain, dynamic `$var` components)
stays silent: better to miss an injection than to guess one. The
design rules:

- **Value-only injection, alternation-aware.** Each pattern is split at
  depth-zero `|` (escaped `\|` is a literal; branches inside groups
  stay put; empty or over-limit shapes — more than 8 branches — drop
  the whole pattern), and every branch must reduce to one whole
  identifier (an `A::b` qualified chain allowed; `^ $ \b` anchors,
  lookaround guards, and call-shape tails `\(\)`/`\(` are stripped).
  C++-keyword branches are dropped as noise; if no branch survives and
  they are not all noise, the pattern may take the restricted regex
  path below. A single surviving branch behaves exactly like the
  classic single-chain gate; several surviving branches run as one
  multi-term lookup. If nothing resolves to an indexed symbol, the
  hook prints nothing and exits 0; a search is never delayed or denied.
- **Restricted regex path (on by default).** When
  `CCG_PRETOOLUSE_REGEX` allows it (`1` forces it on, `0` opts out;
  unset or any other value falls back to the built-in default, which
  is on — the switch gates only this path, never the
  alternation split) and the re-joined non-noise branches pass the
  guardrails (length ≤ 64 bytes, no pathological quantifier forms —
  adjacent quantifiers and any `)` followed by a quantifier such as
  `(a*)*` or `(a|aa)+` are rejected, and a failing `std::regex`
  construction is caught), the hook runs one full-table scan of the
  symbol-name table with full-match semantics (`regex_match`, not
  `regex_search`), skipping names longer than 128 bytes. A unique
  matched name takes the five-line/candidate-list exits; 2-5 matched
  names render the multi-term list; more than 5 disclose the count
  only.
- **Selector semantics (2026-10-24).** The stripped pattern runs as a
  selector over the symbol table; only what it selects **uniquely**
  pins the five-line format. Several hits mean the query itself does
  not carry enough information to pick one symbol, and the hook answers
  with a **candidate list** instead of guessing (field evidence: an
  occurrence-ranked pick on a thin-margin ambiguity actively misled —
  4/10 vs 10/10 for the list, and the model trusted the injected
  identity over the same-name line). The list names the callable
  same-name symbols by references (top 5, `(+N more)` tail) with their
  definitions, and its last line teaches the pinning move — re-grep
  with a qualified name (example = the head candidate) or a path
  filter. A Grep `path` input pre-filters candidates by definition
  location before the uniqueness decision, so a narrowing path can
  collapse a list into the five-line. Zero hits after all narrowing
  stay silent. The multi-term lookup (alternation branches, merged
  `-e` patterns, or matched regex names) reuses these exits: branches
  load candidates one by one, deduplicate by symbol id, and when
  exactly one branch hits, the answer is byte-identical to that
  branch's single-chain query; when several branches hit with a total
  of at most 5 symbols, a multi-term candidate list renders (first
  line discloses the joined pattern and the hit count, entries ordered
  by branch order then references, closing line teaches re-grepping
  one term); a total above 5 discloses the count in one line instead
  of a list.
- **Five-line V7.1 format, 1400-byte budget.** A hit renders identity
  (qualified name + parameter list + kind + parent chain), definition
  coordinate, same-name disambiguation (top 3 rivals — "hits at those
  coords belong to other symbols"), top-3 callers with their
  highest-frequency site (zero-caller symbols get the explicit
  "answer with no callers" wording, not a defensive guess), and the
  dispatch confidence line. Over-budget text walks the spec's degrade
  ladder (short names, drop "(+N more)" tails, cut at the last ";",
  hard truncate with a marker). The candidate list and the multi-term
  list share the same 1400-byte budget; over budget they drop rows
  from the end (the "(+N more)" count grows), keeping the header and
  the hint line, and the multi-term list additionally caps every
  display field (pattern 64 B, qualified name 96 B, path 64 B,
  UTF-8-sequence-safe) plus a hard truncation fuse so the byte ceiling
  holds mechanically.
- **Read-only, in-process, deadline-guarded.** The handler opens the
  database directly (300 ms busy timeout — a writer holding the lock
  makes it skip silently) instead of spawning query children, and
  probes the standard hook deadline before each expensive phase. The
  regex path materializes the name table in segments (20 000 rows per
  segment; a probe between segments can abandon the rest) after a
  probe that precedes any scan SQL, and the in-memory scan loop probes
  again every 256 names — one logical full-table scan per event, never
  more. No
  dedup across events is needed because one search maps to at most one
  injection; within a render, duplicate same-name/caller entries are
  impossible by construction (they are keyed by symbol).
- **Silent exits piggyback the digest (Opportunistic).** Every silent
  exit — zero hits, an extraction gate declining, a deadline probe, a
  malformed payload, the regex switch off or its guardrails declining,
  any tool outside the four search arms, and every
  Read/Glob call (delivery-only carriers: no intent extraction, no
  symbol query, the delivery check runs before anything the search
  arms pay) — attempts one `hook_digest_flush(Opportunistic)` with the
  same 900-byte/2.5-second PTU budget the PostToolUse piggyback uses.
  The Opportunistic mode is what keeps this safe: a missing journal
  directory (one stat), a contended `flush.lock`, a live claim lease,
  an unconverged queue, or an empty backlog all return at zero cost,
  and the drain child process is Poll-mode-only behavior that never
  happens here. Any exit that produces an injection envelope — the
  five-line block, the candidate list, the multi-term list, or the
  over-match count line — does not piggyback: the
  1400-byte injection envelope stays unmixed and the backlog waits for
  the next silent exit. Successful deliveries leave a
  `flushed-pretooluse-*` marker (24 h aging) whose count surfaces as
  `digest.preToolUseFlushes` in `ccg hook status --json`; the only
  relaxation of read-onlyness is claiming/deleting journal files on
  the filesystem, never the database's write path.

### Built-in hook timeout: 10 s host limit, 9 s absolute deadline, detach not kill

A real edit of a large public header can legitimately force a near-full
reparse; no hook budget can bound that. The built-in `ccg hook
posttooluse` handler therefore never lets the refresh block the session:

- The installed `PostToolUse` entry carries `"timeout": 10` — the host's
  hard limit.
- The handler works to an **absolute deadline measured from the
  process's real creation time**, not a fixed wait. Waiting for `ccg
  update` only gets what is left after the already-elapsed process age
  and a 1 s reserve for the impact computation and injection write
  (`max(0, budget − elapsed_since_creation − 1000)` ms, so about 8 s in
  practice). Without the reserve, a 9 s wait plus the ~1.3 s fixed
  post-processing on a 12k-TU database overruns the host's 10 s limit
  and the injection is lost. The budget is 9 s on Windows and Linux and
  8 s on other POSIX platforms, where the process-age baseline is only a
  lower bound (see below).
  - Windows: `GetProcessTimes(GetCurrentProcess())` creation time
    against `GetSystemTimeAsFileTime`, so image loading, DLL
    initialization and AV scanning are all inside the budget. Because
    that difference is pure wall-clock arithmetic, it is cross-checked
    against a monotonic baseline (static-init wall difference plus the
    larger of the `QueryPerformanceCounter` and `GetTickCount64`
    deltas): a backward system-clock jump therefore keeps the elapsed
    time on the conservative upper bound instead of resetting the
    budget.
  - Linux: `/proc/self/stat` `starttime` against `/proc/uptime`.
  - Other POSIX (including macOS): the module's earliest static
    initialization is only a **lower bound** — image loading that
    happened before it is invisible, so the measured process age can
    only under-count. There is no permission-free creation-time API to
    fall back to, so the default total budget is reduced to 8 s on these
    platforms: the extra second is measured slack for the invisible
    image load instead of an assumption that the 1 s reserve absorbs it.
    Do not treat this baseline as a real creation time; it is best
    effort.
  - The actual wait budget is printed to stderr as
    `ccg hook: wait budget <n> ms (process age <m> ms)`.
- On the deadline it **detaches and never kills**: the child keeps
  running to completion in the background (Windows:
  `CREATE_BREAKAWAY_FROM_JOB` so the parent's job cannot take it down,
  falling back when the job forbids breakaway; POSIX: `setsid`, so it is
  in its own session). Its stdout/stderr go to delete-on-close temp
  files, so nothing leaks when the parent exits first.
- The injection then reads
  `[ccg] 已记录 <file>（索引更新在后台继续；影响面基于既有索引）`
  and appends the symbol impact computed from the **existing** index.
  For comment-only edits that impact (and the field owner proxy) is
  computed **before** the update wait, so a detach does not have to
  query a database held by the background writer. If the deadline
  leaves no room for that pre-computation, the injection says
  `影响面未查询（索引更新占用，已跳过）` instead of claiming the
  impact could not be resolved. The queued marker for that file is not
  lost: the background batch clears it only under the coverage/hash
  rule in §2.
- `CCG_HOOK_UPDATE_TIMEOUT_MS` caps the wait further (the test suite
  uses a tiny value to force the detach branch); it can never extend the
  absolute deadline. `CCG_HOOK_TOTAL_BUDGET_MS` and
  `CCG_HOOK_POST_RESERVE_MS` override the 9 s/1 s components for the
  deadline tests; production leaves all three unset.
- `CCG_PRETOOLUSE_REGEX` gates the restricted regex path of the
  PreToolUse injection (§3.2): `1` enables it, `0` disables it, and an
  unset or any other value falls back to the built-in default
  (currently enabled — the closed-book paired evaluation found the
  regex path harmless, so it ships on). The value is read once per
  process and cached —
  the hook is a fresh process per tool event, so each event parses it at
  most once and every repeated check on the decision path is a pure
  memory read. The switch gates only the regex path: alternation
  splitting and multi-pattern lookup are always on. Windows note:
  environment variable lookup is case-insensitive there, so a
  differently-cased variant (for example an all-lowercase spelling)
  takes effect as well — set exactly one spelling and do not export
  case variants.
- On POSIX, `execv` failure in the spawned child is reported back
  through a close-on-exec sentinel pipe (and, absent the pipe, a
  conservative `exit 127 + empty output` check). The handler then takes
  the honest "could not be started - queued for the next drain" branch
  instead of claiming the edit was indexed.

Environment knobs of the sample script:

| variable | default | meaning |
|---|---|---|
| `CCG_HOOK_CCG` | `ccg` | the ccg executable to spawn |
| `CCG_DB` | (ccg default `./.ccg` folder) | output folder (or one configuration directory inside it), passed through to the spawn |
| `CCG_HOOK_LOG` | discard | append the spawn's stdout/stderr here |

Notes:

- Run the hook with the working directory the layout folder belongs to
  (or set `CCG_DB` to the `.ccg` folder / one configuration directory);
  `ccg update` resolves relative file paths from the spawn's working
  directory, which the script inherits. Database discovery is
  `CCG_DB` > `-o`/`./.ccg` folder scan; a folder holding exactly ONE
  configuration auto-selects it, several require `--config` (the
  internal `ccg hook` handlers resolve identically and stay silent when
  nothing is found).
- The script is standalone and testable without any hook registration:
  feed it a fake PostToolUse JSON on stdin and point `CCG_HOOK_CCG` at
  any command that records its argv — the corpus filter, the spawn
  arguments, and the milliseconds-fast exit are all observable that way
  (the project's e2e gates exercise the command semantics; the script
  has no test hooks of its own to keep it production-shaped).

## 4. `ccg index` participation: startup takeover + tail drain

A full or incremental `ccg index` run is the heavyweight authoritative
writer, and its parse stage is minutes-long but read-only. It therefore
joins the SAME protocol above by TAKING THE CLAIM AT STARTUP and holding
it across the whole run — multi-writer coexistence (the common root of
`database is locked` storms and of the delete-then-insert foreign-key
rollbacks observed in the field) is dissolved by design:

1. **Parse stage — claim held.** Right after opening the db the run
   STEALS `update_claim` (db-layer `steal_update`: the same
   holder/lease_until write as the pre-existing "expired is stealable"
   rule, just triggered actively) with the same
   `ccg-update:pid:<pid>:t<ticks>` token shape as `ccg update` — the
   holder semantics do not depend on which command holds it, and the pid
   probe therefore works across binaries. A live holder is displaced
   (a `took over the update lease from '…'` note is printed) and yields
   gracefully at its next heartbeat — between batches (`peek_pending`),
   inside its commit transaction (`heartbeat_in_tx`, a typed
   `ClaimStolenError` rolls the batch back and stops the loop), or at
   dequeue time (`clear_covered`). While the steal's `BEGIN IMMEDIATE`
   queues behind the displaced holder's in-flight write lock, the index
   prints one `waiting to take over the update claim (pid …, queue …)`
   line per retry (1/2/5/10 s capped backoff, `--wait` cap).
   `--no-wait` keeps the fail-fast behavior (exit 1 instead of taking a
   live holder over). While the index holds the claim, hooks only
   enqueue — no update subprocess is spawned.
   **Parse-stage keepalive.** The parse stage itself can outlast any
   lease (26 minutes measured on the largest real corpus vs a 300 s
   lease), and none of the pre-existing beat points (in-transaction
   heartbeat, between-batch peek, post-batch dequeue) fire until parsing
   is DONE — an expired-but-alive holder is exactly what the hook's
   "expired is stealable" rule spawns a new writer for, and the index
   then discovers the theft at its first commit-stage beat and yields
   the whole run (the F-1 verdict of the real-corpus acceptance). A
   companion jthread therefore renews the lease for the whole parse
   window: it owns a SEPARATE `GraphStore` connection (zero sharing with
   the main thread's connection or statement cache — no new concurrency
   surface, the same short-IMMEDIATE-transaction shape every other
   writer uses) and beats `renew_update_lease` every
   `clamp(lease/4, 1 s, 60 s)`. BUSY is tolerated (retry next cycle;
   busy never means stolen), a zero-row beat means truly stolen (the
   thread parks itself and the main thread, stopping the keepalive
   before taking the write lock, throws the same typed
   `ClaimStolenError`: clean exit, queue belongs to the thief). The
   in-transaction heartbeat takes over the moment the commit stage
   starts. `ccg update`'s engine batches get the same coverage for free
   (they pass the same caller-held holder into the indexer). Test
   injection: `CCG_TEST_PARSE_KEEPALIVE=0` disables the thread
   (the regression-pin control arm); `CCG_TEST_PARSE_HOLD_MS` stretches
   the parse stage and `CCG_TEST_TAKEOVER_LEASE_MS` shrinks the takeover
   lease, together reproducing the pathology on a small fixture.
2. **Pre-commit freshness recheck.** Between the parse and the write
   transaction every parsed main TU is re-statted (`mtime_ns` + size, the
   fingerprint's own identity fields). A file edited while parsing
   re-parses right there (in-memory replace — the commit stage stores the
   fresh bytes); a file that vanished is degraded to a `pending_updates`
   row (the update event flow's missing-file cleanup owns it). Nothing
   knowingly stale enters the commit stage.
3. **Stale-snapshot recheck (insert-side backstop).** The parse stage
   snapshots `file_id`s outside any transaction; in the transitional
   window where the displaced holder's in-flight batch is still
   committing, rows that snapshot references may already be gone. Old
   ids flowing into foreign-key columns abort the whole batch with
   `FOREIGN KEY constraint failed` (reproduced 2/2 in a controlled
   two-process experiment). The commit transaction therefore diffs the
   snapshot against the live id set once (`all_file_ids`): dead-id path
   references are re-registered (upsert → fresh id, semantics of a
   first-seen file) and dead-id snapshot-payload writes (fingerprints,
   hashes) are dropped. Single-writer reality: zero hits, one
   primary-key scan of constant cost.
4. **In-transaction heartbeat.** Inside the commit transaction SQLite
   offers no nested transaction, so the short-transaction heartbeat
   pattern cannot run — but executing the precompiled `CLAIM_HEARTBEAT`
   UPDATE in the SAME transaction is legal, and that is what the index
   does (throttled to one beat per 60 s of wall clock across the store
   loops). Zero affected rows = the claim was stolen: the run throws, the
   transaction RAII rolls the WHOLE commit stage back, and the process
   reports `claim stolen mid-commit - nothing written`. A stale-index
   rerun is cheap compared with two writers replaying different
   worldviews. The UPDATE matches `WHERE holder = ?`, so a beaten holder
   can never touch the thief's row. (`ccg update`'s engine batches use
   the same in-transaction heartbeat via the caller-held holder; a steal
   mid-batch stops the holder loop gracefully with the queue preserved.)
5. **Adaptive lease.** The claim's lease is
   `max(update-lease-ms, 2 × index_commit_ms)` where `index_commit_ms`
   is the measured commit-stage wall clock recorded after each COMMIT
   (first run: `2 ×` the 300 s default as a conservative floor — a
   pre-lease binary cannot read an uncommitted heartbeat and only sees
   the initial `lease_until`). The floor covers the commit stage; the
   parse stage is NOT expected to fit inside any lease (it is
   minutes-to-tens-of-minutes on large corpora) — that is exactly what
   the parse-stage keepalive above is for. A crash is self-healed by the
   pid probe (a dead holder's lease is judged dead regardless of
   `lease_until`).
6. **Release + tail drain.** After COMMIT, while the claim is still
   held, queued events (edits hooks enqueued during the run) are
   converged IN PROCESS: bounded catch-up rounds run the ordinary
   incremental engine over the queue (seeded batch, stage-A fast path,
   `clear_covered` dequeue) until the queue empties or a limit is hit
   (round count / time budget; knobs `index-drain-rounds` = 8,
   `index-drain-budget-ms` = 300000). The output is three-part and
   untruncated: a detection note (`N files changed while indexing -
   re-processing (drain round K)`), the FULL file list (one path per
   line, no cap — a settled design ruling), and a closing result line
   (`index drain finished: N round(s), queue empty`, or the honest
   leftover form `index drain stopped after N round(s) - M event(s)
   still queued`, or the stolen form). Leftovers (budget exceeded,
   project-file lines that need the msbuild capture linkage) go to the
   regular mechanism: one detached `ccg update --drain` (never waited,
   never killed — the same platform detach the hook uses). Finally the
   claim is released (own row only, RAII on every unwind path; the
   guard is destroyed before the store closes).

What this removes: the pre-lease world where the index's 30 s
`busy_timeout` ran into a minutes-long update batch and the whole parse
work was rolled back (observed 2/2 in the field, with mechanism
near-certain under 10–60 s edit cadences), and the reverse starvation
where a queued index sat invisible behind a holder with no way to know
who holds what. What it does not remove: the lock wait itself — the
takeover still queues behind the displaced holder's in-flight batch
(seconds, visible on stderr with holder identity).

`ccg msbuild` shares the same tail and therefore the same flags.

## 5. Batch/scripted use without hooks

Anything that can list changed files can drive the same machinery:

```
git diff --name-only | while read f; do ccg update "$f"; done
ccg update --drain
```

Each `ccg update` is cheap when the claim is free and a single queue
insert when it is not; the drain converges the queue at the end.

## 6. Project-file events: the capture-refresh linkage

Editing a **project file** (`.sln` / `.vcxproj` / `.vcxitems` / `.props` /
`.targets` / `.filters`) changes *where the corpus comes from* — the TU
list, include paths, property inheritance — not the bytes of any TU.
Those edits used to be dropped silently (the hook's corpus-extension
filter never let them through, and `ccg update <vcxproj>` only ever
reported "not tracked"). The linkage (msbuild-incr phase 1) routes them
through the SAME queue into a re-capture:

1. **Hook gate widened**: project files pass the PostToolUse filter
   (`cg::util::is_project_file`, ASCII-folded suffix match) and enter
   `pending_updates` exactly like corpus files. The injected line only
   reports the linkage facts (a project file has no symbols, so no
   impact query runs): captured / skipped-unchanged / busy / failed /
   untracked / still running in the background.
2. **Holder classification**: the `ccg update` holder peeks the queue and
   splits each batch — corpus lines take the normal incremental engine
   batch, project-file lines go to the refresh path below. Project-file
   lines never reach the "untracked" dequeue branch first.
3. **Registry reverse-lookup (the platform-neutral gate)**: the project
   root comes from the db's `project_root` meta; the registry is
   `<root>/.ccg/msbuild.json`. The line triggers ONLY the entry whose
   name equals this db's config (`Debug-x64`), and only when the line is
   the registered solution itself or lives under the project root. No
   root, no registry, no match — the line is dequeued as untracked
   (never piles up). A non-Windows machine has no registry, so project
   events simply never trigger a capture there.
4. **Debounce**: before spawning the capture the holder waits for the
   queue to go quiet (per-db meta `msbuild_refresh_debounce_ms`,
   default 3000; `ccg config set msbuild-refresh-debounce-ms N`), with
   the lease heartbeating throughout and new queue lines resetting the
   window (30 s hard cap). IDE save-storms collapse into one capture.
5. **Hash gate (R11, paired with the evaluate channel)**: after the
   quiet window and before the lock probe the holder reads the refresh
   snapshot `<root>/.ccg/msbuild/<stem>/snapshot.json` — written by the
   tail of every successful evaluate capture — and re-derives the input
   identity: the sha256 of every recorded file (the sln, every member
   `.vcxproj`, every project's import closure — the evaluation produces
   it for free), the current `INCLUDE` hash, the MSBuild/cl paths the
   next capture's discovery cache would reuse, the derived
   VCTargetsPath, and the ccg version. ALL equal ⇒ the event is already
   covered by the existing corpus: the line is dequeued with
   `"projectRefresh": "skipped-unchanged"` and NO capture is spawned
   (most IDE saves change nothing that shapes the corpus). Anything
   unequal — no snapshot, a changed file, a changed toolchain identity,
   a corrupt snapshot line — falls through to the real refresh below.
6. **Mutual exclusion**: the holder only PROBES
   `<root>/.ccg/msbuild/capture-<stem>.lock` (read-only pid-liveness
   probe); a live lock defers the refresh and retains the line. The lock
   itself is acquired by `ccg msbuild` for its whole window (see
   MSBUILD.md).
7. **Handoff**: the holder RELEASES the update claim and spawns
   `ccg msbuild <solution> --config C --platform P -o <layout>`
   synchronously (30 min cap, detach-not-kill). The capture's index
   commit queues on the update claim (Coordination::wait), so the
   handoff is what makes the whole chain deadlock-free.
8. **After success**: the holder re-claims, dequeues the project line
   and RELOADS the compile commands (new TUs are already indexed by the
   capture's own incremental tail — unchanged TU fingerprints keep them
   skipped; a removed TU is purged; the engine itself is untouched).
   If another update took the claim meanwhile, the line stays with it —
   an idempotent re-capture at worst.
9. **Keep-old on failure**: a non-exec-class MSBuild failure (broken
   project XML, missing import) aborts the capture without touching the
   cdb/db/registry (exit 1) — the linkage retains the queued line so
   the next edit retries it. The db is never half-refreshed. The refresh
   snapshot is part of the keep-old contract: it is written only after a
   fully successful capture, so a failed run never leaves a snapshot
   claiming inputs the corpus does not reflect.

End-to-end budget: debounce 3 s (default) + the spawned capture (the
default evaluate channel — seconds for typical solutions) + the
incremental tail (~0.5 s on the 6-TU fixture; the index side only
parses the actually-changed TUs). An unchanged event costs only the
hash gate (file reads plus one directory probe, no subprocess) and is
absorbed as `skipped-unchanged`. The gate `ccg_msbuild_e2e` pins the
whole loop: happy path, keep-old, hash-gate absorption, repair
convergence, root-external line, live-lock deferral.
并发披露：联动采集进行中另一更新进程对同一项目文件的编辑会让路（busy），被留下的队列行由采集收尾进程自动消费再跑一轮采集（采集幂等，结果正确）——高并发编辑流下采集轮数可能多于事件合并数。
