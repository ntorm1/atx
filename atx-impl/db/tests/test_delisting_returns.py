"""PF4-S4 S4-0: observed DLRET terminal-return catalog + DLSTCD reconciliation.

Helpers below are copied (not imported) from db/tests/test_delisting.py per the task brief,
then lightly parameterized (security_id/symbol keyword-only, defaulting to the original
hardcoded values) so this module's own multi-security match/vendor_only/warehouse_only test
can seed more than one security without changing behavior for the two verbatim TDD tests.
"""

from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


SECURITY_ID = "SEC-TEST-DELIST"


def _date_value(value) -> dt.date:
    return value.date() if hasattr(value, "date") else value


def _seed_security(
    tmp_store,
    *,
    security_id: str = SECURITY_ID,
    symbol: str = "DLS",
    name: str = "Delist Test Corp.",
) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO securities (
            security_id,
            issuer_id,
            primary_symbol,
            name,
            source
        )
        VALUES (?, ?, ?, ?, ?)
        """,
        [security_id, security_id, symbol, name, "test"],
    )


def _insert_vendor_identifier(
    tmp_store,
    *,
    vendor_id: str,
    security_id: str = SECURITY_ID,
    available_at: dt.datetime = dt.datetime(2024, 4, 2, 12, 0),
) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO security_identifier_history (
            security_id,
            id_type,
            id_value,
            valid_from,
            valid_to,
            as_of_date,
            available_at,
            source,
            run_id
        )
        VALUES (?, 'PERMNO', ?, DATE '2020-01-01', NULL, DATE '2024-04-01', ?, 'fixture_permno', 'id-run')
        """,
        [security_id, vendor_id, available_at],
    )


def _insert_listing_status(
    tmp_store,
    *,
    listing_status_id: str,
    status: str,
    valid_from: dt.date,
    valid_to: dt.date | None = None,
    as_of_date: dt.date | None = None,
    available_at: dt.datetime | None = None,
    last_evidence_as_of_date: dt.date | None = None,
    last_evidence_at: dt.datetime | None = None,
    method: str = "trading_system_action_checkpoint",
    security_id: str = SECURITY_ID,
    symbol: str = "DLS",
) -> None:
    as_of_date = as_of_date or valid_from
    available_at = available_at or dt.datetime.combine(as_of_date, dt.time(22, 0))
    last_evidence_as_of_date = last_evidence_as_of_date or as_of_date
    last_evidence_at = last_evidence_at or available_at
    tmp_store.con.execute(
        """
        INSERT INTO listing_status_intervals (
            listing_status_id,
            security_id,
            symbol,
            listing_venue_code,
            listing_venue_name,
            listing_exchange_code,
            status,
            valid_from,
            valid_to,
            as_of_date,
            available_at,
            last_evidence_as_of_date,
            last_evidence_at,
            source,
            evidence_source,
            evidence_source_table,
            source_event_id,
            source_snapshot_directory,
            source_url,
            method,
            details_json,
            run_id
        )
        VALUES (
            ?, ?, ?, 'Q', 'NASDAQ Global Select Market', 'NASDAQ',
            ?, ?, ?, ?, ?, ?, ?,
            'test_listing_status',
            'nasdaq_trading_system_adds_deletes',
            'nasdaq_listing_events',
            'event-1',
            NULL,
            'https://example.test/listing-events',
            ?,
            '{}',
            'listing-run'
        )
        """,
        [
            listing_status_id,
            security_id,
            symbol,
            status,
            valid_from,
            valid_to,
            as_of_date,
            available_at,
            last_evidence_as_of_date,
            last_evidence_at,
            method,
        ],
    )


def test_injected_dlret_file_populates_terminal_return_per_delisted_security_day(tmp_store, tmp_path):
    from db.delisting import (
        DelistingReturnObservationOptions,
        load_delisting_return_observations,
        refresh_delisting_events,
        refresh_delisting_terminal_returns,
    )
    _seed_security(tmp_store)
    _insert_listing_status(tmp_store, listing_status_id="ls-obs",
                           status="inactive", valid_from=dt.date(2024, 4, 1),
                           available_at=dt.datetime(2024, 4, 2, 12, 0))
    refresh_delisting_events(tmp_store)
    csv_path = tmp_path / "crsp_dlret.csv"
    csv_path.write_text(
        "PERMNO,security_id,TICKER,DLSTDT,DLSTCD,DLRET,DLRETX,available_at\n"
        "12345,SEC-TEST-DELIST,DLS,2024-04-01,233,-0.640000,-0.640000,2024-04-03T12:00:00\n",
        encoding="utf-8")
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))
    rows = refresh_delisting_terminal_returns(tmp_store)
    assert rows == 1
    term = tmp_store.con.execute(
        "SELECT security_id, delist_date, terminal_return, terminal_return_source, crsp_dlstcd "
        "FROM delisting_terminal_returns").fetchone()
    assert term[0] == SECURITY_ID
    assert _date_value(term[1]) == dt.date(2024, 4, 1)
    assert term[2] == pytest.approx(-0.64)
    assert term[3] == "observed"
    assert term[4] == 233


def test_dlstcd_reconciliation_flags_vendor_vs_warehouse_mismatch(tmp_store, tmp_path):
    from db.delisting import (
        DelistingReturnObservationOptions, load_delisting_return_observations,
        reconcile_delisting_codes, refresh_delisting_events, refresh_delisting_terminal_returns)
    _seed_security(tmp_store)
    # warehouse public proxy detects a generic NASDAQ_DELETE (family: exchange/dropped) ...
    _insert_listing_status(tmp_store, listing_status_id="ls-recon", status="inactive",
                           valid_from=dt.date(2024, 4, 1), available_at=dt.datetime(2024, 4, 2, 12, 0))
    refresh_delisting_events(tmp_store)
    # ... but the vendor DLSTCD 233 is a MERGER (2xx family) -> mismatch
    csv_path = tmp_path / "crsp_dlret_merger.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        "12345,SEC-TEST-DELIST,2024-04-01,233,0.052000,2024-04-03T12:00:00\n", encoding="utf-8")
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))
    refresh_delisting_terminal_returns(tmp_store)
    reconcile_delisting_codes(tmp_store)
    status, family, reason = tmp_store.con.execute(
        "SELECT reconciliation_status, vendor_dlstcd_family, warehouse_reason_category "
        "FROM delisting_code_reconciliation").fetchone()
    assert status == "mismatch"
    assert family == "merger"
    assert reason in ("exchange_delete", "DELISTED_OR_TRANSFERRED_UNKNOWN", None)


def test_delisting_terminal_return_catalog_is_complete(tmp_store) -> None:
    # Schema-as-contract: every table migration 0185 creates must carry a table_catalog row
    # and at least one field_catalog row, mirroring test_signal_eval.py's
    # test_signal_eval_tables_are_catalogued for the PF4-S1 tables.
    tables = (
        "delisting_terminal_returns",
        "delisting_code_reconciliation",
        "terminal_return_policy_dim",
    )
    catalogued = {
        row[0]
        for row in tmp_store.con.execute(
            f"SELECT table_name FROM table_catalog WHERE table_name IN ({','.join('?' for _ in tables)})",
            list(tables),
        ).fetchall()
    }
    assert catalogued == set(tables), f"missing from table_catalog: {set(tables) - catalogued}"

    field_counts = tmp_store.con.execute(
        f"""
        SELECT table_name, count(*) AS n_fields
        FROM field_catalog
        WHERE table_name IN ({','.join('?' for _ in tables)})
        GROUP BY table_name
        """,
        list(tables),
    ).fetchall()
    field_counts_by_table = dict(field_counts)
    for table in tables:
        assert field_counts_by_table.get(table, 0) >= 1, f"{table} has no field_catalog rows"


def test_no_dlret_file_leaves_terminal_returns_empty(tmp_store) -> None:
    # Empty-by-default: with nothing injected, the materializer writes zero rows and the
    # table stays empty -- no imputed/invented terminal return ever appears.
    from db.delisting import refresh_delisting_terminal_returns

    rows = refresh_delisting_terminal_returns(tmp_store)
    assert rows == 0
    count = tmp_store.con.execute("SELECT count(*) FROM delisting_terminal_returns").fetchone()[0]
    assert count == 0


def test_dlstcd_reconciliation_reports_match_vendor_only_and_warehouse_only(tmp_store, tmp_path) -> None:
    from db.delisting import (
        DelistingReturnObservationOptions, load_delisting_return_observations,
        reconcile_delisting_codes, refresh_delisting_events, refresh_delisting_terminal_returns)

    # A: public evidence + vendor observation agree (NASDAQ_DELETE proxy vs. a vendor "dropped"
    # 5xx DLSTCD) -> match.
    _seed_security(tmp_store, security_id="SEC-TEST-MATCH", symbol="MTA")
    _insert_listing_status(
        tmp_store, listing_status_id="ls-match", status="inactive",
        valid_from=dt.date(2024, 5, 1), available_at=dt.datetime(2024, 5, 2, 12, 0),
        security_id="SEC-TEST-MATCH", symbol="MTA",
    )

    # B: vendor observation only, no public listing-status evidence at all -> vendor_only.
    _seed_security(tmp_store, security_id="SEC-TEST-VENDOR-ONLY", symbol="VOA")

    # C: public evidence only, no vendor observation ever lands -> warehouse_only.
    _seed_security(tmp_store, security_id="SEC-TEST-WH-ONLY", symbol="WHA")
    _insert_listing_status(
        tmp_store, listing_status_id="ls-wh-only", status="inactive",
        valid_from=dt.date(2024, 5, 3), available_at=dt.datetime(2024, 5, 4, 12, 0),
        security_id="SEC-TEST-WH-ONLY", symbol="WHA",
    )

    refresh_delisting_events(tmp_store)

    csv_path = tmp_path / "crsp_dlret_multi.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        "11111,SEC-TEST-MATCH,2024-05-01,520,-0.010000,2024-05-03T12:00:00\n"
        "22222,SEC-TEST-VENDOR-ONLY,2024-05-05,384,0.015000,2024-05-06T12:00:00\n",
        encoding="utf-8",
    )
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))

    refresh_delisting_terminal_returns(tmp_store)
    reconcile_delisting_codes(tmp_store)

    rows = {
        row[0]: (row[1], row[2])
        for row in tmp_store.con.execute(
            "SELECT security_id, reconciliation_status, vendor_dlstcd_family "
            "FROM delisting_code_reconciliation"
        ).fetchall()
    }
    assert rows["SEC-TEST-MATCH"] == ("match", "dropped")
    assert rows["SEC-TEST-VENDOR-ONLY"] == ("vendor_only", "exchange")
    assert rows["SEC-TEST-WH-ONLY"] == ("warehouse_only", None)
