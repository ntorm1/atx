"""Consumers of the revisioned Nasdaq listing tables (AF1).

A1 stores canonical ``add``/``delete`` actions and keeps superseded snapshot rows
(``is_latest_revision = false``) for audit instead of deleting them. Readers that mean
"the current state" -- the action-vocabulary check, dataset watermarks and their
completeness check, and the per-snapshot listing metrics -- must accept the canonical
vocabulary and read only the latest revision of each snapshot.
"""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.quality.checks_features_catalog import feature_catalog_check_specs
from atx_db.quality.checks_market_reference import market_reference_check_specs

DIRECTORY_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt"
EVENTS_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/TradingSystemAddsDeletes.txt"
SOURCE_DATED = dt.date(2026, 9, 18)  # the file's own File Creation Time date
OPERATOR_DATED = dt.date(2026, 9, 20)  # the pre-A1 operator stamp, superseded on reload
RECEIVED = dt.datetime(2026, 9, 20, 0, 6, 22)
RELOADED = dt.datetime(2026, 9, 25, 9, 0)
_STALE_DAYS = {"daily_macro_stale_days": 7, "monthly_macro_stale_days": 60, "valuation_stale_gap_days": 10}


def _check(specs, name: str):
    return next(check for check in specs(**_STALE_DAYS) if check.check_name == name)


def _directory(store, symbol: str, as_of: dt.date, *, latest: bool, financial_status: str) -> None:
    store.con.execute(
        """
        INSERT INTO nasdaq_symbol_directory
            (directory, symbol, security_name, market_category, exchange, cqs_symbol, etf, test_issue,
             financial_status, round_lot_size, next_shares, nasdaq_symbol, as_of_date, source_url, run_id,
             source_loaded_at, available_at, is_latest_revision)
        VALUES ('nasdaqlisted', ?, ?, 'Q', 'NASDAQ', NULL, false, false, ?, 100, false, ?, ?, ?, 'r', ?, ?, ?)
        """,
        [symbol, f"{symbol} Common Stock", financial_status, symbol, as_of, DIRECTORY_URL, RELOADED, RECEIVED,
         latest],
    )


def _event(
    store,
    event_id: str,
    symbol: str,
    *,
    effective: dt.date = SOURCE_DATED,
    as_of: dt.date = SOURCE_DATED,
    created: dt.datetime = dt.datetime(2026, 9, 18, 21, 31),
    latest: bool = True,
    actions: tuple[str | None, str | None, str | None] = ("add", None, None),
) -> None:
    store.con.execute(
        """
        INSERT INTO nasdaq_listing_events
            (event_id, symbol, security_id, company_name, nasdaq_action, bx_action, psx_action, effective_date,
             primary_listing_market, as_of_date, source_file_created_at, source_url, run_id, source_loaded_at,
             available_at, is_latest_revision)
        VALUES (?, ?, NULL, ?, ?, ?, ?, ?, 'Q', ?, ?, ?, 'r', ?, ?, ?)
        """,
        [event_id, symbol, f"{symbol} Corp", *actions, effective, as_of, created, EVENTS_URL, RELOADED, RECEIVED,
         latest],
    )


@pytest.mark.parametrize(
    "actions",
    [
        ("add", None, None),
        ("delete", "delete", "add"),
        ("Add", None, "Delete"),  # stored before A1 canonicalized the vocabulary
        ("A", "D", " d "),
        (None, "", None),
    ],
)
def test_listing_event_action_check_accepts_canonical_and_legacy_spellings(tmp_store, actions):
    check = _check(market_reference_check_specs, "bad_listing_event_actions")
    _event(tmp_store, "valid", "AAA", actions=actions)
    assert tmp_store.con.execute(check.sql).fetchone() == (0.0,)

    _event(tmp_store, "unrecognized", "BBB", actions=("add", "X", None))
    assert tmp_store.con.execute(check.sql).fetchone() == (1.0,)


def _listing_interval(store, symbol: str) -> None:
    store.con.execute(
        """
        INSERT INTO listing_status_intervals
            (listing_status_id, security_id, symbol, listing_venue_code, listing_venue_name,
             listing_exchange_code, status, valid_from, valid_to, as_of_date, available_at,
             last_evidence_as_of_date, source, evidence_source, evidence_source_table, method,
             source_loaded_at)
        VALUES (?, ?, ?, 'Q', 'Nasdaq', 'Q', 'active', ?, NULL, ?, ?, ?,
                'atx_listing_status_intervals_v1', 'nasdaq_symbol_directory',
                'nasdaq_symbol_directory', 'snapshot_presence', ?)
        """,
        [f"lsi-{symbol}", f"SEC-CIK-{symbol}", symbol, SOURCE_DATED, SOURCE_DATED, RECEIVED, SOURCE_DATED,
         RECEIVED],
    )


def _listing_watermarks(store) -> dict[tuple[str, str], str]:
    return {
        (dataset, name): value
        for dataset, name, value in store.con.execute(
            "SELECT dataset_id, watermark_name, watermark_value FROM dataset_watermarks "
            "WHERE dataset_id IN ('nasdaq_symbol_directory', 'nasdaq_listing_events')"
        ).fetchall()
    }


def test_superseded_listing_rows_are_invisible_to_current_state_consumers(tmp_store):
    from atx_db.listing_metrics import SecurityListingMetricsOptions, refresh_security_listing_metrics
    from atx_db.watermarks import refresh_warehouse_watermarks

    completeness = _check(feature_catalog_check_specs, "missing_core_dataset_watermarks")

    def missing_watermarks():
        # Legacy sources the schema template lacks; the check only counts their rows.
        legacy = ("tbltickerhistory_daily", "finra_short_interest", "thirteenf_submissions")
        for table in legacy:
            tmp_store.con.execute(f"CREATE TABLE {table} (stub INTEGER)")
        try:
            return tmp_store.con.execute(completeness.sql).fetchone()
        finally:
            for table in legacy:
                tmp_store.con.execute(f"DROP TABLE {table}")

    refresh_warehouse_watermarks(tmp_store)
    baseline = missing_watermarks()

    # Only superseded event rows: no current listing-event state, so no watermark -- and the
    # completeness check must not demand one.
    _event(tmp_store, "ev-operator-dated", "AAA", effective=dt.date(2026, 9, 19), as_of=OPERATOR_DATED,
           created=dt.datetime(2026, 9, 20, 0, 0), latest=False)
    refresh_warehouse_watermarks(tmp_store)
    assert _listing_watermarks(tmp_store) == {}
    assert missing_watermarks() == baseline

    # A re-dated reload: the operator-dated 2026-09-20 directory rows are superseded by the
    # source-dated 2026-09-18 rows of the same content.
    _directory(tmp_store, "AAA", OPERATOR_DATED, latest=False, financial_status="D")
    _directory(tmp_store, "AAA", SOURCE_DATED, latest=True, financial_status="N")
    _event(tmp_store, "ev-source-dated", "AAA", effective=dt.date(2026, 9, 17))
    _listing_interval(tmp_store, "AAA")

    refresh_warehouse_watermarks(tmp_store)
    assert _listing_watermarks(tmp_store) == {
        ("nasdaq_symbol_directory", "max_as_of_date"): "2026-09-18",
        ("nasdaq_listing_events", "max_effective_date"): "2026-09-17",
        ("nasdaq_listing_events", "max_as_of_date"): "2026-09-18",
        ("nasdaq_listing_events", "max_source_file_created_at"): "2026-09-18 21:31:00",
    }
    assert missing_watermarks() == baseline

    # Per-snapshot listing metrics: one snapshot (2026-09-18), not a phantom 2026-09-20
    # "deficient" snapshot from the superseded operator-dated rows.
    assert refresh_security_listing_metrics(tmp_store, SecurityListingMetricsOptions(source="fixture")) == 1
    assert tmp_store.con.execute(
        "SELECT as_of_date, financial_status_code FROM security_listing_metrics WHERE is_latest_revision"
    ).fetchall() == [(SOURCE_DATED, "N")]


def test_listing_metrics_are_visible_from_the_directory_receipt_not_the_reload(tmp_store):
    """A retained directory file received 2026-09-20 00:06 and reloaded 2026-09-25 is
    knowable from its receipt: an as-of reader between receipt and reload sees the
    deficiency flag, and one before the receipt sees nothing."""
    from atx_db.asof.ownership import security_listing_metrics_asof
    from atx_db.listing_metrics import SecurityListingMetricsOptions, refresh_security_listing_metrics

    _directory(tmp_store, "AAA", SOURCE_DATED, latest=True, financial_status="D")
    _listing_interval(tmp_store, "AAA")
    assert refresh_security_listing_metrics(tmp_store, SecurityListingMetricsOptions(source="fixture")) == 1
    assert tmp_store.con.execute("SELECT available_at FROM security_listing_metrics").fetchall() == [(RECEIVED,)]

    between = security_listing_metrics_asof(
        tmp_store, as_of_date=dt.date(2026, 9, 21), as_of_ts=dt.datetime(2026, 9, 21, 22, 0)
    )
    assert between[["symbol", "financial_status_code", "is_deficient"]].values.tolist() == [["AAA", "D", True]]
    before_receipt = security_listing_metrics_asof(
        tmp_store, as_of_date=dt.date(2026, 9, 19), as_of_ts=dt.datetime(2026, 9, 19, 22, 0)
    )
    assert before_receipt.empty
