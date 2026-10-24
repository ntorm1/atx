"""Independent component checks. No whole-fit or generic accuracy qualification."""
import argparse
import collections
import json
import math
from pathlib import Path

from replay import CLASSES, number, requests, require, validate_value


def summarize(source, report):
    rs = requests(source)
    require(report.get("schema") == "atx.cold-seed.empirical-component.v1", "schema")
    require(type(report["request_count"]) is int and report["request_count"] == len(rs), "count")
    require(type(report["rows"]) is list and len(report["rows"]) == len(rs), "row denominator")
    reps = report["repetitions"]
    require(type(reps) is int and 1 <= reps <= 1000, "repetitions")
    groups = collections.defaultdict(list)
    result = dict(requests=len(rs), vector_admitted=0, scalar_admitted=0, refused=0,
                  retries={}, max_vector_scalar_difference=0., max_cold_residual=0.,
                  max_allowance_ratio=0., min_boundary_distance=None, failures=[],
                  whole_fit_qualified=False, generic_error_bound_certified=False,
                  timing_qualified=False)
    distances = []
    retry_counts = collections.Counter()
    for r, row in zip(rs, report["rows"]):
        require(row["request"] == r, "request join")
        require(type(row["origin"]) is int and row["origin"] in (0, 1, 2), "origin")
        require(type(row["retry"]) is int and 0 <= row["retry"] <= 9, "retry")
        require(type(row["scalar_full_proof"]) is bool, "scalar proof")
        require(row["class"] in CLASSES, "class")
        require(number(row["allowance"]) and row["allowance"] >= 0, "allowance")
        for k in ("sigma", "vector_price", "boundary", "distance", "scalar_floor"):
            require(row[k] is None or number(row[k]), "numeric " + k)
        validate_value(row["scalar"], r)
        accepted = row["sigma"] is not None
        require(accepted == (row["origin"] != 0), "origin/acceptance")
        if accepted:
            require(row["sigma"] == r["sigma"], "sigma changed")
        require((row["retry"] == 0) == (row["origin"] == 2), "retry/origin")
        retry_counts[row["retry"]] += 1
        result[{0: "refused", 1: "scalar_admitted", 2: "vector_admitted"}[row["origin"]]] += 1
        reasons = []
        if accepted != row["scalar_full_proof"]: reasons.append("scalar_decision_changed")
        if row["scalar_full_proof"] != r["accepted"]: reasons.append("captured_decision_changed")
        if accepted:
            p = row["scalar"]["price"]
            if p is None or abs(p - r["target"]) > r["budget"]:
                reasons.append("cold_price_budget")
            if row["scalar_floor"] is None or not r["target"] > row["scalar_floor"]:
                reasons.append("cold_floor")
            if row["scalar"]["class"] in ("unsupported_regime", "exercise_plateau"):
                reasons.append("scalar_classification")
            if p is not None:
                result["max_cold_residual"] = max(result["max_cold_residual"], abs(p-r["target"]))
        if row["origin"] == 2:
            require(row["class"] == "continuation", "vector continuation")
            require(number(row["vector_price"]) and number(row["boundary"]) and
                    row["boundary"] > 0 and number(row["distance"]), "vector evidence")
            require(r["r"] >= 0 and r["q"] >= 0 and r["sigma"] > .005, "vector scope")
            require([r[k] for k in ("n_collocation", "n_quadrature", "n_quad_price",
                    "max_newton_iter", "opts_tol", "profile")] == [12,24,48,6,1e-10,0], "scheme")
            distance = math.log((r["K"] if r["side"] == 0 else r["S"]) / row["boundary"]) / (
                r["sigma"] * math.sqrt(r["T"]))
            require(abs(distance-row["distance"]) <= 1e-12*max(1., abs(distance)) and
                    distance >= .25, "boundary evidence")
            allowance = 1e-11 * max(1., r["S"], r["K"], abs(r["target"]), abs(row["vector_price"]))
            require(row["allowance"] == allowance and abs(row["vector_price"]-r["target"]) +
                    allowance <= r["budget"], "empirical margin")
            distances.append(distance)
            p = row["scalar"]["price"]
            if p is None:
                reasons.append("no_scalar_reference")
            else:
                error = abs(row["vector_price"]-p)
                result["max_vector_scalar_difference"] = max(result["max_vector_scalar_difference"], error)
                result["max_allowance_ratio"] = max(result["max_allowance_ratio"], error/allowance)
                if error > allowance: reasons.append("empirical_allowance_exceeded")
        if reasons:
            result["failures"].append(dict(batch=r["batch"], index=r["index"], reasons=reasons))
        groups[r["batch"]].append(row)
    seen = set()
    clocks = [0] * reps
    for t in report["timings"]:
        for k in ("rep", "batch", "requests", "vector_packs", "vector_lanes", "vector_admitted",
                  "scalar_fallbacks", "scalar_admitted", "floor_preparations", "floor_hits",
                  "vector_ns", "scalar_ns", "floor_ns", "inclusive_ns"):
            require(type(t[k]) is int and t[k] >= 0, "counter " + k)
        key = t["rep"], t["batch"]
        require(0 <= key[0] < reps and key[1] in groups and key not in seen, "timing identity")
        seen.add(key)
        rows = groups[key[1]]
        require(t["requests"] == len(rows) and t["vector_lanes"] == 4*t["vector_packs"], "work count")
        require(t["vector_admitted"] == sum(r["origin"] == 2 for r in rows), "admissions")
        require(t["scalar_admitted"] == sum(r["origin"] == 1 for r in rows), "scalar admissions")
        require(t["scalar_fallbacks"] + t["vector_admitted"] == len(rows), "fallback denominator")
        require(t["vector_admitted"] <= t["vector_lanes"] <= len(rows), "vector attempts")
        require(t["inclusive_ns"] >= sum(t[k] for k in ("vector_ns", "scalar_ns", "floor_ns")), "clock parts")
        clocks[key[0]] += t["inclusive_ns"]
    require(len(seen) == reps * len(groups), "timing denominator")
    result["retries"] = dict(sorted(retry_counts.items()))
    result["min_boundary_distance"] = min(distances) if distances else None
    result["component_ns_unqualified"] = clocks
    result["component_correctness_passed"] = not result["failures"]
    return result


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path); ap.add_argument("native", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    result = summarize(json.loads(args.input.read_text()), json.loads(args.native.read_text()))
    with args.output.open("x", encoding="utf-8") as f:
        json.dump(result, f, indent=2); f.write("\n")
    if not result["component_correctness_passed"]: raise SystemExit(1)
