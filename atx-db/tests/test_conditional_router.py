from __future__ import annotations

import datetime as dt
import json

import pytest

from atx_db.conditional_router import (
    FACTOR_ID,
    FALLBACK_FACTOR_ID,
    PRIMARY_FACTOR_ID,
    ConditionalRouterOptions,
    load_conditional_router_inputs,
    refresh_conditional_router_values,
)


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


def test_router_prefers_profitability_and_uses_issuance_only_when_missing(tmp_store) -> None:
    for security_id, op_value, issuance_value in (
        ("A", 1.0, -3.0),
        ("B", -1.0, 3.0),
        ("C", None, 2.0),
    ):
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
    assert output.loc["A", "raw_value"] == pytest.approx(1.0)
    assert output.loc["B", "raw_value"] == pytest.approx(-1.0)
    assert output.loc["C", "raw_value"] == pytest.approx(2.0)
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
        "atx-db conditional OP/issuance router v3",
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
    assert dependencies == sorted([(PRIMARY_FACTOR_ID,), (FALLBACK_FACTOR_ID,)])
