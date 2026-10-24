"""Pinned, audit-only review of two changed-target FloorVol classifications.

This module is never imported by fitting. The immutable baseline remains the
strict diagnostic; the explicit reviewed contract is a separate proof, not a
general permission to demote rows near a floor.
"""
import hashlib
import json
import math
from pathlib import Path

FROZEN_CONTRACT = "frozen-wave1-v1"
REVIEWED_CONTRACT = "reviewed-feasible-floor-v1"
REVIEW_FIXTURE = Path(__file__).with_name("fixtures") / "spy-reviewed-feasible-floor-v1.json"
REVIEW_CANONICAL_SHA256 = "f25179157477e1c3e2e8f6eaf65ab1388715a290592a191588952fb73baea0db"


def canonical_sha256(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     allow_nan=False).encode()).hexdigest()


def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def within_roundoff(actual, expected, scale=1.0):
    return finite_number(actual) and finite_number(expected) and \
        abs(actual - expected) <= 32 * math.ulp(1.0) * max(scale, abs(actual), abs(expected))


def same_model(actual, expected):
    # Integers include >2**53 contract identifiers and must never be compared
    # after float conversion. Extra diagnostic fields do not change a model.
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(
            key in actual and same_model(actual[key], value) for key, value in expected.items())
    if isinstance(expected, float):
        return within_roundoff(actual, expected)
    return type(actual) is type(expected) and actual == expected


def one(items, predicate):
    matches = [item for item in items if predicate(item)]
    return matches[0] if len(matches) == 1 else None


def prove_transition(item, report, rows, zero_bounds):
    expected_row, expected_query = item["raw_row"], item["expected_query"]
    row_id, strike = expected_row["row_id"], expected_row["strike"]
    row = one(rows, lambda value: value.get("row_id") == row_id)
    # All original fields are frozen, including raw spot/carry, timestamps and
    # unmodified bid/ask. No tolerance applies to original input observations.
    if row != expected_row:
        return "raw_contract_mismatch", None
    entries = [entry for snapshot in report.get("graded_stats", {}).get(
        "acceptance_audit", {}).get("snapshots", []) for entry in snapshot]
    entry = one(entries, lambda value: value.get("chain_id") == row["chain_id"])
    if entry is None or not same_model(entry, item["entry"]):
        return "published_model_or_method_mismatch", None
    carry = one(entry.get("carry_curve", []), lambda value: value.get("strike") == strike)
    if not same_model(carry, item["carry_point"]):
        return "published_carry_mismatch", None
    query = one(entry.get("queries", []), lambda value:
                value.get("strike") == strike and value.get("side") == "call")
    if query is None:
        return "missing_or_duplicate_actual_query", None
    scale = max(1.0, row["spot"], strike)
    for key, expected in expected_query.items():
        actual = query.get(key)
        matches = within_roundoff(actual, expected, scale) if key in ("price", "served_price") \
            else key in query and type(actual) is type(expected) and actual == expected
        if not matches:
            return "actual_query_or_target_mismatch", None
    node = one(entry.get("nodes", []), lambda value: value.get("strike") == strike)
    if node is None or not within_roundoff(node.get("call"), query["price"], scale):
        return "published_node_target_mismatch", None
    market = entry["market"]
    tv, tc = market["variance_years"], market["calendar_years"]
    spot = market["spot"]
    prepaid = spot * math.exp(-carry["continuous_yield"] * tc) - market["dividend_pv"]
    rate = market["rate"] * tc / tv
    yield_rate = -math.log(prepaid / spot) / tv
    zero, ceiling = zero_bounds(spot, strike, tv, rate, yield_rate, "call")
    terminal_floor = max(0.0, spot - strike,
                         prepaid - strike * math.exp(-market["rate"] * tc))
    band = 1e-9 * ceiling + 1e-12
    proof = item["cold_proof"]
    old, target = proof["old_price"], query["price"]
    allowance = 32 * math.ulp(1.0) * scale
    if not (row["ask"] < terminal_floor and abs(old - target) < 1e-7 and
            zero - allowance <= target <= zero + band and old > zero + band and
            proof["old_sigma"] > 0.005 and proof["old_search_iv"] == proof["old_sigma"] and
            proof["old_search_status"] == 0 and proof["old_exercise_class"] == 0 and
            proof["new_search_iv"] == 0.005 and proof["new_search_status"] == 0 and
            within_roundoff(proof["old_cold_price"], old, scale) and
            within_roundoff(proof["zero_vol_supremum"], zero, scale) and
            within_roundoff(proof["cold_price_at_sigma_floor"], zero, scale) and
            within_roundoff(proof["ceiling"], ceiling, scale) and
            within_roundoff(proof["band"], band) and
            within_roundoff(proof["new_price"], target, scale)):
        return "reviewed_floor_proof_mismatch", None
    return None, {"row_id": row_id, "old_target": old, "old_sigma": proof["old_sigma"],
        "new_target": target, "target_shift_usd": target - old,
        "zero_vol_supremum": zero, "zero_vol_band_usd": band,
        "cold_price_at_sigma_floor": proof["cold_price_at_sigma_floor"],
        "raw_ask": row["ask"], "terminal_floor": terminal_floor,
        "roundoff_allowance_usd": allowance, "status": query["status"], "reason": query["reason"],
        "iv_lower": query["iv_lower"], "iv_upper": query["iv_upper"], "method": entry["method"]}


def reviewed_transitions(new_ids, eligibility, report, rows, zero_bounds):
    result = {"accepted_row_ids": [], "proofs": [], "failures": [],
              "fixture_canonical_sha256": REVIEW_CANONICAL_SHA256,
              "roundoff_rule": "32*epsilon*max(1,spot,strike,abs(actual),abs(expected)); "
                               "original raw observations and classification metadata are exact"}
    try:
        fixture = json.loads(REVIEW_FIXTURE.read_text(encoding="utf-8-sig"))
        if canonical_sha256(fixture) != REVIEW_CANONICAL_SHA256:
            raise ValueError("review_fixture_hash_mismatch")
        if eligibility.get("input_sha256") != fixture["input_sha256"] or \
                eligibility.get("minimum_identified_rows") != fixture["minimum_identified_rows"] or \
                canonical_sha256(eligibility.get("identification_baseline")) != \
                fixture["baseline_canonical_sha256"]:
            raise ValueError("review_input_or_baseline_mismatch")
        if not isinstance(report, dict):
            raise ValueError("missing_actual_api_evidence")
        policy = report.get("policy_definition", {}).get("graded", {})
        if policy.get("cold_fit") != "joint_feasible_mid" or \
                policy.get("terminal_floor_recovery") is not True:
            raise ValueError("review_policy_mismatch")
        expected = {item["raw_row"]["row_id"]: item for item in fixture["contracts"]}
        for row_id in new_ids:
            if row_id not in expected:
                result["failures"].append({"row_id": row_id, "kind": "unreviewed_new_price_only"})
                continue
            reason, proof = prove_transition(expected[row_id], report, rows, zero_bounds)
            if reason:
                result["failures"].append({"row_id": row_id, "kind": reason})
            else:
                result["accepted_row_ids"].append(row_id)
                result["proofs"].append(proof)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, ZeroDivisionError) as error:
        result["accepted_row_ids"] = []
        result["proofs"] = []
        result["failures"].append({"kind": "invalid_review_evidence", "detail": str(error)})
    return result
