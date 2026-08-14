"""Govern the PIT quarterly asset-turnover trend."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT eight-observation quarterly asset-turnover trend v1"
FACTOR_ID = "efficiency_quarterly_asset_turnover_trend_8q"
TREND_GRID_FACTOR_ID = "profitability_quarterly_gross_profitability_trend_8q"
QGP_FACTOR_ID = "profitability_quarterly_gross_profitability_lagged_assets"


def _asset_turnover_trend(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT eight-observation quarterly asset-turnover trend',
            'fundamental_efficiency',
            'Seasonally adjusted elapsed-time OLS slope in quarterly revenue-to-lagged-assets over the governed eight-observation PIT trend grid.',
            'ols_slope_8obs(revenue/lagged_assets ~ elapsed_quarters + calendar_quarter)',
            '["factor:profitability_quarterly_gross_profitability_trend_8q","factor:profitability_quarterly_gross_profitability_lagged_assets"]',
            1,760,'{"method":"none","by":[]}','asset_turnover_change_per_quarter',
            'higher_is_better','zscore',true,
            'Every QGP parent observation must be visible by the governed monthly trend-grid decision timestamp.',
            'atx_db.asset_turnover_trend','atx-db',?,
            '{"method":"winsorize_zscore_cs","observations":8,"seasonal_fixed_effects":true,"minimum_seasonal_quarters":3,"elapsed_time":true,"maximum_absolute_asset_turnover":20.0,"maximum_absolute_trend":5.0,"winsor_limits":[0.01,0.01],"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    conn.executemany(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','asset_turnover_trend',?,?,?)),
                  ?,'factor',?,?,NULL,NULL,1,?,760,true,?)
        """,
        [
            (
                FACTOR_ID,"factor",TREND_GRID_FACTOR_ID,FACTOR_ID,
                TREND_GRID_FACTOR_ID,TREND_GRID_FACTOR_ID,
                f"factor:{TREND_GRID_FACTOR_ID}",SOURCE,
            ),
            (
                FACTOR_ID,"factor",QGP_FACTOR_ID,FACTOR_ID,
                QGP_FACTOR_ID,QGP_FACTOR_ID,f"factor:{QGP_FACTOR_ID}",SOURCE,
            ),
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=260, name="asset_turnover_trend", up=_asset_turnover_trend)
]
