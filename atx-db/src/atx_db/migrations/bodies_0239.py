"""Govern PIT annual R&D-to-market-equity factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT R&D intensity v1"
FACTOR_ID = "valuation_rd_to_market_equity"


def _rd_to_market_equity(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT annual R&D-to-market equity','fundamental_valuation',
            'Latest visible positive annual R&D expense divided by contemporaneous positive market capitalization on the governed monthly decision date.',
            'zscore(winsorize_1pct(annual_rd_expense/market_cap))',
            '["dataset:market_cap","metric:rd_expense","universe:us_common_equity_liquid_v1"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Annual R&D filing, market capitalization, and governed universe membership must be visible at the decision close; missing R&D is not imputed as zero.',
            'atx_db.rd_intensity','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_duration_days":[330,380],"maximum_fundamental_age_days":550,"minimum_names_per_date":20,"monthly_sampling":"last_eligible_market_cap_date","positive_rd_expense":true,"positive_market_cap":true,"missing_rd_imputed_as_zero":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id = ?", [FACTOR_ID])
    dependencies = (
        ("dataset", "market_cap", None, None, "market_cap", "market_cap"),
        ("metric", "rd_expense", None, "rd_expense", None, "rd_expense"),
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
                sha256(concat_ws('|','rd_to_market_equity',?,?,?)),
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
    Migration(version=239,name="rd_to_market_equity_factor_definition",up=_rd_to_market_equity)
]
