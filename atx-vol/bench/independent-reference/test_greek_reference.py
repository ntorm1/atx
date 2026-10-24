"""Independent closed forms and refusal controls, not production parity."""
import math
import unittest

import numpy as np

from greek_reference import classify_stencil, greek_reference


def european(model):
    s, k, t, v, r, q = (model[x] for x in ("S", "K", "T", "sigma", "r", "q"))
    z = v * math.sqrt(t)
    d1 = (math.log(s/k) + (r-q+.5*v*v)*t)/z
    d2 = d1-z
    n = lambda x: .5 * math.erfc(-x/math.sqrt(2))
    phi = math.exp(-.5*d1*d1)/math.sqrt(2*math.pi)
    sign = 1 if model["side"] == "call" else -1
    return dict(price=sign*(s*math.exp(-q*t)*n(sign*d1)-k*math.exp(-r*t)*n(sign*d2)),
                delta=sign*math.exp(-q*t)*n(sign*d1),
                gamma=math.exp(-q*t)*phi/(s*z))


class GreekReferenceTests(unittest.TestCase):
    def test_european_both_sides_and_carry_refine_to_closed_form(self):
        for side in ("put", "call"):
            for scheme in ("cn", "dirk_l", "dirk_third"):
                model = dict(S=100., K=100., T=.25, sigma=.2, r=-.01, q=.03, side=side)
                exact = european(model)
                coarse = greek_reference(model, nx=513, nt=128, scheme=scheme, american=False)
                fine = greek_reference(model, nx=2049, nt=512, scheme=scheme, american=False)
                self.assertEqual(fine["regime"], "continuation")
                for field, cap in (("price", .0001), ("delta", .00002), ("gamma", .000003)):
                    error = abs(fine[field]-exact[field])
                    self.assertLess(error, cap, (side, scheme, field, error))
                    self.assertLess(error, abs(coarse[field]-exact[field])*.4+1e-11)
                self.assertFalse(fine["greek_accuracy_established"])

    def test_unaligned_payoff_kink_meets_absolute_closed_form_controls(self):
        # Sampling a kink at different fractional grid positions can reverse an
        # already tiny coarse error. Retain an unaligned control without claiming
        # a pointwise monotone refinement rate for that mesh sequence.
        for side in ("put", "call"):
            for scheme in ("cn", "dirk_l", "dirk_third"):
                model = dict(S=103., K=100., T=.25, sigma=.2, r=-.01, q=.03, side=side)
                exact = european(model)
                answer = greek_reference(model, nx=2049, nt=512, scheme=scheme, american=False)
                for field, cap in (("price", .0001), ("delta", .00002), ("gamma", .000003)):
                    self.assertLess(abs(answer[field]-exact[field]), cap)

    def test_zero_rate_american_put_has_european_derivatives(self):
        for side in ("put", "call"):
            for scheme in ("cn", "dirk_l", "dirk_third"):
                model = dict(S=100., K=100., T=.5, sigma=.3, r=0., q=0., side=side)
                answer = greek_reference(model, nx=2049, nt=512, scheme=scheme)
                exact = european(model)
                self.assertEqual(answer["regime"], "continuation")
                self.assertLess(abs(answer["delta"]-exact["delta"]), 2e-5)
                self.assertLess(abs(answer["gamma"]-exact["gamma"]), 3e-6)
                self.assertLessEqual(answer["max_complementarity_residual"], answer["lcp_tolerance"])

    def test_exercised_put_supplies_affine_limit_with_actual_lcp_evidence(self):
        for scheme in ("cn", "dirk_l", "dirk_third"):
            for side, spot, rate, carry, sign in (("put", 50., .05, 0., -1.), ("call", 150., 0., .1, 1.)):
                model = dict(S=spot, K=100., T=.5, sigma=.2, r=rate, q=carry, side=side)
                answer = greek_reference(model, nx=1025, nt=256, scheme=scheme)
                self.assertEqual(answer["regime"], "exercise")
                self.assertEqual((answer["price"], answer["delta"], answer["gamma"]), (50., sign, 0.))
                self.assertTrue(all(x > answer["lcp_tolerance"] for x in answer["stencil_continuation"]))

    def test_mixed_and_ambiguous_discrete_states_do_not_supply_derivatives(self):
        self.assertEqual(classify_stencil(np.array([2., 2.1, 2.2]), np.array([2., 2., 2.]),
                                         np.array([.1, 0., 0.]), 1e-9), "mixed")
        self.assertEqual(classify_stencil(np.ones(3), np.ones(3), np.zeros(3), 1e-9), "ambiguous")
        self.assertEqual(classify_stencil(np.full(3, 2.), np.ones(3), np.zeros(3), 1e-9), "continuation")

    def test_invalid_inputs_and_explicit_resource_limits_refuse(self):
        model = dict(S=100., K=100., T=.5, sigma=.2, r=.03, q=.01, side="put")
        for options in (dict(nx=32), dict(nx=True), dict(nt=0), dict(nt=True),
                        dict(nx=65537), dict(nt=8192), dict(nx=32769, nt=4096),
                        dict(domain_scale=.5), dict(domain_scale=True), dict(scheme="unknown")):
            with self.assertRaises(ValueError, msg=str(options)):
                greek_reference(model, **options)
        for change in (dict(S=math.nan), dict(S=True), dict(sigma=0.), dict(T=-1.),
                       dict(side="invalid"), dict(dividends=[dict(t=.2, cash=1.)])):
            with self.assertRaises(ValueError, msg=str(change)):
                greek_reference(dict(model, **change))


if __name__ == "__main__":
    unittest.main()
