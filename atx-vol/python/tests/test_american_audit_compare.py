"""Postimplementation checks of exact joins, refusal coverage and provenance."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import math
import os
import sys
from dataclasses import replace
from pathlib import Path

import pytest

PATH = Path(__file__).resolve().parents[2] / "tools" / "american_audit_compare.py"


def import_tool(path, module_name):
    spec = importlib.util.spec_from_file_location(module_name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


audit = import_tool(PATH, "american_audit_compare")


def interval():
    return {"calendar_end": .25, "variance_end": .2, "rate": .03,
            "continuous_yield": .01, "volatility": .2, "cash_at_end": 0.0}


def case(grid="default65", *, explicit=True, status="ok", spot=100.):
    result = {"fixture": "example", "grid": grid, "spot": spot,
              "intervals": [interval()], "model_status": status}
    if explicit:
        result["requested_strikes"] = [100.]
    if status == "ok":
        result.update(pricing_status="ok", prices=[
            {"expiry": 0, "strike": 100., "side": side, "exercise": "american", "price": 3.}
            for side in ("call", "put")])
    else:
        result["model_error"] = "resource cap"
    return result


def source(*cases, explicit=True):
    result = {"schema_version": 2, "cases": list(cases) or [case(), case("fine")]}
    if explicit:
        result.update(requested_sides=["call", "put"], requested_exercises=["american"])
    return result


def references():
    return {"schema_version": 1, "references": [
        {"reference_id": i, "arguments": {"spot": 100., "intervals": [interval()],
                                          "strike": 100., "side": side, "exercise": "american"},
         "status": "observed_stable", "reference_price": price,
         "refinement_uncertainty_estimate": .001, "stability_threshold": .01}
        for i, (side, price) in enumerate((("call", 4.), ("put", 2.)))]}


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def test_exact_join_and_paired_error_change_uses_same_contracts():
    document = source()
    document["cases"][1]["prices"][0]["price"] = 3.5
    document["cases"][1]["prices"][1]["price"] = 2.25
    report = audit.compare_documents(document, references())
    assert report["grids"]["default65"]["observed_stable_errors"]["mae"] == 1.
    assert report["grids"]["fine"]["observed_stable_errors"]["mae"] == .375
    paired = report["paired_against_baseline"]["fine"]
    assert paired["paired_rows"] == 2
    assert paired["mean_absolute_error_change"] == -.625
    assert paired["improved_rows"] == 2
    assert report["matched_reference_count"] == 2
    assert report["rows"][0]["refinement_uncertainty_estimate"] == .001


def test_renamed_fixture_joins_by_numeric_schedule_and_same_name_does_not_join_changed_spot():
    document = source()
    document["cases"][1]["fixture"] = "renamed"
    report = audit.compare_documents(document, references())
    assert len(report["fixtures"]) == 1
    assert report["fixtures"][0]["fixture_names"] == ["example", "renamed"]
    document["cases"][1]["spot"] = 101.
    report = audit.compare_documents(document, references())
    assert len(report["fixtures"]) == 2
    assert report["paired_against_baseline"]["fine"]["paired_rows"] == 0
    assert report["grids"]["fine"]["reference_status_counts"]["unmatched_reference"] == 2


@pytest.mark.parametrize("field", ["calendar_end", "variance_end", "rate", "continuous_yield",
                                   "volatility", "cash_at_end"])
def test_each_dual_clock_and_model_coefficient_is_part_of_exact_key(field):
    document = source()
    for item in document["cases"]:
        item["intervals"][0][field] += 1.0e-12
    report = audit.compare_documents(document, references())
    assert report["matched_reference_count"] == 0
    assert len(report["unmatched_references"]) == 2


def test_prefix_join_uses_entire_prefix_not_just_terminal_clock():
    document = source(case())
    entry = interval()
    entry.update(calendar_end=.5, variance_end=.3)
    document["cases"][0]["intervals"].append(entry)
    for row in document["cases"][0]["prices"]:
        row["expiry"] = 1
    refs = references()
    for ref in refs["references"]:
        ref["arguments"]["intervals"].append(copy.deepcopy(entry))
        ref["arguments"]["intervals"][0]["variance_end"] = .19
    report = audit.compare_documents(document, refs)
    assert report["matched_reference_count"] == 0


@pytest.mark.parametrize("status, expected", [("refused", "model_refused"),
                                              ("grid_refused", "grid_refused")])
def test_failed_grid_keeps_whole_requested_denominator(status, expected):
    report = audit.compare_documents(source(case(), case("fine", status=status)), references())
    candidate = report["grids"]["fine"]
    assert candidate["universe_rows"] == 2
    assert candidate["priced_rows"] == 0
    assert candidate["model_status_counts"] == {expected: 2}
    assert candidate["observed_stable_coverage"] == 0
    assert report["paired_against_baseline"]["fine"]["baseline_only_rows"] == 2
    assert candidate["observed_stable_errors"]["mae"] is None


def test_all_refused_explicit_universe_and_zero_error_count_are_distinct():
    report = audit.compare_documents(source(case(status="refused")), references())
    assert report["grids"]["default65"]["universe_rows"] == 2
    assert report["grids"]["default65"]["observed_stable_errors"]["count"] == 0
    assert report["fixtures"][0]["universe_basis"] == "explicit_requested_contracts"


def test_partial_missing_prices_and_different_requested_sets_do_not_improve_paired_selection():
    document = source()
    document["cases"][0]["prices"][0]["price"] = 40.
    document["cases"][1]["prices"].pop(0)
    report = audit.compare_documents(document, references())
    assert report["grids"]["fine"]["model_status_counts"] == {"missing_price": 1, "priced": 1}
    paired = report["paired_against_baseline"]["fine"]
    assert paired["baseline_only_rows"] == 1
    assert paired["baseline_paired_errors"]["mae"] == 1.
    assert paired["mean_absolute_error_change"] == 0.
    document["cases"][1]["requested_strikes"] = [110.]
    document["cases"][1]["prices"][0]["strike"] = 110.
    report = audit.compare_documents(document, references())
    assert report["grids"]["fine"]["model_status_counts"]["not_requested"] == 2


def test_missing_entire_numeric_case_counts_on_every_grid():
    other = case(spot=110.)
    other["fixture"] = "different-model"
    report = audit.compare_documents(source(case(), case("fine"), other), references())
    assert report["grids"]["fine"]["universe_rows"] == 4
    assert report["grids"]["fine"]["model_status_counts"]["case_absent"] == 2


def test_legacy_schema2_union_and_reference_inference_are_explicit():
    old = source(case(explicit=False), case("fine", explicit=False, status="refused"), explicit=False)
    old["cases"][0]["prices"].pop()
    report = audit.compare_documents(old, references())
    assert report["grids"]["fine"]["universe_rows"] == 2
    assert report["fixtures"][0]["universe_basis"].startswith("inferred_union")
    assert report["grids"]["default65"]["model_status_counts"]["missing_price"] == 1


def test_legacy_all_refused_unmatched_case_retains_unknown_denominator_and_case_status():
    old = source(case(explicit=False, status="refused", spot=101.), explicit=False)
    report = audit.compare_documents(old, references())
    grid = report["grids"]["default65"]
    assert grid["universe_rows"] == 0
    assert grid["priced_coverage"] is None
    assert grid["case_status_counts"] == {"model_refused": 1}
    assert grid["unknown_universe_cases"] == 1
    assert report["cases"][0]["error"] == "resource cap"


def test_unresolved_and_solver_failure_are_retained_and_excluded_from_stable_error():
    refs = references()
    refs["references"][0]["status"] = "unresolved"
    refs["references"][0]["refinement_uncertainty_estimate"] = 1.
    refs["references"][1] = {key: value for key, value in refs["references"][1].items()
                               if key in ("reference_id", "arguments")}
    refs["references"][1].update(status="solver_failure", reason="did not solve")
    report = audit.compare_documents(source(), refs)
    summary = report["grids"]["default65"]
    assert summary["observed_stable_errors"]["count"] == 0
    assert summary["unresolved_diagnostic_errors"]["mae"] == 1.
    assert summary["reference_status_counts"] == {"solver_failure": 1, "unresolved": 1}
    assert any(row["reference_reason"] == "did not solve" for row in report["rows"])


def test_replacement_requires_existing_exact_keys_and_preserves_both_reference_versions():
    original = references()
    original["references"][0]["status"] = "unresolved"
    replacement = references()
    replacement["references"] = replacement["references"][:1]
    replacement["references"][0]["reference_price"] = 3.8
    report = audit.compare_documents(source(), original, replacement_reference=replacement)
    receipt = report["reference_replacements"][0]
    assert receipt["original"]["status"] == "unresolved"
    assert receipt["replacement"]["reference_price"] == 3.8
    assert sum(row["reference_source"] == "replacement" for row in report["rows"]) == 2
    replacement["references"][0]["arguments"]["strike"] = 101.
    with pytest.raises(ValueError, match="absent from original"):
        audit.compare_documents(source(), original, replacement_reference=replacement)


def test_model_prices_in_reference_document_are_never_reference_truth():
    refs = references()
    refs["cases"] = [{"rows": [{"source_price": 100000., "reference_price": 100000.}]}]
    report = audit.compare_documents(source(), refs)
    assert report["grids"]["default65"]["observed_stable_errors"]["mae"] == 1.


@pytest.mark.parametrize("mutation", ["reference_key", "reference_id", "case", "price", "strike"])
def test_duplicate_input_records_refuse(mutation):
    document, refs = source(), references()
    if mutation == "reference_key":
        duplicate = copy.deepcopy(refs["references"][0])
        duplicate["reference_id"] = 2
        refs["references"].append(duplicate)
    elif mutation == "reference_id":
        refs["references"][1]["reference_id"] = 0
    elif mutation == "case":
        document["cases"].append(copy.deepcopy(document["cases"][0]))
    elif mutation == "price":
        document["cases"][0]["prices"].append(copy.deepcopy(document["cases"][0]["prices"][0]))
    else:
        document["cases"][0]["requested_strikes"].append(100.)
    with pytest.raises(ValueError, match="duplicate|unique"):
        audit.compare_documents(document, refs)


@pytest.mark.parametrize("value", [math.nan, math.inf, -1., True, "3.0"])
def test_invalid_model_price_is_never_silently_skipped(value):
    document = source()
    document["cases"][0]["prices"][0]["price"] = value
    with pytest.raises(ValueError):
        audit.compare_documents(document, references())


def test_nonfinite_reference_negative_uncertainty_and_false_stability_refuse():
    for field, value in (("reference_price", math.nan), ("refinement_uncertainty_estimate", -1.),
                          ("refinement_uncertainty_estimate", 1.)):
        refs = references()
        refs["references"][0][field] = value
        with pytest.raises(ValueError):
            audit.compare_documents(source(), refs)


def test_inexact_large_integer_cannot_silently_change_exact_signature():
    refs = references()
    refs["references"][0]["arguments"]["strike"] = 2**53 + 1
    with pytest.raises(ValueError, match="represented exactly"):
        audit.compare_documents(source(), refs)


def test_malformed_clocks_unknown_status_and_outside_requested_contract_refuse():
    for mutation in ("clock", "status", "requested", "side", "expiry", "interval_schema"):
        document = source()
        item = document["cases"][0]
        if mutation == "clock":
            item["intervals"][0]["variance_end"] = -1.
        elif mutation == "status":
            item["pricing_status"] = "pretend_ok"
        elif mutation == "requested":
            item["requested_strikes"] = [101.]
        elif mutation == "side":
            item["prices"][0]["side"] = "unknown"
        elif mutation == "expiry":
            item["prices"][0]["expiry"] = True
        else:
            item["intervals"][0]["local_volatility"] = []
        with pytest.raises(ValueError):
            audit.compare_documents(document, references())


@pytest.mark.parametrize("limit", ["max_cases", "max_price_rows", "max_references", "max_expanded_rows"])
def test_resource_limits_are_enforced_before_large_expansion(limit):
    with pytest.raises(ValueError):
        audit.compare_documents(source(), references(), limits=replace(audit.Limits(), **{limit: 1}))


def test_cli_receipts_and_immutable_output(tmp_path):
    src = write_json(tmp_path / "source.json", source())
    ref = write_json(tmp_path / "ref.json", references())
    replacement = references()
    replacement["references"] = replacement["references"][:1]
    repl = write_json(tmp_path / "repl.json", replacement)
    out = tmp_path / "out.json"
    args = ["--input", str(src), "--reference", str(ref), "--replacement-reference", str(repl),
            "--output", str(out)]
    assert audit.main(args) == 0
    before = out.read_bytes()
    report = json.loads(before)
    for path, role in ((src, "input"), (ref, "reference"), (repl, "replacement_reference")):
        assert report["provenance"][role]["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest()
    assert report["tool_sha256"] == audit.TOOL_SHA256
    assert audit.main(args) == 2
    assert out.read_bytes() == before


def test_alias_and_hardlink_rejected_without_touching_source(tmp_path):
    src = write_json(tmp_path / "source.json", source())
    ref = write_json(tmp_path / "ref.json", references())
    before = src.read_bytes()
    assert audit.main(["--input", str(src), "--reference", str(ref), "--output", str(src)]) == 2
    alias = tmp_path / "hardlink.json"
    os.link(src, alias)
    with pytest.raises(ValueError, match="hardlinks"):
        audit.check_distinct_paths([src, alias])
    assert src.read_bytes() == before


@pytest.mark.parametrize("payload", [b'{"schema_version":2,"schema_version":2}',
                                      b'{"value":NaN}', b'{"value":1e999}', b'[]'])
def test_json_duplicate_keys_nonfinite_and_nonobject_roots_refuse(tmp_path, payload):
    path = tmp_path / "malformed.json"
    path.write_bytes(payload)
    with pytest.raises(ValueError):
        audit.load_document(path, audit.Limits())


def test_bom_byte_hash_size_bound_and_depth_bound(tmp_path):
    path = tmp_path / "input.json"
    payload = b"\xef\xbb\xbf" + json.dumps(source()).encode()
    path.write_bytes(payload)
    document, receipt = audit.load_document(path, audit.Limits())
    assert document == source()
    assert receipt["sha256"] == hashlib.sha256(payload).hexdigest()
    with pytest.raises(ValueError, match="byte limit"):
        audit.load_document(path, replace(audit.Limits(), max_file_bytes=10))
    path.write_text('{"nested":' + '[' * 34 + '0' + ']' * 34 + '}', encoding="utf-8")
    with pytest.raises(ValueError, match="nesting"):
        audit.load_document(path, audit.Limits())


def test_source_hash_is_captured_at_import_not_recomputed_after_execution(tmp_path):
    copy_path = tmp_path / "tool_copy.py"
    copy_path.write_bytes(PATH.read_bytes())
    module = import_tool(copy_path, "audit_compare_hash_test")
    original_hash = module.TOOL_SHA256
    with copy_path.open("ab") as stream:
        stream.write(b"\n# changed after import\n")
    report = module.compare_documents(source(), references())
    assert report["tool_sha256"] == original_hash
    assert original_hash != hashlib.sha256(copy_path.read_bytes()).hexdigest()
