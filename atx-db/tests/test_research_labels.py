"""R3a monthly survivorship-safe labels: parity with the daily panel, long-horizon
terminal stitching, halt-gap terminal dating, post-terminal exclusion, classified
attrition diagnostics, validity SQL."""

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
INDEX = {day: position for position, day in enumerate(SESSIONS)}
# S survives; D/P delist the session after their last trade (observed/policy); X's last
# trade is a month-end decision date and it delists at the entry session; H halts on
# 2024-03-22 and its terminal is dated 2024-04-10 (12 absent sessions: re-anchored); B
# halts for 39 sessions (beyond the 31-session bound: not re-anchored); G ceases with a
# public event but no terminal.
LAST_TRADE = {"S": SESSIONS[-1], "D": dt.date(2023, 8, 15), "P": dt.date(2024, 2, 14),
              "G": dt.date(2024, 3, 28), "X": dt.date(2023, 10, 31), "H": dt.date(2024, 3, 22),
              "B": dt.date(2023, 5, 15)}
STRAY = ("D", dt.date(2023, 9, 1))  # a print after D's terminal, on an entry session
TERMINALS = {"D": (dt.date(2023, 8, 16), -0.3, "observed", "obs-D", dt.datetime(2023, 8, 20)),
             "P": (dt.date(2024, 2, 15), -0.3, "policy", None, dt.datetime(2024, 2, 16)),
             "X": (dt.date(2023, 11, 1), -0.4, "observed", "obs-X", dt.datetime(2023, 11, 3)),
             "H": (dt.date(2024, 4, 10), -0.5, "observed", "obs-H", dt.datetime(2024, 4, 12)),
             "B": (dt.date(2023, 7, 10), -0.6, "policy", None, dt.datetime(2023, 7, 11))}
G_CESSATION = dt.date(2024, 4, 1)
ECONOMIC = [c for c in FORWARD_RETURN_SS_COLUMNS if c not in {"forward_return_id", "source", "run_id"}]


def _effective(security):
    """Halt-gap rule, restated independently: 1..31 absent sessions -> first session after last trade."""
    delist = TERMINALS[security][0]
    last = LAST_TRADE[security]
    absent = sum(1 for day in SESSIONS if last < day < delist)
    return SESSIONS[INDEX[last] + 1] if 1 <= absent <= 31 else delist


def _price(security, index):
    return {"S": 100.0, "D": 20.0, "P": 5.0, "G": 50.0, "X": 8.0, "H": 30.0, "B": 12.0}[security] * math.exp(
        0.0007 * index + 0.03 * math.sin(index / 5 + len(security) + ord(security[0]) % 7))


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


def _matured(entry, horizon):
    return INDEX[entry] + horizon < len(SESSIONS)


def _end(entry, horizon):
    return SESSIONS[INDEX[entry] + horizon]


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
        ("adjusted_close", "forward_return_publication_v3", "r3a")]
    assert publication.CALCULATION_VERSION == "forward_return_publication_v3"


def test_long_horizons_stitch_terminals_and_exclude_post_terminal_formations(store):
    refresh_monthly_forward_labels(store)
    entries = _entries()
    for security, (_, terminal, kind, observation, available) in TERMINALS.items():
        last, effective = LAST_TRADE[security], _effective(security)
        for horizon in MONTHLY_HORIZON_SESSIONS:
            rows = store.con.execute("""
                SELECT as_of_date, forward_end_date, raw_forward_return, forward_return, terminal_return,
                       terminal_return_source, return_observation_id, delist_date, is_stitched, available_at
                FROM forward_returns_survivorship_safe
                WHERE source=? AND security_id=? AND horizon_days=? ORDER BY as_of_date
            """, [MONTHLY_LABEL_SOURCE, security, horizon]).fetchall()
            spanning = [e for e in entries if e <= last and _matured(e, horizon) and _end(e, horizon) >= effective]
            assert [row[0] for row in rows if row[8]] == spanning
            for row in (row for row in rows if row[8]):
                raw = _price(security, INDEX[last]) / _price(security, INDEX[row[0]]) - 1
                assert row[1] == _end(row[0], horizon)
                assert row[2] == pytest.approx(raw, rel=1e-12)
                assert row[3] == pytest.approx((1 + raw) * (1 + terminal) - 1, rel=1e-12)
                # The published delist_date is the effective (halt-gap) date.
                assert row[4:8] == (terminal, kind, observation, effective)
                assert row[9] == available  # the terminal clock is the latest input
            # No anchor on or after the terminal survives, including D's stray print.
            assert all(row[0] < effective for row in rows)
    assert store.con.execute("""
        SELECT count(*) FROM forward_returns_survivorship_safe
        WHERE source=? AND security_id=? AND as_of_date=?""", [MONTHLY_LABEL_SOURCE, *STRAY]).fetchone() == (0,)


def test_halt_gap_terminal_lands_in_the_one_month_label(store):
    """Reviewer probe 4: H last trades 2024-03-22, delists 2024-04-10 at -50%."""
    refresh_monthly_forward_labels(store)
    row = store.con.execute("""
        SELECT forward_end_date, delist_date, is_stitched, forward_return, available_at
        FROM forward_returns_survivorship_safe
        WHERE source=? AND security_id='H' AND as_of_date=DATE '2024-03-01' AND horizon_days=21
    """, [MONTHLY_LABEL_SOURCE]).fetchone()
    assert row is not None, "the -50% delisting must not vanish from the March 1-month label"
    raw = _price("H", INDEX[dt.date(2024, 3, 22)]) / _price("H", INDEX[dt.date(2024, 3, 1)]) - 1
    assert row[0] == dt.date(2024, 4, 1) and row[1] == dt.date(2024, 3, 25) and row[2]
    assert row[3] == pytest.approx((1 + raw) * 0.5 - 1, rel=1e-12)
    assert row[4] == dt.datetime(2024, 4, 12)  # not knowable before the terminal was
    # B's terminal is 39 absent sessions after its last trade: beyond the bound, a window
    # ending inside that gap is not stitched (and is classified, below, as attrition).
    assert store.con.execute("""
        SELECT count(*) FROM forward_returns_survivorship_safe
        WHERE source=? AND security_id='B' AND as_of_date=DATE '2023-05-01' AND horizon_days=21
    """, [MONTHLY_LABEL_SOURCE]).fetchone() == (0,)


def test_diagnostics_classify_attrition_and_count_the_decision_denominator(store):
    refresh_monthly_forward_labels(store)
    report = monthly_label_diagnostics(store)
    assert report["terminal_gaps"] == {"terminals": 5, "no_prior_bar": 0, "next_session": 3,
                                       "re_anchored": 1, "beyond_bound": 1, "max_gap_sessions": 39}
    by_horizon = {row["horizon_days"]: row for row in report["horizons"]}
    entries = _entries()
    bars = {(security, day) for security, last in LAST_TRADE.items() for day in SESSIONS if day <= last}
    bars.add(STRAY)
    for horizon in MONTHLY_HORIZON_SESSIONS:
        row = by_horizon[horizon]
        matured = [e for e in entries if _matured(e, horizon)]
        decisions = [(s, e) for e in matured for s in LAST_TRADE if (s, SESSIONS[INDEX[e] - 1]) in bars]
        no_entry = [(s, e) for s, e in decisions if (s, e) not in bars]
        assert (row["decision_names"], row["no_entry_bar"]) == (len(decisions), len(no_entry))
        assert no_entry == [("X", dt.date(2023, 11, 1))]
        assert row["no_entry_bar_ceased"] == row["no_entry_bar_ceased_with_terminal"] == 1

        def ceased(security, h=horizon, windows=tuple(matured)):
            return [e for e in windows if (security, e) in bars and e <= LAST_TRADE[security] < _end(e, h)]

        g_lost = ceased("G")
        b_lost = [e for e in ceased("B") if _end(e, horizon) < _effective("B")]
        assert row["unlabeled_ceased_event_without_terminal"] == len(g_lost) > 0
        assert row["unlabeled_terminal_beyond_halt_bound"] == len(b_lost)
        assert row["unlabeled_invalid_terminal"] == row["unlabeled_ceased_unexplained"] == 0
        assert row["unlabeled_endpoint_bar_missing"] == row["unlabeled_other"] == 0
        assert row["survivorship_attrition"] == row["unlabeled_matured"] == len(g_lost) + len(b_lost)
        # D's stray print after its terminal is a correct exclusion, not attrition.
        assert row["excluded_post_terminal"] == int(STRAY[1] in matured)
        for kind in ("observed", "policy"):
            spanning = sum(1 for s, t in TERMINALS.items() if t[2] == kind
                           for e in matured if (s, e) in bars and e <= LAST_TRADE[s]
                           and _end(e, horizon) >= _effective(s))
            assert row[f"{kind}_terminals"] == spanning
        assert row["other_terminals"] == row["off_contract_rows"] == 0
        assert row["formation_units"] == {21: 1, 63: 3, 126: 6, 252: 12}[horizon]
        assert row["rows"] == row["anchors_matured"] - row["unlabeled_matured"]
    assert by_horizon[21]["unlabeled_terminal_beyond_halt_bound"] > 0
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
    version = publication.CALCULATION_VERSION
    try:
        con.execute("""
            CREATE TABLE l AS SELECT * FROM (VALUES
              ('valid', 'id1', 'adjusted_close', 'forward_return_publication_v3', 0.1, 0.1, NULL, false, false,
               NULL, NULL, NULL, DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('valid', 'id2', 'adjusted_close', 'forward_return_publication_v3', -0.45, 0.1, -0.5, true, true,
               DATE '2024-01-20', 'observed', 'obs', DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('invalid', 'id3', 'adjusted_close', 'forward_return_publication_v3', -0.45, 0.1, -0.5, true, true,
               DATE '2024-01-20', 'policy', 'obs', DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('invalid', 'id4', 'adjusted_close', 'forward_return_publication_v3', -0.45, 0.1, -0.5, true, true,
               DATE '2024-01-02', 'observed', 'obs', DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('invalid', 'id5', 'adjusted_close', 'forward_return_publication_v3', 0.1, 0.1, NULL, false, false,
               NULL, NULL, NULL, DATE '2024-02-02', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('invalid', 'id6', 'adjusted_close', 'forward_return_publication_v3', 0.1, 0.1, NULL, false, false,
               NULL, NULL, NULL, DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-07-01 22:00'),
              ('unsupported_basis', 'id7', 'close', 'forward_return_publication_v3', 0.1, 0.1, NULL, false, false,
               NULL, NULL, NULL, DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('unsupported_basis', 'id8', 'adjusted_close', 'forward_return_publication_v2', 0.1, 0.1, NULL,
               false, false, NULL, NULL, NULL, DATE '2024-02-01', DATE '2024-01-02', TIMESTAMP '2024-02-01 22:00'),
              ('missing', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
               NULL, NULL, NULL, NULL, DATE '2024-01-02', NULL)
            ) t(expected, forward_return_id, price_basis, calculation_version, forward_return,
                raw_forward_return, terminal_return, is_delisted_in_horizon, is_stitched, delist_date,
                terminal_return_source, return_observation_id, forward_end_date, entry, available_at)
        """)
        fragment = label_status_sql(label="l", calculation_version=f"'{version}'",
                                    expected_end="DATE '2024-02-01'", entry="l.entry",
                                    cutoff="TIMESTAMP '2024-06-01 22:00:00'")
        rows = con.execute(f"SELECT expected, {fragment} FROM l").fetchall()
        # A pre-v3 label (VA1: vendor factor-decrease artifact not repaired) is never consumed.
        assert [row[1] for row in rows] == [row[0] for row in rows]
        assert label_status_sql().count("?") == 5
    finally:
        con.close()
