"""Govern point-in-time quarterly tax-expense momentum."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT tax expense momentum v1"
FACTOR_ID = "earnings_tax_expense_momentum"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _tax_expense_momentum(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT quarterly tax expense momentum','fundamental_earnings',
            'Seasonally differenced quarterly income-tax expense scaled by same-quarter prior-year assets, following Thomas-Zhang tax expense momentum.',
            '(income_tax_q-income_tax_q_minus_4)/assets_q_minus_4',
            '["metric:income_tax","metric:total_assets","factor:investment_conservative_asset_growth"]',
            1,430,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Current and same-quarter prior-year tax expense and prior assets must be visible at the governed month end; zero surprises, stale observations, and absolute ratios above one are excluded; no value is imputed.',
            'atx_db.tax_expense_momentum','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"quarter_duration_days":[70,115],"same_quarter_gap_days":[300,430],"maximum_reporting_age_days":200,"maximum_absolute_raw_value":1.0,"zero_surprises_excluded":true,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID,SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?",[FACTOR_ID])
    dependencies = (
        ("factor",PARENT_FACTOR_ID,PARENT_FACTOR_ID,None),
        ("metric","income_tax",None,"income_tax"),
        ("metric","total_assets",None,"total_assets"),
    )
    for dependency_type,name,dependency_factor_id,dependency_metric_id in dependencies:
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','tax_expense_momentum',?,?)),?,?,?, ?,?,NULL,
                      1,?,430,true,?)""",
            [FACTOR_ID,name,FACTOR_ID,dependency_type,name,dependency_factor_id,
             dependency_metric_id,f"{dependency_type}:{name}",SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=245,name="tax_expense_momentum_factor",up=_tax_expense_momentum)]
