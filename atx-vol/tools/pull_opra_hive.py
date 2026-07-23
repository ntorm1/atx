#!/usr/bin/env python3
"""Cost-gated OPRA cbbo-1m pull into the v2 date-partitioned hive.

Port of ``tools/pull_opra_universe_batch.py`` targeting the redesigned layout
(design §3): each session is ONE parquet file holding every symbol,

    <root>/date=YYYY-MM-DD/part-0.parquet

with the exact 8-column schema the C++ loaders read (``ts, underlying, symbol,
instrument_id, bid_px, ask_px, bid_sz, ask_sz``; px are 1e-9 fixed-point int64,
an unset side stored as INT64_MIN). Rows are sorted by ``underlying`` then
``symbol`` and written **one row group per underlying**, so a per-symbol read is
row-group pruned via column statistics and ``plan_missing`` can read the
on-disk symbol set from the parquet footer without a data scan.

What changed from v1 (per design §3 / §7)
-----------------------------------------
  * Plan unit is (date -> missing-symbol set): if a date file is absent every
    requested symbol is missing; if present, only the underlyings NOT already in
    its footer statistics are missing; a complete file drops the date entirely.
  * ONE ``get_range`` per date over the union of missing parents (fewer API
    round-trips), decoded to a single frame, then merged into the date file as
    the UNION (atomic tmp + rename) via ``merge_date_file``.
  * ``--force`` re-pulls and rewrites the requested symbols (other underlyings
    already in the file are preserved).
Everything else is carried over verbatim: key handling, XNYS calendar, the fixed
19:55:00Z snapshot minute, the FREE ``get_cost`` preflight + retry/sampling, the
$-cap degrade (index leg + top-N by weight, BLOCK below floor), the DBN cache +
quarantine, and spend accounting.

Snapshot minute / DST note
--------------------------
The window is a FIXED UTC minute (default 19:55:00Z) for the whole run, matching
the existing hive and the atx-core databento_bulk_opra C++ precedent. 19:55Z ==
15:55 America/New_York during EDT and == 14:55 ET during EST; a fixed UTC minute
keeps the hive uniform across the DST boundary (see the v1 tool for the full
rationale).

Cost discipline (hard cap — design §7 guardrail)
------------------------------------------------
  1. FREE preflight first: ``metadata.get_cost`` (no billable egress) over the
     per-day snapshot windows, counting ONLY the (date, symbol) cells not already
     on disk — a resume estimates (and spends) $0 for cached boards. Logged before
     any paid request.
  2. HARD CAP: if the estimate exceeds --cap, DEGRADE (do not ask): keep the index
     leg + the top-N constituents by index weight that fit; if not even index +
     --min-degrade-names fit, STOP and exit BLOCKED (exit 3) — never pull blind.
  3. --dry-run does the preflight, prints the plan + estimate, and exits 0 without
     pulling.
  4. Resumable / idempotent: a present date file's symbols are skipped (no API
     call, no spend); the raw per-day DBN is cached under <root>/_dbn/ so a crash
     after download but before split never re-bills. Running the tool twice costs $0.

API key comes from $DATABENTO_API_KEY or a .env file (default search: ./.env then
C:/atx/.env, or --env-file). The key is never printed or logged.

Usage:
  python pull_opra_hive.py \
      --universe atx-vol/data/universe/spy_top50_2026-01-01.csv \
      --start 2026-07-20 --end 2026-07-21 \
      --out C:/atx-data/opra-hive [--snap-utc 19:55] [--cap 100] [--dry-run]
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
from dataclasses import dataclass, field
from typing import Callable, Optional

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

try:  # databento is optional: the module must import (and be tested) without it.
    import databento as db
except ImportError:  # pragma: no cover - exercised only where the pkg is absent
    db = None

INT64_MIN = np.iinfo(np.int64).min
DBN_UNDEF = np.iinfo(np.int64).max  # databento UNDEF_PRICE
DATASET = "OPRA.PILLAR"
SCHEMA = "cbbo-1m"

# One parquet file per session; true hive `date=` key (design §3). Matches the
# migrate tool and the `load_opra_hive` C++ reader — keep in lockstep with them.
DATE_FILE = "part-0.parquet"

# Exact column order + types the C++ loaders consume (design §3).
COLUMNS = ["ts", "underlying", "symbol", "instrument_id",
           "bid_px", "ask_px", "bid_sz", "ask_sz"]
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


def get_cost_retry(client, parents: list[str], date: str,
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


# ── Plan: which (date, symbol) cells are missing on disk (never re-pull) ────────
def _empty_decoded() -> pd.DataFrame:
    return pd.DataFrame({
        "ts": pd.Series([], dtype="datetime64[ns]"),
        "underlying": pd.Series([], dtype="object"),
        "symbol": pd.Series([], dtype="object"),
        "instrument_id": pd.Series([], dtype="int64"),
        "bid_px": pd.Series([], dtype="int64"),
        "ask_px": pd.Series([], dtype="int64"),
        "bid_sz": pd.Series([], dtype="int64"),
        "ask_sz": pd.Series([], dtype="int64"),
    })


def date_file_underlyings(path: pathlib.Path) -> set[str]:
    """Distinct ``underlying`` values in a date file, from footer statistics.

    The writer emits one row group per underlying, so each group's min == max ==
    that underlying — no data scan. If a group lacks stats (older writer), fall
    back to reading only the single ``underlying`` column."""
    pf = pq.ParquetFile(path)
    md = pf.metadata
    idx = pf.schema_arrow.get_field_index("underlying")
    if idx < 0:
        raise ValueError(f"{path}: no 'underlying' column")
    syms: set[str] = set()
    for rg in range(md.num_row_groups):
        st = md.row_group(rg).column(idx).statistics
        if st is None or not st.has_min_max:
            return set(pf.read(columns=["underlying"]).column(0).to_pylist())
        syms.add(st.min)
        syms.add(st.max)
    return syms


def plan_missing(out_root, symbols: list[str], dates: list[str],
                 force: bool = False) -> dict[str, list[str]]:
    """Map each date to the requested symbols not yet on disk (universe order).

    Empty/absent date file -> all requested symbols. Partial file -> only the
    underlyings missing from its footer. Complete file -> date omitted. With
    ``force`` every requested symbol is (re)planned for every date."""
    out_root = pathlib.Path(out_root)
    plan: dict[str, list[str]] = {}
    for date in dates:
        target = out_root / f"date={date}" / DATE_FILE
        if force or not target.exists():
            plan[date] = list(symbols)
            continue
        have = date_file_underlyings(target)
        miss = [s for s in symbols if s not in have]
        if miss:
            plan[date] = miss
    return plan


# ── Decode one day's DBN into a single 8-column frame ──────────────────────────
def decode_date_frame(store, date: str, snap_hm: str,
                      root_to_sym: dict[str, str],
                      want_syms: Optional[set[str]]) -> tuple[pd.DataFrame, int]:
    """Decode a day's DBN to the canonical 8-column frame, restricted to
    ``want_syms``. Returns (frame, n_unmapped_rows)."""
    df = store.to_df(price_type="fixed", pretty_ts=False, map_symbols=True)
    if df is None or df.empty:
        return _empty_decoded(), 0
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
    if want_syms is not None:
        frame = frame[frame["underlying"].isin(want_syms)]
    return frame[COLUMNS].reset_index(drop=True), n_unmapped


# ── Atomic write + merge of a date file ────────────────────────────────────────
def _write_date_file(df: pd.DataFrame, target: pathlib.Path) -> None:
    """Write ``df`` to ``target`` atomically, one row group per underlying."""
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_suffix(".parquet.tmp")
    writer = pq.ParquetWriter(tmp, ARROW_SCHEMA)
    try:
        if len(df) == 0:
            writer.write_table(pa.Table.from_pandas(
                df[COLUMNS], schema=ARROW_SCHEMA, preserve_index=False))
        else:
            # sort=True groups (and orders) by underlying; within a group the
            # incoming symbol order is preserved (caller sorts by symbol first).
            for _sym, grp in df.groupby("underlying", sort=True):
                writer.write_table(pa.Table.from_pandas(
                    grp[COLUMNS], schema=ARROW_SCHEMA, preserve_index=False))
    finally:
        writer.close()
    os.replace(tmp, target)


def merge_date_file(existing_path, new_frame: pd.DataFrame, target_path,
                    force: bool = False) -> int:
    """Merge ``new_frame`` into the date file at ``target_path`` and return the
    total row count written.

    * No existing file -> write ``new_frame``.
    * ``force`` -> requested underlyings (those in ``new_frame``) replace the
      on-disk copy; other existing underlyings are preserved.
    * otherwise -> UNION: existing rows kept, only new underlyings appended
      (existing wins on any accidental overlap — never re-pull-overwrite).
    Rows are sorted by (underlying, symbol) and the file is swapped atomically."""
    new_frame = (new_frame[COLUMNS].copy() if len(new_frame) else _empty_decoded())
    existing_path = pathlib.Path(existing_path) if existing_path is not None else None
    if existing_path is not None and existing_path.exists():
        existing = pq.read_table(existing_path).to_pandas()[COLUMNS]
        new_unds = set(new_frame["underlying"].unique().tolist())
        if force:
            kept = existing[~existing["underlying"].isin(new_unds)]
            combined = pd.concat([kept, new_frame], ignore_index=True)
        else:
            have = set(existing["underlying"].unique().tolist())
            add = new_frame[~new_frame["underlying"].isin(have)]
            combined = pd.concat([existing, add], ignore_index=True)
    else:
        combined = new_frame
    combined = combined.sort_values(["underlying", "symbol"], kind="stable").reset_index(drop=True)
    _write_date_file(combined, pathlib.Path(target_path))
    return len(combined)


# ── DBN store loader (real databento; injectable for tests) ────────────────────
def _default_store_loader(path: pathlib.Path):
    if db is None:  # pragma: no cover - only where databento is absent
        raise RuntimeError("databento is not installed; cannot read cached DBN")
    return db.DBNStore.from_file(path)


# ── FREE preflight + hard-cap degrade ──────────────────────────────────────────
@dataclass
class Preflight:
    unit_cost: float
    total_cost: float
    n_missing: int
    keep: list[str]
    dropped: list[str]
    blocked: bool
    sample_dates: list[str] = field(default_factory=list)


def preflight(client, plan: dict[str, list[str]], symbols: list[str],
              weight: dict[str, float], *, snap_hm: str, cap: float,
              sample_days: int, index_symbol: str,
              min_degrade_names: int) -> Preflight:
    """FREE cost estimate for the missing cells, degrading the kept set under the
    hard cap. Mirrors the v1 arithmetic exactly."""
    n_missing = sum(len(v) for v in plan.values())
    all_missing = sorted(plan)
    step = max(1, len(all_missing) // max(1, sample_days))
    sample_dates = all_missing[::step][:sample_days] or all_missing[:1]
    unit_costs: list[float] = []
    for date in sample_dates:
        parents = [to_parent(s) for s in plan[date]]
        cost = get_cost_retry(client, parents, date, snap_hm)
        unit_costs.append(cost / max(len(parents), 1))
        print(f"  {date}: {len(parents)} syms est=${cost:.6f}  "
              f"(unit ${unit_costs[-1]:.8f}/sym-day)")
    unit_cost = sum(unit_costs) / len(unit_costs)
    total_cost = unit_cost * n_missing

    keep = list(symbols)
    dropped: list[str] = []
    blocked = False
    if total_cost > cap:
        print(f"\nOVER CAP: estimate ${total_cost:.4f} > ${cap:.2f} — DEGRADING "
              f"to index leg + top-N by weight (design §7).")
        # Per-symbol total estimate: get_cost per symbol on a representative
        # session (median missing date), scaled by that symbol's missing-day count.
        rep_date = sorted(plan)[len(plan) // 2]
        miss_days_of = {s: sum(1 for d in plan if s in plan[d]) for s in symbols}
        per_sym_total: dict[str, float] = {}
        for s in symbols:
            c1 = get_cost_retry(client, [to_parent(s)], rep_date, snap_hm)
            per_sym_total[s] = c1 * max(miss_days_of[s], 1)
        idx = index_symbol.strip().upper()
        ranked = ([idx] if idx in weight else []) + \
                 sorted((s for s in symbols if s != idx), key=lambda s: -weight.get(s, 0.0))
        keep, running = [], 0.0
        for s in ranked:
            if running + per_sym_total.get(s, 0.0) <= cap:
                keep.append(s)
                running += per_sym_total.get(s, 0.0)
            else:
                dropped.append(s)
        floor_ok = (idx in keep) and (len([s for s in keep if s != idx]) >= min_degrade_names)
        print(f"  degrade kept N={len(keep)} (est ${running:.4f}); dropped {len(dropped)}: "
              f"{','.join(dropped) if dropped else '(none)'}")
        blocked = not floor_ok
        # `running` is the honest authorized total for the kept set.
        total_cost = running
        unit_cost = running / max(sum(miss_days_of[s] for s in keep), 1)
    return Preflight(unit_cost, total_cost, n_missing, keep, dropped, blocked, sample_dates)


# ── Paid pull: one get_range per date, decode -> merge_date_file ───────────────
@dataclass
class PullResult:
    boards_written: int
    dates_written: int
    unmapped_rows: int
    actual_spend: float
    manifest: list[dict]
    failed_dates: list[str]


def pull(client, plan: dict[str, list[str]], out_root, *, snap_hm: str,
         root_to_sym: dict[str, str], unit_cost: float = 0.0, force: bool = False,
         store_loader: Optional[Callable] = None) -> PullResult:
    """Pull every planned date: one ``get_range`` over the union of its missing
    parents, decode to one frame, merge into the date file. Resumable: a cached
    DBN is decoded without an API call; a present date file's symbols are already
    excluded upstream by ``plan_missing``."""
    store_loader = store_loader or _default_store_loader
    out_root = pathlib.Path(out_root)
    dbn_dir = out_root / "_dbn"
    dbn_dir.mkdir(parents=True, exist_ok=True)
    manifest: list[dict] = []
    boards = dates_written = unmapped = 0
    actual_spend = 0.0
    failed: list[str] = []
    ordered = sorted(plan)

    for i, date in enumerate(ordered):
        miss = list(plan[date])
        if not miss:
            continue
        want = set(miss)
        parents = [to_parent(s) for s in miss]
        digest = hashlib.sha256((date + "|" + ",".join(sorted(miss))).encode()).hexdigest()[:12]
        dbn_path = dbn_dir / f"{date}_{snap_hm.replace(':', '')}_{digest}.dbn.zst"
        store = None
        tag = ""
        if dbn_path.exists():
            # A crash DURING a prior DBN write can leave a torn/corrupt .dbn.zst
            # that still exists() and would raise on every resume. Guard the
            # decode: quarantine the bad file (.corrupt) and re-pull rather than
            # wedging the whole run.
            try:
                store = store_loader(dbn_path)
                tag = "cached"
            except Exception as exc:  # noqa: BLE001
                corrupt = dbn_path.parent / (dbn_path.name + ".corrupt")
                try:
                    os.replace(dbn_path, corrupt)
                except OSError:
                    pass
                print(f"  {date}: cached DBN unreadable ({str(exc)[:70]}); quarantined "
                      f"-> {corrupt.name}, re-pulling", file=sys.stderr)
                store = None
        if store is None:
            for attempt in range(6):
                try:
                    start, end = snap_window(date, snap_hm)
                    store = client.timeseries.get_range(
                        dataset=DATASET, symbols=parents, schema=SCHEMA,
                        start=start, end=end, stype_in="parent")
                    break
                except Exception as exc:  # noqa: BLE001
                    print(f"  {date} pull retry {attempt + 1}: {str(exc)[:90]}", file=sys.stderr)
                    time.sleep(min(10 * 2 ** attempt, 120))
            if store is None:
                print(f"  {date}: FAILED after retries — left for a later resume", file=sys.stderr)
                failed.append(date)
                continue
            # Atomic cache write (same-fs tmp -> os.replace), so a crash during
            # the write never leaves a torn .dbn.zst that wedges the next resume.
            dbn_tmp = dbn_path.parent / (dbn_path.name + ".tmp")
            store.to_file(dbn_tmp)
            os.replace(dbn_tmp, dbn_path)
            tag = "pulled"

        frame, nu = decode_date_frame(store, date, snap_hm, root_to_sym, want)
        unmapped += nu
        target = out_root / f"date={date}" / DATE_FILE
        existing = target if target.exists() else None
        merge_date_file(existing, frame, target, force=force)

        written = set(frame["underlying"].unique().tolist())
        for s in miss:
            if s in written:
                recs = int((frame["underlying"] == s).sum())
                manifest.append({"date": date, "symbol": s, "records": recs, "status": "ok"})
            else:
                manifest.append({"date": date, "symbol": s, "records": 0, "status": "no_options"})
        boards += len(written)
        dates_written += 1
        # Realized spend estimate = sampled unit cost x boards actually pulled
        # this session (cached/skipped boards were already paid or never billable).
        if tag == "pulled":
            actual_spend += unit_cost * len(written)
        if i % 10 == 0 or tag == "cached":
            print(f"  [{i + 1}/{len(ordered)}] {date} {tag}: {len(written)} boards "
                  f"(running_spend=${actual_spend:.4f})", flush=True)

    return PullResult(boards, dates_written, unmapped, actual_spend, manifest, failed)


# ── CLI ────────────────────────────────────────────────────────────────────────
def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--universe", type=pathlib.Path,
                     help="D1 fixture (data/universe/spy_top50_2026-01-01.csv)")
    src.add_argument("--symbols-file", type=pathlib.Path,
                     help="plain one-symbol-per-line list (weights unknown -> no degrade ranking)")
    ap.add_argument("--start", default="2026-07-01")
    ap.add_argument("--end", default="2026-07-31")
    ap.add_argument("--snap-utc", default="19:55", help="HH:MM UTC snapshot minute (default 19:55)")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("C:/atx-data/opra-hive"))
    ap.add_argument("--cap", type=float, default=100.0, help="hard $ cap (design §7)")
    ap.add_argument("--sample-days", type=int, default=3,
                    help="sessions to sample for the batched cost estimate (each "
                    "get_cost prices one whole snapshot minute; unit x cells = total)")
    ap.add_argument("--index-symbol", default="SPY", help="always-kept index leg on degrade")
    ap.add_argument("--min-degrade-names", type=int, default=3,
                    help="BLOCK if fewer than index-leg + this many names fit under cap")
    ap.add_argument("--env-file", default="", help="path to a .env holding DATABENTO_API_KEY")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true", help="rewrite requested symbols on disk")
    return ap


def run(args, client=None, store_loader: Optional[Callable] = None) -> int:
    """Plan -> free preflight -> (dry-run/exit) -> paid pull. ``client`` is
    injectable so tests can drive the whole flow with a fake (no network/key)."""
    uni = read_universe(args.universe if args.universe else args.symbols_file)
    symbols = [s for s, _ in uni]
    weight = {s: w for s, w in uni}
    root_to_sym = {s.replace(".", ""): s for s in symbols}
    dates = trading_sessions(args.start, args.end)
    if not dates:
        raise SystemExit(f"no trading sessions in [{args.start}, {args.end}]")

    print(f"universe={len(symbols)} sessions={len(dates)} [{dates[0]}..{dates[-1]}] "
          f"dataset={DATASET} schema={SCHEMA} snap={args.snap_utc}Z out={args.out}")

    plan = plan_missing(args.out, symbols, dates, force=args.force)
    n_missing = sum(len(v) for v in plan.values())
    print(f"cells: total={len(symbols) * len(dates)} to_pull={n_missing} "
          f"(over {len(plan)} sessions){' [FORCE]' if args.force else ''}")
    if n_missing == 0:
        print("ALL boards already on disk — nothing to pull, $0.00.")
        return 0

    if client is None:
        key = read_api_key(args.env_file)
        if db is None:
            raise SystemExit("BLOCKED: databento is not installed; cannot pull.")
        client = db.Historical(key=key)

    print("\nFREE preflight (metadata.get_cost — no egress):")
    pf = preflight(client, plan, symbols, weight, snap_hm=args.snap_utc, cap=args.cap,
                   sample_days=args.sample_days, index_symbol=args.index_symbol,
                   min_degrade_names=args.min_degrade_names)
    print(f"\nESTIMATE (remaining spend): ${pf.total_cost:.4f} = "
          f"${pf.unit_cost:.8f}/sym-day x {pf.n_missing} cells (cap ${args.cap:.2f})")
    if pf.blocked:
        print(f"\nBLOCKED: even {args.index_symbol} + {args.min_degrade_names} names exceed "
              f"cap ${args.cap:.2f} (kept only {pf.keep}). No data pulled.", file=sys.stderr)
        return 3

    print(f"\nAuthorized estimate ${pf.total_cost:.4f} within cap ${args.cap:.2f}. "
          f"Symbols kept: {len(pf.keep)} (dropped {len(pf.dropped)}).")
    if args.dry_run:
        print("DRY RUN — no data pulled.")
        return 0

    keepset = set(pf.keep)
    pull_plan = {d: [s for s in plan[d] if s in keepset] for d in plan}
    pull_plan = {d: v for d, v in pull_plan.items() if v}

    res = pull(client, pull_plan, args.out, snap_hm=args.snap_utc, root_to_sym=root_to_sym,
               unit_cost=pf.unit_cost, force=args.force, store_loader=store_loader)

    args.out.mkdir(parents=True, exist_ok=True)
    mpath = args.out / f"manifest_hive_{args.start}_{args.end}_{args.snap_utc.replace(':', '')}.csv"
    pd.DataFrame(res.manifest).to_csv(mpath, index=False)
    print(f"\nDONE boards_written={res.boards_written} dates_written={res.dates_written} "
          f"unmapped_rows={res.unmapped_rows} failed_sessions={len(res.failed_dates)}")
    print(f"ACTUAL SPEND (realized preflight of pulled cells): ${res.actual_spend:.4f}")
    print(f"kept N={len(pf.keep)} dropped={len(pf.dropped)} manifest={mpath}")
    return 0 if not res.failed_dates else 5


def main() -> int:
    return run(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
