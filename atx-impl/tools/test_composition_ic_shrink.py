"""Synthetic checks for composition_ic_shrink (rule ic-shrink-v1, platform v8 R-10; no real data).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_ic_shrink.py
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
import fit_composition_weights as fcw  # noqa: E402
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)
from test_composition_rules import ref_cap  # noqa: E402  (an independent loop port of the member cap)

# Shared with atx-impl/tests/strategy_ic_shrink_test.cpp (IcShrinkV1.SharedFixtureFractions): both rules must give
# its exact fractions to 1e-15, so the Python fitter equals the C++ rule on it.
FIXTURE = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "ic_shrink_v1.json"
SHRINK_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ic-shrink-v1")
STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")


def fraction(pair) -> float:
    return pair[0] / pair[1]


def fixture() -> tuple[dict, list[str], list[str], list[float]]:
    fx = json.loads(FIXTURE.read_text(encoding="utf-8"))
    rows = fx["members"]
    return fx, [r["id"] for r in rows], [r["theme"] for r in rows], [r["ic"] for r in rows]


def capped(shares: list[float], themes: list[str]) -> list[float]:
    """share / T, then the cap 1/(2T) by the independent loop port (the rule's last step for any shares)."""
    t = len(set(themes))
    w = ref_cap({k: s / t for k, s in enumerate(shares)}, dict(enumerate(themes)), 1.0 / (2 * t))
    return [w[k] for k in range(len(shares))]


def written_rule(themes: list[str], ics: list[float]) -> list[float]:
    """The registration read literally, independent of the module: theme mean, shrunk = .5 mean + .5 ic, floor 0,
    share = floored / theme sum (equal 1/n with no positive value), w = share / T, cap 1/(2T)."""
    mean = {t: sum(x for th, x in zip(themes, ics) if th == t) / themes.count(t) for t in set(themes)}
    floored = [max(0.5 * mean[t] + 0.5 * x, 0.0) for t, x in zip(themes, ics)]
    mass = {t: sum(p for th, p in zip(themes, floored) if th == t) for t in set(themes)}
    shares = [p / mass[t] if mass[t] > 0 else 1.0 / themes.count(t) for t, p in zip(themes, floored)]
    return capped(shares, themes)


class DeclaredRule(unittest.TestCase):
    def test_registered_constants(self):
        self.assertEqual(cis.RULE_ID, "ic-shrink-v1")
        self.assertEqual((cis.INTENSITY, cis.FLOOR, cis.RUNNER_TOLERANCE, cis.MAX_THEMES), (0.5, 0.0, 1e-12, 32))
        self.assertEqual(len(cis.RULE_TEXT), 5)
        self.assertIn("ic-shrink-v1", fcw.COMPOSITIONS)
        self.assertIn("ic-shrink-v1", fcw.PRIOR_COMPOSITIONS)
        self.assertNotIn("ic-shrink-v1", fcw.AIM_RULES)

    def test_shared_fixture_fractions(self):
        """The fixture the C++ rule test reads: shrunk ICs, shares and weights equal its exact fractions to 1e-15."""
        fx, ids, themes, ics = fixture()
        self.assertEqual((fx["intensity"], fx["floor"]), (cis.INTENSITY, cis.FLOOR))
        self.assertEqual(list(dict.fromkeys(themes)), fx["theme_order"])
        fit = cis.ic_shrink(ids, themes, ics)
        shrunk, shares, _, equal = cis.shrunk_shares(themes, ics)
        for k, row in enumerate(fx["members"]):
            self.assertLessEqual(abs(shrunk[k] - fraction(row["shrunk"])), 1e-15, row["id"])
            self.assertLessEqual(abs(shares[k] - fraction(row["share"])), 1e-15, row["id"])
            self.assertLessEqual(abs(float(fit.weights[k]) - fraction(row["weight"])), 1e-15, row["id"])
        self.assertEqual(equal, fx["equal_share_themes"])
        self.assertEqual(fit.provenance["capped_members"], fx["capped"])
        self.assertEqual(len(fit.provenance["cap_iterations"]), fx["cap_passes"])
        self.assertEqual(fit.provenance["cap"], fraction(fx["cap"]))
        self.assertAlmostEqual(float(np.sum(fit.weights)), 1.0, places=15)

    def test_written_rule_and_the_james_stein_reading(self):
        _, ids, themes, ics = fixture()
        fit = cis.ic_shrink(ids, themes, ics)
        want = written_rule(themes, ics)
        for k in range(len(ids)):
            self.assertLessEqual(abs(float(fit.weights[k]) - want[k]), 1e-15, ids[k])
        # A theme without a floored member and with a positive mean: share = .5 / n + .5 * ic / sum ic, the
        # IC-proportional share shrunk halfway toward the equal share.
        _, shares, _, _ = cis.shrunk_shares(themes, ics)
        for theme in ("momentum", "flow"):
            members = [k for k, t in enumerate(themes) if t == theme]
            total = sum(ics[k] for k in members)
            for k in members:
                self.assertLessEqual(abs(shares[k] - (0.5 / len(members) + 0.5 * ics[k] / total)), 1e-15, ids[k])

    def test_fixture_tells_wrong_rules_apart(self):
        """Review T-1: equal within-theme shares, no shrinkage, full shrinkage, no floor and no equal-share fallback
        each move some weight by more than 1e-3 on the fixture."""
        _, ids, themes, ics = fixture()
        fit = [float(w) for w in cis.ic_shrink(ids, themes, ics).weights]

        def gap(other):
            return max(abs(a - b) for a, b in zip(other, fit))

        self.assertGreater(gap(capped([1.0 / themes.count(t) for t in themes], themes)), 1e-3)
        for intensity in (0.0, 1.0):
            self.assertGreater(gap(capped(cis.shrunk_shares(themes, ics, intensity=intensity)[1], themes)), 1e-3)
        shrunk, shares, _, equal = cis.shrunk_shares(themes, ics)
        total = {t: sum(s for th, s in zip(themes, shrunk) if th == t) for t in set(themes)}
        self.assertGreater(gap([s / total[t] / 4 for t, s in zip(themes, shrunk)]), 1e-3)          # no floor
        self.assertGreater(gap(capped([0.0 if t in equal else a for t, a in zip(themes, shares)], themes)), 1e-3)
        self.assertGreater(gap([a / 4 for a in shares]), 1e-3)                                      # no cap

    def test_block_and_attach_are_what_the_runner_reads(self):
        """strategy_ic_admission.cpp: theme_standardise {rule ic-shrink-v1, rerank true, themes of the weighted
        members, ic_shrink {intensity, floor, members: every member, a floored one at weight 0 too}}, schema v2."""
        _, ids, themes, ics = fixture()
        fit = cis.ic_shrink(ids, themes, ics)
        block = fit.block
        self.assertEqual((block["rule"], block["rerank"]), ("ic-shrink-v1", True))
        self.assertEqual(block["themes"], {i: t for i, t, w in zip(ids, themes, fit.weights) if w > 0})
        self.assertNotIn("a3", block["themes"])
        self.assertEqual(block["ic_shrink"], {"intensity": 0.5, "floor": 0.0,
                                              "members": {i: {"theme": t, "ic": x} for i, t, x in zip(ids, themes, ics)}})
        self.assertEqual(fit.provenance["floored_members"], ["a3", "c1", "c2", "c3"])
        self.assertEqual(fit.provenance["equal_share_themes"], ["quality"])
        self.assertEqual(fit.provenance["rule"], "ic-shrink-v1")
        doc = cis.attach({"schema": cr.WEIGHTS_SCHEMA_V1, "provenance": {}}, fit)
        self.assertEqual(doc["schema"], "atx.dsl-composition-weights/v2")
        self.assertEqual(doc["theme_standardise"], block)
        self.assertIs(doc["provenance"]["ic_shrink"], fit.provenance)
        json.dumps(doc, allow_nan=False)  # serialisable as the fitter writes it

    def test_refusals(self):
        for args in (([], [], []), (["a"], ["x", "y"], [0.1]), (["a", "a"], ["x", "y"], [0.1, 0.2]),
                     (["a", "b"], ["x", "y"], [0.1, math.nan]), (["a", "b"], ["x", "y"], [0.1, True]),
                     (["a", "b"], ["x", "y"], [0.1, None]),
                     ([f"m{k}" for k in range(33)], [f"t{k}" for k in range(33)], [0.1] * 33),
                     (["a", "b"], ["x", "x"], [0.002, 0.001])):           # one theme: the cap 1/2 has nowhere to go
            with self.assertRaises(cr.RuleError, msg=repr(args)):
                cis.ic_shrink(*args)
        with self.assertRaises(fcw.FitError):  # the fitter's error class is honoured
            cis.ic_shrink(["a"], ["x"], [0.1], error=fcw.FitError)


class FitterEndToEnd(unittest.TestCase):
    """fit_composition_weights --composition ic-shrink-v1 on the fitter's synthetic world (as test_composition_rules)."""

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
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json"):
            cls.std_code, _ = fcw.fit(cls.fx.args(cls.root / "std", **STD_ARGS))
            cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "shrink", **SHRINK_ARGS))
        cls.std_bytes = {p.name: p.read_bytes() for p in (cls.root / "std").iterdir()}
        cls.bytes = {p.name: p.read_bytes() for p in (cls.root / "shrink").iterdir()}
        cls.std = json.loads(cls.std_bytes[fcw.OUTPUT_WEIGHTS])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])
        cls.adm = json.loads(cls.bytes[fcw.OUTPUT_ADMISSION])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def members(self) -> list[str]:
        """The parent rule's member set (admitted, non-degenerate: every one weighted under ew-theme-std-v1), in the
        fitter's admission order."""
        rows = {c["id"]: c for c in self.adm["candidates"]}
        weighted = {i for i, w in self.std["weights"].items() if w > 0}
        return sorted(weighted, key=lambda i: rows[i]["admission_rank"])

    def test_weights_follow_the_rule_on_the_admission_ics(self):
        self.assertEqual((self.code, self.std_code), (fcw.EXIT_OK, fcw.EXIT_OK))
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.std_bytes[fcw.OUTPUT_ADMISSION])  # screen unchanged
        rows = {c["id"]: c for c in self.adm["candidates"]}
        members = self.members()
        self.assertEqual(sorted(members), sorted(["slow_a_clone", "slow_a_twin", "flip", "medium", "medium_half"]))
        want = written_rule([rows[i]["theme"] for i in members], [rows[i]["train_mean"] for i in members])
        w = self.doc["weights"]
        for i in self.ids:
            self.assertLessEqual(abs(w[i] - dict(zip(members, want)).get(i, 0.0)), 1e-15, i)
        self.assertAlmostEqual(sum(w.values()), 1.0, places=15)
        self.assertNotEqual(w, self.std["weights"])                       # the ICs moved weight inside the themes
        self.assertEqual(self.doc["signs"], self.std["signs"])
        self.assertEqual((self.doc["provenance"]["rule"], self.summary["composition"]), ("ic-shrink-v1",) * 2)
        self.assertEqual(self.doc["provenance"]["ic_shrink"]["member_ic"],
                         {i: rows[i]["train_mean"] for i in members})
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_document_is_what_the_runner_reads(self):
        """Schema v2 with the ic-shrink-v1 block; the rule re-applied in library order (the runner's order) gives the
        pinned weights within the runner's 1e-12."""
        rows = {c["id"]: c for c in self.adm["candidates"]}
        members = self.members()
        self.assertEqual(self.doc["schema"], "atx.dsl-composition-weights/v2")
        self.assertNotIn("theme_redistribution", self.doc)
        block = self.doc["theme_standardise"]
        self.assertEqual((block["rule"], block["rerank"]), ("ic-shrink-v1", True))
        self.assertEqual(set(block["themes"]), {i for i, x in self.doc["weights"].items() if x > 0})
        self.assertEqual(block["ic_shrink"]["members"],
                         {i: {"theme": rows[i]["theme"], "ic": rows[i]["train_mean"]} for i in members})
        self.assertEqual((block["ic_shrink"]["intensity"], block["ic_shrink"]["floor"]), (0.5, 0.0))
        library = [i for i in self.ids if i in members]
        again = cis.ic_shrink(library, [block["ic_shrink"]["members"][i]["theme"] for i in library],
                              [block["ic_shrink"]["members"][i]["ic"] for i in library])
        for i, x in zip(library, again.weights):
            self.assertLessEqual(abs(float(x) - self.doc["weights"][i]), cis.RUNNER_TOLERANCE, i)
        self.assertTrue(all(self.doc["weights"][i] == 0.0 for i in self.ids if i not in members))

    def test_flag_absent_paths_never_touch_the_rule(self):
        """Flag-absent identity: with the rule's functions made to fail, ew-theme-std-v1 and ew-theme-v1 fit to the
        same bytes; only --composition ic-shrink-v1 reaches composition_ic_shrink."""
        boom = unittest.mock.Mock(side_effect=AssertionError("ic-shrink-v1 reached without its flag"))
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                unittest.mock.patch.object(cis, "ic_shrink", boom), unittest.mock.patch.object(cis, "attach", boom):
            code, _ = fcw.fit(self.fx.args(self.root / "std-again", **STD_ARGS))
            v1_code, _ = fcw.fit(self.fx.args(self.root / "v1-a", **tfw.V4_ARGS))
        self.assertEqual((code, v1_code), (fcw.EXIT_OK, fcw.EXIT_OK))
        boom.assert_not_called()
        self.assertEqual({p.name: p.read_bytes() for p in (self.root / "std-again").iterdir()}, self.std_bytes)
        v1 = json.loads((self.root / "v1-a" / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertNotIn("theme_standardise", v1)
        self.assertNotIn("ic_shrink", v1["provenance"])


if __name__ == "__main__":
    unittest.main()
