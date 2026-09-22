"""Coverage SLO seeding for the derived-metrics / market-daily-1d public schemas.

Seeds ``api_schema_coverage_slo`` rows for the two schema codes T9 registers in
``api.catalog`` (``derived-metrics`` on ``ATX.US.FUNDAMENTALS``,
``market-daily-1d`` on ``ATX.US.EQUITIES``). Without an active SLO row,
``provider_coverage.refresh_provider_coverage`` raises ``RuntimeError`` the
moment it iterates either schema in ``api.catalog.DATASETS`` -- see
``_active_slo``.
"""

from __future__ import annotations

import duckdb

from ..provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS
from ._runner import Migration

_NEW_SCHEMA_CODES = frozenset({"derived-metrics", "market-daily-1d"})


def _derived_and_market_daily_coverage_slo(conn: duckdb.DuckDBPyConnection) -> None:
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
            if slo.schema_code in _NEW_SCHEMA_CODES
        ],
    )


MIGRATIONS = [
    Migration(
        version=303,
        name="derived_and_market_daily_coverage_slo",
        up=_derived_and_market_daily_coverage_slo,
    )
]
