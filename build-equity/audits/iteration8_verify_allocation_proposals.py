"""Independent, partial software oracle for the frozen 2013 equity-book attempt.

Run only after the native attempt stops. Imports the existing APNL decoder, never
engine code. A failed replay has no performance result, even if every published
proposal passes. Missing prices are never filled and unrecorded decisions are
never guessed. Decimal accounting uses the exact binary64 inputs at precision 50;
unit sizing and price products explicitly retain binary64 representability.
"""
import argparse
from decimal import Decimal, localcontext
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys

import numpy as np


WORKTREE = Path("C:/atx/.worktrees/equity-platform")
DECODER = Path("C:/atx/build-equity/audits/iteration6-baseline-verify.py")
CONTEXT = Path("C:/atx/data/tickerhistory_training_native_20260919/context.bin")
BASELINE = Path("C:/atx/data/equity_baseline_training_2013_20260919")
ATTEMPT = Path("C:/atx/data/equity_book_training_2013_20260920")
HASHES = {}
ZERO, ONE = Decimal(0), Decimal(1)
FEE_RATE, BORROW_RATE = Decimal("0.0005"), Decimal("0.0365")
ECONOMIC_TOL = Decimal("1e-8")
DAY_NS = 86_400_000_000_000
MONEY_FIELDS = {"pretrade_nav", "pretrade_cash", "traded_dollars", "trade_cost",
                "posttrade_cash", "posttrade_nav"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read(path):
    path = Path(path).resolve()
    data = path.read_bytes()
    HASHES[str(path)] = digest(data)
    return data


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def invalid_constant(value):
    raise ValueError(f"nonfinite JSON constant: {value}")


def load_json(path):
    require(Path(path).stat().st_size <= 16 * 1024 * 1024, f"oversized metadata: {path}")
    return json.loads(read(path), object_pairs_hook=no_duplicates,
                      parse_constant=invalid_constant)


def decimal(value):
    require(isinstance(value, (int, float)) and not isinstance(value, bool),
            f"expected numeric value: {value!r}")
    value = float(value)
    require(math.isfinite(value), "nonfinite numeric input")
    return Decimal.from_float(value)


def integer(value, label):
    require(isinstance(value, int) and not isinstance(value, bool) and value >= 0,
            f"invalid {label}")
    return value


def compare(actual, expected, label, n, nav):
    actual = decimal(actual)
    # Independent accumulation can differ from canonical-order native binary64
    # sums. This roundoff allowance is separate from the fixed economic limits.
    roundoff = Decimal(128) * Decimal.from_float(sys.float_info.epsilon) * max(1, n)
    scale = max(ONE, abs(expected), abs(nav)) if label in MONEY_FIELDS else max(ONE, abs(expected))
    tolerance = max(Decimal("2e-12"), roundoff * scale)
    error = abs(actual - expected)
    require(error <= tolerance, f"{label}: certificate error {error} exceeds {tolerance}")
    return {"absolute_error": str(error), "roundoff_tolerance": str(tolerance)}


class MissingHeldMark(Exception):
    def __init__(self, period, instrument, evaluation):
        self.evidence = {
            "period": period, "instrument": instrument,
            "session_key_ns": str(int(evaluation.keys[period])),
            "security_id": evaluation.axes["instrument_ids"][instrument],
        }
        super().__init__(str(self.evidence))


def mark(units, evaluation, period):
    values = []
    for i, quantity in enumerate(units):
        if quantity == 0:
            values.append(ZERO)
            continue
        price = float(evaluation.columns["close"][period, i])
        if not math.isfinite(price) or price <= 0:
            raise MissingHeldMark(period, i, evaluation)
        represented = quantity * price
        require(math.isfinite(represented) and represented != 0,
                f"held value overflow/underflow at {period}/{i}")
        values.append(decimal(represented))
    return values


def risk_inputs(context, evaluation, certificate, context_first, units):
    decision = certificate["decision_period"]
    row = context_first + decision
    first = row - 63
    require(first >= 0 and certificate["first_context_row"] == first and
            certificate["decision_context_row"] == row, "risk window is not original lagged 64 rows")
    require(certificate["first_risk_session_key_ns"] == str(int(context.keys[first])),
            "risk window first key mismatch")
    require(context.keys[row] == evaluation.keys[decision], "risk window decision mismatch")
    require(re.fullmatch(r"[0-9a-f]{64}", certificate["risk_snapshot_sha256"]) is not None,
            "malformed native risk digest")
    close, raw, volume = (context.columns[k][first:row + 1] for k in ("close", "raw_close", "volume"))
    observed = (np.isfinite(close) & (close > 0) & np.isfinite(raw) & (raw > 0)
                & np.isfinite(volume) & (volume >= 0))
    with np.errstate(all="ignore"):
        binary_returns = close[1:] / close[:-1] - 1
    valid = observed[1:] & observed[:-1] & np.isfinite(binary_returns)
    counts = valid.sum(axis=0)
    ready = counts == 63
    eligible = evaluation.mask[decision].astype(bool)
    require(all(not units[i] or not eligible[i] or ready[i] for i in range(context.n)),
            "held eligible name lacks full adjacent risk history")
    variance = []
    for i in range(context.n):
        if not ready[i]:
            variance.append(Decimal("0.000001"))
            continue
        # A separate high-precision population variance, not native sum order.
        returns = [decimal(float(close[t, i])) / decimal(float(close[t - 1, i])) - ONE
                   for t in range(1, 64)]
        mean = sum(returns, ZERO) / 63
        variance.append(max(Decimal("0.000001"), sum(((r - mean) ** 2 for r in returns), ZERO) / 63))
    hasher = hashlib.sha256()
    hasher.update(b"independent-equity-risk-inputs-v1\n")
    hasher.update(context.manifest["artifact_id"].encode())
    hasher.update(f"\n{first}\n{row}\n".encode())
    for field in (close, raw, volume):
        hasher.update(field.astype("<f8", copy=False).tobytes(order="C"))
    return eligible & ready, variance, {
        "context_artifact_id": context.manifest["artifact_id"],
        "first_context_row": first, "decision_context_row": row,
        "first_session_key_ns": str(int(context.keys[first])),
        "last_session_key_ns": str(int(context.keys[row])),
        "independent_raw_risk_window_sha256": hasher.hexdigest(),
        "native_risk_snapshot_sha256": certificate["risk_snapshot_sha256"],
        "native_derived_digest_recomputed": False,
        "risk_ready_instruments": int(ready.sum()),
        "eligible_and_risk_ready_instruments": int((eligible & ready).sum()),
    }


def check_proposal(certificate, context, evaluation, books, context_first, units, cash, values):
    period = certificate["execution_period"]
    decision = certificate["decision_period"]
    n = evaluation.n
    nav = cash + sum(values, ZERO)
    require(nav > 0, "nonpositive pretrade NAV")
    comparisons = {key: compare(certificate[key], value, key, n, nav)
                   for key, value in (("pretrade_nav", nav), ("pretrade_cash", cash))}
    weights = certificate["proposed_weights_canonical_order"]
    require(isinstance(weights, list) and len(weights) == n, "proposal canonical width mismatch")
    weights = [decimal(w) for w in weights]
    admitted, variance, risk = risk_inputs(context, evaluation, certificate, context_first, units)
    fixed = ~admitted
    require(all(w == 0 for i, w in enumerate(weights) if fixed[i]), "nonzero ineligible/risk-unready proposal")
    # The native field counts mandatory exits inside the held/admitted union;
    # it does not count every excluded canonical column.
    fixed_exits = sum(bool(units[i]) and bool(fixed[i]) for i in range(n))
    require(certificate["fixed_zero_count"] == fixed_exits, "mandatory-exit count mismatch")
    require(certificate["union_instruments"] == sum(bool(units[i]) or bool(admitted[i]) for i in range(n)),
            "held/admitted union mismatch")
    reserve = ONE - FEE_RATE * Decimal("0.2")
    require(all(abs(w) <= Decimal("0.01") * reserve + ECONOMIC_TOL for w in weights),
            "prefee name cap exceeded")
    new_units, new_values = [], []
    for i, weight in enumerate(weights):
        if weight == 0:
            new_units.append(0.0)
            new_values.append(ZERO)
            continue
        price = float(evaluation.columns["close"][period, i])
        require(math.isfinite(price) and price > 0, f"proposed mark missing at {period}/{i}")
        desired = float(weight) * float(nav)
        quantity = desired / price
        represented = quantity * price
        require(all(math.isfinite(x) and x != 0 for x in (desired, quantity, represented)),
                f"proposed value/unit overflow/underflow at {period}/{i}")
        new_units.append(quantity)
        new_values.append(decimal(represented))
    deltas = [new - old for new, old in zip(new_values, values)]
    traded = sum(map(abs, deltas), ZERO)
    fee = traded * FEE_RATE
    post_cash = cash - sum(deltas, ZERO) - fee
    assets = sum(new_values, ZERO)
    post_nav = post_cash + assets
    require(post_nav > 0 and abs(post_nav - (nav - fee)) < Decimal("1e-35"),
            "self-financing conservation failed")
    preference = books.columns["weight"][decision // 5]
    objective = sum((Decimal("0.5") * (w - decimal(float(preference[i]))) ** 2
                     + variance[i] * w * w for i, w in enumerate(weights)), ZERO)
    expected = {
        "fee_reserve": reserve,
        "requested_prefee_net": sum(weights, ZERO),
        "requested_prefee_gross": sum(map(abs, weights), ZERO),
        "requested_turnover": sum((abs(w - value / nav) for w, value in zip(weights, values)), ZERO),
        "traded_dollars": traded, "trade_cost": fee,
        "posttrade_cash": post_cash, "posttrade_nav": post_nav,
        "actual_turnover": traded / nav,
        "postfee_net": assets / post_nav,
        "postfee_gross": sum(map(abs, new_values), ZERO) / post_nav,
        "postfee_max_name": max(map(abs, new_values), default=ZERO) / post_nav,
        "objective": objective,
    }
    require(abs(expected["requested_prefee_net"]) <= ECONOMIC_TOL, "prefee net exceeded")
    require(expected["requested_prefee_gross"] <= reserve + ECONOMIC_TOL, "prefee gross exceeded")
    require(expected["requested_turnover"] <= Decimal("0.2") + ECONOMIC_TOL, "requested turnover exceeded")
    for key, limit in (("actual_turnover", Decimal("0.2")), ("postfee_gross", ONE),
                       ("postfee_max_name", Decimal("0.01")), ("postfee_net", ZERO)):
        require(abs(expected[key]) <= limit + ECONOMIC_TOL, f"economic constraint failed: {key}")
    for key, value in expected.items():
        comparisons[key] = compare(certificate[key], value, key, n, nav)
    require(isinstance(certificate["solver_used"], bool) and isinstance(certificate["polished"], bool),
            "invalid solver flags")
    for key in ("primal_residual", "dual_residual"):
        residual = decimal(certificate[key])
        require(residual >= 0 and (not certificate["solver_used"] or residual <= Decimal("1e-6")),
                f"recorded solver residual exceeds frozen limit: {key}")
    return new_units, post_cash, new_values, {
        "decision_period": decision, "execution_period": period,
        "decision_session_key_ns": certificate["decision_session_key_ns"],
        "execution_session_key_ns": certificate["execution_session_key_ns"],
        "solver_used": certificate["solver_used"], "polished": certificate["polished"],
        "nonzero_proposed_names": sum(w != 0 for w in weights),
        "risk_inputs": risk,
        "independent_economic_values": {key: str(value) for key, value in expected.items()},
        "certificate_comparisons": comparisons,
    }


def stable_inputs():
    for name, expected in HASHES.items():
        with Path(name).open("rb") as source:
            hasher = hashlib.sha256()
            while block := source.read(1024 * 1024):
                hasher.update(block)
        require(hasher.hexdigest() == expected, f"input changed during verification: {name}")


def verify():
    require(not sys.flags.optimize, "run without -O: imported panel decoder uses assertions")
    read(Path(__file__))
    decoder_source = read(DECODER)
    module = importlib.util.module_from_spec(importlib.util.spec_from_file_location("baseline_panel_decoder", DECODER))
    # Execute the snapshotted, hashed bytes rather than opening a mutable module a second time.
    exec(compile(decoder_source, str(DECODER), "exec"), module.__dict__)
    context = module.Panel(CONTEXT)
    evaluation = module.Panel(BASELINE / "evaluation.bin")
    combo = module.Panel(BASELINE / "combo.bin")
    books = module.Panel(BASELINE / "books.bin")
    HASHES.update(module.INPUT_HASHES)
    for panel in (evaluation, combo, books):
        module.same_instrument_axes(context, panel)
    require(np.array_equal(evaluation.keys, combo.keys), "combo evaluation keys mismatch")
    require(np.array_equal(books.keys, evaluation.keys[::5]), "original weekly schedule mismatch")
    require(evaluation.parents["source-context"] == context.manifest["artifact_id"], "evaluation ancestry mismatch")
    require(combo.parents["research"] == evaluation.manifest["artifact_id"], "combo ancestry mismatch")
    require(books.parents["research"] == evaluation.manifest["artifact_id"] and
            books.parents["combo"] == combo.manifest["artifact_id"], "preference ancestry mismatch")
    original_failure = load_json(BASELINE / "failure.json")
    request = load_json(ATTEMPT / "request.json")
    failure = load_json(ATTEMPT / "failure.json")
    certificates = load_json(ATTEMPT / "allocation_certificates.json")
    require(original_failure["status"] == "failed" and failure["status"] == "failed", "expected failed original windows")
    require(failure["schema"] == "atx-equity-book-attempt-v1" and request["status"] == "started", "attempt schema mismatch")
    require(certificates["schema"] == "atx-equity-allocation-certificates-v1" and
            certificates["status"] == "failed-replay-proposals-only", "expected failed-replay proposals")
    require((ATTEMPT / ".pending").is_dir() and not (ATTEMPT / "manifest.json").exists() and
            not (ATTEMPT / "report/manifest.json").exists(), "failed attempt published completion or lost pending marker")
    recipe = request["recipe"]
    require(failure["recipe"] == recipe and failure["parents"] == request["parents"], "failure/request mismatch")
    expected_parents = {"source-context": context.manifest["artifact_id"], "evaluation": evaluation.manifest["artifact_id"],
                        "combo": combo.manifest["artifact_id"], "preferences": books.manifest["artifact_id"]}
    require(len(request["parents"]) == 4 and {p["role"]: p["sha256"] for p in request["parents"]} == expected_parents,
            "attempt parent identities mismatch")
    expected_recipe = {"profile": "constrained-preference-weekly-v1", "initial_nav": 100000000.0,
                       "execution_delay_observations": "1", "trade_bps_per_absolute_dollar": 5.0,
                       "annual_borrow_bps": 365.0, "borrow_day_basis": 365, "risk_return_count": 63,
                       "variance_floor": 1e-6, "risk_penalty": 1.0, "net_limit": 0, "gross_limit": 1.0,
                       "name_limit": 0.01, "full_l1_turnover_limit": 0.2, "economic_tolerance": 1e-8}
    for key, value in expected_recipe.items():
        require(recipe[key] == value, f"frozen recipe mismatch: {key}")
    baseline_recipe = json.loads(evaluation.manifest["recipe"])
    require(recipe["baseline_recipe"] == baseline_recipe == original_failure["recipe"], "original baseline recipe changed")
    require(baseline_recipe["evaluation_start"] == "2013-04-04" and
            baseline_recipe["evaluation_end_exclusive"] == "2014-01-01", "original evaluation window changed")
    first = int(np.searchsorted(context.keys, evaluation.keys[0]))
    require(first >= 63 and np.array_equal(context.keys[first:first + evaluation.d], evaluation.keys), "context slice mismatch")
    for field in ("close", "raw_close", "volume"):
        require(np.array_equal(context.columns[field][first:first + evaluation.d].view("<u8"),
                               evaluation.columns[field].view("<u8")), f"context/evaluation bytes differ: {field}")
    require(np.all((evaluation.mask == 0) | (context.mask[first:first + evaluation.d] == 1)), "expanded eligibility")
    proposals = certificates["decisions"]
    require(isinstance(proposals, list), "decisions must be an array")
    for index, certificate in enumerate(proposals):
        decision = integer(certificate["decision_period"], "decision period")
        execution = integer(certificate["execution_period"], "execution period")
        require(decision == index * 5 and execution == decision + 1 and execution < evaluation.d - 1,
                "proposal is not an original executable schedule prefix")
        require(certificate["decision_session_key_ns"] == str(int(evaluation.keys[decision])) and
                certificate["execution_session_key_ns"] == str(int(evaluation.keys[execution])), "proposal session key mismatch")
    cash, units = Decimal(100000000), [0.0] * evaluation.n
    consumed, checked, financing = 0, [], []
    stop = None
    try:
        for period in range(evaluation.d - 1):
            values = mark(units, evaluation, period)
            require(cash + sum(values, ZERO) > 0, "nonpositive carried NAV")
            if period == consumed * 5 + 1:
                if consumed == len(proposals):
                    stop = {"kind": "unrecorded-next-proposal", "period": period,
                            "interpretation": "no weights inferred beyond recorded proposal frontier"}
                    break
                units, cash, values, evidence = check_proposal(
                    proposals[consumed], context, evaluation, books, first, units, cash, values)
                checked.append(evidence)
                consumed += 1
            elapsed = Decimal(int(evaluation.keys[period + 1]) - int(evaluation.keys[period])) / DAY_NS
            require(elapsed > 0, "nonpositive elapsed calendar time")
            shorts = sum((-value for value in values if value < 0), ZERO)
            borrow = shorts * BORROW_RATE * elapsed / 365
            # Failure at the next held mark precedes debit of this interval's borrow.
            end_values = mark(units, evaluation, period + 1)
            cash -= borrow
            require(cash + sum(end_values, ZERO) > 0, "nonpositive post-financing NAV")
            financing.append({"period": period, "next_period": period + 1,
                              "elapsed_calendar_days": str(elapsed), "start_short_dollars": str(shorts),
                              "borrow_debited": str(borrow)})
        else:
            stop = {"kind": "last-evaluation-observation", "period": evaluation.d - 1}
    except MissingHeldMark as gap:
        stop = {"kind": "missing-held-mark", **gap.evidence,
                "incomplete_interval_borrow_debited": False}
    require(consumed == len(proposals), "native published proposals beyond independently observed failure")
    failure_text = failure["error"]
    match = re.search(r"missing/nonpositive required close at period=(\d+) instrument=(\d+) "
                      r"session_key_ns=(-?\d+) security_id=([^\s;]+)(?:\s|$)", failure_text)
    if match:
        require(stop["kind"] == "missing-held-mark", "native held-gap failure not independently reproduced")
        require((stop["period"], stop["instrument"], stop["session_key_ns"], stop["security_id"]) ==
                (int(match[1]), int(match[2]), match[3], match[4]), "first held-gap boundary mismatch")
    else:
        require(stop["kind"] != "missing-held-mark", "independent held gap differs from native failure type")
    stable_inputs()
    return {
        "schema": "atx-independent-allocation-proposals-v1",
        "verification_status": "verified-partial-allocation-evidence" if checked else "no-allocation-evidence",
        "scope": "partial software oracle of recorded proposals and observable carry only; not a performance replay",
        "original_replay_failed": True, "native_constrained_replay_failed": True,
        "completed_performance_result": False, "investment_qualification": "unverified",
        "native_failure": failure_text, "original_baseline_failure": original_failure["error"],
        "native_failure_independently_reproduced": bool(match),
        "non_mark_failure_scope": "solver/internal failure not independently reproduced" if not match else None,
        "proposal_count": len(proposals), "verified_proposal_count": len(checked),
        "observed_complete_intervals": len(financing), "observation_stop": stop,
        "decimal_precision": 50,
        "arithmetic": "exact input doubles; binary64 unit sizing/marks; Decimal sums, fees, ACT/365 borrow and constraints",
        "constraints": {"full_l1_turnover": "0.2", "postfee_gross": "1", "postfee_name": "0.01", "net_tolerance": "1e-8"},
        "risk_digest_scope": "native derived digest recorded, not recomputed; independent raw input digest and adjacent-return readiness verified",
        "certified_optimality": False, "source_correctness_attested": False,
        "input_hashes": dict(sorted(HASHES.items())), "parents": expected_parents,
        "decisions": checked, "financing_of_observed_intervals": financing,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=WORKTREE / "build-equity/audits/iteration8-allocation-proposal-verification.json")
    args = parser.parse_args()
    require(not args.output.exists(), "verification output must be fresh")
    code = 0
    try:
        with localcontext() as context:
            context.prec = 50
            result = verify()
    except Exception as error:
        result = {"schema": "atx-independent-allocation-proposals-v1", "verification_status": "failed",
                  "error": repr(error), "completed_performance_result": False,
                  "input_hashes": dict(sorted(HASHES.items()))}
        code = 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(result, output, indent=2, allow_nan=False)
        output.write("\n")
    print(json.dumps({key: value for key, value in result.items()
                      if key not in {"input_hashes", "decisions", "financing_of_observed_intervals"}}, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
