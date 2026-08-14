"""Govern persistence-weighted earnings surprise."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT earnings-persistence-weighted SUE v1"
FACTOR_ID = "earnings_sue_low_volatility_persistence"
SUE_FACTOR_ID = "earnings_standardized_unexpected_eps"


def _earnings_persistence(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT earnings-persistence-weighted SUE','fundamental_earnings',
            'Standardized unexpected earnings direction scaled continuously by the cross-sectional percentile of low ex-ante seasonal earnings-change volatility.',
            '(2*rank_cs(SUE)-1)*rank_cs(-prior_seasonal_change_std)',
            '["factor:earnings_standardized_unexpected_eps"]',
            1,2200,'{"method":"cross_sectional_rank_scaling","hard_filter":false}',
            'persistence_weighted_rank','higher_is_better','zscore',true,
            'Inherits the governed SUE availability; volatility uses only seasonal earnings changes strictly preceding the current surprise.',
            'atx_db.earnings_persistence','atx-db',?,
            '{"method":"sue_rank_times_low_volatility_percentile_then_winsorize_zscore_cs","return_fitted_parameters":false,"missing_volatility_policy":"drop"}',
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
        ) VALUES (sha256(concat_ws('|','earnings_persistence',?,?,?)),
                  ?,'factor',?,?,NULL,NULL,1,?,2200,true,?)
        """,
        [
            FACTOR_ID, "factor", SUE_FACTOR_ID, FACTOR_ID, SUE_FACTOR_ID,
            SUE_FACTOR_ID, f"factor:{SUE_FACTOR_ID}", SOURCE,
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=265, name="earnings_persistence_weighted_sue", up=_earnings_persistence)]
