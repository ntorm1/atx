"""Point-in-time net share issuance factor definition."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT net share issuance v1"
FACTOR_ID = "financing_low_net_share_issuance"


def _net_share_issuance_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id, factor_name, family, description, expression,
            input_ids_json, direction, lookback_days,
            neutralization_spec_json, unit, sign, scale,
            is_point_in_time_safe, available_at_policy, declared_in,
            owner, source, standardization_spec_json, valid_from, valid_to
        ) VALUES (
            ?, 'PIT low net share issuance', 'fundamental_financing',
            'Negative one-year log change in split-adjusted shares outstanding; repurchases score high and net issuance scores low.',
            '-ln((shares_t*split_index_t)/(shares_t_1*split_index_t_1))',
            '["metric:shares_outstanding","market:split_factor"]',
            1, 430,
            '{"method":"none","by":[]}', 'log_change', 'higher_is_better', 'zscore',
            true,
            'Visible at a month-end close only after both share observations and the intervening split history are available.',
            'atx_db.net_issuance', 'atx-db', ?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01]}',
            DATE '1900-01-01', NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id = ?", [FACTOR_ID])
    for dependency_type, dependency_name, expression in (
        ("metric", "shares_outstanding", "metric:shares_outstanding"),
        ("market", "split_factor", "market:split_factor"),
    ):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id, factor_id, dependency_type, dependency_name,
                dependency_factor_id, dependency_metric_id, dependency_source_id,
                dependency_depth, expression, lookback_days, is_direct, source
            ) VALUES (
                sha256(concat_ws('|', 'net_issuance', ?, ?, ?)),
                ?, ?, ?, NULL, ?, NULL, 1, ?, 430, true, ?
            )
            """,
            [
                FACTOR_ID,
                dependency_type,
                dependency_name,
                FACTOR_ID,
                dependency_type,
                dependency_name,
                dependency_name if dependency_type == "metric" else None,
                expression,
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=199,
        name="net_share_issuance_factor",
        up=_net_share_issuance_factor,
    )
]
