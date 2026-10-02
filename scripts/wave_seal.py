"""The hidden-data scan of one research wave: every log the wave produced, searched for a date at or after the
research seal (research_window.SEAL_DATE). Counts and file names only; no log content is reported.

The logs (``wave_logs``): every attempt's run dirs of each spec the wave ran (the screen library, the cell, its -gm
copy: u, fit, w, card, marginal, ref, nav, monitor, check, summ), the wave's reader and bundle runs, and the console
of every command the driver ran (<out_dir>/consoles/: add-alpha with its K1 plan, research_cycle, git). The verify
stage scans them before any return is read; the record stage scans them again, after the judge's runs, before it
writes the hidden-data line.
"""
from __future__ import annotations

import re

from wave_context import Wave
import research_window as RW   # atx-engine/tools (wave_context puts it on the path): the one source of the seal

DATE = re.compile(r"(?<![0-9])((?:19|20)[0-9]{2}-[01][0-9]-[0-3][0-9])(?![0-9])")
LOG_NAMES = ("stdout.log", "stderr.log")


def wave_specs(done: dict) -> list[str]:
    """The cell specs the wave ran so far: the screen library, the cell, the -gm copy (each once, in that order)."""
    out = []
    for spec in ((done.get("screen") or {}).get("spec"), (done.get("spec") or {}).get("cell_spec"),
                 (done.get("match") or {}).get("cell_spec")):
        if spec and spec not in out:
            out.append(spec)
    return out


def wave_logs(w: Wave, done: dict) -> list[str]:
    """Every log file the wave produced so far (root-relative, sorted, each once)."""
    dirs = []
    for spec in wave_specs(done):
        for base in w.phase_bases(spec).values():
            dirs += w.run_dirs(base)
    dirs += w.run_dirs(w.wave_path("bundle"))
    readers = w.path(w.wave_path("readers"))
    dirs += [w.rel(p) for p in sorted(readers.glob("*-run*")) if p.is_dir()] if readers.is_dir() else []
    files = {f"{d}/{n}" for d in dirs for n in LOG_NAMES if w.path(f"{d}/{n}").is_file()}
    consoles = w.path(w.wave_path("consoles"))
    files |= {w.rel(p) for p in consoles.glob("*.log")} if consoles.is_dir() else set()
    return sorted(files)


def scan(w: Wave, files: list[str]) -> dict:
    """{files, seal, tokens_at_or_after_seal, where[{file, tokens}]} of ``files``."""
    hits = []
    for rel in files:
        text = w.path(rel).read_text(encoding="utf-8", errors="replace")
        n = sum(1 for t in DATE.findall(text) if t >= RW.SEAL_DATE)
        if n:
            hits.append({"file": rel, "tokens": n})
    return {"files": len(files), "seal": RW.SEAL_DATE, "tokens_at_or_after_seal": sum(h["tokens"] for h in hits),
            "where": hits}


def problems(seal: dict) -> list[str]:
    if not seal["tokens_at_or_after_seal"]:
        return []
    return [f"{seal['tokens_at_or_after_seal']} date token(s) at or after the seal {RW.SEAL_DATE} in "
            f"{[h['file'] for h in seal['where']]} (inspect: a data date is a seal breach)"]
