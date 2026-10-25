"""Primitive-only tests; no twelve-grid or full Greek pilot is run here."""
from decimal import Decimal, localcontext
from fractions import Fraction
import hashlib
import itertools
import json
import math
from pathlib import Path
import unittest
from unittest.mock import patch

import numpy as np
from scipy.linalg import solve_banded

from lcp_correction import LcpCorrectionRefused, solve_obstacle_control
from pde_reference import _multiply, solve_obstacle


FIXTURE = Path(__file__).with_name("fixtures") / "zero-rate-dirkl-stage60-v1.npz"


def captured():
    meta = json.loads(FIXTURE.with_suffix(".json").read_text(encoding="utf-8"))
    if hashlib.sha256(FIXTURE.read_bytes()).hexdigest() != meta["fixture_sha256"]:
        raise AssertionError("Captured obstacle fixture hash mismatch")
    with np.load(FIXTURE, allow_pickle=False) as data:
        keys = ("lower", "diagonal", "upper", "rhs", "payoff", "initial")
        arrays = [data[key].copy() for key in keys]
        for key, value in zip(keys, arrays):
            if hashlib.sha256(value.tobytes()).hexdigest() != meta["arrays"][key]["sha256"]:
                raise AssertionError("Captured obstacle array mismatch")
    return arrays, meta["tolerance"]


def decimal_complementarity(arrays, values):
    """Re-evaluate all original rows at 100 digits from exact binary inputs."""
    lower, diagonal, upper, rhs, payoff, _ = arrays
    with localcontext() as context:
        context.prec = 100
        d = lambda x: Decimal.from_float(float(x))
        maximum = Decimal(0)
        for i, value in enumerate(values):
            continuation = d(diagonal[i])*d(value)-d(rhs[i])
            if i:
                continuation += d(lower[i-1])*d(values[i-1])
            if i+1 < len(values):
                continuation += d(upper[i])*d(values[i+1])
            maximum = max(maximum, abs(min(continuation, d(value)-d(payoff[i]))))
        return maximum


def rational_obstacle(matrix, rhs, payoff):
    """Independent exhaustive exact policy solve, only for tiny integer controls."""
    n = len(rhs)
    answers = set()
    for policy in itertools.product((False, True), repeat=n):
        rows = [[Fraction(int(i == j)) if policy[i] else Fraction(matrix[i][j])
                 for j in range(n)] + [Fraction(payoff[i] if policy[i] else rhs[i])]
                for i in range(n)]
        for column in range(n):
            pivot = next(i for i in range(column, n) if rows[i][column])
            rows[column], rows[pivot] = rows[pivot], rows[column]
            scale = rows[column][column]
            rows[column] = [x/scale for x in rows[column]]
            for i in range(n):
                if i != column:
                    scale = rows[i][column]
                    rows[i] = [a-scale*b for a, b in zip(rows[i], rows[column])]
        values = tuple(row[-1] for row in rows)
        continuation = [sum(Fraction(a)*v for a, v in zip(row, values))-b
                        for row, b in zip(matrix, rhs)]
        if all(min(c, v-p) == 0 for c, v, p in zip(continuation, values, payoff)):
            answers.add(values)
    if len(answers) != 1:
        raise AssertionError("Independent control lacks a unique LCP solution")
    return np.array([float(x) for x in answers.pop()])


class CorrectionTests(unittest.TestCase):
    def small(self):
        return [np.array([-1., -1.]), np.array([3., 3., 3.]), np.array([-1., -1.]),
                np.array([-1., 1., 3.]), np.zeros(3), np.zeros(3)]

    def test_captured_failed_stage_uses_two_solves_per_policy_and_original_tolerance(self):
        arrays, tolerance = captured()
        with patch("lcp_correction.solve_banded", wraps=solve_banded, create=True) as calls:
            result = solve_obstacle_control(*arrays, tolerance, mode="one_correction")
        self.assertEqual(result["policy_solves"], 2)
        self.assertEqual(result["correction_solves"], 2)
        self.assertEqual(result["linear_solves"], 4)
        self.assertEqual(calls.call_count, 4)
        self.assertLessEqual(result["complementarity_residual"], tolerance)
        self.assertLessEqual(decimal_complementarity(arrays, result["values"]),
                             Decimal.from_float(tolerance))
        actual = float(np.max(np.abs(np.minimum(_multiply(*arrays[:3], result["values"])-arrays[3],
                                               result["values"]-arrays[4]))))
        self.assertEqual(actual, result["complementarity_residual"])

    def test_small_obstacle_matches_independent_exact_policies(self):
        expected = rational_obstacle([[3, -1, 0], [-1, 3, -1], [0, -1, 3]], [-1, 1, 3], [0, 0, 0])
        result = solve_obstacle_control(*self.small(), mode="one_correction")
        np.testing.assert_allclose(result["values"], expected, atol=1e-14, rtol=0)
        self.assertEqual(result["linear_solves"], 2*result["iterations"])
        self.assertEqual(result["correction_solves"], result["policy_solves"])

    def test_low_budget_refuses_before_partial_pair(self):
        with self.assertRaises(LcpCorrectionRefused) as caught:
            solve_obstacle_control(*self.small(), mode="one_correction", max_linear_solves=1)
        state = caught.exception.diagnostics
        self.assertEqual(state["reason"], "linear_budget")
        self.assertEqual(state["linear_solves"], 0)
        self.assertEqual(state["policy_solves"], 0)
        self.assertEqual(state["correction_solves"], 0)

    def test_default_and_explicit_original_keep_old_results_and_inputs(self):
        arrays = self.small()
        before = [value.copy() for value in arrays]
        expected = solve_obstacle(*arrays)
        with patch("lcp_correction._one_correction") as experimental:
            for kwargs in ({}, {"mode": "original"}):
                actual = solve_obstacle_control(*arrays, **kwargs)
                self.assertEqual(actual.keys(), expected.keys())
                np.testing.assert_array_equal(actual["values"], expected["values"])
                self.assertEqual(actual["iterations"], expected["iterations"])
                self.assertEqual(actual["complementarity_residual"],
                                 expected["complementarity_residual"])
            experimental.assert_not_called()
        for actual, saved in zip(arrays, before):
            np.testing.assert_array_equal(actual, saved)

    def test_exercise_continuation_and_mandatory_correction_equations(self):
        for rhs, expected in (([-2., -2.], [0., 0.]), ([1., 1.], [1., 1.])):
            arrays = [np.array([-1.]), np.array([2., 2.]), np.array([-1.]),
                      np.array(rhs), np.zeros(2), np.full(2, -1.)]
            original = [value.copy() for value in arrays]
            calls = []

            def observe(shape, band, target, **kwargs):
                answer = solve_banded(shape, band, target, **kwargs)
                calls.append((band.copy(), target.copy(), answer.copy()))
                return answer

            with self.subTest(rhs=rhs), patch("lcp_correction.solve_banded", observe):
                actual = solve_obstacle_control(*arrays, mode="one_correction")
                np.testing.assert_array_equal(actual["values"], expected)
                self.assertFalse(actual["values"].flags.writeable)
                self.assertEqual(len(calls), 2*actual["iterations"])
                for policy, correction in zip(calls[::2], calls[1::2]):
                    np.testing.assert_array_equal(policy[0], correction[0])
                    band = policy[0]
                    residual = policy[1]-_multiply(band[2, :-1], band[1],
                                                   band[0, 1:], policy[2])
                    np.testing.assert_array_equal(correction[1], residual)
                # A zero selected-system residual still gets exactly one solve.
                self.assertTrue(any(np.all(call[1] == 0.) for call in calls[1::2]))
                for value, saved in zip(arrays, original):
                    np.testing.assert_array_equal(value, saved)

    def test_already_solved_initial_uses_no_linear_budget(self):
        arrays = self.small()
        arrays[-1] = rational_obstacle([[3, -1, 0], [-1, 3, -1], [0, -1, 3]],
                                      [-1, 1, 3], [0, 0, 0])
        with patch("lcp_correction.solve_banded") as linear:
            result = solve_obstacle_control(*arrays, mode="one_correction", max_linear_solves=0)
        linear.assert_not_called()
        self.assertEqual(result["iterations"], 0)
        self.assertEqual(result["linear_solves"], 0)
        self.assertEqual(result["residual_checks"], 1)
        np.testing.assert_array_equal(result["values"], arrays[-1])
        self.assertFalse(np.shares_memory(result["values"], arrays[-1]))

    def test_partial_budget_keeps_the_actual_checked_pair(self):
        arrays, tolerance = captured()
        with patch("lcp_correction.solve_banded", wraps=solve_banded) as calls:
            with self.assertRaises(LcpCorrectionRefused) as caught:
                solve_obstacle_control(*arrays, tolerance, mode="one_correction",
                                       max_linear_solves=3)
        state = caught.exception.diagnostics
        self.assertEqual(state["reason"], "linear_budget")
        self.assertEqual(calls.call_count, 2)
        self.assertEqual(state["iterations"], 1)
        self.assertEqual(state["policy_solves"], 1)
        self.assertEqual(state["correction_solves"], 1)
        self.assertEqual(state["residual_checks"], 2)
        values = state["last_checked_values"]
        actual = float(np.max(np.abs(np.minimum(_multiply(*arrays[:3], values)-arrays[3],
                                               values-arrays[4]))))
        self.assertEqual(actual, state["last_checked_residual"])
        self.assertGreater(actual, tolerance)
        self.assertFalse(values.flags.writeable)
        self.assertFalse(np.shares_memory(values, arrays[-1]))

    def test_iteration_cap_cannot_be_enlarged_and_last_vector_is_not_stale(self):
        arrays = [np.zeros(1), np.ones(2), np.zeros(1), np.full(2, 2.),
                  np.zeros(2), np.zeros(2)]
        outputs = [value for i in range(1, 5) for value in
                   (np.full(2, i/8.), np.zeros(2))]
        with patch("lcp_correction.solve_banded", side_effect=outputs) as calls:
            with self.assertRaises(LcpCorrectionRefused) as caught:
                solve_obstacle_control(*arrays, mode="one_correction", max_linear_solves=1000)
        state = caught.exception.diagnostics
        self.assertEqual(state["reason"], "policy_iteration_limit")
        self.assertEqual(state["iterations"], 4)
        self.assertEqual(state["policy_iteration_bound"], 4)
        self.assertEqual(state["linear_solve_budget"], 8)
        self.assertEqual(state["linear_solve_bound"], 8)
        self.assertEqual(state["linear_solves"], calls.call_count)
        self.assertEqual(calls.call_count, 8)
        self.assertEqual(state["residual_checks"], 5)
        np.testing.assert_array_equal(state["last_checked_values"], [.5, .5])
        self.assertEqual(state["last_checked_residual"], 1.5)
        self.assertNotEqual(state["last_checked_residual"], 1.625)

    def test_final_permitted_update_is_checked(self):
        arrays = [np.zeros(1), np.ones(2), np.zeros(1), np.full(2, 2.),
                  np.zeros(2), np.zeros(2)]
        outputs = [value for i in range(1, 5) for value in
                   (np.full(2, 2. if i == 4 else i/8.), np.zeros(2))]
        with patch("lcp_correction.solve_banded", side_effect=outputs) as calls:
            result = solve_obstacle_control(*arrays, mode="one_correction")
        self.assertEqual(result["iterations"], 4)
        self.assertEqual(calls.call_count, 8)
        self.assertEqual(result["complementarity_residual"], 0.)
        np.testing.assert_array_equal(result["values"], [2., 2.])

    def test_invalid_inputs_refuse_before_solves(self):
        invalid_arrays = []
        for index, bad in ((0, np.zeros(3)), (1, np.array(3.)), (2, np.ones(2)),
                           (3, np.array([1., math.nan, 3.])), (4, np.zeros((3, 1))),
                           (5, np.zeros(2)), (1, np.full(3, .5)),
                           (3, np.array([1.+2.j, 1., 3.])), (1, [10**1000]*3)):
            arrays = self.small()
            arrays[index] = bad
            invalid_arrays.append(arrays)
        invalid_arrays.append([np.array([]) for _ in range(6)])
        with patch("lcp_correction.solve_banded") as calls:
            for arrays in invalid_arrays:
                with self.subTest(arrays=arrays), self.assertRaises(ValueError):
                    solve_obstacle_control(*arrays, mode="one_correction")
            for tolerance in (0., -1., math.nan, math.inf, True, None, "1e-9", 10**1000):
                with self.subTest(tolerance=tolerance), self.assertRaises(ValueError):
                    solve_obstacle_control(*self.small(), tolerance, mode="one_correction")
            for budget in (-1, 1.5, True, "4"):
                with self.subTest(budget=budget), self.assertRaises(ValueError):
                    solve_obstacle_control(*self.small(), mode="one_correction",
                                           max_linear_solves=budget)
            for mode in (None, "refined", True):
                with self.subTest(mode=mode), self.assertRaises(ValueError):
                    solve_obstacle_control(*self.small(), mode=mode)
            with self.assertRaises(ValueError):
                solve_obstacle_control(*self.small(), max_linear_solves=0)
        calls.assert_not_called()

    def test_nonfinite_stages_and_solver_failure_report_attempted_work(self):
        arrays = [np.zeros(1), np.ones(2), np.zeros(1), np.ones(2),
                  np.zeros(2), np.zeros(2)]
        controls = (
            ([np.full(2, math.inf)], "nonfinite_policy_solves", 1, 0),
            ([np.ones(2), np.full(2, math.nan)], "nonfinite_correction_solves", 1, 1),
            ([np.full(2, 1e308), np.full(2, 1e308)], "nonfinite_corrected_values", 1, 1),
            ([np.linalg.LinAlgError("controlled failure")], "linear_solve_failure", 1, 0),
        )
        for outputs, reason, policies, corrections in controls:
            with self.subTest(reason=reason), \
                    patch("lcp_correction.solve_banded", side_effect=outputs) as calls:
                with self.assertRaises(LcpCorrectionRefused) as caught:
                    solve_obstacle_control(*arrays, mode="one_correction")
                state = caught.exception.diagnostics
                self.assertEqual(state["reason"], reason)
                self.assertEqual(state["policy_solves"], policies)
                self.assertEqual(state["correction_solves"], corrections)
                self.assertEqual(state["linear_solves"], policies+corrections)
                self.assertEqual(calls.call_count, policies+corrections)
                self.assertEqual(state["iterations"], 0)
                self.assertEqual(state["last_checked_residual"], 1.)
                np.testing.assert_array_equal(state["last_checked_values"], arrays[-1])

    def test_nonfinite_residuals_refuse_without_claiming_a_finite_final_pair(self):
        arrays = [np.zeros(1), np.full(2, 2.), np.zeros(1), np.ones(2),
                  np.zeros(2), np.full(2, 1e308)]
        with patch("lcp_correction.solve_banded") as calls:
            with self.assertRaises(LcpCorrectionRefused) as caught:
                solve_obstacle_control(*arrays, mode="one_correction")
        calls.assert_not_called()
        state = caught.exception.diagnostics
        self.assertEqual(state["reason"], "nonfinite_original_residual")
        self.assertIsNone(state["last_checked_values"])
        self.assertIsNone(state["last_checked_residual"])
        arrays[-1] = np.zeros(2)
        with patch("lcp_correction.solve_banded", return_value=np.full(2, 1e308)) as calls:
            with self.assertRaises(LcpCorrectionRefused) as caught:
                solve_obstacle_control(*arrays, mode="one_correction")
        self.assertEqual(calls.call_count, 1)
        state = caught.exception.diagnostics
        self.assertEqual(state["reason"], "nonfinite_correction_rhs")
        self.assertEqual(state["policy_solves"], 1)
        self.assertEqual(state["correction_solves"], 0)
        self.assertEqual(state["last_checked_residual"], 1.)

    def test_single_row_boundary_still_executes_one_correction(self):
        for rhs, expected in ((-1., 0.), (1., .5)):
            with self.subTest(rhs=rhs):
                arrays = [np.array([]), np.array([2.]), np.array([]), np.array([rhs]),
                          np.array([0.]), np.array([1.])]
                result = solve_obstacle_control(*arrays, mode="one_correction")
                np.testing.assert_array_equal(result["values"], [expected])
                self.assertEqual(result["linear_solves"], 2)
                self.assertEqual(result["policy_solves"], 1)
                self.assertEqual(result["correction_solves"], 1)


if __name__ == "__main__":
    unittest.main()
