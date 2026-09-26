"""Price wave inputs (tier-1 v2 node 1.12): natives, monthly spine and provisional labels, as capped workers.

Every stage runs through ``research.workers.run_jobs`` (each job under the memory guard: 0.6 GiB job cap,
DuckDB 256MB / 1 thread; admission and FIFO queueing are the guard's, ruling C-58); a job the guard stopped
in-run (exit 137) is re-run. Start ``run`` under the guard as an orchestrator: ``run_memory_guarded.py
--job-gb 0.2 --allow-nested-guards --wait-minutes 30 -- python scripts/research_price_wave.py run ...``.
No warehouse is opened: the inputs are the
retained ``TickerHistory3.parquet`` and the X.3 benchmark files. No forward return is read against a
feature: labels are written to the label matrix and only counted (ruling R-6).

    python scripts/research_price_wave.py run [--stages units,bars,...] [--years 2012-2026] [--workers 2]
    python scripts/research_price_wave.py coverage     # the report tables from the stored outputs

Stages (in order): units, dups, ids, bars, lines, bench, spine, market, line_month, natives, labels, store,
manifest, lake, coverage (``ids``: the rows without a positive vendor securityID, by labeled exclusion
reason, ruling C-57).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path("C:/atx/atx-db/data/research")
TH3 = Path("C:/atx/atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet")
BENCH_SNAPSHOT = "bench-20260926"
STAGES = ("units", "dups", "ids", "bars", "lines", "bench", "spine", "market", "line_month", "natives", "labels",
          "store", "manifest", "lake", "coverage")
YEARS = tuple(range(2012, 2027))
LABEL_YEARS = tuple(range(2012, 2024))
FEATURE_GROUPS = 4


def _jobs(args: argparse.Namespace, stage: str, argvs: list[list[str]], summary: dict[str, Any], *,
          raw: bool = False) -> None:
    from atx_db.research.workers import EXIT_HEADROOM, EXIT_NOT_ADMITTED, run_jobs

    script = str(Path(__file__).resolve())
    common = ["--run", args.run, "--root", str(args.root), "--th3", str(args.th3), "--buckets", str(args.buckets)]
    full = [list(argv) if raw else [sys.executable, script, "stage", stage, *argv, *common] for argv in argvs]
    log_dir = Path(args.root) / "work" / "price_wave" / args.run / "logs" / stage
    pending = list(range(len(full)))
    results: dict[int, dict[str, Any]] = {}
    for attempt in range(1, 6):
        receipts: list[dict[str, Any]] = []
        started = time.perf_counter()
        codes = run_jobs([full[i] for i in pending], max_workers=args.workers, job_gb=args.job_gb,
                         receipts=receipts, log_dir=log_dir / f"attempt-{attempt}")
        for index, code, receipt in zip(pending, codes, receipts, strict=True):
            results[index] = {"argv": argvs[index], "exit": code, "peak_gb": receipt.get("peak_job_memory_gb"),
                              "seconds": receipt.get("seconds"), "wait_seconds": receipt.get("wait_seconds"),
                              "attempt": attempt, "status": receipt.get("status")}
        # 137: stopped in-run by the guard; 78: not admitted within the wait (C-58): both are re-run.
        pending = [i for i in pending if results[i]["exit"] in (EXIT_HEADROOM, EXIT_NOT_ADMITTED)]
        print(f"[{stage}] attempt {attempt}: {len(codes)} jobs in {time.perf_counter() - started:.0f} s, "
              f"exits {sorted({results[i]['exit'] for i in results})}", flush=True)
        if not pending:
            break
        time.sleep(60)
    failed = {i: r for i, r in results.items() if r["exit"] != 0}
    peaks = [r["peak_gb"] for r in results.values() if r["peak_gb"] is not None]
    summary["stages"][stage] = {"jobs": len(full), "failed": len(failed), "peak_gb_max": max(peaks, default=None),
                                "results": [results[i] for i in sorted(results)]}
    _save(args, summary)
    if failed:
        raise SystemExit(f"stage {stage}: {len(failed)} jobs failed: {list(failed.values())[:3]} (logs {log_dir})")


def _save(args: argparse.Namespace, summary: dict[str, Any]) -> None:
    from atx_db.research import spine

    spine.write_json(spine.work_dir(args.root, args.run) / "summary.json", summary)


def _years(text: str) -> tuple[int, ...]:
    if "-" in text:
        low, high = (int(x) for x in text.split("-", 1))
        return tuple(range(low, high + 1))
    return tuple(int(x) for x in text.split(","))


def _run(args: argparse.Namespace) -> int:
    from atx_db.research import spine

    work = spine.work_dir(args.root, args.run)
    work.mkdir(parents=True, exist_ok=True)
    path = work / "summary.json"
    summary = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {"run": args.run, "stages": {}}
    summary["started_at"] = dt.datetime.now(dt.UTC).isoformat()
    stages = STAGES if args.stages == "all" else tuple(args.stages.split(","))
    years = _years(args.years)
    for stage in stages:
        if stage not in STAGES:
            raise SystemExit(f"unknown stage {stage!r}")
        if stage == "bars":
            _jobs(args, stage, [["--bucket", str(b)] for b in range(args.buckets)], summary)
        elif stage in ("spine", "line_month", "natives"):
            _jobs(args, stage, [["--year", str(y)] for y in years], summary)
        elif stage == "labels":
            # The orchestrator runs at a 0.2 GiB cap: the spec, the seal and the manifest are worker jobs
            # (each under its own 0.6 guard) that leave a JSON the orchestrator reads.
            _jobs(args, "labels_prepare", [[]], summary)
            prepared = spine.read_json(work / "labels" / "prepared.json")
            summary["label_sha"], summary["label_spec"] = prepared["label_sha"], prepared["spec"]
            _save(args, summary)
            _jobs(args, stage, [["--year", str(y), "--label-sha", summary["label_sha"]]
                                for y in years if y in LABEL_YEARS], summary)
            _jobs(args, "labels_complete", [["--label-sha", summary["label_sha"]]], summary)
            summary["label_complete"] = spine.read_json(work / "labels" / "complete.json")
            _save(args, summary)
        elif stage == "store":
            from atx_db.research.price_natives import NATIVES

            ids = [n.feature_id for n in NATIVES]
            groups = [ids[i::FEATURE_GROUPS] for i in range(FEATURE_GROUPS)]
            _jobs(args, stage, [["--features", ",".join(g)] for g in groups if g], summary)
        elif stage == "manifest":
            _jobs(args, stage, [[]], summary)
            summary["manifest"] = spine.read_json(work / "manifest.json")
            _save(args, summary)
        elif stage == "bench":
            # X.3 C1: the owed re-parse (the committed loader; new manifest), then check + register.
            fetch = str(Path(__file__).resolve().with_name("fetch_benchmarks.py"))
            _jobs(args, "bench_parse", [[sys.executable, fetch, "--parse-only"]], summary, raw=True)
            _jobs(args, stage, [[]], summary)
        else:
            _jobs(args, stage, [[]], summary)
    summary["finished_at"] = dt.datetime.now(dt.UTC).isoformat()
    _save(args, summary)
    print(json.dumps({k: v for k, v in summary.items() if k != "stages"}, indent=1, default=str))
    for name, item in summary["stages"].items():
        print(f"{name}: jobs {item.get('jobs')} failed {item.get('failed')} peak {item.get('peak_gb_max')} GiB")
    return 0


def _prepare_labels(args: argparse.Namespace, work: Path) -> None:
    """Worker: the provisional label spec, its sha, ``_label.json`` and the windows (``labels/prepared.json``)."""
    import pyarrow as pa

    from atx_db.research import spine
    from atx_db.research.label_matrix import WINDOW_SCHEMA, LabelMatrix, compute_label_sha

    spec = spine.label_spec(work, args.th3)
    label_sha = compute_label_sha(spec)
    matrix = LabelMatrix(args.root)
    matrix.create(label_sha, spec)
    windows = spine.label_windows()
    table = pa.table({"eom": [w[0] for w in windows], "h": [w[1] for w in windows],
                      "formation_date": [w[2] for w in windows], "entry_date": [w[3] for w in windows],
                      "exit_date": [w[4] for w in windows]}).cast(WINDOW_SCHEMA)
    matrix.write_windows(label_sha, table)
    matrix.close()
    spine.write_json(work / "labels" / "prepared.json", {"label_sha": label_sha, "spec": spec})


def _complete_labels(args: argparse.Namespace, work: Path) -> None:
    """Worker: seal the label set once every (h, formation year) of the grid is written (1.9: reads need it)."""
    from atx_db.research import spine
    from atx_db.research.label_matrix import LabelMatrix

    matrix = LabelMatrix(args.root)
    grid = sorted(LABEL_YEARS)
    missing = [(h, y) for h in spine.LABEL_HORIZONS for y in grid if not matrix.has(args.label_sha, h, y)]
    if missing:
        raise SystemExit(f"label set {args.label_sha[:12]} is missing {missing}; not sealed")
    path = matrix.complete(args.label_sha, spine.LABEL_HORIZONS, grid)
    matrix.close()
    spine.write_json(work / "labels" / "complete.json", {"label_sha": args.label_sha, "path": str(path),
                                                         "horizons": list(spine.LABEL_HORIZONS), "years": grid})


def _manifest(args: argparse.Namespace, work: Path) -> None:
    """Worker: the w1_price wave manifest of the stored natives (``manifest.json``)."""
    from atx_db.research import feature_store as fs
    from atx_db.research import spine
    from atx_db.research.price_natives import WAVE

    entries = []
    for path in sorted((work / "store").glob("group-*.json")):
        entries += spine.read_json(path)["stats"]["entries"]
    manifest_sha = fs.write_manifest(args.root, WAVE, [{k: e[k] for k in ("basis", "feature_id", "feature_sha")}
                                                       for e in entries])
    spine.write_json(work / "manifest.json", {"wave": WAVE, "manifest_sha": manifest_sha, "features": len(entries)})


# ---------------------------------------------------------------------------
# Worker entry points
# ---------------------------------------------------------------------------

def _stage(args: argparse.Namespace) -> int:
    from atx_db.research import price_natives, spine

    work = spine.work_dir(args.root, args.run)
    started = time.perf_counter()
    name = args.stage_name
    if name == "units":
        spine.stage_units(args.th3, work)
    elif name == "dups":
        spine.duplicate_key_stats(args.th3, work)
    elif name == "ids":
        spine.unidentified_rows_stats(args.th3, work)
    elif name == "bars":
        spine.stage_bars(args.th3, work, args.bucket, args.buckets)
    elif name == "lines":
        spine.stage_lines(work)
    elif name == "bench":
        _stage_bench(args, work)
    elif name == "spine":
        spine.stage_spine(work, args.year, _breakpoints(args))
    elif name == "market":
        spine.stage_market(work)
    elif name == "line_month":
        spine.stage_line_month(work, args.year)
    elif name == "natives":
        price_natives.stage_natives(work, args.year, args.buckets)
    elif name == "labels_prepare":
        _prepare_labels(args, work)
    elif name == "labels":
        spine.stage_labels(work, args.year, args.label_sha, args.root)
    elif name == "labels_complete":
        _complete_labels(args, work)
    elif name == "manifest":
        _manifest(args, work)
    elif name == "store":
        _stage_store(args, work)
    elif name == "lake":
        _stage_lake(args, work)
    elif name == "coverage":
        _stage_coverage(args, work)
    else:
        raise SystemExit(f"unknown stage {name!r}")
    print(f"[{name}] done in {time.perf_counter() - started:.1f} s", flush=True)
    return 0


def _breakpoints(args: argparse.Namespace) -> list[str]:
    from atx_db.research import research_lake as lake

    return lake.lake_files(BENCH_SNAPSHOT, "bench_french_me_breakpoints", root=args.root)


def _stage_bench(args: argparse.Namespace, work: Path) -> None:
    """X.3 C1: check the re-parsed benchmark manifest (no ``year`` column, all present), register it in the lake."""
    from atx_db.research import benchmarks_load, research_lake, spine

    started = time.perf_counter()
    manifest = benchmarks_load.load_manifest()["datasets"]
    problems = [f"{name}: {entry.get('status')}" for name, entry in manifest.items() if entry.get("status") != "present"]
    with_year = [name for name, entry in manifest.items() if any(col == "year" for col, _ in entry.get("schema", []))]
    if problems or with_year:
        raise SystemExit(f"benchmark re-parse: {problems} / datasets with a 'year' column {with_year}")
    specs = benchmarks_load.lake_registration_specs()
    registered = research_lake.register_benchmarks(BENCH_SNAPSHOT, specs, root=args.root)
    lake = research_lake.lake_manifest(BENCH_SNAPSHOT, args.root)
    verify = research_lake.verify_lake_snapshot(BENCH_SNAPSHOT, args.root)
    stats = {"datasets": {name: {"rows": entry["rows"], "parquet_sha256": entry["parquet_sha256"],
                                 "date_min": entry.get("date_min"), "date_max": entry.get("date_max")}
                          for name, entry in manifest.items()},
             "snapshot": BENCH_SNAPSHOT, "registered": sorted(registered), "lake_snapshot_sha256":
             lake.get("snapshot_sha256"), "lake_rows": {n: d["rows"] for n, d in lake["datasets"].items()},
             "verify": verify}
    spine.finish_receipt(work / "bench.json", "x3-reparse", {"snapshot": BENCH_SNAPSHOT}, [], stats, started)


def _stage_store(args: argparse.Namespace, work: Path) -> None:
    from atx_db.research import price_natives, spine

    started = time.perf_counter()
    ids = args.features.split(",")
    result = price_natives.build_store(work, ids, args.root, args.th3)
    (work / "store").mkdir(parents=True, exist_ok=True)
    spine.finish_receipt(work / "store" / f"group-{ids[0]}.json", price_natives.natives_code_digest(),
                         {"features": ids}, [], result, started)


def _stage_lake(args: argparse.Namespace, work: Path) -> None:
    """Export ``spine_monthly`` (JKP layout), ``market_ew_daily`` and ``line_types`` as one sealed lake snapshot."""
    from atx_db.research import research_lake as lake
    from atx_db.research import spine

    started = time.perf_counter()
    digest = spine.files_digest(work, "base/year=*.parquet")
    snapshot = f"price-wave-{spine.th3_sha256(args.th3)[:12]}-{digest[:12]}"
    receipt = work / "lake.json"
    if receipt.is_file() and spine.read_json(receipt)["stats"].get("snapshot") == snapshot:
        return
    base = spine.parquet_list(spine.base_files(work))
    lines = spine.sql_text((work / "lines.parquet").as_posix())
    # type_basis and vendor_earnings_evidence let a consumer evaluate the symmetric sub-universe (lines with
    # vendor earnings evidence, alive or delisted alike): the directory admits living lines by name, while a
    # delisted line needs earnings evidence, so the micro-cap tail without evidence is survivor-tilted.
    datasets = {
        "spine_monthly": f"""
            SELECT s.eom, s.line_id, s.price, s.shares_lagged, s.me_line, s.ret_1m, s.dollar_volume_21d,
                   s.n_sessions_21d, s.universe_basis, s.size_grp, s.formation_date, s.has_session_bar,
                   s.security_type, s.me_basis, s.shares_lag_date, s.size_basis, l.type_basis,
                   coalesce(l.max_earn_504d, 0) > 0 AS vendor_earnings_evidence, year(s.eom) AS year
            FROM read_parquet({base}) s JOIN read_parquet({lines}) l USING (line_id)
            WHERE s.eom >= DATE '{spine.FIRST_EOM}' ORDER BY s.eom, s.line_id""",
        "market_ew_daily": f"""
            SELECT session, sno, prev_eom, m, lm, m_unclipped, names, clipped, extreme, r_min, r_max, market_basis,
                   year
            FROM read_parquet('{(work / 'market.parquet').as_posix()}') ORDER BY sno""",
        "line_types": f"""
            SELECT line_id, first_date, last_date, bars, current_symbol, last_symbol, security_type, type_basis,
                   directory_name, eligible, 0 AS year
            FROM read_parquet('{(work / 'lines.parquet').as_posix()}') ORDER BY line_id""",
    }
    path = lake.export_lake_snapshot(None, snapshot, datasets, root=args.root, source_meta={
        "retained_file": args.th3.as_posix(), "retained_sha256": spine.th3_sha256(args.th3),
        "spine_version": spine.SPINE_VERSION, "code_digest": spine.spine_code_digest(), "work_run": args.run,
        "universe_basis": spine.UNIVERSE_BASIS, "me_basis": spine.ME_BASIS, "size_basis": spine.SIZE_BASIS})
    manifest = lake.lake_manifest(snapshot, args.root)
    stats = {"snapshot": snapshot, "path": path.as_posix(), "snapshot_sha256": manifest["snapshot_sha256"],
             "datasets": {n: {"rows": d["rows"], "dataset_sha256": d["dataset_sha256"], "files": len(d["files"])}
                          for n, d in manifest["datasets"].items()}}
    spine.finish_receipt(receipt, spine.spine_code_digest(), {"snapshot": snapshot}, [], stats, started)


def _stage_coverage(args: argparse.Namespace, work: Path) -> None:
    """Coverage per feature x year (value rows per month over the spine), label coverage, terminal_pending."""
    from atx_db.research import price_natives, spine
    from atx_db.research.label_matrix import LabelMatrix

    started = time.perf_counter()
    con = spine.connect(work, "coverage")
    features: dict[str, Any] = {}
    try:
        for path in sorted((work / "store").glob("group-*.json")):
            for entry in spine.read_json(path)["stats"]["entries"]:
                rows = con.execute(f"""
                    SELECT year(eom), count(DISTINCT eom), min(v), median(v), max(v), min(u),
                           count(*) FILTER (WHERE v >= 4500)
                    FROM (SELECT eom, count(*) FILTER (WHERE reason = 0) AS v, count(*) AS u
                          FROM read_parquet('{entry['path']}') GROUP BY eom)
                    GROUP BY 1 ORDER BY 1
                """).fetchall()
                features[entry["feature_id"]] = {"feature_sha": entry["feature_sha"], "reasons": entry["reasons"],
                                                 "years": {str(y): {"months": int(m), "min": int(lo),
                                                                    "median": float(md), "max": int(hi),
                                                                    "universe_min": int(um),
                                                                    "months_ge_4500": int(ge)}
                                                           for y, m, lo, md, hi, um, ge in rows}}
        universe = con.execute(f"""
            SELECT year(eom), count(DISTINCT eom), min(n), median(n), max(n), min(sb), median(sb)
            FROM (SELECT eom, count(*) AS n, count(*) FILTER (WHERE has_session_bar) AS sb
                  FROM read_parquet({spine.parquet_list(spine.base_files(work))})
                  WHERE eom >= DATE '{spine.FIRST_EOM}' GROUP BY eom)
            GROUP BY 1 ORDER BY 1
        """).fetchall()
    finally:
        con.close()
        spine.drop_scratch(work, "coverage")
    summary = json.loads((work / "summary.json").read_text(encoding="utf-8")) if (work / "summary.json").is_file() \
        else {}
    labels: dict[str, Any] = {}
    label_sha = summary.get("label_sha")
    if label_sha:
        matrix = LabelMatrix(args.root)
        for h in (1, 3, 6, 12):
            for year in LABEL_YEARS:
                path = matrix.path(label_sha, h, year).with_suffix(".json")
                if path.is_file():
                    side = json.loads(path.read_text(encoding="utf-8"))
                    labels.setdefault(str(h), {})[str(year)] = {"rows": side["rows"], "reasons": side["reasons"]}
        matrix.close()
    stats = {"features": features, "universe": [
        {"year": int(y), "months": int(m), "min": int(lo), "median": float(md), "max": int(hi),
         "session_bar_min": int(sbl), "session_bar_median": float(sbm)} for y, m, lo, md, hi, sbl, sbm in universe],
        "labels": {"label_sha": label_sha, "by_horizon_year": labels}, "natives": len(price_natives.NATIVES)}
    spine.finish_receipt(work / "coverage.json", price_natives.natives_code_digest(), {}, [], stats, started)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("run", "stage"))
    parser.add_argument("stage_name", nargs="?")
    parser.add_argument("--run", default="th3-20260920")
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--th3", type=Path, default=TH3)
    parser.add_argument("--stages", default="all")
    parser.add_argument("--years", default=f"{YEARS[0]}-{YEARS[-1]}")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--job-gb", type=float, default=0.6)
    parser.add_argument("--buckets", type=int, default=16)
    parser.add_argument("--bucket", type=int)
    parser.add_argument("--year", type=int)
    parser.add_argument("--label-sha")
    parser.add_argument("--features")
    args = parser.parse_args(argv)
    if args.command == "run":
        return _run(args)
    return _stage(args)


if __name__ == "__main__":
    raise SystemExit(main())
