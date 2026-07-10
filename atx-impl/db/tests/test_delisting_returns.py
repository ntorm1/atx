"""PF4-S4 S4-0: observed DLRET terminal-return catalog + DLSTCD reconciliation.

Helpers below are copied (not imported) from db/tests/test_delisting.py per the task brief,
then lightly parameterized (security_id/symbol keyword-only, defaulting to the original
hardcoded values) so this module's own multi-security match/vendor_only/warehouse_only test
can seed more than one security without changing behavior for the two verbatim TDD tests.
"""

from __future__ import annotations

import datetime as dt
import warnings

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
        "SELECT security_id, delist_date, terminal_return, terminal_return_source, crsp_dlstcd, "
        "available_at "
        "FROM delisting_terminal_returns").fetchone()
    assert term[0] == SECURITY_ID
    assert _date_value(term[1]) == dt.date(2024, 4, 1)
    assert term[2] == pytest.approx(-0.64)
    assert term[3] == "observed"
    assert term[4] == 233
    # No-lookahead invariant: available_at is the observation's delisting-confirmation
    # timestamp (the DLRET row's own available_at), never the delist event date and never
    # now(). A regression stamping it from delist_date or now() must fail this test.
    assert term[5] == dt.datetime(2024, 4, 3, 12, 0, 0)
    assert term[5] > dt.datetime(2024, 4, 1, 0, 0, 0)  # strictly postdates delist_date
    assert term[5] > dt.datetime(2024, 4, 2, 12, 0, 0)  # strictly postdates the listing-status event's available_at


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


def test_reconciliation_id_is_unique_when_two_delisting_events_share_a_security_day(tmp_store) -> None:
    # reconciliation_id is delisting_code_reconciliation's PRIMARY KEY. A listing-status
    # delete and a snapshot-absence event landing on the same (security_id, symbol,
    # delist_date) are two distinct delisting_events rows (distinct delisting_event_id) that
    # must not collapse to the same reconciliation_id -- that would raise a PK violation on
    # insert (or, worse, silently lose a row).
    from db.delisting import DelistingEventOptions, reconcile_delisting_codes, refresh_delisting_events

    _seed_security(tmp_store, security_id="SEC-TEST-DUP-EVENT", symbol="DUP")
    # Row A: an exchange delete on 2024-06-01 -> NASDAQ_DELETE event.
    _insert_listing_status(
        tmp_store, listing_status_id="ls-dup-delete", status="inactive",
        valid_from=dt.date(2024, 6, 1), available_at=dt.datetime(2024, 6, 1, 22, 0),
        security_id="SEC-TEST-DUP-EVENT", symbol="DUP",
    )
    # Row B: the same security's last "active" snapshot interval ends the same day ->
    # SNAPSHOT_ABSENCE event, same (security_id, symbol, delist_date), different delist_code
    # and different delisting_event_id.
    _insert_listing_status(
        tmp_store, listing_status_id="ls-dup-absence", status="active",
        valid_from=dt.date(2020, 1, 1), valid_to=dt.date(2024, 6, 1),
        last_evidence_as_of_date=dt.date(2024, 6, 1), last_evidence_at=dt.datetime(2024, 6, 1, 23, 0),
        security_id="SEC-TEST-DUP-EVENT", symbol="DUP",
    )

    refresh_delisting_events(tmp_store, DelistingEventOptions(include_snapshot_absence=True))

    event_ids = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT delisting_event_id FROM delisting_events WHERE security_id = ?",
            ["SEC-TEST-DUP-EVENT"],
        ).fetchall()
    }
    assert len(event_ids) == 2  # sanity: two distinct events really landed for the same security/day

    # Must not raise a PRIMARY KEY violation on insert.
    reconcile_delisting_codes(tmp_store)

    reconciliation_ids = [
        row[0]
        for row in tmp_store.con.execute(
            "SELECT reconciliation_id FROM delisting_code_reconciliation WHERE security_id = ?",
            ["SEC-TEST-DUP-EVENT"],
        ).fetchall()
    ]
    assert len(reconciliation_ids) == 2  # no silent row loss
    assert len(set(reconciliation_ids)) == 2  # no PK collision


def test_refresh_and_reconcile_are_idempotent_on_rerun(tmp_store, tmp_path) -> None:
    # Both materializers replace-by-source. Calling either twice on unchanged inputs must
    # leave the table holding exactly the rows it held after the first call -- same row
    # count, same primary keys -- not a doubled set.
    from db.delisting import (
        DelistingReturnObservationOptions, load_delisting_return_observations,
        reconcile_delisting_codes, refresh_delisting_events, refresh_delisting_terminal_returns)

    _seed_security(tmp_store)
    _insert_listing_status(tmp_store, listing_status_id="ls-idem", status="inactive",
                           valid_from=dt.date(2024, 4, 1), available_at=dt.datetime(2024, 4, 2, 12, 0))
    refresh_delisting_events(tmp_store)
    csv_path = tmp_path / "crsp_dlret_idem.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        "12345,SEC-TEST-DELIST,2024-04-01,233,-0.640000,2024-04-03T12:00:00\n",
        encoding="utf-8")
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))

    refresh_delisting_terminal_returns(tmp_store)
    reconcile_delisting_codes(tmp_store)

    def _pks(table: str, pk: str) -> list:
        return [row[0] for row in tmp_store.con.execute(f"SELECT {pk} FROM {table} ORDER BY {pk}").fetchall()]

    terminal_pks_1 = _pks("delisting_terminal_returns", "terminal_return_id")
    reconciliation_pks_1 = _pks("delisting_code_reconciliation", "reconciliation_id")
    assert terminal_pks_1  # sanity: the first call actually landed rows
    assert reconciliation_pks_1

    refresh_delisting_terminal_returns(tmp_store)
    reconcile_delisting_codes(tmp_store)

    assert _pks("delisting_terminal_returns", "terminal_return_id") == terminal_pks_1
    assert _pks("delisting_code_reconciliation", "reconciliation_id") == reconciliation_pks_1


def test_compute_delisting_terminal_returns_is_shuffle_invariant(tmp_store, tmp_path) -> None:
    # Sprint determinism clause D: row order of the input frames must not affect the output.
    from db.delisting import (
        DelistingReturnObservationOptions, compute_delisting_terminal_returns,
        load_delisting_return_observations, refresh_delisting_events)

    _seed_security(tmp_store, security_id="SEC-SHUF-A", symbol="SHA")
    _seed_security(tmp_store, security_id="SEC-SHUF-B", symbol="SHB")
    _insert_listing_status(tmp_store, listing_status_id="ls-shuf-a", status="inactive",
                           valid_from=dt.date(2024, 7, 1), available_at=dt.datetime(2024, 7, 2, 12, 0),
                           security_id="SEC-SHUF-A", symbol="SHA")
    _insert_listing_status(tmp_store, listing_status_id="ls-shuf-b", status="inactive",
                           valid_from=dt.date(2024, 7, 3), available_at=dt.datetime(2024, 7, 4, 12, 0),
                           security_id="SEC-SHUF-B", symbol="SHB")
    refresh_delisting_events(tmp_store)

    csv_path = tmp_path / "crsp_dlret_shuffle.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        # Two competing observations for SEC-SHUF-A on the same delist_date -- the
        # later-available_at row must win regardless of row order.
        "31111,SEC-SHUF-A,2024-07-01,233,-0.100000,2024-07-02T09:00:00\n"
        "31112,SEC-SHUF-A,2024-07-01,233,-0.200000,2024-07-03T09:00:00\n"
        "32222,SEC-SHUF-B,2024-07-03,384,0.030000,2024-07-05T09:00:00\n",
        encoding="utf-8")
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))

    observations = tmp_store.con.execute(
        """
        SELECT
            delisting_return_observation_id, source, security_id, symbol, delist_date,
            as_of_date, available_at, source_loaded_at, crsp_dlstcd, delisting_return,
            delisting_return_ex_div, return_basis, successor_security_id
        FROM delisting_return_observations
        """
    ).df()
    events = tmp_store.con.execute(
        "SELECT delisting_event_id, security_id, symbol, delist_date, as_of_date, available_at, delist_code "
        "FROM delisting_events"
    ).df()
    policy_dim = tmp_store.con.execute(
        "SELECT policy_code, corporate_action_type, terminal_return_basis, combine_successor, "
        "default_return, is_observed_required, description FROM terminal_return_policy_dim"
    ).df()
    assert len(observations) == 3  # sanity: a real shuffle needs more than one row order
    assert len(events) == 2

    baseline = compute_delisting_terminal_returns(observations, events, policy_dim).reset_index(drop=True)

    # Deterministic reversal, not an RNG shuffle: with as few as 2 input rows a fixed-seed
    # `sample(frac=1, random_state=...)` can land on the identity permutation by chance (a
    # 50/50 coin flip at n=2), which would silently turn this into a no-op property test.
    # Reversal is a fixed, seedless reordering that is guaranteed to differ from the original
    # for any frame with 2+ rows.
    shuffled_observations = observations.iloc[::-1].reset_index(drop=True)
    shuffled_events = events.iloc[::-1].reset_index(drop=True)
    assert list(shuffled_observations["delisting_return_observation_id"]) != list(
        observations["delisting_return_observation_id"]
    )  # sanity: the reversal actually reordered the rows
    shuffled = compute_delisting_terminal_returns(
        shuffled_observations, shuffled_events, policy_dim
    ).reset_index(drop=True)

    pd.testing.assert_frame_equal(baseline, shuffled)


def test_compute_delisting_code_reconciliation_is_shuffle_invariant(tmp_store, tmp_path) -> None:
    # Sprint determinism clause D: row order of the input frames must not affect the output.
    # Three securities with distinct (security_id, delist_date, symbol) combos -- no tied sort
    # keys -- so the assertion isolates order-*independence* rather than a merge tie-break.
    from db.delisting import (
        DelistingReturnObservationOptions, compute_delisting_code_reconciliation,
        load_delisting_return_observations, refresh_delisting_events, refresh_delisting_terminal_returns)

    _seed_security(tmp_store, security_id="SEC-SHUF-MATCH", symbol="SFM")
    _insert_listing_status(
        tmp_store, listing_status_id="ls-shuf-match", status="inactive",
        valid_from=dt.date(2024, 8, 1), available_at=dt.datetime(2024, 8, 2, 12, 0),
        security_id="SEC-SHUF-MATCH", symbol="SFM",
    )
    _seed_security(tmp_store, security_id="SEC-SHUF-VENDOR", symbol="SFV")
    _seed_security(tmp_store, security_id="SEC-SHUF-WH", symbol="SFW")
    _insert_listing_status(
        tmp_store, listing_status_id="ls-shuf-wh", status="inactive",
        valid_from=dt.date(2024, 8, 3), available_at=dt.datetime(2024, 8, 4, 12, 0),
        security_id="SEC-SHUF-WH", symbol="SFW",
    )
    refresh_delisting_events(tmp_store)

    csv_path = tmp_path / "crsp_dlret_shuffle_recon.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        "41111,SEC-SHUF-MATCH,2024-08-01,520,-0.010000,2024-08-03T12:00:00\n"
        "42222,SEC-SHUF-VENDOR,2024-08-05,384,0.015000,2024-08-06T12:00:00\n",
        encoding="utf-8")
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))
    refresh_delisting_terminal_returns(tmp_store)

    events = tmp_store.con.execute(
        "SELECT delisting_event_id, security_id, symbol, delist_date, as_of_date, available_at, delist_code "
        "FROM delisting_events"
    ).df()
    terminal_returns = tmp_store.con.execute(
        "SELECT return_observation_id, security_id, symbol, delist_date, as_of_date, available_at, crsp_dlstcd "
        "FROM delisting_terminal_returns"
    ).df()
    code_dim = tmp_store.con.execute("SELECT delist_code, reason_category FROM delist_code_dim").df()
    assert len(events) == 2  # sanity: a real shuffle needs more than one row order (MATCH, WH -- VENDOR has no event)
    assert len(code_dim) >= 2

    baseline = compute_delisting_code_reconciliation(events, terminal_returns, code_dim).reset_index(drop=True)

    # Deterministic reversal, not an RNG shuffle: with as few as 2 input rows a fixed-seed
    # `sample(frac=1, random_state=...)` can land on the identity permutation by chance (a
    # 50/50 coin flip at n=2), which would silently turn this into a no-op property test.
    # Reversal is a fixed, seedless reordering that is guaranteed to differ from the original
    # for any frame with 2+ rows.
    shuffled_events = events.iloc[::-1].reset_index(drop=True)
    shuffled_terminal_returns = terminal_returns.iloc[::-1].reset_index(drop=True)
    shuffled_code_dim = code_dim.iloc[::-1].reset_index(drop=True)
    assert list(shuffled_events["delisting_event_id"]) != list(events["delisting_event_id"])  # sanity: reordered
    shuffled = compute_delisting_code_reconciliation(
        shuffled_events, shuffled_terminal_returns, shuffled_code_dim
    ).reset_index(drop=True)

    pd.testing.assert_frame_equal(baseline, shuffled)


def test_compute_delisting_code_reconciliation_sort_is_total_order_with_colliding_security_day() -> None:
    # S4-0-fix2 (carried-over Minor from S4-0's review): the final sort keyed on
    # (security_id, delist_date, symbol) is not a total order once two delisting_events share
    # that triple -- the duplicate-event case reconciliation_id's own hardening legitimized
    # (see test_reconciliation_id_is_unique_when_two_delisting_events_share_a_security_day).
    # kind="mergesort" is stable, so tied rows keep their *input* relative order -- meaning the
    # emitted row *order* (not the row *set*) flips depending on input order, violating the
    # sprint's determinism clause D ("same inputs + params -> byte-identical rows"). The prior
    # shuffle-invariance test above deliberately used distinct (security_id, delist_date, symbol)
    # combos and therefore cannot catch this. This test uses two events colliding on that triple
    # (distinct delisting_event_id) with no terminal-return rows at all, so the only thing that
    # can make output order differ between "forward" and "reversed" input is exactly this bug.
    from db.delisting import compute_delisting_code_reconciliation

    events = pd.DataFrame(
        [
            {
                "delisting_event_id": "evt-dup-delete",
                "security_id": "SEC-DUP-SORT",
                "symbol": "DUP",
                "delist_date": dt.date(2024, 6, 1),
                "as_of_date": dt.date(2024, 6, 1),
                "available_at": dt.datetime(2024, 6, 1, 22, 0),
                "delist_code": "NASDAQ_DELETE",
            },
            {
                "delisting_event_id": "evt-dup-absence",
                "security_id": "SEC-DUP-SORT",
                "symbol": "DUP",
                "delist_date": dt.date(2024, 6, 1),
                "as_of_date": dt.date(2024, 6, 1),
                "available_at": dt.datetime(2024, 6, 1, 23, 0),
                "delist_code": "SNAPSHOT_ABSENCE",
            },
        ]
    )
    terminal_returns = pd.DataFrame(
        columns=[
            "return_observation_id",
            "security_id",
            "symbol",
            "delist_date",
            "as_of_date",
            "available_at",
            "crsp_dlstcd",
        ]
    )
    code_dim = pd.DataFrame(
        [
            {"delist_code": "NASDAQ_DELETE", "reason_category": "DELISTED_OR_TRANSFERRED_UNKNOWN"},
            {"delist_code": "SNAPSHOT_ABSENCE", "reason_category": "ABSENT_FROM_PUBLIC_DIRECTORY"},
        ]
    )

    baseline = compute_delisting_code_reconciliation(events, terminal_returns, code_dim).reset_index(drop=True)

    reversed_events = events.iloc[::-1].reset_index(drop=True)
    assert list(reversed_events["delisting_event_id"]) != list(events["delisting_event_id"])  # sanity: reordered
    reversed_result = compute_delisting_code_reconciliation(
        reversed_events, terminal_returns, code_dim
    ).reset_index(drop=True)

    # Row *set* is stable regardless (this must hold both before and after the fix)...
    assert set(baseline["delisting_event_id"]) == set(reversed_result["delisting_event_id"]) == {
        "evt-dup-delete",
        "evt-dup-absence",
    }
    # ...but row *order* must also be byte-identical -- this is what the missing total-order key
    # breaks: with only (security_id, delist_date, symbol) as the sort key, both rows tie, the
    # stable sort preserves input order, and the two calls above fed opposite input orders.
    pd.testing.assert_frame_equal(baseline, reversed_result)


# ---------------------------------------------------------------------------
# PF4-S4 S4-1: deterministic spinoff/merger terminal-return policy.
#
# The two tests immediately below are verbatim from the plan (task-s4-1-plan.md); everything
# after them is this task's own, each written to discriminate (fail if its behaviour is
# reverted).
# ---------------------------------------------------------------------------


def test_cash_merger_policy_terminal_return_is_deterministic(tmp_store):
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim

    policy = load_terminal_return_policy_dim(tmp_store)  # reads the seeded dim
    events = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "symbol": "DLS",
                "delist_date": dt.date(2024, 4, 1),
                "as_of_date": dt.date(2024, 4, 1),
                "available_at": dt.datetime(2024, 4, 2, 12, 0),
                "corporate_action_type": "merger",
            }
        ]
    )
    actions = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "ex_date": dt.date(2024, 4, 1),
                "action_type": "cash_merger",
                "cash_amount": 21.0,
                "last_pre_delist_adjusted_close": 20.0,
                "available_at": dt.datetime(2024, 4, 1, 22, 0),
            }
        ]
    )
    out = apply_terminal_return_policy(events, actions, policy)
    row = out.iloc[0]
    assert row["terminal_return"] == pytest.approx(0.05)  # 21/20 - 1
    assert row["terminal_return_source"] == "policy"
    assert row["terminal_return_policy"] == "merger_cash"


def test_exchange_delete_without_observation_yields_no_policy_return(tmp_store):
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim

    policy = load_terminal_return_policy_dim(tmp_store)
    events = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "symbol": "DLS",
                "delist_date": dt.date(2024, 4, 1),
                "as_of_date": dt.date(2024, 4, 1),
                "available_at": dt.datetime(2024, 4, 2, 12, 0),
                "corporate_action_type": "exchange_delete",
            }
        ]
    )
    out = apply_terminal_return_policy(events, pd.DataFrame(), policy)
    assert out.empty  # DQC flags the uncovered name; policy never invents -30%


def test_terminal_return_policy_dim_is_seeded_and_catalogued_idempotently(tmp_store) -> None:
    # R1: the dimension is policy-as-data, seeded inside migration 0185's own body (not by a
    # runtime seed_terminal_return_policy_dim() helper). Re-applying 0185 must be a no-op row
    # count -- INSERT OR REPLACE, not INSERT -- proving the seed is idempotent.
    from db.delisting import TERMINAL_RETURN_POLICY_ROWS
    from db.migrations.registry import MIGRATIONS

    expected_codes = {row[0] for row in TERMINAL_RETURN_POLICY_ROWS}
    assert len(expected_codes) == 6

    rows = tmp_store.con.execute(
        "SELECT policy_code FROM terminal_return_policy_dim ORDER BY policy_code"
    ).fetchall()
    assert {row[0] for row in rows} == expected_codes
    assert len(rows) == 6

    migration_0185 = next(m for m in MIGRATIONS if m.version == 185)
    migration_0185.up(tmp_store.con)

    rows_after = tmp_store.con.execute(
        "SELECT policy_code FROM terminal_return_policy_dim ORDER BY policy_code"
    ).fetchall()
    assert len(rows_after) == 6
    assert {row[0] for row in rows_after} == expected_codes


def test_observed_terminal_return_wins_over_policy_when_both_exist(tmp_store, tmp_path) -> None:
    # R4: compute_delisting_terminal_returns must union observed + policy rows, but observed
    # always wins -- a delist with BOTH a real DLRET and a matching cash-merger corporate action
    # must still emit exactly one terminal row, tagged 'observed', never 'policy'.
    from db.delisting import (
        DelistingReturnObservationOptions,
        compute_delisting_terminal_returns,
        load_delisting_return_observations,
        load_terminal_return_policy_dim,
    )

    _seed_security(tmp_store, security_id="SEC-OBS-WINS", symbol="OWS")
    policy = load_terminal_return_policy_dim(tmp_store)

    csv_path = tmp_path / "crsp_dlret_obs_wins.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        "51111,SEC-OBS-WINS,2024-04-01,233,-0.600000,2024-04-03T12:00:00\n",
        encoding="utf-8",
    )
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE")
    )
    observations = tmp_store.con.execute(
        """
        SELECT
            delisting_return_observation_id, source, security_id, symbol, delist_date,
            as_of_date, available_at, source_loaded_at, crsp_dlstcd, delisting_return,
            delisting_return_ex_div, return_basis, successor_security_id
        FROM delisting_return_observations
        """
    ).df()

    # events deliberately carries no corporate_action_type -- delisting_events never has that
    # column; compute_delisting_terminal_returns must derive it from corporate_actions itself.
    events = pd.DataFrame(
        [
            {
                "security_id": "SEC-OBS-WINS",
                "symbol": "OWS",
                "delist_date": dt.date(2024, 4, 1),
                "as_of_date": dt.date(2024, 4, 1),
                "available_at": dt.datetime(2024, 4, 2, 12, 0),
            }
        ]
    )
    corporate_actions = pd.DataFrame(
        [
            {
                "security_id": "SEC-OBS-WINS",
                "action_type": "merger",
                "ex_date": dt.date(2024, 4, 1),
                "cash_amount": 21.0,
                "last_pre_delist_adjusted_close": 20.0,
                "available_at": dt.datetime(2024, 4, 1, 22, 0),
            }
        ]
    )

    result = compute_delisting_terminal_returns(
        observations, events, policy, corporate_actions=corporate_actions
    )
    assert len(result) == 1
    row = result.iloc[0]
    assert row["terminal_return_source"] == "observed"
    assert row["terminal_return"] == pytest.approx(-0.6)  # the observed DLRET, not 21/20-1=0.05


def test_uncovered_merger_gets_a_policy_row_alongside_an_unrelated_observed_delist(tmp_store, tmp_path) -> None:
    # R4's union, exercised with TWO securities in one call: one has a real observed DLRET and
    # no corporate action; the other has no observation but a matching cash-merger corporate
    # action. Both must land in the same compute_delisting_terminal_returns() output -- the
    # observed row untouched, and a brand-new policy row for the uncovered merger.
    from db.delisting import (
        DelistingReturnObservationOptions,
        compute_delisting_terminal_returns,
        load_delisting_return_observations,
        load_terminal_return_policy_dim,
    )

    _seed_security(tmp_store, security_id="SEC-OBS-ONLY", symbol="OBO")
    _seed_security(tmp_store, security_id="SEC-POLICY-ONLY", symbol="POA")
    policy = load_terminal_return_policy_dim(tmp_store)

    csv_path = tmp_path / "crsp_dlret_union.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        "61111,SEC-OBS-ONLY,2024-05-01,384,0.010000,2024-05-03T12:00:00\n",
        encoding="utf-8",
    )
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE")
    )
    observations = tmp_store.con.execute(
        """
        SELECT
            delisting_return_observation_id, source, security_id, symbol, delist_date,
            as_of_date, available_at, source_loaded_at, crsp_dlstcd, delisting_return,
            delisting_return_ex_div, return_basis, successor_security_id
        FROM delisting_return_observations
        """
    ).df()

    events = pd.DataFrame(
        [
            {
                "security_id": "SEC-OBS-ONLY",
                "symbol": "OBO",
                "delist_date": dt.date(2024, 5, 1),
                "as_of_date": dt.date(2024, 5, 1),
                "available_at": dt.datetime(2024, 5, 2, 12, 0),
            },
            {
                "security_id": "SEC-POLICY-ONLY",
                "symbol": "POA",
                "delist_date": dt.date(2024, 5, 5),
                "as_of_date": dt.date(2024, 5, 5),
                "available_at": dt.datetime(2024, 5, 6, 12, 0),
            },
        ]
    )
    corporate_actions = pd.DataFrame(
        [
            {
                "security_id": "SEC-POLICY-ONLY",
                "action_type": "merger",
                "ex_date": dt.date(2024, 5, 5),
                "cash_amount": 15.0,
                "last_pre_delist_adjusted_close": 10.0,
                "available_at": dt.datetime(2024, 5, 5, 22, 0),
            }
        ]
    )

    result = compute_delisting_terminal_returns(
        observations, events, policy, corporate_actions=corporate_actions
    ).set_index("security_id")

    assert set(result.index) == {"SEC-OBS-ONLY", "SEC-POLICY-ONLY"}
    assert result.loc["SEC-OBS-ONLY", "terminal_return_source"] == "observed"
    assert result.loc["SEC-OBS-ONLY", "terminal_return"] == pytest.approx(0.01)
    assert result.loc["SEC-POLICY-ONLY", "terminal_return_source"] == "policy"
    assert result.loc["SEC-POLICY-ONLY", "terminal_return_policy"] == "merger_cash"
    assert result.loc["SEC-POLICY-ONLY", "terminal_return"] == pytest.approx(0.5)  # 15/10 - 1


def test_stock_merger_falls_through_when_cash_consideration_is_absent(tmp_store) -> None:
    # R2's ordering rule: merger_cash and merger_stock both key on corporate_action_type=
    # "merger"; merger_cash is evaluated first (ascending policy_code) but must fail its own
    # required-input contract (no cash_amount) before merger_stock is even tried.
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim

    policy = load_terminal_return_policy_dim(tmp_store)
    events = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "symbol": "DLS",
                "delist_date": dt.date(2024, 4, 1),
                "as_of_date": dt.date(2024, 4, 1),
                "available_at": dt.datetime(2024, 4, 2, 12, 0),
                "corporate_action_type": "merger",
            }
        ]
    )
    actions = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "ex_date": dt.date(2024, 4, 1),
                "action_type": "stock_merger",
                # deliberately no cash_amount at all
                "successor_security_id": "SEC-SUCCESSOR",
                "successor_value": 25.0,
                "last_pre_delist_adjusted_close": 20.0,
                "available_at": dt.datetime(2024, 4, 1, 22, 0),
            }
        ]
    )
    out = apply_terminal_return_policy(events, actions, policy)
    assert len(out) == 1
    row = out.iloc[0]
    assert row["terminal_return_policy"] == "merger_stock"
    assert row["terminal_return"] == pytest.approx(0.25)  # 25/20 - 1
    assert row["successor_security_id"] == "SEC-SUCCESSOR"


@pytest.mark.parametrize("bad_close", [0.0, -5.0, None])
def test_policy_terminal_return_never_divides_by_nonpositive_last_pre_delist_close(tmp_store, bad_close) -> None:
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim

    policy = load_terminal_return_policy_dim(tmp_store)
    events = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "symbol": "DLS",
                "delist_date": dt.date(2024, 4, 1),
                "as_of_date": dt.date(2024, 4, 1),
                "available_at": dt.datetime(2024, 4, 2, 12, 0),
                "corporate_action_type": "merger",
            }
        ]
    )
    actions = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "ex_date": dt.date(2024, 4, 1),
                "action_type": "cash_merger",
                "cash_amount": 21.0,
                "last_pre_delist_adjusted_close": bad_close,
                "available_at": dt.datetime(2024, 4, 1, 22, 0),
            }
        ]
    )
    out = apply_terminal_return_policy(events, actions, policy)
    assert out.empty  # never inf/NaN, never a row: last_pre_delist_adjusted_close must be > 0


def test_policy_terminal_return_available_at_is_max_of_action_and_last_pre_delist(tmp_store) -> None:
    # R3: available_at = max(corporate_action.available_at, last_pre_delist_available_at), never
    # delist_date/as_of_date/now(). last_pre_delist_available_at is set strictly later here so
    # the assertion actually distinguishes "max" from "always the corporate action's own".
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim

    policy = load_terminal_return_policy_dim(tmp_store)
    events = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "symbol": "DLS",
                "delist_date": dt.date(2024, 4, 1),
                "as_of_date": dt.date(2024, 4, 1),
                "available_at": dt.datetime(2024, 4, 2, 12, 0),
                "corporate_action_type": "merger",
            }
        ]
    )
    actions = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "ex_date": dt.date(2024, 4, 1),
                "action_type": "cash_merger",
                "cash_amount": 21.0,
                "last_pre_delist_adjusted_close": 20.0,
                "available_at": dt.datetime(2024, 4, 1, 22, 0),
                "last_pre_delist_available_at": dt.datetime(2024, 4, 2, 9, 0),
            }
        ]
    )
    out = apply_terminal_return_policy(events, actions, policy)
    row = out.iloc[0]
    assert row["available_at"] == dt.datetime(2024, 4, 2, 9, 0)
    assert row["available_at"] > dt.datetime(2024, 4, 1, 0, 0, 0)  # strictly postdates delist_date


def test_apply_terminal_return_policy_is_shuffle_invariant(tmp_store) -> None:
    # Sprint determinism clause D: row order of the input frames must not affect the output.
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim

    policy = load_terminal_return_policy_dim(tmp_store)
    events = pd.DataFrame(
        [
            {
                "security_id": "SEC-SHUF-POL-A",
                "symbol": "SPA",
                "delist_date": dt.date(2024, 9, 1),
                "as_of_date": dt.date(2024, 9, 1),
                "available_at": dt.datetime(2024, 9, 2, 12, 0),
                "corporate_action_type": "merger",
            },
            {
                "security_id": "SEC-SHUF-POL-B",
                "symbol": "SPB",
                "delist_date": dt.date(2024, 9, 3),
                "as_of_date": dt.date(2024, 9, 3),
                "available_at": dt.datetime(2024, 9, 4, 12, 0),
                "corporate_action_type": "merger",
            },
        ]
    )
    actions = pd.DataFrame(
        [
            {
                "security_id": "SEC-SHUF-POL-A",
                "ex_date": dt.date(2024, 9, 1),
                "action_type": "cash_merger",
                "cash_amount": 12.0,
                "last_pre_delist_adjusted_close": 10.0,
                "available_at": dt.datetime(2024, 9, 1, 22, 0),
            },
            {
                "security_id": "SEC-SHUF-POL-B",
                "ex_date": dt.date(2024, 9, 3),
                "action_type": "cash_merger",
                "cash_amount": 30.0,
                "last_pre_delist_adjusted_close": 25.0,
                "available_at": dt.datetime(2024, 9, 3, 22, 0),
            },
        ]
    )

    baseline = apply_terminal_return_policy(events, actions, policy).reset_index(drop=True)

    shuffled_events = events.iloc[::-1].reset_index(drop=True)
    shuffled_actions = actions.iloc[::-1].reset_index(drop=True)
    assert list(shuffled_events["security_id"]) != list(events["security_id"])  # sanity: reordered
    shuffled = apply_terminal_return_policy(shuffled_events, shuffled_actions, policy).reset_index(drop=True)

    pd.testing.assert_frame_equal(baseline, shuffled)


# ---------------------------------------------------------------------------
# PF4-S4 S4-1-fix: two carried Minors folded in before S4-2.
# ---------------------------------------------------------------------------


def test_compute_terminal_returns_union_is_futurewarning_clean(tmp_store) -> None:
    # M1: unioning an observed row that carries a REAL terminal_return_ex_div (DLRETX, float64)
    # with an uncovered-merger policy row (all-NA object ex_div) used to raise the pandas 2.2
    # "concatenation with empty or all-NA entries is deprecated" FutureWarning at the concat of
    # the observed and policy frames. The union must be warning-clean (the frame is inserted into
    # DuckDB downstream). Discriminates: reverting to a bare pd.concat re-raises here.
    from db.delisting import compute_delisting_terminal_returns, load_terminal_return_policy_dim

    policy = load_terminal_return_policy_dim(tmp_store)
    observations = pd.DataFrame(
        [
            {
                "delisting_return_observation_id": "obs-m1",
                "source": "crsp_sample",
                "security_id": "SEC-M1-OBS",
                "symbol": "M1O",
                "delist_date": dt.date(2024, 5, 1),
                "as_of_date": dt.date(2024, 5, 1),
                "available_at": dt.datetime(2024, 5, 3, 12, 0),
                "source_loaded_at": dt.datetime(2024, 5, 3, 12, 0),
                "crsp_dlstcd": 233,
                "delisting_return": -0.60,
                "delisting_return_ex_div": -0.58,  # a REAL DLRETX value -> float64 column
                "return_basis": None,
                "successor_security_id": None,
            }
        ]
    )
    events = pd.DataFrame(
        [
            {
                "security_id": "SEC-M1-OBS",
                "symbol": "M1O",
                "delist_date": dt.date(2024, 5, 1),
                "as_of_date": dt.date(2024, 5, 1),
                "available_at": dt.datetime(2024, 5, 2, 12, 0),
            },
            {
                "security_id": "SEC-M1-POLICY",
                "symbol": "M1P",
                "delist_date": dt.date(2024, 5, 5),
                "as_of_date": dt.date(2024, 5, 5),
                "available_at": dt.datetime(2024, 5, 6, 12, 0),
            },
        ]
    )
    corporate_actions = pd.DataFrame(
        [
            {
                "security_id": "SEC-M1-POLICY",
                "action_type": "merger",
                "ex_date": dt.date(2024, 5, 5),
                "cash_amount": 15.0,
                "last_pre_delist_adjusted_close": 10.0,
                "available_at": dt.datetime(2024, 5, 5, 22, 0),
            }
        ]
    )

    with warnings.catch_warnings():
        warnings.simplefilter("error", FutureWarning)
        result = compute_delisting_terminal_returns(
            observations, events, policy, corporate_actions=corporate_actions
        )

    assert set(result["terminal_return_source"]) == {"observed", "policy"}
    observed_row = result[result["security_id"] == "SEC-M1-OBS"].iloc[0]
    assert observed_row["terminal_return_ex_div"] == pytest.approx(-0.58)  # real ex-div survives the union
    policy_row = result[result["security_id"] == "SEC-M1-POLICY"].iloc[0]
    assert pd.isna(policy_row["terminal_return_ex_div"])  # policy side is genuinely NA, not fabricated


def test_exchange_delete_with_full_merger_inputs_still_yields_no_policy_return(tmp_store) -> None:
    # M2: the plan's exchange_delete test fed an EMPTY corporate_actions, so apply_terminal_return_
    # policy short-circuited at its top guard and never exercised the is_observed_required filter.
    # Here corporate_actions is NON-empty and carries a full cash-merger input set that WOULD
    # compute a terminal return if exchange_delete were treated like a merger -- the merge + policy
    # candidate loop actually run, and the is_observed_required policy must still yield no row.
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim

    policy = load_terminal_return_policy_dim(tmp_store)
    events = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "symbol": "DLS",
                "delist_date": dt.date(2024, 4, 1),
                "as_of_date": dt.date(2024, 4, 1),
                "available_at": dt.datetime(2024, 4, 2, 12, 0),
                "corporate_action_type": "exchange_delete",
            }
        ]
    )
    actions = pd.DataFrame(
        [
            {
                "security_id": SECURITY_ID,
                "ex_date": dt.date(2024, 4, 1),
                "action_type": "exchange_delete",
                "cash_amount": 21.0,  # full cash-merger inputs, deliberately present
                "last_pre_delist_adjusted_close": 20.0,
                "available_at": dt.datetime(2024, 4, 1, 22, 0),
            }
        ]
    )
    out = apply_terminal_return_policy(events, actions, policy)
    assert out.empty  # is_observed_required policy: exchange_delete never gets a policy return

    # Discrimination: the IDENTICAL financial inputs under a 'merger' event DO produce a row, so
    # out.empty above is the is_observed_required policy at work, not merely absent inputs.
    merger_out = apply_terminal_return_policy(
        events.assign(corporate_action_type="merger"),
        actions.assign(action_type="cash_merger"),
        policy,
    )
    assert len(merger_out) == 1
    assert merger_out.iloc[0]["terminal_return_policy"] == "merger_cash"
    assert merger_out.iloc[0]["terminal_return"] == pytest.approx(0.05)  # 21/20 - 1
