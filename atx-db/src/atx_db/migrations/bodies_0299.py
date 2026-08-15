"""Coverage SLO seeding for the restatement-events public schema."""

from __future__ import annotations

import duckdb

from ..provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS
from ._runner import Migration


def _restatement_coverage_slo(conn: duckdb.DuckDBPyConnection) -> None:
    conn.executemany(
        """
        INSERT OR REPLACE INTO api_schema_coverage_slo (
            dataset_id,schema_code,slo_version,expected_history_start,
            minimum_history_years,minimum_security_count,minimum_item_count,
            maximum_freshness_lag_days,citation,description,is_active,
            valid_from,valid_to,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,true,TIMESTAMP '1900-01-01',NULL,now())
        """,
        [
            (
                slo.dataset_id,
                slo.schema_code,
                slo.slo_version,
                slo.expected_history_start,
                slo.minimum_history_years,
                slo.minimum_security_count,
                slo.minimum_item_count,
                slo.maximum_freshness_lag_days,
                slo.citation,
                slo.description,
            )
            for slo in DEFAULT_PROVIDER_COVERAGE_SLOS
        ],
    )


MIGRATIONS = [
    Migration(
        version=299,
        name="restatement_coverage_slo",
        up=_restatement_coverage_slo,
    )
]
