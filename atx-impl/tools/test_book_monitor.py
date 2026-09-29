"""Synthetic known-answer checks for book_monitor (no real data).

Run: python -m pytest atx-impl/tools/test_book_monitor.py -q
"""
import contextlib
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import book_monitor as bm  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402

DAY = fcw.DAY_NS
T0 = 1_577_836_800_000_000_000  # 2020-01-01
ROLE = "3e" * 32


def standardized(n: int, seed: int) -> np.ndarray:
    """Mean exactly 0 and sample SD exactly 1."""
    u = np.random.default_rng(seed).standard_normal(n)
    u -= u.mean()
    return u / u.std(ddof=1)


def sessions(n: int, start: int = 0) -> list[int]:
    return [T0 + (start + j) * DAY for j in range(n)]


def write_csv(path: Path, header: list[str], rows: list[list]) -> Path:
    path.write_text("\n".join([",".join(header)] + [",".join(str(x) for x in r) for r in rows]) + "\n")
    return path


class World:
    """TRAIN inputs of n sessions and live inputs of m more sessions, each a separate file set."""

    MEMBERS = ["a", "b", "c"]

    def __init__(self, root: Path, n=500, m=60):
        self.root, self.n, self.m = root, n, m
        root.mkdir(parents=True, exist_ok=True)
        adm = {"schema": fcw.ADMISSION_SCHEMA, "inputs": {"train_manifest_sha256": ROLE}, "admitted": self.MEMBERS,
               "candidates": [{"id": i, "s_k": 1, "cache_payload_sha256": f"{k + 1:02x}" * 32}
                              for k, i in enumerate(self.MEMBERS + ["rejected"])]}
        self.admission = root / "admission.json"
        self.admission.write_text(json.dumps(adm))

    # -------- M2 inputs
    def daily_ic(self, name: str, live_ic: float | None = None) -> Path:
        rows = []
        for k, cid in enumerate(self.MEMBERS):
            w = standardized(self.n // 5, 10 + k)
            vals = np.empty(self.n)
            vals[::5] = 0.03 + 0.1 * w           # weekly observations: mean .03, sd .1 exactly
            other = np.arange(self.n) % 5 != 0
            vals[other] = 0.03 + 0.05 * np.sin(np.arange(self.n))[other]
            ses = sessions(self.n)
            if live_ic is not None:
                vals = np.concatenate([vals, np.full(self.m, live_ic if cid == "a" else 0.03)])
                ses = sessions(self.n + self.m)
            rows += [[cid, 5, 100 + j, s, "", repr(float(v)), repr(float(v))] for j, (s, v) in enumerate(zip(ses, vals))]
            rows += [[cid, 21, 100 + j, s, "", "0.5", "0.5"] for j, s in enumerate(ses)]  # other horizons ignored
        return write_csv(self.root / name, ["id", "horizon", "decision_index", "session_ns", "pearson", "rank_ic",
                                            "oriented_rank_ic"], rows)

    def sleeve(self, name: str, live_turn: float | None = None) -> Path:
        rows = []
        for cid in self.MEMBERS:
            t = 0.1 + 0.01 * np.cos(np.arange(self.n))
            ses = sessions(self.n)
            if live_turn is not None:
                t = np.concatenate([t, np.full(self.m, live_turn if cid == "b" else 0.1)])
                ses = sessions(self.n + self.m)
            rows += [[cid, 100 + j, s, repr(float(v)), "0.0", "1.0"] for j, (s, v) in enumerate(zip(ses, t))]
        return write_csv(self.root / name, ["id", "decision_index", "session_ns", "turnover", "pnl", "coverage"], rows)

    # -------- M1 / M3 inputs
    def bias(self, name: str, scale_last: float | None = None, book=True) -> Path:
        n = self.n + (self.m if scale_last is not None else 0)
        rng = np.random.default_rng(3)
        z = rng.standard_normal(n) * np.where(rng.random(n) < 0.1, 3.0, 1.0)  # fat tails: kurtosis ~ 8
        z = (z - z.mean()) / z.std(ddof=1)
        if scale_last is not None:
            z[-63:] = scale_last * standardized(63, 4)
        rows = [["random", "random-000", s, "0.01", "0.0", "0.5", "nan", "nan"] for s in sessions(n)]
        if book:
            rows += [["book", "book", s, "0.01", repr(float(0.01 * v)), repr(float(v)), "nan", "nan"]
                     for s, v in zip(sessions(n), z)]
        return write_csv(self.root / name, ["family", "name", "session", "forecast_vol", "realized_return", "z", "b63",
                                            "b252"], rows)

    def holdings_days(self, name: str, live_bps: float | None = None, live_fill: float = 0.99) -> Path:
        n = self.n + (self.m if live_bps is not None else 0)
        rows, nav = [], 1e9
        for j, s in enumerate(sessions(n)):
            live = live_bps is not None and j >= self.n
            traded = 5e7 * (1 + 0.1 * math.sin(j))
            bps = live_bps if live else 10.0 + 0.5 * math.sin(j)
            fill = live_fill if live else 0.99 + 0.005 * math.sin(0.7 * j)
            pre = nav * (1 + 0.001 * math.sin(3 * j))
            post = pre - bps * 1e-4 * traded
            unfilled = traded * (1 - fill) / fill
            rows.append([s, 1, 1, 1 if j else 0, repr(pre), repr(post), 1800, 1800, 5e8, 5e8, 1.0, 0.0,
                         repr(traded if j else 0.0), 1600, 10, 0, 0, repr(unfilled if j else 0.0), 0.05, 1.0, 0.0,
                         1600, 0, 0, "applied", 0, 0])
            nav = post
        header = ("session_ns,decision,rebalance,executed,pretrade_nav,posttrade_nav,rows,held_names,long_dollars,"
                  "short_dollars,gross_leverage,net_leverage,traded_dollars,fills,capped_fills,blocked_absent,"
                  "blocked_liquidity,unfilled_dollars,planned_turnover,planned_gross,planned_net,orders_placed,"
                  "blocked_short_names,blocked_short_dollars,neutralize,neutralize_skipped,locate_zeroed").split(",")
        return write_csv(self.root / name, header, rows)

    # -------- M4 input
    def fit_work(self, name: str, common: float = 0.0, live: bool = False) -> Path:
        work = self.root / name
        fdir = work / ROLE / "sometag" / "factors"
        fdir.mkdir(parents=True)
        rng = np.random.default_rng(5)
        n = self.n + (self.m if live else 0)
        f = rng.standard_normal(n)
        a = rng.standard_normal((63, len(self.MEMBERS)))
        calm = np.linalg.qr(a - a.mean(axis=0))[0]
        for k, cid in enumerate(self.MEMBERS):
            x = rng.standard_normal(n)
            if live and common:
                x[-63:] = common * f[-63:] + (1 - common) * x[-63:]
            elif live:  # exactly uncorrelated window: orthonormal centered columns
                x[-63:] = calm[:, k]
            body = {"schema": fcw.FACTOR_SCHEMA, "role_manifest_sha256": ROLE, "candidate_id": cid,
                    "cache_payload_sha256": f"{k + 1:02x}" * 32, "f_unsigned": [float(v) for v in x]}
            (fdir / f"k-{k:064x}.json").write_bytes(fcw.canonical_compact(fcw.seal(body)))
        return work

    def run(self, argv) -> tuple[int, dict | None, str]:
        err, out = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bm.main([str(a) for a in argv])
        doc = None
        for a, b in zip(argv, argv[1:]):
            if a == "--output" and (Path(b) / "monitor.json").is_file():
                doc = json.loads((Path(b) / "monitor.json").read_bytes())
        return code, doc, err.getvalue()


class Primitives(unittest.TestCase):
    def test_siegmund_arl_and_cusum_path(self):
        self.assertAlmostEqual(bm.siegmund_arl(0.0), 938.2, delta=0.5)   # h 5, k .5: ARL0 ~ 938 observations
        self.assertAlmostEqual(bm.siegmund_arl(1.0), 10.34, delta=0.05)  # a 1-sigma shift: ~10 observations
        np.testing.assert_allclose(bm.cusum(np.array([-1.0, -1.0, 2.0, -1.0]), "low"), [0.5, 1.0, 0.0, 0.5])
        np.testing.assert_allclose(bm.cusum(np.array([1.0, 0.2, 3.0]), "high"), [0.5, 0.2, 2.7])
        self.assertEqual([bm.cusum_status(x) for x in (0.0, 2.5, 4.99, 5.0)], ["ok", "warn", "warn", "alarm"])

    def test_rolling_and_pairwise(self):
        np.testing.assert_allclose(bm.rolling_mean(np.array([1.0, 2.0, np.nan, 4.0]), 2), [np.nan, 1.5, 2.0, 4.0])
        x = np.column_stack([np.arange(20.0), np.arange(20.0), -np.arange(20.0)])
        self.assertAlmostEqual(bm.mean_pairwise_corr(x), (1 - 1 - 1) / 3, places=12)


class BaselineAndLive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        w = cls.w = World(cls.root / "in")
        cls.train_inputs = ["--daily-ic", w.daily_ic("ic.csv"), "--admission", w.admission,
                            "--sleeve-daily", w.sleeve("sleeve.csv"), "--bias", w.bias("bias.csv"),
                            "--holdings-days", w.holdings_days("hd.csv"), "--fit-work", w.fit_work("work")]
        cls.code, cls.base, cls.err = w.run(["--baseline", *cls.train_inputs, "--output", cls.root / "base"])
        cls.ref = cls.root / "base" / "monitor.json"

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def live(self, name, **kw):
        w = self.w
        argv = ["--reference", self.ref, "--admission", w.admission,
                "--daily-ic", w.daily_ic(f"{name}-ic.csv", kw.get("ic", 0.03)),
                "--sleeve-daily", w.sleeve(f"{name}-sl.csv", kw.get("turn", 0.1)),
                "--bias", w.bias(f"{name}-bias.csv", kw.get("bias", 1.0)),
                "--holdings-days", w.holdings_days(f"{name}-hd.csv", kw.get("bps", 10.0), kw.get("fill", 0.99)),
                "--fit-work", w.fit_work(f"{name}-work", kw.get("common", 0.0), live=True),
                "--output", self.root / name]
        code, doc, err = w.run(argv)
        self.assertEqual(code, 0, err)
        return doc

    def test_baseline_reference_distributions(self):
        self.assertEqual(self.code, 0, self.err)
        b = self.base
        self.assertEqual(b["mode"], "baseline")
        self.assertTrue(b["in_sample"])
        ref = b["reference"]
        self.assertEqual(ref["last_session_ns"], sessions(self.w.n)[-1])
        a = ref["M2"]["members"]["a"]
        self.assertAlmostEqual(a["ic_weekly_mean"], 0.03, places=12)
        self.assertAlmostEqual(a["ic_weekly_sd"], 0.1, places=12)
        self.assertAlmostEqual(ref["M3"]["model_cost_per_dollar"], 1e-3, delta=2e-5)
        self.assertLess(ref["M3"]["ratio_p5"], 1.0)
        self.assertGreater(ref["M3"]["ratio_p95"], 1.0)
        self.assertEqual(sorted(b["checks"]), ["M1", "M2", "M3", "M4", "decision"])
        self.assertEqual(b["checks"]["M2"]["cusum"]["h"], 5.0)
        self.assertEqual(b["checks"]["M2"]["cusum"]["k"], 0.5)
        self.assertGreater(b["checks"]["M2"]["cusum"]["arl0_years"], 5.0)
        np.testing.assert_allclose(b["checks"]["M1"]["band"], [1 - math.sqrt(2 / 63), 1 + math.sqrt(2 / 63)], rtol=1e-9)
        self.assertEqual(b["checks"]["M4"]["comomentum_stock_level"][:3], "n/a")
        self.assertEqual(b["checks"]["decision"]["status"], "n/a")

    def test_quiet_live_is_ok(self):
        doc = self.live("quiet")
        self.assertEqual(doc["mode"], "live")
        self.assertEqual(doc["checks"]["M1"]["status"], "ok")
        self.assertEqual(doc["checks"]["M3"]["status"], "ok")
        a = doc["checks"]["M2"]["members"]["a"]
        self.assertEqual(a["ic_cusum"]["observations"], self.w.m // 5)
        self.assertEqual(a["ic_cusum"]["status"], "ok")   # IC at its prior mean: the increments are -k
        self.assertEqual(a["ic_cusum"]["value"], 0.0)

    def test_ic_decline_raises_the_cusum_alarm_at_h(self):
        doc = self.live("decline", ic=-0.07)  # (mu0 - x) / sd0 = 1: +0.5 per weekly observation
        a = doc["checks"]["M2"]["members"]["a"]
        self.assertAlmostEqual(a["ic_cusum"]["value"], 0.5 * (self.w.m // 5), places=9)
        self.assertEqual(a["ic_cusum"]["status"], "alarm")
        self.assertEqual(a["ic63"]["status"], "warn")
        self.assertEqual(doc["checks"]["M2"]["members"]["b"]["ic_cusum"]["status"], "ok")
        self.assertEqual(doc["checks"]["M2"]["status"], "alarm")
        self.assertEqual(doc["action"][:4], "none")

    def test_turnover_jump(self):
        doc = self.live("turn", turn=0.5)
        b = doc["checks"]["M2"]["members"]["b"]
        self.assertEqual(b["turnover_cusum"]["status"], "alarm")
        self.assertGreater(b["turnover_cusum"]["high"], b["turnover_cusum"]["low"])
        self.assertEqual(b["turnover63"]["status"], "warn")
        self.assertEqual(doc["checks"]["M2"]["members"]["a"]["turnover_cusum"]["status"], "ok")

    def test_bias_bands(self):
        k = self.base["reference"]["M1"]["pooled_kurtosis"]
        plain, half = math.sqrt(2 / 63), 1.96 * math.sqrt((k - 1) / (4 * 63))
        self.assertGreater(k, 5.0)
        self.assertGreater(half, plain)  # fat tails widen the alarm band
        for scale, want in ((1.0, "ok"), (1 + (plain + half) / 2, "warn"), (1 + half + 0.1, "alarm"),
                            (1 - (plain + half) / 2, "warn")):
            m1 = self.live(f"bias{scale:.4f}", bias=scale)["checks"]["M1"]
            self.assertAlmostEqual(m1["b63"], scale, places=9)
            self.assertEqual(m1["status"], want, scale)
            self.assertAlmostEqual(m1["realized_over_forecast"], scale, places=9)
            np.testing.assert_allclose(m1["kurtosis_band"], [1 - half, 1 + half], rtol=1e-9)

    def test_execution_cost_and_fill(self):
        doc = self.live("cost15", bps=15.0)
        m3 = doc["checks"]["M3"]
        model = self.base["reference"]["M3"]["model_cost_per_dollar"]
        self.assertAlmostEqual(m3["cost_ratio"]["value"], 15e-4 / model, places=9)
        self.assertEqual(m3["cost_ratio"]["status"], "alarm")
        doc = self.live("cost12", bps=12.0)
        self.assertEqual(doc["checks"]["M3"]["cost_ratio"]["status"], "warn")   # inside [.7, 1.3], outside p5-p95
        doc = self.live("fill", fill=0.9)
        self.assertEqual(doc["checks"]["M3"]["fill_rate"]["status"], "alarm")
        self.assertAlmostEqual(doc["checks"]["M3"]["fill_rate"]["value"], 0.9, places=9)

    def test_crowding(self):
        self.assertEqual(self.live("calm", common=0.0)["checks"]["M4"]["status"], "ok")
        doc = self.live("crowded", common=0.9)
        self.assertEqual(doc["checks"]["M4"]["status"], "alarm")
        self.assertGreater(doc["checks"]["M4"]["mean_pairwise_corr"], 0.5)

    def test_deterministic_bytes(self):
        code, _, err = self.w.run(["--baseline", *self.train_inputs, "--output", self.root / "base2"])
        self.assertEqual(code, 0, err)
        self.assertEqual((self.root / "base2" / "monitor.json").read_bytes(), self.ref.read_bytes())


class MissingInputs(unittest.TestCase):
    def test_every_check_states_its_missing_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            w = World(Path(tmp) / "in")
            code, doc, err = w.run(["--baseline", "--bias", w.bias("b.csv", book=False), "--output",
                                    Path(tmp) / "out"])
            self.assertEqual(code, 0, err)
            c = doc["checks"]
            self.assertEqual({k: v["status"] for k, v in c.items()},
                             {"M1": "n/a", "M2": "n/a", "M3": "n/a", "M4": "n/a", "decision": "n/a"})
            self.assertIn("--book-weights", c["M1"]["reason"])
            self.assertIn("--admission", c["M2"]["reason"])
            self.assertEqual(doc["status"], "n/a")

    def test_decide_health_and_refusals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            w = World(root / "in")
            dec = root / "decide"
            dec.mkdir()
            (dec / "decision.json").write_text(json.dumps({
                "schema": "atx.book-decision/v1", "asof": "2021-08-04",
                "health": {"status": "error", "checks": [{"check": "neutralization", "status": "error"}]},
                "transfer_coefficient": {"target": 0.18}}))
            w.holdings_days("hd.csv")
            (root / "in" / "hd.csv").replace(dec / "holdings_days.csv")
            code, doc, err = w.run(["--baseline", "--decide", dec, "--output", root / "out"])
            self.assertEqual(code, 0, err)
            self.assertEqual(doc["checks"]["decision"]["status"], "alarm")
            self.assertEqual(doc["checks"]["decision"]["transfer_coefficient"], 0.18)
            self.assertNotEqual(doc["checks"]["M3"]["status"], "n/a")  # the decide dir's holdings_days.csv
            code, _, err = w.run(["--baseline", "--decide", dec, "--output", root / "out"])
            self.assertEqual(code, 1)
            self.assertIn("refusing overwrite", err)
            code, _, err = w.run(["--reference", root / "out" / "monitor.json", "--output", root / "live"])
            self.assertEqual(code, 0, err)
            code, _, err = w.run(["--reference", root / "live" / "monitor.json", "--output", root / "live2"])
            self.assertEqual(code, 1)
            self.assertIn("not a baseline", err)


if __name__ == "__main__":
    unittest.main()
