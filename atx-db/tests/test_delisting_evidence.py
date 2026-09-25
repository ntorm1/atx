"""Tier1-S4 T3: public delisting evidence streams and their precedence fold."""

from __future__ import annotations

import datetime as dt

import pytest


def _seed_bars(store, security_id, symbol, days):
    values = ",".join(
        f"('test','{security_id}','{symbol}',DATE '{day}',10.0,1000,TIMESTAMP '{day} 22:00:00',DATE '{day}',true)"
        for day in days
    )
    store.con.execute(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, volume, "
        "available_at, as_of_date, is_latest_revision) VALUES " + values
    )


def _business_days(start, count):
    day = dt.date.fromisoformat(start)
    out = []
    while len(out) < count:
        if day.weekday() < 5:
            out.append(day.isoformat())
        day += dt.timedelta(days=1)
    return out


SESSIONS = _business_days("2024-01-01", 60)


def _seed_two_securities(store):
    store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-LIVE','CIK-0000000001','LIVE','Live Corp','test'),"
        "('SEC-GONE','CIK-0000000002','GONE','Gone Corp','test')"
    )
    _seed_bars(store, "SEC-LIVE", "LIVE", SESSIONS)
    _seed_bars(store, "SEC-GONE", "GONE", SESSIONS[:10])


def test_archive_last_trade_fires_only_past_the_gap(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    rows = refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(archive_gap_sessions=30, run_id="t"))
    assert rows == 1
    row = tmp_store.con.execute(
        "SELECT security_id, evidence_kind, delist_date, reason_category, reason_confidence, "
        "evidence_rank, delist_code FROM delisting_evidence"
    ).fetchone()
    assert row[0] == "SEC-GONE"
    assert row[1] == "archive_last_trade"
    # Effective-date basis: the first observed session after the last observed trade.
    assert row[2] == dt.date.fromisoformat(SESSIONS[10])
    assert row[3] == "unknown"
    assert row[4] == "low"
    assert int(row[5]) == 4
    assert row[6] == "ARCHIVE_LAST_TRADE"


def test_a_security_still_trading_at_the_archive_end_is_never_inferred(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    live = tmp_store.con.execute("SELECT count(*) FROM delisting_evidence WHERE security_id = 'SEC-LIVE'").fetchone()[0]
    assert int(live) == 0


def test_a_short_gap_is_not_a_delist(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    rows = refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(archive_gap_sessions=55, run_id="t"))
    assert rows == 0


def test_form_25_nse_is_an_exchange_delist(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','0000000002-24-000001',DATE '2024-01-16','25-NSE',"
        "TIMESTAMP '2024-01-16 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    row = tmp_store.con.execute(
        "SELECT reason_category, evidence_rank, available_at FROM delisting_evidence "
        "WHERE evidence_kind = 'sec_form_25'"
    ).fetchone()
    assert row[0] == "exchange_delist"
    assert int(row[1]) == 1
    assert row[2] == dt.datetime(2024, 1, 16, 17, 0, 0)


def test_form_25_after_a_merger_filing_is_a_merger(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','0000000002-23-000009',DATE '2023-11-01','DEFM14A',"
        "TIMESTAMP '2023-11-01 17:00:00','file://t'),"
        "('SEC-GONE','0000000002','0000000002-24-000001',DATE '2024-01-16','25',"
        "TIMESTAMP '2024-01-16 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    reason = tmp_store.con.execute(
        "SELECT reason_category FROM delisting_evidence WHERE evidence_kind = 'sec_form_25'"
    ).fetchone()[0]
    assert reason == "merger_acquisition"


def test_a_bare_form_25_with_no_merger_evidence_is_unexplained(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','0000000002-24-000001',DATE '2024-01-16','25',"
        "TIMESTAMP '2024-01-16 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    reason = tmp_store.con.execute(
        "SELECT reason_category FROM delisting_evidence WHERE evidence_kind = 'sec_form_25'"
    ).fetchone()[0]
    # An issuer-requested removal says that the security left, not why (plan A3 (3)).
    assert reason == "unknown"


def test_nasdaq_delete_and_form_15_are_captured(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
        "effective_date, as_of_date, source_url, is_latest_revision) VALUES "
        "('EV-1','GONE','SEC-GONE','D',DATE '2024-01-17',DATE '2024-01-17','file://t',true)"
    )
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','0000000002-24-000002',DATE '2024-02-01','15-12B',"
        "TIMESTAMP '2024-02-01 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    kinds = {
        str(row[0]): (str(row[1]), int(row[2]))
        for row in tmp_store.con.execute(
            "SELECT evidence_kind, reason_category, evidence_rank FROM delisting_evidence"
        ).fetchall()
    }
    assert kinds["nasdaq_delete"] == ("unknown", 2)
    assert kinds["sec_form_15"] == ("voluntary", 3)
    assert kinds["archive_last_trade"] == ("unknown", 4)


def test_bankruptcy_overlay_upgrades_the_reason(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_symbol_directory (directory, symbol, security_name, exchange, etf, "
        "test_issue, financial_status, as_of_date, source_url, is_latest_revision) VALUES "
        "('nasdaqlisted','GONE','Gone Corp - Common Stock','NASDAQ',false,false,'Q',"
        "DATE '2024-01-10','file://t',true)"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    row = tmp_store.con.execute("SELECT reason_category, reason_confidence FROM delisting_evidence").fetchone()
    assert row[0] == "bankruptcy"
    assert row[1] == "high"


def test_the_fold_keeps_one_event_per_cessation_with_the_primary_notice(tmp_store):
    from atx_db.delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
        "effective_date, as_of_date, source_url, is_latest_revision) VALUES "
        "('EV-1','GONE','SEC-GONE','D',DATE '2024-01-12',DATE '2024-01-12','file://t',true)"
    )
    options = DelistingEvidenceOptions(run_id="t")
    refresh_delisting_evidence(tmp_store, options)
    # The corroborated delete (notice date = last trade) and the archive cessation are one
    # cessation cluster: exactly one event, dated the first session after the last trade.
    assert fold_evidence_into_delisting_events(tmp_store, options) == 1
    winner = tmp_store.con.execute(
        "SELECT listing_status_source, delist_code, delist_reason, inferred_from_absence, delist_date, "
        "json_extract_string(details_json, '$.effective_date_basis') "
        "FROM delisting_events WHERE source = ?",
        [options.event_source],
    ).fetchone()
    assert winner == (
        "nasdaq_delete",
        "NASDAQ_DELETE",
        "unknown",  # a trading-system delete says that, not why
        False,
        dt.date.fromisoformat(SESSIONS[10]),
        "last_observed_trade",
    )


def test_the_fold_never_touches_the_listing_status_source(tmp_store):
    from atx_db.delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    tmp_store.con.execute(
        "INSERT INTO delisting_events (delisting_event_id, source, listing_status_source, "
        "source_listing_status_id, symbol, delist_date, as_of_date, available_at, delist_code, "
        "delist_reason, delisting_return_type, return_policy, return_confidence, evidence_source, "
        "evidence_source_table, method, evidence_confidence) VALUES "
        "('legacy','atx_delisting_proxy_v1','legacy_src','legacy_id','OLD',DATE '2020-01-01',"
        "DATE '2020-01-01',TIMESTAMP '2020-01-01 22:00:00','NASDAQ_DELETE','exchange_delete',"
        "'UNOBSERVED','none','none','listing_status_intervals','listing_status_intervals',"
        "'trading_system_delete_action','high')"
    )
    _seed_two_securities(tmp_store)
    options = DelistingEvidenceOptions(run_id="t")
    refresh_delisting_evidence(tmp_store, options)
    fold_evidence_into_delisting_events(tmp_store, options)
    survivors = tmp_store.con.execute(
        "SELECT count(*) FROM delisting_events WHERE source = 'atx_delisting_proxy_v1'"
    ).fetchone()[0]
    assert int(survivors) == 1


def test_refresh_is_idempotent(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    options = DelistingEvidenceOptions(run_id="t")
    first = refresh_delisting_evidence(tmp_store, options)
    second = refresh_delisting_evidence(tmp_store, options)
    assert first == second
    total = tmp_store.con.execute("SELECT count(*) FROM delisting_evidence").fetchone()[0]
    assert int(total) == second


@pytest.mark.parametrize("latest_status", ["N", None])
def test_bankruptcy_overlay_uses_the_newest_snapshot_per_event(tmp_store, latest_status):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_symbol_directory "
        "(directory, symbol, financial_status, as_of_date, source_url, is_latest_revision) VALUES "
        "('nasdaqlisted','GONE','EQ',DATE '2024-01-01','file://t',true),"
        "('nasdaqlisted','GONE',?,DATE '2024-01-10','file://t',true),"
        "('nasdaqlisted','GONE','Q',DATE '2024-01-13','file://t',true)",
        [latest_status],
    )
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events "
        "(event_id, symbol, security_id, nasdaq_action, effective_date, as_of_date, source_url, is_latest_revision) VALUES "
        "('EV-OLD','GONE','SEC-GONE','D',DATE '2024-01-09',DATE '2024-01-09','file://t',true),"
        "('EV-NEW','GONE','SEC-GONE','D',DATE '2024-01-12',DATE '2024-01-12','file://t',true)"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(include_archive_inference=False))
    assert tmp_store.con.execute(
        "SELECT source_event_id, reason_category FROM delisting_evidence ORDER BY delist_date"
    ).fetchall() == [("EV-OLD", "bankruptcy"), ("EV-NEW", "unknown")]


def test_evidence_sources_coexist_and_refresh_independently(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    first_options = DelistingEvidenceOptions(run_id="original")
    other_options = DelistingEvidenceOptions(source="other_evidence", run_id="other")
    refresh_delisting_evidence(tmp_store, first_options)
    original = tmp_store.con.execute(
        "SELECT * FROM delisting_evidence WHERE source = ?", [first_options.source]
    ).fetchall()
    refresh_delisting_evidence(tmp_store, other_options)
    assert (
        tmp_store.con.execute("SELECT * FROM delisting_evidence WHERE source = ?", [first_options.source]).fetchall()
        == original
    )
    other = tmp_store.con.execute(
        "SELECT * FROM delisting_evidence WHERE source = ?", [other_options.source]
    ).fetchall()
    refresh_delisting_evidence(tmp_store, first_options)
    assert (
        tmp_store.con.execute("SELECT * FROM delisting_evidence WHERE source = ?", [other_options.source]).fetchall()
        == other
    )
    assert tmp_store.con.execute("SELECT count(DISTINCT evidence_id) FROM delisting_evidence").fetchone()[0] == 2
    assert original[0][0] != other[0][0]


@pytest.mark.parametrize("kind", ["sec_form_25", "sec_form_15", "nasdaq_delete"])
@pytest.mark.parametrize(
    ("event_date", "available_at", "expected"),
    [
        ("2024-01-12", "2024-01-12 22:00:00", 1),
        ("2024-01-12", "2024-01-12 22:00:01", 0),
        ("2024-01-13", "2024-01-12 17:00:00", 0),
    ],
)
def test_as_of_bounds_explicit_evidence_by_date_and_availability(tmp_store, kind, event_date, available_at, expected):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    if kind == "nasdaq_delete":
        tmp_store.con.execute(
            "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
            "effective_date, as_of_date, source_file_created_at, source_url, is_latest_revision) "
            "VALUES ('EV','GONE','SEC-GONE','D',?,DATE '2024-01-12',?,'file://t',true)",
            [event_date, available_at],
        )
    else:
        tmp_store.con.execute(
            "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, "
            "form, acceptance_datetime, source_url) "
            "VALUES ('SEC-GONE','0000000002','ACC',?,?,?,'file://t')",
            [event_date, "25" if kind == "sec_form_25" else "15-12B", available_at],
        )
    options = DelistingEvidenceOptions(as_of_date=dt.date(2024, 1, 12), include_archive_inference=False)
    assert refresh_delisting_evidence(tmp_store, options) == expected


def test_as_of_bounds_the_archive_sessions_and_last_bar(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    early = DelistingEvidenceOptions(as_of_date=dt.date.fromisoformat(SESSIONS[20]))
    assert refresh_delisting_evidence(tmp_store, early) == 0
    # The future resumed trade must not hide the gap visible at the historical cutoff.
    _seed_bars(tmp_store, "SEC-GONE", "GONE", [SESSIONS[50]])
    cutoff = dt.date.fromisoformat(SESSIONS[40])
    assert refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(as_of_date=cutoff)) == 1
    assert tmp_store.con.execute("SELECT available_at FROM delisting_evidence").fetchone()[0] == dt.datetime.combine(
        cutoff, dt.time(22)
    )
    assert refresh_delisting_evidence(tmp_store) == 0


def test_archive_ignores_superseded_sessions(tmp_store):
    from atx_db.delisting_evidence import refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "UPDATE equity_daily_bars SET is_latest_revision = false WHERE trade_date > ?", [SESSIONS[20]]
    )
    assert refresh_delisting_evidence(tmp_store) == 0


def test_archive_ignores_superseded_last_bar(tmp_store):
    from atx_db.delisting_evidence import refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    _seed_bars(tmp_store, "SEC-GONE", "GONE", [SESSIONS[-1]])
    tmp_store.con.execute(
        "UPDATE equity_daily_bars SET is_latest_revision = false WHERE security_id = 'SEC-GONE' AND trade_date = ?",
        [SESSIONS[-1]],
    )
    assert refresh_delisting_evidence(tmp_store) == 1
    assert tmp_store.con.execute("SELECT delist_date FROM delisting_evidence").fetchone()[0] == dt.date.fromisoformat(
        SESSIONS[10]
    )


def test_delete_and_directory_ignore_superseded_revisions(tmp_store):
    from atx_db.delisting_evidence import refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
        "effective_date, as_of_date, source_url, is_latest_revision) VALUES "
        "('EV','GONE','SEC-GONE','D',DATE '2024-01-12',DATE '2024-01-12','file://t',false)"
    )
    tmp_store.con.execute(
        "INSERT INTO nasdaq_symbol_directory "
        "(directory, symbol, financial_status, as_of_date, source_url, is_latest_revision) VALUES "
        "('nasdaqlisted','GONE','Q',DATE '2024-01-10','file://t',false)"
    )
    assert refresh_delisting_evidence(tmp_store) == 1
    assert tmp_store.con.execute("SELECT evidence_kind, reason_category FROM delisting_evidence").fetchall() == [
        ("archive_last_trade", "unknown")
    ]


def test_as_of_bounds_merger_reason_and_carries_its_availability(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, "
        "form, acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','FORM25',DATE '2024-01-12','25',TIMESTAMP '2024-01-12 17:00:00','file://t'),"
        "('SEC-GONE','0000000002','MERGER',DATE '2024-01-10','DEFM14A',TIMESTAMP '2024-01-13 17:00:00','file://t')"
    )
    for day, expected in [(12, "unknown"), (13, "merger_acquisition")]:
        refresh_delisting_evidence(
            tmp_store, DelistingEvidenceOptions(as_of_date=dt.date(2024, 1, day), include_archive_inference=False)
        )
        assert tmp_store.con.execute("SELECT reason_category, available_at FROM delisting_evidence").fetchone() == (
            expected,
            dt.datetime(2024, 1, day, 17),
        )


def test_as_of_bounds_bankruptcy_reason_and_carries_its_availability(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, "
        "form, acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','FORM15',DATE '2024-01-12','15-12B',TIMESTAMP '2024-01-12 17:00:00','file://t')"
    )
    tmp_store.con.execute(
        "INSERT INTO nasdaq_symbol_directory "
        "(directory, symbol, financial_status, as_of_date, available_at, source_url, is_latest_revision) VALUES "
        "('nasdaqlisted','GONE','Q',DATE '2024-01-10',TIMESTAMP '2024-01-13 17:00:00','file://t',true)"
    )
    for day, expected in [(12, "voluntary"), (13, "bankruptcy")]:
        refresh_delisting_evidence(
            tmp_store, DelistingEvidenceOptions(as_of_date=dt.date(2024, 1, day), include_archive_inference=False)
        )
        assert tmp_store.con.execute("SELECT reason_category, available_at FROM delisting_evidence").fetchone() == (
            expected,
            dt.datetime(2024, 1, day, 17),
        )


@pytest.mark.parametrize("cleared_status", ["N", None])
def test_cleared_bankruptcy_carries_the_new_snapshot_availability(tmp_store, cleared_status):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, "
        "form, acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','FORM15',DATE '2024-01-12','15-12B',TIMESTAMP '2024-01-12 17:00:00','file://t')"
    )
    tmp_store.con.execute(
        "INSERT INTO nasdaq_symbol_directory "
        "(directory, symbol, financial_status, as_of_date, available_at, source_url, is_latest_revision) VALUES "
        "('nasdaqlisted','GONE','Q',DATE '2024-01-01',TIMESTAMP '2024-01-01 17:00:00','file://t',true),"
        "('nasdaqlisted','GONE',?,DATE '2024-01-10',TIMESTAMP '2024-01-13 17:00:00','file://t',true)",
        [cleared_status],
    )
    for day, expected in [(12, "bankruptcy"), (13, "voluntary")]:
        refresh_delisting_evidence(
            tmp_store, DelistingEvidenceOptions(as_of_date=dt.date(2024, 1, day), include_archive_inference=False)
        )
        assert tmp_store.con.execute("SELECT reason_category, available_at FROM delisting_evidence").fetchone() == (
            expected,
            dt.datetime(2024, 1, day, 17),
        )


def test_fold_independently_bounds_dates_availability_and_revisions(tmp_store):
    from atx_db.delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    # SEC-GONE's last trade is Fri 2024-01-12; its cessation is observable at the Mon
    # 2024-01-15 session (the delist date), not before.
    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, "
        "form, acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','KNOWN',DATE '2024-01-10','15-12B',TIMESTAMP '2024-01-10 17:00:00','file://t'),"
        "('SEC-GONE','0000000002','LATE',DATE '2024-01-11','15-12B',TIMESTAMP '2024-01-16 17:00:00','file://t'),"
        "('SEC-GONE','0000000002','FUTURE',DATE '2024-01-16','15-12B',TIMESTAMP '2024-01-12 17:00:00','file://t'),"
        "('SEC-GONE','0000000002','STALE',DATE '2024-01-09','15-12B',TIMESTAMP '2024-01-09 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(include_archive_inference=False))
    tmp_store.con.execute("UPDATE delisting_evidence SET is_latest_revision = false WHERE source_event_id = 'STALE'")
    assert (
        fold_evidence_into_delisting_events(tmp_store, DelistingEvidenceOptions(as_of_date=dt.date(2024, 1, 12))) == 0
    )
    assert (
        fold_evidence_into_delisting_events(tmp_store, DelistingEvidenceOptions(as_of_date=dt.date(2024, 1, 15))) == 1
    )
    row = tmp_store.con.execute(
        "SELECT source_event_id, delist_date, available_at, delist_reason, "
        "json_array_length(json_extract(details_json, '$.evidence')) FROM delisting_events"
    ).fetchone()
    assert row == ("KNOWN", dt.date(2024, 1, 15), dt.datetime(2024, 1, 15, 22), "voluntary", 1)


# ---------------------------------------------------------------------------
# A3 economic fixtures: cessation-anchored events, corroboration window, reasons, RX2.
# ---------------------------------------------------------------------------

LONG_SESSIONS = _business_days("2023-01-02", 280)


def _seed_priced_bars(store, security_id, symbol, days, *, close=10.0, adjusted=None):
    adjusted = close if adjusted is None else adjusted
    store.con.executemany(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, adjusted_close, "
        "volume, available_at, as_of_date, is_latest_revision) VALUES ('test', ?, ?, ?, ?, ?, 1000, ?, ?, true)",
        [
            (security_id, symbol, day, close, adjusted, dt.datetime.fromisoformat(day) + dt.timedelta(hours=22), day)
            for day in days
        ],
    )


def _file(store, security_id, cik, accession, filing_date, form, accepted=None):
    accepted = accepted or f"{filing_date} 17:00:00"
    store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES (?, ?, ?, ?, ?, ?, 'file://t')",
        [security_id, cik, accession, filing_date, form, accepted],
    )


def _calendar_days_before(day: str, days: int) -> str:
    return (dt.date.fromisoformat(day) - dt.timedelta(days=days)).isoformat()


def _build(store, **option_overrides):
    from atx_db.delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    options = DelistingEvidenceOptions(run_id="a3", **option_overrides)
    refresh_delisting_evidence(store, options)
    return fold_evidence_into_delisting_events(store, options)


def _events(store):
    return store.con.execute(
        "SELECT security_id, delist_date, delist_reason, inferred_from_absence, available_at, "
        "json_extract_string(details_json, '$.effective_date_basis'), "
        "json_extract(details_json, '$.cik_linked')::BOOLEAN "
        "FROM delisting_events ORDER BY security_id"
    ).fetchall()


def test_issuer_25nse_for_notes_never_terminates_the_trading_common(tmp_store):
    from atx_db.calendar import TradingCalendarDataset, TradingCalendarOptions
    from atx_db.delisting import refresh_delisting_terminal_returns, refresh_survivorship_safe_forward_returns
    from atx_db.signal_eval import IC_HORIZONS

    common = "SEC-CIK-0000000010"
    notice_index = 59
    # The archive runs 280 sessions; the common trades 200 sessions past the 25-NSE filed for
    # its issuer's notes and stops within the gap threshold of the archive end.
    _seed_priced_bars(tmp_store, "SEC-LIVE", "LIVE", LONG_SESSIONS)
    _seed_priced_bars(tmp_store, common, "ACME", LONG_SESSIONS[: notice_index + 201])
    _file(tmp_store, common, "0000000010", "NOTES-25NSE", LONG_SESSIONS[notice_index], "25-NSE")

    assert _build(tmp_store) == 0
    evidence = tmp_store.con.execute(
        "SELECT evidence_kind, reason_category, json_extract_string(details_json, '$.disposition'), "
        "json_extract_string(details_json, '$.price_series_status') FROM delisting_evidence"
    ).fetchall()
    assert evidence == [
        ("sec_form_25", "exchange_delist", "issuer_notice_security_continues_trading", "traded_past_notice_window")
    ]

    refresh_delisting_terminal_returns(tmp_store)
    assert tmp_store.con.execute("SELECT count(*) FROM delisting_terminal_returns").fetchone()[0] == 0
    TradingCalendarDataset().load(tmp_store, TradingCalendarOptions())
    refresh_survivorship_safe_forward_returns(tmp_store)
    after_filing = tmp_store.con.execute(
        "SELECT count(*), count(*) FILTER (WHERE is_delisted_in_horizon) "
        "FROM forward_returns_survivorship_safe WHERE security_id = ? AND as_of_date > ?",
        [common, LONG_SESSIONS[notice_index]],
    ).fetchone()
    # Every horizon of every post-filing formation whose endpoint the common reached.
    expected = sum(max(0, 200 - horizon) for horizon in IC_HORIZONS)
    assert after_filing == (expected, 0)


def test_cash_merger_target_is_one_merger_event_without_policy_terminal(tmp_store):
    from atx_db.delisting import refresh_delisting_terminal_returns

    target = "SEC-CIK-0000000020"
    last = 100
    _seed_priced_bars(tmp_store, "SEC-LIVE", "LIVE", LONG_SESSIONS[:200])
    _seed_priced_bars(tmp_store, target, "TGT", LONG_SESSIONS[: last + 1])
    _file(tmp_store, target, "0000000020", "PROXY", _calendar_days_before(LONG_SESSIONS[last], 90), "DEFM14A")
    _file(tmp_store, target, "0000000020", "FORM25", _calendar_days_before(LONG_SESSIONS[last], 7), "25")

    assert _build(tmp_store) == 1
    ((security, delist_date, reason, inferred, available_at, basis, cik_linked),) = _events(tmp_store)
    assert (security, reason, inferred, basis, cik_linked) == (
        target,
        "merger_acquisition",
        False,
        "last_observed_trade",
        True,
    )
    assert delist_date == dt.date.fromisoformat(LONG_SESSIONS[last + 1])
    # Available once the 31st absent session is observed -- neither at the Form 25 filing nor
    # stamped with the (much later) archive end.
    assert available_at == dt.datetime.fromisoformat(LONG_SESSIONS[last + 31]) + dt.timedelta(hours=22)
    duplicates = tmp_store.con.execute(
        "SELECT count(*) FROM (SELECT 1 FROM delisting_events GROUP BY security_id, delist_date HAVING count(*) > 1)"
    ).fetchone()[0]
    assert duplicates == 0

    assert refresh_delisting_terminal_returns(tmp_store) == 0  # RX2: never Shumway for a merger


def test_notice_corroborated_cessation_is_known_before_the_gap_threshold(tmp_store):
    target = "SEC-CIK-0000000020"
    last = 100
    _seed_priced_bars(tmp_store, "SEC-LIVE", "LIVE", LONG_SESSIONS[:200])
    _seed_priced_bars(tmp_store, target, "TGT", LONG_SESSIONS[: last + 1])
    _file(tmp_store, target, "0000000020", "PROXY", _calendar_days_before(LONG_SESSIONS[last], 90), "DEFM14A")
    _file(tmp_store, target, "0000000020", "FORM25", _calendar_days_before(LONG_SESSIONS[last], 7), "25")

    # On the last trade date the cessation is not yet observable.
    assert _build(tmp_store, as_of_date=dt.date.fromisoformat(LONG_SESSIONS[last])) == 0
    # At the first absent session the corroborated Form 25 already forms the merger event.
    assert _build(tmp_store, as_of_date=dt.date.fromisoformat(LONG_SESSIONS[last + 1])) == 1
    ((_security, delist_date, reason, inferred, available_at, _basis, _cik),) = _events(tmp_store)
    assert (delist_date, reason, inferred) == (
        dt.date.fromisoformat(LONG_SESSIONS[last + 1]),
        "merger_acquisition",
        False,
    )
    assert available_at == dt.datetime.fromisoformat(LONG_SESSIONS[last + 1]) + dt.timedelta(hours=22)


def test_cik_less_gap_is_an_unknown_absence_event_with_shumway_policy_and_bias_exposure(tmp_store):
    from atx_db.delisting import delisting_policy_bias_exposure, refresh_delisting_terminal_returns

    orphan = "TBLTICKERHISTORY-999"
    linked = "SEC-CIK-0000000030"
    _seed_priced_bars(tmp_store, "SEC-LIVE", "LIVE", LONG_SESSIONS[:200])
    _seed_priced_bars(tmp_store, orphan, "ZZZ", LONG_SESSIONS[:51])
    _seed_priced_bars(tmp_store, linked, "LNK", LONG_SESSIONS[:61])
    _file(tmp_store, linked, "0000000030", "TENK", LONG_SESSIONS[10], "10-K")

    assert _build(tmp_store) == 2
    events = {row[0]: row for row in _events(tmp_store)}
    assert events[orphan][1:4] == (dt.date.fromisoformat(LONG_SESSIONS[51]), "unknown", True)
    assert events[orphan][6] is False
    assert events[linked][2:4] == ("unknown", True)
    assert events[linked][6] is True

    assert refresh_delisting_terminal_returns(tmp_store) == 2
    policies = tmp_store.con.execute(
        "SELECT security_id, terminal_return, terminal_return_source, terminal_return_policy "
        "FROM delisting_terminal_returns ORDER BY security_id"
    ).fetchall()
    assert policies == [
        (linked, -0.30, "policy", "performance_unknown"),
        (orphan, -0.30, "policy", "performance_unknown"),
    ]
    exposure = delisting_policy_bias_exposure(tmp_store)
    assert exposure["cik_less_unknown_performance_policy_rows"] == 1
    assert exposure["unknown_reason_performance_policy_rows"] == 2
    assert exposure["merger_or_voluntary_performance_policy_rows"] == 0
    check = tmp_store.con.execute(
        "SELECT status, observed_value FROM data_quality_checks WHERE check_name = 'policy_terminal_bias_exposure'"
    ).fetchall()
    assert check == [("warning", 1.0)]
    # L2 SQL E: no invalid terminal among the visible latest revisions.
    invalid = tmp_store.con.execute(
        """
        WITH visible_terminal AS (
          SELECT * FROM delisting_terminal_returns
          QUALIFY row_number() OVER (
            PARTITION BY source, security_id, delist_date
            ORDER BY available_at DESC, source_loaded_at DESC, terminal_return_id DESC) = 1
        )
        SELECT count(*) FILTER (WHERE terminal_return IS NULL OR NOT isfinite(terminal_return)
                                 OR terminal_return < -1) FROM visible_terminal
        """
    ).fetchone()[0]
    assert invalid == 0


@pytest.mark.parametrize("spelling", ["D", "d", "Delete", "delete", "DELETE", " Delete "])
def test_nasdaq_delete_spellings_are_equivalent(tmp_store, spelling):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, bx_action, "
        "effective_date, as_of_date, source_url, is_latest_revision) VALUES "
        "('EV-1','GONE','SEC-GONE',?,NULL,DATE '2024-01-12',DATE '2024-01-12','file://t',true),"
        "('EV-MOVE','GONE','SEC-GONE',?,'Add',DATE '2024-01-11',DATE '2024-01-11','file://t',true)",
        [spelling, spelling],
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(include_archive_inference=False))
    # One delete; a delete on one venue with an add on another is a venue move, not a removal.
    assert tmp_store.con.execute(
        "SELECT source_event_id, json_extract_string(details_json, '$.disposition') FROM delisting_evidence"
    ).fetchall() == [("EV-1", "corroborated_cessation")]


def test_uncorroborated_notices_are_retained_with_dispositions_and_never_become_events(tmp_store):
    _seed_two_securities(tmp_store)  # SEC-GONE last trades SESSIONS[9]; SEC-LIVE trades to the end
    tmp_store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-SHELL','CIK-0000000003','SHEL','Shell Corp','test')"
    )
    _file(tmp_store, "SEC-LIVE", "0000000001", "LIVE-25", SESSIONS[5], "25-NSE")  # trades to the archive end
    _file(tmp_store, "SEC-SHELL", "0000000003", "SHELL-15", SESSIONS[5], "15-12G")  # no price series at all
    _file(tmp_store, "SEC-GONE", "0000000002", "LATE-15", SESSIONS[40], "15-12B")  # 30 sessions after it stopped
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
        "effective_date, as_of_date, source_url, is_latest_revision) VALUES "
        "('NOID','ORPH',NULL,'D',DATE '2024-01-12',DATE '2024-01-12','file://t',true)"
    )
    assert _build(tmp_store) == 1  # only SEC-GONE's archive cessation
    dispositions = dict(
        tmp_store.con.execute(
            "SELECT coalesce(source_event_id, evidence_kind), json_extract_string(details_json, '$.disposition') "
            "FROM delisting_evidence"
        ).fetchall()
    )
    assert dispositions == {
        "LIVE-25": "issuer_notice_security_continues_trading",
        "SHELL-15": "notice_without_price_series",
        "LATE-15": "notice_without_price_series",
        "NOID": "notice_without_price_series",
        "archive_last_trade": "archive_cessation",
    }
    event = tmp_store.con.execute(
        "SELECT security_id, delist_reason, inferred_from_absence FROM delisting_events"
    ).fetchall()
    assert event == [("SEC-GONE", "unknown", True)]  # the late Form 15 explains nothing


def test_notices_outside_the_archive_are_ranked_by_extrapolated_sessions(tmp_store):
    # The archive starts Mon 2024-01-01; SEC-GONE's series ends at its 10th session. A Form 25
    # from 2003 must not corroborate that cessation merely because it predates the archive;
    # one filed the Friday before the first session may.
    _seed_two_securities(tmp_store)
    _file(tmp_store, "SEC-GONE", "0000000002", "OLD-25", "2003-06-02", "25")
    _file(tmp_store, "SEC-GONE", "0000000002", "EDGE-25", "2023-12-29", "25")
    assert _build(tmp_store) == 1
    rows = dict(
        tmp_store.con.execute(
            "SELECT source_event_id, json_extract_string(details_json, '$.disposition') "
            "FROM delisting_evidence WHERE evidence_kind = 'sec_form_25'"
        ).fetchall()
    )
    assert rows == {
        "OLD-25": "issuer_notice_security_continues_trading",
        "EDGE-25": "corroborated_cessation",
    }
    assert tmp_store.con.execute(
        "SELECT listing_status_source, source_event_id, inferred_from_absence FROM delisting_events"
    ).fetchall() == [("sec_form_25", "EDGE-25", False)]
