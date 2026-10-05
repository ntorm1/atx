"""COVR: ucrtbase (Release CRT) vs ucrtbased (Debug CRT) exp2 / pow on the risk estimator's decay arguments,
against the correctly rounded value (decimal, 80 digits). Synthetic arguments only (half-lives)."""
import ctypes, struct
from decimal import Decimal, getcontext
getcontext().prec = 80
LN2 = Decimal(2).ln()
def bits(x): return f"{struct.unpack('<Q', struct.pack('<d', x))[0]:016x}"
def cr_exp2(x): return float(str((Decimal(x) * LN2).exp()))       # float(str) rounds to nearest
def cr_pow(d, l): return float(str(Decimal(d) ** l))
rel = ctypes.CDLL(r"C:\Windows\System32\ucrtbase.dll")
dbg = ctypes.CDLL(r"C:\Windows\System32\ucrtbased.dll")
for lib in (rel, dbg):
    lib.pow.restype = ctypes.c_double; lib.pow.argtypes = (ctypes.c_double, ctypes.c_double)
    lib.exp2.restype = ctypes.c_double; lib.exp2.argtypes = (ctypes.c_double,)
print("exp2(-1.0 / H): ucrtbase (Release) / ucrtbased (Debug) / correctly rounded")
for H in (10, 21, 42, 84, 252, 504):
    x = -1.0 / H
    a, b, c = rel.exp2(x), dbg.exp2(x), cr_exp2(x)
    print(f"  H={H:4d} x={bits(x)} rel {bits(a)} dbg {bits(b)} cr {bits(c)} {'SAME' if a == b else 'DIFF'};"
          f" rel==cr {a == c}, dbg==cr {b == c}")
    for d, name in ((a, "d_rel"), (b, "d_dbg")):
        for l in range(1, 6):
            pr, pd, pc = rel.pow(d, float(l)), dbg.pow(d, float(l)), cr_pow(d, l)
            if pr != pd or pr != pc:
                print(f"     pow({name},{l}) rel {bits(pr)} dbg {bits(pd)} cr {bits(pc)}")
n, diff, rel_ok, dbg_ok = 2000, 0, 0, 0
for H in range(1, n + 1):
    x = -1.0 / H
    a, b, c = rel.exp2(x), dbg.exp2(x), cr_exp2(x)
    diff += a != b; rel_ok += a == c; dbg_ok += b == c
print(f"scan H=1..{n}: differ {diff}; ucrtbase correctly rounded {rel_ok}/{n}; ucrtbased correctly rounded {dbg_ok}/{n}")
