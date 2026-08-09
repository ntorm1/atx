"""First-filed standardized unexpected quarterly revenue factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT standardized unexpected revenue v1"
FACTOR_ID = "earnings_standardized_unexpected_revenue"


def _revenue_surprise_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT standardized unexpected quarterly revenue','fundamental_earnings',
            'First-filed USD quarterly revenue seasonal change less its prior eight-change mean, scaled by prior eight-change volatility.',
            '((revenue_q-revenue_q_minus_4)-mean_prior_8_changes)/std_prior_8_changes',
            '["metric:revenue_quarterly"]',1,150,
            '{"method":"none","by":[]}','standardized_surprise',
            'higher_is_better','zscore',true,
            'Visible at a monthly close only after the initial SEC filing; later comparative restatements are excluded.',
            'atx_db.revenue_surprise','atx-db',?,
            '{"method":"seasonal_random_walk_with_drift_then_winsorize_zscore_cs","history":8,"winsor_limits":[0.01,0.01],"currency":"USD"}',
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
        ) VALUES (sha256(concat_ws('|','surge',?,'revenue_quarterly')),?,
                  'metric','revenue_quarterly',NULL,'revenue_quarterly',NULL,
                  1,'metric:revenue_quarterly',150,true,?)
        """,
        [FACTOR_ID, FACTOR_ID, SOURCE],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=205, name="standardized_unexpected_revenue_factor", up=_revenue_surprise_factor)]
