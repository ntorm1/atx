"""Earnings-event dataset and event features (task P3; fix round 1).

For one sealed R2a monthly panel run (one basis) this module builds one immutable,
content-addressed *event version*: one row per (owner, fiscal period) in
``research_earnings_events`` with the announcement moment, its exchange session and the
event returns, plus the formation-aligned view ``research_event_features`` over the
panel's owner lines.

Events (one row per owner x fiscal period)
------------------------------------------
Owners are the CIKs of the panel's owner-linked primary lines (``cohort_reason='valid'``).
A fiscal period is a duration (quarter, year-to-date or annual) ending on
``fundamental_periods.period_end`` whose *original* filing is a 10-K, 10-Q, 10-KT or 10-QT
(amendments never define an event). The original is the earliest filing carrying the
period. A period whose original filing is not about it (it is not that filing's own
period: the latest period end carrying at least a quarter of the filing's points) is a
comparative (``comparative_only_period``, counted). A period filed more than
``max_filing_lag_days`` after its end is kept and flagged ``late_original_filing``.

Known limitation: the event set is keyed by the (later) periodic filing, so an 8-K
earnings release never followed by a 10-K/10-Q is absent. Announcement values are
point in time; the selection is conditioned on the later filing.

Announcement (``announcement_basis``)
-------------------------------------
``8k_202``
    The earliest 8-K with Item 2.02 of the owner's CIK whose ``rdq`` =
    ``coalesce(report_date, filing_date, acceptance date)`` (the ``fundamental_periods.rdq``
    rule) lies in ``[period_end, period_end + max_filing_lag_days]``. Its *effective* date
    is the report date, unless the report date is implausible (below). An 8-K whose
    effective date is after the periodic filing date is **rejected and counted**
    (``rdq_after_filing_date``).
``periodic_filing``
    Fallback (``no_8k_202_in_window`` or the rejection): the original periodic filing.
Implausible report date (Item 2.02 must be furnished within 4 business days): when the
acceptance date is more than 4 business days after ``report_date`` (without a usable
clock: filing date more than 5), the report date is not the release date (some filers
enter the fiscal period end). The announcement then comes from the acceptance
(``timing_source='report_date_implausible'``, counted, blocker). Business days are the
observed XNYS sessions inside the calendar and weekdays outside it.
Ambiguity is labeled in ``announcement_flags`` (and counted): ``multiple_8k_202_before_filing``
(two or more 2.02 8-Ks before the filing: the earliest may be preliminary),
``prior_period_candidate`` (the matched 8-K was also the candidate of an earlier period),
``late_original_filing``, ``report_date_implausible``. The stored
``fundamental_periods.rdq`` is kept as ``period_rdq``; agreement is counted.

Session attribution (observed XNYS calendar ``equity_daily_bars calendar``): the EDGAR
acceptance stamp (naive UTC; true UTC 2004-2025, verified by DST shifts on the retained
archive) is converted to America/New_York. Session day before 09:30 ET ->
``pre_open`` same session; 09:30-16:00 -> ``intraday`` same session; at or after 16:00 ->
``after_close`` next session; not a session day -> ``non_session_day`` next session; no
usable time (missing, zone-less or date-only stamp, or an 8-K accepted within the deadline
but on a later day than its report date) -> ``unknown_time``: next session after the
announcement date. An announcement before the calendar's first session is out of range:
no session, ``event_reason='before_session_calendar'`` (counted). Early closes are not
modeled (blocker).

Clocks (point in time)
----------------------
``evidence_available_at`` = the later of the acceptance and SEC filed date + 46 h (FC1
``sec_filed_date_plus_46h_v1``); ``event_available_at`` = the later of that and the 22:00
UTC close clock of the session after the event session: **no event feature is visible
before the close of event session + 1**. EAR and run-up are also no earlier than their
input bars, terminal returns and market days.

Returns (survivorship)
----------------------
One-session ``adjusted_close`` returns (the forward-label bar pick, VA1
``vendor_artifact_repaired``: vendor factor-decrease artifacts neutralized). The market is the
equal-weighted mean of **bar returns only** over the panel's eligible lines of the latest
formation before the day (linked, unlinked, later-delisting lines; a name leaves after its
last bar: CRSP EW ex-DLRET), counting a return only when both bars were known at the day's
22:00 UTC close clock, so every market row's clock is its session close (seal check) and no
other name's late terminal can delay an event. It is published only when at least
``market_min_names`` lines have a return (else ``missing_market_return``). In an event
line's *own* windows its selected delisting terminal (the forward-label terminal and
halt-gap dating) enters as the return of its delisting session (first session on or after
the effective delist date) when valid with a prior price; its later bars are dropped. With
``AR = r - m`` and ``AR = 0`` after a valid delisting inside the window (proceeds held in
the market): ``ear_m1p1`` = sum over E-1..E+1 (all 3 days), ``runup_m21_m2`` = sum over
E-21..E-2 (all 20); a window that starts after the delisting is ``delisted_before_window``.
The price line is the owner's primary line at the latest formation before E.

SUE (derived-state contract, migration 0315 / FQ1 gates)
--------------------------------------------------------
Every ``sue_ni`` state of the event's fiscal period is staged, NULL states included, with
a state reason: ``ambiguous_derived_owner``, ``uncertified_history`` (not
``event_reconstructed``), ``definition_hash_mismatch`` (seed hash), ``invalid_current_state``
(value_status not valid / NULL / non-finite), ``invalid_value_origin``, else ``valid``.
The event row's ``sue`` is the state current at ``sue_available_at`` = max(the period's
first state clock, event clock) (the newest state not after it; ``sue_first_available_at``
holds that state's own clock), valid over ``[sue_available_at, sue_valid_to)`` with
``sue_valid_to`` the next state's clock (seal check: later than ``sue_available_at``). The
formation feature ``sue_ni_event`` belongs to the latest visible event (below): its newest
state visible at the cutoff (ties: state clock, then ``derived_value_id``); the newest state
wins even when it is invalid (value NULL with its reason, no fallback to an older value or
period). Its clock is never earlier than the state's own clock or the event clock. It is the
event-clocked variant of the panel's ``sue_ni`` (same states): catalog at most one of the
two as a hypothesis.

Formation view
--------------
``research_event_features``: dense over the panel's valid primary lines x
:data:`EVENT_FEATURES` per formation. Per line the owner's latest *visible* event is chosen
first (event clock at or before the cutoff, event session at most ``max_age_days`` old);
each feature is that event's value when its own clock is at or before the cutoff, else
NULL with ``event_value_pending`` (never the prior period's value). Without a visible event:
``no_periodic_quarterly_filer`` (no periodic filing and no event visible yet: 20-F/40-F
filers) or ``no_recent_event``.
``days_since_announcement`` = formation date - event session (calendar days). The shape
(formation x security x feature: raw_value, reason, available_at, fiscal_period_end,
event_session, announcement_basis) is the contract for the planned feature-store adapter.

Versions: ``event_version`` = sha256 of the spec (panel seal, parameters, SUE definition
hash, aggregate fingerprints of every warehouse relation read) and the digest of this
module and every module whose rules shape the output. A sealed version is returned
unchanged; a ``building``/``failed`` one is rebuilt. :func:`validate_event_version`
re-runs the seal invariants and recomputes both content digests. Tables live in the
research store (RX6), schema owned here until registered as a store migration.
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
from .. import _vendor_artifact
from .._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from ..derived_registry import DERIVED_SOURCE_NAME
from . import panel as _panel
from .store import ResearchStore

# v3 (VA1): bar returns read the vendor_artifact_repaired adjusted close (selected_bars_sql).
QUERY_VERSION = "research-earnings-events-v3"
EVENT_SCHEMA_VERSION = 2
KNOWN_SCHEMA_VERSIONS = (1, 2)

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
SOURCE_ACCEPTANCE = "acceptance"
SOURCE_NO_CLOCK = "no_exact_clock"
SOURCE_ACCEPTED_LATER = "accepted_after_report_date"
SOURCE_IMPLAUSIBLE = "report_date_implausible"

CLOCK_EXACT = "exact_offset"
CLOCK_NORMALIZED = "normalized_utc_unqualified"
CLOCK_ZONE_UNKNOWN = "zone_unknown"
CLOCK_DATE_ONLY = "date_only"
CLOCK_MISSING = "missing"
TIMED_CLOCKS = (CLOCK_EXACT, CLOCK_NORMALIZED)

FLAG_MULTIPLE = "multiple_8k_202_before_filing"
FLAG_PRIOR = "prior_period_candidate"
FLAG_LATE = "late_original_filing"
FLAG_IMPLAUSIBLE = SOURCE_IMPLAUSIBLE
ANNOUNCEMENT_FLAGS = (FLAG_MULTIPLE, FLAG_PRIOR, FLAG_LATE, FLAG_IMPLAUSIBLE)

EXCHANGE_TIMEZONE = "America/New_York"
SESSION_OPEN_ET = "09:30:00"
SESSION_CLOSE_ET = "16:00:00"
ITEM_202_DEADLINE_BUSINESS_DAYS = 4
FILING_DATE_ROLLOVER_DAYS = 1
BAR_CLOCK_HOURS = 22
EVIDENCE_FLOOR_HOURS = 46
PRICE_BASIS = "adjusted_close"
MARKET_BASIS = "equal_weight_panel_eligible_lines_bar_returns_at_session_close"
EAR_WINDOW = (-1, 1)
RUNUP_WINDOW = (-21, -2)
TERMINAL_LOOKBACK_DAYS = 400
SUE_METRIC = "sue_ni"
SUE_WINDOW = "q"
SUE_HISTORY_STATUS = "event_reconstructed"
INVALID_VALUE_ORIGINS = ("legacy_unspecified", "unavailable", "incomparable")
PERIODIC_FORMS = ("10-K", "10-Q", "10-KT", "10-QT")
DURATION_PERIOD_TYPES = ("quarter", "semiannual_ytd", "multi_quarter_ytd", "annual")
OWN_PERIOD_MIN_POINT_SHARE = 0.25

FEATURE_EAR = "ear_m1p1"
FEATURE_RUNUP = "runup_m21_m2"
FEATURE_DAYS_SINCE = "days_since_announcement"
FEATURE_SUE = "sue_ni_event"
EVENT_FEATURES = (FEATURE_EAR, FEATURE_RUNUP, FEATURE_DAYS_SINCE, FEATURE_SUE)

VALID = "valid"
REASON_NO_CLOCK = "no_announcement_clock"
REASON_BEFORE_CALENDAR = "before_session_calendar"
REASON_NOT_MATURED = "event_window_not_matured"
REASON_NO_FORMATION = "no_prior_formation"
REASON_NO_LINE = "no_owner_price_line"
REASON_DELISTED = "delisted_before_window"
REASON_INCOMPLETE = "incomplete_price_window"
REASON_NO_MARKET = "missing_market_return"
REASON_NO_SUE = "missing_sue"
REASON_NO_RECENT = "no_recent_event"
REASON_PENDING = "event_value_pending"
REASON_NO_FILER = "no_periodic_quarterly_filer"

STATUS_BUILDING = "building"
STATUS_SEALED = "sealed"
STATUS_EMPTY = "sealed_empty"
STATUS_FAILED = "failed"
SEALED_STATUSES = (STATUS_SEALED, STATUS_EMPTY)

MARKET_BLOCKER = "market_return_equal_weight_only_value_weight_pending_p4"
EARLY_CLOSE_BLOCKER = "early_close_sessions_assume_1600_close"
WIRE_BLOCKER = "announcement_clock_is_edgar_acceptance_not_newswire_release"
SELECTION_BLOCKER = "event_set_conditioned_on_later_periodic_filing"
RECONSTRUCTED_BLOCKER = "reconstructed_identity_universe_and_availability_not_certifiable"

REQUIRED_WAREHOUSE_TABLES = ("fundamental_periods", "sec_submissions", "trading_calendar", "equity_daily_bars",
                             "derived_metric_values")
_VERSION_TABLES = ("research_event_features", "research_event_market", "research_earnings_events",
                   "research_event_versions")
#: Schema v1 -> v2 (fix round 1): columns added in place.
_V2_COLUMNS = (("announcement_flags", "VARCHAR"), ("announcement_candidates", "INTEGER"),
               ("report_lag_business_days", "INTEGER"), ("sue_valid_to", "TIMESTAMP"),
               ("delisting_session", "DATE"))
_EVENT_COLUMNS = (
    "event_version", "owner_cik", "fiscal_period_end", "period_security_id", "periodic_accession", "periodic_form",
    "periodic_filing_date", "period_rdq", "announcement_basis", "rdq_rejection", "announcement_accession", "rdq",
    "acceptance_at", "acceptance_local_et", "acceptance_clock_status", "timing_source", "session_timing",
    "announcement_date", "event_session", "evidence_available_at", "event_available_at", "event_reason",
    "security_id", "line_formation_date", "ear_m1p1", "ear_reason", "ear_available_at", "runup_m21_m2",
    "runup_reason", "runup_available_at", "sue", "sue_reason", "sue_period_end", "sue_derived_value_id",
    "sue_first_available_at", "sue_available_at", "market_basis", *(name for name, _ in _V2_COLUMNS))
_TEMP_TABLES = ("_ev_cal", "_ev_days", "_ev_forms", "_ev_cohort", "_ev_owners", "_ev_universe", "_ev_universe_lines",
                "_ev_day_formation", "_ev_per_filing", "_ev_owner_ids", "_ev_originals", "_ev_base", "_ev_8k",
                "_ev_periodic_acc", "_ev_match", "_ev_events", "_ev_term_sel", "_ev_term_bars", "_ev_terminals",
                "_ev_mkt", "_ev_windows", "_ev_chunk", "_ev_chunk_lines", "_ev_bars", "_ev_ret", "_ev_sue_rev",
                "_ev_vis", "_ev_filers")
_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_PKG = Path(__file__).resolve().parents[1]
#: Every module whose rules or constants shape the output (M3).
_CODE_FILES = (Path(__file__), _PKG / "_forward_return_publication.py", _PKG / "research" / "panel.py",
               _PKG / "_fundamental_clock.py", _PKG / "derived_registry.py", _PKG / "_derived_pit.py",
               _PKG / "_derived_annual.py", _PKG / "delisting.py")
_DIGEST_CHUNK = 12


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


def sue_definition_hash() -> str:
    """Seed definition hash of ``sue_ni`` (the derived publisher's hash, annual plan included)."""
    from .. import _derived_annual as annual
    from .. import _derived_pit as pit
    from ..derived_registry import default_derived_definitions

    by_code = {definition.metric_code: definition for definition in default_derived_definitions()}
    definition = by_code[SUE_METRIC]
    return pit.definition_hash(definition, annual.plan_for(definition, by_code))


def _clock_status_sql(at: str, raw: str, local: str) -> str:
    """Acceptance clock status; a date-only stamp is tested on the UTC instant and on raw length (M2)."""
    return f"""CASE WHEN {at} IS NULL THEN '{CLOCK_MISSING}'
                    WHEN {raw} IS NOT NULL AND length(trim({raw})) <= 10 THEN '{CLOCK_DATE_ONLY}'
                    WHEN {raw} IS NOT NULL
                         AND NOT regexp_matches(trim({raw}), '(?i)(z|[+-][0-9]{{2}}:?[0-9]{{2}})$')
                        THEN '{CLOCK_ZONE_UNKNOWN}'
                    WHEN CAST({at} AS TIME) = TIME '00:00:00' OR CAST({local} AS TIME) = TIME '00:00:00'
                        THEN '{CLOCK_DATE_ONLY}'
                    WHEN {raw} IS NULL THEN '{CLOCK_NORMALIZED}'
                    ELSE '{CLOCK_EXACT}' END"""


def _local_sql(at: str) -> str:
    return f"timezone('{EXCHANGE_TIMEZONE}', timezone('UTC', {at}))"


# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

def ensure_event_schema(con: Any) -> None:
    """Create or migrate the ``research_event*`` tables (schema v2); refuse a newer unknown schema."""
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
    for column, kind in _V2_COLUMNS:  # v1 -> v2 in place (a no-op on a fresh store)
        con.execute(f"ALTER TABLE research_earnings_events ADD COLUMN IF NOT EXISTS {column} {kind}")
    con.execute("INSERT INTO research_event_schema VALUES (?, ?, ?)",
                [EVENT_SCHEMA_VERSION, "earnings_events_v2_flags_delisting_sue_states", _now()])


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
    market_min_names: int = 100
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
                                    ("market_min_names", options.market_min_names, 1, 100_000),
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


def _column(store: ResearchStore, table: str, column: str, cast: str = "VARCHAR", alias: str = "") -> str:
    """The column (qualified by ``alias``) when the warehouse has it, else a typed NULL."""
    if store.warehouse_has(table, column):
        return f"{alias}.{column}" if alias else column
    return f"CAST(NULL AS {cast})"


def _input_fingerprints(store: ResearchStore, calendar: tuple[str, str]) -> dict[str, Any]:
    """Aggregate fingerprints of every warehouse relation read (part of the version identity)."""
    con = store.con
    updated = _column(store, "fundamental_periods", "updated_at", "TIMESTAMP")
    queries: dict[str, tuple[str, list[Any]]] = {
        "fundamental_periods": (f"""
            SELECT count(*), min(available_at), max(available_at), max(rdq), sum(statement_point_count),
                   max({updated}) FROM fundamental_periods""", []),
        "sec_submissions": ("SELECT count(*), max(source_loaded_at), max(acceptance_datetime) FROM sec_submissions",
                            []),
        "equity_daily_bars": ("SELECT count(*), max(source_loaded_at), max(trade_date) FROM equity_daily_bars", []),
        "derived_metric_values": ("""
            SELECT count(*), max(source_loaded_at), max(available_at) FROM derived_metric_values
            WHERE source=? AND metric_code=? AND metric_window=?""", [DERIVED_SOURCE_NAME, SUE_METRIC, SUE_WINDOW]),
        "trading_calendar": ("""
            SELECT count(*), min(trade_date), max(trade_date), sum(date_diff('day', DATE '1970-01-01', trade_date))
            FROM trading_calendar WHERE calendar_id=? AND source=? AND is_open""", list(calendar)),
    }
    if store.warehouse_has("delisting_terminal_returns"):
        queries["delisting_terminal_returns"] = (
            "SELECT count(*), max(available_at), max(source_loaded_at) FROM delisting_terminal_returns", [])
    out: dict[str, Any] = {}
    for table, (sql, params) in queries.items():
        values = con.execute(sql, params).fetchone()
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
        "own_period_min_point_share": OWN_PERIOD_MIN_POINT_SHARE,
        "max_filing_lag_days": options.max_filing_lag_days,
        "max_age_days": options.max_age_days,
        "item_202_deadline_business_days": ITEM_202_DEADLINE_BUSINESS_DAYS,
        "filing_date_rollover_days": FILING_DATE_ROLLOVER_DAYS,
        "session": {"timezone": EXCHANGE_TIMEZONE, "open": SESSION_OPEN_ET, "close": SESSION_CLOSE_ET,
                    "early_close": "not_modeled", "calendar_id": calendar[0], "calendar_source": calendar[1]},
        "clocks": {"evidence_policy": FUNDAMENTAL_CLOCK_POLICY, "evidence_floor_hours": EVIDENCE_FLOOR_HOURS,
                   "bar_clock_hours": BAR_CLOCK_HOURS, "raw_acceptance_column": raw_acceptance},
        "returns": {"price_basis": PRICE_BASIS, "adjustment_repair": _vendor_artifact.REPAIR_VERSION,
                    "market_basis": MARKET_BASIS, "ear_window": list(EAR_WINDOW),
                    "runup_window": list(RUNUP_WINDOW), "abnormal": "sum_of_daily_return_minus_market",
                    "market_min_names": options.market_min_names, "terminal_lookback_days": TERMINAL_LOOKBACK_DAYS,
                    "post_delisting_abnormal": 0.0, "terminal_rule": _publication.CALCULATION_VERSION},
        "sue": {"source": DERIVED_SOURCE_NAME, "metric_code": SUE_METRIC, "metric_window": SUE_WINDOW,
                "definition_hash": sue_definition_hash(), "history_status": SUE_HISTORY_STATUS,
                "invalid_value_origins": list(INVALID_VALUE_ORIGINS),
                "period_tolerance_days": options.sue_period_tolerance_days,
                "event_row": "state_current_at_visibility",
                "feature": "newest_visible_state_of_latest_visible_event"},
        "formation_pick": "latest_visible_event_then_value_or_event_value_pending",
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
    bounds = con.execute("SELECT min(trade_date), max(trade_date) FROM _ev_cal").fetchone()
    if bounds is None or bounds[0] is None:
        raise EventStoreError("the session calendar is empty")
    # Every calendar day: session flag, business-day running count (sessions inside the
    # observed calendar, weekdays outside it) and the next session.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_days AS
        WITH series AS (
            SELECT CAST(generate_series AS DATE) AS day
            FROM generate_series(DATE '1993-01-01', CAST(? AS DATE) + 400, INTERVAL 1 DAY)
        ), marked AS (
            SELECT s.day, c.trade_date IS NOT NULL AS is_session,
                   CASE WHEN s.day BETWEEN CAST(? AS DATE) AND CAST(? AS DATE) THEN c.trade_date IS NOT NULL
                        ELSE isodow(s.day) <= 5 END AS is_business
            FROM series s LEFT JOIN _ev_cal c ON c.trade_date=s.day
        )
        SELECT day, is_session,
               sum(CASE WHEN is_business THEN 1 ELSE 0 END) OVER (ORDER BY day) AS business_le,
               min(CASE WHEN is_session THEN day END) OVER (
                   ORDER BY day ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING) AS next_session
        FROM marked
    """, [bounds[1], bounds[0], bounds[1]])
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
        SELECT s.trade_date, f.formation_date FROM _ev_cal s ASOF JOIN _ev_forms f ON s.trade_date > f.formation_date
    """)
    sessions, formations, owners, lines = con.execute("""
        SELECT (SELECT count(*) FROM _ev_cal), (SELECT count(*) FROM _ev_forms), (SELECT count(*) FROM _ev_owners),
               (SELECT count(*) FROM _ev_cohort WHERE valid_primary)
    """).fetchone()
    return {"sessions": int(sessions), "formations": int(formations), "owners": int(owners),
            "owner_line_formations": int(lines)}


def _stage_periods(store: ResearchStore, options: EarningsEventOptions) -> dict[str, Any]:
    con = store.con
    where = f"""fp.normalized_period_type IN ({_sql_list(DURATION_PERIOD_TYPES)})
          AND upper(trim(coalesce(fp.form, ''))) IN ({_sql_list(PERIODIC_FORMS)})
          AND fp.period_end IS NOT NULL AND fp.accession_number IS NOT NULL
          AND {_cik('fp.cik')} IN (SELECT owner_cik FROM _ev_owners)"""
    # One row per (owner, period end, filing) straight from the warehouse (M8).
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_per_filing AS
        SELECT {_cik('fp.cik')} AS owner_cik, fp.period_end, fp.accession_number,
               min(fp.security_id) AS period_security_id, min(upper(trim(fp.form))) AS form,
               min(coalesce(fp.fdate, fp.as_of_date)) AS filing_date, min(fp.available_at) AS available_at,
               min(fp.rdq) AS period_rdq, sum(fp.statement_point_count) AS points
        FROM fundamental_periods fp WHERE {where}
        GROUP BY 1, 2, 3
    """)
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ev_owner_ids AS SELECT DISTINCT fp.security_id, "
                f"{_cik('fp.cik')} AS owner_cik FROM fundamental_periods fp WHERE {where}")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_originals AS
        WITH own AS (
            SELECT owner_cik, accession_number, max(period_end) FILTER (WHERE points >= ? * max_points) AS own_end
            FROM (SELECT *, max(points) OVER (PARTITION BY owner_cik, accession_number) AS max_points
                  FROM _ev_per_filing)
            GROUP BY owner_cik, accession_number
        )
        SELECT p.*, CASE WHEN p.filing_date IS NULL THEN 'missing_filing_date'
                         WHEN p.period_end IS DISTINCT FROM o.own_end THEN 'comparative_only_period'
                         WHEN p.filing_date < p.period_end THEN 'filing_before_period_end' END AS exclusion,
               coalesce(date_diff('day', p.period_end, p.filing_date) > ?, false) AS late_original
        FROM _ev_per_filing p JOIN own o ON o.owner_cik=p.owner_cik AND o.accession_number=p.accession_number
        QUALIFY row_number() OVER (PARTITION BY p.owner_cik, p.period_end
                                   ORDER BY p.filing_date NULLS LAST, p.available_at NULLS LAST,
                                            p.accession_number) = 1
    """, [OWN_PERIOD_MIN_POINT_SHARE, options.max_filing_lag_days])
    con.execute("CREATE OR REPLACE TEMP TABLE _ev_base AS SELECT * EXCLUDE (exclusion) FROM _ev_originals "
                "WHERE exclusion IS NULL")
    filings, originals, events, late = con.execute("""
        SELECT (SELECT count(*) FROM _ev_per_filing), (SELECT count(*) FROM _ev_originals),
               (SELECT count(*) FROM _ev_base), (SELECT count(*) FROM _ev_base WHERE late_original)
    """).fetchone()
    excluded = {str(reason): int(n) for reason, n in con.execute(
        "SELECT exclusion, count(*) FROM _ev_originals WHERE exclusion IS NOT NULL GROUP BY 1 ORDER BY 1").fetchall()}
    return {"owner_period_filings": int(filings), "owner_periods": int(originals), "events": int(events),
            "excluded": excluded, "late_original_filing": int(late)}


def _stage_announcements(store: ResearchStore, options: EarningsEventOptions) -> None:
    con = store.con
    raw = _column(store, "sec_submissions", "acceptance_datetime_raw", alias="s")
    timed = _sql_list(TIMED_CLOCKS)
    deadline, rollover = ITEM_202_DEADLINE_BUSINESS_DAYS, FILING_DATE_ROLLOVER_DAYS
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_8k AS
        WITH picked AS (
            SELECT * FROM (
                SELECT {_cik('s.cik')} AS owner_cik, s.accession_number, s.filing_date, s.report_date,
                       s.acceptance_datetime AS acceptance_at, {raw} AS acceptance_raw,
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
        ), clocked AS (
            SELECT p.* EXCLUDE (pick), {_local_sql('p.acceptance_at')} AS acceptance_local_et FROM picked p
        ), statused AS (
            SELECT c.*, {_clock_status_sql('c.acceptance_at', 'c.acceptance_raw', 'c.acceptance_local_et')}
                       AS clock_status
            FROM clocked c
        ), lagged AS (
            SELECT s.*,
                   CASE WHEN s.report_date IS NULL THEN NULL
                        WHEN s.clock_status IN ({timed}) THEN a.business_le - r.business_le
                        ELSE f.business_le - r.business_le END AS report_lag_business_days,
                   CASE WHEN s.clock_status IN ({timed}) THEN {deadline} ELSE {deadline + rollover} END AS lag_bound
            FROM statused s
            LEFT JOIN _ev_days r ON r.day=s.report_date
            LEFT JOIN _ev_days a ON a.day=CAST(s.acceptance_local_et AS DATE)
            LEFT JOIN _ev_days f ON f.day=s.filing_date
        )
        SELECT * EXCLUDE (lag_bound),
               coalesce(report_lag_business_days > lag_bound, false) AS report_date_implausible,
               CASE WHEN coalesce(report_lag_business_days > lag_bound, false)
                    THEN CASE WHEN clock_status IN ({timed}) THEN CAST(acceptance_local_et AS DATE)
                              ELSE coalesce(filing_date, rdq) END
                    ELSE rdq END AS effective_date
        FROM lagged
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_periodic_acc AS
        WITH picked AS (
            SELECT b.owner_cik, b.accession_number, s.acceptance_datetime AS acceptance_at, {raw} AS acceptance_raw
            FROM sec_submissions s JOIN _ev_base b
              ON s.accession_number=b.accession_number AND {_cik('s.cik')}=b.owner_cik
            QUALIFY row_number() OVER (PARTITION BY b.owner_cik, b.accession_number
                                       ORDER BY s.acceptance_datetime DESC NULLS LAST,
                                                s.source_loaded_at DESC NULLS LAST, s.source_url) = 1
        ), clocked AS (SELECT *, {_local_sql('acceptance_at')} AS acceptance_local_et FROM picked)
        SELECT *, {_clock_status_sql('acceptance_at', 'acceptance_raw', 'acceptance_local_et')} AS clock_status
        FROM clocked
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_match AS
        WITH cand AS (
            SELECT b.owner_cik, b.period_end, b.filing_date AS periodic_filed,
                   k.accession_number AS k_accession, k.rdq AS k_rdq, k.effective_date AS k_effective,
                   k.acceptance_at AS k_acc, k.acceptance_raw AS k_raw, k.acceptance_local_et AS k_local,
                   k.clock_status AS k_clock, k.filing_date AS k_filed, k.report_lag_business_days AS k_lag,
                   k.report_date_implausible AS k_implausible
            FROM _ev_base b JOIN _ev_8k k
              ON k.owner_cik=b.owner_cik AND k.rdq >= b.period_end AND k.rdq <= b.period_end + CAST(? AS INTEGER)
        ), counted AS (
            SELECT owner_cik, period_end, count(*) FILTER (WHERE k_effective <= periodic_filed) AS k_candidates
            FROM cand GROUP BY owner_cik, period_end
        )
        SELECT c.*, n.k_candidates
        FROM cand c JOIN counted n ON n.owner_cik=c.owner_cik AND n.period_end=c.period_end
        QUALIFY row_number() OVER (PARTITION BY c.owner_cik, c.period_end
                                   ORDER BY c.k_rdq, c.k_acc NULLS LAST, c.k_accession) = 1
    """, [options.max_filing_lag_days])
    same = _sql_list(SAME_SESSION_TIMINGS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_events AS
        WITH src AS (
            SELECT b.*, m.k_accession, m.k_rdq, m.k_effective, m.k_acc, m.k_raw, m.k_local, m.k_clock, m.k_filed,
                   m.k_lag, m.k_implausible, coalesce(m.k_candidates, 0) AS k_candidates,
                   p.acceptance_at AS p_acc, p.acceptance_raw AS p_raw, p.acceptance_local_et AS p_local,
                   coalesce(p.clock_status, '{CLOCK_MISSING}') AS p_clock,
                   CASE WHEN m.k_accession IS NULL THEN '{REJECT_NO_8K}'
                        WHEN m.k_effective > b.filing_date THEN '{REJECT_AFTER_FILING}' END AS rdq_rejection
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
                   CASE WHEN rdq_rejection IS NULL THEN k_local ELSE p_local END AS acceptance_local_et,
                   CASE WHEN rdq_rejection IS NULL THEN k_clock ELSE p_clock END AS acceptance_clock_status,
                   rdq_rejection IS NULL AND k_implausible AS implausible,
                   CASE WHEN rdq_rejection IS NULL THEN k_lag END AS report_lag_business_days,
                   CASE WHEN rdq_rejection IS NULL THEN coalesce(k_filed, k_rdq) ELSE filing_date END
                       AS evidence_filed
            FROM src
        ), timing AS (
            SELECT *,
                   CASE WHEN implausible THEN '{SOURCE_IMPLAUSIBLE}'
                        WHEN acceptance_clock_status NOT IN ({timed}) THEN '{SOURCE_NO_CLOCK}'
                        WHEN CAST(acceptance_local_et AS DATE) > claimed_date THEN '{SOURCE_ACCEPTED_LATER}'
                        ELSE '{SOURCE_ACCEPTANCE}' END AS timing_source,
                   acceptance_clock_status IN ({timed})
                       AND (implausible OR CAST(acceptance_local_et AS DATE) <= claimed_date) AS timed_announcement
            FROM ann
        ), dated AS (
            SELECT *,
                   CASE WHEN timed_announcement THEN CAST(acceptance_local_et AS DATE)
                        WHEN implausible THEN coalesce(evidence_filed, claimed_date)
                        ELSE claimed_date END AS announcement_date
            FROM timing
        ), attributed AS (
            SELECT d.*,
                   CASE WHEN NOT d.timed_announcement THEN '{TIMING_UNKNOWN}'
                        WHEN NOT coalesce(dd.is_session, false) THEN '{TIMING_NON_SESSION}'
                        WHEN CAST(d.acceptance_local_et AS TIME) < TIME '{SESSION_OPEN_ET}' THEN '{TIMING_PRE_OPEN}'
                        WHEN CAST(d.acceptance_local_et AS TIME) < TIME '{SESSION_CLOSE_ET}' THEN '{TIMING_INTRADAY}'
                        ELSE '{TIMING_AFTER_CLOSE}' END AS session_timing,
                   greatest(CASE WHEN d.acceptance_clock_status IN ({timed}) THEN d.acceptance_at END,
                            CAST(d.evidence_filed AS TIMESTAMP) + INTERVAL {EVIDENCE_FLOOR_HOURS} HOUR)
                       AS evidence_available_at,
                   dd.next_session
            FROM dated d LEFT JOIN _ev_days dd ON dd.day=d.announcement_date
        ), sessioned AS (
            -- An announcement before the calendar's first session is out of range (N3): no session.
            SELECT a.*, a.announcement_date < (SELECT min(trade_date) FROM _ev_cal) AS before_calendar,
                   CASE WHEN a.announcement_date < (SELECT min(trade_date) FROM _ev_cal) THEN NULL
                        WHEN a.session_timing IN ({same}) THEN a.announcement_date ELSE a.next_session END
                       AS event_session
            FROM attributed a
        )
        SELECT x.*, s.session_number AS e_num,
               CASE WHEN x.evidence_available_at IS NULL THEN '{REASON_NO_CLOCK}'
                    WHEN coalesce(x.before_calendar, false) THEN '{REASON_BEFORE_CALENDAR}'
                    WHEN n.trade_date IS NULL THEN '{REASON_NOT_MATURED}'
                    ELSE '{VALID}' END AS event_reason,
               CASE WHEN x.evidence_available_at IS NOT NULL AND n.trade_date IS NOT NULL
                    THEN greatest(CAST(n.trade_date AS TIMESTAMP) + INTERVAL {BAR_CLOCK_HOURS} HOUR,
                                  x.evidence_available_at) END AS event_available_at,
               f.formation_date AS line_formation_date, c.security_id,
               NULLIF(concat_ws(',',
                   CASE WHEN x.announcement_basis='{BASIS_8K}' AND x.k_candidates > 1 THEN '{FLAG_MULTIPLE}' END,
                   CASE WHEN x.announcement_basis='{BASIS_8K}' AND EXISTS (
                            SELECT 1 FROM _ev_match m2 WHERE m2.owner_cik=x.owner_cik
                              AND m2.period_end < x.period_end AND m2.k_accession=x.k_accession)
                        THEN '{FLAG_PRIOR}' END,
                   CASE WHEN x.late_original THEN '{FLAG_LATE}' END,
                   CASE WHEN x.implausible THEN '{FLAG_IMPLAUSIBLE}' END), '') AS announcement_flags
        FROM sessioned x
        LEFT JOIN _ev_cal s ON s.trade_date=x.event_session
        LEFT JOIN _ev_cal n ON n.session_number=s.session_number+1
        ASOF LEFT JOIN _ev_forms f ON x.event_session > f.formation_date
        LEFT JOIN _ev_cohort c ON c.formation_date=f.formation_date AND c.owner_cik=x.owner_cik AND c.valid_primary
    """)


def _announcement_diagnostic(con: Any) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key, column in (("basis", "announcement_basis"), ("rdq_rejection", "coalesce(rdq_rejection, 'none')"),
                        ("acceptance_clock_status", "acceptance_clock_status"), ("timing_source", "timing_source"),
                        ("session_timing", "session_timing"), ("event_reason", "event_reason")):
        out[key] = {str(k): int(n) for k, n in con.execute(
            f"SELECT {column}, count(*) FROM _ev_events GROUP BY 1 ORDER BY 1").fetchall()}
    out["flags"] = {flag: int(con.execute(
        "SELECT count(*) FROM _ev_events WHERE list_contains(string_split(coalesce(announcement_flags, ''), ','), ?)",
        [flag]).fetchone()[0]) for flag in ANNOUNCEMENT_FLAGS}
    agree = con.execute(f"""
        SELECT count(*) FILTER (WHERE period_rdq = k_rdq), count(*) FILTER (WHERE period_rdq <> k_rdq),
               count(*) FILTER (WHERE period_rdq IS NULL)
        FROM _ev_events WHERE announcement_basis='{BASIS_8K}'
    """).fetchone()
    out["period_rdq_vs_8k"] = {"equal": int(agree[0]), "differs": int(agree[1]), "period_rdq_null": int(agree[2])}
    return out


# ---------------------------------------------------------------------------
# Returns
# ---------------------------------------------------------------------------

def _stage_terminals(store: ResearchStore) -> dict[str, Any]:
    """``_ev_terminals``: the forward-label terminal of each universe line, dated to a session (M7)."""
    con = store.con
    if not store.warehouse_has("delisting_terminal_returns"):
        con.execute("CREATE OR REPLACE TEMP TABLE _ev_terminals (security_id VARCHAR, delist_session DATE, "
                    "terminal_return DOUBLE, clock TIMESTAMP, usable BOOLEAN)")
        return {"table": "absent"}
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_term_sel AS
        SELECT * FROM ({_publication.selected_terminals_sql()}) t
        WHERE t.security_id IN (SELECT security_id FROM _ev_universe_lines)
    """, [None, None])
    bars = _publication.selected_bars_sql(PRICE_BASIS, security_filter=(
        "EXISTS (SELECT 1 FROM _ev_term_sel t WHERE t.security_id=equity_daily_bars.security_id "
        "AND equity_daily_bars.trade_date < t.delist_date "
        f"AND equity_daily_bars.trade_date >= t.delist_date - {TERMINAL_LOOKBACK_DAYS})"))
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ev_term_bars AS {bars}", [None, None])
    effective = _publication.effective_terminals_sql(terminals="_ev_term_sel", bars="_ev_term_bars",
                                                     calendar="_ev_cal")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_terminals AS
        SELECT e.security_id, CASE WHEN d.is_session THEN d.day ELSE d.next_session END AS delist_session,
               e.terminal_return, greatest(e.terminal_available_at, e.last_price_available_at) AS clock,
               coalesce(e.terminal_valid AND e.last_price_date IS NOT NULL, false) AS usable
        FROM ({effective}) e LEFT JOIN _ev_days d ON d.day=e.effective_delist_date
    """)
    total, usable = con.execute("SELECT count(*), count(*) FILTER (WHERE usable) FROM _ev_terminals").fetchone()
    return {"terminals": int(total), "usable": int(usable)}


def _stage_returns(con: Any, lines_table: str, first_day: dt.date, last_day: dt.date, *, terminals: bool) -> None:
    """``_ev_ret``: one-session returns of ``lines_table`` in the range.

    With ``terminals`` (event lines' own windows only) the line's delisting terminal is spliced in
    on its delisting session and later bars are dropped; the market uses bar returns only (N1).
    """
    row = con.execute("SELECT max(trade_date) FROM _ev_cal WHERE trade_date < ?", [first_day]).fetchone()
    start = row[0] if row and row[0] is not None else first_day
    bars = _publication.selected_bars_sql(PRICE_BASIS, security_filter=(
        f"trade_date BETWEEN DATE '{start.isoformat()}' AND DATE '{last_day.isoformat()}' "
        f"AND security_id IN (SELECT security_id FROM {lines_table})"))
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ev_bars AS {bars}", [None, None])
    base = """
            SELECT b.security_id, b.trade_date, b.price / p.price - 1 AS r,
                   greatest(b.price_available_at, p.price_available_at) AS clock
            FROM _ev_bars b
            JOIN _ev_cal s ON s.trade_date=b.trade_date
            JOIN _ev_cal ps ON ps.session_number=s.session_number-1
            JOIN _ev_bars p ON p.security_id=b.security_id AND p.trade_date=ps.trade_date
            WHERE b.trade_date BETWEEN ? AND ?"""
    if not terminals:
        con.execute(f"CREATE OR REPLACE TEMP TABLE _ev_ret AS {base}", [first_day, last_day])
        return
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_ret AS
        WITH base AS ({base})
        SELECT base.* FROM base LEFT JOIN _ev_terminals t ON t.security_id=base.security_id
        WHERE t.delist_session IS NULL OR base.trade_date < t.delist_session
        UNION ALL
        SELECT t.security_id, t.delist_session, t.terminal_return, t.clock FROM _ev_terminals t
        WHERE t.usable AND t.delist_session BETWEEN ? AND ?
          AND t.security_id IN (SELECT security_id FROM {lines_table})
    """, [first_day, last_day, first_day, last_day])


def _year_ranges(first: dt.date, last: dt.date) -> list[tuple[dt.date, dt.date]]:
    return [(max(first, dt.date(year, 1, 1)), min(last, dt.date(year, 12, 31)))
            for year in range(first.year, last.year + 1)]


def _build_market(store: ResearchStore, version: str, options: EarningsEventOptions) -> dict[str, Any]:
    """Equal-weighted daily market over the eligible lines of the latest formation before each day."""
    con = store.con
    bounds = con.execute("SELECT min(trade_date), max(trade_date) FROM _ev_day_formation").fetchone()
    if bounds is None or bounds[0] is None:
        return {"days": 0}
    below = late = 0
    close = f"CAST(r.trade_date AS TIMESTAMP) + INTERVAL {BAR_CLOCK_HOURS} HOUR"
    for first, last in _year_ranges(bounds[0], bounds[1]):
        # Bar returns only (no terminal splice, no terminal-based drop): a name leaves the market
        # after its last bar, and a return enters only when both bars were known at the day's close
        # clock, so the market row's clock is always the session close (N1).
        _stage_returns(con, "_ev_universe_lines", first, last, terminals=False)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _ev_mkt AS
            SELECT r.trade_date, d.formation_date,
                   avg(r.r) FILTER (WHERE r.clock <= {close}) AS market_return,
                   count(*) FILTER (WHERE r.clock <= {close}) AS names,
                   count(*) FILTER (WHERE r.clock <= {close} AND abs(r.r) > 1.0) AS extreme,
                   count(*) FILTER (WHERE r.clock > {close}) AS late
            FROM _ev_ret r
            JOIN _ev_day_formation d ON d.trade_date=r.trade_date
            JOIN _ev_universe u ON u.formation_date=d.formation_date AND u.security_id=r.security_id
            GROUP BY r.trade_date, d.formation_date
        """)
        below_n, late_n = con.execute("SELECT count(*) FILTER (WHERE names < ?), coalesce(sum(late), 0) FROM _ev_mkt",
                                      [options.market_min_names]).fetchone()
        below, late = below + int(below_n), late + int(late_n)
        with store.transaction():
            con.execute(f"""
                INSERT INTO research_event_market
                SELECT ?, trade_date, formation_date, market_return, names, extreme,
                       CAST(trade_date AS TIMESTAMP) + INTERVAL {BAR_CLOCK_HOURS} HOUR
                FROM _ev_mkt WHERE names >= ?
            """, [version, options.market_min_names])
    days, low, mean, extreme = con.execute("""
        SELECT count(*), min(names), avg(names), count(*) FILTER (WHERE extreme_returns > 0)
        FROM research_event_market WHERE event_version=?
    """, [version]).fetchone()
    return {"days": int(days), "days_below_min_names": below, "late_bar_returns_excluded": late,
            "min_names": None if low is None else int(low),
            "mean_names": None if mean is None else round(float(mean), 3),
            "days_with_abs_return_over_100pct": int(extreme)}


def _build_windows(store: ResearchStore, version: str) -> None:
    """``_ev_windows``: EAR and run-up sums, counts, clocks and delisting per event with a price line."""
    con = store.con
    lo_e, hi_e = EAR_WINDOW
    lo_r, hi_r = RUNUP_WINDOW
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_windows (
            owner_cik VARCHAR, period_end DATE, ear_line_n BIGINT, ear_n BIGINT, ear_sum DOUBLE,
            ear_clock TIMESTAMP, ear_all_post BOOLEAN, runup_line_n BIGINT, runup_n BIGINT, runup_sum DOUBLE,
            runup_clock TIMESTAMP, runup_all_post BOOLEAN, delisting_session DATE)
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
        _stage_returns(con, "_ev_chunk_lines", span[0], span[1], terminals=True)
        ear, runup = f"rel BETWEEN {lo_e} AND {hi_e}", f"rel BETWEEN {lo_r} AND {hi_r}"
        con.execute(f"""
            INSERT INTO _ev_windows
            WITH days AS (
                SELECT e.owner_cik, e.period_end, e.security_id, s.trade_date, s.session_number - e.e_num AS rel
                FROM _ev_chunk e JOIN _ev_cal s ON s.session_number BETWEEN e.e_num + {lo_r} AND e.e_num + {hi_e}
            ), joined AS (
                SELECT d.owner_cik, d.period_end, d.rel, d.trade_date, t.delist_session,
                       coalesce(t.usable AND d.trade_date > t.delist_session, false) AS post,
                       r.r, m.market_return AS m, greatest(r.clock, m.available_at) AS clock
                FROM days d
                LEFT JOIN _ev_ret r ON r.security_id=d.security_id AND r.trade_date=d.trade_date
                LEFT JOIN research_event_market m ON m.event_version=? AND m.trade_date=d.trade_date
                LEFT JOIN _ev_terminals t ON t.security_id=d.security_id
            ), scored AS (
                SELECT *, CASE WHEN post THEN 0.0 ELSE r - m END AS ar, post OR r IS NOT NULL AS has_line
                FROM joined
            )
            SELECT owner_cik, period_end,
                   count(*) FILTER (WHERE {ear} AND has_line), count(ar) FILTER (WHERE {ear}),
                   sum(ar) FILTER (WHERE {ear}), max(clock) FILTER (WHERE {ear}), bool_and(post) FILTER (WHERE {ear}),
                   count(*) FILTER (WHERE {runup} AND has_line), count(ar) FILTER (WHERE {runup}),
                   sum(ar) FILTER (WHERE {runup}), max(clock) FILTER (WHERE {runup}),
                   bool_and(post) FILTER (WHERE {runup}),
                   min(trade_date) FILTER (WHERE trade_date = delist_session)
            FROM scored GROUP BY owner_cik, period_end
        """, [version])


def _stage_sue(store: ResearchStore, options: EarningsEventOptions) -> None:
    """``_ev_sue_rev``: every ``sue_ni`` state of each event's period, NULL states included, gated (I1)."""
    con = store.con
    table = "derived_metric_values"
    value_status = _column(store, table, "value_status", alias="d")
    history_status = _column(store, table, "history_status", alias="d")
    definition = _column(store, table, "definition_hash", alias="d")
    origin = _column(store, table, "value_origin", alias="d")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_sue_rev AS
        WITH rev AS (
            SELECT i.owner_cik, d.security_id, d.period_end, d.value, d.available_at, d.derived_value_id,
                   {value_status} AS value_status, {history_status} AS history_status,
                   {definition} AS definition_hash, {origin} AS value_origin
            FROM derived_metric_values d JOIN _ev_owner_ids i ON i.security_id=d.security_id
            WHERE d.source=? AND d.metric_code=? AND d.metric_window=? AND d.available_at IS NOT NULL
        ), matched AS (
            SELECT e.owner_cik, e.period_end AS fiscal_period_end, r.period_end AS sue_period_end, r.security_id,
                   r.value, r.available_at, r.derived_value_id, r.value_status, r.history_status,
                   r.definition_hash, r.value_origin,
                   dense_rank() OVER (PARTITION BY e.owner_cik, e.period_end
                                      ORDER BY abs(date_diff('day', r.period_end, e.period_end)), r.period_end)
                       AS closeness
            FROM _ev_events e JOIN rev r ON r.owner_cik=e.owner_cik
             AND r.period_end BETWEEN e.period_end - CAST(? AS INTEGER) AND e.period_end + CAST(? AS INTEGER)
        ), kept AS (SELECT * EXCLUDE (closeness) FROM matched WHERE closeness=1),
        owners AS (
            SELECT owner_cik, fiscal_period_end, count(DISTINCT security_id) AS derived_owners
            FROM kept GROUP BY owner_cik, fiscal_period_end
        )
        SELECT k.*,
               CASE WHEN o.derived_owners > 1 THEN 'ambiguous_derived_owner'
                    WHEN k.history_status IS DISTINCT FROM '{SUE_HISTORY_STATUS}' THEN 'uncertified_history'
                    WHEN k.definition_hash IS DISTINCT FROM ? THEN 'definition_hash_mismatch'
                    WHEN k.value_status IS DISTINCT FROM 'valid' OR k.value IS NULL OR NOT isfinite(k.value)
                        THEN 'invalid_current_state'
                    WHEN k.value_origin IS NULL OR k.value_origin IN ({_sql_list(INVALID_VALUE_ORIGINS)})
                        THEN 'invalid_value_origin'
                    ELSE '{VALID}' END AS state_reason,
               lead(k.available_at) OVER (PARTITION BY k.owner_cik, k.fiscal_period_end
                                          ORDER BY k.available_at, k.derived_value_id) AS next_state_at
        FROM kept k JOIN owners o ON o.owner_cik=k.owner_cik AND o.fiscal_period_end=k.fiscal_period_end
    """, [DERIVED_SOURCE_NAME, SUE_METRIC, SUE_WINDOW, options.sue_period_tolerance_days,
          options.sue_period_tolerance_days, sue_definition_hash()])


def _insert_events(store: ResearchStore, version: str) -> None:
    lo_r, hi_r = RUNUP_WINDOW
    ear_days = EAR_WINDOW[1] - EAR_WINDOW[0] + 1
    runup_days = hi_r - lo_r + 1

    def reason(n_line: str, n_both: str, all_post: str, days: int) -> str:
        return f"""CASE WHEN e.event_reason<>'{VALID}' THEN e.event_reason
                        WHEN e.line_formation_date IS NULL THEN '{REASON_NO_FORMATION}'
                        WHEN e.security_id IS NULL THEN '{REASON_NO_LINE}'
                        WHEN coalesce({all_post}, false) THEN '{REASON_DELISTED}'
                        WHEN coalesce({n_line}, 0) < {days} THEN '{REASON_INCOMPLETE}'
                        WHEN coalesce({n_both}, 0) < {days} THEN '{REASON_NO_MARKET}'
                        ELSE '{VALID}' END"""

    with store.transaction():
        store.con.execute(f"""
            INSERT INTO research_earnings_events ({", ".join(_EVENT_COLUMNS)})
            WITH sue_clock AS (
                -- visibility clock of the period's SUE: its first state, never before the event clock
                SELECT r.owner_cik, r.fiscal_period_end,
                       greatest(min(r.available_at), any_value(e.event_available_at)) AS visible_at
                FROM _ev_sue_rev r JOIN _ev_events e ON e.owner_cik=r.owner_cik AND e.period_end=r.fiscal_period_end
                GROUP BY r.owner_cik, r.fiscal_period_end
            ), first_sue AS (
                -- the state current at that clock (N5): newest state not after it
                SELECT r.* FROM _ev_sue_rev r
                JOIN sue_clock c ON c.owner_cik=r.owner_cik AND c.fiscal_period_end=r.fiscal_period_end
                WHERE r.available_at <= c.visible_at
                QUALIFY row_number() OVER (PARTITION BY r.owner_cik, r.fiscal_period_end
                                           ORDER BY r.available_at DESC, r.derived_value_id DESC) = 1
            ), scored AS (
                SELECT e.*, w.ear_sum, w.ear_clock, w.runup_sum, w.runup_clock, w.delisting_session,
                       {reason('w.ear_line_n', 'w.ear_n', 'w.ear_all_post', ear_days)} AS ear_reason,
                       {reason('w.runup_line_n', 'w.runup_n', 'w.runup_all_post', runup_days)} AS runup_reason,
                       s.value AS sue_value, s.state_reason AS sue_state_reason, s.sue_period_end,
                       s.derived_value_id AS sue_id, s.available_at AS sue_first, s.next_state_at AS sue_next
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
                   CASE WHEN event_reason='{VALID}' AND sue_state_reason='{VALID}' THEN sue_value END,
                   CASE WHEN event_reason<>'{VALID}' THEN event_reason
                        WHEN sue_id IS NULL THEN '{REASON_NO_SUE}' ELSE sue_state_reason END,
                   sue_period_end, sue_id, sue_first,
                   CASE WHEN event_reason='{VALID}' AND sue_id IS NOT NULL
                        THEN greatest(sue_first, event_available_at) END,
                   ?,
                   announcement_flags, CAST(k_candidates AS INTEGER), CAST(report_lag_business_days AS INTEGER),
                   sue_next, delisting_session
            FROM scored
        """, [version, MARKET_BASIS])


def _insert_features(store: ResearchStore, version: str, options: EarningsEventOptions) -> int:
    """Dense formation view; returns the row count.

    Per formation and line: the owner's latest *visible* event (event clock <= cutoff, session within
    ``max_age_days``) is chosen first; a feature whose value clock is still after the cutoff is
    ``event_value_pending`` (NULL), never the prior period's value (N2). ``sue_ni_event`` is the newest
    state of that event's period visible at the cutoff (the newest wins even when invalid).
    """
    con = store.con
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_vis AS
        SELECT owner_cik, fiscal_period_end, event_session, announcement_basis, event_available_at,
               ear_m1p1, ear_reason, ear_available_at, runup_m21_m2, runup_reason, runup_available_at
        FROM research_earnings_events WHERE event_version=? AND event_reason='{VALID}'
    """, [version])
    # An owner is a periodic filer at a formation once a 10-K/10-Q or an event is visible (M6).
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ev_filers AS
        SELECT o.owner_cik, f.first_filing, e.first_event_at
        FROM _ev_owners o
        LEFT JOIN (SELECT owner_cik, min(filing_date) AS first_filing FROM _ev_per_filing GROUP BY 1) f
          ON f.owner_cik=o.owner_cik
        LEFT JOIN (SELECT owner_cik, min(event_available_at) AS first_event_at FROM research_earnings_events
                   WHERE event_version=? AND event_reason='{VALID}' GROUP BY 1) e ON e.owner_cik=o.owner_cik
    """, [version])
    formations = [row[0] for row in con.execute("SELECT formation_date FROM _ev_forms ORDER BY 1").fetchall()]
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
                    SELECT l.formation_date, l.cutoff, l.security_id, e.*
                    FROM lines l JOIN _ev_vis e
                      ON e.owner_cik=l.owner_cik AND e.event_available_at <= l.cutoff
                     AND e.event_session >= l.formation_date - CAST(? AS INTEGER)
                    QUALIFY row_number() OVER (PARTITION BY l.formation_date, l.security_id
                                               ORDER BY e.fiscal_period_end DESC) = 1
                ), sue AS (
                    SELECT p.formation_date, p.security_id, r.value, r.state_reason,
                           greatest(r.available_at, p.event_available_at) AS clock
                    FROM picked p JOIN _ev_sue_rev r
                      ON r.owner_cik=p.owner_cik AND r.fiscal_period_end=p.fiscal_period_end
                     AND greatest(r.available_at, p.event_available_at) <= p.cutoff
                    QUALIFY row_number() OVER (PARTITION BY p.formation_date, p.security_id
                                               ORDER BY r.available_at DESC, r.derived_value_id DESC) = 1
                ), vals AS (
                    SELECT formation_date, security_id, '{FEATURE_EAR}' AS feature_id,
                           CASE WHEN ear_available_at <= cutoff THEN ear_m1p1 END AS value,
                           CASE WHEN ear_available_at <= cutoff THEN ear_reason ELSE '{REASON_PENDING}' END AS reason,
                           CASE WHEN ear_available_at <= cutoff THEN ear_available_at END AS clock,
                           fiscal_period_end, event_session, announcement_basis
                    FROM picked
                    UNION ALL
                    SELECT formation_date, security_id, '{FEATURE_RUNUP}',
                           CASE WHEN runup_available_at <= cutoff THEN runup_m21_m2 END,
                           CASE WHEN runup_available_at <= cutoff THEN runup_reason ELSE '{REASON_PENDING}' END,
                           CASE WHEN runup_available_at <= cutoff THEN runup_available_at END,
                           fiscal_period_end, event_session, announcement_basis
                    FROM picked
                    UNION ALL
                    SELECT formation_date, security_id, '{FEATURE_DAYS_SINCE}',
                           CAST(date_diff('day', event_session, formation_date) AS DOUBLE), '{VALID}',
                           event_available_at, fiscal_period_end, event_session, announcement_basis
                    FROM picked
                    UNION ALL
                    SELECT p.formation_date, p.security_id, '{FEATURE_SUE}',
                           CASE WHEN s.state_reason='{VALID}' THEN s.value END,
                           coalesce(s.state_reason, '{REASON_PENDING}'), s.clock,
                           p.fiscal_period_end, p.event_session, p.announcement_basis
                    FROM picked p LEFT JOIN sue s ON s.formation_date=p.formation_date AND s.security_id=p.security_id
                ), grid AS (
                    SELECT l.formation_date, l.cutoff, l.security_id, l.owner_cik, f.feature_id
                    FROM lines l CROSS JOIN (SELECT unnest(?::VARCHAR[]) AS feature_id) f
                )
                SELECT ?, g.formation_date, g.security_id, g.owner_cik, g.feature_id, v.value,
                       CASE WHEN v.feature_id IS NOT NULL THEN v.reason
                            WHEN NOT coalesce(fl.first_filing <= g.formation_date
                                              OR fl.first_event_at <= g.cutoff, false) THEN '{REASON_NO_FILER}'
                            ELSE '{REASON_NO_RECENT}' END,
                       v.clock, v.fiscal_period_end, v.event_session, v.announcement_basis
                FROM grid g
                LEFT JOIN vals v
                  ON v.formation_date=g.formation_date AND v.security_id=g.security_id AND v.feature_id=g.feature_id
                LEFT JOIN _ev_filers fl ON fl.owner_cik=g.owner_cik
            """, [batch[0], batch[-1], options.max_age_days, list(EVENT_FEATURES), version])
    rows = con.execute("SELECT count(*) FROM research_event_features WHERE event_version=?", [version]).fetchone()
    return int(rows[0])


# ---------------------------------------------------------------------------
# Digests
# ---------------------------------------------------------------------------

def _events_digest(con: Any, version: str) -> str:
    row = con.execute("""
        SELECT sha256(coalesce(string_agg(md5(concat_ws('|', owner_cik, fiscal_period_end, periodic_accession,
                   announcement_basis, coalesce(rdq_rejection, ''), coalesce(announcement_accession, ''),
                   coalesce(CAST(acceptance_at AS VARCHAR), ''), timing_source, session_timing,
                   coalesce(CAST(event_session AS VARCHAR), ''), coalesce(CAST(event_available_at AS VARCHAR), ''),
                   event_reason, coalesce(security_id, ''), coalesce(printf('%.12e', ear_m1p1), ''), ear_reason,
                   coalesce(CAST(ear_available_at AS VARCHAR), ''), coalesce(printf('%.12e', runup_m21_m2), ''),
                   runup_reason, coalesce(printf('%.12e', sue), ''), sue_reason,
                   coalesce(CAST(sue_available_at AS VARCHAR), ''), coalesce(CAST(sue_valid_to AS VARCHAR), ''),
                   coalesce(announcement_flags, ''), coalesce(CAST(delisting_session AS VARCHAR), ''))), ''
                   ORDER BY owner_cik, fiscal_period_end), ''))
        FROM research_earnings_events WHERE event_version=?
    """, [version]).fetchone()
    return str(row[0])


def _features_digest(con: Any, version: str) -> str:
    """sha256 over per-formation digests of the stored feature rows (bounded per chunk)."""
    formations = [row[0] for row in con.execute(
        "SELECT DISTINCT formation_date FROM research_event_features WHERE event_version=? ORDER BY 1",
        [version]).fetchall()]
    digests: list[str] = []
    for start in range(0, len(formations), _DIGEST_CHUNK):
        batch = formations[start:start + _DIGEST_CHUNK]
        digests.extend(str(row[1]) for row in con.execute("""
            SELECT formation_date, sha256(string_agg(md5(concat_ws('|', security_id, owner_cik, feature_id,
                       coalesce(printf('%.12e', raw_value), ''), reason, coalesce(CAST(available_at AS VARCHAR), ''),
                       coalesce(CAST(fiscal_period_end AS VARCHAR), ''), coalesce(CAST(event_session AS VARCHAR), ''),
                       coalesce(announcement_basis, ''))), '' ORDER BY security_id, feature_id))
            FROM research_event_features WHERE event_version=? AND formation_date BETWEEN ? AND ?
            GROUP BY formation_date ORDER BY formation_date
        """, [version, batch[0], batch[-1]]).fetchall())
    return _sha("\n".join(digests))


def _reason_counts(con: Any, version: str) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for column in ("ear_reason", "runup_reason", "sue_reason"):
        out[column] = {str(k): int(n) for k, n in con.execute(
            f"SELECT {column}, count(*) FROM research_earnings_events WHERE event_version=? GROUP BY 1 ORDER BY 1",
            [version]).fetchall()}
    out["events_with_delisting_in_window"] = int(con.execute(
        "SELECT count(*) FROM research_earnings_events WHERE event_version=? AND delisting_session IS NOT NULL",
        [version]).fetchone()[0])
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
            JOIN research_event_versions v ON v.event_version=x.event_version
            JOIN research_panel_calendar c ON c.run_id=v.panel_run_id AND c.formation_date=x.formation_date
            WHERE x.event_version=? AND x.available_at IS NOT NULL
              AND (x.available_at > c.cutoff OR x.event_session >= x.formation_date)""", [version]),
        "feature_grain": ("""
            SELECT count(*) FROM (SELECT formation_date, security_id, feature_id FROM research_event_features
                                  WHERE event_version=? GROUP BY ALL HAVING count(*) > 1)""", [version]),
        # N1: the market is bar-close clocked; no terminal (or late bar) clock may leak into it.
        "market_clock_not_session_close": (f"""
            SELECT count(*) FROM research_event_market WHERE event_version=?
              AND available_at IS DISTINCT FROM CAST(trade_date AS TIMESTAMP) + INTERVAL {BAR_CLOCK_HOURS} HOUR""",
                                           [version]),
        # N5: an event-row SUE is valid while it is visible.
        "sue_superseded_before_visible": ("""
            SELECT count(*) FROM research_earnings_events WHERE event_version=? AND sue_available_at IS NOT NULL
              AND sue_valid_to IS NOT NULL AND sue_valid_to <= sue_available_at""", [version]),
    }
    return {name: int(con.execute(sql, params).fetchone()[0]) for name, (sql, params) in checks.items()}


def validate_event_version(store: ResearchStore, event_version: str) -> dict[str, int]:
    """Re-run the seal invariants and recompute both content digests of a sealed version (M4)."""
    con = store.con
    row = con.execute("SELECT status, events_sha256, features_sha256, query_version FROM research_event_versions "
                      "WHERE event_version=?", [event_version]).fetchone()
    if row is None:
        raise EventStoreError(f"event version {event_version} is absent")
    status, events_sha, features_sha, query_version = row
    if query_version != QUERY_VERSION:  # N4: legacy rows lack the v2 columns and feature ids
        raise EventStoreError(f"event version {event_version} was built by {query_version!r}; only "
                              f"{QUERY_VERSION!r} versions are valid (rebuild it)")
    if status not in SEALED_STATUSES:
        raise EventStoreError(f"event version {event_version} is {status!r}, not sealed")
    checks = _checks(con, event_version, _calendar_keys())
    bad = {name: n for name, n in checks.items() if n}
    if bad:
        raise EventStoreError(f"event version {event_version} violates {bad}")
    if _events_digest(con, event_version) != events_sha:
        raise EventStoreError(f"event version {event_version}: event rows differ from their sealed digest")
    if _features_digest(con, event_version) != features_sha:
        raise EventStoreError(f"event version {event_version}: feature rows differ from their sealed digest")
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
    inputs = _input_fingerprints(store, calendar)
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
        diagnostic["periods"] = _stage_periods(store, options)
        _stage_announcements(store, options)
        diagnostic["announcements"] = _announcement_diagnostic(con)
        diagnostic["terminals"] = _stage_terminals(store)
        diagnostic["market"] = _build_market(store, version, options)
        _build_windows(store, version)
        _stage_sue(store, options)
        _insert_events(store, version)
        feature_rows = _insert_features(store, version, options)
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
                  _features_digest(con, version), _now(), version])
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
    blockers = [MARKET_BLOCKER, EARLY_CLOSE_BLOCKER, WIRE_BLOCKER, SELECTION_BLOCKER]
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
    implausible = announcements["timing_source"].get(SOURCE_IMPLAUSIBLE, 0)
    if implausible:
        blockers.append(f"report_date_implausible:{implausible}/{events}")
    if diagnostic["terminals"].get("table") == "absent":
        blockers.append("delisting_terminal_returns_absent")
    if not diagnostic["panel"]["owners"]:
        blockers.append("no_linked_owners")
    return tuple(blockers)


__all__ = [
    "ANNOUNCEMENT_BASES",
    "ANNOUNCEMENT_FLAGS",
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
    "sue_definition_hash",
    "validate_event_version",
]
