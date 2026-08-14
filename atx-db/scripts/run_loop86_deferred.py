#!/usr/bin/env python
"""Finish Loop 86 after a detached broad-bar loader releases the warehouse."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import time
from pathlib import Path

FACTOR_ID = "earnings_sue_price_momentum_residual_12_1"


def _atomic_json(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, sort_keys=True, indent=2, default=str) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def _pid_exists(pid: int) -> bool:
    if os.name == "nt":
        import ctypes

        process_query_limited_information = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(
            process_query_limited_information, False, pid
        )
        if not handle:
            return False
        ctypes.windll.kernel32.CloseHandle(handle)
        return True
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _run(
    command: list[str],
    *,
    cwd: Path,
    log_path: Path,
    accepted_codes: tuple[int, ...] = (0,),
) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8", newline="\n") as log:
        completed = subprocess.run(
            command,
            cwd=cwd,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=False,
        )
    if completed.returncode not in accepted_codes:
        raise RuntimeError(
            f"command failed with exit code {completed.returncode}: {' '.join(command)}"
        )
    return completed.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wait-pid", type=int, required=True)
    parser.add_argument("--maximum-wait-seconds", type=int, default=21_600)
    parser.add_argument(
        "--loader-stdout",
        type=Path,
        default=Path("data/logs/broad-bars-resume-canonical.stdout.log"),
    )
    parser.add_argument(
        "--loader-stderr",
        type=Path,
        default=Path("data/logs/broad-bars-resume-canonical.stderr.log"),
    )
    parser.add_argument(
        "--loader-success-marker",
        default='"equity_daily_bars_rows"',
    )
    parser.add_argument(
        "--status-path",
        type=Path,
        default=Path("research/loop86-deferred-status.json"),
    )
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    factor_repo = repo.parent / "atx-factor"
    warehouse = repo / "data" / "warehouse.duckdb"
    started_at = dt.datetime.now(dt.UTC)
    status: dict[str, object] = {
        "schema_version": 1,
        "state": "waiting_for_breadth_loader",
        "wait_pid": args.wait_pid,
        "started_at": started_at.isoformat(),
        "factor_id": FACTOR_ID,
    }
    _atomic_json(repo / args.status_path, status)

    deadline = time.monotonic() + args.maximum_wait_seconds
    while _pid_exists(args.wait_pid):
        if time.monotonic() >= deadline:
            status.update(state="timed_out", finished_at=dt.datetime.now(dt.UTC).isoformat())
            _atomic_json(repo / args.status_path, status)
            return 3
        time.sleep(10)

    loader_stdout = (repo / args.loader_stdout).read_text(
        encoding="utf-8", errors="replace"
    )
    loader_stderr = (repo / args.loader_stderr).read_text(
        encoding="utf-8", errors="replace"
    )
    if "Traceback (most recent call last)" in loader_stderr or (
        args.loader_success_marker not in loader_stdout
    ):
        status.update(
            state="breadth_loader_failed",
            finished_at=dt.datetime.now(dt.UTC).isoformat(),
        )
        _atomic_json(repo / args.status_path, status)
        return 4

    status["state"] = "replaying_l1vsun_13f_analysis"
    _atomic_json(repo / args.status_path, status)
    _run(
        [
            str(repo / ".venv/Scripts/atx-db.exe"),
            "backtest-13f-signals",
            "--run-id",
            "loop86-broad-price-replay",
        ],
        cwd=repo,
        log_path=repo / "data/logs/loop86-l1vsun-backtest.log",
    )
    _run(
        [
            sys.executable,
            "scripts/recreate_l1vsun_13f_analysis.py",
            "--json-output",
            "research/recreated-l1vsun-13f-amendment-analysis.json",
        ],
        cwd=repo,
        log_path=repo / "data/logs/loop86-l1vsun-report.log",
    )

    status["state"] = "building_factor"
    _atomic_json(repo / args.status_path, status)
    _run(
        [
            sys.executable,
            "scripts/build_fundamental_momentum.py",
            "--run-id",
            "loop86-price-controlled-fundamental-momentum-v1",
        ],
        cwd=repo,
        log_path=repo / "data/logs/loop86-fundamental-momentum-build.log",
    )

    status["state"] = "running_ic_diagnostics"
    _atomic_json(repo / args.status_path, status)
    _run(
        [
            sys.executable,
            "scripts/evaluate_signals.py",
            "--factor-ids",
            FACTOR_ID,
            "--horizons",
            "21",
            "63",
            "126",
            "252",
            "--run-id",
            "loop86-price-controlled-fundamental-momentum-ic",
            "--screen-only",
        ],
        cwd=repo,
        log_path=repo / "data/logs/loop86-fundamental-momentum-ic.log",
    )

    status["state"] = "running_portfolio_exploration"
    _atomic_json(repo / args.status_path, status)
    exploration_code = _run(
        [
            str(factor_repo / ".venv/Scripts/atx-factor.exe"),
            "explore-candidate",
            "--db-path",
            str(warehouse),
            "--candidate",
            FACTOR_ID,
            "--trial-ledger",
            "research/trial-ledger.json",
            "--output",
            "research/loop86-fundamental-momentum-exploration.json",
            "--registry",
            "research/mega-alpha-registry.json",
            "--shadow-registry",
            "research/shadow-alpha-registry.json",
            "--summary-only",
        ],
        cwd=factor_repo,
        log_path=repo / "data/logs/loop86-fundamental-momentum-exploration.log",
        accepted_codes=(0, 2),
    )
    status.update(
        state="complete",
        exploration_exit_code=exploration_code,
        finished_at=dt.datetime.now(dt.UTC).isoformat(),
    )
    _atomic_json(repo / args.status_path, status)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
