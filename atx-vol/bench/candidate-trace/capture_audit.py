"""Independent structural/denominator audit of the exact six-phase capture.

This validates recorded work and control flow, not the numerical price proof.
The scalar actual-API audit remains mandatory and separate. Clocks are diagnostic.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

PHASES = ["ScreenProposal", "ScreenProof", "FastProposal", "FastProof", "CompleteFast", "RetryProof"]
SPY_INPUT = "b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e"
SPY_POLICY = "781ddc09f7f26cfbfeb6c32edae1e3591b725a8f8d8d5b85d18eeda00b23f34a"


def require(condition, context):
    if not condition:
        raise ValueError(context)


def integer(value):
    require(type(value) is int and 0 <= value <= 2**64 - 1, "invalid unsigned count")
    return value


def boolean(value):
    require(type(value) is bool, "invalid boolean")
    return value


def real(value):
    require(type(value) is dict and set(value) == {"value", "bits"}, "real encoding")
    bits = value["bits"]
    require(type(bits) is str and len(bits) == 16 and all(c in "0123456789abcdef" for c in bits),
            "binary64 bits")
    result = struct.unpack(">d", bytes.fromhex(bits))[0]
    if math.isfinite(result):
        require(type(value["value"]) in (int, float) and
                struct.pack(">d", value["value"]).hex() == bits, "decimal/bits mismatch")
    else:
        require(value["value"] is None, "nonfinite decimal must be null")
    return result


def encoded(value):
    return {"value": value, "bits": struct.pack(">d", value).hex()}


def options(value):
    if value is None:
        return
    require(set(value) == {"n_collocation", "n_quadrature", "n_quad_price", "max_newton_iter", "tol", "profile"},
            "AL option fields")
    for key in ("n_collocation", "n_quadrature", "n_quad_price", "max_newton_iter"):
        require(integer(value[key]) <= 65535, "AL option width")
    require(integer(value["profile"]) <= 2, "AL profile")
    real(value["tol"])


def fast_options(raw):
    return dict(raw or {"n_collocation": 12, "n_quadrature": 24, "n_quad_price": 48,
                       "max_newton_iter": 6, "tol": encoded(1e-10)}, profile=1)


SCREEN = {"n_collocation": 7, "n_quadrature": 16, "n_quad_price": 48,
          "max_newton_iter": 5, "tol": encoded(1e-8), "profile": 2}


def floor_state(value):
    integer(value["preparations"]); integer(value["hits"])
    require(len(value["slots"]) == 2, "floor slot count")
    for i, slot in enumerate(value["slots"]):
        prepared = boolean(slot["prepared"])
        for key in ("anchor", "T", "r", "q"):
            number = real(slot[key])
            require(math.isfinite(number), "nonfinite floor key")
            if not prepared:
                require(slot[key] == encoded(0.), "unprepared floor has stale key")
        options(slot["options"])
        require(integer(slot["side"]) == (i if prepared else 1), "floor side/slot mismatch")
        if not prepared:
            require(slot["options"] is None, "unprepared floor has options")
    require(value["preparations"] >= sum(s["prepared"] for s in value["slots"]), "floor preparation census")


def usable(phase):
    return phase["outcome"] == 1 and phase["sigma"] is not None and \
        math.isfinite(real(phase["sigma"])) and real(phase["sigma"]) > .005


def validate_trace(trace):
    require(boolean(trace["complete"]), "incomplete trace")
    request, opts, phases = trace["request"], trace["options"], trace["phases"]
    require(set(request) == {"price", "S", "K", "T", "r", "q", "budget", "side"}, "request fields")
    numbers = {key: real(request[key]) for key in request if key != "side"}
    require(integer(request["side"]) <= 1, "request side")
    options(opts["al_options"])
    for key in ("tol", "warm_start", "price_tol"):
        real(opts[key])
    require(integer(opts["method"]) <= 1 and integer(opts["strategy"]) <= 1, "inverse enums")
    require(integer(opts["max_iter"]) <= 65535, "inverse iteration width")
    boolean(opts["has_correction"]); boolean(opts["refuse_non_monotone"])
    if opts["seed_profile"] is not None:
        require(integer(opts["seed_profile"]) <= 2, "inverse seed profile")
    require(integer(trace["kernel"]) <= 1 and len(phases) == 6, "kernel/phase identity")
    for i, phase in enumerate(phases):
        require(integer(phase["phase"]) == i, "phase order")
        visited = boolean(phase["visited"])
        require(boolean(phase["complete"]) == visited, "incomplete phase")
        boolean(phase["stopped_early"])
        outcome = integer(phase["outcome"])
        require(outcome <= 4 and (outcome != 0) == visited, "phase outcome")
        integer(phase["elapsed_ns"])
        require(len(phase["work"]) == 3, "work axes")
        for work in phase["work"]:
            integer(work)
        for key in ("floor_preparations", "floor_hits"):
            integer(phase[key])
        for key in ("sigma", "seed_sigma"):
            if phase[key] is not None:
                real(phase[key])
        for key in ("error", "typed_status"):
            if phase[key] is not None:
                integer(phase[key])
        options(phase["options"])
        if not visited:
            require(not phase["stopped_early"] and phase["elapsed_ns"] == 0 and phase["work"] == [0, 0, 0] and
                    phase["floor_preparations"] == phase["floor_hits"] == 0 and
                    all(phase[key] is None for key in ("sigma", "seed_sigma", "error", "typed_status", "options")),
                    "unvisited phase has work/result")
            continue
        expected_options = SCREEN if i == 0 else fast_options(opts["al_options"]) if i in (2, 4) else opts["al_options"]
        require(phase["options"] == expected_options, "wrong phase option scheme")
        require(not phase["stopped_early"] or i in (0, 2), "wrong early-stop phase")
        if i in (1, 3, 5):
            require(phase["seed_sigma"] == phases[i - 1]["sigma"] and usable(phases[i - 1]), "proof seed")
            require(phase["error"] is None and phase["typed_status"] is None and outcome in (1, 2, 4), "proof result")
            require((phase["sigma"] is not None) == (outcome == 1), "proof optional result")
            if outcome == 1:
                require(phase["sigma"] == phase["seed_sigma"], "proof changed proposed sigma")
        else:
            require(phase["seed_sigma"] is None, "inverse has seed sigma")
            if i in (0, 2):
                require(outcome in (1, 3, 4) and phase["typed_status"] is None, "proposal result type")
                require((phase["error"] is not None) == (outcome == 3), "proposal error")
                require((phase["sigma"] is not None) == (outcome == 1), "proposal sigma")
            elif outcome != 4:
                require(outcome in (1, 2) and phase["typed_status"] is not None and
                        phase["typed_status"] <= 5 and (phase["typed_status"] == 0) == (outcome == 1) and
                        phase["sigma"] is not None and phase["error"] is None, "typed full retry")
        if outcome == 4:
            require(all(phase[key] is None for key in ("sigma", "error", "typed_status")), "allocation failure invented result")

    controls = numbers["budget"] > 0 and math.isfinite(numbers["budget"]) and opts["method"] == 0 and \
        not opts["has_correction"] and real(opts["warm_start"]) == 0 and real(opts["price_tol"]) == 0
    visited = []
    expected_outcome, sigma = 3, None

    def visit(i):
        visited.append(i)
        require(phases[i]["visited"], "missing required phase")
        return phases[i]

    def proposal_and_proof(i):
        p = visit(i)
        if p["outcome"] == 4:
            return 4, None
        if usable(p):
            proof = visit(i + 1)
            if proof["outcome"] == 4:
                return 4, None
            if proof["outcome"] == 1:
                return 2, proof["sigma"]
        return 3, None

    if not controls:
        expected_outcome = 1
    else:
        if trace["kernel"] == 1:
            expected_outcome, sigma = proposal_and_proof(0)
        if expected_outcome == 3:
            expected_outcome, sigma = proposal_and_proof(2)
            if expected_outcome == 3 and phases[2]["stopped_early"]:
                p = visit(4)
                if p["outcome"] == 4:
                    expected_outcome = 4
                elif usable(p):
                    proof = visit(5)
                    expected_outcome = 4 if proof["outcome"] == 4 else 2 if proof["outcome"] == 1 else 3
                    sigma = proof["sigma"] if expected_outcome == 2 else None
    require(visited == [i for i, p in enumerate(phases) if p["visited"]], "fabricated phase after short circuit")
    require(integer(trace["outcome"]) == expected_outcome and trace["sigma"] == sigma, "whole decision mismatch")
    stats = {"screen_attempted": controls and trace["kernel"] == 1,
             "screen_stopped_early": phases[0]["stopped_early"] if phases[0]["outcome"] != 4 else False,
             "screen_accepted": phases[1]["outcome"] == 1,
             "stopped_early": phases[2]["stopped_early"] if phases[2]["outcome"] != 4 else False,
             "accepted_early": phases[3]["outcome"] == 1 and phases[2]["stopped_early"],
             "retried_full_search": phases[4]["visited"]}
    require(set(trace["stats"]) == set(stats), "decision flags")
    require(all(boolean(trace["stats"][key]) == value for key, value in stats.items()), "decision flag mismatch")
    require(len(trace["work"]) == 3 and all(integer(trace["work"][i]) == sum(p["work"][i] for p in phases)
                                          for i in range(3)), "solve ledger phase sum")
    require(integer(trace["elapsed_ns"]) >= sum(p["elapsed_ns"] for p in phases), "nested clock sum")
    before, after = trace["floor_before"], trace["floor_after"]
    floor_state(before); floor_state(after)
    for field, key in (("floor_preparations", "preparations"), ("floor_hits", "hits")):
        require(integer(trace[field]) == sum(p[field] for p in phases) == after[key] - before[key], "floor delta mismatch")
    cache_present = boolean(trace["floor_cache_present"])
    if not cache_present:
        require(before["preparations"] == before["hits"] == after["preparations"] == after["hits"] == 0,
                "null cache has work")
    elif expected_outcome == 2:
        slot = after["slots"][request["side"]]
        require(slot["prepared"] and slot["anchor"] == request["S" if request["side"] == 0 else "K"] and
                all(slot[key] == request[key] for key in ("T", "r", "q")) and
                slot["options"] == opts["al_options"], "accepted floor model mismatch")
    return expected_outcome, stats


def validate_capture(value):
    require(value["schema"] == "atx-price-candidate-capture-v1" and value["phase_order"] == PHASES, "capture schema")
    require(value["timing_qualifying"] is False and value["capture_enabled"] is True and value["valid"] is True,
            "unqualified/incomplete capture")
    require(value["record_overflow"] is False and value["cache_overflow"] is False, "capture overflow")
    records, caches = value["records"], value["caches"]
    count = integer(value["attempted_calls"])
    require(count > 0 and count == len(records) <= integer(value["record_capacity"]), "call denominator")
    require(len(caches) <= integer(value["cache_capacity"]), "cache denominator")
    by_id, previous = {}, {}
    for i, cache in enumerate(caches, 1):
        require(integer(cache["id"]) == i and integer(cache["address"]) > 0 and cache["closed"] is True, "cache identity")
        require(integer(cache["created_before_call"]) <= integer(cache["closed_after_calls"]) <= count, "cache interval")
        floor_state(cache["final_state"])
        for old in by_id.values():
            if old["address"] == cache["address"]:
                require(old["closed_after_calls"] <= cache["created_before_call"], "overlapping reused cache address")
        by_id[i] = cache
    counts = {"reimply_candidate_attempts": count, "reimply_candidate_roots": 0,
              "reimply_screen_attempts": 0, "reimply_screen_stops": 0, "reimply_screen_roots": 0,
              "reimply_proposal_stops": 0, "reimply_proposal_roots": 0, "reimply_proposal_retries": 0}
    mapping = {"screen_attempted": "reimply_screen_attempts", "screen_stopped_early": "reimply_screen_stops",
               "screen_accepted": "reimply_screen_roots", "stopped_early": "reimply_proposal_stops",
               "accepted_early": "reimply_proposal_roots", "retried_full_search": "reimply_proposal_retries"}
    phase_calls, phase_ns = [0] * 6, [0] * 6
    phase_work = [[0] * 3 for _ in range(6)]
    models = set()
    for i, record in enumerate(records):
        require(integer(record["sequence"]) == i, "call identity")
        trace = record["trace"]
        outcome, stats = validate_trace(trace)
        cache_id = integer(record["cache_id"])
        require((cache_id != 0) == trace["floor_cache_present"], "cache presence")
        if cache_id:
            cache = by_id[cache_id]
            require(cache["created_before_call"] <= i < cache["closed_after_calls"], "call outside owner lifetime")
            for key in ("preparations", "hits"):
                require(previous.get(cache_id, {}).get(key, 0) <= trace["floor_before"][key] <=
                        trace["floor_after"][key] <= cache["final_state"][key], "cache census reversed")
            previous[cache_id] = trace["floor_after"]
        counts["reimply_candidate_roots"] += outcome == 2
        for key, name in mapping.items():
            counts[name] += stats[key]
        for j, phase in enumerate(trace["phases"]):
            phase_calls[j] += phase["visited"]
            phase_ns[j] += phase["elapsed_ns"]
            for k in range(3):
                phase_work[j][k] += phase["work"][k]
        request = trace["request"]
        models.add(tuple(request[key]["bits"] for key in ("S", "K", "T", "r", "q")) +
                   (request["side"], json.dumps(trace["options"]["al_options"], sort_keys=True)))
    slices = value["slice_counters"]["slices"]
    require(type(slices) is list and slices, "missing independent fit counters")
    observed = {key: sum(integer(entry["timing"][key]) for entry in slices) for key in counts}
    require(observed == counts, "whole-fit counter join")
    return {"schema": "atx-price-candidate-summary-v1", "timing_qualifying": False,
            "attempts": count, "accepted": counts["reimply_candidate_roots"],
            "refused": count - counts["reimply_candidate_roots"], "cache_lifetimes": len(caches),
            "model_key_count": len(models), "phase_order": PHASES, "phase_calls": phase_calls,
            "phase_ns": phase_ns, "phase_work": phase_work,
            "inclusive_observed_ns": sum(r["trace"]["elapsed_ns"] for r in records), "fit_counters": counts}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_json(path):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate JSON key")
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding="utf-8-sig"), object_pairs_hook=pairs)


def main():
    parser = argparse.ArgumentParser()
    for name in ("capture", "predictions", "report", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--spy", action="store_true")
    args = parser.parse_args()
    require(not args.out.exists(), "output already exists")
    value, report = read_json(args.capture), read_json(args.report)
    summary = validate_capture(value)
    require(sha256(args.capture) == report["bench"]["candidate_capture"]["sha256"], "native capture hash")
    require(sha256(args.predictions) == value["prediction_sha256"], "prediction hash")
    for key in ("build_info", "process_identity", "per_rep"):
        require(value["fit_report"][key] == report["bench"][key], "native report join")
    require(value["slice_counters"] == report["graded_stats"], "independent slice counters changed")
    if args.spy:
        require(value["input_sha256"] == SPY_INPUT and value["policy_sha256"] == SPY_POLICY,
                "frozen SPY input/policy")
        require((summary["attempts"], summary["accepted"], summary["refused"]) == (700, 684, 16),
                "frozen SPY denominator changed")
    summary["capture_sha256"] = sha256(args.capture)
    summary["prediction_sha256"] = sha256(args.predictions)
    summary["report_sha256"] = sha256(args.report)
    args.out.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
