#!/usr/bin/env python3
"""Cost-gated YTD OPRA cbbo-1m pull for the SPY dispersion universe (D2).

For every NYSE session in [--start, --end] and every symbol in the D1 universe
fixture, pull the OPRA full-chain consolidated BBO (dataset OPRA.PILLAR, schema
cbbo-1m) at the single pre-close snapshot minute (default 19:55:00Z) into the
per-(symbol, date) Parquet hive

    <out>/{symbol}/{date}.parquet

with the exact 8-column schema `atx::vol::load_opra_daterange` reads
(ts, underlying, symbol, instrument_id, bid_px, ask_px, bid_sz, ask_sz; px are
1e-9 fixed-point int64, an unset side stored as INT64_MIN). This EXTENDS the
existing hive at C:/atx-data/spy-dispersion/opra (the MAG7 pull), reusing every
board already on disk.

Snapshot minute / DST note
--------------------------
The window is a FIXED UTC minute (default 19:55:00Z) for the whole run, matching
the existing hive and the atx-core databento_bulk_opra C++ precedent (kSnapStart
"T19:55:00"). 19:55Z == 15:55 America/New_York during EDT (Mar 8 2026 onward) and
== 14:55 ET during EST (Jan-Mar). A fixed UTC minute keeps the entire hive uniform
across the DST boundary (a per-day ET->UTC conversion would put Jan boards at 20:55Z
and conflict with the already-pulled Jan MAG7 boards). Choose deliberately for hive
uniformity + reuse; the "~15:55 ET" target is met exactly for the EDT majority of YTD.

Cost discipline ($300 hard cap — sprint §3 guardrail)
-----------------------------------------------------
  1. FREE preflight first: metadata.get_cost (no billable egress) over the exact
     per-day snapshot windows, counting ONLY the (symbol, date) cells not already
     on disk — so a resume/second run estimates (and spends) $0 for cached boards.
     The dollar estimate is LOGGED before any paid request.
  2. HARD CAP: if the estimated remaining total exceeds --cap, DEGRADE (do not ask):
     keep window/quality, keep the index leg (SPY) + the top-N constituents by index
     weight that fit under the cap, log exactly which names are dropped and N. If not
     even the index leg + a couple of names fit, STOP and exit BLOCKED (exit 3) with
     the numbers — never pull blind.
  3. --dry-run does the preflight, prints the plan + estimate, and exits 0 WITHOUT
     pulling (the free go/no-go evidence).
  4. Resumable / idempotent: an already-present target parquet is skipped (no API
     call, no spend); the raw per-day DBN is cached under <out>/_dbn/ so a crash
     after download but before split never re-bills. Running the tool twice costs $0.

API key comes from $DATABENTO_API_KEY or a .env file (default search: ./.env then
C:/atx/.env, or --env-file). The key is never printed or logged.

Usage:
  python pull_opra_universe_batch.py \
      --universe atx-vol/data/universe/spy_top50_2026-01-01.csv \
      --start 2026-01-02 --end 2026-07-17 \
      --out C:/atx-data/spy-dispersion/opra [--snap-utc 19:55] [--cap 300] [--dry-run]
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import os
import pathlib
import sys
import time

import databento as db
import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

INT64_MIN = np.iinfo(np.int64).min
DBN_UNDEF = np.iinfo(np.int64).max  # databento UNDEF_PRICE
DATASET = "OPRA.PILLAR"
SCHEMA = "cbbo-1m"

# Exact column order + types load_opra_daterange consumes (see make_universe.py).
ARROW_SCHEMA = pa.schema([
    ("ts", pa.timestamp("ns")),
    ("underlying", pa.string()),
    ("symbol", pa.string()),
    ("instrument_id", pa.int64()),
    ("bid_px", pa.int64()),
    ("ask_px", pa.int64()),
    ("bid_sz", pa.int64()),
    ("ask_sz", pa.int64()),
])


# ── API key (from env or .env; NEVER printed) ──────────────────────────────────
def read_api_key(env_file: str = "") -> str:
    key = os.environ.get("DATABENTO_API_KEY", "").strip()
    if key:
        return key
    candidates = [pathlib.Path(env_file)] if env_file else []
    candidates += [pathlib.Path(".env"), pathlib.Path("C:/atx/.env")]
    for path in candidates:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        for line in text.splitlines():
            line = line.strip()
            if line.startswith("DATABENTO_API_KEY"):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise SystemExit(
        "BLOCKED: DATABENTO_API_KEY not set (checked $DATABENTO_API_KEY, ./.env, "
        "C:/atx/.env). Provide a key via env or --env-file; nothing pulled."
    )


# ── Universe fixture reader (D1 CSV/TSV or a plain symbol list) ─────────────────
def read_universe(path: pathlib.Path) -> list[tuple[str, float]]:
    """Return [(symbol, weight)] in priority order (index leg first, then weight
    descending — exactly the row order the D1 fixture emits). Accepts the D1
    tab/comma fixture with a `symbol` column, or a bare one-symbol-per-line list."""
    raw = path.read_text(encoding="utf-8-sig")
    lines = [ln for ln in raw.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
    if not lines:
        raise SystemExit(f"universe file {path} is empty")
    delim = "\t" if "\t" in lines[0] else ("," if "," in lines[0] else None)
    if delim and "symbol" in lines[0].lower():
        reader = csv.DictReader(io.StringIO("\n".join(lines)), delimiter=delim)
        out: list[tuple[str, float]] = []
        for row in reader:
            sym = (row.get("symbol") or "").strip()
            if not sym:
                continue
            try:
                w = float((row.get("raw_weight") or "0").strip())
            except ValueError:
                w = 0.0
            out.append((sym, w))
        if not out:
            raise SystemExit(f"universe file {path} has a header but no symbol rows")
        return out
    # Bare list: first whitespace token per line is the symbol; weight unknown.
    return [(ln.split()[0].strip(), 0.0) for ln in lines]


# ── Trading calendar (XNYS sessions; fallback to weekdays) ─────────────────────
def trading_sessions(start: str, end: str) -> list[str]:
    try:
        import exchange_calendars as xcals

        cal = xcals.get_calendar("XNYS")
        return [d.strftime("%Y-%m-%d") for d in cal.sessions_in_range(start, end)]
    except Exception as exc:  # noqa: BLE001
        print(f"WARNING: exchange_calendars unavailable ({str(exc)[:80]}); "
              f"falling back to Mon-Fri weekdays (may include market holidays).",
              file=sys.stderr)
        days = pd.bdate_range(start=start, end=end)
        return [d.strftime("%Y-%m-%d") for d in days]


def to_parent(sym: str) -> str:
    return sym.replace(".", "") + ".OPT"


def snap_window(date: str, snap_hm: str) -> tuple[str, str]:
    hh, mm = snap_hm.split(":")
    start = f"{date}T{hh}:{mm}:00"
    end_minute = int(hh) * 60 + int(mm) + 1
    end = f"{date}T{end_minute // 60:02d}:{end_minute % 60:02d}:00"
    return start, end


def get_cost_retry(client: db.Historical, parents: list[str], date: str,
                   snap_hm: str, attempts: int = 4) -> float:
    start, end = snap_window(date, snap_hm)
    for attempt in range(attempts):
        try:
            return float(client.metadata.get_cost(
                dataset=DATASET, symbols=parents, schema=SCHEMA,
                start=start, end=end, stype_in="parent"))
        except Exception as exc:  # noqa: BLE001
            if attempt == attempts - 1:
                raise
            print(f"    get_cost retry {attempt + 1} ({date}): {str(exc)[:90]}",
                  file=sys.stderr)
            time.sleep(3 * (attempt + 1))
    return 0.0


def write_hive_file(grp: pd.DataFrame, tgt: pathlib.Path) -> None:
    tgt.parent.mkdir(parents=True, exist_ok=True)
    tbl = pa.Table.from_pandas(
        grp[["ts", "underlying", "symbol", "instrument_id",
             "bid_px", "ask_px", "bid_sz", "ask_sz"]],
        schema=ARROW_SCHEMA, preserve_index=False)
    # Atomic write: tmp then rename, so a crash never leaves a half-written board.
    tmp = tgt.with_suffix(".parquet.tmp")
    pq.write_table(tbl, tmp)
    os.replace(tmp, tgt)


def decode_and_split(store: db.DBNStore, date: str, snap_hm: str,
                     root_to_sym: dict[str, str], out: pathlib.Path,
                     want_syms: set[str], force: bool,
                     manifest: list[dict], seen: set[str]) -> tuple[int, int]:
    """Decode one day's DBN and split to per-symbol parquet. Returns (files_written,
    unmapped_rows). Only symbols in want_syms (missing this day) are written."""
    df = store.to_df(price_type="fixed", pretty_ts=False, map_symbols=True)
    if df.empty:
        return 0, 0
    df = df.reset_index()
    snap_ts = pd.Timestamp(f"{date}T{snap_hm}:00")  # constant snapshot stamp (naive UTC)
    frame = pd.DataFrame({
        "ts": pd.Series([snap_ts] * len(df), dtype="datetime64[ns]"),
        "symbol": df["symbol"].astype(str),
        "instrument_id": df["instrument_id"].astype("int64"),
        "bid_px": df["bid_px_00"].astype("int64"),
        "ask_px": df["ask_px_00"].astype("int64"),
        "bid_sz": df["bid_sz_00"].astype("int64"),
        "ask_sz": df["ask_sz_00"].astype("int64"),
    })
    frame.loc[frame["bid_px"] == DBN_UNDEF, "bid_px"] = INT64_MIN
    frame.loc[frame["ask_px"] == DBN_UNDEF, "ask_px"] = INT64_MIN
    # OSI root = first 6 chars (space padded); strip trailing digits; map to universe symbol.
    roots = frame["symbol"].str[:6].str.strip()
    base = roots.str.replace(r"\d+$", "", regex=True)
    frame["underlying"] = base.map(root_to_sym)
    unmapped = frame["underlying"].isna()
    n_unmapped = int(unmapped.sum())
    frame = frame[~unmapped]
    n_files = 0
    for sym, grp in frame.groupby("underlying", sort=False):
        sym = str(sym)
        if sym not in want_syms:
            continue
        seen.add(sym)
        tgt = out / sym / f"{date}.parquet"
        if tgt.exists() and not force:
            manifest.append({"symbol": sym, "date": date, "records": len(grp), "status": "exists"})
            continue
        write_hive_file(grp, tgt)
        n_files += 1
        manifest.append({"symbol": sym, "date": date, "records": len(grp), "status": "ok"})
    return n_files, n_unmapped


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--universe", type=pathlib.Path,
                     help="D1 fixture (data/universe/spy_top50_2026-01-01.csv)")
    src.add_argument("--symbols-file", type=pathlib.Path,
                     help="plain one-symbol-per-line list (weights unknown -> no degrade ranking)")
    ap.add_argument("--start", default="2026-01-02")
    ap.add_argument("--end", default="2026-07-17")
    ap.add_argument("--snap-utc", default="19:55", help="HH:MM UTC snapshot minute (default 19:55)")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("C:/atx-data/spy-dispersion/opra"))
    ap.add_argument("--cap", type=float, default=300.0, help="hard $ cap (sprint §3)")
    ap.add_argument("--sample-days", type=int, default=3,
                    help="sessions to sample for the batched cost estimate (each "
                    "get_cost prices one whole snapshot minute; unit x cells = total)")
    ap.add_argument("--index-symbol", default="SPY", help="always-kept index leg on degrade")
    ap.add_argument("--min-degrade-names", type=int, default=3,
                    help="BLOCK if fewer than index-leg + this many names fit under cap")
    ap.add_argument("--env-file", default="", help="path to a .env holding DATABENTO_API_KEY")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true", help="rewrite existing parquet boards")
    args = ap.parse_args()

    uni = read_universe(args.universe if args.universe else args.symbols_file)
    symbols = [s for s, _ in uni]
    weight = {s: w for s, w in uni}
    root_to_sym = {s.replace(".", ""): s for s in symbols}
    dates = trading_sessions(args.start, args.end)
    if not dates:
        raise SystemExit(f"no trading sessions in [{args.start}, {args.end}]")

    print(f"universe={len(symbols)} sessions={len(dates)} [{dates[0]}..{dates[-1]}] "
          f"dataset={DATASET} schema={SCHEMA} snap={args.snap_utc}Z out={args.out}")

    # ── Plan: which (symbol, date) cells are missing on disk (never re-pull) ────
    missing_by_date: dict[str, list[str]] = {}
    on_disk = 0
    for date in dates:
        miss = [s for s in symbols if not (args.out / s / f"{date}.parquet").exists()]
        on_disk += len(symbols) - len(miss)
        if miss:
            missing_by_date[date] = miss
    n_missing = sum(len(v) for v in missing_by_date.values())
    print(f"cells: total={len(symbols) * len(dates)} on_disk={on_disk} "
          f"to_pull={n_missing} (over {len(missing_by_date)} sessions)")
    if n_missing == 0:
        print("ALL boards already on disk — nothing to pull, $0.00.")
        return 0

    key = read_api_key(args.env_file)
    client = db.Historical(key=key)

    # ── FREE preflight (fast, batched) ─────────────────────────────────────────
    # A single get_cost call prices one WHOLE snapshot minute for the full parent
    # set (each call is a slow ~10-30 s metadata round-trip, so we sample a few
    # sessions rather than one call per day). The per-(symbol,date) unit cost x the
    # number of cells still to pull is the remaining-spend estimate; on-disk cells
    # are excluded, so a resume estimates (and spends) $0.
    all_missing = sorted(missing_by_date)
    step = max(1, len(all_missing) // max(1, args.sample_days))
    sample_dates = all_missing[::step][: args.sample_days] or all_missing[:1]
    print(f"\nFREE preflight (metadata.get_cost — no egress; sampling "
          f"{len(sample_dates)} of {len(all_missing)} sessions {sample_dates}):")
    unit_costs = []
    for date in sample_dates:
        parents = [to_parent(s) for s in missing_by_date[date]]
        cost = get_cost_retry(client, parents, date, args.snap_utc)
        unit = cost / max(len(parents), 1)
        unit_costs.append(unit)
        print(f"  {date}: {len(parents)} syms est=${cost:.6f}  (unit ${unit:.8f}/sym-day)")
    unit_cost = sum(unit_costs) / len(unit_costs)
    total_cost = unit_cost * n_missing
    print(f"\nESTIMATE (remaining spend): ${total_cost:.4f} = ${unit_cost:.8f}/sym-day "
          f"x {n_missing} cells (cap ${args.cap:.2f})")

    keep = symbols  # default: pull the whole universe
    dropped: list[str] = []
    # ── $300 degrade: keep index leg + top-N-by-weight that fit ────────────────
    if total_cost > args.cap:
        print(f"\nOVER CAP: estimate ${total_cost:.4f} > ${args.cap:.2f} — DEGRADING "
              f"to index leg + top-N by weight (sprint §3).")
        # Per-symbol total estimate: get_cost per symbol on a representative session
        # (median missing date), scaled by that symbol's missing-day count.
        rep_date = sorted(missing_by_date)[len(missing_by_date) // 2]
        miss_days_of = {s: sum(1 for d in missing_by_date if s in missing_by_date[d])
                        for s in symbols}
        per_sym_total: dict[str, float] = {}
        for s in symbols:
            c1 = get_cost_retry(client, [to_parent(s)], rep_date, args.snap_utc)
            per_sym_total[s] = c1 * max(miss_days_of[s], 1)
        # Priority order: index leg first, then descending index weight.
        idx = args.index_symbol.strip().upper()
        ranked = ([idx] if idx in weight else []) + \
                 sorted((s for s in symbols if s != idx), key=lambda s: -weight.get(s, 0.0))
        keep, running = [], 0.0
        for s in ranked:
            if running + per_sym_total.get(s, 0.0) <= args.cap:
                keep.append(s)
                running += per_sym_total.get(s, 0.0)
            else:
                dropped.append(s)
        floor_ok = (idx in keep) and (len([s for s in keep if s != idx]) >= args.min_degrade_names)
        print(f"  degrade kept N={len(keep)} (est ${running:.4f}); dropped {len(dropped)}: "
              f"{','.join(dropped) if dropped else '(none)'}")
        if not floor_ok:
            print(f"\nBLOCKED: even {idx} + {args.min_degrade_names} names exceed cap "
                  f"${args.cap:.2f} (kept only {keep}). No data pulled.", file=sys.stderr)
            return 3
        # `running` is the summed per-symbol estimate of the kept set = the honest
        # authorized total; also refresh the unit cost to the kept set for spend tracking.
        total_cost = running
        unit_cost = running / max(sum(miss_days_of[s] for s in keep), 1)
        print(f"  degraded estimate (kept set): ${total_cost:.4f} (cap ${args.cap:.2f})")

    print(f"\nAuthorized estimate ${total_cost:.4f} within cap ${args.cap:.2f}. "
          f"Symbols kept: {len(keep)}.")

    if args.dry_run:
        print("DRY RUN — no data pulled.")
        return 0

    # ── Paid pull: per session, one get_range over that day's missing kept parents ─
    keepset = set(keep)
    dbn_dir = args.out / "_dbn"
    dbn_dir.mkdir(parents=True, exist_ok=True)
    manifest: list[dict] = []
    seen: set[str] = set()
    n_files = n_unmapped = 0
    actual_spend = 0.0
    failed_days: list[str] = []

    for i, date in enumerate(sorted(missing_by_date)):
        miss = [s for s in missing_by_date[date] if s in keepset]
        if not miss:
            continue
        parents = [to_parent(s) for s in miss]
        digest = hashlib.sha256((date + "|" + ",".join(sorted(miss))).encode()).hexdigest()[:12]
        dbn_path = dbn_dir / f"{date}_{args.snap_utc.replace(':', '')}_{digest}.dbn.zst"
        if dbn_path.exists():
            store = db.DBNStore.from_file(dbn_path)
            tag = "cached"
        else:
            store = None
            for attempt in range(6):
                try:
                    store = client.timeseries.get_range(
                        dataset=DATASET, symbols=parents, schema=SCHEMA,
                        start=snap_window(date, args.snap_utc)[0],
                        end=snap_window(date, args.snap_utc)[1], stype_in="parent")
                    break
                except Exception as exc:  # noqa: BLE001
                    print(f"  {date} pull retry {attempt + 1}: {str(exc)[:90]}", file=sys.stderr)
                    time.sleep(min(10 * 2 ** attempt, 120))
            if store is None:
                print(f"  {date}: FAILED after retries — left for a later resume", file=sys.stderr)
                failed_days.append(date)
                continue
            store.to_file(dbn_path)
            tag = "pulled"
        assert store is not None  # None path already continued above
        nf, nu = decode_and_split(store, date, args.snap_utc, root_to_sym,
                                  args.out, keepset, args.force, manifest, seen)
        n_files += nf
        n_unmapped += nu
        # Realized spend estimate = sampled unit cost x boards actually written
        # this session (cached/skipped boards were already paid or never billable).
        if tag == "pulled":
            actual_spend += unit_cost * nf
        if i % 10 == 0 or tag == "cached":
            print(f"  [{i + 1}/{len(missing_by_date)}] {date} {tag}: {nf} boards "
                  f"(running_spend=${actual_spend:.4f})", flush=True)

    # Symbols in the kept set that never produced data (no listed options that day).
    for date, miss in missing_by_date.items():
        for s in miss:
            if s in keepset and s not in seen:
                manifest.append({"symbol": s, "date": date, "records": 0, "status": "no_options"})

    mpath = args.out / f"manifest_ytd_{args.start}_{args.end}_{args.snap_utc.replace(':', '')}.csv"
    pd.DataFrame(manifest).to_csv(mpath, index=False)
    print(f"\nDONE boards_written={n_files} symbols_with_data={len(seen)} "
          f"unmapped_rows={n_unmapped} failed_sessions={len(failed_days)}")
    print(f"ACTUAL SPEND (realized preflight of pulled cells): ${actual_spend:.4f}")
    print(f"kept N={len(keep)} dropped={len(dropped)} manifest={mpath}")
    return 0 if not failed_days else 5


if __name__ == "__main__":
    raise SystemExit(main())
