"""Cells root (hand cells P9-L / P9-X / P9-V): the driver's own reader and bundle argvs (scripts/wave_steps.reader_argv
and bundle_argv, the forms wave_stage_cell.read_once / match_engine use), run under the bounded runner, with a print
that carries ONLY the keys the hand step needs. Nothing else of a reader's output is printed.

usage (cwd = the pool root; interpreter C:/Program Files/Python312/python.exe):
  hand_readers.py mech  OUT_DIR NAME NAVKEY=DIR [NAVKEY=DIR ...] [--keys gross|mechanics]
      the mechanics reader (wave_readers.py mechanics: construction keys only, no return / Sharpe / NAV figure)
      into OUT_DIR/NAME.json, run dir OUT_DIR/NAME-run1; prints per NAV the dir, scenario, daily CSV SHA-256 and
      either the all-rows S2 gross alone (--keys gross: G_parent of PM6-6) or the v8-mech rows
      (scripts/wave_rules.mechanics_check: the six rows, value vs limit, pass) (--keys mechanics)
  hand_readers.py bundle OUT_DIR NAME BASE_DIR CELL_DIR EVAL_EXE
      nav_summ --engine-stats EVAL_EXE --protocol v8 --bundle BASE CELL --bundle-json OUT_DIR/NAME.json (the pinned
      verb, B2-11), run dir OUT_DIR/NAME-run1; prints ONLY paired.{dsr, memmel_se, sessions, rho, degenerate},
      paired.lw.{p_value, p_one_sided} and nav_summ_run.engine_exe_sha256 (the B2-11 hash check by hand); the year
      tables and every other key of the bundle file are not read
Every run waits for admission up to 1800 s (the runner's --admission-wait-seconds; HOST-6a).
"""
from __future__ import annotations

import hashlib
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "scripts"))
import wave_rules as WR  # noqa: E402
import wave_steps as WS  # noqa: E402

PY = "C:/Program Files/Python312/python.exe"
DRIVER = {"admission_wait_seconds": 1800}


def series_files(navs: list[str]) -> list[str]:
    """wave_stage_cell.nav_series: the NAVs' daily_*.csv and capacity_curve.csv, bound with their SHA-256."""
    out = []
    for d in navs:
        p = ROOT / d
        out += [f"{d}/{f.name}" for f in sorted(p.glob("daily_*.csv"))] if p.is_dir() else []
        out += [f"{d}/capacity_curve.csv"] if (p / "capacity_curve.csv").is_file() else []
    return out


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run(argv: list[str], run_dir: str) -> dict:
    done = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True)
    r = json.loads((ROOT / run_dir / "receipt.json").read_text(encoding="utf-8")) \
        if (ROOT / run_dir / "receipt.json").is_file() else {}
    if not r:   # the runner refused before it ran anything (no receipt): its own refusal text only, no reader output
        print("runner refusal (stderr tail): " + " | ".join(done.stderr.strip().splitlines()[-6:]))
    peak = (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20
    print(f"runner exit {done.returncode}; receipt {run_dir}/receipt.json: outcome {r.get('outcome')} exit "
          f"{r.get('exit_code')} wall {r.get('wall_seconds')} s peak {peak} MiB exe "
          f"{(r.get('executable_sha256') or '')[:16]} source {(r.get('source_sha') or '')[:8]} store-block "
          f"{'store' in r}")
    return r


def mech(out_dir: str, name: str, pairs: list[str], keys: str) -> int:
    navs = dict(p.split("=", 1) for p in pairs)
    out, run_dir = f"{out_dir}/{name}.json", f"{out_dir}/{name}-run1"
    argv = WS.reader_argv(PY, "mechanics", navs, out, run_dir, ROOT, series_files(list(navs.values())), DRIVER)
    print("argv: " + " ".join(shlex.quote(a) for a in argv))
    r = run(argv, run_dir)
    if r.get("outcome") != "completed" or r.get("exit_code") != 0:
        return 1
    doc = json.loads((ROOT / out).read_text(encoding="utf-8"))
    print(f"output {out} sha256 {sha(ROOT / out)}")
    for k, d in navs.items():
        m = doc["navs"][k]
        print(f"{k}: dir {m['dir']} scenario {m['scenario']} daily_csv_sha256 {m['daily_csv_sha256']}")
        if keys == "gross":
            print(f"  mean_gross_leverage_all_rows {m['mean_gross_leverage_all_rows']!r}")
        else:
            chk = WR.mechanics_check(m, "v8-mech")
            for row in chk["rows"]:
                print(f"  {row['check']}: {row['value']!r} vs {row['limit']}: {'pass' if row['pass'] else 'FAIL'}")
            print(f"  v8-mech (unscaled): {'PASS' if chk['pass'] else 'FAIL'} "
                  f"({sum(r['pass'] for r in chk['rows'])} / {len(chk['rows'])} rows)")
    return 0


def bundle(out_dir: str, name: str, base: str, cell: str, verb: str) -> int:
    out, run_dir = f"{out_dir}/{name}.json", f"{out_dir}/{name}-run1"
    argv = WS.bundle_argv(PY, base, cell, out, run_dir, ROOT, series_files([base, cell]), DRIVER, verb)
    print("argv: " + " ".join(shlex.quote(a) for a in argv))
    r = run(argv, run_dir)
    if r.get("outcome") != "completed" or r.get("exit_code") != 0:
        return 1
    doc = json.loads((ROOT / out).read_text(encoding="utf-8"))
    p = doc["paired"]
    lw = p.get("lw") or {}
    run_meta = doc.get("nav_summ_run") or p.get("nav_summ_run") or {}
    print(f"output {out} sha256 {sha(ROOT / out)}")
    print(f"paired (cell {doc.get('final')} vs base {doc.get('base')}): dsr {p.get('dsr')!r} memmel_se "
          f"{p.get('memmel_se')!r} lw.p_value (two-sided) {lw.get('p_value')!r} lw.p_one_sided "
          f"{lw.get('p_one_sided')!r} sessions {p.get('sessions')} rho {p.get('rho')!r} degenerate "
          f"{p.get('degenerate', False)}")
    print(f"nav_summ_run.engine_exe_sha256 {run_meta.get('engine_exe_sha256')} (pin of exes.eval: "
          f"{sha(ROOT / verb)})")
    return 0


def main(argv: list[str]) -> int:
    if not argv:
        raise SystemExit(__doc__)
    if argv[0] == "mech":
        keys = "mechanics"
        if "--keys" in argv:
            i = argv.index("--keys")
            keys = argv[i + 1]
            argv = argv[:i] + argv[i + 2:]
        return mech(argv[1], argv[2], argv[3:], keys)
    if argv[0] == "bundle":
        return bundle(*argv[1:6])
    raise SystemExit(__doc__)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
