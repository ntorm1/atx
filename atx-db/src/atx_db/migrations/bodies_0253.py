"""Govern the joint cash-profitability and conservative-investment factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT profitability-investment composite v1"
FACTOR_ID = "composite_cash_profitability_conservative_investment"
CASH_FACTOR_ID = "profitability_cash_operating_profitability"
INVESTMENT_FACTOR_ID = "investment_conservative_asset_growth"


def _profitability_investment(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT cash profitability and conservative investment',
            'fundamental_composite',
            'Untuned equal-weight blend of cash operating profitability and negative asset growth, motivated by the investment CAPM.',
            '0.5*z(profitability_cash_operating_profitability)+0.5*z(investment_conservative_asset_growth)',
            '["factor:profitability_cash_operating_profitability","factor:investment_conservative_asset_growth"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Both governed PIT factor values must exist on the same security and formation date; weights are fixed before return inspection.',
            'atx_db.profitability_investment','atx-db',?,
            '{"method":"equal_weight_then_winsorize_zscore_cs","weights":{"profitability_cash_operating_profitability":0.5,"investment_conservative_asset_growth":0.5},"winsor_limits":[0.01,0.01],"weights_fitted_to_returns":false,"missing_components_imputed":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency in (CASH_FACTOR_ID, INVESTMENT_FACTOR_ID):
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','profitability_investment',?,?)),
                      ?,'factor',?,?,NULL,NULL,1,?,550,true,?)""",
            [
                FACTOR_ID,
                dependency,
                FACTOR_ID,
                dependency,
                dependency,
                f"factor:{dependency}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=253,
        name="profitability_conservative_investment_composite",
        up=_profitability_investment,
    )
]
