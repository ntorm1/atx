"""Point-in-time contracts for the monthly research panel (task R2a)."""

from __future__ import annotations

import ctypes
import datetime as dt
import hashlib
import json
import math
import sys
import tracemalloc
from pathlib import Path

import duckdb
import pytest

from atx_db import _derived_pit as pit
from atx_db import derived_lineage
from atx_db import fundamental_signal_research as fsr
from atx_db.derived_registry import DERIVED_SOURCE_NAME, DerivedMetricDefinition
from atx_db.market_daily import MARKET_DAILY_SOURCE_NAME
from atx_db.research import panel as rp
from atx_db.research.store import ResearchStore
from atx_db.universe_us_listed import UNIVERSE_SOURCE_NAME

START = dt.date(2023, 10, 1)
END = dt.date(2024, 3, 1)
AS_OF = dt.date(2024, 4, 5)
RUN_AT = dt.datetime(2024, 4, 6, 12, tzinfo=dt.UTC)
FORMATIONS = [dt.date(2023, 10, 31), dt.date(2023, 11, 30), dt.date(2023, 12, 29),
              dt.date(2024, 1, 31), dt.date(2024, 2, 29), dt.date(2024, 3, 28)]
DERIVED = (("roa_q", "q"), ("accruals_ttm", "ttm"))
MARKET = ("market_cap", "momentum_12_1")
LINES = {1: ("SEC-CIK-0000000001", "AAA", "common", "XNAS"),
         2: ("SEC-CIK-0000000002", "BBB", "common_unverified", "XNYS"),
         3: ("SEC-CIK-0000000003", "CCC", "common", "XNAS")}
TAIL = "TBLTICKERHISTORY-7"
FUND = "SEC-CIK-0000000009"


def _definitions() -> tuple[DerivedMetricDefinition, ...]:
    derived = tuple(DerivedMetricDefinition(
        metric_code=code, family="test", expression="x", window=window, inputs=("item:x",),
        requires_market=False, description="test", version="1") for code, window in DERIVED)
    # market_cap reads the (possibly issuer-level DEI) share count: owner scope.
    # momentum reads only the line's own prices: price-line scope.
    inputs = {"market_cap": ("market:close", "market:shares_outstanding"), "momentum_12_1": ("market:adj_close",)}
    market = tuple(DerivedMetricDefinition(
        metric_code=code, family="market", expression="close", window="daily", inputs=inputs[code],
        requires_market=True, description="test", version="1") for code in MARKET)
    return derived + market


def _sessions(first: dt.date, last: dt.date) -> list[dt.date]:
    days, cursor = [], first
    while cursor <= last:
        if cursor.weekday() < 5 and cursor not in rp.nyse_full_day_closures(cursor.year):
            days.append(cursor)
        cursor += dt.timedelta(days=1)
    return days


def _bucket(day: dt.date) -> int:
    return (day.year * 12 + day.month - 1 + int(day.day >= 15)) // 3


class Warehouse:
    """Minimal file warehouse with the columns the research panel reads."""

    def __init__(self, path: Path, *, missing_sessions: tuple[dt.date, ...] = ()) -> None:
        self.path = path
        self.con = duckdb.connect(str(path), config={"memory_limit": "256MB", "threads": "1"})
        self.con.execute("SET TimeZone='UTC'")
        self.hashes = {(d.metric_code, d.window): pit.definition_hash(d)
                       for d in _definitions() if d.window != "daily"}
        self._schema()
        self.sessions = [d for d in _sessions(dt.date(2023, 9, 1), AS_OF) if d not in missing_sessions]

    def _schema(self) -> None:
        self.con.execute("""
            CREATE TABLE derived_metric_definitions (
              metric_code VARCHAR, metric_window VARCHAR, expression VARCHAR,
              inputs_json VARCHAR, version VARCHAR);
            CREATE TABLE derived_metric_values (
              derived_value_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
              metric_code VARCHAR, metric_window VARCHAR, target_bucket BIGINT, period_end DATE,
              value DOUBLE, available_at TIMESTAMP, valid_to TIMESTAMP, inputs_hash VARCHAR,
              definition_hash VARCHAR, history_status VARCHAR, value_status VARCHAR,
              selected_input_refs_json VARCHAR, selected_input_refs_hash VARCHAR, as_of_date DATE,
              fiscal_period_start DATE, fiscal_period_end DATE, value_origin VARCHAR);
            CREATE TABLE fundamental_standardized (
              standardized_id VARCHAR PRIMARY KEY, security_id VARCHAR, canonical_code VARCHAR,
              cik VARCHAR, basis VARCHAR, source VARCHAR, period_start DATE, period_end DATE,
              available_at TIMESTAMP, value DOUBLE, unit VARCHAR, unit_type VARCHAR);
            CREATE TABLE universe_us_listed_membership (
              universe_id VARCHAR, source VARCHAR, security_id VARCHAR, symbol VARCHAR,
              security_type VARCHAR, exchange_code VARCHAR, has_cik BOOLEAN, cik VARCHAR,
              reason VARCHAR, valid_from DATE, valid_to DATE, available_at TIMESTAMP,
              as_of_date DATE);
            CREATE TABLE security_identifier_history (
              security_id VARCHAR, id_type VARCHAR, id_value VARCHAR, valid_from DATE,
              valid_to DATE, available_at TIMESTAMP, as_of_date DATE);
            CREATE TABLE market_daily_metrics (
              market_daily_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
              trade_date DATE, available_at TIMESTAMP, as_of_date DATE, close DOUBLE, volume BIGINT,
              fundamental_available_at TIMESTAMP, market_cap DOUBLE, momentum_12_1 DOUBLE,
              shares_source VARCHAR);
            CREATE TABLE equity_daily_bars (
              security_id VARCHAR, symbol VARCHAR, trade_date DATE, close DOUBLE,
              adjusted_close DOUBLE, shares_outstanding BIGINT, available_at TIMESTAMP,
              source VARCHAR, vendor_security_id VARCHAR);
            CREATE TABLE sec_company_tickers (cik VARCHAR, ticker VARCHAR, source_loaded_at TIMESTAMP);
            CREATE TABLE shares_outstanding_history (
              share_history_id VARCHAR, source VARCHAR, security_id VARCHAR, symbol VARCHAR,
              cik VARCHAR, share_count_type VARCHAR, taxonomy VARCHAR, concept VARCHAR,
              unit VARCHAR, period_type VARCHAR, period_start DATE, period_end DATE,
              effective_date DATE, as_of_date DATE, available_at TIMESTAMP,
              accession_number VARCHAR, share_count DOUBLE, share_class VARCHAR);
        """)
        for definition in _definitions():
            if definition.window != "daily":
                self.con.execute("INSERT INTO derived_metric_definitions VALUES (?,?,?,?,?)",
                                 [definition.metric_code, definition.window, definition.expression,
                                  '["item:x"]', definition.version])

    def listing(self, cik: int, line: str, symbol: str, security_type: str, exchange: str, *,
                first: dt.date, last: dt.date, reason: str = "member", ticker: bool = True,
                volume: int = 1000, shares: int = 1_000_000, shares_source: str = "dei") -> None:
        for day in (d for d in self.sessions if first <= d <= last):
            self.con.execute("""
                INSERT INTO equity_daily_bars
                (security_id, symbol, trade_date, close, adjusted_close, shares_outstanding, available_at,
                 source, vendor_security_id)
                VALUES (?,?,?,100.0,100.0,?,NULL,'test_bars','1')
            """, [line, symbol, day, shares])
        if ticker:
            self.con.execute("INSERT INTO sec_company_tickers VALUES (?,?, TIMESTAMP '2024-04-05 06:00:00')",
                             [str(cik), symbol])
        self.con.execute("""
            INSERT INTO universe_us_listed_membership VALUES
            ('us_listed_reconstructed_v1',?,?,?,?,?,?,?,?,?,?,CAST(? AS TIMESTAMP) + INTERVAL 22 HOUR,?)
        """, [UNIVERSE_SOURCE_NAME, line, symbol, security_type, exchange, ticker,
              f"{cik:010d}" if ticker else None, reason, first,
              None if last >= self.sessions[-1] else last, first, first])
        # Strict membership and dated CIKs exist only from the post-history
        # snapshot (the run5 V6 shape): nothing is visible at any formation.
        self.con.execute("""
            INSERT INTO universe_us_listed_membership VALUES
            ('us_listed_v1',?,?,?,'common',?,true,?,'member',DATE '2024-04-05',NULL,
             TIMESTAMP '2024-04-05 10:00:00',DATE '2024-04-05')
        """, [UNIVERSE_SOURCE_NAME, line, symbol, exchange, f"{cik:010d}"])
        self.con.execute("""
            INSERT INTO security_identifier_history VALUES
            (?,'CIK',?,DATE '2024-04-05',NULL,TIMESTAMP '2024-04-05 10:00:00',DATE '2024-04-05')
        """, [line, f"{cik:010d}"])
        for day in (d for d in self.sessions if first <= d <= last):
            cutoff = dt.datetime.combine(day, dt.time(21))
            self.con.execute("""
                INSERT INTO market_daily_metrics VALUES (?,?,?,?,?,?,100.0,?,NULL,?,0.1,?)
            """, [f"{line}|{day}", MARKET_DAILY_SOURCE_NAME, line, day, cutoff, day, volume,
                  1000.0 * cik + day.toordinal() % 7, shares_source])

    def state(self, cik: int, code: str, window: str, period_end: dt.date, value: float | None,
              clock: dt.datetime, *, origin: str = "quarterly", state_id: str | None = None) -> str:
        state_id = state_id or f"{cik}_{code}_{period_end}_{clock:%Y%m%d%H%M}"
        owner = f"SEC-COMPANYFACTS-UNRESOLVED-CIK-{cik:010d}"
        leaf_id = f"leaf_{state_id}"
        leaf_clock = clock - dt.timedelta(hours=1)
        bucket = _bucket(period_end)
        self.con.execute("INSERT INTO fundamental_standardized VALUES "
                         "(?,?,'x',?,'quarterly','test',NULL,?,?,1.0,'USD','monetary')",
                         [leaf_id, owner, str(cik), period_end, leaf_clock])
        payload = json.dumps({"version": 1, "refs": [{
            "kind": "item", "code": "x", "bucket": bucket, "offset": 0, "status": "selected",
            "state_id": leaf_id, "available_at": leaf_clock.isoformat(), "cik": str(cik),
            "basis": "quarterly", "source": "test", "period_start": None,
            "period_end": period_end.isoformat()}]}, sort_keys=True, separators=(",", ":"))
        self.con.execute("""
            INSERT INTO derived_metric_values VALUES
            (?,?,?,?,?,?,?,?,?,NULL,?,?,'event_reconstructed',?,?,?,?,NULL,?,?)
        """, [state_id, DERIVED_SOURCE_NAME, owner, code, window, bucket, period_end, value, clock,
              "b" * 64, self.hashes[(code, window)], "valid" if value is not None else "missing_input_or_domain",
              payload, hashlib.sha256(payload.encode()).hexdigest(), clock.date(), period_end, origin])
        return state_id

    def close(self) -> None:
        self.con.close()


def _at(day: str, hour: int = 18, minute: int = 0) -> dt.datetime:
    return dt.datetime.combine(dt.date.fromisoformat(day), dt.time(hour, minute))


@pytest.fixture
def registry(monkeypatch):
    defs = _definitions()
    monkeypatch.setattr(fsr, "default_derived_definitions", lambda: defs)
    return defs


def _populate(wh: Warehouse) -> dict[str, str]:
    for cik, (line, symbol, security_type, exchange) in LINES.items():
        # BBB's market_cap uses the vendor (archive) share count: unverified size.
        wh.listing(cik, line, symbol, security_type, exchange, first=dt.date(2023, 9, 1), last=AS_OF,
                   shares_source="archive" if cik == 2 else "dei")
    wh.listing(7, TAIL, "OLD", "unknown", "UNKNOWN", first=dt.date(2023, 9, 1),
               last=dt.date(2023, 11, 10), reason="reconstructed_no_listing_evidence", ticker=False)
    wh.con.execute("""
        INSERT INTO universe_us_listed_membership VALUES
        ('us_listed_reconstructed_v1',?,?,'FFF','fund','ARCX',true,'0000000009','member',
         DATE '2023-09-01',NULL,TIMESTAMP '2023-09-01 22:00:00',DATE '2023-09-01')
    """, [UNIVERSE_SOURCE_NAME, FUND])
    ids = {}
    for cik in (1, 2):
        wh.state(cik, "roa_q", "q", dt.date(2023, 6, 30), 0.01 * cik, _at("2023-08-10"))
        ids[f"q3_{cik}"] = wh.state(cik, "roa_q", "q", dt.date(2023, 9, 30), 0.02 * cik, _at("2023-11-09"))
    # AAA's Q3 is revised on 2023-12-05; its Q4 lands 30 minutes after the
    # January cutoff, so January must still show the Q3 revision.
    ids["q3_1_rev"] = wh.state(1, "roa_q", "q", dt.date(2023, 9, 30), 0.025, _at("2023-12-05"))
    ids["q4_1_late"] = wh.state(1, "roa_q", "q", dt.date(2023, 12, 31), 0.03, _at("2024-01-31", 22, 30))
    ids["q4_2"] = wh.state(2, "roa_q", "q", dt.date(2023, 12, 31), 0.04, _at("2024-02-15"))
    for period, clock in ((dt.date(2023, 6, 30), "2023-08-10"), (dt.date(2023, 9, 30), "2023-11-09"),
                          (dt.date(2023, 12, 31), "2024-02-20")):
        wh.state(1, "accruals_ttm", "ttm", period, -0.05, _at(clock))
    # BBB stops reporting after Q2 (quarterly origin): stale after 200 days.
    wh.state(2, "accruals_ttm", "ttm", dt.date(2023, 6, 30), -0.07, _at("2023-08-10"))
    # CCC's value is an annual fallback for FY ending 2023-03-31: 400-day bound.
    wh.state(3, "accruals_ttm", "ttm", dt.date(2023, 3, 31), -0.02, _at("2023-06-01"), origin="annual_fallback")
    wh.state(3, "roa_q", "q", dt.date(2023, 9, 30), 0.05, _at("2023-11-09"))
    # The delisted tail's issuer has content but no owner-linked member anywhere.
    ids["tail_q"] = wh.state(7, "roa_q", "q", dt.date(2023, 6, 30), 0.07, _at("2023-08-10"))
    return ids


@pytest.fixture
def warehouse(tmp_path, registry):
    wh = Warehouse(tmp_path / "wh.duckdb")
    ids = _populate(wh)
    wh.close()
    return tmp_path / "wh.duckdb", ids


#: The fixture registry's features. Pinned so the fixtures do not track the default set,
#: which also carries the P2 natives (their inputs are added only by the P2 test).
BASE_FEATURES = tuple(rp.PanelFeature(code, code, window) for code, window in (*DERIVED, *((m, "daily") for m in MARKET)))


def _options(run_id: str, basis: str = rp.BASIS_RECONSTRUCTED, **extra) -> rp.ResearchPanelOptions:
    extra.setdefault("features", BASE_FEATURES)
    return rp.ResearchPanelOptions(run_id=run_id, basis=basis, start_month=START, end_month=END,
                                   as_of_date=AS_OF, run_at=RUN_AT, **extra)


def _value(con, run_id, formation, security, feature):
    return con.execute("""
        SELECT raw_value, reason, available_at, derived_value_id, age_days, max_age_days,
               identity_basis, universe_basis, availability_basis, owner_cik
        FROM research_panel_values
        WHERE run_id=? AND formation_date=? AND security_id=? AND feature_id=?
    """, [run_id, formation, security, feature]).fetchone()


# ---------------------------------------------------------------------------
# Calendar
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(("year", "month", "expected"), [
    (2024, 3, dt.date(2024, 3, 28)),   # Good Friday 2024-03-29
    (2013, 3, dt.date(2013, 3, 28)),   # Good Friday 2013-03-29
    (2021, 5, dt.date(2021, 5, 28)),   # Memorial Day 2021-05-31
    (2022, 12, dt.date(2022, 12, 30)),  # Saturday month end
    (2021, 12, dt.date(2021, 12, 31)),  # New Year on Saturday: NYSE open Friday
    (2016, 1, dt.date(2016, 1, 29)),   # Sunday month end
    (2023, 9, dt.date(2023, 9, 29)),
    (2012, 10, dt.date(2012, 10, 31)),  # Sandy closures were Oct 29-30
])
def test_month_end_session_follows_nyse_rules(year, month, expected):
    assert rp.expected_month_end_session(year, month) == expected


def test_missing_month_end_is_never_replaced_by_an_earlier_session():
    sessions = [d for d in _sessions(dt.date(2024, 1, 2), dt.date(2024, 4, 5))
                if d not in (dt.date(2024, 2, 29),)]
    rows = rp.month_end_calendar(sessions, start_month=dt.date(2024, 1, 1), end_month=dt.date(2024, 5, 1),
                                 as_of_date=dt.date(2024, 4, 5), run_at=dt.datetime(2024, 4, 6))
    by_month = {row.month_start.month: row for row in rows}
    assert by_month[2].status == rp.CALENDAR_MISSING_MONTH_END
    assert by_month[2].last_observed_session == dt.date(2024, 2, 28)
    assert by_month[2].formation_date is None
    assert by_month[1].status == rp.CALENDAR_FORMED
    assert (by_month[1].formation_date, by_month[1].entry_date) == (dt.date(2024, 1, 31), dt.date(2024, 2, 1))
    assert by_month[1].cutoff == dt.datetime(2024, 1, 31, 22)
    assert by_month[3].formation_date == dt.date(2024, 3, 28)
    assert by_month[4].status == rp.CALENDAR_AFTER_CUTOFF
    assert by_month[5].status == rp.CALENDAR_AFTER_CUTOFF
    # A session on a rule holiday means the rule and the data disagree: refuse.
    odd = rp.month_end_calendar([*sessions, dt.date(2024, 3, 29)], start_month=dt.date(2024, 3, 1),
                                end_month=dt.date(2024, 3, 1), as_of_date=dt.date(2024, 4, 5),
                                run_at=dt.datetime(2024, 4, 6))
    assert odd[0].status == rp.CALENDAR_RULE_CONFLICT
    last = rp.month_end_calendar(sessions[:-5], start_month=dt.date(2024, 3, 1), end_month=dt.date(2024, 3, 1),
                                 as_of_date=dt.date(2024, 4, 5), run_at=dt.datetime(2024, 4, 6))
    assert last[0].status == rp.CALENDAR_MISSING_NEXT_SESSION


# ---------------------------------------------------------------------------
# Reconstructed panel: point in time, labeled, validated
# ---------------------------------------------------------------------------

def test_reconstructed_panel_is_point_in_time_and_labeled(tmp_path, warehouse):
    wh_path, ids = warehouse
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=wh_path) as store:
        result = rp.build_research_panel(store, _options("recon"))
        con = store.con
        assert result.status == "complete"
        assert result.formations == 6
        formed = [row[0] for row in con.execute("""
            SELECT formation_date FROM research_panel_calendar
            WHERE run_id='recon' AND status='formed' ORDER BY 1""").fetchall()]
        assert formed == FORMATIONS
        aaa, bbb, ccc = (LINES[i][0] for i in (1, 2, 3))
        # Revision and late filing: selected by availability at each cutoff only.
        assert _value(con, "recon", dt.date(2023, 11, 30), aaa, "roa_q")[3] == ids["q3_1"]
        assert _value(con, "recon", dt.date(2023, 12, 29), aaa, "roa_q")[:2] == (0.025, "valid")
        jan = _value(con, "recon", dt.date(2024, 1, 31), aaa, "roa_q")
        assert jan[3] == ids["q3_1_rev"] and jan[0] == 0.025
        assert jan[2] <= dt.datetime(2024, 1, 31, 22)
        assert _value(con, "recon", dt.date(2024, 2, 29), aaa, "roa_q")[3] == ids["q4_1_late"]
        assert _value(con, "recon", dt.date(2024, 1, 31), bbb, "roa_q")[3] == ids["q3_2"]
        # No row anywhere was knowable only after its formation cutoff.
        assert con.execute("""
            SELECT count(*) FROM research_panel_values v JOIN research_panel_calendar c
              ON c.run_id=v.run_id AND c.formation_date=v.formation_date
            WHERE v.run_id='recon' AND (v.available_at>c.cutoff OR v.latest_input_clock>c.cutoff)
        """).fetchone()[0] == 0
        # Staleness by window and origin, with an explicit reason; stale keeps its value.
        assert _value(con, "recon", dt.date(2023, 12, 29), bbb, "accruals_ttm")[1] == "valid"
        stale = _value(con, "recon", dt.date(2024, 1, 31), bbb, "accruals_ttm")
        assert stale[:2] == (-0.07, "stale_current_anchor") and stale[4:6] == (215, 200)
        annual = _value(con, "recon", dt.date(2024, 3, 28), ccc, "accruals_ttm")
        assert annual[1] == "valid" and annual[4:6] == (363, 400)
        # Every row carries its bases; identity is the reconstructed bridge CIK.
        assert jan[6:] == ("current_ticker_unverified", "us_listed_reconstructed_v1",
                           rp.FUNDAMENTAL_AVAILABILITY_BASIS, "0000000001")
        cap = _value(con, "recon", dt.date(2024, 1, 31), aaa, "market_cap")
        assert cap[1] == "valid" and cap[8] == rp.MARKET_AVAILABILITY_BASIS
        assert cap[2] == dt.datetime(2024, 1, 31, 21)
        assert cap[6] == "current_ticker_unverified"  # issuer-share feature: owner scope
        # Size carries its share basis: only a DEI count is verified (N1).
        size = dict((row[0], row[1:]) for row in con.execute("""
            SELECT security_id || '/' || feature_id, shares_source, size_status FROM research_panel_values
            WHERE run_id='recon' AND formation_date='2024-01-31'
        """).fetchall())
        assert size[f"{aaa}/market_cap"] == ("dei", rp.SIZE_VERIFIED)
        assert size[f"{bbb}/market_cap"] == ("archive", rp.UNVERIFIED_VENDOR_SHARES)
        assert size[f"{aaa}/momentum_12_1"] == (None, None) and size[f"{aaa}/roa_q"] == (None, None)
        policy = json.loads(con.execute("SELECT spec_json FROM research_panel_runs WHERE run_id='recon'"
                                        ).fetchone()[0])["size_policy"]
        assert policy["size_features"] == ["market_cap"] and policy["verified_status"] == rp.SIZE_VERIFIED
        assert "line_market_cap" not in {row[0] for row in con.execute(
            "SELECT DISTINCT feature_id FROM research_panel_values WHERE run_id='recon'").fetchall()}
        # Cohort exclusions are explicit, including the delisted tail.
        cohort = dict(con.execute("""
            SELECT security_id, cohort_reason || ':' || coalesce(owner_link_reason,'linked')
            FROM research_panel_cohort WHERE run_id='recon' AND formation_date='2023-10-31'
        """).fetchall())
        assert cohort[TAIL] == "missing_owner_link:no_current_ticker"
        assert cohort[FUND].startswith("not_common")
        assert cohort[bbb] == "valid:linked"  # common_unverified is eligible in reconstruction
        assert TAIL not in dict(con.execute("""
            SELECT security_id, 1 FROM research_panel_cohort
            WHERE run_id='recon' AND formation_date='2023-11-30'""").fetchall())
        # Survivorship (I1): the unlinked tail keeps every identity-free market
        # feature; owner features are NULL rows carrying the cohort exclusion.
        october = dt.date(2023, 10, 31)
        momentum = _value(con, "recon", october, TAIL, "momentum_12_1")
        assert momentum[:2] == (0.1, "valid")
        assert momentum[6] == rp.PRICE_LINE_IDENTITY_BASIS and momentum[9] is None
        for feature in ("roa_q", "accruals_ttm", "market_cap"):
            tail_row = _value(con, "recon", october, TAIL, feature)
            assert tail_row[:4] == (None, "missing_owner_link", None, None)
        assert _value(con, "recon", october, FUND, "momentum_12_1") is None  # not eligible
        coverage = {row[0]: (row[1], row[2], row[3], json.loads(row[4])) for row in con.execute("""
            SELECT feature_id, eligible_members, valid_members, values_emitted, reasons_json
            FROM research_panel_coverage WHERE run_id='recon' AND formation_date='2023-10-31'
        """).fetchall()}
        assert coverage["momentum_12_1"] == (4, 3, 4, {"valid": 4})
        assert coverage["roa_q"][:3] == (4, 3, 4)
        assert coverage["roa_q"][3] == {"valid": 2, "missing_metric_state": 1, "missing_owner_link": 1}
        assert all(sum(item[3].values()) == item[0] for item in coverage.values())
        attrition = con.execute("""
            SELECT formation_date, eligible_members, owner_unlinked_members, owner_link_attrition
            FROM research_panel_calendar WHERE run_id='recon' AND status='formed' ORDER BY 1
        """).fetchall()
        assert attrition[0] == (october, 4, 1, 0.25)
        assert all(row[1:3] == (3, 0) for row in attrition[1:])
        assert "owner_link_attrition:1/19" in result.blockers
        diagnostic = json.loads(con.execute(
            "SELECT diagnostic_json FROM research_panel_runs WHERE run_id='recon'").fetchone()[0])
        assert diagnostic["owner_link_attrition"]["by_formation"]["2023-10-31"] == 0.25
        assert diagnostic["lineage_proofs_by_method"] == {"set_exact": diagnostic["lineage_proofs"]}
        validated = rp.validate_research_panel(store, "recon")
        assert validated.panel_sha256 == result.panel_sha256
        assert validated.formations == tuple(FORMATIONS)
        con.execute("""
            UPDATE research_panel_values SET raw_value=raw_value+1
            WHERE run_id='recon' AND formation_date='2024-01-31' AND feature_id='roa_q'
        """)
        with pytest.raises(ValueError, match="value digest mismatch"):
            rp.validate_research_panel(store, "recon")


def test_newest_market_revision_wins_even_when_null_and_one_formation_equals_the_panel(tmp_path, warehouse):
    """R2e: a newer market revision that withholds market_cap (NULL, split unresolved) is the value at
    the cutoff; the older revision's value is never revived under the newer clock. The public
    one-formation API reproduces the panel's value digests for the same formation."""
    wh_path, _ = warehouse
    bbb, november = LINES[2][0], dt.date(2023, 11, 30)
    con = duckdb.connect(str(wh_path))
    con.execute("INSERT INTO market_daily_metrics VALUES (?,?,?,?,?,?,100.0,1000,NULL,NULL,0.2,'split_unresolved')",
                [f"{bbb}|{november}|rev", MARKET_DAILY_SOURCE_NAME, bbb, november,
                 dt.datetime(2023, 11, 30, 21, 30), november])
    con.close()
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=wh_path) as store:
        rp.build_research_panel(store, _options("newest"))
        con = store.con
        cap = con.execute("""
            SELECT raw_value, reason, available_at, shares_source, size_status FROM research_panel_values
            WHERE run_id='newest' AND formation_date=? AND security_id=? AND feature_id='market_cap'
        """, [november, bbb]).fetchone()
        assert cap == (None, "invalid_current_state", dt.datetime(2023, 11, 30, 21, 30), "split_unresolved",
                       rp.UNVERIFIED_VENDOR_SHARES)
        assert _value(con, "newest", november, bbb, "momentum_12_1")[:3] == (
            0.2, "valid", dt.datetime(2023, 11, 30, 21, 30))
        assert _value(con, "newest", dt.date(2023, 10, 31), bbb, "market_cap")[1] == "valid"
        rp.validate_research_panel(store, "newest")
        coverage = dict(con.execute("""
            SELECT feature_id, values_sha256 FROM research_panel_coverage WHERE run_id='newest' AND formation_date=?
        """, [november]).fetchall())
        staged = rp.stage_formation(store, basis=rp.BASIS_RECONSTRUCTED, formation_date=november,
                                    cutoff=dt.datetime(2023, 11, 30, 22, tzinfo=dt.UTC), run_id="one_formation",
                                    features=BASE_FEATURES)
        assert staged.eligible == con.execute("SELECT eligible_members FROM research_panel_calendar "
                                              "WHERE run_id='newest' AND formation_date=?", [november]).fetchone()[0]
        digests = {}
        for batch in rp.formation_batches(store, staged):
            digests.update(batch.values_sha256)
            if batch.is_market:
                assert con.execute(f"""
                    SELECT raw_value, reason FROM {rp.FORMATION_VALUES_RELATION}
                    WHERE security_id=? AND feature_id='market_cap'""", [bbb]).fetchone() == (
                    None, "invalid_current_state")
        assert digests == coverage and len(digests) == 4
        with pytest.raises(ValueError, match="names a panel run"):
            rp.stage_formation(store, basis=rp.BASIS_RECONSTRUCTED, formation_date=november,
                               cutoff=dt.datetime(2023, 11, 30, 22, tzinfo=dt.UTC), run_id="newest",
                               features=BASE_FEATURES)
        # R2e m3: re-staging (same session, later cutoff) while the generator is suspended is refused.
        batches = rp.formation_batches(store, staged)
        next(batches)
        rp.stage_formation(store, basis=rp.BASIS_RECONSTRUCTED, formation_date=november,
                           cutoff=dt.datetime(2023, 12, 1, 12, tzinfo=dt.UTC), run_id="one_later",
                           features=BASE_FEATURES)
        with pytest.raises(RuntimeError, match="replaced"):
            next(batches)


P2_FEATURES = tuple(rp.PanelFeature(code, code, "daily") for code in sorted(rp.PRICE_WINDOW_FEATURES))


def test_price_liquidity_natives_equal_hand_computation_and_never_read_a_bar_after_the_cutoff(tmp_path, warehouse):
    """P2: AAA's MAX, 52-week-high ratio, Amihud, downside deviation and turnover at the January formation
    equal a hand computation from its own bars. Two bars knowable only after the cutoff (a late vendor row
    of the session itself and a later revision of an earlier bar) are never inputs. Turnover is NULL with a
    reason on unverified shares, a short history is labeled, and unlinked lines keep the price-line natives."""
    wh_path, _ = warehouse
    aaa, bbb, ccc = (LINES[i][0] for i in (1, 2, 3))
    october, january = dt.date(2023, 10, 31), dt.date(2024, 1, 31)
    con = duckdb.connect(str(wh_path))
    con.execute("ALTER TABLE equity_daily_bars ADD COLUMN volume BIGINT")
    con.execute("ALTER TABLE market_daily_metrics ADD COLUMN shares_outstanding DOUBLE")
    con.execute("UPDATE market_daily_metrics SET shares_outstanding = 2000000")   # AAA's is a DEI count
    # CCC: a 2:1 split on 2024-01-22 (back-adjusted closes halve before it) inside its turnover window.
    con.execute("UPDATE equity_daily_bars SET volume = 5000, adjusted_close = CASE WHEN trade_date < "
                "DATE '2024-01-22' THEN 50.0 ELSE 100.0 END WHERE security_id=?", [ccc])
    con.execute("DELETE FROM equity_daily_bars WHERE security_id=?", [aaa])
    series = []
    for i, day in enumerate(_sessions(dt.date(2023, 9, 1), AS_OF)):
        adj = 50.0 + 10.0 * math.sin(i / 3.0) + 0.05 * i
        series.append((day, adj * 1.25, adj, 10_000 + 997 * (i % 13)))
    con.executemany("""
        INSERT INTO equity_daily_bars (security_id, symbol, trade_date, close, adjusted_close, shares_outstanding,
                                       available_at, source, vendor_security_id, volume)
        VALUES (?, 'AAA', ?, ?, ?, 1000000, NULL, 'test_bars', '1', ?)
    """, [[aaa, day, close, adj, volume] for day, close, adj, volume in series])
    for day, clock in ((january, dt.datetime(2024, 1, 31, 23, 30)), (dt.date(2024, 1, 10), dt.datetime(2024, 2, 5))):
        con.execute("INSERT INTO equity_daily_bars (security_id, symbol, trade_date, close, adjusted_close, "
                    "shares_outstanding, available_at, source, vendor_security_id, volume) "
                    "VALUES (?, 'AAA', ?, 999.0, 999.0, 1000000, ?, 'z_late_vendor', '1', 999999999)",
                    [aaa, day, clock])
    con.close()
    # Hand computation over the bars dated up to January 31 (the late rows excluded).
    bars = [row for row in series if row[0] <= january]
    close, adj, volume = ([row[k] for row in bars] for k in (1, 2, 3))
    returns = [adj[i] / adj[i - 1] - 1.0 for i in range(1, len(adj))]
    last = range(len(adj) - 21, len(adj))
    expected = {
        "max_daily_return_21d": max(returns[-21:]),
        "pct_from_high_252d": adj[-1] / max(adj[-252:]) - 1.0,
        "amihud_illiquidity_21d": sum(abs(adj[i] / adj[i - 1] - 1.0) / (close[i] * volume[i]) for i in last)
        / 21 * 1e9,
        "downside_deviation_60d": math.sqrt(sum(min(r, 0.0) ** 2 for r in returns[-60:]) / 60) * math.sqrt(252),
        "turnover_21d": sum(volume[-21:]) / 21 / 2_000_000,
    }
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=wh_path) as store:
        assert rp.build_research_panel(store, _options("p2", features=P2_FEATURES)).status == "complete"
        con = store.con
        for feature, value in expected.items():
            row = _value(con, "p2", january, aaa, feature)
            assert row[1] == "valid" and row[0] == pytest.approx(value, rel=1e-12, abs=0), (feature, row, value)
            assert row[2] == dt.datetime(2024, 1, 31, 22)          # the session bar's clock, never 23:30
        assert expected["pct_from_high_252d"] < 0 and expected["max_daily_return_21d"] > 0
        size = dict((row[0], row[1:]) for row in con.execute("""
            SELECT security_id, raw_value, reason, shares_source, size_status FROM research_panel_values
            WHERE run_id='p2' AND formation_date=? AND feature_id='turnover_21d'""", [january]).fetchall())
        assert size[aaa][2:] == ("dei", rp.SIZE_VERIFIED)
        assert size[bbb] == (None, rp.UNVERIFIED_SHARES_REASON, "archive", rp.UNVERIFIED_VENDOR_SHARES)
        assert size[ccc][:2] == (None, rp.SPLIT_WINDOW_REASON)
        # 42 bars since September 1 (41 returns): the 60-return window is incomplete in October.
        assert _value(con, "p2", october, aaa, "downside_deviation_60d")[:2] == (None, rp.INCOMPLETE_WINDOW_REASON)
        # The unlinked delisted tail keeps the identity-free natives; turnover is owner-scoped.
        tail = _value(con, "p2", october, TAIL, "max_daily_return_21d")
        assert tail[:2] == (0.0, "valid") and tail[6] == rp.PRICE_LINE_IDENTITY_BASIS
        assert _value(con, "p2", october, TAIL, "turnover_21d")[1] == "missing_owner_link"
        definitions = json.loads(con.execute("SELECT definitions_json FROM research_panel_runs WHERE run_id='p2'"
                                             ).fetchone()[0])
        assert {m["metric_code"]: m["source"] for m in definitions["market"]} == {
            f.metric_code: rp.BARS_SOURCE for f in P2_FEATURES}
        rp.validate_research_panel(store, "p2")


def test_missing_month_end_gets_no_formation_rows(tmp_path, registry):
    missing = (dt.date(2024, 2, 29),)
    wh = Warehouse(tmp_path / "wh.duckdb", missing_sessions=missing)
    _populate(wh)
    wh.close()
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=tmp_path / "wh.duckdb") as store:
        result = rp.build_research_panel(store, _options("gap"))
        con = store.con
        assert result.formations == 5
        assert con.execute("""
            SELECT status, last_observed_session, formation_date FROM research_panel_calendar
            WHERE run_id='gap' AND month_start='2024-02-01'
        """).fetchone() == ("missing_month_end", dt.date(2024, 2, 28), None)
        assert con.execute("""
            SELECT count(*) FROM research_panel_values
            WHERE run_id='gap' AND formation_date BETWEEN '2024-02-01' AND '2024-02-29'
        """).fetchone()[0] == 0
        assert "missing_or_conflicting_month_end_sessions" in result.blockers
        rp.validate_research_panel(store, "gap")


def test_strict_basis_on_v6_state_is_untestable_and_empty(tmp_path, warehouse):
    wh_path, _ = warehouse
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=wh_path) as store:
        strict = rp.build_research_panel(store, _options("strict", rp.BASIS_STRICT))
        recon = rp.build_research_panel(store, _options("recon", rp.BASIS_RECONSTRUCTED))
        con = store.con
        assert strict.status == "untestable_strict"
        assert strict.value_rows == 0 and strict.formations == 6
        assert con.execute("""
            SELECT count(*), count(DISTINCT status), min(status)
            FROM research_panel_coverage WHERE run_id='strict'
        """).fetchone() == (6 * 4, 1, "empty_common_cohort")
        assert "no_valid_cohort_members" in strict.blockers
        assert recon.status == "complete" and recon.valid_values > 0
        assert con.execute("""
            SELECT DISTINCT feature_scope, identity_basis, universe_basis FROM research_panel_values
            WHERE run_id='recon' ORDER BY 1
        """).fetchall() == [("owner", "current_ticker_unverified", "us_listed_reconstructed_v1"),
                            ("price_line", "price_line", "us_listed_reconstructed_v1")]
        assert rp.validate_research_panel(store, "strict").status == "untestable_strict"


def test_lineage_is_resolved_once_per_selected_state(tmp_path, warehouse, monkeypatch):
    wh_path, ids = warehouse
    calls: list[str] = []
    real = derived_lineage.qualify_selected_lineage

    def counting(con, root_ids, **kwargs):
        calls.extend(root_ids)
        return real(con, root_ids, **kwargs)

    monkeypatch.setattr(derived_lineage, "qualify_selected_lineage", counting)
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=wh_path) as store:
        rp.build_research_panel(store, _options("once"))
        con = store.con
        selected = {row[0] for row in con.execute("""
            SELECT DISTINCT derived_value_id FROM research_panel_values
            WHERE run_id='once' AND derived_value_id IS NOT NULL""").fetchall()}
        repeats = con.execute("""
            SELECT count(*) FROM (SELECT derived_value_id FROM research_panel_values
            WHERE run_id='once' AND derived_value_id IS NOT NULL GROUP BY 1 HAVING count(*)>1)
        """).fetchone()[0]
        proofs = con.execute("""
            SELECT derived_value_id, count(*), min(method), min(proof_digest) FROM research_lineage_proofs
            WHERE run_id='once' GROUP BY 1
        """).fetchall()
        digests = dict(con.execute("""
            SELECT DISTINCT derived_value_id, lineage_digest FROM research_panel_values
            WHERE run_id='once' AND derived_value_id IS NOT NULL""").fetchall())
        # The slim proof row equals the Python resolver's proof of the same root.
        python = real(con, [ids["q3_1_rev"]], expected_cik=None, decision_cutoff=None,
                      expected_definition_hashes={
                          ("roa_q", "q"): con.execute(
                              "SELECT DISTINCT definition_hash FROM derived_metric_values "
                              "WHERE derived_value_id=?", [ids["q3_1_rev"]]).fetchone()[0]},
                      max_depth=16, max_nodes=512, max_bytes=1_048_576)[ids["q3_1_rev"]]
        batches = json.loads(con.execute(
            "SELECT diagnostic_json FROM research_panel_runs WHERE run_id='once'").fetchone()[0]
        )["this_invocation"]["batches"]
    assert repeats > 0  # the same state is selected at several month ends ...
    assert all(n == 1 for _, n, _, _ in proofs)  # ... but proved exactly once
    # An owner whose CIK no owner-linked member has is never proved (counted).
    assert ids["tail_q"] not in {row[0] for row in proofs}
    assert sum(item.get("skipped_unlinked_roots", 0) for item in batches) == 1
    assert selected <= {row[0] for row in proofs}
    assert {row[2] for row in proofs} == {"set_exact"} and not calls  # no Python fallback needed
    assert digests[ids["q3_1_rev"]] == python.digest == dict((r[0], r[3]) for r in proofs)[ids["q3_1_rev"]]


def test_batching_and_resume_reproduce_the_same_panel(tmp_path, warehouse, monkeypatch):
    wh_path, _ = warehouse
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=wh_path) as store:
        con = store.con
        one = rp.build_research_panel(store, _options("one", metric_batch_size=1))
        two = rp.build_research_panel(store, _options("two", metric_batch_size=2))
        again = rp.build_research_panel(store, _options("again", metric_batch_size=2))

        def digests(run_id):
            return con.execute("""
                SELECT formation_date, feature_id, values_sha256, reasons_json
                FROM research_panel_coverage WHERE run_id=? ORDER BY 1, 2
            """, [run_id]).fetchall(), con.execute("""
                SELECT month_start, cohort_sha256 FROM research_panel_calendar WHERE run_id=? ORDER BY 1
            """, [run_id]).fetchall()

        assert digests("one") == digests("two")
        assert again.panel_sha256 == two.panel_sha256
        assert one.panel_sha256 != two.panel_sha256  # batch size is part of the frozen spec
        real = rp._derived_formations

        def fail_in_january(store_, run_id, batch, spec, rows, ordinals):
            if any(row.formation_date == dt.date(2024, 1, 31) for row in rows):
                raise RuntimeError("injected failure")
            return real(store_, run_id, batch, spec, rows, ordinals)

        monkeypatch.setattr(rp, "_derived_formations", fail_in_january)
        with pytest.raises(RuntimeError, match="injected failure"):
            rp.build_research_panel(store, _options("resumed", metric_batch_size=2, formation_chunk=2))
        assert con.execute("SELECT status FROM research_panel_runs WHERE run_id='resumed'").fetchone()[0] == "failed"
        # Formation chunks before January committed; January's chunk did not.
        assert [row[0] for row in con.execute("""
            SELECT DISTINCT formation_date FROM research_panel_coverage
            WHERE run_id='resumed' AND metric_window<>'daily' ORDER BY 1""").fetchall()] == FORMATIONS[:2]
        with pytest.raises(ValueError, match="not sealed"):
            rp.validate_research_panel(store, "resumed")
        monkeypatch.setattr(rp, "_derived_formations", real)
        with pytest.raises(ValueError, match="already exists"):
            rp.build_research_panel(store, _options("resumed", metric_batch_size=2))
        resumed = rp.build_research_panel(store, _options("resumed", metric_batch_size=2, resume=True))
        assert resumed.status == "complete"
        assert resumed.panel_sha256 == two.panel_sha256
        assert digests("resumed") == digests("two")
        with pytest.raises(ValueError, match="sealed"):
            rp.build_research_panel(store, _options("resumed", metric_batch_size=2, resume=True))
        # Proofs commit per chunk: a failure mid-proof resumes after the last
        # committed chunk, never re-proves a root, and reproduces the panel.
        real_prove = rp._lineage.prove_roots
        chunks: list[int] = []

        def fail_third_chunk(con_, hashes, **kwargs):
            chunks.append(1)
            if len(chunks) == 3:
                raise RuntimeError("injected proof failure")
            return real_prove(con_, hashes, **kwargs)

        monkeypatch.setattr(rp._lineage, "prove_roots", fail_third_chunk)
        with pytest.raises(RuntimeError, match="injected proof failure"):
            rp.build_research_panel(store, _options("chunked", metric_batch_size=2, proof_chunk_roots=2))
        assert con.execute("SELECT count(*) FROM research_lineage_proofs WHERE run_id='chunked'").fetchone()[0] == 4
        chunked = rp.build_research_panel(store, _options("chunked", metric_batch_size=2, proof_chunk_roots=2,
                                                          resume=True))
        assert chunked.panel_sha256 == two.panel_sha256
        assert con.execute("""
            SELECT count(*) = count(DISTINCT derived_value_id) FROM research_lineage_proofs WHERE run_id='chunked'
        """).fetchone()[0]
        rp.validate_research_panel(store, "chunked")


def test_owner_features_attach_to_one_primary_line_per_issuer(tmp_path, registry):
    wh = Warehouse(tmp_path / "wh.duckdb")
    ids = _populate(wh)
    # A second current class line of issuer 1, trading 5x AAA's volume.
    wh.listing(1, "TBLTICKERHISTORY-8", "AAA-B", "common", "XNAS", first=dt.date(2023, 9, 1), last=AS_OF,
               volume=5000, shares=250_000)
    wh.close()
    aaa, class_b = LINES[1][0], "TBLTICKERHISTORY-8"
    line_cap = rp.PanelFeature("line_market_cap", "line_market_cap", "daily")
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=tmp_path / "wh.duckdb") as store:
        rp.build_research_panel(store, _options("lines", unverified_vendor_shares=True,
                                                features=(*BASE_FEATURES, line_cap)))
        con = store.con
        cohort = dict((row[0], row[1:]) for row in con.execute("""
            SELECT security_id, issuer_lines, primary_line, primary_line_rule, owner_cik FROM research_panel_cohort
            WHERE run_id='lines' AND formation_date='2024-01-31' AND security_id IN (?, ?)
        """, [aaa, class_b]).fetchall())
        assert cohort[class_b] == (2, True, rp.PRIMARY_RULE_DOLLAR_VOLUME, "0000000001")
        assert cohort[aaa] == (2, False, rp.PRIMARY_RULE_DOLLAR_VOLUME, "0000000001")
        january = dt.date(2024, 1, 31)
        # The issuer's fundamental and issuer-share rows exist once, on the primary line.
        primary = _value(con, "lines", january, class_b, "roa_q")
        assert primary[:2] == (0.025, "valid") and primary[3] == ids["q3_1_rev"]
        for feature in ("roa_q", "accruals_ttm", "market_cap"):
            assert _value(con, "lines", january, aaa, feature)[:4] == (None, rp.SECONDARY_LINE_REASON, None, None)
        assert con.execute("""
            SELECT count(*) FROM research_panel_values
            WHERE run_id='lines' AND feature_scope='owner' AND raw_value IS NOT NULL
            GROUP BY formation_date, feature_id, owner_cik ORDER BY 1 DESC LIMIT 1
        """).fetchone()[0] == 1
        # Price-line features stay on every line; the opt-in line size is the
        # line's own vendor count, labeled unverified end to end (N1).
        assert _value(con, "lines", january, aaa, "momentum_12_1")[1] == "valid"
        assert _value(con, "lines", january, aaa, "line_market_cap")[0] == 100.0 * 1_000_000
        b_cap = _value(con, "lines", january, class_b, "line_market_cap")
        assert b_cap[0] == 100.0 * 250_000 and b_cap[8] == rp.VENDOR_SHARES_AVAILABILITY_BASIS
        assert con.execute("""
            SELECT DISTINCT shares_source, size_status FROM research_panel_values
            WHERE run_id='lines' AND feature_id='line_market_cap' AND raw_value IS NOT NULL
        """).fetchall() == [("equity_daily_bars_vendor", rp.UNVERIFIED_VENDOR_SHARES)]
        spec = json.loads(con.execute("SELECT spec_json FROM research_panel_runs WHERE run_id='lines'").fetchone()[0])
        assert spec["unverified_vendor_shares"] is True
        assert spec["size_policy"]["size_features"] == ["line_market_cap", "market_cap"]
        coverage = json.loads(con.execute("""
            SELECT reasons_json FROM research_panel_coverage
            WHERE run_id='lines' AND formation_date='2024-01-31' AND feature_id='roa_q'
        """).fetchone()[0])
        assert coverage[rp.SECONDARY_LINE_REASON] == 1
        rp.validate_research_panel(store, "lines")


def test_vendor_share_size_is_opt_in_only(registry):
    line_cap = rp.PanelFeature("line_market_cap", "line_market_cap", "daily")
    assert "line_market_cap" not in {f.feature_id for f in rp.default_panel_features()}
    with pytest.raises(ValueError, match="unverified_vendor_shares"):
        rp._validate(_options("gate", features=(line_cap,)))
    _, features, spec = rp._validate(_options("gate", features=(line_cap,), unverified_vendor_shares=True))
    assert [f.feature_id for f in features] == ["line_market_cap"] and spec["unverified_vendor_shares"] is True


def _selection_fixture(con, seed: int) -> list[tuple[dt.date, dt.datetime]]:
    """Random derived histories, including I/J violations and late as_of dates."""
    import random

    rng = random.Random(seed)
    con.execute("""
        CREATE TABLE derived_metric_values (derived_value_id VARCHAR, source VARCHAR, security_id VARCHAR,
          metric_code VARCHAR, metric_window VARCHAR, target_bucket BIGINT, period_end DATE,
          available_at TIMESTAMP, as_of_date DATE)
    """)
    rows = []
    quarter_ends = [dt.date(2022, 3, 31), dt.date(2022, 6, 30), dt.date(2022, 9, 30), dt.date(2022, 12, 31),
                    dt.date(2023, 3, 31), dt.date(2023, 6, 30), dt.date(2023, 9, 30)]
    for owner in range(12):
        for code in ("m_a", "m_b"):
            for n in range(rng.randint(0, 9)):
                period = rng.choice(quarter_ends)
                if rng.random() < 0.15:  # a shifted fiscal end in the same bucket (breaks I)
                    period = period - dt.timedelta(days=rng.randint(1, 4))
                bucket = None if rng.random() < 0.7 else _bucket(period) + rng.choice((0, 0, 1))  # J
                at = dt.datetime.combine(period, dt.time(17)) + dt.timedelta(days=rng.randint(20, 400),
                                                                              hours=rng.choice((0, 5, 6)))
                as_of = at.date() + dt.timedelta(days=rng.choice((0, 0, 0, 40)))
                rows.append([f"s{owner}_{code}_{n}_{rng.randint(0, 9)}", DERIVED_SOURCE_NAME, f"O{owner}",
                             code, "q", bucket, period, at, as_of])
    con.executemany("INSERT INTO derived_metric_values VALUES (?,?,?,?,?,?,?,?,?)", rows)
    formations = [rp.expected_month_end_session(2022 + (m // 12), m % 12 + 1) for m in range(3, 27)]
    return [(day, dt.datetime.combine(day, dt.time(22))) for day in formations]


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_cross_formation_selection_equals_the_fq1_ranking(seed):
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    calendar = _selection_fixture(con, seed)
    fsr.stage_metric_legs(con, [fsr.MetricLeg(code, "q", "a" * 64, 200, 400) for code in ("m_a", "m_b")],
                          table="_rp_metrics")
    rp._stage_run_calendar(con, [rp.CalendarRow(day.replace(day=1), day, day, day, cutoff, day, "formed")
                                 for day, cutoff in calendar])
    con.execute(f"""
        CREATE TEMP TABLE _rp_states AS
        SELECT d.*, {rp._BUCKET_SQL} AS bucket FROM derived_metric_values d
    """)
    stats = rp._stage_segments(con, len(calendar))
    fast = set(con.execute("""
        SELECT c.decision_date, s.metric_code, s.metric_window, s.security_id, s.derived_value_id
        FROM _rp_seg s JOIN _rp_cal_ord c ON c.ord>=s.from_ord AND c.ord<s.to_ord
    """).fetchall())
    fsr.stage_state_selection(con, calendar_table="_rp_calendar", metrics_table="_rp_metrics",
                              metric_source=DERIVED_SOURCE_NAME)
    exact = set(con.execute("SELECT * FROM _fs_selected_keys").fetchall())
    assert stats["irregular_owner_metrics"] > 0 and len(exact) > 50
    assert fast == exact
    # One selection per (formation, owner, metric): segments never overlap.
    assert len(fast) == len({row[:4] for row in fast})


def test_research_store_attaches_the_warehouse_read_only(tmp_path, warehouse):
    wh_path, _ = warehouse
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=wh_path) as store:
        assert store.status().versions == (1, 2, 3)
        with pytest.raises(duckdb.Error):
            store.con.execute("DELETE FROM derived_metric_values")
        assert store.warehouse_has("derived_metric_values", "selected_input_refs_hash")
        assert not store.warehouse_has("research_panel_runs")
    with pytest.raises(ValueError, match="must not be the warehouse"):
        ResearchStore(wh_path, warehouse_path=wh_path).open()


# ---------------------------------------------------------------------------
# Bounded memory at 256 MB / 1 thread on a 1M-row derived fixture
# ---------------------------------------------------------------------------

def _peak_working_set_mb() -> float | None:
    if sys.platform != "win32":
        return None

    class Counters(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t)]

    counters = Counters()
    counters.cb = ctypes.sizeof(Counters)
    kernel32, psapi = ctypes.WinDLL("kernel32"), ctypes.WinDLL("psapi")
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
    psapi.GetProcessMemoryInfo.restype = ctypes.c_int
    if not psapi.GetProcessMemoryInfo(kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
        return None
    return counters.PeakWorkingSetSize / 2**20


SCALE_CODES = (("m1_q", "q"), ("m2_q", "q"), ("m3_ttm", "ttm"), ("m4_ttm", "ttm"))


def scale_definitions() -> tuple[DerivedMetricDefinition, ...]:
    return tuple(DerivedMetricDefinition(
        metric_code=code, family="test", expression="x", window=window, inputs=("item:x",),
        requires_market=False, description="test", version="1") for code, window in SCALE_CODES)


class _Link:
    def __init__(self, i: int) -> None:
        self.price_security_id = f"SEC-CIK-{i:010d}"
        self.cik = f"{i:010d}"
        self.valid_from = dt.date(2020, 1, 2)
        self.valid_to = None
        self.available_at = dt.datetime(2020, 1, 2, 22)
        self.identity_basis = "current_ticker_unverified"
        self.link_method = "cik_security_id"
        self.unlinked_reason = None


def build_scale_warehouse(path: Path, issuers: int, quarters: int = 62) -> tuple[int, list[_Link]]:
    """issuers x 4 metrics x quarters x 2 revisions derived states, one leaf each."""
    defs = scale_definitions()
    con = duckdb.connect(str(path), config={"memory_limit": "256MB", "threads": "1"})
    con.execute("SET TimeZone='UTC'")
    con.execute("""
        CREATE TABLE derived_metric_definitions (metric_code VARCHAR, metric_window VARCHAR,
          expression VARCHAR, inputs_json VARCHAR, version VARCHAR);
        CREATE TABLE codes (ordinal INTEGER, metric_code VARCHAR, metric_window VARCHAR, definition_hash VARCHAR);
    """)
    for ordinal, definition in enumerate(defs):
        con.execute("INSERT INTO derived_metric_definitions VALUES (?,?,?,?,?)",
                    [definition.metric_code, definition.window, "x", '["item:x"]', "1"])
        con.execute("INSERT INTO codes VALUES (?,?,?,?)",
                    [ordinal, definition.metric_code, definition.window, pit.definition_hash(definition)])
    # Quarter q ends at month-end q*3 months after 2008-03-31; each has an
    # original filing (+40 days) and a revision (+100 days).
    con.execute(f"""
        CREATE TABLE grid AS
        SELECT i.i AS issuer, c.ordinal, c.metric_code, c.metric_window, c.definition_hash, q.q,
               r.r AS revision,
               last_day(DATE '2008-03-31' + to_months(3 * q.q)) AS period_end
        FROM range(1, {issuers + 1}) i(i), codes c, range(0, {quarters}) q(q), range(0, 2) r(r)
    """)
    con.execute("""
        CREATE TABLE fundamental_standardized AS
        SELECT 'L' || issuer || '_' || ordinal || '_' || q || '_' || revision AS standardized_id,
               'SEC-COMPANYFACTS-UNRESOLVED-CIK-' || lpad(CAST(issuer AS VARCHAR),10,'0') AS security_id,
               'x' AS canonical_code, CAST(issuer AS VARCHAR) AS cik, 'quarterly' AS basis,
               'test' AS source, CAST(NULL AS DATE) AS period_start, period_end,
               CAST(period_end AS TIMESTAMP) + to_days(CASE WHEN revision=0 THEN 40 ELSE 100 END)
                 + INTERVAL 17 HOUR AS available_at, 1.0 AS value
        FROM grid;
        ALTER TABLE fundamental_standardized ADD PRIMARY KEY (standardized_id);
    """)
    con.execute("""
        CREATE TABLE derived_metric_values AS
        WITH base AS (
          SELECT g.*, 'D' || issuer || '_' || ordinal || '_' || q || '_' || revision AS derived_value_id,
                 'L' || issuer || '_' || ordinal || '_' || q || '_' || revision AS leaf_id,
                 CAST(floor((year(period_end)*12+month(period_end)-1+
                   CASE WHEN day(period_end)>=15 THEN 1 ELSE 0 END)/3.0) AS BIGINT) AS bucket,
                 CAST(period_end AS TIMESTAMP) + to_days(CASE WHEN revision=0 THEN 40 ELSE 100 END)
                   + INTERVAL 18 HOUR AS available_at
          FROM grid g
        ), payloaded AS (
          SELECT *, '{"refs":[{"available_at":"' || strftime(available_at - INTERVAL 1 HOUR, '%Y-%m-%dT%H:%M:%S')
                    || '","basis":"quarterly","bucket":' || bucket || ',"cik":"' || issuer
                    || '","code":"x","kind":"item","offset":0,"period_end":"' || strftime(period_end, '%Y-%m-%d')
                    || '","period_start":null,"source":"test","state_id":"' || leaf_id
                    || '","status":"selected"}],"version":1}' AS payload
          FROM base
        )
        SELECT derived_value_id, 'atx-db declarative derived metrics v1' AS source,
               'SEC-COMPANYFACTS-UNRESOLVED-CIK-' || lpad(CAST(issuer AS VARCHAR),10,'0') AS security_id,
               metric_code, metric_window, bucket AS target_bucket, period_end,
               (issuer % 97) / 100.0 + revision / 1000.0 AS value, available_at,
               CAST(NULL AS TIMESTAMP) AS valid_to, repeat('b', 64) AS inputs_hash, definition_hash,
               'event_reconstructed' AS history_status, 'valid' AS value_status,
               payload AS selected_input_refs_json, sha256(payload) AS selected_input_refs_hash,
               CAST(available_at AS DATE) AS as_of_date, CAST(NULL AS DATE) AS fiscal_period_start,
               period_end AS fiscal_period_end, 'quarterly' AS value_origin
        FROM payloaded;
        ALTER TABLE derived_metric_values ADD PRIMARY KEY (derived_value_id);
    """)
    rows = con.execute("SELECT count(*) FROM derived_metric_values").fetchone()[0]
    sessions = _sessions(dt.date(2023, 1, 2), dt.date(2024, 1, 10))
    con.execute("""
        CREATE TABLE market_daily_metrics (market_daily_id VARCHAR, source VARCHAR, security_id VARCHAR,
          trade_date DATE, available_at TIMESTAMP, as_of_date DATE, close DOUBLE, volume BIGINT,
          fundamental_available_at TIMESTAMP);
        CREATE TABLE universe_us_listed_membership AS
        SELECT 'us_listed_reconstructed_v1' AS universe_id, ? AS source,
               'SEC-CIK-' || lpad(CAST(i AS VARCHAR),10,'0') AS security_id, 'S' || i AS symbol,
               'common' AS security_type, 'XNAS' AS exchange_code, true AS has_cik,
               lpad(CAST(i AS VARCHAR),10,'0') AS cik, 'member' AS reason, DATE '2020-01-02' AS valid_from,
               CAST(NULL AS DATE) AS valid_to, TIMESTAMP '2020-01-02 22:00:00' AS available_at,
               DATE '2020-01-02' AS as_of_date
        FROM range(1, ?) t(i);
    """, [UNIVERSE_SOURCE_NAME, issuers + 1])
    con.executemany("INSERT INTO market_daily_metrics VALUES (?,?,'SEC-CIK-0000000001',?,?,?,100.0,1000,NULL)",
                    [[f"m{day}", MARKET_DAILY_SOURCE_NAME, day, dt.datetime.combine(day, dt.time(21)), day]
                     for day in sessions])
    con.close()
    return int(rows), [_Link(i) for i in range(1, issuers + 1)]


# Realistic lineage shapes: multi-leaf depth-0 rollups, depth-1 and depth-2
# metrics over metric children, restatements (new leaves, a revised state, the
# superseded state closed by valid_to) and NULL-valued states.
REALISTIC_ITEMS = ("rev", "ni", "eq")
REALISTIC_METRICS = (
    # code, window, inputs, refs: ("item", code, offsets) or ("metric", code, offsets)
    ("rq_margin", "q", ("item:ni", "item:rev"), (("item", "ni", (0,)), ("item", "rev", (0,)))),
    ("ra_equity", "avg2", ("item:eq",), (("item", "eq", (0, 1)),)),
    ("rt_rev", "ttm", ("item:rev",), (("item", "rev", (0, 1, 2, 3)),)),
    ("rt_roe", "ttm", ("item:ni", "item:eq"), (("item", "ni", (0, 1, 2, 3)), ("item", "eq", (0, 4)))),
    ("gt_growth", "ttm", ("metric:rt_rev",), (("metric", "rt_rev", (0, 4)),)),
    ("gt_mix", "ttm", ("metric:rt_roe", "metric:rt_rev"), (("metric", "rt_roe", (0,)), ("metric", "rt_rev", (0,)))),
    ("ht_mix_growth", "ttm", ("metric:gt_mix",), (("metric", "gt_mix", (0, 4)),)),
)


def realistic_definitions() -> tuple[DerivedMetricDefinition, ...]:
    return tuple(DerivedMetricDefinition(
        metric_code=code, family="test", expression="x", window=window, inputs=inputs,
        requires_market=False, description="test", version="1") for code, window, inputs, _ in REALISTIC_METRICS)


def _quarter_end(q: int) -> dt.date:
    month = 3 * (q % 4) + 3
    year = 2011 + q // 4
    return dt.date(year, month, _calendar_days(year, month))


def _calendar_days(year: int, month: int) -> int:
    import calendar

    return calendar.monthrange(year, month)[1]


def _bulk(con, table: str, types: tuple[str, ...], rows: list[list]) -> None:
    if rows:
        columns = ",".join(f"unnest(CAST(? AS {kind}[]))" for kind in types)
        con.execute(f"INSERT INTO {table} SELECT {columns}", [list(column) for column in zip(*rows, strict=True)])


def build_realistic_warehouse(path: Path, issuers: int, quarters: int = 48, seed: int = 7,
                              first_session: dt.date = dt.date(2019, 1, 2),
                              last_session: dt.date = dt.date(2023, 1, 10)) -> dict[str, int]:
    """issuers x quarters x 7 metrics (2-20 leaves, depth 0-2) with restatements and NULLs."""
    import random

    from atx_db import _derived_annual as annual

    rng = random.Random(seed)
    defs = {d.metric_code: d for d in realistic_definitions()}
    hashes = {code: pit.definition_hash(d, annual.plan_for(d, defs)) for code, d in defs.items()}
    con = duckdb.connect(str(path), config={"memory_limit": "256MB", "threads": "1"})
    con.execute("SET TimeZone='UTC'")
    con.execute("""
        CREATE TABLE derived_metric_definitions (metric_code VARCHAR, metric_window VARCHAR,
          expression VARCHAR, inputs_json VARCHAR, version VARCHAR);
        CREATE TABLE fundamental_standardized (standardized_id VARCHAR PRIMARY KEY, security_id VARCHAR,
          canonical_code VARCHAR, cik VARCHAR, basis VARCHAR, source VARCHAR, period_start DATE,
          period_end DATE, available_at TIMESTAMP, value DOUBLE);
        CREATE TABLE derived_metric_values (derived_value_id VARCHAR PRIMARY KEY, source VARCHAR,
          security_id VARCHAR, metric_code VARCHAR, metric_window VARCHAR, target_bucket BIGINT,
          period_end DATE, value DOUBLE, available_at TIMESTAMP, valid_to TIMESTAMP, inputs_hash VARCHAR,
          definition_hash VARCHAR, history_status VARCHAR, value_status VARCHAR,
          selected_input_refs_json VARCHAR, selected_input_refs_hash VARCHAR, as_of_date DATE,
          fiscal_period_start DATE, fiscal_period_end DATE, value_origin VARCHAR);
    """)
    for code, d in defs.items():
        con.execute("INSERT INTO derived_metric_definitions VALUES (?,?,?,?,?)",
                    [code, d.window, d.expression, json.dumps(list(d.inputs), separators=(",", ":")), d.version])
    leaf_types = ("VARCHAR",) * 6 + ("DATE", "DATE", "TIMESTAMP", "DOUBLE")
    state_types = ("VARCHAR",) * 5 + ("BIGINT", "DATE", "DOUBLE", "TIMESTAMP", "TIMESTAMP") + ("VARCHAR",) * 6 \
        + ("DATE", "DATE", "DATE", "VARCHAR")
    counts = {"leaves": 0, "states": 0, "null_states": 0, "restated_quarters": 0}
    leaves: list[list] = []
    states: list[list] = []
    for issuer in range(1, issuers + 1):
        owner = f"SEC-COMPANYFACTS-UNRESOLVED-CIK-{issuer:010d}"
        restated = [rng.random() < 0.2 for _ in range(quarters)]
        counts["restated_quarters"] += sum(restated)

        def leaf(item: str, q: int, version: int, issuer: int = issuer) -> tuple[str, dt.datetime]:
            end = _quarter_end(q)
            return (f"L{issuer}_{item}_{q}_{version}",
                    dt.datetime.combine(end, dt.time(17)) + dt.timedelta(days=40 if version == 0 else 100))

        for q in range(quarters):
            end = _quarter_end(q)
            for item in REALISTIC_ITEMS:
                for version in (0, 1) if restated[q] else (0,):
                    leaf_id, at = leaf(item, q, version)
                    leaves.append([leaf_id, owner, item, str(issuer), "quarterly", "test", None, end, at,
                                   rng.uniform(1, 100)])
        state_index: dict[tuple[str, int, int], tuple[str, dt.datetime]] = {}
        depths: dict[str, int] = {}
        for code, window, _, spec in REALISTIC_METRICS:
            depth = depths[code] = max((1 + depths[ref] for kind, ref, _ in spec if kind == "metric"), default=0)
            for q in range(4 + 4 * depth, quarters):
                end = _quarter_end(q)
                versions = (0, 1) if restated[q] else (0,)
                for version in versions:
                    at = dt.datetime.combine(end, dt.time(18 + depth)) + dt.timedelta(
                        days=40 if version == 0 else 100)
                    refs = []
                    for kind, ref_code, offsets in spec:
                        for offset in offsets:
                            q_ref = q - offset
                            ref_version = version if offset == 0 else int(restated[q_ref])
                            ref_end = _quarter_end(q_ref)
                            if kind == "item":
                                leaf_id, leaf_at = leaf(ref_code, q_ref, ref_version)
                                refs.append({"kind": "item", "code": ref_code, "bucket": _bucket(ref_end),
                                             "offset": offset, "status": "selected", "state_id": leaf_id,
                                             "available_at": str(leaf_at), "cik": str(issuer),
                                             "basis": "quarterly", "source": "test", "period_start": None,
                                             "period_end": ref_end.isoformat()})
                            else:
                                child_id, child_at = state_index[(ref_code, q_ref, ref_version)]
                                refs.append({"kind": "metric", "code": ref_code, "bucket": _bucket(ref_end),
                                             "offset": offset, "status": "selected", "state_id": child_id,
                                             "available_at": str(child_at), "cik": None, "basis": None,
                                             "source": DERIVED_SOURCE_NAME, "period_start": None,
                                             "period_end": ref_end.isoformat(), "inputs_hash": "b" * 64,
                                             "definition_hash": hashes[ref_code]})
                    payload = json.dumps({"version": 1, "refs": refs}, sort_keys=True, separators=(",", ":"))
                    state_id = hashlib.sha256(f"{owner}|{code}|{q}|{version}".encode()).hexdigest()
                    state_index[(code, q, version)] = (state_id, at)
                    null = rng.random() < 0.05
                    counts["null_states"] += null
                    superseded = version == 0 and restated[q]
                    valid_to = dt.datetime.combine(end, dt.time(18 + depth)) + dt.timedelta(days=100) \
                        if superseded else None
                    states.append([state_id, DERIVED_SOURCE_NAME, owner, code, window, _bucket(end), end,
                                   None if null else rng.uniform(-1, 1), at, valid_to, "b" * 64, hashes[code],
                                   "event_reconstructed", "missing_input_or_domain" if null else "valid",
                                   payload, hashlib.sha256(payload.encode()).hexdigest(), at.date(), None, end,
                                   "quarterly"])
        if len(states) > 20_000 or issuer == issuers:
            counts["leaves"] += len(leaves)
            counts["states"] += len(states)
            _bulk(con, "fundamental_standardized", leaf_types, leaves)
            _bulk(con, "derived_metric_values", state_types, states)
            leaves, states = [], []
    sessions = _sessions(first_session, last_session)
    con.execute("""
        CREATE TABLE market_daily_metrics (market_daily_id VARCHAR, source VARCHAR, security_id VARCHAR,
          trade_date DATE, available_at TIMESTAMP, as_of_date DATE, close DOUBLE, volume BIGINT,
          fundamental_available_at TIMESTAMP);
        CREATE TABLE universe_us_listed_membership AS
        SELECT 'us_listed_reconstructed_v1' AS universe_id, ? AS source,
               'SEC-CIK-' || lpad(CAST(i AS VARCHAR),10,'0') AS security_id, 'S' || i AS symbol,
               'common' AS security_type, 'XNAS' AS exchange_code, true AS has_cik,
               lpad(CAST(i AS VARCHAR),10,'0') AS cik, 'member' AS reason, DATE '2010-01-04' AS valid_from,
               CAST(NULL AS DATE) AS valid_to, TIMESTAMP '2010-01-04 22:00:00' AS available_at,
               DATE '2010-01-04' AS as_of_date
        FROM range(1, ?) t(i);
    """, [UNIVERSE_SOURCE_NAME, issuers + 1])
    _bulk(con, "market_daily_metrics", ("VARCHAR", "VARCHAR", "VARCHAR", "DATE", "TIMESTAMP", "DATE", "DOUBLE",
                                        "BIGINT", "TIMESTAMP"),
          [[f"m{day}", MARKET_DAILY_SOURCE_NAME, "SEC-CIK-0000000001", day, dt.datetime.combine(day, dt.time(21)),
            day, 100.0, 1000, None] for day in sessions])
    con.close()
    return counts


def realistic_links(issuers: int, unlinked_share: float = 0.0) -> list[_Link]:
    """One reconstructed link per issuer line; the first ``unlinked_share`` of every 10 stay unlinked."""
    links = [_Link(i) for i in range(1, issuers + 1)]
    for link in links:
        link.valid_from, link.available_at = dt.date(2010, 1, 4), dt.datetime(2010, 1, 4, 22)
        if int(link.cik) % 10 < round(unlinked_share * 10):
            link.cik, link.link_method, link.unlinked_reason = None, None, "no_current_ticker"
    return links


@pytest.mark.slow
def test_realistic_multileaf_lineage_panel(tmp_path, monkeypatch):
    defs = realistic_definitions()
    monkeypatch.setattr(fsr, "default_derived_definitions", lambda: defs)
    issuers = 60
    counts = build_realistic_warehouse(tmp_path / "wh.duckdb", issuers, quarters=48)
    options = rp.ResearchPanelOptions(
        run_id="real", basis=rp.BASIS_RECONSTRUCTED, start_month=dt.date(2021, 1, 1), end_month=dt.date(2022, 12, 1),
        as_of_date=dt.date(2023, 1, 10), run_at=dt.datetime(2023, 1, 11, tzinfo=dt.UTC), metric_batch_size=6,
        features=tuple(rp.PanelFeature(code, code, window) for code, window, _, _ in REALISTIC_METRICS))
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=tmp_path / "wh.duckdb") as store:
        result = rp.build_research_panel(store, options, owner_links=realistic_links(issuers))
        con = store.con
        methods = dict(con.execute("""
            SELECT method, count(*) FROM research_lineage_proofs WHERE run_id='real' GROUP BY 1""").fetchall())
        statuses = dict(con.execute("""
            SELECT status, count(*) FROM research_lineage_proofs WHERE run_id='real' GROUP BY 1""").fetchall())
        # Spot-check the set-based rows against the Python resolver (all shapes).
        sample = con.execute("""
            SELECT derived_value_id, status, reason, selected_cik, proof_digest, leaf_count
            FROM research_lineage_proofs WHERE run_id='real' USING SAMPLE 40 ROWS (reservoir, 3)
        """).fetchall()
        _, _, expected = fsr.resolve_definitions(con, {(c, w) for c, w, _, _ in REALISTIC_METRICS})
        python = derived_lineage.qualify_selected_lineage(
            con, [row[0] for row in sample], expected_cik=None, decision_cutoff=None,
            expected_definition_hashes=expected, max_depth=16, max_nodes=512, max_bytes=1_048_576)
        rp.validate_research_panel(store, "real")
    assert counts["null_states"] > 0 and counts["restated_quarters"] > 0
    assert result.status == "complete" and result.formations == 24
    assert set(methods) == {"set_exact", "set_certified"}  # no Python fallback on well-formed lineage
    assert statuses.keys() == {"qualified", "invalid"}
    for root, status, reason, cik, digest, leaves in sample:
        proof = python[root]
        assert (status, reason, cik, digest, leaves) == (proof.status, proof.reason, proof.selected_cik,
                                                         proof.digest, len(proof.leaf_ids))


SCALE_OPTIONS = dict(basis=rp.BASIS_RECONSTRUCTED, start_month=dt.date(2023, 1, 1),
                     end_month=dt.date(2023, 12, 1), as_of_date=dt.date(2024, 1, 10),
                     run_at=dt.datetime(2024, 1, 11, tzinfo=dt.UTC), metric_batch_size=4,
                     features=tuple(rp.PanelFeature(code, code, window) for code, window in SCALE_CODES))


@pytest.mark.slow
def test_one_million_derived_rows_stay_memory_bounded(tmp_path, monkeypatch):
    defs = scale_definitions()
    monkeypatch.setattr(fsr, "default_derived_definitions", lambda: defs)
    issuers = 2000
    path = tmp_path / "wh.duckdb"
    rows, links = build_scale_warehouse(path, issuers)
    assert rows >= 990_000
    options = rp.ResearchPanelOptions(run_id="big", **SCALE_OPTIONS)
    tracemalloc.start()
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=path,
                       memory_limit="256MB", threads=1) as store:
        result = rp.build_research_panel(store, options, owner_links=links)
        _, python_peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        proofs = store.con.execute("SELECT count(*) FROM research_lineage_proofs").fetchone()[0]
        diagnostic = store.con.execute("SELECT diagnostic_json FROM research_panel_runs").fetchone()[0]
    working_set = _peak_working_set_mb()
    print(f"R2A_MEMORY rows={rows} status={result.status} formations={result.formations} "
          f"values={result.value_rows} valid={result.valid_values} proofs={proofs} "
          f"python_peak_mb={python_peak / 2**20:.1f} process_peak_working_set_mb={working_set}")
    print(f"R2A_DIAGNOSTIC {diagnostic}")
    assert result.status == "complete"
    assert result.formations == 12
    assert result.valid_values == 12 * 4 * issuers
    assert python_peak < 300 * 2**20
