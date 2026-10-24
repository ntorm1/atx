"""Pinned QuantLib source/clock verification for an isolated public comparator."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import struct
import subprocess
import sys

QUANTLIB_COMMIT = "e96579fd471b2e9f6bc974c96674d1ccfff248fa"
HERE = Path(__file__).resolve().parent


def source_tree_digest(root):
    root = Path(root)
    paths = sorted([p for p in (root / "ql").rglob("*") if p.is_file()] + [root / "LICENSE.TXT"])
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.relative_to(root).as_posix().encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def verify_source(root, manifest):
    if source_tree_digest(root) != manifest["tree_sha256"]:
        raise ValueError("QuantLib extracted source tree differs from pinned archive")
    for reference in manifest.get("reference_files", []):
        if hashlib.sha256((Path(root) / reference["path"]).read_bytes()).hexdigest() != reference["sha256"]:
            raise ValueError("QuantLib official regression source changed")


def check_clock(raw, actual):
    try:
        if any(isinstance(actual[k], bool) or not isinstance(actual[k], (int, float)) or
               not math.isfinite(actual[k]) for k in ["T", "r", "q", "variance", "discount", "dividend_discount"]):
            return False
        if actual["T"] != raw["T"]:
            return False
        eps = sys.float_info.epsilon
        # QdPutCallParityEngine reconstructs annualized rates from discounts.
        # Bound the representation roundoff explicitly; do not change the model.
        for rate, discount in [("r", "discount"), ("q", "dividend_discount")]:
            if abs(actual[rate] - raw[rate]) > 64 * eps * max(1., abs(raw[rate] * raw["T"])) / raw["T"]:
                return False
            if abs(actual[discount] - math.exp(-raw[rate] * raw["T"])) > 8 * eps:
                return False
        variance = raw["sigma"] ** 2 * raw["T"]
        return abs(actual["variance"] - variance) <= 8 * eps * max(variance, sys.float_info.min)
    except (KeyError, TypeError, ValueError, ZeroDivisionError):
        return False


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def price_digest(values):
    value = 14695981039346656037
    for price in values:
        bits = struct.unpack("<Q", struct.pack("<d", price))[0] if finite(price) else 0x7ff8000000000000
        value = ((value ^ bits) * 1099511628211) & ((1 << 64) - 1)
    return value


def european_put(c):
    v = c["sigma"] * math.sqrt(c["T"])
    d1 = (math.log(c["S"] / c["K"]) + (c["r"] - c["q"] + .5 * c["sigma"] ** 2) * c["T"]) / v
    return .5 * c["K"] * math.exp(-c["r"] * c["T"]) * math.erfc((d1 - v) / math.sqrt(2)) - \
           .5 * c["S"] * math.exp(-c["q"] * c["T"]) * math.erfc(d1 / math.sqrt(2))


def check_self_test(report):
    if report.get("quantlib_commit") != QUANTLIB_COMMIT:
        raise ValueError("Missing/wrong official source self-test")
    expected = {(24, 3, 9, .05, 0): (.1069528125898476, 1e-6),
                (24, 3, 9, .05, 1): (.1069524359360852, 1e-6),
                (35, 8, 16, .075, 0): (.3671116758420414, 1e-9),
                (35, 8, 16, .075, 1): (.3671111055677869, 1e-9)}
    rows = report.get("official_checks", [])
    found = set()
    for row in rows:
        key = (row["l"], row["m"], row["n"], row["input"]["r"], row["equation"])
        if key not in expected or key in found:
            raise ValueError("Changed/duplicate official premium fixture")
        value, tolerance = expected[key]
        if row["expected"] != value or row["tolerance"] != tolerance or not finite(row["premium"]) or \
                abs(row["premium"] - value) > tolerance:
            raise ValueError("Official premium regression failed")
        found.add(key)
    if found != set(expected):
        raise ValueError("Official premium coverage incomplete")
    clock = report.get("clock_checks", [])
    if len(clock) != 4 or any(not check_clock(r["input"], r["effective"]) for r in clock):
        raise ValueError("Exact clock self-test failed")
    factory = report.get("factory_checks", [])
    if len(factory) != 3 or {r["scheme"] for r in factory} != {0, 1, 2} or any(
            r["same_prices"] is not True or not finite(r["fresh"]) or r["fresh"] != r["factory"] for r in factory):
        raise ValueError("Official factory parity failed")
    european = report.get("european_checks", [])
    if len(european) != 9 or any(not finite(r["price"]) or
            abs(r["price"] - european_put(r["input"])) > 1e-10 for r in european):
        raise ValueError("Independent European self-test failed")


def summarize(native, corpus):
    if native.get("schema") != "american-put-quantlib-comparator-v1" or native.get("quantlib_commit") != QUANTLIB_COMMIT:
        raise ValueError("Unsupported native QuantLib report")
    cases = [dict(group=g["id"], S=s, **{k: g[k] for k in ["K", "T", "sigma", "r", "q"]})
             for g in corpus["groups"] for s in g["spots"]]
    count = len(cases)
    refs = native.get("references", [])
    if native.get("cases") != cases or native.get("case_count") != count or len(refs) != count or \
            native.get("group_count") != len(corpus["groups"]):
        raise ValueError("Frozen case/model denominator mismatch")
    check_self_test(native.get("self_test", {}))
    effective = native.get("effective_models", [])
    if len(effective) != len(corpus["groups"]):
        raise ValueError("Missing effective clock evidence")
    for original, model in zip(corpus["groups"], effective):
        expected = dict(S=original["spots"][0], **{k: original[k] for k in ["K", "T", "sigma", "r", "q"]})
        if model.get("group") != original["id"] or model.get("input") != expected or \
                not check_clock(expected, model.get("effective", {})):
            raise ValueError("Effective model does not match frozen input")
    engines = native.get("engines", [])
    if sorted(e.get("name", "") for e in engines) != ["atx_bulk_fast", "atx_default", "ql_accurate", "ql_fast"]:
        raise ValueError("Missing/duplicate engine")
    result = dict(schema="quantlib-american-comparison-summary-v1", case_count=count,
        general_american_independent_method_reference=False,
        reference="Official QuantLib highPrecision, independently implemented ALO; cross-check refined ATX ALO",
        reference_resolution="Unresolved when implementations disagree by more than5% of the synthetic budget",
        engines=[], build=native.get("build"), affinity=native.get("affinity"))
    for engine in engines:
        name = engine["name"]
        prices, reused = engine.get("cold_prices", []), engine.get("reused_prices", [])
        if len(prices) != count or len(reused) != count or prices != reused or price_digest(prices) != price_digest(reused):
            raise ValueError("Cold/reused output mismatch")
        row = dict(name=name, attempted=count, finite=sum(finite(p) for p in prices),
                   reuse_kind=engine["reuse_kind"], budgets={})
        expected_kind = "engine-market-scheme; boundary recomputed per spot" if name.startswith("ql_") else "solved boundary across spots"
        if engine["reuse_kind"] != expected_kind:
            raise ValueError("Ambiguous reuse semantics")
        repetitions = len(engine.get("cold_s", []))
        if repetitions == 0:
            raise ValueError("Missing timing repetitions")
        for clock in ["cold_s", "reuse_setup_s", "reuse_eval_s", "reuse_total_s"]:
            values = engine.get(clock, [])
            if len(values) != repetitions or any(not finite(v) or v < 0 for v in values):
                raise ValueError("Invalid timing vector")
            row[clock] = dict(repetitions=repetitions, median_s=statistics.median(values), min_s=min(values),
                max_s=max(values), median_us_per_attempt=statistics.median(values) * 1e6 / count)
        if any(total < setup + evaluation for total, setup, evaluation in zip(
                engine["reuse_total_s"], engine["reuse_setup_s"], engine["reuse_eval_s"])):
            raise ValueError("Reuse total omits setup/evaluation")
        for key in ["cold_digests", "reused_digests"]:
            if engine.get(key) != [price_digest(prices)] * repetitions:
                raise ValueError("Missing/changed repetition digest")
        for key in ["cold_status", "reused_status"]:
            if len(engine.get(key, [])) != count:
                raise ValueError("Missing status denominator")
        for budget in [.001, .0001]:
            score = dict(resolved_pass=0, resolved_fail=0, reference_unresolved=0, numerical_failure=0,
                         max_abs_residual=None, failures=[])
            residuals = []
            for i, (case, price, ref) in enumerate(zip(cases, prices, refs)):
                closed = european_put(case) if case["r"] <= 0 and case["q"] >= 0 else None
                expected = closed if closed is not None else ref["quantlib_high_precision"]
                residual = abs(price - expected) if finite(price) and finite(expected) else None
                if residual is not None:
                    residuals.append(residual)
                if not finite(price):
                    outcome = "numerical_failure"
                elif not finite(expected) or (closed is None and (not finite(ref["atx_refined"]) or
                        abs(expected - ref["atx_refined"]) > .05 * budget)):
                    outcome = "reference_unresolved"
                else:
                    outcome = "resolved_pass" if residual <= budget else "resolved_fail"
                score[outcome] += 1
                if outcome != "resolved_pass":
                    score["failures"].append(dict(index=i, outcome=outcome, case=case, price=price,
                        reference=expected, residual=residual, status=engine["cold_status"][i]))
            score["max_abs_residual"] = max(residuals, default=None)
            row["budgets"][format(budget, "g")] = score
        result["engines"].append(row)
    return result


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_new(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def run(args):
    manifest = json.loads(Path(args.manifest).read_text())
    verify_source(args.upstream, manifest)
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary, corpus_path = Path(args.binary).resolve(), Path(args.corpus).resolve()
    corpus = json.loads(corpus_path.read_text())
    command = [str(binary), "--input", str(corpus_path), "--output", str(output / "native.json"), "--reps", str(args.reps)]
    if args.affinity is not None:
        command += ["--affinity", str(args.affinity)]
    if args.require_pcore:
        command += ["--require-pcore"]
    repository = HERE.parents[2]
    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(repository), *arguments], text=True).strip()
    receipt = dict(schema="quantlib-comparator-run-v1", started_utc=datetime.now(timezone.utc).isoformat(),
        command=command, source_commit=git("rev-parse", "HEAD"), dirty_status=git("status", "--porcelain"),
        binary_sha256=sha256(binary), corpus_sha256=sha256(corpus_path), upstream=manifest,
        native_source_sha256=sha256(HERE / "quantlib_main.cpp"), python_source_sha256=sha256(__file__),
        cmake_source_sha256=sha256(HERE / "quantlib.cmake"), python=sys.version, platform=platform.platform())
    cache = binary.parent.parent / "CMakeCache.txt"
    if cache.is_file():
        receipt["cmake_cache_sha256"] = sha256(cache)
        receipt["cmake_cache_settings"] = [line for line in cache.read_text().splitlines() if line.startswith(
            ("CMAKE_BUILD_TYPE:", "CMAKE_INTERPROCEDURAL_OPTIMIZATION:", "CMAKE_CXX_FLAGS:",
             "CMAKE_CXX_FLAGS_RELEASE:", "ATX_VOL_PUBLIC_QUANTLIB_DIR:"))]
    env = dict(os.environ, ATX_VOL_FIT_WORKERS="1", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1")
    process = subprocess.run(command, env=env, capture_output=True, text=True, timeout=1800)
    (output / "stdout.txt").write_text(process.stdout, encoding="utf-8")
    (output / "stderr.txt").write_text(process.stderr, encoding="utf-8")
    receipt.update(returncode=process.returncode, finished_utc=datetime.now(timezone.utc).isoformat(),
                   binary_sha256_after=sha256(binary), corpus_sha256_after=sha256(corpus_path))
    write_new(output / "receipt.json", receipt)
    if process.returncode:
        raise RuntimeError("Native QuantLib comparator failed; see " + str(output))
    if receipt["binary_sha256"] != receipt["binary_sha256_after"] or receipt["corpus_sha256"] != receipt["corpus_sha256_after"]:
        raise ValueError("Inputs changed during execution")
    verify_source(args.upstream, manifest)
    summary = summarize(json.loads((output / "native.json").read_text()), corpus)
    summary["native_sha256"] = sha256(output / "native.json")
    write_new(output / "summary.json", summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-source")
    parser.add_argument("--manifest", default=str(HERE / "upstream.json"))
    parser.add_argument("--binary")
    parser.add_argument("--upstream")
    parser.add_argument("--corpus")
    parser.add_argument("--output")
    parser.add_argument("--reps", type=int, default=10)
    parser.add_argument("--affinity", type=int)
    parser.add_argument("--require-pcore", action="store_true")
    args = parser.parse_args()
    if args.verify_source:
        verify_source(args.verify_source, json.loads(Path(args.manifest).read_text()))
    elif all([args.binary, args.upstream, args.corpus, args.output]):
        run(args)
    else:
        parser.error("Require --verify-source or --binary/--upstream/--corpus/--output")


if __name__ == "__main__":
    main()
