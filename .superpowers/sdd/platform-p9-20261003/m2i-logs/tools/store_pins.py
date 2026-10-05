"""Store pins (W3-10 extended by PRE-29): for each risk store, the path, the SHA-256 of manifest.json, of every payload
the manifest's `files` block lists (recomputed from disk and checked against the listed value and size), and the
container block (file, root_sha256, file_sha256, bytes, format). Reads no statistic.

usage: python store_pins.py STORE_DIR [STORE_DIR ...]
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
    ok = True
    for d in map(Path, argv):
        m = d / "manifest.json"
        doc = json.loads(m.read_text(encoding="utf-8"))
        print(f"store {d.as_posix()}  model {doc.get('model')}")
        print(f"  manifest.json sha256 {sha(m)} bytes {m.stat().st_size}")
        for name, rec in sorted((doc.get("files") or {}).items()):
            p = d / name
            got = sha(p)
            good = got == rec.get("sha256") and p.stat().st_size == rec.get("bytes")
            ok &= good
            print(f"  {name:<24} sha256 {got} bytes {p.stat().st_size:>12} listed-match {good}")
        c = doc.get("container") or {}
        print(f"  container file {c.get('file')} format {c.get('format')} bytes {c.get('bytes')}")
        print(f"  container root_sha256 {c.get('root_sha256')}")
        print(f"  container file_sha256 {c.get('file_sha256')}")
        extra = sorted(p.name for p in d.iterdir() if p.is_file() and p.name not in (doc.get("files") or {})
                       and p.name != "manifest.json")
        print(f"  files on disk not listed in `files`: {extra or 'none'}")
    print(f"VERDICT: {'every listed payload matches its pin' if ok else 'A LISTED PAYLOAD DOES NOT MATCH'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
