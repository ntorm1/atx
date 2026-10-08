#!/usr/bin/env python
"""Publish the reconciliation serving table in bounded whole-security shards.

A single full-universe materialization of ``v_fundamental_reconciliation_contextual``
can exceed available memory because the whole contextual result must stage at
once. Scoped publishes are the governed alternative: each shard upserts its
security scope and prunes stale rows only inside that scope, so sequential
shards compose into exactly one full publication without ever holding the
complete result in memory.

Usage
-----
  python scripts/refresh_reconciliation_sharded.py --shards 16
  python scripts/refresh_reconciliation_sharded.py --shards 16 --start-shard 7   # resume
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.fundamental_reconciliation import (
    FundamentalReconciliationRefreshOptions,
    refresh_fundamental_reconciliation_serving,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sequential security-bucket reconciliation serving publish."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--shards", type=int, default=16)
    parser.add_argument("--start-shard", type=int, default=1)
    parser.add_argument("--memory-limit", choices=("256MB", "384MB"), default="384MB")
    parser.add_argument("--threads", type=int, choices=(1,), default=1)
    parser.add_argument("--receipts-dir", type=Path, default=Path("C:/atx/.superpowers/sdd/tier1-v2/receipts"))
    parser.add_argument("--run-id-prefix", default="recon-sharded")
    parser.add_argument(
        "--only-shard",
        type=int,
        default=None,
        help="Internal: execute exactly one shard in this process and exit. "
        "The parent runs every shard in a fresh interpreter because a "
        "long-lived process was measured to degrade from ~110s to ~840s "
        "per shard at identical warehouse size.",
    )
    return parser.parse_args()


def _security_shards(
    db_path: Path, shard_count: int, *, memory_limit: str = "256MB", threads: int = 1,
) -> list[list[str]]:
    shards: list[list[str]] = [[] for _ in range(shard_count)]
    with DuckDBStore(db_path, read_only=True, memory_limit=memory_limit, threads=threads) as store:
        if store.con.execute("SELECT 1 FROM fundamental_standardized WHERE security_id IS NULL OR trim(security_id)='' LIMIT 1").fetchone():
            raise ValueError("standardized rows lack a security key; refuse to silently omit them")
        for security_id, bucket in store.con.execute(
            "SELECT DISTINCT security_id,hash(security_id)%? FROM fundamental_standardized ORDER BY security_id",
            [shard_count],
        ).fetchall():
            shards[int(bucket)].append(security_id)
    return shards


def main() -> int:
    args = parse_args()
    if not os.environ.get("ATX_GUARD_JOB"):
        raise SystemExit("reconciliation requires the memory guard")
    if args.only_shard is None:
        from atx_db.activation import _companyfacts_parent_guard
        _companyfacts_parent_guard()  # Same non-HEAVY nested-admission contract.
    if args.shards < 1:
        raise SystemExit("--shards must be positive")
    shards = _security_shards(args.db_path, args.shards, memory_limit=args.memory_limit, threads=args.threads)
    if args.only_shard is None:
        print(
            json.dumps(
                {
                    "step": "plan",
                    "security_count": sum(len(s) for s in shards),
                    "shard_count": len(shards),
                    "start_shard": args.start_shard,
                }
            ),
            flush=True,
        )
    for index, shard in enumerate(shards, start=1):
        if index < args.start_shard:
            continue
        if args.only_shard is not None and index != args.only_shard:
            continue
        if not shard:
            continue
        if args.only_shard is None:
            args.receipts_dir.mkdir(parents=True, exist_ok=True)
            stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%S%f")
            base = args.receipts_dir / f"reconciliation-{stamp}-s{index:04d}"
            child = subprocess.run(
                [
                    sys.executable,
                    "C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py",
                    "--quiet", "--job-gb", "0.8", "--heavy", "--wait-minutes", "30",
                    "--parent-job", os.environ["ATX_GUARD_JOB"], "--disk-path", str(args.db_path.resolve().parent),
                    "--min-free-disk-gb", "35", "--receipt", str(base.with_suffix(".json")),
                    "--stdout", str(base.with_suffix(".out")), "--stderr", str(base.with_suffix(".err")), "--",
                    sys.executable,
                    str(Path(__file__).resolve()),
                    "--db-path",
                    str(args.db_path),
                    "--shards",
                    str(args.shards),
                    "--only-shard",
                    str(index),
                    "--memory-limit",
                    args.memory_limit,
                    "--threads",
                    str(args.threads),
                    "--run-id-prefix",
                    args.run_id_prefix,
                ],
                creationflags=subprocess.CREATE_NO_WINDOW | subprocess.CREATE_BREAKAWAY_FROM_JOB,
            )
            receipt = json.loads(base.with_suffix(".json").read_text())
            if child.returncode != 0 or receipt.get("cap_hit"):
                print(
                    json.dumps(
                        {"step": "shard", "shard": index, "status": "failed", "exit_code": child.returncode}
                    ),
                    flush=True,
                )
                return child.returncode or 1
            continue
        begun = time.monotonic()
        with DuckDBStore(args.db_path, memory_limit=args.memory_limit, threads=1) as store:
            store.con.execute("PRAGMA disable_progress_bar")
            result = refresh_fundamental_reconciliation_serving(
                store,
                FundamentalReconciliationRefreshOptions(
                    security_ids=tuple(shard),
                    run_id=f"{args.run_id_prefix}-s{index:02d}",
                ),
            )
        print(
            json.dumps(
                {
                    "step": "shard",
                    "shard": index,
                    "of": len(shards),
                    "security_count": len(shard),
                    "build_id": result.build_id,
                    "serving_rows_in_scope": result.row_count,
                    "seconds": round(time.monotonic() - begun, 1),
                }
            ),
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
