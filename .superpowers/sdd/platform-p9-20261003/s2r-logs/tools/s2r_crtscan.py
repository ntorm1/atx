"""S2R: scan ucrtbase.dll vs ucrtbased.dll (this host) for last-bit differences, per libm function."""
import ctypes, random, struct
rel = ctypes.CDLL(r"C:\Windows\System32\ucrtbase.dll")
dbg = ctypes.CDLL(r"C:\Windows\System32\ucrtbased.dll")
D = ctypes.c_double
for L in (rel, dbg):
    for n in ("exp", "log", "tanh", "sqrt", "log1p", "expm1", "log2", "cbrt"):
        getattr(L, n).restype = D; getattr(L, n).argtypes = (D,)
    L.pow.restype = D; L.pow.argtypes = (D, D)
    L._set_FMA3_enable.argtypes = (ctypes.c_int,)
rng = random.Random(0x5212)
N = 200_000
def u(lo, hi): return rng.uniform(lo, hi)
unary = {
    "exp": lambda: u(-20, 20), "log": lambda: 10 ** u(-6, 9), "tanh": lambda: u(-5, 5),
    "sqrt": lambda: 10 ** u(-6, 9), "log1p": lambda: u(-0.5, 2), "expm1": lambda: u(-2, 2),
    "log2": lambda: 10 ** u(-6, 9), "cbrt": lambda: u(-1e6, 1e6),
}
binary = {
    "pow(x, 0.5)": (lambda: 10 ** u(-4, 6), 0.5), "pow(x, 2)": (lambda: u(-10, 10), 2.0),
    "pow(x, 3)": (lambda: u(-100, 100), 3.0), "pow(x, 0.3)": (lambda: 10 ** u(-4, 6), 0.3),
    "pow(x, 1.5)": (lambda: 10 ** u(-4, 6), 1.5), "pow(x, y) y~U(-3,3)": (lambda: 10 ** u(-4, 6), None),
}
for fma in (1, 0):
    rel._set_FMA3_enable(fma); dbg._set_FMA3_enable(fma)
    print(f"--- FMA3 enable = {fma} (both DLLs); N = {N} per function")
    for n, g in unary.items():
        fr, fd = getattr(rel, n), getattr(dbg, n)
        diff = sum(1 for _ in range(N) if (lambda x: fr(x) != fd(x))(g()))
        print(f"    {n:22s} differing {diff}")
    for n, (g, e) in binary.items():
        diff = 0
        for _ in range(N):
            x = abs(g()) if e != 2.0 and e != 3.0 else g()
            y = e if e is not None else u(-3, 3)
            if rel.pow(x, y) != dbg.pow(x, y):
                diff += 1
        print(f"    {n:22s} differing {diff}")
rel._set_FMA3_enable(1); dbg._set_FMA3_enable(1)
