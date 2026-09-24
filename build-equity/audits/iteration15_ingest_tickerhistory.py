"""Iteration 15: prepare + natively ingest six calendar-year tickerhistory windows (2014..2019).

Reproduces EXACTLY the recipe that produced the existing 2013 training window, one
calendar year at a time (Jan 1 .. Dec 31 inclusive; never any date >= 2020-01-01).

Recipe evidence (all paths relative to C:/atx or the equity-platform worktree):

  Prepared 2013 window
    C:/atx/data/tickerhistory_training_20120326_20131231_20260919/manifest.json
      policy_version = "tickerhistory-qa-v1"; status = "complete"
      window = {"start_inclusive": "2012-03-26", "end_inclusive": "2013-12-31"}
      source.path   = C:\\Users\\natha\\Downloads\\tbltickerhistory3_10y.zip
      source.sha256 = 7d2b7a615a08ef3b8686608eda384c4e30e2ce23bdee294ea141b3c9334acd8f
      source.size_bytes = 3542506361; member = tbltickerhistory3_10y.txt (crc 15b6e051)
      counts = {source_rows 31598499, selected 2910733, accepted 2883147, rejected 27586,
                duplicate_positive_keys 4, outside_window_rows 28687766}
    C:/atx/build-equity/iteration6-prepare-training.log  (prepare stdout JSON for that window)

  Native load of the 2013 window
    C:/atx/data/tickerhistory_training_native_20260919/segments/_ingestion.manifest.json
      schema atx-ingestion-v1; preparation.policy_version tickerhistory-qa-v1;
      recipe.version tickerhistory-native-load-v1; recipe.min_date_nanos 1332720000000000000
      (= 2012-03-26 = window start_inclusive); exclude_no_sector false; rows_filtered 0;
      rows_malformed 0; rows_kept == rows_read == 2883147 == accepted_rows; dates_written 445
    C:/atx/data/tickerhistory_training_native_20260919/segments/_manifest.json (same counts)
    C:/atx/build-equity/iteration6-native-load.log  ("[atx-impl] stage=load ... input_sha256=
      6131dc64..." == accepted.zip sha256; native_exit_code=0; elapsed_seconds=135.7)

  Preparation tool  atx-engine/tools/prepare_tickerhistory.py
    :22   POLICY_VERSION = "tickerhistory-qa-v1"
    :139  prepare(source, output_dir, start, end); :147 output_dir.mkdir(exist_ok=False)
    :243  window selection is inclusive: `start_key <= date <= end_key`
    :289-:293 writes accepted.zip, optional conflict_fixture.zip, manifest.json
    :303-:306 argparse: --source, --out-dir, --start-date, --end-date (ISO dates)

  atx-impl load flags  atx-impl/src/config.cpp
    :73  "preparation-manifest"   :75 "zip"   :76 "out"   :77 "min-date"
    :43  "exclude-no-sector" (boolean; NOT passed -> recipe exclude_no_sector=false as in 2013)
  atx-impl/src/stage_load.cpp
    :17-:32 --zip, --out, --min-date (YYYY-MM-DD) are required
    :39     created_at_nanos = 0 (fixed; deterministic)
    :41-:43 begin_ingestion_provenance(cfg.zip, cfg.out, cfg.preparation_manifest)
  atx-impl/src/stage_data_provenance.cpp
    :193-:208 reserve_output: --out must be absent or an empty directory
    :130-:172 validate_preparation: manifest status/policy/accepted binding/window checks
    :245-:286 finish_ingestion_provenance: rows_read must equal prepared accepted_rows;
              writes _ingestion.manifest.json with a "preparation" block only when
              --preparation-manifest was passed (the 2013 receipt HAS that block).

  Drivers that consumed the 2013 window (for the argument style, not the load itself):
    C:/atx/build-equity/audits/iteration6_build_training_context.py :153-:156
      (`--preparation-manifest <prepared>/manifest.json` to `panel`)
    C:/atx/build-equity/audits/iteration7_run_source_audit.py :22-:23
      (`--source C:/Users/natha/Downloads/tbltickerhistory3_10y.zip`)
    C:/atx/build-equity/audits/iteration6_build_training_context.py :60-:102
      (ctypes GetProcessMemoryInfo / PROCESS_MEMORY_COUNTERS_EX peak working set)

Reproduced command lines (per year YYYY):
  <python> atx-engine/tools/prepare_tickerhistory.py --source <zip>
      --out-dir <data-root>/tickerhistory_training_<YYYY>0101_<YYYY>1231_<stamp>
      --start-date YYYY-01-01 --end-date YYYY-12-31
  <atx-impl.exe> load --zip <prepared>/accepted.zip
      --out <data-root>/tickerhistory_training_native_<YYYY>_<stamp>/segments
      --min-date YYYY-01-01 --preparation-manifest <prepared>/manifest.json

Notes on single-calendar-year reproduction:
  * prepare re-scans the entire 11 GB source member for every window (no index); each
    year costs roughly the same wall time as the 2013 run.
  * load has no max-date flag; the window upper bound is enforced entirely by prepare
    (accepted.zip contains only in-window rows), so rows_kept must equal accepted_rows.
  * --min-date is set to the window start (recipe.min_date_nanos == start_inclusive in 2013).

Stdlib only. Never overwrites: refuses when an output dir or receipt path exists.
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_YEARS = [2014, 2015, 2016, 2017, 2018, 2019]
CUTOFF = dt.date(2020, 1, 1)
DEFAULT_ATX_IMPL = "C:/atx/.worktrees/equity-platform/build-equity/bin/atx-impl.exe"
DEFAULT_DATA_ROOT = "C:/atx/data"
DEFAULT_STAMP = "20260920"
DEFAULT_SOURCE_ZIP = "C:/Users/natha/Downloads/tbltickerhistory3_10y.zip"
DEFAULT_PREPARE_TOOL = "C:/atx/.worktrees/equity-platform/atx-engine/tools/prepare_tickerhistory.py"
AUDITS_DIR = Path("C:/atx/.worktrees/equity-platform/build-equity/audits")
EXPECTED_SOURCE_SHA256 = "7d2b7a615a08ef3b8686608eda384c4e30e2ce23bdee294ea141b3c9334acd8f"
EXPECTED_SOURCE_SIZE = 3542506361
POLICY_VERSION = "tickerhistory-qa-v1"
LOAD_RECIPE_VERSION = "tickerhistory-native-load-v1"
INGESTION_SCHEMA = "atx-ingestion-v1"
EPOCH = dt.date(1970, 1, 1)


# ----------------------------------------------------------------------------- helpers
def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path):
    with path.open("r", encoding="utf-8-sig") as stream:
        return json.load(stream)


def write_json_new(path: Path, value) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def write_json_over(path: Path, value) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def date_nanos(day: dt.date) -> int:
    return (day - EPOCH).days * 86_400_000_000_000


def fwd(path: Path) -> str:
    return str(path).replace("\\", "/")


def log(message: str) -> None:
    print(message, flush=True)


# ------------------------------------------------------------ Windows peak working set
class _ProcessMemoryCountersEx(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.c_uint32), ("PageFaultCount", ctypes.c_uint32),
        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


def peak_memory(process: subprocess.Popen) -> dict | None:
    """Peak working set / pagefile of a (finished or running) child via psapi. None if unavailable."""
    if sys.platform != "win32":
        return None
    try:
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        query = psapi.GetProcessMemoryInfo
        query.argtypes = [ctypes.c_void_p, ctypes.POINTER(_ProcessMemoryCountersEx), ctypes.c_uint32]
        query.restype = ctypes.c_int
        counters = _ProcessMemoryCountersEx()
        counters.cb = ctypes.sizeof(counters)
        # CPython keeps the exact child handle alive until the Popen object is destroyed.
        if not query(int(process._handle), ctypes.byref(counters), counters.cb):
            return None
        return {"peak_working_set_bytes": int(counters.PeakWorkingSetSize),
                "peak_pagefile_bytes": int(counters.PeakPagefileUsage),
                "page_fault_count": int(counters.PageFaultCount)}
    except Exception:  # noqa: BLE001 - diagnostics only, never fatal
        return None


# ----------------------------------------------------------------------- child runner
def run_child(args: list[str], log_path: Path, cwd: Path) -> dict:
    """Run a child, capture stdout+stderr bytes to log_path (exclusive create), report stats."""
    started_utc = utc_now()
    started = time.perf_counter()
    with log_path.open("xb") as stream:
        process = subprocess.Popen(args, cwd=str(cwd), stdout=stream, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL)
        exit_code = process.wait()
        memory = peak_memory(process)
    wall = time.perf_counter() - started
    record = {"args": args, "exit_code": exit_code, "wall_seconds": round(wall, 3),
              "started_utc": started_utc, "ended_utc": utc_now(), "log": fwd(log_path),
              "peak_memory": memory}
    if memory is None:
        record["peak_memory_note"] = "GetProcessMemoryInfo unavailable; peak working set not recorded"
    return record


# ------------------------------------------------------------------------- per year
class YearPaths:
    def __init__(self, year: int, data_root: Path, stamp: str):
        self.year = year
        self.start = dt.date(year, 1, 1)
        self.end = dt.date(year, 12, 31)
        self.prepared = data_root / f"tickerhistory_training_{year}0101_{year}1231_{stamp}"
        self.native = data_root / f"tickerhistory_training_native_{year}_{stamp}"
        self.segments = self.native / "segments"
        self.manifest = self.prepared / "manifest.json"
        self.accepted = self.prepared / "accepted.zip"
        self.prepare_log = self.prepared / "prepare.log"
        self.prepare_log_staging = self.native / "prepare.log.partial"
        self.load_log = self.native / "load.log"
        self.ingestion_manifest = self.segments / "_ingestion.manifest.json"
        self.native_manifest = self.segments / "_manifest.json"


def build_commands(paths: YearPaths, ctx: dict) -> tuple[list[str], list[str]]:
    prepare_cmd = [ctx["python"], fwd(ctx["prepare_tool"]),
                   "--source", fwd(ctx["source_zip"]),
                   "--out-dir", fwd(paths.prepared),
                   "--start-date", paths.start.isoformat(),
                   "--end-date", paths.end.isoformat()]
    load_cmd = [fwd(ctx["atx_impl"]), "load",
                "--zip", fwd(paths.accepted),
                "--out", fwd(paths.segments),
                "--min-date", paths.start.isoformat(),
                "--preparation-manifest", fwd(paths.manifest)]
    return prepare_cmd, load_cmd


def check_prepared_manifest(paths: YearPaths, manifest: dict, source_sha: str) -> tuple[list[str], dict]:
    problems: list[str] = []
    counts = manifest.get("counts", {})
    if manifest.get("status") != "complete":
        problems.append(f"prepared manifest status={manifest.get('status')!r} != 'complete'")
    if manifest.get("policy_version") != POLICY_VERSION:
        problems.append(f"policy_version={manifest.get('policy_version')!r} != {POLICY_VERSION!r}")
    window = manifest.get("window", {})
    if window.get("start_inclusive") != paths.start.isoformat() or \
            window.get("end_inclusive") != paths.end.isoformat():
        problems.append(f"window {window} != [{paths.start}, {paths.end}]")
    source = manifest.get("source", {})
    if source.get("sha256") != source_sha:
        problems.append("prepared manifest source.sha256 does not match the hashed source ZIP")
    if source.get("crc_verified") is not True:
        problems.append("source.crc_verified is not true")
    if manifest.get("accepted", {}).get("rows_preserved_byte_for_byte") is not True:
        problems.append("accepted.rows_preserved_byte_for_byte is not true")
    daily = manifest.get("daily_counts", [])
    bad_dates = [d["date"] for d in daily
                 if not (paths.start <= dt.date.fromisoformat(d["date"]) <= paths.end)]
    if bad_dates:
        problems.append(f"daily_counts dates outside window: {bad_dates[:5]}")
    if any(dt.date.fromisoformat(d["date"]) >= CUTOFF for d in daily):
        problems.append("daily_counts contains a date >= 2020-01-01")
    summary = {
        "policy_version": manifest.get("policy_version"), "status": manifest.get("status"),
        "window": window, "counts": counts,
        "primary_rejection_counts": manifest.get("primary_rejection_counts"),
        "overlapping_reason_counts": manifest.get("overlapping_reason_counts"),
        "nonrejecting_flags": manifest.get("nonrejecting_flags"),
        "duplicate_positive_keys": counts.get("duplicate_positive_keys", 0),
        "duplicate_key_dates_quarantined": [d["date"] for d in daily
                                           if d.get("duplicate_positive_keys", 0) > 0],
        "daily_dates": len(daily),
        "daily_dates_with_accepted_rows": sum(1 for d in daily if d.get("accepted", 0) > 0),
        "first_date": daily[0]["date"] if daily else None,
        "last_date": daily[-1]["date"] if daily else None,
        "conflict_fixture": manifest.get("conflict_fixture"),
        "accepted": manifest.get("accepted"),
    }
    return problems, summary


def check_native(paths: YearPaths, prepared_summary: dict, accepted_sha: str) -> tuple[list[str], dict]:
    problems: list[str] = []
    ingestion = read_json(paths.ingestion_manifest)
    native = read_json(paths.native_manifest)
    recipe = json.loads(ingestion["recipe"]) if isinstance(ingestion.get("recipe"), str) \
        else ingestion.get("recipe", {})
    if ingestion.get("schema") != INGESTION_SCHEMA:
        problems.append(f"ingestion schema={ingestion.get('schema')!r}")
    if ingestion.get("status") != "complete":
        problems.append(f"ingestion status={ingestion.get('status')!r}")
    prep = ingestion.get("preparation") or {}
    if prep.get("policy_version") != POLICY_VERSION:
        problems.append("ingestion.preparation.policy_version missing/mismatch (was --preparation-manifest honoured?)")
    if prep.get("accepted_sha256") != accepted_sha:
        problems.append("ingestion.preparation.accepted_sha256 != accepted.zip sha256")
    if ingestion.get("input", {}).get("sha256") != accepted_sha:
        problems.append("ingestion.input.sha256 != accepted.zip sha256")
    if recipe.get("version") != LOAD_RECIPE_VERSION:
        problems.append(f"recipe.version={recipe.get('version')!r}")
    if str(recipe.get("min_date_nanos")) != str(date_nanos(paths.start)):
        problems.append(f"recipe.min_date_nanos={recipe.get('min_date_nanos')} != {date_nanos(paths.start)}")
    if recipe.get("exclude_no_sector") is not False:
        problems.append("recipe.exclude_no_sector is not false")
    accepted_rows = int(prepared_summary["counts"].get("accepted_rows", -1))
    for name, doc in (("recipe", recipe), ("_manifest", native)):
        if int(doc.get("rows_malformed", -1)) != 0:
            problems.append(f"{name}.rows_malformed={doc.get('rows_malformed')} != 0")
        if int(doc.get("rows_filtered", -1)) != 0:
            problems.append(f"{name}.rows_filtered={doc.get('rows_filtered')} != 0")
        if int(doc.get("rows_kept", -1)) != accepted_rows:
            problems.append(f"{name}.rows_kept={doc.get('rows_kept')} != accepted_rows={accepted_rows}")
        if int(doc.get("rows_read", -1)) != accepted_rows:
            problems.append(f"{name}.rows_read={doc.get('rows_read')} != accepted_rows={accepted_rows}")
    if int(recipe.get("dates_written", -1)) != int(native.get("dates_written", -2)):
        problems.append("dates_written differs between recipe and _manifest.json")
    if int(native.get("dates_written", -1)) != prepared_summary["daily_dates_with_accepted_rows"]:
        problems.append(f"dates_written={native.get('dates_written')} != prepared dates with accepted rows="
                        f"{prepared_summary['daily_dates_with_accepted_rows']}")
    seg_names = sorted(entry["filename"] for entry in ingestion.get("segments", []))
    on_disk = sorted(p.name for p in paths.segments.glob("*.seg"))
    if seg_names != on_disk:
        problems.append(f"segment list in ingestion manifest ({len(seg_names)}) != *.seg on disk ({len(on_disk)})")
    if len(seg_names) != int(native.get("dates_written", -1)):
        problems.append("segment count != dates_written")
    seg_dates = []
    for name in seg_names:
        try:
            seg_dates.append(dt.date.fromisoformat(name[:-4]))
        except ValueError:
            problems.append(f"segment name not a date: {name}")
    out_of_year = [d.isoformat() for d in seg_dates if not (paths.start <= d <= paths.end)]
    if out_of_year:
        problems.append(f"segments outside calendar year: {out_of_year[:5]}")
    if any(d >= CUTOFF for d in seg_dates):
        problems.append("segment date >= 2020-01-01")
    summary = {
        "schema": ingestion.get("schema"), "status": ingestion.get("status"),
        "preparation": prep, "recipe": recipe, "native_manifest": native,
        "segments_count": len(seg_names),
        "first_segment": seg_names[0] if seg_names else None,
        "last_segment": seg_names[-1] if seg_names else None,
        "dates_written": native.get("dates_written"),
        "distinct_securities": native.get("distinct_securities"),
        "rows_read": native.get("rows_read"), "rows_kept": native.get("rows_kept"),
        "rows_filtered": native.get("rows_filtered"), "rows_malformed": native.get("rows_malformed"),
        "fields": native.get("fields"),
    }
    return problems, summary


def process_year(year: int, ctx: dict) -> dict:
    paths = YearPaths(year, ctx["data_root"], ctx["stamp"])
    prepare_cmd, load_cmd = build_commands(paths, ctx)
    record: dict = {
        "year": year, "window": {"start_inclusive": paths.start.isoformat(),
                                 "end_inclusive": paths.end.isoformat()},
        "prepared_dir": fwd(paths.prepared), "native_dir": fwd(paths.native),
        "segments_dir": fwd(paths.segments),
        "prepare_command": prepare_cmd, "load_command": load_cmd,
        "started_utc": utc_now(), "accepted": False, "reasons": [], "manifest_sha256": {},
    }
    reasons: list[str] = record["reasons"]
    log(f"== year {year}")
    log("  prepare: " + subprocess.list2cmdline(prepare_cmd))
    log("  load:    " + subprocess.list2cmdline(load_cmd))

    if paths.prepared.exists():
        reasons.append(f"refusing: prepared output dir exists: {fwd(paths.prepared)}")
    if paths.native.exists():
        reasons.append(f"refusing: native output dir exists: {fwd(paths.native)}")
    if reasons:
        record["ended_utc"] = utc_now()
        return record
    if ctx["dry_run"]:
        reasons.append("dry-run: nothing executed")
        record["ended_utc"] = utc_now()
        return record

    year_started = time.perf_counter()
    try:
        # 1. Source ZIP hash before this year's work.
        sha_before = sha256_file(ctx["source_zip"])
        record["source_zip_sha256_before"] = sha_before
        if sha_before != EXPECTED_SOURCE_SHA256:
            reasons.append(f"source ZIP sha256 before run {sha_before} != expected {EXPECTED_SOURCE_SHA256}")
            return record

        # 2. Own the native dir (holds logs); prepare creates its own dir (exist_ok=False).
        paths.native.mkdir(parents=True, exist_ok=False)

        # 3. prepare_tickerhistory.py
        log(f"  running prepare for {year} ...")
        prep = run_child(prepare_cmd, paths.prepare_log_staging, ctx["repo_root"])
        record["prepare"] = prep
        if paths.prepared.is_dir():
            shutil.move(str(paths.prepare_log_staging), str(paths.prepare_log))
            prep["log"] = fwd(paths.prepare_log)
        log(f"  prepare exit={prep['exit_code']} wall={prep['wall_seconds']}s "
            f"peak_ws={(prep.get('peak_memory') or {}).get('peak_working_set_bytes')}")
        if prep["exit_code"] != 0:
            reasons.append(f"prepare exit code {prep['exit_code']}")
            failure = paths.prepared / "failure.json"
            if failure.exists():
                record["prepare_failure"] = read_json(failure)
            return record
        if not paths.manifest.exists() or not paths.accepted.exists():
            reasons.append("prepare produced no manifest.json/accepted.zip")
            return record
        record["manifest_sha256"]["prepared_manifest"] = sha256_file(paths.manifest)
        accepted_sha = sha256_file(paths.accepted)
        record["accepted_zip_sha256"] = accepted_sha
        record["accepted_zip_size_bytes"] = paths.accepted.stat().st_size
        manifest = read_json(paths.manifest)
        problems, prepared_summary = check_prepared_manifest(paths, manifest, sha_before)
        if manifest.get("accepted", {}).get("sha256") != accepted_sha:
            problems.append("manifest.accepted.sha256 != hashed accepted.zip")
        record["prepared"] = prepared_summary
        reasons.extend(problems)
        if problems:
            return record

        # 4. atx-impl load
        log(f"  running atx-impl load for {year} ...")
        load = run_child(load_cmd, paths.load_log, ctx["repo_root"])
        record["load"] = load
        log(f"  load exit={load['exit_code']} wall={load['wall_seconds']}s "
            f"peak_ws={(load.get('peak_memory') or {}).get('peak_working_set_bytes')}")
        if load["exit_code"] != 0:
            reasons.append(f"atx-impl load exit code {load['exit_code']}")

        # 5. Source ZIP hash after this year's work.
        sha_after = sha256_file(ctx["source_zip"])
        record["source_zip_sha256_after"] = sha_after
        if sha_after != sha_before:
            reasons.append("source ZIP sha256 changed during the run")
        if load["exit_code"] != 0:
            return record

        # 6. Native manifests + assertions.
        for key, path in (("ingestion_manifest", paths.ingestion_manifest),
                          ("native_manifest", paths.native_manifest)):
            if path.exists():
                record["manifest_sha256"][key] = sha256_file(path)
            else:
                reasons.append(f"missing {fwd(path)}")
        if any(r.startswith("missing ") for r in reasons):
            return record
        problems, native_summary = check_native(paths, prepared_summary, accepted_sha)
        record["native"] = native_summary
        reasons.extend(problems)
        record["accepted"] = not reasons
    except Exception as error:  # noqa: BLE001 - recorded, never aborts the receipt
        reasons.append(f"exception: {type(error).__name__}: {error}")
    finally:
        record["wall_seconds"] = round(time.perf_counter() - year_started, 3)
        record["ended_utc"] = utc_now()
    return record


# ------------------------------------------------------------------------------ main
def next_receipt_path(stamp: str) -> Path:
    attempt = 1
    while True:
        candidate = AUDITS_DIR / f"iteration15-ingest-{stamp}-attempt{attempt}.json"
        if not candidate.exists():
            return candidate
        attempt += 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prepare + natively ingest calendar-year tickerhistory windows.")
    parser.add_argument("--year", action="append", type=int, dest="years",
                        help="calendar year (repeatable); default: 2014..2019")
    parser.add_argument("--dry-run", action="store_true",
                        help="print commands/paths, verify inputs and hashes, write nothing")
    parser.add_argument("--atx-impl", default=DEFAULT_ATX_IMPL)
    parser.add_argument("--data-root", default=DEFAULT_DATA_ROOT)
    parser.add_argument("--stamp", default=DEFAULT_STAMP)
    parser.add_argument("--source-zip", default=DEFAULT_SOURCE_ZIP)
    parser.add_argument("--prepare-tool", default=DEFAULT_PREPARE_TOOL)
    parser.add_argument("--stop-on-failure", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    years = args.years or list(DEFAULT_YEARS)
    for year in years:
        if dt.date(year, 12, 31) >= CUTOFF or year < 2012:
            raise SystemExit(f"refusing year {year}: window must lie entirely before 2020-01-01 (and >= 2012)")
    ctx = {
        "python": fwd(Path(sys.executable)), "atx_impl": Path(args.atx_impl),
        "data_root": Path(args.data_root), "stamp": args.stamp, "source_zip": Path(args.source_zip),
        "prepare_tool": Path(args.prepare_tool), "dry_run": args.dry_run,
        "repo_root": Path(args.prepare_tool).resolve().parents[2],
    }
    started_utc = utc_now()
    receipt: dict = {
        "schema": "atx.iteration15-tickerhistory-ingest-v1", "status": "started",
        "started_utc": started_utc, "stamp": args.stamp, "dry_run": args.dry_run,
        "years_requested": years, "python_version": sys.version, "python_executable": ctx["python"],
        "platform": platform.platform(), "argv": argv, "driver": fwd(Path(__file__).resolve()),
        "inputs": {}, "years": [], "accepted": False, "reasons": [],
    }
    global_reasons: list[str] = receipt["reasons"]

    # Inputs: existence + hashes.
    for key, path in (("atx_impl", ctx["atx_impl"]), ("prepare_tool", ctx["prepare_tool"]),
                      ("source_zip", ctx["source_zip"]), ("python", Path(sys.executable))):
        entry = {"path": fwd(path), "exists": path.is_file()}
        if entry["exists"]:
            entry["size_bytes"] = path.stat().st_size
            entry["sha256"] = sha256_file(path)
            entry["writable"] = os.access(path, os.W_OK)
        else:
            global_reasons.append(f"missing input {key}: {fwd(path)}")
        receipt["inputs"][key] = entry
        log(f"input {key}: {entry}")
    zip_entry = receipt["inputs"]["source_zip"]
    if zip_entry.get("exists"):
        receipt["inputs"]["source_zip"]["expected_sha256"] = EXPECTED_SOURCE_SHA256
        receipt["inputs"]["source_zip"]["expected_size_bytes"] = EXPECTED_SOURCE_SIZE
        if zip_entry["sha256"] != EXPECTED_SOURCE_SHA256 or zip_entry["size_bytes"] != EXPECTED_SOURCE_SIZE:
            global_reasons.append("source ZIP sha256/size does not match the 2013 preparation manifest")
        if zip_entry["writable"]:
            receipt["inputs"]["source_zip"]["note"] = ("source ZIP is not marked read-only (mode 0666 as in the "
                                                       "2013 run); integrity guarded by sha256 before/after")
    if not ctx["data_root"].is_dir():
        global_reasons.append(f"data root missing: {fwd(ctx['data_root'])}")

    receipt_path = None
    if not args.dry_run:
        receipt_path = next_receipt_path(args.stamp)
        write_json_new(receipt_path, receipt)  # reserve the attempt number exclusively
        receipt["receipt_path"] = fwd(receipt_path)
        log(f"receipt reserved: {fwd(receipt_path)}")

    try:
        if global_reasons:
            log("input verification failed; no year will run: " + "; ".join(global_reasons))
        for year in years:
            if global_reasons and not args.dry_run:
                break
            record = process_year(year, ctx)
            receipt["years"].append(record)
            log(f"  year {year} accepted={record['accepted']} reasons={record['reasons']}")
            if not record["accepted"] and args.stop_on_failure and not args.dry_run:
                global_reasons.append(f"stopped after year {year} failure (--stop-on-failure)")
                break
    finally:
        receipt["ended_utc"] = utc_now()
        ran = [r for r in receipt["years"]]
        receipt["accepted"] = (not global_reasons and bool(ran) and len(ran) == len(years)
                               and all(r["accepted"] for r in ran) and not args.dry_run)
        receipt["status"] = "dry-run" if args.dry_run else ("complete" if receipt["accepted"] else "failed")
        if args.dry_run:
            log(json.dumps(receipt, indent=2, sort_keys=True))
        else:
            write_json_over(receipt_path, receipt)
            log(f"receipt written: {fwd(receipt_path)} accepted={receipt['accepted']}")
    return 0 if (receipt["accepted"] or (args.dry_run and not global_reasons)) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
