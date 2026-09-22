"""Tier1-S4 T1/T2: the point-in-time US-listed universe."""

from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

UNIVERSE_TABLE = "universe_us_listed_membership"

EXPECTED_COLUMNS = (
    "membership_id",
    "universe_id",
    "security_id",
    "symbol",
    "valid_from",
    "valid_to",
    "available_at",
    "security_type",
    "exchange_code",
    "has_cik",
    "cik",
    "market_cap_decile",
    "reason",
    "rules_json",
    "decision_count",
    "as_of_date",
    "is_latest_revision",
    "source",
    "run_id",
    "source_loaded_at",
)


def _columns(store, relation):
    rows = store.con.execute(
        """
        SELECT column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main' AND table_name = ?
        ORDER BY column_index
        """,
        [relation],
    ).fetchall()
    return tuple(str(row[0]) for row in rows)


def test_membership_table_has_the_pit_interval_shape(tmp_store):
    assert _columns(tmp_store, UNIVERSE_TABLE) == EXPECTED_COLUMNS


def test_membership_table_is_catalogued(tmp_store):
    row = tmp_store.con.execute(
        "SELECT layer, grain, natural_key_json FROM table_catalog WHERE table_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()
    assert row is not None
    assert row[0] == "serving"
    assert row[1] == "universe_id,security_id,valid_from"
    assert json.loads(row[2]) == ["universe_id", "security_id", "valid_from"]


def test_membership_fields_are_catalogued(tmp_store):
    count = tmp_store.con.execute(
        "SELECT count(*) FROM field_catalog WHERE table_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()[0]
    assert int(count) == len(EXPECTED_COLUMNS)


def test_membership_has_a_lake_partition_spec(tmp_store):
    row = tmp_store.con.execute(
        "SELECT partition_columns_json, watermark_column FROM lake_partition_specs WHERE object_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()
    assert row is not None
    assert json.loads(row[0]) == ["as_of_date"]
    assert row[1] == "available_at"


@pytest.mark.parametrize(
    "check_name,severity",
    [
        ("universe_us_listed_overlapping_intervals", "critical"),
        ("universe_us_listed_missing_decile", "warning"),
    ],
)
def test_membership_quality_checks_are_registered(tmp_store, check_name, severity):
    row = tmp_store.con.execute(
        "SELECT severity, threshold_value, comparator, enabled FROM quality_check_registry WHERE check_name = ?",
        [check_name],
    ).fetchone()
    assert row is not None
    assert row[0] == severity
    assert float(row[1]) == 0.0
    assert row[2] == "eq"
    assert bool(row[3]) is True


def test_membership_is_a_default_lake_export_object():
    from atx_db.lake import DEFAULT_EXPORT_OBJECTS

    assert UNIVERSE_TABLE in DEFAULT_EXPORT_OBJECTS


CLASSIFICATION_CASES = (
    ("Apple Inc. - Common Stock", "common"),
    ("Alphabet Inc. - Class C Capital Stock", "common"),
    ("Simon Property Group, Inc. Common Stock", "common"),
    ("Taiwan Semiconductor Manufacturing Company Ltd. American Depositary Shares", "ADR"),
    ("Banco Santander, S.A. ADR", "ADR"),
    ("Prologis, Inc. Common Stock (REIT)", "REIT"),
    ("Realty Income Corporation Real Estate Investment Trust", "REIT"),
    ("Enterprise Products Partners L.P.", "LP"),
    ("Energy Transfer LP Common Units", "unit"),
    ("Bank of America Corporation Depositary Shares Series GG", "preferred"),
    ("Wells Fargo & Company 7.5% Preferred Series L", "preferred"),
    ("Churchill Capital Corp VII Warrant", "warrant"),
    ("Ajax Capital Rights", "right"),
    ("iShares Core S&P 500 ETF", "fund"),
    ("iPath Series B S&P 500 VIX Short-Term Futures ETN", "ETN"),
    ("Morgan Stanley Emerging Markets Domestic Debt Fund, Inc.", "fund"),
    ("Goldman Sachs Group 6.125% Notes due 2060", "note"),
)


@pytest.mark.parametrize("security_name,expected", CLASSIFICATION_CASES)
def test_classify_security_type(security_name, expected):
    from atx_db.universe_us_listed import classify_security_type

    assert classify_security_type(security_name) == expected


def test_etf_flag_beats_the_name():
    from atx_db.universe_us_listed import classify_security_type

    assert classify_security_type("Vanguard Total Stock Market", etf=True) == "ETF"


def test_test_issue_flag_wins_outright():
    from atx_db.universe_us_listed import classify_security_type

    assert classify_security_type("Apple Inc. - Common Stock", test_issue=True) == "test"


def test_only_four_security_types_are_eligible():
    from atx_db.universe_us_listed import ELIGIBLE_SECURITY_TYPES

    assert ELIGIBLE_SECURITY_TYPES == ("ADR", "LP", "REIT", "common")


@pytest.mark.parametrize(
    "raw,expected",
    [
        ("NASDAQ", "XNAS"),
        ("N", "XNYS"),
        ("A", "XASE"),
        ("P", "ARCX"),
        ("Z", "BATS"),
        ("V", None),
        ("", None),
        (None, None),
    ],
)
def test_exchange_code_for(raw, expected):
    from atx_db.universe_us_listed import exchange_code_for

    assert exchange_code_for(raw) == expected


def _sessions(dates):
    return pd.DataFrame(
        {
            "trade_date": [dt.date.fromisoformat(d) for d in dates],
            "session_rank": range(1, len(dates) + 1),
        }
    )


def _decision(security_id, date, rank, **overrides):
    row = {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date.fromisoformat(date),
        "session_rank": rank,
        "available_at": pd.Timestamp(f"{date} 22:00:00"),
        "security_type": "common",
        "exchange_code": "XNAS",
        "has_cik": True,
        "cik": "0000320193",
        "market_cap_decile": 9,
    }
    row.update(overrides)
    return row


SESSION_DATES = [
    "2024-01-02",
    "2024-01-03",
    "2024-01-04",
    "2024-01-05",
    "2024-01-08",
    "2024-01-09",
    "2024-01-10",
    "2024-01-11",
    "2024-01-12",
    "2024-01-16",
]


def _options(**overrides):
    from atx_db.universe_us_listed import UniverseUsListedOptions

    return UniverseUsListedOptions(lookback_days=3, run_id="test-run", **overrides)


def test_contiguous_decisions_collapse_to_one_interval():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", "2024-01-02", 1),
            _decision("SEC-1", "2024-01-03", 2),
            _decision("SEC-1", "2024-01-04", 3),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert len(out) == 1
    row = out.iloc[0]
    assert row["valid_from"] == dt.date(2024, 1, 2)
    # last bar is session 3; lookback_days=3 extends membership to session 5.
    assert row["valid_to"] == dt.date(2024, 1, 8)
    assert int(row["decision_count"]) == 3
    assert row["reason"] == "member"


def test_a_state_change_opens_a_new_interval_with_no_gap():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", "2024-01-02", 1, exchange_code="XNYS"),
            _decision("SEC-1", "2024-01-03", 2, exchange_code="XNYS"),
            _decision("SEC-1", "2024-01-04", 3, exchange_code="XNAS"),
            _decision("SEC-1", "2024-01-05", 4, exchange_code="XNAS"),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert list(out["exchange_code"]) == ["XNYS", "XNAS"]
    assert out.iloc[0]["valid_to"] == dt.date(2024, 1, 3)
    assert out.iloc[1]["valid_from"] == dt.date(2024, 1, 4)


def test_a_gap_longer_than_the_lookback_splits_the_interval():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", "2024-01-02", 1),
            _decision("SEC-1", "2024-01-12", 9),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert len(out) == 2
    assert out.iloc[0]["valid_to"] == dt.date(2024, 1, 4)
    assert out.iloc[1]["valid_from"] == dt.date(2024, 1, 12)


def test_an_interval_reaching_the_archive_end_stays_open():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame([_decision("SEC-1", "2024-01-16", 10)])
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert pd.isna(out.iloc[0]["valid_to"])


def test_the_unresolved_cik_tail_is_retained_and_labelled():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame([_decision("SEC-2", "2024-01-02", 1, has_cik=False, cik=None)])
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert len(out) == 1
    assert bool(out.iloc[0]["has_cik"]) is False
    assert out.iloc[0]["reason"] == "member_no_cik"


def test_the_decile_is_taken_at_valid_from_only():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", "2024-01-02", 1, market_cap_decile=4),
            _decision("SEC-1", "2024-01-03", 2, market_cap_decile=7),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert len(out) == 1
    assert int(out.iloc[0]["market_cap_decile"]) == 4


def test_output_is_row_order_independent_and_stably_sorted():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    rows = [
        _decision("SEC-2", "2024-01-03", 2),
        _decision("SEC-1", "2024-01-02", 1),
        _decision("SEC-1", "2024-01-03", 2),
    ]
    sessions = _sessions(SESSION_DATES)
    first = compute_universe_us_listed_intervals(pd.DataFrame(rows), sessions, _options())
    second = compute_universe_us_listed_intervals(pd.DataFrame(list(reversed(rows))), sessions, _options())
    pd.testing.assert_frame_equal(first, second)
    assert list(first["security_id"]) == ["SEC-1", "SEC-2"]


def test_membership_id_is_a_stable_content_hash():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame([_decision("SEC-1", "2024-01-02", 1)])
    sessions = _sessions(SESSION_DATES)
    a = compute_universe_us_listed_intervals(decisions, sessions, _options())
    b = compute_universe_us_listed_intervals(decisions, sessions, _options())
    assert a.iloc[0]["membership_id"] == b.iloc[0]["membership_id"]
    assert len(a.iloc[0]["membership_id"]) == 64


def _seed_universe_warehouse(store):
    store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-AAPL', 'CIK-0000320193', 'AAPL', 'Apple Inc.', 'test'),"
        "('SEC-SPY', NULL, 'SPY', 'SPDR S&P 500 ETF Trust', 'test'),"
        "('SEC-TAIL', NULL, 'TAIL', 'Tail Holdings Inc.', 'test')"
    )
    bars = []
    for day in ("2024-01-02", "2024-01-03", "2024-01-04"):
        for security_id, symbol in (("SEC-AAPL", "AAPL"), ("SEC-SPY", "SPY"), ("SEC-TAIL", "TAIL")):
            bars.append(
                f"('test','{security_id}','{symbol}',DATE '{day}',10.0,1000,"
                f"TIMESTAMP '{day} 22:00:00',DATE '{day}',true)"
            )
    store.con.execute(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, volume, "
        "available_at, as_of_date, is_latest_revision) VALUES " + ",".join(bars)
    )
    store.con.execute(
        "INSERT INTO nasdaq_symbol_directory "
        "(directory, symbol, security_name, exchange, etf, test_issue, as_of_date, source_url) VALUES "
        "('nasdaqlisted','AAPL','Apple Inc. - Common Stock','NASDAQ',false,false,DATE '2024-01-01','file://t'),"
        "('nasdaqlisted','SPY','SPDR S&P 500 ETF Trust','NASDAQ',true,false,DATE '2024-01-01','file://t'),"
        "('otherlisted','TAIL','Tail Holdings Inc. Common Stock','N',false,false,DATE '2024-01-01','file://t')"
    )


def test_refresh_writes_members_and_retains_the_unresolved_tail(tmp_store):
    from atx_db.universe_us_listed import (
        UniverseUsListedOptions,
        refresh_universe_us_listed,
        universe_us_listed,
    )

    _seed_universe_warehouse(tmp_store)
    rows = refresh_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t"))
    assert rows == 2  # AAPL and TAIL; SPY is an ETF and is excluded

    members = universe_us_listed(tmp_store, dt.date(2024, 1, 3))
    assert sorted(members["security_id"]) == ["SEC-AAPL", "SEC-TAIL"]
    assert set(members["exchange_code"]) == {"XNAS", "XNYS"}

    fundamentals = universe_us_listed(tmp_store, dt.date(2024, 1, 3), require_cik=True)
    assert list(fundamentals["security_id"]) == ["SEC-AAPL"]
    assert list(fundamentals["cik"]) == ["0000320193"]


def test_refresh_reports_the_excluded_tail(tmp_store):
    from atx_db.universe_us_listed import (
        UniverseUsListedOptions,
        load_universe_decisions,
    )

    _seed_universe_warehouse(tmp_store)
    _eligible, _sessions, exclusions = load_universe_decisions(
        tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t")
    )
    assert exclusions["not_eligible_security_type"] == 3  # SPY on three sessions
    assert exclusions["not_eligible_exchange"] == 0


def test_refresh_is_idempotent(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, refresh_universe_us_listed

    _seed_universe_warehouse(tmp_store)
    options = UniverseUsListedOptions(lookback_days=2, run_id="t")
    first = refresh_universe_us_listed(tmp_store, options)
    second = refresh_universe_us_listed(tmp_store, options)
    assert first == second
    total = tmp_store.con.execute("SELECT count(*) FROM universe_us_listed_membership").fetchone()[0]
    assert int(total) == second


def test_membership_intervals_never_overlap(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, refresh_universe_us_listed

    _seed_universe_warehouse(tmp_store)
    refresh_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t"))
    overlaps = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM universe_us_listed_membership a
        JOIN universe_us_listed_membership b
          ON a.universe_id = b.universe_id
         AND a.security_id = b.security_id
         AND a.membership_id <> b.membership_id
         AND a.valid_from <= coalesce(b.valid_to, DATE '9999-12-31')
         AND b.valid_from <= coalesce(a.valid_to, DATE '9999-12-31')
        """
    ).fetchone()[0]
    assert int(overlaps) == 0


# --- Fix round 1: the migration-0304 quality_check_registry rows are backed by real
# SqlQualityChecks (atx_db.quality.checks_universe) and actually run through the shared
# runner, not just registered as inert metadata. ---

_OVERLAP_CHECK_NAME = "universe_us_listed_overlapping_intervals"
_MISSING_DECILE_CHECK_NAME = "universe_us_listed_missing_decile"


def _insert_membership_rows(store, rows):
    store.con.executemany(
        """
        INSERT INTO universe_us_listed_membership (
            membership_id, universe_id, security_id, symbol, valid_from, valid_to,
            available_at, security_type, exchange_code, has_cik, cik, market_cap_decile,
            reason, rules_json, decision_count, as_of_date, is_latest_revision, source
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        rows,
    )


def _membership_row(
    membership_id,
    security_id,
    valid_from,
    valid_to,
    *,
    universe_id="universe_us_listed",
    market_cap_decile=5,
):
    return (
        membership_id,
        universe_id,
        security_id,
        security_id,
        dt.date.fromisoformat(valid_from),
        None if valid_to is None else dt.date.fromisoformat(valid_to),
        pd.Timestamp(f"{valid_from} 22:00:00"),
        "common",
        "XNAS",
        True,
        "0000320193",
        market_cap_decile,
        "member",
        "{}",
        1,
        dt.date.fromisoformat(valid_from),
        True,
        "test",
    )


def _insert_market_daily_rows(store, rows):
    store.con.executemany(
        """
        INSERT INTO market_daily_metrics (
            market_daily_id, source, security_id, trade_date, market_cap,
            available_at, inputs_hash, as_of_date, is_latest_revision
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        rows,
    )


def _market_daily_row(security_id, trade_date, market_cap):
    return (
        f"{security_id}-{trade_date}",
        "test",
        security_id,
        dt.date.fromisoformat(trade_date),
        market_cap,
        pd.Timestamp(f"{trade_date} 22:00:00"),
        f"hash-{security_id}-{trade_date}",
        dt.date.fromisoformat(trade_date),
        True,
    )


def _run_universe_checks(store, check_name):
    from atx_db.quality import run_warehouse_quality_checks

    results = run_warehouse_quality_checks(store, record=False, check_names={check_name})
    assert len(results) == 1
    return results[0]


def test_universe_quality_checks_pass_on_clean_disjoint_intervals(tmp_store):
    _insert_membership_rows(
        tmp_store,
        [
            _membership_row("m-1", "SEC-1", "2024-01-02", "2024-01-04"),
            _membership_row("m-2", "SEC-1", "2024-01-10", None),
            _membership_row("m-3", "SEC-2", "2024-01-02", None),
        ],
    )
    _insert_market_daily_rows(
        tmp_store,
        [
            _market_daily_row("SEC-1", "2024-01-02", 1_000_000.0),
            _market_daily_row("SEC-2", "2024-01-02", 2_000_000.0),
        ],
    )

    overlap_result = _run_universe_checks(tmp_store, _OVERLAP_CHECK_NAME)
    assert overlap_result.status == "passed"
    assert overlap_result.observed_value == 0.0
    assert overlap_result.severity == "critical"

    decile_result = _run_universe_checks(tmp_store, _MISSING_DECILE_CHECK_NAME)
    assert decile_result.status == "passed"
    assert decile_result.observed_value == 0.0
    assert decile_result.severity == "warning"


def test_overlapping_intervals_check_fails_on_an_engineered_overlap(tmp_store):
    _insert_membership_rows(
        tmp_store,
        [
            # SEC-1 has two intervals that overlap Jan5-Jan10: a critical, gate-blocking bug.
            _membership_row("m-1", "SEC-1", "2024-01-02", "2024-01-10"),
            _membership_row("m-2", "SEC-1", "2024-01-05", None),
        ],
    )

    result = _run_universe_checks(tmp_store, _OVERLAP_CHECK_NAME)
    assert result.status == "failed"
    assert result.observed_value == 1.0
    assert result.severity == "critical"


def test_missing_decile_check_fails_when_a_priced_row_has_no_decile(tmp_store):
    _insert_membership_rows(
        tmp_store,
        [
            _membership_row("m-1", "SEC-1", "2024-01-02", None, market_cap_decile=None),
        ],
    )
    _insert_market_daily_rows(
        tmp_store,
        [
            _market_daily_row("SEC-1", "2024-01-02", 1_000_000.0),
        ],
    )

    result = _run_universe_checks(tmp_store, _MISSING_DECILE_CHECK_NAME)
    assert result.status == "warning"
    assert result.observed_value == 1.0
    assert result.severity == "warning"
