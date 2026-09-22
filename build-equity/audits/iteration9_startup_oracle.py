"""Analytic separable KKT startup oracle, with numerical dual bisection.

Independent of engine/native optimization code. This solves only the first
all-cash allocation; it is not a holdings-aware general optimizer or a P&L trial.
The coordinate formula is the weighted quadratic proximal map for an L1 penalty
with an interval constraint. Net and turnover duals are found by bisection, then
checked against 70-digit Decimal risk moments and KKT conditions.
"""
from __future__ import annotations

from datetime import datetime, timezone
from decimal import Decimal, localcontext
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys
import time

import numpy as np


ROOT = Path("C:/atx/.worktrees/equity-platform")
SHARED = Path("C:/atx")
OUTPUT = ROOT / "build-equity/audits/iteration9-startup-oracle.json"
READER = SHARED / "build-equity/audits/iteration6-baseline-verify.py"
CONTEXT = SHARED / "data/tickerhistory_training_native_20260919/context.bin"
BASELINE = SHARED / "data/equity_baseline_training_2013_20260919"
REQUEST = SHARED / "data/equity_book_training_2013_20260920/request.json"
EXPECTED_IDS = {
    "context": "ec572b826dce65fbd4cd391921f57f01e1ce084864ec84b3e6a944003ead8079",
    "evaluation": "57b7c21c9320e98c1dee6c97e5f3b8cde211c5efdd688c2daf6fb28daa3cd475",
    "preferences": "615019e7c83c34adc950bf41bda2e28923960249fbef0fccd8b84e14c251eb88",
}
DECISION_ROW = 256
FIRST_RISK_ROW = 193
RETURN_COUNT = 63
DECIMAL_PRECISION = 70
MAX_BISECTION_STEPS = 160
KKT_TOLERANCE = Decimal("1e-14")
MOMENT_TOLERANCE = Decimal("1e-14")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def import_reader():
    source = READER.read_bytes()
    spec = importlib.util.spec_from_file_location("iteration9_archived_panel_reader", READER)
    require(spec is not None, "cannot import archived panel reader")
    module = importlib.util.module_from_spec(spec)
    exec(compile(source, str(READER), "exec"), module.__dict__)
    return module, hashlib.sha256(source).hexdigest()


def decimal(value):
    return Decimal.from_float(float(value))


def freeze_risk(context):
    close = context.columns["close"][FIRST_RISK_ROW:DECISION_ROW + 1]
    raw = context.columns["raw_close"][FIRST_RISK_ROW:DECISION_ROW + 1]
    volume = context.columns["volume"][FIRST_RISK_ROW:DECISION_ROW + 1]
    require(close.shape == (64, context.n), "risk window shape mismatch")
    # Matches the frozen profile's observation meaning. Membership does not mask
    # history; an unobserved pair is counted explicitly and never bridged.
    observed = (np.isfinite(close) & (close > 0) & np.isfinite(raw) & (raw > 0)
                & np.isfinite(volume) & (volume >= 0))
    counts = np.zeros(context.n, dtype=np.int64)
    variance = np.full(context.n, 1e-6)
    exact_variance = [Decimal("0.000001")] * context.n
    variance_error = Decimal(0)
    nonfinite_returns = 0
    observed_pairs = 0
    floored = 0
    with localcontext() as ctx:
        ctx.prec = DECIMAL_PRECISION
        for i in range(context.n):
            usable = []
            for j in range(1, 64):
                if not (observed[j - 1, i] and observed[j, i]):
                    continue
                observed_pairs += 1
                value = float(close[j, i]) / float(close[j - 1, i]) - 1.0
                if not math.isfinite(value):
                    nonfinite_returns += 1
                    continue
                usable.append(value)
            counts[i] = len(usable)
            if len(usable) != RETURN_COUNT:
                continue
            mean = math.fsum(usable) / RETURN_COUNT
            estimate = math.fsum((value - mean) ** 2 for value in usable) / RETURN_COUNT
            require(math.isfinite(estimate) and estimate >= 0, "nonfinite risk moment")
            variance[i] = max(1e-6, estimate)
            exact_returns = [decimal(close[j, i]) / decimal(close[j - 1, i]) - 1
                             for j in range(1, 64)]
            exact_mean = sum(exact_returns) / RETURN_COUNT
            exact = sum((value - exact_mean) ** 2 for value in exact_returns) / RETURN_COUNT
            exact_variance[i] = max(Decimal("0.000001"), exact)
            variance_error = max(variance_error, abs(decimal(variance[i]) - exact_variance[i]))
            floored += estimate < 1e-6
    require(variance_error <= MOMENT_TOLERANCE, "independent risk moment check failed")
    return counts, variance, exact_variance, {
        "risk_ready_count_all_names": int(np.count_nonzero(counts == RETURN_COUNT)),
        "risk_unready_count_all_names": int(np.count_nonzero(counts != RETURN_COUNT)),
        "observed_pairs": observed_pairs, "nonfinite_computed_returns": nonfinite_returns,
        "variance_floor_applied_ready_count": floored,
        "max_abs_float_vs_decimal_variance_error": str(variance_error),
        "independent_moment_tolerance": str(MOMENT_TOLERANCE),
        "unready_variance": "floor placeholder only; entry prohibited",
    }


def coordinates(preference, hessian, cap, nu, tau):
    difference = preference - nu
    return np.clip(np.sign(difference) * np.maximum(np.abs(difference) - tau, 0.0)
                   / hessian, -cap, cap)


def solve_net(preference, hessian, cap, tau):
    low = float(np.min(preference)) - tau
    high = float(np.max(preference)) + tau
    evaluations = 0
    best = None
    for _ in range(MAX_BISECTION_STEPS):
        nu = low + (high - low) / 2.0
        weights = coordinates(preference, hessian, cap, nu, tau)
        net = math.fsum(map(float, weights))
        evaluations += 1
        candidate = (abs(net), nu, weights, net)
        if best is None or candidate[0] < best[0]:
            best = candidate
        if net == 0.0 or nu == low or nu == high:
            break
        if net > 0:
            low = nu
        else:
            high = nu
    require(best is not None and best[0] <= 1e-14, "net dual bisection failed")
    return best[1], best[2], evaluations, [low, high]


def solve_startup(preference, hessian, cap, turnover):
    require(len(preference) > 0 and np.isfinite(preference).all(), "invalid preference union")
    require(np.isfinite(hessian).all() and (hessian > 0).all(), "invalid diagonal Hessian")
    nu, weights, evaluations, net_bracket = solve_net(preference, hessian, cap, 0.0)
    unconstrained_gross = math.fsum(abs(float(value)) for value in weights)
    if unconstrained_gross <= turnover:
        return weights, nu, 0.0, {"turnover_active": False, "net_evaluations": evaluations,
            "outer_steps": 0, "tau_bracket": [0.0, 0.0], "nu_bracket": net_bracket,
            "net_and_box_only_gross": unconstrained_gross}
    low = 0.0
    high = 2.0 * float(np.max(np.abs(preference))) + 1.0
    high_nu, high_weights, calls, high_bracket = solve_net(preference, hessian, cap, high)
    evaluations += calls
    require(math.fsum(abs(float(value)) for value in high_weights) <= turnover,
            "turnover multiplier could not be bracketed")
    steps = 0
    for _ in range(MAX_BISECTION_STEPS):
        tau = low + (high - low) / 2.0
        if tau == low or tau == high:
            break
        nu, weights, calls, net_bracket = solve_net(preference, hessian, cap, tau)
        evaluations += calls
        steps += 1
        gross = math.fsum(abs(float(value)) for value in weights)
        if gross > turnover:
            low = tau
        else:
            high, high_nu, high_weights, high_bracket = tau, nu, weights, net_bracket
    # Return the feasible side of the scalar bracket. Do not renormalize, clip
    # after solving, or spend an unrecorded tolerance budget to hit turnover.
    return high_weights, high_nu, high, {"turnover_active": True,
        "net_evaluations": evaluations, "outer_steps": steps,
        "tau_bracket": [low, high], "nu_bracket": high_bracket,
        "net_and_box_only_gross": unconstrained_gross}


def certify(preference, variance, union, full_weights, nu, tau):
    with localcontext() as ctx:
        ctx.prec = DECIMAL_PRECISION
        zero, one = Decimal(0), Decimal(1)
        ndec, tdec = decimal(nu), decimal(tau)
        turnover, fee_rate = Decimal("0.2"), Decimal("0.0005")
        cap = Decimal("0.01") * (one - fee_rate * turnover)
        full = [decimal(value) for value in full_weights]
        net, gross = sum(full), sum(abs(value) for value in full)
        stationarity = complementarity = box_violation = zero
        dual_values = []
        dual_infimum = zero
        positive = negative = at_zero = cap_bound = 0
        union_set = set(map(int, union))
        objective = sum(Decimal("0.5") * (value - decimal(a)) ** 2 + risk * value ** 2
                        for value, a, risk in zip(full, preference, variance))
        for i in range(len(full)):
            if i not in union_set:
                require(full[i] == zero, "oracle opened an excluded coordinate")
                dual_infimum += Decimal("0.5") * decimal(preference[i]) ** 2
                continue
            value, a, h = full[i], decimal(preference[i]), one + 2 * variance[i]
            gradient = h * value - a + ndec
            upper = lower = zero
            if value > zero:
                subgradient = one
                positive += 1
            elif value < zero:
                subgradient = -one
                negative += 1
            else:
                at_zero += 1
                subgradient = max(-one, min(one, -gradient / tdec)) if tdec else zero
            reduced = gradient + tdec * subgradient
            if value > zero and abs(value - cap) <= Decimal("1e-16"):
                upper = max(zero, -reduced)
                cap_bound += 1
            elif value < zero and abs(value + cap) <= Decimal("1e-16"):
                lower = max(zero, reduced)
                cap_bound += 1
            stationarity = max(stationarity, abs(reduced + upper - lower))
            complementarity = max(complementarity, abs(upper * (value - cap)),
                                  abs(lower * (-value - cap)))
            box_violation = max(box_violation, max(zero, abs(value) - cap))
            shifted = a - ndec
            soft = max(zero, abs(shifted) - tdec)
            minimizer = min(cap, soft / h) * (one if shifted >= zero else -one)
            dual_infimum += (Decimal("0.5") * h * minimizer ** 2 - a * minimizer
                             + Decimal("0.5") * a ** 2 + ndec * minimizer
                             + tdec * abs(minimizer))
            dual_values.append({"instrument_index": i, "l1_subgradient": str(subgradient),
                                "upper_box_multiplier": str(upper),
                                "lower_box_multiplier": str(lower)})
        primal_violation = max(abs(net), max(zero, gross - turnover), box_violation)
        complementarity = max(complementarity, abs(tdec * (gross - turnover)))
        dual_objective = dual_infimum - tdec * turnover
        gap = objective - dual_objective
        residual = max(primal_violation, stationarity, complementarity, abs(gap))
        require(tdec >= 0 and residual <= KKT_TOLERANCE, "high-precision KKT check failed")
        postfee_nav_ratio = one - fee_rate * gross
        postfee_gross = gross / postfee_nav_ratio
        postfee_max_name = max(map(abs, full)) / postfee_nav_ratio
        require(postfee_nav_ratio > 0 and postfee_gross <= 1 + KKT_TOLERANCE and
                postfee_max_name <= Decimal("0.01") + KKT_TOLERANCE,
                "fee-adjusted exposure certification failed")
        return {"decimal_precision": DECIMAL_PRECISION,
            "arithmetic_basis": "exact binary64 source prices/preferences, Decimal risk recomputation",
            "max_kkt_residual": str(residual), "stationarity_linf": str(stationarity),
            "primal_feasibility_linf": str(primal_violation),
            "complementarity_linf": str(complementarity), "box_violation": str(box_violation),
            "primal_objective_full_canonical": str(objective),
            "dual_objective_full_canonical": str(dual_objective), "primal_dual_gap": str(gap),
            "net": str(net), "gross_equals_startup_turnover": str(gross),
            "postfee_nav_ratio": str(postfee_nav_ratio), "postfee_gross": str(postfee_gross),
            "postfee_max_name": str(postfee_max_name), "positive_names": positive,
            "negative_names": negative, "zero_union_names": at_zero, "box_bound_names": cap_bound,
            "tolerance": str(KKT_TOLERANCE), "per_coordinate_dual_values": dual_values}


def run():
    require(not OUTPUT.exists(), f"refusing to overwrite oracle: {OUTPUT}")
    require(sys.flags.optimize == 0, "archived reader requires enabled Python assertions")
    started = time.perf_counter()
    reader, reader_sha = import_reader()
    context = reader.Panel(CONTEXT)
    evaluation = reader.Panel(BASELINE / "evaluation.bin")
    books = reader.Panel(BASELINE / "books.bin")
    request = reader.load_json(REQUEST)
    for label, panel in (("context", context), ("evaluation", evaluation), ("preferences", books)):
        require(panel.manifest["artifact_id"] == EXPECTED_IDS[label], f"changed frozen {label}")
        reader.same_instrument_axes(context, panel)
    require(np.array_equal(evaluation.keys, context.keys[256:]) and
            np.array_equal(books.keys, evaluation.keys[::5]), "original axes/schedule changed")
    require(evaluation.d == 189 and evaluation.n == 1661 and books.d == 38,
            "unexpected original evaluation dimensions")
    profile = request["recipe"]
    for key, expected in (("risk_return_count", 63), ("risk_penalty", 1.0),
                          ("variance_floor", 1e-6), ("full_l1_turnover_limit", 0.2),
                          ("name_limit", 0.01), ("gross_limit", 1.0),
                          ("trade_bps_per_absolute_dollar", 5.0), ("initial_nav", 100_000_000.0),
                          ("execution_delay_observations", "1")):
        require(profile[key] == expected, f"startup profile differs: {key}")
    counts, variance, exact_variance, risk_stats = freeze_risk(context)
    preference = books.columns["weight"][0].copy()
    eligibility = evaluation.mask[0].astype(bool)
    require(np.isfinite(preference).all() and np.all(preference[~eligibility] == 0),
            "invalid/ineligible frozen preference")
    union = np.flatnonzero(eligibility & (counts == RETURN_COUNT))
    require(len(union) > 0, "empty startup solver union")
    hessian = 1.0 + 2.0 * variance[union]
    fee_reserve = 1.0 - (5.0 * 1e-4) * 0.2
    chosen, nu, tau, bisection = solve_startup(preference[union], hessian,
                                              0.01 * fee_reserve, 0.2)
    weights = np.zeros(context.n)
    weights[union] = chosen
    certificate = certify(preference, exact_variance, union, weights, nu, tau)
    # Execution marks are used only after optimization to check that this first
    # proposal can be represented as TRI units. No future interval or P&L is read.
    marks = evaluation.columns["close"][1]
    for i in np.flatnonzero(weights):
        require(math.isfinite(float(marks[i])) and marks[i] > 0,
                f"nonzero startup allocation lacks execution mark: instrument={i}")
    dollars = [0.0 if weight == 0 else ((float(weight) * 100_000_000.0) / float(marks[i]))
               * float(marks[i]) for i, weight in enumerate(weights)]
    require(all(math.isfinite(value) for value in dollars), "unrepresentable execution dollars")
    traded = math.fsum(abs(value) for value in dollars)
    cost = traded * (5.0 * 1e-4)
    post_nav = 100_000_000.0 - cost
    pins = dict(reader.INPUT_HASHES)
    pins[str(READER)] = reader_sha
    pins[str(Path(__file__).resolve())] = sha_file(__file__)
    require(all(sha_file(name) == expected for name, expected in pins.items()),
            "oracle input/reader/source changed during run")
    result = {"schema": "atx.iteration9-startup-oracle-v1", "status": "certified-startup-only",
        "method": "analytic-separable-KKT-with-numerical-dual-bisection",
        "reference": "https://web.stanford.edu/~boyd/papers/pdf/prox_algs.pdf",
        "reference_scope": "Parikh/Boyd separable proximal maps and L1 soft thresholding; constrained derivation here",
        "derivation": "H=1+2*variance; w=clip(soft(a-nu,tau)/H,-cap*r,cap*r); sum(w)=0; tau>=0; tau*(sum(abs(w))-.2)=0",
        "strict_convexity": "H_i>=1 makes the primal solution unique; dual roots can have flat intervals",
        "scope": "original first training decision, all cash; no engine/native calls, no later holdings optimizer, no PnL",
        "input_hashes": pins, "panel_artifact_ids": EXPECTED_IDS,
        "decision_period": 0, "execution_period": 1, "decision_context_row": DECISION_ROW,
        "first_risk_context_row": FIRST_RISK_ROW,
        "decision_session_key_ns": str(int(evaluation.keys[0])),
        "execution_session_key_ns": str(int(evaluation.keys[1])),
        "risk_session_keys_ns": [str(int(value)) for value in context.keys[193:257]],
        "instrument_namespace": context.axes["instrument_namespace"],
        "instrument_ids": context.axes["instrument_ids"],
        "original_instrument_indices": context.axes["original_instrument_indices"],
        "canonical_instruments": context.n, "union_instruments": len(union),
        "union_indices": union.tolist(), "decision_eligible_count": int(eligibility.sum()),
        "eligible_but_risk_unready": int(np.count_nonzero(eligibility & (counts != RETURN_COUNT))),
        "risk": risk_stats, "valid_return_count": counts.tolist(), "variance": variance.tolist(),
        "preference_weights": preference.tolist(), "weights": weights.tolist(),
        "configuration": {"risk_penalty": 1.0, "variance_floor": 1e-6,
            "initial_nav": 100_000_000.0, "fee_bps": 5.0, "fee_reserve": fee_reserve,
            "name_limit_postfee": 0.01, "name_limit_prefee": 0.01 * fee_reserve,
            "turnover_full_l1": 0.2, "gross_limit_postfee": 1.0,
            "gross_constraint": "redundant at all-cash startup: turnover .2 < prefee gross limit .9999"},
        "dual_variables": {"net_nu": nu, "turnover_tau": tau,
            "gross_multiplier": 0.0, "box_and_l1": "see Decimal certificate per-coordinate values"},
        "bisection": {**bisection, "max_steps_each": MAX_BISECTION_STEPS,
            "rounding_termination": "adjacent binary64 bracket or exact net root; feasible tau endpoint"},
        "certificate": certificate,
        "representable_execution": {"absolute_trade_dollars": traded, "trade_cost_dollars": cost,
            "posttrade_nav": post_nav, "turnover": traded / 100_000_000.0,
            "postfee_net": math.fsum(dollars) / post_nav,
            "postfee_gross": math.fsum(abs(value) for value in dollars) / post_nav,
            "postfee_max_name": max(map(abs, dollars)) / post_nav,
            "summation": "math.fsum; independent of native ordered accumulation"},
        "input_pins_unchanged": True, "finished_utc": datetime.now(timezone.utc).isoformat(),
        "wall_seconds": time.perf_counter() - started,
        "qualification": "numerical startup benchmark only; source economics/investment validity unverified"}
    with OUTPUT.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": result["status"], "union_instruments": len(union),
        "positive_names": certificate["positive_names"], "negative_names": certificate["negative_names"],
        "gross": certificate["gross_equals_startup_turnover"],
        "objective": certificate["primal_objective_full_canonical"],
        "max_kkt_residual": certificate["max_kkt_residual"], "nu": nu, "tau": tau,
        "output": str(OUTPUT), "wall_seconds": result["wall_seconds"]}), flush=True)


if __name__ == "__main__":
    run()
