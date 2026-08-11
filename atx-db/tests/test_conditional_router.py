from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.conditional_router import (
    FACTOR_ID,
    FALLBACK_FACTOR_ID,
    PRIMARY_FACTOR_ID,
    SECONDARY_FACTOR_ID,
    ConditionalRouterOptions,
    compute_conditional_router_rows,
    load_conditional_router_inputs,
    refresh_conditional_router_values,
)


def test_quarterly_secondary_preserves_primary_extreme_decile_membership() -> None:
    as_of_date = dt.date(2025, 1, 31)
    available_at = dt.datetime(2025, 1, 31, 22)
    inputs = pd.DataFrame(
        [
            {
                "input_factor_value_id": f"primary-{index}",
                "input_factor_id": PRIMARY_FACTOR_ID,
                "input_factor_name": "primary",
                "input_raw_value": float(index),
                "input_value": float(index),
                "input_available_at": available_at,
                "input_source": "test-primary",
                "security_id": f"S{index:03d}",
                "symbol": f"S{index:03d}",
                "as_of_date": as_of_date,
                "route": "primary",
                "secondary_factor_value_id": f"secondary-{index}",
                "secondary_raw_value": float(99 - index),
                "secondary_value": float(99 - index),
                "secondary_available_at": available_at,
                "secondary_source": "test-secondary",
            }
            for index in range(100)
        ]
    )
    rows = compute_conditional_router_rows(inputs).copy()
    ordered = rows.sort_values("value", kind="stable")

    assert set(ordered.head(10)["security_id"]) == {
        f"S{index:03d}" for index in range(10)
    }
    assert set(ordered.tail(10)["security_id"]) == {
        f"S{index:03d}" for index in range(90, 100)
    }
    assert ordered.iloc[-1]["security_id"] == "S090"
    lineage = json.loads(ordered.iloc[-1]["input_lineage_json"])
    assert lineage["secondary_used"] is True
    assert lineage["ordering"]["top_bottom_bucket_membership_preserved"] is True


def _insert_factor(store, *, factor_id: str, security_id: str, value: float) -> None:
    as_of_date = dt.date(2025, 1, 31)
    store.con.execute(
        """
        INSERT INTO fundamental_factor_values (
            factor_value_id, factor_id, factor_name, family, security_id, symbol,
            as_of_date, raw_value, value, available_at, input_ids_json,
            input_lineage_json, is_latest_revision, run_id, source
        ) VALUES (?, ?, ?, 'test', ?, ?, ?, ?, ?, ?, '[]', '{}', true, 'fixture', 'test')
        """,
        [
            f"{factor_id}-{security_id}",
            factor_id,
            factor_id,
            security_id,
            security_id,
            as_of_date,
            value,
            value,
            dt.datetime(2025, 1, 31, 22),
        ],
    )


def _insert_universe_member(store, security_id: str) -> None:
    store.con.execute(
        """
        INSERT INTO universe_membership (
            universe_id, security_id, symbol, valid_from, valid_to, as_of_date,
            is_member, reason, rules_json, decision_count, available_at, source,
            run_id, is_latest_revision, source_loaded_at
        ) VALUES (
            'us_common_equity_liquid_v1', ?, ?, DATE '2025-01-01', NULL,
            DATE '2025-01-01', true, 'test', '{}', 1,
            TIMESTAMP '2025-01-01', 'test', 'test', true, TIMESTAMP '2025-01-01'
        )
        """,
        [security_id, security_id],
    )


def test_router_prefers_profitability_and_uses_issuance_only_when_missing(tmp_store) -> None:
    for security_id, op_value, issuance_value in (
        ("A", 1.0, -3.0),
        ("B", -1.0, 3.0),
        ("C", None, 2.0),
    ):
        _insert_universe_member(tmp_store, security_id)
        if op_value is not None:
            _insert_factor(
                tmp_store,
                factor_id=PRIMARY_FACTOR_ID,
                security_id=security_id,
                value=op_value,
            )
        _insert_factor(
            tmp_store,
            factor_id=FALLBACK_FACTOR_ID,
            security_id=security_id,
            value=issuance_value,
        )
    options = ConditionalRouterOptions(minimum_names_per_date=3, run_id="test-router")
    inputs = load_conditional_router_inputs(tmp_store, options).set_index("security_id")
    assert inputs.loc["A", "input_factor_id"] == PRIMARY_FACTOR_ID
    assert inputs.loc["B", "input_factor_id"] == PRIMARY_FACTOR_ID
    assert inputs.loc["C", "input_factor_id"] == FALLBACK_FACTOR_ID

    assert refresh_conditional_router_values(tmp_store, options) == 3
    assert refresh_conditional_router_values(tmp_store, options) == 3
    output = tmp_store.con.execute(
        """
        SELECT security_id, raw_value, value, available_at, input_lineage_json
        FROM fundamental_factor_values
        WHERE factor_id = ?
        ORDER BY security_id
        """,
        [FACTOR_ID],
    ).df().set_index("security_id")
    assert output.loc["B", "raw_value"] < output.loc["A", "raw_value"]
    assert output.loc["A", "raw_value"] < output.loc["C", "raw_value"]
    assert output["value"].mean() == pytest.approx(0.0)
    assert json.loads(output.loc["C", "input_lineage_json"])["selected_route"] == (
        "fallback"
    )


def test_conditional_router_factor_is_governed_by_migration(tmp_store) -> None:
    row = tmp_store.con.execute(
        """
        SELECT family, direction, is_point_in_time_safe, source
        FROM factor_definition
        WHERE factor_id = ?
        """,
        [FACTOR_ID],
    ).fetchone()
    assert row == (
        "fundamental_composite",
        1,
        True,
        "atx-db governed cash-decile router v6",
    )
    dependencies = tmp_store.con.execute(
        """
        SELECT dependency_factor_id
        FROM factor_dependency_edges
        WHERE factor_id = ?
        ORDER BY dependency_factor_id
        """,
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == sorted(
        [(PRIMARY_FACTOR_ID,), (FALLBACK_FACTOR_ID,), (SECONDARY_FACTOR_ID,)]
    )
