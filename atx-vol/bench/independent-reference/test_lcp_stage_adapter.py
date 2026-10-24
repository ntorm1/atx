"""Tiny adapter controls only; none of the twelve prospective grids run here."""
import sys
import unittest
from unittest.mock import patch

import numpy as np

import greek_reference as original
from lcp_correction import LcpCorrectionRefused, solve_obstacle_control
from lcp_stage_adapter import StageCollector, private_reference


MODEL = dict(S=100., K=100., T=.1, sigma=.2, r=0., q=0., side="put")


class StageAdapterTests(unittest.TestCase):
    def test_private_original_namespace_preserves_default_bits(self):
        existing = original.solve_obstacle
        function = private_reference("original")
        self.assertIsNot(function.__globals__, original.greek_reference.__globals__)
        self.assertIs(function.__globals__["solve_obstacle"], existing)
        answer = function(MODEL, nx=33, nt=4)
        self.assertEqual(answer, original.greek_reference(MODEL, nx=33, nt=4))
        self.assertIs(original.solve_obstacle, existing)
        self.assertNotIn("_isolated_lcp_greek_reference", sys.modules)

    def test_explicit_correction_collects_actual_tiny_stage_work_only(self):
        existing = original.solve_obstacle
        collector = StageCollector()
        function = private_reference("one_correction", collector)
        answer = function(MODEL, nx=33, nt=4)
        self.assertEqual(answer["stages"], 6)
        self.assertEqual(len(collector.records), 6)
        self.assertIsNone(collector.failure_arrays)
        for index, row in enumerate(collector.records, 1):
            self.assertEqual((row["stage"], row["status"]), (index, "computed"))
            work = row["work"]
            self.assertEqual(work["mode"], "one_correction")
            self.assertEqual(work["policy_solves"], work["correction_solves"])
            self.assertEqual(work["linear_solves"], 2*work["iterations"])
            self.assertEqual(work["policy_iteration_bound"], 33)
            self.assertLessEqual(work["complementarity_residual"], 1e-10)
            self.assertNotIn("values", work)
        self.assertEqual(sum(r["work"]["iterations"] for r in collector.records),
                         answer["policy_iterations"])
        self.assertIs(original.solve_obstacle, existing)
        with self.assertRaises(ValueError):
            private_reference("one_correction", collector)

    def test_typed_refusal_keeps_actual_vector_and_unknown_work_is_not_zeroed(self):
        collector = StageCollector()
        arrays = [np.zeros(1), np.ones(2), np.zeros(1), np.ones(2),
                  np.zeros(2), np.zeros(2)]
        def limited(*args, **kwargs):
            return solve_obstacle_control(*args, **kwargs, max_linear_solves=0)
        with patch("lcp_correction.solve_obstacle_control", limited):
            with self.assertRaises(LcpCorrectionRefused):
                collector(*arrays, tolerance=1e-10)
        row = collector.records[0]
        self.assertEqual(row["status"], "refused")
        self.assertEqual(row["work"]["linear_solves"], 0)
        self.assertEqual(row["work"]["last_checked_residual"], 1.)
        np.testing.assert_array_equal(collector.failure_arrays["last_checked_values"], arrays[-1])
        arrays[-1][0] = 100.
        self.assertEqual(collector.failure_arrays["initial"][0], 0.)
        other = StageCollector()
        with patch("lcp_correction.solve_obstacle_control", side_effect=ValueError("invalid")):
            with self.assertRaises(ValueError):
                other(*arrays, tolerance=1e-10)
        self.assertEqual(other.records[0]["status"], "unobserved_failure")
        self.assertIsNone(other.records[0]["work"])
        self.assertIsNone(other.failure_arrays)

    def test_invalid_modes_and_missing_collectors_refuse_before_loading(self):
        with patch("lcp_stage_adapter.importlib.util.spec_from_file_location") as load:
            for mode, collector in ((None, None), ("unknown", None),
                                    ("one_correction", None), ("original", StageCollector())):
                with self.subTest(mode=mode), self.assertRaises(ValueError):
                    private_reference(mode, collector)
            load.assert_not_called()


if __name__ == "__main__":
    unittest.main()
