from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


def _input_row(
    *,
    security_id: str = "SEC-CIK-0000320193",
    symbol: str = "AAPL",
    as_of_date: dt.date = dt.date(2026, 6, 27),
    directory: str = "nasdaqlisted",
    market_category: str | None = "Q",
    exchange: str | None = "NASDAQ",
    security_name: str = "Apple Inc. Common Stock",
    financial_status: str | None = "N",
    etf: bool = False,
    test_issue: bool = False,
    next_shares: bool = False,
    round_lot_size: int | None = 100,
    listing_status: str = "active",
    listing_venue_code: str | None = "Q",
    listing_venue_name: str | None = "Nasdaq",
    listing_exchange_code: str | None = "Q",
    available_at: dt.datetime = dt.datetime(2026, 6, 28, 7, 16),
) -> dict:
    return {
        "security_id": security_id,
        "symbol": symbol,
        "as_of_date": as_of_date,
        "directory": directory,
        "market_category": market_category,
        "exchange": exchange,
        "security_name": security_name,
        "financial_status": financial_status,
        "etf": etf,
        "test_issue": test_issue,
        "next_shares": next_shares,
        "round_lot_size": round_lot_size,
        "listing_status": listing_status,
        "listing_venue_code": listing_venue_code,
        "listing_venue_name": listing_venue_name,
        "listing_exchange_code": listing_exchange_code,
        "available_at": available_at,
    }


def test_security_listing_metrics_decode_deficient_flag():
    from atx_db.listing_metrics import (
        SecurityListingMetricsOptions,
        compute_security_listing_metrics,
    )

    out = compute_security_listing_metrics(
        pd.DataFrame([_input_row(financial_status="D", market_category="S")]),
        options=SecurityListingMetricsOptions(source="fixture"),
    )
    assert len(out) == 1
    row = out.iloc[0]
    assert row["financial_status_code"] == "D"
    assert row["financial_status_label"] == "Deficient"
    assert bool(row["has_financial_status"]) is True
    assert bool(row["is_listing_compliant"]) is False
    assert bool(row["is_deficient"]) is True
    assert bool(row["is_delinquent"]) is False
    assert bool(row["is_bankrupt"]) is False
    assert bool(row["is_noncompliant"]) is True
    assert row["market_tier"] == "NASDAQ Capital Market"
    assert row["as_of_date"] == dt.date(2026, 6, 27)
    assert row["metric_id"]


def test_security_listing_metrics_compliant_and_otherlisted_unknown_status():
    from atx_db.listing_metrics import compute_security_listing_metrics

    out = compute_security_listing_metrics(
        pd.DataFrame(
            [
                _input_row(financial_status="N", symbol="MSFT", security_id="SEC-CIK-0000789019"),
                _input_row(
                    symbol="JPM",
                    security_id="SEC-CIK-0000019617",
                    directory="otherlisted",
                    market_category=None,
                    exchange="N",
                    financial_status=None,
                    listing_venue_code="N",
                    listing_exchange_code="N",
                ),
            ]
        )
    )
    by_symbol = {r["symbol"]: r for _, r in out.iterrows()}

    compliant = by_symbol["MSFT"]
    assert bool(compliant["is_listing_compliant"]) is True
    assert bool(compliant["is_noncompliant"]) is False
    assert bool(compliant["has_financial_status"]) is True

    # otherlisted (NYSE) names carry no NASDAQ compliance flag: unknown, not non-compliant
    other = by_symbol["JPM"]
    assert other["financial_status_code"] is None or pd.isna(other["financial_status_code"])
    assert bool(other["has_financial_status"]) is False
    assert bool(other["is_noncompliant"]) is False
    assert bool(other["is_deficient"]) is False
    assert other["listing_exchange_name"] == "New York Stock Exchange"


def test_security_listing_metrics_combined_status_codes():
    from atx_db.listing_metrics import compute_security_listing_metrics

    out = compute_security_listing_metrics(
        pd.DataFrame(
            [
                _input_row(financial_status="H", symbol="AAA", security_id="SEC-1"),
                _input_row(financial_status="K", symbol="BBB", security_id="SEC-2"),
                _input_row(financial_status="Q", symbol="CCC", security_id="SEC-3"),
            ]
        )
    )
    by_symbol = {r["symbol"]: r for _, r in out.iterrows()}

    h = by_symbol["AAA"]  # Deficient and Delinquent
    assert (bool(h["is_deficient"]), bool(h["is_delinquent"]), bool(h["is_bankrupt"])) == (True, True, False)

    k = by_symbol["BBB"]  # Deficient, Delinquent, and Bankrupt
    assert (bool(k["is_deficient"]), bool(k["is_delinquent"]), bool(k["is_bankrupt"])) == (True, True, True)

    q = by_symbol["CCC"]  # Bankrupt only
    assert (bool(q["is_deficient"]), bool(q["is_delinquent"]), bool(q["is_bankrupt"])) == (False, False, True)


def test_security_listing_metrics_dedup_grain():
    """Two symbols resolving to one security_id on one snapshot collapse to one grain row."""
    from atx_db.listing_metrics import compute_security_listing_metrics

    out = compute_security_listing_metrics(
        pd.DataFrame(
            [
                _input_row(symbol="GOOG", security_id="SEC-CIK-0001652044", financial_status="N"),
                _input_row(symbol="GOOGL", security_id="SEC-CIK-0001652044", financial_status="N"),
            ]
        )
    )
    assert len(out) == 1


def _insert_directory(store, *, symbol, as_of_date, financial_status, source_loaded_at, directory="nasdaqlisted"):
    store.con.execute(
        """
        INSERT INTO nasdaq_symbol_directory
            (directory, symbol, security_name, market_category, exchange, cqs_symbol,
             etf, test_issue, financial_status, round_lot_size, next_shares, nasdaq_symbol,
             as_of_date, source_url, run_id, source_loaded_at)
        VALUES (?, ?, ?, 'Q', 'NASDAQ', NULL, false, false, ?, 100, false, ?, ?, 'u', 'r', ?)
        """,
        [directory, symbol, f"{symbol} Common Stock", financial_status, symbol, as_of_date, source_loaded_at],
    )


def _insert_listing(store, *, listing_status_id, security_id, symbol, valid_from, as_of_date, available_at):
    store.con.execute(
        """
        INSERT INTO listing_status_intervals
            (listing_status_id, security_id, symbol, listing_venue_code, listing_venue_name,
             listing_exchange_code, status, valid_from, valid_to, as_of_date, available_at,
             source, evidence_source, evidence_source_table, method, source_loaded_at)
        VALUES (?, ?, ?, 'Q', 'Nasdaq', 'Q', 'active', ?, NULL, ?, ?,
                'atx_listing_status_intervals_v1', 'nasdaq_symbol_directory',
                'nasdaq_symbol_directory', 'snapshot_presence', ?)
        """,
        [listing_status_id, security_id, symbol, valid_from, as_of_date, available_at, available_at],
    )


def test_security_listing_metrics_preserve_snapshot_revisions(tmp_store):
    from atx_db.asof import security_listing_metrics_asof
    from atx_db.listing_metrics import (
        SecurityListingMetricsOptions,
        refresh_security_listing_metrics,
    )

    _insert_listing(
        tmp_store,
        listing_status_id="lsi-aapl",
        security_id="SEC-CIK-0000320193",
        symbol="AAPL",
        valid_from=dt.date(2026, 6, 20),
        as_of_date=dt.date(2026, 6, 27),
        available_at=dt.datetime(2026, 6, 28, 7, 16),
    )
    # Snapshot 1: deficient on 2026-06-27
    _insert_directory(
        tmp_store,
        symbol="AAPL",
        as_of_date=dt.date(2026, 6, 27),
        financial_status="D",
        source_loaded_at=dt.datetime(2026, 6, 27, 20, 51),
    )
    # Snapshot 2: recovered to normal on 2026-06-28
    _insert_directory(
        tmp_store,
        symbol="AAPL",
        as_of_date=dt.date(2026, 6, 28),
        financial_status="N",
        source_loaded_at=dt.datetime(2026, 6, 28, 20, 51),
    )

    options = SecurityListingMetricsOptions(source="fixture")
    assert refresh_security_listing_metrics(tmp_store, options) == 2

    # As of the first snapshot's availability, the security is deficient.
    early = security_listing_metrics_asof(tmp_store, as_of_date=dt.date(2026, 6, 27), symbols=("AAPL",))
    assert len(early) == 1
    assert early.iloc[0]["financial_status_code"] == "D"
    assert bool(early.iloc[0]["is_deficient"]) is True

    # As of a later date, the latest snapshot shows recovery.
    later = security_listing_metrics_asof(tmp_store, as_of_date=dt.date(2026, 6, 30), symbols=("AAPL",))
    assert len(later) == 1
    assert later.iloc[0]["financial_status_code"] == "N"
    assert bool(later.iloc[0]["is_deficient"]) is False

    # Re-running the same snapshots must not create duplicate latest rows per grain.
    assert refresh_security_listing_metrics(tmp_store, options) == 2
    dupes = tmp_store.con.execute(
        """
        SELECT count(*) FROM (
            SELECT source, security_id, as_of_date,
                   count(*) FILTER (WHERE is_latest_revision) AS latest_rows
            FROM security_listing_metrics
            GROUP BY 1, 2, 3
            HAVING count(*) FILTER (WHERE is_latest_revision) > 1
        )
        """
    ).fetchone()[0]
    assert dupes == 0
