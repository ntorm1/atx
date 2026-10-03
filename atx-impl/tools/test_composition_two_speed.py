"""--two-speed two-speed-v1 (platform v8 Y-5, lane YCOMB; Ruling PM8-12: a thin wrapper): the fitter writes only the
spec block on the parent's document (the rule is C++: strategy_two_speed.hpp, strategy_ic_two_speed.cpp,
engine/book/two_speed.hpp, tested by TwoSpeed.*, TwoSpeedRunner.* and BookTwoSpeed.*); flag absent, the parent's
bytes; refused outside a standardised single-window composition."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import composition_rules as cr  # noqa: E402
import composition_two_speed as cts  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)

STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")
ERC_ARGS = dict(STD_ARGS, theme_erc="theme-erc-v1")
TWO_ARGS = dict(ERC_ARGS, two_speed="two-speed-v1")


class FitterEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = tfw.aim_world()
        extra = dict(extra)
        extra["flip"] = dict(extra["flip"], theme="value")
        extra["medium_half"] = dict(extra["medium_half"], tier="C+")
        cls.fx = tfw.Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                             candidate_extra=extra)
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json"):
            cls.parent_code, _ = fcw.fit(cls.fx.args(cls.root / "erc", **ERC_ARGS))
            cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "two", **TWO_ARGS))
        load = lambda name: {p.name: p.read_bytes() for p in (cls.root / name).iterdir()}  # noqa: E731
        cls.parent_bytes, cls.bytes = load("erc"), load("two")
        cls.parent = json.loads(cls.parent_bytes[fcw.OUTPUT_WEIGHTS])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_document_is_the_parent_plus_the_spec_block(self):
        self.assertEqual((self.parent_code, self.code), (fcw.EXIT_OK, fcw.EXIT_OK))
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.parent_bytes[fcw.OUTPUT_ADMISSION])
        rest = json.loads(json.dumps(self.doc))
        self.assertEqual(rest.pop(cts.BLOCK), {"rule": "two-speed-v1"})
        self.assertEqual(rest, self.parent)
        self.assertEqual(self.doc["provenance"]["rule"], "theme-erc-v1")
        self.assertEqual(self.summary["two_speed"], {"rule": "two-speed-v1"})
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_flag_absent_paths_never_touch_the_rule(self):
        boom = unittest.mock.Mock(side_effect=AssertionError("two-speed-v1 reached without its flag"))
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                unittest.mock.patch.object(cts, "attach", boom):
            code, _ = fcw.fit(self.fx.args(self.root / "erc-again", **ERC_ARGS))
        self.assertEqual(code, fcw.EXIT_OK)
        boom.assert_not_called()
        self.assertEqual({p.name: p.read_bytes() for p in (self.root / "erc-again").iterdir()}, self.parent_bytes)

    def test_refused_outside_its_parents(self):
        cases = [dict(STD_ARGS, composition="ew-theme-v1", two_speed="two-speed-v1"),
                 dict(STD_ARGS, two_speed="two-speed-v1", theme_resid="theme-resid-v1")]
        for k, case in enumerate(cases):
            out = self.root / f"refused-{k}"
            with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"), \
                    self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **case))
            self.assertIn("two-speed-v1", str(caught.exception))
            self.assertFalse(out.exists())
        with self.assertRaises(cts.TwoSpeedError):
            cts.check_args(self.fx.args(self.root / "x", **TWO_ARGS), prior=True, pooled=True)
        with self.assertRaises(cts.TwoSpeedError):
            cts.attach({"theme_standardise": {"rerank": False}, "provenance": {}})
        self.assertIsNone(fcw.parse_args(self.fx.argv(self.root / "x", "v4-prior-v1")).two_speed)


if __name__ == "__main__":
    unittest.main()
