import copy
from decimal import Decimal, localcontext
import json
import os
from pathlib import Path
import unittest

import cubic_weights_audit as audit


class CubicWeightAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = os.environ.get('ATX_CUBIC_WEIGHT_CONTROL_FIXTURE')
        if not fixture:
            raise unittest.SkipTest('explicit fixed24 native control receipt required')
        cls.native = json.loads(Path(fixture).read_text())

    def test_all24_native_controls_and_independent_positive_integrals(self):
        result = audit.verify(self.native)
        self.assertTrue(result['controls_passed'])
        self.assertEqual(len(result['precision_checks']), 12)
        self.assertEqual(len(result['quadrature_checks']), 168)
        self.assertFalse(result['stage_a_complete'])
        self.assertFalse(result['semigroup_run'])
        self.assertFalse(result['full_price_error_bound'])

    def test_refused_precision_remains_in_complete_denominator(self):
        native = copy.deepcopy(self.native)
        row = native['rows'][0]
        row.update(status='refused', error_code=2, error='OutOfRange: control refusal', weights=None)
        row['work'] = dict(moment_attempts=0, normal_functions=0, polynomial_terms=0,
                           decimal_digits=100)
        result = audit.verify(native)
        self.assertFalse(result['controls_passed'])
        self.assertEqual(result['precision_outcomes'], 24)
        self.assertEqual(len(result['refused']), 1)

    def test_invalid_input_work_and_representation_mutants_fail_closed(self):
        mutations = [
            lambda n: n['rows'].pop(),
            lambda n: n['rows'][0].update(id='unknown'),
            lambda n: n['rows'][0]['input'].update(stock=101.),
            lambda n: n['rows'][0]['input_bits'].update(stock='0'),
            lambda n: n['rows'][0].update(requested_digits=200),
            lambda n: n['rows'][0]['work'].update(moment_attempts=True),
            lambda n: n['rows'][0]['work'].update(normal_functions=7),
            lambda n: n['rows'][0]['weights']['value'].pop(),
            lambda n: n['rows'][0]['weights']['value'].__setitem__(0, '-1'),
            lambda n: n['rows'][0]['weights'].update(mass='1'),
            lambda n: n['rows'][0]['weights'].update(maximum_condition='1e99'),
            lambda n: n.update(semigroup_run=True),
            lambda n: n.update(full_price_error_bound=True),
            lambda n: n.update(declaration='unfrozen'),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                changed = copy.deepcopy(self.native)
                mutate(changed)
                with self.assertRaises(ValueError):
                    audit.verify(changed)

    def test_common_moment_bias_is_detected_by_independent_quadrature(self):
        changed = copy.deepcopy(self.native)
        with localcontext() as context:
            context.prec = 250
            for row in changed['rows']:
                weights = row['weights']
                for field in ('value', 'derivative', 'scaled_moments'):
                    weights[field] = [str(Decimal(x)*Decimal('1.0001')) for x in weights[field]]
                for field in ('mass', 'discounted_stock_ratio'):
                    weights[field] = str(Decimal(weights[field])*Decimal('1.0001'))
        result = audit.verify(changed)
        self.assertTrue(all(x['passed'] for x in result['precision_checks']))
        self.assertFalse(result['controls_passed'])
        self.assertTrue(any(not x['passed'] for x in result['quadrature_checks']))


if __name__ == '__main__':
    unittest.main()
