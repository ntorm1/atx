#!/usr/bin/env python3
"""Fast whole-universe OPRA cbbo-1m snapshot pull (ALL_SYMBOLS path).

Why this exists (measured 2026-08-23, OPRA.PILLAR)
--------------------------------------------------
``pull_opra_hive.py`` asks the provider to resolve the universe server-side:
one ``get_range`` per date with ``stype_in="parent"`` over every requested
parent (``AAPL.OPT``, ...). That resolution is the bottleneck, and it does not
scale -- measured on the 616-name xsec universe at 2026-08-21T19:55Z::

      1 parent   ->   3.2 s
      5 parents  ->  14.3 s
     25 parents  ->  57.7 s
     50 parents  ->  78.5 s
    100 parents  ->  504 gateway timeout after 98 s
    200 parents  -> 109.6 s
    616 parents  ->  504 gateway timeout after 60 s   (every attempt)

So the whole-universe request the hive tool issues can never complete: six
retries with exponential backoff burn ~10 minutes and then give up. The same
wall applies to the FREE ``metadata.get_cost`` preflight, which uses the
identical resolver -- the run stalls before it ever reaches a paid call.

Asking for ``ALL_SYMBOLS`` skips symbol resolution entirely. The provider
streams the minute as it is stored, and the universe filter happens locally::

    ALL_SYMBOLS 1-minute cbbo-1m   ->  8.2 s, 36.2 MB zst, 1,933,684 records
    ALL_SYMBOLS definition (1 day) -> 16.5 s, 89.5 MB zst, 2,253,273 records
    decode + join + filter + write ->  8.0 s
    ------------------------------------------------------------------
    616-name universe board, end to end -> ~33 s (vs. never)

``ALL_SYMBOLS`` responses carry NO symbology mappings (``metadata.mappings`` is
empty, so ``to_df(map_symbols=True)`` yields ``symbol=None`` on every row).
The instrument_id -> raw_symbol map therefore comes from that session's
``definition`` schema, which is strictly better than the OSI-root string
parsing the v1/v2 tools do: the definition record carries ``underlying``,
``strike_price``, ``expiration`` and ``instrument_class`` directly, so no
6-char root slice and no trailing-digit strip (the strip is what silently folds
an adjusted-deliverable ``AAPL1`` board onto ``AAPL``; see the comment in
``pull_opra_universe_snapshot.py``). Definitions are cached per session as a
compact 3-column parquet, so only the first run of a session pays for them.

Equivalence is verified, not asserted: rebuilding 2026-08-11 through this path
and diffing against the hive file the parent-symbol tool produced gives
629,465 == 629,465 rows, 0 keys only-in-old, 0 keys only-in-new, and 0 value
mismatches across bid_px/ask_px/bid_sz/ask_sz.

Cost
----
``metadata.get_cost`` returned $0.00 for every request shape tried here
(1 parent, 616 parents, ALL_SYMBOLS 1 minute, ALL_SYMBOLS whole day,
ALL_SYMBOLS definitions) -- this account is on a flat-rate OPRA license, so
egress volume is not billed per byte and the cap/degrade machinery in the v1/v2
tools has nothing to gate. ``--cap`` is still honoured: the preflight runs
(it is free and fast on ALL_SYMBOLS) and the run REFUSES if the estimate ever
comes back above the cap, so a license change cannot silently spend.

Output contract
---------------
Identical to ``pull_opra_hive.py`` -- ``<root>/date=YYYY-MM-DD/data.parquet``,
columns ``ts, underlying, symbol, instrument_id, bid_px, ask_px, bid_sz,
ask_sz`` (px 1e-9 fixed-point int64, unset side = INT64_MIN), sorted by
(underlying, symbol), one row group per underlying. Merge/resume semantics are
reused verbatim from that module (``plan_missing`` / ``merge_date_file``).

Usage:
  python atx-vol/tools/pull_opra_allsym.py \
      --universe atx-vol/data/universe/xsec_2026-08.csv \
      --start 2026-08-21 --end 2026-08-21 --snap-et 15:55 \
      --out C:/atx-data/opra-hive

  # entire OPRA universe, no filter (every underlier the provider quoted)
  python atx-vol/tools/pull_opra_allsym.py --universe ALL \
      --start 2026-08-21 --end 2026-08-21 --snap-et 15:55 --out C:/atx-data/opra-all
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
    COLUMNS,
    DATASET,
    DATE_FILE,
    SCHEMA,
    merge_date_file,
    plan_missing,
    read_api_key,
    read_universe,
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
DEF_SCHEMA = "definition"
# Only these two instrument classes are options boards; OPRA also emits an
# underlying stub record ('K') that carries no strike and must not reach the hive.
OPTION_CLASSES = (b"C", b"P")
DEF_MAP_SCHEMA = pa.schema([
    ("instrument_id", pa.int64()),
    ("symbol", pa.string()),
    ("underlying", pa.string()),
])


# ── Provider fetch (cached on disk; a present cache file is never re-fetched) ──
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


def definition_map(client, cache_root: pathlib.Path, date: str,
                   prune_raw: bool = True) -> pd.DataFrame:
    """instrument_id -> (raw_symbol, OSI underlying root) for one session.

    Cached as a 3-column parquet (~60 MB) so the ~900 MB definition ndarray is
    decoded at most once per session, not once per run. ``_fetch`` returns
    immediately when the raw file is already on disk, which is how the caller's
    prefetch pool hands this function an already-downloaded session."""
    small = def_small_path(cache_root, date)
    if small.exists():
        return pq.ParquetFile(small).read().to_pandas()
    raw = _fetch(client, def_raw_path(cache_root, date), schema=DEF_SCHEMA,
                 start=f"{date}T00:00:00", end=f"{date}T23:59:59")
    arr = db.DBNStore.from_file(raw).to_ndarray()
    arr = arr[np.isin(arr["instrument_class"], OPTION_CLASSES)]
    frame = pd.DataFrame({
        "instrument_id": arr["instrument_id"].astype("int64"),
        "symbol": arr["raw_symbol"].astype(str),
        "underlying": arr["underlying"].astype(str),
    }).drop_duplicates("instrument_id", keep="last").reset_index(drop=True)
    del arr
    tmp = small.with_suffix(".parquet.tmp")
    pq.write_table(pa.Table.from_pandas(frame, schema=DEF_MAP_SCHEMA, preserve_index=False),
                   tmp)
    os.replace(tmp, small)
    if prune_raw:
        # The 90 MB raw definition response is 3 columns of interest wrapped in
        # a 400-byte-per-record struct; once the map parquet exists it is dead
        # weight -- 90 MB x 252 sessions is ~23 GB/year of cache holding data we
        # already distilled. Re-deriving it costs one download, and OPRA egress
        # is flat-rate on this account, so the raw file is not worth the disk.
        # Ordered strictly AFTER the atomic rename: a crash before that point
        # leaves the raw file intact and the next run re-derives from it.
        try:
            raw.unlink(missing_ok=True)
        except OSError:
            pass  # Windows: an AV scanner or another run may hold the handle.
    return frame


def build_frame(quote_path: pathlib.Path, defs: pd.DataFrame, date: str, hm: str,
                root_to_sym: Optional[dict[str, str]]) -> tuple[pd.DataFrame, int]:
    """Decode one minute of ALL_SYMBOLS cbbo-1m into the canonical 8-column frame.

    ``root_to_sym`` maps the OSI underlying root to the universe's spelling of
    the ticker (``BRKB`` -> ``BRK.B``); ``None`` keeps every underlier the
    provider quoted, spelled as the definition record spells it. Matching is
    EXACT on the definition's ``underlying`` field -- an adjusted-deliverable
    root (``AAPL1``) is a different instrument and is dropped, not folded onto
    ``AAPL``, which is what the C++ loaders (``osi_root_matches_ticker``)
    already refuse. Returns (frame, rows_returned_by_provider)."""
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
    keep = defs if root_to_sym is None else defs[defs["underlying"].isin(root_to_sym)]
    frame = quotes.merge(keep, on="instrument_id", how="inner")
    del quotes
    frame.loc[frame["bid_px"] == DBN_UNDEF, "bid_px"] = INT64_MIN
    frame.loc[frame["ask_px"] == DBN_UNDEF, "ask_px"] = INT64_MIN
    if root_to_sym is not None:
        frame["underlying"] = frame["underlying"].map(root_to_sym)
    frame["ts"] = pd.Timestamp(f"{date}T{hm}:00")
    frame = frame[COLUMNS].sort_values(["underlying", "symbol"], kind="stable")
    return frame.reset_index(drop=True), n_returned


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--universe", required=True,
                    help="universe fixture path, or the literal ALL for no filter")
    ap.add_argument("--start", required=True)
    ap.add_argument("--end", required=True)
    grp = ap.add_mutually_exclusive_group()
    grp.add_argument("--snap-et", default="", help="ET market-clock HH:MM (DST-aware)")
    grp.add_argument("--snap-utc", default="", help="fixed UTC HH:MM")
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--cap", type=float, default=25.0,
                    help="refuse if the FREE preflight estimate exceeds this ($)")
    ap.add_argument("--prefetch", type=int, default=3,
                    help="dates to download ahead of the decoder (IO overlap)")
    ap.add_argument("--keep-def-raw", action="store_true",
                    help="keep the ~90 MB raw definition DBN per session; by default it "
                         "is deleted once the compact map parquet is written")
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

    if args.universe.upper() == "ALL":
        symbols, root_to_sym = [], None
    else:
        symbols = [s for s, _w in read_universe(pathlib.Path(args.universe))]
        root_to_sym = {s.replace(".", ""): s for s in symbols}

    # Resume plan. With no universe filter there is no per-symbol footer check
    # to make, so the unit is the whole date file.
    if root_to_sym is None:
        todo = [d for d in dates
                if args.force or not (args.out / f"date={d}" / DATE_FILE).exists()]
    else:
        plan = plan_missing(args.out, symbols, dates, force=args.force,
                            expected_minute=minute)
        todo = [d for d in dates if d in plan]
    print(f"sessions={len(dates)} to_pull={len(todo)} "
          f"universe={'ALL' if root_to_sym is None else len(symbols)} "
          f"minute={'ET ' + args.snap_et if args.snap_et else 'UTC ' + args.snap_utc}")
    if not todo:
        print("nothing to do (every requested date is already complete)")
        return 0

    client = db.Historical(key=read_api_key(args.env_file))

    # FREE preflight. Cheap on ALL_SYMBOLS (~0.5 s/date, no symbol resolution),
    # so it stays in the loop as a live guard against a license/pricing change.
    est = 0.0
    for d in todo:
        s, e = snap_window(d, minute)
        est += float(client.metadata.get_cost(
            dataset=DATASET, symbols="ALL_SYMBOLS", schema=SCHEMA, start=s, end=e))
        if not (args.out / "_defs" / f"{d}.parquet").exists():
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

    qdir = args.out / "_dbn_all"
    ddir = args.out / "_defs"
    qdir.mkdir(parents=True, exist_ok=True)
    ddir.mkdir(parents=True, exist_ok=True)

    def q_path(d: str) -> pathlib.Path:
        return qdir / f"{d}_{minute[d].replace(':', '')}.dbn.zst"

    # Downloads are IO-bound and the decode is not, so run a small prefetch pool
    # ahead of a SEQUENTIAL decoder: a decoded session peaks near 1 GB of
    # ndarray, and decoding several at once is how this OOMs.
    rows_total, failed = 0, []
    t_run = time.perf_counter()
    # Both fetches go through the pool: the whole-day `definition` download is
    # ~2x the quote download, so leaving it inline (as the first cut did) put
    # ~16 s of un-overlapped IO on every session's critical path.
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
                defs = definition_map(client, ddir, d, prune_raw=not args.keep_def_raw)
                frame, n_ret = build_frame(qp, defs, d, minute[d], root_to_sym)
            except Exception as exc:  # noqa: BLE001
                print(f"  [{i}/{len(todo)}] {d}: FAILED {str(exc)[:110]}", file=sys.stderr)
                failed.append(d)
                continue
            target = args.out / f"date={d}" / DATE_FILE
            n = merge_date_file(target if target.exists() else None, frame, target,
                                force=args.force)
            rows_total += len(frame)
            print(f"  [{i}/{len(todo)}] {d} @{minute[d]}Z  provider={n_ret:,d} "
                  f"kept={len(frame):,d} underliers={frame['underlying'].nunique()} "
                  f"file_rows={n:,d}  ({time.perf_counter() - t0:.1f}s)")

    print(f"DONE dates={len(todo) - len(failed)}/{len(todo)} rows={rows_total:,d} "
          f"elapsed={time.perf_counter() - t_run:.1f}s")
    if failed:
        print(f"FAILED dates ({len(failed)}): {', '.join(failed)}", file=sys.stderr)
        return 5
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
