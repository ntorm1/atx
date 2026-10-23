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


def selection_cell(policy: str, cohort: str, **classes) -> dict:
    return {"policy": policy, "cohort": cohort,
            "classes": {name: {"eligible_rows": 10, "eligible_price_coverage": cov,
                               "containment": contain, "mae_spread": mae,
                               "violations": {"monotone": 0, "convex": 0, "calendar": bad},
                               "duplicate_disagreements": 0}
                        for name, (cov, contain, mae, bad) in classes.items()}}


def test_selection_is_deterministic_and_never_picks_an_unqualified_policy(tmp_path):
    cells = [selection_cell("b", "dev", x=(0.9, 0.99, 0.10, 0), y=(0.8, 0.99, 0.10, 0)),
             selection_cell("a", "dev", x=(0.9, 0.99, 0.10, 0), y=(0.95, 0.97, 0.05, 0)),
             selection_cell("c", "dev", x=(0.99, 0.99, 0.01, 1), y=(0.7, 0.99, 0.02, 0))]
    # x: a and b tie on coverage and MAE/spread -> the smaller name; c has a violation.
    assert sm.choose(cells, "dev", "x", 0.985) == "a"
    # y: a is under the containment bar; b covers more than c.
    assert sm.choose(cells, "dev", "y", 0.985) == "b"
    assert sm.choose(cells, "dev", "z", 0.985) is None
    table = tmp_path / "table.json"
    table.write_text(json.dumps({"cells": cells}), encoding="utf-8")
    out = tmp_path / "select.json"
    assert sm.main(["select", "--table", str(table), "--output", str(out)]) == 0
    document = json.loads(out.read_text(encoding="utf-8"))
    assert document["winners"] == {"dev": {"x": "a", "y": "b"}}
    assert document["single_policy"] == "a" and document["wins"] == {"a": 1, "b": 1}
    with pytest.raises(ValueError, match="already exists"):
        sm.main(["select", "--table", str(table), "--output", str(out)])


def test_validated_only_selection_admits_only_publication_validate_policies(tmp_path):
    # C2's validation is the admission bar for the combined candidate: with --validated-only a
    # policy qualifies only when its policy file sets graded.publication = validate.
    plain = tmp_path / "plain.json"
    plain.write_text(json.dumps({"schema_version": 1, "admission": "graded", "graded": {}}),
                     encoding="utf-8")
    checked = tmp_path / "checked.json"
    checked.write_text(json.dumps({"schema_version": 1, "admission": "graded",
                                   "graded": {"publication": "validate"}}), encoding="utf-8")
    best = selection_cell("best", "dev", x=(0.99, 0.99, 0.01, 0))
    best["policy_path"] = str(plain)
    valid = selection_cell("valid", "dev", x=(0.90, 0.99, 0.10, 0))
    valid["policy_path"] = str(checked)
    unknown = selection_cell("unknown", "dev", x=(0.995, 0.99, 0.01, 0))  # no policy_path
    cells = [best, valid, unknown]
    assert sm.choose(cells, "dev", "x", 0.985) == "unknown"
    assert sm.choose(cells, "dev", "x", 0.985, validated_only=True) == "valid"
    table = tmp_path / "table.json"
    table.write_text(json.dumps({"cells": cells}), encoding="utf-8")
    out = tmp_path / "select.json"
    assert sm.main(["select", "--table", str(table), "--output", str(out),
                    "--validated-only"]) == 0
    document = json.loads(out.read_text(encoding="utf-8"))
    assert document["validated_only"] is True
    assert document["winners"] == {"dev": {"x": "valid"}}


def test_expiry_service_counts_served_slices_and_types_every_unserved_one():
    # One row per (chain, status, served); chain = one fitted expiry slice.
    eligible = pl.DataFrame({
        "chain_id": [1, 1, 2, 3, 4, 5, 6],
        "status": ["ok", "calendar_arbitrage", "calendar_arbitrage", "arbitrage_failure",
                   "calendar_arbitrage", "unsupported_deliverable", "market_distance_failure"],
        "served": [True, False, False, False, False, False, False]},
        schema_overrides={"chain_id": pl.UInt64})
    slices = {
        1: {"grade": "A"},
        2: {"grade": "calendar_arbitrage", "error": "American calendar price crossing"},
        3: {"grade": "arbitrage_failure", "error": "InvalidArgument: AmericanPriceSlice: node 11: "
            "American vertical-spread slope bound violated"},
        4: {"grade": "calendar_arbitrage", "error": "American calendar price crossing",
            "publication": {"rollbacks": 2}},
        6: {"grade": "B"},
    }
    got = sm.expiry_service(eligible, slices)
    assert got["expiries"] == 6 and got["expiries_served"] == 1
    assert got["expiry_completeness"] == pytest.approx(1 / 6)
    assert got["unserved_reasons"] == {
        "arbitrage:vertical_spread": 1, "calendar": 1, "not_fitted:unsupported_deliverable": 1,
        "rows_refused:market_distance_failure": 1, "validator_rollback+calendar": 1}
    assert sm.slice_reason({"grade": "arbitrage_failure", "error": "InvalidArgument: "
                            "nondividend American: scaled European upper bound violated at "
                            "strike 3.9"}, []) == "arbitrage:nondividend_upper_bound"
    assert sm.slice_reason({"grade": "arbitrage_failure", "error": "InvalidArgument: "
                            "AmericanPriceSlice: node 4: call/put convexity violated above the "
                            "global lower convex envelope"}, []) == "arbitrage:convexity"
    assert sm.slice_reason({"grade": "insufficient_quotes"}, []) == "insufficient_quotes"
    assert sm.expiry_service(eligible.head(0), {})["expiry_completeness"] is None


def test_a_zero_mae_spread_wins_its_tie_break():
    # Review C3 minor: `mae_spread or inf` turned a real 0.0 into +inf in the tie-break.
    cells = [selection_cell("a", "dev", x=(0.9, 0.99, 0.05, 0)),
             selection_cell("b", "dev", x=(0.9, 0.99, 0.0, 0))]
    assert sm.choose(cells, "dev", "x", 0.985) == "b"
    cells[1]["classes"]["x"]["mae_spread"] = None  # unknown MAE ranks last, not first
    assert sm.choose(cells, "dev", "x", 0.985) == "a"


def test_bounded_subprocess_times_out_instead_of_hanging():
    # Review C3 minor: the score step had no timeout; both steps now share one bound.
    code, stderr = sm.bounded([sys.executable, "-c", "import time; time.sleep(30)"], 0.5)
    assert code == "timeout" and stderr == ""
    code, _ = sm.bounded([sys.executable, "-c", "raise SystemExit(3)"], 30)
    assert code == 3


def test_resume_keeps_a_failed_cell_and_its_first_error(tmp_path, monkeypatch):
    # Review C3 minor: resume re-ran a failed cell (no CSV), the re-run failed on the leftover
    # outputs and the table then showed that error instead of the real one.
    binary = tmp_path / "bench.exe"
    binary.write_bytes(b"x")
    spec = write_spec(tmp_path, binary=str(binary))
    out = tmp_path / "out"
    out.mkdir()
    first = {"policy": "p", "cohort": "dev", "run": {"exit": 2, "stderr_tail": "the real error"}}
    later = {"policy": "p", "cohort": "dev", "run": {"exit": 2, "stderr_tail": "output exists"}}
    (out / "cells.jsonl").write_text(json.dumps(first) + "\n" + json.dumps(later) + "\n",
                                     encoding="utf-8")
    calls = []
    monkeypatch.setattr(sm, "run_cell", lambda *a: calls.append(a) or dict(later))
    assert sm.main(["run", "--spec", str(spec), "--output", str(out)]) == 1
    assert calls == []
    cell = json.loads((out / "table.json").read_text(encoding="utf-8"))["cells"][0]
    assert cell["run"]["stderr_tail"] == "the real error" and cell["resumed_failed"] is True
    assert sm.main(["run", "--spec", str(spec), "--output", str(out), "--rerun-failed"]) == 1
    assert len(calls) == 1


def test_a_rerun_never_overwrites_the_first_stderr(tmp_path, monkeypatch):
    spec = sm.load_spec(write_spec(tmp_path))
    out = tmp_path / "out"
    answers = iter([(2, "first failure"), (2, "second failure")])
    monkeypatch.setattr(sm, "bounded", lambda *a, **k: next(answers))
    for _ in range(2):
        record = sm.run_cell(spec, out, "p", "dev")
        assert record["run"]["exit"] == 2
    cell = out / "runs" / "dev" / "p"
    assert (cell / "run.stderr.txt").read_text(encoding="utf-8") == "first failure"
    assert (cell / "run.stderr.rerun-1.txt").read_text(encoding="utf-8") == "second failure"
