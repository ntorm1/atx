"""Bounded files and independent row audit for the offline observation archive.

This module has no solver or saved-stage launcher. Fraction results are exact
for the input binary64 numbers; Decimal100 is a separate numerical diagnostic.
"""
from decimal import Context, Decimal, ROUND_HALF_EVEN, localcontext
from fractions import Fraction
import hashlib
import io
import json
import math
from pathlib import Path
import zipfile

import numpy as np

from lcp_observe import MAX_ROWS, MAX_UPDATES, verify_observation


INPUT_NAMES=('lower','diagonal','upper','rhs','payoff','initial','last_checked_values')
JSON_CAP=1024*1024
STAGE_CAP=24*1024*1024
TOTAL_CAP=128*1024*1024
TOLERANCE=1e-10
OBS_KEYS={'status','work','states','max_updates','allocated_state_bytes','input_hashes',
          'source_hashes','linear_calls','selected_multiplies','nonfinite_observations',
          'observation_cap_reached','recurrence'}
WORK_KEYS={'mode','iterations','policy_solves','correction_solves','linear_solves',
           'residual_checks','policy_iteration_bound','linear_solve_bound','linear_solve_budget','tolerance'}
STATE_KEYS={'index','linear_calls','selected_multiplies','residual','selected_system_residual','values_sha256'}


def require(condition, message='Invalid observation archive'):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def _object(pairs):
    result={}
    for key,value in pairs:
        require(key not in result,'Duplicate JSON key')
        result[key]=value
    return result


def parse_json(data):
    def nonfinite(value):
        raise ValueError('Nonfinite JSON literal: '+value)
    def finite_float(value):
        result=float(value)
        require(math.isfinite(result),'Nonfinite JSON number')
        return result
    try:
        return json.loads(data,object_pairs_hook=_object,parse_constant=nonfinite,parse_float=finite_float)
    except (UnicodeError,json.JSONDecodeError) as error:
        raise ValueError('Invalid JSON') from error


def read_json(path,cap=JSON_CAP):
    path=Path(path)
    require(not path.is_symlink() and path.is_file() and path.stat().st_size<=cap)
    return parse_json(path.read_bytes())


def encoded_json(value):
    return (json.dumps(value,sort_keys=True,indent=2,allow_nan=False)+'\n').encode()


def same_json(left,right):
    # Python equality aliases bool/int and integral float/int. Receipts do not.
    return encoded_json(left)==encoded_json(right)


def write_json(path,value,cap=JSON_CAP):
    data=encoded_json(value)
    require(len(data)<=cap,'JSON storage cap exceeded')
    with Path(path).open('xb') as f:f.write(data)


def file_record(path):
    path=Path(path)
    require(not path.is_symlink() and path.is_file())
    return dict(file=path.name,bytes=path.stat().st_size,sha256=sha(path.read_bytes()))


def checked_file(directory,record,expected_name,cap):
    require(set(record)=={'file','bytes','sha256'})
    require(record['file']==expected_name and Path(expected_name).name==expected_name)
    require(type(record['bytes']) is int and 0<=record['bytes']<=cap)
    path=Path(directory)/expected_name
    require(path.is_file() and not path.is_symlink() and path.stat().st_size==record['bytes'])
    require(file_record(path)==record,'File receipt mismatch')
    return path


def input_specs(n):
    require(type(n) is int and 1<=n<=MAX_ROWS)
    return {name:(np.dtype('float64'),(n-1 if name in ('lower','upper') else n,)) for name in INPUT_NAMES}


def npz_storage(path):
    with zipfile.ZipFile(path) as z:return sum(m.file_size for m in z.infolist())


def load_npz(path,specs,cap=STAGE_CAP):
    """Validate all headers before asking NumPy to allocate any member array."""
    path=Path(path)
    require(not path.is_symlink() and path.is_file() and path.stat().st_size<=cap)
    try:
        with zipfile.ZipFile(path) as z:
            members=z.infolist()
            require(len(members)==len(specs) and {m.filename for m in members}=={n+'.npy' for n in specs})
            require(sum(m.file_size for m in members)<=cap)
            for m in members:
                require(not m.is_dir() and m.flag_bits&1==0 and m.compress_type in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED))
                require(m.file_size<=cap)
                with z.open(m) as f:
                    version=np.lib.format.read_magic(f)
                    require(version in ((1,0),(2,0)))
                    header_reader=(np.lib.format.read_array_header_1_0 if version==(1,0)
                                   else np.lib.format.read_array_header_2_0)
                    shape,fortran,dtype=header_reader(f,max_header_size=4096)
                    expected_dtype,expected_shape=specs[m.filename[:-4]]
                    require(dtype==expected_dtype and shape==expected_shape and not fortran and not dtype.hasobject)
                    require(f.tell()+math.prod(shape)*dtype.itemsize==m.file_size)
        with np.load(path,allow_pickle=False) as z:
            result={name:z[name] for name in specs}
        for name,(dtype,shape) in specs.items():
            a=result[name]
            require(a.dtype==dtype and a.shape==shape and a.flags.c_contiguous)
            require(np.isfinite(a).all())
            if dtype==np.dtype('bool'):require(np.all(a.view(np.uint8)<=1),'Noncanonical policy bits')
        return result
    except (OSError,EOFError,zipfile.BadZipFile,KeyError) as error:
        raise ValueError('Malformed NPZ') from error


class _BoundedFile(io.BufferedWriter):
    def __init__(self,raw,cap):
        super().__init__(raw)
        self.cap=cap

    def write(self,data):
        require(self.tell()+len(data)<=self.cap,'NPZ storage cap exceeded')
        return super().write(data)


def write_npz(path,arrays,cap=STAGE_CAP):
    require(type(cap) is int and cap>=0)
    with _BoundedFile(io.FileIO(path,'xb'),cap) as f:
        # Reserve complete headers/directory before opening ZipFile, so a tiny
        # cap does not leave NumPy's internal ZipFile cleanup on a closed stream.
        needed=22
        for a in arrays.values():
            header=io.BytesIO()
            np.lib.format.write_array_header_1_0(header,np.lib.format.header_data_from_array_1_0(a))
            needed+=len(header.getvalue())+a.nbytes+256
        require(needed<=cap,'NPZ storage cap exceeded')
        with zipfile.ZipFile(f,'w',compression=zipfile.ZIP_STORED) as z:
            for name,a in arrays.items():
                with z.open(name+'.npy','w',force_zip64=True) as member:
                    np.lib.format.write_array(member,a,allow_pickle=False)


def audit_indices(result):
    count=len(result['values'])
    indices=[] if not count else [count-1]
    confirmed=result['recurrence']['confirmed']
    if confirmed is not None:
        require(type(confirmed['end']) is int and 0<=confirmed['end']<count)
        indices.append(confirmed['end'])
    return sorted(set(indices))


def _ratio(value):
    return str(value.numerator)+'/'+str(value.denominator)


def exact_audit(inputs,result,tolerance=TOLERANCE):
    """Check all rows of at most two selected states; never call a linear solver."""
    lower,diagonal,upper,rhs,payoff,_=inputs
    n=len(diagonal)
    indices=audit_indices(result)
    require(1<=n<=MAX_ROWS and len(indices)<=2)
    exact_tol=Fraction.from_float(float(tolerance))
    summaries=[]
    for index in indices:
        values=result['values'][index]
        previous=None if index==0 else result['policies'][index-1]
        exact_hash,decimal_hash=hashlib.sha256(),hashlib.sha256()
        maxima=[Fraction(0),Fraction(0),Fraction(0)]
        dmax=[Decimal(0),Decimal(0),Decimal(0)]
        violations,selected_violations=[],[]
        with localcontext(Context(prec=100,rounding=ROUND_HALF_EVEN,Emin=-999999,Emax=999999)):
            for row in range(n):
                f=lambda a,i:Fraction.from_float(float(a[i]))
                d=lambda a,i:Decimal.from_float(float(a[i]))
                continuation=f(diagonal,row)*f(values,row)
                dec=d(diagonal,row)*d(values,row)
                if row:
                    continuation+=f(lower,row-1)*f(values,row-1)
                    dec+=d(lower,row-1)*d(values,row-1)
                if row+1<n:
                    continuation+=f(upper,row)*f(values,row+1)
                    dec+=d(upper,row)*d(values,row+1)
                continuation-=f(rhs,row);dec-=d(rhs,row)
                gap=f(values,row)-f(payoff,row)
                dgap=d(values,row)-d(payoff,row)
                complement=abs(min(continuation,gap));dc=abs(min(dec,dgap))
                selected=None if previous is None else (gap if previous[row] else continuation)
                ds=None if previous is None else (dgap if previous[row] else dec)
                maxima[0]=max(maxima[0],complement)
                maxima[1]=max(maxima[1],abs(continuation))
                dmax[0]=max(dmax[0],dc);dmax[1]=max(dmax[1],abs(dec))
                if selected is not None:
                    maxima[2]=max(maxima[2],abs(selected));dmax[2]=max(dmax[2],abs(ds))
                    if abs(selected)>exact_tol:selected_violations.append(row)
                if complement>exact_tol:violations.append(row)
                exact_hash.update((f'{row}|{_ratio(continuation)}|{_ratio(gap)}|'+
                    ('none' if selected is None else _ratio(selected))+'\n').encode())
                decimal_hash.update((f'{row}|{dec}|{dgap}|{ds}\n').encode())
        summaries.append(dict(index=index,rows=n,
            exact=dict(complementarity_max=_ratio(maxima[0]),continuation_abs_max=_ratio(maxima[1]),
                       selected_max=None if previous is None else _ratio(maxima[2]),
                       violations=violations,selected_violations=selected_violations,rows_sha256=exact_hash.hexdigest()),
            decimal100=dict(complementarity_max=str(dmax[0]),continuation_abs_max=str(dmax[1]),
                            selected_max=None if previous is None else str(dmax[2]),rows_sha256=decimal_hash.hexdigest())))
    return dict(indices=indices,rows_checked=n*len(indices),tolerance_exact=_ratio(exact_tol),states=summaries)


def _observation_shape(result):
    require(set(result)==OBS_KEYS)
    extra={'complementarity_residual'} if result['status']=='computed' else {'reason','last_checked_residual'}
    require(set(result['work'])==WORK_KEYS|extra)
    require(all(set(row)==STATE_KEYS for row in result['states']))
    require(set(result['recurrence'])=={'first_state_repeat','first_policy_repeat','confirmed'})
    for name in ('first_state_repeat','first_policy_repeat'):
        entry=result['recurrence'][name]
        require(entry is None or set(entry)=={'start','end'})
        if entry is not None:require(all(type(entry[k]) is int for k in ('start','end')))
    entry=result['recurrence']['confirmed']
    require(entry is None or set(entry)=={'kind','start','period','end'})
    if entry is not None:require(all(type(entry[k]) is int for k in ('start','period','end')))


def export_stage(directory,identity,inputs,old_terminal,result,*,cap=STAGE_CAP):
    require(identity and all(c.isalnum() or c in '-_' for c in identity))
    verify_observation(inputs,result,tolerance=TOLERANCE)
    observation={k:v for k,v in result.items() if k not in ('values','policies','terminal_values')}
    _observation_shape(observation)
    values=result['values']
    require(sum(a.nbytes for a in inputs)+old_terminal.nbytes+values.nbytes+result['policies'].nbytes<=cap)
    terminal_matches=bool(len(values) and np.array_equal(values[-1].view(np.uint64),old_terminal.view(np.uint64)))
    payload=dict(schema='atx-lcp-stage-observation-v1',id=identity,observation=observation,
                 terminal_present=result['terminal_values'] is not None,
                 original_terminal_bits_equal=terminal_matches,exact=exact_audit(inputs,result))
    encoded=encoded_json(payload)
    require(len(encoded)<=JSON_CAP)
    directory=Path(directory)
    trajectory=directory/(identity+'.npz')
    input_bytes=sum(a.nbytes for a in inputs)+old_terminal.nbytes
    write_npz(trajectory,dict(values=values,policies=result['policies']),cap-input_bytes-len(encoded))
    scalar=directory/(identity+'.json')
    write_json(scalar,payload)
    return dict(id=identity,states=len(values),rows=len(inputs[1]),
                trajectory=file_record(trajectory),json=file_record(scalar))


def read_stage(directory,entry,inputs,old_terminal):
    require(set(entry)=={'id','states','rows','trajectory','json'})
    identity=entry['id'];require(type(identity) is str and identity and all(c.isalnum() or c in '-_' for c in identity))
    n=len(inputs[1]);count=entry['states']
    require(type(count) is int and 0<=count<=MAX_UPDATES+1 and type(entry['rows']) is int and entry['rows']==n)
    trajectory=checked_file(directory,entry['trajectory'],identity+'.npz',STAGE_CAP)
    scalar=checked_file(directory,entry['json'],identity+'.json',JSON_CAP)
    with zipfile.ZipFile(trajectory) as z:
        require(all(m.compress_type==zipfile.ZIP_STORED for m in z.infolist()),'Trajectory must be uncompressed')
    require(npz_storage(trajectory)+scalar.stat().st_size+sum(a.nbytes for a in inputs)+old_terminal.nbytes<=STAGE_CAP)
    arrays=load_npz(trajectory,{'values':(np.dtype('float64'),(count,n)),
                                'policies':(np.dtype('bool'),(count,n))})
    payload=read_json(scalar)
    require(set(payload)=={'schema','id','observation','terminal_present','original_terminal_bits_equal','exact'})
    require(payload['schema']=='atx-lcp-stage-observation-v1' and payload['id']==identity)
    require(type(payload['terminal_present']) is bool and payload['terminal_present']==bool(count))
    observation=payload['observation'];_observation_shape(observation)
    result=dict(observation,**arrays,terminal_values=arrays['values'][-1] if count else None)
    verify_observation(inputs,result,tolerance=TOLERANCE)
    expected_equal=bool(count and np.array_equal(arrays['values'][-1].view(np.uint64),old_terminal.view(np.uint64)))
    require(type(payload['original_terminal_bits_equal']) is bool and payload['original_terminal_bits_equal']==expected_equal)
    require(same_json(payload['exact'],exact_audit(inputs,result)),'Independent exact-row audit mismatch')
    return result
