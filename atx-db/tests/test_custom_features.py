from __future__ import annotations

import datetime as dt
import json
import math
from collections.abc import Iterator
from contextlib import contextmanager

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.custom_features import (
    FEATURE_DEFINITIONS,
    FEATURE_SOURCE,
    FEATURE_VERSION,
    LABEL_SOURCE,
    PRICE_SOURCE,
    CustomEvaluationOptions,
    CustomFeatureOptions,
    calendar_hac_statistics,
    evaluate_custom_features,
    holm_eight,
    refresh_custom_features,
)
from atx_db.migrations.bodies_0313 import create_custom_feature_tables

SHA = "a"*64
RUN_AT = dt.datetime(2026, 9, 20, 22)


@pytest.fixture
def store() -> Iterator[DuckDBStore]:
    result = DuckDBStore(":memory:")
    result.connection = duckdb.connect(config={"threads": 1, "memory_limit": "128MB"})
    result.con.execute("SET preserve_insertion_order=false")
    create_custom_feature_tables(result.con)
    result.con.execute("""
        CREATE TABLE equity_daily_bars (
            source VARCHAR,security_id VARCHAR,trade_date DATE,
            open DOUBLE,high DOUBLE,low DOUBLE,close DOUBLE,adjusted_close DOUBLE,
            volume BIGINT,available_at TIMESTAMP
        );
        CREATE TABLE forward_returns_survivorship_safe (
            forward_return_id VARCHAR,source VARCHAR,security_id VARCHAR,
            as_of_date DATE,horizon_days INTEGER,forward_end_date DATE,
            forward_return DOUBLE,is_delisted_in_horizon BOOLEAN,
            terminal_return_source VARCHAR,available_at TIMESTAMP,source_loaded_at TIMESTAMP
        );
    """)
    try:
        yield result
    finally:
        result.con.close()


def _sessions(start: dt.date, count: int) -> list[dt.date]:
    result = []
    while len(result) < count:
        if start.weekday() < 5:
            result.append(start)
        start += dt.timedelta(days=1)
    return result


def _bars(store: DuckDBStore, dates: list[dt.date], names: int = 2) -> None:
    store.con.executemany("INSERT INTO equity_daily_bars VALUES (?,?,?,?,?,?,?,?,?,?)", [
        [PRICE_SOURCE, f"S{security:03}", date,
         10+session*.01, 11+session*.01, 9+session*.01,
         10+session*.01, 20+session*.02+.03*math.sin(session), 200_000,
         dt.datetime.combine(date, dt.time(22))]
        for security in range(names) for session, date in enumerate(dates)
    ])


def _build(store: DuckDBStore, dates: list[dt.date], run: str, source: str = FEATURE_SOURCE) -> dict:
    return refresh_custom_features(store, CustomFeatureOptions(
        dates[-1], RUN_AT, run, SHA, feature_source=source, partitions=2,
    ))


def test_causal_calendar_windows_missing_sessions_and_source_clocks(store) -> None:
    dates = _sessions(dt.date(2020, 1, 1), 140)
    _bars(store, dates)
    # The global calendar retains the date through S001. Per-security shift
    # would silently choose the wrong endpoint for S000's five-session reversal.
    store.con.execute("DELETE FROM equity_daily_bars WHERE security_id='S000' AND trade_date=?", [dates[129]])
    store.con.execute("UPDATE equity_daily_bars SET available_at=TIMESTAMP '2030-01-01' "
                      "WHERE security_id='S001' AND trade_date=?", [dates[132]])
    _build(store, dates, "causal")
    row = store.con.execute("""
        SELECT input_end_date,entry_date,decision_at,input_available_at,
               reversal_5,known_by_decision FROM custom_features_daily
        WHERE security_id='S000' AND decision_date=?
    """, [dates[135]]).fetchone()
    assert row[:2] == (dates[134], dates[136])
    assert row[2] == dt.datetime.combine(dates[135], dt.time(22))
    assert row[3] == dt.datetime.combine(dates[134]+dt.timedelta(days=1), dt.time(12))
    assert row[4] is None
    assert row[5] is True
    assert store.con.execute("SELECT known_by_decision FROM custom_features_daily "
                             "WHERE security_id='S001' AND decision_date=?", [dates[135]]).fetchone() == (False,)
    # Full observed windows on the other security preserve exact fixed endpoints.
    value = store.con.execute("SELECT momentum_126_skip_21 FROM custom_features_daily "
                             "WHERE security_id='S001' AND decision_date=?", [dates[131]]).fetchone()[0]
    expected = (20+109*.02+.03*math.sin(109))/(20+4*.02+.03*math.sin(4))-1
    assert value == pytest.approx(expected)
    assert store.con.execute("SELECT count(*) FROM custom_feature_definitions").fetchone() == (8,)


def test_scoped_replacement_is_idempotent_and_rolls_back(store, monkeypatch) -> None:
    dates = _sessions(dt.date(2020, 1, 1), 70)
    _bars(store, dates)
    first = _build(store, dates, "first")
    _build(store, dates, "other", "other-feature-source")
    _build(store, dates, "second")
    assert store.con.execute("SELECT count(*) FROM custom_features_daily WHERE feature_source=?", [FEATURE_SOURCE]).fetchone()[0] == first["rows"]
    previous = store.con.execute("SELECT * FROM custom_features_daily ORDER BY feature_source,security_id,decision_date").fetchall()
    original = store.transaction

    @contextmanager
    def failed_publication():
        with original():
            yield
            raise RuntimeError("publication failure")

    monkeypatch.setattr(store, "transaction", failed_publication)
    with pytest.raises(RuntimeError, match="publication failure"):
        _build(store, dates, "failed")
    assert store.con.execute("SELECT * FROM custom_features_daily ORDER BY feature_source,security_id,decision_date").fetchall() == previous
    assert store.con.execute("SELECT count(*) FROM custom_feature_runs WHERE run_id='failed'").fetchone() == (0,)


def _research_fixture(store: DuckDBStore, dates: list[dt.date], decisions: list[int]) -> None:
    """Direct wide rows isolate evaluation from the already covered feature formulas."""
    _bars(store, dates, names=1)
    con = store.con
    calendar_hash = con.execute("""
        SELECT sha256(string_agg(CAST(trade_date AS VARCHAR),',' ORDER BY trade_date))
        FROM equity_daily_bars
    """).fetchone()[0]
    values = []
    for decision in decisions:
        for security in range(200):
            value = security/200
            values.append([
                FEATURE_SOURCE, FEATURE_VERSION, f"S{security:03}", dates[decision],
                dt.datetime.combine(dates[decision], dt.time(22)), dates[decision-1],
                dt.datetime.combine(dates[decision-1]+dt.timedelta(days=1), dt.time(12)),
                dates[decision+1], decision+1, True, True, 10.0, 2_000_000., 63, 127, 63,
                *[value]*8, "fixture-build",
            ])
    con.executemany("INSERT INTO custom_features_daily VALUES ("+",".join(["?"]*25)+")", values)
    con.execute("INSERT INTO custom_feature_runs VALUES (?,?,?,?,?,?,?,?,?,?)", [
        "fixture-build", "build", FEATURE_SOURCE, FEATURE_VERSION, PRICE_SOURCE, SHA,
        dates[-1], RUN_AT, json.dumps({"calendar_sha256": calendar_hash}), json.dumps({"rows": len(values)}),
    ])
    labels = []
    for decision in decisions:
        for horizon in (5, 21, 63):
            if decision+1+horizon >= len(dates):
                continue
            ending = dates[decision+1+horizon]
            for security in range(200):
                # Label deliberately belongs to ENTRY, not the decision session.
                labels.append([
                    f"{decision}-{horizon}-{security}", LABEL_SOURCE, f"S{security:03}",
                    dates[decision+1], horizon, ending, security/1000,
                    security == 0, "policy" if security == 0 else None,
                    dt.datetime.combine(ending, dt.time(22)), RUN_AT,
                ])
    con.executemany("INSERT INTO forward_returns_survivorship_safe VALUES (?,?,?,?,?,?,?,?,?,?,?)", labels)


def test_deciles_rank_before_missing_labels_and_revisions_preserve_invalidity(store) -> None:
    dates = _sessions(dt.date(2024, 1, 1), 170)
    _research_fixture(store, dates, [65, 66])
    # Lose the highest 10 labels AFTER formation. They remain in Q10 and its denominator.
    store.con.execute("DELETE FROM forward_returns_survivorship_safe WHERE security_id>='S190'")
    store.con.execute("""
        INSERT INTO forward_returns_survivorship_safe
        SELECT forward_return_id||'-revision',source,security_id,as_of_date,horizon_days,
               forward_end_date,CAST('NaN' AS DOUBLE),is_delisted_in_horizon,terminal_return_source,
               available_at+INTERVAL 1 HOUR,source_loaded_at
        FROM forward_returns_survivorship_safe WHERE security_id='S189'
    """)
    result = evaluate_custom_features(store, CustomEvaluationOptions(dates[-1], RUN_AT, "eval", "fixture-build"))
    top = store.con.execute("""
        SELECT eligible_count,labeled_count,missing_count,invalid_label_count,mean_forward_return
        FROM custom_feature_deciles WHERE feature_id='reversal_5' AND horizon_sessions=21
          AND decision_date=? AND decile=10
    """, [dates[65]]).fetchone()
    assert top[:4] == (20, 9, 11, 1)
    assert top[4] == pytest.approx(.184)
    bottom = store.con.execute("""
        SELECT eligible_count,terminal_count,imputed_count FROM custom_feature_deciles
        WHERE feature_id='reversal_5' AND horizon_sessions=21 AND decision_date=? AND decile=1
    """, [dates[65]]).fetchone()
    assert bottom == (20, 1, 1)
    assert result["evaluation_rows"] == 72
    assert result["production_eligible"] == []
    assert store.con.execute("SELECT count(*) FROM custom_feature_evaluations WHERE production_eligible").fetchone() == (0,)


def test_split_purge_embargo_and_constant_dates_remain_visible(store) -> None:
    dates = _sessions(dt.date(2020, 10, 1), 200)
    boundary = next(index for index, day in enumerate(dates) if day.year == 2021)
    decisions = [boundary-10, boundary+5, boundary+65, boundary+66]
    _research_fixture(store, dates, decisions)
    store.con.execute("UPDATE custom_features_daily SET reversal_5=1 WHERE decision_date=?", [dates[boundary+66]])
    evaluate_custom_features(store, CustomEvaluationOptions(dates[-1], RUN_AT, "split", "fixture-build"))
    rows = store.con.execute("""
        SELECT decision_date,status FROM custom_feature_deciles
        WHERE feature_id='reversal_5' AND horizon_sessions=21
        GROUP BY decision_date,status ORDER BY decision_date
    """).fetchall()
    assert rows == [
        (dates[boundary-10], "purged_split_crossing"),
        (dates[boundary+5], "embargo"),
        (dates[boundary+65], "evaluated"),
        (dates[boundary+66], "constant_features"),
    ]
    # A primary feature with no train results still exists and has NULL significance.
    assert store.con.execute("SELECT p_value,holm_p_value FROM custom_feature_evaluations "
                             "WHERE feature_id='reversal_5' AND horizon_sessions=21 AND split='train'").fetchone() == (None, None)


def test_calendar_hac_known_spread_gaps_and_full_family_holm() -> None:
    values = [(index, .02+.01*math.sin(index/3)) for index in range(350) if index % 7 != 0]
    statistics = calendar_hac_statistics(values, 21)
    mean = sum(value for _, value in values)/len(values)
    centered = dict((session, value-mean) for session, value in values)
    variance = sum(value*value for value in centered.values())
    for lag in range(1, 21):
        variance += 2*(1-lag/21)*sum(value*centered.get(session-lag, 0) for session, value in centered.items())
    assert statistics["hac_standard_error"] == pytest.approx(math.sqrt(variance)/len(values))
    assert statistics["gross_mean"] == pytest.approx(mean)
    assert statistics["p_value"] < .001
    assert statistics["hac_lags"] == 20
    compressed = calendar_hac_statistics(list(enumerate(value for _, value in values)), 21)
    assert statistics["hac_standard_error"] != pytest.approx(compressed["hac_standard_error"])
    assert calendar_hac_statistics([(i, .02) for i in range(300)], 21)["p_value"] is None
    assert calendar_hac_statistics([(1, .1), (2, .2)], 21)["p_value"] is None
    feature = next(iter(FEATURE_DEFINITIONS))
    corrected = holm_eight({feature: .007})
    assert corrected[feature] == pytest.approx(.056)
    assert len(corrected) == 8
    assert sum(value is None for value in corrected.values()) == 7


def test_missing_label_horizons_fail_explicitly(store) -> None:
    dates = _sessions(dt.date(2024, 1, 1), 170)
    _research_fixture(store, dates, [65])
    store.con.execute("DELETE FROM forward_returns_survivorship_safe WHERE horizon_days=63")
    with pytest.raises(ValueError, match=r"missing required horizons \[63\]"):
        evaluate_custom_features(store, CustomEvaluationOptions(dates[-1], RUN_AT, "missing", "fixture-build"))
    assert store.con.execute("SELECT count(*) FROM custom_feature_runs WHERE run_id='missing'").fetchone() == (0,)
