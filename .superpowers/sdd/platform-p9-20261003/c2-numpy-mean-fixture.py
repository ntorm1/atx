"""P9 C2 fix round 1 (Ruling C2-R3): the expected values of CalibrateGross.NumpyMeanIsNumpysMeanAtEverySize.

Run: "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/c2-numpy-mean-fixture.py
(numpy 1.26.4). Prints, per series and size, the bit pattern of float(np.ndarray.mean()) of a contiguous float64
array, and checks the C++ model (0.0, then += numpy's pairwise sum of each 8192-value block in order, / n) against
numpy on those and on 400 random series. Series (the gtest forms the same doubles: integer arithmetic below 2^53 and
one correctly rounded division):  ramp[k] = 1.0 + k / 997.0 ;  hash[k] = 0.5 + ((k * 2654435761) % 1000003) / 1000003.0
"""
import struct

import numpy as np

BLOCK, PW_BLOCK = 8192, 128


def pairwise(a, lo, n):
    if n < 8:
        res = 0.0
        for i in range(n):
            res += a[lo + i]
        return res
    if n <= PW_BLOCK:
        r = [a[lo + j] for j in range(8)]
        i = 8
        while i < n - (n % 8):
            for j in range(8):
                r[j] += a[lo + i + j]
            i += 8
        res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]))
        while i < n:
            res += a[lo + i]
            i += 1
        return res
    n2 = n // 2
    n2 -= n2 % 8
    return pairwise(a, lo, n2) + pairwise(a, lo + n2, n - n2)


def model(values):
    acc = 0.0
    for lo in range(0, len(values), BLOCK):
        acc += pairwise(values, lo, min(BLOCK, len(values) - lo))
    return acc / len(values)


def bits(x):
    return "0x%016xULL" % struct.unpack("<Q", struct.pack("<d", x))[0]


SERIES = {"ramp": lambda k: 1.0 + k / 997.0, "hash": lambda k: 0.5 + ((k * 2654435761) % 1000003) / 1000003.0}
SIZES = (5, 129, 1023, 8192, 8193, 9000, 16384, 16385, 20001, 40000)
print("numpy", np.__version__)
for name, f in SERIES.items():
    for n in SIZES:
        v = [f(k) for k in range(n)]
        m = float(np.array(v, dtype=np.float64).mean())
        assert model(v) == m, (name, n)
        print(f"    {{\"{name}\", {n}U, {bits(m)}}},")
rng = np.random.default_rng(11)
bad = 0
for _ in range(400):
    n = int(rng.integers(1, 50000))
    a = rng.uniform(0.2, 1.8, n) * (10.0 ** int(rng.integers(-3, 3)))
    bad += model([float(x) for x in a]) != float(a.mean())
print("random series where the model differs from numpy:", bad, "of 400")
