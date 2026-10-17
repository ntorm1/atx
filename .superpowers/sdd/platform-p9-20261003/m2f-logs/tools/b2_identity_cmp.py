"""M2f B2 TRAIN identity comparator (rulings B2-R1 (b), B2-6, B2-7): the engine eval verb vs the Python statistics.

For each pair (X5, YF0) the runs P (Python), Erel (--engine-stats, Release verb) and Edbg (Debug verb) were made by
run_bounded_research.py into build-equity/p9-m2f-b2-<pair>-<mode>-run (stdout.log / stderr.log, receipt.json) with
--json build-equity/p9-m2f-b2-identity/<pair>-<mode>.json. Compared, per pair, E vs P for E in (Erel, Edbg), and
Erel vs Edbg:
  - stdout bytes and stderr bytes;
  - the --json rows with nav_summ_run popped, by the lane's comparator test_nav_summ_engine.close (floats within
    1e-12 relative, a "paired" block within 1e-12 / (1 - rho), p_value / p_one_sided / valid_draws / cbb_valid_draws
    exact; structure, ints and strings exact);
  - counts of float leaves bit-equal / within tolerance, and the largest relative difference (a number about the two
    implementations, not about the book);
  - E's nav_summ_run names the verb it ran (engine_exe, engine_exe_sha256 = the file's SHA-256);
  - no "degenerate" / "one_minus_rho" key (both pairs are non-degenerate, b2-rho-pairs.txt).
A stdout line that differs (B2-7) is printed with every JSON float leaf that differs between the two runs (path, both
values, relative difference) and the decision-field check; it is reported to the PM, never accepted here.
Nothing else of stdout is printed.
"""
from __future__ import annotations

import hashlib
import json
import math
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-2")
sys.path.insert(0, str(ROOT / "atx-impl" / "tools"))
import test_nav_summ_engine as TE  # noqa: E402  (the lane's comparator `close`)

OUT = ROOT / "build-equity" / "p9-m2f-b2-identity"
PAIRS = {"X5": 1.254e-11, "YF0": 2.252e-11}           # 1e-12 / (1 - rho), b2-rho-pairs.txt
MODES = ("P", "Erel", "Edbg")
DECISION_KEYS = ("p_value", "p_one_sided", "valid_draws", "cbb_valid_draws", "n", "dsr_n", "count", "trials",
                 "ledger_n", "n_tot", "degenerate", "pass", "verdict")


def run_dir(pair: str, mode: str) -> Path:
    return ROOT / "build-equity" / f"p9-m2f-b2-{pair.lower()}-{mode.lower()}-run"


def load_rows(pair: str, mode: str) -> tuple[list, list]:
    doc = json.loads((OUT / f"{pair}-{mode}.json").read_text(encoding="utf-8"))
    rows = doc if isinstance(doc, list) else [doc]
    runs = [r.pop("nav_summ_run", None) for r in rows]
    return rows, runs


def leaves(x, path=""):
    if isinstance(x, dict):
        for k, v in x.items():
            yield from leaves(v, f"{path}.{k}")
    elif isinstance(x, list):
        for i, v in enumerate(x):
            yield from leaves(v, f"{path}[{i}]")
    else:
        yield path, x


def float_stats(a, b) -> tuple[int, int, float, list]:
    la, lb = dict(leaves(a)), dict(leaves(b))
    bit, near, worst, diffs = 0, 0, 0.0, []
    for p, x in la.items():
        y = lb.get(p)
        if isinstance(x, float) and isinstance(y, float):
            if x == y or (math.isnan(x) and math.isnan(y)):
                bit += 1
            else:
                near += 1
                rel = abs(x - y) / max(abs(x), abs(y), 1e-300)
                worst = max(worst, rel)
                diffs.append((p, x, y, rel))
    return bit, near, worst, diffs


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def compare(pair: str, ma: str, mb: str) -> bool:
    ok = True
    da, db = run_dir(pair, ma), run_dir(pair, mb)
    out_a, out_b = (da / "stdout.log").read_bytes(), (db / "stdout.log").read_bytes()
    err_a, err_b = (da / "stderr.log").read_bytes(), (db / "stderr.log").read_bytes()
    ra, runs_a = load_rows(pair, ma)
    rb, runs_b = load_rows(pair, mb)
    print(f"\n== {pair}: {mb} vs {ma}")
    print(f"  stdout: {len(out_a.splitlines())} vs {len(out_b.splitlines())} lines, byte-equal {out_a == out_b}")
    print(f"  stderr: {len(err_a)} vs {len(err_b)} bytes, byte-equal {err_a == err_b}")
    ok &= err_a == err_b
    problems = TE.close(ra, rb)
    bit, near, worst, diffs = float_stats(ra, rb)
    print(f"  json rows {len(ra)} vs {len(rb)}; close() problems {len(problems)}; float leaves bit-equal {bit}, "
          f"differing within tolerance or not {near}; largest relative difference {worst:.3e}")
    for p in problems:
        print(f"    PROBLEM {p}")
    ok &= not problems
    keys_a = {p for p, _ in leaves(ra)}
    keys_b = {p for p, _ in leaves(rb)}
    bad = sorted(k for k in keys_a | keys_b if k.rsplit(".", 1)[-1] in ("degenerate", "one_minus_rho"))
    print(f"  degenerate / one_minus_rho keys: {bad or 'none'}")
    ok &= not bad
    if out_a != out_b:
        ok = False
        la, lb = out_a.decode("utf-8").splitlines(), out_b.decode("utf-8").splitlines()
        print("  B2-7 CANDIDATES (differing stdout lines; for the PM, not accepted):")
        for i, (x, y) in enumerate(zip(la, lb)):
            if x != y:
                print(f"    line {i + 1}:\n      {ma}: {x}\n      {mb}: {y}")
        if len(la) != len(lb):
            print(f"    line counts differ: {len(la)} vs {len(lb)}")
        print("    every differing JSON float leaf (path, both values, relative difference):")
        for p, x, y, rel in diffs:
            print(f"      {p}: {x!r} | {y!r} | {rel:.3e}")
        dec = [(p, x, y) for (p, x), (_, y) in zip(sorted(leaves(ra)), sorted(leaves(rb)))
               if p.rsplit(".", 1)[-1] in DECISION_KEYS and x != y]
        print(f"    decision fields differing: {dec or 'none'}")
    for mode, runs in ((ma, runs_a), (mb, runs_b)):
        for r in runs:
            exe = (r or {}).get("engine_exe")
            if mode == "P":
                print(f"  {mode} nav_summ_run names a verb: {bool(exe)}")
                ok &= not exe
            else:
                real = sha256(Path(exe)) if exe and Path(exe).is_file() else None
                same = bool(real) and r.get("engine_exe_sha256") == real
                print(f"  {mode} nav_summ_run engine_exe {exe} sha256 {str(r.get('engine_exe_sha256'))[:16]} "
                      f"= file {same}")
                ok &= same
    print(f"  RESULT {'IDENTICAL under B2-R1 (b)' if ok else 'NOT IDENTICAL (see above)'}")
    return ok


def main() -> int:
    every = True
    for pair in PAIRS:
        for mb in ("Erel", "Edbg"):
            every &= compare(pair, "P", mb)
        every &= compare(pair, "Erel", "Edbg")
    print(f"\nOVERALL {'HOLDS' if every else 'DOES NOT HOLD'}")
    return 0 if every else 1


if __name__ == "__main__":
    raise SystemExit(main())
