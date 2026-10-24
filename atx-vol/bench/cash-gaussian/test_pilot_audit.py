import copy
import math
import unittest

import pilot_audit as audit


def fixture():
    """Synthetic receipt controls; never pricing or frozen-matrix accuracy evidence."""
    cases, manifest, rows = [], [], []
    for name in audit.IDS + tuple('uncomputed'+str(i) for i in range(42)):
        model = dict(spot=100., strike=100., maturity=1., sigma=.2, rate=.04, yield_=0.)
        model['yield'] = model.pop('yield_')
        model.update(side='put' if name.endswith('p') else 'call', cash_process='spot-liquidator',
                     exercise='american', events=[])
        selected = name in audit.IDS
        cases.append(dict(id=name, input=dict(model, cum_and_post_exercise=True),
                          reference=dict(price=10., uncertainty=.00001)))
        manifest.append(dict(id=name, selected=selected,
                             status='requested' if selected else 'uncomputed', input=model))
        if not selected:
            continue
        pairs = [(256, 0), (512, 0), (1024, 0)]
        if model['side'] == 'put':
            pairs += [(256, 1), (256, 2)]
        for n, level in pairs:
            grid = audit.expected_grid(model, n, [])
            steps = [dict(**{'from': s['from_']}, to=s['to'], cash_at_from=s['cash_at_from'])
                     for s in audit.expected_steps(model, level, [])]
            count = len(steps)
            work = dict.fromkeys(audit.WORK, 0)
            work.update(planned_steps=count, completed_steps=count, grid_nodes=len(grid),
                        expectations=count*len(grid), zero_state_evaluations=count,
                        interval_evaluations=count*(len(grid)-1)*2,
                        created_knots=2+count*len(grid), exercise_projections=count)
            rows.append(dict(id=name, positive_intervals=n, time_level=level,
                             status='success', phase='price', error='', error_code=0,
                             price=10., price_binary64=str(audit.bits(10.)),
                             schedule=dict(events=[], steps=steps, stock_grid=grid), work=work))
    report = dict(schema='cash-gaussian-pilot-v1', timed=False,
                  declaration_commit=audit.DECLARATION, primitive_commit=audit.PRIMITIVE,
                  owner_commit=audit.OWNER, input_fingerprint='12110935955959749584',
                  options=audit.OPTIONS.copy(), manifest=manifest, rows=rows)
    return dict(cases=cases), report


class PilotReader(unittest.TestCase):
    def test_complete_denominator_and_axes(self):
        reference, report = fixture()
        result = audit.verify(reference, report)
        self.assertEqual(result['requested_outcomes'], 32)
        self.assertEqual(result['uncomputed_cases'], 42)
        self.assertEqual(result['classifications'], {'pass': 32})
        self.assertFalse(result['reference_promoted'])
        self.assertFalse(result['timing_qualified'])

    def test_refusal_retained_and_axis_inference_unavailable(self):
        reference, report = fixture()
        row = report['rows'][0]
        row.update(status='refused', error='Unavailable: query budget', error_code=6,
                   price=None, price_binary64=None)
        row['work'].update(completed_steps=0, created_knots=2, expectations=2,
                           zero_state_evaluations=1, interval_evaluations=2,
                           exercise_projections=0)
        result = audit.verify(reference, report)
        self.assertEqual(result['classifications']['refused'], 1)
        self.assertEqual(result['axes']['A1p']['space']['status'], 'incomplete')

    def test_malformed_receipts_fail_closed(self):
        reference, report = fixture()
        def mutate(path, value):
            def change(data):
                node = data
                for key in path[:-1]:
                    node = node[key]
                node[path[-1]] = value
            return change
        mutations = [lambda x: x['rows'].pop(),
            mutate(['rows', 0, 'id'], 'unknown'), mutate(['options', 'max_knots'], 16384),
            mutate(['options', 'probability_cutoff'], 1e-12),
            mutate(['manifest', 0, 'input', 'spot'], 101.),
            mutate(['manifest', 0, 'selected'], False),
            mutate(['rows', 0, 'price'], math.nan),
            mutate(['rows', 0, 'price_binary64'], '0'),
            mutate(['rows', 0, 'work', 'completed_steps'], 0),
            mutate(['rows', 0, 'work', 'expectations'], True),
            mutate(['rows', 0, 'work', 'interval_evaluations'], 0),
            mutate(['rows', 0, 'schedule', 'stock_grid', 2], 99.),
            mutate(['rows', 0, 'schedule', 'steps', 0, 'from'], .5),
            mutate(['rows', 0, 'error'], 'ignored failure'),
            mutate(['rows', 0, 'error_code'], 9),
            mutate(['timed'], True), mutate(['owner_commit'], 'other')]
        for change in mutations:
            data = copy.deepcopy(report)
            change(data)
            with self.subTest(change=change), self.assertRaises(ValueError):
                audit.verify(reference, data)


if __name__ == '__main__':
    unittest.main()
