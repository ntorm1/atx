"""Bounded research workers: one subprocess per job, each capped by its own Windows job object.

:func:`run_jobs` runs callables (pickled, executed in the child) or argv lists, at most
``max_workers`` at a time. Every job is started through this file run as a script (a
supervisor that imports only the standard library) which

* puts itself in a new job object with ``job_gb`` as the job memory cap (``0.6`` GiB for
  research workers, index section 4 M1) and kill-on-close, so the job and every process it
  starts can never commit more than the cap (an allocation beyond it fails in the worker);
* watches the host: physical free < 1.5 GiB or commit free < 3 GiB at any instant terminates
  the job (the guard floors, never lowered);
* writes a JSON receipt with the job's peak committed memory (``peak_job_memory_gb``, the
  number research acceptance reports), its status and exit code.

Admission (M4): a job starts only when ``min(physical free, commit free) >= job_gb + 2`` GiB;
otherwise the runner waits (a closed gate is a wait, never a failure). Children get
``OPENBLAS_NUM_THREADS=1`` and the DuckDB research settings through the environment
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
import threading
import time
import traceback
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any

GIB = 1024 ** 3
DEFAULT_JOB_GB = 0.6
MAX_JOB_GB = 1.0
ADMISSION_MARGIN_GB = 2.0
FLOOR_PHYSICAL_GB = 1.5
FLOOR_COMMIT_GB = 3.0
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
_SCRIPT = Path(__file__).resolve()


# ---------------------------------------------------------------------------
# Host headroom (Windows GetPerformanceInfo, the guard's own measure)
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


def admitted(job_gb: float) -> bool:
    headroom = host_headroom()
    values = [v for v in headroom.values() if v == v]
    return not values or min(values) >= job_gb + ADMISSION_MARGIN_GB


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
# Supervisor (this file run as a script; standard library only)
# ---------------------------------------------------------------------------

class _BasicLimit(ctypes.Structure):
    _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
                ("flags", ctypes.c_ulong), ("minimum_working_set", ctypes.c_size_t),
                ("maximum_working_set", ctypes.c_size_t), ("active_processes", ctypes.c_ulong),
                ("affinity", ctypes.c_size_t), ("priority", ctypes.c_ulong), ("scheduling", ctypes.c_ulong)]


class _IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in
                ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]


class _ExtendedLimit(ctypes.Structure):
    _fields_ = [("basic", _BasicLimit), ("io", _IoCounters), ("process_memory", ctypes.c_size_t),
                ("job_memory", ctypes.c_size_t), ("peak_process_memory", ctypes.c_size_t),
                ("peak_job_memory", ctypes.c_size_t)]


_JOB_OBJECT_LIMIT_JOB_MEMORY = 0x200
_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
_EXTENDED_LIMIT_INFORMATION = 9


class _Job:
    """A job object holding this process (children inherit it) with a job memory cap."""

    def __init__(self, job_gb: float) -> None:
        from ctypes import wintypes as w

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        for name, argtypes, restype in (
                ("CreateJobObjectW", [ctypes.c_void_p, w.LPCWSTR], w.HANDLE),
                ("GetCurrentProcess", [], w.HANDLE),
                ("SetInformationJobObject", [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD], w.BOOL),
                ("QueryInformationJobObject", [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p],
                 w.BOOL),
                ("AssignProcessToJobObject", [w.HANDLE, w.HANDLE], w.BOOL),
                ("TerminateJobObject", [w.HANDLE, w.UINT], w.BOOL)):
            function = getattr(kernel, name)
            function.argtypes, function.restype = argtypes, restype
        self.kernel = kernel
        self.handle = kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = _ExtendedLimit()
        limits.basic.flags = _JOB_OBJECT_LIMIT_JOB_MEMORY | _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        # The OS keeps page multiples: align down to 1 MiB so the cap read back is the cap asked for.
        self.cap_bytes = (int(job_gb * GIB) // (1 << 20)) * (1 << 20)
        limits.job_memory = self.cap_bytes
        if not kernel.SetInformationJobObject(self.handle, _EXTENDED_LIMIT_INFORMATION, ctypes.byref(limits),
                                              ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())
        if not kernel.AssignProcessToJobObject(self.handle, kernel.GetCurrentProcess()):
            raise ctypes.WinError(ctypes.get_last_error())
        actual = self.query().job_memory
        if not self.cap_bytes - (1 << 16) < actual <= self.cap_bytes:
            raise RuntimeError(f"job memory cap verification failed ({actual} != {self.cap_bytes})")

    def query(self) -> _ExtendedLimit:
        actual = _ExtendedLimit()
        if not self.kernel.QueryInformationJobObject(self.handle, _EXTENDED_LIMIT_INFORMATION, ctypes.byref(actual),
                                                     ctypes.sizeof(actual), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return actual

    def peak_gb(self) -> float:
        return self.query().peak_job_memory / GIB

    def terminate(self, code: int) -> None:
        self.kernel.TerminateJobObject(self.handle, code)


def _supervise(argv: Sequence[str]) -> int:
    args = list(argv)
    job_gb = float(args[args.index("--job-gb") + 1])
    receipt_path = Path(args[args.index("--receipt") + 1])
    call = args[args.index("--call") + 1] if "--call" in args else None
    command = args[args.index("--") + 1:] if "--" in args else []
    state: dict[str, Any] = {"job_gb": job_gb, "status": "starting", "headroom_start": host_headroom(),
                             "pid": os.getpid(), "call": call, "command": command or None}
    started = time.perf_counter()
    lock = threading.Lock()

    def record() -> None:
        tmp = receipt_path.with_name(receipt_path.name + ".tmp")
        tmp.write_text(json.dumps(state, indent=1, default=str) + "\n", encoding="utf-8")
        os.replace(tmp, receipt_path)

    if not 0 < job_gb <= MAX_JOB_GB or (call is None) == (not command):
        state["status"] = "bad_arguments"
        record()
        return 2
    job: _Job | None = None
    if os.name == "nt":
        try:
            job = _Job(job_gb)
        except Exception as error:  # fail closed: a research job never runs uncapped
            state.update(status="job_object_failed", error=repr(error))
            record()
            return EXIT_UNGUARDED
        state["job_memory_cap_bytes"] = job.query().job_memory
    stop = threading.Event()

    def watchdog() -> None:
        while not stop.wait(1.0):
            headroom = host_headroom()
            if headroom["physical_free_gb"] < FLOOR_PHYSICAL_GB or headroom["commit_free_gb"] < FLOOR_COMMIT_GB:
                with lock:
                    state.update(status="stopped_low_headroom", headroom=headroom,
                                 peak_job_memory_gb=None if job is None else job.peak_gb(),
                                 seconds=round(time.perf_counter() - started, 3))
                    record()
                if job is not None:
                    job.terminate(EXIT_HEADROOM)
                os._exit(EXIT_HEADROOM)

    threading.Thread(target=watchdog, daemon=True).start()
    state["status"] = "running"
    record()
    code = 0
    try:
        if call is not None:
            with open(call, "rb") as handle:
                target = pickle.load(handle)
            try:
                target()
            except MemoryError:
                traceback.print_exc()
                state["error"] = "MemoryError (job memory cap)"
                code = EXIT_MEMORY_CAP
            except BaseException as error:  # the job's own failure: exit 1 with its traceback
                traceback.print_exc()
                state["error"] = repr(error)[:2000]
                code = 1
        else:
            with subprocess.Popen(command, stdin=subprocess.DEVNULL) as child:
                code = child.wait()
    finally:
        stop.set()
        with lock:
            state.update(status="completed" if code == 0 else "failed", returncode=code,
                         seconds=round(time.perf_counter() - started, 3), headroom_end=host_headroom(),
                         peak_job_memory_gb=None if job is None else round(job.peak_gb(), 4))
            record()
    return code


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def _launch(index: int, job: Callable[[], None] | Sequence[str], job_gb: float, scratch: Path,
            env: Mapping[str, str], log_dir: Path | None) -> tuple[subprocess.Popen[bytes], Path, Any]:
    receipt = scratch / f"job-{index}.receipt.json"
    argv = [sys.executable, str(_SCRIPT), "--supervise", "--job-gb", str(job_gb), "--receipt", str(receipt)]
    if callable(job):
        target = scratch / f"job-{index}.pickle"
        target.write_bytes(pickle.dumps(job))
        argv += ["--call", str(target)]
    else:
        argv += ["--", *[str(part) for part in job]]
    log = None
    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
        log = (log_dir / f"job-{index}.log").open("ab")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    process = subprocess.Popen(argv, env=dict(env), stdin=subprocess.DEVNULL, stdout=log,
                               stderr=subprocess.STDOUT if log is not None else None, creationflags=flags)
    return process, receipt, log


def run_jobs(jobs: Sequence[Callable[[], None]] | Sequence[list[str]], max_workers: int = 2, *,
             job_gb: float = DEFAULT_JOB_GB, admission: bool = True, log_dir: Path | None = None,
             env: Mapping[str, str] | None = None, receipts: list[dict[str, Any]] | None = None,
             on_finish: Callable[[int, int, dict[str, Any]], None] | None = None,
             poll_s: float = 0.5) -> list[int]:
    """Run each job in its own capped subprocess, ``max_workers`` at a time; return the exit codes.

    ``jobs``: picklable zero-argument callables (module-level functions or ``functools.partial``
    of them) or argv lists. ``receipts`` (if given) is filled with each job's receipt in job
    order (status, ``peak_job_memory_gb``, seconds, headroom). Exit codes: 0 ok, 1 the job
    raised, 75 the job hit its memory cap, 137 stopped at a host floor, 70 no cap possible.
    """
    if isinstance(max_workers, bool) or not isinstance(max_workers, int) or not 1 <= max_workers <= 8:
        raise ValueError("max_workers must be an integer 1..8")
    if not 0 < job_gb <= MAX_JOB_GB:
        raise ValueError(f"job_gb must be in (0, {MAX_JOB_GB}] GiB (index section 4 M1)")
    for job in jobs:
        if not callable(job) and not (isinstance(job, (list, tuple)) and job and all(isinstance(p, (str, Path))
                                                                                      for p in job)):
            raise TypeError("each job is a zero-argument callable or a non-empty argv list")
        if callable(job):
            pickle.dumps(job)  # fail here, not in the child, for a lambda or a closure
    child_env = {**os.environ, **WORKER_ENV, **dict(env or {})}
    results: list[int | None] = [None] * len(jobs)
    collected: list[dict[str, Any]] = [{} for _ in jobs]
    pending = list(range(len(jobs)))
    running: dict[int, tuple[subprocess.Popen[bytes], Path, Any]] = {}
    last_wait_note = 0.0
    with tempfile.TemporaryDirectory(prefix="atx_research_jobs_") as scratch_dir:
        scratch = Path(scratch_dir)
        try:
            while pending or running:
                while pending and len(running) < max_workers:
                    if admission and not admitted(job_gb):
                        now = time.monotonic()
                        if now - last_wait_note > 60:
                            headroom = host_headroom()
                            print(f"[workers] admission wait: phys {headroom['physical_free_gb']:.2f} GiB, "
                                  f"commit {headroom['commit_free_gb']:.2f} GiB < job {job_gb} + "
                                  f"{ADMISSION_MARGIN_GB} GiB", file=sys.stderr, flush=True)
                            last_wait_note = now
                        break
                    index = pending.pop(0)
                    running[index] = _launch(index, jobs[index], job_gb, scratch, child_env, log_dir)
                for index, (process, receipt, log) in list(running.items()):
                    code = process.poll()
                    if code is None:
                        continue
                    if log is not None:
                        log.close()
                    try:
                        collected[index] = json.loads(receipt.read_text(encoding="utf-8"))
                    except (OSError, ValueError):
                        collected[index] = {"status": "no_receipt"}
                    collected[index]["returncode"] = code
                    results[index] = code
                    del running[index]
                    if on_finish is not None:
                        on_finish(index, code, collected[index])
                time.sleep(poll_s)
        finally:
            for process, _, log in running.values():  # interrupted: stop what this runner started
                process.kill()
                process.wait()
                if log is not None:
                    log.close()
    if receipts is not None:
        receipts.extend(collected)
    return [int(code) for code in results]  # type: ignore[arg-type]


if __name__ == "__main__":
    if sys.path and Path(sys.path[0]).resolve() == _SCRIPT.parent:
        sys.path.pop(0)  # the research package directory must not shadow top-level modules
    if "--supervise" in sys.argv:
        raise SystemExit(_supervise(sys.argv[1:]))
    raise SystemExit("usage: workers.py --supervise --job-gb G --receipt PATH (--call PICKLE | -- argv...)")
