"""Point-in-time split epochs for share-basis rebasing in the derived engine.

A stock split changes every per-share value and share count reported after it.
A filing states all of its values on the share basis in force when it is
issued (ASC 260-10-55-12, SAB Topic 4C restate for splits before issuance), so
a derived state's basis is fixed by its availability clock. Two operands filed
at different clocks are compared on one basis by rebasing each to the frame's
clock with the splits that became known in between; when a split between them
cannot be ruled in or out, the basis is unknown and the comparison is labelled
incomparable. Every proof here rests on reconstructed vendor data (a later
snapshot of the bars), never on a verified corporate-action record.

Series. Bars are read per (security, source, load ``run_id``) series. Factor
steps are computed only between factored bars of one series, so a source or
load boundary is never evidence of a split; it is a coverage break. A
factor-free load (no bar of any security with ``adjusted_close`` different from
``close``, e.g. a loader that defaults one to the other) never proves coverage.

Candidates. A day-over-day step k of the vendor factor ``adjusted_close /
close`` of at least STOCK_DIVIDEND_MIN_STEP, or a bar with an explicit vendor
``split_factor``. A factor step alone is never a split: a large cash
distribution, liquidating dividend or spin-off moves a total-return factor (and
the raw price) like a split. The vendor encodes splits as exact ratios, so the
ratio decides what evidence is needed. *Simple*, with x = max(k, 1/k) and
within SIMPLE_RATIO_TOLERANCE (vendor rounding): out of band, x = p/q with
q <= SIMPLE_RATIO_MAX_DENOMINATOR (p unbounded: 1:50 reverse splits and
beyond); in band, x - 1 is a whole number of STOCK_DIVIDEND_RATIO_STEP (a 5,
10, 12.5, 15, 20 or 25 % stock dividend: 21:20, 11:10, 9:8, 23:20, 6:5, 5:4),
which a cash distribution matches only by chance (20:19 = a 5 % yield does
not). Share evidence is
the archive count reaching the pre-step count x k (read SHARE_LEAD_BARS bars
before the step), and it counts only from its own availability, the A8 vendor
share-run clock (``market_daily``): a split-derived count -- a split-like step
(max(k, 1/k) >= ``market_daily.SPLIT_FACTOR_MIN``) whose count is within
SPLIT_DERIVED_SHARE_TOLERANCE (A8 ``SPLIT_RUN_TOLERANCE``) of the pre-step count
x k -- is known at its first bar; any other match (a looser one, up to
SHARE_CORROBORATION_TOLERANCE, a smaller stock dividend, a signature) is a copy
of a later cover-page count, known only the A8 filer-family lag
(SHARE_MODELED_LAG_DAYS: domestic 90 days; foreign, a 20-F/40-F in
``shares_outstanding_history``, and unknown, no rows, 150 days) after its first
bar. Either way the evidence compares the count with the pre-step count, which
it reveals, so it is never known before that pre-step run is (A8 follow-up:
the line's first run at its first bar, any later run the family lag after its
first bar; a DEI-matched run may be public earlier, so this is conservative).

- explicit ``split_factor``: a split (``vendor_split_field``) known at that bar;
- out-of-band (k outside [SPLIT_MIN_RATIO, SPLIT_MAX_RATIO]) and simple: a split
  (``vendor_factor+shares``) once the count follows within LATE_SHARE_WINDOW_BARS
  sessions (vendor counts lag splits by up to a year); until then the hazard
  ``split_pending_share_confirmation``; no confirmation in that window is the
  permanent hazard ``split_unconfirmed``. A flat count never makes it a
  distribution;
- out-of-band and not simple: a split if the count follows within
  SHARE_WINDOW_BARS sessions (e.g. a split and a dividend on one day), else a
  ``distribution`` when the window closes;
- in-band and simple (21:20, 11:10, 6:5, 5:4 ...): a stock split or dividend
  only if the count makes one discrete jump by k (within
  STOCK_DIVIDEND_SHARE_TOLERANCE x |k - 1|) within LATE_SHARE_WINDOW_BARS
  sessions (counts lag stock dividends like splits), so routine issuance never
  confirms; pending until then, else a ``distribution`` when the window closes;
- in-band and not simple: a ``distribution`` (special dividend, spin-off, fund
  distribution; per-share fundamentals are not restated), known at once;
- a simple candidate without any share data: the permanent hazard ``no_share_data``.

``distribution`` events are audit only and never applied.

Hazards (``_pit_split_hazards``) make the basis unknown for an operand filed on
or before AMBIGUOUS_EPOCH_DAYS after the hazard's ex-date, while the hazard is
open at the frame's clock: ``pending_confirmation`` (a window not yet decided),
``split_pending_share_confirmation``, ``split_unconfirmed``, ``no_share_data``,
``conflicting_series`` (two series report different splits within the ambiguity
window) and the flat-factor signature. The signature: the factor is flat (or
missing) on a day the raw price jumps to within SIGNATURE_PRICE_TOLERANCE of a
split ratio (n:1 for n >= 2, or 5:2, either way). It is ``signature_pending``
until the count moves to near a split ratio (within
SIGNATURE_SHARE_RATIO_TOLERANCE) that cancels the price jump (within
SIGNATURE_SHARE_TOLERANCE, the ex-day return) within LATE_SHARE_WINDOW_BARS
sessions, which makes it the permanent ``flat_factor_split_signature`` (a split
the vendor factor missed); otherwise it closes as ``signature_unconfirmed`` at
the window's end. A hazard is closed early
when a split confirmed in another series explains it. Hazards may use share
observations after their opening bar: they only ever withdraw a proof.

Coverage (``_pit_split_coverage``) is a run of one factor series' factored bars
with no gap above COVERAGE_MAX_GAP_DAYS. A basis is known when one run spans the
operand's clock to the frame's clock (less COVERAGE_GRACE_DAYS), no split known
by the frame went ex within AMBIGUOUS_EPOCH_DAYS of the operand's clock, and no
hazard is open. A split is applied to an operand filed before its ex-date once
it is known by the frame's clock.

Known gaps: stock dividends under STOCK_DIVIDEND_MIN_STEP (e.g. 2-3 %) are not
detected; a stock dividend paid with a cash dividend on one day (an inexact
in-band ratio) reads as a distribution; a vendor factor that misses a 3:2 split,
or a split whose ex-day return moves the price more than
SIGNATURE_PRICE_TOLERANCE off its ratio, or whose count moves outside the
signature's window or tolerances, is not caught (the ruling trades these for
not holding every +/-50 % day open for a year: 22.7k such flat-factor days on
the retained bars).

Owner lookup (R1e). Fundamentals live under accounting ids (``SEC-CIK-*``,
``SEC-COMPANYFACTS-UNRESOLVED-CIK-*``), bars under price-line ids; only some
lines share their owner's id. :func:`bridge_links` maps each accounting id to
its price lines through the owner bridge's single-class link rows
(``market_owner_bridge.MarketOwnerBridge.single_class_link_rows``, read-only):
a line's epochs count for an accounting id only inside the link's interval
(``valid_from <= ex_date < valid_to``) and only from the link's availability (a
split is known at the later of its own clock and the link's; a coverage run
proves a basis only once its link is visible by the frame's clock). ADR and
non-common lines are never linked (an ADS ratio change is not an ordinary-share
split). A split on a line of a multi-class issuer counts only when every class
line linked on its ex-date shows it (within AMBIGUOUS_EPOCH_DAYS and
SHARE_CORROBORATION_TOLERANCE; the owner split is known once every one is),
else the permanent hazard MULTI_CLASS_SPLIT_DISAGREEMENT -- also when only one
class line is visible. Every split and coverage row carries its ``link``
label (the bridge ``link_method``: ``current_sec_ticker``,
``cik_security_id``, ``shared_security_id``, ``reconstructed_history_<tier>``,
``+class_agreement`` when multi-class; LINK_SAME_SECURITY_ID without a
bridge). Links are reconstructed (a current-ticker backcast or RI1 history,
modeled availability), never verified identity. A store without the bridge's
inputs (no symbol/vendor columns on the bars) reads epochs under the id itself.

Staging is set-based: :func:`refresh_scope` reads the bars once for a whole
refresh (in hash buckets of about STAGE_CHUNK_ROWS bars);
:func:`prepare_split_epochs` then copies one security's rows.
"""

from __future__ import annotations

from collections.abc import Iterator, Sequence
from contextlib import contextmanager
from contextvars import ContextVar
from typing import Any
from uuid import uuid4

from .market_daily import (
    ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY,
    FOREIGN_FILER_FORMS,
    SPLIT_FACTOR_MIN,
    SPLIT_RUN_TOLERANCE,
)
from .market_owner_bridge import (
    OWNER_MODE_RECONSTRUCTED,
    SINGLE_CLASS_LINK_COLUMNS,
    build_market_owner_bridge,
    values_relation_sql,
)

__all__ = [
    "AMBIGUOUS_EPOCH_DAYS",
    "COVERAGE_GRACE_DAYS",
    "COVERAGE_MAX_GAP_DAYS",
    "LATE_SHARE_WINDOW_BARS",
    "LINK_SAME_SECURITY_ID",
    "LISTS_ALIAS",
    "LISTS_JOIN",
    "MULTI_CLASS_SPLIT_DISAGREEMENT",
    "SHARE_CORROBORATION_TOLERANCE",
    "SHARE_LEAD_BARS",
    "SHARE_MODELED_LAG_DAYS",
    "SHARE_WINDOW_BARS",
    "SIGNATURE_PRICE_TOLERANCE",
    "SIGNATURE_SHARE_RATIO_TOLERANCE",
    "SIGNATURE_SHARE_TOLERANCE",
    "SIMPLE_RATIO_MAX_DENOMINATOR",
    "SIMPLE_RATIO_TOLERANCE",
    "SPLIT_DERIVED_SHARE_TOLERANCE",
    "SPLIT_MAX_RATIO",
    "SPLIT_MIN_RATIO",
    "STAGE_CHUNK_ROWS",
    "STOCK_DIVIDEND_MIN_STEP",
    "STOCK_DIVIDEND_RATIO_STEP",
    "STOCK_DIVIDEND_SHARE_TOLERANCE",
    "applied_events_sql",
    "basis_known_sql",
    "basis_links_sql",
    "bridge_links",
    "cleanup_split_epochs",
    "factor_series_sql",
    "prepare_split_epochs",
    "rebase_factor_sql",
    "refresh_scope",
]

SPLIT_MIN_RATIO = 0.8
SPLIT_MAX_RATIO = 1.25
STOCK_DIVIDEND_MIN_STEP = 0.05
#: An out-of-band ratio is simple when max(k, 1/k) = p/q, q <= this, within the vendor's rounding.
SIMPLE_RATIO_MAX_DENOMINATOR = 10
#: An in-band ratio is simple when max(k, 1/k) - 1 is a whole number of these (half-percent stock dividends).
STOCK_DIVIDEND_RATIO_STEP = 0.005
SIMPLE_RATIO_TOLERANCE = 1e-5
SHARE_CORROBORATION_TOLERANCE = 0.1
#: A8 ``SPLIT_RUN_TOLERANCE``: a count this close to the pre-step count x k is split-derived.
SPLIT_DERIVED_SHARE_TOLERANCE = SPLIT_RUN_TOLERANCE
#: An in-band count jump must match k within this fraction of |k - 1|.
STOCK_DIVIDEND_SHARE_TOLERANCE = 0.2
#: A8 ``ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY``: an unmatched vendor share run is known this long after it
#: starts, per filer family (foreign: a 20-F/40-F in ``shares_outstanding_history``; unknown: no rows).
SHARE_MODELED_LAG_DAYS = dict(ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY)
SIGNATURE_PRICE_TOLERANCE = 0.05
SIGNATURE_SHARE_RATIO_TOLERANCE = 0.1
SIGNATURE_SHARE_TOLERANCE = 0.2
COVERAGE_MAX_GAP_DAYS = 10
COVERAGE_GRACE_DAYS = 4
AMBIGUOUS_EPOCH_DAYS = 5
SHARE_WINDOW_BARS = 63
LATE_SHARE_WINDOW_BARS = 252
SHARE_LEAD_BARS = 5
#: Bars per staging bucket when a refresh reads the bars table.
STAGE_CHUNK_ROWS = 1_000_000
#: The ``link`` label of epochs read under the security's own id (no owner bridge).
LINK_SAME_SECURITY_ID = "same_security_id"
#: A split on one class line of a multi-class issuer that not every class line linked on its ex-date shows.
MULTI_CLASS_SPLIT_DISAGREEMENT = "multi_class_split_disagreement"
#: Link rows bound per VALUES statement.
LINK_CHUNK_ROWS = 1_000
_EVENTS = "_pit_split_events"
_HAZARDS = "_pit_split_hazards"
_COVERAGE = "_pit_split_coverage"
_LISTS = "_pit_split_lists"
_LINKS = "_split_stage_links"
_HELPERS = ("_split_stage_bars", "_split_stage_steps", "_split_stage_classified", "_split_stage_factor_series",
            "_split_stage_lag")
#: (relation, covered security ids or None for every security, owner links or None) of the active refresh.
_ACTIVE: ContextVar[tuple[str, frozenset[str] | None, tuple[tuple[Any, ...], ...] | None] | None] = ContextVar(
    "_split_epochs_active", default=None)
#: The bridge inputs (``market_owner_bridge``): without them the store reads epochs under the id itself.
_BRIDGE_INPUTS = {
    "equity_daily_bars": {"security_id", "symbol", "source", "vendor_security_id", "trade_date", "close"},
    "fundamental_standardized": {"security_id", "cik", "basis", "available_at", "canonical_code", "period_end",
                                 "unit", "unit_type"},
    "shares_outstanding_history": {"security_id", "cik", "share_count_type", "taxonomy", "available_at",
                                   "share_count", "share_class", "effective_date", "accession_number",
                                   "share_history_id"},
    "derived_metric_values": {"security_id", "source", "metric_code"},
}
_FACTOR = ("CASE WHEN close > 0 AND adjusted_close > 0 AND isfinite(close) AND isfinite(adjusted_close) "
           "THEN adjusted_close / close END")


def _bar_columns(con: Any) -> set[str]:
    return {row[0] for row in con.execute(
        "SELECT column_name FROM duckdb_columns() WHERE table_name = 'equity_daily_bars' "
        "AND schema_name = 'main' AND NOT internal").fetchall()}


def _series_sql(columns: set[str]) -> str:
    parts = [f"coalesce(CAST({name} AS VARCHAR), '')" for name in ("source", "run_id") if name in columns]
    return f"concat_ws('|', {', '.join(parts)})" if parts else "''"


def factor_series_sql(columns: set[str]) -> str:
    """The (source|run_id) series of ``equity_daily_bars`` that carry a vendor factor anywhere, table-wide.

    A factor-free load (``adjusted_close = close`` on every bar) never proves coverage or carries events.
    """
    return f"SELECT DISTINCT {_series_sql(columns)} AS series FROM equity_daily_bars WHERE abs({_FACTOR} - 1) > 1e-9"


def _simple_sql(k: str) -> str:
    """Whether max(k, 1/k) is p/q with q <= SIMPLE_RATIO_MAX_DENOMINATOR, within the vendor's rounding."""
    x = f"greatest({k}, 1 / ({k}))"
    return (f"(len(list_filter(range(1, {SIMPLE_RATIO_MAX_DENOMINATOR + 1}), lambda q_x: "
            f"abs({x} * q_x - round({x} * q_x)) <= {SIMPLE_RATIO_TOLERANCE} * {x} * q_x)) > 0)")


def _stock_dividend_sql(k: str) -> str:
    """Whether max(k, 1/k) - 1 is a whole number of STOCK_DIVIDEND_RATIO_STEP, within the vendor's rounding."""
    steps = f"((greatest({k}, 1 / ({k})) - 1) / {STOCK_DIVIDEND_RATIO_STEP})"
    return (f"(abs({steps} - round({steps})) <= "
            f"{SIMPLE_RATIO_TOLERANCE} * greatest({k}, 1 / ({k})) / {STOCK_DIVIDEND_RATIO_STEP})")


def _split_ratio_near_sql(ratio: str, tolerance: float) -> str:
    """Whether max(r, 1/r) is within ``tolerance`` of n:1 (n >= 2) or 5:2."""
    x = f"greatest({ratio}, 1 / ({ratio}))"
    return (f"(abs({x} / greatest(round({x}), 2) - 1) <= {tolerance} "
            f"OR abs({x} / 2.5 - 1) <= {tolerance})")


def _create_relation(con: Any, relation: str, kind: str) -> None:
    con.execute(f"""CREATE {kind}TABLE {relation} (
        security_id VARCHAR, kind VARCHAR, series VARCHAR, ex_date DATE, from_at TIMESTAMP,
        known_at TIMESTAMP, until_at TIMESTAMP, ratio DOUBLE, evidence VARCHAR,
        link VARCHAR DEFAULT '{LINK_SAME_SECURITY_ID}')""")


def _stage(con: Any, relation: str, kind: str, security_ids: Sequence[str] | None, *, linked: bool = False) -> None:
    """Create ``relation`` holding split, distribution, hazard and coverage rows.

    Every scoped security is read once, in hash buckets of about
    STAGE_CHUNK_ROWS bars so each sort stays bounded. Which loads carry a
    vendor factor is decided over the whole table, so a scoped refresh and a
    full one agree. ``linked``: the scoped ids are price lines of the owner
    links in ``_LINKS``, whose accounting ids carry the A8 filer family.
    """
    _create_relation(con, relation, kind)
    columns = _bar_columns(con)
    if not {"security_id", "trade_date", "close", "adjusted_close"} <= columns:
        return
    series = _series_sql(columns)
    scoped, params = "", []
    if security_ids is not None:
        scoped, params = "AND security_id IN (SELECT unnest(?::VARCHAR[]))", [sorted(set(security_ids))]
    try:
        con.execute(f"CREATE OR REPLACE TEMP TABLE _split_stage_factor_series AS {factor_series_sql(columns)}")
        # The A8 filer-family lag of a vendor share run matched only loosely (a cover-page copy).
        con.execute("CREATE OR REPLACE TEMP TABLE _split_stage_lag (security_id VARCHAR, lag_days INTEGER)")
        history = {row[0] for row in con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'shares_outstanding_history' "
            "AND schema_name = 'main' AND NOT internal").fetchall()}
        if {"security_id", "form"} <= history:
            forms = ", ".join(f"'{form}'" for form in FOREIGN_FILER_FORMS)
            # A linked line takes the family of every accounting id it is linked to (foreign wins).
            key, join = "h.security_id", ""
            if linked:
                key = "coalesce(l.price_security_id, h.security_id)"
                join = (f"LEFT JOIN (SELECT DISTINCT content_security_id, price_security_id FROM {_LINKS}) l "
                        "ON l.content_security_id = h.security_id")
            keyed = f"AND {key} IN (SELECT unnest(?::VARCHAR[]))" if security_ids is not None else ""
            con.execute(f"""
                INSERT INTO _split_stage_lag
                SELECT {key}, CASE WHEN bool_or(h.form IN ({forms})) THEN {SHARE_MODELED_LAG_DAYS['foreign']}
                                   ELSE {SHARE_MODELED_LAG_DAYS['domestic']} END
                FROM shares_outstanding_history h {join} WHERE h.security_id IS NOT NULL {keyed} GROUP BY 1
            """, params)
        count = con.execute(f"SELECT count(*) FROM equity_daily_bars WHERE true {scoped}", params).fetchone()
        buckets = max(1, -(-int(count[0]) // STAGE_CHUNK_ROWS))
        for bucket in range(buckets):
            scope = f"{scoped} AND hash(security_id) % {buckets} = {bucket}" if buckets > 1 else scoped
            _stage_chunk(con, relation, columns, series, scope, params)
    finally:
        for table in _HELPERS:
            con.execute(f"DROP TABLE IF EXISTS {table}")


def _stage_chunk(con: Any, relation: str, columns: set[str], series: str, scope: str, params: list[Any]) -> None:
    shares = "CAST(shares_outstanding AS DOUBLE)" if "shares_outstanding" in columns else "NULL::DOUBLE"
    clock = ("coalesce(available_at, trade_date::TIMESTAMP + INTERVAL 22 HOUR)" if "available_at" in columns
             else "trade_date::TIMESTAMP + INTERVAL 22 HOUR")
    field = (f"CASE WHEN split_factor > 0 AND isfinite(split_factor) AND abs(split_factor - 1) >= "
             f"{STOCK_DIVIDEND_MIN_STEP} THEN CAST(split_factor AS DOUBLE) END"
             if "split_factor" in columns else "NULL::DOUBLE")
    band = f"BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO}"
    step, tol, gap = STOCK_DIVIDEND_MIN_STEP, SHARE_CORROBORATION_TOLERANCE, COVERAGE_MAX_GAP_DAYS
    window, late, lead = SHARE_WINDOW_BARS, LATE_SHARE_WINDOW_BARS, SHARE_LEAD_BARS
    partition = "PARTITION BY security_id, series ORDER BY trade_date"
    before = f"{partition} ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _split_stage_bars AS
        WITH counted AS (
            SELECT *, last_value(shares IGNORE NULLS) OVER ({before}) AS prev_shares
            FROM (
                SELECT security_id, {series} AS series, trade_date, CAST(close AS DOUBLE) AS close,
                       {shares} AS shares, {clock} AS available_at, {field} AS split_field, {_FACTOR} AS factor
                FROM equity_daily_bars
                WHERE security_id IS NOT NULL AND trade_date IS NOT NULL {scope}
                QUALIFY row_number() OVER (PARTITION BY security_id, {series}, trade_date
                                           ORDER BY {clock} DESC NULLS LAST, adjusted_close DESC NULLS LAST,
                                                    close DESC NULLS LAST) = 1
            )
        ), runs AS (
            -- Vendor share runs (A8): a run starts where the count changes; run 1 is the line's first count.
            SELECT *, sum(CASE WHEN shares IS NOT NULL AND shares IS DISTINCT FROM prev_shares THEN 1 ELSE 0 END)
                          OVER ({partition} ROWS UNBOUNDED PRECEDING) AS share_run
            FROM counted
        )
        SELECT *, CASE WHEN shares IS NOT NULL
                       THEN min(available_at) OVER (PARTITION BY security_id, series, share_run) END AS run_at
        FROM runs
    """, params)
    # One pass: every step of interest, classified by the evidence it needs.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _split_stage_steps AS
        WITH windowed AS (
            SELECT *, lag(trade_date) OVER w AS prior_date, lag(close) OVER w AS prior_close,
                   -- A NULL-factor bar never hides a step: compare with the last factored bar.
                   last_value(factor IGNORE NULLS) OVER ({before}) AS prior_factor,
                   last_value(CASE WHEN factor IS NOT NULL THEN trade_date END IGNORE NULLS) OVER ({before})
                       AS prior_factor_date,
                   lag(trade_date, {lead}) OVER w AS lead_date,
                   last_value(shares IGNORE NULLS) OVER wb AS shares_before,
                   -- The pre-step count's run (its first bar's clock; run 1 is the line's first count).
                   last_value(CASE WHEN shares IS NOT NULL THEN share_run END IGNORE NULLS) OVER wb AS before_run,
                   last_value(CASE WHEN shares IS NOT NULL THEN run_at END IGNORE NULLS) OVER wb AS before_run_at,
                   last_value(shares IGNORE NULLS) OVER wl AS shares_later,
                   last_value(trade_date) OVER ws AS short_end_date, last_value(available_at) OVER ws AS short_end_at,
                   count(*) OVER ws AS short_bars,
                   last_value(trade_date) OVER wl AS late_end_date, last_value(available_at) OVER wl AS late_end_at,
                   count(*) OVER wl AS late_bars
            FROM _split_stage_bars
            WINDOW w AS ({partition}),
                   wb AS ({partition} ROWS BETWEEN {window + lead} PRECEDING AND {lead + 1} PRECEDING),
                   ws AS ({partition} ROWS BETWEEN CURRENT ROW AND {window} FOLLOWING),
                   wl AS ({partition} ROWS BETWEEN CURRENT ROW AND {late} FOLLOWING)
        ), measured AS (
            SELECT security_id, series, trade_date AS ex_date, available_at AS ex_at, split_field,
                   coalesce(lead_date, trade_date) AS lead_date, shares_before, shares_later, before_run,
                   before_run_at,
                   short_end_date, short_end_at, short_bars = {window + 1} AS short_complete,
                   late_end_date, late_end_at, late_bars = {late + 1} AS late_complete,
                   CASE WHEN factor IS NOT NULL AND prior_factor IS NOT NULL
                             AND date_diff('day', prior_factor_date, trade_date) <= {gap}
                        THEN factor / prior_factor END AS k,
                   CASE WHEN prior_close > 0 AND close > 0 THEN close / prior_close END AS price_ratio
            FROM windowed
            WHERE prior_date IS NOT NULL AND date_diff('day', prior_date, trade_date) <= {gap}
        ), classed AS (
            SELECT *,
                   CASE WHEN split_field IS NOT NULL THEN 'field'
                        WHEN abs(k - 1) >= {step} THEN
                            CASE WHEN k {band}
                                 THEN (CASE WHEN {_stock_dividend_sql('k')} THEN 'simple_in' ELSE 'inexact_in' END)
                                 ELSE (CASE WHEN {_simple_sql('k')} THEN 'simple_out' ELSE 'inexact_out' END) END
                        WHEN {_split_ratio_near_sql('price_ratio', SIGNATURE_PRICE_TOLERANCE)} THEN 'signature'
                   END AS cls
            FROM measured
        )
        -- Exact splits, exact stock dividends and signatures wait up to a year for a lagging count.
        SELECT *,
               CASE WHEN cls IN ('simple_out', 'simple_in', 'signature') THEN late_end_date ELSE short_end_date END
                   AS search_end_date,
               CASE WHEN cls = 'simple_in' THEN late_complete ELSE short_complete END AS window_complete,
               CASE WHEN cls = 'simple_in' THEN late_end_at ELSE short_end_at END AS window_end_at
        FROM classed WHERE cls IS NOT NULL
    """)
    # Corroboration: the earliest availability of share evidence for each candidate (the A8 run clock).
    family_lag = f"to_days(coalesce(l.lag_days, {SHARE_MODELED_LAG_DAYS['unknown']}))"
    split_like = f"greatest(s.k, 1 / s.k) >= {SPLIT_FACTOR_MIN * (1 - SIMPLE_RATIO_TOLERANCE)!r}"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _split_stage_classified AS
        WITH confirmed AS (
            SELECT s.security_id, s.series, s.ex_date,
                   min(CASE WHEN {split_like}
                                 AND abs(b.shares / (s.shares_before * s.k) - 1) <= {SPLIT_DERIVED_SHARE_TOLERANCE}
                            THEN b.available_at
                            ELSE b.available_at + {family_lag} END) AS count_at
            FROM _split_stage_steps s
            JOIN _split_stage_bars b
              ON b.security_id = s.security_id AND b.series = s.series
             AND b.trade_date BETWEEN s.lead_date AND s.search_end_date
            LEFT JOIN _split_stage_lag l ON l.security_id = s.security_id
            WHERE s.cls <> 'field' AND s.cls <> 'inexact_in' AND s.shares_before > 0 AND b.shares > 0
              AND CASE s.cls
                  WHEN 'simple_in' THEN b.prev_shares > 0
                       AND abs(b.shares / (b.prev_shares * s.k) - 1)
                           <= {STOCK_DIVIDEND_SHARE_TOLERANCE} * abs(s.k - 1)
                       AND abs(b.shares / (s.shares_before * s.k) - 1) <= {tol}
                  WHEN 'signature' THEN
                       {_split_ratio_near_sql('b.shares / s.shares_before', SIGNATURE_SHARE_RATIO_TOLERANCE)}
                       AND abs(b.shares / s.shares_before * s.price_ratio - 1) <= {SIGNATURE_SHARE_TOLERANCE}
                  ELSE abs(b.shares / (s.shares_before * s.k) - 1) <= {tol}
                       AND abs(b.shares / s.shares_before - 1) >= {step} END
            GROUP BY ALL
        ), dated AS (
            -- The evidence reveals the pre-step count: never known before that run is (A8 follow-up).
            SELECT s.*, greatest(c.count_at, CASE WHEN s.before_run = 1 THEN s.before_run_at
                                                  ELSE s.before_run_at + {family_lag} END) AS confirm_at
            FROM _split_stage_steps s
            JOIN confirmed c USING (security_id, series, ex_date)
            LEFT JOIN _split_stage_lag l ON l.security_id = s.security_id
        )
        SELECT s.*, c.confirm_at,
               CASE WHEN s.cls = 'field' THEN 'split'
                    WHEN s.cls = 'inexact_in' THEN 'distribution'
                    WHEN s.cls = 'signature' THEN
                        CASE WHEN c.confirm_at IS NOT NULL THEN 'flat_factor_split_signature'
                             WHEN s.late_complete THEN 'signature_unconfirmed' ELSE 'signature_pending' END
                    WHEN c.confirm_at IS NOT NULL THEN 'split'
                    WHEN s.cls IN ('simple_out', 'simple_in') AND (s.shares_before IS NULL OR s.shares_later IS NULL)
                        THEN 'no_share_data'
                    WHEN s.cls = 'simple_out' THEN
                        CASE WHEN s.late_complete THEN 'split_unconfirmed' ELSE 'split_pending_share_confirmation' END
                    WHEN s.window_complete THEN 'distribution'
                    ELSE 'pending_confirmation' END AS outcome
        FROM _split_stage_steps s
        LEFT JOIN dated c USING (security_id, series, ex_date)
    """)
    con.execute(f"""
        INSERT INTO {relation} (security_id, kind, series, ex_date, from_at, known_at, until_at, ratio, evidence)
        WITH raw_splits AS (
            SELECT security_id, series, ex_date, ex_at,
                   CASE WHEN cls = 'field' THEN ex_at ELSE greatest(ex_at, confirm_at) END AS known_at,
                   coalesce(split_field, k) AS ratio,
                   CASE WHEN cls = 'field' THEN 'vendor_split_field' ELSE 'vendor_factor+shares' END AS evidence
            FROM _split_stage_classified WHERE outcome = 'split'
        ), deduplicated AS (
            -- The same split seen by several series is applied once.
            SELECT * FROM raw_splits s
            WHERE NOT EXISTS (
                SELECT 1 FROM raw_splits o
                WHERE o.security_id = s.security_id
                  AND abs(date_diff('day', o.ex_date, s.ex_date)) <= {AMBIGUOUS_EPOCH_DAYS}
                  AND abs(o.ratio / s.ratio - 1) <= {tol}
                  AND (o.known_at < s.known_at OR (o.known_at = s.known_at AND (o.series < s.series
                       OR (o.series = s.series AND o.ex_date < s.ex_date)))))
        ), conflicted AS (
            SELECT s.*, EXISTS (
                SELECT 1 FROM deduplicated o
                WHERE o.security_id = s.security_id AND (o.series <> s.series OR o.ex_date <> s.ex_date)
                  AND abs(date_diff('day', o.ex_date, s.ex_date)) <= {AMBIGUOUS_EPOCH_DAYS}) AS conflict
            FROM deduplicated s
        ), splits AS (
            SELECT * FROM conflicted WHERE NOT conflict
        ), open_hazards AS (
            SELECT security_id, series, ex_date, ex_at AS from_at,
                   CASE WHEN outcome = 'split' THEN greatest(ex_at, confirm_at)
                        WHEN outcome = 'distribution' THEN window_end_at
                        WHEN outcome = 'signature_unconfirmed' THEN late_end_at END AS until_at,
                   CASE WHEN cls = 'signature' THEN 1 / price_ratio ELSE coalesce(split_field, k) END AS ratio,
                   CASE WHEN outcome = 'split' AND cls = 'simple_out' THEN 'split_pending_share_confirmation'
                        WHEN outcome IN ('split', 'distribution') THEN 'pending_confirmation'
                        ELSE outcome END AS evidence
            FROM _split_stage_classified
            WHERE (outcome = 'split' AND cls <> 'field' AND confirm_at > ex_at)
               OR (outcome = 'distribution' AND cls <> 'inexact_in')
               OR outcome NOT IN ('split', 'distribution')
            UNION ALL
            SELECT security_id, series, ex_date, ex_at, NULL, ratio, 'conflicting_series'
            FROM conflicted WHERE conflict
        ), hazards AS (
            -- A split confirmed in any series closes a hazard it explains.
            SELECT h.*, (SELECT min(s.known_at) FROM splits s
                         WHERE s.security_id = h.security_id
                           AND abs(date_diff('day', s.ex_date, h.ex_date)) <= {AMBIGUOUS_EPOCH_DAYS}
                           AND (h.ratio IS NULL OR abs(s.ratio / h.ratio - 1) <= {tol})) AS explained_at
            FROM open_hazards h
        ), runs AS (
            SELECT security_id, series, available_at, sum(starts_run) OVER ({partition}) AS run
            FROM (
                SELECT security_id, series, trade_date, available_at,
                       CASE WHEN date_diff('day', lag(trade_date) OVER ({partition}), trade_date) <= {gap}
                            THEN 0 ELSE 1 END AS starts_run
                FROM _split_stage_bars
                -- A factor-free load (adjusted_close = close throughout) never proves coverage.
                WHERE factor IS NOT NULL AND series IN (SELECT series FROM _split_stage_factor_series)
            )
        ), coverage AS (
            SELECT security_id, series, min(available_at) AS first_at, max(available_at) AS last_at
            FROM runs GROUP BY security_id, series, run
        )
        SELECT security_id, 'split' AS kind, series, ex_date, ex_at AS from_at, known_at,
               NULL::TIMESTAMP AS until_at, ratio, evidence FROM splits
        UNION ALL
        SELECT security_id, 'distribution', series, ex_date, ex_at,
               CASE WHEN cls = 'inexact_in' THEN ex_at ELSE window_end_at END, NULL, k, 'distribution'
        FROM _split_stage_classified WHERE outcome = 'distribution'
        UNION ALL
        SELECT security_id, 'hazard', series, ex_date, from_at, NULL,
               CASE WHEN explained_at IS NULL THEN until_at WHEN until_at IS NULL THEN explained_at
                    ELSE least(until_at, explained_at) END, ratio, evidence
        FROM hazards
        WHERE explained_at IS NULL OR explained_at > from_at
        UNION ALL
        SELECT security_id, 'coverage', series, NULL, first_at, NULL, last_at, NULL, NULL FROM coverage
    """)


def bridge_links(store: Any, *, item_codes: Sequence[str], derived_source: str) -> tuple[tuple[Any, ...], ...] | None:
    """The accounting id -> price line split lookup (R1e): the owner bridge's single-class link rows.

    Rows in ``market_owner_bridge.SINGLE_CLASS_LINK_COLUMNS`` layout, from the
    reconstructed-mode bridge (current-ticker backcast plus RI1 history at the
    tier in force on each segment's bars; ``item_codes`` are the accounting
    content it reads). ``None`` when the store lacks the bridge's inputs (see
    :data:`_BRIDGE_INPUTS`): epochs are then read under the id itself.
    """
    wanted = {table: set(columns) for table, columns in _BRIDGE_INPUTS.items()}
    for table, column in store.con.execute(
            "SELECT table_name, column_name FROM duckdb_columns() WHERE schema_name = 'main' AND NOT internal "
            "AND table_name IN (SELECT unnest(?::VARCHAR[]))", [sorted(_BRIDGE_INPUTS)]).fetchall():
        wanted[str(table)].discard(str(column))
    if any(wanted.values()):
        return None
    bridge = build_market_owner_bridge(store, mode=OWNER_MODE_RECONSTRUCTED, item_codes=tuple(sorted(item_codes)),
                                       derived_source=derived_source)
    return tuple(tuple(row) for row in bridge.single_class_link_rows())


def _stage_links(con: Any, relation: str, kind: str, security_ids: Sequence[str] | None,
                 links: Sequence[Sequence[Any]]) -> None:
    """Create ``relation`` keyed by accounting id from the epochs of each id's linked price lines."""
    _create_relation(con, relation, kind)
    scope = None if security_ids is None else set(security_ids)
    rows = [tuple(row) for row in links if scope is None or row[0] in scope]
    lines = sorted({str(row[1]) for row in rows})
    if not lines:
        return
    line_relation = f'"_split_stage_lines_{uuid4().hex}"'
    try:
        con.execute(f"CREATE OR REPLACE TEMP TABLE {_LINKS} AS {values_relation_sql(SINGLE_CLASS_LINK_COLUMNS, 0)}")
        for start in range(0, len(rows), LINK_CHUNK_ROWS):
            chunk = rows[start:start + LINK_CHUNK_ROWS]
            con.execute(f"INSERT INTO {_LINKS} {values_relation_sql(SINGLE_CLASS_LINK_COLUMNS, len(chunk))}",
                        [value for row in chunk for value in row])
        _stage(con, line_relation, "TEMP ", lines, linked=True)
        _map_links(con, relation, line_relation)
    finally:
        con.execute(f"DROP TABLE IF EXISTS {line_relation}")
        con.execute(f"DROP TABLE IF EXISTS {_LINKS}")


def _map_links(con: Any, relation: str, lines: str) -> None:
    """Map price-line epochs (``lines``) onto the accounting ids linked to them in ``_LINKS``."""
    tol, near = SHARE_CORROBORATION_TOLERANCE, AMBIGUOUS_EPOCH_DAYS
    inside = "{e}.ex_date >= {k}.valid_from AND ({k}.valid_to IS NULL OR {e}.ex_date < {k}.valid_to)"
    con.execute(f"""
        INSERT INTO {relation}
        WITH placed AS (
            -- A line's split, distribution or hazard counts for an accounting id inside the link's interval.
            SELECT k.content_security_id AS content, k.price_security_id AS line, k.issuer_key,
                   k.available_at AS link_at, k.link_method, e.kind, e.series, e.ex_date, e.from_at, e.known_at,
                   e.until_at, e.ratio, e.evidence
            FROM {lines} e JOIN {_LINKS} k
              ON k.price_security_id = e.security_id AND {inside.format(e='e', k='k')}
            WHERE e.kind IN ('split', 'distribution', 'hazard')
        ), agreement AS (
            -- Every class line of the issuer linked on the ex-date must show the split (multi-class issuers).
            SELECT p.content, p.line, p.series, p.ex_date,
                   bool_or(o.issuer_multi_class) AS multi_class,
                   count(DISTINCT o.price_security_id) AS class_lines,
                   count(DISTINCT CASE WHEN o.price_security_id = p.line OR m.security_id IS NOT NULL
                                       THEN o.price_security_id END) AS agreeing,
                   max(greatest(coalesce(m.known_at, o.available_at), o.available_at)) AS agreed_at
            FROM placed p
            JOIN {_LINKS} o ON o.content_security_id = p.content AND o.issuer_key = p.issuer_key
             AND {inside.format(e='p', k='o')}
            LEFT JOIN {lines} m ON m.kind = 'split' AND m.security_id = o.price_security_id
             AND o.price_security_id <> p.line AND abs(date_diff('day', m.ex_date, p.ex_date)) <= {near}
             AND abs(m.ratio / p.ratio - 1) <= {tol}
            WHERE p.kind = 'split'
            GROUP BY p.content, p.line, p.series, p.ex_date
        ), candidates AS (
            SELECT p.content AS security_id, p.line || '|' || p.series AS series, p.ex_date, p.from_at,
                   greatest(p.known_at, p.link_at, a.agreed_at) AS known_at, p.ratio, p.evidence,
                   p.link_method || CASE WHEN a.multi_class THEN '+class_agreement' ELSE '' END AS link,
                   NOT a.multi_class OR (a.class_lines >= 2 AND a.agreeing = a.class_lines) AS agreed
            FROM placed p JOIN agreement a USING (content, line, series, ex_date)
            WHERE p.kind = 'split'
        ), accepted AS (
            SELECT * FROM candidates WHERE agreed
        ), deduplicated AS (
            -- The same split seen on several lines of the accounting id is applied once.
            SELECT * FROM accepted s
            WHERE NOT EXISTS (
                SELECT 1 FROM accepted o
                WHERE o.security_id = s.security_id AND abs(date_diff('day', o.ex_date, s.ex_date)) <= {near}
                  AND abs(o.ratio / s.ratio - 1) <= {tol}
                  AND (o.known_at < s.known_at OR (o.known_at = s.known_at AND (o.series < s.series
                       OR (o.series = s.series AND o.ex_date < s.ex_date)))))
        ), conflicted AS (
            SELECT s.*, EXISTS (
                SELECT 1 FROM deduplicated o
                WHERE o.security_id = s.security_id AND (o.series <> s.series OR o.ex_date <> s.ex_date)
                  AND abs(date_diff('day', o.ex_date, s.ex_date)) <= {near}) AS conflict
            FROM deduplicated s
        ), segments AS (
            -- Contiguous link segments of one (accounting id, line) form one coverage interval.
            SELECT *, sum(starts) OVER (PARTITION BY content_security_id, price_security_id ORDER BY valid_from)
                          AS segment
            FROM (SELECT *, CASE WHEN lag(valid_to) OVER (PARTITION BY content_security_id, price_security_id
                                                          ORDER BY valid_from) = valid_from THEN 0 ELSE 1 END
                                AS starts
                  FROM {_LINKS})
        ), intervals AS (
            SELECT content_security_id, price_security_id, min(valid_from) AS valid_from,
                   CASE WHEN bool_or(valid_to IS NULL) THEN NULL ELSE max(valid_to) END AS valid_to,
                   max(available_at) AS link_at,
                   array_to_string(list_sort(list_distinct(list(link_method))), ',') AS link
            FROM segments GROUP BY content_security_id, price_security_id, segment
        ), coverage AS (
            -- A line's coverage run proves a basis for the accounting id only inside the link, once it is visible.
            SELECT i.content_security_id AS security_id, c.security_id || '|' || c.series AS series,
                   greatest(c.from_at, i.valid_from::TIMESTAMP) AS from_at, i.link_at AS known_at,
                   CASE WHEN i.valid_to IS NULL THEN c.until_at ELSE least(c.until_at, i.valid_to::TIMESTAMP) END
                       AS until_at, i.link
            FROM {lines} c JOIN intervals i ON i.price_security_id = c.security_id
            WHERE c.kind = 'coverage'
        )
        SELECT security_id, 'split', series, ex_date, from_at, known_at, NULL, ratio, evidence, link
        FROM conflicted WHERE NOT conflict
        UNION ALL
        SELECT security_id, 'hazard', series, ex_date, from_at, NULL, NULL, ratio, 'conflicting_series', link
        FROM conflicted WHERE conflict
        UNION ALL
        SELECT security_id, 'hazard', series, ex_date, from_at, NULL, NULL, ratio,
               '{MULTI_CLASS_SPLIT_DISAGREEMENT}', link
        FROM candidates WHERE NOT agreed
        UNION ALL
        SELECT content, kind, line || '|' || series, ex_date, from_at, known_at, until_at, ratio, evidence,
               link_method
        FROM placed WHERE kind IN ('distribution', 'hazard')
        UNION ALL
        SELECT security_id, 'coverage', series, NULL, from_at, known_at, until_at, NULL, NULL, link
        FROM coverage WHERE from_at <= until_at
    """)


def _stage_scope(con: Any, relation: str, kind: str, security_ids: Sequence[str] | None,
                 links: Sequence[Sequence[Any]] | None) -> None:
    if links is None:
        _stage(con, relation, kind, security_ids)
    else:
        _stage_links(con, relation, kind, security_ids, links)


@contextmanager
def refresh_scope(store: Any, security_ids: Sequence[str] | None, *, persistent: bool,
                  links: Sequence[Sequence[Any]] | None = None) -> Iterator[str]:
    """Stage split epochs for a whole refresh in one pass over the bars.

    ``persistent`` stores the relation in the main catalog so it survives the
    driver's connection recycling (like its security-id page); otherwise it is
    a temporary table. The relation is dropped on exit. ``links`` (see
    :func:`bridge_links`) keys the epochs by accounting id through the owner
    bridge; ``None`` keys them by the bars' own security id.
    """
    name = f"_derived_split_epochs_{uuid4().hex}"
    relation, kind = f'"{name}"', "TEMP "
    if persistent:
        database = store.con.execute("SELECT current_database()").fetchone()
        assert database is not None
        catalog = str(database[0]).replace('"', '""')
        relation, kind = f'"{catalog}"."main"."{name}"', ""
    token = None
    frozen = None if links is None else tuple(tuple(row) for row in links)
    try:
        _stage_scope(store.con, relation, kind, security_ids, frozen)
        token = _ACTIVE.set((relation, None if security_ids is None else frozenset(security_ids), frozen))
        yield relation
    finally:
        if token is not None:
            _ACTIVE.reset(token)
        store.con.execute(f"DROP TABLE IF EXISTS {relation}")


def prepare_split_epochs(con: Any, security_id: str) -> int:
    """Copy one security's split epochs into the per-security relations; return the split count.

    Inside :func:`refresh_scope` the rows come from the refresh's staged
    relation; otherwise this security's bars are staged on the spot (three
    scans of the bars table: use a refresh scope for more than a few ids),
    through the refresh's owner links when it has them.
    """
    active = _ACTIVE.get()
    staged = None
    if active is not None and (active[1] is None or security_id in active[1]):
        relation = active[0]
    else:
        relation = staged = f'"_split_stage_single_{uuid4().hex}"'
    try:
        if staged is not None:
            _stage_scope(con, relation, "TEMP ", [security_id], active[2] if active is not None else None)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {_EVENTS} AS
            SELECT ex_date, known_at AS available_at, ratio, evidence, kind, series, from_at AS ex_at, link
            FROM {relation} WHERE security_id = ? AND kind IN ('split', 'distribution')
        """, [security_id])
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {_HAZARDS} AS
            SELECT ex_date, from_at, until_at, ratio, evidence AS reason, series, link
            FROM {relation} WHERE security_id = ? AND kind = 'hazard'
        """, [security_id])
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {_COVERAGE} AS
            SELECT from_at AS first_at, until_at AS last_at, series, known_at, link
            FROM {relation} WHERE security_id = ? AND kind = 'coverage'
        """, [security_id])
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {_LISTS} AS
            SELECT (SELECT list(struct_pack(ex_date := ex_date, known_at := available_at, ratio := ratio,
                                            evidence := evidence, link := link) ORDER BY ex_date)
                    FROM {_EVENTS} WHERE kind = 'split') AS splits,
                   (SELECT list(struct_pack(ex_date := ex_date, from_at := from_at, until_at := until_at)
                                ORDER BY ex_date) FROM {_HAZARDS}) AS hazards,
                   (SELECT list(struct_pack(first_at := first_at, last_at := last_at, known_at := known_at,
                                            link := link) ORDER BY first_at)
                    FROM {_COVERAGE}) AS coverage
        """)
    finally:
        if staged is not None:
            con.execute(f"DROP TABLE IF EXISTS {staged}")
    return int(con.execute(f"SELECT count(*) FROM {_EVENTS} WHERE kind = 'split'").fetchone()[0])


def cleanup_split_epochs(con: Any) -> None:
    for table in (_EVENTS, _HAZARDS, _COVERAGE, _LISTS, *_HELPERS):
        con.execute(f"DROP TABLE IF EXISTS {table}")


#: The frame SQL cross-joins this one-row relation of one security's splits,
#: hazards and coverage as lists, filtered by lambdas that capture the operand
#: and frame clocks: no correlated subquery re-materializes the frame.
LISTS_ALIAS = "split_lists"
LISTS_JOIN = f"CROSS JOIN {_LISTS} {LISTS_ALIAS}"


def _list(name: str) -> str:
    return f"{LISTS_ALIAS}.{name}"


def _applied(clock: str, frame_clock: str) -> str:
    return (f"list_filter({_list('splits')}, lambda split_x: split_x.known_at <= ({frame_clock}) "
            f"AND split_x.ex_date > CAST(({clock}) AS DATE))")


def rebase_factor_sql(clock: str, frame_clock: str, exponent: int) -> str:
    """Multiplier that restates a value filed at ``clock`` on the basis at ``frame_clock``.

    A split of ratio k (new shares per old share) divides per-share values
    (exponent -1) and multiplies share counts (exponent +1). A split applies to
    an operand filed before its ex-date once it is known by the frame's clock.
    """
    return (f"exp({exponent} * coalesce(list_sum(list_transform({_applied(clock, frame_clock)}, "
            f"lambda split_x: ln(split_x.ratio))), 0))")


def applied_events_sql(clock: str, frame_clock: str) -> str:
    """The splits :func:`rebase_factor_sql` applies, with their evidence and owner link, for lineage."""
    return (f"list_transform({_applied(clock, frame_clock)}, lambda split_x: struct_pack("
            f"ex_date := split_x.ex_date, ratio := split_x.ratio, evidence := split_x.evidence, "
            f"link := split_x.link))")


def _covered(clock: str, frame_clock: str) -> str:
    # A run proves the basis once its owner link is visible by the frame's clock.
    return (f"list_filter({_list('coverage')}, lambda run_x: run_x.first_at <= ({clock}) "
            f"AND run_x.last_at >= ({frame_clock}) - INTERVAL {COVERAGE_GRACE_DAYS} DAY "
            f"AND (run_x.known_at IS NULL OR run_x.known_at <= ({frame_clock})))")


def basis_links_sql(clock: str, frame_clock: str) -> str:
    """The owner-link labels of the coverage runs that span ``clock`` to ``frame_clock``, for lineage."""
    return f"list_sort(list_distinct(list_transform({_covered(clock, frame_clock)}, lambda run_x: run_x.link)))"


def basis_known_sql(clock: str, frame_clock: str) -> str:
    """Whether every split between ``clock`` and ``frame_clock`` is known and applied exactly."""
    covered = _covered(clock, frame_clock)
    near = (f"list_filter({_list('splits')}, lambda split_x: split_x.known_at <= ({frame_clock}) "
            f"AND abs(date_diff('day', split_x.ex_date, CAST(({clock}) AS DATE))) <= {AMBIGUOUS_EPOCH_DAYS})")
    open_hazards = (f"list_filter({_list('hazards')}, lambda hazard_x: hazard_x.from_at <= ({frame_clock}) "
                    f"AND (hazard_x.until_at IS NULL OR hazard_x.until_at > ({frame_clock})) "
                    f"AND date_diff('day', CAST(({clock}) AS DATE), hazard_x.ex_date) >= -{AMBIGUOUS_EPOCH_DAYS})")
    return (f"(coalesce(len({covered}), 0) > 0 AND coalesce(len({near}), 0) = 0 "
            f"AND coalesce(len({open_hazards}), 0) = 0)")
