"""COVR: ucrtbase vs ucrtbased on the other libm functions the risk estimator path calls (random arguments in the
ranges the estimator uses; synthetic)."""
import ctypes, random, math
rel = ctypes.CDLL(r"C:\Windows\System32\ucrtbase.dll")
dbg = ctypes.CDLL(r"C:\Windows\System32\ucrtbased.dll")
for lib in (rel, dbg):
    for fn in ("exp", "log", "sin", "cos", "sqrt", "exp2", "log2", "ldexp"):
        f = getattr(lib, fn); f.restype = ctypes.c_double
        f.argtypes = (ctypes.c_double, ctypes.c_int) if fn == "ldexp" else (ctypes.c_double,)
rng = random.Random(20261004)
N = 200_000
cases = {
    "log (u in (0,1], Box-Muller)": ("log", lambda: (rng.getrandbits(53) + 1) * 2.0**-53),
    "sin (2 pi u)": ("sin", lambda: 6.283185307179586 * rng.getrandbits(53) * 2.0**-53),
    "cos (2 pi u)": ("cos", lambda: 6.283185307179586 * rng.getrandbits(53) * 2.0**-53),
    "exp (x in [-12, 2], ln-vol / caps)": ("exp", lambda: rng.uniform(-12.0, 2.0)),
    "log (x in [1e-6, 1e13], caps / adv / sigma)": ("log", lambda: 10 ** rng.uniform(-6, 13)),
    "exp2 (x in [-1, 0])": ("exp2", lambda: -rng.random()),
    "sqrt (x in [1e-12, 1])": ("sqrt", lambda: 10 ** rng.uniform(-12, 0)),
}
for label, (fn, gen) in cases.items():
    fr, fd = getattr(rel, fn), getattr(dbg, fn)
    d = 0
    for _ in range(N):
        x = gen()
        d += fr(x) != fd(x)
    print(f"{label:45s} {fn:5s}: {d} of {N} differ")
