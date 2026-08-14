"""Govern PIT gross-profit enterprise yield."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT enterprise yield v1"
FACTOR_ID = "valuation_gross_profit_enterprise_yield"
PARENT_FACTOR_ID = "profitability_gross_profitability"


def _gross_profit_enterprise_yield(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT gross-profit enterprise yield','fundamental_valuation',
            'Latest visible positive annual gross profit divided by positive component-lineaged enterprise value at the governed monthly decision close.',
            'zscore(winsorize_1pct(annual_gross_profit/enterprise_value))',
            '["dataset:enterprise_value","factor:profitability_gross_profitability","universe:us_common_equity_liquid_v1"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Enterprise value, source gross-profit factor, governed universe membership, and month-end price must all be visible at the decision close; no missing input is imputed.',
            'atx_db.enterprise_yield','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"maximum_fundamental_age_days":550,"minimum_names_per_date":20,"monthly_sampling":"last_eligible_trading_day","positive_enterprise_value":true,"positive_gross_profit":true,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id = ?", [FACTOR_ID])
    dependencies = (
        ("dataset", "enterprise_value", None, None, "enterprise_value", "enterprise_value"),
        ("factor", PARENT_FACTOR_ID, PARENT_FACTOR_ID, None, None, PARENT_FACTOR_ID),
        (
            "universe",
            "us_common_equity_liquid_v1",
            None,
            None,
            None,
            "us_common_equity_liquid_v1",
        ),
    )
    for (
        dependency_type,
        dependency_name,
        dependency_factor_id,
        dependency_metric_id,
        dependency_source_id,
        expression,
    ) in dependencies:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (
                sha256(concat_ws('|','gross_profit_enterprise_yield',?,?,?)),
                ?,?,?,?,?,?,1,?,550,true,?
            )
            """,
            [
                FACTOR_ID,
                dependency_type,
                dependency_name,
                FACTOR_ID,
                dependency_type,
                dependency_name,
                dependency_factor_id,
                dependency_metric_id,
                dependency_source_id,
                f"{dependency_type}:{expression}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=237,
        name="gross_profit_enterprise_yield_factor_definition",
        up=_gross_profit_enterprise_yield,
    )
]
