"""Recompute the source-pin digests (tripwire recipe: per path in order "<path>\n<len>\n<text CRLF->LF>") from git
blobs or the work tree (rev "WT"): dsl_vm, ic_result (M2c) and the marginal pair cache (D2 / R-1).

usage: digests.py REV [REV ...]
"""
import hashlib
import re
import subprocess
import sys

REPO = "C:/atx-wt/pool-2"
LISTS = {"dsl_vm_sources": ("atx-impl/src/strategy_ic_signal_cache.cpp", "dsl_vm_sources_sha256"),
         "ic_result_sources": ("atx-impl/src/strategy_ic_result_cache.cpp", "ic_result_sources_sha256"),
         "pair_sources": ("atx-impl/src/strategy_marginal_pair_cache.cpp", "pair_sources_sha256")}


def blob(rev, path):
    if rev == "WT":
        with open(f"{REPO}/{path}", "rb") as f:
            return f.read()
    return subprocess.run(["git", "-C", REPO, "show", f"{rev}:{path}"], capture_output=True, check=True).stdout


def parse(src, name, pin):
    m = re.search(r"\b" + name + r"\{(.*?)\};", src, re.S)
    paths = re.findall(r'"([^"]+)"', m.group(1))
    p = re.search(r"\b" + pin + r'\s*=\s*"([0-9a-f]{64})"', src).group(1)
    return paths, p


for rev in sys.argv[1:]:
    for name, (owner, pin) in LISTS.items():
        src = blob(rev, owner).decode()
        paths, p = parse(src, name, pin)
        mat = b""
        for rel in paths:
            t = blob(rev, rel).replace(b"\r\n", b"\n")
            mat += rel.encode() + b"\n" + str(len(t)).encode() + b"\n" + t
        d = hashlib.sha256(mat).hexdigest()
        print(f"{rev:10s} {name:18s} {len(paths)} files pin {p[:16]} recomputed {d[:16]} "
              f"{'EQUAL' if d == p else 'DIFF'}")
