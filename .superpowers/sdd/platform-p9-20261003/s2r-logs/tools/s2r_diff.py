"""S2R: diff two DISABLED_PerRowDigestDump outputs (Debug vs Release) per input, row, mode, cell."""
import struct
import sys
from collections import defaultdict


def load(path):
    inputs, rows, cells = {}, {}, defaultdict(dict)
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            p = line.split()
            if line.startswith("[lit-input]"):
                inputs[p[1]] = p[2]
            elif line.startswith("[lit-row]"):
                k = int(p[1])
                rows[k] = (p[2][3:], p[3][3:], p[4][7:], " ".join(p[5:]))
            elif line.startswith("[lit-cell]"):
                cells[int(p[1])][int(p[2])] = (p[3], p[4], p[5])
    return inputs, rows, cells


def f(h):
    return struct.unpack("<d", struct.pack("<Q", int(h, 16)))[0]


def main(a, b):
    ia, ra, ca = load(a)
    ib, rb, cb = load(b)
    for name in ia:
        if ia[name] != ib.get(name):
            print(f"INPUT {name}: {ia[name]} -> {ib.get(name)}")
    modes = ("ae", "rf", "oracle")
    for k in sorted(ra):
        for m, mode in enumerate(modes):
            if ra[k][m] != rb[k][m]:
                diffs = [c for c in ca[k] if ca[k][c][m] != cb[k][c][m]]
                first = diffs[0] if diffs else None
                detail = ""
                if first is not None:
                    x, y = ca[k][first][m], cb[k][first][m]
                    ulp = int(y, 16) - int(x, 16)
                    detail = f" first cell {first} (t={first // 12}, j={first % 12}): {x} {f(x)!r} -> {y} {f(y)!r} (bits {ulp:+d})"
                print(f"ROW {k:02d} {mode:6s} {ra[k][m]} -> {rb[k][m]} cells={len(diffs)}{detail}  {ra[k][3]}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
