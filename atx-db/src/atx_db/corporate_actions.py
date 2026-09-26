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
    ``distribution_pending_classification`` in band), a candidate R1d decided
    cannot be resolved (``split_unconfirmed``, ``no_share_data`` at the late
    window's close; ``conflicting_series``), a factor step across a data gap, a
    factor decrease, or float noise (``factor_noise``: a factor increase whose
    yield 1 - 1/k is below FACTOR_NOISE_YIELD). Never a split.
``cash_dividend``
    a factor residual below SPECIAL_DISTRIBUTION_MIN_YIELD of the prior close:
    cash amount = (1 - 1/k) x prior close; upgraded to
    ``vendor_factor+xbrl_dps`` when a same-security quarterly USD XBRL
    dividends-per-share fact matches it, from that fact's clock.
``distribution_unclassified``
    a residual of at least SPECIAL_DISTRIBUTION_MIN_YIELD (special dividend,
    spin-off-like), or an exact stock-dividend ratio whose share count never
    followed.

Revisions (point in time). Each step (``details.step_id``) has one row per
state, each available from the clock its state was decidable: a step whose
label is decided later (a late share confirmation, a share window closing, the
late window closing on an unconfirmed split) first has a pending row at the
ex-date bar clock; an XBRL corroboration is a later revision of the cash
dividend at max(ex-date clock, the fact's ``available_at``). A later revision
names ``supersedes_event_id``; a superseded row carries ``superseded_at`` and
``is_latest_revision = false``. :func:`corporate_actions_asof_sql` is the
as-of relation (each step's latest row visible at a cutoff);
:func:`corporate_actions_current_sql` is each step's final row dated at the
first clock of its label (for event consumers that count economics once).

Every row's ``details_json`` carries ``evidence_basis``, ``corroboration``,
``reason``, the step ratio and the prior close. Everything is reconstructed
from a later vendor snapshot, never a verified corporate-action record.

The primary series of a security is chosen among the (source|run_id) series
that carry a vendor factor anywhere (R1d's factor-series set; a factor-free
load never carries events); factored bars of any other series are counted,
never silently dropped. The build is set-based: R1d's epochs are staged once
per build, the XBRL facts once, and the bars are read in hash buckets of about
``_split_epochs.STAGE_CHUNK_ROWS`` rows.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any

from . import _split_epochs
from ._split_epochs import LATE_SHARE_WINDOW_BARS, SPLIT_MAX_RATIO, SPLIT_MIN_RATIO, _stock_dividend_sql
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
#: R1d hazards that leave a split candidate unresolved (signature hazards are not factor steps).
UNRESOLVED_HAZARDS = (*OPEN_HAZARDS, *LATE_HAZARDS, "conflicting_series")
EVENT_COLUMNS = ("source", "security_id", "symbol", "action_type", "ex_date", "declaration_date", "record_date",
                 "payable_date", "cash_amount", "split_from", "split_to", "adjustment_factor", "details_json",
                 "available_at", "run_id")
_HELPERS = ("_ca_raw", "_ca_series", "_ca_bars", "_ca_steps", "_ca_events")
_BUILD_HELPERS = ("_ca_factor_series", "_ca_dps")


def _json_text(field: str, alias: str = "") -> str:
    column = f"{alias}.details_json" if alias else "details_json"
    return f"CASE WHEN json_valid({column}) THEN json_extract_string({column}, '$.{field}') END"


def step_key_sql(alias: str = "") -> str:
    """One corporate-action step: P8's ``step_id`` across its revisions; any other row stands alone."""
    prefix = f"{alias}." if alias else ""
    return (f"coalesce({_json_text('step_id', alias)}, "
            f"{prefix}action_type || '|' || coalesce({prefix}details_json, ''))")


def corporate_actions_asof_sql(cutoff: str, relation: str = "corporate_actions") -> str:
    """Each step's latest row visible at ``cutoff`` (a SQL timestamp expression): the as-of relation.

    Rows with no ``available_at`` (other producers) are always visible.
    """
    return f"""
        SELECT c.* FROM {relation} c
        WHERE c.available_at IS NULL OR c.available_at <= {cutoff}
        QUALIFY row_number() OVER (
            PARTITION BY c.source, c.security_id, c.ex_date, {step_key_sql('c')}
            ORDER BY c.available_at DESC NULLS LAST, try_cast({_json_text('revision', 'c')} AS INTEGER) DESC NULLS LAST
        ) = 1
    """


def corporate_actions_current_sql(relation: str = "corporate_actions") -> str:
    """Each step's final row, dated at the first clock of its label (same ``action_type``).

    For event consumers that use the economic fields once per step (factor
    history, dividend metrics): an XBRL upgrade of a cash dividend keeps the
    dividend's ex-date clock; a late split keeps its confirmation clock. A
    superseded row (``superseded_at`` set) is never returned.
    """
    return f"""
        SELECT * EXCLUDE (label_first_at) REPLACE (label_first_at AS available_at)
        FROM (
            SELECT c.*, min(c.available_at) OVER (
                       PARTITION BY c.source, c.security_id, c.ex_date, {step_key_sql('c')}, c.action_type
                   ) AS label_first_at
            FROM {relation} c
        )
        WHERE {_json_text('superseded_at')} IS NULL
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
    factor = ("CASE WHEN close > 0 AND adjusted_close > 0 AND isfinite(close) AND isfinite(adjusted_close) "
              "THEN adjusted_close / close END")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _ca_raw AS
        SELECT security_id, {series} AS series, trade_date, CAST(close AS DOUBLE) AS close, {symbol} AS symbol,
               {clock} AS available_at, {factor} AS factor
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
                   date_diff('day', lag(trade_date) OVER w, trade_date) AS gap_days,
                   -- the clock at which R1d's late share window (LATE_SHARE_WINDOW_BARS sessions) closes
                   lead(available_at, {LATE_SHARE_WINDOW_BARS}) OVER w AS late_close_at
            FROM _ca_bars WINDOW w AS (PARTITION BY security_id ORDER BY trade_date)
        ) WHERE abs(ln_k) > {FACTOR_NOISE}
    """)
    stock_dividend_grid = _stock_dividend_sql("s.k")
    band = f"BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO}"
    gap = _split_epochs.COVERAGE_MAX_GAP_DAYS
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
                   list_sort(list_distinct(list(evidence) FILTER (
                       WHERE kind = 'hazard' AND evidence IN ({_in_list(UNRESOLVED_HAZARDS)})
                         AND NOT (evidence IN ({_in_list(OPEN_HAZARDS)}) AND until_at IS NOT NULL)))) AS hazard_list
            FROM {epochs} WHERE kind IN ('split', 'distribution', 'hazard')
              AND security_id IN (SELECT DISTINCT security_id FROM _ca_steps)
            GROUP BY ALL
        ), classed AS (
            SELECT s.*, r.split_ratio, r.split_known_at, r.split_evidence, r.distribution_known_at,
                   coalesce(r.distribution, false) AS distribution,
                   nullif(array_to_string(r.hazard_list, ','), '') AS hazards,
                   coalesce(list_has_any(r.hazard_list, [{_in_list(OPEN_HAZARDS)}]), false) AS open_hazard,
                   coalesce(list_has_any(r.hazard_list, [{_in_list(LATE_HAZARDS)}]), false) AS late_hazard,
                   1 - 1 / s.k AS yield,
                   -- what is knowable at the ex-date clock about a step whose label is decided later
                   CASE WHEN s.k {band} THEN 'distribution_pending_classification'
                        ELSE 'split_candidate_pending' END AS pending_reason,
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
               -- When the label is decidable; NULL: still pending at the end of the data.
               CASE WHEN c.action_type IN ('split', 'stock_dividend') THEN greatest(c.ex_at, c.split_known_at)
                    WHEN c.open_hazard THEN NULL
                    WHEN c.late_hazard THEN c.late_close_at
                    WHEN c.distribution AND c.action_type = 'distribution_unclassified'
                        THEN greatest(c.ex_at, coalesce(c.distribution_known_at, c.ex_at))
                    ELSE c.ex_at END AS decided_at
        FROM classed c
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
                                           ORDER BY s.available_at, s.concept) = 1
            )
            SELECT e.*, m.fact AS dps FROM ev e LEFT JOIN matched m USING (security_id, ex_date)
        """
    split_from = "CASE WHEN ratio_q IS NULL THEN 1.0 WHEN k >= 1 THEN ratio_q ELSE round(ratio_q / k) END"
    split_to = "CASE WHEN ratio_q IS NULL THEN k WHEN k >= 1 THEN round(k * ratio_q) ELSE ratio_q END"
    event_id = "md5(step_id || '|' || CAST({} AS VARCHAR))"
    con.execute(f"""
        INSERT INTO {relation} ({', '.join(EVENT_COLUMNS)}, is_latest_revision)
        WITH ev AS (
            SELECT *, CASE WHEN action_type IN ('cash_dividend', 'distribution_unclassified')
                           THEN yield * prior_close END AS cash
            FROM _ca_events
        ), dressed AS ({dressed}),
        steps AS (
            SELECT d.*, md5(concat_ws('|', ?, security_id, CAST(ex_date AS VARCHAR))) AS step_id,
                   d.decided_at IS NULL AS unresolved,
                   coalesce(d.action_type = 'cash_dividend' AND d.dps.available_at > d.ex_at, false) AS xbrl_later
            FROM dressed d
        ), finals AS (
            SELECT s.*,
                   (NOT s.unresolved AND s.decided_at > s.ex_at) OR s.xbrl_later AS revised,
                   CASE WHEN s.unresolved THEN s.ex_at WHEN s.xbrl_later THEN s.dps.available_at
                        ELSE s.decided_at END AS final_at
            FROM steps s
        ), revisions AS (
            -- The latest state of every step (pending when still undecided at the end of the data).
            SELECT f.*, CASE WHEN f.revised THEN 1 ELSE 0 END AS revision, f.unresolved AS pending, f.final_at AS row_at,
                   true AS latest, NULL::TIMESTAMP AS superseded_at,
                   CASE WHEN f.unresolved THEN 'adjustment_unclassified' ELSE f.action_type END AS row_type,
                   CASE WHEN f.unresolved THEN f.pending_reason ELSE f.reason END AS row_reason,
                   f.dps AS row_dps
            FROM finals f
            UNION ALL
            -- The state at the ex-date clock, superseded when the label (or its XBRL evidence) is decided.
            SELECT f.*, 0, NOT f.xbrl_later, f.ex_at, false, f.final_at,
                   CASE WHEN f.xbrl_later THEN f.action_type ELSE 'adjustment_unclassified' END,
                   CASE WHEN f.xbrl_later THEN f.reason ELSE f.pending_reason END,
                   NULL
            FROM finals f WHERE f.revised
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
                   xbrl_dps := row_dps, step_id := step_id, revision := revision,
                   event_id := {event_id.format('revision')},
                   supersedes_event_id := CASE WHEN revision > 0 THEN {event_id.format('revision - 1')} END,
                   superseded_at := superseded_at, classifier := 'r1d_split_epochs', basis := 'vendor_reconstructed')),
               row_at, ?, latest
        FROM revisions
    """, [source, source, run_id])
    # Tie-out: one (latest) row per step rebuilds the vendor's cumulative factor on every bar of every line.
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
    return {"lines": int(tie[0]), "deviation": tie[1], "over": int(tie[2]),
            "bars_outside_primary_series": int(accounting[0]), "lines_with_other_series": int(accounting[1]),
            "lines_without_factor_series": int(accounting[2])}


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
            target = _columns(store.con, "corporate_actions")
            columns = [*EVENT_COLUMNS, *(["is_latest_revision"] if "is_latest_revision" in target else [])]
            with store.transaction():
                store.con.execute(f"DELETE FROM corporate_actions WHERE source IN "
                                  f"({', '.join('?' for _ in sources)}) {scope}", [*sources, *scope_params])
                store.con.execute(f"INSERT INTO corporate_actions ({', '.join(columns)}) "
                                  f"SELECT {', '.join(columns)} FROM {relation}")
            rows = int(store.con.execute(f"SELECT count(*) FROM {relation}").fetchone()[0])
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
