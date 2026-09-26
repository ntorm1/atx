"""Memory guard for every Python / pytest / DuckDB process (ruling C-58, node M0).

What prevents OOM is the per-job hard cap: the command runs inside a Windows job object whose committed
memory is capped at ``--job-gb`` (page-aligned, never above the request; at most 1.0 GiB). The job cannot
commit past its cap, so it cannot exhaust the host; an allocation beyond it fails inside the job.

Admission is size-based and host-wide:
* free commit, net of what our running jobs may still claim (each running job's cap minus its live
  commit), must be >= cap + 1.0 GiB reserve. Free physical memory is NOT an admission condition;
* host-wide slot files: the sum of our running caps stays <= 2.0 GiB and at most one ``--heavy`` job runs;
* ``--wait-minutes N`` queues FIFO for admission and a slot (earliest waiter first; a HEAVY waiter held
  back only by the running HEAVY job keeps its budget but does not block later non-heavy jobs). Slot
  files of dead processes (PID + creation time) are reclaimed. ``--wait-minutes 0`` tries once.

In-run protection (1 s poll): stop the job when free commit < 0.75 GiB, or free physical < 0.25 GiB for
30 consecutive seconds (thrash), or the optional disk floor fails. A stopped slice resumes from its ledger.

A guard started inside another guard's job is refused (that outer cap would bind it). An orchestrator
that starts guarded jobs itself (research ``workers.run_jobs``) runs with ``--allow-nested-guards``: its
job then lets a child created with CREATE_BREAKAWAY_FROM_JOB leave, which run_jobs uses only to start
another guard (that guard caps itself in a job and slot of its own).

Exit codes: the command's own code when it ran to completion; 70 no job cap possible (never runs
uncapped); 78 not admitted (wait timeout or disk preflight); 137 stopped in-run; 127 the command could
not be started; 2 bad arguments.
The JSON receipt records the native peak job commit on every exit, including stops and wait timeouts.

    python run_memory_guarded.py --job-gb 0.6 --wait-minutes 30 [--heavy] [--receipt R.json] -- <command...>
    python run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 -- <orchestrator...>
    python run_memory_guarded.py --status
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes as c
import datetime as dt
import json
import math
import os
import shutil
import subprocess
import sys
import time
from collections.abc import Iterator, Mapping, Sequence
from ctypes import wintypes as w
from pathlib import Path
from typing import Any

GIB = 1024 ** 3
PAGE_BYTES = 4096
MAX_JOB_GB = 1.0
SLOT_BUDGET_GB = 2.0
ADMISSION_RESERVE_GB = 1.0
STOP_COMMIT_GB = 0.75
STOP_PHYSICAL_GB = 0.25
THRASH_SECONDS = 30.0
POLL_SECONDS = 1.0
RECORD_SECONDS = 30.0
MAX_WAIT_MINUTES = 24 * 60
EXIT_BAD_ARGUMENTS = 2
EXIT_UNGUARDED = 70
EXIT_NOT_ADMITTED = 78
EXIT_STOPPED = 137
EXIT_LAUNCH_FAILED = 127
GUARD_VERSION = "c58-m0"
SLOT_DIR_ENV = "ATX_GUARD_SLOT_DIR"
CHILD_ENV = {"OPENBLAS_NUM_THREADS": "1"}
_JOB_MSG_JOB_MEMORY_LIMIT = 10
_STILL_ACTIVE = 259


def page_aligned_cap(job_gb: float) -> int:
    """The cap the job object gets: whole pages, never above the request (C-8)."""
    return int(job_gb * GIB) // PAGE_BYTES * PAGE_BYTES


def guard_home() -> Path:
    return Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "atx-memory-guard"


def default_slot_dir() -> Path:
    return Path(os.environ[SLOT_DIR_ENV]) if os.environ.get(SLOT_DIR_ENV) else guard_home() / "slots"


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


# ---------------------------------------------------------------------------
# Win32 structures and bindings
# ---------------------------------------------------------------------------

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


class MemoryUsage(c.Structure):  # JOBOBJECT_MEMORY_USAGE_INFORMATION (class 28)
    _fields_ = [("job_memory", c.c_ulonglong), ("peak_job_memory", c.c_ulonglong)]


class CompletionPort(c.Structure):  # JOBOBJECT_ASSOCIATE_COMPLETION_PORT (class 7)
    _fields_ = [("key", c.c_void_p), ("port", w.HANDLE)]


_API: dict[str, Any] = {}


def _api() -> tuple[Any, Any]:
    if not _API:
        kernel = c.WinDLL("kernel32", use_last_error=True)
        psapi = c.WinDLL("psapi", use_last_error=True)
        signatures = {
            "CreateJobObjectW": ([c.c_void_p, w.LPCWSTR], w.HANDLE),
            "OpenJobObjectW": ([w.DWORD, w.BOOL, w.LPCWSTR], w.HANDLE),
            "GetCurrentProcess": ([], w.HANDLE),
            "OpenProcess": ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
            "GetProcessTimes": ([w.HANDLE] + [c.POINTER(c.c_ulonglong)] * 4, w.BOOL),
            "GetExitCodeProcess": ([w.HANDLE, c.POINTER(w.DWORD)], w.BOOL),
            "CloseHandle": ([w.HANDLE], w.BOOL),
            "SetInformationJobObject": ([w.HANDLE, c.c_int, c.c_void_p, w.DWORD], w.BOOL),
            "QueryInformationJobObject": ([w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.c_void_p], w.BOOL),
            "AssignProcessToJobObject": ([w.HANDLE, w.HANDLE], w.BOOL),
            "IsProcessInJob": ([w.HANDLE, w.HANDLE, c.POINTER(w.BOOL)], w.BOOL),
            "TerminateJobObject": ([w.HANDLE, w.UINT], w.BOOL),
            "CreateIoCompletionPort": ([w.HANDLE, w.HANDLE, c.c_size_t, w.DWORD], w.HANDLE),
            "GetQueuedCompletionStatus": ([w.HANDLE, c.POINTER(w.DWORD), c.POINTER(c.c_size_t),
                                           c.POINTER(c.c_void_p), w.DWORD], w.BOOL),
        }
        for name, (argtypes, restype) in signatures.items():
            function = getattr(kernel, name)
            function.argtypes, function.restype = argtypes, restype
        psapi.GetPerformanceInfo.argtypes = [c.POINTER(Performance), w.DWORD]
        psapi.GetPerformanceInfo.restype = w.BOOL
        _API.update(kernel=kernel, psapi=psapi)
    return _API["kernel"], _API["psapi"]


def host_memory() -> dict[str, float]:
    """Free physical and free commit (GiB) plus the exact free-commit bytes."""
    _, psapi = _api()
    info = Performance()
    info.cb = c.sizeof(info)
    if not psapi.GetPerformanceInfo(c.byref(info), c.sizeof(info)):
        raise c.WinError(c.get_last_error())
    commit_free = (info.commit_limit - info.commit_total) * info.page_size
    return {"physical_free_gb": round(info.physical_available * info.page_size / GIB, 4),
            "commit_free_gb": round(commit_free / GIB, 4), "commit_free_bytes": commit_free,
            "commit_limit_gb": round(info.commit_limit * info.page_size / GIB, 3)}


def process_created(pid: int) -> int | None:
    """Creation FILETIME of a live process; None when it has exited or never existed."""
    kernel, _ = _api()
    handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        # Access denied means the process exists but is not ours to query: treat it as live.
        return -1 if c.get_last_error() == 5 else None
    try:
        code = w.DWORD()
        if not kernel.GetExitCodeProcess(handle, c.byref(code)) or code.value != _STILL_ACTIVE:
            return None
        times = [c.c_ulonglong() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *[c.byref(t) for t in times]):
            return -1
        return int(times[0].value)
    finally:
        kernel.CloseHandle(handle)


def job_commit_bytes(job_name: str) -> int | None:
    """Live committed memory of another guard's named job (None when it cannot be read)."""
    kernel, _ = _api()
    handle = kernel.OpenJobObjectW(0x0004, False, job_name)  # JOB_OBJECT_QUERY
    if not handle:
        return None
    try:
        usage = MemoryUsage()
        if not kernel.QueryInformationJobObject(handle, 28, c.byref(usage), c.sizeof(usage), None):
            return None
        return int(usage.job_memory)
    finally:
        kernel.CloseHandle(handle)


def inside_job(job_name: str) -> bool:
    """True when this process runs inside another guard's named job (nested: that job's cap would bind)."""
    kernel, _ = _api()
    handle = kernel.OpenJobObjectW(0x0004, False, job_name)  # JOB_OBJECT_QUERY
    if not handle:
        return False
    try:
        result = w.BOOL()
        return bool(kernel.IsProcessInJob(kernel.GetCurrentProcess(), handle, c.byref(result)) and result.value)
    finally:
        kernel.CloseHandle(handle)


class Job:
    """A named job object holding this supervisor (children inherit it) with a page-aligned commit cap."""

    def __init__(self, name: str, cap_bytes: int, *, allow_nested_guards: bool = False) -> None:
        kernel, _ = _api()
        self.kernel = kernel
        self.name = name
        self.handle = kernel.CreateJobObjectW(None, name)
        if not self.handle:
            raise c.WinError(c.get_last_error())
        limits = ExtendedLimit()
        # JOB_MEMORY + KILL_ON_JOB_CLOSE. No breakaway, except for an orchestrator (--allow-nested-guards):
        # BREAKAWAY_OK lets only a child created with CREATE_BREAKAWAY_FROM_JOB leave, which research
        # workers use solely to start another guard (that child caps itself in its own job and slot).
        limits.basic.flags = 0x200 | 0x2000 | (0x800 if allow_nested_guards else 0)
        limits.job_memory = cap_bytes
        if not kernel.SetInformationJobObject(self.handle, 9, c.byref(limits), c.sizeof(limits)):
            raise c.WinError(c.get_last_error())
        self.port = kernel.CreateIoCompletionPort(w.HANDLE(-1), None, 0, 1)
        if self.port:
            association = CompletionPort(None, self.port)
            if not kernel.SetInformationJobObject(self.handle, 7, c.byref(association), c.sizeof(association)):
                self.port = None
        # Associate this small supervisor first, so every descendant inherits the cap from creation.
        if not kernel.AssignProcessToJobObject(self.handle, kernel.GetCurrentProcess()):
            raise c.WinError(c.get_last_error())
        self.cap_bytes = int(self.limits().job_memory)
        if self.cap_bytes != cap_bytes or not 0 < self.cap_bytes:
            raise RuntimeError(f"job cap read back {self.cap_bytes} != page-aligned request {cap_bytes}")
        self.cap_hit_events = 0

    def limits(self) -> ExtendedLimit:
        actual = ExtendedLimit()
        if not self.kernel.QueryInformationJobObject(self.handle, 9, c.byref(actual), c.sizeof(actual), None):
            raise c.WinError(c.get_last_error())
        return actual

    def peak_bytes(self) -> int | None:
        try:
            return int(self.limits().peak_job_memory)
        except OSError:
            return None

    def drain_messages(self) -> None:
        if not self.port:
            return
        message, key, overlapped = w.DWORD(), c.c_size_t(), c.c_void_p()
        while self.kernel.GetQueuedCompletionStatus(self.port, c.byref(message), c.byref(key),
                                                    c.byref(overlapped), 0):
            if message.value == _JOB_MSG_JOB_MEMORY_LIMIT:
                self.cap_hit_events += 1

    def terminate(self, code: int) -> None:
        """Terminates only this explicitly assigned job, including this supervisor."""
        self.kernel.TerminateJobObject(self.handle, code)


# ---------------------------------------------------------------------------
# Host-wide slots and the admission rule
# ---------------------------------------------------------------------------

class Slot:
    """One job's host-wide slot file: ``state`` is "waiting" (queued) or "running" (admitted)."""

    FIELDS = ("seq", "pid", "created", "state", "cap_bytes", "heavy", "job_name", "receipt", "enqueued_at_utc",
              "admitted_at_utc", "command_head")

    def __init__(self, seq: int, pid: int, created: int, state: str, cap_bytes: int, heavy: bool, job_name: str,
                 receipt: str, enqueued_at_utc: str, admitted_at_utc: str | None = None,
                 command_head: str = "") -> None:
        self.seq, self.pid, self.created, self.state = int(seq), int(pid), int(created), str(state)
        self.cap_bytes, self.heavy, self.job_name, self.receipt = int(cap_bytes), bool(heavy), job_name, receipt
        self.enqueued_at_utc, self.admitted_at_utc, self.command_head = enqueued_at_utc, admitted_at_utc, command_head

    def as_dict(self) -> dict[str, Any]:
        return {name: getattr(self, name) for name in self.FIELDS}


def decide(me: Slot, slots: Sequence[Slot], free_commit_bytes: int, usage: Mapping[int, int | None], *,
           budget_bytes: int = page_aligned_cap(SLOT_BUDGET_GB),
           reserve_bytes: int = page_aligned_cap(ADMISSION_RESERVE_GB)) -> tuple[bool, list[str], dict[str, Any]]:
    """The one admission rule. ``usage`` maps a running slot's seq to its live job commit (None: unknown)."""
    others = [s for s in slots if s.seq != me.seq]
    running = [s for s in others if s.state == "running"]
    earlier = sorted((s for s in others if s.state == "waiting" and s.seq < me.seq), key=lambda s: s.seq)
    running_caps = sum(s.cap_bytes for s in running)
    heavy_running = any(s.heavy for s in running)
    unclaimed = 0
    for slot in running:
        used = usage.get(slot.seq)
        unclaimed += slot.cap_bytes if used is None else max(0, slot.cap_bytes - used)
    effective_free = free_commit_bytes - unclaimed
    reasons: list[str] = []
    if me.heavy and heavy_running:
        reasons.append("heavy_running")
    if running_caps + me.cap_bytes > budget_bytes:
        reasons.append("slot_budget")
    if effective_free < me.cap_bytes + reserve_bytes:
        reasons.append("free_commit")
    # FIFO: every earlier live waiter blocks me, except a HEAVY waiter held only by the running HEAVY job
    # when I am not heavy; it then keeps its budget for when that job ends.
    bypassable = [s for s in earlier if s.heavy and heavy_running and not me.heavy]
    blockers = [s for s in earlier if s not in bypassable]
    if blockers:
        reasons.append(f"fifo_behind_seq_{blockers[0].seq}")
    elif bypassable:
        light_running = sum(s.cap_bytes for s in running if not s.heavy)
        if light_running + me.cap_bytes + max(s.cap_bytes for s in bypassable) > budget_bytes:
            reasons.append("budget_held_for_heavy_waiter")
    detail = {"effective_free_commit_gb": round(effective_free / GIB, 4),
              "free_commit_gb": round(free_commit_bytes / GIB, 4),
              "running_unclaimed_gb": round(unclaimed / GIB, 4),
              "running_caps_gb": round(running_caps / GIB, 4), "running_jobs": len(running),
              "heavy_running": heavy_running, "earlier_waiters": len(earlier)}
    return not reasons, reasons, detail


class SlotPool:
    """Slot files under one host-wide directory, serialized by an OS byte-range lock."""

    def __init__(self, directory: Path) -> None:
        self.dir = directory
        self.dir.mkdir(parents=True, exist_ok=True)

    @contextlib.contextmanager
    def lock(self, timeout_s: float = 120.0) -> Iterator[None]:
        import msvcrt

        fd = os.open(self.dir / ".lock", os.O_RDWR | os.O_CREAT, 0o600)
        deadline = time.monotonic() + timeout_s
        try:
            while True:
                try:
                    os.lseek(fd, 0, os.SEEK_SET)
                    msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"slot lock {self.dir / '.lock'} not acquired in {timeout_s} s")
                    time.sleep(0.05)
            try:
                yield
            finally:
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        finally:
            os.close(fd)

    def path(self, slot: Slot) -> Path:
        return self.dir / f"slot-{slot.seq:012d}-{slot.pid}.json"

    def next_seq(self) -> int:
        seq_path = self.dir / ".seq"
        try:
            value = int(seq_path.read_text(encoding="ascii").strip() or 0) + 1
        except (OSError, ValueError):
            value = 1
        _write_atomic(seq_path, f"{value}\n")
        return value

    def read_all(self) -> list[tuple[Path, Slot | None]]:
        found: list[tuple[Path, Slot | None]] = []
        for path in sorted(self.dir.glob("slot-*.json")):
            try:
                found.append((path, Slot(**json.loads(path.read_text(encoding="utf-8")))))
            except (OSError, ValueError, TypeError):
                found.append((path, None))
        return found

    def live_slots(self) -> tuple[list[Slot], list[dict[str, Any]]]:
        """Live slots, after removing those of dead processes (call under the lock)."""
        live: list[Slot] = []
        reclaimed: list[dict[str, Any]] = []
        for path, slot in self.read_all():
            if slot is not None:
                created = process_created(slot.pid)
                if created is not None and (created == -1 or created == slot.created):
                    live.append(slot)
                    continue
            with contextlib.suppress(OSError):
                path.unlink()
            reclaimed.append({"file": path.name, "pid": None if slot is None else slot.pid,
                              "state": None if slot is None else slot.state,
                              "cap_gb": None if slot is None else round(slot.cap_bytes / GIB, 4)})
        return live, reclaimed

    def write(self, slot: Slot) -> None:
        _write_atomic(self.path(slot), json.dumps(slot.as_dict()) + "\n")

    def remove(self, slot: Slot) -> None:
        with contextlib.suppress(OSError):
            self.path(slot).unlink()


def _write_atomic(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(text, encoding="utf-8")
    for attempt in range(20):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:  # a reader holds the target open for a moment
            time.sleep(0.05 * (attempt + 1))
    path.write_text(text, encoding="utf-8")
    with contextlib.suppress(OSError):
        tmp.unlink()


def _utc() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="seconds")


def _status(slot_dir: Path) -> int:
    pool = SlotPool(slot_dir)
    with pool.lock():
        live, reclaimed = pool.live_slots()
    usage = {s.seq: job_commit_bytes(s.job_name) for s in live if s.state == "running"}
    memory = host_memory()
    print(json.dumps({"slot_dir": str(slot_dir), "host": memory, "reclaimed": reclaimed,
                      "running_caps_gb": round(sum(s.cap_bytes for s in live if s.state == "running") / GIB, 4),
                      "slots": [{**s.as_dict(), "cap_gb": round(s.cap_bytes / GIB, 4),
                                 "live_commit_gb": None if usage.get(s.seq) is None
                                 else round(usage[s.seq] / GIB, 4)} for s in live]}, indent=1))
    return 0


# ---------------------------------------------------------------------------
# Supervisor
# ---------------------------------------------------------------------------

def main(argv: Sequence[str] | None = None) -> int:
    if os.name != "nt":
        raise SystemExit("This guard requires Windows; refusing an unguarded launch.")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--job-gb", type=float, default=0.6,
                        help="job commit cap in GiB, (0, 1.0]; probes/pytest/research 0.6, writer slices 0.8, "
                             "set-based passes 1.0")
    parser.add_argument("--heavy", action="store_true", help="HEAVY job: at most one runs host-wide")
    parser.add_argument("--wait-minutes", type=float, default=0.0,
                        help="queue FIFO up to N minutes for admission and a slot (0: try once)")
    parser.add_argument("--receipt", type=Path, help="JSON receipt path (must not exist; default: auto)")
    parser.add_argument("--stdout", type=Path)
    parser.add_argument("--stderr", type=Path)
    parser.add_argument("--disk-path", type=Path)
    parser.add_argument("--min-free-disk-gb", type=float)
    parser.add_argument("--slot-dir", type=Path, help=f"host-wide slot directory (default ${SLOT_DIR_ENV} or "
                                                      "%%LOCALAPPDATA%%/atx-memory-guard/slots)")
    parser.add_argument("--test-stop-commit-gb", type=float,
                        help="TEST ONLY: raise the in-run free-commit stop threshold above 0.75 GiB")
    parser.add_argument("--allow-nested-guards", action="store_true",
                        help="orchestrator mode: the command may start further guarded jobs (research "
                             "workers.run_jobs) outside this job; each of them takes its own cap and slot")
    parser.add_argument("--quiet", action="store_true", help="do not echo the receipt JSON to stdout")
    parser.add_argument("--status", action="store_true", help="print the live slots and host memory, then exit")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    slot_dir = args.slot_dir or default_slot_dir()
    if args.status:
        return _status(slot_dir)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("Supply a command after --.")
    if not math.isfinite(args.job_gb) or not 0 < args.job_gb <= MAX_JOB_GB:
        parser.error(f"--job-gb must be in (0, {MAX_JOB_GB}] GiB (caps above 1.0 GiB are forbidden, C-58).")
    if not math.isfinite(args.wait_minutes) or not 0 <= args.wait_minutes <= MAX_WAIT_MINUTES:
        parser.error(f"--wait-minutes must be in [0, {MAX_WAIT_MINUTES}].")
    stop_commit_gb = STOP_COMMIT_GB
    if args.test_stop_commit_gb is not None:
        if not math.isfinite(args.test_stop_commit_gb) or not STOP_COMMIT_GB <= args.test_stop_commit_gb <= 64:
            parser.error("--test-stop-commit-gb may only raise the stop threshold (0.75..64 GiB).")
        stop_commit_gb = args.test_stop_commit_gb
    try:
        validate_disk_options(args.disk_path, args.min_free_disk_gb)
    except ValueError as exc:
        parser.error(str(exc))
    receipt: Path = args.receipt or guard_home() / "receipts" / (
        dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%S") + f"-{os.getpid()}.json")
    if receipt.exists():
        raise SystemExit(f"Receipt already exists: {receipt}; use a new run filename.")

    _api()
    cap_request = page_aligned_cap(args.job_gb)
    own_pid = os.getpid()
    own_created = process_created(own_pid) or 0
    state: dict[str, Any] = {
        "guard_version": GUARD_VERSION, "command": command, "job_limit_gb": args.job_gb,
        "job_cap_bytes": cap_request, "heavy": args.heavy, "allow_nested_guards": args.allow_nested_guards,
        "status": "preflight", "guard_pid": own_pid,
        "receipt": str(receipt), "headroom": host_memory(),
        "thresholds": {"admission_reserve_gb": ADMISSION_RESERVE_GB, "slot_budget_gb": SLOT_BUDGET_GB,
                       "stop_commit_gb": stop_commit_gb, "stop_physical_gb": STOP_PHYSICAL_GB,
                       "thrash_seconds": THRASH_SECONDS, "test_override": args.test_stop_commit_gb is not None},
        "child_env_forced": CHILD_ENV, "native_peak_job_memory_gb": None, "cap_hit": False,
    }
    job: Job | None = None

    def record(**updates: Any) -> None:
        state.update(updates)
        if job is not None:
            peak = job.peak_bytes()
            if peak is not None:
                state["native_peak_job_memory_bytes"] = peak
                state["native_peak_job_memory_gb"] = round(peak / GIB, 4)
            job.drain_messages()
            state["cap_hit_events"] = job.cap_hit_events
            state["cap_hit"] = job.cap_hit_events > 0
        receipt.parent.mkdir(parents=True, exist_ok=True)
        _write_atomic(receipt, json.dumps(state, indent=2, default=str) + "\n")
        if not args.quiet:
            print(json.dumps(state, default=str), flush=True)

    try:
        job = Job(f"Local\\atx-memory-guard-{own_pid}-{own_created}", cap_request,
                  allow_nested_guards=args.allow_nested_guards)
    except (OSError, RuntimeError) as error:
        record(status="job_object_failed", error=repr(error))
        return EXIT_UNGUARDED
    state["job_cap_bytes"] = job.cap_bytes
    if args.disk_path is not None:
        state["disk"], disk_failure = sample_disk(args.disk_path, args.min_free_disk_gb)
        if disk_failure is not None:
            record(status=f"refused_{disk_failure}")
            return EXIT_NOT_ADMITTED

    pool = SlotPool(slot_dir)
    requested = time.monotonic()
    admission: dict[str, Any] = {"slot_dir": str(slot_dir), "wait_limit_minutes": args.wait_minutes,
                                 "requested_at_utc": _utc(), "reclaimed_slots": []}
    state["admission"] = admission
    with pool.lock():
        live, reclaimed = pool.live_slots()
        admission["reclaimed_slots"] += reclaimed
        outer = [s for s in live if s.state == "running" and inside_job(s.job_name)]
        if outer:
            # Nested inside another guard's job: that job's cap would bind this one too. An orchestrator
            # that starts guarded jobs runs with --allow-nested-guards, whose launches leave its job.
            record(status="refused_nested_in_guard", nested_in={"seq": outer[0].seq, "pid": outer[0].pid,
                                                                "cap_gb": round(outer[0].cap_bytes / GIB, 4)})
            return EXIT_NOT_ADMITTED
        me = Slot(seq=pool.next_seq(), pid=own_pid, created=own_created, state="waiting", cap_bytes=job.cap_bytes,
                  heavy=args.heavy, job_name=job.name, receipt=str(receipt), enqueued_at_utc=admission["requested_at_utc"],
                  command_head=" ".join(command)[:200])
        pool.write(me)
    admission["seq"] = me.seq
    deadline = requested + args.wait_minutes * 60
    next_note = 0.0
    try:
        while True:
            with pool.lock():
                live, reclaimed = pool.live_slots()
                admission["reclaimed_slots"] += reclaimed
                if not any(s.seq == me.seq for s in live):
                    pool.write(me)  # the directory was cleared under us: re-register with the same ticket
                usage = {s.seq: job_commit_bytes(s.job_name) for s in live if s.state == "running"}
                memory = host_memory()
                admitted, reasons, detail = decide(me, live, memory["commit_free_bytes"], usage)
                if admitted:
                    me.state, me.admitted_at_utc = "running", _utc()
                    pool.write(me)
            admission.update(last_reasons=reasons, last_detail=detail,
                             wait_seconds=round(time.monotonic() - requested, 1))
            seen = admission.setdefault("reasons_seen", [])
            seen += [reason for reason in reasons if reason not in seen]
            if admitted:
                admission["admitted_at_utc"] = me.admitted_at_utc
                break
            now = time.monotonic()
            if now >= deadline:
                record(status="refused_wait_timeout", headroom=memory)
                return EXIT_NOT_ADMITTED
            if now >= next_note:
                record(status="queued", headroom=memory)
                next_note = now + RECORD_SECONDS
            time.sleep(POLL_SECONDS)

        stdout = args.stdout.open("xb") if args.stdout else None
        stderr = args.stderr.open("xb") if args.stderr else None
        env = {**os.environ, **CHILD_ENV}
        started = time.monotonic()
        minimum = {"commit_free_gb": memory["commit_free_gb"], "physical_free_gb": memory["physical_free_gb"]}
        state["min_headroom"] = minimum
        # CreateProcess does not resolve a relative program path written with forward slashes.
        executable = str(Path(command[0]).resolve()) if Path(command[0]).is_file() else None
        try:
            child = subprocess.Popen(command, executable=executable, stdin=subprocess.DEVNULL, stdout=stdout,
                                     stderr=stderr, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
        except OSError as error:
            record(status="launch_failed", error=repr(error)[:500], headroom=memory)
            return EXIT_LAUNCH_FAILED
        with child:
            record(status="running", child_pid=child.pid, headroom=memory)
            next_report = time.monotonic() + RECORD_SECONDS
            low_physical_since: float | None = None
            while child.poll() is None:
                memory = host_memory()
                minimum["commit_free_gb"] = min(minimum["commit_free_gb"], memory["commit_free_gb"])
                minimum["physical_free_gb"] = min(minimum["physical_free_gb"], memory["physical_free_gb"])
                now = time.monotonic()
                stop: str | None = None
                if memory["commit_free_gb"] < stop_commit_gb:
                    stop = "low_commit"
                if memory["physical_free_gb"] < STOP_PHYSICAL_GB:
                    low_physical_since = now if low_physical_since is None else low_physical_since
                    if now - low_physical_since >= THRASH_SECONDS:
                        stop = stop or "thrash"
                else:
                    low_physical_since = None
                if stop is None and args.disk_path is not None:
                    state["disk"], disk_failure = sample_disk(args.disk_path, args.min_free_disk_gb)
                    stop = disk_failure
                if stop is not None:
                    record(status=f"stopped_{stop}", stop_reason=stop, returncode=EXIT_STOPPED, headroom=memory,
                           run_seconds=round(now - started, 1))
                    with pool.lock():
                        pool.remove(me)
                    # Terminates only this explicitly assigned job: the command and this supervisor.
                    job.terminate(EXIT_STOPPED)
                    return EXIT_STOPPED
                if now >= next_report:
                    record(headroom=memory)
                    next_report = now + RECORD_SECONDS
                time.sleep(POLL_SECONDS)
            code = child.returncode
            record(status="completed" if code == 0 else "failed", returncode=code, headroom=host_memory(),
                   run_seconds=round(time.monotonic() - started, 1))
        for handle in (stdout, stderr):
            if handle:
                handle.close()
        # Keep the job handle open until process exit; closing it earlier would terminate this supervisor
        # too. The OS then cleans up any leftover children.
        return code
    except BaseException as error:
        if state.get("status") in ("preflight", "queued", "running"):
            record(status="interrupted", error=repr(error)[:500])
        raise
    finally:
        with contextlib.suppress(Exception), pool.lock(timeout_s=10):
            pool.remove(me)


if __name__ == "__main__":
    raise SystemExit(main())
