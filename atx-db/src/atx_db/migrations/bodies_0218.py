"""Point-in-time four-quarter change in quarterly ROE."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT four-quarter change in ROE v1"
FACTOR_ID = "profitability_q_factor_delta_roe"
QUARTERLY_ROE_FACTOR_ID = "profitability_q_factor_roe"


def _delta_roe(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT four-quarter change in quarterly ROE',
            'fundamental_profitability_growth',
            'Current point-in-time quarterly ROE minus the same fiscal quarter approximately one year earlier, following the dROE signal used by the q5 expected-growth model.',
            'quarterly_roe_t-quarterly_roe_t_minus_4',
            '["factor:profitability_q_factor_roe"]',
            1,600,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Both governed quarterly-ROE decisions must be visible at the current monthly close and their earnings period ends must be 300-430 days apart.',
            'atx_db.delta_roe','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"period_gap_days":[300,430],"complete_case":true,"return_fitted_parameters":false}',
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
        ) VALUES (sha256(concat_ws('|','delta_roe',?,?)),?,
                  'factor',?,?,NULL,NULL,1,?,600,true,?)
        """,
        [
            FACTOR_ID,
            QUARTERLY_ROE_FACTOR_ID,
            FACTOR_ID,
            QUARTERLY_ROE_FACTOR_ID,
            QUARTERLY_ROE_FACTOR_ID,
            f"factor:{QUARTERLY_ROE_FACTOR_ID}",
            SOURCE,
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=218, name="four_quarter_delta_roe", up=_delta_roe)]
