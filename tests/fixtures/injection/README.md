# Injection byte-lock fixture

`byte-lock.json` pins the exact `additionalContext` bytes the real
`ccg hook pretooluse` processor must render for three lock forms.
The synthetic corpus sources live in `corpus/` beside this file
(namespaced same-name functions split across `alpha/` and `beta/`):

- `unique-qualified` — a qualified name pins one symbol: five-line
  fact block (identity, def coord, same-name disambiguation, callers,
  dispatch tier);
- `same-name-list` — a bare ambiguous name answers with the
  candidate list;
- `path-narrowed` — the same bare name plus a path filter collapses
  the ambiguity back to the unique five-line branch.

Only the corpus sources and the locked bytes are committed. The
reference database is never committed: `injection-bytelock.cmake`
copies `corpus/` into a build-tree work directory, indexes it with
the ccg binary under test (engine discovery follows the same
convention as `tests/e2e.cmake` — no flags, ccg finds its engine),
and runs the hook with that work directory as cwd, so displayed
paths stay relative and the gate cannot drift across machines.
A schema bump is therefore not a problem either: the database is
rebuilt from scratch on every run.

## Regenerating the lock

Any intentional change to the renderer or the corpus sources turns
the `ccg_injection_bytelock` gate red. After review, re-lock by
setting one environment variable and re-running the gate once —
the live output is written back into each entry's `expect` field:

```sh
# via ctest (index + regen in one shot):
CCG_BYTELOCK_REGEN=1 ctest --test-dir <build> -R ccg_injection_bytelock

# or directly with cmake -P (what the ctest runs):
CCG_BYTELOCK_REGEN=1 cmake -P <repo>/tests/injection-bytelock.cmake \
    <build>/src/Release/ccg.exe <repo> <work-dir>
```

The regeneration run refuses to write when any entry renders empty,
and the resulting file must contain relative paths only — if an
absolute path sneaks in, the cwd/roots wiring is wrong and must be
fixed before committing the new lock.
