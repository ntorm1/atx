"""Run the whole alpha panel build in order, each stage as its own memory-guarded process.

Usage (from C:/atx/atx-db, under an orchestrator guard of 0.2 GiB with --allow-nested-guards)::

    python ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 -- \
        .venv/Scripts/python.exe -m atx_db.alpha_panel.build [--from STAGE] [--only STAGE,...]

Stages (see docs/ALPHA_PANEL.md and ``STAGES``, in dependency order): prices, identity tiers, fundamentals,
short interest and volume, panel core, SEC filings / earnings calendar / Form 4, FTD, Reg SHO, 13F, identity
table, security master, corporate actions, delisting, panel assembly, borrow proxy, metrics, consumer exports.
Each stage is resumable on its own.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

from . import common as C

GUARD = C.PACKAGE_ROOT.parent / ".superpowers" / "sdd" / "tier1-parity" / "run_memory_guarded.py"
PY = sys.executable

M = "atx_db.alpha_panel."
# the mega-alpha data request's acceptance role (read only)
LO1_ROLE = os.environ.get("ATX_LO1_ROLE", "C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-lo1")
STAGES: list[tuple[str, float, list[str]]] = [
    ("prices", 1.0, ["-m", M + "prices"]),
    ("identity", 0.8, ["-m", M + "identity_links", "build"]),
    ("backfill", 1.0, ["-m", M + "identity_backfill"]),
    ("fundamentals_prepare", 0.8, ["-m", M + "fundamentals", "prepare"]),
    ("fundamentals_batches", 0.8, ["-m", M + "fundamentals", "batches"]),
    ("fundamentals_finalize", 0.8, ["-m", M + "fundamentals", "finalize"]),
    ("fundamentals_validate", 0.8, ["-m", M + "fundamentals", "validate"]),
    ("short_interest_fetch", 0.6, ["-m", M + "short_interest", "fetch"]),
    ("short_interest", 0.8, ["-m", M + "short_interest", "build"]),
    ("short_volume_download", 0.6, ["-m", M + "short_volume", "download"]),
    ("short_volume", 0.8, ["-m", M + "short_volume", "build"]),
    ("names", 1.0, ["-m", M + "identity_names"]),
    ("combine", 1.0, ["-m", M + "identity_backfill"]),
    # panel core and membership (line-sessions, ADV rank, member) feed identity_table and delisting
    ("panel_core", 1.0, ["-m", M + "panel", "--stages", "core,membership"]),
    # SEC filings, earnings calendar, Form 4, FTD, Reg SHO, short-volume splits, 13F (request D1-D3, D11, D12)
    ("sec_filings", 1.0, ["-m", M + "sec_filings", "--phase", "all"]),
    ("earnings_calendar", 0.8, ["-m", M + "earnings_calendar"]),
    ("insider", 0.8, ["-m", M + "insider"]),
    ("ftd_download", 0.6, ["-m", M + "ftd", "download"]),
    ("ftd", 0.8, ["-m", M + "ftd", "build"]),
    *[(f"regsho_download_{m}", 0.3, ["-m", M + "regsho", "download", "--market", m])
      for m in ("nasdaq", "nyse_combined", "cboe_bzx", "finra_otc")],
    ("regsho", 0.8, ["-m", M + "regsho", "build"]),
    ("short_volume_ext", 0.8, ["-m", M + "short_volume_ext"]),
    ("thirteenf_fetch", 0.8, ["-m", M + "thirteenf", "fetch"]),
    ("thirteenf", 0.8, ["-m", M + "thirteenf", "build"]),
    ("identity_table", 0.8, ["-m", M + "identity_table"]),
    ("security_master", 0.8, ["-m", M + "security_master"]),
    ("corporate_actions", 0.8, ["-m", M + "corporate_actions"]),
    ("delisting", 0.8, ["-m", M + "delisting"]),
    ("panel", 1.0, ["-m", M + "panel", "--stages", "assemble"]),
    ("borrow_proxy", 0.8, ["-m", M + "borrow_proxy"]),
    ("metrics", 1.0, ["-m", M + "metrics"]),
    # consumer exports: fundamental events contract, then the request's acceptance role (fields on its axes)
    ("fund_export", 0.8, ["-m", M + "fund_export", "build"]),
    ("fund_export_verify", 0.8, ["-m", M + "fund_export", "verify"]),
    ("export_lo1", 1.0, ["-m", M + "export_impl", "align", "--role", LO1_ROLE, "--out",
                         str(C.build_root() / "export" / "lo1-fields-v2")]),
    # optional research extras, not in the default chain's consumer contract (see docs/ALPHA_PANEL.md)
    ("characteristics", 1.0, ["-m", M + "characteristics"]),
    ("coverage", 1.0, ["-m", M + "coverage"]),
]


def run_stage(name: str, cap: float, args: list[str]) -> int:
    env = dict(os.environ, OPENBLAS_NUM_THREADS="1", PYTHONPATH=str(C.PACKAGE_ROOT / "src"))
    cmd = [PY, str(GUARD), "--job-gb", str(cap), "--wait-minutes", "60", "--quiet"]
    parent = os.environ.get("ATX_GUARD_JOB")
    if parent:  # running under an orchestrator guard: stop with it (as atx_db.research.workers)
        cmd += ["--parent-job", parent]
    cmd += ["--", PY, *args]
    flags = 0
    if os.name == "nt":
        flags = subprocess.CREATE_NO_WINDOW | (subprocess.CREATE_BREAKAWAY_FROM_JOB if parent else 0)
    log = C.build_root() / "_logs" / f"{name}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    for attempt in range(3):
        with log.open("a", encoding="utf-8") as fh:
            fh.write(f"\n=== {name} attempt {attempt + 1} {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            fh.flush()
            rc = subprocess.call(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=str(C.PACKAGE_ROOT), env=env,
                                 creationflags=flags)
        if rc != 137:  # 137 = low-memory stop by the guard: resumable, retry
            break
    print(f"{name}: exit {rc} in {time.time() - t0:.0f}s (log {log})", flush=True)
    return rc


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="start", default=None)
    ap.add_argument("--only", default=None)
    args = ap.parse_args(argv)
    names = [s for s, _, _ in STAGES]
    todo = STAGES
    if args.only:
        want = set(args.only.split(","))
        todo = [s for s in STAGES if s[0] in want]
    elif args.start:
        todo = STAGES[names.index(args.start):]
    for name, cap, a in todo:
        if run_stage(name, cap, a) != 0:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
