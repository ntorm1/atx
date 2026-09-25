"""R1d: point-in-time split epochs and share-basis rebasing in the derived engine."""

from __future__ import annotations

import datetime as dt
import math
import statistics
from pathlib import Path
from types import SimpleNamespace

import duckdb
import pytest

from atx_db import _split_epochs as split_epochs
from atx_db import derived_metrics as engine
from atx_db.derived_registry import DerivedMetricDefinition

BARS_DDL = """
    CREATE TABLE equity_daily_bars (
        source VARCHAR, security_id VARCHAR, trade_date DATE, close DOUBLE,
        adjusted_close DOUBLE, shares_outstanding BIGINT, available_at TIMESTAMP,
        run_id VARCHAR, split_factor DOUBLE)
"""
START, END = dt.date(2018, 12, 1), dt.date(2022, 6, 30)


def _weekdays(start: dt.date, end: dt.date):
    day = start
    while day <= end:
        if day.weekday() < 5:
            yield day
        day += dt.timedelta(days=1)


def _bars(con, security_id, start=START, end=END, *, split=None, dividends=(), shares=None, drop=(),
          factorless=(), flat_factor=False, factor_scale=0.95, source="vendor", run_id=None, split_field=None,
          share_steps=(), price_moves=()):
    """Daily bars whose vendor factor is ``adjusted_close / close``.

    ``factor_scale`` 0.95 is a total-return factor carrying later dividends (a
    load whose factor is 1 everywhere is factor-free and proves nothing).
    ``split=(ex_date, k)``: k new shares per old share from ``ex_date``; the raw
    close moves by 1/k and history is back-adjusted (factor 1/k before the
    split) unless ``flat_factor`` (a vendor factor that misses it).
    ``dividends=((ex_date, cut), ...)``: a total-return factor (1 - cut) before
    each ex-date. ``shares=(before, after, lag_days)`` moves the archive share
    count ``lag_days`` after the ex-date; ``share_steps=((date, count), ...)``
    sets it from a date. ``price_moves=((date, multiplier), ...)`` moves the raw
    close (and the adjusted close with it) from a date. ``split_field`` puts an
    explicit vendor ``split_factor`` on the ex-date bar.
    """
    ex_date, ratio = split if split else (None, 1.0)
    for day in _weekdays(start, end):
        if day in drop:
            continue
        before = ex_date is not None and day < ex_date
        factor = factor_scale * (1.0 if flat_factor or not before else 1.0 / ratio) * math.prod(
            1.0 - cut for date, cut in dividends if day < date)
        close = 10.0 * (ratio if before else 1.0) * math.prod(move for date, move in price_moves if day >= date)
        count = None
        if shares:
            count = shares[0] if (ex_date is None or day < ex_date + dt.timedelta(days=shares[2])) else shares[1]
        for date, value in share_steps:
            if day >= date:
                count = value
        adjusted = None if day in factorless else close * factor
        field = split_field if split_field is not None and day == ex_date else None
        con.execute("INSERT INTO equity_daily_bars VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    [source, security_id, day, close, adjusted, count, dt.datetime.combine(day, dt.time(22)),
                     run_id, field])


def _con():
    con = duckdb.connect(":memory:")
    con.execute("SET memory_limit='256MB'")
    con.execute("SET threads=1")
    con.execute(BARS_DDL)
    return con


def _events(con, security_id):
    split_epochs.prepare_split_epochs(con, security_id)
    return con.execute("SELECT kind, ex_date, available_at, round(ratio, 6), evidence FROM _pit_split_events "
                       "ORDER BY ex_date").fetchall()


def _hazards(con):
    return con.execute("SELECT ex_date, from_at, until_at, round(ratio, 6), reason FROM _pit_split_hazards "
                       "ORDER BY ex_date").fetchall()


def _known(con, clock: dt.datetime, frame: dt.datetime) -> bool:
    sql = split_epochs.basis_known_sql(f"TIMESTAMP '{clock}'", f"TIMESTAMP '{frame}'")
    return con.execute(f"SELECT {sql} FROM (SELECT 1) {split_epochs.LISTS_JOIN}").fetchone()[0]


def _at(day: dt.date) -> dt.datetime:
    return dt.datetime.combine(day, dt.time(22))


def _window_close(ex_date: dt.date) -> dt.date:
    """The last bar of the share window opened on ``ex_date`` (fixture calendar: every weekday)."""
    return list(_weekdays(ex_date, ex_date + dt.timedelta(days=200)))[split_epochs.SHARE_WINDOW_BARS]


# --- detection -------------------------------------------------------------------------------


def test_a_split_is_known_once_the_share_count_follows_the_factor():
    con = _con()
    _bars(con, "FWD", dt.date(2021, 1, 1), dt.date(2021, 9, 30), split=(dt.date(2021, 3, 15), 2.0),
          dividends=((dt.date(2021, 2, 10), 0.02), (dt.date(2021, 5, 10), 0.10)), shares=(1000, 2000, 3))
    _bars(con, "REV", dt.date(2021, 1, 1), dt.date(2021, 6, 30), split=(dt.date(2021, 4, 1), 0.1),
          shares=(1000, 100, 0))
    # The share count moves on 2021-03-18: the split is known from that bar, pending before.
    # A 2 % dividend is no candidate; a 10 % special dividend (k = 10/9, not a stock-dividend
    # ratio) is a distribution at once.
    assert _events(con, "FWD") == [
        ("split", dt.date(2021, 3, 15), _at(dt.date(2021, 3, 18)), 2.0, "vendor_factor+shares"),
        ("distribution", dt.date(2021, 5, 10), _at(dt.date(2021, 5, 10)), 1.111111, "distribution")]
    assert _hazards(con) == [
        (dt.date(2021, 3, 15), _at(dt.date(2021, 3, 15)), _at(dt.date(2021, 3, 18)), 2.0,
         "split_pending_share_confirmation")]
    assert _events(con, "REV") == [
        ("split", dt.date(2021, 4, 1), _at(dt.date(2021, 4, 1)), 0.1, "vendor_factor+shares")]
    assert _hazards(con) == []
    con.close()


EX = dt.date(2021, 3, 15)


@pytest.mark.parametrize(("bars", "events", "hazards"), [
    # N1: an exact 2:1 whose vendor count follows 100 days later is a split from that bar, pending before.
    (dict(split=(EX, 2.0), shares=(1000, 2000, 100)),
     [("split", EX, _at(dt.date(2021, 6, 23)), 2.0, "vendor_factor+shares")],
     [(EX, _at(EX), _at(dt.date(2021, 6, 23)), 2.0, "split_pending_share_confirmation")]),
    # N1: the same with a count that never moves is never a distribution: basis unknown for good.
    (dict(split=(EX, 2.0), shares=(1000, 1000, 0)), [], [(EX, _at(EX), None, 2.0, "split_unconfirmed")]),
    # N1: a 1:30 reverse split (NKLA 2024) with a count 200 days late.
    (dict(split=(EX, 1 / 30), shares=(3000, 100, 200)),
     [("split", EX, _at(dt.date(2021, 10, 1)), 0.033333, "vendor_factor+shares")],
     [(EX, _at(EX), _at(dt.date(2021, 10, 1)), 0.033333, "split_pending_share_confirmation")]),
    # N2: a 7 % distribution (inexact) with a 6 % issuance four weeks later: a distribution at once.
    (dict(dividends=((EX, 0.07),), price_moves=((EX, 0.93),), shares=(1000, 1000, 0),
          share_steps=((dt.date(2021, 4, 12), 1060),)),
     [("distribution", EX, _at(EX), 1.075269, "distribution")], []),
    # N2: an exact 6:5 whose count only drifts 6 % (issuance, not the ratio) is a distribution.
    (dict(split=(EX, 1.2), shares=(1000, 1000, 0), share_steps=((dt.date(2021, 4, 1), 1060),)),
     [("distribution", EX, _at(_window_close(EX)), 1.2, "distribution")],
     [(EX, _at(EX), _at(_window_close(EX)), 1.2, "pending_confirmation")]),
], ids=["n1_count_lags", "n1_count_stale", "n1_reverse_1_30_lags", "n2_distribution_and_issuance",
        "n2_exact_ratio_issuance_drift"])
def test_exact_ratios_wait_for_shares_and_inexact_in_band_steps_are_distributions(bars, events, hazards):
    con = _con()
    _bars(con, "X", dt.date(2021, 1, 1), dt.date(2022, 6, 30), **bars)
    assert _events(con, "X") == events
    assert _hazards(con) == hazards
    con.close()


def test_a_small_stock_split_is_pending_until_the_share_count_shows_it():
    con = _con()
    # 6:5 (a 20 % stock dividend) sits inside the dividend band; shares move 5 days later.
    _bars(con, "SD", dt.date(2021, 1, 1), dt.date(2021, 6, 30), split=(dt.date(2021, 3, 15), 1.2),
          shares=(1000, 1200, 5))
    assert _events(con, "SD") == [
        ("split", dt.date(2021, 3, 15), _at(dt.date(2021, 3, 22)), 1.2, "vendor_factor+shares")]
    # A filing inside the confirmation window is compared on an unknown basis.
    assert not _known(con, dt.datetime(2021, 2, 9, 21), dt.datetime(2021, 3, 19, 21))
    assert _known(con, dt.datetime(2021, 2, 9, 21), dt.datetime(2021, 5, 10, 21))
    con.close()


def test_a_flat_factor_with_a_split_signature_is_never_proof_of_no_split():
    con = _con()
    # A real 1:10 reverse split: raw close x10 and archive shares /10, vendor factor flat.
    _bars(con, "V", dt.date(2021, 1, 1), dt.date(2021, 6, 30), split=(dt.date(2021, 1, 15), 0.1),
          shares=(1000, 100, 0), flat_factor=True)
    assert _events(con, "V") == []
    assert _hazards(con) == [(dt.date(2021, 1, 15), _at(dt.date(2021, 1, 15)), None, 0.1,
                              "flat_factor_split_signature")]
    assert not _known(con, dt.datetime(2021, 1, 10, 21), dt.datetime(2021, 5, 10, 21))
    # No split: a 30 % share issuance and a 5 % down day inside one share window are no signature.
    _bars(con, "ISS", dt.date(2021, 1, 1), dt.date(2021, 6, 30), shares=(1000, 1000, 0),
          share_steps=((dt.date(2021, 3, 1), 1300),), price_moves=((dt.date(2021, 2, 15), 0.95),))
    assert _events(con, "ISS") == [] and _hazards(con) == []
    assert _known(con, dt.datetime(2021, 1, 10, 21), dt.datetime(2021, 5, 10, 21))
    con.close()


def test_coverage_needs_a_continuous_vendor_factor():
    con = _con()
    gap = set(_weekdays(dt.date(2021, 3, 1), dt.date(2021, 3, 19)))
    missing = set(_weekdays(dt.date(2021, 5, 3), dt.date(2021, 5, 28)))
    _bars(con, "GAP", dt.date(2021, 1, 1), dt.date(2021, 6, 30), drop=gap, factorless=missing)
    split_epochs.prepare_split_epochs(con, "GAP")
    runs = con.execute("SELECT first_at::DATE, last_at::DATE FROM _pit_split_coverage ORDER BY 1").fetchall()
    assert runs == [(dt.date(2021, 1, 1), dt.date(2021, 2, 26)), (dt.date(2021, 3, 22), dt.date(2021, 4, 30)),
                    (dt.date(2021, 5, 31), dt.date(2021, 6, 30))]
    con.close()


def test_without_a_bars_table_every_basis_is_unknown():
    con = duckdb.connect(":memory:")
    assert split_epochs.prepare_split_epochs(con, "ANY") == 0
    assert con.execute("SELECT count(*) FROM _pit_split_coverage").fetchone() == (0,)
    split_epochs.cleanup_split_epochs(con)
    con.close()


# --- engine: rebasing on the frame's split basis --------------------------------------------


def _definition(code, expression, inputs, window="q"):
    return DerivedMetricDefinition(code, "growth", expression, window, inputs, False, f"R1d probe {code}.", "1")


CATALOG = (
    _definition("eps_qoq_t", "qoq(eps_diluted)", ("item:eps_diluted",)),
    _definition("eps_yoy_t", "yoy(eps_diluted)", ("item:eps_diluted",)),
    _definition("eps_ttm_t", "ttm(eps_diluted)", ("item:eps_diluted",), "ttm"),
    _definition("eps_cagr_2y_t", "cagr(eps_diluted, 2)", ("item:eps_diluted",)),
    _definition("eps_change_t", "eps_diluted - lag(eps_diluted, 4)", ("item:eps_diluted",)),
    _definition("eps_sd4_t", "stdev_q(eps_change_t, 4)", ("metric:eps_change_t",)),
    _definition("shares_yoy_t", "yoy(shares_outstanding_period_end)", ("item:shares_outstanding_period_end",)),
    _definition("book_ps_t", "safe_div(common_equity, shares_outstanding_period_end)",
                ("item:common_equity", "item:shares_outstanding_period_end")),
)
QUARTERS = tuple(dt.date(year, month, day) for year in (2019, 2020, 2021)
                 for month, day in ((3, 31), (6, 30), (9, 30), (12, 31)))
# Economic series in pre-split units; a 1:10 reverse split (k = 0.1) goes ex on
# 2021-01-15, between the filings for Q6 (2020-11-09) and Q7 (2021-02-09).
EPS_PRE = (0.05, 0.06, 0.04, 0.08, 0.05, 0.07, 0.06, 0.09, 0.07, 0.08, 0.10, 0.12)
SHARES_PRE = tuple(1000.0 + 10.0 * index for index in range(12))
EQUITY = tuple(500.0 + 5.0 * index for index in range(12))
SPLIT, K = dt.date(2021, 1, 15), 0.1
POST = tuple(eps / K for eps in EPS_PRE)  # the same series on the post-split basis
NO_SPLIT = dt.date(2099, 1, 1)
REVERSE_SHARES = (1000, 100, 0)


def _clock(index):
    return dt.datetime.combine(QUARTERS[index] + dt.timedelta(days=40), dt.time(21))


@pytest.fixture
def store(monkeypatch):
    con = _con()
    con.execute("""
        CREATE TABLE fundamental_standardized (
            standardized_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
            cik VARCHAR, item_id INTEGER, canonical_code VARCHAR, basis VARCHAR,
            period_start DATE, period_end DATE, value DOUBLE, as_of_date DATE,
            available_at TIMESTAMP, input_codes_json VARCHAR, input_item_ids_json VARCHAR,
            rule_id VARCHAR, combination_rule VARCHAR, revision_sequence INTEGER,
            is_latest_revision BOOLEAN)
    """)
    con.execute("""
        CREATE TABLE derived_metric_values (
            derived_value_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
            metric_code VARCHAR, metric_window VARCHAR, period_end DATE, value DOUBLE,
            available_at TIMESTAMP, inputs_hash VARCHAR, as_of_date DATE,
            is_latest_revision BOOLEAN, run_id VARCHAR, value_status VARCHAR,
            revision_group_id VARCHAR, revision_sequence INTEGER, revision_count INTEGER,
            valid_to TIMESTAMP, target_bucket BIGINT, definition_hash VARCHAR,
            arithmetic_available_at TIMESTAMP, history_status VARCHAR,
            value_origin VARCHAR, fiscal_period_start DATE, fiscal_period_end DATE,
            selected_input_refs_json VARCHAR, selected_input_refs_hash VARCHAR)
    """)
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: CATALOG)
    try:
        yield SimpleNamespace(con=con, path=Path(":memory:"), initialize=lambda: None)
    finally:
        con.close()


def _seed(store, security_id, *, split_filed_after=SPLIT, k=K):
    """Each quarter's facts as filed: on the post-split basis once the split preceded the filing."""
    for index, end in enumerate(QUARTERS):
        start = QUARTERS[index - 1] + dt.timedelta(days=1) if index else end - dt.timedelta(days=90)
        clock = _clock(index)
        post = clock.date() > split_filed_after
        rows = (("eps_diluted", "quarterly", start, EPS_PRE[index] / k if post else EPS_PRE[index]),
                ("shares_outstanding_period_end", "instant", None,
                 SHARES_PRE[index] * k if post else SHARES_PRE[index]),
                ("common_equity", "instant", None, EQUITY[index]))
        for code, basis, begin, value in rows:
            store.con.execute(
                "INSERT INTO fundamental_standardized VALUES (?, 'test', ?, '1', 1, ?, ?, ?, ?, ?, ?, ?, "
                "'[]', '[]', 'r', 'direct', 1, true)",
                [f"{security_id}|{code}|{end}", security_id, code, basis, begin, end, value, clock.date(), clock])


def _state(store, security_id, code, index):
    return store.con.execute("""
        SELECT value, value_status, value_origin FROM derived_metric_values
        WHERE security_id = ? AND metric_code = ? AND period_end = ?
        ORDER BY available_at DESC, derived_value_id DESC LIMIT 1
    """, [security_id, code, QUARTERS[index]]).fetchone()


def _refresh(store):
    engine.refresh_derived_metrics(store)


def _growth(series, index, periods):
    return (series[index] - series[index - periods]) / abs(series[index - periods])


def test_a_reverse_split_between_filings_is_rebased_on_the_frames_basis(store):
    _seed(store, "S")
    _bars(store.con, "S", split=(SPLIT, K), shares=REVERSE_SHARES)
    _refresh(store)

    # Unrebased this would be (0.9 - 0.06) / 0.06 = +14.0.
    assert _state(store, "S", "eps_qoq_t", 7) == (pytest.approx((POST[7] - POST[6]) / POST[6]), "valid", "quarterly")
    assert _state(store, "S", "eps_yoy_t", 9) == (pytest.approx((POST[9] - POST[5]) / POST[5]), "valid", "quarterly")
    assert _state(store, "S", "eps_ttm_t", 9) == (pytest.approx(sum(POST[6:10])), "valid", "quarterly")
    assert _state(store, "S", "eps_cagr_2y_t", 10) == (
        pytest.approx((POST[10] / POST[2]) ** 0.5 - 1), "valid", "quarterly")
    assert _state(store, "S", "shares_yoy_t", 8) == (
        pytest.approx(SHARES_PRE[8] / SHARES_PRE[4] - 1), "valid", "instant")
    changes = [POST[index] - POST[index - 4] for index in range(6, 10)]
    assert _state(store, "S", "eps_sd4_t", 9) == (pytest.approx(statistics.stdev(changes)), "valid", "quarterly")
    # Same balance date and filing: no rebase, no cross-period claim.
    assert _state(store, "S", "book_ps_t", 8)[0] == pytest.approx(EQUITY[8] / (SHARES_PRE[8] * K))


def test_values_before_the_split_is_known_stay_on_their_own_basis(store):
    _seed(store, "S")
    _bars(store.con, "S", split=(SPLIT, K), shares=REVERSE_SHARES)
    _refresh(store)
    # Evaluated at the Q6 filing (2020-11-09): the split is not yet known.
    assert _state(store, "S", "eps_ttm_t", 6) == (pytest.approx(sum(EPS_PRE[3:7])), "valid", "quarterly")
    assert _state(store, "S", "eps_change_t", 6)[0] == pytest.approx(EPS_PRE[6] - EPS_PRE[2])


def test_without_bars_or_near_a_split_the_basis_is_unproven(store):
    _seed(store, "NB")  # no bars at all
    _seed(store, "AMB", split_filed_after=dt.date(2021, 2, 5))
    _bars(store.con, "AMB", split=(dt.date(2021, 2, 5), K), shares=REVERSE_SHARES)
    # STALE: no split, but the bars stop at 2020-12-31.
    _seed(store, "STALE", split_filed_after=NO_SPLIT)
    _bars(store.con, "STALE", end=dt.date(2020, 12, 31))
    _refresh(store)

    for index in (5, 7, 9):
        assert _state(store, "NB", "eps_qoq_t", index)[1:] == ("valid", "incomparable"), index
    assert _state(store, "NB", "shares_yoy_t", 8)[1:] == ("valid", "incomparable")
    # A split after the last bar would not have been seen.
    assert _state(store, "STALE", "eps_qoq_t", 5)[1:] == ("valid", "quarterly")
    assert _state(store, "STALE", "eps_qoq_t", 9)[1:] == ("valid", "incomparable")
    # The Q7 filing (2021-02-09) is 4 days after the ex-date: restatement is uncertain.
    for index in (7, 8):
        assert _state(store, "AMB", "eps_qoq_t", index)[1:] == ("valid", "incomparable"), index
    assert _state(store, "AMB", "eps_qoq_t", 9) == (
        pytest.approx((POST[9] - POST[8]) / POST[8]), "valid", "quarterly")


@pytest.mark.parametrize("cut", [0.2937, 0.4731], ids=["special_dividend", "spin_off"])
def test_a_distribution_is_never_rebased(store, cut):
    # No split; a 29.37 % special dividend, or a spin-off worth 47.31 % of the price (a factor step
    # of 1.90, near 2:1 but not an exact split ratio), goes ex 2021-01-15 with a flat share count.
    _seed(store, "D", split_filed_after=NO_SPLIT)
    _bars(store.con, "D", dividends=((dt.date(2021, 1, 15), cut),), price_moves=((dt.date(2021, 1, 15), 1 - cut),),
          shares=(1000, 1000, 0))
    _refresh(store)
    # At the Q7 filing (2021-02-09) the share window is still open: value right, basis unknown.
    assert _state(store, "D", "eps_qoq_t", 7) == (
        pytest.approx(_growth(EPS_PRE, 7, 1)), "valid", "incomparable")
    # Decided as a distribution when the window closes (mid-April): later comparisons are proven, unrebased.
    assert _state(store, "D", "eps_ttm_t", 9) == (pytest.approx(sum(EPS_PRE[6:10])), "valid", "quarterly")
    assert _state(store, "D", "shares_yoy_t", 8) == (
        pytest.approx(SHARES_PRE[8] / SHARES_PRE[4] - 1), "valid", "instant")
    assert _state(store, "D", "eps_yoy_t", 9) == (pytest.approx(_growth(EPS_PRE, 9, 4)), "valid", "quarterly")
