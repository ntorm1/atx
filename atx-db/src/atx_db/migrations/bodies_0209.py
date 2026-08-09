"""Corrected and materializable public-company Altman Z-score."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT corrected Altman Z-score v1"
FACTOR_ID = "distress_altman_z_score"
FACTOR_DEPENDENCIES = (
    "profitability_operating_cash_flow_to_assets",
    "value_book_to_market",
)
METRIC_DEPENDENCIES = (
    "current_assets",
    "current_liabilities",
    "retained_earnings",
    "total_liabilities",
    "operating_income_ttm",
    "revenue_ttm",
)


def _corrected_altman_z_score(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT corrected Altman Z-score','fundamental_distress',
            'Public-company Altman Z-score using working capital, retained earnings, TTM operating income, market equity over total liabilities, and TTM sales. Corrects the legacy empty total-debt dependency.',
            '1.2*(current_assets-current_liabilities)/assets + 1.4*retained_earnings/assets + 3.3*operating_income_ttm/assets + 0.6*market_cap/total_liabilities + revenue_ttm/assets',
            ?,1,550,'{"method":"none","by":[]}','score','higher_is_better','zscore',true,
            'Visible at the later upstream monthly decision timestamp after all statement and TTM inputs are visible.',
            'atx_db.altman_distress','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"currency":"USD","liability_denominator":"total_liabilities"}',
            DATE '1900-01-01',NULL
        )
        """,
        [
            FACTOR_ID,
            '["factor:profitability_operating_cash_flow_to_assets","factor:value_book_to_market","metric:current_assets","metric:current_liabilities","metric:retained_earnings","metric:total_liabilities","metric:operating_income_ttm","metric:revenue_ttm"]',
            SOURCE,
        ],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency in FACTOR_DEPENDENCIES:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','corrected_altman_factor',?,?)),?,
                      'factor',?,?,NULL,NULL,1,?,550,true,?)
            """,
            [
                FACTOR_ID,
                dependency,
                FACTOR_ID,
                dependency,
                dependency,
                f"factor:{dependency}",
                SOURCE,
            ],
        )
    for metric in METRIC_DEPENDENCIES:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','corrected_altman_metric',?,?)),?,
                      'metric',?,NULL,?,NULL,1,?,550,true,?)
            """,
            [FACTOR_ID, metric, FACTOR_ID, metric, metric, f"metric:{metric}", SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=209, name="corrected_altman_z_score", up=_corrected_altman_z_score)]
