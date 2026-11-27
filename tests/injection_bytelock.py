#!/usr/bin/env python3
"""PreToolUse injection byte-lock gate.

Runs the REAL `ccg hook pretooluse` processor against a fixture database
that the caller (tests/injection-bytelock.cmake) just built inside the
work directory from the committed corpus sources
(tests/fixtures/injection/corpus), and asserts the rendered
additionalContext is byte-identical to the expectations locked in
tests/fixtures/injection/byte-lock.json. Only the corpus sources and
the locked bytes live in this repository; the database itself is
rebuilt on every run, so the gate is fully machine-independent.

Lock forms covered by the manifest entries:
  * unique qualified name -> five-line fact block (identity, def
    coordinate, same-name disambiguation, callers, dispatch tier);
  * bare ambiguous name -> candidate list ("mode": "list");
  * bare ambiguous name + path filter -> collapses the ambiguity and
    must reproduce the unique five-line branch byte-for-byte;
  * restricted-regex pattern ("mode": "regex" / "envelope"): a
    full match on exactly one symbol name -> the five-line branch;
    more matching names than the list cap -> the graded envelope
    (hit total, the lexicographically smallest names that can be
    re-queried verbatim, one narrowing hint; exact match required,
    since the head selection does not depend on database insertion
    order);
  * restricted-regex second layer ("mode": "regex-partial" /
    "regex-partial-list" / "regex-partial-envelope"): a pattern with
    no full match is re-scanned for fragments inside the names; the
    hits take the same three exits as above (five-line branch,
    multi-term list, graded envelope);
  * names the engine cannot settle within its step limit are left
    out of the hits and their count is disclosed on every exit (the
    "-undetermined" entries).

Fallback semantics for five-line entries (no mode, or mode "regex" or
"regex-partial"): when a qualified-name query renders as a candidate
list instead (the symbol's same-name family grew beyond one), the
entry still passes softly as LIST when the rendered list carries the
locked symbol's qualified name and def coordinate. List-mode,
envelope-mode and path-narrowed entries always require an exact byte
match.

Set CCG_BYTELOCK_REGEN=1 to rewrite the manifest's expect fields with
the live output (the reviewed way to re-lock after an intentional
format change); see the README beside the manifest.

  python -I injection_bytelock.py <ccg.exe> <repo-root> <work-dir>
"""
import json
import os
import subprocess
import sys

MANIFEST_REL = "tests/fixtures/injection/byte-lock.json"
LIST_HEAD = '" matches '


def qualified_of(want):
    """Identity line's qualified name: `[ccg] A::b(params) — ...` -> A::b."""
    first = want.split("\n", 1)[0]
    body = first[len("[ccg] "):]
    return body.split("(", 1)[0]


def def_at_of(want):
    """Definition coordinate as a list-entry suffix: `@file:line`."""
    for line in want.split("\n"):
        if line.startswith("[ccg] def: "):
            coord = line[len("[ccg] def: "):].rsplit(":", 1)[0]
            return "@" + coord
    raise AssertionError("no def line in locked output")


def run_hook(ccg, work_dir, env, pattern, path=None):
    tool_input = {"pattern": pattern, "output_mode": "content"}
    if path is not None:
        tool_input["path"] = path
    payload = json.dumps({
        "hook_event_name": "PreToolUse",
        "tool_name": "Grep",
        "tool_input": tool_input,
    })
    proc = subprocess.run(
        [ccg, "hook", "pretooluse"], input=payload, capture_output=True,
        text=True, encoding="utf-8", errors="replace", cwd=work_dir,
        env=env)
    if not proc.stdout.strip():
        return ""
    return json.loads(proc.stdout.strip())["hookSpecificOutput"][
        "additionalContext"]


def main():
    ccg, repo, work = sys.argv[1:4]
    manifest = os.path.join(repo, MANIFEST_REL)
    doc = json.load(open(manifest, encoding="utf-8"))
    # 参考库由调用方（cmake 闸门）刚在工作目录里现场索引；
    # cwd=工作目录让 display_path 相对化（换机零漂移）。
    db_folder = os.path.join(work, ".ccg")
    env = dict(os.environ)
    env["CCG_DB"] = db_folder
    # 正则路径及其第二层显式开启：宿主环境残留的开关取值不得翻转
    # 正则条目。
    env["CCG_PRETOOLUSE_REGEX"] = "1"
    env["CCG_PRETOOLUSE_REGEX_PARTIAL"] = "1"
    regen = os.environ.get("CCG_BYTELOCK_REGEN") == "1"
    failures = []

    for entry in doc["entries"]:
        label = entry["id"]
        shown = entry["pattern"] + (
            " @" + entry["path"] if "path" in entry else "")
        want = entry["expect"]
        got = run_hook(ccg, work, env,
                       entry["pattern"], entry.get("path"))
        if regen:
            if not got:
                print("%-16s %-46s REGEN REFUSED (empty render)"
                      % (label, shown))
                failures.append(label)
                continue
            entry["expect"] = got
            print("%-16s %-46s REGENERATED (%d B)"
                  % (label, shown, len(got.encode("utf-8"))))
            continue
        if got == want:
            print("%-16s %-46s IDENTICAL" % (label, shown))
            continue
        # 清单族回退：五行块条目（无 mode，或 mode 为 regex /
        # regex-partial）渲染成候选清单（同名族变大）时，锁定符号的
        # 限定名与 def 坐标仍在清单里即软通过（清单、信封与路径收窄
        # 条目必须逐字节相等）。
        if (entry.get("mode") in (None, "regex", "regex-partial")
                and "path" not in entry
                and LIST_HEAD in got and got.startswith("[ccg] \"")):
            if qualified_of(want) in got and def_at_of(want) in got:
                print("%-16s %-46s LIST (family; locked def present)"
                      % (label, shown))
                continue
        failures.append(label)
        print("%-16s %-46s DIFF" % (label, shown))
        print("  want %d B: %r" % (len(want.encode("utf-8")), want[:300]))
        print("  got  %d B: %r" % (len(got.encode("utf-8")), got[:300]))

    if regen:
        if failures:
            print("regeneration REFUSED for: %s (nothing written)"
                  % ", ".join(failures))
            return 1
        with open(manifest, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print("regenerated %d entry(ies) -> %s" % (len(doc["entries"]),
                                                   manifest))
        return 0

    print("byte-lock: %d failure(s)" % len(failures))
    if failures:
        print("FAILED:", ", ".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
