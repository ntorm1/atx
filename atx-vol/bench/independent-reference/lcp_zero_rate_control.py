"""Fixed twelve-request offline control; no historical reference promotion."""
import copy

from greek_compare import finite, require, verify_row
from greek_study import closed_form_control, study_spec, summarize_reference


MODEL = dict(id="zero_rate_closed_form-put-03", group="zero_rate_closed_form", side="put",
             S=100., K=100., T=1., sigma=.2, r=0., q=0.)
MODE = "one_correction"
COUNTS = ("iterations", "policy_solves", "correction_solves", "linear_solves",
          "residual_checks", "policy_iteration_bound", "linear_solve_bound", "linear_solve_budget")
FAILURES = {"linear_budget", "policy_iteration_limit", "nonfinite_original_residual",
            "linear_solve_failure", "nonfinite_policy_solves", "nonfinite_correction_solves",
            "nonfinite_correction_rhs", "nonfinite_corrected_values"}


def fixed_requests():
    schemes, configs = study_spec(2)
    return [dict(id=MODEL["id"], model=dict(MODEL), scheme=scheme, label=label,
                 options=dict(nx=nx, nt=nt, domain_scale=domain), lcp_mode=MODE)
            for scheme in schemes for label, (nx, nt, domain) in configs.items()]


def summarize_control(rows):
    expected = fixed_requests()
    require(type(rows) is list and len(rows) == len(expected) == 12,
            "All twelve original zero-rate requests are required")
    observed = []
    for row, request in zip(rows, expected):
        require(type(row) is dict and {"status", "stage_work"} <= set(row) and
                all(row.get(key) == value for key, value in request.items()),
                "Fixed mode/model/grid/order identity")
        require(all(finite(row["model"][key]) for key in ("S", "K", "T", "sigma", "r", "q"))
                and all(type(row["options"][key]) is int for key in ("nx", "nt"))
                and finite(row["options"]["domain_scale"]), "Actual numeric model/grid types")
        verify_row(row, MODEL, version=2)
        observed.append(validate_stage_work(row))
    exact = closed_form_control(MODEL)
    errors = []
    for row in rows:
        differences = None
        if row["status"] == "computed":
            differences = {field: abs(row["result"][field]-value)
                           if row["result"][field] is not None else None
                           for field, value in exact.items()}
        errors.append(dict(scheme=row["scheme"], label=row["label"], status=row["status"],
                           absolute_error=differences))
    diagnostics = copy.deepcopy(summarize_reference(MODEL, rows, version=2))
    for value in diagnostics.values():
        value["prospective_rule_outcome"] = value["status"]
        value["status"] = "not_promoted"
    computed = sum(row["status"] == "computed" for row in rows)
    return dict(schema="atx-zero-rate-lcp-control-summary-v1", requested=12,
        computed=computed, refused=12-computed, analytic=exact, analytic_errors=errors,
        observed_work=observed, refinement_diagnostics=diagnostics,
        solver_controls_passed=computed == 12 and all(
            value["prospective_rule_outcome"] == "empirically_resolved"
            for value in diagnostics.values()),
        reference_decisions_updated=False, full_144_run=False, accuracy_established=False,
        full_bundle_qualified=False, timing_qualified=False)


def validate_stage_work(row):
    records = row["stage_work"]
    nx, nt = row["options"]["nx"], row["options"]["nt"]
    stages, tolerance = 2*nt-2, 1e-10
    require(type(records) is list and len(records) <= stages, "Bounded stage-work denominator")
    total = dict(policy_solves=0, correction_solves=0, linear_solves=0, iterations=0,
                 max_iterations=0, residual_checks=0, observed_stages=len(records),
                 unobserved_failure=False)
    residuals = []
    for index, record in enumerate(records):
        require(type(record["stage"]) is int and record["stage"] == index+1,
                "Contiguous actual stage sequence")
        complete = record["status"] == "computed"
        if not complete:
            require(index+1 == len(records) and row["status"] == "refused",
                    "No stage after a refusal or missing final result")
        if record["status"] == "unobserved_failure":
            require(record["work"] is None and type(record["exception_type"]) is str and
                    bool(record["exception_type"]) and type(record["detail"]) is str and
                    bool(record["detail"]), "Unknown work cannot become invented zero counts")
            total["unobserved_failure"] = True
            continue
        require(record["status"] in ("computed", "refused"), "Typed stage status")
        work = record["work"]
        fields = set(COUNTS) | {"mode", "tolerance"} | (
            {"complementarity_residual"} if complete else {"reason", "last_checked_residual"})
        require(type(work) is dict and set(work) == fields, "Complete bounded-work metadata")
        require(work["mode"] == MODE and finite(work["tolerance"]) and
                work["tolerance"] == tolerance, "Original unscaled stage tolerance")
        require(all(type(work[key]) is int and work[key] >= 0 for key in COUNTS),
                "Integer stage work")
        count, policy, correction = (work[key] for key in
                                     ("iterations", "policy_solves", "correction_solves"))
        require(work["policy_iteration_bound"] == nx and
                work["linear_solve_bound"] == work["linear_solve_budget"] == 2*nx and
                0 <= count <= correction <= policy <= nx and
                policy-count <= 1 and policy-correction <= 1 and
                work["linear_solves"] == policy+correction and
                work["residual_checks"] == count+1, "Actual bounded two-solve policy work")
        if complete:
            require(count == policy == correction and finite(work["complementarity_residual"])
                    and 0 <= work["complementarity_residual"] <= tolerance,
                    "Completed corrected update and original residual")
            residuals.append(work["complementarity_residual"])
        else:
            require(work["reason"] in FAILURES and
                    (work["last_checked_residual"] is None or
                     finite(work["last_checked_residual"]) and work["last_checked_residual"] >= 0),
                    "Typed failure with last checked residual")
            require(work["reason"] != "policy_iteration_limit" or count == nx,
                    "Original update bound reached before iteration refusal")
        for key in ("policy_solves", "correction_solves", "linear_solves",
                    "iterations", "residual_checks"):
            total[key] += work[key]
        total["max_iterations"] = max(total["max_iterations"], count)
    if row["status"] == "computed":
        require(len(records) == stages and all(r["status"] == "computed" for r in records),
                "Every successful PDE stage accounted")
        result = row["result"]
        require(result["stages"] == stages and result["policy_iterations"] == total["iterations"]
                and result["max_policy_iterations"] == total["max_iterations"] and
                result["max_complementarity_residual"] == max(residuals),
                "PDE and primitive aggregate work agree")
    return total
