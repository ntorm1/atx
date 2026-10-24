"""Explicit offline one-correction LCP experiment, not a production solver.

The default delegates to the unchanged original solver. The opt-in route uses
binary64 for both selected-policy solves and original unscaled residuals; this
is an empirical linear-algebra control, not a higher-precision error bound.
"""
import math

import numpy as np
from scipy.linalg import solve_banded

from pde_reference import _multiply, solve_obstacle


class LcpCorrectionRefused(ArithmeticError):
    """An experimental solve refused, with bounded-work diagnostic evidence."""

    def __init__(self, diagnostics):
        self.diagnostics = diagnostics
        super().__init__(f"One-correction obstacle refused: {diagnostics['reason']}; "
                         f"last_checked_residual={diagnostics['last_checked_residual']}")


def _one_correction(lower, diagonal, upper, rhs, payoff, initial, tolerance, max_linear_solves):
    if isinstance(tolerance, (bool, np.bool_)) or \
            not isinstance(tolerance, (int, float, np.integer, np.floating)):
        raise ValueError("Invalid obstacle arrays/tolerance")
    try:
        tolerance = float(tolerance)
    except OverflowError as error:
        raise ValueError("Invalid obstacle arrays/tolerance") from error
    inputs = (lower, diagonal, upper, rhs, payoff, initial)
    if any(np.iscomplexobj(a) for a in inputs):
        raise ValueError("Invalid obstacle arrays/tolerance")
    try:
        arrays = [np.asarray(a, dtype=np.float64) for a in inputs]
    except (TypeError, OverflowError) as error:
        raise ValueError("Invalid obstacle arrays/tolerance") from error
    if any(a.ndim != 1 or not np.isfinite(a).all() for a in arrays):
        raise ValueError("Invalid obstacle arrays/tolerance")
    lower, diagonal, upper, rhs, payoff, initial = arrays
    n = len(diagonal)
    if n < 1 or any(len(a) != n for a in (rhs, payoff, initial)) or \
            len(lower) != n-1 or len(upper) != n-1 or \
            not math.isfinite(tolerance) or tolerance <= 0:
        raise ValueError("Invalid obstacle arrays/tolerance")
    if max_linear_solves is not None and \
            (type(max_linear_solves) is not int or max_linear_solves < 0):
        raise ValueError("Require a nonnegative integer linear-solve budget")
    with np.errstate(over="ignore", invalid="ignore"):
        row_sum = diagonal.copy()
        row_sum[1:] += lower
        row_sum[:-1] += upper
    if np.any(lower > 0) or np.any(upper > 0) or \
            not np.isfinite(row_sum).all() or np.any(row_sum <= 0):
        raise ValueError("Reference requires a strictly dominant M-matrix")

    bound = n+2
    budget = 2*bound if max_linear_solves is None else min(max_linear_solves, 2*bound)
    work = dict(mode="one_correction", iterations=0, policy_solves=0, correction_solves=0,
                linear_solves=0, residual_checks=0, policy_iteration_bound=bound,
                linear_solve_bound=2*bound, linear_solve_budget=budget, tolerance=float(tolerance))
    last_values, last_residual = None, None

    def snapshot(values):
        if values is None:
            return None
        out = values.copy()
        out.flags.writeable = False
        return out

    def refuse(reason):
        raise LcpCorrectionRefused(dict(work, reason=reason,
                                       last_checked_values=snapshot(last_values),
                                       last_checked_residual=last_residual))

    def inspect(values):
        nonlocal last_values, last_residual
        work["residual_checks"] += 1
        with np.errstate(over="ignore", invalid="ignore"):
            continuation = _multiply(lower, diagonal, upper, values)-rhs
            gap = values-payoff
        if not np.isfinite(continuation).all() or not np.isfinite(gap).all():
            refuse("nonfinite_original_residual")
        residual = float(np.max(np.abs(np.minimum(continuation, gap))))
        # These references always describe the same checked iterate. Future
        # policy updates create a new array, and publication snapshots ownership.
        last_values, last_residual = values, residual
        return continuation, gap, residual

    def linear(band, selected_rhs, kind):
        work[kind] += 1
        work["linear_solves"] += 1
        try:
            answer = solve_banded((1, 1), band, selected_rhs, check_finite=False,
                                  overwrite_ab=False, overwrite_b=False)
        except np.linalg.LinAlgError:
            refuse("linear_solve_failure")
        if not np.isfinite(answer).all():
            refuse("nonfinite_"+kind)
        return answer

    def result(values, residual):
        return dict(work, values=snapshot(values), complementarity_residual=residual)

    values = initial.copy()
    continuation, gap, residual = inspect(values)
    if residual <= tolerance:
        return result(values, residual)
    band = np.zeros((3, n), dtype=np.float64)
    for _ in range(bound):
        # Reserve a whole selected-policy/correction pair. Resource refusal may
        # not publish an uncorrected candidate or execute a partial pair.
        if budget-work["linear_solves"] < 2:
            refuse("linear_budget")
        exercise = gap < continuation
        band[0, 1:] = np.where(exercise[:-1], 0., upper)
        band[1] = np.where(exercise, 1., diagonal)
        band[2, :-1] = np.where(exercise[1:], 0., lower)
        selected_rhs = np.where(exercise, payoff, rhs)
        candidate = linear(band, selected_rhs, "policy_solves")
        with np.errstate(over="ignore", invalid="ignore"):
            correction_rhs = selected_rhs-_multiply(band[2, :-1], band[1], band[0, 1:], candidate)
        if not np.isfinite(correction_rhs).all():
            refuse("nonfinite_correction_rhs")
        correction = linear(band, correction_rhs, "correction_solves")
        with np.errstate(over="ignore", invalid="ignore"):
            values = candidate+correction
        if not np.isfinite(values).all():
            refuse("nonfinite_corrected_values")
        work["iterations"] += 1
        continuation, gap, residual = inspect(values)
        if residual <= tolerance:
            return result(values, residual)
    # Admission was checked after the final permitted update too. A refusal's
    # diagnostic residual belongs to its saved vector, never the prior iterate.
    refuse("policy_iteration_limit")


def solve_obstacle_control(lower, diagonal, upper, rhs, payoff, initial,
                           tolerance=1e-11, *, mode="original", max_linear_solves=None):
    """Select an explicit offline mode; default result keys/arithmetic stay old.

    The corrected mode allows at most n+2 policy updates and 2*(n+2) linear
    calls, optionally reduced by max_linear_solves. Counters count attempted
    calls; iterations count completed corrected updates. Refusals retain only
    the last finite checked vector/residual pair, labeled as such.
    """
    if type(mode) is not str or mode not in ("original", "one_correction"):
        raise ValueError("Unknown offline obstacle mode")
    if mode == "original":
        if max_linear_solves is not None:
            raise ValueError("Linear-solve budget applies only to the correction experiment")
        return solve_obstacle(lower, diagonal, upper, rhs, payoff, initial, tolerance)
    return _one_correction(lower, diagonal, upper, rhs, payoff, initial,
                           tolerance, max_linear_solves)
