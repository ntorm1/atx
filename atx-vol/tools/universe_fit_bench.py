#!/usr/bin/env python3
"""Universe fit / price / risk robustness benchmark (lane F, wave 3 of the risk-engine sprint).

One command sequence, for a frozen binary and policy, produces
  (a) a LABEL-FREE robustness report over every chain of one bucket of the universe board
      (holdout names included, never scored against labels), and
  (b) a LABELLED report over a stratified panel of DEVELOPMENT-split underliers at every
      30-minute bucket,
both with per-(uid, root, bucket, expiry) chain scorecards, worst-N rankings and a
deterministic failure-mode taxonomy, plus the C++ probe (atx-vol-fit-risk-probe) that prices
and Greeks every served row through the risk engine. Docs: atx-vol/docs/universe-fit-bench.md.

Subcommands
  panel    draw <= 300 development underliers over 18 label-free strata (seed F-panel-v1)
  prepare  per-bucket development datasets (quote_benchmark.prepare) + labels.csv for the probe
  run      shard a dataset to a row budget by underlier hash, run atx-vol-quote-bench per shard
           (provenance via quote_benchmark.run_native) and the probe per shard; <= 600 s each
  score    chain scorecards -> report JSON (schema atx-vol-universe-fit-bench v1)
  report   summary.md over score reports (failure x archetype, worst-20, cross-bucket stability)

LABEL GOVERNANCE. Labels (srPrc, srVol, vendor Greeks) are opened only by `prepare` / `score`
and only for datasets whose manifest says rows_by_split == {"development": n}; the label-free
tier never calls the label reader. Validation membership is never written or printed.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import contextlib
import ctypes
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

import numpy as np
import polars as pl
import pyarrow as pa
import pyarrow.csv as pacsv
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quote_benchmark as qb  # noqa: E402  (imported, never edited)
import shape_cohort as sc  # noqa: E402
import universe_archetypes as ua  # noqa: E402

SCHEMA = "atx-vol-universe-fit-bench"
SCHEMA_VERSION = 1
RUN_SCHEMA = "atx-vol-universe-fit-run"
TOOL_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
PANEL_SEED = "F-panel-v1"
SHARD_SEED = "F-shard-v1"
PANEL_SIZE = 300
STEP_LIMIT_S = 600
MAX_FIT_WORKERS = 4
BUCKETS = ("1000", "1030", "1100", "1130", "1200", "1230", "1300", "1330", "1400", "1430", "1500")
SOURCE = Path(r"C:\Users\natha\Downloads\OptionIntradayHist_30min_2026-10-14.parquet")
KIVMIN = 0.005
TICK = 0.01
LABEL_COLUMNS = ("srPrc", "srVol", "reference_price", "reference_vol", "de", "ve")
SERVED = qb.PRICE_SERVED_STATUSES
SHAPE_STRATA = tuple(c[0] for c in sc.SYMBOL_CLASSES) + tuple(c[0] for c in sc.EXPIRY_CLASSES)
STRATA = SHAPE_STRATA + tuple(a for a, _ in ua.ARCHETYPE_RULES) + tuple(ua.ROSTERS)
# Every ticker the design's §2.4 census names as an example (forced into the panel when it is a
# development underlier present on the board).
EXAMPLE_TICKERS = (
    "SPY", "QQQ", "IWM", "GLD", "NVDA", "META", "MSFT", "AAPL", "SOXL", "MSTU", "CAPR", "MRNX",
    "EOSE", "LCID", "AMC", "NIO", "NVD", "UBRL", "PTIR", "TARK", "XRPN", "SPCQ", "MSTY", "PLTY",
    "AAPB", "FBL", "TSDD", "DIA", "SPX", "NDX", "RUT", "XSP", "OEX", "XEO", "TQQQ", "SQQQ", "SOXS",
    "UVXY", "TSM", "BABA", "ASML", "NVO", "SCHW", "PNC", "MTB", "BLK", "CZR", "XP", "TPR", "CSCO",
    "FDX", "SLAB", "SGOV", "SHY", "HYG", "BKNG", "CAR", "KLAC", "VGT", "WHR", "ZIM", "SONY",
    "NVAX", "REPL", "SKYD", "PEN", "IEF", "PGR", "NUGT", "ABVEF", "CMBMF", "GETY", "LESLQ", "MAPS",
    "MAXNQ", "NOTE", "REEAF", "SGMOQ", "SNBRQ",
)
DTE_BANDS = ((7, "<=7"), (30, "7-30"), (90, "30-90"), (math.inf, ">90"))  # oracle_scorecard
MONEYNESS_BANDS = ((0.8, "<0.8"), (0.95, "0.8-0.95"), (1.05, "0.95-1.05"), (1.2, "1.05-1.2"),
                   (math.inf, ">1.2"))
FIT_REFUSALS = ("invalid_input", "insufficient_quotes", "fit_failure", "arbitrage_failure",
                "calendar_arbitrage")
INVALID_STATUSES = ("invalid_market", "inconsistent_chain", "unsupported_deliverable",
                    "invalid_snapshot_spot", "carry_conflict", "stale_spot", "input_rejected")
# (mode, rule text). First match wins; the predicate of each mode is FAILURE_PREDICATES[mode].
FAILURE_MODES = (
    ("unsupported_exercise", "every row of the chain is input_status unsupported_exercise"),
    ("invalid_input", "no fit-eligible row, or the chain was refused for its input "
                      "(invalid_input / invalid_market / inconsistent_chain / unsupported "
                      "deliverable / snapshot-spot refusals)"),
    ("strike_ladder", "input: >= 10 % of eligible rows have K/S > 3 with spot >= 20"),
    ("insufficient_quotes", "refusal insufficient_quotes (graded) or no served row with "
                            "insufficient_quotes the dominant status"),
    ("calendar_arbitrage", "refusal calendar_arbitrage, or no served row with calendar_arbitrage "
                           "the dominant status"),
    ("arbitrage_failure", "refusal arbitrage_failure / certificate: a no-served chain whose "
                          "dominant status is arbitrage_failure or market_distance_failure"),
    ("fit_failure", "refusal fit_failure, or any other no-served chain"),
    ("outside_market", "served price containment < 0.95"),
    ("price_only_dominant", "> 50 % of served price rows are ok_price_only"),
    ("kIvMin_floor", "a served vol at the kIvMin floor (0.005)"),
    ("vendor_disagreement", "labels: vol RMSE > 300 bp while containment >= 0.99"),
    ("greek_refusal", "> 20 % of probe-evaluated rows not Ok (Refused / Unavailable)"),
    ("reprice_mismatch", "probe |dP| > max(1e-6 P, 1e-8) on a served row"),
    ("slow_fit", "chain wall > p99 of its stratum and > 1 s"),
    ("ok", "none of the above"),
)
FAILURE_ORDER = tuple(m for m, _ in FAILURE_MODES)


# ── small utilities ────────────────────────────────────────────────────────

def sha256(path: Path) -> str:
    return qb.sha256(Path(path))


def canonical_sha256(obj) -> str:
    return hashlib.sha256(json.dumps(obj, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True).encode()).hexdigest()


def hash_key(seed: str, name: str) -> tuple[int, str]:
    return (qb.stable_id([seed, name]), name)


def is_development(name: str) -> bool:
    return qb.split_for(name, 0.2, "atx-vol-v1") == "development"


def require_development(manifest: dict) -> int:
    """Labels open only for rows_by_split == {"development": n}; returns n."""
    split = manifest.get("rows_by_split")
    if (not isinstance(split, dict) or set(split) != {"development"}
            or not isinstance(split["development"], int) or split["development"] <= 0):
        raise PermissionError("labels open only for a development-split dataset "
                              "(rows_by_split == {development: n})")
    return split["development"]


def host_load() -> dict:
    """Best-effort host load sample: CPU % over 3 s and free RAM (psutil when present)."""
    try:
        import psutil  # noqa: PLC0415
        cpu = sum(psutil.cpu_percent(interval=1.0) for _ in range(3)) / 3.0
        return {"utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                "cpu_pct": round(cpu, 1),
                "free_mb": int(psutil.virtual_memory().available / 1048576)}
    except Exception as exc:  # noqa: BLE001 - load sampling never fails a run
        return {"utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                "error": str(exc)}


def fit_workers() -> int:
    raw = os.environ.get("ATX_VOL_FIT_WORKERS", "")
    workers = int(raw) if raw.isdigit() else 0
    if not 1 <= workers <= MAX_FIT_WORKERS:
        raise ValueError(f"set ATX_VOL_FIT_WORKERS to 1..{MAX_FIT_WORKERS} (lane rule)")
    return workers


def git_sha() -> str | None:
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=Path(__file__).resolve().parent,
                             capture_output=True, text=True, check=True, timeout=30)
        return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def iso_day(days: int) -> str:
    return (dt.date(1970, 1, 1) + dt.timedelta(days=int(days))).isoformat()


# ── process CPU + time limit around quote_benchmark.run_native ───────────────

class _FileTime(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint32), ("hi", ctypes.c_uint32)]


def process_cpu_seconds(proc: subprocess.Popen) -> float | None:
    """Kernel + user CPU of a finished child (Windows GetProcessTimes); None elsewhere."""
    if os.name != "nt":
        return None
    times = [_FileTime() for _ in range(4)]
    handle = ctypes.c_void_p(int(proc._handle))  # noqa: SLF001 - the child's own handle
    if not ctypes.windll.kernel32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
        return None
    return sum(((t.hi << 32) | t.lo) / 1e7 for t in times[2:])


def run_limited(command: list[str], *, stdout=None, text: bool = False,
                limit_s: float = STEP_LIMIT_S) -> tuple[subprocess.CompletedProcess, dict]:
    """subprocess.run with a hard wall limit and the child's CPU time."""
    started = time.perf_counter()
    with subprocess.Popen(command, stdout=stdout, text=text) as proc:
        try:
            out, _ = proc.communicate(timeout=limit_s)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            raise RuntimeError(f"step exceeded the {limit_s:.0f} s limit: {command[0]}") from None
        cpu = process_cpu_seconds(proc)
        code = proc.returncode
    return (subprocess.CompletedProcess(command, code, out, None),
            {"process_cpu_s": cpu, "process_wall_s": time.perf_counter() - started})


@contextlib.contextmanager
def measured_native(sink: list):
    """Route quote_benchmark's one subprocess.run through run_limited (CPU + 600 s guard)."""
    real = qb.subprocess.run

    def patched(command, check=False, stdout=None, text=False, **_kwargs):
        result, cost = run_limited(command, stdout=stdout, text=text)
        sink.append(cost)
        if check and result.returncode:
            raise subprocess.CalledProcessError(result.returncode, command, result.stdout)
        return result

    qb.subprocess.run = patched
    try:
        yield
    finally:
        qb.subprocess.run = real


# ── features and the panel ──────────────────────────────────────────────────

FEATURE_READ = tuple(dict.fromkeys(sc.FEATURE_COLUMNS + ua.FEATURE_COLUMNS))


def load_feature_quotes(dataset: Path) -> tuple[pl.DataFrame, dict]:
    """The quote columns features need, from a verified dataset; never opens labels.parquet."""
    manifest_path = Path(dataset) / "manifest.json"
    quotes_path = Path(dataset) / "quotes.parquet"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    digest = sha256(quotes_path)
    if manifest.get("files", {}).get("quotes.parquet", {}).get("sha256") != digest:
        raise ValueError(f"{dataset}: quotes.parquet does not match its manifest")
    names = set(pq.read_schema(quotes_path).names)
    if names & set(LABEL_COLUMNS):
        raise ValueError(f"{dataset}: quotes.parquet carries label columns")
    quotes = pl.read_parquet(quotes_path, columns=list(FEATURE_READ))
    quotes = quotes.filter(pl.col("underlier").is_not_null() & pl.col("expiry_days").is_not_null()
                           & pl.col("snapshot_ns").is_not_null())
    day = snapshot_day(quotes)
    quotes = quotes.with_columns((pl.col("expiry_days") - pl.lit(day).cast(pl.Int32))
                                 .cast(pl.Int32).alias("dte"))
    return quotes, {"dataset": str(Path(dataset).resolve()), "quotes_parquet_sha256": digest,
                    "manifest_sha256": sha256(manifest_path), "labels_read": False}


def snapshot_day(quotes: pl.DataFrame) -> dt.date:
    days = quotes.select(pl.col("snapshot_ns").cast(pl.Datetime("ns", "UTC"))
                         .dt.convert_time_zone("America/New_York").dt.date().unique())
    if days.height != 1:
        raise ValueError("one New York snapshot date per dataset is required")
    return days.item()


def build_features(quotes: pl.DataFrame) -> pl.DataFrame:
    """Per-underlier label-free features and every stratum boolean."""
    shape, parts = sc.classify("universe", quotes, None)
    per_exp = parts["per_expiry"].filter(pl.col("eligible_rows") > 0)
    short = set(per_exp.filter(pl.col("dte") <= 3)["underlier"].to_list())
    long_ = set(per_exp.filter(pl.col("dte") > 365)["underlier"].to_list())
    arche = ua.features(quotes)
    arche_map = {r["underlier"]: r for r in arche.to_dicts()}
    records = []
    for row in shape.to_dicts():
        name = row["underlier"]
        a = arche_map.get(name, {"underlier": name})
        strata = [c for c in SHAPE_STRATA[:len(sc.SYMBOL_CLASSES)] if row.get(c)]
        if row.get("classified"):
            strata += [c for c, members in (("short_dated", short), ("long_dated", long_))
                       if name in members]
            strata += ua.archetypes({**a, "underlier": name})
        records.append({"underlier": name, "classified": bool(row.get("classified")),
                        "eligible_rows": row.get("eligible_rows") or 0,
                        "spot": row.get("spot"), "straddle_vol": row.get("straddle_vol"),
                        "two_sided": a.get("two_sided"), "median_sdiv": a.get("median_sdiv"),
                        "dist_yield_1y": a.get("dist_yield_1y"), "front_ratio": a.get("front_ratio"),
                        "kink_ratio": a.get("kink_ratio"),
                        "far_strike_share": a.get("far_strike_share"),
                        "strata": ";".join(s for s in STRATA if s in strata)})
    return pl.DataFrame(records, infer_schema_length=None).sort("underlier")


def allocate(sizes: dict[str, int], total: int) -> dict[str, int]:
    """Equal shares with caps (water-filling): the largest integer level L with
    sum(min(size, L)) <= total, then the leftover one each, in STRATA order, to strata whose
    pool exceeds L (largest remainder among equal shares)."""
    live = [s for s in STRATA if sizes.get(s, 0) > 0]
    if not live or total <= 0:
        return {s: 0 for s in STRATA}
    level = 0
    hi = max(sizes[s] for s in live)
    while level < hi and sum(min(sizes[s], level + 1) for s in live) <= total:
        level += 1
    quotas = {s: min(sizes.get(s, 0), level) for s in STRATA}
    left = total - sum(quotas.values())
    for s in live:
        if left <= 0:
            break
        if sizes[s] > quotas[s]:
            quotas[s] += 1
            left -= 1
    return quotas


def draw_panel(features: pl.DataFrame, size: int = PANEL_SIZE, seed: str = PANEL_SEED,
               forced: tuple[str, ...] = EXAMPLE_TICKERS) -> dict:
    """Deterministic stratified draw over DEVELOPMENT underliers only."""
    pool = [r for r in features.to_dicts() if r["classified"] and is_development(r["underlier"])]
    members: dict[str, list[str]] = {s: [] for s in STRATA}
    strata_of: dict[str, list[str]] = {}
    for r in pool:
        names = [s for s in (r["strata"] or "").split(";") if s]
        strata_of[r["underlier"]] = names
        for s in names:
            members[s].append(r["underlier"])
    for s in STRATA:
        members[s].sort(key=lambda n: hash_key(seed, n))
    pool_names = {r["underlier"] for r in pool}
    chosen = [n for n in sorted(set(forced) & pool_names, key=lambda n: hash_key(seed, n))]
    chosen_set = set(chosen)
    quotas = allocate({s: len(v) for s, v in members.items()}, size - len(chosen))
    order = sorted((s for s in STRATA if members[s]), key=lambda s: (len(members[s]), s))
    for s in order:
        have = sum(1 for n in members[s] if n in chosen_set)
        for n in members[s]:
            if have >= quotas[s] or len(chosen) >= size:
                break
            if n not in chosen_set:
                chosen.append(n)
                chosen_set.add(n)
                have += 1
    cursor = {s: 0 for s in order}
    while len(chosen) < size:
        progressed = False
        for s in order:
            while cursor[s] < len(members[s]) and members[s][cursor[s]] in chosen_set:
                cursor[s] += 1
            if cursor[s] < len(members[s]) and len(chosen) < size:
                chosen.append(members[s][cursor[s]])
                chosen_set.add(members[s][cursor[s]])
                progressed = True
        if not progressed:
            break
    roster = sorted(chosen)
    per_stratum = {s: {"pool": len(members[s]), "quota": quotas[s],
                       "drawn": sum(1 for n in roster if s in strata_of.get(n, []))}
                   for s in STRATA}
    return {"roster": roster, "strata_of": {n: strata_of.get(n, []) for n in roster},
            "per_stratum": per_stratum, "forced_included": len(set(forced) & set(roster)),
            "pool_underliers": len(pool)}


def panel_rule() -> dict:
    return {"seed": PANEL_SEED, "size": PANEL_SIZE, "split": "split_for(u, 0.2, 'atx-vol-v1') "
            "== 'development'", "strata": list(STRATA),
            "shape_cohort_symbol_rules": {c: r for c, _l, _p, r in sc.SYMBOL_CLASSES},
            "shape_cohort_expiry_rules": {c: "uid has an eligible expiry with " + r
                                          for c, _l, _p, r in sc.EXPIRY_CLASSES},
            "archetype_rules": dict(ua.ARCHETYPE_RULES), "archetype_features": ua.FEATURE_DEFINITIONS,
            "rosters": {k: list(v) for k, v in ua.ROSTERS.items()},
            "forced_examples": list(EXAMPLE_TICKERS),
            "allocation": allocate.__doc__.split("\n")[0] + " " + allocate.__doc__.split("\n")[1],
            "selection": "forced examples first; strata rarest first take unchosen members in "
                         "stable_id([seed, u]) order until the stratum quota is covered; then "
                         "round-robin top-up to size"}


def cmd_panel(args: argparse.Namespace) -> dict:
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    quotes, info = load_feature_quotes(Path(args.features_dataset))
    features = build_features(quotes)
    dev = features.filter(pl.col("underlier").map_elements(is_development, return_dtype=pl.Boolean))
    draw = draw_panel(features)
    roster_path = out / "panel-roster.csv"
    pl.DataFrame({"underlier": draw["roster"],
                  "strata": [";".join(draw["strata_of"][n]) for n in draw["roster"]]}) \
        .write_csv(roster_path)
    dev.write_csv(out / "panel-features-development.csv")
    rule = panel_rule()
    manifest = {"schema": "atx-vol-universe-fit-panel", "schema_version": 1, "tool_sha256": TOOL_SHA256,
                "features_from": info, "rule": rule, "panel_rule_sha256": canonical_sha256(rule),
                "roster_file": roster_path.name, "panel_roster_sha256": sha256(roster_path),
                "roster_size": len(draw["roster"]), "per_stratum": draw["per_stratum"],
                "forced_included": draw["forced_included"], "pool_underliers": draw["pool_underliers"],
                "strata_non_empty": sum(1 for v in draw["per_stratum"].values() if v["drawn"] > 0)}
    qb.write_json(out / "panel.json", manifest)
    print(json.dumps({"roster": len(draw["roster"]), "strata_non_empty": manifest["strata_non_empty"],
                      "panel_rule_sha256": manifest["panel_rule_sha256"],
                      "panel_roster_sha256": manifest["panel_roster_sha256"]}))
    return manifest


# ── prepare: development datasets per bucket + probe labels ───────────────────

def vendor_greeks(source: Path, wanted: dict[str, np.ndarray]) -> dict[str, pl.DataFrame]:
    """One pass over the source's (de, ve) columns; rows are taken by global row index (the
    adapter's row_id is the source offset). Callers have already passed require_development."""
    every = np.unique(np.concatenate([v for v in wanted.values() if v.size])) if wanted else None
    taken_ids, taken_de, taken_ve = [], [], []
    offset = 0
    pf = pq.ParquetFile(source)
    for batch in pf.iter_batches(columns=["de", "ve"], batch_size=1 << 20):
        n = batch.num_rows
        lo, hi = np.searchsorted(every, [offset, offset + n])
        if hi > lo:
            idx = every[lo:hi] - offset
            taken_ids.append(every[lo:hi])
            taken_de.append(batch.column(0).to_numpy(zero_copy_only=False)[idx].astype(np.float64))
            taken_ve.append(batch.column(1).to_numpy(zero_copy_only=False)[idx].astype(np.float64))
        offset += n
    frame = pl.DataFrame({"row_id": np.concatenate(taken_ids).astype(np.uint64),
                          "vendor_delta": np.concatenate(taken_de),
                          "vendor_vega": np.concatenate(taken_ve)})
    return {k: frame.join(pl.DataFrame({"row_id": v.astype(np.uint64)}), on="row_id", how="semi")
            for k, v in wanted.items()}


def _read_labels(dataset: Path) -> pl.DataFrame:
    """THE label reader (instrumented by the tests): refuses non-development datasets."""
    manifest = json.loads((Path(dataset) / "manifest.json").read_text(encoding="utf-8"))
    require_development(manifest)
    return pl.read_parquet(Path(dataset) / "labels.parquet")


def cmd_prepare(args: argparse.Namespace) -> dict:
    roster = pl.read_csv(args.roster)["underlier"].to_list()
    if any(not is_development(n) for n in roster):
        raise PermissionError("the panel roster holds a non-development underlier")
    root = Path(args.output)
    buckets = [b for b in args.buckets.split(",") if b]
    log = []
    for b in buckets:
        out = root / b
        if out.exists():
            raise ValueError(f"{out} exists; prepare refuses to overwrite")
        before = host_load()
        started = time.perf_counter()
        code = qb.main(["prepare", "--source", str(args.source), "--output", str(out), "--split",
                        "development", "--symbols", ",".join(roster), "--buckets", b, "--csv"])
        if code:
            raise RuntimeError(f"quote_benchmark prepare failed for bucket {b}")
        log.append({"bucket": b, "seconds": time.perf_counter() - started,
                    "load_before": before, "load_after": host_load()})
    wanted = {}
    for b in buckets:
        manifest = json.loads((root / b / "manifest.json").read_text(encoding="utf-8"))
        require_development(manifest)
        wanted[b] = pl.read_parquet(root / b / "quotes.parquet", columns=["row_id"])["row_id"].to_numpy()
    started = time.perf_counter()
    greeks = vendor_greeks(Path(args.source), wanted)
    for b in buckets:
        labels = _read_labels(root / b).join(greeks[b], on="row_id", how="left")
        labels.select("row_id", "reference_price", "reference_vol", "vendor_delta", "vendor_vega") \
            .write_csv(root / b / "probe-labels.csv")
    result = {"buckets": log, "vendor_greek_seconds": time.perf_counter() - started}
    qb.write_json(root / f"prepare-log-{'-'.join(buckets)}.json", result)
    print(json.dumps({"buckets": len(log), "seconds": [round(x["seconds"], 1) for x in log]}))
    return result


# ── run: shards, native fit, probe ────────────────────────────────────────────

def plan_shards(rows_by_uid: dict[str, int], budget: int, seed: str = SHARD_SEED) -> dict:
    """Underlier -> shard by stable hash, with the fewest shards whose largest one holds at most
    `budget` rows; an underlier larger than the budget alone exceeds it (whole underliers stay
    together: one snapshot spot and the calendar checks span every root and expiry)."""
    if budget <= 0:
        raise ValueError("row budget must be positive")
    total = sum(rows_by_uid.values())
    names = sorted(rows_by_uid, key=lambda n: hash_key(seed, n))
    biggest = max(rows_by_uid.values(), default=0)
    count = max(1, math.ceil(total / budget))
    for _ in range(len(names) + 1):
        shard = {n: qb.stable_id([seed, n]) % count for n in names}
        sizes = Counter()
        for n, k in shard.items():
            sizes[k] += rows_by_uid[n]
        over = [k for k, v in sizes.items() if v > budget
                and not (v == biggest and biggest > budget
                         and sum(1 for n in names if shard[n] == k) == 1)]
        if not over or count >= len(names):
            return {"count": count, "shard_of": shard, "rows": [sizes[k] for k in range(count)],
                    "budget": budget, "oversize_underliers":
                        sorted(n for n, v in rows_by_uid.items() if v > budget)}
        count += 1
    raise RuntimeError("unreachable: shard planning did not converge")


def write_shard_dataset(parent: dict, frame: pl.DataFrame, path: Path, extra: dict) -> dict:
    path.mkdir(parents=True, exist_ok=False)
    table = frame.to_arrow().cast(qb.QUOTE_SCHEMA)
    writer = pq.ParquetWriter(path / "quotes.parquet", qb.QUOTE_SCHEMA, compression="zstd")
    writer.write_table(table)
    writer.close()
    csv = pacsv.CSVWriter(path / "quotes.csv", qb.QUOTE_SCHEMA)
    csv.write_table(table)
    csv.close()
    manifest = dict(parent)
    manifest["rows_selected"] = frame.height
    manifest["rows_by_input_status"] = dict(Counter(frame["input_status"].to_list()))
    manifest["rows_by_split"] = dict(Counter(frame["split"].to_list()))
    manifest["files"] = {name: {"sha256": sha256(path / name), "bytes": (path / name).stat().st_size}
                         for name in ("quotes.parquet", "quotes.csv")}
    manifest["shard"] = extra
    qb.write_json(path / "manifest.json", manifest)
    return manifest


def basis_from_native(native: dict, path: Path) -> int:
    rows = [(c["chain_id"], c["spot"]) for c in (native.get("one_spot") or {}).get("chains", [])
            if str(c.get("status", "")).startswith("rebased") and c.get("spot") is not None]
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("chain_id,spot\n")
        for chain, spot in rows:
            stream.write(f"{int(chain)},{float(spot)!r}\n")
    return len(rows)


def probe_shard(probe: Path, sdir: Path, kernel: str, workers: int, dividends: str,
                tolerance: float, basis: bool, rows: bool, manifest: Path | None) -> dict:
    """One atx-vol-fit-risk-probe call on a shard directory (600 s guard)."""
    cmd = [str(probe), "--quotes", str(sdir / "dataset" / "quotes.csv"),
           "--predictions", str(sdir / "predictions.csv"), "--out", str(sdir / "probe.json"),
           "--kernel", kernel, "--threads", str(workers), "--dividends", dividends,
           "--carry-tolerance", repr(tolerance)]
    if basis:
        cmd += ["--basis", str(sdir / "basis.csv")]
    if rows:
        cmd += ["--rows", str(sdir / "probe-rows.csv")]
    if manifest is not None:
        cmd += ["--labels", str(sdir / "probe-labels.csv"), "--manifest", str(manifest)]
    result, cost = run_limited(cmd, stdout=subprocess.PIPE, text=True)
    if result.returncode:
        raise RuntimeError(f"probe failed on {sdir} (exit {result.returncode})")
    return {**cost, "stdout": json.loads(result.stdout), "path": str((sdir / "probe.json").resolve()),
            "probe_sha256": sha256(probe)}


def cmd_reprobe(args: argparse.Namespace) -> dict:
    """Re-run only the probe on every shard of a finished run (a fixed probe binary; the fit
    outputs are untouched). The previous probe.json is kept as probe-<old sha8>.json."""
    workers = fit_workers()
    run_dir = Path(args.run)
    run = load_run(run_dir)
    dataset = Path(run["dataset"])
    manifest = json.loads((dataset / "manifest.json").read_text(encoding="utf-8"))
    for shard in run["shards"]:
        sdir = Path(shard["dataset"]).parent
        old = sdir / "probe.json"
        if old.exists():
            old.replace(sdir / f"probe-{run['probe_sha256'][:8]}.json")
        labelled = (sdir / "probe-labels.csv").exists()
        if labelled:
            require_development(manifest)
        before = host_load()
        shard["probe"] = probe_shard(Path(args.probe), sdir, run["kernel"], workers,
                                     run["dividends"], run["carry_price_tolerance"],
                                     (sdir / "basis.csv").stat().st_size > len("chain_id,spot\n"),
                                     args.probe_rows or (sdir / "probe-rows.csv").exists(),
                                     dataset / "manifest.json" if labelled else None)
        shard["probe"]["load_before"] = before
        shard["probe"]["load_after"] = host_load()
        print(json.dumps({"shard": shard["index"], "probe_wall_s":
                          round(shard["probe"]["process_wall_s"], 1)}), flush=True)
    run["probe"] = str(Path(args.probe).resolve())
    run["probe_sha256"] = sha256(Path(args.probe))
    run.setdefault("reprobed", []).append(dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"))
    qb.write_json(run_dir / "run.json", run)
    return run


def cmd_run(args: argparse.Namespace) -> dict:
    workers = fit_workers()
    dataset = Path(args.dataset)
    parent = qb.verify_dataset(dataset)
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    quotes = pl.read_parquet(dataset / "quotes.parquet")
    key = quotes["underlier"].fill_null("")  # an unnamed row still lands in exactly one shard
    rows_by_uid = dict(key.value_counts().iter_rows())
    plan = plan_shards(rows_by_uid, args.rows_per_shard)
    quotes = quotes.with_columns(key.replace_strict(plan["shard_of"], return_dtype=pl.Int64)
                                 .alias("_shard"))
    labels = None
    if args.labels:
        require_development(parent)
        labels = pl.read_csv(args.labels, schema_overrides={"row_id": pl.UInt64})
    policy_doc = None
    if args.policy != "auto":
        policy_doc = json.loads(Path(args.policy).read_text(encoding="utf-8"))
    tolerance = float(((policy_doc or {}).get("snapshot") or {}).get("carry_price_tolerance", 0.0))
    shards = []
    for k in range(plan["count"]):
        sub = quotes.filter(pl.col("_shard") == k).drop("_shard").sort("row_id")
        if sub.is_empty():
            continue
        sdir = out / f"shard-{k:02d}"
        write_shard_dataset(parent, sub, sdir / "dataset",
                            {"index": k, "count": plan["count"], "seed": SHARD_SEED,
                             "rows_per_shard": args.rows_per_shard,
                             "parent_manifest_sha256": sha256(dataset / "manifest.json")})
        record = {"index": k, "rows": sub.height, "dataset": str((sdir / "dataset").resolve()),
                  "load_before": host_load()}
        cost: list = []
        with measured_native(cost):
            receipt = qb.run_native(argparse.Namespace(
                dataset=sdir / "dataset", binary=Path(args.binary), output=sdir / "predictions.csv",
                model="quote-slice", policy=args.policy, curve=None, common_policy=None,
                progress=None, dividends=args.dividends,
                extra=["--", "--report", str(sdir / "native.json"), "--timing-detail"]))
        record["fit"] = cost[-1] if cost else None
        record["receipt"] = str((sdir / "predictions.csv.run.json").resolve())
        native = receipt["native_report"]
        n_basis = basis_from_native(native, sdir / "basis.csv")
        if labels is not None:
            labels.join(sub.select(pl.col("row_id").cast(pl.UInt64)), on="row_id", how="semi") \
                .write_csv(sdir / "probe-labels.csv")
        record["probe"] = probe_shard(Path(args.probe), sdir, args.kernel, workers, args.dividends,
                                      tolerance, n_basis > 0, args.probe_rows,
                                      dataset / "manifest.json" if labels is not None else None)
        probe_cost = record["probe"]
        record["load_after"] = host_load()
        shards.append(record)
        print(json.dumps({"shard": k, "rows": sub.height,
                          "fit_wall_s": round(record["fit"]["process_wall_s"], 1),
                          "probe_wall_s": round(probe_cost["process_wall_s"], 1)}), flush=True)
    run = {"schema": RUN_SCHEMA, "schema_version": 1, "tool_sha256": TOOL_SHA256,
           "dataset": str(dataset.resolve()), "dataset_manifest_sha256": sha256(dataset / "manifest.json"),
           "policy": args.policy, "policy_sha256": sha256(Path(args.policy)) if args.policy != "auto" else None,
           "dividends": args.dividends, "binary": str(Path(args.binary).resolve()),
           "binary_sha256": sha256(Path(args.binary)), "probe": str(Path(args.probe).resolve()),
           "probe_sha256": sha256(Path(args.probe)), "kernel": args.kernel, "fit_workers": workers,
           "carry_price_tolerance": tolerance, "plan": {k: v for k, v in plan.items() if k != "shard_of"},
           "shards": shards, "timing_qualified": False,
           "host": {"cpu": platform.processor(), "fit_workers": workers}}
    qb.write_json(out / "run.json", run)
    return run


# ── score: chain scorecards ─────────────────────────────────────────────────

def band_expr(column: str, bands) -> pl.Expr:
    """The band label of a numeric column (first edge it does not exceed); null when non-finite."""
    value = pl.col(column).cast(pl.Float64)
    expr = pl.when(value.is_null() | ~value.is_finite()).then(pl.lit(None, pl.String))
    for edge, name in bands:
        expr = expr.when(value <= edge).then(pl.lit(name))
    return expr.otherwise(pl.lit(None, pl.String))


def row_frame(quotes: pl.DataFrame, predictions: pl.DataFrame, labels: pl.DataFrame | None,
              day: dt.date) -> pl.DataFrame:
    """Every quote row with its prediction (and labels for development data) and the row
    predicates every chain metric is a sum of."""
    joined = quotes.join(predictions, on="row_id", how="left", validate="1:1")
    if labels is not None:
        joined = joined.join(labels.select("row_id", "reference_price", "reference_vol"),
                             on="row_id", how="left", validate="1:1")
    else:
        joined = joined.with_columns(pl.lit(None, pl.Float64).alias("reference_price"),
                                     pl.lit(None, pl.Float64).alias("reference_vol"))
    finite = lambda c: pl.col(c).is_not_null() & pl.col(c).is_finite()  # noqa: E731
    mid = (pl.col("bid") + pl.col("ask")) * 0.5
    spread = (pl.col("ask") - pl.col("bid")).clip(lower_bound=TICK)
    j = joined.with_columns(
        (pl.col("input_status") == "ok").alias("eligible"),
        pl.col("status").fill_null("absent").alias("status"),
    ).with_columns(
        (pl.col("eligible") & pl.col("status").is_in(SERVED) & finite("predicted_price")
         & (pl.col("predicted_price") >= 0)).alias("served_price"),
        (pl.col("eligible") & (pl.col("status") == "ok") & finite("predicted_vol")
         & (pl.col("predicted_vol") > 0)).alias("served_vol"),
        (pl.col("eligible") & (pl.col("status") == "ok_price_only")).alias("price_only"),
        (pl.col("eligible") & (pl.col("bid") > 0) & (pl.col("ask") > 0)).alias("two_sided"),
        (pl.col("eligible") & ~(pl.col("bid") > 0) & (pl.col("ask") > 0)).alias("ask_only"),
        (pl.col("eligible") & (pl.col("strike") / pl.col("spot") > 3.0)
         & (pl.col("spot") >= 20)).alias("far_strike"),
        mid.alias("mid"), spread.alias("spread"),
        (pl.col("expiry_days") - pl.lit(day).cast(pl.Int32)).alias("dte"),
        (pl.col("strike") / pl.col("spot")).alias("moneyness"),
    ).with_columns(
        (pl.col("served_price") & (pl.col("predicted_price") >= pl.col("bid"))
         & (pl.col("predicted_price") <= pl.col("ask"))).alias("inside"),
        pl.when(pl.col("served_price")).then((pl.col("predicted_price") - pl.col("mid")).abs()
                                             / pl.col("spread")).alias("mid_err_spreads"),
        pl.when(pl.col("served_price")).then(
            pl.max_horizontal(pl.col("bid") - pl.col("predicted_price"),
                              pl.col("predicted_price") - pl.col("ask"), pl.lit(0.0))
            / pl.col("spread")).alias("outside_spreads"),
        (pl.col("served_vol") & (pl.col("predicted_vol") <= KIVMIN + 1e-12)).alias("kivmin"),
        pl.when(pl.col("served_vol") & finite("reference_vol") & (pl.col("reference_vol") > 0))
          .then((pl.col("predicted_vol") - pl.col("reference_vol")) * 1e4).alias("vol_err_bp"),
        pl.when(pl.col("served_price") & finite("reference_price") & (pl.col("reference_price") >= 0))
          .then(pl.col("predicted_price") - pl.col("reference_price")).alias("price_err"),
        pl.when(pl.col("eligible") & finite("reference_price") & (pl.col("reference_price") >= 0))
          .then(pl.col("reference_price").is_between(pl.col("bid"), pl.col("ask")).cast(pl.Float64))
          .alias("ref_inside"),
    ).with_columns(
        (pl.col("price_err").abs() / pl.col("spread")).alias("price_err_spreads"),
        band_expr("dte", DTE_BANDS).alias("dte_band"),
        band_expr("moneyness", MONEYNESS_BANDS).alias("moneyness_band"),
    )
    return j


def chain_shape_breaks(frame: pl.DataFrame) -> pl.DataFrame:
    """quote_benchmark._shape_breaks VERBATIM on a frame whose `underlier` is the composite
    underlier + US + root (so a break names its root), attributed to chains: local kinds to
    their own expiry's chain, a calendar break to its EARLIER expiry's chain."""
    keyed = frame.with_columns((pl.col("underlier") + "\x1f" + pl.col("root")).alias("underlier"))
    breaks = qb._shape_breaks(keyed.lazy())  # noqa: SLF001 - reuse, never re-implement
    chains = keyed.select("underlier", "expiry_days", "chain_id").unique(["underlier", "expiry_days"])
    return breaks.join(chains, on=["underlier", "expiry_days"], how="left")


def _quantile(values: list[float], q: float) -> float | None:
    clean = sorted(v for v in values if v is not None and math.isfinite(v))
    if not clean:
        return None
    return clean[min(len(clean) - 1, max(0, round(q * (len(clean) - 1))))]


def _rmse(values: list[float]) -> float | None:
    return math.sqrt(sum(v * v for v in values) / len(values)) if values else None


def _mean(values) -> float | None:
    vals = [v for v in values if v is not None and math.isfinite(v)]
    return sum(vals) / len(vals) if vals else None


def chain_cards(rows: pl.DataFrame, breaks: pl.DataFrame, slices: dict, probe: dict,
                archetypes: dict, bucket: str, labelled: bool) -> list[dict]:
    """One scorecard per chain_id of `rows` (a single-bucket row frame)."""
    by_kind = defaultdict(Counter)
    for chain, kind in breaks.select("chain_id", "kind").iter_rows():
        by_kind[chain][kind] += 1
    agg = rows.group_by("chain_id", maintain_order=True).agg(
        pl.col("underlier").first(), pl.col("root").first(), pl.col("expiry_days").first(),
        pl.col("dte").first(), pl.col("years").median().alias("variance_years"),
        pl.len().alias("rows"), pl.col("eligible").sum(), pl.col("two_sided").sum(),
        pl.col("ask_only").sum(), pl.col("served_price").sum(), pl.col("served_vol").sum(),
        pl.col("price_only").sum(), pl.col("inside").sum(), pl.col("kivmin").sum(),
        pl.col("far_strike").sum(),
        (pl.col("input_status") == "unsupported_exercise").sum().alias("unsupported_rows"),
        pl.col("mid_err_spreads").mean().alias("mid_err_spreads_mae"),
        pl.col("outside_spreads").max().alias("max_outside_spreads"),
        pl.col("status").value_counts(sort=True).alias("status_counts"),
        (pl.col("admitted").cast(pl.Int64, strict=False).sum() if "admitted" in rows.columns
         else pl.lit(None, pl.Int64)).alias("admitted"),
        pl.col("vol_err_bp").drop_nulls().alias("vol_err"),
        pl.col("price_err").drop_nulls().abs().mean().alias("price_mae"),
        pl.col("price_err_spreads").drop_nulls().mean().alias("price_in_spreads_mae"),
        pl.col("ref_inside").drop_nulls().mean().alias("ref_containment"),
    )
    cards = []
    for r in agg.to_dicts():
        chain = int(r["chain_id"])
        statuses = {d["status"]: int(d["count"]) for d in r["status_counts"]}
        served = r["served_price"]
        s = slices.get(chain)
        dominant = max(((n, k) for k, n in statuses.items() if k not in SERVED),
                       default=(0, None))[1]
        fit = {"grade": s.get("grade") if s else None,
               "refusal": (s.get("refusal") if s and s.get("refusal") else
                           ("" if served else (dominant or ""))),
               "error": (s.get("error") if s else None) or None,
               "point_status": s.get("point_status") if s else None,
               "method": s.get("profile") if s else None,
               "calendar_gap": s.get("calendar_gap") if s else None,
               "calendar_gap_strike": s.get("calendar_gap_strike") if s else None,
               "outside_fraction": s.get("outside_fraction") if s else None,
               "wall_us": ((s.get("timing") or {}).get("wall_us") if s else None)}
        errs = list(r["vol_err"] or [])
        lab = None
        if labelled:
            lab = {"n": len(errs), "vol_rmse_bp": _rmse(errs), "vol_mae_bp": _mean(abs(e) for e in errs),
                   "vol_bias_bp": _mean(errs), "price_mae": r["price_mae"],
                   "price_in_spreads_mae": r["price_in_spreads_mae"],
                   "ref_containment": r["ref_containment"]}
        g = probe.get(chain)
        cards.append({
            "chain_id": chain, "uid": r["underlier"], "root": r["root"], "bucket_et": bucket,
            "expiry": iso_day(r["expiry_days"]), "dte_cal": int(r["dte"]),
            "variance_years": r["variance_years"], "archetypes": archetypes.get(r["underlier"], []),
            "rows": r["rows"], "eligible": r["eligible"], "two_sided": r["two_sided"],
            "ask_only": r["ask_only"], "admitted": r["admitted"], "status_counts": statuses,
            "fit": fit, "served_price_rows": served, "served_vol_rows": r["served_vol"],
            "price_only_rows": r["price_only"],
            "containment": (r["inside"] / served) if served else None,
            "mid_err_spreads_mae": r["mid_err_spreads_mae"],
            "max_outside_spreads": r["max_outside_spreads"],
            "shape": {k: by_kind[chain].get(k, 0) for k in ("monotone", "convex", "calendar",
                                                            "duplicate")},
            "api_violations": None, "labels": lab, "greeks": greek_block(g),
            "kivmin_rows": r["kivmin"], "far_strike_rows": r["far_strike"],
            "unsupported_rows": r["unsupported_rows"],
            "failure_mode": None, "secondary": archetypes.get(r["underlier"], []),
        })
    return cards


def greek_block(chain: dict | None) -> dict | None:
    """The probe's per-chain counts in the report's greeks shape (None without a probe)."""
    if chain is None:
        return None
    c = chain["counts"]
    routes = c["routes"]
    vendor = c["vendor"]
    return {"evaluated": c["state"]["evaluated"],
            "price_only": c["state"]["price_only"], "invalid_input": c["state"]["invalid_input"],
            "routes": {k: routes[k] for k in ("family", "cold_analytic", "cold_fd", "exercised",
                                              "refused")},
            "flags": dict(c["flags"]), "availability": dict(c["lane"]),
            "nonfinite_greek": c["nonfinite_greek"], "sanity_fail": dict(c["sanity_fail"]),
            "reprice_max_abs": c["reprice"]["max_abs"], "reprice_max_rel": c["reprice"]["max_rel"],
            "reprice_max_vol_bp": c["reprice"]["max_vol_bp"],
            "reprice_mismatch": c["reprice"]["mismatch"], "basis": chain.get("basis"),
            "vendor": ({"n": vendor["n"], "delta_mae": vendor["delta_mae"],
                        "vega_sym_rel": vendor["vega_sym_rel"]} if vendor["n"] else None)}


# ── failure taxonomy ────────────────────────────────────────────────────────

def _no_served(c: dict) -> bool:
    return not c["served_price_rows"]


def _dominant(c: dict) -> str | None:
    rest = {k: v for k, v in c["status_counts"].items() if k not in SERVED}
    return max(rest.items(), key=lambda kv: (kv[1], kv[0]))[0] if rest else None


def _greek_refusal(c: dict) -> bool:
    g = c.get("greeks")
    if not g or not g["evaluated"]:
        return False
    return (g["evaluated"] - g["availability"].get("ok", 0)) / g["evaluated"] > 0.20


FAILURE_PREDICATES = {
    "unsupported_exercise": lambda c, x: c["rows"] > 0 and c["unsupported_rows"] == c["rows"],
    "invalid_input": lambda c, x: (c["eligible"] == 0 or c["fit"]["refusal"] == "invalid_input"
                                   or (_no_served(c) and _dominant(c) in INVALID_STATUSES)),
    "strike_ladder": lambda c, x: c["eligible"] > 0 and c["far_strike_rows"] / c["eligible"] >= 0.10,
    "insufficient_quotes": lambda c, x: (c["fit"]["refusal"] == "insufficient_quotes"
                                         or (_no_served(c) and _dominant(c) == "insufficient_quotes")),
    "calendar_arbitrage": lambda c, x: (c["fit"]["refusal"] == "calendar_arbitrage"
                                        or (_no_served(c) and _dominant(c) == "calendar_arbitrage")),
    "arbitrage_failure": lambda c, x: (c["fit"]["refusal"] == "arbitrage_failure"
                                       or (_no_served(c) and _dominant(c) in
                                           ("arbitrage_failure", "market_distance_failure"))),
    "fit_failure": lambda c, x: c["fit"]["refusal"] == "fit_failure" or _no_served(c),
    "outside_market": lambda c, x: c["containment"] is not None and c["containment"] < 0.95,
    "price_only_dominant": lambda c, x: (c["served_price_rows"] > 0
                                         and c["price_only_rows"] / c["served_price_rows"] > 0.5),
    "kIvMin_floor": lambda c, x: c["kivmin_rows"] > 0,
    "vendor_disagreement": lambda c, x: (c["labels"] is not None
                                         and c["labels"]["vol_rmse_bp"] is not None
                                         and c["labels"]["vol_rmse_bp"] > 300
                                         and (c["containment"] or 0) >= 0.99),
    "greek_refusal": lambda c, x: _greek_refusal(c),
    "reprice_mismatch": lambda c, x: bool(c.get("greeks") and c["greeks"]["reprice_mismatch"]),
    "slow_fit": lambda c, x: (c["fit"]["wall_us"] is not None and c["fit"]["wall_us"] > 1e6
                              and c["fit"]["wall_us"] > x["p99"].get(stratum_of(c), math.inf)),
    "ok": lambda c, x: True,
}


def stratum_of(card: dict) -> str:
    return card["archetypes"][0] if card["archetypes"] else "none"


def classify_failures(cards: list[dict]) -> None:
    """Assign exactly one primary mode per chain (first matching rule in FAILURE_ORDER)."""
    walls = defaultdict(list)
    for c in cards:
        if c["fit"]["wall_us"] is not None:
            walls[stratum_of(c)].append(float(c["fit"]["wall_us"]))
    context = {"p99": {k: _quantile(v, 0.99) for k, v in walls.items()}}
    for c in cards:
        c["failure_mode"] = next(m for m in FAILURE_ORDER if FAILURE_PREDICATES[m](c, context))


# ── aggregates, worst lists, report ─────────────────────────────────────────

def metrics(rows: pl.DataFrame, cards: list[dict], labelled: bool) -> dict:
    """METRICS over a row frame and the scorecards of the same chains."""
    r = rows.select(
        pl.len().alias("rows"), pl.col("eligible").sum().alias("eligible_rows"),
        pl.col("served_price").sum().alias("served_price_rows"),
        pl.col("served_vol").sum().alias("served_vol_rows"), pl.col("inside").sum().alias("inside"),
        pl.col("mid_err_spreads").mean().alias("mae"),
        pl.col("mid_err_spreads").quantile(0.95).alias("p95"),
        pl.col("price_only").sum().alias("price_only_rows"), pl.col("kivmin").sum().alias("kivmin_rows"),
        pl.col("vol_err_bp").drop_nulls().implode().alias("vol_err"),
        pl.col("price_err").abs().mean().alias("price_mae"),
        pl.col("price_err_spreads").mean().alias("price_in_spreads_mae"),
    ).to_dicts()[0]
    refusals = Counter()
    for c in cards:
        for k, v in c["status_counts"].items():
            if k not in SERVED:
                refusals[k] += v
    walls = [float(c["fit"]["wall_us"]) for c in cards if c["fit"]["wall_us"] is not None]
    cpu = sum(walls) / 1e6 if walls else None
    eligible = r["eligible_rows"]
    out = {"rows": r["rows"], "eligible_rows": eligible, "chains": len(cards),
           "price_coverage": r["served_price_rows"] / eligible if eligible else None,
           "vol_coverage": r["served_vol_rows"] / eligible if eligible else None,
           "containment": r["inside"] / r["served_price_rows"] if r["served_price_rows"] else None,
           "mid_err_spreads_mae": r["mae"], "mid_err_spreads_p95": r["p95"],
           **{f"shape_{k}": sum(c["shape"][k] for c in cards)
              for k in ("monotone", "convex", "calendar", "duplicate")},
           "refusals": dict(sorted(refusals.items())), "price_only_rows": r["price_only_rows"],
           "kivmin_rows": r["kivmin_rows"], "fit_cpu_s": cpu,
           "rows_per_cpu_s": (r["rows"] / cpu) if cpu else None,
           "chain_wall_us_p50": _quantile(walls, 0.5), "chain_wall_us_p99": _quantile(walls, 0.99)}
    if labelled:
        errs = list(r["vol_err"] or [])
        out.update({"vol_bp_bias": _mean(errs), "vol_bp_mae": _mean(abs(e) for e in errs),
                    "vol_bp_rmse": _rmse(errs), "vol_bp_p95": _quantile([abs(e) for e in errs], 0.95),
                    "price_mae": r["price_mae"], "price_in_spreads_mae": r["price_in_spreads_mae"]})
    greeks = [c["greeks"] for c in cards if c.get("greeks")]
    out["greeks_evaluated"] = sum(g["evaluated"] for g in greeks)
    out["greeks_refused"] = sum(g["evaluated"] - g["availability"].get("ok", 0) for g in greeks)
    out["greeks_sanity_fail"] = sum(sum(g["sanity_fail"].values()) for g in greeks)
    out["reprice_mismatch_rows"] = sum(g["reprice_mismatch"] for g in greeks)
    out["reprice_max_abs"] = max((g["reprice_max_abs"] or 0.0 for g in greeks), default=None)
    return out


def chain_ref(c: dict, value) -> dict:
    return {"uid": c["uid"], "root": c["root"], "bucket_et": c["bucket_et"], "expiry": c["expiry"],
            "chain_id": c["chain_id"], "value": value}


def chain_key(c: dict) -> tuple:
    return (c["uid"], c["root"], c["bucket_et"], c["expiry"], c["chain_id"])


def worst(cards: list[dict], value, descending: bool = True, n: int = 50) -> list[dict]:
    """Top-n by `value` (None excluded); ties broken by the chain key, so the ranking is a
    function of the chains, never of their input order."""
    scored = [(value(c), c) for c in cards]
    scored = [(v, c) for v, c in scored if v is not None and (not isinstance(v, float) or math.isfinite(v))]
    scored.sort(key=lambda vc: ((-vc[0] if descending else vc[0]), chain_key(vc[1])))
    return [chain_ref(c, v) for v, c in scored[:n]]


def worst_lists(cards: list[dict], n: int = 50) -> dict:
    g = lambda c: c.get("greeks") or {}  # noqa: E731
    return {
        "containment": worst(cards, lambda c: c["containment"], descending=False, n=n),
        "refused_rows": worst(cards, lambda c: (c["eligible"] - c["served_price_rows"]) or None, n=n),
        "vol_rmse_bp": worst(cards, lambda c: (c["labels"] or {}).get("vol_rmse_bp"), n=n),
        "greek_refusal": worst(cards, lambda c: ((g(c).get("evaluated", 0)
                                                  - g(c).get("availability", {}).get("ok", 0))
                                                 or None), n=n),
        "reprice_mismatch": worst(cards, lambda c: g(c).get("reprice_max_abs") or None, n=n),
        "fit_wall_us": worst(cards, lambda c: c["fit"]["wall_us"], n=n),
    }


def failure_table(cards: list[dict]) -> list[dict]:
    out = []
    rules = dict(FAILURE_MODES)
    for mode in FAILURE_ORDER:
        mine = [c for c in cards if c["failure_mode"] == mode]
        by_arch = Counter()
        for c in mine:
            for a in (c["archetypes"] or ["none"]):
                by_arch[a] += 1
        out.append({"mode": mode, "rule": rules[mode], "chains": len(mine),
                    "rows": sum(c["rows"] for c in mine), "uids": len({c["uid"] for c in mine}),
                    "by_archetype": dict(sorted(by_arch.items())),
                    "examples": [chain_ref(c, None) for c in sorted(mine, key=chain_key)[:10]]})
    return out


# Chain-level fields (a chain's shape breaks, probe counts and fit wall) that a row-level band
# such as moneyness cannot split; they are null there and live in the chain cards.
CHAIN_LEVEL_FIELDS = ("shape_monotone", "shape_convex", "shape_calendar", "shape_duplicate",
                      "fit_cpu_s", "rows_per_cpu_s", "chain_wall_us_p50", "chain_wall_us_p99",
                      "greeks_evaluated", "greeks_refused", "greeks_sanity_fail",
                      "reprice_mismatch_rows", "reprice_max_abs")


def group_metrics(rows: pl.DataFrame, cards: list[dict], key: str, labelled: bool,
                  row_level: bool = False) -> list[dict]:
    """Metrics per band. A chain-level band (DTE: one expiry per chain) takes whole chains; a
    row-level band (moneyness) takes its rows: refusals are recounted from the rows' status and the
    chain-level fields are null."""
    out = []
    for value in sorted(v for v in rows[key].unique().to_list() if v is not None):
        sub = rows.filter(pl.col(key) == value)
        chains = set(zip(sub["bucket_et"].to_list(), sub["chain_id"].to_list()))
        mine = [c for c in cards if (c["bucket_et"], c["chain_id"]) in chains]
        m = metrics(sub, mine, labelled)
        if row_level:
            m.update({k: None for k in CHAIN_LEVEL_FIELDS})
            counts = sub.filter(~pl.col("status").is_in(list(SERVED)))["status"].value_counts()
            m["refusals"] = dict(sorted((str(r["status"]), int(r["count"]))
                                        for r in counts.to_dicts()))
        out.append({key: value, **m})
    return out


def archetype_metrics(rows: pl.DataFrame, cards: list[dict], labelled: bool) -> list[dict]:
    """A chain (and its rows) counts in every archetype of its uid; "none" when it has none."""
    pairs = [(c["bucket_et"], c["chain_id"], a) for c in cards for a in (c["archetypes"] or ["none"])]
    member = pl.DataFrame(pairs, schema={"bucket_et": pl.String, "chain_id": pl.UInt64,
                                         "archetype": pl.String}, orient="row")
    joined = rows.join(member, on=["bucket_et", "chain_id"], how="inner")
    out = []
    for a in sorted(member["archetype"].unique().to_list()):
        mine = [c for c in cards if a in (c["archetypes"] or ["none"])]
        out.append({"archetype": a, "n_uids": len({c["uid"] for c in mine}),
                    **metrics(joined.filter(pl.col("archetype") == a), mine, labelled)})
    return out


def load_run(run_dir: Path) -> dict:
    run = json.loads((Path(run_dir) / "run.json").read_text(encoding="utf-8"))
    if run.get("schema") != RUN_SCHEMA:
        raise ValueError(f"{run_dir}: not a {RUN_SCHEMA} directory")
    return run


PREDICTION_EXTRAS = ("admitted", "vol_convention", "grade")


def read_predictions(path: Path) -> pl.DataFrame:
    """quote_benchmark.scan_predictions (its validation) plus the optional native columns the
    scorecards report (admitted, vol_convention, grade) when the artifact has them."""
    core = qb.scan_predictions(path).select(
        "row_id", "predicted_price", "predicted_vol", "status",
        pl.col("chain_id").alias("prediction_chain_id")).collect()
    with path.open("r", encoding="utf-8") as stream:
        header = stream.readline().strip().split(",")
    extras = [c for c in PREDICTION_EXTRAS if c in header]
    if extras:
        more = pl.read_csv(path, columns=["row_id", *extras], infer_schema_length=0)
        more = more.with_columns(pl.col("row_id").cast(pl.UInt64))
        core = core.join(more, on="row_id", how="left", validate="1:1")
    return core


def score_bucket(dataset: Path, run_dir: Path, tier: str, archetypes: dict) -> tuple[pl.DataFrame, list[dict], dict]:
    manifest = qb.verify_dataset(Path(dataset))
    run = load_run(run_dir)
    buckets = manifest["selection"]["buckets_new_york"]
    if len(buckets) != 1:
        raise ValueError(f"{dataset}: one bucket per dataset")
    bucket = buckets[0]
    labelled = tier == "labelled-panel"
    labels = _read_labels(Path(dataset)) if labelled else None
    quotes = pl.read_parquet(Path(dataset) / "quotes.parquet")
    preds, slices, probe, receipts = [], {}, {}, []
    for shard in run["shards"]:
        receipt = json.loads(Path(shard["receipt"]).read_text(encoding="utf-8"))
        receipts.append(receipt)
        preds.append(read_predictions(Path(receipt["predictions"])))
        for s in ((receipt["native_report"].get("graded_stats") or {}).get("slices") or []):
            slices[int(s["chain_id"])] = s
        probe_path = (shard.get("probe") or {}).get("path")
        if probe_path:
            for c in json.loads(Path(probe_path).read_text(encoding="utf-8"))["chains"]:
                probe[int(c["chain_id"])] = c
    predictions = pl.concat(preds, how="diagonal_relaxed")
    clash = quotes.select("row_id", "chain_id").join(predictions.select(
        "row_id", "prediction_chain_id"), on="row_id", how="inner").filter(
        pl.col("chain_id") != pl.col("prediction_chain_id")).height
    if clash:
        raise ValueError(f"{clash} predictions disagree with their quote row's chain")
    predictions = predictions.drop("prediction_chain_id")
    day = snapshot_day(quotes)
    rows = row_frame(quotes, predictions, labels, day).with_columns(pl.lit(bucket).alias("bucket_et"))
    breaks = chain_shape_breaks(rows)
    cards = chain_cards(rows, breaks, slices, probe, archetypes, bucket, labelled)
    info = {"bucket": bucket, "manifest": manifest, "run": run, "receipts": receipts,
            "date": day.isoformat(), "labels_opened": labels is not None}
    return rows, cards, info


def archetype_table(path: Path | None, dataset: Path | None) -> dict:
    """uid -> strata list: from a panel roster / features CSV, or computed label-free."""
    if path is not None:
        frame = pl.read_csv(path, infer_schema_length=0)
    else:
        quotes, _ = load_feature_quotes(Path(dataset))
        frame = build_features(quotes)
    return {r["underlier"]: [s for s in (r["strata"] or "").split(";") if s]
            for r in frame.select("underlier", "strata").to_dicts()}


def cmd_score(args: argparse.Namespace) -> dict:
    tier = args.tier
    pairs = [p.split("=", 1) for p in args.pair]
    archetypes = archetype_table(Path(args.archetypes) if args.archetypes else None,
                                 Path(pairs[0][0]))
    all_rows, all_cards, infos = [], [], []
    for dataset, run_dir in pairs:
        manifest = json.loads((Path(dataset) / "manifest.json").read_text(encoding="utf-8"))
        if tier == "labelled-panel":
            require_development(manifest)
        elif set(manifest.get("rows_by_split", {})) - {"universe", "development"}:
            raise PermissionError("label-free tier: unexpected split in manifest")
        rows, cards, info = score_bucket(Path(dataset), Path(run_dir), tier, archetypes)
        all_rows.append(rows.select(SCORE_ROW_COLUMNS))
        all_cards.extend(cards)
        infos.append(info)
    rows = pl.concat(all_rows, how="vertical_relaxed")
    classify_failures(all_cards)
    report = assemble_report(rows, all_cards, infos, tier, args)
    write_report(report, Path(args.output))
    print(json.dumps({"chains": len(all_cards), "rows": report["totals"]["rows"],
                      "failure_modes": {m["mode"]: m["chains"] for m in report["failure_modes"]
                                        if m["chains"]}}))
    return report


SCORE_ROW_COLUMNS = ("bucket_et", "chain_id", "row_id", "eligible", "served_price", "served_vol",
                     "price_only", "inside", "mid_err_spreads", "kivmin", "vol_err_bp", "price_err",
                     "price_err_spreads", "dte_band", "moneyness_band", "status")


def native_cost(native: dict) -> dict:
    """The native tool's own shard aggregates (present under every policy, unlike graded
    per-chain timing, which a policy file must request)."""
    return {k: native.get(k) for k in ("chain_latency_us_p50", "chain_latency_us_p95",
                                       "chain_latency_us_p99", "fit_and_reprice_seconds",
                                       "served_rows", "fitted_chains")}


def assemble_report(rows: pl.DataFrame, cards: list[dict], infos: list[dict], tier: str,
                    args: argparse.Namespace) -> dict:
    labelled = tier == "labelled-panel"
    run0 = infos[0]["run"]
    loads = [s.get("load_before") for i in infos for s in i["run"]["shards"]]
    loads_after = [s.get("load_after") for i in infos for s in i["run"]["shards"]]
    panel = json.loads(Path(args.panel).read_text(encoding="utf-8")) if args.panel else {}
    report = {
        "schema": SCHEMA, "schema_version": SCHEMA_VERSION,
        "provenance": {
            "git_sha": git_sha(), "tool_sha256": TOOL_SHA256,
            "binary_sha256": run0.get("binary_sha256"), "probe_sha256": run0.get("probe_sha256"),
            "policy_path": run0.get("policy"), "policy_sha256": run0.get("policy_sha256"),
            "dividends": run0.get("dividends"),
            "source_sha256": infos[0]["manifest"].get("source", {}).get("sha256"),
            "dataset_manifest_sha256": [i["run"]["dataset_manifest_sha256"] for i in infos],
            "host": {"cpu": platform.processor(), "fit_workers": run0.get("fit_workers"),
                     "load_before": loads, "load_after": loads_after},
            "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
            "timing_qualified": False},
        "scope": {"date": infos[0]["date"], "buckets": [i["bucket"] for i in infos], "tier": tier,
                  "split": "development" if labelled else "universe",
                  "panel_rule_sha256": panel.get("panel_rule_sha256"),
                  "panel_roster_sha256": panel.get("panel_roster_sha256"),
                  "labels_opened": any(i["labels_opened"] for i in infos)},
        "totals": metrics(rows, cards, labelled),
        "by_bucket": [{"bucket_et": i["bucket"], **metrics(
            rows.filter(pl.col("bucket_et") == i["bucket"]),
            [c for c in cards if c["bucket_et"] == i["bucket"]], labelled)} for i in infos],
        "by_archetype": archetype_metrics(rows, cards, labelled),
        "by_dte_band": group_metrics(rows, cards, "dte_band", labelled),
        "by_moneyness_band": group_metrics(rows, cards, "moneyness_band", labelled, row_level=True),
        "failure_modes": failure_table(cards),
        "worst": worst_lists(cards),
        "run_cost": [{"bucket_et": i["bucket"], "shards": [
            {"rows": s["rows"], "fit": s.get("fit"), "probe_wall_s": (s.get("probe") or {}).get("process_wall_s"),
             "native": native_cost(r["native_report"])}
            for s, r in zip(i["run"]["shards"], i["receipts"])]} for i in infos],
        "chains": sorted(cards, key=chain_key),
    }
    return report


def write_report(report: dict, path: Path) -> None:
    """chains[] goes to chains.parquet beside the JSON past 50k chains (hash in the JSON)."""
    if len(report["chains"]) > 50000:
        side = path.with_suffix(".chains.parquet")
        pl.DataFrame([{**c, **{k: json.dumps(c[k]) for k in ("fit", "shape", "labels", "greeks",
                                                             "status_counts", "archetypes",
                                                             "secondary")}}
                      for c in report["chains"]], infer_schema_length=None).write_parquet(side)
        report = {**report, "chains": {"file": side.name, "sha256": sha256(side),
                                       "count": len(report["chains"])}}
    with path.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(clean_json(report), indent=1, sort_keys=False, allow_nan=False,
                                default=_json_default) + "\n")


def clean_json(value):
    """Non-finite floats become null (JSON has no NaN); containers recurse."""
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {k: clean_json(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [clean_json(v) for v in value]
    return value


def _json_default(value):
    if isinstance(value, (np.integer,)):
        return int(value)
    if isinstance(value, (np.floating,)):
        return float(value)
    raise TypeError(f"not JSON serializable: {type(value)}")


REPORT_KEYS = ("schema", "schema_version", "provenance", "scope", "totals", "by_bucket",
               "by_archetype", "by_dte_band", "by_moneyness_band", "failure_modes", "worst", "chains")
CHAIN_KEYS = ("uid", "root", "bucket_et", "expiry", "dte_cal", "variance_years", "archetypes",
              "rows", "eligible", "two_sided", "ask_only", "admitted", "status_counts", "fit",
              "served_price_rows", "served_vol_rows", "price_only_rows", "containment",
              "mid_err_spreads_mae", "max_outside_spreads", "shape", "api_violations", "labels",
              "greeks", "failure_mode", "secondary")


def validate_report(report: dict) -> None:
    """Schema-v1 structural check (raises ValueError)."""
    if report.get("schema") != SCHEMA or report.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("not a universe-fit-bench v1 report")
    missing = [k for k in REPORT_KEYS if k not in report]
    if missing:
        raise ValueError(f"report missing {missing}")
    if report["provenance"].get("timing_qualified") is not False:
        raise ValueError("timings must be labelled unqualified")
    chains = report["chains"]
    if isinstance(chains, list):
        for c in chains:
            gone = [k for k in CHAIN_KEYS if k not in c]
            if gone:
                raise ValueError(f"chain missing {gone}")
            if c["failure_mode"] not in FAILURE_ORDER:
                raise ValueError(f"unknown failure mode {c['failure_mode']}")
        if sum(m["chains"] for m in report["failure_modes"]) != len(chains):
            raise ValueError("failure modes do not partition the chains")
    if report["scope"]["tier"] == "label-free-board" and report["scope"]["labels_opened"]:
        raise ValueError("label-free tier opened labels")


# ── report: summary.md ──────────────────────────────────────────────────────

def _fmt(v, digits: int = 3) -> str:
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.{digits}g}" if abs(v) < 1e4 else f"{v:,.0f}"
    return f"{v:,}" if isinstance(v, int) else str(v)


def load_chains(report: dict, path: Path) -> list[dict]:
    chains = report["chains"]
    if isinstance(chains, list):
        return chains
    side = path.parent / chains["file"]
    out = []
    for row in pl.read_parquet(side).to_dicts():
        for k in ("fit", "shape", "labels", "greeks", "status_counts", "archetypes", "secondary"):
            row[k] = json.loads(row[k])
        out.append(row)
    return out


def stability(chains: list[dict]) -> dict:
    """Per (uid, bucket) the uid's mode = most frequent chain mode (ties: rule order); count the
    uids whose mode changes across buckets."""
    modes = defaultdict(lambda: defaultdict(Counter))
    for c in chains:
        modes[c["uid"]][c["bucket_et"]][c["failure_mode"]] += 1
    changed, transitions = 0, Counter()
    per_uid = {}
    for uid, buckets in modes.items():
        seq = []
        for b in sorted(buckets):
            counts = buckets[b]
            seq.append(max(counts, key=lambda m: (counts[m], -FAILURE_ORDER.index(m))))
        per_uid[uid] = seq
        if len(set(seq)) > 1:
            changed += 1
            for a, b in zip(seq, seq[1:]):
                if a != b:
                    transitions[f"{a}->{b}"] += 1
    return {"uids": len(modes), "uids_changing_mode": changed,
            "buckets_per_uid_max": max((len(v) for v in modes.values()), default=0),
            "top_transitions": transitions.most_common(10)}


def summary_markdown(reports: list[tuple[dict, list[dict], str]]) -> str:
    lines = ["# Universe fit / price / risk robustness — summary", "",
             "All timings are UNQUALIFIED (shared, loaded host). Generated by "
             "atx-vol/tools/universe_fit_bench.py report.", ""]
    for report, chains, name in reports:
        sc_ = report["scope"]
        t = report["totals"]
        lines += [f"## {name}: {sc_['tier']} ({', '.join(sc_['buckets'])})", "",
                  f"policy `{report['provenance']['policy_path']}` sha256 "
                  f"`{(report['provenance']['policy_sha256'] or '-')[:12]}`; binary "
                  f"`{(report['provenance']['binary_sha256'] or '')[:12]}`; labels opened: "
                  f"{sc_['labels_opened']}", "",
                  "| metric | value |", "|---|---|"]
        for k in ("rows", "eligible_rows", "chains", "price_coverage", "vol_coverage", "containment",
                  "mid_err_spreads_mae", "mid_err_spreads_p95", "shape_monotone", "shape_convex",
                  "shape_calendar", "shape_duplicate", "price_only_rows", "kivmin_rows",
                  "fit_cpu_s", "rows_per_cpu_s", "chain_wall_us_p50", "chain_wall_us_p99",
                  "vol_bp_rmse", "vol_bp_mae", "vol_bp_bias", "greeks_evaluated", "greeks_refused",
                  "greeks_sanity_fail", "reprice_mismatch_rows", "reprice_max_abs"):
            if k in t:
                lines.append(f"| {k} | {_fmt(t[k])} |")
        if len(report["by_bucket"]) > 1:
            lines += ["", "| bucket | rows | chains | price cov | containment | vol RMSE bp | "
                      "chain p99 us |", "|---|---|---|---|---|---|---|"]
            for b in report["by_bucket"]:
                lines.append(f"| {b['bucket_et']} | {_fmt(b['rows'])} | {_fmt(b['chains'])} | "
                             f"{_fmt(b['price_coverage'])} | {_fmt(b['containment'])} | "
                             f"{_fmt(b.get('vol_bp_rmse'))} | {_fmt(b['chain_wall_us_p99'])} |")
        lines += ["", "### Run cost per shard (unqualified)", "",
                  "| bucket | rows | fit CPU s | fit wall s | probe wall s | native chain p50 us | "
                  "native chain p99 us |", "|---|---|---|---|---|---|---|"]
        for rc in report["run_cost"]:
            for sh in rc["shards"]:
                fit, nat = sh.get("fit") or {}, sh.get("native") or {}
                lines.append(f"| {rc['bucket_et']} | {_fmt(sh['rows'])} | {_fmt(fit.get('process_cpu_s'))} | "
                             f"{_fmt(fit.get('process_wall_s'))} | {_fmt(sh.get('probe_wall_s'))} | "
                             f"{_fmt(nat.get('chain_latency_us_p50'))} | {_fmt(nat.get('chain_latency_us_p99'))} |")
        modes = [m for m in report["failure_modes"] if m["chains"]]
        arches = sorted({a for m in modes for a in m["by_archetype"]})
        lines += ["", "### Failure modes (primary, first matching rule)", "",
                  "| mode | chains | rows | uids |", "|---|---|---|---|"]
        lines += [f"| {m['mode']} | {_fmt(m['chains'])} | {_fmt(m['rows'])} | {_fmt(m['uids'])} |"
                  for m in modes]
        lines += ["", "### Failure mode x archetype (chains; a chain counts in each archetype of "
                  "its uid)", "", "| mode | " + " | ".join(arches) + " |",
                  "|---|" + "---|" * len(arches)]
        for m in modes:
            lines.append(f"| {m['mode']} | " + " | ".join(_fmt(m["by_archetype"].get(a, 0))
                                                         for a in arches) + " |")
        for key, title in (("containment", "lowest containment"), ("refused_rows", "most refused rows"),
                           ("vol_rmse_bp", "highest vol RMSE (bp)"),
                           ("greek_refusal", "most Greek refusals"),
                           ("reprice_mismatch", "largest reprice gap (USD)"),
                           ("fit_wall_us", "slowest chains (us, unqualified)")):
            items = report["worst"][key][:20]
            if not items:
                continue
            lines += ["", f"### Worst 20: {title}", "", "| uid | root | bucket | expiry | value |",
                      "|---|---|---|---|---|"]
            lines += [f"| {i['uid']} | {i['root']} | {i['bucket_et']} | {i['expiry']} | "
                      f"{_fmt(i['value'])} |" for i in items]
        st = stability(chains)
        lines += ["", "### Cross-bucket stability of each uid's primary mode", "",
                  f"{st['uids']} uids; {st['uids_changing_mode']} change mode across "
                  f"{st['buckets_per_uid_max']} buckets.", ""]
        if st["top_transitions"]:
            lines += ["| transition | count |", "|---|---|"]
            lines += [f"| {k} | {v} |" for k, v in st["top_transitions"]]
        lines.append("")
    return "\n".join(lines) + "\n"


def cmd_report(args: argparse.Namespace) -> str:
    loaded = []
    for spec in args.input:
        name, _, path = spec.partition("=")
        report = json.loads(Path(path).read_text(encoding="utf-8"))
        if isinstance(report["chains"], list):
            validate_report(report)
        loaded.append((report, load_chains(report, Path(path)), name))
    text = summary_markdown(loaded)
    Path(args.output).write_text(text, encoding="utf-8")
    print(json.dumps({"summary": str(Path(args.output).resolve()), "reports": len(loaded)}))
    return text


# ── CLI ─────────────────────────────────────────────────────────────────────

def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("panel", help="draw the development panel from label-free features")
    p.add_argument("--features-dataset", required=True, type=Path,
                   help="a label-free dataset (e.g. the universe-20261014 1030 board)")
    p.add_argument("--output", required=True, type=Path)
    p.set_defaults(func=cmd_panel)
    p = sub.add_parser("prepare", help="per-bucket development datasets + probe labels")
    p.add_argument("--roster", required=True, type=Path)
    p.add_argument("--source", type=Path, default=SOURCE)
    p.add_argument("--buckets", default=",".join(BUCKETS))
    p.add_argument("--output", required=True, type=Path)
    p.set_defaults(func=cmd_prepare)
    p = sub.add_parser("run", help="shard, run the native fitter and the probe per shard")
    p.add_argument("--dataset", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--probe", required=True, type=Path)
    p.add_argument("--policy", default="auto")
    p.add_argument("--dividends", default="funding-pv", choices=list(qb.DIVIDEND_CONVENTIONS))
    p.add_argument("--rows-per-shard", type=int, required=True)
    p.add_argument("--kernel", default="both", choices=["family", "cold", "both"])
    p.add_argument("--labels", type=Path, help="probe-labels.csv (development datasets only)")
    p.add_argument("--probe-rows", action="store_true", help="also write the probe's per-row CSV")
    p.set_defaults(func=cmd_run)
    p = sub.add_parser("reprobe", help="re-run only the probe on every shard of a run")
    p.add_argument("--run", required=True, type=Path)
    p.add_argument("--probe", required=True, type=Path)
    p.add_argument("--probe-rows", action="store_true")
    p.set_defaults(func=cmd_reprobe)
    p = sub.add_parser("score", help="chain scorecards -> report JSON")
    p.add_argument("--pair", action="append", required=True, help="DATASET=RUN_DIR (one per bucket)")
    p.add_argument("--tier", required=True, choices=["labelled-panel", "label-free-board"])
    p.add_argument("--archetypes", help="CSV with underlier,strata (panel roster); default: "
                                        "computed label-free from the first dataset")
    p.add_argument("--panel", help="panel.json (rule / roster hashes)")
    p.add_argument("--output", required=True, type=Path)
    p.set_defaults(func=cmd_score)
    p = sub.add_parser("report", help="summary.md over score reports")
    p.add_argument("--input", action="append", required=True, help="NAME=report.json")
    p.add_argument("--output", required=True, type=Path)
    p.set_defaults(func=cmd_report)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        args.func(args)
        return 0
    except (ValueError, OSError, RuntimeError, PermissionError) as exc:
        print(f"universe-fit-bench: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
