"""Compare a NAV output directory with its reference by file SHA-256, JSON leaf PATH and CSV shape only (no value is
printed): the root identity check of the M2g Q5 holdings re-run (and COV step 4 when it runs).

usage: python nav_cmp.py REF_DIR NEW_DIR [--allow FILE:/json/path ...]
Every file (recursive) is compared by SHA-256. A differing JSON file lists the leaf paths whose value differs or that
exist on one side only; a differing CSV lists whether the header is equal, the row counts and the count of differing
cells per column. A difference is "inside the list" only when it is a JSON leaf named by --allow. Exit 0 = every
difference inside the list.
"""
from __future__ import annotations

import csv
import hashlib
import json
import sys
from pathlib import Path


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


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
    ref, new = Path(argv[0]), Path(argv[1])
    allow: dict[str, set[str]] = {}
    for i, t in enumerate(argv):
        if t == "--allow":
            f, path = argv[i + 1].split(":", 1)
            allow.setdefault(f, set()).add(path)
    rf = {p.relative_to(ref).as_posix() for p in ref.rglob("*") if p.is_file()}
    nf = {p.relative_to(new).as_posix() for p in new.rglob("*") if p.is_file()}
    print(f"reference {ref.as_posix()} ({len(rf)} files)  new {new.as_posix()} ({len(nf)} files)")
    ok = True
    for side, names in (("reference only", rf - nf), ("new only", nf - rf)):
        if names:
            ok = False
            print(f"  files {side}: {sorted(names)}")
    same = differ = 0
    for name in sorted(rf & nf):
        a, b = ref / name, new / name
        if sha(a) == sha(b):
            same += 1
            continue
        differ += 1
        if name.endswith(".json"):
            la = dict(leaves(json.loads(a.read_text(encoding="utf-8"))))
            lb = dict(leaves(json.loads(b.read_text(encoding="utf-8"))))
            paths = sorted(k for k in set(la) | set(lb) if la.get(k, "<absent>") != lb.get(k, "<absent>"))
            outside = [k for k in paths if k not in allow.get(name, set())]
            ok &= not outside
            print(f"  DIFF {name}: {len(paths)} leaf paths differ: {paths[:40]}; outside the list: {outside[:40] or 'none'}")
        elif name.endswith(".csv"):
            ra = list(csv.reader(a.open(newline="")))
            rb = list(csv.reader(b.open(newline="")))
            cols: dict[str, int] = {}
            for x, y in zip(ra[1:], rb[1:]):
                for j, (u, v) in enumerate(zip(x, y)):
                    if u != v:
                        key = ra[0][j] if j < len(ra[0]) else f"col{j}"
                        cols[key] = cols.get(key, 0) + 1
            ok = False
            print(f"  DIFF {name}: header equal {ra[:1] == rb[:1]}; rows {len(ra) - 1} vs {len(rb) - 1}; "
                  f"differing cells by column {cols}")
        else:
            ok = False
            print(f"  DIFF {name} (binary)")
    print(f"  files byte-identical {same}, differing {differ}")
    print(f"VERDICT: {'inside the list' if ok else 'OUTSIDE the list'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
