"""Shared adjusted-close basis: the one bar pick and the vendor artifact repair (VA1, v2 rule).

**Bar pick.** Every builder that reads ``equity_daily_bars`` picks one physical row per
``(security_id, trade_date)`` with :func:`bar_pick_order_sql`, the publisher's total order
(newest visible first, then source, vendor id, symbol, prices, load time; then the share count
for the builders that feed the repair). A whole row is picked, so a newer revision's NULL price
wins (never an older revision's value) and tied revisions give the same row on every run and
thread count (ruling C-20). :func:`bars_relation_sql` supplies the pick's optional columns as NULL
to a bars relation that lacks them.

**Artifact repair.** The vendor's adjusted close is ``close x cumulative factor``; the
day-over-day factor step ``k = (adjusted/close)_t / (adjusted/close)_(t-1)`` is above 1 on a
cash-dividend ex-date and far from 1 on a split. A *factor decrease* (``k < 1``) that is not a
split is a vendor artifact: no distribution lowers a total-return factor. The retained TickerHistory3
snapshot (2026-09-20) has one on its artifact session 2021-01-04 for ~3,430 lines (median 1.8 %,
up to 96 %), so every adjusted return spanning that date is biased down by a step that tracks the
line's dividends; 9 of its exact-ratio steps are the inverse of a later forward split of the line
applied from 2021-01-04 instead of from the line's start (CVNA x0.2, CRWD x0.25, MNST x0.5, ...).

A step is taken between consecutive factored bars (positive finite close and adjusted close) at
most ``_split_epochs.COVERAGE_MAX_GAP_DAYS`` calendar days apart (a step across a data gap is never
repaired: P8's ``factor_step_across_data_gap``) with ``ln k < -corporate_actions.FACTOR_NOISE``.
Let ``r = ln(close_t / close_(t-1))`` (the raw move across the step), ``h = |ln k| / 2`` (half the
step) and the **share veto** be: the step is split-like (``1/k >= market_daily.SPLIT_FACTOR_MIN``)
and the line's vendor share count moved by ``k`` on the same bar (within
:data:`SHARE_MOVE_TOLERANCE`). ``artifact`` (:data:`REPAIR_VERSION` ``vendor_artifact_repair_v2``,
ruling C-35) when the step is a decrease and one of:

(a) ``1 - k < _split_epochs.STOCK_DIVIDEND_MIN_STEP``: R1d never reads such a step as a split
    candidate and P8 always labels it ``factor_decrease`` (any session);
(b) **on a vendor artifact session** (:data:`ARTIFACT_SESSIONS`: the prior factored bar is before
    the session and the step's bar on or after it): every decrease, exact ratio or not, unless the
    raw close followed on the same bar (``r >= max(h, ln ARTIFACT_SESSION_MIN_RISE)``: HSDT 1:35,
    ISIG 1:7, TURN 1:3, ACOR 1:6 are kept) or the share veto holds;
(c) **off the artifact sessions** the step's ratio is not exact (an R1d stock-dividend grid ratio
    inside ``[SPLIT_MIN_RATIO, SPLIT_MAX_RATIO]`` or ``p/q`` with ``q <= 10`` outside it is an R1d
    split or unresolved split hazard, never repaired here), the raw close did not rise by ``h`` on
    the same bar (a consolidation whose price followed: IMOS, AIV), the share veto does not hold
    (FURY) and, outside R1d's non-split band (``k < SPLIT_MIN_RATIO``), the raw close did not rise
    by ``h`` on any of the :data:`LOOKBACK_SESSIONS` preceding bars either (the vendor dated the
    factor after the price: ARCM x5 one session earlier).

Exact-ratio steps are repaired only on an artifact session: off it, "no raw follow-through" is not
PIT-safe (of 25 historical exact decreases with a >= h raw jump within 5 sessions, 10 jump *after*
the factor step), so such steps stay with R1d/P8's hazard labels. The artifact sessions are
snapshot data, not a rule: :func:`artifact_sessions_sql` lists the sessions on which at least
:data:`ARTIFACT_SESSION_MIN_LINES` lines show a factor decrease (on the retained snapshot:
2021-01-04 only), so a new delivery's artifact session is surfaced, never assumed.

Repair: the repaired adjusted close is ``adjusted_close x exp(-sum of ln k over artifact steps at
or before the bar)`` per line, so the repaired return across an artifact step is the raw return
and every other step (dividends, splits, spin-offs) is untouched. The rule reads only the bar, its
predecessor and at most :data:`LOOKBACK_SESSIONS` earlier bars (all known before the bar), so it
adds no look-ahead. Returns inside a contiguous window depend only on the steps inside it (the
anchoring cancels), except that the look-back of a step within the window's first
:data:`LOOKBACK_SESSIONS` bars is cut short (off-session inexact out-of-band steps only). Label:
:data:`VENDOR_ARTIFACT_REPAIRED`.
"""

from __future__ import annotations

import datetime as dt

VENDOR_ARTIFACT_REPAIRED = "vendor_artifact_repaired"
# v2 (0.13 / C-35): artifact-session rule (exact ratios repaired on 2021-01-04 unless the price or the
# share count followed), same-bar raw-rise and share-count vetoes everywhere, 5-bar look-back veto
# off the session. v1 missed 12 exact-ratio artifact lines and fabricated ARCM/FURY/IMOS/AIV.
REPAIR_VERSION = "vendor_artifact_repair_v2"
#: Helper columns the repair adds (``va_ln_k``: the neutralized step, 0 elsewhere; ``va_multiplier``).
REPAIR_COLUMNS = ("va_ln_k", "va_multiplier")
#: Vendor artifact sessions of the pinned TickerHistory3 2026-09-20 snapshot (artifact_sessions_sql).
ARTIFACT_SESSIONS: tuple[dt.date, ...] = (dt.date(2021, 1, 4),)
#: A session with at least this many lines showing a factor decrease is an artifact candidate.
ARTIFACT_SESSION_MIN_LINES = 100
#: On an artifact session a decrease is kept only if the raw close rose by at least this ratio too.
ARTIFACT_SESSION_MIN_RISE = 1.25
#: Off the artifact sessions: preceding bars whose raw rise vetoes an out-of-band repair.
LOOKBACK_SESSIONS = 5
#: Share veto: same-bar vendor share ratio within this relative distance of ``k``.
SHARE_MOVE_TOLERANCE = 0.05


def bar_pick_order_sql(alias: str = "", *, with_shares: bool = False) -> str:
    """ORDER BY terms that pick one physical ``equity_daily_bars`` row per (security, date).

    The publisher's total order, shared by every bar builder (C-20): newest visible first (NULL
    ``available_at`` counts as the 22:00 UTC bar clock), then ``source`` ascending, vendor id,
    symbol, the higher adjusted and raw close, the latest load. ``alias`` qualifies the columns
    (``"b"`` -> ``b.available_at``). ``with_shares`` appends the larger vendor share count: every
    builder that feeds :func:`repaired_bars_sql` its ``shares`` passes it, so rows tied on the
    prices give the repair's share veto the same count in every builder. Builders that read
    further columns (volume, open) append their own tie-breaks after these terms.
    """
    p = f"{alias}." if alias else ""
    shares = f", {p}shares_outstanding DESC NULLS LAST" if with_shares else ""
    return (f"coalesce({p}available_at, CAST({p}trade_date AS TIMESTAMP) + INTERVAL '22 hours') DESC, "
            f"{p}source ASC, {p}vendor_security_id ASC NULLS LAST, {p}symbol ASC, "
            f"{p}adjusted_close DESC NULLS LAST, {p}close DESC NULLS LAST, {p}source_loaded_at DESC{shares}")


#: Columns the shared pick and the repair read beyond the key and price columns, with their
#: warehouse types (``schema.py``; ``shares_outstanding`` from migration 0263).
PICK_OPTIONAL_COLUMNS: tuple[tuple[str, str], ...] = (
    ("vendor_security_id", "VARCHAR"), ("source_loaded_at", "TIMESTAMP"), ("volume", "BIGINT"),
    ("shares_outstanding", "BIGINT"))


def bars_relation_sql(table: str = "equity_daily_bars") -> str:
    """``table`` as a parenthesized relation (the caller aliases it) with every column of
    :data:`PICK_OPTIONAL_COLUMNS`.

    ``UNION ALL BY NAME`` with an empty row set adds a column the relation lacks as NULL (minimal
    test fixtures; the warehouse table has them all) and leaves a present one as it is. The planner
    drops the empty branch, so the caller's filters and projections still reach the table scan.
    """
    nulls = ", ".join(f"CAST(NULL AS {kind}) AS {name}" for name, kind in PICK_OPTIONAL_COLUMNS)
    return f"(SELECT * FROM {table} UNION ALL BY NAME SELECT {nulls} WHERE false)"


def _rules(k: str) -> tuple[float, float, float, int, str, float]:
    """(min candidate step, non-split band floor, factor noise, max gap days, R1d exact-ratio SQL of
    ``k``, split-like floor)."""
    # Imported lazily: _split_epochs imports market_daily, which imports this module.
    from ._split_epochs import (
        COVERAGE_MAX_GAP_DAYS,
        SPLIT_MAX_RATIO,
        SPLIT_MIN_RATIO,
        STOCK_DIVIDEND_MIN_STEP,
        _simple_sql,
        _stock_dividend_sql,
    )
    from .corporate_actions import FACTOR_NOISE
    from .market_daily import SPLIT_FACTOR_MIN

    exact = (f"(CASE WHEN {k} BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO} THEN {_stock_dividend_sql(k)} "
             f"ELSE {_simple_sql(k)} END)")
    return (float(STOCK_DIVIDEND_MIN_STEP), float(SPLIT_MIN_RATIO), float(FACTOR_NOISE), int(COVERAGE_MAX_GAP_DAYS),
            exact, float(SPLIT_FACTOR_MIN))


#: Working columns of :func:`_steps_sql` dropped from every output.
_WORK_COLUMNS = ("va_close", "va_factor", "va_shares", "va_prior_factor", "va_prior_close", "va_prior_date",
                 "va_prior_shares", "va_k", "va_r", "va_share_k", "va_look_up", "va_step")


def _steps_sql(relation: str, *, close: str, adjusted: str, shares: str | None, key: str, order: str) -> str:
    """``relation`` plus the step columns: ``va_k`` (factor step from the prior factored bar),
    ``va_r`` (raw log move across it), ``va_share_k`` (same-bar vendor share ratio), ``va_look_up``
    (largest raw log move of the preceding bars) and ``va_step`` (a gap-bounded factor decrease)."""
    _, _, noise, gap, _, _ = _rules("va_k")
    prior = f"PARTITION BY {key} ORDER BY {order} ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING"
    look = f"PARTITION BY {key} ORDER BY {order} ROWS BETWEEN {LOOKBACK_SESSIONS} PRECEDING AND 1 PRECEDING"
    shares_sql = ("CAST(NULL AS DOUBLE)" if shares is None
                  else f"CASE WHEN {shares} > 0 THEN CAST({shares} AS DOUBLE) END")
    return f"""
            SELECT *, max(va_r) OVER ({look}) AS va_look_up,
                   coalesce(va_k IS NOT NULL AND date_diff('day', va_prior_date, {order}) <= {gap}
                            AND ln(va_k) < -{noise!r}, false) AS va_step
            FROM (
              SELECT *, CASE WHEN va_factor IS NOT NULL AND va_prior_factor IS NOT NULL
                             THEN va_factor / va_prior_factor END AS va_k,
                        CASE WHEN va_factor IS NOT NULL AND va_prior_factor IS NOT NULL
                             THEN ln(va_close / va_prior_close) END AS va_r,
                        CASE WHEN va_factor IS NOT NULL AND va_prior_shares > 0
                             THEN va_shares / va_prior_shares END AS va_share_k
              FROM (
                SELECT *,
                       last_value(va_factor IGNORE NULLS) OVER ({prior}) AS va_prior_factor,
                       last_value(CASE WHEN va_factor IS NOT NULL THEN va_close END IGNORE NULLS) OVER ({prior})
                           AS va_prior_close,
                       last_value(CASE WHEN va_factor IS NOT NULL THEN {order} END IGNORE NULLS) OVER ({prior})
                           AS va_prior_date,
                       -- the prior factored bar's own share count (-1: none), never an older bar's
                       last_value(CASE WHEN va_factor IS NOT NULL THEN coalesce(va_shares, -1.0) END IGNORE NULLS)
                           OVER ({prior}) AS va_prior_shares
                FROM (
                    SELECT *, CAST({close} AS DOUBLE) AS va_close, {shares_sql} AS va_shares,
                           CASE WHEN {close} > 0 AND {adjusted} > 0 AND isfinite({close}) AND isfinite({adjusted})
                                THEN CAST({adjusted} AS DOUBLE) / CAST({close} AS DOUBLE) END AS va_factor
                    FROM {relation}
                ) va_base
              ) va_prior
            ) va_ratio"""


def repaired_bars_sql(relation: str, *, close: str = "close", adjusted: str = "adjusted_close",
                      shares: str | None = None, key: str = "security_id", order: str = "trade_date") -> str:
    """SELECT of ``relation`` (one row per ``key``/``order``) plus :data:`REPAIR_COLUMNS`.

    ``relation`` is a relation name or a parenthesized subquery; ``close``/``adjusted`` name its
    raw and vendor-adjusted price columns (NULL or non-positive prices are allowed: such a bar is
    not factored and neither starts nor ends a step) and ``shares`` its vendor share count column
    (the same physical bar's; ``None`` disables the share veto, so every production caller passes
    it). The repaired adjusted close of a row is ``adjusted * va_multiplier``. Binds no parameters.
    """
    step, band, _, _, exact, split_like = _rules("va_k")
    half = "(-0.5 * ln(va_k))"
    share_veto = (f"(1 / va_k >= {split_like!r} AND va_share_k IS NOT NULL "
                  f"AND abs(va_share_k / va_k - 1) <= {SHARE_MOVE_TOLERANCE!r})")
    on_session = " OR ".join(f"(va_prior_date < DATE '{day.isoformat()}' AND {order} >= DATE '{day.isoformat()}')"
                             for day in ARTIFACT_SESSIONS) or "false"
    return f"""
        SELECT * EXCLUDE ({", ".join(_WORK_COLUMNS)}),
               exp(-sum(va_ln_k) OVER (PARTITION BY {key} ORDER BY {order}
                                       ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)) AS va_multiplier
        FROM (
            SELECT *,
                   CASE WHEN va_step
                             AND (1 - va_k < {step!r}
                                  OR (({on_session})
                                      AND NOT coalesce(va_r >= greatest({half}, ln({ARTIFACT_SESSION_MIN_RISE!r})), false)
                                      AND NOT coalesce({share_veto}, false))
                                  OR (NOT ({on_session}) AND NOT {exact}
                                      AND NOT coalesce(va_r >= {half}, false)
                                      AND NOT coalesce({share_veto}, false)
                                      AND (va_k >= {band!r} OR NOT coalesce(va_look_up >= {half}, false))))
                        THEN ln(va_k) ELSE 0.0 END AS va_ln_k
            FROM ({_steps_sql(relation, close=close, adjusted=adjusted, shares=shares, key=key, order=order)}
            ) va_steps
        ) va_classified"""


def artifact_sessions_sql(relation: str, *, close: str = "close", adjusted: str = "adjusted_close",
                          key: str = "security_id", order: str = "trade_date",
                          min_lines: int = ARTIFACT_SESSION_MIN_LINES) -> str:
    """SELECT ``session, lines`` of the sessions on which ``>= min_lines`` lines show a factor decrease.

    The artifact-session detector (C-35): run it over a new delivery's bars (one row per
    ``key``/``order``) and compare with :data:`ARTIFACT_SESSIONS`; a session it lists that is not
    pinned is a new vendor artifact to review, never repaired by assumption. Binds no parameters.
    """
    return f"""
        SELECT {order} AS session, count(DISTINCT {key}) AS lines
        FROM ({_steps_sql(relation, close=close, adjusted=adjusted, shares=None, key=key, order=order)}
        ) va_steps
        WHERE va_step
        GROUP BY {order}
        HAVING count(DISTINCT {key}) >= {int(min_lines)}"""


__all__ = [
    "ARTIFACT_SESSIONS",
    "ARTIFACT_SESSION_MIN_LINES",
    "ARTIFACT_SESSION_MIN_RISE",
    "LOOKBACK_SESSIONS",
    "PICK_OPTIONAL_COLUMNS",
    "REPAIR_COLUMNS",
    "REPAIR_VERSION",
    "SHARE_MOVE_TOLERANCE",
    "VENDOR_ARTIFACT_REPAIRED",
    "artifact_sessions_sql",
    "bar_pick_order_sql",
    "bars_relation_sql",
    "repaired_bars_sql",
]
