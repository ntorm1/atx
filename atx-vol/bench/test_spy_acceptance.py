"""Adversarial tests of the acceptance oracle, independent of fitter internals."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import spy_acceptance as audit


def surface():
    bounds = {"spot": 100.0, "discount_min": 1.0, "discount_max": 1.0,
              "prepaid_min": 100.0, "prepaid_max": 100.0}
    nodes = [{"strike": 90.0, "call": 12.0, "put": 2.0},
             {"strike": 100.0, "call": 5.0, "put": 5.0},
             {"strike": 110.0, "call": 2.0, "put": 12.0}]
    queries = []
    for strike in (90.0, 92.5, 95.0, 97.5, 100.0, 102.5, 105.0, 107.5, 110.0):
        for side in ("call", "put"):
            left = 0 if strike <= 100 else 1
            fraction = (strike - nodes[left]["strike"]) / 10
            price = nodes[left][side] * (1 - fraction) + nodes[left + 1][side] * fraction
            queries.append({"strike": strike, "side": side, "status": "price_only",
                            "served_price": price, "price": price, "volatility": None,
                            "reason": "low_vega", "iv_lower": None, "iv_upper": None,
                            "vol_convention": "none"})
    return {"chain_id": 1, "grade": "A", "calendar_years": 1.0,
            "bounds": bounds, "nodes": nodes, "queries": queries,
            "market": {"spot": 100.0, "rate": 0.0, "continuous_yield": 0.0,
                       "calendar_years": 1.0, "variance_years": 1.0, "dividend_pv": 0.0},
            "dividend_model": "continuous_yield", "carry_curve": [],
            "withheld_nodes": [], "record_attached": False}


def document(*slices):
    return {"graded_stats": {"acceptance_audit": {
        "schema": "atx-vol-final-api-audit-v1", "representation": "certified_piecewise_linear",
        "timing_qualifying": False, "snapshots": [list(slices)]}}}


class FinalApiOracleTests(unittest.TestCase):
    def raw_fixture(self):
        value = surface()
        value["dividend_model"] = "aggregate_funding_pv_approximation"
        value["carry_curve"] = [{"strike": node["strike"], "continuous_yield": 0.0}
                                for node in value["nodes"]]
        report = document(value)
        report["policy_definition"] = {"snapshot": {"one_spot": "nearest",
                                                   "carry_price_tolerance": 1e-6}}
        rows = [{"chain_id": 1, "row_id": index, "strike": node["strike"], "bid": 0.0,
                 "ask": 20.0, "spot": 100.0, "rate": 0.0, "sdiv": 0.0, "ddiv_pv": 0.0,
                 "years": 1.0, "years_calendar": 1.0, "is_call": 1, "exercise": "American",
                 "input_status": "ok", "snapshot_ns": 100, "observation_ns": 100}
                for index, node in enumerate(value["nodes"])]
        return report, rows, value

    def test_raw_model_and_bounds_reconstruction_passes(self):
        report, rows, _ = self.raw_fixture()
        self.assertEqual(audit.raw_model_violations(report, rows), [])

    def test_exported_looser_bounds_cannot_redefine_oracle(self):
        report, rows, value = self.raw_fixture()
        value["bounds"]["discount_max"] = 2.0
        self.assertTrue(any(v["kind"] == "bounds_differ_from_raw"
                            for v in audit.raw_model_violations(report, rows)))

    def test_exported_carry_and_market_must_match_raw_normalization(self):
        report, rows, value = self.raw_fixture()
        value["market"]["rate"] = 0.1
        value["carry_curve"][0]["continuous_yield"] = 0.1
        kinds = {v["kind"] for v in audit.raw_model_violations(report, rows)}
        self.assertIn("market_differs_from_raw", kinds)
        self.assertIn("carry_differs_from_raw", kinds)

    def test_genuine_price_only_domain_passes(self):
        self.assertEqual(audit.api_violations(document(surface())), [])

    def test_record_route_requires_its_own_continuous_representation_proof(self):
        value = surface()
        value["record_attached"] = True
        self.assertTrue(any(v["kind"] == "unproved_record_representation"
                            for v in audit.api_violations(document(value))))

    def test_missing_native_audit_cannot_pass(self):
        self.assertTrue(audit.api_violations({}))

    def test_between_node_api_price_divergence_fails(self):
        value = surface()
        value["queries"][4]["price"] += 0.01
        self.assertTrue(audit.api_violations(document(value)))

    def test_withheld_price_leak_fails(self):
        value = surface()
        value["withheld_nodes"] = [0, 1, 0]
        self.assertTrue(audit.api_violations(document(value)))

    def test_nonconvex_prices_fail_even_if_quotes_would_contain(self):
        value = surface()
        value["nodes"][1]["call"] = 8.0
        value["nodes"][1]["put"] = 8.0
        self.assertTrue(audit.api_violations(document(value)))

    def test_calendar_failure_between_nonadjacent_domains_is_detected(self):
        early, middle, later = surface(), surface(), surface()
        middle["chain_id"], later["chain_id"] = 2, 3
        middle["calendar_years"], later["calendar_years"] = 2.0, 3.0
        # The middle expiry offers only strike 100..110. Early/later overlap at90.
        middle["nodes"] = middle["nodes"][1:]
        middle["queries"] = [q for q in middle["queries"] if q["strike"] >= 100]
        later["nodes"][0]["call"] -= 0.01
        later["nodes"][0]["put"] -= 0.01
        for query in later["queries"]:
            if query["strike"] <= 100:
                change = 0.01 * (100 - query["strike"]) / 10
                query["price"] -= change
                query["served_price"] -= change
        violations = audit.api_violations(document(early, middle, later))
        self.assertTrue(any(v["kind"] == "calendar" for v in violations))

    def test_missing_union_or_between_node_query_is_not_silently_accepted(self):
        value = surface()
        value["queries"].pop(4)
        self.assertTrue(audit.api_violations(document(value)))

    def test_price_only_cannot_claim_identified_volatility(self):
        value = surface()
        value["queries"][0]["volatility"] = 0.2
        self.assertTrue(audit.api_violations(document(value)))

    def test_actual_api_interval_metadata_is_required_and_well_formed(self):
        for patch in ({"iv_lower": 0.3, "iv_upper": 0.2}, {"iv_upper": float("inf")},
                      {"reason": "none"}, {"vol_convention": "european_equivalent"}):
            value = surface()
            value["queries"][0].update(patch)
            self.assertTrue(any(v["kind"] == "invalid_api_metadata"
                                for v in audit.api_violations(document(value))))
        value = surface()
        del value["queries"][0]["iv_upper"]
        self.assertFalse(audit.valid_query_metadata(value["queries"][0]))

    def test_csv_metadata_must_match_actual_api_including_interval_and_convention(self):
        query = surface()["queries"][0]
        query.update(reason="floor_vol", iv_lower=0.0, iv_upper=0.005)
        row = {"status": "ok_price_only", "price_only_reason": "floor_vol",
               "predicted_vol": "", "iv_lower": "0", "iv_upper": "0.005",
               "vol_convention": "none"}
        self.assertTrue(audit.prediction_metadata_matches(query, row))
        for field, wrong in (("iv_upper", "0.006"), ("iv_lower", ""),
                             ("predicted_vol", "0.2"), ("price_only_reason", "low_vega"),
                             ("vol_convention", "fitted_curve"), ("status", "ok")):
            self.assertFalse(audit.prediction_metadata_matches(query, dict(row, **{field: wrong})))
        query.update(status="success", reason="none", volatility=0.2, iv_lower=None,
                     iv_upper=None, vol_convention="reimplied_repaired")
        row.update(status="ok", price_only_reason="none", predicted_vol="0.2",
                   iv_lower="", iv_upper="", vol_convention="reimplied_repaired")
        self.assertTrue(audit.prediction_metadata_matches(query, row))
        self.assertFalse(audit.prediction_metadata_matches(query, dict(row, predicted_vol="0.21")))

    def test_discounted_terminal_floor_is_checked_with_nonzero_carry(self):
        value = surface()
        value["market"]["rate"] = 0.1
        value["market"]["continuous_yield"] = 1e-8
        self.assertTrue(any(v["kind"] == "terminal_european_floor"
                            for v in audit.api_violations(document(value))))


class ColdNodeRootOracleTests(unittest.TestCase):
    def fixture(self):
        value = surface()
        options = {"explicit": False, "n_collocation": 12, "n_quadrature": 24,
                   "n_quad_price": 0, "max_newton_iter": 8, "tol": 1e-10, "profile": 0}
        value["al_options"] = options
        nodes = {node["strike"] for node in value["nodes"]}
        for query in value["queries"]:
            if query["strike"] not in nodes:
                continue
            query.update(status="success", volatility=0.2, reason="none",
                         vol_convention="reimplied_repaired")
            query["cold_node"] = {"status": "evaluated", "model": "andersen_lake",
                "options": options.copy(), "spot": 100.0, "strike": query["strike"],
                "variance_years": 1.0, "effective_rate": 0.0, "calendar_yield": 0.0,
                "effective_yield": 0.0, "sigma_lower": 0.2 - 1e-7, "sigma_center": 0.2,
                "sigma_upper": 0.2 + 1e-7, "radius": 1e-7,
                "price_lower": query["price"] - 1e-5, "price_center": query["price"],
                "price_upper": query["price"] + 1e-5}
        return document(value), value["queries"][0]

    def test_all_identified_node_roots_must_strictly_straddle(self):
        report, query = self.fixture()
        result = audit.cold_root_audit(report)
        self.assertTrue(result["strict_accuracy_passed"])
        self.assertEqual(result["strict_straddles"], 6)
        query["cold_node"]["price_lower"] = query["price"] + 1e-6
        query["cold_node"]["price_center"] = query["price"] + 2e-6
        result = audit.cold_root_audit(report)
        self.assertFalse(result["strict_accuracy_passed"])
        self.assertEqual(result["failure_count"], 1)
        self.assertEqual(result["failures"][0]["kind"], "cold_root_outside_1e7_bracket")

    def test_roundoff_and_flat_prices_are_separate_unresolved_evidence(self):
        for delta in (0.0, 1e-13):
            report, query = self.fixture()
            query["cold_node"]["price_lower"] = query["price"] + delta
            result = audit.cold_root_audit(report)
            self.assertFalse(result["strict_accuracy_passed"])
            self.assertEqual(result["roundoff_only_count"], 1)
            self.assertEqual(result["failure_count"], 0)

    def test_wrong_contract_options_or_widened_iv_radius_cannot_pass(self):
        for patch in ({"effective_yield": 0.1}, {"radius": 2e-7},
                      {"sigma_upper": 0.2 + 2e-7}, {"effective_rate": 0.1}):
            report, query = self.fixture()
            query["cold_node"].update(patch)
            self.assertEqual(audit.cold_root_audit(report)["failure_count"], 1)
        report, query = self.fixture()
        query["cold_node"]["options"]["profile"] = 1
        self.assertEqual(audit.cold_root_audit(report)["failure_count"], 1)

    def test_missing_unsupported_nonfinite_or_nonmonotone_forward_evidence_fails(self):
        for patch in ({"status": "unsupported_model"}, {"price_upper": None},
                      {"price_upper": float("nan")}, {"price_center": 100.0}):
            report, query = self.fixture()
            query["cold_node"].update(patch)
            self.assertEqual(audit.cold_root_audit(report)["failure_count"], 1)
        report, query = self.fixture()
        del query["cold_node"]
        self.assertEqual(audit.cold_root_audit(report)["failure_count"], 1)


class ColdClockOracleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def timing_fixture(self, count=100, native_cold=0.5):
        predictions = self.directory / "predictions.csv"
        predictions.write_text("same actual predictions\n", encoding="utf-8")
        prediction_hash = audit.sha256(predictions)
        self.receipt = {"binary_sha256": "binary", "input_sha256": "input",
                        "policy_sha256": "policy", "predictions_sha256": prediction_hash}
        self.report = {"policy_definition": {"test_policy": True}}
        manifest = {"binary": {"sha256": "binary"}, "input": {"sha256": "input"},
                    "policy": {"sha256": "policy"}, "predictions_sha256": prediction_hash,
                    "predictions_identical": True, "fast_math": False, "source": {"dirty": False},
                    "trials_ok": 100, "trials_requested": 100, "trials": []}
        for index in range(count):
            identity = {"pid": index + 1, "creation_filetime": str(1000000000 + index)}
            affinity = {"fit_workers": {"env_ATX_VOL_FIT_WORKERS": "1", "resolved_auto": 1},
                        "pinned": True, "is_pcore": True, "requested_cpu": 6,
                        "current_cpu_at_receipt": 6, "process_affinity_mask": "0x40",
                        "nested_pools": {"surface_expiry_fanout": 1}, "smt_siblings": [7],
                        "busy": {"smt_sibling_pct": [0.0]}}
            native = {"policy_definition": self.report["policy_definition"], "bench": {
                "process_identity": identity, "affinity": affinity, "reps": 1,
                "build_info": {"fast_math": False}, "per_rep": [
                    {"rep": 1, "cold_calibration_s": native_cold, "deam_memo_entries_at_start": 0}]}}
            path = self.directory / f"trial-{index}.json"
            path.write_text(json.dumps(native), encoding="utf-8")
            launcher_affinity = copy.deepcopy(affinity)
            launcher_affinity["smt_sibling_busy_pct"] = [0.0]
            manifest["trials"].append({"trial": index + 1, "exit_code": 0,
                "report": path.name, "report_sha256": audit.sha256(path),
                "predictions": predictions.name, "predictions_sha256": prediction_hash,
                "cold_calibration_s": 0.5, "affinity": launcher_affinity,
                "process_identity": identity})
        return manifest

    def checks(self, manifest):
        return audit.cold_timing_checks(manifest, self.directory, self.receipt, self.report)

    def test_100_independently_joined_fresh_native_reports_pass(self):
        checks = self.checks(self.timing_fixture())
        self.assertTrue(all(checks.values()), checks)

    def test_summary_cannot_turn_one_trial_into_100(self):
        self.assertFalse(self.checks(self.timing_fixture(count=1))["at_least100_fresh_processes"])

    def test_summary_cannot_hide_native_50second_clock(self):
        self.assertFalse(self.checks(self.timing_fixture(native_cold=50.0))["all_cold_under1second"])

    def test_duplicate_native_report_or_process_cannot_count_twice(self):
        manifest = self.timing_fixture()
        manifest["trials"][1] = copy.deepcopy(manifest["trials"][0])
        manifest["trials"][1]["trial"] = 2
        self.assertFalse(self.checks(manifest)["distinct_trial_report_process_evidence"])

    def test_summary_cannot_hide_native_multi_cpu_affinity_or_busy_sibling(self):
        manifest = self.timing_fixture()
        trial = manifest["trials"][0]
        path = self.directory / trial["report"]
        native = audit.read_json(path)
        native["bench"]["affinity"]["process_affinity_mask"] = "0xc0"
        native["bench"]["affinity"]["busy"]["smt_sibling_pct"] = [70.0]
        path.write_text(json.dumps(native), encoding="utf-8")
        trial["report_sha256"] = audit.sha256(path)
        checks = self.checks(manifest)
        self.assertFalse(checks["one_pcore"])
        self.assertFalse(checks["idle_sibling"])

    def test_changed_prediction_file_cannot_pass_saved_hash(self):
        manifest = self.timing_fixture()
        (self.directory / "predictions.csv").write_text("changed\n", encoding="utf-8")
        self.assertFalse(self.checks(manifest)["same_predictions"])

    def test_process_launch_identity_must_match_native_kernel_identity(self):
        manifest = self.timing_fixture()
        manifest["trials"][0]["process_identity"] = {"pid": 999, "creation_filetime": "1"}
        self.assertFalse(self.checks(manifest)["distinct_trial_report_process_evidence"])

    def test_native_worker_object_and_single_cpu_are_required(self):
        trial = {"affinity": {"fit_workers": {"env_ATX_VOL_FIT_WORKERS": "1", "resolved_auto": 1},
                              "pinned": True, "is_pcore": True, "requested_cpu": 6,
                              "current_cpu_at_receipt": 6, "process_affinity_mask": "0x40",
                              "nested_pools": {"surface_expiry_fanout": 1}}}
        self.assertTrue(audit.one_core_trial(trial))
        two = copy.deepcopy(trial)
        two["affinity"]["fit_workers"]["resolved_auto"] = 2
        self.assertFalse(audit.one_core_trial(two))
        two = copy.deepcopy(trial)
        two["affinity"]["process_affinity_mask"] = "0xc0"
        self.assertFalse(audit.one_core_trial(two))
        two = copy.deepcopy(trial)
        two["affinity"]["nested_pools"]["surface_expiry_fanout"] = 2
        self.assertFalse(audit.one_core_trial(two))


if __name__ == "__main__":
    unittest.main()
