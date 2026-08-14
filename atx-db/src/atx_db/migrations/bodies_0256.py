"""Govern point-in-time same-quarter gross-margin change."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly gross margin change v1"
FACTOR_ID = "profitability_quarterly_gross_margin_change_yoy"
REVENUE_GROWTH_FACTOR_ID = "growth_quarterly_revenue_yoy"
QOP_FACTOR_ID = "profitability_quarterly_operating_profitability_lagged_assets"


def _quarterly_gross_margin_change(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT same-quarter change in quarterly gross margin','fundamental_profitability',
            'Current quarterly gross margin minus same-quarter prior-year gross margin; expansion is preferred.',
            'gross_margin_t-gross_margin_t_4',
            '["factor:growth_quarterly_revenue_yoy","factor:profitability_quarterly_operating_profitability_lagged_assets"]',
            1,430,'{"method":"none","by":[]}','ratio_change',
            'higher_is_better','zscore',true,
            'Exact current and same-quarter-prior-year quarterly statements must both be visible at the governed monthly decision; missing inputs are not imputed.',
            'atx_db.quarterly_gross_margin_change','atx-db',?,
            '{"method":"winsorize_zscore_cs","same_quarter_prior_year":true,"winsor_limits":[0.01,0.01],"maximum_absolute_gross_margin":5.0,"maximum_absolute_margin_change":5.0,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    conn.executemany(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','quarterly_gross_margin_change',?,?,?)),
                  ?,?,?,?,?,?,1,?,430,true,?)
        """,
        [
            (
                FACTOR_ID,
                "factor",
                REVENUE_GROWTH_FACTOR_ID,
                FACTOR_ID,
                "factor",
                REVENUE_GROWTH_FACTOR_ID,
                REVENUE_GROWTH_FACTOR_ID,
                None,
                None,
                f"factor:{REVENUE_GROWTH_FACTOR_ID}",
                SOURCE,
            ),
            (
                FACTOR_ID,
                "factor",
                QOP_FACTOR_ID,
                FACTOR_ID,
                "factor",
                QOP_FACTOR_ID,
                QOP_FACTOR_ID,
                None,
                None,
                f"factor:{QOP_FACTOR_ID}",
                SOURCE,
            ),
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=256,
        name="quarterly_gross_margin_change",
        up=_quarterly_gross_margin_change,
    )
]
