"""Independent small systems only; no saved-stage loader or PDE fixture."""
import itertools
from pathlib import Path
import unittest
from unittest.mock import patch

import numpy as np

import lcp_observe
import lcp_policy_retention as retention


def system():
    return [np.array([-1.,-1.]),np.array([3.,3.,3.]),np.array([-1.,-1.]),
            np.array([-1.,1.,3.]),np.zeros(3),np.zeros(3)]


def exact_policy_solution(inputs):
    lower,diagonal,upper,rhs,payoff,_=inputs
    n=len(diagonal);matrix=np.diag(diagonal)+np.diag(lower,-1)+np.diag(upper,1)
    solutions=[]
    for policy in itertools.product((False,True),repeat=n):
        a=matrix.copy();b=rhs.copy()
        for row,exercise in enumerate(policy):
            if exercise:a[row]=np.eye(n)[row];b[row]=payoff[row]
        values=np.linalg.solve(a,b);c=matrix@values-rhs;g=values-payoff
        if np.max(np.abs(np.minimum(c,g)))<=1e-12:solutions.append(values)
    if not solutions:raise AssertionError('Independent enumeration found no feasible policy')
    for value in solutions:np.testing.assert_allclose(value,solutions[0],rtol=0,atol=1e-12)
    return solutions[0]


class SelectionTests(unittest.TestCase):
    def test_original_strict_rule_undoes_admissible_ulp_sized_continuation(self):
        # Independent fixture, rather than a row copied from a saved system.
        floor=64.;gap=33*np.spacing(floor);c=4*gap;tol=1e-10
        self.assertLess(gap,c);self.assertLess(c,tol)
        selector=retention.PolicySelector(1,32)
        initial=selector.choose(np.array([-2e-10]),np.array([0.]),tol)
        self.assertFalse(initial[0])
        next_policy=selector.choose(np.array([c]),np.array([gap]),tol)
        self.assertFalse(next_policy[0])  # Original gap<c would clamp this row.
        self.assertEqual(selector.work['retained_overrides'],1)

    def test_initial_identity_threshold_and_mandatory_reselection(self):
        s=retention.PolicySelector(4,128)
        c=np.array([-2e-10,2e-10,-2e-10,2e-10]);g=np.zeros(4)
        np.testing.assert_array_equal(s.choose(c,g,1e-10),g<c)
        c=np.array([1e-10,-1.01e-10,0.,0.]);g=np.array([0.,0.,-1.01e-10,1.01e-10])
        # row0 stays continuation despite strict exercise; row1 must leave exercise;
        # row2 must select exercise for an actual negative obstacle violation.
        np.testing.assert_array_equal(s.choose(c,g,1e-10),[False,False,True,True])
        self.assertEqual(s.work['failing_reselected_rows'],2)

    def test_neighbor_change_releases_previously_admissible_policy(self):
        s=retention.PolicySelector(2,64)
        np.testing.assert_array_equal(s.choose(np.array([1.,-1.]),np.zeros(2),1e-10),[True,False])
        np.testing.assert_array_equal(s.choose(np.array([0.,0.]),np.zeros(2),1e-10),[True,False])
        # A negative off-diagonal neighbor movement makes row0 infeasible.
        np.testing.assert_array_equal(s.choose(np.array([-2e-10,0.]),np.zeros(2),1e-10),[False,False])

    def test_private_buffer_and_nonfinite_refusal(self):
        s=retention.PolicySelector(1,32);c=np.array([-1.]);g=np.array([0.])
        out=s.choose(c,g,1e-10);out[0]=True
        self.assertFalse(s.choose(np.array([1e-12]),g,1e-10)[0])
        np.testing.assert_array_equal(c,[-1.]);np.testing.assert_array_equal(g,[0.])
        with self.assertRaises(ValueError):s.choose(np.array([np.nan]),g,1e-10)


class PrimitiveTests(unittest.TestCase):
    def test_default_is_bit_identical_to_pinned_correction(self):
        inputs=system();ordinary=lcp_observe._load_frozen().solve_obstacle_control(*inputs,1e-10,mode='one_correction')
        actual=retention.solve_policy_control(*inputs,1e-10)
        np.testing.assert_array_equal(actual.pop('values').view(np.uint64),ordinary.pop('values').view(np.uint64))
        self.assertEqual(actual,ordinary)

    def test_actual_coupled_solve_matches_independent_policy_enumeration(self):
        for rhs in (np.array([-1.,1.,3.]),np.array([-.1,1.,3.]),
                    np.array([3.,-2.,3.]),np.array([-2.,-1.,-3.])):
            inputs=system();inputs[3]=rhs;before=[a.copy() for a in inputs]
            actual=retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
            self.assertEqual(actual['mode'],retention.MODE)
            np.testing.assert_allclose(actual['values'],exact_policy_solution(inputs),rtol=0,atol=1e-12)
            self.assertLessEqual(actual['complementarity_residual'],1e-10)
            self.assertEqual(actual['policy_work']['selection_completions'],actual['policy_solves'])
            for a,b in zip(inputs,before):np.testing.assert_array_equal(a.view(np.uint64),b.view(np.uint64))

    def test_actual_neighbor_motion_forces_reselection(self):
        inputs=system();inputs[3]=np.array([-.1,1.,3.])
        # Initial row0 is obstacle-admissible. Solving its neighbors makes its
        # continuation negative; its retained exercise row must then be released.
        answer=retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        self.assertEqual(answer['iterations'],2)
        self.assertGreater(answer['values'][0],0)
        self.assertGreaterEqual(answer['policy_work']['failing_reselected_rows'],1)
        self.assertGreaterEqual(answer['policy_work']['admissible_retained_rows'],2)
        np.testing.assert_allclose(answer['values'],exact_policy_solution(inputs),rtol=0,atol=1e-12)

    def test_global_gate_refuses_even_if_policy_does_not_change(self):
        inputs=system()
        with self.assertRaises(retention.LcpRetentionRefused) as caught:
            retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible',max_linear_solves=0)
        d=caught.exception.diagnostics
        self.assertEqual(d['reason'],'linear_budget');self.assertEqual(d['linear_solves'],0)
        self.assertGreater(d['last_checked_residual'],1e-10)

    def test_workspace_unknown_mode_and_call_budget_before_work(self):
        inputs=system()
        with self.assertRaises(retention.LcpRetentionRefused) as caught:
            retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible',max_policy_bytes=0)
        self.assertEqual(caught.exception.diagnostics['reason'],'policy_workspace')
        for mode,budget in (('unknown',256),('retain_admissible',257),('retain_admissible',-1)):
            with self.subTest(mode=mode,budget=budget),self.assertRaises(ValueError):
                retention.solve_policy_control(*inputs,1e-10,mode=mode,max_linear_solves=budget)

    def test_partial_pair_budget_and_per_call_reset(self):
        inputs=system()
        with self.assertRaises(retention.LcpRetentionRefused) as caught:
            retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible',max_linear_solves=1)
        self.assertEqual(caught.exception.diagnostics['linear_solves'],0)
        a=retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        b=retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        self.assertEqual(a['policy_work'],b['policy_work'])
        np.testing.assert_array_equal(a['values'].view(np.uint64),b['values'].view(np.uint64))

    def test_unchanged_policy_is_not_convergence_and_original_bounds_remain(self):
        # Script only the selection to be deliberately wrong; actual coupled
        # solves/inspect/admission must still refuse. This is not retention evidence.
        def always_exercise(selector,c,g,tol):
            return np.ones(selector.rows,dtype=bool)
        for n,bound,reason in ((3,5,'policy_iteration_limit'),(127,128,'linear_budget')):
            inputs=[np.zeros(n-1),np.ones(n),np.zeros(n-1),np.ones(n),np.zeros(n),np.zeros(n)]
            with patch.object(retention.PolicySelector,'choose',always_exercise):
                with self.assertRaises(retention.LcpRetentionRefused) as caught:
                    retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
            d=caught.exception.diagnostics
            self.assertEqual(d['reason'],reason);self.assertEqual(d['iterations'],bound)
            self.assertEqual(d['linear_solves'],2*bound)
            self.assertEqual(d['residual_checks'],bound+1)
            self.assertEqual(d['last_checked_residual'],1.)

    def test_source_assignment_and_allocation_guards(self):
        inputs=system()
        with patch.object(retention,'SOURCE_HASHES',dict(retention.SOURCE_HASHES,**{'lcp_correction.py':'0'*64})):
            with self.assertRaises(ValueError):retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        with patch.object(retention,'_ASSIGNMENT','missing assignment'):
            with self.assertRaises(ValueError):retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        with patch.object(retention.PolicySelector,'_owned',side_effect=MemoryError('controlled allocation failure')):
            with self.assertRaises(retention.LcpRetentionRefused) as caught:
                retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        d=caught.exception.diagnostics
        self.assertEqual(d['reason'],'allocation_unavailable')
        self.assertEqual(d['policy_work']['selection_attempts'],1)
        self.assertEqual(d['policy_work']['selection_completions'],0)
        self.assertNotIn('values',d);self.assertNotIn('last_checked_residual',d)
        with patch.object(retention,'PolicySelector',side_effect=MemoryError('constructor allocation')):
            with self.assertRaises(retention.LcpRetentionRefused) as caught:
                retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        self.assertIsNone(caught.exception.diagnostics['policy_work'])
        self.assertEqual(caught.exception.diagnostics['reason'],'allocation_unavailable')

    def test_original_nonfinite_and_partial_pair_refusals_keep_work(self):
        inputs=system();original=retention._primitive
        def nonfinite_primitive(selector=None):
            p=original(selector)
            p.solve_banded=lambda *args,**kwargs:np.full(3,np.nan)
            return p
        with patch.object(retention,'_primitive',side_effect=nonfinite_primitive):
            with self.assertRaises(retention.LcpRetentionRefused) as caught:
                retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible')
        d=caught.exception.diagnostics
        self.assertEqual(d['reason'],'nonfinite_policy_solves')
        self.assertEqual(d['linear_solves'],1);self.assertEqual(d['correction_solves'],0)
        self.assertEqual(d['policy_work']['selection_completions'],1)
        inputs[3]=np.array([-.1,1.,3.])
        with self.assertRaises(retention.LcpRetentionRefused) as caught:
            retention.solve_policy_control(*inputs,1e-10,mode='retain_admissible',max_linear_solves=3)
        d=caught.exception.diagnostics
        self.assertEqual(d['reason'],'linear_budget');self.assertEqual(d['linear_solves'],2)
        self.assertEqual(d['iterations'],1);self.assertEqual(d['policy_work']['selection_completions'],1)


if __name__=='__main__':unittest.main()
