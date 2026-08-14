"""Govern first-filed, price-deflated quarterly earnings acceleration."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT price-deflated earnings acceleration v1"
FACTOR_ID = "earnings_quarterly_acceleration"
SUE_FACTOR_ID = "earnings_standardized_unexpected_eps"


def _earnings_acceleration(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT quarterly earnings acceleration','fundamental_earnings',
            'Quarter-over-quarter change in seasonally differenced diluted EPS growth, deflated by prior-quarter split-adjusted prices.',
            '((eps_t-eps_t_4)/price_t_1)-((eps_t_1-eps_t_5)/price_t_2)',
            '["factor:earnings_standardized_unexpected_eps","metric:eps_diluted_quarterly","market:close"]',
            1,610,'{"method":"none","by":[]}','price_deflated_eps_change',
            'higher_is_better','zscore',true,
            'First-filed quarterly EPS t, t-1, t-4, and t-5 plus period-end prices t-1 and t-2 must all be visible; monthly formation uses the governed SUE decision grid.',
            'atx_db.earnings_acceleration','atx-db',?,
            '{"method":"winsorize_zscore_cs","revision_policy":"first_filed_period_value_only","seasonal_gap_days":[350,380],"adjacent_gap_days":[60,130],"price_deflator_lags":[1,2],"split_adjusted":true,"maximum_signal_age_days":150,"maximum_absolute_acceleration":10.0,"winsor_limits":[0.01,0.01],"return_fitted_parameters":false}',
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
        ) VALUES (sha256(concat_ws('|','earnings_acceleration',?,?,?)),
                  ?,?,?,?,?,?,1,?,610,true,?)
        """,
        [
            (
                FACTOR_ID, "factor", SUE_FACTOR_ID, FACTOR_ID, "factor", SUE_FACTOR_ID,
                SUE_FACTOR_ID, None, None, f"factor:{SUE_FACTOR_ID}", SOURCE,
            ),
            (
                FACTOR_ID, "metric", "eps_diluted_quarterly", FACTOR_ID, "metric",
                "eps_diluted_quarterly", None, "eps_diluted_quarterly", None,
                "metric:eps_diluted_quarterly", SOURCE,
            ),
            (
                FACTOR_ID, "market", "close", FACTOR_ID, "market", "close", None,
                None, None, "market:close", SOURCE,
            ),
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=258, name="earnings_acceleration", up=_earnings_acceleration)
]
