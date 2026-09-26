"""Memory guard (C-58, node M0): a killed guard's slot is reclaimed and the next HEAVY waiter is admitted."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

GUARD = Path(__file__).resolve().parents[2] / ".superpowers/sdd/tier1-parity/run_memory_guarded.py"


def _receipt(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _until(predicate, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.2)
    return False


@pytest.mark.skipif(os.name != "nt", reason="the guard is a Windows job object")
def test_killed_heavy_holder_slot_is_reclaimed_and_queued_heavy_waiter_runs(tmp_path):
    slots = tmp_path / "slots"
    common = [sys.executable, str(GUARD), "--job-gb", "0.2", "--heavy", "--slot-dir", str(slots), "--quiet"]
    holder_receipt, waiter_receipt = tmp_path / "holder.json", tmp_path / "waiter.json"
    holder = subprocess.Popen([*common, "--receipt", str(holder_receipt), "--",
                               sys.executable, "-c", "import time; time.sleep(120)"])
    waiter = None
    try:
        assert _until(lambda: _receipt(holder_receipt).get("status") == "running", 60)
        waiter = subprocess.Popen([*common, "--wait-minutes", "2", "--receipt", str(waiter_receipt), "--",
                                   sys.executable, "-c", "print('admitted')"])
        assert _until(lambda: "heavy_running" in (_receipt(waiter_receipt).get("admission") or {})
                      .get("reasons_seen", []), 60)
        holder.kill()  # the supervisor dies without releasing its slot; kill-on-close ends its sleeper
        holder.wait(30)
        assert waiter.wait(90) == 0
    finally:
        for process in (holder, waiter):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(30)
    receipt = _receipt(waiter_receipt)
    assert receipt["status"] == "completed" and receipt["returncode"] == 0
    # guard_pid is the supervisor itself (a venv python.exe is a launcher that starts it as a child).
    assert [slot["pid"] for slot in receipt["admission"]["reclaimed_slots"]] == [_receipt(holder_receipt)["guard_pid"]]
    assert receipt["native_peak_job_memory_gb"] is not None
    assert list(slots.glob("slot-*.json")) == []
