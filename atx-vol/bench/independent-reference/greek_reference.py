"""Independent Delta/Gamma reference experiment; see greek-plan-v1.md.

SciPy linear algebra only; no production option pricing implementation is called.
The direct stage LCP adaptation does not inherit a penalty-method error theorem.
"""
import math

import numpy as np
from scipy.linalg import solve_banded

from pde_reference import _boundary, _multiply, solve_obstacle


def classify_stencil(values, payoff, continuation, tolerance):
    """Use actual final-stage residuals, not a price-only intrinsic test."""
    arrays = [np.asarray(a, dtype=float) for a in (values, payoff, continuation)]
    if any(a.shape != (3,) or not np.isfinite(a).all() for a in arrays) or \
            not math.isfinite(tolerance) or tolerance <= 0:
        raise ValueError("Invalid three-point LCP evidence")
    values, payoff, continuation = arrays
    gap = values-payoff
    active = (np.abs(gap) <= tolerance) & (continuation > tolerance) & (payoff > 0)
    inactive = (gap > tolerance) & (np.abs(continuation) <= tolerance)
    if np.all(active):
        return "exercise"
    if np.all(inactive):
        return "continuation"
    return "mixed" if np.all(active | inactive) else "ambiguous"


def _validate(model, nx, nt, domain_scale, scheme, american):
    if type(nx) is not int or nx < 33 or nx % 2 != 1 or nx > 32769 or \
            type(nt) is not int or nt < 4 or nt > 4096 or \
            type(domain_scale) not in (int, float) or not math.isfinite(domain_scale) or domain_scale < 1.:
        raise ValueError("Require bounded odd nx>=33, nt>=4 and domain_scale>=1")
    if scheme not in ("cn", "dirk_l", "dirk_third") or type(american) is not bool:
        raise ValueError("Invalid scheme/exercise style")
    stages = 1 if scheme == "cn" else 2
    if nx * nt * stages > 2**27:
        raise ValueError("Declared stage-node work budget exceeded")
    if model.get("dividends") or model.get("side") not in ("put", "call"):
        raise ValueError("Only explicit continuous-carry put/call models supported")
    scalars = [model.get(k) for k in ("S", "K", "T", "sigma", "r", "q")]
    if any(type(x) not in (int, float) or not math.isfinite(x) for x in scalars) or \
            any(x <= 0 for x in scalars[:4]):
        raise ValueError("Require finite model with positive S/K/T/sigma")
    return scalars


def greek_reference(model, *, nx=1025, nt=256, domain_scale=1.0,
                    scheme="dirk_l", american=True):
    """Return diagnostics and partial derivatives, never an accuracy certificate."""
    spot, strike, maturity, sigma, rate, carry = _validate(
        model, nx, nt, domain_scale, scheme, american)
    call = model["side"] == "call"
    drift = rate-carry-.5*sigma*sigma
    width = domain_scale * max(.1, 8*sigma*math.sqrt(maturity)+abs(drift*maturity),
                               abs(math.log(strike)-math.log(spot))+.1)
    if not math.isfinite(width) or width > 100.:
        raise ValueError("Unrepresentable reference domain")
    dx = 2*width/(nx-1)
    spatial = spot*np.exp(np.linspace(-width, width, nx))
    center = nx//2
    spatial[center] = spot
    if not np.isfinite(spatial).all() or np.any(spatial <= 0) or np.any(np.diff(spatial) <= 0):
        raise ValueError("Unrepresentable reference grid")
    lo = .5*sigma*sigma/dx**2-.5*drift/dx
    diag = -sigma*sigma/dx**2-rate
    hi = .5*sigma*sigma/dx**2+.5*drift/dx
    if not all(math.isfinite(x) for x in (lo, diag, hi)) or lo < 0 or hi < 0:
        raise ValueError("Refine spatial grid to preserve monotonicity")
    payoff = np.maximum(spatial-strike if call else strike-spatial, 0.)
    values = payoff.copy()
    times = maturity*(np.arange(nt+1, dtype=float)/nt)**2
    tolerance = 1e-12*max(1., spot, strike)
    worst_residual, iterations, max_iterations, stage_count = 0., 0, 0, 0
    final_continuation = np.zeros(nx-2)

    def operator(full):
        return lo*full[:-2]+diag*full[1:-1]+hi*full[2:]

    def stage(rhs, theta_dt, boundary, initial):
        nonlocal worst_residual, iterations, max_iterations, stage_count
        lower = np.full(nx-3, -theta_dt*lo)
        diagonal = np.full(nx-2, 1-theta_dt*diag)
        upper = np.full(nx-3, -theta_dt*hi)
        if not math.isfinite(theta_dt) or theta_dt <= 0 or 1+theta_dt*rate <= 0:
            raise ValueError("Non-monotone implicit stage")
        rhs = rhs.copy()
        rhs[0] += theta_dt*lo*boundary[0]
        rhs[-1] += theta_dt*hi*boundary[1]
        if not np.isfinite(rhs).all():
            raise ArithmeticError("Nonfinite stage right-hand side")
        if american:
            result = solve_obstacle(lower, diagonal, upper, rhs, payoff[1:-1],
                                    initial[1:-1], tolerance=tolerance)
            interior = result["values"]
            worst_residual = max(worst_residual, result["complementarity_residual"])
            iterations += result["iterations"]
            max_iterations = max(max_iterations, result["iterations"])
        else:
            band = np.zeros((3, nx-2))
            band[0, 1:], band[1], band[2, :-1] = upper, diagonal, lower
            interior = solve_banded((1, 1), band, rhs, check_finite=False)
        if not np.isfinite(interior).all():
            raise ArithmeticError("Nonfinite stage solution")
        full = np.empty(nx)
        full[0], full[-1], full[1:-1] = boundary[0], boundary[1], interior
        continuation = _multiply(lower, diagonal, upper, interior)-rhs
        stage_count += 1
        return full, continuation

    for step in range(nt):
        dt = float(times[step+1]-times[step])
        boundary = [_boundary(float(s), strike, float(times[step+1]), rate, carry, call, american)
                    for s in (spatial[0], spatial[-1])]
        if not all(math.isfinite(x) for x in boundary):
            raise ArithmeticError("Nonfinite far boundary")
        old = values
        old_op = operator(old)
        if step < 2 or scheme == "cn":
            theta = 1. if step < 2 else .5
            rhs = old[1:-1]+(1-theta)*dt*old_op
            values, final_continuation = stage(rhs, theta*dt, boundary, old)
        else:
            theta = 1-math.sqrt(.5) if scheme == "dirk_l" else 1/3
            first, _ = stage(old[1:-1]+(1-theta)*dt*old_op, theta*dt, boundary, old)
            rhs = old[1:-1]+.5*dt*old_op+(.5-theta)*dt*operator(first)
            values, final_continuation = stage(rhs, theta*dt, boundary, first)

    stencil = values[center-1:center+2]
    stencil_payoff = payoff[center-1:center+2]
    continuation = final_continuation[center-2:center+1]
    regime = classify_stencil(stencil, stencil_payoff, continuation, tolerance) if american else "continuation"
    delta, gamma = None, None
    if regime == "exercise":
        delta, gamma = (1. if call else -1.), 0.
    elif regime == "continuation":
        vx = (stencil[2]-stencil[0])/(2*dx)
        vxx = ((stencil[2]-stencil[1])-(stencil[1]-stencil[0]))/(dx*dx)
        delta, gamma = float(vx/spot), float((vxx-vx)/(spot*spot))
        if not math.isfinite(delta) or not math.isfinite(gamma):
            raise ArithmeticError("Nonfinite derivative")
    return dict(price=float(values[center]), delta=delta, gamma=gamma, regime=regime,
                stencil_values=stencil.tolist(), stencil_payoff=stencil_payoff.tolist(),
                stencil_continuation=continuation.tolist(), stencil_spots=spatial[center-1:center+2].tolist(),
                nx=nx, nt=nt, domain_scale=domain_scale, scheme=scheme, american=american,
                log_half_width=width, log_spacing=dx, domain=[float(spatial[0]), float(spatial[-1])],
                lcp_tolerance=tolerance, max_complementarity_residual=worst_residual,
                policy_iterations=iterations, max_policy_iterations=max_iterations,
                stages=stage_count, stage_node_updates=stage_count*(nx-2),
                accuracy_established=False, greek_accuracy_established=False)
