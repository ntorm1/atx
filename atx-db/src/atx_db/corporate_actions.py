"""Corporate-action events from the vendor price-adjustment factor (P8).

Every day-over-day step k of a line's vendor factor ``adjusted_close / close``
(consecutive factored bars of the security's primary bar series) becomes one
labelled event, so the events reconstruct the vendor's cumulative factor
(tie-out: ln f_d = ln f_last - sum of ln k over events after d, per line,
within TIE_OUT_TOLERANCE; a load that fails it writes nothing). The split
classifier is R1d's corroborated split epochs (``_split_epochs``), the one the
derived engine and ``market_daily`` use:

``split`` / ``stock_dividend``
    a split epoch R1d applies (ratio k, ``split_to:split_from`` its exact p:q
    form); ``stock_dividend`` for an in-band ratio above 1 (21:20 ... 5:4).
    ``corroboration`` is ``share_count`` (the archive count follows the factor)
    or ``vendor_field`` (an explicit vendor ``split_factor``).
``adjustment_unclassified``
    a split-sized step whose classification is pending at its clock
    (PENDING_REASONS: ``split_candidate_pending`` out of band,
    ``distribution_pending_classification`` in band; a ``conflicting_series``
    candidate, which has no decision clock, stays pending), a candidate R1d
    decided cannot be resolved (``split_unconfirmed``, ``no_share_data`` at
    R1d's verdict clock: the late window's close plus the line's family lag,
    as a matching count could stay unpublished that long), an R1d-confirmed
    out-of-band ratio that is not exact and lacks split evidence of its own
    (``split_ratio_implausible``, M2 / Ruling C-17, :func:`_stage_m2`: a raw
    price that moves with the factor, a line that is not fund-like, and a
    discrete count move by the ratio whose residual is at most a same-day
    dividend; rejects year-end fund distributions, spin-offs, E&P purges and
    misdated factor steps whose loose count happened to match -- R1d itself
    still applies them, so here P8 and the derived engine differ), a factor
    step across a data gap, a factor decrease, or float noise (``factor_noise``:
    a factor increase whose yield 1 - 1/k is below FACTOR_NOISE_YIELD). Never a
    split.
``cash_dividend``
    a factor residual below SPECIAL_DISTRIBUTION_MIN_YIELD of the prior close:
    cash amount = (1 - 1/k) x prior close; upgraded to
    ``vendor_factor+xbrl_dps`` when a same-security quarterly USD XBRL
    dividends-per-share fact matches it, from that fact's clock.
``distribution_unclassified``
    a residual of at least SPECIAL_DISTRIBUTION_MIN_YIELD (special dividend,
    spin-off-like), or an exact stock-dividend ratio whose share count never
    followed.

Revisions (point in time). Each step has a stable id (``details.step_id``,
:func:`step_id_sql`: md5 of ``source|security_id|ex_date``) and one row per
state, each available from the clock its state was decidable: (0) a step
whose label is decided later (a late share confirmation; a negative verdict
at R1d's verdict clock -- a share window closing on a distribution, including
a factor decrease R1d reads as one, or the late window closing on an
unconfirmed split) or not by the end of the data first has a pending row at
the ex-date bar clock; (1) the
decided label at its decision clock; (2) an XBRL corroboration of a cash
dividend at max(decision clock, the fact's ``available_at``). Revisions are
numbered by clock (``details.revision``); ``details.event_id`` =
md5(step_id|revision); a later revision names ``supersedes_event_id``; a
superseded row carries ``superseded_at`` and ``is_latest_revision = false``.
:func:`corporate_actions_asof_sql` is the as-of relation (each step's latest
row visible at a cutoff); :func:`corporate_actions_current_sql` is each step's
first decided revision at its own clock, pending steps absent (for event
consumers that count economics once).

Every row's ``details_json`` carries ``evidence_basis``, ``corroboration``,
``reason``, the step ratio and the prior close. Everything is reconstructed
from a later vendor snapshot, never a verified corporate-action record.

The primary series of a security is chosen among the (source|run_id) series
that carry a vendor factor anywhere (R1d's factor-series set; a factor-free
load never carries events); factored bars of any other series are counted,
never silently dropped. The build is set-based: R1d's epochs are staged once
per build, the XBRL facts once, and the bars are read in hash buckets of about
``_split_epochs.STAGE_CHUNK_ROWS`` rows. The load raises before its
transaction when the tie-out fails, and replaces its rows through an
INSERT-only table swap (``corporate_actions`` has a ``DEFAULT now()`` column),
then checkpoints.
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
#: Reasons of a step whose classification is not decided yet at the row's clock.
PENDING_REASONS = ("split_candidate_pending", "distribution_pending_classification")
#: A residual of at least this fraction of the prior close is a special or spin-off-like distribution.
SPECIAL_DISTRIBUTION_MIN_YIELD = 0.10
#: Factor steps with |ln k| below this are numerical noise, not events.
FACTOR_NOISE = 1e-9
#: A factor increase whose yield 1 - 1/k is below this is float noise (``factor_noise``), never a cash dividend.
FACTOR_NOISE_YIELD = 1e-5
TIE_OUT_TOLERANCE = 1e-6
DPS_CONCEPTS = ("CommonStockDividendsPerShareDeclared", "CommonStockDividendsPerShareCashPaid")
#: XBRL per-share units compared with the (USD) cash amount.
DPS_UNITS = ("USD/SHARES", "USD/SHARE")
#: An XBRL DPS fact corroborates when it matches the event's cash (or the period's sum) within these.
DPS_RELATIVE_TOLERANCE = 0.02
DPS_ABSOLUTE_TOLERANCE = 0.005
DPS_MAX_PERIOD_DAYS = 100
#: R1d hazards still open at the end of the data: the step's classification is pending.
OPEN_HAZARDS = ("pending_confirmation", "split_pending_share_confirmation")
#: R1d hazards decided only when the late share window closes.
LATE_HAZARDS = ("split_unconfirmed", "no_share_data")
#: R1d hazards with no decision clock (two series disagree): the step stays pending.
UNDATED_HAZARDS = ("conflicting_series",)
#: R1d hazards that leave a split candidate unresolved (signature hazards are not factor steps).
UNRESOLVED_HAZARDS = (*OPEN_HAZARDS, *LATE_HAZARDS, *UNDATED_HAZARDS)
#: M2 (Ruling C-17): an R1d-confirmed out-of-band step whose ratio is not exact (not p/q with q <= 10) is a
#: split only with split evidence of its own (see ``_stage_m2``); otherwise it is this. Never a split.
SPLIT_RATIO_IMPLAUSIBLE = "split_ratio_implausible"
#: (a) a discrete count move: one bar's count ratio r (count / the previous count) within R1d's window with
#: |k / r - 1| at most this (a whole split, not cumulative fund creations).
M2_JUMP_TOLERANCE = 0.02
#: (c) a residual k / r - 1 beyond this (either way) must be a same-day dividend: positive, on a line with a
#: cash-dividend step in the M2_HISTORY_BARS bars before the step.
M2_EXACT_RESIDUAL = 0.001
#: (b) the raw close moves with the factor on the step bar: max(p, 1/p) of price ratio x k at most this
#: (rejects price-flat and misdated factor steps).
M2_PRICE_TOLERANCE = 1.5
M2_HISTORY_BARS = 260
#: (d) a fund-like line: at least this many vendor count changes in the M2_HISTORY_BARS bars before the step
#: (creations/redemptions; an operating company's count changes a few times a year).
M2_FUND_COUNT_CHANGES = 24
#: Reasons of split-sized steps that are never applied as splits (``daily_adjustments`` labels them).
UNAPPLIED_SPLIT_REASONS = (*PENDING_REASONS, *UNRESOLVED_HAZARDS, SPLIT_RATIO_IMPLAUSIBLE)
EVENT_COLUMNS = ("source", "security_id", "symbol", "action_type", "ex_date", "declaration_date", "record_date",
                 "payable_date", "cash_amount", "split_from", "split_to", "adjustment_factor", "details_json",
                 "available_at", "run_id")
_HELPERS = ("_ca_raw", "_ca_series", "_ca_bars", "_ca_steps", "_ca_classed", "_ca_m2", "_ca_events", "_ca_dressed")
_BUILD_HELPERS = ("_ca_factor_series", "_ca_dps")


def _json_text(field: str, alias: str = "") -> str:
    column = f"{alias}.details_json" if alias else "details_json"
    return f"CASE WHEN json_valid({column}) THEN json_extract_string({column}, '$.{field}') END"


def step_id_sql(source: str, security_id: str, ex_date: str) -> str:
    """The stable id of one P8 step (SQL expressions in): md5 of ``source|security_id|ex_date``.

    A line has one step per ex-date (its primary series), so the id is the same in every build and
    every revision of the step; ``details.event_id`` = md5(step_id|revision) names one revision.
    """
    return f"md5(concat_ws('|', {source}, {security_id}, CAST({ex_date} AS VARCHAR)))"


def _step_revision_sql(order: str) -> str:
    """Row number of a P8 row among its step's revisions (``order``: ASC or DESC by clock)."""
    return (f"row_number() OVER (PARTITION BY c.source, c.security_id, c.ex_date, {_json_text('step_id', 'c')} "
            f"ORDER BY c.available_at {order} NULLS LAST, "
            f"try_cast({_json_text('revision', 'c')} AS INTEGER) {order} NULLS LAST)")


def corporate_actions_asof_sql(cutoff: str, relation: str = "corporate_actions") -> str:
    """Each P8 step's latest row visible at ``cutoff`` (a SQL timestamp expression): the as-of relation.

    Rows of other producers (no ``step_id``) are returned as they are; rows with no ``available_at``
    are always visible.
    """
    return f"""
        SELECT c.* FROM {relation} c
        WHERE c.available_at IS NULL OR c.available_at <= {cutoff}
        QUALIFY {_json_text('step_id', 'c')} IS NULL OR {_step_revision_sql('DESC')} = 1
    """


def corporate_actions_current_sql(relation: str = "corporate_actions") -> str:
    """Each P8 step's first decided revision, at its own clock; other producers' rows as they are.

    For event consumers that use the economic fields once per step (factor
    history, dividend metrics). A step is absent while it is pending (no
    pending row, so presence never tells which way it resolves), appears at its
    decision clock with its decided label (a late split at its confirmation, an
    unconfirmed split or an R1d distribution at R1d's verdict clock), and a
    later XBRL corroboration -- the same economics -- is not in this relation
    (its evidence lives in the revision at its own clock). No row ever shows a
    later state at an earlier clock.
    """
    return f"""
        SELECT c.* FROM {relation} c
        WHERE {_json_text('step_id', 'c')} IS NULL OR coalesce({_json_text('pending', 'c')}, 'false') <> 'true'
        QUALIFY {_json_text('step_id', 'c')} IS NULL OR {_step_revision_sql('ASC')} = 1
    """


@dataclass(frozen=True)
class CorporateActionsOptions:
    source: str = SOURCE_NAME
    #: Accepted for job-parameter compatibility with the inferred producer; unused (every step is an event).
    min_cash_amount: float = 0.0001
    #: Accepted for job-parameter compatibility with the inferred producer; unused.
    max_dividend_factor: float = 0.999999
    run_id: str | None = None
    security_ids: tuple[str, ...] | None = None
    #: Remove the legacy inferred producer's rows. False is refused while such rows exist in scope:
    #: ``adjustment_factor_history`` sums every source, so both would double count every dividend.
    replace_legacy_inferred: bool = True


def _columns(con: Any, table: str) -> set[str]:
    return {row[0] for row in con.execute(
        "SELECT column_name FROM duckdb_columns() WHERE table_name = ? AND schema_name = 'main' AND NOT internal",
        [table]).fetchall()}


def _in_list(values: tuple[str, ...]) -> str:
    return ", ".join("'" + value.replace("'", "''") + "'" for value in values)


def build_corporate_action_events(con: Any, relation: str, *, source: str = SOURCE_NAME, run_id: str | None = None,
                                  security_ids: tuple[str, ...] | None = None, kind: str = "TEMP ") -> dict[str, Any]:
    """Create ``relation`` (the ``corporate_actions`` columns plus ``is_latest_revision``) with every revision.

    Returns the counts by action type (latest revisions), the revision counts,
    the series accounting and the per-line tie-out summary.
    """
    columns = ", ".join(f"{name} {kind_}" for name, kind_ in (
        ("source", "VARCHAR"), ("security_id", "VARCHAR"), ("symbol", "VARCHAR"), ("action_type", "VARCHAR"),
        ("ex_date", "DATE"), ("declaration_date", "DATE"), ("record_date", "DATE"), ("payable_date", "DATE"),
        ("cash_amount", "DOUBLE"), ("split_from", "DOUBLE"), ("split_to", "DOUBLE"),
        ("adjustment_factor", "DOUBLE"), ("details_json", "VARCHAR"), ("available_at", "TIMESTAMP"),
        ("run_id", "VARCHAR"), ("is_latest_revision", "BOOLEAN")))
    con.execute(f"CREATE {kind}TABLE {relation} ({columns})")
    summary: dict[str, Any] = {"lines": 0, "tie_out_max_abs": 0.0, "tie_out_lines_over": 0,
                               "tie_out_tolerance": TIE_OUT_TOLERANCE, "by_type": {}, "revisions": 0,
                               "pending_latest": 0, "bars_outside_primary_series": 0,
                               "lines_with_other_series": 0, "lines_without_factor_series": 0}
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
        # R1d's table-wide factor-series set; the XBRL facts are read once.
        con.execute(f"CREATE OR REPLACE TEMP TABLE _ca_factor_series AS {_split_epochs.factor_series_sql(bars)}")
        dps = _stage_dps(con, scoped, params)
        with _split_epochs.refresh_scope(holder, security_ids, persistent=False) as epochs:
            for bucket in range(buckets):
                scope = f"{scoped} AND hash(security_id) % {buckets} = {bucket}" if buckets > 1 else scoped
                chunk = _build_chunk(con, relation, epochs, bars, series, scope, params, source, run_id, dps)
                summary["lines"] += chunk["lines"]
                summary["tie_out_max_abs"] = max(summary["tie_out_max_abs"], chunk["deviation"] or 0.0)
                summary["tie_out_lines_over"] += chunk["over"]
                for key in ("bars_outside_primary_series", "lines_with_other_series", "lines_without_factor_series"):
                    summary[key] += chunk[key]
    finally:
        for table in (*_HELPERS, *_BUILD_HELPERS):
            con.execute(f"DROP TABLE IF EXISTS {table}")
    summary["by_type"] = dict(con.execute(
        f"SELECT action_type, count(*) FROM {relation} WHERE is_latest_revision GROUP BY 1 ORDER BY 1").fetchall())
    summary["revisions"] = int(con.execute(
        f"SELECT count(*) FROM {relation} WHERE NOT is_latest_revision").fetchone()[0])
    summary["pending_latest"] = int(con.execute(
        f"SELECT count(*) FROM {relation} WHERE is_latest_revision AND {_json_text('pending')} = 'true'"
    ).fetchone()[0])
    return summary


def _stage_dps(con: Any, scoped: str, params: list[Any]) -> bool:
    """Stage same-security quarterly USD XBRL dividends-per-share facts once; False when there are none."""
    needed = {"security_id", "concept", "unit", "period_start", "period_end", "value", "available_at"}
    if not needed <= _columns(con, "sec_company_facts"):
        return False
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_dps AS
        SELECT security_id, period_start, period_end, value, available_at, concept
        FROM sec_company_facts
        WHERE concept IN ({_in_list(DPS_CONCEPTS)}) AND upper(replace(unit, ' ', '')) IN ({_in_list(DPS_UNITS)})
          AND value > 0 AND date_diff('day', period_start, period_end) <= {DPS_MAX_PERIOD_DAYS} {scoped}
    """, params)
    return True


def _build_chunk(con: Any, relation: str, epochs: str, bars: set[str], series: str, scope: str,
                 params: list[Any], source: str, run_id: str | None, dps: bool) -> dict[str, Any]:
    clock = ("coalesce(available_at, trade_date::TIMESTAMP + INTERVAL 22 HOUR)" if "available_at" in bars
             else "trade_date::TIMESTAMP + INTERVAL 22 HOUR")
    symbol = "CAST(symbol AS VARCHAR)" if "symbol" in bars else "NULL::VARCHAR"
    shares = ("CASE WHEN shares_outstanding > 0 THEN CAST(shares_outstanding AS DOUBLE) END"
              if "shares_outstanding" in bars else "NULL::DOUBLE")
    factor = ("CASE WHEN close > 0 AND adjusted_close > 0 AND isfinite(close) AND isfinite(adjusted_close) "
              "THEN adjusted_close / close END")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_raw AS
        SELECT security_id, {series} AS series, trade_date, CAST(close AS DOUBLE) AS close, {symbol} AS symbol,
               {clock} AS available_at, {factor} AS factor, {shares} AS shares
        FROM equity_daily_bars
        WHERE security_id IS NOT NULL AND trade_date IS NOT NULL {scope}
        QUALIFY row_number() OVER (PARTITION BY security_id, {series}, trade_date
                                   ORDER BY {clock} DESC NULLS LAST, adjusted_close DESC NULLS LAST,
                                            close DESC NULLS LAST) = 1
    """, params)
    # The primary series of each security: a factor-carrying series (most factored bars, then the latest).
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ca_series AS
        SELECT *, coalesce(row_number() OVER (
                      PARTITION BY security_id, carries_factor ORDER BY factored DESC, last_date DESC, series) = 1
                      AND carries_factor, false) AS is_primary
        FROM (
            SELECT security_id, series, count(factor) AS factored, max(trade_date) AS last_date,
                   series IN (SELECT series FROM _ca_factor_series) AS carries_factor
            FROM _ca_raw GROUP BY security_id, series
        )
        WHERE factored > 0
    """)
    accounting = con.execute("""
        SELECT coalesce(sum(factored) FILTER (WHERE NOT is_primary), 0),
               count(DISTINCT security_id) FILTER (WHERE NOT is_primary),
               count(DISTINCT security_id) - count(DISTINCT security_id) FILTER (WHERE is_primary)
        FROM _ca_series
    """).fetchone()
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ca_bars AS
        SELECT r.security_id, r.trade_date, r.close, r.symbol, r.available_at, ln(r.factor) AS ln_factor
        FROM _ca_raw r JOIN _ca_series p ON p.security_id = r.security_id AND p.series = r.series AND p.is_primary
        WHERE r.factor IS NOT NULL
    """)
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
    band = f"BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO}"
    gap = _split_epochs.COVERAGE_MAX_GAP_DAYS
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_classed AS
        WITH s AS (SELECT *, exp(ln_k) AS k FROM _ca_steps),
        r1d AS (
            SELECT security_id, ex_date,
                   max(ratio) FILTER (WHERE kind = 'split') AS split_ratio,
                   min(known_at) FILTER (WHERE kind = 'split') AS split_known_at,
                   max(evidence) FILTER (WHERE kind = 'split') AS split_evidence,
                   bool_or(kind = 'distribution') AS distribution,
                   min(known_at) FILTER (WHERE kind = 'distribution') AS distribution_known_at,
                   -- R1d's negative verdicts are final only once any count in their window is public.
                   max(known_at) FILTER (WHERE kind = 'hazard') AS verdict_at,
                   -- Unresolved candidates only: a closed pending hazard belongs to a decided step.
                   list_sort(list_distinct(list(evidence) FILTER (
                       WHERE kind = 'hazard' AND evidence IN ({_in_list(UNRESOLVED_HAZARDS)})
                         AND NOT (evidence IN ({_in_list(OPEN_HAZARDS)}) AND until_at IS NOT NULL)))) AS hazard_list
            FROM {epochs} WHERE kind IN ('split', 'distribution', 'hazard')
              AND security_id IN (SELECT DISTINCT security_id FROM _ca_steps)
            GROUP BY ALL
        ), classed AS (
            SELECT s.*, r.split_ratio, r.split_known_at, r.split_evidence, r.distribution_known_at, r.verdict_at,
                   coalesce(r.distribution, false) AS distribution,
                   nullif(array_to_string(r.hazard_list, ','), '') AS hazards,
                   coalesce(list_has_any(r.hazard_list, [{_in_list((*OPEN_HAZARDS, *UNDATED_HAZARDS))}]), false)
                       AS open_hazard,
                   coalesce(list_has_any(r.hazard_list, [{_in_list(LATE_HAZARDS)}]), false) AS late_hazard,
                   1 - 1 / s.k AS yield,
                   -- what is knowable at the ex-date clock about a step whose label is decided later
                   CASE WHEN s.k {band} THEN 'distribution_pending_classification'
                        ELSE 'split_candidate_pending' END AS pending_reason,
                   -- M2: a confirmed out-of-band ratio that is not exact needs split evidence of its own.
                   coalesce(r.split_ratio IS NOT NULL AND r.split_evidence <> 'vendor_split_field'
                            AND s.k NOT {band} AND NOT {_split_epochs._simple_sql('s.k')}, false) AS m2_candidate,
                   CASE
                       WHEN r.split_ratio IS NOT NULL THEN
                           CASE WHEN s.k > 1 AND s.k {band} AND r.split_evidence <> 'vendor_split_field'
                                THEN 'stock_dividend' ELSE 'split' END
                       WHEN len(r.hazard_list) > 0 THEN 'adjustment_unclassified'
                       WHEN s.gap_days > {gap} THEN 'adjustment_unclassified'
                       WHEN s.k > 1 AND 1 - 1 / s.k < {FACTOR_NOISE_YIELD} THEN 'adjustment_unclassified'
                       WHEN s.k < 1 THEN 'adjustment_unclassified'
                       WHEN coalesce(r.distribution, false) AND s.k {band} AND {stock_dividend_grid}
                           THEN 'distribution_unclassified'
                       WHEN 1 - 1 / s.k >= {SPECIAL_DISTRIBUTION_MIN_YIELD} THEN 'distribution_unclassified'
                       ELSE 'cash_dividend' END AS action_type,
                   CASE
                       WHEN r.split_ratio IS NOT NULL THEN NULL
                       WHEN len(r.hazard_list) > 0 THEN array_to_string(r.hazard_list, ',')
                       WHEN s.gap_days > {gap} THEN 'factor_step_across_data_gap'
                       WHEN s.k > 1 AND 1 - 1 / s.k < {FACTOR_NOISE_YIELD} THEN 'factor_noise'
                       WHEN s.k < 1 THEN 'factor_decrease'
                       WHEN coalesce(r.distribution, false) AND s.k {band} AND {stock_dividend_grid}
                           THEN 'stock_dividend_ratio_unconfirmed'
                       WHEN 1 - 1 / s.k >= {SPECIAL_DISTRIBUTION_MIN_YIELD} THEN 'special_or_spinoff'
                       ELSE NULL END AS reason
            FROM s LEFT JOIN r1d r USING (security_id, ex_date)
        )
        SELECT c.*,
               -- The exact p:q form of a split ratio (split_to new shares for split_from old).
               CASE WHEN c.action_type IN ('split', 'stock_dividend') THEN
                   list_filter(range(1, 201), lambda q_x: abs(greatest(c.k, 1 / c.k) * q_x
                       - round(greatest(c.k, 1 / c.k) * q_x)) <= 1e-5 * greatest(c.k, 1 / c.k) * q_x)[1] END
                   AS ratio_q,
               -- When the label is decidable; NULL: still pending at the end of the data. A step R1d
               -- read as a distribution is decided at R1d's clocks whatever its label here (a factor
               -- decrease that could have confirmed as a reverse split is pending until then too); a
               -- negative verdict waits for R1d's verdict clock (window end plus the family lag).
               CASE WHEN c.split_ratio IS NOT NULL THEN greatest(c.ex_at, c.split_known_at)
                    WHEN c.open_hazard THEN NULL
                    WHEN c.late_hazard THEN c.verdict_at
                    WHEN c.distribution THEN greatest(c.ex_at, coalesce(c.distribution_known_at, c.ex_at),
                                                      coalesce(c.verdict_at, c.ex_at))
                    ELSE c.ex_at END AS decided_at
        FROM classed c
    """)
    _stage_m2(con)
    # M2 verdicts replace the split label (and its clock) of the candidates.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_events AS
        SELECT c.* REPLACE (
                   CASE WHEN m.implausible THEN 'adjustment_unclassified' ELSE c.action_type END AS action_type,
                   CASE WHEN m.implausible THEN '{SPLIT_RATIO_IMPLAUSIBLE}' ELSE c.reason END AS reason,
                   CASE WHEN c.m2_candidate THEN m.decided_at ELSE c.decided_at END AS decided_at),
               m.evidence AS m2
        FROM _ca_classed c LEFT JOIN _ca_m2 m USING (security_id, ex_date)
    """)
    tol = f"greatest({DPS_ABSOLUTE_TOLERANCE}, {DPS_RELATIVE_TOLERANCE} * "
    dressed = "SELECT e.*, NULL::STRUCT(value DOUBLE, available_at TIMESTAMP, concept VARCHAR) AS dps FROM ev e"
    if dps:
        # A fact whose period holds the ex-date and matches the cash (or the period's cash sum).
        dressed = f"""
            WITH facts AS (
                SELECT * FROM _ca_dps
                WHERE security_id IN (SELECT security_id FROM ev WHERE action_type = 'cash_dividend')
            ), summed AS (
                SELECT d.*, sum(e.cash) AS period_cash
                FROM facts d JOIN ev e
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
                                           ORDER BY s.available_at, s.concept, s.period_start, s.period_end,
                                                    s.value) = 1
            )
            SELECT e.*, m.fact AS dps FROM ev e LEFT JOIN matched m USING (security_id, ex_date)
        """
    split_from = "CASE WHEN ratio_q IS NULL THEN 1.0 WHEN k >= 1 THEN ratio_q ELSE round(ratio_q / k) END"
    split_to = "CASE WHEN ratio_q IS NULL THEN k WHEN k >= 1 THEN round(k * ratio_q) ELSE ratio_q END"
    event_id = "md5(step_id || '|' || CAST({} AS VARCHAR))"
    # One row per step, with its stable id and the clock of its XBRL corroboration: never before the
    # fact's own clock, and never before the label it corroborates is decided.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_dressed AS
        WITH ev AS (
            SELECT *, CASE WHEN action_type IN ('cash_dividend', 'distribution_unclassified')
                           THEN yield * prior_close END AS cash
            FROM _ca_events
        ), dressed AS ({dressed})
        SELECT d.*, {step_id_sql('?', 'd.security_id', 'd.ex_date')} AS step_id,
               CASE WHEN d.action_type = 'cash_dividend' AND d.decided_at IS NOT NULL AND d.dps IS NOT NULL
                    THEN greatest(d.decided_at, d.dps.available_at) END AS xbrl_at
        FROM dressed d
    """, [source])
    con.execute(f"""
        INSERT INTO {relation} ({', '.join(EVENT_COLUMNS)}, is_latest_revision)
        WITH states AS (
            -- (0) pending at the ex-date clock: the label is decided later, or not by the end of the data.
            SELECT s.*, s.ex_at AS row_at, 'adjustment_unclassified' AS row_type, s.pending_reason AS row_reason,
                   true AS pending, CASE WHEN false THEN s.dps END AS row_dps
            FROM _ca_dressed s WHERE s.decided_at IS NULL OR s.decided_at > s.ex_at
            UNION ALL
            -- (1) the decided label at its decision clock, with XBRL evidence only if already public then.
            SELECT s.*, s.decided_at, s.action_type, s.reason, false,
                   CASE WHEN s.xbrl_at = s.decided_at THEN s.dps END
            FROM _ca_dressed s WHERE s.decided_at IS NOT NULL
            UNION ALL
            -- (2) a cash dividend corroborated by a later XBRL dividends-per-share fact, from its clock.
            SELECT s.*, s.xbrl_at, s.action_type, s.reason, false, s.dps
            FROM _ca_dressed s WHERE s.xbrl_at > s.decided_at
        ), revisions AS (
            -- Revisions of a step are numbered by clock; each is superseded at the next one's clock.
            SELECT *, row_number() OVER (PARTITION BY step_id ORDER BY row_at) - 1 AS revision,
                   lead(row_at) OVER (PARTITION BY step_id ORDER BY row_at) AS superseded_at
            FROM states
        )
        SELECT ?, security_id, symbol, row_type, ex_date, NULL, NULL, NULL,
               CASE WHEN row_type IN ('cash_dividend', 'distribution_unclassified') THEN cash END,
               CASE WHEN row_type IN ('split', 'stock_dividend') THEN {split_from} END,
               CASE WHEN row_type IN ('split', 'stock_dividend') THEN {split_to} END,
               1 / k,
               to_json(struct_pack(
                   evidence_basis := CASE
                       WHEN row_type IN ('split', 'stock_dividend') THEN split_evidence
                       WHEN row_type = 'cash_dividend' AND row_dps IS NOT NULL THEN 'vendor_factor+xbrl_dps'
                       ELSE 'vendor_factor' END,
                   corroboration := CASE WHEN row_type IN ('split', 'stock_dividend') THEN
                       CASE split_evidence WHEN 'vendor_factor+shares' THEN 'share_count'
                                           WHEN 'vendor_split_field' THEN 'vendor_field' END END,
                   reason := row_reason, pending := pending, ratio := k, price_factor := 1 / k,
                   prior_close := prior_close, yield := CASE WHEN k > 1 THEN yield END, gap_days := gap_days,
                   xbrl_dps := row_dps, m2 := CASE WHEN NOT pending THEN m2 END, step_id := step_id,
                   revision := revision,
                   event_id := {event_id.format('revision')},
                   supersedes_event_id := CASE WHEN revision > 0 THEN {event_id.format('revision - 1')} END,
                   superseded_at := superseded_at, classifier := 'r1d_split_epochs', basis := 'vendor_reconstructed')),
               row_at, ?, superseded_at IS NULL
        FROM revisions
    """, [source, run_id])
    tie = _tie_out(con, relation)
    return {"lines": int(tie[0]), "deviation": tie[1], "over": int(tie[2]),
            "bars_outside_primary_series": int(accounting[0]), "lines_with_other_series": int(accounting[1]),
            "lines_without_factor_series": int(accounting[2])}


def _stage_m2(con: Any) -> None:
    """``_ca_m2``: the point-in-time M2 verdict of each candidate of ``_ca_classed`` (Ruling C-17).

    A confirmed out-of-band step whose ratio is not exact is a split only if all hold:
    (b) the raw close moves with the factor on the step bar (max(x, 1/x) of price ratio x k at most
    M2_PRICE_TOLERANCE); (d) the line is not fund-like (fewer than M2_FUND_COUNT_CHANGES vendor count
    changes in the M2_HISTORY_BARS bars before the step, up to the pre-step bar SHARE_LEAD_BARS + 1 before
    it: a lead bar's count may be unpublished at the verdict); (a) a discrete count move corroborates it: a
    bar of R1d's window (SHARE_LEAD_BARS before to SHARE_WINDOW_BARS after the step) whose count ratio r to
    the previous count has |k/r - 1| at most M2_JUMP_TOLERANCE; (c) and whose residual k/r - 1, beyond
    M2_EXACT_RESIDUAL, is a same-day dividend (positive, on a line with a cash-dividend step in the
    M2_HISTORY_BARS bars before). Otherwise it is ``split_ratio_implausible`` (fail closed).

    Clocks: (b) and (d) are known at the step bar, so failing either decides the label at R1d's
    confirmation clock; a split is decided at the later of R1d's confirmation and the first corroborating
    bar whose previous count is public (R1d's A8 run clock: a count run is public at its first bar when it
    is the line's first, else the longest family lag after it -- the previous count can be an unpublished
    intermediate run); no corroborating bar is a verdict only once the window has closed and any count in it
    would be public (window end + the longest A8 family lag); until then the step is pending.
    """
    lead, window = _split_epochs.SHARE_LEAD_BARS, _split_epochs.SHARE_WINDOW_BARS
    lag_days = max(_split_epochs.SHARE_MODELED_LAG_DAYS.values())
    qualifies = (f"abs(j.residual) <= {M2_JUMP_TOLERANCE} AND (abs(j.residual) <= {M2_EXACT_RESIDUAL} "
                 f"OR (j.residual > 0 AND coalesce(h.cash_steps, 0) > 0))")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_m2 AS
        WITH cand AS (
            SELECT security_id, ex_date, k, ex_at, split_known_at FROM _ca_classed WHERE m2_candidate
        ), bars AS (
            -- Every bar of the primary series (a NULL-factor bar still carries a count), numbered.
            SELECT r.security_id, r.trade_date, r.close, r.shares, r.available_at,
                   row_number() OVER w AS bar_no, lag(r.close) OVER w AS prior_close,
                   last_value(r.shares IGNORE NULLS) OVER (PARTITION BY r.security_id ORDER BY r.trade_date
                       ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS prev_shares
            FROM _ca_raw r
            JOIN _ca_series p ON p.security_id = r.security_id AND p.series = r.series AND p.is_primary
            WHERE r.security_id IN (SELECT security_id FROM cand)
            WINDOW w AS (PARTITION BY r.security_id ORDER BY r.trade_date)
        ), runs AS (
            -- Vendor count runs (R1d's A8 model): a run starts where the count changes; run 1 is the first count.
            SELECT *, sum(CASE WHEN shares IS NOT NULL AND shares IS DISTINCT FROM prev_shares THEN 1 ELSE 0 END)
                          OVER (PARTITION BY security_id ORDER BY trade_date ROWS UNBOUNDED PRECEDING) AS share_run
            FROM bars
        ), published AS (
            -- When a run's count is public: at its first bar for the line's first run, else the family lag after.
            SELECT security_id, share_run,
                   CASE WHEN share_run = 1 THEN min(available_at)
                        ELSE min(available_at) + INTERVAL {lag_days} DAY END AS public_at
            FROM runs WHERE shares IS NOT NULL GROUP BY security_id, share_run
        ), at_ex AS (
            SELECT c.*, b.bar_no AS ex_no, b.close / b.prior_close AS price_ratio
            FROM cand c JOIN bars b ON b.security_id = c.security_id AND b.trade_date = c.ex_date
        ), history AS (
            SELECT x.security_id, x.ex_date,
                   count(*) FILTER (WHERE b.shares IS NOT NULL AND b.prev_shares IS NOT NULL
                                      AND b.shares <> b.prev_shares AND b.bar_no <= x.ex_no - {lead + 1})
                       AS count_changes,
                   count(e.ex_date) AS cash_steps
            FROM at_ex x
            JOIN bars b ON b.security_id = x.security_id AND b.bar_no BETWEEN x.ex_no - {M2_HISTORY_BARS} AND x.ex_no - 1
            LEFT JOIN _ca_classed e ON e.security_id = b.security_id AND e.ex_date = b.trade_date
                 AND e.action_type = 'cash_dividend'
            GROUP BY x.security_id, x.ex_date
        ), jumps AS (
            -- A count move is evidence only once its previous count is public too.
            SELECT x.security_id, x.ex_date, greatest(b.available_at, p.public_at) AS public_at,
                   b.shares / b.prev_shares AS r, x.k / (b.shares / b.prev_shares) - 1 AS residual
            FROM at_ex x
            JOIN runs b ON b.security_id = x.security_id AND b.bar_no BETWEEN x.ex_no - {lead} AND x.ex_no + {window}
            JOIN published p ON p.security_id = b.security_id AND p.share_run = b.share_run - 1
            WHERE b.shares > 0 AND b.prev_shares > 0 AND b.shares <> b.prev_shares
        ), corroborated AS (
            SELECT j.security_id, j.ex_date,
                   min(j.public_at) FILTER (WHERE {qualifies}) AS jump_at,
                   arg_min(j.r, j.public_at) FILTER (WHERE {qualifies}) AS first_r,
                   arg_min(j.r, abs(j.residual)) AS closest_r
            FROM jumps j LEFT JOIN history h USING (security_id, ex_date)
            GROUP BY j.security_id, j.ex_date
        ), windows AS (
            SELECT x.security_id, x.ex_date, count(*) = {window + 1} AS complete, max(b.available_at) AS end_at
            FROM at_ex x JOIN bars b ON b.security_id = x.security_id AND b.bar_no BETWEEN x.ex_no AND x.ex_no + {window}
            GROUP BY x.security_id, x.ex_date
        ), judged AS (
            SELECT x.*, coalesce(h.count_changes, 0) AS count_changes, coalesce(h.cash_steps, 0) AS cash_steps,
                   c.jump_at, coalesce(c.first_r, c.closest_r) AS share_ratio, w.complete, w.end_at,
                   coalesce(greatest(x.price_ratio * x.k, 1 / (x.price_ratio * x.k)) <= {M2_PRICE_TOLERANCE}, false)
                       AS price_moves,
                   coalesce(h.count_changes, 0) >= {M2_FUND_COUNT_CHANGES} AS fund_like
            FROM at_ex x
            LEFT JOIN history h USING (security_id, ex_date)
            LEFT JOIN corroborated c USING (security_id, ex_date)
            LEFT JOIN windows w USING (security_id, ex_date)
        ), verdicts AS (
            SELECT *, CASE WHEN NOT price_moves THEN 'price_did_not_move'
                           WHEN fund_like THEN 'fund_like_count'
                           WHEN jump_at IS NOT NULL THEN 'discrete_count_move'
                           WHEN complete THEN 'no_discrete_count_move' END AS verdict
            FROM judged
        )
        SELECT security_id, ex_date,
               CASE WHEN verdict IS NULL THEN NULL ELSE verdict <> 'discrete_count_move' END AS implausible,
               CASE verdict WHEN 'discrete_count_move' THEN greatest(ex_at, split_known_at, jump_at)
                            WHEN 'no_discrete_count_move'
                                THEN greatest(ex_at, split_known_at, end_at + INTERVAL {lag_days} DAY)
                            WHEN 'price_did_not_move' THEN greatest(ex_at, split_known_at)
                            WHEN 'fund_like_count' THEN greatest(ex_at, split_known_at) END AS decided_at,
               -- Evidence known by the verdict's clock only (the window's counts only once it has closed).
               struct_pack(verdict := verdict, price_ratio_x_k := price_ratio * k, count_changes := count_changes,
                           cash_steps := cash_steps,
                           share_ratio := CASE WHEN verdict IN ('discrete_count_move', 'no_discrete_count_move')
                                               THEN share_ratio END,
                           residual := CASE WHEN verdict IN ('discrete_count_move', 'no_discrete_count_move')
                                            THEN k / share_ratio - 1 END,
                           corroborated_at := CASE WHEN verdict = 'discrete_count_move' THEN jump_at END) AS evidence
        FROM verdicts
    """)


def _tie_out(con: Any, relation: str) -> tuple[int, float | None, int]:
    """(lines, max |deviation|, lines over TIE_OUT_TOLERANCE) of the bucket's lines in ``_ca_bars``.

    One (latest) row per step must rebuild the vendor's cumulative factor on every bar of every line.
    """
    tie = con.execute(f"""
        WITH marked AS (
            SELECT b.security_id, b.trade_date, b.ln_factor,
                   last_value(b.ln_factor) OVER (PARTITION BY b.security_id ORDER BY b.trade_date
                       ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS ln_last,
                   coalesce(sum(-ln(e.adjustment_factor)) OVER (PARTITION BY b.security_id ORDER BY b.trade_date
                       ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING), 0) AS ln_after
            FROM _ca_bars b
            LEFT JOIN (SELECT security_id, ex_date, adjustment_factor FROM {relation}
                       WHERE is_latest_revision AND security_id IN (SELECT DISTINCT security_id FROM _ca_bars)) e
              ON e.security_id = b.security_id AND e.ex_date = b.trade_date
        ), lines AS (
            SELECT security_id, max(abs(ln_factor - ln_last + ln_after)) AS deviation FROM marked GROUP BY 1
        )
        SELECT count(*), max(deviation), count(*) FILTER (WHERE deviation > {TIE_OUT_TOLERANCE}) FROM lines
    """).fetchone()
    assert tie is not None
    return int(tie[0]), tie[1], int(tie[2])


def _replace_rows_by_swap(con: Any, relation: str, sources: list[str], scope: str, scope_params: list[Any]) -> int:
    """Replace the ``sources`` rows of ``corporate_actions`` (in ``scope``) with ``relation``; return its rows.

    ``corporate_actions`` has a ``DEFAULT now()`` column, so this batch path never UPDATEs or DELETEs it in
    place (DuckDB 1.5.5 WAL replay trap): the kept rows and the new ones are copied INSERT-only into a
    table made from the live DDL, which replaces the old one. Run it inside the caller's transaction.
    """
    table, swap = "corporate_actions", "corporate_actions__swap"
    where = "database_name = current_database() AND schema_name = current_schema() AND table_name = ?"
    ddl = con.execute(f"SELECT sql FROM duckdb_tables() WHERE {where}", [table]).fetchone()
    if ddl is None or not str(ddl[0]).startswith(f"CREATE TABLE {table}("):
        raise RuntimeError(f"cannot derive the {table} DDL for a row swap")
    indexes = [row[0] for row in con.execute(f"SELECT sql FROM duckdb_indexes() WHERE {where}", [table]).fetchall()]
    if any(sql is None for sql in indexes):
        raise RuntimeError(f"cannot recreate the {table} indexes without their stored SQL")
    names = [row[0] for row in con.execute(
        "SELECT column_name FROM duckdb_columns() WHERE table_name = ? AND schema_name = current_schema() "
        "AND database_name = current_database() ORDER BY column_index", [table]).fetchall()]
    every = ", ".join(f'"{name}"' for name in names)
    columns = ", ".join([*EVENT_COLUMNS, *(["is_latest_revision"] if "is_latest_revision" in names else [])])
    replaced = f"source IN ({', '.join('?' for _ in sources)}) {scope}"
    kept = int(con.execute(f"SELECT count(*) FROM {table} WHERE NOT ({replaced})", [*sources, *scope_params])
               .fetchone()[0])
    rows = int(con.execute(f"SELECT count(*) FROM {relation}").fetchone()[0])
    con.execute(f"DROP TABLE IF EXISTS {swap}")
    con.execute(f"CREATE TABLE {swap}{str(ddl[0])[len(f'CREATE TABLE {table}'):]}")
    con.execute(f"INSERT INTO {swap} ({every}) SELECT {every} FROM {table} WHERE NOT ({replaced})",
                [*sources, *scope_params])
    con.execute(f"INSERT INTO {swap} ({columns}) SELECT {columns} FROM {relation}")
    copied = int(con.execute(f"SELECT count(*) FROM {swap}").fetchone()[0])
    if copied != kept + rows:
        raise RuntimeError(f"{table} swap row-count proof failed: {kept} kept + {rows} new != {copied}")
    con.execute(f"DROP TABLE {table}")
    con.execute(f"ALTER TABLE {swap} RENAME TO {table}")
    for sql in indexes:
        con.execute(sql)
    return rows


class CorporateActionsDataset(Dataset):
    dataset_id = "corporate_actions"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def _check(self, store: DuckDBStore, name: str, status: str, observed: float, threshold: float,
               details: dict[str, Any]) -> None:
        quality_check(store, dataset_id=self.dataset_id, table_name="corporate_actions", check_name=name,
                      status=status, observed_value=observed, threshold_value=threshold, details=details)

    def load(self, store: DuckDBStore, options: CorporateActionsOptions) -> DatasetLoadResult:
        relation = "_ca_build"
        scope, scope_params = "", []
        if options.security_ids is not None:
            scope, scope_params = "AND security_id IN (SELECT unnest(?::VARCHAR[]))", [sorted(options.security_ids)]
        legacy_sql = f"SELECT count(*) FROM corporate_actions WHERE source = ? {scope}"
        legacy_before = int(store.con.execute(legacy_sql, [LEGACY_SOURCE_NAME, *scope_params]).fetchone()[0])
        if legacy_before and not options.replace_legacy_inferred:
            raise ValueError(f"{legacy_before} rows of {LEGACY_SOURCE_NAME!r} are in scope: keeping them next to "
                             "these events would double count every dividend; load with replace_legacy_inferred")
        store.con.execute(f"DROP TABLE IF EXISTS {relation}")
        try:
            summary = build_corporate_action_events(store.con, relation, source=options.source,
                                                    run_id=options.run_id, security_ids=options.security_ids)
            tie_details = {key: summary[key] for key in ("lines", "tie_out_lines_over", "tie_out_tolerance")}
            if summary["tie_out_lines_over"]:
                # Nothing is written: the previous rows (and the legacy rows) stay in place.
                self._check(store, "factor_tie_out", "failed", float(summary["tie_out_max_abs"]),
                            TIE_OUT_TOLERANCE, tie_details)
                raise RuntimeError(f"corporate action events do not rebuild the vendor factor on "
                                   f"{summary['tie_out_lines_over']} lines (max {summary['tie_out_max_abs']:.3g})")
            sources = [options.source] + ([LEGACY_SOURCE_NAME] if options.replace_legacy_inferred else [])
            with store.transaction():
                rows = _replace_rows_by_swap(store.con, relation, sources, scope, scope_params)
            store.con.execute("CHECKPOINT")
        finally:
            store.con.execute(f"DROP TABLE IF EXISTS {relation}")
        legacy_after = int(store.con.execute(legacy_sql, [LEGACY_SOURCE_NAME, *scope_params]).fetchone()[0])
        self._check(store, "rows_loaded", "passed" if rows > 0 else "warning", float(rows), 1.0,
                    {"source": options.source, "by_type": summary["by_type"], "revisions": summary["revisions"],
                     "pending_latest": summary["pending_latest"]})
        self._check(store, "factor_tie_out", "passed", float(summary["tie_out_max_abs"]), TIE_OUT_TOLERANCE,
                    tie_details)
        series = {key: summary[key] for key in ("bars_outside_primary_series", "lines_with_other_series",
                                                "lines_without_factor_series")}
        self._check(store, "secondary_series_bars", "warning" if any(series.values()) else "passed",
                    float(summary["bars_outside_primary_series"]), 0.0, series)
        self._check(store, "legacy_inferred_replaced", "passed" if legacy_after == 0 else "failed",
                    float(legacy_before), 0.0, {"legacy_source": LEGACY_SOURCE_NAME, "rows_before": legacy_before,
                                                "rows_after": legacy_after})
        return DatasetLoadResult(
            dataset_id=self.dataset_id, rows_loaded=rows, source=options.source,
            details=json.loads(json.dumps({**summary, "legacy_rows_before": legacy_before,
                                           "legacy_rows_after": legacy_after}, default=str)),
        )
