"""Bounded retained-policy archive with an independent no-solve reader.

Only pure file and exact-row helpers are shared with the strict archive. Its
policy reader is never used: actual selections are separate from raw masks.
"""
from pathlib import Path
import zipfile

import numpy as np

import lcp_observe_archive as common
from lcp_retained_observe import MAX_UPDATES, verify_retained


SCHEMA='atx-lcp-retained-stage-v1'
SELECTION_KEYS={'index','source_state','linear_calls','selected_multiplies','policy_sha256'}
POLICY_KEYS={'workspace_required_bytes','workspace_reserved_bytes','selection_attempts',
             'selection_completions','initial_policy_rows','admissible_retained_rows',
             'retained_overrides','failing_reselected_rows','policy_changes'}


def exact_audit(inputs,result):
    # policies[k-1] in the shared evaluator must be the actual completed
    # selection for state k. Raw comparisons never enter this slot.
    return common.exact_audit(inputs,dict(values=result['values'],
        policies=result['used_policies'],recurrence=result['recurrence']))


def _shape(observation):
    common.require(set(observation)==common.OBS_KEYS|{'selections'})
    extra={'complementarity_residual'} if observation['status']=='computed' else {'reason','last_checked_residual'}
    common.require(set(observation['work'])==common.WORK_KEYS|extra|{'base_mode','policy_work'})
    common.require(set(observation['work']['policy_work'])==POLICY_KEYS)
    common.require(all(set(row)==common.STATE_KEYS for row in observation['states']))
    common.require(all(set(row)==SELECTION_KEYS for row in observation['selections']))
    recurrence=observation['recurrence']
    names=('first_state_repeat','first_raw_policy_repeat','first_used_policy_repeat')
    common.require(set(recurrence)==set(names)|{'confirmed'})
    for name in names:
        entry=recurrence[name]
        common.require(entry is None or set(entry)=={'start','end'})
        if entry is not None:common.require(all(type(entry[k]) is int for k in ('start','end')))
    entry=recurrence['confirmed']
    common.require(entry is None or set(entry)=={'kind','start','period','end'})
    if entry is not None:common.require(all(type(entry[k]) is int for k in ('start','period','end')))


def export_stage(directory,identity,inputs,old_terminal,result,*,cap=common.STAGE_CAP):
    common.require(type(cap) is int and 0<=cap<=common.STAGE_CAP)
    common.require(type(identity) is str and identity and all(c.isalnum() or c in '-_' for c in identity))
    verify_retained(inputs,result,tolerance=common.TOLERANCE)
    arrays={k:result[k] for k in ('values','raw_policies','used_policies')}
    observation={k:v for k,v in result.items() if k not in set(arrays)|{'terminal_values'}}
    _shape(observation)
    input_bytes=sum(a.nbytes for a in inputs)+old_terminal.nbytes
    common.require(input_bytes+sum(a.nbytes for a in arrays.values())<=cap)
    values=arrays['values']
    matches=bool(len(values) and np.array_equal(values[-1].view(np.uint64),old_terminal.view(np.uint64)))
    payload=dict(schema=SCHEMA,id=identity,observation=observation,
        terminal_present=result['terminal_values'] is not None,
        original_terminal_bits_equal=matches,exact=exact_audit(inputs,result))
    encoded=common.encoded_json(payload)
    common.require(len(encoded)<=common.JSON_CAP)
    directory=Path(directory)
    trajectory=directory/(identity+'.npz')
    common.write_npz(trajectory,arrays,cap-input_bytes-len(encoded))
    scalar=directory/(identity+'.json')
    common.write_json(scalar,payload)
    return dict(id=identity,states=len(values),selections=len(arrays['used_policies']),rows=len(inputs[1]),
        trajectory=common.file_record(trajectory),json=common.file_record(scalar))


def read_stage(directory,entry,inputs,old_terminal):
    common.require(set(entry)=={'id','states','selections','rows','trajectory','json'})
    identity=entry['id']
    common.require(type(identity) is str and identity and all(c.isalnum() or c in '-_' for c in identity))
    n=len(inputs[1]);count=entry['states'];selections=entry['selections']
    common.require(type(count) is int and 0<=count<=MAX_UPDATES+1)
    common.require(type(selections) is int and 0<=selections<=min(count,MAX_UPDATES))
    common.require(type(entry['rows']) is int and entry['rows']==n)
    trajectory=common.checked_file(directory,entry['trajectory'],identity+'.npz',common.STAGE_CAP)
    scalar=common.checked_file(directory,entry['json'],identity+'.json',common.JSON_CAP)
    with zipfile.ZipFile(trajectory) as z:
        common.require(all(m.compress_type==zipfile.ZIP_STORED for m in z.infolist()),'Trajectory must be uncompressed')
    common.require(common.npz_storage(trajectory)+scalar.stat().st_size+
        sum(a.nbytes for a in inputs)+old_terminal.nbytes<=common.STAGE_CAP)
    arrays=common.load_npz(trajectory,{'values':(np.dtype('float64'),(count,n)),
        'raw_policies':(np.dtype('bool'),(count,n)),
        'used_policies':(np.dtype('bool'),(selections,n))})
    payload=common.read_json(scalar)
    common.require(set(payload)=={'schema','id','observation','terminal_present','original_terminal_bits_equal','exact'})
    common.require(payload['schema']==SCHEMA and payload['id']==identity)
    common.require(type(payload['terminal_present']) is bool and payload['terminal_present']==bool(count))
    observation=payload['observation'];_shape(observation)
    result=dict(observation,**arrays,terminal_values=arrays['values'][-1] if count else None)
    verify_retained(inputs,result,tolerance=common.TOLERANCE)
    expected=bool(count and np.array_equal(arrays['values'][-1].view(np.uint64),old_terminal.view(np.uint64)))
    common.require(type(payload['original_terminal_bits_equal']) is bool and payload['original_terminal_bits_equal']==expected)
    common.require(common.same_json(payload['exact'],exact_audit(inputs,result)),'Independent exact-row audit mismatch')
    return result
