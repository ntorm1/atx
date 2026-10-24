"""Fixed pilot and immutable evidence writer; no production or latency claim."""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys

CONFIGS = {
    "space-coarse": (2049, 512, 1.), "center": (4097, 512, 1.),
    "space-fine": (8193, 512, 1.), "time-coarse": (4097, 256, 1.),
    "time-fine": (4097, 1024, 1.), "domain-wide": (8193, 512, 2.),
}
SCHEMES = ("cn", "dirk_l", "dirk_third")


def summarize_reference(model, rows):
    """Implement the predeclared rule without looking at a candidate derivative."""
    if type(model.get("S")) not in (int, float) or not math.isfinite(model["S"]) or model["S"] <= 0:
        raise ValueError("Invalid reference spot")
    wanted = {(s, label) for s in SCHEMES for label in CONFIGS}
    keys = [(row.get("scheme"), row.get("label")) for row in rows]
    incomplete = len(keys) != len(wanted) or set(keys) != wanted
    out = {}
    for field in ("delta", "gamma"):
        def unresolved(reason):
            return dict(status="unresolved", reason=reason, center=None, uncertainty=None)
        if incomplete or any(row.get("status") != "computed" for row in rows):
            out[field] = unresolved("incomplete_or_refused")
            continue
        results = {key: row["result"] for key, row in zip(keys, rows)}
        regimes = {r.get("regime") for r in results.values()}
        if len(regimes) != 1 or not regimes <= {"exercise", "continuation"}:
            out[field] = unresolved("inconsistent_regime")
            continue
        if any(type(r.get(field)) not in (int, float) or not math.isfinite(r[field]) for r in results.values()):
            out[field] = unresolved("missing_or_nonfinite_field")
            continue
        if any(type(r.get("log_spacing")) not in (int, float) or
               not math.isfinite(r["log_spacing"]) or r["log_spacing"] <= 0 or
               type(r.get("stencil_values")) is not list or len(r["stencil_values"]) != 3 or
               any(type(x) not in (int, float) or not math.isfinite(x) for x in r["stencil_values"])
               for r in results.values()):
            out[field] = unresolved("missing_or_nonfinite_stencil")
            continue
        indicators = []
        for r in results.values():
            p = 32*sys.float_info.epsilon*max(1., *[abs(x) for x in r["stencil_values"]])
            s, dx = model["S"], r["log_spacing"]
            indicator = p/(s*dx) if field == "delta" else p*(4/dx**2+1/dx)/s**2
            if not math.isfinite(indicator) or indicator < 0:
                raise ValueError("Invalid cancellation indicator")
            indicators.append(indicator)
        rounding = max(indicators)
        centers = [results[s, "center"][field] for s in SCHEMES]
        center = .5*(min(centers)+max(centers))
        axes, ratio_pass = {}, True
        for scheme in SCHEMES:
            get = lambda label: results[scheme, label][field]
            space = [abs(get("space-coarse")-get("center")), abs(get("center")-get("space-fine"))]
            time = [abs(get("time-coarse")-get("center")), abs(get("center")-get("time-fine"))]
            domain = abs(get("domain-wide")-get("center"))
            ratios = [a[1] <= .5*a[0]+2*rounding for a in (space, time)]
            ratio_pass = ratio_pass and all(ratios)
            axes[scheme] = dict(space=space, time=time, domain=domain, ratio_pass=ratios)
        uncertainty = .5*(max(centers)-min(centers))+rounding+max(
            max(a["space"])+max(a["time"])+a["domain"] for a in axes.values())
        reserve = {}
        for label, fraction in (("operational", .001), ("stress", .01)):
            shock = fraction*model["S"]
            coefficient = shock if field == "delta" else .5*shock*shock
            reserve[label] = uncertainty*coefficient <= .000025
        reason = "refinement_ratio" if not ratio_pass else "reference_reserve" if not all(reserve.values()) else None
        out[field] = dict(status="unresolved" if reason else "empirically_resolved", reason=reason,
                          center=center, uncertainty=uncertainty, roundoff_indicator=rounding,
                          axes=axes, reserve_pass=reserve, regime=next(iter(regimes)))
    return out


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False)+"\n", encoding="utf-8")


def run(output):
    here = Path(__file__).resolve().parent
    root = here.parents[2]
    corpus_path = here.parent/"american-greeks/corpus-v1.json"
    selection_path = here.parent/"american-greeks/refinement-v1.json"
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=root, text=True).strip()
    if git("status", "--porcelain"):
        raise ValueError("Freeze clean source before the pilot")
    head = git("rev-parse", "HEAD")
    corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    selection = json.loads(selection_path.read_text(encoding="utf-8"))
    cases = corpus["cases"]
    by_id = {c["id"]: c for c in cases}
    if len(cases) != 715 or len(by_id) != 715 or len(selection) != 12 or len(set(selection)) != 12 or \
            not set(selection) <= set(by_id):
        raise ValueError("Frozen pilot population mismatch")
    output.mkdir(parents=True, exist_ok=False)
    source_dir = output/"sources"
    source_dir.mkdir()
    sources = [here/name for name in ("greek_reference.py", "greek_study.py", "pde_reference.py",
                                    "greek-plan-v1.md", "test_greek_reference.py", "test_greek_study.py")]
    sources += [corpus_path, selection_path, here.parent/"american-greeks/risk-contract-proposal-v1.json"]
    hashes = {}
    for source in sources:
        copied = source_dir/source.name
        copied.write_bytes(source.read_bytes())
        hashes[str(source.relative_to(root))] = digest(copied)
    for name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
        os.environ[name] = "1"
    import numpy
    import scipy
    from greek_reference import greek_reference
    manifest = dict(schema="atx-independent-greek-pilot-v1", source=head, source_hashes=hashes,
                    input_count=715, selected_ids=selection,
                    uncomputed_ids=[c["id"] for c in cases if c["id"] not in selection],
                    requested_results=216, schemes=SCHEMES, configurations=CONFIGS,
                    started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    python=sys.version, executable=sys.executable, platform=platform.platform(),
                    numpy=numpy.__version__, scipy=scipy.__version__,
                    accuracy_established=False, full_bundle_qualified=False, timing_qualified=False,
                    completed=False)
    write_json(output/"manifest.json", manifest)
    all_rows, summaries = [], {}
    with (output/"outcomes.jsonl").open("x", encoding="utf-8") as stream:
        for case_id in selection:
            model, case_rows = by_id[case_id], []
            for scheme in SCHEMES:
                for label, (nx, nt, domain_scale) in CONFIGS.items():
                    row = dict(id=case_id, model=model, scheme=scheme, label=label,
                               options=dict(nx=nx, nt=nt, domain_scale=domain_scale))
                    try:
                        row.update(status="computed", result=greek_reference(model, scheme=scheme,
                                   nx=nx, nt=nt, domain_scale=domain_scale))
                    except (ValueError, ArithmeticError) as error:
                        row.update(status="refused", reason=type(error).__name__, detail=str(error))
                    stream.write(json.dumps(row, allow_nan=False)+"\n")
                    stream.flush()
                    case_rows.append(row)
                    all_rows.append(row)
            summaries[case_id] = summarize_reference(model, case_rows)
            print(case_id, {f: a["status"]+":"+str(a["reason"]) for f, a in summaries[case_id].items()}, flush=True)
    unchanged = git("rev-parse", "HEAD") == head and not git("status", "--porcelain") and all(
        digest(root/name) == value for name, value in hashes.items())
    write_json(output/"summary.json", dict(fields=summaries,
        other_cold_fields={x: "uncomputed" for x in ("vega", "theta", "rho", "vanna", "volga", "charm")},
        candidate_comparison_performed=False, full_bundle_qualified=False))
    manifest.update(completed=len(all_rows) == 216, source_unchanged=unchanged,
                    computed=sum(r["status"] == "computed" for r in all_rows),
                    refused=sum(r["status"] == "refused" for r in all_rows),
                    outcomes_sha256=digest(output/"outcomes.jsonl"), summary_sha256=digest(output/"summary.json"),
                    finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    write_json(output/"manifest.json", manifest)
    return 0 if manifest["completed"] and unchanged else 2


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    raise SystemExit(run(parser.parse_args().output))
