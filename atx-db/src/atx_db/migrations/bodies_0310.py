"""Retirement wave 2: deprecate legacy valuation surfaces and close two factors."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

DEPRECATED_TABLES: tuple[str, ...] = ("enterprise_value", "market_cap", "valuation_multiples")
RETIRED_FACTOR_IDS: tuple[str, ...] = (
    "investment_low_abnormal_capex",
    "risk_operating_leverage",
)
DEPRECATION_PREFIX = "[DEPRECATED 2026-09-19; superseded by market_daily_metrics] "
RETIREMENT_DATE = "2026-09-19"


def _retire_valuation_surfaces(conn: duckdb.DuckDBPyConnection) -> None:
    # Keep every legacy table and row; change only catalog and scheduler metadata.
    for table_name in DEPRECATED_TABLES:
        conn.execute(
            """
            UPDATE table_catalog
            SET description = ? || coalesce(description, ''), updated_at = now()
            WHERE table_name = ?
              AND NOT starts_with(coalesce(description, ''), ?)
            """,
            [DEPRECATION_PREFIX, table_name, DEPRECATION_PREFIX],
        )
        conn.execute(
            """
            UPDATE dataset_catalog
            SET description = ? || coalesce(description, ''),
                metadata_json = json_merge_patch(
                    coalesce(metadata_json, '{}'),
                    '{"deprecated":true,"superseded_by":"market_daily_metrics"}'
                ),
                updated_at = now()
            WHERE dataset_id = ?
              AND NOT starts_with(coalesce(description, ''), ?)
            """,
            [DEPRECATION_PREFIX, table_name, DEPRECATION_PREFIX],
        )
        # Existing installations may already have the removed default jobs seeded.
        conn.execute(
            """
            UPDATE etl_job_definitions
            SET enabled = false, updated_at = now()
            WHERE dataset_id = ? AND enabled
            """,
            [table_name],
        )
    placeholders = ", ".join("?" for _ in RETIRED_FACTOR_IDS)
    conn.execute(
        f"""
        UPDATE factor_definition
        SET valid_to = DATE '{RETIREMENT_DATE}', declared_in = 'retired', updated_at = now()
        WHERE factor_id IN ({placeholders}) AND valid_to IS NULL
        """,
        list(RETIRED_FACTOR_IDS),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=310, name="retire_valuation_surfaces", up=_retire_valuation_surfaces)
]
