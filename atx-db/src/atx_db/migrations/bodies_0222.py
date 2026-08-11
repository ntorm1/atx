"""Reject implausibly scaled quarterly operating-profitability observations."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly operating profitability v1"
FACTOR_ID = "profitability_quarterly_operating_profitability_lagged_assets"


def _quarterly_profitability_scale_guard(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        UPDATE factor_definition
        SET description = 'Latest SEC-visible quarterly revenue minus COGS minus SG&A plus R&D, scaled by one-quarter-lagged total assets; implausible absolute raw ratios above five are rejected as unit-scale errors.',
            standardization_spec_json = '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"quarter_duration_days":[70,115],"lagged_assets_gap_days":[60,130],"gross_profit_fallback":"reported_gross_profit","missing_rd_expense":"zero","maximum_absolute_raw_value":5.0,"return_fitted_parameters":false}',
            updated_at = now()
        WHERE factor_id = ? AND source = ?
        """,
        [FACTOR_ID, SOURCE],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=222,
        name="quarterly_operating_profitability_scale_guard",
        up=_quarterly_profitability_scale_guard,
    )
]
