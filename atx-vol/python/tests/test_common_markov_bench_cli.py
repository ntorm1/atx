"""Postimplementation native common-process checks against synthetic raw quotes."""
from __future__ import annotations

import csv
import hashlib
import json
import math
import os
import subprocess
import time

import pytest

from test_quote_bench_cli import binary as binary, invoke, make_quotes


def write_rows(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def read_rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def policy_file(tmp_path, **updates):
    policy = {
        "schema_version": 1, "model": "common-markov",
        "profiles": [{"name": "control", "base": "sparse",
                      "grid": {"grid_nodes": 129, "concentration_scale": .2},
                      "fit": {"initial_sigma": .3, "max_evaluations": 1,
                              "prior_weight": 0, "strike_roughness_weight": 0,
                              "time_roughness_weight": 0}}],
        "regimes": {key + "_profile": "control" for key in ("dense", "ordinary", "sparse")},
    }
    policy.update(updates)
    path = tmp_path / "common-policy.json"
    path.write_text(json.dumps(policy))
    return path


def two_expiry_quotes(source):
    """Closed-form zero-carry calls at .5 and 1 year, both generated at sigma .3."""
    make_quotes(source)
    rows = read_rows(source)
    other = []
    for row in rows:
        strike = float(row["strike"])
        d1 = math.log(100 / strike) / .3 + .15
        price = 50 * math.erfc(-d1 / math.sqrt(2)) - (
            strike / 2 * math.erfc(-(d1 - .3) / math.sqrt(2)))
        other.append({**row, "row_id": int(row["row_id"]) + 1000, "chain_id": 18,
                      "expiry_days": 21200, "years": 1., "years_calendar": 1.,
                      "bid": price - .001, "ask": price + .001})
    write_rows(source, rows + other)
    return rows + other


def knot_policy(tmp_path, knots=(0, 1.5), **fit):
    path = policy_file(tmp_path)
    policy = json.loads(path.read_text())
    policy["profiles"][0]["fit"].update(
        time_shape="variance-time-knots", variance_time_knots=list(knots), **fit)
    path.write_text(json.dumps(policy))
    return path, policy


def common_run(binary, source, output, policy=None, *extra):
    selection = ("--common-policy", str(policy)) if policy else ()
    return invoke(binary, source, output, "--model", "common-markov", *selection, *extra)


def test_common_flat_control_has_independent_price_and_equivalent_iv(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    expected = make_quotes(source)
    result = common_run(binary, source, output, policy_file(tmp_path))
    assert result.returncode == 0, result.stderr
    rows, report = read_rows(output), json.loads(result.stdout)
    assert len(rows) == len(expected)
    assert all(row["status"] == "ok" and row["model"] == "common-markov" for row in rows)
    assert max(abs(float(row["predicted_price"]) - price)
               for row, price in zip(rows, expected)) < .04
    assert max(abs(float(row["predicted_vol"]) - .3) for row in rows) < .003
    assert report["fitted_snapshots"] == 1
    assert report["served_rows"] == report["served_vol_rows"] == len(expected)
    decision, = report["policy_decisions"]
    assert decision["fit"]["termination"] == "evaluation_limit"
    assert decision["fit"]["evaluations"] == 1
    assert decision["fit"]["sigmas"] == [.3]
    assert "not fitted local sigma" in report["predicted_vol_convention"]
    assert "no continuous-American" in report["accuracy_scope"]
    definition = report["policy_definition"]
    assert definition["market"]["max_spot_relative_deviation"] == .005
    assert definition["profiles"][0]["model"]["max_stored_rows"] == 1000000
    first = output.read_bytes()
    assert common_run(binary, source, output, tmp_path / "common-policy.json").returncode == 0
    assert output.read_bytes() == first


def test_common_expiries_share_one_process_and_cumulative_market(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    other = []
    for row in rows:
        strike = float(row["strike"])
        sd = .3
        d1 = math.log(100 / strike) / sd + sd / 2
        d2 = d1 - sd
        price = 50 * math.erfc(-d1 / math.sqrt(2)) - strike / 2 * math.erfc(-d2 / math.sqrt(2))
        other.append({**row, "row_id": int(row["row_id"]) + 1000, "chain_id": 18,
                      "expiry_days": 21200, "years": 1., "years_calendar": 1.,
                      "bid": price - .001, "ask": price + .001})
    write_rows(source, rows + other)
    result = common_run(binary, source, output, policy_file(tmp_path))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["fitted_chains"] == 2
    assert report["fitted_snapshots"] == 1
    assert report["served_rows"] == 52
    decision, = report["policy_decisions"]
    assert decision["fit"]["time_groups"] == 1
    fit = decision["fit"]
    assert fit["time_shape"] == "shared" and fit["variance_time_knots"] == []
    assert fit["time_basis"] is None and "time_interpolation" not in fit
    assert [(row["left_group"], row["right_group"], row["right_weight"], row["variance_active"])
            for row in fit["interval_time_weights"]] == [(0, 0, 0, True)] * 2
    assert [row["variance_time"] for row in fit["interval_time_weights"]] == [.25, .75]
    assert [expiry["calendar_years"]["median"] for expiry in decision["market"]["expiries"]] == [.5, 1.]
    assert {row["chain_id"] for row in read_rows(output)} == {"17", "18"}


def test_common_resolvent_is_opt_in_and_reports_weighted_work(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path)
    before = common_run(binary, source, output, path)
    assert before.returncode == 0, before.stderr
    default_bytes = output.read_bytes()
    default = json.loads(before.stdout)
    policy = json.loads(path.read_text())
    policy["profiles"][0]["model"] = {"diffusion_method": "explicit-euler"}
    path.write_text(json.dumps(policy))
    explicit = common_run(binary, source, output, path)
    assert explicit.returncode == 0, explicit.stderr
    assert output.read_bytes() == default_bytes
    assert json.loads(explicit.stdout)["policy_definition"] == default["policy_definition"]
    policy["profiles"][0]["model"] = {
        "diffusion_method": "variance-resolvent", "max_calendar_step": 1 / 104,
        "max_variance_step": 1 / 104,
    }
    path.write_text(json.dumps(policy))
    candidate = common_run(binary, source, output, path)
    assert candidate.returncode == 0, candidate.stderr
    report = json.loads(candidate.stdout)
    assert report["served_rows"] == len(read_rows(source)) == 26
    model = report["policy_decisions"][0]["fit"]["model"]
    assert model["diffusion_method"] == "variance-resolvent"
    assert model["resolvent_steps"] == model["diffusion_steps"] == 52
    assert model["transition_work"] == 2 * model["stored_rows"] == 2 * 129 * 52
    assert model["quote_prefix_rollback_work"] == 26 * model["transition_work"]
    interval, = model["resolved_intervals"]
    assert interval["diffusion_steps"] == 52
    assert interval["calendar_end"] == interval["variance_end"] == .5
    assert interval["prefix_transition_work"] == model["transition_work"]
    assert interval["min_calendar_step"] == pytest.approx(1 / 104)
    assert interval["max_variance_step"] == pytest.approx(1 / 104)
    assert model["physical_resolvent_factors"] > 0
    assert model["resolvent_factor_blocks"] > 0
    assert model["max_forward_mass_error"] < 1e-10
    config = report["policy_definition"]["profiles"][0]["model"]
    assert config["diffusion_method"] == "variance-resolvent"
    assert config["max_stored_rows"] == 1_000_000
    assert config["max_transition_work"] == 8_000_000
    policy["profiles"][0]["model"]["max_transition_work"] = model["transition_work"] - 1
    path.write_text(json.dumps(policy))
    limited = common_run(binary, source, output, path)
    assert limited.returncode == 0, limited.stderr
    refused = json.loads(limited.stdout)
    assert refused["served_rows"] == 0
    assert len(read_rows(output)) == 26
    assert all(not row["predicted_price"] for row in read_rows(output))
    assert "work" in json.dumps(refused["policy_decisions"][0]).lower()


@pytest.mark.parametrize("model", [
    {"diffusion_method": "unknown"}, {"diffusion_method": 1},
    {"max_transition_work": 0}, {"max_transition_work": 16_000_001},
    {"max_transition_work": 1.5}, {"max_transition_work": True},
    {"diffusion_method": "variance-resolvent", "max_transition_work": 257},
])
def test_common_rejects_invalid_diffusion_controls_before_output(binary, tmp_path, model):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path)
    policy = json.loads(path.read_text())
    policy["profiles"][0]["model"] = model
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode != 0
    assert not output.exists()


def test_common_price_survives_unidentifiable_equivalent_iv(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path, equivalent_iv={"min_time_value": 1000})
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    rows, report = read_rows(output), json.loads(result.stdout)
    assert all(row["status"] == "ok" and row["predicted_price"] for row in rows)
    assert all(row["vol_status"] == "unidentifiable" and not row["predicted_vol"] for row in rows)
    assert report["served_rows"] == 26
    assert report["served_vol_rows"] == 0


def test_common_equivalent_iv_preserves_funding_pv_and_distinct_clocks(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    prepaid, discount, sd = 100 * math.exp(.05) - 2, math.exp(-.01), .3 * math.sqrt(.25)
    expected = []
    for row in rows:
        strike = float(row["strike"])
        d1 = math.log(prepaid / (strike * discount)) / sd + sd / 2
        d2 = d1 - sd
        price = prepaid / 2 * math.erfc(-d1 / math.sqrt(2)) - (
            strike * discount / 2 * math.erfc(-d2 / math.sqrt(2)))
        expected.append(price)
        row.update(years=.25, rate=.02, sdiv=-.1, ddiv_pv=2,
                   bid=price - .001, ask=price + .001)
    write_rows(source, rows)
    path = policy_file(tmp_path)
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    predictions, report = read_rows(output), json.loads(result.stdout)
    assert all(row["status"] == "ok" for row in predictions)
    assert max(abs(float(row["predicted_price"]) - price)
               for row, price in zip(predictions, expected)) < .04
    assert max(abs(float(row["predicted_vol"]) - .3) for row in predictions) < .004
    expiry, = report["policy_decisions"][0]["market"]["expiries"]
    assert expiry["integrated_rate"] == pytest.approx(.01)
    assert expiry["integrated_effective_yield"] == pytest.approx(-math.log(prepaid / 100))
    refused = common_run(binary, source, output, path, "--dividends", "none")
    assert refused.returncode == 0, refused.stderr
    assert all(row["status"] == "market_dividend_convention" for row in read_rows(output))


@pytest.mark.parametrize("change,status", [
    ({"spot": "102"}, "market_spot_dispersion"),
    ({"sdiv": "1"}, "market_carry_dispersion"),
    ({"ask": "-1"}, "market_invalid_row"),
])
def test_common_market_refuses_whole_snapshot_and_keeps_every_row(binary, tmp_path, change, status):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    rows[-1].update(change)
    write_rows(source, rows)
    result = common_run(binary, source, output, policy_file(tmp_path))
    assert result.returncode == 0, result.stderr
    predictions, report = read_rows(output), json.loads(result.stdout)
    assert len(predictions) == len(rows)
    assert all(row["status"] == status and not row["predicted_price"] for row in predictions)
    assert report["served_rows"] == 0
    assert report["policy_decisions"][0]["market"]["status"] == status.removeprefix("market_")
    assert "features" not in report["policy_decisions"][0]


def test_common_duplicate_expiries_are_refused_before_liquidity_routing(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    other = [{**row, "row_id": int(row["row_id"]) + 1000, "chain_id": 18} for row in rows]
    write_rows(source, rows + other)
    result = common_run(binary, source, output)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["ambiguous_expiry_chains"] == 2
    assert all(row["status"] == "ambiguous_expiry" for row in read_rows(output))
    assert "features" not in report["policy_decisions"][0]


def test_common_bidless_contract_is_an_upper_bound_and_override_is_exact(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    rows[0]["bid"] = 0
    write_rows(source, rows)
    path = policy_file(tmp_path, symbol_overrides=[{"symbol": "AAA", "profile": "control"}])
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["policy_decisions"][0]["fit"]["bidless_quotes"] == 1
    assert report["policy_decisions"][0]["policy_source"] == "exact-symbol-override"
    assert read_rows(output)[0]["quote_status"] == "bidless_upper_bound"


@pytest.mark.parametrize("extra", [("--policy", "auto"), ("--curve", "essvi"),
                                   ("--max-weight", "100"), ("--min-quotes", "4")])
def test_common_refuses_legacy_policy_flags(binary, tmp_path, extra):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    result = common_run(binary, source, output, None, *extra)
    assert result.returncode != 0
    assert "incompatible" in result.stderr
    assert not output.exists()


@pytest.mark.parametrize("document", [
    {"schema_version": 1},
    {"schema_version": 1, "model": "common-markov", "relax_arbitrage": True},
    {"schema_version": 1, "model": "common-markov", "market": {"max_quotes": 999999}},
    {"schema_version": 1, "model": "common-markov", "profiles": [
        {"name": "sparse", "base": "sparse", "fit": {"max_evaluations": -1}}]},
])
def test_common_policy_is_closed_and_bounded(binary, tmp_path, document):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    policy = tmp_path / "invalid.json"
    policy.write_text(json.dumps(document))
    result = common_run(binary, source, output, policy)
    assert result.returncode != 0
    assert not output.exists()


def test_common_policy_duplicate_keys_hardlink_alias_and_resource_refusal(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = tmp_path / "policy.json"
    path.write_text('{"schema_version":1,"model":"common-markov","model":"common-markov"}')
    assert common_run(binary, source, output, path).returncode != 0
    alias = tmp_path / "alias.json"
    os.link(source, alias)
    original = source.read_bytes()
    assert common_run(binary, source, output, alias).returncode != 0
    assert source.read_bytes() == original
    result = common_run(binary, source, output, policy_file(tmp_path, market={"max_quotes": 3}))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["resource_refusal_counts"] == {"market_resource_limit": 1}
    assert len(read_rows(output)) == 26


def test_common_progress_binds_inputs_and_preserves_predictions(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path, progress = policy_file(tmp_path), tmp_path / "progress.jsonl"
    baseline = common_run(binary, source, output, path)
    assert baseline.returncode == 0, baseline.stderr
    expected = output.read_bytes()
    result = common_run(binary, source, output, path, "--progress", str(progress))
    assert result.returncode == 0, result.stderr
    assert output.read_bytes() == expected
    report = json.loads(result.stdout)
    records = [json.loads(line) for line in progress.read_bytes().splitlines()]
    assert [r["record_type"] for r in records] == ["start", "snapshot", "complete"]
    assert [r["sequence"] for r in records] == [0, 1, 2]
    header, snapshot, complete = records
    binding = header["run_binding"]
    assert binding["source"]["input_sha256"] == hashlib.sha256(source.read_bytes()).hexdigest()
    assert binding["source"]["policy_sha256"] == hashlib.sha256(path.read_bytes()).hexdigest()
    assert binding["source"]["executable_sha256"] == hashlib.sha256(binary.read_bytes()).hexdigest()
    assert header["configuration"] == report["policy_definition"]
    assert len({r["run_binding_sha256"] for r in records}) == 1
    assert snapshot["objective_evaluations"] == report["objective_evaluations_known"] == 1
    assert snapshot["fit_termination"] == "evaluation_limit"
    assert snapshot["row_status_counts"] == {"ok": 26}
    assert not snapshot["stage_attempted"]["seed"]
    assert all(value for name, value in snapshot["stage_attempted"].items() if name != "seed")
    assert all(math.isfinite(x) and x >= 0 for x in snapshot["stage_elapsed_microseconds"].values())
    assert report["stage_elapsed_microseconds"] == snapshot["stage_elapsed_microseconds"]
    assert complete["state"] == "numerical_processing_complete"
    assert complete["completed_rows"] == complete["input_rows"] == 26
    assert all(key not in snapshot for key in ("bid", "ask", "strike", "srPrc", "srVol"))
    before = progress.read_bytes()
    refused = common_run(binary, source, output, path, "--progress", str(progress))
    assert refused.returncode != 0
    assert progress.read_bytes() == before


@pytest.mark.parametrize("failure,stage", [
    ("ambiguous", None), ("market", "market"), ("policy", "policy"),
    ("grid", "grid"), ("fit_limit", "fit"), ("fit", "fit"),
])
def test_common_stage_timing_and_progress_include_refusals(binary, tmp_path, failure, stage):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    path = policy_file(tmp_path)
    policy = json.loads(path.read_text())
    if failure == "ambiguous":
        rows += [{**row, "row_id": int(row["row_id"]) + 1000, "chain_id": 18}
                 for row in rows]
    elif failure == "market":
        rows[-1]["spot"] = "102"
    elif failure == "policy":
        for row in rows:
            row["years"] = 0
    elif failure == "grid":
        # Finite accepted spot; configured upper grid stock overflows at runtime.
        for row in rows:
            row["spot"] = 1e308
    elif failure == "fit_limit":
        policy["profiles"][0]["fit"]["max_quotes"] = 1
    else:
        policy["profiles"][0]["model"] = {"max_transitions": 1}
    path.write_text(json.dumps(policy))
    write_rows(source, rows)
    progress = tmp_path / "progress.jsonl"
    result = common_run(binary, source, output, path, "--progress", str(progress))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    decision, = report["policy_decisions"]
    assert decision["status"] != "ok"
    names = ["market", "policy", "grid", "fit", "equivalent_iv"]
    visited = names[:names.index(stage) + 1] if stage else []
    assert decision["stage_attempted"] == {**{name: name in visited for name in names}, "seed": False}
    assert all(value >= 0 for value in decision["stage_elapsed_microseconds"].values())
    assert decision["objective_evaluations"] == (None if failure == "fit" else 0)
    assert report["objective_evaluation_counts_unavailable"] == (failure == "fit")
    records = [json.loads(line) for line in progress.read_bytes().splitlines()]
    assert records[1]["status"] == decision["status"]
    assert records[-1]["completed_rows"] == len(rows)


@pytest.mark.parametrize("target", ["source", "output", "report", "policy", "hardlink"])
def test_common_progress_rejects_aliases_before_writing(binary, tmp_path, target):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path, report = policy_file(tmp_path), tmp_path / "report.json"
    alias = tmp_path / "hardlink"
    os.link(source, alias)
    progress = {"source": source, "output": output, "report": report,
                "policy": path, "hardlink": alias}[target]
    original = source.read_bytes()
    result = common_run(binary, source, output, path, "--report", str(report),
                        "--progress", str(progress))
    assert result.returncode != 0
    assert source.read_bytes() == original
    assert not output.exists() and not report.exists()


def test_common_progress_retains_completed_prefix_after_real_process_termination(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    original = read_rows(source)
    # Many small snapshots ensure a completed prefix exists while work remains;
    # the test kills a real child, without a production fault-injection hook.
    rows = [{**row, "underlier": f"S{i:04}", "root": f"S{i:04}", "chain_id": i + 1,
             "row_id": i * len(original) + j} for i in range(512)
            for j, row in enumerate(original)]
    write_rows(source, rows)
    path, progress = policy_file(tmp_path), tmp_path / "partial.jsonl"
    command = [str(binary), "--input", str(source), "--output", str(output),
               "--model", "common-markov", "--common-policy", str(path),
               "--progress", str(progress)]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 20
        prefix = []
        while process.poll() is None and time.monotonic() < deadline:
            if progress.exists():
                prefix = progress.read_bytes().split(b"\n")[:-1]
                if len(prefix) >= 2:
                    break
            time.sleep(.005)
        assert process.poll() is None and len(prefix) >= 2
        process.kill()
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        process.stderr.close()
    records = [json.loads(line) for line in progress.read_bytes().split(b"\n")[:-1]]
    assert records[0]["state"] == "incomplete"
    assert 1 <= len(records) - 1 < records[0]["snapshots_expected"]
    assert all(record["record_type"] == "snapshot" for record in records[1:])
    assert [record["sequence"] for record in records] == list(range(len(records)))
    assert not output.exists()


def test_native_progress_and_wrapper_receipts_agree_end_to_end(binary, tmp_path):
    from test_quote_benchmark import prepare, qb
    dataset, _ = prepare(tmp_path)
    output, progress = tmp_path / "prices.csv", tmp_path / "progress.jsonl"
    args = qb.parser().parse_args([
        "run", "--dataset", str(dataset), "--binary", str(binary), "--output", str(output),
        "--model", "common-markov", "--progress", str(progress),
    ])
    receipt = qb.run_native(args)
    assert receipt["progress"]["sha256"] == qb.sha256(progress)
    assert receipt["start_receipt_sha256"] == qb.sha256(qb.Path(receipt["start_receipt"]))
    assert receipt["native_report"]["rows"] == 6
    assert receipt["progress"]["completed_snapshots"] == receipt["native_report"]["snapshots"]


def test_progress_reports_empty_eligible_cohort_without_invented_snapshots(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    for row in rows:
        row["input_status"] = "unsupported_deliverable"
    write_rows(source, rows)
    progress = tmp_path / "progress.jsonl"
    result = common_run(binary, source, output, None, "--progress", str(progress))
    assert result.returncode == 0, result.stderr
    records = [json.loads(line) for line in progress.read_bytes().splitlines()]
    assert [record["record_type"] for record in records] == ["start", "complete"]
    assert records[0]["rows_excluded_by_input_status"] == len(rows)
    assert records[0]["snapshots_expected"] == 0
    assert records[1]["completed_snapshots"] == records[1]["completed_rows"] == 0
    assert records[1]["row_status_counts"] == {"unsupported_deliverable": len(rows)}
    assert len(read_rows(output)) == len(rows)


def test_configured_initialization_is_default_and_preserves_output(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path)
    original = common_run(binary, source, output, path)
    assert original.returncode == 0, original.stderr
    expected = output.read_bytes()
    policy = json.loads(path.read_text())
    policy["initialization"] = {"mode": "configured"}
    path.write_text(json.dumps(policy))
    explicit = common_run(binary, source, output, path)
    assert explicit.returncode == 0, explicit.stderr
    assert output.read_bytes() == expected
    report = json.loads(explicit.stdout)
    decision, = report["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == "configured" and not init["attempted"]
    assert init["effective_initial_sigmas"] == init["effective_prior_sigmas"] == [.3]
    assert not decision["stage_attempted"]["seed"]
    assert decision["stage_elapsed_microseconds"]["seed"] == 0
    assert init["eligible_quotes"] is None
    assert report["initialization_states"] == {"configured": 1}
    assert report["policy_definition"]["initialization"]["seed"]["max_inversions"] == 384


@pytest.mark.parametrize("prior", [None, .4])
def test_complete_seeds_preserve_configured_prior_and_full_quotes(binary, tmp_path, prior):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    rows[0]["bid"] = 0
    write_rows(source, rows)
    path = policy_file(tmp_path, initialization={"mode": "quote-seed"})
    policy = json.loads(path.read_text())
    fit = policy["profiles"][0]["fit"]
    fit.update(initial_sigma=.18, initial_sigmas=[.2], prior_weight=.7)
    if prior is not None:
        fit["prior_sigmas"] = [prior]
    path.write_text(json.dumps(policy))
    progress = tmp_path / "seed-progress.jsonl"
    result = common_run(binary, source, output, path, "--progress", str(progress))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    decision, = report["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == "applied" and init["attempted"] and init["applied"]
    assert init["effective_initial_sigmas"] == pytest.approx([.3], abs=1e-5)
    old_prior = .2 if prior is None else prior
    assert init["effective_prior_sigmas"] == [old_prior]
    assert init["configured_prior_source"] == ("configured_initial" if prior is None else "prior_sigmas")
    assert init["configured_initial_source"] == "initial_sigmas"
    sigma, = decision["fit"]["sigmas"]
    assert decision["fit"]["regularization_loss"] == pytest.approx(.7 * math.log(sigma / old_prior)**2)
    assert decision["fit"]["quote_count"] == report["served_rows"] == len(rows)
    assert init["used_quotes"] == 5
    assert init["refused_quotes"] + init["used_quotes"] == len(rows)
    assert init["quote_status_counts"]["bidless"] == 1
    assert sum(init["quote_status_counts"].values()) == len(rows)
    assert init["inversion_calls"] == 15 and init["reserved_root_iteration_slots"] == 15 * 32
    assert decision["stage_attempted"]["seed"]
    assert decision["stage_elapsed_microseconds"]["seed"] > 0
    assert report["per_profile_counts"]["control"]["initialization_states"] == {"applied": 1}
    header, snapshot, complete = [json.loads(line) for line in progress.read_bytes().splitlines()]
    assert header["configuration"]["initialization"] == report["policy_definition"]["initialization"]
    assert snapshot["initialization"]["effective_prior_sigmas"] == [old_prior]
    assert snapshot["initialization"]["quote_status_counts"] == init["quote_status_counts"]
    assert "expiries" not in snapshot["initialization"]
    assert complete["initialization_states"] == {"applied": 1}


@pytest.mark.parametrize("controls,state", [
    ({"max_abs_log_forward_moneyness": 0}, "fallback_incomplete"),
    ({"max_input_quotes": 1}, "fallback_error"),
])
def test_seed_fallback_reproduces_configured_output(binary, tmp_path, controls, state):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path)
    baseline = common_run(binary, source, output, path)
    assert baseline.returncode == 0, baseline.stderr
    expected = output.read_bytes()
    policy = json.loads(path.read_text())
    policy["initialization"] = {"mode": "quote-seed", "seed": controls}
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    assert output.read_bytes() == expected
    decision, = json.loads(result.stdout)["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == state and init["attempted"] and not init["applied"]
    assert init["effective_initial_sigmas"] == init["effective_prior_sigmas"] == [.3]
    assert decision["fit"]["quote_count"] == 26
    if state == "fallback_error":
        assert init["eligible_quotes"] is init["inversion_calls"] is None
        assert init["error_code"]
    else:
        assert init["eligible_quotes"] == init["used_quotes"] == init["inversion_calls"] == 0
        assert init["quote_status_counts"] == {"far_moneyness": 26}


def test_budget_incomplete_term_seeds_never_apply_partial_vector(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    # Distinct expiry with usable quote intervals; the inversion budget permits
    # only the first endpoint's triplet, leaving the second forward group unready.
    rows += [{**row, "row_id": int(row["row_id"]) + 1000, "chain_id": 18,
              "expiry_days": 21200, "years": 1., "years_calendar": 1.} for row in rows]
    write_rows(source, rows)
    path = policy_file(tmp_path)
    policy = json.loads(path.read_text())
    policy["profiles"][0]["fit"].update(time_shape="per-variance-interval", initial_sigma=.2)
    path.write_text(json.dumps(policy))
    baseline = common_run(binary, source, output, path)
    assert baseline.returncode == 0, baseline.stderr
    expected = output.read_bytes()
    policy["initialization"] = {"mode": "quote-seed", "seed": {"max_inversions": 3}}
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    assert output.read_bytes() == expected
    decision, = json.loads(result.stdout)["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == "fallback_incomplete"
    assert init["parameter_status_counts"] == {"ready": 1, "missing_endpoint": 1}
    assert init["effective_initial_sigmas"] == init["effective_prior_sigmas"] == [.2, .2]
    assert init["used_quotes"] == 1 and init["inversion_calls"] == 3
    assert init["quote_status_counts"]["inversion_budget"] > 0
    assert decision["fit"]["quote_count"] == len(rows)


@pytest.mark.parametrize("refusal", ["iv_floor", "sigma_bounds"])
def test_seed_numerical_refusal_keeps_configured_fit(binary, tmp_path, refusal):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path)
    policy = json.loads(path.read_text())
    if refusal == "iv_floor":
        row = read_rows(source)[0]
        # Exact zero-carry ATM call at the inverter's .005 floor.
        price = 100 * math.erf(.005 * math.sqrt(.5) / (2 * math.sqrt(2)))
        row.update(strike=100, bid=price, ask=price)
        write_rows(source, [row])
        policy["profiles"][0]["fit"].update(initial_sigma=.02, min_sigma=.001)
    else:
        policy["profiles"][0]["fit"].update(initial_sigma=.2, max_sigma=.25)
    path.write_text(json.dumps(policy))
    baseline = common_run(binary, source, output, path)
    assert baseline.returncode == 0, baseline.stderr
    expected = output.read_bytes()
    policy["initialization"] = {"mode": "quote-seed"}
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    assert output.read_bytes() == expected
    decision, = json.loads(result.stdout)["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == "fallback_incomplete" and not init["applied"]
    assert init["quote_status_counts"][refusal] > 0 and init["used_quotes"] == 0
    assert decision["fit"]["quote_count"] == len(read_rows(source))


def test_shared_seeds_broadcast_knots_without_moving_explicit_prior(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path, initialization={"mode": "quote-seed"})
    policy = json.loads(path.read_text())
    policy["profiles"][0]["fit"].update(
        log_state_knots=[-.2, 0, .2], initial_sigma=.2, prior_sigmas=[.15, .2, .25], prior_weight=.8)
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    decision, = json.loads(result.stdout)["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == "applied"
    assert init["effective_initial_sigmas"] == pytest.approx([.3, .3, .3], abs=1e-5)
    assert init["effective_prior_sigmas"] == [.15, .2, .25]
    assert init["layout"]["log_state_knots"] == [-.2, 0, .2]
    sigma = decision["fit"]["sigmas"]
    expected_penalty = .8 * sum(math.log(x / y)**2 for x, y in zip(sigma, [.15, .2, .25])) / 3
    assert decision["fit"]["regularization_loss"] == pytest.approx(expected_penalty)


def test_seed_inversion_error_samples_are_bounded_and_all_refusals_counted(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    for row in rows:
        row.update(bid=200, ask=201)  # Above a zero-carry call's price bound.
    rows += [{**row, "row_id": int(row["row_id"]) + 1000, "chain_id": 18,
              "expiry_days": 21200, "years": 1., "years_calendar": 1.} for row in rows]
    write_rows(source, rows)
    path = policy_file(tmp_path, initialization={
        "mode": "quote-seed", "seed": {"candidates_per_expiry": 32}})
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    decision, = json.loads(result.stdout)["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == "fallback_incomplete"
    failures = init["quote_status_counts"]["inversion_failure"]
    assert failures > 32
    assert len(init["inversion_error_samples"]) == init["inversion_error_sample_limit"] == 32
    assert init["inversion_errors_not_sampled"] == failures - 32
    assert all(0 <= item["quote_index"] < len(rows) and 0 < len(item["error"]) <= 1024
               for item in init["inversion_error_samples"])
    assert sum(init["quote_status_counts"].values()) == decision["fit"]["quote_count"] == len(rows)


@pytest.mark.parametrize("initialization", [
    {"mode": "unknown"}, {"seed": {}}, {"mode": "configured", "seed": {}},
    {"mode": "quote-seed", "seed": {"unknown": 1}},
    {"mode": "quote-seed", "seed": {"max_inversions": 3073}},
    {"mode": "quote-seed", "seed": {"max_input_quotes": 0}},
    {"mode": "quote-seed", "seed": {"max_root_iteration_slots": 95}},
    {"mode": "quote-seed", "seed": {"max_iv_iterations": 32.5}},
    {"mode": "quote-seed", "seed": {"inversion_options": {"n_quad_price": 0}}},
    {"mode": "quote-seed", "seed": {"inversion_options": {"n_quadrature": 9}}},
    {"mode": "quote-seed", "seed": {"max_relative_spread": float("nan")}},
])
def test_seed_policy_is_closed_and_bounded_before_execution(binary, tmp_path, initialization):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path, initialization=initialization)
    progress = tmp_path / "progress.jsonl"
    result = common_run(binary, source, output, path, "--progress", str(progress))
    assert result.returncode != 0
    assert not output.exists() and not progress.exists()


@pytest.mark.parametrize("failure", ["market", "fit_limit", "fit"])
def test_seed_stage_and_fitter_failures_do_not_trigger_retry(binary, tmp_path, failure):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path, initialization={"mode": "quote-seed"})
    policy = json.loads(path.read_text())
    if failure == "market":
        policy["market"] = {"max_quotes": 1}
    elif failure == "fit_limit":
        policy["profiles"][0]["fit"]["max_quotes"] = 1
    else:
        policy["profiles"][0]["model"] = {"max_transitions": 1}
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    decision, = report["policy_decisions"]
    assert report["served_rows"] == 0 and len(read_rows(output)) == 26
    init = decision["initialization"]
    if failure == "fit":
        assert decision["status"] == "fit_failure"
        assert init["state"] == "applied" and decision["stage_attempted"]["seed"]
        assert decision["objective_evaluations"] is None
    else:
        assert init["state"] == "not_attempted" and init["reason"] == decision["status"]
        assert not decision["stage_attempted"]["seed"]
        assert decision["stage_elapsed_microseconds"]["seed"] == 0


def test_common_variance_time_knots_expand_policy_and_report_owned_basis(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    rows = two_expiry_quotes(source)
    path, _ = knot_policy(tmp_path, max_evaluations=40)
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    profile, = report["policy_definition"]["profiles"]
    assert profile["fit"]["time_shape"] == "variance-time-knots"
    assert profile["fit"]["variance_time_knots"] == [0, 1.5]
    decision, = report["policy_decisions"]
    fit = decision["fit"]
    assert fit["time_shape"] == "variance-time-knots" and fit["variance_time_knots"] == [0, 1.5]
    assert fit["time_groups"] == fit["parameter_count"] == len(fit["sigmas"]) == 2
    weights = fit["interval_time_weights"]
    assert [(row["left_group"], row["right_group"], row["variance_active"])
            for row in weights] == [(0, 1, True)] * 2
    a, b = (row["right_weight"] for row in weights)
    assert (a, b) == pytest.approx((.25 / 1.5, .75 / 1.5), rel=1e-15)
    assert "midpoint" in fit["time_interpolation"]
    # Independent 2x2 Gram-Schmidt reference for the represented interval map.
    first, second = (1 - a, 1 - b), (a, b)
    r11 = math.hypot(*first)
    r12 = (first[0] * second[0] + first[1] * second[1]) / r11
    r22 = math.hypot(second[0] - r12 * first[0] / r11, second[1] - r12 * first[1] / r11)
    inverse = max(1 / r11 + abs(r12) / (r11 * r22), 1 / r22)
    basis = fit["time_basis"]
    assert basis["quoted_variance_intervals"] == 2
    assert basis["column_support_counts"] == [2, 2]
    assert basis["column_l2_norms"] == pytest.approx([r11, math.hypot(*second)], rel=1e-14)
    assert basis["common_normalization"] == pytest.approx(math.sqrt(2), rel=1e-15)
    assert basis["qr_reciprocal_condition_inf"] == pytest.approx(
        1 / (max(r11 + abs(r12), r22) * inverse), rel=1e-12)
    assert basis["min_reciprocal_condition"] == 1e-12
    predictions = read_rows(output)
    assert len(predictions) == len(rows) == report["served_rows"]
    assert all(row["status"] == "ok" for row in predictions)
    assert max(abs(float(row["predicted_vol"]) - .3) for row in predictions) < .004


def test_common_equal_knot_controls_reproduce_shared_prediction_bytes(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    two_expiry_quotes(source)
    shared = common_run(binary, source, output, policy_file(tmp_path))
    assert shared.returncode == 0, shared.stderr
    shared_rows, shared_report = read_rows(output), json.loads(shared.stdout)
    # Legacy expanded definitions gain no key, so their configuration hash is unchanged.
    assert "variance_time_knots" not in shared_report["policy_definition"]["profiles"][0]["fit"]
    path, _ = knot_policy(tmp_path)
    knots = common_run(binary, source, output, path)
    assert knots.returncode == 0, knots.stderr
    knot_rows, knot_report = read_rows(output), json.loads(knots.stdout)
    columns = ("row_id", "status", "predicted_price", "predicted_vol", "vol_status")
    assert [tuple(row[c] for c in columns) for row in knot_rows] == [
        tuple(row[c] for c in columns) for row in shared_rows]
    a, = shared_report["policy_decisions"]
    b, = knot_report["policy_decisions"]
    for key in ("objective", "interval_loss", "midpoint_loss", "max_interval_error"):
        assert a["fit"][key] == b["fit"][key], key
    assert a["fit"]["model"]["transition_work"] == b["fit"]["model"]["transition_work"]
    assert b["fit"]["sigmas"] == [.3, .3]


def test_common_variance_time_knots_refuse_quote_seeds_and_keep_configured_fit(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    two_expiry_quotes(source)
    path, policy = knot_policy(tmp_path)
    baseline = common_run(binary, source, output, path)
    assert baseline.returncode == 0, baseline.stderr
    expected = output.read_bytes()
    policy["initialization"] = {"mode": "quote-seed"}
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    assert output.read_bytes() == expected
    decision, = json.loads(result.stdout)["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == "fallback_error" and not init["applied"]
    assert init["error_code"] == "Unavailable"
    assert init["inversion_calls"] is None and init["eligible_quotes"] is None
    assert init["layout"]["time_shape"] == "variance-time-knots"
    assert init["layout"]["variance_time_knots"] == [0, 1.5]
    assert init["effective_initial_sigmas"] == init["effective_prior_sigmas"] == [.3, .3]
    assert decision["status"] == "ok" and decision["fit"]["quote_count"] == 52


@pytest.mark.parametrize("fit", [
    {"variance_time_knots": [0, 1]},
    {"time_shape": "variance-time-knots"},
    {"time_shape": "variance-time-knot", "variance_time_knots": [0, 1]},
    {"time_shape": "variance-time-knots", "variance_time_knots": [.5]},
    {"time_shape": "variance-time-knots", "variance_time_knots": [1, .5]},
    {"time_shape": "variance-time-knots", "variance_time_knots": [.5, .5]},
    {"time_shape": "variance-time-knots", "variance_time_knots": [-.1, .5]},
    {"time_shape": "variance-time-knots", "variance_time_knots": [0, "1"]},
    {"time_shape": "variance-time-knots", "variance_time_knots": 1},
    {"time_shape": "variance-time-knots", "variance_time_knots": list(range(9))},
    {"time_shape": "variance-time-knots", "variance_time_knots": [0, 1], "initial_sigmas": [.3]},
])
def test_common_variance_time_knot_policy_is_closed_before_execution(binary, tmp_path, fit):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path)
    policy = json.loads(path.read_text())
    policy["profiles"][0]["fit"].update(fit)
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode != 0
    assert not output.exists()


def test_common_unsupported_time_knot_is_a_typed_fit_failure(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    # One quoted positive-variance interval cannot identify two time controls.
    path, _ = knot_policy(tmp_path, knots=(0, 1))
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    decision, = report["policy_decisions"]
    assert decision["status"] == "fit_failure" and decision["error_code"] == "OutOfRange"
    assert "time basis" in decision["error"]
    assert report["served_rows"] == 0
    assert all(row["status"] == "fit_failure" for row in read_rows(output))


SEARCH_KEYS = ("search", "evaluations_per_parameter", "max_scaled_evaluations",
               "objective_tolerance", "gradient_tolerance", "jacobian_log_step",
               "initial_damping", "max_damping")


def test_default_search_keeps_definition_and_reports_effective_budget(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    result = common_run(binary, source, output, policy_file(tmp_path))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    fit_definition = report["policy_definition"]["profiles"][0]["fit"]
    # Legacy expanded definitions (and their configuration hashes) gain no key.
    assert not any(key in fit_definition for key in SEARCH_KEYS)
    decision, = report["policy_decisions"]
    fit = decision["fit"]
    assert fit["search"] == "coordinate-pattern"
    assert fit["evaluation_budget"] == fit["evaluations"] == 1
    assert fit["jacobian_evaluations"] == fit["reused_quote_prices"] == 0
    assert fit["priced_quotes"] == 26 and fit["evaluation_rollback_work"] > 0
    assert report["fit_termination_counts"] == {"control:evaluation_limit": 1}


def test_levenberg_marquardt_scaled_budget_is_explicit_and_reported(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    expected = make_quotes(source)
    path = policy_file(tmp_path)
    policy = json.loads(path.read_text())
    policy["profiles"][0]["fit"].update(
        log_state_knots=[-.2, 0, .2], initial_sigma=.2, max_evaluations=64,
        search="levenberg-marquardt", evaluations_per_parameter=32, max_scaled_evaluations=256,
        max_sweeps=40, objective_tolerance=1e-9)
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    fit_definition = report["policy_definition"]["profiles"][0]["fit"]
    assert fit_definition["search"] == "levenberg-marquardt"
    assert fit_definition["evaluations_per_parameter"] == 32
    assert fit_definition["max_scaled_evaluations"] == 256
    assert all(key in fit_definition for key in SEARCH_KEYS)
    decision, = report["policy_decisions"]
    fit = decision["fit"]
    assert fit["search"] == "levenberg-marquardt"
    assert fit["evaluation_budget"] == 96  # 32 x 3 knots, above the 64 base, below the 256 cap
    assert fit["evaluations"] <= 96
    assert fit["termination"] in {"objective_tolerance", "step_tolerance", "gradient_tolerance"}
    assert fit["jacobian_evaluations"] > 0 and fit["jacobian_evaluations"] % 3 == 0
    # A shared shape reuses nothing: every evaluation prices every quote.
    assert fit["reused_quote_prices"] == 0
    assert fit["priced_quotes"] == fit["evaluations"] * 26
    # Explicit clocks vary with sigma, so only the retained model's work is a floor.
    assert fit["evaluation_rollback_work"] >= fit["model"]["quote_prefix_rollback_work"]
    assert fit["sigmas"] == pytest.approx([.3, .3, .3], abs=.02)
    assert report["fit_termination_counts"] == {f"control:{fit['termination']}": 1}
    rows = read_rows(output)
    assert max(abs(float(row["predicted_price"]) - price) for row, price in zip(rows, expected)) < .04


@pytest.mark.parametrize("fit", [
    {"search": "newton"},
    {"evaluations_per_parameter": 8},
    {"max_scaled_evaluations": 64},
    {"evaluations_per_parameter": 8, "max_scaled_evaluations": 8193},
    {"evaluations_per_parameter": 8.5, "max_scaled_evaluations": 64},
    {"jacobian_log_step": 0},
    {"initial_damping": 1, "max_damping": .5},
    {"objective_tolerance": float("nan")},
])
def test_search_policy_is_closed_and_bounded_before_execution(binary, tmp_path, fit):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path)
    policy = json.loads(path.read_text())
    policy["profiles"][0]["fit"].update(fit)
    path.write_text(json.dumps(policy))
    result = common_run(binary, source, output, path)
    assert result.returncode != 0
    assert not output.exists()


def black_scholes_call(strike, sigma, years=.5):
    width = sigma * math.sqrt(years)
    d1 = math.log(100 / strike) / width + width / 2
    return 50 * math.erfc(-d1 / math.sqrt(2)) - strike / 2 * math.erfc(-(d1 - width) / math.sqrt(2))


def test_interval_fallback_seeds_wide_and_bidless_market_within_budget(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    for index, row in enumerate(rows):
        strike = float(row["strike"])
        # Asks at sigma .45; OTM bids at sigma .2 (wide), every third OTM and all ITM bidless,
        # so the bid-mid-ask primary admits nothing.
        bidless = index % 3 == 0 or strike < 100
        row["bid"] = 0 if bidless else black_scholes_call(strike, .2)
        row["ask"] = black_scholes_call(strike, .45)
    write_rows(source, rows)
    path = policy_file(tmp_path, initialization={"mode": "quote-seed", "seed": {
        "estimator": "bid-mid-ask-then-sigma-interval", "max_inversions": 12}})
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    definition = report["policy_definition"]["initialization"]
    assert definition["seed"]["estimator"] == "bid-mid-ask-then-sigma-interval"
    assert "geometric midpoint" in definition["interval_estimator"]
    decision, = report["policy_decisions"]
    init = decision["initialization"]
    assert init["state"] == "applied_interval_fallback" and init["applied"]
    # The bid-mid-ask primary refuses every quote before inverting anything.
    assert init["primary_inversion_calls"] == 0 and init["used_quotes"] == 0
    assert set(init["quote_status_counts"]) <= {"bidless", "wide_spread", "far_moneyness"}
    fallback = init["interval_fallback"]
    assert fallback["state"] == "applied" and fallback["remaining_max_inversions"] == 12
    assert fallback["estimator"] == "sigma-interval"
    assert init["inversion_calls"] == fallback["inversion_calls"] <= 12
    interval = fallback["sigma_interval"]
    assert interval["lower"] == pytest.approx(.2, abs=1e-5)
    assert interval["upper"] == pytest.approx(.45, abs=1e-5)
    assert interval["geometric_midpoint"] == pytest.approx(math.sqrt(.2 * .45), abs=1e-5)
    assert not interval["crossed"]
    assert len(fallback["interval_quotes"]) == fallback["used_quotes"] > 0
    assert any(q["lower_sigma"] is None for q in fallback["interval_quotes"])  # Bidless ask only.
    assert init["effective_initial_sigmas"] == pytest.approx([math.sqrt(.2 * .45)], abs=1e-5)
    assert init["effective_prior_sigmas"] == [.3]  # Configured prior retained.
    assert report["initialization_states"] == {"applied_interval_fallback": 1}
    assert decision["fit"]["quote_count"] == len(rows)


@pytest.mark.parametrize("estimator", ["sigma", "interval", 1])
def test_seed_estimator_is_closed(binary, tmp_path, estimator):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path, initialization={"mode": "quote-seed",
                                                 "seed": {"estimator": estimator}})
    result = common_run(binary, source, output, path)
    assert result.returncode != 0
    assert not output.exists()
