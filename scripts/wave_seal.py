"""The hidden-data scan of one research wave: every log the wave produced, searched for a date at or after the
research seal (research_window.SEAL_DATE). Counts, token forms and file names only; no log content is reported.

The logs (``wave_logs``): every attempt's run dirs of each spec the wave ran (the screen library, the cell, its -gm
copy: u, fit, w, card, marginal, ref, nav, monitor, check, summ), the wave's reader and bundle runs, and the console
of every command the driver ran (<out_dir>/consoles/: add-alpha with its K1 plan, research_cycle, git). The verify
stage scans them before any return is read; the record stage scans them again, after the judge's runs, before it
writes the hidden-data line. Every attempt is scanned (a failed attempt may have read data before it failed).

Date forms (``tokens``; each token's date: a year's Jan 1, a quarter's first day):
  iso       YYYY-MM-DD
  compact   YYYYMMDD with a real month and day, not inside a word or a number (no letter, digit or '.' either side)
  year      year=YYYY / year: YYYY
  quarter   YYYYQn / YYYY-Qn (n 1-4), not inside a word
Classes of a token dated at or after the seal (``scan``):
  allowed         the token is on the allow list: the bootstrap seeds (SEEDS) and the run's rulings
                  (research_wave.py --seal-allow TOKEN=RULING, kept by the verify receipt for the record stage);
                  recorded with its ruling, count and files
  seal_reference  the seal date itself on a line that names the seal (a guard refusing: "... at or after the research
                  seal 2024-01-01 ...", a fields manifest's seal.exclusive_end): the guard working, counted
  hit             anything else: a stop for the PM's ruling
"""
from __future__ import annotations

import re

from wave_context import Wave
import research_window as RW   # atx-engine/tools (wave_context puts it on the path): the one source of the seal

LOG_NAMES = ("stdout.log", "stderr.log")
FORMS = {"iso": re.compile(r"(?<![0-9])((?:19|20)[0-9]{2})-([01][0-9])-([0-3][0-9])(?![0-9])"),
         "compact": re.compile(r"(?<![A-Za-z0-9.])((?:19|20)[0-9]{2})([01][0-9])([0-3][0-9])(?![A-Za-z0-9.])"),
         "year": re.compile(r"(?<![A-Za-z0-9_])year\s*[=:]\s*((?:19|20)[0-9]{2})(?![0-9])", re.IGNORECASE),
         "quarter": re.compile(r"(?<![A-Za-z0-9])((?:19|20)[0-9]{2})-?[Qq]([1-4])(?![A-Za-z0-9])")}
SEEDS = {"20260927": "nav_summ's default bootstrap seed (not a date)",
         "20260929": "nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date)"}


def tokens(line: str) -> list[tuple[str, str, str]]:
    """[(raw token, its ISO date, form)] of one line (an impossible month or day is no date)."""
    out = []
    for form, rx in FORMS.items():
        for mt in rx.finditer(line):
            g = mt.groups()
            if form in ("iso", "compact"):
                if not (1 <= int(g[1]) <= 12 and 1 <= int(g[2]) <= 31):
                    continue
                iso = f"{g[0]}-{g[1]}-{g[2]}"
            elif form == "year":
                iso = f"{g[0]}-01-01"
            else:
                iso = f"{g[0]}-{3 * int(g[1]) - 2:02d}-01"
            out.append((mt.group(0), iso, form))
    return out


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


def rulings(w: Wave, done: dict) -> dict:
    """{token: ruling} the run honours besides the seeds: the verify receipt's (a record stage run later keeps them)
    and this invocation's --seal-allow."""
    carried = ((done.get("verify") or {}).get("seal_scan") or {}).get("rulings") or {}
    return dict(carried, **getattr(w, "seal_allow", {}))


def scan(w: Wave, files: list[str], ruled: dict | None = None) -> dict:
    """{files, seal, forms, tokens_at_or_after_seal, where[{file, tokens}], seal_references{tokens, files},
    allowed[{token, ruling, tokens, files}], rulings} of ``files`` (see the module doc)."""
    ruled = dict(ruled or {})
    allow = dict(SEEDS, **ruled)
    hits, refs, allowed = [], {"tokens": 0, "files": []}, {}
    for rel in files:
        n = 0
        for line in w.path(rel).read_text(encoding="utf-8", errors="replace").splitlines():
            for raw, iso, _form in tokens(line):
                if iso < RW.SEAL_DATE:
                    continue
                if raw in allow:
                    a = allowed.setdefault(raw, {"token": raw, "ruling": allow[raw], "tokens": 0, "files": []})
                    a["tokens"] += 1
                    if rel not in a["files"]:
                        a["files"].append(rel)
                elif iso == RW.SEAL_DATE and "seal" in line.lower():
                    refs["tokens"] += 1
                    if rel not in refs["files"]:
                        refs["files"].append(rel)
                else:
                    n += 1
        if n:
            hits.append({"file": rel, "tokens": n})
    return {"files": len(files), "seal": RW.SEAL_DATE, "forms": list(FORMS),
            "tokens_at_or_after_seal": sum(h["tokens"] for h in hits), "where": hits, "seal_references": refs,
            "allowed": sorted(allowed.values(), key=lambda a: a["token"]), "rulings": ruled}


def problems(seal: dict) -> list[str]:
    if not seal["tokens_at_or_after_seal"]:
        return []
    return [f"{seal['tokens_at_or_after_seal']} date token(s) at or after the seal {RW.SEAL_DATE} in "
            f"{[h['file'] for h in seal['where']]} (inspect: a data date is a seal breach; a token that is not one is "
            "allowed by ruling: research_wave.py run ... --seal-allow TOKEN=RULING)"]
