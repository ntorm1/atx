"""v8 C-2: report-only traded-horizon columns (ic_theta on the card, f_theta in the admission table, K6 marginal IC on
the card). Hand values and the report-only contract (synthetic data only).

Run: python -m pytest -q -p no:cacheprovider atx-impl/tools/test_horizon_stats.py
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
import fit_composition_weights as fcw  # noqa: E402
import horizon_stats as hs  # noqa: E402
import test_alpha_report_card as card_base  # noqa: E402
import test_fit_composition_weights as fit_base  # noqa: E402


def setUpModule():  # hermetic: the fitter's in-file theme list
    fit_base.setUpModule()


def tearDownModule():
    fit_base.tearDownModule()


class HandValues(unittest.TestCase):
    def test_ic_theta_matches_hand_value(self):
        a, r, theta = 0.04, 0.9, 0.05
        m = [a * r ** (h - 1) for h in range(1, 64)]
        x = (1 - theta) * r
        closed = a * theta * (1 - x ** 63) / (1 - x)                      # geometric series, h = 1..63
        loop = sum(theta * (1 - theta) ** (h - 1) * m[h - 1] for h in range(1, 64))
        self.assertAlmostEqual(hs.ic_theta(m), closed, places=15)
        self.assertAlmostEqual(hs.ic_theta(m), loop, places=15)
        self.assertEqual(hs.HORIZON_THETA, fcw.AIM_THETA)                  # the reference construction theta
        self.assertAlmostEqual(float(hs.theta_weights().sum()), 1 - 0.95 ** 63, places=14)  # not renormalised
        self.assertIsNone(hs.ic_theta(m[:-1]))
        self.assertIsNone(hs.ic_theta(m[:10] + [None] + m[11:]))
        self.assertIsNone(hs.ic_theta(m[:10] + [math.nan] + m[11:]))

    def test_theta_book_returns_hand_case(self):
        q = np.array([[0.0, 0.0], [1.0, -1.0], [0.0, 0.0], [-1.0, 1.0]])
        fwd = np.array([[0.5, 0.5], [0.01, 0.02], [0.03, -0.01], [0.0, 0.05]])
        f, live = hs.theta_book_returns(q, fwd, theta=0.5)
        # b: flat, (.5, -.5), (.25, -.25), (-.375, .375)
        self.assertEqual(list(live), [False, True, True, True])
        self.assertTrue(math.isnan(f[0]))
        np.testing.assert_allclose(f[1:], [0.5 * 0.01 - 0.5 * 0.02, 0.25 * 0.03 + 0.25 * 0.01, 0.375 * 0.05],
                                   rtol=0, atol=1e-18)
        with self.assertRaises(ValueError):
            hs.theta_book_returns(q, fwd[:3])


def fit_files(fx, out: Path, extra=(), screen="v4-prior-v1", prior=True) -> dict:
    argv = fx.argv(out, screen, [*(["--orientation", "prior", "--composition", "ew-theme-v1"] if prior else []),
                                 *extra])
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        code = fcw.main(argv)
    assert code in (fcw.EXIT_OK, fcw.EXIT_NO_WEIGHTS), code
    return {p.name: p.read_bytes() for p in out.iterdir()}


class FTheta(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = fit_base.v4_world()
        cls.ids, cls.signals = ids, signals
        cls.fx = fit_base.Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids,
                                  families=["fam"] * len(ids), candidate_extra=extra)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def assert_report_only(self, plain: dict, report: dict, csv_columns: tuple):
        a, b = json.loads(plain[fcw.OUTPUT_ADMISSION]), json.loads(report[fcw.OUTPUT_ADMISSION])
        self.assertEqual(set(b) - set(a), {"report_only"})
        self.assertIn("gates nothing, selects nothing, weights nothing", b["report_only"]["f_theta"]["use"])
        for row_a, row_b in zip(a["candidates"], b["candidates"]):
            self.assertEqual(set(row_b) - set(row_a), set(fcw.F_THETA_COLUMNS))
            self.assertEqual(row_a, {k: v for k, v in row_b.items() if k not in fcw.F_THETA_COLUMNS})
        self.assertEqual({k: v for k, v in b.items() if k not in ("report_only", "candidates")},
                         {k: v for k, v in a.items() if k != "candidates"})  # verdicts, counts, admitted order
        lines_a = plain[fcw.OUTPUT_ADMISSION_CSV].decode().splitlines()
        lines_b = report[fcw.OUTPUT_ADMISSION_CSV].decode().splitlines()
        self.assertEqual(lines_b[0].split(","), list(csv_columns) + list(fcw.F_THETA_COLUMNS))
        self.assertEqual([",".join(x.split(",")[:-2]) for x in lines_b], lines_a)
        if fcw.OUTPUT_WEIGHTS in plain:  # the weights pin the admission bytes' SHA and nothing else changes
            wa, wb = json.loads(plain[fcw.OUTPUT_WEIGHTS]), json.loads(report[fcw.OUTPUT_WEIGHTS])
            self.assertEqual(wb["provenance"].pop("admission_sha256"), fcw.hashlib.sha256(
                report[fcw.OUTPUT_ADMISSION]).hexdigest())
            wa["provenance"].pop("admission_sha256")
            self.assertEqual(wa, wb)
        return b

    def test_f_theta_is_report_only(self):
        plain = fit_files(self.fx, self.root / "v4-plain")
        report = fit_files(self.fx, self.root / "v4-report", ["--report-f-theta"])
        adm = self.assert_report_only(plain, report, fcw.V4_CSV_COLUMNS)
        # the store serves the same column
        stored = fit_files(self.fx, self.root / "v4-stored", ["--report-f-theta", "--work-dir", str(self.root / "w")])
        self.assertEqual(stored, report)
        stored = fit_files(self.fx, self.root / "v4-stored2", ["--report-f-theta", "--work-dir", str(self.root / "w")])
        self.assertEqual(stored, report)
        # the value: mean and HAC t of s_k * f_theta over live TRAIN decisions, from the context's book
        role = fcw.RoleManifest(self.fx.manifest, self.fx.train_sha)
        ctx = fcw.Context.build(role)
        train = ((role.sessions[role.begin:role.end] >= fcw.FIT_BEGIN_NS) &
                 (role.sessions[role.begin:role.end] < fcw.TRAIN_END_NS))
        rows = {r["id"]: r for r in adm["candidates"]}
        for cid in ("flip", "slow_a"):
            q, _ = ctx.book(self.signals[self.ids.index(cid)], 1)
            f, live = hs.theta_book_returns(q, ctx.forward)
            x = f[live & train]
            self.assertEqual(rows[cid]["f_theta"], float(x.mean()))
            self.assertEqual(rows[cid]["f_theta_hac_t"], fcw.newey_west_t(x))
        self.assertIsNone(rows["insufficient"]["f_theta"])  # prior_sign 0: no orientation, no column value

    def test_f_theta_is_report_only_on_the_v3_screen(self):
        panel, signals, ids = fit_base.screen_world()
        fx = fit_base.Fixture(self.root / "v3fx", panel, signals, fit_base.SCREEN_RUNNER_SIGNS, ids=ids,
                              families=["fam"] * len(ids))
        plain = fit_files(fx, self.root / "v3-plain", screen="v3-admit-v1", prior=False)
        report = fit_files(fx, self.root / "v3-report", ["--report-f-theta"], screen="v3-admit-v1", prior=False)
        adm = self.assert_report_only(plain, report, fcw.CSV_COLUMNS)
        rows = {r["id"]: r for r in adm["candidates"]}
        self.assertEqual(rows["slow_b"]["s_k"], -1)
        self.assertGreater(rows["slow_b"]["f_theta"], 0)  # oriented by the screen sign like every other column


class CardColumns(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        p = card_base.make_panel(dates=240, names=330, score_begin=100, seed=12)
        rng = np.random.default_rng(1)
        cls.world = card_base.CardWorld(cls.root / "w", p, {i: rng.standard_normal(p["close"].shape)
                                                            for i in ("a", "b", "c")}, ref_horizons=(21,))
        cls.plain_code, _ = cls.world.run(cls.root / "plain")
        cls.k6 = cls.root / "marginal_ic.json"
        cls.k6.write_text(json.dumps({"schema": "atx.marginal-ic/v1", "candidates": [
            {"id": "a", "ic21": 0.02, "ic21_hac_t": 2.5, "marginal_ic21": 0.011, "marginal_hac_t": 1.9,
             "max_abs_rho": 0.4, "max_rho_member": "b", "n_dates": 120},
            {"id": "b", "ic21": -0.01, "ic21_hac_t": -1.0, "marginal_ic21": None, "marginal_hac_t": None,
             "max_abs_rho": None, "max_rho_member": None}]}), encoding="utf-8")
        cls.code, cls.err = cls.world.run(cls.root / "report", ["--ic-theta", "--marginal-ic", str(cls.k6)])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_card_ic_theta_matches_hand_value_of_its_decay_curve(self):
        self.assertEqual((self.plain_code, self.code), (0, 0), self.err)
        for cid in ("a", "b", "c"):
            card = card_base.load(self.root / "report", cid)
            m = card["decay"]["ic"]
            hand = sum(0.05 * 0.95 ** (h - 1) * m[h - 1] for h in range(1, 64))
            self.assertAlmostEqual(card["horizon"]["ic_theta"], hand, delta=1e-9 * max(1.0, abs(hand)))
            self.assertEqual((card["horizon"]["theta"], card["horizon"]["h"]), (0.05, [1, 63]))

    def test_marginal_ic_rows_are_copied(self):
        a, b, c = (card_base.load(self.root / "report", i) for i in ("a", "b", "c"))
        self.assertEqual(a["marginal_ic"], {"ic21": 0.02, "ic21_hac_t": 2.5, "marginal_ic21": 0.011,
                                            "marginal_hac_t": 1.9, "max_abs_rho": 0.4, "max_rho_member": "b"})
        self.assertIsNone(b["marginal_ic"]["marginal_ic21"])
        self.assertEqual(c["marginal_ic"], {"status": "absent from marginal_ic.json"})
        index = json.loads((self.root / "report" / "index.json").read_bytes())
        rows = {r["id"]: r for r in index["candidates"]}
        self.assertEqual(rows["a"]["marginal_ic21"], 0.011)
        self.assertIn(str(self.k6), index["inputs"]["files"])

    def test_switches_off_leave_the_cards_unchanged(self):
        plain = {p.name: p.read_bytes() for p in (self.root / "plain").iterdir()}
        for name, data in plain.items():
            self.assertNotIn(b"ic_theta", data, name)
            self.assertNotIn(b"marginal_ic", data, name)
        report = card_base.load(self.root / "report", "a")
        for key in ("horizon", "marginal_ic"):
            report.pop(key)
        self.assertEqual(json.dumps(report, sort_keys=True), json.dumps(card_base.load(self.root / "plain", "a"),
                                                                        sort_keys=True))

    def test_admission_f_theta_is_copied_and_bad_k6_refused(self):
        adm_path = self.world.admission
        original = adm_path.read_bytes()
        adm = json.loads(original)
        for row in adm["candidates"]:
            row.update(f_theta=0.0003, f_theta_hac_t=1.25)
        adm_path.write_bytes(json.dumps(adm, indent=2).encode())
        try:
            code, err = self.world.run(self.root / "copied")
            self.assertEqual(code, 0, err)
            card = card_base.load(self.root / "copied", "a")
            self.assertEqual((card["admission"]["f_theta"], card["admission"]["f_theta_hac_t"]), (0.0003, 1.25))
        finally:
            adm_path.write_bytes(original)
        code, err = self.world.run(self.root / "pinned", ["--marginal-ic", str(self.k6), "--marginal-ic-sha256",
                                                          "0" * 64])
        self.assertEqual(code, 1)
        self.assertIn("marginal IC: SHA-256 pin differs", err)
        bad = self.root / "bad.json"
        bad.write_text(json.dumps([{"id": "a", "ic21": 0.1}]), encoding="utf-8")
        code, err = self.world.run(self.root / "bad", ["--marginal-ic", str(bad)])
        self.assertEqual(code, 1)
        self.assertIn("K6 keys", err)


if __name__ == "__main__":
    unittest.main()
