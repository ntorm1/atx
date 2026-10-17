"""S2R: full-cell attribution of rows 5 / 6 / 83 to the CRT pow, and the Debug cost of source forms."""
import ctypes
import math
import struct
import sys
from collections import defaultdict
from fractions import Fraction

DBG_DUMP, REL_DUMP = sys.argv[1], sys.argv[2]
KI, KD = 12, 64


def load(path):
    cells = defaultdict(dict)
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("[lit-cell]"):
                p = line.split()
                cells[int(p[1])][int(p[2])] = (p[3], p[4], p[5])
    return cells


def d(h):
    return struct.unpack("<d", struct.pack("<Q", int(h, 16)))[0]


def b(x):
    if math.isnan(x):
        return "nan"
    return f"{struct.unpack('<Q', struct.pack('<d', x))[0]:016x}"


def canon(h):
    x = d(h)
    return "nan" if math.isnan(x) else h


def cr_int_pow(x, n):
    """Correctly rounded x**n for a small positive integer n (exact rational, one rounding)."""
    return float(Fraction(x) ** n)


dbg, rel = load(DBG_DUMP), load(REL_DUMP)
crt = {}
for tag, path in (("ucrtbase", r"C:\Windows\System32\ucrtbase.dll"),
                  ("ucrtbased", r"C:\Windows\System32\ucrtbased.dll")):
    lib = ctypes.CDLL(path)
    lib.pow.restype = ctypes.c_double
    lib.pow.argtypes = (ctypes.c_double, ctypes.c_double)
    crt[tag] = lib.pow

DELAY, BACKFILL = 27, 53
# Inputs per cell, read back from rows that carry the field bits unchanged; cells whose source row is
# NaN there (warm-up of delay, NaN returns) are checked to be NaN in the target row.
close = {}
for c in range(KD * KI):
    if c + 3 * KI < KD * KI:
        close[c] = dbg[DELAY][c + 3 * KI][0]
ret = {c: dbg[BACKFILL][c][0] for c in range(KD * KI)}

rows = {
    5: ("power(close, 0.5)", close, 0.5, False),
    6: ("signedpower(returns, 2)", ret, 2.0, True),
    83: ("close ^ 3", close, 3.0, False),
}
forms = {
    "sqrt / x*x / (x*x)*x": lambda a, e: {0.5: math.sqrt(a), 2.0: a * a, 3.0: (a * a) * a}[e],
    "sqrt / x*x / x*(x*x)": lambda a, e: {0.5: math.sqrt(a), 2.0: a * a, 3.0: a * (a * a)}[e],
    "correctly rounded": lambda a, e: math.sqrt(a) if e == 0.5 else cr_int_pow(a, int(e)),
}
for row, (name, src, e, signed) in rows.items():
    tally = defaultdict(int)
    for c in range(KD * KI):
        if c not in src:
            continue
        x = d(src[c])
        if math.isnan(x) or math.isnan(d(dbg[row][c][0])):
            continue  # NaN input, or an input-recovery cell (ts_backfill filled a NaN hole)
        a = abs(x) if signed else x
        s = (1.0 if x > 0 else (-1.0 if x < 0 else 0.0)) if signed else 1.0
        tally["cells"] += 1
        got_d = canon(dbg[row][c][0])
        got_r = canon(rel[row][c][0])
        tally["debug==ucrtbased"] += got_d == b(s * crt["ucrtbased"](a, e))
        tally["release==ucrtbase"] += got_r == b(s * crt["ucrtbase"](a, e))
        tally["debug!=release"] += got_d != got_r
        for fname, fn in forms.items():
            moved = got_d != b(s * fn(a, e))
            tally[f"debug cells moved by [{fname}]"] += moved
            if moved and fname == "correctly rounded":
                print(f"    row {row} cell {c}: Debug {got_d} != correctly rounded {b(s * fn(a, e))}")
            tally[f"release cells moved by [{fname}]"] += got_r != b(s * fn(a, e))
    print(f"row {row:02d} {name}:")
    for k, v in tally.items():
        print(f"    {k:48s} {v}")
