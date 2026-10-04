"""Synthetic checks for composition_theme_erc (rule theme-erc-v1, platform v8 expansion X, lane XCOMB; no real data).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_theme_erc.py
"""
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import composition_ic_shrink as cis  # noqa: E402
import composition_rules as cr  # noqa: E402
import composition_theme_erc as cte  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)
from test_composition_rules import ref_cap  # noqa: E402  (an independent loop port of the member cap)

# Shared with atx-impl/tests/strategy_ic_theme_erc_test.cpp (ThemeErcV1.SharedFixtureFractions): both rules must give
# its exact fractions, so the Python fitter equals the C++ rule on it.
FIXTURE = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "theme_erc_v1.json"
STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")
SHRINK_ARGS = dict(STD_ARGS, composition="ic-shrink-v1")
ERC_ARGS = dict(STD_ARGS, theme_erc="theme-erc-v1")
ERC_SHRINK_ARGS = dict(SHRINK_ARGS, theme_erc="theme-erc-v1")


def fraction(pair) -> float:
    return pair[0] / pair[1]


def fixture() -> tuple[dict, list[str], list[str], list[float], list[str], list[list[float]]]:
    fx = json.loads(FIXTURE.read_text(encoding="utf-8"))
    rows = fx["members"]
    order = fx["covariance"]["themes"]
    cov = [[fraction(v) for v in row] for row in fx["covariance"]["matrix"]]
    return fx, [r["id"] for r in rows], [r["theme"] for r in rows], [fraction(r["share"]) for r in rows], order, cov


def contributions(cov: list[list[float]], b: list[float]) -> list[float]:
    """c_t = b_t (C b)_t, written independently of the module."""
    m = np.asarray(cov) @ np.asarray(b)
    return [float(x) for x in np.asarray(b) * m]


def capped(ids, themes, shares, theme_share: dict) -> list[float]:
    """a_k * b_theme, then the cap 1/(2T) by the independent loop port."""
    t = len(set(themes))
    w = ref_cap({k: a * theme_share[th] for k, (a, th) in enumerate(zip(shares, themes))}, dict(enumerate(themes)),
                1.0 / (2 * t))
    return [w[k] for k in range(len(ids))]


class DeclaredRule(unittest.TestCase):
    def test_registered_constants(self):
        self.assertEqual(cte.RULE_ID, "theme-erc-v1")
        self.assertEqual(cte.BLOCK, "theme_erc")
        self.assertEqual(cte.PARENT_RULES, ("ew-theme-std-v1", "ic-shrink-v1"))
        self.assertEqual((cte.SWEEPS, cte.DISPERSION), (10000, 1e-10))
        self.assertEqual((cte.SHARE_TOLERANCE, cte.RUNNER_TOLERANCE, cte.MAX_THEMES), (1e-12, 1e-12, 32))
        self.assertEqual(cr.CAP_TOLERANCE, 1e-12)  # the runner's theme_erc_cap_tolerance

    def test_shared_fixture_fractions(self):
        """The fixture's exact fractions (the C++ test asserts the same ones): ERC theme shares 1/2, 1/3, 1/6 over the
        covariance's own theme order, the cap on value_a in one pass, every weight."""
        fx, ids, themes, shares, order, cov = fixture()
        out = cte.rule_weights(ids, themes, shares, order, cov)
        for t, b in zip(order, out["theme_shares"]):
            self.assertLessEqual(abs(b - fraction(fx["theme_shares"][t])), 1e-15, t)
        self.assertLessEqual(out["dispersion"], 1e-14)
        self.assertEqual(out["cap"], fraction(fx["cap"]))
        self.assertEqual(len(out["passes"]), fx["cap_passes"])
        self.assertEqual([ids[k] for it in out["passes"] for k in it["capped"]], fx["capped"])
        for k, row in enumerate(fx["members"]):
            self.assertLessEqual(abs(out["uncapped"][k] - fraction(row["weight_before_cap"])), 1e-15, row["id"])
            self.assertLessEqual(abs(out["weights"][k] - fraction(row["weight"])), 1e-15, row["id"])
        self.assertAlmostEqual(float(np.sum(out["weights"])), 1.0, places=15)
        self.assertEqual((fx["sweeps"], fx["dispersion"]), (cte.SWEEPS, cte.DISPERSION))

    def test_written_rule(self):
        """Steps 3-4 read literally: shares with equal risk contributions (checked on c_t, not on the solver), then
        a_k * b and the cap by the independent loop port."""
        fx, ids, themes, shares, order, cov = fixture()
        out = cte.rule_weights(ids, themes, shares, order, cov)
        c = contributions(cov, out["theme_shares"])
        self.assertLessEqual(max(c) - min(c), 1e-14 * max(c))
        self.assertAlmostEqual(sum(out["theme_shares"]), 1.0, places=15)
        want = capped(ids, themes, shares, dict(zip(order, out["theme_shares"])))
        for k in range(len(ids)):
            self.assertLessEqual(abs(out["weights"][k] - want[k]), 1e-15, ids[k])

    def test_erc_closed_forms(self):
        """Two groups: s_1 = sigma_2 / (sigma_1 + sigma_2) whatever the correlation; equal correlations: s ~ 1/sigma;
        scale free; one group takes everything."""
        for rho in (-0.5, 0.0, 0.7):
            s, _, d = cte.erc_shares([[1.0, 2.0 * rho], [2.0 * rho, 4.0]])
            self.assertLessEqual(abs(s[0] - 2.0 / 3.0), 1e-15, rho)
            self.assertLessEqual(abs(s[1] - 1.0 / 3.0), 1e-15, rho)
            self.assertLessEqual(d, 1e-14, rho)
        vol = [1.0, 2.0, 4.0]
        cov = [[(1.0 if i == j else 0.25) * vol[i] * vol[j] for j in range(3)] for i in range(3)]
        s, _, _ = cte.erc_shares(cov)
        for x, want in zip(s, (4.0 / 7.0, 2.0 / 7.0, 1.0 / 7.0)):
            self.assertLessEqual(abs(x - want), 1e-15)
        _, _, _, _, _, fcov = fixture()
        small = [[v * 1e-6 for v in row] for row in fcov]
        a, _, _ = cte.erc_shares(fcov)
        b, _, _ = cte.erc_shares(small)
        self.assertLessEqual(max(abs(x - y) for x, y in zip(a, b)), 1e-15)
        self.assertEqual(cte.erc_shares([[3.0]])[0], [1.0])

    def test_fixture_tells_wrong_rules_apart(self):
        """Review T-1: equal theme shares (the parent's 1/T), inverse-volatility shares (no correlation), no cap, and
        the covariance read in the members' theme order instead of its recorded order each move some weight by more
        than 1e-3."""
        fx, ids, themes, shares, order, cov = fixture()
        good = cte.rule_weights(ids, themes, shares, order, cov)["weights"]
        gap = lambda w: max(abs(float(a) - float(b)) for a, b in zip(w, good))  # noqa: E731
        self.assertGreater(gap(capped(ids, themes, shares, {t: 1.0 / 3.0 for t in order})), 1e-3)
        inv = [1.0 / math.sqrt(cov[i][i]) for i in range(3)]
        self.assertGreater(gap(capped(ids, themes, shares, {t: v / sum(inv) for t, v in zip(order, inv)})), 1e-3)
        b = dict(zip(order, cte.rule_weights(ids, themes, shares, order, cov)["theme_shares"]))
        self.assertGreater(gap([a * b[t] for a, t in zip(shares, themes)]), 1e-3)
        first = list(dict.fromkeys(themes))  # value, momentum, quality: the matrix taken in that order by mistake
        wrong = dict(zip(first, cte.erc_shares(cov)[0]))
        self.assertGreater(gap(capped(ids, themes, shares, wrong)), 1e-3)

    def test_sleeves_and_covariance(self):
        """Step 2: r_t = sum_{k in t} a_k * (s_k f_k) over the mask's decisions; the sample covariance (n - 1), exactly
        symmetric, equals numpy's."""
        rng = np.random.default_rng(7)
        matrix = rng.normal(0.0, 1e-3, size=(5, 40))
        matrix[1, 3] = 0.0  # a flat decision is 0 in the fitter's series
        themes = ["b", "a", "b", "c", "a"]
        shares = [0.25, 0.6, 0.75, 1.0, 0.4]
        mask = np.ones(40, dtype=bool)
        mask[:5] = False
        series = cte.sleeve_series(matrix, themes, shares, ["a", "b", "c"], mask)
        want = np.vstack([0.6 * matrix[1, 5:] + 0.4 * matrix[4, 5:], 0.25 * matrix[0, 5:] + 0.75 * matrix[2, 5:],
                          matrix[3, 5:]])
        self.assertLessEqual(float(np.max(np.abs(series - want))), 1e-18)
        cov = cte.covariance(series)
        ref = np.cov(want, ddof=1)
        for i in range(3):
            for j in range(3):
                self.assertEqual(cov[i][j], cov[j][i])
                self.assertLessEqual(abs(cov[i][j] - ref[i, j]), 1e-12 * abs(ref[i, i]))

    def test_parent_shares(self):
        """Step 1: the parent's weights_before_cap normalised inside each theme (ew-theme-std-v1 tier shares)."""
        parent = cr.StdFit(weights=np.array([0.0]), theme_table={}, block={},
                           provenance={"weights_before_cap": {"x": 0.3, "y": 0.2, "z": 0.5}})
        self.assertEqual(cte.parent_shares(parent, ["x", "y", "z"], ["t", "t", "u"]), [0.6, 0.4, 1.0])
        with self.assertRaises(cte.ErcError):
            cte.parent_shares(cr.StdFit(weights=np.array([0.0]), theme_table={}, block={}, provenance={}),
                              ["x"], ["t"])

    def test_refusals(self):
        fx, ids, themes, shares, order, cov = fixture()
        bad = [list(r) for r in cov]
        bad[0][1] += 1e-9  # not symmetric
        cases = [
            lambda: cte.erc_shares([]),
            lambda: cte.erc_shares([[1.0, 0.0]]),
            lambda: cte.erc_shares(bad),
            lambda: cte.erc_shares([[0.0, 0.0], [0.0, 1.0]]),
            lambda: cte.erc_shares([[float("nan"), 0.0], [0.0, 1.0]]),
            lambda: cte.erc_shares([[1.0]], sweeps=0),
            lambda: cte.rule_weights(ids, themes, [s * 1.001 for s in shares], order, cov),   # shares do not sum to 1
            lambda: cte.rule_weights(ids, themes, [-s for s in shares], order, cov),
            lambda: cte.rule_weights(ids, themes, shares, order[:2], [r[:2] for r in cov[:2]]),  # a theme missing
            lambda: cte.rule_weights(ids[:-1], themes, shares, order, cov),
            lambda: cte.rule_weights(["a", "b"], ["x", "x"], [0.75, 0.25], ["x"], [[1.0]]),     # infeasible cap
        ]
        for k, case in enumerate(cases):
            with self.assertRaises(cte.ErcError, msg=str(k)):
                case()
        hard = [[1.0, 0.999], [0.999, 1.0]]  # one sweep does not equalise a near-singular 3 x 3
        hard3 = [[1.0, 0.99, -0.5], [0.99, 4.0, 0.2], [-0.5, 0.2, 9.0]]
        self.assertGreater(cte.erc_shares(hard3, sweeps=1)[2], 1e-10)
        with unittest.mock.patch.object(cte, "SWEEPS", 1), self.assertRaises(cte.ErcError):
            cte.rule_weights(["a", "b", "c"], ["x", "y", "z"], [1.0, 1.0, 1.0], ["x", "y", "z"], hard3)
        self.assertLessEqual(cte.erc_shares(hard)[2], 1e-12)

    def test_block_and_attach_are_what_the_runner_reads(self):
        """theme_erc() on a synthetic parent: the block records every member with its share, the covariance in sorted
        theme order and the registered constants; attach replaces the parent's block and records the rule."""
        rng = np.random.default_rng(3)
        ids = [f"m{k}" for k in range(9)]
        themes = ["v", "q", "v", "q", "m", "m", "v", "q", "m"]
        parent = cis.ic_shrink(ids, themes, [0.003, 0.002, 0.001, 0.004, 0.002, 0.003, 0.002, 0.003, 0.0025])
        scale = np.array([[1.0 if t == "v" else 0.5 if t == "q" else 3.0] for t in themes])
        matrix = rng.normal(0.0, 1e-3, size=(9, 300)) * scale
        mask = np.ones(300, dtype=bool)
        fit = cte.theme_erc(parent, ids, themes, matrix, mask, "ic-shrink-v1")
        block = fit.block
        self.assertEqual((block["rule"], block["rerank"]), ("theme-erc-v1", True))
        erc = block["theme_erc"]
        self.assertEqual((erc["sweeps"], erc["dispersion"]), (10000, 1e-10))
        self.assertEqual(erc["covariance"]["themes"], ["m", "q", "v"])
        before = parent.provenance["weights_before_cap"]
        for i, t in zip(ids, themes):
            mass = sum(before[j] for j, u in zip(ids, themes) if u == t)
            self.assertEqual(erc["members"][i]["theme"], t)
            self.assertLessEqual(abs(erc["members"][i]["share"] - before[i] / mass), 1e-16)
        again = cte.rule_weights(ids, themes, [erc["members"][i]["share"] for i in ids], erc["covariance"]["themes"],
                                 erc["covariance"]["matrix"])
        self.assertLessEqual(float(np.max(np.abs(again["weights"] - fit.weights))), 1e-15)
        self.assertEqual(set(block["themes"]), {i for i, w in zip(ids, fit.weights) if w > 0})
        b = fit.provenance["theme_shares"]
        self.assertLess(b["m"], b["q"])  # the volatile sleeve takes the smallest share
        doc = {"schema": "v1", "provenance": {"rule": "ic-shrink-v1", "ic_shrink": {"x": 1}},
               "theme_standardise": parent.block}
        cte.attach(doc, fit)
        self.assertEqual(doc["schema"], cr.WEIGHTS_SCHEMA_V2)
        self.assertEqual(doc["theme_standardise"], block)
        self.assertEqual(doc["provenance"]["rule"], "theme-erc-v1")
        self.assertEqual(doc["provenance"]["theme_erc"]["parent_rule"], "ic-shrink-v1")
        self.assertEqual(doc["provenance"]["ic_shrink"], {"x": 1})
        with self.assertRaises(cte.ErcError):
            cte.theme_erc(parent, ids, themes, matrix, mask, "ew-theme-std-aim-v1")


class FitterEndToEnd(unittest.TestCase):
    """fit_composition_weights --theme-erc theme-erc-v1 on the fitter's synthetic world (as test_composition_ic_shrink)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = tfw.aim_world()
        extra = dict(extra)
        extra["flip"] = dict(extra["flip"], theme="value")           # two themes, five admitted members: M > 2T
        extra["medium_half"] = dict(extra["medium_half"], tier="C+")  # a scored grade (the std parent reads tiers)
        cls.ids = ids
        cls.fx = tfw.Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                             candidate_extra=extra)
        cls.spy = {}
        real = cte.sleeve_series

        def spy(matrix, themes, shares, order, mask):
            cls.spy.update(matrix=np.array(matrix), mask=np.array(mask), themes=list(themes), shares=list(shares))
            return real(matrix, themes, shares, order, mask)

        with unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json"):
            cls.std_code, _ = fcw.fit(cls.fx.args(cls.root / "std", **STD_ARGS))
            cls.shrink_code, _ = fcw.fit(cls.fx.args(cls.root / "shrink", **SHRINK_ARGS))
            with unittest.mock.patch.object(cte, "sleeve_series", spy):
                cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "erc", **ERC_ARGS))
            cls.shrink_erc_code, cls.shrink_summary = fcw.fit(cls.fx.args(cls.root / "erc-shrink", **ERC_SHRINK_ARGS))
        load = lambda name: {p.name: p.read_bytes() for p in (cls.root / name).iterdir()}  # noqa: E731
        cls.std_bytes, cls.shrink_bytes = load("std"), load("shrink")
        cls.bytes, cls.shrink_erc_bytes = load("erc"), load("erc-shrink")
        cls.std = json.loads(cls.std_bytes[fcw.OUTPUT_WEIGHTS])
        cls.shrink = json.loads(cls.shrink_bytes[fcw.OUTPUT_WEIGHTS])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])
        cls.shrink_doc = json.loads(cls.shrink_erc_bytes[fcw.OUTPUT_WEIGHTS])
        cls.adm = json.loads(cls.bytes[fcw.OUTPUT_ADMISSION])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def members(self) -> list[str]:
        rows = {c["id"]: c for c in self.adm["candidates"]}
        weighted = {i for i, w in self.std["weights"].items() if w > 0}
        return sorted(weighted, key=lambda i: rows[i]["admission_rank"])

    def test_weights_follow_the_rule_on_the_parents_shares(self):
        """The admission is the parent's; the recorded shares are the parent's pre-cap within-theme shares; the weights
        are steps 3-4 on the recorded inputs; ERC moved weight across the themes."""
        self.assertEqual((self.code, self.std_code, self.shrink_code, self.shrink_erc_code), (fcw.EXIT_OK,) * 4)
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.std_bytes[fcw.OUTPUT_ADMISSION])
        members = self.members()
        rows = {c["id"]: c for c in self.adm["candidates"]}
        erc = self.doc["theme_standardise"]["theme_erc"]
        before = self.std["provenance"]["std"]["weights_before_cap"]
        for i in members:
            mass = sum(before[j] for j in members if rows[j]["theme"] == rows[i]["theme"])
            self.assertEqual(erc["members"][i], {"theme": rows[i]["theme"], "share": before[i] / mass})
        self.assertEqual(sorted(erc["members"]), sorted(members))
        out = cte.rule_weights(members, [rows[i]["theme"] for i in members], [erc["members"][i]["share"] for i in members],
                               erc["covariance"]["themes"], erc["covariance"]["matrix"])
        for i, w in zip(members, out["weights"]):
            self.assertLessEqual(abs(self.doc["weights"][i] - float(w)), 1e-15, i)
        self.assertTrue(all(self.doc["weights"][i] == 0.0 for i in self.ids if i not in members))
        self.assertAlmostEqual(sum(self.doc["weights"].values()), 1.0, places=15)
        shares = self.doc["provenance"]["theme_erc"]["theme_shares"]
        self.assertGreater(max(abs(b - 0.5) for b in shares.values()), 1e-3)  # not the parent's 1/T
        self.assertEqual(self.doc["signs"], self.std["signs"])
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_sleeves_are_the_fitters_signed_series_on_the_train_mask(self):
        """Step 2 reads the members' signed factor series (the blend diagnostic's matrix) over the TRAIN decisions; the
        recorded covariance is that of the share-weighted sleeves."""
        members = self.members()
        self.assertEqual(self.spy["matrix"].shape[0], len(members))
        self.assertEqual(int(self.spy["mask"].sum()), self.doc["provenance"]["theme_erc"]["decisions"])
        self.assertGreater(self.doc["provenance"]["theme_erc"]["decisions"], 100)
        erc = self.doc["theme_standardise"]["theme_erc"]
        order = erc["covariance"]["themes"]
        series = cte.sleeve_series(self.spy["matrix"], self.spy["themes"], self.spy["shares"], order, self.spy["mask"])
        ref = np.cov(series, ddof=1)
        cov = np.asarray(erc["covariance"]["matrix"])
        self.assertLessEqual(float(np.max(np.abs(cov - ref))), 1e-12 * float(np.max(np.abs(ref))))
        # the blend diagnostic and the sleeves read the same signed, zero-filled rows
        diag = self.doc["provenance"]["blend_in_sample_TRAIN_diagnostic"]
        w = np.array([self.doc["weights"][i] for i in members])
        self.assertEqual(diag, fcw.factor_stats(w @ self.spy["matrix"]))

    def test_document_is_what_the_runner_reads(self):
        """Schema v2 with the theme-erc-v1 block; provenance.rule names the rule, the parent's provenance block stays;
        the summary keeps the parent composition and records the theme shares."""
        self.assertEqual(self.doc["schema"], "atx.dsl-composition-weights/v2")
        block = self.doc["theme_standardise"]
        self.assertEqual((block["rule"], block["rerank"]), ("theme-erc-v1", True))
        self.assertEqual(set(block["themes"]), {i for i, x in self.doc["weights"].items() if x > 0})
        self.assertEqual(self.doc["provenance"]["rule"], "theme-erc-v1")
        self.assertEqual(self.doc["provenance"]["theme_erc"]["parent_rule"], "ew-theme-std-v1")
        self.assertIn("std", self.doc["provenance"])
        self.assertEqual((self.summary["composition"], self.summary["theme_erc"]["parent_rule"]),
                         ("ew-theme-std-v1", "ew-theme-std-v1"))
        self.assertEqual(self.summary["theme_erc"]["theme_shares"], self.doc["provenance"]["theme_erc"]["theme_shares"])
        self.assertNotIn("theme_residualise", self.doc)

    def test_ic_shrink_parent(self):
        """On an ic-shrink-v1 parent the shares are its shrunk-IC shares; the block replaces ic-shrink-v1's."""
        members = self.members()
        erc = self.shrink_doc["theme_standardise"]["theme_erc"]
        before = self.shrink["provenance"]["ic_shrink"]["weights_before_cap"]
        rows = {c["id"]: c for c in self.adm["candidates"]}
        for i in members:
            mass = sum(before[j] for j in members if rows[j]["theme"] == rows[i]["theme"])
            self.assertLessEqual(abs(erc["members"][i]["share"] - before[i] / mass), 1e-16, i)
        self.assertEqual(self.shrink_doc["provenance"]["theme_erc"]["parent_rule"], "ic-shrink-v1")
        self.assertIn("ic_shrink", self.shrink_doc["provenance"])
        self.assertEqual(erc["covariance"]["themes"], self.doc["theme_standardise"]["theme_erc"]["covariance"]["themes"])
        self.assertNotEqual(self.shrink_doc["weights"], self.doc["weights"])

    def test_refused_outside_its_parents(self):
        """--theme-erc needs ew-theme-std-v1 or ic-shrink-v1 on the prior path and is refused with --theme-resid,
        before anything is published."""
        cases = [dict(STD_ARGS, composition="ew-theme-v1", theme_erc="theme-erc-v1"),
                 dict(STD_ARGS, composition="ew-theme-std-aim-v1", theme_erc="theme-erc-v1"),
                 dict(ERC_ARGS, theme_resid="theme-resid-v1")]
        for k, case in enumerate(cases):
            out = self.root / f"refused-{k}"
            with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                    self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **case))
            self.assertIn("theme-erc-v1", str(caught.exception))
            self.assertFalse(out.exists())
        self.assertIsNone(fcw.parse_args(self.fx.argv(self.root / "x", "v4-prior-v1")).theme_erc)

    def test_flag_absent_paths_never_touch_the_rule(self):
        """Flag-absent identity: with the rule's functions made to fail, ew-theme-std-v1, ic-shrink-v1 and ew-theme-v1
        fit to the same bytes as before."""
        boom = unittest.mock.Mock(side_effect=AssertionError("theme-erc-v1 reached without its flag"))
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                unittest.mock.patch.object(cte, "theme_erc", boom), unittest.mock.patch.object(cte, "attach", boom):
            code, _ = fcw.fit(self.fx.args(self.root / "std-again", **STD_ARGS))
            shrink_code, _ = fcw.fit(self.fx.args(self.root / "shrink-again", **SHRINK_ARGS))
            v1_code, _ = fcw.fit(self.fx.args(self.root / "v1-a", **tfw.V4_ARGS))
        self.assertEqual((code, shrink_code, v1_code), (fcw.EXIT_OK,) * 3)
        boom.assert_not_called()
        self.assertEqual({p.name: p.read_bytes() for p in (self.root / "std-again").iterdir()}, self.std_bytes)
        self.assertEqual({p.name: p.read_bytes() for p in (self.root / "shrink-again").iterdir()}, self.shrink_bytes)
        v1 = json.loads((self.root / "v1-a" / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertNotIn("theme_erc", v1["provenance"])


if __name__ == "__main__":
    unittest.main()
