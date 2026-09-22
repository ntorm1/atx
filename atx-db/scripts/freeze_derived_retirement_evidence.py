"""Freeze genuine legacy outputs from an explicitly supplied pre-retirement source export.

Run with the repository venv and --baseline-root pointing at an exported atx-db
directory from commit 5b11a272. All imports resolve there; outputs are written to
the active checkout's tests/data. Uses a temporary warehouse, never the live DB.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import importlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-root", type=Path, required=True)
    parser.add_argument("--baseline-commit", default="5b11a272")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    baseline = args.baseline_root.resolve()
    source = baseline / "src"
    if not (source / "atx_db" / "annual_margin_change.py").is_file():
        parser.error("--baseline-root must contain the pre-retirement atx-db source")
    sys.path.insert(0, str(source))
    sys.path.insert(1, str(project / "tests"))
    import derived_retirement_fixtures as fixture

    from atx_db.connection import DuckDBStore

    assert Path(importlib.import_module("atx_db").__file__).resolve().is_relative_to(source)
    # Prove the named baseline against committed Git blobs, not a mutable label.
    checked_sources = (*fixture.MODULE_FACTORS, *fixture.RETAINED_PARENTS,
                       "enterprise_value", "valuation_multiples", "quarterly_revenue_margin_confirmation")
    for name in checked_sources:
        relative = f"atx-db/src/atx_db/{name}.py"
        committed = subprocess.check_output(["git", "show", f"{args.baseline_commit}:{relative}"], cwd=project)
        actual = (source / "atx_db" / f"{name}.py").read_bytes()
        if committed.replace(b"\r\n", b"\n") != actual.replace(b"\r\n", b"\n"):
            raise ValueError(f"baseline source differs from {args.baseline_commit}: {name}")
    commit = subprocess.check_output(
        ["git", "rev-parse", args.baseline_commit], cwd=project, text=True,
    ).strip()
    tables = fixture.build_retirement_fact_tables()
    fixture.validate_retirement_fact_tables(tables)
    base_input_sha256 = fixture.digest(tables)
    print("source input rows: " + str({name: len(rows) for name, rows in tables.items()}), flush=True)
    with tempfile.TemporaryDirectory(prefix="atx-retirement-freeze-") as temporary:
        store = DuckDBStore(Path(temporary) / "evidence.duckdb")
        store.analytical_threads = 1
        with store:
            store.con.execute("PRAGMA threads=1")
            fixture.write_retirement_facts(store)
            mismatches = store.con.execute("""
                SELECT count(*) FROM fundamental_standardized s
                FULL OUTER JOIN fundamental_statement_points p
                  ON p.statement_point_id = s.standardized_id
                WHERE s.value IS DISTINCT FROM p.value
                   OR s.available_at IS DISTINCT FROM p.available_at
                   OR s.period_end IS DISTINCT FROM p.period_end
                   OR s.canonical_code IS DISTINCT FROM p.canonical_metric
                """).fetchone()[0]
            assert mismatches == 0, "both source layers must contain identical facts"
            for table in ("market_cap", "enterprise_value"):
                frame = store.con.execute(f"SELECT * EXCLUDE(source_loaded_at,updated_at) FROM {table} ORDER BY 1").df()
                # Preserve exact floats while mapping Pandas NaT/NaN to JSON null.
                pandas = importlib.import_module("pandas")
                tables[table] = [{column: None if pandas.isna(value) else
                                 value.isoformat() if hasattr(value, "isoformat") else value
                                 for column, value in row.items()} for row in frame.to_dict(orient="records")]
                print(f"genuine denominator {table}: {len(frame)}", flush=True)
            rows = fixture.legacy_retirement_rows(store)
            # Keep Python's exact round-trippable float repr, not pandas' rounded JSON.
            records = [{column: value.isoformat() if hasattr(value, "isoformat") else value
                        for column, value in row.items()} for row in rows.to_dict(orient="records")]
            counts = {
                factor: {"rows": len(group), "securities": int(group.security_id.nunique()),
                         "dates": int(group.as_of_date.nunique()), "first_date": str(group.as_of_date.min()),
                         "last_date": str(group.as_of_date.max())}
                for factor, group in rows.groupby("factor_id")
            }
            assert len(counts) == 23
            assert all(item["securities"] >= 20 and item["rows"] > 0 for item in counts.values())
            assert rows.loc[rows.factor_id == "earnings_tax_to_book_income", "as_of_date"].min().year <= 2018
            assert set(rows.loc[rows.factor_id == "intangibles_large_rd_increase", "raw_value"]) == {0.0, 1.0}
            rates = store.con.execute("""
                SELECT DISTINCT cast(json_extract_string(input_lineage_json, '$.statutory_rate') AS DOUBLE)
                FROM fundamental_factor_values WHERE factor_id = 'earnings_tax_to_book_income'
                ORDER BY 1
                """).fetchall()
            assert rates == [(0.21,), (0.35,)], "both statutory-rate regimes must execute"
            options = fixture.baseline_options()
    hashes = {}
    for module in list(sys.modules.values()):
        filename = getattr(module, "__file__", None)
        if filename and Path(filename).resolve().is_relative_to(source / "atx_db"):
            path = Path(filename).resolve()
            hashes[path.relative_to(baseline).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    for path in sorted((source / "atx_db" / "seeds").glob("*.csv")):
        hashes[path.relative_to(baseline).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    metadata = {
        "format_version": 1, "baseline_commit": commit, "source_sha256": hashes,
        "baseline_verification": "Legacy, retained parent and denominator sources equal named Git blobs "
                                 "after CRLF-to-LF normalization; source_sha256 records exact executed bytes.",
        "fixture_source_sha256": hashlib.sha256(Path(fixture.__file__).read_bytes()).hexdigest(),
        "generator_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "input_sha256": fixture.digest(tables), "base_input_sha256": base_input_sha256,
        "output_sha256": fixture.digest(records),
        "serialization": "UTF-8 JSON; sorted object keys; compact separators; ISO dates/timestamps; "
                         "finite round-trip Python floats; rows sorted factor/security/date; gzip mtime=0",
        "factor_ids": list(fixture.FACTOR_IDS), "factor_counts": counts, "options": options,
        "parent_chain": list(fixture.RETAINED_PARENTS),
        "availability_policy": "Genuine legacy timestamps preserved verbatim, including unsafe per-name "
                               "timestamps preceding the complete standardized cohort. No correction applied.",
        "scenarios": ["independent quarterly flows and changing balances 2016-2023", "annual sums of actual quarters",
                      "real four-quarter TTM", "2017/2018 statutory tax boundary", "two-for-one share splits",
                      "late amendments to old annual periods", "missing tax/dividend/deferred-revenue components",
                      "annual and quarterly flow amendments with revised actual TTM vintages",
                      "negative income, zero revenue and zero assets observations", "low capitalization and ADV eligibility",
                      "reporting stops after 2019 and becomes stale", "universe entry and exit",
                      "varying per-name price availability within monthly cohorts"],
        "runtime": {"python": sys.version.split()[0], "pandas": importlib.import_module("pandas").__version__,
                    "duckdb": importlib.import_module("duckdb").__version__, "duckdb_threads": 1},
    }
    fixture.DATA_DIR.mkdir(parents=True, exist_ok=True)
    for name, payload in (
        ("inputs", {"metadata": metadata, "tables": tables}),
        ("expected", {"metadata": metadata, "rows": records}),
    ):
        path = fixture.DATA_DIR / f"derived_retirement_{name}.json.gz"
        path.write_bytes(gzip.compress(fixture.canonical_bytes(payload), compresslevel=9, mtime=0))
        print(f"wrote {path}: {path.stat().st_size} bytes", flush=True)
    print(json.dumps({"baseline_commit": commit, "input_sha256": metadata["input_sha256"],
                      "output_sha256": metadata["output_sha256"], "factor_counts": counts}, indent=2), flush=True)


if __name__ == "__main__":
    main()
