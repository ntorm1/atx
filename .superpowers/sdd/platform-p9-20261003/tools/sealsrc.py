"""Per-hit source check for PM ruling SEAL-ALLOW (P9 progress.md): every sealed date token in the logs the wave's seal
scan reads (wave_seal.wave_logs over the done receipts), classified by source. Prints file, line, token, form and
source only -- no log content.

  A  = the file name docs/plans/2026-10-02-x5-equity-curve.png (the untracked owner plot in the runner's dirty list)
  B  = a receipt "started_utc" wall-clock stamp ("started_utc": "<date>T...)
  OTHER = anything else: a stop under the ruling

usage: python sealsrc.py [--root C:/atx-wt/pool-2] [--manifest scripts/specs/v8/waves/y-s.json]
exit 0 when every hit is A or B, 1 when any hit is OTHER.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from collections import Counter
from pathlib import Path
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--root", default="C:/atx-wt/pool-2")
ap.add_argument("--manifest", default="scripts/specs/v8/waves/y-s.json")
a = ap.parse_args()
root = Path(a.root)
sys.path.insert(0, str(root / "scripts"))
from wave_context import Wave  # noqa: E402
import wave_seal as WSL  # noqa: E402
import wave_stages  # noqa: E402

PNG = "docs/plans/2026-10-02-x5-equity-curve.png"
w = Wave(Path(a.manifest), root)
rdir = w.path(w.wave_path("receipts"))
done = {}
for i, st in enumerate(wave_stages.STAGES):
    p = rdir / f"{i + 1:02d}-{st.name}.json"
    if not p.is_file():
        break
    done[st.name] = json.loads(p.read_text(encoding="utf-8"))["outputs"]
files = WSL.wave_logs(w, done)
print(f"done stages: {list(done)}; files scanned: {len(files)}")
by = Counter()
other = 0
for rel in files:
    for ln, line in enumerate(w.path(rel).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        for form, rx in WSL.FORMS.items():
            for mt in rx.finditer(line):
                raw, g = mt.group(0), mt.groups()
                try:                                   # wave_seal.tokens' own date rule
                    if form in ("iso", "compact"):
                        iso = dt.date(int(g[0]), int(g[1]), int(g[2])).isoformat()
                    else:
                        iso = WSL.RW.period_begin(int(g[0]), int(g[1]) if form == "quarter" else None).isoformat()
                except ValueError:
                    continue
                if not WSL.sealed(iso) or raw in WSL.SEEDS:
                    continue
                if iso == WSL.RW.SEAL_DATE and "seal" in line.lower():
                    continue
                s, e = mt.start(), mt.end()
                if form == "iso" and line[max(0, s - 11):e + 20] == PNG:
                    src = "A"
                elif form == "iso" and line[max(0, s - 16):s] == '"started_utc": "' and line[e:e + 1] == "T":
                    src = "B"
                else:
                    src = "OTHER"
                    other += 1
                by[(raw, src)] += 1
                print(f"  {rel}:{ln} token={raw} form={form} source={src}")
print("summary:", ", ".join(f"{t} {s} x{n}" for (t, s), n in sorted(by.items())) or "no hits")
print("OTHER hits:", other)
sys.exit(1 if other else 0)
