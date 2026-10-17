"""GATE agent 3, step 5 (ruling B2-13, prereg C.11): rho of the P9 pairs' references at the gate's P9-B0, the form of
`m2g-logs/tools/b2_13_rho.py` (root M2g item 3) with the pairs named on the command line.

The series are nav_summ's paired-block series (`atx-impl/tools/nav_summ.py` at this head): each dir's primary scenario
(`scenario_of`), `load_daily` (refuses any session at or after the seal), `net_series`, `align`; the Release eval verb
`atx-research-eval` receives them as nav_summ's `--engine-stats` adapter writes them and is called as
`exe --daily c0=<child> --paired <reference> --json`. From the verb's JSON only `cells[0].paired.{sessions, rho,
one_minus_rho, degenerate}` is read; every other key is discarded unread and never printed. Printed per pair: dirs,
scenario, daily-file SHA-256 prefix, T, rho and 1 - rho (stable form), the verb's degenerate flag and the class
(degenerate iff 1 - rho < 1e-6, B2-13), and the |rho verb - rho numpy| cross-check of the verb. No mean, SD, Sharpe,
return, NAV or turnover figure is printed.

usage: python rho_pairs.py EVAL_EXE LABEL=CHILD_DIR,REFERENCE_DIR [...]   (cwd = the pool root)
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "atx-impl" / "tools"))
import nav_summ as NS  # noqa: E402

KEEP = ("sessions", "rho", "one_minus_rho", "degenerate")


def sha12(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()[:12]


def series(d: str):
    path = ROOT / d
    scen = NS.scenario_of(NS.load_summary(path))["scenario"]
    return NS.net_series(NS.load_daily(path, scen)), scen, sha12(path / f"daily_{scen}.csv")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        raise SystemExit(__doc__)
    eng = NS.EngineStats(argv[0])
    print("B2-13 rho of the P9 pairs' references (engine eval verb on nav_summ's paired series; no return statistic)")
    print(f"verb {eng.exe}  sha256 {eng.sha256}")
    for spec in argv[1:]:
        name, dirs = spec.split("=", 1)
        child, parent = dirs.split(",", 1)
        a_nets, a_scen, a_sha = series(child)
        b_nets, b_scen, b_sha = series(parent)
        argv_v = [str(eng.exe), "--daily", f"c0={eng.csv(a_nets)}", "--paired", eng.csv(b_nets), "--json"]
        done = subprocess.run(argv_v, capture_output=True, text=True, check=False)
        if done.returncode != 0:
            raise SystemExit(f"rho_pairs: verb exit {done.returncode}: {done.stderr.strip()[:300]}")
        paired = json.loads(done.stdout)["cells"][0]["paired"]
        got = {k: paired.get(k) for k in KEEP}
        del paired, done
        a, b = NS.align(a_nets, b_nets)
        rho_np = float(np.corrcoef(a, b)[0, 1])
        omr = got["one_minus_rho"]
        flag = omr < 1e-6
        print(f"\n{name}")
        print(f"  child     {child}  scenario {a_scen}  daily sha256 {a_sha}")
        print(f"  reference {parent}  scenario {b_scen}  daily sha256 {b_sha}")
        print(f"  T (common sessions) verb {got['sessions']} / aligned {a.size}")
        print(f"  rho (verb)              {got['rho']!r}")
        print(f"  1 - rho (verb, stable)  {omr!r}")
        print(f"  verb degenerate flag    {got['degenerate']}")
        print(f"  '1 - rho < 1e-6' flag   {flag}")
        print(f"  cross-check |rho verb - rho numpy| {abs(got['rho'] - rho_np):.3e}")
        print("  class: " + ("DEGENERATE: the B2-13 / PRE-12 / PRE-33 fallback of the cell applies" if flag else
                             "paired test defined (1 - rho >= 1e-6)"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
