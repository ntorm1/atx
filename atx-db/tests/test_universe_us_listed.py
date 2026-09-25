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
    # A2: every exclusion precedes the eligible types, and `common` needs positive
    # common/ordinary-share evidence in the listing name.
    ("XYZ Fund LP", "fund"),
    ("Acme Corp Common Stock", "common"),
    ("Acme Corp", "common_unverified"),
    ("BlackRock Municipal Income Trust", "common_unverified"),
    ("Adams Natural Resources Fund, Inc. Common Stock", "fund"),
    ("Shopify Inc. Class A Subordinate Voting Shares", "common"),
    ("Alibaba Group Holding Limited American Depositary Shares each representing eight Ordinary share", "ADR"),
    ("Spotify Technology S.A. Ordinary Shares", "common"),
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


def test_common_unverified_is_strict_ineligible_but_kept_by_the_reconstruction():
    from atx_db.universe_us_listed import (
        ELIGIBLE_SECURITY_TYPES,
        RECONSTRUCTED_ELIGIBLE_SECURITY_TYPES,
    )

    assert "common_unverified" not in ELIGIBLE_SECURITY_TYPES
    assert set(RECONSTRUCTED_ELIGIBLE_SECURITY_TYPES) == {*ELIGIBLE_SECURITY_TYPES, "common_unverified"}


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
        "('SEC-TAIL', 'CIK-0000777777', 'TAIL', 'Tail Holdings Inc.', 'test')"
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
        "(directory, symbol, security_name, exchange, etf, test_issue, as_of_date, source_url, available_at) VALUES "
        "('nasdaqlisted','AAPL','Apple Inc. - Common Stock','NASDAQ',false,false,DATE '2024-01-01','file://t',"
        "TIMESTAMP '2024-01-01 12:00:00'),"
        "('nasdaqlisted','SPY','SPDR S&P 500 ETF Trust','NASDAQ',true,false,DATE '2024-01-01','file://t',"
        "TIMESTAMP '2024-01-01 12:00:00'),"
        "('otherlisted','TAIL','Tail Holdings Inc. Common Stock','N',false,false,DATE '2024-01-01','file://t',"
        "TIMESTAMP '2024-01-01 12:00:00')"
    )
    # Only AAPL has dated CIK evidence; TAIL's securities.entity_id is a *current*
    # mapping and must not be used by the strict universe.
    store.con.execute(
        "INSERT INTO security_identifier_history "
        "(security_id, id_type, id_value, valid_from, valid_to, as_of_date, available_at, source) VALUES "
        "('SEC-AAPL','CIK','0000320193',DATE '2020-01-01',NULL,DATE '2020-01-01',"
        "TIMESTAMP '2020-01-01 12:00:00','t')"
    )


def test_refresh_writes_members_and_retains_the_unresolved_tail(tmp_store):
    from atx_db.universe_us_listed import (
        UniverseUsListedOptions,
        build_universe_us_listed,
        universe_us_listed,
    )

    _seed_universe_warehouse(tmp_store)
    summary = build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t"))
    # AAPL and TAIL in each variant; SPY is an ETF and is excluded from both.
    assert summary.rows_by_universe == {"us_listed_v1": 2, "us_listed_reconstructed_v1": 2}

    members = universe_us_listed(tmp_store, dt.date(2024, 1, 3))
    assert sorted(members["security_id"]) == ["SEC-AAPL", "SEC-TAIL"]
    assert set(members["exchange_code"]) == {"XNAS", "XNYS"}

    fundamentals = universe_us_listed(tmp_store, dt.date(2024, 1, 3), require_cik=True)
    assert list(fundamentals["security_id"]) == ["SEC-AAPL"]
    assert list(fundamentals["cik"]) == ["0000320193"]


def test_refresh_reports_the_excluded_tail(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    _seed_universe_warehouse(tmp_store)
    summary = build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t"))
    strict = summary.dispositions["strict"]
    assert strict["not_eligible_security_type"] == 3  # SPY on three sessions
    assert strict["not_eligible_security_type:ETF"] == 3
    assert strict.get("not_eligible_exchange", 0) == 0
    assert strict["eligible"] == 6
    details = json.loads(
        tmp_store.con.execute(
            "SELECT details_json FROM data_quality_checks WHERE dataset_id = 'universe_us_listed' "
            "AND check_name = 'rows_loaded' ORDER BY checked_at DESC LIMIT 1"
        ).fetchone()[0]
    )
    assert details["decision_dispositions"]["not_eligible_security_type:ETF"] == 3


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


# --- A2 (pre-run5): PIT clocks, dated identity, labeled reconstruction, bounded build. ---

_TS = dt.datetime.fromisoformat
_D = dt.date.fromisoformat


def _add_bars(store, rows):
    """rows: (security_id, symbol, trade_date) -- clock modeled at trade_date 22:00."""

    store.con.executemany(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, volume, "
        "available_at, as_of_date, is_latest_revision) VALUES ('test', ?, ?, ?, 10.0, 1000, ?, ?, true)",
        [(security_id, symbol, _D(day), _TS(f"{day} 22:00:00"), _D(day)) for security_id, symbol, day in rows],
    )


def _add_directory(store, rows):
    """rows: (symbol, security_name, exchange, as_of_date, available_at[, etf])."""

    store.con.executemany(
        "INSERT INTO nasdaq_symbol_directory (directory, symbol, security_name, exchange, etf, "
        "test_issue, as_of_date, source_url, available_at) VALUES (?, ?, ?, ?, ?, false, ?, 'file://t', ?)",
        [
            (
                "nasdaqlisted" if row[2] == "NASDAQ" else "otherlisted",
                row[0],
                row[1],
                row[2],
                bool(row[5]) if len(row) > 5 else False,
                _D(row[3]),
                _TS(row[4]),
            )
            for row in rows
        ],
    )


def _add_cik(store, rows):
    """rows: (security_id, cik, valid_from, valid_to|None, as_of_date, available_at)."""

    store.con.executemany(
        "INSERT INTO security_identifier_history (security_id, id_type, id_value, valid_from, valid_to, "
        "as_of_date, available_at, source) VALUES (?, 'CIK', ?, ?, ?, ?, ?, 'test')",
        [
            (sid, cik, _D(vf), None if vt is None else _D(vt), _D(asof), _TS(avail))
            for sid, cik, vf, vt, asof, avail in rows
        ],
    )


def _add_market_caps(store, rows):
    """rows: (security_id, trade_date, market_cap, available_at, is_latest_revision, revision_tag)."""

    from atx_db.market_daily import MARKET_DAILY_SOURCE_NAME

    store.con.executemany(
        "INSERT INTO market_daily_metrics (market_daily_id, source, security_id, trade_date, market_cap, "
        "available_at, inputs_hash, as_of_date, is_latest_revision) VALUES (?, ?, ?, ?, ?, ?, 'h', ?, ?)",
        [
            (f"{sid}|{day}|{tag}", MARKET_DAILY_SOURCE_NAME, sid, _D(day), cap, _TS(avail), _D(day), latest)
            for sid, day, cap, avail, latest, tag in rows
        ],
    )


def _membership(store, universe_id="us_listed_v1"):
    return store.con.execute(
        """
        SELECT security_id, symbol, valid_from, valid_to, available_at, security_type, exchange_code,
               has_cik, cik, market_cap_decile, reason, rules_json, decision_count
        FROM universe_us_listed_membership WHERE universe_id = ?
        ORDER BY security_id, valid_from
        """,
        [universe_id],
    ).fetchall()


def _l2_sql_d(store):
    """L2 report §7 acceptance SQL D, verbatim: (overlapping intervals, missing/conflicting strict CIKs)."""

    overlapping = store.con.execute(
        """
        SELECT count(*) AS overlapping_intervals
        FROM universe_us_listed_membership a
        JOIN universe_us_listed_membership b
          ON a.security_id=b.security_id AND a.universe_id=b.universe_id AND a.source=b.source
         AND a.membership_id < b.membership_id
         AND a.valid_from <= coalesce(b.valid_to,DATE '9999-12-31')
         AND b.valid_from <= coalesce(a.valid_to,DATE '9999-12-31')
        WHERE a.is_latest_revision AND b.is_latest_revision
        """
    ).fetchone()[0]
    conflicting = store.con.execute(
        """
        WITH identity_at_start AS (
          SELECT m.membership_id,m.cik,
                 count(DISTINCT h.id_value) AS cik_count, min(h.id_value) AS selected_cik
          FROM universe_us_listed_membership m
          LEFT JOIN security_identifier_history h
            ON h.security_id=m.security_id AND h.id_type='CIK'
           AND h.valid_from<=m.valid_from AND (h.valid_to IS NULL OR h.valid_to>m.valid_from)
           AND h.available_at<=m.available_at AND h.as_of_date<=m.valid_from
          WHERE m.universe_id='us_listed_v1' AND m.security_type='common'
            AND m.has_cik AND m.is_latest_revision
          GROUP BY m.membership_id,m.cik
        )
        SELECT count(*) AS missing_or_conflicting_cik
        FROM identity_at_start
        WHERE cik_count<>1 OR selected_cik IS DISTINCT FROM cik
        """
    ).fetchone()[0]
    return int(overlapping), int(conflicting)


def _seed_future_loaded_directory(store):
    """L2 §4 counterexample: 2020 bars, directory as-of 2019-12-31 but loaded 2026-09-20."""

    store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-X', 'CIK-0000999999', 'XCO', 'X Corp', 'test'),"
        "('SEC-Y', 'CIK-0000888888', 'YCO', 'Y Corp', 'test')"
    )
    _add_bars(
        store,
        [(sid, sym, day) for sid, sym in (("SEC-X", "XCO"), ("SEC-Y", "YCO")) for day in ("2020-01-02", "2020-01-03")],
    )
    _add_directory(
        store,
        [
            ("XCO", "X Corp - Common Stock", "NASDAQ", "2019-12-31", "2026-09-20 10:00:00"),
            ("YCO", "Y Corp - Common Stock", "NASDAQ", "2019-12-31", "2026-09-20 10:00:00"),
        ],
    )
    # Dated CIK evidence for SEC-X only; SEC-Y has nothing but a current entity_id.
    _add_cik(store, [("SEC-X", "0000320193", "2019-01-01", None, "2019-01-01", "2019-01-02 00:00:00")])


def test_future_loaded_directory_never_backdates_membership(tmp_store):
    from atx_db.universe_us_listed import (
        UniverseUsListedOptions,
        build_universe_us_listed,
        universe_us_listed,
    )

    _seed_future_loaded_directory(tmp_store)
    summary = build_universe_us_listed(
        tmp_store, UniverseUsListedOptions(lookback_days=5, as_of_date=_D("2026-09-25"), run_id="t")
    )
    strict = _membership(tmp_store)
    assert {row[0] for row in strict} == {"SEC-X", "SEC-Y"}
    # Late-known decisions are emitted late: no strict decision is available before the load.
    assert min(row[4] for row in strict) >= _TS("2026-09-20 10:00:00")
    assert summary.late_decisions["strict"] == 4
    # CIK only from dated identifier rows -- never the current securities.entity_id.
    by_security = {row[0]: row for row in strict}
    assert by_security["SEC-X"][7] is True and by_security["SEC-X"][8] == "0000320193"
    assert by_security["SEC-Y"][7] is False and by_security["SEC-Y"][8] is None
    assert by_security["SEC-Y"][10] == "member_no_cik"
    assert universe_us_listed(tmp_store, _D("2020-01-02")).empty
    assert sorted(universe_us_listed(tmp_store, _D("2026-09-20"))["security_id"]) == ["SEC-X", "SEC-Y"]
    assert _l2_sql_d(tmp_store) == (0, 0)

    # The labeled reconstruction models availability at bar close and says so on every row.
    reconstructed = _membership(tmp_store, "us_listed_reconstructed_v1")
    assert {row[0]: row[4] for row in reconstructed} == {
        "SEC-X": _TS("2020-01-02 22:00:00"),
        "SEC-Y": _TS("2020-01-02 22:00:00"),
    }
    rules = json.loads(reconstructed[0][11])
    assert rules["evidence_status"] == "reconstructed"
    assert rules["identity_basis"] == "current_ticker_unverified"
    assert rules["availability_basis"] == "modeled_backcast"
    assert rules["certification_eligible"] is False
    assert json.loads(strict[0][11])["evidence_status"] == "pit_snapshot"


def test_knowledge_cutoff_hides_inputs_loaded_after_it(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    _seed_future_loaded_directory(tmp_store)
    summary = build_universe_us_listed(
        tmp_store, UniverseUsListedOptions(lookback_days=5, as_of_date=_D("2026-09-19"), run_id="t")
    )
    assert summary.rows_by_universe["us_listed_v1"] == 0
    assert summary.diagnostics["directory_rows_after_knowledge_cutoff"] == 2
    assert summary.dispositions["strict"]["no_listing_reference"] == 4
    # Nothing listed as of the cutoff: the reconstruction retains the names, labeled.
    reconstructed = _membership(tmp_store, "us_listed_reconstructed_v1")
    assert {row[10] for row in reconstructed} == {"reconstructed_no_listing_evidence"}
    assert {(row[5], row[6]) for row in reconstructed} == {("unknown", "UNKNOWN")}


def test_late_known_run_is_split_from_on_time_decisions(tmp_store):
    """A snapshot dated 01-02 but loaded 01-05 12:00: sessions 01-02..01-04 are late-known
    (emitted at the load clock, in their own interval); from 01-05 the same snapshot is
    usable by the session cutoff and decisions are on time again."""

    from atx_db.universe_us_listed import (
        UniverseUsListedOptions,
        build_universe_us_listed,
        universe_us_listed,
    )

    _add_bars(
        tmp_store,
        [("SEC-L", "LATE", day) for day in ("2024-01-02", "2024-01-03", "2024-01-04", "2024-01-05", "2024-01-08")],
    )
    _add_directory(tmp_store, [("LATE", "Late Corp - Common Stock", "NASDAQ", "2024-01-02", "2024-01-05 12:00:00")])
    build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=3, run_id="t"))
    rows = _membership(tmp_store)
    assert [(row[2], row[3], row[4], row[12]) for row in rows] == [
        (_D("2024-01-02"), _D("2024-01-04"), _TS("2024-01-05 12:00:00"), 3),
        (_D("2024-01-05"), None, _TS("2024-01-05 22:00:00"), 2),
    ]
    assert universe_us_listed(tmp_store, _D("2024-01-03")).empty
    assert list(universe_us_listed(tmp_store, _D("2024-01-05"))["security_id"]) == ["SEC-L"]


def test_cik_change_inside_one_security_splits_intervals_with_their_own_cik(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    days = ("2024-01-02", "2024-01-03", "2024-01-04", "2024-01-05", "2024-01-08", "2024-01-09", "2024-01-10")
    _add_bars(tmp_store, [(sid, sym, day) for sid, sym in (("SEC-S", "SSS"), ("SEC-T", "TTT")) for day in days])
    _add_directory(
        tmp_store,
        [
            ("SSS", "S Corp - Common Stock", "NASDAQ", "2024-01-01", "2024-01-01 12:00:00"),
            ("TTT", "T Corp - Common Stock", "NASDAQ", "2024-01-01", "2024-01-01 12:00:00"),
        ],
    )
    _add_cik(
        tmp_store,
        [
            ("SEC-S", "0000000011", "2020-01-01", "2024-01-05", "2020-01-01", "2020-01-01 00:00:00"),
            ("SEC-S", "0000000022", "2024-01-05", None, "2024-01-04", "2024-01-04 09:00:00"),
            # SEC-T's successor CIK is only *learned* on 01-09: 01-05/01-08 have no CIK.
            ("SEC-T", "0000000033", "2020-01-01", "2024-01-05", "2020-01-01", "2020-01-01 00:00:00"),
            ("SEC-T", "0000000044", "2024-01-05", None, "2024-01-05", "2024-01-09 09:00:00"),
        ],
    )
    build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=3, run_id="t"))
    rows = _membership(tmp_store)
    assert [(row[0], row[2], row[3], row[8], row[10]) for row in rows] == [
        ("SEC-S", _D("2024-01-02"), _D("2024-01-04"), "0000000011", "member"),
        ("SEC-S", _D("2024-01-05"), None, "0000000022", "member"),
        ("SEC-T", _D("2024-01-02"), _D("2024-01-04"), "0000000033", "member"),
        ("SEC-T", _D("2024-01-05"), _D("2024-01-08"), None, "member_no_cik"),
        ("SEC-T", _D("2024-01-09"), None, "0000000044", "member"),
    ]
    assert _l2_sql_d(tmp_store) == (0, 0)


def test_conflicting_or_malformed_dated_ciks_are_not_a_cik(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    _add_bars(tmp_store, [("SEC-Z", "ZZZ", "2024-01-02"), ("SEC-W", "WWW", "2024-01-02")])
    _add_directory(
        tmp_store,
        [
            ("ZZZ", "Z Corp - Common Stock", "NASDAQ", "2024-01-01", "2024-01-01 12:00:00"),
            ("WWW", "W Corp - Common Stock", "NASDAQ", "2024-01-01", "2024-01-01 12:00:00"),
        ],
    )
    _add_cik(
        tmp_store,
        [
            ("SEC-Z", "0000000001", "2020-01-01", None, "2020-01-01", "2020-01-01 00:00:00"),
            ("SEC-Z", "0000000002", "2021-01-01", None, "2021-01-01", "2021-01-01 00:00:00"),
            ("SEC-W", "CIK-ABC", "2020-01-01", None, "2020-01-01", "2020-01-01 00:00:00"),
        ],
    )
    build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=3, run_id="t"))
    rows = {row[0]: row for row in _membership(tmp_store)}
    for security_id in ("SEC-Z", "SEC-W"):
        assert rows[security_id][7] is False
        assert rows[security_id][8] is None
        assert rows[security_id][10] == "member_conflicting_cik"
    assert _l2_sql_d(tmp_store) == (0, 0)


def test_market_cap_is_the_revision_visible_at_the_cutoff(tmp_store):
    """B's corrected market cap (150) arrives in 2026 as the latest revision; the decile
    on 2024-01-02 must use the 50 visible at the cutoff (B below A), not the correction."""

    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    _add_bars(tmp_store, [("SEC-A", "AAA", "2024-01-02"), ("SEC-B", "BBB", "2024-01-02")])
    _add_directory(
        tmp_store,
        [
            ("AAA", "A Corp - Common Stock", "NASDAQ", "2024-01-01", "2024-01-01 12:00:00"),
            ("BBB", "B Corp - Common Stock", "NASDAQ", "2024-01-01", "2024-01-01 12:00:00"),
        ],
    )
    _add_market_caps(
        tmp_store,
        [
            ("SEC-A", "2024-01-02", 100.0, "2024-01-02 22:00:00", True, "r1"),
            ("SEC-B", "2024-01-02", 50.0, "2024-01-02 21:00:00", False, "r1"),
            ("SEC-B", "2024-01-02", 150.0, "2026-01-01 00:00:00", True, "r2"),
        ],
    )
    build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=3, run_id="t"))
    for universe_id in ("us_listed_v1", "us_listed_reconstructed_v1"):
        deciles = {row[0]: row[9] for row in _membership(tmp_store, universe_id)}
        assert deciles == {"SEC-A": 2, "SEC-B": 1}, universe_id


def test_late_known_member_never_enters_an_on_time_decile(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    _add_bars(tmp_store, [("SEC-A", "AAA", "2024-01-02"), ("SEC-C", "CCC", "2024-01-02")])
    _add_directory(
        tmp_store,
        [
            ("AAA", "A Corp - Common Stock", "NASDAQ", "2024-01-01", "2024-01-01 12:00:00"),
            ("CCC", "C Corp - Common Stock", "NASDAQ", "2024-01-01", "2024-06-01 00:00:00"),
        ],
    )
    _add_market_caps(
        tmp_store,
        [
            ("SEC-A", "2024-01-02", 100.0, "2024-01-02 22:00:00", True, "r1"),
            ("SEC-C", "2024-01-02", 50.0, "2024-01-02 22:00:00", True, "r1"),
        ],
    )
    build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=3, run_id="t"))
    rows = {row[0]: row for row in _membership(tmp_store)}
    # A was known at 2024-01-02 22:00, when C was not: A ranks alone (decile 1). C is
    # ranked at its own clock against everything known by then (A and C -> C lowest).
    assert rows["SEC-A"][4] == _TS("2024-01-02 22:00:00") and rows["SEC-A"][9] == 1
    assert rows["SEC-C"][4] == _TS("2024-06-01 00:00:00") and rows["SEC-C"][9] == 1


def test_reconstruction_backcasts_by_latest_symbol_and_labels_the_tail(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    days = ("2024-01-02", "2024-01-03", "2024-01-04", "2024-01-05", "2024-01-08", "2024-01-09", "2024-01-10")
    _add_bars(
        tmp_store,
        # R1 renamed OLD -> NEW; R2 delisted (symbol absent from the current directory);
        # R3 stopped trading long before the snapshot, whose REUSE line is someone else's.
        [("SEC-R1", "OLD", day) for day in days[:4]]
        + [("SEC-R1", "NEW", day) for day in days[4:]]
        + [("SEC-R2", "GONE", day) for day in days[:3]]
        + [("SEC-R3", "REUSE", day) for day in days[:2]]
        + [("SEC-R4", "LIVE", day) for day in days],
    )
    _add_directory(
        tmp_store,
        [
            ("NEW", "Newco Inc. - Common Stock", "NASDAQ", "2024-01-10", "2024-01-10 12:00:00"),
            ("REUSE", "Someone Else Holdings - Common Stock", "N", "2024-01-10", "2024-01-10 12:00:00"),
            ("LIVE", "Live Corp", "N", "2024-01-10", "2024-01-10 12:00:00"),
        ],
    )
    # Current (open) CIK mapping learned today: used by the reconstruction only.
    _add_cik(tmp_store, [("SEC-R1", "0000123456", "2026-09-20", None, "2026-09-20", "2026-09-20 10:00:00")])
    build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t"))

    recon = _membership(tmp_store, "us_listed_reconstructed_v1")
    by_security: dict[str, list] = {}
    for row in recon:
        by_security.setdefault(row[0], []).append(row)
    assert [(row[1], row[2], row[5], row[6], row[8], row[10]) for row in by_security["SEC-R1"]] == [
        ("OLD", _D("2024-01-02"), "common", "XNAS", "0000123456", "member"),
        ("NEW", _D("2024-01-08"), "common", "XNAS", "0000123456", "member"),
    ]
    assert [row[4] for row in by_security["SEC-R1"]] == [_TS("2024-01-02 22:00:00"), _TS("2024-01-08 22:00:00")]
    for security_id in ("SEC-R2", "SEC-R3"):
        (row,) = by_security[security_id]
        assert (row[5], row[6], row[10]) == ("unknown", "UNKNOWN", "reconstructed_no_listing_evidence")
    # No positive common evidence: kept by the reconstruction, strict-ineligible.
    assert [(row[5], row[6]) for row in by_security["SEC-R4"]] == [("common_unverified", "XNYS")]

    strict = _membership(tmp_store)
    # Strict sees the snapshot only from its own date and has no dated CIK in 2024.
    assert [(row[0], row[1], row[2], row[7]) for row in strict] == [("SEC-R1", "NEW", _D("2024-01-10"), False)]


def test_batching_does_not_change_the_output(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    _seed_universe_warehouse(tmp_store)
    _seed_future_loaded_directory(tmp_store)
    snapshot_sql = "SELECT * EXCLUDE (source_loaded_at) FROM universe_us_listed_membership ORDER BY membership_id"
    one = build_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t"))
    first = tmp_store.con.execute(snapshot_sql).fetchall()
    many = build_universe_us_listed(
        tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t", max_bars_per_batch=1)
    )
    assert one.batch_count == 1 and many.batch_count == many.bar_rows > 1
    assert tmp_store.con.execute(snapshot_sql).fetchall() == first
    assert one.dispositions == many.dispositions


@pytest.mark.slow
def test_two_million_bar_build_is_memory_bounded(tmp_store):
    """2M bars at DuckDB 256MB / 1 thread: Python never holds a security-day frame."""

    import tracemalloc

    from atx_db.market_daily import MARKET_DAILY_SOURCE_NAME
    from atx_db.universe_us_listed import UniverseUsListedOptions, build_universe_us_listed

    con = tmp_store.con
    securities, sessions = 2000, 1000
    con.execute(
        f"""
        INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, volume,
                                       available_at, as_of_date, is_latest_revision)
        SELECT 'bench', 'SEC-' || lpad(s::VARCHAR, 6, '0'), 'S' || s::VARCHAR,
               DATE '2020-01-01' + d::INTEGER, 10.0, 1000,
               (DATE '2020-01-01' + d::INTEGER)::TIMESTAMP + INTERVAL 22 HOUR,
               DATE '2020-01-01' + d::INTEGER, true
        FROM range({securities}) a(s), range({sessions}) b(d)
        """
    )
    name = (
        "CASE WHEN s % 10 < 7 THEN 'Company ' || s || ' Common Stock' "
        "WHEN s % 10 = 7 THEN 'Company ' || s || ' Fund' ELSE 'Company ' || s || ' Holdings' END"
    )
    for as_of, clock in (
        ("DATE '2020-01-01'", "TIMESTAMP '2020-01-01 12:00:00'"),
        (f"DATE '2020-01-01' + {sessions - 1}", "TIMESTAMP '2026-09-20 10:00:00'"),
    ):
        con.execute(
            f"""
            INSERT INTO nasdaq_symbol_directory (directory, symbol, security_name, exchange, etf,
                                                 test_issue, as_of_date, source_url, available_at)
            SELECT 'nasdaqlisted', 'S' || s::VARCHAR, {name}, 'NASDAQ', false, false, {as_of}, 'file://b', {clock}
            FROM range({securities}) a(s)
            """
        )
    con.execute(
        f"""
        INSERT INTO security_identifier_history (security_id, id_type, id_value, valid_from, valid_to,
                                                 as_of_date, available_at, source)
        SELECT 'SEC-' || lpad(s::VARCHAR, 6, '0'), 'CIK', lpad((1000 + s)::VARCHAR, 10, '0'), DATE '2019-01-01',
               CASE WHEN s % 10 = 0 THEN DATE '2020-06-01' END, DATE '2019-01-01', TIMESTAMP '2019-01-01', 'b'
        FROM range({securities}) a(s)
        UNION ALL
        SELECT 'SEC-' || lpad(s::VARCHAR, 6, '0'), 'CIK', lpad((900000 + s)::VARCHAR, 10, '0'),
               DATE '2020-06-01', NULL, DATE '2019-01-01', TIMESTAMP '2019-01-01', 'b'
        FROM range({securities}) a(s) WHERE s % 10 = 0
        """
    )
    con.execute(
        """
        INSERT INTO market_daily_metrics (market_daily_id, source, security_id, trade_date, market_cap,
                                          available_at, inputs_hash, as_of_date, is_latest_revision)
        SELECT security_id || '|' || trade_date, ?, security_id, trade_date,
               1e6 * (1 + hash(security_id) % 1000), available_at, 'h', trade_date, true
        FROM equity_daily_bars
        """,
        [MARKET_DAILY_SOURCE_NAME],
    )
    con.execute("SET memory_limit = '256MB'")
    con.execute("SET threads = 1")
    tracemalloc.start()
    try:
        summary = build_universe_us_listed(
            tmp_store, UniverseUsListedOptions(run_id="bench", as_of_date=_D("2026-09-25"))
        )
        _current, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()
    assert summary.bar_rows == securities * sessions
    assert peak < 300 * 2**20, f"python peak {peak / 2**20:.1f} MiB"
    # 1400 common names + 200 in-history CIK changes (s % 10 == 0, all common);
    # fund and common_unverified names are strict-ineligible.
    assert summary.rows_by_universe["us_listed_v1"] == 1600
    assert summary.dispositions["strict"]["eligible"] == 1400 * sessions
    assert _l2_sql_d(tmp_store) == (0, 0)
    missing_deciles = con.execute(
        "SELECT count(*) FROM universe_us_listed_membership WHERE security_type = 'common' "
        "AND market_cap_decile IS NULL"
    ).fetchone()[0]
    assert int(missing_deciles) == 0
