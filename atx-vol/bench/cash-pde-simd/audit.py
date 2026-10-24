"""Independent frozen-input, exact-output and empirical cash frontier audit."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct

REFERENCE_SHA = 'd4b5c2e81c8971cd9d37abd601139217ba8e71161eee38c3ada7c1dba60343cc'
MODES = ('original', 'batch-scalar', 'batch-avx2')
FLOAT_FIELDS = ('price', 'delta', 'gamma', 'max_policy_time', 'max_lcp_violation')
INT_FIELDS = ('space_intervals', 'time_steps', 'max_policy_iterations', 'lcp_rounding_ties')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def bits(value):
    require(finite(value), 'finite binary64 required')
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def load_reference(path):
    data = path.read_bytes()
    require(hashlib.sha256(data).hexdigest() == REFERENCE_SHA, 'reference SHA')
    return json.loads(data)


def verify(reference, report):
    require(report['schema'] == 'cash-pde-batch-cold-v1', 'schema')
    require(report['mode'] in MODES, 'mode')
    require(report['input_fingerprint'] == '12110935955959749584', 'input fingerprint')
    require(type(report['requests']) is int and report['requests'] == 50, 'request denominator')
    require(type(report['successes']) is int and type(report['refusals']) is int
            and report['successes'] == 50 and report['refusals'] == 0, 'refusal denominator')
    rows = report['rows']
    require(len(rows) == len(reference['cases']) == 50, 'row denominator')
    grid = report['grid']['space_base']
    require(type(grid) is int and grid in (400, 800, 1600), 'grid')
    level = (400, 800, 1600).index(grid)
    require(report['grid'] == reference['cases'][0]['pde_grids'][level]['grid'], 'grid options')
    require(type(report['timed']) is bool, 'timed type')
    require((report['timed'] and finite(report['inclusive_seconds'])
             and report['inclusive_seconds'] > 0) or
            (not report['timed'] and report['inclusive_seconds'] is None), 'clock')
    require(report['build']['fast_math'] is False, 'fast math')
    require(type(report['ipo']) is bool, 'IPO provenance')
    model_keys = {'spot', 'strike', 'maturity', 'sigma', 'rate', 'continuous_yield',
                  'side', 'cash_process', 'exercise', 'events'}
    populations = Counter()
    for old, row in zip(reference['cases'], rows, strict=True):
        require(row['id'] == old['id'], 'ID/order join')
        require(row['status'] == 'success' and row['error'] == '', 'status')
        actual, expected = row['input'], old['input']
        require(set(actual) == model_keys, 'model fields')
        for field in ('spot', 'strike', 'maturity', 'sigma', 'rate', 'continuous_yield'):
            key = 'yield' if field == 'continuous_yield' else field
            require(bits(actual[field]) == bits(expected[key]), 'model value')
        for field in ('side', 'cash_process', 'exercise'):
            require(actual[field] == expected[field], 'model convention')
        require(len(actual['events']) == len(expected['events']), 'event count')
        for a, b in zip(actual['events'], expected['events'], strict=True):
            require(set(a) == {'tau', 'cash'}, 'event fields')
            require(all(bits(a[k]) == bits(b[k]) for k in ('tau', 'cash')), 'event bits')
        result = row['result']
        require(set(result) == set(FLOAT_FIELDS + INT_FIELDS) | {'binary64'}, 'result fields')
        require(set(result['binary64']) == set(FLOAT_FIELDS), 'binary64 fields')
        for field in FLOAT_FIELDS:
            require(type(result['binary64'][field]) is str, 'bits encoding')
            require(int(result['binary64'][field]) == bits(result[field]), 'bits/value mismatch')
        for field in INT_FIELDS:
            require(type(result[field]) is int and result[field] >= 0, 'integer result')
        base = old['pde_grids'][level]
        require(bits(result['price']) == bits(base['price']), 'original price bits')
        for field, old_field in [('space_intervals', 'space_intervals'),
                                 ('time_steps', 'implicit_solves'),
                                 ('max_policy_iterations', 'max_policy_iterations'),
                                 ('lcp_rounding_ties', 'lcp_rounding_ties')]:
            require(result[field] == base['work'][old_field], 'original work')
        for field, old_field in [('max_policy_time', 'worst_policy_calendar_time'),
                                 ('max_lcp_violation', 'max_lcp_violation')]:
            require(bits(result[field]) == bits(base['work'][old_field]), 'original policy bits')
        price_error = abs(result['price'] - old['reference']['price'])
        uncertainty = old['reference']['uncertainty']
        require(finite(uncertainty) and uncertainty >= 0, 'reference uncertainty')
        status = ('pass' if price_error + uncertainty <= .001 else
                  'fail' if price_error - uncertainty > .001 else 'uncertain')
        populations['all50_' + status] += 1
        if old['same_cash_model']:
            require(actual['cash_process'] == 'spot-liquidator', 'matching model scope')
            populations['spot44_' + status] += 1
        else:
            populations['other_models'] += 1
    require(sum(populations['all50_' + s] for s in ('pass', 'fail', 'uncertain')) == 50,
            'all-case accuracy denominator')
    require(sum(populations['spot44_' + s] for s in ('pass', 'fail', 'uncertain')) == 44
            and populations['other_models'] == 6, 'model accuracy denominator')
    return {'grid': grid, 'mode': report['mode'], **populations}


def quiet(report):
    """Conservative declared-host gate; missing topology/load never means idle."""
    try:
        a = report['affinity']
        siblings = a['busy']['smt_sibling_pct']
        system = a['busy']['system_pct']
        return (report['timed'] and a['requested_cpu'] == 6 and a['pinned'] is True
                and a['is_pcore'] is True and a['current_cpu_at_receipt'] == 6
                and type(siblings) is list and len(siblings) > 0
                and all(finite(x) and 0 <= x < 5. for x in siblings)
                and finite(system) and 0 <= system < 10.)
    except (KeyError, TypeError):
        return False


def summarize(reference, reports):
    groups = defaultdict(list)
    rows = []
    for report in reports:
        rows.append(verify(reference, report))
        groups[(report['grid']['space_base'], report['mode'])].append(report)
    require(set(groups) == {(g, m) for g in (400, 800, 1600) for m in MODES},
            'all grids and routes required')
    require(len({len(v) for v in groups.values()}) == 1, 'unbalanced route/grid trials')
    for grid in (400, 800, 1600):
        original = groups[(grid, 'original')][0]
        for mode in MODES:
            for report in groups[(grid, mode)]:
                require([r['result'] for r in report['rows']] ==
                        [r['result'] for r in original['rows']], 'cross-route result bits')
    return {'complete_unique_grid_cases': 150, 'reports': len(reports),
            'repetitions_per_route_grid': len(next(iter(groups.values()))),
            'exact_output_parity': True, 'frontier': rows,
            'native_quiet_all': all(quiet(r) for r in reports),
            'timing_qualification': 'Requires launcher process/source/binary/host audit too; '
                                    'this numerical audit alone never qualifies latency.',
            'all_case_cash_accuracy_qualified': False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('reports', type=Path, nargs='+')
    args = parser.parse_args()
    require(not args.output.exists(), 'output already exists')
    reference = load_reference(args.reference)
    result = summarize(reference, [json.loads(p.read_text(encoding='utf-8-sig'))
                                   for p in args.reports])
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
