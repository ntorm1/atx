"""Tiny synthetic controls only; never load a saved production stage."""
import copy
from decimal import localcontext, ROUND_DOWN
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile

import numpy as np

import lcp_observe as observer
import lcp_observe_archive as archive
import lcp_observe_run as runner


def small():
    return [np.array([-1., -1.]), np.array([3., 3., 3.]),
            np.array([-1., -1.]), np.array([-1., 1., 3.]),
            np.zeros(3), np.zeros(3)]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def fixture(directory):
    directory.mkdir()
    cases, outcomes = [], []
    names = archive.INPUT_NAMES
    for index in range(4):
        case = dict(id='tiny-'+str(index), scheme='scheme-'+str(index), label='tiny',
                    stage=index+1, file=f'tiny-{index}-failure.npz', rows=3)
        values = small()
        np.savez_compressed(directory/case['file'], **dict(zip(names, values+[values[-1]])))
        case['sha256'] = digest((directory/case['file']).read_bytes())
        cases.append(case)
        work = dict(mode='one_correction', iterations=5, policy_solves=5, correction_solves=5,
                    linear_solves=10, residual_checks=6, policy_iteration_bound=5,
                    linear_solve_bound=10, linear_solve_budget=10, tolerance=1e-10,
                    reason='policy_iteration_limit', last_checked_residual=3.)
        outcomes.append(dict(id='synthetic', scheme=case['scheme'], label='tiny',
            options=dict(nx=5), lcp_mode='one_correction', status='refused',
            exception_type='LcpCorrectionRefused', failure_snapshot=dict(file=case['file'], sha256=case['sha256']),
            stage_work=[dict(stage=case['stage'], status='refused', work=work)]))
    manifest = dict(schema='atx-zero-rate-lcp-control-v1', completed=True,
                    selected_ids=['synthetic'], requested_results=4, computed=0, refused=4,
                    requests=[{k:r[k] for k in ('id','scheme','label','options','lcp_mode')} for r in outcomes],
                    lcp_mode='one_correction')
    (directory/'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
    (directory/'outcomes.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in outcomes), encoding='utf-8')
    return dict(cases=cases, selected='synthetic', requests=4, computed=0, refused=4,
                history={n:digest((directory/n).read_bytes()) for n in ('manifest.json','outcomes.jsonl')})


class LoaderTests(unittest.TestCase):
    def test_fixed_inputs_join_and_extra_identity_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'in'; contract=fixture(root)
            loaded=runner._load_inputs(root, contract)
            self.assertEqual([r['case']['id'] for r in loaded], [r['id'] for r in contract['cases']])
            np.testing.assert_array_equal(loaded[0]['arrays'][-1], small()[-1])
            (root/'extra-failure.npz').write_bytes(b'bad')
            with self.assertRaises(ValueError): runner._load_inputs(root, contract)

    def test_historical_bytes_and_stage_join_cannot_be_laundered(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'in'; contract=fixture(root)
            path=root/'outcomes.jsonl'; original=path.read_bytes()
            rows=[json.loads(line) for line in original.splitlines()]
            rows[0]['stage_work'][-1]['stage']=99
            path.write_text(''.join(json.dumps(r)+'\n' for r in rows))
            with self.assertRaises(ValueError): runner._load_inputs(root, contract)
            contract['history'][path.name]=digest(path.read_bytes())
            with self.assertRaises(ValueError): runner._load_inputs(root, contract)

    def test_npz_preflight_rejects_duplicate_shape_dtype_order_and_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'x.npz'; specs={'v':(np.dtype('float64'),(2,))}
            for value in (np.zeros(3), np.zeros(2,dtype=np.float32), np.array([object(),object()])):
                np.savez(path,v=value)
                with self.subTest(dtype=str(value.dtype),size=value.size), self.assertRaises(ValueError):
                    archive.load_npz(path,specs,4096)
            np.savez(path,v=np.zeros(2))
            with self.assertRaises(ValueError): archive.load_npz(path,specs,1)
            raw=io.BytesIO();np.save(raw,np.zeros(2))
            with warnings.catch_warnings(),zipfile.ZipFile(path,'w') as z:
                warnings.simplefilter('ignore',UserWarning)
                z.writestr('v.npy',raw.getvalue());z.writestr('v.npy',raw.getvalue())
            with self.assertRaises(ValueError): archive.load_npz(path,specs,4096)
            # Header declares huge storage while the ZIP member itself is tiny.
            raw=io.BytesIO();np.lib.format.write_array_header_1_0(raw,dict(descr='<f8',fortran_order=False,shape=(10**12,)))
            with zipfile.ZipFile(path,'w') as z:z.writestr('v.npy',raw.getvalue())
            with self.assertRaises(ValueError): archive.load_npz(path,specs,4096)
            np.savez(path,v=np.asfortranarray(np.zeros((2,2))))
            with self.assertRaises(ValueError):archive.load_npz(path,{'v':(np.dtype('float64'),(2,2))},4096)

    def test_strict_json_and_bounded_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'x.json'
            for text in ('{"a":1,"a":2}','{"a":NaN}','{"a":1e400}'):
                path.write_text(text)
                with self.assertRaises(ValueError):archive.read_json(path,4096)
            out=Path(tmp)/'limited.npz'
            with self.assertRaises(ValueError):archive.write_npz(out,{'v':np.zeros(100)},64)
            self.assertLessEqual(out.stat().st_size,64)

    def test_missing_duplicate_and_changed_input_members_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'in';contract=fixture(root)
            duplicated=copy.deepcopy(contract);duplicated['cases'][1]=duplicated['cases'][0]
            with self.assertRaises(ValueError):runner._load_inputs(root,duplicated)
            path=root/contract['cases'][0]['file'];saved=path.read_bytes();path.unlink()
            with self.assertRaises(ValueError):runner._load_inputs(root,contract)
            path.write_bytes(saved+b'changed')
            with self.assertRaises(ValueError):runner._load_inputs(root,contract)


class ArchiveTests(unittest.TestCase):
    def test_round_trip_and_independent_state_reconstruction(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);inputs=small();result=observer.observe_stage(inputs)
            entry=archive.export_stage(root,'tiny',inputs,inputs[-1],result)
            restored=archive.read_stage(root,entry,inputs,inputs[-1])
            np.testing.assert_array_equal(restored['values'].view(np.uint64),result['values'].view(np.uint64))
            self.assertEqual(restored['recurrence'],result['recurrence'])
            data=archive.read_json(root/entry['json']['file'],archive.JSON_CAP)
            data['observation']['states'][0]['residual']+=1
            (root/entry['json']['file']).write_text(json.dumps(data))
            entry['json']=archive.file_record(root/entry['json']['file'])
            with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])

    def test_rehashed_states_and_terminal_presence_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);inputs=small();result=observer.observe_stage(inputs)
            entry=archive.export_stage(root,'tiny',inputs,inputs[-1],result)
            path=root/entry['trajectory']['file']
            with np.load(path) as z:values=z['values'].copy();policies=z['policies'].copy()
            policies[0,0]=not policies[0,0]
            np.savez(path,values=values,policies=policies)
            entry['trajectory']=archive.file_record(path)
            with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);entry=archive.export_stage(root,'tiny',inputs,inputs[-1],result)
            path=root/entry['json']['file'];data=archive.read_json(path,archive.JSON_CAP)
            data['terminal_present']=False;path.write_text(json.dumps(data));entry['json']=archive.file_record(path)
            with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])

    def test_reason_phase_laundering_and_erased_prefix_refuse(self):
        for reason in ('policy_iteration_limit','linear_solve_failure','nonfinite_policy_solves','nonfinite_original_residual'):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);inputs=small();r=observer.observe_stage(inputs,max_updates=0)
                entry=archive.export_stage(root,'tiny',inputs,inputs[-1],r)
                path=root/entry['json']['file'];data=archive.read_json(path,archive.JSON_CAP)
                data['observation']['work']['reason']=reason
                data['observation']['observation_cap_reached']=False
                path.write_text(json.dumps(data));entry['json']=archive.file_record(path)
                with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);inputs=small();r=observer.observe_stage(inputs,max_updates=0)
            entry=archive.export_stage(root,'tiny',inputs,inputs[-1],r)
            path=root/entry['json']['file'];data=archive.read_json(path)
            obs=data['observation'];obs['states']=[]
            obs['work'].update(reason='nonfinite_original_residual',last_checked_residual=None)
            obs.update(nonfinite_observations=1,observation_cap_reached=False,
                recurrence=dict(first_state_repeat=None,first_policy_repeat=None,confirmed=None))
            data['terminal_present']=False
            empty=np.empty((0,3));policies=np.empty((0,3),dtype=bool)
            np.savez(root/entry['trajectory']['file'],values=empty,policies=policies)
            path.write_text(json.dumps(data))
            entry.update(states=0,json=archive.file_record(path),
                trajectory=archive.file_record(root/entry['trajectory']['file']))
            with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])

    def test_exact_rows_and_selected_state_contract(self):
        inputs=small();r=observer.observe_stage(inputs,max_updates=0)
        audit=archive.exact_audit(inputs,r)
        self.assertEqual(audit['indices'],[0]);self.assertEqual(audit['rows_checked'],3)
        row=audit['states'][0]
        self.assertEqual(row['exact']['complementarity_max'],'3/1')
        self.assertEqual(row['exact']['violations'],[1,2])
        self.assertEqual(row['decimal100']['complementarity_max'],'3')
        self.assertIsNone(row['exact']['selected_max'])
        # Selection is deterministic: final plus earliest two-lap close, deduped.
        scripted=dict(values=np.zeros((5,3)),policies=np.zeros((5,3),dtype=bool),
                      recurrence=dict(confirmed=dict(end=2)))
        self.assertEqual(archive.audit_indices(scripted),[2,4])
        scripted['recurrence']['confirmed']['end']=4
        self.assertEqual(archive.audit_indices(scripted),[4])

    def test_exact_cancellation_and_preceding_policy_are_independent(self):
        # Binary64 central+upper cancels to zero; the exact first row is +1.
        inputs=[np.zeros(2),np.array([1e16,1.,1.]),np.array([1.,0.]),
                np.array([1e16,0.,0.]),np.array([0.,1.,2.]),np.zeros(3)]
        result=dict(values=np.ones((2,3)),policies=np.array([[False,True,False],[False,True,True]]),
                    recurrence=dict(confirmed=None))
        audit=archive.exact_audit(inputs,result)['states'][0]
        self.assertEqual(audit['exact']['violations'],[0,2])
        self.assertEqual(audit['exact']['selected_violations'],[0,2])
        self.assertEqual(audit['exact']['complementarity_max'],'1/1')
        self.assertEqual(audit['exact']['selected_max'],'1/1')
        self.assertEqual(audit['decimal100']['complementarity_max'],'1')
        expected='0|1/1|1/1|1/1\n1|1/1|0/1|0/1\n2|1/1|-1/1|1/1\n'
        self.assertEqual(audit['exact']['rows_sha256'],digest(expected.encode()))
        with localcontext() as context:
            context.prec=3;context.rounding=ROUND_DOWN;context.Emax=1;context.Emin=-1
            self.assertEqual(archive.exact_audit(inputs,result)['states'][0],audit)

    def test_rehashed_exact_summary_and_noncanonical_policy_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);inputs=small();r=observer.observe_stage(inputs)
            entry=archive.export_stage(root,'tiny',inputs,inputs[-1],r)
            path=root/entry['json']['file'];data=archive.read_json(path)
            data['exact']['states'][0]['exact']['rows_sha256']='0'*64
            path.write_text(json.dumps(data));entry['json']=archive.file_record(path)
            with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])
            with np.load(root/entry['trajectory']['file']) as z:
                values=z['values'];policies=z['policies']
            policies.view(np.uint8)[0,0]=2
            np.savez(root/entry['trajectory']['file'],values=values,policies=policies)
            with self.assertRaises(ValueError):archive.load_npz(root/entry['trajectory']['file'],
                {'values':(np.dtype('float64'),values.shape),'policies':(np.dtype('bool'),policies.shape)})

    def test_stage_budget_and_compressed_trajectory_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);inputs=small();r=observer.observe_stage(inputs)
            with self.assertRaises(ValueError):archive.export_stage(root,'too-small',inputs,inputs[-1],r,cap=8)
            entry=archive.export_stage(root,'tiny',inputs,inputs[-1],r)
            path=root/entry['trajectory']['file']
            np.savez_compressed(path,values=r['values'],policies=r['policies'])
            entry['trajectory']=archive.file_record(path)
            with self.assertRaises(ValueError):archive.read_stage(root,entry,inputs,inputs[-1])


class RunTests(unittest.TestCase):
    def source(self,commit,require_clean=False):
        if commit!='a'*40:raise ValueError('Unexpected reviewed commit')
        return {'source.py':b'# synthetic source\n'}

    def test_complete_four_tiny_roundtrip_no_solver_in_reader(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(runner,'_git_sources',self.source):
            root=Path(tmp);contract=fixture(root/'in')
            runner._run(root/'in',root/'out','a'*40,contract)
            with patch.object(runner,'observe_stage',side_effect=AssertionError('Reader called solver')):
                report=runner._verify(root/'out','a'*40,contract)
            self.assertEqual(report['systems'],4)
            self.assertEqual(report['pde_runs'],0)
            self.assertLessEqual(report['linear_calls'],1024)
            self.assertEqual(report['automatic_exact_row_evaluations'],2*report['exact_rows'])
            with patch.object(archive,'TOTAL_CAP',runner._inventory(root/'out')[1]-1):
                with self.assertRaises(ValueError):runner._verify(root/'out','a'*40,contract)
            with self.assertRaises(FileExistsError):runner._run(root/'in',root/'out','a'*40,contract)
            with self.assertRaises(ValueError):runner._verify(root/'out','b'*40,contract)

    def test_incomplete_output_is_retained(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(runner,'_git_sources',self.source):
            root=Path(tmp);contract=fixture(root/'in')
            with patch.object(runner,'observe_stage',side_effect=RuntimeError('injected')):
                with self.assertRaises(RuntimeError):runner._run(root/'in',root/'out','a'*40,contract)
            self.assertFalse(archive.read_json(root/'out'/'manifest.json',archive.JSON_CAP)['completed'])
            with self.assertRaises(ValueError):runner._verify(root/'out','a'*40,contract)

    def test_final_reader_failure_never_publishes_complete(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(runner,'_git_sources',self.source):
            root=Path(tmp);contract=fixture(root/'in')
            def fail(*args,**kwargs):
                self.assertFalse(archive.read_json(root/'out'/'manifest.json')['completed'])
                raise ValueError('Reader rejects prospective manifest')
            with patch.object(runner,'_verify',side_effect=fail):
                with self.assertRaises(ValueError):runner._run(root/'in',root/'out','a'*40,contract)
            self.assertFalse(archive.read_json(root/'out'/'manifest.json')['completed'])
            self.assertEqual(len(list((root/'out'/'stages').glob('*.npz'))),4)

    def test_inventory_source_and_summary_mutants(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(runner,'_git_sources',self.source):
            root=Path(tmp);contract=fixture(root/'in');out=root/'out'
            runner._run(root/'in',out,'a'*40,contract)
            manifest_path=out/'manifest.json';original=manifest_path.read_bytes()
            for mutate in (lambda m:m.update(completed=False),
                           lambda m:m['stages'].append(copy.deepcopy(m['stages'][0])),
                           lambda m:m['summary'].update(linear_calls=0),
                           lambda m:m.update(source_commit='b'*40)):
                m=json.loads(original);mutate(m);manifest_path.write_text(json.dumps(m))
                with self.assertRaises(ValueError):runner._verify(out,'a'*40,contract)
            manifest_path.write_bytes(original)
            (out/'unlisted').write_text('extra')
            with self.assertRaises(ValueError):runner._verify(out,'a'*40,contract)
            (out/'unlisted').unlink()
            (out/'sources'/'source.py').write_text('# changed\n')
            m=json.loads(original);m['inventory']=runner._inventory(out)[0]
            manifest_path.write_text(json.dumps(m))
            with self.assertRaises(ValueError):runner._verify(out,'a'*40,contract)

    def test_frozen_git_and_original_source_checks_are_live(self):
        root=runner.HERE.parents[2]
        def fake_git(*args):
            if args==('rev-parse','--show-toplevel'):return str(root).encode()
            if args==('rev-parse','HEAD'):return b'a'*40
            if args==('status','--porcelain'):return b''
            if args[0]=='show':return (root/args[1].split(':',1)[1]).read_bytes()
            raise AssertionError(args)
        with patch.object(runner,'_git',side_effect=fake_git):
            sources=runner._git_sources('a'*40,require_clean=True)
            self.assertEqual(set(sources),set(runner.SOURCE_NAMES))
            with patch.dict(runner.PINNED,{'lcp_observe.py':'0'*64}):
                with self.assertRaises(ValueError):runner._git_sources('a'*40)
        def bad_git(*args):
            if args[0]=='show' and args[1].endswith('lcp_observe.py'):return b'# altered committed observer\n'
            return fake_git(*args)
        with patch.object(runner,'_git',side_effect=bad_git):
            with self.assertRaises(ValueError):runner._git_sources('a'*40)


if __name__=='__main__':unittest.main()
