#!/usr/bin/env python
"""Run bounded integrity checks for predicted-announcement earnings seasonality."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import duckdb

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH
from atx_db.earnings_seasonality import FACTOR_ID, SOURCE_NAME
from atx_db.migrations import verify_migration_checksums


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    args = parser.parse_args()
    con = duckdb.connect(str(args.db_path))
    try:
        verify_migration_checksums(con)
        audit = con.execute(
            """
            SELECT
                count(*) AS rows,
                count(DISTINCT factor_value_id) AS ids,
                count(DISTINCT security_id || '|' || CAST(as_of_date AS VARCHAR)) AS keys,
                count_if(value IS NULL OR NOT isfinite(value)) AS bad_values,
                count_if(input_lineage_json IS NULL) AS missing_lineage,
                count_if(available_at > CAST(as_of_date AS TIMESTAMP) + INTERVAL 1 DAY) AS late_outputs,
                count_if(TRY_CAST(json_extract_string(
                    input_lineage_json,'$.anchor.available_at'
                ) AS TIMESTAMP) > available_at) AS late_anchor,
                count_if(TRY_CAST(json_extract_string(
                    input_lineage_json,'$.latest_history_available_at'
                ) AS TIMESTAMP) > available_at) AS late_history,
                count_if(TRY_CAST(json_extract_string(
                    input_lineage_json,'$.formation.price_available_at'
                ) AS TIMESTAMP) > available_at) AS late_price,
                count_if(json_array_length(json_extract(input_lineage_json,'$.history'))<>20)
                    AS invalid_history_count,
                count_if(TRY_CAST(json_extract_string(
                    input_lineage_json,'$.same_quarter_observations'
                ) AS INTEGER)<>5) AS invalid_same_quarter_count,
                count_if(date_trunc('month',as_of_date+INTERVAL 1 MONTH)
                    <>TRY_CAST(json_extract_string(
                        input_lineage_json,'$.formation.predicted_announcement_month'
                    ) AS DATE)) AS invalid_prediction_month
            FROM fundamental_factor_values
            WHERE factor_id=? AND source=?
            """,
            [FACTOR_ID, SOURCE_NAME],
        ).fetchone()
        version = con.execute(
            """
            SELECT max(CAST(version AS INTEGER))
            FROM schema_migrations WHERE regexp_matches(version,'^[0-9]+$')
            """
        ).fetchone()[0]
    finally:
        con.close()
    fields = (
        "rows", "ids", "keys", "bad_values", "missing_lineage", "late_outputs",
        "late_anchor", "late_history", "late_price", "invalid_history_count",
        "invalid_same_quarter_count", "invalid_prediction_month",
    )
    result = {
        "migration": version,
        "checksums_verified": True,
        **dict(zip(fields, audit, strict=True)),
    }
    print(json.dumps(result, indent=2))
    invalid = result["rows"] != result["ids"] or result["rows"] != result["keys"]
    invalid = invalid or any(result[field] for field in fields[3:])
    return 1 if invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
