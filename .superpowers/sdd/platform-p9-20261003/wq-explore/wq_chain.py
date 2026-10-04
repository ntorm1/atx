"""WQ-explore: the primary book's chain (u -> fit -> w -> NAV) on another library, Release exes run directly
(no cycle / wave command, nothing recorded in the trial ledger). Argv = the P9-B0 argv of
build-equity/p9-b0-yf0-{u,fit,w,nav}-run/start.json with the library, the output dirs, the candidate cache and the
fitter work dir replaced (and, for a WQ library, no --recipe: the prior metadata is in the library rows).

usage: wq_chain.py NAME LIBRARY_JSON [--recipe PATH] [--skip-nav] [--aim-leverage L] [--nav-name nav]
       wq_chain.py --nav-only NAME COMBINED_JSON [--aim-leverage L] [--nav-name nav]
Everything is written under build-equity/wq-explore/NAME/.
"""
from __future__ import annotations
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
X = "build-equity/wq-explore"
IC = str(ROOT / "build-equity-rel/bin/atx-equity-strategy-ic.exe")
NAV = str(ROOT / "build-equity-rel/bin/atx-equity-strategy-targets.exe")
PY = sys.executable
MEM_W = "4096"  # admission budget only (the primary used 3072; WQ strings need up to 12 VM slots); measured peak ~1.5 GiB
TRAIN = ["--train", "build-equity/train-2020-2023-lo3/manifest.json", "--train-sha256",
         "e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4"]
FIELDS = ["--train-fields", "build-equity/train-2020-2023-lo3-fields-v15", "--train-fields-sha256",
          "26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09"]


def sha(path):
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()


def run(cmd, log):
    t = time.time()
    with open(ROOT / log, "w") as fh:
        fh.write(json.dumps(cmd) + "\n")
        fh.flush()
        rc = subprocess.run(cmd, cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT).returncode
    print(f"  {log}: exit {rc} in {time.time() - t:.0f}s", flush=True)
    if rc != 0:
        print((ROOT / log).read_text()[-1500:])
        raise SystemExit(rc)


def nav(name, combined, lev, nav_name):
    out = f"{X}/{name}/{nav_name}"
    run([NAV, "nav", "--combined", combined, "--combined-sha256", sha(combined),
         "--role", TRAIN[1], "--role-sha256", TRAIN[3],
         "--fields", FIELDS[1] + "/manifest.json", "--fields-sha256", FIELDS[3], "--output", out,
         "--rule", "aim-partial-v5", "--cadence", "1", "--trade-fraction", ".05", "--dust-multiple", ".1",
         "--aim-leverage", lev, "--daily-turnover-mean-max", ".20", "--daily-turnover-p95-max", ".30",
         "--neutralize", "price-risk-v1", "--max-bytes", "1073741824", "--order-basis", "delta", "--exit-rate", ".05",
         "--locate-in-aim", "--liquidity-cache", "--warm-start-sessions", "60", "--capacity-curve",
         "--label-role", "build-equity/train-2020-2023-lo3-dlret/manifest.json", "--label-role-sha256",
         "95e16cfe3ad0e0c9e7cf94acb6df4004e6f286ae59dff7c0e725ea33aace5069"], f"{X}/{name}/{nav_name}.log")


def main():
    a = sys.argv[1:]
    lev = a[a.index("--aim-leverage") + 1] if "--aim-leverage" in a else "1.1828"
    nav_name = a[a.index("--nav-name") + 1] if "--nav-name" in a else "nav"
    if a[0] == "--nav-only":
        (ROOT / X / a[1]).mkdir(parents=True, exist_ok=True)
        nav(a[1], a[2], lev, nav_name)
        return
    name, lib = a[0], a[1]
    d = f"{X}/{name}"
    (ROOT / d).mkdir(parents=True, exist_ok=True)
    L = ["--library", lib, "--library-sha256", sha(lib)]
    cache = ["--candidate-cache", f"{X}/cache"]
    if not (ROOT / d / "u").exists():
        run([IC, *L, *TRAIN, *FIELDS, "--output", f"{d}/u", "--max-memory-mib", "2560", "--min-names", "1000",
             "--workers", "4", "--no-composition", *cache], f"{d}/u.log")
    if not (ROOT / d / "fit").exists():
        recipe = []
        if "--recipe" in a:
            r = a[a.index("--recipe") + 1]
            recipe = ["--recipe", r, "--recipe-sha256", sha(r)]
        cmd = [PY, "atx-impl/tools/fit_composition_weights.py", *L, *TRAIN,
               "--orientations", f"{d}/u/orientations.json", "--orientations-sha256", sha(f"{d}/u/orientations.json"),
               "--runner-summary", f"{d}/u/summary.json", "--runner-summary-sha256", sha(f"{d}/u/summary.json"),
               "--orientation", "prior", "--screen", "v4-prior-v1", "--composition", "ew-theme-std-v1",
               "--theme-erc", "theme-erc-v1", *recipe, "--work-dir", f"{X}/fit-work", "--max-seconds", "3000",
               "--output", f"{d}/fit"]
        run(cmd, f"{d}/fit.log")
    if not (ROOT / d / "w").exists():
        cw = f"{d}/fit/composition_weights.json"
        run([IC, *L, *TRAIN, *FIELDS, "--output", f"{d}/w", "--max-memory-mib", MEM_W, "--min-names", "1000",
             "--workers", "4", "--save-combined", *cache, "--composition-weights", cw,
             "--composition-weights-sha256", sha(cw)], f"{d}/w.log")
    if "--skip-nav" not in a and not (ROOT / d / nav_name).exists():
        nav(name, f"{d}/w/train_combined.json", lev, nav_name)


if __name__ == "__main__":
    main()
