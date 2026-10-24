"""Pinned, isolated American-put comparison. No optional Python dependencies."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import urllib.request

UPSTREAM_COMMIT = "e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4"
HERE = Path(__file__).resolve().parent


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_new(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def verify_upstream(directory, manifest):
    root = Path(directory).resolve()
    for entry in manifest["files"]:
        path = (root / entry["path"]).resolve()
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError(f"Missing or invalid upstream path: {entry['path']}")
        if sha256(path) != entry["sha256"]:
            raise ValueError(f"Upstream hash mismatch: {entry['path']}")


def fetch_upstream(directory):
    manifest = json.loads((HERE / "upstream.json").read_text())
    root = Path(directory)
    root.mkdir(parents=True, exist_ok=True)
    for entry in manifest["files"]:
        path = root / entry["path"]
        if path.exists():
            if sha256(path) != entry["sha256"]:
                raise ValueError(f"Refusing to overwrite changed upstream file: {path}")
            continue
        url = f"https://raw.githubusercontent.com/simu-ai/american_put/{UPSTREAM_COMMIT}/{entry['path']}"
        with urllib.request.urlopen(url, timeout=30) as response:
            body = response.read()
        if hashlib.sha256(body).hexdigest() != entry["sha256"]:
            raise ValueError(f"Download hash mismatch: {url}")
        path.write_bytes(body)
    verify_upstream(root, manifest)


def make_corpus(scope="diagnostic"):
    groups = []
    for maturity in [1 / 365, .25, 1., 3.]:
        for sigma in [.05, .2, .8]:
            for rate, yield_ in [(.03, 0.), (.03, .01), (.03, .08), (.03, -.02)]:
                groups.append(dict(id=f"g{len(groups):03d}", K=100., T=maturity,
                    sigma=sigma, r=rate, q=yield_, spots=[40., 70., 90., 100., 110., 140., 200.]))
    for label, rate, yield_ in [("zero_rate_closed_form", 0., 0.),
            ("double_boundary_diagnostic", -.01, -.03),
            ("negative_rate_european", -.01, .02)]:
        groups.append(dict(id=label, K=100., T=1., sigma=.2, r=rate, q=yield_,
                           spots=[40., 70., 90., 100., 110., 140., 200.]))
    if scope == "common":
        groups = [g for g in groups if g["r"] > 0 and g["q"] >= 0]
    elif scope != "diagnostic":
        raise ValueError("Unknown corpus scope")
    return dict(schema="american-put-comparator-corpus-v1", scope=scope, model="continuous-yield GBM",
                clock="one shared year fraction T; constant annualized r, q, sigma",
                currency="USD per share; synthetic K=100", groups=groups)


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def time_stats(values, count):
    if not values or any(not finite(v) or v < 0 for v in values):
        raise ValueError("Invalid timing vector")
    return dict(repetitions=len(values), median_s=statistics.median(values),
                min_s=min(values), max_s=max(values),
                median_us_per_attempt=statistics.median(values) * 1e6 / count)


def summarize_report(report):
    if report.get("schema") != "american-put-public-comparator-v1":
        raise ValueError("Unsupported native report")
    count = report["case_count"]
    refs = report["references"]
    if not isinstance(count, int) or isinstance(count, bool) or count <= 0 or len(refs) != count or \
            len(report.get("cases", [])) != count:
        raise ValueError("Invalid reference denominator")
    names = [e.get("name") for e in report.get("engines", [])]
    if sorted(names) != sorted(["simu_fast", "simu_finer", "atx_default", "atx_bulk_fast"]):
        raise ValueError("Missing or duplicated native engine profile")
    result = dict(schema="american-put-public-comparison-summary-v1", case_count=count,
        independent_general_american_accuracy_established=False,
        reference="Refined atx-vol Reference: correlated in-house check, not independent American oracle",
        clocks="Cold has fresh per-contract setup, not cold OS/CPU caches; prepared includes setup per immutable model group",
        engines=[])
    checks = report.get("upstream_adapter_checks", [])
    expected_checks = {(name, i) for name in ["simu_fast", "simu_finer"] for i in range(count)}
    if len(checks) != 2 * count or any(item.get("bit_identical") is not True for item in checks) or \
            {(item.get("engine"), item.get("index")) for item in checks} != expected_checks:
        raise ValueError("Native adapter does not reproduce original upstream calc()")
    for engine in report["engines"]:
        prices, prepared = engine["cold_prices"], engine["prepared_prices"]
        if len(prices) != count or len(prepared) != count:
            raise ValueError("Price vector does not match frozen denominator")
        row = dict(name=engine["name"], attempted=count, finite=sum(finite(x) for x in prices),
                   cold_prepared_mismatches=sum(a != b for a, b in zip(prices, prepared)), budgets={})
        if row["cold_prepared_mismatches"]:
            raise ValueError("Cold/prepared numerical parity failed")
        row["numerical_failure_contexts"] = [dict(index=i,
            status=engine.get("cold_status", [None] * count)[i],
            case=report.get("cases", [None] * count)[i]) for i, p in enumerate(prices) if not finite(p)]
        for key in ["cold_digests", "prepared_digests"]:
            values = engine.get(key, [])
            if len(values) != len(engine["cold_s"]) or len(set(values)) != 1:
                raise ValueError("Native repeated prices are not deterministic")
        for clock in ["cold_s", "prepared_setup_s", "prepared_eval_s", "prepared_total_s"]:
            row[clock] = time_stats(engine[clock], count)
            if len(engine[clock]) != len(engine["cold_s"]):
                raise ValueError("Clock repetition mismatch")
        if any(t < s + e for t, s, e in zip(engine["prepared_total_s"],
                engine["prepared_setup_s"], engine["prepared_eval_s"])):
            raise ValueError("Prepared total excludes charged setup/evaluation")
        for budget in [.001, .0001]:
            bucket = dict(resolved_pass=0, resolved_fail=0, reference_unresolved=0,
                          numerical_failure=0, max_abs_residual=None, failures=[])
            residuals = []
            for i, (price, ref) in enumerate(zip(prices, refs)):
                closed = ref.get("closed_form")
                reference = closed if finite(closed) else ref.get("price")
                residual = abs(price - reference) if finite(price) and finite(reference) else None
                if residual is not None:
                    residuals.append(residual)
                if not finite(price):
                    kind = "numerical_failure"
                elif not finite(reference) or (not finite(closed) and (
                        not finite(ref.get("refinement_difference")) or ref["refinement_difference"] > budget * .05)):
                    kind = "reference_unresolved"
                else:
                    kind = "resolved_pass" if residual <= budget else "resolved_fail"
                bucket[kind] += 1
                if kind != "resolved_pass":
                    bucket["failures"].append(dict(index=i, kind=kind, price=price,
                        reference=reference, residual=residual,
                        case=report.get("cases", [None] * count)[i]))
            bucket["max_abs_residual"] = max(residuals, default=None)
            row["budgets"][format(budget, "g")] = bucket
        result["engines"].append(row)
    result["affinity"] = report.get("affinity")
    result["build"] = report.get("build")
    return result


def run(args):
    upstream = json.loads((HERE / "upstream.json").read_text())
    verify_upstream(args.upstream, upstream)
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    corpus = Path(args.corpus).resolve()
    binary = Path(args.binary).resolve()
    command = [str(binary), "--input", str(corpus), "--output", str(output / "native.json"),
               "--reps", str(args.reps)]
    if args.affinity is not None:
        command += ["--affinity", str(args.affinity)]
    if args.require_pcore:
        command += ["--require-pcore"]
    env = dict(os.environ, ATX_VOL_FIT_WORKERS="1", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1")
    repository = HERE.parents[2]
    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(repository), *arguments], text=True).strip()
    receipt = dict(schema="american-put-comparator-run-v1", started_utc=datetime.now(timezone.utc).isoformat(),
        command=command, binary_sha256=sha256(binary), corpus_sha256=sha256(corpus),
        source_commit=git("rev-parse", "HEAD"), dirty_status=git("status", "--porcelain"),
        upstream=upstream, manifest_sha256=sha256(HERE / "upstream.json"),
        python=sys.version, platform=platform.platform(), pid=os.getpid(),
        native_source_sha256=sha256(HERE / "comparator_main.cpp"),
        runner_sha256=sha256(__file__), cmake_sha256=sha256(HERE.parents[1] / "CMakeLists.txt"))
    cache = binary.parent.parent / "CMakeCache.txt"
    if cache.is_file():
        receipt["cmake_cache_sha256"] = sha256(cache)
        receipt["cmake_cache_settings"] = [line for line in cache.read_text().splitlines() if line.startswith(
            ("CMAKE_BUILD_TYPE:", "CMAKE_INTERPROCEDURAL_OPTIMIZATION:", "CMAKE_CXX_FLAGS:",
             "CMAKE_CXX_FLAGS_RELEASE:", "ATX_VOL_PUBLIC_AMERICAN_PUT_DIR:"))]
    completed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=1800)
    (output / "stdout.txt").write_text(completed.stdout, encoding="utf-8")
    (output / "stderr.txt").write_text(completed.stderr, encoding="utf-8")
    receipt.update(returncode=completed.returncode, finished_utc=datetime.now(timezone.utc).isoformat(),
                   binary_sha256_after=sha256(binary), corpus_sha256_after=sha256(corpus))
    write_new(output / "receipt.json", receipt)
    if completed.returncode:
        raise RuntimeError(f"Native comparator failed; see {output / 'stderr.txt'}")
    if receipt["binary_sha256_after"] != receipt["binary_sha256"] or receipt["corpus_sha256_after"] != receipt["corpus_sha256"]:
        raise RuntimeError("Inputs changed during run")
    report = json.loads((output / "native.json").read_text())
    if report["upstream_commit"] != UPSTREAM_COMMIT:
        raise ValueError("Native upstream commit mismatch")
    expected = json.loads(corpus.read_text())
    expected_cases = [dict(group=g["id"], S=s, **{k: g[k] for k in ["K", "T", "sigma", "r", "q"]})
                      for g in expected["groups"] for s in g["spots"]]
    if report.get("cases") != expected_cases or report.get("group_count") != len(expected["groups"]):
        raise ValueError("Native model/case denominator differs from frozen corpus")
    verify_upstream(args.upstream, upstream)
    summary = summarize_report(report)
    summary["native_sha256"] = sha256(output / "native.json")
    write_new(output / "summary.json", summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    fetch = sub.add_parser("fetch")
    fetch.add_argument("--directory", required=True)
    corpus = sub.add_parser("corpus")
    corpus.add_argument("--output", required=True)
    corpus.add_argument("--scope", choices=["diagnostic", "common"], default="diagnostic")
    run_parser = sub.add_parser("run")
    for name in ["binary", "upstream", "corpus", "output"]:
        run_parser.add_argument(f"--{name}", required=True)
    run_parser.add_argument("--reps", type=int, default=10)
    run_parser.add_argument("--affinity", type=int)
    run_parser.add_argument("--require-pcore", action="store_true")
    summary = sub.add_parser("summarize")
    summary.add_argument("--input", required=True)
    summary.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.command == "fetch":
        fetch_upstream(args.directory)
    elif args.command == "corpus":
        write_new(args.output, make_corpus(args.scope))
    elif args.command == "run":
        run(args)
    else:
        write_new(args.output, summarize_report(json.loads(Path(args.input).read_text())))


if __name__ == "__main__":
    main()
