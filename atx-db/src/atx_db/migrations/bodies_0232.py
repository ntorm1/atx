"""Govern SUE confirmed by same-sign direct revenue growth."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT SUE revenue-growth agreement v1"
FACTOR_ID = "earnings_sue_revenue_growth_agreement"
SUE_FACTOR_ID = "earnings_standardized_unexpected_eps"
REVENUE_GROWTH_FACTOR_ID = "growth_quarterly_revenue_yoy"


def _earnings_revenue_growth_agreement(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT SUE with same-sign revenue-growth confirmation',
            'fundamental_earnings',
            'Standardized unexpected EPS retained only when same-quarter year-over-year revenue growth has the same sign.',
            'zscore(sue | sue*quarterly_revenue_growth>0)',
            '["factor:earnings_standardized_unexpected_eps","factor:growth_quarterly_revenue_yoy"]',
            1,500,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Both governed factor values must exist on the same security/monthly decision key; available_at is their maximum.',
            'atx_db.earnings_revenue_growth_agreement','atx-db',?,
            '{"method":"same_sign_gate_then_zscore_sue","gate":"sue*quarterly_revenue_growth>0","return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency_factor_id in (SUE_FACTOR_ID, REVENUE_GROWTH_FACTOR_ID):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (
                sha256(concat_ws('|','sue_revenue_growth_agreement',?,?)),?,
                'factor',?,?,NULL,NULL,1,?,500,true,?
            )
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
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=232,
        name="earnings_revenue_growth_agreement",
        up=_earnings_revenue_growth_agreement,
    )
]
