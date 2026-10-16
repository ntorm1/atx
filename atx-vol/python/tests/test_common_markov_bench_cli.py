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
    assert decision["fit"]["interval_groups"] == [0, 0]
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
    assert all(snapshot["stage_attempted"].values())
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
    assert decision["stage_attempted"] == {name: name in visited for name in names}
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
