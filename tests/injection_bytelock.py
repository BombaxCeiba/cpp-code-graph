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
    must reproduce the unique five-line branch byte-for-byte.

Fallback semantics for five-line entries: when a qualified-name query
renders as a candidate list instead (the symbol's same-name family
grew beyond one), the entry still passes softly as LIST when the
rendered list carries the locked symbol's qualified name and def
coordinate. List-mode and path-narrowed entries always require an
exact byte match.

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
        # 清单族回退：限定名条目渲染成候选清单（同名族变大）
        # 时，锁定符号的限定名与 def 坐标仍在清单里即软通过
        #（list 与路径收窄条目必须逐字节相等）。
        if (entry.get("mode") != "list" and "path" not in entry
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
