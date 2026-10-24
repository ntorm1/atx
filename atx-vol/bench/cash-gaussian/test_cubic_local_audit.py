import copy
from decimal import Decimal, localcontext
from fractions import Fraction
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import cubic_local_audit as audit


def model(name):
    values = dict(audit.original.declared())[name]
    return dict(zip(audit.original.FIELDS, values))


class LocalDerivationTests(unittest.TestCase):
    def test_independent_pi_and_small_degree_beta_controls(self):
        self.assertEqual(float(audit.pi_control()), math.pi)
        with localcontext() as context:
            context.prec = 250
            self.assertLess(abs(audit.pi_control()-Decimal(
                '3.14159265358979323846264338327950288419716939937510')), Decimal('1e-50'))
        for degree in (2, 3):
            for j in range(degree+1):
                for power in range(13):
                    # Independent expansion in u with exact beta factorials.
                    expected = sum((Fraction(math.comb(degree, j)*math.comb(power, k)*
                        2**k*(-1)**(power-k)*math.factorial(k+j)*math.factorial(degree-j),
                        math.factorial(k+degree+1)) for k in range(power+1)), Fraction(0))
                    self.assertEqual(audit.integral(degree, j, power), expected)

    def test_adjacent_cell_requires_direct_bounded_series_at_both_precisions(self):
        for digits in (100, 200):
            result = audit.derive(model('adjacent-high'), digits)
            self.assertEqual(result['route'], 'local_series')
            self.assertEqual(result['reason'], 'none')
            self.assertTrue(1 <= result['J'] <= 16 and 1 <= result['N'] <= 16)
            self.assertLessEqual(result['relative_derivative'], Decimal(10)**(20-digits))
            self.assertLessEqual(result['products'], 262144)
            self.assertEqual(result['selection_checks'], 16*(result['J']-1)+result['N'])

    def test_all_original_domain_routes_remain_explicit(self):
        for name, reason in [('zero-lower', 'zero_lower'), ('central', 'wide_cell'),
                             ('small-variance', 'density_variation')]:
            result = audit.derive(model(name), 100)
            self.assertEqual(result['route'], 'original_moments')
            self.assertEqual(result['reason'], reason)
            self.assertEqual(result['products'], 0)
        order_cap = model('central')
        order_cap.update(lower=99.9, upper=100.1, dt=1., sigma=.2, rate=0.)
        order_cap['yield'] = 0.
        result = audit.derive(order_cap, 100)
        self.assertEqual(result['reason'], 'order_cap')
        self.assertEqual(result['selection_checks'], 256)

    def test_value_and_derivative_prefactors_are_distinct(self):
        result = audit.derive(model('positive-near-zero-time'), 100)
        self.assertEqual(result['route'], 'local_series')
        ratio = result['derivative_bound']/result['value_bound']
        self.assertGreater(ratio, Decimal(4)/3)
        self.assertLess(result['RF'], Decimal('1e-80'))
        self.assertEqual(len(result['value_conditions']), 4)
        self.assertEqual(len(result['derivative_conditions']), 3)


def synthetic_protocol_fixture(baseline):
    """Protocol test only: old negative weights plus independently derived metadata.

    This is NOT a new native run or a successful local-series weight receipt.
    Its old three precision failures must remain visible to the new reader.
    """
    native = copy.deepcopy(baseline)
    native.update(schema='cash-cubic-local-weight-control-v1', declaration=audit.DECLARATION,
                  primitive_source=audit.PRIMITIVE, limits=audit.LIMITS,
                  scalar_object_bytes={'100': 80, '200': 112})
    for row in native['rows']:
        derived = audit.derive(row['input'], row['requested_digits'])
        local_route = derived['route'] == 'local_series'
        size = native['scalar_object_bytes'][str(row['requested_digits'])]
        work = dict(moments=dict(row['work']), route=derived['route'], reason=derived['reason'],
                    selection_checks=derived['selection_checks'],
                    coefficient_products=derived['products'], local_functions=derived['local_functions'],
                    workspace_bytes=size*(2312 if local_route else 256)
                        if derived['reason'] != 'zero_lower' else 0,
                    log_order=derived['J'], exponential_order=derived['N'],
                    used_log1p=derived['used_log1p'])
        work['moments']['polynomial_terms'] = 0 if local_route else 28
        row['work'] = work
        row['local'] = {key: ['']*count for key, count in audit.LOCAL_ARRAYS.items()}
        row['local'].update({key: '' for key in audit.LOCAL_SCALARS})
        if local_route:
            for key in ('value_conditions', 'derivative_conditions'):
                row['local'][key] = [str(x) for x in derived[key]]
            row['weights']['maximum_condition'] = str(max(
                derived['value_conditions']+derived['derivative_conditions']))
            row['local'].update(value_remainders=[str(derived['value_bound'])]*4,
                derivative_remainders=[str(derived['derivative_bound'])]*3,
                density_remainder=str(derived['RF']),
                relative_value_remainder=str(derived['relative_value']),
                relative_derivative_remainder=str(derived['relative_derivative']))
    return native


class LocalReceiptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = os.environ.get('ATX_CUBIC_WEIGHT_CONTROL_FIXTURE')
        if not path:
            raise unittest.SkipTest('explicit frozen original24 receipt required')
        cls.baseline_bytes = Path(path).read_bytes()
        cls.baseline = json.loads(cls.baseline_bytes)
        cls.native = synthetic_protocol_fixture(cls.baseline)

    def test_valid_negative_protocol_retains_all_historical_failures(self):
        result = audit.verify(self.native, self.baseline_bytes)
        self.assertEqual(result['precision_outcomes'], 24)
        self.assertEqual(len(result['precision_checks']), 12)
        self.assertEqual(len(result['quadrature_checks']), 168)
        self.assertEqual([x['id'] for x in result['precision_checks'] if not x['passed']],
                         ['adjacent-high', 'adjacent-low', 'adjacent-unit'])
        self.assertFalse(result['controls_passed'])
        self.assertFalse(result['historical']['controls_passed'])
        for field in ('stage_a_complete', 'semigroup_run', 'full_price_error_bound',
                      'timing_qualified'):
            self.assertFalse(result[field])

    def test_fixed_denominator_provenance_and_local_work_mutants(self):
        mutations = [
            lambda n: n['rows'].pop(),
            lambda n: n.update(declaration='unreviewed'),
            lambda n: n.update(primitive_source='unreviewed'),
            lambda n: n.update(semigroup_run=True),
            lambda n: n['limits'].update(max_log_order=17),
            lambda n: n['rows'][4]['input'].update(stock=1.),
            lambda n: n['rows'][4]['input_bits'].update(stock='0'),
            lambda n: n['rows'][4]['work'].update(route='original_moments'),
            lambda n: n['rows'][4]['work'].update(reason='wide_cell'),
            lambda n: n['rows'][4]['work'].update(log_order=1),
            lambda n: n['rows'][4]['work'].update(coefficient_products=0),
            lambda n: n['rows'][4]['work'].update(selection_checks=True),
            lambda n: n['rows'][4]['work'].update(local_functions=0),
            lambda n: n['rows'][4]['work'].update(workspace_bytes=0),
            lambda n: n['rows'][4]['work'].update(used_log1p=False),
            lambda n: n['rows'][4]['local']['value_conditions'].pop(),
            lambda n: n['rows'][4]['local']['value_conditions'].__setitem__(0, '2'),
            lambda n: n['rows'][4]['weights'].update(maximum_condition='2'),
            lambda n: n['rows'][4]['local'].update(density_remainder='1'),
            lambda n: n['rows'][4]['local']['derivative_remainders'].__setitem__(0, '1e-300'),
            lambda n: n['rows'][0]['local'].update(density_remainder='1e-100'),
            lambda n: n['scalar_object_bytes'].update({'100': True}),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                changed = copy.deepcopy(self.native)
                mutate(changed)
                with self.assertRaises(ValueError):
                    audit.verify(changed, self.baseline_bytes)
        with self.assertRaises(ValueError):
            audit.verify(self.native, self.baseline_bytes+b'\n')

    def test_refusal_retains_request_and_completed_work_without_partial_weights(self):
        changed = copy.deepcopy(self.native)
        row = changed['rows'][4]
        row.update(status='refused', weights=None, local=None,
                   error_code=6, error='Unavailable: synthetic allocation refusal')
        result = audit.verify(changed, self.baseline_bytes)
        self.assertEqual(result['precision_outcomes'], 24)
        self.assertEqual(len(result['refused']), 1)
        self.assertFalse(result['controls_passed'])
        row['weights'] = self.native['rows'][4]['weights']
        with self.assertRaises(ValueError):
            audit.verify(changed, self.baseline_bytes)

    def test_common_local_weight_bias_is_independently_rejected(self):
        changed = copy.deepcopy(self.native)
        with localcontext() as context:
            context.prec = 250
            for row in changed['rows']:
                if row['work']['route'] != 'local_series':
                    continue
                weights = row['weights']
                for key in ('value', 'derivative', 'scaled_moments'):
                    weights[key] = [str(Decimal(x)*Decimal('1.0001')) for x in weights[key]]
                for key in ('mass', 'discounted_stock_ratio'):
                    weights[key] = str(Decimal(weights[key])*Decimal('1.0001'))
        # Raw moments must remain the original diagnostic calculation, and
        # independent quadrature would also detect a common scale error.
        with self.assertRaises(ValueError):
            audit.verify(changed, self.baseline_bytes)

    def test_balanced_corrupt_weights_pass_partition_but_fail_quadrature(self):
        changed = copy.deepcopy(self.native)
        with localcontext() as context:
            context.prec = 250
            for row in changed['rows']:
                if row['work']['route'] != 'local_series':
                    continue
                values = [Decimal(x) for x in row['weights']['value']]
                moved = values[0]*Decimal('.0001')
                values[0] += moved
                values[1] -= moved
                row['weights']['value'] = [str(x) for x in values]
        result = audit.verify(changed, self.baseline_bytes)
        self.assertTrue(any(not x['passed'] for x in result['quadrature_checks']))
        self.assertFalse(result['controls_passed'])

    def test_cli_preserves_negative_result_and_refuses_existing_output(self):
        with tempfile.TemporaryDirectory(prefix='cash-local-protocol-test-') as temporary:
            root = Path(temporary)
            baseline, report, output = (root/name for name in ('original.json', 'synthetic.json',
                                                               'verification.json'))
            baseline.write_bytes(self.baseline_bytes)
            report.write_text(json.dumps(self.native), encoding='utf-8')
            command = [sys.executable, str(Path(audit.__file__)), '--baseline', str(baseline),
                       '--report', str(report), '--output', str(output)]
            first = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            data = output.read_bytes()
            result = json.loads(data)
            self.assertFalse(result['controls_passed'])
            self.assertEqual(result['baseline_sha256'], audit.BASELINE_SHA)
            self.assertEqual(result['original_reader_sha256'], audit.ORIGINAL_READER_SHA)
            second = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(output.read_bytes(), data)


if __name__ == '__main__':
    unittest.main()
