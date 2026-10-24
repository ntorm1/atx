"""Contract/receipt regressions for the isolated public American-put comparator."""
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import public_comparator as comparator


class ComparatorContractTests(unittest.TestCase):
    def test_corpus_contains_cheap_wings_closed_forms_and_negative_rate_diagnostics(self):
        corpus = comparator.make_corpus()
        self.assertEqual(corpus["schema"], "american-put-comparator-corpus-v1")
        self.assertEqual(len(corpus["groups"]), 51)
        self.assertEqual(len({g["id"] for g in corpus["groups"]}), 51)
        self.assertTrue(any(g["r"] == g["q"] == 0 for g in corpus["groups"]))
        self.assertTrue(any(g["q"] < g["r"] < 0 for g in corpus["groups"]))
        self.assertTrue(any(g["T"] < .01 and max(g["spots"]) == 200 for g in corpus["groups"]))
        self.assertTrue(all(g["K"] == 100 and len(g["spots"]) == 7 for g in corpus["groups"]))
        common = comparator.make_corpus("common")
        self.assertEqual(len(common["groups"]), 36)
        self.assertTrue(all(g["r"] > 0 and g["q"] >= 0 for g in common["groups"]))

    def test_original_upstream_hashes_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            manifest = {"files": [{"path": "header.hpp", "sha256": "0" * 64}]}
            (path / "header.hpp").write_text("wrong")
            with self.assertRaises(ValueError):
                comparator.verify_upstream(path, manifest)
            (path / "header.hpp").unlink()
            with self.assertRaises(ValueError):
                comparator.verify_upstream(path, manifest)

    def test_reference_error_is_not_independent_accuracy_and_failures_stay_in_denominator(self):
        report = {"schema": "american-put-public-comparator-v1", "case_count": 3, "cases": [{}, {}, {}],
                  "references": [
                      {"price": 1.0, "refinement_difference": 1e-7, "closed_form": None},
                      {"price": 2.0, "refinement_difference": .01, "closed_form": None},
                      {"price": 0.0, "refinement_difference": 0.0, "closed_form": 0.0}],
                  "engines": [{"name": "example", "cold_prices": [1.00001, 2.0, None],
                      "prepared_prices": [1.00001, 2.0, None], "cold_s": [1.0],
                      "prepared_setup_s": [.2], "prepared_eval_s": [.1], "prepared_total_s": [.31],
                      "cold_digests": [123], "prepared_digests": [123]}]}
        report["engines"] = [dict(copy.deepcopy(report["engines"][0]), name=name) for name in
                             ["simu_fast", "simu_finer", "atx_default", "atx_bulk_fast"]]
        report["upstream_adapter_checks"] = [dict(engine=name, index=i, bit_identical=True)
            for name in ["simu_fast", "simu_finer"] for i in range(3)]
        summary = comparator.summarize_report(report)
        row = summary["engines"][0]
        self.assertEqual(row["attempted"], 3)
        self.assertEqual(row["finite"], 2)
        self.assertEqual(row["budgets"]["0.001"]["resolved_pass"], 1)
        self.assertEqual(row["budgets"]["0.001"]["reference_unresolved"], 1)
        self.assertEqual(row["budgets"]["0.001"]["numerical_failure"], 1)
        self.assertFalse(summary["independent_general_american_accuracy_established"])
        malformed = copy.deepcopy(report)
        malformed["engines"][0]["prepared_prices"].pop()
        with self.assertRaises(ValueError):
            comparator.summarize_report(malformed)
        for key in ["cases", "upstream_adapter_checks"]:
            malformed = copy.deepcopy(report)
            malformed.pop(key)
            with self.assertRaises(ValueError):
                comparator.summarize_report(malformed)
        malformed = copy.deepcopy(report)
        malformed["engines"][0].pop("cold_digests")
        with self.assertRaises(ValueError):
            comparator.summarize_report(malformed)
        malformed = copy.deepcopy(report)
        malformed["upstream_adapter_checks"][0] = malformed["upstream_adapter_checks"][1]
        with self.assertRaises(ValueError):
            comparator.summarize_report(malformed)
        malformed = copy.deepcopy(report)
        malformed["upstream_adapter_checks"] = [{"bit_identical": False}] * 6
        with self.assertRaises(ValueError):
            comparator.summarize_report(malformed)
        malformed = copy.deepcopy(report)
        malformed["engines"][0]["cold_digests"] = [1, 2]
        with self.assertRaises(ValueError):
            comparator.summarize_report(malformed)
        malformed = copy.deepcopy(report)
        malformed["engines"][0]["prepared_prices"][0] = 5.
        with self.assertRaises(ValueError):
            comparator.summarize_report(malformed)


@unittest.skipUnless(os.environ.get("ATX_PUBLIC_AMERICAN_BINARY"), "explicit isolated native binary required")
class NativeComparatorTests(unittest.TestCase):
    def test_native_rejects_invalid_models_and_duplicate_group_identity(self):
        good = comparator.make_corpus()
        good["groups"] = good["groups"][:1]
        bad_values = []
        for field, value in [("K", 0), ("T", -1), ("sigma", True), ("r", "0.03")]:
            data = copy.deepcopy(good)
            data["groups"][0][field] = value
            bad_values.append(data)
        duplicate = copy.deepcopy(good)
        duplicate["groups"].append(copy.deepcopy(duplicate["groups"][0]))
        bad_values.append(duplicate)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for data in bad_values:
                (path / "input.json").write_text(json.dumps(data))
                result = subprocess.run([os.environ["ATX_PUBLIC_AMERICAN_BINARY"], "--input",
                    str(path / "input.json"), "--output", str(path / "output.json"), "--reps", "1"],
                    capture_output=True, text=True, timeout=60)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((path / "output.json").exists())

    def test_native_contract_setup_and_independent_european_check(self):
        corpus = comparator.make_corpus()
        corpus["groups"] = [corpus["groups"][10],
                            next(g for g in corpus["groups"] if g["r"] == g["q"] == 0)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "corpus.json").write_text(json.dumps(corpus))
            result = subprocess.run([os.environ["ATX_PUBLIC_AMERICAN_BINARY"], "--input",
                str(path / "corpus.json"), "--output", str(path / "report.json"), "--reps", "2"],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((path / "report.json").read_text())
            self.assertEqual(report["case_count"], 14)
            self.assertEqual(report["upstream_commit"], comparator.UPSTREAM_COMMIT)
            self.assertTrue(all(item["bit_identical"] for item in report["upstream_adapter_checks"]))
            for engine in report["engines"]:
                self.assertEqual(len(engine["cold_prices"]), 14)
                self.assertEqual(len(engine["prepared_prices"]), 14)
                self.assertEqual(len(engine["cold_s"]), 2)
                self.assertTrue(all(x > 0 for x in engine["prepared_setup_s"]))
                self.assertTrue(all(total >= setup + evaluate for total, setup, evaluate in zip(
                    engine["prepared_total_s"], engine["prepared_setup_s"], engine["prepared_eval_s"])))
                for cold, prepared in zip(engine["cold_prices"], engine["prepared_prices"]):
                    self.assertEqual(cold, prepared)
                if engine["name"].startswith("atx_"):
                    for index in range(7, 14):
                        self.assertIsNotNone(engine["cold_prices"][index])
                        self.assertAlmostEqual(engine["cold_prices"][index],
                                               report["references"][index]["closed_form"], places=10)
            summary = comparator.summarize_report(report)
            self.assertEqual(len(summary["engines"]), 4)
            self.assertFalse(summary["independent_general_american_accuracy_established"])


if __name__ == "__main__":
    unittest.main()
