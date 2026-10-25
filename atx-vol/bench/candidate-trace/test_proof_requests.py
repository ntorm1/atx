import copy
import struct
import unittest

import proof_requests
from test_capture_audit import fixture, options


class ProofRequests(unittest.TestCase):
    def test_exact_seed_and_null_options_preserved(self):
        source = fixture()
        result = proof_requests.extract(source)
        row = result["requests"][0]
        self.assertEqual(struct.pack(">d", row["sigma"]).hex(),
                         source["records"][0]["trace"]["phases"][1]["seed_sigma"]["bits"])
        self.assertFalse(row["opts_explicit"])
        self.assertEqual([row[k] for k in ("n_collocation", "n_quadrature", "n_quad_price",
                         "max_newton_iter", "opts_tol", "profile")], [12, 24, 48, 6, 1e-10, 0])
        self.assertEqual(result["extraction"]["proof_requests"], 1)
        self.assertEqual(result["extraction"]["source_join"],
                         [dict(index=0, sequence=0, phase=1,
                               seed_bits=struct.pack(">d", .35).hex())])

    def test_bad_capture_is_not_a_subset_escape(self):
        source = fixture()
        source["attempted_calls"] = 2
        with self.assertRaises(ValueError):
            proof_requests.extract(source)

    def test_no_seed_refusal_stays_in_the_full_capture_denominator(self):
        source = fixture()
        refused = copy.deepcopy(source["records"][0])
        refused["sequence"] = 1
        trace = refused["trace"]
        empty = copy.deepcopy(trace["phases"][2])
        first = copy.deepcopy(trace["phases"][0])
        trace["phases"] = [dict(copy.deepcopy(empty), phase=i) for i in range(6)]
        trace["phases"][0] = dict(first, outcome=3, sigma=None, error=0)
        trace["phases"][2] = dict(first, phase=2, outcome=3, sigma=None, error=0,
                                  options=options(12, 24, 48, 6, 1e-10, 1))
        trace.update(outcome=3, sigma=None, work=[2, 2, 2], floor_hits=0)
        trace["floor_before"] = copy.deepcopy(trace["floor_after"])
        trace["stats"] = {k: k == "screen_attempted" for k in trace["stats"]}
        source["records"].append(refused)
        source["attempted_calls"] = 2
        source["caches"][0]["closed_after_calls"] = 2
        source["slice_counters"]["slices"][0]["timing"].update(
            reimply_candidate_attempts=2, reimply_screen_attempts=2)
        result = proof_requests.extract(source)
        self.assertEqual(len(result["requests"]), 1)
        self.assertEqual(result["extraction"]["capture_attempts"], 2)
        self.assertEqual(result["extraction"]["capture_refused"], 1)
        self.assertEqual(result["extraction"]["calls_without_proof"],
                         [dict(sequence=1, outcome=3, phase_outcomes=[3, 0, 3, 0, 0, 0])])

    def test_explicit_reference_options_do_not_become_null(self):
        source = fixture()
        opts = options(12, 24, 48, 6, 1e-10, 0)
        trace = source["records"][0]["trace"]
        trace["options"]["al_options"] = copy.deepcopy(opts)
        trace["phases"][1]["options"] = copy.deepcopy(opts)
        for state in (trace["floor_before"], trace["floor_after"], source["caches"][0]["final_state"]):
            state["slots"][1]["options"] = copy.deepcopy(opts)
        result = proof_requests.extract(source)
        self.assertTrue(result["requests"][0]["opts_explicit"])


if __name__ == "__main__":
    unittest.main()
