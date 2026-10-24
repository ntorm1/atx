"""Numerical prerequisites before using the new offline reference as evidence."""
import math
import unittest

import numpy as np

from pde_reference import price_group, solve_obstacle


def black_put(spot, strike, maturity, sigma, rate, carry):
    vol = sigma * math.sqrt(maturity)
    d1 = (math.log(spot / strike) + (rate - carry + .5 * sigma ** 2) * maturity) / vol
    return .5 * strike * math.exp(-rate * maturity) * math.erfc((d1 - vol) / math.sqrt(2)) - \
        .5 * spot * math.exp(-carry * maturity) * math.erfc(d1 / math.sqrt(2))


class ObstacleTests(unittest.TestCase):
    def test_clamping_a_linear_solution_does_not_solve_complementarity(self):
        # A concrete counterexample to solve-then-max: clipped [0,1/3] has a
        # negative continuation residual; the true LCP solution is [0,1/2].
        matrix = np.array([[2., -1.], [-1., 2.]])
        rhs = np.array([-1., 1.])
        clipped = np.maximum(np.linalg.solve(matrix, rhs), 0.)
        self.assertLess((matrix @ clipped - rhs).min(), -.3)
        result = solve_obstacle(np.array([-1.]), np.array([2., 2.]),
                                np.array([-1.]), rhs, np.zeros(2), np.zeros(2))
        np.testing.assert_allclose(result['values'], [0., .5], rtol=0, atol=1e-14)
        self.assertLessEqual(result['complementarity_residual'], 1e-14)

    def test_nonfinite_and_non_monotone_matrices_refuse(self):
        for lower, diagonal in [(np.array([1.]), np.array([2., 2.])),
                                (np.array([-1.]), np.array([math.nan, 2.]))]:
            with self.assertRaises(ValueError):
                solve_obstacle(lower, diagonal, np.array([-1.]),
                               np.zeros(2), np.zeros(2), np.zeros(2))


class PriceReferenceTests(unittest.TestCase):
    def test_european_limit_refines_toward_independent_black_prices(self):
        group = dict(K=100., T=.25, sigma=.2, r=0., q=0., side='put', spots=[90., 100., 110.])
        exact = np.array([black_put(s, 100., .25, .2, 0., 0.) for s in group['spots']])
        coarse = price_group(group, nx=513, nt=128)
        fine = price_group(group, nx=1025, nt=256)
        coarse_error = np.max(np.abs(np.array(coarse['prices']) - exact))
        fine_error = np.max(np.abs(np.array(fine['prices']) - exact))
        self.assertLess(fine_error, .001)
        self.assertLess(fine_error, .4 * coarse_error)
        self.assertLess(fine['max_complementarity_residual'], 1e-8)

    def test_american_put_respects_obstacle_and_exceeds_european(self):
        group = dict(K=100., T=1., sigma=.2, r=.05, q=0., side='put', spots=[70., 100., 140.])
        result = price_group(group, nx=1025, nt=256)
        for spot, price in zip(group['spots'], result['prices']):
            self.assertGreaterEqual(price, max(100. - spot, 0.) - 1e-10)
            self.assertGreaterEqual(price, black_put(spot, 100., 1., .2, .05, 0.) - .001)
            self.assertLessEqual(price, 100.)
        # Widely used Black-Scholes American put benchmark, loose here:
        # this is a smoke check, not a claimed exact decimal reference.
        self.assertTrue(6.08 < result['prices'][1] < 6.10)

    def test_invalid_grid_and_models_do_not_produce_prices(self):
        group = dict(K=100., T=1., sigma=.2, r=.05, q=0., side='put', spots=[100.])
        for options in [dict(nx=1), dict(nx=100), dict(nt=0), dict(domain_scale=.5), dict(domain_scale=True)]:
            with self.assertRaises(ValueError):
                price_group(group, **options)
        for field, value in [('sigma', 0.), ('r', math.nan), ('T', -1.),
                             ('spots', [math.inf]), ('spots', [True]), ('spots', ['100'])]:
            with self.assertRaises(ValueError):
                price_group(dict(group, **{field: value}))
        with self.assertRaises(ValueError):
            price_group(dict(group, dividends=[dict(time=.5, amount=1.)]))


if __name__ == '__main__':
    unittest.main()
