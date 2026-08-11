"""Govern same-quarter year-over-year revenue growth."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly revenue growth v1"
FACTOR_ID = "growth_quarterly_revenue_yoy"
QOP_FACTOR_ID = "profitability_quarterly_operating_profitability_lagged_assets"


def _quarterly_revenue_growth(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT same-quarter year-over-year revenue growth','fundamental_growth',
            'Same-quarter year-over-year quarterly revenue growth from governed PIT statements.',
            'revenue_t/revenue_t_4-1',
            '["factor:profitability_quarterly_operating_profitability_lagged_assets"]',
            1,500,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Uses the closest governed prior quarterly report 330-400 days earlier; both reports must be visible by the current monthly decision.',
            'atx_db.quarterly_revenue_growth','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"period_gap_days":[330,400],"maximum_absolute_raw_value":10.0,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (
            sha256(concat_ws('|','quarterly_revenue_growth',?,?)),?,
            'factor',?,?,NULL,NULL,1,?,500,true,?
        )
        """,
        [
            FACTOR_ID,
            QOP_FACTOR_ID,
            FACTOR_ID,
            QOP_FACTOR_ID,
            QOP_FACTOR_ID,
            f"factor:{QOP_FACTOR_ID}",
            SOURCE,
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=231, name="quarterly_revenue_growth", up=_quarterly_revenue_growth)
]
