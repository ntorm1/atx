"""Root M6 (EXE-CUR, SPDLOG-RELINK): for each exe named in the p9-* receipts of build-equity/, the newest receipt
(highest tag p9-2r < p9-2z < p9-3a ...) that lists it, per preset (BuildDir), its source, its SHA-256, and whether the bytes on disk equal it.
usage: exe_sha_end.py RECEIPT_DIR BUILD_DIR (receipts whose BuildDir is BUILD_DIR only)"""
import hashlib
import json
import re
import sys
from pathlib import Path

build = Path(sys.argv[1])
want = Path(sys.argv[2]).resolve()
def key(tag):
    m = re.fullmatch(r"p9-(\d)([a-z])", tag)
    return (int(m.group(1)), m.group(2)) if m else None
newest = {}
for rec in build.glob("mega-p9-*-receipt.json"):
    tag = rec.name[len("mega-"):-len("-receipt.json")]
    k = key(tag)
    if k is None or k < (2, "p"):
        continue
    r = json.loads(rec.read_text(encoding="utf-8-sig"))
    if Path(r.get("BuildDir", "")).resolve() != want:
        continue
    for name, e in (r.get("Executables") or {}).items():
        if name not in newest or k > newest[name][0]:
            newest[name] = (k, tag, r.get("Source", "")[:8], e["Sha256"], Path(e["Path"]))
for name in sorted(newest):
    _, tag, src, sha, path = newest[name]
    disk = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "MISSING"
    print(f"{name}: newest receipt {tag} source {src} sha256 {sha} disk {'= receipt' if disk == sha else '!= receipt ' + disk}")
