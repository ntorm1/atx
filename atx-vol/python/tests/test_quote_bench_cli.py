"""Post-implementation native boundary checks; no market-data fixture required."""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
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


def invoke(binary: Path, source: Path, output: Path, *extra: str, env=None):
    return subprocess.run(
        [str(binary), "--input", str(source), "--output", str(output), *extra],
        capture_output=True, text=True, timeout=60, check=False, env=env,
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


@pytest.mark.parametrize("paired", [False, True])
def test_native_joint_feasible_mid_reports_method_and_whole_surface_fallback(binary, tmp_path, paired):
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    if paired:
        with source.open(newline="") as stream:
            originals = list(csv.DictReader(stream))
        puts = [dict(row, row_id=str(int(row["row_id"]) + 1000), is_call="0",
                     bid=str(float(row["bid"]) + float(row["strike"]) - 100),
                     ask=str(float(row["ask"]) + float(row["strike"]) - 100)) for row in originals]
        with source.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(originals[0]))
            writer.writeheader()
            writer.writerows(originals + puts)
    document = {"schema_version": 1, "admission": "graded",
                "graded": {"certificate": "box", "certificate_box_half_ticks": 0.0},
                "serve": {"floor_price_only": True, "metadata": True}}
    runs = {}
    for name, mode in (("default", None), ("off", "existing_policy"), ("on", "joint_feasible_mid")):
        selected = json.loads(json.dumps(document))
        if mode is not None:
            selected["graded"]["cold_fit"] = mode
        policy = tmp_path / (name + ".json")
        policy.write_text(json.dumps(selected))
        output = tmp_path / (name + ".csv")
        result = invoke(binary, source, output, "--policy", str(policy), "--timing-detail")
        assert result.returncode == 0, result.stderr
        runs[name] = (output, json.loads(result.stdout))
    assert runs["default"][0].read_bytes() == runs["off"][0].read_bytes()
    assert runs["default"][1]["policy_definition"] == runs["off"][1]["policy_definition"]
    assert "cold_fit" not in runs["off"][1]["policy_definition"]["graded"]
    report = runs["on"][1]
    assert report["policy_definition"]["graded"]["cold_fit"] == "joint_feasible_mid"
    (entry,) = report["graded_stats"]["slices"]
    assert entry["cold_fit"]["requested"] == "joint_feasible_mid"
    assert "reimply_price_enclosure_hits" in entry["timing"]
    if paired:
        assert entry["method"] == "joint_feasible_mid"
        assert entry["cold_fit"]["fallback"] == "none"
        assert entry["cold_fit"]["final_price_observations"] > 0
        assert entry["cold_fit"]["final_price_identified_nodes"] > 0
        assert entry["grade"] == "B"
        with runs["on"][0].open(newline="") as stream:
            predictions = list(csv.DictReader(stream))
        assert len(predictions) == 52
        assert all(row["status"] in ("ok", "ok_price_only") for row in predictions)
        assert all(row["quote_status"] == "projected_price" for row in predictions)
    else:
        assert entry["method"] != "joint_feasible_mid"
        assert entry["cold_fit"]["fallback"] == "unsupported_input"
        # The policy hash differs; numerical output and row classifications do not.
        with runs["on"][0].open(newline="") as stream:
            actual = list(csv.DictReader(stream))
        with runs["off"][0].open(newline="") as stream:
            expected = list(csv.DictReader(stream))
        for a, b in zip(actual, expected):
            for key in ("predicted_price", "predicted_vol", "status", "quote_status"):
                assert a[key] == b[key]


@pytest.mark.parametrize("mode", ["joint_raw_mid", "unknown", True, 1, None, {}])
def test_native_joint_cold_mode_rejects_invalid_policy_before_output(binary, tmp_path, mode):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    policy = tmp_path / "policy.json"
    policy.write_text(json.dumps({"schema_version": 1, "admission": "graded",
                                  "graded": {"cold_fit": mode}}))
    result = invoke(binary, source, output, "--policy", str(policy))
    assert result.returncode != 0
    assert not output.exists()


def test_native_acceptance_audit_queries_actual_final_surface(binary, tmp_path):
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    # Both sides have original boxes so the independent economic oracle can
    # audit every published node without inventing a spread for a synthetic put.
    with source.open(newline="") as stream:
        originals = list(csv.DictReader(stream))
    fields = list(originals[0]) + ["observation_ns"]
    for row in originals:
        row["observation_ns"] = "1"
    puts = [dict(row, row_id=str(int(row["row_id"]) + 1000), is_call="0",
                 bid=str(float(row["bid"]) + float(row["strike"]) - 100),
                 ask=str(float(row["ask"]) + float(row["strike"]) - 100)) for row in originals]
    with source.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(originals + puts)
    policy = tmp_path / "policy.json"
    policy.write_text(json.dumps({"schema_version": 1, "admission": "graded",
                                  "serve": {"metadata": True, "floor_price_only": True}}))
    plain = invoke(binary, source, tmp_path / "plain.csv", "--policy", str(policy))
    audited = invoke(binary, source, tmp_path / "audit.csv", "--policy", str(policy),
                     "--acceptance-audit")
    assert audited.returncode == plain.returncode == 0, audited.stderr
    assert (tmp_path / "plain.csv").read_bytes() == (tmp_path / "audit.csv").read_bytes()
    native = json.loads(audited.stdout)
    block = native["graded_stats"]["acceptance_audit"]
    assert block["timing_qualifying"] is False
    assert block["representation"] == "certified_piecewise_linear"
    assert "acceptance_audit" not in json.loads(plain.stdout)["graded_stats"]
    (entry,) = block["snapshots"][0]
    assert len(entry["nodes"]) >= 2
    assert len(entry["queries"]) > 2 * len(entry["nodes"])
    assert all(query["status"] in ("success", "price_only") for query in entry["queries"])
    assert all({"iv_lower", "iv_upper", "vol_convention", "reason"} <= query.keys()
               for query in entry["queries"])
    node_strikes = {node["strike"] for node in entry["nodes"]}
    assert entry["al_options"] == {"explicit": False, "n_collocation": 12,
        "n_quadrature": 24, "n_quad_price": 48, "max_newton_iter": 6, "tol": 1e-10, "profile": 0}
    checked = 0
    for query in entry["queries"]:
        if query["strike"] not in node_strikes or query["status"] != "success":
            assert "cold_node" not in query
            continue
        cold = query["cold_node"]
        assert cold["status"] == "evaluated"
        assert cold["options"] == entry["al_options"]
        assert cold["options"]["profile"] == 0
        assert cold["radius"] == 1e-7
        assert cold["sigma_lower"] == query["volatility"] - 1e-7
        assert cold["sigma_upper"] == query["volatility"] + 1e-7
        assert cold["floor_sigma"] == 0.005
        # With zero rate/carry this American fixture equals closed-form Black-Scholes.
        for name, sigma_key in (("lower", "sigma_lower"), ("center", "sigma_center"),
                                ("upper", "sigma_upper"), ("at_floor", "floor_sigma")):
            sigma_t = cold[sigma_key] * math.sqrt(0.5)
            d1 = math.log(100.0 / query["strike"]) / sigma_t + 0.5 * sigma_t
            d2 = d1 - sigma_t
            expected = 50.0 * math.erfc(-d1 / math.sqrt(2.0)) - \
                       0.5 * query["strike"] * math.erfc(-d2 / math.sqrt(2.0))
            if query["side"] == "put":
                expected += query["strike"] - 100.0
            assert abs(cold["price_" + name] - expected) < 1e-10
        checked += 1
    assert checked > 0
    verifier_path = Path(__file__).resolve().parents[2] / "bench" / "spy_acceptance.py"
    spec = importlib.util.spec_from_file_location("native_cold_node_verifier", verifier_path)
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    root_audit = verifier.cold_root_audit(native)
    assert root_audit["strict_accuracy_passed"], root_audit
    assert root_audit["identified_nodes"] == checked
    economic_audit = verifier.cold_price_audit(native, verifier.load_quotes(source))
    assert economic_audit["price_accuracy_passed"], economic_audit
    assert economic_audit["identified_rows_checked"] == checked
    assert economic_audit["accuracy_contract"]["version"] == "raw-spread-price-v1"
    assert economic_audit["accuracy_contract"]["raw_boxes_modified"] is False
    with (tmp_path / "audit.csv").open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    by_contract = {(query["strike"], query["side"]): query for query in entry["queries"]}
    for strike, row in zip(range(75, 126, 2), rows):
        query = by_contract[strike, "call"]
        assert query["vol_convention"] == row["vol_convention"]
        assert query["reason"] == row["price_only_reason"]
        for key, column in (("iv_lower", "iv_lower"), ("iv_upper", "iv_upper"),
                            ("volatility", "predicted_vol")):
            assert query[key] == (float(row[column]) if row[column] else None)


@pytest.mark.skipif(os.name != "nt", reason="Windows native/launcher process receipt")
def test_native_cold_launcher_joins_distinct_kernel_process_identities(binary, tmp_path):
    source, policy = tmp_path / "quotes.csv", tmp_path / "policy.json"
    make_quotes(source)
    policy.write_text(json.dumps({"schema_version": 1, "admission": "graded"}))
    out = tmp_path / "trials"
    launcher = Path(__file__).resolve().parents[2] / "bench" / "run_cold_trials.ps1"
    result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(launcher), "-Binary", str(binary.resolve()), "-InputCsv", str(source),
        "-Policy", str(policy), "-OutDir", str(out), "-Trials", "2"],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8-sig"))
    assert len(manifest["trials"]) == 2
    identities = set()
    for trial in manifest["trials"]:
        native = json.loads((out / trial["report"]).read_text(encoding="utf-8-sig"))
        identity = trial["process_identity"]
        assert identity == native["bench"]["process_identity"]
        identities.add((identity["pid"], identity["creation_filetime"]))
        assert (out / trial["predictions"]).is_file()
    assert len(identities) == 2


def test_native_carry_normalization_policy_is_explicit_and_bounded(binary, tmp_path):
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    policy = tmp_path / "policy.json"
    for snapshot, accepted in [
            ({"one_spot": "nearest", "carry_price_tolerance": 1e-6}, True),
            ({"one_spot": "off", "carry_price_tolerance": 1e-6}, False),
            ({"one_spot": "nearest", "carry_price_tolerance": -1e-6}, False)]:
        policy.write_text(json.dumps({"schema_version": 1, "admission": "graded",
                                      "snapshot": snapshot}))
        native = invoke(binary, source, tmp_path / "predictions.csv", "--policy", str(policy))
        assert (native.returncode == 0) == accepted, native.stderr
        if accepted:
            report = json.loads(native.stdout)
            assert report["policy_definition"]["snapshot"]["carry_price_tolerance"] == 1e-6
            assert report["one_spot"]["chains"][0]["carry_normalization"]["strikes_changed"] == 0


def test_native_terminal_floor_recovery_policy_is_explicit_and_reports_counts(binary, tmp_path):
    source, policy = tmp_path / "quotes.csv", tmp_path / "policy.json"
    make_quotes(source)
    for graded, accepted in [
            ({"certificate": "box", "terminal_floor_recovery": True}, True),
            ({"certificate": "box", "terminal_floor_recovery": False}, True),
            ({"terminal_floor_recovery": True}, False),
            ({"certificate": "box", "terminal_floor_recovery": "true"}, False)]:
        policy.write_text(json.dumps({"schema_version": 1, "admission": "graded", "graded": graded}))
        result = invoke(binary, source, tmp_path / "predictions.csv", "--policy", str(policy),
                        "--timing-detail")
        assert (result.returncode == 0) == accepted, result.stderr
        if not accepted:
            continue
        report = json.loads(result.stdout)
        enabled = graded["terminal_floor_recovery"]
        assert report["policy_definition"]["graded"].get("terminal_floor_recovery", False) == enabled
        assert len(report["graded_stats"]["slices"]) == 1
        for entry in report["graded_stats"]["slices"]:
            timing = entry["timing"]
            assert all(key in timing for key in ("reimply_native_attempts", "reimply_native_roots",
                "reimply_native_fallbacks", "reimply_native_max_bound_iv"))
            if enabled:
                assert entry["repair"]["terminal_floor_asks"] == 0
                assert entry["repair"]["terminal_floor_max_ask_raise"] == 0
                # This policy deliberately leaves inbox_projection off: the
                # terminal-floor rung still writes its actual box diagnostics.
                assert entry["inbox_projection"]["terminal_floor_asks"] == 0
                assert entry["inbox_projection"]["terminal_floor_max_ask_raise"] == 0


@pytest.mark.parametrize("budget,price_only,accepted", [
    ({"reimply_price_cap": 0.001, "reimply_spread_fraction": 0.01}, True, True),
    ({"reimply_price_cap": 0.0, "reimply_spread_fraction": 0.0}, False, True),
    ({"reimply_price_cap": 0.001}, True, False),
    ({"reimply_spread_fraction": 0.01}, True, False),
    ({"reimply_price_cap": -0.001, "reimply_spread_fraction": 0.01}, True, False),
    ({"reimply_price_cap": "0.001", "reimply_spread_fraction": 0.01}, True, False),
    ({"reimply_price_cap": 0.001, "reimply_spread_fraction": 0.01}, False, False),
])
def test_native_reimply_price_budget_is_opt_in_and_has_exact_provenance(
        binary, tmp_path, budget, price_only, accepted):
    source, output, policy = tmp_path / "quotes.csv", tmp_path / "out.csv", tmp_path / "policy.json"
    make_quotes(source)
    policy.write_text(json.dumps({
        "schema_version": 1, "admission": "graded",
        "serve": {"floor_price_only": price_only, "metadata": True},
        "profiles": [{"name": name, "base": name, **budget}
                     for name in ["dense", "ordinary", "sparse"]],
    }))
    result = invoke(binary, source, output, "--policy", str(policy))
    assert (result.returncode == 0) == accepted, result.stderr
    if not accepted:
        assert not output.exists()
        return
    report = json.loads(result.stdout)
    for profile in report["policy_definition"]["profiles"]:
        if budget["reimply_price_cap"] > 0:
            assert profile["reimply_price_cap"] == 0.001
            assert profile["reimply_spread_fraction"] == 0.01
        else:
            assert "reimply_price_cap" not in profile
            assert "reimply_spread_fraction" not in profile


def test_native_funding_carry_normalization_is_reported_and_material_conflicts_refuse(
        binary, tmp_path):
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    with source.open(newline="") as stream:
        reader = csv.DictReader(stream)
        fields, rows = reader.fieldnames, list(reader)
    for row in rows:
        row["ddiv_pv"] = "1.8"
    put = dict(rows[12], row_id="999", is_call="0", bid="8.0", ask="8.1", sdiv="1e-10")
    rows.append(put)
    policy = tmp_path / "policy.json"
    policy.write_text(json.dumps({"schema_version": 1, "admission": "graded",
                                  "snapshot": {"one_spot": "nearest",
                                               "carry_price_tolerance": 1e-6}}))
    for carry, expected in [("1e-10", "rebased"), ("0.01", "carry_conflict")]:
        rows[-1]["sdiv"] = carry
        with source.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
        native = invoke(binary, source, tmp_path / "predictions.csv", "--policy", str(policy),
                        "--dividends", "funding-pv")
        assert native.returncode == 0, native.stderr
        record = json.loads(native.stdout)["one_spot"]["chains"][0]
        assert record["status"] == expected
        if expected == "rebased":
            assert record["carry_normalization"]["strikes_changed"] == 1
            assert 0 < record["carry_normalization"]["max_price_change_bound"] < 1e-6


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


# PM ruling R3 (integrate-W2c-a, 2026-10-21): graded admission defaults to SPY1s Phase 1
# arm D. ARM_D_GRADED spells that default out key by key; HISTORICAL_GRADED (the kill switch)
# restores the pre-R3 graded path byte for byte.
ARM_D_GRADED = {"fit_space": "vol", "deam_profile": "screen", "deam_cache": True,
                "deam_inversion": "premium", "confirm": "always", "confirm_k": 10.0,
                "confirm_kernel": "fast", "grid_kernel": "fast", "rung_memo": True}
HISTORICAL_GRADED = {"fit_space": "price", "grid_kernel": "reference", "rung_memo": False}
# A served price of the arm D default sits within the price image of the inversion
# tolerance (2 x 1e-7 vol x vega <= 1e-5 USD here) of the historical one (Premium vs Search
# roots, AlProfile::Fast confirm and grid at 1e-12 relative).
ARM_D_PRICE_TOLERANCE = 1.0e-5


@pytest.mark.parametrize("graded_keys", [None, HISTORICAL_GRADED], ids=["default", "historical"])
def test_native_graded_admission_adds_grades_without_changing_point_prices(binary, tmp_path,
                                                                           graded_keys):
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    static_output, graded_output = tmp_path / "static.csv", tmp_path / "graded.csv"
    static = invoke(binary, source, static_output)
    assert static.returncode == 0, static.stderr
    document = {"schema_version": 1, "admission": "graded"}
    if graded_keys is not None:
        document["graded"] = graded_keys
    policy = write_policy(tmp_path / "graded.json", document)
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
            # A point-fitted expiry is served unchanged (historical: byte for byte; the
            # arm D default: within its documented price tolerance) and graded A.
            if graded_keys is None:
                assert after["status"] == "ok"
                assert abs(float(after["predicted_price"]) - float(before["predicted_price"])) \
                    <= ARM_D_PRICE_TOLERANCE
            else:
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


@pytest.mark.parametrize("graded_keys", [None, HISTORICAL_GRADED], ids=["default", "historical"])
def test_native_graded_snapshot_above_expiry_cap_falls_back_to_static(binary, tmp_path,
                                                                      graded_keys):
    source = tmp_path / "quotes.csv"
    make_wide_snapshot(source, 129)  # kQuoteSurfaceMaxExpiries is 128
    static_output, graded_output = tmp_path / "static.csv", tmp_path / "graded.csv"
    static = invoke(binary, source, static_output)
    assert static.returncode == 0, static.stderr
    document = {"schema_version": 1, "admission": "graded"}
    if graded_keys is not None:
        document["graded"] = graded_keys
    policy = write_policy(tmp_path / "graded.json", document)
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
        if graded_keys is None and before["status"] == "ok":
            # The static fallback runs the graded profiles' arm D controls.
            assert abs(float(after["predicted_price"]) - float(before["predicted_price"])) \
                <= ARM_D_PRICE_TOLERANCE
        else:
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
    serial = {**os.environ, "ATX_VOL_FIT_WORKERS": "1"}
    plain = invoke(binary, source, plain_output, "--policy", str(policy), env=serial)
    assert plain.returncode == 0, plain.stderr
    timed = invoke(binary, source, timed_output, "--policy", str(policy), "--timing-detail",
                   env=serial)
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
    # Serial only: L2d's fan-out overlaps chains at fit_workers > 1, hence the pin above.
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
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "price", "deam_cache": True}},
             "require fit_space vol"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "price", "deam_profile": "fast"}},
             "require fit_space vol"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "quick"}},
             "deam_profile must be accurate, fast or screen"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_cache": 1}},
             "deam_cache must be true or false"),
            # Opt-in per-expiry rung memo (lane L2c).
            ({"schema_version": 1, "admission": "graded", "graded": {"rung_memo": "yes"}},
             "rung_memo must be true or false"),
            ({"schema_version": 1, "graded": {"rung_memo": True}},
             "require admission graded"),
            # Opt-in Premium inversion of the cache (lane L2e).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "price", "deam_inversion": "premium"}},
             "deam_inversion requires fit_space vol"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_inversion": "newton"}},
             "deam_inversion must be search or premium"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "fast",
                         "deam_inversion": "premium"}},
             "deam_inversion premium requires deam_profile accurate or screen"),
            # Screen seed tier, confirm policy / kernel, grid kernel (SPY1s Phase 1).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "search"}},
             "deam_profile screen requires deam_inversion premium"),
            # The R3 default's opt-outs resolve the partner keys: off the Screen seed tier
            # (price channel, or Search implying the accurate kernel) the confirm keys are inert.
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "price", "confirm_kernel": "fast"}},
             "confirm keys require deam_profile screen"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"deam_inversion": "search", "confirm": "band"}},
             "confirm keys require deam_profile screen"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "accurate",
                         "deam_inversion": "premium", "confirm": "band"}},
             "confirm keys require deam_profile screen"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "confirm": "sometimes"}},
             "confirm must be always, band or never"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "confirm": "band", "confirm_k": 0}},
             "confirm_k must be a positive number"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "confirm_kernel": "screen"}},
             "confirm_kernel must be reference or fast"),
            ({"schema_version": 1, "admission": "graded", "graded": {"grid_kernel": "screen"}},
             "grid_kernel must be reference or fast"),
            # Opt-in box certificate (lane L3r1).
            ({"schema_version": 1, "admission": "graded", "graded": {"certificate": "loose"}},
             "certificate must be exact or box"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"certificate_box_half_ticks": 1}},
             "certificate_box_half_ticks requires certificate box"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"certificate": "exact", "certificate_box_half_ticks": 0.5}},
             "certificate_box_half_ticks requires certificate box"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"certificate": "box", "certificate_box_half_ticks": 200}},
             "outside supported bounds"),
            ({"schema_version": 1, "graded": {"certificate": "box"}},
             "require admission graded"),
            # Opt-in exact in-box projection rung (lane C4): it backs up the box rung.
            ({"schema_version": 1, "admission": "graded", "graded": {"inbox_projection": True}},
             "inbox_projection requires certificate box"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"certificate": "exact", "inbox_projection": True}},
             "inbox_projection requires certificate box"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"certificate": "box", "inbox_projection": "yes"}},
             "inbox_projection must be true or false"),
            # Opt-in market wing ceiling (lane L3r1 lever 3).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"wing_mult_source": "quotes"}},
             "wing_mult_source must be policy or market"),
            ({"schema_version": 1, "admission": "graded", "graded": {"wing_mult_source": 1}},
             "must be a string"),
            ({"schema_version": 1, "graded": {"wing_mult_source": "market"}},
             "require admission graded"),
            # Opt-in distance-to-market gate / repair (lane L3q).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"market_distance": "clip"}},
             "market_distance must be off, gate or repair"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"market_distance_spreads": 1.0}},
             "market_distance_spreads requires market_distance gate or repair"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"market_distance": "gate", "market_distance_spreads": -1}},
             "outside supported bounds"),
            ({"schema_version": 1, "graded": {"market_distance": "gate"}},
             "require admission graded"),
            # Opt-in publication validation (lane C2).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"publication": "strict"}},
             "publication must be off or validate"),
            ({"schema_version": 1, "admission": "graded", "graded": {"publication": True}},
             "must be a string"),
            ({"schema_version": 1, "graded": {"publication": "validate"}},
             "require admission graded"),
            # Opt-in serving records (lane H1).
            ({"schema_version": 1, "admission": "graded", "graded": {"serve_records": "on"}},
             "serve_records must be true or false"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"serve_record_profile": "tick"}},
             "serve_record_profile requires serve_records true"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"serve_records": True, "serve_record_profile": "fast"}},
             "serve_record_profile must be default, tick or no_ladders"),
            ({"schema_version": 1, "graded": {"serve_records": True}},
             "require admission graded"),
            # Screen routing gate kappa (SPY1s Phase 2).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "accurate",
                         "deam_inversion": "premium", "seed_gate_kappa": 4.3e-5}},
             "seed_gate_kappa requires deam_profile screen"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "seed_gate_kappa": 0}},
             "seed_gate_kappa must be a positive number"),
            # Sigma-family iterates (SPY1s Phase 2).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "family": "sometimes"}},
             "family must be off, adaptive or always"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "confirm": "band", "family": "adaptive"}},
             "family requires deam_profile screen and confirm always"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "family_c": 3}},
             "family_c requires family adaptive or always"),
            # (sigma, q) families and the family grid (SPY1s Phase 2 step 5).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "family_q": True}},
             "family_q requires family adaptive or always"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen", "deam_cache": True,
                         "deam_inversion": "premium", "family": "adaptive",
                         "grid_kernel": "family"}},
             "grid_kernel family requires family_q and deam_cache"),
            # Family-native iterates (SPY1s Phase 3 lever 1).
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "family": "adaptive",
                         "family_native": True}},
             "family_native requires family_q and confirm always"),
            ({"schema_version": 1, "admission": "graded",
              "graded": {"fit_space": "vol", "deam_profile": "screen",
                         "deam_inversion": "premium", "family": "adaptive", "family_q": True,
                         "family_native": 1}},
             "family_native must be true or false")):
        policy = write_policy(tmp_path / "policy.json", document)
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode != 0, document
        assert message in result.stderr, (document, result.stderr)


def test_native_box_certificate_is_opt_in_with_provenance(binary, tmp_path):
    # graded.certificate box (lane L3r1): on clean quotes every point fit passes the
    # exact certificate, so the box certificate never runs and the predictions are
    # byte-identical to plain graded; only the opt-in provenance is added. An
    # explicit "exact" adds nothing.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    plain_output, box_output, exact_output = (
        tmp_path / "plain.csv", tmp_path / "box.csv", tmp_path / "exact.csv")
    plain_policy = write_policy(tmp_path / "plain.json",
                                {"schema_version": 1, "admission": "graded"})
    plain = invoke(binary, source, plain_output, "--policy", str(plain_policy))
    assert plain.returncode == 0, plain.stderr
    box_policy = write_policy(tmp_path / "box.json", {
        "schema_version": 1, "admission": "graded",
        "graded": {"certificate": "box", "certificate_box_half_ticks": 0.25}})
    box = invoke(binary, source, box_output, "--policy", str(box_policy))
    assert box.returncode == 0, box.stderr
    assert box_output.read_bytes() == plain_output.read_bytes()
    graded = json.loads(box.stdout)["policy_definition"]["graded"]
    assert graded["certificate"] == "box"
    assert graded["certificate_box_half_ticks"] == 0.25
    plain_graded = json.loads(plain.stdout)["policy_definition"]["graded"]
    assert not {"certificate", "certificate_box_half_ticks"} & set(plain_graded)
    exact_policy = write_policy(tmp_path / "exact.json", {
        "schema_version": 1, "admission": "graded", "graded": {"certificate": "exact"}})
    exact = invoke(binary, source, exact_output, "--policy", str(exact_policy))
    assert exact.returncode == 0, exact.stderr
    assert exact_output.read_bytes() == plain_output.read_bytes()
    assert json.loads(exact.stdout)["policy_definition"]["graded"] == plain_graded


def test_native_inbox_projection_is_opt_in_with_provenance(binary, tmp_path):
    # graded.inbox_projection (lane C4): it runs only where the box rung refused. On clean
    # quotes every point fit passes the exact certificate, so the predictions are
    # byte-identical to the box certificate alone; the opt-in provenance is added (the
    # policy echo and a per-slice record, not attempted). An explicit false adds nothing.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    outputs = {name: tmp_path / f"{name}.csv" for name in ("box", "inbox", "off")}
    graded = {
        "box": {"certificate": "box", "certificate_box_half_ticks": 0.0},
        "inbox": {"certificate": "box", "certificate_box_half_ticks": 0.0,
                  "inbox_projection": True},
        "off": {"certificate": "box", "certificate_box_half_ticks": 0.0,
                "inbox_projection": False}}
    reports = {}
    for name, keys in graded.items():
        policy = write_policy(tmp_path / f"{name}.json",
                              {"schema_version": 1, "admission": "graded", "graded": keys})
        run = invoke(binary, source, outputs[name], "--policy", str(policy))
        assert run.returncode == 0, run.stderr
        reports[name] = json.loads(run.stdout)
    assert outputs["inbox"].read_bytes() == outputs["box"].read_bytes()
    assert outputs["off"].read_bytes() == outputs["box"].read_bytes()
    assert reports["inbox"]["policy_definition"]["graded"]["inbox_projection"] is True
    assert "inbox_projection" not in reports["box"]["policy_definition"]["graded"]
    assert reports["off"]["policy_definition"] == reports["box"]["policy_definition"]
    slices = reports["inbox"]["graded_stats"]["slices"]
    assert slices and all(s["inbox_projection"]["outcome"] == "not_attempted" for s in slices)
    assert all("inbox_projection" not in s for s in reports["box"]["graded_stats"]["slices"])
    assert "inbox_projection" not in reports["inbox"]["graded_stats"]


def test_native_market_wing_ceiling_is_opt_in_with_provenance(binary, tmp_path):
    # graded.wing_mult_source market (lane L3r1 lever 3): on clean quotes no served
    # multiple reaches the policy ceiling, so the predictions are byte-identical to
    # plain graded; only the opt-in provenance is added. An explicit "policy" adds
    # nothing.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    outputs = {name: tmp_path / f"{name}.csv" for name in ("plain", "market", "policy")}
    graded = {"plain": None, "market": {"wing_mult_source": "market"},
              "policy": {"wing_mult_source": "policy"}}
    reports = {}
    for name, keys in graded.items():
        document = {"schema_version": 1, "admission": "graded"}
        if keys is not None:
            document["graded"] = keys
        policy = write_policy(tmp_path / f"{name}.json", document)
        result = invoke(binary, source, outputs[name], "--policy", str(policy))
        assert result.returncode == 0, (name, result.stderr)
        reports[name] = json.loads(result.stdout)["policy_definition"]["graded"]
    plain_bytes = outputs["plain"].read_bytes()
    assert outputs["market"].read_bytes() == plain_bytes
    assert outputs["policy"].read_bytes() == plain_bytes
    assert reports["market"]["wing_mult_source"] == "market"
    assert "wing_mult_source" not in reports["plain"]
    assert reports["policy"] == reports["plain"]


def test_native_market_distance_is_opt_in_with_provenance(binary, tmp_path):
    # graded.market_distance (lane L3q): with a bound no served row reaches (the
    # synthetic quotes are 0.002 wide, so the scorer's one-tick spread floor applies),
    # neither the gate nor the repair changes a prediction; only the opt-in
    # provenance is added. An explicit "off" adds nothing.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    graded = {"plain": None,
              "gate": {"market_distance": "gate", "market_distance_spreads": 1000},
              "repair": {"market_distance": "repair", "market_distance_spreads": 500},
              "off": {"market_distance": "off"}}
    outputs = {name: tmp_path / f"{name}.csv" for name in graded}
    reports = {}
    for name, keys in graded.items():
        document = {"schema_version": 1, "admission": "graded"}
        if keys is not None:
            document["graded"] = keys
        policy = write_policy(tmp_path / f"{name}.json", document)
        result = invoke(binary, source, outputs[name], "--policy", str(policy))
        assert result.returncode == 0, (name, result.stderr)
        reports[name] = json.loads(result.stdout)["policy_definition"]["graded"]
    plain_bytes = outputs["plain"].read_bytes()
    for name in ("gate", "repair", "off"):
        assert outputs[name].read_bytes() == plain_bytes, name
    assert reports["gate"]["market_distance"] == "gate"
    assert reports["gate"]["market_distance_spreads"] == 1000
    assert reports["repair"]["market_distance"] == "repair"
    assert reports["repair"]["market_distance_spreads"] == 500
    assert not {"market_distance", "market_distance_spreads"} & set(reports["plain"])
    assert reports["off"] == reports["plain"]


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
    # The accurate-kernel Search cache (L2a) serves exactly the historical price-channel
    # predictions (the R3 kill switch); only its provenance is added to the report.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    plain_output, cached_output = tmp_path / "plain.csv", tmp_path / "cached.csv"
    plain_policy = write_policy(tmp_path / "plain.json", {
        "schema_version": 1, "admission": "graded", "graded": HISTORICAL_GRADED})
    plain = invoke(binary, source, plain_output, "--policy", str(plain_policy))
    assert plain.returncode == 0, plain.stderr
    cached_policy = write_policy(tmp_path / "cached.json", {
        "schema_version": 1, "admission": "graded",
        "graded": {"fit_space": "vol", "deam_profile": "accurate", "deam_cache": True,
                   "deam_inversion": "search", "grid_kernel": "reference",
                   "rung_memo": False}})
    cached = invoke(binary, source, cached_output, "--policy", str(cached_policy))
    assert cached.returncode == 0, cached.stderr
    assert cached_output.read_bytes() == plain_output.read_bytes()
    graded = json.loads(cached.stdout)["policy_definition"]["graded"]
    assert (graded["fit_space"], graded["deam_profile"], graded["deam_cache"]) == (
        "vol", "accurate", True)
    plain_graded = json.loads(plain.stdout)["policy_definition"]["graded"]
    assert not set(ARM_D_GRADED) & set(plain_graded)
    assert not {"deam_inversion", "confirm", "grid_kernel", "rung_memo"} & set(graded)


def test_native_premium_inversion_is_opt_in_with_audit_counters(binary, tmp_path):
    # graded.deam_inversion premium (lane L2e): provenance only when opted in; the
    # --deam-audit run serves the same predictions as the plain premium run and reports
    # the premium attempts and the audit pairs per chain; --deam-audit needs
    # --timing-detail.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    policy = write_policy(tmp_path / "premium.json", {
        "schema_version": 1, "admission": "graded",
        "graded": {"fit_space": "vol", "deam_cache": True, "deam_inversion": "premium"}})
    plain_output, audit_output = tmp_path / "premium.csv", tmp_path / "audit.csv"
    plain = invoke(binary, source, plain_output, "--policy", str(policy))
    assert plain.returncode == 0, plain.stderr
    assert json.loads(plain.stdout)["policy_definition"]["graded"]["deam_inversion"] == "premium"
    audit = invoke(binary, source, audit_output, "--policy", str(policy), "--timing-detail",
                   "--deam-audit")
    assert audit.returncode == 0, audit.stderr
    assert audit_output.read_bytes() == plain_output.read_bytes()
    slices = json.loads(audit.stdout)["graded_stats"]["slices"]
    timings = [entry["timing"] for entry in slices if "timing" in entry]
    assert timings
    premium = sum(t["deam_premium"] for t in timings)
    assert premium > 0
    assert premium == sum(t["deam_inversions"] for t in timings)
    assert sum(t["deam_audit_pairs"] for t in timings) == premium
    assert all(sum(t["deam_audit_dsigma_decades"]) == t["deam_audit_pairs"] for t in timings)
    assert all(sum(t["deam_premium_fallback_by_reason"]) == t["deam_premium_fallbacks"]
               for t in timings)
    refused = invoke(binary, source, tmp_path / "refused.csv", "--policy", str(policy),
                     "--deam-audit")
    assert refused.returncode != 0
    assert "--deam-audit requires --timing-detail" in refused.stderr


def test_native_rung_memo_is_byte_identical_and_opt_in(binary, tmp_path):
    # The per-expiry rung memo (lane L2c) is exact-keyed reuse: with and without the
    # accurate vol-space cache it serves exactly the historical graded predictions, and
    # under the R3 default (memo on) exactly the default's predictions with the memo off;
    # only the memo's provenance differs in the report.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    reference = {}
    for name, graded_keys in (("historical", HISTORICAL_GRADED),
                              ("default-off", {"rung_memo": False})):
        reference[name] = tmp_path / f"{name}.csv"
        policy = write_policy(tmp_path / f"{name}.json", {
            "schema_version": 1, "admission": "graded", "graded": graded_keys})
        plain = invoke(binary, source, reference[name], "--policy", str(policy))
        assert plain.returncode == 0, plain.stderr
        assert "rung_memo" not in json.loads(plain.stdout)["policy_definition"]["graded"]
    for name, base, graded_keys in (
            ("memo", "historical", {**HISTORICAL_GRADED, "rung_memo": True}),
            ("memo-vol", "historical", {"fit_space": "vol", "deam_profile": "accurate",
                                        "deam_cache": True, "deam_inversion": "search",
                                        "grid_kernel": "reference", "rung_memo": True}),
            ("memo-default", "default-off", {})):
        output = tmp_path / f"{name}.csv"
        policy = write_policy(tmp_path / f"{name}.json", {
            "schema_version": 1, "admission": "graded", "graded": graded_keys})
        keyed = invoke(binary, source, output, "--policy", str(policy))
        assert keyed.returncode == 0, keyed.stderr
        assert output.read_bytes() == reference[base].read_bytes(), name
        assert json.loads(keyed.stdout)["policy_definition"]["graded"]["rung_memo"] is True


def test_native_graded_default_is_arm_d_and_the_kill_switch_restores_the_historical_path(
        binary, tmp_path):
    # PM ruling R3: a graded policy without vol-space keys runs SPY1s Phase 1 arm D (its
    # explicit policy is byte-identical and prints the same definition); HISTORICAL_GRADED
    # prints the pre-R3 definition (no vol-space, grid or memo key) and serves the same rows
    # as the default within the documented price tolerance; a static policy is untouched.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    runs = {}
    for name, document in (
            ("default", {"schema_version": 1, "admission": "graded"}),
            ("arm-d", {"schema_version": 1, "admission": "graded", "graded": ARM_D_GRADED}),
            ("historical", {"schema_version": 1, "admission": "graded",
                            "graded": HISTORICAL_GRADED}),
            ("static", {"schema_version": 1})):
        output = tmp_path / f"{name}.csv"
        result = invoke(binary, source, output, "--policy",
                        str(write_policy(tmp_path / f"{name}.json", document)))
        assert result.returncode == 0, result.stderr
        with output.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        runs[name] = (output.read_bytes(), json.loads(result.stdout), rows)
    default = runs["default"][1]["policy_definition"]["graded"]
    for key, value in ARM_D_GRADED.items():
        assert default[key] == value, key
    assert runs["arm-d"][0] == runs["default"][0]
    assert runs["arm-d"][1]["policy_definition"] == runs["default"][1]["policy_definition"]
    assert not set(ARM_D_GRADED) & set(runs["historical"][1]["policy_definition"]["graded"])
    assert "graded" not in runs["static"][1]["policy_definition"]
    for on, off in zip(runs["default"][2], runs["historical"][2]):
        assert (on["row_id"], on["status"], on["grade"]) == (off["row_id"], off["status"],
                                                            off["grade"])
        if on["status"] == "ok":
            assert abs(float(on["predicted_price"]) - float(off["predicted_price"])) \
                <= ARM_D_PRICE_TOLERANCE


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


def test_native_ladder_wing_tail_key_is_strict_echoed_and_inert_on_clean_quotes(binary, tmp_path):
    # Lane L3a: graded.ladder_wing_tail. Clean quotes have no repaired node, so the
    # key changes no row; it is echoed only when on and needs the knot-first ladder.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    graded = {"schema_version": 1, "admission": "graded", "price_only": True}
    for document, message in (
            ({**graded, "graded": {"ladder_wing_tail": "floor-price-only"}},
             "ladder_wing_tail requires ladder knot-first"),
            ({**graded, "graded": {"ladder": "off", "ladder_wing_tail": "floor-price-only"}},
             "ladder_wing_tail requires ladder knot-first"),
            ({**graded, "graded": {"ladder": "knot-first", "ladder_wing_tail": "slope"}},
             "ladder_wing_tail must be off, floor-price-only or floor-price-only+wing-floor")):
        policy = write_policy(tmp_path / "bad.json", document)
        result = invoke(binary, source, tmp_path / "bad.csv", "--policy", str(policy))
        assert result.returncode != 0, document
        assert message in result.stderr, (document, result.stderr)
    runs = {}
    for name, extra in (("plain", {}), ("off", {"ladder_wing_tail": "off"}),
                        ("on", {"ladder_wing_tail": "floor-price-only"}),
                        ("floor", {"ladder_wing_tail": "floor-price-only+wing-floor"})):
        output = tmp_path / f"{name}.csv"
        policy = write_policy(tmp_path / f"{name}.json",
                              {**graded, "graded": {"ladder": "knot-first", **extra}})
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode == 0, result.stderr
        runs[name] = (output.read_bytes(), json.loads(result.stdout))
    assert runs["off"][0] == runs["plain"][0]
    assert runs["on"][0] == runs["plain"][0]
    assert "ladder_wing_tail" not in runs["plain"][1]["policy_definition"]["graded"]
    assert "ladder_wing_tail" not in runs["off"][1]["policy_definition"]["graded"]
    assert runs["on"][1]["policy_definition"]["graded"]["ladder_wing_tail"] == "floor-price-only"
    assert runs["on"][1]["price_only_rows"] == 0
    # The wing floor runs (it may move the clean chain's knot wing) and is echoed.
    assert (runs["floor"][1]["policy_definition"]["graded"]["ladder_wing_tail"]
            == "floor-price-only+wing-floor")


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


# Lane B0 benchmark harness (tools/bench_clocks.hpp, docs/benchmark-clocks.md): the bench
# object's clocks, exclusive stages, counters, cold reset and affinity receipt.
BENCH_STAGES = ("normalization", "family_build_validation", "inverses", "endpoints",
                "curve_repair", "final_grid_validation", "reference_exceptions", "publication",
                "fit_other", "serve_build")
SERIAL = {"ATX_VOL_FIT_WORKERS": "1"}
# A cold rep 2 is not faster than this share of rep 1 (docs/benchmark-clocks.md); a warm
# memo hit skips every inversion and runs several times faster.
COLD_REP_MIN_RATIO = 0.5


def make_american_snapshot(path: Path, expiries: int) -> None:
    # Puts and calls on spot 100 at a 5% rate, no dividends: the puts carry an early-exercise
    # premium, so admission inverts them on the American kernel. Calls are European values
    # (no early exercise without dividends); puts are European values floored at intrinsic.
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
            years = 0.1 + 0.05 * expiry
            for step in range(-8, 9):
                strike = 100.0 + 2.5 * step
                sigma = 0.25 - 0.2 * math.log(strike / 100.0)
                sigma_t = sigma * math.sqrt(years)
                d1 = (math.log(100.0 / strike) + 0.05 * years) / sigma_t + 0.5 * sigma_t
                discount = strike * math.exp(-0.05 * years)
                call = 100.0 * phi(d1) - discount * phi(d1 - sigma_t)
                put = max(call - 100.0 + discount, strike - 100.0, 0.0)
                for is_call, price in ((1, call), (0, put)):
                    half = max(0.02, 0.02 * price)
                    writer.writerow([
                        row, 2000 + expiry, strike, is_call, 100, max(price - half, 0.0),
                        price + half, years, years, 0.05, 0, 0, "ok", "American", "AAA", "AAA",
                        round(years * 365.25 * 1000), 1,
                    ])
                    row += 1


def bench_run(binary, source, output, *extra, workers="1"):
    env = {**os.environ, "ATX_VOL_FIT_WORKERS": workers}
    result = invoke(binary, source, output, *extra, env=env)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_native_bench_clocks_are_reported_and_stages_reconcile(binary, tmp_path):
    source = tmp_path / "quotes.csv"
    make_american_snapshot(source, 4)
    policy = write_policy(tmp_path / "graded.json", {"schema_version": 1, "admission": "graded"})
    plain_output, timed_output = tmp_path / "plain.csv", tmp_path / "timed.csv"
    plain = bench_run(binary, source, plain_output, "--policy", str(policy))
    timed = bench_run(binary, source, timed_output, "--policy", str(policy), "--timing-detail",
                      "--reps", "2", "--cold-reset")
    # The harness only measures: predictions are byte-identical to the plain run.
    assert timed_output.read_bytes() == plain_output.read_bytes()
    bench = plain["bench"]
    assert bench["schema"] == "atx-vol-bench-clocks-v2"
    assert bench["reps"] == 1 and bench["cold_reset"] is False
    assert bench["stages"] == "unavailable: requires --timing-detail"
    assert bench["clocks"]["ready_to_quote_s"]["status"] == "not_built"
    assert bench["affinity"]["pinned"] is False
    assert bench["build_info"]["fast_math"] is False
    assert bench["counters"]["simd"]["vector_pack_dispatches"].startswith("unavailable")
    bench = timed["bench"]
    clocks = bench["clocks"]
    assert clocks["cold_calibration_s"]["summary"]["n"] == 2
    assert set(clocks["legacy_fit_reprice_s"]["summary"]) == {"n", "min", "median", "p95",
                                                              "p99", "max"}
    for rep in bench["per_rep"]:
        # Cold calibration contains the legacy clock (it adds classification/grouping).
        assert rep["cold_calibration_s"] >= rep["legacy_fit_reprice_s"]
        assert rep["predictions_identical_to_rep1"] is True
        assert rep["cold_reset"]["applied"] is True
        assert rep["deam_memo_entries_at_start"] == 0
    assert timed["fit_and_reprice_seconds"] == bench["per_rep"][0]["legacy_fit_reprice_s"]
    process = clocks["process_wall_s"]
    assert process["status"] == "measured"
    timers = process["sub_timers"]
    assert {"startup_s", "setup_s", "read_s", "decode_s", "compute_s", "write_s",
            "policy_load_s"} <= set(timers)
    assert process["value"] >= timers["compute_s"] >= sum(r["cold_calibration_s"]
                                                           for r in bench["per_rep"])
    stages = bench["stages"]
    assert set(stages["stages"]) == set(BENCH_STAGES)
    assert stages["stages"]["family_build_validation"] == {
        "seconds": None, "status": "not_built",
        "reason": stages["stages"]["family_build_validation"]["reason"]}
    assert stages["stages"]["serve_build"]["status"] == "not_built"  # graded.serve_records off
    timed_stages = [s["seconds"] for s in stages["stages"].values() if s["status"] == "timed"]
    assert all(seconds >= 0.0 for seconds in timed_stages)
    assert sum(timed_stages) == pytest.approx(stages["sum_s"], rel=1e-9, abs=1e-12)
    # unattributed_s is the cold clock (its own stamps) minus the measured stages: time no
    # timer claimed (surface scheduling, a fit lap's unmeasured remainder), not zero by
    # construction; the lap clock's unclaimed time explains it to rounding.
    assert stages["unattributed_s"] == pytest.approx(
        stages["cold_calibration_s"] - stages["sum_s"], rel=1e-9, abs=1e-12)
    assert stages["unclaimed_lap_s"] >= 0.0
    assert abs(stages["span_check_s"]) <= 1e-9
    assert stages["unattributed_s"] > 0.0
    assert stages["reconciles_within_1pct"] is (
        abs(stages["unattributed_s"]) <= 0.01 * stages["cold_calibration_s"])
    assert stages["attribution_valid"] is True and stages["attribution_conflict_s"] == 0.0
    assert stages["stages"]["inverses"]["seconds"] > 0.0
    assert stages["stages"]["final_grid_validation"]["seconds"] > 0.0
    counters = bench["counters"]
    rows = counters["rows"]
    assert rows["input_rows"] == timed["rows"] and rows["served_rows"] == timed["served_rows"]
    assert rows["served_iv_rows"] == timed["served_rows"]
    assert sum(rows["rows_by_status"].values()) == timed["rows"]
    families = counters["families"]
    assert families["built"] == 0 and families["iterate_prices"] == 0  # family route off
    # K4: the family sides the fit requested (and refused) are counted; none with the route off.
    assert families["attempted"] == 0 and families["refused"] == 0
    route = counters["fit_route"]
    assert route["timed_chains"] == 4
    assert route["al_boundary_solves"] == sum(route["al_boundary_solves_by_bucket"].values())
    assert route["inversions"] > 0 and route["certification_passes"] >= 4
    assert sum(route["premium_fallbacks_by_reason"]) == route["premium_fallbacks"]
    assert counters["deam_stage_clock"]["endpoint_bands"] >= 0  # bands only on channel fits
    assert plain["bench"]["counters"]["families"] == "unavailable: requires --timing-detail"
    ledger = [rep["solve_ledger"]["sl_al_boundary_solves"] for rep in bench["per_rep"]]
    assert ledger[0] == ledger[1] == counters["solve_ledger"]["sl_al_boundary_solves"] > 0


def test_native_cold_reset_repeats_rep1_work_where_a_memo_crosses_reps(binary, tmp_path):
    # rung_memo false: the graded fits read the de-Am cache from the fitting thread's memo,
    # which survives an in-process rep unless --cold-reset clears it.
    source = tmp_path / "quotes.csv"
    make_american_snapshot(source, 4)
    policy = write_policy(tmp_path / "thread-memo.json", {
        "schema_version": 1, "admission": "graded", "graded": {"rung_memo": False}})
    warm = bench_run(binary, source, tmp_path / "warm.csv", "--policy", str(policy),
                     "--reps", "2")["bench"]["per_rep"]
    cold = bench_run(binary, source, tmp_path / "cold.csv", "--policy", str(policy),
                     "--reps", "2", "--cold-reset")["bench"]["per_rep"]

    def solves(rep):
        return rep["solve_ledger"]["sl_al_boundary_solves"]

    kept = warm[1]["deam_memo_entries_at_start"]
    assert kept >= 4  # at least one cache per expiry survives rep 1
    assert solves(warm[1]) < solves(warm[0])
    assert cold[1]["cold_reset"]["cleared"][0]["entries_before"] == kept
    assert cold[1]["deam_memo_entries_at_start"] == 0
    assert solves(cold[1]) == solves(cold[0]) == solves(warm[0])
    assert all(rep["predictions_identical_to_rep1"] for rep in warm + cold)
    assert cold[1]["cold_calibration_s"] >= COLD_REP_MIN_RATIO * cold[0]["cold_calibration_s"]
    assert (tmp_path / "warm.csv").read_bytes() == (tmp_path / "cold.csv").read_bytes()


def test_native_affinity_pins_the_process_and_reports_the_core(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "pinned.csv"
    make_quotes(source)
    last_cpu = str((os.cpu_count() or 1) - 1)
    receipt = bench_run(binary, source, output, "--curve", "essvi", "--affinity", last_cpu)
    affinity = receipt["bench"]["affinity"]
    assert affinity["pinned"] is True and affinity["requested_cpu"] == int(last_cpu)
    assert affinity["process_affinity_mask"] == hex(1 << int(last_cpu))
    assert affinity["thread_affinity_mask"] == hex(1 << int(last_cpu))
    assert affinity["current_cpu_at_receipt"] == int(last_cpu)
    assert affinity["fit_workers"]["resolved_auto"] == 1
    assert affinity["nested_pools"]["surface_expiry_fanout"] == 1
    assert isinstance(affinity["is_pcore"], bool) and affinity["cpu_model"]
    refused_output = tmp_path / "refused.csv"
    required = invoke(binary, source, refused_output, "--curve", "essvi", "--affinity", last_cpu,
                      "--require-pcore", env={**os.environ, **SERIAL})
    if affinity["is_pcore"]:
        assert required.returncode == 0, required.stderr
    else:  # an efficiency core (hybrid host): refused before any output
        assert required.returncode != 0
        assert "is not on a performance core" in required.stderr
        assert not refused_output.exists()


@pytest.mark.parametrize(("extra", "workers", "message"), [
    (("--reps", "0"), "1", "--reps must be 1..1000"),
    (("--reps", "1001"), "1", "--reps must be 1..1000"),
    (("--require-pcore",), "1", "--require-pcore requires --affinity"),
    (("--affinity", "0"), "2", "--affinity requires ATX_VOL_FIT_WORKERS=1"),
    (("--model", "common-markov", "--reps", "2"), "1", "require --model quote-slice"),
    (("--model", "common-markov", "--cold-reset"), "1", "require --model quote-slice"),
])
def test_native_bench_flags_are_validated_before_output(binary, tmp_path, extra, workers,
                                                        message):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    result = invoke(binary, source, output, *extra,
                    env={**os.environ, "ATX_VOL_FIT_WORKERS": workers})
    assert result.returncode != 0
    assert message in result.stderr
    assert not output.exists()


# ── Lane C0: identifiability publication, convention metadata, one spot per snapshot ──
#
# The policy keys `serve` {floor_price_only, metadata} and `snapshot` {one_spot,
# stale_spot_bps, stale_spot} are opt-in: absent (or off) they leave every prediction byte
# for byte as before.


def crr_american(spot: float, strike: float, years: float, sigma: float, rate: float,
                 q: float, call: bool, steps: int) -> float:
    dt = years / steps
    u = math.exp(sigma * math.sqrt(dt))
    d = 1.0 / u
    p = (math.exp((rate - q) * dt) - d) / (u - d)
    disc = math.exp(-rate * dt)
    sign = 1.0 if call else -1.0
    values = [max(sign * (spot * u**j * d ** (steps - j) - strike), 0.0) for j in range(steps + 1)]
    for i in range(steps - 1, -1, -1):
        for j in range(i + 1):
            exercise = sign * (spot * u**j * d ** (i - j) - strike)
            values[j] = max(disc * (p * values[j + 1] + (1.0 - p) * values[j]), exercise)
    return values[0]


C0_COLUMNS = ["price_only_reason", "iv_lower", "iv_upper", "vol_convention", "model_kind",
              "dividend_convention", "quote_ns", "underlying_ns", "spot_version", "spot_rule",
              "carry_version", "carry_convention", "raw_spot", "spot_stale", "price_valid",
              "vol_valid"]
C0_DEEP_ITM_PUT = "39"  # K 136 put of the wing chain


@pytest.fixture(scope="module")
def wing_chain(tmp_path_factory) -> Path:
    """The L3a-b wing chain (quote_slice_identification_test.cpp wing_chain): spot 99.71,
    T 0.1, sigma 0.15, r 0.03, strikes 60-140 every 4, a 0.06 yield on alternate strikes
    below 90, +/- 5 % spreads (at least 0.01), 400-step CRR prices."""
    path = tmp_path_factory.mktemp("c0") / "wing.csv"
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["row_id", "chain_id", "strike", "is_call", "spot", "bid", "ask",
                         "years", "years_calendar", "rate", "sdiv", "ddiv_pv", "input_status",
                         "exercise", "underlier", "root", "expiry_days", "snapshot_ns"])
        row = 0
        for strike in range(60, 141, 4):
            q = 0.06 if strike < 90 and ((strike - 60) // 4) % 2 == 0 else 0.0
            for call in (1, 0):
                price = crr_american(99.71, strike, 0.1, 0.15, 0.03, q, call == 1, 400)
                spread = max(0.01, price * 0.05)
                writer.writerow([row, 17, strike, call, 99.71, max(price - spread, 0.0),
                                 price + spread, 0.1, 0.1, 0.03, q, 0, "ok", "American", "WWW",
                                 "WWW", 21000, 1])
                row += 1
    return path


def c0_graded_policy(path: Path, **extra) -> Path:
    """The CLI's graded-v1 historical arm on the wing chain (the library test's graded_v1)."""
    graded = {"term_prior_weight": 1.0, "prior_decay_log_time": 1.0, "prior_quote_halflife": 6.0,
              "channel_node_weight": 0.5, "calendar_retry_factor": 4.0,
              "grade_b_min_mid_strikes": 4, "calendar_repair_half_spreads": 0.5,
              **HISTORICAL_GRADED}
    profiles = [{"name": name, "base": name, "channel": {"repair_half_spreads": 0.5}}
                for name in PROFILE_NAMES]
    return write_policy(path, {"schema_version": 1, "admission": "graded", "graded": graded,
                               "profiles": profiles, **extra})


def run_c0(binary: Path, source: Path, tmp_path: Path, name: str, policy: Path):
    output, report = tmp_path / f"{name}.csv", tmp_path / f"{name}-native.json"
    result = invoke(binary, source, output, "--policy", str(policy), "--report", str(report))
    assert result.returncode == 0, result.stderr
    return read_quotes(output), json.loads(report.read_text()), output


def test_native_c0_serve_and_snapshot_keys_are_strict(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    routed = [{"name": name, "base": name, "dividends": "inferred-schedule"}
              for name in PROFILE_NAMES]
    base = {"schema_version": 1}
    for document, message in (
            ({**base, "serve": {"floor_price_only": "yes"}},
             "serve floor_price_only must be true or false"),
            ({**base, "serve": {"metadata": 1}}, "serve metadata must be true or false"),
            ({**base, "serve": {"price_only": True}}, "Unknown policy key: price_only"),
            ({**base, "serve": True}, "must be a JSON object"),
            ({**base, "snapshot": {"one_spot": "first"}},
             "one_spot must be off, nearest or median"),
            ({**base, "snapshot": {"stale_spot_bps": 3}},
             "stale_spot keys require one_spot nearest or median"),
            ({**base, "snapshot": {"one_spot": "off", "stale_spot": "refuse"}},
             "stale_spot keys require one_spot nearest or median"),
            ({**base, "snapshot": {"one_spot": "median", "stale_spot": "drop"}},
             "stale_spot must be flag or refuse"),
            ({**base, "snapshot": {"one_spot": "median", "stale_spot_bps": -1}},
             "outside supported bounds"),
            ({**base, "snapshot": {"one_spot": "nearest", "spot": 1}},
             "Unknown policy key: spot"),
            ({**base, "snapshot": {"one_spot": "nearest"}, "profiles": routed},
             "one_spot requires dividends aggregate")):
        policy = write_policy(tmp_path / "policy.json", document)
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode != 0, document
        assert message in result.stderr, (document, result.stderr)
        assert not output.exists()


def test_native_call_floor_key_is_strict_graded_only_and_inert_when_legacy(
        binary, tmp_path, wing_chain):
    # Lane C3: serve.call_floor = legacy | discounted_intrinsic, graded admission only.
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    for document, message in (
            ({"schema_version": 1, "serve": {"call_floor": "discount"}},
             "serve call_floor must be legacy or discounted_intrinsic"),
            ({"schema_version": 1, "serve": {"call_floor": True}}, "must be"),
            ({"schema_version": 1, "serve": {"call_floor": "discounted_intrinsic"}},
             "serve call_floor requires admission graded")):
        policy = write_policy(tmp_path / "policy.json", document)
        result = invoke(binary, source, output, "--policy", str(policy))
        assert result.returncode != 0, document
        assert message in result.stderr, (document, result.stderr)
        assert not output.exists()
    plain = c0_graded_policy(tmp_path / "plain.json")
    legacy = c0_graded_policy(tmp_path / "legacy.json", serve={"call_floor": "legacy"})
    floored = c0_graded_policy(tmp_path / "floored.json",
                               serve={"call_floor": "discounted_intrinsic"})
    _, plain_report, plain_csv = run_c0(binary, wing_chain, tmp_path, "plain", plain)
    _, legacy_report, legacy_csv = run_c0(binary, wing_chain, tmp_path, "legacy", legacy)
    _, on_report, _ = run_c0(binary, wing_chain, tmp_path, "floored", floored)
    assert legacy_csv.read_bytes() == plain_csv.read_bytes()
    assert "serve" not in legacy_report["policy_definition"]
    assert "call_floor" not in plain_report["graded_stats"]
    assert on_report["policy_definition"]["serve"] == {
        "floor_price_only": False, "metadata": False, "call_floor": "discounted_intrinsic"}
    # Opt-in provenance per slice: the wing chain is one dividend-bearing snapshot (alternate
    # strikes carry a yield), so it lists no line and lifts nothing.
    slices = on_report["graded_stats"]["slices"]
    assert all(s["call_floor"] == {"lines": 0, "lifted_nodes": 0, "refused": False}
               for s in slices)


def test_native_c0_keys_off_leave_predictions_unchanged(binary, tmp_path, wing_chain):
    plain = c0_graded_policy(tmp_path / "plain.json")
    off = c0_graded_policy(tmp_path / "off.json",
                           serve={"floor_price_only": False, "metadata": False},
                           snapshot={"one_spot": "off"})
    _, plain_report, plain_csv = run_c0(binary, wing_chain, tmp_path, "plain", plain)
    _, off_report, off_csv = run_c0(binary, wing_chain, tmp_path, "off", off)
    assert off_csv.read_bytes() == plain_csv.read_bytes()
    for report in (plain_report, off_report):
        for key in ("identifiability", "output_conventions", "one_spot"):
            assert key not in report
    assert "serve" not in plain_report["policy_definition"]
    assert "snapshot" not in off_report["policy_definition"]
    # Metadata only appends columns: every historical column is unchanged.
    meta = c0_graded_policy(tmp_path / "meta.json", serve={"metadata": True})
    _, meta_report, meta_csv = run_c0(binary, wing_chain, tmp_path, "meta", meta)
    plain_lines = plain_csv.read_text().splitlines()
    meta_lines = meta_csv.read_text().splitlines()
    assert len(meta_lines) == len(plain_lines)
    for before, after in zip(plain_lines, meta_lines):
        assert after.startswith(before + ",")
    assert meta_lines[0].split(",")[-len(C0_COLUMNS):] == C0_COLUMNS
    assert meta_report["output_conventions"]["columns"] == ",".join(C0_COLUMNS)


def test_native_floor_price_only_serves_the_deep_itm_put_price_only_with_its_price(
        binary, tmp_path, wing_chain):
    # The historical graded arm serves the K 136 put at its exercise value with the floor
    # volatility 0.005 as an identified IV; serve.floor_price_only serves the same price
    # price-only (floor_vol) and no floor volatility at all.
    off_policy = c0_graded_policy(tmp_path / "off.json", serve={"metadata": True})
    on_policy = c0_graded_policy(tmp_path / "on.json",
                                 serve={"floor_price_only": True, "metadata": True})
    off, off_report, _ = run_c0(binary, wing_chain, tmp_path, "off", off_policy)
    on, on_report, _ = run_c0(binary, wing_chain, tmp_path, "on", on_policy)
    before = next(row for row in off if row["row_id"] == C0_DEEP_ITM_PUT)
    after = next(row for row in on if row["row_id"] == C0_DEEP_ITM_PUT)
    assert before["status"] == "ok" and float(before["predicted_vol"]) == 0.005
    # A repair moved its node to the exercise value; the re-implied root is the floor. The
    # Legacy row keeps that value but is labelled with the library query's answer (one
    # answer per contract): no convention, not a valid volatility, floor_vol and the
    # interval the price-only arm publishes.
    assert before["vol_convention"] == "none"
    assert (before["price_valid"], before["vol_valid"]) == ("1", "0")
    assert before["price_only_reason"] == "floor_vol"
    off_identifiability = off_report["identifiability"]
    assert off_identifiability["identified_floor_vol_rows"] >= 1
    assert off_identifiability["identified_vol_rows"] == sum(row["vol_valid"] == "1"
                                                             for row in off)
    assert (off_identifiability["identified_vol_rows"] + off_identifiability[
        "identified_floor_vol_rows"] == off_identifiability["served_vol_rows"])
    assert after["status"] == "ok_price_only"
    assert after["predicted_price"] == before["predicted_price"]
    assert after["predicted_vol"] == "" and after["vol_status"] == "unidentifiable"
    assert after["price_only_reason"] == "floor_vol"
    assert float(after["iv_lower"]) == 0.0 and float(after["iv_upper"]) > 0.005
    assert (before["iv_lower"], before["iv_upper"]) == (after["iv_lower"], after["iv_upper"])
    assert after["vol_convention"] == "none"
    assert (after["price_valid"], after["vol_valid"]) == ("1", "0")
    # No floor value is ever served as an identified IV under the key.
    assert not [row for row in on if row["predicted_vol"] and float(row["predicted_vol"]) <= 0.005]
    identifiability = on_report["identifiability"]
    assert identifiability["identified_floor_vol_rows"] == 0
    assert identifiability["floor_price_only"] is True
    ok = sum(row["status"] == "ok" for row in on)
    price_only = sum(row["status"] == "ok_price_only" for row in on)
    assert identifiability["served_vol_rows"] == ok
    assert identifiability["identified_vol_rows"] == ok
    assert identifiability["served_price_rows"] == ok + price_only
    assert identifiability["price_only_reason_counts"]["floor_vol"] >= 1
    assert sum(identifiability["price_only_reason_counts"].values()) == price_only
    # Price coverage never shrinks: every price the historical arm served is served.
    served = {row["row_id"]: row["predicted_price"] for row in off if row["predicted_price"]}
    assert all(next(r for r in on if r["row_id"] == i)["predicted_price"] == price
               for i, price in served.items())
    assert on_report["policy_definition"]["serve"] == {"floor_price_only": True,
                                                       "metadata": True}
    # serve.floor_price_only implies price_only (its PriceOnly rows - every reason,
    # unidentified_interval included - are served ok_price_only); the definition says so.
    assert on_report["policy_definition"]["price_only"] is True
    assert "price_only" not in off_report["policy_definition"]
    # Without metadata the key still publishes the price-only row (historical columns).
    bare = c0_graded_policy(tmp_path / "bare.json", serve={"floor_price_only": True})
    rows, _, _ = run_c0(binary, wing_chain, tmp_path, "bare", bare)
    row = next(row for row in rows if row["row_id"] == C0_DEEP_ITM_PUT)
    assert (row["status"], row["predicted_price"]) == ("ok_price_only", before["predicted_price"])
    assert "price_only_reason" not in row


def write_calendar_repair_snapshot(path: Path, drift: float) -> None:
    """quote_slice_identification_test's calendar-repair surface in CSV form (400-step CRR
    prices, r 5 %): N (chain 17, 0.45 y, sigma 50 %, q 4 %, puts 116-148) recorded at spot
    100 + `drift` and F (chain 18, 0.5 y, sigma 5 % skewed up to 35 % below K 116, puts
    60-148) at spot 100. The calendar repair caps N's puts onto F's exercise values; with
    `drift` 1e-6 N's K 116 put (row 0) is then a hair above its own exercise value
    (measured: C0-20221022-fix1 cli-two-expiry-reasons.txt)."""
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["row_id", "chain_id", "strike", "is_call", "spot", "bid", "ask",
                         "years", "years_calendar", "rate", "sdiv", "ddiv_pv", "input_status",
                         "exercise", "underlier", "root", "expiry_days", "snapshot_ns"])
        row = 0
        chains = ((17, 0.45, 100.0 + drift, 116, 0.04, 0.5, 0.0),
                  (18, 0.5, 100.0, 60, 0.0, 0.05, 0.30))
        for chain, years, spot, low, q, atm, skew in chains:
            for strike in range(low, 149, 4):
                sigma = atm + skew * min(max((116.0 - strike) / 40.0, 0.0), 1.0)
                price = crr_american(spot, float(strike), years, sigma, 0.05, q, False, 400)
                spread = max(0.01, price * 0.05)
                writer.writerow([row, chain, strike, 0, spot, max(price - spread, 0.0),
                                 price + spread, years, years, 0.05, q, 0, "ok", "American",
                                 "XYZ", "XYZ", 21000 + int(years * 365), 1])
                row += 1


def calendar_repair_policy(path: Path, *, graded=None, **extra) -> Path:
    profiles = [{"name": name, "base": name, "side_policy": "all",
                 "channel": {"repair_half_spreads": 100}} for name in PROFILE_NAMES]
    return write_policy(path, {
        "schema_version": 1, "admission": "graded",
        "graded": {"grade_b_min_mid_strikes": 10, "calendar_repair_half_spreads": 100,
                   **(graded or {})},
        "profiles": profiles, **extra})


@pytest.fixture(scope="module")
def calendar_repair_chain(tmp_path_factory) -> Path:
    path = tmp_path_factory.mktemp("c0-plateau") / "calendar-repair.csv"
    write_calendar_repair_snapshot(path, 1e-6)
    return path


def test_native_flat_exercise_region_and_low_vega_rows_keep_their_price(
        binary, tmp_path, calendar_repair_chain):
    # N's repaired puts K 116-140 re-imply only a non-unique root: K 116 inside the
    # kernel's exercise region (flat_exercise_region, interval [0, sigma_ex]), K 120-140
    # outside it but uncertified (low_vega, no interval). serve.floor_price_only serves
    # them price-only with their price; the Legacy arm keeps the historical root but says
    # it is not identified - the same reasons, interval and price.
    on, on_report, _ = run_c0(binary, calendar_repair_chain, tmp_path, "on",
                              calendar_repair_policy(tmp_path / "on.json", serve={
                                  "floor_price_only": True, "metadata": True}))
    off, off_report, _ = run_c0(binary, calendar_repair_chain, tmp_path, "off",
                                calendar_repair_policy(tmp_path / "off.json",
                                                       serve={"metadata": True}))
    expected = {"0": "flat_exercise_region", **{str(i): "low_vega" for i in range(1, 7)}}
    for row in on:
        reason = expected.get(row["row_id"])
        if reason is None:
            assert (row["status"], row["price_only_reason"]) == ("ok", "none"), row
            assert row["vol_valid"] == "1"
            continue
        assert row["status"] == "ok_price_only" and row["predicted_vol"] == "", row
        assert row["price_only_reason"] == reason
        assert row["vol_convention"] == "none"
        assert (row["price_valid"], row["vol_valid"]) == ("1", "0")
    by_id = {row["row_id"]: row for row in on}
    assert by_id["0"]["predicted_price"] == "16"  # F's exercise value at K 116
    assert float(by_id["0"]["iv_lower"]) == 0.0 and 0.13 < float(by_id["0"]["iv_upper"]) < 0.14
    assert all(by_id[i]["iv_lower"] == by_id[i]["iv_upper"] == "" for i in map(str, range(1, 7)))
    reasons = on_report["identifiability"]["price_only_reason_counts"]
    assert reasons == {"flat_exercise_region": 1, "low_vega": 6}
    for row in off:
        twin = by_id[row["row_id"]]
        assert row["predicted_price"] == twin["predicted_price"]
        assert (row["price_only_reason"], row["iv_lower"], row["iv_upper"]) == (
            twin["price_only_reason"], twin["iv_lower"], twin["iv_upper"])
        if row["row_id"] in expected:
            assert row["status"] == "ok" and float(row["predicted_vol"]) > 0.005, row
            assert (row["vol_convention"], row["vol_valid"]) == ("none", "0")
    identifiability = off_report["identifiability"]
    assert identifiability["served_vol_rows"] - identifiability["identified_vol_rows"] == 7
    assert identifiability["identified_floor_vol_rows"] == 0


def test_native_market_gated_rows_carry_no_price_only_reason(
        binary, tmp_path, calendar_repair_chain):
    # The market-distance gate refuses N's repaired K 116-124 puts (F's exercise values sit
    # more than a spread below N's own quotes), which serve.floor_price_only alone serves
    # flat_exercise_region / low_vega: a refused row has no served price, so no reason or
    # interval either.
    rows, report, _ = run_c0(binary, calendar_repair_chain, tmp_path, "gated",
                             calendar_repair_policy(
                                 tmp_path / "gated.json",
                                 graded={"market_distance": "gate",
                                         "market_distance_spreads": 1.0},
                                 serve={"floor_price_only": True, "metadata": True}))
    gated = [row for row in rows if row["status"] == "market_distance_failure"]
    assert {row["row_id"] for row in gated} >= {"0", "1", "2"}
    for row in gated:
        assert (row["price_only_reason"], row["iv_lower"], row["iv_upper"]) == ("none", "", "")
        assert (row["vol_convention"], row["price_valid"], row["vol_valid"]) == ("none", "0", "0")
    assert report["identifiability"]["price_only_reason_counts"] == {"low_vega": 4}  # K 128-140


def run_graded(binary: Path, source: Path, tmp_path: Path, name: str, document: dict):
    policy = write_policy(tmp_path / f"{name}.json", document)
    output, native = tmp_path / f"{name}.csv", tmp_path / f"{name}-native.json"
    result = invoke(binary, source, output, "--policy", str(policy), "--report", str(native))
    assert result.returncode == 0, (name, result.stderr)
    return output, json.loads(result.stdout), json.loads(native.read_text())


def test_native_publication_validation_is_opt_in_with_provenance(binary, tmp_path):
    # graded.publication validate (lane C2): on clean quotes nothing is repaired, gated or
    # found, so the predictions are byte-identical to plain graded; the policy definition
    # echoes the key and the native report carries the opt-in publication counters. An
    # explicit "off" adds nothing.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    runs = {}
    for name, keys in {"plain": None, "validate": {"publication": "validate"},
                       "off": {"publication": "off"}}.items():
        document = {"schema_version": 1, "admission": "graded"}
        if keys is not None:
            document["graded"] = keys
        runs[name] = run_graded(binary, source, tmp_path, name, document)
    plain_bytes = runs["plain"][0].read_bytes()
    assert runs["validate"][0].read_bytes() == plain_bytes
    assert runs["off"][0].read_bytes() == plain_bytes
    definitions = {name: run[1]["policy_definition"]["graded"] for name, run in runs.items()}
    assert definitions["validate"]["publication"] == "validate"
    assert "publication" not in definitions["plain"]
    assert definitions["off"] == definitions["plain"]
    stats = {name: run[2]["graded_stats"] for name, run in runs.items()}
    counters = stats["validate"]["publication"]
    for key in ("violations", "label_violations", "rollbacks", "withheld_rows",
                "joint_nonconverged", "joint_infeasible"):
        assert counters[key] == 0, key
    assert all(entry["publication"]["violations"] == 0 for entry in stats["validate"]["slices"])
    for name in ("plain", "off"):
        assert "publication" not in stats[name]
        assert not any("publication" in entry for entry in stats[name]["slices"])


def test_native_serve_records_are_opt_in_with_provenance(binary, tmp_path):
    # graded.serve_records (lane H1): after each served slice's fit the serve_build stage
    # builds its serving record, publishes it into the run's book and audits the
    # evaluate_quote_slice fast path. On clean quotes no node is moved, so every served row
    # keeps its record-free answer and the predictions are byte-identical to plain graded;
    # between nodes the record's family root stays within its contract (iv_tolerance 1e-7
    # plus the accurate inversion's own tolerance) of the record-free accurate root. The
    # definition echoes the keys; explicit false adds nothing.
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    runs = {}
    for name, keys in {"plain": None,
                       "records": {"serve_records": True, "serve_record_profile": "no_ladders"},
                       "off": {"serve_records": False}}.items():
        document = {"schema_version": 1, "admission": "graded"}
        if keys is not None:
            document["graded"] = keys
        runs[name] = run_graded(binary, source, tmp_path, name, document)
    plain_bytes = runs["plain"][0].read_bytes()
    assert runs["records"][0].read_bytes() == plain_bytes
    assert runs["off"][0].read_bytes() == plain_bytes
    definitions = {name: run[1]["policy_definition"]["graded"] for name, run in runs.items()}
    assert definitions["records"]["serve_records"] is True
    assert definitions["records"]["serve_record_profile"] == "no_ladders"
    assert "serve_records" not in definitions["plain"]
    assert definitions["off"] == definitions["plain"]
    block = runs["records"][2]["graded_stats"]["serve_records"]
    assert block["profile"] == "no_ladders"
    assert block["records"] >= 1
    assert block["book_versions"] == block["records"]  # one publication per record
    assert block["rows_compared"] >= 1
    assert block.get("rows_differ", 0) == 0
    assert block["midpoints_compared"] >= 1
    assert block.get("midpoints_on_refused_off_ok", 0) == 0
    assert block["midpoints_max_dvol"] <= 2.0e-7
    assert block["first_usable_us"]["n"] == block["records"]
    for name in ("plain", "off"):
        assert "serve_records" not in runs[name][2]["graded_stats"]


def test_native_publication_validation_keeps_one_answer_per_refused_contract(
        binary, tmp_path, calendar_repair_chain):
    # Lane C2's combined path through the CLI: N is repaired onto F's exercise values (the
    # repair validated after its commit), the market-distance gate refuses the rows that
    # land beyond one spread and withholds them, and the final check passes the published
    # surface. Validation changes no prediction here; every refused row is withheld (the
    # library API refuses it too), no row's labels disagree with the typed query, nothing
    # was rolled back and no slice was refused.
    graded = {"market_distance": "gate", "market_distance_spreads": 1.0}
    serve = {"floor_price_only": True, "metadata": True}
    gate, _, _ = run_graded(binary, calendar_repair_chain, tmp_path, "gate", json.loads(
        calendar_repair_policy(tmp_path / "gate-doc.json", graded=graded,
                               serve=serve).read_text()))
    checked, summary, native = run_graded(
        binary, calendar_repair_chain, tmp_path, "checked", json.loads(
            calendar_repair_policy(tmp_path / "checked-doc.json",
                                   graded={**graded, "publication": "validate"},
                                   serve=serve).read_text()))
    assert checked.read_bytes() == gate.read_bytes()
    assert summary["policy_definition"]["graded"]["publication"] == "validate"
    rows = read_quotes(checked)
    refused = [row for row in rows if row["status"] == "market_distance_failure"]
    assert {row["row_id"] for row in refused} >= {"0", "1", "2"}
    for row in refused:
        assert (row["predicted_price"], row["predicted_vol"]) == ("", "")
        assert (row["price_only_reason"], row["vol_convention"]) == ("none", "none")
    counters = native["graded_stats"]["publication"]
    assert counters["withheld_rows"] == len(refused)
    assert counters["checks"] >= 1  # N's calendar repair, validated before its commit
    for key in ("rollbacks", "violations", "label_violations"):
        assert counters[key] == 0, key
    assert "publication_invalid" not in json.dumps(native["graded_stats"]["slices"])


# Lane C2 fix round 1 (review M1): a surface on which the bidirectional calendar rung runs
# and repairs a non-dividend expiry into a non-dividend violation (the dev200b LEMB
# 20742 / 20777 pattern, on synthetic quotes).
# - N (0.15 y): no dividend. Its deep-ITM calls cannot sit below S - dfK.
# - D (0.30 y): a 6 % yield. Its deep-ITM calls are exercised, at S - K.
# So N crosses D on every deep-ITM call, by up to K (1 - df).
# The calls below K 90 are quoted 0.4 wide on each side: too narrow for either
# one-sided rung, wide enough for the joint projection, which lowers N's calls below
# S - dfK.
# Inputs are rounded to 6 decimals, so the snapshot does not depend on libm's last bit.
JOINT_SNAPSHOT_SHA256 = "7a01deb726d5fa56bafa7913c24ee488cb1f3122a7ab9480859f20dded785c67"
# The historical rule: the C0 base binary (atx-vol-quote-bench-c0fix-7a78bb44, e22ff2c4
# code; no non-dividend recheck anywhere) serves this snapshot under JOINT_H0_REPAIR
# serially. The full dev200b cohort is the parity evidence for the same ruling: its h0 +
# repair arm hashes to 2e31e087... on the base binary and on lane C2.
JOINT_BASE_SHA256 = "28b14ac06772f2efcc079efff20321d3f7729c1eb8dd740f994a6e2b565a9c7e"
JOINT_H0_REPAIR = {
    "term_prior_weight": 1.0, "prior_decay_log_time": 1.0, "prior_quote_halflife": 6.0,
    "channel_node_weight": 0.5, "calendar_retry_factor": 4.0, "grade_b_min_mid_strikes": 4,
    "calendar_repair_half_spreads": 0.5, "certificate": "box",
    "certificate_box_half_ticks": 0.0, "market_distance": "repair",
    "market_distance_spreads": 1.0,
}


def write_joint_nondividend_snapshot(path: Path) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["row_id", "chain_id", "strike", "is_call", "spot", "bid", "ask",
                         "years", "years_calendar", "rate", "sdiv", "ddiv_pv", "input_status",
                         "exercise", "underlier", "root", "expiry_days", "snapshot_ns"])
        row = 0
        for chain, (years, q) in enumerate(((0.15, 0.0), (0.30, 0.06))):
            for strike in range(50, 151, 5):
                for call in (1, 0):
                    price = crr_american(100.0, strike, years, 0.25, 0.05, q, call == 1, 400)
                    half = 0.4 if call and strike < 90 else max(0.02, 0.03 * price)
                    writer.writerow([row, 3000 + chain, strike, call, 100,
                                     round(max(price - half, 0.0), 6), round(price + half, 6),
                                     years, years, 0.05, q, 0, "ok", "American", "JJJ", "JJJ",
                                     21000 + round(years * 365), 1])
                    row += 1


def test_native_joint_rung_rechecks_the_nondividend_certificate_only_under_validate(
        binary, tmp_path):
    # PM ruling 2026-10-23 on lane C2 (review-L3q m3; brief deliverable 6), surface wiring.
    # Without validate, fit_quote_surface keeps the historical rule (Skip):
    # - the joint repair applies, and N is served bit-identical to the base binary;
    # - so a keys-ON repair arm never loses a row its base served.
    # Under validate it re-certifies (Recheck):
    # - the rung refuses (NotCertified), and N's rows are refused;
    # - nothing was committed, so nothing is checked or rolled back.
    # The test fails if joint_attempt passes Recheck always: the off arm then loses N.
    # It also fails if joint_attempt passes Skip always: under validate the repair then
    # applies and the after-mutation check rolls it back (outcome "applied",
    # rollbacks 1, checks 1).
    source = tmp_path / "quotes.csv"
    write_joint_nondividend_snapshot(source)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == JOINT_SNAPSHOT_SHA256
    runs = {}
    for name, extra in (("off", {}), ("validate", {"publication": "validate"})):
        policy = write_policy(tmp_path / f"{name}.json", {
            "schema_version": 1, "admission": "graded",
            "graded": {**JOINT_H0_REPAIR, **extra},
            "profiles": [{"name": profile, "base": profile,
                          "channel": {"repair_half_spreads": 0.5}}
                         for profile in PROFILE_NAMES]})
        output, native = tmp_path / f"{name}.csv", tmp_path / f"{name}-native.json"
        result = invoke(binary, source, output, "--policy", str(policy), "--dividends",
                        "funding-pv", "--report", str(native), env={**os.environ, **SERIAL})
        assert result.returncode == 0, (name, result.stderr)
        runs[name] = (output, read_quotes(output), json.loads(native.read_text()))

    def near(name):
        return next(entry for entry in runs[name][2]["graded_stats"]["slices"]
                    if entry["chain_id"] == 3000)

    off_csv, off_rows, off = runs["off"]
    assert hashlib.sha256(off_csv.read_bytes()).hexdigest() == JOINT_BASE_SHA256
    record = near("off")["market_distance"]
    assert record["calendar_joint_repaired"] is True
    assert record["calendar_last_channel_repair"]["outcome"] == "applied"
    served = [row["status"] for row in off_rows if row["chain_id"] == "3000"]
    assert (served.count("ok"), len(served)) == (34, 42)
    assert "publication" not in off["graded_stats"]

    _, on_rows, on = runs["validate"]
    entry = near("validate")
    assert entry["market_distance"]["calendar_last_channel_repair"]["outcome"] == "not_certified"
    assert entry["market_distance"]["calendar_joint_repaired"] is False
    assert entry["grade"] == "calendar_arbitrage"
    refused = [row["status"] for row in on_rows if row["chain_id"] == "3000"]
    assert refused == ["calendar_arbitrage"] * 42
    counters = on["graded_stats"]["publication"]
    assert counters["joint_feasible"] == 1  # the projection settled; the recheck refused it
    for key in ("checks", "rollbacks", "violations", "label_violations"):
        assert counters[key] == 0, key


def test_native_metadata_columns_carry_conventions_and_provenance(binary, tmp_path):
    source = tmp_path / "quotes.csv"
    make_quotes(source)
    policy = write_policy(tmp_path / "policy.json",
                          {"schema_version": 1, "serve": {"metadata": True}})
    rows, report, _ = run_c0(binary, source, tmp_path, "meta", policy)
    assert rows and all(set(C0_COLUMNS) <= set(row) for row in rows)
    served = [row for row in rows if row["status"] == "ok"]
    assert served
    for row in rows:
        assert row["model_kind"] == "andersen_lake"
        assert row["dividend_convention"] == "aggregate_funding_pv_approximation"
        assert (row["spot_version"], row["spot_rule"], row["carry_version"]) == ("0", "own", "0")
        assert row["carry_convention"] == "supplied"
        assert row["raw_spot"] == "" and row["spot_stale"] == "0"
        assert row["quote_ns"] == "" and row["underlying_ns"] == ""  # no observation_ns column
        assert row["price_valid"] == ("1" if row["predicted_price"] else "0")
        assert row["vol_valid"] == ("1" if row["predicted_vol"] else "0")
        expected = {"fitted_curve", "reimplied_repaired"} if row["status"] == "ok" else {"none"}
        assert row["vol_convention"] in expected, row
    conventions = report["output_conventions"]
    for key in ("vol_convention", "model_kind", "dividend_convention", "quote_clock",
                "underlying_clock", "versions", "validity"):
        assert conventions[key]
    assert report["identifiability"]["served_vol_rows"] == len(served)


def write_two_expiry_snapshot(path: Path, drift: float) -> None:
    """Two expiries of one snapshot priced at ONE spot 100 (200-step CRR, sigma 0.2, r 0.05);
    the far chain records a spot `drift` above it and is observed 3 s later (the SPY
    per-chain spot difference behind most served calendar crossings)."""
    snapshot = 1791988200000000000
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["row_id", "chain_id", "snapshot_ns", "observation_ns", "strike",
                         "is_call", "spot", "bid", "ask", "years", "years_calendar", "rate",
                         "sdiv", "ddiv_pv", "input_status", "exercise", "underlier", "root",
                         "expiry_days"])
        row = 0
        for chain, days, recorded, lag in ((17, 30, 100.0, 1), (18, 60, 100.0 + drift, 4)):
            years = days / 365.0
            for strike in [70 + 2.5 * i for i in range(25)]:
                for call in (1, 0):
                    price = crr_american(100.0, strike, years, 0.2, 0.05, 0.0, call == 1, 200)
                    spread = max(0.01, 0.005 * price)
                    writer.writerow([row, chain, snapshot, snapshot + lag * 1_000_000_000,
                                     strike, call, recorded, max(price - spread, 0.0),
                                     price + spread, years, years, 0.05, 0.0, 0.0, "ok",
                                     "American", "XYZ", "XYZ", 21000 + days])
                    row += 1


def test_native_one_spot_removes_a_per_chain_spot_calendar_crossing(binary, tmp_path):
    source = tmp_path / "two-expiry.csv"
    write_two_expiry_snapshot(source, 0.05)
    off_policy = write_policy(tmp_path / "off.json", {"schema_version": 1})
    on_policy = write_policy(tmp_path / "on.json", {
        "schema_version": 1, "snapshot": {"one_spot": "nearest"}, "serve": {"metadata": True}})
    off, off_report, _ = run_c0(binary, source, tmp_path, "off", off_policy)
    on, on_report, _ = run_c0(binary, source, tmp_path, "on", on_policy)
    # Each chain's own spot: the far chain's deep in-the-money puts sit 5 cents under the
    # near chain's exercise value, a calendar crossing that refuses both expiries.
    assert {row["status"] for row in off} == {"calendar_arbitrage"}
    assert off_report["calendar_rejected_chains"] == 2
    assert {row["status"] for row in on} == {"ok"}
    block = on_report["one_spot"]
    assert block["mode"] == "nearest" and "FORWARD-preserving only" in block["semantics"]
    (spot,) = block["snapshots"]
    assert (spot["spot"], spot["rule"], spot["underlying_ns"]) == (
        100.0, "nearest", 1791988201000000000)
    chains = {chain["chain_id"]: chain for chain in block["chains"]}
    assert (chains[17]["raw_spot"], chains[18]["raw_spot"]) == (100.0, 100.05)
    assert chains[18]["yield_shift"] == pytest.approx(math.log(100.0 / 100.05) / (60 / 365),
                                                      rel=1e-12)
    assert block["chain_status_counts"] == {"rebased": 2}
    for row in on:
        assert (row["spot_version"], row["spot_rule"], row["carry_version"]) == (
            "1", "nearest", "1")
        assert row["carry_convention"] == "forward_preserving"
        assert row["underlying_ns"] == "1791988201000000000"
        assert float(row["raw_spot"]) == (100.0 if row["chain_id"] == "17" else 100.05)
        assert row["quote_ns"] == ("1791988201000000000" if row["chain_id"] == "17"
                                   else "1791988204000000000")
    assert on_report["policy_definition"]["snapshot"] == {
        "one_spot": "nearest", "stale_spot_bps": 5.0, "stale_spot": "flag"}
    # The median rule: one spot from every row (half at 100, half at 100.05), no instant.
    median_policy = write_policy(tmp_path / "median.json", {
        "schema_version": 1, "snapshot": {"one_spot": "median"}, "serve": {"metadata": True}})
    rows, report, _ = run_c0(binary, source, tmp_path, "median", median_policy)
    (spot,) = report["one_spot"]["snapshots"]
    assert (spot["spot"], spot["rule"], spot["underlying_ns"]) == (100.025, "median", 0)
    assert {row["underlying_ns"] for row in rows} == {""}
    assert {(row["spot_version"], row["spot_rule"]) for row in rows} == {("1", "median")}


@pytest.mark.parametrize("disposition", ["flag", "refuse"])
def test_native_one_spot_types_a_stale_chain(binary, tmp_path, disposition):
    source = tmp_path / "two-expiry.csv"
    write_two_expiry_snapshot(source, 0.05)
    policy = write_policy(tmp_path / "policy.json", {
        "schema_version": 1, "serve": {"metadata": True},
        "snapshot": {"one_spot": "nearest", "stale_spot_bps": 1, "stale_spot": disposition}})
    rows, report, _ = run_c0(binary, source, tmp_path, disposition, policy)
    near = [row for row in rows if row["chain_id"] == "17"]
    far = [row for row in rows if row["chain_id"] == "18"]
    assert {row["status"] for row in near} == {"ok"}
    assert {row["spot_stale"] for row in near} == {"0"}
    if disposition == "flag":
        assert {row["status"] for row in far} == {"ok"}
        assert {row["spot_stale"] for row in far} == {"1"}
        assert report["one_spot"]["chain_status_counts"] == {"rebased": 1, "rebased_stale": 1}
    else:
        assert {row["status"] for row in far} == {"stale_spot"}
        assert all(row["predicted_price"] == "" for row in far)
        assert report["one_spot"]["chain_status_counts"] == {"rebased": 1, "stale_spot": 1}
