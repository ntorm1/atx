"""--theme-hedge theme-cov-hedge-v1 (P9 lane AL-COMB, cell P9-H; a thin wrapper, PM8-12): the fitter writes only the
spec block on the parent's document (the rule is C++: engine/combine/char_cov_hedge.hpp and
strategy_ic_theme_hedge.{hpp,cpp}, tested by CharCovHedge.* and ThemeHedgeRule.*); the block is the C++ header's
constants and the K-P9-6 rule-table row's; flag absent, the parent's bytes; refused outside a standardised
single-window composition."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import composition_rules as cr  # noqa: E402
import composition_theme_hedge as cth  # noqa: E402
import composition_engine_fit as cef  # noqa: E402
import composition_theme_erc as cte  # noqa: E402
import composition_theme_tsmom as ct  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_composition_engine_fit as tcef  # noqa: E402  (its --engine-fit stand-ins, not its tests)
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)

HEADER = Path(__file__).resolve().parents[1] / "src" / "strategy_ic_theme_hedge.hpp"
RULES = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "composition_rules_list.json"
STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")
ERC_ARGS = dict(STD_ARGS, theme_erc="theme-erc-v1")
HEDGE_ARGS = dict(ERC_ARGS, theme_hedge="theme-cov-hedge-v1")
STACK_ARGS = dict(HEDGE_ARGS, mom_volman="mom-volman-v1")
SPEC = {"rule": "theme-cov-hedge-v1", "lookback": 252, "lag": 2, "step": 21, "min_pairs": 126}


class DeclaredRule(unittest.TestCase):
    def test_constants_match_the_runner_header(self):
        text = HEADER.read_text(encoding="utf-8")
        for name, value in (("lookback", cth.LOOKBACK), ("lag", cth.LAG), ("step", cth.STEP),
                            ("min_pairs", cth.MIN_PAIRS)):
            found = re.search(rf"theme_hedge_{name}\s*=\s*(\d+);", text)
            self.assertIsNotNone(found, name)
            self.assertEqual(int(found.group(1)), value, name)
        self.assertRegex(text, rf'theme_hedge_rule\s*=\s*"{re.escape(cth.RULE_ID)}";')
        self.assertEqual(cth.spec_block(), SPEC)  # registered blind (DMRS 2020 with the house's windows)

    def test_block_is_the_rule_tables_row(self):
        rows = [r for r in json.loads(RULES.read_text(encoding="utf-8"))["rules"] if r["id"] == cth.RULE_ID]
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual((row["block_key"], row["stage"], row["recipe_key"]), (cth.BLOCK, "hedge", "composition_hedge"))
        self.assertEqual(sorted(row["incompatible"]), ["theme-resid-v1", "two-speed-v1"])
        # P9 lane D2's K-P9-9 params form (ruling D1-PSCHEMA): a closed object of the rule key and the registered
        # constants, each optional, typed and const; the spec block the fitter writes is exactly those consts.
        schema = row["params_schema"]
        self.assertEqual((schema["type"], schema["additionalProperties"]), ("object", False))
        self.assertNotIn("required", schema)
        self.assertEqual({k: v["const"] for k, v in schema["properties"].items()}, cth.spec_block())
        self.assertEqual({k: v["type"] for k, v in schema["properties"].items()},
                         {"rule": "string", "lookback": "integer", "lag": "integer", "step": "integer",
                          "min_pairs": "integer"})

    def test_attach_and_its_refusals(self):
        parent = {"weights": {"a": 1.0}, "theme_standardise": {"rerank": True, "themes": ["value"]},
                  "provenance": {"rule": "ew-theme-std-v1"}}
        doc = json.loads(json.dumps(parent))
        self.assertIs(cth.attach(doc), doc)
        self.assertEqual(doc.pop(cth.BLOCK), SPEC)
        self.assertEqual(doc, parent)
        refused = [{"provenance": {}},
                   {"theme_standardise": {"rerank": False}, "provenance": {}},
                   {"theme_standardise": {"rerank": 1}, "provenance": {}},
                   {"theme_standardise": [], "provenance": {}},
                   dict(parent, theme_residualise={"rule": "theme-resid-v1"}),
                   dict(parent, theme_sleeves={"rule": "two-speed-v1"})]
        for case in refused:
            before = json.dumps(case, sort_keys=True)
            with self.assertRaises(cth.HedgeError):
                cth.attach(case)
            self.assertEqual(json.dumps(case, sort_keys=True), before)  # refused before any write
        with self.assertRaises(KeyError):
            cth.attach({"provenance": {}}, error=KeyError)

    def test_check_args(self):
        ok = argparse.Namespace(theme_hedge=cth.RULE_ID, composition="ic-shrink-v1", theme_resid=None, two_speed=None)
        cth.check_args(ok, prior=True, pooled=False)
        cth.check_args(argparse.Namespace(theme_hedge=None), prior=False, pooled=True)  # flag absent: no check
        bad = [(dict(vars(ok), composition="ew-theme-v1"), True, False),
               (dict(vars(ok), composition="ic-shrink-aim-v1"), True, False),
               (dict(vars(ok), theme_resid="theme-resid-v1"), True, False),
               (dict(vars(ok), two_speed="two-speed-v1"), True, False),
               (vars(ok), False, False),
               (vars(ok), True, True)]
        for fields, prior, pooled in bad:
            with self.assertRaises(cth.HedgeError, msg=str((fields, prior, pooled))):
                cth.check_args(argparse.Namespace(**fields), prior=prior, pooled=pooled)
        # Accepted under --engine-fit (P9 lane D2): the block holds only the registered constants.
        cth.check_args(argparse.Namespace(**dict(vars(ok), engine_fit=True)), prior=True, pooled=False)


class FitterEndToEnd(unittest.TestCase):
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
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json"):
            cls.parent_code, _ = fcw.fit(cls.fx.args(cls.root / "erc", **ERC_ARGS))
            cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "hedge", **HEDGE_ARGS))
            cls.stack_code, cls.stack_summary = fcw.fit(cls.fx.args(cls.root / "stack", **STACK_ARGS))
            cls.volman_code, _ = fcw.fit(cls.fx.args(cls.root / "volman", **dict(ERC_ARGS, mom_volman="mom-volman-v1")))
        load = lambda name: {p.name: p.read_bytes() for p in (cls.root / name).iterdir()}  # noqa: E731
        cls.parent_bytes, cls.bytes, cls.stack_bytes = load("erc"), load("hedge"), load("stack")
        cls.volman_bytes = load("volman")
        cls.parent = json.loads(cls.parent_bytes[fcw.OUTPUT_WEIGHTS])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_document_is_the_parent_plus_the_spec_block(self):
        self.assertEqual((self.parent_code, self.code), (fcw.EXIT_OK, fcw.EXIT_OK))
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.parent_bytes[fcw.OUTPUT_ADMISSION])
        rest = json.loads(json.dumps(self.doc))
        self.assertEqual(rest.pop(cth.BLOCK), SPEC)
        self.assertEqual(rest, self.parent)
        self.assertEqual(self.doc["provenance"]["rule"], "theme-erc-v1")
        self.assertEqual(self.summary["theme_hedge"], SPEC)
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_stacks_on_mom_volman(self):
        """--theme-hedge after --mom-volman: the volman document plus the spec block, nothing else changed."""
        self.assertEqual((self.stack_code, self.volman_code), (fcw.EXIT_OK, fcw.EXIT_OK))
        stack = json.loads(self.stack_bytes[fcw.OUTPUT_WEIGHTS])
        self.assertEqual(stack.pop(cth.BLOCK), SPEC)
        self.assertEqual(stack, json.loads(self.volman_bytes[fcw.OUTPUT_WEIGHTS]))
        self.assertEqual(self.stack_summary["theme_hedge"], SPEC)

    def test_flag_absent_paths_never_touch_the_rule(self):
        boom = unittest.mock.Mock(side_effect=AssertionError("theme-cov-hedge-v1 reached without its flag"))
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                unittest.mock.patch.object(cth, "attach", boom), unittest.mock.patch.object(cth, "spec_block", boom):
            code, _ = fcw.fit(self.fx.args(self.root / "erc-again", **ERC_ARGS))
        self.assertEqual(code, fcw.EXIT_OK)
        boom.assert_not_called()
        self.assertEqual({p.name: p.read_bytes() for p in (self.root / "erc-again").iterdir()}, self.parent_bytes)
        self.assertIsNone(fcw.parse_args(self.fx.argv(self.root / "x", "v4-prior-v1")).theme_hedge)
        flagged = self.fx.argv(self.root / "x", "v4-prior-v1", extra=("--theme-hedge", cth.RULE_ID))
        self.assertEqual(fcw.parse_args(flagged).theme_hedge, cth.RULE_ID)

    def test_refused_outside_its_parents(self):
        cases = [dict(STD_ARGS, composition="ew-theme-v1", theme_hedge="theme-cov-hedge-v1"),
                 dict(STD_ARGS, theme_hedge="theme-cov-hedge-v1", theme_resid="theme-resid-v1"),
                 dict(ERC_ARGS, theme_hedge="theme-cov-hedge-v1", two_speed="two-speed-v1")]
        for k, case in enumerate(cases):
            out = self.root / f"refused-{k}"
            with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                    self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **case))
            self.assertIn("theme-cov-hedge-v1", str(caught.exception))
            self.assertFalse(out.exists())
        with self.assertRaises(cth.HedgeError):
            cth.check_args(self.fx.args(self.root / "x", **HEDGE_ARGS), prior=True, pooled=True)


class EngineFit(unittest.TestCase):
    """--theme-hedge under --engine-fit (P9 lane D2's flag, its stand-in executables): the C++-valued document plus the
    same spec block; the admission files and the weights file minus provenance.engine_fit are the flag-absent run's."""

    def test_the_spec_block_rides_on_the_engine_document(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fx = tcef.World.build(root)
            case = dict(STD_ARGS, theme_erc=cte.RULE_ID, theme_tsmom=ct.RULE_ID, theme_hedge=cth.RULE_ID)
            with unittest.mock.patch.object(cr, "REGISTRY_PATH", root / "no-registry.json"):
                plain_code, _ = fcw.fit(fx.args(root / "plain", **case))
                with tcef.StandIns(root / "factors") as stand:
                    code, summary = fcw.fit(fx.args(root / "engine", **case, **tcef.placeholders(root / "in")))
            self.assertEqual((plain_code, code), (fcw.EXIT_OK, fcw.EXIT_OK))
            self.assertEqual(stand.calls, ["screen", "fit"])
            plain, engine = tcef.load(root / "plain"), tcef.load(root / "engine")
            self.assertEqual(sorted(engine), sorted(plain))
            for name in (fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV):
                self.assertEqual(engine[name], plain[name], name)
            self.assertEqual(tcef.without_engine_fit(engine[fcw.OUTPUT_WEIGHTS]), plain[fcw.OUTPUT_WEIGHTS])
            doc = json.loads(engine[fcw.OUTPUT_WEIGHTS])
            self.assertEqual(doc[cth.BLOCK], SPEC)
            self.assertEqual(summary["theme_hedge"], SPEC)
            self.assertEqual(doc["provenance"][cef.PROVENANCE_KEY]["status"], "agree")


if __name__ == "__main__":
    unittest.main()
