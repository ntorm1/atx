"""Promote quarterly ROE into the production conditional router."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db conditional profitability router v2"
FACTOR_ID = "composite_operating_profitability_or_net_issuance"
PRIMARY_FACTOR_ID = "profitability_operating_profitability"
OVERLAY_FACTOR_ID = "profitability_q_factor_roe"
FALLBACK_FACTOR_ID = "financing_low_net_share_issuance"


def _factor_edge(conn: duckdb.DuckDBPyConnection, dependency: str) -> None:
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','conditional_router_v2',?,?)),?,
                  'factor',?,?,NULL,NULL,1,?,550,true,?)
        """,
        [
            FACTOR_ID,
            dependency,
            FACTOR_ID,
            dependency,
            dependency,
            f"factor:{dependency}",
            SOURCE,
        ],
    )


def _quarterly_roe_router(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        UPDATE factor_definition
        SET factor_name = 'Conditional profitability blend or net issuance',
            description = 'Operating profitability with a 25% quarterly q-factor ROE overlay when both are present; otherwise operating profitability; net issuance remains the fallback only when operating profitability is absent.',
            expression = 'if(OP,0.75*OP+0.25*quarterly_ROE_if_available,low_net_share_issuance)',
            input_ids_json = '["factor:profitability_operating_profitability","factor:profitability_q_factor_roe","factor:financing_low_net_share_issuance"]',
            available_at_policy = 'The blend is available after both primary inputs; the primary-only route follows operating profitability; the fallback route is used only when operating profitability is absent.',
            source = ?,
            standardization_spec_json = '{"method":"conditional_blend_then_zscore_cs","quarterly_roe_weight":0.25,"primary_required_for_overlay":true,"fallback_only_when_primary_missing":true,"return_fitted_weights":false}'
        WHERE factor_id = ?
        """,
        [SOURCE, FACTOR_ID],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency in (PRIMARY_FACTOR_ID, OVERLAY_FACTOR_ID, FALLBACK_FACTOR_ID):
        _factor_edge(conn, dependency)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=216,
        name="quarterly_roe_conditional_router",
        up=_quarterly_roe_router,
    )
]
