"""v8 D-2 (contract K2): the goldens of Exposures.FitterFactorsEqualTo1e12 are the fitter's own output.

The C++ test (atx-impl/tests/strategy_exposures_verb_test.cpp) builds the fixture below operation
for operation, exports it with the `exposures` machinery and compares the fitter's factor series,
computed from the export, with the arrays it embeds. This test recomputes those arrays with
fit_composition_weights.py (PricePanel, neutralization_basis, Context.build, book,
factor_returns) and requires the embedded ones to equal them, so the C++ goldens cannot drift
from the fitter. It also checks that the C++ Householder convention reproduces numpy's Q, and,
when ATX_EQUITY_TARGETS_EXE names a built atx-equity-strategy-targets, runs the verb on a
synthetic engine role and feeds the export to the fitter's Context.
"""

import hashlib
import json
import math
import os
import re
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fit_composition_weights as fcw  # noqa: E402

CPP_TEST = HERE.parent / "tests" / "strategy_exposures_verb_test.cpp"
MASK = (1 << 64) - 1
D, N, BEGIN, REFUSED_SESSION = 170, 60, 130, 140
ROLE_DATES, SCORE_BEGIN, DAY_NS = 420, 383, 86_400_000_000_000


class Lcg:
    def __init__(self, seed):
        self.state = seed

    def next(self):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) & MASK
        return float(self.state >> 11) * 2.0 ** -53


def fixture(dates=D, member_from=0):
    """strategy_exposures_verb_test.cpp fitter_fixture(), operation for operation."""
    close = np.zeros((dates, N))
    rng = Lcg(2026)
    for i in range(N):
        close[0, i] = 20.0 + i
    for t in range(1, dates):
        common = 0.03 * (rng.next() - 0.5)
        for i in range(N):
            loading = 0.5 + 0.02 * i
            idio = 0.005 + 0.0002 * ((i * 7) % N)
            close[t, i] = close[t - 1, i] * (1 + loading * common + idio * (rng.next() - 0.5))
    raw = close.copy()
    for t in range(150, dates):
        close[t, 5] = close[t, 5] * 2.0
    volume = np.zeros((dates, N))
    for t in range(dates):
        for i in range(N):
            volume[t, i] = 1e4 * (1 + (i * 11) % N) * (0.5 + rng.next())
    present = np.ones((dates, N), dtype=np.uint8)
    member = np.zeros((dates, N), dtype=np.uint8)
    for t in range(dates):
        for i in range(N):
            if (t * 7 + i * 3) % 97 == 0 or (t == REFUSED_SESSION and i < 15):
                present[t, i] = 0
            member[t, i] = 1 if present[t, i] and (t + 2 * i) % 19 != 0 and t >= member_from else 0
    for a in (close, raw, volume):
        a[present == 0] = np.nan
    s1, s2 = np.zeros((dates, N)), np.zeros((dates, N))
    g = Lcg(77)
    for t in range(dates):
        for i in range(N):
            s1[t, i] = g.next() - 0.5
            s2[t, i] = (0.5 + 0.02 * i) + 0.2 * (g.next() - 0.5)
            if (t * 3 + i) % 23 == 0:
                s1[t, i] = np.nan
    return {"close": close, "raw_close": raw, "volume": volume, "present": present, "member": member,
            "s1": s1, "s2": s2}


def fitter_context(f, begin, end):
    payload = {k: f[k] for k in ("close", "raw_close", "volume", "present", "member")}
    role = SimpleNamespace(payload=lambda: payload, begin=begin, end=end, instruments=N, sha="f" * 64)
    return fcw.Context.build(role)


def cpp_array(name):
    text = CPP_TEST.read_text()
    body = re.search(r"%s\{(.*?)\};" % name, text, re.S).group(1)
    return [math.nan if v.strip() == "missing" else float(v) for v in body.split(",") if v.strip()]


def cpp_basis_rows():
    text = CPP_TEST.read_text()
    body = re.search(r"kFitterBasis\{\{(.*?)\}\};", text, re.S).group(1)
    rows = []
    for m in re.finditer(r"\{(\d+),\s*(\d+),\s*\{([^}]*)\}\}", body):
        rows.append((int(m.group(1)), int(m.group(2)), [float(v) for v in m.group(3).split(",")]))
    return rows


def householder_q(design):
    """strategy_price_exposures.cpp householder_q (dgeqr2 + dorg2r), column-major."""
    m = design.shape[0]
    a = [list(design[:, j]) for j in range(4)]
    tau = [0.0] * 4
    for j in range(4):
        x = a[j]
        norm = math.sqrt(sum(x[i] * x[i] for i in range(j + 1, m)))
        if norm == 0:
            continue
        alpha = x[j]
        beta = -math.copysign(math.hypot(alpha, norm), alpha)
        tau[j] = (beta - alpha) / beta
        scale = 1 / (alpha - beta)
        for i in range(j + 1, m):
            x[i] *= scale
        x[j] = beta
        for c in range(j + 1, 4):
            y = a[c]
            w = y[j] + sum(x[i] * y[i] for i in range(j + 1, m))
            w *= tau[j]
            y[j] -= w
            for i in range(j + 1, m):
                y[i] -= w * x[i]
    for j in range(3, -1, -1):
        v = a[j]
        if j + 1 < 4:
            v[j] = 1.0
            for c in range(j + 1, 4):
                y = a[c]
                w = sum(v[i] * y[i] for i in range(j, m)) * tau[j]
                for i in range(j, m):
                    y[i] -= w * v[i]
        for i in range(j + 1, m):
            v[i] *= -tau[j]
        v[j] = 1 - tau[j]
        for i in range(j):
            v[i] = 0.0
    return np.column_stack([np.array(c) for c in a])


def test_cpp_goldens_are_the_fitters_output():
    f = fixture()
    ctx = fitter_context(f, BEGIN, D - 2)
    assert ctx.refused == [{"decision_index": REFUSED_SESSION, "reason": "too-few-usable-names", "used_rows": 42}]
    assert [int(x) for x in ctx.used_rows] == [int(x) for x in cpp_array("kFitterUsedRows")]
    for name, golden in (("s1", cpp_array("kFitterS1")), ("s2", cpp_array("kFitterS2"))):
        q, live = ctx.book(f[name], 1)
        values = ctx.factor_returns(q)
        assert len(golden) == len(values)
        for x, ok, g in zip(values, live, golden):
            assert ok == (not math.isnan(g))
            if ok:
                assert abs(float(x) - g) <= 1e-15
    for offset, name, q in cpp_basis_rows():
        p = int(np.flatnonzero(ctx.columns == name)[0])
        assert np.max(np.abs(ctx.basis[offset][:, p] - np.array(q))) <= 1e-15


def test_householder_convention_is_numpys():
    f = fixture()
    panel = fcw.PricePanel(f["close"], f["raw_close"], f["volume"], f["present"])
    used = f["member"].astype(bool) & f["present"].astype(bool)
    checked = 0
    for d in range(BEGIN, D - 2):
        cols = np.flatnonzero(used[d])
        exposures, ok = panel.exposures(d, cols)
        basis, _ = fcw.neutralization_basis(exposures[ok])
        if basis is None:
            continue
        x = exposures[ok]
        m = x.shape[0]
        z = np.column_stack([np.clip((x[:, k] - x[:, k].sum() / m) /
                                     math.sqrt(float(((x[:, k] - x[:, k].sum() / m) ** 2).sum()) / (m - 1)),
                                     -fcw.CLIP_Z, fcw.CLIP_Z) for k in range(3)])
        q = householder_q(np.column_stack([np.ones(m), z]))
        assert np.max(np.abs(q - basis)) <= 1e-13
        checked += 1
    assert checked == D - 2 - BEGIN - 1


def _write_role(root: Path, f) -> tuple[Path, str]:
    """strategy_exposures_verb_test.cpp write_role(): an engine-conformant research role."""
    root.mkdir()
    sessions = np.array([(17683 + t) * DAY_NS for t in range(ROLE_DATES)], dtype="<i8")
    ids = np.array([1000 + 7 * i for i in range(N)], dtype="<u8")
    files = {}
    for name, data in (("sessions.i64", sessions), ("ids.u64", ids),
                       ("close.f64", f["close"].astype("<f8")), ("raw_close.f64", f["raw_close"].astype("<f8")),
                       ("volume.f64", f["volume"].astype("<f8")), ("present.u8", f["present"].astype("u1")),
                       ("member.u8", f["member"].astype("u1"))):
        raw = np.ascontiguousarray(data).tobytes()
        (root / name).write_bytes(raw)
        files[name] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    membership = {"rule": "research-prior63-usd-adv-topn-v1", "top_n": N, "lookback_sessions": 63,
                  "lag_sessions": 1, "min_raw_price_exclusive": 5, "min_adv_exclusive": 5000000,
                  "ties": "securityID-ascending", "missing": "complete-prior-calendar-window-required",
                  "common_stock_verified": False}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete",
                "instrument_namespace": "spiderrock.securityID", "dates": ROLE_DATES, "instruments": N,
                "score_begin": SCORE_BEGIN, "score_end": ROLE_DATES,
                "score_start_ns": int(sessions[SCORE_BEGIN]), "score_end_ns": int(sessions[-1]) + DAY_NS,
                "source_sha256": "a" * 64, "membership_recipe": json.dumps(membership, separators=(",", ":")),
                "clock_recipe": "modeled-session+22h-mark+23h-decision-v1",
                "close_basis": "f64(raw-f32-close)*f64-cumulReturnFactor", "volume_basis": "raw-share-volume",
                "common_stock_verified": False, "historical_vintage_verified": False,
                "declared_output_bytes": ROLE_DATES * N * 26 + ROLE_DATES * 8 + N * 8, "files": files}
    text = (json.dumps(manifest, indent=2) + "\n").encode()
    (root / "manifest.json").write_bytes(text)
    return root / "manifest.json", hashlib.sha256(text).hexdigest()


def _context_from_export(directory: Path, begin: int, end: int) -> "fcw.Context":
    """The fitter's Context from the K2 files (what the fitter follow-up reads)."""
    manifest = json.loads((directory / "manifest.json").read_text())
    t = manifest["decisions"]
    assert (manifest["decision_begin"], manifest["decision_end_exclusive"]) == (begin, end)
    basis = np.frombuffer((directory / "basis.f64").read_bytes(), dtype="<f8").reshape(t, N, 4)
    forward = np.frombuffer((directory / "forward_returns.f64").read_bytes(), dtype="<f8").reshape(t, N)
    used = basis[:, :, 0] != 0
    columns = np.flatnonzero(used.any(axis=0)).astype(np.int64)
    arrays = {"columns": columns, "used": used[:, columns].astype(np.uint8),
              "basis": np.ascontiguousarray(basis[:, columns, :].transpose(0, 2, 1)),
              "forward": np.where(np.isnan(forward[:, columns]), 0.0, forward[:, columns]),
              "used_rows": np.array(manifest["used_rows"], dtype=np.int64)}
    return fcw.Context("f" * 64, begin, end, arrays, manifest["refused"])


@pytest.mark.skipif(not os.environ.get("ATX_EQUITY_TARGETS_EXE"), reason="needs a built atx-equity-strategy-targets")
def test_verb_export_reproduces_the_fitters_factors(tmp_path):
    f = fixture(ROLE_DATES, 63)
    manifest, sha = _write_role(tmp_path / "role", f)
    out = tmp_path / "export"
    subprocess.run([os.environ["ATX_EQUITY_TARGETS_EXE"], "exposures", "--role", str(manifest),
                    "--role-sha256", sha, "--output", str(out)], check=True)
    begin, end = SCORE_BEGIN, ROLE_DATES - 2
    own = fitter_context(f, begin, end)
    exported = _context_from_export(out, begin, end)
    assert exported.refused == own.refused
    assert list(exported.used_rows) == list(own.used_rows)
    for name in ("s1", "s2"):
        q_own, live_own = own.book(f[name], 1)
        q_exp, live_exp = exported.book(f[name], 1)
        assert np.array_equal(live_own, live_exp)
        a, b = own.factor_returns(q_own), exported.factor_returns(q_exp)
        assert np.max(np.abs(a[live_own] - b[live_own])) <= 1e-12
