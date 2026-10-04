"""S2R: call ucrtbase.dll pow and ucrtbased.dll pow on the exact moved-cell inputs."""
import ctypes
import math
import struct
import sys
from collections import defaultdict

DUMP = sys.argv[1]
cells = defaultdict(dict)
with open(DUMP, encoding="utf-8", errors="replace") as fh:
    for line in fh:
        if line.startswith("[lit-cell]"):
            p = line.split()
            cells[int(p[1])][int(p[2])] = p[3]  # ae bits


def d(h):
    return struct.unpack("<d", struct.pack("<Q", int(h, 16)))[0]


def bits(x):
    return f"{struct.unpack('<Q', struct.pack('<d', x))[0]:016x}"


rel = ctypes.CDLL(r"C:\Windows\System32\ucrtbase.dll")
dbg = ctypes.CDLL(r"C:\Windows\System32\ucrtbased.dll")
for lib in (rel, dbg):
    lib.pow.restype = ctypes.c_double
    lib.pow.argtypes = (ctypes.c_double, ctypes.c_double)
    lib.sqrt.restype = ctypes.c_double
    lib.sqrt.argtypes = (ctypes.c_double,)

KI = 12
DELAY, BACKFILL = 27, 53  # rows delay(close, 3), ts_backfill(returns, 5)
cases = [
    ("power(close, 0.5)", 5, 407, lambda c: d(cells[DELAY][c + 3 * KI]), 0.5, False),
    ("close ^ 3", 83, 296, lambda c: d(cells[DELAY][c + 3 * KI]), 3.0, False),
]
# signedpower(returns, 2): every cell whose bits differ between the two dumps is passed on argv[2]
for c in (int(x) for x in sys.argv[2].split(",")):
    cases.append(("signedpower(returns, 2)", 6, c, lambda c: d(cells[BACKFILL][c]), 2.0, True))

for name, row, c, getx, e, signed in cases:
    x = getx(c)
    a = abs(x) if signed else x
    pr, pd = rel.pow(a, e), dbg.pow(a, e)
    s = math.copysign(1.0, x) if signed else 1.0
    exact = {0.5: math.sqrt(a), 2.0: a * a, 3.0: None}[e]
    print(f"{name:24s} cell {c:3d} x={x!r} ({bits(x)})")
    print(f"    ucrtbase  pow = {bits(s * pr)}   ucrtbased pow = {bits(s * pd)}   "
          f"dump Debug ae = {cells[row][c]}"
          + (f"   correctly-rounded ref = {bits(s * exact)}" if exact is not None else ""))
