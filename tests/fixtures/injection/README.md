# Injection byte-lock fixture

`byte-lock.json` pins the exact `additionalContext` bytes the real
`ccg hook pretooluse` processor must render for fifteen lock forms.
The synthetic corpus sources live in `corpus/` beside this file
(namespaced same-name functions split across `alpha/` and `beta/`,
a family of eight `note_*` functions under `gamma/`, and under
`delta/` a template instance, a family of overloaded operators, one
very long name and a few names carrying fragments):

- `unique-qualified` — a qualified name pins one symbol: five-line
  fact block (identity, def coord, same-name disambiguation, callers,
  dispatch tier);
- `same-name-list` — a bare ambiguous name answers with the
  candidate list;
- `path-narrowed` — the same bare name plus a path filter collapses
  the ambiguity back to the unique five-line branch;
- `regex-unique` — a restricted-regex pattern whose full match is one
  symbol name answers with the five-line branch;
- `regex-overmatch` — a pattern matching more names than the list
  cap answers with the graded envelope (hit total, the five
  lexicographically smallest names that can be re-queried verbatim,
  one narrowing hint). The head is selected by lexicographic order
  rather than database insertion order, so the lock does not depend
  on how the reference database was written. The runner pins
  `CCG_PRETOOLUSE_REGEX=1`, so host residue cannot flip the regex
  entries;
- `regex-envelope-direct` — operator names carry regex
  metacharacters and cannot be re-queried verbatim; the head is then
  taken from the re-queryable names only while the total counts
  every hit;
- `regex-envelope-no-direct` — when no matching name can be
  re-queried verbatim the envelope lists no name and the advice line
  drops that half sentence;
- `regex-unique-template`, `regex-partial-unique-template` — a name
  with a double colon inside template arguments
  (`Box<delta::Item>`) is loaded as a whole, both as a verbatim
  re-query and through a fragment of it (second layer);
- `regex-partial-unique` — a restricted-regex pattern with no full
  match, whose fragment sits inside exactly one symbol name, answers
  with the five-line block of that symbol (the second matching
  layer);
- `regex-partial-list` — the same layer with two to five matching
  names answers with the multi-term list;
- `regex-partial-envelope` — the same layer above the list cap
  answers with the graded envelope;
- `regex-partial-envelope-undetermined`,
  `regex-partial-unique-undetermined`,
  `regex-partial-list-undetermined` — names the engine cannot settle
  within its step limit are left out of the hits, and every exit
  discloses how many there are (a clause in the envelope's first
  line, a trailing line under the five-line block and under the
  multi-term list). The runner also pins
  `CCG_PRETOOLUSE_REGEX_PARTIAL=1` for the second-layer entries.

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
