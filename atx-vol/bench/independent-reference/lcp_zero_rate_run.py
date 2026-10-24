"""Capture or verify the fixed twelve zero-rate controls; never promote a pilot."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

from greek_compare import read_json, require, verify_pilot
from greek_study import digest
from lcp_zero_rate_control import MODE, MODEL, fixed_requests, summarize_control


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PREFIX = "atx-vol/bench/independent-reference/"
BASELINE_HASHES = {
    "manifest.json": "cd13a2d26c1cfea6356a87a4a0a6a9d9e7f474a663851367b24f802347f9ec25",
    "outcomes.jsonl": "f93caa9032237d362bd969bb518e1bb0d988a78bf0e3b4d9d7ed2a510dbfd4b3",
    "summary.json": "34fd09b562e97d9f18a2c3c58377dcee701ff7e2d9d46155e9dac161e2cecd9f",
}
PLAN = "9641ef8d"
PRIMITIVE = "17ab5fbfd177d728c2abe2d6e2d822a9e802571f"
FROZEN_FILES = (("lcp_correction.py", PRIMITIVE), ("pde_reference.py", "5df44c59"),
                ("greek_reference.py", "5df44c59"),
                ("zero-rate-lcp-correction-plan-v1.md", PLAN))
SOURCES = (
    "lcp_zero_rate_run.py", "lcp_zero_rate_control.py", "lcp_stage_adapter.py",
    "lcp_correction.py", "greek_reference.py", "pde_reference.py", "greek_study.py",
    "greek_compare.py", "zero-rate-lcp-correction-plan-v1.md", "greek-plan-v2.md",
    "test_lcp_zero_rate_control.py", "test_lcp_stage_adapter.py", "test_lcp_zero_rate_run.py",
)


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT)


def json_write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False)+"\n", encoding="utf-8")


def same_text(left, right):
    return left.replace(b"\r\n", b"\n") == right.replace(b"\r\n", b"\n")


def baseline(directory):
    require(all(digest(directory/name) == value for name, value in BASELINE_HASHES.items()),
            "Exact immutable second-pilot receipt")
    corpus, selection, decisions = verify_pilot(directory)
    require(MODEL["id"] in selection and
            next(case for case in corpus["cases"] if case["id"] == MODEL["id"]) == MODEL,
            "Unchanged synthetic zero-rate model")
    rows = [json.loads(line) for line in (directory/"outcomes.jsonl").read_text().splitlines()]
    original = [row for row in rows if row["id"] == MODEL["id"]]
    require(len(original) == 12 and sum(row["status"] == "computed" for row in original) == 6,
            "All six old successes and six old refusals retained")
    return corpus, original, decisions[MODEL["id"]]


def source_snapshot(output):
    require(not git("status", "--porcelain").strip(), "Freeze clean source before execution")
    for name, revision in FROZEN_FILES:
        require(same_text((HERE/name).read_bytes(), git("show", revision+":"+PREFIX+name)),
                "Reviewed primitive/default/declaration source unchanged")
    output.mkdir(parents=True, exist_ok=False)
    sources = output/"sources"
    sources.mkdir()
    hashes = {}
    for name in SOURCES:
        (sources/name).write_bytes((HERE/name).read_bytes())
        hashes[PREFIX+name] = digest(sources/name)
    return git("rev-parse", "HEAD").decode().strip(), hashes


def copy_baseline(source, output):
    output.mkdir()
    (output/"sources").mkdir()
    for name in BASELINE_HASHES:
        (output/name).write_bytes((source/name).read_bytes())
    manifest = read_json(source/"manifest.json")
    for path in manifest["source_hashes"]:
        name = Path(path.replace("\\", "/")).name
        (output/"sources"/name).write_bytes((source/"sources"/name).read_bytes())


def execute_requests(output, stream):
    from lcp_stage_adapter import StageCollector, private_reference
    import numpy as np
    rows = []
    for row in fixed_requests():
        print("starting", row["scheme"], row["label"], flush=True)
        collector = StageCollector()
        solve = private_reference(MODE, collector)
        try:
            result = solve(row["model"], scheme=row["scheme"], **row["options"])
            row.update(status="computed", result=result)
        except (ValueError, ArithmeticError) as error:
            row.update(status="refused", reason="ValueError" if isinstance(error, ValueError)
                       else "ArithmeticError", exception_type=type(error).__name__,
                       detail=str(error))
        row["stage_work"] = collector.records
        row["failure_snapshot"] = None
        if collector.failure_arrays is not None:
            name = row["scheme"]+"-"+row["label"]+"-failure.npz"
            path = output/name
            with path.open("xb") as file:
                np.savez_compressed(file, **collector.failure_arrays)
            row["failure_snapshot"] = dict(file=name, sha256=digest(path))
        stream.write(json.dumps(row, allow_nan=False)+"\n")
        stream.flush()
        rows.append(row)
        print("finished", row["scheme"], row["label"], row["status"],
              "observed_stages", len(collector.records), flush=True)
    return rows


def run(output, historical, mode):
    require(mode == MODE, "Only the explicit approved correction mode is executable here")
    # Set before the private module or the collector imports NumPy/SciPy.
    for name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
        os.environ[name] = "1"
    corpus, original, old_decisions = baseline(historical)
    head, hashes = source_snapshot(output)
    copy_baseline(historical, output/"historical-v2")
    import numpy
    import scipy
    manifest = dict(schema="atx-zero-rate-lcp-control-v1", source=head, source_hashes=hashes,
        lcp_mode=MODE, primitive_source=PRIMITIVE,
        declaration_commit=git("rev-parse", PLAN).decode().strip(),
        baseline_hashes=BASELINE_HASHES, input_count=715, selected_ids=[MODEL["id"]],
        uncomputed_ids=[case["id"] for case in corpus["cases"] if case["id"] != MODEL["id"]],
        requested_results=12, requests=fixed_requests(),
        started_utc=datetime.now(timezone.utc).isoformat(),
        python=sys.version, executable=sys.executable, platform=platform.platform(),
        numpy=numpy.__version__, scipy=scipy.__version__, completed=False,
        reference_decisions_updated=False, full_144_run=False, accuracy_established=False,
        full_bundle_qualified=False, timing_qualified=False)
    json_write(output/"manifest.json", manifest)
    with (output/"outcomes.jsonl").open("x", encoding="utf-8") as stream:
        rows = execute_requests(output, stream)
    summary = summarize_control(rows)
    summary["historical_outcomes"] = original
    summary["historical_decisions"] = old_decisions
    summary["other_cold_fields"] = {field: "uncomputed" for field in
                                    ("vega", "theta", "rho", "vanna", "volga", "charm")}
    json_write(output/"summary.json", summary)
    unchanged = git("rev-parse", "HEAD").decode().strip() == head and \
        not git("status", "--porcelain").strip() and all(
            digest(ROOT/name) == value for name, value in hashes.items())
    history_unchanged = all(digest(historical/name) == value
                            for name, value in BASELINE_HASHES.items())
    manifest.update(completed=len(rows) == 12, source_unchanged=unchanged,
        history_unchanged=history_unchanged, computed=summary["computed"],
        refused=summary["refused"], outcomes_sha256=digest(output/"outcomes.jsonl"),
        summary_sha256=digest(output/"summary.json"),
        finished_utc=datetime.now(timezone.utc).isoformat())
    json_write(output/"manifest.json", manifest)
    if unchanged and history_unchanged:
        verify_run(output)
        return 0
    return 2


def verify_failure_snapshot(directory, row):
    import numpy as np
    snapshot = row["failure_snapshot"]
    typed = bool(row["stage_work"]) and row["stage_work"][-1]["status"] == "refused"
    if not typed:
        require(snapshot is None, "No invented failed-stage snapshot")
        return
    expected = row["scheme"]+"-"+row["label"]+"-failure.npz"
    require(type(snapshot) is dict and set(snapshot) == {"file", "sha256"} and
            snapshot["file"] == expected and digest(directory/expected) == snapshot["sha256"],
            "Exact owned failed-stage archive")
    work = row["stage_work"][-1]["work"]
    names = {"lower", "diagonal", "upper", "rhs", "payoff", "initial"}
    if work["last_checked_residual"] is not None:
        names.add("last_checked_values")
    with np.load(directory/expected, allow_pickle=False) as arrays:
        require(set(arrays.files) == names, "Complete failed-stage inputs and checked vector")
        n = row["options"]["nx"]-2
        for name in names:
            require(arrays[name].dtype == np.dtype("float64") and
                    arrays[name].shape == (n-1 if name in ("lower", "upper") else n,) and
                    np.isfinite(arrays[name]).all(), "Finite actual failed-stage arrays")
        if "last_checked_values" in arrays:
            values = arrays["last_checked_values"]
            continuation = arrays["diagonal"]*values
            continuation[1:] += arrays["lower"]*values[:-1]
            continuation[:-1] += arrays["upper"]*values[1:]
            residual = float(np.max(np.abs(np.minimum(continuation-arrays["rhs"],
                                                      values-arrays["payoff"]))))
            require(residual == work["last_checked_residual"], "Saved vector and residual agree")


def verify_run(directory):
    manifest = read_json(directory/"manifest.json")
    require(manifest["schema"] == "atx-zero-rate-lcp-control-v1" and
            manifest["lcp_mode"] == MODE and manifest["primitive_source"] == PRIMITIVE and
            manifest["declaration_commit"] == git("rev-parse", PLAN).decode().strip() and
            re.fullmatch("[0-9a-f]{40}", manifest["source"]), "Frozen source/mode declaration")
    require(all(manifest[field] is True for field in
                ("completed", "source_unchanged", "history_unchanged")) and
            all(manifest[field] is False for field in
                ("reference_decisions_updated", "full_144_run", "accuracy_established",
                 "full_bundle_qualified", "timing_qualified")), "Completed unpromoted control")
    require(manifest["baseline_hashes"] == BASELINE_HASHES, "Frozen historical artifacts")
    corpus, original, decisions = baseline(directory/"historical-v2")
    require(manifest["requested_results"] == 12 and manifest["input_count"] == 715 and
            manifest["selected_ids"] == [MODEL["id"]] and manifest["requests"] == fixed_requests()
            and manifest["uncomputed_ids"] == [case["id"] for case in corpus["cases"]
                                                if case["id"] != MODEL["id"]],
            "All twelve requests and all714 uncomputed contracts")
    hashes = manifest["source_hashes"]
    require(set(hashes) == {PREFIX+name for name in SOURCES}, "Complete source denominator")
    for name, value in hashes.items():
        archived = directory/"sources"/Path(name).name
        require(digest(archived) == value and same_text(archived.read_bytes(),
                git("show", manifest["source"]+":"+name)), "Archived source/Git join")
    for name, revision in FROZEN_FILES:
        require(same_text((directory/"sources"/name).read_bytes(),
                         git("show", revision+":"+PREFIX+name)),
                "Actual archived primitive/default/declaration equals reviewed revision")
    require(all(type(manifest[field]) is str and bool(manifest[field]) for field in
                ("python", "executable", "platform", "numpy", "scipy",
                 "started_utc", "finished_utc")), "Recorded execution environment")
    started = datetime.fromisoformat(manifest["started_utc"])
    finished = datetime.fromisoformat(manifest["finished_utc"])
    require(started.tzinfo is not None and finished.tzinfo is not None and finished >= started,
            "Ordered explicit UTC-offset timestamps")
    for name in ("outcomes", "summary"):
        suffix = ".jsonl" if name == "outcomes" else ".json"
        require(digest(directory/(name+suffix)) == manifest[name+"_sha256"], "Result digest")
    rows = [json.loads(line) for line in (directory/"outcomes.jsonl").read_text().splitlines()]
    summary = summarize_control(rows)
    for row in rows:
        verify_failure_snapshot(directory, row)
    summary.update(historical_outcomes=original, historical_decisions=decisions,
        other_cold_fields={field: "uncomputed" for field in
                           ("vega", "theta", "rho", "vanna", "volga", "charm")})
    require(read_json(directory/"summary.json") == summary and
            all(type(manifest[field]) is int and manifest[field] == summary[field]
                for field in ("computed", "refused")), "Full summary/status reconstruction")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--output", type=Path)
    actions.add_argument("--verify", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--mode", choices=(MODE,))
    args = parser.parse_args()
    if args.verify:
        require(args.baseline is None and args.mode is None, "Verification uses archived inputs")
        result = verify_run(args.verify)
        print(json.dumps({key: result[key] for key in
                          ("requested", "computed", "refused", "solver_controls_passed")}))
    else:
        require(args.baseline is not None and args.mode == MODE, "Explicit baseline/mode required")
        raise SystemExit(run(args.output, args.baseline, args.mode))
