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
  read as one issuer, and a sole line receives the issuer's DEI shares;
* class guard: an issuer with more than one current SEC ticker or
  concurrently linked line -- linked or not, strict owner-key splits
  included -- never pairs issuer DEI with one class line's price; strict mode
  assigns no DEI; a per-class DEI filing is never used as an issuer count.

A8 (market-cap share, class, ADR, split and currency basis):
* a multi-class issuer's cap is sum(close_i x class count_i) over its bridged
  class lines only when every class is bridged and the counts reconcile to an
  issuer total -- $100x10M + $50x30M = $2.5B, never $100x40M -- else NULL with
  ``shares_source='multiclass_unresolved'``;
* a sibling class missing from the SEC map (CWEN/CWEN-A) or delisted before
  the snapshot (DISCA/DISCK), with one issuer DEI count or none, withholds the
  surviving line's cap and multiples for the dual-class period;
* LEN/LEN-B is dual-class through class evidence (not A2's classifier);
* an ADR line uses its vendor ADS count, never the ordinary DEI count;
* a split after the latest 10-Q never halves or doubles the cap;
* a DEI count off the line's own vendor count by > 1.5x is a basis conflict;
* a non-USD reporter keeps its market cap but no valuation multiple, counted;
* a vendor share run is used only once known: from the filing of the DEI
  cover count it equals (AAPL shape: the run starts on the cover date, two
  weeks before the filing), at its start when derived from a split, else
  after the modeled lag (Celgene shape).

P1 (RI1 reconstructed history -> bridge):
* a delisted line with no current ticker takes its RI1 issuer in reconstructed
  mode (``reconstructed_history``, tier in force, evidence id), is visible only
  from bars whose cutoff reaches the RI1 evidence clock, and is never linked in
  strict mode;
* an RI1 conflict stays unlinked (``conflicting_reconstruction``); a link
  contested later is linked only before the contest; one issuer is linked on
  one line per day (the other line's overlap is trimmed and counted).
"""

from __future__ import annotations

import datetime as dt
import inspect
from itertools import pairwise
from types import SimpleNamespace

import pytest

from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import DERIVED_SOURCE_NAME, seed_derived_metric_definitions
from atx_db.market_daily import (
    ARCHIVE_MODELED_LAG_DAYS,
    MARKET_DAILY_SOURCE_NAME,
    MARKET_DAILY_STRICT_SOURCE_NAME,
    MarketDailyDataset,
    MarketDailyOptions,
    _referenced_codes,
    owner_bridge_report,
    refresh_market_daily_metrics,
    vendor_share_state_query,
)
from atx_db.market_owner_bridge import (
    IDENTITY_BASIS_CURRENT_AND_RECONSTRUCTED,
    IDENTITY_BASIS_CURRENT_TICKER,
    RECONSTRUCTION_TIERS_HIGH_ONLY,
    CurrencyEvent,
    MarketOwnerBridge,
    OwnerLinkEvidence,
    PriceLine,
    ReconstructedLinkEvidence,
    are_class_siblings,
    build_market_owner_bridge,
    classify_reconstructed,
    classify_reconstructed_with_history,
    classify_sec_tickers,
    normalize_symbol,
    parse_ads_ratio,
)
from tests.test_market_daily import _fact, _weekdays

X_CIK = "0000000042"
X_OWNER = "SEC-CIK-0000000042"
B_LINE = "TBLTICKERHISTORY-7"
DEAD_LINE = "TBLTICKERHISTORY-9"
_QUARTERS = (dt.date(2019, 3, 31), dt.date(2019, 6, 30), dt.date(2019, 9, 30), dt.date(2019, 12, 31))
_DATES = _weekdays(dt.date(2020, 1, 2), 130)  # 2020-01-02 .. 2020-07-01
_FUNDAMENTALS_VISIBLE = dt.date(2020, 2, 10)  # last quarter available 2020-02-09 21:00


def _bar(store, security_id, symbol, trade_date, close, shares=None, adj=None):
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
            close if adj is None else adj,
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


def _dei(
    store,
    security_id,
    cik,
    count,
    available_at,
    key,
    accession="acc",
    share_class=None,
    *,
    effective=None,
    taxonomy="dei",
    concept="EntityCommonStockSharesOutstanding",
):
    effective = effective or available_at.date()
    store.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count, source_url, share_class
        ) VALUES (?, 'test', ?, ?, 'shares_outstanding', ?, ?,
                  'shares', 'instant', ?, ?, ?, ?, ?, 1, 1, true, ?, 'test', ?)
        """,
        [
            key,
            security_id,
            cik,
            taxonomy,
            concept,
            effective,
            effective,
            available_at.date(),
            available_at,
            accession,
            count,
            share_class,
        ],
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
        "SELECT trade_date, ps_ttm, pe_ttm, fundamental_available_at, shares_outstanding, shares_source, "
        "market_cap, close, pb, enterprise_value FROM market_daily_metrics "
        "WHERE source = ? AND security_id = ? ORDER BY trade_date",
        [source, security_id],
    ).fetchall()


_Q4_CLOCK = dt.datetime(2020, 2, 9, 21)  # revenue for 2019-12-31 becomes available
_DEI_VISIBLE = dt.date(2020, 1, 15)  # the fixture's DEI count is available 2020-01-15 21:00


def test_secondary_class_line_gets_owner_fundamentals_only_in_reconstructed_mode(multiclass):
    refresh_market_daily_metrics(multiclass, MarketDailyOptions())
    a_rows = {row[0]: row for row in _rows(multiclass, X_OWNER)}
    b_rows = _rows(multiclass, B_LINE)
    assert len(b_rows) == len(_DATES)
    for trade_date, _ps, _pe, fundamental_at, shares, shares_source, cap, close, _pb, _ev in b_rows:
        # The link: X's own issuer state on every date, never ahead of the bar's cutoff; the
        # fourth quarter becomes part of that state exactly from its own availability.
        assert fundamental_at is not None and fundamental_at == a_rows[trade_date][3]
        assert fundamental_at <= dt.datetime.combine(trade_date, dt.time(22))
        assert (fundamental_at >= _Q4_CLOCK) == (trade_date >= _FUNDAMENTALS_VISIBLE)
        # A8: the issuer-level DEI count (40M) never prices either class line.
        a = a_rows[trade_date]
        if trade_date < _DEI_VISIBLE:
            # No issuer total yet to prove the vendor counts are per class: withheld.
            assert shares_source == a[5] == "multiclass_unresolved"
            assert shares is None and cap is None and a[6] is None
        else:
            # Issuer cap = sum(close_i x class count_i) on both lines; each keeps its own count.
            assert (shares_source, shares, a[5], a[4]) == ("class_sum", 30_000_000, "class_sum", 10_000_000)
            assert cap == a[6] == pytest.approx(close * 30_000_000 + a[7] * 10_000_000)
    # The issuer multiples exist on both class lines once the issuer cap is resolved.
    for rows in (b_rows, list(a_rows.values())):
        visible = [row for row in rows if row[0] >= _FUNDAMENTALS_VISIBLE]
        assert visible and all(row[1] is not None and row[2] is not None and row[8] is not None for row in visible)
        assert all(row[1] is None and row[2] is None for row in rows if row[0] < _DEI_VISIBLE)

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
    assert bridge["linked_by_identity_basis"] == {"current_ticker_unverified": 2}
    assert bridge["multi_line_issuers"] == 1
    # A8: both class lines are priced only by the class sum (no DEI), not withheld at the bridge.
    assert bridge["dei_shares_withheld_lines"] == 2 and bridge["valuation_withheld_lines"] == 0
    assert bridge["withheld_multi_common_class"] == 2 and bridge["withheld_non_common_line"] == 0
    assert bridge["share_basis_lines"] == {"multi_class": 2}
    assert bridge["non_common_tickers_ignored"] == 0
    assert bridge["ticker_snapshot_observed_at"] == "2026-09-20T12:00:00"
    # A8 stage detail: row counts by share basis and the availability basis of bar-derived shares.
    share_basis = bridge["share_basis"]
    assert share_basis["cross_class_share_basis_rows"] == 0
    assert share_basis["class_sum_rows"] > 0 and share_basis["multiclass_unresolved_rows"] > 0
    assert share_basis["class_sum_rows"] + share_basis["multiclass_unresolved_rows"] == 2 * len(_DATES)
    assert share_basis["shares_availability_basis"]["archive"] == "vendor_run_clock"
    assert set(share_basis["archive_run_clocks"]) == {"split_derived", "dei_matched", "first_run", "modeled_lag"}
    assert result.details["archive_run_clocks"] == share_basis["archive_run_clocks"]
    assert result.details["shares_availability_basis"]["dei"] == "filing_available_at"
    assert result.details["share_basis"] == share_basis
    # Every run is labeled with its identity basis at the top level of the details ...
    assert (result.details["owner_mode"], result.details["identity_basis"], result.details["availability_basis"]) == (
        "reconstructed",
        "current_ticker_unverified",
        "modeled",
    )
    # ... and the same accounting is durable in data_quality_checks for run evidence.
    assert owner_bridge_report(multiclass) == bridge


def test_unlinked_sibling_class_still_blocks_issuer_dei_on_the_listed_class(tmp_store):
    """X has two current SEC tickers but its B line's vendor symbol does not map (B unlinked)."""
    seed_derived_metric_definitions(tmp_store)
    _ticker(tmp_store, X_CIK, "XA")
    _ticker(tmp_store, X_CIK, "XB")
    _issuer_facts(tmp_store, X_OWNER)
    for offset, trade_date in enumerate(_DATES):
        _bar(tmp_store, X_OWNER, "XA", trade_date, 100.0 + offset / 100, shares=10_000_000)
        _bar(tmp_store, B_LINE, "XBV", trade_date, 50.0 + offset / 100, shares=30_000_000)
    _dei(tmp_store, X_OWNER, X_CIK, 40_000_000, dt.datetime(2020, 1, 15, 21), "x-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    a_rows = _rows(tmp_store, X_OWNER)
    # Never $100 x 40M, and no partial class-A-only issuer multiple: class B is not bridged,
    # so the class sum cannot cover the issuer -- cap and multiples are withheld.
    assert {row[5] for row in a_rows} == {"multiclass_unresolved"}
    assert all(row[4] is None and row[6] is None and row[1] is None and row[2] is None for row in a_rows)
    report = owner_bridge_report(tmp_store)
    assert report["linked_lines"] == 1 and report["unlinked_by_reason"] == {"no_current_ticker": 1}
    assert report["multi_line_issuers"] == 1
    assert report["share_basis"]["multiclass_unresolved_rows"] == len(_DATES)


def test_per_class_dei_filing_is_never_used_as_an_issuer_count(tmp_store):
    """Single listed line; the issuer's later 10-Q reports DEI per class (A 3M, B 2M)."""
    seed_derived_metric_definitions(tmp_store)
    _ticker(tmp_store, "66", "QQ")
    _issuer_facts(tmp_store, "SEC-CIK-0000000066")
    _dei(tmp_store, "SEC-CIK-0000000066", "66", 5_000_000, dt.datetime(2020, 1, 15, 21), "q-1", "acc-1")
    per_class = dt.datetime(2020, 4, 15, 21)
    _dei(tmp_store, "SEC-CIK-0000000066", "66", 3_000_000, per_class, "q-2a", "acc-2", "A")
    _dei(tmp_store, "SEC-CIK-0000000066", "66", 2_000_000, per_class, "q-2b", "acc-2", "B")
    for offset, trade_date in enumerate(_DATES):
        _bar(tmp_store, "TBLTICKERHISTORY-30", "QQ", trade_date, 40.0 + offset / 100, shares=4_900_000)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = {row[0]: row for row in _rows(tmp_store, "TBLTICKERHISTORY-30")}
    before = rows[dt.date(2020, 4, 14)]
    assert before[5] == "dei" and before[4] == 5_000_000 and before[1] is not None
    # A8: the per-class filing proves a second (unlisted) class; one listed line cannot
    # carry the issuer cap, so neither 5M (issuer) nor 4.9M (one class) prices it.
    after = rows[dt.date(2020, 4, 16)]
    assert after[5] == "multiclass_unresolved" and after[4] is None and after[6] is None
    assert after[1] is None and after[2] is None
    report = owner_bridge_report(tmp_store)
    assert report["valuation_withheld_lines"] == 0 and report["withheld_per_class_dei"] == 1


def _directory(store, symbol, name):
    store.con.execute(
        "INSERT INTO nasdaq_symbol_directory (directory, symbol, security_name, exchange, etf, test_issue, "
        "as_of_date, source_url) VALUES ('otherlisted', ?, ?, 'N', false, false, DATE '2026-09-18', 'test')",
        [symbol, name],
    )


def test_listed_preferred_ticker_does_not_withhold_the_common_line(tmp_store):
    """Probe 5 (JPM/BAC pattern): one common line; the SEC map also lists a preferred ticker."""
    seed_derived_metric_definitions(tmp_store)
    _ticker(tmp_store, X_CIK, "XA")
    _ticker(tmp_store, X_CIK, "XA-PB")
    _issuer_facts(tmp_store, X_OWNER)
    for offset, trade_date in enumerate(_DATES):
        _bar(tmp_store, X_OWNER, "XA", trade_date, 100.0 + offset / 100, shares=10_000_000)
    _dei(tmp_store, X_OWNER, X_CIK, 10_000_000, dt.datetime(2020, 1, 15, 21), "x-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    visible = [row for row in _rows(tmp_store, X_OWNER) if row[0] >= _FUNDAMENTALS_VISIBLE]
    assert visible and all(row[1] is not None and row[2] is not None and row[8] is not None for row in visible)
    assert {row[5] for row in visible} == {"dei"}
    report = owner_bridge_report(tmp_store)
    assert report["valuation_withheld_lines"] == 0 and report["dei_shares_withheld_lines"] == 0
    assert report["non_common_tickers_ignored"] == 1
    assert report["non_common_tickers_by_basis"] == {"ticker_suffix": 1}
    assert report["non_common_tickers_by_class"] == {"preferred": 1}


def test_directory_classes_keep_common_valuation_and_withhold_true_dual_class(tmp_store):
    """X: common plus warrant/ETN/note/preferred tickers, an unlisted ticker and a traded preferred
    line; G: a true dual-class issuer (two common classes, both traded)."""
    seed_derived_metric_definitions(tmp_store)
    for ticker in ("XA", "XAW", "XETN", "XBND", "XA-PB", "XOTC"):
        _ticker(tmp_store, X_CIK, ticker)
    _directory(tmp_store, "XA", "X Corp Common Stock")
    _directory(tmp_store, "XAW", "X Corp Warrants")
    _directory(tmp_store, "XETN", "X Corp Exchange Traded Notes due 2030")
    _directory(tmp_store, "XBND", "X Corp 5.00% Senior Notes due 2031")
    for ticker in ("GA", "GC"):
        _ticker(tmp_store, "77", ticker)
    _directory(tmp_store, "GA", "G Inc Class A Common Stock")
    _directory(tmp_store, "GC", "G Inc Class C Capital Stock")
    _issuer_facts(tmp_store, X_OWNER)
    _issuer_facts(tmp_store, "SEC-CIK-0000000077")
    for offset, trade_date in enumerate(_DATES):
        _bar(tmp_store, X_OWNER, "XA", trade_date, 100.0 + offset / 100, shares=10_000_000)
        _bar(tmp_store, "TBLTICKERHISTORY-50", "XA-PB", trade_date, 25.0, shares=4_000_000)
        _bar(tmp_store, "SEC-CIK-0000000077", "GA", trade_date, 70.0 + offset / 100, shares=5_000_000)
        _bar(tmp_store, "TBLTICKERHISTORY-51", "GC", trade_date, 69.0 + offset / 100, shares=6_000_000)
    _dei(tmp_store, X_OWNER, X_CIK, 10_000_000, dt.datetime(2020, 1, 15, 21), "x-dei")
    _dei(tmp_store, "SEC-CIK-0000000077", "77", 11_000_000, dt.datetime(2020, 1, 15, 21), "g-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())

    def visible(line):
        return [row for row in _rows(tmp_store, line) if row[0] >= _FUNDAMENTALS_VISIBLE]

    # X's common line keeps DEI and every multiple; its preferred line is linked but never valued.
    common = visible(X_OWNER)
    assert {row[5] for row in common} == {"dei"} and all(row[1] is not None and row[8] is not None for row in common)
    preferred = visible("TBLTICKERHISTORY-50")
    assert preferred and all(row[3] is not None and row[1] is None and row[5] == "archive" for row in preferred)
    # G's two common classes: issuer DEI never prices one class; the 5M + 6M class counts
    # reconcile to the 11M issuer total, so both lines carry the class-sum issuer cap.
    ga, gc = visible("SEC-CIK-0000000077"), visible("TBLTICKERHISTORY-51")
    assert ga and len(ga) == len(gc)
    for a, c in zip(ga, gc, strict=True):
        assert (a[5], a[4], c[5], c[4]) == ("class_sum", 5_000_000, "class_sum", 6_000_000)
        assert a[6] == c[6] == pytest.approx(a[7] * 5_000_000 + c[7] * 6_000_000)
        assert a[1] is not None and a[1] == pytest.approx(c[1])
    report = owner_bridge_report(tmp_store)
    assert report["withheld_multi_common_class"] == 2 and report["withheld_non_common_line"] == 1
    assert report["multi_line_issuers"] == 1
    assert report["non_common_tickers_by_class"] == {"ETN": 1, "note": 1, "preferred": 1, "warrant": 1}
    assert report["non_common_tickers_by_basis"] == {"directory_name": 3, "ticker_suffix": 1}
    assert report["unlisted_untraded_tickers_ignored"] == 1  # XOTC: issuer is listed, XOTC is not traded


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
    # R1e split-evidence lookup: the unresolved-CIK content id resolves to its single-class line,
    # never before the link's (modeled) availability.
    bridge = _bridge(tmp_store)
    (line,) = bridge.single_class_lines(unresolved, dt.date(2020, 3, 2))
    assert (line.price_security_id, line.link_method, line.identity_basis) == (
        "TBLTICKERHISTORY-11",
        "current_sec_ticker",
        "current_ticker_unverified",
    )
    assert bridge.single_class_lines(unresolved, dt.date(2019, 12, 31)) == ()
    assert bridge.single_class_lines(unresolved, dt.date(2020, 1, 2), as_of=dt.datetime(2020, 1, 2, 21)) == ()
    assert ("SEC-CIK-0000000077", "TBLTICKERHISTORY-11") in {row[:2] for row in bridge.single_class_link_rows()}


def _bridge(store):
    item_codes, metric_codes = _referenced_codes()
    return build_market_owner_bridge(
        store, item_codes=item_codes, metric_codes=metric_codes, derived_source=DERIVED_SOURCE_NAME
    )


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
    assert all(row[3] is None for row in rows if row[0] < first_linked)
    assert all(row[3] is not None for row in rows if row[0] >= first_linked)
    # Strict assigns no DEI and has no class basis yet: B never carries the issuer's 40M
    # count; unlinked it is its own vendor line, linked (two classes) it is withheld.
    assert {row[5] for row in rows if row[0] < first_linked} == {"archive"}
    assert {row[5] for row in rows if row[0] >= first_linked} == {"multiclass_unresolved"}
    assert all(row[4] is None and row[6] is None for row in rows if row[0] >= first_linked)
    assert all(row[3] is None for row in _rows(multiclass, DEAD_LINE, MARKET_DAILY_STRICT_SOURCE_NAME))


def test_strict_owner_key_split_of_one_cik_is_one_multi_line_issuer(multiclass):
    evidence = (
        _evidence(),
        _evidence(
            price_security_id=X_OWNER,
            owner_security_id="SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000042",
            share_class_symbol="XA",
            evidence_id="ev-a",
        ),
    )
    options = MarketDailyOptions(owner_mode="strict", source=MARKET_DAILY_STRICT_SOURCE_NAME)
    refresh_market_daily_metrics(multiclass, options, owner_evidence=evidence)
    for line in (X_OWNER, B_LINE):
        linked = [row for row in _rows(multiclass, line, MARKET_DAILY_STRICT_SOURCE_NAME) if row[3] is not None]
        assert linked and {row[5] for row in linked} == {"multiclass_unresolved"}
        assert all(row[1] is None and row[2] is None and row[6] is None for row in linked)
    report = owner_bridge_report(multiclass, source=MARKET_DAILY_STRICT_SOURCE_NAME)
    assert report["linked_lines"] == 2 and report["multi_line_issuers"] == 1
    assert report["dei_shares_withheld_lines"] == 2 and report["valuation_withheld_lines"] == 2


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
    # Two listed classes: no single-class price line carries X's split evidence.
    assert bridge.single_class_lines(X_OWNER, dt.date(2020, 3, 2)) == ()

    snapshot_sql = "SELECT * EXCLUDE (source_loaded_at) FROM market_daily_metrics ORDER BY security_id, trade_date"
    refresh_market_daily_metrics(multiclass, MarketDailyOptions(batch_size=1))
    per_owner = multiclass.con.execute(snapshot_sql).fetchall()
    refresh_market_daily_metrics(multiclass, MarketDailyOptions(batch_size=500))
    assert multiclass.con.execute(snapshot_sql).fetchall() == per_owner


def test_reconstructed_rules_ambiguity_and_symbol_normalization():
    day = dt.date(2020, 1, 2)
    long_gone = day - dt.timedelta(days=400)
    old_snapshot, new_snapshot = dt.datetime(2025, 1, 1), dt.datetime(2026, 9, 20)
    lines = [
        PriceLine("TBLTICKERHISTORY-1", "BF.B", day, day, 1),
        PriceLine("TBLTICKERHISTORY-2", "DUP", day, day, 1),
        PriceLine("SEC-CIK-0000000005", "GONE", day, day, 1),
        PriceLine("LEGACY-1", "AAA", day, day, 1),
        PriceLine("TBLTICKERHISTORY-3", "AAA2", day, day, 1),
        # Reuse paths that must not link: symbol-keyed vendor lines, and a stale SEC-id line
        # whose CIK's current ticker is held by another line.
        PriceLine("TBLTICKERHISTORY-SYMBOL-SK-VENDOR-MISSING", "SK", day, day, 1),
        PriceLine("TBLTICKERHISTORY-40", "SK2", day, day, 1, symbol_keyed=True),
        PriceLine("SEC-CIK-0000014693", "BFOLD", long_gone, long_gone, 5),
        PriceLine("TBLTICKERHISTORY-4", "RE", day, day, 1),
    ]
    tickers = [
        ("14693", "BF-B", None),
        ("1", "DUP", None),
        ("2", "DUP", None),
        ("55", "SK", None),
        ("56", "SK2", None),
        # A CIK that dropped out of the SEC file leaves a stale row for a ticker now reused.
        ("3", "RE", old_snapshot),
        ("4", "RE", new_snapshot),
    ]
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
    assert by_line["LEGACY-1"].identity_basis == "shared_security_id_unverified"
    assert by_line["TBLTICKERHISTORY-3"].unlinked_reason == "no_current_ticker"
    assert by_line["TBLTICKERHISTORY-SYMBOL-SK-VENDOR-MISSING"].unlinked_reason == "symbol_keyed_line"
    assert by_line["TBLTICKERHISTORY-40"].unlinked_reason == "symbol_keyed_line"
    assert by_line["SEC-CIK-0000014693"].unlinked_reason == "superseded_cik_line"
    assert by_line["TBLTICKERHISTORY-4"].owner_security_id == "SEC-CIK-0000000004"
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


# ---------------------------------------------------------------------------
# A8: market-cap share, class, ADR, split and currency basis
# ---------------------------------------------------------------------------

_TWO_A, _TWO_B = "SEC-CIK-0000000101", "TBLTICKERHISTORY-101"
_ISSUER_CLOCK = dt.datetime(2020, 1, 15, 21)


def _visible(store, line, source=MARKET_DAILY_SOURCE_NAME):
    return [row for row in _rows(store, line, source) if row[0] >= _FUNDAMENTALS_VISIBLE]


def _two_class(store, *, total, a_shares=10_000_000, b_shares=30_000_000):
    """Plan fixture: class A $100 x 10M, class B $50 x 30M, one issuer with two listed classes."""
    seed_derived_metric_definitions(store)
    _ticker(store, "101", "TCA")
    _ticker(store, "101", "TCB")
    _issuer_facts(store, _TWO_A)
    for trade_date in _DATES:
        _bar(store, _TWO_A, "TCA", trade_date, 100.0, shares=a_shares)
        _bar(store, _TWO_B, "TCB", trade_date, 50.0, shares=b_shares)
    if total == "per_class_dei":  # both counts in one 10-K cover
        _dei(store, _TWO_A, "101", 10_000_000, _ISSUER_CLOCK, "tc-a", "tc-10k", "A")
        _dei(store, _TWO_A, "101", 30_000_000, _ISSUER_CLOCK, "tc-b", "tc-10k", "B")
    elif total in ("dei_total", "dei_total_unlisted_class"):
        count = 40_000_000 if total == "dei_total" else 43_000_000  # a 3M unlisted third class
        _dei(store, _TWO_A, "101", count, _ISSUER_CLOCK, "tc-t")
    elif total == "gaap_total":  # no DEI (dimensional covers); the balance-sheet total only
        _dei(
            store,
            _TWO_A,
            "101",
            40_000_000,
            _ISSUER_CLOCK,
            "tc-g",
            taxonomy="us-gaap",
            concept="CommonStockSharesOutstanding",
            effective=dt.date(2019, 12, 31),
        )
    refresh_derived_metrics(store, DerivedMetricsOptions())


@pytest.mark.parametrize("total", ["per_class_dei", "dei_total", "gaap_total"])
def test_two_class_issuer_cap_is_the_class_sum_never_one_price_times_the_total(tmp_store, total):
    _two_class(tmp_store, total=total)
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    a_rows, b_rows = _rows(tmp_store, _TWO_A), _rows(tmp_store, _TWO_B)
    for a, b in zip(a_rows, b_rows, strict=True):
        assert 4_000_000_000 not in (a[6], b[6])  # never $100 x 40M
        if a[0] < _DEI_VISIBLE:
            assert a[5] == b[5] == "multiclass_unresolved" and a[6] is None and b[6] is None
            continue
        # sum(price_i x class shares_i) = 100 x 10M + 50 x 30M, on both class lines.
        assert (a[5], a[4], b[5], b[4]) == ("class_sum", 10_000_000, "class_sum", 30_000_000)
        assert a[6] == b[6] == pytest.approx(2_500_000_000)
    visible_a, visible_b = _visible(tmp_store, _TWO_A), _visible(tmp_store, _TWO_B)
    assert visible_a and len(visible_a) == len(visible_b)
    assert all(a[1] is not None and a[1] == pytest.approx(b[1]) for a, b in zip(visible_a, visible_b, strict=True))
    report = owner_bridge_report(tmp_store)["share_basis"]
    assert report["cross_class_share_basis_rows"] == 0
    assert report["class_sum_rows"] == 2 * sum(1 for row in a_rows if row[0] >= _DEI_VISIBLE)
    # A refresh scoped to one class line computes its whole issuer group (same rows).
    before = _rows(tmp_store, _TWO_B)
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions(security_ids=(_TWO_B,)))
    assert _rows(tmp_store, _TWO_B) == before


@pytest.mark.parametrize(
    ("total", "a_shares", "b_shares"),
    [
        ("per_class_dei", 40_000_000, 40_000_000),  # vendor repeats the issuer total on each class line
        ("dei_total", 10_000_000, None),  # one class has no count
        ("none", 10_000_000, 30_000_000),  # no issuer total proves the counts are per class
        ("dei_total_unlisted_class", 10_000_000, 30_000_000),  # 40M of 43M: an unlisted class is missing
    ],
)
def test_unevidenced_class_counts_withhold_the_issuer_cap(tmp_store, total, a_shares, b_shares):
    _two_class(tmp_store, total=total, a_shares=a_shares, b_shares=b_shares)
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    for line in (_TWO_A, _TWO_B):
        rows = _rows(tmp_store, line)
        assert {row[5] for row in rows} == {"multiclass_unresolved"}
        assert all(row[4] is None and row[6] is None and row[1] is None and row[2] is None for row in rows)
    report = owner_bridge_report(tmp_store)["share_basis"]
    assert report["multiclass_unresolved_rows"] == 2 * len(_DATES) and report["cross_class_share_basis_rows"] == 0


_ADR_NAME = "Foo Holdings Limited American Depositary Shares, each representing ten ordinary shares"


@pytest.mark.parametrize(
    ("vendor", "expected"),
    [(100_000_000, "archive_ads"), (None, "adr_ratio_unresolved"), (1_000_000_000, "adr_ratio_unresolved")],
)
def test_adr_line_uses_the_ads_vendor_count_never_the_ordinary_dei(tmp_store, vendor, expected):
    """1 ADS = 10 ordinary; DEI 1e9 ordinary; the vendor reports 1e8 ADS (or nothing, or ordinaries)."""
    seed_derived_metric_definitions(tmp_store)
    _ticker(tmp_store, "202", "FOO")
    _directory(tmp_store, "FOO", _ADR_NAME)
    _issuer_facts(tmp_store, "SEC-CIK-0000000202")
    for trade_date in _DATES:
        _bar(tmp_store, "TBLTICKERHISTORY-202", "FOO", trade_date, 20.0, shares=vendor)
    _dei(tmp_store, "SEC-CIK-0000000202", "202", 1_000_000_000, _ISSUER_CLOCK, "foo-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = [row for row in _rows(tmp_store, "TBLTICKERHISTORY-202") if row[0] >= _DEI_VISIBLE]
    assert rows and {row[5] for row in rows} == {expected}
    # Never the ordinary-share basis: $20 x 1e9 ordinary shares.
    assert all(row[4] != 1_000_000_000 or row[5] != "dei" for row in _rows(tmp_store, "TBLTICKERHISTORY-202"))
    if expected == "archive_ads":
        assert all(row[4] == 100_000_000 and row[6] == pytest.approx(2_000_000_000) for row in rows)
        assert all(row[1] is not None for row in _visible(tmp_store, "TBLTICKERHISTORY-202"))
    else:
        # No vendor count, or one whose 10-ordinary-per-ADS equivalent contradicts the DEI.
        assert all(row[4] is None and row[6] is None and row[1] is None for row in rows)
    item_codes, metric_codes = _referenced_codes()
    bridge = build_market_owner_bridge(
        tmp_store, item_codes=item_codes, metric_codes=metric_codes, derived_source=DERIVED_SOURCE_NAME
    )
    (row,) = [row for row in bridge.rows if row.linked]
    assert (row.share_basis, row.adr_ratio, row.dei_shares_eligible) == ("adr", 10.0, False)


@pytest.mark.parametrize("vendor_lag_days", [0, 7])
def test_split_after_the_latest_10q_never_halves_or_doubles_the_cap(tmp_store, vendor_lag_days):
    """2:1 split on 2020-04-01 after the 10-Q (cover 2020-02-10, filed 2020-02-14, 100M shares)."""
    seed_derived_metric_definitions(tmp_store)
    owner = "SEC-CIK-0000000303"
    _ticker(tmp_store, "303", "SPL")
    _issuer_facts(tmp_store, owner)
    split = dt.date(2020, 4, 1)
    vendor_update = split + dt.timedelta(days=vendor_lag_days)
    for offset, trade_date in enumerate(_DATES):
        price = 100.0 + offset / 10
        close, adj = (price, price / 2) if trade_date < split else (price / 2, price / 2)
        shares = 100_000_000 if trade_date < vendor_update else 200_000_000
        _bar(tmp_store, owner, "SPL", trade_date, close, shares=shares, adj=adj)
    _dei(tmp_store, owner, "303", 100_000_000, dt.datetime(2020, 2, 14, 21), "spl", effective=dt.date(2020, 2, 10))
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = {row[0]: row for row in _rows(tmp_store, owner)}
    pre = [row for day, row in rows.items() if dt.date(2020, 2, 14) <= day < split]
    post = [row for day, row in rows.items() if day >= split]
    assert pre and {row[5] for row in pre} == {"dei"}
    lagged = [row for row in post if row[0] < vendor_update]
    assert all(row[5] == "split_unresolved" and row[6] is None and row[1] is None for row in lagged)
    absorbed = [row for row in post if row[0] >= vendor_update]
    assert absorbed and {row[5] for row in absorbed} == {"archive_split_adjusted"}
    assert all(row[4] == 200_000_000 and row[1] is not None for row in absorbed)
    # Continuous within 5 % across the split; never a halved cap.
    assert absorbed[0][6] == pytest.approx(pre[-1][6], rel=0.05)
    caps = [row[6] for row in sorted(rows.values()) if row[6] is not None]
    assert all(abs(later / earlier - 1) < 0.05 for earlier, later in pairwise(caps))
    report = owner_bridge_report(tmp_store)["share_basis"]
    assert report["split_unresolved_rows"] == len(lagged)


_COVER, _FILED = dt.date(2020, 4, 17), dt.datetime(2020, 5, 1, 21)


def test_vendor_run_starting_on_the_cover_date_is_unknown_until_the_filing(tmp_store):
    """AAPL shape: the vendor starts its 15,204,137,000 run on the 10-Q cover date (04-17), two
    weeks before the filing is accepted (05-01 21:00); the bars between keep the prior run."""
    seed_derived_metric_definitions(tmp_store)
    owner = "SEC-CIK-0000320193"
    _ticker(tmp_store, "320193", "AAPL")
    _issuer_facts(tmp_store, owner)
    for trade_date in _DATES:
        shares = 15_334_082_000 if trade_date < _COVER else 15_204_137_000
        _bar(tmp_store, owner, "AAPL", trade_date, 300.0, shares=shares)
    _dei(tmp_store, owner, "320193", 15_204_137_000, _FILED, "aapl-10q", effective=_COVER)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = _rows(tmp_store, owner)
    before = [row for row in rows if row[0] < _FILED.date()]
    after = [row for row in rows if row[0] >= _FILED.date()]
    assert [row for row in before if row[0] >= _COVER]  # the look-ahead window is covered
    assert {(row[5], row[4]) for row in before} == {("archive", 15_334_082_000)}
    assert after and {(row[5], row[4]) for row in after} == {("dei", 15_204_137_000)}
    assert all(row[6] == pytest.approx(300.0 * row[4]) for row in rows)


def test_unmatched_vendor_run_is_known_only_after_the_modeled_lag(tmp_store):
    """Celgene shape (2012: 438,810 thousand = 438.81M shares): a vendor count change that no DEI
    cover count explains is used only ARCHIVE_MODELED_LAG_DAYS after its run starts."""
    seed_derived_metric_definitions(tmp_store)
    owner = "SEC-CIK-0000816284"
    _ticker(tmp_store, "816284", "CELG")
    _issuer_facts(tmp_store, owner)
    change = dt.date(2020, 3, 2)
    for trade_date in _DATES:
        _bar(tmp_store, owner, "CELG", trade_date, 76.0, shares=438_810_000 if trade_date < change else 424_000_000)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    known = dt.datetime.combine(change, dt.time(22)) + dt.timedelta(days=ARCHIVE_MODELED_LAG_DAYS)
    rows = _rows(tmp_store, owner)
    assert {row[5] for row in rows} == {"archive"}
    for row in rows:
        expected = 424_000_000 if dt.datetime.combine(row[0], dt.time(22)) >= known else 438_810_000
        assert row[4] == expected and row[6] == pytest.approx(76.0 * expected)
    assert any(row[4] == 424_000_000 for row in rows) and any(row[4] == 438_810_000 for row in rows[40:])


def test_class_count_matched_to_a_per_class_dei_enters_the_class_sum_at_its_filing(tmp_store):
    """Class A's vendor count moves to 11M on the 10-Q cover date; the per-class DEI filed two weeks
    later carries it. The class sum stays $2.5B until the filing, then $2.6B (never early)."""
    seed_derived_metric_definitions(tmp_store)
    _ticker(tmp_store, "101", "TCA")
    _ticker(tmp_store, "101", "TCB")
    _issuer_facts(tmp_store, _TWO_A)
    for trade_date in _DATES:
        _bar(tmp_store, _TWO_A, "TCA", trade_date, 100.0, shares=10_000_000 if trade_date < _COVER else 11_000_000)
        _bar(tmp_store, _TWO_B, "TCB", trade_date, 50.0, shares=30_000_000)
    _dei(tmp_store, _TWO_A, "101", 10_000_000, _ISSUER_CLOCK, "tc-a", "tc-10k", "A")
    _dei(tmp_store, _TWO_A, "101", 30_000_000, _ISSUER_CLOCK, "tc-b", "tc-10k", "B")
    _dei(tmp_store, _TWO_A, "101", 11_000_000, _FILED, "tc-a2", "tc-10q", "A", effective=_COVER)
    _dei(tmp_store, _TWO_A, "101", 30_000_000, _FILED, "tc-b2", "tc-10q", "B", effective=_COVER)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = [row for row in _rows(tmp_store, _TWO_A) if row[0] >= _DEI_VISIBLE]
    assert [row for row in rows if _COVER <= row[0] < _FILED.date()]
    for row in rows:
        a_shares = 11_000_000 if row[0] >= _FILED.date() else 10_000_000
        assert (row[5], row[4]) == ("class_sum", a_shares)
        assert row[6] == pytest.approx(100.0 * a_shares + 50.0 * 30_000_000)


@pytest.mark.parametrize("k", [2.0, 0.1, 1.37])
def test_a_vendor_share_run_never_spans_a_split_and_an_adr_without_ratio_is_withheld(tmp_store, k):
    """A8 fix C1 (rv-a8 probe shape, forward 2:1 and reverse 1:10): vendor 100M, then an unmatched
    104M run from 02-03, a split on 03-02 the vendor absorbs on 03-09 (104M x k). On a linked line
    without DEI and on an unlinked line, the pre-split count never prices a post-split bar: the
    bars are NULL (split_pending_share_update) until the split-derived run is known. That run's
    count reveals the unmatched 104M run's, so it is known no earlier than that run (A8 follow-up):
    on the unlinked line 150 days after 02-03 (after the fixture), on the linked line at max(90-day
    modeled lag, 05-03; the 02-03 bar's own late arrival, 06-01 12:00): a run is never known
    before the bars that define it. N1: an inexact 1.37 factor step (spin-off / special dividend;
    the count does not move) is no split and opens no window. I1: an ADR line whose name states no
    ADS ratio is withheld (adr_ratio_unknown)."""
    spin_off = k == 1.37
    seed_derived_metric_definitions(tmp_store)
    linked, unlinked, adr = "SEC-CIK-0000000501", "TBLTICKERHISTORY-601", "TBLTICKERHISTORY-502"
    _ticker(tmp_store, "501", "SPLA")
    _ticker(tmp_store, "502", "FOOB")
    _directory(tmp_store, "FOOB", "Foob Holdings Limited American Depositary Shares")
    split, absorbed, run_start = dt.date(2020, 3, 2), dt.date(2020, 3, 9), dt.date(2020, 2, 3)
    late_bar = dt.datetime(2020, 6, 1, 12)
    known = {linked: dt.date(2020, 6, 1), unlinked: None}  # the split-derived run's first known session
    for trade_date in _DATES:
        close = 50.0 if trade_date < split else 50.0 / k
        shares = 100_000_000 if trade_date < run_start else 104_000_000
        shares = round(104_000_000 * k) if trade_date >= absorbed and not spin_off else shares
        for line, symbol in ((linked, "SPLA"), (unlinked, "SPLX")):
            _bar(tmp_store, line, symbol, trade_date, close, shares=shares, adj=50.0 / k)
        _bar(tmp_store, adr, "FOOB", trade_date, 20.0, shares=50_000_000)
    tmp_store.con.execute("UPDATE equity_daily_bars SET available_at = ? WHERE security_id = ? AND trade_date = ?",
                          [late_bar, linked, run_start])
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())

    def pending(line, day):
        return split <= day and (known[line] is None or day < known[line])

    for line in (linked, unlinked):
        rows = _rows(tmp_store, line)
        if spin_off:  # the known count keeps pricing the line through the distribution
            assert rows and {row[5] for row in rows} == {"archive"} and all(row[6] is not None for row in rows)
            continue
        window = [row for row in rows if pending(line, row[0])]
        assert [row for row in window if row[0] >= absorbed] and all(
            (row[5], row[4], row[6]) == ("split_pending_share_update", None, None) for row in window
        )
        before = [row for row in rows if row[0] < split]
        after = [row for row in rows if row[0] >= split and not pending(line, row[0])]
        # The unmatched 104M run is not known yet before the split (modeled lag).
        assert {(row[5], row[4]) for row in before} == {("archive", 100_000_000)}
        assert bool(after) == (known[line] is not None)
        assert all((row[5], row[4]) == ("archive", round(104_000_000 * k)) for row in after)
        caps = [row[6] for row in rows if row[6] is not None]
        assert all(abs(later / earlier - 1) < 0.05 for earlier, later in pairwise(caps))  # never x2 or x10
    adr_rows = _rows(tmp_store, adr)
    assert adr_rows and all((row[5], row[4], row[6], row[1]) == ("adr_ratio_unknown", None, None, None)
                            for row in adr_rows)
    report = owner_bridge_report(tmp_store)["share_basis"]
    pending_rows = sum(pending(line, day) for line in (linked, unlinked) for day in _DATES)
    assert report["split_pending_share_update_rows"] == (0 if spin_off else pending_rows)
    assert report["adr_ratio_unknown_rows"] == len(_DATES)
    assert spin_off or report["rows_by_share_clock"]["split_derived"] > 0
    # The read relation other readers join (R2d) states the same clock, window and basis per bar.
    item_codes, metric_codes = _referenced_codes()
    bridge = build_market_owner_bridge(
        tmp_store, item_codes=item_codes, metric_codes=metric_codes, derived_source=DERIVED_SOURCE_NAME
    )
    sql, params = vendor_share_state_query(bridge, [linked, unlinked, adr])
    state = {(row[0], row[1]): row for row in tmp_store.con.execute(sql, params).fetchall()}
    assert state[(adr, _DATES[-1])][10:] == ("adr", None, "adr_ratio_unknown")
    for line in (linked, unlinked):
        assert all(state[(line, day)][9] != spin_off for day in _DATES if split <= day < absorbed)  # split_pending
        if not spin_off:
            assert all(state[(line, day)][9] == pending(line, day) for day in _DATES if day >= split)
    if not spin_off:
        assert {state[(linked, day)][3] for day in _DATES if day >= known[linked]} == {"split_derived"}
        assert state[(linked, known[linked])][6] == late_bar  # pit_available_at: the late bar, not 03-09
        assert state[(unlinked, _DATES[-1])][3] == "first_run"


def _valuation_facts(store, owner, unit=None):
    _issuer_facts(store, owner)
    for period_end in _QUARTERS:
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=40), dt.time(21))
        _fact(store, owner, "ebitda_standardised", "quarterly", period_end, 200_000.0, available_at)
        _fact(store, owner, "total_debt", "instant", period_end, 1_000_000.0, available_at)
        _fact(store, owner, "cash_and_st_investments", "instant", period_end, 300_000.0, available_at)
    if unit is not None:
        store.con.execute(
            "UPDATE fundamental_standardized SET unit = ?, unit_type = 'monetary' WHERE security_id = ?", [unit, owner]
        )


def test_non_usd_reporter_keeps_market_cap_but_no_valuation_and_is_counted(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for cik, ticker, unit in (("404", "USDX", "USD"), ("405", "CADX", "CAD")):
        owner = f"SEC-CIK-0000000{cik}"
        _ticker(tmp_store, cik, ticker)
        _valuation_facts(tmp_store, owner, unit)
        for trade_date in _DATES:
            _bar(tmp_store, owner, ticker, trade_date, 30.0, shares=1_000_000)
        _dei(tmp_store, owner, cik, 1_000_000, _ISSUER_CLOCK, f"{ticker}-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    # 0328's nullable column: the writer labels the row once the column exists.
    tmp_store.con.execute("ALTER TABLE market_daily_metrics ADD COLUMN IF NOT EXISTS currency_status VARCHAR")
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    labels = dict(tmp_store.con.execute(
        "SELECT security_id, list_distinct(list(coalesce(currency_status, 'usd'))) FROM market_daily_metrics GROUP BY 1"
    ).fetchall())
    assert labels == {"SEC-CIK-0000000404": ["usd"], "SEC-CIK-0000000405": ["non_usd"]}

    def valuations(owner):
        return tmp_store.con.execute(
            "SELECT market_cap, pe_ttm, ev_ebitda, ps_ttm FROM market_daily_metrics "
            "WHERE security_id = ? AND trade_date >= ? ORDER BY trade_date",
            [owner, _FUNDAMENTALS_VISIBLE],
        ).fetchall()

    usd, cad = valuations("SEC-CIK-0000000404"), valuations("SEC-CIK-0000000405")
    assert usd and all(None not in row for row in usd)
    assert cad and all(row[0] == pytest.approx(30_000_000) and row[1:] == (None, None, None) for row in cad)
    report = owner_bridge_report(tmp_store)
    assert report["share_basis"]["currency_withheld_rows"] == {"non_usd": len(_DATES)}
    assert report["share_basis"]["non_usd_null_rows"] == len(_DATES)
    assert report["currency_evidence_content_ids"] == 1


def test_currency_status_intervals_follow_the_latest_monetary_filing():
    def event(member, period_end, at, usd=False, foreign=False, unknown=False):
        return CurrencyEvent(member, period_end, at, usd, foreign, unknown)

    cad_q1 = event("O", dt.date(2019, 3, 31), dt.datetime(2019, 5, 10), foreign=True)
    usd_q1 = event("U", dt.date(2020, 3, 31), dt.datetime(2020, 5, 10), usd=True)
    old_amendment = event("O", dt.date(2019, 3, 31), dt.datetime(2020, 6, 1), foreign=True)
    mixed_q2 = event("U", dt.date(2020, 6, 30), dt.datetime(2020, 8, 10), usd=True, foreign=True)
    bridge = MarketOwnerBridge(
        mode="reconstructed",
        lines={},
        rows=(),
        owner_members={"O": ("O", "U")},
        currency_events={"O": (cad_q1, old_amendment), "U": (usd_q1, mixed_q2)},
    )
    intervals = bridge.currency_intervals([SimpleNamespace(owner_security_id="O")])
    # CAD until the USD filing of a newer period; an amendment of the older CAD period does not
    # reopen it; a convenience translation (USD + foreign in one filing) is "mixed".
    assert intervals == [
        ("O", dt.datetime(2019, 5, 10), dt.datetime(2020, 5, 10), "non_usd"),
        ("O", dt.datetime(2020, 8, 10), None, "mixed"),
    ]


def test_sibling_class_missing_from_the_sec_map_withholds_the_listed_line(tmp_store):
    """(i) CWEN/CWEN-A: the SEC map lists only CWEN; the CWEN.A line is unlinked; one issuer
    DEI count (153M) within 1.5x of CWEN's own 118M, so only class evidence can catch it."""
    seed_derived_metric_definitions(tmp_store)
    owner = "SEC-CIK-0000000091"
    _ticker(tmp_store, "91", "CWEN")
    _issuer_facts(tmp_store, owner)
    for trade_date in _DATES:
        _bar(tmp_store, owner, "CWEN", trade_date, 30.0, shares=118_000_000)
        _bar(tmp_store, "TBLTICKERHISTORY-91", "CWEN.A", trade_date, 28.0, shares=35_000_000)
    _dei(tmp_store, owner, "91", 153_000_000, _ISSUER_CLOCK, "cwen-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = _rows(tmp_store, owner)
    assert {row[5] for row in rows} == {"multiclass_unresolved"}
    assert all(row[4] is None and row[6] is None and row[1] is None and row[2] is None for row in rows)
    report = owner_bridge_report(tmp_store)
    assert report["unlinked_by_reason"] == {"no_current_ticker": 1} and report["sibling_class_lines"] == 1


@pytest.mark.parametrize("with_dei", [True, False])
def test_sibling_class_delisted_before_the_snapshot_withholds_the_dual_class_period(tmp_store, with_dei):
    """(ii)/(iii) DISCA/DISCK before WBD: the surviving line (DISCA, now WBD) and a delisted
    DISCK line traded together until the classes collapsed on 2020-04-01."""
    seed_derived_metric_definitions(tmp_store)
    owner, survivor, delisted = "SEC-CIK-0000000092", "TBLTICKERHISTORY-61", "TBLTICKERHISTORY-62"
    _ticker(tmp_store, "92", "WBD")
    _issuer_facts(tmp_store, owner)
    collapse = dt.date(2020, 4, 1)
    for trade_date in _DATES:
        if trade_date < collapse:
            _bar(tmp_store, survivor, "DISCA", trade_date, 30.0, shares=150_000_000)
            _bar(tmp_store, delisted, "DISCK", trade_date, 29.0, shares=350_000_000)
        else:
            _bar(tmp_store, survivor, "WBD", trade_date, 30.0, shares=500_000_000)
    if with_dei:
        _dei(tmp_store, owner, "92", 500_000_000, _ISSUER_CLOCK, "wbd-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = _rows(tmp_store, survivor)
    dual = [row for row in rows if row[0] < collapse]
    # Neither the issuer DEI (500M x one class's price) nor the line's own 150M (a partial
    # issuer cap against issuer earnings) prices the dual-class period.
    assert {row[5] for row in dual} == {"multiclass_unresolved"}
    assert all(row[4] is None and row[6] is None and row[1] is None and row[2] is None for row in dual)
    single = [row for row in rows if row[0] >= collapse]
    assert {row[5] for row in single} == {"dei" if with_dei else "archive"}
    assert all(row[1] is not None for row in single)
    if with_dei:
        assert all(row[4] == 500_000_000 for row in single)
    else:
        # No DEI explains the line's new 500M run: it is known only after the modeled lag;
        # until then the bar carries the last known run (150M).
        known = dt.datetime.combine(collapse, dt.time(22)) + dt.timedelta(days=ARCHIVE_MODELED_LAG_DAYS)
        for row in single:
            assert row[4] == (500_000_000 if dt.datetime.combine(row[0], dt.time(22)) >= known else 150_000_000)
        assert any(row[4] == 500_000_000 for row in single)
    assert _rows(tmp_store, delisted) and all(row[3] is None for row in _rows(tmp_store, delisted))
    assert owner_bridge_report(tmp_store)["sibling_class_lines"] == 1
    # R1e split-evidence lookup: no single-class price line while two classes traded.
    bridge = _bridge(tmp_store)
    assert bridge.single_class_lines(owner, dt.date(2020, 3, 2)) == ()
    assert [row.price_security_id for row in bridge.single_class_lines(owner, dt.date(2020, 4, 15))] == [survivor]


@pytest.mark.parametrize(("dei", "expected"), [(40_000_000, "dei_archive_conflict"), (10_300_000, "dei")])
def test_dei_archive_ratio_guard_rejects_a_foreign_share_basis(tmp_store, dei, expected):
    """(iv) No class evidence at all: a DEI count 4x the line's own vendor count on both the
    DEI effective date and the bar date is not this line's share basis (limit 1.5x)."""
    seed_derived_metric_definitions(tmp_store)
    owner = "SEC-CIK-0000000094"
    _ticker(tmp_store, "94", "RAT")
    _issuer_facts(tmp_store, owner)
    for trade_date in _DATES:
        _bar(tmp_store, owner, "RAT", trade_date, 50.0, shares=10_000_000)
    _dei(tmp_store, owner, "94", dei, _ISSUER_CLOCK, "rat-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = _rows(tmp_store, owner)
    assert {row[5] for row in rows if row[0] < _DEI_VISIBLE} == {"archive"}
    guarded = [row for row in rows if row[0] >= _DEI_VISIBLE]
    assert {row[5] for row in guarded} == {expected}
    if expected == "dei":
        assert all(row[4] == dei and row[1] is not None for row in guarded if row[0] >= _FUNDAMENTALS_VISIBLE)
    else:
        assert all(row[4] is None and row[6] is None and row[1] is None for row in guarded)
        assert owner_bridge_report(tmp_store)["share_basis"]["dei_archive_conflict_rows"] == len(guarded)


def test_len_class_b_is_a_share_class_through_class_evidence(tmp_store):
    """(v) A2 types "Lennar Corporation Class B" common_unverified; A8's class evidence
    (the name's share class, the LEN/LEN-B ticker pair) makes LEN dual-class."""
    seed_derived_metric_definitions(tmp_store)
    owner = "SEC-CIK-0000000093"
    _ticker(tmp_store, "93", "LEN")
    _ticker(tmp_store, "93", "LEN-B")
    _directory(tmp_store, "LEN", "Lennar Corporation Class A Common Stock")
    _directory(tmp_store, "LEN.B", "Lennar Corporation Class B")
    _issuer_facts(tmp_store, owner)
    for trade_date in _DATES:
        _bar(tmp_store, owner, "LEN", trade_date, 100.0, shares=250_000_000)
        _bar(tmp_store, "TBLTICKERHISTORY-93", "LEN.B", trade_date, 80.0, shares=30_000_000)
    _dei(tmp_store, owner, "93", 280_000_000, _ISSUER_CLOCK, "len-dei")
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    for line, own in ((owner, 250_000_000), ("TBLTICKERHISTORY-93", 30_000_000)):
        rows = [row for row in _rows(tmp_store, line) if row[0] >= _DEI_VISIBLE]
        assert rows and {row[5] for row in rows} == {"class_sum"}  # never DEI on LEN
        assert all(row[4] == own and row[6] == pytest.approx(100.0 * 250e6 + 80.0 * 30e6) for row in rows)
    # Without the directory's class wording, the LEN/LEN-B ticker pair alone is class evidence.
    classes = classify_sec_tickers(
        [("93", "LEN", None), ("93", "LEN-B", None)], {"LEN": "common", "LEN.B": "common_unverified"}
    )
    by_ticker = {item.ticker: item for item in classes}
    assert (by_ticker["LEN-B"].security_class, by_ticker["LEN-B"].basis) == ("class_share", "ticker_class_suffix")
    assert by_ticker["LEN-B"].counted_common


def test_class_sibling_symbols_and_ads_ratios():
    siblings = [("LEN", "LEN-B"), ("DISCA", "DISCK"), ("BRK-A", "BRK.B"), ("CWEN", "CWEN.A"), ("VIAC", "VIACA")]
    assert all(are_class_siblings(left, right) for left, right in siblings)
    strangers = [("GOOG", "GOOGL"), ("T", "TA"), ("LEN", "LEN"), ("BAC", "BAC-PL"), ("ABCD", "ABCDE")]
    assert not any(are_class_siblings(left, right) for left, right in strangers)
    assert parse_ads_ratio(_ADR_NAME) == 10.0
    assert parse_ads_ratio("Grupo Aeromexico ADS (each representing ten (10) Common Shares)") == 10.0
    assert parse_ads_ratio("X ADS, each representing one-half of one ordinary share") == 0.5
    assert parse_ads_ratio("X American Depositary Shares each representing 1/4 of an ordinary share") == 0.25
    assert parse_ads_ratio("Abivax SA - American Depositary Shares") is None
    assert parse_ads_ratio("Y Depositary Shares, each representing a 1/1,000th Interest in a Preferred") is None


def test_identity_row_columns_are_written_when_the_table_has_them(multiclass):
    """A9 adds nullable owner/identity columns; the writer fills them per row when present."""
    for column in ("owner_security_id", "identity_basis", "availability_basis", "link_method"):
        # Migration 0327 adds them; an older template gets them here.
        multiclass.con.execute(f"ALTER TABLE market_daily_metrics ADD COLUMN IF NOT EXISTS {column} VARCHAR")
    refresh_market_daily_metrics(multiclass, MarketDailyOptions())
    rows = multiclass.con.execute(
        "SELECT security_id, owner_security_id, identity_basis, availability_basis, link_method "
        "FROM market_daily_metrics GROUP BY ALL ORDER BY security_id"
    ).fetchall()
    assert rows == [
        (X_OWNER, X_OWNER, "current_ticker_unverified", "modeled", "current_sec_ticker"),
        (B_LINE, X_OWNER, "current_ticker_unverified", "modeled", "current_sec_ticker"),
        (DEAD_LINE, None, None, None, None),
    ]


def test_no_wall_clock_in_the_bridge_source():
    import atx_db.market_owner_bridge as module

    source = inspect.getsource(module)
    for forbidden in ("date.today", "utcnow", "Timestamp.now", "time.time", "datetime.now", "current_date"):
        assert forbidden not in source, forbidden


# ---------------------------------------------------------------------------
# P1: RI1 reconstructed history -> bridge
# ---------------------------------------------------------------------------

OLD_LINE = "TBLTICKERHISTORY-901"
OLD_CIK = "0000000501"
OLD_OWNER = "SEC-CIK-0000000501"


def _month_starts(first: dt.date, months: int) -> list[dt.date]:
    """The first weekday of each month (a sparse bar calendar keeps the fixture small)."""
    out = []
    for index in range(months):
        year, month = first.year + (first.month - 1 + index) // 12, (first.month - 1 + index) % 12 + 1
        out.append(_weekdays(dt.date(year, month, 1), 1)[0])
    return out


def _ri1_rows(first: dt.date, last: dt.date):
    """Genuine RI1 output for vendor line 901: 13 distinct quarterly cover counts the vendor count follows."""
    import duckdb

    from atx_db import identity_reconstruction as ir

    as_ofs = [dt.date(2014 + (index * 3) // 12, (index * 3) % 12 + 1, 20) for index in range(13)]
    facts = [
        ir.IssuerShareFact(OLD_CIK, "dei:EntityCommonStockSharesOutstanding", as_of, float(50_123_456 + i * 111_111),
                           f"a{i}", "10-Q", as_of + dt.timedelta(days=10))
        for i, as_of in enumerate(as_ofs)
    ]
    runs = [
        ir.VendorShareRun(901, int(fact.value // 1000), fact.as_of,
                          as_ofs[i + 1] - dt.timedelta(days=1) if i + 1 < len(as_ofs) else last)
        for i, fact in enumerate(facts)
    ]
    con = duckdb.connect(":memory:", config={"memory_limit": "128MB", "threads": 1})
    ir.stage_share_runs(con, runs)
    ir.stage_share_facts(con, facts)
    line = ir.VendorLine(901, OLD_LINE, first, last, 42, "OLDCO", (("OLDCO", first, last),))
    result = ir.reconstruct_issuer_links(con, [line], horizon=dt.date(2020, 12, 31), price_start=dt.date(2012, 3, 26))
    return result, result.evidence_rows(observed_at=dt.datetime(2026, 9, 20), run_id="p1-test")


def test_delisted_line_takes_its_reconstructed_owner_point_in_time_and_never_in_strict_mode(tmp_store):
    from atx_db.historical_identity import EVIDENCE_COLUMNS

    seed_derived_metric_definitions(tmp_store)
    dates = _month_starts(dt.date(2014, 1, 1), 42)  # 2014-01 .. 2017-06, delisted
    for trade_date in dates:
        _bar(tmp_store, OLD_LINE, "OLDCO", trade_date, 10.0)
    quarter_ends = [dt.date(2013, 12, 31)] + [
        dt.date(year, month, 31 if month in (3, 12) else 30) for year in (2014, 2015, 2016) for month in (3, 6, 9, 12)
    ]
    for index, period_end in enumerate(quarter_ends):
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=40), dt.time(21))
        _fact(tmp_store, OLD_OWNER, "revenue", "quarterly", period_end, 1_000_000.0 + index, available_at)
        _fact(tmp_store, OLD_OWNER, "net_income_to_common", "quarterly", period_end, 100_000.0, available_at)
        _fact(tmp_store, OLD_OWNER, "common_equity", "instant", period_end, 5_000_000.0, available_at)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    result, evidence_rows = _ri1_rows(dates[0], dates[-1])
    (link,) = result.links
    names = [name for name, _kind in EVIDENCE_COLUMNS]
    tmp_store.con.executemany(
        f"INSERT INTO security_identity_evidence ({', '.join(names)}) VALUES ({', '.join('?' for _ in names)})",
        [[row[name] for name in names] for row in evidence_rows],
    )

    # Reconstructed mode reads the RI1 row from security_identity_evidence (rule 5).
    bridge = _bridge(tmp_store)
    (row,) = [row for row in bridge.rows if row.price_security_id == OLD_LINE]
    assert (row.owner_security_id, row.identity_basis, row.availability_basis, row.link_method, row.tier) == (
        OLD_OWNER, "reconstructed_history", "modeled", "reconstructed_history_medium", "medium")
    assert (row.valid_from, row.valid_to, row.available_at, row.evidence_id) == (
        link.valid_from, link.valid_to, link.available_at, evidence_rows[0]["evidence_id"])
    detail = bridge.summary()
    assert (detail["unlinked_lines"], detail["linked_by_identity_basis"]) == (0, {"reconstructed_history": 1})
    # The bridge-level label names both bases; the delisted rule-5 line is not an A5 stale holder.
    assert (detail["identity_basis"], detail["stale_links"]) == (IDENTITY_BASIS_CURRENT_AND_RECONSTRUCTED, 0)
    assert (detail["reconstructed_history"]["linked_lines_by_tier"], detail["reconstructed_history"]["tier_filter"]) == (
        {"medium": 1}, "high_medium")

    # The link is never visible before its RI1 evidence clock (third count's 10-Q + 46h).
    assert link.available_at == dt.datetime(2014, 7, 31, 22)
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    visible = [panel[0] for panel in _rows(tmp_store, OLD_LINE) if panel[3] is not None]
    assert visible and min(visible) == dt.date(2014, 8, 1)
    assert all(dt.datetime.combine(day, dt.time(22)) >= link.available_at for day in visible)

    # Strict mode never consumes reconstructed evidence; the high-only sensitivity leaves it below tier.
    item_codes, metric_codes = _referenced_codes()
    strict = build_market_owner_bridge(tmp_store, mode="strict", evidence=result.owner_links(), item_codes=item_codes,
                                       metric_codes=metric_codes, derived_source=DERIVED_SOURCE_NAME)
    assert [row.unlinked_reason for row in strict.rows if row.price_security_id == OLD_LINE] == ["no_dated_evidence"]
    assert strict.rejected_evidence == {"evidence_not_verified_dated": 1}
    high_only = build_market_owner_bridge(tmp_store, item_codes=item_codes, metric_codes=metric_codes,
                                          derived_source=DERIVED_SOURCE_NAME,
                                          reconstruction_tiers=RECONSTRUCTION_TIERS_HIGH_ONLY)
    assert [row.unlinked_reason for row in high_only.rows if row.price_security_id == OLD_LINE] == [
        "reconstruction_below_tier"]


def _ri1(line, cik, valid, available_at, history, status="reconstructed"):
    return ReconstructedLinkEvidence(f"ev-{line}-{cik}", status, cik, None, line, valid[0], valid[1], available_at,
                                     history)


def test_reconstructed_conflicts_stay_unlinked_and_tiers_and_issuers_are_point_in_time():
    a, b, c, p = "TBLTICKERHISTORY-11", "TBLTICKERHISTORY-12", "TBLTICKERHISTORY-13", "TBLTICKERHISTORY-14"
    d = dt.date
    lines = [
        PriceLine(a, "AAA", d(2015, 1, 2), d(2016, 12, 30), 500),
        PriceLine(b, "BBB", d(2015, 1, 2), d(2016, 12, 30), 500),
        PriceLine(c, "CCC", d(2015, 6, 1), d(2016, 6, 29), 100),
        PriceLine(p, "AAAAP", d(2015, 1, 2), d(2016, 12, 30), 500),
    ]
    clock, c_clock = dt.datetime(2015, 7, 31, 22), dt.datetime(2015, 9, 1, 22)
    evidence = [
        # A: medium from its clock; a rival contests it from 2016-03-01 23:30 (after that day's cutoff).
        _ri1(a, "0000000701", (d(2015, 1, 2), d(2016, 12, 31)), clock,
             (("medium", clock), ("low", dt.datetime(2016, 3, 1, 23, 30)))),
        # B: an unresolved RI1 conflict -- both sides are evidence, neither is picked.
        _ri1(b, "0000000702", (d(2015, 1, 2), d(2016, 12, 31)), None, (), "conflicting"),
        _ri1(b, "0000000703", (d(2015, 1, 2), d(2016, 12, 31)), None, (), "conflicting"),
        # C: the same issuer as A on a second, shorter line overlapping A's link.
        _ri1(c, "0000000701", (d(2015, 6, 1), d(2016, 6, 30)), c_clock, (("medium", c_clock),)),
        # P: the same issuer's delisted Series A preferred (Nasdaq fifth letter P; not a current ticker),
        # fingerprinted to the issuer because the vendor carries the common count on it.
        _ri1(p, "0000000701", (d(2015, 1, 2), d(2016, 12, 31)), clock, (("medium", clock),)),
    ]
    rows, _members, _ambiguous, stats = classify_reconstructed_with_history(lines, (), {}, reconstructed=evidence)

    linked = [(row.price_security_id, row.valid_from, row.valid_to, row.tier, row.available_at, row.share_basis,
               row.withheld_reason) for row in rows if row.linked]
    assert linked == [
        # A is linked only while its tier in force is inside the filter (low is never auto-picked).
        (a, d(2015, 1, 2), d(2016, 3, 2), "medium", clock, "single", None),
        # C keeps only the days A does not hold (A's link is visible first): one issuer, one line per day.
        (c, d(2016, 3, 2), d(2016, 6, 30), "medium", c_clock, "single", None),
        # The preferred line is linked but withheld (no DEI, no valuation); it neither claims nor loses days.
        (p, d(2015, 1, 2), d(2016, 12, 31), "medium", clock, "withheld", "non_common_line"),
    ]
    assert [row.unlinked_reason for row in rows if row.price_security_id == b] == ["conflicting_reconstruction"]
    assert (stats["linked_lines_by_tier"], stats["segments_excluded_by_tier"], stats["unlinked_lines_by_reason"]) == (
        {"medium": 3}, {"low": 1}, {"conflicting_reconstruction": 1})
    assert (stats["linked_lines_non_common"], stats["linked_lines_never_visible"]) == ({"preferred": 1}, 0)
    assert stats["cik_day_dedupe"] == {
        "rule": "common_lines_only;current_ticker_first_then_first_visible_then_first_bar_then_id", "ciks": 1,
        "segments_trimmed": 1, "days_removed": 275, "lines_removed": 0}
