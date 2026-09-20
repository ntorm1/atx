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
            f"ASOF LEFT JOIN (SELECT security_id, available_at, value FROM latest_by_code "
            f"WHERE code = {_quote(code)}) {alias} "
            f"ON {alias}.security_id = b.security_id AND b.cutoff >= {alias}.available_at"
        )
        projections.append(f'{alias}.value AS "{code}"')
        projections.append(f'{alias}.available_at AS "{code}__at"')
    return "\n    ".join(joins), ",\n           ".join(projections)


def build_market_daily_sql(
    *,
    item_codes: tuple[str, ...],
    metric_codes: tuple[str, ...],
    daily_definitions: tuple[DerivedMetricDefinition, ...],
    security_count: int,
    date_predicate: str,
) -> str:
    """Return the full ``INSERT`` statement for one batch of securities.

    Bind order, matching the placeholders in the query text left to right:
    the batch's security ids (for ``bars``), then the date-predicate params
    (start/end/bar_source, in that order, however many of the three are
    present), then ``derived_source`` (the ``fund_long`` union's derived-side
    filter), then ``source`` twice (the id hash, then the ``source`` column),
    then ``run_id``.
    """
    codes = tuple(item_codes) + tuple(metric_codes)
    joins, projections = _asof_joins(codes)
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

    return f"""
INSERT INTO market_daily_metrics (
    market_daily_id, source, security_id, symbol, trade_date, close, adj_close, volume,
    shares_outstanding, shares_source, shares_reconciliation_ratio,
    {metric_columns},
    fundamental_available_at, available_at, inputs_hash, as_of_date, is_latest_revision, run_id
)
WITH bars AS (
    SELECT security_id, trade_date,
           arg_max(symbol, (available_at, source)) AS symbol,
           arg_max(close, (available_at, source)) AS close,
           arg_max(adjusted_close, (available_at, source)) AS adj_close,
           arg_max(volume, (available_at, source)) AS volume,
           arg_max(shares_outstanding, (available_at, source)) AS archive_shares,
           greatest(max(available_at),
                    CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR) AS bar_at,
           CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR AS cutoff
    FROM equity_daily_bars
    WHERE close > 0 AND adjusted_close > 0 AND trade_date IS NOT NULL
      AND security_id IN ({securities})
      {date_predicate}
    GROUP BY security_id, trade_date
), fund_long AS (
    SELECT security_id, canonical_code AS code, period_end, value, available_at, revision_sequence
    FROM fundamental_standardized
    WHERE is_latest_revision AND basis IN ('quarterly', 'instant')
      AND canonical_code IN ({_in_list(item_codes)})
    UNION ALL
    SELECT security_id, metric_code AS code, period_end, value, available_at, 0
    FROM derived_metric_values
    WHERE source = ?
      AND metric_code IN ({_in_list(metric_codes)})
), latest_by_code AS (
    -- The running latest-known value: at each availability event the code
    -- carries the value of the newest fiscal period a consumer could have
    -- known, so a later-arriving restatement of an OLDER period never
    -- overwrites a newer one. A plain ASOF on available_at alone would.
    SELECT security_id, code, available_at, value
    FROM (
        SELECT security_id, code, available_at,
               arg_max(value, (period_end, available_at, revision_sequence)) OVER w AS value,
               row_number() OVER (PARTITION BY security_id, code, available_at
                                  ORDER BY period_end DESC, revision_sequence DESC) AS rk
        FROM fund_long
        WINDOW w AS (PARTITION BY security_id, code
                     ORDER BY available_at, period_end, revision_sequence
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1
), shares_state AS (
    -- Same running-latest-value discipline for the DEI share count: the
    -- newest effective_date known as of each available_at, never overwritten
    -- by a later-arriving revision of an older effective_date.
    SELECT security_id, available_at, share_count
    FROM (
        SELECT security_id, available_at,
               arg_max(share_count, (effective_date, available_at, revision_sequence)) OVER w
                   AS share_count,
               row_number() OVER (PARTITION BY security_id, available_at
                                  ORDER BY effective_date DESC, revision_sequence DESC) AS rk
        FROM shares_outstanding_history
        WHERE is_latest_revision AND share_count_type = 'shares_outstanding'
          AND available_at IS NOT NULL AND share_count > 0
        WINDOW w AS (PARTITION BY security_id
                     ORDER BY available_at, effective_date, revision_sequence
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1
), joined AS (
    SELECT b.security_id, b.trade_date, b.symbol, b.close, b.adj_close, b.volume,
           b.archive_shares, b.bar_at, b.cutoff,
           s.share_count AS dei_shares,
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
           j.bar_at AS "shares_outstanding__at", j.bar_at AS "log_return__at",
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
       greatest(f.bar_at, coalesce(f.fundamental_available_at, f.bar_at)),
       sha256(f.security_id || '|' || CAST(f.trade_date AS VARCHAR) || '|' ||
              CAST(f.close AS VARCHAR) || '|' || CAST(f.adj_close AS VARCHAR) || '|' ||
              coalesce(CAST(f."shares_outstanding" AS VARCHAR), '') || '|' ||
              coalesce(f.shares_source, '') || '|' ||
              coalesce(CAST(f.fundamental_available_at AS VARCHAR), '')),
       f.trade_date, true, ?
FROM final f
WHERE greatest(f.bar_at, coalesce(f.fundamental_available_at, f.bar_at)) IS NOT NULL
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

    date_fragments: list[str] = []
    date_params: list[Any] = []
    if options.start_date is not None:
        date_fragments.append("AND trade_date >= ?")
        date_params.append(options.start_date)
    if options.end_date is not None:
        date_fragments.append("AND trade_date <= ?")
        date_params.append(options.end_date)
    if options.bar_source is not None:
        date_fragments.append("AND source = ?")
        date_params.append(options.bar_source)
    date_predicate = "\n      ".join(date_fragments)

    total = 0
    for batch in batches:
        sql = build_market_daily_sql(
            item_codes=item_codes,
            metric_codes=metric_codes,
            daily_definitions=daily,
            security_count=len(batch),
            date_predicate=date_predicate,
        )
        bind: list[Any] = [
            *batch,
            *date_params,
            options.derived_source,
            options.source,
            options.source,
            options.run_id,
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
    """
    row = store.con.execute(
        """
        WITH per_security AS (
            SELECT security_id,
                   count(*) FILTER (WHERE shares_reconciliation_ratio IS NOT NULL) AS observed,
                   count(*) FILTER (
                       WHERE shares_reconciliation_ratio IS NOT NULL
                         AND abs(shares_reconciliation_ratio - 1.0) <= ?
                   ) AS within
            FROM market_daily_metrics
            WHERE source = ?
            GROUP BY security_id
        )
        SELECT count(*) FILTER (WHERE observed > 0),
               count(*) FILTER (WHERE observed > 0 AND within = observed)
        FROM per_security
        """,
        [tolerance, source],
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
