"""Post-implementation verification of cohort isolation and score integrity."""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
from pathlib import Path

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq
import pytest


TOOLS = Path(__file__).resolve().parents[2] / "tools"
SPEC = importlib.util.spec_from_file_location("quote_benchmark", TOOLS / "quote_benchmark.py")
qb = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qb)


def source_table() -> pa.Table:
    n = 6
    table = pa.table({
        "okey_tk": ["AAA"] * 3 + ["BBB"] * 3,
        "okey_dt": [dt.date(2027, 1, 15)] * n,
        "okey_xx": [90., 100., 110.] * 2,
        "okey_cp": ["Call", "Put", "Call"] * 2,
        "date": [dt.datetime(2026, 10, 14, 14, 30, tzinfo=dt.timezone.utc)] * n,
        "timestamp": [dt.datetime(2026, 10, 14, 14, 30, 1, tzinfo=dt.timezone.utc)] * n,
        "undSecKey_tk": ["AAA"] * 3 + ["BBB"] * 3,
        "uBid": [99.99] * n, "uAsk": [100.01] * n,
        "bidPrc": [1.] * n, "askPrc": [1.2] * n,
        "bidSz": [10] * n, "askSz": [10] * n,
        "years": [.3] * n, "yearsC": [.25] * n,
        "rate": [.04] * n, "sdiv": [.01] * n,
        "ddiv": [0.] * n, "ddivPv": [0.] * n,
        "exType": ["American"] * n,
        "srPrc": [1.1] * n, "srVol": [.3] * n,
        "bidIV": [998.] * n, "askIV": [999.] * n, "synSpot": [9999.] * n,
        "atmVol": [999.] * n, "error": [999] * n,
    })
    return table


def prepare(tmp_path: Path, table: pa.Table | None = None, *, name="cohort", batch_size=2):
    source = tmp_path / f"{name}.parquet"
    pq.write_table(table if table is not None else source_table(), source, row_group_size=3)
    output = tmp_path / name
    args = qb.parser().parse_args(["prepare", "--source", str(source), "--output", str(output),
                                  "--split", "all", "--csv", "--batch-size", str(batch_size)])
    manifest = qb.prepare(args)
    return output, manifest


def prediction_receipt(dataset: Path, *, rows=None):
    quotes = pl.read_parquet(dataset / "quotes.parquet")
    if rows is not None:
        quotes = quotes.head(rows)
    pred = quotes.select("row_id", "chain_id").with_columns(
        pl.lit(1.11).alias("predicted_price"), pl.lit(.31).alias("predicted_vol"),
        pl.lit("ok").alias("status"))
    path = dataset / "predictions.csv"
    pred.write_csv(path)
    receipt = dataset / "predictions.csv.run.json"
    qb.write_json(receipt, {"predictions": str(path), "prediction_sha256": qb.sha256(path),
                           "quote_parquet_sha256": qb.sha256(dataset / "quotes.parquet"),
                           "quote_csv_sha256": qb.sha256(dataset / "quotes.csv")})
    return path, receipt


def score(dataset: Path, receipt: Path):
    args = qb.parser().parse_args(["score", "--dataset", str(dataset), "--run-receipt", str(receipt),
                                  "--output", str(dataset / "score.json"), "--split", "all"])
    return qb.score(args)


def test_poisoning_reference_and_vendor_model_fields_cannot_change_inputs(tmp_path):
    first, manifest = prepare(tmp_path, name="original")
    table = source_table()
    for name in ("srPrc", "srVol", "bidIV", "askIV", "synSpot", "atmVol"):
        table = table.set_column(table.schema.get_field_index(name), name,
                                 pa.array([float("nan")] * table.num_rows))
    second, changed = prepare(tmp_path, table, name="poisoned")
    assert manifest["files"]["quotes.parquet"] == changed["files"]["quotes.parquet"]
    assert manifest["files"]["quotes.csv"] == changed["files"]["quotes.csv"]
    assert manifest["files"]["labels.parquet"] != changed["files"]["labels.parquet"]
    assert set(pq.read_schema(first / "quotes.parquet").names).isdisjoint(qb.LABEL_COLUMNS)
    assert set(pq.read_schema(second / "labels.parquet").names) == set(qb.LABEL_SCHEMA.names)


def test_row_identity_and_chain_groups_survive_batch_boundaries(tmp_path):
    one, _ = prepare(tmp_path, name="small", batch_size=2)
    two, _ = prepare(tmp_path, name="large", batch_size=20)
    a, b = pl.read_parquet(one / "quotes.parquet"), pl.read_parquet(two / "quotes.parquet")
    assert a.equals(b)
    assert a["row_id"].to_list() == list(range(6))
    assert a["chain_id"].n_unique() == 2
    assert a.group_by("underlier").agg(pl.col("split").n_unique())["split"].to_list() == [1, 1]
    assert a["spot"].to_list() == [100.] * 6


def test_rejections_are_retained_and_accounted_without_labels(tmp_path):
    table = source_table()
    table = table.set_column(table.schema.get_field_index("exType"), "exType",
                             pa.array(["European"] + ["American"] * 5))
    table = table.set_column(table.schema.get_field_index("uBid"), "uBid",
                             pa.array([99.99, -99.] + [99.99] * 4))
    table = table.set_column(table.schema.get_field_index("yearsC"), "yearsC",
                             pa.array([.25, .25, 0., .25, .25, .25]))
    dataset, manifest = prepare(tmp_path, table)
    q = pl.read_parquet(dataset / "quotes.parquet")
    assert manifest["rows_selected"] == 6
    assert q["input_status"].to_list() == ["unsupported_exercise", "invalid_underlier_quote",
                                          "invalid_time", "ok", "ok", "ok"]
    assert pl.read_parquet(dataset / "labels.parquet").height == 6


def test_normalization_refuses_a_label_bearing_frame():
    batch = source_table().to_batches()[0]
    with pytest.raises(ValueError, match="allowlist"):
        qb.normalize_quotes(batch, 0, bucket_minutes=30, validation_fraction=.2, split_seed="seed")


def test_distinct_snapshots_in_one_selection_bucket_never_merge(tmp_path):
    table = source_table()
    times = [dt.datetime(2026, 10, 14, 14, minute, tzinfo=dt.timezone.utc)
             for minute in (30, 35, 55, 30, 35, 55)]
    table = table.set_column(table.schema.get_field_index("date"), "date", pa.array(times))
    dataset, _ = prepare(tmp_path, table)
    q = pl.read_parquet(dataset / "quotes.parquet")
    assert q["snapshot_ns"].n_unique() == 3
    assert q["chain_id"].n_unique() == 6


def test_missing_predictions_reduce_coverage_instead_of_silently_shrinking_cohort(tmp_path):
    dataset, _ = prepare(tmp_path)
    _, receipt = prediction_receipt(dataset, rows=4)
    metrics = score(dataset, receipt)["metrics"]
    assert metrics["rows"] == metrics["eligible_rows"] == 6
    assert metrics["price_scored_rows"] == 4
    assert metrics["price_coverage"] == pytest.approx(4 / 6)
    assert metrics["price_mae"] == pytest.approx(.01)
    assert metrics["vol_bp_mae"] == pytest.approx(100.)
    assert metrics["price_in_spreads_mae"] == pytest.approx(.05)
    assert metrics["midpoint_price_mae"] == pytest.approx(0.)


def test_missing_reference_labels_do_not_reduce_prediction_coverage(tmp_path):
    table = source_table()
    table = table.set_column(table.schema.get_field_index("srPrc"), "srPrc",
                             pa.array([-99.] * 6))
    dataset, _ = prepare(tmp_path, table)
    _, receipt = prediction_receipt(dataset)
    metrics = score(dataset, receipt)["metrics"]
    assert metrics["price_coverage"] == 1.
    assert metrics["price_scoring_coverage"] == 0.
    assert metrics["price_prediction_rows"] == 6
    assert metrics["price_label_rows"] == metrics["price_scored_rows"] == 0
    assert metrics["price_mae"] is None


@pytest.mark.parametrize("corruption", ["duplicates", "unknown", "wrong_chain", "null_chain", "hash"])
def test_score_refuses_identity_or_provenance_corruption(tmp_path, corruption):
    dataset, _ = prepare(tmp_path)
    path, receipt = prediction_receipt(dataset)
    df = pl.read_csv(path, schema_overrides={"row_id": pl.UInt64, "chain_id": pl.UInt64})
    if corruption == "duplicates":
        df = pl.concat([df, df.head(1)])
    elif corruption == "unknown":
        df = df.with_columns(pl.lit(100, dtype=pl.UInt64).alias("row_id"))
    elif corruption == "wrong_chain":
        df = df.with_columns(pl.lit(0, dtype=pl.UInt64).alias("chain_id"))
    elif corruption == "null_chain":
        df = df.with_columns(pl.lit(None, dtype=pl.UInt64).alias("chain_id"))
    else:
        df = df.with_columns(pl.lit(999.).alias("predicted_price"))
    df.write_csv(path)
    if corruption != "hash":
        data = json.loads(receipt.read_text())
        data["prediction_sha256"] = qb.sha256(path)
        qb.write_json(receipt, data)
    with pytest.raises(ValueError):
        score(dataset, receipt)


def test_explicit_selection_is_label_independent_and_keeps_original_row_ids(tmp_path):
    source = tmp_path / "source.parquet"
    pq.write_table(source_table(), source)
    args = qb.parser().parse_args(["prepare", "--source", str(source), "--output", str(tmp_path / "chosen"),
                                  "--symbols", "BBB", "--split", "all", "--buckets", "1030"])
    qb.prepare(args)
    q = pl.read_parquet(tmp_path / "chosen" / "quotes.parquet")
    assert q["row_id"].to_list() == [3, 4, 5]
    assert q["underlier"].to_list() == ["BBB"] * 3


def test_american_exercise_spelling_is_canonical_for_the_native_boundary(tmp_path):
    table = source_table()
    table = table.set_column(table.schema.get_field_index("exType"), "exType",
                             pa.array(["american", "AMERICAN", "AmErIcAn", "American",
                                       "European", None]))
    dataset, _ = prepare(tmp_path, table)
    quotes = pl.read_parquet(dataset / "quotes.parquet")
    assert quotes["exercise"].to_list() == ["American"] * 4 + ["European", None]
    assert quotes["input_status"].to_list() == ["ok"] * 4 + ["unsupported_exercise"] * 2


@pytest.mark.parametrize("extra", [["--curve", "svi"], ["--curve=svi"],
                                  ["--policy", "auto"], ["--policy=auto"]])
def test_native_extras_cannot_override_the_recorded_curve(tmp_path, monkeypatch, extra):
    dataset, _ = prepare(tmp_path)
    binary = tmp_path / "unused-native.exe"
    binary.write_bytes(b"not executed")
    def forbidden(*args, **kwargs):
        pytest.fail("invalid provenance arguments reached subprocess execution")
    monkeypatch.setattr(qb.subprocess, "run", forbidden)
    args = qb.parser().parse_args([
        "run", "--dataset", str(dataset), "--binary", str(binary),
        "--output", str(tmp_path / "new-predictions.csv"), "--curve", "essvi", "--", *extra,
    ])
    with pytest.raises(ValueError, match="provenance-bound"):
        qb.run_native(args)


@pytest.mark.parametrize("target", ["manifest.json", "quotes.parquet", "labels.parquet",
                                   "predictions.csv", "predictions.csv.run.json"])
def test_score_never_overwrites_validated_inputs_or_receipts(tmp_path, target):
    dataset, _ = prepare(tmp_path)
    _, receipt = prediction_receipt(dataset)
    destination = dataset / target
    before = destination.read_bytes()
    args = qb.parser().parse_args([
        "score", "--dataset", str(dataset), "--run-receipt", str(receipt),
        "--output", str(destination), "--split", "all",
    ])
    with pytest.raises(ValueError, match="output already exists"):
        qb.score(args)
    assert destination.read_bytes() == before


def test_exclusive_json_write_preserves_a_concurrent_output(tmp_path):
    output = tmp_path / "already-created.json"
    output.write_text("existing evidence")
    with pytest.raises(FileExistsError):
        qb.write_json(output, {"replacement": True}, overwrite=False)
    assert output.read_text() == "existing evidence"


@pytest.mark.parametrize("target", ["labels.parquet", "manifest.json", "predictions", "receipt"])
def test_native_report_cannot_replace_cohort_or_pending_run_artifacts(tmp_path, monkeypatch, target):
    dataset, _ = prepare(tmp_path)
    binary = tmp_path / "unused-native.exe"
    binary.write_bytes(b"not executed")
    output = tmp_path / "new-predictions.csv"
    report = (output if target == "predictions" else Path(str(output) + ".run.json")
              if target == "receipt" else dataset / target)
    before = report.read_bytes() if report.exists() else None
    def forbidden(*args, **kwargs):
        pytest.fail("unsafe report output reached native execution")
    monkeypatch.setattr(qb.subprocess, "run", forbidden)
    args = qb.parser().parse_args([
        "run", "--dataset", str(dataset), "--binary", str(binary),
        "--output", str(output), "--", "--report", str(report),
    ])
    with pytest.raises(ValueError, match="timing report must use a new path"):
        qb.run_native(args)
    if before is not None:
        assert report.read_bytes() == before
    else:
        assert not report.exists()


def test_native_new_timing_report_remains_usable(tmp_path, monkeypatch):
    dataset, _ = prepare(tmp_path)
    binary = tmp_path / "unused-native.exe"
    binary.write_bytes(b"not executed")
    output = tmp_path / "new-predictions.csv"
    report = tmp_path / "new-timing.json"
    def execute(command, **kwargs):
        assert command[command.index("--report") + 1] == str(report)
        output.write_text("predictions")
        report.write_text("{}")
        return qb.subprocess.CompletedProcess(command, 0, stdout='{"policy_mode":"auto"}')
    monkeypatch.setattr(qb.subprocess, "run", execute)
    args = qb.parser().parse_args([
        "run", "--dataset", str(dataset), "--binary", str(binary),
        "--output", str(output), "--", "--report", str(report),
    ])
    receipt = qb.run_native(args)
    assert receipt["curve"] is None
    assert receipt["policy"] == "auto"
    assert receipt["native_report"] == {"policy_mode": "auto"}
    assert report.read_text() == "{}"
    assert Path(str(output) + ".run.json").is_file()


@pytest.mark.parametrize("mutate", [False, True])
def test_custom_policy_content_is_bound_to_run_receipt(tmp_path, monkeypatch, mutate):
    dataset, _ = prepare(tmp_path)
    binary = tmp_path / "unused-native.exe"
    binary.write_bytes(b"not executed")
    output = tmp_path / "new-predictions.csv"
    policy = tmp_path / "policy.json"
    document = {"schema_version": 1, "symbol_overrides": [
        {"symbol": "AAA", "profile": "sparse"}]}
    policy.write_text(json.dumps(document))
    original_hash = qb.sha256(policy)

    def execute(command, **kwargs):
        assert command[command.index("--policy") + 1] == str(policy.resolve())
        assert "--curve" not in command
        output.write_text("predictions")
        if mutate:
            policy.write_text('{"schema_version":1}')
        return qb.subprocess.CompletedProcess(command, 0, stdout='{"policy_mode":"file"}')

    monkeypatch.setattr(qb.subprocess, "run", execute)
    args = qb.parser().parse_args([
        "run", "--dataset", str(dataset), "--binary", str(binary),
        "--output", str(output), "--policy", str(policy),
    ])
    if mutate:
        with pytest.raises(ValueError, match="policy configuration changed"):
            qb.run_native(args)
        assert not Path(str(output) + ".run.json").exists()
    else:
        receipt = qb.run_native(args)
        assert receipt["policy_sha256"] == original_hash
        assert receipt["policy_document"] == document
        assert receipt["policy"] == str(policy.resolve())


def test_policy_and_fixed_curve_are_mutually_exclusive():
    with pytest.raises(SystemExit):
        qb.parser().parse_args([
            "run", "--dataset", "dataset", "--binary", "runner", "--output", "predictions",
            "--policy", "auto", "--curve", "essvi",
        ])


def candidate_receipt(dataset: Path, name: str, ids: list[int], prices: list[float],
                      vols: list[float]) -> Path:
    q = pl.read_parquet(dataset / "quotes.parquet").filter(pl.col("row_id").is_in(ids))
    pred = q.select("row_id", "chain_id").with_columns(
        pl.Series("predicted_price", prices), pl.Series("predicted_vol", vols),
        pl.lit("ok").alias("status"))
    path = dataset / f"{name}.csv"
    pred.write_csv(path)
    receipt = dataset / f"{name}.run.json"
    qb.write_json(receipt, {"predictions": str(path), "prediction_sha256": qb.sha256(path),
                           "quote_parquet_sha256": qb.sha256(dataset / "quotes.parquet"),
                           "quote_csv_sha256": qb.sha256(dataset / "quotes.csv")})
    return receipt


def compare(dataset: Path, a: Path, b: Path, *, name="compare") -> dict:
    args = qb.parser().parse_args([
        "compare", "--dataset", str(dataset), "--run-a", str(a), "--run-b", str(b),
        "--output", str(dataset / f"{name}.json"), "--split", "all",
    ])
    return qb.compare(args)


def test_paired_comparison_cannot_claim_improvement_by_dropping_hard_contracts(tmp_path):
    dataset, _ = prepare(tmp_path)
    a = candidate_receipt(dataset, "a", [0, 1, 2], [1.1, 3.1, 101.1], [.3, .4, .5])
    b = candidate_receipt(dataset, "b", [0, 1], [1.1, 4.1], [.31, float("nan")])
    result = compare(dataset, a, b)
    own_a = result["arm_a"]["metrics_own_population"]
    own_b = result["arm_b"]["metrics_own_population"]
    assert own_b["price_mae"] < own_a["price_mae"]  # Misleading unpaired headline.
    assert own_a["price_coverage"] == .5
    assert own_b["price_coverage"] == pytest.approx(1 / 3)
    paired = result["paired"]
    assert paired["price_intersection_rows"] == 2
    assert paired["price_a_only_scored_rows"] == 1
    assert paired["price_b_only_scored_rows"] == 0
    assert paired["price_a_mae"] == pytest.approx(1.)
    assert paired["price_b_mae"] == pytest.approx(1.5)
    assert paired["price_paired_mean_abs_error_reduction"] == pytest.approx(-.5)
    assert paired["price_fraction_worsened"] == .5
    assert paired["price_fraction_unchanged"] == .5
    assert paired["vol_bp_intersection_rows"] == 1  # Independent finite-vol mask.
    assert paired["vol_bp_paired_mean_abs_error_reduction"] == pytest.approx(-100.)
    reversed_result = compare(dataset, b, a, name="reversed")
    assert reversed_result["paired"]["price_paired_mean_abs_error_reduction"] == pytest.approx(.5)


def test_empty_paired_intersection_reports_unknown_errors_not_perfect_fit(tmp_path):
    dataset, _ = prepare(tmp_path)
    a = candidate_receipt(dataset, "a", [0, 1], [1.1, 1.2], [.3, .4])
    b = candidate_receipt(dataset, "b", [2, 3], [1.1, 1.2], [.3, .4])
    result = compare(dataset, a, b)["paired"]
    assert result["price_intersection_rows"] == result["vol_bp_intersection_rows"] == 0
    assert result["price_a_mae"] is None
    assert result["price_b_mae"] is None
    assert result["price_paired_mean_abs_error_reduction"] is None
    assert result["price_fraction_improved"] is None


def test_comparison_rejects_a_receipt_from_another_input_cohort(tmp_path):
    original, _ = prepare(tmp_path, name="original")
    changed = source_table()
    changed = changed.set_column(changed.schema.get_field_index("okey_xx"), "okey_xx",
                                 pa.array([95., 100., 110.] * 2))
    other, _ = prepare(tmp_path, changed, name="other")
    a = candidate_receipt(original, "a", [0], [1.1], [.3])
    b = candidate_receipt(other, "b", [0], [1.1], [.3])
    with pytest.raises(ValueError, match="different quote cohort"):
        compare(original, a, b)


def test_comparison_verifies_actual_native_csv_digest_not_only_parquet(tmp_path):
    dataset, _ = prepare(tmp_path)
    a = candidate_receipt(dataset, "a", [0], [1.1], [.3])
    b = candidate_receipt(dataset, "b", [0], [1.1], [.3])
    changed = json.loads(b.read_text())
    changed["quote_csv_sha256"] = "0" * 64
    qb.write_json(b, changed)
    with pytest.raises(ValueError, match="quote CSV digest"):
        compare(dataset, a, b)


def test_report_strata_and_outside_reference_diagnostic_preserve_original_coverage(tmp_path):
    table = source_table()
    for column, values in {
        "okey_tk": ["AAA1", "AAA", "AAA", "BBB", "BBB", "BBB"],
        "okey_xx": [25., 50., 110., 90., 100., 110.],
        "ddivPv": [1., 0., 0., 0., 0., 0.],
        "srPrc": [100., 1.1, 1.1, 1.1, 1.1, 1.1],
    }.items():
        table = table.set_column(table.schema.get_field_index(column), column, pa.array(values))
    dataset, _ = prepare(tmp_path, table)
    _, receipt = prediction_receipt(dataset)
    result = score(dataset, receipt)
    assert result["metrics"]["eligible_rows"] == 6
    assert result["metrics"]["price_scored_rows"] == 6
    assert result["metrics"]["reference_outside_quote_rows"] == 1
    for rows in result["raw_strata"].values():
        assert sum(row["rows"] for row in rows) == 6
        assert sum(row["eligible_rows"] for row in rows) == 6
    kinds = {row["contract_kind"]: row for row in result["raw_strata"]["contract_kind"]}
    assert kinds["adjusted_suffix"]["eligible_rows"] == 1
    assert kinds["standard_symbol"]["eligible_rows"] == 5
    ref = {row["reference_location"]: row for row in result["reference_nbbo_diagnostic"]}
    assert ref["outside"]["price_scored_rows"] == 1  # It remains scored, not discarded.
    assert ref["inside"]["price_scored_rows"] == 5


def test_comparison_never_overwrites_a_provenance_artifact(tmp_path):
    dataset, _ = prepare(tmp_path)
    a = candidate_receipt(dataset, "a", [0], [1.1], [.3])
    b = candidate_receipt(dataset, "b", [0], [1.1], [.3])
    before = a.read_bytes()
    args = qb.parser().parse_args([
        "compare", "--dataset", str(dataset), "--run-a", str(a), "--run-b", str(b),
        "--output", str(a), "--split", "all",
    ])
    with pytest.raises(ValueError, match="output already exists"):
        qb.compare(args)
    assert a.read_bytes() == before
