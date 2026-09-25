"""Monthly point-in-time research panel (task R2a; fix round 1).

One formation per closed calendar month: the month's last NYSE session, when
it was observed. The decision cutoff is that session at 22:00 UTC and entry is
the next observed session. A month whose last session is missing is recorded as
``missing_month_end`` and never replaced by an earlier (mid-month) session.

Bases (rulings RX1/RX6):

``reconstructed``
    ``us_listed_reconstructed_v1`` membership and the A5 reconstructed
    price-line -> owner bridge (current SEC ticker map backcast, modeled clock).
    Rows are labeled ``identity_basis`` / ``universe_basis`` /
    ``availability_basis`` and are research input, never certifiable history.
``strict``
    ``us_listed_v1`` membership with CIKs from dated identifier history only. It
    is always attempted; with no valid cohort anywhere the run is sealed as
    ``untestable_strict``.

Grain and survivorship. The value panel is dense over the *eligible* members
of each formation (eligible type and venue, or the retained delisted tail):
exactly one row per (formation, eligible member, feature), so every coverage
row's ``reasons_json`` sums to its ``eligible_members``. Features have a scope:

``price_line``
    Identity-free market features (returns, momentum, volatility, dollar
    volume). They need no owner link and are published for every eligible
    member, linked or not, labeled ``identity_basis='price_line'``. The
    delisted tail that the owner bridge cannot link (``no_current_ticker``)
    therefore stays in every market feature and control.

Size (interim, R2a fix round 2). Vendor share counts (``equity_daily_bars``
and market_daily's ``archive*``/``class_sum`` share sources) are in vendor
units (thousands) and dated from the cover as-of date, before the filing is
public. Until the filing-matched share relation (A8) lands (follow-up R2d):
``line_market_cap`` (own close x own vendor count) is not a default feature
and is computed only with ``unverified_vendor_shares=True`` (rows labeled
``availability_basis='vendor_shares_bar_clock_unverified'``); every size row
carries ``shares_source`` and ``size_status`` (``verified_dei_shares`` only
for DEI counts, else ``unverified_vendor_shares``), and the spec's
``size_policy`` tells R2b to use only verified size.
``owner``
    Fundamental (derived) features and issuer-share market features
    (``market_cap``, valuation ratios). They attach to exactly one line per
    issuer per formation: the owner-linked (``cohort_reason='valid'``) line with
    the highest trailing 30-day dollar volume (``primary_line_rule`` labels how
    the primary was chosen; ties and missing volume fall back to the smallest
    ``security_id``). Every other eligible member has a NULL row whose reason
    is its cohort exclusion (``missing_owner_link``, ``ambiguous_owner_link``,
    ...) or ``secondary_issuer_line``.

The share of eligible members lacking an owner link is recorded per formation
(``research_panel_calendar.owner_unlinked_members`` / ``owner_link_attrition``)
and as the counted run blocker ``owner_link_attrition:<unlinked>/<eligible>``.

Selection and lineage. Per derived metric batch the warehouse is scanned once
for all formations: the selected state per (owner, metric, formation) is the
FQ1 rule (bucket-latest, then latest period, visible at the cutoff) computed as
a running arg-max over the formation calendar, which equals the FQ1 ranking
whenever each bucket has one period end and each period end one bucket; any
(owner, metric) history that breaks that falls back to the FQ1 ranking itself.
Lineage is proved once per selected ``derived_value_id`` per run by the
set-based prover (:mod:`atx_db.research.lineage`, provably equal to the
Python resolver) into the slim ``research_lineage_proofs`` table, in committed
chunks, so an interrupted run resumes after the last proved chunk. Owners whose
identifier names a CIK that no owner-linked member has at any formation cannot
match a member (their leaves' CIK would have to differ from their owner CIK)
and are not proved; the count is reported. There is no state-count abort:
selection and prior-owner history sizes are counted in ``diagnostic_json``.

Market revisions (R2e). A market feature is the newest ``market_daily_metrics``
revision of the formation session visible at the cutoff, column by column, even
when that revision withholds the value (NULL: ``split_unresolved``, a DEI
conflict, the currency guard). The row is then ``invalid_current_state``. An
older revision's value is never revived under the newer revision's clock:
DuckDB's ``arg_max`` skips NULL arguments, so every pick uses ``arg_max_null``.
The same rule applies to ``shares_source``, the fundamental clock, the trailing
dollar volume of the primary-line rule and turnover's share count. Bars are
different: the rows of one bar session are vendors, not revisions, and are
deduped exactly as market_daily dedupes them (each column from the newest vendor
row that has it).

Price/liquidity natives (P2). Identity-free (``price_line``) for every eligible
line: ``amihud_illiquidity_21d``, ``pct_from_high_252d``, ``max_daily_return_21d``
and ``downside_deviation_60d``. ``turnover_21d`` is owner-scoped (volume over the
issuer's verified DEI share count; ``unverified_shares`` with NULL otherwise, and
``split_in_price_window`` when a split falls inside its 21 bars).
They are computed from the line's own bars in the 400 days before the session,
with row windows as ``equity_price_metrics`` defines them; only bars whose clock
is at or before the cutoff are read. Where ``equity_price_metrics`` holds a
feature's column with values, the run reads that column instead (one source per
feature per run, recorded in the definitions). A window without the bars it needs
is ``incomplete_price_window``.

One formation (public). :func:`stage_formation` and :func:`formation_batches`
run this builder for a single (session, cutoff) and yield per-batch values and
digests equal to a panel run's for that formation (the P7 cross-section screen).

``raw_value`` is kept only for ``valid`` and ``stale_current_anchor`` rows (both
point-in-time safe); every other reason carries NULL. Consumers read
``reason='valid'`` (stale anchors carry a value for diagnostics only), and only
runs whose status is ``complete`` (or ``untestable_strict`` / ``blocked_empty``
as evidence) after :func:`validate_research_panel`.
"""

from __future__ import annotations

import calendar as _calendar
import datetime as dt
import hashlib
import itertools
import json
import math
import re
import time
from collections.abc import Iterable, Iterator, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .. import derived_lineage as _derived_lineage
from .. import fundamental_signal_research as fsr
from .. import market_owner_bridge as _market_owner_bridge
from .._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from ..derived_registry import DERIVED_SOURCE_NAME
from ..market_daily import MARKET_DAILY_SOURCE_NAME, MARKET_DAILY_STRICT_SOURCE_NAME
from ..universe_us_listed import UNIVERSE_SOURCE_NAME
from . import catalog as _catalog
from . import lineage as _lineage
from . import store as _store
from .store import ResearchStore

# v4 (R2e): the newest visible market revision wins even when NULL. Pre-v4 runs can
# carry an older revision's value under a newer clock; the validator refuses them.
# v5 (P2): price/liquidity natives; line_market_cap reads bars with market_daily's
# vendor rule (R2e m2). Existing features' value digests are unchanged.
QUERY_VERSION = "research-monthly-pit-panel-v5"
MARKET_REVISION_RULE = ("newest market_daily revision visible at the cutoff wins per column, "
                        "a NULL included (arg_max_null)")
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

SCOPE_OWNER = "owner"
SCOPE_PRICE_LINE = "price_line"
PRICE_LINE_IDENTITY_BASIS = "price_line"
SECONDARY_LINE_REASON = "secondary_issuer_line"
PRIMARY_RULE_SINGLE = "single_line"
PRIMARY_RULE_DOLLAR_VOLUME = "max_trailing_dollar_volume_30d"
PRIMARY_RULE_TIEBREAK = "security_id_tiebreak"
TRAILING_DOLLAR_VOLUME_DAYS = 30
# Cohort reasons that mean "eligible, but no usable owner link".
OWNER_LINK_FAILURES = ("missing_owner_link", "invalid_owner_link_cik", "ambiguous_owner_link",
                       "missing_dated_cik", "invalid_dated_cik", "ambiguous_dated_cik")
# market_daily inputs that carry issuer-level (owner) share counts.
OWNER_MARKET_INPUTS = frozenset({"shares_outstanding", "dei_shares"})
OWNER_MARKET_CODES = frozenset({"market_cap"})
# Size verification (interim until the A8 filing-matched share relation, R2d).
UNVERIFIED_VENDOR_SHARES = "unverified_vendor_shares"
VENDOR_SHARES_AVAILABILITY_BASIS = "vendor_shares_bar_clock_unverified"
VERIFIED_SHARES_SOURCES = ("dei",)
SIZE_VERIFIED = "verified_dei_shares"
LINE_SHARES_SOURCE = "equity_daily_bars_vendor"
# P2 price/liquidity natives: where they read from, and their reasons.
BARS_SOURCE = "equity_daily_bars"
PRICE_METRICS_SOURCE = "equity_price_metrics"
#: ``equity_price_metrics.DEFAULT_SOURCE`` (not imported: that module loads the as-of layer).
PRICE_METRICS_SOURCE_NAME = "derived_equity_price_metrics_v1"
#: Calendar days of bars read per formation: 252 sessions plus holidays, with slack.
PRICE_LOOKBACK_DAYS = 400
PRICE_WINDOW_BARS = 252
AMIHUD_SCALE = 1e9
INCOMPLETE_WINDOW_REASON = "incomplete_price_window"
UNVERIFIED_SHARES_REASON = "unverified_shares"
#: Turnover divides 21 bars of volume by today's share count: a split inside the window
#: mixes share bases. The back-adjustment factor close/adj of the window's bars then
#: moves by the split ratio (a 5:4 split or more); ordinary dividends move it by about
#: the yield. Such a window is NULL with this reason.
SPLIT_WINDOW_REASON = "split_in_price_window"
SPLIT_RATIO_LIMIT = 1.25
#: P11 hook (not assigned yet): an owner whose statements are IFRS (20-F/40-F) and not
#: standardized will carry this reason on owner (derived) features instead of
#: ``missing_metric_state``. See ``_derived_formations``.
IFRS_REPORTER_REASON = "ifrs_reporter_not_standardized"
# Panel-native features (not warehouse metrics). ``scope``: price_line (every
# eligible line) or owner (the issuer's primary line); ``size``: the value reads a
# share count, so rows carry ``shares_source``/``size_status``; ``requires`` names
# the option that must be set to compute one. P2 natives use the line's own bars
# (``window_bars``: the bars the window needs), or the ``equity_price_metrics``
# column of the same definition when the warehouse has it with values.
NATIVE_FEATURES: dict[str, dict[str, Any]] = {
    "line_market_cap": {
        "metric_window": MARKET_WINDOW,
        "expression": "close * shares_outstanding",
        "inputs": ["equity_daily_bars.close", "equity_daily_bars.shares_outstanding"],
        "version": "1",
        "source_column": "equity_daily_bars.close*equity_daily_bars.shares_outstanding",
        "unit_basis": "vendor_units_unverified",
        "requires": UNVERIFIED_VENDOR_SHARES,
        "scope": SCOPE_PRICE_LINE,
        "size": True,
    },
    "amihud_illiquidity_21d": {
        "metric_window": MARKET_WINDOW,
        "expression": "1e9 * mean(|r_t| / (close_t * volume_t)) over the last 21 bars; all 21 required",
        "inputs": ["equity_daily_bars.adjusted_close", "equity_daily_bars.close", "equity_daily_bars.volume"],
        "version": "1",
        "unit_basis": "abs_return_per_1e9_dollars_traded",
        "reference": "Amihud (2002)",
        "scope": SCOPE_PRICE_LINE,
        "size": False,
        "window_bars": 22,
        "price_metrics_column": "amihud_illiquidity_21d",
    },
    "pct_from_high_252d": {
        "metric_window": MARKET_WINDOW,
        "expression": "adj_t / max(adj over the last 252 bars) - 1",
        "inputs": ["equity_daily_bars.adjusted_close"],
        "version": "1",
        "unit_basis": "fraction",
        "reference": "George and Hwang (2004)",
        "scope": SCOPE_PRICE_LINE,
        "size": False,
        "window_bars": 1,
        "price_metrics_column": "pct_from_high_252d",
    },
    "max_daily_return_21d": {
        "metric_window": MARKET_WINDOW,
        "expression": "max(r_t) over the last 21 bars; all 21 returns required",
        "inputs": ["equity_daily_bars.adjusted_close"],
        "version": "1",
        "unit_basis": "fraction",
        "reference": "Bali, Cakici and Whitelaw (2011)",
        "scope": SCOPE_PRICE_LINE,
        "size": False,
        "window_bars": 22,
    },
    "downside_deviation_60d": {
        "metric_window": MARKET_WINDOW,
        "expression": "sqrt(252 * mean(min(r_t, 0)^2)) over the last 60 bars; all 60 returns required",
        "inputs": ["equity_daily_bars.adjusted_close"],
        "version": "1",
        "unit_basis": "annualized_fraction",
        "reference": "Ang, Chen and Xing (2006)",
        "scope": SCOPE_PRICE_LINE,
        "size": False,
        "window_bars": 61,
        "price_metrics_column": "downside_deviation_60d",
    },
    "turnover_21d": {
        "metric_window": MARKET_WINDOW,
        "expression": "mean(volume over the last 21 bars) / verified (DEI) shares outstanding; all 21 required; "
                      "NULL when close/adj moves by more than 25% in the window (a split)",
        "inputs": ["equity_daily_bars.volume", "market_daily_metrics.shares_outstanding",
                   "market_daily_metrics.shares_source"],
        "version": "1",
        "unit_basis": "fraction_of_shares_per_day",
        "reference": "Datar, Naik and Radcliffe (1998)",
        # Owner scope: the denominator is the issuer's DEI count (the panel's rule for
        # share inputs); only a single-class line carries shares_source='dei'.
        "scope": SCOPE_OWNER,
        "size": True,
        "window_bars": 21,
    },
}
#: The natives computed from the P2 bar window (every native but line_market_cap).
PRICE_WINDOW_FEATURES = frozenset(code for code, spec in NATIVE_FEATURES.items() if "window_bars" in spec)
#: Seed-metric roles (catalog ``EXCLUDED_SEED_METRICS`` reasons) the panel never carries as
#: research features: ``presence_indicator`` is a 0/1 building block of an ever-reported
#: missing-is-not-zero rule (catalog round 2), not a characteristic.
EXCLUDED_SEED_ROLES = ("presence_indicator",)


def excluded_seed_metrics() -> dict[str, str]:
    """Seed metrics the panel does not pull as features: ``metric_code`` -> catalog role."""
    return {code: role for code, role in _catalog.EXCLUDED_SEED_METRICS.items() if role in EXCLUDED_SEED_ROLES}
DEFAULT_PROOF_CHUNK_ROOTS = 8192
FORMATION_CHUNK = 12

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

# Every module whose semantics a run's rows depend on: a change between a
# failed run and its resume refuses the resume.
_CODE_FILES = (*(Path(str(module.__file__)) for module in (
    fsr, _derived_lineage, _lineage, _market_owner_bridge, _store)), Path(__file__))


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


def _sql_list(values: Iterable[str]) -> str:
    return ",".join(_sql_text(value) for value in values)


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


def price_line_codes(definitions: Iterable[Any]) -> frozenset[str]:
    """Daily metrics computable from a price line alone, plus the native features.

    A daily metric is owner-scoped when it is ``market_cap``, reads an item, a
    non-daily metric or an issuer-level share input (``shares_outstanding``
    may be the owner's DEI count), or reads another owner-scoped metric.
    """
    daily = {d.metric_code: d for d in definitions if d.window == MARKET_WINDOW}
    owner = set(OWNER_MARKET_CODES & daily.keys())
    changed = True
    while changed:
        changed = False
        for code, definition in sorted(daily.items()):
            if code in owner:
                continue
            if (definition.item_inputs or OWNER_MARKET_INPUTS & set(definition.market_inputs)
                    or any(metric not in daily or metric in owner for metric in definition.metric_inputs)):
                owner.add(code)
                changed = True
    return frozenset(set(daily) - owner) | frozenset(
        code for code, spec in NATIVE_FEATURES.items() if spec["scope"] == SCOPE_PRICE_LINE)


def size_codes(definitions: Iterable[Any]) -> frozenset[str]:
    """Daily metrics that read a share count (``market_cap`` and everything built on it).

    Their rows carry ``shares_source`` / ``size_status``; so do the natives that
    read one (``line_market_cap``, ``turnover_21d``).
    """
    daily = {d.metric_code: d for d in definitions if d.window == MARKET_WINDOW}
    size = set(OWNER_MARKET_CODES & daily.keys())
    changed = True
    while changed:
        changed = False
        for code, definition in sorted(daily.items()):
            if code not in size and (OWNER_MARKET_INPUTS & set(definition.market_inputs)
                                     or any(metric in size for metric in definition.metric_inputs)):
                size.add(code)
                changed = True
    return frozenset(size) | frozenset(code for code, spec in NATIVE_FEATURES.items() if spec["size"])


def feature_scopes(features: Iterable[PanelFeature]) -> dict[str, str]:
    """``feature_id`` -> ``price_line`` or ``owner``."""
    line_codes = price_line_codes(fsr.default_derived_definitions())
    return {f.feature_id: SCOPE_PRICE_LINE if f.is_market and f.metric_code in line_codes else SCOPE_OWNER
            for f in features}


def default_panel_features() -> tuple[PanelFeature, ...]:
    """Every seed metric with a panel window plus the ungated native features; id = code.

    The P2 price/liquidity natives are defaults. ``line_market_cap`` is gated
    behind ``unverified_vendor_shares`` (R2a fix round 2) and is never a default
    feature. Seed metrics of an excluded role (:func:`excluded_seed_metrics`,
    presence indicators) are never pulled.
    """
    excluded = excluded_seed_metrics()
    seeds = [PanelFeature(row.metric_code, row.metric_code, row.window)
             for row in fsr.default_derived_definitions()
             if row.window in PANEL_WINDOWS and row.metric_code not in excluded]
    natives = [PanelFeature(code, code, spec["metric_window"]) for code, spec in NATIVE_FEATURES.items()
               if not spec.get("requires")]
    return tuple(sorted(seeds + natives, key=lambda feature: feature.feature_id))


def canonical_features(features: Iterable[PanelFeature | dict[str, Any]] | None, *,
                       unverified_vendor_shares: bool = False) -> tuple[PanelFeature, ...]:
    source = default_panel_features() if features is None else tuple(features)
    registry = {row.metric_code: row.window for row in fsr.default_derived_definitions()}
    registry.update({code: spec["metric_window"] for code, spec in NATIVE_FEATURES.items()})
    excluded = excluded_seed_metrics()
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
        if registry.get(item.metric_code) != item.metric_window:
            raise ValueError(f"{item.feature_id}: unknown seed metric {item.metric_code}/{item.metric_window}")
        if item.metric_code in excluded:
            raise ValueError(f"{item.feature_id}: {item.metric_code} is a {excluded[item.metric_code]}, "
                             "not a research feature")
        key = (item.metric_code, item.metric_window)
        if item.feature_id in ids or key in keys:
            raise ValueError(f"duplicate feature or metric: {item.feature_id} {key}")
        ids.add(item.feature_id)
        keys.add(key)
        result.append(item)
    if not 1 <= len(result) <= 1024:
        raise ValueError("feature count must be between 1 and 1024")
    gated = [f.feature_id for f in result
             if NATIVE_FEATURES.get(f.metric_code, {}).get("requires") == UNVERIFIED_VENDOR_SHARES]
    if gated and not unverified_vendor_shares:
        raise ValueError(f"{gated} read vendor share counts (vendor units, cover-date clock); they need "
                         f"{UNVERIFIED_VENDOR_SHARES}=True until the A8 share relation lands (R2d)")
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
    # Execution granularity; output-invariant, so not part of the spec:
    # roots proved (and committed) per chunk, formations associated per pass.
    proof_chunk_roots: int = DEFAULT_PROOF_CHUNK_ROOTS
    formation_chunk: int = FORMATION_CHUNK
    # Explicit opt-in to size from vendor share counts (``line_market_cap``);
    # its rows are labeled unverified and R2b size logic must exclude them.
    unverified_vendor_shares: bool = False


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
    chunk = options.proof_chunk_roots
    if isinstance(chunk, bool) or not isinstance(chunk, int) or not 1 <= chunk <= 1_000_000:
        raise ValueError("proof_chunk_roots must be 1..1000000")
    part = options.formation_chunk
    if isinstance(part, bool) or not isinstance(part, int) or not 1 <= part <= 120:
        raise ValueError("formation_chunk must be 1..120")
    types = tuple(options.eligible_security_types)
    if not types or any(not isinstance(t, str) or not re.fullmatch(r"[A-Za-z_]{1,32}", t) for t in types):
        raise ValueError("eligible_security_types must be simple type names")
    run_at = _utc_naive(options.run_at, "run_at")
    if run_at.date() < options.as_of_date:
        raise ValueError("run_at precedes as_of_date")
    if not isinstance(options.unverified_vendor_shares, bool):
        raise ValueError(f"{UNVERIFIED_VENDOR_SHARES} must be a bool")
    features = canonical_features(options.features, unverified_vendor_shares=options.unverified_vendor_shares)
    scopes = feature_scopes(features)
    sized = size_codes(fsr.default_derived_definitions())
    labels = _basis_labels(options.basis)
    spec = {
        "basis": options.basis,
        "universe_id": labels["universe_id"],
        "identity_basis": labels["identity_basis"],
        "features": [[f.feature_id, f.metric_code, f.metric_window, scopes[f.feature_id]] for f in features],
        UNVERIFIED_VENDOR_SHARES: options.unverified_vendor_shares,
        # R2b-facing contract: size, breakpoint and size-neutral logic may use a
        # size feature's value only where size_status is verified.
        "size_policy": {
            "size_features": sorted(f.feature_id for f in features if f.is_market and f.metric_code in sized),
            "verified_shares_sources": list(VERIFIED_SHARES_SOURCES),
            "verified_status": SIZE_VERIFIED,
            "unverified_status": UNVERIFIED_VENDOR_SHARES,
            "rule": "use a size feature only where size_status = verified_status",
        },
        "metric_batch_size": size,
        "max_age_days": options.max_age_days,
        "annual_max_age_days": options.annual_max_age_days,
        "annual_value_origins": list(fsr.ANNUAL_VALUE_ORIGINS),
        "eligible_security_types": sorted(set(types)) if options.basis == BASIS_RECONSTRUCTED else ["common"],
        "include_unlisted_tail": bool(options.include_unlisted_tail) if options.basis == BASIS_RECONSTRUCTED
        else False,
        "value_bearing_reasons": list(VALUE_BEARING_REASONS),
        "decision_policy": "last NYSE session of each closed month at 22:00 UTC; next observed session entry",
        "grain": "dense over eligible members; owner features on one primary line per issuer",
        "primary_line_policy": [PRIMARY_RULE_DOLLAR_VOLUME, TRAILING_DOLLAR_VOLUME_DAYS, PRIMARY_RULE_TIEBREAK],
        "owner_link_failures": list(OWNER_LINK_FAILURES),
        "market_revision_rule": MARKET_REVISION_RULE,
        "excluded_seed_roles": list(EXCLUDED_SEED_ROLES),
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
    columns = [("market_daily_metrics", c) for c in ("close", "volume", "fundamental_available_at")]
    columns += [("market_daily_metrics", f.metric_code) for f in features
                if f.is_market and f.metric_code not in NATIVE_FEATURES]
    sized = size_codes(fsr.default_derived_definitions())
    if any(f.is_market and f.metric_code in sized and f.metric_code not in NATIVE_FEATURES for f in features):
        columns.append(("market_daily_metrics", "shares_source"))
    if any(f.metric_code == "line_market_cap" for f in features):
        columns += [("equity_daily_bars", c) for c in ("close", "adjusted_close", "shares_outstanding")]
    if any(f.metric_code in PRICE_WINDOW_FEATURES for f in features):
        # The bar window is always available as the fallback source.
        columns += [("equity_daily_bars", c) for c in ("trade_date", "close", "adjusted_close", "volume",
                                                       "available_at", "source")]
    if any(f.metric_code == "turnover_21d" for f in features):
        columns += [("market_daily_metrics", c) for c in ("shares_outstanding", "shares_source")]
    absent = [f"{table}.{column}" for table, column in columns if not store.warehouse_has(table, column)]
    if absent:
        raise RuntimeError(f"warehouse lacks panel input columns: {absent}")


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
                   "owner_link_reason", "cohort_reason", "eligible", "issuer_lines", "primary_line",
                   "primary_line_rule")
_VALUE_COLUMNS = ("formation_date", "security_id", "feature_id", "metric_code", "metric_window",
                  "raw_value", "reason", "lineage_status", "available_at", "latest_input_clock",
                  "period_end", "fiscal_period_start", "fiscal_period_end", "value_origin",
                  "age_days", "max_age_days", "owner_cik", "derived_value_id",
                  "derived_owner_security_id", "lineage_digest", "identity_basis",
                  "universe_basis", "availability_basis", "feature_scope", "shares_source", "size_status")
_CALENDAR_STAT_COLUMNS = ("visible_members", "eligible_members", "valid_members", "owner_unlinked_members",
                          "owner_link_attrition", "multi_line_issuers", "cohort_reasons_json", "cohort_sha256")
_COVERAGE_COLUMNS = ("run_id", "formation_date", "feature_id", "metric_code", "metric_window", "batch_ordinal",
                     "status", "eligible_members", "valid_members", "selected_states", "values_emitted",
                     "values_valid", "unmatched_owner_states", "reasons_json", "values_sha256", "feature_scope")
_PROOF_COLUMNS = ("run_id", "derived_value_id", "derived_owner_security_id", "metric_code", "metric_window",
                  "root_available_at", "root_as_of_date", "selected_cik", "status", "reason", "proof_digest",
                  "oldest_fiscal_end", "newest_fiscal_end", "latest_input_clock", "leaf_count", "method")


def _row_json(columns: Sequence[str]) -> str:
    return "to_json(struct_pack(" + ",".join(f"{c} := {c}" for c in columns) + "))"


_COHORT_DIGEST_SQL = ("SELECT sha256(coalesce(string_agg(" + _row_json(_COHORT_COLUMNS)
                      + ", chr(10) ORDER BY security_id), '')) FROM {relation} {where}")
_VALUE_DIGEST_SQL = ("SELECT feature_id, sha256(coalesce(string_agg(" + _row_json(_VALUE_COLUMNS)
                     + ", chr(10) ORDER BY security_id), '')) FROM {relation} {where} GROUP BY feature_id")
_EMPTY_SHA = hashlib.sha256(b"").hexdigest()


def _leak_predicate(cutoff: str, prefix: str = "") -> str:
    """Rows knowable only after the cutoff, or value-bearing rows the rules reject."""
    p = prefix
    return (f"({p}available_at>{cutoff} OR (({p}reason='valid' OR {p}raw_value IS NOT NULL) AND "
            f"({p}latest_input_clock IS NULL OR {p}latest_input_clock>{cutoff})) OR "
            f"({p}raw_value IS NOT NULL AND {p}reason NOT IN ({_sql_list(VALUE_BEARING_REASONS)})))")


# ---------------------------------------------------------------------------
# Builder: calendar, cohorts, primary lines
# ---------------------------------------------------------------------------

def _code_sha() -> str:
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())
    return digest.hexdigest()


def _native_sources(store: ResearchStore, features: Iterable[PanelFeature]) -> dict[str, str]:
    """Where each P2 native reads from in this run: one source per feature per run.

    ``equity_price_metrics`` (source ``derived_equity_price_metrics_v1``) when the
    warehouse has the feature's column *with values* (the same row-window definition,
    precomputed); otherwise the line's own bars. The table can exist with its columns
    and no rows, and "column exists" alone would then drop every value. The choice is
    recorded in the run's definitions, so a resume after it changes is refused.
    """
    sources: dict[str, str] = {}
    for feature in features:
        spec = NATIVE_FEATURES.get(feature.metric_code, {})
        if feature.metric_code not in PRICE_WINDOW_FEATURES:
            continue
        column = spec.get("price_metrics_column")
        populated = False
        if column and all(store.warehouse_has("equity_price_metrics", c) for c in (
                column, "source", "trade_date", "as_of_date", "available_at", "metric_id")):
            populated = bool(store.con.execute(f"""
                SELECT count(*) FROM (SELECT 1 FROM equity_price_metrics
                                      WHERE source=? AND "{column}" IS NOT NULL LIMIT 1)
            """, [PRICE_METRICS_SOURCE_NAME]).fetchone()[0])
        sources[feature.metric_code] = PRICE_METRICS_SOURCE if populated else BARS_SOURCE
    return sources


def _definitions(con: Any, features: tuple[PanelFeature, ...], native_sources: dict[str, str] | None = None,
                 ) -> tuple[str, str, dict[tuple[str, str], str]]:
    roots = {(f.metric_code, f.metric_window) for f in features if not f.is_market}
    derived: list[Any] = []
    hashes: dict[tuple[str, str], str] = {}
    if roots:
        encoded, _, hashes = fsr.resolve_definitions(con, roots)
        derived = json.loads(encoded)
    registry = {row.metric_code: row for row in fsr.default_derived_definitions()}
    market = []
    for f in features:
        if not f.is_market:
            continue
        if f.metric_code in NATIVE_FEATURES:
            entry = {"metric_code": f.metric_code, **NATIVE_FEATURES[f.metric_code]}
            if f.metric_code in PRICE_WINDOW_FEATURES:
                if not native_sources or f.metric_code not in native_sources:
                    raise RuntimeError(f"{f.metric_code}: its source was not resolved for this run")
                entry["source"] = native_sources[f.metric_code]
            market.append(entry)
            continue
        row = registry[f.metric_code]
        market.append({"metric_code": f.metric_code, "metric_window": f.metric_window,
                       "expression": row.expression, "inputs": list(row.inputs), "version": row.version,
                       "source_column": f"market_daily_metrics.{f.metric_code}"})
    encoded = _canonical({"derived": derived, "market": sorted(market, key=lambda m: m["metric_code"])})
    return encoded, _sha(encoded), hashes


def _blockers(basis: str, formations: int, valid_members: int, valid_values: int,
              calendar_rows: Sequence[CalendarRow], unlinked: int, eligible: int) -> tuple[str, ...]:
    blockers = list(_BASE_BLOCKERS)
    if basis == BASIS_RECONSTRUCTED:
        blockers.append("reconstructed_identity_universe_and_availability_not_certifiable")
    if unlinked:
        blockers.append(f"owner_link_attrition:{unlinked}/{eligible}")
    if any(row.status in (CALENDAR_MISSING_MONTH_END, CALENDAR_RULE_CONFLICT) for row in calendar_rows):
        blockers.append("missing_or_conflicting_month_end_sessions")
    if not formations:
        blockers.append("no_formed_month_ends")
    if not valid_members:
        blockers.append("no_valid_cohort_members")
    if not valid_values:
        blockers.append("no_valid_feature_values")
    return tuple(blockers)


def _stage_run_calendar(con: Any, formed: Sequence[CalendarRow]) -> None:
    """``_rp_calendar`` (all formations) and ``_rp_cal_ord`` (with 0-based ordinals)."""
    pairs = [(row.formation_date, row.cutoff) for row in formed]
    if any(later[1] <= earlier[1] for earlier, later in itertools.pairwise(pairs)):  # type: ignore[operator]
        raise RuntimeError("formation cutoffs must increase with the formation date")
    fsr.stage_calendar(con, pairs, table="_rp_calendar")  # type: ignore[arg-type]
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_cal_ord AS
        SELECT CAST(row_number() OVER (ORDER BY decision_date) - 1 AS INTEGER) AS ord, decision_date, cutoff
        FROM _rp_calendar
    """)


def _stage_cohorts(store: ResearchStore, spec: dict[str, Any], market_source: str) -> dict[dt.date, dict[str, Any]]:
    """Stage ``_rp_cohort_all`` for every formation; return per-formation statistics."""
    con = store.con
    fsr.stage_cohort(
        con, calendar_table="_rp_calendar", universe_id=spec["universe_id"],
        identity_basis=spec["identity_basis"], universe_source=UNIVERSE_SOURCE_NAME,
        owner_links_table="_fs_owner_links" if spec["basis"] == BASIS_RECONSTRUCTED else None,
        eligible_security_types=tuple(spec["eligible_security_types"]),
        include_unlisted_tail=spec["include_unlisted_tail"], membership_detail=True,
    )
    # Trailing dollar volume, only for lines of multi-line issuers.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_line_dv AS
        WITH groups AS (
          SELECT decision_date, cik FROM _fs_cohort_cal WHERE cohort_reason='valid'
          GROUP BY ALL HAVING count(*)>1
        ), lines AS (
          SELECT k.decision_date, k.security_id, c.cutoff
          FROM _fs_cohort_cal k JOIN groups g ON g.decision_date=k.decision_date AND g.cik=k.cik
          JOIN _rp_calendar c ON c.decision_date=k.decision_date
          WHERE k.cohort_reason='valid'
        ), sessions AS (
          SELECT l.decision_date, l.security_id, m.trade_date,
                 arg_max_null(CAST(m.close AS DOUBLE)*CAST(m.volume AS DOUBLE),
                              (m.available_at, m.market_daily_id)) AS dollar_volume
          FROM lines l JOIN market_daily_metrics m ON m.security_id=l.security_id
           AND m.trade_date BETWEEN l.decision_date-{TRAILING_DOLLAR_VOLUME_DAYS} AND l.decision_date
           AND m.available_at<=l.cutoff AND m.as_of_date<=l.decision_date
          WHERE m.source=?
          GROUP BY ALL
        )
        SELECT decision_date, security_id,
               sum(dollar_volume) FILTER (WHERE isfinite(dollar_volume) AND dollar_volume>0) AS dollar_volume
        FROM sessions GROUP BY ALL
    """, [market_source])
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_cohort_all AS
        WITH valid AS (
          SELECT k.decision_date, k.security_id, k.cik, d.dollar_volume,
                 count(*) OVER (PARTITION BY k.decision_date, k.cik) AS issuer_lines,
                 row_number() OVER (PARTITION BY k.decision_date, k.cik
                                    ORDER BY coalesce(d.dollar_volume, -1) DESC, k.security_id) AS line_rank
          FROM _fs_cohort_cal k
          LEFT JOIN _rp_line_dv d ON d.decision_date=k.decision_date AND d.security_id=k.security_id
          WHERE k.cohort_reason='valid'
        ), groups AS (
          SELECT decision_date, cik,
                 max(coalesce(dollar_volume, -1)) FILTER (WHERE line_rank=1) AS top,
                 max(coalesce(dollar_volume, -1)) FILTER (WHERE line_rank=2) AS second
          FROM valid GROUP BY ALL
        )
        SELECT k.decision_date, k.security_id, k.symbol, k.security_type, k.exchange_code,
               k.membership_reason, k.membership_cik, k.cik AS owner_cik, k.identity_basis,
               k.owner_link_method, k.owner_link_reason, k.cohort_reason,
               coalesce(k.is_common, false) AS eligible,
               CAST(v.issuer_lines AS INTEGER) AS issuer_lines,
               CASE WHEN v.security_id IS NOT NULL THEN v.line_rank=1 END AS primary_line,
               CASE WHEN v.security_id IS NULL THEN NULL
                    WHEN v.issuer_lines=1 THEN '{PRIMARY_RULE_SINGLE}'
                    WHEN g.top>0 AND g.top>coalesce(g.second, -1) THEN '{PRIMARY_RULE_DOLLAR_VOLUME}'
                    ELSE '{PRIMARY_RULE_TIEBREAK}' END AS primary_line_rule,
               CASE WHEN k.cohort_reason='valid' AND v.line_rank>1 THEN '{SECONDARY_LINE_REASON}'
                    ELSE k.cohort_reason END AS leg_reason
        FROM _fs_cohort_cal k
        LEFT JOIN valid v ON v.decision_date=k.decision_date AND v.security_id=k.security_id
        LEFT JOIN groups g ON g.decision_date=k.decision_date AND g.cik=k.cik
    """)
    failures = _sql_list(OWNER_LINK_FAILURES)
    stats: dict[dt.date, dict[str, Any]] = {}
    for day, visible, eligible, valid, unlinked, multi in con.execute(f"""
        SELECT decision_date, count(*), count(*) FILTER (WHERE eligible),
               count(*) FILTER (WHERE cohort_reason='valid'),
               count(*) FILTER (WHERE eligible AND cohort_reason IN ({failures})),
               count(DISTINCT owner_cik) FILTER (WHERE issuer_lines>1)
        FROM _rp_cohort_all GROUP BY 1
    """).fetchall():
        stats[day] = {"visible": int(visible), "eligible": int(eligible), "valid": int(valid),
                      "unlinked": int(unlinked), "multi_line_issuers": int(multi),
                      "attrition": round(int(unlinked) / int(eligible), 9) if eligible else None,
                      "reasons": {"cohort_reason": {}, "owner_link_reason": {}, "primary_line_rule": {},
                                  "eligible_cohort_reason": {}}}
    for key, sql in (
        ("cohort_reason", "SELECT decision_date, cohort_reason, count(*) FROM _rp_cohort_all GROUP BY ALL"),
        ("eligible_cohort_reason", """
            SELECT decision_date, leg_reason, count(*) FROM _rp_cohort_all WHERE eligible GROUP BY ALL"""),
        ("owner_link_reason", """
            SELECT decision_date, coalesce(owner_link_reason,'linked'), count(*) FROM _rp_cohort_all
            WHERE cohort_reason NOT IN ('overlapping_membership','not_common') GROUP BY ALL"""),
        ("primary_line_rule", """
            SELECT decision_date, primary_line_rule, count(*) FROM _rp_cohort_all
            WHERE primary_line GROUP BY ALL"""),
    ):
        for day, reason, n in con.execute(sql).fetchall():
            stats[day]["reasons"][key][str(reason)] = int(n)
    return stats


def _empty_cohort_stats() -> dict[str, Any]:
    """The statistics of a formation with no visible member."""
    return {"visible": 0, "eligible": 0, "valid": 0, "unlinked": 0, "multi_line_issuers": 0, "attrition": None,
            "reasons": {"cohort_reason": {}, "owner_link_reason": {}, "primary_line_rule": {},
                        "eligible_cohort_reason": {}}}


def _persist_cohorts(store: ResearchStore, run_id: str, formed: Sequence[CalendarRow],
                     stats: dict[dt.date, dict[str, Any]]) -> None:
    con = store.con
    done = dict(con.execute("""
        SELECT formation_date, cohort_sha256 FROM research_panel_calendar
        WHERE run_id=? AND cohort_sha256 IS NOT NULL
    """, [run_id]).fetchall())
    for row in formed:
        day = row.formation_date
        cohort = stats.setdefault(day, _empty_cohort_stats())  # type: ignore[arg-type]
        cohort["digest"] = con.execute(_COHORT_DIGEST_SQL.format(
            relation="_rp_cohort_all", where="WHERE decision_date=?"), [day]).fetchone()[0]
        stored = done.get(day)
        if stored is not None:
            if stored != cohort["digest"]:
                raise RuntimeError(f"cohort for {day} changed since the interrupted run")
            continue
        with store.transaction():
            con.execute(f"INSERT INTO research_panel_cohort (run_id,formation_date,{','.join(_COHORT_COLUMNS)}) "
                        f"SELECT ?,decision_date,{','.join(_COHORT_COLUMNS)} FROM _rp_cohort_all "
                        f"WHERE decision_date=?", [run_id, day])
            con.execute(f"""
                UPDATE research_panel_calendar
                SET {','.join(f'{c}=?' for c in _CALENDAR_STAT_COLUMNS)}
                WHERE run_id=? AND month_start=?
            """, [cohort["visible"], cohort["eligible"], cohort["valid"], cohort["unlinked"],
                  cohort["attrition"], cohort["multi_line_issuers"], _canonical(cohort["reasons"]),
                  cohort["digest"], run_id, row.month_start])


# ---------------------------------------------------------------------------
# Builder: derived metric batches (selection across formations, proofs)
# ---------------------------------------------------------------------------

_BUCKET_SQL = ("coalesce(d.target_bucket, CAST(floor((year(d.period_end)*12+month(d.period_end)-1+"
               "CASE WHEN day(d.period_end)>=15 THEN 1 ELSE 0 END)/3.0) AS BIGINT))")


def _stage_segments(con: Any, formations: int) -> dict[str, int]:
    """``_rp_seg``: the selected state per (owner, metric) as [from_ord, to_ord) runs.

    Fast path (exact under I: one period end per bucket, and J: one bucket per
    period end): the FQ1 selection at a formation is the arg-max of
    (period_end, available_at, derived_value_id) over the states visible there,
    and visibility only grows with the formation, so a running maximum over each
    state's first visible formation gives every formation's selection. Owners x
    metrics that break I or J are selected by the FQ1 ranking itself.
    """
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_irregular AS
        SELECT DISTINCT security_id, metric_code, metric_window FROM (
          SELECT security_id, metric_code, metric_window FROM _rp_states
          GROUP BY security_id, metric_code, metric_window, bucket HAVING count(DISTINCT period_end)>1
          UNION ALL
          SELECT security_id, metric_code, metric_window FROM _rp_states
          GROUP BY security_id, metric_code, metric_window, period_end HAVING count(DISTINCT bucket)>1)
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_seg AS
        WITH regular AS (
          SELECT s.security_id, s.metric_code, s.metric_window, s.period_end, s.available_at,
                 greatest(s.period_end, s.as_of_date) AS need_date, s.derived_value_id
          FROM _rp_states s ANTI JOIN _rp_irregular i
            ON i.security_id=s.security_id AND i.metric_code=s.metric_code
           AND i.metric_window=s.metric_window
        ), by_clock AS (
          SELECT r.*, c.ord AS clock_ord FROM regular r ASOF JOIN _rp_cal_ord c ON r.available_at<=c.cutoff
        ), visible AS (
          SELECT b.*, greatest(b.clock_ord, c.ord) AS first_ord
          FROM by_clock b ASOF JOIN _rp_cal_ord c ON b.need_date<=c.decision_date
        ), firsts AS (
          SELECT security_id, metric_code, metric_window, first_ord,
                 max(struct_pack(p := period_end, a := available_at, i := derived_value_id)) AS k
          FROM visible GROUP BY ALL
        ), running AS (
          SELECT *, max(k) OVER (PARTITION BY security_id, metric_code, metric_window ORDER BY first_ord
                                 ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS best
          FROM firsts
        )
        SELECT security_id, metric_code, metric_window, CAST(first_ord AS INTEGER) AS from_ord,
               CAST(lead(first_ord, 1, {int(formations)}) OVER (
                 PARTITION BY security_id, metric_code, metric_window ORDER BY first_ord) AS INTEGER) AS to_ord,
               best.i AS derived_value_id
        FROM running
    """)
    irregular = int(con.execute("SELECT count(*) FROM _rp_irregular").fetchone()[0])
    if irregular:
        con.execute("""
            CREATE OR REPLACE TEMP TABLE _rp_irr_states AS
            SELECT s.* FROM _rp_states s SEMI JOIN _rp_irregular i
              ON i.security_id=s.security_id AND i.metric_code=s.metric_code AND i.metric_window=s.metric_window
        """)
        for start in range(0, formations, FORMATION_CHUNK):
            con.execute("""
                CREATE OR REPLACE TEMP TABLE _rp_cal_chunk AS
                SELECT decision_date, cutoff FROM _rp_cal_ord WHERE ord BETWEEN ? AND ?
            """, [start, start + FORMATION_CHUNK - 1])
            fsr.stage_state_selection(con, calendar_table="_rp_cal_chunk", metrics_table="_rp_metrics",
                                      metric_source=DERIVED_SOURCE_NAME, state_source="_rp_irr_states")
            con.execute("""
                INSERT INTO _rp_seg
                SELECT k.security_id, k.metric_code, k.metric_window, c.ord, c.ord+1, k.derived_value_id
                FROM _fs_selected_keys k JOIN _rp_cal_ord c ON c.decision_date=k.decision_date
            """)
    segments = int(con.execute("SELECT count(*) FROM _rp_seg").fetchone()[0])
    return {"irregular_owner_metrics": irregular, "segments": segments}


def _prove_todo(store: ResearchStore, run_id: str, hashes: dict[tuple[str, str], str],
                todo_sql: str, params: list[Any], chunk: int) -> dict[str, int]:
    """Prove every root of ``todo_sql`` (columns: root_id) into the slim proof table.

    Each chunk commits on its own, so an interrupted run resumes after the last
    committed chunk (the todo relation excludes proved roots).
    """
    con = store.con
    # Owner-ordered chunks: an owner's states, their metric children and their
    # leaves land in the same chunk (fetched once, from nearby storage).
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_todo AS
        SELECT t.root_id, s.security_id, s.metric_code, s.metric_window, s.available_at, s.as_of_date,
               row_number() OVER (ORDER BY s.security_id, s.available_at, t.root_id) AS n
        FROM ({todo_sql}) t JOIN _rp_states s ON s.derived_value_id=t.root_id
    """, params)
    total = int(con.execute("SELECT count(*) FROM _rp_todo").fetchone()[0])
    counts: dict[str, int] = {"roots": total}
    for start in range(1, total + 1, chunk):
        with store.transaction():
            con.execute("CREATE OR REPLACE TEMP TABLE _rp_chunk AS SELECT * FROM _rp_todo WHERE n BETWEEN ? AND ?",
                        [start, start + chunk - 1])
            got = _lineage.prove_roots(con, hashes, roots_table="_rp_chunk")
            con.execute(f"""
                INSERT INTO research_lineage_proofs ({','.join(_PROOF_COLUMNS)})
                SELECT ?, r.root_id, c.security_id, c.metric_code, c.metric_window, c.available_at,
                       c.as_of_date, r.selected_cik, r.status, r.reason, r.proof_digest, r.oldest_fiscal_end,
                       r.newest_fiscal_end, r.latest_input_clock, r.leaf_count, r.method
                FROM _lp_result r JOIN _rp_chunk c ON c.root_id=r.root_id
            """, [run_id])
        for key, value in got.items():
            if key != "levels":
                counts[key] = counts.get(key, 0) + int(value)
        counts["max_levels"] = max(counts.get("max_levels", 0), int(got.get("levels", 0)))
    return counts


def _stage_batch_proofs(con: Any, run_id: str) -> None:
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_batch_proofs AS
        SELECT p.* FROM research_lineage_proofs p
        JOIN _rp_metrics m ON m.metric_code=p.metric_code AND m.metric_window=p.metric_window
        WHERE p.run_id=?
    """, [run_id])


def _derived_selection(store: ResearchStore, run_id: str, batch: _Batch, spec: dict[str, Any],
                       hashes: dict[tuple[str, str], str], formations: int, chunk: int) -> dict[str, Any]:
    """Select, fetch and prove one derived metric batch for every formation at once."""
    con = store.con
    started = time.perf_counter()
    fsr.stage_metric_legs(con, [
        fsr.MetricLeg(f.metric_code, f.metric_window, hashes[(f.metric_code, f.metric_window)],
                      spec["max_age_days"], spec["annual_max_age_days"],
                      f.metric_code == "eps_diluted_q_growth_yoy")
        for f in batch.features
    ], table="_rp_metrics")
    last_date, last_cutoff = con.execute("SELECT max(decision_date), max(cutoff) FROM _rp_cal_ord").fetchone()
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_states AS
        SELECT d.derived_value_id, d.source, d.security_id, d.metric_code, d.metric_window, d.target_bucket,
               {_BUCKET_SQL} AS bucket, d.period_end, d.available_at, d.as_of_date
        FROM derived_metric_values d
        JOIN _rp_metrics m ON m.metric_code=d.metric_code AND m.metric_window=d.metric_window
        WHERE d.source=? AND d.period_end<=? AND d.available_at<=? AND d.as_of_date<=?
    """, [DERIVED_SOURCE_NAME, last_date, last_cutoff, last_date])
    stats: dict[str, Any] = {"states": int(con.execute("SELECT count(*) FROM _rp_states").fetchone()[0])}
    stats.update(_stage_segments(con, formations))
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_sel_rows AS
        SELECT d.derived_value_id, d.source, d.security_id, d.metric_code, d.metric_window, d.target_bucket,
               d.period_end, d.value, d.available_at, d.as_of_date, d.valid_to, d.inputs_hash,
               d.definition_hash, d.history_status, d.value_status, d.selected_input_refs_hash,
               d.fiscal_period_start, d.fiscal_period_end, d.value_origin
        FROM derived_metric_values d
        JOIN _rp_metrics m ON m.metric_code=d.metric_code AND m.metric_window=d.metric_window
        WHERE d.source=? AND d.derived_value_id IN (SELECT derived_value_id FROM _rp_seg)
    """, [DERIVED_SOURCE_NAME])
    stats["selection_seconds"] = round(time.perf_counter() - started, 3)
    # Owners that can match a member: id names a linked member's CIK, the id is
    # a member line, or the id names no CIK at all (cannot judge: prove).
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_owner_keep AS
        WITH owners AS (
          SELECT DISTINCT security_id, regexp_extract(security_id, 'CIK-([0-9]{1,10})$', 1) AS id_cik
          FROM _rp_seg
        ), linked AS (
          SELECT DISTINCT owner_cik FROM _rp_cohort_all WHERE cohort_reason='valid'
        ), lines AS (
          SELECT DISTINCT security_id FROM _rp_cohort_all WHERE cohort_reason='valid'
        )
        SELECT o.security_id FROM owners o
        WHERE o.id_cik=''
           OR lpad(o.id_cik, 10, '0') IN (SELECT owner_cik FROM linked)
           OR o.security_id IN (SELECT security_id FROM lines)
    """)
    stats["selected_roots"], stats["skipped_unlinked_roots"] = (int(v) for v in con.execute("""
        SELECT count(DISTINCT derived_value_id),
               count(DISTINCT derived_value_id) FILTER (WHERE security_id NOT IN (SELECT security_id FROM _rp_owner_keep))
        FROM _rp_seg
    """).fetchone())
    started = time.perf_counter()
    _stage_batch_proofs(con, run_id)
    stats["proofs_selected"] = _prove_todo(store, run_id, hashes, """
        SELECT DISTINCT s.derived_value_id AS root_id FROM _rp_seg s
        SEMI JOIN _rp_owner_keep o ON o.security_id=s.security_id
        ANTI JOIN _rp_batch_proofs p ON p.derived_value_id=s.derived_value_id
    """, [], chunk)
    _stage_batch_proofs(con, run_id)
    # Prior history of owners whose selected state proves no CIK (FQ1's
    # prior-owner association), proved once per root; no count abort.
    stats["proofs_prior"] = _prove_todo(store, run_id, hashes, """
        WITH unowned AS (
          SELECT p.derived_owner_security_id AS security_id, p.metric_code, p.metric_window,
                 max(p.root_available_at) AS upto
          FROM _rp_batch_proofs p SEMI JOIN _rp_seg s ON s.derived_value_id=p.derived_value_id
          WHERE p.selected_cik IS NULL GROUP BY ALL
        )
        SELECT DISTINCT st.derived_value_id AS root_id
        FROM _rp_states st JOIN unowned u ON u.security_id=st.security_id
         AND u.metric_code=st.metric_code AND u.metric_window=st.metric_window
        WHERE st.available_at<u.upto
          AND st.derived_value_id NOT IN (SELECT derived_value_id FROM _rp_batch_proofs)
    """, [], chunk)
    _stage_batch_proofs(con, run_id)
    stats["proof_seconds"] = round(time.perf_counter() - started, 3)
    return stats


def _derived_formations(store: ResearchStore, run_id: str, batch: _Batch, spec: dict[str, Any],
                        rows: Sequence[CalendarRow], ordinals: Sequence[int]) -> dict[str, Any]:
    """Associate the batch's selected states with the eligible members of a few formations.

    One pass per formation chunk: the batch-wide state and proof relations are
    joined once per chunk, not once per formation.
    """
    con = store.con
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_cal_part (ord INTEGER, decision_date DATE, cutoff TIMESTAMP)")
    con.executemany("INSERT INTO _rp_cal_part VALUES (?,?,?)",
                    [[ordinal, row.formation_date, row.cutoff] for row, ordinal in zip(rows, ordinals, strict=True)])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _fs_selected_keys AS
        SELECT c.decision_date, s.metric_code, s.metric_window, s.security_id, s.derived_value_id
        FROM _rp_seg s JOIN _rp_cal_part c ON c.ord>=s.from_ord AND c.ord<s.to_ord
    """)
    fsr.stage_selected_rows(con, calendar_table="_rp_cal_part", metric_source=DERIVED_SOURCE_NAME,
                            rows_source="_rp_sel_rows")
    # Only the primary line of an owner-linked issuer competes for owner
    # features; every other eligible member keeps its exclusion reason.
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_leg_cohort AS
        SELECT k.decision_date, k.security_id, k.owner_cik AS cik, k.leg_reason AS cohort_reason
        FROM _rp_cohort_all k SEMI JOIN _rp_cal_part c ON c.decision_date=k.decision_date
        WHERE k.eligible
    """)
    fsr.stage_owner_legs(con, run_id=run_id, calendar_table="_rp_cal_part", cohort_table="_rp_leg_cohort",
                         metrics_table="_rp_metrics", proof_table="_rp_batch_proofs", staged_slim=True)
    kept = "l.cohort_reason='valid'"
    # P11 hook (IFRS_REPORTER_REASON): once P11 lands, a valid owner whose filings are
    # IFRS and unstandardized takes ``ifrs_reporter_not_standardized`` here in place of
    # ``missing_metric_state`` (l.reason), keeping the NULL value and the row count.
    state = {column: f"CASE WHEN {kept} THEN l.{column} END" for column in (
        "lineage_status", "available_at", "latest_input_clock", "period_end", "fiscal_period_start",
        "fiscal_period_end", "value_origin", "derived_value_id", "derived_owner_security_id",
        "lineage_digest")}
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_batch_values AS
        SELECT l.decision_date AS formation_date, l.security_id, f.feature_id,
               l.metric_code, l.metric_window,
               CAST(CASE WHEN l.reason='valid' THEN l.raw_value
                         WHEN l.reason='stale_current_anchor'
                              AND l.latest_input_clock<=l.cutoff
                              AND l.latest_input_clock<=l.available_at
                              AND (l.state_valid_to IS NULL OR l.state_valid_to>l.cutoff)
                         THEN l.raw_value END AS DOUBLE) AS raw_value,
               l.reason, {state['lineage_status']} AS lineage_status,
               {state['available_at']} AS available_at, {state['latest_input_clock']} AS latest_input_clock,
               {state['period_end']} AS period_end, {state['fiscal_period_start']} AS fiscal_period_start,
               {state['fiscal_period_end']} AS fiscal_period_end, {state['value_origin']} AS value_origin,
               CASE WHEN {kept} THEN CAST(l.age_days AS INTEGER) END AS age_days,
               CASE WHEN {kept} THEN CAST(l.max_age_days AS INTEGER) END AS max_age_days,
               k.owner_cik, {state['derived_value_id']} AS derived_value_id,
               {state['derived_owner_security_id']} AS derived_owner_security_id,
               {state['lineage_digest']} AS lineage_digest, k.identity_basis,
               ? AS universe_basis, ? AS availability_basis, '{SCOPE_OWNER}' AS feature_scope,
               CAST(NULL AS VARCHAR) AS shares_source, CAST(NULL AS VARCHAR) AS size_status
        FROM _fs_leg l
        JOIN _rp_features f ON f.metric_code=l.metric_code AND f.metric_window=l.metric_window
        JOIN _rp_cohort_all k ON k.decision_date=l.decision_date AND k.security_id=l.security_id
    """, [spec["universe_id"], FUNDAMENTAL_AVAILABILITY_BASIS])
    features = {(f.metric_code, f.metric_window): f.feature_id for f in batch.features}
    unmatched: dict[tuple[dt.date, str], int] = {}
    for day, code, window, n in con.execute("""
        SELECT decision_date, metric_code, metric_window, count(*) FROM _fs_owner_state
        WHERE association_cik IS NULL GROUP BY ALL
    """).fetchall():
        unmatched[(day, features[(code, window)])] = int(n)
    selected = {(day, feature): int(n) for day, feature, n in con.execute("""
        SELECT formation_date, feature_id, count(*) FILTER (WHERE derived_value_id IS NOT NULL)
        FROM _rp_batch_values GROUP BY ALL
    """).fetchall()}
    return {"unmatched": unmatched, "selected": selected}


# ---------------------------------------------------------------------------
# Builder: market batch
# ---------------------------------------------------------------------------

_BAR_CLOCK_SQL = f"greatest(b.available_at, CAST(b.trade_date AS TIMESTAMP) + INTERVAL {DECISION_HOUR} HOUR)"


def _stage_price_window(con: Any, row: CalendarRow) -> None:
    """``_rp_price_window``: per eligible line, the P2 aggregates over its last 252 bars.

    A bar is one session of the line's positive-price vendor rows whose clock
    (``greatest(available_at, trade_date 22:00)``) is at or before the cutoff,
    deduped like market_daily dedupes a bar (each column from the newest
    ``(available_at, source)`` row that has it). Only bars dated in the
    ``PRICE_LOOKBACK_DAYS`` before the session are read, so no input is dated
    after the session or knowable only after the cutoff.

    Windows count bars (rows), as ``equity_price_metrics`` does. Returns are
    ``adj_t / adj_(t-1) - 1`` of consecutive bars; the first bar read has none.
    ``back`` = 1 is the latest bar; a line whose latest bar is not the formation
    session gets no value (``missing_market_row``). The scan is bounded by the
    formation (eligible lines x lookback), never the whole bar history.
    """
    first = row.formation_date - dt.timedelta(days=PRICE_LOOKBACK_DAYS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_price_window AS
        WITH bars AS (
          SELECT b.security_id, b.trade_date,
                 CAST(arg_max(b.close, (b.available_at, b.source)) AS DOUBLE) AS close,
                 CAST(arg_max(b.adjusted_close, (b.available_at, b.source)) AS DOUBLE) AS adj,
                 CAST(arg_max(b.volume, (b.available_at, b.source)) AS DOUBLE) AS volume,
                 max({_BAR_CLOCK_SQL}) AS bar_at
          FROM equity_daily_bars b
          SEMI JOIN (SELECT security_id FROM _rp_cohort_all WHERE decision_date=? AND eligible) k
            ON k.security_id=b.security_id
          WHERE b.trade_date BETWEEN ? AND ? AND b.close>0 AND b.adjusted_close>0
            AND isfinite(b.close) AND isfinite(b.adjusted_close) AND {_BAR_CLOCK_SQL}<=?
          GROUP BY b.security_id, b.trade_date
        ), seq AS (
          SELECT *, lag(adj) OVER w AS prior_adj,
                 count(*) OVER (PARTITION BY security_id) - row_number() OVER w + 1 AS back
          FROM bars WINDOW w AS (PARTITION BY security_id ORDER BY trade_date)
        ), r AS (
          SELECT security_id, trade_date, adj, volume, bar_at, back, adj / prior_adj - 1.0 AS ret,
                 CASE WHEN close * volume > 0 THEN abs(adj / prior_adj - 1.0) / (close * volume) END AS illiq,
                 close / adj AS basis
          FROM seq WHERE back <= {PRICE_WINDOW_BARS}
        )
        SELECT security_id, max(trade_date) AS last_date, max(bar_at) AS available_at,
               max(adj) FILTER (WHERE back = 1) AS adj_last, max(adj) AS high_252,
               count(ret) FILTER (WHERE back <= 21) AS n_ret_21, max(ret) FILTER (WHERE back <= 21) AS max_ret_21,
               count(illiq) FILTER (WHERE back <= 21) AS n_illiq_21,
               avg(illiq) FILTER (WHERE back <= 21) AS illiq_21,
               count(ret) FILTER (WHERE back <= 60) AS n_ret_60,
               avg(power(least(ret, 0.0), 2)) FILTER (WHERE back <= 60) AS down_sq_60,
               count(volume) FILTER (WHERE back <= 21) AS n_vol_21,
               avg(volume) FILTER (WHERE back <= 21) AS volume_21,
               max(basis) FILTER (WHERE back <= 21) / min(basis) FILTER (WHERE back <= 21) AS basis_move_21
        FROM r GROUP BY security_id
    """, [row.formation_date, first, row.formation_date, row.cutoff])


def _price_window_parts(con: Any, features: Sequence[PanelFeature], spec: dict[str, Any], market_source: str,
                        row: CalendarRow) -> list[str]:
    """Long-form value rows of the P2 natives of one formation (see ``NATIVE_FEATURES``).

    ``reason_hint`` names why a present row has no value: ``incomplete_price_window``
    (fewer bars or returns than the window needs), ``unverified_shares`` (turnover
    whose share count is not a DEI count), ``split_in_price_window`` (turnover
    across a split) or ``missing_market_row`` (turnover without a market_daily row
    for its share count).
    """
    sources = spec["native_sources"]
    from_bars = [f for f in features if sources[f.metric_code] == BARS_SOURCE]
    from_metrics = [f for f in features if sources[f.metric_code] == PRICE_METRICS_SOURCE]
    parts: list[str] = []
    session = f"DATE '{row.formation_date.isoformat()}'"
    incomplete = _sql_text(INCOMPLETE_WINDOW_REASON)
    if from_bars:
        _stage_price_window(con, row)
    window_sql = {
        "pct_from_high_252d": ("CASE WHEN high_252 > 0 THEN adj_last / high_252 - 1.0 END", "NULL"),
        "max_daily_return_21d": ("CASE WHEN n_ret_21 = 21 THEN max_ret_21 END",
                                 f"CASE WHEN n_ret_21 < 21 THEN {incomplete} END"),
        "amihud_illiquidity_21d": (f"CASE WHEN n_illiq_21 = 21 THEN illiq_21 * {AMIHUD_SCALE!r} END",
                                   f"CASE WHEN n_illiq_21 < 21 THEN {incomplete} END"),
        "downside_deviation_60d": ("CASE WHEN n_ret_60 = 60 THEN sqrt(down_sq_60) * sqrt(252.0) END",
                                   f"CASE WHEN n_ret_60 < 60 THEN {incomplete} END"),
    }
    for f in from_bars:
        if f.metric_code == "turnover_21d":
            continue
        value, hint = window_sql[f.metric_code]
        parts.append(f"SELECT security_id, {_sql_text(f.feature_id)} AS feature_id, CAST({value} AS DOUBLE) AS "
                     f"value, available_at, CAST(NULL AS TIMESTAMP) AS fundamental_available_at, "
                     f"'{BARS_SOURCE}' AS value_origin, CAST(NULL AS VARCHAR) AS shares_source, "
                     f"CAST({hint} AS VARCHAR) AS reason_hint FROM _rp_price_window WHERE last_date={session}")
    turnover = next((f for f in from_bars if f.metric_code == "turnover_21d"), None)
    if turnover is not None:
        # The line's verified share count at the session: the newest visible
        # market_daily revision (arg_max_null, as every market pick).
        con.execute("""
            CREATE OR REPLACE TEMP TABLE _rp_line_shares AS
            SELECT m.security_id, max(m.available_at) AS available_at,
                   arg_max_null(m.fundamental_available_at, (m.available_at, m.market_daily_id))
                     AS fundamental_available_at,
                   CAST(arg_max_null(m.shares_outstanding, (m.available_at, m.market_daily_id)) AS DOUBLE)
                     AS shares,
                   arg_max_null(m.shares_source, (m.available_at, m.market_daily_id)) AS shares_source
            FROM market_daily_metrics m
            JOIN _rp_cal_one c ON m.trade_date=c.decision_date AND m.available_at<=c.cutoff
                              AND m.as_of_date<=c.decision_date
            JOIN _rp_cohort_all k ON k.decision_date=c.decision_date AND k.security_id=m.security_id AND k.eligible
            WHERE m.source=?
            GROUP BY m.security_id
        """, [market_source])
        verified = _sql_list(VERIFIED_SHARES_SOURCES)
        parts.append(f"""
            SELECT w.security_id, {_sql_text(turnover.feature_id)} AS feature_id,
                   CAST(CASE WHEN s.shares_source IN ({verified}) AND w.n_vol_21 = 21 AND s.shares > 0
                             THEN w.volume_21 / s.shares END AS DOUBLE) AS value,
                   greatest(w.available_at, s.available_at) AS available_at, s.fundamental_available_at,
                   '{BARS_SOURCE}' AS value_origin, s.shares_source,
                   CASE WHEN s.security_id IS NULL THEN 'missing_market_row'
                        WHEN s.shares_source IS NULL OR s.shares_source NOT IN ({verified})
                             THEN {_sql_text(UNVERIFIED_SHARES_REASON)}
                        WHEN w.n_vol_21 < 21 THEN {incomplete}
                        WHEN w.basis_move_21 > {SPLIT_RATIO_LIMIT!r} THEN {_sql_text(SPLIT_WINDOW_REASON)}
                        END AS reason_hint
            FROM _rp_price_window w LEFT JOIN _rp_line_shares s ON s.security_id=w.security_id
            WHERE w.last_date={session}""")
    if from_metrics:
        # The same definitions, precomputed by equity_price_metrics (its clock is the
        # running max of its input bars' clocks): the session's newest visible row.
        columns = [NATIVE_FEATURES[f.metric_code]["price_metrics_column"] for f in from_metrics]
        picks = ", ".join(f'arg_max_null(p."{c}", (p.available_at, p.metric_id)) AS "{c}"' for c in columns)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _rp_price_metrics AS
            SELECT p.security_id, max(p.available_at) AS available_at, {picks}
            FROM equity_price_metrics p
            JOIN _rp_cal_one c ON p.trade_date=c.decision_date AND p.available_at<=c.cutoff
                              AND p.as_of_date<=c.decision_date
            JOIN _rp_cohort_all k ON k.decision_date=c.decision_date AND k.security_id=p.security_id AND k.eligible
            WHERE p.source=?
            GROUP BY p.security_id
        """, [PRICE_METRICS_SOURCE_NAME])
        for f, column in zip(from_metrics, columns, strict=True):
            parts.append(f"SELECT security_id, {_sql_text(f.feature_id)} AS feature_id, CAST(\"{column}\" AS DOUBLE) "
                         f"AS value, available_at, CAST(NULL AS TIMESTAMP) AS fundamental_available_at, "
                         f"'{PRICE_METRICS_SOURCE}' AS value_origin, CAST(NULL AS VARCHAR) AS shares_source, "
                         f"CAST(NULL AS VARCHAR) AS reason_hint FROM _rp_price_metrics")
    return parts


def _market_formation(store: ResearchStore, batch: _Batch, spec: dict[str, Any], market_source: str,
                      row: CalendarRow) -> dict[str, Any]:
    con = store.con
    fsr.stage_calendar(con, [(row.formation_date, row.cutoff)], table="_rp_cal_one")  # type: ignore[list-item]
    columns = [f.metric_code for f in batch.features if f.metric_code not in NATIVE_FEATURES]
    sized = set(spec["size_policy"]["size_features"])
    long_parts = []
    if columns:
        # The newest visible revision wins per column even when it withholds the
        # value (arg_max_null); plain arg_max would skip its NULL and revive an
        # older revision's value under the newer clock (``max(available_at)``).
        picks = ",\n".join(f'arg_max_null(m."{code}", (m.available_at, m.market_daily_id)) AS "{code}"'
                           for code in columns)
        # The share basis of the row's size (market_cap and what reads it):
        # only a DEI count is verified; vendor ('archive*', 'class_sum') is not.
        shares = ("arg_max_null(m.shares_source, (m.available_at, m.market_daily_id))"
                  if any(f.feature_id in sized for f in batch.features if f.metric_code in columns)
                  else "CAST(NULL AS VARCHAR)")
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _rp_market AS
            SELECT m.security_id, max(m.available_at) AS available_at,
                   arg_max_null(m.fundamental_available_at, (m.available_at, m.market_daily_id))
                     AS fundamental_available_at,
                   {shares} AS shares_source,
                   {picks}
            FROM market_daily_metrics m
            JOIN _rp_cal_one c ON m.trade_date=c.decision_date AND m.available_at<=c.cutoff
                              AND m.as_of_date<=c.decision_date
            JOIN _rp_cohort_all k ON k.decision_date=c.decision_date AND k.security_id=m.security_id
                                 AND k.eligible
            WHERE m.source=?
            GROUP BY m.security_id
        """, [market_source])
        long_parts += [
            f"SELECT security_id, {_sql_text(f.feature_id)} AS feature_id, CAST(\"{f.metric_code}\" AS DOUBLE) "
            f"AS value, available_at, fundamental_available_at, 'market_daily' AS value_origin, "
            f"{'shares_source' if f.feature_id in sized else 'CAST(NULL AS VARCHAR)'} AS shares_source, "
            f"CAST(NULL AS VARCHAR) AS reason_hint FROM _rp_market"
            for f in batch.features if f.metric_code in columns]
    if any(f.metric_code == "line_market_cap" for f in batch.features):
        # UNVERIFIED (opt-in only): the line's own close x its own vendor share
        # count, deduped and clocked exactly as market_daily dedupes a bar
        # (``_bars_by_session_sql``): positive-price rows, each column from the
        # newest (available_at, source) row that has it. The rows of one session
        # are vendors, not revisions (each vendor's rows are replaced in place),
        # so a price-only vendor's missing count is taken from another vendor, as
        # market_daily's ``archive_shares`` is (R2e m2). The vendor count is in
        # vendor units and dated from the cover as-of date, which precedes the
        # filing; R2d replaces it with the A8 share relation.
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _rp_line_cap AS
            SELECT b.security_id,
                   CAST(arg_max(b.close, (b.available_at, b.source)) AS DOUBLE)
                   * CAST(arg_max(b.shares_outstanding, (b.available_at, b.source)) AS DOUBLE) AS value,
                   greatest(max(b.available_at), CAST(b.trade_date AS TIMESTAMP) + INTERVAL {DECISION_HOUR} HOUR)
                     AS available_at
            FROM equity_daily_bars b
            JOIN _rp_cohort_all k ON k.decision_date=? AND k.security_id=b.security_id AND k.eligible
            WHERE b.trade_date=? AND b.close>0 AND b.adjusted_close>0
            GROUP BY b.security_id, b.trade_date
        """, [row.formation_date, row.formation_date])
        feature = next(f for f in batch.features if f.metric_code == "line_market_cap")
        long_parts.append(f"SELECT security_id, {_sql_text(feature.feature_id)} AS feature_id, value, "
                          f"available_at, CAST(NULL AS TIMESTAMP) AS fundamental_available_at, "
                          f"'equity_daily_bars' AS value_origin, '{LINE_SHARES_SOURCE}' AS shares_source, "
                          f"CAST(NULL AS VARCHAR) AS reason_hint FROM _rp_line_cap")
    window = [f for f in batch.features if f.metric_code in PRICE_WINDOW_FEATURES]
    if window:
        long_parts += _price_window_parts(con, window, spec, market_source, row)
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_batch_features (feature_id VARCHAR, metric_code VARCHAR, "
                "feature_scope VARCHAR, size_feature BOOLEAN, availability_basis VARCHAR)")
    con.executemany("INSERT INTO _rp_batch_features VALUES (?,?,?,?,?)", [
        [f.feature_id, f.metric_code, spec["scopes"][f.feature_id], f.feature_id in sized,
         VENDOR_SHARES_AVAILABILITY_BASIS if NATIVE_FEATURES.get(f.metric_code, {}).get("requires")
         == UNVERIFIED_VENDOR_SHARES else MARKET_AVAILABILITY_BASIS]
        for f in batch.features])
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_batch_values AS
        WITH long AS ({' UNION ALL '.join(long_parts)}), grid AS (
          SELECT k.decision_date, c.cutoff, k.security_id, k.owner_cik, k.identity_basis, k.cohort_reason,
                 k.leg_reason, f.feature_id, f.metric_code, f.feature_scope, f.size_feature,
                 f.availability_basis AS feature_availability_basis
          FROM _rp_cohort_all k JOIN _rp_cal_one c ON c.decision_date=k.decision_date
          CROSS JOIN _rp_batch_features f
          WHERE k.eligible
        ), judged AS (
          SELECT g.*, x.value, x.available_at, x.fundamental_available_at, x.value_origin, x.shares_source,
                 x.security_id IS NOT NULL AS has_row,
                 CASE WHEN g.feature_scope='{SCOPE_OWNER}' AND g.leg_reason<>'valid' THEN g.leg_reason
                      WHEN g.cohort_reason='overlapping_membership' THEN 'overlapping_membership'
                      WHEN x.security_id IS NULL THEN 'missing_market_row'
                      WHEN x.available_at>g.cutoff OR coalesce(x.fundamental_available_at>g.cutoff, false)
                           THEN 'invalid_input_clock'
                      WHEN x.reason_hint IS NOT NULL THEN x.reason_hint
                      WHEN x.value IS NULL OR NOT isfinite(x.value) THEN 'invalid_current_state'
                      ELSE 'valid' END AS reason,
                 (g.feature_scope='{SCOPE_PRICE_LINE}' OR g.leg_reason='valid')
                   AND g.cohort_reason<>'overlapping_membership' AS kept
          FROM grid g LEFT JOIN long x ON x.security_id=g.security_id AND x.feature_id=g.feature_id
        )
        SELECT decision_date AS formation_date, security_id, feature_id, metric_code,
               '{MARKET_WINDOW}' AS metric_window,
               CAST(CASE WHEN reason='valid' THEN value END AS DOUBLE) AS raw_value,
               reason, CAST(NULL AS VARCHAR) AS lineage_status,
               CASE WHEN kept THEN available_at END AS available_at,
               CASE WHEN kept THEN available_at END AS latest_input_clock,
               CAST(NULL AS DATE) AS period_end, CAST(NULL AS DATE) AS fiscal_period_start,
               CAST(NULL AS DATE) AS fiscal_period_end, CASE WHEN kept THEN value_origin END AS value_origin,
               CASE WHEN kept AND has_row THEN CAST(0 AS INTEGER) END AS age_days,
               CAST(NULL AS INTEGER) AS max_age_days, owner_cik,
               CAST(NULL AS VARCHAR) AS derived_value_id,
               CAST(NULL AS VARCHAR) AS derived_owner_security_id,
               CAST(NULL AS VARCHAR) AS lineage_digest,
               CASE WHEN feature_scope='{SCOPE_PRICE_LINE}' THEN '{PRICE_LINE_IDENTITY_BASIS}'
                    ELSE identity_basis END AS identity_basis,
               ? AS universe_basis, feature_availability_basis AS availability_basis, feature_scope,
               CASE WHEN kept AND size_feature THEN shares_source END AS shares_source,
               CASE WHEN kept AND size_feature AND has_row AND shares_source IS NOT NULL THEN
                    CASE WHEN shares_source IN ({_sql_list(VERIFIED_SHARES_SOURCES)}) THEN '{SIZE_VERIFIED}'
                         ELSE '{UNVERIFIED_VENDOR_SHARES}' END END AS size_status
        FROM judged
    """, [spec["universe_id"]])
    selected = {(row.formation_date, feature): int(n) for feature, n in con.execute("""
        SELECT feature_id, count(*) FILTER (WHERE available_at IS NOT NULL) FROM _rp_batch_values GROUP BY 1
    """).fetchall()}
    return {"unmatched": {}, "selected": selected}


# ---------------------------------------------------------------------------
# Builder: writes, finalize
# ---------------------------------------------------------------------------

def _write_batch(store: ResearchStore, run_id: str, row: CalendarRow, batch: _Batch,
                 result: dict[str, Any], cohort: dict[str, Any], scopes: dict[str, str]) -> None:
    con = store.con
    day = row.formation_date
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_formation_values AS "
                "SELECT * FROM _rp_batch_values WHERE formation_date=?", [day])
    leaks = con.execute(f"""
        SELECT count(*) FROM _rp_formation_values v JOIN _rp_calendar c ON c.decision_date=v.formation_date
        WHERE {_leak_predicate('c.cutoff', 'v.')}
    """).fetchone()[0]
    if leaks:
        raise RuntimeError(f"{leaks} staged values are not visible at the formation cutoff")
    digests = dict(con.execute(_VALUE_DIGEST_SQL.format(relation="_rp_formation_values", where="")).fetchall())
    emitted = dict(con.execute("SELECT feature_id, count(*) FROM _rp_formation_values GROUP BY 1").fetchall())
    reasons: dict[str, dict[str, int]] = {}
    for feature_id, reason, n in con.execute(
            "SELECT feature_id, reason, count(*) FROM _rp_formation_values GROUP BY ALL ORDER BY ALL").fetchall():
        reasons.setdefault(feature_id, {})[reason] = int(n)
    con.execute(f"INSERT INTO research_panel_values (run_id,{','.join(_VALUE_COLUMNS)}) "
                f"SELECT ?,{','.join(_VALUE_COLUMNS)} FROM _rp_formation_values", [run_id])
    coverage = []
    for feature in batch.features:
        counts = reasons.get(feature.feature_id, {})
        values_valid = int(counts.get("valid", 0))
        coverage.append([
            run_id, day, feature.feature_id, feature.metric_code, feature.metric_window,
            batch.ordinal, "formed" if values_valid else "no_valid_values", cohort["eligible"], cohort["valid"],
            int(result["selected"].get((day, feature.feature_id), 0)), int(emitted.get(feature.feature_id, 0)),
            values_valid, int(result["unmatched"].get((day, feature.feature_id), 0)), _canonical(counts),
            digests.get(feature.feature_id, _EMPTY_SHA), scopes[feature.feature_id],
        ])
    _insert_coverage(con, coverage)


def _insert_coverage(con: Any, rows: list[list[Any]]) -> None:
    con.executemany(f"""
        INSERT INTO research_panel_coverage ({','.join(_COVERAGE_COLUMNS)})
        VALUES ({','.join('?' * len(_COVERAGE_COLUMNS))})
    """, rows)


def _panel_digest(con: Any, run_id: str, manifest: Sequence[Any]) -> str:
    calendar_sha = _stream_digest(con, f"""
        SELECT month_start,expected_session,last_observed_session,formation_date,cutoff,
               entry_date,status,{','.join(_CALENDAR_STAT_COLUMNS)}
        FROM research_panel_calendar WHERE run_id=? ORDER BY month_start
    """, [run_id])
    coverage_sha = _stream_digest(con, f"""
        SELECT {','.join(_COVERAGE_COLUMNS[1:5] + _COVERAGE_COLUMNS[6:])}
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
    scopes = {item[0]: item[3] for item in spec["features"]}
    native_sources = _native_sources(store, features)
    work_spec = {**spec, "scopes": scopes, "native_sources": native_sources}
    definitions_json, definitions_sha, hashes = _definitions(con, features, native_sources)
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
    phases: dict[str, float] = {}
    started = time.perf_counter()
    if basis == BASIS_RECONSTRUCTED:
        if owner_links is None:
            rows, summary = _owner_bridge_rows(store)
        else:
            rows, summary = tuple(owner_links), None
        staged = stage_owner_links(con, rows)
        bridge_json = _canonical({"staged": staged, "bridge_summary": summary})
    phases["owner_bridge"] = time.perf_counter() - started
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_features "
                "(feature_id VARCHAR, metric_code VARCHAR, metric_window VARCHAR)")
    con.executemany("INSERT INTO _rp_features VALUES (?,?,?)",
                    [[f.feature_id, f.metric_code, f.metric_window] for f in features])
    now = dt.datetime.now(dt.UTC).replace(tzinfo=None)
    existing = con.execute("""
        SELECT status,spec_sha256,definitions_sha256,code_sha256,calendar_sha256,source_ids_json,
               owner_bridge_json,query_version
        FROM research_panel_runs WHERE run_id=?
    """, [options.run_id]).fetchone()
    with store.transaction():
        if existing is not None:
            if not options.resume:
                raise ValueError(f"run_id already exists: {options.run_id}")
            if existing[0] not in ("building", "failed"):
                raise ValueError(f"run {options.run_id} is sealed ({existing[0]}); it cannot be resumed")
            if tuple(existing[1:]) != (spec_sha, definitions_sha, code_sha, calendar_sha, source_json,
                                       bridge_json, QUERY_VERSION):
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
    formed = [row for row in calendar_rows if row.status == CALENDAR_FORMED]
    batch_stats: list[dict[str, Any]] = []
    try:
        started = time.perf_counter()
        _stage_run_calendar(con, formed)
        cohorts = _stage_cohorts(store, spec, labels["market_source"]) if formed else {}
        _persist_cohorts(store, options.run_id, formed, cohorts)
        phases["cohorts"] = time.perf_counter() - started
        done = {(row[0], row[1]) for row in con.execute(
            "SELECT formation_date, feature_id FROM research_panel_coverage WHERE run_id=?",
            [options.run_id]).fetchall()}
        ordinals = {row.formation_date: index for index, row in enumerate(formed)}
        for batch in batches:
            pending = [row for row in formed
                       if not all((row.formation_date, f.feature_id) in done for f in batch.features)]
            live = [row for row in pending if cohorts[row.formation_date]["eligible"]]
            for row in pending:
                if row not in live:
                    with store.transaction():
                        _insert_coverage(con, [[
                            options.run_id, row.formation_date, f.feature_id, f.metric_code, f.metric_window,
                            batch.ordinal, "empty_common_cohort", 0, 0, 0, 0, 0, 0, _canonical({}), _EMPTY_SHA,
                            scopes[f.feature_id]] for f in batch.features])
            if not live:
                continue
            stats: dict[str, Any] = {"batch": batch.ordinal, "features": len(batch.features)}
            if not batch.is_market:
                stats.update(_derived_selection(store, options.run_id, batch, work_spec, hashes, len(formed),
                                                options.proof_chunk_roots))
            started = time.perf_counter()
            if batch.is_market:
                for row in live:
                    with store.transaction():
                        result = _market_formation(store, batch, work_spec, labels["market_source"], row)
                        _write_batch(store, options.run_id, row, batch, result, cohorts[row.formation_date], scopes)
            else:
                for start in range(0, len(live), options.formation_chunk):
                    part = live[start:start + options.formation_chunk]
                    with store.transaction():
                        result = _derived_formations(store, options.run_id, batch, work_spec, part,
                                                     [ordinals[row.formation_date] for row in part])
                        for row in part:
                            _write_batch(store, options.run_id, row, batch, result, cohorts[row.formation_date],
                                         scopes)
            stats["formation_seconds"] = round(time.perf_counter() - started, 3)
            batch_stats.append(stats)
        return _finalize(store, options.run_id, basis, calendar_rows,
                         [QUERY_VERSION, spec_sha, definitions_sha, code_sha, calendar_sha,
                          source_json, bridge_json], {"phase_seconds": {k: round(v, 3) for k, v in phases.items()},
                                                      "batches": batch_stats},
                         _excluded_seed_counts())
    except Exception as exc:
        with store.transaction():
            con.execute("""
                UPDATE research_panel_runs SET status='failed',diagnostic_json=?
                WHERE run_id=? AND status='building'
            """, [_canonical({"error": type(exc).__name__, "message": str(exc),
                              "this_invocation": {"batches": batch_stats}}), options.run_id])
        raise


def _excluded_seed_counts() -> dict[str, Any]:
    """Seed metrics of a panel window that the default pull skipped, by role (run detail)."""
    pulled = {row.metric_code for row in fsr.default_derived_definitions() if row.window in PANEL_WINDOWS}
    skipped = {code: role for code, role in excluded_seed_metrics().items() if code in pulled}
    return {"by_role": {role: sum(1 for r in skipped.values() if r == role) for role in EXCLUDED_SEED_ROLES},
            "metric_codes": sorted(skipped)}


def _finalize(store: ResearchStore, run_id: str, basis: str, calendar_rows: Sequence[CalendarRow],
              manifest: Sequence[Any], invocation: dict[str, Any],
              excluded_seeds: dict[str, Any] | None = None) -> ResearchPanelResult:
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
    attrition = con.execute("""
        SELECT formation_date, eligible_members, owner_unlinked_members FROM research_panel_calendar
        WHERE run_id=? AND status='formed' ORDER BY formation_date
    """, [run_id]).fetchall()
    eligible = sum(int(row[1] or 0) for row in attrition)
    unlinked = sum(int(row[2] or 0) for row in attrition)
    shares = [int(row[2]) / int(row[1]) for row in attrition if row[1]]
    proofs: dict[str, dict[str, int]] = {"method": {}, "status": {}}
    for method, proof_status, n in con.execute("""
        SELECT method, status, count(*) FROM research_lineage_proofs WHERE run_id=? GROUP BY ALL ORDER BY ALL
    """, [run_id]).fetchall():
        proofs["method"][method] = proofs["method"].get(method, 0) + int(n)
        proofs["status"][proof_status] = proofs["status"].get(proof_status, 0) + int(n)
    con.execute("CHECKPOINT")
    diagnostic = {"months": len(calendar_rows), "formations": len(formed),
                  "calendar_status": dict(sorted(status_counts.items())),
                  "cohort_rows": int(cohort_rows), "valid_cohort_rows": int(valid_members),
                  "coverage_rows": int(coverage_rows), "value_rows": int(value_rows),
                  "valid_values": int(valid_values), "lineage_proofs": sum(proofs["method"].values()),
                  "lineage_proofs_by_method": proofs["method"], "lineage_proofs_by_status": proofs["status"],
                  "owner_link_attrition": {
                      "unlinked_member_formations": unlinked, "eligible_member_formations": eligible,
                      "share": round(unlinked / eligible, 9) if eligible else None,
                      "max_formation_share": round(max(shares), 9) if shares else None,
                      "by_formation": {row[0].isoformat(): (round(int(row[2]) / int(row[1]), 9) if row[1] else None)
                                       for row in attrition}},
                  "research_db_bytes": store.path.stat().st_size if store.path.is_file() else None,
                  "excluded_seed_metrics": excluded_seeds or {},
                  "this_invocation": invocation}
    blockers = _blockers(basis, len(formed), int(valid_members), int(valid_values), calendar_rows,
                         unlinked, eligible)
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
# One formation through the same builder (public; the P7 cross-section screen)
# ---------------------------------------------------------------------------

#: Temp relations a staged formation fills on the store's connection: the cohort
#: (one row per visible member, keyed by ``decision_date``, with ``eligible``,
#: ``cohort_reason``, ``leg_reason``, ``primary_line`` ...) and, for the batch
#: :func:`formation_batches` last yielded, the value rows (the panel's value columns).
FORMATION_COHORT_RELATION = "_rp_cohort_all"
FORMATION_VALUES_RELATION = "_rp_batch_values"


@dataclass(frozen=True)
class StagedFormation:
    """One formation staged by :func:`stage_formation`.

    ``cohort`` is the formation's statistics (``visible``, ``eligible``, ``valid``,
    ``unlinked``, ``multi_line_issuers``, ``attrition``, ``reasons``), as the panel
    stores them in ``research_panel_calendar``; ``spec`` is the panel spec and
    ``scopes`` maps each feature to ``price_line`` / ``owner``. ``cutoff`` is naive UTC.
    ``cohort_sha256`` is the staged cohort's digest, re-checked before every batch;
    ``native_sources`` is where each P2 native reads from in this formation.
    """

    run_id: str
    basis: str
    formation_date: dt.date
    cutoff: dt.datetime
    features: tuple[PanelFeature, ...]
    scopes: dict[str, str]
    spec: dict[str, Any]
    labels: dict[str, str]
    cohort: dict[str, Any]
    owner_bridge: dict[str, Any] | None
    metric_batch_size: int
    cohort_sha256: str = ""
    native_sources: dict[str, str] = field(default_factory=dict)

    @property
    def members(self) -> int:
        return int(self.cohort["visible"])

    @property
    def eligible(self) -> int:
        return int(self.cohort["eligible"])

    @property
    def calendar_row(self) -> CalendarRow:
        return CalendarRow(self.formation_date.replace(day=1), self.formation_date, self.formation_date,
                           self.formation_date, self.cutoff, None, CALENDAR_FORMED)


@dataclass(frozen=True)
class FormationBatch:
    """One metric batch of a staged formation.

    Its rows are in :data:`FORMATION_VALUES_RELATION` until the generator resumes.
    ``values_sha256`` is the per-feature value digest, the same as a panel run's
    ``research_panel_coverage.values_sha256`` for that formation.
    """

    ordinal: int
    feature_ids: tuple[str, ...]
    is_market: bool
    values_sha256: dict[str, str]
    definitions_sha256: str
    stats: dict[str, Any]


def stage_formation(store: ResearchStore, *, basis: str, formation_date: dt.date, cutoff: dt.datetime,
                    features: Sequence[PanelFeature | dict[str, Any]] | None = None,
                    run_id: str = "formation_one", max_age_days: int = DEFAULT_MAX_AGE_DAYS,
                    annual_max_age_days: int = DEFAULT_ANNUAL_MAX_AGE_DAYS,
                    eligible_security_types: tuple[str, ...] = fsr.RECONSTRUCTED_ELIGIBLE_TYPES,
                    include_unlisted_tail: bool = True, unverified_vendor_shares: bool = False,
                    metric_batch_size: int = 16, owner_links: Sequence[Any] | None = None) -> StagedFormation:
    """Stage one formation's owner links, calendar and cohort with the panel's own rules.

    ``formation_date`` is the session the market features read (normally an
    observed session, :func:`observed_sessions`). ``cutoff`` is the timezone-aware
    UTC knowledge cutoff for every input, on or after that session. The spec,
    cohort, primary lines and, through :func:`formation_batches`, the values and
    digests are what :func:`build_research_panel` produces for a formation with
    this session and cutoff. ``features=None`` is the default panel feature set;
    ``()`` stages the cohort only. ``owner_links`` overrides the reconstructed
    bridge, as in the panel.

    Derived batches prove lineage into the store's ``research_lineage_proofs``
    under ``run_id``. Proofs do not depend on the cutoff and are reused.
    ``run_id`` must not name a panel run of the store, so a real run's proofs are
    never touched. They are not removed afterwards: a *later*
    :func:`build_research_panel` with the same id would adopt them (they are per
    root and deterministic, so its values are unaffected, but its proof table then
    holds rows it did not prove). Screens should use a scratch store (R2e m4).
    """
    if type(formation_date) is not dt.date:
        raise ValueError("formation_date must be a date")
    if _utc_naive(cutoff, "cutoff").date() < formation_date:
        raise ValueError("cutoff precedes the formation session")
    requested = None if features is None else tuple(features)
    cohort_only = requested == ()
    month = formation_date.replace(day=1)
    options = ResearchPanelOptions(
        run_id=run_id, basis=basis, start_month=month, end_month=month, as_of_date=formation_date,
        run_at=cutoff, features=None if cohort_only else requested, metric_batch_size=metric_batch_size,
        max_age_days=max_age_days, annual_max_age_days=annual_max_age_days,
        eligible_security_types=tuple(eligible_security_types), include_unlisted_tail=include_unlisted_tail,
        unverified_vendor_shares=unverified_vendor_shares)
    run_at, checked, spec = _validate(options)
    staged_features: tuple[PanelFeature, ...] = () if cohort_only else checked
    if cohort_only:
        spec = {**spec, "features": [], "size_policy": {**spec["size_policy"], "size_features": []}}
    con = store.con
    if con.execute("SELECT count(*) FROM research_panel_runs WHERE run_id=?", [run_id]).fetchone()[0]:
        raise ValueError(f"run_id {run_id!r} names a panel run of this store; use a formation-only id")
    _require_inputs(store, basis, staged_features, build_bridge=owner_links is None)
    labels = _basis_labels(basis)
    bridge = None
    if basis == BASIS_RECONSTRUCTED:
        if owner_links is None:
            rows, summary = _owner_bridge_rows(store)
        else:
            rows, summary = tuple(owner_links), None
        bridge = {"staged": stage_owner_links(con, rows), "bridge_summary": summary}
    _stage_run_calendar(con, [CalendarRow(month, formation_date, formation_date, formation_date, run_at, None,
                                          CALENDAR_FORMED)])
    cohort = _stage_cohorts(store, spec, labels["market_source"]).get(formation_date) or _empty_cohort_stats()
    cohort_sha = con.execute(_COHORT_DIGEST_SQL.format(relation=FORMATION_COHORT_RELATION,
                                                       where="WHERE decision_date=?"), [formation_date]).fetchone()[0]
    return StagedFormation(
        run_id=run_id, basis=basis, formation_date=formation_date, cutoff=run_at, features=staged_features,
        scopes={item[0]: item[3] for item in spec["features"]}, spec=spec, labels=labels, cohort=cohort,
        owner_bridge=bridge, metric_batch_size=metric_batch_size, cohort_sha256=str(cohort_sha),
        native_sources=_native_sources(store, staged_features))


def _check_staged(con: Any, staged: StagedFormation) -> None:
    """Refuse to compute values over a cohort or calendar other than the staged ones (R2e m3).

    Another staging on the same connection (for example the same session at a later
    cutoff, whose cohort may admit links not yet visible at this one) replaces the
    shared temp relations; the cohort digest and the one-row calendar must still be
    this formation's.
    """
    visible, first, last = con.execute(
        f"SELECT count(*), min(decision_date), max(decision_date) FROM {FORMATION_COHORT_RELATION}").fetchone()
    digest = con.execute(_COHORT_DIGEST_SQL.format(relation=FORMATION_COHORT_RELATION, where="WHERE decision_date=?"),
                         [staged.formation_date]).fetchone()[0]
    calendar = con.execute("SELECT count(*), max(decision_date), max(cutoff) FROM _rp_calendar").fetchone()
    if (int(visible), first, last) != (staged.members, staged.formation_date, staged.formation_date) \
            or digest != staged.cohort_sha256 or tuple(calendar) != (1, staged.formation_date, staged.cutoff):
        raise RuntimeError("the staged cohort or calendar was replaced on this connection; stage the formation again")


def formation_batches(store: ResearchStore, staged: StagedFormation, *,
                      proof_chunk_roots: int = DEFAULT_PROOF_CHUNK_ROOTS) -> Iterator[FormationBatch]:
    """Compute a staged formation's values batch by batch, with the panel's batches and SQL.

    Each yielded batch's rows are in :data:`FORMATION_VALUES_RELATION` until the
    generator resumes. Before every batch the staged cohort and calendar are
    re-checked (a re-staging raises). Every row is checked against the cutoff, and a
    row knowable only later raises. Nothing is yielded without features or eligible
    members.
    """
    if isinstance(proof_chunk_roots, bool) or not isinstance(proof_chunk_roots, int) \
            or not 1 <= proof_chunk_roots <= 1_000_000:
        raise ValueError("proof_chunk_roots must be 1..1000000")
    if not staged.features or not staged.eligible:
        return
    con = store.con
    row = staged.calendar_row
    _stage_run_calendar(con, [row])
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_features "
                "(feature_id VARCHAR, metric_code VARCHAR, metric_window VARCHAR)")
    con.executemany("INSERT INTO _rp_features VALUES (?,?,?)",
                    [[f.feature_id, f.metric_code, f.metric_window] for f in staged.features])
    _, definitions_sha, hashes = _definitions(con, staged.features, staged.native_sources)
    work_spec = {**staged.spec, "scopes": staged.scopes, "native_sources": staged.native_sources}
    for batch in _batches(staged.features, staged.metric_batch_size):
        _check_staged(con, staged)
        started = time.perf_counter()
        stats: dict[str, Any] = {}
        if batch.is_market:
            _market_formation(store, batch, work_spec, staged.labels["market_source"], row)
        else:
            stats.update(_derived_selection(store, staged.run_id, batch, work_spec, hashes, 1, proof_chunk_roots))
            _derived_formations(store, staged.run_id, batch, work_spec, [row], [0])
        leaks = con.execute(f"""
            SELECT count(*) FROM {FORMATION_VALUES_RELATION} v
            JOIN _rp_calendar c ON c.decision_date=v.formation_date
            WHERE {_leak_predicate('c.cutoff', 'v.')}
        """).fetchone()[0]
        if leaks:
            raise RuntimeError(f"{leaks} staged values are not visible at the formation cutoff")
        digests = dict(con.execute(_VALUE_DIGEST_SQL.format(relation=FORMATION_VALUES_RELATION, where="")).fetchall())
        stats["seconds"] = round(time.perf_counter() - started, 3)
        yield FormationBatch(batch.ordinal, tuple(f.feature_id for f in batch.features), batch.is_market,
                             {f.feature_id: digests.get(f.feature_id, _EMPTY_SHA) for f in batch.features},
                             definitions_sha, stats)


# ---------------------------------------------------------------------------
# Validator
# ---------------------------------------------------------------------------

def validate_research_panel(store: ResearchStore, run_id: str) -> ResearchPanelValidation:
    """Reject unsealed, changed or point-in-time-unsafe panels before any use.

    Bounded like the build: every check runs per formation and per chunk of at
    most 16 features.
    """
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
               status,cohort_sha256,eligible_members
        FROM research_panel_calendar WHERE run_id=? ORDER BY month_start
    """, [run_id]).fetchall()
    rebuilt = _sha(_canonical([[_iso(v) for v in item[:7]] for item in calendar]))
    if rebuilt != calendar_sha:
        raise ValueError("research panel calendar digest mismatch")
    formed = [item for item in calendar if item[6] == CALENDAR_FORMED]
    for item in formed:
        formation, cutoff, eligible = item[3], item[4], int(item[8] or 0)
        if item[3] != item[1] or cutoff != dt.datetime.combine(item[1], dt.time(DECISION_HOUR)) \
                or item[5] is None or item[5] <= item[3]:
            raise ValueError("research panel formation calendar contract changed")
        digest = con.execute(_COHORT_DIGEST_SQL.format(
            relation="research_panel_cohort", where="WHERE run_id=? AND formation_date=?"),
            [run_id, formation]).fetchone()[0]
        if digest != item[7]:
            raise ValueError(f"research panel cohort digest mismatch at {formation}")
        stored = {feature: (sha, int(emitted), int(members)) for feature, sha, emitted, members in con.execute("""
            SELECT feature_id, values_sha256, values_emitted, eligible_members FROM research_panel_coverage
            WHERE run_id=? AND formation_date=?
        """, [run_id, formation]).fetchall()}
        if set(stored) != set(features):
            raise ValueError(f"research panel coverage incomplete at {formation}")
        rebuilt_values: dict[str, str] = {}
        grain: dict[str, tuple[int, int, int]] = {}
        for start in range(0, len(features), 16):
            chunk = features[start:start + 16]
            where = f"WHERE run_id=? AND formation_date=? AND feature_id IN ({','.join('?' * len(chunk))})"
            rebuilt_values.update(con.execute(_VALUE_DIGEST_SQL.format(
                relation="research_panel_values", where=where), [run_id, formation, *chunk]).fetchall())
            cutoff_sql = f"TIMESTAMP '{cutoff.isoformat(sep=' ')}'"
            for feature, rows, distinct, leaks in con.execute(f"""
                SELECT feature_id, count(*), count(DISTINCT security_id),
                       count(*) FILTER (WHERE {_leak_predicate(cutoff_sql)})
                FROM research_panel_values {where} GROUP BY feature_id
            """, [run_id, formation, *chunk]).fetchall():
                grain[feature] = (int(rows), int(distinct), int(leaks))
        stray = con.execute("""
            SELECT count(*) FROM research_panel_values WHERE run_id=? AND formation_date=?
        """, [run_id, formation]).fetchone()[0] - sum(value[1] for value in stored.values())
        if stray or any(rebuilt_values.get(feature, _EMPTY_SHA) != stored[feature][0] for feature in features):
            raise ValueError(f"research panel value digest mismatch at {formation}")
        for feature in features:
            rows, distinct, leaks = grain.get(feature, (0, 0, 0))
            if leaks or rows != distinct or rows != stored[feature][1] or stored[feature][2] != eligible \
                    or rows != eligible:
                raise ValueError(f"research panel point-in-time or grain contract violated at {formation}")
    orphans = con.execute("""
        SELECT count(*) FROM research_panel_values v
        ANTI JOIN (SELECT formation_date FROM research_panel_calendar WHERE run_id=? AND status='formed') c
          ON c.formation_date=v.formation_date
        WHERE v.run_id=?
    """, [run_id, run_id]).fetchone()[0]
    if orphans:
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
    "FORMATION_COHORT_RELATION",
    "FORMATION_VALUES_RELATION",
    "FUNDAMENTAL_AVAILABILITY_BASIS",
    "MARKET_AVAILABILITY_BASIS",
    "MARKET_REVISION_RULE",
    "NATIVE_FEATURES",
    "OWNER_LINK_FAILURES",
    "PRICE_LINE_IDENTITY_BASIS",
    "QUERY_VERSION",
    "SCOPE_OWNER",
    "SCOPE_PRICE_LINE",
    "SECONDARY_LINE_REASON",
    "SIZE_VERIFIED",
    "UNVERIFIED_VENDOR_SHARES",
    "VENDOR_SHARES_AVAILABILITY_BASIS",
    "VERIFIED_SHARES_SOURCES",
    "CalendarRow",
    "FormationBatch",
    "PanelFeature",
    "ResearchPanelOptions",
    "ResearchPanelResult",
    "ResearchPanelValidation",
    "StagedFormation",
    "build_research_panel",
    "canonical_features",
    "default_panel_features",
    "expected_month_end_session",
    "feature_scopes",
    "formation_batches",
    "month_end_calendar",
    "nyse_full_day_closures",
    "observed_sessions",
    "price_line_codes",
    "size_codes",
    "stage_formation",
    "stage_owner_links",
    "validate_research_panel",
]
