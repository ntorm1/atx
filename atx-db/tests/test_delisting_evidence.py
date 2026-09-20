"""Tier1-S4 T3: public delisting evidence streams and their precedence fold."""

from __future__ import annotations

import datetime as dt


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
    assert row[2] == dt.date.fromisoformat(SESSIONS[9])
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


def test_a_bare_form_25_with_no_merger_evidence_is_voluntary(tmp_store):
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
    assert reason == "voluntary"


def test_nasdaq_delete_and_form_15_are_captured(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
        "effective_date, as_of_date, source_url) VALUES "
        "('EV-1','GONE','SEC-GONE','D',DATE '2024-01-17',DATE '2024-01-17','file://t')"
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
    assert kinds["nasdaq_delete"] == ("exchange_delist", 2)
    assert kinds["sec_form_15"] == ("voluntary", 3)
    assert kinds["archive_last_trade"] == ("unknown", 4)


def test_bankruptcy_overlay_upgrades_the_reason(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_symbol_directory (directory, symbol, security_name, exchange, etf, "
        "test_issue, financial_status, as_of_date, source_url) VALUES "
        "('nasdaqlisted','GONE','Gone Corp - Common Stock','NASDAQ',false,false,'Q',"
        "DATE '2024-01-10','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    row = tmp_store.con.execute("SELECT reason_category, reason_confidence FROM delisting_evidence").fetchone()
    assert row[0] == "bankruptcy"
    assert row[1] == "high"


def test_the_fold_keeps_the_highest_precedence_evidence(tmp_store):
    from atx_db.delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
        "effective_date, as_of_date, source_url) VALUES "
        "('EV-1','GONE','SEC-GONE','D',DATE '2024-01-12',DATE '2024-01-12','file://t')"
    )
    options = DelistingEvidenceOptions(run_id="t")
    refresh_delisting_evidence(tmp_store, options)
    events = fold_evidence_into_delisting_events(tmp_store, options)
    # SEC-GONE's nasdaq_delete (rank 2) and archive_last_trade (rank 4) evidence both land
    # on 2024-01-12 (its last bar), so there is exactly one distinct (security_id,
    # delist_date) pair here and the fold must collapse it to one row, keeping the winner.
    assert events == 1
    winner = tmp_store.con.execute(
        "SELECT listing_status_source, delist_code, delist_reason, inferred_from_absence "
        "FROM delisting_events WHERE source = ? AND delist_date = DATE '2024-01-12'",
        [options.event_source],
    ).fetchone()
    assert winner[0] == "nasdaq_delete"
    assert winner[1] == "NASDAQ_DELETE"
    assert winner[2] == "exchange_delist"
    assert bool(winner[3]) is False


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
