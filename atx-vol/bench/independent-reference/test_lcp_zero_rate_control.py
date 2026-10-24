"""Protocol fixtures only; none of the fixed twelve PDE requests execute here."""
import copy
import json
import os
from pathlib import Path
import unittest

from greek_study import study_spec
import lcp_zero_rate_control as control


MODEL = dict(id="zero_rate_closed_form-put-03", group="zero_rate_closed_form", side="put",
             S=100., K=100., T=1., sigma=.2, r=0., q=0.)


def completed_work(row):
    """Synthetic protocol work consistent with the historical aggregate counts."""
    result = row["result"]
    remaining, maximum = result["policy_iterations"], result["max_policy_iterations"]
    records = []
    for index in range(result["stages"]):
        count = min(remaining, maximum)
        remaining -= count
        n = result["nx"]
        records.append(dict(stage=index+1, status="computed", work=dict(
            mode="one_correction", iterations=count, policy_solves=count,
            correction_solves=count, linear_solves=2*count, residual_checks=count+1,
            policy_iteration_bound=n, linear_solve_bound=2*n, linear_solve_budget=2*n,
            tolerance=result["lcp_tolerance"],
            complementarity_residual=result["max_complementarity_residual"] if index == 0 else 0.)))
    if remaining:
        raise AssertionError("Invalid historical aggregate fixture")
    return records


class ControlDeclarationTests(unittest.TestCase):
    def test_fixed_requests_retain_both_schemes_and_all_six_original_grids(self):
        schemes, configs = study_spec(2)
        expected = [dict(id=MODEL["id"], model=MODEL, scheme=scheme, label=label,
                         options=dict(nx=nx, nt=nt, domain_scale=domain),
                         lcp_mode="one_correction")
                    for scheme in schemes for label, (nx, nt, domain) in configs.items()]
        self.assertEqual(control.fixed_requests(), expected)
        first = control.fixed_requests()
        first[0]["model"]["S"] = 101.
        self.assertEqual(control.fixed_requests(), expected)

    def test_incomplete_duplicate_wrong_mode_or_grid_never_summarizes(self):
        rows = control.fixed_requests()
        for wrong in ([], rows[:-1], rows[:-1]+rows[:1]):
            with self.assertRaises(ValueError):
                control.summarize_control(wrong)


class ControlReceiptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get("ATX_LCP_BASELINE")
        if source is None:
            raise unittest.SkipTest("Explicit immutable v2 baseline required")
        rows = [json.loads(line) for line in
                (Path(source)/"outcomes.jsonl").read_text().splitlines()]
        cls.rows = [row for row in rows if row["id"] == MODEL["id"]]
        for row in cls.rows:
            row["lcp_mode"] = "one_correction"
            if row["status"] == "computed":
                row["stage_work"] = completed_work(row)
            else:
                row["stage_work"] = [dict(stage=1, status="unobserved_failure", work=None,
                                         exception_type=row["reason"], detail=row["detail"])]

    def test_negative_protocol_keeps_all_six_historical_refusals_and_no_promotion(self):
        result = control.summarize_control(self.rows)
        self.assertEqual((result["requested"], result["computed"], result["refused"]), (12, 6, 6))
        self.assertEqual(len(result["analytic_errors"]), 12)
        self.assertFalse(result["solver_controls_passed"])
        self.assertFalse(result["reference_decisions_updated"])
        self.assertFalse(result["full_144_run"])
        for field in ("delta", "gamma"):
            self.assertEqual(result["refinement_diagnostics"][field]["status"], "not_promoted")
            self.assertEqual(result["refinement_diagnostics"][field]["prospective_rule_outcome"],
                             "unresolved")
        self.assertTrue(all(error["absolute_error"] is None for error in
                            result["analytic_errors"] if error["status"] == "refused"))

    def test_stage_counter_and_geometry_mutants_refuse(self):
        mutations = (
            lambda r: r["stage_work"].pop(),
            lambda r: r["stage_work"][0].update(stage=2),
            lambda r: r["stage_work"][0]["work"].update(mode="original"),
            lambda r: r["stage_work"][0]["work"].update(linear_solves=0),
            lambda r: r["stage_work"][0]["work"].update(policy_iteration_bound=10**9),
            lambda r: r["stage_work"][0]["work"].update(linear_solve_budget=10**9),
            lambda r: r["stage_work"][0]["work"].update(tolerance=1e-6),
            lambda r: r["stage_work"][0]["work"].update(residual_checks=True),
            lambda r: r["result"].update(log_spacing=.1),
            lambda r: r.update(lcp_mode="original"),
            lambda r: r["options"].update(nt=1024),
            lambda r: r["model"].update(S=101.),
            lambda r: r["model"].update(T=True),
            lambda r: r["options"].update(domain_scale=True),
            lambda r: r["options"].update(nx=float(r["options"]["nx"])),
        )
        for index, mutate in enumerate(mutations):
            rows = copy.deepcopy(self.rows)
            mutate(rows[0])
            with self.subTest(index=index), self.assertRaises(ValueError):
                control.summarize_control(rows)


if __name__ == "__main__":
    unittest.main()
