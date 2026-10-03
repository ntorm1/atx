"""mom-volman-v1 (P9 lane AL-COMB, cell P9-M): the registered constants (pinned against the C++ header and the K-P9-6
rule-table fixture), the recorded sleeve (theme-tsmom-v1's step 1 for price_momentum, nothing new computed here: the
sigma, target and scale are C++, tested by MomVolman.*), the refusals, and the fitter end to end on its synthetic world
(the parent's document plus the block, nothing else; flag absent, the parent's bytes)."""
from __future__ import annotations

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import composition_mom_volman as cmv  # noqa: E402
import composition_rules as cr  # noqa: E402
import composition_theme_tsmom as ct  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)

HEADER = Path(__file__).resolve().parents[1] / "src" / "strategy_ic_mom_volman.hpp"
RULES = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "composition_rules_list.json"
STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")
ERC_ARGS = dict(STD_ARGS, theme_erc="theme-erc-v1")
VOLMAN_ARGS = dict(ERC_ARGS, mom_volman="mom-volman-v1")
STD_VOLMAN_ARGS = dict(STD_ARGS, mom_volman="mom-volman-v1")


def rule_row(rule_id: str) -> dict:
    rows = [r for r in json.loads(RULES.read_text(encoding="utf-8"))["rules"] if r["id"] == rule_id]
    assert len(rows) == 1, rule_id
    return rows[0]


class DeclaredRule(unittest.TestCase):
    def test_constants_match_the_runner_header(self):
        text = HEADER.read_text(encoding="utf-8")
        for name, value in (("lookback", cmv.LOOKBACK), ("lag", cmv.LAG), ("step", cmv.STEP),
                            ("max_decisions", cmv.MAX_DECISIONS)):
            found = re.search(rf"mom_volman_{name}\s*=\s*(\d+);", text)
            self.assertIsNotNone(found, name)
            self.assertEqual(int(found.group(1)), value, name)
        self.assertRegex(text, rf'mom_volman_rule\s*=\s*"{re.escape(cmv.RULE_ID)}";')
        self.assertRegex(text, rf'mom_volman_theme\s*=\s*"{cmv.THEME}";')
        self.assertEqual((cmv.LOOKBACK, cmv.LAG, cmv.STEP), (126, 2, 21))  # registered blind (BSC's six months)

    def test_block_is_the_rule_tables_row(self):
        """The K-P9-6 fixture row: the same block key, every const the module's, every required key written."""
        row = rule_row(cmv.RULE_ID)
        self.assertEqual((row["block_key"], row["stage"]), (cmv.BLOCK, "schedule"))
        self.assertEqual(sorted(row["incompatible"]), ["theme-resid-v1", "theme-tsmom-v1"])
        schema = row["params_schema"]
        self.assertFalse(schema["additionalProperties"])
        consts = {k: v["const"] for k, v in schema["properties"].items() if "const" in v}
        self.assertEqual(consts, {"rule": cmv.RULE_ID, "theme": cmv.THEME, "lookback": cmv.LOOKBACK, "lag": cmv.LAG,
                                  "step": cmv.STEP})
        for key in ("sessions", "sleeve"):
            self.assertEqual((schema["properties"][key]["minItems"], schema["properties"][key]["maxItems"]),
                             (cmv.LOOKBACK + cmv.LAG, cmv.MAX_DECISIONS), key)
        block = cmv.schedule(*Kernel.world(), parent_rule="ew-theme-std-v1").block
        self.assertEqual(sorted(block), sorted(schema["required"]))
        self.assertEqual(sorted(block), sorted(schema["properties"]))


class Kernel(unittest.TestCase):
    @staticmethod
    def world(decisions: int = 150):
        rng = np.random.default_rng(5)
        ids = ["m1", "v1", "m2", "m3"]
        themes = ["price_momentum", "value", "price_momentum", "price_momentum"]
        weights = [0.2, 0.5, 0.3, 0.0]
        matrix = rng.normal(scale=0.01, size=(4, decisions))
        sessions = np.arange(decisions, dtype=np.int64) * 3 + 1000
        mask = np.ones(decisions, dtype=bool)
        mask[decisions - 7:] = False
        return ids, themes, weights, matrix, sessions, mask

    def test_sleeve_is_tsmoms_step_one_for_price_momentum(self):
        """The recorded sleeve is composition_theme_tsmom.sleeves' price_momentum row, bit for bit: the weighted
        members' final-weight shares (a zero weight is not a member), 0 outside the mask; sessions as integers."""
        ids, themes, weights, matrix, sessions, mask = self.world()
        fit = cmv.schedule(ids, themes, weights, matrix, sessions, mask, "theme-erc-v1")
        want = ct.sleeves(matrix, themes, weights, [cmv.THEME], mask)[0]
        self.assertEqual(fit.block["sleeve"], [float(x) for x in want])
        by_hand = np.where(mask, (0.2 / 0.5) * matrix[0] + (0.3 / 0.5) * matrix[2], 0.0)
        self.assertLessEqual(float(np.max(np.abs(np.array(fit.block["sleeve"]) - by_hand))), 1e-15)
        self.assertEqual(fit.block["sessions"], [int(s) for s in sessions])
        self.assertTrue(all(type(s) is int for s in fit.block["sessions"]))
        self.assertEqual((fit.block["rule"], fit.block["theme"], fit.block["lookback"], fit.block["lag"],
                          fit.block["step"]), (cmv.RULE_ID, "price_momentum", 126, 2, 21))
        self.assertEqual((fit.provenance["parent_rule"], fit.provenance["decisions"], fit.provenance["train_decisions"]),
                         ("theme-erc-v1", 150, 143))
        self.assertEqual(fit.summary, {"parent_rule": "theme-erc-v1", "decisions": 150})
        self.assertEqual(fit.provenance["module_sha256"], cmv.module_sha256())

    def test_refusals(self):
        ids, themes, weights, matrix, sessions, mask = self.world()
        cases = {
            "no weighted momentum member": (ids, ["value"] * 4, weights, matrix, sessions, mask),
            "momentum weight zero": (ids, themes, [0.0, 1.0, 0.0, 0.0], matrix, sessions, mask),
            "negative weight": (ids, themes, [0.2, -0.5, 0.3, 0.0], matrix, sessions, mask),
            "nan weight": (ids, themes, [0.2, float("nan"), 0.3, 0.0], matrix, sessions, mask),
            "one series short": (ids[:3], themes[:3], weights[:3], matrix, sessions, mask),
            "no members": ([], [], [], matrix[:0], sessions, mask),
            "session count": (ids, themes, weights, matrix, sessions[:-1], mask),
            "mask count": (ids, themes, weights, matrix, sessions, mask[:-1]),
            "sessions repeat": (ids, themes, weights, matrix, np.concatenate([sessions[:1], sessions[:-1]]), mask),
            "too few decisions": (ids, themes, weights, matrix[:, :127], sessions[:127], mask[:127]),
        }
        for name, case in cases.items():
            with self.assertRaises(cmv.VolmanError, msg=name):
                cmv.schedule(*case, parent_rule="ew-theme-std-v1")
        with self.assertRaises(KeyError):  # the fitter's own error type is passed through
            cmv.schedule(ids, ["value"] * 4, weights, matrix, sessions, mask, "ew-theme-std-v1", error=KeyError)
        exact = cmv.schedule(ids, themes, weights, matrix[:, :128], sessions[:128], mask[:128], "ew-theme-std-v1")
        self.assertEqual(len(exact.block["sleeve"]), 128)  # the shortest role with one block (j = 127)

    def test_attach_adds_the_block_and_provenance_only(self):
        fit = cmv.schedule(*self.world(), parent_rule="ew-theme-std-v1")
        doc = {"weights": {"m1": 0.5}, "provenance": {"rule": "ew-theme-std-v1"}}
        out = cmv.attach(doc, fit)
        self.assertIs(out, doc)
        self.assertEqual(sorted(doc), ["provenance", "theme_schedule", "weights"])
        self.assertEqual(doc["theme_schedule"], fit.block)
        self.assertIsNot(doc["theme_schedule"], fit.block)
        self.assertEqual(doc["provenance"], {"rule": "ew-theme-std-v1", "mom_volman": fit.provenance})


class FitterEndToEnd(unittest.TestCase):
    """fit_composition_weights --mom-volman mom-volman-v1 on the fitter's synthetic world (as test_composition_theme_tsmom)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = tfw.aim_world()
        extra = dict(extra)
        extra["flip"] = dict(extra["flip"], theme="value")
        extra["medium_half"] = dict(extra["medium_half"], tier="C+", theme="price_momentum")
        extra["medium"] = dict(extra["medium"], theme="price_momentum")  # the weighted momentum members
        cls.fx = tfw.Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                             candidate_extra=extra)
        cls.spy = {}
        real = cmv.schedule

        def spy(ids_, themes, weights, matrix, sessions, mask, parent_rule, error=cmv.VolmanError):
            cls.spy.setdefault(parent_rule, dict(ids=list(ids_), themes=list(themes), weights=[float(w) for w in weights],
                                                 matrix=np.array(matrix), sessions=np.array(sessions),
                                                 mask=np.array(mask)))
            return real(ids_, themes, weights, matrix, sessions, mask, parent_rule, error)

        with unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json"):
            cls.erc_code, _ = fcw.fit(cls.fx.args(cls.root / "erc", **ERC_ARGS))
            cls.std_code, _ = fcw.fit(cls.fx.args(cls.root / "std", **STD_ARGS))
            with unittest.mock.patch.object(cmv, "schedule", spy):
                cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "volman", **VOLMAN_ARGS))
                cls.std_volman_code, _ = fcw.fit(cls.fx.args(cls.root / "std-volman", **STD_VOLMAN_ARGS))
        load = lambda name: {p.name: p.read_bytes() for p in (cls.root / name).iterdir()}  # noqa: E731
        cls.erc_bytes, cls.std_bytes = load("erc"), load("std")
        cls.bytes, cls.std_volman_bytes = load("volman"), load("std-volman")
        cls.erc = json.loads(cls.erc_bytes[fcw.OUTPUT_WEIGHTS])
        cls.std = json.loads(cls.std_bytes[fcw.OUTPUT_WEIGHTS])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])
        cls.std_doc = json.loads(cls.std_volman_bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_document_is_the_parent_plus_the_block(self):
        """Weights, signs, theme_standardise and provenance.rule are the parent's; only the block and
        provenance.mom_volman are added; the admission is the parent's."""
        self.assertEqual((self.code, self.erc_code, self.std_code, self.std_volman_code), (fcw.EXIT_OK,) * 4)
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.erc_bytes[fcw.OUTPUT_ADMISSION])
        for doc, parent, rule in ((self.doc, self.erc, "theme-erc-v1"), (self.std_doc, self.std, "ew-theme-std-v1")):
            rest = json.loads(json.dumps(doc))
            block = rest.pop(cmv.BLOCK)
            prov = rest["provenance"].pop("mom_volman")
            self.assertEqual(rest, parent)
            self.assertEqual(doc["provenance"]["rule"], rule)
            self.assertEqual(prov["parent_rule"], rule)
            self.assertEqual((block["rule"], block["theme"], block["lookback"], block["lag"], block["step"]),
                             (cmv.RULE_ID, "price_momentum", 126, 2, 21))
        self.assertEqual(self.summary["mom_volman"]["parent_rule"], "theme-erc-v1")
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_sleeve_is_the_parents_final_weight_momentum_sleeve(self):
        """The members are the parent's active members with its final weights; the recorded sleeve is the price_momentum
        members' final-weight-share sum of their signed series, 0 outside TRAIN, one entry per decision session."""
        for rule, doc, parent in (("theme-erc-v1", self.doc, self.erc), ("ew-theme-std-v1", self.std_doc, self.std)):
            spy = self.spy[rule]
            self.assertEqual([parent["weights"][i] for i in spy["ids"]], spy["weights"])
            block = doc[cmv.BLOCK]
            self.assertEqual(block["sessions"], [int(s) for s in spy["sessions"]])
            w = np.array(spy["weights"])
            members = [k for k, th in enumerate(spy["themes"]) if th == "price_momentum" and w[k] > 0]
            self.assertTrue(members, rule)
            share = w[members] / w[members].sum()
            want = np.where(spy["mask"], share @ spy["matrix"][members], 0.0)
            got = np.array(block["sleeve"])
            self.assertEqual(got.shape, want.shape)
            self.assertLessEqual(float(np.max(np.abs(got - want))), 1e-12 * max(1.0, float(np.max(np.abs(want)))))
            self.assertGreater(len(block["sleeve"]), cmv.LOOKBACK + cmv.LAG + 2 * cmv.STEP)  # several blocks

    def test_flag_absent_paths_never_touch_the_rule(self):
        boom = unittest.mock.Mock(side_effect=AssertionError("mom-volman-v1 reached without its flag"))
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                unittest.mock.patch.object(cmv, "schedule", boom), unittest.mock.patch.object(cmv, "attach", boom):
            code, _ = fcw.fit(self.fx.args(self.root / "erc-again", **ERC_ARGS))
        self.assertEqual(code, fcw.EXIT_OK)
        boom.assert_not_called()
        self.assertEqual({p.name: p.read_bytes() for p in (self.root / "erc-again").iterdir()}, self.erc_bytes)
        self.assertIsNone(fcw.parse_args(self.fx.argv(self.root / "x", "v4-prior-v1")).mom_volman)
        flagged = self.fx.argv(self.root / "x", "v4-prior-v1", extra=("--mom-volman", cmv.RULE_ID))
        self.assertEqual(fcw.parse_args(flagged).mom_volman, cmv.RULE_ID)

    def test_refused_outside_its_parents(self):
        cases = [dict(STD_ARGS, composition="ew-theme-v1", mom_volman="mom-volman-v1"),
                 dict(STD_ARGS, mom_volman="mom-volman-v1", theme_resid="theme-resid-v1"),
                 dict(ERC_ARGS, mom_volman="mom-volman-v1", theme_tsmom="theme-tsmom-v1")]
        for k, case in enumerate(cases):
            out = self.root / f"refused-{k}"
            with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                    self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **case))
            self.assertIn("mom-volman-v1", str(caught.exception))
            self.assertFalse(out.exists())
        args = self.fx.args(self.root / "x", **VOLMAN_ARGS)
        with self.assertRaises(cmv.VolmanError):
            cmv.check_args(args, prior=True, pooled=True)
        with self.assertRaises(cmv.VolmanError):
            cmv.check_args(args, prior=False, pooled=False)
        cmv.check_args(args, prior=True, pooled=False)
        cmv.check_args(self.fx.args(self.root / "x", **ERC_ARGS), prior=False, pooled=True)  # flag absent: no check


if __name__ == "__main__":
    unittest.main()
