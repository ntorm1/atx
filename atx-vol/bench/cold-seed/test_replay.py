import copy
import importlib.util
import math
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("seed_replay", Path(__file__).with_name("replay.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def bits(x):
    return str(struct.unpack("<Q", struct.pack("<d", x))[0])


def fixture():
    request = dict(batch=0, index=0, S=100., K=100., T=1., r=.03, q=0., side=0,
                   target=10., budget=.001, tol=1e-7, sigma=.2, accepted=True,
                   opts_explicit=False, n_collocation=12, n_quadrature=24,
                   n_quad_price=48, max_newton_iter=6, opts_tol=1e-10, profile=0)
    value = dict(price=10., price_bits=bits(10.), boundary=None, **{"class": "european"},
                 patched=False, price_gate=True)
    source = dict(schema="atx.cold-seed.requests.v1", requests=[request])
    native = dict(schema="atx.cold-seed.replay.v1", request_count=1, repetitions=1,
                  attribution=dict(requests=1,models=1,handles=1,nonfinite=0,
                     prices=[dict(price=10.,price_bits=bits(10.))],
                     setup_and_destruction_s=.01,boundary_solve_s=.05,price_evaluation_s=.03,
                     other_s=.01,inclusive_s=.1),
                  routes=m.ROUTES, timings=[dict(rep=0, order=i, route=i, seconds=.1)
                                           for i in range(4)],
                  rows=[dict(request=request, scalar=value, geometry=copy.deepcopy(value),
                             avx2=copy.deepcopy(value), scalar_full_proof=True)])
    return source, native


class ReplayTests(unittest.TestCase):
    def test_complete(self):
        a,b=fixture()
        r=m.summarize(a,b)
        self.assertEqual(r["requests"],1)
        self.assertEqual(r["geometry_bit_mismatches"],0)
        self.assertFalse(r["production_promotion"])

    def test_missing_duplicate_or_changed_request_refused(self):
        for change in (lambda x:x["rows"].clear(),
                       lambda x:x["rows"].append(copy.deepcopy(x["rows"][0])),
                       lambda x:x["rows"][0]["request"].update(index=1)):
            a,b=fixture();b=copy.deepcopy(b);change(b)
            with self.assertRaises(ValueError):m.summarize(a,b)

    def test_lying_bits_or_gate_refused(self):
        for change in (dict(price_bits=bits(9.)),dict(price_gate=False),dict(price=True)):
            a,b=fixture();b=copy.deepcopy(b);b["rows"][0]["avx2"].update(change)
            with self.assertRaises(ValueError):m.summarize(a,b)

    def test_timing_denominator_is_exact(self):
        a,b=fixture();b["timings"][3]=dict(b["timings"][0])
        with self.assertRaises(ValueError):m.summarize(a,b)

    def test_mismatch_not_hidden(self):
        a,b=fixture();b=copy.deepcopy(b)
        b["rows"][0]["geometry"].update(price=math.nextafter(10.,11.),
            price_bits=bits(math.nextafter(10.,11.)))
        b["rows"][0]["scalar_full_proof"]=False
        r=m.summarize(a,b)
        self.assertEqual(r["geometry_bit_mismatches"],1)
        self.assertEqual(r["captured_proof_mismatches"],1)

    def test_nonfinite_lane_stays_in_denominator(self):
        a,b=fixture();b=copy.deepcopy(b)
        b["rows"][0]["avx2"].update(price=None,price_bits=bits(float("nan")),
            price_gate=False,**{"class":"unsupported_regime"})
        r=m.summarize(a,b)
        self.assertEqual(r["requests"],1)
        self.assertEqual(r["avx2_nonfinite"],1)
        self.assertEqual(r["avx2_price_gate_mismatches"],1)

    def test_option_narrowing_is_rejected(self):
        a,b=fixture();a["requests"][0].update(opts_explicit=True,n_collocation=65548)
        with self.assertRaises(ValueError):m.summarize(a,b)


if __name__ == "__main__":unittest.main()
