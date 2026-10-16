"""GATE step 3, ALCLOCK-1's opt-in check (`task-AL-CLOCK-report.md` "Flag-absent identity procedure" item 4; the form of
m2j-logs/tools/optin_cmp.py): the --info-clock-print run vs the flag-absent run of the same argv and exe.

usage: python clock_optin_cmp.py ABSENT_DIR OPTIN_DIR

Pass = the same file set; every file but the main summary.json byte-identical (SHA-256); in summary.json every differing
leaf path lies under /scenarios/<i>/info_clock and exists on the opt-in side only, and the opt-in document with each
scenario's `info_clock` key deleted equals the flag-absent document (parsed JSON, key order kept). Prints file names,
counts and paths only, never a value.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path


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
    ja = json.loads((a / "summary.json").read_text(encoding="utf-8"))
    jb = json.loads((b / "summary.json").read_text(encoding="utf-8"))
    la, lb = dict(leaves(ja)), dict(leaves(jb))
    paths = sorted(k for k in set(la) | set(lb) if la.get(k, "<absent>") != lb.get(k, "<absent>"))
    pat = re.compile(r"^/scenarios/\d+/info_clock(/|$)")
    outside = [k for k in paths if not pat.match(k)]
    only_b = all(k not in la and k in lb for k in paths)
    scen = jb.get("scenarios")
    n = len(scen) if isinstance(scen, list) else -1
    blocks = sum(1 for s in (scen or []) if isinstance(s, dict) and "info_clock" in s)
    print(f"summary.json: scenarios {n}; scenarios carrying info_clock {blocks}; differing leaf paths {len(paths)}; "
          f"outside /scenarios/*/info_clock {outside[:20] or 'none'}; each on the opt-in side only {only_b}")
    ok &= not outside and only_b and blocks == n
    for s in scen or []:
        if isinstance(s, dict):
            s.pop("info_clock", None)
    eq = json.dumps(jb) == json.dumps(ja)
    print(f"opt-in summary.json minus each scenario's info_clock equals the flag-absent document: {eq}")
    ok &= eq
    print(f"VERDICT: {'holds' if ok else 'FAILS'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
