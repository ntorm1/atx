"""Govern point-in-time Lev-Nissim tax-to-book income."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT tax-to-book income v1"
FACTOR_ID = "earnings_tax_to_book_income"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _tax_to_book_income(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT tax-to-book income ratio','fundamental_earnings',
            'Lev-Nissim after-tax estimated taxable income divided by positive annual book income.',
            '(current_tax/statutory_rate)*(1-statutory_rate)/net_income',
            '["metric:current_tax","metric:net_income","factor:investment_conservative_asset_growth"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Exact-accession annual current tax and net income must be positive and visible at governed month end; observations older than 550 days or ratios above ten are excluded; no value is imputed.',
            'atx_db.tax_to_book_income','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_duration_days":[330,400],"maximum_reporting_age_days":550,"maximum_raw_value":10.0,"statutory_rate_policy":"35pct_through_2017_then_21pct","positive_components_required":true,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID,SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?",[FACTOR_ID])
    dependencies = (
        ("factor",PARENT_FACTOR_ID,PARENT_FACTOR_ID,None),
        ("metric","current_tax",None,"current_tax"),
        ("metric","net_income",None,"net_income"),
    )
    for dependency_type,name,dependency_factor_id,dependency_metric_id in dependencies:
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','tax_to_book_income',?,?)),?,?,?, ?,?,NULL,
                      1,?,550,true,?)""",
            [FACTOR_ID,name,FACTOR_ID,dependency_type,name,dependency_factor_id,
             dependency_metric_id,f"{dependency_type}:{name}",SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=246,name="tax_to_book_income_factor",up=_tax_to_book_income)]
