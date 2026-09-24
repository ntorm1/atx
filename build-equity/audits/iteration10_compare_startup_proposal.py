"""Compare raw startup QP weights and explicit execution intents independently.

Adapted from the immutable iteration9 comparison; no iteration9 file is changed.

No native build, solver, executable or replay call. Even a passing comparison is
only a startup proposal check; a later failed replay produces no performance result.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from decimal import Decimal, localcontext
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys
import struct
import time

import numpy as np


ROOT = Path("C:/atx/.worktrees/equity-platform")
AUDITS = ROOT / "build-equity/audits"
ORACLE = AUDITS / "iteration9-startup-oracle.json"
ORACLE_SHA = "a4d8a681fa6e0737cf72107ddff431b867d34e18e0cc3327e6c68a0813b5b74f"
ORACLE_SOURCE = AUDITS / "iteration9_startup_oracle.py"
DEFAULT_NATIVE = Path("C:/atx/data/equity_book_training_2013_intents_20260920")
COMBO_ID = "def31e5ca03dc2329bb05a2526c4470e77028fea060d0c3743634d6b4f86a692"
COMBO_PINS = {
    "C:/atx/data/equity_baseline_training_2013_20260919/combo.bin":
        "275c19f9b9b281755391c4a9c1bd23f880d5b0b3e400964927bd2438aa22e671",
    "C:/atx/data/equity_baseline_training_2013_20260919/combo.bin.manifest.json":
        "05f0426590676a2bbf1ab1a7055a1d4175da92e03978e217a507c41c9114af6c",
}
WEIGHT_TOL = Decimal("1e-10")
OBJECTIVE_TOL = Decimal("1e-12")
MONEY_TOL = Decimal("1e-6")
ECONOMIC_TOL = Decimal("1e-8")
HASHES = {}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def normalized(path):
    return str(Path(path).resolve()).casefold()


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def read(path):
    path = Path(path).resolve()
    value = path.read_bytes()
    HASHES[str(path)] = hashlib.sha256(value).hexdigest()
    return value


def object_pairs(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, f"duplicate JSON key: {key}")
        value[key] = item
    return value


def invalid_constant(value):
    raise ValueError(f"nonfinite JSON constant: {value}")


def load(path):
    require(Path(path).stat().st_size <= 32 * 1024 * 1024, f"oversized JSON: {path}")
    return json.loads(read(path), object_pairs_hook=object_pairs, parse_constant=invalid_constant)


def dec(value):
    require(isinstance(value, (int, float)) and not isinstance(value, bool), "expected number")
    require(math.isfinite(float(value)), "nonfinite numeric input")
    return Decimal.from_float(float(value))


def import_oracle(expected_hash):
    source = read(ORACLE_SOURCE)
    require(hashlib.sha256(source).hexdigest() == expected_hash, "frozen oracle source changed")
    spec = importlib.util.spec_from_file_location("iteration9_frozen_startup_model", ORACLE_SOURCE)
    require(spec is not None, "cannot import frozen startup model")
    module = importlib.util.module_from_spec(spec)
    exec(compile(source, str(ORACLE_SOURCE), "exec"), module.__dict__)
    return module


def bind_measurement(path, native_root, oracle, request_path, certificate_path):
    measurement = load(path)
    require(measurement["status"] in ("completed", "failed") and
            isinstance(measurement.get("exit_code"), int) and
            isinstance(measurement.get("pid"), int) and measurement.get("finished_utc"),
            "native measurement is not a terminal launched attempt")
    require(not measurement.get("termination_error"), "runner failed to terminate its native child")
    plan = measurement["preflight"]
    preflight_path = path.parent / "iteration10-equity-book-preflight.json"
    preflight = load(preflight_path)
    require(HASHES[str(preflight_path.resolve())] == measurement["preflight_sha256"] and
            preflight == plan, "measurement/preflight binding mismatch")
    argv = measurement["args"]
    require(argv == plan["args"] and argv[1] == "equity-book" and
            normalized(argv[argv.index("--out") + 1]) == normalized(native_root),
            "native command does not identify this attempt")
    recheck = measurement["pin_recheck"]
    require(recheck["all_unchanged"], "runner observed changed inputs/executable/sources")
    observed_inputs = {}
    for category in ("input_pins", "source_pins"):
        require(set(plan[category]) == set(recheck[category]), "runner pin set changed")
        for name, before in plan[category].items():
            after = recheck[category][name]
            require(after["unchanged"] and before == after["after"], f"changed run pin: {name}")
            require(Path(name).stat().st_size == before["size_bytes"] and
                    digest(name) == before["sha256"], f"current run evidence changed: {name}")
            HASHES[str(Path(name).resolve())] = before["sha256"]
            if category == "input_pins":
                observed_inputs[normalized(name)] = before["sha256"]
    executable = plan["executable"]
    before_exe = {key: value for key, value in executable.items() if key != "path"}
    require(recheck["executable"]["unchanged"] and
            recheck["executable"]["after"] == before_exe and
            digest(executable["path"]) == executable["sha256"] and
            Path(executable["path"]).stat().st_size == executable["size_bytes"],
            "native executable binding mismatch")
    HASHES[str(Path(executable["path"]).resolve())] = executable["sha256"]
    for name, expected in oracle["input_hashes"].items():
        if name.endswith(".bin") or name.endswith(".bin.manifest.json"):
            require(observed_inputs.get(normalized(name)) == expected,
                    f"native inputs differ from frozen oracle: {name}")
    for name, expected in COMBO_PINS.items():
        require(observed_inputs.get(normalized(name)) == expected,
                f"native combo differs from original preference parent: {name}")
    output_files = {item["filename"]: item for item in measurement["output_evidence"]["files"]}
    require(len(output_files) == len(measurement["output_evidence"]["files"]),
            "duplicate output evidence filename")
    for artifact in (request_path, certificate_path):
        name = artifact.relative_to(native_root).as_posix()
        require(name in output_files, f"native output missing from runner evidence: {name}")
        item = output_files[name]
        require(digest(artifact) == item["sha256"] and
                artifact.stat().st_size == item["size_bytes"], "native output binding mismatch")
    return measurement, executable["sha256"]


BASE_COMPARISON = AUDITS / "iteration9_compare_startup_proposal.py"
BASE_COMPARISON_SHA = "0585830f65df266fe61ca6fdaedc6b7a968dc91ef4a54305c93012451f2e8af3"
GAP_TOL = Decimal("1e-18")


def same_float(left, right):
    return struct.pack("<d", float(left)) == struct.pack("<d", float(right))


def vector(value, n, label):
    require(isinstance(value, list) and len(value) == n, f"invalid canonical shape: {label}")
    for item in value:
        dec(item)
    return [float(item) for item in value]


def independent_representation(first, recipe, oracle, evaluation, preferences,
                               union, variance, exact_variance):
    """Startup only: all positions begin at exact zero; no later policy oracle."""
    require(recipe["profile"] == "constrained-preference-weekly-intents-v2",
            "unsupported native allocation profile")
    expected_recipe = {
        "model": "MachinePrecisionIntentsV1",
        "eta": "min(64*binary64_epsilon*max(1,abs(raw),abs(previous)),budget/canonical_N)",
        "budget": "economic_tolerance*fee_reserve/(128*(1+fee_rate*(1+gross_limit+name_limit)))",
        "priority": "mandatory-close,abs(raw)<=eta-close,abs(raw-previous)<=eta-hold,target-weight",
        "hold": "exact-current-TRI-units-and-marked-dollars",
        "close": "zero-units-actual-closing-delta-and-fees",
        "economic_limits": "recertify-entire-represented-book-under-original-limits",
        "threshold_retry": "none",
        "physical_share_rounding": False,
        "economic_no_trade_band": False,
    }
    require(recipe["representation"] == expected_recipe, "representation recipe changed")
    require(recipe["economic_tolerance"] == float(ECONOMIC_TOL),
            "economic acceptance tolerance changed")
    require(first["solver_used"] is True and
            first["solver_certificate_scope"] == "continuous-weights-before-representation",
            "raw solver certificate scope is missing or mislabeled")
    n = evaluation.n
    raw = vector(first["continuous_weights_canonical_order"], n, "continuous weights")
    proposed = vector(first["proposed_weights_canonical_order"], n, "resolved weights")
    received = first["proposed_intents_canonical_order"]
    require(isinstance(received, list) and len(received) == n, "incorrect intent shape")
    union_set = set(union)
    require(all(w == 0.0 for i, w in enumerate(raw) if i not in union_set),
            "raw startup proposal opens outside the frozen eligible/risk-ready union")

    nav_float = float(recipe["initial_nav"])
    fee_rate = float(recipe["trade_bps_per_absolute_dollar"]) * 1e-4
    reserve = 1.0 - fee_rate * float(recipe["full_l1_turnover_limit"])
    amplification = (1.0 + fee_rate + fee_rate * float(recipe["gross_limit"]) +
                     fee_rate * float(recipe["name_limit"]))
    budget = (float(recipe["economic_tolerance"]) * reserve / 128.0) / amplification
    per_name = budget / n
    require(all(math.isfinite(x) and x > 0 for x in (reserve, amplification, budget, per_name)),
            "unrepresentable startup representation budget")
    require(same_float(first["fee_reserve"], reserve) and
            same_float(first["representation_l1_budget"], budget),
            "reported fee reserve or representation budget differs")

    expected_actions, expected_weights, etas = [], [], []
    change = fixed_change = max_eta = 0.0
    changes = []
    for i, weight in enumerate(raw):
        previous = 0.0  # Explicit all-cash startup boundary, pinned above.
        eta = 0.0
        if i not in union_set:
            action, represented, payload = "close", 0.0, 0.0
            fixed_change += abs(weight)
        else:
            eta = min(64.0 * sys.float_info.epsilon *
                      max(1.0, abs(weight), abs(previous)), per_name)
            require(math.isfinite(eta) and eta > 0, "invalid independent eta")
            max_eta = max(max_eta, eta)
            if abs(weight) <= eta:
                action, represented, payload = "close", 0.0, 0.0
            elif abs(weight - previous) <= eta:
                action, represented, payload = "hold-current", previous, 0.0
            else:
                action, represented, payload = "target-weight", weight, weight
            change += abs(represented - weight)
        intent = received[i]
        require(isinstance(intent, dict) and set(intent) == {"action", "weight"},
                f"invalid intent object: instrument={i}")
        dec(intent["weight"])
        require(intent["action"] == action and same_float(intent["weight"], payload),
                f"intent differs from independent rule: instrument={i}")
        require(same_float(proposed[i], represented),
                f"resolved weight differs from independent instruction: instrument={i}")
        if not same_float(weight, represented):
            changes.append({"instrument_index": i, "security_id": oracle["instrument_ids"][i],
                            "raw_weight": weight, "resolved_weight": represented,
                            "action": action, "eta": eta})
        expected_actions.append(action)
        expected_weights.append(represented)
        etas.append(eta)
    counts = {action: expected_actions.count(action)
              for action in ("target-weight", "hold-current", "close")}
    require(counts["hold-current"] == 0, "all-cash startup unexpectedly holds a nonzero position")
    require(change <= budget, "independent ordinary representation exceeds its aggregate budget")
    for key, expected in (("representation_l1_change", change),
                          ("fixed_zero_l1_change", fixed_change),
                          ("representation_max_eta", max_eta)):
        require(same_float(first[key], expected), f"native representation scalar differs: {key}")
    require(first["close_count"] == counts["close"] and first["hold_count"] == counts["hold-current"]
            and first["fixed_zero_count"] == 0, "native startup action/fixed-exit counts differ")

    # Independent scalar binary64 execution, using only the pinned APNL marks and
    # independently derived instructions. No engine/helper code is imported.
    marks = evaluation.columns["close"][1]
    units, dollar_floats = [], []
    cash_float = nav_float
    assets_float = traded_float = gross_float = max_name_float = 0.0
    for i, (action, weight) in enumerate(zip(expected_actions, expected_weights)):
        quantity = amount = 0.0
        if action == "target-weight" and weight != 0.0:
            mark = float(marks[i])
            require(math.isfinite(mark) and mark > 0, f"missing represented mark: instrument={i}")
            desired = weight * nav_float
            quantity = desired / mark
            amount = quantity * mark
            require(all(math.isfinite(x) and x != 0 for x in (desired, quantity, amount)),
                    f"unrepresentable startup instruction: instrument={i}")
        elif action == "hold-current":
            quantity = amount = 0.0  # Startup exact input holdings.
        if amount != 0.0:
            cash_float -= amount
            traded_float += abs(amount)
        assets_float += amount
        gross_float += abs(amount)
        max_name_float = max(max_name_float, abs(amount))
        units.append(quantity)
        dollar_floats.append(amount)
    fee_float = traded_float * fee_rate
    cash_float -= fee_float
    post_nav_float = cash_float + assets_float
    require(math.isfinite(post_nav_float) and post_nav_float > 0, "invalid independent posttrade NAV")
    binary_execution = {
        "traded_dollars": traded_float, "trade_cost": fee_float,
        "posttrade_cash": cash_float, "posttrade_nav": post_nav_float,
        "actual_turnover": traded_float / nav_float,
        "postfee_net": assets_float / post_nav_float,
        "postfee_gross": gross_float / post_nav_float,
        "postfee_max_name": max_name_float / post_nav_float,
    }
    binary_errors = {}
    for key, expected in binary_execution.items():
        require(same_float(first[key], expected), f"binary64 native execution differs: {key}")
        binary_errors[key] = str(abs(dec(first[key]) - dec(expected)))

    with localcontext() as ctx:
        ctx.prec = 70
        raw_values = [dec(w) for w in raw]
        resolved_values = [dec(w) for w in proposed]
        frozen = [dec(w) for w in oracle["weights"]]
        differences = [abs(a - b) for a, b in zip(raw_values, frozen)]
        maximum = max(differences)
        index = differences.index(maximum)
        require(maximum <= WEIGHT_TOL, f"raw startup differs from independent oracle: {maximum}")
        pref = [dec(w) for w in preferences.columns["weight"][0]]
        def objective(weights):
            return sum(Decimal("0.5") * (w - a) ** 2 + risk * w ** 2
                       for w, a, risk in zip(weights, pref, exact_variance))
        raw_objective = objective(raw_values)
        raw_difference = abs(raw_objective -
                             Decimal(oracle["certificate"]["primal_objective_full_canonical"]))
        raw_reported_difference = abs(raw_objective - dec(first["continuous_objective"]))
        require(raw_difference <= OBJECTIVE_TOL and raw_reported_difference <= OBJECTIVE_TOL,
                "raw objective differs from frozen oracle or native continuous objective")

        dollars = [dec(amount) for amount in dollar_floats]
        nav = dec(nav_float)
        actual_weights = [amount / nav for amount in dollars]
        represented_objective = objective(actual_weights)
        resolved_objective = objective(resolved_values)
        gap = represented_objective - raw_objective
        objective_errors = {
            "resolved_requested": abs(resolved_objective - dec(first["objective"])),
            "represented_actual": abs(represented_objective - dec(first["represented_objective"])),
            "actual_minus_continuous_gap": abs(gap - dec(first["representation_objective_gap"])),
        }
        require(objective_errors["resolved_requested"] <= OBJECTIVE_TOL and
                objective_errors["represented_actual"] <= OBJECTIVE_TOL and
                objective_errors["actual_minus_continuous_gap"] <= GAP_TOL,
                "represented objective or stable objective gap differs")
        assets = sum(dollars)
        traded = sum(map(abs, dollars))
        fee = traded * Decimal("0.0005")
        post_cash = nav - assets - fee
        post_nav = post_cash + assets
        reconstructed = {
            "traded_dollars": traded, "trade_cost": fee,
            "posttrade_cash": post_cash, "posttrade_nav": post_nav,
            "actual_turnover": traded / nav, "postfee_net": assets / post_nav,
            "postfee_gross": traded / post_nav,
            "postfee_max_name": max(map(abs, dollars)) / post_nav,
            "requested_prefee_net": sum(resolved_values),
            "requested_prefee_gross": sum(map(abs, resolved_values)),
            "requested_turnover": sum(map(abs, resolved_values)),
        }
        errors = {}
        for key, expected in reconstructed.items():
            error = abs(expected - dec(first[key]))
            tolerance = MONEY_TOL if key in {
                "traded_dollars", "trade_cost", "posttrade_cash", "posttrade_nav"
            } else Decimal("1e-12")
            require(error <= tolerance, f"native {key} fails independent Decimal check: {error}")
            errors[key] = str(error)
        require(reconstructed["actual_turnover"] <= Decimal("0.2") + ECONOMIC_TOL and
                abs(reconstructed["postfee_net"]) <= ECONOMIC_TOL and
                reconstructed["postfee_gross"] <= 1 + ECONOMIC_TOL and
                reconstructed["postfee_max_name"] <= Decimal("0.01") + ECONOMIC_TOL and
                abs(reconstructed["requested_prefee_net"]) <= ECONOMIC_TOL and
                reconstructed["requested_prefee_gross"] <= dec(reserve) + ECONOMIC_TOL and
                max(map(abs, resolved_values)) <= Decimal("0.01") * dec(reserve) + ECONOMIC_TOL,
                "represented startup violates original economic limits")
        raw_to_actual = sum(abs(w - r) for w, r in zip(actual_weights, raw_values))
        # Native diagnostic uses a binary64 dollars/NAV division before reduction;
        # its exact operation-order value is also checked independently.
        actual_change_float = 0.0
        for amount, weight in zip(dollar_floats, raw):
            actual_change_float += abs(amount / nav_float - weight)
        require(same_float(first["actual_representation_l1_change"], actual_change_float),
                "native raw-to-actual representation diagnostic differs")
        support = [i for i, value in enumerate(dollars) if value != 0]
        oracle_support = [i for i, value in enumerate(frozen) if value != 0]
        require(support == oracle_support, "represented startup support differs from frozen oracle")
        require(all(i in union_set for i in support), "represented support leaves frozen union")
        return {
            "raw_continuous_solution": {
                "canonical_weights_compared": n,
                "max_abs_weight_difference": str(maximum),
                "max_difference_instrument_index": index,
                "max_difference_security_id": oracle["instrument_ids"][index],
                "l1_weight_difference": str(sum(differences)),
                "weight_tolerance": str(WEIGHT_TOL),
                "independent_objective": str(raw_objective),
                "objective_vs_oracle_difference": str(raw_difference),
                "native_continuous_objective_difference": str(raw_reported_difference),
                "objective_tolerance": str(OBJECTIVE_TOL),
                "nonzero_raw_count": sum(w != 0 for w in raw),
            },
            "representation": {
                "method": "independent-all-cash-machine-rule-from-frozen-recipe",
                "all_canonical_actions_payloads_and_resolved_weights_match_bits": True,
                "action_counts": counts, "canonical_actions": expected_actions,
                "represented_support_indices": support,
                "represented_support_count": len(support),
                "support_matches_independent_oracle": True,
                "changed_coordinates": changes,
                "ordinary_l1_change_binary64": change,
                "ordinary_l1_change_decimal": str(sum(abs(w-r) for w,r in zip(resolved_values,raw_values))),
                "ordinary_l1_budget": budget,
                "maximum_eta": max_eta,
                "fixed_zero_l1_change": fixed_change,
                "raw_to_actual_l1_decimal": str(raw_to_actual),
                "raw_to_actual_l1_binary64": actual_change_float,
                "resolved_requested_objective": str(resolved_objective),
                "represented_actual_objective": str(represented_objective),
                "actual_minus_continuous_objective_gap": str(gap),
                "reported_objective_absolute_errors": {k: str(v) for k,v in objective_errors.items()},
                "gap_tolerance": str(GAP_TOL),
            },
            "actual_execution": {
                "tri_units_canonical_order": units,
                "marked_dollars_canonical_order": dollar_floats,
                "ordered_binary64": binary_execution,
                "binary64_reported_absolute_errors": binary_errors,
                "decimal": {key: str(value) for key,value in reconstructed.items()},
                "decimal_reported_absolute_errors": errors,
                "money_tolerance": str(MONEY_TOL),
                "economic_tolerance": str(ECONOMIC_TOL),
                "accounting_basis": "independent binary64 sizing and ordered accounting plus 70-digit Decimal sums; no PnL",
            },
        }



def compare(args):
    require(sys.flags.optimize == 0, "archived decoder requires enabled assertions")
    require(hashlib.sha256(read(BASE_COMPARISON)).hexdigest() == BASE_COMPARISON_SHA,
            "immutable iteration9 comparison source changed")
    HASHES[str(Path(__file__).resolve())] = digest(__file__)
    oracle = load(ORACLE)
    require(HASHES[str(ORACLE.resolve())] == ORACLE_SHA, "frozen startup oracle changed")
    oracle_pins = {normalized(name): value for name, value in oracle["input_hashes"].items()}
    for name, expected in oracle["input_hashes"].items():
        require(digest(name) == expected, f"frozen oracle input changed: {name}")
        HASHES[str(Path(name).resolve())] = expected
    for name, expected in COMBO_PINS.items():
        require(digest(name) == expected, f"original combo input changed: {name}")
        oracle_pins[normalized(name)] = expected
        HASHES[str(Path(name).resolve())] = expected
    model = import_oracle(oracle_pins[normalized(ORACLE_SOURCE)])
    reader, reader_sha = model.import_reader()
    require(reader_sha == oracle_pins[normalized(model.READER)], "archived decoder changed")
    context = reader.Panel(model.CONTEXT)
    evaluation = reader.Panel(model.BASELINE / "evaluation.bin")
    preferences = reader.Panel(model.BASELINE / "books.bin")
    combo = reader.Panel(model.BASELINE / "combo.bin")
    for label, panel in (("context", context), ("evaluation", evaluation),
                         ("preferences", preferences)):
        require(panel.manifest["artifact_id"] == oracle["panel_artifact_ids"][label],
                f"panel identity mismatch: {label}")
        reader.same_instrument_axes(context, panel)
        require(panel.axes["instrument_ids"] == oracle["instrument_ids"] and
                panel.axes["original_instrument_indices"] == oracle["original_instrument_indices"] and
                panel.axes["instrument_namespace"] == oracle["instrument_namespace"],
                f"canonical oracle/native axes differ: {label}")
    reader.same_instrument_axes(evaluation, combo)
    require(combo.manifest["artifact_id"] == COMBO_ID and
            np.array_equal(combo.keys, evaluation.keys) and preferences.parents["combo"] == COMBO_ID,
            "original preference combo parent differs")
    for name, value in reader.INPUT_HASHES.items():
        require(oracle_pins.get(normalized(name)) == value, "APNL reader input pin mismatch")
        HASHES[str(Path(name).resolve())] = value
    request_path = args.native_root / "request.json"
    certificate_path = args.native_root / "allocation_certificates.json"
    measurement, exe_sha = bind_measurement(args.measurement, args.native_root, oracle,
                                            request_path, certificate_path)
    request, proposals = load(request_path), load(certificate_path)
    require(proposals["schema"] == "atx-equity-allocation-certificates-v1" and
            proposals["status"] in ("complete-replay", "failed-replay-proposals-only"),
            "unsupported native proposal evidence")
    require(len(proposals["decisions"]) > 0, "native attempt produced no first proposal")
    first = proposals["decisions"][0]
    parents = {item["role"]: item["sha256"] for item in request["parents"]}
    require(len(parents) == len(request["parents"]) and
            parents["source-context"] == oracle["panel_artifact_ids"]["context"] and
            parents["evaluation"] == oracle["panel_artifact_ids"]["evaluation"] and
            parents["preferences"] == oracle["panel_artifact_ids"]["preferences"] and
            parents["combo"] == COMBO_ID,
            "native request does not bind the frozen startup inputs")
    recipe = request["recipe"]
    require(recipe["producer_executable_sha256"] == exe_sha, "native recipe producer mismatch")
    for key, expected in (("risk_return_count", 63), ("risk_penalty", 1.0),
                          ("variance_floor", 1e-6), ("full_l1_turnover_limit", 0.2),
                          ("gross_limit", 1.0), ("name_limit", 0.01),
                          ("trade_bps_per_absolute_dollar", 5.0),
                          ("initial_nav", 100_000_000.0), ("execution_delay_observations", "1")):
        require(recipe[key] == expected, f"native startup changed economic parameter {key}")
    for key, expected in (("decision_period", 0), ("execution_period", 1),
                          ("decision_context_row", 256), ("first_context_row", 193),
                          ("decision_session_key_ns", oracle["decision_session_key_ns"]),
                          ("execution_session_key_ns", oracle["execution_session_key_ns"]),
                          ("first_risk_session_key_ns", oracle["risk_session_keys_ns"][0]),
                          ("pretrade_nav", 100_000_000.0), ("pretrade_cash", 100_000_000.0)):
        require(first[key] == expected, f"first native proposal timing/state mismatch: {key}")
    counts, variance, exact_variance, risk_stats = model.freeze_risk(context)
    require(counts.tolist() == oracle["valid_return_count"] and
            variance.tolist() == oracle["variance"], "independent risk snapshot changed")
    union = np.flatnonzero(evaluation.mask[0].astype(bool) & (counts == 63)).tolist()
    require(union == oracle["union_indices"] and len(union) == first["union_instruments"],
            "native/oracle startup union changed")
    numeric = independent_representation(first, recipe, oracle, evaluation, preferences,
                                         union, variance, exact_variance)
    require(all(digest(name) == expected for name, expected in HASHES.items()),
            "comparison input/evidence changed while checking")
    return {"status": "passed-first-startup-intent-comparison", "numeric": numeric,
        "risk_stats": risk_stats, "union_instruments": len(union),
        "native_proposal_count": len(proposals["decisions"]),
        "native_proposal_status": proposals["status"], "native_measurement_status": measurement["status"],
        "native_exit_code": measurement["exit_code"], "native_producer_executable_sha256": exe_sha,
        "native_solver": {key: first[key] for key in ("polished", "primal_residual", "dual_residual",
                                                       "effective_solver_feasibility_tolerance")},
        "oracle_sha256": ORACLE_SHA, "input_hashes": dict(HASHES), "all_pins_unchanged": True,
        "scope": "first all-cash raw solution and represented intents only; no later replay or PnL acceptance",
        "immutable_base_comparison_sha256": BASE_COMPARISON_SHA}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-root", type=Path, default=DEFAULT_NATIVE)
    parser.add_argument("--measurement", type=Path,
                        default=AUDITS / "iteration10-equity-book-measurement.json")
    parser.add_argument("--output", type=Path,
                        default=AUDITS / "iteration10-startup-native-comparison.json")
    args = parser.parse_args()
    require(not args.output.exists(), f"refusing to overwrite comparison: {args.output}")
    started = time.perf_counter()
    result = {"schema": "atx.iteration10-startup-native-comparison-v1", "status": "failed",
              "native_root": str(args.native_root), "measurement": str(args.measurement),
              "script_sha256": digest(__file__), "started_utc": datetime.now(timezone.utc).isoformat()}
    try:
        result.update(compare(args))
    except Exception as error:
        result.update({"error": f"{type(error).__name__}: {error}", "input_hashes": dict(HASHES)})
    result.update({"finished_utc": datetime.now(timezone.utc).isoformat(),
                   "wall_seconds": time.perf_counter() - started})
    with args.output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")
    summary = {key: result[key] for key in ("status", "error", "wall_seconds",
               "native_exit_code", "native_proposal_count", "all_pins_unchanged") if key in result}
    if "numeric" in result:
        summary["raw_comparison"] = result["numeric"]["raw_continuous_solution"]
        representation = result["numeric"]["representation"]
        summary["representation"] = {key: representation[key] for key in (
            "action_counts", "represented_support_count", "ordinary_l1_change_binary64",
            "ordinary_l1_budget", "support_matches_independent_oracle")}
        summary["actual_execution"] = result["numeric"]["actual_execution"]["ordered_binary64"]
    print(json.dumps(summary, sort_keys=True), flush=True)
    return 0 if result["status"] == "passed-first-startup-intent-comparison" else 1


if __name__ == "__main__":
    raise SystemExit(main())
