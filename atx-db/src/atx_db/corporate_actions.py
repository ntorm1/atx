"""Corporate-action events from the vendor price-adjustment factor (P8).

Every day-over-day step k of a line's vendor factor ``adjusted_close / close``
(consecutive factored bars of the security's primary bar series) becomes one
labelled event, so the events reconstruct the vendor's cumulative factor
(tie-out: ln f_d = ln f_last - sum of ln k over events after d, per line,
within TIE_OUT_TOLERANCE). The split classifier is R1d's corroborated split
epochs (``_split_epochs``), the one the derived engine and ``market_daily``
use:

``split`` / ``stock_dividend``
    a split epoch R1d applies (ratio k, ``split_to:split_from`` its exact p:q
    form); ``stock_dividend`` for an in-band ratio above 1 (21:20 ... 5:4).
    ``corroboration`` is ``share_count`` (the archive count follows the factor)
    or ``vendor_field`` (an explicit vendor ``split_factor``).
``adjustment_unclassified``
    a split candidate R1d cannot resolve (pending or never-confirmed share
    evidence, no share data, conflicting series), a factor step across a data
    gap, or a factor decrease; ``reason`` names the hazard. Never a split.
``cash_dividend``
    a factor residual below SPECIAL_DISTRIBUTION_MIN_YIELD of the prior close:
    cash amount = (1 - 1/k) x prior close; ``vendor_factor+xbrl_dps`` when a
    same-security XBRL dividends-per-share fact matches it.
``distribution_unclassified``
    a residual of at least SPECIAL_DISTRIBUTION_MIN_YIELD (special dividend,
    spin-off-like), or an exact stock-dividend ratio whose share count never
    followed.

Every row's ``details_json`` carries ``evidence_basis``, ``corroboration``,
``reason``, the step ratio and the prior close. ``available_at`` is the ex-date
bar's clock; a split confirmed late by the share count is available only from
that evidence's availability (R1d ``known_at``), and a split candidate R1d reads
as a distribution only once its share window has closed without the count
following (R1d's distribution ``known_at``). Everything is reconstructed
from a later vendor snapshot, never a verified corporate-action record.

The build is set-based: R1d's epochs are staged once per build, and the bars
are read in hash buckets of about ``_split_epochs.STAGE_CHUNK_ROWS`` rows.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any

from . import _split_epochs
from ._split_epochs import SPLIT_MAX_RATIO, SPLIT_MIN_RATIO, _stock_dividend_sql
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check

SOURCE_NAME = "vendor factor corporate action events"
#: The inferred-only producer this dataset replaces; its rows are removed on load.
LEGACY_SOURCE_NAME = "tbltickerhistory inferred corporate actions"
ACTION_TYPES = ("split", "stock_dividend", "cash_dividend", "distribution_unclassified", "adjustment_unclassified")
#: A residual of at least this fraction of the prior close is a special or spin-off-like distribution.
SPECIAL_DISTRIBUTION_MIN_YIELD = 0.10
#: Factor steps with |ln k| below this are numerical noise, not events.
FACTOR_NOISE = 1e-9
TIE_OUT_TOLERANCE = 1e-6
DPS_CONCEPTS = ("CommonStockDividendsPerShareDeclared", "CommonStockDividendsPerShareCashPaid")
#: An XBRL DPS fact corroborates when it matches the event's cash (or the period's sum) within these.
DPS_RELATIVE_TOLERANCE = 0.02
DPS_ABSOLUTE_TOLERANCE = 0.005
DPS_MAX_PERIOD_DAYS = 100
#: R1d hazards that leave a split candidate unresolved (signature hazards are not factor steps).
UNRESOLVED_HAZARDS = ("pending_confirmation", "split_pending_share_confirmation", "split_unconfirmed",
                      "no_share_data", "conflicting_series")
EVENT_COLUMNS = ("source", "security_id", "symbol", "action_type", "ex_date", "declaration_date", "record_date",
                 "payable_date", "cash_amount", "split_from", "split_to", "adjustment_factor", "details_json",
                 "available_at", "run_id")
_HELPERS = ("_ca_bars", "_ca_steps", "_ca_events")


@dataclass(frozen=True)
class CorporateActionsOptions:
    source: str = SOURCE_NAME
    #: Accepted for job-parameter compatibility with the inferred producer; unused (every step is an event).
    min_cash_amount: float = 0.0001
    #: Accepted for job-parameter compatibility with the inferred producer; unused.
    max_dividend_factor: float = 0.999999
    run_id: str | None = None
    security_ids: tuple[str, ...] | None = None
    replace_legacy_inferred: bool = True


def _columns(con: Any, table: str) -> set[str]:
    return {row[0] for row in con.execute(
        "SELECT column_name FROM duckdb_columns() WHERE table_name = ? AND schema_name = 'main' AND NOT internal",
        [table]).fetchall()}


def _in_list(values: tuple[str, ...]) -> str:
    return ", ".join("'" + value.replace("'", "''") + "'" for value in values)


def build_corporate_action_events(con: Any, relation: str, *, source: str = SOURCE_NAME, run_id: str | None = None,
                                  security_ids: tuple[str, ...] | None = None, kind: str = "TEMP ") -> dict[str, Any]:
    """Create ``relation`` (the ``corporate_actions`` columns) with every factor-step event.

    Returns the counts by action type and the per-line tie-out summary.
    """
    columns = ", ".join(f"{name} {kind_}" for name, kind_ in (
        ("source", "VARCHAR"), ("security_id", "VARCHAR"), ("symbol", "VARCHAR"), ("action_type", "VARCHAR"),
        ("ex_date", "DATE"), ("declaration_date", "DATE"), ("record_date", "DATE"), ("payable_date", "DATE"),
        ("cash_amount", "DOUBLE"), ("split_from", "DOUBLE"), ("split_to", "DOUBLE"),
        ("adjustment_factor", "DOUBLE"), ("details_json", "VARCHAR"), ("available_at", "TIMESTAMP"),
        ("run_id", "VARCHAR")))
    con.execute(f"CREATE {kind}TABLE {relation} ({columns})")
    summary: dict[str, Any] = {"lines": 0, "tie_out_max_abs": 0.0, "tie_out_lines_over": 0,
                               "tie_out_tolerance": TIE_OUT_TOLERANCE, "by_type": {}}
    bars = _columns(con, "equity_daily_bars")
    if not {"security_id", "trade_date", "close", "adjusted_close"} <= bars:
        return summary
    series = _split_epochs._series_sql(bars)
    scoped, params = "", []
    if security_ids is not None:
        scoped, params = "AND security_id IN (SELECT unnest(?::VARCHAR[]))", [sorted(set(security_ids))]
    count = con.execute(f"SELECT count(*) FROM equity_daily_bars WHERE true {scoped}", params).fetchone()
    buckets = max(1, -(-int(count[0]) // _split_epochs.STAGE_CHUNK_ROWS))
    holder = SimpleNamespace(con=con)
    try:
        with _split_epochs.refresh_scope(holder, security_ids, persistent=False) as epochs:
            for bucket in range(buckets):
                scope = f"{scoped} AND hash(security_id) % {buckets} = {bucket}" if buckets > 1 else scoped
                tie = _build_chunk(con, relation, epochs, bars, series, scope, params, source, run_id)
                summary["lines"] += tie[0]
                summary["tie_out_max_abs"] = max(summary["tie_out_max_abs"], tie[1] or 0.0)
                summary["tie_out_lines_over"] += tie[2]
    finally:
        for table in _HELPERS:
            con.execute(f"DROP TABLE IF EXISTS {table}")
    summary["by_type"] = dict(con.execute(
        f"SELECT action_type, count(*) FROM {relation} GROUP BY 1 ORDER BY 1").fetchall())
    return summary


def _build_chunk(con: Any, relation: str, epochs: str, bars: set[str], series: str, scope: str,
                 params: list[Any], source: str, run_id: str | None) -> tuple[int, float | None, int]:
    clock = ("coalesce(available_at, trade_date::TIMESTAMP + INTERVAL 22 HOUR)" if "available_at" in bars
             else "trade_date::TIMESTAMP + INTERVAL 22 HOUR")
    symbol = "CAST(symbol AS VARCHAR)" if "symbol" in bars else "NULL::VARCHAR"
    factor = ("CASE WHEN close > 0 AND adjusted_close > 0 AND isfinite(close) AND isfinite(adjusted_close) "
              "THEN adjusted_close / close END")
    # The primary series of each security (most factored bars, then the latest), factored bars only.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_bars AS
        WITH bars AS (
            SELECT security_id, {series} AS series, trade_date, CAST(close AS DOUBLE) AS close, {symbol} AS symbol,
                   {clock} AS available_at, {factor} AS factor
            FROM equity_daily_bars
            WHERE security_id IS NOT NULL AND trade_date IS NOT NULL {scope}
            QUALIFY row_number() OVER (PARTITION BY security_id, {series}, trade_date
                                       ORDER BY {clock} DESC NULLS LAST, adjusted_close DESC NULLS LAST,
                                                close DESC NULLS LAST) = 1
        ), primary_series AS (
            SELECT security_id, series FROM bars WHERE factor IS NOT NULL GROUP BY security_id, series
            QUALIFY row_number() OVER (PARTITION BY security_id
                                       ORDER BY count(*) DESC, max(trade_date) DESC, series) = 1
        )
        SELECT b.security_id, b.trade_date, b.close, b.symbol, b.available_at, ln(b.factor) AS ln_factor
        FROM bars b JOIN primary_series p USING (security_id, series)
        WHERE b.factor IS NOT NULL
    """, params)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_steps AS
        SELECT * FROM (
            SELECT security_id, trade_date AS ex_date, symbol, available_at AS ex_at,
                   ln_factor - lag(ln_factor) OVER w AS ln_k, lag(close) OVER w AS prior_close,
                   date_diff('day', lag(trade_date) OVER w, trade_date) AS gap_days
            FROM _ca_bars WINDOW w AS (PARTITION BY security_id ORDER BY trade_date)
        ) WHERE abs(ln_k) > {FACTOR_NOISE}
    """)
    stock_dividend_grid = _stock_dividend_sql("s.k")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_events AS
        WITH s AS (SELECT *, exp(ln_k) AS k FROM _ca_steps),
        r1d AS (
            SELECT security_id, ex_date,
                   max(ratio) FILTER (WHERE kind = 'split') AS split_ratio,
                   min(known_at) FILTER (WHERE kind = 'split') AS split_known_at,
                   max(evidence) FILTER (WHERE kind = 'split') AS split_evidence,
                   bool_or(kind = 'distribution') AS distribution,
                   min(known_at) FILTER (WHERE kind = 'distribution') AS distribution_known_at,
                   -- Unresolved candidates only: a closed pending hazard belongs to a decided step.
                   nullif(array_to_string(list_sort(list_distinct(list(evidence) FILTER (
                       WHERE kind = 'hazard' AND evidence IN ({_in_list(UNRESOLVED_HAZARDS)})
                         AND NOT (evidence IN ('pending_confirmation', 'split_pending_share_confirmation')
                                  AND until_at IS NOT NULL)))), ','), '') AS hazards
            FROM {epochs} WHERE kind IN ('split', 'distribution', 'hazard')
              AND security_id IN (SELECT DISTINCT security_id FROM _ca_steps)
            GROUP BY ALL
        ), classed AS (
            SELECT s.*, r.split_ratio, r.split_known_at, r.split_evidence, r.hazards, r.distribution_known_at,
                   coalesce(r.distribution, false) AS distribution,
                   1 - 1 / s.k AS yield,
                   CASE
                       WHEN r.split_ratio IS NOT NULL THEN
                           CASE WHEN s.k > 1 AND s.k BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO}
                                     AND r.split_evidence <> 'vendor_split_field'
                                THEN 'stock_dividend' ELSE 'split' END
                       WHEN r.hazards IS NOT NULL THEN 'adjustment_unclassified'
                       WHEN s.gap_days > {_split_epochs.COVERAGE_MAX_GAP_DAYS} THEN 'adjustment_unclassified'
                       WHEN s.k < 1 THEN 'adjustment_unclassified'
                       WHEN coalesce(r.distribution, false) AND s.k BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO}
                            AND {stock_dividend_grid} THEN 'distribution_unclassified'
                       WHEN 1 - 1 / s.k >= {SPECIAL_DISTRIBUTION_MIN_YIELD} THEN 'distribution_unclassified'
                       ELSE 'cash_dividend' END AS action_type,
                   CASE
                       WHEN r.split_ratio IS NOT NULL THEN NULL
                       WHEN r.hazards IS NOT NULL THEN r.hazards
                       WHEN s.gap_days > {_split_epochs.COVERAGE_MAX_GAP_DAYS} THEN 'factor_step_across_data_gap'
                       WHEN s.k < 1 THEN 'factor_decrease'
                       WHEN coalesce(r.distribution, false) AND s.k BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO}
                            AND {stock_dividend_grid} THEN 'stock_dividend_ratio_unconfirmed'
                       WHEN 1 - 1 / s.k >= {SPECIAL_DISTRIBUTION_MIN_YIELD} THEN 'special_or_spinoff'
                       ELSE NULL END AS reason
            FROM s LEFT JOIN r1d r USING (security_id, ex_date)
        )
        SELECT c.*,
               -- The exact p:q form of a split ratio (split_to new shares for split_from old).
               CASE WHEN c.action_type IN ('split', 'stock_dividend') THEN
                   list_filter(range(1, 201), lambda q_x: abs(greatest(c.k, 1 / c.k) * q_x
                       - round(greatest(c.k, 1 / c.k) * q_x)) <= 1e-5 * greatest(c.k, 1 / c.k) * q_x)[1] END
                   AS ratio_q
        FROM classed c
    """)
    tol = f"greatest({DPS_ABSOLUTE_TOLERANCE}, {DPS_RELATIVE_TOLERANCE} * "
    dressed = "SELECT e.*, NULL::STRUCT(value DOUBLE, available_at TIMESTAMP, concept VARCHAR) AS dps FROM ev e"
    if {"security_id", "concept", "unit", "period_start", "period_end", "value", "available_at"} <= _columns(
            con, "sec_company_facts"):
        # A same-security quarterly XBRL DPS fact whose period holds the ex-date and matches the
        # cash (or the period's cash sum). It labels evidence only; its own clock is kept.
        dressed = f"""
            WITH dps AS (
                SELECT security_id, period_start, period_end, value, available_at, concept
                FROM sec_company_facts
                WHERE concept IN ({_in_list(DPS_CONCEPTS)}) AND unit ILIKE '%shares%' AND value > 0
                  AND date_diff('day', period_start, period_end) <= {DPS_MAX_PERIOD_DAYS}
                  AND security_id IN (SELECT security_id FROM ev WHERE action_type = 'cash_dividend')
            ), summed AS (
                SELECT d.*, sum(e.cash) AS period_cash
                FROM dps d JOIN ev e
                  ON e.security_id = d.security_id AND e.action_type = 'cash_dividend'
                 AND e.ex_date BETWEEN d.period_start AND d.period_end
                GROUP BY ALL
            ), matched AS (
                SELECT e.security_id, e.ex_date,
                       struct_pack(value := s.value, available_at := s.available_at, concept := s.concept) AS fact
                FROM ev e JOIN summed s
                  ON s.security_id = e.security_id AND e.ex_date BETWEEN s.period_start AND s.period_end
                WHERE e.action_type = 'cash_dividend'
                  AND (abs(s.value - e.cash) <= {tol}e.cash) OR abs(s.value - s.period_cash) <= {tol}s.period_cash))
                QUALIFY row_number() OVER (PARTITION BY e.security_id, e.ex_date
                                           ORDER BY s.available_at, s.concept) = 1
            )
            SELECT e.*, m.fact AS dps FROM ev e LEFT JOIN matched m USING (security_id, ex_date)
        """
    con.execute(f"""
        INSERT INTO {relation} ({', '.join(EVENT_COLUMNS)})
        WITH ev AS (
            SELECT *, CASE WHEN action_type IN ('cash_dividend', 'distribution_unclassified')
                           THEN yield * prior_close END AS cash
            FROM _ca_events
        ), dressed AS ({dressed})
        SELECT ?, security_id, symbol, action_type, ex_date, NULL, NULL, NULL,
               cash,
               CASE WHEN action_type IN ('split', 'stock_dividend')
                    THEN CASE WHEN ratio_q IS NULL THEN 1.0 WHEN k >= 1 THEN ratio_q
                              ELSE round(ratio_q / k) END END,
               CASE WHEN action_type IN ('split', 'stock_dividend')
                    THEN CASE WHEN ratio_q IS NULL THEN k WHEN k >= 1 THEN round(k * ratio_q)
                              ELSE ratio_q END END,
               1 / k,
               to_json(struct_pack(
                   evidence_basis := CASE
                       WHEN action_type IN ('split', 'stock_dividend') THEN split_evidence
                       WHEN action_type = 'cash_dividend' AND dps IS NOT NULL THEN 'vendor_factor+xbrl_dps'
                       ELSE 'vendor_factor' END,
                   corroboration := CASE split_evidence WHEN 'vendor_factor+shares' THEN 'share_count'
                                                        WHEN 'vendor_split_field' THEN 'vendor_field' END,
                   reason := reason, ratio := k, price_factor := 1 / k, prior_close := prior_close,
                   yield := CASE WHEN k > 1 THEN yield END, gap_days := gap_days,
                   xbrl_dps := dps, classifier := 'r1d_split_epochs', basis := 'vendor_reconstructed')),
               -- Available when the label is decided: a late share confirmation for a split, the
               -- window close for a split candidate R1d reads as a distribution (the count never followed).
               CASE WHEN action_type IN ('split', 'stock_dividend') THEN greatest(ex_at, split_known_at)
                    WHEN distribution THEN greatest(ex_at, coalesce(distribution_known_at, ex_at))
                    ELSE ex_at END,
               ?
        FROM dressed
    """, [source, run_id])
    # Tie-out: the events rebuild the vendor's cumulative factor on every bar of every line.
    tie = con.execute(f"""
        WITH marked AS (
            SELECT b.security_id, b.trade_date, b.ln_factor,
                   last_value(b.ln_factor) OVER (PARTITION BY b.security_id ORDER BY b.trade_date
                       ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS ln_last,
                   coalesce(sum(-ln(e.adjustment_factor)) OVER (PARTITION BY b.security_id ORDER BY b.trade_date
                       ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING), 0) AS ln_after
            FROM _ca_bars b
            LEFT JOIN (SELECT security_id, ex_date, adjustment_factor FROM {relation}
                       WHERE security_id IN (SELECT DISTINCT security_id FROM _ca_bars)) e
              ON e.security_id = b.security_id AND e.ex_date = b.trade_date
        ), lines AS (
            SELECT security_id, max(abs(ln_factor - ln_last + ln_after)) AS deviation FROM marked GROUP BY 1
        )
        SELECT count(*), max(deviation), count(*) FILTER (WHERE deviation > {TIE_OUT_TOLERANCE}) FROM lines
    """).fetchone()
    return int(tie[0]), tie[1], int(tie[2])


class CorporateActionsDataset(Dataset):
    dataset_id = "corporate_actions"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: CorporateActionsOptions) -> DatasetLoadResult:
        relation = "_ca_build"
        store.con.execute(f"DROP TABLE IF EXISTS {relation}")
        try:
            summary = build_corporate_action_events(store.con, relation, source=options.source,
                                                    run_id=options.run_id, security_ids=options.security_ids)
            sources = [options.source] + ([LEGACY_SOURCE_NAME] if options.replace_legacy_inferred else [])
            scope, params = "", list(sources)
            if options.security_ids is not None:
                scope, params = "AND security_id IN (SELECT unnest(?::VARCHAR[]))", [*sources,
                                                                                      sorted(options.security_ids)]
            with store.transaction():
                store.con.execute(f"DELETE FROM corporate_actions WHERE source IN "
                                  f"({', '.join('?' for _ in sources)}) {scope}", params)
                store.con.execute(f"INSERT INTO corporate_actions ({', '.join(EVENT_COLUMNS)}) "
                                  f"SELECT {', '.join(EVENT_COLUMNS)} FROM {relation}")
            rows = int(store.con.execute(f"SELECT count(*) FROM {relation}").fetchone()[0])
        finally:
            store.con.execute(f"DROP TABLE IF EXISTS {relation}")
        quality_check(
            store, dataset_id=self.dataset_id, table_name="corporate_actions", check_name="rows_loaded",
            status="passed" if rows > 0 else "warning", observed_value=float(rows), threshold_value=1.0,
            details={"source": options.source, "by_type": summary["by_type"]},
        )
        quality_check(
            store, dataset_id=self.dataset_id, table_name="corporate_actions", check_name="factor_tie_out",
            status="passed" if summary["tie_out_lines_over"] == 0 else "failed",
            observed_value=float(summary["tie_out_max_abs"]), threshold_value=TIE_OUT_TOLERANCE,
            details={key: summary[key] for key in ("lines", "tie_out_lines_over", "tie_out_tolerance")},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id, rows_loaded=rows, source=options.source,
            details=json.loads(json.dumps(summary, default=str)),
        )
