"""Tier1-S3 T3: the derived-metric seed registry, its validation and its ordering."""

from __future__ import annotations

import csv

import pytest

from atx_db.derived_registry import (
    DERIVED_SEED_COLUMNS,
    DERIVED_SEED_PATH,
    METRIC_WINDOWS,
    RECLAIMED_ITEM_CODES,
    DerivedMetricDefinition,
    DerivedRegistryError,
    default_derived_definitions,
    derived_statement_item_codes,
    known_item_codes,
    read_derived_seed,
    seed_derived_metric_definitions,
    topological_order,
    validate_definitions,
)

_ITEMS = frozenset({"revenue", "cost_of_revenue_cogs", "gross_profit__1004", "total_assets"})


def _definition(**overrides) -> DerivedMetricDefinition:
    base = dict(
        metric_code="gross_margin",
        family="profitability",
        expression="safe_div(revenue, total_assets)",
        window="ttm",
        inputs=("item:revenue", "item:total_assets"),
        requires_market=False,
        description="Test metric.",
        version="1",
    )
    base.update(overrides)
    return DerivedMetricDefinition(**base)


def test_seed_header_is_the_charter_header():
    with DERIVED_SEED_PATH.open("r", encoding="utf-8", newline="") as handle:
        header = tuple(next(csv.reader(handle)))
    assert header == DERIVED_SEED_COLUMNS
    assert DERIVED_SEED_COLUMNS == (
        "metric_code",
        "family",
        "expression",
        "window",
        "inputs",
        "requires_market",
        "description",
        "version",
    )


def test_seed_rows_round_trip_into_definitions():
    definitions = read_derived_seed()
    assert definitions == default_derived_definitions()
    codes = {definition.metric_code for definition in definitions}
    assert {"revenue_ttm", "gross_profit_ttm", "gross_margin"} <= codes
    ttm = next(d for d in definitions if d.metric_code == "revenue_ttm")
    assert ttm.item_inputs == ("revenue",)
    assert ttm.metric_inputs == ()
    assert ttm.window in METRIC_WINDOWS


def test_the_shipped_seed_validates_against_the_item_registry():
    validate_definitions(default_derived_definitions(), item_codes=known_item_codes())


def test_known_item_codes_reads_the_fundamental_item_seed():
    codes = known_item_codes()
    assert "revenue" in codes
    assert "total_assets" in codes
    assert "cash_flow_from_operations" in codes


def test_undeclared_reference_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (_definition(inputs=("item:revenue",)),),
            item_codes=_ITEMS,
        )
    assert "total_assets" in str(excinfo.value)


def test_unused_declared_input_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (_definition(inputs=("item:revenue", "item:total_assets", "item:inventory")),),
            item_codes=_ITEMS | {"inventory"},
        )
    assert "inventory" in str(excinfo.value)


def test_unknown_item_code_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (_definition(expression="safe_div(revenue, nonsense)", inputs=("item:revenue", "item:nonsense")),),
            item_codes=_ITEMS,
        )
    assert "nonsense" in str(excinfo.value)


def test_metric_code_colliding_with_an_item_code_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions((_definition(metric_code="revenue"),), item_codes=_ITEMS)
    assert "collides" in str(excinfo.value)


def test_every_reclaimed_code_is_a_dead_derived_statement_row():
    dead = derived_statement_item_codes()
    assert dead >= RECLAIMED_ITEM_CODES, sorted(RECLAIMED_ITEM_CODES - dead)
    assert "revenue" not in dead
    assert "total_assets" not in dead


def test_a_reclaimed_derived_statement_code_is_allowed_as_a_metric_code():
    validate_definitions(
        (_definition(metric_code="roa"),),
        item_codes=_ITEMS | {"roa"},
        reclaimable_codes=frozenset({"roa"}),
    )


def test_market_input_outside_the_daily_window_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (
                _definition(
                    metric_code="bad_market",
                    expression="safe_div(revenue, close)",
                    inputs=("item:revenue", "market:close"),
                    window="ttm",
                ),
            ),
            item_codes=_ITEMS,
        )
    assert "daily" in str(excinfo.value)


def test_daily_window_requires_the_market_flag():
    with pytest.raises(DerivedRegistryError):
        validate_definitions(
            (
                _definition(
                    metric_code="bad_flag",
                    expression="safe_div(revenue, close)",
                    inputs=("item:revenue", "market:close"),
                    window="daily",
                    requires_market=False,
                ),
            ),
            item_codes=_ITEMS,
        )


def test_daily_function_in_a_quarterly_metric_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (_definition(metric_code="bad_fn", expression="tret(21)", inputs=()),),
            item_codes=_ITEMS,
        )
    assert "tret" in str(excinfo.value)


def test_quarter_window_function_on_a_non_grid_metric_reference_is_rejected():
    definitions = (
        _definition(
            metric_code="annual_thing",
            expression="revenue",
            inputs=("item:revenue",),
            window="annual",
        ),
        _definition(
            metric_code="bad_grid",
            expression="ttm(annual_thing)",
            inputs=("metric:annual_thing",),
            window="ttm",
        ),
    )
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(definitions, item_codes=_ITEMS)
    assert "annual_thing" in str(excinfo.value)


def test_topological_order_places_dependencies_first():
    ordered = topological_order(default_derived_definitions())
    positions = {definition.metric_code: index for index, definition in enumerate(ordered)}
    assert positions["gross_profit_q"] < positions["gross_profit_ttm"]
    assert positions["gross_profit_ttm"] < positions["gross_margin"]
    assert positions["total_assets_avg2"] < positions["gross_profitability"]


def test_topological_order_is_stable_across_input_permutations():
    definitions = default_derived_definitions()
    forward = [d.metric_code for d in topological_order(definitions)]
    backward = [d.metric_code for d in topological_order(tuple(reversed(definitions)))]
    assert forward == backward


def test_cycle_is_rejected_by_name():
    cyclic = (
        _definition(metric_code="alpha_metric", expression="beta_metric", inputs=("metric:beta_metric",)),
        _definition(metric_code="beta_metric", expression="alpha_metric", inputs=("metric:alpha_metric",)),
    )
    with pytest.raises(DerivedRegistryError) as excinfo:
        topological_order(cyclic)
    message = str(excinfo.value)
    assert "alpha_metric" in message and "beta_metric" in message


def test_seeding_writes_every_definition_with_its_rank(tmp_store):
    count = seed_derived_metric_definitions(tmp_store)
    assert count == len(default_derived_definitions())
    rows = tmp_store.con.execute(
        "SELECT metric_code, metric_window, topological_rank FROM derived_metric_definitions ORDER BY metric_code"
    ).fetchall()
    assert len(rows) == count
    ranks = {str(code): int(rank) for code, _window, rank in rows}
    assert ranks["gross_profit_q"] < ranks["gross_profit_ttm"] < ranks["gross_margin"]


def test_seeding_is_idempotent(tmp_store):
    first = seed_derived_metric_definitions(tmp_store)
    second = seed_derived_metric_definitions(tmp_store)
    assert first == second
    total = tmp_store.con.execute("SELECT count(*) FROM derived_metric_definitions").fetchone()[0]
    assert total == first
