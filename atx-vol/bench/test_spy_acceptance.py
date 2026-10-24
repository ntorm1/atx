"""Adversarial tests of the acceptance oracle, independent of fitter internals."""
import copy
import csv
import json
import math
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

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
                   "n_quad_price": 48, "max_newton_iter": 6, "tol": 1e-10, "profile": 0}
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
                "price_upper": query["price"] + 1e-5,
                "floor_sigma": 0.005, "price_at_floor": max(0.0,
                    100.0 - query["strike"] if query["side"] == "call" else query["strike"] - 100.0)}
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


class ColdNodePriceOracleTests(unittest.TestCase):
    def fixture(self):
        report, query = ColdNodeRootOracleTests.fixture(self)
        value = report["graded_stats"]["acceptance_audit"]["snapshots"][0][0]
        rows = []
        for node in value["nodes"]:
            for side in ("call", "put"):
                rows.append({"chain_id": 1, "row_id": len(rows), "strike": node["strike"],
                    "bid": node[side] - 0.1, "ask": node[side] + 0.1, "spot": 100.0,
                    "rate": 0.0, "sdiv": 0.0, "ddiv_pv": 0.0, "years": 1.0,
                    "years_calendar": 1.0, "is_call": side == "call", "exercise": "American",
                    "input_status": "ok"})
        return report, query, rows

    @staticmethod
    def residual(query, error, half_width=1e-5):
        cold = query["cold_node"]
        cold["price_lower"] = query["price"] + error - half_width
        cold["price_center"] = query["price"] + error
        cold["price_upper"] = query["price"] + error + half_width

    def test_economic_contract_is_versioned_and_strict_iv_is_only_diagnostic(self):
        report, query, rows = self.fixture()
        self.residual(query, 0.0005)
        result = audit.cold_price_audit(report, rows)
        self.assertTrue(result["price_accuracy_passed"])
        self.assertFalse(result["strict_iv_diagnostic"]["strict_accuracy_passed"])
        self.assertEqual(result["accuracy_contract"]["version"], "raw-spread-price-v1")
        self.assertFalse(result["accuracy_contract"]["greek_accuracy_established"])
        self.assertEqual(result["identified_rows_checked"], 6)
        self.assertEqual(audit.accuracy_contract("strict-iv-v1")["version"], "strict-iv-v1")
        with self.assertRaises(ValueError):
            audit.accuracy_contract("unversioned")

    def test_price_cap_and_original_tight_spread_both_bind(self):
        report, query, rows = self.fixture()
        self.residual(query, 0.0011)
        self.assertFalse(audit.cold_price_audit(report, rows)["price_accuracy_passed"])
        self.residual(query, 0.0002)
        rows[0].update(bid=11.99, ask=12.0)
        result = audit.cold_price_audit(report, rows)
        self.assertFalse(result["price_accuracy_passed"])
        failure = result["failures"][0]
        self.assertEqual(failure["failing_row_id"], rows[0]["row_id"])
        self.assertAlmostEqual(failure["economic_budget_usd"], 0.0001)
        self.assertEqual(failure["raw_budgets"][0]["ask"], 12.0)

    def test_locked_quote_has_zero_economic_budget_with_separate_roundoff(self):
        report, query, rows = self.fixture()
        rows[0].update(bid=12.0, ask=12.0)
        self.assertTrue(audit.cold_price_audit(report, rows)["price_accuracy_passed"])
        self.residual(query, 1e-13)
        result = audit.cold_price_audit(report, rows)
        self.assertTrue(result["price_accuracy_passed"])
        self.assertEqual(result["roundoff_only_rows"], 1)
        self.assertEqual(result["zero_budget_rows"], 1)
        self.assertEqual(result["observations"][0]["economic_budget_usd"], 0.0)
        self.residual(query, 1e-8)
        self.assertFalse(audit.cold_price_audit(report, rows)["price_accuracy_passed"])

    def test_crossed_nonfinite_or_boolean_box_is_never_widened(self):
        for row in ({"bid": 1, "ask": 0}, {"bid": 1, "ask": float("nan")},
                    {"bid": True, "ask": 1}, {"bid": -1, "ask": 1}):
            with self.assertRaises(ValueError):
                audit.raw_price_budget(row)
        report, _, rows = self.fixture()
        rows[0].update(bid=12, ask=11)
        result = audit.cold_price_audit(report, rows)
        self.assertFalse(result["price_accuracy_passed"])
        self.assertTrue(any(f["kind"] == "missing_original_quote_budget" for f in result["failures"]))

    def test_duplicate_contract_uses_every_original_row_not_a_wider_merged_box(self):
        report, query, rows = self.fixture()
        rows.append(dict(rows[0], row_id=100, bid=11.99, ask=12.0))
        self.residual(query, 0.0002)
        result = audit.cold_price_audit(report, rows)
        self.assertFalse(result["price_accuracy_passed"])
        self.assertEqual(result["identified_rows_checked"], 7)
        self.assertEqual([f["failing_row_id"] for f in result["failures"]], [100])

    def test_missing_raw_row_and_invalid_cold_contract_fail_closed(self):
        for mutate in (lambda q: q["cold_node"].update(effective_yield=0.1),
                       lambda q: q["cold_node"].update(price_center=float("nan"))):
            report, query, rows = self.fixture()
            mutate(query)
            self.assertFalse(audit.cold_price_audit(report, rows)["price_accuracy_passed"])
        report, _, rows = self.fixture()
        self.assertFalse(audit.cold_price_audit(report, rows[1:])["price_accuracy_passed"])

    def test_price_only_stays_in_denominator_without_invented_point_volatility(self):
        report, query, rows = self.fixture()
        query.update(status="price_only", volatility=None, reason="low_vega", vol_convention="none")
        result = audit.cold_price_audit(report, rows)
        self.assertTrue(result["price_accuracy_passed"])
        self.assertEqual(result["identified_rows_checked"], 5)
        self.assertEqual(result["price_only_rows"], 1)
        query["volatility"] = 0.2
        self.assertFalse(audit.cold_price_audit(report, rows)["price_accuracy_passed"])

    def test_all_price_only_cannot_make_cold_accuracy_vacuously_pass(self):
        report, _, rows = self.fixture()
        entry = report["graded_stats"]["acceptance_audit"]["snapshots"][0][0]
        for query in entry["queries"]:
            query.update(status="price_only", volatility=None, reason="low_vega",
                         vol_convention="none")
        result = audit.cold_price_audit(report, rows)
        self.assertEqual(result["identified_rows_checked"], 0)
        self.assertEqual(result["price_only_rows"], 6)
        self.assertFalse(result["price_accuracy_passed"])
        self.assertTrue(any(f["kind"] == "no_identified_rows_checked" for f in result["failures"]))

    def test_coverage_count_and_row_identity_both_prevent_silent_demotion(self):
        baseline = {"minimum_identified_rows": 4,
                    "identification_baseline": {"price_only_row_ids": [4, 5]}}
        same = {"identified_rows_checked": 4, "price_only_row_ids": [4, 5]}
        self.assertTrue(audit.identification_coverage(same, baseline)["coverage_passed"])
        fewer = {"identified_rows_checked": 3, "price_only_row_ids": [0, 4, 5]}
        self.assertFalse(audit.identification_coverage(fewer, baseline)["coverage_passed"])
        # Recovering row4 cannot conceal demoting previously identified row0.
        swapped = {"identified_rows_checked": 4, "price_only_row_ids": [0, 5]}
        result = audit.identification_coverage(swapped, baseline)
        self.assertFalse(result["coverage_passed"])
        self.assertEqual(result["new_price_only_row_ids"], [0])
        self.assertEqual(result["baseline_price_only_rows_now_not_price_only"], [4])
        self.assertFalse(audit.identification_coverage(
            {"identified_rows_checked": 0, "price_only_row_ids": []}, {})["coverage_passed"])

    def test_frozen_spy_identification_baseline_has_named_source_and_required_count(self):
        baseline = audit.read_json(audit.IDENTIFICATION_BASELINE)
        self.assertEqual(baseline["input_sha256"], audit.INPUT_SHA256)
        self.assertEqual(baseline["identified_rows"], 12183)
        self.assertEqual(baseline["eligible_rows"], 12866)
        self.assertEqual(len(set(baseline["price_only_row_ids"])), 683)
        self.assertEqual(baseline["source_commit"], "47b2037572690d60ce87113c37340c75ac129913")
        self.assertEqual(baseline["prediction_sha256"],
                         "681c3b44e3edbd799b90b03ccfaad2564d98a580085ae4131093cf749e08a8cf")

    def test_low_vega_estimate_is_separate_and_does_not_relax_price_gate(self):
        report, query, rows = self.fixture()
        self.residual(query, 0.0005, half_width=1e-10)
        result = audit.cold_price_audit(report, rows)
        self.assertTrue(result["price_accuracy_passed"])
        self.assertGreater(result["conditioning"]["max_abs_local_iv_error_estimate"], 0.1)
        self.assertIn("not root-error bounds", result["conditioning"]["scope"])
        self.residual(query, 0.002, half_width=1e-10)
        self.assertFalse(audit.cold_price_audit(report, rows)["price_accuracy_passed"])

    def test_verify_requires_explicit_version_to_change_promotion_gate(self):
        report, query, rows = self.fixture()
        self.residual(query, 0.0005)
        entry = report["graded_stats"]["acceptance_audit"]["snapshots"][0][0]
        entry["dividend_model"] = "aggregate_funding_pv_approximation"
        entry["carry_curve"] = [{"strike": n["strike"], "continuous_yield": 0.0}
                                for n in entry["nodes"]]
        report["policy_definition"] = {"snapshot": {"one_spot": "nearest",
                                                   "carry_price_tolerance": 1e-6}}
        queries = {(q["strike"], q["side"]): q for q in entry["queries"]}
        predictions = []
        for row in rows:
            row.update(is_call=int(row["is_call"]), expiry_days=21000,
                       snapshot_ns=100, observation_ns=100)
            q = queries[row["strike"], "call" if row["is_call"] else "put"]
            predictions.append({"row_id": row["row_id"], "status": "ok",
                "predicted_price": q["price"], "predicted_vol": q["volatility"],
                "price_only_reason": "none", "vol_convention": q["vol_convention"],
                "iv_lower": "", "iv_upper": ""})
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for filename, data in (("input.csv", rows), ("predictions.csv", predictions)):
                with (directory / filename).open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(data[0]))
                    writer.writeheader()
                    writer.writerows(data)
            audit.write_json(directory / "native.json", report)
            receipt = {"exit_code": 0, "input_sha256": audit.sha256(directory / "input.csv"),
                "report_sha256": audit.sha256(directory / "native.json"),
                "predictions_sha256": audit.sha256(directory / "predictions.csv")}
            audit.write_json(directory / "receipt.json", receipt)
            eligibility = {"input_sha256": receipt["input_sha256"], "eligible_rows": 6,
                "required_slices": 1, "containment_min": .995,
                "eligible_row_ids": [row["row_id"] for row in rows]}
            args = SimpleNamespace(input=directory / "input.csv", audit_dir=directory,
                                   command="verify")
            legacy = audit.verify(args, eligibility)
            self.assertFalse(legacy["correctness_passed"])
            self.assertFalse(legacy["checks"]["identified_node_cold_root_accuracy"])
            args.accuracy_contract = "raw-spread-price-v1"
            economic = audit.verify(args, eligibility)
            self.assertTrue(economic["correctness_passed"], economic["checks"])
            self.assertNotIn("identified_node_cold_root_accuracy", economic["checks"])
            self.assertFalse(economic["cold_root_audit"]["strict_accuracy_passed"])
            self.assertFalse(economic["goal_achieved"])
            self.assertEqual(economic["schema"], "atx-vol-spy-acceptance-v3")
            self.assertEqual(len(economic["checks"]), 13)
            self.assertTrue(economic["checks"]["identified_node_target_floor_semantics"])
            # Each timed solution uses its own actual API payload and files,
            # with the same financial gates as the standalone audit.
            own = audit.verify_solution(report, receipt, directory / "predictions.csv",
                directory / "native.json", rows, eligibility,
                audit.RAW_SPREAD_PRICE_CONTRACT, audit.FROZEN_CONTRACT)
            self.assertEqual(own["checks"], economic["checks"])
            bad = copy.deepcopy(report)
            bad_query = bad["graded_stats"]["acceptance_audit"]["snapshots"][0][0]["queries"][0]
            bad_query["price"] += 1.0
            audit.write_json(directory / "different-native.json", bad)
            bad_receipt = dict(receipt, report_sha256=audit.sha256(directory / "different-native.json"))
            invalid = audit.verify_solution(bad, bad_receipt, directory / "predictions.csv",
                directory / "different-native.json", rows, eligibility,
                audit.RAW_SPREAD_PRICE_CONTRACT, audit.FROZEN_CONTRACT)
            self.assertFalse(invalid["correctness_passed"])
            self.assertFalse(invalid["checks"]["every_eligible_prediction_matches_actual_api"])
            # Missing new evidence cannot reuse a historical 12-gate pass.
            del query["cold_node"]["price_at_floor"]
            (directory / "native.json").write_text(json.dumps(report))
            receipt["report_sha256"] = audit.sha256(directory / "native.json")
            (directory / "receipt.json").write_text(json.dumps(receipt))
            missing_floor = audit.verify(args, eligibility)
            self.assertTrue(missing_floor["checks"]["identified_node_cold_reference_price_accuracy"])
            self.assertFalse(missing_floor["checks"]["identified_node_target_floor_semantics"])
            self.assertFalse(missing_floor["correctness_passed"])

    def test_frozen_spy_price_and_low_vega_cases_preserve_both_contract_results(self):
        fixture = audit.read_json(Path(__file__).with_name("fixtures") /
                                  "spy-wave1-cold-price-cases-v1.json")
        result = audit.cold_price_audit(fixture["report"], fixture["raw_rows"])
        self.assertTrue(result["price_accuracy_passed"])
        self.assertEqual(result["identified_rows_checked"], fixture["expected"]["identified_rows_checked"])
        self.assertEqual(result["price_only_rows"], fixture["expected"]["price_only_rows"])
        self.assertEqual(result["strict_iv_diagnostic"]["failure_count"],
                         fixture["expected"]["strict_iv_failures"])
        self.assertAlmostEqual(result["max_center_residual_usd"], 0.0007096053236921307)
        self.assertAlmostEqual(result["conditioning"]["max_abs_local_iv_error_estimate"],
                               0.0017007053632673893)
        # The same saved prices fail a genuinely tighter original market;
        # changing their cold diagnostics or widening a projection box cannot
        # silently substitute for the original row used to set the budget.
        for row in fixture["raw_rows"]:
            if row["row_id"] == 27423846:
                row["ask"] = row["bid"] + 0.01
        self.assertFalse(audit.cold_price_audit(fixture["report"], fixture["raw_rows"])
                         ["price_accuracy_passed"])


class TargetFloorOracleTests(unittest.TestCase):
    @staticmethod
    def bs_atm(sigma):
        # S=K=100, r=q=0, T=1: both sides have this independent European value.
        return 100.0 * math.erf(sigma / (2.0 * math.sqrt(2.0)))

    def fixture(self, target_sigma=0.2, reported_sigma=0.2):
        report, _, rows = ColdNodePriceOracleTests.fixture(self)
        entry = report["graded_stats"]["acceptance_audit"]["snapshots"][0][0]
        query = next(q for q in entry["queries"] if q["strike"] == 100 and q["side"] == "call")
        target = self.bs_atm(target_sigma)
        query.update(price=target, served_price=target, volatility=reported_sigma)
        query["cold_node"].update(sigma_lower=reported_sigma - 1e-7,
            sigma_center=reported_sigma, sigma_upper=reported_sigma + 1e-7,
            price_lower=self.bs_atm(reported_sigma - 1e-7),
            price_center=self.bs_atm(reported_sigma), price_upper=self.bs_atm(reported_sigma + 1e-7),
            floor_sigma=0.005, price_at_floor=self.bs_atm(0.005))
        return report, query, rows

    def test_subfloor_target_passes_monetary_budget_but_fails_floor_semantics(self):
        report, query, rows = self.fixture(0.00499, 0.00501)
        self.assertTrue(audit.cold_price_audit(report, rows)["price_accuracy_passed"])
        result = audit.cold_floor_audit(report, rows)
        self.assertFalse(result["floor_semantics_passed"])
        self.assertEqual(result["failure_count"], 1)
        self.assertEqual(result["failures"][0]["kind"], "target_at_or_below_sigma_floor_price")
        self.assertGreater(query["price"], 1.0e-7 + 1.0e-12)
        self.assertEqual(result["contract"]["version"], "target-iv-floor-v1")

    def test_close_above_floor_is_accepted_without_widening_floor(self):
        report, _, rows = self.fixture(0.00500001, 0.00500001)
        self.assertTrue(audit.cold_floor_audit(report, rows)["floor_semantics_passed"])
        report, _, rows = self.fixture(0.005, 0.00501)
        self.assertFalse(audit.cold_floor_audit(report, rows)["floor_semantics_passed"])
        report, _, rows = self.fixture(0.00501, 0.005)
        result = audit.cold_floor_audit(report, rows)
        self.assertTrue(any(f["kind"] == "reported_sigma_at_or_below_floor" for f in result["failures"]))

    def test_missing_wrong_or_nonfinite_floor_evidence_fails_closed(self):
        for patch in ({"floor_sigma": None}, {"floor_sigma": True}, {"floor_sigma": 0.004},
                      {"price_at_floor": None}, {"price_at_floor": float("nan")},
                      {"price_at_floor": float("inf")}, {"price_at_floor": -1.0},
                      {"price_at_floor": False}):
            report, query, rows = self.fixture()
            query["cold_node"].update(patch)
            self.assertFalse(audit.cold_floor_audit(report, rows)["floor_semantics_passed"], patch)
        for key in ("floor_sigma", "price_at_floor"):
            report, query, rows = self.fixture()
            del query["cold_node"][key]
            self.assertFalse(audit.cold_floor_audit(report, rows)["floor_semantics_passed"])

    def test_zero_vol_supremum_includes_stationary_point_and_negative_rates(self):
        zero, ceiling = audit.zero_volatility_bounds(150.0, 100.0, 10.0, 0.1, 0.05, "call")
        self.assertAlmostEqual(zero, 56.25, places=12)
        self.assertEqual(ceiling, 150.0)
        # A put with both rates negative has an interior maximum in this example.
        zero, ceiling = audit.zero_volatility_bounds(50.0, 150.0, 10.0, -0.05, -0.1, "put")
        self.assertAlmostEqual(zero, 112.5, places=12)
        self.assertAlmostEqual(ceiling, 150.0 * math.exp(0.5))
        self.assertEqual(audit.zero_volatility_bounds(100, 100, 1, 0, 0, "call"), (0.0, 100.0))
        self.assertEqual(audit.zero_volatility_bounds(100, 100, 1, 0.1, 0.1, "put"), (0.0, 100.0))
        for inputs in ((100, 100, 0, 0, 0, "call"), (True, 100, 1, 0, 0, "call"),
                       (100, 100, 1, 0, 0, "other"), (100, 100, 1, -1000, -1000, "put")):
            with self.assertRaises(ValueError):
                audit.zero_volatility_bounds(*inputs)

    def test_zero_volatility_target_screen_is_separate_from_floor_evidence(self):
        report, query, rows = self.fixture()
        query.update(price=1e-8, served_price=1e-8)
        query["cold_node"]["price_at_floor"] = 0.0
        result = audit.cold_floor_audit(report, rows)
        self.assertTrue(any(f["kind"] == "target_in_zero_volatility_band" for f in result["failures"]))
        self.assertFalse(result["floor_semantics_passed"])

    def test_actual_eight_spy_floor_promotions_fail_independent_zero_vol_screen(self):
        fixture = audit.read_json(Path(__file__).with_name("fixtures") /
                                  "spy-economic-v2-floor-promotions-v1.json")
        self.assertEqual(len(fixture["contracts"]), 8)
        for contract in fixture["contracts"]:
            cold = contract["cold_node"]
            zero, ceiling = audit.zero_volatility_bounds(cold["spot"], contract["strike"],
                cold["variance_years"], cold["effective_rate"], cold["effective_yield"], contract["side"])
            self.assertLessEqual(contract["target_price"], zero + 1e-9 * ceiling + 1e-12,
                                 contract["row_id"])
            self.assertEqual(contract["prior_price_only_reason"], "floor_vol")

    def test_no_identified_nodes_or_changed_context_cannot_pass(self):
        report, _, rows = self.fixture()
        entry = report["graded_stats"]["acceptance_audit"]["snapshots"][0][0]
        for query in entry["queries"]:
            query.update(status="price_only", volatility=None, reason="low_vega", vol_convention="none")
        self.assertFalse(audit.cold_floor_audit(report, rows)["floor_semantics_passed"])
        report, query, rows = self.fixture()
        query["cold_node"]["effective_yield"] = 0.1
        self.assertFalse(audit.cold_floor_audit(report, rows)["floor_semantics_passed"])


class ColdClockOracleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def timing_fixture(self, count=100, native_cold=0.5):
        predictions = self.directory / "predictions.csv"
        predictions.write_text("same actual predictions\n", encoding="utf-8")
        prediction_hash = audit.sha256(predictions)
        self.receipt = {"binary_sha256": "b" * 64, "input_sha256": "a" * 64,
                        "policy_sha256": "c" * 64, "predictions_sha256": prediction_hash}
        self.report = {"policy_definition": {"test_policy": True}}
        manifest = {"binary": {"sha256": self.receipt["binary_sha256"]},
                    "input": {"sha256": self.receipt["input_sha256"]},
                    "policy": {"sha256": self.receipt["policy_sha256"]}, "predictions_sha256": prediction_hash,
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

    def test_diagnostic_capture_clock_cannot_be_promoted(self):
        for marker in ({"candidate_capture": {"valid": True}}, {"timing_qualifying": False}):
            with self.subTest(marker=marker):
                manifest = self.timing_fixture()
                trial = manifest["trials"][0]
                path = self.directory / trial["report"]
                native = audit.read_json(path)
                native["bench"].update(marker)
                path.write_text(json.dumps(native), encoding="utf-8")
                trial["report_sha256"] = audit.sha256(path)
                self.assertFalse(self.checks(manifest)["native_reports_verified"])

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
        self.assertFalse(self.checks(manifest)["prediction_artifacts_verified"])

    def different_solution(self):
        manifest = self.timing_fixture()
        path = self.directory / "different-valid-solution.csv"
        path.write_text("a different numerical solution requiring its own proof\n", encoding="utf-8")
        manifest["predictions_identical"] = False
        manifest["trials"][1].update(predictions=path.name, predictions_sha256=audit.sha256(path))
        return manifest

    def test_valid_different_solutions_do_not_require_identical_prediction_hashes(self):
        manifest = self.different_solution()
        checked = []

        def validate(trial, native, predictions):
            checked.append(trial["trial"])
            self.assertEqual(audit.sha256(predictions), trial["predictions_sha256"])
            self.assertEqual(native["bench"]["process_identity"], trial["process_identity"])
            return True  # This unit isolates dispatch; the real validator runs every API gate.

        checks = audit.cold_timing_checks(manifest, self.directory, self.receipt, self.report,
                                          solution_validator=validate)
        self.assertTrue(all(checks.values()), checks)
        self.assertEqual(checked, [2])

    def test_changed_solution_requires_its_own_successful_qualification(self):
        manifest = self.different_solution()
        for validator in (None, lambda *args: False, lambda *args: {"passed": True}):
            checks = audit.cold_timing_checks(manifest, self.directory, self.receipt, self.report,
                                              solution_validator=validator)
            self.assertFalse(checks["every_solution_qualified"])

    def post_fit_fixture(self):
        manifest = self.timing_fixture(count=1)
        trial = manifest["trials"][0]
        native = audit.read_json(self.directory / trial["report"])
        marker = {"schema": "atx-vol-post-fit-api-proof-v1",
            "phase": "after_cold_publication_and_fit_snapshots",
            "cold_clock_contract": "retained_surface_publication_v1",
            "same_retained_instance": True, "audit_completed": True, "failure_reason": "",
            "attempted_snapshot_count": 1, "retained_snapshot_count": 1, "serialized_snapshot_count": 1,
            "attempted_member_count": 2, "retained_member_count": 2, "serialized_member_count": 2,
            "attempted_chain_ids": [10, 11], "retained_chain_ids": [10, 11],
            "serialized_chain_ids": [10, 11], "unsupported_chain_ids": [], "missing_owner_chain_ids": [],
            "graded_surface_fit_invocations_before": 1, "graded_surface_fit_invocations_after": 1,
            "refit_count": 0, "input_sha256": self.receipt["input_sha256"],
            "policy_sha256": self.receipt["policy_sha256"], "executable_sha256": self.receipt["binary_sha256"],
            "prediction_sha256": trial["predictions_sha256"], "audit_s": 0.01,
            "retained_owner_destruction_s": 0.001, "retained_snapshot_capacity": 1,
            "retained_member_capacity": 2, "retained_fit_capacity": 2,
            "retention_setup_in_cold": True, "retained_owner_released_after_audit": True}
        native["bench"]["post_fit_acceptance_audit"] = marker
        native["graded_stats"] = {"acceptance_audit": {"schema": "atx-vol-final-api-audit-v1",
            "phase": "post_fit", "timing_qualifying": False,
            "snapshots": [[{"chain_id": 10}, {"chain_id": 11}]]}}
        return manifest, trial, native, marker

    def save_trial(self, trial, native):
        path = self.directory / trial["report"]
        path.write_text(json.dumps(native), encoding="utf-8")
        trial["report_sha256"] = audit.sha256(path)

    def test_post_fit_proof_marker_joins_ownership_phase_and_own_artifacts(self):
        _, trial, native, _ = self.post_fit_fixture()
        self.assertTrue(all(audit.post_fit_proof_checks(native, trial, self.receipt).values()))
        mutations = {"schema": "other", "phase": "inside_fit", "cold_clock_contract": "other",
            "same_retained_instance": False, "audit_completed": False, "failure_reason": "missing",
            "attempted_snapshot_count": 2, "retained_snapshot_count": True,
            "serialized_snapshot_count": 0, "attempted_member_count": 1, "retained_member_count": 0,
            "serialized_member_count": 1, "attempted_chain_ids": [10, 10],
            "retained_chain_ids": [10], "serialized_chain_ids": [10, 12],
            "unsupported_chain_ids": [10], "missing_owner_chain_ids": [11],
            "graded_surface_fit_invocations_before": 0, "graded_surface_fit_invocations_after": 2,
            "refit_count": 1, "input_sha256": "0" * 64, "policy_sha256": "0" * 64,
            "executable_sha256": "0" * 64, "prediction_sha256": "0" * 64,
            "audit_s": float("nan"), "retained_owner_destruction_s": -1,
            "retained_snapshot_capacity": 0, "retained_member_capacity": 1, "retained_fit_capacity": True,
            "retention_setup_in_cold": False, "retained_owner_released_after_audit": False}
        for field, value in mutations.items():
            bad = copy.deepcopy(native)
            bad["bench"]["post_fit_acceptance_audit"][field] = value
            with self.subTest(field=field):
                self.assertFalse(all(audit.post_fit_proof_checks(bad, trial, self.receipt).values()))
        for field, value in (("phase", "inline"), ("snapshots", [[{"chain_id": 10}]])):
            bad = copy.deepcopy(native)
            bad["graded_stats"]["acceptance_audit"][field] = value
            self.assertFalse(all(audit.post_fit_proof_checks(bad, trial, self.receipt).values()))

    def test_marked_trial_requires_own_full_proof_even_when_predictions_match(self):
        manifest, trial, native, _ = self.post_fit_fixture()
        self.save_trial(trial, native)
        for validator, expected in ((None, False), (lambda *args: False, False), (lambda *args: True, True)):
            checks = audit.cold_timing_checks(manifest, self.directory, self.receipt, self.report,
                                              solution_validator=validator)
            self.assertTrue(checks["native_reports_verified"])
            self.assertEqual(checks["every_solution_qualified"], expected)
        # The lifetime change must be explicit and consistent across a campaign.
        legacy = copy.deepcopy(native)
        del legacy["bench"]["post_fit_acceptance_audit"]
        del legacy["graded_stats"]
        legacy["bench"]["process_identity"] = {"pid": 999, "creation_filetime": "999"}
        other = dict(trial, trial=2, report="legacy.json", process_identity=legacy["bench"]["process_identity"])
        self.save_trial(other, legacy)
        manifest["trials"].append(other)
        checks = audit.cold_timing_checks(manifest, self.directory, self.receipt, self.report,
                                          solution_validator=lambda *args: True)
        self.assertFalse(checks["consistent_cold_clock_contract"])

    def test_qualify_dispatches_each_marked_trial_to_full_solution_verifier(self):
        manifest, trial, native, _ = self.post_fit_fixture()
        self.save_trial(trial, native)
        audit.write_json(self.directory / "native.json", self.report)
        audit.write_json(self.directory / "receipt.json", self.receipt)
        audit.write_json(self.directory / "cold.json", manifest)
        args = SimpleNamespace(input=self.directory / "input.csv", audit_dir=self.directory,
            command="qualify", cold_manifest=self.directory / "cold.json", build_cache=None,
            accuracy_contract=audit.RAW_SPREAD_PRICE_CONTRACT,
            identification_contract=audit.ECONOMIC_IDENTIFICATION_CONTRACT)
        for own_passes in (True, False):
            baseline = {"correctness_passed": True, "checks": {}, "goal_achieved": False}
            own = {"correctness_passed": own_passes, "checks": {"own_financial_gate": own_passes}}
            with patch.object(audit, "load_quotes", return_value=[]), \
                 patch.object(audit, "verify_solution", side_effect=[baseline, own]) as validator:
                result = audit.verify(args, {})
                self.assertEqual(validator.call_count, 2)
                actual = validator.call_args.args
                self.assertEqual(actual[0], native)
                self.assertEqual(actual[1]["report_sha256"], trial["report_sha256"])
                self.assertEqual(actual[2], self.directory / trial["predictions"])
                self.assertEqual(actual[3], self.directory / trial["report"])
                self.assertEqual(actual[-2:], (args.accuracy_contract, args.identification_contract))
                self.assertEqual(result["timing_checks"]["every_solution_qualified"], own_passes)
                self.assertEqual(result["timed_solution_verifications"][0]["checks"], own["checks"])
                self.assertFalse(result["goal_achieved"])  # One synthetic trial is never 100.

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
