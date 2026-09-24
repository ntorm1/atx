"""Observe the existing long-job launch policy; never starts a workload."""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import json
from pathlib import Path
import time

from run_memory_guarded import GIB, Performance


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--physical-gib", type=float, default=6,
                        help="Explicit experiment floor; production default remains 6 GiB")
    parser.add_argument("--commit-gib", type=float, default=8,
                        help="Explicit experiment floor; production default remains 8 GiB")
    args = parser.parse_args()
    if not 3 <= args.physical_gib <= 64 or not 5 <= args.commit_gib <= 128:
        parser.error("Observation floors must retain at least 3 GiB physical / 5 GiB commit")
    if args.receipt.exists():
        raise SystemExit("Refusing to overwrite an observation receipt")
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetPerformanceInfo.argtypes = [ctypes.POINTER(Performance), ctypes.c_ulong]
    psapi.GetPerformanceInfo.restype = ctypes.c_int
    result = {"required_physical_gib": args.physical_gib, "required_commit_gib": args.commit_gib,
              "required_seconds": 120, "sample_interval_seconds": 10,
              "maximum_wait_seconds": 180, "samples": [], "status": "observing"}
    started = time.monotonic()
    qualified_since = None
    while True:
        info = Performance()
        info.cb = ctypes.sizeof(info)
        if not psapi.GetPerformanceInfo(ctypes.byref(info), ctypes.sizeof(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        physical = info.physical_available * info.page_size / GIB
        commit = (info.commit_limit - info.commit_total) * info.page_size / GIB
        now = time.monotonic()
        if physical >= args.physical_gib and commit >= args.commit_gib:
            qualified_since = now if qualified_since is None else qualified_since
        else:
            qualified_since = None
        duration = 0 if qualified_since is None else now - qualified_since
        sample = {"utc": dt.datetime.now(dt.UTC).isoformat(),
                  "physical_free_gib": physical, "commit_free_gib": commit,
                  "qualifying_seconds": duration}
        result["samples"].append(sample)
        print(json.dumps(sample), flush=True)
        if duration >= 120:
            result["status"] = "ready"
        elif now - started >= 180:
            result["status"] = "no_sustained_window"
        args.receipt.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        if result["status"] != "observing":
            print(json.dumps({"status": result["status"]}), flush=True)
            return 0 if result["status"] == "ready" else 78
        time.sleep(10)


if __name__ == "__main__":
    raise SystemExit(main())
