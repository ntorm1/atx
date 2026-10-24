import copy
import math
import unittest

from greek_compare import same_selected_ids, verify_row


MODEL = dict(id="test", S=50., K=100., T=.5, sigma=.2, r=.05, q=0., side="put")


def row():
    width = max(.1, 8*.2*math.sqrt(.5)+abs((.05-.5*.2**2)*.5), abs(math.log(100.)-math.log(50.))+.1)
    dx = 2*width/4096
    spots = [50.*math.exp(-dx), 50., 50.*math.exp(dx)]
    values = [100.-s for s in spots]
    return dict(id="test", model=MODEL, scheme="dirk_l", label="center", status="computed",
        options=dict(nx=4097, nt=512, domain_scale=1.), result=dict(
            nx=4097, nt=512, domain_scale=1., scheme="dirk_l", american=True,
            accuracy_established=False, greek_accuracy_established=False,
            price=50., delta=-1., gamma=0., regime="exercise", log_spacing=dx,
            log_half_width=width, domain=[50.*math.exp(-width), 50.*math.exp(width)],
            stages=1022, stage_node_updates=1022*4095, policy_iterations=1022, max_policy_iterations=1,
            stencil_spots=spots, stencil_values=values, stencil_payoff=values,
            stencil_continuation=[1., 1., 1.], lcp_tolerance=1e-10,
            max_complementarity_residual=0.))


class GreekCompareTests(unittest.TestCase):
    def test_second_experiment_rows_require_their_declared_finer_configuration(self):
        sample = row()
        r = sample["result"]
        r["nx"] = sample["options"]["nx"] = 8193
        r["stage_node_updates"] = 1022*8191
        r["log_spacing"] = dx = r["log_half_width"]/4096
        r["stencil_spots"] = [50.*math.exp(-dx), 50., 50.*math.exp(dx)]
        r["stencil_values"] = r["stencil_payoff"] = [100.-s for s in r["stencil_spots"]]
        verify_row(sample, MODEL, version=2)
        with self.assertRaises(ValueError):
            verify_row(sample, MODEL)
        with self.assertRaises(ValueError):
            verify_row(row(), MODEL, version=2)
        for scheme in ("cn", "unknown"):
            changed = copy.deepcopy(sample)
            changed["scheme"] = changed["result"]["scheme"] = scheme
            with self.assertRaises(ValueError):
                verify_row(changed, MODEL, version=2)

    def test_native_sorted_ids_preserve_exact_population_without_duplicates(self):
        self.assertTrue(same_selected_ids(["a", "b"], ["b", "a"]))
        for actual in (["a"], ["a", "a"], ["a", "other"], ["a", "b", "b"]):
            self.assertFalse(same_selected_ids(actual, ["b", "a"]))

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
            lambda r: r["result"].update(log_half_width=.1),
            lambda r: r["result"]["domain"].__setitem__(1, 10000.),
            lambda r: r["result"].update(lcp_tolerance=1e-5),
            lambda r: r["result"].update(stages=512),
            lambda r: r["result"].update(stage_node_updates=0),
            lambda r: r["result"].update(policy_iterations=-1),
            lambda r: r["result"].update(max_policy_iterations=50000),
        ]
        for mutate in mutations:
            sample = copy.deepcopy(row())
            mutate(sample)
            with self.assertRaises(ValueError):
                verify_row(sample, MODEL)


if __name__ == "__main__":
    unittest.main()
