"""Conditional operating-profitability / net-issuance factor definition."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db conditional OP/issuance router v1"
FACTOR_ID = "composite_operating_profitability_or_net_issuance"
PRIMARY_FACTOR_ID = "profitability_operating_profitability"
FALLBACK_FACTOR_ID = "financing_low_net_share_issuance"


def _conditional_op_issuance_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id, factor_name, family, description, expression,
            input_ids_json, direction, lookback_days,
            neutralization_spec_json, unit, sign, scale,
            is_point_in_time_safe, available_at_policy, declared_in,
            owner, source, standardization_spec_json, valid_from, valid_to
        ) VALUES (
            ?, 'Conditional operating profitability or net issuance',
            'fundamental_composite',
            'Use operating profitability when available for a security/date; otherwise use low net share issuance.',
            'zscore(coalesce_by_availability(operating_profitability, low_net_share_issuance))',
            ?, 1, 430,
            '{"method":"none","by":[]}', 'normalized_score',
            'higher_is_better', 'zscore', true,
            'Visible at the selected upstream factor available_at; operating profitability has strict routing priority.',
            'atx_db.conditional_router', 'atx-db', ?,
            '{"method":"zscore_cs_after_primary_else_fallback","winsor_limits":null}',
            DATE '1900-01-01', NULL
        )
        """,
        [
            FACTOR_ID,
            f'["factor:{PRIMARY_FACTOR_ID}","factor:{FALLBACK_FACTOR_ID}"]',
            SOURCE,
        ],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id = ?", [FACTOR_ID])
    for dependency_factor_id, route in (
        (PRIMARY_FACTOR_ID, "primary"),
        (FALLBACK_FACTOR_ID, "fallback"),
    ):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id, factor_id, dependency_type, dependency_name,
                dependency_factor_id, dependency_metric_id, dependency_source_id,
                dependency_depth, expression, lookback_days, is_direct, source
            ) VALUES (
                sha256(concat_ws('|', 'conditional_router', ?, ?, ?)),
                ?, 'factor', ?, ?, NULL, NULL, 1, ?, 430, true, ?
            )
            """,
            [
                FACTOR_ID,
                dependency_factor_id,
                route,
                FACTOR_ID,
                dependency_factor_id,
                dependency_factor_id,
                f"{route}:factor:{dependency_factor_id}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=200,
        name="conditional_operating_profitability_net_issuance_factor",
        up=_conditional_op_issuance_factor,
    )
]
