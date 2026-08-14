"""Lease-based batch worker entry point for out-of-process artifact generation."""

from __future__ import annotations

import argparse
import os
import time
import uuid
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from ..connection import DEFAULT_DB_PATH, resolve_data_dir
from .app import ARTIFACT_ROOT_ENV, CONTROL_PATH_ENV
from .batch import BatchJob, DuckDBBatchJobRepository, LocalBatchManager
from .commercial import DuckDBPricingCatalog
from .service import WarehouseReadService
from .usage import DuckDBUsageLedger


@dataclass
class BatchWorker:
    manager: LocalBatchManager
    worker_id: str
    lease_seconds: int = 300

    def run_once(self) -> BatchJob | None:
        return self.manager.process_next(worker_id=self.worker_id, lease_seconds=self.lease_seconds)

    def run_until_empty(self, *, maximum_jobs: int | None = None) -> list[BatchJob]:
        completed: list[BatchJob] = []
        while maximum_jobs is None or len(completed) < maximum_jobs:
            job = self.run_once()
            if job is None:
                break
            completed.append(job)
        return completed


def build_worker(
    *,
    warehouse_path: Path | str,
    control_path: Path | str,
    artifact_root: Path | str,
    worker_id: str | None = None,
    lease_seconds: int = 300,
) -> BatchWorker:
    control = Path(control_path)
    ledger = DuckDBUsageLedger(control)
    pricing = DuckDBPricingCatalog(control)
    manager = LocalBatchManager(
        WarehouseReadService(warehouse_path),
        DuckDBBatchJobRepository(control),
        artifact_root,
        usage_ledger=ledger,
        pricing_catalog=pricing,
    )
    identity = worker_id or f"worker-{os.getpid()}-{uuid.uuid4().hex[:12]}"
    return BatchWorker(manager=manager, worker_id=identity, lease_seconds=lease_seconds)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Process queued ATX batch artifacts.")
    parser.add_argument("--warehouse-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--control-path",
        type=Path,
        default=Path(os.environ.get(CONTROL_PATH_ENV, resolve_data_dir() / "control.duckdb")),
    )
    parser.add_argument(
        "--artifact-root",
        type=Path,
        default=Path(os.environ.get(ARTIFACT_ROOT_ENV, resolve_data_dir() / "api-batch-artifacts")),
    )
    parser.add_argument("--worker-id")
    parser.add_argument("--lease-seconds", type=int, default=300)
    parser.add_argument("--poll-seconds", type=float, default=1.0)
    parser.add_argument("--once", action="store_true", help="Attempt one job and exit.")
    parser.add_argument("--until-empty", action="store_true", help="Process queued jobs and exit when empty.")
    return parser


def main(argv: Sequence[str] | None = None) -> None:
    args = _parser().parse_args(argv)
    if args.lease_seconds < 1 or args.poll_seconds <= 0:
        raise SystemExit("lease-seconds and poll-seconds must be positive")
    worker = build_worker(
        warehouse_path=args.warehouse_path,
        control_path=args.control_path,
        artifact_root=args.artifact_root,
        worker_id=args.worker_id,
        lease_seconds=args.lease_seconds,
    )
    if args.once:
        worker.run_once()
        return
    if args.until_empty:
        worker.run_until_empty()
        return
    while True:
        if worker.run_once() is None:
            time.sleep(args.poll_seconds)


if __name__ == "__main__":
    main()
