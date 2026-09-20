"""The daily market panel: bars ASOF-joined to point-in-time fundamentals.

``market_daily_metrics`` is a wide, migration-pinned daily panel (one row per
``(security_id, trade_date)``) produced by joining ``equity_daily_bars`` to the
running latest-known standardized fact and quarterly derived metric available
as of the bar's end-of-day availability (``trade_date + 22h``, never
``period_end``), then chaining the declarative ``daily``-window derived
metrics (market cap, valuation multiples, total returns, realized vol) on top.
"""

from __future__ import annotations

import datetime as dt
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

__all__ = [
    "END_OF_DAY_HOURS",
    "MARKET_DAILY_SOURCE_NAME",
    "MarketDailyDataset",
    "MarketDailyOptions",
    "build_market_daily_sql",
    "daily_context",
    "refresh_market_daily_metrics",
    "shares_reconciliation_report",
]

#: Stable provenance tag for rows this engine writes.
MARKET_DAILY_SOURCE_NAME = "atx-db daily market panel v1"

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
    """
    joins: list[str] = []
    projections: list[str] = []
    for index, code in enumerate(codes):
        alias = f"f{index}"
        joins.append(
            f"ASOF LEFT JOIN (SELECT security_id, available_at, value, state_lineage FROM latest_by_code "
            f"WHERE code = {_quote(code)}) {alias} "
            f"ON {alias}.security_id = b.security_id AND b.cutoff >= {alias}.available_at"
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

    Bind order, matching the placeholders in the query text left to right:
    the effective start date first (the ``is_recent`` computation -- it's in
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
WITH bars_by_session AS (
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
), fund_long AS (
    SELECT security_id, canonical_code AS code, period_end, value, available_at,
           source AS source_rank, rule_id AS rule_rank, basis AS basis_rank, standardized_id AS state_id,
           to_json(struct_pack(state_id := standardized_id, source := source, rule_id := rule_id,
                               basis := basis, value := value, available_at := available_at)) AS state_lineage
    FROM fundamental_standardized
    WHERE basis IN ('quarterly', 'instant') AND available_at IS NOT NULL
      AND canonical_code IN ({_in_list(item_codes)})
      AND security_id IN (SELECT DISTINCT security_id FROM bars)
    UNION ALL
    SELECT security_id, metric_code AS code, period_end, value, available_at, '', '', '', derived_value_id,
           to_json(struct_pack(state_id := derived_value_id, inputs_hash := inputs_hash, value_status := value_status))
    FROM derived_metric_values
    WHERE source = ?
      AND metric_code IN ({_in_list(metric_codes)})
      AND security_id IN (SELECT DISTINCT security_id FROM bars)
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
        SELECT security_id, available_at,
               arg_max(struct_pack(share_count := share_count, state_id := share_history_id),
                       (effective_date, available_at, share_history_id)) OVER w AS state,
               row_number() OVER (PARTITION BY security_id, available_at
                                  ORDER BY effective_date DESC, share_history_id DESC) AS rk
        FROM shares_outstanding_history
        WHERE share_count_type = 'shares_outstanding' AND taxonomy = 'dei'
          AND available_at IS NOT NULL
          AND security_id IN (SELECT DISTINCT security_id FROM bars)
        WINDOW w AS (PARTITION BY security_id ORDER BY available_at
                     RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1), joined AS (
    SELECT b.security_id, b.trade_date, b.symbol, b.close, b.adj_close, b.volume,
           b.archive_shares, b.bar_at, b.cutoff,
           s.share_count AS dei_shares,
           s.state_id AS dei_state_id,
           {projections}
           {"," if projections else ""}{fundamental_at} AS fundamental_available_at
    FROM bars b
    ASOF LEFT JOIN shares_state s
      ON s.security_id = b.security_id AND b.cutoff >= s.available_at
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


def refresh_market_daily_metrics(
    store: DuckDBStore,
    options: MarketDailyOptions | None = None,
) -> int:
    options = options or MarketDailyOptions()
    store.initialize()
    item_codes, metric_codes = _referenced_codes()
    daily = _daily_definitions()

    if options.security_ids is not None:
        identifiers = sorted(options.security_ids)
    else:
        identifiers = [
            str(row[0])
            for row in store.con.execute(
                "SELECT DISTINCT security_id FROM equity_daily_bars WHERE close > 0 ORDER BY security_id"
            ).fetchall()
        ]
    size = max(1, int(options.batch_size))
    batches = [tuple(identifiers[i : i + size]) for i in range(0, len(identifiers), size)]

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
        sql = build_market_daily_sql(
            item_codes=item_codes,
            metric_codes=metric_codes,
            daily_definitions=daily,
            security_count=len(batch),
            bars_extra_predicate=bars_extra_predicate,
            output_date_predicate=output_date_predicate,
        )
        bind: list[Any] = [
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
            cursor = store.con.execute(sql, bind)
            affected = cursor.fetchall()
            total += int(affected[0][0]) if affected else 0
    return total


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
        store.initialize()

    def load(self, store: DuckDBStore, options: Any) -> DatasetLoadResult:
        resolved = options if isinstance(options, MarketDailyOptions) else MarketDailyOptions()
        rows = refresh_market_daily_metrics(store, resolved)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=resolved.source,
            details=shares_reconciliation_report(store, source=resolved.source, tolerance=resolved.shares_tolerance),
        )
