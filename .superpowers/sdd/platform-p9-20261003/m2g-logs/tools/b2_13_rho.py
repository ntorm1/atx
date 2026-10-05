"""Ruling B2-13 (root M2g item 3): rho of the leverage-only pairs X-10 vs Y-F0 and Y-1 vs X-10, from the engine
eval verb `atx-research-eval` (Release, the statistics of record; ruling B2-R3), on the series nav_summ hands it.

The series are exactly nav_summ's paired-block series (`atx-impl/tools/nav_summ.py` at this head; as M2f's
`m2f-logs/tools/b2_rho.py`): each dir's primary scenario (`scenario_of`), `load_daily` (refuses any session at or after
the seal), `net_series` (return rows, first row dropped), `align` (common sessions); the verb receives them as
nav_summ's `--engine-stats` adapter writes them (`EngineStats.csv`: `session_ns,net_return`, Python repr) and is called
as `exe --daily c0=<child> --paired <reference> --json`. From the verb's JSON only `cells[0].paired.{sessions, rho,
one_minus_rho, degenerate}` is read; every other key (Sharpe, dSR, SE, p, CI, moments) is discarded unread and never
printed. Printed per pair: dirs, scenario, daily-file SHA-256 prefix, T, the verb's rho and 1 - rho (stable form,
B2-R1 (a)), the verb's degenerate flag and the B2-R1 (c) / B2-13 class (degenerate iff 1 - rho < 1e-6), and, as a
cross-check of the verb, numpy's corrcoef rho and the stable form from the same aligned arrays (M2f's two figures).
No mean, SD, Sharpe, return, NAV or turnover figure is computed or printed.

usage: python b2_13_rho.py EVAL_EXE   (cwd = the pool-2 root)
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path("C:/atx-wt/pool-2")
sys.path.insert(0, str(ROOT / "atx-impl" / "tools"))
import nav_summ as NS  # noqa: E402

YF0_PINNED = "build-equity/p9-b0-yf0-nav"                              # P9-B0 re-based Y-F0 (integration-log M1d)
X10_PINNED = "build-equity/p9-b0-x10-nav"                              # P9-B0 re-based X-10
Y1_PINNED = "build-equity/p9-b0-y1-nav"                                # P9-B0 re-based Y-1
YF0_LEDGERED = "build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb"     # Y-F0 as ledgered (P9-B0 reference)
X10_LEDGERED = "build-equity/mega-nav-v8x-theme-erc-L2.0-v8ysb"        # X-10 as ledgered
Y1_LEDGERED = "build-equity/mega-nav-v8y-vol-target-L2.0"              # Y-1 as ledgered

PAIRS = [
    ("X-10 vs Y-F0 (pinned, P9-B0)", X10_PINNED, YF0_PINNED),
    ("Y-1 vs X-10 (pinned, P9-B0)", Y1_PINNED, X10_PINNED),
    ("X-10 vs Y-F0 (as ledgered)", X10_LEDGERED, YF0_LEDGERED),
    ("Y-1 vs X-10 (as ledgered)", Y1_LEDGERED, X10_LEDGERED),
]
KEEP = ("sessions", "rho", "one_minus_rho", "degenerate")


def sha12(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()[:12]


def series(d: str) -> tuple[dict, str, str]:
    path = ROOT / d
    scen = NS.scenario_of(NS.load_summary(path))["scenario"]
    return NS.net_series(NS.load_daily(path, scen)), scen, sha12(path / f"daily_{scen}.csv")


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        raise SystemExit(__doc__)
    eng = NS.EngineStats(argv[0])
    print("B2-13 rho of the leverage-only pairs (engine eval verb on nav_summ's paired series; no return statistic)")
    print(f"verb {eng.exe}  sha256 {eng.sha256}")
    for name, child, parent in PAIRS:
        a_nets, a_scen, a_sha = series(child)
        b_nets, b_scen, b_sha = series(parent)
        argv_v = [str(eng.exe), "--daily", f"c0={eng.csv(a_nets)}", "--paired", eng.csv(b_nets), "--json"]
        done = subprocess.run(argv_v, capture_output=True, text=True, check=False)
        if done.returncode != 0:
            raise SystemExit(f"b2_13_rho: verb exit {done.returncode}: {done.stderr.strip()[:300]}")
        paired = json.loads(done.stdout)["cells"][0]["paired"]
        got = {k: paired.get(k) for k in KEEP}
        del paired, done                                   # nothing else of the document is kept
        a, b = NS.align(a_nets, b_nets)
        t = a.size
        rho_np = float(np.corrcoef(a, b)[0, 1])
        za = (a - a.mean()) / a.std(ddof=1)
        zb = (b - b.mean()) / b.std(ddof=1)
        omr_np = float(((za - zb) ** 2).sum() / (2.0 * (t - 1)))
        omr = got["one_minus_rho"]
        cls = ("DEGENERATE (1 - rho < 1e-6): paired SE / t / p print 'na' (B2-6); the cell is decided on its "
               "non-paired acceptance lines only (B2-13)") if omr < 1e-6 else "paired test defined (1 - rho >= 1e-6)"
        print(f"\n{name}")
        print(f"  child     {child}  scenario {a_scen}  daily sha256 {a_sha}")
        print(f"  reference {parent}  scenario {b_scen}  daily sha256 {b_sha}")
        print(f"  T (common sessions) verb {got['sessions']} / aligned {t}")
        print(f"  rho (verb)              {got['rho']!r}")
        print(f"  1 - rho (verb, stable)  {omr!r}")
        print(f"  verb degenerate flag    {got['degenerate']}")
        print(f"  cross-check: rho (numpy corrcoef) {rho_np!r}; 1 - rho (stable form, numpy) {omr_np!r}; "
              f"|rho verb - numpy| {abs(got['rho'] - rho_np):.3e}")
        print(f"  B2-R1 tolerance for paired floats 1e-12 / (1 - rho) = {1e-12 / omr:.3e}")
        print(f"  class: {cls}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
