"""Point-in-time contracts for the monthly research panel (task R2a)."""

from __future__ import annotations

import ctypes
import datetime as dt
import hashlib
import json
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
    market = tuple(DerivedMetricDefinition(
        metric_code=code, family="market", expression="close", window="daily", inputs=("item:x",),
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
              available_at TIMESTAMP, value DOUBLE);
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
              trade_date DATE, available_at TIMESTAMP, as_of_date DATE, close DOUBLE,
              fundamental_available_at TIMESTAMP, market_cap DOUBLE, momentum_12_1 DOUBLE);
            CREATE TABLE equity_daily_bars (
              security_id VARCHAR, symbol VARCHAR, trade_date DATE, close DOUBLE,
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
                first: dt.date, last: dt.date, reason: str = "member", ticker: bool = True) -> None:
        for day in (d for d in self.sessions if first <= d <= last):
            self.con.execute("INSERT INTO equity_daily_bars VALUES (?,?,?,100.0,'test_bars','1')",
                             [line, symbol, day])
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
                INSERT INTO market_daily_metrics VALUES (?,?,?,?,?,?,100.0,NULL,?,0.1)
            """, [f"{line}|{day}", MARKET_DAILY_SOURCE_NAME, line, day, cutoff, day,
                  1000.0 * cik + day.toordinal() % 7])

    def state(self, cik: int, code: str, window: str, period_end: dt.date, value: float | None,
              clock: dt.datetime, *, origin: str = "quarterly", state_id: str | None = None) -> str:
        state_id = state_id or f"{cik}_{code}_{period_end}_{clock:%Y%m%d%H%M}"
        owner = f"SEC-COMPANYFACTS-UNRESOLVED-CIK-{cik:010d}"
        leaf_id = f"leaf_{state_id}"
        leaf_clock = clock - dt.timedelta(hours=1)
        bucket = _bucket(period_end)
        self.con.execute("INSERT INTO fundamental_standardized VALUES (?,?,'x',?,'quarterly','test',NULL,?,?,1.0)",
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
        wh.listing(cik, line, symbol, security_type, exchange, first=dt.date(2023, 9, 1), last=AS_OF)
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
    return ids


@pytest.fixture
def warehouse(tmp_path, registry):
    wh = Warehouse(tmp_path / "wh.duckdb")
    ids = _populate(wh)
    wh.close()
    return tmp_path / "wh.duckdb", ids


def _options(run_id: str, basis: str = rp.BASIS_RECONSTRUCTED, **extra) -> rp.ResearchPanelOptions:
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
        validated = rp.validate_research_panel(store, "recon")
        assert validated.panel_sha256 == result.panel_sha256
        assert validated.formations == tuple(FORMATIONS)
        con.execute("""
            UPDATE research_panel_values SET raw_value=raw_value+1
            WHERE run_id='recon' AND formation_date='2024-01-31' AND feature_id='roa_q'
        """)
        with pytest.raises(ValueError, match="value digest mismatch"):
            rp.validate_research_panel(store, "recon")


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
            SELECT DISTINCT identity_basis, universe_basis FROM research_panel_values
            WHERE run_id='recon' ORDER BY 1
        """).fetchall() == [("current_ticker_unverified", "us_listed_reconstructed_v1")]
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
    assert repeats > 0  # the same state is selected at several month ends ...
    assert len(calls) == len(set(calls))  # ... but qualified exactly once
    assert selected <= set(calls)
    assert ids["q3_1_rev"] in calls


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
        real = rp._derived_batch

        def fail_in_january(store_, run_id, batch, spec, hashes):
            if con.execute("SELECT decision_date FROM _rp_calendar").fetchone()[0] == dt.date(2024, 1, 31):
                raise RuntimeError("injected failure")
            return real(store_, run_id, batch, spec, hashes)

        monkeypatch.setattr(rp, "_derived_batch", fail_in_january)
        with pytest.raises(RuntimeError, match="injected failure"):
            rp.build_research_panel(store, _options("resumed", metric_batch_size=2))
        assert con.execute("SELECT status FROM research_panel_runs WHERE run_id='resumed'").fetchone()[0] == "failed"
        with pytest.raises(ValueError, match="not sealed"):
            rp.validate_research_panel(store, "resumed")
        monkeypatch.setattr(rp, "_derived_batch", real)
        with pytest.raises(ValueError, match="already exists"):
            rp.build_research_panel(store, _options("resumed", metric_batch_size=2))
        resumed = rp.build_research_panel(store, _options("resumed", metric_batch_size=2, resume=True))
        assert resumed.status == "complete"
        assert resumed.panel_sha256 == two.panel_sha256
        assert digests("resumed") == digests("two")
        with pytest.raises(ValueError, match="sealed"):
            rp.build_research_panel(store, _options("resumed", metric_batch_size=2, resume=True))


def test_research_store_attaches_the_warehouse_read_only(tmp_path, warehouse):
    wh_path, _ = warehouse
    with ResearchStore(tmp_path / "research.duckdb", warehouse_path=wh_path) as store:
        assert store.status().versions == (1,)
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
          trade_date DATE, available_at TIMESTAMP, as_of_date DATE, close DOUBLE,
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
    con.executemany("INSERT INTO market_daily_metrics VALUES (?,?,'SEC-CIK-0000000001',?,?,?,100.0,NULL)",
                    [[f"m{day}", MARKET_DAILY_SOURCE_NAME, day, dt.datetime.combine(day, dt.time(21)), day]
                     for day in sessions])
    con.close()
    return int(rows), [_Link(i) for i in range(1, issuers + 1)]


SCALE_OPTIONS = dict(basis=rp.BASIS_RECONSTRUCTED, start_month=dt.date(2023, 1, 1),
                     end_month=dt.date(2023, 12, 1), as_of_date=dt.date(2024, 1, 10),
                     run_at=dt.datetime(2024, 1, 11, tzinfo=dt.UTC), metric_batch_size=4)


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
        proofs = store.con.execute("SELECT count(*) FROM research_panel_proofs").fetchone()[0]
    working_set = _peak_working_set_mb()
    print(f"R2A_MEMORY rows={rows} status={result.status} formations={result.formations} "
          f"values={result.value_rows} valid={result.valid_values} proofs={proofs} "
          f"python_peak_mb={python_peak / 2**20:.1f} process_peak_working_set_mb={working_set}")
    assert result.status == "complete"
    assert result.formations == 12
    assert result.valid_values == 12 * 4 * issuers
    assert python_peak < 300 * 2**20
