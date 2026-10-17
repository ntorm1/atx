"""AL-COMB Q5 (lit section 8 Q5; spec scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json): read q and q_lit
from G-2a's output. Reads ONLY `complete_sessions`, `incomplete_sessions` and five entries of
`style_share_of_summed_variance` (size, residual_vol, liquidity, beta, leverage); every other key of the document
(mean_ex_ante_vol_annual, mean_share, share_of_summed_variance, by_year, mean_gross, mean_uncovered_gross, the other
style columns) is never read or printed.

usage: python q5_read.py G2A_JSON [CROSS_CHECK_G2A_JSON]
"""
from __future__ import annotations

import hashlib
import json
import math
import sys
from pathlib import Path

NO_THEME = ("size", "residual_vol", "liquidity")             # spec columns.no_theme
LIT_EXTRA = ("beta", "leverage")                              # spec columns.lit_upper_bound adds these


def read(path: str) -> dict:
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    g = doc["diagnostics"]["G-2a"]
    if g["status"] != "ok":
        raise SystemExit(f"q5_read: G-2a status {g['status']}")
    r = g["result"]
    shares = r["style_share_of_summed_variance"]
    cols = {k: float(shares[k]) for k in NO_THEME + LIT_EXTRA}
    q = sum(cols[k] for k in NO_THEME)
    return {"sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(), "complete": int(r["complete_sessions"]),
            "incomplete": int(r["incomplete_sessions"]), "cols": cols, "q": q,
            "q_lit": q + sum(cols[k] for k in LIT_EXTRA)}


def bound(q: float) -> str:
    return f"{1.0 / math.sqrt(1.0 - q):.4f}" if q < 1.0 else "n/a"


def main(argv: list[str]) -> int:
    if len(argv) not in (1, 2):
        raise SystemExit(__doc__)
    a = read(argv[0])
    print("AL-COMB Q5 (zero trials): style-variance share of the book on the columns no theme names (G-2a, Euler "
          "contributions over the summed daily ex-ante variance; a column may be negative)")
    print(f"G-2a output {argv[0]}  sha256 {a['sha256']}")
    print(f"sessions: complete {a['complete']}, incomplete {a['incomplete']}")
    print("columns (style_share_of_summed_variance): " + ", ".join(f"{k} {v:+.4f}" for k, v in a["cols"].items()))
    print(f"q     (size + residual_vol + liquidity)        = {a['q']:+.4f}   SR factor bound 1/sqrt(1-q)     "
          f"{bound(a['q'])}")
    print(f"q_lit (q + beta + leverage; lit upper bound) = {a['q_lit']:+.4f}   SR factor bound 1/sqrt(1-q_lit) "
          f"{bound(a['q_lit'])}")
    print("ex-ante vol ratio (ALCOMB-9 (b)): not produced (Q5 reads one book; a ratio needs G-2a on the cell and its "
          "parent)")
    if len(argv) == 2:
        b = read(argv[1])
        print(f"\ncross-check {argv[1]}  sha256 {b['sha256']}")
        print(f"  sessions complete {b['complete']}, incomplete {b['incomplete']}")
        print("  |difference| per column: " + ", ".join(f"{k} {abs(a['cols'][k] - b['cols'][k]):.3e}"
                                                    for k in a["cols"]))
        print(f"  |difference| q {abs(a['q'] - b['q']):.3e}, q_lit {abs(a['q_lit'] - b['q_lit']):.3e}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
