"""Postimplementation independent-solver and convention checks; synthetic data only."""
from __future__ import annotations

import copy
import importlib.util
import json
import math
import sys
from fractions import Fraction
from pathlib import Path

import pytest

ql = pytest.importorskip("QuantLib", reason="independent audit requires optional QuantLib")
PATH = Path(__file__).resolve().parents[2] / "tools" / "american_reference_audit.py"
SPEC = importlib.util.spec_from_file_location("american_reference_audit", PATH)
audit = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = audit
SPEC.loader.exec_module(audit)


@pytest.fixture(autouse=True)
def restore_quantlib_settings():
    original = ql.Settings.instance().evaluationDate
    ql.Settings.instance().evaluationDate = ql.Date(1, 1, 2000)
    yield
    ql.Settings.instance().evaluationDate = original


def interval(t=.5, v=.25, r=.03, q=.01, vol=.2, cash=0):
    return {"calendar_end": t, "variance_end": v, "rate": r,
            "continuous_yield": q, "volatility": vol, "cash_at_end": cash}


def config(**extra):
    return audit.AuditConfig(time_steps=80, space_nodes=160, levels=2, **extra)


def document(*, schema=2, spot=100., intervals=None):
    return {"schema_version": schema, "cases": [
        {"fixture": "synthetic", "grid": "coarse", "spot": spot,
         "intervals": intervals or [interval()], "model_status": "ok", "pricing_status": "ok",
         "prices": [{"expiry": 0, "strike": spot, "side": "put", "exercise": "american",
                     "price": 3.0}]}]}


def black(spot, strike, rate_integral, yield_integral, variance, side):
    sd = math.sqrt(variance)
    d1 = (math.log(spot / strike) + rate_integral - yield_integral + variance / 2) / sd
    d2 = d1 - sd
    def normal(x):
        return .5 * math.erfc(-x / math.sqrt(2))
    call = spot * math.exp(-yield_integral) * normal(d1) - strike * math.exp(-rate_integral) * normal(d2)
    return call if side == "call" else call - spot * math.exp(-yield_integral) + strike * math.exp(-rate_integral)


def test_exact_synthetic_clock_preserves_piecewise_drift_variance_and_pause():
    intervals = [interval(.02, .01, .03, .01, .3),
                 interval(.03, .01, -.01, .02, .3),
                 interval(.1, .07, .03, .01, .3)]
    model = audit.build_process(100, intervals, config())
    previous_calendar = previous_variance = r_integral = q_integral = variance = 0
    for i, entry in enumerate(intervals):
        middle_calendar = (previous_calendar + entry["calendar_end"]) / 2
        synthetic_time = middle_calendar / .1 * model.clock_map["synthetic_years"]
        dc = middle_calendar - previous_calendar
        dv = (entry["variance_end"] - previous_variance) / 2
        assert model.process.riskFreeRate().discount(synthetic_time) == pytest.approx(
            math.exp(-r_integral - entry["rate"] * dc), abs=1e-14)
        assert model.process.dividendYield().discount(synthetic_time) == pytest.approx(
            math.exp(-q_integral - entry["continuous_yield"] * dc), abs=1e-14)
        assert model.process.blackVolatility().blackVariance(synthetic_time, 100) == pytest.approx(
            variance + entry["volatility"]**2 * dv, abs=1e-14)
        assert Fraction(model.clock_map["day_offsets"][i], model.clock_map["horizon_days"]) == (
            Fraction(str(entry["calendar_end"])) / Fraction("0.1"))
        dt = entry["calendar_end"] - previous_calendar
        r_integral += entry["rate"] * dt
        q_integral += entry["continuous_yield"] * dt
        variance += 2 * entry["volatility"]**2 * dv
        previous_calendar, previous_variance = entry["calendar_end"], entry["variance_end"]
    assert len(model.dividends) == 2  # Both clock/parameter changes force rollback stops.
    assert model.clock_map["represented_total_variance"][0] == model.clock_map["represented_total_variance"][1]
    assert model.clock_map["variance_roundtrip_max_abs_error"] < 1e-17


def test_arbitrary_single_interval_dual_clocks_need_no_date_rounding():
    model = audit.build_process(100, [interval(7 / 365, 1 / 252, .04, .01, .02)], config())
    assert model.clock_map["horizon_days"] == 365
    assert model.clock_map["date_rounding"] is False
    assert model.process.blackVolatility().blackVariance(1., 100) == pytest.approx(.02**2 / 252)
    assert model.process.riskFreeRate().discount(1.) == pytest.approx(math.exp(-.04 * 7 / 365))


def test_nonrepresentable_cash_interval_map_is_refused_instead_of_rounded():
    intervals = [interval(.170000000000001, .1, cash=2), interval(.3, .2)]
    result = audit.reference(100, intervals, 100, "put", "american", config())
    assert result["status"] == "unsupported"
    assert "exact interval/date map" in result["reason"]
    assert "reference_price" not in result


@pytest.mark.parametrize("side,exercise,r,q", [("put", "american", .05, 0),
                                              ("call", "american", -.03, -.06),
                                              ("put", "american", -.03, .01),
                                              ("call", "european", .03, .01)])
def test_exposed_domain_solver_matches_stock_quantlib_vanilla_engine(side, exercise, r, q):
    model = audit.build_process(100, [interval(.75, .5, r, q, .3)], config())
    value = audit.fd_price(model, 100, 100, side, exercise,
                           time_steps=160, space_nodes=320, damping_steps=2)["price"]
    payoff = ql.PlainVanillaPayoff(ql.Option.Call if side == "call" else ql.Option.Put, 100)
    ex = (ql.AmericanExercise(model.today, model.maturity) if exercise == "american"
          else ql.EuropeanExercise(model.maturity))
    option = ql.VanillaOption(payoff, ex)
    option.setPricingEngine(ql.FdBlackScholesVanillaEngine(model.process, 160, 320, 2))
    assert value == pytest.approx(option.NPV(), abs=1e-12)


@pytest.mark.parametrize("side", ["call", "put"])
def test_refined_european_matches_independent_dual_clock_formula(side):
    inputs = [interval(.75, .5, -.03, -.06, .3)]
    result = audit.reference(100, inputs, 100, side, "european", config())
    expected = black(100, 100, -.03 * .75, -.06 * .75, .3**2 * .5, side)
    assert result["reference_price"] == pytest.approx(expected, abs=.002)
    assert result["analytic_reference_price"] == pytest.approx(expected, abs=1e-12)
    assert set(result["probes"]) == {"time", "space", "domain"}
    assert result["probes"]["domain"]["minimum_stock"] < result["refinements"][-1]["minimum_stock"]
    assert result["probes"]["domain"]["maximum_stock"] > result["refinements"][-1]["maximum_stock"]


@pytest.mark.parametrize("side", ["call", "put"])
def test_terminal_cash_european_matches_independent_payoff_identity(side):
    cash = 20
    inputs = [interval(.5, .5, .03, .01, .6, cash)]
    result = audit.reference(100, inputs, 100, side, "european", config())
    # Limited-liability terminal put is P(K+D)-P(D); terminal call is C(K+D).
    expected = black(100, 100 + cash, .015, .005, .18, side)
    if side == "put":
        expected -= black(100, cash, .015, .005, .18, "put")
    assert result["reference_price"] == pytest.approx(expected, abs=.01)
    assert "finite mesher minimum" in result["cash_boundary_scope"]
    assert result["clock_map"]["day_offsets"] == [365]


def test_terminal_cash_american_call_includes_pre_dividend_exercise():
    inputs = [interval(.5, .25, .03, 0, .2, 2)]
    result = audit.reference(100, inputs, 100, "call", "american", config())
    expected = black(100, 100, .015, 0, .01, "call")
    assert result["reference_price"] == pytest.approx(expected, abs=.002)


def test_short_low_vol_atm_put_is_stable_and_has_positive_exercise_premium():
    inputs = [interval(7 / 365, 1 / 252, .04, .01, .02)]
    result = audit.reference(100, inputs, 100, "put", "american", audit.AuditConfig())
    european = black(100, 100, .04 * 7 / 365, .01 * 7 / 365, .02**2 / 252, "put")
    assert result["status"] == "observed_stable"
    assert result["refinement_uncertainty_estimate"] < 1e-5
    assert result["reference_price"] > european + .001
    assert result["reference_price"] > .03


def test_zero_tolerance_exposes_unresolved_numerics_and_is_not_exact_claim():
    result = audit.reference(100, [interval()], 100, "put", "american",
                             config(absolute_tolerance=0, relative_tolerance=0))
    assert result["status"] == "unresolved"
    assert result["refinement_uncertainty_estimate"] > 0


@pytest.mark.parametrize("schema", [1, 2])
def test_reference_reuse_does_not_depend_on_source_prices_and_restores_global_date(schema):
    source = document(schema=schema, spot=200)
    other = copy.deepcopy(source["cases"][0])
    other["grid"] = "fine"
    other["prices"][0]["price"] = 5.
    source["cases"].append(other)
    original = ql.Settings.instance().evaluationDate
    try:
        ql.Settings.instance().evaluationDate = ql.Date(17, 7, 2024)
        report = audit.audit_document(source, config())
        assert ql.Settings.instance().evaluationDate == ql.Date(17, 7, 2024)
    finally:
        ql.Settings.instance().evaluationDate = original
    assert len(report["references"]) == 1
    first, second = [case["rows"][0] for case in report["cases"]]
    assert first["reference_id"] == second["reference_id"]
    assert second["difference"] - first["difference"] == pytest.approx(2)
    assert report["cases"][1]["near_atm_summary"]["rows"] == 1
    assert report["dependency"]["native_modules"]


def test_unsupported_local_vol_and_cash_forward_cases_stay_visible():
    source = document()
    source["cases"][0]["intervals"][0]["local_vol"] = [1, 2]
    report = audit.audit_document(source, config())
    assert report["summary"]["reference_status_counts"] == {"unsupported": 1}
    assert report["summary"]["compared_rows"] == 0
    result = audit.reference(100, [interval(cash=120)], 100, "put", "american", config())
    assert result["status"] == "unsupported"
    assert "nonpositive" in result["reason"]


def test_invalid_or_oversized_contract_requests_fail_explicitly():
    source = document()
    source["cases"][0]["prices"].append(copy.deepcopy(source["cases"][0]["prices"][0]))
    with pytest.raises(ValueError, match="duplicate"):
        audit.audit_document(source, config())
    source["cases"][0]["prices"][-1]["strike"] = 101
    with pytest.raises(ValueError, match="max_unique_contracts"):
        audit.audit_document(source, config(max_unique_contracts=1))
    source["schema_version"] = True
    with pytest.raises(ValueError, match="schema_version"):
        audit.audit_document(source, config())


def test_cli_preserves_existing_input_and_rejects_duplicate_json_keys(tmp_path):
    path = tmp_path / "input.json"
    path.write_text(json.dumps(document()))
    original = path.read_bytes()
    assert audit.main(["--input", str(path), "--output", str(path)]) == 1
    assert path.read_bytes() == original
    path.write_text('{"schema_version":1,"schema_version":2,"cases":[]}')
    assert audit.main(["--input", str(path), "--output", str(tmp_path / "result.json")]) == 1
    assert not (tmp_path / "result.json").exists()
