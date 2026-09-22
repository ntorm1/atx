"""Shared compatibility engine contracts, independent of frozen retirement data."""

from __future__ import annotations

import datetime as dt
import json
from dataclasses import replace
from pathlib import Path
from unittest.mock import MagicMock

import pandas as pd
import pytest

from atx_db.derived_compatibility import build_compatibility_sql, load_compatibility_inputs, market_inputs
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
    for identity, name, hour in (("visible-earlier", "VISIBLE", 21),
                                 ("visible-after-cutoff", "VISIBLE", 23),
                                 ("all-after-cutoff", "AFTER_CUTOFF", 23)):
        tmp_store.con.execute(
            "INSERT INTO fundamental_factor_values "
            "(factor_value_id,factor_id,factor_name,family,security_id,symbol,as_of_date,raw_value,value,"
            "available_at,input_ids_json,input_lineage_json,is_latest_revision,source) "
            "VALUES (?,?,?,'test',?,?,'2022-01-31',1,1,?,'[]',?,true,?)",
            [identity, parent_id, parent_id, name, name, dt.datetime(2022, 1, 31, hour), lineage, parent_source],
        )
    metric = compatibility_metrics()["legacy_quarterly_working_capital_accruals"]
    rows = load_compatibility_inputs(tmp_store, metric, universe_id="unused-parent-grid")
    assert rows.security_id.tolist() == ["VISIBLE"]
    assert rows.metric_value.tolist() == [0.1]
    assert json.loads(rows.compatibility_inputs_json.iloc[0])["parent"]["factor_value_id"] == "visible-earlier"


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


EV_BOUNDARY_PATH = Path(__file__).with_name("data") / "derived_ev_boundary_expected.json"


def _ev_boundary_fact_tables():
    """Small independent quarter/annual source facts and competing parent vintages."""
    from tests.test_enterprise_value import _share_history_row, _statement_row

    tables = {name: [] for name in (
        "fundamental_statement_points", "fundamental_ttm_points", "shares_outstanding_history",
        "equity_daily_bars", "universe_membership", "fundamental_factor_values",
    )}
    day = dt.date(2022, 3, 31)
    known = dt.datetime(2022, 2, 15, 18)
    periods = [dt.date(2021, month, end) for month, end in ((3, 31), (6, 30), (9, 30), (12, 31))]
    for index in range(24):
        security = f"EVB{index:02d}"
        for period in (dt.date(2021, 9, 30), dt.date(2021, 12, 31)):
            if index == 23 and period.month == 9:
                continue  # No pre-midnight candidate: same-day filing can win.
            available = known if period.month == 9 else dt.datetime(2022, 3, 31, 18)
            if index == 22:
                # With no pre-midnight filing, the newer 23:00 candidate wins
                # selection and then fails yield visibility; no fallback to 18:00.
                available = dt.datetime(2022, 3, 31, 18 if period.month == 9 else 23)
            amounts = {"total_debt": (15 + index * .7 + (40 if period.month == 12 else 0)) * 1e6,
                       "pref_stock": 1e6, "minority_int_bs": .5e6,
                       "cash_st_inv": (8 + index * .13 + (3 if period.month == 12 else 0)) * 1e6}
            for code, value in amounts.items():
                row = _statement_row(code, value, security_id=security, symbol=security,
                                     period_end=period, available_at=available)
                row["statement_point_id"] += f"-{period}"
                row["fact_revision_id"] = row["revision_group_id"] = row["statement_point_id"]
                tables["fundamental_statement_points"].append(row)
        totals = {}
        for code, base in (("revenue", 40), ("operating_income", 7),
                           ("gross_profit", 16), ("operating_cash_flow", 9)):
            points = []
            for quarter, period in enumerate(periods, 1):
                value = (base + index * (.31 + quarter * .017) + quarter * .4) * 1e6
                point = _statement_row(code, value, security_id=security, symbol=security,
                                       period_end=period, available_at=known)
                point.update(statement_point_id=f"{security}-{code}-q{quarter}",
                             period_type="duration", period_start=dt.date(2021, 3 * quarter - 2, 1))
                point["fact_revision_id"] = point["revision_group_id"] = point["statement_point_id"]
                points.append(point)
            tables["fundamental_statement_points"].extend(points)
            total = sum(point["value"] for point in points)
            totals[code] = total
            if code == "gross_profit":
                annual = dict(points[-1], statement_point_id=f"{security}-gross-annual",
                              period_start=dt.date(2021, 1, 1), value=total, raw_value=total)
                annual["fact_revision_id"] = annual["revision_group_id"] = annual["statement_point_id"]
                tables["fundamental_statement_points"].append(annual)
                continue
            identity = f"{security}-{code}-ttm"
            tables["fundamental_ttm_points"].append({
                "ttm_point_id": identity, "ttm_revision_group_id": identity,
                "anchor_statement_point_id": points[-1]["statement_point_id"],
                "source": "boundary", "security_id": security, "symbol": security, "cik": "0000000001",
                "statement_type": "fixture", "statement_section": "fixture", "canonical_metric": code,
                "canonical_label": code, "unit": "USD", "unit_type": "monetary",
                "ttm_start_date": dt.date(2021, 1, 1), "ttm_end_date": periods[-1],
                "as_of_date": known.date(), "available_at": known, "accession_number": f"{security}-annual",
                "quarter_count": 4, "coverage_days": 365, "min_input_available_at": known,
                "max_input_available_at": known, "input_statement_point_ids_json": json.dumps([
                    point["statement_point_id"] for point in points]),
                "input_accessions_json": json.dumps([point["accession_number"] for point in points]),
                "input_period_ends_json": json.dumps([str(period) for period in periods]),
                "ttm_value": total, "revision_sequence": 1, "revision_count": 1,
                "is_latest_revision": True, "is_value_changed": True,
                "calculation_method": "four_actual_quarters", "source_loaded_at": known,
            })
        share = _share_history_row("shares_outstanding", 10e6 + index * 110000)
        share.update(share_history_id=f"{security}-shares", security_id=security, symbol=security,
                     concept="EntityCommonStockSharesOutstanding", taxonomy="dei")
        tables["shares_outstanding_history"].append(share)
        tables["equity_daily_bars"].append({"source": "boundary", "security_id": security,
            "symbol": security, "trade_date": day, "close": 20 + index * .41,
            "volume": 1e6, "split_factor": 1., "available_at": dt.datetime(2022, 3, 31, 21)})
        tables["universe_membership"].append({"universe_id": "us_common_equity_liquid_v1",
            "security_id": security, "valid_from": dt.date(2021, 1, 1), "as_of_date": dt.date(2021, 1, 1),
            "is_member": True, "is_latest_revision": True, "reason": "boundary", "rules_json": "{}",
            "available_at": dt.datetime(2021, 1, 1), "source": "boundary"})
        for code, factor_id, lineage in (
            ("gross_profit", "profitability_gross_profitability", {"gross_profit": {
                "value": totals["gross_profit"], "id": f"{security}-gross-annual", "period_end": "2021-12-31"}}),
            ("operating_cash_flow", "profitability_operating_cash_flow_to_assets", {"ttm": {
                "operating_cash_flow_ttm": totals["operating_cash_flow"],
                "operating_cash_flow_ttm_id": f"{security}-operating_cash_flow-ttm", "period_end": "2021-12-31"}}),
        ):
            for tag, hour, minute in (("early", 20, 0), ("late", 21, 30), ("after-cutoff", 23, 0)):
                if index == 21 and tag == "early":
                    continue  # All parent candidates fail the 21:00 market clock.
                tables["fundamental_factor_values"].append({
                    "factor_value_id": f"{security}-{code}-{tag}", "factor_id": factor_id,
                    "factor_name": factor_id, "family": "boundary", "security_id": security, "symbol": security,
                    "as_of_date": day, "raw_value": totals[code] / 300e6, "value": index / 24,
                    "available_at": dt.datetime(2022, 3, 31, hour, minute), "input_ids_json": "[]",
                    "input_lineage_json": json.dumps(lineage), "is_latest_revision": True,
                    "source": f"boundary-{tag}", "source_loaded_at": dt.datetime(2022, 3, 31, hour, minute),
                })
    return tables


def _write_ev_boundary_facts(store):
    from atx_db.warehouse import insert_frame

    for table, records in _ev_boundary_fact_tables().items():
        insert_frame(store, pd.DataFrame(records), table, f"boundary_{table}")


def _freeze_ev_boundary_evidence():
    """Explicit regeneration only: execute genuine Git blobs, never during pytest."""
    import hashlib
    import subprocess
    import sys
    import tempfile
    import types

    from tests.conftest import _cached_schema_template, _close_store, _open_template_copy
    from tests.derived_retirement_fixtures import digest

    baseline = "5b11a272c90e9d51cf6fcad3cb3ac0ed6493f9c3"
    modules, hashes = {}, {}
    for name in ("valuation_multiples", "enterprise_value", "enterprise_yield"):
        path = f"atx-db/src/atx_db/{name}.py"
        source = subprocess.check_output(["git", "show", f"{baseline}:{path}"])
        hashes[path] = hashlib.sha256(source).hexdigest()
        module = types.ModuleType(f"atx_db._ev_boundary_baseline_{name}")
        module.__file__ = f"git:{baseline}:{path}"
        sys.modules[module.__name__] = module
        exec(compile(source, module.__file__, "exec"), module.__dict__)
        modules[name] = module
    with tempfile.TemporaryDirectory(prefix="atx-ev-boundary-") as temporary:
        store = _open_template_copy(_cached_schema_template(), Path(temporary) / "boundary.duckdb")
        try:
            store.con.execute("PRAGMA threads=1")
            store.con.execute("SET memory_limit='256MB'")
            _write_ev_boundary_facts(store)
            modules["valuation_multiples"].refresh_market_cap(store)
            modules["enterprise_value"].refresh_enterprise_value(store)
            count = modules["enterprise_yield"].refresh_enterprise_yield_values(store)
            factors = store.con.execute("SELECT factor_id,security_id,as_of_date,raw_value,value,input_lineage_json "
                "FROM fundamental_factor_values WHERE source=? ORDER BY factor_id,security_id",
                [modules["enterprise_yield"].SOURCE_NAME]).df()
            denominators = store.con.execute("SELECT security_id,period_end,enterprise_value,available_at,input_lineage_json "
                "FROM enterprise_value ORDER BY security_id").df()
            assert count == len(factors) == 90
            assert set(factors.factor_id) == set(modules["enterprise_yield"].VARIANT_FACTOR_IDS.values())
            assert len(denominators) == 24
            artifact = {"metadata": {"baseline_commit": baseline, "source_sha256": hashes,
                "base_input_sha256": digest(_ev_boundary_fact_tables()),
                "source_rows": {table: len(rows) for table, rows in _ev_boundary_fact_tables().items()}},
                "factors": factors.to_dict(orient="records"), "denominators": denominators.to_dict(orient="records")}
            artifact = json.loads(json.dumps(artifact, default=str))
            artifact["metadata"]["output_sha256"] = digest({
                "factors": artifact["factors"], "denominators": artifact["denominators"],
            })
            EV_BOUNDARY_PATH.write_text(json.dumps(artifact, sort_keys=True, indent=2) + "\n")
            print(f"Froze genuine baseline: {len(factors)} factor rows, {len(denominators)} denominators")
        finally:
            _close_store(store)


def test_ev_boundary_selection_matches_genuine_baseline(tmp_store):
    from tests.derived_retirement_fixtures import digest

    artifact = json.loads(EV_BOUNDARY_PATH.read_text())
    assert artifact["metadata"]["baseline_commit"] == "5b11a272c90e9d51cf6fcad3cb3ac0ed6493f9c3"
    assert artifact["metadata"]["base_input_sha256"] == digest(_ev_boundary_fact_tables())
    assert artifact["metadata"]["output_sha256"] == digest({
        "factors": artifact["factors"], "denominators": artifact["denominators"],
    })
    tmp_store.con.execute("PRAGMA threads=1")
    tmp_store.con.execute("SET memory_limit='256MB'")
    _write_ev_boundary_facts(tmp_store)
    denominators = tmp_store.con.execute(market_inputs(enterprise=True).sql).df().set_index("security_id")
    for row in artifact["denominators"]:
        actual = denominators.loc[row["security_id"]]
        assert pd.Timestamp(actual.period_end) == pd.Timestamp(row["period_end"])
        assert actual.enterprise_value == row["enterprise_value"]
        components = json.loads(row["input_lineage_json"])["components"]
        identities = [item.get("input_id", item.get("fact_revision_id")) for name, item in components.items()
                      if name != "market_cap"]
        assert set(actual.enterprise_value_id.split("|")) == set(identities)
    expected = pd.DataFrame(artifact["factors"])
    expected["as_of_date"] = pd.to_datetime(expected.as_of_date).dt.date
    options = FactorProjectionOptions(factor_ids=tuple(sorted(expected.factor_id.unique())))
    assert refresh_projected_factor_values(tmp_store, options) == 90
    actual = tmp_store.con.execute("SELECT factor_id,security_id,as_of_date,raw_value,value,input_lineage_json "
        "FROM fundamental_factor_values WHERE source=?", [options.source]).df()
    actual["as_of_date"] = pd.to_datetime(actual.as_of_date).dt.date
    keys = ["factor_id", "security_id", "as_of_date"]
    assert not actual.duplicated(keys).any()
    joined = expected.merge(actual, on=keys, how="outer", suffixes=("_legacy", "_projection"), indicator=True)
    assert set(joined._merge) == {"both"}
    assert joined.raw_value_projection.tolist() == pytest.approx(joined.raw_value_legacy.tolist(), rel=1e-12, abs=1e-12)
    assert joined.value_projection.tolist() == pytest.approx(joined.value_legacy.tolist(), rel=1e-12, abs=1e-12)
    for row in actual.itertuples():
        inputs = json.loads(row.input_lineage_json)["compatibility_inputs"]
        if "parent" in inputs:
            assert inputs["parent"]["factor_value_id"].endswith("-early")
            assert row.security_id != "EVB21"
        assert row.security_id != "EVB22"
    assert tmp_store.con.execute("SELECT count(*) FROM enterprise_value").fetchone()[0] == 0
