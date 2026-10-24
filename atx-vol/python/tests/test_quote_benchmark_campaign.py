"""Postimplementation fixed-queue, durability and native integration checks."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import pytest

from test_quote_bench_cli import binary as binary
from test_quote_benchmark import prediction_receipt, source_table

SPEC = importlib.util.spec_from_file_location(
    "quote_campaign", Path(__file__).resolve().parents[2] / "tools/quote_benchmark_campaign.py")
campaign = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(campaign)
qb = campaign.qb


def fixture_plan(tmp_path, executable=None):
    source = tmp_path / "source.parquet"
    qb.pq.write_table(source_table(), source)
    dataset = tmp_path / "dataset"
    qb.prepare(qb.parser().parse_args([
        "prepare", "--source", str(source), "--output", str(dataset), "--csv",
        "--validation-fraction", "1e-12", "--split", "development"]))
    _, receipt = prediction_receipt(dataset)
    if executable is None:
        executable = tmp_path / "never-executed.exe"
        executable.write_bytes(b"placeholder")
    policy = tmp_path / "policy.json"
    policy.write_text(json.dumps({
        "schema_version": 1, "model": "common-markov",
        "profiles": [{"name": "control", "base": "sparse",
                      "fit": {"max_evaluations": 1}}],
        "regimes": {name + "_profile": "control" for name in ("dense", "ordinary", "sparse")},
    }))
    plan = {"schema_version": 1, "dataset": str(dataset), "binary": str(executable),
            "baseline_receipt": str(receipt),
            "trials": [{"id": "first", "common_policy": "policy.json"},
                       {"id": "second", "common_policy": "policy.json"}]}
    path = tmp_path / "plan.json"
    path.write_text(json.dumps(plan))
    return path, plan, tmp_path / "campaign"


def records(output):
    return [json.loads(line) for line in (output / "progress.jsonl").read_bytes().splitlines()]


@pytest.mark.parametrize("mutation", ["unknown", "schema_bool", "duplicate_id", "path_id",
                                     "uppercase_id", "reserved_id", "internal_id", "empty_trials", "too_many",
                                     "bad_tick"])
def test_invalid_plan_has_no_output_or_native_side_effect(tmp_path, monkeypatch, mutation):
    path, plan, output = fixture_plan(tmp_path)
    if mutation == "unknown":
        plan["split"] = "validation"
    elif mutation == "schema_bool":
        plan["schema_version"] = True
    elif mutation == "duplicate_id":
        plan["trials"][1]["id"] = "first"
    elif mutation == "path_id":
        plan["trials"][0]["id"] = "../outside"
    elif mutation == "uppercase_id":
        plan["trials"][0]["id"] = "FIRST"
    elif mutation == "reserved_id":
        plan["trials"][0]["id"] = "con"
    elif mutation == "internal_id":
        plan["trials"][1]["id"] = "policies"
    elif mutation == "empty_trials":
        plan["trials"] = []
    elif mutation == "too_many":
        plan["trials"] = [{"id": f"t{i}", "common_policy": "auto"} for i in range(65)]
    else:
        plan["tick_size"] = True
    path.write_text(json.dumps(plan))
    monkeypatch.setattr(qb, "run_native", lambda _: pytest.fail("invalid plan reached native"))
    with pytest.raises(ValueError):
        campaign.run_campaign(path, output)
    assert not output.exists()


@pytest.mark.parametrize("payload", ['{"schema_version":1,"schema_version":1}',
                                     '{"value":NaN}', '"not an object"'])
def test_plan_json_is_unambiguous(tmp_path, payload):
    path = tmp_path / "plan.json"
    path.write_text(payload)
    with pytest.raises(ValueError):
        campaign.run_campaign(path, tmp_path / "out")


def test_validation_cohort_refused_before_scoring(tmp_path, monkeypatch):
    path, plan, output = fixture_plan(tmp_path)
    manifest_path = Path(plan["dataset"]) / "manifest.json"
    manifest = json.loads(manifest_path.read_bytes())
    manifest["rows_by_split"] = {"development": 3, "validation": 3}
    manifest_path.write_text(json.dumps(manifest))
    monkeypatch.setattr(qb, "score", lambda _: pytest.fail("validation labels opened"))
    with pytest.raises(ValueError, match="development-only"):
        campaign.run_campaign(path, output)
    assert not output.exists()


def test_raw_split_must_agree_with_development_manifest(tmp_path):
    path, plan, output = fixture_plan(tmp_path)
    dataset = Path(plan["dataset"])
    quotes = qb.pl.read_parquet(dataset / "quotes.parquet").with_columns(
        qb.pl.lit("validation").alias("split"))
    qb.pq.write_table(quotes.to_arrow().cast(qb.QUOTE_SCHEMA), dataset / "quotes.parquet")
    manifest = json.loads((dataset / "manifest.json").read_bytes())
    manifest["files"]["quotes.parquet"]["sha256"] = qb.sha256(dataset / "quotes.parquet")
    (dataset / "manifest.json").write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match="quote split"):
        campaign.run_campaign(path, output)
    assert not output.exists()


def test_all_policies_frozen_before_first_trial_and_failed_trials_retained(tmp_path, monkeypatch):
    path, plan, output = fixture_plan(tmp_path)
    policy = tmp_path / "policy.json"
    original = policy.read_bytes()
    observed = []
    def native(args):
        observed.append(Path(args.common_policy).read_bytes())
        policy.write_text('{"changed":true}')
        raise RuntimeError("controlled native failure")
    monkeypatch.setattr(qb, "run_native", native)
    monkeypatch.setattr(qb, "score", lambda _: pytest.fail("failed run was scored"))
    result = campaign.run_campaign(path, output)
    assert observed == [original, original]
    assert result["state"] == "completed_with_failures"
    assert [trial["state"] for trial in result["trials"]] == ["failed", "failed"]
    journal = records(output)
    assert [r["sequence"] for r in journal] == list(range(6))
    assert journal[-1]["attempted_trials"] == 2
    assert result["progress_sha256"] == qb.sha256(output / "progress.jsonl")
    before = (output / "complete.json").read_bytes()
    with pytest.raises(FileExistsError):
        campaign.run_campaign(path, output)
    assert (output / "complete.json").read_bytes() == before


def test_interruption_retains_prior_trial_and_no_false_completion(tmp_path, monkeypatch):
    path, _, output = fixture_plan(tmp_path)
    calls = 0
    def native(_):
        nonlocal calls
        calls += 1
        if calls == 1:
            raise RuntimeError("first trial failed explicitly")
        raise KeyboardInterrupt
    monkeypatch.setattr(qb, "run_native", native)
    with pytest.raises(KeyboardInterrupt):
        campaign.run_campaign(path, output)
    assert (output / "first/outcome.json").is_file()
    assert not (output / "second/outcome.json").exists()
    assert not (output / "complete.json").exists()
    assert [r["record_type"] for r in records(output)] == [
        "start", "trial_start", "trial_complete", "trial_start"]


@pytest.mark.parametrize("target", ["binary", "policy_snapshot", "start_receipt"])
def test_changed_frozen_input_aborts_remaining_queue(tmp_path, monkeypatch, target):
    path, plan, output = fixture_plan(tmp_path)
    calls = []
    def native(args):
        calls.append(args)
        destination = {"binary": Path(plan["binary"]),
                       "policy_snapshot": Path(args.common_policy),
                       "start_receipt": output / "start.json"}[target]
        destination.write_text("changed")
        raise RuntimeError("failed with altered shared input")
    monkeypatch.setattr(qb, "run_native", native)
    with pytest.raises(ValueError, match="changed"):
        campaign.run_campaign(path, output)
    assert len(calls) == 1
    assert not (output / "complete.json").exists()
    assert not (output / "second").exists()


def test_native_campaign_scores_complete_runs_and_preserves_failed_trial(binary, tmp_path):
    path, plan, output = fixture_plan(tmp_path, binary)
    invalid = tmp_path / "invalid.json"
    invalid.write_text('{"schema_version":1,"model":"common-markov","unknown":true}')
    plan["trials"].insert(1, {"id": "refused", "common_policy": str(invalid)})
    path.write_text(json.dumps(plan))
    result = campaign.run_campaign(path, output)
    assert result["state"] == "completed_with_failures"
    assert [t["state"] for t in result["trials"]] == ["complete", "failed", "complete"]
    a, _, b = result["trials"]
    assert a["metrics_own_population"] == b["metrics_own_population"]
    assert a["paired_with_fixed_baseline"] == b["paired_with_fixed_baseline"]
    assert a["paired_with_fixed_baseline"]["eligible_rows"] == 6
    assert (output / "first/predictions.csv").read_bytes() == (
        output / "second/predictions.csv").read_bytes()
    assert (output / "refused/predictions.csv.start.json").exists()
    assert not (output / "refused/score.json").exists()
    assert [r["sequence"] for r in records(output)] == list(range(8))


def test_real_evaluator_failure_retains_outcomes_and_continues_fixed_queue(binary, tmp_path):
    path, plan, output = fixture_plan(tmp_path, binary)
    receipt_path = Path(plan["baseline_receipt"])
    receipt = json.loads(receipt_path.read_bytes())
    predictions = Path(receipt["predictions"])
    qb.pl.read_csv(predictions, schema_overrides={"chain_id": qb.pl.UInt64}).with_columns(
        qb.pl.lit("unparseable").alias("predicted_price")).write_csv(predictions)
    receipt["prediction_sha256"] = qb.sha256(predictions)
    receipt_path.write_text(json.dumps(receipt))
    result = campaign.run_campaign(path, output)
    assert result["state"] == "completed_with_failures"
    assert [r["state"] for r in result["trials"]] == ["failed", "failed"]
    assert all(r["error_type"] == "ComputeError" for r in result["trials"])
    for name in ("first", "second"):
        assert (output / name / "predictions.csv.run.json").is_file()
        assert (output / name / "score.json").is_file()
        assert (output / name / "outcome.json").is_file()
        assert not (output / name / "comparison.json").exists()
    assert records(output)[-1]["attempted_trials"] == 2
