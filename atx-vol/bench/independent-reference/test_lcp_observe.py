"""Tiny controls only: no saved four-stage input or PDE runner is used."""
import copy
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

import numpy as np

import lcp_correction
import lcp_observe
import pde_reference
from lcp_observe import observe_stage, recurrence, verify_observation


def small():
    return [np.array([-1., -1.]), np.array([3., 3., 3.]), np.array([-1., -1.]),
            np.array([-1., 1., 3.]), np.zeros(3), np.zeros(3)]


class ObservationTests(unittest.TestCase):
    def test_private_seam_matches_original_bits_and_retains_actual_work(self):
        arrays = small()
        before = [a.copy() for a in arrays]
        multiply, linear = pde_reference._multiply, lcp_correction.solve_banded
        expected = lcp_correction.solve_obstacle_control(*arrays, mode='one_correction')
        actual = observe_stage(arrays, tolerance=1e-11)
        self.assertEqual(actual['status'], 'computed')
        self.assertEqual(actual['work']['iterations'], expected['iterations'])
        self.assertEqual(actual['work']['linear_solves'], expected['linear_solves'])
        self.assertEqual(actual['linear_calls'], expected['linear_solves'])
        np.testing.assert_array_equal(actual['values'][-1].view(np.uint64),
                                      expected['values'].view(np.uint64))
        self.assertEqual(len(actual['states']), expected['iterations']+1)
        self.assertTrue(verify_observation(arrays, actual, tolerance=1e-11))
        self.assertFalse(actual['values'].flags.writeable)
        self.assertFalse(actual['policies'].flags.writeable)
        self.assertIs(pde_reference._multiply, multiply)
        self.assertIs(lcp_correction.solve_banded, linear)
        self.assertNotIn('_lcp_observed_primitive', sys.modules)
        for value, saved in zip(arrays, before):
            np.testing.assert_array_equal(value.view(np.uint64), saved.view(np.uint64))

    def test_two_laps_required_and_hash_collisions_never_prove_equality(self):
        values = np.array([[0., 1.], [1., 0.], [0., 1.], [1., 0.], [0., 1.]])
        policies = values.astype(bool)
        residuals = [1.]*5
        self.assertIsNone(recurrence(values[:3], policies[:3], residuals[:3])['confirmed'])
        answer = recurrence(values, policies, residuals)
        self.assertEqual(answer['confirmed'], dict(kind='cycle', start=0, period=2, end=4))
        with patch('lcp_observe._digest', return_value='collision'):
            unique = np.array([[0.], [1.], [2.], [3.], [4.]])
            other = recurrence(unique, np.zeros_like(unique, dtype=bool), residuals)
        self.assertIsNone(other['first_state_repeat'])
        self.assertIsNone(other['confirmed'])
        self.assertIsNotNone(other['first_policy_repeat'])

    def test_signed_zero_policy_and_residual_bits_are_part_of_state(self):
        values = np.array([[0.], [-0.], [0.]])
        policies = np.zeros((3, 1), dtype=bool)
        self.assertIsNone(recurrence(values, policies, [1.]*3)['confirmed'])
        values[:] = 0.
        self.assertEqual(recurrence(values, policies, [1.]*3)['confirmed']['kind'], 'stagnation')
        policies[1, 0] = True
        self.assertIsNone(recurrence(values, policies, [1.]*3)['confirmed'])
        policies[:] = False
        self.assertIsNone(recurrence(values, policies, [1., np.nextafter(1., 2.), 1.])['confirmed'])

    def test_resource_cap_is_observation_not_an_enlarged_algorithm_bound(self):
        answer = observe_stage(small(), tolerance=1e-11, max_updates=0)
        self.assertEqual(answer['status'], 'refused')
        self.assertEqual(answer['work']['reason'], 'linear_budget')
        self.assertEqual(answer['work']['policy_iteration_bound'], 5)
        self.assertEqual(answer['linear_calls'], 0)
        self.assertEqual(len(answer['states']), 1)
        self.assertTrue(answer['observation_cap_reached'])
        self.assertTrue(verify_observation(small(), answer, tolerance=1e-11))
        for limit in (-1, True, 1.5, 129):
            with self.subTest(limit=limit), self.assertRaises(ValueError):
                observe_stage(small(), max_updates=limit)
        arrays = small()
        arrays[-1][:] = -1.  # First unconstrained solve still violates the obstacle.
        limited = observe_stage(arrays, tolerance=1e-11, max_updates=1)
        self.assertTrue(limited['observation_cap_reached'])
        self.assertEqual(limited['linear_calls'], 2)
        self.assertEqual(limited['work']['iterations'], 1)
        self.assertEqual(len(limited['states']), 2)
        self.assertEqual(limited['allocated_state_bytes'], 2*3*9)
        self.assertTrue(verify_observation(arrays, limited, tolerance=1e-11))

    def test_storage_and_shape_refuse_before_any_linear_work(self):
        with patch('lcp_observe._delegate_solve') as linear:
            for budget in (0, 1, True, -1):
                with self.subTest(budget=budget), self.assertRaises(ValueError):
                    observe_stage(small(), max_state_bytes=budget)
            bad = small()
            bad[3] = np.zeros(2)
            with self.assertRaises(ValueError):
                observe_stage(bad)
            bad = small()
            bad[1] = np.zeros(16384)
            with self.assertRaises(ValueError):
                observe_stage(bad)
            linear.assert_not_called()

    def test_reader_rejects_each_false_policy_value_residual_count_and_order(self):
        arrays = small()
        original = observe_stage(arrays, tolerance=1e-11)
        self.assertGreaterEqual(len(original['states']), 2)
        mutants = []
        wrong = copy.deepcopy(original)
        wrong['policies'][0, 0] = not wrong['policies'][0, 0]
        mutants.append(wrong)
        wrong = copy.deepcopy(original)
        wrong['values'][0, 0] = 123.
        mutants.append(wrong)
        wrong = copy.deepcopy(original)
        wrong['states'][0]['residual'] = 123.
        mutants.append(wrong)
        wrong = copy.deepcopy(original)
        wrong['linear_calls'] += 1
        mutants.append(wrong)
        wrong = copy.deepcopy(original)
        wrong['states'] = wrong['states'][:-1]
        mutants.append(wrong)
        wrong = copy.deepcopy(original)
        wrong['values'] = wrong['values'][::-1].copy()
        mutants.append(wrong)
        wrong = copy.deepcopy(original)
        wrong['recurrence']['confirmed'] = dict(kind='cycle', start=0, period=1, end=2)
        mutants.append(wrong)
        for index, mutant in enumerate(mutants):
            with self.subTest(index=index), self.assertRaises(ValueError):
                verify_observation(arrays, mutant, tolerance=1e-11)
        with self.assertRaises(ValueError):
            verify_observation(arrays, original, tolerance=1e-10)

    def test_typed_nonfinite_linear_refusal_has_actual_attempt_and_no_fake_update(self):
        with patch('lcp_observe._delegate_solve', return_value=np.full(3, np.inf)):
            answer = observe_stage(small(), tolerance=1e-11)
        self.assertEqual(answer['work']['reason'], 'nonfinite_policy_solves')
        self.assertEqual(answer['linear_calls'], 1)
        self.assertEqual(answer['work']['iterations'], 0)
        self.assertEqual(len(answer['states']), 1)
        self.assertTrue(verify_observation(small(), answer, tolerance=1e-11))
        with patch('lcp_observe._delegate_solve', side_effect=[np.ones(3), np.full(3, np.inf)]):
            correction = observe_stage(small(), tolerance=1e-11)
        self.assertEqual(correction['work']['reason'], 'nonfinite_correction_solves')
        self.assertEqual(correction['linear_calls'], 2)
        self.assertEqual(correction['work']['iterations'], 0)
        self.assertEqual(len(correction['states']), 1)
        arrays = small()
        arrays[-1][:] = 1e308
        original = observe_stage(arrays, tolerance=1e-11)
        self.assertEqual(original['work']['reason'], 'nonfinite_original_residual')
        self.assertEqual(original['nonfinite_observations'], 1)
        self.assertEqual(original['linear_calls'], 0)
        self.assertEqual(len(original['states']), 0)
        self.assertIsNone(original['terminal_values'])

    def test_frozen_source_mismatch_refuses_before_loading_or_solving(self):
        with patch.dict('lcp_observe.SOURCE_HASHES', {'lcp_correction.py': '0'*64}), \
                patch('lcp_observe._delegate_solve') as linear:
            with self.assertRaises(ValueError):
                observe_stage(small())
            linear.assert_not_called()

    def test_second_source_is_verified_before_either_module_executes(self):
        with patch.dict('lcp_observe.SOURCE_HASHES', {'pde_reference.py': '0'*64}), \
                patch('lcp_observe.types.ModuleType') as load:
            with self.assertRaises(ValueError):
                observe_stage(small())
            load.assert_not_called()

    def test_reader_rejects_boolean_integer_metadata_and_false_terminal_refusal(self):
        answer = observe_stage(small(), tolerance=1e-11)
        wrong = copy.deepcopy(answer)
        wrong['states'][0]['index'] = False
        with self.assertRaises(ValueError):
            verify_observation(small(), wrong, tolerance=1e-11)
        wrong = copy.deepcopy(answer)
        wrong['status'] = 'refused'
        wrong['work']['reason'] = 'linear_solve_failure'
        wrong['work']['last_checked_residual'] = wrong['states'][-1]['residual']
        with self.assertRaises(ValueError):
            verify_observation(small(), wrong, tolerance=1e-11)

    def test_reader_checks_cannot_be_disabled_by_python_optimization(self):
        script = '''from test_lcp_observe import small
from lcp_observe import observe_stage, verify_observation
answer = observe_stage(small(), max_updates=0)
answer['states'][0]['index'] = False
try:
    verify_observation(small(), answer)
except ValueError:
    raise SystemExit(0)
raise SystemExit(1)
'''
        result = subprocess.run([sys.executable, '-O', '-c', script],
                                cwd=Path(__file__).parent, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
