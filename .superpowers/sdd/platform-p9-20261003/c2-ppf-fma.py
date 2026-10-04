"""Emulate engine::eval::norm_ppf two ways: separate multiply/add (runtime SSE2) and fused
multiply-add for each contractible a*b+c (LLVM's constant fold of llvm.fmuladd)."""
import math
import sys
from fractions import Fraction

A = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
     1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00]
B = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
     6.680131188771972e+01, -1.328068155288572e+01]
C = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
     -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00]
D = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
     3.754408661907416e+00]
PLOW = 0.02425
PHIGH = 1.0 - PLOW
SQRT1_2 = 0.70710678118654752440
SQRT2PI = 2.5066282746310002


def fma(a, b, c):
    return float(Fraction(a) * Fraction(b) + Fraction(c))


def madd(fused):
    if fused:
        return fma
    return lambda a, b, c: a * b + c


def ppf(p, fused):
    m = madd(fused)
    if p < PLOW:
        q = math.sqrt(-2.0 * math.log(p))
        num = m(m(m(m(m(C[0], q, C[1]), q, C[2]), q, C[3]), q, C[4]), q, C[5])
        den = m(m(m(m(D[0], q, D[1]), q, D[2]), q, D[3]), q, 1.0)
        x = num / den
    elif p <= PHIGH:
        q = p - 0.5
        r = q * q
        num = m(m(m(m(m(A[0], r, A[1]), r, A[2]), r, A[3]), r, A[4]), r, A[5]) * q
        den = m(m(m(m(m(B[0], r, B[1]), r, B[2]), r, B[3]), r, B[4]), r, 1.0)
        x = num / den
    else:
        q = math.sqrt(-2.0 * math.log(1.0 - p))
        num = m(m(m(m(m(C[0], q, C[1]), q, C[2]), q, C[3]), q, C[4]), q, C[5])
        den = m(m(m(m(D[0], q, D[1]), q, D[2]), q, D[3]), q, 1.0)
        x = -num / den
    e = 0.5 * math.erfc(-x * SQRT1_2) - p
    u = e * SQRT2PI * math.exp(x * x / 2.0)
    return x - u / (1.0 + x * u / 2.0), x


for label, p in (("2/12", 2.0 / 12.0), ("5/12", 5.0 / 12.0), ("8/12", 8.0 / 12.0),
                 ("10/12", 10.0 / 12.0), ("1001/1002", 1001.0 / 1002.0), ("1/4", 0.25),
                 ("blom", (1001 - 0.375) / (1001 + 0.25)), ("over_n", (1001 - 0.5) / 1001)):
    plain, x0 = ppf(p, False)
    fused, x1 = ppf(p, True)
    print(f"{label:>10} p={p!r}: separate {plain:.17g} fused {fused:.17g} "
          f"{'SAME' if plain == fused else 'DIFF'} (start {x0:.17g} vs {x1:.17g})")
print(sys.version)
