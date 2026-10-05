"""Re-run a reference receipt's argv through run_bounded_research.py with named tokens substituted.

usage: rerun.py REF_RUN_DIR NEW_NAME BUILD_TYPE SECONDS MAX_RSS [--exe PATH] [--cache DIR] [--dry]
Substitutes exactly: --output value (-> build-equity/NEW_NAME), optionally the executable and the
--candidate-cache value. Asserts each substituted token existed exactly once; everything else verbatim.
"""
import json, os, subprocess, sys
REPO = r"C:\atx-wt\pool-2"
PY = r"C:\Program Files\Python312\python.exe"
ref, name, btype, secs, rss = sys.argv[1:6]
opts = sys.argv[6:]
exe = opts[opts.index("--exe") + 1] if "--exe" in opts else None
cache = opts[opts.index("--cache") + 1] if "--cache" in opts else None
rec = json.load(open(os.path.join(REPO, "build-equity", ref, "receipt.json"), encoding="utf-8"))
cmd = list(rec["command"])
def sub(flag, value):
    idx = [i for i, t in enumerate(cmd) if t == flag]
    assert len(idx) == 1, (flag, idx)
    cmd[idx[0] + 1] = value
sub("--output", f"build-equity/{name}")
if cache: sub("--candidate-cache", cache)
if exe: cmd[0] = exe
changed = [(i, a, b) for i, (a, b) in enumerate(zip(rec["command"], cmd)) if a != b]
print("substituted:", changed)
env = dict(os.environ)
vc = r"C:\atx-cache\vcpkg_installed\x64-windows"
first = vc + (r"\debug\bin" if btype == "Debug" else r"\bin")
second = vc + (r"\bin" if btype == "Debug" else r"\debug\bin")
env["PATH"] = first + ";" + second + ";" + env.get("PATH", "")
env["PYTHONDONTWRITEBYTECODE"] = "1"
runner = [PY, "scripts/run_bounded_research.py", "--output", f"build-equity/{name}-run", "--seconds", secs,
          "--max-rss-mib", rss, "--min-free-mib", str(rec["limits"]["min_free_mib"]), "--build-type", btype,
          "--admission-wait-seconds", "3600", "--"] + cmd
print(" ".join(runner))
if "--dry" in opts: sys.exit(0)
sys.exit(subprocess.run(runner, cwd=REPO, env=env).returncode)
