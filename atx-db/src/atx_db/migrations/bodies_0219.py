"""Govern the q5 published-slope expected-growth proxy."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT q5 published-slope expected growth proxy v1"
FACTOR_ID = "investment_q5_expected_growth_proxy"
DEPENDENCY_FACTORS = (
    "investment_conservative_asset_growth",
    "profitability_cash_operating_profitability",
    "profitability_q_factor_delta_roe",
)


def _expected_growth(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT q5 expected investment-growth proxy',
            'fundamental_expected_growth',
            'Point-in-time q5 expected one-year investment-growth proxy using the published full-sample slopes for log Tobin q, cash operating profitability, and four-quarter change in ROE.',
            '-0.029*ln(tobins_q)+0.516*cash_operating_profitability+0.771*delta_roe',
            '["factor:investment_conservative_asset_growth","factor:profitability_cash_operating_profitability","factor:profitability_q_factor_delta_roe","source:sec_company_facts"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Annual inputs must be visible and 120-550 days old; delta ROE uses its governed monthly decision; SEC debt facts must belong to the same annual accession.',
            'atx_db.expected_growth','atx-db',?,
            '{"method":"published_slope_composite_then_zscore_cs","published_slopes":{"log_tobins_q":-0.029,"cash_operating_profitability":0.516,"delta_roe":0.771},"winsor_limits":[0.01,0.01],"missing_delta_roe":"zero","return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency_factor_id in DEPENDENCY_FACTORS:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','q5_expected_growth',?,?)),?,
                      'factor',?,?,NULL,NULL,1,?,550,true,?)
            """,
            [
                FACTOR_ID,
                dependency_factor_id,
                FACTOR_ID,
                dependency_factor_id,
                dependency_factor_id,
                f"factor:{dependency_factor_id}",
                SOURCE,
            ],
        )
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','q5_expected_growth',?,'sec_company_facts')),?,
                  'source','sec_company_facts',NULL,NULL,'sec_company_facts',1,
                  'same-accession LongTermDebtNoncurrent plus current debt',550,true,?)
        """,
        [FACTOR_ID, FACTOR_ID, SOURCE],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=219, name="q5_expected_growth_proxy", up=_expected_growth)]
