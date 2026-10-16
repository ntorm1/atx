"""GATE agent 4, step 7 (M2A-4, SQL3-2, STORE-S S-4): the real-tree catalog of the gate head, run INSIDE one bounded run
(`scripts/run_bounded_research.py ... -- python gate_cat.py ...`), so that the second from-scratch catalog walks the
same tree as the first: the runner's own run dir is the only thing the runs write inside the root (its start.json is
there for both walks; its receipt.json is written after this process ends), and both catalog DBs live OUTSIDE the root
(never `build-equity/research-store/`, so the record hook stays off: SQL3-2).

usage: python gate_cat.py STORE_EXE ROOT DB_A DB_B   (cwd = the pool root)
Per catalog: `init --catalog DB`, then `catalog --catalog DB --root ROOT --ledger build-equity/trials.jsonl --classes
atx-engine/schemas/research_store/classes.json` (M6B-FINISH item 7's argv, `m6b-logs/tools/realcat.sh`). Printed: the
exe's SHA-256, each exit code, the `files_seen` / `catalog_digest` lines of each catalog only, and whether the two
digests are equal. Nothing else of the catalog's output is printed; no document is opened.
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    exe, root, dbs = str(Path(argv[0]).resolve()), argv[1], argv[2:4]   # absolute: CreateProcess (first run)
    print(f"store exe {exe} sha256 {hashlib.sha256(Path(exe).read_bytes()).hexdigest()}")
    digests, rc = [], 0
    for tag, db in zip("AB", dbs):
        if Path(db).exists():
            print(f"catalog {tag}: {db} exists: stop")
            return 8
        Path(db).parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([exe, "init", "--catalog", db], capture_output=True, text=True)
        print(f"catalog {tag}: init exit {r.returncode}")
        r = subprocess.run([exe, "catalog", "--catalog", db, "--root", root, "--ledger", "build-equity/trials.jsonl",
                            "--classes", "atx-engine/schemas/research_store/classes.json"],
                           capture_output=True, text=True)
        keep = [ln for ln in r.stdout.splitlines() if ln.startswith(("files_seen", "catalog_digest"))]
        print(f"catalog {tag}: catalog exit {r.returncode}; " + "; ".join(keep))
        if r.returncode != 0:
            print(f"catalog {tag}: stderr (first 300 chars) {r.stderr.strip()[:300]}")
        rc = max(rc, r.returncode)
        digests.append(next((ln.split()[-1] for ln in keep if ln.startswith("catalog_digest")), None))
    same = digests[0] is not None and digests[0] == digests[1]
    print(f"second from-scratch catalog reproduces the digest: {same}")
    return rc if same else max(rc, 1)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
