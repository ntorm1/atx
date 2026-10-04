"""M2f root item B2-R1 (d): rho of the X-5 / Y-F0 pairs on the pinned references, measured and logged BEFORE the
TRAIN identity run of the engine eval verb.

The series are exactly nav_summ's paired-block series (`atx-impl/tools/nav_summ.py` at the merged head): each dir's
primary scenario (`scenario_of`), `load_daily` (refuses any session at or after the seal), `net_series` (return rows,
first row dropped), `align` (common sessions). Printed per pair: the dirs, scenario, T (common sessions), rho as
nav_summ's Python computes it (numpy corrcoef, repr), 1 - rho from it, and the engine's stable form
1 - rho = sum((za - zb)^2) / (2 (T - 1)) (ruling B2-R1 (a); za, zb standardized with ddof 1), and the B2-R1 (c) class
(degenerate iff 1 - rho < 1e-6). No mean, SD, Sharpe, return, NAV or turnover figure is computed or printed.

usage: python b2_rho.py   (cwd = the pool-2 root)
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

import numpy as np

ROOT = Path("C:/atx-wt/pool-2")
sys.path.insert(0, str(ROOT / "atx-impl" / "tools"))
import nav_summ as NS  # noqa: E402

X3 = "build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v8x3b"   # X-5 locked reference_cell e342db4f
X5_LEDGERED = "build-equity/mega-nav-v8x-theme-erc-L1.1720"                            # Y-F0 inputs.reference_cell a03937cf
X5_PINNED = "build-equity/p9-c1-s2-x5-nav"                                             # X-5 Debug reference since C1 (DEC-20)
YF0_LEDGERED = "build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb"                     # Y-F0 spec nav.output
YF0_PINNED = "build-equity/p9-b0-yf0-nav"                                              # P9-B0 re-based Y-F0

PAIRS = [
    ("X-5 (pinned)", X5_PINNED, X3),
    ("Y-F0 (pinned)", YF0_PINNED, X5_PINNED),
    ("X-5 (as ledgered)", X5_LEDGERED, X3),
    ("Y-F0 (as ledgered)", YF0_LEDGERED, X5_LEDGERED),
]


def sha12(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()[:12]


def series(d: str) -> tuple[dict, str, str]:
    path = ROOT / d
    scen = NS.scenario_of(NS.load_summary(path))["scenario"]
    return NS.net_series(NS.load_daily(path, scen)), scen, sha12(path / f"daily_{scen}.csv")


def main() -> int:
    print("B2-R1 (d) rho of the X-5 / Y-F0 pairs (nav_summ's paired series; no return statistic)")
    for name, child, parent in PAIRS:
        a_nets, a_scen, a_sha = series(child)
        b_nets, b_scen, b_sha = series(parent)
        a, b = NS.align(a_nets, b_nets)
        t = a.size
        rho = float(np.corrcoef(a, b)[0, 1])
        za = (a - a.mean()) / a.std(ddof=1)
        zb = (b - b.mean()) / b.std(ddof=1)
        omr_stable = float(((za - zb) ** 2).sum() / (2.0 * (t - 1)))
        cls = "DEGENERATE (1 - rho < 1e-6: no paired test)" if omr_stable < 1e-6 else "paired test defined (1 - rho >= 1e-6)"
        print(f"\n{name}")
        print(f"  child  {child}  scenario {a_scen}  daily sha256 {a_sha}  return rows {len(a_nets)}")
        print(f"  parent {parent}  scenario {b_scen}  daily sha256 {b_sha}  return rows {len(b_nets)}")
        print(f"  T (common sessions) {t}")
        print(f"  rho (numpy corrcoef) {rho!r}")
        print(f"  1 - rho (from numpy rho) {1.0 - rho!r}")
        print(f"  1 - rho (stable form)    {omr_stable!r}")
        print(f"  B2-R1 tolerance for paired floats 1e-12 / (1 - rho) = {1e-12 / omr_stable:.3e}")
        print(f"  class: {cls}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
