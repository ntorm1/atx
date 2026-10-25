"""Independent reader for the fixed Gaussian cash pilot; no reference promotion."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct

REFERENCE_SHA = 'd4b5c2e81c8971cd9d37abd601139217ba8e71161eee38c3ada7c1dba60343cc'
IDS = ('A1p', 'A1c', 'E64p', 'E64c', 'ILp', 'ILc', 'DSPYp', 'DSPYc')
DECLARATION = '5023cdf4faf31ffa3fc78434ad2a216a200d8c87'
PRIMITIVE = '80ae1ca27a56b2739627f1c7638c4529e8e7ee10'
OWNER = '06d83e2fb7d8eeb99dc18894a2ee4501dcc86a38'
OPTIONS = dict(time_base=128, width_sd=8., max_knots=8192, max_created_knots=2**24,
               max_expectations=2**20, max_interval_evaluations=2**28,
               max_events=64, max_steps=4096, probability_cutoff=None,
               infinite_tail='linear', full_interval_integral=True)
WORK = ('created_knots', 'interval_evaluations', 'expectations', 'deterministic_evaluations',
        'zero_state_evaluations', 'planned_steps', 'completed_steps', 'grid_nodes', 'jumps',
        'exercise_projections', 'intersections', 'rounded_intersections',
        'deterministic_rollbacks')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def bits(value):
    require(finite(value), 'finite binary64')
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def close(actual, expected):
    # Allow only cross-language libm/lerp reconstruction roundoff, never raw-input
    # changes or a price-error exception. Raw model/event/result bits stay exact.
    return finite(actual) and abs(actual-expected) <= 32*max(math.ulp(actual), math.ulp(expected))


def normalized(model):
    T = model['maturity']
    raw = [(T if e['tau'] >= T*(1-1e-12) else e['tau'], e['cash'])
           for e in model['events']
           if e['tau'] > 0 and e['tau'] <= T*(1+1e-12) and e['cash'] > 0]
    raw.sort(key=lambda item: item[0])
    events = []
    for time, cash in raw:
        if events and events[-1]['time'] == time:
            events[-1]['cash'] += cash
        else:
            events.append(dict(time=time, cash=cash))
    return events


def expected_steps(model, level, events):
    boundaries = [0.] + [e['time'] for e in events if e['time'] < model['maturity']]
    boundaries.append(model['maturity'])
    cash = {e['time']: e['cash'] for e in events}
    steps = []
    for lo, hi in reversed(list(zip(boundaries[:-1], boundaries[1:], strict=True))):
        count = (max(1, math.ceil(128*(hi-lo)/model['maturity'])) * 2**level
                 if model['side'] == 'put' else 1)
        to = hi
        for j in range(count, 0, -1):
            start = lo if j == 1 else lo+(hi-lo)*((j-1)/count)
            steps.append(dict(from_=start, to=to, cash_at_from=cash.get(lo, 0.) if j == 1 else 0.))
            to = start
    return steps


def expected_grid(model, n, events):
    S, K = model['spot'], model['strike']
    width = 8*model['sigma']*math.sqrt(model['maturity'])
    lo = min(S, K)*math.exp(-width)
    hi = (max(S, K)+sum(e['cash'] for e in events))*math.exp(width)
    logs = math.log(lo), math.log(hi)
    grid = [0., S, K, lo, hi]
    grid.extend(math.exp(logs[0]+(logs[1]-logs[0])*(j/n)) for j in range(1, n))
    return sorted(set(grid))


def same_model(actual, expected):
    keys = {'spot', 'strike', 'maturity', 'sigma', 'rate', 'yield',
            'side', 'cash_process', 'exercise', 'events'}
    require(set(actual) == keys, 'model fields')
    require(expected['cum_and_post_exercise'] is True, 'exercise convention')
    for key in ('spot', 'strike', 'maturity', 'sigma', 'rate', 'yield'):
        require(bits(actual[key]) == bits(expected[key]), 'raw model bits')
    for key in ('side', 'cash_process', 'exercise'):
        require(actual[key] == expected[key], 'raw model convention')
    require(len(actual['events']) == len(expected['events']), 'raw event denominator')
    for a, b in zip(actual['events'], expected['events'], strict=True):
        require(set(a) == {'tau', 'cash'}, 'raw event fields')
        require(all(bits(a[k]) == bits(b[k]) for k in a), 'raw event bits')


def verify_schedule(row, model):
    schedule = row['schedule']
    require(isinstance(schedule, dict) and set(schedule) == {'events', 'steps', 'stock_grid'},
            'schedule fields')
    events = normalized(model)
    require(len(schedule['events']) == len(events), 'normalized event count')
    for actual, expected in zip(schedule['events'], events, strict=True):
        require(set(actual) == {'time', 'cash'}, 'normalized event fields')
        require(all(bits(actual[k]) == bits(expected[k]) for k in actual), 'normalized event bits')
    steps = expected_steps(model, row['time_level'], events)
    require(len(schedule['steps']) == len(steps) <= OPTIONS['max_steps'], 'clock denominator')
    to = model['maturity']
    for actual, expected in zip(schedule['steps'], steps, strict=True):
        require(set(actual) == {'from', 'to', 'cash_at_from'}, 'clock fields')
        require(bits(actual['to']) == bits(to) and actual['to'] > actual['from'], 'clock chain')
        require(close(actual['from'], expected['from_']) and close(actual['to'], expected['to']),
                'independent nested clock')
        require(bits(actual['cash_at_from']) == bits(expected['cash_at_from']), 'clock cash')
        if expected['cash_at_from'] > 0:
            require(bits(actual['from']) == bits(expected['from_']), 'exact event clock')
        to = actual['from']
    require(bits(to) == bits(0.), 'clock endpoint')
    grid = expected_grid(model, row['positive_intervals'], events)
    actual = schedule['stock_grid']
    require(len(actual) == len(grid) <= OPTIONS['max_knots'], 'stock node denominator')
    require(all(close(a, b) for a, b in zip(actual, grid, strict=True)), 'stock grid')
    require(all(a < b for a, b in zip(actual[:-1], actual[1:], strict=True)), 'strict stock grid')
    for x in (0., model['spot'], model['strike']):
        require(any(bits(a) == bits(x) for a in actual), 'exact stock anchors')
    return len(steps), len(grid), len(events)


def verify_work(row, model):
    work = row['work']
    require(set(work) == set(WORK), 'work fields')
    require(all(type(x) is int and x >= 0 for x in work.values()), 'work integer')
    for field in ('created_knots', 'expectations', 'interval_evaluations'):
        require(work[field] <= OPTIONS['max_'+field], 'work resource budget')
    require(work['completed_steps'] <= work['planned_steps'] <= 4096, 'step resource budget')
    if row['phase'] == 'prepare':
        require(row['status'] == 'refused' and row['schedule'] is None and
                all(x == 0 for x in work.values()), 'prepare refusal')
        return
    require(row['phase'] == 'price', 'failure phase')
    steps, nodes, jumps = verify_schedule(row, model)
    require(work['planned_steps'] == steps and work['grid_nodes'] == nodes, 'work schedule join')
    require(work['deterministic_rollbacks'] == work['deterministic_evaluations'] == 0,
            'positive-volatility pilot')
    require(work['expectations'] <= steps*nodes and work['zero_state_evaluations'] <= steps,
            'query count ceiling')
    require(work['zero_state_evaluations'] <= work['expectations'], 'zero query subset')
    gaussian = work['expectations']-work['zero_state_evaluations']
    # Each successful positive query visits every represented interval. A single
    # failed numerical query may stop early; resource refusals occur before it.
    lower_queries = gaussian if row['status'] == 'success' else max(0, gaussian-1)
    require(lower_queries*2 <= work['interval_evaluations'] <= gaussian*OPTIONS['max_knots'],
            'full interval work range')
    require(work['jumps'] <= jumps and work['exercise_projections'] <= steps+jumps,
            'event/exercise work ceiling')
    if row['status'] == 'success':
        require(work['completed_steps'] == steps and work['expectations'] == steps*nodes and
                work['zero_state_evaluations'] == steps, 'complete rollback work')
        require(work['jumps'] == jumps and work['exercise_projections'] == steps+jumps,
                'pre/post exercise work')
        require(work['created_knots'] >= 2+steps*nodes, 'constructed value work')
        require(work['rounded_intersections'] == 0, 'no hidden rounded crossing')


def axis(values):
    if any(value is None for value in values):
        return dict(status='incomplete')
    d1, d2 = values[1]-values[0], values[2]-values[1]
    order = math.log2(d1/d2) if d1*d2 > 0 else None
    return dict(status='complete', prices=values, increments=[d1, d2], observed_order=order)


def verify(reference, report):
    require(report['schema'] == 'cash-gaussian-pilot-v1' and report['timed'] is False,
            'schema and untimed contract')
    require(report['declaration_commit'] == DECLARATION and report['primitive_commit'] == PRIMITIVE
            and report['owner_commit'] == OWNER, 'frozen implementation identity')
    require(report['input_fingerprint'] == '12110935955959749584', 'input fingerprint')
    require(report['options'] == OPTIONS, 'declared options')
    require(report['options']['full_interval_integral'] is True, 'full integral declaration')
    require(len(report['manifest']) == len(reference['cases']) == 50, 'full50 manifest')
    require(len({x['id'] for x in report['manifest']}) == 50, 'unique manifest IDs')
    for actual, expected in zip(report['manifest'], reference['cases'], strict=True):
        require(actual['id'] == expected['id'] and actual['selected'] is (actual['id'] in IDS),
                'manifest ID/order/selection')
        require(actual['status'] == ('requested' if actual['selected'] else 'uncomputed'),
                'uncomputed population')
        same_model(actual['input'], expected['input'])
    prior = {item['id']: item for item in reference['cases']}
    keys = []
    for name in IDS:
        keys.extend((name, n, 0) for n in (256, 512, 1024))
        if prior[name]['input']['side'] == 'put':
            keys.extend((name, 256, level) for level in (1, 2))
    require(len(keys) == 32 and [(r['id'], r['positive_intervals'], r['time_level'])
                                for r in report['rows']] == keys, 'all32 outcomes/order')
    results = []
    for row in report['rows']:
        require(type(row['positive_intervals']) is int and type(row['time_level']) is int,
                'resolution integer')
        require(row['status'] in ('success', 'refused') and type(row['error_code']) is int,
                'typed outcome')
        old = prior[row['id']]
        model = old['input']
        require(model['cash_process'] == 'spot-liquidator' and model['exercise'] == 'american',
                'pilot cash/exercise domain')
        require(model['sigma'] > 0 and model['spot'] > 0 and model['maturity'] > 0,
                'pilot positive state')
        if model['side'] == 'call':
            require(model['rate'] >= 0 and model['yield'] == 0, 'event-only call domain')
        verify_work(row, model)
        result = dict(id=row['id'], positive_intervals=row['positive_intervals'],
                      time_level=row['time_level'], work=row['work'])
        if row['status'] == 'refused':
            require(row['price'] is None and row['price_binary64'] is None and
                    isinstance(row['error'], str) and row['error'] and
                    row['error_code'] in (1, 2, 6),
                    'refusal without partial price')
            result.update(classification='refused', phase=row['phase'], error=row['error'])
        else:
            require(row['error'] == '' and row['error_code'] == 0, 'success status')
            require(isinstance(row['price_binary64'], str) and
                    int(row['price_binary64']) == bits(row['price']), 'price bits')
            intrinsic = max((model['spot']-model['strike']) *
                            (1 if model['side'] == 'call' else -1), 0.)
            require(row['price'] >= intrinsic-1e-10*max(model['strike'], model['spot']),
                    'American intrinsic')
            error = row['price']-old['reference']['price']
            u = old['reference']['uncertainty']
            require(finite(u) and u >= 0, 'reference empirical uncertainty')
            classification = ('pass' if abs(error)+u <= .001 else
                              'fail' if abs(error)-u > .001 else 'uncertain')
            result.update(classification=classification, price=row['price'], error=error,
                          frozen_reference_uncertainty=u)
        results.append(result)
    axes = {}
    for name in IDS:
        values = {(r['positive_intervals'], r['time_level']): r['price']
                  for r in report['rows'] if r['id'] == name}
        axes[name] = dict(space=axis([values[n, 0] for n in (256, 512, 1024)]))
        if prior[name]['input']['side'] == 'put':
            axes[name]['time'] = axis([values[256, t] for t in (0, 1, 2)])
    return dict(full_manifest=50, prior_grid_outcomes_retained=150, selected_cases=8,
                uncomputed_cases=42, requested_outcomes=32, call_outcomes=12, put_outcomes=20,
                classifications=dict(Counter(r['classification'] for r in results)),
                results=results, axes=axes, all_case_promotion=False, reference_promoted=False,
                rigorous_bound=False, timing_qualified=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'refuse existing output')
    reference_bytes = args.reference.read_bytes()
    require(hashlib.sha256(reference_bytes).hexdigest() == REFERENCE_SHA, 'frozen reference hash')
    report_bytes = args.report.read_bytes()
    result = verify(json.loads(reference_bytes), json.loads(report_bytes))
    result['reference_sha256'] = REFERENCE_SHA
    result['report_sha256'] = hashlib.sha256(report_bytes).hexdigest()
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
