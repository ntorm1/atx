import copy
import math
import unittest

import followup_audit as audit
import pilot_audit as prior
from test_pilot_audit import fixture as pilot_fixture


def snapshot(model):
    """Synthetic affine-line evidence; no American pricing/reference result."""
    strike = model['strike']
    lo, hi = strike, math.nextafter(strike, math.inf)
    x, y = [0., lo, hi, 2*strike], [strike, 2**-52, -1., 0.]
    numbers = dict(right_slope=0., strike=strike, stock_lo=lo, stock_hi=hi,
                   difference_lo=y[1], difference_hi=0., slope_difference=0., computed_stock=lo)
    intrinsic = max(hi-strike if model['side'] == 'call' else strike-hi, 0.)
    numbers['difference_hi'] = y[2]-intrinsic
    return dict(stock=x, value=y, stock_bits=[str(prior.bits(v)) for v in x],
                value_bits=[str(prior.bits(v)) for v in y], numbers=numbers,
                number_bits={k: str(prior.bits(v)) for k, v in numbers.items()},
                side=model['side'], tail=False)


def fixture():
    reference, baseline = pilot_fixture()
    models = {r['id']: r['input'] for r in baseline['manifest']}
    rows, crossings = [], []
    for name in ('DSPYp', 'DSPYc'):
        control = dict(models[name], exercise='european', events=[])
        for n in (256, 512, 1024):
            grid = prior.expected_grid(control, n, [])
            nodes = len(grid)
            price = audit.black76(control)
            for M in (1, 32, 128):
                dt = control['maturity']/M
                work = dict(created_knots=2+M*nodes, expectations=M*nodes,
                            interval_evaluations=(nodes-1)*(2+(M-1)*nodes),
                            deterministic_evaluations=0, zero_state_evaluations=M,
                            completed_steps=M, grid_nodes=nodes)
                rows.append(dict(id=name, positive_intervals=n, steps=M, dt=dt,
                                 dt_binary64=str(prior.bits(dt)), control_model=control,
                                 status='success', phase='propagate', error='', error_code=0,
                                 price=price, price_binary64=str(prior.bits(price)),
                                 stock_grid=grid, work=work))
    for key in audit.CROSS_KEYS:
        row = next(r for r in baseline['rows']
                   if (r['id'], r['positive_intervals'], r['time_level']) == key)
        row.update(status='refused', price=None, price_binary64=None,
                   error='OutOfRange: rounded exercise crossing', error_code=2)
        nodes = len(row['schedule']['stock_grid'])
        work = row['work']
        work.update(completed_steps=0, created_knots=2+nodes, expectations=nodes,
                    interval_evaluations=2*(nodes-1), zero_state_evaluations=1,
                    exercise_projections=0, rounded_intersections=1)
        crossings.append(dict(id=key[0], positive_intervals=key[1], time_level=key[2],
                              status='refused', price=None, error=row['error'], error_code=2,
                              work=copy.deepcopy(work), capture_failed=False, capture_attempts=1,
                              records=[snapshot(models[key[0]])]))
    report = dict(schema='cash-gaussian-followup-v1', timed=False,
                  declaration_commit=audit.DECLARATION, input_fingerprint='12110935955959749584',
                  full_interval_integral=True, probability_cutoff=None, infinite_tail='linear',
                  limits=audit.LIMITS.copy(), manifest=[dict(id=r['id'], input=r['input'])
                                                       for r in baseline['manifest']],
                  semigroup=rows, crossings=crossings)
    return reference, baseline, report


class FollowupReader(unittest.TestCase):
    def test_all18_and5_and_high_precision_operand_reconstruction(self):
        result = audit.verify(*fixture())
        self.assertEqual(result['european_outcomes'], 18)
        self.assertEqual(result['failed_crossing_attempts'], 5)
        self.assertEqual(result['semigroup_status_counts'], {'success': 18})
        self.assertTrue(result['one_step_black76_checks_passed'])
        self.assertFalse(result['reference_promoted'])
        self.assertFalse(result['timing_qualified'])
        self.assertTrue(all(not row['nearest_is_distinct'] for row in result['crossings']))

    def test_semigroup_refusal_keeps_the_complete_denominator(self):
        reference, baseline, report = fixture()
        row = report['semigroup'][0]
        row.update(status='refused', price=None, price_binary64=None, error_code=6,
                   error='Unavailable: query budget')
        row['work'].update(completed_steps=0, created_knots=2, expectations=2,
                           interval_evaluations=2, zero_state_evaluations=1)
        result = audit.verify(reference, baseline, report)
        self.assertEqual(result['semigroup_status_counts'], {'refused': 1, 'success': 17})
        self.assertFalse(result['one_step_black76_checks_passed'])
        row['work']['interval_evaluations'] = 3
        with self.assertRaises(ValueError):
            audit.verify(reference, baseline, report)

    def test_invalid_denominators_models_work_or_crossing_evidence_refuse(self):
        reference, baseline, report = fixture()
        def set_value(path, value):
            def change(data):
                node = data
                for key in path[:-1]:
                    node = node[key]
                node[path[-1]] = value
            return change
        def wrong_difference(data):
            record = data['crossings'][0]['records'][0]
            record['numbers']['difference_lo'] *= 2
            record['number_bits']['difference_lo'] = str(prior.bits(
                record['numbers']['difference_lo']))
        changes = [lambda x: x['semigroup'].pop(), lambda x: x['crossings'].pop(),
            set_value(['declaration_commit'], 'changed'), set_value(['timed'], True),
            set_value(['probability_cutoff'], 1e-12),
            set_value(['semigroup', 0, 'steps'], 2),
            set_value(['semigroup', 0, 'control_model', 'exercise'], 'american'),
            set_value(['semigroup', 0, 'control_model', 'events'], [dict(tau=.5, cash=1.)]),
            set_value(['semigroup', 0, 'dt_binary64'], '0'),
            set_value(['semigroup', 0, 'stock_grid', 2], 99.),
            set_value(['semigroup', 0, 'work', 'interval_evaluations'], 0),
            set_value(['semigroup', 0, 'price'], math.nan),
            set_value(['crossings', 0, 'capture_failed'], True),
            set_value(['crossings', 0, 'capture_attempts'], 2),
            set_value(['crossings', 0, 'work', 'expectations'], 2),
            set_value(['crossings', 0, 'records', 0, 'tail'], True),
            set_value(['crossings', 0, 'records', 0, 'stock_bits', 1], '0'),
            wrong_difference]
        for change in changes:
            data = copy.deepcopy(report)
            change(data)
            with self.subTest(change=change), self.assertRaises(ValueError):
                audit.verify(reference, baseline, data)


if __name__ == '__main__':
    unittest.main()
