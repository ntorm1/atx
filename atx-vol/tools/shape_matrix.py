#!/usr/bin/env python3
"""Policy x shape-class x cohort matrix over the native quote-slice fitter (lane C3).

ONE command runs a policy set over a cohort set and writes, per cell and per shape class, the
coverage (all-row / eligible price / identified IV), USD and spread errors, IV median and tails,
served shape violations, the refusal taxonomy, per-expiry completeness and the per-expiry fit
cost, with the sha256 of every prediction file:

    python shape_matrix.py run --spec matrix.json --output DIR
    python shape_matrix.py table --output DIR

Each cell is ``quote_benchmark.py run`` (native fit, hash-bound receipt) then ``quote_benchmark.py
score --class-manifest`` (the L1b stratum; the class rules of the manifest, never re-derived).
The scorer is imported, not edited, so every score JSON keeps the scorer's own evaluator_sha256.

LABEL ISOLATION. SpiderRock marks are opened only by the scorer. The label-free columns this tool
adds (refusal taxonomy, completeness, cost) read the cohort's quotes.parquet feature columns, the
prediction CSV and the native report. Only the development split is accepted; no other cohort
split or cohort name is read.

CHECKPOINTS. Every finished cell appends one line to ``cells.jsonl`` and rewrites ``table.md`` /
``table.json``, so a stopped matrix keeps every finished cell. A cell whose prediction file exists
is never re-run or overwritten: its receipt hash is re-verified and the cell is re-aggregated.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

import polars as pl

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quote_benchmark as qb  # noqa: E402  (the scorer; imported, never modified here)


SCHEMA_VERSION = 1
TOOL_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
SCORER = Path(qb.__file__).resolve()
ALLOWED_SPLIT = "development"
MAX_CELL_SECONDS = 600
FEATURES = ("row_id", "chain_id", "underlier", "expiry_days", "input_status", "split")
COMPLETENESS = ("complete", "ge_0.9", "ge_0.5", "gt_0", "none")


def sha256(path: Path) -> str:
    return qb.sha256(path)


def load_spec(path: Path) -> dict:
    """Validate the matrix spec; refuse anything but the development split."""
    spec = json.loads(path.read_text(encoding="utf-8"))
    if spec.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported matrix spec schema version")
    if spec.get("split", ALLOWED_SPLIT) != ALLOWED_SPLIT:
        raise ValueError("the matrix scores the development split only")
    for key in ("binary", "class_manifest", "cohorts", "policies"):
        if key not in spec:
            raise ValueError(f"matrix spec needs {key}")
    if not isinstance(spec["cohorts"], dict) or not spec["cohorts"]:
        raise ValueError("matrix spec cohorts must be a non-empty name -> dataset map")
    if not isinstance(spec["policies"], dict) or not spec["policies"]:
        raise ValueError("matrix spec policies must be a non-empty name -> policy map")
    for name in list(spec["cohorts"]) + list(spec["policies"]):
        if not name or any(c in name for c in "/\\:") or name.startswith("."):
            raise ValueError(f"matrix names must be bare: {name!r}")
    workers = spec.get("workers", 2)
    if not isinstance(workers, int) or not 1 <= workers <= 8:
        raise ValueError("matrix workers must be an integer in [1, 8]")
    seconds = spec.get("max_cell_seconds", MAX_CELL_SECONDS)
    if not isinstance(seconds, (int, float)) or not 0 < seconds <= MAX_CELL_SECONDS:
        raise ValueError(f"max_cell_seconds must be in (0, {MAX_CELL_SECONDS}]")
    cells = spec.get("cells")
    if cells is None:
        cells = [[p, c] for p in spec["policies"] for c in spec["cohorts"]]
    for policy, cohort in cells:
        if policy not in spec["policies"] or cohort not in spec["cohorts"]:
            raise ValueError(f"cell names an unknown policy or cohort: {policy} x {cohort}")
    spec["cells"] = cells
    spec["workers"] = workers
    spec["max_cell_seconds"] = seconds
    return spec


def python_command() -> list[str]:
    # -E -P -B: user site-packages (polars) stay importable; -I would hide them (ledger L3e).
    return [sys.executable, "-E", "-P", "-B"]


def bounded(command: list[str], timeout: float, env: dict | None = None) -> tuple:
    """(exit code, stderr) of one subprocess, or ("timeout", "") past `timeout` seconds. Both
    steps of a cell (fit and score) run under the spec's max_cell_seconds."""
    try:
        done = subprocess.run(command, env=env, capture_output=True, text=True,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    return done.returncode, done.stderr


def keep_stderr(cell: Path, stderr: str) -> None:
    """The first run's stderr stays in run.stderr.txt; a later run writes run.stderr.rerun-N.txt."""
    path, n = cell / "run.stderr.txt", 0
    while path.exists():
        n += 1
        path = cell / f"run.stderr.rerun-{n}.txt"
    path.write_text(stderr or "", encoding="utf-8")


def run_cell(spec: dict, out: Path, policy: str, cohort: str) -> dict:
    dataset = Path(spec["cohorts"][cohort])
    cell = out / "runs" / cohort / policy
    csv = cell / f"{cohort}-{policy}.csv"
    receipt = Path(str(csv) + ".run.json")
    native = cell / f"{cohort}-{policy}-native.json"
    score = cell / f"{cohort}-{policy}-score.json"
    record = {"policy": policy, "cohort": cohort, "dataset": str(dataset),
              "policy_path": spec["policies"][policy], "csv": str(csv)}
    cell.mkdir(parents=True, exist_ok=True)
    if csv.exists():
        record["run"] = {"resumed": True}
    else:
        env = dict(os.environ, ATX_VOL_FIT_WORKERS=str(spec["workers"]))
        command = python_command() + [str(SCORER), "run", "--dataset", str(dataset),
                                      "--binary", spec["binary"], "--output", str(csv),
                                      "--model", "quote-slice",
                                      "--policy", spec["policies"][policy],
                                      "--dividends", spec.get("dividends", "funding-pv"), "--",
                                      "--report", str(native), "--timing-detail"]
        start = time.perf_counter()
        code, stderr = bounded(command, spec["max_cell_seconds"], env)
        record["run"] = {"exit": code, "wall_s": round(time.perf_counter() - start, 2),
                         "workers": spec["workers"], "stderr_tail": stderr[-800:]}
        keep_stderr(cell, stderr)
        if code != 0:
            return record
    if not receipt.exists():
        record["error"] = "no run receipt"
        return record
    record["prediction_sha256"] = sha256(csv)
    bound = json.loads(receipt.read_text(encoding="utf-8")).get("prediction_sha256")
    if bound != record["prediction_sha256"]:
        raise ValueError(f"{csv}: prediction hash disagrees with its receipt")
    if not score.exists():
        command = python_command() + [str(SCORER), "score", "--dataset", str(dataset),
                                      "--run-receipt", str(receipt), "--output", str(score),
                                      "--split", ALLOWED_SPLIT,
                                      "--class-manifest", spec["class_manifest"]]
        code, stderr = bounded(command, spec["max_cell_seconds"])
        record["score_exit"] = code
        if code != 0:
            record["error"] = f"score failed ({code}): {stderr[-800:]}"
            return record
    record["score_sha256"] = sha256(score)
    record["native_sha256"] = sha256(native) if native.exists() else None
    record["classes"] = cell_classes(dataset, Path(spec["class_manifest"]), csv, score,
                                     native if native.exists() else None)
    record["cell"] = cell_totals(native if native.exists() else None)
    return record


def cell_totals(native: Path | None) -> dict:
    """Whole-cell clocks and C2 publication counters from the native report (B0 clocks)."""
    if native is None:
        return {}
    report = json.loads(native.read_text(encoding="utf-8"))
    graded = report.get("graded_stats", {})
    clocks = report.get("bench", {}).get("clocks", {})
    return {"fit_and_reprice_s": report.get("fit_and_reprice_seconds"),
            "cold_calibration_s": clocks.get("cold_calibration_s"),
            "chain_ms_p50": _ms(report.get("chain_latency_us_p50")),
            "chain_ms_p95": _ms(report.get("chain_latency_us_p95")),
            "served_rows": report.get("served_rows"),
            "publication": graded.get("publication"),
            "graded_chains": graded.get("chains")}


def _ms(value):
    return None if value is None else round(value / 1000.0, 3)


def cell_classes(dataset: Path, manifest: Path, csv: Path, score: Path,
                 native: Path | None) -> dict:
    """Per class: the scorer's metrics plus label-free taxonomy, completeness and cost."""
    scored = json.loads(score.read_text(encoding="utf-8"))
    strata = {row["shape_class"]: row for row in scored["class_strata"]["classes"]}
    dataset_manifest = qb.verify_dataset(dataset)
    class_manifest, _, members = qb._class_membership(manifest, dataset_manifest)
    quotes = (pl.read_parquet(dataset / "quotes.parquet", columns=list(FEATURES))
              .filter(pl.col("split") == ALLOWED_SPLIT))
    pred = pl.read_csv(csv, columns=["row_id", "status", "predicted_price"],
                       schema_overrides={"row_id": pl.UInt64, "status": pl.String,
                                         "predicted_price": pl.Float64}, null_values="")
    rows = quotes.join(pred, on="row_id", how="left").with_columns(
        pl.col("status").fill_null("prediction_absent"),
        (pl.col("status").is_in(qb.PRICE_SERVED_STATUSES)
         & pl.col("predicted_price").is_finite()).fill_null(False).alias("served"))
    keys = ["underlier", "expiry_days"]
    names = [e["name"] for e in class_manifest["classes"]]
    symbol = [e["name"] for e in class_manifest["classes"] if e["kind"] == "symbol"]
    pairs = rows.select(keys).unique()
    # The scorer's complement: (underlier, expiry) pairs of no SYMBOL class.
    complement = pairs.join(members.filter(pl.col("shape_class").is_in(symbol)).select(keys),
                            on=keys, how="anti").with_columns(
        pl.lit("no_symbol_class").alias("shape_class"))
    groups = pl.concat([members.select(keys + ["shape_class"]),
                        complement.select(keys + ["shape_class"]),
                        pairs.with_columns(pl.lit("all_rows").alias("shape_class"))])
    costs = chain_costs(native)
    slices = native_slices(native)
    out = {}
    for name in names + ["no_symbol_class", "all_rows"]:
        member = groups.filter(pl.col("shape_class") == name).select(keys)
        frame = rows.join(member, on=keys, how="semi")
        eligible = frame.filter(pl.col("input_status") == "ok")
        taxonomy = dict(eligible.group_by("status").len().sort("status").iter_rows())
        per_chain = eligible.group_by("chain_id").agg(pl.col("served").mean().alias("f"))
        buckets = dict.fromkeys(COMPLETENESS, 0)
        for f in per_chain["f"].to_list():
            buckets[_bucket(f)] += 1
        chains = set(frame["chain_id"].unique().to_list())
        walls = sorted(costs[c] for c in chains if c in costs)
        metrics = strata.get(name, {})
        out[name] = {
            "rows": frame.height, "eligible_rows": eligible.height,
            "served_rows_all": int(frame["served"].sum()),
            "all_row_coverage": _ratio(int(frame["served"].sum()), frame.height),
            "eligible_price_coverage": metrics.get("price_coverage"),
            "identified_iv_coverage": metrics.get("vol_coverage"),
            "price_only_rows": metrics.get("price_only_predicted_rows"),
            "mae_usd": metrics.get("price_mae"), "p99_usd": metrics.get("price_p99_abs"),
            "mae_spread": metrics.get("price_in_spreads_mae"),
            "containment": metrics.get("price_quote_containment"),
            "iv_median_bp": metrics.get("vol_bp_median_abs"),
            "iv_p95_bp": metrics.get("vol_bp_p95_abs"), "iv_p99_bp": metrics.get("vol_bp_p99_abs"),
            "violations": metrics.get("served_shape_violations"),
            "duplicate_disagreements": metrics.get("served_duplicate_strike_disagreements"),
            "refusal_taxonomy": taxonomy, "completeness": buckets,
            "expiry_fit_ms_p50": _quantile(walls, 0.5), "expiry_fit_ms_p95": _quantile(walls, 0.95),
            "expiries_timed": len(walls),
            **expiry_service(eligible.select("chain_id", "status", "served"), slices)}
    return out


ARBITRAGE_KINDS = (("vertical-spread", "vertical_spread"), ("convexity", "convexity"),
                   ("scaled European upper bound", "nondividend_upper_bound"),
                   ("Legendre", "legendre"), ("call-minus-put", "parity"))


def _dominant(statuses: list[str]) -> str:
    counts: dict = {}
    for status in statuses:
        counts[status] = counts.get(status, 0) + 1
    return min(counts, key=lambda s: (-counts[s], s)) if counts else "no_rows"


def slice_reason(entry: dict | None, statuses: list[str]) -> str:
    """Why one fitted expiry slice serves no eligible row: the native report's refusal (the
    certificate's arbitrage class from its error, calendar, insufficient quotes, ...), prefixed
    'validator_rollback+' when C2's publication validation rolled a repair of it back; a slice
    the fitter served whose rows were all refused is 'rows_refused:<status>'; a chain the native
    report does not list is 'not_fitted:<status>' (the dominant row status)."""
    if entry is None:
        return f"not_fitted:{_dominant(statuses)}"
    grade = entry.get("grade")
    if grade in ("A", "B", "C"):
        refused = [s for s in statuses if s not in qb.PRICE_SERVED_STATUSES]
        return f"rows_refused:{_dominant(refused)}"
    if grade == "calendar_arbitrage":
        reason = "calendar"
    elif grade == "arbitrage_failure":
        error = entry.get("error") or ""
        kind = next((name for needle, name in ARBITRAGE_KINDS if needle in error), "other")
        reason = f"arbitrage:{kind}"
    else:
        reason = str(grade)
    if ((entry.get("publication") or {}).get("rollbacks") or 0) > 0:
        reason = f"validator_rollback+{reason}"
    return reason


def expiry_service(eligible: pl.DataFrame, slices: dict) -> dict:
    """Served-EXPIRY completeness of one class: expiries (chains with an eligible row), those
    with at least one served eligible row, and the reason of every unserved one (slice_reason)."""
    expiries, served, reasons = 0, 0, {}
    for (chain,), frame in sorted(eligible.group_by(["chain_id"]), key=lambda g: g[0]):
        expiries += 1
        if frame["served"].any():
            served += 1
            continue
        reason = slice_reason(slices.get(chain), frame["status"].to_list())
        reasons[reason] = reasons.get(reason, 0) + 1
    return {"expiries": expiries, "expiries_served": served,
            "expiry_completeness": _ratio(served, expiries),
            "unserved_reasons": dict(sorted(reasons.items()))}


def native_slices(native: Path | None) -> dict:
    """chain_id -> the native report's per-slice entry (graded_stats.slices)."""
    if native is None:
        return {}
    report = json.loads(native.read_text(encoding="utf-8"))
    return {entry["chain_id"]: entry for entry in report.get("graded_stats", {}).get("slices", [])}


def chain_costs(native: Path | None) -> dict:
    """Per-chain fit wall (ms) from the native report's per-slice timing (--timing-detail)."""
    if native is None:
        return {}
    report = json.loads(native.read_text(encoding="utf-8"))
    out = {}
    for entry in report.get("graded_stats", {}).get("slices", []):
        timing = entry.get("timing")
        if isinstance(timing, dict) and timing.get("wall_us") is not None:
            out[entry["chain_id"]] = timing["wall_us"] / 1000.0
    return out


def _bucket(f: float) -> str:
    if f >= 1.0:
        return "complete"
    if f >= 0.9:
        return "ge_0.9"
    if f >= 0.5:
        return "ge_0.5"
    return "gt_0" if f > 0.0 else "none"


def _ratio(a: int, b: int):
    return a / b if b else None


def _quantile(values: list[float], q: float):
    if not values:
        return None
    if len(values) == 1:
        return round(values[0], 3)
    return round(statistics.quantiles(values, n=100, method="inclusive")[int(q * 100) - 1], 3)


def _fmt(value, kind: str = "f3") -> str:
    if value is None:
        return "-"
    if kind == "pct":
        return f"{100.0 * value:.2f}%"
    if kind == "int":
        return str(value)
    return f"{value:.3f}" if kind == "f3" else f"{value:.1f}"


def write_table(out: Path, records: list[dict]) -> None:
    """table.json (every field) and table.md (one row per policy x cohort x class)."""
    latest = {}
    for record in records:
        latest[(record["policy"], record["cohort"])] = record
    ordered = [latest[k] for k in sorted(latest, key=lambda k: (k[1], k[0]))]
    (out / "table.json").write_text(json.dumps({"schema_version": SCHEMA_VERSION,
                                                "tool_sha256": TOOL_SHA256,
                                                "scorer_sha256": sha256(SCORER),
                                                "cells": ordered}, indent=1), encoding="utf-8")
    lines = ["# Shape-class matrix", "",
             f"tool `{TOOL_SHA256[:16]}`, scorer `{sha256(SCORER)[:16]}`, split `{ALLOWED_SPLIT}`; "
             "coverage all / eligible / identified-IV; MAE USD; MAE/spread; containment; IV "
             "median / p95 bp; violations mono/convex/calendar; completeness "
             "complete/>=0.9/>=0.5/>0/none chains; served expiries / fitted expiries and the "
             "reason of every unserved one (slice_reason); expiry fit ms p50/p95 (per-chain wall, "
             "unqualified); refusals = the three largest non-served statuses of eligible rows.", "",
             "| cohort | policy | class | cov all | cov elig | cov IV | MAE USD | MAE/spr "
             "| contain | IV med | IV p95 | viol | completeness | served exp | unserved reasons "
             "| fit ms p50/p95 | refusals | pred sha256 |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for record in ordered:
        classes = record.get("classes")
        if not classes:
            lines.append(f"| {record['cohort']} | {record['policy']} | (cell failed: "
                         f"{record.get('error') or record.get('run')}) |" + " |" * 15)
            continue
        for name, c in classes.items():
            v = c.get("violations") or {}
            refused = sorted(((n, k) for k, n in c["refusal_taxonomy"].items()
                              if k not in qb.PRICE_SERVED_STATUSES), reverse=True)[:3]
            comp = "/".join(str(c["completeness"][k]) for k in COMPLETENESS)
            cost = f"{_fmt(c['expiry_fit_ms_p50'], 'f1')}/{_fmt(c['expiry_fit_ms_p95'], 'f1')}"
            served = f"{c.get('expiries_served', '-')}/{c.get('expiries', '-')}"
            why = ", ".join(f"{k} {n}" for k, n in (c.get("unserved_reasons") or {}).items()) or "-"
            lines.append(
                f"| {record['cohort']} | {record['policy']} | {name} "
                f"| {_fmt(c['all_row_coverage'], 'pct')} "
                f"| {_fmt(c['eligible_price_coverage'], 'pct')} "
                f"| {_fmt(c['identified_iv_coverage'], 'pct')} "
                f"| {_fmt(c['mae_usd'])} | {_fmt(c['mae_spread'])} "
                f"| {_fmt(c['containment'], 'pct')} "
                f"| {_fmt(c['iv_median_bp'], 'f1')} | {_fmt(c['iv_p95_bp'], 'f1')} "
                f"| {v.get('monotone', '-')}/{v.get('convex', '-')}/{v.get('calendar', '-')} "
                f"| {comp} | {served} | {why} | {cost} "
                f"| {', '.join(f'{k} {n}' for n, k in refused) or '-'} "
                f"| `{record.get('prediction_sha256', '')[:16]}` |")
    (out / "table.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_cells(out: Path) -> list[dict]:
    path = out / "cells.jsonl"
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def run(args: argparse.Namespace) -> int:
    spec = load_spec(args.spec)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    records = read_cells(out)
    header = {"spec": str(args.spec.resolve()), "spec_sha256": sha256(args.spec),
              "binary_sha256": sha256(Path(spec["binary"])), "tool_sha256": TOOL_SHA256}
    print(json.dumps({"matrix": header}), flush=True)
    first_failure = {}
    for record in records:
        if not record.get("classes"):
            first_failure.setdefault((record["policy"], record["cohort"]), record)
    failures = 0
    for policy, cohort in spec["cells"]:
        csv = out / "runs" / cohort / policy / f"{cohort}-{policy}.csv"
        failed = first_failure.get((policy, cohort))
        if failed is not None and not csv.exists() and not args.rerun_failed:
            # A failed cell is not re-run on resume (its leftovers would make the re-run fail
            # for another reason); its FIRST failure stays the cell's record.
            record = dict(failed, resumed_failed=True)
        else:
            record = run_cell(spec, out, policy, cohort)
        record.update(binary_sha256=header["binary_sha256"], spec_sha256=header["spec_sha256"],
                      finished_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
        failures += 0 if record.get("classes") else 1
        with (out / "cells.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record) + "\n")
        records.append(record)
        write_table(out, records)
        print(json.dumps({"cell": [policy, cohort], "run": record.get("run"),
                          "prediction_sha256": record.get("prediction_sha256"),
                          "error": record.get("error")}), flush=True)
    return 1 if failures else 0


def table(args: argparse.Namespace) -> int:
    out = args.output.resolve()
    records = read_cells(out)
    if not records:
        raise ValueError(f"{out}: no cells.jsonl")
    write_table(out, records)
    return 0


def _violations(metrics: dict) -> int:
    counts = metrics.get("violations") or {}
    return sum(counts.values()) + (metrics.get("duplicate_disagreements") or 0)


def validated(cell: dict) -> bool:
    """Whether the cell's policy runs C2's publication validation (graded.publication =
    validate in its policy file). A cell without a readable policy file is not validated."""
    path = cell.get("policy_path")
    if not path or not Path(path).is_file():
        return False
    policy = json.loads(Path(path).read_text(encoding="utf-8"))
    return (policy.get("graded") or {}).get("publication") == "validate"


def choose(cells: list[dict], cohort: str, name: str, containment: float,
           validated_only: bool = False):
    """The deterministic winner of one (cohort, class): among policies with zero served shape
    violations and containment >= the bar (and, with validated_only, C2's publication
    validation on), the highest eligible price coverage; ties to 1e-12 go to the lower
    MAE/spread, then the smaller policy name. None when none qualifies."""
    best = None
    for cell in cells:
        metrics = (cell.get("classes") or {}).get(name)
        if cell["cohort"] != cohort or not metrics or not metrics.get("eligible_rows"):
            continue
        if validated_only and not validated(cell):
            continue
        contained = metrics.get("containment")
        if contained is None or contained < containment or _violations(metrics):
            continue
        coverage = round(metrics.get("eligible_price_coverage") or 0.0, 12)
        mae = metrics.get("mae_spread")
        key = (-coverage, float("inf") if mae is None else mae, cell["policy"])
        if best is None or key < best[0]:
            best = (key, cell["policy"])
    return None if best is None else best[1]


def select(args: argparse.Namespace) -> int:
    """Per (cohort, class) winners from one or more harness tables, and the single policy that
    wins the most cells (ties: the smaller name). A per-class route is a single configuration
    only when the winners differ in profile keys alone; surface-level arms (certificate box,
    market distance, ladder, publication) are one per run."""
    cells = []
    for path in args.table:
        cells.extend(c for c in json.loads(path.read_text(encoding="utf-8"))["cells"]
                     if c.get("classes"))
    cohorts = sorted({c["cohort"] for c in cells})
    names = []
    for cell in cells:
        names.extend(n for n in cell["classes"] if n not in names)
    winners = {}
    for cohort in cohorts:
        # Every class with eligible rows in this cohort; None where no policy qualifies.
        winners[cohort] = {name: choose(cells, cohort, name, args.containment,
                                        args.validated_only) for name in names
                           if any((c["classes"].get(name) or {}).get("eligible_rows")
                                  for c in cells if c["cohort"] == cohort)}
    wins: dict = {}
    for by_class in winners.values():
        for policy in by_class.values():
            if policy is not None:
                wins[policy] = wins.get(policy, 0) + 1
    single = min(wins, key=lambda p: (-wins[p], p)) if wins else None
    document = {"schema_version": SCHEMA_VERSION, "tool_sha256": TOOL_SHA256,
                "containment": args.containment, "validated_only": args.validated_only,
                "winners": winners, "wins": wins,
                "single_policy": single,
                "tables": {str(p): sha256(p) for p in args.table}}
    write = args.output.resolve()
    if write.exists():
        raise ValueError("selection output already exists; use a new path")
    write.write_text(json.dumps(document, indent=1), encoding="utf-8")
    print(json.dumps({"single_policy": single, "wins": wins}))
    return 0


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="command", required=True)
    r = sub.add_parser("run", help="run and score every policy x cohort cell of a spec")
    r.add_argument("--spec", required=True, type=Path)
    r.add_argument("--output", required=True, type=Path)
    r.add_argument("--rerun-failed", action="store_true",
                   help="re-run cells whose earlier run failed (default: keep the first failure)")
    t = sub.add_parser("table", help="re-aggregate cells.jsonl into table.md / table.json")
    t.add_argument("--output", required=True, type=Path)
    s = sub.add_parser("select", help="deterministic per-class winners from harness tables")
    s.add_argument("--table", required=True, type=Path, action="append")
    s.add_argument("--output", required=True, type=Path)
    s.add_argument("--containment", type=float, default=0.985)
    s.add_argument("--validated-only", action="store_true",
                   help="admit only policies with graded.publication = validate (C2)")
    return p


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    return {"run": run, "table": table, "select": select}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
