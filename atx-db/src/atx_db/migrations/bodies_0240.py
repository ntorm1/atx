"""Govern PIT low abnormal capital-investment factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT abnormal capital investment v1"
FACTOR_ID = "investment_low_abnormal_capex"


def _low_abnormal_capex(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low abnormal capital investment','fundamental_investment',
            'Negative-oriented current TTM capital expenditure relative to the mean of exact PIT one-, two-, and three-year-lag TTM observations.',
            'zscore(winsorize_1pct(1-current_capex/mean(prior_1y,prior_2y,prior_3y)))',
            '["dataset:market_cap","metric:capital_expenditures","universe:us_common_equity_liquid_v1"]',
            1,1296,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Current and all three historical TTM capex observations plus universe membership must be visible by the monthly market-cap close; missing years are not imputed.',
            'atx_db.abnormal_capex','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"maximum_current_age_days":200,"annual_lags":[1,2,3],"lag_tolerance_days":100,"maximum_absolute_abnormal_ratio":10.0,"minimum_names_per_date":20,"monthly_sampling":"last_eligible_market_cap_date","negative_signed_capex_required":true,"missing_lags_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID,SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?",[FACTOR_ID])
    dependencies = (
        ("dataset","market_cap",None,None,"market_cap","market_cap"),
        ("metric","capital_expenditures",None,"capital_expenditures",None,"capital_expenditures"),
        ("universe","us_common_equity_liquid_v1",None,None,None,"us_common_equity_liquid_v1"),
    )
    for dependency_type,dependency_name,dependency_factor_id,dependency_metric_id,dependency_source_id,expression in dependencies:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (
                sha256(concat_ws('|','low_abnormal_capex',?,?,?)),
                ?,?,?,?,?,?,1,?,1296,true,?
            )
            """,
            [
                FACTOR_ID,dependency_type,dependency_name,FACTOR_ID,
                dependency_type,dependency_name,dependency_factor_id,
                dependency_metric_id,dependency_source_id,
                f"{dependency_type}:{expression}",SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=240,name="low_abnormal_capex_factor_definition",up=_low_abnormal_capex)
]
