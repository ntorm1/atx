"""Postimplementation checks for controlled spatial/time audit experiments."""
from __future__ import annotations

import functools
import json
import math
import os
from pathlib import Path
import subprocess

import pytest


@pytest.fixture(scope="module")
def binary():
    configured = os.environ.get("ATX_AMERICAN_MARKOV_AUDIT_BINARY")
    if not configured:
        pytest.skip("ATX_AMERICAN_MARKOV_AUDIT_BINARY is not configured")
    result = Path(configured)
    assert result.is_file()
    return result


@pytest.fixture(scope="module")
def case(binary):
    @functools.lru_cache(maxsize=16)
    def run(fixture, grid, method=None):
        extra = ["--diffusion-method", method] if method is not None else []
        completed = subprocess.run(
            [str(binary), "--full", "--pricing-only", "--case", fixture, "--grid", grid, *extra],
            text=True, capture_output=True, timeout=60, check=False,
        )
        assert completed.returncode == 0, completed.stderr
        report = json.loads(completed.stdout)
        assert len(report["cases"]) == 1
        return report, report["cases"][0]
    return run


def assert_complete(report, result):
    wanted = {(expiry, strike, side, exercise)
              for expiry in range(len(result["intervals"]))
              for strike in result["requested_strikes"]
              for side in report["requested_sides"]
              for exercise in report["requested_exercises"]}
    produced = {(row["expiry"], row["strike"], row["side"], row["exercise"])
                for row in result["prices"]}
    assert len(produced) == len(result["prices"])
    assert wanted == produced
    assert all(math.isfinite(row["price"]) and row["price"] >= 0 for row in result["prices"])


@pytest.mark.parametrize("domain", [8, 16])
def test_matched_domains_preserve_both_reference_gaps_and_requested_contracts(case, domain):
    _, reference = case("high_vol", "sinh129-variance")
    report, result = case("high_vol", f"sinh-match-domain{domain}")
    assert result["model_status"] == result["pricing_status"] == "ok"
    match = result["spacing_match"]
    assert match["status"] == "matched"
    assert match["reference_lower_gap_over_spot"] == reference["grid_diagnostics"]["lower_spot_gap_over_spot"]
    assert match["reference_upper_gap_over_spot"] == reference["grid_diagnostics"]["upper_spot_gap_over_spot"]
    assert result["grid_diagnostics"]["lower_spot_gap_over_spot"] <= match["reference_lower_gap_over_spot"]
    assert result["grid_diagnostics"]["upper_spot_gap_over_spot"] <= match["reference_upper_gap_over_spot"]
    assert match["minimum_nodes"] <= result["resolved_nodes"] <= match["maximum_nodes"] == 1025
    assert match["candidate_evaluations"] == result["resolved_nodes"] - match["minimum_nodes"] + 1
    states = result["resolved_state_grid"]
    assert len(states) == result["resolved_nodes"] == result["model"]["states"]
    assert states[0] == 0 and states[-1] == result["spot"] * domain
    assert sum(value == result["spot"] for value in states) == 1
    assert all(math.isfinite(value) for value in states)
    assert all(a < b for a, b in zip(states, states[1:]))
    assert result["model_limits"] == reference["model_limits"]
    assert result["requested_nodes"] == 129
    assert_complete(report, result)


@pytest.mark.parametrize("factor", [2, 4])
def test_time_refinement_keeps_spatial_grid_and_changes_realized_cfl_steps(case, factor):
    _, reference = case("negative_rate", "sinh129-variance")
    report, result = case("negative_rate", f"sinh129-variance-time{factor}")
    assert result["model_status"] == result["pricing_status"] == "ok"
    assert result["resolved_state_grid"] == reference["resolved_state_grid"]
    assert result["grid_diagnostics"] == reference["grid_diagnostics"]
    assert result["concentration_scale"] == reference["concentration_scale"]
    controls, original = result["resolved_model_controls"], reference["resolved_model_controls"]
    for key in ("cfl_fraction", "max_calendar_step", "max_variance_step"):
        assert controls[key] == original[key] / factor
    assert result["max_clock_step"] == controls["max_calendar_step"]
    assert result["configured_max_clock_step"] == reference["configured_max_clock_step"]
    for key in ("max_states", "max_transitions", "max_stored_rows", "max_stored_volatility_values"):
        assert controls[key] == original[key]
    before = reference["model"]["diffusion_steps"]
    after = result["model"]["diffusion_steps"]
    assert factor * before - (factor - 1) <= after <= factor * before
    assert after > before
    assert result["requested_time_refinement"] == factor
    assert_complete(report, result)


def test_time_refinement_resource_refusal_keeps_grid_controls_and_requested_universe(case):
    _, reference = case("near_cash", "sinh129-variance")
    report, result = case("near_cash", "sinh129-variance-time2")
    assert reference["model_status"] == "ok"
    assert result["model_status"] == "refused"
    assert "limit" in result["model_error"]
    assert not result.get("prices")
    assert result["resolved_state_grid"] == reference["resolved_state_grid"]
    assert result["requested_strikes"] == reference["requested_strikes"]
    assert result["model_limits"] == reference["model_limits"]
    assert result["resolved_model_controls"]["cfl_fraction"] == .45
    assert report["requested_sides"] == ["call", "put"]
    assert report["requested_exercises"] == ["american", "european"]


def test_zero_variance_matching_uses_explicit_fixed_fallback_and_remains_deterministic(case):
    report, result = case("deterministic_cash", "sinh-match-domain8")
    assert result["concentration_scale"] == result["configured_concentration_scale"] == .05
    assert result["spacing_match"]["reference_concentration_scale"] == .05
    assert result["max_deterministic_reference_error"] < 1e-10
    assert_complete(report, result)


def test_unknown_grid_is_an_explicit_cli_error(binary):
    completed = subprocess.run([str(binary), "--full", "--grid", "unknown"],
                               capture_output=True, text=True, timeout=10, check=False)
    assert completed.returncode != 0
    assert "fatal_error" in json.loads(completed.stderr)
    assert not completed.stdout


def test_fixed_domain_spatial_control_keeps_exact_time_subdivisions(case):
    cases = [case("high_vol", f"sinh{nodes}-domain8-clock4800") for nodes in (129, 161, 193)]
    reference = cases[0][1]
    for nodes, (report, result) in zip((129, 161, 193), cases):
        assert result["model_status"] == result["pricing_status"] == "ok"
        assert result["resolved_nodes"] == result["requested_nodes"] == nodes
        assert result["upper_multiple"] == reference["upper_multiple"] == 8
        assert result["concentration_scale"] == reference["concentration_scale"]
        assert result["resolved_time_intervals"] == reference["resolved_time_intervals"]
        assert result["resolved_time_intervals"] == [{
            "interval_index": 0, "calendar_clock_minimum_steps": 4800,
            "variance_clock_minimum_steps": 4800, "resolved_diffusion_steps": 4800,
            "exceeds_clock_minimum": False,
        }]
        assert result["resolved_model_controls"] == reference["resolved_model_controls"]
        assert result["model"]["diffusion_steps"] == 4800
        assert result["model"]["stored_rows"] == nodes * 4800
        assert result["model"]["stored_rows"] <= result["model_limits"]["max_stored_rows"] == 1_000_000
        assert_complete(report, result)


@pytest.mark.parametrize("factor", [2, 3])
def test_fixed_domain_time_control_keeps_grid_and_refines_cfl(case, factor):
    _, reference = case("high_vol", "sinh129-variance-domain8")
    report, result = case("high_vol", f"sinh129-domain8-time{factor}")
    assert result["model_status"] == result["pricing_status"] == "ok"
    assert result["resolved_state_grid"] == reference["resolved_state_grid"]
    assert result["grid_diagnostics"] == reference["grid_diagnostics"]
    assert result["model_limits"] == reference["model_limits"]
    assert result["resolved_model_controls"]["cfl_fraction"] == .9 / factor
    before = reference["resolved_time_intervals"][0]["resolved_diffusion_steps"]
    after = result["resolved_time_intervals"][0]["resolved_diffusion_steps"]
    assert factor * before - (factor - 1) <= after <= factor * before
    assert result["resolved_time_intervals"][0]["exceeds_clock_minimum"] is True
    assert_complete(report, result)


def test_interval_time_provenance_reconciles_cash_and_paused_clocks(case):
    for fixture in ("dated_cash", "clock_pause"):
        _, result = case(fixture, "sinh161-variance-domain8")
        assert result["model_status"] == "ok"
        intervals = result["resolved_time_intervals"]
        assert len(intervals) == len(result["intervals"])
        assert sum(i["resolved_diffusion_steps"] for i in intervals) == result["model"]["diffusion_steps"]
        for index, item in enumerate(intervals):
            assert item["interval_index"] == index
            minimum = max(1, item["calendar_clock_minimum_steps"], item["variance_clock_minimum_steps"])
            assert item["resolved_diffusion_steps"] >= minimum
            assert item["exceeds_clock_minimum"] == (item["resolved_diffusion_steps"] > minimum)
        if fixture == "clock_pause":
            assert intervals[1]["variance_clock_minimum_steps"] == 0


@pytest.mark.parametrize("fixture", ["high_vol", "dated_cash", "clock_pause"])
def test_resolvent_reports_method_work_and_exact_clock_subdivisions(case, fixture):
    report, result = case(fixture, "sinh129-variance", "variance-resolvent")
    assert result["model_status"] == result["pricing_status"] == "ok"
    assert report["diffusion_method"] == result["diffusion_method"] == "variance-resolvent"
    model = result["model"]
    assert model["diffusion_method"] == "variance-resolvent"
    assert model["resolvent_steps"] == model["diffusion_steps"] > 0
    assert model["transition_work"] == model["stored_rows"] + model["states"] * model["resolvent_steps"]
    assert model["physical_resolvent_factors"] > 0
    assert model["resolvent_factor_blocks"] > 0
    assert model["max_forward_mass_error"] < 1e-10
    assert model["max_mean_error"] < 1e-12
    assert result["model_limits"]["max_transition_work"] == 8_000_000
    assert sum(i["resolved_diffusion_steps"] for i in result["resolved_time_intervals"]) == model["diffusion_steps"]
    for item in result["resolved_time_intervals"]:
        assert item["resolved_diffusion_steps"] == max(1, item["calendar_clock_minimum_steps"], item["variance_clock_minimum_steps"])
        assert item["exceeds_clock_minimum"] is False
    assert_complete(report, result)


def test_explicit_method_flag_preserves_default_prices(case):
    _, before = case("high_vol", "sinh129-variance")
    report, after = case("high_vol", "sinh129-variance", "explicit-euler")
    assert report["diffusion_method"] == after["diffusion_method"] == "explicit-euler"
    for key in ("prices", "model", "resolved_time_intervals", "resolved_model_controls"):
        assert before[key] == after[key]
    assert after["model"]["resolvent_steps"] == 0
    assert after["model"]["transition_work"] == after["model"]["stored_rows"]


@pytest.mark.parametrize("args", [
    ["--diffusion-method"], ["--diffusion-method", "unknown"],
    ["--diffusion-method", "explicit-euler", "--diffusion-method", "variance-resolvent"],
])
def test_invalid_or_repeated_diffusion_method_is_cli_error(binary, args):
    completed = subprocess.run([str(binary), *args], capture_output=True, text=True,
                               timeout=10, check=False)
    assert completed.returncode != 0
    assert "fatal_error" in json.loads(completed.stderr)
    assert not completed.stdout
