"""Compress a ``sec_http`` fetch store's pre-C-71 raw objects in place (ruling C-71; resumable).

Each raw ``objects/<sha[:2]>/<sha256>`` whose bytes hash to its name becomes ``<sha256>.gz`` (written
atomically, read back, decompressed and verified against the same raw sha256), and only then is the raw
file removed. A raw file that does not hash to its name is quarantined (kept as ``.corrupt-<stamp>``) and
counted. A kill at any point is safe: rerun and the pass continues. The ledger is not changed (its sha256
and ``bytes`` are over the raw bytes). Safe beside a running fetch worker. No network, no DuckDB.

    run_memory_guarded.py --job-gb 0.4 -- python scripts/compress_sec_fetch_store.py --fetch-dir <store>
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.sec_http import FetchLedgerStore


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fetch-dir", type=Path, required=True)
    parser.add_argument("--limit", type=int, help="Stop after this many raw files (kill-and-resume drill).")
    args = parser.parse_args()
    started = time.monotonic()
    counts = FetchLedgerStore(args.fetch_dir).compress_raw_objects(limit=args.limit)
    ratio = counts["raw_bytes"] / counts["compressed_bytes"] if counts["compressed_bytes"] else None
    print(json.dumps({"compressed": counts, "ratio": None if ratio is None else round(ratio, 3),
                      "elapsed_s": round(time.monotonic() - started, 1)}, sort_keys=True), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
