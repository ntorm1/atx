"""Synthetic postimplementation checks for fit_composition_weights (no real data).

Run: python -m unittest discover -s atx-impl/tools -p test_fit_composition_weights.py -v
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_composition_weights as fcw  # noqa: E402

DAY = fcw.DAY_NS
NAN = math.nan


# ------------------------------------------------------------ literal references
def ref_returns(close, raw, present):
    """Literal port of fill_returns/load_logs over the whole panel (interval t = (t-1, t])."""
    d, n = close.shape
    r = np.full((d, n), NAN)
    market = np.full(d, NAN)

    def logs(t, i):
        c, w = float(close[t, i]), float(raw[t, i])
        ok = present[t, i] != 0 and math.isfinite(c) and math.isfinite(w) and c > 0 and w > 0
        return (math.log(c), math.log(w)) if ok else (NAN, NAN)

    for t in range(1, d):
        total, count = 0.0, 0
        for i in range(n):
            pa, pr = logs(t - 1, i)
            ca, cr = logs(t, i)
            x = NAN
            if not math.isnan(pa) and not math.isnan(ca):
                x = float(close[t, i]) / float(close[t - 1, i]) - 1
                la, lr = ca - pa, cr - pr
                if not math.isfinite(x) or abs(la) > 1.5 or abs(la) > abs(lr) + 0.10:
                    x = NAN
            r[t, i] = x
            if not math.isnan(x):
                total += x
                count += 1
        market[t] = total / count if count else NAN
    return r, market


def ref_exposures(r, market, raw, volume, present, d):
    """Literal port of compute_price_exposures at decision d (all instruments)."""
    n = r.shape[1]
    first = d + 1 - 252 if d >= 252 else 1
    intervals = d + 1 - first if d >= first else 0
    beta_rows, vol_rows = min(252, intervals), min(63, intervals)
    out = np.full((n, 3), NAN)
    for i in range(n):
        rows = list(range(d + 1 - beta_rows, d + 1))
        pairs = [(r[t, i], market[t]) for t in rows if not math.isnan(r[t, i]) and not math.isnan(market[t])]
        beta = NAN
        if len(pairs) >= 126:
            mr = sum(p[0] for p in pairs) / len(pairs)
            mm = sum(p[1] for p in pairs) / len(pairs)
            cov = sum((p[0] - mr) * (p[1] - mm) for p in pairs)
            var = sum((p[1] - mm) ** 2 for p in pairs)
            beta = cov / var if var > 0 else NAN
        xs = [r[t, i] for t in range(d + 1 - vol_rows, d + 1) if not math.isnan(r[t, i])]
        vol = NAN
        if len(xs) >= 32:
            m = sum(xs) / len(xs)
            vol = math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))
        ladv = NAN
        if d + 1 >= 63:
            s = 0.0
            for t in range(d + 1 - 63, d + 1):
                w, v = float(raw[t, i]), float(volume[t, i])
                if present[t, i] and math.isfinite(w) and w > 0 and math.isfinite(v) and v >= 0 and math.isfinite(w * v):
                    s += w * v
            mean = s / 63
            ladv = math.log(mean) if mean > 0 else NAN
        out[i] = (beta, vol, ladv)
    return out, np.isfinite(out).all(axis=1)


def ref_centered_ranks(values):
    """Literal port of sort_ranks + each_centered_rank; values is [(value, index)]."""
    v = sorted(values)
    out = {}
    if len(v) < 2:
        return out
    b = 0
    while b < len(v):
        e = b + 1
        while e < len(v) and v[e][0] == v[b][0]:
            e += 1
        rank = (float(b) + float(e - 1)) / (2.0 * float(len(v) - 1)) - 0.5
        for k in range(b, e):
            out[v[k][1]] = rank
        b = e
    return out


def reference_fit(p, signals, signs):
    """Slow independent rule: per-date loops, lstsq residuals, explicit moments."""
    r, market = ref_returns(p["close"], p["raw"], p["present"])
    eff = (p["member"] == 1) & (p["present"] == 1)
    d_count, n = p["close"].shape
    begin, end = p["score_begin"], d_count - 2
    books = {k: [] for k in range(len(signals))}
    factors = {k: [] for k in range(len(signals))}
    for d in range(begin, end):
        e, ok = ref_exposures(r, market, p["raw"], p["volume"], p["present"], d)
        used = [i for i in range(n) if eff[d, i] and ok[i]]
        fwd = np.array([0.0 if math.isnan(r[d + 2, i]) else r[d + 2, i] for i in range(n)])
        x = None
        if len(used) >= 50:
            z = np.empty((len(used), 3))
            for k in range(3):
                col = e[used, k]
                mean = col.sum() / len(col)
                sd = math.sqrt(((col - mean) ** 2).sum() / (len(col) - 1))
                z[:, k] = np.clip((col - mean) / sd, -5.0, 5.0)
            x = np.column_stack([np.ones(len(used)), z])
        for k, (sig, sign) in enumerate(zip(signals, signs)):
            q = np.zeros(n)
            if x is not None:
                ranks = ref_centered_ranks([(float(sig[d, i]), i) for i in used if math.isfinite(sig[d, i])])
                y = np.array([(sign if sign != 0 else 1) * ranks.get(i, 0.0) for i in used])
                entry = np.abs(y).sum()
                coef = np.linalg.lstsq(x, y, rcond=None)[0]
                res = y - x @ coef
                gross = np.abs(res).sum()
                if entry > 0 and gross > 1e-9 * entry:
                    q[used] = res / gross
            books[k].append(q)
            factors[k].append(float(q @ fwd))
    taus = []
    for k in range(len(signals)):
        qs = books[k]
        taus.append(sum(np.abs(qs[t] - qs[t - 1]).sum() for t in range(1, len(qs))) / (len(qs) - 1))
    active = [k for k in range(len(signals)) if signs[k] != 0 and np.std(factors[k]) > 0]
    f = np.array([factors[k] for k in active])
    t = f.shape[1]
    mu = f.sum(axis=1) / t
    dev = f - mu[:, None]
    cov = dev @ dev.T / (t - 1)
    shrunk = 0.1 * cov + 0.9 * np.diag(np.diag(cov))
    w = np.linalg.solve(shrunk, mu)
    w = np.where(w > 0, w, 0.0)
    weights = np.zeros(len(signals))
    weights[active] = w / w.sum()
    return weights, np.array(taus), {k: np.array(factors[k]) for k in range(len(signals))}


# --------------------------------------------------------------- synthetic data
def synthetic_panel(seed=11, dates=232, names=64, score_begin=150, start="2021-03-01"):
    rng = np.random.default_rng(seed)
    mkt = rng.normal(0.0003, 0.01, dates)
    beta = rng.uniform(0.4, 1.6, names)
    ret = beta[None, :] * mkt[:, None] + rng.normal(0, 0.02, (dates, names)) * rng.uniform(0.5, 2, names)
    close = 40.0 * np.exp(np.cumsum(np.log1p(ret), axis=0))
    scale = np.ones((dates, names))
    scale[100:, 3] = 2.0  # a genuine 2-for-1 split: raw moves, adjusted does not -> kept
    raw = close * scale
    close[120, 5] *= 1.6  # uncorroborated adjusted spike -> guard trips twice
    close[130, 6] *= 5.0  # |log adj| > 1.5 even though raw agrees
    raw[130, 6] *= 5.0
    volume = rng.lognormal(12, 1, (dates, names))
    volume[:, 12] = 0.0  # never any dollar volume -> log_adv NaN -> never used
    present = np.ones((dates, names), dtype=np.uint8)
    present[60:71, 7] = 0
    present[20:62, 14] = 0  # too few beta pairs until later decisions
    present[score_begin + 5, 9] = 0  # member but absent at a decision
    present[dates - 20, 8] = 0
    member = present.copy()
    member[:, 10] = 0
    member[:170, 11] = 0
    member[score_begin + 10, 40:] = 0  # 40 members only -> neutralization refused that decision
    member[:63] = 0
    for x in (close, raw, volume):
        x[present == 0] = NAN
    day0 = np.datetime64(start, "D").astype("datetime64[ns]").astype(np.int64)
    sessions = day0 + DAY * np.arange(dates, dtype=np.int64)
    return {"close": close, "raw": raw, "volume": volume, "present": present, "member": member,
            "sessions": sessions, "ids": np.arange(101, 101 + names, dtype=np.uint64),
            "score_begin": score_begin, "rng": rng}


def synthetic_signals(p):
    rng = p["rng"]
    d, n = p["close"].shape
    r, _ = ref_returns(p["close"], p["raw"], p["present"])
    ahead = np.zeros((d, n))
    ahead[:-2] = np.nan_to_num(r[2:], nan=0.0)  # the synthetic alpha peeks at r(d+2)
    noise = lambda s: rng.normal(0, s, (d, n))  # noqa: E731
    ties = np.round(noise(1.0))
    ties[rng.random((d, n)) < 0.1] = NAN
    sparse = np.full((d, n), NAN)
    sparse[:, 0] = 1.0  # one finite name per date -> no ranks -> degenerate
    signals = [0.5 * ahead + noise(0.02), -0.3 * ahead + noise(0.02), ties + 0.05 * ahead,
               noise(1.0), -0.4 * ahead + noise(0.02), sparse]
    member = p["member"] == 1
    return [np.where(member, s, NAN) for s in signals]


IDS = ["alpha_a", "alpha_b", "tie_heavy", "unoriented", "wrong_way", "sparse_one"]
FAMILIES = ["fam_a", "fam_a", "fam_b", "fam_b", "fam_c", "fam_c"]
SIGNS = [1, -1, 1, 0, 1, 1]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Fixture:
    """Role dir + library + orientations + runner-format candidate cache on disk."""

    def __init__(self, root: Path, panel=None, signals=None, signs=SIGNS, sidecar_role="train",
                 end_ns=None):
        self.root = root
        self.p = panel if panel is not None else synthetic_panel()
        self.signals = signals if signals is not None else synthetic_signals(self.p)
        p = self.p
        d, n = p["close"].shape
        role = root / "role"
        role.mkdir(parents=True)
        files = {}
        for name, arr in (("sessions.i64", p["sessions"].astype("<i8")), ("ids.u64", p["ids"].astype("<u8")),
                          ("present.u8", p["present"]), ("member.u8", p["member"]),
                          ("close.f64", p["close"].astype("<f8")), ("raw_close.f64", p["raw"].astype("<f8")),
                          ("volume.f64", p["volume"].astype("<f8"))):
            data = np.ascontiguousarray(arr).tobytes()
            (role / name).write_bytes(data)
            files[name] = {"bytes": len(data), "sha256": sha(data)}
        manifest = {"schema": fcw.ROLE_SCHEMA, "status": "complete", "dates": d, "instruments": n,
                    "score_begin": p["score_begin"], "score_end": d,
                    "score_start_ns": int(p["sessions"][p["score_begin"]]),
                    "score_end_ns": int(end_ns if end_ns is not None else p["sessions"][-1] + DAY),
                    "source_sha256": "ab" * 32, "files": files}
        self.manifest = role / "manifest.json"
        self.manifest.write_bytes(json.dumps(manifest, indent=2).encode())
        self.train_sha = sha(self.manifest.read_bytes())
        library = {"schema": fcw.LIBRARY_SCHEMA, "id": "synthetic",
                   "candidates": [{"id": i, "family": f, "dsl": f"rank(close) * {k}", "horizons": [5, 21, 63],
                                   "sign_policy": "train-rank-ic21"} for k, (i, f) in enumerate(zip(IDS, FAMILIES))]}
        self.library = root / "library.json"
        self.library.write_bytes(json.dumps(library).encode())
        self.library_sha = sha(self.library.read_bytes())
        dsl_sha = [sha(c["dsl"].encode()) for c in library["candidates"]]
        orientations = {"schema": fcw.ORIENTATIONS_SCHEMA, "recipe_sha256": "cd" * 32,
                        "library_sha256": self.library_sha, "train_manifest_sha256": self.train_sha,
                        "candidates": [{"id": i, "family": f, "dsl_sha256": s, "sign": g}
                                       for i, f, s, g in zip(IDS, FAMILIES, dsl_sha, signs)]}
        (root / "train").mkdir()
        self.orientations = root / "train" / "orientations.json"
        self.orientations.write_bytes((json.dumps(orientations, indent=2) + "\n").encode())
        self.orientations_sha = sha(self.orientations.read_bytes())
        self.cache = root / "cache"
        entry = self.cache / self.train_sha
        entry.mkdir(parents=True)
        for cid, s, dsha in zip(IDS, self.signals, dsl_sha):
            data = np.ascontiguousarray(s.astype("<f8")).tobytes()
            (entry / f"{cid}.f64").write_bytes(data)
            sidecar = {"schema": fcw.CACHE_SCHEMA, "candidate_id": cid, "dsl_sha256": dsha,
                       "role_manifest_sha256": self.train_sha, "role": sidecar_role,
                       "eval_mode": fcw.VM_EVAL_MODE, "layout": fcw.CACHE_LAYOUT, "dates": d, "instruments": n,
                       "bytes": len(data), "payload": f"{cid}.f64", "payload_sha256": sha(data)}
            (entry / f"{cid}.json").write_bytes(json.dumps(sidecar, indent=2).encode())
        self.signs = list(signs)

    def args(self, output: Path, **override) -> argparse.Namespace:
        argv = ["--library", str(self.library), "--library-sha256", self.library_sha,
                "--train", str(self.manifest), "--train-sha256", self.train_sha,
                "--orientations", str(self.orientations), "--orientations-sha256", self.orientations_sha,
                "--candidate-cache", str(self.cache), "--output", str(output)]
        args = fcw.parse_args(argv)
        for k, v in override.items():
            setattr(args, k, v)
        return args


def runner_accepts(text: bytes, library_sha: str, ids: list[str]) -> list[float]:
    """Python port of strategy_ic_runner.cpp composition_weights() acceptance rules."""
    assert 0 < len(text) <= 1 << 20

    def pairs(items):
        keys = [k for k, _ in items]
        assert len(keys) == len(set(keys)), "duplicate key"
        return dict(items)

    j = json.loads(text, object_pairs_hook=pairs)
    assert isinstance(j, dict) and j.get("schema") == "atx.dsl-composition-weights/v1"
    assert j.get("library_sha256") == library_sha and isinstance(j.get("weights"), dict)
    assert set(j["weights"]) <= set(ids), "weight for unknown candidate"
    out = []
    for cid in ids:
        w = j["weights"][cid]
        assert isinstance(w, (int, float)) and not isinstance(w, bool) and math.isfinite(w) and w >= 0
        out.append(float(w))
    return out


# ------------------------------------------------------------------------ tests
class HandComputed(unittest.TestCase):
    def test_centered_tied_ranks_hand_case(self):
        values = np.array([[3.0, 1.0, 3.0, 2.0, NAN], [5.0, NAN, NAN, NAN, NAN], [2.0, 2.0, 2.0, NAN, 2.0]])
        got = fcw.centered_tied_ranks(values, np.isfinite(values))
        # C++ each_centered_rank arithmetic: (b + (e - 1)) / (2 (n - 1)) - 0.5 with n = 4
        np.testing.assert_array_equal(got[0], [5 / 6 - 0.5, 0 / 6 - 0.5, 5 / 6 - 0.5, 2 / 6 - 0.5, 0.0])
        np.testing.assert_allclose(got[0], [1 / 3, -1 / 2, 1 / 3, -1 / 6, 0.0], rtol=0, atol=1e-16)
        np.testing.assert_array_equal(got[1], np.zeros(5))  # one valid name: no ranks
        np.testing.assert_array_equal(got[2], np.zeros(5))  # one tie group: all centered at 0

    def test_ranks_match_literal_composition_port_and_are_antisymmetric(self):
        rng = np.random.default_rng(3)
        values = np.round(rng.normal(0, 2, (40, 30)))
        values[rng.random(values.shape) < 0.2] = NAN
        valid = np.isfinite(values) & (rng.random(values.shape) < 0.9)
        got = fcw.centered_tied_ranks(values, valid)
        for t in range(values.shape[0]):
            ref = ref_centered_ranks([(values[t, i], i) for i in range(values.shape[1]) if valid[t, i]])
            np.testing.assert_array_equal(got[t], [ref.get(i, 0.0) for i in range(values.shape[1])])
        np.testing.assert_allclose(fcw.centered_tied_ranks(-values, valid), -got, rtol=0, atol=1e-15)

    def test_mv_shrink_hand_case_with_nonneg_clip(self):
        # S = [[1,.5,0],[.5,1,0],[0,0,4]] -> Sh = [[1,.05,0],[.05,1,0],[0,0,4]]; mu = [.1,-.01,.2]
        # w3 = .2/4 = .05; [w1,w2] = [.1005, -.015]/.9975 -> w2 clipped; normalize over w1 + w3.
        raw = fcw.shrink_solution(np.array([0.1, -0.01, 0.2]),
                                  np.array([[1.0, 0.5, 0.0], [0.5, 1.0, 0.0], [0.0, 0.0, 4.0]]))
        np.testing.assert_allclose(raw, [0.1005 / 0.9975, -0.015 / 0.9975, 0.05], rtol=0, atol=1e-15)
        # Series with exactly that covariance (x 4/3) and mean: orthogonal zero-mean patterns.
        e1, e2, e3 = np.array([1.0, -1, 1, -1]), np.array([1.0, 1, -1, -1]), np.array([1.0, -1, -1, 1])
        factors = np.vstack([0.1 + e1, -0.01 + 0.5 * e1 + math.sqrt(0.75) * e2, 0.2 + 2 * e3])
        weights, solution = fcw.fit_weights(factors)
        np.testing.assert_allclose(weights, [0.1005 / 0.150375, 0.0, 0.049875 / 0.150375], rtol=0, atol=1e-12)
        self.assertEqual(weights[1], 0.0)
        self.assertLess(solution[1], 0)
        self.assertAlmostEqual(float(weights.sum()), 1.0, places=15)

    def test_all_nonpositive_solution_refuses(self):
        with self.assertRaises(fcw.FitError):
            fcw.fit_weights(np.array([[-1.0, -2.0, 0.5, -0.3], [0.2, -0.4, -0.1, -0.5]]))

    def test_turnover_hand_case(self):
        q = np.array([[0.5, -0.5, 0, 0], [0.25, -0.25, 0.25, -0.25], [0.5, -0.5, 0, 0], [0.5, -0.5, 0, 0]])
        # |dq| sums: 1.0, 1.0, 0.0 over three transitions (deployment excluded) -> 2/3
        self.assertAlmostEqual(fcw.standalone_turnover(q), 2 / 3, places=15)
        self.assertEqual(fcw.standalone_turnover(np.tile(q[:1], (5, 1))), 0.0)


class Exposures(unittest.TestCase):
    def test_vectorized_exposures_match_literal_cpp_port(self):
        p = synthetic_panel()
        panel = fcw.PricePanel(p["close"], p["raw"], p["volume"], p["present"])
        r, market = ref_returns(p["close"], p["raw"], p["present"])
        np.testing.assert_array_equal(np.isnan(panel.returns), np.isnan(r))
        np.testing.assert_allclose(panel.returns, r, rtol=0, atol=1e-15, equal_nan=True)
        np.testing.assert_allclose(panel.market, market, rtol=1e-12, atol=1e-17, equal_nan=True)
        self.assertTrue(np.isnan(r[120, 5]) and np.isnan(r[121, 5]))  # uncorroborated spike
        self.assertTrue(np.isnan(r[130, 6]) and np.isnan(r[131, 6]))  # |log adj| > 1.5
        self.assertFalse(np.isnan(r[100, 3]))  # genuine split kept
        cols = np.arange(p["close"].shape[1])
        for d in (0, 1, 5, 40, 62, 63, 100, 127, 150, 165, 229, 231):
            got, ok = panel.exposures(d, cols)
            want, want_ok = ref_exposures(r, market, p["raw"], p["volume"], p["present"], d)
            np.testing.assert_array_equal(ok, want_ok, err_msg=f"d={d}")
            np.testing.assert_allclose(got, want, rtol=1e-11, atol=1e-14, equal_nan=True, err_msg=f"d={d}")
        _, ok = panel.exposures(150, cols)
        self.assertFalse(ok[12] or ok[14])  # zero dollar volume; too few beta pairs
        self.assertTrue(panel.exposures(229, cols)[1][14])  # enough pairs later

    def test_book_is_neutral_gross_one_and_supported_on_used_rows(self):
        p = synthetic_panel()
        signals = synthetic_signals(p)
        with tempfile.TemporaryDirectory() as tmp:
            role = Fixture(Path(tmp), p, signals)
            loaded = fcw.Role(role.manifest, role.train_sha)
        panel = fcw.PricePanel(loaded.close, loaded.raw_close, loaded.volume, loaded.present)
        ctx = fcw.Context(loaded, panel)
        self.assertEqual([x["decision_index"] for x in ctx.refused], [p["score_begin"] + 10])
        self.assertEqual(ctx.refused[0]["reason"], "too-few-usable-names")
        q, flat = ctx.book(signals[0], 1)
        qn, _ = ctx.book(signals[0], -1)
        np.testing.assert_array_equal(qn, -q)
        self.assertEqual(flat, 1)
        member = (p["member"] == 1) & (p["present"] == 1)
        for t in range(q.shape[0]):
            d = ctx.begin + t
            if not ctx.used[t].any():
                self.assertFalse(q[t].any())
                continue
            self.assertAlmostEqual(float(np.abs(q[t]).sum()), 1.0, places=12)
            self.assertFalse(q[t][~ctx.used[t]].any())
            cols = ctx.columns[ctx.used[t]]
            self.assertTrue(member[d, cols].all())
            e, ok = panel.exposures(d, cols)
            z = (e - e.mean(0)) / e.std(0, ddof=1)
            x = np.column_stack([np.ones(len(cols)), np.clip(z, -5, 5)])
            np.testing.assert_allclose(x.T @ q[t][ctx.used[t]], 0.0, atol=1e-13)


class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.fx = Fixture(cls.root / "fx")
        cls.out = cls.root / "out" / "weights.json"
        cls.summary = fcw.fit(cls.fx.args(cls.out))
        cls.text = cls.out.read_bytes()
        cls.doc = json.loads(cls.text)
        cls.ref_weights, cls.ref_taus, cls.ref_factors = reference_fit(cls.fx.p, cls.fx.signals, SIGNS)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def rows(self):
        return {row["id"]: row for row in self.doc["provenance"]["candidates"]}

    def test_weights_match_independent_reference(self):
        got = np.array([self.doc["weights"][i] for i in IDS])
        np.testing.assert_allclose(got, self.ref_weights, rtol=1e-9, atol=1e-12)
        self.assertAlmostEqual(float(got.sum()), 1.0, places=12)
        self.assertGreater(got[0], 0)
        self.assertGreater(got[1], 0)  # sign -1 candidate earns weight once oriented

    def test_sign_clip_and_degenerate_handling(self):
        rows = self.rows()
        self.assertEqual(rows["unoriented"]["weight"], 0.0)
        self.assertEqual(rows["unoriented"]["status"], "unoriented-sign-0")
        self.assertIsNone(rows["unoriented"]["factor_mean"])
        self.assertIsNone(rows["unoriented"]["mv_solution"])
        self.assertEqual(rows["wrong_way"]["weight"], 0.0)  # negative MV solution clipped
        self.assertTrue(rows["wrong_way"]["clipped"])
        self.assertLess(rows["wrong_way"]["mv_solution"], 0)
        self.assertLess(rows["wrong_way"]["factor_mean"], 0)
        self.assertEqual(rows["sparse_one"]["status"], "degenerate-zero-variance")
        self.assertEqual(rows["sparse_one"]["weight"], 0.0)
        self.assertEqual(rows["sparse_one"]["tau"], 0.0)
        oriented = rows["alpha_b"]
        self.assertEqual(oriented["sign"], -1)
        self.assertGreater(oriented["factor_mean"], 0)
        np.testing.assert_allclose(oriented["factor_mean"], self.ref_factors[1].mean(), rtol=1e-9)
        sharpe = self.ref_factors[0].mean() / self.ref_factors[0].std(ddof=1) * math.sqrt(252)
        np.testing.assert_allclose(rows["alpha_a"]["factor_sharpe_annualized"], sharpe, rtol=1e-9)

    def test_turnover_matches_reference_and_blend(self):
        rows = self.rows()
        taus = np.array([rows[i]["tau"] for i in IDS])
        np.testing.assert_allclose(taus, self.ref_taus, rtol=1e-9, atol=1e-12)
        self.assertGreater(rows["unoriented"]["tau"], 0)  # sign 0 still reports tau
        prov = self.doc["provenance"]
        weighted = sum(self.doc["weights"][i] * rows[i]["tau"] for i in IDS)
        self.assertAlmostEqual(prov["weighted_standalone_turnover"], weighted, places=14)
        self.assertEqual(prov["tau_flagged"], [i for i in IDS if rows[i]["tau"] > 0.70])
        self.assertTrue(all(rows[i]["tau_over_limit"] == (rows[i]["tau"] > 0.70) for i in IDS))
        # the noise candidates redraw every day: far above the 0.70 flag
        self.assertIn("unoriented", prov["tau_flagged"])

    def test_schema_provenance_and_runner_acceptance(self):
        got = runner_accepts(self.text, self.fx.library_sha, IDS)
        self.assertEqual(len(got), len(IDS))
        self.assertEqual(sorted(self.doc["weights"]), sorted(IDS))
        prov = self.doc["provenance"]
        self.assertEqual(prov["rule"], "mv-shrink-0.9-nonneg-v1")
        self.assertEqual(prov["lambda"], 0.9)
        self.assertEqual(prov["role_manifest_sha256"], self.fx.train_sha)
        self.assertEqual(prov["orientations_sha256"], self.fx.orientations_sha)
        self.assertEqual(prov["script_sha256"], sha(Path(fcw.__file__).read_bytes()))
        begin = self.fx.p["score_begin"]
        self.assertEqual(prov["window"]["decision_begin"], begin)
        self.assertEqual(prov["window"]["decision_end_exclusive"], self.fx.p["close"].shape[0] - 2)
        self.assertEqual(len(prov["neutralization_refused_decisions"]), 1)
        rows = self.rows()
        for i, s in zip(IDS, self.fx.signals):
            self.assertEqual(rows[i]["cache_payload_sha256"], sha(np.ascontiguousarray(s.astype("<f8")).tobytes()))
        self.assertEqual(self.summary["sha256"], sha(self.text))
        self.assertEqual(self.text, fcw.canonical_bytes(self.doc))  # canonical bytes

    def test_deterministic_bytes_and_exclusive_output(self):
        again = self.root / "out" / "again.json"
        fcw.fit(self.fx.args(again))
        self.assertEqual(again.read_bytes(), self.text)
        with self.assertRaises(fcw.FitError):
            fcw.fit(self.fx.args(self.out))
        self.assertEqual(self.out.read_bytes(), self.text)
        with self.assertRaises(fcw.FitError):
            fcw.publish_exclusive(self.out, b"{}")
        self.assertEqual(self.out.read_bytes(), self.text)
        self.assertFalse(any(x.name.endswith(".pending") for x in self.out.parent.iterdir()))


class Refusals(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.p = synthetic_panel(dates=200, score_begin=150)
        self.signals = synthetic_signals(self.p)

    def tearDown(self):
        self.tmp.cleanup()

    def refuse(self, fx, fragment, **override):
        out = self.root / "refused.json"
        with self.assertRaises(fcw.FitError) as caught:
            fcw.fit(fx.args(out, **override))
        self.assertIn(fragment, str(caught.exception))
        self.assertFalse(out.exists())

    def test_post_train_role_refused(self):
        p = synthetic_panel(dates=200, score_begin=150, start="2022-08-01")  # runs into 2023
        fx = Fixture(self.root / "late", p, synthetic_signals(p))
        self.refuse(fx, "TRAIN-only")

    def test_non_train_cache_entry_refused(self):
        fx = Fixture(self.root / "val", self.p, self.signals, sidecar_role="validation")
        self.refuse(fx, "not a TRAIN-role signal")

    def test_pins_and_tampering_refused(self):
        fx = Fixture(self.root / "pins", self.p, self.signals)
        self.refuse(fx, "pin differs", library_sha256="0" * 64)
        self.refuse(fx, "pin differs", orientations_sha256="1" * 64)
        payload = fx.cache / fx.train_sha / "alpha_a.f64"
        data = bytearray(payload.read_bytes())
        data[8] ^= 1
        payload.write_bytes(bytes(data))
        self.refuse(fx, "payload SHA-256 mismatch")
        payload.unlink()
        (fx.cache / fx.train_sha / "alpha_a.json").unlink()
        self.refuse(fx, "missing entry alpha_a")

    def test_role_payload_tampering_refused(self):
        fx = Fixture(self.root / "role", self.p, self.signals)
        close = fx.manifest.parent / "close.f64"
        data = bytearray(close.read_bytes())
        data[16] ^= 1
        close.write_bytes(bytes(data))
        self.refuse(fx, "payload SHA close.f64")


if __name__ == "__main__":
    unittest.main()
