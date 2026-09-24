"""Read a bounded desk screen from one validated default FQ1 build."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

MAX_ROWS = 1000
MAX_BYTES = 2_000_000
SQL_PATH = Path(__file__).resolve().parents[1] / "sql/research/fundamental-desk-screen-acceptance.sql"


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--db-path", type=Path, required=True)
    p.add_argument("--build-run-id", required=True)
    p.add_argument("--report-as-of", type=dt.date.fromisoformat, required=True)
    p.add_argument("--output-json", type=Path, required=True)
    return p


def _bounded_rows(con, sql: str, run_id: str, report_as_of: dt.date):
    """Run the SQL only after an in-database byte and row preflight."""
    # Bound the bytes in DuckDB before transferring any JSON payload to
    # Python. The outer aggregate returns only two integers.
    statement = sql.strip().removesuffix(";")
    row_count, payload_bytes = con.execute(
        "SELECT count(*),coalesce(sum(octet_length(encode(CAST(payload AS VARCHAR)))),0) "
        f"FROM ({statement}) AS desk_rows",
        [run_id, report_as_of],
    ).fetchone()
    if row_count > MAX_ROWS or payload_bytes > MAX_BYTES:
        raise ValueError("desk screen exceeded row or transfer byte cap")
    cursor = con.execute(sql, [run_id, report_as_of])
    rows = cursor.fetchmany(MAX_ROWS + 1)
    if len(rows) > MAX_ROWS or cursor.fetchmany(1):
        raise ValueError("desk screen exceeded 1000-row result cap")
    return rows


def read_screen(con, run_id: str, report_as_of: dt.date, sql: str,
                validator, default_specs) -> dict:
    """Validate first, then read exactly one bounded result in this transaction."""
    verified = validator(con, run_id)
    if verified.run_id != run_id or verified.specs != default_specs:
        raise ValueError("build does not contain exactly the frozen default FQ1 signals")
    manifest = con.execute(
        "SELECT as_of_date,spec_json,spec_sha256,panel_sha256 "
        "FROM fundamental_signal_runs WHERE run_id=?",
        [run_id],
    ).fetchone()
    if manifest is None or manifest[3] != verified.panel_sha256:
        raise ValueError("validated build manifest changed")
    build_as_of, spec_json, spec_sha, panel_sha = manifest
    if build_as_of > report_as_of:
        raise ValueError("build snapshot is later than report as-of date")
    try:
        age = json.loads(spec_json)["max_age_days"]
    except (TypeError, KeyError, json.JSONDecodeError) as exc:
        raise ValueError("malformed build freshness contract") from exc
    if type(age) is not int or age != 200:
        raise ValueError("desk screen requires the default 200-day accounting freshness")
    if len(verified.decision_dates) == 0:
        raise ValueError("validated build has no observed decision session")

    rows = _bounded_rows(con, sql, run_id, report_as_of)
    if not rows or rows[0][0] != "diagnostic" or any(
        kind != "name" for kind, _ in rows[1:]
    ):
        raise ValueError("desk screen result contract changed")
    diagnostic = json.loads(rows[0][1])
    if diagnostic["build_run_id"] != run_id or diagnostic["report_as_of"] != report_as_of.isoformat():
        raise ValueError("desk screen selection changed")
    expected_day = max((day for day in verified.decision_dates if day <= report_as_of),
                       default=None)
    if expected_day is None:
        raise ValueError("no observed decision session by report as-of date")
    if diagnostic["decision_date"] != expected_day.isoformat():
        raise ValueError("desk screen selected an unexpected decision session")
    try:
        cutoff = dt.datetime.fromisoformat(diagnostic["decision_cutoff_utc"])
    except (TypeError, ValueError) as exc:
        raise ValueError("desk screen decision cutoff is malformed") from exc
    if cutoff != dt.datetime.combine(expected_day, dt.time(22, 0)):
        raise ValueError("desk screen decision cutoff changed")
    names = [json.loads(payload) for _, payload in rows[1:]]
    outcome = {
        "contract": "fundamental-desk-screen-v2",
        "validation": "completed_default_fq1_panel_digest_verified",
        "build_run_id": run_id,
        "build_as_of": build_as_of.isoformat(),
        "report_as_of": report_as_of.isoformat(),
        "decision_date": expected_day.isoformat(),
        "decision_cutoff_utc": diagnostic["decision_cutoff_utc"],
        "sql_sha256": hashlib.sha256(sql.encode("utf-8")).hexdigest(),
        "build_spec_sha256": spec_sha,
        "panel_sha256": panel_sha,
        "calendar_sha256": verified.calendar_sha256,
        "source_limits": {
            "accounting": "FQ1 selected-input proof; modeled filing availability",
            "market": "same-session visible daily row; no independent price lineage certification",
            "interpretation": "descriptive screen; no alpha or later weekend disclosure claim",
        },
        "limits": {"max_rows": MAX_ROWS, "max_preview_names": MAX_ROWS - 1,
                   "max_result_bytes": MAX_BYTES},
        "diagnostic": diagnostic,
        "preview": names,
        "preview_truncated": diagnostic["cohort_rows"] > len(names),
    }
    encoded = json.dumps(outcome, sort_keys=True, separators=(",", ":"), allow_nan=False)
    if len(encoded.encode("utf-8")) > MAX_BYTES:
        raise ValueError("desk screen exceeded result byte cap")
    return outcome


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    db_path, output = args.db_path.resolve(), args.output_json.resolve()
    if not db_path.is_file():
        parser().error("--db-path must identify an existing warehouse file")
    if output == db_path or output.exists() or output.suffix.lower() != ".json":
        parser().error("--output-json must be a fresh .json path distinct from the warehouse")
    if not output.parent.is_dir():
        parser().error("--output-json parent directory must already exist")
    sql = SQL_PATH.read_text(encoding="utf-8")
    import duckdb

    from atx_db.fundamental_signal_research import (
        canonical_signals,
        default_signals,
        validate_fundamental_signal_panel,
    )

    try:
        with (
            tempfile.TemporaryDirectory(
                prefix=".fundamental-desk-spill-", dir=db_path.parent
            ) as spill,
            duckdb.connect(str(db_path), read_only=True, config={
                "memory_limit": "256MB", "threads": "1",
                "preserve_insertion_order": "false",
                "temp_directory": spill, "max_temp_directory_size": "2GB",
            }) as con,
        ):
            con.execute("SET enable_external_access = false")
            con.execute("SET TimeZone = 'UTC'")
            con.execute("BEGIN TRANSACTION")
            try:
                result = read_screen(
                    con, args.build_run_id, args.report_as_of, sql,
                    validate_fundamental_signal_panel,
                    canonical_signals(default_signals()),
                )
                con.execute("COMMIT")
            except Exception:
                con.execute("ROLLBACK")
                raise
        encoded = json.dumps(result, sort_keys=True, indent=2, allow_nan=False) + "\n"
        if len(encoded.encode("utf-8")) > MAX_BYTES:
            raise ValueError("desk screen exceeded result byte cap")
        # A hard link installs the complete file only if the target is still
        # absent. A failed read/validation never creates a report path.
        fd, temporary = tempfile.mkstemp(prefix=".fundamental-desk-", suffix=".json",
                                          dir=output.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as sink:
                sink.write(encoded)
                sink.flush()
                os.fsync(sink.fileno())
            os.link(temporary, output)
        finally:
            os.unlink(temporary)
    except (ValueError, RuntimeError, OSError, duckdb.Error) as exc:
        print(f"fundamental desk screen unavailable: {exc}", file=sys.stderr)
        return 2
    print(str(output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
