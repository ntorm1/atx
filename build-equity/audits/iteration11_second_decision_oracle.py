"""Diagnostic second-decision separable KKT oracle; never a native proposal.

Reconstructs iteration 10's one recorded allocation and carry to execution row 6,
then solves the original decision-row-5 preference problem using independently
computed holdings/risk. The shifted full-L1 proximal map and scalar dual searches
are independent Python/NumPy math. Missing entry prices are reported, never used
to alter optimization or manufacture executable units. No native code imports.
"""
from __future__ import annotations

from decimal import Decimal, localcontext
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys

import numpy as np


ROOT = Path("C:/atx/.worktrees/equity-platform")
AUDIT = ROOT / "build-equity/audits"
ATTEMPT = Path("C:/atx/data/equity_book_training_2013_intents_20260920")
OUTPUT = AUDIT / "iteration11-second-decision-oracle.json"
RECEIPT = AUDIT / "iteration10-allocation-proposal-verification.json"
CHECKER = AUDIT / "iteration10_verify_allocation_proposals.py"
DECODER = Path("C:/atx/build-equity/audits/iteration6-baseline-verify.py")
CONTEXT = Path("C:/atx/data/tickerhistory_training_native_20260919/context.bin")
BASELINE = Path("C:/atx/data/equity_baseline_training_2013_20260919")
RECEIPT_SHA = "e5eaf851f4d94309d37246d6a567040dfd913bb3eaf9c33c03776e1b5cf49a7d"
CHECKER_SHA = "5EEEAEFFB932A88A2A65FB6F1C46F4B62AB994DE0059D983581FDA4B1D6AB6DF".lower()
DECODER_SHA = "9a2052f35d1da6235b0fd0f11ff1222fe7f213a5a64f608cbaffbb9393064ee3"
DECISION, EXECUTION, STEPS = 5, 6, 180
PRECISION = 70
KKT_TOL = Decimal("1e-14")
ZERO, ONE = Decimal(0), Decimal(1)
PINS = {}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path, expected=None):
    path = Path(path).resolve()
    data = path.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    require(expected is None or sha == expected, f"source snapshot changed: {path}")
    PINS[str(path)] = sha
    return data


def load_module(path, expected, name):
    captured = read(path, expected)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    exec(compile(captured, str(path), "exec"), module.__dict__)
    return module


def dec(value):
    value = float(value)
    require(math.isfinite(value), "nonfinite numeric input")
    return Decimal.from_float(value)


def risk_at_decision(context, row):
    start = row - 63
    require(start >= 0, "insufficient original decision history")
    close, raw, volume = (context.columns[k][start:row + 1] for k in ("close", "raw_close", "volume"))
    require(close.shape == (64, context.n), "risk window shape mismatch")
    observed = (np.isfinite(close) & (close > 0) & np.isfinite(raw) & (raw > 0)
                & np.isfinite(volume) & (volume >= 0))
    with np.errstate(all="ignore"):
        binary_returns = close[1:] / close[:-1] - 1.0
    counts = (observed[1:] & observed[:-1] & np.isfinite(binary_returns)).sum(axis=0)
    variance = [Decimal("0.000001")] * context.n
    for i in np.flatnonzero(counts == 63):
        returns = [dec(close[t, i]) / dec(close[t - 1, i]) - ONE for t in range(1, 64)]
        mean = sum(returns, ZERO) / 63
        variance[i] = max(Decimal("0.000001"), sum(((x - mean) ** 2 for x in returns), ZERO) / 63)
    source = hashlib.sha256(b"iteration11-independent-second-risk-window-v1\n")
    source.update(f"{start}:{row}".encode())
    for field in (close, raw, volume):
        source.update(field.astype("<f8", copy=False).tobytes(order="C"))
    return counts, variance, {"first_context_row": start, "decision_context_row": row,
        "first_session_key_ns": str(int(context.keys[start])),
        "last_session_key_ns": str(int(context.keys[row])),
        "raw_window_sha256": source.hexdigest(), "risk_ready_count": int((counts == 63).sum()),
        "return_count": 63, "precision": PRECISION, "gap_bridging": False}


def coordinates(a, hessian, previous, cap, nu, tau):
    shifted = a - nu - hessian * previous
    return np.clip(previous + np.sign(shifted) * np.maximum(np.abs(shifted) - tau, 0.0)
                   / hessian, -cap, cap)


def net_solve(a, hessian, previous, cap, tau):
    low = float(np.min(a - hessian * cap)) - tau - 1.0
    high = float(np.max(a + hessian * cap)) + tau + 1.0
    best = None
    for step in range(STEPS):
        nu = low + (high - low) / 2.0
        weight = coordinates(a, hessian, previous, cap, nu, tau)
        net = math.fsum(map(float, weight))
        candidate = (abs(net), nu, weight)
        if best is None or candidate[0] < best[0]:
            best = candidate
        if net == 0 or nu == low or nu == high:
            break
        if net > 0:
            low = nu
        else:
            high = nu
    require(best is not None and best[0] <= 1e-14, "net multiplier search failed")
    return best[2], best[1], step + 1


def solve(a, hessian, previous, cap, remaining_turnover):
    require(len(a) > 0 and remaining_turnover > 0, "empty/free turnover infeasible assumption")
    weight, nu, calls = net_solve(a, hessian, previous, cap, 0.0)
    unconstrained_turnover = math.fsum(abs(float(x)) for x in weight - previous)
    if unconstrained_turnover <= remaining_turnover:
        return weight, nu, 0.0, {"turnover_active": False, "net_iterations": calls,
                               "unconstrained_turnover": unconstrained_turnover}
    low, high = 0.0, max(1.0, 2.0 * float(np.max(np.abs(a))))
    high_weight, high_nu, used = net_solve(a, hessian, previous, cap, high)
    calls += used
    require(math.fsum(abs(float(x)) for x in high_weight - previous) <= remaining_turnover,
            "could not bracket feasible shifted-turnover multiplier")
    outer = 0
    for _ in range(STEPS):
        tau = low + (high - low) / 2.0
        if tau == low or tau == high:
            break
        weight, nu, used = net_solve(a, hessian, previous, cap, tau)
        calls += used
        outer += 1
        turnover = math.fsum(abs(float(x)) for x in weight - previous)
        if turnover > remaining_turnover:
            low = tau
        else:
            high, high_weight, high_nu = tau, weight, nu
    return high_weight, high_nu, high, {"turnover_active": True, "net_iterations": calls,
        "outer_iterations": outer, "tau_bracket": [low, high],
        "unconstrained_turnover": unconstrained_turnover,
        "return_side": "feasible side of scalar turnover bracket; no renormalization"}


def certify(preference, variance, previous, free, weights, nu, tau):
    cap = Decimal("0.01") * (ONE - Decimal("0.0005") * Decimal("0.2"))
    gross_cap, turnover_cap = Decimal("0.9999"), Decimal("0.2")
    w, h, a = list(map(dec, weights)), list(map(dec, previous)), list(map(dec, preference))
    ndec, tdec = dec(nu), dec(tau)
    net, gross = sum(w, ZERO), sum(map(abs, w), ZERO)
    turnover = sum((abs(x - y) for x, y in zip(w, h)), ZERO)
    maximum = max(map(abs, w), default=ZERO)
    # This diagnostic omits the gross dual only after establishing strict slack.
    require(gross_cap - gross > Decimal("1e-10"), "gross constraint cannot be certified inactive")
    require(cap - maximum > Decimal("1e-10"), "name cap cannot be certified inactive")
    stationarity, dual_infimum, objective = ZERO, ZERO, ZERO
    changes, holds = 0, 0
    for i in range(len(w)):
        curvature = ONE + 2 * variance[i]
        objective += Decimal("0.5") * (w[i] - a[i]) ** 2 + variance[i] * w[i] ** 2
        if not free[i]:
            require(w[i] == 0, "nonzero excluded coordinate")
            dual_infimum += Decimal("0.5") * a[i] ** 2 + tdec * abs(h[i])
            continue
        gradient = curvature * w[i] - a[i] + ndec
        change = w[i] - h[i]
        if change != 0:
            subgradient = ONE if change > 0 else -ONE
            changes += 1
        else:
            subgradient = max(-ONE, min(ONE, -gradient / tdec)) if tdec else ZERO
            holds += 1
        stationarity = max(stationarity, abs(gradient + tdec * subgradient))
        shifted = a[i] - ndec - curvature * h[i]
        residual = max(ZERO, abs(shifted) - tdec)
        minimizer = h[i] + (ONE if shifted >= 0 else -ONE) * residual / curvature
        minimizer = max(-cap, min(cap, minimizer))
        dual_infimum += (Decimal("0.5") * curvature * minimizer ** 2 - a[i] * minimizer
                         + Decimal("0.5") * a[i] ** 2 + ndec * minimizer
                         + tdec * abs(minimizer - h[i]))
    dual = dual_infimum - tdec * turnover_cap
    primal = max(abs(net), max(ZERO, turnover - turnover_cap),
                 max(ZERO, maximum - cap), max(ZERO, gross - gross_cap))
    complementarity = abs(tdec * (turnover - turnover_cap))
    gap = objective - dual
    residual = max(primal, stationarity, complementarity, abs(gap))
    require(tdec >= 0 and residual <= KKT_TOL, f"independent KKT certificate failed: {residual}")
    postfee_ratio = ONE - Decimal("0.0005") * turnover
    require(postfee_ratio > 0 and abs(net) / postfee_ratio <= KKT_TOL and
            gross / postfee_ratio <= ONE + KKT_TOL and
            maximum / postfee_ratio <= Decimal("0.01") + KKT_TOL, "postfee continuous exposure failed")
    return {"max_kkt_residual": str(residual), "primal_residual": str(primal),
        "stationarity_residual": str(stationarity), "complementarity_residual": str(complementarity),
        "primal_objective": str(objective), "dual_objective": str(dual), "primal_dual_gap": str(gap),
        "net": str(net), "gross": str(gross), "turnover_full_l1": str(turnover),
        "gross_slack": str(gross_cap - gross), "minimum_name_slack": str(cap - maximum),
        "postfee_nav_ratio": str(postfee_ratio), "postfee_gross": str(gross / postfee_ratio),
        "postfee_max_name": str(maximum / postfee_ratio), "postfee_net": str(net / postfee_ratio),
        "free_coordinate_changes": changes, "free_coordinate_exact_holds": holds,
        "tolerance": str(KKT_TOL), "precision": PRECISION,
        "scope": "continuous marked-dollar mathematical candidate; no unavailable entry prices fabricated"}


def run():
    require(not sys.flags.optimize, "run without -O; archived reader requires assertions")
    read(Path(__file__))
    receipt = json.loads(read(RECEIPT, RECEIPT_SHA))
    require(receipt["verified_proposal_count"] == 1 and receipt["observed_complete_intervals"] == 6,
            "iteration 10 verified frontier changed")
    reader = load_module(DECODER, DECODER_SHA, "iteration11_panel_reader")
    checker = load_module(CHECKER, CHECKER_SHA, "iteration11_carry_helper")
    context = reader.Panel(CONTEXT)
    evaluation = reader.Panel(BASELINE / "evaluation.bin")
    books = reader.Panel(BASELINE / "books.bin")
    PINS.update(reader.INPUT_HASHES)
    for name, sha in reader.INPUT_HASHES.items():
        require(receipt["input_hashes"][name] == sha, "original panel snapshot changed")
    for panel in (evaluation, books):
        reader.same_instrument_axes(context, panel)
    certificate_path = ATTEMPT / "allocation_certificates.json"
    proposals = json.loads(read(certificate_path, receipt["input_hashes"][str(certificate_path.resolve())]))["decisions"]
    require(len(proposals) == 1, "unpublished second native proposal must remain absent")
    first = int(np.searchsorted(context.keys, evaluation.keys[0]))
    require(np.array_equal(context.keys[first:first + evaluation.d], evaluation.keys) and
            np.array_equal(books.keys, evaluation.keys[::5]), "original calendar/schedule changed")
    units, cash, financing = [0.0] * evaluation.n, Decimal(100000000), []
    for period in range(EXECUTION):
        values = checker.mark(units, evaluation, period)
        if period == 1:
            units, cash, values, _ = checker.check_proposal(
                proposals[0], context, evaluation, books, first, units, cash, values)
        elapsed = Decimal(int(evaluation.keys[period + 1]) - int(evaluation.keys[period])) / checker.DAY_NS
        shorts = sum((-value for value in values if value < 0), ZERO)
        charge = shorts * Decimal("0.0365") * elapsed / 365
        checker.mark(units, evaluation, period + 1)
        cash -= charge
        financing.append(str(charge))
    values = checker.mark(units, evaluation, EXECUTION)
    nav = cash + sum(values, ZERO)
    require(nav > 0, "nonpositive carried NAV")
    previous = np.array([float(value / nav) for value in values])
    row = first + DECISION
    counts, variance, risk_evidence = risk_at_decision(context, row)
    eligibility = evaluation.mask[DECISION].astype(bool)
    free = eligibility & (counts == 63)
    require(all(not units[i] or not eligibility[i] or counts[i] == 63 for i in range(evaluation.n)),
            "held eligible instrument is risk-unready")
    preference = books.columns["weight"][1].copy()
    require(np.isfinite(preference).all() and np.all(preference[~eligibility] == 0), "invalid original preferences")
    fixed_turnover = math.fsum(abs(float(previous[i])) for i in range(evaluation.n) if not free[i])
    active = np.flatnonzero(free)
    hessian = np.array([float(ONE + 2 * variance[i]) for i in active])
    chosen, nu, tau, search = solve(preference[active], hessian, previous[active],
                                    0.01 * (1.0 - 0.0005 * 0.2), 0.2 - fixed_turnover)
    weights = np.zeros(evaluation.n)
    weights[active] = chosen
    certificate = certify(preference, variance, previous, free, weights, nu, tau)
    unavailable = []
    for i in np.flatnonzero(weights != 0):
        price = float(evaluation.columns["close"][EXECUTION, i])
        if not math.isfinite(price) or price <= 0:
            unavailable.append({"instrument": int(i), "vendor_security_id": evaluation.axes["instrument_ids"][i],
                                "independent_weight": float(weights[i]), "previous_weight": float(previous[i])})
    gnw = 604
    require(evaluation.axes["instrument_ids"][gnw] == "150340", "GNW archive-axis annotation mismatch")
    for name, sha in PINS.items():
        require(hashlib.sha256(Path(name).read_bytes()).hexdigest() == sha, f"input changed during oracle: {name}")
    return {"schema": "atx-iteration11-second-decision-kkt-oracle-v1", "status": "certified-diagnostic-optimum",
        "scope": "independent continuous mathematical optimum from reconstructed carry; no unpublished native comparison",
        "native_second_proposal_available": False, "native_failure_reproduced": False,
        "completed_performance_result": False, "required_entry_marks_all_available": len(unavailable) == 0,
        "actual_second_execution_sized": False, "source_correctness_attested": False,
        "decision_period": DECISION, "execution_period": EXECUTION,
        "decision_session_key_ns": str(int(evaluation.keys[DECISION])),
        "execution_session_key_ns": str(int(evaluation.keys[EXECUTION])),
        "independent_pretrade_cash": str(cash), "independent_pretrade_nav": str(nav),
        "previous_weight_basis": "binary64-rounded ratios of Decimal reconstructed marked dollars and NAV; not an unpublished native NAV",
        "borrow_charges_through_execution": financing, "risk": risk_evidence,
        "free_count": int(free.sum()), "fixed_held_count": sum(bool(units[i]) and not free[i] for i in range(evaluation.n)),
        "mandatory_exit_turnover": fixed_turnover,
        "dual_net_nu": nu, "dual_turnover_tau": tau, "search": search, "certificate": certificate,
        "gnw_diagnostic": {"instrument": gnw, "vendor_security_id": "150340", "archive_ticker_annotation": "GNW",
            "original_decision_eligible": bool(eligibility[gnw]), "valid_return_count": int(counts[gnw]),
            "original_preference": float(preference[gnw]), "previous_weight": float(previous[gnw]),
            "independent_optimum_weight": float(weights[gnw]),
            "interpretation": "mathematical optimization result, not the unpublished native candidate"},
        "missing_entry_mark_diagnostics": unavailable,
        "mathematical_weights_canonical_order": list(map(float, weights)),
        "previous_weights_canonical_order": list(map(float, previous)), "input_hashes": dict(sorted(PINS.items()))}


def main():
    require(not OUTPUT.exists(), f"refusing to overwrite oracle: {OUTPUT}")
    code = 0
    try:
        with localcontext() as context:
            context.prec = PRECISION
            result = run()
    except Exception as error:
        result = {"schema": "atx-iteration11-second-decision-kkt-oracle-v1", "status": "not-certified",
                  "error": repr(error), "completed_performance_result": False,
                  "native_failure_reproduced": False, "input_hashes": dict(sorted(PINS.items()))}
        code = 1
    with OUTPUT.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k not in
                     {"input_hashes", "mathematical_weights_canonical_order", "previous_weights_canonical_order"}}, indent=2))
    print("oracle_sha256", hashlib.sha256(OUTPUT.read_bytes()).hexdigest())
    return code


if __name__ == "__main__":
    raise SystemExit(main())
