"""The daily market panel: bars ASOF-joined to point-in-time fundamentals.

``market_daily_metrics`` is a wide, migration-pinned daily panel (one row per
``(security_id, trade_date)``) produced by joining ``equity_daily_bars`` to the
running latest-known standardized fact and quarterly derived metric available
as of the bar's end-of-day availability (``trade_date + 22h``, never
``period_end``), then chaining the declarative ``daily``-window derived
metrics (market cap, valuation multiples, total returns, realized vol) on top.

A5: a bar's ``security_id`` is a *price line*; fundamentals, derived metrics
and DEI shares belong to an *accounting owner*. The two are joined only
through the explicit ``market_owner_bridge`` (price line -> owner, valid
interval, link availability, identity basis) -- never by equal ids. The mode
(``MarketDailyOptions.owner_mode``) selects the ``strict`` dated-evidence
bridge or the labeled ``reconstructed`` current-ticker backcast (RX1 default);
every refresh records its linked/unlinked accounting as a
``data_quality_checks`` row (``owner_bridge_linkage``) and returns it in the
dataset details.

A8 share basis (``shares_source``), resolved per bar from the bridge row's
``share_basis`` (see ``market_owner_bridge``). No row ever combines one
class's price with another class's or an issuer-total share count:

``dei``
    Single-class line (reconstructed mode): the owner's latest DEI count.
``archive``
    The line's own vendor count (no eligible DEI; strict mode; non-common or
    unlinked lines).
``split_pending_share_update``
    A vendor share run never spans a split. From a real split's ex-date (a
    daily price-adjustment factor step of at least ``SPLIT_FACTOR_MIN`` that is
    an exact split ratio by R1d's classifier, see :func:`_real_split_sql`)
    until the first vendor run that starts on or after it is known, a row
    priced by vendor counts (``archive``, ``archive_ads``, the class sum, and
    ``archive_split_adjusted``) has NULL shares, cap and valuation. The stale
    pre-split count is never multiplied by the post-split price, and it is
    never rebased with an inferred factor. A spin-off, special dividend or
    other inexact factor step opens no window: the share count is unchanged.
``archive_run_pending``
    The bar carries a vendor count but no vendor run is known yet at its cutoff
    (for example a first run matched to a cover count filed later): NULL.
``vendor_shares_zero``
    The bar's vendor count is zero (no count, never a zero cap): NULL.
``bar_price_invalid``
    The session's newest bar revision has a NULL or non-positive close or
    adjusted close: NULL shares and cap (an older revision's price is never
    revived).
``archive_split_adjusted``
    Split guard. The price-adjustment factor between the DEI effective date and
    the bar, ``k = (close_e * adj_t) / (adj_e * close_t)`` (the line's own bar at
    or before the effective date, at most ``ANCHOR_BAR_MAX_GAP_DAYS`` earlier),
    is split-like (``k >= SPLIT_FACTOR_MIN`` or ``<= 1/SPLIT_FACTOR_MIN``) and the
    vendor count has absorbed it (within ``SPLIT_ARCHIVE_TOLERANCE`` of
    ``DEI x k``): the vendor count prices the bar, so a split after the latest
    10-Q never halves or doubles the cap.
``split_unresolved``
    Split-like ``k`` with a real split ex-date after the anchor bar that the
    vendor count has not (yet) absorbed (vendor lag after a split): NULL, not a
    halved/doubled cap, until a DEI dated after the event arrives.
``dei_archive_conflict``
    Basis guard (DEI/archive ratio): the DEI count differs from the line's own
    vendor count by more than ``BASIS_RATIO_LIMIT`` both on the DEI effective
    date and on the bar date (an issuer-total count on one class line, an
    ordinary-share count on an ADS line, a unit error): NULL. When no vendor
    count exists at the effective date the guard cannot judge and DEI is used.
``class_sum``
    Multi-class issuer: ``market_cap`` is ``sum(close_i x vendor count_i)`` over
    the issuer's bridged class lines trading that day, on every class line; its
    ``shares_outstanding`` is the line's own class count. Only when every known
    class is bridged and priced (``issuer_class_lines``, sibling class lines,
    per-class DEI values), every line has a count, no line has an unabsorbed
    split, and the counts reconcile to the issuer total (latest DEI total, else
    the us-gaap ``CommonStockSharesOutstanding`` total; split-adjusted by ``k``)
    within ``CLASS_SUM_TOLERANCE``.
``multiclass_unresolved``
    Any other multi-class bar (including a per-class DEI state on a single
    line): ``shares_outstanding``, ``market_cap`` and every valuation metric NULL.
``archive_ads``
    ADR line with a known ADS ratio: the vendor ADS-basis count, never the
    ordinary-share DEI count.
``adr_ratio_unknown``
    ADR line whose directory name states no ADS ratio: NULL. Vendor ADR counts
    flip between the depositary float and the ADS-equivalent total (INFY 58M vs
    4.26B), and without a ratio no guard can tell which one a run is.
``adr_ratio_unresolved``
    ADR line without a vendor count, or whose vendor count times the directory's
    ADS ratio disagrees with the ordinary DEI count by more than
    ``ADR_RATIO_LIMIT``: NULL.

Reporting currency: every metric in :func:`valuation_dependent_codes` is NULL
while the owner's current monetary facts are not declared in USD
(``currency_guard`` intervals: ``non_usd``, ``mixed``, ``unknown``).
Availability: DEI counts are available at their filing's ``available_at``.
Vendor (bar-derived) counts follow a *run clock*: the vendor starts each share
run on the DEI cover ("as of") date, about two weeks before the filing is
public, so a bar's own count is look-ahead. Each run (a stretch of equal
positive vendor counts on one line) gets an availability instant
(:data:`ARCHIVE_RUN_CLOCKS`): ``split_derived`` (the prior run's count times a
split-like price factor, known at its own start), ``dei_matched`` (equal, up to
vendor rounding, to a DEI cover count of the line's owner dated within a week
of the run start: known at that filing's ``available_at``; or to one already
public at the run start: known then), ``first_run`` (the line's first count,
modeled as known at its first bar) or ``modeled_lag`` (unmatched: modeled, not
verified, as known a filer-family lag after the run start,
:data:`ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY`). A bar that carries a vendor count
uses the latest-starting run known at its cutoff; the split/basis guards read
the anchor-date count the same way. :func:`vendor_share_state_query` exposes
this per-bar state (count, clock kind, run availability, split pending) as a
relation other readers can join. Every refresh states the clocks in
``shares_availability_basis`` and ``archive_run_clocks``, and records row
counts by share basis, by share clock kind (``rows_by_share_clock``) and by
currency status in the ``owner_bridge_linkage`` quality row (``share_basis``).

Row identity lineage: every row's owner id, identity basis, availability basis
and link method are computed from its bridge row; they are written to the
nullable :data:`IDENTITY_ROW_COLUMNS` only when the table has them (A9
migration), so the writer runs unchanged on an older schema. The row's share
clock (:data:`SHARE_CLOCK_ROW_COLUMNS`: clock kind and share availability) is
written the same way once a migration adds those columns.
"""

from __future__ import annotations

import datetime as dt
import json
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .derived_dsl import LowerContext, compile_expression
from .derived_registry import (
    DERIVED_SOURCE_NAME,
    DerivedMetricDefinition,
    default_derived_definitions,
    topological_order,
)
from .market_owner_bridge import (
    BRIDGE_VALUE_COLUMNS,
    CURRENCY_VALUE_COLUMNS,
    MEMBER_VALUE_COLUMNS,
    OWNER_MODE_RECONSTRUCTED,
    OWNER_MODE_STRICT,
    OWNER_MODES,
    SHARE_BASIS_ADR,
    SHARE_BASIS_MULTI_CLASS,
    SHARE_BASIS_SINGLE,
    MarketOwnerBridge,
    OwnerLinkEvidence,
    bridge_value_params,
    build_market_owner_bridge,
    currency_value_params,
    member_value_params,
    values_relation_sql,
)
from .warehouse import quality_check

__all__ = [
    "ADR_RATIO_LIMIT",
    "ANCHOR_BAR_MAX_GAP_DAYS",
    "ARCHIVE_MODELED_LAG_DAYS",
    "ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY",
    "ARCHIVE_MODELED_LAG_DAYS_FOREIGN",
    "ARCHIVE_RUN_CLOCKS",
    "ARCHIVE_RUN_COVER_WINDOW_DAYS",
    "ARCHIVE_RUN_DEI_LEAD_DAYS",
    "ARCHIVE_RUN_PUBLIC_LOOKBACK_DAYS",
    "ARCHIVE_RUN_ROUNDING_SHARES",
    "BASIS_RATIO_LIMIT",
    "CLASS_SUM_TOLERANCE",
    "END_OF_DAY_HOURS",
    "FOREIGN_FILER_FORMS",
    "IDENTITY_ROW_COLUMNS",
    "MARKET_DAILY_SOURCE_NAME",
    "MARKET_DAILY_STRICT_SOURCE_NAME",
    "OWNER_BRIDGE_CHECK_NAME",
    "SHARES_AVAILABILITY_BASIS",
    "SHARES_SOURCES_VENDOR",
    "SHARES_SOURCES_WITHHELD",
    "SHARE_CLOCK_KINDS",
    "SHARE_CLOCK_ROW_COLUMNS",
    "SPLIT_ARCHIVE_TOLERANCE",
    "SPLIT_FACTOR_MIN",
    "SPLIT_RUN_TOLERANCE",
    "VENDOR_SHARE_STATE_COLUMNS",
    "MarketDailyDataset",
    "MarketDailyOptions",
    "build_market_daily_sql",
    "daily_context",
    "owner_bridge_report",
    "refresh_market_daily_metrics",
    "shares_reconciliation_report",
    "valuation_dependent_codes",
    "vendor_share_state_ctes",
    "vendor_share_state_query",
]

#: Stable provenance tag for rows this engine writes (reconstructed-identity
#: owner bridge, the run5 default per RX1). Imported by A2 -- do not rename.
MARKET_DAILY_SOURCE_NAME = "atx-db daily market panel v1"

#: Distinct label for a panel built on the strict (dated identity evidence)
#: owner bridge, so a strict variant can never overwrite or be mistaken for the
#: reconstructed panel.
MARKET_DAILY_STRICT_SOURCE_NAME = "atx-db daily market panel v1 strict identity"

#: ``data_quality_checks.check_name`` of the per-refresh bridge accounting row.
OWNER_BRIDGE_CHECK_NAME = "owner_bridge_linkage"

#: The end-of-day availability convention stamped onto every bar by
#: ``ticker_history.py`` (``trading_date + 22 hours``), named once so every
#: PIT cutoff in this module derives from a single constant.
END_OF_DAY_HOURS = 22

# --- A8 share-basis thresholds (see the module docstring) -------------------
#: A price-adjustment factor change ``k`` between the share-count anchor date
#: and the bar is split-like at ``k >= 1.2`` or ``k <= 1/1.2``. Twelve months of
#: ordinary dividends move ``k`` by well under 1.2; 5:4 and larger splits,
#: reverse splits and large spin-off adjustments cross it.
SPLIT_FACTOR_MIN = 1.2
#: A vendor count within +-10 % of ``count x k`` has absorbed the split.
SPLIT_ARCHIVE_TOLERANCE = 0.10
#: DEI vs the line's own vendor count: a basis conflict beyond 1.5x (or below
#: 1/1.5) on both the DEI effective date and the bar date. Normal vendor
#: vintage drift is a few percent; class, ADS and unit mix-ups are >= 1.5x.
BASIS_RATIO_LIMIT = 1.5
#: The line's own bar used at the anchor date must be at most 14 days earlier.
ANCHOR_BAR_MAX_GAP_DAYS = 14
#: The class lines' vendor counts must sum to the issuer total within +-5 %.
CLASS_SUM_TOLERANCE = 0.05
#: ADR vendor count x ADS ratio vs the ordinary DEI count, beyond 1.5x.
ADR_RATIO_LIMIT = 1.5

# --- A8 vendor share-run clock (see the module docstring) -------------------
#: A run's value matches a DEI cover count only up to vendor rounding: the
#: parquet stores thousands (x1000 on load), so a copied count is within 1,000
#: shares of the cover value. The match counts when either the cover date is at
#: most 7 days before (weekends, holidays) or 1 day after the run start -- the
#: vendor copied that cover count, which is known at the filing -- or the count
#: was already public at the run start from a filing whose cover date is at
#: most 90 days earlier (known at the run start).
ARCHIVE_RUN_ROUNDING_SHARES = 1_000
ARCHIVE_RUN_COVER_WINDOW_DAYS = 7
ARCHIVE_RUN_DEI_LEAD_DAYS = 1
ARCHIVE_RUN_PUBLIC_LOOKBACK_DAYS = 90
#: Filer family of a run's owner: any ``shares_outstanding_history`` row of one
#: of these forms (or an ADR line) makes the owner a foreign filer. The family
#: is "ever foreign" over the owner's whole history, not as-of: that hindsight
#: only ever lengthens the lag.
FOREIGN_FILER_FORMS = ("20-F", "20-F/A", "40-F", "40-F/A")
#: An unmatched run is modeled (not verified) as known this many days after it
#: starts: per filer family, max over the family's forms of max(measured p95
#: cover date -> filing date, statutory deadline). Measured on the retained
#: companyfacts cache, every company (15,200 with a DEI share count), one
#: observation per (accession, cover date), cover dates since 2010, original
#: filings: 10-K n=86,581 p95 63 d (deadline 90); 10-Q n=267,191 p95 40 d
#: (deadline 45); 20-F n=7,106 p95 150 d (deadline 4 months, 120 d); 40-F
#: n=1,146 p95 92 d (home-country deadline, 90 d). Domestic = 90, foreign = 150.
ARCHIVE_MODELED_LAG_DAYS = 90
ARCHIVE_MODELED_LAG_DAYS_FOREIGN = 150
#: A line with no owner link has no known family: the longest lag applies.
ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY: dict[str, int] = {
    "domestic": ARCHIVE_MODELED_LAG_DAYS,
    "foreign": ARCHIVE_MODELED_LAG_DAYS_FOREIGN,
    "unknown": ARCHIVE_MODELED_LAG_DAYS_FOREIGN,
}
#: A run whose value is the prior run's count times the price-adjustment factor
#: change between the two run starts (split-like, within +-5 %) is derived
#: from public split terms.
SPLIT_RUN_TOLERANCE = 0.05
#: How a vendor share run's availability (``archive_clock``) is decided.
ARCHIVE_RUN_CLOCKS: dict[str, str] = {
    "split_derived": "prior run count x split-like price factor (within 5%); known at its own start (split "
    "terms are public at the ex-date)",
    "dei_matched": "equals a DEI cover count up to vendor rounding (1,000 shares) dated 7 days before to 1 day "
    "after the run start, or one already public at the run start (cover date at most 90 days earlier); known at "
    "max(run start, the earliest such filing's available_at)",
    "first_run": "the line's first vendor count; modeled as known at its first bar",
    "modeled_lag": "unmatched run; modeled, not verified: known a filer-family lag after the run start "
    "(domestic 10-K/10-Q 90 days; foreign 20-F/40-F or ADR 150 days; no owner link 150 days)",
}
#: The per-row share clock kind: a DEI filing, or the vendor run clock.
SHARE_CLOCK_KINDS: tuple[str, ...] = ("dei_filing", *ARCHIVE_RUN_CLOCKS)

#: ``shares_source`` values priced by the line's own vendor count (run clock).
SHARES_SOURCES_VENDOR = ("archive", "archive_ads", "archive_split_adjusted", "class_sum")

#: ``shares_source`` values whose row has NULL shares, NULL market cap and
#: every valuation metric withheld, with the source naming the reason.
SHARES_SOURCES_WITHHELD = (
    "adr_ratio_unknown",
    "adr_ratio_unresolved",
    "archive_run_pending",
    "bar_price_invalid",
    "dei_archive_conflict",
    "multiclass_unresolved",
    "split_pending_share_update",
    "split_unresolved",
    "vendor_shares_zero",
)

#: Availability basis of each share-count source (stated in stage detail).
SHARES_AVAILABILITY_BASIS: dict[str, str] = {
    "dei": "filing_available_at",
    **{source: "vendor_run_clock" for source in SHARES_SOURCES_VENDOR},
}

#: The daily metric overridden by the multi-class issuer cap.
_ISSUER_CAP_CODE = "market_cap"

#: Row-level identity lineage columns (added by the A9 migration). Each is
#: computed per row from the bridge and written only when the table has it, so
#: the writer runs unchanged on a schema without them.
IDENTITY_ROW_COLUMNS: dict[str, str] = {
    "owner_security_id": "f.owner_key",
    "identity_basis": "f.identity_basis",
    "availability_basis": "f.availability_basis",
    "link_method": "f.link_method",
}
#: Row-level share clock (A8 I3). 0327's link labels cannot carry it (their
#: ``availability_basis`` is the owner link's), so these nullable columns are
#: listed for the post-B0 0328 bundle and written only once the table has them:
#: ``shares_clock`` (one of :data:`SHARE_CLOCK_KINDS`; NULL on a withheld row)
#: and ``shares_available_at`` (the DEI state's or the vendor run's availability).
SHARE_CLOCK_ROW_COLUMNS: dict[str, str] = {
    "shares_clock": "f.shares_clock",
    "shares_available_at": "f.shares_available_at",
}
_OPTIONAL_ROW_COLUMNS: dict[str, str] = {**IDENTITY_ROW_COLUMNS, **SHARE_CLOCK_ROW_COLUMNS}

#: Columns of :func:`vendor_share_state_query` (one row per bar with a vendor count).
VENDOR_SHARE_STATE_COLUMNS = (
    "security_id",
    "trade_date",
    "pit_shares",
    "pit_clock",
    "pit_lag_family",
    "pit_run_start",
    "pit_available_at",
    "last_split_date",
    "run_pending",
    "split_pending",
    "share_basis",
    "adr_ratio",
    "adr_ratio_reason",
)

#: Row-count (not calendar-day) lookback used to widen the ``bars`` CTE below
#: ``MarketDailyOptions.start_date`` so a scoped/incremental refresh's trailing
#: windows (up to a 252-trading-session realized-vol/total-return frame; the
#: 21-trading-day skip in ``momentum_12_1`` is a second, independent ``tret``
#: whose lag is ``max()``'d with the 252-day one, not added to it, so 252 is
#: the true worst case) are never computed over a truncated bar range. This
#: MUST be a per-security *row* count -- every trailing window is
#: ``ROWS BETWEEN N PRECEDING``, i.e. N rows actually present in the
#: partition, not N calendar days -- so a calendar-day buffer alone silently
#: under-fills for a security with sparse/halted bar coverage (a long halt can
#: span far more calendar time than 252 normal trading sessions). 300 leaves
#: comfortable margin over the 252-session worst case. See
#: ``build_market_daily_sql``'s ``bars_by_session``/``bars`` CTEs: a bar
#: strictly before ``start_date`` is kept only if its descending-trade_date
#: rank *within that security* is <= this limit; a bar on/after ``start_date``
#: is always kept regardless of rank.
_LOOKBACK_ROW_LIMIT = 300

#: A date far enough in the past that no real bar predates it, used so the
#: ``bars_by_session`` CTE's ``is_recent`` computation (and thus the
#: lookback-rank partitioning) is a single, always-valid SQL expression
#: whether or not the caller passed a ``start_date`` -- when there is no
#: ``start_date``, every
#: bar is "recent" relative to this sentinel, so the row-rank restriction is
#: a structural no-op and the full history is used, matching an unscoped run.
_NO_START_DATE_SENTINEL = dt.date(1900, 1, 1)


@dataclass(frozen=True)
class MarketDailyOptions:
    source: str = MARKET_DAILY_SOURCE_NAME
    derived_source: str = DERIVED_SOURCE_NAME
    bar_source: str | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    security_ids: tuple[str, ...] | None = None
    batch_size: int = 200
    shares_tolerance: float = 0.05
    run_id: str | None = None
    #: Price-line -> accounting-owner bridge mode (see ``market_owner_bridge``).
    owner_mode: str = OWNER_MODE_RECONSTRUCTED

    def __post_init__(self) -> None:
        if self.owner_mode not in OWNER_MODES:
            raise ValueError(f"owner_mode must be one of {OWNER_MODES}, got {self.owner_mode!r}")
        # Each identity basis writes its own labeled source; neither may
        # silently replace the other's panel.
        if self.owner_mode == OWNER_MODE_STRICT and self.source == MARKET_DAILY_SOURCE_NAME:
            raise ValueError(
                "strict owner mode must write a distinct labeled source "
                f"(e.g. {MARKET_DAILY_STRICT_SOURCE_NAME!r}), not the reconstructed panel's"
            )
        if self.owner_mode == OWNER_MODE_RECONSTRUCTED and self.source == MARKET_DAILY_STRICT_SOURCE_NAME:
            raise ValueError("the strict-identity panel source cannot hold reconstructed-identity rows")


def daily_context(*, names: tuple[str, ...]) -> LowerContext:
    """The ``LowerContext`` for the daily grid.

    Every name reachable by a ``daily``-window expression -- an ``item:``
    code, a ``metric:`` code (quarterly or another daily metric earlier in
    topological order) or a ``market:`` column -- is already a column of the
    running relation aliased ``p`` by the time a daily metric's chained CTE
    lowers its expression, so one uniform mapping covers all three
    namespaces.
    """
    columns = {name: f'p."{name}"' for name in names}
    availability = {name: f'p."{name}__at"' for name in names}
    return LowerContext(
        grid="day",
        columns=columns,
        availability=availability,
        partition_sql="p.security_id",
        order_sql="p.trade_date",
    )


def _quote(code: str) -> str:
    return "'" + code.replace("'", "''") + "'"


def _in_list(codes: tuple[str, ...]) -> str:
    return ", ".join(_quote(code) for code in codes) if codes else "''"


def _sql_double(value: float) -> str:
    return f"CAST({float(value)!r} AS DOUBLE)"


def _asof_joins(codes: tuple[str, ...]) -> tuple[str, str]:
    """Return (join clauses, projection list) for one ASOF join per code.

    Each code gets its own ``latest_by_code``-filtered subquery so the ASOF
    predicate (``b.cutoff >= alias.available_at``) picks, independently per
    code, the running latest-known value as of the bar's end-of-day cutoff.
    The equality key is the bar's bridged ``owner_key`` (NULL for an unlinked
    line or a link not yet available, which therefore matches nothing).
    """
    joins: list[str] = []
    projections: list[str] = []
    for index, code in enumerate(codes):
        alias = f"f{index}"
        joins.append(
            f"ASOF LEFT JOIN (SELECT security_id, available_at, value, state_lineage FROM latest_by_code "
            f"WHERE code = {_quote(code)}) {alias} "
            f"ON {alias}.security_id = b.owner_key AND b.cutoff >= {alias}.available_at"
        )
        projections.append(f'{alias}.value AS "{code}"')
        projections.append(f'{alias}.available_at AS "{code}__at"')
        projections.append(f'{alias}.state_lineage AS "{code}__state_lineage"')
    return "\n    ".join(joins), ",\n           ".join(projections)


def _bars_by_session_sql(*, security_count: int, bars_extra_predicate: str, with_recent: bool) -> str:
    """One deduped row per ``(security_id, trade_date)`` of the batch's bars.

    Revisions are ranked first and the price checked after: the newest physical
    row wins even when its close or adjusted close is NULL or not positive (the
    session is then ``price_valid = false`` with NULL prices, never an older
    revision's price).

    Bind order: the effective start date (only ``with_recent``), the batch's
    security ids, then the ``bars_extra_predicate`` params.
    """
    # arg_max_null: the newest physical row's value, NULL included. Plain
    # arg_max skips a NULL and would revive an older revision's value (R2e).
    recent = ",\n           (trade_date >= ?) AS is_recent" if with_recent else ""
    return f"""SELECT security_id, trade_date, symbol,
           CASE WHEN close > 0 THEN close END AS close,
           CASE WHEN adj_close > 0 THEN adj_close END AS adj_close,
           volume, archive_shares, bar_at, cutoff,
           coalesce(close > 0 AND adj_close > 0, false) AS price_valid{recent}
    FROM (
        SELECT security_id, trade_date,
               arg_max_null(symbol, (available_at, source)) AS symbol,
               arg_max_null(close, (available_at, source)) AS close,
               arg_max_null(adjusted_close, (available_at, source)) AS adj_close,
               arg_max_null(volume, (available_at, source)) AS volume,
               arg_max_null(shares_outstanding, (available_at, source)) AS archive_shares,
               greatest(max(available_at),
                        CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR) AS bar_at,
               CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR AS cutoff
        FROM equity_daily_bars
        WHERE trade_date IS NOT NULL
          AND security_id IN ({", ".join(["?"] * security_count)})
          {bars_extra_predicate}
        GROUP BY security_id, trade_date
    )"""


#: The owner links the vendor share-run clock matches DEI cover counts through:
#: one row per (price line, owner member id, bridge interval), from the bound
#: ``owner_bridge`` / ``owner_members`` VALUES relations.
_VENDOR_SHARE_LINKS_SQL = f"""SELECT o.price_security_id, m.member_security_id, o.valid_from, o.valid_to,
           coalesce(o.share_basis = '{SHARE_BASIS_ADR}', false) AS adr_line
    FROM owner_bridge o
    JOIN owner_members m ON m.owner_key = o.owner_key"""


def _real_split_sql(k: str) -> str:
    """Whether the daily factor step ``k`` is a real split ex-date (A8 fix N1).

    Split-like (``max(k, 1/k) >= SPLIT_FACTOR_MIN``) **and** an exact split
    ratio by R1d's classifier (``_split_epochs``, read-only), so the panel and
    the derived split guard agree on what a split is: in R1d's band
    ``[SPLIT_MIN_RATIO, SPLIT_MAX_RATIO]`` (6:5, 5:4) a whole number of
    half-percent stock-dividend steps, outside it ``p/q`` with ``q <= 10`` (2:1,
    3:2, 1:10, ...), both within the vendor's rounding. An inexact step (spin-off,
    special dividend, fund distribution) is not a split.
    """
    # Imported here: _split_epochs imports this module's lag constants.
    from ._split_epochs import SPLIT_MAX_RATIO, SPLIT_MIN_RATIO, _simple_sql, _stock_dividend_sql

    split_min = _sql_double(SPLIT_FACTOR_MIN)
    return (
        f"(greatest({k}, 1 / ({k})) >= {split_min} AND CASE WHEN {k} BETWEEN {SPLIT_MIN_RATIO} AND "
        f"{SPLIT_MAX_RATIO} THEN {_stock_dividend_sql(k)} ELSE {_simple_sql(k)} END)"
    )


def vendor_share_state_ctes(*, bars: str, links: str, targets: str) -> str:
    """The A8 vendor share-run clock as ``WITH`` clauses (no leading ``WITH``).

    ``bars`` names one row per ``(security_id, trade_date)`` with the columns
    ``security_id, trade_date, close, adj_close, archive_shares`` (the vendor
    count in shares; a NULL price marks an invalid session) -- the line's full
    history, since runs and split ex-dates are found over it. ``links`` names
    ``(price_security_id, member_security_id, valid_from, valid_to, adr_line)``:
    the owner member ids whose DEI cover counts a run may match, and the filer
    family evidence. ``targets`` names the bars to resolve (``security_id,
    trade_date, cutoff, archive_shares``). Reads ``shares_outstanding_history``.

    Defines ``run_clock`` / ``run_anchor`` (one row per run: count, clock kind,
    filer family, availability) and ``vendor_share_state`` (one row per target
    bar with a vendor count, :data:`VENDOR_SHARE_STATE_COLUMNS`): the
    latest-starting run known at the bar's cutoff (``pit_*``), the latest split
    ex-date at or before the bar, ``run_pending`` (no run known yet) and
    ``split_pending`` (the known run started before that ex-date: it spans a
    split, so its count must not price the bar). ``split_events`` holds the
    real split ex-dates (see :func:`_real_split_sql`).
    """
    split_hi, split_lo = _sql_double(SPLIT_FACTOR_MIN), _sql_double(1.0 / SPLIT_FACTOR_MIN)
    run_split_tol = _sql_double(SPLIT_RUN_TOLERANCE)
    return f"""share_rows AS (
    -- A run is a maximal stretch of equal positive vendor counts on one line
    -- over its full bar history (a NULL-count bar does not break it).
    SELECT security_id, trade_date, close, adj_close, archive_shares,
           archive_shares IS DISTINCT FROM lag(archive_shares) OVER w AS new_run
    FROM {bars}
    WHERE archive_shares > 0
    WINDOW w AS (PARTITION BY security_id ORDER BY trade_date)
), share_runs AS (
    SELECT security_id, trade_date AS run_start, archive_shares AS run_shares,
           CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR AS run_cutoff,
           row_number() OVER r = 1 AS first_run,
           lag(archive_shares) OVER r AS prior_shares,
           -- Price-adjustment factor change between the prior run's start and this one's.
           CASE WHEN lag(adj_close) OVER r > 0 AND close > 0
                THEN (lag(close) OVER r * adj_close) / (lag(adj_close) OVER r * close) END AS run_factor
    FROM share_rows
    WHERE new_run
    WINDOW r AS (PARTITION BY security_id ORDER BY trade_date)
), run_links AS (
    -- The owner member ids linked to the run's line at its start. Evidence
    -- only, so the link's own clock is not required here.
    SELECT r.security_id, r.run_start, r.run_shares, r.run_cutoff, l.member_security_id, l.adr_line
    FROM share_runs r
    JOIN {links} l
      ON l.price_security_id = r.security_id
     AND l.valid_from <= r.run_start
     AND (l.valid_to IS NULL OR r.run_start < l.valid_to)
), foreign_members AS (
    SELECT DISTINCT h.security_id
    FROM shares_outstanding_history h
    WHERE h.form IN ({_in_list(FOREIGN_FILER_FORMS)})
      AND h.security_id IN (SELECT member_security_id FROM run_links)
), run_family AS (
    SELECT k.security_id, k.run_start,
           CASE WHEN bool_or(k.adr_line OR f.security_id IS NOT NULL) THEN 'foreign' ELSE 'domestic' END
               AS lag_family
    FROM run_links k
    LEFT JOIN foreign_members f ON f.security_id = k.member_security_id
    GROUP BY k.security_id, k.run_start
), run_dei AS (
    -- The run's count is a DEI cover count of the line's owner (any class), up
    -- to vendor rounding: public from that filing.
    SELECT k.security_id, k.run_start, min(h.available_at) AS dei_available_at
    FROM run_links k
    JOIN shares_outstanding_history h ON h.security_id = k.member_security_id
    WHERE h.share_count_type = 'shares_outstanding' AND h.taxonomy = 'dei'
      AND h.concept = 'EntityCommonStockSharesOutstanding'
      AND h.available_at IS NOT NULL AND h.share_count > 0
      AND h.effective_date <= k.run_start + {ARCHIVE_RUN_DEI_LEAD_DAYS}
      AND (h.effective_date >= k.run_start - {ARCHIVE_RUN_COVER_WINDOW_DAYS}
           OR (h.available_at <= k.run_cutoff
               AND h.effective_date >= k.run_start - {ARCHIVE_RUN_PUBLIC_LOOKBACK_DAYS}))
      AND abs(h.share_count - k.run_shares) <= {ARCHIVE_RUN_ROUNDING_SHARES}
    GROUP BY k.security_id, k.run_start
), run_base AS (
    SELECT r.security_id, r.run_start, r.run_shares, r.run_cutoff,
           coalesce(f.lag_family, 'unknown') AS lag_family,
           coalesce(r.prior_shares > 0 AND (r.run_factor >= {split_hi} OR r.run_factor <= {split_lo})
                    AND abs(r.run_shares / (r.prior_shares * r.run_factor) - 1) <= {run_split_tol},
                    false) AS split_derived,
           CASE WHEN d.dei_available_at IS NOT NULL THEN 'dei_matched'
                WHEN r.first_run THEN 'first_run'
                ELSE 'modeled_lag' END AS base_clock,
           CASE WHEN d.dei_available_at IS NOT NULL THEN greatest(r.run_cutoff, d.dei_available_at)
                WHEN r.first_run THEN r.run_cutoff
                WHEN f.lag_family = 'domestic' THEN r.run_cutoff + INTERVAL {ARCHIVE_MODELED_LAG_DAYS} DAY
                ELSE r.run_cutoff + INTERVAL {ARCHIVE_MODELED_LAG_DAYS_FOREIGN} DAY END AS base_at
    FROM share_runs r
    LEFT JOIN run_dei d ON d.security_id = r.security_id AND d.run_start = r.run_start
    LEFT JOIN run_family f ON f.security_id = r.security_id AND f.run_start = r.run_start
), run_clock AS (
    -- A split-derived run is known at its own start: split terms are public
    -- at the ex-date, so it never waits for the prior run's clock.
    SELECT security_id, run_start, run_shares, lag_family,
           CASE WHEN split_derived THEN 'split_derived' ELSE base_clock END AS run_clock,
           CASE WHEN split_derived THEN run_cutoff ELSE base_at END AS available_at
    FROM run_base
), run_anchor AS (
    SELECT *, lag(run_shares) OVER r AS prior_run_shares, lag(available_at) OVER r AS prior_available_at
    FROM run_clock
    WINDOW r AS (PARTITION BY security_id ORDER BY run_start)
), archive_state AS (
    -- The latest-starting run known at each availability instant.
    SELECT security_id, available_at AS state_at, state.run_shares AS pit_shares, state.run_clock AS pit_clock,
           state.lag_family AS pit_lag_family, state.run_start AS pit_run_start,
           state.run_at AS pit_available_at
    FROM (
        SELECT security_id, available_at,
               arg_max(struct_pack(run_shares := run_shares, run_clock := run_clock, lag_family := lag_family,
                                   run_start := run_start, run_at := available_at), run_start) OVER w AS state,
               row_number() OVER (PARTITION BY security_id, available_at ORDER BY run_start DESC) AS rk
        FROM run_clock
        WINDOW w AS (PARTITION BY security_id ORDER BY available_at
                     RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1
), split_events AS (
    -- Real split ex-dates only: a daily price-adjustment factor step (daily, so
    -- ordinary dividends never accumulate into one, between valid-price
    -- sessions) that R1d's classifier calls an exact split ratio. A spin-off,
    -- special dividend or other inexact step leaves the share count valid.
    SELECT security_id, trade_date AS ex_date
    FROM (
        SELECT security_id, trade_date,
               (lag(close) OVER w * adj_close) / (lag(adj_close) OVER w * close) AS day_factor
        FROM {bars}
        WHERE close > 0 AND adj_close > 0
        WINDOW w AS (PARTITION BY security_id ORDER BY trade_date)
    )
    WHERE {_real_split_sql("day_factor")}
), vendor_share_state AS (
    SELECT t.security_id, t.trade_date, s.pit_shares, s.pit_clock, s.pit_lag_family, s.pit_run_start,
           s.pit_available_at, x.ex_date AS last_split_date,
           s.pit_shares IS NULL AS run_pending,
           coalesce(x.ex_date > s.pit_run_start, false) AS split_pending
    FROM {targets} t
    ASOF LEFT JOIN archive_state s
      ON s.security_id = t.security_id AND t.cutoff >= s.state_at
    ASOF LEFT JOIN split_events x
      ON x.security_id = t.security_id AND t.trade_date >= x.ex_date
    WHERE t.archive_shares > 0
)"""


def vendor_share_state_query(
    bridge: MarketOwnerBridge,
    security_ids: Sequence[str],
    *,
    end_date: dt.date | None = None,
    bar_source: str | None = None,
) -> tuple[str, list[Any]]:
    """``(sql, params)`` of the vendor share state of every bar with a vendor count.

    The read relation for other readers of bar share counts (R2d): the same
    computation the panel prices vendor-count rows with, one row per
    ``(security_id, trade_date)`` in :data:`VENDOR_SHARE_STATE_COLUMNS`, over
    ``equity_daily_bars`` and ``shares_outstanding_history`` of the connection
    it runs on. ``bridge`` is a :func:`build_market_owner_bridge` result (its
    links decide which DEI cover counts a run may match and the filer family).

    A reader joins it on ``(security_id, trade_date)``: ``pit_shares`` is the
    count known at the bar's cutoff, ``pit_clock`` its clock kind
    (:data:`ARCHIVE_RUN_CLOCKS`; ``modeled_lag`` / ``first_run`` are modeled, not
    verified), ``pit_available_at`` when that run became known (fold it into
    the reader's input clock), and ``run_pending`` / ``split_pending`` mean the
    count must not be used (``archive_run_pending`` /
    ``split_pending_share_update``). ``share_basis`` / ``adr_ratio`` /
    ``adr_ratio_reason`` come from the bridge row covering the bar (available
    at its cutoff; NULL basis = unlinked): ``pit_shares`` alone is a line cap
    only on a ``single``, ``withheld`` or unlinked line. On an ``adr`` line it is
    an ADS count usable only with a known ratio (``adr_ratio_reason =
    'adr_ratio_unknown'`` otherwise: withhold it). On a ``multi_class`` line it
    is one class's count, never an issuer cap. The panel's full decision (DEI
    guards, per-class DEI, class sums) is ``market_daily_metrics.shares_source``.
    For a ``market_daily_metrics`` row the share clock kind is ``'dei_filing'``
    when ``shares_source = 'dei'``, ``pit_clock`` when it is in
    :data:`SHARES_SOURCES_VENDOR`, else NULL.
    """
    lines = tuple(dict.fromkeys(security_ids))
    if not lines:
        raise ValueError("vendor_share_state_query needs at least one price line")
    bridge_rows = bridge.linked_rows(lines)
    members = bridge.members_for(bridge_rows)
    extra: list[str] = []
    extra_params: list[Any] = []
    if end_date is not None:
        extra.append("AND trade_date <= ?")
        extra_params.append(end_date)
    if bar_source is not None:
        extra.append("AND source = ?")
        extra_params.append(bar_source)
    bars = _bars_by_session_sql(
        security_count=len(lines), bars_extra_predicate="\n      ".join(extra), with_recent=False
    )
    sql = f"""
WITH owner_bridge AS (
    {values_relation_sql(BRIDGE_VALUE_COLUMNS, len(bridge_rows))}
), owner_members AS (
    {values_relation_sql(MEMBER_VALUE_COLUMNS, len(members))}
), bars_by_session AS (
    {bars}
), vendor_share_links AS (
    {_VENDOR_SHARE_LINKS_SQL}
), {vendor_share_state_ctes(bars="bars_by_session", links="vendor_share_links", targets="bars_by_session")}
SELECT v.security_id, v.trade_date, v.pit_shares, v.pit_clock, v.pit_lag_family, v.pit_run_start,
       v.pit_available_at, v.last_split_date, v.run_pending, v.split_pending,
       o.share_basis, o.adr_ratio,
       CASE WHEN o.share_basis = '{SHARE_BASIS_ADR}' AND NOT coalesce(o.adr_ratio > 0, false)
            THEN 'adr_ratio_unknown' END AS adr_ratio_reason
FROM vendor_share_state v
LEFT JOIN owner_bridge o
  ON o.price_security_id = v.security_id
 AND o.valid_from <= v.trade_date
 AND (o.valid_to IS NULL OR v.trade_date < o.valid_to)
 AND o.available_at <= CAST(v.trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR
ORDER BY v.security_id, v.trade_date
"""
    params = [*bridge_value_params(bridge_rows), *member_value_params(members), *lines, *extra_params]
    return sql, params


def build_market_daily_sql(
    *,
    item_codes: tuple[str, ...],
    metric_codes: tuple[str, ...],
    daily_definitions: tuple[DerivedMetricDefinition, ...],
    security_count: int,
    bars_extra_predicate: str,
    output_date_predicate: str,
    bridge_row_count: int = 0,
    member_row_count: int = 0,
    currency_row_count: int = 0,
    identity_columns: Sequence[str] = (),
) -> str:
    """Return the full ``INSERT`` statement for one batch of securities.

    ``identity_columns`` names the :data:`IDENTITY_ROW_COLUMNS` the target
    table has (none on a pre-A9 schema); they are appended to the INSERT.

    Trailing-window metrics (``tret``/``rvol``/``avg_d`` -> total returns,
    momentum, realized vol, dollar volume) are computed over the ``bars`` CTE,
    which is ranked from ``bars_by_session`` (one row per
    ``(security_id, trade_date)``, already deduped): every bar on/after the
    caller's ``start_date`` is kept unconditionally, and every bar strictly
    before ``start_date`` is kept only if its descending-``trade_date`` rank
    *within that security* is <= ``_LOOKBACK_ROW_LIMIT``. This is a per
    -security row count, not a calendar-day window, so a scoped/incremental
    refresh's trailing windows are never truncated even for a security with
    sparse/halted bar coverage (unlike a calendar-day buffer, which can
    under-fill when a gap spans more calendar time than the window's row
    count would otherwise need) -- matching what an unscoped run would
    compute. When the caller passed no ``start_date`` at all, the "on/after
    start_date" branch matches every real bar (see ``_NO_START_DATE_SENTINEL``
    in ``refresh_market_daily_metrics``), so the rank restriction is a
    structural no-op and the full history is used, exactly as before this
    fix. ``bars_extra_predicate`` carries only ``end_date``/``bar_source``
    (never a lower date bound -- that's the row-rank's job); it applies
    uniformly to both the "recent" and "lookback" bars, which is safe because
    every "lookback" bar's ``trade_date`` is already `< start_date <=
    end_date` by construction. ``output_date_predicate`` is the caller's
    actual ``[start_date, end_date]`` restriction, applied only to the final
    emitted rows (``f.trade_date``) -- it is what actually scopes what gets
    inserted, matching the DELETE's scope.

    Owner bridge (A5): the batch's linked ``owner_bridge`` rows
    (``BRIDGE_VALUE_COLUMNS``) and ``owner_members`` rows
    (``MEMBER_VALUE_COLUMNS``) are bound as two leading VALUES CTEs. Each bar
    takes the bridge row whose ``[valid_from, valid_to)`` contains its
    ``trade_date`` and whose link ``available_at`` is at or before its cutoff;
    fundamentals, derived metrics and DEI shares are read from every member id
    of that owner and re-keyed to ``owner_key`` before the ASOF joins. The A8
    share basis (module docstring) is then resolved per bar: DEI prices only a
    ``single`` row (behind the split and basis guards), a ``multi_class`` bar is
    priced by the class sum over the issuer's rows of the same trade date in
    this batch (batches never split an issuer), an ``adr`` bar by its vendor ADS
    count. Every metric in :func:`valuation_dependent_codes` is NULL when the
    bridge row is not ``valuation_eligible``, the share basis is withheld
    (:data:`SHARES_SOURCES_WITHHELD`) or the owner's reporting currency is not
    USD (``valuation_withheld``).

    Bind order, matching the placeholders in the query text left to right:
    the bridge rows, the member rows, then the currency-guard rows (in
    ``BRIDGE_VALUE_COLUMNS``/``MEMBER_VALUE_COLUMNS``/``CURRENCY_VALUE_COLUMNS``
    order, one value per column per row), then
    the effective start date (the ``is_recent`` computation -- it's in
    the ``SELECT`` list, textually *before* the ``security_id IN (...)``
    predicate in the ``WHERE`` clause below it, even though ``WHERE`` filters
    first logically; DuckDB numbers ``?`` placeholders by their left-to-right
    position in the parsed text, not by execution order -- always bound, see
    ``_NO_START_DATE_SENTINEL``), then the batch's security ids (for
    ``bars_by_session``), then the ``bars_extra_predicate`` params
    (end/bar_source, in that order, however many of the two are present),
    then ``derived_source`` (the ``fund_long`` union's derived-side filter),
    then ``source`` twice (the id hash, then the ``source`` column), then
    ``run_id``, then the ``output_date_predicate`` params (start_date/end_date,
    in that order, however many are present).
    """
    codes = tuple(item_codes) + tuple(metric_codes)
    joins, projections = _asof_joins(codes)
    split_hi, split_lo = _sql_double(SPLIT_FACTOR_MIN), _sql_double(1.0 / SPLIT_FACTOR_MIN)
    basis_hi, basis_lo = _sql_double(BASIS_RATIO_LIMIT), _sql_double(1.0 / BASIS_RATIO_LIMIT)
    adr_hi, adr_lo = _sql_double(ADR_RATIO_LIMIT), _sql_double(1.0 / ADR_RATIO_LIMIT)
    split_tol, class_tol = _sql_double(SPLIT_ARCHIVE_TOLERANCE), _sql_double(CLASS_SUM_TOLERANCE)
    withheld_sources = _in_list(SHARES_SOURCES_WITHHELD)
    vendor_sources = _in_list(SHARES_SOURCES_VENDOR)
    unknown = sorted(set(identity_columns) - set(_OPTIONAL_ROW_COLUMNS))
    if unknown:
        raise ValueError(f"unknown optional row columns {unknown}")
    identity_insert = "".join(f", {name}" for name in identity_columns)
    identity_select = "".join(f", {_OPTIONAL_ROW_COLUMNS[name]}" for name in identity_columns)
    state_lineage = "[" + ", ".join(
        f'struct_pack(code := {_quote(code)}, state := f."{code}__state_lineage")' for code in codes
    ) + "]" if codes else "'[]'"
    bars_by_session = _bars_by_session_sql(
        security_count=security_count, bars_extra_predicate=bars_extra_predicate, with_recent=True
    )
    vendor_state = vendor_share_state_ctes(bars="bars_by_session", links="vendor_share_links", targets="bars")
    availability_terms = ", ".join(
        f"coalesce(f{index}.available_at, TIMESTAMP '-infinity')" for index in range(len(codes))
    )
    fundamental_at = (
        f"nullif(greatest({availability_terms}), TIMESTAMP '-infinity')" if codes else "CAST(NULL AS TIMESTAMP)"
    )

    chain: list[str] = []
    previous = "panel"
    withheld_codes = valuation_dependent_codes(daily_definitions)
    for rank, definition in enumerate(daily_definitions):
        names = tuple(definition.item_inputs) + tuple(definition.metric_inputs) + tuple(definition.market_inputs)
        lowered = compile_expression(definition.expression, daily_context(names=names))
        alias = f"m{rank}"
        value_sql = lowered.value_sql
        if definition.metric_code == _ISSUER_CAP_CODE:
            # A8: a resolved multi-class bar's cap is the issuer class sum (its
            # own shares_outstanding is only its class count).
            value_sql = f"coalesce(p.class_market_cap, ({value_sql}))"
        if definition.metric_code in withheld_codes:
            # A5/A8 guard: no valuation metric on a withheld bridge row, an
            # unresolved share basis, or a non-USD reporting currency.
            value_sql = f"CASE WHEN p.valuation_withheld THEN NULL ELSE ({value_sql}) END"
        chain.append(
            f"""{alias} AS (
    SELECT p.*,
           {value_sql} AS "{definition.metric_code}",
           {lowered.availability_sql} AS "{definition.metric_code}__at"
    FROM {previous} p
)"""
        )
        previous = alias

    metric_columns = ", ".join(f'"{definition.metric_code}"' for definition in daily_definitions)
    chain_sql = (",\n".join(chain) + ",\n") if chain else ""

    # M1: fold the availability of every trailing-window daily metric into the
    # row-level available_at, not just the bar/fundamental availability. A
    # windowed metric's own "<code>__at" is already max(...) OVER the frame it
    # reads (computed by derived_dsl's lowering), so if a bar *inside* e.g. a
    # 252-day realized-vol window was late-arriving/corrected, that lateness is
    # already captured there; it just needs to reach the row. f.bar_at is
    # never NULL, so coalescing each "<code>__at" to it is a safe, always-
    # defined fallback (rather than the NULL-propagating plain SQL greatest()).
    metric_at_terms = ", ".join(
        f'coalesce(f."{definition.metric_code}__at", f.bar_at)' for definition in daily_definitions
    )
    row_available_at = (
        f"greatest(f.bar_at, coalesce(f.fundamental_available_at, f.bar_at), {metric_at_terms})"
        if metric_at_terms
        else "greatest(f.bar_at, coalesce(f.fundamental_available_at, f.bar_at))"
    )

    return f"""
INSERT INTO market_daily_metrics (
    market_daily_id, source, security_id, symbol, trade_date, close, adj_close, volume,
    shares_outstanding, shares_source, shares_reconciliation_ratio,
    {metric_columns},
    fundamental_available_at, available_at, inputs_hash, as_of_date, is_latest_revision, run_id{identity_insert}
)
WITH owner_bridge AS (
    {values_relation_sql(BRIDGE_VALUE_COLUMNS, bridge_row_count)}
), owner_members AS (
    {values_relation_sql(MEMBER_VALUE_COLUMNS, member_row_count)}
), currency_guard AS (
    {values_relation_sql(CURRENCY_VALUE_COLUMNS, currency_row_count)}
), bars_by_session AS (
    -- One row per (security_id, trade_date) -- the physical-row dedup happens
    -- here, BEFORE the lookback rank below, so a corrected/duplicate physical
    -- row for the same session can never split a session's rank from its
    -- sibling's (row_number() over trade_date DESC would otherwise have to
    -- break same-trade_date ties on undefined physical/plan order).
    {bars_by_session}
), bars AS (
    SELECT security_id, trade_date, symbol, close, adj_close, volume, archive_shares, bar_at, cutoff, price_valid
    FROM (
        SELECT *,
               row_number() OVER (
                   PARTITION BY security_id, is_recent ORDER BY trade_date DESC
               ) AS lookback_rank
        FROM bars_by_session
    )
    WHERE is_recent OR lookback_rank <= {_LOOKBACK_ROW_LIMIT}
), vendor_share_links AS (
    {_VENDOR_SHARE_LINKS_SQL}
), {vendor_state}, bars_owned AS (
    -- The bridge rows of one price line are disjoint intervals (validated in
    -- market_owner_bridge), so at most one matches a bar; a link not yet
    -- available at the bar's cutoff leaves owner_key NULL. A bar with a
    -- vendor count uses the count of the latest run known at its cutoff
    -- (NULL while none is: archive_pending), flagged split_pending when that
    -- run started before a split ex-date at or before the bar. A zero vendor
    -- count is no count (4.7% of TickerHistory3 bars carry shares = 0), never
    -- a zero cap; such a bar is labeled vendor_shares_zero.
    SELECT b.* EXCLUDE (archive_shares),
           CASE WHEN b.archive_shares > 0 THEN v.pit_shares END AS archive_shares,
           coalesce(b.archive_shares <= 0, false) AS vendor_zero,
           v.pit_clock AS archive_clock,
           v.pit_available_at AS archive_available_at,
           coalesce(v.run_pending, false) AS archive_pending,
           coalesce(v.split_pending AND NOT v.run_pending, false) AS split_pending,
           o.owner_key, o.identity_basis, o.availability_basis, o.link_method,
           o.issuer_key, o.share_basis, o.adr_ratio,
           coalesce(o.dei_shares_eligible, false) AS dei_shares_eligible,
           coalesce(NOT o.valuation_eligible, false) AS bridge_valuation_withheld,
           coalesce(o.issuer_class_lines, 1) AS issuer_class_lines,
           coalesce(o.sibling_lines, 0) AS sibling_lines
    FROM bars b
    LEFT JOIN vendor_share_state v
      ON v.security_id = b.security_id AND v.trade_date = b.trade_date
    LEFT JOIN owner_bridge o
      ON o.price_security_id = b.security_id
     AND o.valid_from <= b.trade_date
     AND (o.valid_to IS NULL OR b.trade_date < o.valid_to)
     AND o.available_at <= b.cutoff
), fund_long AS (
    SELECT m.owner_key AS security_id, f.canonical_code AS code, f.period_end, f.value, f.available_at,
           f.source AS source_rank, f.rule_id AS rule_rank, f.basis AS basis_rank, f.standardized_id AS state_id,
           to_json(struct_pack(state_id := f.standardized_id, source := f.source, rule_id := f.rule_id,
                               basis := f.basis, value := f.value, available_at := f.available_at)) AS state_lineage
    FROM fundamental_standardized f
    JOIN owner_members m ON m.member_security_id = f.security_id
    WHERE f.basis IN ('quarterly', 'instant') AND f.available_at IS NOT NULL
      AND f.canonical_code IN ({_in_list(item_codes)})
    UNION ALL
    SELECT m.owner_key, d.metric_code AS code, d.period_end, d.value, d.available_at, '', '', '', d.derived_value_id,
           to_json(struct_pack(state_id := d.derived_value_id, inputs_hash := d.inputs_hash,
               value_status := d.value_status, value_origin := d.value_origin,
               fiscal_period_start := d.fiscal_period_start, fiscal_period_end := d.fiscal_period_end))
    FROM derived_metric_values d
    JOIN owner_members m ON m.member_security_id = d.security_id
    WHERE d.source = ?
      AND d.metric_code IN ({_in_list(metric_codes)})
), latest_by_code AS (
    -- Rank whole states, including NULL invalidations, before exposing value.
    -- RANGE sees every equal-time event; stable IDs break ties without load time.
    -- Newest fiscal period still wins over amendments of an older raw period.
    SELECT security_id, code, available_at, state.value AS value, state.lineage AS state_lineage
    FROM (
        SELECT security_id, code, available_at,
               arg_max(struct_pack(value := value, state_id := state_id, lineage := state_lineage),
                       (period_end, available_at, source_rank, rule_rank, basis_rank, state_id)) OVER w AS state,
               row_number() OVER (PARTITION BY security_id, code, available_at
                                  ORDER BY period_end DESC, source_rank DESC, rule_rank DESC,
                                           basis_rank DESC, state_id DESC) AS rk
        FROM fund_long
        WINDOW w AS (PARTITION BY security_id, code ORDER BY available_at
                     RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1
), dei_classes AS (
    -- Data-side class guard: one filing (owner, accession, effective date,
    -- load clock) that carries more than one count or share class is a
    -- per-class DEI disclosure (class_values > 1); no single count of it is
    -- an issuer total, but the class counts sum to one.
    SELECT m.owner_key, coalesce(h.accession_number, h.share_history_id) AS filing_key,
           h.effective_date, h.available_at,
           coalesce(h.share_class, CAST(h.share_count AS VARCHAR)) AS class_key,
           max(h.share_count) AS class_count, max(h.share_history_id) AS state_id
    FROM shares_outstanding_history h
    JOIN owner_members m ON m.member_security_id = h.security_id
    WHERE h.share_count_type = 'shares_outstanding' AND h.taxonomy = 'dei'
      AND h.available_at IS NOT NULL
    GROUP BY m.owner_key, coalesce(h.accession_number, h.share_history_id), h.effective_date, h.available_at,
             coalesce(h.share_class, CAST(h.share_count AS VARCHAR))
), dei_filings AS (
    SELECT owner_key, effective_date, available_at, max(state_id) AS state_id,
           count(*) AS class_values, sum(class_count) AS total_count
    FROM dei_classes
    GROUP BY owner_key, filing_key, effective_date, available_at
), shares_state AS (
    SELECT security_id, available_at,
           state.state_id AS state_id,
           CASE WHEN state.total_count > 0 THEN state.total_count END AS dei_total,
           state.class_values AS class_values,
           state.effective_date AS effective_date
    FROM (
        SELECT owner_key AS security_id, available_at,
               arg_max(struct_pack(total_count := total_count, class_values := class_values,
                                   effective_date := effective_date, state_id := state_id),
                       (effective_date, available_at, state_id)) OVER w AS state,
               row_number() OVER (PARTITION BY owner_key, available_at
                                  ORDER BY effective_date DESC, state_id DESC) AS rk
        FROM dei_filings
        WINDOW w AS (PARTITION BY owner_key ORDER BY available_at
                     RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1
), gaap_state AS (
    -- Issuer-total fallback for the class-sum reconciliation only (never a
    -- line's share count): the latest us-gaap CommonStockSharesOutstanding.
    SELECT security_id, available_at, state.share_count AS gaap_total, state.effective_date AS gaap_effective_date
    FROM (
        SELECT owner_key AS security_id, available_at,
               arg_max(struct_pack(share_count := share_count, effective_date := effective_date),
                       (effective_date, available_at, share_history_id)) OVER w AS state,
               row_number() OVER (PARTITION BY owner_key, available_at
                                  ORDER BY effective_date DESC, share_history_id DESC) AS rk
        FROM (
            SELECT m.owner_key, h.effective_date, h.available_at, h.share_count, h.share_history_id
            FROM shares_outstanding_history h
            JOIN owner_members m ON m.member_security_id = h.security_id
            WHERE h.share_count_type = 'shares_outstanding' AND h.taxonomy = 'us-gaap'
              AND h.concept = 'CommonStockSharesOutstanding' AND h.available_at IS NOT NULL
              AND h.share_count > 0
        )
        WINDOW w AS (PARTITION BY owner_key ORDER BY available_at
                     RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1
), joined AS (
    SELECT b.security_id, b.trade_date, b.symbol, b.close, b.adj_close, b.volume,
           b.archive_shares, b.archive_clock, b.archive_available_at, b.archive_pending, b.split_pending,
           b.vendor_zero, b.price_valid, b.bar_at, b.cutoff, b.owner_key, b.identity_basis,
           b.availability_basis, b.link_method,
           b.issuer_key, b.share_basis, b.adr_ratio, b.dei_shares_eligible, b.bridge_valuation_withheld,
           b.issuer_class_lines, b.sibling_lines,
           s.dei_total, coalesce(s.class_values, 0) AS dei_class_values, s.state_id AS dei_state_id,
           s.available_at AS dei_state_at,
           g.gaap_total,
           -- The share-count anchor: the DEI effective date, else the us-gaap total's.
           CASE WHEN s.dei_total IS NOT NULL THEN s.effective_date ELSE g.gaap_effective_date END AS anchor_date,
           cg.currency_status,
           {projections}
           {"," if projections else ""}{fundamental_at} AS fundamental_available_at
    FROM bars_owned b
    ASOF LEFT JOIN shares_state s
      ON s.security_id = b.owner_key AND b.cutoff >= s.available_at
    ASOF LEFT JOIN gaap_state g
      ON g.security_id = b.owner_key AND b.cutoff >= g.available_at
    LEFT JOIN currency_guard cg
      ON cg.owner_key = b.owner_key AND cg.from_at <= b.cutoff AND (cg.to_at IS NULL OR b.cutoff < cg.to_at)
    {joins}
), anchored AS (
    -- The line's own bar at/before the anchor date (at most {ANCHOR_BAR_MAX_GAP_DAYS} days
    -- earlier, never after this bar): its vendor count as known at this bar's
    -- cutoff (the anchor run, else the run before it) and the price-adjustment
    -- factor change k = (close_e * adj_t) / (adj_e * close_t) since then, and
    -- whether a real split went ex after that anchor bar.
    SELECT j.*,
           CASE WHEN a.trade_date <= j.trade_date AND j.anchor_date - a.trade_date <= {ANCHOR_BAR_MAX_GAP_DAYS}
                     AND a.archive_shares > 0
                THEN CASE WHEN rc.available_at <= j.cutoff THEN rc.run_shares
                          WHEN rc.prior_available_at <= j.cutoff THEN rc.prior_run_shares END
           END AS anchor_archive,
           CASE WHEN a.trade_date <= j.trade_date AND j.anchor_date - a.trade_date <= {ANCHOR_BAR_MAX_GAP_DAYS}
                     AND a.adj_close > 0 AND a.close > 0
                THEN (a.close * j.adj_close) / (a.adj_close * j.close) END AS split_factor,
           coalesce(x.ex_date > a.trade_date, false) AS split_since_anchor
    FROM joined j
    ASOF LEFT JOIN bars_by_session a
      ON a.security_id = j.security_id AND j.anchor_date >= a.trade_date
    ASOF LEFT JOIN run_anchor rc
      ON rc.security_id = j.security_id AND a.trade_date >= rc.run_start
    ASOF LEFT JOIN split_events x
      ON x.security_id = j.security_id AND j.trade_date >= x.ex_date
), classified AS (
    SELECT x.*,
           -- A per-class DEI state makes a single line multi-class.
           CASE WHEN x.share_basis = '{SHARE_BASIS_SINGLE}' AND x.dei_class_values > 1
                THEN '{SHARE_BASIS_MULTI_CLASS}' ELSE x.share_basis END AS basis,
           -- Split-like since the anchor only when a real split went ex in
           -- between (N1): a spin-off or special dividend never withholds the cap.
           coalesce(x.split_since_anchor AND (x.split_factor >= {split_hi} OR x.split_factor <= {split_lo}),
                    false) AS split_like,
           coalesce(abs(x.archive_shares / (x.anchor_archive * x.split_factor) - 1) <= {split_tol}, false)
               AS split_absorbed
    FROM anchored x
), class_groups AS (
    -- One multi-class issuer-day: its bridged class lines trading that day.
    SELECT issuer_key, trade_date,
           count(*) AS lines_priced,
           count(*) FILTER (WHERE archive_shares > 0) AS lines_with_shares,
           sum(archive_shares) FILTER (WHERE archive_shares > 0) AS class_shares,
           sum(close * archive_shares) FILTER (WHERE archive_shares > 0) AS class_cap,
           max(sibling_lines) AS sibling_lines,
           max(issuer_class_lines) AS listed_classes,
           max(dei_class_values) AS dei_class_values,
           max(coalesce(dei_total, gaap_total)) AS issuer_total,
           bool_and(NOT bridge_valuation_withheld) AS class_sum_allowed,
           count(*) FILTER (WHERE split_like AND NOT split_absorbed) AS stale_split_lines,
           count(*) FILTER (WHERE split_pending) AS split_pending_lines,
           count(*) FILTER (WHERE NOT price_valid) AS invalid_price_lines,
           max(archive_available_at) FILTER (WHERE archive_shares > 0) AS class_available_at,
           max(split_factor) FILTER (WHERE split_like) AS split_factor
    FROM classified
    WHERE basis = '{SHARE_BASIS_MULTI_CLASS}'
    GROUP BY issuer_key, trade_date
), resolved_groups AS (
    SELECT issuer_key, trade_date, class_cap, split_pending_lines, class_available_at,
           coalesce(class_sum_allowed AND sibling_lines = 0 AND lines_with_shares = lines_priced
                    AND lines_priced >= greatest(listed_classes, dei_class_values)
                    AND stale_split_lines = 0 AND split_pending_lines = 0 AND invalid_price_lines = 0
                    AND issuer_total > 0
                    AND abs(class_shares / (issuer_total * coalesce(split_factor, 1.0)) - 1) <= {class_tol},
                    false) AS resolved
    FROM class_groups
), decided AS (
    -- A vendor count from a run that spans a split (split_pending) never
    -- prices a bar: not on an archive/ADS line, not in a class sum, not as
    -- the split-adjusted count of a DEI line.
    SELECT c.*,
           CASE
               -- The newest revision of the session has no valid price (N7).
               WHEN NOT c.price_valid THEN 'bar_price_invalid'
               WHEN c.basis = '{SHARE_BASIS_MULTI_CLASS}' THEN
                   CASE WHEN coalesce(r.resolved, false) THEN 'class_sum'
                        WHEN coalesce(r.split_pending_lines > 0, false) THEN 'split_pending_share_update'
                        ELSE 'multiclass_unresolved' END
               WHEN c.basis = '{SHARE_BASIS_ADR}' THEN
                   CASE WHEN NOT coalesce(c.adr_ratio > 0, false) THEN 'adr_ratio_unknown'
                        WHEN c.split_pending THEN 'split_pending_share_update'
                        WHEN c.archive_pending THEN 'archive_run_pending'
                        WHEN c.vendor_zero THEN 'vendor_shares_zero'
                        WHEN c.archive_shares > 0
                             AND NOT coalesce(c.dei_total > 0
                                              AND (c.archive_shares * c.adr_ratio / c.dei_total > {adr_hi}
                                                   OR c.archive_shares * c.adr_ratio / c.dei_total < {adr_lo}),
                                              false)
                        THEN 'archive_ads' ELSE 'adr_ratio_unresolved' END
               WHEN c.basis = '{SHARE_BASIS_SINGLE}' AND c.dei_shares_eligible AND c.dei_total IS NOT NULL THEN
                   CASE WHEN c.split_like THEN
                            CASE WHEN NOT c.split_pending
                                      AND coalesce(abs(c.archive_shares / (c.dei_total * c.split_factor) - 1)
                                                   <= {split_tol}, false)
                                 THEN 'archive_split_adjusted' ELSE 'split_unresolved' END
                        WHEN c.anchor_archive > 0
                             AND (c.dei_total / c.anchor_archive > {basis_hi}
                                  OR c.dei_total / c.anchor_archive < {basis_lo})
                             AND (coalesce(c.archive_shares, 0) <= 0
                                  OR c.dei_total / c.archive_shares > {basis_hi}
                                  OR c.dei_total / c.archive_shares < {basis_lo})
                        THEN 'dei_archive_conflict'
                        ELSE 'dei' END
               WHEN c.split_pending THEN 'split_pending_share_update'
               WHEN c.archive_pending THEN 'archive_run_pending'
               WHEN c.vendor_zero THEN 'vendor_shares_zero'
               WHEN c.archive_shares IS NOT NULL THEN 'archive'
           END AS shares_source,
           CASE WHEN c.basis = '{SHARE_BASIS_MULTI_CLASS}' AND coalesce(r.resolved, false)
                THEN r.class_cap END AS class_market_cap,
           CASE WHEN c.basis = '{SHARE_BASIS_MULTI_CLASS}' AND coalesce(r.resolved, false)
                THEN r.class_available_at END AS class_available_at
    FROM classified c
    LEFT JOIN resolved_groups r
      ON c.basis = '{SHARE_BASIS_MULTI_CLASS}' AND r.issuer_key = c.issuer_key AND r.trade_date = c.trade_date
), panel AS (
    SELECT j.security_id, j.trade_date, j.symbol, j.close, j.adj_close, j.volume,
           j.archive_shares, j.bar_at, j.fundamental_available_at, j.shares_source,
           CASE WHEN j.shares_source = 'dei' THEN j.dei_total END AS dei_shares,
           CASE WHEN j.shares_source = 'dei' THEN j.dei_total
                WHEN j.shares_source IN ({vendor_sources})
                THEN CAST(j.archive_shares AS DOUBLE) END AS "shares_outstanding",
           -- The row's share clock (I3): a DEI filing, or the vendor run clock
           -- (a class-sum row carries its own line's run kind, and the latest
           -- availability over the issuer-day's class lines); NULL when withheld.
           CASE WHEN j.shares_source = 'dei' THEN 'dei_filing'
                WHEN j.shares_source IN ({vendor_sources}) THEN j.archive_clock END AS shares_clock,
           CASE WHEN j.shares_source = 'dei' THEN j.dei_state_at
                WHEN j.shares_source = 'class_sum' THEN j.class_available_at
                WHEN j.shares_source IN ({vendor_sources}) THEN j.archive_available_at END AS shares_available_at,
           CASE WHEN j.basis = '{SHARE_BASIS_SINGLE}' AND j.dei_shares_eligible AND j.dei_total IS NOT NULL
                     AND j.archive_shares IS NOT NULL AND j.archive_shares <> 0
                THEN j.dei_total / j.archive_shares END AS shares_reconciliation_ratio,
           (j.bridge_valuation_withheld OR j.currency_status IS NOT NULL
            OR coalesce(j.shares_source IN ({withheld_sources}), false)) AS valuation_withheld,
           j.bar_at AS "close__at", j.bar_at AS "adj_close__at", j.bar_at AS "volume__at",
           j.bar_at AS "shares_outstanding__at",
           -- log_return(t) = ln(adj_close(t) / adj_close(t-1)) depends on BOTH
           -- t and t-1's bars, mirroring the same lag(1)-availability
           -- composition derived_dsl.py's own lowering uses for every other
           -- lagged quantity (e.g. tret's `_greatest([inner_at, lag(inner_at,
           -- periods)])`); coalescing the lag to j.bar_at when it's NULL
           -- (first row of a security) is safe since j.bar_at is never NULL.
           greatest(j.bar_at,
                    coalesce(lag(j.bar_at) OVER (PARTITION BY j.security_id ORDER BY j.trade_date), j.bar_at))
               AS "log_return__at",
           j.bar_at AS "archive_shares__at", j.bar_at AS "dei_shares__at",
           CASE WHEN lag(j.adj_close) OVER (PARTITION BY j.security_id ORDER BY j.trade_date) > 0
                THEN ln(j.adj_close / lag(j.adj_close) OVER (PARTITION BY j.security_id
                                                             ORDER BY j.trade_date)) END
               AS "log_return",
           j.* EXCLUDE (security_id, trade_date, symbol, close, adj_close, volume,
                        archive_shares, bar_at, cutoff, fundamental_available_at, shares_source)
    FROM decided j
),
{chain_sql}final AS (SELECT * FROM {previous})
SELECT sha256(? || '|' || f.security_id || '|' || CAST(f.trade_date AS VARCHAR)),
       ?, f.security_id, f.symbol, f.trade_date, f.close, f.adj_close, f.volume,
       f."shares_outstanding", f.shares_source, f.shares_reconciliation_ratio,
       {metric_columns},
       f.fundamental_available_at,
       {row_available_at},
       sha256(to_json(struct_pack(security_id := f.security_id, trade_date := f.trade_date,
              close := f.close, adj_close := f.adj_close, shares := f."shares_outstanding",
              shares_source := f.shares_source, dei_state_id := f.dei_state_id, archive_clock := f.archive_clock,
              owner_key := f.owner_key, identity_basis := f.identity_basis,
              availability_basis := f.availability_basis, link_method := f.link_method,
              valuation_withheld := f.valuation_withheld, share_basis := f.basis,
              class_market_cap := f.class_market_cap, currency_status := f.currency_status,
              fundamental_available_at := f.fundamental_available_at, states := {state_lineage}))),
       f.trade_date, true, ?{identity_select}
FROM final f
WHERE {row_available_at} IS NOT NULL
  {output_date_predicate}
ORDER BY f.security_id, f.trade_date
"""


def valuation_dependent_codes(daily_definitions: tuple[DerivedMetricDefinition, ...]) -> frozenset[str]:
    """Daily metrics that (transitively) combine market data with fundamentals.

    ``market_cap`` (price x shares), returns, volatility and dollar volume are
    market-only; anything reading an ``item:`` or a non-daily ``metric:`` --
    or a daily metric that does -- is a valuation metric withheld by the class
    guard. ``daily_definitions`` must be in topological order.
    """
    daily_codes = {definition.metric_code for definition in daily_definitions}
    dependent: set[str] = set()
    for definition in daily_definitions:
        if definition.item_inputs or any(
            code not in daily_codes or code in dependent for code in definition.metric_inputs
        ):
            dependent.add(definition.metric_code)
    return frozenset(dependent)


def _daily_definitions() -> tuple[DerivedMetricDefinition, ...]:
    definitions = default_derived_definitions()
    ordered = topological_order(definitions)
    return tuple(d for d in ordered if d.window == "daily")


def _referenced_codes() -> tuple[tuple[str, ...], tuple[str, ...]]:
    daily = _daily_definitions()
    daily_codes = {d.metric_code for d in daily}
    items: set[str] = set()
    metrics: set[str] = set()
    for definition in daily:
        items.update(definition.item_inputs)
        metrics.update(code for code in definition.metric_inputs if code not in daily_codes)
    return tuple(sorted(items)), tuple(sorted(metrics))


def _market_connection_recycling(store: DuckDBStore) -> bool:
    """Check replay eligibility and refuse caller temp state before mutation."""
    # Only configured persistent callers can replay their analytical budget.
    # Other callers retain their connection and all existing session state.
    recycle_connection = (
        not str(store.path).startswith(":memory:")
        and store.path.is_file()
        and store.analytical_memory_limit is not None
        and store.analytical_threads is not None
    )
    if recycle_connection:
        temporary = store.con.execute("""SELECT EXISTS (
            SELECT 1 FROM duckdb_tables() WHERE temporary AND NOT internal
        ) OR EXISTS (
            SELECT 1 FROM duckdb_views() WHERE temporary AND NOT internal
        )""").fetchone()
        if temporary is None or temporary[0]:
            raise RuntimeError("market daily cannot bound connection lifetime with caller-owned temporary objects")
    return recycle_connection


def refresh_market_daily_metrics(
    store: DuckDBStore,
    options: MarketDailyOptions | None = None,
    *,
    owner_evidence: Sequence[OwnerLinkEvidence] | None = None,
) -> int:
    """Rebuild the panel rows in scope; returns the inserted row count.

    ``owner_evidence`` feeds the strict owner bridge only (no qualified
    evidence source is wired yet, so strict mode links nothing without it).
    The bridge's linked/unlinked accounting is recorded as an
    ``owner_bridge_linkage`` quality row (see :func:`owner_bridge_report`).
    """
    total, _summary = _refresh_market_daily(store, options or MarketDailyOptions(), owner_evidence)
    return total


def _refresh_market_daily(
    store: DuckDBStore,
    options: MarketDailyOptions,
    owner_evidence: Sequence[OwnerLinkEvidence] | None,
) -> tuple[int, dict[str, object]]:
    recycle_connection = _market_connection_recycling(store)
    store.initialize()
    item_codes, metric_codes = _referenced_codes()
    daily = _daily_definitions()

    # One bounded pass per input (a row per price line, the current ticker
    # snapshot, a row per accounting-content id) resolves every line's owner
    # before any batch runs; batches then bind only their own bridge rows.
    bridge = build_market_owner_bridge(
        store,
        mode=options.owner_mode,
        evidence=owner_evidence,
        item_codes=item_codes,
        metric_codes=metric_codes,
        derived_source=options.derived_source,
    )
    # A scoped refresh of one class line computes (and rewrites) its whole
    # issuer group: the multi-class cap sums over every bridged class line.
    identifiers = (
        bridge.expand_to_issuers(options.security_ids) if options.security_ids is not None else bridge.line_ids()
    )
    batches = bridge.owner_aligned_batches(identifiers, options.batch_size)
    identity_columns = _identity_row_columns(store)

    # Critical fix (row-based per fix round 2): the bars CTE never truncates
    # trailing-window metrics for a scoped/incremental refresh. The row-rank
    # restriction itself is unconditional SQL (see build_market_daily_sql's
    # "bars" CTE); what varies here is only the "effective start" bound to
    # `is_recent`. With no caller start_date, _NO_START_DATE_SENTINEL makes
    # every real bar "recent" so the rank restriction is a structural no-op
    # (full history used, exactly like an unscoped run). The caller's actual
    # start_date/end_date restriction is applied separately, only to the
    # emitted rows (see output_date_* below) -- never to the bars CTE.
    effective_start = options.start_date if options.start_date is not None else _NO_START_DATE_SENTINEL

    bars_extra_fragments: list[str] = []
    bars_extra_params: list[Any] = []
    if options.end_date is not None:
        bars_extra_fragments.append("AND trade_date <= ?")
        bars_extra_params.append(options.end_date)
    if options.bar_source is not None:
        bars_extra_fragments.append("AND source = ?")
        bars_extra_params.append(options.bar_source)
    bars_extra_predicate = "\n      ".join(bars_extra_fragments)

    output_date_fragments: list[str] = []
    output_date_params: list[Any] = []
    if options.start_date is not None:
        output_date_fragments.append("AND f.trade_date >= ?")
        output_date_params.append(options.start_date)
    if options.end_date is not None:
        output_date_fragments.append("AND f.trade_date <= ?")
        output_date_params.append(options.end_date)
    output_date_predicate = "\n  ".join(output_date_fragments)

    total = 0
    counts = _ShareBasisCounts()
    for batch in batches:
        bridge_rows = bridge.linked_rows(batch)
        members = bridge.members_for(bridge_rows)
        currency = bridge.currency_intervals(bridge_rows)
        sql = build_market_daily_sql(
            item_codes=item_codes,
            metric_codes=metric_codes,
            daily_definitions=daily,
            security_count=len(batch),
            bars_extra_predicate=bars_extra_predicate,
            output_date_predicate=output_date_predicate,
            bridge_row_count=len(bridge_rows),
            member_row_count=len(members),
            currency_row_count=len(currency),
            identity_columns=identity_columns,
        )
        bind: list[Any] = [
            *bridge_value_params(bridge_rows),
            *member_value_params(members),
            *currency_value_params(currency),
            effective_start,
            *batch,
            *bars_extra_params,
            options.derived_source,
            options.source,
            options.source,
            options.run_id,
            *output_date_params,
        ]
        with store.transaction():
            predicates = ["source = ?", f"security_id IN ({', '.join(['?'] * len(batch))})"]
            params: list[Any] = [options.source, *batch]
            if options.start_date is not None:
                predicates.append("trade_date >= ?")
                params.append(options.start_date)
            if options.end_date is not None:
                predicates.append("trade_date <= ?")
                params.append(options.end_date)
            store.con.execute(f"DELETE FROM market_daily_metrics WHERE {' AND '.join(predicates)}", params)
            # Detach the INSERT count before the transaction commits or the
            # connection closes; neither a cursor nor temp state crosses it.
            affected = store.con.execute(sql, bind).fetchall()
            # Row counts of this batch by share basis, share clock kind and
            # currency status (materialized before the commit, like the INSERT
            # count). The clock is not stored yet (0328), so the count query
            # re-derives the vendor share state from the same bars.
            counts.add(
                store.con.execute(
                    _share_basis_count_sql(
                        bridge_row_count=len(bridge_rows),
                        member_row_count=len(members),
                        currency_row_count=len(currency),
                        security_count=len(batch),
                        bars_extra_predicate=bars_extra_predicate,
                        date_predicates=[p.replace("trade_date", "m.trade_date") for p in predicates[2:]],
                    ),
                    [
                        *bridge_value_params(bridge_rows),
                        *member_value_params(members),
                        *currency_value_params(currency),
                        *batch,
                        *bars_extra_params,
                        options.source,
                        *batch,
                        *params[1 + len(batch) :],
                    ],
                ).fetchall()
            )
        total += int(affected[0][0]) if affected else 0
        if recycle_connection:
            # close() checkpoints after this batch's successful COMMIT;
            # reopen() replays the existing recorded resource settings.
            store.close()
            store.reopen()
    summary = {"source": options.source, "rows": total, **bridge.summary(identifiers), "share_basis": counts.report()}
    _record_owner_bridge(store, summary)
    return total, summary


def _identity_row_columns(store: DuckDBStore) -> tuple[str, ...]:
    """The optional row columns present on ``market_daily_metrics``.

    :data:`IDENTITY_ROW_COLUMNS` (none before A9's 0327) and
    :data:`SHARE_CLOCK_ROW_COLUMNS` (none before 0328).
    """
    present = {
        str(row[0])
        for row in store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'market_daily_metrics' AND NOT internal"
        ).fetchall()
    }
    return tuple(name for name in _OPTIONAL_ROW_COLUMNS if name in present)


def _share_basis_count_sql(
    *,
    bridge_row_count: int,
    member_row_count: int,
    currency_row_count: int,
    security_count: int,
    bars_extra_predicate: str,
    date_predicates: Sequence[str],
) -> str:
    """Count one batch's written rows by bridge share basis, shares_source, currency status and share clock.

    Bind order: bridge rows, member rows, currency rows, the batch ids and the
    ``bars_extra_predicate`` params (the bars the vendor share state is derived
    from, as in the INSERT), then source, the batch ids again, then the
    date-scope params (start, end; however many are present).
    """
    scope = "".join(f" AND {predicate}" for predicate in date_predicates)
    bars = _bars_by_session_sql(
        security_count=security_count, bars_extra_predicate=bars_extra_predicate, with_recent=False
    )
    vendor_state = vendor_share_state_ctes(
        bars="bars_by_session", links="vendor_share_links", targets="bars_by_session"
    )
    return f"""
WITH owner_bridge AS (
    {values_relation_sql(BRIDGE_VALUE_COLUMNS, bridge_row_count)}
), owner_members AS (
    {values_relation_sql(MEMBER_VALUE_COLUMNS, member_row_count)}
), currency_guard AS (
    {values_relation_sql(CURRENCY_VALUE_COLUMNS, currency_row_count)}
), bars_by_session AS (
    {bars}
), vendor_share_links AS (
    {_VENDOR_SHARE_LINKS_SQL}
), {vendor_state}, written AS (
    SELECT m.security_id, m.trade_date, m.shares_source,
           CAST(m.trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR AS cutoff
    FROM market_daily_metrics m
    WHERE m.source = ? AND m.security_id IN ({", ".join(["?"] * security_count)})
      {scope}
)
SELECT o.share_basis, w.shares_source, c.currency_status,
       CASE WHEN w.shares_source = 'dei' THEN 'dei_filing'
            WHEN w.shares_source IN ({_in_list(SHARES_SOURCES_VENDOR)}) THEN v.pit_clock END AS shares_clock,
       count(*)
FROM written w
LEFT JOIN vendor_share_state v
  ON v.security_id = w.security_id AND v.trade_date = w.trade_date
LEFT JOIN owner_bridge o
  ON o.price_security_id = w.security_id AND o.valid_from <= w.trade_date
 AND (o.valid_to IS NULL OR w.trade_date < o.valid_to) AND o.available_at <= w.cutoff
LEFT JOIN currency_guard c
  ON c.owner_key = o.owner_key AND c.from_at <= w.cutoff AND (c.to_at IS NULL OR w.cutoff < c.to_at)
GROUP BY ALL
"""


#: The only ``shares_source`` values a multi-class / ADR bridge row may take.
_CLASS_SOURCES = {
    SHARE_BASIS_MULTI_CLASS: ("class_sum", "multiclass_unresolved", "split_pending_share_update", "bar_price_invalid"),
    SHARE_BASIS_ADR: (
        "archive_ads",
        "adr_ratio_unknown",
        "adr_ratio_unresolved",
        "archive_run_pending",
        "bar_price_invalid",
        "split_pending_share_update",
        "vendor_shares_zero",
    ),
}


def _cross_class_basis(basis: str | None, source: str | None) -> bool:
    """A row priced by DEI outside the single-class basis, or a class/ADR row off its basis."""
    if source == "dei" and basis != SHARE_BASIS_SINGLE:
        return True
    allowed = _CLASS_SOURCES.get(str(basis))
    return allowed is not None and source not in allowed


class _ShareBasisCounts:
    """Accumulates per-batch share-basis / currency row counts into the refresh report."""

    def __init__(self) -> None:
        self.by_source: dict[str, int] = {}
        self.by_basis: dict[str, int] = {}
        self.by_clock: dict[str, int] = {}
        self.currency: dict[str, int] = {}
        self.cross_class = 0

    def add(self, rows: Sequence[tuple[Any, ...]]) -> None:
        for basis, source, currency, clock, count in rows:
            n = int(count)
            self.by_source[str(source)] = self.by_source.get(str(source), 0) + n
            key = "unlinked" if basis is None else str(basis)
            self.by_basis[key] = self.by_basis.get(key, 0) + n
            # Withheld rows (and rows with no share count) carry no share clock.
            clock_key = "none" if clock is None else str(clock)
            self.by_clock[clock_key] = self.by_clock.get(clock_key, 0) + n
            if currency is not None:
                self.currency[str(currency)] = self.currency.get(str(currency), 0) + n
            # B2 gate: a class or ADR line priced by DEI, or any DEI-priced row
            # outside the single-class basis (0 by construction; measured).
            if _cross_class_basis(basis, source):
                self.cross_class += n

    def report(self) -> dict[str, object]:
        return {
            "rows_by_shares_source": dict(sorted(self.by_source.items())),
            "rows_by_bridge_share_basis": dict(sorted(self.by_basis.items())),
            # I3: rows by the clock their share count is known on (dei_filing,
            # or the vendor run clock; modeled_lag/first_run are modeled).
            "rows_by_share_clock": dict(sorted(self.by_clock.items())),
            "multiclass_unresolved_rows": self.by_source.get("multiclass_unresolved", 0),
            "class_sum_rows": self.by_source.get("class_sum", 0),
            "adr_ratio_unknown_rows": self.by_source.get("adr_ratio_unknown", 0),
            "adr_ratio_unresolved_rows": self.by_source.get("adr_ratio_unresolved", 0),
            "split_unresolved_rows": self.by_source.get("split_unresolved", 0),
            "split_pending_share_update_rows": self.by_source.get("split_pending_share_update", 0),
            "archive_run_pending_rows": self.by_source.get("archive_run_pending", 0),
            "vendor_shares_zero_rows": self.by_source.get("vendor_shares_zero", 0),
            "bar_price_invalid_rows": self.by_source.get("bar_price_invalid", 0),
            "dei_archive_conflict_rows": self.by_source.get("dei_archive_conflict", 0),
            # Rows by the owner's currency status (every status withholds the
            # valuation metrics; a row may also be withheld for a share reason).
            "currency_withheld_rows": dict(sorted(self.currency.items())),
            "currency_withheld_total_rows": sum(self.currency.values()),
            "non_usd_null_rows": self.currency.get("non_usd", 0),
            # A canary, 0 by construction (derived from the same bridge rows);
            # not independent evidence.
            "cross_class_share_basis_rows": self.cross_class,
            "shares_availability_basis": dict(SHARES_AVAILABILITY_BASIS),
            "archive_run_clocks": dict(ARCHIVE_RUN_CLOCKS),
            "archive_modeled_lag_days_by_family": dict(ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY),
            "thresholds": {
                "split_factor_min": SPLIT_FACTOR_MIN,
                "split_archive_tolerance": SPLIT_ARCHIVE_TOLERANCE,
                "basis_ratio_limit": BASIS_RATIO_LIMIT,
                "anchor_bar_max_gap_days": ANCHOR_BAR_MAX_GAP_DAYS,
                "class_sum_tolerance": CLASS_SUM_TOLERANCE,
                "adr_ratio_limit": ADR_RATIO_LIMIT,
                "archive_run_rounding_shares": ARCHIVE_RUN_ROUNDING_SHARES,
                "archive_run_cover_window_days": ARCHIVE_RUN_COVER_WINDOW_DAYS,
                "archive_run_dei_lead_days": ARCHIVE_RUN_DEI_LEAD_DAYS,
                "archive_run_public_lookback_days": ARCHIVE_RUN_PUBLIC_LOOKBACK_DAYS,
                "archive_modeled_lag_days": ARCHIVE_MODELED_LAG_DAYS,
                "archive_modeled_lag_days_foreign": ARCHIVE_MODELED_LAG_DAYS_FOREIGN,
                "split_run_tolerance": SPLIT_RUN_TOLERANCE,
            },
        }


def _record_owner_bridge(store: DuckDBStore, summary: dict[str, object]) -> None:
    """Persist the refresh's bridge accounting (skipped on DDL-only fixtures)."""
    exists = store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'data_quality_checks' AND NOT temporary"
    ).fetchone()
    if not exists or not exists[0]:
        return
    unlinked = int(summary.get("unlinked_lines", 0) or 0)  # type: ignore[call-overload]
    quality_check(
        store,
        dataset_id=MarketDailyDataset.dataset_id,
        table_name="market_daily_metrics",
        check_name=OWNER_BRIDGE_CHECK_NAME,
        # Unlinked lines are expected (delisted/renamed names) and counted, not
        # a failure; the row makes their volume and the identity basis durable.
        status="warning" if unlinked else "passed",
        severity="warning",
        observed_value=float(summary.get("linked_lines", 0) or 0),  # type: ignore[arg-type]
        threshold_value=float(summary.get("lines", 0) or 0),  # type: ignore[arg-type]
        details=summary,
    )


def owner_bridge_report(store: DuckDBStore, *, source: str = MARKET_DAILY_SOURCE_NAME) -> dict[str, object] | None:
    """The latest recorded owner-bridge accounting for ``source`` (or ``None``)."""
    row = store.con.execute(
        """
        SELECT details_json FROM data_quality_checks
        WHERE dataset_id = ? AND check_name = ?
          AND json_extract_string(details_json, '$.source') = ?
        ORDER BY checked_at DESC, check_id DESC
        LIMIT 1
        """,
        [MarketDailyDataset.dataset_id, OWNER_BRIDGE_CHECK_NAME, source],
    ).fetchone()
    return None if row is None else dict(json.loads(row[0]))


def shares_reconciliation_report(
    store: DuckDBStore,
    *,
    source: str = MARKET_DAILY_SOURCE_NAME,
    tolerance: float = 0.05,
) -> dict[str, object]:
    """The spec's shares reconciliation gate: dei vs. archive agreement.

    ``meets_spec_gate`` is the spec's "at least 95% of securities [with both
    sources] reconcile within tolerance"; it is ``False`` when no security
    has both sources observed, since a gate cannot pass on zero evidence.

    I2: the pass/fail criterion per security is the *latest* (most recent
    ``trade_date``) observed ``shares_reconciliation_ratio``, not a
    requirement that every historical day be within tolerance. A single
    transient day of drift -- e.g. a DEI filing landing a day before an
    archive-shares correction catches up -- should not permanently fail a
    security for the life of the panel once the two sources have since
    reconciled; the gate reflects the panel's current state, not its history.
    """
    row = store.con.execute(
        """
        WITH latest AS (
            SELECT security_id, shares_reconciliation_ratio,
                   row_number() OVER (
                       PARTITION BY security_id ORDER BY trade_date DESC
                   ) AS rn
            FROM market_daily_metrics
            WHERE source = ? AND shares_reconciliation_ratio IS NOT NULL
        )
        SELECT count(*) FILTER (WHERE rn = 1),
               count(*) FILTER (WHERE rn = 1 AND abs(shares_reconciliation_ratio - 1.0) <= ?)
        FROM latest
        """,
        [source, tolerance],
    ).fetchone()
    both = int(row[0]) if row else 0
    within = int(row[1]) if row else 0
    pass_rate = (within / both) if both else 0.0
    return {
        "securities_with_both_sources": both,
        "securities_within_tolerance": within,
        "pass_rate": pass_rate,
        "tolerance": tolerance,
        "meets_spec_gate": bool(both) and pass_rate >= 0.95,
    }


class MarketDailyDataset(Dataset):
    dataset_id = "market_daily"
    source_name = MARKET_DAILY_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        # Dataset.run initializes and writes its ledger before calling load().
        _market_connection_recycling(store)
        store.initialize()

    def load(self, store: DuckDBStore, options: Any) -> DatasetLoadResult:
        resolved = options if isinstance(options, MarketDailyOptions) else MarketDailyOptions()
        rows, bridge_summary = _refresh_market_daily(store, resolved, None)
        details: dict[str, Any] = dict(
            shares_reconciliation_report(store, source=resolved.source, tolerance=resolved.shares_tolerance)
        )
        # Every run is labeled with its identity basis at top level (RX1).
        details["owner_mode"] = bridge_summary.get("mode")
        details["identity_basis"] = bridge_summary.get("identity_basis")
        details["availability_basis"] = bridge_summary.get("availability_basis")
        # A8: DEI counts are known at their filing's availability; vendor
        # (bar-derived) counts follow the vendor share-run clock.
        details["shares_availability_basis"] = dict(SHARES_AVAILABILITY_BASIS)
        details["archive_run_clocks"] = dict(ARCHIVE_RUN_CLOCKS)
        details["share_basis"] = bridge_summary.get("share_basis")
        details["owner_bridge"] = bridge_summary
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=resolved.source,
            details=details,
        )
