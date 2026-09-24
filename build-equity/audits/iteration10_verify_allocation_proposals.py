"""Independent intent/accounting oracle for a fresh 2013 equity-book attempt.

Run only after the native attempt stops. Imports the existing APNL decoder, never
engine code. A failed replay has no performance result, even if every published
proposal passes. Missing prices are never filled and unrecorded decisions are
never guessed. Decimal accounting uses the exact binary64 inputs at precision 50;
unit sizing and price products explicitly retain binary64 representability.
Recorded pretrade NAV is used only as a binary64 sizing/selection anchor after
independent carry validates it. Hold copies units exactly. Native code is never
imported. A complete observable replay still does not qualify an investment.
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
DEFAULT_ATTEMPT = Path("C:/atx/data/equity_book_training_2013_intents_20260920")
DECODER_SHA256 = "9a2052f35d1da6235b0fd0f11ff1222fe7f213a5a64f608cbaffbb9393064ee3"
PARENT_CHECKER_SHA256 = "a8020bc4a20d24472a313cdf73f84cff40c755fbcf7f7cb2dd63509aadeb60c2"
HASHES = {}
ZERO, ONE = Decimal(0), Decimal(1)
FEE_RATE, BORROW_RATE = Decimal("0.0005"), Decimal("0.0365")
ECONOMIC_TOL = Decimal("1e-8")
DAY_NS = 86_400_000_000_000
MONEY_FIELDS = {"pretrade_nav", "pretrade_cash", "traded_dollars", "trade_cost",
                "posttrade_cash", "posttrade_nav", "final_cash", "final_assets"}


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


def compare_small(actual, expected, label, n, scale=None):
    # Representation evidence can be far below 1e-12. Do not let a generic
    # absolute comparison floor erase the very corrections being audited.
    scale = max(abs(expected), abs(scale or ZERO), Decimal("1e-290"))
    tolerance = Decimal(128) * decimal(sys.float_info.epsilon) * max(n, 1) * scale
    error = abs(decimal(actual) - expected)
    require(error <= tolerance, f"{label}: representation error {error} exceeds {tolerance}")
    return {"absolute_error": str(error), "roundoff_tolerance": str(tolerance)}


def positive_zero(value):
    return float(value) == 0.0 and math.copysign(1.0, float(value)) > 0


def representation(certificate, admitted, units, values, anchor):
    n = len(units)
    raw = certificate["continuous_weights_canonical_order"]
    resolved = certificate["proposed_weights_canonical_order"]
    intents = certificate["proposed_intents_canonical_order"]
    require(all(isinstance(v, list) and len(v) == n for v in (raw, resolved, intents)),
            "raw/resolved/intent canonical width mismatch")
    raw = [float(decimal(w)) for w in raw]
    resolved = [float(decimal(w)) for w in resolved]
    reserve = 1.0 - 0.0005 * 0.2
    amplification = 1.0 + 0.0005 + 0.0005 * 1.0 + 0.0005 * 0.01
    budget = ((1e-8 * reserve) / 128.0) / amplification
    cap = budget / n
    require(math.isfinite(cap) and cap > 0, "invalid representation allowance")
    ordinary_change, fixed_change, maximum_eta = ZERO, ZERO, ZERO
    close_count, hold_count = 0, 0
    for i, (w, instruction) in enumerate(zip(raw, intents)):
        require(isinstance(instruction, dict) and set(instruction) == {"action", "weight"},
                f"malformed intent at {i}")
        payload = float(decimal(instruction["weight"]))
        previous = float(values[i]) / anchor
        if not admitted[i]:
            action, weight = "close", 0.0
            fixed_change += abs(decimal(w))
            # Outside the held/admitted union the solver never owns a variable.
            require(units[i] != 0 or w == 0, f"raw weight outside solver union at {i}")
        else:
            eta = min(64.0 * sys.float_info.epsilon * max(1.0, abs(w), abs(previous)), cap)
            maximum_eta = max(maximum_eta, decimal(eta))
            if abs(w) <= eta:
                action, weight = "close", 0.0
            elif abs(w - previous) <= eta:
                action, weight = "hold-current", previous
            else:
                action, weight = "target-weight", w
            ordinary_change += abs(decimal(weight) - decimal(w))
        require(instruction["action"] == action, f"causal intent priority mismatch at {i}")
        require(resolved[i] == weight, f"resolved intent weight mismatch at {i}")
        if action == "target-weight":
            require(payload == w, f"target payload differs from raw solver weight at {i}")
        else:
            require(positive_zero(payload), f"Hold/Close payload is not canonical +0 at {i}")
        if action == "close":
            require(positive_zero(resolved[i]), f"Close resolved weight is not canonical +0 at {i}")
            close_count += 1
        if action == "hold-current":
            hold_count += 1
    require(ordinary_change <= decimal(budget), "independent aggregate representation budget exceeded")
    require(certificate["close_count"] == close_count and certificate["hold_count"] == hold_count,
            "intent counts mismatch")
    require(certificate["solver_certificate_scope"] == "continuous-weights-before-representation",
            "raw solver certificate scope changed")
    metrics = {"representation_l1_budget": decimal(budget), "representation_max_eta": maximum_eta,
               "representation_l1_change": ordinary_change, "fixed_zero_l1_change": fixed_change}
    comparisons = {key: compare_small(certificate[key], value, key, n) for key, value in metrics.items()}
    return raw, intents, {"close_count": close_count, "hold_count": hold_count,
                         "target_weight_count": n - close_count - hold_count,
                         "independent_metrics": {key: str(value) for key, value in metrics.items()},
                         "certificate_comparisons": comparisons}


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
    # This anchor has already passed an independent cash/holdings check. Using
    # the actual binary64 operation input retains exact native-sized units at
    # later Hold seams; Decimal cash/fees/borrow remain independent throughout.
    anchor = float(certificate["pretrade_nav"])
    require(anchor > 0, "nonpositive binary64 sizing anchor")
    weights = certificate["proposed_weights_canonical_order"]
    require(isinstance(weights, list) and len(weights) == n, "proposal canonical width mismatch")
    weights = [decimal(w) for w in weights]
    admitted, variance, risk = risk_inputs(context, evaluation, certificate, context_first, units)
    raw, intents, intent_evidence = representation(certificate, admitted, units, values, anchor)
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
        action = intents[i]["action"]
        if action == "hold-current":
            new_units.append(units[i])
            new_values.append(values[i])
            continue
        if action == "close" or weight == 0:
            new_units.append(0.0)
            new_values.append(ZERO)
            continue
        price = float(evaluation.columns["close"][period, i])
        require(math.isfinite(price) and price > 0, f"proposed mark missing at {period}/{i}")
        desired = float(weight) * anchor
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
    raw_weights = list(map(decimal, raw))
    # Include normal Weight unit-sizing roundtrip in actual representation,
    # independently of the optional intent-snap budget.
    actual_weights = [decimal(float(value) / anchor) for value in new_values]
    raw_objective = sum((Decimal("0.5") * (w - decimal(float(preference[i]))) ** 2
                         + variance[i] * w * w for i, w in enumerate(raw_weights)), ZERO)
    actual_objective = sum((Decimal("0.5") * (w - decimal(float(preference[i]))) ** 2
                            + variance[i] * w * w for i, w in enumerate(actual_weights)), ZERO)
    corrections = [actual - original for actual, original in zip(actual_weights, raw_weights)]
    gap_terms = [delta * (raw_weights[i] - decimal(float(preference[i])) + delta / 2
                         + variance[i] * (actual_weights[i] + raw_weights[i]))
                 for i, delta in enumerate(corrections)]
    actual_change = sum(map(abs, corrections), ZERO)
    stable_gap = sum(gap_terms, ZERO)
    small_metrics = {"actual_representation_l1_change": actual_change,
                     "representation_objective_gap": stable_gap}
    for key, value in small_metrics.items():
        scale = sum(map(abs, gap_terms), ZERO) if key == "representation_objective_gap" else value
        intent_evidence["certificate_comparisons"][key] = compare_small(certificate[key], value, key, n, scale)
        intent_evidence["independent_metrics"][key] = str(value)
    for i, intent in enumerate(intents):
        if intent["action"] == "hold-current":
            require(new_units[i].hex() == units[i].hex() and deltas[i] == 0,
                    f"Hold changed units or traded at {i}")
        if intent["action"] == "close":
            require(positive_zero(new_units[i]) and new_values[i] == 0 and deltas[i] == -values[i],
                    f"Close failed to book complete carried exposure at {i}")
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
        "continuous_objective": raw_objective, "represented_objective": actual_objective,
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
        "nonzero_continuous_names": sum(w != 0 for w in raw),
        "intent_representation": intent_evidence,
        "validated_binary64_sizing_nav": anchor,
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


def hash_file(path):
    path = Path(path).resolve()
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            hasher.update(block)
    HASHES[str(path)] = hasher.hexdigest()
    return hasher.hexdigest()


def companions(directory, manifest):
    seen = set()
    for item in manifest["files"]:
        name = item["filename"]
        require(isinstance(name, str) and name not in seen, "duplicate/invalid companion filename")
        seen.add(name)
        path = (directory / name).resolve()
        require(path.is_relative_to(directory.resolve()), "companion escapes attempt directory")
        require(str(path.stat().st_size) == str(item["size_bytes"]), f"companion size mismatch: {name}")
        require(hash_file(path) == item["sha256"], f"companion hash mismatch: {name}")


def snapshot_evidence(preflight_path, attempt, recipe):
    preflight = load_json(preflight_path)
    require(preflight["schema"] == "atx.iteration10-equity-book-preflight-v1", "preflight schema changed")
    args = preflight["args"]
    require(Path(args[args.index("--out") + 1]).resolve() == attempt, "preflight belongs to a different attempt")
    for category in ("input_pins", "source_pins"):
        for name, pin in preflight[category].items():
            require(Path(name).stat().st_size == pin["size_bytes"] and hash_file(name) == pin["sha256"],
                    f"native preflight snapshot changed: {name}")
    executable = preflight["executable"]
    require(Path(executable["path"]).stat().st_size == executable["size_bytes"] and
            hash_file(executable["path"]) == executable["sha256"] == recipe["producer_executable_sha256"],
            "producer executable differs from native preflight snapshot")
    return {"preflight_path": str(preflight_path), "preflight_sha256": HASHES[str(preflight_path.resolve())],
            "producer_executable_sha256": executable["sha256"],
            "selected_source_pins": preflight["source_pins"],
            "scope": "selected first-party source/configuration snapshots; no native helper imported"}


def verify(attempt, preflight_path):
    require(not sys.flags.optimize, "run without -O: imported panel decoder uses assertions")
    read(Path(__file__))
    parent_checker = WORKTREE / "build-equity/audits/iteration9_verify_allocation_proposals.py"
    require(digest(read(parent_checker)) == PARENT_CHECKER_SHA256, "frozen iteration 9 checker changed")
    decoder_source = read(DECODER)
    require(digest(decoder_source) == DECODER_SHA256, "archived panel decoder snapshot changed")
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
    request = load_json(attempt / "request.json")
    certificates = load_json(attempt / "allocation_certificates.json")
    require(original_failure["status"] == "failed", "original baseline failure changed")
    require(request["schema"] == "atx-equity-book-attempt-v1" and request["status"] == "started", "attempt schema mismatch")
    require(certificates["schema"] == "atx-equity-allocation-certificates-v1", "certificate schema mismatch")
    recipe = request["recipe"]
    completed = (attempt / "manifest.json").is_file()
    failure = None
    report_manifest = None
    if completed:
        manifest = load_json(attempt / "manifest.json")
        report_manifest = load_json(attempt / "report/manifest.json")
        require(manifest["schema"] == "atx-equity-book-v1" and manifest["status"] == "complete" and
                manifest["qualification"] == "unknown", "invalid completed outer manifest")
        require(report_manifest["schema"] == "atx-replay-report-v1" and report_manifest["status"] == "complete" and
                manifest["report_id"] == report_manifest["report_id"], "invalid completed report binding")
        require(certificates["status"] == "complete-replay" and not (attempt / ".pending").exists() and
                not (attempt / "report/.pending").exists() and not (attempt / "failure.json").exists(),
                "completed attempt retains failure/pending state")
        require(manifest["recipe"] == recipe and manifest["parents"] == request["parents"], "completion/request mismatch")
        companions(attempt, manifest)
        companions(attempt / "report", report_manifest)
    else:
        failure = load_json(attempt / "failure.json")
        require(failure["schema"] == "atx-equity-book-attempt-v1" and failure["status"] == "failed", "invalid failure envelope")
        require(certificates["status"] == "failed-replay-proposals-only", "expected failed-replay proposals")
        require((attempt / ".pending").is_dir() and not (attempt / "report/manifest.json").exists(),
                "failed attempt published completion or lost pending marker")
        require(failure["recipe"] == recipe and failure["parents"] == request["parents"], "failure/request mismatch")
    expected_parents = {"source-context": context.manifest["artifact_id"], "evaluation": evaluation.manifest["artifact_id"],
                        "combo": combo.manifest["artifact_id"], "preferences": books.manifest["artifact_id"]}
    require(len(request["parents"]) == 4 and {p["role"]: p["sha256"] for p in request["parents"]} == expected_parents,
            "attempt parent identities mismatch")
    expected_recipe = {"profile": "constrained-preference-weekly-intents-v2", "initial_nav": 100000000.0,
                       "execution_delay_observations": "1", "trade_bps_per_absolute_dollar": 5.0,
                       "annual_borrow_bps": 365.0, "borrow_day_basis": 365, "risk_return_count": 63,
                       "variance_floor": 1e-6, "risk_penalty": 1.0, "net_limit": 0, "gross_limit": 1.0,
                       "name_limit": 0.01, "full_l1_turnover_limit": 0.2, "economic_tolerance": 1e-8}
    for key, value in expected_recipe.items():
        require(recipe[key] == value, f"frozen recipe mismatch: {key}")
    expected_representation = {
        "model": "MachinePrecisionIntentsV1",
        "eta": "min(64*binary64_epsilon*max(1,abs(raw),abs(previous)),budget/canonical_N)",
        "budget": "economic_tolerance*fee_reserve/(128*(1+fee_rate*(1+gross_limit+name_limit)))",
        "priority": "mandatory-close,abs(raw)<=eta-close,abs(raw-previous)<=eta-hold,target-weight",
        "hold": "exact-current-TRI-units-and-marked-dollars",
        "close": "zero-units-actual-closing-delta-and-fees",
        "economic_limits": "recertify-entire-represented-book-under-original-limits",
        "threshold_retry": "none", "physical_share_rounding": False, "economic_no_trade_band": False}
    require(recipe["representation"] == expected_representation, "representation recipe changed")
    source_snapshot = snapshot_evidence(preflight_path, attempt, recipe)
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
    failure_text = failure["error"] if failure else ""
    match = re.search(r"missing/nonpositive required close at period=(\d+) instrument=(\d+) "
                      r"session_key_ns=(-?\d+) security_id=([^\s;]+)(?:\s|$)", failure_text)
    if match:
        require(stop["kind"] == "missing-held-mark", "native held-gap failure not independently reproduced")
        require((stop["period"], stop["instrument"], stop["session_key_ns"], stop["security_id"]) ==
                (int(match[1]), int(match[2]), match[3], match[4]), "first held-gap boundary mismatch")
    elif not completed:
        require(stop["kind"] != "missing-held-mark", "independent held gap differs from native failure type")
    complete_evidence = None
    if completed:
        require(stop["kind"] == "last-evaluation-observation" and len(financing) == evaluation.d - 1,
                "native completion not independently reproduced")
        require(len(proposals) == sum(index + 1 < evaluation.d - 1 for index in range(0, evaluation.d, 5)),
                "complete attempt omits executable scheduled decisions")
        require(report_manifest["axes"]["session_keys"] == [str(int(key)) for key in evaluation.keys] and
                report_manifest["axes"]["instrument_ids"] == evaluation.axes["instrument_ids"],
                "completed report changed original axes")
        summary = load_json(attempt / "report/summary.json")
        require(summary["schema"] == "atx-replay-summary-v1" and
                summary["accepted_allocation_count"] == len(proposals) and
                summary["full"]["observed_intervals"] == evaluation.d - 1,
                "completed summary coverage mismatch")
        final_assets = sum(mark(units, evaluation, evaluation.d - 1), ZERO)
        complete_evidence = {"final_cash": str(cash), "final_assets": str(final_assets),
                             "final_nav": str(cash + final_assets),
                             "summary_comparisons": {
                                 "final_cash": compare(summary["final_cash"], cash, "final_cash", evaluation.n, cash + final_assets),
                                 "final_assets": compare(summary["final_assets"], final_assets, "final_assets", evaluation.n, cash + final_assets)},
                             "manifest_companion_hashes_verified": True,
                             "manifest_identity_digests_recomputed": False,
                             "performance_ratios_recomputed": False}
    stable_inputs()
    return {
        "schema": "atx-independent-allocation-intents-v2",
        "native_attempt_root": str(attempt),
        "derived_from_iteration9_checker_sha256": PARENT_CHECKER_SHA256,
        "pinned_panel_decoder_sha256": DECODER_SHA256,
        "verification_status": ("verified-complete-allocation-and-carry-evidence" if completed else
                                "verified-partial-allocation-evidence" if checked else "no-allocation-evidence"),
        "scope": "software oracle of recorded intents, economics and observable carry; no investment qualification",
        "original_replay_failed": True, "native_constrained_replay_failed": not completed,
        "completed_performance_result": completed, "investment_qualification": "unverified",
        "native_failure": failure_text, "original_baseline_failure": original_failure["error"],
        "native_failure_independently_reproduced": bool(match),
        "non_mark_failure_scope": "solver/internal failure not independently reproduced" if failure and not match else None,
        "proposal_count": len(proposals), "verified_proposal_count": len(checked),
        "observed_complete_intervals": len(financing), "observation_stop": stop,
        "decimal_precision": 50,
        "arithmetic": "exact input doubles; independently validated native NAV anchors binary64 sizing/intent selection; exact unit Hold; Decimal sums, fees, ACT/365 borrow and constraints",
        "complete_observation_evidence": complete_evidence,
        "native_source_snapshot": source_snapshot,
        "constraints": {"full_l1_turnover": "0.2", "postfee_gross": "1", "postfee_name": "0.01", "net_tolerance": "1e-8"},
        "risk_digest_scope": "native derived digest recorded, not recomputed; independent raw input digest and adjacent-return readiness verified",
        "certified_optimality": False, "source_correctness_attested": False,
        "input_hashes": dict(sorted(HASHES.items())), "parents": expected_parents,
        "decisions": checked, "financing_of_observed_intervals": financing,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ATTEMPT,
                        help="finished native attempt directory; original context/baseline inputs remain fixed")
    parser.add_argument("--preflight", type=Path, default=WORKTREE / "build-equity/audits/iteration10-equity-book-preflight.json")
    parser.add_argument("--output", type=Path, default=WORKTREE / "build-equity/audits/iteration10-allocation-proposal-verification.json")
    args = parser.parse_args()
    require(not args.output.exists(), "verification output must be fresh")
    code = 0
    try:
        with localcontext() as context:
            context.prec = 50
            result = verify(args.root.resolve(), args.preflight.resolve())
    except Exception as error:
        result = {"schema": "atx-independent-allocation-intents-v2", "verification_status": "failed",
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
