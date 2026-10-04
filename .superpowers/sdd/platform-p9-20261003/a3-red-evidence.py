"""A3-RED evidence (P9 lane A3): the two M1a-RED gtest expectations against numpy 1.26.4 and the Python builders.

Run from the repository root (no data is read; every input is synthetic and written to a temp dir):

    "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/a3-red-evidence.py

It drives the REAL Python reference functions the C++ ports must reproduce bit for bit:
  * prepare_research_fields.digest_and_quantiles (the manifest's member_finite_quantiles; np.quantile at :832);
  * research_fields_price.volume_mean_rows (vol_126; the ring rule at :610-625), with VOL_WINDOW / VOL_MIN_SESSIONS
    patched to the gtest's window and minimum (module globals read at call time; nothing else is changed).
Their host handles (h.RoleRows, h.FieldWriter, role, budget) are minimal in-memory stubs.
"""
from __future__ import annotations

import inspect
import math
from pathlib import Path
import sys
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "atx-engine" / "tools"))
import prepare_research_fields as builder  # noqa: E402
import research_fields_price as price      # noqa: E402

PROBS = [x for _, x in builder.QUANTILES]


def bits(x: float) -> str:
    return f"{x!r} (signbit={int(math.copysign(1.0, x) < 0)}, hex={float(x).hex()})"


class Budget:
    def admit(self, *a, **k):
        pass

    def check(self, *a, **k):
        pass


class Role:
    def __init__(self, n, n_dates, member=None):
        self.n, self.n_dates = n, n_dates
        self.member = np.ones((n_dates, n), dtype=np.uint8) if member is None else member


# ---------------------------------------------------------------------------------------------------------------------
print("numpy", np.__version__, "| python", sys.version.split()[0])
print()
print("=" * 100)
print("(1) ResearchFieldsWriter.QuantilesPartitionLikeNumpy: the {-0.0} case")
print("=" * 100)
print("numpy 1.26.4 numpy/lib/function_base.py _lerp (np.quantile's default method='linear' uses it):")
print(inspect.getsource(np.lib.function_base._lerp))


def builder_quantiles(cells):
    """The builder's own digest_and_quantiles on a one-row payload whose every cell is a member."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "f.f64"
        path.write_bytes(np.asarray(cells, dtype="<f8").tobytes())
        role = Role(len(cells), 1)
        return builder.digest_and_quantiles(path, role, len(cells), Budget())[1]


for cells in ([-0.0], [-0.0, -0.0]):
    print(f"inputs {cells!r}, probabilities {PROBS}")
    q = builder_quantiles(cells)
    for k, v in q.items():
        print(f"  digest_and_quantiles {k:>6}: {bits(v)}")
    direct = np.quantile(np.array(cells), PROBS)
    print("  np.quantile direct     :", [bits(float(x)) for x in direct])
print()
print("numpy 1.26.4's own index and gamma helpers of method 'linear' (np.lib.function_base._get_indexes, _get_gamma):")
print(inspect.getsource(np.lib.function_base._get_indexes))
linear = np.lib.function_base._QuantileMethods["linear"]
for n in (1, 2):
    virtual = np.asanyarray(linear["get_virtual_index"](n, np.array(PROBS)))
    prev, nxt = np.lib.function_base._get_indexes(np.zeros(n), virtual, n)
    gamma = np.lib.function_base._get_gamma(virtual, prev, linear)
    print(f"n={n}: virtual_index={virtual.tolist()} previous={prev.tolist()} next={nxt.tolist()} "
          f"gamma={gamma.tolist()}")
    print(f"       gamma >= 0.5 (the overwrite branch b - diff*(1-gamma)): {(gamma >= 0.5).tolist()}")
print("With a = b = -0.0, diff = b - a = +0.0. Kept branch a + diff*gamma = -0.0 + +0.0 = +0.0; overwrite branch")
print("b - diff*(1-gamma) = -0.0 - +0.0 = -0.0. n=1: virtual_index 0 >= n-1, so previous = next = -1 and gamma = 0-(-1)")
print("= 1 at every p: all five take the overwrite branch -> -0.0. The base test (1239a5ff")
print("research_fields_writer_test.cpp:139) expected same_bits(x, +0.0) at all five ('_lerp(-0.0, -0.0, gamma >= 1) is")
print("+0.0'): wrong for numpy 1.26.4, whose quantiles the manifest records.")
print()

# ---------------------------------------------------------------------------------------------------------------------
print("=" * 100)
print("(2) ResearchFieldsVolumeMean.SumOrderIsNumpys")
print("=" * 100)
print("vol_126 definition (research_fields_price.FIELDS['vol_126']['definition']):")
print("  " + price.FIELDS["vol_126"]["definition"])
src = inspect.getsource(price.volume_mean_rows).splitlines()
print("research_fields_price.volume_mean_rows, the accept rule and the ring:")
for line in src:
    if any(s in line for s in ("ring", "seen", "ok =", "k =", "row = np.where")):
        print("   ", line.strip())


class Rows:
    def __init__(self, volume, present):
        self.data = {"volume.f64": iter(volume), "present.u8": iter(present)}

    def row(self, name):
        return np.asarray(next(self.data[name]))

    def verify(self):
        return []

    def close(self):
        pass


class Writer:
    def __init__(self, output, name, role):
        self.rows, self.f = [], self

    def write(self, row):
        self.rows.append(np.array(row, dtype=np.float64))

    def close(self):
        pass


class Host:
    RoleRows = Rows
    FieldWriter = Writer


def vol_rows(sessions, window, minimum, rule=None):
    """Every row volume_mean_rows writes for these sessions (all present), window and minimum patched."""
    sessions = [list(map(float, s)) for s in sessions]
    n = len(sessions[0])
    old = price.VOL_WINDOW, price.VOL_MIN_SESSIONS
    price.VOL_WINDOW, price.VOL_MIN_SESSIONS = window, minimum
    try:
        role = Role(n, len(sessions) + 1)
        h = Host()
        h.RoleRows = lambda role_, names: Rows(sessions + [[0.0] * n], [[1] * n] * (len(sessions) + 1))
        out = price.volume_mean_rows(h, role, Path("."), Budget())
        return out["vol_126"][0].rows
    finally:
        price.VOL_WINDOW, price.VOL_MIN_SESSIONS = old


def hypothetical(sessions, window, minimum):
    """The same ring with a rule that ACCEPTS negative volumes (ok = present & isfinite(v), no v >= 0): what the
    base test's expectations encode. Not the spec; shown only to locate the test's premise."""
    sessions = np.asarray(sessions, dtype=np.float64)
    n = sessions.shape[1]
    ring, seen = np.zeros((window, n)), np.zeros((window, n), dtype=bool)
    for t, v in enumerate(sessions):
        ok = np.isfinite(v)
        ring[t % window], seen[t % window] = np.where(ok, v, 0.0), ok
    k = seen.sum(axis=0)
    return np.where(k >= minimum, ring.sum(axis=0) / np.maximum(k, 1), np.nan)


cases = [
    ("base :120  two columns, window 3, after sessions 0..3", [[5, 1], [1e16, 1], [1, 1], [-1e16, 1]], 3, 1,
     "row[0] == 1.0 / 3.0"),
    ("base :133  one column, window 9", [[v] for v in (1, 1e16, -1e16, 1, 0, 0, 0, 0, 0)], 9, 1,
     "one[0] == 0.0"),
    ("base :135  two columns, window 9", [[v, 1] for v in (1, 1e16, -1e16, 1, 0, 0, 0, 0, 0)], 9, 1,
     "row[0] == 1.0 / 9.0"),
    ("new  two columns, window 3, after sessions 0..3", [[5, 1], [1, 1], [1e16, 1], [1, 1]], 3, 1,
     "row == {3333333333333334.0, 1.0}"),
    ("new  one column, window 9", [[v] for v in (1e16, 0, 1, 1, 0, 0, 0, 0, 0)], 9, 1,
     "one[0] == 1111111111111111.4"),
    ("new  two columns, window 9", [[v, 1] for v in (1e16, 0, 1, 1, 0, 0, 0, 0, 0)], 9, 1,
     "row == {1111111111111111.1, 1.0}"),
    ("new  NegativeVolumeIsNotAccepted, window 3", [[4], [-1e16], [2]], 3, 1, "one[0] == 3.0"),
]
for label, sessions, window, minimum, expect in cases:
    rows = vol_rows(sessions, window, minimum)
    got = rows[len(sessions)]   # the row written after the last listed session (TrailingMean::value after the pushes)
    print(f"{label}: test expects {expect}")
    print(f"    volume_mean_rows -> {[bits(float(x)) for x in got]}")
    if label.startswith("base"):
        print(f"    hypothetical rule accepting negatives -> "
              f"{[repr(float(x)) for x in hypothetical(sessions, window, minimum)]}")
