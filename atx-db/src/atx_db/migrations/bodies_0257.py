"""Govern point-in-time abnormal receivables growth relative to revenue growth."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT abnormal receivables growth v1"
FACTOR_ID = "quality_low_abnormal_receivables_growth"
REVENUE_GROWTH_FACTOR_ID = "growth_quarterly_revenue_yoy"


def _abnormal_receivables_growth(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low abnormal receivables growth','fundamental_quality',
            'Same-quarter year-over-year receivables growth minus revenue growth; lower disproportionate receivables accumulation is preferred.',
            '(receivables_t/receivables_t_4-1)-(revenue_t/revenue_t_4-1)',
            '["factor:growth_quarterly_revenue_yoy","metric:ar"]',
            -1,430,'{"method":"none","by":[]}','ratio',
            'lower_is_better','zscore',true,
            'Current and same-quarter-prior-year revenue and positive receivables facts must be visible by the governed monthly decision; missing receivables are not imputed.',
            'atx_db.abnormal_receivables_growth','atx-db',?,
            '{"method":"negate_then_winsorize_zscore_cs","same_quarter_prior_year":true,"winsor_limits":[0.01,0.01],"maximum_absolute_receivables_growth":10.0,"maximum_absolute_abnormal_growth":10.0,"missing_receivables_imputed":false,"return_fitted_parameters":false}',
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
        ) VALUES (sha256(concat_ws('|','abnormal_receivables_growth',?,?,?)),
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
                "metric",
                "ar",
                FACTOR_ID,
                "metric",
                "ar",
                None,
                "ar",
                None,
                "metric:ar",
                SOURCE,
            ),
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=257,
        name="abnormal_receivables_growth",
        up=_abnormal_receivables_growth,
    )
]
