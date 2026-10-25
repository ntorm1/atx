"""Tiny retained-mode adapters and protocol controls; no declared PDE grid runs."""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

import greek_reference
import lcp_policy_retention as retention
from lcp_stage_adapter import StageCollector, private_reference
import lcp_zero_rate_control as control
import lcp_zero_rate_run as runner
from test_lcp_zero_rate_control import completed_work


MODE='one_correction_retained_admissible'
TINY=dict(S=100.,K=100.,T=.1,sigma=.2,r=0.,q=0.,side='put')


def arrays():
    return [np.zeros(1),np.ones(2),np.zeros(1),np.ones(2),np.zeros(2),np.zeros(2)]


def observed_record():
    answer=retention.solve_policy_control(*arrays(),tolerance=1e-10,mode='retain_admissible')
    return dict(stage=1,status='computed',work={k:v for k,v in answer.items() if k!='values'})


class RetainedAdapterTests(unittest.TestCase):
    def test_actual_tiny_both_schemes_collect_fresh_stage_selectors(self):
        existing=greek_reference.solve_obstacle
        for scheme in ('dirk_l','dirk_third'):
            collector=StageCollector(mode=MODE)
            solve=private_reference(MODE,collector)
            answer=solve(TINY,nx=33,nt=4,scheme=scheme)
            self.assertEqual(len(collector.records),6)
            self.assertEqual(answer['stages'],6)
            for row in collector.records:
                work=row['work'];policy=work['policy_work']
                self.assertEqual(work['mode'],MODE)
                self.assertEqual(work['base_mode'],'one_correction')
                self.assertEqual(work['linear_solve_budget'],66)
                self.assertEqual(policy['selection_attempts'],work['policy_solves'])
                self.assertEqual(policy['selection_completions'],work['policy_solves'])
                self.assertEqual(policy['initial_policy_rows'],31 if work['policy_solves'] else 0)
                self.assertEqual(policy['workspace_reserved_bytes'],32*31)
            self.assertIs(greek_reference.solve_obstacle,existing)
            with self.assertRaises(ValueError):private_reference(MODE,collector)
        with self.assertRaises(ValueError):private_reference(MODE,StageCollector())

    def test_typed_cap_keeps_owned_snapshot_and_allocation_keeps_unknown_work(self):
        collector=StageCollector(mode=MODE)
        original=retention.solve_policy_control
        def limited(*args,**kwargs):return original(*args,**kwargs,max_linear_solves=0)
        a=arrays()
        with patch.object(retention,'solve_policy_control',limited):
            with self.assertRaises(retention.LcpRetentionRefused):collector(*a,tolerance=1e-10)
        self.assertEqual(collector.records[0]['status'],'refused')
        self.assertEqual(collector.records[0]['work']['linear_solves'],0)
        a[-1][0]=7
        self.assertEqual(collector.failure_arrays['initial'][0],0)
        other=StageCollector(mode=MODE)
        with patch.object(retention.PolicySelector,'_owned',side_effect=MemoryError('controlled')):
            with self.assertRaises(retention.LcpRetentionRefused):other(*arrays(),tolerance=1e-10)
        row=other.records[0]
        self.assertEqual(row['status'],'unobserved_failure');self.assertIsNone(row['work'])
        self.assertIsNone(other.failure_arrays)
        self.assertEqual(row['retention_failure']['reason'],'allocation_unavailable')
        self.assertEqual(row['retention_failure']['policy_work']['selection_attempts'],1)
        self.assertEqual(row['retention_failure']['policy_work']['selection_completions'],0)

    def test_unknown_error_has_no_fabricated_selector_diagnostics(self):
        collector=StageCollector(mode=MODE)
        with patch.object(retention,'solve_policy_control',side_effect=ValueError('controlled')):
            with self.assertRaises(ValueError):collector(*arrays(),tolerance=1e-10)
        self.assertIsNone(collector.records[0]['retention_failure'])
        self.assertIsNone(collector.records[0]['work'])


class RetainedProtocolTests(unittest.TestCase):
    def test_declared_requests_only_change_mode_and_keep_twelve(self):
        old=control.fixed_requests()
        expected=copy.deepcopy(old)
        for row in expected:row['lcp_mode']=MODE
        self.assertEqual(control.fixed_requests(mode=MODE),expected)
        self.assertEqual(len(expected),12)
        self.assertEqual(sum(2*r['options']['nt']-2 for r in expected),13288)
        self.assertEqual(control.fixed_requests(),old)
        with self.assertRaises(ValueError):control.fixed_requests(mode=True)

    def test_selector_conservation_and_cap_mutants_refuse(self):
        row=dict(options=dict(nx=4,nt=4),status='refused',stage_work=[observed_record(),
            dict(stage=2,status='unobserved_failure',work=None,exception_type='ValueError',
                 detail='protocol-only',retention_failure=None)])
        result=control.validate_stage_work(row,mode=MODE)
        self.assertTrue(result['unobserved_failure'])
        for mutate in (lambda w:w.update(mode='one_correction'),
                       lambda w:w.update(base_mode='original'),
                       lambda w:w.update(linear_solve_budget=256),
                       lambda w:w['policy_work'].update(selection_completions=0),
                       lambda w:w['policy_work'].update(initial_policy_rows=0),
                       lambda w:w['policy_work'].update(retained_overrides=1),
                       lambda w:w['policy_work'].update(policy_changes=1),
                       lambda w:w['policy_work'].update(workspace_reserved_bytes=0)):
            wrong=copy.deepcopy(row);mutate(wrong['stage_work'][0]['work'])
            with self.assertRaises(ValueError):control.validate_stage_work(wrong,mode=MODE)

    def test_zero_solve_phase_laundering_and_partial_metadata_refuse(self):
        collector=StageCollector(mode=MODE)
        with patch.object(retention.PolicySelector,'_owned',side_effect=MemoryError('controlled')):
            with self.assertRaises(retention.LcpRetentionRefused):collector(*arrays(),tolerance=1e-10)
        row=dict(options=dict(nx=4,nt=4),status='refused',stage_work=collector.records)
        self.assertTrue(control.validate_stage_work(row,mode=MODE)['unobserved_failure'])
        for change in (lambda d:d.update(reason='policy_iteration_limit'),
                       lambda d:d['policy_work'].update(selection_completions=2),
                       lambda d:d.update(mode='one_correction')):
            wrong=copy.deepcopy(row);change(wrong['stage_work'][0]['retention_failure'])
            with self.assertRaises(ValueError):control.validate_stage_work(wrong,mode=MODE)

    def test_original_receipt_still_reconstructs_without_pde(self):
        path=os.environ.get('ATX_LCP_OLD_CONTROL')
        if path is None:self.skipTest('Explicit immutable original control archive required')
        with patch('lcp_stage_adapter.private_reference',side_effect=AssertionError('PDE in reader')):
            summary=runner.verify_run(Path(path))
        self.assertEqual((summary['requested'],summary['computed'],summary['refused']),(12,8,4))
        self.assertFalse(summary['solver_controls_passed'])


class RetainedRunnerTests(unittest.TestCase):
    def test_missing_initial_checked_vector_requires_actual_nonfinite_operands(self):
        collector=StageCollector(mode=MODE)
        a=arrays();a[1][:]=1e308;a[3][:]=0;a[5][:]=2
        with self.assertRaises(retention.LcpRetentionRefused):collector(*a,tolerance=1e-10)
        row=dict(lcp_mode=MODE,scheme='dirk_l',label='tiny',options=dict(nx=4,nt=4),
                 status='refused',stage_work=collector.records)
        control.validate_stage_work(row,mode=MODE)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);path=root/'dirk_l-tiny-failure.npz'
            def write():
                np.savez_compressed(path,**collector.failure_arrays)
                row['failure_snapshot']=dict(file=path.name,sha256=runner.digest(path))
            write();runner.verify_failure_snapshot(root,row)
            collector.failure_arrays['initial'][:]=0
            write()
            with self.assertRaises(ValueError):runner.verify_failure_snapshot(root,row)

    def test_stubbed_twelve_mode_receipt_source_and_cap_controls(self):
        source=os.environ.get('ATX_LCP_BASELINE')
        if source is None:self.skipTest('Explicit immutable historical baseline required')
        historical=Path(source)
        old=[json.loads(line) for line in (historical/'outcomes.jsonl').read_text().splitlines()]
        old=[r for r in old if r['id']==control.MODEL['id']]
        seen=[]
        def private(mode,collector):
            self.assertEqual(mode,MODE);self.assertEqual(collector.mode,MODE)
            row=old[len(seen)];seen.append(row)
            def solve(model,**options):
                self.assertEqual(model,control.MODEL)
                self.assertEqual(options,dict(row['options'],scheme=row['scheme']))
                if row['status']!='computed':
                    collector.records=[dict(stage=1,status='unobserved_failure',work=None,
                        exception_type='ArithmeticError',detail='protocol-only old refusal',retention_failure=None)]
                    raise ArithmeticError('protocol-only old refusal')
                collector.records=completed_work(row)
                for record in collector.records:
                    w=record['work'];n=row['options']['nx']-2;count=w['policy_solves']
                    w.update(mode=MODE,base_mode='one_correction',linear_solve_budget=256,
                        policy_work=dict(workspace_required_bytes=32*n,workspace_reserved_bytes=32*n,
                            selection_attempts=count,selection_completions=count,
                            initial_policy_rows=n if count else 0,admissible_retained_rows=0,
                            retained_overrides=0,failing_reselected_rows=max(0,count-1)*n,policy_changes=0))
                return copy.deepcopy(row['result'])
            return solve
        real_git=runner.git;fake='f'*40;frozen={}
        def git(*args):
            if args==('status','--porcelain'):return b''
            if args==('rev-parse','HEAD'):return fake.encode()
            if args[0]=='show' and args[1].startswith(fake+':'):
                name=args[1].split(':',1)[1]
                return frozen.get(name,(runner.ROOT/name).read_bytes())
            return real_git(*args)
        with tempfile.TemporaryDirectory() as tmp,patch.object(runner,'git',side_effect=git),\
                patch('lcp_stage_adapter.private_reference',side_effect=private):
            output=Path(tmp)/'new'
            self.assertEqual(runner.run(output,historical,MODE),0)
            self.assertEqual(len(seen),12)
            summary=runner.verify_run(output)
            self.assertEqual(summary['schema'],'atx-zero-rate-retained-lcp-summary-v1')
            self.assertEqual((summary['computed'],summary['refused']),(6,6))
            self.assertFalse(summary['solver_controls_passed'])
            path=output/'manifest.json';original=path.read_bytes();manifest=json.loads(original)
            for mutation in (dict(lcp_mode='one_correction'),dict(effective_linear_solve_cap=32770),
                             dict(retention_source='0'*40),dict(completed=False)):
                runner.json_write(path,dict(manifest,**mutation))
                with self.assertRaises(ValueError):runner.verify_run(output)
            path.write_bytes(original)
            key=runner.PREFIX+'lcp_policy_retention.py'
            changed=(output/'sources/lcp_policy_retention.py').read_bytes()+b'\n# different retained primitive\n'
            frozen[key]=changed;(output/'sources/lcp_policy_retention.py').write_bytes(changed)
            wrong=copy.deepcopy(manifest);wrong['source_hashes'][key]=runner.digest(output/'sources/lcp_policy_retention.py')
            runner.json_write(path,wrong)
            with self.assertRaises(ValueError):runner.verify_run(output)

    def test_new_mode_reader_failure_leaves_incomplete_marker(self):
        source=os.environ.get('ATX_LCP_BASELINE')
        if source is None:self.skipTest('Explicit immutable historical baseline required')
        real_git=runner.git
        def clean(*args):return b'' if args==('status','--porcelain') else real_git(*args)
        with tempfile.TemporaryDirectory() as tmp,patch.object(runner,'git',side_effect=clean),\
                patch.object(runner,'execute_requests',return_value=[]),\
                patch.object(runner,'summarize_control',return_value=dict(computed=0,refused=12)),\
                patch.object(runner,'verify_run',side_effect=ValueError('controlled reader failure')):
            output=Path(tmp)/'new'
            with self.assertRaises(ValueError):runner.run(output,Path(source),MODE)
            self.assertFalse(json.loads((output/'manifest.json').read_text())['completed'])

    def test_source_change_during_reader_cannot_publish_complete(self):
        source=os.environ.get('ATX_LCP_BASELINE')
        if source is None:self.skipTest('Explicit immutable historical baseline required')
        real_git=runner.git;changed=[False]
        def git(*args):
            if args==('status','--porcelain'):return b' M controlled-source' if changed[0] else b''
            return real_git(*args)
        def checked(*args,**kwargs):changed[0]=True;return {}
        with tempfile.TemporaryDirectory() as tmp,patch.object(runner,'git',side_effect=git),\
                patch.object(runner,'execute_requests',return_value=[None]*12),\
                patch.object(runner,'summarize_control',return_value=dict(computed=0,refused=12)),\
                patch.object(runner,'verify_run',side_effect=checked):
            output=Path(tmp)/'new'
            self.assertEqual(runner.run(output,Path(source),MODE),2)
            self.assertFalse(json.loads((output/'manifest.json').read_text())['completed'])


if __name__=='__main__':unittest.main()
