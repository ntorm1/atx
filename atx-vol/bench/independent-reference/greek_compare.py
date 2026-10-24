"""Verify fixed reference receipts, then retain every native candidate comparison."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import re
import subprocess
import sys

from greek_study import CONFIGS, SCHEMES, digest, summarize_reference


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite(x):
    return type(x) in (int, float) and math.isfinite(x)


def verify_row(row, model):
    require(row["id"] == model["id"] and row["model"] == model, "exact model join")
    require(row["scheme"] in SCHEMES and row["label"] in CONFIGS, "fixed scheme/label")
    nx, nt, domain = CONFIGS[row["label"]]
    require(row["options"] == dict(nx=nx, nt=nt, domain_scale=domain), "outer configuration")
    if row["status"] == "refused":
        require("result" not in row and row["reason"] in
                ("ValueError", "ArithmeticError", "OverflowError", "FloatingPointError") and
                type(row["detail"]) is str and bool(row["detail"]), "truthful typed refusal")
        return
    require(row["status"] == "computed", "outcome status")
    r = row["result"]
    require((r["nx"], r["nt"], r["domain_scale"], r["scheme"]) ==
            (nx, nt, domain, row["scheme"]), "inner configuration")
    require(r["american"] is True and r["accuracy_established"] is False and
            r["greek_accuracy_established"] is False, "reference scope")
    s, k, t, sigma, rate, carry = (model[key] for key in ("S", "K", "T", "sigma", "r", "q"))
    width = domain*max(.1, 8*sigma*math.sqrt(t)+abs((rate-carry-.5*sigma*sigma)*t),
                       abs(math.log(k)-math.log(s))+.1)
    dx = 2*width/(nx-1)
    require(r["log_half_width"] == width and r["log_spacing"] == dx and
            r["lcp_tolerance"] == 1e-12*max(1., s, k), "frozen geometry and LCP tolerance")
    require(type(r["domain"]) is list and len(r["domain"]) == 2 and all(finite(x) for x in r["domain"]),
            "finite domain endpoints")
    for actual, expected in zip(r["domain"], (s*math.exp(-width), s*math.exp(width))):
        require(abs(actual-expected) <= 16*sys.float_info.epsilon*max(1., abs(expected)), "domain extent")
    stages = nt if row["scheme"] == "cn" else 2*nt-2
    require(all(type(r[key]) is int and r[key] >= 0 for key in
                ("stages", "stage_node_updates", "policy_iterations", "max_policy_iterations")), "work integers")
    require(r["stages"] == stages and r["stage_node_updates"] == stages*(nx-2) and
            r["max_policy_iterations"] <= nx and
            r["max_policy_iterations"] <= r["policy_iterations"] <= stages*r["max_policy_iterations"],
            "fixed stage and bounded policy work")
    require(finite(r["price"]) and finite(r["log_spacing"]) and r["log_spacing"] > 0 and
            finite(r["lcp_tolerance"]) and r["lcp_tolerance"] > 0 and
            finite(r["max_complementarity_residual"]) and
            0 <= r["max_complementarity_residual"] <= r["lcp_tolerance"], "numeric diagnostics")
    arrays = [r[k] for k in ("stencil_spots", "stencil_values", "stencil_payoff", "stencil_continuation")]
    require(all(type(a) is list and len(a) == 3 and all(finite(x) for x in a) for a in arrays),
            "finite complete stencil")
    spots, values, payoff, continuation = arrays
    tol = r["lcp_tolerance"]
    require(spots[1] == s and r["price"] == values[1], "exact center node")
    for offset, point, value in zip((-1, 0, 1), spots, payoff):
        require(abs(point-s*math.exp(offset*dx)) <= 16*sys.float_info.epsilon*max(1., abs(point)),
                "spot stencil geometry")
        expected = max(point-k if model["side"] == "call" else k-point, 0.)
        require(value == expected, "actual intrinsic evidence")
    states = []
    for value, floor, residual in zip(values, payoff, continuation):
        gap = value-floor
        states.append("exercise" if abs(gap) <= tol and residual > tol and floor > 0 else
                      "continuation" if gap > tol and abs(residual) <= tol else "ambiguous")
    unique = set(states)
    regime = "ambiguous" if "ambiguous" in unique else "mixed" if len(unique) > 1 else states[0]
    require(r["regime"] == regime, "actual discrete regime")
    if regime in ("mixed", "ambiguous"):
        require(r["delta"] is None and r["gamma"] is None, "unresolved derivative representation")
    elif regime == "exercise":
        require(r["delta"] == (1. if model["side"] == "call" else -1.) and r["gamma"] == 0.,
                "affine exercised derivatives")
    else:
        vx = (values[2]-values[0])/(2*dx)
        expected = dict(delta=vx/s, gamma=((values[2]-2*values[1]+values[0])/dx**2-vx)/s**2)
        p = 32*sys.float_info.epsilon*max(1., *map(abs, values))
        allowance = dict(delta=p/(s*dx), gamma=p*(4/dx**2+1/dx)/s**2)
        for field in ("delta", "gamma"):
            require(finite(r[field]) and abs(r[field]-expected[field]) <= allowance[field],
                    "derivative stencil reconstruction")


def read_json(path):
    def invalid(value):
        raise ValueError("Nonfinite JSON token: "+value)
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=invalid)


def verify_pilot(directory):
    manifest = read_json(directory/"manifest.json")
    require(manifest["schema"] == "atx-independent-greek-pilot-v1" and manifest["completed"] is True and
            manifest["source_unchanged"] is True, "completed immutable pilot")
    require(all(manifest[k] is False for k in
                ("accuracy_established", "full_bundle_qualified", "timing_qualified")), "unqualified scope")
    require(re.fullmatch("[0-9a-f]{40}", manifest["source"]), "source commit")
    require(manifest["requested_results"] == 216 and manifest["input_count"] == 715 and
            manifest["schemes"] == list(SCHEMES) and
            manifest["configurations"] == {k: list(v) for k, v in CONFIGS.items()}, "declared population")
    required = {"greek_reference.py", "greek_study.py", "pde_reference.py", "greek-plan-v1.md",
                "test_greek_reference.py", "test_greek_study.py", "corpus-v1.json", "refinement-v1.json",
                "risk-contract-proposal-v1.json"}
    hashes = manifest["source_hashes"]
    require(len(hashes) == len(required) and {Path(p).name for p in hashes} == required, "source denominator")
    root = Path(__file__).resolve().parents[3]
    for name, expected_hash in hashes.items():
        source = directory/"sources"/Path(name).name
        require(digest(source) == expected_hash, "archived source hash")
        git_name = name.replace("\\", "/")  # Git object paths always use forward slashes.
        committed = subprocess.check_output(["git", "show", manifest["source"]+":"+git_name], cwd=root)
        require(source.read_bytes().replace(b"\r\n", b"\n") == committed.replace(b"\r\n", b"\n"),
                "archive source matches frozen Git commit")
    for name in ("outcomes", "summary"):
        suffix = ".jsonl" if name == "outcomes" else ".json"
        require(digest(directory/(name+suffix)) == manifest[name+"_sha256"], "result hash")
    corpus = read_json(directory/"sources/corpus-v1.json")
    selection = read_json(directory/"sources/refinement-v1.json")
    models = {c["id"]: c for c in corpus["cases"]}
    require(len(models) == len(corpus["cases"]) == 715 and len(set(selection)) == len(selection) == 12 and
            manifest["selected_ids"] == selection and set(selection) <= set(models), "frozen population")
    require(manifest["uncomputed_ids"] == [c["id"] for c in corpus["cases"] if c["id"] not in selection],
            "all uncomputed contracts retained")
    rows = [json.loads(line) for line in (directory/"outcomes.jsonl").read_text(encoding="utf-8").splitlines()]
    wanted = {(i, s, label) for i in selection for s in SCHEMES for label in CONFIGS}
    require(len(rows) == 216 and {(r["id"], r["scheme"], r["label"]) for r in rows} == wanted,
            "every fixed request exactly once")
    for row in rows:
        verify_row(row, models[row["id"]])
    require(manifest["computed"] == sum(r["status"] == "computed" for r in rows) and
            manifest["refused"] == sum(r["status"] == "refused" for r in rows), "status denominator")
    summary = read_json(directory/"summary.json")
    recomputed = {i: summarize_reference(models[i], [r for r in rows if r["id"] == i]) for i in selection}
    require(summary["fields"] == recomputed and summary["candidate_comparison_performed"] is False and
            summary["full_bundle_qualified"] is False and summary["other_cold_fields"] ==
            {f: "uncomputed" for f in ("vega", "theta", "rho", "vanna", "volga", "charm")}, "decision reproduction")
    return corpus, selection, recomputed


def compare(pilot, native_path):
    corpus, selection, references = verify_pilot(pilot)
    native_manifest = read_json(native_path.parent/"manifest.json")
    require(digest(native_path) == native_manifest["files"][native_path.name], "original native receipt hash")
    native = read_json(native_path)
    require(native["selected_ids"] == selection, "native exact pilot selection")
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent/"american-greeks"))
    import fd_refinement
    fd_refinement.summarize(native, corpus)  # Revalidate actual stencils, options, masks and all48 evaluations.
    outcomes, totals = [], Counter()
    for case in native["cases"]:
        model = case["input"]
        for evaluation in case["evaluations"]:
            for index, field in enumerate(("delta", "gamma")):
                reference = references[model["id"]][field]
                supplied = evaluation["result_success"] and bool(evaluation["supported"] & (1 << index))
                value = evaluation["values"][index] if supplied else None
                for shock, fraction in (("operational", .001), ("stress", .01)):
                    ds = fraction*model["S"]
                    coefficient = ds if field == "delta" else .5*ds*ds
                    available = reference["status"] == "empirically_resolved"
                    error = abs(value-reference["center"]) if supplied and reference["center"] is not None else None
                    inclusive = (error+reference["uncertainty"])*coefficient if error is not None else None
                    status = "candidate_refused" if not supplied else "reference_unresolved" if not available else \
                             "pass" if inclusive <= .0001 else "fail"
                    outcomes.append(dict(id=model["id"], profile=evaluation["profile"], design=evaluation["design"],
                        scale=evaluation["scale"], field=field, shock=shock, supplied=supplied, value=value,
                        reference_status=reference["status"], reference_center=reference["center"],
                        reference_uncertainty=reference["uncertainty"], raw_abs_error=error,
                        discrepancy_plus_uncertainty_usd=inclusive, status=status))
                    totals[status] += 1
    require(len(outcomes) == 2304, "complete candidate comparison denominator")
    return dict(schema="atx-independent-greek-comparison-v1", pilot_manifest_sha256=digest(pilot/"manifest.json"),
        native_sha256=digest(native_path), source_sha256=digest(__file__),
        native_verifier_sha256=digest(fd_refinement.__file__), reference_summaries=references,
        requested_comparisons=2304, outcomes=outcomes, totals=dict(totals),
        other_six_field_comparisons_uncomputed=6912, unselected_contracts_uncomputed=703,
        price_budget_implies_greek_contract=False, full_bundle_qualified=False, timing_qualified=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pilot", required=True, type=Path)
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = compare(args.pilot, args.native)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(report["totals"], sort_keys=True))
