"""Earnings-event dataset and event features (task P3).

For one sealed R2a monthly panel run (one basis) this module builds one immutable,
content-addressed *event version*: one row per (owner, fiscal period) in
``research_earnings_events`` with the announcement moment, its exchange session and the
event returns, plus the formation-aligned view ``research_event_features`` that joins the
panel's owner lines.

Events (one row per owner x fiscal period)
------------------------------------------
Owners are the CIKs of the panel's owner-linked primary lines (``cohort_reason='valid'``).
A fiscal period is a duration (quarter, year-to-date or annual) that ends on
``fundamental_periods.period_end`` in an original periodic filing (10-K, 10-Q, 10-KT,
10-QT; amendments never define an event). The original is the earliest filing carrying
the period; a period first seen more than ``max_filing_lag_days`` after its end (the
comparative columns of a later filing, pre-XBRL history) is excluded and counted, and
when one filing carries several period ends the one with the most statement points is
the event (the rest are counted).

Announcement (``announcement_basis``)
-------------------------------------
``8k_202``
    The earliest 8-K with Item 2.02 of the owner's CIK whose announcement date
    ``rdq = coalesce(report_date, filing_date, acceptance date)`` (the same rule as
    ``fundamental_periods.rdq``) lies in ``[period_end, period_end + max_filing_lag_days]``.
    An 8-K whose ``rdq`` is after the periodic filing date is **rejected and counted**
    (``rdq_rejection='rdq_after_filing_date'``): the periodic filing disclosed first.
``periodic_filing``
    Fallback (no 8-K 2.02 in the window, or the rejection above): the original
    periodic filing itself, labeled. It is a late proxy for the earnings release.
The stored ``fundamental_periods.rdq`` is kept as ``period_rdq`` and its agreement
with the matched 8-K is counted; the matching is redone by CIK because the
acceptance time (``rdq_available_at``) is not persisted there and the security-id join
can miss.

Session attribution (exchange session calendar ``XNYS`` / ``equity_daily_bars calendar``,
the calendar the labels and the panel use): the EDGAR acceptance stamp (naive UTC in
``sec_submissions``; measured on the retained submissions archive: 8-K 2.02 stamps shift
by one hour across DST, so they are true UTC) is converted to America/New_York.

* on a session day before 09:30 ET -> ``pre_open``: event session = that day;
* 09:30 to 16:00 ET -> ``intraday``: that day;
* at or after 16:00 ET -> ``after_close``: the next session;
* not a session day -> ``non_session_day``: the next session;
* no usable time (missing, zone-less raw stamp, EDGAR date-only midnight, or an 8-K
  accepted after the day its report is dated: the release preceded the filing) ->
  ``unknown_time``: the next session after the announcement date (conservative; the
  [-1, +1] window still covers the announcement day).
Early-close sessions are not modeled (16:00 assumed; labeled blocker).

Clocks (point in time)
----------------------
``evidence_available_at`` = the later of the acceptance stamp and the SEC filed date +
46 h (FC1 policy ``sec_filed_date_plus_46h_v1``). ``event_available_at`` = the later of
that and the close clock (22:00 UTC, the bar convention) of the session after the event
session: **no event feature is visible before the close of event session + 1**. EAR and
run-up are also no earlier than their input bars and market returns. SUE carries its
own clock: ``sue_ni`` (NI seasonal change over the prior-8 sd, share-basis-free) from
``derived_metric_values`` at its own ``available_at`` (10-Q + 46 h), never earlier, and
never before the event clock.

Returns
-------
Daily one-session returns of ``adjusted_close`` (the forward-label bar pick); the market
is the equal-weighted mean over the panel's eligible lines of the latest formation
before the day (linked and unlinked, the delisted tail included), stored per day in
``research_event_market`` (value weighting waits for P4). With ``AR_d = r_d - m_d``:

* ``ear_m1p1`` = sum of AR over sessions E-1, E, E+1 (all three required);
* ``runup_m21_m2`` = sum of AR over sessions E-21 .. E-2 (all twenty required).
The price line is the owner's primary line at the latest formation before E.

Formation view
--------------
``research_event_features``: dense over the panel's valid primary lines x
(:data:`EVENT_FEATURES`) per formation. Each feature takes the owner's latest fiscal
period whose value clock is at or before the formation cutoff and whose event session is
at most ``max_age_days`` old (else ``no_recent_event``); ``days_since_announcement`` =
formation date - event session (calendar days); ``sue`` takes the newest ``sue_ni``
revision visible at the cutoff of the latest period that has one (a SUE that is not yet
visible leaves the previous period's in place: its absence never leaks).

Versions: ``event_version`` = sha256 of the spec (panel seal, parameters, input
fingerprints of every warehouse relation read) and the code digest. A sealed version is
returned unchanged; a ``building``/``failed`` one is rebuilt. Tables live in the research
store (RX6); the schema is owned here until registered as a store migration.
"""

from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import json
import re
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import duckdb

from .. import _forward_return_publication as _publication
from .._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from ..derived_registry import DERIVED_SOURCE_NAME
from . import panel as _panel
from .store import ResearchStore

QUERY_VERSION = "research-earnings-events-v1"
EVENT_SCHEMA_VERSION = 1
KNOWN_SCHEMA_VERSIONS = (1,)

BASIS_8K = "8k_202"
BASIS_PERIODIC = "periodic_filing"
ANNOUNCEMENT_BASES = (BASIS_8K, BASIS_PERIODIC)
REJECT_NO_8K = "no_8k_202_in_window"
REJECT_AFTER_FILING = "rdq_after_filing_date"

TIMING_PRE_OPEN = "pre_open"
TIMING_INTRADAY = "intraday"
TIMING_AFTER_CLOSE = "after_close"
TIMING_NON_SESSION = "non_session_day"
TIMING_UNKNOWN = "unknown_time"
SAME_SESSION_TIMINGS = (TIMING_PRE_OPEN, TIMING_INTRADAY)

CLOCK_EXACT = "exact_offset"
CLOCK_NORMALIZED = "normalized_utc_unqualified"
CLOCK_ZONE_UNKNOWN = "zone_unknown"
CLOCK_DATE_ONLY = "date_only"
CLOCK_MISSING = "missing"
TIMED_CLOCKS = (CLOCK_EXACT, CLOCK_NORMALIZED)

EXCHANGE_TIMEZONE = "America/New_York"
SESSION_OPEN_ET = "09:30:00"
SESSION_CLOSE_ET = "16:00:00"
BAR_CLOCK_HOURS = 22
EVIDENCE_FLOOR_HOURS = 46
PRICE_BASIS = "adjusted_close"
MARKET_BASIS = "equal_weight_panel_eligible_lines"
EAR_WINDOW = (-1, 1)
RUNUP_WINDOW = (-21, -2)
SUE_METRIC = "sue_ni"
SUE_WINDOW = "q"
PERIODIC_FORMS = ("10-K", "10-Q", "10-KT", "10-QT")
DURATION_PERIOD_TYPES = ("quarter", "semiannual_ytd", "multi_quarter_ytd", "annual")

FEATURE_EAR = "ear_m1p1"
FEATURE_RUNUP = "runup_m21_m2"
FEATURE_DAYS_SINCE = "days_since_announcement"
FEATURE_SUE = "sue"
EVENT_FEATURES = (FEATURE_EAR, FEATURE_RUNUP, FEATURE_DAYS_SINCE, FEATURE_SUE)

VALID = "valid"
REASON_NO_CLOCK = "no_announcement_clock"
REASON_NOT_MATURED = "event_window_not_matured"
REASON_NO_FORMATION = "no_prior_formation"
REASON_NO_LINE = "no_owner_price_line"
REASON_INCOMPLETE = "incomplete_price_window"
REASON_NO_MARKET = "missing_market_return"
REASON_NO_SUE = "missing_sue"
REASON_NO_RECENT = "no_recent_event"
REASON_NO_RECENT_SUE = "no_recent_visible_sue"

STATUS_BUILDING = "building"
STATUS_SEALED = "sealed"
STATUS_EMPTY = "sealed_empty"
STATUS_FAILED = "failed"
SEALED_STATUSES = (STATUS_SEALED, STATUS_EMPTY)

MARKET_BLOCKER = "market_return_equal_weight_only_value_weight_pending_p4"
EARLY_CLOSE_BLOCKER = "early_close_sessions_assume_1600_close"
WIRE_BLOCKER = "announcement_clock_is_edgar_acceptance_not_newswire_release"
RECONSTRUCTED_BLOCKER = "reconstructed_identity_universe_and_availability_not_certifiable"

REQUIRED_WAREHOUSE_TABLES = ("fundamental_periods", "sec_submissions", "trading_calendar", "equity_daily_bars",
                             "derived_metric_values")
_VERSION_TABLES = ("research_event_features", "research_event_market", "research_earnings_events",
                   "research_event_versions")
_TEMP_TABLES = ("_ev_cal", "_ev_forms", "_ev_cohort", "_ev_owners", "_ev_universe", "_ev_universe_lines",
                "_ev_day_formation", "_ev_period_rows", "_ev_originals", "_ev_base", "_ev_8k", "_ev_periodic_acc",
                "_ev_match", "_ev_events", "_ev_windows", "_ev_chunk", "_ev_chunk_lines", "_ev_bars", "_ev_ret",
                "_ev_sue_rev", "_ev_candidates")
_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_CODE_FILES = (Path(__file__), Path(_publication.__file__))


class EventStoreError(ValueError):
    """The event store cannot build or validate a version under its contract."""


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False, default=str)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _sql_list(values: Sequence[str]) -> str:
    return ",".join("'" + value.replace("'", "''") + "'" for value in values)


def _code_sha() -> str:
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


def _cik(expression: str) -> str:
    """Ten-digit CIK text of a SQL expression (the panel's ``owner_cik`` spelling)."""
    return f"lpad(regexp_replace(CAST({expression} AS VARCHAR), '[^0-9]', '', 'g'), 10, '0')"


def _calendar_keys() -> tuple[str, str]:
    # The labels' and the panel's observed session calendar.
    from ..delisting import _TRADING_CALENDAR_ID, _TRADING_CALENDAR_SOURCE

    return _TRADING_CALENDAR_ID, _TRADING_CALENDAR_SOURCE


# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

def ensure_event_schema(con: Any) -> None:
    """Create the ``research_event*`` tables (schema v1); refuse a newer unknown schema."""
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_event_schema (
            version INTEGER PRIMARY KEY, name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)""")
    versions = {int(row[0]) for row in con.execute("SELECT version FROM research_event_schema").fetchall()}
    if versions - set(KNOWN_SCHEMA_VERSIONS):
        raise RuntimeError(f"research event schema has unknown versions {sorted(versions)}; code is older")
    if EVENT_SCHEMA_VERSION in versions:
        return
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_event_versions (
            event_version VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            panel_run_id VARCHAR NOT NULL,
            panel_sha256 VARCHAR NOT NULL,
            query_version VARCHAR NOT NULL,
            spec_json VARCHAR NOT NULL,
            spec_sha256 VARCHAR NOT NULL,
            code_sha256 VARCHAR NOT NULL,
            inputs_json VARCHAR NOT NULL,
            blockers_json VARCHAR NOT NULL,
            diagnostic_json VARCHAR,
            events_sha256 VARCHAR,
            features_sha256 VARCHAR,
            created_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS research_earnings_events (
            event_version VARCHAR NOT NULL,
            owner_cik VARCHAR NOT NULL,
            fiscal_period_end DATE NOT NULL,
            period_security_id VARCHAR,
            periodic_accession VARCHAR NOT NULL,
            periodic_form VARCHAR,
            periodic_filing_date DATE,
            period_rdq DATE,
            announcement_basis VARCHAR NOT NULL,
            rdq_rejection VARCHAR,
            announcement_accession VARCHAR,
            rdq DATE,
            acceptance_at TIMESTAMP,
            acceptance_local_et TIMESTAMP,
            acceptance_clock_status VARCHAR NOT NULL,
            timing_source VARCHAR NOT NULL,
            session_timing VARCHAR NOT NULL,
            announcement_date DATE,
            event_session DATE,
            evidence_available_at TIMESTAMP,
            event_available_at TIMESTAMP,
            event_reason VARCHAR NOT NULL,
            security_id VARCHAR,
            line_formation_date DATE,
            ear_m1p1 DOUBLE,
            ear_reason VARCHAR NOT NULL,
            ear_available_at TIMESTAMP,
            runup_m21_m2 DOUBLE,
            runup_reason VARCHAR NOT NULL,
            runup_available_at TIMESTAMP,
            sue DOUBLE,
            sue_reason VARCHAR NOT NULL,
            sue_period_end DATE,
            sue_derived_value_id VARCHAR,
            sue_first_available_at TIMESTAMP,
            sue_available_at TIMESTAMP,
            market_basis VARCHAR NOT NULL,
            PRIMARY KEY (event_version, owner_cik, fiscal_period_end)
        );
        CREATE TABLE IF NOT EXISTS research_event_market (
            event_version VARCHAR NOT NULL,
            trade_date DATE NOT NULL,
            universe_formation_date DATE NOT NULL,
            market_return DOUBLE NOT NULL,
            names BIGINT NOT NULL,
            extreme_returns BIGINT NOT NULL,
            available_at TIMESTAMP NOT NULL,
            PRIMARY KEY (event_version, trade_date)
        );
        CREATE TABLE IF NOT EXISTS research_event_features (
            event_version VARCHAR NOT NULL,
            formation_date DATE NOT NULL,
            security_id VARCHAR NOT NULL,
            owner_cik VARCHAR NOT NULL,
            feature_id VARCHAR NOT NULL,
            raw_value DOUBLE,
            reason VARCHAR NOT NULL,
            available_at TIMESTAMP,
            fiscal_period_end DATE,
            event_session DATE,
            announcement_basis VARCHAR
        );
    """)
    con.execute("INSERT INTO research_event_schema VALUES (?, ?, ?)",
                [EVENT_SCHEMA_VERSION, "earnings_events_v1", _now()])


# ---------------------------------------------------------------------------
# Options, spec and version identity
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class EarningsEventOptions:
    """What determines an event version (``formation_chunk`` is execution only)."""

    panel_run_id: str
    max_filing_lag_days: int = 200
    max_age_days: int = _panel.DEFAULT_MAX_AGE_DAYS
    sue_period_tolerance_days: int = 7
    formation_chunk: int = 12
    verify_panel: bool = False


@dataclass(frozen=True)
class EarningsEventResult:
    event_version: str
    status: str
    reused: bool
    events: int
    feature_rows: int
    blockers: tuple[str, ...]
    diagnostic: dict[str, Any]


def _validate_options(options: EarningsEventOptions) -> EarningsEventOptions:
    if not isinstance(options.panel_run_id, str) or not _ID.fullmatch(options.panel_run_id):
        raise EventStoreError("panel_run_id must be a lower-case identifier")
    for label, value, low, high in (("max_filing_lag_days", options.max_filing_lag_days, 30, 730),
                                    ("max_age_days", options.max_age_days, 1, 1000),
                                    ("sue_period_tolerance_days", options.sue_period_tolerance_days, 0, 30),
                                    ("formation_chunk", options.formation_chunk, 1, 600)):
        if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
            raise EventStoreError(f"{label} must be an integer {low}..{high}")
    return options


def _panel_context(store: ResearchStore, options: EarningsEventOptions) -> dict[str, Any]:
    row = store.con.execute("SELECT status, basis, panel_sha256 FROM research_panel_runs WHERE run_id=?",
                            [options.panel_run_id]).fetchone()
    if row is None:
        raise EventStoreError(f"panel run {options.panel_run_id!r} is absent")
    status, basis, panel_sha = row
    if status not in ("complete", "untestable_strict") or not panel_sha:
        raise EventStoreError(f"panel run {options.panel_run_id} is {status!r}; only sealed complete or "
                              "untestable_strict runs carry events")
    if options.verify_panel:
        _panel.validate_research_panel(store, options.panel_run_id)
    return {"status": str(status), "basis": str(basis), "panel_sha256": str(panel_sha)}


def _input_fingerprints(con: Any, calendar_id: str, calendar_source: str) -> dict[str, Any]:
    """Aggregate fingerprints of every warehouse relation read (part of the version identity)."""
    queries = {
        "fundamental_periods": """
            SELECT count(*), min(available_at), max(available_at), max(rdq) FROM fundamental_periods""",
        "sec_submissions": "SELECT count(*), max(source_loaded_at), max(acceptance_datetime) FROM sec_submissions",
        "equity_daily_bars": "SELECT count(*), max(source_loaded_at), max(trade_date) FROM equity_daily_bars",
        "derived_metric_values": """
            SELECT count(*), max(source_loaded_at), max(available_at) FROM derived_metric_values
            WHERE source=? AND metric_code=? AND metric_window=?""",
        "trading_calendar": """
            SELECT count(*), min(trade_date), max(trade_date) FROM trading_calendar
            WHERE calendar_id=? AND source=? AND is_open""",
    }
    params = {"derived_metric_values": [DERIVED_SOURCE_NAME, SUE_METRIC, SUE_WINDOW],
              "trading_calendar": [calendar_id, calendar_source]}
    out: dict[str, Any] = {}
    for table, sql in queries.items():
        values = con.execute(sql, params.get(table, [])).fetchone()
        out[table] = [None if value is None else str(value) for value in values]
    return out


def _spec(options: EarningsEventOptions, context: dict[str, Any], calendar: tuple[str, str],
          inputs: dict[str, Any], raw_acceptance: bool) -> dict[str, Any]:
    return {
        "query_version": QUERY_VERSION,
        "panel_run_id": options.panel_run_id,
        "panel_sha256": context["panel_sha256"],
        "basis": context["basis"],
        "periodic_forms": list(PERIODIC_FORMS),
        "duration_period_types": list(DURATION_PERIOD_TYPES),
        "max_filing_lag_days": options.max_filing_lag_days,
        "max_age_days": options.max_age_days,
        "session": {"timezone": EXCHANGE_TIMEZONE, "open": SESSION_OPEN_ET, "close": SESSION_CLOSE_ET,
                    "early_close": "not_modeled", "calendar_id": calendar[0], "calendar_source": calendar[1]},
        "clocks": {"evidence_policy": FUNDAMENTAL_CLOCK_POLICY, "evidence_floor_hours": EVIDENCE_FLOOR_HOURS,
                   "bar_clock_hours": BAR_CLOCK_HOURS, "raw_acceptance_column": raw_acceptance},
        "returns": {"price_basis": PRICE_BASIS, "market_basis": MARKET_BASIS, "ear_window": list(EAR_WINDOW),
                    "runup_window": list(RUNUP_WINDOW), "abnormal": "sum_of_daily_return_minus_market"},
        "sue": {"source": DERIVED_SOURCE_NAME, "metric_code": SUE_METRIC, "metric_window": SUE_WINDOW,
                "period_tolerance_days": options.sue_period_tolerance_days},
        "features": list(EVENT_FEATURES),
        "inputs": inputs,
        "duckdb": duckdb.__version__,
    }


# ---------------------------------------------------------------------------
# Staging
# ---------------------------------------------------------------------------

def _stage_panel(con: Any, run_id: str, calendar: tuple[str, str]) -> dict[str, Any]:
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_cal AS
        SELECT trade_date, CAST(row_number() OVER (ORDER BY trade_date) AS INTEGER) AS session_number
        FROM (SELECT DISTINCT trade_date FROM trading_calendar WHERE calendar_id=? AND source=? AND is_open)
    """, list(calendar))
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_forms AS
        SELECT formation_date, cutoff FROM research_panel_calendar
        WHERE run_id=? AND status=? AND formation_date IS NOT NULL AND cutoff IS NOT NULL
    """, [run_id, _panel.CALENDAR_FORMED])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_cohort AS
        SELECT c.formation_date, c.security_id, c.owner_cik, coalesce(c.eligible, false) AS eligible,
               (c.cohort_reason='valid' AND coalesce(c.primary_line, false) AND c.owner_cik IS NOT NULL)
                   AS valid_primary
        FROM research_panel_cohort c SEMI JOIN _ev_forms f ON f.formation_date=c.formation_date
        WHERE c.run_id=?
    """, [run_id])
    con.execute("CREATE OR REPLACE TEMP TABLE _ev_owners AS SELECT DISTINCT owner_cik FROM _ev_cohort "
                "WHERE valid_primary")
    con.execute("CREATE OR REPLACE TEMP TABLE _ev_universe AS SELECT DISTINCT formation_date, security_id "
                "FROM _ev_cohort WHERE eligible")
    con.execute("CREATE OR REPLACE TEMP TABLE _ev_universe_lines AS SELECT DISTINCT security_id FROM _ev_universe")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_day_formation AS
        SELECT s.trade_date, max(f.formation_date) AS formation_date
        FROM _ev_cal s JOIN _ev_forms f ON f.formation_date < s.trade_date GROUP BY s.trade_date
    """)
    sessions, formations, owners, lines = con.execute("""
        SELECT (SELECT count(*) FROM _ev_cal), (SELECT count(*) FROM _ev_forms), (SELECT count(*) FROM _ev_owners),
               (SELECT count(*) FROM _ev_cohort WHERE valid_primary)
    """).fetchone()
    return {"sessions": int(sessions), "formations": int(formations), "owners": int(owners),
            "owner_line_formations": int(lines)}


def _stage_periods(con: Any, options: EarningsEventOptions) -> dict[str, Any]:
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_period_rows AS
        SELECT {_cik('fp.cik')} AS owner_cik, fp.security_id AS period_security_id, fp.period_end,
               fp.accession_number, upper(trim(fp.form)) AS form, coalesce(fp.fdate, fp.as_of_date) AS filing_date,
               fp.available_at, fp.rdq, fp.statement_point_count
        FROM fundamental_periods fp
        WHERE fp.normalized_period_type IN ({_sql_list(DURATION_PERIOD_TYPES)})
          AND upper(trim(coalesce(fp.form, ''))) IN ({_sql_list(PERIODIC_FORMS)})
          AND fp.period_end IS NOT NULL AND fp.accession_number IS NOT NULL
          AND {_cik('fp.cik')} IN (SELECT owner_cik FROM _ev_owners)
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_originals AS
        WITH per_filing AS (
            SELECT owner_cik, period_end, accession_number, min(period_security_id) AS period_security_id,
                   min(form) AS form, min(filing_date) AS filing_date, min(available_at) AS available_at,
                   min(rdq) AS period_rdq, sum(statement_point_count) AS points
            FROM _ev_period_rows GROUP BY owner_cik, period_end, accession_number
        )
        SELECT *, CASE WHEN filing_date IS NULL THEN 'missing_filing_date'
                       WHEN filing_date < period_end OR date_diff('day', period_end, filing_date) > ?
                           THEN 'comparative_or_late_period' END AS exclusion
        FROM per_filing
        QUALIFY row_number() OVER (PARTITION BY owner_cik, period_end
                                   ORDER BY filing_date NULLS LAST, available_at NULLS LAST, accession_number) = 1
    """, [options.max_filing_lag_days])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_base AS
        SELECT * EXCLUDE (exclusion) FROM _ev_originals WHERE exclusion IS NULL
        QUALIFY row_number() OVER (PARTITION BY owner_cik, accession_number ORDER BY points DESC, period_end DESC) = 1
    """)
    rows, originals, kept, events = con.execute("""
        SELECT (SELECT count(*) FROM _ev_period_rows), (SELECT count(*) FROM _ev_originals),
               (SELECT count(*) FROM _ev_originals WHERE exclusion IS NULL), (SELECT count(*) FROM _ev_base)
    """).fetchone()
    excluded = {str(reason): int(n) for reason, n in con.execute(
        "SELECT exclusion, count(*) FROM _ev_originals WHERE exclusion IS NOT NULL GROUP BY 1 ORDER BY 1").fetchall()}
    excluded["duplicate_period_end_same_filing"] = int(kept) - int(events)
    return {"period_rows": int(rows), "owner_periods": int(originals), "events": int(events), "excluded": excluded}


def _stage_announcements(con: Any, options: EarningsEventOptions, raw_acceptance: bool) -> None:
    raw = "s.acceptance_datetime_raw" if raw_acceptance else "CAST(NULL AS VARCHAR)"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_8k AS
        SELECT * FROM (
            SELECT {_cik('s.cik')} AS owner_cik, s.accession_number, s.filing_date, s.report_date,
                   s.acceptance_datetime, {raw} AS acceptance_raw,
                   coalesce(s.report_date, s.filing_date, CAST(s.acceptance_datetime AS DATE)) AS rdq,
                   row_number() OVER (
                       PARTITION BY {_cik('s.cik')}, s.accession_number
                       ORDER BY s.acceptance_datetime DESC NULLS LAST, s.filing_date DESC NULLS LAST,
                                s.source_loaded_at DESC NULLS LAST, s.source_url) AS pick
            FROM sec_submissions s
            WHERE upper(trim(coalesce(s.form, '')))='8-K'
              AND regexp_matches(coalesce(s.items, ''), '(^|[^0-9])2\\.02([^0-9]|$)')
              AND {_cik('s.cik')} IN (SELECT owner_cik FROM _ev_owners)
        ) WHERE pick=1 AND rdq IS NOT NULL
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_periodic_acc AS
        SELECT b.owner_cik, b.accession_number, s.acceptance_datetime, {raw} AS acceptance_raw
        FROM sec_submissions s JOIN _ev_base b
          ON s.accession_number=b.accession_number AND {_cik('s.cik')}=b.owner_cik
        QUALIFY row_number() OVER (PARTITION BY b.owner_cik, b.accession_number
                                   ORDER BY s.acceptance_datetime DESC NULLS LAST,
                                            s.source_loaded_at DESC NULLS LAST, s.source_url) = 1
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_match AS
        SELECT b.owner_cik, b.period_end, k.accession_number AS k_accession, k.rdq AS k_rdq,
               k.acceptance_datetime AS k_acc, k.acceptance_raw AS k_raw, k.filing_date AS k_filed
        FROM _ev_base b JOIN _ev_8k k
          ON k.owner_cik=b.owner_cik AND k.rdq >= b.period_end AND k.rdq <= b.period_end + CAST(? AS INTEGER)
        QUALIFY row_number() OVER (PARTITION BY b.owner_cik, b.period_end
                                   ORDER BY k.rdq, k.acceptance_datetime NULLS LAST, k.accession_number) = 1
    """, [options.max_filing_lag_days])
    timed = _sql_list(TIMED_CLOCKS)
    same = _sql_list(SAME_SESSION_TIMINGS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_events AS
        WITH src AS (
            SELECT b.*, m.k_accession, m.k_rdq, m.k_acc, m.k_raw, m.k_filed,
                   p.acceptance_datetime AS p_acc, p.acceptance_raw AS p_raw,
                   CASE WHEN m.k_accession IS NULL THEN '{REJECT_NO_8K}'
                        WHEN m.k_rdq > b.filing_date THEN '{REJECT_AFTER_FILING}' END AS rdq_rejection
            FROM _ev_base b
            LEFT JOIN _ev_match m ON m.owner_cik=b.owner_cik AND m.period_end=b.period_end
            LEFT JOIN _ev_periodic_acc p ON p.owner_cik=b.owner_cik AND p.accession_number=b.accession_number
        ), ann AS (
            SELECT *,
                   CASE WHEN rdq_rejection IS NULL THEN '{BASIS_8K}' ELSE '{BASIS_PERIODIC}' END AS announcement_basis,
                   CASE WHEN rdq_rejection IS NULL THEN k_accession ELSE accession_number END
                       AS announcement_accession,
                   CASE WHEN rdq_rejection IS NULL THEN k_rdq ELSE filing_date END AS claimed_date,
                   CASE WHEN rdq_rejection IS NULL THEN k_acc ELSE p_acc END AS acceptance_at,
                   CASE WHEN rdq_rejection IS NULL THEN k_raw ELSE p_raw END AS acceptance_raw,
                   CASE WHEN rdq_rejection IS NULL THEN coalesce(k_filed, k_rdq) ELSE filing_date END AS evidence_filed
            FROM src
        ), local AS (
            SELECT *, timezone('{EXCHANGE_TIMEZONE}', timezone('UTC', acceptance_at)) AS acceptance_local_et
            FROM ann
        ), clock AS (
            SELECT *,
                   CASE WHEN acceptance_at IS NULL THEN '{CLOCK_MISSING}'
                        WHEN acceptance_raw IS NOT NULL
                             AND NOT regexp_matches(acceptance_raw, '(?i)(z|[+-][0-9]{{2}}:?[0-9]{{2}})$')
                            THEN '{CLOCK_ZONE_UNKNOWN}'
                        WHEN CAST(acceptance_local_et AS TIME) = TIME '00:00:00' THEN '{CLOCK_DATE_ONLY}'
                        WHEN acceptance_raw IS NULL THEN '{CLOCK_NORMALIZED}'
                        ELSE '{CLOCK_EXACT}' END AS acceptance_clock_status
            FROM local
        ), timing AS (
            SELECT *,
                   CASE WHEN acceptance_clock_status NOT IN ({timed}) THEN 'no_exact_clock'
                        WHEN CAST(acceptance_local_et AS DATE) > claimed_date THEN 'accepted_after_report_date'
                        ELSE 'acceptance' END AS timing_source
            FROM clock
        ), dated AS (
            SELECT *, CASE WHEN timing_source='acceptance' THEN CAST(acceptance_local_et AS DATE)
                           ELSE claimed_date END AS announcement_date
            FROM timing
        ), attributed AS (
            SELECT d.*,
                   CASE WHEN d.timing_source<>'acceptance' THEN '{TIMING_UNKNOWN}'
                        WHEN s.trade_date IS NULL THEN '{TIMING_NON_SESSION}'
                        WHEN CAST(d.acceptance_local_et AS TIME) < TIME '{SESSION_OPEN_ET}' THEN '{TIMING_PRE_OPEN}'
                        WHEN CAST(d.acceptance_local_et AS TIME) < TIME '{SESSION_CLOSE_ET}' THEN '{TIMING_INTRADAY}'
                        ELSE '{TIMING_AFTER_CLOSE}' END AS session_timing,
                   greatest(CASE WHEN d.acceptance_clock_status IN ({timed}) THEN d.acceptance_at END,
                            CAST(d.evidence_filed AS TIMESTAMP) + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR)
                       AS evidence_available_at,
                   (SELECT min(n.trade_date) FROM _ev_cal n WHERE n.trade_date > d.announcement_date) AS next_session
            FROM dated d LEFT JOIN _ev_cal s ON s.trade_date=d.announcement_date
        ), sessioned AS (
            SELECT a.*, CASE WHEN a.session_timing IN ({same}) THEN a.announcement_date ELSE a.next_session END
                            AS event_session
            FROM attributed a
        )
        SELECT x.*, s.session_number AS e_num, n.trade_date AS e_next,
               CASE WHEN x.evidence_available_at IS NULL THEN '{REASON_NO_CLOCK}'
                    WHEN n.trade_date IS NULL THEN '{REASON_NOT_MATURED}'
                    ELSE '{VALID}' END AS event_reason,
               CASE WHEN x.evidence_available_at IS NOT NULL AND n.trade_date IS NOT NULL
                    THEN greatest(CAST(n.trade_date AS TIMESTAMP) + INTERVAL {BAR_CLOCK_HOURS} HOUR,
                                  x.evidence_available_at) END AS event_available_at,
               lf.formation_date AS line_formation_date, c.security_id
        FROM sessioned x
        LEFT JOIN _ev_cal s ON s.trade_date=x.event_session
        LEFT JOIN _ev_cal n ON n.session_number=s.session_number+1
        LEFT JOIN (SELECT x2.owner_cik, x2.period_end, max(f.formation_date) AS formation_date
                   FROM sessioned x2 JOIN _ev_forms f ON f.formation_date < x2.event_session
                   GROUP BY x2.owner_cik, x2.period_end) lf
          ON lf.owner_cik=x.owner_cik AND lf.period_end=x.period_end
        LEFT JOIN _ev_cohort c
          ON c.formation_date=lf.formation_date AND c.owner_cik=x.owner_cik AND c.valid_primary
    """)


def _announcement_diagnostic(con: Any) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key, column in (("basis", "announcement_basis"), ("rdq_rejection", "coalesce(rdq_rejection, 'none')"),
                        ("acceptance_clock_status", "acceptance_clock_status"), ("timing_source", "timing_source"),
                        ("session_timing", "session_timing"), ("event_reason", "event_reason")):
        out[key] = {str(k): int(n) for k, n in con.execute(
            f"SELECT {column}, count(*) FROM _ev_events GROUP BY 1 ORDER BY 1").fetchall()}
    agree = con.execute(f"""
        SELECT count(*) FILTER (WHERE period_rdq = k_rdq), count(*) FILTER (WHERE period_rdq <> k_rdq),
               count(*) FILTER (WHERE period_rdq IS NULL)
        FROM _ev_events WHERE announcement_basis='{BASIS_8K}'
    """).fetchone()
    out["period_rdq_vs_8k"] = {"equal": int(agree[0]), "differs": int(agree[1]), "period_rdq_null": int(agree[2])}
    shared = con.execute(f"""
        SELECT count(*) FROM (SELECT announcement_accession FROM _ev_events WHERE announcement_basis='{BASIS_8K}'
                              GROUP BY 1 HAVING count(*) > 1)
    """).fetchone()
    out["shared_8k_accessions"] = int(shared[0])
    return out


# ---------------------------------------------------------------------------
# Returns
# ---------------------------------------------------------------------------

def _stage_returns(con: Any, lines_table: str, first_day: dt.date, last_day: dt.date) -> None:
    """``_ev_ret``: one-session ``adjusted_close`` returns of ``lines_table`` for sessions in the range."""
    row = con.execute("SELECT max(trade_date) FROM _ev_cal WHERE trade_date < ?", [first_day]).fetchone()
    start = row[0] if row and row[0] is not None else first_day
    bars = _publication.selected_bars_sql(PRICE_BASIS, security_filter=(
        f"trade_date BETWEEN DATE '{start.isoformat()}' AND DATE '{last_day.isoformat()}' "
        f"AND security_id IN (SELECT security_id FROM {lines_table})"))
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ev_bars AS {bars}", [None, None])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_ret AS
        SELECT b.security_id, b.trade_date, b.price / p.price - 1 AS r,
               greatest(b.price_available_at, p.price_available_at) AS clock
        FROM _ev_bars b
        JOIN _ev_cal s ON s.trade_date=b.trade_date
        JOIN _ev_cal ps ON ps.session_number=s.session_number-1
        JOIN _ev_bars p ON p.security_id=b.security_id AND p.trade_date=ps.trade_date
        WHERE b.trade_date BETWEEN ? AND ?
    """, [first_day, last_day])


def _year_ranges(first: dt.date, last: dt.date) -> list[tuple[dt.date, dt.date]]:
    return [(max(first, dt.date(year, 1, 1)), min(last, dt.date(year, 12, 31)))
            for year in range(first.year, last.year + 1)]


def _build_market(store: ResearchStore, version: str) -> dict[str, Any]:
    """Equal-weighted daily market over the eligible lines of the latest formation before each day."""
    con = store.con
    bounds = con.execute("SELECT min(trade_date), max(trade_date) FROM _ev_day_formation").fetchone()
    if bounds is None or bounds[0] is None:
        return {"days": 0}
    for first, last in _year_ranges(bounds[0], bounds[1]):
        _stage_returns(con, "_ev_universe_lines", first, last)
        with store.transaction():
            con.execute("""
                INSERT INTO research_event_market
                SELECT ?, r.trade_date, d.formation_date, avg(r.r), count(*),
                       count(*) FILTER (WHERE abs(r.r) > 1.0), max(r.clock)
                FROM _ev_ret r
                JOIN _ev_day_formation d ON d.trade_date=r.trade_date
                JOIN _ev_universe u ON u.formation_date=d.formation_date AND u.security_id=r.security_id
                GROUP BY r.trade_date, d.formation_date
            """, [version])
    days, low, mean, extreme = con.execute("""
        SELECT count(*), min(names), avg(names), count(*) FILTER (WHERE extreme_returns > 0)
        FROM research_event_market WHERE event_version=?
    """, [version]).fetchone()
    return {"days": int(days), "min_names": None if low is None else int(low),
            "mean_names": None if mean is None else round(float(mean), 3), "days_with_abs_return_over_100pct":
            int(extreme)}


def _build_windows(store: ResearchStore, version: str) -> None:
    """``_ev_windows``: EAR and run-up sums, counts and clocks per event with a price line."""
    con = store.con
    lo_e, hi_e = EAR_WINDOW
    lo_r, hi_r = RUNUP_WINDOW
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_windows (
            owner_cik VARCHAR, period_end DATE, ear_line_n BIGINT, ear_n BIGINT, ear_sum DOUBLE,
            ear_clock TIMESTAMP, runup_line_n BIGINT, runup_n BIGINT, runup_sum DOUBLE, runup_clock TIMESTAMP)
    """)
    bounds = con.execute(f"""
        SELECT min(event_session), max(event_session) FROM _ev_events
        WHERE event_reason='{VALID}' AND security_id IS NOT NULL
    """).fetchone()
    if bounds is None or bounds[0] is None:
        return
    for first, last in _year_ranges(bounds[0], bounds[1]):
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _ev_chunk AS
            SELECT owner_cik, period_end, security_id, e_num FROM _ev_events
            WHERE event_reason='{VALID}' AND security_id IS NOT NULL AND event_session BETWEEN ? AND ?
        """, [first, last])
        con.execute("CREATE OR REPLACE TEMP TABLE _ev_chunk_lines AS SELECT DISTINCT security_id FROM _ev_chunk")
        span = con.execute(f"""
            SELECT (SELECT min(trade_date) FROM _ev_cal WHERE session_number >= k.lo),
                   (SELECT max(trade_date) FROM _ev_cal WHERE session_number <= k.hi)
            FROM (SELECT min(e_num) + {lo_r} AS lo, max(e_num) + {hi_e} AS hi FROM _ev_chunk) k
        """).fetchone()
        if span is None or span[0] is None:
            continue
        _stage_returns(con, "_ev_chunk_lines", span[0], span[1])
        con.execute(f"""
            INSERT INTO _ev_windows
            WITH days AS (
                SELECT e.owner_cik, e.period_end, e.security_id, s.trade_date, s.session_number - e.e_num AS rel
                FROM _ev_chunk e JOIN _ev_cal s ON s.session_number BETWEEN e.e_num + {lo_r} AND e.e_num + {hi_e}
            ), joined AS (
                SELECT d.owner_cik, d.period_end, d.rel, r.r, m.market_return AS m,
                       greatest(r.clock, m.available_at) AS clock
                FROM days d
                LEFT JOIN _ev_ret r ON r.security_id=d.security_id AND r.trade_date=d.trade_date
                LEFT JOIN research_event_market m ON m.event_version=? AND m.trade_date=d.trade_date
            )
            SELECT owner_cik, period_end,
                   count(r) FILTER (WHERE rel BETWEEN {lo_e} AND {hi_e}),
                   count(r - m) FILTER (WHERE rel BETWEEN {lo_e} AND {hi_e}),
                   sum(r - m) FILTER (WHERE rel BETWEEN {lo_e} AND {hi_e}),
                   max(clock) FILTER (WHERE rel BETWEEN {lo_e} AND {hi_e}),
                   count(r) FILTER (WHERE rel BETWEEN {lo_r} AND {hi_r}),
                   count(r - m) FILTER (WHERE rel BETWEEN {lo_r} AND {hi_r}),
                   sum(r - m) FILTER (WHERE rel BETWEEN {lo_r} AND {hi_r}),
                   max(clock) FILTER (WHERE rel BETWEEN {lo_r} AND {hi_r})
            FROM joined GROUP BY owner_cik, period_end
        """, [version])


def _stage_sue(con: Any, options: EarningsEventOptions) -> None:
    """``_ev_sue_rev``: every ``sue_ni`` revision of each event's fiscal period (nearest period end)."""
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_sue_rev AS
        WITH ids AS (SELECT DISTINCT period_security_id AS security_id, owner_cik FROM _ev_period_rows),
        rev AS (
            SELECT i.owner_cik, d.period_end, d.value, d.available_at, d.derived_value_id
            FROM derived_metric_values d JOIN ids i ON i.security_id=d.security_id
            WHERE d.source=? AND d.metric_code=? AND d.metric_window=? AND d.value IS NOT NULL
              AND isfinite(d.value) AND d.available_at IS NOT NULL
        ), matched AS (
            SELECT e.owner_cik, e.period_end AS fiscal_period_end, r.period_end AS sue_period_end, r.value,
                   r.available_at, r.derived_value_id,
                   dense_rank() OVER (PARTITION BY e.owner_cik, e.period_end
                                      ORDER BY abs(date_diff('day', r.period_end, e.period_end)), r.period_end)
                       AS closeness
            FROM _ev_events e JOIN rev r ON r.owner_cik=e.owner_cik
             AND r.period_end BETWEEN e.period_end - CAST(? AS INTEGER) AND e.period_end + CAST(? AS INTEGER)
        )
        SELECT * EXCLUDE (closeness) FROM matched WHERE closeness=1
    """, [DERIVED_SOURCE_NAME, SUE_METRIC, SUE_WINDOW, options.sue_period_tolerance_days,
          options.sue_period_tolerance_days])


def _insert_events(store: ResearchStore, version: str) -> None:
    lo_r, hi_r = RUNUP_WINDOW
    ear_days = EAR_WINDOW[1] - EAR_WINDOW[0] + 1
    runup_days = hi_r - lo_r + 1

    def reason(n_line: str, n_both: str, days: int) -> str:
        return f"""CASE WHEN e.event_reason<>'{VALID}' THEN e.event_reason
                        WHEN e.line_formation_date IS NULL THEN '{REASON_NO_FORMATION}'
                        WHEN e.security_id IS NULL THEN '{REASON_NO_LINE}'
                        WHEN coalesce({n_line}, 0) < {days} THEN '{REASON_INCOMPLETE}'
                        WHEN coalesce({n_both}, 0) < {days} THEN '{REASON_NO_MARKET}'
                        ELSE '{VALID}' END"""

    with store.transaction():
        store.con.execute(f"""
            INSERT INTO research_earnings_events
            WITH first_sue AS (
                SELECT * FROM _ev_sue_rev
                QUALIFY row_number() OVER (PARTITION BY owner_cik, fiscal_period_end
                                           ORDER BY available_at, derived_value_id) = 1
            ), scored AS (
                SELECT e.*, w.ear_sum, w.ear_clock, w.runup_sum, w.runup_clock,
                       {reason('w.ear_line_n', 'w.ear_n', ear_days)} AS ear_reason,
                       {reason('w.runup_line_n', 'w.runup_n', runup_days)} AS runup_reason,
                       s.value AS sue_value, s.sue_period_end, s.derived_value_id AS sue_id,
                       s.available_at AS sue_first
                FROM _ev_events e
                LEFT JOIN _ev_windows w ON w.owner_cik=e.owner_cik AND w.period_end=e.period_end
                LEFT JOIN first_sue s ON s.owner_cik=e.owner_cik AND s.fiscal_period_end=e.period_end
            )
            SELECT ?, owner_cik, period_end, period_security_id, accession_number, form, filing_date, period_rdq,
                   announcement_basis, rdq_rejection, announcement_accession, claimed_date, acceptance_at,
                   acceptance_local_et, acceptance_clock_status, timing_source, session_timing, announcement_date,
                   event_session, evidence_available_at, event_available_at, event_reason, security_id,
                   line_formation_date,
                   CASE WHEN ear_reason='{VALID}' THEN ear_sum END,
                   ear_reason,
                   CASE WHEN ear_reason='{VALID}' THEN greatest(event_available_at, ear_clock)
                        ELSE event_available_at END,
                   CASE WHEN runup_reason='{VALID}' THEN runup_sum END,
                   runup_reason,
                   CASE WHEN runup_reason='{VALID}' THEN greatest(event_available_at, runup_clock)
                        ELSE event_available_at END,
                   CASE WHEN event_reason='{VALID}' THEN sue_value END,
                   CASE WHEN event_reason<>'{VALID}' THEN event_reason
                        WHEN sue_value IS NULL THEN '{REASON_NO_SUE}' ELSE '{VALID}' END,
                   sue_period_end, sue_id, sue_first,
                   CASE WHEN event_reason='{VALID}' AND sue_value IS NOT NULL
                        THEN greatest(sue_first, event_available_at) END,
                   ?
            FROM scored
        """, [version, MARKET_BASIS])


def _insert_features(store: ResearchStore, version: str, options: EarningsEventOptions) -> tuple[int, str]:
    """Dense formation view; returns (rows, digest over per-formation digests)."""
    con = store.con
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_candidates AS
        WITH ev AS (SELECT * FROM research_earnings_events WHERE event_version=? AND event_reason='{VALID}')
        SELECT owner_cik, fiscal_period_end, event_session, announcement_basis, '{FEATURE_EAR}' AS feature_id,
               ear_m1p1 AS value, ear_reason AS reason, ear_available_at AS clock FROM ev
        UNION ALL
        SELECT owner_cik, fiscal_period_end, event_session, announcement_basis, '{FEATURE_RUNUP}',
               runup_m21_m2, runup_reason, runup_available_at FROM ev
        UNION ALL
        SELECT owner_cik, fiscal_period_end, event_session, announcement_basis, '{FEATURE_DAYS_SINCE}',
               NULL, '{VALID}', event_available_at FROM ev
        UNION ALL
        SELECT e.owner_cik, e.fiscal_period_end, e.event_session, e.announcement_basis, '{FEATURE_SUE}',
               r.value, '{VALID}', greatest(r.available_at, e.event_available_at)
        FROM ev e JOIN _ev_sue_rev r ON r.owner_cik=e.owner_cik AND r.fiscal_period_end=e.fiscal_period_end
    """, [version])
    formations = [row[0] for row in con.execute("SELECT formation_date FROM _ev_forms ORDER BY 1").fetchall()]
    digests: list[str] = []
    for start in range(0, len(formations), options.formation_chunk):
        batch = formations[start:start + options.formation_chunk]
        with store.transaction():
            con.execute(f"""
                INSERT INTO research_event_features
                WITH lines AS (
                    SELECT c.formation_date, f.cutoff, c.security_id, c.owner_cik
                    FROM _ev_cohort c JOIN _ev_forms f ON f.formation_date=c.formation_date
                    WHERE c.valid_primary AND c.formation_date BETWEEN ? AND ?
                ), picked AS (
                    SELECT l.formation_date, l.security_id, x.feature_id, x.value, x.reason, x.clock,
                           x.fiscal_period_end, x.event_session, x.announcement_basis
                    FROM lines l JOIN _ev_candidates x
                      ON x.owner_cik=l.owner_cik AND x.clock <= l.cutoff
                     AND x.event_session >= l.formation_date - CAST(? AS INTEGER)
                    QUALIFY row_number() OVER (PARTITION BY l.formation_date, l.security_id, x.feature_id
                                               ORDER BY x.fiscal_period_end DESC, x.clock DESC) = 1
                ), grid AS (
                    SELECT l.formation_date, l.security_id, l.owner_cik, f.feature_id
                    FROM lines l CROSS JOIN (SELECT unnest(?::VARCHAR[]) AS feature_id) f
                )
                SELECT ?, g.formation_date, g.security_id, g.owner_cik, g.feature_id,
                       CASE WHEN p.feature_id IS NULL THEN NULL
                            WHEN g.feature_id='{FEATURE_DAYS_SINCE}'
                                THEN CAST(date_diff('day', p.event_session, g.formation_date) AS DOUBLE)
                            ELSE p.value END,
                       CASE WHEN p.feature_id IS NOT NULL THEN p.reason
                            WHEN g.feature_id='{FEATURE_SUE}' THEN '{REASON_NO_RECENT_SUE}'
                            ELSE '{REASON_NO_RECENT}' END,
                       p.clock, p.fiscal_period_end, p.event_session, p.announcement_basis
                FROM grid g LEFT JOIN picked p
                  ON p.formation_date=g.formation_date AND p.security_id=g.security_id AND p.feature_id=g.feature_id
            """, [batch[0], batch[-1], options.max_age_days, list(EVENT_FEATURES), version])
        digests.extend(str(row[1]) for row in con.execute("""
            SELECT formation_date, sha256(string_agg(md5(concat_ws('|', security_id, owner_cik, feature_id,
                       coalesce(printf('%.12e', raw_value), ''), reason, coalesce(CAST(available_at AS VARCHAR), ''),
                       coalesce(CAST(fiscal_period_end AS VARCHAR), ''), coalesce(CAST(event_session AS VARCHAR), ''),
                       coalesce(announcement_basis, ''))), '' ORDER BY security_id, feature_id))
            FROM research_event_features WHERE event_version=? AND formation_date BETWEEN ? AND ?
            GROUP BY formation_date ORDER BY formation_date
        """, [version, batch[0], batch[-1]]).fetchall())
    rows = con.execute("SELECT count(*) FROM research_event_features WHERE event_version=?", [version]).fetchone()
    return int(rows[0]), _sha("\n".join(digests))


def _events_digest(con: Any, version: str) -> str:
    row = con.execute("""
        SELECT sha256(coalesce(string_agg(md5(concat_ws('|', owner_cik, fiscal_period_end, periodic_accession,
                   announcement_basis, coalesce(rdq_rejection, ''), coalesce(announcement_accession, ''),
                   coalesce(CAST(acceptance_at AS VARCHAR), ''), session_timing,
                   coalesce(CAST(event_session AS VARCHAR), ''), coalesce(CAST(event_available_at AS VARCHAR), ''),
                   event_reason, coalesce(security_id, ''), coalesce(printf('%.12e', ear_m1p1), ''), ear_reason,
                   coalesce(printf('%.12e', runup_m21_m2), ''), runup_reason, coalesce(printf('%.12e', sue), ''),
                   sue_reason, coalesce(CAST(sue_available_at AS VARCHAR), ''))), ''
                   ORDER BY owner_cik, fiscal_period_end), ''))
        FROM research_earnings_events WHERE event_version=?
    """, [version]).fetchone()
    return str(row[0])


def _reason_counts(con: Any, version: str) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for column in ("ear_reason", "runup_reason", "sue_reason"):
        out[column] = {str(k): int(n) for k, n in con.execute(
            f"SELECT {column}, count(*) FROM research_earnings_events WHERE event_version=? GROUP BY 1 ORDER BY 1",
            [version]).fetchall()}
    features: dict[str, dict[str, int]] = {}
    for feature, reason, n in con.execute("""
        SELECT feature_id, reason, count(*) FROM research_event_features WHERE event_version=?
        GROUP BY 1, 2 ORDER BY 1, 2
    """, [version]).fetchall():
        features.setdefault(str(feature), {})[str(reason)] = int(n)
    out["features"] = features
    return out


# ---------------------------------------------------------------------------
# Validation (run at seal; also callable on a sealed version)
# ---------------------------------------------------------------------------

def _checks(con: Any, version: str, calendar: tuple[str, str] | None = None) -> dict[str, int]:
    """Point-in-time and grain invariants; every count must be zero."""
    cal = ("_ev_cal" if calendar is None else
           "(SELECT trade_date, CAST(row_number() OVER (ORDER BY trade_date) AS INTEGER) AS session_number "
           "FROM (SELECT DISTINCT trade_date FROM trading_calendar WHERE calendar_id=? AND source=? AND is_open))")
    cal_params = [] if calendar is None else list(calendar)
    same = _sql_list(SAME_SESSION_TIMINGS)
    checks = {
        "event_grain": ("""
            SELECT count(*) FROM (SELECT owner_cik, fiscal_period_end FROM research_earnings_events
                                  WHERE event_version=? GROUP BY 1, 2 HAVING count(*) > 1)""", [version]),
        "event_clock_before_session_after_close": (f"""
            SELECT count(*) FROM research_earnings_events e
            LEFT JOIN {cal} s ON s.trade_date=e.event_session
            LEFT JOIN {cal} n ON n.session_number=s.session_number+1
            WHERE e.event_version=? AND e.event_reason='{VALID}'
              AND (n.trade_date IS NULL OR e.event_available_at IS NULL
                   OR e.event_available_at < CAST(n.trade_date AS TIMESTAMP) + INTERVAL {BAR_CLOCK_HOURS} HOUR
                   OR e.event_available_at < e.evidence_available_at)""", [*cal_params, *cal_params, version]),
        "feature_clock_before_event_clock": (f"""
            SELECT count(*) FROM research_earnings_events WHERE event_version=? AND event_reason='{VALID}'
              AND (ear_available_at < event_available_at OR runup_available_at < event_available_at
                   OR (sue IS NOT NULL AND (sue_available_at IS NULL OR sue_available_at < event_available_at
                                            OR sue_available_at < sue_first_available_at)))""", [version]),
        "session_rule_violations": (f"""
            SELECT count(*) FROM research_earnings_events WHERE event_version=? AND event_session IS NOT NULL
              AND ((session_timing IN ({same}) AND event_session <> announcement_date)
                   OR (session_timing NOT IN ({same}) AND event_session <= announcement_date))""", [version]),
        "feature_visible_after_cutoff_or_same_session": ("""
            SELECT count(*) FROM research_event_features x
            JOIN research_panel_calendar c ON c.formation_date=x.formation_date
            JOIN research_event_versions v ON v.event_version=x.event_version AND c.run_id=v.panel_run_id
            WHERE x.event_version=? AND x.available_at IS NOT NULL
              AND (x.available_at > c.cutoff OR x.event_session >= x.formation_date)""", [version]),
        "feature_grain": ("""
            SELECT count(*) FROM (SELECT formation_date, security_id, feature_id FROM research_event_features
                                  WHERE event_version=? GROUP BY ALL HAVING count(*) > 1)""", [version]),
    }
    return {name: int(con.execute(sql, params).fetchone()[0]) for name, (sql, params) in checks.items()}


def validate_event_version(store: ResearchStore, event_version: str) -> dict[str, int]:
    """Re-run the seal invariants on a stored version; raise when any is violated."""
    con = store.con
    row = con.execute("SELECT status FROM research_event_versions WHERE event_version=?", [event_version]).fetchone()
    if row is None:
        raise EventStoreError(f"event version {event_version} is absent")
    checks = _checks(con, event_version, _calendar_keys())
    bad = {name: n for name, n in checks.items() if n}
    if bad:
        raise EventStoreError(f"event version {event_version} violates {bad}")
    return checks


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def _drop_temps(con: Any) -> None:
    for table in _TEMP_TABLES:
        with contextlib.suppress(duckdb.Error):
            con.execute(f"DROP TABLE IF EXISTS {table}")


def _stored(con: Any, version: str, reused: bool) -> EarningsEventResult:
    status, blockers, diagnostic = con.execute(
        "SELECT status, blockers_json, diagnostic_json FROM research_event_versions WHERE event_version=?",
        [version]).fetchone()
    events = con.execute("SELECT count(*) FROM research_earnings_events WHERE event_version=?", [version]).fetchone()
    rows = con.execute("SELECT count(*) FROM research_event_features WHERE event_version=?", [version]).fetchone()
    return EarningsEventResult(version, str(status), reused, int(events[0]), int(rows[0]),
                               tuple(json.loads(blockers)), json.loads(diagnostic) if diagnostic else {})


def build_earnings_events(store: ResearchStore, options: EarningsEventOptions) -> EarningsEventResult:
    """Build (or return the sealed) earnings-event version of one sealed R2a panel run."""
    options = _validate_options(options)
    missing = [table for table in REQUIRED_WAREHOUSE_TABLES if not store.warehouse_has(table)]
    if missing:
        raise EventStoreError(f"warehouse lacks {missing}")
    con = store.con
    ensure_event_schema(con)
    context = _panel_context(store, options)
    calendar = _calendar_keys()
    raw_acceptance = store.warehouse_has("sec_submissions", "acceptance_datetime_raw")
    inputs = _input_fingerprints(con, *calendar)
    spec = _spec(options, context, calendar, inputs, raw_acceptance)
    code_sha = _code_sha()
    spec_json = _canonical(spec)
    version = _sha(_canonical({"spec": spec, "code_sha256": code_sha}))
    existing = con.execute("SELECT status FROM research_event_versions WHERE event_version=?", [version]).fetchone()
    if existing is not None and existing[0] in SEALED_STATUSES:
        return _stored(con, version, reused=True)
    with store.transaction():
        for table in _VERSION_TABLES:
            con.execute(f"DELETE FROM {table} WHERE event_version=?", [version])
        con.execute("""
            INSERT INTO research_event_versions (event_version, status, basis, panel_run_id, panel_sha256,
                query_version, spec_json, spec_sha256, code_sha256, inputs_json, blockers_json, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '[]', ?)
        """, [version, STATUS_BUILDING, context["basis"], options.panel_run_id, context["panel_sha256"],
              QUERY_VERSION, spec_json, _sha(spec_json), code_sha, _canonical(inputs), _now()])
    try:
        diagnostic: dict[str, Any] = {"panel": _stage_panel(con, options.panel_run_id, calendar)}
        diagnostic["periods"] = _stage_periods(con, options)
        _stage_announcements(con, options, raw_acceptance)
        diagnostic["announcements"] = _announcement_diagnostic(con)
        diagnostic["market"] = _build_market(store, version)
        _build_windows(store, version)
        _stage_sue(con, options)
        _insert_events(store, version)
        feature_rows, features_sha = _insert_features(store, version, options)
        diagnostic["reasons"] = _reason_counts(con, version)
        checks = _checks(con, version)
        diagnostic["checks"] = checks
        bad = {name: n for name, n in checks.items() if n}
        if bad:
            raise EventStoreError(f"event version {version} violates {bad}")
        blockers = _blockers(context, diagnostic)
        events = diagnostic["periods"]["events"]
        status = STATUS_SEALED if events and feature_rows else STATUS_EMPTY
        with store.transaction():
            con.execute("""
                UPDATE research_event_versions SET status=?, blockers_json=?, diagnostic_json=?, events_sha256=?,
                       features_sha256=?, finished_at=? WHERE event_version=?
            """, [status, _canonical(list(blockers)), _canonical(diagnostic), _events_digest(con, version),
                  features_sha, _now(), version])
    except Exception as exc:
        with contextlib.suppress(duckdb.Error):
            con.execute("UPDATE research_event_versions SET status=?, diagnostic_json=?, finished_at=? "
                        "WHERE event_version=?", [STATUS_FAILED, _canonical({"error": repr(exc)[:2000]}), _now(),
                                                   version])
        raise
    finally:
        _drop_temps(con)
    return _stored(con, version, reused=False)


def _blockers(context: dict[str, Any], diagnostic: dict[str, Any]) -> tuple[str, ...]:
    """Labels a consumer must carry (the version still seals)."""
    blockers = [MARKET_BLOCKER, EARLY_CLOSE_BLOCKER, WIRE_BLOCKER]
    if context["basis"] == _panel.BASIS_RECONSTRUCTED:
        blockers.append(RECONSTRUCTED_BLOCKER)
    announcements = diagnostic["announcements"]
    events = diagnostic["periods"]["events"]
    periodic = announcements["basis"].get(BASIS_PERIODIC, 0)
    if periodic:
        blockers.append(f"periodic_filing_basis_late_proxy:{periodic}/{events}")
    normalized = announcements["acceptance_clock_status"].get(CLOCK_NORMALIZED, 0)
    if normalized:
        blockers.append(f"acceptance_zone_normalized_unqualified:{normalized}/{events}")
    if not diagnostic["panel"]["owners"]:
        blockers.append("no_linked_owners")
    return tuple(blockers)


__all__ = [
    "ANNOUNCEMENT_BASES",
    "BASIS_8K",
    "BASIS_PERIODIC",
    "EVENT_FEATURES",
    "EVENT_SCHEMA_VERSION",
    "MARKET_BASIS",
    "QUERY_VERSION",
    "REJECT_AFTER_FILING",
    "REJECT_NO_8K",
    "EarningsEventOptions",
    "EarningsEventResult",
    "EventStoreError",
    "build_earnings_events",
    "ensure_event_schema",
    "validate_event_version",
]
