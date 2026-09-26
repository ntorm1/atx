#!/usr/bin/env python3
"""Run one owned research process with time/RAM limits and an immutable receipt.

Arguments after -- are passed directly to the process, never through a shell.
Outputs must be new directories inside this worktree. This requires psutil and
does not authorize concurrent compilation or turn a smoke run into alpha evidence.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import time

import psutil


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def stop_owned(process: psutil.Process) -> None:
    """psutil checks creation time before signaling, protecting against PID reuse."""
    try:
        owned = process.children(recursive=True) + [process]
    except psutil.NoSuchProcess:
        return
    for child in reversed(owned):
        try:
            child.terminate()
        except psutil.NoSuchProcess:
            pass
    _, alive = psutil.wait_procs(owned, timeout=2)
    for child in alive:
        try:
            child.kill()
        except psutil.NoSuchProcess:
            pass
    psutil.wait_procs(alive, timeout=2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=180)
    parser.add_argument("--max-rss-mib", type=int, default=1024)
    parser.add_argument("--min-free-mib", type=int, default=256)
    parser.add_argument("--bind", type=Path, action="append", default=[])
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if (not command or not math.isfinite(args.seconds) or not 0 < args.seconds <= 600
            or not 32 <= args.max_rss_mib <= 8192 or not 64 <= args.min_free_mib <= 8192):
        parser.error("require a command, <=600 seconds, and explicit bounded RAM limits")
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if not output.is_relative_to(root) or output == root:
        parser.error("output must be a new directory inside this worktree")
    executable = shutil.which(command[0])
    if executable is None:
        parser.error("executable was not found")
    command[0] = str(Path(executable).resolve())
    bindings = []
    for path in args.bind:
        path = path.resolve(strict=True)
        if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
            parser.error("bind small config/source manifests, not full data payloads")
        bindings.append({"path": str(path), "sha256": digest(path)})
    source = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True)
    if dirty.strip():
        parser.error("commit the source before running a recorded research experiment")
    output.mkdir(parents=True, exist_ok=False)
    receipt = dict(schema="atx.bounded-research-run/v1", source_sha=source,
        started_utc=dt.datetime.now(dt.timezone.utc).isoformat(), command=command,
        executable_sha256=digest(Path(command[0])), bindings=bindings,
        limits=dict(seconds=args.seconds, max_rss_mib=args.max_rss_mib,
                    min_free_mib=args.min_free_mib), sampled_peak_tree_rss_bytes=0,
        minimum_system_free_bytes=psutil.virtual_memory().available,
        outcome="launch-failed", exit_code=None)
    (output / "start.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    started = time.monotonic()
    child = None
    owned = None
    try:
        with (output / "stdout.log").open("xb") as stdout, (output / "stderr.log").open("xb") as stderr:
            child = subprocess.Popen(command, cwd=root, stdout=stdout, stderr=stderr,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            owned = psutil.Process(child.pid)
            while child.poll() is None:
                try:
                    tree = [owned] + owned.children(recursive=True)
                    rss = sum(p.memory_info().rss for p in tree if p.is_running())
                except psutil.NoSuchProcess:
                    rss = 0
                free = psutil.virtual_memory().available
                receipt["sampled_peak_tree_rss_bytes"] = max(receipt["sampled_peak_tree_rss_bytes"], rss)
                receipt["minimum_system_free_bytes"] = min(receipt["minimum_system_free_bytes"], free)
                reason = ("time-limit" if time.monotonic() - started >= args.seconds else
                          "rss-limit" if rss > args.max_rss_mib * 1024**2 else
                          "system-memory-limit" if free < args.min_free_mib * 1024**2 else "")
                if reason:
                    receipt["outcome"] = reason
                    stop_owned(owned)
                    break
                time.sleep(0.25)
            receipt["exit_code"] = child.wait(timeout=5)
            if receipt["outcome"] == "launch-failed":
                receipt["outcome"] = "completed" if child.returncode == 0 else "process-error"
    except BaseException as exc:
        receipt["outcome"] = "interrupted" if isinstance(exc, KeyboardInterrupt) else "runner-error"
        receipt["error"] = str(exc)
        if owned is not None:
            stop_owned(owned)
        if child is not None:
            receipt["exit_code"] = child.wait(timeout=5)
    finally:
        receipt["wall_seconds"] = time.monotonic() - started
        receipt["logs"] = {name: digest(output / name) for name in ("stdout.log", "stderr.log")
                           if (output / name).exists()}
        with (output / "receipt.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2)
            stream.write("\n")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["outcome"] == "completed" else 1


if __name__ == "__main__":
    sys.exit(main())
