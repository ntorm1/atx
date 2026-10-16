"""Cells root (hand cells: no seal scan in research_cycle.py run; SEAL-REF ground): the hidden-data scan of a hand
cell's logs in the form of scripts/wave_seal.py (its FORMS, SEEDS, sealed() and the seal-reference rule), each hit
classified by the sources of the sprint dir's tools/sealsrc.py. Prints file, line, token, form and source only: no
log content.

  A   = the file name docs/plans/2026-10-02-x5-equity-curve.png (sealsrc's A: the untracked owner plot)
  A2  = another untracked owner plot under docs/plans/ named by `git status --short` (?? *.png), whole name matched
  B   = a receipt "started_utc" wall-clock stamp ("started_utc": "<date>T...)
  OTHER = anything else: a STOP before any read (the plan's rule)

usage (cwd = the pool root): hand_seal.py [--base RUN_BASE ...] [--dir DIR ...] [--file FILE ...]
  --base B: every run dir B-run* and its attempt-* sub-dirs; --dir D: D itself; their stdout.log / stderr.log
  --file F: a console log scanned whole
exit 0 when every sealed token is a seed, a seal reference, A, A2 or B; 1 when any is OTHER.
"""
from __future__ import annotations

import datetime as dt
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "scripts"))
import wave_seal as WSL  # noqa: E402

PNG = "docs/plans/2026-10-02-x5-equity-curve.png"
LOG_NAMES = WSL.LOG_NAMES


def untracked_pngs() -> list[str]:
    out = subprocess.run(["git", "status", "--short", "--", "docs/plans"], cwd=ROOT, capture_output=True,
                         text=True).stdout.splitlines()
    return [ln[3:].strip() for ln in out if ln.startswith("?? ") and ln.strip().endswith(".png")]


def files_of(argv: list[str]) -> list[str]:
    bases, dirs, files, key = [], [], [], None
    for a in argv:
        if a in ("--base", "--dir", "--file"):
            key = a
            continue
        {"--base": bases, "--dir": dirs, "--file": files}[key].append(a)
    for b in bases:
        p = ROOT / b
        for d in sorted(p.parent.glob(p.name + "-run*")):
            if d.is_dir():
                dirs.append(d.relative_to(ROOT).as_posix())
                dirs += [x.relative_to(ROOT).as_posix() for x in sorted(d.glob("attempt-*")) if x.is_dir()]
    out = [f"{d}/{n}" for d in dirs for n in LOG_NAMES if (ROOT / d / n).is_file()] + files
    return sorted(dict.fromkeys(out))


def main(argv: list[str]) -> int:
    pngs = untracked_pngs()
    files = files_of(argv)
    by, other, refs, seeds = Counter(), 0, 0, 0
    for rel in files:
        for ln, line in enumerate((ROOT / rel).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for form, rx in WSL.FORMS.items():
                for mt in rx.finditer(line):
                    raw, g = mt.group(0), mt.groups()
                    try:
                        if form in ("iso", "compact"):
                            iso = dt.date(int(g[0]), int(g[1]), int(g[2])).isoformat()
                        else:
                            iso = WSL.RW.period_begin(int(g[0]), int(g[1]) if form == "quarter" else None).isoformat()
                    except ValueError:
                        continue
                    if not WSL.sealed(iso):
                        continue
                    if raw in WSL.SEEDS:
                        seeds += 1
                        continue
                    if iso == WSL.RW.SEAL_DATE and "seal" in line.lower():
                        refs += 1
                        continue
                    s, e = mt.start(), mt.end()
                    if form == "iso" and line[max(0, s - 11):e + 20] == PNG:
                        src = "A"
                    elif any(n in line and line.find(n) <= s and e <= line.find(n) + len(n) for n in pngs):
                        src = "A2"
                    elif form == "iso" and line[max(0, s - 16):s] == '"started_utc": "' and line[e:e + 1] == "T":
                        src = "B"
                    else:
                        src = "OTHER"
                        other += 1
                    by[(raw, src)] += 1
                    print(f"  {rel}:{ln} token={raw} form={form} source={src}")
    print(f"files scanned: {len(files)}; seal {WSL.RW.SEAL_DATE}; forms {list(WSL.FORMS)}; seeds {seeds}; "
          f"seal references {refs}; untracked PNGs named {len(pngs)}")
    print("summary:", ", ".join(f"{t} {s} x{n}" for (t, s), n in sorted(by.items())) or "no hits")
    print("OTHER hits:", other)
    return 1 if other else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
