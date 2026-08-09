"""Ball cash operating profitability factor family."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT cash profitability v1"
FACTOR_ROWS = (
    (
        "profitability_ball_operating_profitability",
        "PIT Ball operating profitability",
        "fundamental_profitability",
        "Annual revenue less COGS and reported SG&A excluding R&D, scaled by prior-year total assets.",
        "(revenue-cogs-(sga-rd_expense))/lag(total_assets,1y)",
        1,
    ),
    (
        "profitability_cash_operating_profitability",
        "PIT cash operating profitability",
        "fundamental_profitability",
        "Ball et al. operating profitability purged of changes in receivables, inventory, prepaid expense, deferred revenue, payables, and accrued liabilities; scaled by prior-year assets.",
        "(operating_profit-d_ar-d_inventory-d_prepaid+d_deferred_revenue+d_ap+d_accrued)/lag(total_assets,1y)",
        1,
    ),
    (
        "quality_low_operating_working_capital_accruals",
        "PIT low operating working-capital accruals",
        "fundamental_quality",
        "Negative-oriented operating working-capital accrual component removed by Ball cash operating profitability.",
        "-(d_ar+d_inventory+d_prepaid-d_deferred_revenue-d_ap-d_accrued)/lag(total_assets,1y)",
        -1,
    ),
)

METRICS = (
    "revenue",
    "cogs",
    "sga",
    "rd_expense",
    "total_assets",
    "ar",
    "inventory",
    "prepaid_expense",
    "deferred_revenue",
    "ap",
    "accrued_liabilities",
)


def _cash_profitability_factor_family(conn: duckdb.DuckDBPyConnection) -> None:
    input_ids = "[" + ",".join(f'\"metric:{metric}\"' for metric in METRICS) + "]"
    for factor_id, name, family, description, expression, direction in FACTOR_ROWS:
        conn.execute(
            """
            INSERT OR REPLACE INTO factor_definition (
                factor_id, factor_name, family, description, expression,
                input_ids_json, direction, lookback_days,
                neutralization_spec_json, unit, sign, scale,
                is_point_in_time_safe, available_at_policy, declared_in,
                owner, source, standardization_spec_json, valid_from, valid_to
            ) VALUES (
                ?, ?, ?, ?, ?, ?, ?, 430,
                '{"method":"none","by":[]}', 'ratio', 'signed', 'zscore',
                true,
                'Visible only after current annual income and balance-sheet facts, prior-year balance-sheet facts, month-end price, and share count are all available.',
                'atx_db.cash_profitability', 'atx-db', ?,
                '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01]}',
                DATE '1900-01-01', NULL
            )
            """,
            [factor_id, name, family, description, expression, input_ids, direction, SOURCE],
        )
        conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id = ?", [factor_id])
        for metric in METRICS:
            conn.execute(
                """
                INSERT INTO factor_dependency_edges (
                    dependency_id, factor_id, dependency_type, dependency_name,
                    dependency_factor_id, dependency_metric_id, dependency_source_id,
                    dependency_depth, expression, lookback_days, is_direct, source
                ) VALUES (
                    sha256(concat_ws('|', 'cash_profitability', ?, 'metric', ?)),
                    ?, 'metric', ?, NULL, ?, NULL, 1, ?, 430, true, ?
                )
                """,
                [
                    factor_id,
                    metric,
                    factor_id,
                    metric,
                    metric,
                    f"metric:{metric}",
                    SOURCE,
                ],
            )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=198,
        name="cash_operating_profitability_factor_family",
        up=_cash_profitability_factor_family,
    )
]
