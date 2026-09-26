"""Bounded research workers: one subprocess per job, each run under the memory guard.

:func:`run_jobs` runs callables (pickled, executed in the child) or argv lists, at most
``max_workers`` at a time. Every job is launched through the ONE memory guard,
``.superpowers/sdd/tier1-parity/run_memory_guarded.py`` (ruling C-58, node M0; ``ATX_MEMORY_GUARD``
names another copy for TESTS ONLY), which

* runs the job inside a Windows job object whose committed memory is capped at ``job_gb``
  (``0.6`` GiB for research workers; page-aligned, never above the request), so the job and
  every process it starts can never commit more than the cap (an allocation beyond it fails in
  the worker);
* admits it by size: free commit, net of what our running jobs may still claim, >= cap + 1.0 GiB;
  host-wide slot files keep the sum of our running caps <= 2.0 GiB (one HEAVY job at most); the
  guard queues FIFO for up to ``wait_minutes`` (free physical memory is not an admission condition);
* stops the job in-run only at free commit < 0.75 GiB or free physical < 0.25 GiB for 30 s
  (exit 137, a normal event: re-run the slice, it resumes from its ledger), and also when the
  orchestrator's own guard stops or dies (``--parent-job``, from ``ATX_GUARD_JOB``), so no worker
  outlives the run that launched it;
* writes a JSON receipt with the job's native peak committed memory on every exit
  (``peak_job_memory_gb`` here, the number research acceptance reports), its status and exit code.

The orchestrator (the process calling :func:`run_jobs`) runs under the guard in orchestrator mode:
``run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 -- python <script>``.

Children get ``OPENBLAS_NUM_THREADS=1`` and the DuckDB research settings through the environment
(``ATX_RESEARCH_DUCKDB_MEMORY_LIMIT=256MB``, ``ATX_RESEARCH_DUCKDB_THREADS=1``), which
:func:`research_connect` applies at connect.

Research workers never open the prod warehouse (ruling R-5): their inputs are lake snapshots
or retained files.
"""

from __future__ import annotations

import ctypes
import json
import os
import pickle
import subprocess
import sys
import tempfile
import time
import traceback
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any

GIB = 1024 ** 3
DEFAULT_JOB_GB = 0.6
MAX_JOB_GB = 1.0
DEFAULT_WAIT_MINUTES = 60.0
GUARD_ENV = "ATX_MEMORY_GUARD"
GUARD_JOB_ENV = "ATX_GUARD_JOB"
MEMORY_ENV = "ATX_RESEARCH_DUCKDB_MEMORY_LIMIT"
THREADS_ENV = "ATX_RESEARCH_DUCKDB_THREADS"
WORKER_ENV: Mapping[str, str] = {
    "OPENBLAS_NUM_THREADS": "1",
    MEMORY_ENV: "256MB",
    THREADS_ENV: "1",
    "PYTHONUNBUFFERED": "1",
}
EXIT_MEMORY_CAP = 75
EXIT_HEADROOM = 137
EXIT_UNGUARDED = 70
EXIT_NOT_ADMITTED = 78
_SCRIPT = Path(__file__).resolve()
_GUARD_RELATIVE = Path(".superpowers") / "sdd" / "tier1-parity" / "run_memory_guarded.py"


def guard_script() -> Path:
    """The memory guard: the first ancestor holding the tracked guard (``$ATX_MEMORY_GUARD``: tests only)."""
    named = os.environ.get(GUARD_ENV)
    if named:
        path = Path(named).resolve()
        if not path.is_file():
            raise FileNotFoundError(f"{GUARD_ENV}={named} is not a file")
        return path
    for parent in _SCRIPT.parents:
        candidate = parent / _GUARD_RELATIVE
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"memory guard {_GUARD_RELATIVE} not found above {_SCRIPT}; set {GUARD_ENV}")


# ---------------------------------------------------------------------------
# Host headroom (informational; admission is the guard's, never re-derived here)
# ---------------------------------------------------------------------------

class _Performance(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong)] + [
        (name, ctypes.c_size_t) for name in
        ("commit_total", "commit_limit", "commit_peak", "physical_total", "physical_available",
         "system_cache", "kernel_total", "kernel_paged", "kernel_nonpaged", "page_size")
    ] + [(name, ctypes.c_ulong) for name in ("handles", "processes", "threads")]


def host_headroom() -> dict[str, float]:
    """Physical free and commit free in GiB (``nan`` off Windows)."""
    if os.name != "nt":
        return {"physical_free_gb": float("nan"), "commit_free_gb": float("nan")}
    info = _Performance()
    info.cb = ctypes.sizeof(info)
    if not ctypes.WinDLL("psapi").GetPerformanceInfo(ctypes.byref(info), ctypes.sizeof(info)):
        raise OSError("GetPerformanceInfo failed")
    return {"physical_free_gb": info.physical_available * info.page_size / GIB,
            "commit_free_gb": (info.commit_limit - info.commit_total) * info.page_size / GIB}


# ---------------------------------------------------------------------------
# Worker-side helpers
# ---------------------------------------------------------------------------

def research_connect(db_path: Path | str | None = None, *, read_only: bool = True, root: Path | None = None) -> Any:
    """A DuckDB connection with the worker's limits from the environment (256MB / 1 thread default)."""
    from .research_lake import connect_bounded

    memory = os.environ.get(MEMORY_ENV, WORKER_ENV[MEMORY_ENV])
    threads = int(os.environ.get(THREADS_ENV, WORKER_ENV[THREADS_ENV]))
    return connect_bounded(db_path, root=root, memory_limit=memory, threads=threads, read_only=read_only)


# ---------------------------------------------------------------------------
# Call runner (this file run as a script inside the guard's job; standard library only)
# ---------------------------------------------------------------------------

def _run_call(argv: Sequence[str]) -> int:
    """Unpickle and run one callable: exit 0, 1 (it raised) or 75 (MemoryError at the job cap)."""
    args = list(argv)
    call = Path(args[args.index("--run-call") + 1])
    error_path = Path(args[args.index("--error") + 1]) if "--error" in args else None
    error: str | None = None
    code = 0
    try:
        with open(call, "rb") as handle:
            target = pickle.load(handle)
        target()
    except MemoryError:
        traceback.print_exc()
        error, code = "MemoryError (job memory cap)", EXIT_MEMORY_CAP
    except BaseException as exc:  # the job's own failure: exit 1 with its traceback
        traceback.print_exc()
        error, code = repr(exc)[:2000], 1
    if error is not None and error_path is not None:
        error_path.write_text(json.dumps({"error": error}) + "\n", encoding="utf-8")
    return code


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def _launch(index: int, job: Callable[[], None] | Sequence[str], job_gb: float, wait_minutes: float, heavy: bool,
            scratch: Path, env: Mapping[str, str], log_dir: Path | None) -> tuple[subprocess.Popen[bytes], Path, Any]:
    receipt = scratch / f"job-{index}.receipt.json"
    argv = [sys.executable, str(guard_script()), "--job-gb", str(job_gb), "--wait-minutes", str(wait_minutes),
            "--receipt", str(receipt), "--quiet"]
    if heavy:
        argv.append("--heavy")
    parent_job = os.environ.get(GUARD_JOB_ENV)
    if parent_job:  # the orchestrator's guard: the worker stops when that guard stops or dies
        argv += ["--parent-job", parent_job]
    argv.append("--")
    if callable(job):
        target = scratch / f"job-{index}.pickle"
        target.write_bytes(pickle.dumps(job))
        argv += [sys.executable, str(_SCRIPT), "--run-call", str(target), "--error",
                 str(scratch / f"job-{index}.error.json")]
    else:
        argv += [str(part) for part in job]
    log = None
    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
        log = (log_dir / f"job-{index}.log").open("ab")
    # The guard takes its own cap and slot, so it must not nest inside this process's job (a nested job
    # would share this process's cap): it is started with CREATE_BREAKAWAY_FROM_JOB, which a guarded
    # orchestrator allows only when it was started with --allow-nested-guards (otherwise the guard stays
    # nested and refuses itself: refused_nested_in_guard, raised below).
    flags = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_BREAKAWAY_FROM_JOB if os.name == "nt" else 0
    process = subprocess.Popen(argv, env=dict(env), stdin=subprocess.DEVNULL, stdout=log,
                               stderr=subprocess.STDOUT if log is not None else None, creationflags=flags)
    return process, receipt, log


def _collect(receipt: Path, code: int) -> dict[str, Any]:
    """The guard receipt, with the fields research callers read (``peak_job_memory_gb``, ``seconds``).

    A result without a guard receipt (``guard_version`` and ``job_cap_bytes``) is never a success: the
    job cannot be shown to have run capped, so it reports exit 70.
    """
    try:
        collected: dict[str, Any] = json.loads(receipt.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        collected = {"status": "no_receipt"}
    if not isinstance(collected, dict) or not collected.get("guard_version") or not collected.get("job_cap_bytes"):
        collected = {"status": "no_valid_guard_receipt", "raw_status": (collected or {}).get("status")
                     if isinstance(collected, dict) else None}
        code = code or EXIT_UNGUARDED
    collected["peak_job_memory_gb"] = collected.get("native_peak_job_memory_gb")
    collected["seconds"] = collected.get("run_seconds")
    collected["wait_seconds"] = (collected.get("admission") or {}).get("wait_seconds")
    error_file = receipt.with_name(receipt.name.replace(".receipt.json", ".error.json"))
    if error_file.is_file():
        try:
            collected["error"] = json.loads(error_file.read_text(encoding="utf-8")).get("error")
        except (OSError, ValueError, AttributeError):
            collected["error"] = "unreadable error file"
    if code == 1 and collected.get("cap_hit"):
        code = EXIT_MEMORY_CAP  # an argv job that died at its cap
    collected["returncode"] = code
    return collected


def run_jobs(jobs: Sequence[Callable[[], None]] | Sequence[list[str]], max_workers: int = 2, *,
             job_gb: float = DEFAULT_JOB_GB, wait_minutes: float = DEFAULT_WAIT_MINUTES, heavy: bool = False,
             log_dir: Path | None = None, env: Mapping[str, str] | None = None,
             receipts: list[dict[str, Any]] | None = None,
             on_finish: Callable[[int, int, dict[str, Any]], None] | None = None,
             poll_s: float = 0.5) -> list[int]:
    """Run each job in its own guarded subprocess, ``max_workers`` at a time; return the exit codes.

    ``jobs``: picklable zero-argument callables (module-level functions or ``functools.partial``
    of them) or argv lists. Admission, queueing (up to ``wait_minutes`` per job) and the in-run
    stop are the memory guard's; this runner never re-derives a memory rule. ``receipts`` (if
    given) is filled with each job's guard receipt in job order (status, ``peak_job_memory_gb``,
    ``seconds``, ``wait_seconds``, headroom). Exit codes: 0 ok, 1 the job raised, 75 the job hit
    its memory cap, 137 stopped in-run by the guard (re-run: resume from the ledger), 78 not
    admitted within ``wait_minutes`` (re-engineer smaller, index section 4 M8), 70 no cap possible
    or no valid guard receipt.
    """
    if isinstance(max_workers, bool) or not isinstance(max_workers, int) or not 1 <= max_workers <= 8:
        raise ValueError("max_workers must be an integer 1..8")
    if not 0 < job_gb <= MAX_JOB_GB:
        raise ValueError(f"job_gb must be in (0, {MAX_JOB_GB}] GiB (index section 4 M1)")
    if not 0 <= wait_minutes <= 24 * 60:
        raise ValueError("wait_minutes must be in [0, 1440]")
    for job in jobs:
        if not callable(job) and not (isinstance(job, (list, tuple)) and job and all(isinstance(p, (str, Path))
                                                                                      for p in job)):
            raise TypeError("each job is a zero-argument callable or a non-empty argv list")
        if callable(job):
            pickle.dumps(job)  # fail here, not in the child, for a lambda or a closure
    guard_script()  # fail closed before launching anything: no job runs uncapped
    child_env = {**os.environ, **WORKER_ENV, **dict(env or {})}
    results: list[int | None] = [None] * len(jobs)
    collected: list[dict[str, Any]] = [{} for _ in jobs]
    pending = list(range(len(jobs)))
    running: dict[int, tuple[subprocess.Popen[bytes], Path, Any]] = {}
    with tempfile.TemporaryDirectory(prefix="atx_research_jobs_") as scratch_dir:
        scratch = Path(scratch_dir)
        try:
            while pending or running:
                while pending and len(running) < max_workers:
                    index = pending.pop(0)
                    running[index] = _launch(index, jobs[index], job_gb, wait_minutes, heavy, scratch, child_env,
                                             log_dir)
                for index, (process, receipt, log) in list(running.items()):
                    code = process.poll()
                    if code is None:
                        continue
                    if log is not None:
                        log.close()
                    collected[index] = _collect(receipt, code)
                    if collected[index].get("status") == "refused_nested_in_guard":
                        raise RuntimeError(
                            "run_jobs was started inside a guarded job without --allow-nested-guards, so each "
                            "worker would share that job's cap. Start the orchestrator with "
                            "run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 -- ...")
                    results[index] = collected[index]["returncode"]
                    del running[index]
                    if on_finish is not None:
                        on_finish(index, results[index], collected[index])
                time.sleep(poll_s)
        finally:
            for process, _, log in running.values():  # interrupted: stop what this runner started
                process.kill()  # the guard's kill-on-close job then ends the job's own processes
                process.wait()
                if log is not None:
                    log.close()
    if receipts is not None:
        receipts.extend(collected)
    return [int(code) for code in results]  # type: ignore[arg-type]


if __name__ == "__main__":
    if sys.path and Path(sys.path[0]).resolve() == _SCRIPT.parent:
        sys.path.pop(0)  # the research package directory must not shadow top-level modules
    if "--run-call" in sys.argv:
        raise SystemExit(_run_call(sys.argv[1:]))
    raise SystemExit("usage: workers.py --run-call PICKLE [--error PATH] (launched by run_jobs under the guard)")
