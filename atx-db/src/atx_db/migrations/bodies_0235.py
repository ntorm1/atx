"""Govern annual change in approximate net-operating-asset turnover."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT annual NOA-proxy turnover change v1"
FACTOR_ID = "efficiency_annual_noa_proxy_turnover_change"
METRIC_DEPENDENCIES = (
    "revenue",
    "stockholders_equity",
    "lt_debt",
    "cash_st_inv",
)


def _noa_proxy_turnover_change(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT annual change in NOA-proxy turnover','fundamental_efficiency',
            'Annual change in revenue turnover on an approximate net-operating-asset denominator. The proxy deliberately excludes unavailable broad PIT short-term debt, preferred stock, minority interest, operating-liability, and complete financial-asset fields and is not labelled exact NOA.',
            'zscore(winsorize_1pct(revenue_t/(equity_t_1+lt_debt_t_1-cash_t_1)-revenue_t_1/(equity_t_2+lt_debt_t_2-cash_t_2)))',
            '["metric:revenue","metric:stockholders_equity","metric:lt_debt","metric:cash_st_inv","universe:us_common_equity_liquid_v1"]',
            1,1100,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'All exact-accession revenue and proxy components must be visible at the governed monthly close; annual periods must be 300-430 days apart, no missing component is imputed, and both proxy denominators must be positive.',
            'atx_db.noa_proxy_turnover_change','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_duration_days":[330,400],"annual_period_gap_days":[300,430],"maximum_fundamental_age_days":550,"minimum_names_per_date":20,"noa_proxy":"stockholders_equity+lt_debt-cash_st_inv","missing_components_imputed":false,"is_exact_noa":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for metric in METRIC_DEPENDENCIES:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','noa_proxy_turnover_change',?,?)),?,
                      'metric',?,NULL,?,NULL,1,?,1100,true,?)
            """,
            [FACTOR_ID, metric, FACTOR_ID, metric, metric, f"metric:{metric}", SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=235,
        name="annual_noa_proxy_turnover_change",
        up=_noa_proxy_turnover_change,
    )
]
