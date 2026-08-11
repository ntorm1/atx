"""Govern same-quarter change in quarterly operating profitability."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly operating profitability change v1"
FACTOR_ID = "profitability_quarterly_operating_profitability_change_yoy"
REVENUE_GROWTH_FACTOR_ID = "growth_quarterly_revenue_yoy"
QOP_FACTOR_ID = "profitability_quarterly_operating_profitability_lagged_assets"


def _quarterly_profitability_change(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT same-quarter change in quarterly operating profitability',
            'fundamental_profitability',
            'Year-over-year change in governed quarterly operating profitability using exact same-quarter pairs.',
            'zscore(winsorize_1pct(qop_t-qop_t_4))',
            '["factor:growth_quarterly_revenue_yoy","factor:profitability_quarterly_operating_profitability_lagged_assets"]',
            1,500,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'The revenue-growth factor supplies the governed same-quarter pair; available_at is the maximum of the pair and both QOP rows.',
            'atx_db.quarterly_profitability_change','atx-db',?,
            '{"method":"cross_sectional_winsorize_then_zscore","maximum_absolute_change":10.0,"winsor_limits":[0.01,0.01],"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency_factor_id in (REVENUE_GROWTH_FACTOR_ID, QOP_FACTOR_ID):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','quarterly_profitability_change',?,?)),?,
                'factor',?,?,NULL,NULL,1,?,500,true,?)
            """,
            [
                FACTOR_ID,
                dependency_factor_id,
                FACTOR_ID,
                dependency_factor_id,
                dependency_factor_id,
                f"factor:{dependency_factor_id}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=234,
        name="quarterly_operating_profitability_change",
        up=_quarterly_profitability_change,
    )
]
