#!/usr/bin/env python3
"""Run a bounded, fixed development experiment queue with durable provenance.

Policies are frozen before execution; scoring never selects a later policy.
This offline harness delegates prediction and evaluation to quote_benchmark.
It deliberately has no automatic promotion or partial-run resume semantics.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys

SPEC = importlib.util.spec_from_file_location(
    "atx_quote_benchmark_campaign_adapter", Path(__file__).with_name("quote_benchmark.py"))
qb = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qb)
CAMPAIGN_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
MAX_TRIALS = 64
MAX_DOCUMENT_BYTES = 1024 * 1024
RESERVED_IDS = {"policies", "con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)),
                *(f"lpt{i}" for i in range(1, 10))}


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _document(path: Path) -> tuple[bytes, dict]:
    with path.open("rb") as stream:
        data = stream.read(MAX_DOCUMENT_BYTES + 1)
    if len(data) > MAX_DOCUMENT_BYTES:
        raise ValueError("campaign document exceeds 1 MiB")
    def reject_constant(value):
        raise ValueError(f"nonfinite JSON constant: {value}")
    result = json.loads(data, object_pairs_hook=_unique_object, parse_constant=reject_constant)
    if not isinstance(result, dict):
        raise ValueError("campaign document must be a JSON object")
    return data, result


def _keys(document: dict, required: set[str], optional: set[str] = frozenset()) -> None:
    if not isinstance(document, dict) or not required <= document.keys():
        raise ValueError(f"missing campaign fields: {sorted(required)}")
    unknown = document.keys() - required - optional
    if unknown:
        raise ValueError(f"unknown campaign fields: {sorted(unknown)}")


def _path(value, base: Path) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError("campaign paths must be nonempty strings")
    return (base / value).resolve()


def _write_bytes(path: Path, data: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


class Journal:
    """Exclusive append-only process-interruption evidence, not a resume database."""
    def __init__(self, path: Path):
        self.stream = path.open("xb")
        self.sequence = 0

    def append(self, kind: str, **fields) -> None:
        record = {"schema_version": 1, "sequence": self.sequence, "record_type": kind,
                  "utc": dt.datetime.now(dt.timezone.utc).isoformat(), **fields}
        payload = (json.dumps(record, sort_keys=True, allow_nan=False) + "\n").encode()
        if len(payload) > MAX_DOCUMENT_BYTES:
            raise ValueError("campaign progress record exceeds 1 MiB")
        self.stream.write(payload)
        self.stream.flush()
        os.fsync(self.stream.fileno())
        self.sequence += 1

    def close(self) -> None:
        self.stream.close()


def _baseline(path: Path, manifest: dict) -> tuple[dict, Path]:
    # Run receipts include per-snapshot diagnostics and may exceed the bounded
    # configuration size. The existing evaluator owns their numerical validation.
    receipt = json.loads(path.read_bytes())
    for name, field in (("quotes.parquet", "quote_parquet_sha256"),
                        ("quotes.csv", "quote_csv_sha256")):
        if receipt.get(field) != manifest["files"][name]["sha256"]:
            raise ValueError("campaign baseline belongs to a different quote cohort")
    predictions = Path(receipt["predictions"]).resolve()
    if qb.sha256(predictions) != receipt.get("prediction_sha256"):
        raise ValueError("campaign baseline prediction hash mismatch")
    return receipt, predictions


def run_campaign(plan_path: Path, output: Path) -> dict:
    plan_path, output = plan_path.resolve(), output.resolve()
    plan_bytes, plan = _document(plan_path)
    _keys(plan, {"schema_version", "dataset", "binary", "baseline_receipt", "trials"},
          {"dividends", "tick_size"})
    if type(plan["schema_version"]) is not int or plan["schema_version"] != 1:
        raise ValueError("unsupported campaign schema")
    trials = plan["trials"]
    if not isinstance(trials, list) or not 1 <= len(trials) <= MAX_TRIALS:
        raise ValueError("campaign requires 1..64 fixed trials")
    dividends = plan.get("dividends", "funding-pv")
    if not isinstance(dividends, str) or dividends not in qb.DIVIDEND_CONVENTIONS:
        raise ValueError("unsupported campaign dividend convention")
    tick = plan.get("tick_size", 0.01)
    if type(tick) not in (float, int) or not 0 < tick <= 1e6:
        raise ValueError("campaign tick size must be positive and finite")
    dataset = _path(plan["dataset"], plan_path.parent)
    binary = _path(plan["binary"], plan_path.parent)
    baseline = _path(plan["baseline_receipt"], plan_path.parent)
    manifest = qb.verify_dataset(dataset)
    if "quotes.csv" not in manifest["files"]:
        raise ValueError("campaign requires prepared native CSV")
    # Refuse a mixed or validation cohort before any scorer can open labels.
    if (manifest.get("rows_by_split") != {"development": manifest["rows_selected"]}
            or manifest["rows_selected"] <= 0):
        raise ValueError("campaign requires a nonempty development-only cohort")
    split_valid = qb.pl.scan_parquet(dataset / "quotes.parquet").select(
        (qb.pl.col("split") == "development").fill_null(False).all()).collect().item()
    if not split_valid:
        raise ValueError("campaign quote split disagrees with development-only manifest")
    _, baseline_predictions = _baseline(baseline, manifest)
    frozen_inputs = {str(path): qb.sha256(path) for path in
                     (binary, dataset / "manifest.json", dataset / "quotes.csv",
                      dataset / "quotes.parquet", baseline, baseline_predictions)}
    frozen_trials = []
    seen = set()
    for trial in trials:
        _keys(trial, {"id", "common_policy"})
        name = trial["id"]
        if (not isinstance(name, str) or re.fullmatch(r"[a-z][a-z0-9_-]{0,63}", name) is None
                or name in seen or name in RESERVED_IDS):
            raise ValueError("campaign trial IDs must be unique lowercase portable identifiers")
        seen.add(name)
        policy = trial["common_policy"]
        if policy == "auto":
            frozen_trials.append((name, None, None))
        else:
            path = _path(policy, plan_path.parent)
            data, _ = _document(path)
            frozen_trials.append((name, path, data))
    # Every configuration and source input is inspected before claiming output.
    # Fresh-directory semantics prevent mixing interrupted experiments or artifacts.
    output.mkdir(parents=True, exist_ok=False)
    policy_dir = output / "policies"
    policy_dir.mkdir()
    selections = []
    for name, source, data in frozen_trials:
        target = policy_dir / f"{name}.json"
        if data is not None:
            _write_bytes(target, data)
            frozen_inputs[str(target)] = hashlib.sha256(data).hexdigest()
        selections.append({"id": name, "policy": str(target) if data is not None else "auto",
                           "source_policy": str(source) if source else None,
                           "policy_sha256": hashlib.sha256(data).hexdigest() if data else None})
    start = {"schema_version": 1, "state": "started_incomplete", "split": "development",
             "plan": plan, "plan_sha256": hashlib.sha256(plan_bytes).hexdigest(),
             "campaign_sha256": CAMPAIGN_SHA256, "runner_sha256": qb.ADAPTER_SHA256,
             "frozen_inputs": frozen_inputs, "trials": selections,
             "selection_policy": "fixed before scoring; no automatic policy selection or promotion"}
    qb.write_json(output / "start.json", start, overwrite=False, durable=True)
    start_hash = qb.sha256(output / "start.json")
    def verify_frozen_inputs():
        for path, digest in frozen_inputs.items():
            if qb.sha256(Path(path)) != digest:
                raise ValueError(f"campaign frozen input changed: {path}")
        if qb.sha256(output / "start.json") != start_hash:
            raise ValueError("campaign start receipt changed")
    journal = Journal(output / "progress.jsonl")
    outcomes = []
    try:
        journal.append("start", start_sha256=start_hash,
                       expected_trials=len(selections))
        for trial in selections:
            # A changed common source invalidates the remaining experiment queue.
            verify_frozen_inputs()
            trial_dir = output / trial["id"]
            trial_dir.mkdir()
            journal.append("trial_start", trial_id=trial["id"])
            try:
                args = qb.parser().parse_args([
                    "run", "--dataset", str(dataset), "--binary", str(binary),
                    "--output", str(trial_dir / "predictions.csv"), "--model", "common-markov",
                    "--common-policy", trial["policy"], "--dividends", dividends,
                    "--progress", str(trial_dir / "native-progress.jsonl"),
                    "--", "--report", str(trial_dir / "native-report.json")])
                qb.run_native(args)
                verify_frozen_inputs()
                receipt = trial_dir / "predictions.csv.run.json"
                # Numerical execution is finished before these functions read labels.
                score = qb.score(argparse.Namespace(dataset=dataset, run_receipt=receipt,
                    output=trial_dir / "score.json", split="development", tick_size=tick))
                comparison = qb.compare(argparse.Namespace(dataset=dataset, run_a=baseline,
                    run_b=receipt, output=trial_dir / "comparison.json",
                    split="development", tick_size=tick))
                outcome = {"trial_id": trial["id"], "state": "complete",
                           "metrics_own_population": score["metrics"],
                           "paired_with_fixed_baseline": comparison["paired"],
                           "artifacts": {name: qb.sha256(trial_dir / name) for name in
                               ("predictions.csv.run.json", "score.json", "comparison.json")}}
            except (OSError, ValueError, RuntimeError, KeyError, qb.pl.exceptions.PolarsError) as exc:
                outcome = {"trial_id": trial["id"], "state": "failed",
                           "error_type": type(exc).__name__, "error": str(exc)[:4096]}
            qb.write_json(trial_dir / "outcome.json", outcome, overwrite=False, durable=True)
            outcomes.append(outcome)
            journal.append("trial_complete", trial_id=trial["id"], state=outcome["state"],
                           outcome_sha256=qb.sha256(trial_dir / "outcome.json"))
        # Recheck shared provenance even if the final native/scorer action failed.
        verify_frozen_inputs()
        complete = {"schema_version": 1, "state": "completed_with_failures" if any(
                       item["state"] == "failed" for item in outcomes) else "complete",
                    "start_sha256": start_hash, "trials": outcomes,
                    "scope": "development experiments; independent price/vol masks and full coverage; "
                             "no holdout evaluation, automatic promotion or speed qualification"}
        journal.append("complete", state=complete["state"], attempted_trials=len(outcomes))
        complete["progress_sha256"] = qb.sha256(output / "progress.jsonl")
        qb.write_json(output / "complete.json", complete, overwrite=False, durable=True)
        return complete
    finally:
        journal.close()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        result = run_campaign(args.plan, args.output)
        print(json.dumps({"campaign": str(args.output.resolve()), "state": result["state"]}))
        return 0 if result["state"] == "complete" else 1
    except (OSError, ValueError, RuntimeError, KeyError, qb.pl.exceptions.PolarsError) as exc:
        print(f"quote-benchmark-campaign: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
