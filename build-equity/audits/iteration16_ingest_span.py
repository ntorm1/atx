"""Iteration 16: prepare + natively ingest ONE multi-year tickerhistory span.

Derived from iteration15_ingest_tickerhistory.py (untouched; not imported -- helpers
are copied so the two stay independent). Checkpoint 16 needs a single prepared +
natively loaded span (default 2012-03-26..2019-12-31) because `atx-impl panel` takes
one `--segs` dir and iteration15's per-calendar-year native dirs each bind to a
one-year accepted.zip and cannot be merged. Same prepare -> load -> verify -> receipt
recipe as iteration15 (see that file for the full citation trail), generalised from a
fixed Jan1..Dec31 calendar year to one arbitrary [start-date, end-date] window (no --year loop).

  <python> atx-engine/tools/prepare_tickerhistory.py --source <zip>
      --out-dir <data-root>/tickerhistory_training_<start:%Y%m%d>_<end:%Y%m%d>_<stamp>[_attemptN]
      --start-date <start> --end-date <end>
  <atx-impl.exe> load --zip <prepared>/accepted.zip
      --out <data-root>/tickerhistory_training_native_<label>_<stamp>[_attemptN]/segments
      --min-date <start> --preparation-manifest <prepared>/manifest.json

load has no max-date flag; the window upper bound is enforced entirely by prepare
(accepted.zip contains only in-window rows), so rows_kept must equal accepted_rows.
--min-date is the window start (recipe.min_date_nanos == start_inclusive).
Stdlib only. Never overwrites: refuses (exit non-zero, nothing executed) if the
prepared dir, native dir, or receipt path already exists.
"""
from __future__ import annotations

import argparse, ctypes, datetime as dt, hashlib, json, os, platform, shutil, subprocess, sys, time
from pathlib import Path

CUTOFF = dt.date(2020, 1, 1)
DEFAULT_ATX_IMPL = "C:/atx/.worktrees/equity-platform/build-equity/bin/atx-impl.exe"
DEFAULT_DATA_ROOT = "C:/atx/data"
DEFAULT_STAMP = "20260920"
DEFAULT_SOURCE_ZIP = "C:/Users/natha/Downloads/tbltickerhistory3_10y.zip"
DEFAULT_PREPARE_TOOL = "C:/atx/.worktrees/equity-platform/atx-engine/tools/prepare_tickerhistory.py"
DEFAULT_START_DATE = "2012-03-26"
DEFAULT_END_DATE = "2019-12-31"
DEFAULT_LABEL = "2012_2019"
AUDITS_DIR = Path("C:/atx/.worktrees/equity-platform/build-equity/audits")
EXPECTED_SOURCE_SHA256 = "7d2b7a615a08ef3b8686608eda384c4e30e2ce23bdee294ea141b3c9334acd8f"
EXPECTED_SOURCE_SIZE = 3542506361
POLICY_VERSION = "tickerhistory-qa-v1"
LOAD_RECIPE_VERSION = "tickerhistory-native-load-v1"
INGESTION_SCHEMA = "atx-ingestion-v1"
EPOCH = dt.date(1970, 1, 1)

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

def date_nanos(day: dt.date) -> int:
    return (day - EPOCH).days * 86_400_000_000_000

def fwd(path: Path) -> str:
    return str(path).replace("\\", "/")

def log(message: str) -> None:
    print(message, flush=True)

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

class SpanPaths:
    def __init__(self, start: dt.date, end: dt.date, label: str, stamp: str, attempt: int, data_root: Path):
        self.start, self.end, self.label = start, end, label
        suffix = "" if attempt <= 1 else f"_attempt{attempt}"
        self.prepared = data_root / (f"tickerhistory_training_{start:%Y%m%d}_{end:%Y%m%d}_{stamp}" + suffix)
        self.native = data_root / (f"tickerhistory_training_native_{label}_{stamp}" + suffix)
        self.segments = self.native / "segments"
        self.manifest, self.accepted = self.prepared / "manifest.json", self.prepared / "accepted.zip"
        self.prepare_log = self.prepared / "prepare.log"
        self.prepare_log_staging = self.native / "prepare.log.partial"
        self.load_log = self.native / "load.log"
        self.ingestion_manifest = self.segments / "_ingestion.manifest.json"
        self.native_manifest = self.segments / "_manifest.json"

def build_commands(paths: SpanPaths, ctx: dict) -> tuple[list[str], list[str]]:
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

def check_source_zip(path: Path, dry_run: bool) -> tuple[list[str], dict]:
    """Existence + size + sha256 vs. the 2013-recipe source ZIP. sha256 is only ever
    computed when the size already matches (no point hashing a mismatched file), and is
    additionally skipped in dry-run mode (the source is ~3.5 GB; hashing it is slow)."""
    problems: list[str] = []
    entry = {"path": fwd(path), "exists": path.is_file()}
    if not entry["exists"]:
        problems.append(f"missing source zip: {fwd(path)}")
        return problems, entry
    entry["size_bytes"] = path.stat().st_size
    entry["expected_size_bytes"], entry["expected_sha256"] = EXPECTED_SOURCE_SIZE, EXPECTED_SOURCE_SHA256
    entry["writable"] = os.access(path, os.W_OK)
    if entry["size_bytes"] != EXPECTED_SOURCE_SIZE:
        problems.append(f"source zip size {entry['size_bytes']} != expected {EXPECTED_SOURCE_SIZE}")
        entry["sha256"], entry["sha256_note"] = None, "sha256 not computed: size already mismatched"
        return problems, entry
    if dry_run:
        entry["sha256"] = None
        entry["sha256_note"] = "sha256 skipped in dry-run (size matches expected; ~3.5 GB hash skipped)"
        return problems, entry
    entry["sha256"] = sha256_file(path)
    if entry["sha256"] != EXPECTED_SOURCE_SHA256:
        problems.append(f"source zip sha256 {entry['sha256']} != expected {EXPECTED_SOURCE_SHA256}")
    return problems, entry

def check_prepared_manifest(paths: SpanPaths, manifest: dict, source_sha: str) -> tuple[list[str], dict]:
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
    if source.get("crc_verified") is not True: problems.append("source.crc_verified is not true")
    if manifest.get("accepted", {}).get("rows_preserved_byte_for_byte") is not True:
        problems.append("accepted.rows_preserved_byte_for_byte is not true")
    daily = manifest.get("daily_counts", [])
    bad_dates = [d["date"] for d in daily if not (paths.start <= dt.date.fromisoformat(d["date"]) <= paths.end)]
    if bad_dates: problems.append(f"daily_counts dates outside window: {bad_dates[:5]}")
    if any(dt.date.fromisoformat(d["date"]) >= CUTOFF for d in daily):
        problems.append("daily_counts contains a date >= 2020-01-01")
    summary = {
        "policy_version": manifest.get("policy_version"), "status": manifest.get("status"),
        "window": window, "counts": counts,
        "primary_rejection_counts": manifest.get("primary_rejection_counts"),
        "overlapping_reason_counts": manifest.get("overlapping_reason_counts"),
        "nonrejecting_flags": manifest.get("nonrejecting_flags"),
        "duplicate_positive_keys": counts.get("duplicate_positive_keys", 0),
        "duplicate_key_dates_quarantined": [d["date"] for d in daily if d.get("duplicate_positive_keys", 0) > 0],
        "daily_dates": len(daily), "conflict_fixture": manifest.get("conflict_fixture"),
        "daily_dates_with_accepted_rows": sum(1 for d in daily if d.get("accepted", 0) > 0),
        "first_date": daily[0]["date"] if daily else None, "last_date": daily[-1]["date"] if daily else None,
        "accepted": manifest.get("accepted"),
    }
    return problems, summary

def check_native(paths: SpanPaths, prepared_summary: dict, accepted_sha: str) -> tuple[list[str], dict]:
    problems: list[str] = []
    ingestion = read_json(paths.ingestion_manifest)
    native = read_json(paths.native_manifest)
    recipe = json.loads(ingestion["recipe"]) if isinstance(ingestion.get("recipe"), str) \
        else ingestion.get("recipe", {})
    if ingestion.get("schema") != INGESTION_SCHEMA: problems.append(f"ingestion schema={ingestion.get('schema')!r}")
    if ingestion.get("status") != "complete": problems.append(f"ingestion status={ingestion.get('status')!r}")
    prep = ingestion.get("preparation") or {}
    if prep.get("policy_version") != POLICY_VERSION:
        problems.append("ingestion.preparation.policy_version missing/mismatch (was --preparation-manifest honoured?)")
    if prep.get("accepted_sha256") != accepted_sha:
        problems.append("ingestion.preparation.accepted_sha256 != accepted.zip sha256")
    if ingestion.get("input", {}).get("sha256") != accepted_sha:
        problems.append("ingestion.input.sha256 != accepted.zip sha256")
    if recipe.get("version") != LOAD_RECIPE_VERSION: problems.append(f"recipe.version={recipe.get('version')!r}")
    if str(recipe.get("min_date_nanos")) != str(date_nanos(paths.start)):
        problems.append(f"recipe.min_date_nanos={recipe.get('min_date_nanos')} != {date_nanos(paths.start)}")
    if recipe.get("exclude_no_sector") is not False: problems.append("recipe.exclude_no_sector is not false")
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
    if len(seg_names) != int(native.get("dates_written", -1)): problems.append("segment count != dates_written")
    seg_dates = []
    for name in seg_names:
        try:
            seg_dates.append(dt.date.fromisoformat(name[:-4]))
        except ValueError:
            problems.append(f"segment name not a date: {name}")
    out_of_window = [d.isoformat() for d in seg_dates if not (paths.start <= d <= paths.end)]
    if out_of_window: problems.append(f"segments outside span window: {out_of_window[:5]}")
    if any(d >= CUTOFF for d in seg_dates): problems.append("segment date >= 2020-01-01")
    summary = {
        "schema": ingestion.get("schema"), "status": ingestion.get("status"),
        "preparation": prep, "recipe": recipe, "native_manifest": native,
        "segments_count": len(seg_names),
        "first_segment": seg_names[0] if seg_names else None, "last_segment": seg_names[-1] if seg_names else None,
        "dates_written": native.get("dates_written"), "distinct_securities": native.get("distinct_securities"),
        "rows_read": native.get("rows_read"), "rows_kept": native.get("rows_kept"),
        "rows_filtered": native.get("rows_filtered"), "rows_malformed": native.get("rows_malformed"),
        "fields": native.get("fields"),
    }
    return problems, summary

def receipt_path_for(label: str, attempt: int, dry_run: bool) -> Path:
    suffix = "-dryrun" if dry_run else ""
    return AUDITS_DIR / f"iteration16-ingest-span-{label}-attempt{attempt}{suffix}.json"

def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prepare + natively ingest one span tickerhistory window.")
    parser.add_argument("--start-date", default=DEFAULT_START_DATE)
    parser.add_argument("--end-date", default=DEFAULT_END_DATE)
    parser.add_argument("--label", default=DEFAULT_LABEL)
    parser.add_argument("--stamp", default=DEFAULT_STAMP)
    parser.add_argument("--attempt", type=int, default=1)
    parser.add_argument("--dry-run", action="store_true",
                        help="print commands/paths, verify inputs (sha skipped), write nothing")
    parser.add_argument("--atx-impl", default=DEFAULT_ATX_IMPL)
    parser.add_argument("--data-root", default=DEFAULT_DATA_ROOT)
    parser.add_argument("--source-zip", default=DEFAULT_SOURCE_ZIP)
    parser.add_argument("--prepare-tool", default=DEFAULT_PREPARE_TOOL)
    return parser.parse_args(argv)

def main(argv: list[str]) -> int:
    all_started = time.perf_counter()

    def phase(name: str) -> None:
        log(f"== {name} elapsed={round(time.perf_counter() - all_started, 3)}s")

    args = parse_args(argv)
    try:
        start = dt.date.fromisoformat(args.start_date)
        end = dt.date.fromisoformat(args.end_date)
    except ValueError as error:
        log(f"refusing: bad --start-date/--end-date: {error}")
        return 1
    if start > end:
        log(f"refusing: start-date {start} is after end-date {end}")
        return 1
    if end >= CUTOFF or start.year < 2012:
        log(f"refusing span [{start},{end}]: window must lie entirely before 2020-01-01 (and >= 2012)")
        return 1
    if args.attempt < 1:
        log("refusing: --attempt must be >= 1")
        return 1

    ctx = {
        "python": fwd(Path(sys.executable)), "atx_impl": Path(args.atx_impl),
        "data_root": Path(args.data_root), "stamp": args.stamp, "source_zip": Path(args.source_zip),
        "prepare_tool": Path(args.prepare_tool), "dry_run": args.dry_run,
        "repo_root": Path(args.prepare_tool).resolve().parents[2],
    }
    paths = SpanPaths(start, end, args.label, args.stamp, args.attempt, ctx["data_root"])
    receipt_path = receipt_path_for(args.label, args.attempt, args.dry_run)
    prepare_cmd, load_cmd = build_commands(paths, ctx)

    phase("input verification")
    log("  prepare: " + subprocess.list2cmdline(prepare_cmd))
    log("  load:    " + subprocess.list2cmdline(load_cmd))
    log(f"  prepared_dir: {fwd(paths.prepared)}")
    log(f"  native_dir:   {fwd(paths.native)}")

    # Never overwrite the receipt ("x" mode below); refuse before touching anything else.
    if receipt_path.exists():
        log(f"refusing: receipt already exists: {fwd(receipt_path)}; nothing executed")
        return 1
    problems: list[str] = []
    if paths.prepared.exists(): problems.append(f"refusing: prepared output dir exists: {fwd(paths.prepared)}")
    if paths.native.exists(): problems.append(f"refusing: native output dir exists: {fwd(paths.native)}")

    receipt: dict = {
        "schema": "atx.iteration16-tickerhistory-ingest-span-v1",
        "status": "started", "started_utc": utc_now(), "label": args.label, "stamp": args.stamp,
        "attempt": args.attempt, "dry_run": args.dry_run,
        "window": {"start_inclusive": start.isoformat(), "end_inclusive": end.isoformat()},
        "prepared_dir": fwd(paths.prepared), "native_dir": fwd(paths.native), "segments_dir": fwd(paths.segments),
        "prepare_command": prepare_cmd, "load_command": load_cmd,
        "python_version": sys.version, "python_executable": ctx["python"], "platform": platform.platform(),
        "argv": argv, "driver": fwd(Path(__file__).resolve()), "receipt_path": fwd(receipt_path),
        "inputs": {}, "accepted": False, "problems": problems,
    }

    for key, path in (("atx_impl", ctx["atx_impl"]), ("prepare_tool", ctx["prepare_tool"]), ("python", Path(sys.executable))):
        entry = {"path": fwd(path), "exists": path.is_file()}
        if not entry["exists"]: problems.append(f"missing input {key}: {fwd(path)}")
        receipt["inputs"][key] = entry
    src_problems, src_entry = check_source_zip(ctx["source_zip"], args.dry_run)
    receipt["inputs"]["source_zip"] = src_entry
    problems.extend(src_problems)
    if not ctx["data_root"].is_dir(): problems.append(f"data root missing: {fwd(ctx['data_root'])}")

    if args.dry_run or problems:
        log(("problems found; nothing executed: " + "; ".join(problems)) if problems else "dry-run: nothing executed")
        receipt["ended_utc"] = utc_now()
        receipt["status"] = "dry-run" if args.dry_run else "failed"
        receipt["accepted"] = False
        write_json_new(receipt_path, receipt)
        log(f"receipt written: {fwd(receipt_path)}")
        return 0 if (args.dry_run and not problems) else 1

    try:
        phase("prepare")
        paths.native.mkdir(parents=True, exist_ok=False)
        sha_before = sha256_file(ctx["source_zip"])
        receipt["source_zip_sha256_before"] = sha_before
        if sha_before != EXPECTED_SOURCE_SHA256:
            problems.append(f"source ZIP sha256 before run {sha_before} != expected {EXPECTED_SOURCE_SHA256}")
            raise RuntimeError("source integrity check failed before prepare")

        prep = run_child(prepare_cmd, paths.prepare_log_staging, ctx["repo_root"])
        receipt["prepare"] = prep
        if paths.prepared.is_dir():
            shutil.move(str(paths.prepare_log_staging), str(paths.prepare_log))
            prep["log"] = fwd(paths.prepare_log)
        log(f"  prepare exit={prep['exit_code']} wall={prep['wall_seconds']}s "
            f"peak_ws={(prep.get('peak_memory') or {}).get('peak_working_set_bytes')}")
        if prep["exit_code"] != 0:
            problems.append(f"prepare exit code {prep['exit_code']}")
            failure = paths.prepared / "failure.json"
            if failure.exists():
                receipt["prepare_failure"] = read_json(failure)
            raise RuntimeError("prepare failed")
        if not paths.manifest.exists() or not paths.accepted.exists():
            problems.append("prepare produced no manifest.json/accepted.zip")
            raise RuntimeError("prepare output missing")

        manifest_sha256 = {"prepared_manifest": sha256_file(paths.manifest)}
        receipt["manifest_sha256"] = manifest_sha256
        accepted_sha = sha256_file(paths.accepted)
        receipt["accepted_zip_sha256"] = accepted_sha
        receipt["accepted_zip_size_bytes"] = paths.accepted.stat().st_size
        manifest = read_json(paths.manifest)
        prep_problems, prepared_summary = check_prepared_manifest(paths, manifest, sha_before)
        if manifest.get("accepted", {}).get("sha256") != accepted_sha:
            prep_problems.append("manifest.accepted.sha256 != hashed accepted.zip")
        receipt["prepared"] = prepared_summary
        problems.extend(prep_problems)
        if prep_problems:
            raise RuntimeError("prepared manifest cross-checks failed")

        phase("load")
        load = run_child(load_cmd, paths.load_log, ctx["repo_root"])
        receipt["load"] = load
        log(f"  load exit={load['exit_code']} wall={load['wall_seconds']}s "
            f"peak_ws={(load.get('peak_memory') or {}).get('peak_working_set_bytes')}")
        if load["exit_code"] != 0:
            problems.append(f"atx-impl load exit code {load['exit_code']}")

        sha_after = sha256_file(ctx["source_zip"])
        receipt["source_zip_sha256_after"] = sha_after
        if sha_after != sha_before:
            problems.append("source ZIP sha256 changed during the run")
        if load["exit_code"] != 0:
            raise RuntimeError("load failed")

        phase("verify")
        for key, path in (("ingestion_manifest", paths.ingestion_manifest),
                          ("native_manifest", paths.native_manifest)):
            if path.exists():
                manifest_sha256[key] = sha256_file(path)
            else:
                problems.append(f"missing {fwd(path)}")
        if any(p.startswith("missing ") for p in problems):
            raise RuntimeError("native manifests missing")
        native_problems, native_summary = check_native(paths, prepared_summary, accepted_sha)
        receipt["native"] = native_summary
        problems.extend(native_problems)
        receipt["accepted"] = not problems
    except Exception as error:  # noqa: BLE001 - recorded, never aborts the receipt
        if not any(str(error) == p for p in problems):
            problems.append(f"exception: {type(error).__name__}: {error}")
    finally:
        phase("receipt")
        receipt["problems"] = problems
        receipt["accepted"] = bool(receipt.get("accepted")) and not problems
        receipt["status"] = "complete" if receipt["accepted"] else "failed"
        receipt["ended_utc"] = utc_now()
        receipt["wall_seconds"] = round(time.perf_counter() - all_started, 3)
        write_json_new(receipt_path, receipt)
        log(f"receipt written: {fwd(receipt_path)} accepted={receipt['accepted']}")

    return 0 if receipt["accepted"] else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
