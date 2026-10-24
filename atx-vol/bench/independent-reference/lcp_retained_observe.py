"""Private retained-policy observation and an independent no-solve reader.

Actual selections and raw comparisons are different evidence. Full state also
includes preceding policy memory. No saved-stage loader or PDE call lives here.
"""
import hashlib
import math
from pathlib import Path
import types

import numpy as np
from scipy.linalg import solve_banded as _delegate_solve

from lcp_observe import MAX_ROWS, MAX_UPDATES, SOURCE_HASHES as NUMERIC_HASHES
from lcp_observe import _inputs, _bits_equal, _float_bits


MAX_STATE_BYTES=((MAX_UPDATES+1)*9+MAX_UPDATES)*MAX_ROWS
MODE='one_correction_retained_admissible'
SOURCE_HASHES=dict(NUMERIC_HASHES,**{
    'lcp_observe.py':'897c67981f508655074113bb1903dd738ebb165cb3256b17ff65d75a71bd7e30',
    'lcp_policy_retention.py':'7aa0f0d09e0ad577d806319e234b9b9a2e1012a9035970d4a83c3550dd641237'})


def _require(value):
    if not value:raise ValueError('Contradictory retained-policy evidence')


def _digest(data):return hashlib.sha256(data).hexdigest()


def _load_retention():
    sources={}
    for name,expected in SOURCE_HASHES.items():
        path=Path(__file__).with_name(name)
        content=path.read_bytes().replace(b'\r\n',b'\n')
        _require(_digest(content)==expected)
        sources[name]=content
    module=types.ModuleType('_lcp_retained_observation_primitive')
    module.__file__=str(Path(__file__).with_name('lcp_policy_retention.py'))
    exec(compile(sources['lcp_policy_retention.py'],module.__file__,'exec'),module.__dict__)
    return module


def retained_recurrence(values,raw,used,residuals):
    _require(isinstance(values,np.ndarray) and values.dtype==np.float64 and values.ndim==2)
    count,n=values.shape
    _require(1<=n<=MAX_ROWS and count<=MAX_UPDATES+1 and np.isfinite(values).all())
    _require(isinstance(raw,np.ndarray) and raw.dtype==np.bool_ and raw.shape==values.shape)
    _require(isinstance(used,np.ndarray) and used.dtype==np.bool_ and used.ndim==2 and used.shape[1]==n)
    _require(len(used) in (max(0,count-1),count) and len(used)<=MAX_UPDATES)
    _require(np.all(raw.view(np.uint8)<=1) and np.all(used.view(np.uint8)<=1))
    _require(len(residuals)==count and all(type(r) is float and math.isfinite(r) and r>=0 for r in residuals))
    keys=[_digest(values[i].tobytes()+raw[i].tobytes()+_float_bits(residuals[i])+
                  (b'initial' if i==0 else b'previous'+used[i-1].tobytes())) for i in range(count)]
    def equal(i,j):
        return keys[i]==keys[j] and (i==0)==(j==0) and _bits_equal(values[i],values[j]) and \
            np.array_equal(raw[i],raw[j]) and _float_bits(residuals[i])==_float_bits(residuals[j]) and \
            (i==0 or np.array_equal(used[i-1],used[j-1]))
    first_state=first_raw=first_used=None
    for end in range(count):
        for start in range(end):
            if first_state is None and equal(start,end):first_state=dict(start=start,end=end)
            if first_raw is None and np.array_equal(raw[start],raw[end]):first_raw=dict(start=start,end=end)
            if start>0 and first_used is None and np.array_equal(used[start-1],used[end-1]):
                first_used=dict(start=start,end=end)
    confirmed=None
    for end in range(2,count):
        for period in range(1,end//2+1):
            start=end-2*period
            if all(equal(start+k,start+period+k) for k in range(period+1)):
                confirmed=dict(kind='stagnation' if period==1 else 'cycle',start=start,period=period,end=end)
                break
        if confirmed is not None:break
    return dict(first_state_repeat=first_state,first_raw_policy_repeat=first_raw,
                first_used_policy_repeat=first_used,confirmed=confirmed)


def observe_retained(arrays,tolerance=1e-10,*,max_updates=MAX_UPDATES,max_state_bytes=MAX_STATE_BYTES):
    inputs,tolerance=_inputs(arrays,tolerance)
    _require(type(max_updates) is int and 0<=max_updates<=MAX_UPDATES)
    _require(type(max_state_bytes) is int and 0<=max_state_bytes<=MAX_STATE_BYTES)
    n=len(inputs[1]);updates=min(n+2,max_updates)
    required=((updates+1)*9+updates)*n
    _require(required<=max_state_bytes)
    module=_load_retention()
    inputs=[a.copy() for a in inputs]
    for a in inputs:a.flags.writeable=False
    hashes=[_digest(a.tobytes()) for a in inputs]
    values=np.empty((updates+1,n),dtype=np.float64)
    raw=np.empty((updates+1,n),dtype=np.bool_)
    used=np.empty((updates,n),dtype=np.bool_)
    states=[];selections=[]
    linear_calls=selected_multiplies=nonfinite_observations=0
    ordinary_selector=module.PolicySelector
    ordinary_primitive=module._primitive

    class ObservedSelector(ordinary_selector):
        def choose(self,continuation,gap,tol):
            selected=super().choose(continuation,gap,tol)
            index=len(selections)
            _require(index<len(used) and index==len(states)-1)
            used[index]=selected
            selections.append(dict(index=index,source_state=index,linear_calls=linear_calls,
                selected_multiplies=selected_multiplies,policy_sha256=_digest(selected.tobytes())))
            return selected

    def instrument(selector=None):
        primitive=ordinary_primitive(selector)
        original_multiply=primitive._multiply
        def multiply(lower,diagonal,upper,current):
            nonlocal selected_multiplies,nonfinite_observations
            product=original_multiply(lower,diagonal,upper,current)
            if not (lower is inputs[0] and diagonal is inputs[1] and upper is inputs[2]):
                selected_multiplies+=1
                return product
            with np.errstate(over='ignore',invalid='ignore'):
                continuation=product-inputs[3];gap=current-inputs[4]
            if not np.isfinite(current).all() or not np.isfinite(continuation).all() or not np.isfinite(gap).all():
                nonfinite_observations+=1
                return product
            index=len(states)
            _require(index<len(values) and index<=len(selections))
            values[index]=current;raw[index]=gap<continuation
            selected_residual=None if index==0 else float(np.max(np.abs(np.where(used[index-1],gap,continuation))))
            states.append(dict(index=index,linear_calls=linear_calls,selected_multiplies=selected_multiplies,
                residual=float(np.max(np.abs(np.minimum(continuation,gap)))),
                selected_system_residual=selected_residual,values_sha256=_digest(current.tobytes())))
            return product
        def linear(*args,**kwargs):
            nonlocal linear_calls
            _require(linear_calls<2*updates)
            linear_calls+=1
            return _delegate_solve(*args,**kwargs)
        primitive._multiply=multiply;primitive.solve_banded=linear
        return primitive

    module.PolicySelector=ObservedSelector;module._primitive=instrument
    try:
        work=module.solve_policy_control(*inputs,tolerance,mode='retain_admissible',max_linear_solves=2*updates)
        status='computed';terminal=work.pop('values')
    except module.LcpRetentionRefused as error:
        if error.diagnostics['reason'] in ('policy_workspace','allocation_unavailable'):
            raise  # Incomplete observation; never invent missing original diagnostics.
        work=dict(error.diagnostics);terminal=work.pop('last_checked_values');status='refused'
    _require(hashes==[_digest(a.tobytes()) for a in inputs])
    values=values[:len(states)];raw=raw[:len(states)];used=used[:len(selections)]
    for a in (values,raw,used):a.flags.writeable=False
    result=dict(status=status,work=work,terminal_values=terminal,values=values,raw_policies=raw,
        used_policies=used,states=states,selections=selections,max_updates=max_updates,
        allocated_state_bytes=required,input_hashes=hashes,source_hashes=dict(SOURCE_HASHES),
        linear_calls=linear_calls,selected_multiplies=selected_multiplies,nonfinite_observations=nonfinite_observations,
        observation_cap_reached=status=='refused' and work['reason']=='linear_budget')
    result['recurrence']=retained_recurrence(values,raw,used,[s['residual'] for s in states])
    verify_retained(inputs,result,tolerance=tolerance)
    return result


def verify_retained(arrays,result,*,tolerance=1e-10):
    """Independently reconstruct actual retention; never call the strict reader."""
    inputs,tolerance=_inputs(arrays,tolerance)
    lower,diagonal,upper,rhs,payoff,initial=inputs;n=len(diagonal)
    try:
        values,raw,used,states,selections,work=(result[k] for k in
            ('values','raw_policies','used_policies','states','selections','work'))
        limit=result['max_updates']
        _require(type(limit) is int and 0<=limit<=MAX_UPDATES)
        updates=min(n+2,limit);budget=2*updates
        _require(values.dtype==np.float64 and values.shape==(len(states),n) and np.isfinite(values).all())
        _require(raw.dtype==np.bool_ and raw.shape==values.shape)
        _require(used.dtype==np.bool_ and used.shape==(len(selections),n))
        _require(np.all(raw.view(np.uint8)<=1) and np.all(used.view(np.uint8)<=1))
        _require(len(states)<=updates+1 and len(used)<=updates)
        _require(type(result['allocated_state_bytes']) is int and result['allocated_state_bytes']==((updates+1)*9+updates)*n)
        _require(result['source_hashes']==SOURCE_HASHES and result['input_hashes']==[_digest(a.tobytes()) for a in inputs])
        for key in ('iterations','policy_solves','correction_solves','linear_solves','residual_checks',
                    'policy_iteration_bound','linear_solve_bound','linear_solve_budget'):
            _require(type(work[key]) is int and work[key]>=0)
        for key in ('linear_calls','selected_multiplies','nonfinite_observations'):
            _require(type(result[key]) is int and result[key]>=0)
        _require(work['mode']==MODE and work['base_mode']=='one_correction')
        _require(type(work['tolerance']) is float and _float_bits(work['tolerance'])==_float_bits(tolerance))
        _require(work['policy_iteration_bound']==n+2 and work['linear_solve_bound']==2*(n+2))
        _require(work['linear_solve_budget']==budget)
        _require(work['linear_solves']==result['linear_calls']==work['policy_solves']+work['correction_solves']<=budget)
        _require(work['correction_solves']<=work['policy_solves']<=work['correction_solves']+1)
        _require(work['iterations']<=work['correction_solves']<=work['iterations']+1)
        _require(work['policy_solves']==len(selections))
        _require(0<=result['nonfinite_observations']<=1)
        _require(work['residual_checks']==len(states)+result['nonfinite_observations']==work['iterations']+1)
        _require(work['correction_solves']<=result['selected_multiplies']<=work['correction_solves']+1)
        with np.errstate(over='ignore',invalid='ignore'):
            initial_c=diagonal*initial
            initial_c[1:]+=lower*initial[:-1];initial_c[:-1]+=upper*initial[1:];initial_c-=rhs
            initial_g=initial-payoff
        _require(bool(states)==bool(np.isfinite(initial_c).all() and np.isfinite(initial_g).all()))
        if states:_require(_bits_equal(values[0],initial))
        expected_work=dict(workspace_required_bytes=32*n,workspace_reserved_bytes=32*n,
            selection_attempts=len(selections),selection_completions=len(selections),initial_policy_rows=n if selections else 0,
            admissible_retained_rows=0,retained_overrides=0,failing_reselected_rows=0,policy_changes=0)
        for index,row in enumerate(states):
            c=diagonal*values[index]
            c[1:]+=lower*values[index,:-1];c[:-1]+=upper*values[index,1:];c-=rhs
            g=values[index]-payoff
            _require(np.isfinite(c).all() and np.isfinite(g).all())
            _require(np.array_equal(raw[index],g<c))
            residual=float(np.max(np.abs(np.minimum(c,g))))
            _require(all(type(row[k]) is int for k in ('index','linear_calls','selected_multiplies')))
            _require(row['index']==index and row['linear_calls']==2*index and row['selected_multiplies']==index)
            _require(row['values_sha256']==_digest(values[index].tobytes()))
            _require(type(row['residual']) is float and _float_bits(row['residual'])==_float_bits(residual))
            if index+1<len(states):_require(residual>tolerance)
            if index:
                selected=float(np.max(np.abs(np.where(used[index-1],g,c))))
                _require(type(row['selected_system_residual']) is float and _float_bits(row['selected_system_residual'])==_float_bits(selected))
            else:_require(row['selected_system_residual'] is None)
            if index<len(selections):
                selection=selections[index]
                _require(all(type(selection[k]) is int for k in ('index','source_state','linear_calls','selected_multiplies')))
                _require(selection['index']==selection['source_state']==index and selection['linear_calls']==2*index and selection['selected_multiplies']==index)
                _require(selection['policy_sha256']==_digest(used[index].tobytes()))
                if index==0:
                    expected=raw[index]
                else:
                    admissible=np.abs(np.minimum(c,g))<=tolerance
                    expected=np.where(admissible,used[index-1],raw[index])
                    retained=int(np.count_nonzero(admissible))
                    expected_work['admissible_retained_rows']+=retained
                    expected_work['failing_reselected_rows']+=n-retained
                    expected_work['retained_overrides']+=int(np.count_nonzero(admissible & (expected!=raw[index])))
                    expected_work['policy_changes']+=int(np.count_nonzero(expected!=used[index-1]))
                _require(np.array_equal(used[index],expected))
        _require(len(selections)<=len(states))
        _require(all(type(value) is int for value in work['policy_work'].values()) and work['policy_work']==expected_work)
        terminal=result['terminal_values']
        _require((terminal is None and not states) or (terminal is not None and states and _bits_equal(terminal,values[-1])))
        completed=work['iterations']
        phase=(work['policy_solves']-completed,work['correction_solves']-completed,
               result['selected_multiplies']-completed,result['nonfinite_observations'])
        if result['status']=='computed':
            _require(phase==(0,0,0,0) and states and states[-1]['residual']<=tolerance)
            _require(type(work['complementarity_residual']) is float)
            _require(_float_bits(work['complementarity_residual'])==_float_bits(states[-1]['residual']))
        else:
            _require(result['status']=='refused' and (not states or states[-1]['residual']>tolerance))
            phases={'linear_budget':((0,0,0,0),),'policy_iteration_limit':((0,0,0,0),),
                'linear_solve_failure':((1,0,0,0),(1,1,1,0)),
                'nonfinite_policy_solves':((1,0,0,0),),'nonfinite_correction_rhs':((1,0,1,0),),
                'nonfinite_correction_solves':((1,1,1,0),),'nonfinite_corrected_values':((1,1,1,0),),
                'nonfinite_original_residual':((0,0,0,1),)}
            reason=work['reason'];_require(reason in phases and phase in phases[reason])
            if reason=='linear_budget':_require(completed==updates<n+2)
            elif reason=='policy_iteration_limit':_require(completed==n+2 and budget==2*(n+2))
            elif reason!='nonfinite_original_residual':_require(completed<updates)
            _require((work['last_checked_residual'] is None and not states) or
                (states and type(work['last_checked_residual']) is float and
                 _float_bits(work['last_checked_residual'])==_float_bits(states[-1]['residual'])))
        expected_cap=result['status']=='refused' and work['reason']=='linear_budget'
        _require(result['observation_cap_reached'] is expected_cap)
        if expected_cap:_require(result['linear_calls']==budget)
        for key in ('first_state_repeat','first_raw_policy_repeat','first_used_policy_repeat','confirmed'):
            entry=result['recurrence'][key]
            if entry is not None:
                _require(all(type(entry[k]) is int for k in ('start','end')))
                if key=='confirmed':_require(type(entry['period']) is int)
        _require(result['recurrence']==retained_recurrence(values,raw,used,[s['residual'] for s in states]))
    except (KeyError,IndexError,TypeError,AttributeError,OverflowError) as error:
        raise ValueError('Contradictory retained-policy evidence') from error
    return True
