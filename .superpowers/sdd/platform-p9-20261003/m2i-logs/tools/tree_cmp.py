"""Release = Release identity (COV-12 (b), C3 of M2i): compare two risk-store output directories written by the SAME
exe, file by file, by full SHA-256 (the compare half of covr_cmp.py det, applied to outputs the bounded runner wrote).
Prints every file's full SHA-256 on both sides (review S-5) and the container root SHA-256 from each manifest; reads no
statistic. Exit 0 = same file set and every file byte-identical (whole model.atxcov and manifest.json included).

usage: python tree_cmp.py FIRST_DIR REPEAT_DIR
"""
from __future__ import annotations

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


def main(argv: list[str]) -> int:
    a, b = Path(argv[0]), Path(argv[1])
    fa = {p.relative_to(a).as_posix(): p for p in a.rglob("*") if p.is_file()}
    fb = {p.relative_to(b).as_posix(): p for p in b.rglob("*") if p.is_file()}
    print(f"first {a.as_posix()} ({len(fa)} files) | repeat {b.as_posix()} ({len(fb)} files)")
    ok = set(fa) == set(fb)
    if not ok:
        print(f"  file sets differ: first only {sorted(set(fa) - set(fb))}, repeat only {sorted(set(fb) - set(fa))}")
    for name in sorted(set(fa) | set(fb)):
        ha = sha(fa[name]) if name in fa else "<absent>"
        hb = sha(fb[name]) if name in fb else "<absent>"
        eq = ha == hb
        ok &= eq
        print(f"  {'EQUAL ' if eq else 'DIFFER'} {name:<26} {fa[name].stat().st_size if name in fa else '-':>12} "
              f"{ha} {hb if not eq else '(same)'}")
    for side, d in (("first", a), ("repeat", b)):
        m = d / "manifest.json"
        if m.is_file():
            c = json.loads(m.read_text(encoding="utf-8")).get("container") or {}
            print(f"  {side} container root_sha256 {c.get('root_sha256')} file_sha256 {c.get('file_sha256')}")
    print(f"VERDICT: {'IDENTICAL (every file byte-equal)' if ok else 'IDENTITY MISS'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
