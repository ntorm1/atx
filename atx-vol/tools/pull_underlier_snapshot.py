#!/usr/bin/env python3
"""Underlier NBBO at the option snapshot minute — the ``uBid``/``uAsk``/``uPrc``
half of a tblOptionIntradayHist-shaped row.

The OPRA board carries only option quotes. Every downstream consumer that wants
a SpiderRock-shaped row needs the underlier's own quote at the same instant
(``uBid``, ``uAsk``, ``uPrc`` — `oracle_store_metadata.py:27-92`, cols 20-22),
and the fitter wants a spot to anchor the forward against rather than relying
solely on the parity-implied one (`opra_panel.hpp:368-371` falls back to
put-call parity off the earliest co-terminal pair, and `Unavailable` if there
is none).

Same ALL_SYMBOLS trick as `pull_opra_allsym.py`, and the same catch::

    EQUS.MINI bbo-1m     ALL_SYMBOLS 1 min -> 5.4 s,  0.30 MB, 8,908 quoting names
    EQUS.MINI definition ALL_SYMBOLS 1 day -> 13,196 records, 4.8 MB

An ALL_SYMBOLS response carries NO symbology mappings on this dataset either
(``metadata.mappings`` is empty; ``to_df(map_symbols=True)`` returned
``symbol=None`` on all 8,908 rows), so the instrument_id -> ticker map again
comes from that session's ``definition`` schema, cached per session.

Cost — this one is NOT free
---------------------------
Unlike OPRA.PILLAR (flat-rate on this account, every ``get_cost`` returns
$0.00), the equity datasets are metered::

    EQUS.MINI  bbo-1m     ALL_SYMBOLS 1 minute -> $0.0251
    EQUS.MINI  definition ALL_SYMBOLS 1 day    -> $0.0708
    XNAS.BASIC cbbo-1m    ALL_SYMBOLS 1 minute -> $0.0305

So a session costs ~$0.096 the first time and ~$0.025 on any later minute of a
session whose definitions are already cached; a full year of daily snapshots is
roughly $24. The FREE preflight runs before every paid request and the run
REFUSES above ``--cap``.

Index levels are NOT available here
-----------------------------------
``SPX``, ``NDX``, ``RUT``, ``XEO`` and friends are index levels, not securities,
and appear in no equity feed. Those underliers get NO row from this tool, by
construction — their forward has to come from put-call parity off the option
board itself. That is not a defect to work around: it is the reason the
parity-implied forward path exists. This tool covers the ~5,500 equity/ETF
roots; the cash-settled index complex is parity-only.

Output
------
``<root>/date=YYYY-MM-DD/underlier.parquet``, one row per quoting underlier::

    ts            timestamp[ns]  the snapshot minute, naive UTC (matches the board)
    underlying    string         ticker as the definition record spells it
    instrument_id int64
    bid_px        int64          1e-9 fixed point, unset = INT64_MIN
    ask_px        int64          1e-9 fixed point, unset = INT64_MIN
    bid_sz        int64
    ask_sz        int64

Deliberately the same fixed-point convention and the same INT64_MIN unset
sentinel as the OPRA hive (`pull_opra_hive.py:136-144`), so the C++ side reuses
one set of sentinel rules instead of learning a second dialect. The mid is left
to the consumer — this file records what was quoted, not a derived price.

Usage:
  python atx-vol/tools/pull_underlier_snapshot.py \
      --start 2026-08-21 --end 2026-08-21 --snap-et 15:55 \
      --out C:/atx-data/underlier-hive
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import pathlib
import sys
import time
from typing import Optional

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pull_opra_hive import (  # noqa: E402
    read_api_key,
    snap_window,
    snapshot_minute_utc,
    trading_sessions,
)

try:
    import databento as db
except ImportError:  # pragma: no cover
    db = None

INT64_MIN = np.iinfo(np.int64).min
DBN_UNDEF = np.iinfo(np.int64).max
DATASET = "EQUS.MINI"
SCHEMA = "bbo-1m"
DEF_SCHEMA = "definition"
DATE_FILE = "underlier.parquet"

COLUMNS = ["ts", "underlying", "instrument_id", "bid_px", "ask_px", "bid_sz", "ask_sz"]
ARROW_SCHEMA = pa.schema([
    ("ts", pa.timestamp("ns")),
    ("underlying", pa.string()),
    ("instrument_id", pa.int64()),
    ("bid_px", pa.int64()),
    ("ask_px", pa.int64()),
    ("bid_sz", pa.int64()),
    ("ask_sz", pa.int64()),
])
DEF_MAP_SCHEMA = pa.schema([
    ("instrument_id", pa.int64()),
    ("underlying", pa.string()),
])


def _fetch(client, path: pathlib.Path, *, schema: str, start: str, end: str) -> pathlib.Path:
    if path.exists():
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.parent / (path.name + ".tmp")
    last: Optional[Exception] = None
    for attempt in range(5):
        try:
            client.timeseries.get_range(
                dataset=DATASET, symbols="ALL_SYMBOLS", schema=schema,
                start=start, end=end, path=str(tmp))
            os.replace(tmp, path)
            return path
        except Exception as exc:  # noqa: BLE001
            last = exc
            try:
                tmp.unlink(missing_ok=True)
            except OSError:
                pass
            print(f"  fetch retry {attempt + 1} ({schema} {start}): {str(exc)[:90]}",
                  file=sys.stderr)
            time.sleep(min(5 * 2 ** attempt, 60))
    raise RuntimeError(f"{schema} fetch failed for {start}: {last}")


def def_small_path(cache_root: pathlib.Path, date: str) -> pathlib.Path:
    return cache_root / f"{date}.parquet"


def def_raw_path(cache_root: pathlib.Path, date: str) -> pathlib.Path:
    return cache_root / f"{date}.dbn.zst"


def definition_map(client, cache_root: pathlib.Path, date: str) -> pd.DataFrame:
    """instrument_id -> ticker for one session, cached as a 2-column parquet."""
    small = def_small_path(cache_root, date)
    if small.exists():
        return pq.ParquetFile(small).read().to_pandas()
    raw = _fetch(client, def_raw_path(cache_root, date), schema=DEF_SCHEMA,
                 start=f"{date}T00:00:00", end=f"{date}T23:59:59")
    arr = db.DBNStore.from_file(raw).to_ndarray()
    frame = pd.DataFrame({
        "instrument_id": arr["instrument_id"].astype("int64"),
        "underlying": arr["raw_symbol"].astype(str),
    }).drop_duplicates("instrument_id", keep="last").reset_index(drop=True)
    del arr
    tmp = small.with_suffix(".parquet.tmp")
    pq.write_table(pa.Table.from_pandas(frame, schema=DEF_MAP_SCHEMA, preserve_index=False),
                   tmp)
    os.replace(tmp, small)
    return frame


def build_frame(quote_path: pathlib.Path, defs: pd.DataFrame,
                date: str, hm: str) -> tuple[pd.DataFrame, int]:
    arr = db.DBNStore.from_file(quote_path).to_ndarray()
    n_returned = len(arr)
    quotes = pd.DataFrame({
        "instrument_id": arr["instrument_id"].astype("int64"),
        "bid_px": arr["bid_px_00"].astype("int64"),
        "ask_px": arr["ask_px_00"].astype("int64"),
        "bid_sz": arr["bid_sz_00"].astype("int64"),
        "ask_sz": arr["ask_sz_00"].astype("int64"),
    })
    del arr
    frame = quotes.merge(defs, on="instrument_id", how="inner")
    frame.loc[frame["bid_px"] == DBN_UNDEF, "bid_px"] = INT64_MIN
    frame.loc[frame["ask_px"] == DBN_UNDEF, "ask_px"] = INT64_MIN
    frame["ts"] = pd.Timestamp(f"{date}T{hm}:00")
    # One row per ticker. A ticker resolving to several instrument_ids at one
    # instant would otherwise silently fan out the option join downstream; keep
    # the widest-size quote, which is the one a taker would actually see.
    frame["_sz"] = frame["bid_sz"] + frame["ask_sz"]
    frame = (frame.sort_values(["underlying", "_sz"], kind="stable")
                  .drop_duplicates("underlying", keep="last"))
    frame = frame[COLUMNS].sort_values("underlying", kind="stable")
    return frame.reset_index(drop=True), n_returned


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", required=True)
    ap.add_argument("--end", required=True)
    grp = ap.add_mutually_exclusive_group()
    grp.add_argument("--snap-et", default="", help="ET market-clock HH:MM (DST-aware)")
    grp.add_argument("--snap-utc", default="", help="fixed UTC HH:MM")
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--cap", type=float, default=5.0,
                    help="refuse if the FREE preflight estimate exceeds this ($). "
                         "Equity data is METERED, unlike OPRA.")
    ap.add_argument("--prefetch", type=int, default=3)
    ap.add_argument("--env-file", default="")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if db is None:
        print("BLOCKED: databento is not installed.", file=sys.stderr)
        return 3
    if not args.snap_et and not args.snap_utc:
        args.snap_et = "15:55"

    dates = trading_sessions(args.start, args.end)
    if not dates:
        print(f"no XNYS sessions in [{args.start}, {args.end}]", file=sys.stderr)
        return 2
    minute = {d: (snapshot_minute_utc(d, args.snap_et) if args.snap_et else args.snap_utc)
              for d in dates}
    todo = [d for d in dates
            if args.force or not (args.out / f"date={d}" / DATE_FILE).exists()]
    print(f"sessions={len(dates)} to_pull={len(todo)} "
          f"minute={'ET ' + args.snap_et if args.snap_et else 'UTC ' + args.snap_utc}")
    if not todo:
        print("nothing to do (every requested session is already on disk)")
        return 0

    client = db.Historical(key=read_api_key(args.env_file))
    ddir = args.out / "_defs"
    qdir = args.out / "_dbn"
    ddir.mkdir(parents=True, exist_ok=True)
    qdir.mkdir(parents=True, exist_ok=True)

    est = 0.0
    for d in todo:
        s, e = snap_window(d, minute)
        est += float(client.metadata.get_cost(
            dataset=DATASET, symbols="ALL_SYMBOLS", schema=SCHEMA, start=s, end=e))
        if not def_small_path(ddir, d).exists():
            est += float(client.metadata.get_cost(
                dataset=DATASET, symbols="ALL_SYMBOLS", schema=DEF_SCHEMA,
                start=f"{d}T00:00:00", end=f"{d}T23:59:59"))
    print(f"PREFLIGHT: est=${est:.4f} over {len(todo)} session(s) (cap ${args.cap:.2f})")
    if est > args.cap:
        print(f"REFUSED: ${est:.4f} exceeds cap ${args.cap:.2f}. Nothing pulled.",
              file=sys.stderr)
        return 3
    if args.dry_run:
        print("DRY RUN: no data pulled.")
        return 0

    def q_path(d: str) -> pathlib.Path:
        return qdir / f"{d}_{minute[d].replace(':', '')}.dbn.zst"

    rows_total, failed = 0, []
    t_run = time.perf_counter()
    with cf.ThreadPoolExecutor(max_workers=max(1, args.prefetch)) as pool:
        futs, dfuts = {}, {}
        for d in todo:
            s, e = snap_window(d, minute)
            futs[d] = pool.submit(_fetch, client, q_path(d), schema=SCHEMA, start=s, end=e)
            if not def_small_path(ddir, d).exists():
                dfuts[d] = pool.submit(_fetch, client, def_raw_path(ddir, d),
                                       schema=DEF_SCHEMA, start=f"{d}T00:00:00",
                                       end=f"{d}T23:59:59")
        for i, d in enumerate(todo, 1):
            t0 = time.perf_counter()
            try:
                qp = futs[d].result()
                if d in dfuts:
                    dfuts[d].result()
                defs = definition_map(client, ddir, d)
                frame, n_ret = build_frame(qp, defs, d, minute[d])
            except Exception as exc:  # noqa: BLE001
                print(f"  [{i}/{len(todo)}] {d}: FAILED {str(exc)[:110]}", file=sys.stderr)
                failed.append(d)
                continue
            target = args.out / f"date={d}" / DATE_FILE
            target.parent.mkdir(parents=True, exist_ok=True)
            tmp = target.with_suffix(".parquet.tmp")
            pq.write_table(pa.Table.from_pandas(frame[COLUMNS], schema=ARROW_SCHEMA,
                                                preserve_index=False), tmp)
            os.replace(tmp, target)
            rows_total += len(frame)
            print(f"  [{i}/{len(todo)}] {d} @{minute[d]}Z  provider={n_ret:,d} "
                  f"tickers={len(frame):,d}  ({time.perf_counter() - t0:.1f}s)")

    print(f"DONE sessions={len(todo) - len(failed)}/{len(todo)} rows={rows_total:,d} "
          f"elapsed={time.perf_counter() - t_run:.1f}s")
    if failed:
        print(f"FAILED sessions ({len(failed)}): {', '.join(failed)}", file=sys.stderr)
        return 5
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
