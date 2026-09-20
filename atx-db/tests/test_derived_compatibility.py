"""Shared compatibility engine contracts, independent of frozen retirement data."""

from __future__ import annotations

import datetime as dt
import json
from dataclasses import replace
from unittest.mock import MagicMock

import pandas as pd
import pytest

from atx_db.derived_compatibility import build_compatibility_sql, load_compatibility_inputs
from atx_db.derived_compatibility_catalog import compatibility_metrics
from atx_db.derived_compatibility_parents import refresh_compatibility_parents
from atx_db.derived_factor_projection import (
    FactorProjectionOptions,
    compute_projection_rows,
    default_projections,
    refresh_projected_factor_values,
)
from atx_db.derived_registry import default_derived_definitions


def test_compatibility_names_are_separate_from_conventional_metrics():
    conventional = {m.metric_code for m in default_derived_definitions()}
    compatibility = compatibility_metrics()
    assert len(compatibility) == 23
    assert all(code.startswith("legacy_") for code in compatibility)
    assert not conventional.intersection(compatibility)
    assert {p.metric_code for p in default_projections() if p.source_window == "legacy"} == set(compatibility)
    # These conventional definitions retain their corrected/standard meanings.
    assert {"rsst_accruals", "asset_turnover", "shares_growth_yoy", "tax_to_book_income"} <= conventional


def test_compatibility_selectors_bind_to_the_current_schema(tmp_store):
    for code, metric in compatibility_metrics().items():
        result = tmp_store.con.execute(build_compatibility_sql(metric, "empty-universe")).df()
        assert result.empty, code
        assert {"security_id", "as_of_date", "metric_value", "decision_available_at", "compatibility_inputs_json"} <= set(result)


def test_working_capital_keeps_raw_measure_and_reverses_only_standardized_orientation():
    projection = next(p for p in default_projections() if p.retired_module == "quarterly_working_capital_accruals")
    projection = replace(projection, minimum_names_per_date=3, winsor_limit=0)
    inputs = pd.DataFrame({
        "security_id": ["A", "B", "C"], "symbol": ["A", "B", "C"],
        "as_of_date": [dt.date(2022, 1, 31)] * 3, "period_end": [dt.date(2021, 12, 31)] * 3,
        "metric_value": [1., 2., 3.],
        "metric_available_at": [dt.datetime(2022, 1, 20)] * 3,
        "decision_available_at": [dt.datetime(2022, 1, 31, 21)] * 3,
    })
    rows = compute_projection_rows(inputs, projection, FactorProjectionOptions())
    assert rows.raw_value.tolist() == [1., 2., 3.]
    assert rows.value.tolist() == pytest.approx([1., 0., -1.])


def test_compatibility_does_not_depend_on_deprecated_valuation_tables():
    for metric in compatibility_metrics().values():
        sql = build_compatibility_sql(metric, "test")
        for table in ("market_cap", "enterprise_value", "valuation_multiples"):
            assert f"FROM {table} " not in sql
            assert f"JOIN {table} " not in sql


def test_retained_factor_is_published_only_by_explicit_projection_opt_in(monkeypatch):
    selected: list[str] = []

    def load(store, projection, options):
        selected.append(projection.factor_id)
        return pd.DataFrame()

    monkeypatch.setattr("atx_db.derived_factor_projection.load_projection_inputs", load)
    store = MagicMock()
    assert refresh_projected_factor_values(store) == 0
    assert len(selected) == 23
    assert "investment_conservative_asset_growth" not in selected
    selected.clear()
    refresh_projected_factor_values(store, FactorProjectionOptions(factor_ids=("investment_conservative_asset_growth",)))
    assert selected == ["investment_conservative_asset_growth"]


def test_parent_helper_scopes_history_and_reports_empty_cohorts(monkeypatch):
    calls = []

    def record(store, options):
        calls.append(options)
        return 0

    module = "atx_db.derived_compatibility_parents"
    monkeypatch.setattr(f"{module}.refresh_quarterly_operating_profitability_values", record)
    monkeypatch.setattr(f"{module}.refresh_quarterly_revenue_growth_values", record)
    start, end = dt.date(2022, 1, 1), dt.date(2022, 3, 31)
    result = refresh_compatibility_parents(MagicMock(), FactorProjectionOptions(
        factor_ids=("profitability_quarterly_operating_profitability_change_yoy",),
        start_date=start, end_date=end, universe_id="governed-test", run_id="test-run",
    ))
    assert result.selected_factor_count == 1
    assert result.empty_parents == ("quarterly_operating_profitability", "quarterly_revenue_growth")
    assert calls[0].start_date == start-dt.timedelta(days=600)
    assert calls[1].start_date == start
    assert all(call.end_date == end and call.run_id == "test-run" for call in calls)
    assert calls[0].universe_id == "governed-test"


def test_legacy_child_waits_for_a_later_peer_in_the_broader_parent_cohort():
    projection = replace(
        next(p for p in default_projections() if p.retired_module == "quarterly_working_capital_accruals"),
        minimum_names_per_date=3, winsor_limit=0,
    )
    # The child has three eligible names. Their parents were publishable only
    # after a fourth, ineligible-for-the-child peer became available at 21:45.
    inputs = pd.DataFrame([
        {"security_id": name, "symbol": name, "as_of_date": dt.date(2022, 1, 31),
         "period_end": dt.date(2021, 12, 31), "metric_value": value,
         "metric_available_at": dt.datetime(2022, 1, 31, 21),
         "decision_available_at": dt.datetime(2022, 1, 31, 21, minute),
         "compatibility_inputs_json": json.dumps({"parent_cohort_available_at": "2022-01-31T21:45:00"})}
        for name, value, minute in (("A", 1., 0), ("B", 2., 5), ("C", 9., 10), ("FUTURE", 80., 59))
    ])
    inputs.loc[inputs.security_id == "FUTURE", "decision_available_at"] = pd.Timestamp("2022-01-31 23:00")
    rows = compute_projection_rows(inputs, projection, FactorProjectionOptions())
    assert rows.security_id.tolist() == ["A", "B", "C"]
    assert rows.value.tolist() == pytest.approx([3/19**0.5, 2/19**0.5, -5/19**0.5])
    assert set(rows.available_at) == {pd.Timestamp("2022-01-31 22:00")}
    for row in rows.itertuples():
        lineage = json.loads(row.input_lineage_json)
        assert pd.Timestamp(lineage["decision"]["input_available_at"]) < pd.Timestamp("2022-01-31 21:45")
        assert pd.Timestamp(lineage["compatibility_inputs"]["parent_cohort_available_at"]) <= row.available_at


def test_legacy_loader_rejects_parent_decisions_after_the_historical_cutoff(tmp_store):
    parent_id = "profitability_quarterly_cash_operating_profitability_lagged_assets"
    parent_source = "atx-db PIT quarterly cash profitability v1"
    lineage = json.dumps({"net_operating_working_capital_change": 1,
                          "statements": {"lagged_total_assets": 10, "current_period_end": "2021-12-31"}})
    for name, hour in (("VISIBLE", 21), ("AFTER_CUTOFF", 23)):
        tmp_store.con.execute(
            "INSERT INTO fundamental_factor_values "
            "(factor_value_id,factor_id,factor_name,family,security_id,symbol,as_of_date,raw_value,value,"
            "available_at,input_ids_json,input_lineage_json,is_latest_revision,source) "
            "VALUES (?,?,?,'test',?,?,'2022-01-31',1,1,?,'[]',?,true,?)",
            [name, parent_id, parent_id, name, name, dt.datetime(2022, 1, 31, hour), lineage, parent_source],
        )
    metric = compatibility_metrics()["legacy_quarterly_working_capital_accruals"]
    rows = load_compatibility_inputs(tmp_store, metric, universe_id="unused-parent-grid")
    assert rows.security_id.tolist() == ["VISIBLE"]
    assert rows.metric_value.tolist() == [0.1]


def test_retirement_migration_preserves_existing_definitions_and_is_idempotent(tmp_store):
    from atx_db.migrations.bodies_0309 import _retire_declarations
    from tests.derived_retirement_fixtures import FACTOR_IDS

    before = tmp_store.con.execute("SELECT * EXCLUDE(declared_in) FROM factor_definition ORDER BY factor_id").df()
    retained = tmp_store.con.execute(
        "SELECT * FROM factor_definition WHERE factor_id='investment_conservative_asset_growth'"
    ).fetchall()
    _retire_declarations(tmp_store.con)
    after = tmp_store.con.execute("SELECT * EXCLUDE(declared_in) FROM factor_definition ORDER BY factor_id").df()
    pd.testing.assert_frame_equal(before.reset_index(drop=True), after[after.factor_id.isin(before.factor_id)].reset_index(drop=True))
    retired = tmp_store.con.execute(
        "SELECT factor_id,declared_in FROM factor_definition WHERE factor_id IN ("+
        ",".join("?" for _ in FACTOR_IDS)+")", list(FACTOR_IDS),
    ).fetchall()
    assert set(retired) == {(factor_id, "atx_db.derived_factor_projection") for factor_id in FACTOR_IDS}
    assert tmp_store.con.execute(
        "SELECT * FROM factor_definition WHERE factor_id='investment_conservative_asset_growth'"
    ).fetchall() == retained
    first = tmp_store.con.execute("SELECT * FROM factor_definition ORDER BY factor_id").fetchall()
    _retire_declarations(tmp_store.con)
    assert tmp_store.con.execute("SELECT * FROM factor_definition ORDER BY factor_id").fetchall() == first


@pytest.mark.parametrize("empty,experimental", [(False, False), (True, False), (False, True)])
def test_refresh_supersedes_only_owned_legacy_rows_in_scope(tmp_store, monkeypatch, empty, experimental):
    projection = replace(next(p for p in default_projections() if p.retired_module == "net_issuance"), minimum_names_per_date=3)
    for date in (dt.date(2022, 1, 31), dt.date(2022, 2, 28), dt.date(2022, 3, 31)):
        tmp_store.con.execute(
            "INSERT INTO fundamental_factor_values "
            "(factor_value_id,factor_id,factor_name,family,security_id,symbol,as_of_date,raw_value,value,"
            "available_at,input_ids_json,input_lineage_json,is_latest_revision,source) "
            "VALUES (?,?,?,'test','A','A',?,9,9,?,'[]','{}',true,?)",
            [str(date), projection.factor_id, projection.factor_name, date,
             dt.datetime.combine(date, dt.time(21)), projection.legacy_source],
        )
    tmp_store.con.execute(
        "INSERT INTO fundamental_factor_values "
        "(factor_value_id,factor_id,factor_name,family,security_id,symbol,as_of_date,raw_value,value,"
        "available_at,input_ids_json,input_lineage_json,is_latest_revision,source) "
        "SELECT 'unrelated-source',factor_id,factor_name,family,"
        "security_id,symbol,as_of_date,raw_value,value,available_at,input_ids_json,input_lineage_json,"
        "is_latest_revision,'unrelated' "
        "FROM fundamental_factor_values WHERE factor_value_id='2022-02-28'"
    )
    before = tmp_store.con.execute(
        "SELECT * EXCLUDE(is_latest_revision) FROM fundamental_factor_values ORDER BY factor_value_id"
    ).fetchall()
    inputs = pd.DataFrame([
        {"security_id": name, "symbol": name, "as_of_date": dt.date(2022, 2, 28),
         "period_end": dt.date(2021, 12, 31), "metric_value": value,
         "metric_available_at": dt.datetime(2022, 1, 15), "decision_available_at": dt.datetime(2022, 2, 28, 21)}
        for name, value in (("A", 1.), ("B", 2.), ("C", 3.))
    ])
    monkeypatch.setattr("atx_db.derived_factor_projection.default_projections", lambda: (projection,))
    monkeypatch.setattr("atx_db.derived_factor_projection.load_projection_inputs", lambda *args: pd.DataFrame() if empty else inputs)
    source = "experimental" if experimental else "atx-db derived factor projection v1"
    count = refresh_projected_factor_values(tmp_store, FactorProjectionOptions(
        source=source, start_date=dt.date(2022, 2, 28), end_date=dt.date(2022, 2, 28),
    ))
    assert count == (0 if empty else 3)
    state = dict(tmp_store.con.execute(
        "SELECT factor_value_id,is_latest_revision FROM fundamental_factor_values WHERE source<>?", [source]
    ).fetchall())
    assert state == {"2022-01-31": True, "2022-02-28": experimental, "2022-03-31": True, "unrelated-source": True}
    assert tmp_store.con.execute(
        "SELECT * EXCLUDE(is_latest_revision) FROM fundamental_factor_values WHERE source<>? ORDER BY factor_value_id", [source]
    ).fetchall() == before
