"""GATE (RESUME 12) NAV runs, copied from m2j-logs/tools/nav_run.py (logs dir gate-logs): flag-absent identity of the
parents (step 3: X-5, Y-F0 through argv and through --spec; AL-CLOCK's procedure and ALCLOCK-1's opt-in), all through
the bounded runner (RUN-1 / RUN-2: --seconds 600 --max-rss-mib 2048).

usage:
  python nav_run.py argv CELL BUILD OUT [EXTRA ...]   the cell's reference argv with only --output changed to
                                                      build-equity/OUT, EXTRA tokens appended (e.g. --info-clock-print)
  python nav_run.py spec CELL BUILD OUT               nav --spec gate-logs/CELL.navspec.json --output build-equity/OUT
  python nav_run.py printspec CELL BUILD              <exe> nav <reference argv verbatim> --print-spec, stdout bytes
                                                      written to gate-logs/CELL.navspec.json (runs nothing, reads no
                                                      data); exit code and SHA-256 printed
CELL: x5 (reference build-equity/p9-c1-s2-x5-nav, receipt -run; M2J-2) | yf0 (build-equity/p9-b0-yf0-nav, receipt -run).
BUILD: rel (build-equity-rel/bin, --build-type Release) | dbg (build-equity/bin, --build-type Debug).
One summary line per run is appended to gate-logs/nav-runs.txt (outcome, exit, wall, peak, exe SHA-256, source, build
type, argv token diff against the reference, and whether the receipt / start record carries a `store` block: SQL3-2
wants none); the receipt is copied to gate-logs/receipt-OUT.json. No output file is opened here.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[5]
SD = REPO / ".superpowers" / "sdd" / "platform-p9-20261003"
LOGS = SD / "gate-logs"
PY = sys.executable
REFS = {"x5": "build-equity/p9-c1-s2-x5-nav-run/receipt.json", "yf0": "build-equity/p9-b0-yf0-nav-run/receipt.json"}
BUILDS = {"rel": ("build-equity-rel/bin/atx-equity-strategy-targets.exe", "Release"),
          "dbg": ("build-equity/bin/atx-equity-strategy-targets.exe", "Debug")}


def sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def ref_argv(cell: str) -> list[str]:
    return json.loads((REPO / REFS[cell]).read_text(encoding="utf-8"))["command"][1:]


def bounded(out: str, exe: Path, bt: str, nav_args: list[str], ref: list[str] | None) -> int:
    run_dir = REPO / "build-equity" / f"{out}-run"
    exe_sha = sha_file(exe)
    cmd = [PY, "scripts/run_bounded_research.py", "--output", str(run_dir.relative_to(REPO).as_posix()),
           "--seconds", "600", "--max-rss-mib", "2048", "--build-type", bt, "--admission-wait-seconds", "1800",
           "--", str(exe), *nav_args]
    log = LOGS / f"runner-{out}.log"
    with log.open("wb") as fh:
        rc = subprocess.run(cmd, cwd=REPO, stdout=fh, stderr=subprocess.STDOUT,
                            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}).returncode
    rec = run_dir / "receipt.json"
    line = f"{out} runner-exit {rc} exe-before {exe_sha[:16]}"
    if rec.is_file():
        shutil.copyfile(rec, LOGS / f"receipt-{out}.json")
        r = json.loads(rec.read_text(encoding="utf-8"))
        start = run_dir / "start.json"
        s = json.loads(start.read_text(encoding="utf-8")) if start.is_file() else {}
        line += (f" :: outcome {r.get('outcome')} exit {r.get('exit_code')} wall_s {r.get('wall_seconds')} "
                 f"peak_MiB {round((r.get('sampled_peak_tree_rss_bytes') or 0) / 2**20)} exe "
                 f"{(r.get('executable_sha256') or '')[:16]} source {(r.get('source_sha') or '')[:8]} build_type "
                 f"{r.get('build_type')} argv_sha256 {(r.get('argv_sha256') or '')[:16]} git {r.get('git')!r} "
                 f"store-block receipt {'store' in r} start {'store' in s}")
        if ref is not None:
            got = r["command"][1:]
            diff = [(i, a, b) for i, (a, b) in enumerate(zip(ref, got)) if a != b]
            line += f" | argv vs reference: {len(ref)} / {len(got)} tokens, differing {diff}"
            if len(got) > len(ref):
                line += f", appended {got[len(ref):]}"
    print(line)
    with (LOGS / "nav-runs.txt").open("a", encoding="utf-8") as fh:
        fh.write(line + "\n")
    return rc


def main(argv: list[str]) -> int:
    verb, cell, build = argv[0], argv[1], argv[2]
    rel, bt = BUILDS[build]
    exe = REPO / rel
    if verb == "printspec":
        spec = LOGS / f"{cell}.navspec.json"
        done = subprocess.run([str(exe), *ref_argv(cell), "--print-spec"], cwd=REPO, capture_output=True)
        spec.write_bytes(done.stdout)
        line = (f"printspec {cell} exe {sha_file(exe)[:16]} exit {done.returncode} stderr-bytes {len(done.stderr)} "
                f"spec {spec.name} bytes {len(done.stdout)} sha256 {hashlib.sha256(done.stdout).hexdigest()}")
        print(line)
        with (LOGS / "nav-runs.txt").open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")
        return done.returncode
    out = argv[3]
    if verb == "argv":
        ref = ref_argv(cell)
        args = list(ref)
        k = args.index("--output")
        args[k + 1] = f"build-equity/{out}"
        return bounded(out, exe, bt, args + argv[4:], ref)
    if verb == "spec":
        spec = (LOGS / f"{cell}.navspec.json").relative_to(REPO).as_posix()
        return bounded(out, exe, bt, ["nav", "--spec", spec, "--output", f"build-equity/{out}"], None)
    raise SystemExit(f"unknown verb {verb}")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
