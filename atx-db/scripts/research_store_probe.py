"""Research store scale probe (node 1.9): one real feature from the retained vendor file, as capped workers.

Stage ``lake`` exports the month-end bars of ``TickerHistory3.parquet`` (2012-04 .. 2026-08) into a lake
snapshot (L1); stage ``feature`` builds ``prc_log`` (price at the month-end session; log_winsor_z, sign -1,
positive price required) from that snapshot into the Parquet feature store (L2, basis ``probe``) with R2b's
transforms, then times a scan. ``run`` launches both stages through ``workers.run_jobs`` (each under the
memory guard, 0.6 GiB cap; admission and queueing are the guard's, ruling C-58) and prints every measurement
with the job's native peak committed memory. ``run`` itself is an orchestrator: start it under the guard with
``run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 -- python ... run``.

The month-end session is the last bar date of the month with >= 1,000 bars (a probe rule: stray bars on
exchange closures are few lines; node 1.11 supplies the canonical XNYS calendar). No warehouse is opened.

    python scripts/research_store_probe.py run
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path

DEFAULT_TH3 = Path("C:/atx/atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet")
START, END = dt.date(2012, 4, 1), dt.date(2026, 8, 31)
MIN_SESSION_BARS = 1000
BASIS, FEATURE = "probe", "prc_log"


def _stage_lake(th3: Path, root: Path, snapshot: str, out: Path) -> None:
    from atx_db.research import research_lake as lake

    source = lake.sql_text(th3.as_posix())
    select = f"""
        WITH counts AS (
            SELECT tradingDate AS session, count(*) AS bars FROM read_parquet({source})
            WHERE tradingDate BETWEEN DATE '{START}' AND DATE '{END}' GROUP BY 1
        ), month_end AS (
            SELECT max(session) AS session FROM counts WHERE bars >= {MIN_SESSION_BARS}
            GROUP BY date_trunc('month', session)
        )
        SELECT b.tradingDate AS session, last_day(b.tradingDate) AS eom,
               'TBLTICKERHISTORY-' || CAST(b.securityID AS VARCHAR) AS line_id, b.securityID AS vendor_id,
               CAST(b.close AS DOUBLE) AS close, CAST(b.volume AS DOUBLE) AS volume, b.shares,
               year(b.tradingDate) AS year
        FROM read_parquet({source}) b JOIN month_end m ON b.tradingDate = m.session
    """
    sha_file = th3.with_name(th3.name + ".sha256")
    th3_sha = sha_file.read_text(encoding="utf-8").split()[0] if sha_file.is_file() else None
    started = time.perf_counter()
    path = lake.export_lake_snapshot(None, snapshot, {"bars_month_end": select}, root=root, source_meta={
        "retained_file": th3.as_posix(), "retained_sha256": th3_sha, "bytes": th3.stat().st_size,
        "session_rule": f"last bar date of the month with >= {MIN_SESSION_BARS} bars (probe)"})
    manifest = lake.lake_manifest(snapshot, root)
    entry = manifest["datasets"]["bars_month_end"]
    con = lake.connect_bounded(None, root=root)
    dupes = con.execute(
        "SELECT count(*) - count(DISTINCT (eom, line_id)) FROM read_parquet(?, hive_partitioning=true)",
        [lake.lake_files(snapshot, "bars_month_end", root=root)]).fetchone()[0]
    months = con.execute("SELECT count(DISTINCT eom) FROM read_parquet(?, hive_partitioning=true)",
                         [lake.lake_files(snapshot, "bars_month_end", root=root)]).fetchone()[0]
    con.close()
    out.write_text(json.dumps({
        "stage": "lake", "snapshot": snapshot, "path": path.as_posix(), "rows": entry["rows"], "months": months,
        "duplicate_eom_line_rows": int(dupes), "files": len(entry["files"]),
        "bytes": sum(f["bytes"] for f in entry["files"]), "dataset_sha256": entry["dataset_sha256"],
        "seconds": round(time.perf_counter() - started, 2)}, indent=1), encoding="utf-8")


def _stage_feature(root: Path, snapshot: str, out: Path) -> None:
    import pandas as pd

    from atx_db.research import feature_store as fs
    from atx_db.research import features as rf
    from atx_db.research import research_lake as lake

    policy = rf.StandardizationPolicy()
    catalog_row = {"feature_id": FEATURE, "definition": "close at the month-end session", "expected_sign": -1,
                   "preferred_transform": "log_winsor_z", "domain": "positive_value_required", "theme": "size",
                   "universe": "every vendor line with a bar at the month-end session (probe, no type filter)"}
    digest = lake.lake_dataset_digest(snapshot, "bars_month_end", root)
    sha = fs.compute_feature_sha(FEATURE, fs.catalog_row_payload(catalog_row, policy),
                                 fs.store_code_digest(Path(__file__)), {"bars_month_end": digest})
    store = fs.FeatureStore(root)
    con = lake.connect_bounded(None, root=root)
    years = lake.lake_manifest(snapshot, root)["datasets"]["bars_month_end"]["years"]
    per_year: dict[int, int] = {}

    def chunks():
        for year in years:
            files = lake.lake_files(snapshot, "bars_month_end", [year], root)
            frame = con.execute("""
                SELECT eom, line_id, close AS raw, session FROM read_parquet(?, hive_partitioning=true)
                QUALIFY row_number() OVER (PARTITION BY eom, line_id ORDER BY close, volume) = 1
                ORDER BY eom, line_id
            """, [files]).df()
            positive = frame["raw"] > 0
            frame["status"] = "in_domain"
            frame.loc[~positive & frame["raw"].notna(), "status"] = "nonpositive_value"
            frame.loc[frame["raw"].isna(), "status"] = "nonfinite_value"
            frame["owner_id"] = None
            frame["available_at"] = pd.to_datetime(frame["session"]) + pd.Timedelta(hours=22)
            per_year[int(year)] = len(frame)
            yield frame[["eom", "line_id", "owner_id", "raw", "status", "available_at"]]

    started = time.perf_counter()
    path = fs.build_feature(store, BASIS, FEATURE, sha, chunks(), expected_sign=-1, log_base=True, policy=policy,
                            meta={"lake_snapshot": snapshot, "catalog_row": catalog_row})
    write_seconds = time.perf_counter() - started
    meta = store.read_meta(BASIS, FEATURE, sha)
    t0 = time.perf_counter()
    one_year = store.scan(BASIS, [FEATURE], years=[2020]).aggregate("count(*), avg(z)").fetchone()
    scan_year_s = time.perf_counter() - t0
    t0 = time.perf_counter()
    full = store.scan(BASIS, [FEATURE]).aggregate(
        "count(*), count(*) FILTER (WHERE reason = 0), count(DISTINCT eom), min(eom), max(eom)").fetchone()
    scan_full_s = time.perf_counter() - t0
    monthly = con.execute(f"""
        SELECT min(n), median(n), max(n) FROM (
            SELECT eom, count(*) FILTER (WHERE reason = 0) AS n FROM read_parquet('{path.as_posix()}') GROUP BY eom)
    """).fetchone()
    con.close()
    store.close()
    out.write_text(json.dumps({
        "stage": "feature", "feature_sha": sha, "path": path.as_posix(), "rows": meta["rows"],
        "value_rows": meta["value_rows"], "reasons": meta["reasons"], "bytes": meta["bytes"],
        "mib": round(meta["bytes"] / 2**20, 2), "bytes_per_row": round(meta["bytes"] / meta["rows"], 2),
        "write_seconds": round(write_seconds, 2), "store_write_seconds": meta["write_seconds"],
        "rows_per_year": per_year, "valid_names_per_month_min_median_max": [monthly[0], monthly[1], monthly[2]],
        "scan_2020_rows": one_year[0], "scan_2020_seconds": round(scan_year_s, 3),
        "scan_full": {"rows": full[0], "valid": full[1], "months": full[2], "eom_min": str(full[3]),
                      "eom_max": str(full[4]), "seconds": round(scan_full_s, 3)}}, indent=1), encoding="utf-8")


def _run(args: argparse.Namespace) -> int:
    from atx_db.research.workers import EXIT_HEADROOM, run_jobs

    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d%H%M")
    snapshot = args.snapshot or f"probe-th3-month-end-{stamp}"
    work = Path(args.root) / "probe" / snapshot
    work.mkdir(parents=True, exist_ok=True)
    script = Path(__file__).resolve()
    common = ["--root", str(args.root), "--snapshot", snapshot]
    stages = [("lake", [sys.executable, str(script), "stage-lake", "--th3", str(args.th3), *common,
                        "--out", str(work / "lake.json")]),
              ("feature", [sys.executable, str(script), "stage-feature", *common, "--out", str(work / "feature.json")])]
    summary = {"snapshot": snapshot, "job_gb": args.job_gb, "stages": {}}
    for name, argv in stages:
        stopped = []
        for attempt in range(1, 6):  # a guard in-run stop is re-run; the guard queues it for re-admission
            receipts: list[dict] = []
            codes = run_jobs([argv], max_workers=1, job_gb=args.job_gb, receipts=receipts,
                             log_dir=work / "logs" / f"{name}-{attempt}")
            if codes[0] != EXIT_HEADROOM:
                break
            stopped.append(receipts[0])
            time.sleep(60)
        out = work / f"{name}.json"
        summary["stages"][name] = {"exit": codes[0], "attempts": attempt, "host_floor_stops": stopped,
                                   "receipt": receipts[0],
                                   "result": json.loads(out.read_text(encoding="utf-8")) if out.is_file() else None}
        if codes[0] != 0:
            break
    (work / "summary.json").write_text(json.dumps(summary, indent=1, default=str), encoding="utf-8")
    print(json.dumps(summary, indent=1, default=str))
    return 0 if all(s["exit"] == 0 for s in summary["stages"].values()) else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("run", "stage-lake", "stage-feature"))
    parser.add_argument("--th3", type=Path, default=DEFAULT_TH3)
    parser.add_argument("--root", type=Path, default=Path("C:/atx/atx-db/data/research"))
    parser.add_argument("--snapshot")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--job-gb", type=float, default=0.6)
    args = parser.parse_args(argv)
    if args.command == "run":
        return _run(args)
    if not args.snapshot or args.out is None:
        parser.error("stages need --snapshot and --out")
    if args.command == "stage-lake":
        _stage_lake(args.th3, args.root, args.snapshot, args.out)
    else:
        _stage_feature(args.root, args.snapshot, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
