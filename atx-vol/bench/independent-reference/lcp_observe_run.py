"""Explicit fixed four-saved-stage observation; no PDE entry point or tuning flags.

Run only after exact-source review. Verification performs no linear solve and
does not execute archived source. No historical reference decision is changed.
"""
import os
# Set before NumPy/SciPy import in the standalone launcher. This is not timing.
for _name in ('OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS'):
    os.environ[_name]='1'

import argparse
from datetime import datetime, timezone
from pathlib import Path
import platform
import re
import subprocess
import sys

import numpy as np
import scipy

import lcp_observe_archive as ar
from lcp_observe import MAX_UPDATES, SOURCE_HASHES, observe_stage


HERE=Path(__file__).resolve().parent
SOURCE_NAMES=('lcp_observe_run.py','lcp_observe_archive.py','test_lcp_observe_run.py',
              'lcp_observe.py','test_lcp_observe.py','lcp_correction.py','pde_reference.py',
              'lcp-four-stage-observation-plan-v1.md','lcp-four-stage-runner-plan-v1.md')
PINNED=dict(SOURCE_HASHES,**{'lcp_observe.py':'897c67981f508655074113bb1903dd738ebb165cb3256b17ff65d75a71bd7e30'})
_CASES=(
    ('dirk_l','space-fine',221,'76a0f697d0756e5b2c2ed4b3558e111dd8c9f334e833a90155000f06dc683056'),
    ('dirk_l','domain-wide',423,'99db1ddfa479aa26060d55e92c3192118aa4c076cc1983609849105059c2408a'),
    ('dirk_third','space-fine',190,'fa679a732cfb1e0440b17c716cfbddf0c428b8983fc9add26b70f962a45add74'),
    ('dirk_third','domain-wide',424,'c79dfccffce87347cc8138626cd9dd45a5009ab56fe6bdfb6416323a993067c2'))
CONTRACT=dict(cases=[dict(id=s+'-'+label,scheme=s,label=label,stage=stage,
    file=s+'-'+label+'-failure.npz',rows=16383,sha256=sha) for s,label,stage,sha in _CASES],
    selected='zero_rate_closed_form-put-03',requests=12,computed=8,refused=4,
    history={'manifest.json':'79350467d0298be8778c3ab644250476ad5e0d566451998d8eb00a1264c56a53',
             'outcomes.jsonl':'162feebb7f22d3262fec34310b7de5d78e566838da83c1f733eff4f3e7a63e69'})
MANIFEST_KEYS={'schema','completed','source_commit','versions','threads','started_utc','finished_utc',
               'contract','stages','summary','inventory','source_unchanged','inputs_unchanged',
               'pde_runs','historical_decisions_updated','timing_qualified','caps'}
CAPS=dict(max_updates_per_stage=128,max_linear_calls_per_stage=256,
          stage_bytes=ar.STAGE_CAP,total_bytes=ar.TOTAL_CAP,json_bytes=ar.JSON_CAP,
          max_exact_rows_per_pass=131064,automatic_exact_audit_passes=2)


def _git(*args):
    return subprocess.run(['git','-C',str(HERE),*args],check=True,capture_output=True).stdout


def _git_sources(commit,require_clean=False):
    ar.require(type(commit) is str and re.fullmatch('[0-9a-f]{40}',commit) is not None)
    root=Path(_git('rev-parse','--show-toplevel').decode().strip())
    if require_clean:
        ar.require(_git('rev-parse','HEAD').decode().strip()==commit and not _git('status','--porcelain').strip(),
                   'Require a clean frozen source checkout')
    sources={}
    for name in SOURCE_NAMES:
        path=HERE/name
        ar.require(not path.is_symlink())
        source=path.read_bytes()
        git_path=path.relative_to(root).as_posix()
        committed=_git('show',commit+':'+git_path)
        canonical=source.replace(b'\r\n',b'\n')
        ar.require(canonical==committed.replace(b'\r\n',b'\n'),'Source differs from reviewed commit: '+name)
        if name in PINNED:ar.require(ar.sha(canonical)==PINNED[name],'Approved observer/primitive changed')
        sources[name]=source
    return sources


def _load_inputs(directory,contract):
    """Private explicit contract permits tiny tests; public CLI always uses CONTRACT."""
    directory=Path(directory)
    ar.require(directory.is_dir() and not directory.is_symlink())
    cases=contract['cases']
    ar.require(len(cases)==4 and len({c['id'] for c in cases})==4 and len({c['file'] for c in cases})==4)
    ar.require({p.name for p in directory.glob('*-failure.npz')}=={c['file'] for c in cases},'Failure input denominator changed')
    for name,expected in contract['history'].items():
        path=directory/name
        ar.require(path.name==name and not path.is_symlink() and path.stat().st_size<=8*ar.JSON_CAP)
        ar.require(ar.sha(path.read_bytes())==expected,'Historical input hash mismatch')
    manifest=ar.read_json(directory/'manifest.json')
    ar.require(manifest['schema']=='atx-zero-rate-lcp-control-v1' and manifest['completed'] is True)
    ar.require(manifest['lcp_mode']=='one_correction' and manifest['selected_ids']==[contract['selected']])
    ar.require(manifest['requested_results']==contract['requests'] and manifest['computed']==contract['computed']
               and manifest['refused']==contract['refused']==4)
    rows=[ar.parse_json(line) for line in (directory/'outcomes.jsonl').read_bytes().splitlines()]
    ar.require(len(rows)==contract['requests'])
    identities=[(r['scheme'],r['label']) for r in rows]
    requests=manifest['requests']
    ar.require(len(set(identities))==len(rows)==len(requests))
    ar.require([(r['scheme'],r['label']) for r in requests]==identities)
    for row,request in zip(rows,requests):
        ar.require(all(row[k]==v for k,v in request.items()))
        ar.require(row['id']==contract['selected'] and row['lcp_mode']=='one_correction')
    ar.require(sum(r['status']=='computed' for r in rows)==contract['computed'])
    failed=[r for r in rows if r['status']=='refused']
    ar.require(len(failed)==4 and {(r['scheme'],r['label']) for r in failed}=={(c['scheme'],c['label']) for c in cases})
    loaded=[]
    for case in cases:
        row=next(r for r in failed if (r['scheme'],r['label'])==(case['scheme'],case['label']))
        ar.require(row['exception_type']=='LcpCorrectionRefused')
        ar.require(row['failure_snapshot']==dict(file=case['file'],sha256=case['sha256']))
        n=case['rows'];ar.require(row['options']['nx']==n+2)
        last=row['stage_work'][-1];work=last['work']
        ar.require(last['stage']==case['stage'] and last['status']=='refused')
        ar.require(work['mode']=='one_correction' and work['reason']=='policy_iteration_limit')
        ar.require(type(work['tolerance']) is float and work['tolerance']==ar.TOLERANCE)
        for key in ('iterations','policy_solves','correction_solves','policy_iteration_bound'):
            ar.require(type(work[key]) is int and work[key]==n+2)
        for key in ('linear_solves','linear_solve_bound','linear_solve_budget'):
            ar.require(type(work[key]) is int and work[key]==2*(n+2))
        ar.require(work['residual_checks']==n+3)
        path=directory/case['file']
        ar.require(Path(case['file']).name==case['file'] and not path.is_symlink())
        ar.require(path.stat().st_size<=ar.STAGE_CAP and ar.sha(path.read_bytes())==case['sha256'])
        data=ar.load_npz(path,ar.input_specs(n))
        arrays=[data[k] for k in ar.INPUT_NAMES[:6]]
        old=data['last_checked_values']
        cont=arrays[1]*old
        cont[1:]+=arrays[0]*old[:-1];cont[:-1]+=arrays[2]*old[1:];cont-=arrays[3]
        residual=float(np.max(np.abs(np.minimum(cont,old-arrays[4]))))
        ar.require(np.isfinite(cont).all() and residual>ar.TOLERANCE and residual==work['last_checked_residual'])
        loaded.append(dict(case=case,arrays=arrays,old_terminal=old,input_path=path))
    return loaded


def _versions():
    return dict(python=sys.version,numpy=np.__version__,scipy=scipy.__version__,platform=platform.platform())


def _inventory(directory):
    result={}
    total=0
    for path in sorted(Path(directory).rglob('*')):
        ar.require(not path.is_symlink() and not path.is_junction(),'Linked evidence path')
        if path.is_dir():continue
        ar.require(path.is_file())
        total+=path.stat().st_size
        ar.require(total<=ar.TOTAL_CAP,'Complete output storage cap exceeded')
        if path.name=='manifest.json' and path.parent==directory:continue
        record=ar.file_record(path)
        result[path.relative_to(directory).as_posix()]=dict(bytes=record['bytes'],sha256=record['sha256'])
    return result,total


def _copy_new(output,path,data):
    _,used=_inventory(output)
    ar.require(used+len(data)+2*ar.JSON_CAP<=ar.TOTAL_CAP,'Complete output storage cap exceeded')
    with path.open('xb') as f:f.write(data)


def _summary(results):
    stages=[]
    for identity,result in results:
        confirmed=result['recurrence']['confirmed']
        stages.append(dict(id=identity,status=result['status'],
            reason=None if result['status']=='computed' else result['work']['reason'],
            updates=result['work']['iterations'],linear_calls=result['linear_calls'],
            checked_states=len(result['states']),observation_cap=result['observation_cap_reached'],
            recurrence='inconclusive' if confirmed is None else confirmed['kind'],
            exact_rows=result['values'].shape[1]*len(ar.audit_indices(result)),
            checked_state_reserved_bytes=result['allocated_state_bytes'],
            checked_state_retained_bytes=result['values'].nbytes+result['policies'].nbytes,
            original_input_array_bytes=(7*result['values'].shape[1]-2)*8,
            solver_temporary_bytes=None))
    return dict(systems=len(stages),pde_runs=0,stages=stages,
        updates=sum(s['updates'] for s in stages),linear_calls=sum(s['linear_calls'] for s in stages),
        exact_rows=sum(s['exact_rows'] for s in stages),
        automatic_exact_row_evaluations=2*sum(s['exact_rows'] for s in stages),
        historical_decisions_updated=False,timing_qualified=False)


def _verify(directory,source_commit,contract,*,_manifest=None):
    directory=Path(directory)
    ar.require(directory.is_dir() and not directory.is_symlink())
    manifest=ar.read_json(directory/'manifest.json') if _manifest is None else _manifest
    ar.require(set(manifest)==MANIFEST_KEYS and manifest['schema']=='atx-lcp-four-observation-v1')
    ar.require(manifest['completed'] is True and manifest['source_commit']==source_commit)
    ar.require(ar.same_json(manifest['contract'],contract) and ar.same_json(manifest['caps'],CAPS))
    ar.require(manifest['source_unchanged'] is True and manifest['inputs_unchanged'] is True)
    ar.require(manifest['pde_runs']==0 and manifest['historical_decisions_updated'] is False and manifest['timing_qualified'] is False)
    ar.require(manifest['versions']==_versions(),'Reader environment differs from recorded arithmetic environment')
    ar.require(manifest['threads']=={name:'1' for name in ('OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS')})
    for key in ('started_utc','finished_utc'):
        value=datetime.fromisoformat(manifest[key]);ar.require(value.utcoffset().total_seconds()==0)
    inventory,_=_inventory(directory)
    ar.require(ar.same_json(inventory,manifest['inventory']),'Archive inventory mismatch')
    sources=_git_sources(source_commit)
    expected={f'sources/{name}' for name in sources}|{f'inputs/{name}' for name in contract['history']}|{
        'inputs/'+c['file'] for c in contract['cases']}|{f'stages/{c["id"]}{suffix}' for c in contract['cases'] for suffix in ('.json','.npz')}
    ar.require(set(inventory)==expected)
    ar.require({p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_dir()}=={'inputs','sources','stages'})
    for name,data in sources.items():
        ar.require((directory/'sources'/name).read_bytes().replace(b'\r\n',b'\n')==data.replace(b'\r\n',b'\n'),
                   'Archived source does not match reviewed Git source')
    loaded=_load_inputs(directory/'inputs',contract)
    ar.require([entry['id'] for entry in manifest['stages']]==[c['id'] for c in contract['cases']])
    summaries=[]
    for item,entry in zip(loaded,manifest['stages']):
        result=ar.read_stage(directory/'stages',entry,item['arrays'],item['old_terminal'])
        ar.require(result['max_updates']==MAX_UPDATES)
        stage_files=entry['trajectory']['bytes']+entry['json']['bytes']+item['input_path'].stat().st_size
        logical=ar.npz_storage(item['input_path'])+ar.npz_storage(directory/'stages'/entry['trajectory']['file'])+entry['json']['bytes']
        ar.require(max(stage_files,logical)<=ar.STAGE_CAP)
        # Only small derived summary objects survive the next stage; trajectories are sequential.
        summaries.extend(_summary([(entry['id'],result)])['stages'])
    report=dict(systems=len(summaries),pde_runs=0,stages=summaries,
        updates=sum(s['updates'] for s in summaries),linear_calls=sum(s['linear_calls'] for s in summaries),
        exact_rows=sum(s['exact_rows'] for s in summaries),
        automatic_exact_row_evaluations=2*sum(s['exact_rows'] for s in summaries),
        historical_decisions_updated=False,timing_qualified=False)
    ar.require(report['systems']==4 and report['updates']<=512 and report['linear_calls']<=1024 and report['exact_rows']<=131064)
    ar.require(ar.same_json(manifest['summary'],report),'Derived denominator/work summary mismatch')
    return report


def _run(inputs,output,source_commit,contract):
    inputs,output=Path(inputs).resolve(),Path(output).resolve()
    ar.require(not output.is_relative_to(inputs),'Output must not modify the historical input directory')
    sources=_git_sources(source_commit,require_clean=True)
    loaded=_load_inputs(inputs,contract)
    output.mkdir(parents=True,exist_ok=False)
    initial=dict(schema='atx-lcp-four-observation-v1',completed=False,source_commit=source_commit,
        versions=_versions(),threads={name:os.environ[name] for name in ('OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS')},
        started_utc=datetime.now(timezone.utc).isoformat(),finished_utc=None,contract=contract,
        stages=[],summary=None,inventory={},source_unchanged=False,inputs_unchanged=False,
        pde_runs=0,historical_decisions_updated=False,timing_qualified=False,caps=CAPS)
    ar.write_json(output/'manifest.json',initial)
    for name in ('sources','inputs','stages'):(output/name).mkdir()
    for name,data in sources.items():_copy_new(output,output/'sources'/name,data)
    for name in list(contract['history'])+[c['file'] for c in contract['cases']]:
        _copy_new(output,output/'inputs'/name,(inputs/name).read_bytes())
    summaries=[]
    for item in loaded:
        _,used=_inventory(output)
        ar.require(used+ar.STAGE_CAP+2*ar.JSON_CAP<=ar.TOTAL_CAP,'Complete output storage reservation failed')
        result=observe_stage(item['arrays'],ar.TOLERANCE,max_updates=MAX_UPDATES)
        entry=ar.export_stage(output/'stages',item['case']['id'],item['arrays'],item['old_terminal'],result)
        initial['stages'].append(entry)
        summaries.extend(_summary([(entry['id'],result)])['stages'])
        del result
    ar.require(_git_sources(source_commit,require_clean=True)==sources,'Source changed during observation')
    _load_inputs(inputs,contract)  # Rehash/rejoin original bytes; never run a second observation.
    initial.update(completed=True,source_unchanged=True,inputs_unchanged=True,
                   finished_utc=datetime.now(timezone.utc).isoformat())
    initial['summary']=dict(systems=len(summaries),pde_runs=0,stages=summaries,
        updates=sum(s['updates'] for s in summaries),linear_calls=sum(s['linear_calls'] for s in summaries),
        exact_rows=sum(s['exact_rows'] for s in summaries),
        automatic_exact_row_evaluations=2*sum(s['exact_rows'] for s in summaries),
        historical_decisions_updated=False,timing_qualified=False)
    initial['inventory']=_inventory(output)[0]
    # One independent reader pass, before any complete marker exists on disk.
    # A killed/failed validation leaves the original incomplete manifest intact.
    report=_verify(output,source_commit,contract,_manifest=initial)
    ar.require(_git_sources(source_commit,require_clean=True)==sources,'Source changed during validation')
    _load_inputs(inputs,contract)
    pending=output/'manifest.complete.json'
    ar.write_json(pending,initial)
    pending.replace(output/'manifest.json')
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='command',required=True)
    run=commands.add_parser('run')
    run.add_argument('--inputs',type=Path,required=True);run.add_argument('--output',type=Path,required=True)
    verify=commands.add_parser('verify')
    verify.add_argument('--directory',type=Path,required=True);verify.add_argument('--source-commit',required=True)
    args=parser.parse_args()
    if args.command=='run':
        commit=_git('rev-parse','HEAD').decode().strip()
        result=_run(args.inputs,args.output,commit,CONTRACT)
    else:result=_verify(args.directory,args.source_commit,CONTRACT)
    print(ar.encoded_json(result).decode(),end='')


if __name__=='__main__':main()
