"""One bounded in-memory probe of DuckDB builtin parameter-binding overhead.

No warehouse, source archive, pandas API, or persistent DuckDB is opened.
Run once under the existing Windows guard with a <=0.75 GiB process-tree cap.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import json
import os
import sys
from ctypes import wintypes
from pathlib import Path


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
            "PagefileUsage", "PeakPagefileUsage", "PrivateUsage",
        )
    ]


def _private_memory() -> dict[str, int | float]:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCounters), wintypes.DWORD,
    ]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    counters = ProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return {"private_bytes": counters.PrivateUsage,
            "private_gib": counters.PrivateUsage / 1024**3,
            "working_set_bytes": counters.WorkingSetSize,
            "peak_working_set_bytes": counters.PeakWorkingSetSize}


def _literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        raise RuntimeError("The probe requires Windows private-byte accounting")
    if args.output.exists():
        raise FileExistsError("Refusing to overwrite a probe receipt")
    report = {"pid": os.getpid(), "started_at": dt.datetime.now(dt.UTC).isoformat(),
              "duckdb_memory_limit": "64MB", "threads": 1, "database": ":memory:",
              "status": "running", "snapshots": [], "comparisons": {}}

    def persist() -> None:
        with args.output.open("w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, default=str)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())

    def observe(label: str) -> None:
        snapshot = {"label": label, **_private_memory()}
        for library in ("pandas", "numpy"):
            modules = [name for name in sys.modules if name == library or name.startswith(library + ".")]
            snapshot[library + "_loaded"] = library in sys.modules
            snapshot[library + "_module_count"] = len(modules)
        report["snapshots"].append(snapshot)
        persist()

    observe("before_duckdb_import")
    try:
        import duckdb

        report["duckdb_version"] = duckdb.__version__
        observe("after_duckdb_import")
        with duckdb.connect(":memory:", config={
            "memory_limit": "64MB", "threads": "1", "preserve_insertion_order": "false",
        }) as con:
            con.execute("SET TimeZone='UTC'")
            observe("after_connect_and_literal_setup")
            run_id = "operator's activation-companyfacts-archive18"
            timestamp = dt.datetime(2026, 9, 24, 23, 59, 8, 123456, tzinfo=dt.UTC)
            utc_text = timestamp.replace(tzinfo=None).isoformat(sep=" ", timespec="microseconds")
            expected = con.execute(
                f"SELECT CAST({_literal(run_id)} AS VARCHAR), TIMESTAMP {_literal(utc_text)}"
            ).fetchone()
            observe("after_sql_literals")
            bound_string = con.execute("SELECT CAST(? AS VARCHAR)", [run_id]).fetchone()
            report["comparisons"]["builtin_string_equal"] = bound_string == (expected[0],)
            observe("after_bound_builtin_string")
            bound_datetime = con.execute("SELECT CAST(? AS TIMESTAMP)", [timestamp]).fetchone()
            report["comparisons"]["builtin_utc_datetime_equal"] = bound_datetime == (expected[1],)
            observe("after_bound_builtin_datetime")
            combined = con.execute("SELECT CAST(? AS VARCHAR), CAST(? AS TIMESTAMP)", [run_id, timestamp]).fetchone()
            report["comparisons"]["combined_equal"] = combined == expected
            report["literal_result"] = expected
            report["bound_result"] = combined
            observe("after_combined_binding")
        observe("after_connection_close")
        assert all(report["comparisons"].values()), "Literal and bound builtin values differ"
        report.update(status="completed", finished_at=dt.datetime.now(dt.UTC).isoformat())
        persist()
        print(json.dumps(report, default=str))
        return 0
    except BaseException as exc:
        report.update(status="failed", error_type=type(exc).__name__, error=str(exc),
                      finished_at=dt.datetime.now(dt.UTC).isoformat())
        persist()
        raise


if __name__ == "__main__":
    raise SystemExit(main())
