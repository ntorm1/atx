"""Fixed four-stage retained-policy experiment; run only after source review.

No PDE entry point, tuning flags, retries or historical decision rewrites.
The reader never solves and never executes archived source.
"""
import os
for _name in ('OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS'):
    os.environ[_name]='1'

import argparse
from datetime import datetime, timezone
from pathlib import Path
import re

import lcp_observe_archive as common
import lcp_observe_run as predecessor
import lcp_retained_archive as archive
from lcp_retained_observe import MAX_UPDATES, SOURCE_HASHES, observe_retained


HERE=Path(__file__).resolve().parent
SCHEMA='atx-lcp-four-retained-observation-v1'
CONTRACT=predecessor.CONTRACT
MANIFEST_KEYS=predecessor.MANIFEST_KEYS
CAPS=dict(predecessor.CAPS,mode='one_correction_retained_admissible',
          max_retained_state_bytes=21117687,max_selector_reservation_bytes=524256)
SOURCE_NAMES=predecessor.SOURCE_NAMES+(
    'lcp_policy_retention.py','test_lcp_policy_retention.py','lcp-policy-retention-plan-v1.md',
    'lcp_retained_observe.py','test_lcp_retained_observe.py','lcp-retained-observation-plan-v1.md',
    'lcp_retained_archive.py','lcp_retained_run.py','test_lcp_retained_run.py')
PINNED=dict(SOURCE_HASHES,**{
    'lcp_retained_observe.py':'9a59c72040e8e4d490cf2a56d35ba94c6ab5748bb70fc6e928eb902e9b9b34f1',
    'lcp_observe_archive.py':'3d247bcb7e5b69b90fdad66678d33a707a3f8e014875dd631391dd24c7e9b2d9',
    'lcp_observe_run.py':'bb8f4cf80bb6edfa4717215dbf26c654c36f66fe6bf88cd189726604a18d0070'})


def _git_sources(commit,require_clean=False):
    common.require(type(commit) is str and re.fullmatch('[0-9a-f]{40}',commit) is not None)
    root=Path(predecessor._git('rev-parse','--show-toplevel').decode().strip())
    if require_clean:
        common.require(predecessor._git('rev-parse','HEAD').decode().strip()==commit and
            not predecessor._git('status','--porcelain').strip(),'Require clean frozen source checkout')
    sources={}
    for name in SOURCE_NAMES:
        path=HERE/name
        common.require(not path.is_symlink())
        source=path.read_bytes();canonical=source.replace(b'\r\n',b'\n')
        committed=predecessor._git('show',commit+':'+path.relative_to(root).as_posix())
        common.require(canonical==committed.replace(b'\r\n',b'\n'),'Source differs from reviewed commit: '+name)
        if name in PINNED:common.require(common.sha(canonical)==PINNED[name],'Frozen numerical/reader source changed')
        sources[name]=source
    return sources


def _stage_summary(identity,result):
    confirmed=result['recurrence']['confirmed'];n=result['values'].shape[1]
    return dict(id=identity,status=result['status'],
        reason=None if result['status']=='computed' else result['work']['reason'],
        updates=result['work']['iterations'],linear_calls=result['linear_calls'],
        checked_states=len(result['states']),actual_selections=len(result['selections']),
        observation_cap=result['observation_cap_reached'],
        recurrence='inconclusive' if confirmed is None else confirmed['kind'],
        exact_rows=n*len(common.audit_indices(result)),
        checked_state_reserved_bytes=result['allocated_state_bytes'],
        checked_state_retained_bytes=sum(result[k].nbytes for k in ('values','raw_policies','used_policies')),
        original_input_array_bytes=(7*n-2)*8,
        selector_reserved_bytes=result['work']['policy_work']['workspace_reserved_bytes'],
        policy_retained_overrides=result['work']['policy_work']['retained_overrides'],
        solver_temporary_bytes=None)


def _summary(stages):
    return dict(systems=len(stages),pde_runs=0,stages=stages,
        updates=sum(s['updates'] for s in stages),linear_calls=sum(s['linear_calls'] for s in stages),
        exact_rows=sum(s['exact_rows'] for s in stages),
        automatic_exact_row_evaluations=2*sum(s['exact_rows'] for s in stages),
        policy_retained_overrides=sum(s['policy_retained_overrides'] for s in stages),
        historical_decisions_updated=False,timing_qualified=False)


def _verify(directory,source_commit,contract,*,_manifest=None):
    directory=Path(directory)
    common.require(directory.is_dir() and not directory.is_symlink())
    manifest=common.read_json(directory/'manifest.json') if _manifest is None else _manifest
    common.require(set(manifest)==MANIFEST_KEYS and manifest['schema']==SCHEMA)
    common.require(manifest['completed'] is True and manifest['source_commit']==source_commit)
    common.require(common.same_json(manifest['contract'],contract) and common.same_json(manifest['caps'],CAPS))
    common.require(manifest['source_unchanged'] is True and manifest['inputs_unchanged'] is True)
    common.require(type(manifest['pde_runs']) is int and manifest['pde_runs']==0 and
        manifest['historical_decisions_updated'] is False and manifest['timing_qualified'] is False)
    common.require(common.same_json(manifest['versions'],predecessor._versions()),'Reader arithmetic environment differs')
    common.require(manifest['threads']=={name:'1' for name in ('OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS')})
    stamps=[]
    for key in ('started_utc','finished_utc'):
        value=datetime.fromisoformat(manifest[key])
        common.require(value.utcoffset() is not None and value.utcoffset().total_seconds()==0)
        stamps.append(value)
    common.require(stamps[0]<=stamps[1])
    inventory,_=predecessor._inventory(directory)
    common.require(common.same_json(inventory,manifest['inventory']),'Archive inventory mismatch')
    sources=_git_sources(source_commit)
    expected={f'sources/{name}' for name in sources}|{f'inputs/{name}' for name in contract['history']}|{
        'inputs/'+c['file'] for c in contract['cases']}|{
        f'stages/{c["id"]}{suffix}' for c in contract['cases'] for suffix in ('.json','.npz')}
    common.require(set(inventory)==expected)
    common.require({p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_dir()}=={'inputs','sources','stages'})
    for name,data in sources.items():
        common.require((directory/'sources'/name).read_bytes().replace(b'\r\n',b'\n')==data.replace(b'\r\n',b'\n'),
            'Archived source does not match reviewed Git source')
    loaded=predecessor._load_inputs(directory/'inputs',contract)
    common.require([entry['id'] for entry in manifest['stages']]==[c['id'] for c in contract['cases']])
    summaries=[]
    for item,entry in zip(loaded,manifest['stages']):
        result=archive.read_stage(directory/'stages',entry,item['arrays'],item['old_terminal'])
        common.require(result['max_updates']==MAX_UPDATES)
        actual=entry['trajectory']['bytes']+entry['json']['bytes']+item['input_path'].stat().st_size
        logical=common.npz_storage(item['input_path'])+common.npz_storage(directory/'stages'/entry['trajectory']['file'])+entry['json']['bytes']
        common.require(max(actual,logical)<=common.STAGE_CAP)
        summaries.append(_stage_summary(entry['id'],result))
        del result
    report=_summary(summaries)
    common.require(report['systems']==4 and report['updates']<=512 and report['linear_calls']<=1024 and report['exact_rows']<=131064)
    common.require(common.same_json(manifest['summary'],report),'Derived denominator/work summary mismatch')
    return report


def _run(inputs,output,source_commit,contract):
    inputs,output=Path(inputs).resolve(),Path(output).resolve()
    common.require(not output.is_relative_to(inputs),'Output must not modify historical input directory')
    sources=_git_sources(source_commit,require_clean=True)
    loaded=predecessor._load_inputs(inputs,contract)
    output.mkdir(parents=True,exist_ok=False)
    manifest=dict(schema=SCHEMA,completed=False,source_commit=source_commit,
        versions=predecessor._versions(),threads={name:os.environ[name] for name in ('OMP_NUM_THREADS','OPENBLAS_NUM_THREADS','MKL_NUM_THREADS')},
        started_utc=datetime.now(timezone.utc).isoformat(),finished_utc=None,contract=contract,
        stages=[],summary=None,inventory={},source_unchanged=False,inputs_unchanged=False,
        pde_runs=0,historical_decisions_updated=False,timing_qualified=False,caps=CAPS)
    common.write_json(output/'manifest.json',manifest)
    for name in ('sources','inputs','stages'):(output/name).mkdir()
    for name,data in sources.items():predecessor._copy_new(output,output/'sources'/name,data)
    for name in list(contract['history'])+[c['file'] for c in contract['cases']]:
        predecessor._copy_new(output,output/'inputs'/name,(inputs/name).read_bytes())
    summaries=[]
    for item in loaded:
        _,used=predecessor._inventory(output)
        common.require(used+common.STAGE_CAP+2*common.JSON_CAP<=common.TOTAL_CAP,'Complete output storage reservation failed')
        result=observe_retained(item['arrays'],common.TOLERANCE,max_updates=MAX_UPDATES)
        entry=archive.export_stage(output/'stages',item['case']['id'],item['arrays'],item['old_terminal'],result)
        manifest['stages'].append(entry);summaries.append(_stage_summary(entry['id'],result))
        del result
    common.require(_git_sources(source_commit,require_clean=True)==sources,'Source changed during observation')
    predecessor._load_inputs(inputs,contract)
    manifest.update(completed=True,source_unchanged=True,inputs_unchanged=True,
        finished_utc=datetime.now(timezone.utc).isoformat(),summary=_summary(summaries))
    manifest['inventory']=predecessor._inventory(output)[0]
    # Exactly one independent reader pass. Any error leaves the incomplete
    # marker intact; no automatic solve/retry or extra exact audit is hidden.
    report=_verify(output,source_commit,contract,_manifest=manifest)
    common.require(_git_sources(source_commit,require_clean=True)==sources,'Source changed during validation')
    predecessor._load_inputs(inputs,contract)
    pending=output/'manifest.complete.json'
    common.write_json(pending,manifest)
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
        result=_run(args.inputs,args.output,predecessor._git('rev-parse','HEAD').decode().strip(),CONTRACT)
    else:result=_verify(args.directory,args.source_commit,CONTRACT)
    print(common.encoded_json(result).decode(),end='')


if __name__=='__main__':main()
