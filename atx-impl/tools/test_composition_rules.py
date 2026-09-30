"""Synthetic checks for composition_rules (rule ew-theme-std-v1, platform v8 R-1; no real data).

Run: python -m pytest atx-impl/tools/test_composition_rules.py -q
"""
import contextlib
import hashlib
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import composition_rules as cr  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)

STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")


def ref_cap(weights: dict, themes: dict, cap: float) -> dict:
    """Loop port of rule 4: members above the cap go to the cap; each theme's excess is spread over the other themes'
    uncapped members in proportion to their weights; repeat until none is above the cap."""
    w, done = dict(weights), set()
    while True:
        over = [i for i in w if i not in done and w[i] > cap * (1 + 1e-12)]
        if not over:
            return w
        spill = {}
        for i in over:
            spill[themes[i]] = spill.get(themes[i], 0.0) + w[i] - cap
            w[i] = cap
            done.add(i)
        snapshot = dict(w)
        for t, amount in sorted(spill.items()):
            takers = [i for i in w if i not in done and themes[i] != t]
            total = sum(snapshot[i] for i in takers)
            for i in takers:
                w[i] += amount * snapshot[i] / total


class DeclaredRule(unittest.TestCase):
    def test_declared_constants(self):
        self.assertEqual(cr.STD_RULE_ID, "ew-theme-std-v1")
        self.assertEqual(cr.TIER_REGRADES_V8, (("res_mom_12_1", "B+", "B-"), ("ear", "B+", "C+"), ("sue", "C+", "C+"),
                                               ("ins_opp", "B-", "C+")))
        self.assertEqual(cr.TIER_SCORES_DECLARED, {"A": 1.0, "A-": 0.9, "B+": 0.8, "B": 0.7, "B-": 0.55, "C+": 0.4})
        self.assertEqual(len(cr.RULE_TEXT), 5)
        self.assertIn("ew-theme-std-v1", fcw.COMPOSITIONS)
        self.assertIn("ew-theme-std-v1", fcw.PRIOR_COMPOSITIONS)

    def test_tier_weights_sum_to_theme_share(self):
        themes = ["value", "value", "value", "momentum", "options", "momentum"]
        scores = [1.0, 0.8, 0.4, 0.55, 0.7, 0.9]
        w = cr.tier_weights(themes, scores)
        self.assertAlmostEqual(float(w.sum()), 1.0, places=15)
        for t in set(themes):  # every theme carries 1/T
            self.assertAlmostEqual(sum(x for x, u in zip(w, themes) if u == t), 1 / 3, places=15, msg=t)
        # within a theme the weights are proportional to the tier scores
        self.assertAlmostEqual(w[0] / w[1], 1.0 / 0.8, places=14)
        self.assertAlmostEqual(w[1] / w[2], 0.8 / 0.4, places=14)
        self.assertAlmostEqual(w[5] / w[3], 0.9 / 0.55, places=14)
        self.assertAlmostEqual(w[4], 1 / 3, places=15)  # a one-member theme holds the whole theme share
        # equal scores are ew-theme-v1: 1 / (T * n_theme)
        v1, _ = fcw.ew_theme_weights(themes)
        np.testing.assert_allclose(cr.tier_weights(themes, [0.4] * len(themes)), v1, rtol=0, atol=1e-16)

    def test_member_cap_redistributes(self):
        # T = 2: the one-member theme x is capped from 1/2 to 1/4; its excess goes to theme y (1/6 -> 1/4 each).
        w, passes = cr.member_cap(np.array([0.5, 1 / 6, 1 / 6, 1 / 6]), ["x", "y", "y", "y"], 0.25)
        np.testing.assert_allclose(w, [0.25, 0.25, 0.25, 0.25], rtol=0, atol=1e-15)
        self.assertEqual([p["capped"] for p in passes], [[0]])
        self.assertAlmostEqual(passes[0]["excess_by_theme"]["x"], 0.25, places=15)
        # A cascade, T = 3, cap 1/6: a1 (1/3) spills 1/6 over b (1/6 each -> 5/24) and c (1/12 each -> 5/48); b is now
        # above the cap and spills 1/12 over c alone (its own theme and a are excluded): c -> 1/8 each.
        ids = ["a1", "b1", "b2", "c1", "c2", "c3", "c4"]
        themes = ["a", "b", "b", "c", "c", "c", "c"]
        base = cr.tier_weights(themes, [1.0] * 7)
        cap = 1 / 6
        w, passes = cr.member_cap(base, themes, cap)
        np.testing.assert_allclose(w, [1 / 6, 1 / 6, 1 / 6, 1 / 8, 1 / 8, 1 / 8, 1 / 8], rtol=0, atol=1e-15)
        self.assertEqual([p["capped"] for p in passes], [[0], [1, 2]])
        self.assertAlmostEqual(passes[1]["excess_by_theme"]["b"], 1 / 12, places=15)
        want = ref_cap(dict(zip(ids, base)), dict(zip(ids, themes)), cap)
        for k, i in enumerate(ids):
            self.assertAlmostEqual(w[k], want[i], places=15, msg=i)
        self.assertAlmostEqual(float(w.sum()), 1.0, places=15)
        for k in (0, 1, 2):
            self.assertEqual(w[k], cap)  # a capped member sits exactly at the cap
        # Infeasible: no other theme to take the excess.
        with self.assertRaises(cr.RuleError):
            cr.member_cap(np.array([0.5, 0.5]), ["x", "x"], 0.25)
        with self.assertRaises(cr.RuleError):
            cr.member_cap(np.array([0.5, 0.5]), ["x", "y"], 0.25)

    def test_regrades_are_the_declared_table(self):
        ids = ["res_mom_12_1", "ear", "sue", "ins_opp", "bm"]
        tiers, applied = cr.apply_regrades(ids, ["B+", "B+", "C+", "B-", "B"])
        self.assertEqual(tiers, ["B-", "C+", "C+", "C+", "B"])  # bm unchanged: no other tier changes
        self.assertEqual([a["status"] for a in applied], ["applied", "applied", "declared-unchanged", "applied"])
        # a source already at the target is accepted; a member outside the table is recorded as such
        tiers, applied = cr.apply_regrades(["ear", "bm"], ["C+", "B"])
        self.assertEqual(tiers, ["C+", "B"])
        self.assertEqual({a["id"]: a["status"] for a in applied},
                         {"res_mom_12_1": "not-a-member", "ear": "already-at-target", "sue": "not-a-member",
                          "ins_opp": "not-a-member"})
        with self.assertRaises(cr.RuleError) as caught:
            cr.apply_regrades(["ear"], ["A"])
        self.assertIn("neither the declared B+ nor C+", str(caught.exception))


class RegistryAndFit(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def registry(self, tiers: dict, scores=None) -> Path:
        path = self.tmp / "registry.json"
        path.write_text(json.dumps({"schema": "atx.alpha-registry/v1",
                                    "tier_scores": scores or dict(cr.TIER_SCORES_DECLARED),
                                    "alphas": [{"id": i, "tier": t} for i, t in tiers.items()]}))
        return path

    def test_registry_tiers_first_library_tier_fallback(self):
        ids = ["ear", "sue", "bm", "fresh"]
        themes = ["em", "em", "value", "value"]
        library_tiers = ["A", "A", "A", "B"]  # the registry overrides the first three
        path = self.registry({"ear": "B+", "sue": "C+", "bm": "B"}, scores=dict(cr.TIER_SCORES_DECLARED, B=0.6))
        fit = cr.ew_theme_std(ids, themes, library_tiers, registry_path=path)
        p = fit.provenance
        self.assertEqual(p["member_tier_source"], {"ear": "registry", "sue": "registry", "bm": "registry",
                                                   "fresh": "library-recipe"})
        self.assertEqual(p["member_tiers"], {"ear": "C+", "sue": "C+", "bm": "B", "fresh": "B"})
        self.assertEqual(p["tier_scores_source"], "registry")
        self.assertEqual(p["registry_sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(p["member_scores"]["bm"], 0.6)  # the registry's score table, not the declared one
        # T = 2, cap 1/4: no member above it here (em: .25/.25; value: .25/.25)
        np.testing.assert_allclose(fit.weights, [0.25, 0.25, 0.25, 0.25], rtol=0, atol=1e-15)
        # registry absent: library tiers and the declared scores
        fit = cr.ew_theme_std(ids, themes, ["B+", "C+", "A", "B"], registry_path=self.tmp / "none.json")
        self.assertEqual(fit.provenance["tier_source"], "library-recipe tier field (registry absent)")
        self.assertEqual(fit.provenance["member_scores"], {"ear": 0.4, "sue": 0.4, "bm": 1.0, "fresh": 0.7})

    def test_one_member_theme_is_capped_and_block_names_every_weighted_member(self):
        ids = ["iv", "v1", "v2", "v3", "v4", "m1", "m2", "m3", "m4"]
        themes = ["options"] + ["value"] * 4 + ["momentum"] * 4
        fit = cr.ew_theme_std(ids, themes, ["B", "A", "B+", "B", "C+", "B", "B", "B", "B"],
                              registry_path=self.tmp / "none.json")
        w = dict(zip(ids, fit.weights))
        self.assertAlmostEqual(w["iv"], 1 / 6, places=15)  # 1/T = 1/3 capped at 1/(2T) = 1/6
        self.assertEqual(fit.provenance["capped_members"], ["iv"])
        self.assertAlmostEqual(sum(fit.weights), 1.0, places=15)
        self.assertAlmostEqual(fit.theme_table["options"]["theme_weight"], 1 / 6, places=15)
        # the excess 1/6 went to value and momentum pro rata: each theme weight 1/3 + 1/12
        for t in ("value", "momentum"):
            self.assertAlmostEqual(fit.theme_table[t]["theme_weight"], 1 / 3 + 1 / 12, places=15, msg=t)
            within = fit.theme_table[t]["within_theme_weights"]
            self.assertAlmostEqual(sum(within.values()), 1.0, places=15)
        self.assertAlmostEqual(w["v1"] / w["v2"], 1.0 / 0.8, places=13)  # tier proportions survive the cap
        self.assertEqual(fit.block, {"rule": "ew-theme-std-v1", "rerank": True, "themes": dict(zip(ids, themes))})
        doc = cr.attach_std({"schema": cr.WEIGHTS_SCHEMA_V1, "weights": w, "provenance": {}}, fit)
        self.assertEqual(doc["schema"], cr.WEIGHTS_SCHEMA_V2)
        self.assertEqual(doc["theme_standardise"]["rule"], "ew-theme-std-v1")
        json.dumps(doc, allow_nan=False)  # the fitter's canonical_bytes accepts it

    def test_aim_gain_composes_with_theme_std(self):
        """R-3 on the R-1 rule (Ruling E-27): the aim gains multiply the tier weights inside each theme, renormalised
        so each theme keeps 1/T; the member cap 1/(2T) applies after the gains; gains of 1 are ew-theme-std-v1 bit for
        bit; the runner block stays ew-theme-std-v1's."""
        none = self.tmp / "none.json"
        ids = ["v1", "v2", "v3", "m1", "m2", "o1", "o2"]
        themes = ["value"] * 3 + ["momentum"] * 2 + ["options"] * 2
        tiers = ["A", "B+", "C+", "B", "B", "B", "C+"]
        std = cr.ew_theme_std(ids, themes, tiers, registry_path=none)
        ones = cr.ew_theme_std(ids, themes, tiers, registry_path=none, gains=[1.0] * len(ids))
        self.assertEqual(ones.weights.tobytes(), std.weights.tobytes())                 # bit for bit
        self.assertEqual((ones.block, ones.provenance["weights_before_cap"]), (std.block, std.provenance[
            "weights_before_cap"]))
        gains = [1.0, 0.2, 0.5, 0.05, 1.0, 0.3, 0.9]
        fit = cr.ew_theme_std(ids, themes, tiers, registry_path=none, gains=gains)
        self.assertEqual((fit.block, fit.provenance["rule"]), (std.block, "ew-theme-std-aim-v1"))
        self.assertEqual(fit.provenance["aim_gains"], dict(zip(ids, gains)))
        before, scores, g = fit.provenance["weights_before_cap"], fit.provenance["member_scores"], dict(zip(ids, gains))
        for t in set(themes):                                   # theme shares stay 1/T; within: score x gain
            members = [i for i, u in zip(ids, themes) if u == t]
            self.assertAlmostEqual(sum(before[i] for i in members), 1 / 3, places=15, msg=t)
            total = sum(scores[i] * g[i] for i in members)
            for i in members:
                self.assertAlmostEqual(before[i], scores[i] * g[i] / (3 * total), places=15, msg=i)
        want = ref_cap(before, dict(zip(ids, themes)), 1 / 6)                          # then the cap
        for k, i in enumerate(ids):
            self.assertAlmostEqual(fit.weights[k], want[i], places=15, msg=i)
        self.assertAlmostEqual(float(fit.weights.sum()), 1.0, places=15)
        self.assertIn("m1", std.provenance["capped_members"])                           # the cap sees the gains:
        self.assertNotIn("m1", fit.provenance["capped_members"])                        # m1 (gain .05) falls below it
        self.assertEqual(fit.theme_table["momentum"]["aim_gains"], {"m1": 0.05, "m2": 1.0})
        self.assertNotIn("aim_gains", std.theme_table["momentum"])                      # ew-theme-std-v1 unchanged
        for bad in ([1.0] * 6, [0.0] + [1.0] * 6, [float("nan")] + [1.0] * 6):
            with self.assertRaises(cr.RuleError):
                cr.ew_theme_std(ids, themes, tiers, registry_path=none, gains=bad)

    def test_unscored_tier_and_bad_inputs_refused(self):
        none = self.tmp / "none.json"
        with self.assertRaises(cr.RuleError) as caught:
            cr.ew_theme_std(["a", "b", "c"], ["x", "x", "y"], ["C", "B", "B"], registry_path=none)
        self.assertIn("without a declared score", str(caught.exception))
        with self.assertRaises(fcw.FitError):  # the fitter's error class is honoured
            cr.ew_theme_std(["a"], ["x", "y"], ["B"], registry_path=none, error=fcw.FitError)
        with self.assertRaises(cr.RuleError):  # one theme: the cap 1/2 has nowhere to go
            cr.ew_theme_std(["a"], ["x"], ["B"], registry_path=none)


class FitterEndToEnd(unittest.TestCase):
    """fit_composition_weights --composition ew-theme-std-v1 on the fitter's synthetic world (registry absent)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = tfw.aim_world()
        extra = dict(extra)
        extra["flip"] = dict(extra["flip"], theme="value")           # two themes, five admitted members: M > 2T
        extra["medium_half"] = dict(extra["medium_half"], tier="C+")  # a scored grade
        cls.ids = ids
        cls.fx = tfw.Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                             candidate_extra=extra)
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json"):
            cls.v1_code, _ = fcw.fit(cls.fx.args(cls.root / "v1", **tfw.V4_ARGS))
            cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "std", **STD_ARGS))
            cls.aim_code, cls.aim_summary = fcw.fit(cls.fx.args(cls.root / "aim", **tfw.AIM_ARGS))
            cls.std_aim_code, cls.std_aim_summary = fcw.fit(cls.fx.args(cls.root / "std-aim", **dict(
                STD_ARGS, composition="ew-theme-std-aim-v1")))
        cls.v1_bytes = {p.name: p.read_bytes() for p in (cls.root / "v1").iterdir()}
        cls.bytes = {p.name: p.read_bytes() for p in (cls.root / "std").iterdir()}
        cls.v1 = json.loads(cls.v1_bytes[fcw.OUTPUT_WEIGHTS])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])
        cls.adm = json.loads(cls.bytes[fcw.OUTPUT_ADMISSION])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_weights_follow_the_rule_on_the_admitted_members(self):
        self.assertEqual((self.code, self.v1_code), (fcw.EXIT_OK, fcw.EXIT_OK))
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.v1_bytes[fcw.OUTPUT_ADMISSION])  # screen unchanged
        members = [i for i in self.ids if self.v1["weights"][i] > 0]
        self.assertEqual(sorted(members), sorted(["slow_a_clone", "slow_a_twin", "flip", "medium", "medium_half"]))
        rows = {c["id"]: c for c in self.adm["candidates"]}
        themes = {i: rows[i]["theme"] for i in members}
        tiers = {"slow_a_clone": 1.0, "slow_a_twin": 0.8, "flip": 0.4, "medium": 0.7, "medium_half": 0.4}
        # independent: tier shares of 1/T, then the cap loop port
        order = [i for i in self.ids if i in members]
        total = {t: sum(tiers[i] for i in order if themes[i] == t) for t in set(themes.values())}
        base = {i: tiers[i] / (2 * total[themes[i]]) for i in order}
        want = ref_cap(base, themes, 0.25)
        w = self.doc["weights"]
        for i in self.ids:
            self.assertAlmostEqual(w[i], want.get(i, 0.0), places=15, msg=i)
        self.assertAlmostEqual(sum(w.values()), 1.0, places=15)
        self.assertEqual(self.doc["signs"], self.v1["signs"])
        self.assertEqual(self.doc["provenance"]["rule"], "ew-theme-std-v1")
        self.assertEqual(self.doc["provenance"]["std"]["capped_members"], sorted(["medium", "slow_a_clone"],
                                                                                 key=order.index))
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_std_aim_fit_is_the_std_rule_on_the_aim_gains(self):
        """--composition ew-theme-std-aim-v1: the same admission; member weights = ew_theme_std with the members' aim
        gains (the ew-theme-aim-v1 fit's gains, same records); the runner block and schema are ew-theme-std-v1's."""
        self.assertEqual((self.std_aim_code, self.aim_code), (fcw.EXIT_OK, fcw.EXIT_OK))
        out = self.root / "std-aim"
        doc = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
        aim = json.loads((self.root / "aim" / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertEqual((out / fcw.OUTPUT_ADMISSION).read_bytes(), self.bytes[fcw.OUTPUT_ADMISSION])
        self.assertEqual((doc["schema"], doc["theme_standardise"]), (self.doc["schema"], self.doc["theme_standardise"]))
        self.assertEqual((doc["provenance"]["rule"], self.std_aim_summary["composition"]),
                         ("ew-theme-std-aim-v1", "ew-theme-std-aim-v1"))
        gains = aim["provenance"]["aim"]["gain"]
        self.assertEqual(doc["provenance"]["aim"]["gain"], gains)                     # the same gains (report block)
        members = [i for i in self.ids if self.doc["weights"][i] > 0]
        rows = {c["id"]: c for c in self.adm["candidates"]}
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"):
            want = cr.ew_theme_std(members, [rows[i]["theme"] for i in members], [rows[i]["tier"] for i in members],
                                   gains=[gains[i] for i in members])
        self.assertEqual(doc["weights"], {i: float(dict(zip(members, want.weights)).get(i, 0.0)) for i in self.ids})
        self.assertEqual(doc["provenance"]["std"]["aim_gains"], {i: gains[i] for i in members})
        self.assertNotEqual(doc["weights"], self.doc["weights"])                      # the gains moved weight
        self.assertEqual(self.std_aim_summary["aim_theme_weights"],
                         {t: e["theme_weight"] for t, e in sorted(doc["provenance"]["themes"].items())})
        self.assertNotIn("aim", self.doc["provenance"])                              # ew-theme-std-v1 unchanged
        self.assertNotIn("aim_gains", self.doc["provenance"]["std"])

    def test_document_is_what_the_runner_reads(self):
        # strategy_ic_admission.cpp: schema v2 iff one theme block; theme_standardise {rule, rerank bool, themes}
        # naming every weighted member with a theme.
        self.assertEqual(self.doc["schema"], "atx.dsl-composition-weights/v2")
        self.assertNotIn("theme_redistribution", self.doc)
        block = self.doc["theme_standardise"]
        self.assertEqual((block["rule"], block["rerank"]), ("ew-theme-std-v1", True))
        weighted = {i for i, x in self.doc["weights"].items() if x > 0}
        self.assertEqual(set(block["themes"]), weighted)
        self.assertEqual(set(block["themes"].values()), {"value", "reversal_seasonality"})
        self.assertEqual(self.v1["schema"], "atx.dsl-composition-weights/v1")
        self.assertNotIn("theme_standardise", self.v1)

    def test_identity_weights_graft_onto_the_ew_theme_v1_file(self):
        src = self.root / "v1" / fcw.OUTPUT_WEIGHTS
        out = self.root / "identity.json"
        argv = ["identity-weights", "--weights", str(src), "--weights-sha256", hashlib.sha256(src.read_bytes()).hexdigest(),
                "--out", str(out)]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cr.main(argv), 0)
            self.assertEqual(cr.main(argv), 1)  # never overwritten
        doc = json.loads(out.read_bytes())
        self.assertEqual(doc["schema"], "atx.dsl-composition-weights/v2")
        self.assertEqual((doc["weights"], doc["signs"]), (self.v1["weights"], self.v1["signs"]))
        self.assertEqual(doc["theme_standardise"]["rerank"], False)
        weighted = {i for i, x in self.v1["weights"].items() if x > 0}
        self.assertEqual(set(doc["theme_standardise"]["themes"]), weighted)
        self.assertEqual(doc["train_manifest_sha256"], self.v1["train_manifest_sha256"])
        bad = list(argv)
        bad[bad.index("--out") + 1] = str(self.root / "other.json")
        bad[bad.index("--weights-sha256") + 1] = "0" * 64
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cr.main(bad), 1)
        with self.assertRaises(cr.RuleError):  # only an ew-theme-v1 document
            cr.identity_document(self.doc)
        self.assertFalse(math.isnan(sum(doc["weights"].values())))


if __name__ == "__main__":
    unittest.main()
