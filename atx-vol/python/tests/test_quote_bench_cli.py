"""Post-implementation native boundary checks; no market-data fixture required."""
from __future__ import annotations

import csv
import datetime as dt
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess

import pytest


@pytest.fixture
def binary() -> Path:
    configured = os.environ.get("ATX_QUOTE_BENCH_BINARY")
    if not configured:
        pytest.skip("ATX_QUOTE_BENCH_BINARY is not configured")
    path = Path(configured)
    assert path.is_file()
    return path


def make_quotes(path: Path) -> list[float]:
    # Independent closed-form European values: with zero rate/yield the
    # American call has no early-exercise premium.
    values = []
    def phi(x: float) -> float:
        return 0.5 * math.erfc(-x / math.sqrt(2.0))

    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow([
            "row_id", "chain_id", "strike", "is_call", "spot", "bid", "ask",
            "years", "years_calendar", "rate", "sdiv", "ddiv_pv",
            "input_status", "exercise", "underlier", "root", "expiry_days", "snapshot_ns",
        ])
        for row, strike in enumerate(range(75, 126, 2)):
            sigma_t = 0.3 * math.sqrt(0.5)
            d1 = math.log(100.0 / strike) / sigma_t + 0.5 * sigma_t
            d2 = d1 - sigma_t
            price = 100.0 * phi(d1) - strike * phi(d2)
            values.append(price)
            writer.writerow([
                row, 17, strike, 1, 100, price - 0.001, price + 0.001,
                0.5, 0.5, 0, 0, 0, "ok", "American", "AAA", "AAA", 21000, 1,
            ])
    return values


def invoke(binary: Path, source: Path, output: Path, *extra: str):
    return subprocess.run(
        [str(binary), "--input", str(source), "--output", str(output), *extra],
        capture_output=True, text=True, timeout=60, check=False,
    )


def test_native_fit_recovers_independent_flat_call_values(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    expected = make_quotes(source)
    result = invoke(binary, source, output, "--curve", "essvi")
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == len(expected)
    assert all(row["status"] == "ok" for row in rows)
    assert max(abs(float(row["predicted_vol"]) - 0.3) for row in rows) < 0.001
    assert max(abs(float(row["predicted_price"]) - value)
               for row, value in zip(rows, expected)) < 0.03
    first = output.read_bytes()
    assert invoke(binary, source, output, "--curve", "essvi").returncode == 0
    assert output.read_bytes() == first
    report = json.loads(result.stdout)
    assert report["served_rows"] == len(expected)
    assert report["fitted_chains"] == 1
    assert report["policy_mode"] == "fixed-curve"
    assert all(row["policy_source"] == "fixed-curve" for row in rows)


def test_native_rejects_label_columns(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    lines = source.read_text().splitlines()
    source.write_text(lines[0] + ",srVol\n" +
                      "\n".join(line + ",0.3" for line in lines[1:]) + "\n")
    result = invoke(binary, source, output)
    assert result.returncode != 0
    assert "quote-only input schema" in result.stderr
    assert not output.exists()


@pytest.mark.parametrize(("mode", "api_name"), [
    ("funding-pv", "aggregate_funding_pv_approximation"),
    ("escrowed-pv", "aggregate_pv_approximation"),
    ("none", "continuous_yield"),
])
def test_native_records_explicit_dividend_convention(binary, tmp_path, mode, api_name):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    result = invoke(binary, source, output, "--curve", "essvi", "--dividends", mode)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["dividend_model"] == mode
    assert report["policy_definition"]["profiles"][0]["dividend_model"] == api_name
    if mode != "none":
        assert "approximation without ex-dates" in report["dividend_convention_assumption"]
    assert report["served_rows"] == 26


def test_native_refuses_ambiguous_aggregate_pv_mode(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    result = invoke(binary, source, output, "--dividends", "aggregate-pv")
    assert result.returncode != 0
    assert "funding-pv, escrowed-pv or none" in result.stderr
    assert not output.exists()


def test_native_funding_pv_accepts_positive_prepaid_above_spot(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    with source.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    prepaid = 100.0 * math.exp(2.0 * 0.5) - 150.0
    sigma_t = 0.3 * math.sqrt(0.5)
    expected = []
    for index, row in enumerate(rows):
        strike = prepaid * (0.75 + index * 0.02)
        d1 = math.log(prepaid / strike) / sigma_t + 0.5 * sigma_t
        d2 = d1 - sigma_t
        # Converted yield is negative, r=0: these American calls have no
        # early-exercise premium. Direct formula supplies an independent check.
        price = prepaid * 0.5 * math.erfc(-d1 / math.sqrt(2.0)) - (
            strike * 0.5 * math.erfc(-d2 / math.sqrt(2.0)))
        expected.append(price)
        row.update(strike=str(strike), sdiv="-2", ddiv_pv="150",
                   bid=str(price - 0.001), ask=str(price + 0.001))
    with source.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    result = invoke(binary, source, output, "--curve", "essvi")
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        predictions = list(csv.DictReader(stream))
    assert all(row["status"] == "ok" for row in predictions)
    assert max(abs(float(row["predicted_vol"]) - 0.3) for row in predictions) < 0.001
    assert max(abs(float(row["predicted_price"]) - price)
               for row, price in zip(predictions, expected)) < 0.03
    assert json.loads(result.stdout)["dividend_model"] == "funding-pv"
    for mode in ("escrowed-pv", "none"):
        result = invoke(binary, source, output, "--curve", "essvi", "--dividends", mode)
        assert result.returncode == 0, result.stderr
        with output.open(newline="") as stream:
            assert all(row["status"] == "invalid_market" for row in csv.DictReader(stream))


def test_native_rejects_duplicate_ids_and_oversized_cohort(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    result = invoke(binary, source, output, "--max-rows", "2")
    assert result.returncode != 0
    assert "exceeds --max-rows" in result.stderr
    lines = source.read_text().splitlines()
    source.write_text("\n".join(lines + [lines[1]]) + "\n")
    result = invoke(binary, source, output)
    assert result.returncode != 0
    assert "Duplicate row_id" in result.stderr


def test_native_preserves_source_when_report_aliases_input(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    original = source.read_bytes()
    result = invoke(binary, source, output, "--report", str(source))
    assert result.returncode != 0
    assert source.read_bytes() == original


def test_native_preserves_hardlinked_source(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "alias.csv"
    make_quotes(source)
    original = source.read_bytes()
    os.link(source, output)
    result = invoke(binary, source, output)
    assert result.returncode != 0
    assert source.read_bytes() == original


def test_native_rejects_mixed_expiry_chain(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    with source.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    rows[-1]["years"] = "0.6"
    with source.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        assert all(row["status"] == "inconsistent_chain" for row in csv.DictReader(stream))


def test_native_counts_adjusted_deliverables_as_unsupported(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    with source.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    for row in rows:
        row["root"] = "AAA1"
    with source.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert all(row["status"] == "unsupported_deliverable" for row in rows)
    assert all(row["predicted_price"] == "" for row in rows)


@pytest.mark.parametrize("mode", ["auto", "fixed", "override"])
def test_native_refuses_calendar_crossing_for_whole_affected_slices(binary, tmp_path, mode):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    with source.open(newline="") as stream:
        first = list(csv.DictReader(stream))
    later = []
    for index, original in enumerate(first):
        row = dict(original)
        strike = float(row["strike"])
        # A longer-dated call with smaller TOTAL variance costs less; both
        # individual slices are convex, but their calendar combination is not.
        d1 = math.log(100.0 / strike) / 0.1 + 0.05
        d2 = d1 - 0.1
        price = (100.0 * 0.5 * math.erfc(-d1 / math.sqrt(2.0))
                 - strike * 0.5 * math.erfc(-d2 / math.sqrt(2.0)))
        row.update(row_id=str(index + len(first)), chain_id="18", years="1.0",
                   years_calendar="1.0", expiry_days="21183",
                   bid=str(max(0.000001, price - 0.000001)), ask=str(price + 0.000001))
        later.append(row)
    with source.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(first[0]))
        writer.writeheader()
        writer.writerows(first + later)
    extra = []
    if mode == "fixed":
        extra = ["--curve", "essvi"]
    elif mode == "override":
        policy = tmp_path / "policy.json"
        policy.write_text(json.dumps({
            "schema_version": 1,
            "symbol_overrides": [{"symbol": "AAA", "profile": "ordinary"}],
        }))
        extra = ["--policy", str(policy)]
    result = invoke(binary, source, output, *extra)
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert all(row["status"] == "calendar_arbitrage" for row in rows)
    assert all(row["predicted_price"] == "" for row in rows)
    report = json.loads(result.stdout)
    assert report["calendar_rejected_chains"] == 2
    assert report["served_rows"] == 0


def test_native_canonicalizes_snapshot_identity_and_refuses_duplicate_expiry(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    with source.open(newline="") as stream:
        first = list(csv.DictReader(stream))
    second = []
    for index, original in enumerate(first):
        row = dict(original)
        row.update(row_id=str(index + len(first)), chain_id="18",
                   snapshot_ns="01", expiry_days="021000")
        second.append(row)
    with source.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(first[0]))
        writer.writeheader()
        writer.writerows(first + second)
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        assert all(row["status"] == "ambiguous_expiry" for row in csv.DictReader(stream))
    assert json.loads(result.stdout)["ambiguous_expiry_chains"] == 2


def read_quotes(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def write_quotes(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def test_native_routes_once_per_exact_symbol_root_snapshot(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    first = read_quotes(source)
    later = [dict(row, chain_id="18", years="1.0", years_calendar="1.0", expiry_days="21183")
             for row in first[11:15]]
    dense = [dict(row, chain_id="19", underlier="BBB", root="BBB") for row in first]
    sparse = [dict(row, chain_id="20", snapshot_ns="2", bid="1", ask="3")
              for row in first[11:15]]
    rows = first + later + dense + sparse
    for index, row in enumerate(rows):
        row["row_id"] = str(index)
    write_quotes(source, rows)
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["policy_mode"] == "auto"
    decisions = {(item["underlier"], item["snapshot_ns"]): item
                 for item in report["policy_decisions"]}
    assert decisions["AAA", "1"]["eligible_chain_ids"] == [17, 18]
    assert decisions["AAA", "1"]["resolved_profile"] == "ordinary"
    assert decisions["BBB", "1"]["resolved_profile"] == "dense"
    assert decisions["AAA", "2"]["resolved_profile"] == "sparse"
    assert decisions["AAA", "1"]["features"]["median_near_money_strikes"] == 15
    predictions = read_quotes(output)
    assert all(row["resolved_profile"] == "ordinary" for row in predictions[:30])
    assert all(row["policy_source"] == "raw-liquidity" for row in predictions)
    assert report["per_profile_counts"]["ordinary"]["attempted_chains"] == 2
    assert sum(counts["rows"] for counts in report["per_profile_counts"].values()) == len(rows)
    assert sum(counts["served_rows"] for counts in report["per_profile_counts"].values()) == report["served_rows"]


def test_native_exact_override_and_expanded_custom_profile(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    policy = tmp_path / "policy.json"
    policy.write_text(json.dumps({
        "schema_version": 1,
        "profiles": [{"name": name, "base": name} for name in ["dense", "ordinary", "sparse"]] + [
            {"name": "custom-smile", "base": "ordinary", "curve": "svi", "min_quotes": 8,
             "quality": {"max_weight": 321}, "parametric": {"huber_k": 1.25},
             "spline": {"grid": [-0.7, -0.2, 0.1, 0.8]}}],
        "symbol_overrides": [{"symbol": "AAA", "profile": "custom-smile"}],
    }))
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode == 0, result.stderr
    rows = read_quotes(output)
    assert all(row["resolved_profile"] == "custom-smile" for row in rows)
    assert all(row["policy_source"] == "exact-symbol-override" for row in rows)
    assert all(row["actual_curve"] == "svi" for row in rows)
    report = json.loads(result.stdout)
    definition = next(p for p in report["policy_definition"]["profiles"] if p["name"] == "custom-smile")
    assert definition["quality"]["max_weight"] == 321
    assert definition["parametric"]["huber_k"] == 1.25
    assert definition["parametric"]["validate_no_arb"] is True
    assert definition["dividend_model"] == "aggregate_funding_pv_approximation"
    assert definition["spline"]["grid"] == [-0.7, -0.2, 0.1, 0.8]
    assert report["policy_definition_format"] == "expanded-runtime-configuration-v1"
    assert all(row["configured_curve"] == "svi" for row in rows)
    document = json.loads(policy.read_text())
    document["symbol_overrides"][0]["symbol"] = "aaa"
    policy.write_text(json.dumps(document))
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode == 0, result.stderr
    assert all(row["policy_source"] == "raw-liquidity" for row in read_quotes(output))


def test_native_implied_forward_profile_records_every_estimator_control(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)  # Calls only: the estimator has no pairs and refuses.
    policy = tmp_path / "policy.json"
    policy.write_text(json.dumps({
        "schema_version": 1,
        "profiles": [{"name": name, "base": name, "carry_mode": "implied-forward-parity",
                      "implied_forward": {"min_pairs": 3, "al_preset": "bulk"}}
                     for name in ["dense", "ordinary", "sparse"]],
    }))
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    definition = next(p for p in report["policy_definition"]["profiles"] if p["name"] == "dense")
    assert definition["carry_mode"] == "implied_forward"
    carry = definition["implied_forward"]
    assert carry["method"] == "parity" and carry["al_preset"] == "bulk"
    assert carry["max_outlier_fraction"] == 0.34
    assert "al_opts" not in carry  # Named presets need no field-by-field expansion.
    resolution, = report["carry_resolutions"]
    assert resolution["fallback_cause"] == "estimate_refused"
    assert report["carry_status_counts"] == {"fallback:insufficient_pairs": 1}


@pytest.mark.parametrize("document", [
    # An implied_forward block under the resolved supplied carry would be inert.
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","implied_forward":{"min_pairs":3}}]}',
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","carry_mode":"supplied",'
    '"implied_forward":{"min_pairs":3}}]}',
    '{"schema_version":1,"srVol":0.3}',
    '{"schema_version":1,"schema_version":1}',
    '{"schema_version":1,"regimes":{"dense_profile":"dense","dense_profile":"dense"}}',
    '{"schema_version":1,"regimes":{"near_money_log_band":1e999}}',
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","quality":{"max_weight":true}}]}',
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","parametric":{"validate_no_arb":false}}]}',
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","convex":{"node_cap":1000000}}]}',
    '{"schema_version":1,"symbol_overrides":[{"symbol":"AAA","profile":"missing"}]}',
    '{"schema_version":1,"symbol_overrides":[{"symbol":"AAA","profile":"dense"},{"symbol":"AAA","profile":"sparse"}]}',
    '{"schema_version":1,"regimes":{"dense_max_bidless_fraction":1.5}}',
    '{"schema_version":1,"profiles":[{"name":"bad,name","base":"dense"}]}',
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","spline":{"grid":[0,1,1,2]}}]}',
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","parametric":{"max_inner_iter":1.5}}]}',
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","min_quotes":18446744073709551615}]}',
    '{"schema_version":1,"profiles":[{"name":"dense","base":"dense","parametric":{"lee_bound_project":false}}]}',
    '{"schema_version":1,"extra":' + '[' * 20 + '0' + ']' * 20 + '}',
])
def test_native_rejects_invalid_policy_before_output(binary, tmp_path, document):
    source, output, policy = tmp_path / "quotes.csv", tmp_path / "predictions.csv", tmp_path / "policy.json"
    make_quotes(source)
    policy.write_text(document)
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode != 0
    assert not output.exists()


@pytest.mark.parametrize("extra", [
    ["--policy", "auto", "--curve", "essvi"],
    ["--curve", "essvi", "--curve", "svi"],
    ["--min-quotes", "4"],
    ["--policy", "auto", "--max-spread-vol", "0.5"],
])
def test_native_refuses_ambiguous_policy_overrides(binary, tmp_path, extra):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    result = invoke(binary, source, output, *extra)
    assert result.returncode != 0
    assert not output.exists()


def test_native_preserves_policy_when_outputs_alias_it(binary, tmp_path):
    source, output, policy = tmp_path / "quotes.csv", tmp_path / "predictions.csv", tmp_path / "policy.json"
    make_quotes(source)
    original = b'{"schema_version":1}'
    policy.write_bytes(original)
    assert invoke(binary, source, policy, "--policy", str(policy)).returncode != 0
    assert policy.read_bytes() == original
    assert invoke(binary, source, output, "--policy", str(policy), "--report", str(policy)).returncode != 0
    assert policy.read_bytes() == original
    alias = tmp_path / "policy-hardlink.json"
    os.link(policy, alias)
    assert invoke(binary, source, alias, "--policy", str(policy)).returncode != 0
    assert policy.read_bytes() == original
    policy.write_bytes(b" " * (1024 * 1024 + 1))
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode != 0
    assert "1 MiB" in result.stderr


def test_native_duplicate_expiry_does_not_inflate_liquidity(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    first = read_quotes(source)
    duplicate = [dict(row, chain_id="18") for row in first]
    valid = [dict(row, chain_id="19", years="1", years_calendar="1", expiry_days="21183")
             for row in first[11:15]]
    rows = first + duplicate + valid
    for index, row in enumerate(rows):
        row["row_id"] = str(index)
    write_quotes(source, rows)
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    decision, = json.loads(result.stdout)["policy_decisions"]
    assert decision["eligible_chain_ids"] == [19]
    assert decision["ambiguous_chain_ids"] == [17, 18]
    assert decision["features"]["unique_contracts"] == 4
    assert decision["resolved_profile"] == "sparse"
    assert all(row["status"] == "ambiguous_expiry" for row in read_quotes(output)[:52])


def test_native_reports_selected_family_separately_when_no_curve_exists(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    write_quotes(source, read_quotes(source)[12:14])
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    predictions = read_quotes(output)
    assert all(row["resolved_profile"] == "sparse" for row in predictions)
    assert all(row["configured_curve"] == "essvi" for row in predictions)
    assert all(row["actual_curve"] == "" for row in predictions)
    counts = json.loads(result.stdout)["per_profile_counts"]["sparse"]
    assert counts["fitted_chains"] == 0
    assert counts["served_rows"] == 0
    assert counts["actual_curve_rows"] == {}
    assert counts["no_curve_rows"] == 2
    assert sum(counts["row_status_counts"].values()) == 2


def test_native_help_after_input_does_not_start_run(binary, tmp_path):
    source, output = tmp_path / "missing.csv", tmp_path / "predictions.csv"
    result = invoke(binary, source, output, "--help")
    assert result.returncode == 0, result.stderr
    assert "Defaults: --policy auto --dividends funding-pv" in result.stdout
    assert not output.exists()


def write_policy(path: Path, document: dict) -> Path:
    path.write_text(json.dumps(document))
    return path


def test_native_graded_admission_adds_grades_without_changing_point_prices(binary, tmp_path):
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    static_output, graded_output = tmp_path / "static.csv", tmp_path / "graded.csv"
    static = invoke(binary, source, static_output)
    assert static.returncode == 0, static.stderr
    policy = write_policy(tmp_path / "graded.json",
                          {"schema_version": 1, "admission": "graded"})
    graded = invoke(binary, source, graded_output, "--policy", str(policy))
    assert graded.returncode == 0, graded.stderr
    with static_output.open(newline="") as stream:
        static_reader = csv.DictReader(stream)
        static_rows = list(static_reader)
        assert "grade" not in static_reader.fieldnames
    with graded_output.open(newline="") as stream:
        graded_reader = csv.DictReader(stream)
        graded_rows = list(graded_reader)
        assert graded_reader.fieldnames[-1] == "grade"
    assert len(static_rows) == len(graded_rows)
    assert all(row["status"] == "ok" for row in static_rows)
    for before, after in zip(static_rows, graded_rows):
        if before["status"] == "ok":
            # A point-fitted expiry is served unchanged and graded A.
            assert after["predicted_price"] == before["predicted_price"]
            assert after["grade"] == "A"
        if after["status"] == "ok":
            assert after["grade"] in {"A", "B", "C"}
        else:
            assert after["grade"] == ""
    report = json.loads(graded.stdout)
    assert report["admission"] == "graded"
    assert sum(report["graded_stats"]["served_rows"].values()) == report["served_rows"]
    assert report["policy_definition"]["admission"] == "graded"
    assert "admission" not in json.loads(static.stdout)


def make_wide_snapshot(path: Path, expiries: int) -> None:
    # One snapshot with `expiries` flat-volatility call expiries (zero carry,
    # so American calls equal European values).
    def phi(x: float) -> float:
        return 0.5 * math.erfc(-x / math.sqrt(2.0))

    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow([
            "row_id", "chain_id", "strike", "is_call", "spot", "bid", "ask",
            "years", "years_calendar", "rate", "sdiv", "ddiv_pv",
            "input_status", "exercise", "underlier", "root", "expiry_days", "snapshot_ns",
        ])
        row = 0
        for expiry in range(expiries):
            years = 0.1 + 0.01 * expiry
            for strike in range(75, 126, 2):
                sigma_t = 0.3 * math.sqrt(years)
                d1 = math.log(100.0 / strike) / sigma_t + 0.5 * sigma_t
                price = 100.0 * phi(d1) - strike * phi(d1 - sigma_t)
                writer.writerow([
                    row, 1000 + expiry, strike, 1, 100, max(price - 0.001, 0.0), price + 0.001,
                    years, years, 0, 0, 0, "ok", "American", "AAA", "AAA",
                    round(years * 365.25 * 1000), 1,
                ])
                row += 1


def test_native_graded_snapshot_above_expiry_cap_falls_back_to_static(binary, tmp_path):
    source = tmp_path / "quotes.csv"
    make_wide_snapshot(source, 129)  # kQuoteSurfaceMaxExpiries is 128
    static_output, graded_output = tmp_path / "static.csv", tmp_path / "graded.csv"
    static = invoke(binary, source, static_output)
    assert static.returncode == 0, static.stderr
    policy = write_policy(tmp_path / "graded.json",
                          {"schema_version": 1, "admission": "graded"})
    graded = invoke(binary, source, graded_output, "--policy", str(policy))
    assert graded.returncode == 0, graded.stderr
    with static_output.open(newline="") as stream:
        static_rows = list(csv.DictReader(stream))
    with graded_output.open(newline="") as stream:
        graded_rows = list(csv.DictReader(stream))
    served = [row for row in static_rows if row["status"] == "ok"]
    assert len(served) > 0.9 * len(static_rows)
    for before, after in zip(static_rows, graded_rows):
        assert after["status"] == before["status"]
        assert after["predicted_price"] == before["predicted_price"]
        assert after["grade"] == ("A" if before["status"] == "ok" else "")
    report = json.loads(graded.stdout)
    assert report["graded_stats"]["static_fallback"] == {"snapshots": 1, "chains": 129}
    assert report["served_rows"] == len(served)


TIMING_BUCKETS = ("deam_us", "solve_us", "project_us", "price_us", "certify_us", "calendar_us",
                  "other_us")


def report_quantile(values: list[float], p: float) -> float:
    # quote_bench_main.cpp quantile(): linear interpolation between order statistics.
    ordered = sorted(values)
    location = p * (len(ordered) - 1)
    lo = int(location)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (location - lo) * (ordered[hi] - ordered[lo])


def test_native_timing_detail_reports_per_chain_counters_and_real_latency_percentiles(
        binary, tmp_path):
    source = tmp_path / "quotes.csv"
    make_wide_snapshot(source, 4)
    policy = write_policy(tmp_path / "graded.json", {"schema_version": 1, "admission": "graded"})
    plain_output, timed_output = tmp_path / "plain.csv", tmp_path / "timed.csv"
    plain = invoke(binary, source, plain_output, "--policy", str(policy))
    assert plain.returncode == 0, plain.stderr
    timed = invoke(binary, source, timed_output, "--policy", str(policy), "--timing-detail")
    assert timed.returncode == 0, timed.stderr
    # Opt-in: the predictions are byte-identical and the plain report has no timing keys.
    assert timed_output.read_bytes() == plain_output.read_bytes()
    plain_report, timed_report = json.loads(plain.stdout), json.loads(timed.stdout)
    assert "chain_latency_scope" not in plain_report
    assert "timing_detail" not in plain_report["graded_stats"]
    assert all("timing" not in entry for entry in plain_report["graded_stats"]["slices"])
    slices = timed_report["graded_stats"]["slices"]
    assert len(slices) == 4
    walls = []
    for entry in slices:
        timing = entry["timing"]
        assert timing["rung_attempts"] >= 1
        assert timing["certification_passes"] >= 1
        assert timing["wall_us"] == sum(timing[key] for key in TIMING_BUCKETS)
        assert timing["al_price_calls"] == sum(timing["al_by_bucket"].values())
        # The repair-rung cross-cut is reported, and idle without graded.repair channel.
        assert (timing["repair_rung_visits"], timing["repair_rung_us"]) == (0, 0)
        walls.append(timing["wall_us"])
    # Real per-chain percentiles: each graded chain's own fit wall, not the surface
    # wall divided by its expiries (which made p50 = p95 = p99 for one snapshot).
    for p, name in ((0.50, "p50"), (0.95, "p95"), (0.99, "p99")):
        assert timed_report[f"chain_latency_us_{name}"] == pytest.approx(
            report_quantile(walls, p), rel=1e-12, abs=1e-9)
    detail = timed_report["graded_stats"]["timing_detail"]
    assert detail["surfaces"] == 1
    assert detail["chain_wall_us"] == sum(walls)
    # Chain time is a subset of the surface wall (up to per-bucket microsecond rounding).
    assert detail["surface_wall_us"] + 4 * len(TIMING_BUCKETS) >= sum(walls)
    refused = invoke(binary, source, tmp_path / "markov.csv", "--model", "common-markov",
                     "--timing-detail")
    assert refused.returncode != 0
    assert "--timing-detail requires --model quote-slice" in refused.stderr


def test_native_graded_options_are_strict(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    for document, message in (
            ({"schema_version": 1, "admission": "loose"}, "static or graded"),
            ({"schema_version": 1, "graded": {"term_prior_weight": 1}}, "require admission graded"),
            ({"schema_version": 1, "admission": "graded", "graded": {"bogus": 1}},
             "Unknown policy key"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"calendar_retry_factor": 0.5}}, "outside supported bounds"),
            ({"schema_version": 1, "admission": "graded", "profiles": [
                {"name": "dense", "base": "dense", "carry_mode": "implied-forward"}]},
             "requires supplied carry"),
            ({"schema_version": 1, "admission": "graded", "profiles": [
                {"name": "dense", "base": "dense", "dividends": "inferred-schedule"}]},
             "requires dividends aggregate"),
            ({"schema_version": 1, "admission": "graded", "profiles": [
                {"name": "dense", "base": "dense", "dividends": "inferred-schedule-exact"}]},
             "requires dividends aggregate"),
            # Opt-in channel repair and channel dominance keys.
            ({"schema_version": 1, "admission": "graded", "graded": {"repair": "lower"}},
             "must be off or channel"),
            ({"schema_version": 1, "admission": "graded", "graded": {"repair_max_nodes": 8}},
             "budgets require repair channel"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"repair": "off", "repair_max_vol": 0.02}},
             "budgets require repair channel"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"repair": "channel", "repair_max_nodes": 0}},
             "outside supported bounds"),
            ({"schema_version": 1, "admission": "graded", "profiles": [
                {"name": "dense", "base": "dense",
                 "channel": {"point_max_outside_fraction": 1.5}}]},
             "outside supported bounds"),
            ({"schema_version": 1, "profiles": [
                {"name": "dense", "base": "dense",
                 "channel": {"point_max_outside_fraction": 0.05}}]},
             "point_max_outside_fraction requires admission graded"),
            ({"schema_version": 1, "admission": "graded", "profiles": [
                {"name": "dense", "base": "dense",
                 "channel": {"point_min_outside_quotes": 4}}]},
             "point_min_outside_quotes requires point_max_outside_fraction"),
            ({"schema_version": 1, "admission": "graded", "profiles": [
                {"name": "dense", "base": "dense",
                 "channel": {"point_max_outside_fraction": 0.05,
                             "point_min_outside_quotes": 0}}]},
             "outside supported bounds"),
            # Opt-in per-quote de-Am IV cache keys (lane L2a).
            ({"schema_version": 1, "admission": "graded", "graded": {"fit_space": "variance"}},
             "fit_space must be price or vol"),
            ({"schema_version": 1, "admission": "graded", "graded": {"deam_cache": True}},
             "require fit_space vol"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "price", "deam_profile": "fast"}},
             "require fit_space vol"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "quick"}},
             "deam_profile must be accurate or fast"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_cache": 1}},
             "deam_cache must be true or false")):
        policy = write_policy(tmp_path / "policy.json", document)
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode != 0, document
        assert message in result.stderr, (document, result.stderr)


def test_native_graded_repair_and_dominance_keys_are_opt_in(binary, tmp_path):
    # On clean quotes every point fit certifies and meets its quotes, so the
    # opt-in rungs never run: the predictions are byte-identical to graded
    # without them, and only the opt-in provenance is added to the report.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    plain_output, keyed_output = tmp_path / "plain.csv", tmp_path / "keyed.csv"
    plain_policy = write_policy(tmp_path / "plain.json",
                                {"schema_version": 1, "admission": "graded"})
    plain = invoke(binary, source, plain_output, "--policy", str(plain_policy))
    assert plain.returncode == 0, plain.stderr
    keyed_policy = write_policy(tmp_path / "keyed.json", {
        "schema_version": 1, "admission": "graded",
        "graded": {"repair": "channel", "repair_max_nodes": 32, "repair_max_vol": 0.01},
        "profiles": [{"name": name, "base": name,
                      "channel": {"point_max_outside_fraction": 1.0,
                                  "point_min_outside_quotes": 4}}
                     for name in ("dense", "ordinary", "sparse")]})
    keyed = invoke(binary, source, keyed_output, "--policy", str(keyed_policy))
    assert keyed.returncode == 0, keyed.stderr
    assert keyed_output.read_bytes() == plain_output.read_bytes()
    plain_report, report = json.loads(plain.stdout), json.loads(keyed.stdout)
    graded = report["policy_definition"]["graded"]
    assert graded["repair"] == "channel"
    assert graded["repair_max_nodes"] == 32
    assert graded["repair_max_vol"] == 0.01
    assert graded["repair_immaterial_price"] == 5.0e-4
    assert "repair" not in plain_report["policy_definition"]["graded"]
    for profile in report["policy_definition"]["profiles"]:
        assert profile["channel"]["point_max_outside_fraction"] == 1.0
        assert profile["channel"]["point_min_outside_quotes"] == 4
    for profile in plain_report["policy_definition"]["profiles"]:
        assert "point_min_outside_quotes" not in profile["channel"]
    assert "repair channel" in report["graded_semantics"]
    assert "point_max_outside_fraction" in report["graded_semantics"]
    assert "repair channel" not in plain_report["graded_semantics"]
    slices = report["graded_stats"]["slices"]
    assert slices
    for entry in slices:
        assert entry["repair"]["outcome"] == "not_attempted"
        assert entry["repair_rung"] is False
        assert entry["calendar_channel_repaired"] is False
        assert entry["point_gated"] is False
        assert entry["point_fallback"] is False
        assert entry["point_outside_quotes"] >= 0
    for entry in plain_report["graded_stats"]["slices"]:
        assert "repair" not in entry and "point_gated" not in entry


def test_native_accurate_iv_cache_is_byte_identical_and_opt_in(binary, tmp_path):
    # The accurate-kernel cache serves exactly the price-channel predictions;
    # only the opt-in provenance is added to the report.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    plain_output, cached_output = tmp_path / "plain.csv", tmp_path / "cached.csv"
    plain_policy = write_policy(tmp_path / "plain.json",
                                {"schema_version": 1, "admission": "graded"})
    plain = invoke(binary, source, plain_output, "--policy", str(plain_policy))
    assert plain.returncode == 0, plain.stderr
    cached_policy = write_policy(tmp_path / "cached.json", {
        "schema_version": 1, "admission": "graded",
        "graded": {"fit_space": "vol", "deam_profile": "accurate", "deam_cache": True}})
    cached = invoke(binary, source, cached_output, "--policy", str(cached_policy))
    assert cached.returncode == 0, cached.stderr
    assert cached_output.read_bytes() == plain_output.read_bytes()
    graded = json.loads(cached.stdout)["policy_definition"]["graded"]
    assert (graded["fit_space"], graded["deam_profile"], graded["deam_cache"]) == (
        "vol", "accurate", True)
    plain_graded = json.loads(plain.stdout)["policy_definition"]["graded"]
    assert not {"fit_space", "deam_profile", "deam_cache"} & set(plain_graded)


PROFILE_NAMES = ("dense", "ordinary", "sparse")


def test_native_ladder_and_price_only_keys_are_strict(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    knot = [{"name": name, "base": name, "curve": "knot-spline"} for name in PROFILE_NAMES]
    gated = [{"name": name, "base": name, "channel": {"point_max_outside_fraction": 0.05}}
             for name in PROFILE_NAMES]
    graded = {"schema_version": 1, "admission": "graded"}
    for document, message in (
            ({"schema_version": 1, "price_only": "yes"}, "price_only must be true or false"),
            ({"schema_version": 1, "price_only": 1}, "price_only must be true or false"),
            ({**graded, "price_only": None}, "price_only must be true or false"),
            ({"schema_version": 1, "graded": {"ladder": "knot-first"}},
             "require admission graded"),
            ({**graded, "graded": {"ladder": "point-first"}}, "must be off or knot-first"),
            ({**graded, "graded": {"ladder_max_outside_fraction": 0.1}},
             "containment keys require ladder knot-first"),
            ({**graded, "graded": {"ladder": "off", "ladder_min_outside_quotes": 4}},
             "containment keys require ladder knot-first"),
            ({**graded, "graded": {"ladder": "off", "ladder_channel_max_outside_fraction": 1}},
             "containment keys require ladder knot-first"),
            ({**graded, "graded": {"ladder": "knot-first", "ladder_max_outside_fraction": 1.5}},
             "outside supported bounds"),
            ({**graded, "graded": {"ladder": "knot-first",
                                   "ladder_channel_max_outside_fraction": -0.1}},
             "outside supported bounds"),
            ({**graded, "graded": {"ladder": "knot-first", "ladder_min_outside_quotes": 0}},
             "outside supported bounds"),
            ({**graded, "graded": {"ladder": "knot-first"}, "profiles": knot},
             "requires point profiles"),
            ({**graded, "graded": {"ladder": "knot-first"}, "profiles": gated},
             "replaces the dominance gate")):
        policy = write_policy(tmp_path / "policy.json", document)
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode != 0, document
        assert message in result.stderr, (document, result.stderr)


def test_native_price_only_and_ladder_off_keys_leave_predictions_unchanged(binary, tmp_path):
    # Clean quotes: no served price is a repaired node without a volatility, so
    # price_only changes no row; "ladder": "off" is the historical admission.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    runs = {}
    for name, document in (
            ("plain", {"schema_version": 1, "admission": "graded"}),
            ("keyed", {"schema_version": 1, "admission": "graded", "price_only": True,
                       "graded": {"ladder": "off"}}),
            ("static", {"schema_version": 1}),
            ("static_keyed", {"schema_version": 1, "price_only": True})):
        output = tmp_path / f"{name}.csv"
        result = invoke(binary, source, output, "--policy",
                        str(write_policy(tmp_path / f"{name}.json", document)))
        assert result.returncode == 0, result.stderr
        runs[name] = (output.read_bytes(), json.loads(result.stdout))
    assert runs["keyed"][0] == runs["plain"][0]
    assert runs["static_keyed"][0] == runs["static"][0]
    for keyed, plain in (("keyed", "plain"), ("static_keyed", "static")):
        report, plain_report = runs[keyed][1], runs[plain][1]
        assert report["policy_definition"]["price_only"] is True
        assert "price_only" not in plain_report["policy_definition"]
        assert report["price_only_rows"] == 0
        assert "ok_price_only" in report["price_only_semantics"]
        assert "price_only_rows" not in plain_report
    keyed_report = runs["keyed"][1]
    assert "ladder" not in keyed_report["policy_definition"]["graded"]
    assert "ladder knot-first" not in keyed_report["graded_semantics"]
    for entry in keyed_report["graded_stats"]["slices"]:
        assert "method" not in entry and "ladder_fallback" not in entry


def test_native_knot_first_ladder_serves_the_knot_slice_with_provenance(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    expected = make_quotes(source)
    policy = write_policy(tmp_path / "ladder.json", {
        "schema_version": 1, "admission": "graded", "price_only": True,
        "graded": {"ladder": "knot-first", "ladder_max_outside_fraction": 0.05,
                   "ladder_channel_max_outside_fraction": 0.05,
                   "ladder_min_outside_quotes": 1}})
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert all(row["status"] == "ok" for row in rows)
    assert all(row["grade"] in {"A", "B"} for row in rows)
    assert max(abs(float(row["predicted_price"]) - value)
               for row, value in zip(rows, expected)) < 0.03
    report = json.loads(result.stdout)
    definition = report["policy_definition"]["graded"]
    assert definition["ladder"] == "knot-first"
    assert definition["ladder_max_outside_fraction"] == 0.05
    assert definition["ladder_channel_max_outside_fraction"] == 0.05
    assert definition["ladder_min_outside_quotes"] == 1
    assert "ladder knot-first" in report["graded_semantics"]
    (entry,) = report["graded_stats"]["slices"]
    # Pass A: the certified, in-market knot slice serves (channel, no fallback).
    assert entry["method"] == "channel"
    assert entry["ladder_fallback"] == "none"
    assert entry["knot_status"] == "success"
    assert entry["outside_fraction"] <= 0.05
    assert entry["outside_quotes"] >= 0
    assert report["price_only_rows"] == 0


def european_put(strike: float, years: float, sigma: float, q: float) -> float:
    # Zero rate: an American put has no early-exercise premium, so this is its price.
    sigma_t = sigma * math.sqrt(years)
    d1 = (math.log(100.0 / strike) - q * years) / sigma_t + 0.5 * sigma_t
    d2 = d1 - sigma_t
    def phi(x: float) -> float:
        return 0.5 * math.erfc(-x / math.sqrt(2.0))
    return strike * phi(-d2) - 100.0 * math.exp(-q * years) * phi(-d1)


def price_only_puts(years: float = 0.5, sigma: float = 0.15,
                    zigzag: bool = True) -> list[tuple[int, float, float]]:
    """(strike, yield, price) of the PriceOnly fixture (quote_surface_test put_zigzag_chain).

    Deep in-the-money puts, zero rate, strikes 100-148 every 4, per-row yield 0 at
    even positions (both ends) and 4% between. A 4% put is worth about K - S e^{-qT},
    above the chord of its zero-yield neighbours (about K - S), so a channel fit's
    convexity repair lowers it below its own zero-volatility value: a certified
    price without an identifiable volatility (library status PriceOnly).
    """
    out = []
    for index, strike in enumerate(range(100, 149, 4)):
        q = 0.04 if zigzag and index % 2 == 1 else 0.0
        out.append((strike, q, european_put(strike, years, sigma, q)))
    return out


def write_price_only_quotes(path: Path, *, crossing: bool = False) -> None:
    # crossing adds a later expiry quoted far below the first: a calendar crossing.
    chains = [(17, 0.5, 21000, price_only_puts())]
    if crossing:
        chains.append((18, 1.0, 21183, price_only_puts(1.0, 0.05, zigzag=False)))
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow([
            "row_id", "chain_id", "strike", "is_call", "spot", "bid", "ask",
            "years", "years_calendar", "rate", "sdiv", "ddiv_pv",
            "input_status", "exercise", "underlier", "root", "expiry_days", "snapshot_ns",
        ])
        row = 0
        for chain, years, days, puts in chains:
            for strike, q, price in puts:
                spread = max(0.01, 0.05 * price)
                writer.writerow([row, chain, strike, 0, 100, max(price - spread, 0.0),
                                 price + spread, years, years, 0, q, 0, "ok", "American",
                                 "AAA", "AAA", days, 1])
                row += 1


def price_only_policy(path: Path, *, graded: bool, price_only: bool | None) -> Path:
    # Point profiles in graded admission (the channel refits repair convexity);
    # knot-spline profiles on the static path. Unbounded convexity repair.
    profiles = [{"name": name, "base": name, "channel": {"repair_half_spreads": 100},
                 **({} if graded else {"curve": "knot-spline"})} for name in PROFILE_NAMES]
    document = {"schema_version": 1, "profiles": profiles}
    if graded:
        document["admission"] = "graded"
    if price_only is not None:
        document["price_only"] = price_only
    return write_policy(path, document)


@pytest.mark.parametrize("graded", [False, True], ids=["static", "graded"])
def test_native_price_only_rows_are_served_only_under_the_key(binary, tmp_path, graded):
    source = tmp_path / "quotes.csv"
    write_price_only_quotes(source)
    runs = {}
    for name, key in (("on", True), ("off", False), ("absent", None)):
        output = tmp_path / f"{name}.csv"
        policy = price_only_policy(tmp_path / f"{name}.json", graded=graded, price_only=key)
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode == 0, result.stderr
        runs[name] = (output.read_bytes(), read_quotes(output), json.loads(result.stdout))
    # Off is the historical behaviour: byte-identical to a policy without the key.
    assert runs["off"][0] == runs["absent"][0]
    on, off = runs["on"][1], runs["off"][1]
    price_only = [i for i, row in enumerate(on) if row["status"] == "ok_price_only"]
    assert price_only, "the fixture must produce PriceOnly rows"
    served = [i for i, row in enumerate(on) if row["status"] == "ok"]
    assert len(served) + len(price_only) == len(on)
    for i in price_only:
        # Key on: the certified price, a blank volatility, never a vol row.
        assert float(on[i]["predicted_price"]) > 0.0
        assert on[i]["predicted_vol"] == ""
        assert on[i]["vol_status"] == "unidentifiable"
        assert on[i].get("grade", "") == ("B" if graded else "")
        # Key off: refused invalid_volatility, no price; the same vol_status.
        assert off[i]["status"] == "invalid_volatility"
        assert off[i]["predicted_price"] == "" and off[i]["predicted_vol"] == ""
        assert off[i]["vol_status"] == "unidentifiable"
        assert off[i].get("grade", "") == ""
    for i in served:
        assert on[i] == off[i]
        assert float(on[i]["predicted_vol"]) > 0.0 and on[i]["vol_status"] == "ok"
    report, off_report = runs["on"][2], runs["off"][2]
    assert report["price_only_rows"] == len(price_only)
    assert report["served_rows"] == off_report["served_rows"] == len(served)
    assert "price_only_rows" not in off_report
    (profile,) = [counts for counts in report["per_profile_counts"].values() if counts.get("rows")]
    assert profile["price_only_rows"] == len(price_only)
    assert profile["row_status_counts"] == {"ok": len(served), "ok_price_only": len(price_only)}
    if graded:
        assert report["graded_stats"]["price_only_rows"] == {"B": len(price_only)}
        assert report["graded_stats"]["served_rows"] == {"B": len(served)}


@pytest.mark.parametrize("graded", [False, True], ids=["static", "graded"])
def test_native_calendar_refusal_resets_price_only_rows(binary, tmp_path, graded):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    write_price_only_quotes(source, crossing=True)
    policy = price_only_policy(tmp_path / "policy.json", graded=graded, price_only=True)
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode == 0, result.stderr
    rows = read_quotes(output)
    report = json.loads(result.stdout)
    refused = [row for row in rows if row["status"] == "calendar_arbitrage"]
    # A refused expiry carries no price, no volatility and no evaluated vol_status.
    assert refused
    for row in refused:
        assert row["predicted_price"] == "" and row["predicted_vol"] == ""
        assert row["vol_status"] == "not_evaluated"
        assert row.get("grade", "") == ""
    price_only = [row for row in rows if row["status"] == "ok_price_only"]
    assert report["price_only_rows"] == len(price_only)
    if graded:
        # Inside the surface the later, lower-confidence expiry is refused; the
        # first keeps its PriceOnly rows.
        assert {row["chain_id"] for row in refused} == {"18"}
        assert price_only and all(row["chain_id"] == "17" for row in price_only)
    else:
        # The static rule refuses both slices of the crossing pair, so every
        # price-only row of the first is withdrawn from price_only_rows.
        assert len(refused) == len(rows)
        assert report["calendar_rejected_chains"] == 2
        assert report["served_rows"] == 0 and not price_only


def load_quote_benchmark():
    spec = importlib.util.spec_from_file_location(
        "quote_benchmark_for_native_cli",
        Path(__file__).resolve().parents[2] / "tools" / "quote_benchmark.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_native_price_only_rows_are_scored_as_price_served_never_vol_served(binary, tmp_path):
    # End to end: raw quotes -> prepare -> native run (graded, key on and off) -> score.
    qb = load_quote_benchmark()
    puts = price_only_puts()
    n = len(puts)
    when = dt.datetime(2026, 10, 14, 14, 30, tzinfo=dt.timezone.utc)
    source = tmp_path / "source.parquet"
    qb.pq.write_table(qb.pa.table({
        "okey_tk": ["AAA"] * n, "okey_dt": [dt.date(2027, 1, 15)] * n,
        "okey_xx": [float(strike) for strike, _, _ in puts], "okey_cp": ["Put"] * n,
        "date": [when] * n, "timestamp": [when + dt.timedelta(seconds=1)] * n,
        "undSecKey_tk": ["AAA"] * n, "uBid": [100.0] * n, "uAsk": [100.0] * n,
        "bidPrc": [max(price - max(0.01, 0.05 * price), 0.0) for _, _, price in puts],
        "askPrc": [price + max(0.01, 0.05 * price) for _, _, price in puts],
        "bidSz": [10] * n, "askSz": [10] * n, "years": [0.5] * n, "yearsC": [0.5] * n,
        "rate": [0.0] * n, "sdiv": [q for _, q, _ in puts], "ddiv": [0.0] * n,
        "ddivPv": [0.0] * n, "exType": ["American"] * n,
        "srPrc": [price for _, _, price in puts], "srVol": [0.15] * n,
        "bidIV": [998.0] * n, "askIV": [999.0] * n, "synSpot": [9999.0] * n,
        "atmVol": [999.0] * n, "error": [999] * n,
    }), source)
    dataset = tmp_path / "dataset"
    qb.prepare(qb.parser().parse_args(["prepare", "--source", str(source), "--output",
                                       str(dataset), "--split", "all", "--csv"]))
    metrics = {}
    for name, key in (("on", True), ("off", False)):
        output = tmp_path / f"{name}.csv"
        policy = price_only_policy(tmp_path / f"{name}.json", graded=True, price_only=key)
        qb.run_native(qb.parser().parse_args(["run", "--dataset", str(dataset), "--binary",
                                              str(binary), "--output", str(output),
                                              "--policy", str(policy)]))
        statuses = [row["status"] for row in read_quotes(output)]
        scored = qb.score(qb.parser().parse_args([
            "score", "--dataset", str(dataset), "--run-receipt", str(output) + ".run.json",
            "--output", str(tmp_path / f"{name}-score.json"), "--split", "all"]))["metrics"]
        ok, price_only = statuses.count("ok"), statuses.count("ok_price_only")
        assert scored["eligible_rows"] == n
        # Price-served = ok | ok_price_only; vol-served = ok only.
        assert scored["predicted_rows"] == scored["price_prediction_rows"] == ok + price_only
        assert scored["price_only_predicted_rows"] == price_only
        assert scored["vol_prediction_rows"] == scored["vol_scored_rows"] == ok
        assert scored["price_coverage"] == pytest.approx((ok + price_only) / n)
        assert scored["vol_coverage"] == pytest.approx(ok / n)
        metrics[name] = (scored, ok, price_only)
    (on, on_ok, on_price_only), (off, off_ok, off_price_only) = metrics["on"], metrics["off"]
    assert on_price_only > 0 and off_price_only == 0
    assert on_ok == off_ok
    assert on["predicted_rows"] == off["predicted_rows"] + on_price_only
    # The key changes price coverage only: every volatility metric is identical.
    for metric in ("vol_prediction_rows", "vol_scored_rows", "vol_coverage", "vol_bp_mae",
                   "vol_bp_p99_abs"):
        assert on[metric] == off[metric], metric


def test_native_knot_spline_profile_serves_a_channel_spline(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    expected = make_quotes(source)
    profiles = [{"name": name, "base": name, "curve": "knot-spline",
                 "channel": {"repair_half_spreads": 0.5, "channel_weight": 100}}
                for name in ("dense", "ordinary", "sparse")]
    policy = write_policy(tmp_path / "policy.json",
                          {"schema_version": 1, "profiles": profiles})
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert all(row["configured_curve"] == "knot-spline" for row in rows)
    assert all(row["actual_curve"] == "spline-vol" for row in rows)
    assert all(row["status"] == "ok" for row in rows)
    assert max(abs(float(row["predicted_vol"]) - 0.3) for row in rows) < 0.005
    assert max(abs(float(row["predicted_price"]) - value)
               for row, value in zip(rows, expected)) < 0.03
    report = json.loads(result.stdout)
    definition = report["policy_definition"]["profiles"]
    assert all(profile["method"] == "knot-spline" for profile in definition)
    assert all(profile["channel"]["repair_half_spreads"] == 0.5 for profile in definition)
    bad = write_policy(tmp_path / "bad.json", {"schema_version": 1, "profiles": [
        {"name": "dense", "base": "dense", "channel": {"unknown": 1}}]})
    result = invoke(binary, source, output, "--policy", str(bad))
    assert result.returncode != 0
    assert "Unknown policy key" in result.stderr
    assert all("family" not in profile["channel"] for profile in definition)


def test_native_channel_family_solves_the_channel_objective(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    expected = make_quotes(source)
    profiles = [{"name": name, "base": name, "curve": "knot-spline",
                 "channel": {"family": "essvi", "family_iterations": 8}}
                for name in ("dense", "ordinary", "sparse")]
    policy = write_policy(tmp_path / "policy.json",
                          {"schema_version": 1, "profiles": profiles})
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert all(row["configured_curve"] == "channel-essvi" for row in rows)
    assert all(row["actual_curve"] == "essvi" for row in rows)
    assert all(row["status"] == "ok" for row in rows)
    assert max(abs(float(row["predicted_price"]) - value)
               for row, value in zip(rows, expected)) < 0.03
    definition = json.loads(result.stdout)["policy_definition"]["profiles"]
    assert all(profile["channel"]["family"] == "essvi" for profile in definition)
    assert all(profile["channel"]["family_iterations"] == 8 for profile in definition)
    for family in ("linear-variance", "cstar"):
        bad = write_policy(tmp_path / "bad.json", {"schema_version": 1, "profiles": [
            {"name": "dense", "base": "dense", "curve": "knot-spline",
             "channel": {"family": family}}]})
        result = invoke(binary, source, output, "--policy", str(bad))
        assert result.returncode != 0, family


def test_native_dividend_lattice_keys_are_strict_and_recorded(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    write_quotes(source, [dict(row, ddiv="0") for row in read_quotes(source)])
    for extra, message in (
            ({"dividend_lattice": "crr"}, "dividend_lattice must be"),
            ({"dividend_lattice": 3}, "must be a string"),
            ({"dividend_lattice_steps": 0}, "outside supported bounds"),
            ({"dividend_lattice_steps": 20002}, "outside supported bounds"),
            ({"dividend_lattice_steps": 301.5}, "correct type"),
            ({"dividend_lattice": "spliced-bbsr", "dividend_lattice_steps": 1},
             "invalid quote quality or model point count")):
        profiles = [{"name": name, "base": name, "dividends": "inferred-schedule", **extra}
                    for name in ("dense", "ordinary", "sparse")]
        policy = write_policy(tmp_path / "policy.json",
                              {"schema_version": 1, "profiles": profiles})
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode != 0, extra
        assert message in result.stderr, (extra, result.stderr)
    # On an aggregate profile the keys would be inert, so they are refused.
    for extra in ({"dividend_lattice": "escrowed-lr"}, {"dividend_lattice_steps": 401},
                  {"dividends": "aggregate", "dividend_lattice": "escrowed-lr"}):
        profiles = [{"name": name, "base": name, **extra}
                    for name in ("dense", "ordinary", "sparse")]
        policy = write_policy(tmp_path / "policy.json",
                              {"schema_version": 1, "profiles": profiles})
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode != 0, extra
        assert "require dividends inferred-schedule" in result.stderr, (extra, result.stderr)
    # A routed profile records both keys; the default records no scheme.
    profiles = [{"name": name, "base": name, "dividends": "inferred-schedule",
                 "dividend_lattice": "escrowed-lr", "dividend_lattice_steps": 401}
                for name in ("dense", "ordinary", "sparse")]
    policy = write_policy(tmp_path / "policy.json", {"schema_version": 1, "profiles": profiles})
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    definition = report["policy_definition"]["profiles"]
    assert all(profile["dividend_lattice"] == "escrowed-lr" for profile in definition)
    assert all(profile["discrete_div_steps"] == 401 for profile in definition)
    # No cash in life: the chain never reaches the lattice, so no scheme is claimed.
    route, = report["dividend_schedule"]["routes"]
    assert route["reason"] == "no_ex_date_in_life"
    assert route["lattice_scheme"] is None and route["lattice_steps"] is None
    default = invoke(binary, source, output)
    assert default.returncode == 0, default.stderr
    definition = json.loads(default.stdout)["policy_definition"]["profiles"]
    assert all("dividend_lattice" not in profile for profile in definition)
    assert all(profile["discrete_div_steps"] == 301 for profile in definition)


def test_native_malformed_ddiv_cell_is_a_typed_schedule_refusal_not_an_abort(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    clean = [dict(row, ddiv="0") for row in read_quotes(source)]
    write_quotes(source, clean)
    baseline = invoke(binary, source, output)
    assert baseline.returncode == 0, baseline.stderr
    expected = output.read_bytes()
    write_quotes(source, [dict(row, ddiv="n/a") if index == 3 else row
                          for index, row in enumerate(clean)])
    # The default path never reads ddiv: same bytes, no abort.
    default = invoke(binary, source, output)
    assert default.returncode == 0, default.stderr
    assert output.read_bytes() == expected
    # The opt-in schedule of that snapshot is refused, typed, and the row counted.
    profiles = [{"name": name, "base": name, "dividends": "inferred-schedule"}
                for name in ("dense", "ordinary", "sparse")]
    policy = write_policy(tmp_path / "policy.json", {"schema_version": 1, "profiles": profiles})
    routed = invoke(binary, source, output, "--policy", str(policy))
    assert routed.returncode == 0, routed.stderr
    block = json.loads(routed.stdout)["dividend_schedule"]
    assert block["malformed_ddiv_rows"] == 1
    schedule, = block["schedules"]
    assert (schedule["status"], schedule["reason"]) == ("invalid", "non_finite_input")
    route, = block["routes"]
    assert route["route"] == "escrow" and route["reason"] == "schedule_not_covered"
    assert output.read_bytes() == expected


def write_dated_dividend_chain(path: Path, valuation_day: int) -> None:
    # The make_quotes chain on a dated clock: valued on `valuation_day`, expiring
    # 182 days later, rate 5%, and one 1.00 cash dividend whose PV identity dates
    # it exactly 91 days out (ddiv_pv = exp(-r * 91 / 365), ACT/365).
    make_quotes(path)
    rate, horizon = 0.05, 182
    pv = math.exp(-rate * 91 / 365.0)
    years = str(horizon / 365.0)
    write_quotes(path, [dict(row, rate=str(rate), ddiv="1", ddiv_pv=repr(pv), years=years,
                             years_calendar=years, expiry_days=str(valuation_day + horizon),
                             snapshot_ns=str(valuation_day * 86_400 * 10**9))
                        for row in read_quotes(path)])


def test_native_empty_route_over_cash_in_life_is_a_window_mismatch(binary, tmp_path):
    # Smooth-div review N2 (tools/quote_bench_main.cpp, fit_with_schedule): the
    # route's event count is compared with the life's BEFORE applies(), so an
    # empty route over a life that holds an ex-date is route_window_mismatch, not
    # no_ex_date_in_life. The input that reaches it: a valuation day past
    # dividend_schedule_instants' int64 guard (valuation_day * ns/day plus 36,526
    # days must stay below 9e18, i.e. day < ~67,640; dividend_schedule.cpp). The
    # instants come back empty there while the schedule, which counts days only,
    # still dates the dividend inside the option's life.
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    profiles = [{"name": name, "base": name, "dividends": "inferred-schedule"}
                for name in ("dense", "ordinary", "sparse")]
    policy = write_policy(tmp_path / "policy.json", {"schema_version": 1, "profiles": profiles})

    def route_of(valuation_day):
        write_dated_dividend_chain(source, valuation_day)
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode == 0, result.stderr
        block = json.loads(result.stdout)["dividend_schedule"]
        schedule, = block["schedules"]
        # The schedule is the same on either side of the guard: one exact dividend.
        assert (schedule["status"], schedule["reason"]) == ("ok", "none")
        event, = schedule["events"]
        assert (event["days"], event["amount"], event["kind"]) == (91.0, 1.0, "exact")
        route, = block["routes"]
        assert route["coverage"] == "covered" and route["events_in_life"] == 1
        return route

    # Control inside the guard: the route admits the same ex-date.
    control = route_of(20_000)
    assert control["route_events"] == 1
    assert control["reason"] not in ("route_window_mismatch", "no_ex_date_in_life")
    # Past the guard (day 69,000, year 2158): an empty route, labelled a mismatch.
    route = route_of(69_000)
    assert route["route_events"] == 0 and route["n_outside_tau_window"] == 0
    assert route["route"] == "escrow"
    assert route["reason"] == "route_window_mismatch"
    assert route["lattice_scheme"] is None and route["lattice_steps"] is None
