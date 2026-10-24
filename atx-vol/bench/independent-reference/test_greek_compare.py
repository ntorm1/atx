import copy
import math
import unittest

from greek_compare import verify_row


MODEL = dict(id="test", S=50., K=100., T=.5, sigma=.2, r=.05, q=0., side="put")


def row():
    spots = [50.*math.exp(-.001), 50., 50.*math.exp(.001)]
    values = [100.-s for s in spots]
    return dict(id="test", model=MODEL, scheme="dirk_l", label="center", status="computed",
        options=dict(nx=4097, nt=512, domain_scale=1.), result=dict(
            nx=4097, nt=512, domain_scale=1., scheme="dirk_l", american=True,
            accuracy_established=False, greek_accuracy_established=False,
            price=50., delta=-1., gamma=0., regime="exercise", log_spacing=.001,
            stencil_spots=spots, stencil_values=values, stencil_payoff=values,
            stencil_continuation=[1., 1., 1.], lcp_tolerance=1e-10,
            max_complementarity_residual=0.))


class GreekCompareTests(unittest.TestCase):
    def test_exact_exercised_receipt_and_truthful_refusal(self):
        verify_row(row(), MODEL)
        refused = row()
        refused.pop("result")
        refused.update(status="refused", reason="ArithmeticError", detail="retained solve failure")
        verify_row(refused, MODEL)

    def test_semantic_mutants_cannot_pass_by_matching_outer_label(self):
        mutations = [
            lambda r: r.update(id="other"),
            lambda r: r.update(scheme="other"),
            lambda r: r.update(model=dict(MODEL, S=51.)),
            lambda r: r["options"].update(nx=2049),
            lambda r: r["result"].update(nx=2049),
            lambda r: r["result"].update(scheme="cn"),
            lambda r: r["result"].update(american=False),
            lambda r: r["result"].update(greek_accuracy_established=True),
            lambda r: r["result"].update(price=49.),
            lambda r: r["result"].update(gamma=.1),
            lambda r: r["result"].update(regime="continuation"),
            lambda r: r["result"].update(log_spacing=0.),
            lambda r: r["result"]["stencil_spots"].__setitem__(1, 51.),
            lambda r: r["result"]["stencil_payoff"].__setitem__(0, 0.),
            lambda r: r["result"]["stencil_continuation"].__setitem__(1, float("nan")),
            lambda r: r["result"].update(max_complementarity_residual=1.),
        ]
        for mutate in mutations:
            sample = copy.deepcopy(row())
            mutate(sample)
            with self.assertRaises(ValueError):
                verify_row(sample, MODEL)


if __name__ == "__main__":
    unittest.main()
