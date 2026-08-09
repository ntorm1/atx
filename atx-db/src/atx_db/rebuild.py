from __future__ import annotations

import datetime as dt
import uuid
from dataclasses import dataclass
from typing import Any

from .connection import DuckDBStore
from .dataset import Dataset
from .jobs import DATASET_REGISTRY, OptionFactory, current_git_sha, normalize_params, refresh_quant_warehouse
from .warehouse import json_dumps, now_utc_naive


@dataclass(frozen=True)
class WarehouseRebuildResult:
    rebuild_run_id: str
    orchestrator_run_id: str
    status: str
    git_sha: str | None
    dataset_counts: dict[str, int]


def _as_date(value: str | dt.date | None) -> dt.date | None:
    if value is None or value == "":
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return dt.date.fromisoformat(str(value))


def _dataset_counts(store: DuckDBStore, run_id: str) -> dict[str, int]:
    rows = store.con.execute(
        """
        SELECT dataset_id, coalesce(rows, 0)
        FROM etl_job_steps
        WHERE run_id = ?
        ORDER BY dataset_id
        """,
        [run_id],
    ).fetchall()
    return {str(dataset_id): int(rows_loaded or 0) for dataset_id, rows_loaded in rows}


def run_warehouse_rebuild(
    store: DuckDBStore,
    *,
    registry: dict[str, tuple[type[Dataset], OptionFactory]] | None = None,
    since: str | dt.date | None = None,
    until: str | dt.date | None = None,
    rebuild_run_id: str | None = None,
    orchestrator_run_id: str | None = None,
    git_sha: str | None = None,
    gate: bool = True,
    actor: str = "warehouse_rebuild",
    params: dict[str, Any] | None = None,
) -> WarehouseRebuildResult:
    """Drive a full, auditable warehouse rebuild through the orchestrator."""

    selected_registry = DATASET_REGISTRY if registry is None else registry
    since_date = _as_date(since)
    until_date = _as_date(until)
    resolved_git_sha = git_sha if git_sha is not None else current_git_sha()
    rebuild_id = rebuild_run_id or str(uuid.uuid4())
    run_id = orchestrator_run_id or f"rebuild-{rebuild_id}"
    started_at = now_utc_naive()
    run_params = normalize_params(params or {})
    if since_date is not None:
        run_params["since"] = since_date.isoformat()
        run_params["start_date"] = since_date.isoformat()
    if until_date is not None:
        run_params["until"] = until_date.isoformat()
        run_params["end_date"] = until_date.isoformat()
    store.con.execute(
        """
        INSERT OR REPLACE INTO warehouse_rebuild_runs (
            rebuild_run_id, orchestrator_run_id, git_sha, since_date, until_date,
            status, dataset_counts_json, params_json, started_at, finished_at,
            error_message, source_loaded_at
        )
        VALUES (?, ?, ?, ?, ?, 'running', NULL, ?, ?, NULL, NULL, ?)
        """,
        [
            rebuild_id,
            run_id,
            resolved_git_sha,
            since_date,
            until_date,
            json_dumps({"params": run_params, "gate": gate}),
            started_at,
            started_at,
        ],
    )
    try:
        result = refresh_quant_warehouse(
            store,
            params=run_params,
            registry=selected_registry,
            run_id=run_id,
            full_rebuild=True,
            gate=gate,
            git_sha=resolved_git_sha,
            actor=actor,
        )
        counts = _dataset_counts(store, result.run_id)
        finished_at = now_utc_naive()
        store.con.execute(
            """
            UPDATE warehouse_rebuild_runs
            SET status = ?,
                dataset_counts_json = ?,
                finished_at = ?,
                error_message = NULL
            WHERE rebuild_run_id = ?
            """,
            [result.status, json_dumps(counts), finished_at, rebuild_id],
        )
        return WarehouseRebuildResult(
            rebuild_run_id=rebuild_id,
            orchestrator_run_id=result.run_id,
            status=result.status,
            git_sha=resolved_git_sha,
            dataset_counts=counts,
        )
    except Exception as exc:
        finished_at = now_utc_naive()
        counts = _dataset_counts(store, run_id)
        store.con.execute(
            """
            UPDATE warehouse_rebuild_runs
            SET status = 'failed',
                dataset_counts_json = ?,
                finished_at = ?,
                error_message = ?
            WHERE rebuild_run_id = ?
            """,
            [json_dumps(counts), finished_at, str(exc), rebuild_id],
        )
        raise
