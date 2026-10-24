"""Private observational seam; the declared four saved stages are never loaded here.

The frozen solver is loaded, not reimplemented. This file has no CLI, PDE call,
saved-stage loader or permission to execute the declared four-stage experiment.
"""
import hashlib
import math
from pathlib import Path
import struct
import types

import numpy as np
from scipy.linalg import solve_banded as _delegate_solve


MAX_ROWS = 16383
MAX_UPDATES = 128
MAX_STATE_BYTES = 129*MAX_ROWS*9
SOURCE_HASHES = {
    'lcp_correction.py': 'f898a504ec26551a9a2811b73b9b8faa91d0449270b9365f4568fdb40b22b40b',
    'pde_reference.py': '1c26dc39c206b3d0f3c1bd99977bdcb2f7600b03613ab7728f0cdf8876588b90',
}


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def _require(condition):
    if not condition:
        raise ValueError('Contradictory observation evidence')


def _bits_equal(left, right):
    return left.shape == right.shape and np.array_equal(left.view(np.uint64), right.view(np.uint64))


def _float_bits(value):
    return struct.pack('=d', float(value))


def _inputs(arrays, tolerance):
    if len(arrays) != 6 or any(np.iscomplexobj(a) for a in arrays):
        raise ValueError('Require six real input arrays')
    if isinstance(tolerance, (bool, np.bool_)) or not isinstance(tolerance, (int, float, np.number)):
        raise ValueError('Require original finite positive tolerance')
    tolerance = float(tolerance)
    if not math.isfinite(tolerance) or tolerance <= 0:
        raise ValueError('Require original finite positive tolerance')
    inputs = [np.asarray(a, dtype=np.float64) for a in arrays]
    if any(a.ndim != 1 or not np.isfinite(a).all() for a in inputs):
        raise ValueError('Require finite one-dimensional arrays')
    n = len(inputs[1])
    if not 1 <= n <= MAX_ROWS or [len(a) for a in inputs] != [n-1, n, n-1, n, n, n]:
        raise ValueError('Invalid or oversized observation shape')
    return inputs, tolerance


def _load_frozen():
    modules, sources = {}, {}
    for name in SOURCE_HASHES:
        path = Path(__file__).with_name(name)
        source = path.read_bytes().replace(b'\r\n', b'\n')
        if _digest(source) != SOURCE_HASHES[name]:
            raise ValueError('Frozen observation source mismatch: '+name)
        sources[name] = (path, source)
    # Validate the complete source pair before either module executes imports.
    for name, (path, source) in sources.items():
        module = types.ModuleType('_lcp_observed_'+name.removesuffix('.py'))
        module.__file__ = str(path)
        exec(compile(source, str(path), 'exec'), module.__dict__)
        modules[name] = module
    primitive, original = modules['lcp_correction.py'], modules['pde_reference.py']
    primitive._multiply = original._multiply
    primitive.solve_obstacle = original.solve_obstacle
    return primitive


def recurrence(values, policies, residuals):
    """Hashes filter candidates; full bit comparisons establish every equality."""
    if not isinstance(values, np.ndarray) or values.dtype != np.float64 or values.ndim != 2 or \
            not isinstance(policies, np.ndarray) or policies.dtype != np.bool_ or \
            policies.shape != values.shape or len(residuals) != len(values) or \
            len(values) > MAX_UPDATES+1 or not 1 <= values.shape[1] <= MAX_ROWS or \
            not np.isfinite(values).all() or \
            any(isinstance(v, (bool, np.bool_)) or not isinstance(v, (float, np.floating)) or
                not math.isfinite(v) or v < 0 for v in residuals):
        raise ValueError('Invalid finite recurrence evidence')
    keys = [_digest(v.tobytes()+p.tobytes()+_float_bits(r))
            for v, p, r in zip(values, policies, residuals)]
    policy_keys = [_digest(p.tobytes()) for p in policies]
    equality = {}

    def equal(i, j):
        if (i, j) not in equality:
            equality[i, j] = keys[i] == keys[j] and _bits_equal(values[i], values[j]) and \
                np.array_equal(policies[i], policies[j]) and _float_bits(residuals[i]) == _float_bits(residuals[j])
        return equality[i, j]

    first_state, first_policy = None, None
    for end in range(len(values)):
        for start in range(end):
            if first_state is None and equal(start, end):
                first_state = dict(start=start, end=end)
            if first_policy is None and policy_keys[start] == policy_keys[end] and \
                    np.array_equal(policies[start], policies[end]):
                first_policy = dict(start=start, end=end)
    confirmed = None
    for end in range(2, len(values)):
        for period in range(1, end//2+1):
            start = end-2*period
            if all(equal(start+k, start+period+k) for k in range(period+1)):
                confirmed = dict(kind='stagnation' if period == 1 else 'cycle',
                                 start=start, period=period, end=end)
                break
        if confirmed is not None:
            break
    return dict(first_state_repeat=first_state, first_policy_repeat=first_policy, confirmed=confirmed)


def observe_stage(arrays, tolerance=1e-10, *, max_updates=MAX_UPDATES, max_state_bytes=MAX_STATE_BYTES):
    """Observe one supplied small/saved system; never load a declared input itself."""
    inputs, tolerance = _inputs(arrays, tolerance)
    if type(max_updates) is not int or not 0 <= max_updates <= MAX_UPDATES or \
            type(max_state_bytes) is not int or not 0 <= max_state_bytes <= MAX_STATE_BYTES:
        raise ValueError('Invalid observation resource control')
    n = len(inputs[1])
    updates = min(n+2, max_updates)
    required = (updates+1)*n*9
    if required > max_state_bytes:
        raise ValueError('Observation state budget exceeded')
    primitive = _load_frozen()
    original_multiply = primitive._multiply
    inputs = [a.copy() for a in inputs]
    for a in inputs:
        a.flags.writeable = False
    hashes = [_digest(a.tobytes()) for a in inputs]
    values = np.empty((updates+1, n), dtype=np.float64)
    policies = np.empty((updates+1, n), dtype=np.bool_)
    states = []
    linear_calls = selected_multiplies = nonfinite_observations = 0

    def multiply(lower, diagonal, upper, current):
        nonlocal selected_multiplies, nonfinite_observations
        product = original_multiply(lower, diagonal, upper, current)
        if not (lower is inputs[0] and diagonal is inputs[1] and upper is inputs[2]):
            selected_multiplies += 1
            return product
        with np.errstate(over='ignore', invalid='ignore'):
            continuation = product-inputs[3]
            gap = current-inputs[4]
        if not np.isfinite(current).all() or not np.isfinite(continuation).all() or not np.isfinite(gap).all():
            nonfinite_observations += 1
            return product  # The original inspect emits its unchanged typed refusal.
        index = len(states)
        if index >= len(values):
            raise RuntimeError('Observer state cap exceeded')
        values[index] = current
        policies[index] = gap < continuation
        selected_residual = None if index == 0 else float(np.max(np.abs(
            np.where(policies[index-1], gap, continuation))))
        states.append(dict(index=index, linear_calls=linear_calls,
            selected_multiplies=selected_multiplies,
            residual=float(np.max(np.abs(np.minimum(continuation, gap)))),
            selected_system_residual=selected_residual,
            values_sha256=_digest(current.tobytes())))
        return product

    def linear(*args, **kwargs):
        nonlocal linear_calls
        if linear_calls >= 2*updates:
            raise RuntimeError('Observer linear-call cap exceeded')
        linear_calls += 1
        return _delegate_solve(*args, **kwargs)

    primitive._multiply, primitive.solve_banded = multiply, linear
    try:
        answer = primitive.solve_obstacle_control(*inputs, tolerance, mode='one_correction',
                                                  max_linear_solves=2*updates)
        status = 'computed'
        terminal = answer.pop('values')
    except primitive.LcpCorrectionRefused as error:
        answer = dict(error.diagnostics)
        terminal = answer.pop('last_checked_values')
        status = 'refused'
    if hashes != [_digest(a.tobytes()) for a in inputs]:
        raise RuntimeError('Frozen input was modified')
    values, policies = values[:len(states)], policies[:len(states)]
    values.flags.writeable = policies.flags.writeable = False
    result = dict(status=status, work=answer, terminal_values=terminal, values=values, policies=policies,
        states=states, max_updates=max_updates, allocated_state_bytes=required, input_hashes=hashes,
        source_hashes=dict(SOURCE_HASHES), linear_calls=linear_calls,
        selected_multiplies=selected_multiplies, nonfinite_observations=nonfinite_observations,
        observation_cap_reached=status == 'refused' and answer['reason'] == 'linear_budget')
    result['recurrence'] = recurrence(values, policies, [row['residual'] for row in states])
    verify_observation(inputs, result, tolerance=tolerance)
    return result


def verify_observation(arrays, result, *, tolerance=1e-10):
    """Independent original-row reconstruction; does not call either solver."""
    inputs, tolerance = _inputs(arrays, tolerance)
    lower, diagonal, upper, rhs, payoff, initial = inputs
    n = len(diagonal)
    try:
        values, policies, states, work = (result[key] for key in ('values','policies','states','work'))
        limit = result['max_updates']
        _require(type(limit) is int and 0 <= limit <= MAX_UPDATES)
        budget = 2*min(n+2, limit)
        _require(values.dtype == np.float64 and values.shape == (len(states), n))
        _require(policies.dtype == np.bool_ and policies.shape == values.shape)
        _require(len(states) <= budget//2+1 and np.isfinite(values).all())
        _require(result['input_hashes'] == [_digest(a.tobytes()) for a in inputs])
        _require(result['source_hashes'] == SOURCE_HASHES)
        _require(type(result['allocated_state_bytes']) is int)
        _require(result['allocated_state_bytes'] == (budget//2+1)*n*9 <= MAX_STATE_BYTES)
        for key in ('iterations','policy_solves','correction_solves','linear_solves','residual_checks',
                    'policy_iteration_bound','linear_solve_bound','linear_solve_budget'):
            _require(type(work[key]) is int and work[key] >= 0)
        for key in ('linear_calls','selected_multiplies','nonfinite_observations'):
            _require(type(result[key]) is int and result[key] >= 0)
        _require(type(work['tolerance']) is float)
        _require(work['mode'] == 'one_correction' and _float_bits(work['tolerance']) == _float_bits(tolerance))
        _require(work['policy_iteration_bound'] == n+2 and work['linear_solve_bound'] == 2*(n+2))
        _require(work['linear_solve_budget'] == budget)
        _require(work['linear_solves'] == result['linear_calls'] == work['policy_solves']+work['correction_solves'] <= budget)
        _require(work['correction_solves'] <= work['policy_solves'] <= work['correction_solves']+1)
        _require(work['iterations'] <= work['correction_solves'] <= work['iterations']+1)
        _require(0 <= result['nonfinite_observations'] <= 1)
        _require(work['residual_checks'] == len(states)+result['nonfinite_observations'] == work['iterations']+1)
        _require(work['correction_solves'] <= result['selected_multiplies'] <= work['correction_solves']+1)
        if states:
            _require(_bits_equal(values[0], initial))
        for index, row in enumerate(states):
            # Separate expression here: do not trust the observer's row products,
            # selected masks, hashes or exported residual summary.
            continuation = diagonal*values[index]
            continuation[1:] += lower*values[index, :-1]
            continuation[:-1] += upper*values[index, 1:]
            continuation -= rhs
            gap = values[index]-payoff
            _require(np.isfinite(continuation).all() and np.isfinite(gap).all())
            _require(all(type(row[key]) is int for key in ('index','linear_calls','selected_multiplies')))
            _require(row['index'] == index and row['linear_calls'] == 2*index)
            _require(row['selected_multiplies'] == index)
            _require(row['values_sha256'] == _digest(values[index].tobytes()))
            _require(np.array_equal(policies[index], gap < continuation))
            residual = float(np.max(np.abs(np.minimum(continuation, gap))))
            _require(type(row['residual']) is float and _float_bits(row['residual']) == _float_bits(residual))
            if index+1 < len(states):
                _require(residual > tolerance)  # The original solver would otherwise have stopped here.
            if index:
                selected = float(np.max(np.abs(np.where(policies[index-1], gap, continuation))))
                _require(type(row['selected_system_residual']) is float)
                _require(_float_bits(row['selected_system_residual']) == _float_bits(selected))
            else:
                _require(row['selected_system_residual'] is None)
        terminal = result['terminal_values']
        _require((terminal is None and not states) or (terminal is not None and states and _bits_equal(terminal, values[-1])))
        completed = work['iterations']
        # Deltas locate the exact original phase relative to the last completed
        # corrected update. Only inspect can add a nonfinite observation; only
        # correction-RHS evaluation can precede its corresponding solver call.
        phase = (work['policy_solves']-completed, work['correction_solves']-completed,
                 result['selected_multiplies']-completed, result['nonfinite_observations'])
        if result['status'] == 'computed':
            _require(phase == (0, 0, 0, 0))
            _require(states and states[-1]['residual'] <= tolerance)
            _require(_float_bits(work['complementarity_residual']) == _float_bits(states[-1]['residual']))
            _require(work['policy_solves'] == work['correction_solves'] == work['iterations'])
        else:
            _require(result['status'] == 'refused')
            _require(not states or states[-1]['residual'] > tolerance)
            phases = {
                'linear_budget': ((0, 0, 0, 0),),
                'policy_iteration_limit': ((0, 0, 0, 0),),
                'linear_solve_failure': ((1, 0, 0, 0), (1, 1, 1, 0)),
                'nonfinite_policy_solves': ((1, 0, 0, 0),),
                'nonfinite_correction_rhs': ((1, 0, 1, 0),),
                'nonfinite_correction_solves': ((1, 1, 1, 0),),
                'nonfinite_corrected_values': ((1, 1, 1, 0),),
                'nonfinite_original_residual': ((0, 0, 0, 1),),
            }
            reason = work['reason']
            _require(reason in phases and phase in phases[reason])
            if reason == 'linear_budget':
                _require(completed == budget//2 < n+2)
            elif reason == 'policy_iteration_limit':
                _require(completed == n+2 and budget == 2*(n+2))
            elif reason != 'nonfinite_original_residual':
                _require(completed < budget//2)  # A whole pair was reserved before this attempt.
            _require((work['last_checked_residual'] is None and not states) or \
                (states and _float_bits(work['last_checked_residual']) == _float_bits(states[-1]['residual'])))
        expected_cap = result['status'] == 'refused' and work['reason'] == 'linear_budget'
        _require(result['observation_cap_reached'] is expected_cap)
        if expected_cap:
            _require(result['linear_calls'] == budget and work['iterations'] == budget//2)
        _require(result['recurrence'] == recurrence(values, policies, [row['residual'] for row in states]))
    except (KeyError, IndexError, TypeError, AttributeError, OverflowError) as error:
        raise ValueError('Contradictory observation evidence') from error
    return True
