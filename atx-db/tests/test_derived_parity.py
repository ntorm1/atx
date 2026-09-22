"""Complete frozen legacy regression evidence and generic projection PIT contracts."""

from __future__ import annotations

import datetime as dt
import gzip
import importlib
import json
from collections import Counter
from dataclasses import replace
from pathlib import Path

import pandas as pd
import pytest

from atx_db.derived_compatibility_parents import refresh_compatibility_parents
from atx_db.derived_factor_projection import (
    FactorProjection,
    FactorProjectionOptions,
    compute_projection_rows,
    default_projections,
    load_projection_inputs,
    refresh_projected_factor_values,
)
from atx_db.derived_registry import DERIVED_SOURCE_NAME
from atx_db.market_daily import MARKET_DAILY_SOURCE_NAME
from atx_db.warehouse import insert_frame
from tests.conftest import _close_store, _open_template_copy
from tests.derived_retirement_fixtures import (
    DATA_DIR,
    FACTOR_IDS,
    MODULE_FACTORS,
    build_retirement_fact_tables,
    digest,
    frozen_expected_rows,
)

PROJECTION_SOURCE = "atx-db derived factor projection v1"
RETIRED_MODULES = tuple(MODULE_FACTORS)
RETIRED_BUILD_SCRIPTS = (
    "build_altman_distress.py", "build_beneish_m_score.py", "build_net_operating_assets.py",
    "build_rsst_accruals.py", "build_quarterly_working_capital_accruals.py",
    "build_quarterly_gross_margin_change.py", "build_quarterly_profitability_change.py",
    "build_external_financing.py", "build_net_debt_financing.py", "build_net_issuance.py",
    "build_net_payout.py", "build_rd_increase.py", "build_tax_expense_momentum.py", "build_tax_to_book_income.py",
)


@pytest.fixture(scope="module")
def parity_warehouse(_schema_template, tmp_path_factory):
    """Same raw facts as the frozen baseline, with no deprecated valuation tables."""
    store = _open_template_copy(_schema_template, tmp_path_factory.mktemp("retirement") / "parity.duckdb")
    try:
        for table, rows in build_retirement_fact_tables().items():
            insert_frame(store, pd.DataFrame(rows), table, f"parity_{table}")
        mismatch = store.con.execute(
            "SELECT count(*) FROM fundamental_standardized s "
            "FULL OUTER JOIN fundamental_statement_points p "
            "ON p.statement_point_id=s.standardized_id "
            "WHERE s.value IS DISTINCT FROM p.value OR s.available_at IS DISTINCT FROM p.available_at"
        ).fetchone()[0]
        assert mismatch == 0, "Both input layers must contain exactly the same facts and clocks"
        parent_result = refresh_compatibility_parents(store)
        assert parent_result.selected_factor_count == 23
        assert not parent_result.empty_parents
        refresh_projected_factor_values(store)
        yield store
    finally:
        _close_store(store)


def test_projection_covers_the_independent_retirement_contract():
    projections = default_projections()
    expected = {name: len(factors) for name, factors in MODULE_FACTORS.items()}
    assert Counter(p.retired_module for p in projections) == {**expected, "KEPT": 1}
    assert {p.factor_id for p in projections if p.retired_module != "KEPT"} == set(FACTOR_IDS)
    assert len(projections) == len({p.factor_id for p in projections}) == 24


def test_frozen_evidence_identifies_actual_legacy_sources_and_the_same_input_facts():
    with gzip.open(DATA_DIR / "derived_retirement_expected.json.gz", "rt", encoding="utf-8") as stream:
        artifact = json.load(stream)
    metadata = artifact["metadata"]
    assert metadata["baseline_commit"] == "5b11a272c90e9d51cf6fcad3cb3ac0ed6493f9c3"
    assert metadata["base_input_sha256"] == digest(build_retirement_fact_tables())
    assert set(metadata["factor_ids"]) == set(FACTOR_IDS)
    assert set(metadata["factor_counts"]) == set(FACTOR_IDS)
    assert all(f"src/atx_db/{name}.py" in metadata["source_sha256"] for name in MODULE_FACTORS)


@pytest.mark.parametrize("module_name", RETIRED_MODULES)
def test_retired_module_is_gone(module_name):
    with pytest.raises(ModuleNotFoundError):
        importlib.import_module(f"atx_db.{module_name}")


def test_retired_build_scripts_are_gone():
    scripts = Path(__file__).resolve().parents[1] / "scripts"
    assert len(RETIRED_BUILD_SCRIPTS) == 14
    for name in RETIRED_BUILD_SCRIPTS:
        assert not (scripts / name).exists(), name


def test_retired_factor_declarations_are_registered_on_a_fresh_warehouse(tmp_store):
    stale = tmp_store.con.execute(
        "SELECT factor_id FROM factor_definition WHERE declared_in IN ("+
        ",".join("?" for _ in RETIRED_MODULES)+")", [f"atx_db.{name}" for name in RETIRED_MODULES],
    ).fetchall()
    assert not stale
    rows = tmp_store.con.execute(
        "SELECT factor_id,declared_in FROM factor_definition WHERE factor_id IN ("+
        ",".join("?" for _ in FACTOR_IDS)+")", list(FACTOR_IDS),
    ).fetchall()
    assert set(rows) == {(factor_id, "atx_db.derived_factor_projection") for factor_id in FACTOR_IDS}


def test_all_retirement_values_and_complete_keys_match_frozen_legacy(parity_warehouse, record_property):
    expected = frozen_expected_rows()
    actual = parity_warehouse.con.execute(
        "SELECT factor_id,security_id,as_of_date,raw_value,value,available_at "
        "FROM fundamental_factor_values WHERE source=?", [PROJECTION_SOURCE]
    ).df()
    actual["as_of_date"] = pd.to_datetime(actual["as_of_date"]).dt.date
    keys = ["factor_id", "security_id", "as_of_date"]
    assert not actual.duplicated(keys).any()
    assert set(actual.factor_id) == set(FACTOR_IDS)
    compared = expected.merge(actual, on=keys, how="outer", suffixes=("_legacy", "_projection"), indicator=True)
    failures = []
    evidence = []
    for factor_id in FACTOR_IDS:
        rows = compared[compared.factor_id == factor_id]
        missing = rows[rows._merge != "both"]
        if not missing.empty:
            failures.append(f"{factor_id}: {len(missing)} unmatched keys; {missing[[*keys, '_merge']].head(3).to_dict('records')}")
        matched = rows[rows._merge == "both"]
        if matched.empty:
            failures.append(f"{factor_id}: no compared observations")
            continue
        assert matched.security_id.nunique() >= 20
        raw_error = ((matched.raw_value_projection-matched.raw_value_legacy).abs()/matched.raw_value_legacy.abs().clip(lower=1)).max()
        zscore_error = (matched.value_projection-matched.value_legacy).abs().max()
        if raw_error > 1e-9 or zscore_error > 1e-9:
            failures.append(f"{factor_id}: scaled raw error={raw_error}, zscore error={zscore_error}")
        evidence.append({"factor_id": factor_id,"rows":len(matched),"dates":int(matched.as_of_date.nunique()),
                         "securities":int(matched.security_id.nunique()),"raw_error":float(raw_error),"zscore_error":float(zscore_error)})
    record_property("retirement_evidence", json.dumps(evidence))
    assert not failures, "\n".join(failures)


def test_retirement_projection_availability_covers_the_entire_actual_cohort(parity_warehouse):
    actual = parity_warehouse.con.execute(
        "SELECT factor_id,security_id,as_of_date,available_at,input_lineage_json "
        "FROM fundamental_factor_values WHERE source=?", [PROJECTION_SOURCE]
    ).df()
    assert set(actual.factor_id) == set(FACTOR_IDS)
    for _, cohort in actual.groupby(["factor_id", "as_of_date"]):
        lineages = [json.loads(value) for value in cohort.input_lineage_json]
        latest_input = max(pd.Timestamp(value["decision"]["input_available_at"]) for value in lineages)
        cutoff = pd.Timestamp(cohort.as_of_date.iloc[0]) + pd.Timedelta(hours=22)
        assert set(cohort.available_at) == {cutoff}
        for lineage in lineages:
            assert pd.Timestamp(lineage["metric"]["available_at"]) <= latest_input
            assert pd.Timestamp(lineage["decision"]["available_at"]) == cutoff
        assert latest_input <= cutoff
    # The normal replacement path needs no deprecated denominator materialization.
    assert parity_warehouse.con.execute("SELECT count(*) FROM market_cap").fetchone()[0] == 0
    assert parity_warehouse.con.execute("SELECT count(*) FROM enterprise_value").fetchone()[0] == 0


@pytest.fixture
def projection_store(tmp_store):
    """Small independent grid for PIT and scoped-refresh regression checks."""
    con = tmp_store.con
    for index, security_id in enumerate(("A", "B", "C")):
        con.execute(
            "INSERT INTO universe_membership "
            "(universe_id,security_id,valid_from,as_of_date,is_member,source,available_at,reason,rules_json) "
            "VALUES ('us_common_equity_liquid_v1',?,'2021-01-01','2021-01-01',"
            "true,'test','2021-01-01','member','{}')",
            [security_id],
        )
        for date in (dt.date(2022, 1, 31), dt.date(2022, 2, 28), dt.date(2022, 3, 31)):
            con.execute(
                "INSERT INTO equity_daily_bars "
                "(source,security_id,symbol,trade_date,close,available_at) VALUES ('test',?,?,?,10,?)",
                [security_id, security_id, date, dt.datetime.combine(date, dt.time(21))],
            )
        for seq, (period, available, value) in enumerate(
            (
                ("2021-09-30", "2022-01-01", 10.0),
                ("2021-12-31", "2022-01-15", 20.0),
                ("2021-12-31", "2022-02-15", 25.0),
                ("2021-09-30", "2022-03-01", 999.0),
                ("2021-12-31", "2022-04-01", 30.0),
            )
        ):
            con.execute(
                "INSERT INTO derived_metric_values "
                "(derived_value_id,source,security_id,metric_code,metric_window,period_end,"
                "value,available_at,inputs_hash,as_of_date,is_latest_revision) "
                "VALUES (?, ?,?,'asset_growth','q',?,?,?,'test',?,?)",
                [
                    f"{security_id}-{seq}",
                    DERIVED_SOURCE_NAME,
                    security_id,
                    period,
                    value + index,
                    available,
                    available,
                    seq == 4,
                ],
            )
    return tmp_store


def test_projection_standardized_values_wait_for_eligible_cohort():
    projection = FactorProjection(
        factor_id="test_factor",
        metric_code="test_metric",
        source_window="quarter",
        orientation=1,
        factor_name="Test factor",
        family="test",
        winsor_limit=0.0,
        minimum_names_per_date=3,
        retired_module="KEPT",
    )
    inputs = []
    for day, hour, minutes in ((31, 21, (0, 30, 15)), (30, 20, (0, 10, 5))):
        date = dt.date(2022, 1, day)
        for security_id, value, minute in zip(("A", "B", "C"), (1.0, 2.0, 9.0), minutes, strict=True):
            available_at = dt.datetime.combine(date, dt.time(hour, minute))
            inputs.append(
                {
                    "security_id": security_id,
                    "symbol": security_id,
                    "as_of_date": date,
                    "metric_value": value,
                    "metric_available_at": available_at - dt.timedelta(minutes=5),
                    "decision_available_at": available_at,
                    "period_end": dt.date(2021, 12, 31),
                }
            )
    # An ineligible peer must not postpone publication of the actual cohort.
    inputs.append(
        {
            **inputs[0],
            "security_id": "INELIGIBLE",
            "metric_value": float("inf"),
            "decision_available_at": dt.datetime(2022, 1, 31, 22),
        }
    )
    frame = pd.DataFrame(inputs)
    rows = compute_projection_rows(frame, projection, FactorProjectionOptions())
    assert len(rows) == 6
    for date, expected in (
        (dt.date(2022, 1, 31), pd.Timestamp("2022-01-31 21:30")),
        (dt.date(2022, 1, 30), pd.Timestamp("2022-01-30 20:10")),
    ):
        cohort = rows[rows["as_of_date"] == date]
        assert set(cohort["security_id"]) == {"A", "B", "C"}
        assert set(cohort["available_at"]) == {expected}
        first = cohort[cohort["security_id"] == "A"].iloc[0]
        assert first["raw_value"] == 1.0
        # Sample standard deviation of (1, 2, 9) is sqrt(19): the later peer
        # contributes to the earlier security's actual published score.
        assert first["value"] == pytest.approx(-3.0 / 19.0**0.5)
        for row in cohort.itertuples():
            lineage = json.loads(row.input_lineage_json)
            own_input = frame[(frame["as_of_date"] == date) & (frame["security_id"] == row.security_id)].iloc[0]
            assert pd.Timestamp(lineage["decision"]["available_at"]) == expected
            assert pd.Timestamp(lineage["decision"]["input_available_at"]) == own_input["decision_available_at"]
            assert pd.Timestamp(lineage["metric"]["available_at"]) == own_input["metric_available_at"]


def test_projection_selects_latest_known_period_and_preserves_historical_revisions(projection_store):
    projection = next(p for p in default_projections() if p.retired_module == "KEPT")
    rows = load_projection_inputs(projection_store, projection, FactorProjectionOptions())
    actual = rows[rows["security_id"] == "A"]
    assert actual["metric_value"].tolist() == [20.0, 25.0, 25.0]
    assert actual["metric_available_at"].dt.date.tolist() == [
        dt.date(2022, 1, 15),
        dt.date(2022, 2, 15),
        dt.date(2022, 2, 15),
    ]
    assert set(actual["period_end"].dt.date) == {dt.date(2021, 12, 31)}


def test_projection_daily_rows_respect_cutoff_and_choose_one_revision(projection_store):
    projection = replace(
        next(p for p in default_projections() if p.metric_code == "legacy_rd_to_market_equity"),
        metric_code="rd_to_market_equity", source_window="daily",
    )
    con = projection_store.con
    con.execute(
        "INSERT INTO equity_daily_bars (source,security_id,symbol,trade_date,close,available_at) "
        "VALUES ('late','A','A','2022-01-31',30,'2022-01-31 23:00:00')"
    )
    for suffix, hour, value in (("early", 20, 1.0), ("latest", 21, 2.0), ("future", 23, 99.0)):
        con.execute(
            "INSERT INTO market_daily_metrics "
            "(market_daily_id,source,security_id,trade_date,rd_to_market_equity,available_at,inputs_hash,as_of_date) "
            "VALUES (?,?,'A','2022-01-31',?,?,'test','2022-01-31')",
            [suffix, MARKET_DAILY_SOURCE_NAME, value, dt.datetime(2022, 1, 31, hour)],
        )
    rows = load_projection_inputs(projection_store, projection, FactorProjectionOptions())
    assert rows["metric_value"].tolist() == [2.0]
    assert rows["metric_available_at"].tolist() == [pd.Timestamp("2022-01-31 21:00")]
    assert rows["decision_available_at"].tolist() == [pd.Timestamp("2022-01-31 21:00")]


@pytest.mark.parametrize("empty_refresh", [False, True])
def test_projection_scoped_refresh_preserves_other_dates(projection_store, monkeypatch, empty_refresh):
    projection = replace(next(p for p in default_projections() if p.retired_module == "KEPT"), minimum_names_per_date=2)
    monkeypatch.setattr("atx_db.derived_factor_projection.default_projections", lambda: (projection,))
    assert refresh_projected_factor_values(
        projection_store, FactorProjectionOptions(factor_ids=(projection.factor_id,))
    ) == 9
    before = projection_store.con.execute(
        "SELECT * FROM fundamental_factor_values WHERE as_of_date <> '2022-02-28' ORDER BY factor_value_id"
    ).fetchall()
    if empty_refresh:
        projection_store.con.execute("DELETE FROM derived_metric_values")
    count = refresh_projected_factor_values(
        projection_store, FactorProjectionOptions(
            factor_ids=(projection.factor_id,), start_date=dt.date(2022, 2, 28), end_date=dt.date(2022, 2, 28)
        )
    )
    assert count == (0 if empty_refresh else 3)
    assert (
        projection_store.con.execute(
            "SELECT * FROM fundamental_factor_values WHERE as_of_date <> '2022-02-28' ORDER BY factor_value_id"
        ).fetchall()
        == before
    )
    assert (
        projection_store.con.execute(
            "SELECT count(*) FROM fundamental_factor_values WHERE as_of_date = '2022-02-28'"
        ).fetchone()[0]
        == count
    )
