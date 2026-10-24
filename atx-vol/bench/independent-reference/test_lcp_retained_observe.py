"""Tiny trajectory controls; never load or solve a saved failed stage."""
import copy
import hashlib
import unittest
from unittest.mock import patch

import numpy as np

import lcp_policy_retention as primitive
import lcp_retained_observe as observer


def small():
    return [np.array([-1.,-1.]),np.array([3.,3.,3.]),np.array([-1.,-1.]),
            np.array([-.1,1.,3.]),np.zeros(3),np.zeros(3)]


def divergence():
    inputs=[np.zeros(1),np.array([2.,1.]),np.zeros(1),np.zeros(2),np.zeros(2),np.array([-1e-8,1.])]
    # Scripted inexact intermediate solve forces another selection after row0
    # becomes admissible. This exercises recording, not a real-price claim.
    solves=[np.array([1e-12,1.]),np.zeros(2),np.zeros(2),np.zeros(2)]
    with patch.object(observer,'_delegate_solve',side_effect=solves):
        result=observer.observe_retained(inputs)
    return inputs,result


class ObservationTests(unittest.TestCase):
    def test_actual_primitive_bits_counts_and_global_isolation(self):
        inputs=small();original_choose=primitive.PolicySelector.choose
        answer=primitive.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        result=observer.observe_retained(inputs)
        np.testing.assert_array_equal(result['terminal_values'].view(np.uint64),answer['values'].view(np.uint64))
        self.assertEqual(result['work'],{k:v for k,v in answer.items() if k!='values'})
        self.assertEqual(result['used_policies'].shape,(2,3))
        self.assertIs(primitive.PolicySelector.choose,original_choose)
        self.assertTrue(observer.verify_retained(inputs,result))

    def test_actual_retention_diverges_from_raw_and_reader_has_no_solver(self):
        inputs,result=divergence()
        self.assertTrue(result['raw_policies'][1,0]);self.assertFalse(result['used_policies'][1,0])
        self.assertEqual(result['work']['policy_work']['retained_overrides'],1)
        with patch.object(observer,'_delegate_solve',side_effect=AssertionError('Reader solved')):
            self.assertTrue(observer.verify_retained(inputs,result))

    def test_rehashed_used_mask_raw_substitution_and_counts_refuse(self):
        inputs,result=divergence()
        wrong=copy.deepcopy(result);wrong['used_policies']=wrong['raw_policies'][:2].copy()
        for index,mask in enumerate(wrong['used_policies']):
            wrong['selections'][index]['policy_sha256']=hashlib.sha256(mask.tobytes()).hexdigest()
        with self.assertRaises(ValueError):observer.verify_retained(inputs,wrong)
        wrong=copy.deepcopy(result);wrong['work']['policy_work']['retained_overrides']=0
        with self.assertRaises(ValueError):observer.verify_retained(inputs,wrong)

    def test_prior_policy_changes_selected_equation_reconstruction(self):
        inputs,result=divergence();wrong=copy.deepcopy(result)
        wrong['states'][2]['selected_system_residual']=1.
        with self.assertRaises(ValueError):observer.verify_retained(inputs,wrong)

    def test_zero_solve_reason_phases_and_erased_initial_state_refuse(self):
        inputs=small();result=observer.observe_retained(inputs,max_updates=0)
        for reason in ('policy_iteration_limit','linear_solve_failure','nonfinite_policy_solves','nonfinite_original_residual'):
            wrong=copy.deepcopy(result);wrong['work']['reason']=reason;wrong['observation_cap_reached']=False
            with self.subTest(reason=reason),self.assertRaises(ValueError):observer.verify_retained(inputs,wrong)
        wrong=copy.deepcopy(result)
        wrong.update(values=np.empty((0,3)),raw_policies=np.empty((0,3),dtype=bool),states=[],terminal_values=None,
                     nonfinite_observations=1,observation_cap_reached=False)
        wrong['work'].update(reason='nonfinite_original_residual',last_checked_residual=None)
        wrong['recurrence']=observer.retained_recurrence(wrong['values'],wrong['raw_policies'],wrong['used_policies'],[])
        with self.assertRaises(ValueError):observer.verify_retained(inputs,wrong)

    def test_failed_selection_has_no_following_checked_update(self):
        inputs=small()
        for calls in ([np.linalg.LinAlgError('policy')],[np.zeros(3),np.linalg.LinAlgError('correction')]):
            with patch.object(observer,'_delegate_solve',side_effect=calls):result=observer.observe_retained(inputs)
            self.assertEqual(len(result['states']),1);self.assertEqual(len(result['used_policies']),1)
            self.assertEqual(result['work']['reason'],'linear_solve_failure')
            self.assertEqual(result['linear_calls'],len(calls))
            self.assertTrue(observer.verify_retained(inputs,result))

    def test_genuine_nonfinite_initial_inspect_preserves_empty_prefix(self):
        inputs=[np.zeros(0),np.array([1e308]),np.zeros(0),np.zeros(1),np.zeros(1),np.array([2.])]
        result=observer.observe_retained(inputs)
        self.assertEqual(len(result['states']),0);self.assertEqual(result['nonfinite_observations'],1)
        self.assertEqual(result['work']['reason'],'nonfinite_original_residual')
        self.assertTrue(observer.verify_retained(inputs,result))

    def test_state_budget_and_inputs_are_immutable(self):
        inputs=small();before=[a.copy() for a in inputs]
        with self.assertRaises(ValueError):observer.observe_retained(inputs,max_state_bytes=0)
        result=observer.observe_retained(inputs,max_updates=0)
        self.assertEqual(result['allocated_state_bytes'],27)
        for a,b in zip(inputs,before):np.testing.assert_array_equal(a.view(np.uint64),b.view(np.uint64))
        for key in ('values','raw_policies','used_policies'):self.assertFalse(result[key].flags.writeable)

    def test_nonfinite_attempt_phases_are_preserved(self):
        diagonal=[np.zeros(0),np.ones(1),np.zeros(0),np.ones(1),np.zeros(1),np.zeros(1)]
        cases=[('nonfinite_policy_solves',diagonal,[np.array([np.nan])]),
               ('nonfinite_correction_solves',diagonal,[np.ones(1),np.array([np.nan])]),
               ('nonfinite_corrected_values',diagonal,[np.array([1e308]),np.array([1e308])])]
        overflow=[np.zeros(0),np.array([1e308]),np.zeros(0),np.zeros(1),np.array([2.]),np.ones(1)]
        cases.append(('nonfinite_original_residual',overflow,[np.array([2.]),np.zeros(1)]))
        for reason,inputs,solves in cases:
            with self.subTest(reason=reason),patch.object(observer,'_delegate_solve',side_effect=solves):
                result=observer.observe_retained(inputs)
                self.assertEqual(result['work']['reason'],reason)
                self.assertEqual(result['linear_calls'],len(solves))
                self.assertTrue(observer.verify_retained(inputs,result))

    def test_source_and_allocation_fail_without_a_false_observation(self):
        with patch.dict(observer.SOURCE_HASHES,{'lcp_policy_retention.py':'0'*64}):
            with self.assertRaises(ValueError):observer.observe_retained(small())
        module=observer._load_retention()
        with patch.object(module.PolicySelector,'_owned',side_effect=MemoryError('controlled')),\
                patch.object(observer,'_load_retention',return_value=module):
            with self.assertRaises(ArithmeticError) as caught:observer.observe_retained(small())
        self.assertEqual(caught.exception.diagnostics['reason'],'allocation_unavailable')
        self.assertEqual(caught.exception.diagnostics['policy_work']['selection_completions'],0)


class RecurrenceTests(unittest.TestCase):
    def test_identical_prices_with_distinct_memory_are_not_same_state(self):
        values=np.zeros((3,1));raw=np.zeros((3,1),dtype=bool);used=np.array([[False],[True]])
        r=observer.retained_recurrence(values,raw,used,[0.,0.,0.])
        self.assertIsNone(r['first_state_repeat']);self.assertIsNone(r['confirmed'])
        self.assertEqual(r['first_raw_policy_repeat'],dict(start=0,end=1))

    def test_two_laps_include_prior_policy_and_signed_zero(self):
        values=np.zeros((6,1));raw=np.zeros((6,1),dtype=bool)
        used=np.array([[False],[True],[False],[True],[False]])
        r=observer.retained_recurrence(values,raw,used,[0.]*6)
        self.assertEqual(r['confirmed'],dict(kind='cycle',start=1,period=2,end=5))
        values[3,0]=-0.
        r=observer.retained_recurrence(values,raw,used,[0.]*6)
        self.assertIsNone(r['confirmed'])

    def test_digest_collisions_do_not_replace_full_equality(self):
        with patch.object(observer,'_digest',return_value='collision'):
            result=observer.retained_recurrence(np.array([[0.],[1.],[2.]]),np.zeros((3,1),dtype=bool),
                                                np.zeros((2,1),dtype=bool),[0.,0.,0.])
        self.assertIsNone(result['first_state_repeat']);self.assertIsNone(result['confirmed'])


if __name__=='__main__':unittest.main()
