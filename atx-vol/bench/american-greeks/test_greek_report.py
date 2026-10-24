import copy
import json
import math
import os
from pathlib import Path
import unittest
from greek_report import (FIELDS, ENGINES, PROFILES, DEFAULT_OPTIONS, derivatives,
                          summarize, validate_report, validate_requested_ids)


def polynomial_stencil(scale=1.):
    x = [2., .4, .7, .03, .01]
    h = [scale * .01] * 5
    offsets = [[0] * 5]
    for axis in range(5):
        for sign in [1, -1]:
            offset = [0] * 5
            offset[axis] = sign
            offsets.append(offset)
    for axis in [1, 2]:
        for a, b in [(1, 1), (1, -1), (-1, 1), (-1, -1)]:
            offset = [0] * 5
            offset[0], offset[axis] = a, b
            offsets.append(offset)
    samples = []
    for offset in offsets:
        S, v, T, r, q = [a + b * c for a, b, c in zip(x, h, offset)]
        p = S*S + 3*S*v + 4*v*v + 5*T + 6*S*T + 7*r + 8*q
        samples.append(dict(offset=offset, price=p, status='finite', intrinsic_band=False))
    return dict(h=h, scale=scale, samples=samples)


class Reconstruction(unittest.TestCase):
    def test_center_is_part_of_stencil_regime_evidence(self):
        stencil = polynomial_stencil()
        for sample in stencil['samples'][1:]:
            sample['intrinsic_band'] = True
        result = derivatives(stencil)
        for field in FIELDS:
            self.assertTrue(result[field]['intrinsic_band_changed'], field)
            self.assertTrue(result[field]['intrinsic_evidence_complete'], field)

    def test_unavailable_center_does_not_claim_complete_regime_evidence(self):
        stencil = polynomial_stencil()
        stencil['samples'][0].update(price=None, status='refused', intrinsic_band=None)
        result = derivatives(stencil)
        self.assertIsNotNone(result['theta']['value'])
        self.assertIsNone(result['gamma']['value'])
        for field in FIELDS:
            self.assertFalse(result[field]['intrinsic_evidence_complete'], field)

    def test_polynomial_signs_units_and_mixed_derivatives(self):
        expected = [9.4, 2., 9.2, -17., 7., 8., 3., 8., -6.]
        for scale in [1., .5, .25]:
            actual = derivatives(polynomial_stencil(scale))
            self.assertEqual(set(actual), set(FIELDS))
            for field, want in zip(FIELDS, expected):
                self.assertAlmostEqual(actual[field]['value'], want, delta=2e-8)

    def test_missing_duplicate_boolean_and_refused_samples(self):
        for mutate in [lambda s: s['samples'].pop(),
                       lambda s: s['samples'].__setitem__(1, s['samples'][2]),
                       lambda s: s['samples'][0].update(price=True),
                       lambda s: s['h'].__setitem__(0, 0.)]:
            bad = polynomial_stencil()
            mutate(bad)
            with self.assertRaises(ValueError):
                derivatives(bad)
        refused = polynomial_stencil()
        refused['samples'][1].update(price=None, status='refused', intrinsic_band=None)
        actual = derivatives(refused)
        self.assertIsNone(actual['delta']['value'])
        self.assertIsNone(actual['gamma']['value'])
        self.assertIsNotNone(actual['vega']['value'])


def receipt_fixture():
    c = dict(id='fixture', group='fixture', side='put', S=2., K=1., T=.7, sigma=.4, r=.03, q=.01)
    base = [min(.001*c['S'], .02*c['S']*c['sigma']*math.sqrt(c['T'])),
            .001*max(c['sigma'], .1), .002*c['T'], .0001, .0001]
    levels = []
    for scale in [1., .5, .25]:
        level = polynomial_stencil(scale)
        level['h'] = [scale*x for x in base]
        levels.append(level)
    refs = {name: dict(options=options, stencils=copy.deepcopy(levels)) for name, options in PROFILES.items()}
    engines, maps = {}, {}
    for name in ENGINES:
        fields = {}
        for field in FIELDS:
            supported = field != 'phi' if name.startswith('cold') else True
            if name == 'sigma_q_family' and field in ['vanna','volga','charm']: supported = False
            fields[field] = dict(requested=True, supported=supported, value=1. if supported else None,
                reason=('ApiValueUnqualified' if name.startswith('cold') else 'Served') if supported else 'NotInApi')
        engines[name] = dict(price=1., price_status='success', fields=fields)
        if name.startswith('sigma'):
            box = dict(anchor=c['K'], sigma_lo=.8*c['sigma'], sigma_hi=1.2*c['sigma'], n_sigma=12)
            if name == 'sigma_q_family': box.update(q_lo=c['q']-.0005,q_hi=c['q']+.0005,n_q=4)
            engines[name]['box'] = box
            maps[name] = copy.deepcopy(levels)
            for level in maps[name]:
                for sample in level['samples']:
                    d = sample['offset']
                    if d[2] or d[3] or (name == 'sigma_family' and d[4]):
                        sample.update(status='NoModelAxis',price=None,intrinsic_band=None)
    corpus = dict(case_count=1,derivative_tolerances=None,convention={},cases=[c])
    report = dict(schema='atx-american-greek-native-v1',input_case_count=1,case_count=1,
                  selected_ids=['fixture'],requested_fields=FIELDS,convention={},derivative_tolerances=None,
                  reference_independent_method=False,continuous_carry_only=True,step_scales=[1.,.5,.25],
                  family_sigma_factors=[.8,1.2],family_q_half_width=.0005,candidate_options=DEFAULT_OPTIONS,
                  cases=[dict(input=c,engines=engines,references=refs,served_maps=maps)])
    return report, corpus


class Receipt(unittest.TestCase):
    def test_v2_cold_availability_uses_actual_masks_and_reasons(self):
        report, corpus = receipt_fixture()
        report['schema'] = 'atx-american-greek-native-v2'
        for name in ['cold_fd', 'cold_al']:
            engine = report['cases'][0]['engines'][name]
            engine.update(native_result_success=True, native_requested_mask=511,
                          native_supported_mask=511, native_reasons=['ComputedEstimate']*9)
            for field, entry in engine['fields'].items():
                if field != 'phi':
                    entry['reason'] = 'ComputedEstimate'
        engine = report['cases'][0]['engines']['cold_al']
        engine['native_supported_mask'] &= ~(1 << 3)
        engine['native_reasons'][3] = 'StencilCrossing'
        engine['fields']['theta'].update(supported=False, value=None, reason='StencilCrossing')
        summary = summarize(report, corpus)
        self.assertEqual(summary['totals']['cold_al']['theta']['requested'], 1)
        self.assertEqual(summary['totals']['cold_al']['theta']['compared'], 0)
        self.assertEqual(summary['totals']['cold_al']['delta']['compared'], 1)
        mutations = [lambda e:(e['native_reasons'].__setitem__(3, 'NotRequested'),
                               e['fields']['theta'].update(reason='NotRequested')),
                     lambda e:e['fields']['phi'].update(supported=True, value=123.),
                     lambda e:e.update(native_supported_mask=True),
                     lambda e:e.update(native_requested_mask=510),
                     lambda e:e.update(
                         native_supported_mask=e['native_supported_mask'] & ~(1 << 8)),
                     lambda e:e['native_reasons'].__setitem__(3, 'ComputedEstimate'),
                     lambda e:e['fields']['theta'].update(value=0.),
                     lambda e:e.pop('native_requested_mask'),
                     lambda e:e.update(native_result_success=False)]
        for mutate in mutations:
            bad = copy.deepcopy(report)
            mutate(bad['cases'][0]['engines']['cold_al'])
            with self.assertRaises((ValueError, KeyError)):
                validate_report(bad, corpus)

    def test_execution_requires_the_actual_requested_ids(self):
        report, corpus = receipt_fixture()
        validate_report(report, corpus)  # a standalone declared subset is legitimate
        for expected in [['fixture', 'omitted'], ['different'], ['fixture', 'fixture'], [], [True]]:
            with self.assertRaises(ValueError):
                validate_requested_ids(report, expected)
        validate_requested_ids(report, ['fixture'])

    def test_price_derived_intrinsic_flags_are_independently_checked(self):
        report, corpus = receipt_fixture()
        report['cases'][0]['references']['refined32']['stencils'][0]['samples'][0]['intrinsic_band']=True
        with self.assertRaises(ValueError):
            validate_report(report, corpus)

    def test_intrinsic_evidence_uses_bumped_spot_and_both_sides(self):
        for side in ['put', 'call']:
            report, corpus = receipt_fixture()
            row = report['cases'][0]
            row['input'].update(K=2., side=side)
            for engine in ['sigma_family', 'sigma_q_family']:
                row['engines'][engine]['box']['anchor'] = 2.
            all_levels = [r['stencils'] for r in row['references'].values()] + list(row['served_maps'].values())
            for levels in all_levels:
                for level in levels:
                    for sample in level['samples']:
                        if sample['status'] != 'finite':
                            continue
                        spot = 2. + sample['offset'][0]*level['h'][0]
                        intrinsic = max(2.-spot if side == 'put' else spot-2., 0.)
                        sample.update(price=intrinsic, intrinsic_band=intrinsic > 0)
            validate_report(report, corpus)
            index = 2 if side == 'put' else 1
            row['references']['refined32']['stencils'][0]['samples'][index]['intrinsic_band'] = False
            with self.assertRaises(ValueError):
                validate_report(report, corpus)

    def test_denominators_and_no_qualification(self):
        report, corpus = receipt_fixture()
        summary = summarize(report, corpus)
        self.assertFalse(summary['qualified'])
        for fields in summary['totals'].values():
            for v in fields.values():
                self.assertEqual(v['requested'],1)
                self.assertEqual(v['supported']+v['refused_or_unsupported'],1)
        self.assertEqual(summary['totals']['cold_al']['phi']['supported'],0)

    def test_adversarial_receipts_fail_closed(self):
        changes = [lambda r:r['cases'].clear(), lambda r:r.update(case_count=True),
            lambda r:r['selected_ids'].append('fixture'),
            lambda r:r['cases'][0]['engines'].pop('cold_fd'),
            lambda r:r['cases'][0]['engines']['cold_fd']['fields'].pop('phi'),
            lambda r:r['cases'][0]['engines']['cold_fd']['fields']['delta'].update(value=True),
            lambda r:r['cases'][0]['engines']['sigma_family']['fields']['theta'].update(reason='NearExerciseEdge'),
            lambda r:r['cases'][0]['references']['refined32']['stencils'].pop(),
            lambda r:r['cases'][0]['served_maps']['sigma_family'][0]['samples'][5].update(status='finite',price=1.,intrinsic_band=False),
            lambda r:r['cases'][0]['input'].update(q=False), lambda r:r.update(derivative_tolerances=.001),
            lambda r:r['candidate_options'].update(n_quad_price=24)]
        for change in changes:
            report, corpus = receipt_fixture()
            report = copy.deepcopy(report)
            change(report)
            with self.assertRaises((ValueError,KeyError)):
                validate_report(report,corpus)

    def test_native_european_stencils_match_independent_black_formula(self):
        native = os.environ.get('ATX_GREEK_NATIVE_REPORT')
        if not native: self.skipTest('native pilot report required')
        report = json.loads(Path(native).read_text())
        checked = 0
        for row in report['cases']:
            c = row['input']
            if c['id'] not in ['negative_rate_european-put-03','g031-call-03']: continue
            for reference in row['references'].values():
                for stencil in reference['stencils']:
                    for sample in stencil['samples']:
                        d,h=sample['offset'],stencil['h']
                        S,v,T,r,q=[c[k]+d[i]*h[i] for i,k in enumerate(['S','sigma','T','r','q'])]
                        z=v*math.sqrt(T)
                        d1=(math.log(S/c['K'])+(r-q+.5*v*v)*T)/z
                        d2=d1-z
                        N=lambda x:.5*math.erfc(-x/math.sqrt(2))
                        call=S*math.exp(-q*T)*N(d1)-c['K']*math.exp(-r*T)*N(d2)
                        want=call if c['side']=='call' else call-S*math.exp(-q*T)+c['K']*math.exp(-r*T)
                        self.assertEqual(sample['status'],'finite')
                        self.assertAlmostEqual(sample['price'],want,delta=5e-12*max(S,c['K']))
                        checked+=1
        self.assertEqual(checked,2*2*3*19)


if __name__ == '__main__':
    unittest.main()
