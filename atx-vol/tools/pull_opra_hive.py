#!/usr/bin/env python3
"""Cost-gated OPRA cbbo-1m pull into the v2 date-partitioned hive.

Port of ``tools/pull_opra_universe_batch.py`` targeting the redesigned layout
(design §3): each session is ONE parquet file holding every symbol,

    <root>/date=YYYY-MM-DD/data.parquet

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
Everything else is carried over verbatim: key handling, XNYS calendar, the FREE
``get_cost`` preflight + retry/sampling, the $-cap degrade (index leg + top-N by
weight, BLOCK below floor), the DBN cache + quarantine, and spend accounting.

Snapshot minute / DST note
--------------------------
``--snap-utc`` (default, unchanged from v1) is a FIXED UTC minute (default
19:55:00Z) for the whole run, matching the existing hive and the atx-core
databento_bulk_opra C++ precedent. 19:55Z == 15:55 America/New_York during EDT
and == 14:55 ET during EST -- a fixed UTC minute keeps the file-naming/manifest
scheme uniform, but the ET *market-clock* time it lands on DRIFTS an hour across
the DST boundary.

``--snap-et HH:MM`` (opt-in, mutually exclusive with ``--snap-utc``) instead
fixes the ET market-clock time and lets the UTC minute vary per session via
``zoneinfo`` (e.g. 15:55 ET == 19:55Z in EDT, 20:55Z in EST) -- the correct
choice for a multi-year backfill (2022-2026 crosses many DST transitions) where
staying anchored to the same point in the trading session matters more than a
uniform UTC clock minute. The per-date resolved minute still flows into the DBN
cache name and the date file's stamped ``ts``; a resume additionally checks that
stamp against the expected instant for that date and re-pulls (replacing, not
unioning) on a mismatch (e.g. a DST-boundary re-run, or switching snapshot
conventions between runs).

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
     Two sidecars carry that guarantee where there is no data to cache:
     <root>/_absent/<date>.json latches SYMBOLS the provider confirmed had no
     rows, and <root>/_empty/<date>.json latches a whole DATE whose response was
     empty (the permanent case being a snapshot minute after an early close,
     which returns nothing forever). Both are skipped on resume for $0; both are
     cleared explicitly by the operator (--retry-empty / --force / deleting the
     file), never automatically inside a run.

API key comes from $DATABENTO_API_KEY or a .env file (default search: ./.env then
C:/atx/.env, or --env-file). The key is never printed or logged.

Usage:
  python pull_opra_hive.py \
      --universe atx-vol/data/universe/spy_top50_2026-01-01.csv \
      --start 2026-07-20 --end 2026-07-21 \
      --out C:/atx-data/opra-hive [--snap-utc 19:55 | --snap-et 15:55] \
      [--cap 100] [--dry-run]
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import os
import pathlib
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, Optional
from zoneinfo import ZoneInfo

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

# One parquet file per session; true hive `date=` key (design §3). Frozen name
# by the plan's Global Constraints (atx-core write_hive_parquet hard-codes
# "/data.parquet"); the migrate tool and `load_opra_hive` reader use the same —
# keep in lockstep with them.
DATE_FILE = "data.parquet"

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


# ── ET-anchored snapshot minute (DST-aware) ─────────────────────────────────────
# ``--snap-et`` (opt-in, mutually exclusive with the legacy ``--snap-utc``) fixes
# the snapshot to a market-clock time (e.g. 15:55 America/New_York) instead of a
# fixed UTC minute. The UTC instant that maps to then varies with DST across a
# multi-year backfill (2022-2026 crosses many transitions) -- a fixed-UTC-minute
# hive would otherwise anchor to a DRIFTING ET time across the DST boundary,
# which is the actual correctness bug this flag exists to fix.
ET = ZoneInfo("America/New_York")


def snapshot_minute_utc(date_str: str, et_hhmm: str) -> str:
    """UTC HH:MM of `et_hhmm` America/New_York on `date_str` (DST-aware)."""
    h, mnt = (int(x) for x in et_hhmm.split(":"))
    d = dt.date.fromisoformat(date_str)
    local = dt.datetime(d.year, d.month, d.day, h, mnt, tzinfo=ET)
    u = local.astimezone(dt.timezone.utc)
    return f"{u.hour:02d}:{u.minute:02d}"


def snap_window(date: str, snap_hm: "str | dict[str, str]") -> tuple[str, str]:
    """Build the one-minute [start, end) window for ``date``.

    ``snap_hm`` is either a single "HH:MM" (legacy: one fixed UTC minute for the
    whole run) or a ``{date: "HH:MM"}`` map (per-date minute, e.g. from
    ``--snap-et``); resolved here so every call site (``get_cost_retry`` via
    ``preflight``, and ``pull``'s ``get_range``) gets the right per-date window
    without having to know which mode is active."""
    hm = snap_hm[date] if isinstance(snap_hm, dict) else snap_hm
    hh, mm = hm.split(":")
    start = f"{date}T{hh}:{mm}:00"
    end_minute = int(hh) * 60 + int(mm) + 1
    end = f"{date}T{end_minute // 60:02d}:{end_minute % 60:02d}:00"
    return start, end


def get_cost_retry(client, parents: list[str], date: str,
                   snap_hm: "str | dict[str, str]", attempts: int = 4) -> float:
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
    """Distinct ``underlying`` values in a date file.

    Fast path: when every row group holds a single underlying (min == max ==
    that value), the footer statistics give the full set with no data scan. But
    the one-row-group-per-underlying layout is NOT frozen (design §3 defers it),
    so a file written by a generic writer may put several underlyings in one
    sorted row group — then min/max are only the extremes and the middle names
    would be silently missed. So if ANY row group lacks stats or spans a range
    (min != max), fall back to a single-column full read of ``underlying`` for
    the whole file — correct for any writer, still cheaper than reading data
    columns. Getting this wrong would mark on-disk symbols as missing and
    re-pull them for real money."""
    pf = pq.ParquetFile(path)
    md = pf.metadata
    idx = pf.schema_arrow.get_field_index("underlying")
    if idx < 0:
        raise ValueError(f"{path}: no 'underlying' column")
    syms: set[str] = set()
    for rg in range(md.num_row_groups):
        st = md.row_group(rg).column(idx).statistics
        if st is None or not st.has_min_max or st.min != st.max:
            return set(pf.read(columns=["underlying"]).column(0).to_pylist())
        syms.add(st.min)
    return syms


# Sentinel: the file holds MORE THAN ONE distinct ``ts`` value across its row
# groups -- itself a violation of the one-constant-ts-per-date invariant the
# C++ loader chain (first_ts_ns -> snapshot_ts_ns -> corpus fingerprint) relies
# on. Distinct from ``None`` ("can't tell" -- no row groups / no ts column,
# pre-existing fail-open behavior, unchanged) so `plan_missing` can force a
# mismatch/repull for a mixed file without silently matching an arbitrary row
# group's value, and without touching the None-fail-open path at all.
_MIXED_SNAP_TS = object()


def _date_file_snap_ts(path: pathlib.Path):
    """Cheaply read the date file's stamped ``ts`` -- normally one constant
    instant for the whole session (``decode_date_frame`` stamps every row the
    same). Scans EVERY row group's footer statistics (min==max means a single
    value -- no data read needed for a well-formed file); only falls back to
    reading a row group's own ``ts`` column when its footer stats are absent or
    span more than one value. Returns:
      * a ``pd.Timestamp`` when exactly one distinct value is present across
        the whole file (the healthy case).
      * ``_MIXED_SNAP_TS`` when MORE THAN ONE distinct value is found anywhere
        in the file -- always treated as a mismatch by the caller, never
        silently matched against whichever row group happened to be read
        first (row groups are NOT guaranteed to be in any particular order
        relative to "the truth").
      * ``None`` when nothing can be determined (no row groups, or no ``ts``
        column) -- unchanged pre-existing fail-open behavior."""
    pf = pq.ParquetFile(path)
    md = pf.metadata
    if md.num_row_groups == 0:
        return None
    idx = pf.schema_arrow.get_field_index("ts")
    if idx < 0:
        return None
    seen: set = set()
    for rg in range(md.num_row_groups):
        st = md.row_group(rg).column(idx).statistics
        if st is not None and st.has_min_max and st.min == st.max:
            seen.add(pd.Timestamp(st.min))
        else:
            tbl = pf.read_row_group(rg, columns=["ts"])
            seen.update(pd.Timestamp(v) for v in tbl.column(0).to_pylist())
        if len(seen) > 1:
            return _MIXED_SNAP_TS
    if len(seen) != 1:
        return None
    return next(iter(seen))


# ── Absent-symbol sidecar (<out>/_absent/<date>.json) ───────────────────────────
# Remembers, per date and snapshot minute, which requested underlyings came back
# with zero rows (e.g. genuinely no listed options that session) so a resume
# doesn't re-queue -- and re-bill -- the same permanently-absent name forever.
# Read/write are best-effort and atomic: a missing or corrupt sidecar is treated
# as "no record" (never crashes a resume; the next pull rewrites it).
def _absent_sidecar_path(out_root: pathlib.Path, date: str) -> pathlib.Path:
    return out_root / "_absent" / f"{date}.json"


def _load_absent_sidecar(out_root: pathlib.Path, date: str) -> Optional[dict]:
    path = _absent_sidecar_path(out_root, date)
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError:
        return None
    try:
        data = json.loads(raw)
    except ValueError:
        return None
    if not isinstance(data, dict) or "minute_utc" not in data or "symbols" not in data:
        return None
    return data


def _write_absent_sidecar(out_root: pathlib.Path, date: str, minute_utc: str,
                          symbols: list[str]) -> None:
    """Atomic tmp + ``os.replace``, matching ``_write_date_file``'s pattern."""
    path = _absent_sidecar_path(out_root, date)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "minute_utc": minute_utc,
        "symbols": sorted(symbols),
        "asof": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(payload), encoding="utf-8")
    os.replace(tmp, path)


def _merge_absent_sidecar(out_root: pathlib.Path, date: str, minute_utc: str,
                          requested_symbols: list[str], zero_row_symbols: list[str],
                          force: bool) -> None:
    """Record this pull's zero-row requested symbols for ``date``.

    ``force`` (true for an explicit ``--force`` OR a minute-mismatch repull --
    see ``pull``'s ``sidecar_force``) makes this call authoritative for every
    symbol in ``requested_symbols`` -- each is either re-confirmed absent (in
    ``zero_row_symbols``) or dropped from the sidecar (it came back with data
    this time). Critically, a prior entry for a symbol NOT in
    ``requested_symbols`` this round (e.g. one the cap-degrade filter dropped
    before ``pull()`` ever saw it this session) is left untouched: this call
    never re-checked it, so ``force`` must not erase that memory and cause it
    to be re-queued -- and re-billed -- on a later run. Without ``force``,
    entries only accumulate (nothing already recorded is ever dropped this
    way); a prior record for a DIFFERENT minute (e.g. a pre-DST-transition
    entry) is discarded rather than merged, since a sidecar holds exactly one
    minute's data. Nothing is written when there's nothing new to say and no
    force (existing sidecar, if any, is left completely untouched)."""
    prior = _load_absent_sidecar(out_root, date)
    prior_syms = (set(prior.get("symbols", []))
                 if prior and prior.get("minute_utc") == minute_utc else set())
    if force:
        untouched = prior_syms - set(requested_symbols)
        new_syms = untouched | set(zero_row_symbols)
    else:
        if not zero_row_symbols:
            return
        new_syms = prior_syms | set(zero_row_symbols)
    _write_absent_sidecar(out_root, date, minute_utc, sorted(new_syms))


# ── Settled-empty latch (<out>/_empty/<date>.json) ──────────────────────────────
# FIX-IMPORTANT-2. Remembers, per date, that the provider answered a specific
# question -- (snapshot minute, requested symbol set) -- with ZERO rows, so a
# resume treats it as SETTLED and skips it instead of paying to ask again.
#
# This is the absent-latch's shape applied one level up. `_absent` latches "this
# SYMBOL had no data that session"; this latches "this whole DATE returned
# nothing at that minute". Same one-JSON-per-date layout, same atomic
# tmp + os.replace write, same corrupt-tolerant read, same "a sidecar holds
# exactly one minute's verdict" rule -- deliberately NOT a parallel mechanism.
#
# It cannot be folded into `_absent` itself: a settled-empty date is (by
# construction) a minute-mismatch REPULL date, and `plan_missing` `continue`s out
# of the repull branch BEFORE sidecar subtraction runs, so an `_absent` entry
# would never be consulted. It must also stay OUT of `_absent` semantically --
# recording "every symbol is absent" would latch the date complete forever, which
# is exactly what the EMPTY-SESSION branch below refuses to do.
#
# The identity is the CACHE ENTRY's identity, because this latch is a $0
# stand-in for the cached empty response that FIX-N-1 deletes: (minute_utc,
# digest) where digest is `sha256(date|sorted(miss))[:12]`. So a different
# snapshot minute or a different requested symbol set is a different question and
# re-pulls automatically -- which matters, because the real-world cause of a
# permanently-empty answer is a snapshot minute that landed after an early close,
# and the real-world fix for that is to move the snapshot minute.
def _empty_latch_path(out_root: pathlib.Path, date: str) -> pathlib.Path:
    return out_root / "_empty" / f"{date}.json"


def _load_empty_latch(out_root: pathlib.Path, date: str) -> Optional[dict]:
    path = _empty_latch_path(out_root, date)
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError:
        return None
    try:
        data = json.loads(raw)
    except ValueError:
        return None
    if not isinstance(data, dict) or "minute_utc" not in data or "digest" not in data:
        return None
    return data


def _is_settled_empty(out_root: pathlib.Path, date: str, minute_utc: str, digest: str) -> bool:
    latch = _load_empty_latch(out_root, date)
    return bool(latch and latch.get("minute_utc") == minute_utc
                and latch.get("digest") == digest)


def _write_empty_latch(out_root: pathlib.Path, date: str, minute_utc: str,
                       digest: str, n_requested: int) -> None:
    """Atomic tmp + ``os.replace``, matching ``_write_absent_sidecar``."""
    path = _empty_latch_path(out_root, date)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "minute_utc": minute_utc,
        "digest": digest,
        "n_requested": n_requested,
        "asof": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(payload), encoding="utf-8")
    os.replace(tmp, path)


def _clear_empty_latch(out_root: pathlib.Path, date: str) -> None:
    """Best-effort: a latch we cannot remove is never worth failing a pull for
    (worst case the operator deletes it by hand, which is a documented step)."""
    try:
        _empty_latch_path(out_root, date).unlink(missing_ok=True)
    except OSError:
        pass


class MissingPlan(dict):
    """``plan_missing``'s return value: a plain ``dict[str, list[str]]`` (date ->
    missing symbols) for full backward compatibility with existing equality
    assertions (``dict.__eq__`` ignores subclass/extra attributes), PLUS
    ``repull_dates``: the subset of keys whose on-disk file failed the minute
    check and must be REPLACED, not unioned, when pulled (see ``pull``'s
    ``date_force``)."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.repull_dates: set[str] = set()


def plan_missing(out_root, symbols: list[str], dates: list[str],
                 force: bool = False,
                 expected_minute: Optional[dict[str, str]] = None) -> MissingPlan:
    """Map each date to the requested symbols not yet on disk (universe order).

    Empty/absent date file -> all requested symbols. Partial file -> only the
    underlyings missing from its footer. Complete file -> date omitted. With
    ``force`` every requested symbol is (re)planned for every date.

    ``expected_minute`` (optional, ``{date: "HH:MM"}`` UTC) opts a caller into
    two DST-correctness features, both no-ops when omitted (so existing direct
    callers/tests that don't pass it see byte-identical behavior):
      * Resume minute check: if the on-disk file's stamped ``ts`` does not equal
        `date + expected_minute[date]` -- OR the file itself holds more than
        one distinct ``ts`` (``_date_file_snap_ts``'s ``_MIXED_SNAP_TS``, e.g.
        a prior partial repull that predates this fix) -- the date is fully
        re-planned (all requested symbols) and added to ``repull_dates`` --
        pull() replaces (not unions) that date's file, discarding whatever was
        on disk rather than force-merging into it.
      * Absent-symbol sidecar subtraction: for a date that DOES match, symbols
        recorded absent in ``<out>/_absent/<date>.json`` for that SAME minute
        are dropped from the missing set (never re-queued/re-billed)."""
    out_root = pathlib.Path(out_root)
    plan = MissingPlan()
    for date in dates:
        target = out_root / f"date={date}" / DATE_FILE
        want_hm = expected_minute.get(date) if expected_minute else None
        if force or not target.exists():
            plan[date] = list(symbols)
            continue
        if want_hm is not None:
            have_ts = _date_file_snap_ts(target)
            want_ts = pd.Timestamp(f"{date}T{want_hm}:00")
            mixed = have_ts is _MIXED_SNAP_TS
            if mixed or (have_ts is not None and have_ts != want_ts):
                have_hm = "mixed" if mixed else f"{have_ts.hour:02d}:{have_ts.minute:02d}"
                print(f"MINUTE-MISMATCH {date} have={have_hm} want={want_hm} — repull",
                      file=sys.stderr)
                plan[date] = list(symbols)
                plan.repull_dates.add(date)
                continue
        have = date_file_underlyings(target)
        miss = [s for s in symbols if s not in have]
        if want_hm is not None and miss:
            absent = _load_absent_sidecar(out_root, date)
            if absent is not None and absent.get("minute_utc") == want_hm:
                absent_syms = set(absent.get("symbols", []))
                miss = [s for s in miss if s not in absent_syms]
        if miss:
            plan[date] = miss
    return plan


# ── Decode one day's DBN into a single 8-column frame ──────────────────────────
def decode_date_frame(store, date: str, snap_hm: str,
                      root_to_sym: dict[str, str],
                      want_syms: Optional[set[str]]) -> tuple[pd.DataFrame, int, int]:
    """Decode a day's DBN to the canonical 8-column frame, restricted to
    ``want_syms``. Returns (frame, n_unmapped_rows, n_returned_rows) --
    ``n_returned_rows`` is the RAW decoded row count before the underlying
    mapping and ``want_syms`` filters (row-level, not cell-level: this call
    already scopes the API request to the requested parents, so it is the
    honest "how much did the provider actually send back" figure, distinct
    from "how many of the cells we asked for came back written")."""
    df = store.to_df(price_type="fixed", pretty_ts=False, map_symbols=True)
    if df is None or df.empty:
        return _empty_decoded(), 0, 0
    df = df.reset_index()
    n_returned = len(df)
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
    return frame[COLUMNS].reset_index(drop=True), n_unmapped, n_returned


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
        # NOT pq.read_table(): existing_path lives under a `date=YYYY-MM-DD/`
        # directory, which pyarrow's dataset layer treats as a Hive partition
        # key and injects as a spurious `date` column into the result
        # (reproduced against pyarrow 18.0.0). ParquetFile.read() reads the
        # file's actual on-disk schema only -- same fix already applied in
        # migrate_opra_hive.py's _merge_union_date for this identical trap.
        # Do not "simplify" this back to pq.read_table.
        existing = pq.ParquetFile(existing_path).read().to_pandas()[COLUMNS]
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
              weight: dict[str, float], *, snap_hm: "str | dict[str, str]", cap: float,
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
    # NOTE: this is a sampled preflight unit cost x cells actually pulled this
    # session -- a modeled ESTIMATE, not an invoice or an exact post-call
    # charge (see F-06 / R2-b: it used to be printed and named as if it were).
    realized_estimate: float
    manifest: list[dict]
    failed_dates: list[str]
    cells_requested: int = 0  # every (date, symbol) cell handed to this call
    cells_failed: int = 0     # cells whose date failed get_range after retries
    rows_returned: int = 0    # raw decoded rows, pre-mapping/want filters (row-level)


def pull(client, plan: dict[str, list[str]], out_root, *, snap_hm: "str | dict[str, str]",
         root_to_sym: dict[str, str], unit_cost: float = 0.0, force: bool = False,
         store_loader: Optional[Callable] = None,
         repull_dates: Optional[set] = None,
         retry_empty: bool = False) -> PullResult:
    """Pull every planned date: one ``get_range`` over the union of its missing
    parents, decode to one frame, merge into the date file. Resumable: a cached
    DBN is decoded without an API call; a present date file's symbols are already
    excluded upstream by ``plan_missing``.

    ``snap_hm`` is either one fixed "HH:MM" (legacy, applied to every date) or a
    ``{date: "HH:MM"}`` map (``--snap-et``'s per-date, DST-aware minute); either
    way the RESOLVED per-date minute flows into the DBN cache name, the
    ``get_range`` window, and the frame's stamped ``ts``.

    ``repull_dates`` (from ``plan_missing``'s minute-mismatch check) marks dates
    whose on-disk file is stamped with the WRONG snapshot instant for the whole
    file, not just for the requested symbols. Those dates get a genuine full
    replacement (``existing=None`` into ``merge_date_file`` -- the file is
    written from scratch, containing only this call's own frame), never a
    force-style merge: a force merge only replaces underlyings present in the
    fresh frame, which would silently leave any OLD-minute row behind for a
    symbol this call didn't get data for this time (zero rows, a cap-degrade
    drop, ...), corrupting the one-ts-per-date invariant the C++ loader chain
    relies on. Omitted/None for callers that don't opt into the minute check
    (byte-identical to the old force-only behavior).

    ``retry_empty`` (FIX-IMPORTANT-2) is the operator's explicit escape hatch
    from the settled-empty latch: it clears the latch for any date it covers and
    re-pulls, which COSTS MONEY. Default False, and nothing inside a run ever
    sets it -- recovery from a settled-empty date is operator-initiated only."""
    store_loader = store_loader or _default_store_loader
    out_root = pathlib.Path(out_root)
    dbn_dir = out_root / "_dbn"
    dbn_dir.mkdir(parents=True, exist_ok=True)
    manifest: list[dict] = []
    boards = dates_written = unmapped = 0
    realized_estimate = 0.0
    cells_requested = cells_failed = rows_returned = 0
    failed: list[str] = []
    repull_dates = repull_dates or set()
    ordered = sorted(plan)

    for i, date in enumerate(ordered):
        miss = list(plan[date])
        if not miss:
            continue
        hm = snap_hm[date] if isinstance(snap_hm, dict) else snap_hm
        cells_requested += len(miss)
        want = set(miss)
        parents = [to_parent(s) for s in miss]
        digest = hashlib.sha256((date + "|" + ",".join(sorted(miss))).encode()).hexdigest()[:12]
        dbn_path = dbn_dir / f"{date}_{hm.replace(':', '')}_{digest}.dbn.zst"

        # FIX-IMPORTANT-2. A settled-empty date is skipped BEFORE any provider
        # contact -- this is the $0 guarantee, and it has to sit above both the
        # cache read and `get_range`.
        if _is_settled_empty(out_root, date, hm, digest):
            if not (force or retry_empty):
                print(f"EMPTY-LATCHED {date}: the provider already answered this exact "
                      f"request (minute {hm}, {len(miss)} boards) with 0 rows, and that is "
                      f"recorded as settled in _empty/{date}.json -- SKIPPED, no provider "
                      f"call, $0. This is the permanent case (e.g. a snapshot minute after "
                      f"an early close returns 0 rows forever), so retrying it on every "
                      f"resume would bill the same date again for the same answer. To force "
                      f"a fresh pull: re-run with --retry-empty, delete "
                      f"_empty/{date}.json, or move the snapshot minute (a latch is one "
                      f"minute's verdict and does not apply to another).", file=sys.stderr)
                cells_failed += len(miss)
                continue
            # Operator-initiated recovery only. Drop the latch AND any cached
            # empty response, so this run genuinely re-asks instead of replaying.
            _clear_empty_latch(out_root, date)
            try:
                dbn_path.unlink(missing_ok=True)
            except OSError:
                pass
            print(f"EMPTY-RETRY {date}: clearing the settled-empty latch on operator "
                  f"request ({'--force' if force else '--retry-empty'}) -- this date WILL "
                  f"be billed again.", file=sys.stderr)

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
                    start, end = snap_window(date, hm)
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
                cells_failed += len(miss)
                continue
            # Atomic cache write (same-fs tmp -> os.replace), so a crash during
            # the write never leaves a torn .dbn.zst that wedges the next resume.
            dbn_tmp = dbn_path.parent / (dbn_path.name + ".tmp")
            store.to_file(dbn_tmp)
            os.replace(dbn_tmp, dbn_path)
            tag = "pulled"

        frame, nu, nr = decode_date_frame(store, date, hm, root_to_sym, want)
        unmapped += nu
        rows_returned += nr
        target = out_root / f"date={date}" / DATE_FILE
        is_repull = date in repull_dates
        if is_repull and len(frame) == 0:
            # FIX-C-2. A repull is a genuine FULL replacement (see below), so an
            # empty fresh response would write a 0-row parquet OVER the existing
            # file. That destroys paid data -- and it is not even recoverable by
            # re-running, because a 0-row file makes `_date_file_snap_ts` return
            # None, `plan_missing` fails OPEN on None (re-planning and re-BILLING
            # every symbol on every future resume, against dates that are now
            # metered), and with no readable `ts` the date can never re-enter the
            # repull path at all -- the minute-mismatch detector goes permanently
            # blind to it. "Replace with nothing" was never the intent of the
            # repull exception to merge_date_file's never-overwrite rule.
            #
            # An empty response is a provider hiccup / a snapshot minute that
            # landed after an early close / a symbol set the provider sharded
            # away -- all transient, none of them evidence that the stored rows
            # are wrong. So keep the file exactly as it is, say so loudly, and
            # leave the date planned: its on-disk `ts` is untouched, so the very
            # next resume re-detects MINUTE-MISMATCH and re-queues the repull.
            #
            # FIX-N-1. That last sentence was FALSE until this unlink existed.
            # The raw response is cached above (`store.to_file` -> os.replace)
            # BEFORE this guard, under a digest of `date|sorted(miss)`. A resume
            # re-plans the identical symbol set for the identical date, so it
            # recomputes the identical digest, hits `dbn_path.exists()`, replays
            # the EMPTY store from disk, and never calls `get_range` at all. The
            # date was wedged permanently: every resume printed this same line
            # and exited 0 (the date is deliberately not in `failed_dates`), the
            # file stayed stamped at the stale minute, and FIX-C-1 turned it into
            # a per-cell load error forever after, so the partition was never
            # built. Nothing upstream could see it either -- `EMPTY-REPULL` is
            # not parsed by the orchestrator and `parse_minute_mismatch_dates`
            # is not called by anything.
            #
            # An empty response is not a cacheable ANSWER, so it does not get to
            # occupy the cache slot for a real one. Two properties this must
            # keep, in tension:
            #
            #   * It must NOT re-bill inside this run. The `continue` below is
            #     the whole guarantee: the date is dropped for this invocation,
            #     billed exactly once, no automatic retry. Deleting the cache
            #     only restores the ABILITY of a later, explicitly-invoked run
            #     to fetch -- it never itself causes a fetch.
            #   * It must only fire on an EMPTY PROVIDER RESPONSE (`nr == 0`),
            #     not merely an empty decoded frame. A response that did return
            #     rows which then all failed the underlying mapping or the
            #     want-set filter will decode identically every single time, so
            #     dropping ITS cache would buy a guaranteed re-bill for a
            #     guaranteed identical outcome on every future resume -- the
            #     exact forever-re-billing loop FIX-C-2 exists to prevent. That
            #     case keeps its cache; its problem is the symbol map, not the
            #     provider.
            #
            # The stderr line below is written to be TRUE of whichever branch
            # ran -- the previous wording promised a recovery the code did not
            # implement, and an operator who believes a false recovery promise
            # concludes the tool is broken rather than that a file needs
            # deleting.
            # FIX-IMPORTANT-2. `nr == 0` separates "the provider returned
            # nothing" from "rows came back and were all filtered out", which is
            # the right axis -- but "returned nothing" is NOT the same as
            # "transient", and FIX-N-1 treated it as if it were. The comment
            # above names the counter-example itself: a snapshot minute that
            # landed after an early close returns 0 rows at that window EVERY
            # TIME, forever. Unlinking the cache made every subsequent resume
            # call `get_range` again, so a $0-forever path became pay-per-resume
            # -- the recurring-charge loop FIX-C-2 exists to prevent, narrowed
            # rather than removed.
            #
            # So: still drop the cache (an empty response is not a cacheable
            # ANSWER and must not occupy the slot for a real one), but LATCH the
            # result as settled, exactly as `_absent` latches a confirmed absent
            # symbol. The gate at the top of this loop then makes every later
            # resume free, and recovery stays explicitly operator-initiated
            # (`--retry-empty`, `--force`, deleting the sidecar, or moving the
            # snapshot minute). Nothing here can cause an automatic re-bill.
            #
            # MINOR-1. Three reachable states, three texts. The failed-`unlink`
            # case used to fall through into the `nr > 0` wording and print "the
            # provider DID return 0 row(s) that mapped to none of the requested
            # underlyings" -- self-contradictory, and false. It is reachable on
            # Windows: `PermissionError` is an `OSError`, and an AV scanner or a
            # concurrent pull holding the handle is the ordinary way to get one.
            # N-1's original sin was stderr that was not true of the branch that
            # actually ran; that must not be re-committed here.
            unlink_error = None
            if nr == 0:
                try:
                    dbn_path.unlink(missing_ok=True)
                except OSError as exc:  # noqa: BLE001
                    # Never fatal: a cache file we cannot remove costs a wedged
                    # date, but raising here would abandon the rest of the plan.
                    unlink_error = str(exc)[:70]
                _write_empty_latch(out_root, date, hm, digest, len(miss))
            if nr == 0 and unlink_error is None:
                recovery = (f"The date stays planned, its empty response was NOT cached, and "
                            f"the empty ANSWER is latched as settled in _empty/{date}.json so "
                            f"every later resume skips it for $0 instead of re-billing the "
                            f"same date for the same nothing. To force a fresh pull once you "
                            f"believe the provider has recovered: --retry-empty, or delete "
                            f"that sidecar. If the cause is an early close, move the snapshot "
                            f"minute instead -- the latch only covers minute {hm}.")
            elif nr == 0:
                recovery = (f"The date stays planned and the empty ANSWER is latched as "
                            f"settled in _empty/{date}.json, so later resumes are $0. The "
                            f"empty response itself could NOT be removed from the DBN cache "
                            f"({unlink_error}) -- it is still at _dbn/{dbn_path.name}, which "
                            f"is harmless (it only makes the skip doubly free) but means a "
                            f"--retry-empty must be able to delete it to actually re-pull. "
                            f"Delete both files by hand if it stays locked.")
            else:
                recovery = (f"The date stays planned, but the provider DID return {nr} row(s) "
                            f"that mapped to none of the requested underlyings, so the response "
                            f"is kept at _dbn/{dbn_path.name} and a resume will replay it rather "
                            f"than re-bill for the same answer. Fix the symbol mapping, or "
                            f"delete that file to force a fresh pull.")
            print(f"EMPTY-REPULL {date}: 0/{len(miss)} requested boards returned at {hm} on a "
                  f"repull-flagged (minute-mismatch) date -- the existing date file is KEPT "
                  f"rather than replaced by an empty one (writing it would destroy paid data, "
                  f"blind the mismatch detector, and re-bill this date on every resume). "
                  f"{recovery}",
                  file=sys.stderr)
            cells_failed += len(miss)
            continue
        if is_repull:
            # Minute mismatch: the file's stamped ts is flat-out wrong for the
            # WHOLE date, not just for the requested symbols. A force-style
            # merge_date_file call would only replace underlyings present in
            # `frame` (this pull's actual return), preserving anything ELSE
            # already on disk -- so a requested-but-zero-row symbol, a
            # cap-degrade drop, or any name outside the current universe would
            # silently keep its OLD-minute rows, leaving the file holding two
            # different snapshot instants at once (violating the
            # one-constant-ts-per-date invariant the C++ loader chain --
            # first_ts_ns -> snapshot_ts_ns -> corpus fingerprint -- relies on,
            # and defeating `_date_file_snap_ts`'s mismatch check forever:
            # the mismatch branch `continue`s before sidecar subtraction ever
            # runs, so there is no other path back to a clean file). Treat the
            # date as if no file existed at all: a genuine full replacement
            # with only what THIS pull actually returned.
            existing = None
        else:
            existing = target if target.exists() else None
        merge_date_file(existing, frame, target, force=force)
        # FIX-IMPORTANT-2. This date produced real rows, so any settled-empty
        # latch it carries is stale and must not be left to suppress a future
        # legitimate pull of the same question.
        _clear_empty_latch(out_root, date)

        written = set(frame["underlying"].unique().tolist())
        zero_row_syms = [s for s in miss if s not in written]
        sidecar_force = force or is_repull
        if not written and len(miss) > 1:
            # A whole-session zero-row response (every requested symbol, not
            # just one) is a bad-window/holiday signature -- e.g. an XNYS
            # early close (Jul 3, day after Thanksgiving, Christmas Eve: 13:00
            # ET close) where a 15:55 ET cbbo-1m window falls after the
            # session ended -- NOT genuine per-name absence. Recording it
            # would latch the date "complete" forever (plan_missing would
            # subtract every symbol via the sidecar on every future resume,
            # recoverable only via --force). Skip the sidecar write entirely
            # and log loudly instead so the orchestrator can see the date was
            # skipped-empty and may need a --snap adjustment or a deliberate
            # re-check, not silent amnesia.
            print(f"EMPTY-SESSION {date}: 0/{len(miss)} requested boards returned at "
                  f"{hm} -- likely an early-close/holiday session outside the snapshot "
                  f"window, not per-name absence; absent-symbol sidecar NOT recorded "
                  f"(will re-check next resume)", file=sys.stderr)
        else:
            _merge_absent_sidecar(out_root, date, hm, miss, zero_row_syms, sidecar_force)
        for s in miss:
            if s in written:
                recs = int((frame["underlying"] == s).sum())
                manifest.append({"date": date, "symbol": s, "records": recs, "status": "ok"})
            else:
                manifest.append({"date": date, "symbol": s, "records": 0, "status": "no_options"})
        boards += len(written)
        dates_written += 1
        # Realized ESTIMATE = sampled unit cost x boards actually pulled this
        # session (cached/skipped boards were already paid or never billable).
        # This is still the free preflight estimator pricing them after the
        # fact -- never an invoice or an exact post-call charge (F-06).
        if tag == "pulled":
            realized_estimate += unit_cost * len(written)
        if i % 10 == 0 or tag == "cached":
            print(f"  [{i + 1}/{len(ordered)}] {date} {tag}: {len(written)} boards "
                  f"(running_estimate=${realized_estimate:.4f})", flush=True)

    return PullResult(
        boards, dates_written, unmapped, realized_estimate, manifest, failed,
        cells_requested=cells_requested, cells_failed=cells_failed, rows_returned=rows_returned,
    )


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
    snap = ap.add_mutually_exclusive_group()
    snap.add_argument("--snap-utc", default="19:55",
                      help="HH:MM UTC snapshot minute (default 19:55; FIXED for the whole run "
                      "-- drifts against market-clock time across a DST boundary)")
    snap.add_argument("--snap-et", default=None,
                      help="HH:MM America/New_York snapshot time (mutually exclusive with "
                      "--snap-utc); DST-aware -- the UTC minute is computed per session via "
                      "zoneinfo, so it stays anchored to the same market-clock time across DST "
                      "transitions (e.g. 15:55 ET == 19:55Z in EDT, 20:55Z in EST)")
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
    ap.add_argument("--retry-empty", action="store_true",
                    help="COSTS MONEY. Clear the settled-empty latches "
                         "(<out>/_empty/<date>.json) for the dates in range and re-pull them. "
                         "A date whose provider response was empty is latched as settled so "
                         "resumes skip it for $0 -- the permanent case (a snapshot minute "
                         "after an early close) would otherwise re-bill forever. Pass this "
                         "only when you believe the provider has recovered; if the cause was "
                         "an early close, change --snap-et/--snap-utc instead (a latch covers "
                         "exactly one snapshot minute and does not apply to another).")
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

    # ``--snap-et`` opts into a per-date, DST-aware UTC minute; the legacy
    # ``--snap-utc`` path passes ``args.snap_utc`` straight through as a plain
    # string everywhere pull()/preflight() take ``snap_hm``, exactly as before
    # this feature existed (byte-identical for existing callers). Either way,
    # `minute_for_date` is threaded into ``plan_missing`` so the resume minute
    # check + absent-symbol sidecar are active for both modes -- a no-op for a
    # well-behaved existing caller (same minute every run, no sidecar yet).
    if args.snap_et:
        minute_for_date = {d: snapshot_minute_utc(d, args.snap_et) for d in dates}
        snap_arg = minute_for_date
        snap_label = args.snap_et
        snap_desc = f"{args.snap_et} ET (DST-aware)"
    else:
        minute_for_date = {d: args.snap_utc for d in dates}
        snap_arg = args.snap_utc
        snap_label = args.snap_utc
        snap_desc = f"{args.snap_utc}Z"

    print(f"universe={len(symbols)} sessions={len(dates)} [{dates[0]}..{dates[-1]}] "
          f"dataset={DATASET} schema={SCHEMA} snap={snap_desc} out={args.out}")

    plan = plan_missing(args.out, symbols, dates, force=args.force,
                        expected_minute=minute_for_date)
    repull_dates = getattr(plan, "repull_dates", set())
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
    pf = preflight(client, plan, symbols, weight, snap_hm=snap_arg, cap=args.cap,
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
    pull_repull_dates = repull_dates & set(pull_plan)

    res = pull(client, pull_plan, args.out, snap_hm=snap_arg, root_to_sym=root_to_sym,
               unit_cost=pf.unit_cost, force=args.force, store_loader=store_loader,
               repull_dates=pull_repull_dates,
               retry_empty=getattr(args, "retry_empty", False))

    args.out.mkdir(parents=True, exist_ok=True)
    mpath = args.out / f"manifest_hive_{args.start}_{args.end}_{snap_label.replace(':', '')}.csv"
    pd.DataFrame(res.manifest).to_csv(mpath, index=False)
    print(f"\nDONE boards_written={res.boards_written} dates_written={res.dates_written} "
          f"unmapped_rows={res.unmapped_rows} failed_sessions={len(res.failed_dates)}")
    no_options = res.cells_requested - res.boards_written - res.cells_failed
    print(f"cells: requested={res.cells_requested} written={res.boards_written} "
          f"no_options={no_options} failed={res.cells_failed}  "
          f"|  rows: returned={res.rows_returned} unmapped={res.unmapped_rows}")
    print(f"REALIZED ESTIMATE (sampled preflight unit cost x cells actually pulled this "
          f"session): ${res.realized_estimate:.4f}")
    print("  NOTE: this is a modeled cost ESTIMATE re-pricing the cells this session "
          "actually pulled with the free preflight sampler -- it is NOT an invoice, NOT "
          "an exact post-call charge from Databento, and it excludes requested cells "
          "that never became a written board (no_options/failed above). Reconcile "
          "against provider billing before treating it as settled spend.")
    print(f"kept N={len(pf.keep)} dropped={len(pf.dropped)} manifest={mpath}")
    return 0 if not res.failed_dates else 5


def main() -> int:
    return run(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
