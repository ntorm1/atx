"""Vendor adjustment-artifact repair: one shared repaired adjusted-close series (VA1).

The vendor's adjusted close is ``close x cumulative factor``; the day-over-day factor step
``k = (adjusted/close)_t / (adjusted/close)_(t-1)`` is above 1 on a cash-dividend ex-date and
far from 1 on a split. A *factor decrease* (``k < 1``) that is not a split is a vendor artifact:
no distribution lowers a total-return factor. The retained TickerHistory3 snapshot has one on
2021-01-04 for 3,415 dividend payers (median 1.8 %, up to 96 %), so every adjusted return
spanning that date is biased down by the step, which tracks the line's dividends.

P8 (``corporate_actions``) labels exactly these steps ``adjustment_unclassified`` / reason
``factor_decrease``: a step with ``k < 1`` that R1d (``_split_epochs``) does not read as a split
or an unresolved split candidate and that does not cross a data gap. This module neutralizes
them in every return builder with one bar-level rule on consecutive factored bars:

``artifact`` when all hold
    * both bars are factored (positive finite close and adjusted close) and at most
      ``_split_epochs.COVERAGE_MAX_GAP_DAYS`` calendar days apart (a step across a data gap is
      never repaired: P8's ``factor_step_across_data_gap``);
    * ``ln k < -corporate_actions.FACTOR_NOISE`` (a factor decrease);
    * either ``1 - k < _split_epochs.STOCK_DIVIDEND_MIN_STEP`` (R1d never reads such a step as a
      split candidate, so P8 always labels it ``factor_decrease``), or the step is a larger
      decrease whose ratio is not exact (not a stock-dividend grid ratio inside
      ``[SPLIT_MIN_RATIO, SPLIT_MAX_RATIO]``, not ``p/q`` with ``q <= 10`` outside it; exact
      ratios are R1d splits or unresolved split hazards, never repaired here) and that is either
      inside R1d's non-split band (``k >= SPLIT_MIN_RATIO``: R1d reads an inexact in-band step as
      a distribution, P8 as ``factor_decrease``) or a raw close that did **not** follow the
      factor: ``|ln(close_t / close_(t-1))| < |ln(adjusted_t / adjusted_(t-1))|``. A genuine
      reverse split moves the raw close by ``~1/k`` and leaves the adjusted return near zero, so
      an inexact out-of-band decrease whose price followed is kept.

On the retained TickerHistory3 snapshot this reproduces P8's ``factor_decrease`` steps
(including all 3,415 of 2021-01-04) except the inexact decreases whose raw price followed
(reverse-split-like, kept); the VA1 report has the reconciliation.

Repair: the repaired adjusted close is ``adjusted_close x exp(-sum of ln k over artifact steps
at or before the bar)`` per line, so the repaired return across an artifact step is the raw
return times the step's other factor moves (none: the step is exactly neutralized) and every
other step (dividends, splits, spin-offs) is untouched. Returns inside any contiguous window
depend only on the steps inside it (the anchoring cancels), so a date-scoped read gives the
same returns as a full-history read. The rule reads only the bar and its predecessor (known
before the bar), so it adds no look-ahead. Label: :data:`VENDOR_ARTIFACT_REPAIRED`.
"""

from __future__ import annotations

VENDOR_ARTIFACT_REPAIRED = "vendor_artifact_repaired"
REPAIR_VERSION = "vendor_artifact_repair_v1"
#: Helper columns the repair adds (``va_ln_k``: the neutralized step, 0 elsewhere; ``va_multiplier``).
REPAIR_COLUMNS = ("va_ln_k", "va_multiplier")


def _rules(k: str) -> tuple[float, float, float, int, str]:
    """(min candidate step, non-split band floor, factor noise, max gap days, R1d exact-ratio SQL of ``k``)."""
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

    exact = (f"(CASE WHEN {k} BETWEEN {SPLIT_MIN_RATIO} AND {SPLIT_MAX_RATIO} THEN {_stock_dividend_sql(k)} "
             f"ELSE {_simple_sql(k)} END)")
    return (float(STOCK_DIVIDEND_MIN_STEP), float(SPLIT_MIN_RATIO), float(FACTOR_NOISE), int(COVERAGE_MAX_GAP_DAYS),
            exact)


def repaired_bars_sql(relation: str, *, close: str = "close", adjusted: str = "adjusted_close",
                      key: str = "security_id", order: str = "trade_date") -> str:
    """SELECT of ``relation`` (one row per ``key``/``order``) plus :data:`REPAIR_COLUMNS`.

    ``relation`` is a relation name or a parenthesized subquery; ``close``/``adjusted`` name its
    raw and vendor-adjusted price columns (NULL or non-positive prices are allowed: such a bar is
    not factored and neither starts nor ends a step). The repaired adjusted close of a row is
    ``adjusted * va_multiplier``. Binds no parameters.
    """
    step, band, noise, gap, exact = _rules("va_k")
    prior = f"PARTITION BY {key} ORDER BY {order} ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING"
    return f"""
        SELECT * EXCLUDE (va_factor, va_prior_factor, va_prior_close, va_prior_date, va_close, va_k),
               exp(-sum(va_ln_k) OVER (PARTITION BY {key} ORDER BY {order}
                                       ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)) AS va_multiplier
        FROM (
            SELECT *,
                   CASE WHEN va_k IS NOT NULL AND date_diff('day', va_prior_date, {order}) <= {gap}
                             AND ln(va_k) < -{noise!r}
                             AND (1 - va_k < {step!r}
                                  OR (NOT {exact}
                                      AND (va_k >= {band!r}
                                           OR abs(ln(va_close / va_prior_close))
                                              < abs(ln(va_close / va_prior_close) + ln(va_k)))))
                        THEN ln(va_k) ELSE 0.0 END AS va_ln_k
            FROM (
              SELECT *, CASE WHEN va_factor IS NOT NULL AND va_prior_factor IS NOT NULL
                             THEN va_factor / va_prior_factor END AS va_k
              FROM (
                SELECT *,
                       last_value(va_factor IGNORE NULLS) OVER ({prior}) AS va_prior_factor,
                       last_value(CASE WHEN va_factor IS NOT NULL THEN va_close END IGNORE NULLS) OVER ({prior})
                           AS va_prior_close,
                       last_value(CASE WHEN va_factor IS NOT NULL THEN {order} END IGNORE NULLS) OVER ({prior})
                           AS va_prior_date
                FROM (
                    SELECT *, CAST({close} AS DOUBLE) AS va_close,
                           CASE WHEN {close} > 0 AND {adjusted} > 0 AND isfinite({close}) AND isfinite({adjusted})
                                THEN CAST({adjusted} AS DOUBLE) / CAST({close} AS DOUBLE) END AS va_factor
                    FROM {relation}
                ) va_base
              ) va_prior
            ) va_ratio
        ) va_steps"""


__all__ = ["REPAIR_COLUMNS", "REPAIR_VERSION", "VENDOR_ARTIFACT_REPAIRED", "repaired_bars_sql"]
