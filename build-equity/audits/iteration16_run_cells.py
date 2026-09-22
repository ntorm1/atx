#!/usr/bin/env python3
"""iteration16_run_cells.py -- drive the 13 pre-registered cp16 scorecard cells.

Pre-registered by rulings R16-4 (2017 x top-3000 is NOT FIT, never run), R16-5
(panel screen), R16-11 (measure first) and R16-12 (layout).

Grid: year Y in 2013..2019 x cut in {1000, 3000} = 14 cells; (2017, 3000) is
pre-declared NOT FIT ("5,072 > kMaxIcInstruments 4,096") and is skipped.

Per cell three children run in order, each logged to <out>/<phase>.log, peak
working set sampled every 0.25 s via psapi.GetProcessMemoryInfo, elapsed wall
recorded:
  panel     -> C:/atx/data/equity_scorecard16_ctx_{Y}_t{CUT}_{stamp}/context.bin
  baseline  -> C:/atx/data/equity_scorecard16_base_{Y}_t{CUT}_{stamp}
  ic        -> C:/atx/data/equity_scorecard16_ic_{Y}_t{CUT}_{stamp}
The three dirs sit DIRECTLY under C:/atx/data because equity-ic resolves the
frozen audit dir equity_source_reconciliation_2013_20260919 at the PARENT of
--baseline-dir (ruling R16-12).

EVALUATION START S(Y) = the first NYSE session on/after Y-01-01, from a hard
table (the stages require an exact session key; Y-01-01 is a holiday and would
be refused): 2013 -> 2013-04-04 (cp14 anchor), 2014 -> 2014-01-02,
2015 -> 2015-01-02, 2016 -> 2016-01-04, 2017 -> 2017-01-03, 2018 -> 2018-01-02,
2019 -> 2019-01-02. --evaluation-end and panel --end are (Y+1)-01-01;
--universe-eval-start stays the calendar Y-01-01.

WARMUP RULE (no session calendar is available to this script; membership.csv
carries rebalance dates only): panel --start = (Y-1)-01-01 minus 10 calendar
days, which is ~260 NYSE sessions before Y-01-01 and so comfortably above the
256 rows kEquityBaselineWarmup needs -- surplus warmup rows are harmless. 2013
uses the cp14 anchor 2012-03-26. Override any year with --warmup-start-{Y}.

Immutability: a phase whose output directory exists is refused; --resume skips a
cell whose <ic>/ic.csv exists; --attempt N > 1 appends _attempt{N} to every dir.
Exit 0 iff all three phases of every RUN cell exited 0.
"""
from __future__ import annotations

import argparse
import ctypes
from concurrent.futures import ThreadPoolExecutor
import datetime as dt
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

DATA_ROOT = Path("C:/atx/data")
WORKTREE = Path("C:/atx/.worktrees/equity-platform")
YEARS = (2013, 2014, 2015, 2016, 2017, 2018, 2019)
CUTS = (1000, 3000)
NOT_FIT = {(2017, 3000): "5,072 > kMaxIcInstruments 4,096"}
VALIDATION_YEARS = (2020, 2021, 2022)
EVAL_START = {2013: "2013-04-04", 2014: "2014-01-02", 2015: "2015-01-02", 2016: "2016-01-04",
              2017: "2017-01-03", 2018: "2018-01-02", 2019: "2019-01-02",
              2020: "2020-01-02", 2021: "2021-01-04", 2022: "2022-01-03"}
CP14_DESIGN = WORKTREE / "atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md"
CP14_DESIGN_SHA256 = "888c726b123c02b39636876a667cfab890b4304794f6be42d46d35befbedbe25"
CP16_DESIGN = WORKTREE / "atx-engine/reviews/2026-09-20-iteration16-alpha-scorecard-design.md"
CP16_DESIGN_SHA256 = "a83484037cae527e2fbfba5dce04289742d1c81666b2c03c132f3da9268c04d1"
AUDITS = WORKTREE / "build-equity/audits"
SAMPLE_SECONDS = 0.25
PHASES = ("panel", "baseline", "ic")
PHASE_DIR = {"panel": "ctx", "baseline": "base", "ic": "ic"}
PHASE_MARKER = {"panel": "context.bin", "baseline": "evaluation.bin", "ic": "ic.csv"}

def fwd(path: Path) -> str:
    return str(path).replace("\\", "/")

def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

def sha256_file(path: Path):
    try:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1 << 20), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None

def line_count(path: Path) -> int:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return sum(1 for line in stream if line.strip())
    except OSError:
        return 0

def str2bool(text: str) -> bool:
    if text.strip().lower() in ("true", "1", "yes", "on"):
        return True
    if text.strip().lower() in ("false", "0", "no", "off"):
        return False
    raise argparse.ArgumentTypeError("expected true/false, got " + text)

class _ProcessMemoryCountersEx(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.c_uint32), ("PageFaultCount", ctypes.c_uint32),
        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t)]

def sample_memory(process: subprocess.Popen):
    """Peak working set of a running or just-finished child; None when unavailable."""
    if sys.platform != "win32":
        return None
    try:
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        query = psapi.GetProcessMemoryInfo
        query.argtypes = [ctypes.c_void_p, ctypes.POINTER(_ProcessMemoryCountersEx), ctypes.c_uint32]
        query.restype = ctypes.c_int
        counters = _ProcessMemoryCountersEx()
        counters.cb = ctypes.sizeof(counters)
        # CPython keeps the child handle alive until the Popen object is destroyed.
        if not query(int(process._handle), ctypes.byref(counters), counters.cb):
            return None
        return {"peak_working_set_bytes": int(counters.PeakWorkingSetSize),
                "peak_pagefile_bytes": int(counters.PeakPagefileUsage)}
    except Exception:  # noqa: BLE001 - diagnostics only, never fatal
        return None

def run_child(args: list, log_path: Path) -> dict:
    """Run one child to completion, sampling peak working set every SAMPLE_SECONDS."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started_utc, started = utc_now(), time.perf_counter()
    peak_ws = peak_pf = samples = 0
    seen = False
    with log_path.open("xb") as stream:
        process = subprocess.Popen(args, stdout=stream, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL)
        while True:
            done = process.poll() is not None
            shot = sample_memory(process)
            if shot is not None:
                seen, samples = True, samples + 1
                peak_ws = max(peak_ws, shot["peak_working_set_bytes"])
                peak_pf = max(peak_pf, shot["peak_pagefile_bytes"])
            if done:
                break
            time.sleep(SAMPLE_SECONDS)
        exit_code = process.wait()
    record = {"args": args, "exit_code": exit_code,
              "elapsed_s": round(time.perf_counter() - started, 3),
              "peak_ws_bytes": peak_ws if seen else None,
              "peak_pagefile_bytes": peak_pf if seen else None,
              "memory_samples": samples, "sample_interval_s": SAMPLE_SECONDS,
              "started_utc": started_utc, "ended_utc": utc_now(), "log": fwd(log_path)}
    if not seen:
        record["peak_ws_note"] = "GetProcessMemoryInfo unavailable; peak working set not recorded"
    return record

def warmup_start(year: int, overrides: dict) -> str:
    if year in overrides:
        return overrides[year]
    if year == 2013:
        return "2012-03-26"
    return (dt.date(year - 1, 1, 1) - dt.timedelta(days=10)).isoformat()

def cell_dirs(year: int, cut: int, stamp: str, attempt: int, ic_prefix: str = "equity_scorecard16",
              ic_stamp: str = None, base_prefix: str = "equity_scorecard16",
              base_stamp: str = None, panel_prefix: str = "equity_scorecard16",
              panel_stamp: str = None) -> dict:
    """ctx keeps the cp16 naming unless --panel-name-prefix/--panel-stamp are given; the base and
    ic dirs may carry their own prefix/stamp so a signal-only sweep (cp17 families) reuses the cp16
    contexts and baselines untouched."""
    suffix = "" if attempt <= 1 else "_attempt%d" % attempt
    def name(kind: str, prefix: str = "equity_scorecard16", this_stamp: str = stamp) -> Path:
        return DATA_ROOT / ("%s_%s_%d_t%d_%s%s" % (prefix, kind, year, cut, this_stamp, suffix))
    return {"ctx": name("ctx", panel_prefix, panel_stamp or stamp), "base": name("base", base_prefix, base_stamp or stamp),
            "ic": name("ic", ic_prefix, ic_stamp or stamp)}

def build_commands(year: int, cut: int, dirs: dict, opt) -> dict:
    start, end = EVAL_START[year], "%d-01-01" % (year + 1)
    context = fwd(dirs["ctx"] / "context.bin")
    floor = []
    if opt.min_dollar_adv > 0:
        floor = ["--min-dollar-adv", format(opt.min_dollar_adv, "f").rstrip("0").rstrip("."),
                 "--dollar-adv-window", str(opt.dollar_adv_window)]
    asof = []
    for spec in opt.asof_field:
        asof += ["--asof-field", spec]
    if opt.asof_max_stale_days is not None:
        asof += ["--asof-max-stale-days", str(opt.asof_max_stale_days)]
    return {
        "panel": [fwd(opt.atx_impl), "panel", "--segs", fwd(opt.segs), "--panel-out", context,
                  "--start", warmup_start(year, opt.warmup_overrides), "--end", end,
                  "--min-adv-usd", "0", "--min-price", "1", "--top-n-by-adv", "0",
                  "--compact-universe", "true",
                  "--preparation-manifest", fwd(opt.prep_manifest),
                  "--universe-membership", fwd(opt.membership),
                  "--universe-cut", "%d:0.00" % cut,
                  "--universe-eval-start", "%d-01-01" % year] + asof,
        "baseline": [fwd(opt.atx_impl), "equity-baseline", "--panel", context,
                     "--out", fwd(dirs["base"]), "--evaluation-start", start,
                     "--evaluation-end", end, "--max-working-bytes", "3000000000"] + floor,
        "ic": [fwd(opt.atx_impl), "equity-ic", "--panel", context,
               "--baseline-dir", fwd(dirs["base"]), "--out", fwd(dirs["ic"]),
               "--evaluation-start", start, "--evaluation-end", end,
               "--max-working-bytes", "3000000000", "--trial-ledger", fwd(opt.ledger)]}

def cell_digests(dirs: dict) -> dict:
    return {"ctx/context.bin.manifest.json": sha256_file(dirs["ctx"] / "context.bin.manifest.json"),
            "base/manifest.json": sha256_file(dirs["base"] / "manifest.json"),
            "ic/ic.csv": sha256_file(dirs["ic"] / "ic.csv"),
            "ic/quantile_spread.csv": sha256_file(dirs["ic"] / "quantile_spread.csv"),
            "ic/signal_autocorr.csv": sha256_file(dirs["ic"] / "signal_autocorr.csv"),
            "ic/manifest.json": sha256_file(dirs["ic"] / "manifest.json")}

def preflight(opt) -> dict:
    report = {"checked_utc": utc_now(), "inputs": {}, "errors": []}
    for label, path, must in (("atx_impl", opt.atx_impl, True), ("segs", opt.segs, True),
                              ("prep_manifest", opt.prep_manifest, True),
                              ("membership", opt.membership, True),
                              ("cp14_design", CP14_DESIGN, True),
                              ("cp16_design", CP16_DESIGN, True),
                              ("ledger_dir", Path(opt.ledger).parent, True)):
        exists = Path(path).exists()
        report["inputs"][label] = {"path": fwd(Path(path)), "exists": exists}
        if must and not exists:
            report["errors"].append("missing required input %s: %s" % (label, fwd(Path(path))))
        if Path(path).is_file():
            report["inputs"][label]["sha256"] = sha256_file(Path(path))
    cp14 = report["inputs"]["cp14_design"].get("sha256")
    report["cp14_design_sha256_expected"] = CP14_DESIGN_SHA256
    if cp14 != CP14_DESIGN_SHA256:
        report["errors"].append("cp14 design sha256 %s != expected %s" % (cp14, CP14_DESIGN_SHA256))
    cp16 = report["inputs"]["cp16_design"].get("sha256")
    report["cp16_design_sha256"] = cp16
    report["cp16_design_sha256_expected"] = opt.expect_design_sha
    if opt.expect_design_sha and cp16 != opt.expect_design_sha:
        report["errors"].append("cp16 design sha256 %s != --expect-design-sha %s"
                                % (cp16, opt.expect_design_sha))
    return report

def parse_args(argv):
    parser = argparse.ArgumentParser(description="cp16 13-cell scorecard runner")
    parser.add_argument("--atx-impl", type=Path, default=WORKTREE / "build-equity/bin/atx-impl.exe")
    parser.add_argument("--segs", type=Path, default=DATA_ROOT
                        / "tickerhistory_training_native_2012_2019_20260920/segments")
    parser.add_argument("--prep-manifest", type=Path, default=DATA_ROOT
                        / "tickerhistory_training_20120326_20191231_20260920/manifest.json")
    parser.add_argument("--membership", type=Path, default=DATA_ROOT
                        / "equity_universe_pit_2013_2019_20260920/membership.bin")
    parser.add_argument("--ledger", type=Path, default=WORKTREE
                        / "atx-engine/reviews/trial-ledger-cp16-restrictions.jsonl")
    parser.add_argument("--stamp", default="20260920")
    parser.add_argument("--attempt", type=int, default=1)
    parser.add_argument("--only-year", default=None,
                        help="one year or a comma list (e.g. 2018,2019) from %s"
                             % ",".join(str(y) for y in YEARS + VALIDATION_YEARS))
    parser.add_argument("--allow-validation", action="store_true",
                        help="permit --only-year in the validation span %d-%d"
                             % (VALIDATION_YEARS[0], VALIDATION_YEARS[-1]))
    parser.add_argument("--only-cut", type=int, default=None, choices=list(CUTS))
    parser.add_argument("--expect-design-sha", default=CP16_DESIGN_SHA256)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--receipt-tag", default="", help="suffix for the receipt file name (partial runs)")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--stop-on-failure", type=str2bool, default=True)
    parser.add_argument("--phases", default="panel,baseline,ic",
                        help="comma list; phases left out must already have their marker on disk")
    parser.add_argument("--ic-name-prefix", default="equity_scorecard16",
                        help="directory prefix for the ic phase only (cp17: equity_scorecard17)")
    parser.add_argument("--ic-stamp", default=None, help="stamp for the ic phase only (default --stamp)")
    parser.add_argument("--base-name-prefix", default="equity_scorecard16",
                        help="directory prefix for the baseline phase only (cp19: equity_scorecard19)")
    parser.add_argument("--base-stamp", default=None, help="stamp for the baseline phase only (default --stamp)")
    parser.add_argument("--panel-name-prefix", default="equity_scorecard16",
                        help="directory prefix for the panel (ctx) dir; baseline/ic read the panel from it")
    parser.add_argument("--panel-stamp", default=None, help="stamp for the panel (ctx) dir only (default --stamp)")
    parser.add_argument("--asof-field", action="append", default=[], metavar="NAME=PATH",
                        help="repeatable; passed verbatim to the panel phase as --asof-field NAME=PATH")
    parser.add_argument("--asof-max-stale-days", type=int, default=None,
                        help="passed to the panel phase as --asof-max-stale-days when given")
    parser.add_argument("--min-dollar-adv", type=float, default=0.0,
                        help="equity-baseline liquidity floor; passed through only when > 0")
    parser.add_argument("--dollar-adv-window", type=int, default=21,
                        help="equity-baseline dollar-ADV window (passed with --min-dollar-adv)")
    parser.add_argument("--parallel", type=int, default=1,
                        help="cells run concurrently (bounded by RAM; ic-only cells peak < 1 GB)")
    parser.add_argument("--log-tag", default=None, help="suffix for per-phase log names (default --receipt-tag)")
    for year in YEARS:
        parser.add_argument("--warmup-start-%d" % year, dest="warmup_%d" % year, default=None)
    opt = parser.parse_args(argv)
    opt.warmup_overrides = dict((y, getattr(opt, "warmup_%d" % y))
                                for y in YEARS if getattr(opt, "warmup_%d" % y))
    if opt.attempt < 1:
        parser.error("--attempt must be >= 1")
    if opt.only_year is not None:
        years = []
        for token in (t.strip() for t in str(opt.only_year).split(",")):
            if not token:
                continue
            try:
                year = int(token)
            except ValueError:
                parser.error("--only-year: invalid year %r" % token)
            if year not in YEARS + VALIDATION_YEARS:
                parser.error("--only-year: %d not in %s"
                             % (year, ",".join(str(y) for y in YEARS + VALIDATION_YEARS)))
            years.append(year)
        if not years:
            parser.error("--only-year: empty list")
        opt.only_year = tuple(years)
    for year in opt.only_year or ():
        if year in VALIDATION_YEARS and not opt.allow_validation:
            parser.error("year %d is in the validation span 2020-2022; pass --allow-validation (note: "
                         "atx-impl equity-baseline/equity-ic seal evaluation at 2020-01-01 and will "
                         "refuse the run until that code-level seal is lifted by a declared ruling)"
                         % year)
    for spec in opt.asof_field:
        name, sep, path = spec.partition("=")
        if not sep or not path or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            parser.error("--asof-field expects NAME=PATH with NAME matching [A-Za-z_][A-Za-z0-9_]*, "
                         "got %r" % spec)
        if not Path(path).exists():
            parser.error("--asof-field %s: path does not exist: %s" % (name, path))
    if opt.asof_max_stale_days is not None and opt.asof_max_stale_days < 0:
        parser.error("--asof-max-stale-days must be >= 0")
    opt.phases = tuple(p.strip() for p in opt.phases.split(",") if p.strip())
    if any(p not in PHASES for p in opt.phases) or not opt.phases:
        parser.error("--phases must be a non-empty subset of " + ",".join(PHASES))
    if opt.parallel < 1:
        parser.error("--parallel must be >= 1")
    if opt.log_tag is None:
        opt.log_tag = opt.receipt_tag
    return opt

def selected_cells(opt) -> list:
    wanted = opt.only_year or ()
    years = YEARS + (VALIDATION_YEARS if opt.allow_validation
                     and any(y in VALIDATION_YEARS for y in wanted) else ())
    return [(y, c) for y in years for c in CUTS
            if (opt.only_year is None or y in wanted)
            and (opt.only_cut is None or c == opt.only_cut)]

def main(argv=None) -> int:
    opt = parse_args(argv)
    receipt_path = AUDITS / ("iteration16-cells-attempt%d%s%s.json"
                             % (opt.attempt, opt.receipt_tag, "-dryrun" if opt.dry_run else ""))
    if receipt_path.exists():
        print("REFUSED: receipt already exists (immutable): %s" % fwd(receipt_path))
        return 2
    pre = preflight(opt)
    for message in pre["errors"]:
        print("PREFLIGHT ERROR: " + message)
    receipt = {"script": fwd(Path(__file__).resolve()),
               "script_sha256": sha256_file(Path(__file__).resolve()),
               "generated_utc": utc_now(), "attempt": opt.attempt, "stamp": opt.stamp,
               "dry_run": bool(opt.dry_run), "resume": bool(opt.resume),
               "stop_on_failure": bool(opt.stop_on_failure),
               "evaluation_start_rule": "first NYSE session on/after Y-01-01, hard table "
                                        + json.dumps(EVAL_START),
               "warmup_rule": "(Y-1)-01-01 minus 10 calendar days (~260 NYSE sessions, above the "
                              "256-row baseline warmup; surplus rows harmless); 2013 = "
                              "2012-03-26 (cp14 anchor); override with --warmup-start-<Y>",
               "warmup_overrides": dict((str(k), v) for k, v in opt.warmup_overrides.items()),
               "not_fit_cells": {"2017_t3000": NOT_FIT[(2017, 3000)]},
               "preflight": pre,
               "ledger": {"path": fwd(opt.ledger), "lines_before": line_count(opt.ledger)},
               "cells": []}
    fatal = bool(pre["errors"]) and not opt.dry_run
    all_ok = not fatal
    receipt["phases"] = list(opt.phases)
    receipt["ic_name_prefix"], receipt["ic_stamp"] = opt.ic_name_prefix, opt.ic_stamp or opt.stamp
    receipt["base_name_prefix"], receipt["base_stamp"] = opt.base_name_prefix, opt.base_stamp or opt.stamp
    receipt["min_dollar_adv"], receipt["dollar_adv_window"] = opt.min_dollar_adv, opt.dollar_adv_window
    receipt["parallel"] = opt.parallel
    receipt["panel_name_prefix"], receipt["panel_stamp"] = opt.panel_name_prefix, opt.panel_stamp or opt.stamp
    receipt["asof_fields"], receipt["asof_max_stale_days"] = list(opt.asof_field), opt.asof_max_stale_days
    asof_note = ""
    if opt.asof_field or opt.asof_max_stale_days is not None:
        asof_note = "  asof=[%s] asof_max_stale_days=%s" % (", ".join(opt.asof_field),
                                                           opt.asof_max_stale_days)
    stop = {"flag": False}

    def run_cell(index: int, year: int, cut: int) -> dict:
        dirs = cell_dirs(year, cut, opt.stamp, opt.attempt, opt.ic_name_prefix, opt.ic_stamp,
                         opt.base_name_prefix, opt.base_stamp, opt.panel_name_prefix, opt.panel_stamp)
        commands = build_commands(year, cut, dirs, opt)
        entry = {"year": year, "cut": cut, "eval_start": EVAL_START[year],
                 "eval_end_exclusive": "%d-01-01" % (year + 1),
                 "warmup_start": warmup_start(year, opt.warmup_overrides),
                 "dirs": dict((k, fwd(v)) for k, v in dirs.items()),
                 "commands": commands, "phases": {}}
        if (year, cut) in NOT_FIT:
            entry["status"], entry["reason"] = "skipped_not_fit", NOT_FIT[(year, cut)]
            print("SKIP cell %d t%d -- NOT FIT: %s" % (year, cut, entry["reason"]))
            return entry
        if opt.resume and (dirs["ic"] / "ic.csv").exists():
            entry["status"], entry["digests"] = "skipped_resume", cell_digests(dirs)
            print("SKIP cell %d t%d -- resume, %s exists" % (year, cut, fwd(dirs["ic"] / "ic.csv")))
            return entry
        if opt.dry_run:
            entry["status"] = "dry_run"
            print("== cell %d t%d  warmup=%s  eval=[%s, %s)%s =="
                  % (year, cut, entry["warmup_start"], entry["eval_start"],
                     entry["eval_end_exclusive"], asof_note))
            for phase in PHASES:
                out_dir = dirs[PHASE_DIR[phase]]
                print("  [%s%s] out=%s exists=%s" % (phase, "" if phase in opt.phases else " (reuse)",
                                                     fwd(out_dir), out_dir.exists()))
                if phase in opt.phases:
                    print("  " + " ".join(commands[phase]))
            return entry
        if fatal or stop["flag"]:
            entry["status"] = "not_run_preflight_failed" if fatal else "not_run_stopped"
            return entry
        if opt.parallel > 1 and index:
            time.sleep(2.0 * (index % opt.parallel))  # soften ledger-lock contention at start
        entry["status"] = "run"
        cell_ok = True
        for phase in PHASES:
            out_dir = dirs[PHASE_DIR[phase]]
            marker = out_dir / PHASE_MARKER[phase]
            if phase not in opt.phases:
                if marker.exists():
                    entry["phases"][phase] = {"exit_code": None, "status": "reused_existing",
                                              "marker": fwd(marker)}
                    continue
                entry["phases"][phase] = {"exit_code": None,
                                          "error": "phase not selected and marker missing: " + fwd(marker)}
                print("REFUSED %d t%d %s: not selected and %s missing" % (year, cut, phase, fwd(marker)))
                cell_ok = False
                break
            if out_dir.exists():
                if opt.resume and marker.exists():
                    entry["phases"][phase] = {"exit_code": None, "status": "skipped_resume_phase",
                                              "marker": fwd(marker)}
                    print("SKIP %d t%d %s -- resume, %s exists" % (year, cut, phase, fwd(marker)))
                    continue
                entry["phases"][phase] = {"exit_code": None,
                                          "error": "output dir exists (immutable): " + fwd(out_dir)}
                print("REFUSED %d t%d %s: output dir exists %s" % (year, cut, phase, fwd(out_dir)))
                cell_ok = False
                break
            if phase == "panel":
                out_dir.mkdir(parents=True, exist_ok=False)
            log_path = AUDITS / "iteration16-cells-logs" / ("%d_t%d_%s%s.log" % (year, cut, phase, opt.log_tag))
            if phase == "panel" and asof_note:
                print("== cell %d t%d%s ==" % (year, cut, asof_note))
            print("RUN %d t%d %s" % (year, cut, phase))
            result = run_child(commands[phase], log_path)
            entry["phases"][phase] = result
            print("  exit=%s elapsed_s=%s peak_ws_bytes=%s"
                  % (result["exit_code"], result["elapsed_s"], result["peak_ws_bytes"]))
            if result["exit_code"] != 0:
                cell_ok = False
                break
        entry["digests"], entry["ok"] = cell_digests(dirs), cell_ok
        if not cell_ok and opt.stop_on_failure:
            print("STOP: --stop-on-failure and cell %d t%d failed" % (year, cut))
            stop["flag"] = True
        return entry

    cells = selected_cells(opt)
    if opt.parallel > 1 and not opt.dry_run:
        with ThreadPoolExecutor(max_workers=opt.parallel) as pool:
            entries = list(pool.map(lambda ic: run_cell(ic[0], *ic[1]), enumerate(cells)))
    else:
        entries = []
        for index, (year, cut) in enumerate(cells):
            entries.append(run_cell(index, year, cut))
            if stop["flag"]:
                break
    for entry in entries:
        receipt["cells"].append(entry)
        if "ok" in entry:
            all_ok = all_ok and entry["ok"]
    receipt["ledger"]["lines_after"] = line_count(opt.ledger)
    receipt["all_run_cells_ok"] = bool(all_ok)
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    with receipt_path.open("x", encoding="utf-8") as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")
    print("receipt: %s" % fwd(receipt_path))
    return 0 if all_ok else 1

if __name__ == "__main__":
    raise SystemExit(main())
