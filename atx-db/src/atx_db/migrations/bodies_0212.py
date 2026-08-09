"""Optimize continuous-strength lineage through the governed Piotroski surface."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT continuous financial strength v1"
FACTOR_ID = "quality_continuous_financial_strength"
HIGH_BOOK_TO_MARKET_FACTOR_ID = (
    "quality_continuous_financial_strength_high_book_to_market"
)
PIOTROSKI_FACTOR_ID = "quality_piotroski_f_score"
BOOK_TO_MARKET_FACTOR_ID = "value_book_to_market"


def _factor_edge(
    conn: duckdb.DuckDBPyConnection,
    factor_id: str,
    dependency: str,
) -> None:
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','continuous_strength_v2',?,?)),?,
                  'factor',?,?,NULL,NULL,1,?,1100,true,?)
        """,
        [
            factor_id,
            dependency,
            factor_id,
            dependency,
            dependency,
            f"factor:{dependency}",
            SOURCE,
        ],
    )


def _compact_continuous_lineage(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        UPDATE factor_definition
        SET description = 'Equal-weight mean of nine rank-standardized continuous annual components read from the governed Piotroski input lineage. Uses fixed literature-specified weights and no return fitting.',
            input_ids_json = '["factor:quality_piotroski_f_score"]',
            available_at_policy = 'Inherits the exact PIT annual input decision from quality_piotroski_f_score; no annual facts are reloaded or reserialized.',
            standardization_spec_json = '{"method":"equal_weight_rank_z_then_composite_z","component_count":9,"rank_method":"average","complete_case":true,"return_fitted_weights":false,"lineage":"factor_reference"}'
        WHERE factor_id = ?
        """,
        [FACTOR_ID],
    )
    conn.execute(
        """
        UPDATE factor_definition
        SET input_ids_json = '["factor:quality_continuous_financial_strength","factor:value_book_to_market"]',
            available_at_policy = 'Inherits the compact continuous-strength PIT decision and its already governed point-in-time book-to-market condition.'
        WHERE factor_id = ?
        """,
        [HIGH_BOOK_TO_MARKET_FACTOR_ID],
    )
    for factor_id in (FACTOR_ID, HIGH_BOOK_TO_MARKET_FACTOR_ID):
        conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [factor_id])
    _factor_edge(conn, FACTOR_ID, PIOTROSKI_FACTOR_ID)
    _factor_edge(conn, HIGH_BOOK_TO_MARKET_FACTOR_ID, FACTOR_ID)
    _factor_edge(conn, HIGH_BOOK_TO_MARKET_FACTOR_ID, BOOK_TO_MARKET_FACTOR_ID)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=212,
        name="compact_continuous_strength_lineage",
        up=_compact_continuous_lineage,
    )
]
