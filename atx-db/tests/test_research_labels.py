"""R3a monthly survivorship-safe labels: parity with the daily panel, long-horizon
terminal stitching, post-terminal exclusion, attrition diagnostics, validity SQL."""

from __future__ import annotations

import datetime as dt
import itertools
import math

import duckdb
import pandas as pd
import pytest

from atx_db import _forward_return_publication as publication
from atx_db.connection import DuckDBStore
from atx_db.delisting import (
    FORWARD_RETURN_SS_COLUMNS,
    SurvivorshipSafeForwardReturnOptions,
    refresh_survivorship_safe_forward_returns,
)
from atx_db.research.labels import (
    DAILY_LABEL_SOURCE,
    MONTHLY_HORIZON_SESSIONS,
    MONTHLY_LABEL_SOURCE,
    MonthlyForwardLabelOptions,
    label_status_sql,
    month_end_formations,
    monthly_label_diagnostics,
    refresh_monthly_forward_labels,
)

SESSIONS = [day.date() for day in pd.bdate_range("2023-01-02", "2024-12-31")]
LAST_TRADE = {"S": SESSIONS[-1], "D": dt.date(2023, 8, 15), "P": dt.date(2024, 2, 14),
              "G": dt.date(2024, 3, 28)}
STRAY = ("D", dt.date(2023, 9, 1))  # a print after D's terminal, on an entry session
TERMINALS = {"D": (dt.date(2023, 8, 16), -0.3, "observed", "obs-D", dt.datetime(2023, 8, 20)),
             "P": (dt.date(2024, 2, 15), -0.3, "policy", None, dt.datetime(2024, 2, 16))}
G_CESSATION = dt.date(2024, 4, 1)
ECONOMIC = [c for c in FORWARD_RETURN_SS_COLUMNS if c not in {"forward_return_id", "source", "run_id"}]


def _price(security, index):
    return {"S": 100.0, "D": 20.0, "P": 5.0, "G": 50.0}[security] * math.exp(
        0.0007 * index + 0.03 * math.sin(index / 5 + len(security)))


@pytest.fixture
def store(tmp_path):
    store = DuckDBStore(tmp_path / "labels.duckdb")
    store.connection = duckdb.connect(str(store.path), config={"threads": 1, "memory_limit": "256MB"})
    store._configure_session(store.con)
    store.analytical_memory_limit = "256MB"
    store.analytical_threads = 1
    store._initialized = True
    store.con.execute("""
        CREATE TABLE equity_daily_bars (
            source VARCHAR, security_id VARCHAR, symbol VARCHAR, trade_date DATE,
            close DOUBLE, adjusted_close DOUBLE, available_at TIMESTAMP,
            vendor_security_id VARCHAR, source_loaded_at TIMESTAMP);
        CREATE TABLE trading_calendar (calendar_id VARCHAR, trade_date DATE, is_open BOOLEAN, source VARCHAR);
        CREATE TABLE delisting_terminal_returns (
            terminal_return_id VARCHAR, security_id VARCHAR, delist_date DATE,
            terminal_return DOUBLE, terminal_return_source VARCHAR,
            return_observation_id VARCHAR, available_at TIMESTAMP, source_loaded_at TIMESTAMP);
        CREATE TABLE delisting_events (security_id VARCHAR, delist_date DATE, available_at TIMESTAMP);
        CREATE TABLE forward_returns_survivorship_safe (
            forward_return_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL, security_id VARCHAR NOT NULL, symbol VARCHAR,
            as_of_date DATE NOT NULL, horizon_days INTEGER NOT NULL, forward_end_date DATE,
            raw_forward_return DOUBLE, terminal_return DOUBLE, forward_return DOUBLE NOT NULL,
            is_delisted_in_horizon BOOLEAN NOT NULL, is_stitched BOOLEAN NOT NULL,
            delist_date DATE, terminal_return_source VARCHAR, return_observation_id VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true, available_at TIMESTAMP NOT NULL,
            run_id VARCHAR, price_basis VARCHAR, calculation_version VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now());
    """)
    store.con.executemany("INSERT INTO trading_calendar VALUES ('XNYS', ?, true, 'equity_daily_bars calendar')",
                          [(day,) for day in SESSIONS])
    bars = []
    for security, last in LAST_TRADE.items():
        for index, day in enumerate(SESSIONS):
            if day <= last or (security, day) == STRAY:
                price = _price(security, index)
                # Raw close deliberately differs: the label basis must be adjusted_close.
                bars.append((security, security, day, 2 * price, price,
                             dt.datetime.combine(day, dt.time(22))))
    store.con.executemany("INSERT INTO equity_daily_bars VALUES ('prices', ?, ?, ?, ?, ?, ?, 'v', '2025-01-01')", bars)
    for security, (delist, value, kind, observation, available) in TERMINALS.items():
        store.con.execute("INSERT INTO delisting_terminal_returns VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                          [f"t-{security}", security, delist, value, kind, observation, available, available])
        store.con.execute("INSERT INTO delisting_events VALUES (?, ?, ?)", [security, delist, available])
    store.con.execute("INSERT INTO delisting_events VALUES ('G', ?, '2024-04-05')", [G_CESSATION])
    try:
        yield store
    finally:
        store.connection.close()


def _entries():
    return [day for previous, day in itertools.pairwise(SESSIONS) if previous.month != day.month]


def _rows(store, source, horizons):
    frame = store.con.execute(f"""
        SELECT {', '.join(ECONOMIC)} FROM forward_returns_survivorship_safe
        WHERE source = ? AND horizon_days IN ({', '.join(str(h) for h in horizons)})
        ORDER BY security_id, as_of_date, horizon_days
    """, [source]).df()
    return frame


def test_monthly_labels_equal_daily_rows_on_month_end_entry_sessions(store):
    daily = refresh_survivorship_safe_forward_returns(store, SurvivorshipSafeForwardReturnOptions())
    monthly = refresh_monthly_forward_labels(store, MonthlyForwardLabelOptions(run_id="r3a"))
    assert daily > 0 and monthly > 0
    entries = _entries()
    assert [pair[1] for pair in month_end_formations(store)] == entries
    assert [pair[0] for pair in month_end_formations(store)] == [
        SESSIONS[SESSIONS.index(day) - 1] for day in entries]
    anchors = {row[0] for row in store.con.execute(
        "SELECT DISTINCT as_of_date FROM forward_returns_survivorship_safe WHERE source=?",
        [MONTHLY_LABEL_SOURCE]).fetchall()}
    # Only entry sessions: never a mid-month session, never the unclosed first month.
    assert anchors == set(entries) and len(entries) == 23  # Feb-2023 .. Dec-2024 entries
    assert store.con.execute("""
        SELECT DISTINCT horizon_days FROM forward_returns_survivorship_safe WHERE source=? ORDER BY 1
    """, [MONTHLY_LABEL_SOURCE]).fetchall() == [(h,) for h in MONTHLY_HORIZON_SESSIONS]
    # Parity: identical economics, clocks, basis and version to the daily panel's rows.
    got = _rows(store, MONTHLY_LABEL_SOURCE, (21, 63))
    expected = _rows(store, DAILY_LABEL_SOURCE, (21, 63))
    expected = expected[expected["as_of_date"].dt.date.isin(entries)].reset_index(drop=True)
    assert len(got) == len(expected) > 100
    pd.testing.assert_frame_equal(got, expected)
    assert store.con.execute("""
        SELECT DISTINCT price_basis, calculation_version, run_id FROM forward_returns_survivorship_safe
        WHERE source=?""", [MONTHLY_LABEL_SOURCE]).fetchall() == [
        ("adjusted_close", publication.CALCULATION_VERSION, "r3a")]


def test_long_horizons_stitch_terminals_and_exclude_post_terminal_formations(store):
    refresh_monthly_forward_labels(store)
    index = {day: position for position, day in enumerate(SESSIONS)}
    entries = _entries()
    for security, (delist, terminal, kind, observation, available) in TERMINALS.items():
        last = LAST_TRADE[security]
        for horizon in (126, 252):
            rows = store.con.execute("""
                SELECT as_of_date, forward_end_date, raw_forward_return, forward_return, terminal_return,
                       terminal_return_source, return_observation_id, delist_date, is_stitched, available_at
                FROM forward_returns_survivorship_safe
                WHERE source=? AND security_id=? AND horizon_days=? ORDER BY as_of_date
            """, [MONTHLY_LABEL_SOURCE, security, horizon]).fetchall()
            spanning = [e for e in entries if e <= last and index[e] + horizon < len(SESSIONS)
                        and SESSIONS[index[e] + horizon] >= delist]
            stitched = [row for row in rows if row[8]]
            assert [row[0] for row in stitched] == spanning and spanning
            for row in stitched:
                raw = _price(security, index[last]) / _price(security, index[row[0]]) - 1
                assert row[1] == SESSIONS[index[row[0]] + horizon]
                assert row[2] == pytest.approx(raw, rel=1e-12)
                assert row[3] == pytest.approx((1 + raw) * (1 + terminal) - 1, rel=1e-12)
                assert row[4:8] == (terminal, kind, observation, delist)
                assert row[9] == available  # the terminal clock is the latest input
            # No anchor on or after the terminal survives, including D's stray print.
            assert all(row[0] < delist for row in rows)
    assert store.con.execute("""
        SELECT count(*) FROM forward_returns_survivorship_safe
        WHERE source=? AND security_id=? AND as_of_date=?""", [MONTHLY_LABEL_SOURCE, *STRAY]).fetchone() == (0,)


def test_diagnostics_count_terminal_provenance_and_unstitched_cessation_attrition(store):
    refresh_monthly_forward_labels(store)
    report = monthly_label_diagnostics(store)
    by_horizon = {row["horizon_days"]: row for row in report["horizons"]}
    index = {day: position for position, day in enumerate(SESSIONS)}
    entries = _entries()
    for horizon in MONTHLY_HORIZON_SESSIONS:
        row = by_horizon[horizon]

        def matured(e, h=horizon):
            return index[e] + h < len(SESSIONS)

        g_lost = [e for e in entries if e <= LAST_TRADE["G"] and matured(e)
                  and SESSIONS[index[e] + horizon] >= G_CESSATION]
        stray_matured = matured(STRAY[1])
        assert row["unlabeled_matured_spanning_event"] == len(g_lost)
        assert row["unlabeled_matured"] == len(g_lost) + int(stray_matured)
        for security, (delist, _, kind, _, _) in TERMINALS.items():
            spanning = [e for e in entries if e <= LAST_TRADE[security] and matured(e)
                        and SESSIONS[index[e] + horizon] >= delist]
            assert row[f"{kind}_terminals"] == len(spanning)
        assert row["other_terminals"] == row["off_contract_rows"] == 0
        assert row["formation_units"] == {21: 1, 63: 3, 126: 6, 252: 12}[horizon]
        assert row["rows"] == row["anchors_matured"] - row["unlabeled_matured"]
    assert by_horizon[252]["label_coverage"] < by_horizon[21]["label_coverage"] < 1.0


def test_guards_never_replace_the_daily_panel_or_accept_unknown_rules(store):
    with pytest.raises(ValueError, match="own source"):
        refresh_monthly_forward_labels(store, MonthlyForwardLabelOptions(source=DAILY_LABEL_SOURCE))
    with pytest.raises(ValueError, match="formation must be one of"):
        publication.refresh_forward_return_publication(
            store, source="x", run_id=None, price_basis="adjusted_close", cutoff=None,
            columns=FORWARD_RETURN_SS_COLUMNS, horizons=(21,), calendar_id="XNYS",
            calendar_source="equity_daily_bars calendar", formation="week_end")
    with pytest.raises(ValueError, match="distinct positive"):
        publication.refresh_forward_return_publication(
            store, source="x", run_id=None, price_basis="adjusted_close", cutoff=None,
            columns=FORWARD_RETURN_SS_COLUMNS, horizons=(21, 21), calendar_id="XNYS",
            calendar_source="equity_daily_bars calendar")


def test_label_status_sql_is_reusable_with_column_expressions():
    con = duckdb.connect()
    try:
        con.execute("""
            CREATE TABLE l AS SELECT * FROM (VALUES
              ('valid', 'id1', 'adjusted_close', 'forward_return_publication_v1', 0.1, 0.1, NULL, false, false,
               NULL, NULL, NULL, DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('valid', 'id2', 'adjusted_close', 'forward_return_publication_v1', -0.45, 0.1, -0.5, true, true,
               DATE '2024-01-20', 'observed', 'obs', DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('invalid', 'id3', 'adjusted_close', 'forward_return_publication_v1', -0.45, 0.1, -0.5, true, true,
               DATE '2024-01-20', 'policy', 'obs', DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('invalid', 'id4', 'adjusted_close', 'forward_return_publication_v1', -0.45, 0.1, -0.5, true, true,
               DATE '2024-01-02', 'observed', 'obs', DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('invalid', 'id5', 'adjusted_close', 'forward_return_publication_v1', 0.1, 0.1, NULL, false, false,
               NULL, NULL, NULL, DATE '2024-02-02', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('invalid', 'id6', 'adjusted_close', 'forward_return_publication_v1', 0.1, 0.1, NULL, false, false,
               NULL, NULL, NULL, DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-07-01 22:00'),
              ('unsupported_basis', 'id7', 'close', 'forward_return_publication_v1', 0.1, 0.1, NULL, false, false,
               NULL, NULL, NULL, DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('missing', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
               NULL, NULL, NULL, NULL, DATE '2024-01-02', NULL)
            ) t(expected, forward_return_id, price_basis, calculation_version, forward_return,
                raw_forward_return, terminal_return, is_delisted_in_horizon, is_stitched, delist_date,
                terminal_return_source, return_observation_id, forward_end_date, entry, available_at)
        """)
        fragment = label_status_sql(label="l", calculation_version="'forward_return_publication_v1'",
                                    expected_end="DATE '2024-02-01'", entry="l.entry",
                                    cutoff="TIMESTAMP '2024-06-01 22:00:00'")
        rows = con.execute(f"SELECT expected, {fragment} FROM l").fetchall()
        assert [row[1] for row in rows] == [row[0] for row in rows]
        assert label_status_sql().count("?") == 5
    finally:
        con.close()
