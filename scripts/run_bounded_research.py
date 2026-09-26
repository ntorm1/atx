#!/usr/bin/env python3
"""Run one owned research process with time/RAM limits and an immutable receipt.

Arguments after -- are passed directly to the process, never through a shell.
Outputs must be new directories inside this worktree. This requires psutil and
does not authorize concurrent compilation or turn a smoke run into alpha evidence.
This is a sampled operational guard, not a process sandbox: supported research
commands must not detach children between samples. Observed descendants remain
owned and monitored after the direct child exits.
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


def live_owned(owned: dict) -> list[psutil.Process]:
    for process in list(owned.values()):
        try:
            if process.is_running():
                for child in process.children(recursive=True):
                    owned[(child.pid, child.create_time())] = child
        except psutil.NoSuchProcess:
            pass
    return [process for process in owned.values() if process.is_running()]


def stop_owned(owned: dict) -> None:
    """psutil checks creation time before signaling, protecting against PID reuse."""
    live = live_owned(owned)
    for child in reversed(live):
        try:
            child.terminate()
        except psutil.NoSuchProcess:
            pass
    _, alive = psutil.wait_procs(live, timeout=2)
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
    owned = {}
    try:
        with (output / "stdout.log").open("xb") as stdout, (output / "stderr.log").open("xb") as stderr:
            if psutil.virtual_memory().available < args.min_free_mib * 1024**2:
                receipt["outcome"] = "prelaunch-memory-refusal"
                raise RuntimeError("available system memory is below the launch floor")
            child = subprocess.Popen(command, cwd=root, stdout=stdout, stderr=stderr,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            process = psutil.Process(child.pid)
            owned[(process.pid, process.create_time())] = process
            while True:
                tree = live_owned(owned)
                if child.poll() is not None and not tree:
                    break
                rss = 0
                for process in tree:
                    try:
                        rss += process.memory_info().rss
                    except psutil.NoSuchProcess:
                        pass
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
        if receipt["outcome"] != "prelaunch-memory-refusal":
            receipt["outcome"] = "interrupted" if isinstance(exc, KeyboardInterrupt) else "runner-error"
        receipt["error"] = str(exc)
        if owned:
            stop_owned(owned)
        if child is not None:
            receipt["exit_code"] = child.wait(timeout=5)
    finally:
        receipt["wall_seconds"] = time.monotonic() - started
        receipt["owned_processes"] = [{"pid": pid, "create_time": born} for pid, born in owned]
        receipt["ownership_scope"] = "sampled descendant identities; commands must not detach unsampled children"
        receipt["logs"] = {name: digest(output / name) for name in ("stdout.log", "stderr.log")
                           if (output / name).exists()}
        with (output / "receipt.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2)
            stream.write("\n")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["outcome"] == "completed" else 1


if __name__ == "__main__":
    sys.exit(main())
