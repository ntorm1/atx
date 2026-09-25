"""Monthly point-in-time research panel (task R2a).

One formation per closed calendar month: the month's last NYSE session, when
it was observed. The decision cutoff is that session at 22:00 UTC and entry is
the next observed session. A month whose last session is missing is recorded as
``missing_month_end`` and never replaced by an earlier (mid-month) session.

Selection, owner association, lineage proof and rejection rules are the FQ1
builder's (:func:`atx_db.fundamental_signal_research.stage_selected_states`)
run over a one-row month-end calendar relation and a metric batch. Lineage is
resolved once per selected ``derived_value_id`` per run (the proof table is the
run cache), never once per date.

Bases (rulings RX1/RX6):

``reconstructed``
    ``us_listed_reconstructed_v1`` membership and the A5 reconstructed
    price-line -> owner bridge (current SEC ticker map backcast, modeled clock).
    Rows are labeled ``identity_basis`` / ``universe_basis`` /
    ``availability_basis`` and are research input, never certifiable history.
``strict``
    ``us_listed_v1`` membership with CIKs from dated identifier history only. It
    is always attempted; with no valid cohort anywhere the run is sealed as
    ``untestable_strict`` with zero value rows.

Output is a long raw panel in the research store, bounded to one month-end and
one metric batch per transaction:
``research_panel_values(formation_date, security_id, feature_id, raw_value,
reason, lineage_status, available_at, fiscal_period_end, owner_cik, bases...)``.
Only valid-cohort members with a selected state get a value row; every other
outcome (cohort exclusions, ``missing_metric_state``, missing market rows) is
counted per formation and feature in ``research_panel_coverage.reasons_json``
and per member in ``research_panel_cohort``. ``raw_value`` is kept only for
``valid`` and ``stale_current_anchor`` rows (both point-in-time safe); every
other reason carries NULL so a rejected value cannot leak downstream.
Consumers must read only runs whose status is ``complete`` (or inspect
``untestable_strict`` / ``blocked_empty`` as evidence) after
:func:`validate_research_panel`.
"""

from __future__ import annotations

import calendar as _calendar
import datetime as dt
import hashlib
import json
import math
import re
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import fundamental_signal_research as fsr
from .._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from ..derived_registry import DERIVED_SOURCE_NAME
from ..market_daily import MARKET_DAILY_SOURCE_NAME, MARKET_DAILY_STRICT_SOURCE_NAME
from ..universe_us_listed import UNIVERSE_SOURCE_NAME
from .store import ResearchStore

QUERY_VERSION = "research-monthly-pit-panel-v1"
BASIS_STRICT = "strict"
BASIS_RECONSTRUCTED = "reconstructed"
BASES = (BASIS_STRICT, BASIS_RECONSTRUCTED)
FUNDAMENTAL_AVAILABILITY_BASIS = "conservative_filing_46h"
MARKET_AVAILABILITY_BASIS = "modeled_trade_date_22h"
DERIVED_WINDOWS = ("q", "ttm", "avg2")
MARKET_WINDOW = "daily"
PANEL_WINDOWS = (*DERIVED_WINDOWS, MARKET_WINDOW)
DECISION_HOUR = 22
DEFAULT_MAX_AGE_DAYS = 200
DEFAULT_ANNUAL_MAX_AGE_DAYS = 400
# Reasons whose selected value is point-in-time safe to publish.
VALUE_BEARING_REASONS = ("valid", "stale_current_anchor")
SEALED_STATUSES = ("complete", "untestable_strict", "blocked_empty")

CALENDAR_FORMED = "formed"
CALENDAR_MISSING_MONTH_END = "missing_month_end"
CALENDAR_RULE_CONFLICT = "calendar_rule_conflict"
CALENDAR_MISSING_NEXT_SESSION = "missing_next_session"
CALENDAR_AFTER_CUTOFF = "after_knowledge_cutoff"

_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_SHA = re.compile(r"^[0-9a-f]{64}$")
_BASE_BLOCKERS = (
    "modeled_filing_availability_not_exact_delivery_vintage",
    "source_backfill_and_historical_membership_may_be_incomplete",
    "month_end_calendar_from_nyse_rules_and_observed_sessions",
    "research_only_not_release_eligible",
)
# Unscheduled full-day NYSE closures (weekday rule holidays are computed).
NYSE_SPECIAL_CLOSURES = frozenset({
    dt.date(1994, 4, 27), dt.date(2001, 9, 11), dt.date(2001, 9, 12), dt.date(2001, 9, 13),
    dt.date(2001, 9, 14), dt.date(2004, 6, 11), dt.date(2007, 1, 2), dt.date(2012, 10, 29),
    dt.date(2012, 10, 30), dt.date(2018, 12, 5), dt.date(2025, 1, 9),
})

_CODE_FILES = (Path(__file__), Path(fsr.__file__))


# ---------------------------------------------------------------------------
# Small canonical helpers
# ---------------------------------------------------------------------------

def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _sha(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _iso(value: Any) -> Any:
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    if isinstance(value, float) and not math.isfinite(value):
        return {"nonfinite": repr(value)}
    return value


def _stream_digest(con: Any, query: str, params: list[Any]) -> str:
    digest = hashlib.sha256()
    cursor = con.execute(query, params)
    while batch := cursor.fetchmany(2048):
        for row in batch:
            digest.update(_canonical([_iso(item) for item in row]).encode("utf-8"))
            digest.update(b"\n")
    return digest.hexdigest()


def _utc_naive(value: dt.datetime, label: str) -> dt.datetime:
    if not isinstance(value, dt.datetime) or value.tzinfo is None or value.utcoffset() is None:
        raise ValueError(f"{label} must be timezone-aware UTC")
    if value.utcoffset() != dt.timedelta(0):
        raise ValueError(f"{label} must have UTC offset zero")
    return value.astimezone(dt.UTC).replace(tzinfo=None)


def _sql_text(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


# ---------------------------------------------------------------------------
# NYSE month-end rule
# ---------------------------------------------------------------------------

def _easter(year: int) -> dt.date:
    """Gregorian Easter Sunday (anonymous algorithm)."""
    a, b, c = year % 19, year // 100, year % 100
    d, e = b // 4, b % 4
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l_ = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l_) // 451
    month = (h + l_ - 7 * m + 114) // 31
    day = (h + l_ - 7 * m + 114) % 31 + 1
    return dt.date(year, month, day)


def _nth_weekday(year: int, month: int, weekday: int, nth: int) -> dt.date:
    first = dt.date(year, month, 1)
    return first + dt.timedelta(days=(weekday - first.weekday()) % 7 + 7 * (nth - 1))


def _last_weekday(year: int, month: int, weekday: int) -> dt.date:
    last = dt.date(year, month, _calendar.monthrange(year, month)[1])
    return last - dt.timedelta(days=(last.weekday() - weekday) % 7)


def _observed(day: dt.date) -> dt.date:
    if day.weekday() == 5:
        return day - dt.timedelta(days=1)
    if day.weekday() == 6:
        return day + dt.timedelta(days=1)
    return day


def nyse_full_day_closures(year: int) -> frozenset[dt.date]:
    """NYSE full-day holidays of ``year`` (rule-based) plus known special closures.

    New Year's Day on a Saturday is not observed on the prior Friday (NYSE rule).
    """
    days: set[dt.date] = set()
    new_year = dt.date(year, 1, 1)
    if new_year.weekday() == 6:
        days.add(new_year + dt.timedelta(days=1))
    elif new_year.weekday() < 5:
        days.add(new_year)
    if year >= 1998:
        days.add(_nth_weekday(year, 1, 0, 3))
    days.add(_nth_weekday(year, 2, 0, 3))
    days.add(_easter(year) - dt.timedelta(days=2))
    days.add(_last_weekday(year, 5, 0))
    if year >= 2022:
        days.add(_observed(dt.date(year, 6, 19)))
    days.add(_observed(dt.date(year, 7, 4)))
    days.add(_nth_weekday(year, 9, 0, 1))
    days.add(_nth_weekday(year, 11, 3, 4))
    days.add(_observed(dt.date(year, 12, 25)))
    days.update(day for day in NYSE_SPECIAL_CLOSURES if day.year == year)
    return frozenset(day for day in days if day.year == year)


def expected_month_end_session(year: int, month: int) -> dt.date:
    """The last NYSE session of the month under the holiday rules."""
    closures = nyse_full_day_closures(year)
    day = dt.date(year, month, _calendar.monthrange(year, month)[1])
    while day.weekday() >= 5 or day in closures:
        day -= dt.timedelta(days=1)
    return day


@dataclass(frozen=True)
class CalendarRow:
    month_start: dt.date
    expected_session: dt.date
    last_observed_session: dt.date | None
    formation_date: dt.date | None
    cutoff: dt.datetime | None
    entry_date: dt.date | None
    status: str

    def canonical(self) -> list[Any]:
        return [_iso(value) for value in (
            self.month_start, self.expected_session, self.last_observed_session,
            self.formation_date, self.cutoff, self.entry_date, self.status)]


def _months(start: dt.date, end: dt.date) -> list[dt.date]:
    months = []
    cursor = dt.date(start.year, start.month, 1)
    while cursor <= end:
        months.append(cursor)
        cursor = dt.date(cursor.year + cursor.month // 12, cursor.month % 12 + 1, 1)
    return months


def month_end_calendar(sessions: Sequence[dt.date], *, start_month: dt.date, end_month: dt.date,
                       as_of_date: dt.date, run_at: dt.datetime) -> list[CalendarRow]:
    """Classify every month in range against the observed session list.

    ``sessions`` are the observed decision sessions known by ``run_at`` and not
    after ``as_of_date``. ``run_at`` is naive UTC.
    """
    observed = sorted(set(sessions))
    observed_set = set(observed)
    rows = []
    for month in _months(start_month, end_month):
        expected = expected_month_end_session(month.year, month.month)
        in_month = [day for day in observed if day.year == month.year and day.month == month.month]
        last = in_month[-1] if in_month else None
        cutoff = dt.datetime.combine(expected, dt.time(DECISION_HOUR))
        later = [day for day in observed if day > expected]
        entry = later[0] if later else None
        if expected > as_of_date or cutoff > run_at:
            status, formation, cutoff_value, entry = CALENDAR_AFTER_CUTOFF, None, None, None
        elif last is not None and last > expected:
            status, formation, cutoff_value, entry = CALENDAR_RULE_CONFLICT, None, None, None
        elif expected not in observed_set:
            status, formation, cutoff_value, entry = CALENDAR_MISSING_MONTH_END, None, None, None
        elif entry is None:
            status, formation, cutoff_value = CALENDAR_MISSING_NEXT_SESSION, None, None
        else:
            status, formation, cutoff_value = CALENDAR_FORMED, expected, cutoff
        rows.append(CalendarRow(month, expected, last, formation, cutoff_value, entry, status))
    return rows


# ---------------------------------------------------------------------------
# Options, features and batches
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class PanelFeature:
    feature_id: str
    metric_code: str
    metric_window: str

    @property
    def is_market(self) -> bool:
        return self.metric_window == MARKET_WINDOW


def default_panel_features() -> tuple[PanelFeature, ...]:
    """Every seed metric with a panel window; ``feature_id`` = ``metric_code``."""
    return tuple(sorted(
        (PanelFeature(row.metric_code, row.metric_code, row.window)
         for row in fsr.default_derived_definitions() if row.window in PANEL_WINDOWS),
        key=lambda feature: feature.feature_id,
    ))


def canonical_features(features: Iterable[PanelFeature | dict[str, Any]] | None) -> tuple[PanelFeature, ...]:
    source = default_panel_features() if features is None else tuple(features)
    registry = {row.metric_code: row for row in fsr.default_derived_definitions()}
    result: list[PanelFeature] = []
    ids: set[str] = set()
    keys: set[tuple[str, str]] = set()
    for item in source:
        if isinstance(item, dict):
            if set(item) != {"feature_id", "metric_code", "metric_window"}:
                raise ValueError("feature requires only feature_id, metric_code and metric_window")
            item = PanelFeature(item["feature_id"], item["metric_code"], item["metric_window"])
        if not isinstance(item, PanelFeature):
            raise ValueError(f"unsupported feature specification: {item!r}")
        for text in (item.feature_id, item.metric_code):
            if not isinstance(text, str) or not _ID.fullmatch(text):
                raise ValueError(f"invalid feature identifier: {text!r}")
        if item.metric_window not in PANEL_WINDOWS:
            raise ValueError(f"{item.feature_id}: unsupported window {item.metric_window!r}")
        definition = registry.get(item.metric_code)
        if definition is None or definition.window != item.metric_window:
            raise ValueError(f"{item.feature_id}: unknown seed metric {item.metric_code}/{item.metric_window}")
        key = (item.metric_code, item.metric_window)
        if item.feature_id in ids or key in keys:
            raise ValueError(f"duplicate feature or metric: {item.feature_id} {key}")
        ids.add(item.feature_id)
        keys.add(key)
        result.append(item)
    if not 1 <= len(result) <= 1024:
        raise ValueError("feature count must be between 1 and 1024")
    return tuple(sorted(result, key=lambda feature: feature.feature_id))


@dataclass(frozen=True)
class ResearchPanelOptions:
    run_id: str
    basis: str
    start_month: dt.date
    end_month: dt.date
    as_of_date: dt.date
    run_at: dt.datetime
    features: tuple[PanelFeature | dict[str, Any], ...] | None = None
    metric_batch_size: int = 16
    max_age_days: int = DEFAULT_MAX_AGE_DAYS
    annual_max_age_days: int = DEFAULT_ANNUAL_MAX_AGE_DAYS
    eligible_security_types: tuple[str, ...] = fsr.RECONSTRUCTED_ELIGIBLE_TYPES
    include_unlisted_tail: bool = True
    resume: bool = False


@dataclass(frozen=True)
class ResearchPanelResult:
    run_id: str
    status: str
    months: int
    formations: int
    value_rows: int
    valid_values: int
    panel_sha256: str
    blockers: tuple[str, ...]


@dataclass(frozen=True)
class ResearchPanelValidation:
    run_id: str
    status: str
    basis: str
    panel_sha256: str
    formations: tuple[dt.date, ...]
    features: tuple[str, ...]
    value_rows: int
    valid_values: int


@dataclass(frozen=True)
class _Batch:
    ordinal: int
    features: tuple[PanelFeature, ...]

    @property
    def is_market(self) -> bool:
        return self.features[0].is_market


def _batches(features: tuple[PanelFeature, ...], size: int) -> tuple[_Batch, ...]:
    derived = sorted((f for f in features if not f.is_market), key=lambda f: (f.metric_code, f.metric_window))
    market = sorted((f for f in features if f.is_market), key=lambda f: f.metric_code)
    groups = [tuple(derived[i:i + size]) for i in range(0, len(derived), size)]
    if market:
        groups.append(tuple(market))
    return tuple(_Batch(index, group) for index, group in enumerate(groups))


def _basis_labels(basis: str) -> dict[str, str]:
    if basis == BASIS_STRICT:
        return {"universe_id": fsr.STRICT_UNIVERSE_ID, "identity_basis": fsr.IDENTITY_BASIS_STRICT,
                "market_source": MARKET_DAILY_STRICT_SOURCE_NAME}
    return {"universe_id": fsr.RECONSTRUCTED_UNIVERSE_ID, "identity_basis": fsr.IDENTITY_BASIS_RECONSTRUCTED,
            "market_source": MARKET_DAILY_SOURCE_NAME}


def _validate(options: ResearchPanelOptions) -> tuple[dt.datetime, tuple[PanelFeature, ...], dict[str, Any]]:
    if not isinstance(options.run_id, str) or not _ID.fullmatch(options.run_id):
        raise ValueError("run_id must be a lower-case identifier")
    if options.basis not in BASES:
        raise ValueError(f"basis must be one of {BASES}")
    if any(type(value) is not dt.date for value in (options.start_month, options.end_month, options.as_of_date)):
        raise ValueError("start_month, end_month and as_of_date must be dates")
    if options.start_month > options.end_month:
        raise ValueError("start_month must not be after end_month")
    if (options.end_month.year - options.start_month.year) * 12 + options.end_month.month \
            - options.start_month.month >= 600:
        raise ValueError("requested range exceeds 600 months")
    for label, value in (("max_age_days", options.max_age_days),
                         ("annual_max_age_days", options.annual_max_age_days)):
        if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 3650:
            raise ValueError(f"{label} must be 1..3650")
    if options.annual_max_age_days < options.max_age_days:
        raise ValueError("annual_max_age_days must be >= max_age_days")
    size = options.metric_batch_size
    if isinstance(size, bool) or not isinstance(size, int) or not 1 <= size <= 64:
        raise ValueError("metric_batch_size must be 1..64")
    types = tuple(options.eligible_security_types)
    if not types or any(not isinstance(t, str) or not re.fullmatch(r"[A-Za-z_]{1,32}", t) for t in types):
        raise ValueError("eligible_security_types must be simple type names")
    run_at = _utc_naive(options.run_at, "run_at")
    if run_at.date() < options.as_of_date:
        raise ValueError("run_at precedes as_of_date")
    features = canonical_features(options.features)
    labels = _basis_labels(options.basis)
    spec = {
        "basis": options.basis,
        "universe_id": labels["universe_id"],
        "identity_basis": labels["identity_basis"],
        "features": [[f.feature_id, f.metric_code, f.metric_window] for f in features],
        "metric_batch_size": size,
        "max_age_days": options.max_age_days,
        "annual_max_age_days": options.annual_max_age_days,
        "annual_value_origins": list(fsr.ANNUAL_VALUE_ORIGINS),
        "eligible_security_types": sorted(set(types)) if options.basis == BASIS_RECONSTRUCTED else ["common"],
        "include_unlisted_tail": bool(options.include_unlisted_tail) if options.basis == BASIS_RECONSTRUCTED
        else False,
        "value_bearing_reasons": list(VALUE_BEARING_REASONS),
        "decision_policy": "last NYSE session of each closed month at 22:00 UTC; next observed session entry",
    }
    return run_at, features, spec


# ---------------------------------------------------------------------------
# Warehouse inputs
# ---------------------------------------------------------------------------

def _require_inputs(store: ResearchStore, basis: str, features: tuple[PanelFeature, ...],
                    build_bridge: bool) -> None:
    needed = ["derived_metric_values", "derived_metric_definitions", "fundamental_standardized",
              "universe_us_listed_membership", "market_daily_metrics"]
    if basis == BASIS_STRICT:
        needed.append("security_identifier_history")
    elif build_bridge:
        needed.append("equity_daily_bars")
    missing = [table for table in needed if not store.warehouse_has(table)]
    if missing:
        raise RuntimeError(f"warehouse is missing research inputs: {missing}")
    if not store.warehouse_has("derived_metric_values", "selected_input_refs_hash"):
        raise RuntimeError("warehouse must already have DL1 selected-input lineage migration 0323")
    absent = [f.metric_code for f in features if f.is_market
              and not store.warehouse_has("market_daily_metrics", f.metric_code)]
    if absent:
        raise RuntimeError(f"market_daily_metrics lacks daily feature columns: {absent}")


def observed_sessions(con: Any, *, market_source: str, first_day: dt.date,
                      as_of_date: dt.date, run_at: dt.datetime) -> list[dt.date]:
    """Observed decision sessions (FQ1 session rule) known by ``run_at``."""
    rows = con.execute("""
        SELECT DISTINCT trade_date FROM market_daily_metrics
        WHERE source=? AND trade_date BETWEEN ? AND ?
          AND available_at <= ? AND as_of_date <= ?
          AND available_at <= CAST(trade_date AS TIMESTAMP) + INTERVAL 22 HOUR
          AND as_of_date <= trade_date
          AND close IS NOT NULL AND isfinite(close) AND close>0
        ORDER BY trade_date
    """, [market_source, first_day, as_of_date, run_at, as_of_date]).fetchall()
    return [row[0] for row in rows]


_LINK_COLUMNS = ("price_security_id", "cik", "valid_from", "valid_to", "available_at",
                 "identity_basis", "link_method", "unlinked_reason")
_LINK_TYPES = ("VARCHAR", "VARCHAR", "DATE", "DATE", "TIMESTAMP", "VARCHAR", "VARCHAR", "VARCHAR")


def stage_owner_links(con: Any, rows: Iterable[Any], table: str = "_fs_owner_links") -> dict[str, Any]:
    """Stage bridge rows (A5 ``BridgeRow`` or equivalent) as the owner-link relation.

    Returns a JSON-safe accounting of the staged rows.
    """
    if not re.fullmatch(r"_[a-z][a-z0-9_]{0,62}", table):
        raise ValueError(f"unsupported staging relation: {table!r}")
    payload = [tuple(getattr(row, column) for column in _LINK_COLUMNS) for row in rows]
    columns = ", ".join(f"{name} {kind}" for name, kind in zip(_LINK_COLUMNS, _LINK_TYPES, strict=True))
    con.execute(f"CREATE OR REPLACE TEMP TABLE {table} ({columns})")
    row_sql = "(" + ",".join(f"CAST(? AS {kind})" for kind in _LINK_TYPES) + ")"
    for start in range(0, len(payload), 1000):
        chunk = payload[start:start + 1000]
        con.execute(f"INSERT INTO {table} VALUES " + ",".join([row_sql] * len(chunk)),
                    [value for row in chunk for value in row])
    linked: dict[str, int] = {}
    unlinked: dict[str, int] = {}
    for row in payload:
        if row[1] is not None:
            linked[str(row[6])] = linked.get(str(row[6]), 0) + 1
        else:
            unlinked[str(row[7])] = unlinked.get(str(row[7]), 0) + 1
    return {"rows": len(payload), "linked_by_method": dict(sorted(linked.items())),
            "unlinked_by_reason": dict(sorted(unlinked.items()))}


def _owner_bridge_rows(store: ResearchStore) -> tuple[tuple[Any, ...], dict[str, Any]]:
    from ..market_owner_bridge import OWNER_MODE_RECONSTRUCTED, build_market_owner_bridge

    bridge = build_market_owner_bridge(store, mode=OWNER_MODE_RECONSTRUCTED,  # type: ignore[arg-type]
                                       derived_source=DERIVED_SOURCE_NAME)
    return bridge.rows, bridge.summary()


# ---------------------------------------------------------------------------
# Digest projections (shared by the builder and the validator)
# ---------------------------------------------------------------------------

_COHORT_COLUMNS = ("security_id", "symbol", "security_type", "exchange_code", "membership_reason",
                   "membership_cik", "owner_cik", "identity_basis", "owner_link_method",
                   "owner_link_reason", "cohort_reason")
_VALUE_COLUMNS = ("formation_date", "security_id", "feature_id", "metric_code", "metric_window",
                  "raw_value", "reason", "lineage_status", "available_at", "latest_input_clock",
                  "period_end", "fiscal_period_start", "fiscal_period_end", "value_origin",
                  "age_days", "max_age_days", "owner_cik", "derived_value_id",
                  "derived_owner_security_id", "lineage_digest", "identity_basis",
                  "universe_basis", "availability_basis")


def _row_json(columns: Sequence[str]) -> str:
    return "to_json(struct_pack(" + ",".join(f"{c} := {c}" for c in columns) + "))"


_COHORT_DIGEST_SQL = ("SELECT sha256(coalesce(string_agg(" + _row_json(_COHORT_COLUMNS)
                      + ", chr(10) ORDER BY security_id), '')) FROM {relation} {where}")
_VALUE_DIGEST_SQL = ("SELECT feature_id, sha256(coalesce(string_agg(" + _row_json(_VALUE_COLUMNS)
                     + ", chr(10) ORDER BY security_id), '')) FROM {relation} {where} GROUP BY feature_id")
_EMPTY_SHA = hashlib.sha256(b"").hexdigest()


# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------

def _code_sha() -> str:
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())
    return digest.hexdigest()


def _definitions(con: Any, features: tuple[PanelFeature, ...]) -> tuple[str, str, dict[tuple[str, str], str]]:
    roots = {(f.metric_code, f.metric_window) for f in features if not f.is_market}
    derived: list[Any] = []
    hashes: dict[tuple[str, str], str] = {}
    if roots:
        encoded, _, hashes = fsr.resolve_definitions(con, roots)
        derived = json.loads(encoded)
    registry = {row.metric_code: row for row in fsr.default_derived_definitions()}
    market = [{"metric_code": f.metric_code, "metric_window": f.metric_window,
               "expression": registry[f.metric_code].expression,
               "inputs": list(registry[f.metric_code].inputs),
               "version": registry[f.metric_code].version,
               "source_column": f"market_daily_metrics.{f.metric_code}"}
              for f in features if f.is_market]
    encoded = _canonical({"derived": derived, "market": sorted(market, key=lambda m: m["metric_code"])})
    return encoded, _sha(encoded), hashes


def _blockers(basis: str, formations: int, valid_members: int, valid_values: int,
              calendar_rows: Sequence[CalendarRow]) -> tuple[str, ...]:
    blockers = list(_BASE_BLOCKERS)
    if basis == BASIS_RECONSTRUCTED:
        blockers.append("reconstructed_identity_universe_and_availability_not_certifiable")
    if any(row.status in (CALENDAR_MISSING_MONTH_END, CALENDAR_RULE_CONFLICT) for row in calendar_rows):
        blockers.append("missing_or_conflicting_month_end_sessions")
    if not formations:
        blockers.append("no_formed_month_ends")
    if not valid_members:
        blockers.append("no_valid_cohort_members")
    if not valid_values:
        blockers.append("no_valid_feature_values")
    return tuple(blockers)


def _stage_formation_cohort(store: ResearchStore, row: CalendarRow, spec: dict[str, Any]) -> dict[str, Any]:
    con = store.con
    counts = fsr.stage_cohort(
        con, calendar_table="_rp_calendar", universe_id=spec["universe_id"],
        identity_basis=spec["identity_basis"], universe_source=UNIVERSE_SOURCE_NAME,
        owner_links_table="_fs_owner_links" if spec["basis"] == BASIS_RECONSTRUCTED else None,
        eligible_security_types=tuple(spec["eligible_security_types"]),
        include_unlisted_tail=spec["include_unlisted_tail"], membership_detail=True,
    )
    visible, eligible, _, valid = counts.get(row.formation_date, (0, 0, 0, 0))
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_cohort_rows AS
        SELECT decision_date AS formation_date, security_id, symbol, security_type, exchange_code,
               membership_reason, membership_cik, cik AS owner_cik, identity_basis,
               owner_link_method, owner_link_reason, cohort_reason
        FROM _fs_cohort_cal
    """)
    reasons = {
        "cohort_reason": dict(con.execute(
            "SELECT cohort_reason, count(*) FROM _rp_cohort_rows GROUP BY 1 ORDER BY 1").fetchall()),
        "owner_link_reason": dict(con.execute("""
            SELECT coalesce(owner_link_reason,'linked'), count(*) FROM _rp_cohort_rows
            WHERE cohort_reason NOT IN ('overlapping_membership','not_common') GROUP BY 1 ORDER BY 1
        """).fetchall()),
    }
    digest = con.execute(_COHORT_DIGEST_SQL.format(relation="_rp_cohort_rows", where="")).fetchone()[0]
    return {"visible": visible, "eligible": eligible, "valid": valid,
            "reasons_json": _canonical(reasons), "digest": digest}


def _derived_batch(store: ResearchStore, run_id: str, batch: _Batch, spec: dict[str, Any],
                   hashes: dict[tuple[str, str], str]) -> dict[str, Any]:
    con = store.con
    fsr.stage_metric_legs(con, [
        fsr.MetricLeg(f.metric_code, f.metric_window, hashes[(f.metric_code, f.metric_window)],
                      spec["max_age_days"], spec["annual_max_age_days"],
                      f.metric_code == "eps_diluted_q_growth_yoy")
        for f in batch.features
    ], table="_rp_metrics")
    fsr.stage_selected_states(
        con, run_id=run_id, all_hashes=hashes, calendar_table="_rp_calendar",
        cohort_table="_fs_cohort_cal", metrics_table="_rp_metrics",
        proof_table="research_panel_proofs", metric_source=DERIVED_SOURCE_NAME,
        max_selected_states=100_000 * len(batch.features),
        max_prior_states=100_000 * len(batch.features),
    )
    # A stale state keeps its value only when every later rule of the cascade
    # (input clock, expiry) also holds; any other rejection publishes NULL.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_batch_values AS
        SELECT l.decision_date AS formation_date, l.security_id, f.feature_id,
               l.metric_code, l.metric_window,
               CAST(CASE WHEN l.reason='valid' THEN l.raw_value
                         WHEN l.reason='stale_current_anchor'
                              AND l.latest_input_clock<=l.cutoff
                              AND l.latest_input_clock<=l.available_at
                              AND (l.state_valid_to IS NULL OR l.state_valid_to>l.cutoff)
                         THEN l.raw_value END AS DOUBLE) AS raw_value,
               l.reason, l.lineage_status, l.available_at, l.latest_input_clock,
               l.period_end, l.fiscal_period_start, l.fiscal_period_end, l.value_origin,
               CAST(l.age_days AS INTEGER) AS age_days, CAST(l.max_age_days AS INTEGER) AS max_age_days,
               l.cik AS owner_cik, l.derived_value_id, l.derived_owner_security_id,
               l.lineage_digest, c.identity_basis, ? AS universe_basis, ? AS availability_basis
        FROM _fs_leg l
        JOIN _rp_features f ON f.metric_code=l.metric_code AND f.metric_window=l.metric_window
        JOIN _fs_cohort_cal c ON c.decision_date=l.decision_date AND c.security_id=l.security_id
        WHERE l.cohort_reason='valid' AND l.derived_value_id IS NOT NULL
    """, [spec["universe_id"], FUNDAMENTAL_AVAILABILITY_BASIS])
    reason_rows = con.execute("""
        SELECT metric_code, metric_window, reason, count(*) FROM _fs_leg
        WHERE cohort_reason='valid' GROUP BY ALL ORDER BY ALL
    """).fetchall()
    unmatched = {(code, window): int(n) for code, window, n in con.execute("""
        SELECT metric_code, metric_window, count(*) FROM _fs_owner_state
        WHERE association_cik IS NULL GROUP BY ALL
    """).fetchall()}
    by_metric: dict[tuple[str, str], dict[str, int]] = {}
    for code, window, reason, n in reason_rows:
        by_metric.setdefault((code, window), {})[reason] = int(n)
    return {"reasons": by_metric, "unmatched": unmatched}


def _market_batch(store: ResearchStore, batch: _Batch, spec: dict[str, Any], market_source: str) -> dict[str, Any]:
    con = store.con
    columns = [f.metric_code for f in batch.features]
    picks = ",\n".join(f'arg_max(m."{code}", (m.available_at, m.market_daily_id)) AS "{code}"' for code in columns)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_market AS
        SELECT c.decision_date, c.cutoff, m.security_id,
               max(m.available_at) AS available_at,
               arg_max(m.fundamental_available_at, (m.available_at, m.market_daily_id)) AS fundamental_available_at,
               {picks}
        FROM market_daily_metrics m
        JOIN _rp_calendar c ON m.trade_date=c.decision_date AND m.available_at<=c.cutoff
                           AND m.as_of_date<=c.decision_date
        JOIN _fs_cohort_cal k ON k.decision_date=c.decision_date AND k.security_id=m.security_id
                             AND k.cohort_reason='valid'
        WHERE m.source=?
        GROUP BY c.decision_date, c.cutoff, m.security_id
    """, [market_source])
    unions = "\nUNION ALL\n".join(
        f"SELECT decision_date, cutoff, security_id, {_sql_text(f.feature_id)} AS feature_id, "
        f"{_sql_text(f.metric_code)} AS metric_code, CAST(\"{f.metric_code}\" AS DOUBLE) AS value, "
        f"available_at, fundamental_available_at FROM _rp_market"
        for f in batch.features
    )
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_batch_values AS
        WITH long AS ({unions}), judged AS (
          SELECT *, CASE WHEN available_at>cutoff
                              OR coalesce(fundamental_available_at>cutoff, false)
                         THEN 'invalid_input_clock'
                         WHEN value IS NULL OR NOT isfinite(value) THEN 'invalid_current_state'
                         ELSE 'valid' END AS reason
          FROM long
        )
        SELECT j.decision_date AS formation_date, j.security_id, j.feature_id, j.metric_code,
               '{MARKET_WINDOW}' AS metric_window,
               CAST(CASE WHEN j.reason='valid' THEN j.value END AS DOUBLE) AS raw_value,
               j.reason, CAST(NULL AS VARCHAR) AS lineage_status, j.available_at,
               j.available_at AS latest_input_clock, CAST(NULL AS DATE) AS period_end,
               CAST(NULL AS DATE) AS fiscal_period_start, CAST(NULL AS DATE) AS fiscal_period_end,
               'market_daily' AS value_origin, CAST(0 AS INTEGER) AS age_days,
               CAST(NULL AS INTEGER) AS max_age_days, k.cik AS owner_cik,
               CAST(NULL AS VARCHAR) AS derived_value_id,
               CAST(NULL AS VARCHAR) AS derived_owner_security_id,
               CAST(NULL AS VARCHAR) AS lineage_digest, k.identity_basis,
               ? AS universe_basis, ? AS availability_basis
        FROM judged j JOIN _fs_cohort_cal k
          ON k.decision_date=j.decision_date AND k.security_id=j.security_id
    """, [spec["universe_id"], MARKET_AVAILABILITY_BASIS])
    valid_members = int(con.execute(
        "SELECT count(*) FROM _fs_cohort_cal WHERE cohort_reason='valid'").fetchone()[0])
    by_metric: dict[tuple[str, str], dict[str, int]] = {}
    for code, reason, n in con.execute(
            "SELECT metric_code, reason, count(*) FROM _rp_batch_values GROUP BY ALL ORDER BY ALL").fetchall():
        by_metric.setdefault((code, MARKET_WINDOW), {})[reason] = int(n)
    for f in batch.features:
        counts = by_metric.setdefault((f.metric_code, MARKET_WINDOW), {})
        missing = valid_members - sum(counts.values())
        if missing:
            counts["missing_market_row"] = missing
    return {"reasons": by_metric, "unmatched": {}}


def _write_batch(store: ResearchStore, run_id: str, row: CalendarRow, batch: _Batch,
                 result: dict[str, Any], valid_members: int) -> None:
    con = store.con
    leaks = con.execute("""
        SELECT count(*) FROM _rp_batch_values v JOIN _rp_calendar c ON c.decision_date=v.formation_date
        WHERE v.available_at>c.cutoff
           OR ((v.reason='valid' OR v.raw_value IS NOT NULL) AND
               (v.latest_input_clock IS NULL OR v.latest_input_clock>c.cutoff))
           OR (v.raw_value IS NOT NULL AND v.reason NOT IN ({reasons}))
    """.format(reasons=",".join(_sql_text(reason) for reason in VALUE_BEARING_REASONS))).fetchone()[0]
    if leaks:
        raise RuntimeError(f"{leaks} staged values are not visible at the formation cutoff")
    digests = dict(con.execute(_VALUE_DIGEST_SQL.format(relation="_rp_batch_values", where="")).fetchall())
    emitted = dict(con.execute("SELECT feature_id, count(*) FROM _rp_batch_values GROUP BY 1").fetchall())
    con.execute(f"INSERT INTO research_panel_values (run_id,{','.join(_VALUE_COLUMNS)}) "
                f"SELECT ?,{','.join(_VALUE_COLUMNS)} FROM _rp_batch_values", [run_id])
    coverage = []
    for feature in batch.features:
        reasons = result["reasons"].get((feature.metric_code, feature.metric_window), {})
        values_valid = int(reasons.get("valid", 0))
        selected = sum(n for reason, n in reasons.items()
                       if reason not in ("missing_metric_state", "missing_market_row"))
        coverage.append([
            run_id, row.formation_date, feature.feature_id, feature.metric_code, feature.metric_window,
            batch.ordinal, "formed" if values_valid else "no_valid_values", valid_members, selected,
            int(emitted.get(feature.feature_id, 0)), values_valid,
            int(result["unmatched"].get((feature.metric_code, feature.metric_window), 0)),
            _canonical(dict(sorted(reasons.items()))), digests.get(feature.feature_id, _EMPTY_SHA),
        ])
    _insert_coverage(con, coverage)


def _insert_coverage(con: Any, rows: list[list[Any]]) -> None:
    con.executemany("""
        INSERT INTO research_panel_coverage
        (run_id,formation_date,feature_id,metric_code,metric_window,batch_ordinal,status,
         valid_members,selected_states,values_emitted,values_valid,unmatched_owner_states,
         reasons_json,values_sha256)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    """, rows)


def _panel_digest(con: Any, run_id: str, manifest: Sequence[Any]) -> str:
    calendar_sha = _stream_digest(con, """
        SELECT month_start,expected_session,last_observed_session,formation_date,cutoff,
               entry_date,status,visible_members,eligible_members,valid_members,
               cohort_reasons_json,cohort_sha256
        FROM research_panel_calendar WHERE run_id=? ORDER BY month_start
    """, [run_id])
    coverage_sha = _stream_digest(con, """
        SELECT formation_date,feature_id,metric_code,metric_window,status,valid_members,
               selected_states,values_emitted,values_valid,unmatched_owner_states,
               reasons_json,values_sha256
        FROM research_panel_coverage WHERE run_id=? ORDER BY formation_date,feature_id
    """, [run_id])
    return _sha(_canonical([*manifest, calendar_sha, coverage_sha]))


def build_research_panel(store: ResearchStore, options: ResearchPanelOptions, *,
                         owner_links: Sequence[Any] | None = None) -> ResearchPanelResult:
    """Build (or resume) one monthly research panel run in the research store.

    ``owner_links`` overrides the reconstructed bridge (A5 ``BridgeRow``-shaped
    rows); by default it is built from the attached warehouse. It is ignored for
    the strict basis.
    """
    run_at, features, spec = _validate(options)
    con = store.con
    basis = options.basis
    labels = _basis_labels(basis)
    _require_inputs(store, basis, features, build_bridge=owner_links is None)
    batches = _batches(features, options.metric_batch_size)
    definitions_json, definitions_sha, hashes = _definitions(con, features)
    spec_json = _canonical(spec)
    spec_sha = _sha(spec_json)
    code_sha = _code_sha()
    session_source = MARKET_DAILY_SOURCE_NAME
    sessions = observed_sessions(con, market_source=session_source,
                                 first_day=dt.date(options.start_month.year, options.start_month.month, 1),
                                 as_of_date=options.as_of_date, run_at=run_at)
    calendar_rows = month_end_calendar(sessions, start_month=options.start_month, end_month=options.end_month,
                                       as_of_date=options.as_of_date, run_at=run_at)
    calendar_sha = _sha(_canonical([row.canonical() for row in calendar_rows]))
    source_ids = {
        "universe_id": labels["universe_id"], "universe_source": UNIVERSE_SOURCE_NAME,
        "identity_basis": labels["identity_basis"], "metric_source": DERIVED_SOURCE_NAME,
        "market_source": labels["market_source"], "session_source": session_source,
        "fundamental_clock_policy": FUNDAMENTAL_CLOCK_POLICY,
        "owner_links": ("market_owner_bridge:reconstructed" if basis == BASIS_RECONSTRUCTED
                        else "security_identifier_history:dated"),
    }
    source_json = _canonical(source_ids)
    bridge_json = None
    if basis == BASIS_RECONSTRUCTED:
        if owner_links is None:
            rows, summary = _owner_bridge_rows(store)
        else:
            rows, summary = tuple(owner_links), None
        staged = stage_owner_links(con, rows)
        bridge_json = _canonical({"staged": staged, "bridge_summary": summary})
    fsr.stage_calendar(con, [], table="_rp_calendar")
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_features "
                "(feature_id VARCHAR, metric_code VARCHAR, metric_window VARCHAR)")
    con.executemany("INSERT INTO _rp_features VALUES (?,?,?)",
                    [[f.feature_id, f.metric_code, f.metric_window] for f in features])
    now = dt.datetime.now(dt.UTC).replace(tzinfo=None)
    existing = con.execute("""
        SELECT status,spec_sha256,definitions_sha256,code_sha256,calendar_sha256,source_ids_json,
               owner_bridge_json
        FROM research_panel_runs WHERE run_id=?
    """, [options.run_id]).fetchone()
    with store.transaction():
        if existing is not None:
            if not options.resume:
                raise ValueError(f"run_id already exists: {options.run_id}")
            if existing[0] not in ("building", "failed"):
                raise ValueError(f"run {options.run_id} is sealed ({existing[0]}); it cannot be resumed")
            if tuple(existing[1:]) != (spec_sha, definitions_sha, code_sha, calendar_sha, source_json,
                                       bridge_json):
                raise ValueError("resume refused: spec, definitions, code, calendar, sources or bridge changed")
            con.execute("UPDATE research_panel_runs SET status='building' WHERE run_id=?", [options.run_id])
        else:
            con.execute("""
                INSERT INTO research_panel_runs
                (run_id,status,basis,universe_id,identity_basis,universe_basis,
                 fundamental_availability_basis,market_availability_basis,query_version,spec_json,
                 spec_sha256,definitions_json,definitions_sha256,code_sha256,source_ids_json,
                 calendar_sha256,start_month,end_month,as_of_date,run_at,warehouse_path,
                 owner_bridge_json,blockers_json,created_at)
                VALUES (?,'building',?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """, [options.run_id, basis, labels["universe_id"], labels["identity_basis"],
                  labels["universe_id"], FUNDAMENTAL_AVAILABILITY_BASIS, MARKET_AVAILABILITY_BASIS,
                  QUERY_VERSION, spec_json, spec_sha, definitions_json, definitions_sha, code_sha,
                  source_json, calendar_sha, options.start_month, options.end_month, options.as_of_date,
                  run_at, str(store.warehouse_path), bridge_json, _canonical(list(_BASE_BLOCKERS)), now])
            con.executemany("""
                INSERT INTO research_panel_calendar
                (run_id,month_start,expected_session,last_observed_session,formation_date,cutoff,
                 entry_date,status)
                VALUES (?,?,?,?,?,?,?,?)
            """, [[options.run_id, row.month_start, row.expected_session, row.last_observed_session,
                   row.formation_date, row.cutoff, row.entry_date, row.status] for row in calendar_rows])
    try:
        done_cohorts = dict(con.execute("""
            SELECT formation_date, cohort_sha256 FROM research_panel_calendar
            WHERE run_id=? AND cohort_sha256 IS NOT NULL
        """, [options.run_id]).fetchall())
        done_features = {(row[0], row[1]) for row in con.execute(
            "SELECT formation_date, feature_id FROM research_panel_coverage WHERE run_id=?",
            [options.run_id]).fetchall()}
        for row in calendar_rows:
            if row.status != CALENDAR_FORMED:
                continue
            assert row.formation_date is not None and row.cutoff is not None
            fsr.stage_calendar(con, [(row.formation_date, row.cutoff)], table="_rp_calendar")
            with store.transaction():
                cohort = _stage_formation_cohort(store, row, spec)
                stored = done_cohorts.get(row.formation_date)
                if stored is not None and stored != cohort["digest"]:
                    raise RuntimeError(f"cohort for {row.formation_date} changed since the interrupted run")
                if stored is None:
                    con.execute(f"INSERT INTO research_panel_cohort (run_id,formation_date,"
                                f"{','.join(_COHORT_COLUMNS)}) SELECT ?,formation_date,"
                                f"{','.join(_COHORT_COLUMNS)} FROM _rp_cohort_rows", [options.run_id])
                    con.execute("""
                        UPDATE research_panel_calendar
                        SET visible_members=?,eligible_members=?,valid_members=?,
                            cohort_reasons_json=?,cohort_sha256=?
                        WHERE run_id=? AND month_start=?
                    """, [cohort["visible"], cohort["eligible"], cohort["valid"],
                          cohort["reasons_json"], cohort["digest"], options.run_id, row.month_start])
            for batch in batches:
                if all((row.formation_date, f.feature_id) in done_features for f in batch.features):
                    continue
                with store.transaction():
                    if cohort["valid"] == 0:
                        _insert_coverage(con, [[
                            options.run_id, row.formation_date, f.feature_id, f.metric_code,
                            f.metric_window, batch.ordinal, "empty_common_cohort", 0, 0, 0, 0, 0,
                            _canonical({}), _EMPTY_SHA] for f in batch.features])
                        continue
                    if batch.is_market:
                        result = _market_batch(store, batch, spec, labels["market_source"])
                    else:
                        result = _derived_batch(store, options.run_id, batch, spec, hashes)
                    _write_batch(store, options.run_id, row, batch, result, cohort["valid"])
        return _finalize(store, options.run_id, basis, calendar_rows,
                         [QUERY_VERSION, spec_sha, definitions_sha, code_sha, calendar_sha,
                          source_json, bridge_json])
    except Exception as exc:
        with store.transaction():
            con.execute("""
                UPDATE research_panel_runs SET status='failed',diagnostic_json=?
                WHERE run_id=? AND status='building'
            """, [_canonical({"error": type(exc).__name__, "message": str(exc)}), options.run_id])
        raise


def _finalize(store: ResearchStore, run_id: str, basis: str, calendar_rows: Sequence[CalendarRow],
              manifest: Sequence[Any]) -> ResearchPanelResult:
    con = store.con
    formed = [row for row in calendar_rows if row.status == CALENDAR_FORMED]
    value_rows, valid_values = con.execute("""
        SELECT count(*), count(*) FILTER (WHERE reason='valid')
        FROM research_panel_values WHERE run_id=?
    """, [run_id]).fetchone()
    cohort_rows, valid_members = con.execute("""
        SELECT count(*), count(*) FILTER (WHERE cohort_reason='valid')
        FROM research_panel_cohort WHERE run_id=?
    """, [run_id]).fetchone()
    coverage_rows = con.execute("SELECT count(*) FROM research_panel_coverage WHERE run_id=?",
                                [run_id]).fetchone()[0]
    feature_count = len(json.loads(con.execute(
        "SELECT spec_json FROM research_panel_runs WHERE run_id=?", [run_id]).fetchone()[0])["features"])
    if coverage_rows != len(formed) * feature_count:
        raise RuntimeError("coverage does not contain every formed month-end and feature")
    status_counts: dict[str, int] = {}
    for row in calendar_rows:
        status_counts[row.status] = status_counts.get(row.status, 0) + 1
    proofs = con.execute("SELECT count(*) FROM research_panel_proofs WHERE run_id=?", [run_id]).fetchone()[0]
    diagnostic = {"months": len(calendar_rows), "formations": len(formed),
                  "calendar_status": dict(sorted(status_counts.items())),
                  "cohort_rows": int(cohort_rows), "valid_cohort_rows": int(valid_members),
                  "coverage_rows": int(coverage_rows), "value_rows": int(value_rows),
                  "valid_values": int(valid_values), "lineage_proofs": int(proofs)}
    blockers = _blockers(basis, len(formed), int(valid_members), int(valid_values), calendar_rows)
    if basis == BASIS_STRICT and not valid_members:
        status = "untestable_strict"
    elif not valid_values:
        status = "blocked_empty"
    else:
        status = "complete"
    panel_sha = _panel_digest(con, run_id, manifest)
    with store.transaction():
        con.execute("""
            UPDATE research_panel_runs
            SET status=?,diagnostic_json=?,blockers_json=?,panel_sha256=?,finished_at=?
            WHERE run_id=? AND status='building'
        """, [status, _canonical(diagnostic), _canonical(list(blockers)), panel_sha,
              dt.datetime.now(dt.UTC).replace(tzinfo=None), run_id])
    return ResearchPanelResult(run_id, status, len(calendar_rows), len(formed), int(value_rows),
                               int(valid_values), panel_sha, blockers)


# ---------------------------------------------------------------------------
# Validator
# ---------------------------------------------------------------------------

def validate_research_panel(store: ResearchStore, run_id: str) -> ResearchPanelValidation:
    """Reject unsealed, changed or point-in-time-unsafe panels before any use."""
    con = store.con
    row = con.execute("""
        SELECT status,basis,query_version,spec_json,spec_sha256,definitions_json,definitions_sha256,
               code_sha256,calendar_sha256,source_ids_json,owner_bridge_json,panel_sha256
        FROM research_panel_runs WHERE run_id=?
    """, [run_id]).fetchone()
    if row is None or row[0] not in SEALED_STATUSES:
        raise ValueError("research panel run is absent or not sealed")
    (status, basis, query_version, spec_json, spec_sha, definitions_json, definitions_sha,
     code_sha, calendar_sha, source_json, bridge_json, panel_sha) = row
    if query_version != QUERY_VERSION:
        raise ValueError("unsupported research panel query version")
    if (_sha(spec_json) != spec_sha or _sha(definitions_json) != definitions_sha
            or not all(_SHA.fullmatch(str(v)) for v in (code_sha, calendar_sha, panel_sha))):
        raise ValueError("research panel manifest hash mismatch")
    spec = json.loads(spec_json)
    features = tuple(item[0] for item in spec["features"])
    calendar = con.execute("""
        SELECT month_start,expected_session,last_observed_session,formation_date,cutoff,entry_date,
               status,cohort_sha256
        FROM research_panel_calendar WHERE run_id=? ORDER BY month_start
    """, [run_id]).fetchall()
    rebuilt = _sha(_canonical([[_iso(v) for v in item[:7]] for item in calendar]))
    if rebuilt != calendar_sha:
        raise ValueError("research panel calendar digest mismatch")
    formed = [item for item in calendar if item[6] == CALENDAR_FORMED]
    for item in formed:
        if item[3] != item[1] or item[4] != dt.datetime.combine(item[1], dt.time(DECISION_HOUR)) \
                or item[5] is None or item[5] <= item[3]:
            raise ValueError("research panel formation calendar contract changed")
        digest = con.execute(_COHORT_DIGEST_SQL.format(
            relation="research_panel_cohort", where="WHERE run_id=? AND formation_date=?"),
            [run_id, item[3]]).fetchone()[0]
        if digest != item[7]:
            raise ValueError(f"research panel cohort digest mismatch at {item[3]}")
        stored = dict(con.execute("""
            SELECT feature_id, values_sha256 FROM research_panel_coverage
            WHERE run_id=? AND formation_date=?
        """, [run_id, item[3]]).fetchall())
        if set(stored) != set(features):
            raise ValueError(f"research panel coverage incomplete at {item[3]}")
        # Bounded like the build: at most 16 features' rows per ordered aggregate.
        rebuilt_values: dict[str, str] = {}
        for start in range(0, len(features), 16):
            chunk = features[start:start + 16]
            rebuilt_values.update(con.execute(_VALUE_DIGEST_SQL.format(
                relation="research_panel_values",
                where=f"WHERE run_id=? AND formation_date=? AND feature_id IN ({','.join('?' * len(chunk))})"),
                [run_id, item[3], *chunk]).fetchall())
        stray = con.execute("""
            SELECT count(*) FROM research_panel_values WHERE run_id=? AND formation_date=?
        """, [run_id, item[3]]).fetchone()[0] - con.execute("""
            SELECT coalesce(sum(values_emitted),0) FROM research_panel_coverage
            WHERE run_id=? AND formation_date=?
        """, [run_id, item[3]]).fetchone()[0]
        if stray or any(rebuilt_values.get(feature, _EMPTY_SHA) != stored[feature] for feature in features):
            raise ValueError(f"research panel value digest mismatch at {item[3]}")
    leaks, orphans, duplicates = con.execute("""
        SELECT count(*) FILTER (WHERE c.formation_date IS NOT NULL AND (v.available_at>c.cutoff
                  OR ((v.reason='valid' OR v.raw_value IS NOT NULL)
                      AND (v.latest_input_clock IS NULL OR v.latest_input_clock>c.cutoff))
                  OR (v.raw_value IS NOT NULL AND v.reason NOT IN ({reasons})))),
               count(*) FILTER (WHERE c.formation_date IS NULL),
               count(*) - count(DISTINCT (v.formation_date, v.feature_id, v.security_id))
        FROM research_panel_values v LEFT JOIN research_panel_calendar c
          ON c.run_id=v.run_id AND c.formation_date=v.formation_date AND c.status='formed'
        WHERE v.run_id=?
    """.format(reasons=",".join(_sql_text(reason) for reason in VALUE_BEARING_REASONS)),
        [run_id]).fetchone()
    if leaks or orphans or duplicates:
        raise ValueError("research panel point-in-time or grain contract violated")
    manifest = [QUERY_VERSION, spec_sha, definitions_sha, code_sha, calendar_sha, source_json, bridge_json]
    if _panel_digest(con, run_id, manifest) != panel_sha:
        raise ValueError("research panel digest mismatch")
    value_rows, valid_values = con.execute("""
        SELECT count(*), count(*) FILTER (WHERE reason='valid') FROM research_panel_values WHERE run_id=?
    """, [run_id]).fetchone()
    return ResearchPanelValidation(run_id, status, basis, panel_sha, tuple(item[3] for item in formed),
                                   features, int(value_rows), int(valid_values))


__all__ = [
    "BASES",
    "BASIS_RECONSTRUCTED",
    "BASIS_STRICT",
    "CALENDAR_AFTER_CUTOFF",
    "CALENDAR_FORMED",
    "CALENDAR_MISSING_MONTH_END",
    "CALENDAR_MISSING_NEXT_SESSION",
    "CALENDAR_RULE_CONFLICT",
    "FUNDAMENTAL_AVAILABILITY_BASIS",
    "MARKET_AVAILABILITY_BASIS",
    "QUERY_VERSION",
    "CalendarRow",
    "PanelFeature",
    "ResearchPanelOptions",
    "ResearchPanelResult",
    "ResearchPanelValidation",
    "build_research_panel",
    "canonical_features",
    "default_panel_features",
    "expected_month_end_session",
    "month_end_calendar",
    "nyse_full_day_closures",
    "observed_sessions",
    "stage_owner_links",
    "validate_research_panel",
]
