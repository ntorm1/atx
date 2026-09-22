"""Compare one terminal native attempt's first proposal with the frozen KKT oracle.

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
import time

import numpy as np


ROOT = Path("C:/atx/.worktrees/equity-platform")
AUDITS = ROOT / "build-equity/audits"
ORACLE = AUDITS / "iteration9-startup-oracle.json"
ORACLE_SHA = "a4d8a681fa6e0737cf72107ddff431b867d34e18e0cc3327e6c68a0813b5b74f"
ORACLE_SOURCE = AUDITS / "iteration9_startup_oracle.py"
DEFAULT_NATIVE = Path("C:/atx/data/equity_book_training_2013_qpfix_20260920")
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
    preflight_path = path.parent / "iteration9-equity-book-preflight.json"
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


def compare(args):
    require(sys.flags.optimize == 0, "archived decoder requires enabled assertions")
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
    native_weights = first["proposed_weights_canonical_order"]
    require(isinstance(native_weights, list) and len(native_weights) == context.n,
            "native proposal has incorrect canonical shape")
    with localcontext() as ctx:
        ctx.prec = 70
        values = [dec(value) for value in native_weights]
        frozen = [dec(value) for value in oracle["weights"]]
        differences = [abs(a - b) for a, b in zip(values, frozen)]
        maximum = max(differences)
        index = differences.index(maximum)
        require(maximum <= WEIGHT_TOL, f"native startup weights differ: {maximum}")
        union_set = set(union)
        require(all(value == 0 for i, value in enumerate(values) if i not in union_set),
                "native proposal opens a risk-unready or ineligible name")
        objective = sum(Decimal("0.5") * (value - dec(preference)) ** 2 + risk * value ** 2
                        for value, preference, risk in zip(values, preferences.columns["weight"][0],
                                                           exact_variance))
        objective_diff = abs(objective - Decimal(oracle["certificate"]["primal_objective_full_canonical"]))
        reported_objective_diff = abs(objective - dec(first["objective"]))
        require(objective_diff <= OBJECTIVE_TOL and reported_objective_diff <= OBJECTIVE_TOL,
                "native startup objective differs from independent recomputation/oracle")
        marks = evaluation.columns["close"][1]
        dollars = []
        for i, weight in enumerate(native_weights):
            value = 0.0
            if weight != 0:
                mark = float(marks[i])
                require(math.isfinite(mark) and mark > 0, f"missing proposed mark: instrument={i}")
                desired = float(weight) * 100_000_000.0
                units = desired / mark
                value = units * mark
                require(all(math.isfinite(v) and v != 0 for v in (desired, units, value)),
                        "unrepresentable native proposal")
            dollars.append(dec(value))
        nav = Decimal(100_000_000)
        assets = sum(dollars)
        traded = sum(map(abs, dollars))
        fee = traded * Decimal("0.0005")
        post_cash = nav - assets - fee
        post_nav = post_cash + assets
        reconstructed = {"traded_dollars": traded, "trade_cost": fee,
            "posttrade_cash": post_cash, "posttrade_nav": post_nav,
            "actual_turnover": traded / nav, "postfee_net": assets / post_nav,
            "postfee_gross": traded / post_nav,
            "postfee_max_name": max(map(abs, dollars)) / post_nav,
            "requested_prefee_net": sum(values), "requested_prefee_gross": sum(map(abs, values)),
            "requested_turnover": sum(map(abs, values))}
        errors = {}
        for key, expected in reconstructed.items():
            error = abs(expected - dec(first[key]))
            tolerance = MONEY_TOL if key in {"traded_dollars", "trade_cost", "posttrade_cash",
                                             "posttrade_nav"} else Decimal("1e-12")
            require(error <= tolerance, f"native {key} does not reconcile: {error}")
            errors[key] = str(error)
        require(reconstructed["actual_turnover"] <= Decimal("0.2") + ECONOMIC_TOL and
                abs(reconstructed["postfee_net"]) <= ECONOMIC_TOL and
                reconstructed["postfee_gross"] <= 1 + ECONOMIC_TOL and
                reconstructed["postfee_max_name"] <= Decimal("0.01") + ECONOMIC_TOL,
                "representable native portfolio violates original economic limits")
        numeric = {"canonical_weights_compared": len(values),
            "max_abs_weight_difference": str(maximum), "max_difference_instrument_index": index,
            "max_difference_security_id": oracle["instrument_ids"][index],
            "l1_weight_difference": str(sum(differences)), "weight_tolerance": str(WEIGHT_TOL),
            "independent_objective": str(objective), "objective_vs_oracle_difference": str(objective_diff),
            "native_reported_objective_difference": str(reported_objective_diff),
            "objective_tolerance": str(OBJECTIVE_TOL),
            "representable_execution": {key: str(value) for key, value in reconstructed.items()},
            "reported_execution_absolute_errors": errors, "money_tolerance": str(MONEY_TOL),
            "original_economic_tolerance": str(ECONOMIC_TOL),
            "accounting_basis": "binary64 TRI sizing/products then 70-digit Decimal sums; no PnL"}
    require(all(digest(name) == expected for name, expected in HASHES.items()),
            "comparison input/evidence changed while checking")
    return {"status": "passed-first-proposal-comparison", "numeric": numeric,
        "risk_stats": risk_stats, "union_instruments": len(union),
        "native_proposal_count": len(proposals["decisions"]),
        "native_proposal_status": proposals["status"], "native_measurement_status": measurement["status"],
        "native_exit_code": measurement["exit_code"], "native_producer_executable_sha256": exe_sha,
        "native_solver": {key: first[key] for key in ("polished", "primal_residual", "dual_residual",
                                                       "effective_solver_feasibility_tolerance")},
        "oracle_sha256": ORACLE_SHA, "input_hashes": dict(HASHES), "all_pins_unchanged": True,
        "scope": "first all-cash proposal numerical comparison only; no later replay or PnL acceptance"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-root", type=Path, default=DEFAULT_NATIVE)
    parser.add_argument("--measurement", type=Path,
                        default=AUDITS / "iteration9-equity-book-measurement.json")
    parser.add_argument("--output", type=Path,
                        default=AUDITS / "iteration9-startup-native-comparison.json")
    args = parser.parse_args()
    require(not args.output.exists(), f"refusing to overwrite comparison: {args.output}")
    started = time.perf_counter()
    result = {"schema": "atx.iteration9-startup-native-comparison-v1", "status": "failed",
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
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ("input_hashes", "risk_stats")}, sort_keys=True), flush=True)
    return 0 if result["status"] == "passed-first-proposal-comparison" else 1


if __name__ == "__main__":
    raise SystemExit(main())
