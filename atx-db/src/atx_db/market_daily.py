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
    MEMBER_VALUE_COLUMNS,
    OWNER_MODE_RECONSTRUCTED,
    OWNER_MODE_STRICT,
    OWNER_MODES,
    OwnerLinkEvidence,
    bridge_value_params,
    build_market_owner_bridge,
    member_value_params,
    values_relation_sql,
)
from .warehouse import quality_check

__all__ = [
    "END_OF_DAY_HOURS",
    "MARKET_DAILY_SOURCE_NAME",
    "MARKET_DAILY_STRICT_SOURCE_NAME",
    "OWNER_BRIDGE_CHECK_NAME",
    "MarketDailyDataset",
    "MarketDailyOptions",
    "build_market_daily_sql",
    "daily_context",
    "owner_bridge_report",
    "refresh_market_daily_metrics",
    "shares_reconciliation_report",
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
) -> str:
    """Return the full ``INSERT`` statement for one batch of securities.

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
    of that owner and re-keyed to ``owner_key`` before the ASOF joins. DEI
    shares join only when the row is ``dei_shares_eligible``.

    Bind order, matching the placeholders in the query text left to right:
    the bridge rows then the member rows (7 and 2 values per row, in
    ``BRIDGE_VALUE_COLUMNS``/``MEMBER_VALUE_COLUMNS`` order), then
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
    state_lineage = "[" + ", ".join(
        f'struct_pack(code := {_quote(code)}, state := f."{code}__state_lineage")' for code in codes
    ) + "]" if codes else "'[]'"
    securities = ", ".join(["?"] * security_count)
    availability_terms = ", ".join(
        f"coalesce(f{index}.available_at, TIMESTAMP '-infinity')" for index in range(len(codes))
    )
    fundamental_at = (
        f"nullif(greatest({availability_terms}), TIMESTAMP '-infinity')" if codes else "CAST(NULL AS TIMESTAMP)"
    )

    chain: list[str] = []
    previous = "panel"
    for rank, definition in enumerate(daily_definitions):
        names = tuple(definition.item_inputs) + tuple(definition.metric_inputs) + tuple(definition.market_inputs)
        lowered = compile_expression(definition.expression, daily_context(names=names))
        alias = f"m{rank}"
        chain.append(
            f"""{alias} AS (
    SELECT p.*,
           {lowered.value_sql} AS "{definition.metric_code}",
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
    fundamental_available_at, available_at, inputs_hash, as_of_date, is_latest_revision, run_id
)
WITH owner_bridge AS (
    {values_relation_sql(BRIDGE_VALUE_COLUMNS, bridge_row_count)}
), owner_members AS (
    {values_relation_sql(MEMBER_VALUE_COLUMNS, member_row_count)}
), bars_by_session AS (
    -- One row per (security_id, trade_date) -- the physical-row dedup happens
    -- here, BEFORE the lookback rank below, so a corrected/duplicate physical
    -- row for the same session can never split a session's rank from its
    -- sibling's (row_number() over trade_date DESC would otherwise have to
    -- break same-trade_date ties on undefined physical/plan order).
    SELECT security_id, trade_date,
           arg_max(symbol, (available_at, source)) AS symbol,
           arg_max(close, (available_at, source)) AS close,
           arg_max(adjusted_close, (available_at, source)) AS adj_close,
           arg_max(volume, (available_at, source)) AS volume,
           arg_max(shares_outstanding, (available_at, source)) AS archive_shares,
           greatest(max(available_at),
                    CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR) AS bar_at,
           CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR AS cutoff,
           (trade_date >= ?) AS is_recent
    FROM equity_daily_bars
    WHERE close > 0 AND adjusted_close > 0 AND trade_date IS NOT NULL
      AND security_id IN ({securities})
      {bars_extra_predicate}
    GROUP BY security_id, trade_date
), bars AS (
    SELECT security_id, trade_date, symbol, close, adj_close, volume, archive_shares, bar_at, cutoff
    FROM (
        SELECT *,
               row_number() OVER (
                   PARTITION BY security_id, is_recent ORDER BY trade_date DESC
               ) AS lookback_rank
        FROM bars_by_session
    )
    WHERE is_recent OR lookback_rank <= {_LOOKBACK_ROW_LIMIT}
), bars_owned AS (
    -- The bridge rows of one price line are disjoint intervals (validated in
    -- market_owner_bridge), so at most one matches a bar; a link not yet
    -- available at the bar's cutoff leaves owner_key NULL.
    SELECT b.*, o.owner_key, o.identity_basis,
           CASE WHEN o.dei_shares_eligible THEN o.owner_key END AS dei_owner_key
    FROM bars b
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
), shares_state AS (
    SELECT security_id, available_at,
           state.state_id AS state_id,
           CASE WHEN state.share_count > 0 THEN state.share_count END AS share_count
    FROM (
        SELECT m.owner_key AS security_id, h.available_at,
               arg_max(struct_pack(share_count := h.share_count, state_id := h.share_history_id),
                       (h.effective_date, h.available_at, h.share_history_id)) OVER w AS state,
               row_number() OVER (PARTITION BY m.owner_key, h.available_at
                                  ORDER BY h.effective_date DESC, h.share_history_id DESC) AS rk
        FROM shares_outstanding_history h
        JOIN owner_members m ON m.member_security_id = h.security_id
        WHERE h.share_count_type = 'shares_outstanding' AND h.taxonomy = 'dei'
          AND h.available_at IS NOT NULL
        WINDOW w AS (PARTITION BY m.owner_key ORDER BY h.available_at
                     RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1), joined AS (
    SELECT b.security_id, b.trade_date, b.symbol, b.close, b.adj_close, b.volume,
           b.archive_shares, b.bar_at, b.cutoff, b.owner_key, b.identity_basis,
           s.share_count AS dei_shares,
           s.state_id AS dei_state_id,
           {projections}
           {"," if projections else ""}{fundamental_at} AS fundamental_available_at
    FROM bars_owned b
    ASOF LEFT JOIN shares_state s
      ON s.security_id = b.dei_owner_key AND b.cutoff >= s.available_at
    {joins}
), panel AS (
    SELECT j.security_id, j.trade_date, j.symbol, j.close, j.adj_close, j.volume,
           j.archive_shares, j.dei_shares, j.bar_at, j.fundamental_available_at,
           coalesce(j.dei_shares, CAST(j.archive_shares AS DOUBLE)) AS "shares_outstanding",
           CASE WHEN j.dei_shares IS NOT NULL THEN 'dei'
                WHEN j.archive_shares IS NOT NULL THEN 'archive' END AS shares_source,
           CASE WHEN j.dei_shares IS NULL OR j.archive_shares IS NULL OR j.archive_shares = 0
                THEN NULL ELSE j.dei_shares / j.archive_shares END AS shares_reconciliation_ratio,
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
                        archive_shares, dei_shares, bar_at, cutoff, fundamental_available_at)
    FROM joined j
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
              shares_source := f.shares_source, dei_state_id := f.dei_state_id,
              owner_key := f.owner_key, identity_basis := f.identity_basis,
              fundamental_available_at := f.fundamental_available_at, states := {state_lineage}))),
       f.trade_date, true, ?
FROM final f
WHERE {row_available_at} IS NOT NULL
  {output_date_predicate}
ORDER BY f.security_id, f.trade_date
"""


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
    identifiers = sorted(options.security_ids) if options.security_ids is not None else bridge.line_ids()
    batches = bridge.owner_aligned_batches(identifiers, options.batch_size)

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
    for batch in batches:
        bridge_rows = bridge.linked_rows(batch)
        members = bridge.members_for(bridge_rows)
        sql = build_market_daily_sql(
            item_codes=item_codes,
            metric_codes=metric_codes,
            daily_definitions=daily,
            security_count=len(batch),
            bars_extra_predicate=bars_extra_predicate,
            output_date_predicate=output_date_predicate,
            bridge_row_count=len(bridge_rows),
            member_row_count=len(members),
        )
        bind: list[Any] = [
            *bridge_value_params(bridge_rows),
            *member_value_params(members),
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
        total += int(affected[0][0]) if affected else 0
        if recycle_connection:
            # close() checkpoints after this batch's successful COMMIT;
            # reopen() replays the existing recorded resource settings.
            store.close()
            store.reopen()
    summary = {"source": options.source, "rows": total, **bridge.summary(identifiers)}
    _record_owner_bridge(store, summary)
    return total, summary


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
        details["owner_bridge"] = bridge_summary
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=resolved.source,
            details=details,
        )
