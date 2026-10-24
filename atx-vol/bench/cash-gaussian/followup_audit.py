"""Independent semigroup and Decimal crossing diagnostics; no pricing substitution."""
import argparse
import bisect
from collections import Counter
from decimal import Decimal, localcontext
import hashlib
import json
import math
from pathlib import Path

import pilot_audit as prior

DECLARATION = 'ed4139038bca27b3a8a175193f3b290031ad9bae'
PILOT_SHA = '3fe82b1981e3f892cb58683c48a4f0df95781fc0bf795536809d85dbbbfd1c79'
LIMITS = dict(max_knots=8192, max_created_knots=2**24,
              max_expectations=2**20, max_interval_evaluations=2**28)
CROSS_KEYS = [('A1p', 256, 1), ('A1p', 256, 2),
              ('E64c', 256, 0), ('E64c', 512, 0), ('E64c', 1024, 0)]
SEMIGROUP_WORK = ('created_knots', 'expectations', 'interval_evaluations',
                  'deterministic_evaluations', 'zero_state_evaluations',
                  'completed_steps', 'grid_nodes')
require, bits, finite = prior.require, prior.bits, prior.finite


def black76(model):
    """Independent Python closed form; no ATX or historical fitted price."""
    S, K, T, sigma = (model[k] for k in ('spot', 'strike', 'maturity', 'sigma'))
    F = S*math.exp((model['rate']-model['yield'])*T)
    discount = math.exp(-model['rate']*T)
    sd = sigma*math.sqrt(T)
    d1 = math.log(F/K)/sd+.5*sd
    d2 = d1-sd
    cdf = lambda x: .5*math.erfc(-x/math.sqrt(2))
    if model['side'] == 'call':
        return discount*(F*cdf(d1)-K*cdf(d2))
    return discount*(K*cdf(-d2)-F*cdf(-d1))


def verify_semigroup(row, original):
    model = dict(original, exercise='european', events=[])
    prior.same_model(row['control_model'], model)
    n, M = row['positive_intervals'], row['steps']
    require(type(n) is int and n in (256, 512, 1024) and
            type(M) is int and M in (1, 32, 128), 'declared semigroup resolution')
    require(bits(row['dt']) == bits(model['maturity']/M) and
            isinstance(row['dt_binary64'], str) and int(row['dt_binary64']) == bits(row['dt']),
            'exact constant dt')
    require(row['status'] in ('success', 'refused'), 'semigroup typed outcome')
    work = row['work']
    require(set(work) == set(SEMIGROUP_WORK), 'semigroup work fields')
    require(all(type(x) is int and x >= 0 for x in work.values()), 'semigroup work integers')
    for field in ('created_knots', 'expectations', 'interval_evaluations'):
        require(work[field] <= LIMITS['max_'+field], 'semigroup resource budget')
    require(work['completed_steps'] <= M, 'semigroup step count')
    require(work['deterministic_evaluations'] == 0, 'positive-volatility control')
    if row['phase'] == 'prepare':
        require(row['status'] == 'refused' and not row['stock_grid'] and
                all(x == 0 for x in work.values()), 'preparation refusal')
    else:
        require(row['phase'] in ('payoff', 'propagate'), 'semigroup phase')
        grid = prior.expected_grid(model, n, [])
        require(len(row['stock_grid']) == len(grid) <= LIMITS['max_knots'], 'grid denominator')
        require(all(prior.close(a, b) for a, b in zip(row['stock_grid'], grid, strict=True)),
                'independent control grid')
        require(all(a < b for a, b in zip(row['stock_grid'][:-1], row['stock_grid'][1:],
                                        strict=True)), 'strict control grid')
        for anchor in (0., model['spot'], model['strike']):
            require(any(bits(x) == bits(anchor) for x in row['stock_grid']), 'control anchors')
        if row['phase'] == 'propagate':
            nodes = len(grid)
            require(work['grid_nodes'] == nodes and work['expectations'] <= M*nodes,
                    'query/owned-grid accounting')
            complete = work['completed_steps']
            require(complete*nodes <= work['expectations'] <= min(M, complete+1)*nodes,
                    'partial query accounting')
            partial_queries = work['expectations']-complete*nodes
            require(work['zero_state_evaluations'] == complete+(partial_queries > 0),
                    'zero query accounting')
            completed_intervals = ((nodes-1)*(2+(complete-1)*nodes) if complete else 0)
            positive_partial = max(0, partial_queries-1)
            input_nodes = nodes if complete else 2
            maximum_intervals = completed_intervals+positive_partial*input_nodes
            minimum_intervals = (maximum_intervals-input_nodes
                                 if row['error_code'] == 2 and positive_partial else
                                 maximum_intervals)
            require(minimum_intervals <= work['interval_evaluations'] <= maximum_intervals,
                    'complete/partial interval accounting')
            require(work['created_knots'] == 2+complete*nodes,
                    'complete/partial construction accounting')
            full = dict(created_knots=2+M*nodes, expectations=M*nodes,
                        interval_evaluations=(nodes-1)*(2+(M-1)*nodes),
                        deterministic_evaluations=0, zero_state_evaluations=M,
                        completed_steps=M, grid_nodes=nodes)
            if row['status'] == 'success':
                require(work == full, 'every full Gaussian interval accounted')
        else:
            require(row['status'] == 'refused' and work['expectations'] == 0 and
                    all(x == 0 for x in work.values()), 'payoff refusal')
    base = dict(id=row['id'], positive_intervals=n, steps=M, work=work)
    require(type(row['error_code']) is int, 'typed error code')
    if row['status'] == 'refused':
        require(row['price'] is None and row['price_binary64'] is None and
                row['error_code'] in (1, 2, 6) and bool(row['error']), 'no partial price')
        return dict(base, status='refused', error=row['error'])
    require(row['phase'] == 'propagate' and row['error'] == '' and row['error_code'] == 0,
            'successful full propagation')
    require(isinstance(row['price_binary64'], str) and
            int(row['price_binary64']) == bits(row['price']), 'semigroup price bits')
    reference = black76(model)
    return dict(base, status='success', price=row['price'], black76=reference,
                error=row['price']-reference)


def reconstructed_value(snapshot, stock):
    """Reproduce the captured PWL's rounded slope/subtraction and one fused FMA."""
    x, y = snapshot['stock'], snapshot['value']
    i = bisect.bisect_right(x, stock)-1
    require(i >= 0, 'PWL domain')
    slope = (snapshot['numbers']['right_slope'] if i+1 == len(x) else
             (y[i+1]-y[i])/(x[i+1]-x[i]))
    displacement = stock-x[i]
    with localcontext() as context:
        context.prec = 200
        return float(Decimal.from_float(slope)*Decimal.from_float(displacement)
                     + Decimal.from_float(y[i]))


def crossing_math(numbers, precision):
    with localcontext() as context:
        context.prec = precision
        lo, hi, a, b = (Decimal.from_float(numbers[k])
                         for k in ('stock_lo', 'stock_hi', 'difference_lo', 'difference_hi'))
        crossing = lo+(hi-lo)*a/(a-b)
        require(lo < crossing < hi, 'exact strict line crossing')
        # For the two captured affine lines only: chord(max) minus max(lines)
        # reaches this height at their intersection. This is not a price bound.
        chord_excess = abs(a*b)/(abs(a)+abs(b))
        return crossing, crossing-lo, hi-crossing, chord_excess


def verify_crossing(row, baseline, original):
    require(row['status'] == baseline['status'] == 'refused' and
            row['error'] == baseline['error'] == 'OutOfRange: rounded exercise crossing' and
            row['error_code'] == baseline['error_code'] == 2 and row['price'] is None,
            'unchanged baseline typed refusal')
    require(set(row['work']) == set(prior.WORK) and
            all(type(x) is int and x >= 0 for x in row['work'].values()) and
            row['work'] == baseline['work'], 'all baseline work counters unchanged')
    require(row['capture_failed'] is False and type(row['capture_attempts']) is int and
            row['capture_attempts'] == len(row['records']) == 1, 'complete one-crossing capture')
    s = row['records'][0]
    require(s['tail'] is False and s['side'] == original['side'], 'captured finite branch/side')
    require(len(s['stock']) == len(s['value']) == len(s['stock_bits']) == len(s['value_bits'])
            and 2 <= len(s['stock']) <= 8192, 'PWL denominator')
    for values, encoded in ((s['stock'], s['stock_bits']), (s['value'], s['value_bits'])):
        require(all(isinstance(b, str) and int(b) == bits(a)
                    for a, b in zip(values, encoded, strict=True)), 'PWL value bits')
    require(bits(s['stock'][0]) == bits(0.) and
            all(a < b for a, b in zip(s['stock'][:-1], s['stock'][1:], strict=True)),
            'PWL stock ordering')
    numbers = s['numbers']
    fields = {'right_slope', 'strike', 'stock_lo', 'stock_hi', 'difference_lo',
              'difference_hi', 'slope_difference', 'computed_stock'}
    require(set(numbers) == set(s['number_bits']) == fields, 'crossing operand fields')
    require(all(isinstance(s['number_bits'][k], str) and
                int(s['number_bits'][k]) == bits(v) for k, v in numbers.items()),
            'crossing operand bits')
    require(bits(numbers['strike']) == bits(original['strike']), 'captured strike')
    lo, hi = numbers['stock_lo'], numbers['stock_hi']
    union = sorted(set(s['stock']+[numbers['strike']]))
    index = bisect.bisect_left(union, lo)
    require(index+1 < len(union) and bits(union[index]) == bits(lo) and
            bits(union[index+1]) == bits(hi), 'adjacent pre-max endpoints')
    for stock, field in ((lo, 'difference_lo'), (hi, 'difference_hi')):
        intrinsic = max(stock-numbers['strike'] if s['side'] == 'call'
                        else numbers['strike']-stock, 0.)
        difference = reconstructed_value(s, stock)-intrinsic
        require(bits(difference) == bits(numbers[field]), 'independent captured PWL difference')
    opposite = (numbers['difference_lo'] > 0 > numbers['difference_hi'] or
                numbers['difference_lo'] < 0 < numbers['difference_hi'])
    require(opposite and numbers['slope_difference'] == 0, 'strict finite crossing')
    require(bits(numbers['computed_stock']) in (bits(lo), bits(hi)),
            'original rounded refusal condition')
    low = crossing_math(numbers, 100)
    high = crossing_math(numbers, 200)
    require(all(abs(a-b) <= Decimal('1e-90')*max(Decimal(1), abs(b))
                for a, b in zip(low, high, strict=True)), 'Decimal precision agreement')
    nearest = float(high[0])
    require(bits(nearest) == bits(float(low[0])), 'stable nearest binary64')
    with localcontext() as context:
        context.prec = 100
        ulp_lo = high[1]/Decimal.from_float(math.ulp(lo))
        ulp_hi = high[2]/Decimal.from_float(math.ulp(hi))
    return dict(id=row['id'], positive_intervals=row['positive_intervals'],
                time_level=row['time_level'], original_error=row['error'], work=row['work'],
                decimal100_intersection=str(low[0]), decimal200_intersection=str(high[0]),
                distance_lo=str(high[1]), distance_hi=str(high[2]),
                distance_lo_in_ulp=str(ulp_lo), distance_hi_in_ulp=str(ulp_hi),
                nearest_binary64=nearest, nearest_bits=str(bits(nearest)),
                nearest_is_distinct=lo < nearest < hi,
                original_computed_stock=numbers['computed_stock'],
                captured_line_chord_excess=str(high[3]), full_price_error_bound=False)


def verify(reference, baseline, report):
    prior.verify(reference, baseline)  # The unchanged all-32 denominator remains mandatory.
    require(report['schema'] == 'cash-gaussian-followup-v1' and report['timed'] is False,
            'untimed follow-up schema')
    require(report['declaration_commit'] == DECLARATION and
            report['input_fingerprint'] == '12110935955959749584', 'predeclaration/input identity')
    require(report['full_interval_integral'] is True and
            report['probability_cutoff'] is None and report['infinite_tail'] == 'linear' and
            report['limits'] == LIMITS,
            'full integral and unchanged resource contract')
    require(len(report['manifest']) == len(reference['cases']) == 50, 'all50 raw inputs')
    for actual, expected in zip(report['manifest'], reference['cases'], strict=True):
        require(actual['id'] == expected['id'], 'raw ID/order')
        prior.same_model(actual['input'], expected['input'])
    original = {row['id']: row['input'] for row in reference['cases']}
    keys = [(name, n, count) for name in ('DSPYp', 'DSPYc')
            for n in (256, 512, 1024) for count in (1, 32, 128)]
    require([(r['id'], r['positive_intervals'], r['steps']) for r in report['semigroup']] == keys,
            'all18 semigroup outcomes')
    results = [verify_semigroup(row, original[row['id']]) for row in report['semigroup']]
    actual_cross_keys = [(r['id'], r['positive_intervals'], r['time_level'])
                         for r in report['crossings']]
    require(actual_cross_keys == CROSS_KEYS, 'all5 failed-crossing attempts')
    base = {(r['id'], r['positive_intervals'], r['time_level']): r for r in baseline['rows']}
    crossings = [verify_crossing(row, base[key], original[row['id']])
                 for row, key in zip(report['crossings'], CROSS_KEYS, strict=True)]
    one_step = [r for r in results if r['steps'] == 1]
    return dict(full_manifest=50, prior_grid_outcomes_retained=150,
                prior_pilot_outcomes_retained=32,
                european_outcomes=18, failed_crossing_attempts=5,
                semigroup_status_counts=dict(Counter(r['status'] for r in results)),
                one_step_black76_checks_passed=all(r['status'] == 'success' and
                                                  abs(r['error']) <= 5e-10 for r in one_step),
                semigroup=results, crossings=crossings, reference_promoted=False,
                full_price_error_bound=False, timing_qualified=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('reference', 'baseline', 'report', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'refuse existing output')
    reference_bytes, baseline_bytes = args.reference.read_bytes(), args.baseline.read_bytes()
    require(hashlib.sha256(reference_bytes).hexdigest() == prior.REFERENCE_SHA,
            'frozen original reference hash')
    require(hashlib.sha256(baseline_bytes).hexdigest() == PILOT_SHA, 'frozen original pilot hash')
    report_bytes = args.report.read_bytes()
    result = verify(json.loads(reference_bytes), json.loads(baseline_bytes),
                    json.loads(report_bytes))
    result.update(reference_sha256=prior.REFERENCE_SHA, baseline_sha256=PILOT_SHA,
                  report_sha256=hashlib.sha256(report_bytes).hexdigest())
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
