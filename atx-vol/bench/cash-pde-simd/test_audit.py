import copy
import unittest

import audit


def fixture(grid=400, mode='original'):
    reference = {'cases': []}
    native = {'schema': 'cash-pde-batch-cold-v1', 'mode': mode,
              'input_fingerprint': '12110935955959749584', 'requests': 50,
              'successes': 50, 'refusals': 0, 'timed': True, 'inclusive_seconds': .2,
              'build': {'fast_math': False}, 'ipo': False, 'rows': [],
              'affinity': {'requested_cpu': 6, 'pinned': True, 'is_pcore': True,
                           'current_cpu_at_receipt': 6,
                           'busy': {'smt_sibling_pct': [0.], 'system_pct': 4.}}}
    for i in range(50):
        model = dict(spot=100., strike=100., maturity=1., sigma=.2, rate=.04,
                     continuous_yield=0., side='call', exercise='american', events=[],
                     cash_process='spot-liquidator' if i < 44 else 'escrowed')
        original = copy.deepcopy(model)
        original['yield'] = original.pop('continuous_yield')
        original['cum_and_post_exercise'] = True
        grids = []
        for n in (400, 800, 1600):
            options = dict(space_base=n, time_base=n // 2, space_level=0, time_level=0,
                           width_sd=8., rannacher_steps=2, domain_sigma=0., time_grading=2.)
            grids.append({'price': 5., 'grid': options, 'work': {
                'space_intervals': n, 'implicit_solves': n // 2 + 2,
                'max_policy_iterations': 2, 'worst_policy_calendar_time': .8,
                'max_lcp_violation': 0., 'lcp_rounding_ties': 0}})
        reference['cases'].append({'id': str(i), 'input': original, 'pde_grids': grids,
                                   'same_cash_model': i < 44,
                                   'reference': {'price': 5., 'uncertainty': .00001}})
        result = dict(price=5., delta=.5, gamma=.01, space_intervals=grid,
                      time_steps=grid // 2 + 2, max_policy_iterations=2,
                      max_policy_time=.8, max_lcp_violation=0., lcp_rounding_ties=0)
        result['binary64'] = {k: str(audit.bits(result[k])) for k in audit.FLOAT_FIELDS}
        native['rows'].append({'id': str(i), 'input': model, 'status': 'success',
                               'error': '', 'result': result})
    native['grid'] = grids[(400, 800, 1600).index(grid)]['grid']
    return reference, native


class AuditTests(unittest.TestCase):
    def test_complete_denominators_and_exact_parity(self):
        reference, report = fixture()
        result = audit.verify(reference, report)
        self.assertEqual(result['all50_pass'], 50)
        self.assertEqual(result['spot44_pass'], 44)
        self.assertEqual(result['other_models'], 6)

    def test_missing_duplicate_refused_or_changed_rows_fail(self):
        for change in (lambda x: x['rows'].pop(),
                       lambda x: x['rows'].__setitem__(1, copy.deepcopy(x['rows'][0])),
                       lambda x: x['rows'][0].update(status='refused'),
                       lambda x: x.update(refusals=1),
                       lambda x: x['rows'][0]['input'].update(rate=.041)):
            a, b = fixture(); change(b)
            with self.assertRaises(ValueError): audit.verify(a, b)

    def test_price_bits_policy_bits_and_nonfinite_are_checked(self):
        for change in (lambda r: r.update(price=5.001),
                       lambda r: r['binary64'].update(price=str(audit.bits(5.001))),
                       lambda r: r.update(gamma=float('nan')),
                       lambda r: r.update(time_steps=0),
                       lambda r: r.update(max_policy_time=.7)):
            a, b = fixture(); change(b['rows'][0]['result'])
            with self.assertRaises(ValueError): audit.verify(a, b)

    def test_all_three_modes_and_grids_are_required(self):
        reference, _ = fixture()
        reports = [fixture(g, m)[1] for g in (400, 800, 1600) for m in audit.MODES]
        result = audit.summarize(reference, reports)
        self.assertEqual(result['complete_unique_grid_cases'], 150)
        self.assertFalse(result['all_case_cash_accuracy_qualified'])
        with self.assertRaises(ValueError): audit.summarize(reference, reports[:-1])
        with self.assertRaises(ValueError): audit.summarize(reference, reports + [reports[0]])

    def test_cross_route_greek_drift_is_rejected(self):
        reference, _ = fixture()
        reports = [fixture(g, m)[1] for g in (400, 800, 1600) for m in audit.MODES]
        result = reports[2]['rows'][0]['result']
        result['delta'] = .51
        result['binary64']['delta'] = str(audit.bits(.51))
        with self.assertRaises(ValueError): audit.summarize(reference, reports)

    def test_unknown_busy_or_unpinned_host_is_never_quiet(self):
        _, original = fixture()
        self.assertTrue(audit.quiet(original))
        for mutate in (lambda r: r.update(timed=False),
                       lambda r: r['affinity'].update(pinned=False),
                       lambda r: r['affinity'].pop('busy'),
                       lambda r: r['affinity']['busy'].update(smt_sibling_pct=[]),
                       lambda r: r['affinity']['busy'].update(smt_sibling_pct=[None]),
                       lambda r: r['affinity']['busy'].update(smt_sibling_pct=[5.]),
                       lambda r: r['affinity']['busy'].update(system_pct=10.)):
            report = copy.deepcopy(original); mutate(report)
            self.assertFalse(audit.quiet(report))

    def test_empirical_uncertainty_is_not_silently_passed(self):
        a, b = fixture()
        a['cases'][0]['reference'].update(price=5.001, uncertainty=.0001)
        result = audit.verify(a, b)
        self.assertEqual(result['spot44_uncertain'], 1)
        self.assertEqual(result['spot44_pass'], 43)


if __name__ == '__main__':
    unittest.main()
