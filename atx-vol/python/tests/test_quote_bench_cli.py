"""Post-implementation native boundary checks; no market-data fixture required."""
from __future__ import annotations

import csv
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


@pytest.mark.parametrize("document", [
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
