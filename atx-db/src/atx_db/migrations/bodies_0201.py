"""Strict cash-flow net payout yield factor definition."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT cash-flow net payout yield v1"
FACTOR_ID = "financing_net_payout_yield"


def _net_payout_yield_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id, factor_name, family, description, expression,
            input_ids_json, direction, lookback_days,
            neutralization_spec_json, unit, sign, scale,
            is_point_in_time_safe, available_at_policy, declared_in,
            owner, source, standardization_spec_json, valid_from, valid_to
        ) VALUES (
            ?, 'PIT cash-flow net payout yield', 'fundamental_financing',
            'Same-filing TTM common dividends plus common-share repurchases less common-stock issuance, divided by split-aware market capitalization.',
            '(-common_div_paid_ttm - share_repurchases_ttm - stock_issuance_ttm) / market_cap',
            '["metric:common_div_paid_ttm","metric:share_repurchases_ttm","metric:stock_issuance_ttm","metric:market_cap"]',
            1, 550,
            '{"method":"none","by":[]}', 'ratio', 'higher_is_better', 'zscore',
            true,
            'Visible at a monthly close only after all three same-filing TTM components, share count, split history, and price are available.',
            'atx_db.net_payout', 'atx-db', ?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.025,0.025],"missing_components":"complete_case"}',
            DATE '1900-01-01', NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id = ?", [FACTOR_ID])
    for dependency_type, dependency_name in (
        ("metric", "common_div_paid_ttm"),
        ("metric", "share_repurchases_ttm"),
        ("metric", "stock_issuance_ttm"),
        ("metric", "market_cap"),
    ):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id, factor_id, dependency_type, dependency_name,
                dependency_factor_id, dependency_metric_id, dependency_source_id,
                dependency_depth, expression, lookback_days, is_direct, source
            ) VALUES (
                sha256(concat_ws('|', 'net_payout', ?, ?, ?)),
                ?, ?, ?, NULL, ?, NULL, 1, ?, 550, true, ?
            )
            """,
            [
                FACTOR_ID,
                dependency_type,
                dependency_name,
                FACTOR_ID,
                dependency_type,
                dependency_name,
                dependency_name,
                f"{dependency_type}:{dependency_name}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=201, name="cash_flow_net_payout_yield_factor", up=_net_payout_yield_factor)
]
