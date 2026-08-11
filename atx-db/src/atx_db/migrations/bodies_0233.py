"""Govern revenue growth confirmed by non-declining gross margin."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly revenue margin confirmation v1"
FACTOR_ID = "growth_quarterly_revenue_margin_confirmation"
REVENUE_GROWTH_FACTOR_ID = "growth_quarterly_revenue_yoy"
QOP_FACTOR_ID = "profitability_quarterly_operating_profitability_lagged_assets"


def _quarterly_revenue_margin_confirmation(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT revenue growth with non-declining gross margin','fundamental_growth',
            'Same-quarter revenue-growth score retained only when gross margin does not decline year over year.',
            'zscore(revenue_growth_score | gross_margin_t-gross_margin_t_4>=0)',
            '["factor:growth_quarterly_revenue_yoy","factor:profitability_quarterly_operating_profitability_lagged_assets"]',
            1,500,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Gross margins are reconstructed from the exact current/prior QOP rows already governed in revenue-growth lineage.',
            'atx_db.quarterly_revenue_margin_confirmation','atx-db',?,
            '{"method":"nondeclining_gross_margin_gate_then_zscore_revenue_growth","maximum_absolute_gross_margin":5.0,"return_fitted_parameters":false}',
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
            ) VALUES (sha256(concat_ws('|','revenue_margin_confirmation',?,?)),?,
                'factor',?,?,NULL,NULL,1,?,500,true,?)
            """,
            [FACTOR_ID, dependency_factor_id, FACTOR_ID, dependency_factor_id,
             dependency_factor_id, f"factor:{dependency_factor_id}", SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=233, name="quarterly_revenue_margin_confirmation", up=_quarterly_revenue_margin_confirmation)]
