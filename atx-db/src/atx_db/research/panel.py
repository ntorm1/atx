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

Size (R2d). A share count is read only at its own availability clock, at or
before the cutoff, never at the bar date: the vendor starts a share run on the
DEI cover date, before the filing is public. ``line_market_cap`` (own close x own
vendor count; opt-in with ``unverified_vendor_shares=True``) takes the count from
A8's vendor share relation (``market_daily.vendor_share_state_query``): the
latest run known at the session's cutoff, folded into the row's clock
(``latest_input_clock``) and named in ``availability_basis``
(``vendor_share_run_clock:<dei_matched|split_derived|first_run|modeled_lag>``;
``modeled_lag`` and ``first_run`` are modeled, not verified). An ADR line's count
is an ADS count, used only when the ADS ratio is known (a line whose bridge row
states an ADS basis for the session but is not yet visible is ``adr_ratio_unknown``).
A withheld count is NULL with its label as the reason for ``line_market_cap`` and
turnover: for ``line_market_cap`` the labels it derives from the A8 relation
(``archive_run_pending``, ``split_pending_share_update``, ``adr_ratio_unknown``,
``vendor_shares_zero``, ``bar_price_invalid``) and market_daily's basis verdict on
the same line and session (:data:`LINE_CAP_BASIS_VERDICTS`: ``adr_ratio_unresolved``,
``dei_archive_conflict``: the line's vendor count is in the wrong basis); for
turnover every withheld label of its market_daily row. A
market_daily size feature (``market_cap`` and what reads it) is NULL with the
label as its ``shares_source`` and keeps ``invalid_current_state``, so its R2a
digests are unchanged. When market_daily records a row's share clock (0328
``shares_clock``/``shares_available_at``), it is folded into the size row's clock
and named in its basis. Every size row carries ``shares_source`` and
``size_status`` (``verified_dei_shares`` only for DEI counts, else
``unverified_vendor_shares``); the spec's ``size_policy`` tells R2b to use only
verified size.

Foreign filers (P11). A valid owner with no metric state at a formation whose
CIK is inside a ``foreign_filers`` reason interval at the cutoff
(``ifrs_reporter_not_standardized``, ``foreign_filer_no_xbrl_financials``) carries
that reason instead of ``missing_metric_state``: a label, never a dropped row or
value. The intervals are read from the ``foreign_filer_reason_intervals``
relation of the connection (:func:`stage_foreign_filer_reasons` builds it from
the verified taxonomy artifacts); the spec records their source and digest, or
``not_supplied``.
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
dollar volume of the primary-line rule and turnover's share count. Bars follow
market_daily's one bar rule (``market_daily._bars_by_session_sql``): the rows of
one ``(security_id, trade_date)`` are ranked by ``(available_at, source)`` and
every column comes from the newest row visible at the cutoff, a NULL included
(``arg_max_null``); the price is checked after the pick, so a session whose newest
row has no positive close and adjusted close has no price (never an older row's).

Price/liquidity natives (P2, fix round 1). Identity-free (``price_line``) for
every eligible line: ``amihud_illiquidity_21d``, ``pct_from_high_252d``,
``max_daily_return_21d`` and ``downside_deviation_60d``. ``turnover_21d`` is
owner-scoped (volume over the issuer's verified DEI share count). They are
computed from the line's own bars only (never ``equity_price_metrics``, whose
row-count windows are another definition), with VA1's ``vendor_artifact_repaired``
adjusted close (a vendor factor decrease that is not a split, such as the 2021-01-04
step, is neutralized: v8), on the XNYS session calendar: a
window is the last N rule sessions ending at the formation session, a line
*observes* a session when it has a valid-price bar on it, and a daily return
exists only between two consecutive sessions both observed. A line whose history
does not reach the window's first session (for returns, the session before it)
is ``insufficient_history``; one with fewer observations than the native's
declared minimum (``min_observed``: the literature's monthly minimums, 15 valid
daily returns of 21 for MAX and Amihud, 45 of 60 for downside deviation, 200
observed prices of 252 for the 52-week high; 90 % of turnover's 21 sessions) is
``window_gaps``; Amihud averages positive-volume days and is
``zero_volume_in_window`` below its minimum. Turnover
is NULL with ``unverified_shares`` (a vendor count), the A8 withheld label (a
withheld count) or ``split_in_price_window`` (an exact split or stock-dividend
ratio by R1d's classifier inside its window). Only bars whose clock is at or
before the cutoff are read.

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

from .. import _split_epochs, _vendor_artifact
from .. import calendar as _xnys
from .. import derived_lineage as _derived_lineage
from .. import fundamental_signal_research as fsr
from .. import market_daily as _market_daily
from .. import market_owner_bridge as _market_owner_bridge
from .._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY

# The canonical XNYS calendar (1.11) lives in ``atx_db.calendar``; re-exported here.
from ..calendar import (
    decision_cutoff_utc,
    expected_month_end_session,
    nyse_full_day_closures,
    xnys_sessions,
)
from ..derived_registry import DERIVED_SOURCE_NAME
from ..market_daily import MARKET_DAILY_SOURCE_NAME, MARKET_DAILY_STRICT_SOURCE_NAME, SHARES_SOURCES_WITHHELD
from ..universe_us_listed import UNIVERSE_SOURCE_NAME
from . import catalog as _catalog
from . import lineage as _lineage
from . import store as _store
from .store import ResearchStore

# v4 (R2e): the newest visible market revision wins even when NULL. Pre-v4 runs can
# carry an older revision's value under a newer clock; the validator refuses them.
# v5 (P2): price/liquidity natives. Existing features' value digests are unchanged.
# v6 (P2 fix round 1): the natives' windows are XNYS session windows over bars read
# with market_daily's one bar rule (arg_max_null); equity_price_metrics is never read.
# v7 (R2d): shares at their own availability clock (A8 vendor share relation), ADR
# ratio gate, withheld share labels as reasons, P11 foreign-filer reasons.
# v8 (VA1 + R2d fix 1): the P2 natives' bar returns read the vendor_artifact_repaired
# adjusted close; line_market_cap withholds on market_daily's basis verdicts and on an
# ADS bridge interval not visible yet; line share state chunked by bar count.
QUERY_VERSION = "research-monthly-pit-panel-v8"
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
DECISION_HOUR = _xnys.DECISION_HOUR_UTC
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
# Size verification: only a DEI count is verified; vendor counts (every A8 run
# clock, the modeled ones included) are not.
UNVERIFIED_VENDOR_SHARES = "unverified_vendor_shares"
#: ``line_market_cap``'s availability basis: the A8 vendor share-run clock. A row with a
#: known run is labeled ``vendor_share_run_clock:<clock kind>``.
VENDOR_SHARES_AVAILABILITY_BASIS = "vendor_share_run_clock"
VERIFIED_SHARES_SOURCES = ("dei",)
SIZE_VERIFIED = "verified_dei_shares"
#: ``line_market_cap``'s ``shares_source``: A8's vocabulary (``archive`` a line's vendor
#: count, ``archive_ads`` an ADR line's ADS count with a known ratio).
LINE_SHARES_SOURCE = "archive"
LINE_ADS_SHARES_SOURCE = "archive_ads"
#: market_daily verdicts on a line's vendor count *basis* that withhold ``line_market_cap``
#: for that line and session (R2d review I1): ADS count x ratio disagrees with the DEI
#: count, or DEI and the vendor count disagree (an issuer total on a class line, an
#: ordinary count on an ADS line, a unit error). market_daily's other withheld labels
#: judge the issuer's count (``multiclass_unresolved``, ``split_unresolved``) or repeat what
#: the panel derives from the A8 relation itself, so they do not withhold a line's own cap.
LINE_CAP_BASIS_VERDICTS = ("adr_ratio_unresolved", "dei_archive_conflict")
#: At most this many eligible lines per :func:`market_daily.vendor_share_state_query` call.
LINE_SHARES_CHUNK = 1000
#: Bars per call at a 384 MB DuckDB memory limit (R2d review m1: 1.0M and 1.6M bars ran at
#: ~0.5 GB process peak, 2.5M ran out of memory); scaled linearly with the connection's limit.
LINE_SHARES_BARS_AT_384MB = 1_000_000
SHARE_CLOCK_POLICY = ("a share count is read at its own availability clock at or before the cutoff: DEI at its "
                      "filing, a vendor count at the A8 run clock (vendor_share_state_query; modeled_lag and "
                      "first_run are modeled, not verified); the clock is folded into latest_input_clock; a "
                      "withheld count is NULL with its A8 label")
# P11 foreign-filer reasons (``foreign_filers``; not imported: that module loads pandas
# and the archive readers). They label a valid owner's missing_metric_state rows.
FOREIGN_FILER_INTERVALS_RELATION = "foreign_filer_reason_intervals"
IFRS_REPORTER_REASON = "ifrs_reporter_not_standardized"
NO_XBRL_FINANCIALS_REASON = "foreign_filer_no_xbrl_financials"
FOREIGN_FILER_REASONS = (IFRS_REPORTER_REASON, NO_XBRL_FINANCIALS_REASON)
# P2 price/liquidity natives: where they read from, and their reasons.
BARS_SOURCE = "equity_daily_bars"
#: Calendar days of bars read per formation: 252 XNYS sessions plus holidays, with slack.
PRICE_LOOKBACK_DAYS = 400
PRICE_WINDOW_SESSIONS = 252
AMIHUD_SCALE = 1e9
#: Minimum observations per window (below it the value is NULL with ``window_gaps``):
#: the literature's monthly conventions for the return windows (Bali, Cakici and
#: Whitelaw; Amihud: 15 valid daily returns of 21; 45 of 60 for downside deviation;
#: 200 observed prices of 252 for the 52-week high), and this share of the sessions
#: for turnover's volume window.
MIN_DAILY_RETURNS_21D = 15
MIN_DAILY_RETURNS_60D = 45
MIN_OBSERVED_PRICES_252D = 200
MIN_OBSERVED_SHARE = 0.9
INSUFFICIENT_HISTORY_REASON = "insufficient_history"
WINDOW_GAPS_REASON = "window_gaps"
ZERO_VOLUME_REASON = "zero_volume_in_window"
UNVERIFIED_SHARES_REASON = "unverified_shares"
#: Turnover divides 21 sessions of volume by today's share count: a split or stock
#: dividend inside the window mixes share bases. A step of the vendor factor
#: adj/close between two consecutive observed bars of the window that R1d's classifier
#: calls an exact ratio (``_split_epochs``: at least a 5 % step; a whole number of
#: half-percent steps inside [0.8, 1.25], else p/q with q <= 10) makes the window NULL
#: with this reason. Share evidence is not waited for (it arrives after the cutoff),
#: so a cash distribution that happens to match an exact ratio also fails closed.
SPLIT_WINDOW_REASON = "split_in_price_window"


def _min_observed(sessions: int) -> int:
    return math.ceil(MIN_OBSERVED_SHARE * sessions - 1e-9)
# Panel-native features (not warehouse metrics). ``scope``: price_line (every
# eligible line) or owner (the issuer's primary line); ``size``: the value reads a
# share count, so rows carry ``shares_source``/``size_status``; ``requires`` names
# the option that must be set to compute one. P2 natives use the line's own bars on
# the XNYS session calendar: ``window_sessions`` rule sessions ending at the
# formation session, ``min_history_sessions`` sessions of history (the window, plus
# the session before it for daily returns) and ``min_observed`` observed sessions
# (daily returns for the return windows).
NATIVE_FEATURES: dict[str, dict[str, Any]] = {
    "line_market_cap": {
        "metric_window": MARKET_WINDOW,
        "expression": "close x the line's vendor share count of the latest A8 share run known at the cutoff; an "
                      "ADR line's ADS count only with a known ADS ratio; NULL with the A8 label when withheld, "
                      "and with market_daily's basis verdict (adr_ratio_unresolved, dei_archive_conflict) for "
                      "the same line and session",
        "inputs": ["equity_daily_bars.close", "equity_daily_bars.adjusted_close",
                   "equity_daily_bars.shares_outstanding", "shares_outstanding_history",
                   "market_owner_bridge.share_basis", "market_owner_bridge.adr_ratio",
                   "market_daily_metrics.shares_source"],
        "version": "3",
        "source_column": "equity_daily_bars.close*market_daily.vendor_share_state_query.pit_shares",
        "unit_basis": "usd_line_market_value",
        "requires": UNVERIFIED_VENDOR_SHARES,
        "scope": SCOPE_PRICE_LINE,
        "size": True,
    },
    "amihud_illiquidity_21d": {
        "metric_window": MARKET_WINDOW,
        "expression": "1e9 * mean(|r_t| / (close_t * volume_t)) over the daily returns of the last 21 XNYS sessions "
                      "with positive volume; >= 15 such days required",
        "inputs": ["equity_daily_bars.adjusted_close", "equity_daily_bars.close", "equity_daily_bars.volume"],
        "adjustment_repair": _vendor_artifact.REPAIR_VERSION,
        "version": "4",
        "unit_basis": "abs_return_per_1e9_dollars_traded",
        "reference": "Amihud (2002)",
        "scope": SCOPE_PRICE_LINE,
        "size": False,
        "window_sessions": 21,
        "min_history_sessions": 22,
        "min_observed": MIN_DAILY_RETURNS_21D,
    },
    "pct_from_high_252d": {
        "metric_window": MARKET_WINDOW,
        "expression": "adj_t / max(adj over the observed sessions of the last 252 XNYS sessions) - 1; >= 252 "
                      "sessions since the first bar and >= 200 observed prices in the window required",
        "inputs": ["equity_daily_bars.adjusted_close", "equity_daily_bars.close"],
        "adjustment_repair": _vendor_artifact.REPAIR_VERSION,
        "version": "4",
        "unit_basis": "fraction",
        "reference": "George and Hwang (2004)",
        "scope": SCOPE_PRICE_LINE,
        "size": False,
        "window_sessions": PRICE_WINDOW_SESSIONS,
        "min_history_sessions": PRICE_WINDOW_SESSIONS,
        "min_observed": MIN_OBSERVED_PRICES_252D,
    },
    "max_daily_return_21d": {
        "metric_window": MARKET_WINDOW,
        "expression": "max(r_t) over the daily returns of the last 21 XNYS sessions; >= 15 returns required",
        "inputs": ["equity_daily_bars.adjusted_close", "equity_daily_bars.close"],
        "adjustment_repair": _vendor_artifact.REPAIR_VERSION,
        "version": "4",
        "unit_basis": "fraction",
        "reference": "Bali, Cakici and Whitelaw (2011)",
        "scope": SCOPE_PRICE_LINE,
        "size": False,
        "window_sessions": 21,
        "min_history_sessions": 22,
        "min_observed": MIN_DAILY_RETURNS_21D,
    },
    "downside_deviation_60d": {
        "metric_window": MARKET_WINDOW,
        "expression": "sqrt(252 * mean(min(r_t, 0)^2)) over the daily returns of the last 60 XNYS sessions; "
                      ">= 45 returns required (zero-target semideviation, LPM2)",
        "inputs": ["equity_daily_bars.adjusted_close", "equity_daily_bars.close"],
        "adjustment_repair": _vendor_artifact.REPAIR_VERSION,
        "version": "4",
        "unit_basis": "annualized_fraction",
        "reference": "published analogue: downside risk of Ang, Chen and Xing (2006), who price downside beta; "
                     "this is the Sortino zero-target semideviation",
        "scope": SCOPE_PRICE_LINE,
        "size": False,
        "window_sessions": 60,
        "min_history_sessions": 61,
        "min_observed": MIN_DAILY_RETURNS_60D,
    },
    "turnover_21d": {
        "metric_window": MARKET_WINDOW,
        "expression": "mean(volume over the observed sessions of the last 21 XNYS sessions) / verified (DEI) "
                      "shares outstanding; >= 19 sessions required; NULL when an exact split or stock-dividend "
                      "ratio (R1d classifier) lies inside the window",
        "inputs": ["equity_daily_bars.volume", "equity_daily_bars.close", "equity_daily_bars.adjusted_close",
                   "market_daily_metrics.shares_outstanding", "market_daily_metrics.shares_source"],
        "version": "2",
        "unit_basis": "fraction_of_shares_per_day",
        "reference": "Datar, Naik and Radcliffe (1998)",
        # Owner scope: the denominator is the issuer's DEI count (the panel's rule for
        # share inputs); only a single-class line carries shares_source='dei'.
        "scope": SCOPE_OWNER,
        "size": True,
        "window_sessions": 21,
        "min_history_sessions": 21,
        "min_observed": _min_observed(21),
    },
}
#: The natives computed from the P2 bar window (every native but line_market_cap).
PRICE_WINDOW_FEATURES = frozenset(code for code, spec in NATIVE_FEATURES.items() if "window_sessions" in spec)
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

# Every module whose semantics a run's rows depend on: a change between a
# failed run and its resume refuses the resume.
_CODE_FILES = (*(Path(str(module.__file__)) for module in (
    fsr, _derived_lineage, _lineage, _market_owner_bridge, _store, _split_epochs, _market_daily, _vendor_artifact,
    _xnys)), Path(__file__))


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
# NYSE month-end rule (the session rules are ``atx_db.calendar``'s)
# ---------------------------------------------------------------------------

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
        cutoff = decision_cutoff_utc(expected).replace(tzinfo=None)  # naive UTC, as ``run_at``
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
            "share_clock": SHARE_CLOCK_POLICY,
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
        # The A8 vendor share relation reads the bars and the DEI cover counts;
        # market_daily's share verdict of the same line and session can withhold.
        columns += [("equity_daily_bars", c) for c in ("trade_date", "close", "adjusted_close", "volume",
                                                       "shares_outstanding", "available_at", "source", "symbol")]
        columns.append(("market_daily_metrics", "shares_source"))
        columns += [("shares_outstanding_history", c) for c in (
            "security_id", "form", "share_count_type", "taxonomy", "concept", "available_at", "share_count",
            "effective_date")]
    if any(f.metric_code in PRICE_WINDOW_FEATURES for f in features):
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


def _build_bridge(store: ResearchStore, basis: str) -> Any:
    """The A5/A8 market owner bridge of the basis (reconstructed, or strict: dated evidence only)."""
    mode = (_market_owner_bridge.OWNER_MODE_STRICT if basis == BASIS_STRICT
            else _market_owner_bridge.OWNER_MODE_RECONSTRUCTED)
    return _market_owner_bridge.build_market_owner_bridge(store, mode=mode,  # type: ignore[arg-type]
                                                          derived_source=DERIVED_SOURCE_NAME)


def _owner_links(store: ResearchStore, basis: str, owner_links: Sequence[Any] | None,
                 ) -> tuple[dict[str, Any] | None, Any]:
    """Stage the reconstructed owner links; return (bridge accounting, the built bridge or None)."""
    if basis != BASIS_RECONSTRUCTED:
        return None, None
    bridge = None
    if owner_links is None:
        bridge = _build_bridge(store, basis)
        rows, summary = bridge.rows, bridge.summary()
    else:
        rows, summary = tuple(owner_links), None
    return {"staged": stage_owner_links(store.con, rows), "bridge_summary": summary}, bridge


def _share_bridge(store: ResearchStore, basis: str, features: Sequence[PanelFeature], built: Any,
                  supplied: Any) -> Any:
    """The bridge ``line_market_cap`` reads A8's share state through, or None when it is not requested.

    ``supplied`` (a ``MarketOwnerBridge``) wins; else the owner bridge this call built;
    else one is built for the basis (the strict bridge links nothing without dated
    evidence, so every run there has the unknown filer family's lag).
    """
    if not any(f.metric_code == "line_market_cap" for f in features):
        return None
    if supplied is not None:
        return supplied
    return built if built is not None else _build_bridge(store, basis)


_MEMORY_SETTING = re.compile(r"\s*([0-9]+(?:\.[0-9]+)?)\s*([KMGT]?)i?B\s*", re.IGNORECASE)


def _line_share_chunk_bars(con: Any) -> int:
    """Bars per A8 share-state call: :data:`LINE_SHARES_BARS_AT_384MB` scaled to the memory limit."""
    match = _MEMORY_SETTING.fullmatch(str(con.execute("SELECT current_setting('memory_limit')").fetchone()[0]))
    if match is None:
        return LINE_SHARES_BARS_AT_384MB
    limit = float(match.group(1)) * {"": 1, "K": 2 ** 10, "M": 2 ** 20, "G": 2 ** 30,
                                     "T": 2 ** 40}[match.group(2).upper()]
    return max(100_000, int(LINE_SHARES_BARS_AT_384MB * limit / (384 * 2 ** 20)))


def share_bridge_digest(bridge: Any) -> str:
    """Content digest of the rows ``line_market_cap`` reads through the share bridge (R2d m4).

    Per bridge row: line, owner, validity interval, availability, share basis and ADS
    ratio, so a supplied bridge or changed ratios between a failed run and its resume
    change the run's ``source_ids``.
    """
    rows = [[row.price_security_id, row.owner_security_id, _iso(row.valid_from), _iso(row.valid_to),
             _iso(row.available_at), row.share_basis, _iso(row.adr_ratio)] for row in bridge.rows]
    return _sha(_canonical(sorted(rows, key=_canonical)))


def _stage_line_shares(store: ResearchStore, bridge: Any, dates: Sequence[dt.date], *,
                       max_bars: int | None = None) -> dict[str, int]:
    """``_rp_share_state``: A8's vendor share state of every eligible line at each formation session.

    One :func:`market_daily.vendor_share_state_query` per chunk of lines over their bars
    up to the last session (a run's clock and a bar's state use only bars and filings up
    to that bar, so the state at an earlier session is the same), kept only at the
    eligible (line, formation session) pairs. A chunk holds at most ``max_bars`` bars
    (default :func:`_line_share_chunk_bars`, from the connection's memory limit; a line
    with more bars is a chunk of its own) and :data:`LINE_SHARES_CHUNK` lines.
    ``_rp_share_dates`` records the sessions staged; ``_rp_adr_lines`` the bridge
    intervals with an ADS basis (visible or not, for the ratio gate).
    """
    con = store.con
    budget = _line_share_chunk_bars(con) if max_bars is None else int(max_bars)
    if budget < 1:
        raise ValueError("max_bars must be positive")
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_share_dates (decision_date DATE)")
    con.executemany("INSERT INTO _rp_share_dates VALUES (?)", [[day] for day in sorted(set(dates))])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_share_state (
          security_id VARCHAR, trade_date DATE, pit_shares DOUBLE, pit_clock VARCHAR,
          pit_available_at TIMESTAMP, run_pending BOOLEAN, split_pending BOOLEAN, share_basis VARCHAR,
          adr_ratio DOUBLE, adr_ratio_reason VARCHAR)
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_share_pairs AS
        SELECT DISTINCT k.security_id, k.decision_date FROM _rp_cohort_all k
        SEMI JOIN _rp_share_dates d ON d.decision_date=k.decision_date
        WHERE k.eligible
    """)
    counts = con.execute("""
        SELECT l.security_id, count(b.security_id)
        FROM (SELECT DISTINCT security_id FROM _rp_share_pairs) l
        LEFT JOIN equity_daily_bars b ON b.security_id=l.security_id AND b.trade_date <= ?
        GROUP BY 1 ORDER BY 1
    """, [max(dates)]).fetchall()
    lines = [str(line) for line, _ in counts]
    chunks: list[list[str]] = []
    total = 0
    for line, bars in counts:
        if not chunks or total + int(bars) > budget or len(chunks[-1]) >= LINE_SHARES_CHUNK:
            chunks.append([])
            total = 0
        chunks[-1].append(str(line))
        total += int(bars)
    for chunk in chunks:
        sql, params = _market_daily.vendor_share_state_query(bridge, chunk, end_date=max(dates))
        con.execute(f"""
            INSERT INTO _rp_share_state
            SELECT v.security_id, v.trade_date, CAST(v.pit_shares AS DOUBLE), v.pit_clock, v.pit_available_at,
                   v.run_pending, v.split_pending, v.share_basis, CAST(v.adr_ratio AS DOUBLE), v.adr_ratio_reason
            FROM ({sql}) v SEMI JOIN _rp_share_pairs p ON p.security_id=v.security_id AND p.decision_date=v.trade_date
        """, params)
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_adr_lines (security_id VARCHAR, valid_from DATE, valid_to DATE)")
    adr = [[row.price_security_id, row.valid_from, row.valid_to] for row in bridge.linked_rows(lines)
           if row.share_basis == _market_owner_bridge.SHARE_BASIS_ADR]
    if adr:
        con.executemany("INSERT INTO _rp_adr_lines VALUES (?,?,?)", adr)
    return {"lines": len(lines), "chunks": len(chunks), "max_bars_per_chunk": budget,
            "states": int(con.execute("SELECT count(*) FROM _rp_share_state").fetchone()[0]),
            "adr_intervals": len(adr)}


def _stage_foreign_filer_intervals(store: ResearchStore) -> dict[str, Any]:
    """``_rp_ff_intervals`` from the connection's ``foreign_filer_reason_intervals`` (P11), or empty.

    The relation is looked up in the connection's temp catalog, the research catalog
    and the warehouse (no failing statement, so a caller's transaction is safe).
    Returns the spec entry: the source, the interval count and a digest of the rows
    the panel reads, so a resume over other intervals is refused.
    """
    con = store.con
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_ff_intervals "
                "(cik VARCHAR, reason_code VARCHAR, valid_from TIMESTAMP, valid_to TIMESTAMP)")
    entry: dict[str, Any] = {"reasons": list(FOREIGN_FILER_REASONS), "relation": FOREIGN_FILER_INTERVALS_RELATION,
                             "rule": "a valid owner's missing_metric_state takes the reason of the interval "
                                     "containing the cutoff (valid_from <= cutoff < valid_to); label only"}
    found = con.execute("""
        SELECT count(*) FROM (
          SELECT database_name, schema_name, table_name AS name FROM duckdb_tables()
          UNION ALL SELECT database_name, schema_name, view_name FROM duckdb_views() WHERE NOT internal)
        WHERE name=? AND schema_name='main' AND database_name IN ('temp', ?, ?)
    """, [FOREIGN_FILER_INTERVALS_RELATION, store.catalog, store.warehouse_alias]).fetchone()[0]
    if not found:
        return {**entry, "source": "not_supplied"}
    con.execute(f"""
        INSERT INTO _rp_ff_intervals
        SELECT CAST(cik AS VARCHAR), reason_code, CAST(valid_from AS TIMESTAMP), CAST(valid_to AS TIMESTAMP)
        FROM {FOREIGN_FILER_INTERVALS_RELATION}
        WHERE reason_code IN ({_sql_list(FOREIGN_FILER_REASONS)}) AND cik IS NOT NULL AND valid_from IS NOT NULL
    """)
    return {**entry, "source": FOREIGN_FILER_INTERVALS_RELATION, **_ff_intervals_digest(con)}


def _check_ff_intervals(con: Any, spec: dict[str, Any]) -> None:
    """Refuse to label over intervals other than the ones the spec recorded."""
    entry = spec.get("foreign_filer_reasons") or {}
    got = _ff_intervals_digest(con)
    if entry.get("source") == "not_supplied":
        ok = got["intervals"] == 0
    else:
        ok = entry.get("source") is not None and all(entry.get(key) == value for key, value in got.items())
    if not ok:
        raise RuntimeError("the staged foreign-filer intervals differ from the spec; stage the run again")


def _ff_intervals_digest(con: Any) -> dict[str, Any]:
    count = int(con.execute("SELECT count(*) FROM _rp_ff_intervals").fetchone()[0])
    digest = _stream_digest(con, "SELECT cik, reason_code, valid_from, valid_to FROM _rp_ff_intervals ORDER BY ALL",
                            [])
    return {"intervals": count, "intervals_sha256": digest}


def stage_foreign_filer_reasons(store: ResearchStore, companyfacts_zip: str | Path, *,
                                artifacts_dir: str | Path | None = None) -> dict[str, Any]:
    """Build the P11 ``foreign_filer_reason_intervals`` relation on the store's connection.

    From the verified taxonomy artifacts of the live ``companyfacts.zip``
    (``foreign_filers.resolve_taxonomy_artifacts``; a stale or partial scan raises) and
    the warehouse ``sec_submissions`` when present. Call it before
    :func:`build_research_panel` / :func:`stage_formation`; returns the disclosure summary.
    """
    from .. import foreign_filers as ff

    artifacts = ff.resolve_taxonomy_artifacts(companyfacts_zip, artifacts_dir)
    submissions = "sec_submissions" if store.warehouse_has("sec_submissions") else None
    return ff.build_foreign_filer_disclosure(
        store.con, members=ff.parquet_relation(artifacts.members_path),
        filings=ff.parquet_relation(artifacts.filings_path), submissions=submissions,
        intervals_table=FOREIGN_FILER_INTERVALS_RELATION)


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
    """Where each P2 native reads from in this run (recorded in the definitions): the line's own bars.

    ``equity_price_metrics`` is never read (P2 fix round 1, I-1): its windows count
    rows, not XNYS sessions, and its clock is a market-wide running max, so none of
    its columns is the same feature under the same clock.
    """
    del store
    return {f.metric_code: BARS_SOURCE for f in features if f.metric_code in PRICE_WINDOW_FEATURES}


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
    # P11: a valid owner with no metric state whose CIK is inside a foreign-filer reason
    # interval at the cutoff takes that reason in place of ``missing_metric_state``,
    # keeping the NULL value and the row count (label only). Windows of one CIK are
    # disjoint; min() keeps one row per (formation, CIK) regardless.
    _check_ff_intervals(con, spec)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rp_ff_reason AS
        SELECT c.decision_date, i.cik, min(i.reason_code) AS reason_code
        FROM _rp_cal_part c
        JOIN _rp_ff_intervals i ON i.valid_from <= c.cutoff AND (i.valid_to IS NULL OR c.cutoff < i.valid_to)
        GROUP BY ALL
    """)
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
               CASE WHEN l.reason='missing_metric_state' AND {kept} AND ff.reason_code IS NOT NULL
                    THEN ff.reason_code ELSE l.reason END AS reason,
               {state['lineage_status']} AS lineage_status,
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
        LEFT JOIN _rp_ff_reason ff ON ff.decision_date=l.decision_date AND ff.cik=k.owner_cik
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


def _share_clock_picks(spec: dict[str, Any], alias: str = "m") -> str:
    """market_daily's row share clock (A8 I3 columns, 0328), newest visible revision; NULL before 0328."""
    if spec.get("market_share_clock", "not_in_warehouse") == "not_in_warehouse":
        return "CAST(NULL AS TIMESTAMP) AS shares_available_at, CAST(NULL AS VARCHAR) AS shares_clock"
    key = f"({alias}.available_at, {alias}.market_daily_id)"
    return (f"arg_max_null({alias}.shares_available_at, {key}) AS shares_available_at, "
            f"arg_max_null({alias}.shares_clock, {key}) AS shares_clock")


def _share_clock_basis_sql(clock: str) -> str:
    """A size row's availability basis naming its share clock kind (NULL: the feature's own basis)."""
    return (f"CASE WHEN {clock} IS NOT NULL THEN {_sql_text(MARKET_AVAILABILITY_BASIS + ';shares_clock:')} "
            f"|| {clock} END")


#: Why a session has no price: its newest visible bar row has no positive, finite
#: close and adjusted close (market_daily's A8 label for the same bar).
BAR_PRICE_INVALID_REASON = "bar_price_invalid"


def _split_step_sql(k: str) -> str:
    """Whether the vendor-factor step ``k`` is an exact split or stock-dividend ratio (R1d's classifier)."""
    return (f"(abs({k} - 1) >= {_split_epochs.STOCK_DIVIDEND_MIN_STEP!r} AND CASE WHEN {k} BETWEEN "
            f"{_split_epochs.SPLIT_MIN_RATIO!r} AND {_split_epochs.SPLIT_MAX_RATIO!r} "
            f"THEN {_split_epochs._stock_dividend_sql(k)} ELSE {_split_epochs._simple_sql(k)} END)")


def _stage_price_window(con: Any, row: CalendarRow) -> None:
    """``_rp_price_window``: per eligible line with a bar on the session, its P2 window aggregates.

    Bars follow market_daily's one bar rule: the rows of one ``(security_id,
    trade_date)`` whose clock (``greatest(available_at, trade_date 22:00)``) is at or
    before the cutoff are ranked by ``(available_at, source)``, and every column comes
    from the newest one, a NULL included (``arg_max_null``). The price is checked
    after the pick: a session whose picked close or adjusted close is not positive
    and finite is not observed (and at the formation session it is
    ``bar_price_invalid``). Only bars dated in the ``PRICE_LOOKBACK_DAYS`` before the
    session are read, so no input is dated after the session or knowable only after
    the cutoff; the scan is bounded by the formation (eligible lines x lookback).

    Windows are XNYS rule sessions (``_rp_xnys``: ``back`` = 1 is the formation
    session). ``history_sessions`` is the ``back`` of the line's earliest observed
    session read. A daily return ``adj_t / adj_(t-1) - 1`` exists only when the
    line observed both ``t`` and the session right before it (``back + 1``); a
    return across a gap is not daily and is not used. ``split_21`` is a vendor-factor
    step (``(adj/close)_t / (adj/close)_(t-1)`` between consecutive observed bars of
    the 21-session window) that R1d's classifier calls an exact ratio.

    ``adj`` is the ``vendor_artifact_repaired`` adjusted close (VA1,
    :func:`atx_db._vendor_artifact.repaired_bars_sql` over the picked bars): a vendor
    factor decrease that is not a split (the 2021-01-04 step on ~3,400 dividend payers)
    is neutralized, so no daily return, window level or split step spans it. The rule
    reads a bar and its predecessor only (no look-ahead), and returns inside the read
    window do not depend on where the read starts.
    """
    first = row.formation_date - dt.timedelta(days=PRICE_LOOKBACK_DAYS)
    sessions = sorted({*xnys_sessions(first, row.formation_date), row.formation_date}, reverse=True)
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_xnys (session DATE, back INTEGER)")
    con.executemany("INSERT INTO _rp_xnys VALUES (?, ?)", [[day, back] for back, day in enumerate(sessions, 1)])
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_price_window AS
        WITH picked AS (
          SELECT b.security_id, b.trade_date,
                 CAST(arg_max_null(b.close, (b.available_at, b.source)) AS DOUBLE) AS close,
                 CAST(arg_max_null(b.adjusted_close, (b.available_at, b.source)) AS DOUBLE) AS adj,
                 CAST(arg_max_null(b.volume, (b.available_at, b.source)) AS DOUBLE) AS volume,
                 max({_BAR_CLOCK_SQL}) AS bar_at
          FROM equity_daily_bars b
          SEMI JOIN (SELECT security_id FROM _rp_cohort_all WHERE decision_date=? AND eligible) k
            ON k.security_id=b.security_id
          WHERE b.trade_date BETWEEN ? AND ? AND {_BAR_CLOCK_SQL}<=?
          GROUP BY b.security_id, b.trade_date
        ), repaired AS ({_vendor_artifact.repaired_bars_sql('picked', adjusted='adj')}
        ), obs AS (
          SELECT p.security_id, x.back, p.close, p.adj * p.va_multiplier AS adj, p.volume, p.bar_at
          FROM repaired p JOIN _rp_xnys x ON x.session=p.trade_date
          WHERE p.close>0 AND p.adj>0 AND isfinite(p.close) AND isfinite(p.adj)
        ), seq AS (
          SELECT *, lag(back) OVER w AS prior_back, lag(adj) OVER w AS prior_adj, lag(close) OVER w AS prior_close
          FROM obs WINDOW w AS (PARTITION BY security_id ORDER BY back DESC)
        ), r AS (
          SELECT security_id, back, close, adj, volume, bar_at, prior_back,
                 CASE WHEN prior_back = back + 1 THEN adj / prior_adj - 1.0 END AS ret,
                 (adj / close) / (prior_adj / prior_close) AS step_k
          FROM seq
        ), line AS (
          SELECT security_id, max(back) AS history_sessions,
                 max(bar_at) FILTER (WHERE back <= {PRICE_WINDOW_SESSIONS}) AS available_at,
                 max(adj) FILTER (WHERE back = 1) AS adj_last,
                 max(adj) FILTER (WHERE back <= {PRICE_WINDOW_SESSIONS}) AS high_252,
                 count(*) FILTER (WHERE back <= {PRICE_WINDOW_SESSIONS}) AS n_obs_252,
                 count(ret) FILTER (WHERE back <= 21) AS n_ret_21, max(ret) FILTER (WHERE back <= 21) AS max_ret_21,
                 count(ret) FILTER (WHERE back <= 21 AND volume IS NOT NULL) AS n_ret_vol_21,
                 count(ret) FILTER (WHERE back <= 21 AND volume > 0) AS n_illiq_21,
                 avg(abs(ret) / (close * volume)) FILTER (WHERE back <= 21 AND volume > 0) AS illiq_21,
                 count(ret) FILTER (WHERE back <= 60) AS n_ret_60,
                 avg(power(least(ret, 0.0), 2)) FILTER (WHERE back <= 60) AS down_sq_60,
                 count(volume) FILTER (WHERE back <= 21) AS n_vol_21,
                 avg(volume) FILTER (WHERE back <= 21) AS volume_21,
                 coalesce(bool_or({_split_step_sql('step_k')}) FILTER (WHERE back <= 21 AND prior_back <= 21),
                          false) AS split_21
          FROM r GROUP BY security_id
        )
        SELECT p.security_id, coalesce(l.available_at, p.bar_at) AS available_at,
               NOT coalesce(p.close>0 AND p.adj>0 AND isfinite(p.close) AND isfinite(p.adj), false) AS price_invalid,
               l.* EXCLUDE (security_id, available_at)
        FROM picked p LEFT JOIN line l ON l.security_id=p.security_id
        WHERE p.trade_date=?
    """, [row.formation_date, first, row.formation_date, row.cutoff, row.formation_date])


def _window_gate_sql(code: str, count_column: str, prefix: str = "") -> str:
    """The ``reason_hint`` CASE arms of a P2 window: price, history, observed sessions."""
    spec, p = NATIVE_FEATURES[code], prefix
    return (f"WHEN {p}price_invalid THEN {_sql_text(BAR_PRICE_INVALID_REASON)} "
            f"WHEN {p}history_sessions < {spec['min_history_sessions']} "
            f"THEN {_sql_text(INSUFFICIENT_HISTORY_REASON)} "
            f"WHEN {p}{count_column} < {spec['min_observed']} THEN {_sql_text(WINDOW_GAPS_REASON)} ")


def _price_window_parts(con: Any, features: Sequence[PanelFeature], spec: dict[str, Any], market_source: str,
                        row: CalendarRow) -> list[str]:
    """Long-form value rows of the P2 natives of one formation (see ``NATIVE_FEATURES``).

    ``reason_hint`` names why a present row has no value: ``bar_price_invalid`` (the
    session's picked bar has no valid price), ``insufficient_history`` (the line's
    history does not reach the window's first session), ``window_gaps`` (fewer
    observed sessions or daily returns than the declared minimum),
    ``zero_volume_in_window`` (Amihud: too few positive-volume days), and for
    turnover ``missing_market_row`` (no market_daily row for its share count), the A8
    withheld label of a withheld count, ``unverified_shares`` (a count that is not
    DEI) or ``split_in_price_window``. A row has a value only when it has no hint.
    """
    if any(spec["native_sources"][f.metric_code] != BARS_SOURCE for f in features):
        raise RuntimeError("P2 natives read the line's own bars only")
    parts: list[str] = []
    _stage_price_window(con, row)
    window_sql = {
        "pct_from_high_252d": ("adj_last / high_252 - 1.0", "n_obs_252", ""),
        "max_daily_return_21d": ("max_ret_21", "n_ret_21", ""),
        "amihud_illiquidity_21d": (f"illiq_21 * {AMIHUD_SCALE!r}", "n_ret_vol_21",
                                   f"WHEN n_illiq_21 < {NATIVE_FEATURES['amihud_illiquidity_21d']['min_observed']} "
                                   f"THEN {_sql_text(ZERO_VOLUME_REASON)} "),
        "downside_deviation_60d": ("sqrt(down_sq_60) * sqrt(252.0)", "n_ret_60", ""),
    }
    for f in features:
        if f.metric_code == "turnover_21d":
            continue
        value, count_column, extra = window_sql[f.metric_code]
        parts.append(f"""
            SELECT security_id, feature_id, CAST(CASE WHEN reason_hint IS NULL THEN v END AS DOUBLE) AS value,
                   available_at, fundamental_available_at, value_origin, shares_source, reason_hint,
                   CAST(NULL AS VARCHAR) AS availability_basis
            FROM (SELECT security_id, {_sql_text(f.feature_id)} AS feature_id, {value} AS v, available_at,
                         CAST(NULL AS TIMESTAMP) AS fundamental_available_at, '{BARS_SOURCE}' AS value_origin,
                         CAST(NULL AS VARCHAR) AS shares_source,
                         CAST(CASE {_window_gate_sql(f.metric_code, count_column)}{extra}END AS VARCHAR) AS reason_hint
                  FROM _rp_price_window)""")
    turnover = next((f for f in features if f.metric_code == "turnover_21d"), None)
    if turnover is not None:
        # The line's verified share count at the session: the newest visible
        # market_daily revision (arg_max_null, as every market pick), with its share
        # clock when market_daily records it (R2d).
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _rp_line_shares AS
            SELECT m.security_id, max(m.available_at) AS available_at,
                   arg_max_null(m.fundamental_available_at, (m.available_at, m.market_daily_id))
                     AS fundamental_available_at,
                   CAST(arg_max_null(m.shares_outstanding, (m.available_at, m.market_daily_id)) AS DOUBLE)
                     AS shares,
                   arg_max_null(m.shares_source, (m.available_at, m.market_daily_id)) AS shares_source,
                   {_share_clock_picks(spec)}
            FROM market_daily_metrics m
            JOIN _rp_cal_one c ON m.trade_date=c.decision_date AND m.available_at<=c.cutoff
                              AND m.as_of_date<=c.decision_date
            JOIN _rp_cohort_all k ON k.decision_date=c.decision_date AND k.security_id=m.security_id AND k.eligible
            WHERE m.source=?
            GROUP BY m.security_id
        """, [market_source])
        verified = _sql_list(VERIFIED_SHARES_SOURCES)
        parts.append(f"""
            SELECT security_id, feature_id,
                   CAST(CASE WHEN reason_hint IS NULL AND shares > 0 THEN volume_21 / shares END AS DOUBLE) AS value,
                   available_at, fundamental_available_at, value_origin, shares_source, reason_hint,
                   availability_basis
            FROM (
              SELECT w.security_id, {_sql_text(turnover.feature_id)} AS feature_id, w.volume_21, s.shares,
                     greatest(w.available_at, s.available_at, s.shares_available_at) AS available_at,
                     s.fundamental_available_at, '{BARS_SOURCE}' AS value_origin, s.shares_source,
                     {_share_clock_basis_sql('s.shares_clock')} AS availability_basis,
                     CAST(CASE WHEN s.security_id IS NULL THEN 'missing_market_row'
                               WHEN s.shares_source IN ({_sql_list(SHARES_SOURCES_WITHHELD)}) THEN s.shares_source
                               WHEN s.shares_source IS NULL OR s.shares_source NOT IN ({verified})
                                    THEN {_sql_text(UNVERIFIED_SHARES_REASON)}
                               {_window_gate_sql('turnover_21d', 'n_vol_21', 'w.')}
                               WHEN w.split_21 THEN {_sql_text(SPLIT_WINDOW_REASON)}
                               END AS VARCHAR) AS reason_hint
              FROM _rp_price_window w LEFT JOIN _rp_line_shares s ON s.security_id=w.security_id)""")
    return parts


def _line_cap_part(con: Any, feature: PanelFeature, row: CalendarRow, market_source: str) -> str:
    """The long-form ``line_market_cap`` rows of one formation (UNVERIFIED; opt-in only).

    The line's own close, picked with market_daily's one bar rule (every column from
    the newest ``(available_at, source)`` row, a NULL included; the price checked
    after the pick), times the vendor share count of the latest A8 share run known at
    the session's cutoff (``_rp_share_state``, :func:`_stage_line_shares`), never the
    bar's own count: the vendor starts a run on the DEI cover date, before the filing
    is public. The run's availability is folded into the row's clock and its kind
    named in ``availability_basis``. An ADR line's count is an ADS count (priced by the
    ADS close), used only when the bridge states the ADS ratio; a line whose bridge
    interval for the session has an ADS basis that is not visible yet at the cutoff is
    ``adr_ratio_unknown`` too (R2d review m5: withheld only, never priced as a plain
    line; the later-visible row decides only that the value is withheld, never a value).
    Withheld, with the label as the reason and ``shares_source``, in this order:
    ``bar_price_invalid``; market_daily's basis verdict on the same line and session
    (the newest ``market_daily_metrics`` row visible at the cutoff, the same pick as the
    market features, whose ``shares_source`` is in :data:`LINE_CAP_BASIS_VERDICTS`:
    ``adr_ratio_unresolved`` or ``dei_archive_conflict``, the vendor count is in the
    wrong basis, R2d review I1); ``adr_ratio_unknown``; ``vendor_shares_zero``;
    ``archive_run_pending`` (no run known yet) and ``split_pending_share_update`` (the
    known run started before a split ex-date at or before the bar). A bar with no vendor
    count has no value (``invalid_current_state``). A line market_daily does not cover
    has no verdict.
    Like A8, the bar's own count gates only by presence and sign, never by magnitude.
    """
    staged = con.execute("SELECT count(*) FROM _rp_share_dates WHERE decision_date=?",
                         [row.formation_date]).fetchone()[0]
    if not staged:
        raise RuntimeError(f"the line share state was not staged for {row.formation_date}")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rp_line_cap AS
        WITH verdict AS (
          -- The same newest-visible pick as _rp_market (the caller staged _rp_cal_one).
          SELECT m.security_id,
                 arg_max_null(m.shares_source, (m.available_at, m.market_daily_id)) AS shares_source
          FROM market_daily_metrics m
          JOIN _rp_cal_one c ON m.trade_date=c.decision_date AND m.available_at<=c.cutoff
                            AND m.as_of_date<=c.decision_date
          JOIN _rp_cohort_all k ON k.decision_date=c.decision_date AND k.security_id=m.security_id AND k.eligible
          WHERE m.source=?
          GROUP BY m.security_id
        ), adr_hidden AS (
          SELECT DISTINCT security_id FROM _rp_adr_lines
          WHERE coalesce(valid_from <= ?, true) AND (valid_to IS NULL OR ? < valid_to)
        ), picked AS (
          SELECT b.security_id,
                 CAST(arg_max_null(b.close, (b.available_at, b.source)) AS DOUBLE) AS close,
                 CAST(arg_max_null(b.adjusted_close, (b.available_at, b.source)) AS DOUBLE) AS adj,
                 CAST(arg_max_null(b.shares_outstanding, (b.available_at, b.source)) AS DOUBLE) AS vendor_shares,
                 greatest(max(b.available_at), CAST(b.trade_date AS TIMESTAMP) + INTERVAL {DECISION_HOUR} HOUR)
                   AS bar_at
          FROM equity_daily_bars b
          JOIN _rp_cohort_all k ON k.decision_date=? AND k.security_id=b.security_id AND k.eligible
          WHERE b.trade_date=?
          GROUP BY b.security_id, b.trade_date
        ), judged AS (
          SELECT p.security_id, p.close, p.vendor_shares, s.pit_shares, s.pit_clock, s.share_basis,
                 greatest(p.bar_at, s.pit_available_at) AS available_at,
                 CASE WHEN NOT coalesce(p.close > 0 AND p.adj > 0 AND isfinite(p.close) AND isfinite(p.adj), false)
                           THEN {_sql_text(BAR_PRICE_INVALID_REASON)}
                      WHEN v.shares_source IN ({_sql_list(LINE_CAP_BASIS_VERDICTS)}) THEN v.shares_source
                      WHEN s.share_basis = {_sql_text(_market_owner_bridge.SHARE_BASIS_ADR)}
                           AND s.adr_ratio_reason IS NOT NULL THEN s.adr_ratio_reason
                      WHEN s.share_basis IS NULL AND a.security_id IS NOT NULL THEN 'adr_ratio_unknown'
                      WHEN p.vendor_shares <= 0 THEN 'vendor_shares_zero'
                      WHEN p.vendor_shares IS NULL THEN NULL
                      WHEN s.security_id IS NULL OR s.run_pending THEN 'archive_run_pending'
                      WHEN s.split_pending THEN 'split_pending_share_update'
                      END AS reason_hint
          FROM picked p
          LEFT JOIN _rp_share_state s ON s.security_id=p.security_id AND s.trade_date=?
          LEFT JOIN verdict v ON v.security_id=p.security_id
          LEFT JOIN adr_hidden a ON a.security_id=p.security_id
        )
        SELECT security_id,
               CASE WHEN reason_hint IS NULL AND vendor_shares > 0 AND pit_shares > 0 THEN close * pit_shares END
                 AS value,
               available_at, reason_hint,
               CASE WHEN reason_hint IS NOT NULL THEN reason_hint
                    WHEN vendor_shares > 0 AND share_basis = {_sql_text(_market_owner_bridge.SHARE_BASIS_ADR)}
                         THEN {_sql_text(LINE_ADS_SHARES_SOURCE)}
                    WHEN vendor_shares > 0 THEN {_sql_text(LINE_SHARES_SOURCE)} END AS shares_source,
               CASE WHEN reason_hint IS NULL AND pit_clock IS NOT NULL
                    THEN {_sql_text(VENDOR_SHARES_AVAILABILITY_BASIS + ':')} || pit_clock END AS availability_basis
        FROM judged
    """, [market_source, row.formation_date, row.formation_date, row.formation_date, row.formation_date,
          row.formation_date])
    return (f"SELECT security_id, {_sql_text(feature.feature_id)} AS feature_id, value, available_at, "
            f"CAST(NULL AS TIMESTAMP) AS fundamental_available_at, '{BARS_SOURCE}' AS value_origin, shares_source, "
            f"CAST(reason_hint AS VARCHAR) AS reason_hint, availability_basis FROM _rp_line_cap")


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
        # A size row's clock folds in its share clock when market_daily records it
        # (0328). A withheld count is NULL (market_daily withholds the value) and its
        # A8 label is the row's ``shares_source``; the reason stays
        # ``invalid_current_state``, so R2a digests with unchanged inputs are unchanged
        # (the P7 screen maps these rows to ``share_basis_withheld``).
        size_columns = any(f.feature_id in sized for f in batch.features if f.metric_code in columns)
        shares = ("arg_max_null(m.shares_source, (m.available_at, m.market_daily_id))" if size_columns
                  else "CAST(NULL AS VARCHAR)")
        clock = (_share_clock_picks(spec) if size_columns
                 else "CAST(NULL AS TIMESTAMP) AS shares_available_at, CAST(NULL AS VARCHAR) AS shares_clock")
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _rp_market AS
            SELECT m.security_id, max(m.available_at) AS available_at,
                   arg_max_null(m.fundamental_available_at, (m.available_at, m.market_daily_id))
                     AS fundamental_available_at,
                   {shares} AS shares_source, {clock},
                   {picks}
            FROM market_daily_metrics m
            JOIN _rp_cal_one c ON m.trade_date=c.decision_date AND m.available_at<=c.cutoff
                              AND m.as_of_date<=c.decision_date
            JOIN _rp_cohort_all k ON k.decision_date=c.decision_date AND k.security_id=m.security_id
                                 AND k.eligible
            WHERE m.source=?
            GROUP BY m.security_id
        """, [market_source])
        for f in batch.features:
            if f.metric_code not in columns:
                continue
            if f.feature_id in sized:
                size_sql = ("greatest(available_at, shares_available_at) AS available_at, fundamental_available_at, "
                            "'market_daily' AS value_origin, shares_source, CAST(NULL AS VARCHAR) AS reason_hint, "
                            f"{_share_clock_basis_sql('shares_clock')} AS availability_basis")
            else:
                size_sql = ("available_at, fundamental_available_at, 'market_daily' AS value_origin, "
                            "CAST(NULL AS VARCHAR) AS shares_source, CAST(NULL AS VARCHAR) AS reason_hint, "
                            "CAST(NULL AS VARCHAR) AS availability_basis")
            long_parts.append(f"SELECT security_id, {_sql_text(f.feature_id)} AS feature_id, "
                              f"CAST(\"{f.metric_code}\" AS DOUBLE) AS value, {size_sql} FROM _rp_market")
    if any(f.metric_code == "line_market_cap" for f in batch.features):
        long_parts.append(_line_cap_part(con, next(f for f in batch.features if f.metric_code == "line_market_cap"),
                                         row, market_source))
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
                 x.availability_basis AS row_availability_basis, x.security_id IS NOT NULL AS has_row,
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
               ? AS universe_basis,
               CASE WHEN kept AND row_availability_basis IS NOT NULL THEN row_availability_basis
                    ELSE feature_availability_basis END AS availability_basis, feature_scope,
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


def _warehouse_spec(store: ResearchStore, spec: dict[str, Any]) -> dict[str, Any]:
    """The spec plus what the warehouse and connection supply: P11 reasons, market_daily's share clock."""
    clock = all(store.warehouse_has("market_daily_metrics", c) for c in ("shares_clock", "shares_available_at"))
    return {**spec, "foreign_filer_reasons": _stage_foreign_filer_intervals(store),
            "market_share_clock": ("market_daily_metrics.shares_clock,shares_available_at" if clock
                                   else "not_in_warehouse")}


def build_research_panel(store: ResearchStore, options: ResearchPanelOptions, *,
                         owner_links: Sequence[Any] | None = None,
                         share_bridge: Any = None) -> ResearchPanelResult:
    """Build (or resume) one monthly research panel run in the research store.

    ``owner_links`` overrides the reconstructed bridge (A5 ``BridgeRow``-shaped
    rows); by default it is built from the attached warehouse. It is ignored for
    the strict basis. ``share_bridge`` (a ``MarketOwnerBridge``) overrides the bridge
    ``line_market_cap`` reads A8's share state through (default: the basis's bridge).
    P11 reasons are read from the connection's ``foreign_filer_reason_intervals``
    (:func:`stage_foreign_filer_reasons`) when it exists.
    """
    run_at, features, spec = _validate(options)
    con = store.con
    basis = options.basis
    labels = _basis_labels(basis)
    _require_inputs(store, basis, features, build_bridge=owner_links is None)
    spec = _warehouse_spec(store, spec)
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
    phases: dict[str, float] = {}
    started = time.perf_counter()
    staged_links, built = _owner_links(store, basis, owner_links)
    bridge_json = None if staged_links is None else _canonical(staged_links)
    shares_bridge = _share_bridge(store, basis, features, built, share_bridge)
    if shares_bridge is not None:
        source_ids["line_shares"] = f"market_daily.vendor_share_state_query:{shares_bridge.mode}"
        source_ids["line_share_bridge_sha256"] = share_bridge_digest(shares_bridge)
    source_json = _canonical(source_ids)
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
        if shares_bridge is not None and formed:
            started = time.perf_counter()
            phases_detail = _stage_line_shares(store, shares_bridge, [row.formation_date for row in formed])
            phases["line_shares"] = time.perf_counter() - started
            batch_stats.append({"line_shares": phases_detail})
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
    ``native_sources`` is where each P2 native reads from in this formation;
    ``share_bridge`` is the bridge ``line_market_cap`` reads A8's share state through
    (None when it is not requested).
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
    share_bridge: Any = field(default=None, repr=False, compare=False)

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
                    metric_batch_size: int = 16, owner_links: Sequence[Any] | None = None,
                    share_bridge: Any = None) -> StagedFormation:
    """Stage one formation's owner links, calendar and cohort with the panel's own rules.

    ``formation_date`` is the session the market features read (normally an
    observed session, :func:`observed_sessions`). ``cutoff`` is the timezone-aware
    UTC knowledge cutoff for every input, on or after that session. The spec,
    cohort, primary lines and, through :func:`formation_batches`, the values and
    digests are what :func:`build_research_panel` produces for a formation with
    this session and cutoff. ``features=None`` is the default panel feature set;
    ``()`` stages the cohort only. ``owner_links`` and ``share_bridge`` override the
    reconstructed bridge and the line-share bridge, as in the panel. A vendor share
    count is read as known at the session's bar cutoff (22:00), never later, even
    when ``cutoff`` is later.

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
    spec = _warehouse_spec(store, spec)
    labels = _basis_labels(basis)
    bridge, built = _owner_links(store, basis, owner_links)
    _stage_run_calendar(con, [CalendarRow(month, formation_date, formation_date, formation_date, run_at, None,
                                          CALENDAR_FORMED)])
    cohort = _stage_cohorts(store, spec, labels["market_source"]).get(formation_date) or _empty_cohort_stats()
    cohort_sha = con.execute(_COHORT_DIGEST_SQL.format(relation=FORMATION_COHORT_RELATION,
                                                       where="WHERE decision_date=?"), [formation_date]).fetchone()[0]
    return StagedFormation(
        run_id=run_id, basis=basis, formation_date=formation_date, cutoff=run_at, features=staged_features,
        scopes={item[0]: item[3] for item in spec["features"]}, spec=spec, labels=labels, cohort=cohort,
        owner_bridge=bridge, metric_batch_size=metric_batch_size, cohort_sha256=str(cohort_sha),
        native_sources=_native_sources(store, staged_features),
        share_bridge=_share_bridge(store, basis, staged_features, built, share_bridge))


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
    if staged.share_bridge is not None:
        _stage_line_shares(store, staged.share_bridge, [staged.formation_date])
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
    "FOREIGN_FILER_INTERVALS_RELATION",
    "FOREIGN_FILER_REASONS",
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
    "share_bridge_digest",
    "size_codes",
    "stage_foreign_filer_reasons",
    "stage_formation",
    "stage_owner_links",
    "validate_research_panel",
    "xnys_sessions",
]
