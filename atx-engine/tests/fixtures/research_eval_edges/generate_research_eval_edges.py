"""The eval verb's edge-case fixture (platform P9 lane B2, fix round 1: rulings B2-R1 and B2-R2).

Two inputs where a faithful port cannot match the Python bit for bit, or did not, with the values today's Python
statistics of record compute on them, so the engine (gtest EvalEdges.*, atx-engine-research-eval-tests) is tied to
them under the rulings' rules:

  paired     B2-R1: a cell b against its reference a at a stated 1 - rho. The series come from an exact generator
             (below) that the gtest repeats bit for bit, so no series file is committed. Python:
             nav_summ.paired_stats(b, a, draws 999, block 21, seed 20260929, one-sided). A case with 1 - rho >= 1e-6
             ties within 1e-12 / (1 - rho) relative with equal counts; below 1e-6 the engine refuses the pair
             (degenerate: SE / t / p null) and the Python value is recorded for the record only.
  cscv       B2-R2: five generated candidates and one constant 0.001 series, 240 sessions, 8 blocks. Python:
             backtest_integrity.cscv_pbo (exhaustive). The constant candidate's Sharpe is 0 in every split.
  house_dsr  B2-9: the house DSR rows at N from 2 to 1e6 (HOUSE_N). Python: norm_ppf (bisection) at 1 - 1/N and
             1 - 1/(N e); expected_max_sr and deflated_sharpe at each moment set and V of ``direct``; and the verb's
             rows on two generated cells (the reference a and HOUSE_SEED's cell at HOUSE_DRIFT): nav_summ.dsr_rows
             (cross) and backtest_integrity.lo_null_dsr (lo) at dsr_n = N. Tied within 1e-12 relative for every N.

  python generate_research_eval_edges.py --write    regenerate expected.json (a deliberate fixture change)
  python generate_research_eval_edges.py --check    recompute the values from the committed parameters (exit 0 = tied)

The generator (each step one IEEE-754 double operation, so C++ and Python produce the same bits): a 64-bit LCG
s <- s * 6364136223846793005 + 1442695040888963407 (mod 2^64), u = (s >> 11) * 2^-53, then
  reference a_k = ((u - 0.5) * 0.02) + 0.0004                (stream A_SEED)
  noise     n_k = u - 0.5                                    (stream N_SEED)
  cell      b_k = (a_k - SHIFT) + (eps * n_k)                (SHIFT 0: the difference is noise; eps 0 is a)
  candidate c of the CSCV grid: ((u - 0.5) * 0.02) + drift_c (stream CSCV_SEED + c); the last column 0.001.
1 - rho is measured on the standardized difference series, sum((za - zb)^2) / (2 (T - 1)), the stable form the
engine uses (ruling B2-R1 (a)); eps is bisected to the target 1 - rho at --write and committed.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import platform
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
TOOLS = REPO / "atx-impl" / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import backtest_integrity as BI  # noqa: E402
import nav_summ as NS  # noqa: E402

SCHEMA = "atx.research-eval-edges-fixture/v1"
EXPECTED = "expected.json"
MASK = (1 << 64) - 1
LCG_MUL, LCG_ADD = 6364136223846793005, 1442695040888963407
A_SEED, N_SEED, CSCV_SEED = 20261003, 777, 4242
T, DRAWS, BLOCK, SEED = 750, 999, 21, 20260929
SHIFT = 0.0
DEGENERATE = 1e-6                     # ruling B2-R1 (c): 1 - rho below this is refused
# target 1 - rho per case: the ruling's 5e-4 / 5e-5 / 1e-6 boundary (both sides) and the review's 5e-6 / 5e-9 / 6e-13.
# 5e-4 is nudged 0.1% to 5.005e-4: at exactly 5e-4, rho = 0.9995 sits on the rounding boundary of nav_summ's 3-decimal
# rho print, where a 1e-16 difference flips the printed digit (no tolerance can keep a printed boundary byte-equal).
TARGETS = (("omr_5e-4", 5.005e-4), ("omr_5e-5", 5e-5), ("omr_5e-6", 5e-6), ("omr_1.02e-6", 1.02e-6),
           ("omr_0.98e-6", 0.98e-6), ("omr_5e-9", 5e-9), ("omr_6e-13", 6e-13))
CSCV_T, CSCV_BLOCKS, CSCV_CONSTANT = 240, 8, 0.001
CSCV_DRIFTS = (0.0004, -0.0002, 0.0001, 0.0003, -0.0001)
# ruling B2-9: the trial counts N of the house DSR tie, 2 to 1e6 (the recorded N_tot are 13..233; a mined campaign's
# registry count can pass 1e4), and the second cell of the verb rows
HOUSE_N = (2, 3, 4, 5, 7, 10, 13, 41, 100, 233, 1000, 4097, 10000, 33333, 100000, 314159, 1000000)
HOUSE_SEED, HOUSE_DRIFT = 9090, -0.0002
# (label, per-session SR, sessions, skew, raw kurtosis, V): a v8 verdict book against the window variance of SD .15
# annual, the same book against Lo's single-cell variance, and a fat-tailed short book
HOUSE_DIRECT = (("window", 0.06, 1006, -0.3, 4.5, (0.15 / math.sqrt(252)) ** 2),
                ("lo", 0.06, 1006, -0.3, 4.5, (1.0 + 0.06 * 0.06 / 2.0) / 1006),
                ("fat", 0.11, 252, -1.2, 9.0, (0.4 / math.sqrt(252)) ** 2))


def uniforms(seed: int, n: int) -> list[float]:
    s, out = seed & MASK, []
    for _ in range(n):
        s = (s * LCG_MUL + LCG_ADD) & MASK
        out.append((s >> 11) * 2.0 ** -53)
    return out


def reference() -> np.ndarray:
    return np.array([((u - 0.5) * 0.02) + 0.0004 for u in uniforms(A_SEED, T)])


def noise() -> np.ndarray:
    return np.array([u - 0.5 for u in uniforms(N_SEED, T)])


def cell(eps: float, shift: float = SHIFT) -> np.ndarray:
    return np.array([(a - shift) + (eps * n) for a, n in zip(reference(), noise())])


def one_minus_rho(a: np.ndarray, b: np.ndarray) -> float:
    za = (a - a.mean()) / a.std(ddof=1)
    zb = (b - b.mean()) / b.std(ddof=1)
    return float(np.sum((za - zb) ** 2) / (2.0 * (a.size - 1)))


def bisect_eps(target: float) -> float:
    a = reference()
    lo, hi = 0.0, 1.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if mid in (lo, hi):
            break
        if one_minus_rho(a, cell(mid)) < target:
            lo = mid
        else:
            hi = mid
    return hi


def plain(x):
    """JSON-safe: NaN -> None, numpy scalars -> Python, tuples -> lists."""
    if isinstance(x, dict):
        return {k: plain(v) for k, v in x.items()}
    if isinstance(x, (list, tuple, np.ndarray)):
        return [plain(v) for v in x]
    if isinstance(x, (np.floating, float)):
        return float(x) if math.isfinite(x) else None
    if isinstance(x, np.integer):
        return int(x)
    return x


def paired_case(name: str, target: float | None, eps: float, shift: float) -> dict:
    a, b = reference(), cell(eps, shift)
    p = NS.paired_stats(b, a, draws=DRAWS, block=BLOCK, seed=SEED, one_sided=True)
    omr = one_minus_rho(b, a)
    return {"name": name, "target_one_minus_rho": target, "eps": eps, "shift": shift, "one_minus_rho": omr,
            "numpy_one_minus_rho": 1.0 - float(np.corrcoef(b, a)[0, 1]), "degenerate": omr < DEGENERATE,
            "python": plain(p)}


def cscv_matrix() -> np.ndarray:
    cols = [[((u - 0.5) * 0.02) + d for u in uniforms(CSCV_SEED + c, CSCV_T)] for c, d in enumerate(CSCV_DRIFTS)]
    cols.append([CSCV_CONSTANT] * CSCV_T)
    return np.array(cols).T


def cscv_case() -> dict:
    r = BI.cscv_pbo(cscv_matrix(), blocks=CSCV_BLOCKS)
    r = dict(r, logits=[float(x) for x in r["logits"]])
    return {"sessions": CSCV_T, "blocks": CSCV_BLOCKS, "constant": CSCV_CONSTANT, "drifts": list(CSCV_DRIFTS),
            "seed": CSCV_SEED, "python": plain(r)}


def house_cell() -> np.ndarray:
    return np.array([((u - 0.5) * 0.02) + HOUSE_DRIFT for u in uniforms(HOUSE_SEED, T)])


def house_dsr_case() -> dict:
    """Ruling B2-9: the Python house DSR at every N of HOUSE_N, directly and as the verb's cross / lo rows."""
    ppf = [{"n": n, "p_hi": 1.0 - 1.0 / n, "ppf_hi": NS.norm_ppf(1.0 - 1.0 / n),
            "p_lo": 1.0 - 1.0 / (n * math.e), "ppf_lo": NS.norm_ppf(1.0 - 1.0 / (n * math.e))} for n in HOUSE_N]
    direct = []
    for label, sr, t, skew, kurt, v in HOUSE_DIRECT:
        rows = []
        for n in HOUSE_N:
            sr0 = NS.expected_max_sr(v, n)
            rows.append({"n": n, "sr0_daily": sr0, "dsr": NS.deflated_sharpe(sr, t, skew, kurt, sr0)})
        direct.append({"label": label, "sr_daily": sr, "sessions": t, "skew": skew, "kurtosis": kurt, "variance": v,
                       "rows": rows})
    ma, mb = NS.net_moments(reference()), NS.net_moments(house_cell())
    verb = [{"n": n, "cross": NS.dsr_rows([ma, mb], n), "lo": BI.lo_null_dsr(ma, n)} for n in HOUSE_N]
    return plain({"n": list(HOUSE_N), "seed": HOUSE_SEED, "drift": HOUSE_DRIFT, "norm_ppf": ppf, "direct": direct,
                  "verb_moments": [ma, mb], "verb": verb})


def document(eps_by_name: dict[str, float]) -> dict:
    cases = [paired_case(n, t, eps_by_name[n], SHIFT) for n, t in TARGETS]
    cases.append(paired_case("self", None, 0.0, 0.0))
    return {"schema": SCHEMA, "generator": "atx-engine/tests/fixtures/research_eval_edges/generate_research_eval_edges.py",
            "host": {"python": platform.python_version(), "numpy": np.__version__},
            "lcg": {"mul": LCG_MUL, "add": LCG_ADD, "a_seed": A_SEED, "n_seed": N_SEED},
            "paired": {"sessions": T, "draws": DRAWS, "block": BLOCK, "seed": SEED, "one_sided": True,
                       "degenerate_below": DEGENERATE, "tolerance": "1e-12 / (1 - rho) relative (ruling B2-R1 (b))",
                       "cases": cases},
            "cscv": cscv_case(), "house_dsr": house_dsr_case()}


def encode(doc) -> bytes:
    return (json.dumps(doc, indent=1, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def write(directory: Path = HERE) -> dict:
    doc = document({n: bisect_eps(t) for n, t in TARGETS})
    (directory / EXPECTED).write_bytes(encode(doc))
    return doc


def committed(directory: Path = HERE) -> dict:
    return json.loads((directory / EXPECTED).read_bytes())


def check(directory: Path = HERE) -> list[str]:
    want = committed(directory)
    eps = {c["name"]: c["eps"] for c in want["paired"]["cases"] if c["name"] != "self"}
    got = json.loads(encode(document(eps)))
    got["host"] = want["host"]
    return [] if want == got else ["expected.json differs from today's Python on the committed parameters"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the eval verb's edge-case fixture (P9 B2 fix round 1)")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    if a.write:
        write()
        return 0
    problems = check()
    for p in problems:
        print(f"research_eval_edges: {p}", file=sys.stderr)
    print("research_eval_edges: tied" if not problems else "research_eval_edges: differs")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
