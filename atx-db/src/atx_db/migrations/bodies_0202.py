"""First-filed standardized unexpected diluted EPS factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT standardized unexpected EPS v1"
FACTOR_ID = "earnings_standardized_unexpected_eps"


def _earnings_surprise_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT standardized unexpected diluted EPS','fundamental_earnings',
            'First-filed quarterly diluted EPS less first-filed EPS from the same quarter one year earlier, scaled by prior seasonal-change volatility.',
            '(eps_q-eps_q_minus_4)/std_prior_20_seasonal_eps_changes',
            '["metric:eps_diluted_quarterly"]',1,150,
            '{"method":"none","by":[]}','standardized_surprise','higher_is_better','zscore',
            true,'Visible at a monthly close only after the initial SEC filing; later comparative restatements are excluded.',
            'atx_db.earnings_surprise','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"history_min":4,"history_max":20}',
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
        ) VALUES (sha256(concat_ws('|','sue',?,'eps_diluted_quarterly')),?,
                  'metric','eps_diluted_quarterly',NULL,'eps_diluted_quarterly',NULL,
                  1,'metric:eps_diluted_quarterly',150,true,?)
        """,
        [FACTOR_ID, FACTOR_ID, SOURCE],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=202, name="standardized_unexpected_eps_factor", up=_earnings_surprise_factor)]
