"""theme-tsmom-v1 (platform v8 Y, lane YCOMB, rule Y-2): the registered constants (pinned against the C++ header), the
mass kernel on its closed form and on the shared fixture (the C++ kernel reproduces the same bits), the block windows
and their look-ahead boundary, and the fitter end to end on its synthetic world (the parent's document plus the block,
nothing else; flag absent, the parent's bytes)."""
from __future__ import annotations

import json
import re
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import composition_rules as cr  # noqa: E402
import composition_theme_erc as cte  # noqa: E402
import composition_theme_tsmom as ct  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)

# Shared with atx-impl/tests/strategy_ic_theme_tsmom_test.cpp (ThemeTsmom.SharedFixtureMasses).
FIXTURE = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "theme_tsmom_v1.json"
HEADER = Path(__file__).resolve().parents[1] / "src" / "strategy_ic_theme_tsmom.hpp"
STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")
ERC_ARGS = dict(STD_ARGS, theme_erc="theme-erc-v1")
TSMOM_ARGS = dict(ERC_ARGS, theme_tsmom="theme-tsmom-v1")
STD_TSMOM_ARGS = dict(STD_ARGS, theme_tsmom="theme-tsmom-v1")


class DeclaredRule(unittest.TestCase):
    def test_constants_match_the_runner_header(self):
        text = HEADER.read_text(encoding="utf-8")
        for name, value in (("lookback", ct.LOOKBACK), ("lag", ct.LAG), ("step", ct.STEP),
                            ("max_blocks", ct.MAX_BLOCKS)):
            found = re.search(rf"theme_tsmom_{name}=(\d+);", text)
            self.assertIsNotNone(found, name)
            self.assertEqual(int(found.group(1)), value, name)
        self.assertIn(f'theme_tsmom_rule="{ct.RULE_ID}"', text)
        self.assertEqual((ct.LOOKBACK, ct.LAG, ct.STEP), (252, 3, 21))  # registered blind (Ehsani-Linnainmaa 12-1)


class Kernel(unittest.TestCase):
    def test_closed_form(self):
        """One theme lost: its mass 0, the rest scaled by S / K (sums in order); all won or none won: verbatim."""
        m, off = ct.masses([0.2, 0.3, 0.5], [1.0, -1.0, 2.0])
        scale = (0.0 + 0.2 + 0.3 + 0.5) / (0.0 + 0.2 + 0.5)
        self.assertEqual((m, off), ([0.2 * scale, 0.0, 0.5 * scale], 1))
        self.assertAlmostEqual(sum(m), 1.0, places=15)
        self.assertEqual(ct.masses([0.25, 0.75], [0.1, 0.2]), ([0.25, 0.75], 0))
        self.assertEqual(ct.masses([0.25, 0.75], [-0.1, 0.0]), ([0.25, 0.75], 0))  # zero is not a win
        m, off = ct.masses([0.5, 0.5], [0.0, 1e-300])
        self.assertEqual((m, off), ([0.0, 1.0], 1))
        # wrong rules told apart: equal split of the freed mass, no renormalisation
        m, _ = ct.masses([0.1, 0.3, 0.6], [-1.0, 1.0, 1.0])
        self.assertNotEqual(m, [0.0, 0.35, 0.65])
        self.assertNotEqual(m, [0.0, 0.3, 0.6])

    def test_refusals(self):
        for parent, trailing in (([], []), ([0.5], [0.1, 0.2]), ([0.0, 1.0], [1.0, 1.0]),
                                 ([float("nan")], [1.0]), ([1.0], [float("inf")]), ([1], [1.0])):
            with self.assertRaises(ct.TsmomError):
                ct.masses(parent, trailing)

    def test_shared_fixture(self):
        """The fixture's masses are the kernel's, bit for bit (the C++ kernel is held to the same values)."""
        fx = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.assertEqual(fx["rule"], ct.RULE_ID)
        self.assertGreaterEqual(len(fx["cases"]), 5)
        for case in fx["cases"]:
            m, off = ct.masses(case["parent"], case["trailing"])
            self.assertEqual(m, case["masses"])
            self.assertEqual(off, case["off"])
        self.assertEqual({c["off"] for c in fx["cases"]} >= {0, 1, 2}, True)


class Windows(unittest.TestCase):
    def test_block_starts_and_window(self):
        """j_b = lookback + lag - 1 + b step; T(j) sums d = j - lag - lookback + 1 .. j - lag."""
        self.assertEqual(ct.block_starts(254), [])
        self.assertEqual(ct.block_starts(255), [254])
        self.assertEqual(ct.block_starts(297), [254, 275, 296])
        series = np.arange(1.0, 13.0).reshape(1, 12)  # r(d) = d + 1
        starts = ct.block_starts(12, lookback=3, lag=2, step=2)
        self.assertEqual(starts, [4, 6, 8, 10])
        for j in starts:
            self.assertEqual(ct.trailing(series, j, lookback=3, lag=2), [float(sum(range(j - 4 + 1, j - 2 + 2)))])

    def test_no_term_realised_after_the_block(self):
        """Look-ahead probe: changing any r(d) with d > j - lag leaves block j's sums; d = j - lag is read."""
        rng = np.random.default_rng(3)
        series = rng.normal(size=(2, 400))
        for j in ct.block_starts(400):
            base = ct.trailing(series, j)
            later = series.copy()
            later[:, j - ct.LAG + 1:] += 1e3
            self.assertEqual(ct.trailing(later, j), base)
            edge = series.copy()
            edge[:, j - ct.LAG] += 1.0
            self.assertNotEqual(ct.trailing(edge, j), base)
            first = series.copy()
            first[:, j - ct.LAG - ct.LOOKBACK] += 1e3  # just outside the window
            self.assertEqual(ct.trailing(first, j), base)

    def test_sleeves_mix_by_final_weight_share(self):
        matrix = np.array([[1.0, 2.0, 3.0], [3.0, 2.0, 1.0], [5.0, 5.0, 5.0], [7.0, 7.0, 7.0]])
        themes, weights = ["a", "a", "b", "b"], [0.1, 0.3, 0.6, 0.0]
        out = ct.sleeves(matrix, themes, weights, ["a", "b"], np.array([True, True, False]))
        np.testing.assert_allclose(out[0], [0.25 * 1 + 0.75 * 3, 0.25 * 2 + 0.75 * 2, 0.0], rtol=0, atol=1e-15)
        np.testing.assert_allclose(out[1], [5.0, 5.0, 0.0], rtol=0, atol=0)  # the zero-weight member does not enter


class FitterEndToEnd(unittest.TestCase):
    """fit_composition_weights --theme-tsmom theme-tsmom-v1 on the fitter's synthetic world (as test_composition_theme_erc)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = tfw.aim_world()
        extra = dict(extra)
        extra["flip"] = dict(extra["flip"], theme="value")
        extra["medium_half"] = dict(extra["medium_half"], tier="C+")
        cls.ids = ids
        cls.fx = tfw.Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                             candidate_extra=extra)
        cls.spy = {}
        real = ct.schedule

        def spy(ids_, themes, weights, matrix, sessions, mask, parent_rule, error=ct.TsmomError):
            cls.spy.setdefault(parent_rule, dict(ids=list(ids_), themes=list(themes), weights=[float(w) for w in weights],
                                                 matrix=np.array(matrix), sessions=np.array(sessions),
                                                 mask=np.array(mask)))
            return real(ids_, themes, weights, matrix, sessions, mask, parent_rule, error)

        with unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json"):
            cls.erc_code, _ = fcw.fit(cls.fx.args(cls.root / "erc", **ERC_ARGS))
            cls.std_code, _ = fcw.fit(cls.fx.args(cls.root / "std", **STD_ARGS))
            with unittest.mock.patch.object(ct, "schedule", spy):
                cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "tsmom", **TSMOM_ARGS))
                cls.std_tsmom_code, _ = fcw.fit(cls.fx.args(cls.root / "std-tsmom", **STD_TSMOM_ARGS))
        load = lambda name: {p.name: p.read_bytes() for p in (cls.root / name).iterdir()}  # noqa: E731
        cls.erc_bytes, cls.std_bytes = load("erc"), load("std")
        cls.bytes, cls.std_tsmom_bytes = load("tsmom"), load("std-tsmom")
        cls.erc = json.loads(cls.erc_bytes[fcw.OUTPUT_WEIGHTS])
        cls.std = json.loads(cls.std_bytes[fcw.OUTPUT_WEIGHTS])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])
        cls.std_doc = json.loads(cls.std_tsmom_bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_document_is_the_parent_plus_the_block(self):
        """Weights, signs, theme_standardise and provenance.rule are the parent's; only the block and
        provenance.theme_tsmom are added; the admission is the parent's."""
        self.assertEqual((self.code, self.erc_code, self.std_code, self.std_tsmom_code), (fcw.EXIT_OK,) * 4)
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.erc_bytes[fcw.OUTPUT_ADMISSION])
        for doc, parent, rule in ((self.doc, self.erc, "theme-erc-v1"), (self.std_doc, self.std, "ew-theme-std-v1")):
            rest = json.loads(json.dumps(doc))
            block = rest.pop(ct.BLOCK)
            prov = rest["provenance"].pop("theme_tsmom")
            self.assertEqual(rest, parent)
            self.assertEqual(doc["provenance"]["rule"], rule)
            self.assertEqual(prov["parent_rule"], rule)
            self.assertEqual((block["rule"], block["lookback"], block["lag"], block["step"]),
                             (ct.RULE_ID, 252, 3, 21))
        self.assertEqual(self.summary["theme_tsmom"]["parent_rule"], "theme-erc-v1")
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_blocks_are_the_registered_windows_of_the_parents_sleeves(self):
        """Themes = the weighted themes in ascending order; one block per registered start, its session the decision's;
        each trailing sum is the window sum of the final-weight-share sleeve, recomputed here with numpy."""
        spy = self.spy["theme-erc-v1"]
        block = self.doc[ct.BLOCK]
        weighted = sorted({self.erc["theme_standardise"]["themes"][i] for i, w in self.erc["weights"].items() if w > 0})
        self.assertEqual(block["themes"], weighted)
        decisions = spy["matrix"].shape[1]
        starts = list(range(254, decisions, 21))
        self.assertGreater(len(starts), 3)
        self.assertEqual([b["from_session"] for b in block["blocks"]], [int(spy["sessions"][j]) for j in starts])
        w = np.array(spy["weights"])
        self.assertEqual([self.erc["weights"][i] for i in spy["ids"]], spy["weights"])  # the parent's final weights
        for t_index, theme in enumerate(weighted):
            members = [k for k, th in enumerate(spy["themes"]) if th == theme and w[k] > 0]
            share = w[members] / w[members].sum()
            r = np.where(spy["mask"], share @ spy["matrix"][members], 0.0)
            for j, b in zip(starts, block["blocks"]):
                want = float(np.sum(r[j - 3 - 252 + 1: j - 3 + 1]))
                self.assertLessEqual(abs(b["trailing"][t_index] - want), 1e-12 * max(1.0, abs(want)), (theme, j))
        prov = self.doc["provenance"]["theme_tsmom"]
        self.assertEqual((prov["blocks"], prov["first_block_decision"], prov["decisions"]), (len(starts), 254, decisions))

    def test_flag_absent_paths_never_touch_the_rule(self):
        boom = unittest.mock.Mock(side_effect=AssertionError("theme-tsmom-v1 reached without its flag"))
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                unittest.mock.patch.object(ct, "schedule", boom), unittest.mock.patch.object(ct, "attach", boom):
            code, _ = fcw.fit(self.fx.args(self.root / "erc-again", **ERC_ARGS))
            std_code, _ = fcw.fit(self.fx.args(self.root / "std-again", **STD_ARGS))
        self.assertEqual((code, std_code), (fcw.EXIT_OK,) * 2)
        boom.assert_not_called()
        self.assertEqual({p.name: p.read_bytes() for p in (self.root / "erc-again").iterdir()}, self.erc_bytes)
        self.assertEqual({p.name: p.read_bytes() for p in (self.root / "std-again").iterdir()}, self.std_bytes)

    def test_refused_outside_its_parents(self):
        cases = [dict(STD_ARGS, composition="ew-theme-v1", theme_tsmom="theme-tsmom-v1"),
                 dict(STD_ARGS, composition="ew-theme-std-aim-v1", theme_tsmom="theme-tsmom-v1"),
                 dict(STD_TSMOM_ARGS, theme_resid="theme-resid-v1")]
        for k, case in enumerate(cases):
            out = self.root / f"refused-{k}"
            with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                    self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **case))
            self.assertIn("theme-tsmom-v1", str(caught.exception))
            self.assertFalse(out.exists())
        args = self.fx.args(self.root / "x", **STD_TSMOM_ARGS)
        with self.assertRaises(ct.TsmomError):
            ct.check_args(args, prior=True, pooled=True)
        self.assertIsNone(fcw.parse_args(self.fx.argv(self.root / "x", "v4-prior-v1")).theme_tsmom)
        self.assertEqual(cte.RULE_ID, "theme-erc-v1")  # the parent rule name the schedule records under --theme-erc


if __name__ == "__main__":
    unittest.main()
