"""Batch stage ``bars_unit_correction``: the A9 TickerHistory vendor-shares correction (tier-1 v2 1.3).

TickerHistory3 parquet deliveries state ``shares`` in THOUSANDS; the old loaders stored the raw
vendor value in ``equity_daily_bars.shares_outstanding`` (and ``market_cap_usd = shares x close``),
so those runs are 1,000x too small. Migration 0327 only gates on the unit (read-only); this stage
corrects the stored rows as a ledgered batch stage (``batch_runner``; run by OPS in 1.8):

* **Decision = the A9 rule, never re-derived:** ``plan`` calls
  ``migrations.bodies_0327.ticker_history_unit_inventory`` (the same function 0327's gate calls,
  as ``research/spine.py`` reuses its ``_median_verdict``) and freezes its per-load-run decision in
  the run's plan. A run is ``scale_x1000`` (its recorded input format means thousands and its
  median agrees), ``none`` (a unit-aware loader stored units) or ``abort`` (any doubt: the stage
  refuses to plan). Ruling C-81: a line of a thousands run with any stored row above
  ``THOUSANDS_ROW_CEILING`` (1e8; x1000 would exceed any real share count) is
  ``shares_unit_suspect`` as a WHOLE line: its ``shares_outstanding`` and ``market_cap_usd`` are
  withheld (NULL), never rescaled by guess. Whole lines only (0.13 D10): no unit boundary is ever
  placed inside a line's load run, so none falls on a factor-step bar.
* **Batches:** initially 16 whole-line hash buckets, doubled until every transaction writes at
  most 2M rows across bars, corrections, temp decisions/lines and the batch ledger (index M2).
  The exact bucket count and output-row bound are frozen in the plan.
  ``build`` copies the bucket's bars of EVERY source into ``equity_daily_bars__next`` with
  ``shares_outstanding`` and ``market_cap_usd`` multiplied by 1000 on the thousands runs (NULL on
  suspect lines, unchanged elsewhere) and writes one ``equity_bar_unit_corrections__next`` row per
  (line, load run) it decided: ``thousands`` x1000, ``shares_unit_suspect`` x1 (withheld) or
  ``units`` x1 (kept), with the A9 evidence. A bucket whose live bars hold a duplicate
  ``(security_id, trade_date, source)`` key fails loud.
* **Finalize:** per-bucket row counts and order-independent content fingerprints of live and
  staged bars AND correction ledgers equal the frozen plan and committed outputs; bars have 0
  duplicate keys, the scaled / withheld row counts equal the frozen A9 inventory exactly, the
  ledger key ``(security_id, run_start)`` is unique; then ONE short transaction swaps
  ``equity_daily_bars`` and ``equity_bar_unit_corrections`` with their shadows
  (``_bulk_publication.publish_validated_shadows``) and marks the run published. The runner
  ``CHECKPOINT``s and asserts no WAL. ``equity_daily_bars`` carries no index (M5); a live index
  makes ``prepare`` refuse (DuckDB cannot rename an indexed table).

Runs already recorded as decided in the live ledger are skipped (a thousands run's median reads
as units once corrected); prior unit-aware decisions are preserved, not ledgered again. The
stage never scales a run twice; with nothing left to correct it
refuses to plan. The live bars must not be written while a run is open (finalize refuses when
they changed); abandon the run (``run_slices.py --abandon``) and open a new run_key instead.
"""

from __future__ import annotations

import hashlib
import json
from collections.abc import Mapping
from typing import Any

import duckdb

from ._bulk_publication import publish_validated_shadows
from .batch_runner import (
    BatchResult,
    BatchSpec,
    BatchStage,
    mark_published,
    register_stage,
    run_record,
    store_for,
    utc_now,
)

__all__ = ["BARS", "BARS_NEXT", "BUCKETS", "LEDGER", "LEDGER_NEXT", "STAGE", "STAGE_NAME", "unit_decisions"]

STAGE_NAME = "bars_unit_correction"
BUCKETS = 16
MAX_BATCH_ROWS = 2_000_000
BARS, BARS_NEXT = "equity_daily_bars", "equity_daily_bars__next"
LEDGER, LEDGER_NEXT = "equity_bar_unit_corrections", "equity_bar_unit_corrections__next"
THOUSANDS_MULTIPLIER = 1000
_CORRECTED_BASES = ("thousands", "shares_unit_suspect", "units")


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)


def _catalog_ddl(conn: duckdb.DuckDBPyConnection, table: str) -> str:
    row = conn.execute(
        "SELECT sql FROM duckdb_tables() WHERE database_name = current_database() "
        "AND schema_name = current_schema() AND table_name = ?",
        [table],
    ).fetchone()
    prefix = f"CREATE TABLE {table}("
    if row is None or not str(row[0]).startswith(prefix):
        raise RuntimeError(f"cannot derive the {table} DDL for its staging table")
    return str(row[0])


def _columns(conn: duckdb.DuckDBPyConnection, table: str) -> list[str]:
    return [str(row[0]) for row in conn.execute(
        "SELECT column_name FROM duckdb_columns() WHERE database_name = current_database() "
        "AND schema_name = current_schema() AND table_name = ? ORDER BY column_index",
        [table],
    ).fetchall()]


def _indexes(conn: duckdb.DuckDBPyConnection, table: str) -> list[str]:
    return [str(row[0]) for row in conn.execute(
        "SELECT index_name FROM duckdb_indexes() WHERE database_name = current_database() "
        "AND schema_name = current_schema() AND table_name = ?",
        [table],
    ).fetchall()]


def _already_corrected_runs(conn: duckdb.DuckDBPyConnection) -> set[str | None]:
    """Load runs the live ledger records as decided (the ledger's evidence names the run)."""
    rows = conn.execute(
        f"SELECT DISTINCT json_extract_string(evidence, '$.run_id') FROM {LEDGER} WHERE unit_basis IN (?, ?, ?)",
        list(_CORRECTED_BASES),
    ).fetchall()
    return {row[0] for row in rows}


def unit_decisions(conn: duckdb.DuckDBPyConnection) -> dict[str, Any]:
    """The A9 per-run decisions and C-81 suspect lines for the stored bars (read-only).

    Raises when any undecided run has action ``abort`` (the 0327 rule's doubt) or when a suspect
    line list is truncated. ``runs``: one entry per load run with its ``unit_basis`` (thousands |
    units | None = no positive shares) and evidence; ``suspect_lines``: whole lines withheld.
    """
    from .migrations.bodies_0327 import (
        THOUSANDS_ROW_CEILING,
        TICKER_HISTORY_SOURCE,
        ticker_history_unit_inventory,
    )

    corrected = _already_corrected_runs(conn)
    inventory: list[dict[str, Any]] = ticker_history_unit_inventory(conn)
    skipped = [run["run_id"] for run in inventory if run["run_id"] in corrected]
    pending = [run for run in inventory if run["run_id"] not in corrected]
    refused = [run for run in pending if run["action"] == "abort"]
    if refused:
        raise RuntimeError(
            "bars_unit_correction refuses to plan: the A9 unit rule cannot decide "
            + "; ".join(f"run_id={run['run_id']!r} ({run['rows_in_run']:,} rows): {run['reason']}" for run in refused)
        )
    runs: list[dict[str, Any]] = []
    suspect: list[dict[str, Any]] = []
    for run in pending:
        if run["action"] == "scale_x1000":
            basis: str | None = "thousands"
        elif run["stored_unit"] == "units":
            basis = "units"
        else:
            basis = None  # no positive shares: nothing to scale, nothing to ledger
        lines = list(run["shares_unit_suspect_lines"])
        if len(lines) != int(run["shares_unit_suspect_line_count"]):
            raise RuntimeError(f"run {run['run_id']!r}: the inventory listed {len(lines)} of "
                               f"{run['shares_unit_suspect_line_count']} suspect lines")
        if lines and basis != "thousands":
            raise RuntimeError(f"run {run['run_id']!r} has suspect lines but is not a thousands run")
        runs.append({
            "run_id": run["run_id"], "unit_basis": basis,
            "multiplier": THOUSANDS_MULTIPLIER if basis == "thousands" else 1,
            "action": run["action"], "decision_basis": run["decision_basis"], "stored_unit": run["stored_unit"],
            "source_format": run["source_format"], "format_evidence": run["format_evidence"],
            "median_verdict": run["median_verdict"], "median_positive_shares": run["median_positive_shares"],
            "rows_in_run": int(run["rows_in_run"]), "distinct_securities": int(run["distinct_securities"]),
            "rows_above_thousands_ceiling": int(run["rows_above_thousands_ceiling"]),
            "shares_unit_suspect_rows": int(run["shares_unit_suspect_rows"]), "reason": run["reason"],
        })
        suspect += [{"run_id": run["run_id"], **line} for line in lines]
    return {"rule": "A9 migrations.bodies_0327.ticker_history_unit_inventory (C-81 whole-line suspects)",
            "source": TICKER_HISTORY_SOURCE, "thousands_row_ceiling": int(THOUSANDS_ROW_CEILING),
            "runs": runs, "suspect_lines": suspect, "already_corrected_runs": skipped}


# ---------------------------------------------------------------------------
# Stage callables
# ---------------------------------------------------------------------------

def plan(conn: duckdb.DuckDBPyConnection, run_key: str) -> list[BatchSpec]:
    decisions = unit_decisions(conn)
    if not any(run["unit_basis"] == "thousands" for run in decisions["runs"]):
        raise RuntimeError("bars_unit_correction has nothing to correct: no undecided vendor-thousands run "
                           f"(already corrected: {decisions['already_corrected_runs']})")
    buckets = BUCKETS
    written = _batch_write_counts(conn, decisions, buckets)
    while max(written.values(), default=0) > MAX_BATCH_ROWS:
        buckets *= 2
        if buckets > 65_536:
            raise RuntimeError("a whole security line exceeds the batch bound; cannot split its unit decision")
        written = _batch_write_counts(conn, decisions, buckets)
    source = _fingerprints(conn, BARS, buckets)
    ledger = _fingerprints(conn, LEDGER, buckets)
    planned = []
    for bucket in range(buckets):
        payload = {"bucket": bucket, "buckets": buckets, "decisions": decisions,
                   "source_fingerprint": source.get(bucket, [0, 0, 0]),
                   "ledger_fingerprint": ledger.get(bucket, [0, 0, 0]),
                   "transaction_rows": written[bucket]}
        digest = hashlib.sha256(_canonical(payload).encode()).hexdigest()
        planned.append(BatchSpec(batch_id=bucket, lo=str(bucket), hi=str(bucket), input_sha256=digest,
                                 payload=payload))
    return planned


def prepare(conn: duckdb.DuckDBPyConnection, run_key: str) -> None:
    # open_run inserts and updates its one build_runs row in this same transaction.
    ledger_rows = int(conn.execute(f"SELECT count(*) FROM {LEDGER}").fetchone()[0])
    if ledger_rows + 2 > MAX_BATCH_ROWS:
        raise RuntimeError("existing correction ledger exceeds the prepare transaction bound; needs sliced copy")
    indexed = _indexes(conn, BARS)
    if indexed:
        raise RuntimeError(f"{BARS} carries index(es) {indexed}: a bulk table must not (M5) and an indexed "
                           "table cannot be published by rename; drop them by migration first")
    for live, shadow in ((BARS, BARS_NEXT), (LEDGER, LEDGER_NEXT)):
        ddl = _catalog_ddl(conn, live)
        conn.execute(f"DROP TABLE IF EXISTS {shadow}")
        conn.execute(f"CREATE TABLE {shadow}{ddl[len(f'CREATE TABLE {live}'):]}")
    conn.execute(f"INSERT INTO {LEDGER_NEXT} SELECT * FROM {LEDGER}")


def discard(conn: duckdb.DuckDBPyConnection, run_key: str) -> None:
    for shadow in (BARS_NEXT, LEDGER_NEXT):
        conn.execute(f"DROP TABLE IF EXISTS {shadow}")


def _load_decisions(conn: duckdb.DuckDBPyConnection, decisions: Mapping[str, Any], run_key: str) -> None:
    """Temp tables ``_buc_runs`` (decided runs with their evidence) and ``_buc_suspect`` (withheld lines)."""
    conn.execute("CREATE OR REPLACE TEMP TABLE _buc_runs (run_id VARCHAR, unit_basis VARCHAR, "
                 "multiplier BIGINT, evidence VARCHAR)")
    conn.execute("CREATE OR REPLACE TEMP TABLE _buc_suspect (run_id VARCHAR, security_id VARCHAR)")
    for run in decisions["runs"]:
        if run["unit_basis"] is None:
            continue
        evidence = {"rule": "A9", "stage_run_key": run_key, "run_id": run["run_id"],
                    "decision_basis": run["decision_basis"], "format_evidence": run["format_evidence"],
                    "source_format": run["source_format"], "median_verdict": run["median_verdict"],
                    "run_median_positive_shares": run["median_positive_shares"], "run_rows": run["rows_in_run"],
                    "run_rows_above_ceiling": run["rows_above_thousands_ceiling"]}
        conn.execute("INSERT INTO _buc_runs VALUES (?, ?, ?, ?)",
                     [run["run_id"], run["unit_basis"], int(run["multiplier"]), _canonical(evidence)])
    for line in decisions["suspect_lines"]:
        conn.execute("INSERT INTO _buc_suspect VALUES (?, ?)", [line["run_id"], line["security_id"]])


def build(conn: duckdb.DuckDBPyConnection, spec: BatchSpec, run_key: str) -> BatchResult:
    payload = spec.payload
    bucket, buckets = int(payload["bucket"]), int(payload["buckets"])  # type: ignore[call-overload]
    decisions: Mapping[str, Any] = payload["decisions"]  # type: ignore[assignment]
    source, ceiling = str(decisions["source"]), int(decisions["thousands_row_ceiling"])
    current = _fingerprints(conn, BARS, buckets, bucket).get(bucket, [0, 0, 0])
    if current != payload["source_fingerprint"]:
        raise RuntimeError(f"bucket {bucket}: live content differs from the frozen input")
    if int(payload["transaction_rows"]) > MAX_BATCH_ROWS:
        raise RuntimeError(f"bucket {bucket}: planned transaction exceeds the output-row bound")
    _load_decisions(conn, decisions, run_key)
    in_bucket = [buckets, bucket]
    rows_in, duplicate_keys = conn.execute(
        f"""
        SELECT coalesce(sum(n), 0), count(*) FILTER (WHERE n > 1)
        FROM (SELECT count(*) AS n FROM {BARS} b WHERE hash(b.security_id) % ? = ?
              GROUP BY b.security_id, b.trade_date, b.source)
        """,
        in_bucket,
    ).fetchone() or (0, 0)
    if duplicate_keys:
        raise RuntimeError(f"bucket {bucket}: {duplicate_keys:,} duplicate (security_id, trade_date, source) keys "
                           f"in live {BARS}; the stage does not pick among them")
    # One row per decided (line, load run) with shares: the ledger rows and the counts finalize checks.
    conn.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _buc_lines AS
        SELECT b.security_id, b.run_id, any_value(r.unit_basis) AS unit_basis,
               any_value(r.multiplier) AS multiplier, any_value(r.evidence) AS run_evidence,
               bool_or(s.security_id IS NOT NULL) AS suspect,
               min(b.trade_date) AS run_start, max(b.trade_date) AS run_end, count(*) AS line_rows,
               count(b.shares_outstanding) AS rows_with_shares,
               count(*) FILTER (WHERE b.shares_outstanding > {ceiling}) AS rows_above_ceiling,
               max(b.shares_outstanding) AS max_stored_shares
        FROM {BARS} b
        JOIN _buc_runs r ON b.run_id IS NOT DISTINCT FROM r.run_id
        LEFT JOIN _buc_suspect s ON b.run_id IS NOT DISTINCT FROM s.run_id AND b.security_id = s.security_id
        WHERE b.source = ? AND hash(b.security_id) % ? = ?
        GROUP BY b.security_id, b.run_id
        HAVING count(b.shares_outstanding) > 0
        """,
        [source, *in_bucket],
    )
    stamp = utc_now()
    conn.execute(
        f"""
        INSERT INTO {LEDGER_NEXT} (security_id, run_start, run_end, unit_basis, multiplier, evidence, created_at)
        SELECT security_id, run_start, run_end,
               CASE WHEN suspect THEN 'shares_unit_suspect' ELSE unit_basis END,
               CASE WHEN suspect THEN 1.0 ELSE CAST(multiplier AS DOUBLE) END,
               json_merge_patch(run_evidence, json_object(
                   'line_rows', line_rows, 'line_rows_with_shares', rows_with_shares,
                   'line_rows_above_ceiling', rows_above_ceiling, 'line_max_stored_shares', max_stored_shares,
                   'withheld', suspect))::VARCHAR,
               ?
        FROM _buc_lines
        """,
        [stamp],
    )
    stats = conn.execute(
        """
        SELECT run_id,
               coalesce(sum(rows_with_shares) FILTER (WHERE unit_basis = 'thousands' AND NOT suspect), 0),
               coalesce(sum(rows_with_shares) FILTER (WHERE suspect), 0),
               coalesce(sum(rows_with_shares) FILTER (WHERE unit_basis = 'units'), 0),
               count(*) FILTER (WHERE unit_basis = 'thousands' AND NOT suspect),
               count(*) FILTER (WHERE suspect),
               count(*) FILTER (WHERE unit_basis = 'units')
        FROM _buc_lines GROUP BY run_id ORDER BY run_id NULLS FIRST
        """
    ).fetchall()
    correction_rows = sum(int(row[4]) + int(row[5]) + int(row[6]) for row in stats)
    transaction_rows = int(rows_in) + 2 * correction_rows + _decision_rows(decisions) + 1
    if transaction_rows != int(payload["transaction_rows"]) or transaction_rows > MAX_BATCH_ROWS:
        raise RuntimeError(f"bucket {bucket}: transaction output rows {transaction_rows} differ from plan/bound")
    columns = _columns(conn, BARS)
    corrected = {
        "shares_outstanding": "CASE WHEN s.security_id IS NOT NULL THEN NULL "
                              f"WHEN r.unit_basis = 'thousands' THEN b.shares_outstanding * {THOUSANDS_MULTIPLIER} "
                              "ELSE b.shares_outstanding END",
        "market_cap_usd": "CASE WHEN s.security_id IS NOT NULL THEN NULL "
                          f"WHEN r.unit_basis = 'thousands' THEN b.market_cap_usd * {THOUSANDS_MULTIPLIER}.0 "
                          "ELSE b.market_cap_usd END",
    }
    missing = set(corrected) - set(columns)
    if missing:
        raise RuntimeError(f"{BARS} lacks {sorted(missing)}")
    select = ", ".join(corrected.get(name, f'b."{name}"') for name in columns)
    inserted = conn.execute(
        f"""
        INSERT INTO {BARS_NEXT} ({", ".join(f'"{name}"' for name in columns)})
        SELECT {select}
        FROM {BARS} b
        LEFT JOIN _buc_runs r ON b.source = ? AND b.run_id IS NOT DISTINCT FROM r.run_id
        LEFT JOIN _buc_suspect s
          ON b.source = ? AND b.run_id IS NOT DISTINCT FROM s.run_id AND b.security_id = s.security_id
        WHERE hash(b.security_id) % ? = ?
        """,
        [source, source, *in_bucket],
    ).fetchone()
    rows_out = int(inserted[0]) if inserted else 0
    if rows_out != int(rows_in):
        raise RuntimeError(f"bucket {bucket}: {rows_out:,} rows staged for {int(rows_in):,} live rows")
    for name in ("_buc_lines", "_buc_runs", "_buc_suspect"):
        conn.execute(f"DROP TABLE IF EXISTS temp.main.{name}")
    note = {
        "bucket": bucket, "duplicate_keys": int(duplicate_keys),
        "output_fingerprint": _fingerprints(conn, BARS_NEXT, buckets, bucket).get(bucket, [0, 0, 0]),
        "ledger_output_fingerprint": _fingerprints(conn, LEDGER_NEXT, buckets, bucket).get(bucket, [0, 0, 0]),
        "transaction_rows": transaction_rows,
        "runs": [{"run_id": run_id, "scaled_rows": int(scaled), "withheld_rows": int(withheld),
                  "kept_rows": int(kept), "thousands_lines": int(t_lines), "suspect_lines": int(s_lines),
                  "units_lines": int(u_lines)}
                 for run_id, scaled, withheld, kept, t_lines, s_lines, u_lines in stats],
    }
    return BatchResult(rows_in=int(rows_in), rows_out=rows_out, note=_canonical(note))


def _bucket_counts(conn: duckdb.DuckDBPyConnection, table: str, buckets: int = BUCKETS) -> dict[int, int]:
    return {int(bucket): int(count) for bucket, count in conn.execute(
        f"SELECT hash(security_id) % {buckets}, count(*) FROM {table} GROUP BY 1"
    ).fetchall()}


def _decision_rows(decisions: Mapping[str, Any]) -> int:
    return sum(run["unit_basis"] is not None for run in decisions["runs"]) + len(decisions["suspect_lines"])


def _batch_write_counts(conn: duckdb.DuckDBPyConnection, decisions: Mapping[str, Any],
                        buckets: int) -> dict[int, int]:
    """All written rows: bars, temp line table, correction ledger, temp decisions, batch ledger."""
    runs = [run["run_id"] for run in decisions["runs"] if run["unit_basis"] is not None]
    lines = dict(conn.execute(f"""
        SELECT hash(security_id) % {buckets}, count(*) FROM (
            SELECT security_id, run_id FROM {BARS}
            WHERE source = ? AND shares_outstanding IS NOT NULL
              AND (list_contains(?::VARCHAR[], run_id) OR (run_id IS NULL AND ?))
            GROUP BY security_id, run_id) GROUP BY 1
        """, [decisions["source"], [run for run in runs if run is not None], None in runs]).fetchall())
    bars = _bucket_counts(conn, BARS, buckets)
    overhead = _decision_rows(decisions) + 1
    return {k: bars.get(k, 0) + 2 * int(lines.get(k, 0)) + overhead for k in range(buckets)}


def _fingerprints(conn: duckdb.DuckDBPyConnection, table: str, buckets: int,
                  bucket: int | None = None) -> dict[int, list[int]]:
    """Bounded multiset checks of all columns, not a cryptographic per-row content claim.

    Count, 128-bit sum and XOR of DuckDB row hashes detect same-count input/output drift.
    DuckDB version is pinned by the runtime; these aggregates never retain full rows.
    """
    where = "" if bucket is None else f"WHERE hash(b.security_id) % {buckets} = {int(bucket)}"
    return {int(k): [int(n), int(total), int(xor)] for k, n, total, xor in conn.execute(
        f"SELECT hash(b.security_id) % {buckets}, count(*), sum(hash(b)::HUGEINT), bit_xor(hash(b)) "
        f"FROM {table} b {where} GROUP BY 1").fetchall()}


def finalize(conn: duckdb.DuckDBPyConnection, run_key: str) -> dict[str, object]:
    record = run_record(conn, STAGE_NAME, run_key)
    if record is None:
        raise RuntimeError(f"no build run ({STAGE_NAME!r}, {run_key!r})")
    plan_rows = record["spec"]["plan"]
    decisions = plan_rows[0]["payload"]["decisions"]
    buckets = int(plan_rows[0]["payload"]["buckets"])
    batches = conn.execute(
        "SELECT batch_id, rows_in, rows_out, note FROM build_batches WHERE stage = ? AND run_key = ? "
        "ORDER BY batch_id",
        [STAGE_NAME, run_key],
    ).fetchall()
    failures: list[str] = []
    if sorted(int(b[0]) for b in batches) != list(range(buckets)):
        failures.append(f"ledgered batches {[int(b[0]) for b in batches]} are not the {buckets} buckets")
    live_counts, next_counts = _bucket_counts(conn, BARS, buckets), _bucket_counts(conn, BARS_NEXT, buckets)
    live_fingerprints, next_fingerprints = _fingerprints(conn, BARS, buckets), _fingerprints(conn, BARS_NEXT, buckets)
    ledger_fingerprints = _fingerprints(conn, LEDGER, buckets)
    next_ledger_fingerprints = _fingerprints(conn, LEDGER_NEXT, buckets)
    for batch in plan_rows:
        bucket = int(batch["payload"]["bucket"])
        if live_fingerprints.get(bucket, [0, 0, 0]) != batch["payload"]["source_fingerprint"]:
            failures.append(f"bucket {bucket}: live input fingerprint changed")
        if ledger_fingerprints.get(bucket, [0, 0, 0]) != batch["payload"]["ledger_fingerprint"]:
            failures.append(f"bucket {bucket}: live correction ledger fingerprint changed")
    for batch_id, _, _, note in batches:
        if next_fingerprints.get(int(batch_id), [0, 0, 0]) != json.loads(note)["output_fingerprint"]:
            failures.append(f"bucket {batch_id}: staged output fingerprint changed")
        if next_ledger_fingerprints.get(int(batch_id), [0, 0, 0]) != json.loads(note)["ledger_output_fingerprint"]:
            failures.append(f"bucket {batch_id}: staged correction ledger fingerprint changed")
    for batch_id, rows_in, rows_out, _ in batches:
        bucket = int(batch_id)
        if live_counts.get(bucket, 0) != int(rows_in) or next_counts.get(bucket, 0) != int(rows_out):
            failures.append(f"bucket {bucket}: live {live_counts.get(bucket, 0):,} / staged "
                            f"{next_counts.get(bucket, 0):,} rows vs ledgered {int(rows_in):,} / {int(rows_out):,}")
    live_rows, next_rows = sum(live_counts.values()), sum(next_counts.values())
    if live_rows != next_rows:
        failures.append(f"row counts differ: live {live_rows:,}, staged {next_rows:,}")
    duplicate_keys = 0
    for bucket in range(buckets):  # bounded aggregates: one bucket (at most 2M keys) per statement
        found = conn.execute(
            f"""SELECT count(*) FROM (SELECT 1 FROM {BARS_NEXT} WHERE hash(security_id) % {buckets} = ?
                GROUP BY security_id, trade_date, source HAVING count(*) > 1)""",
            [bucket],
        ).fetchone()
        duplicate_keys += int(found[0]) if found else 0
    if duplicate_keys:
        failures.append(f"{duplicate_keys:,} duplicate (security_id, trade_date, source) keys staged")
    # Corrected rows vs the frozen A9 inventory (rows with non-null stored shares, as it counts them).
    totals: dict[str | None, dict[str, int]] = {}
    for *_, note in batches:
        for item in json.loads(note)["runs"]:
            bucket_totals = totals.setdefault(item["run_id"], {})
            for key in ("scaled_rows", "withheld_rows", "kept_rows", "thousands_lines", "suspect_lines",
                        "units_lines"):
                bucket_totals[key] = bucket_totals.get(key, 0) + int(item[key])
    inventory_check = []
    for run in decisions["runs"]:
        got = totals.get(run["run_id"], {})
        if run["unit_basis"] == "thousands":
            expected = {"scaled_rows": run["rows_in_run"] - run["shares_unit_suspect_rows"],
                        "withheld_rows": run["shares_unit_suspect_rows"], "kept_rows": 0}
        elif run["unit_basis"] == "units":
            expected = {"scaled_rows": 0, "withheld_rows": 0, "kept_rows": run["rows_in_run"]}
        else:
            expected = {"scaled_rows": 0, "withheld_rows": 0, "kept_rows": 0}
        actual = {key: got.get(key, 0) for key in expected}
        inventory_check.append({"run_id": run["run_id"], "unit_basis": run["unit_basis"], "expected": expected,
                                "actual": actual, "lines": {key: got.get(key, 0) for key in
                                                            ("thousands_lines", "suspect_lines", "units_lines")}})
        if actual != expected:
            failures.append(f"run {run['run_id']!r}: corrected rows {actual} != A9 inventory {expected}")
    ledger = conn.execute(
        f"""SELECT unit_basis, count(*) FROM {LEDGER_NEXT}
            WHERE json_extract_string(evidence, '$.stage_run_key') = ? GROUP BY 1 ORDER BY 1""",
        [run_key],
    ).fetchall()
    ledger_rows = {str(basis): int(count) for basis, count in ledger}
    expected_ledger = {
        "thousands": sum(t.get("thousands_lines", 0) for t in totals.values()),
        "shares_unit_suspect": sum(t.get("suspect_lines", 0) for t in totals.values()),
        "units": sum(t.get("units_lines", 0) for t in totals.values()),
    }
    if {key: ledger_rows.get(key, 0) for key in expected_ledger} != expected_ledger or set(ledger_rows) - set(
            expected_ledger):
        failures.append(f"ledger rows {ledger_rows} != batch lines {expected_ledger}")
    if expected_ledger["shares_unit_suspect"] != len(decisions["suspect_lines"]):
        failures.append(f"{expected_ledger['shares_unit_suspect']} suspect lines ledgered, the inventory lists "
                        f"{len(decisions['suspect_lines'])}")
    key_found = conn.execute(
        f"SELECT count(*) FROM (SELECT 1 FROM {LEDGER_NEXT} GROUP BY security_id, run_start HAVING count(*) > 1)"
    ).fetchone()
    ledger_key_duplicates = int(key_found[0]) if key_found else 0
    if ledger_key_duplicates:
        failures.append(f"{ledger_key_duplicates} duplicate (security_id, run_start) keys in the correction ledger")
    receipt: dict[str, object] = {
        "stage": STAGE_NAME, "run_key": run_key, "rows": next_rows, "live_rows_before": live_rows,
        "buckets": buckets, "max_batch_rows": max(live_counts.values(), default=0),
        "max_transaction_rows": max(int(json.loads(b[3])["transaction_rows"]) for b in batches),
        "prepare_transaction_rows": sum(values[0] for values in ledger_fingerprints.values()) + 2,
        "input_output_fingerprints_match": not any("fingerprint" in f for f in failures),
        "duplicate_keys": duplicate_keys, "ledger_key_duplicates": ledger_key_duplicates,
        "inventory_check": inventory_check, "ledger_rows_added": ledger_rows,
        "suspect_lines": [{key: line[key] for key in ("run_id", "security_id", "rows", "rows_above_ceiling",
                                                        "max_stored_shares")} for line in decisions["suspect_lines"]],
        "already_corrected_runs": decisions["already_corrected_runs"],
    }
    if failures:
        raise RuntimeError("bars_unit_correction publication gate failed: " + "; ".join(failures))
    publish_validated_shadows(
        store_for(conn),
        tables=((BARS, BARS_NEXT), (LEDGER, LEDGER_NEXT)),
        before_swap=lambda: mark_published(conn, STAGE_NAME, run_key, receipt),
    )
    return receipt


STAGE = BatchStage(name=STAGE_NAME, plan=plan, prepare=prepare, build=build, finalize=finalize,
                   memory_limit="384MB", threads=1, discard=discard,
                   code_dependencies=("migrations/bodies_0327.py", "_bulk_publication.py"))
register_stage(STAGE)
