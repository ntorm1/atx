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
steps are computed only between consecutive bars of one series, so a source or
load boundary is never evidence of a split; it is a coverage break. When a
security has a series whose vendor factor varies, its series with a constant
factor (for example a loader that defaults ``adjusted_close`` to ``close``)
never prove coverage.

Events (``_pit_split_events``; ``kind`` 'split' is applied, 'distribution' is
audit only). A candidate is a day-over-day step of the vendor factor
``adjusted_close / close`` of at least STOCK_DIVIDEND_MIN_STEP, or a bar with an
explicit vendor ``split_factor``. A factor step alone is never a split: a large
cash distribution, liquidating dividend or spin-off moves a total-return factor
exactly like a split (and the raw price with it).

- ``vendor_split_field``: the explicit field is the split ratio, known at that bar;
- ``vendor_factor+shares``: the archive share count moves to the pre-step count
  times the factor ratio (within SHARE_CORROBORATION_TOLERANCE) between
  SHARE_LEAD_BARS bars before the step and SHARE_WINDOW_BARS bars after it.
  Share evidence is used only at its own availability (the A8 vendor share-run
  clock, ``market_daily``): a count within SPLIT_DERIVED_SHARE_TOLERANCE of the
  pre-step count times the ratio is derived from the public split terms and is
  known at its first bar; a looser match is a vendor copy of a later cover-page
  count and is known only SHARE_MODELED_LAG_DAYS after its first bar. The
  pre-step count is read SHARE_LEAD_BARS bars before the step, so a count the
  vendor moved a few sessions early still corroborates;
- ``distribution``: the share window closes with a flat count (within
  FLAT_SHARE_TOLERANCE). The window is one quarter of sessions because vendor
  share runs start at cover-page dates, so a count that lags a split shows it
  by the next cover; a shorter window would read a lagged split as a
  distribution.

Hazards (``_pit_split_hazards``) make the basis unknown for an operand filed on
or before AMBIGUOUS_EPOCH_DAYS after the hazard's ex-date, while the hazard is
open at the frame's clock:

- ``pending_confirmation``: a candidate whose share window has not decided it;
- ``no_share_data`` / ``shares_inconsistent``: a candidate that can never be
  corroborated or refuted; open forever;
- ``flat_factor_split_signature``: the factor is flat (or missing) while the
  raw price and the share count both move by split-sized ratios that cancel
  (market value continuous); a vendor factor that misses a split is never read
  as proof of none; open forever;
- ``conflicting_series``: two series report different splits within the
  ambiguity window.

A hazard is closed early when a split confirmed in another series explains it.
Hazards may use share observations after their opening bar: they only ever
withdraw a proof, never supply one.

Coverage (``_pit_split_coverage``) is a run of one factor series' bars with a
factor and no gap above COVERAGE_MAX_GAP_DAYS. A basis is known when one run
spans the operand's clock to the frame's clock (less COVERAGE_GRACE_DAYS), no
split known by the frame went ex within AMBIGUOUS_EPOCH_DAYS of the operand's
clock, and no hazard is open. A split is applied to an operand filed before its
ex-date once it is known by the frame's clock.

Known gaps: stock dividends under STOCK_DIVIDEND_MIN_STEP (e.g. 2-3 %) are not
detected, so per-share values across them are off by that step while labelled
comparable; a flat factor with a split-sized price move but no share data is
not a hazard; a vendor count that lags a split by more than SHARE_WINDOW_BARS
sessions reads as a distribution; a security whose only bars come from a
factor-free load (``adjusted_close`` defaulted to ``close``) proves coverage
it cannot have.

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

__all__ = [
    "AMBIGUOUS_EPOCH_DAYS",
    "COVERAGE_GRACE_DAYS",
    "COVERAGE_MAX_GAP_DAYS",
    "FLAT_SHARE_TOLERANCE",
    "LISTS_ALIAS",
    "LISTS_JOIN",
    "SHARE_CORROBORATION_TOLERANCE",
    "SHARE_LEAD_BARS",
    "SHARE_MODELED_LAG_DAYS",
    "SHARE_WINDOW_BARS",
    "SPLIT_DERIVED_SHARE_TOLERANCE",
    "SPLIT_MAX_RATIO",
    "SPLIT_MIN_RATIO",
    "STAGE_CHUNK_ROWS",
    "STOCK_DIVIDEND_MIN_STEP",
    "applied_events_sql",
    "basis_known_sql",
    "cleanup_split_epochs",
    "prepare_split_epochs",
    "rebase_factor_sql",
    "refresh_scope",
]

SPLIT_MIN_RATIO = 0.8
SPLIT_MAX_RATIO = 1.25
STOCK_DIVIDEND_MIN_STEP = 0.05
SHARE_CORROBORATION_TOLERANCE = 0.1
#: A8 ``SPLIT_RUN_TOLERANCE``: a count this close to the pre-step count x k is split-derived.
SPLIT_DERIVED_SHARE_TOLERANCE = 0.05
#: A8 ``ARCHIVE_MODELED_LAG_DAYS``: an unmatched vendor share run is known this long after it starts.
SHARE_MODELED_LAG_DAYS = 90
FLAT_SHARE_TOLERANCE = 0.05
COVERAGE_MAX_GAP_DAYS = 10
COVERAGE_GRACE_DAYS = 4
AMBIGUOUS_EPOCH_DAYS = 5
SHARE_WINDOW_BARS = 63
SHARE_LEAD_BARS = 5
#: Bars per staging bucket when a refresh reads the bars table.
STAGE_CHUNK_ROWS = 1_000_000
_EVENTS = "_pit_split_events"
_HAZARDS = "_pit_split_hazards"
_COVERAGE = "_pit_split_coverage"
_LISTS = "_pit_split_lists"
_HELPERS = ("_split_stage_bars", "_split_stage_steps", "_split_stage_classified")
#: (relation, covered security ids or None for every security) of the active refresh.
_ACTIVE: ContextVar[tuple[str, frozenset[str] | None] | None] = ContextVar("_split_epochs_active", default=None)
_FACTOR = ("CASE WHEN close > 0 AND adjusted_close > 0 AND isfinite(close) AND isfinite(adjusted_close) "
           "THEN adjusted_close / close END")


def _bar_columns(con: Any) -> set[str]:
    return {row[0] for row in con.execute(
        "SELECT column_name FROM duckdb_columns() WHERE table_name = 'equity_daily_bars' "
        "AND schema_name = 'main' AND NOT internal").fetchall()}


def _series_sql(columns: set[str]) -> str:
    parts = [f"coalesce(CAST({name} AS VARCHAR), '')" for name in ("source", "run_id") if name in columns]
    return f"concat_ws('|', {', '.join(parts)})" if parts else "''"


def _stage(con: Any, relation: str, kind: str, security_ids: Sequence[str] | None) -> None:
    """Create ``relation`` holding split, distribution, hazard and coverage rows.

    Every scoped security is read once, in hash buckets of about
    STAGE_CHUNK_ROWS bars so each sort stays bounded. A security's rows depend
    on its own bars only, so a scoped refresh and a full one agree.
    """
    con.execute(f"""CREATE {kind}TABLE {relation} (
        security_id VARCHAR, kind VARCHAR, series VARCHAR, ex_date DATE, from_at TIMESTAMP,
        known_at TIMESTAMP, until_at TIMESTAMP, ratio DOUBLE, evidence VARCHAR)""")
    columns = _bar_columns(con)
    if not {"security_id", "trade_date", "close", "adjusted_close"} <= columns:
        return
    series = _series_sql(columns)
    scoped, params = "", []
    if security_ids is not None:
        scoped, params = "AND security_id IN (SELECT unnest(?::VARCHAR[]))", [sorted(set(security_ids))]
    try:
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
    step = STOCK_DIVIDEND_MIN_STEP
    tol = SHARE_CORROBORATION_TOLERANCE
    window, lead = SHARE_WINDOW_BARS, SHARE_LEAD_BARS
    partition = "PARTITION BY security_id, series ORDER BY trade_date"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _split_stage_bars AS
        SELECT security_id, {series} AS series, trade_date, CAST(close AS DOUBLE) AS close,
               {shares} AS shares, {clock} AS available_at, {field} AS split_field, {_FACTOR} AS factor
        FROM equity_daily_bars
        WHERE security_id IS NOT NULL AND trade_date IS NOT NULL {scope}
        QUALIFY row_number() OVER (PARTITION BY security_id, {series}, trade_date
                                   ORDER BY {clock} DESC NULLS LAST, adjusted_close DESC NULLS LAST,
                                            close DESC NULLS LAST) = 1
    """, params)
    # One pass: every step of interest, with its share evidence, per series.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _split_stage_steps AS
        WITH windowed AS (
            SELECT *, lag(trade_date) OVER w AS prior_date, lag(factor) OVER w AS prior_factor,
                   lag(close) OVER w AS prior_close, lag(trade_date, {lead}) OVER w AS lead_date,
                   last_value(shares IGNORE NULLS) OVER ({partition}
                       ROWS BETWEEN {window + lead} PRECEDING AND {lead + 1} PRECEDING) AS shares_before,
                   last_value(shares IGNORE NULLS) OVER fw AS shares_after,
                   last_value(trade_date) OVER fw AS window_end_date,
                   last_value(available_at) OVER fw AS window_end_at,
                   count(*) OVER fw AS window_bars
            FROM _split_stage_bars
            WINDOW w AS ({partition}), fw AS ({partition} ROWS BETWEEN CURRENT ROW AND {window} FOLLOWING)
        ), measured AS (
            SELECT security_id, series, trade_date AS ex_date, available_at AS ex_at, split_field,
                   coalesce(lead_date, trade_date) AS lead_date, shares_before, window_end_date, window_end_at,
                   window_bars,
                   CASE WHEN prior_factor IS NOT NULL AND factor IS NOT NULL THEN factor / prior_factor END AS k,
                   CASE WHEN prior_close > 0 AND close > 0 THEN close / prior_close END AS price_ratio,
                   CASE WHEN shares_before > 0 AND shares_after > 0 THEN shares_after / shares_before END
                       AS share_ratio
            FROM windowed
            WHERE prior_date IS NOT NULL AND date_diff('day', prior_date, trade_date) <= {COVERAGE_MAX_GAP_DAYS}
        )
        SELECT *, coalesce(split_field IS NOT NULL OR abs(k - 1) >= {step}, false) AS candidate
        FROM measured
        WHERE split_field IS NOT NULL OR abs(k - 1) >= {step}
           -- A split the factor missed: price and count move by split-sized ratios that cancel.
           OR ((k IS NULL OR abs(k - 1) < {step}) AND NOT (price_ratio {band}) AND NOT (share_ratio {band})
               AND abs(share_ratio * price_ratio - 1) <= {tol})
    """)
    # Corroboration: the earliest availability of a count that moved by k.
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _split_stage_classified AS
        WITH confirmed AS (
            SELECT s.security_id, s.series, s.ex_date,
                   min(CASE WHEN abs(b.shares / (s.shares_before * s.k) - 1) <= {SPLIT_DERIVED_SHARE_TOLERANCE}
                            THEN b.available_at
                            ELSE b.available_at + INTERVAL {SHARE_MODELED_LAG_DAYS} DAY END) AS confirm_at
            FROM _split_stage_steps s
            JOIN _split_stage_bars b
              ON b.security_id = s.security_id AND b.series = s.series
             AND b.trade_date BETWEEN s.lead_date AND s.window_end_date
            WHERE s.candidate AND s.split_field IS NULL AND s.shares_before > 0 AND b.shares > 0
              AND abs(b.shares / (s.shares_before * s.k) - 1) <= {tol}
              AND abs(b.shares / s.shares_before - 1) >= {step}
            GROUP BY ALL
        )
        SELECT s.*, c.confirm_at,
               CASE WHEN s.split_field IS NOT NULL OR c.confirm_at IS NOT NULL THEN 'split'
                    WHEN NOT s.candidate THEN 'hazard'
                    WHEN s.window_bars = {window + 1} AND abs(s.share_ratio - 1) < {FLAT_SHARE_TOLERANCE}
                        THEN 'distribution'
                    ELSE 'hazard' END AS outcome
        FROM _split_stage_steps s
        LEFT JOIN confirmed c USING (security_id, series, ex_date)
    """)
    con.execute(f"""
        INSERT INTO {relation}
        WITH raw_splits AS (
            SELECT security_id, series, ex_date, ex_at,
                   CASE WHEN split_field IS NOT NULL THEN ex_at ELSE greatest(ex_at, confirm_at) END AS known_at,
                   coalesce(split_field, k) AS ratio,
                   CASE WHEN split_field IS NOT NULL THEN 'vendor_split_field' ELSE 'vendor_factor+shares' END
                       AS evidence
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
                        WHEN outcome = 'distribution' THEN window_end_at END AS until_at,
                   CASE WHEN candidate THEN coalesce(split_field, k) ELSE share_ratio END AS ratio,
                   CASE WHEN outcome IN ('split', 'distribution') THEN 'pending_confirmation'
                        WHEN NOT candidate THEN 'flat_factor_split_signature'
                        WHEN shares_before IS NULL OR share_ratio IS NULL THEN 'no_share_data'
                        WHEN window_bars < {window + 1} THEN 'pending_confirmation'
                        ELSE 'shares_inconsistent' END AS evidence
            FROM _split_stage_classified
            WHERE outcome <> 'split' OR (split_field IS NULL AND confirm_at > ex_at)
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
        ), varying AS (
            SELECT security_id, series, max(factor) / min(factor) - 1 > 1e-9 AS varies
            FROM _split_stage_bars WHERE factor IS NOT NULL GROUP BY ALL
        ), runs AS (
            SELECT security_id, series, available_at, sum(starts_run) OVER ({partition}) AS run
            FROM (
                SELECT security_id, series, trade_date, available_at,
                       CASE WHEN date_diff('day', lag(trade_date) OVER ({partition}), trade_date)
                                 <= {COVERAGE_MAX_GAP_DAYS} THEN 0 ELSE 1 END AS starts_run
                FROM _split_stage_bars WHERE factor IS NOT NULL
            )
        ), coverage AS (
            SELECT r.security_id, r.series, min(r.available_at) AS first_at, max(r.available_at) AS last_at
            FROM runs r JOIN varying v ON v.security_id = r.security_id AND v.series = r.series
            -- A constant-factor series proves nothing when another series carries a real factor.
            WHERE v.varies OR NOT EXISTS (
                SELECT 1 FROM varying o WHERE o.security_id = r.security_id AND o.varies)
            GROUP BY r.security_id, r.series, r.run
        )
        SELECT security_id, 'split' AS kind, series, ex_date, ex_at AS from_at, known_at,
               NULL::TIMESTAMP AS until_at, ratio, evidence FROM splits
        UNION ALL
        SELECT security_id, 'distribution', series, ex_date, ex_at, window_end_at, NULL, k, 'distribution'
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


@contextmanager
def refresh_scope(store: Any, security_ids: Sequence[str] | None, *, persistent: bool) -> Iterator[str]:
    """Stage split epochs for a whole refresh in one pass over the bars.

    ``persistent`` stores the relation in the main catalog so it survives the
    driver's connection recycling (like its security-id page); otherwise it is
    a temporary table. The relation is dropped on exit.
    """
    name = f"_derived_split_epochs_{uuid4().hex}"
    relation, kind = f'"{name}"', "TEMP "
    if persistent:
        database = store.con.execute("SELECT current_database()").fetchone()
        assert database is not None
        catalog = str(database[0]).replace('"', '""')
        relation, kind = f'"{catalog}"."main"."{name}"', ""
    token = None
    try:
        _stage(store.con, relation, kind, security_ids)
        token = _ACTIVE.set((relation, None if security_ids is None else frozenset(security_ids)))
        yield relation
    finally:
        if token is not None:
            _ACTIVE.reset(token)
        store.con.execute(f"DROP TABLE IF EXISTS {relation}")


def prepare_split_epochs(con: Any, security_id: str) -> int:
    """Copy one security's split epochs into the per-security relations; return the split count.

    Inside :func:`refresh_scope` the rows come from the refresh's staged
    relation; otherwise this security's bars are staged on the spot (two
    scans of the bars table: use a refresh scope for more than a few ids).
    """
    active = _ACTIVE.get()
    staged = None
    if active is not None and (active[1] is None or security_id in active[1]):
        relation = active[0]
    else:
        relation = staged = f'"_split_stage_single_{uuid4().hex}"'
    try:
        if staged is not None:
            _stage(con, relation, "TEMP ", [security_id])
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {_EVENTS} AS
            SELECT ex_date, known_at AS available_at, ratio, evidence, kind, series, from_at AS ex_at
            FROM {relation} WHERE security_id = ? AND kind IN ('split', 'distribution')
        """, [security_id])
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {_HAZARDS} AS
            SELECT ex_date, from_at, until_at, ratio, evidence AS reason, series
            FROM {relation} WHERE security_id = ? AND kind = 'hazard'
        """, [security_id])
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {_COVERAGE} AS
            SELECT from_at AS first_at, until_at AS last_at, series
            FROM {relation} WHERE security_id = ? AND kind = 'coverage'
        """, [security_id])
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {_LISTS} AS
            SELECT (SELECT list(struct_pack(ex_date := ex_date, known_at := available_at, ratio := ratio,
                                            evidence := evidence) ORDER BY ex_date)
                    FROM {_EVENTS} WHERE kind = 'split') AS splits,
                   (SELECT list(struct_pack(ex_date := ex_date, from_at := from_at, until_at := until_at)
                                ORDER BY ex_date) FROM {_HAZARDS}) AS hazards,
                   (SELECT list(struct_pack(first_at := first_at, last_at := last_at) ORDER BY first_at)
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
    """The splits :func:`rebase_factor_sql` applies, with their evidence, for lineage."""
    return (f"list_transform({_applied(clock, frame_clock)}, lambda split_x: struct_pack("
            f"ex_date := split_x.ex_date, ratio := split_x.ratio, evidence := split_x.evidence))")


def basis_known_sql(clock: str, frame_clock: str) -> str:
    """Whether every split between ``clock`` and ``frame_clock`` is known and applied exactly."""
    covered = (f"list_filter({_list('coverage')}, lambda run_x: run_x.first_at <= ({clock}) "
               f"AND run_x.last_at >= ({frame_clock}) - INTERVAL {COVERAGE_GRACE_DAYS} DAY)")
    near = (f"list_filter({_list('splits')}, lambda split_x: split_x.known_at <= ({frame_clock}) "
            f"AND abs(date_diff('day', split_x.ex_date, CAST(({clock}) AS DATE))) <= {AMBIGUOUS_EPOCH_DAYS})")
    open_hazards = (f"list_filter({_list('hazards')}, lambda hazard_x: hazard_x.from_at <= ({frame_clock}) "
                    f"AND (hazard_x.until_at IS NULL OR hazard_x.until_at > ({frame_clock})) "
                    f"AND date_diff('day', CAST(({clock}) AS DATE), hazard_x.ex_date) >= -{AMBIGUOUS_EPOCH_DAYS})")
    return (f"(coalesce(len({covered}), 0) > 0 AND coalesce(len({near}), 0) = 0 "
            f"AND coalesce(len({open_hazards}), 0) = 0)")
