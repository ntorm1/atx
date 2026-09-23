"""Bounded, read-only view of one activation snapshot's durable ledgers."""

from __future__ import annotations

import datetime as dt
import uuid
from contextlib import closing
from pathlib import Path
from typing import Any

import duckdb

from .activation import STAGE_ORDER
from .migrations import MIGRATIONS

_SOURCE_STAGES = {
    "submissions_load": ("sec_submissions", "-submissions"),
    "earnings_release_facts": ("sec_earnings_release_facts", "-earnings-release"),
    "companyfacts_load": ("sec_company_facts", "-companyfacts"),
}
_REQUIRED_TABLES = ("schema_migrations", "activation_stage_runs", "dataset_runs")


def _base(db_path: Path, as_of_date: dt.date) -> dict[str, Any]:
    return {
        "status": "unavailable",
        "db_path": str(db_path),
        "as_of_date": as_of_date.isoformat(),
        "ledger_completion": "unknown",
        "production_readiness": "unassessed",
        "process_liveness": "unknown",
        "operator_check_needed": True,
        "unattributed_attempts_present": None,
        "stages": [],
    }


def _iso(value: dt.datetime | None) -> str | None:
    return value.isoformat() + "Z" if value is not None else None


def _stage_rows(con: duckdb.DuckDBPyConnection, snapshot: str) -> list[tuple[Any, ...]]:
    # Select JSON-derived, whitelisted scalars only. Invalid or missing metadata
    # cannot make a historical attempt appear under the requested snapshot.
    return con.execute(
        """
        WITH valid_attempts AS (
            SELECT stage, run_id, status, started_at, finished_at, "rows",
                   error IS NOT NULL AS has_error,
                   CASE WHEN json_valid(params_json) THEN params_json ELSE NULL END AS safe_params
            FROM activation_stage_runs
        ), snapshot_attempts AS (
            SELECT stage, run_id, status, started_at, finished_at, "rows",
                   has_error,
                   try_cast(json_extract_string(safe_params, '$.companyfacts_limit') AS BIGINT)
                       AS companyfacts_limit,
                   CASE WHEN json_extract_string(safe_params, '$.companyfacts_symbol_source')
                                  IN ('sec_company_tickers', 'archive_members')
                        THEN json_extract_string(safe_params, '$.companyfacts_symbol_source')
                        ELSE NULL END AS companyfacts_symbol_source,
                   try_cast(json_extract_string(safe_params, '$.submissions_batch_size') AS BIGINT)
                       AS submissions_batch_size,
                   CASE WHEN json_extract_string(safe_params, '$.earnings_release_ciks') = 'null'
                        THEN NULL
                        ELSE try_cast(json_array_length(
                            json_extract(safe_params, '$.earnings_release_ciks')) AS BIGINT)
                   END AS earnings_release_cik_count,
                   row_number() OVER (
                       PARTITION BY stage ORDER BY started_at DESC, run_id DESC
                   ) AS attempt_rank
            FROM valid_attempts
            WHERE regexp_full_match(
                  json_extract_string(safe_params, '$.as_of_date'),
                  '[0-9]{4}-[0-9]{2}-[0-9]{2}')
              AND try_cast(json_extract_string(safe_params, '$.as_of_date') AS DATE) = ?
        )
        SELECT stage, run_id, status, started_at, finished_at, "rows", has_error,
               companyfacts_limit, companyfacts_symbol_source, submissions_batch_size,
               earnings_release_cik_count
        FROM snapshot_attempts WHERE attempt_rank = 1
        """,
        [snapshot],
    ).fetchall()


def _source_attempt(
    con: duckdb.DuckDBPyConnection, stage: str, stage_run_id: str,
    started_at: dt.datetime, finished_at: dt.datetime | None,
) -> dict[str, Any]:
    result: dict[str, Any] = {"match": "unknown", "status": "unknown", "run_id": None,
                              "resume_candidate_run_id": None}
    if finished_at is None or finished_at < started_at:
        return result
    dataset_id, suffix = _SOURCE_STAGES[stage]
    matches = con.execute(
        """
        SELECT run_id, status FROM dataset_runs
        WHERE dataset_id = ? AND started_at >= ? AND started_at <= ?
          AND json_extract_string(
              CASE WHEN json_valid(params_json) THEN params_json ELSE NULL END,
              '$.run_id') = ?
        ORDER BY started_at DESC, run_id DESC LIMIT 2
        """,
        [dataset_id, started_at, finished_at, f"{stage_run_id}{suffix}"],
    ).fetchall()
    if not matches:
        result["match"] = "missing"
    elif len(matches) > 1:
        result["match"] = "ambiguous"
    else:
        run_id, status = matches[0]
        try:
            safe_uuid = str(uuid.UUID(str(run_id)))
        except (TypeError, ValueError, AttributeError):
            result["match"] = "invalid_uuid"
        else:
            result.update(match="unique", status=status, run_id=safe_uuid)
            if status == "failed":
                result["resume_candidate_run_id"] = safe_uuid
    return result


def pipeline_status(db_path: Path, as_of_date: dt.date) -> dict[str, Any]:
    """Inspect only migration and run ledgers; never initialize or mutate the DB."""
    db_path = Path(db_path)
    report = _base(db_path, as_of_date)
    if not db_path.is_file():
        report["reason"] = "database_missing"
        return report
    try:
        with closing(duckdb.connect(
            str(db_path), read_only=True,
            config={"memory_limit": "256MB", "threads": "1", "TimeZone": "UTC"},
        )) as con:
            present = {
                row[0] for row in con.execute(
                    "SELECT table_name FROM information_schema.tables "
                    "WHERE table_schema = 'main' AND table_name IN (?, ?, ?)",
                    list(_REQUIRED_TABLES),
                ).fetchall()
            }
            missing = [name for name in _REQUIRED_TABLES if name not in present]
            if missing:
                report.update(reason="ledger_table_missing", missing_tables=missing)
                return report

            registered = sorted({int(m.version) for m in MIGRATIONS})
            applied = sorted({int(row[0]) for row in con.execute(
                "SELECT DISTINCT try_cast(version AS INTEGER) FROM schema_migrations "
                "WHERE try_cast(version AS INTEGER) IS NOT NULL"
            ).fetchall()})
            pending = sorted(set(registered) - set(applied))
            highest_applied = max(applied, default=None)
            lower_gaps = [v for v in pending if highest_applied is not None and v < highest_applied]
            report["migrations"] = {
                "registered_versions": registered,
                "applied_versions": applied,
                "pending_versions": pending,
                "lower_version_gaps": lower_gaps,
                "schema_current": not pending,
            }
            report["unattributed_attempts_present"] = bool(con.execute(
                """SELECT EXISTS (
                    SELECT 1 FROM activation_stage_runs
                    WHERE CASE WHEN coalesce(json_valid(params_json), false)
                        THEN NOT coalesce(regexp_full_match(
                                 json_extract_string(params_json, '$.as_of_date'),
                                 '[0-9]{4}-[0-9]{2}-[0-9]{2}'), false)
                             OR try_cast(json_extract_string(params_json, '$.as_of_date') AS DATE)
                                IS NULL
                        ELSE true END
                )"""
            ).fetchone()[0])
            latest = {row[0]: row for row in _stage_rows(con, as_of_date.isoformat())}
            for stage in STAGE_ORDER:
                row = latest.get(stage)
                if row is None:
                    report["stages"].append({"stage": stage, "status": "missing"})
                    continue
                (_, run_id, status, started, finished, ledger_rows, has_error,
                 facts_limit, facts_source, subs_batch, earnings_ciks) = row
                item: dict[str, Any] = {
                    "stage": stage, "status": status, "run_id": run_id,
                    "started_at": _iso(started), "finished_at": _iso(finished),
                    "ledger_rows": ledger_rows, "has_error": bool(has_error),
                    "scope": {},
                }
                if stage == "companyfacts_load":
                    item["scope"] = {"companyfacts_limit": facts_limit,
                                     "companyfacts_symbol_source": facts_source}
                elif stage == "submissions_load":
                    item["scope"] = {"submissions_batch_size": subs_batch}
                elif stage == "earnings_release_facts":
                    item["scope"] = {"earnings_release_cik_count": earnings_ciks}
                if stage in _SOURCE_STAGES:
                    item["source_attempt"] = _source_attempt(con, stage, run_id, started, finished)
                report["stages"].append(item)

            statuses = [item["status"] for item in report["stages"]]
            report["ledger_completion"] = (
                "completed" if all(s == "completed" for s in statuses) else "incomplete"
            )
            report["status"] = "ok"
            report["operator_check_needed"] = "running" in statuses
            return report
    except (duckdb.Error, OSError, ValueError, TypeError):
        # DuckDB exceptions may echo SQL parameters or persisted values.
        report.update(reason="database_unavailable")
        return report
