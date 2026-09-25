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
* class guard (until A8's class basis): an issuer with more than one current
  SEC ticker or concurrently linked line -- linked or not, strict owner-key
  splits included -- never pairs issuer DEI with one class line's price and
  gets NULL valuation multiples; strict mode assigns no DEI; a per-class DEI
  filing is never used as an issuer count.
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


def _dei(store, security_id, cik, count, available_at, key, accession="acc", share_class=None):
    store.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count, source_url, share_class
        ) VALUES (?, 'test', ?, ?, 'shares_outstanding', 'dei', 'EntityCommonStockSharesOutstanding',
                  'shares', 'instant', ?, ?, ?, ?, ?, 1, 1, true, ?, 'test', ?)
        """,
        [
            key,
            security_id,
            cik,
            available_at.date(),
            available_at.date(),
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
        # Class guard: the issuer-level DEI count (40M) is never paired with B's price;
        # market cap is B's own line value.
        assert shares_source == "archive" and shares == 30_000_000
        assert cap == pytest.approx(close * 30_000_000)
    # Two current SEC tickers: no line-cap / issuer-total multiples on either class until A8.
    for rows in (b_rows, list(a_rows.values())):
        assert all(row[1] is None and row[2] is None and row[8] is None and row[9] is None for row in rows)
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
    assert bridge["linked_by_identity_basis"] == {"current_ticker_unverified": 2}
    assert bridge["multi_line_issuers"] == 1
    assert bridge["dei_shares_withheld_lines"] == 2 and bridge["valuation_withheld_lines"] == 2
    assert bridge["ticker_snapshot_observed_at"] == "2026-09-20T12:00:00"
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
    # Never $100 x 40M: A keeps its own 10M line shares and no issuer multiple.
    assert {row[5] for row in a_rows} == {"archive"} and {row[4] for row in a_rows} == {10_000_000}
    assert all(row[6] == pytest.approx(row[7] * 10_000_000) for row in a_rows)
    assert all(row[1] is None and row[2] is None for row in a_rows)
    report = owner_bridge_report(tmp_store)
    assert report["linked_lines"] == 1 and report["unlinked_by_reason"] == {"no_current_ticker": 1}
    assert report["multi_line_issuers"] == 1 and report["valuation_withheld_lines"] == 1


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
    after = rows[dt.date(2020, 4, 16)]
    assert after[5] == "archive" and after[4] == 4_900_000
    assert after[1] is None and after[2] is None and after[6] == pytest.approx(after[7] * 4_900_000)


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
    assert all(row[3] is None for row in rows if row[0] < first_linked)
    assert all(row[3] is not None for row in rows if row[0] >= first_linked)
    # Strict assigns no DEI (no class basis yet): B never carries the issuer's 40M count.
    assert {row[5] for row in rows} == {"archive"}
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
        assert linked and {row[5] for row in linked} == {"archive"}
        assert all(row[1] is None and row[2] is None for row in linked)
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


def test_no_wall_clock_in_the_bridge_source():
    import atx_db.market_owner_bridge as module

    source = inspect.getsource(module)
    for forbidden in ("date.today", "utcnow", "Timestamp.now", "time.time", "datetime.now", "current_date"):
        assert forbidden not in source, forbidden
