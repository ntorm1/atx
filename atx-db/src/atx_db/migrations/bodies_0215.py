"""Point-in-time q-factor-style quarterly ROE profitability."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT q-factor ROE v1"
FACTOR_ID = "profitability_q_factor_roe"
METRICS = ("net_income", "stockholders_equity")


def _quarterly_roe(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT q-factor quarterly ROE','fundamental_profitability',
            'Latest announced single-quarter net income divided by one-quarter-lagged positive stockholders equity. This is an explicit point-in-time adaptation of the Hou-Xue-Zhang ROE factor.',
            'quarterly_net_income/one_quarter_lagged_stockholders_equity',
            '["metric:net_income","metric:stockholders_equity","universe:us_common_equity_liquid_v1"]',
            1,200,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'The latest 70-115 day earnings period and a positive equity observation 60-130 days earlier must both be visible at the governed monthly close; earnings may be at most 200 days old.',
            'atx_db.quarterly_roe','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"quarter_duration_days":[70,115],"lagged_equity_gap_days":[60,130],"numerator_adaptation":"reported_net_income","denominator_adaptation":"stockholders_equity","return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for metric in METRICS:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','quarterly_roe',?,?)),?,
                      'metric',?,NULL,?,NULL,1,?,200,true,?)
            """,
            [FACTOR_ID, metric, FACTOR_ID, metric, metric, f"metric:{metric}", SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=215, name="quarterly_q_factor_roe", up=_quarterly_roe)]
