"""Shape-class matrix harness (tools/shape_matrix.py): spec guard, per-class columns, table."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import polars as pl
import pytest


TOOLS = Path(__file__).resolve().parents[2] / "tools"
TESTS = Path(__file__).resolve().parent


def _load(name: str, folder: Path):
    spec = importlib.util.spec_from_file_location(name, folder / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules.setdefault(name, module)
    spec.loader.exec_module(module)
    return module


qb = _load("quote_benchmark", TOOLS)
sm = _load("shape_matrix", TOOLS)
fixtures = _load("test_quote_benchmark", TESTS)  # the scorer's own synthetic cohort
IDS = {"row_id": pl.UInt64, "chain_id": pl.UInt64}


def write_spec(tmp_path: Path, **change) -> Path:
    spec = {"schema_version": 1, "binary": "bench.exe", "class_manifest": "m.json",
            "split": "development", "cohorts": {"dev": str(tmp_path)},
            "policies": {"p": "auto"}}
    spec.update(change)
    path = tmp_path / "spec.json"
    path.write_text(json.dumps(spec), encoding="utf-8")
    return path


def test_spec_is_development_only_bounded_and_names_known_cells(tmp_path):
    spec = sm.load_spec(write_spec(tmp_path))
    assert spec["cells"] == [["p", "dev"]] and spec["workers"] == 2
    assert spec["max_cell_seconds"] == sm.MAX_CELL_SECONDS
    for change, message in (({"split": "validation"}, "development split only"),
                            ({"split": "all"}, "development split only"),
                            ({"cells": [["p", "other"]]}, "unknown policy or cohort"),
                            ({"policies": {"a/b": "auto"}}, "bare"),
                            ({"max_cell_seconds": 601}, "max_cell_seconds"),
                            ({"workers": 0}, "workers"),
                            ({"schema_version": 2}, "schema version")):
        with pytest.raises(ValueError, match=message):
            sm.load_spec(write_spec(tmp_path, **change))


def test_completeness_buckets_have_closed_edges():
    assert [sm._bucket(f) for f in (1.0, 0.95, 0.9, 0.89, 0.5, 0.49, 1e-9, 0.0)] == [
        "complete", "ge_0.9", "ge_0.9", "ge_0.5", "ge_0.5", "gt_0", "gt_0", "none"]
    assert sm._quantile([], 0.5) is None and sm._quantile([4.0], 0.95) == 4.0
    assert sm._quantile([1.0, 2.0, 3.0], 0.5) == 2.0


def scored_cell(tmp_path: Path):
    """The scorer's shape cohort with two of nine rows refused, scored on development."""
    dataset, receipt = fixtures.shape_run(tmp_path)
    run = json.loads(receipt.read_text(encoding="utf-8"))
    csv = Path(run["predictions"])
    frame = pl.read_csv(csv, schema_overrides=IDS)
    frame = frame.with_columns(
        pl.when(pl.int_range(pl.len()) < 2).then(pl.lit("calendar_arbitrage"))
        .otherwise(pl.col("status")).alias("status"),
        pl.when(pl.int_range(pl.len()) < 2).then(None)
        .otherwise(pl.col("predicted_price")).alias("predicted_price"))
    frame.write_csv(csv)
    run["prediction_sha256"] = qb.sha256(csv)
    qb.write_json(receipt, run)
    manifest = fixtures.class_manifest(dataset, fixtures.SHAPE_CLASSES)
    split = pl.read_parquet(dataset / "quotes.parquet")["split"].unique().to_list()
    if "development" not in split:
        pytest.skip("synthetic cohort drew no development underlier")  # never on this fixture
    score = dataset / "score.json"
    qb.score(qb.parser().parse_args(["score", "--dataset", str(dataset), "--run-receipt",
                                     str(receipt), "--output", str(score), "--split",
                                     "development", "--class-manifest", str(manifest)]))
    return dataset, manifest, csv, score


def test_cell_classes_add_taxonomy_completeness_and_cost_to_the_scorer(tmp_path):
    dataset, manifest, csv, score = scored_cell(tmp_path)
    quotes = pl.read_parquet(dataset / "quotes.parquet").filter(pl.col("split") == "development")
    chains = quotes["chain_id"].unique().to_list()
    native = tmp_path / "native.json"
    native.write_text(json.dumps({"graded_stats": {"slices": [
        {"chain_id": c, "timing": {"wall_us": 1000.0 * (i + 1)}} for i, c in enumerate(chains)]}}),
        encoding="utf-8")
    classes = sm.cell_classes(dataset, manifest, csv, score, native)
    assert list(classes) == ["aaa", "far", "near", "no_symbol_class", "all_rows"]
    strata = {r["shape_class"]: r for r in
              json.loads(score.read_text(encoding="utf-8"))["class_strata"]["classes"]}
    for name, c in classes.items():
        # The scorer's metrics are carried verbatim; the taxonomy partitions eligible rows.
        assert c["eligible_price_coverage"] == strata[name]["price_coverage"]
        assert c["mae_spread"] == strata[name]["price_in_spreads_mae"]
        assert c["violations"] == strata[name]["served_shape_violations"]
        assert sum(c["refusal_taxonomy"].values()) == c["eligible_rows"]
        assert c["eligible_rows"] == strata[name]["eligible_rows"]
    everything = classes["all_rows"]
    pred = pl.read_csv(csv, schema_overrides=IDS).join(quotes.select("row_id"), on="row_id",
                                                        how="semi")
    refused = pred.filter(pl.col("status") == "calendar_arbitrage").height
    assert everything["refusal_taxonomy"].get("calendar_arbitrage", 0) == refused
    assert sum(everything["completeness"].values()) == len(chains)
    assert everything["expiries_timed"] == len(chains)
    assert everything["expiry_fit_ms_p50"] == sm._quantile(
        [float(i + 1) for i in range(len(chains))], 0.5)


def test_table_has_one_row_per_class_and_names_failed_cells(tmp_path):
    dataset, manifest, csv, score = scored_cell(tmp_path)
    record = {"policy": "p", "cohort": "dev", "prediction_sha256": "ab" * 32,
              "classes": sm.cell_classes(dataset, manifest, csv, score, None)}
    failed = {"policy": "q", "cohort": "dev", "run": {"exit": 1}, "error": "refused at load"}
    out = tmp_path / "out"
    out.mkdir()
    sm.write_table(out, [record, failed])
    table = (out / "table.md").read_text(encoding="utf-8").splitlines()
    rows = [line for line in table if line.startswith("| dev | p |")]
    assert len(rows) == 5 and all("`abababababababab`" in line for line in rows)
    assert any(line.startswith("| dev | q | (cell failed:") for line in table)
    document = json.loads((out / "table.json").read_text(encoding="utf-8"))
    assert [c["policy"] for c in document["cells"]] == ["p", "q"]
