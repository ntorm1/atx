"""Constructed FC1 policy fixtures; these do not measure live SEC prevalence."""

from __future__ import annotations

import datetime as dt
import json

import pytest

from atx_db._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY, _effective_relation
from atx_db.activation import ActivationOptions, stage_statement_points
from atx_db.asof.fundamentals import FUNDAMENTALS_ASOF_SQL, fundamentals_asof
from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.estimates import EstimateActualsDataset, EstimateActualsOptions
from atx_db.features import FundamentalFeatureBuildOptions, FundamentalFeatureDataset
from atx_db.fundamental_statements import (
    refresh_fundamental_periods,
    refresh_fundamental_statement_points,
)
from atx_db.fundamental_xbrl_metrics import _fetch_companyfacts_candidates
from atx_db.fundamentals import refresh_fundamental_fact_revisions
from atx_db.market_daily import MarketDailyOptions, refresh_market_daily_metrics
from atx_db.migrations.bodies_0319 import _fundamental_effective_clock
from atx_db.quality.checks_market_reference import market_reference_check_specs
from atx_db.standardization import FundamentalStandardizationOptions, refresh_fundamental_standardized


def _fact(store, start, end, filed, value, *, accession=None, concept="Revenues", stored=None):
    filed = dt.date.fromisoformat(filed)
    end = dt.date.fromisoformat(end)
    accession = accession or f"accession-{end}-{filed}"
    stored = stored or dt.datetime.combine(filed, dt.time(22))
    taxonomy = "dei" if concept == "EntityCommonStockSharesOutstanding" else "us-gaap"
    unit = "shares" if taxonomy == "dei" else "USD"
    store.con.execute("""
        INSERT INTO sec_company_facts (
            source, security_id, cik, taxonomy, concept, unit, period_start, period_end,
            filed_date, fiscal_year, fiscal_period, form, accession_number, value,
            available_at, source_url, source_loaded_at
        ) VALUES ('SEC companyfacts', 'clock-issuer', '0000000001', ?, ?, ?, ?, ?, ?, ?, ?,
                  '10-Q', ?, ?, ?, 'fixture', TIMESTAMP '2024-06-01')
    """, [taxonomy, concept, unit, start, end, filed, end.year, f"Q{(end.month - 1) // 3 + 1}",
          accession, value, stored])
    store.con.execute("""
        INSERT INTO fundamental_points (
            source, security_id, symbol, metric, taxonomy, unit, period_start, period_end,
            as_of_date, fiscal_year, fiscal_period, form, accession_number, value,
            available_at, source_loaded_at
        ) VALUES ('SEC companyfacts', 'clock-issuer', 'CLK', ?, ?, ?, ?, ?, ?, ?, ?,
                  '10-Q', ?, ?, ?, TIMESTAMP '2024-06-01')
    """, [concept, taxonomy, unit, start, end, filed, end.year, f"Q{(end.month - 1) // 3 + 1}",
          accession, value, stored])


def _bar(store, day):
    store.con.execute("""
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close, adjusted_close,
            volume, is_adjusted, available_at, as_of_date, is_latest_revision, shares_outstanding
        ) VALUES ('fixture', 'clock-issuer', 'CLK', ?, 46, 46, 46, 46, 46, 1000,
                  false, CAST(? AS DATE) + INTERVAL 22 HOUR, ?, true, 100)
    """, [day, day, day])


def _raw_snapshot(store):
    return {
        table: store.con.execute(f"SELECT * FROM {table} ORDER BY accession_number").fetchall()
        for table in ("sec_company_facts", "fundamental_points")
    }


def _raw_asof(store, cutoff):
    sql = FUNDAMENTALS_ASOF_SQL.format(symbol_join="", metric_join="")
    return store.con.execute(sql, [cutoff.date(), cutoff]).df()


@pytest.mark.parametrize(("filed", "decision"), [("2024-02-01", "2024-02-02"),
                                                   ("2024-02-02", "2024-02-05")])
def test_winter_acceptance_raw_standardized_derived_daily_and_weekend(tmp_store, filed, decision):
    store = tmp_store
    store.con.execute("""
        INSERT INTO securities (security_id, primary_symbol, name, source)
        VALUES ('clock-issuer', 'CLK', 'Clock fixture', 'fixture')
    """)
    for start, end, prior_filed, value in (
        ("2022-10-01", "2022-12-31", "2023-02-01", 90),
        ("2023-01-01", "2023-03-31", "2023-05-01", 100),
        ("2023-04-01", "2023-06-30", "2023-08-01", 110),
        ("2023-07-01", "2023-09-30", "2023-11-01", 120),
        ("2023-10-01", "2023-12-31", filed, 130),
    ):
        _fact(store, start, end, prior_filed, value)
    _fact(store, None, "2023-12-31", filed, 100,
          concept="EntityCommonStockSharesOutstanding", accession="shares")
    filed_day = dt.date.fromisoformat(filed)
    same_day = dt.datetime.combine(filed_day, dt.time(22))
    effective = same_day + dt.timedelta(days=1)
    # Constructed acceptance at 17:15 EST, fifteen minutes after the decision.
    # It and a different, earlier earnings release must not accelerate FC1.
    store.con.execute("""
        INSERT INTO sec_submissions (
            security_id, cik, accession_number, filing_date, report_date,
            acceptance_datetime, form, items, source_url
        ) VALUES ('clock-issuer', '0000000001', ?, ?, DATE '2023-12-31', ?, '10-Q', '', 'fixture'),
                 ('clock-issuer', '0000000001', 'release', DATE '2024-01-25',
                  DATE '2024-01-25', TIMESTAMP '2024-01-25 18:00:00', '8-K', '2.02', 'fixture')
    """, [f"accession-2023-12-31-{filed}", filed_day, same_day + dt.timedelta(minutes=15)])
    raw_before = _raw_snapshot(store)
    options = ActivationOptions(db_path=store.path, run_id="clock-fixture")
    result = stage_statement_points(store, options)
    assert result.detail["fundamental_clock_policy"] == FUNDAMENTAL_CLOCK_POLICY
    assert result.detail["shares_history_rows"] == 1
    assert store.con.execute("SELECT available_at FROM shares_outstanding_history").fetchall() == [(effective,)]
    refresh_fundamental_periods(store)
    assert store.con.execute("""
        SELECT DISTINCT rdq, available_at FROM fundamental_periods
        WHERE period_end=DATE '2023-12-31'
    """).fetchall() == [(dt.date(2024, 1, 25), effective)]
    refresh_fundamental_standardized(store, FundamentalStandardizationOptions(symbols=("CLK",)))
    assert store.con.execute("""
        SELECT value, available_at FROM fundamental_standardized
        WHERE canonical_code='revenue' AND basis='quarterly' AND period_end=DATE '2023-12-31'
    """).fetchall() == [(130.0, effective)]
    refresh_derived_metrics(store, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    assert store.con.execute("""
        SELECT period_end, value, available_at FROM derived_metric_values
        WHERE metric_code='revenue_ttm' AND value_status='valid' ORDER BY period_end
    """).fetchall() == [
        (dt.date(2023, 9, 30), 420.0, dt.datetime(2023, 11, 2, 22)),
        (dt.date(2023, 12, 31), 460.0, effective),
    ]
    before = _raw_asof(store, same_day)
    assert before.loc[before.metric == "Revenues", "value"].tolist() == [90, 100, 110, 120]
    assert 130 not in _raw_asof(store, effective - dt.timedelta(microseconds=1)).value.tolist()
    after = _raw_asof(store, effective)
    after = after.loc[after.metric == "Revenues"]
    assert after.value.tolist() == [90, 100, 110, 120, 130]
    assert after.loc[after.value == 130, "available_at"].iloc[0].to_pydatetime() == effective
    _bar(store, filed)
    _bar(store, decision)
    refresh_market_daily_metrics(store, MarketDailyOptions(security_ids=("clock-issuer",)))
    rows = store.con.execute("SELECT trade_date, ps_ttm FROM market_daily_metrics ORDER BY trade_date").fetchall()
    assert rows == [(filed_day, pytest.approx(4600 / 420)),
                    (dt.date.fromisoformat(decision), pytest.approx(10))]
    # Friday facts become policy-eligible Saturday; no synthetic weekend bar.
    assert _raw_snapshot(store) == raw_before
    store.close()
    public = fundamentals_asof(effective.date(), effective, store.path, symbols=("CLK",), metrics=("Revenues",))
    assert public.value.tolist() == [90, 100, 110, 120, 130]


def test_effective_clock_precedes_revision_order_and_scoped_ids_are_stable(tmp_store):
    store = tmp_store
    _fact(store, "2023-10-01", "2023-12-31", "2024-02-01", 120, accession="a",
          stored=dt.datetime(2024, 2, 2, 23))
    _fact(store, "2023-10-01", "2023-12-31", "2024-02-02", 130, accession="b")
    _fact(store, None, "2023-12-31", "2024-02-01", 900, concept="Assets", accession="assets")
    raw_before = _raw_snapshot(store)
    refresh_fundamental_fact_revisions(store)
    expected = [("a", 1, dt.datetime(2024, 2, 2, 23), None),
                ("b", 2, dt.datetime(2024, 2, 3, 22), dt.datetime(2024, 2, 2, 23))]
    assert store.con.execute("""
        SELECT accession_number, revision_sequence, available_at, previous_available_at
        FROM fundamental_fact_revisions WHERE concept='Revenues' ORDER BY revision_sequence
    """).fetchall() == expected
    refresh_fundamental_statement_points(store)
    snapshots = {
        table: store.con.execute(f"SELECT * EXCLUDE (updated_at) FROM {table} ORDER BY 1").fetchall()
        for table in ("fundamental_fact_revisions", "fundamental_statement_points")
    }
    for _ in range(2):
        refresh_fundamental_fact_revisions(store, ("Revenues",))
        refresh_fundamental_statement_points(store, ("Revenues",))
        for table, snapshot in snapshots.items():
            assert store.con.execute(f"SELECT * EXCLUDE (updated_at) FROM {table} ORDER BY 1").fetchall() == snapshot
    assert _raw_snapshot(store) == raw_before


@pytest.mark.parametrize(("source", "filed", "stored", "expected"), [
    ("SEC companyfacts", "2024-02-01", "2024-02-01 09:00:00", "2024-02-02 22:00:00"),
    ("SEC companyfacts", "2024-07-01", None, "2024-07-02 22:00:00"),
    ("SEC companyfacts", "2024-02-01", "2024-02-04 23:00:00", "2024-02-04 23:00:00"),
    ("SEC companyfacts", None, "2024-02-01 22:00:00", "excluded"),
    ("SEC companyfacts", "invalid", "2024-02-01 22:00:00", "excluded"),
    ("SEC companyfacts", "infinity", "2024-02-01 22:00:00", "excluded"),
    ("other", "invalid", "2024-02-01 09:00:00", "2024-02-01 09:00:00"),
    ("other", None, None, None),
])
def test_unresolved_dates_and_non_sec_clocks(tmp_store, source, filed, stored, expected):
    # VARCHAR fixtures cover malformed input that typed production DATE columns
    # normally reject; no NULL date can pass the effective SEC projection.
    tmp_store.con.execute("CREATE TEMP TABLE clock_rows (source VARCHAR, filed_date VARCHAR, available_at TIMESTAMP)")
    tmp_store.con.execute("INSERT INTO clock_rows VALUES (?, ?, ?)", [source, filed, stored])
    before = tmp_store.con.execute("SELECT * FROM clock_rows").fetchall()
    rows = tmp_store.con.execute(f"SELECT available_at FROM {_effective_relation('clock_rows', 'filed_date')}").fetchall()
    wanted = [] if expected == "excluded" else [(None if expected is None else dt.datetime.fromisoformat(expected),)]
    assert rows == wanted
    assert tmp_store.con.execute("SELECT * FROM clock_rows").fetchall() == before


def test_raw_feature_and_overlap_quality_consumers_share_policy(tmp_store):
    store = tmp_store
    _fact(store, "2023-10-01", "2023-12-31", "2024-02-01", 130)
    raw_before = _raw_snapshot(store)
    _bar(store, "2024-02-01")
    _bar(store, "2024-02-02")
    FundamentalFeatureDataset().load(store, FundamentalFeatureBuildOptions(symbols=("CLK",), run_id="clock"))
    assert store.con.execute("""
        SELECT value, available_at FROM feature_values WHERE feature_name='fund_revenue_reported'
    """).fetchall() == [(130.0, dt.datetime(2024, 2, 2, 22))]
    assert FUNDAMENTAL_CLOCK_POLICY in store.con.execute("""
        SELECT available_at_policy FROM feature_definitions WHERE feature_name='fund_revenue_reported'
    """).fetchone()[0]
    params = json.loads(store.con.execute("SELECT params_json FROM feature_build_manifests").fetchone()[0])
    assert params["fundamental_clock_policy"] == FUNDAMENTAL_CLOCK_POLICY
    store.con.execute("""
        INSERT INTO universe_membership (
            universe_id, security_id, valid_from, valid_to, as_of_date, available_at,
            is_member, is_latest_revision, source, reason, rules_json
        ) VALUES ('clock', 'clock-issuer', DATE '2024-02-01', DATE '2024-02-01',
                  DATE '2024-02-01', TIMESTAMP '2024-02-01', true, true, 'fixture', 'fixture', '{}')
    """)
    _fundamental_effective_clock(store.con)
    assert store.con.execute("""
        SELECT universe_price_days, price_fundamental_days FROM v_price_fundamental_overlap
    """).fetchall() == [(1, 0)]
    check = next(check for check in market_reference_check_specs(
        daily_macro_stale_days=7, monthly_macro_stale_days=60, valuation_stale_gap_days=10
    ) if check.check_name == "priced_fundamental_universe_decision_coverage")
    assert store.con.execute(check.sql).fetchone() == (1.0,)
    assert store.con.execute(check.detail_sql).fetchall()[0][2] == dt.date(2024, 2, 2)
    notes = store.con.execute("SELECT pit_notes FROM table_catalog WHERE table_name='derived_metric_values'").fetchone()
    _fundamental_effective_clock(store.con)
    assert store.con.execute("SELECT pit_notes FROM table_catalog WHERE table_name='derived_metric_values'").fetchone() == notes
    assert notes[0].count(FUNDAMENTAL_CLOCK_POLICY) == 1
    assert _raw_snapshot(store) == raw_before


def test_future_xbrl_and_estimate_actual_builds_use_effective_sec_clock(tmp_store):
    _fact(tmp_store, None, "2023-12-31", "2024-02-01", 900, concept="AssetsCurrent", accession="assets")
    _fact(tmp_store, "2023-01-01", "2023-12-31", "2024-02-01", 40,
          concept="ResearchAndDevelopmentExpense", accession="rd")
    _fact(tmp_store, "2023-10-01", "2023-12-31", "2024-02-01", 130, accession="revenue")
    before = _raw_snapshot(tmp_store)
    candidates = _fetch_companyfacts_candidates(tmp_store, None)
    assert {"AssetsCurrent", "ResearchAndDevelopmentExpense"} <= set(candidates.concept)
    assert candidates.available_at.nunique() == 1
    assert candidates.available_at.iloc[0].to_pydatetime() == dt.datetime(2024, 2, 2, 22)
    EstimateActualsDataset().load(tmp_store, EstimateActualsOptions(measure_codes=("REVENUE",)))
    assert tmp_store.con.execute("SELECT value, available_at FROM est_actual").fetchall() == [
        (130.0, dt.datetime(2024, 2, 2, 22)),
    ]
    assert _raw_snapshot(tmp_store) == before
