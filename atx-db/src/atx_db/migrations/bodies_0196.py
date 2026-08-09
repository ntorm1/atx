"""Fama-French-style operating profitability factor definition."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

FACTOR_ID = "profitability_operating_profitability"


def _operating_profitability_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id, factor_name, family, description, expression,
            input_ids_json, direction, lookback_days,
            neutralization_spec_json, unit, sign, scale,
            is_point_in_time_safe, available_at_policy, declared_in,
            owner, source, standardization_spec_json, valid_from, valid_to
        ) VALUES (
            'profitability_operating_profitability',
            'PIT Fama-French operating profitability',
            'fundamental_profitability',
            'Annual revenue minus COGS, SG&A, and interest expense over positive book equity; missing cost components are zero only when at least one cost component is reported. Minority interest is not yet available and is disclosed in row lineage.',
            'method:pit_annual_operating_profitability|numerator:revenue-cogs-sga-interest_expense|denominator:stockholders_equity|max_age_days:550',
            '["metric:revenue","metric:cogs","metric:sga","metric:interest_expense","metric:stockholders_equity"]',
            1, 0, '{"method":"none","by":[]}', 'ratio', 'signed', 'zscore',
            true,
            'Available after the month-end close when price, shares, and same-filing annual inputs are visible and no more than 550 days old.',
            'atx_db.fundamental_signals', 'atx-db',
            'atx-db PIT fundamental signals v1',
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01]}',
            DATE '1900-01-01', NULL
        )
        """
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id = ?", [FACTOR_ID])
    for metric in ("revenue", "cogs", "sga", "interest_expense", "stockholders_equity"):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id, factor_id, dependency_type, dependency_name,
                dependency_factor_id, dependency_metric_id, dependency_source_id,
                dependency_depth, expression, lookback_days, is_direct, source
            ) VALUES (
                sha256(concat_ws('|', 'pit_fundamental_signal', ?, 'metric', ?)),
                ?, 'metric', ?, NULL, ?, NULL, 1, ?, 0, true,
                'atx-db PIT fundamental signals v1'
            )
            """,
            [FACTOR_ID, metric, FACTOR_ID, metric, metric, f"metric:{metric}"],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=196, name="operating_profitability_factor", up=_operating_profitability_factor)
]

