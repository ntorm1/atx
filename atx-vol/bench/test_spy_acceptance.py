"""Adversarial tests of the acceptance oracle, independent of fitter internals."""
import copy
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
                            "served_price": price, "price": price, "volatility": None})
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
    def test_genuine_price_only_domain_passes(self):
        self.assertEqual(audit.api_violations(document(surface())), [])

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

    def test_discounted_terminal_floor_is_checked_with_nonzero_carry(self):
        value = surface()
        value["market"]["rate"] = 0.1
        value["market"]["continuous_yield"] = 1e-8
        self.assertTrue(any(v["kind"] == "terminal_european_floor"
                            for v in audit.api_violations(document(value))))


class ColdClockOracleTests(unittest.TestCase):
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
