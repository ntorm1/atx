#!/usr/bin/env python3
"""Batch-mode variant of pull_opra_universe_snapshot.py: submit ONE Databento batch
job for the whole parent universe (server-side prep, no interactive gateway), poll
until done, download the DBN shards, and split them into the same
{symbol}/{date}.parquet hive.

Use when the interactive historical gateway is 504-ing. Same $0 flat-rate cost
profile; same output layout and manifest.

  python pull_opra_universe_batch.py --symbols-file ... [--date 2026-07-01]
      [--snap-utc 14:00] [--out data/opra_universe] [--job-id ID] [--poll 30]
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

import databento as db
import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

INT64_MIN = np.iinfo(np.int64).min
DBN_UNDEF = np.iinfo(np.int64).max
DATASET = "OPRA.PILLAR"
SCHEMA = "cbbo-1m"

ARROW_SCHEMA = pa.schema([
    ("ts", pa.timestamp("ns")),
    ("underlying", pa.string()),
    ("symbol", pa.string()),
    ("bid_px", pa.int64()),
    ("ask_px", pa.int64()),
    ("bid_sz", pa.int64()),
    ("ask_sz", pa.int64()),
])


def read_api_key(env_path: pathlib.Path = pathlib.Path(".env")) -> str:
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("DATABENTO_API_KEY"):
            return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise SystemExit("DATABENTO_API_KEY not found in .env")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols-file", type=pathlib.Path, required=True)
    ap.add_argument("--date", default="2026-07-01")
    ap.add_argument("--snap-utc", default="14:00")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("data/opra_universe"))
    ap.add_argument("--job-id", default="", help="resume: skip submit, use this job")
    ap.add_argument("--poll", type=int, default=30)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    symbols = [s.strip() for s in args.symbols_file.read_text().splitlines() if s.strip()]
    root_to_sym = {s.replace(".", ""): s for s in symbols}
    parents = sorted({s.replace(".", "") + ".OPT" for s in symbols})

    start = f"{args.date}T{args.snap_utc}:00"
    hh, mm = args.snap_utc.split(":")
    end_minute = int(hh) * 60 + int(mm) + 1
    end = f"{args.date}T{end_minute // 60:02d}:{end_minute % 60:02d}:00"

    key = read_api_key()
    client = db.Historical(key=key)

    # Batch API caps a job at 2,000 symbols — split the parent list.
    MAX_PER_JOB = 1500
    parent_groups = [parents[i:i + MAX_PER_JOB] for i in range(0, len(parents), MAX_PER_JOB)]

    job_ids: list[str] = [j for j in args.job_id.split(",") if j]
    if not job_ids:
        for gi, group in enumerate(parent_groups):
            job = client.batch.submit_job(
                dataset=DATASET, symbols=group, schema=SCHEMA,
                start=start, end=end, stype_in="parent", encoding="dbn",
                compression="zstd")
            job_ids.append(job["id"])
            print(f"submitted batch job {gi + 1}/{len(parent_groups)} id={job['id']} "
                  f"({len(group)} parents) [{start}Z,{end}Z)", flush=True)

    # ── Poll until all done ───────────────────────────────────────────────────
    pending = set(job_ids)
    while pending:
        try:
            jobs = {j["id"]: j for j in client.batch.list_jobs()}
        except Exception as e:  # noqa: BLE001
            print(f"poll error (retrying): {str(e)[:100]}", file=sys.stderr, flush=True)
            time.sleep(args.poll)
            continue
        for jid in sorted(pending):
            state = (jobs.get(jid) or {}).get("state", "unknown")
            print(f"job {jid}: {state}", flush=True)
            if state == "done":
                pending.discard(jid)
            elif state in ("expired", "failed"):
                print(f"job {jid} terminal state {state}", file=sys.stderr)
                return 1
        if pending:
            time.sleep(args.poll)

    # ── Download shards ───────────────────────────────────────────────────────
    dl_dir = args.out / "_batch" / job_ids[0]
    dl_dir.mkdir(parents=True, exist_ok=True)
    n_dl = 0
    for jid in job_ids:
        files = client.batch.download(job_id=jid, output_dir=dl_dir)
        n_dl += len(files)
    print(f"downloaded {n_dl} files -> {dl_dir}", flush=True)

    # ── Decode + split to hive ────────────────────────────────────────────────
    manifest_rows: list[dict] = []
    seen: set[str] = set()
    n_files = n_unmapped = 0
    snap_ts = pd.Timestamp(f"{args.date}T{args.snap_utc}:00")
    for f in sorted(dl_dir.rglob("*.dbn.zst")):
        store = db.DBNStore.from_file(f)
        df = store.to_df(price_type="fixed", pretty_ts=False, map_symbols=True)
        if df.empty:
            continue
        df = df.reset_index()
        out = pd.DataFrame({
            "ts": pd.Series([snap_ts] * len(df), dtype="datetime64[ns]"),
            "symbol": df["symbol"].astype(str),
            "bid_px": df["bid_px_00"].astype("int64"),
            "ask_px": df["ask_px_00"].astype("int64"),
            "bid_sz": df["bid_sz_00"].astype("int64"),
            "ask_sz": df["ask_sz_00"].astype("int64"),
        })
        out.loc[out["bid_px"] == DBN_UNDEF, "bid_px"] = INT64_MIN
        out.loc[out["ask_px"] == DBN_UNDEF, "ask_px"] = INT64_MIN
        roots = out["symbol"].str[:6].str.strip()
        base = roots.str.replace(r"\d+$", "", regex=True)
        out["underlying"] = base.map(root_to_sym)
        unmapped = out["underlying"].isna()
        n_unmapped += int(unmapped.sum())
        out = out[~unmapped]
        for sym, grp in out.groupby("underlying", sort=False):
            tgt = args.out / str(sym) / f"{args.date}.parquet"
            seen.add(str(sym))
            if tgt.exists() and not args.force:
                manifest_rows.append({"symbol": sym, "records": len(grp), "status": "exists"})
                continue
            tgt.parent.mkdir(parents=True, exist_ok=True)
            tbl = pa.Table.from_pandas(
                grp[["ts", "underlying", "symbol", "bid_px", "ask_px", "bid_sz", "ask_sz"]],
                schema=ARROW_SCHEMA, preserve_index=False)
            pq.write_table(tbl, tgt)
            n_files += 1
            manifest_rows.append({"symbol": sym, "records": len(grp), "status": "ok"})

    n_empty = 0
    for sym in symbols:
        if sym not in seen:
            n_empty += 1
            manifest_rows.append({"symbol": sym, "records": 0, "status": "no_options"})
    mpath = args.out / f"manifest_batch_{args.date}_{args.snap_utc.replace(':', '')}.csv"
    pd.DataFrame(manifest_rows).to_csv(mpath, index=False)
    print(f"DONE files={n_files} symbols_with_data={len(seen)} no_options={n_empty} "
          f"unmapped_records={n_unmapped}", flush=True)
    print(f"manifest: {mpath}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
