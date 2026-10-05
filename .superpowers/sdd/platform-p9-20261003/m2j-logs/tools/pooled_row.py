"""D-COV-EIG third pooled eigen row ("without structurally null ranks"), from the per-rank values already printed in gate
report item 4 (d) (m2i-logs/dcov-read.txt: rank, T_k, b_k per recipe), as the pre-run list of item 4 (e) states:
N = sum T_k over the included ranks, band sqrt(2 / N), pooled b = sqrt(sum (T_k - 1) b_k^2 / (N - 1)); the same formula
on the two existing rows' rank sets is printed beside their printed values (the residual is the between-rank term of
the z means the printed values do not carry). Opens no store file.

usage: python pooled_row.py DCOV_READ_TXT EXCLUDED_V11 EXCLUDED_COVV1   (comma-separated ranks, or "none")
"""
from __future__ import annotations

import math
import re
import sys

ROW = re.compile(r"^eigen-(\d\d)\s+(\d+)\s+[\d.]+ \|\s+([\d.]+)\s+\w .*?\|\s+([\d.]+)\s+\w")
POOLED = re.compile(r"^(pooled, [^N]+?)\s+N (\d+)\s+band ([\d.]+) \| v1.1 b ([\d.]+) .*?atx-cov-v1 b ([\d.]+)")


def ranks(arg: str) -> set[int]:
    return set() if arg == "none" else {int(x) for x in arg.split(",")}


def pooled(rows, keep: set[int], col: int) -> tuple[int, float]:
    sel = [r for r in rows if r[0] in keep]
    n = sum(r[1] for r in sel)
    return n, math.sqrt(sum((r[1] - 1) * r[col] ** 2 for r in sel) / (n - 1))


def main(argv: list[str]) -> int:
    lines = open(argv[0], encoding="utf-8").read().splitlines()
    rows = [(int(m[1]), int(m[2]), float(m[3]), float(m[4])) for m in map(ROW.match, lines) if m]
    printed = [m.groups() for m in map(POOLED.match, lines) if m]
    allr = {r[0] for r in rows}
    print(f"per-rank rows read: {len(rows)} (ranks {min(allr):02d}-{max(allr):02d})")
    for label, n, band, b11, bcov in printed:
        keep = allr if "all ranks" in label else allr - {0}
        n1, f11 = pooled(rows, keep, 2)
        _, fcov = pooled(rows, keep, 3)
        print(f"{label.strip():<26} printed (pooled z SD): N {n} band {band} | v1.1 {b11} | atx-cov-v1 {bcov}; "
              f"per-rank formula: N {n1} | v1.1 {f11:.4f} (residual {float(b11) - f11:+.4f}) | atx-cov-v1 "
              f"{fcov:.4f} (residual {float(bcov) - fcov:+.4f})")
    x11, xcov = ranks(argv[1]), ranks(argv[2])
    k11, kcov = allr - x11, allr - xcov
    n11, f11 = pooled(rows, k11, 2)
    ncov, fcov = pooled(rows, kcov, 3)
    same = k11 == allr and kcov == allr
    print(f"excluded as structurally null: v1.1 {sorted(x11) or 'none'} | atx-cov-v1 {sorted(xcov) or 'none'}")
    print(f"pooled, without structurally null ranks: N {n11 if n11 == ncov else f'{n11}/{ncov}'} band "
          f"{math.sqrt(2 / n11):.4f} | per-rank formula v1.1 {f11:.4f} | atx-cov-v1 {fcov:.4f}")
    if same:
        allrow = next(p for p in printed if "all ranks" in p[0])
        print(f"no rank excluded in either recipe: the row's rank set IS the all-ranks set, so by the list's own pooling "
              f"(sample SD of the pooled z) its value is the printed all-ranks row exactly: N {allrow[1]} band "
              f"{allrow[2]} | v1.1 {allrow[3]} | atx-cov-v1 {allrow[4]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
