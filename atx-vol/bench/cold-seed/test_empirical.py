import copy
import math
import struct
import unittest

import empirical


def fixture():
    r = dict(batch=0,index=0,S=100.,K=100.,T=1.,r=.04,q=.012,target=10.,budget=.001,
             tol=1e-7,sigma=.25,side=1,n_collocation=12,n_quadrature=24,n_quad_price=48,
             max_newton_iter=6,profile=0,opts_tol=1e-10,accepted=True,opts_explicit=False)
    source = dict(schema="atx.cold-seed.requests.v1",requests=[])
    rows = []
    for i in range(4):
        a = dict(r,index=i); source["requests"].append(a)
        rows.append(dict(request=a,sigma=.25,origin=2,retry=0,vector_price=10.,boundary=70.,
            distance=math.log(100./70.)/.25,allowance=1e-11*100.,**{"class":"continuation"},
            scalar_full_proof=True,scalar_floor=1.,scalar=dict(price=10.,
            price_bits=str(struct.unpack("<Q",struct.pack("<d",10.))[0]),boundary=70.,
            **{"class":"continuation"},patched=False,price_gate=True)))
    report = dict(schema="atx.cold-seed.empirical-component.v1",request_count=4,
                  repetitions=1,rows=rows,timings=[dict(rep=0,batch=0,requests=4,
        vector_packs=1,vector_lanes=4,vector_admitted=4,scalar_fallbacks=0,scalar_admitted=0,
        floor_preparations=1,floor_hits=3,vector_ns=100,scalar_ns=0,floor_ns=10,inclusive_ns=120)])
    return source, report


class EmpiricalTests(unittest.TestCase):
    def test_valid_explicit_scope_still_unqualified(self):
        s,r=fixture(); result=empirical.summarize(s,r)
        self.assertTrue(result["component_correctness_passed"])
        self.assertEqual(result["vector_admitted"],4)
        self.assertFalse(result["whole_fit_qualified"])
        self.assertFalse(result["generic_error_bound_certified"])
        self.assertFalse(result["timing_qualified"])

    def test_structural_mutants_fail_closed(self):
        for mutate in (lambda r:r["rows"].pop(),lambda r:r["timings"].clear(),
            lambda r:r["rows"][0].update(retry=9),lambda r:r["rows"][0].update(origin=1),lambda r:r["rows"][0].update(sigma=.251),
            lambda r:r["rows"][0].update(boundary=99.),
            lambda r:r["rows"][0].update(allowance=0.),
            lambda r:r["rows"][0].update(**{"class":"exercise_plateau"}),
            lambda r:r["timings"][0].update(scalar_fallbacks=1),
            lambda r:r["timings"][0].update(vector_lanes=3),
            lambda r:r["rows"][0].update(sigma=True)):
            s,r=fixture();mutate(r)
            with self.assertRaises(ValueError):empirical.summarize(s,r)

    def test_scalar_failures_retained_not_dropped(self):
        for mutate,reason in ((lambda r:r["rows"][0].update(scalar_full_proof=False),
                              "scalar_decision_changed"),
                             (lambda r:r["rows"][0].update(scalar_floor=10.),"cold_floor")):
            s,r=fixture();mutate(r);result=empirical.summarize(s,r)
            self.assertFalse(result["component_correctness_passed"])
            self.assertEqual(result["requests"],4)
            self.assertIn(reason,result["failures"][0]["reasons"])

    def test_empirical_allowance_violation_is_material(self):
        s,r=fixture();row=r["rows"][0];p=10.00001
        row["scalar"].update(price=p,price_bits=str(struct.unpack("<Q",struct.pack("<d",p))[0]))
        result=empirical.summarize(s,r)
        self.assertFalse(result["component_correctness_passed"])
        self.assertIn("empirical_allowance_exceeded",result["failures"][0]["reasons"])


if __name__ == "__main__":unittest.main()
