"""Root M4 (RESUME 10; copied from m3-logs/tools) keep-both check (B2-EVAL-KEEP, B2-14) for a clean --no-ff merge.

usage: keep_both.py BASE OURS THEIRS MERGED FILE...

For each FILE touched by both sides: the -U0 hunks of base..ours must reappear unchanged (same - / + lines, in the
same order) in theirs..merged, and the hunks of base..theirs in ours..merged. Two disjoint sides pass exactly; a
dropped, edited or interleaved hunk of either side fails. Prints per file the hunk count of each side, the merged
line ranges of each side's hunks, and PASS / FAIL. Text only: no file content is printed.
"""
import re
import subprocess
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def hunks(a: str, b: str, path: str) -> list:
    out = subprocess.run(["git", "diff", "-U0", "--no-color", "--no-ext-diff", a, b, "--", path],
                         capture_output=True, text=True, encoding="utf-8", errors="replace", check=True).stdout
    found, cur = [], None
    for line in out.splitlines():
        m = HUNK.match(line)
        if m:
            cur = {"new_start": int(m.group(3)), "new_len": int(m.group(4) or "1"), "minus": [], "plus": []}
            found.append(cur)
        elif cur is not None and line.startswith("-") and not line.startswith("---"):
            cur["minus"].append(line[1:])
        elif cur is not None and line.startswith("+") and not line.startswith("+++"):
            cur["plus"].append(line[1:])
    return found


def body(hs: list) -> list:
    return [(tuple(h["minus"]), tuple(h["plus"])) for h in hs]


def ranges(hs: list) -> str:
    return ", ".join(f":{h['new_start']}-{h['new_start'] + max(h['new_len'], 1) - 1}" for h in hs)


def main() -> int:
    base, ours, theirs, merged, *files = sys.argv[1:]
    ok_all = True
    for path in files:
        side_o, side_t = hunks(base, ours, path), hunks(base, theirs, path)
        after_t, after_o = hunks(ours, merged, path), hunks(theirs, merged, path)
        ok_t = body(side_t) == body(after_t)
        ok_o = body(side_o) == body(after_o)
        ok = ok_t and ok_o
        ok_all &= ok
        print(f"{path}: ours {len(side_o)} hunks (merged {ranges(after_o)}) kept {ok_o}; "
              f"theirs {len(side_t)} hunks (merged {ranges(after_t)}) kept {ok_t}; {'PASS' if ok else 'FAIL'}")
    print("RESULT", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
