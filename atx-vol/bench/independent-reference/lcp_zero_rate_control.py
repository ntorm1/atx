"""Fixed twelve-request offline control; no historical reference promotion."""
import copy

from greek_compare import finite, require, verify_row
from greek_study import closed_form_control, study_spec, summarize_reference


MODEL = dict(id="zero_rate_closed_form-put-03", group="zero_rate_closed_form", side="put",
             S=100., K=100., T=1., sigma=.2, r=0., q=0.)
MODE = "one_correction"
RETAINED_MODE = "one_correction_retained_admissible"
COUNTS = ("iterations", "policy_solves", "correction_solves", "linear_solves",
          "residual_checks", "policy_iteration_bound", "linear_solve_bound", "linear_solve_budget")
FAILURES = {"linear_budget", "policy_iteration_limit", "nonfinite_original_residual",
            "linear_solve_failure", "nonfinite_policy_solves", "nonfinite_correction_solves",
            "nonfinite_correction_rhs", "nonfinite_corrected_values"}


def _mode(mode):
    require(type(mode) is str and mode in (MODE, RETAINED_MODE), "Declared LCP mode")


def fixed_requests(mode=MODE):
    _mode(mode)
    schemes, configs = study_spec(2)
    return [dict(id=MODEL["id"], model=dict(MODEL), scheme=scheme, label=label,
                 options=dict(nx=nx, nt=nt, domain_scale=domain), lcp_mode=mode)
            for scheme in schemes for label, (nx, nt, domain) in configs.items()]


def summarize_control(rows, mode=MODE):
    expected = fixed_requests(mode)
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
        observed.append(validate_stage_work(row, mode))
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
    schema = "atx-zero-rate-lcp-control-summary-v1" if mode == MODE else "atx-zero-rate-retained-lcp-summary-v1"
    return dict(schema=schema, requested=12,
        computed=computed, refused=12-computed, analytic=exact, analytic_errors=errors,
        observed_work=observed, refinement_diagnostics=diagnostics,
        solver_controls_passed=computed == 12 and all(
            value["prospective_rule_outcome"] == "empirically_resolved"
            for value in diagnostics.values()),
        reference_decisions_updated=False, full_144_run=False, accuracy_established=False,
        full_bundle_qualified=False, timing_qualified=False)


POLICY_COUNTS = {"workspace_required_bytes", "workspace_reserved_bytes", "selection_attempts",
                "selection_completions", "initial_policy_rows", "admissible_retained_rows",
                "retained_overrides", "failing_reselected_rows", "policy_changes"}


def _policy(work, nx, expected_selections=None, workspace_refused=False):
    require(type(work) is dict and set(work) == POLICY_COUNTS and
            all(type(v) is int and v >= 0 for v in work.values()), "Complete integer selector work")
    n = nx-2
    attempts, complete = work["selection_attempts"], work["selection_completions"]
    require(work["workspace_required_bytes"] == 32*n and
            work["workspace_reserved_bytes"] == (0 if workspace_refused else 32*n),
            "Original bounded selector reservation")
    require(0 <= complete <= attempts <= min(nx, 128) and attempts-complete <= 1,
            "Actual selector attempt/completion bound")
    require(work["initial_policy_rows"] == (n if complete else 0) and
            work["admissible_retained_rows"]+work["failing_reselected_rows"] == max(0, complete-1)*n and
            work["retained_overrides"] <= work["admissible_retained_rows"] and
            work["policy_changes"] <= work["failing_reselected_rows"], "Selector row conservation")
    if expected_selections is not None:
        require(attempts == complete == expected_selections, "Observed original and selector work join")
    if workspace_refused:
        require(attempts == 0, "No selection before unavailable workspace")


def _unknown_retention(record, nx):
    require(set(record) == {"stage", "status", "work", "exception_type", "detail", "retention_failure"},
            "Explicit unknown retained work")
    detail = record["retention_failure"]
    if detail is None:
        require(record["exception_type"] != "LcpRetentionRefused", "Typed failure diagnostics required")
        return
    require(type(detail) is dict and set(detail) == {"mode", "base_mode", "reason", "phase", "policy_work"}
            and detail["mode"] == RETAINED_MODE and detail["base_mode"] == MODE
            and record["exception_type"] == "LcpRetentionRefused", "Typed partial retention metadata")
    workspace = detail["reason"] == "policy_workspace"
    require((workspace and detail["phase"] == "preflight") or
            (detail["reason"] == "allocation_unavailable" and detail["phase"] == "unavailable"),
            "Unknown work is not an algorithmic/numerical failure")
    if detail["policy_work"] is None:
        require(not workspace, "Workspace refusal retains its constructed reservation")
    else:
        _policy(detail["policy_work"], nx, workspace_refused=workspace)


def _retained_phase(work, nx):
    count, policy, correction = (work[k] for k in ("iterations", "policy_solves", "correction_solves"))
    phase = (policy-count, correction-count)
    reason = work["reason"]
    allowed = {"linear_budget": {(0, 0)}, "policy_iteration_limit": {(0, 0)},
        "linear_solve_failure": {(1, 0), (1, 1)}, "nonfinite_policy_solves": {(1, 0)},
        "nonfinite_correction_rhs": {(1, 0)}, "nonfinite_correction_solves": {(1, 1)},
        "nonfinite_corrected_values": {(1, 1)}, "nonfinite_original_residual": {(0, 0)}}
    require(phase in allowed[reason], "Failure phase agrees with attempted paired solves")
    residual = work["last_checked_residual"]
    if residual is None:
        require(reason == "nonfinite_original_residual" and count == 0,
                "Only initial nonfinite evidence may lack a checked residual")
    else:
        require(residual > work["tolerance"], "Failed work cannot erase an admitted original state")
        require(reason != "nonfinite_original_residual" or count > 0, "Initial nonfinite phase")
    if reason == "linear_budget":
        require(count < nx and work["linear_solve_budget"]-work["linear_solves"] < 2,
                "Effective pair budget exhausted before resource refusal")


def validate_stage_work(row, mode=MODE):
    _mode(mode)
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
            if mode == RETAINED_MODE:
                _unknown_retention(record, nx)
            continue
        require(record["status"] in ("computed", "refused"), "Typed stage status")
        if mode == RETAINED_MODE:
            require(set(record) == {"stage", "status", "work"}, "Exact observed retained-stage fields")
        work = record["work"]
        fields = set(COUNTS) | {"mode", "tolerance"} | (
            {"complementarity_residual"} if complete else {"reason", "last_checked_residual"})
        if mode == RETAINED_MODE:
            fields |= {"base_mode", "policy_work"}
        require(type(work) is dict and set(work) == fields, "Complete bounded-work metadata")
        require(work["mode"] == mode and finite(work["tolerance"]) and
                work["tolerance"] == tolerance, "Original unscaled stage tolerance")
        require(all(type(work[key]) is int and work[key] >= 0 for key in COUNTS),
                "Integer stage work")
        count, policy, correction = (work[key] for key in
                                     ("iterations", "policy_solves", "correction_solves"))
        budget = 2*nx if mode == MODE else min(2*nx, 256)
        require(work["policy_iteration_bound"] == nx and work["linear_solve_bound"] == 2*nx and
                work["linear_solve_budget"] == budget and
                0 <= count <= correction <= policy <= budget//2 and
                policy-count <= 1 and policy-correction <= 1 and
                work["linear_solves"] == policy+correction and
                work["residual_checks"] == count+1, "Actual bounded two-solve policy work")
        if mode == RETAINED_MODE:
            require(work["base_mode"] == MODE, "Original arithmetic underneath retained policy")
            _policy(work["policy_work"], nx, expected_selections=policy)
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
            if mode == RETAINED_MODE:
                _retained_phase(work, nx)
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
