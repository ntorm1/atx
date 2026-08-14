"""Govern PIT enterprise-yield research features."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT enterprise yield v1"
FACTORS = (
    (
        "valuation_enterprise_yield_ebit",
        "PIT operating-income enterprise yield",
        "operating_income",
        "operating_income_ttm/enterprise_value",
    ),
    (
        "valuation_enterprise_yield_sales",
        "PIT revenue enterprise yield",
        "revenue",
        "revenue_ttm/enterprise_value",
    ),
)


def _enterprise_yield(conn: duckdb.DuckDBPyConnection) -> None:
    for factor_id, factor_name, metric, expression in FACTORS:
        conn.execute(
            """
            INSERT OR REPLACE INTO factor_definition (
                factor_id,factor_name,family,description,expression,input_ids_json,
                direction,lookback_days,neutralization_spec_json,unit,sign,scale,
                is_point_in_time_safe,available_at_policy,declared_in,owner,source,
                standardization_spec_json,valid_from,valid_to
            ) VALUES (
                ?,?,'fundamental_valuation',
                'Monthly enterprise yield built from component-lineaged enterprise value and the latest fully visible positive TTM fundamental.',
                ?,?,1,550,'{"method":"none","by":[]}','normalized_score',
                'higher_is_better','zscore',true,
                'Enterprise value, TTM fundamental, governed universe membership, and month-end price must all be visible at the decision close; no missing input is imputed.',
                'atx_db.enterprise_yield','atx-db',?,
                '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"maximum_fundamental_age_days":550,"minimum_names_per_date":20,"monthly_sampling":"last_eligible_trading_day","positive_enterprise_value":true,"positive_fundamental":true,"missing_components_imputed":false,"return_fitted_parameters":false}',
                DATE '1900-01-01',NULL
            )
            """,
            [
                factor_id,
                factor_name,
                f"zscore(winsorize_1pct({expression}))",
                f'["dataset:enterprise_value","metric:{metric}","universe:us_common_equity_liquid_v1"]',
                SOURCE,
            ],
        )
        conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id = ?", [factor_id])
        for dependency_type, dependency_name in (
            ("dataset", "enterprise_value"),
            ("metric", metric),
            ("universe", "us_common_equity_liquid_v1"),
        ):
            conn.execute(
                """
                INSERT INTO factor_dependency_edges (
                    dependency_id,factor_id,dependency_type,dependency_name,
                    dependency_factor_id,dependency_metric_id,dependency_source_id,
                    dependency_depth,expression,lookback_days,is_direct,source
                ) VALUES (
                    sha256(concat_ws('|','enterprise_yield',?,?,?)),?,?,?,
                    NULL,CASE WHEN ?='metric' THEN ? ELSE NULL END,
                    CASE WHEN ?='dataset' THEN ? ELSE NULL END,
                    1,?,550,true,?
                )
                """,
                [
                    factor_id,
                    dependency_type,
                    dependency_name,
                    factor_id,
                    dependency_type,
                    dependency_name,
                    dependency_type,
                    dependency_name,
                    dependency_type,
                    dependency_name,
                    f"{dependency_type}:{dependency_name}",
                    SOURCE,
                ],
            )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=236, name="enterprise_yield_factor_definitions", up=_enterprise_yield)
]
