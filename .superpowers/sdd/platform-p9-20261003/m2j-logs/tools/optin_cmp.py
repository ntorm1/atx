"""M2j item 4 (PRE-7a, C3-9): the --ann-mean-gross run vs the flag-absent run of the same argv and exe.

usage: python optin_cmp.py ABSENT_DIR OPTIN_DIR

Pass = the same file set; every file but the main summary.json byte-identical (SHA-256); in summary.json the leaf paths
that differ are exactly /scenarios/<i>/ann_mean_gross, one per scenario object, each present on the opt-in side only;
and deleting the lines '"ann_mean_gross": ...' from the opt-in summary.json gives the flag-absent file byte for byte,
with exactly one deleted line per scenario. Prints file names, counts and paths only, never a value.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

LINE = re.compile(rb'^[ ]*"ann_mean_gross": [^\n]*\n', re.MULTILINE)


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def leaves(v, p=""):
    if isinstance(v, dict):
        for k, x in v.items():
            yield from leaves(x, f"{p}/{k}")
    elif isinstance(v, list):
        for i, x in enumerate(v):
            yield from leaves(x, f"{p}/{i}")
    else:
        yield p, v


def main(argv: list[str]) -> int:
    a, b = Path(argv[0]), Path(argv[1])
    fa = {p.relative_to(a).as_posix() for p in a.rglob("*") if p.is_file()}
    fb = {p.relative_to(b).as_posix() for p in b.rglob("*") if p.is_file()}
    ok = fa == fb
    print(f"absent {a.as_posix()} ({len(fa)} files) | opt-in {b.as_posix()} ({len(fb)} files); same file set {ok}")
    differ = [n for n in sorted(fa & fb) if sha(a / n) != sha(b / n)]
    print(f"files differing: {differ}")
    ok &= differ == ["summary.json"]
    sa, sb = (a / "summary.json").read_bytes(), (b / "summary.json").read_bytes()
    ja, jb = json.loads(sa), json.loads(sb)
    la, lb = dict(leaves(ja)), dict(leaves(jb))
    paths = sorted(k for k in set(la) | set(lb) if la.get(k, "<absent>") != lb.get(k, "<absent>"))
    scen = ja.get("scenarios")
    n = len(scen) if isinstance(scen, list) else -1
    want = [f"/scenarios/{i}/ann_mean_gross" for i in range(n)]
    only_b = all(k not in la and k in lb for k in paths)
    print(f"summary.json: scenarios {n}; differing leaf paths {paths}; each on the opt-in side only {only_b}")
    ok &= sorted(paths) == sorted(want) and only_b
    stripped, count = LINE.subn(b"", sb)
    eq = stripped == sa
    print(f"opt-in summary.json minus its '\"ann_mean_gross\"' lines: {count} lines deleted; equals the flag-absent "
          f"file byte for byte: {eq}")
    ok &= eq and count == n
    cap = "capacity/summary.json"
    if cap in fa:
        print(f"{cap} holds an ann_mean_gross key on the opt-in side: {b'ann_mean_gross' in (b / cap).read_bytes()}")
        ok &= b"ann_mean_gross" not in (b / cap).read_bytes()
    print(f"VERDICT: {'HOLDS (only summary.json, one ann_mean_gross key per scenario)' if ok else 'MISS'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
