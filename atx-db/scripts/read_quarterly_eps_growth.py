"""Read recent issuer quarterly diluted- or basic-EPS YoY states through IQ2 qualification."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import tempfile
from contextlib import nullcontext
from pathlib import Path

import duckdb

from atx_db.api.service import WarehouseReadService

# Each measure reads its own canonical growth metric; the reader never
# substitutes one EPS measure for the other or rebuilds EPS locally.
METRICS = {"diluted": "eps_diluted_q_growth_yoy", "basic": "eps_basic_q_growth_yoy"}
METRIC = METRICS["diluted"]
MAX_ROWS = 2048
MAX_BYTES = 2_000_000
FIELDS = [
    "period_end", "value", "value_status", "issuer_owner_id", "cik",
    "available_at", "as_of_date", "source_loaded_at", "revision_group_id", "inputs_hash",
]
REQUIRED_COLUMNS = {
    "fundamental_fact_revisions": {
        "security_id", "cik", "as_of_date", "available_at", "source_loaded_at",
    },
    "derived_metric_definitions": {
        "metric_code", "expression", "metric_window", "version", "inputs_json",
    },
    "derived_metric_values": {
        "derived_value_id", "revision_group_id", "security_id", "metric_code", "metric_window",
        "period_end", "value", "value_status", "inputs_hash", "as_of_date", "available_at",
        "source_loaded_at", "source", "valid_to", "definition_hash", "target_bucket",
        "fiscal_period_start", "fiscal_period_end", "history_status",
        "selected_input_refs_json", "selected_input_refs_hash",
    },
    "fundamental_standardized": {
        "standardized_id", "canonical_code", "cik", "basis", "source",
        "period_start", "period_end", "available_at", "value",
    },
}


class _SnapshotService(WarehouseReadService):
    """Reuse the caller's bounded read-only transaction for preflight and IQ2."""

    def __init__(self, con):
        self.con = con

    def _connect(self):
        return nullcontext(self.con)


def _utc_timestamp(value: str) -> dt.datetime:
    stamp = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if stamp.tzinfo is None:
        raise ValueError("content cutoff requires an explicit UTC offset")
    return stamp.astimezone(dt.UTC)


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--db-path", type=Path, required=True)
    p.add_argument("--cik", required=True, help="SEC CIK; this reader does not resolve tickers")
    p.add_argument("--content-as-of", type=_utc_timestamp, required=True)
    p.add_argument("--start", type=dt.date.fromisoformat, required=True)
    p.add_argument("--end", type=dt.date.fromisoformat, required=True, help="Exclusive period-end boundary")
    p.add_argument("--latest", type=int, default=3, help="Distinct observed periods, including unavailable states (1-12)")
    p.add_argument("--measure", choices=sorted(METRICS), default="diluted",
                   help="GAAP quarterly EPS measure whose canonical YoY growth is read")
    p.add_argument("--output-json", type=Path, required=True)
    return p


def _missing_columns(con) -> dict[str, list[str]]:
    missing = {}
    for table, required in REQUIRED_COLUMNS.items():
        columns = con.execute(
            "SELECT column_name FROM information_schema.columns "
            "WHERE table_catalog=current_database() AND table_schema='main' AND table_name=? "
            f"AND column_name IN ({','.join('?' for _ in required)})",
            [table, *sorted(required)],
        ).fetchall()
        absent = sorted(required - {row[0] for row in columns})
        if absent:
            missing[table] = absent
    return missing


def _check_transfer(con, sql: str, parameters: list, *, rows: int, byte_limit: int) -> dict:
    """Only two scalar aggregates cross into Python before the payload can."""
    count, size = con.execute(
        "SELECT count(*), coalesce(sum(octet_length(encode(CAST(to_json(t) AS VARCHAR)))),0) "
        f"FROM ({sql}) t", parameters,
    ).fetchone()
    if count > rows or size > byte_limit:
        raise ValueError(f"read preflight exceeds bound: {count} rows / {size} bytes")
    return {"rows": count, "bytes": size}


def _preflight(con, cik: str, cutoff: dt.datetime, metric: str = METRIC) -> dict:
    # Match IQ2's discovery and collision-check clocks exactly. Never fabricate
    # an unresolved-owner identifier or use today's ticker as historical proof.
    normalized = WarehouseReadService._normalized_cik_sql("cik")
    owners = f"""
        WITH visible AS (
            SELECT security_id, {normalized} AS normalized_cik
            FROM fundamental_fact_revisions
            WHERE coalesce(available_at,source_loaded_at) <= ?
              AND coalesce(as_of_date,CAST(coalesce(available_at,source_loaded_at) AS DATE)) <= CAST(? AS DATE)
        ), selected AS (
            SELECT DISTINCT security_id FROM visible WHERE normalized_cik=?
        )
        SELECT DISTINCT v.security_id,v.normalized_cik
        FROM visible v JOIN selected USING (security_id)
        WHERE v.normalized_cik IS NOT NULL
    """
    naive = cutoff.replace(tzinfo=None)
    parameters = [naive, naive, cik]
    result = {"owner_pairs": _check_transfer(con, owners, parameters, rows=64, byte_limit=65_536)}
    result["definitions"] = _check_transfer(
        con, "SELECT metric_code,expression,metric_window,version,inputs_json FROM derived_metric_definitions",
        [], rows=4096, byte_limit=MAX_BYTES,
    )
    # IQ2 ranks whole revisions before range filtering. Bound all visible EPS
    # revisions for these actual owners, not merely a prematurely cut page.
    roots = f"""
        SELECT d.derived_value_id,d.revision_group_id,d.security_id,d.metric_code,d.metric_window,
               d.period_end,d.value,d.value_status,d.inputs_hash,d.as_of_date,d.available_at,d.source_loaded_at
        FROM derived_metric_values d
        WHERE d.security_id IN (SELECT security_id FROM ({owners}) o)
          AND coalesce(d.available_at,d.source_loaded_at) <= ?
          AND coalesce(d.as_of_date,CAST(coalesce(d.available_at,d.source_loaded_at) AS DATE)) <= CAST(? AS DATE)
          AND d.metric_code=? AND d.metric_window='q'
    """
    result["visible_metric_revisions"] = _check_transfer(
        con, roots, [*parameters, naive, naive, metric], rows=MAX_ROWS, byte_limit=MAX_BYTES,
    )
    return result


def read_quarterly_eps(con, *, cik: str, content_as_of: dt.datetime,
                       start: dt.date, end: dt.date, latest: int = 3, measure: str = "diluted") -> dict:
    if measure not in METRICS:
        raise ValueError(f"measure must be one of {sorted(METRICS)}")
    metric = METRICS[measure]
    if not cik.isascii() or not cik.isdecimal() or not 1 <= len(cik) <= 10 or int(cik) == 0:
        raise ValueError("CIK must contain 1-10 decimal digits and be nonzero")
    if content_as_of.tzinfo is None:
        raise ValueError("content cutoff requires an explicit UTC offset")
    if not 1 <= (end - start).days <= 3660 or not 1 <= latest <= 12:
        raise ValueError("request requires 1-3660 calendar days and latest between 1 and 12")
    cutoff = content_as_of.astimezone(dt.UTC)
    cik = cik.zfill(10)
    report = {
        "contract": "quarterly-eps-desk-reader-v1",
        "cik": cik, "content_as_of": cutoff, "issuer_lookup_as_of": None,
        "period_range": {"start": start, "end_exclusive": end}, "requested_period_count": latest,
        "measure": measure, "metric": metric, "window": "q", "vintage": "latest",
        "source_limits": {
            "authority": "WarehouseReadService.issuer_content_range selected-lineage qualification",
            "availability": "Stored accounting event clocks; reconstructed history is not verified historical-vintage data.",
            "market": "CIK-selected issuer content; no historical security association or market-join certification.",
            "growth": "Canonical (current-prior)/abs(prior); zero/missing bases retain NULL domain states. No local EPS reconstruction.",
            "periods": "Latest observed qualified states in requested range, including NULL; absent fiscal quarters are not inferred.",
        },
        "limits": {"max_visible_revisions": MAX_ROWS, "max_result_bytes": MAX_BYTES,
                   "duckdb_memory": "256MB", "threads": 1},
        "rows": [],
    }
    missing = _missing_columns(con)
    if missing:
        report.update(status="schema_prerequisite_missing", missing_columns=missing,
                      required_schema="Migration 323 selected input refs and its prerequisite tables/columns")
        return report
    report["preflight"] = _preflight(con, cik, cutoff, metric)
    result = _SnapshotService(con).issuer_content_range(
        schema_name="derived-metrics", cik=cik, content_as_of=cutoff, start=start, end=end,
        items=[metric], basis=["q"], fields=FIELDS, vintage="latest", limit=MAX_ROWS,
    )
    report["issuer_diagnostics"] = result.metadata
    if result.metadata.get("truncated") or result.metadata.get("derived_lineage_scan_limited"):
        report["status"] = "query_incomplete"
        return report
    periods = sorted({row["period_end"] for row in result.data}, reverse=True)[:latest]
    rows = [dict(row) for row in result.data if row["period_end"] in periods]
    rows.sort(key=lambda row: (row["period_end"], row["issuer_owner_id"], row["revision_group_id"] or ""), reverse=True)
    for row in rows:
        # The only arithmetic performed by the reader is unit presentation.
        row["growth_percent"] = None if row["value"] is None else row["value"] * 100.0
    report["rows"] = rows
    report["observed_period_count"] = len(periods)
    report["qualified_state_count_in_range"] = len(result.data)
    report["numeric_state_count"] = sum(row["value"] is not None for row in rows)
    if not rows:
        report["status"] = (
            "no_qualified_states" if result.metadata.get("derived_lineage_rejected_count", 0)
            else "issuer_ownership_unresolved" if not result.metadata.get("issuer_owner_ids")
            else "materialization_missing_for_request"
        )
    elif result.metadata.get("issuer_owner_status") != "resolved_single_owner" or len(rows) != len(periods):
        report["status"] = "ambiguous_issuer_states"
    elif result.metadata.get("derived_lineage_rejected_count", 0):
        report["status"] = "qualified_states_with_lineage_gaps"
    elif len(periods) < latest:
        report["status"] = "insufficient_observed_periods"
    elif any(row["value"] is None for row in rows):
        report["status"] = "selected_states_unavailable"
    else:
        report["status"] = "qualified_numeric_observations"
    return report


def _write_report(output: Path, result: dict) -> None:
    encoded = json.dumps(result, default=str, indent=2, sort_keys=True, allow_nan=False) + "\n"
    if len(encoded.encode("utf-8")) > MAX_BYTES:
        raise ValueError("report exceeds result byte limit")
    fd, temporary = tempfile.mkstemp(prefix=".quarterly-eps-", suffix=".json", dir=output.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as sink:
            sink.write(encoded)
            sink.flush()
            os.fsync(sink.fileno())
        os.link(temporary, output)
    finally:
        os.unlink(temporary)


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    db_path, output = args.db_path.resolve(), args.output_json.resolve()
    if not db_path.is_file():
        parser().error("--db-path must identify an existing warehouse")
    if output == db_path or output.exists() or output.suffix.lower() != ".json" or not output.parent.is_dir():
        parser().error("--output-json requires a fresh .json path in an existing directory")
    try:
        with (
            tempfile.TemporaryDirectory(prefix=".quarterly-eps-spill-", dir=db_path.parent) as spill,
            duckdb.connect(str(db_path), read_only=True, config={
                "memory_limit": "256MB", "threads": "1", "preserve_insertion_order": "false",
                "temp_directory": spill, "max_temp_directory_size": "2GB",
            }) as con,
        ):
            con.execute("SET enable_external_access=false")
            con.execute("SET TimeZone='UTC'")
            con.execute("BEGIN TRANSACTION")
            try:
                result = read_quarterly_eps(
                    con, cik=args.cik, content_as_of=args.content_as_of,
                    start=args.start, end=args.end, latest=args.latest, measure=args.measure,
                )
                con.execute("COMMIT")
            except Exception:
                con.execute("ROLLBACK")
                raise
    except (ValueError, RuntimeError, OSError, duckdb.Error) as exc:
        result = {"contract": "quarterly-eps-desk-reader-v1", "status": "query_unavailable",
                  "measure": args.measure, "error": str(exc)[:512], "rows": []}
    try:
        _write_report(output, result)
    except (ValueError, OSError) as exc:
        print(f"quarterly EPS report unavailable: {exc}", file=sys.stderr)
        return 2
    print(f"{result['status']}: {output}")
    return 0 if result["status"] == "qualified_numeric_observations" else 2


if __name__ == "__main__":
    raise SystemExit(main())
