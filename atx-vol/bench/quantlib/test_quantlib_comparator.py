"""Exact-clock and official-regression gates for the isolated QuantLib comparator."""
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import quantlib_comparator as comparator


class ClockContractTests(unittest.TestCase):
    def test_receipt_cannot_disguise_failed_prices_or_boolean_model_values(self):
        comparator.check_price_status(0., "finite")
        comparator.check_price_status(None, "unsupported contract")
        for price, status in [(1., "failed"), (None, "finite"), (False, "finite"),
                              (float("nan"), "nonfinite"), (None, ""), (None, True)]:
            with self.assertRaises(ValueError):
                comparator.check_price_status(price, status)
        self.assertFalse(comparator.exact_json({"r": False}, {"r": 0.}))
        self.assertFalse(comparator.exact_json({"explicit": 0}, {"explicit": False}))

    def test_date_conversion_must_preserve_requested_time_discount_and_variance(self):
        raw = dict(S=100., K=100., T=.25, sigma=.2, r=.03, q=.01)
        actual = dict(T=.25, r=.03, q=.01, variance=.01,
                      discount=math.exp(-.03 * .25), dividend_discount=math.exp(-.01 * .25))
        self.assertTrue(comparator.check_clock(raw, actual))
        for field, value in [("T", 91 / 365), ("r", .031), ("variance", .04),
                             ("discount", math.exp(-.03)), ("q", True)]:
            changed = dict(actual, **{field: value})
            self.assertFalse(comparator.check_clock(raw, changed), field)
        changed = dict(actual)
        changed.pop("variance")
        self.assertFalse(comparator.check_clock(raw, changed))

    def test_source_tree_digest_detects_unlisted_header_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "ql").mkdir()
            header = root / "ql" / "a.hpp"
            header.write_text("initial")
            (root / "LICENSE.TXT").write_text("test license")
            digest = comparator.source_tree_digest(root)
            comparator.verify_source(root, {"tree_sha256": digest})
            header.write_text("changed")
            with self.assertRaises(ValueError):
                comparator.verify_source(root, {"tree_sha256": digest})


@unittest.skipUnless(os.environ.get("ATX_QUANTLIB_COMPARATOR_BINARY"), "explicit native binary required")
class NativeQuantLibTests(unittest.TestCase):
    def test_common_corpus_has_exact_model_join_and_charged_reuse(self):
        corpus_path = Path(__file__).resolve().parents[1] / "public-american" / "corpus-common-v1.json"
        corpus = json.loads(corpus_path.read_text())
        corpus["groups"] = corpus["groups"][:2]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "corpus.json").write_text(json.dumps(corpus))
            result = subprocess.run([os.environ["ATX_QUANTLIB_COMPARATOR_BINARY"], "--input",
                str(root / "corpus.json"), "--output", str(root / "native.json"), "--reps", "2"],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            native = json.loads((root / "native.json").read_text())
            self.assertEqual(native["case_count"], 14)
            self.assertEqual(len(native["cases"]), 14)
            self.assertEqual(len(native["references"]), 14)
            self.assertEqual(len(native["effective_models"]), 2)
            for group in native["effective_models"]:
                self.assertTrue(comparator.check_clock(group["input"], group["effective"]))
            self.assertEqual({e["name"] for e in native["engines"]},
                             {"ql_fast", "ql_accurate", "atx_default", "atx_bulk_fast",
                              "atx_accurate_fast", "atx_screen"})
            summary = comparator.summarize(native, corpus)
            self.assertFalse(summary["general_american_independent_method_reference"])
            self.assertTrue(all(e["attempted"] == 14 for e in summary["engines"]))
            for key in ["cases", "effective_models", "self_test"]:
                changed = copy.deepcopy(native)
                changed.pop(key)
                with self.assertRaises(ValueError):
                    comparator.summarize(changed, corpus)
            for field, value in [("options", {}), ("cold_status", ["failed"] * 14),
                                 ("reused_status", [True] * 14)]:
                changed = copy.deepcopy(native)
                changed["engines"][0][field] = value
                with self.assertRaises(ValueError, msg=field):
                    comparator.summarize(changed, corpus)
            changed = copy.deepcopy(native)
            changed["references"][0]["quantlib_status"] = "failed"
            with self.assertRaises(ValueError):
                comparator.summarize(changed, corpus)
            changed = copy.deepcopy(native)
            changed["engines"][0]["cold_prices"][0] = True
            changed["engines"][0]["reused_prices"][0] = True
            with self.assertRaises(ValueError):
                comparator.summarize(changed, corpus)
            changed = copy.deepcopy(native)
            changed["reference_options"]["atx_refined"]["tol"] = .1
            with self.assertRaises(ValueError):
                comparator.summarize(changed, corpus)
            changed = copy.deepcopy(native)
            changed["engines"][0].pop("cold_digests")
            with self.assertRaises(ValueError):
                comparator.summarize(changed, corpus)
            for engine in native["engines"]:
                self.assertEqual(len(engine["cold_prices"]), 14)
                self.assertEqual(engine["cold_prices"], engine["reused_prices"])
                self.assertEqual(len(engine["cold_s"]), 2)
                self.assertTrue(all(total >= setup + evaluation for total, setup, evaluation in zip(
                    engine["reuse_total_s"], engine["reuse_setup_s"], engine["reuse_eval_s"])))
                self.assertEqual(engine["reuse_kind"], "engine-market-scheme; boundary recomputed per spot"
                    if engine["name"].startswith("ql_") else "solved boundary across spots")

    def test_official_premiums_and_exact_time_adapter_before_benchmark(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "official.json"
            result = subprocess.run([os.environ["ATX_QUANTLIB_COMPARATOR_BINARY"], "--self-test",
                "--output", str(output)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output.read_text())
            comparator.check_self_test(report)
            for group in ["official_checks", "clock_checks", "european_checks"]:
                changed = copy.deepcopy(report)
                changed[group][0]["input"]["K"] = 101.
                with self.assertRaises(ValueError, msg=group):
                    comparator.check_self_test(changed)
                changed = copy.deepcopy(report)
                changed[group][0] = copy.deepcopy(changed[group][1])
                with self.assertRaises(ValueError, msg=group + " duplicate"):
                    comparator.check_self_test(changed)
            changed = copy.deepcopy(report)
            changed["schema"] = "unknown"
            with self.assertRaises(ValueError):
                comparator.check_self_test(changed)
            self.assertEqual(report["quantlib_commit"], comparator.QUANTLIB_COMMIT)
            self.assertGreaterEqual(len(report["official_checks"]), 4)
            self.assertEqual(len(report["clock_checks"]), 4)
            self.assertEqual(len(report["european_checks"]), 9)
            self.assertEqual(len(report["factory_checks"]), 3)
            for row in report["official_checks"]:
                self.assertLessEqual(abs(row["premium"] - row["expected"]), row["tolerance"])
            self.assertTrue(all(comparator.check_clock(row["input"], row["effective"])
                                for row in report["clock_checks"]))
            self.assertTrue(all(abs(row["price"] - row["closed_form"]) < 1e-10
                                for row in report["european_checks"]))
            self.assertTrue(all(row["same_prices"] for row in report["factory_checks"]))


if __name__ == "__main__":
    unittest.main()
