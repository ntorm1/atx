"""Private, explicit solver substitution for the fixed offline LCP experiment.

The original Greek/PDE module and process-global imported modules are untouched.
The loaded private module has no registration in sys.modules or shared cache.
"""
import importlib.util
from pathlib import Path


class StageCollector:
    def __init__(self):
        self.records = []
        self.failure_arrays = None

    def __call__(self, lower, diagonal, upper, rhs, payoff, initial, tolerance):
        # Import only after the runner has set its numerical-thread environment.
        from lcp_correction import LcpCorrectionRefused, solve_obstacle_control
        stage = len(self.records)+1
        try:
            answer = solve_obstacle_control(lower, diagonal, upper, rhs, payoff, initial,
                                            tolerance, mode="one_correction")
        except LcpCorrectionRefused as error:
            work = dict(error.diagnostics)
            values = work.pop("last_checked_values")
            self.failure_arrays = {name: array.copy() for name, array in zip(
                ("lower", "diagonal", "upper", "rhs", "payoff", "initial"),
                (lower, diagonal, upper, rhs, payoff, initial))}
            if values is not None:
                self.failure_arrays["last_checked_values"] = values.copy()
            self.records.append(dict(stage=stage, status="refused", work=work))
            raise
        except (ValueError, ArithmeticError) as error:
            # Never fabricate attempted-solve counts if no typed work record exists.
            self.records.append(dict(stage=stage, status="unobserved_failure", work=None,
                                     exception_type=type(error).__name__, detail=str(error)))
            raise
        work = {key: value for key, value in answer.items() if key != "values"}
        self.records.append(dict(stage=stage, status="computed", work=work))
        return answer


def private_reference(mode, collector=None):
    """Load the unchanged source into an isolated namespace; no global monkeypatch.

    original preserves the original imported solver; one_correction requires a
    fresh caller-owned collector, attached only to this new module instance.
    """
    if type(mode) is not str or mode not in ("original", "one_correction"):
        raise ValueError("Unknown private offline LCP mode")
    if mode == "one_correction":
        if not isinstance(collector, StageCollector) or collector.records:
            raise ValueError("A fresh correction collector is required")
    elif collector is not None:
        raise ValueError("Original mode must not receive a correction collector")
    source = Path(__file__).with_name("greek_reference.py")
    spec = importlib.util.spec_from_file_location("_isolated_lcp_greek_reference", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if mode == "one_correction":
        module.solve_obstacle = collector
    return module.greek_reference
