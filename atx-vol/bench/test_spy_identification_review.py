"""Adversarial tests for the explicit two-row audit review, never fitting inputs."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import spy_acceptance as audit


class ReviewedIdentificationTests(unittest.TestCase):
    def fixture(self):
        review = audit.read_json(Path(__file__).with_name("fixtures") /
                                 "spy-reviewed-feasible-floor-v1.json")
        baseline = audit.read_json(audit.IDENTIFICATION_BASELINE)
        rows = [copy.deepcopy(item["raw_row"]) for item in review["contracts"]]
        entry = copy.deepcopy(review["contracts"][0]["entry"])
        entry["carry_curve"] = [copy.deepcopy(item["carry_point"]) for item in review["contracts"]]
        entry["queries"] = [copy.deepcopy(item["expected_query"]) for item in review["contracts"]]
        entry["nodes"] = [{"strike": q["strike"], "call": q["price"], "put": 0.0}
                          for q in entry["queries"]]
        report = {"policy_definition": {"graded": {"cold_fit": "joint_feasible_mid",
                                                    "terminal_floor_recovery": True}},
                  "graded_stats": {"acceptance_audit": {"snapshots": [[entry]]}}}
        prices = {"identified_rows_checked": 12850,
                  "price_only_row_ids": baseline["price_only_row_ids"][:14] +
                                        [row["row_id"] for row in rows]}
        eligibility = {"input_sha256": audit.INPUT_SHA256, "minimum_identified_rows": 12183,
                       "identification_baseline": baseline}
        return prices, eligibility, report, rows, entry

    def check(self, values):
        prices, eligibility, report, rows, _ = values
        return audit.identification_coverage(prices, eligibility,
            contract="reviewed-feasible-floor-v1", report=report, rows=rows)

    def test_fixture_preserves_original_64_bit_identity_without_float_rounding(self):
        values = self.fixture()
        self.assertEqual(values[3][0]["chain_id"], 9021385734418239560)
        self.assertEqual(values[3][0]["snapshot_ns"], 1791988200000000000)
        self.assertEqual(values[3][0]["observation_ns"], 1791988205890628000)
        self.assertIs(type(values[3][0]["chain_id"]), int)
        values[4]["chain_id"] = float(values[4]["chain_id"])
        self.assertFalse(self.check(values)["coverage_passed"])

    def test_default_stays_strict_and_reviewed_result_discloses_both_transitions(self):
        values = self.fixture()
        strict = audit.identification_coverage(values[0], values[1])
        self.assertFalse(strict["coverage_passed"])
        self.assertFalse(strict["strict_coverage_passed"])
        self.assertEqual(strict["reviewed_accepted_row_ids"], [])
        reviewed = self.check(values)
        self.assertTrue(reviewed["coverage_passed"], reviewed)
        self.assertFalse(reviewed["strict_coverage_passed"])
        self.assertEqual(reviewed["new_price_only_row_ids"], [27418697, 27419115])
        self.assertEqual(reviewed["reviewed_accepted_row_ids"], [27418697, 27419115])
        self.assertEqual(reviewed["minimum_identified_rows"], 12183)

    def test_third_demoted_id_or_lower_identified_count_fails(self):
        values = self.fixture()
        values[0]["price_only_row_ids"].append(999)
        self.assertFalse(self.check(values)["coverage_passed"])
        values = self.fixture()
        values[0]["identified_rows_checked"] = 12182
        self.assertFalse(self.check(values)["coverage_passed"])

    def test_changed_model_raw_quote_method_or_floor_policy_fails(self):
        for mutation in (lambda v: v[4]["market"].update(rate=0.05),
                         lambda v: v[4].update(dividend_model="continuous_yield"),
                         lambda v: v[4].update(method="channel"),
                         lambda v: v[4].update(cold_fit_fallback="unsupported_input"),
                         lambda v: v[4]["carry_curve"][0].update(continuous_yield=0.0),
                         lambda v: v[3][0].update(ask=656.0),
                         lambda v: v[3][0].update(expiry_days=21567),
                         lambda v: v[2]["policy_definition"]["graded"].update(
                             terminal_floor_recovery=False)):
            values = self.fixture()
            mutation(values)
            self.assertFalse(self.check(values)["coverage_passed"])

    def test_changed_price_status_reason_interval_or_actual_node_fails(self):
        for fields in ({"price": 655.9104551}, {"served_price": 655.9104551},
                       {"status": "success"}, {"reason": "low_vega"},
                       {"iv_lower": None}, {"iv_upper": 0.01}, {"volatility": 0.005},
                       {"price": float("nan")}, {"vol_convention": "variance_clock"}):
            values = self.fixture()
            values[4]["queries"][0].update(fields)
            self.assertFalse(self.check(values)["coverage_passed"], fields)
        values = self.fixture()
        values[4]["nodes"][0]["call"] += 1e-8
        self.assertFalse(self.check(values)["coverage_passed"])

    def test_only_machine_roundoff_change_is_permitted(self):
        values = self.fixture()
        for key in ("price", "served_price"):
            values[4]["queries"][0][key] += 1e-12
        values[4]["nodes"][0]["call"] += 1e-12
        self.assertTrue(self.check(values)["coverage_passed"])
        values[4]["queries"][0]["price"] += 1e-9
        self.assertFalse(self.check(values)["coverage_passed"])

    def test_missing_duplicate_or_wrong_raw_api_identity_fails(self):
        for mutation in (lambda v: v[4]["queries"].pop(),
                         lambda v: v[4]["queries"].append(v[4]["queries"][0]),
                         lambda v: v[3].append(v[3][0]),
                         lambda v: v[4]["carry_curve"].append(v[4]["carry_curve"][0]),
                         lambda v: v[4].update(chain_id=1)):
            values = self.fixture()
            mutation(values)
            self.assertFalse(self.check(values)["coverage_passed"])
        values = self.fixture()
        self.assertFalse(audit.identification_coverage(values[0], values[1],
            contract="reviewed-feasible-floor-v1")["coverage_passed"])

    def test_wrong_input_baseline_or_review_fixture_hash_fails(self):
        for mutation in (lambda v: v[1].update(input_sha256="wrong"),
                         lambda v: v[1]["identification_baseline"].update(
                             native_report_sha256="wrong"),
                         lambda v: v[1].update(minimum_identified_rows=12182)):
            values = self.fixture()
            mutation(values)
            self.assertFalse(self.check(values)["coverage_passed"])
        review_module = audit.identification_review
        with tempfile.TemporaryDirectory() as directory:
            changed = audit.read_json(review_module.REVIEW_FIXTURE)
            changed["contracts"][0]["cold_proof"]["old_sigma"] += 0.01
            path = Path(directory) / "changed.json"
            path.write_text(json.dumps(changed))
            with patch.object(review_module, "REVIEW_FIXTURE", path):
                self.assertFalse(self.check(self.fixture())["coverage_passed"])

    def test_unknown_contract_rejected_and_cli_lists_explicit_choices(self):
        with self.assertRaises(ValueError):
            values = self.fixture()
            audit.identification_coverage(values[0], values[1], contract="unknown")
        result = subprocess.run([sys.executable, str(Path(audit.__file__)), "--help"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertIn("--identification-contract", result.stdout)
        self.assertIn("frozen-wave1-v1", result.stdout)
        self.assertIn("reviewed-feasible-floor-v1", result.stdout)

    def test_oracle_remains_importable_by_file_without_bench_on_sys_path(self):
        source = "import importlib.util; from pathlib import Path; " \
            "p=Path.cwd()/'atx-vol/bench/spy_acceptance.py'; " \
            "s=importlib.util.spec_from_file_location('external_oracle',p); " \
            "m=importlib.util.module_from_spec(s); s.loader.exec_module(m); " \
            "assert m.FROZEN_CONTRACT == 'frozen-wave1-v1'"
        result = subprocess.run([sys.executable, "-c", source], capture_output=True,
                                text=True, check=False, cwd=Path(audit.__file__).parents[2])
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
