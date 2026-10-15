"""M6b (RESUME 11) SQL4 identity runs: re-run a reference receipt's argv through run_bounded_research.py with named
tokens substituted (copy of m2d-logs/tools/rerun.py, extended).

usage: rerun.py RECEIPT NEW_NAME SECONDS MAX_RSS [--u] [--cache DIR] [--pair-cache DIR] [--append TOKEN]... [--dry]
RECEIPT: a receipt.json (its `command` is the argv of record). Substitutes exactly: the --output value (->
build-equity/NEW_NAME); with --u, `--composition-weights F --composition-weights-sha256 S` -> `--no-composition`
(the u form of a w argv); with --cache, the --candidate-cache value; with --pair-cache, adds `--pair-cache DIR` (the
reference has none). Appends each --append token at the end. Asserts each substituted token existed exactly once;
everything else verbatim. The runner's own dir is build-equity/NEW_NAME-run. Debug PATH order as m2d's tool.
"""
import json
import os
import subprocess
import sys

REPO = r"C:\atx-wt\pool-2"
PY = r"C:\Program Files\Python312\python.exe"
receipt, name, secs, rss = sys.argv[1:5]
opts = sys.argv[5:]


def opt(flag):
    return opts[opts.index(flag) + 1] if flag in opts else None


append = [opts[i + 1] for i, t in enumerate(opts) if t == "--append"]
rec = json.load(open(receipt, encoding="utf-8"))
cmd = list(rec["command"])


def sub(flag, value):
    idx = [i for i, t in enumerate(cmd) if t == flag]
    assert len(idx) == 1, (flag, idx)
    cmd[idx[0] + 1] = value


sub("--output", f"build-equity/{name}")
if "--u" in opts:
    i = cmd.index("--composition-weights")
    j = cmd.index("--composition-weights-sha256")
    assert j == i + 2 and cmd.count("--composition-weights") == 1, (i, j)
    cmd[i:j + 2] = ["--no-composition"]
if opt("--cache"):
    sub("--candidate-cache", opt("--cache"))
if opt("--pair-cache"):
    assert "--pair-cache" not in cmd
    cmd += ["--pair-cache", opt("--pair-cache")]
cmd += append
print("argv:", " ".join(cmd[1:]))
env = dict(os.environ)
vc = r"C:\atx-cache\vcpkg_installed\x64-windows"
env["PATH"] = vc + r"\debug\bin;" + vc + r"\bin;" + env.get("PATH", "")
env["PYTHONDONTWRITEBYTECODE"] = "1"
runner = [PY, "scripts/run_bounded_research.py", "--output", f"build-equity/{name}-run", "--seconds", secs,
          "--max-rss-mib", rss, "--min-free-mib", str(rec["limits"]["min_free_mib"]), "--build-type", "Debug",
          "--admission-wait-seconds", "3600", "--"] + cmd
if "--dry" in opts:
    sys.exit(0)
sys.exit(subprocess.run(runner, cwd=REPO, env=env).returncode)
