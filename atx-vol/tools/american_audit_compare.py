#!/usr/bin/env python3
"""Compare finite-model grids with saved independent numerical references.

Exact pricing-input joins, explicit coverage and paired populations. This tool
does not run a reference solver and never treats model prices as reference truth.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

TOOL_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
INTERVAL_FIELDS = {"calendar_end", "variance_end", "rate", "continuous_yield",
                   "volatility", "cash_at_end"}
ARGUMENT_FIELDS = {"spot", "intervals", "strike", "side", "exercise"}
REFERENCE_STATUSES = {"observed_stable", "unresolved", "solver_failure", "unsupported"}
PRICING_STATUSES = {"ok", "workspace_refused", "batch_refused", "scalar_refused"}


@dataclass(frozen=True)
class Limits:
    max_file_bytes: int = 64 * 1024 * 1024
    max_cases: int = 4096
    max_price_rows: int = 500000
    max_references: int = 100000
    max_expanded_rows: int = 500000

    def validate(self):
        for name, cap in (("max_file_bytes", 256 * 1024 * 1024), ("max_cases", 16384),
                          ("max_price_rows", 2000000), ("max_references", 1000000),
                          ("max_expanded_rows", 2000000)):
            value = getattr(self, name)
            if type(value) is not int or not 1 <= value <= cap:
                raise ValueError(f"{name} must be an integer in [1, {cap}]")


def finite(value, name, *, positive=False, nonnegative=False):
    if type(value) not in (int, float):
        raise ValueError(f"{name} must be numeric")
    try:
        result = float(value)
    except OverflowError as error:
        raise ValueError(f"{name} exceeds finite double range") from error
    if type(value) is int and result != value:
        raise ValueError(f"{name} integer cannot be represented exactly as a double")
    if not math.isfinite(result) or (positive and result <= 0) or (nonnegative and result < 0):
        raise ValueError(f"{name} is outside its finite domain")
    return 0.0 if result == 0 else result


def name(value, field):
    if not isinstance(value, str) or not 1 <= len(value) <= 256:
        raise ValueError(f"{field} must be a nonempty bounded string")
    return value


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def intervals(value):
    if not isinstance(value, list) or not 1 <= len(value) <= 64:
        raise ValueError("intervals must have 1..64 entries")
    out, previous_calendar, previous_variance = [], 0.0, 0.0
    for item in value:
        if not isinstance(item, dict) or set(item) != INTERVAL_FIELDS:
            raise ValueError("unsupported interval schema; local volatility is not inferred")
        item = {key: finite(item[key], key) for key in sorted(INTERVAL_FIELDS)}
        if (item["calendar_end"] <= previous_calendar or item["variance_end"] < previous_variance
                or item["volatility"] < 0 or item["cash_at_end"] < 0):
            raise ValueError("invalid dual clocks, volatility or cash")
        previous_calendar, previous_variance = item["calendar_end"], item["variance_end"]
        out.append(item)
    return out


def arguments(value):
    if not isinstance(value, dict) or set(value) != ARGUMENT_FIELDS:
        raise ValueError("reference arguments must contain the exact pricing input schema")
    if value["side"] not in ("call", "put") or value["exercise"] not in ("american", "european"):
        raise ValueError("unknown side or exercise")
    return {"spot": finite(value["spot"], "spot", positive=True),
            "intervals": intervals(value["intervals"]),
            "strike": finite(value["strike"], "strike", positive=True),
            "side": value["side"], "exercise": value["exercise"]}


def reference_index(document, limits):
    if not isinstance(document, dict) or type(document.get("schema_version")) is not int:
        raise ValueError("reference document requires schema_version=1")
    rows = document.get("references")
    if document["schema_version"] != 1 or not isinstance(rows, list):
        raise ValueError("reference document requires schema_version=1 and references array")
    if not 1 <= len(rows) <= limits.max_references:
        raise ValueError("reference count outside resource limit")
    result, ids = {}, set()
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("reference record must be an object")
        args = arguments(row.get("arguments"))
        key = canonical(args)
        if key in result:
            raise ValueError("duplicate exact reference signature")
        ref_id = row.get("reference_id")
        if type(ref_id) is not int or ref_id < 0 or ref_id in ids:
            raise ValueError("reference_id must be a unique nonnegative integer")
        ids.add(ref_id)
        status = row.get("status")
        if status not in REFERENCE_STATUSES:
            raise ValueError("unknown reference status")
        record = {"arguments": args, "reference_id": ref_id, "status": status}
        if status in ("observed_stable", "unresolved"):
            for field in ("reference_price", "refinement_uncertainty_estimate", "stability_threshold"):
                record[field] = finite(row.get(field), field, nonnegative=True)
            if (status == "observed_stable" and record["refinement_uncertainty_estimate"]
                    > record["stability_threshold"]):
                raise ValueError("observed-stable reference exceeds its stability threshold")
        else:
            if any(row.get(field) is not None for field in
                   ("reference_price", "refinement_uncertainty_estimate")):
                raise ValueError("failed reference may not publish a usable price or uncertainty")
            reason = row.get("reason", "")
            if not isinstance(reason, str) or len(reason) > 4096:
                raise ValueError("reference reason must be a bounded string")
            record["reason"] = reason
        record["uncertainty_scope"] = row.get(
            "uncertainty_scope", "observed refinement differences, not a proven error bound")
        result[key] = record
    return result


def merge_references(document, replacement, limits):
    result = reference_index(document, limits)
    replacements = []
    if replacement is not None:
        updates = reference_index(replacement, limits)
        if not updates.keys() <= result.keys():
            raise ValueError("replacement reference contains keys absent from original reference")
        for key, updated in updates.items():
            old = result[key]
            replacements.append({"contract_sha256": digest(updated["arguments"]),
                                 "original": old, "replacement": updated})
            result[key] = {**updated, "reference_source": "replacement"}
    for record in result.values():
        record.setdefault("reference_source", "original")
    return result, replacements


def choices(value, allowed, field):
    if (not isinstance(value, list) or not value or len(value) > len(allowed)
            or any(item not in allowed for item in value) or len(set(value)) != len(value)):
        raise ValueError(f"invalid {field}")
    return value


def case_status(case):
    grid_status = case.get("grid_status", "ok")
    model_status = case.get("model_status")
    pricing_status = case.get("pricing_status")
    if grid_status not in ("ok", "refused"):
        raise ValueError("unknown grid_status")
    if grid_status == "refused":
        if model_status is not None or pricing_status is not None:
            raise ValueError("refused grid cannot also publish model/pricing status")
        return "grid_refused"
    if model_status not in ("ok", "refused", "grid_refused"):
        raise ValueError("unknown or missing model_status")
    if model_status in ("refused", "grid_refused"):
        if pricing_status is not None:
            raise ValueError("refused model cannot publish pricing status")
        return "model_refused" if model_status == "refused" else "grid_refused"
    if pricing_status not in PRICING_STATUSES:
        raise ValueError("unknown or missing pricing_status")
    return "priced" if pricing_status == "ok" else pricing_status


def make_arguments(spot, schedule, expiry, strike, side, exercise):
    return {"spot": spot, "intervals": schedule[:expiry + 1], "strike": strike,
            "side": side, "exercise": exercise}


def source_cases(document, limits):
    if (not isinstance(document, dict) or type(document.get("schema_version")) is not int
            or document["schema_version"] != 2 or not isinstance(document.get("cases"), list)
            or not 1 <= len(document["cases"]) <= limits.max_cases):
        raise ValueError("source requires schema_version=2 and bounded nonempty cases")
    sides = document.get("requested_sides")
    exercises = document.get("requested_exercises")
    if (sides is None) != (exercises is None):
        raise ValueError("requested_sides and requested_exercises must appear together")
    if sides is not None:
        choices(sides, {"call", "put"}, "requested_sides")
        choices(exercises, {"american", "european"}, "requested_exercises")
    models, grids, count, requested_count = {}, set(), 0, 0
    for raw in document["cases"]:
        if not isinstance(raw, dict):
            raise ValueError("case must be an object")
        fixture, grid = name(raw.get("fixture"), "fixture"), name(raw.get("grid"), "grid")
        spot = finite(raw.get("spot"), "spot", positive=True)
        schedule = intervals(raw.get("intervals"))
        model_key = canonical({"spot": spot, "intervals": schedule})
        status = case_status(raw)
        prices = raw.get("prices", [])
        if not isinstance(prices, list) or (status != "priced" and prices):
            raise ValueError("prices require successful model and pricing statuses")
        if status == "priced" and not prices:
            raise ValueError("successful pricing case cannot have no prices")
        count += len(prices)
        if count > limits.max_price_rows:
            raise ValueError("source price row limit exceeded")
        model = models.setdefault(model_key, {"spot": spot, "intervals": schedule,
                                             "fixtures": set(), "cases": {}, "universe": {}})
        if grid in model["cases"]:
            raise ValueError("duplicate numeric model/grid case, even if fixture names differ")
        model["fixtures"].add(fixture)
        grids.add(grid)
        produced = {}
        for row in prices:
            if not isinstance(row, dict) or set(row) != {"expiry", "strike", "side", "exercise", "price"}:
                raise ValueError("invalid produced price-row schema")
            expiry = row["expiry"]
            if type(expiry) is not int or not 0 <= expiry < len(schedule):
                raise ValueError("invalid expiry index")
            args = arguments(make_arguments(spot, schedule, expiry, row["strike"],
                                            row["side"], row["exercise"]))
            key = canonical(args)
            if key in produced:
                raise ValueError("duplicate price contract")
            produced[key] = finite(row["price"], "price", nonnegative=True)
            model["universe"][key] = args
        requested = None
        if "requested_strikes" in raw:
            strikes = raw["requested_strikes"]
            if (sides is None or not isinstance(strikes, list) or not 1 <= len(strikes) <= 4096):
                raise ValueError("explicit strikes require bounded list and top-level sides/exercises")
            strikes = [finite(value, "requested_strike", positive=True) for value in strikes]
            if len(set(strikes)) != len(strikes):
                raise ValueError("duplicate requested strike")
            requested_count += len(schedule) * len(strikes) * len(sides) * len(exercises)
            if requested_count > limits.max_expanded_rows:
                raise ValueError("requested row limit exceeded")
            requested = {}
            for expiry in range(len(schedule)):
                for strike in strikes:
                    for side in sides:
                        for exercise in exercises:
                            args = make_arguments(spot, schedule, expiry, strike, side, exercise)
                            requested[canonical(args)] = args
            if not produced.keys() <= requested.keys():
                raise ValueError("produced contract not in explicit requested universe")
            model["universe"].update(requested)
        model["cases"][grid] = {"fixture": fixture, "status": status, "prices": produced,
                                 "requested": requested,
                                 "error": raw.get("grid_error", raw.get(
                                     "model_error", raw.get("pricing_error")))}
    return models, sorted(grids)


def metrics(rows):
    differences = [row["difference"] for row in rows if row["difference"] is not None]
    if not differences:
        return {"count": 0, "mae": None, "max_absolute_error": None, "mean_signed_error": None}
    n = len(differences)
    return {"count": n, "mae": math.fsum(abs(value) / n for value in differences),
            "max_absolute_error": max(map(abs, differences)),
            "mean_signed_error": math.fsum(value / n for value in differences)}


def summary(rows):
    priced = sum(row["model_status"] == "priced" for row in rows)
    stable = [row for row in rows if row["reference_status"] == "observed_stable"]
    compared = sum(row["difference"] is not None for row in stable)
    return {"universe_rows": len(rows), "priced_rows": priced,
            "priced_coverage": priced / len(rows) if rows else None,
            "model_status_counts": dict(sorted(Counter(row["model_status"] for row in rows).items())),
            "reference_status_counts": dict(sorted(Counter(
                row["reference_status"] for row in rows).items())),
            "observed_stable_reference_rows": len(stable),
            "observed_stable_compared_rows": compared,
            "observed_stable_coverage": compared / len(stable) if stable else None,
            "observed_stable_errors": metrics(stable),
            "unresolved_diagnostic_errors": metrics(
                [row for row in rows if row["reference_status"] == "unresolved"])}


def paired_summary(baseline, candidate):
    stable_keys = {key for key, row in baseline.items()
                   if row["reference_status"] == "observed_stable"}
    base_priced = {key for key in stable_keys if baseline[key]["difference"] is not None}
    candidate_priced = {key for key in stable_keys if candidate[key]["difference"] is not None}
    common = sorted(base_priced & candidate_priced)
    transitions = Counter((baseline[key]["model_status"], candidate[key]["model_status"])
                          for key in baseline)
    delta = [abs(candidate[key]["difference"]) - abs(baseline[key]["difference"]) for key in common]
    return {"universe_rows": len(baseline), "observed_stable_reference_rows": len(stable_keys),
            "paired_rows": len(common), "baseline_only_rows": len(base_priced - candidate_priced),
            "candidate_only_rows": len(candidate_priced - base_priced),
            "neither_rows": len(stable_keys - base_priced - candidate_priced),
            "baseline_paired_errors": metrics([baseline[key] for key in common]),
            "candidate_paired_errors": metrics([candidate[key] for key in common]),
            "mean_absolute_error_change": math.fsum(value / len(delta) for value in delta)
                if delta else None,
            "improved_rows": sum(value < 0 for value in delta),
            "worsened_rows": sum(value > 0 for value in delta),
            "unchanged_rows": sum(value == 0 for value in delta),
            "model_status_transitions": [{"baseline": pair[0], "candidate": pair[1], "rows": count}
                                         for pair, count in sorted(transitions.items())]}


def compare_documents(source, reference, *, replacement_reference=None,
                      baseline_grid="default65", limits=Limits()):
    limits.validate()
    name(baseline_grid, "baseline_grid")
    refs, replacements = merge_references(reference, replacement_reference, limits)
    models, grids = source_cases(source, limits)
    if baseline_grid not in grids:
        raise ValueError("baseline grid is absent")
    # Exact prefix indexing bounds the join to O(reference_count + cases*64),
    # without guessing contracts from a fixture name or source price.
    prefixes = {}
    for key, ref in refs.items():
        args = ref["arguments"]
        prefix = canonical({"spot": args["spot"], "intervals": args["intervals"]})
        prefixes.setdefault(prefix, {})[key] = args
    output_rows, per_grid, matched_refs, expanded = [], {grid: {} for grid in grids}, set(), 0
    fixtures, fixture_rows, case_results = [], {}, []
    for model_key, model in sorted(models.items()):
        explicit = all(case["requested"] is not None for case in model["cases"].values())
        if not explicit:
            for expiry in range(len(model["intervals"])):
                prefix = canonical({"spot": model["spot"], "intervals": model["intervals"][:expiry + 1]})
                model["universe"].update(prefixes.get(prefix, {}))
        fixture_id = digest(json.loads(model_key))
        fixture_rows[fixture_id] = {grid: [] for grid in grids}
        fixtures.append({"fixture_id": fixture_id, "fixture_names": sorted(model["fixtures"]),
                         "spot": model["spot"], "intervals": model["intervals"],
                         "universe_contracts": len(model["universe"]),
                         "universe_basis": "explicit_requested_contracts" if explicit else
                             "inferred_union_of_produced_requested_and_exact_reference_contracts"})
        expanded += len(model["universe"]) * len(grids)
        if expanded > limits.max_expanded_rows:
            raise ValueError("expanded comparison row limit exceeded")
        for grid in grids:
            case = model["cases"].get(grid)
            case_results.append({"fixture_id": fixture_id, "grid": grid,
                                 "status": case["status"] if case else "case_absent",
                                 "error": case["error"] if case else None,
                                 "produced_rows": len(case["prices"]) if case else 0,
                                 "universe_rows": len(model["universe"]),
                                 "unknown_universe": not model["universe"],
                                 "explicit_requested_rows": len(case["requested"])
                                     if case and case["requested"] is not None else None})
            for key, args in sorted(model["universe"].items()):
                price = case["prices"].get(key) if case else None
                state = "case_absent" if case is None else case["status"]
                if case and case["requested"] is not None and key not in case["requested"]:
                    state = "not_requested"
                elif state == "priced" and price is None:
                    state = "missing_price"
                ref = refs.get(key)
                if ref:
                    matched_refs.add(key)
                reference_price = ref.get("reference_price") if ref else None
                difference = price - reference_price if price is not None and reference_price is not None else None
                if difference is not None and not math.isfinite(difference):
                    raise ValueError("price difference exceeds finite range")
                row = {"fixture_id": fixture_id, "grid": grid, "contract_sha256": digest(args),
                       "arguments": args, "model_status": state, "model_price": price,
                       "model_error": case["error"] if case else None,
                       "reference_status": ref["status"] if ref else "unmatched_reference",
                       "reference_source": ref["reference_source"] if ref else None,
                       "reference_price": reference_price, "difference": difference,
                       "refinement_uncertainty_estimate": ref.get("refinement_uncertainty_estimate") if ref else None,
                       "stability_threshold": ref.get("stability_threshold") if ref else None,
                       "reference_reason": ref.get("reason") if ref else None}
                per_grid[grid][(model_key, key)] = row
                fixture_rows[fixture_id][grid].append(row)
                output_rows.append(row)
    grid_summaries = {}
    for grid, rows in per_grid.items():
        cases = [case for case in case_results if case["grid"] == grid]
        grid_summaries[grid] = {
            **summary(list(rows.values())), "numeric_fixture_count": len(cases),
            "case_status_counts": dict(sorted(Counter(case["status"] for case in cases).items())),
            "unknown_universe_cases": sum(case["unknown_universe"] for case in cases)}
    return {"schema_version": 1, "tool_sha256": TOOL_SHA256, "baseline_grid": baseline_grid,
            "reference_count": len(refs), "matched_reference_count": len(matched_refs),
            "unmatched_references": [refs[key] for key in sorted(refs.keys() - matched_refs)],
            "reference_replacements": replacements, "fixtures": fixtures,
            "cases": case_results, "rows": output_rows, "grids": grid_summaries,
            "by_fixture": [{"fixture_id": fixture["fixture_id"], "grid": grid,
                            **summary(fixture_rows[fixture["fixture_id"]][grid])}
                           for fixture in fixtures for grid in grids],
            "paired_against_baseline": {grid: paired_summary(per_grid[baseline_grid], rows)
                                        for grid, rows in per_grid.items() if grid != baseline_grid},
            "scope": {"join": "exact normalized double pricing inputs; no tolerance or fixture-name joins",
                      "stability": "observed_stable is an empirical refinement classification, not proof",
                      "uncertainty": "retained refinement estimates are not rigorous error bounds",
                      "coverage": "all grids use the same numeric-model contract union; missing/refused cases remain",
                      "errors": "source price minus independent reference; stable and unresolved separate",
                      "timing": "no speed claims; only existing numerical artifacts are compared"}}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError(f"nonfinite JSON constant {value}")


def load_document(path, limits):
    path = Path(path).resolve()
    with path.open("rb") as stream:
        content = stream.read(limits.max_file_bytes + 1)
    if len(content) > limits.max_file_bytes:
        raise ValueError("input JSON exceeds byte limit")
    try:
        document = json.loads(content, object_pairs_hook=unique_object, parse_constant=reject_constant)
    except (RecursionError, UnicodeError) as error:
        raise ValueError("invalid JSON encoding or nesting") from error
    stack = [(document, 0)]
    while stack:
        value, depth = stack.pop()
        if depth > 32:
            raise ValueError("JSON nesting exceeds limit")
        if isinstance(value, dict):
            stack.extend((child, depth + 1) for child in value.values())
        elif isinstance(value, list):
            stack.extend((child, depth + 1) for child in value)
        elif isinstance(value, (float, int)) and not isinstance(value, bool):
            finite(value, "JSON number")
    if not isinstance(document, dict):
        raise ValueError("JSON document must be an object")
    return document, {"path": str(path), "sha256": hashlib.sha256(content).hexdigest(),
                      "bytes": len(content), "declared_tool_sha256": document.get("tool_sha256"),
                      "declared_input_sha256": document.get("input_sha256")}


def check_distinct_paths(paths):
    resolved = [Path(path).resolve() for path in paths]
    for i, left in enumerate(resolved):
        for right in resolved[i + 1:]:
            if left == right or (left.exists() and right.exists() and os.path.samefile(left, right)):
                raise ValueError("input/output paths alias, including hardlinks")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--replacement-reference", type=Path)
    parser.add_argument("--baseline-grid", default="default65")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    limits = Limits()
    try:
        paths = [args.input, args.reference, args.output]
        if args.replacement_reference:
            paths.append(args.replacement_reference)
        check_distinct_paths(paths)
        if args.output.exists():
            raise ValueError("output already exists; immutable reports require a new path")
        source, source_receipt = load_document(args.input, limits)
        reference, reference_receipt = load_document(args.reference, limits)
        replacement, replacement_receipt = None, None
        if args.replacement_reference:
            replacement, replacement_receipt = load_document(args.replacement_reference, limits)
        report = compare_documents(source, reference, replacement_reference=replacement,
                                   baseline_grid=args.baseline_grid, limits=limits)
        report["provenance"] = {"input": source_receipt, "reference": reference_receipt,
                                "replacement_reference": replacement_receipt}
        serialized = json.dumps(report, indent=2, allow_nan=False) + "\n"
        with args.output.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(serialized)
            stream.flush()
            os.fsync(stream.fileno())
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(f"american_audit_compare: {error}", file=sys.stderr)
        return 2
    print(f"Compared {len(report['rows'])} rows across {len(report['grids'])} grids; "
          f"{report['matched_reference_count']}/{report['reference_count']} reference keys matched.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
