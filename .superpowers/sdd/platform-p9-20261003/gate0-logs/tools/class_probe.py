"""GATE-0 (RESUME 11) STORE-M1: how pool-2's store exe classes a synthetic receipt.json of each receipt schema.

Plants a synthetic tree in SCRATCH (two receipt.json files: one atx.library-spec-receipt/v1, one
atx.bounded-research-run/v1; no repository data), runs atx-research-store init / catalog with this tree's classes.json,
and prints each file's catalogued class (research_store.Store over the catalog). usage: class_probe.py REPO SCRATCH
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    repo, scratch = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    exe = repo / "build-equity" / "bin" / "atx-research-store.exe"
    classes = repo / "atx-engine" / "schemas" / "research_store" / "classes.json"
    tree, db = scratch / "tree", scratch / "probe.sqlite"
    probes = {"lib/receipt.json": {"schema": "atx.library-spec-receipt/v1", "library": "probe"},
              "run/receipt.json": {"schema": "atx.bounded-research-run/v1"}}
    for rel, doc in probes.items():
        (tree / rel).parent.mkdir(parents=True, exist_ok=True)
        (tree / rel).write_text(json.dumps(doc) + "\n", encoding="utf-8", newline="\n")
    includes = [arg for rel in probes for arg in ("--include", str(tree / rel))]   # a walk starts at seeds / includes
    for argv in (["init", "--catalog", str(db)],
                 ["catalog", "--catalog", str(db), "--root", str(tree), *includes, "--classes", str(classes)]):
        done = subprocess.run([str(exe), *argv], capture_output=True, text=True, timeout=120)
        print(f"{argv[0]} exit {done.returncode}")
        if done.returncode != 0:
            print(done.stderr.strip())
            return 1
    sys.path.insert(0, str(repo / "atx-engine" / "tools"))
    import research_store as rs  # noqa: PLC0415
    with rs.Store.open(db) as store:
        for rel in probes:
            row = store.get("artifact", {"path_key": rel})
            print(f"{rel} {probes[rel]['schema']} -> class {None if row is None else row['class']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
