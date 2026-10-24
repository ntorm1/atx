"""Tiny archive/launcher fixtures only; never run one of the four saved systems."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

import lcp_observe_archive as common
import lcp_observe_run as old_runner
import lcp_retained_archive as archive
import lcp_retained_run as runner
from test_lcp_observe_run import fixture
from test_lcp_retained_observe import small,divergence
from lcp_retained_observe import observe_retained


class ArchiveTests(unittest.TestCase):
    def test_round_trip_preserves_actual_policy_and_rehashed_substitution_refuses(self):
        inputs,result=divergence()
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);entry=archive.export_stage(root,'tiny',inputs,inputs[-1],result)
            restored=archive.read_stage(root,entry,inputs,inputs[-1])
            np.testing.assert_array_equal(restored['used_policies'],result['used_policies'])
            self.assertFalse(restored['used_policies'][1,0]);self.assertTrue(restored['raw_policies'][1,0])
            path=root/entry['trajectory']['file']
            np.savez(path,values=result['values'],raw_policies=result['raw_policies'],used_policies=result['raw_policies'][:2])
            entry['trajectory']=common.file_record(path)
            with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])

    def test_exact_selected_equation_uses_actual_not_raw_mask(self):
        inputs=[np.zeros(0),np.array([2.]),np.zeros(0),np.zeros(1),np.zeros(1),np.zeros(1)]
        # Pure independent row-audit fixture, not a valid solver trajectory.
        result=dict(values=np.array([[0.],[1.]]),raw_policies=np.ones((2,1),dtype=bool),
                    used_policies=np.zeros((1,1),dtype=bool),recurrence=dict(confirmed=None))
        audit=archive.exact_audit(inputs,result)
        self.assertEqual(audit['states'][0]['exact']['selected_max'],'2/1')

    def test_forged_counts_and_exact_summary_refuse_after_rehash(self):
        inputs,result=divergence()
        for mutate in (lambda d:d['observation']['work']['policy_work'].update(retained_overrides=0),
                       lambda d:d['exact'].update(rows_checked=0),
                       lambda d:d.update(terminal_present=False)):
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);entry=archive.export_stage(root,'tiny',inputs,inputs[-1],result)
                path=root/entry['json']['file'];data=common.read_json(path);mutate(data)
                path.write_text(json.dumps(data));entry['json']=common.file_record(path)
                with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])

    def test_actual_mask_shape_budget_and_old_schema_refuse(self):
        inputs=small();result=observe_retained(inputs)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            with self.assertRaises(ValueError):archive.export_stage(root,'limited',inputs,inputs[-1],result,cap=1)
            entry=archive.export_stage(root,'tiny',inputs,inputs[-1],result)
            wrong=copy.deepcopy(entry);wrong['selections']=129
            with self.assertRaises(ValueError):archive.read_stage(root,wrong,inputs,inputs[-1])
            path=root/entry['json']['file'];data=common.read_json(path);data['schema']='atx-lcp-stage-observation-v1'
            path.write_text(json.dumps(data));entry['json']=common.file_record(path)
            with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])

    def test_zero_solve_phase_laundering_refuses_through_archive(self):
        inputs=small();result=observe_retained(inputs,max_updates=0)
        for reason in ('policy_iteration_limit','linear_solve_failure',
                       'nonfinite_policy_solves','nonfinite_original_residual'):
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);entry=archive.export_stage(root,'tiny',inputs,inputs[-1],result)
                path=root/entry['json']['file'];data=common.read_json(path)
                data['observation']['work']['reason']=reason
                data['observation']['observation_cap_reached']=False
                path.write_text(json.dumps(data));entry['json']=common.file_record(path)
                with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])


class RunnerTests(unittest.TestCase):
    def source(self,commit,require_clean=False):
        if commit!='b'*40:raise ValueError('Wrong reviewed source')
        return {'source.py':b'# retained synthetic source\n'}

    def test_four_tiny_roundtrip_exactly_two_audit_passes_and_no_reader_solve(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(runner,'_git_sources',self.source):
            root=Path(tmp);contract=fixture(root/'in')
            with patch.object(common,'exact_audit',wraps=common.exact_audit) as audit:
                report=runner._run(root/'in',root/'out','b'*40,contract)
            self.assertEqual(audit.call_count,8)
            self.assertEqual(report['systems'],4);self.assertEqual(report['pde_runs'],0)
            self.assertEqual(report['automatic_exact_row_evaluations'],2*report['exact_rows'])
            with patch.object(runner,'observe_retained',side_effect=AssertionError('Reader invoked observer')):
                self.assertEqual(runner._verify(root/'out','b'*40,contract),report)
            with self.assertRaises(FileExistsError):runner._run(root/'in',root/'out','b'*40,contract)

    def test_fatal_observation_and_reader_failure_leave_incomplete(self):
        for seam in ('observe_retained','_verify'):
            with tempfile.TemporaryDirectory() as tmp,patch.object(runner,'_git_sources',self.source):
                root=Path(tmp);contract=fixture(root/'in')
                with patch.object(runner,seam,side_effect=ArithmeticError('controlled fatal phase')):
                    with self.assertRaises(ArithmeticError):runner._run(root/'in',root/'out','b'*40,contract)
                self.assertFalse(common.read_json(root/'out'/'manifest.json')['completed'])
                with self.assertRaises(ValueError):runner._verify(root/'out','b'*40,contract)

    def test_rehashed_source_summary_control_and_denominator_mutants(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(runner,'_git_sources',self.source):
            root=Path(tmp);contract=fixture(root/'in');out=root/'out'
            runner._run(root/'in',out,'b'*40,contract)
            path=out/'manifest.json';original=path.read_bytes()
            for mutate in (lambda m:m.update(source_commit='c'*40),
                           lambda m:m['stages'].append(copy.deepcopy(m['stages'][0])),
                           lambda m:m['stages'].pop(),
                           lambda m:m['summary'].update(policy_retained_overrides=10000),
                           lambda m:m['caps'].update(max_updates_per_stage=129)):
                data=json.loads(original);mutate(data);path.write_text(json.dumps(data))
                with self.assertRaises(ValueError):runner._verify(out,'b'*40,contract)
            path.write_bytes(original);(out/'sources'/'source.py').write_text('# changed\n')
            data=json.loads(original);data['inventory']=old_runner._inventory(out)[0]
            path.write_text(json.dumps(data))
            with self.assertRaises(ValueError):runner._verify(out,'b'*40,contract)

    def test_rehashed_input_corruption_and_extra_file_refuse(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(runner,'_git_sources',self.source):
            root=Path(tmp);contract=fixture(root/'in');out=root/'out'
            runner._run(root/'in',out,'b'*40,contract)
            path=out/'manifest.json';original=path.read_bytes()
            extra=out/'stages'/'extra.json';extra.write_text('{}')
            data=json.loads(original);data['inventory']=old_runner._inventory(out)[0]
            path.write_text(json.dumps(data))
            with self.assertRaises(ValueError):runner._verify(out,'b'*40,contract)
            extra.unlink();path.write_bytes(original)
            source=out/'inputs'/contract['cases'][0]['file']
            with np.load(source) as z:arrays={name:z[name] for name in z.files}
            arrays['rhs'][0]+=1
            np.savez(source,**arrays)
            data=json.loads(original);data['inventory']=old_runner._inventory(out)[0]
            path.write_text(json.dumps(data))
            with self.assertRaises(ValueError):runner._verify(out,'b'*40,contract)

    def test_git_source_join_cleanliness_and_frozen_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);here=root/'bench';here.mkdir()
            path=here/'source.py';path.write_bytes(b'# frozen\r\n')
            clean=[True]
            def git(*args):
                if args==('rev-parse','--show-toplevel'):return str(root).encode()
                if args==('rev-parse','HEAD'):return b'b'*40
                if args==('status','--porcelain'):return b'' if clean[0] else b' M source.py'
                if args==('show','b'*40+':bench/source.py'):return b'# frozen\n'
                raise AssertionError(args)
            with patch.object(runner,'HERE',here),patch.object(runner,'SOURCE_NAMES',('source.py',)),\
                 patch.object(runner,'PINNED',{'source.py':common.sha(b'# frozen\n')}),\
                 patch.object(old_runner,'_git',git):
                self.assertEqual(runner._git_sources('b'*40,True),{'source.py':b'# frozen\r\n'})
                clean[0]=False
                with self.assertRaises(ValueError):runner._git_sources('b'*40,True)
                clean[0]=True;path.write_bytes(b'# changed\n')
                with self.assertRaises(ValueError):runner._git_sources('b'*40)
                path.write_bytes(b'# frozen\n')
                with patch.object(runner,'PINNED',{'source.py':'0'*64}):
                    with self.assertRaises(ValueError):runner._git_sources('b'*40)


if __name__=='__main__':unittest.main()
