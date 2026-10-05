"""Record the CPU affinity of every atx-equity-strategy-risk.exe process that appears while a run is live (C3 of M2i:
the 1-CPU leg is launched with `start /affinity 1`, which the bounded runner and its child inherit). Polls every 0.25 s
until SECONDS pass or STOP_FILE exists; one line per process seen (pid, affinity, the exe's --output argument, its
parent's affinity). Reads no output file.

usage: python aff_probe.py OUT_FILE SECONDS STOP_FILE
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import psutil


def main(argv: list[str]) -> int:
    out, seconds, stop = Path(argv[0]), float(argv[1]), Path(argv[2])
    seen: dict[int, str] = {}
    t0 = time.time()
    while time.time() - t0 < seconds and not stop.exists():
        for p in psutil.process_iter(["pid", "name"]):
            if (p.info["name"] or "").lower() != "atx-equity-strategy-risk.exe" or p.pid in seen:
                continue
            try:
                cmd = p.cmdline()
                outdir = cmd[cmd.index("--output") + 1] if "--output" in cmd else "?"
                parent = p.parent()
                line = (f"pid {p.pid} affinity {p.cpu_affinity()} output {outdir} parent {parent.pid if parent else '?'} "
                        f"{parent.name() if parent else ''} parent-affinity {parent.cpu_affinity() if parent else '?'} "
                        f"at +{time.time() - t0:.1f}s")
            except (psutil.Error, ValueError) as e:
                line = f"pid {p.pid} probe error {e!r}"
            seen[p.pid] = line
            with out.open("a", encoding="utf-8") as fh:
                fh.write(line + "\n")
        time.sleep(0.25)
    with out.open("a", encoding="utf-8") as fh:
        fh.write(f"probe end after {time.time() - t0:.1f}s, processes seen {len(seen)}, cpus on host "
                 f"{psutil.cpu_count()}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
