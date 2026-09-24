"""Controller-only Windows job memory/headroom guard with an optional disk floor."""
from __future__ import annotations

import argparse
import ctypes as c
import json
import math
import os
import shutil
import subprocess
import time
from ctypes import wintypes as w
from pathlib import Path

GIB = 1024 ** 3


def validate_disk_options(disk_path: Path | None, min_free_disk_gb: float | None) -> None:
    """Keep disk protection explicit and reject an incomplete or invalid policy."""
    if (disk_path is None) != (min_free_disk_gb is None):
        raise ValueError("Supply --disk-path and --min-free-disk-gb together.")
    if min_free_disk_gb is not None and (not math.isfinite(min_free_disk_gb) or min_free_disk_gb <= 0):
        raise ValueError("--min-free-disk-gb must be positive and finite.")


def sample_disk(disk_path: Path, min_free_disk_gb: float) -> tuple[dict[str, object], str | None]:
    """Measure the existing target's volume; unavailable evidence fails closed."""
    sample: dict[str, object] = {
        "path": str(disk_path), "free_gb": None, "min_free_gb": min_free_disk_gb,
    }
    try:
        resolved = disk_path.resolve(strict=True)
        sample["path"] = str(resolved)
        free = shutil.disk_usage(resolved).free
    except (OSError, ValueError, RuntimeError):
        return sample, "disk_measurement_error"
    sample["free_gb"] = free / GIB
    return sample, "low_disk" if free < min_free_disk_gb * GIB else None


class BasicLimit(c.Structure):
    _fields_ = [
        ("process_time", c.c_longlong), ("job_time", c.c_longlong),
        ("flags", w.DWORD), ("minimum_working_set", c.c_size_t),
        ("maximum_working_set", c.c_size_t), ("active_processes", w.DWORD),
        ("affinity", c.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD),
    ]


class IoCounters(c.Structure):
    _fields_ = [(name, c.c_ulonglong) for name in
                ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]


class ExtendedLimit(c.Structure):
    _fields_ = [("basic", BasicLimit), ("io", IoCounters),
                ("process_memory", c.c_size_t), ("job_memory", c.c_size_t),
                ("peak_process_memory", c.c_size_t), ("peak_job_memory", c.c_size_t)]


class Performance(c.Structure):
    _fields_ = [("cb", w.DWORD)] + [
        (name, c.c_size_t) for name in
        ("commit_total", "commit_limit", "commit_peak", "physical_total", "physical_available",
         "system_cache", "kernel_total", "kernel_paged", "kernel_nonpaged", "page_size")
    ] + [(name, w.DWORD) for name in ("handles", "processes", "threads")]


def main() -> int:
    if os.name != "nt":
        raise SystemExit("This controller guard requires Windows; refusing an unguarded launch.")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job-gb", type=float, default=4)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--stdout", type=Path)
    parser.add_argument("--stderr", type=Path)
    parser.add_argument("--disk-path", type=Path)
    parser.add_argument("--min-free-disk-gb", type=float)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or not 0 < args.job_gb <= 4:
        raise SystemExit("Supply a command and a positive job limit at most 4 GiB.")
    try:
        validate_disk_options(args.disk_path, args.min_free_disk_gb)
    except ValueError as exc:
        parser.error(str(exc))
    kernel = c.WinDLL("kernel32", use_last_error=True)
    psapi = c.WinDLL("psapi", use_last_error=True)
    signatures = {
        "CreateJobObjectW": ([c.c_void_p, w.LPCWSTR], w.HANDLE),
        "GetCurrentProcess": ([], w.HANDLE),
        "SetInformationJobObject": ([w.HANDLE, c.c_int, c.c_void_p, w.DWORD], w.BOOL),
        "QueryInformationJobObject": ([w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.c_void_p], w.BOOL),
        "AssignProcessToJobObject": ([w.HANDLE, w.HANDLE], w.BOOL),
        "TerminateJobObject": ([w.HANDLE, w.UINT], w.BOOL),
    }
    for name, (argtypes, restype) in signatures.items():
        function = getattr(kernel, name)
        function.argtypes, function.restype = argtypes, restype
    psapi.GetPerformanceInfo.argtypes = [c.POINTER(Performance), w.DWORD]
    psapi.GetPerformanceInfo.restype = w.BOOL

    def headroom() -> dict[str, float]:
        info = Performance()
        info.cb = c.sizeof(info)
        if not psapi.GetPerformanceInfo(c.byref(info), c.sizeof(info)):
            raise c.WinError(c.get_last_error())
        return {
            "physical_free_gb": info.physical_available * info.page_size / GIB,
            "commit_free_gb": (info.commit_limit - info.commit_total) * info.page_size / GIB,
        }

    state = {"command": command, "job_limit_gb": args.job_gb,
             "status": "preflight", "headroom": headroom()}
    if args.receipt.exists():
        raise SystemExit(f"Receipt already exists: {args.receipt}; use a new run filename.")

    def record() -> None:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(state), flush=True)

    if min(state["headroom"].values()) < args.job_gb + 2:
        state["status"] = "refused_low_headroom"
        record()
        return 78
    if args.disk_path is not None:
        state["disk"], disk_failure = sample_disk(args.disk_path, args.min_free_disk_gb)
        if disk_failure is not None:
            state["status"] = f"refused_{disk_failure}"
            record()
            return 78
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise c.WinError(c.get_last_error())
    limits = ExtendedLimit()
    limits.basic.flags = 0x200 | 0x2000  # JOB_MEMORY + KILL_ON_JOB_CLOSE; no breakaway.
    limits.job_memory = int(args.job_gb * GIB)
    if not kernel.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)):
        raise c.WinError(c.get_last_error())
    # Associate this small supervisor first, so every descendant inherits the cap
    # from creation. An unsupported nested-job assignment fails before launching.
    if not kernel.AssignProcessToJobObject(job, kernel.GetCurrentProcess()):
        raise c.WinError(c.get_last_error())
    actual = ExtendedLimit()
    if not kernel.QueryInformationJobObject(job, 9, c.byref(actual), c.sizeof(actual), None):
        raise c.WinError(c.get_last_error())
    if actual.job_memory != limits.job_memory:
        raise SystemExit("Job cap verification failed; refusing to launch.")
    stdout = args.stdout.open("xb") if args.stdout else None
    stderr = args.stderr.open("xb") if args.stderr else None
    with subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                          creationflags=subprocess.CREATE_NO_WINDOW) as child:
        state.update(status="running", child_pid=child.pid)
        record()
        next_report = time.monotonic() + 30
        while child.poll() is None:
            memory = headroom()
            if memory["physical_free_gb"] < 1.5 or memory["commit_free_gb"] < 3:
                state.update(status="stopped_low_headroom", headroom=memory)
                record()
                # Terminates only this explicitly assigned job, including supervisor.
                kernel.TerminateJobObject(job, 137)
                raise SystemExit(137)
            if args.disk_path is not None:
                state["disk"], disk_failure = sample_disk(args.disk_path, args.min_free_disk_gb)
                if disk_failure is not None:
                    state.update(status=f"stopped_{disk_failure}", headroom=memory)
                    record()
                    # The same owned job includes the child and supervisor only.
                    kernel.TerminateJobObject(job, 137)
                    raise SystemExit(137)
            if time.monotonic() >= next_report:
                state["headroom"] = memory
                record()
                next_report = time.monotonic() + 30
            time.sleep(1)
        state.update(status="completed" if child.returncode == 0 else "failed",
                     returncode=child.returncode, headroom=headroom())
        if kernel.QueryInformationJobObject(job, 9, c.byref(actual), c.sizeof(actual), None):
            state["native_peak_job_memory_gb"] = actual.peak_job_memory / GIB
        record()
        if stdout:
            stdout.close()
        if stderr:
            stderr.close()
        # Keep the job handle open until process exit; closing it earlier would
        # terminate this supervisor too. The OS then cleans up any leftover children.
        return child.returncode


if __name__ == "__main__":
    raise SystemExit(main())
