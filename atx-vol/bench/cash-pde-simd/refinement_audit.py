"""Fixed DSPY axis-study reader; no new accuracy or speed qualification."""
import argparse
import hashlib
import json
import math
from pathlib import Path

from audit import FLOAT_FIELDS, INT_FIELDS, bits, finite, load_reference, require

LEVELS = ((0, 0), (1, 0), (0, 1), (1, 1), (2, 0), (0, 2))
IDS = ('DSPYp', 'DSPYc')
DECLARATION = 'be0551a871315d4ca8c5f1f6d24e9b5f19c40c0f'
GRID = dict(space_base=1600, time_base=800, width_sd=8., rannacher_steps=2,
            domain_sigma=0., time_grading=2.)


def same_input(actual, expected):
    require(set(actual) == set(expected) - {'cum_and_post_exercise'}, 'input fields')
    require(expected['cum_and_post_exercise'] is True, 'exercise convention')
    for key in ('spot', 'strike', 'maturity', 'sigma', 'rate', 'yield'):
        require(bits(actual[key]) == bits(expected[key]), 'model bits')
    for key in ('side', 'cash_process', 'exercise'):
        require(actual[key] == expected[key], 'model convention')
    require(len(actual['events']) == len(expected['events']), 'event denominator')
    for a, b in zip(actual['events'], expected['events'], strict=True):
        require(set(a) == {'tau', 'cash'}, 'event fields')
        require(all(bits(a[k]) == bits(b[k]) for k in ('tau', 'cash')), 'event bits')


def verify(reference, report):
    require(report['schema'] == 'cash-dspy-axis-v1' and report['timed'] is False, 'schema/clock')
    require(report['declaration_commit'] == DECLARATION, 'predeclaration')
    require(report['input_fingerprint'] == '12110935955959749584', 'input fingerprint')
    require(report['grid_base'] == GRID, 'grid options')
    require(len(report['manifest']) == len(reference['cases']) == 50, 'full50 manifest')
    for a, b in zip(report['manifest'], reference['cases'], strict=True):
        require(a['id'] == b['id'] and a['selected'] is (a['id'] in IDS), 'selection/order')
        same_input(a['input'], b['input'])
    expected_keys = [(i, s, t) for i in IDS for s, t in LEVELS]
    require([(r['id'], r['space_level'], r['time_level']) for r in report['rows']]
            == expected_keys, 'all12 selected outcomes/order')
    prior = {r['id']: r for r in reference['cases']}
    results = []
    for row in report['rows']:
        old = prior[row['id']]
        result = row['result']
        require(row['status'] in ('success', 'refused'), 'typed status')
        if row['status'] == 'refused':
            require(result is None and bool(row['error']), 'refusal evidence')
            results.append(dict(id=row['id'], levels=[row['space_level'], row['time_level']],
                                classification='refused', error=row['error']))
            continue
        require(row['error'] == '' and isinstance(result, dict), 'success evidence')
        require(set(result) == set(FLOAT_FIELDS + INT_FIELDS) | {'binary64'}, 'result fields')
        require(set(result['binary64']) == set(FLOAT_FIELDS), 'bit fields')
        for field in FLOAT_FIELDS:
            require(isinstance(result['binary64'][field], str) and
                    int(result['binary64'][field]) == bits(result[field]), 'result bits')
        for field in INT_FIELDS:
            require(type(result[field]) is int and result[field] >= 0, 'result integer')
        require(result['space_intervals'] == 1600 * 2 ** row['space_level'], 'space grid')
        base = old['pde_grids'][2]
        extra_half_steps = 2 * (len(old['input']['events']) + 1)
        expected_steps = ((base['work']['implicit_solves'] - extra_half_steps)
                          * 2 ** row['time_level'] + extra_half_steps)
        require(result['time_steps'] == expected_steps, 'nested event time grid')
        require(set(row['work']) == {'thomas_sweeps', 'fresh_seed_sweeps', 'forward_rows',
                                    'continuation_rows', 'identity_rows', 'continuation_runs'},
                'work fields')
        require(all(type(x) is int and x >= 0 for x in row['work'].values()), 'work counts')
        w = row['work']
        require(w['thomas_sweeps'] >= result['time_steps'] > 0, 'sweep accounting')
        require(w['forward_rows'] == w['thomas_sweeps'] * result['space_intervals'] and
                w['continuation_rows'] + w['identity_rows'] == w['forward_rows'], 'row accounting')
        if row['space_level'] == row['time_level'] == 0:
            require(bits(result['price']) == bits(base['price']), 'baseline price bits')
            require(all(v == base['work_census'][k] for k, v in w.items()), 'baseline census')
            for key, prior_key in [('time_steps', 'implicit_solves'),
                                   ('max_policy_iterations', 'max_policy_iterations'),
                                   ('lcp_rounding_ties', 'lcp_rounding_ties')]:
                require(result[key] == base['work'][prior_key], 'baseline policy')
            for key, prior_key in [('max_policy_time', 'worst_policy_calendar_time'),
                                   ('max_lcp_violation', 'max_lcp_violation')]:
                require(bits(result[key]) == bits(base['work'][prior_key]), 'baseline policy bits')
        error = result['price'] - old['reference']['price']
        u = old['reference']['uncertainty']
        require(finite(u) and u >= 0, 'reference uncertainty')
        classification = ('pass' if abs(error) + u <= .001 else
                          'fail' if abs(error) - u > .001 else 'uncertain')
        results.append(dict(id=row['id'], levels=[row['space_level'], row['time_level']],
                            price=result['price'], error=error, uncertainty=u,
                            classification=classification, **w))
    axes = {}
    for name in IDS:
        values = {(r['space_level'], r['time_level']): r['result']['price']
                  for r in report['rows'] if r['id'] == name and r['status'] == 'success'}
        if len(values) != 6:
            axes[name] = {'status': 'refused member; axis inference unavailable'}
            continue
        base = values[0, 0]
        s1, s2 = values[1, 0] - base, values[2, 0] - values[1, 0]
        t1, t2 = values[0, 1] - base, values[0, 2] - values[0, 1]
        axes[name] = dict(space_increments=[s1, s2], time_increments=[t1, t2],
            observed_space_order=math.log2(s1 / s2) if s1 * s2 > 0 else None,
            observed_time_order=math.log2(t1 / t2) if t1 * t2 > 0 else None,
            mixed_increment=values[1, 1] - values[1, 0] - values[0, 1] + base)
    return dict(full_manifest=50, prior_grid_outcomes_retained=150, selected_outcomes=12,
                all_case_promotion=False, timing_qualified=False, results=results, axes=axes)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'refuse existing output')
    data = args.report.read_bytes()
    result = verify(load_reference(args.reference), json.loads(data))
    result['report_sha256'] = hashlib.sha256(data).hexdigest()
    args.output.write_text(json.dumps(result, indent=2) + '\n')
