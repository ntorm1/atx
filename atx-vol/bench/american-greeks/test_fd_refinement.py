import copy
import math
import json
import os
from pathlib import Path
import unittest

import fd_refinement as m


def fixture():
    c=dict(id='fixture',S=100.,K=100.,T=.5,sigma=.2,r=.03,q=.01,side='put')
    entries=[]
    for profile,design,scale in m.identities():
        h=m.steps(c,design,scale); samples=[]
        for d in m.OFFSETS:
            model=[c['S']+d[0]*h[0],c['K'],c['T']+d[3]*h[3],
                   c['sigma']+d[1]*h[1],c['r']+d[2]*h[2],c['q']]
            s,k,t,v,r,q=model
            samples.append(dict(model=model,price=10+.01*s*s+2*v*v+3*t*t+4*r+5*s*v+6*s*t,
                                internal_boundary=50.,boundary_required=True,exercised=False,evaluated=True))
        fields=m.reconstruct(samples,h,False,False)
        entries.append(dict(profile=profile,design=design,scale=scale,options=copy.deepcopy(m.PROFILES[profile]),
            requested_steps=h[:],actual_steps=h[:],rho_forward=False,time_forward=False,
            result_success=True,requested=511,supported=511,reasons=[1]*9,
            values=[fields[f]['value'] for f in m.RISK_FIELDS]+[samples[0]['price']],samples=samples,wall_s=.001,
            work=dict(sample_attempts=17,boundary_cold_solves=0,boundary_warm_solves=0,
                      boundary_price_calls=0,public_price_calls=0,european_price_calls=17,degenerate_samples=0)))
    corpus=dict(case_count=1,convention={'theta':'calendar'},cases=[c])
    report=dict(schema='atx-american-fd-refinement-v1',case_count=1,input_case_count=1,
        selected_ids=['fixture'],cases=[dict(input=c,evaluations=entries)],convention=corpus['convention'],
        derivative_tolerances=None,qualified=False,latency_qualified=False,warm_start=False,
        continuous_carry_only=True,reference_independent_method=False)
    return report,corpus


class RefinementTest(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('ATX_FD_NATIVE_REPORT'), 'set ATX_FD_NATIVE_REPORT for native receipt')
    def test_native_receipt_and_independent_european_sample_prices(self):
        report=json.loads(Path(os.environ['ATX_FD_NATIVE_REPORT']).read_text())
        corpus=json.loads((m.HERE/'corpus-v1.json').read_text())
        m.summarize(report,corpus)
        count=0
        for row in report['cases']:
            if row['input']['id']!='negative_rate_european-put-03': continue
            for e in row['evaluations']:
                for sample in e['samples']:
                    S,K,T,v,r,q=sample['model']
                    root=math.sqrt(T); d1=(math.log(S/K)+(r-q+.5*v*v)*T)/(v*root); d2=d1-v*root
                    n=lambda x: .5*math.erfc(-x/math.sqrt(2.))
                    price=K*math.exp(-r*T)*n(-d2)-S*math.exp(-q*T)*n(-d1)
                    self.assertAlmostEqual(sample['price'],price,delta=2e-11*max(1.,abs(price)))
                    count+=1
        self.assertEqual(count,48*17)

    def test_independent_european_atm_zero_carry(self):
        c=dict(S=100.,K=100.,T=1.,sigma=.2,r=0.,q=0.,side='call')
        g=m.european_greeks(c)
        density=math.exp(-.005)/math.sqrt(2*math.pi)
        self.assertAlmostEqual(g['gamma'],density/20.,places=15)
        self.assertAlmostEqual(g['theta'],-10*density,places=14)
        c['side']='put'; p=m.european_greeks(c)
        self.assertEqual(g['theta'],p['theta'])
        self.assertEqual(g['gamma'],p['gamma'])
        self.assertAlmostEqual(g['delta']-p['delta'],1.)

    def test_polynomial_central_and_forward_signs(self):
        for forward in [False, True]:
            samples = []
            h = [.1, .001, .0001, .002]
            for d in m.OFFSETS:
                x = [100+d[0]*h[0], .2+d[1]*h[1], .03+d[2]*h[2], .5+d[3]*h[3]]
                s, v, r, t = x
                samples.append(dict(price=10+.01*s*s+2*v*v+3*t*t+4*r+5*s*v+6*s*t))
            if forward:
                samples[6] = samples[0]
                samples[8] = samples[0]
                samples[15], samples[16] = samples[1], samples[2]
            result = m.reconstruct(samples, h, forward, forward)
            expected = [6., .02, 500.8, -(603.+(.006 if forward else 0.)), 4., 5., 4., -6.]
            for name, value in zip(m.RISK_FIELDS, expected):
                self.assertAlmostEqual(result[name]['value'], value, delta=2e-6)

    def test_cancel_indicator_and_missing_operands(self):
        samples = [dict(price=100.) for _ in range(17)]
        result = m.reconstruct(samples, [.1,.001,.0001,.001], False, False)
        self.assertEqual(result['gamma']['value'], 0.)
        self.assertGreater(result['gamma']['roundoff_indicator'], 0.)
        samples[7]['price'] = None
        result = m.reconstruct(samples, [.1,.001,.0001,.001], False, False)
        self.assertIsNone(result['theta']['value'])
        self.assertIsNotNone(result['gamma']['value'])

    def test_exercise_requires_actual_boundary_or_clamp(self):
        sample = dict(model=[99.,100.,.5,.2,.03,.01], price=1.00000001,
                      internal_boundary=98., boundary_required=True, exercised=False, evaluated=True)
        self.assertEqual(m.sample_regime(sample, 'put'), 'Continuation')
        sample['internal_boundary'] = 99.
        sample['exercised'] = True
        self.assertEqual(m.sample_regime(sample, 'put'), 'Exercised')
        sample['exercised'] = False
        with self.assertRaises(ValueError): m.sample_regime(sample, 'put')
        sample['internal_boundary'] = None
        self.assertEqual(m.sample_regime(sample, 'put'), 'Nonfinite')

    def test_fixture_metadata_and_identity_adversaries(self):
        report, corpus = fixture()
        result = m.summarize(report, corpus)
        self.assertEqual(result['case_count'], 1)
        self.assertEqual(result['field_requests'], 48*9)
        self.assertFalse(result['qualified'])
        for mutate in [
            lambda x: x['cases'][0]['evaluations'].pop(),
            lambda x: x['cases'][0]['evaluations'][0]['samples'][1]['model'].__setitem__(0, 101.),
            lambda x: x['cases'][0]['evaluations'][0]['samples'][0].__setitem__('exercised', True),
            lambda x: x['cases'][0]['evaluations'][0].__setitem__('supported', 0),
            lambda x: x['cases'][0]['evaluations'][0].__setitem__('requested', True),
            lambda x: x['cases'][0]['evaluations'][0]['values'].__setitem__(3, 123.),
            lambda x: x['cases'][0]['evaluations'][0]['options'].__setitem__('tol', 1e-3),
            lambda x: x.__setitem__('case_count', 2),
        ]:
            changed = copy.deepcopy(report); mutate(changed)
            with self.assertRaises(ValueError): m.summarize(changed, corpus)

    def test_failed_center_preserves_slots_without_inventing_evaluations(self):
        report,corpus=fixture()
        entry=report['cases'][0]['evaluations'][0]
        entry.update(result_success=False,requested=0,supported=0,reasons=[0]*9,
                     values=[None]*9,error={'code':1,'message':'refused'})
        for i,sample in enumerate(entry['samples']):
            sample['price']=None
            sample['evaluated']=i==0
        entry['work'].update(sample_attempts=1,european_price_calls=0,public_price_calls=1)
        result=m.summarize(report,corpus)
        self.assertEqual(result['field_requests'],432)
        entry['samples'][4]['evaluated']=True
        with self.assertRaises(ValueError): m.summarize(report,corpus)

    def test_model_requires_boundary_even_when_flag_and_boundary_are_removed(self):
        report,corpus=fixture()
        e=report['cases'][0]['evaluations'][0]
        for sample in e['samples']:
            sample['boundary_required']=False
            sample['internal_boundary']=None
        with self.assertRaises(ValueError): m.summarize(report,corpus)

    def test_boundary_requirement_uses_internal_call_model_and_zero_rate_yield(self):
        for side,r,q,T,v,required in [
            ('put',.03,.01,.5,.2,True), ('put',0.,-.01,.5,.2,True),
            ('call',-.01,0.,.5,.2,True), ('call',.03,0.,.5,.2,False),
            ('put',-.01,-.02,.5,.2,False), ('put',-.01,.02,.5,.2,False),
            ('put',.03,.01,1e-12,.2,False), ('put',.03,.01,.5,1e-8,False),
        ]:
            sample=dict(model=[100.,100.,T,v,r,q],price=10.,internal_boundary=50. if required else None,
                        boundary_required=required,exercised=False,evaluated=True)
            self.assertEqual(m.sample_regime(sample,side),'Continuation')
            sample['boundary_required']=not required
            with self.assertRaises(ValueError): m.sample_regime(sample,side)


if __name__ == '__main__':
    unittest.main()
