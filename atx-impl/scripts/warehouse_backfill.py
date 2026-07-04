#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Mapping

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db import backfill as backfill_driver
from db.jobs import DATASET_REGISTRY
from db.orchestrator import DatasetOrchestrator


ACTOR = "warehouse_backfill_cli"


def _parser(registry: Mapping[str, Any]) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Launch, resume, and inspect governed warehouse backfill runs."
    )
    sub = parser.add_subparsers(dest="command", required=True)
    db_parent = argparse.ArgumentParser(add_help=False)
    db_parent.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)

    backfill = sub.add_parser(
        "backfill",
        parents=[db_parent],
        help="Launch a governed windowed backfill through DatasetOrchestrator.run_backfill.",
    )
    backfill.add_argument("--dataset", required=True, choices=sorted(registry))
    backfill.add_argument("--start", required=True, help="Inclusive ISO start date.")
    backfill.add_argument("--end", required=True, help="Exclusive ISO end date.")
    backfill.add_argument("--chunk", required=True, help="Chunk size, e.g. 1mo, 1q, 30d.")
    backfill.add_argument("--max-parallel", type=int, default=1)

    resume = sub.add_parser(
        "resume",
        parents=[db_parent],
        help=(
            "Resume by --backfill-run-id. Orchestrated runs replay the original "
            "mode/window/include-dependencies/max-parallel metadata. If only a "
            "backfill_run row exists, the fallback safely reruns the root dataset "
            "window only with max_parallel=1 because dependency metadata was not persisted."
        ),
    )
    resume.add_argument("--backfill-run-id", required=True)

    status = sub.add_parser(
        "status",
        parents=[db_parent],
        help="Print JSON rows from v_backfill_status for a dataset and optional run id.",
    )
    status.add_argument("--dataset", required=True)
    status.add_argument("--backfill-run-id")

    return parser


def _result_json(
    result: Any,
    *,
    resumed_via: str | None = None,
    semantics: str | None = None,
) -> dict[str, Any]:
    payload = {
        "run_id": getattr(result, "run_id", getattr(result, "backfill_run_id", None)),
        "backfill_run_id": result.backfill_run_id,
        "status": result.status,
        "dataset_id": result.dataset_id if hasattr(result, "dataset_id") else None,
        "dataset_order": list(getattr(result, "dataset_order", ())),
        "partitions_planned": result.partitions_planned,
        "partitions_succeeded": result.partitions_succeeded,
        "partitions_skipped": result.partitions_skipped,
        "partitions_failed": result.partitions_failed,
        "rows_written": result.rows_written,
        "requested_max_parallel": result.requested_max_parallel,
        "effective_max_parallel": result.effective_max_parallel,
    }
    if resumed_via is not None:
        payload["resumed_via"] = resumed_via
    if semantics is not None:
        payload["semantics"] = semantics
    return payload


def _status_rows(
    store: DuckDBStore,
    *,
    dataset_id: str,
    backfill_run_id: str | None,
) -> list[dict[str, Any]]:
    sql = """
        SELECT
            backfill_run_id,
            run_id,
            root_dataset_id,
            partition_dataset_id,
            dataset_id,
            partition_key,
            window_lo,
            window_hi,
            status,
            rows_written,
            watermark_after,
            attempts,
            dead_letter_state,
            dead_letter_error,
            dead_lettered_at,
            dead_letter_run_id,
            run_status,
            run_started_at,
            run_finished_at,
            run_start_date,
            run_end_date,
            run_chunk,
            run_error_message,
            partition_updated_at
        FROM v_backfill_status
        WHERE (root_dataset_id = ? OR partition_dataset_id = ? OR dataset_id = ?)
    """
    params: list[Any] = [dataset_id, dataset_id, dataset_id]
    if backfill_run_id:
        sql += " AND backfill_run_id = ?"
        params.append(backfill_run_id)
    sql += " ORDER BY partition_dataset_id, window_lo, window_hi, partition_key"
    cursor = store.con.execute(sql, params)
    columns = [description[0] for description in cursor.description]
    return [dict(zip(columns, row)) for row in cursor.fetchall()]


def _direct_resume_from_backfill_run(
    store: DuckDBStore,
    *,
    backfill_run_id: str,
    registry: Mapping[str, Any],
) -> Any:
    row = store.con.execute(
        """
        SELECT dataset_id, start_date, end_date, chunk
        FROM backfill_run
        WHERE backfill_run_id = ?
        """,
        [backfill_run_id],
    ).fetchone()
    if row is None:
        raise KeyError(f"No backfill_run found for backfill_run_id {backfill_run_id!r}")
    store.con.execute(
        """
        UPDATE backfill_run
        SET status = 'running',
            finished_at = NULL,
            error_message = NULL
        WHERE backfill_run_id = ?
        """,
        [backfill_run_id],
    )
    return backfill_driver.run_backfill(
        store,
        str(row[0]),
        row[1],
        row[2],
        str(row[3]),
        registry=registry,
        backfill_run_id=backfill_run_id,
        include_dependencies=False,
        max_parallel=1,
        dead_letter=True,
    )


def main(
    argv: list[str] | None = None,
    *,
    registry: Mapping[str, Any] | None = None,
) -> int:
    selected_registry = DATASET_REGISTRY if registry is None else registry
    args = _parser(selected_registry).parse_args(argv)
    with DuckDBStore(args.db_path) as store:
        orchestrator = DatasetOrchestrator(
            store,
            selected_registry,
            actor=ACTOR,
            sleeper=lambda _delay: None,
        )
        if args.command == "backfill":
            result = orchestrator.run_backfill(
                args.dataset,
                args.start,
                args.end,
                args.chunk,
                max_parallel=args.max_parallel,
            )
            print(json.dumps(_result_json(result), indent=2, default=str))
            return 0

        if args.command == "resume":
            try:
                result = orchestrator.resume_backfill(args.backfill_run_id)
                payload = _result_json(
                    result,
                    resumed_via="orchestrator",
                    semantics=(
                        "Replayed the original orchestrated mode/window/dependency/"
                        "parallelism metadata under the same backfill_run_id; "
                        "succeeded partitions were skipped and non-terminal partitions rerun."
                    ),
                )
            except ValueError as exc:
                if "requires an orchestrated run header" not in str(exc):
                    raise
                result = _direct_resume_from_backfill_run(
                    store,
                    backfill_run_id=args.backfill_run_id,
                    registry=selected_registry,
                )
                payload = _result_json(
                    result,
                    resumed_via="backfill_driver_root_only",
                    semantics=(
                        "No orchestrator manifest was persisted for this backfill_run_id; "
                        "reran the saved root dataset window only with max_parallel=1. "
                        "Completed partitions were skipped and failed/running/dead-lettered "
                        "root partitions were rerun."
                    ),
                )
            print(json.dumps(payload, indent=2, default=str))
            return 0

        rows = _status_rows(
            store,
            dataset_id=args.dataset,
            backfill_run_id=args.backfill_run_id,
        )
        print(
            json.dumps(
                {
                    "dataset_id": args.dataset,
                    "backfill_run_id": args.backfill_run_id,
                    "count": len(rows),
                    "rows": rows,
                },
                indent=2,
                default=str,
            )
        )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
