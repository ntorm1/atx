"""Private offline policy-selection experiment; ordinary correction stays pinned.

Retention changes only policy selection. It never admits a price or skips an
original row check. No PDE/production call site or saved-stage launcher lives here.
"""
import ast
import hashlib
import math
from pathlib import Path

import numpy as np

from lcp_observe import MAX_ROWS, SOURCE_HASHES, _inputs, _load_frozen


MODE='one_correction_retained_admissible'
MAX_POLICY_BYTES=32*MAX_ROWS
MAX_LINEAR_SOLVES=256
_ASSIGNMENT='        exercise = gap < continuation\n'


class LcpRetentionRefused(ArithmeticError):
    def __init__(self,diagnostics):
        self.diagnostics=diagnostics
        super().__init__('Retained-policy experiment refused: '+diagnostics['reason'])


class PolicySelector:
    """One invocation owns its preceding actual selection and logical work."""
    def __init__(self,rows,max_policy_bytes=MAX_POLICY_BYTES):
        if type(rows) is not int or not 1<=rows<=MAX_ROWS or type(max_policy_bytes) is not int or \
                not 0<=max_policy_bytes<=MAX_POLICY_BYTES:
            raise ValueError('Invalid policy resource control')
        self.rows=rows
        self.previous=None
        self.work=dict(workspace_required_bytes=32*rows,workspace_reserved_bytes=0,
            selection_attempts=0,selection_completions=0,initial_policy_rows=0,
            admissible_retained_rows=0,retained_overrides=0,failing_reselected_rows=0,policy_changes=0)
        if 32*rows>max_policy_bytes:
            raise LcpRetentionRefused(dict(mode=MODE,base_mode='one_correction',reason='policy_workspace',
                phase='preflight',policy_work=dict(self.work)))
        self.work['workspace_reserved_bytes']=32*rows

    @staticmethod
    def _owned(policy):
        return policy.copy()

    def choose(self,continuation,gap,tolerance):
        if not isinstance(continuation,np.ndarray) or not isinstance(gap,np.ndarray) or \
                continuation.dtype!=np.float64 or gap.dtype!=np.float64 or \
                continuation.shape!=(self.rows,) or gap.shape!=(self.rows,) or \
                not np.isfinite(continuation).all() or not np.isfinite(gap).all() or \
                isinstance(tolerance,(bool,np.bool_)) or not math.isfinite(tolerance) or tolerance<=0:
            raise ValueError('Invalid inspected policy operands')
        self.work['selection_attempts']+=1
        strict=gap<continuation
        initial=self.previous is None
        if initial:
            selected=strict
            retained=overrides=failing=changes=0
        else:
            admitted=np.abs(np.minimum(continuation,gap))<=tolerance
            selected=np.where(admitted,self.previous,strict)
            retained=int(np.count_nonzero(admitted))
            overrides=int(np.count_nonzero(admitted & (selected!=strict)))
            failing=self.rows-retained
            changes=int(np.count_nonzero(selected!=self.previous))
        # Publish ownership/counts only when all selection allocation succeeded.
        owned=self._owned(selected)
        self.previous=owned
        self.work['selection_completions']+=1
        self.work['initial_policy_rows']+=self.rows if initial else 0
        self.work['admissible_retained_rows']+=retained
        self.work['retained_overrides']+=overrides
        self.work['failing_reselected_rows']+=failing
        self.work['policy_changes']+=changes
        return selected


def _primitive(selector=None):
    primitive=_load_frozen()
    if selector is None:return primitive
    path=Path(__file__).with_name('lcp_correction.py')
    source=path.read_bytes().replace(b'\r\n',b'\n')
    if hashlib.sha256(source).hexdigest()!=SOURCE_HASHES['lcp_correction.py']:
        raise ValueError('Frozen correction source mismatch')
    text=source.decode()
    if text.count(_ASSIGNMENT)!=1:
        raise ValueError('Require exactly the reviewed policy assignment')
    changed=text.replace(_ASSIGNMENT,'        exercise = _choose_policy(continuation, gap, tolerance)\n')
    parsed=ast.parse(changed,filename=str(path))
    functions=[node for node in parsed.body if isinstance(node,ast.FunctionDef) and node.name=='_one_correction']
    if len(functions)!=1:raise ValueError('Missing frozen correction function')
    # Recompile only the hash-qualified function, changing one assignment.
    # All its original arithmetic, exceptions and globals stay in this private module.
    primitive._choose_policy=selector.choose
    exec(compile(ast.Module(body=functions,type_ignores=[]),str(path),'exec'),primitive.__dict__)
    return primitive


def _tag(diagnostics,selector):
    return dict(diagnostics,mode=MODE,base_mode='one_correction',policy_work=dict(selector.work))


def solve_policy_control(lower,diagonal,upper,rhs,payoff,initial,tolerance=1e-10,*,
                         mode='unchanged',max_linear_solves=None,max_policy_bytes=MAX_POLICY_BYTES):
    """Default delegates unchanged; only retain_admissible changes the selector.

    The experimental call is bounded to128 complete paired updates. The source
    retains its original n+2 algorithmic bound and original final row admission.
    """
    if mode not in ('unchanged','retain_admissible'):
        raise ValueError('Unknown policy control mode')
    inputs=(lower,diagonal,upper,rhs,payoff,initial)
    if mode=='unchanged':
        return _primitive().solve_obstacle_control(*inputs,tolerance,mode='one_correction',
                                                   max_linear_solves=max_linear_solves)
    if max_linear_solves is None:max_linear_solves=MAX_LINEAR_SOLVES
    if type(max_linear_solves) is not int or not 0<=max_linear_solves<=MAX_LINEAR_SOLVES:
        raise ValueError('Experimental linear-call cap must be within0..256')
    selector=None
    try:
        arrays,tolerance=_inputs(inputs,tolerance)
        selector=PolicySelector(len(arrays[1]),max_policy_bytes)
        primitive=_primitive(selector)
        try:
            answer=primitive.solve_obstacle_control(*arrays,tolerance,mode='one_correction',
                                                     max_linear_solves=max_linear_solves)
        except primitive.LcpCorrectionRefused as error:
            raise LcpRetentionRefused(_tag(error.diagnostics,selector)) from error
        return _tag(answer,selector)
    except MemoryError as error:
        # No unobserved original residual/counter is fabricated on allocation failure.
        raise LcpRetentionRefused(dict(mode=MODE,base_mode='one_correction',reason='allocation_unavailable',
            phase='unavailable',policy_work=None if selector is None else dict(selector.work))) from error
