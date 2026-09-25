"""A5: explicit price-line -> accounting-owner bridge for the daily market panel.

Economic/PIT contracts proven here:
* a secondary share-class line (``TBLTICKERHISTORY-7`` trading X's current
  class-B ticker) receives X's fundamentals in reconstructed mode, and none in
  strict mode without dated evidence;
* a delisted line whose last symbol is no longer a current ticker is unlinked
  and counted -- never silently dropped or given someone else's data;
* a former holder of a reused symbol is not linked to the current holder's
  issuer, even when the loader gave it the issuer's SEC id;
* strict links never become available before their evidence clock;
* accounting content split across the unresolved-CIK and resolved owner ids is
  read as one issuer, and a sole line receives the issuer's DEI shares while a
  multi-class issuer's lines keep line-level shares (A8 defines class basis).
"""

from __future__ import annotations

import datetime as dt
import inspect

import pytest

from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import DERIVED_SOURCE_NAME, seed_derived_metric_definitions
from atx_db.market_daily import (
    MARKET_DAILY_SOURCE_NAME,
    MARKET_DAILY_STRICT_SOURCE_NAME,
    MarketDailyDataset,
    MarketDailyOptions,
    _referenced_codes,
    owner_bridge_report,
    refresh_market_daily_metrics,
)
from atx_db.market_owner_bridge import (
    IDENTITY_BASIS_CURRENT_TICKER,
    OwnerLinkEvidence,
    PriceLine,
    build_market_owner_bridge,
    classify_reconstructed,
    normalize_symbol,
)
from tests.test_market_daily import _fact, _weekdays

X_CIK = "0000000042"
X_OWNER = "SEC-CIK-0000000042"
B_LINE = "TBLTICKERHISTORY-7"
DEAD_LINE = "TBLTICKERHISTORY-9"
_QUARTERS = (dt.date(2019, 3, 31), dt.date(2019, 6, 30), dt.date(2019, 9, 30), dt.date(2019, 12, 31))
_DATES = _weekdays(dt.date(2020, 1, 2), 130)  # 2020-01-02 .. 2020-07-01
_FUNDAMENTALS_VISIBLE = dt.date(2020, 2, 10)  # last quarter available 2020-02-09 21:00


def _bar(store, security_id, symbol, trade_date, close, shares=None):
    store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close,
            adjusted_close, volume, split_factor, is_adjusted, available_at,
            as_of_date, is_latest_revision, shares_outstanding
        ) VALUES ('test', ?, ?, ?, ?, ?, ?, ?, ?, 1000, 1.0, false, ?, ?, true, ?)
        """,
        [
            security_id,
            symbol,
            trade_date,
            close,
            close,
            close,
            close,
            close,
            dt.datetime.combine(trade_date, dt.time(22)),
            trade_date,
            shares,
        ],
    )


def _ticker(store, cik, ticker):
    store.con.execute(
        "INSERT INTO sec_company_tickers (cik, ticker, title, security_id, source_loaded_at) "
        "VALUES (?, ?, 'issuer', ?, TIMESTAMP '2026-09-20 12:00:00')",
        [cik, ticker, f"SEC-CIK-{int(cik):010d}"],
    )


def _dei(store, security_id, cik, count, available_at, key):
    store.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count, source_url
        ) VALUES (?, 'test', ?, ?, 'shares_outstanding', 'dei', 'EntityCommonStockSharesOutstanding',
                  'shares', 'instant', ?, ?, ?, ?, 'acc', 1, 1, true, ?, 'test')
        """,
        [key, security_id, cik, available_at.date(), available_at.date(), available_at.date(), available_at, count],
    )


def _issuer_facts(store, owner_id):
    for index, period_end in enumerate(_QUARTERS):
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=40), dt.time(21))
        _fact(store, owner_id, "revenue", "quarterly", period_end, 1_000_000.0 + index, available_at)
        _fact(store, owner_id, "net_income_to_common", "quarterly", period_end, 100_000.0, available_at)
        _fact(store, owner_id, "common_equity", "instant", period_end, 5_000_000.0, available_at)


@pytest.fixture
def multiclass(tmp_store):
    """Owner X has a class-A line (SEC id) and a class-B line (vendor id); one line is delisted."""
    seed_derived_metric_definitions(tmp_store)
    _ticker(tmp_store, X_CIK, "XA")
    _ticker(tmp_store, X_CIK, "XB")
    _issuer_facts(tmp_store, X_OWNER)
    for offset, trade_date in enumerate(_DATES):
        _bar(tmp_store, X_OWNER, "XA", trade_date, 100.0 + offset / 100, shares=10_000_000)
        _bar(tmp_store, B_LINE, "XB", trade_date, 50.0 + offset / 100, shares=30_000_000)
        if trade_date < dt.date(2020, 3, 2):
            _bar(tmp_store, DEAD_LINE, "DEAD", trade_date, 5.0, shares=1_000_000)
    # An issuer-level DEI count must not be paired with either class line's price.
    _dei(tmp_store, X_OWNER, X_CIK, 40_000_000, dt.datetime(2020, 1, 15, 21), "x-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    return tmp_store


def _rows(store, security_id, source=MARKET_DAILY_SOURCE_NAME):
    return store.con.execute(
        "SELECT trade_date, ps_ttm, pe_ttm, fundamental_available_at, shares_outstanding, shares_source "
        "FROM market_daily_metrics WHERE source = ? AND security_id = ? ORDER BY trade_date",
        [source, security_id],
    ).fetchall()


def test_secondary_class_line_gets_owner_fundamentals_only_in_reconstructed_mode(multiclass):
    refresh_market_daily_metrics(multiclass, MarketDailyOptions())
    a_rows = {row[0]: row for row in _rows(multiclass, X_OWNER)}
    b_rows = _rows(multiclass, B_LINE)
    assert len(b_rows) == len(_DATES)
    visible = [row for row in b_rows if row[0] >= _FUNDAMENTALS_VISIBLE]
    assert visible and all(row[1] is not None and row[2] is not None for row in visible)
    for trade_date, ps_ttm, _pe, fundamental_at, shares, shares_source in b_rows:
        # Same issuer state as the owner's own line on every date, never ahead of the bar's cutoff.
        assert fundamental_at is not None and fundamental_at == a_rows[trade_date][3]
        assert fundamental_at <= dt.datetime.combine(trade_date, dt.time(22))
        assert shares_source == "archive" and shares == 30_000_000
        if trade_date < _FUNDAMENTALS_VISIBLE:
            assert ps_ttm is None  # the fourth quarter of revenue is not yet available
            continue
        close_b = multiclass.con.execute(
            "SELECT close FROM equity_daily_bars WHERE security_id = ? AND trade_date = ?", [B_LINE, trade_date]
        ).fetchone()[0]
        # Valued on B's own market value (B price x B line shares) over X's TTM revenue.
        assert ps_ttm == pytest.approx(close_b * 30_000_000 / (4 * 1_000_000.0 + 6.0))
    # Multi-class owner: the issuer-level DEI count (40M) is never paired with one class's price.
    assert {row[5] for row in a_rows.values()} == {"archive"}

    strict = MarketDailyOptions(owner_mode="strict", source=MARKET_DAILY_STRICT_SOURCE_NAME)
    refresh_market_daily_metrics(multiclass, strict)
    strict_b = _rows(multiclass, B_LINE, MARKET_DAILY_STRICT_SOURCE_NAME)
    assert len(strict_b) == len(b_rows)
    assert all(row[1] is None and row[3] is None for row in strict_b)
    # The strict variant is a separate labeled panel; the reconstructed one is intact.
    assert _rows(multiclass, B_LINE) == b_rows
    strict_report = owner_bridge_report(multiclass, source=MARKET_DAILY_STRICT_SOURCE_NAME)
    assert strict_report is not None
    assert strict_report["linked_lines"] == 0
    assert strict_report["unlinked_by_reason"] == {"no_dated_evidence": 3}


def test_delisted_line_without_current_ticker_is_unlinked_and_counted(multiclass):
    result = MarketDailyDataset().run(multiclass, MarketDailyOptions())
    dead = _rows(multiclass, DEAD_LINE)
    assert dead and all(row[1] is None and row[3] is None for row in dead)
    bridge = result.details["owner_bridge"]
    assert bridge["mode"] == "reconstructed"
    assert bridge["identity_basis"] == IDENTITY_BASIS_CURRENT_TICKER
    assert bridge["availability_basis"] == "modeled"
    assert bridge["lines"] == 3
    assert bridge["linked_lines"] == 2 and bridge["unlinked_lines"] == 1
    assert bridge["unlinked_by_reason"] == {"no_current_ticker": 1}
    assert bridge["unlinked_bar_rows"] == len(dead)
    assert bridge["linked_by_method"] == {"current_sec_ticker": 2}
    assert bridge["secondary_lines_linked"] == 1
    assert bridge["multi_line_owners"] == 1 and bridge["dei_shares_withheld_lines"] == 2
    assert bridge["ticker_snapshot_observed_at"] == "2026-09-20T12:00:00"
    # The same accounting is durable in data_quality_checks for run evidence.
    assert owner_bridge_report(multiclass) == bridge


def test_reused_symbol_former_holder_is_not_linked_to_current_issuer(tmp_store):
    """The loader gave the SEC id to the *old* line (more observations); only the live line links."""
    seed_derived_metric_definitions(tmp_store)
    _ticker(tmp_store, "88", "ZZ")
    _issuer_facts(tmp_store, "SEC-CIK-0000000088")
    old_dates = _weekdays(dt.date(2019, 1, 2), 300)
    for trade_date in old_dates:
        _bar(tmp_store, "SEC-CIK-0000000088", "ZZ", trade_date, 9.0)
    for trade_date in _DATES:
        _bar(tmp_store, "TBLTICKERHISTORY-20", "ZZ", trade_date, 30.0, shares=1_000_000)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    old = _rows(tmp_store, "SEC-CIK-0000000088")
    assert old and all(row[3] is None for row in old)
    live = _rows(tmp_store, "TBLTICKERHISTORY-20")
    assert all(row[1] is not None for row in live if row[0] >= _FUNDAMENTALS_VISIBLE)
    report = owner_bridge_report(tmp_store)
    assert report["unlinked_by_reason"] == {"symbol_held_by_other_line": 1}


def test_unresolved_and_resolved_owner_content_merge_and_sole_line_takes_dei(tmp_store):
    """Renamed single-class issuer Y: content under both CIK owner ids, one price line."""
    seed_derived_metric_definitions(tmp_store)
    _ticker(tmp_store, "77", "YNEW")
    unresolved = "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000077"
    _issuer_facts(tmp_store, unresolved)
    later = dt.datetime(2020, 5, 11, 21)
    _fact(tmp_store, "SEC-CIK-0000000077", "preferred_stock", "instant", dt.date(2020, 3, 31), 1.0, later)
    _dei(tmp_store, unresolved, "77", 3_000_000, dt.datetime(2020, 1, 15, 21), "y-dei")
    for offset, trade_date in enumerate(_DATES):
        symbol = "YOLD" if trade_date < dt.date(2020, 4, 1) else "YNEW"
        _bar(tmp_store, "TBLTICKERHISTORY-11", symbol, trade_date, 20.0 + offset / 100, shares=2_900_000)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = {row[0]: row for row in _rows(tmp_store, "TBLTICKERHISTORY-11")}
    # Fundamentals from the unresolved-CIK owner, DEI shares through the bridge (sole line).
    feb = rows[dt.date(2020, 2, 12)]
    assert feb[1] is not None and feb[4] == 3_000_000 and feb[5] == "dei"
    assert rows[dt.date(2020, 1, 14)][5] == "archive"  # DEI not yet available
    # The resolved-owner fact joins the same issuer state from its own clock on.
    assert rows[dt.date(2020, 5, 11)][3] == later
    assert rows[dt.date(2020, 5, 8)][3] < later
    assert owner_bridge_report(tmp_store)["dei_shares_withheld_lines"] == 0


def _evidence(**overrides):
    base = {
        "price_security_id": B_LINE,
        "cik": X_CIK,
        "valid_from": dt.date(2019, 1, 1),
        "available_at": dt.datetime(2020, 6, 15, 21),
        "evidence_status": "verified_dated",
        "availability_status": "verified",
        "artifact_sha256": "a" * 64,
        "source_locator": "sec/8-A12B/0000000042-20-000001.txt",
        "share_class_symbol": "XB",
        "source_published_at": dt.datetime(2020, 6, 15, 20),
        "evidence_id": "ev-1",
    }
    base.update(overrides)
    return OwnerLinkEvidence(**base)


def test_strict_link_is_never_available_before_its_evidence_clock(multiclass):
    evidence = (
        _evidence(),
        _evidence(evidence_id="ev-snapshot", evidence_status="snapshot"),
        _evidence(evidence_id="ev-early", available_at=dt.datetime(2020, 6, 15, 19)),
        # Two verified facts assigning the delisted line to different issuers over one interval.
        _evidence(
            price_security_id=DEAD_LINE,
            cik="99",
            evidence_id="ev-a",
            available_at=dt.datetime(2019, 1, 5),
            source_published_at=None,
        ),
        _evidence(
            price_security_id=DEAD_LINE,
            cik="98",
            evidence_id="ev-b",
            available_at=dt.datetime(2019, 1, 5),
            source_published_at=None,
        ),
    )
    item_codes, metric_codes = _referenced_codes()
    bridge = build_market_owner_bridge(
        multiclass,
        mode="strict",
        evidence=evidence,
        item_codes=item_codes,
        metric_codes=metric_codes,
        derived_source=DERIVED_SOURCE_NAME,
    )
    linked = [row for row in bridge.rows if row.linked]
    assert [(row.price_security_id, row.owner_security_id) for row in linked] == [(B_LINE, X_OWNER)]
    assert linked[0].available_at == evidence[0].available_at >= evidence[0].source_published_at
    assert bridge.rejected_evidence == {
        "available_before_publication": 1,
        "conflicting_evidence": 2,
        "evidence_not_verified_dated": 1,
    }

    options = MarketDailyOptions(owner_mode="strict", source=MARKET_DAILY_STRICT_SOURCE_NAME)
    refresh_market_daily_metrics(multiclass, options, owner_evidence=evidence)
    rows = _rows(multiclass, B_LINE, MARKET_DAILY_STRICT_SOURCE_NAME)
    first_linked = min(row[0] for row in rows if row[3] is not None)
    assert first_linked == dt.date(2020, 6, 15)  # cutoff 22:00 >= evidence clock 21:00
    assert all(row[1] is None for row in rows if row[0] < first_linked)
    assert all(row[1] is not None for row in rows if row[0] >= first_linked)
    assert all(row[1] is None for row in _rows(multiclass, DEAD_LINE, MARKET_DAILY_STRICT_SOURCE_NAME))


def test_owner_aligned_batches_never_split_an_owner_and_match_one_batch(multiclass):
    item_codes, metric_codes = _referenced_codes()
    bridge = build_market_owner_bridge(
        multiclass,
        item_codes=item_codes,
        metric_codes=metric_codes,
        derived_source=DERIVED_SOURCE_NAME,
    )
    batches = bridge.owner_aligned_batches(bridge.line_ids(), 1)
    assert batches == [(X_OWNER, B_LINE), (DEAD_LINE,)]

    snapshot_sql = "SELECT * EXCLUDE (source_loaded_at) FROM market_daily_metrics ORDER BY security_id, trade_date"
    refresh_market_daily_metrics(multiclass, MarketDailyOptions(batch_size=1))
    per_owner = multiclass.con.execute(snapshot_sql).fetchall()
    refresh_market_daily_metrics(multiclass, MarketDailyOptions(batch_size=500))
    assert multiclass.con.execute(snapshot_sql).fetchall() == per_owner


def test_reconstructed_rules_ambiguity_and_symbol_normalization():
    day = dt.date(2020, 1, 2)
    lines = [
        PriceLine("TBLTICKERHISTORY-1", "BF.B", day, day, 1),
        PriceLine("TBLTICKERHISTORY-2", "DUP", day, day, 1),
        PriceLine("SEC-CIK-0000000005", "GONE", day, day, 1),
        PriceLine("LEGACY-1", "AAA", day, day, 1),
        PriceLine("TBLTICKERHISTORY-3", "AAA2", day, day, 1),
    ]
    tickers = [("14693", "BF-B", None), ("1", "DUP", None), ("2", "DUP", None)]
    rows, members, _ambiguous = classify_reconstructed(
        lines,
        tickers,
        {"LEGACY-1": frozenset(), "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000014693": frozenset({"0000014693"})},
    )
    by_line = {row.price_security_id: row for row in rows}
    assert normalize_symbol("bf/b ") == normalize_symbol("BF-B") == "BF-B"
    assert (by_line["TBLTICKERHISTORY-1"].owner_security_id, by_line["TBLTICKERHISTORY-1"].share_class_symbol) == (
        "SEC-CIK-0000014693",
        "BF-B",
    )
    assert members["SEC-CIK-0000014693"] == ("SEC-CIK-0000014693", "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000014693")
    assert by_line["TBLTICKERHISTORY-2"].unlinked_reason == "ambiguous_current_ticker"
    assert by_line["SEC-CIK-0000000005"].link_method == "cik_security_id"
    assert by_line["LEGACY-1"].link_method == "shared_security_id"
    assert by_line["TBLTICKERHISTORY-3"].unlinked_reason == "no_current_ticker"
    # Reconstructed availability is modeled from the line's first bar cutoff, and labeled so.
    linked = by_line["TBLTICKERHISTORY-1"]
    assert linked.available_at == dt.datetime(2020, 1, 2, 22) and linked.availability_basis == "modeled"


def test_each_identity_basis_writes_its_own_labeled_source():
    with pytest.raises(ValueError, match="distinct labeled source"):
        MarketDailyOptions(owner_mode="strict")
    with pytest.raises(ValueError, match="strict-identity panel source"):
        MarketDailyOptions(source=MARKET_DAILY_STRICT_SOURCE_NAME)
    with pytest.raises(ValueError, match="owner_mode"):
        MarketDailyOptions(owner_mode="current")


def test_no_wall_clock_in_the_bridge_source():
    import atx_db.market_owner_bridge as module

    source = inspect.getsource(module)
    for forbidden in ("date.today", "utcnow", "Timestamp.now", "time.time", "datetime.now", "current_date"):
        assert forbidden not in source, forbidden
