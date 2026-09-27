"""Run a ledgered batch stage in guarded slices (tier-1 v2 1.3; ``atx_db.batch_runner``).

Operator form (the orchestrator itself runs under the memory guard in orchestrator mode):

    run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 -- \\
      python scripts/run_slices.py --db <warehouse> --stage bars_unit_correction --run-key <key> \\
        [--spec-json <file>] --job-gb 0.8 --max-batches 8 --max-minutes 10 \\
        [--until-complete --sleep 60] [--finalize] [--heavy] [--abandon REASON]

Every slice is a FRESH guarded subprocess (``run_memory_guarded.py --job-gb <x> --wait-minutes <w>
-- python run_slices.py ... --single-slice``), started outside the orchestrator's job
(CREATE_BREAKAWAY_FROM_JOB, allowed by ``--allow-nested-guards``) with ``--parent-job`` so it stops
when the orchestrator's guard does. A slice opens the run (the first one plans and prepares it),
builds at most ``--max-batches`` batches or ``--max-minutes``, CHECKPOINTs and exits, releasing
the file. With ``--until-complete`` the orchestrator repeats: a guard refusal (exit 78: not
admitted within ``--wait-minutes``) or a guard stop (exit 137) sleeps ``--sleep`` seconds and
retries (the stopped slice resumes from the ledger); any other failure stops the orchestrator
with that exit code. After the last batch, ``--finalize`` runs ONE finalize slice (validate, swap,
CHECKPOINT, no-WAL assert). ``--abandon REASON`` runs one slice that marks the open run abandoned
and drops its staging tables.

One JSON line per slice is appended to ``<receipts-dir>/<stage>-<run_key>.jsonl`` (admitted or
refused, guard status and peak, cap hit, wait, batches committed, remaining, the finalize
receipt, exit code); guard receipts, slice results and logs go to
``<receipts-dir>/<stage>-<run_key>/``. The orchestrator imports neither DuckDB nor ``atx_db``.

``--single-slice`` (internal; also usable by hand inside a guard) runs one slice in-process and
prints JSON events (``batch_begin``/``batch_committed``) and a result line; ``--result-json``
writes the result. Exit codes: 0 ok, 1 slice error, 70 not under the memory guard, 78/137
propagated from a slice's guard, 3 ``--max-slices`` reached.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import subprocess
import sys
import time
import traceback
from pathlib import Path
from typing import Any

_SCRIPT = Path(__file__).resolve()
_SRC = _SCRIPT.parents[1] / "src"
CANONICAL_RECEIPTS_DIR = Path(r"C:\atx\.superpowers\sdd\tier1-v2\receipts")
_GUARD_RELATIVE = Path(".superpowers") / "sdd" / "tier1-parity" / "run_memory_guarded.py"
_CANONICAL_GUARD = Path(r"C:\atx") / _GUARD_RELATIVE
GUARD_JOB_ENV = "ATX_GUARD_JOB"
EXIT_SLICE_ERROR = 1
EXIT_MAX_SLICES = 3
EXIT_UNGUARDED = 70
EXIT_NOT_ADMITTED = 78
EXIT_STOPPED = 137
RETRY_CODES = (EXIT_NOT_ADMITTED, EXIT_STOPPED)


def _utc() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="seconds")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--run-key", required=True)
    parser.add_argument("--spec-json", type=Path, help="JSON object: the run's spec (default {})")
    parser.add_argument("--job-gb", type=float, default=0.8, help="guard cap of each slice (<= 1.0)")
    parser.add_argument("--wait-minutes", type=float, default=30.0, help="guard admission wait of each slice")
    parser.add_argument("--heavy", action="store_true", help="slices run as the HEAVY job (token holder only)")
    parser.add_argument("--max-batches", type=int, default=8)
    parser.add_argument("--max-minutes", type=float, default=10.0)
    parser.add_argument("--checkpoint-every", type=int, default=4)
    parser.add_argument("--recycle-every", type=int, default=4)
    parser.add_argument("--duckdb-memory", help="override the stage's DuckDB memory_limit (one step down, M8)")
    parser.add_argument("--threads", type=int, help="override the stage's DuckDB threads")
    parser.add_argument("--until-complete", action="store_true")
    parser.add_argument("--sleep", type=float, default=60.0, help="seconds to sleep after a refusal or guard stop")
    parser.add_argument("--finalize", action="store_true", help="publish after the last batch (one finalize slice)")
    parser.add_argument("--abandon", metavar="REASON", help="mark the open run abandoned and drop its staging")
    parser.add_argument("--max-slices", type=int, default=200)
    parser.add_argument("--receipts-dir", type=Path, default=CANONICAL_RECEIPTS_DIR)
    parser.add_argument("--code-sha", help="git sha recorded on the run (default: git rev-parse HEAD, if any)")
    parser.add_argument("--single-slice", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--phase", choices=("batches", "finalize", "abandon"), default="batches",
                        help=argparse.SUPPRESS)
    parser.add_argument("--result-json", type=Path, help=argparse.SUPPRESS)
    return parser


def _code_sha() -> str | None:
    try:
        found = subprocess.run(["git", "-C", str(_SCRIPT.parent), "rev-parse", "HEAD"], capture_output=True,
                               text=True, timeout=30, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    sha = found.stdout.strip()
    return sha if found.returncode == 0 and len(sha) == 40 else None


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(value, indent=1, default=str) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _read_json(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


# ---------------------------------------------------------------------------
# One slice, in-process (inside a guard)
# ---------------------------------------------------------------------------

def _wal_bytes(db: Path) -> int | None:
    wal = Path(str(db) + ".wal")
    return wal.stat().st_size if wal.exists() else None


def single_slice(args: argparse.Namespace) -> int:
    if not os.environ.get(GUARD_JOB_ENV):
        print(json.dumps({"error": "a slice runs only under run_memory_guarded.py (ATX_GUARD_JOB unset)"}),
              flush=True)
        return EXIT_UNGUARDED
    if str(_SRC) not in sys.path:
        sys.path.insert(0, str(_SRC))  # an export runs its own code, not the editable install
    from atx_db import batch_runner as br

    def emit(event: dict[str, object]) -> None:
        print(json.dumps(event, default=str), flush=True)

    spec = json.loads(args.spec_json.read_text(encoding="utf-8")) if args.spec_json else {}
    if not isinstance(spec, dict):
        raise SystemExit("--spec-json must hold a JSON object")
    result: dict[str, Any] = {"stage": args.stage, "run_key": args.run_key, "phase": args.phase, "pid": os.getpid(),
                              "db": str(args.db), "started_utc": _utc(), "committed": [], "remaining": None,
                              "stopped": None, "error": None}
    code = 0
    began = time.monotonic()
    try:
        stage = br.get_stage(args.stage)
        conn = br.connect(args.db, stage, memory_limit=args.duckdb_memory, threads=args.threads)
        try:
            if args.phase == "abandon":
                br.abandon_run(conn, args.stage, args.run_key, args.abandon or "abandoned by operator")
                result["abandoned"] = True
            else:
                br.open_run(conn, args.stage, args.run_key, spec, args.code_sha or _code_sha())
                record = br.run_record(conn, args.stage, args.run_key)
                result["run_status"] = None if record is None else record["status"]
        finally:
            conn.close()
        if args.phase == "batches":
            if result["run_status"] == "published":
                result.update(remaining=0, stopped="complete", published=True)
            else:
                outcome = br.run_slice(args.db, args.stage, args.run_key, max_batches=args.max_batches,
                                       max_seconds=max(1, int(args.max_minutes * 60)),
                                       checkpoint_every=args.checkpoint_every, recycle_every=args.recycle_every,
                                       memory_limit=args.duckdb_memory, threads=args.threads, on_event=emit)
                result.update(committed=list(outcome.committed), remaining=outcome.remaining,
                              stopped=outcome.stopped, error=outcome.error)
                if outcome.stopped == "error":
                    code = EXIT_SLICE_ERROR
        elif args.phase == "finalize":
            result.update(receipt=br.finalize_run(args.db, args.stage, args.run_key,
                                                  memory_limit=args.duckdb_memory, threads=args.threads),
                          finalized=True, remaining=0, stopped="complete")
    except Exception as exc:
        result.update(error=repr(exc)[:4000], traceback=traceback.format_exc()[-6000:])
        code = EXIT_SLICE_ERROR
    result.update(seconds=round(time.monotonic() - began, 1), finished_utc=_utc(), wal_bytes_after=_wal_bytes(args.db))
    if args.result_json:
        _write_json(args.result_json, result)
    emit({"event": "slice_result", **result})
    return code


# ---------------------------------------------------------------------------
# Orchestrator (stdlib only)
# ---------------------------------------------------------------------------

def _guard_script() -> Path:
    for parent in _SCRIPT.parents:
        candidate = parent / _GUARD_RELATIVE
        if candidate.is_file():
            return candidate
    if _CANONICAL_GUARD.is_file():
        return _CANONICAL_GUARD
    raise FileNotFoundError(f"memory guard {_GUARD_RELATIVE} not found")


def _worker_argv(args: argparse.Namespace, phase: str, result: Path) -> list[str]:
    argv = [sys.executable, str(_SCRIPT), "--db", str(args.db.resolve()), "--stage", args.stage,
            "--run-key", args.run_key, "--single-slice", "--phase", phase, "--result-json", str(result),
            "--max-batches", str(args.max_batches), "--max-minutes", str(args.max_minutes),
            "--checkpoint-every", str(args.checkpoint_every), "--recycle-every", str(args.recycle_every)]
    if args.spec_json:
        argv += ["--spec-json", str(args.spec_json.resolve())]
    if args.duckdb_memory:
        argv += ["--duckdb-memory", args.duckdb_memory]
    if args.threads:
        argv += ["--threads", str(args.threads)]
    if args.code_sha:
        argv += ["--code-sha", args.code_sha]
    if phase == "abandon":
        argv += ["--abandon", args.abandon]
    return argv


def _run_guarded_slice(args: argparse.Namespace, guard: Path, parent_job: str, run_dir: Path, number: int,
                       phase: str) -> dict[str, Any]:
    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%S%fZ")
    base = run_dir / f"slice-{number:04d}-{phase}-{stamp}"
    receipt, result = Path(f"{base}.guard.json"), Path(f"{base}.result.json")
    out, err, guard_log = Path(f"{base}.out.log"), Path(f"{base}.err.log"), Path(f"{base}.guard.log")
    argv = [sys.executable, str(guard), "--job-gb", str(args.job_gb), "--wait-minutes", str(args.wait_minutes),
            "--receipt", str(receipt), "--stdout", str(out), "--stderr", str(err), "--quiet",
            "--parent-job", parent_job, "--disk-path", str(args.db.resolve().parent),
            "--min-free-disk-gb", "35"]
    if args.heavy:
        argv.append("--heavy")
    argv += ["--", *_worker_argv(args, phase, result)]
    started_utc, began = _utc(), time.monotonic()
    flags = (subprocess.CREATE_NO_WINDOW | subprocess.CREATE_BREAKAWAY_FROM_JOB) if os.name == "nt" else 0
    with guard_log.open("xb") as log:
        try:
            process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=flags)
        except OSError as exc:
            raise SystemExit(f"cannot start a guarded slice outside this job ({exc!r}): start the orchestrator "
                             "under run_memory_guarded.py --job-gb 0.2 --allow-nested-guards") from exc
        try:
            code = process.wait()
        except BaseException:
            process.kill()  # the guard's kill-on-close job ends the slice with it
            process.wait()
            raise
    guard_receipt = _read_json(receipt) or {}
    outcome = _read_json(result) or {}
    if code == 0 and (not guard_receipt.get("guard_version") or not guard_receipt.get("job_cap_bytes")
                      or guard_receipt.get("returncode") != 0 or guard_receipt.get("status") != "completed"):
        code = EXIT_UNGUARDED
        outcome["error"] = "slice has no successful native guard receipt"
    admission = guard_receipt.get("admission") or {}
    finalize_receipt = outcome.get("receipt")
    return {
        "utc_start": started_utc, "utc_end": _utc(), "slice": number, "phase": phase, "rc": code,
        "guard_status": guard_receipt.get("status"), "admitted": bool(admission.get("admitted_at_utc")),
        "refused": code == EXIT_NOT_ADMITTED, "stopped_by_guard": code == EXIT_STOPPED,
        "job_limit_gb": guard_receipt.get("job_limit_gb"), "heavy": bool(guard_receipt.get("heavy")),
        "guard_peak_gb": guard_receipt.get("native_peak_job_memory_gb"), "cap_hit": guard_receipt.get("cap_hit"),
        "wait_seconds": admission.get("wait_seconds"), "reasons_seen": admission.get("reasons_seen"),
        "run_seconds": guard_receipt.get("run_seconds"), "wall_seconds": round(time.monotonic() - began, 1),
        "min_headroom": guard_receipt.get("min_headroom"), "committed": outcome.get("committed"),
        "remaining": outcome.get("remaining"), "stopped": outcome.get("stopped"), "error": outcome.get("error"),
        "run_status": outcome.get("run_status"), "finalized": bool(outcome.get("finalized")),
        "finalize_receipt": finalize_receipt, "wal_bytes_after": outcome.get("wal_bytes_after"),
        "guard_receipt": str(receipt), "result": str(result), "stdout": str(out), "stderr": str(err),
    }


def orchestrate(args: argparse.Namespace) -> int:
    parent_job = os.environ.get(GUARD_JOB_ENV)
    if not parent_job:
        print("run_slices.py runs under run_memory_guarded.py --job-gb 0.2 --allow-nested-guards "
              "(ATX_GUARD_JOB unset); refusing to start slices", file=sys.stderr)
        return EXIT_UNGUARDED
    if not 0 < args.job_gb <= 1.0:
        raise SystemExit("--job-gb must be in (0, 1.0] (C-58)")
    if args.max_batches < 1 or args.max_minutes <= 0 or args.max_slices < 1 or args.sleep < 0:
        raise SystemExit("--max-batches, --max-minutes and --max-slices must be positive, --sleep >= 0")
    guard = _guard_script()
    key = f"{args.stage}-{args.run_key}"
    run_dir = args.receipts_dir / key
    run_dir.mkdir(parents=True, exist_ok=True)
    ledger = args.receipts_dir / f"{key}.jsonl"
    phase = "abandon" if args.abandon else "batches"
    for number in range(1, args.max_slices + 1):
        line = _run_guarded_slice(args, guard, parent_job, run_dir, number, phase)
        with ledger.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(line, default=str) + "\n")
        print(json.dumps({k: line[k] for k in ("slice", "phase", "rc", "guard_status", "guard_peak_gb", "committed",
                                               "remaining", "stopped", "error")}, default=str), flush=True)
        code = int(line["rc"])
        if line["guard_status"] == "refused_nested_in_guard":
            print("a slice guard found itself nested in this job: start the orchestrator with "
                  "--allow-nested-guards", file=sys.stderr)
            return EXIT_UNGUARDED
        if code in RETRY_CODES:
            if line["guard_status"] in ("refused_low_disk", "refused_disk_measurement_error",
                                        "stopped_low_disk", "stopped_disk_measurement_error",
                                        "refused_parent_gone", "stopped_parent_gone"):
                return code
            time.sleep(args.sleep)
            continue
        if code != 0:
            return code
        if phase in ("abandon", "finalize"):
            return 0
        if line["remaining"] is None:
            print("slice exited 0 without a result", file=sys.stderr)
            return EXIT_SLICE_ERROR
        if int(line["remaining"]) == 0:
            if not args.finalize or line.get("run_status") == "published":
                return 0
            phase = "finalize"
            continue
        if not args.until_complete:
            return 0
    return EXIT_MAX_SLICES


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if any(not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", text)
           for text in (args.stage, args.run_key)):
        raise SystemExit("stage and run-key must be bounded identifiers, never paths")
    if args.single_slice:
        return single_slice(args)
    return orchestrate(args)


if __name__ == "__main__":
    raise SystemExit(main())
