"""Govern the PIT eight-quarter trend in quarterly gross profitability."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT eight-quarter gross profitability trend v1"
FACTOR_ID = "profitability_quarterly_gross_profitability_trend_8q"
QGP_FACTOR_ID = "profitability_quarterly_gross_profitability_lagged_assets"


def _profitability_trend(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT eight-quarter gross profitability trend','fundamental_profitability',
            'Seasonally adjusted OLS slope in governed quarterly gross profitability over the latest eight distinct fiscal quarters.',
            'ols_slope_8obs(gross_profit/lagged_assets ~ elapsed_quarters + calendar_quarter)',
            '["factor:profitability_quarterly_gross_profitability_lagged_assets"]',
            1,760,'{"method":"none","by":[]}','profitability_change_per_quarter',
            'higher_is_better','zscore',true,
            'All eight parent observations must be visible by the monthly decision timestamp; one observation per distinct fiscal period is retained.',
            'atx_db.profitability_trend','atx-db',?,
            '{"method":"winsorize_zscore_cs","observations":8,"seasonal_fixed_effects":true,"minimum_seasonal_quarters":3,"elapsed_time":true,"history_span_days":[580,1000],"maximum_absolute_trend":1.0,"winsor_limits":[0.01,0.01],"return_fitted_parameters":false}',
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
        ) VALUES (sha256(concat_ws('|','profitability_trend',?,?,?)),
                  ?,'factor',?,?,NULL,NULL,1,?,760,true,?)
        """,
        [
            FACTOR_ID,
            "factor",
            QGP_FACTOR_ID,
            FACTOR_ID,
            QGP_FACTOR_ID,
            QGP_FACTOR_ID,
            f"factor:{QGP_FACTOR_ID}",
            SOURCE,
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=259, name="profitability_trend", up=_profitability_trend)]
