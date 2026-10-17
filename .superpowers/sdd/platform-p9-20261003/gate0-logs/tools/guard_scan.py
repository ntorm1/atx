"""GATE-0 (RESUME 11) STORE-M1: the classes guard's scan with its own PY_DIRS and with extra dirs, read-only.

Imports atx-engine/tools/test_research_store_classes.py by path (PYTHONDONTWRITEBYTECODE=1 expected), sets PY_DIRS to
the module's own tuple plus each --add dir, and prints: the PY_DIRS used, scanned file count, distinct literal count,
and every unregistered literal with its files (the guard's own `unregistered` against classes.json at the date given).

usage: guard_scan.py REPO [--add DIR ...] [--today YYYY-MM-DD]
"""
from __future__ import annotations

import argparse
import datetime
import importlib.util
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(allow_abbrev=False)
    ap.add_argument("repo", type=Path)
    ap.add_argument("--add", action="append", default=[])
    ap.add_argument("--today", default=None)
    a = ap.parse_args()
    path = a.repo / "atx-engine" / "tools" / "test_research_store_classes.py"
    spec = importlib.util.spec_from_file_location("guard_under_scan", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    dirs = tuple(mod.PY_DIRS) + tuple(d for d in a.add if d not in mod.PY_DIRS)
    mod.PY_DIRS = dirs
    today = datetime.date.fromisoformat(a.today) if a.today else datetime.date.today()
    files = mod.scanned_files(a.repo)
    found = mod.scan(a.repo)
    missing = mod.unregistered(found, mod.load(a.repo / "atx-engine" / "schemas" / "research_store" / "classes.json"),
                               today)
    print(f"PY_DIRS {list(dirs)}")
    print(f"today {today.isoformat()}")
    print(f"scanned_files {len(files)}")
    print(f"distinct_literals {len(found)}")
    print(f"unregistered {len(missing)}")
    for literal, where in missing.items():
        print(f"  {literal}: {', '.join(where)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
