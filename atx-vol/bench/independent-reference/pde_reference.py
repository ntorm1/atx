"""Independent, offline finite-difference reference; not shipping pricing code.

Central log-price differences, graded time steps with initial implicit damping,
and a policy-iteration solve of each discrete obstacle problem. Numerical
refinement is empirical evidence, not an interval error certificate. No ATX or
QuantLib numerical implementation is called here. Requires NumPy and SciPy.
"""
import math

import numpy as np
from scipy.linalg import solve_banded


def _multiply(lower, diagonal, upper, values):
    result = diagonal * values
    result[1:] += lower * values[:-1]
    result[:-1] += upper * values[1:]
    return result


def solve_obstacle(lower, diagonal, upper, rhs, payoff, initial, tolerance=1e-11):
    """Solve min(M*v-rhs,v-payoff)=0 for a strictly dominant M-matrix.

    Policy iteration: each row chooses the smaller current equation residual,
    then solves those equations together. Reisinger and Witte (2012),
    https://arxiv.org/abs/1012.4976 . A linear solve followed by clipping is not
    equivalent. Every returned solution must pass the complementarity residual.
    """
    arrays = [np.asarray(a, dtype=float) for a in [lower, diagonal, upper, rhs, payoff, initial]]
    lower, diagonal, upper, rhs, payoff, initial = arrays
    n = len(diagonal)
    if n < 1 or any(a.ndim != 1 or not np.isfinite(a).all() for a in arrays) or \
            any(len(a) != n for a in [rhs, payoff, initial]) or \
            len(lower) != n - 1 or len(upper) != n - 1 or \
            not math.isfinite(tolerance) or tolerance <= 0:
        raise ValueError('Invalid obstacle arrays/tolerance')
    row_sum = diagonal.copy()
    row_sum[1:] += lower
    row_sum[:-1] += upper
    if np.any(lower > 0) or np.any(upper > 0) or np.any(row_sum <= 0):
        raise ValueError('Reference requires a strictly dominant M-matrix')
    values = initial.copy()
    band = np.zeros((3, n))
    for iteration in range(n + 2):
        continuation = _multiply(lower, diagonal, upper, values) - rhs
        gap = values - payoff
        residual = float(np.max(np.abs(np.minimum(continuation, gap))))
        if residual <= tolerance:
            return dict(values=values, iterations=iteration, complementarity_residual=residual)
        exercise = gap < continuation
        band[0, 1:] = np.where(exercise[:-1], 0., upper)
        band[1] = np.where(exercise, 1., diagonal)
        band[2, :-1] = np.where(exercise[1:], 0., lower)
        values = solve_banded((1, 1), band, np.where(exercise, payoff, rhs), check_finite=False)
        if not np.isfinite(values).all():
            raise ArithmeticError('Nonfinite policy solve')
    raise ArithmeticError(f'Obstacle iteration did not converge: residual={residual}')


def _boundary(spot, strike, tau, rate, carry, call, american):
    # Exact deterministic exercise envelope used as a far-boundary asymptote.
    # Its truncation error is NOT assumed zero: independent domain expansion is
    # mandatory before a reference can be declared resolved.
    def value(time):
        result = spot * math.exp(-carry * time) - strike * math.exp(-rate * time)
        return max(result if call else -result, 0.)
    if not american:
        return value(tau)
    times = [0., tau]
    if rate * carry > 0. and rate != carry:
        stationary = (math.log(abs(carry)) + math.log(spot) -
                      math.log(abs(rate)) - math.log(strike)) / (carry - rate)
        if 0. < stationary < tau:
            times.append(stationary)
    return max(value(t) for t in times)


def price_group(group, *, nx=1025, nt=512, domain_scale=1.0, american=True):
    """Price spots sharing a model in one independent PDE solve.

    This routine returns diagnostics only; it does not label its output accurate.
    Compare time, space and domain refinements separately. Cash dividends and
    Greeks are intentionally not represented by this initial model contract.
    """
    if group.get('dividends'):
        raise ValueError('This reference does not support discrete cash dividends')
    if isinstance(nx, bool) or not isinstance(nx, int) or nx < 33 or nx % 2 != 1 or \
            isinstance(nt, bool) or not isinstance(nt, int) or nt < 4 or \
            not math.isfinite(domain_scale) or domain_scale < 1.:
        raise ValueError('Require odd nx>=33, nt>=4 and domain_scale>=1')
    strike, maturity, sigma, rate, carry = (group[k] for k in ['K', 'T', 'sigma', 'r', 'q'])
    scalars = [strike, maturity, sigma, rate, carry]
    spots = np.asarray(group['spots'], dtype=float)
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in scalars) or \
            strike <= 0 or maturity <= 0 or sigma <= 0 or spots.ndim != 1 or not len(spots) or \
            not np.isfinite(spots).all() or np.any(spots <= 0) or group.get('side', 'put') not in ['put', 'call']:
        raise ValueError('Invalid model/spots/side')
    call = group.get('side', 'put') == 'call'
    drift = rate - carry - .5 * sigma * sigma
    width = domain_scale * max(math.log(4.), 8. * sigma * math.sqrt(maturity) + abs(drift * maturity),
                               float(np.max(np.abs(np.log(spots / strike)))) + math.log(2.))
    if not math.isfinite(width) or width > 100.:
        raise ValueError('Reference domain is unrepresentable')
    log_grid = np.linspace(-width, width, nx)
    spatial = strike * np.exp(log_grid)
    dx = 2. * width / (nx - 1)
    lower_op = .5 * sigma * sigma / dx ** 2 - .5 * drift / dx
    center_op = -sigma * sigma / dx ** 2 - rate
    upper_op = .5 * sigma * sigma / dx ** 2 + .5 * drift / dx
    if lower_op < 0. or upper_op < 0.:
        raise ValueError('Refine spatial grid to preserve monotonicity')
    payoff = np.maximum(spatial - strike if call else strike - spatial, 0.)
    values = payoff.copy()
    time_grid = maturity * (np.arange(nt + 1, dtype=float) / nt) ** 2
    worst_residual = 0.
    policy_solves = 0
    max_iterations = 0
    for step in range(nt):
        dt = time_grid[step + 1] - time_grid[step]
        theta = 1. if step < 2 else .5
        rhs = values[1:-1] + (1. - theta) * dt * (
            lower_op * values[:-2] + center_op * values[1:-1] + upper_op * values[2:])
        low_boundary = _boundary(float(spatial[0]), strike, time_grid[step + 1], rate, carry, call, american)
        high_boundary = _boundary(float(spatial[-1]), strike, time_grid[step + 1], rate, carry, call, american)
        a, b, c = -theta * dt * lower_op, 1. - theta * dt * center_op, -theta * dt * upper_op
        rhs[0] -= a * low_boundary
        rhs[-1] -= c * high_boundary
        diagonal = np.full(nx - 2, b)
        lower = np.full(nx - 3, a)
        upper = np.full(nx - 3, c)
        if american:
            solution = solve_obstacle(lower, diagonal, upper, rhs, payoff[1:-1], values[1:-1],
                                      tolerance=1e-11 * max(1., strike))
            values[1:-1] = solution['values']
            worst_residual = max(worst_residual, solution['complementarity_residual'])
            policy_solves += solution['iterations']
            max_iterations = max(max_iterations, solution['iterations'])
        else:
            band = np.zeros((3, nx - 2))
            band[0, 1:], band[1], band[2, :-1] = upper, diagonal, lower
            values[1:-1] = solve_banded((1, 1), band, rhs, check_finite=False)
        values[0], values[-1] = low_boundary, high_boundary
    # Linear interpolation in S preserves the affine exercise payoff exactly.
    prices = np.interp(spots, spatial, values)
    if not np.isfinite(prices).all():
        raise ArithmeticError('Nonfinite final prices')
    return dict(prices=prices.tolist(), nx=nx, nt=nt, domain_scale=domain_scale,
                log_half_width=width, domain=[float(spatial[0]), float(spatial[-1])],
                american=american, policy_solves=policy_solves, max_policy_iterations=max_iterations,
                max_complementarity_residual=worst_residual,
                scheme='graded-time CN with first two implicit steps; exact discrete LCP policy iteration',
                accuracy_established=False, greek_accuracy_established=False)
