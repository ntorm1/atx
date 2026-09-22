"""Independent numerical comparator for the iteration-12 security-transition checkpoint.

This script was written WITHOUT reading any native C++ source. Its only inputs are
(1) the exact Fraction/Decimal oracle document
``build-equity/audits/iteration12-transition-oracle.json`` and (2) a textual
description of the native measurement line format. Every expected number is
re-derived here from first principles in exact rational arithmetic.

Three independent comparisons are performed for each native case:

  (a) native ``inputs`` versus the oracle's inputs (exact binary64 equality, or
      within one ulp of the oracle's exact rational);
  (b) native ``values`` versus values recomputed in exact ``Fraction`` arithmetic
      from the *native* inputs (closes the loop on the native's own inputs);
  (c) native ``values`` versus the oracle's exact rational expected values.

Arithmetic convention (re-derived, and cross-checked against the oracle document):

    old_share_equivalents        = old_units * old_tri / old_raw
    entitled_share_equivalents   = entitled_pre_split_share_equivalents
    successor_shares_delivered   = old_share_equivalents * successor_shares_per_old_share
    successor_units_added        = successor_shares_delivered * successor_raw / successor_tri
    successor_units_after        = successor_units_before + successor_units_added
    old_units_after              = 0                       (exact)
    settled_cash_after_transition= settled_cash_before      (exact)
    pending_signed_cash_claim    = entitled_share_equivalents * cash_per_old_share
    transition_value_bridge      = incremental_successor_value + claim - removed_old_marked_value
    payment: cash += claim, claim -> 0, NAV unchanged.

Input-domain note: the native ``inputs`` block carries ten keys and does NOT carry
``unrelated_signed_claims``; this comparator therefore assumes that leg is exactly
zero, matching every oracle scenario (all 14 carry ``unrelated_signed_claims = 0/1``).

Rational-construction note (deliberate): native inputs are printed from binary64
with ``max_digits10`` in the classic locale, so they round-trip exactly. They are
converted with ``Fraction(float_value)`` -- i.e. the *exact* binary64 value, not a
re-reading of the decimal literal as a decimal rational. This is what the native
implementation actually computed with. The oracle's ``exact_rationals`` remain the
ground truth for comparison (c); the oracle's ``nearest_binary64_references`` are
the ground truth for comparison (a).

Qualification: see ``QUALIFICATION`` below. This is a synthetic accounting identity
check only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from fractions import Fraction
from pathlib import Path

SCHEMA = "atx-iteration12-native-comparison-v1"
MEASUREMENT_SCHEMA = "atx-security-transition-measurement-v1"
MARKER = "SECURITY_TRANSITION_MEASUREMENT "

ROOT = Path("C:/atx/.worktrees/equity-platform")
DEFAULT_ORACLE = ROOT / "build-equity/audits/iteration12-transition-oracle.json"
DEFAULT_LOGS = [
    ROOT / "build-equity/audits/iteration12-tests.log",
    ROOT / "build-equity/audits/iteration12-wrapper-test-output.log",
]
DEFAULT_OUT = ROOT / "build-equity/audits/iteration12-native-comparison.json"

QUALIFICATION = (
    "Synthetic accounting identity check against an exact oracle; not evidence of "
    "real corporate-action admission, replay integration, physical delivery, loan "
    "discharge, or investment performance."
)

# Exactly these native case ids must appear, each exactly once (after de-duplication
# of byte-identical decoded objects).
REQUIRED_CASE_IDS = (
    "long-canonical",
    "short-canonical",
    "long-old_scale_8",
    "long-successor_scale_4",
    "long-independent_scales_8_4",
)

INPUT_KEYS = (
    "old_units",
    "old_raw",
    "old_tri",
    "successor_units_before",
    "successor_raw",
    "successor_tri",
    "successor_shares_per_old_share",
    "cash_per_old_share",
    "entitled_pre_split_share_equivalents",
    "settled_cash_before",
)

VALUE_KEYS = (
    "old_share_equivalents",
    "entitled_share_equivalents",
    "old_units_after",
    "old_units_delta",
    "removed_old_marked_value",
    "successor_share_equivalents_delivered",
    "successor_units_added",
    "successor_units_after",
    "successor_value_before",
    "successor_value_after",
    "incremental_successor_value",
    "pending_signed_cash_claim",
    "gross_receivable",
    "gross_payable",
    "settled_cash_after_transition",
    "transition_settled_cash_delta",
    "nav_before",
    "nav_after_transition",
    "transition_value_bridge",
    "payment_cash_delta",
    "payment_claim_delta",
    "settled_cash_after_payment",
    "claim_after_full_payment",
    "nav_after_payment",
    "payment_nav_change",
)

RESIDUAL_KEYS = (
    "representation_residual",
    "transition_accounting_residual",
    "payment_accounting_residual",
)

# --------------------------------------------------------------------------- #
# Numerical bound policy
# --------------------------------------------------------------------------- #
# Every quantity gets a *leg-derived* bound:
#
#     bound(key) = 64 * 2**-52 * max( max(|actual|, |expected|), sum_i |leg_i| )
#
# The first term is the plain relative bound demanded for nonzero quantities with
# no absolute floor. The second term is the cancellation-aware bound: the legs are
# the terms whose signed sum defines the quantity, so their absolute sum is the
# magnitude at which rounding is actually committed before cancellation. For a key
# produced by a single product/quotient (no cancellation) the leg sum equals the
# quantity itself and the bound collapses exactly to the plain relative bound. For
# a key that is a difference of larger terms -- including exactly-zero quantities
# and the transition bridge (0.2946 out of ~120 of gross legs) -- the leg sum is
# what governs. No arbitrary relative test is ever made against zero.
#
# Leg names resolve against a namespace containing the exact recomputed values
# (VALUE_KEYS) plus the exact native inputs prefixed with "in_".
LEG_SETS = {
    # single-term products / quotients: leg sum == |value| -> plain relative bound
    "old_share_equivalents": ("old_share_equivalents",),
    "entitled_share_equivalents": ("entitled_share_equivalents",),
    "removed_old_marked_value": ("removed_old_marked_value",),
    "successor_share_equivalents_delivered": ("successor_share_equivalents_delivered",),
    "successor_units_added": ("successor_units_added",),
    "successor_value_before": ("successor_value_before",),
    "pending_signed_cash_claim": ("pending_signed_cash_claim",),
    # sign selectors off the single claim leg
    "gross_receivable": ("pending_signed_cash_claim",),
    "gross_payable": ("pending_signed_cash_claim",),
    "payment_cash_delta": ("pending_signed_cash_claim",),
    "payment_claim_delta": ("pending_signed_cash_claim",),
    # exact set-to-zero / pass-through: also checked for exact equality below
    "old_units_after": ("in_old_units",),
    "old_units_delta": ("in_old_units",),
    "settled_cash_after_transition": ("in_settled_cash_before",),
    "transition_settled_cash_delta": ("in_settled_cash_before", "settled_cash_after_transition"),
    # cancellation-prone sums and differences
    "successor_units_after": ("in_successor_units_before", "successor_units_added"),
    "successor_value_after": ("successor_value_before", "incremental_successor_value"),
    "incremental_successor_value": ("successor_value_after", "successor_value_before"),
    "nav_before": ("in_settled_cash_before", "removed_old_marked_value", "successor_value_before"),
    "nav_after_transition": (
        "in_settled_cash_before",
        "successor_value_after",
        "pending_signed_cash_claim",
    ),
    "transition_value_bridge": (
        "incremental_successor_value",
        "pending_signed_cash_claim",
        "removed_old_marked_value",
    ),
    "settled_cash_after_payment": ("in_settled_cash_before", "pending_signed_cash_claim"),
    "claim_after_full_payment": ("pending_signed_cash_claim", "pending_signed_cash_claim"),
    "nav_after_payment": ("settled_cash_after_payment", "successor_value_after"),
    "payment_nav_change": ("nav_after_payment", "nav_after_transition"),
}

# Residual leg sets. The native residual definitions were NOT read (no C++ was
# opened); these leg sets are declared from the accounting identities the residuals
# are named for, and are deliberately the full gross magnitudes that feed those
# identities.
RESIDUAL_LEG_SETS = {
    "representation_residual": (
        "removed_old_marked_value",
        "successor_value_before",
        "successor_value_after",
        "incremental_successor_value",
    ),
    "transition_accounting_residual": (
        "nav_before",
        "nav_after_transition",
        "transition_value_bridge",
    ),
    "payment_accounting_residual": (
        "nav_after_transition",
        "nav_after_payment",
        "payment_cash_delta",
        "payment_claim_delta",
    ),
}

# Identity checks and their leg sets.
IDENTITY_LEG_SETS = {
    "nav_bridge_identity": ("nav_after_transition", "nav_before", "transition_value_bridge"),
    "payment_nav_invariance": ("nav_after_payment", "nav_after_transition"),
}

ULP_MULTIPLIER = 64
EPS = Fraction(1, 2 ** 52)
BOUND_SCALE = Fraction(ULP_MULTIPLIER) * EPS


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #
def sha256_and_len(path: Path):
    data = path.read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def parse_exact(text: str) -> Fraction:
    """Parse an oracle 'n/d' exact rational."""
    return Fraction(text)


def to_fraction_from_binary64(value) -> Fraction:
    """Exact rational value of a binary64 that the native printed."""
    return Fraction(float(value))


def f2f(value: Fraction) -> float:
    try:
        return float(value)
    except (OverflowError, ValueError):
        return math.inf if value > 0 else -math.inf


def exact_text(value: Fraction) -> str:
    return f"{value.numerator}/{value.denominator}"


def recompute(inputs: dict) -> dict:
    """Recompute every expected value in exact Fraction arithmetic from native inputs."""
    ou = inputs["old_units"]
    oraw = inputs["old_raw"]
    otri = inputs["old_tri"]
    sub = inputs["successor_units_before"]
    sraw = inputs["successor_raw"]
    stri = inputs["successor_tri"]
    ratio = inputs["successor_shares_per_old_share"]
    crate = inputs["cash_per_old_share"]
    entitled = inputs["entitled_pre_split_share_equivalents"]
    cash = inputs["settled_cash_before"]
    unrelated = Fraction(0)  # not carried in the native inputs block; zero in all scenarios

    old_shares = ou * otri / oraw
    delivered = old_shares * ratio
    added_units = delivered * sraw / stri
    after_units = sub + added_units

    old_value = ou * otri
    before_value = sub * stri
    after_value = after_units * stri
    added_value = after_value - before_value

    claim = entitled * crate

    nav_before = cash + old_value + before_value + unrelated
    nav_after = cash + after_value + unrelated + claim
    bridge = added_value + claim - old_value

    cash_after_payment = cash + claim
    nav_after_payment = cash_after_payment + after_value + unrelated + Fraction(0)

    return {
        "old_share_equivalents": old_shares,
        "entitled_share_equivalents": entitled,
        "old_units_after": Fraction(0),
        "old_units_delta": -ou,
        "removed_old_marked_value": old_value,
        "successor_share_equivalents_delivered": delivered,
        "successor_units_added": added_units,
        "successor_units_after": after_units,
        "successor_value_before": before_value,
        "successor_value_after": after_value,
        "incremental_successor_value": added_value,
        "pending_signed_cash_claim": claim,
        "gross_receivable": max(Fraction(0), claim),
        "gross_payable": max(Fraction(0), -claim),
        "settled_cash_after_transition": cash,
        "transition_settled_cash_delta": Fraction(0),
        "nav_before": nav_before,
        "nav_after_transition": nav_after,
        "transition_value_bridge": bridge,
        "payment_cash_delta": claim,
        "payment_claim_delta": -claim,
        "settled_cash_after_payment": cash_after_payment,
        "claim_after_full_payment": Fraction(0),
        "nav_after_payment": nav_after_payment,
        "payment_nav_change": nav_after_payment - nav_after,
    }


def leg_namespace(inputs: dict, recomputed: dict) -> dict:
    ns = {f"in_{k}": v for k, v in inputs.items()}
    ns.update(recomputed)
    return ns


def leg_sum(names, ns) -> Fraction:
    total = Fraction(0)
    for name in names:
        total += abs(ns[name])
    return total


def bound_for(names, ns, actual: Fraction, expected: Fraction) -> Fraction:
    relative_term = max(abs(actual), abs(expected))
    legs_term = leg_sum(names, ns)
    return BOUND_SCALE * max(relative_term, legs_term)


# --------------------------------------------------------------------------- #
# Log scanning
# --------------------------------------------------------------------------- #
def scan_logs(log_paths):
    """Return (records, evidence, errors).

    records: case_id -> {"object":..., "line":..., "line_sha256":..., "sources":[...]}
    """
    decoder = json.JSONDecoder()
    records = {}
    evidence = []
    errors = []

    for path in log_paths:
        entry = {"path": str(path).replace("\\", "/"), "exists": path.exists()}
        if not path.exists():
            entry["status"] = "missing"
            evidence.append(entry)
            errors.append(f"log file not found: {entry['path']}")
            continue
        sha, size = sha256_and_len(path)
        entry.update({"status": "scanned", "sha256": sha, "bytes": size})
        text = path.read_text(encoding="utf-8", errors="replace")
        matched = 0
        for lineno, raw_line in enumerate(text.splitlines(), start=1):
            idx = raw_line.find(MARKER)
            if idx < 0:
                continue
            matched += 1
            tail = raw_line[idx + len(MARKER):].lstrip()
            try:
                obj, _end = decoder.raw_decode(tail)
            except ValueError as exc:
                errors.append(f"{entry['path']}:{lineno}: undecodable measurement JSON: {exc}")
                continue
            if not isinstance(obj, dict):
                errors.append(f"{entry['path']}:{lineno}: measurement payload is not a JSON object")
                continue
            schema = obj.get("schema")
            if schema != MEASUREMENT_SCHEMA:
                errors.append(
                    f"{entry['path']}:{lineno}: unexpected measurement schema {schema!r} "
                    f"(want {MEASUREMENT_SCHEMA!r})"
                )
                continue
            case_id = obj.get("case_id")
            if not isinstance(case_id, str) or not case_id:
                errors.append(f"{entry['path']}:{lineno}: measurement has no usable case_id")
                continue
            canonical = json.dumps(obj, sort_keys=True, separators=(",", ":"))
            source = f"{entry['path']}:{lineno}"
            if case_id in records:
                prior = records[case_id]
                if prior["canonical"] != canonical:
                    errors.append(
                        f"conflicting duplicate measurement for case_id {case_id!r}: "
                        f"{prior['sources'][0]} vs {source}"
                    )
                    prior["conflicting"] = True
                else:
                    prior["sources"].append(source)
                    prior["line_sha256_all"].append(
                        hashlib.sha256(raw_line.encode("utf-8")).hexdigest()
                    )
                continue
            records[case_id] = {
                "object": obj,
                "canonical": canonical,
                "line": raw_line,
                "line_sha256_all": [hashlib.sha256(raw_line.encode("utf-8")).hexdigest()],
                "sources": [source],
                "conflicting": False,
            }
        entry["marker_lines"] = matched
        evidence.append(entry)

    return records, evidence, errors


# --------------------------------------------------------------------------- #
# Per-case comparison
# --------------------------------------------------------------------------- #
def compare_case(case_id, record, scenario):
    failures = []
    notes = []
    obj = record["object"]

    native_inputs_raw = obj.get("inputs")
    native_values_raw = obj.get("values")
    native_residuals_raw = obj.get("residuals")
    for name, blob in (
        ("inputs", native_inputs_raw),
        ("values", native_values_raw),
        ("residuals", native_residuals_raw),
    ):
        if not isinstance(blob, dict):
            failures.append(f"{case_id}: measurement block {name!r} missing or not an object")
    if failures:
        return {"case_id": case_id, "passed": False, "failures": failures}, failures

    oracle_inputs_exact = {
        k: parse_exact(v) for k, v in scenario["inputs"]["exact_rationals"].items()
    }
    oracle_inputs_b64 = scenario["inputs"]["nearest_binary64_references"]
    oracle_values_exact = {
        k: parse_exact(v) for k, v in scenario["expected"]["exact_rationals"].items()
    }

    # ---- (a) native inputs vs oracle inputs -------------------------------- #
    input_rows = []
    native_inputs = {}
    for key in INPUT_KEYS:
        if key not in native_inputs_raw:
            failures.append(f"{case_id}: native inputs missing key {key!r}")
            continue
        raw = native_inputs_raw[key]
        if raw is None or not isinstance(raw, (int, float)) or isinstance(raw, bool):
            failures.append(f"{case_id}: native input {key!r} is non-finite or non-numeric ({raw!r})")
            continue
        value = float(raw)
        if not math.isfinite(value):
            failures.append(f"{case_id}: native input {key!r} is non-finite ({raw!r})")
            continue
        native_inputs[key] = to_fraction_from_binary64(value)
        oracle_exact = oracle_inputs_exact.get(key)
        oracle_ref = oracle_inputs_b64.get(key)
        if oracle_exact is None or oracle_ref is None:
            failures.append(f"{case_id}: oracle scenario lacks input key {key!r}")
            continue
        status = None
        if value == float(oracle_ref):
            status = "exact_binary64_match"
        else:
            delta = abs(native_inputs[key] - oracle_exact)
            allowed = Fraction(math.ulp(float(oracle_ref)) if oracle_ref != 0 else math.ulp(0.0))
            if delta <= allowed:
                status = "within_1_ulp_of_oracle_exact"
                notes.append(f"{case_id}: input {key!r} differs from oracle reference but within 1 ulp")
            else:
                status = "MISMATCH"
                failures.append(
                    f"{case_id}: input {key!r} native={value!r} oracle_b64={oracle_ref!r} "
                    f"oracle_exact={exact_text(oracle_exact)} delta={f2f(delta)!r} > 1 ulp"
                )
        input_rows.append(
            {
                "key": key,
                "native": value,
                "oracle_binary64_reference": oracle_ref,
                "oracle_exact": exact_text(oracle_exact) if oracle_exact is not None else None,
                "status": status,
            }
        )

    extra_inputs = sorted(set(native_inputs_raw) - set(INPUT_KEYS))
    if extra_inputs:
        notes.append(f"{case_id}: native inputs carry additional keys not compared: {extra_inputs}")

    if len(native_inputs) != len(INPUT_KEYS):
        return (
            {
                "case_id": case_id,
                "passed": False,
                "failures": failures,
                "notes": notes,
                "inputs": input_rows,
            },
            failures,
        )

    # ---- exact recomputation from the NATIVE inputs ------------------------ #
    recomputed = recompute(native_inputs)
    ns = leg_namespace(native_inputs, recomputed)

    # ---- (b)/(c) native values ------------------------------------------- #
    value_rows = []
    native_values = {}
    for key in VALUE_KEYS:
        if key not in native_values_raw:
            failures.append(f"{case_id}: native values missing key {key!r}")
            continue
        raw = native_values_raw[key]
        if raw is None:
            failures.append(f"{case_id}: native value {key!r} is non-finite (printed null)")
            continue
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            failures.append(f"{case_id}: native value {key!r} is not numeric ({raw!r})")
            continue
        value = float(raw)
        if not math.isfinite(value):
            failures.append(f"{case_id}: native value {key!r} is non-finite ({raw!r})")
            continue
        native_values[key] = value
        actual = to_fraction_from_binary64(value)
        expected_recomputed = recomputed[key]
        expected_oracle = oracle_values_exact.get(key)
        if expected_oracle is None:
            failures.append(f"{case_id}: oracle scenario lacks expected value {key!r}")
            continue

        legs = LEG_SETS[key]
        bound_b = bound_for(legs, ns, actual, expected_recomputed)
        bound_c = bound_for(legs, ns, actual, expected_oracle)
        diff_b = abs(actual - expected_recomputed)
        diff_c = abs(actual - expected_oracle)
        ok_b = diff_b <= bound_b
        ok_c = diff_c <= bound_c
        if not ok_b:
            failures.append(
                f"{case_id}: value {key!r} vs exact-recompute-from-native-inputs: "
                f"|{value!r} - {f2f(expected_recomputed)!r}| = {f2f(diff_b)!r} > bound {f2f(bound_b)!r} "
                f"(legs {list(legs)})"
            )
        if not ok_c:
            failures.append(
                f"{case_id}: value {key!r} vs oracle exact: "
                f"|{value!r} - {f2f(expected_oracle)!r}| = {f2f(diff_c)!r} > bound {f2f(bound_c)!r} "
                f"(legs {list(legs)})"
            )
        value_rows.append(
            {
                "key": key,
                "native": value,
                "recomputed_exact": exact_text(expected_recomputed),
                "recomputed_float": f2f(expected_recomputed),
                "oracle_exact": exact_text(expected_oracle),
                "oracle_float": f2f(expected_oracle),
                "abs_diff_vs_recomputed": f2f(diff_b),
                "abs_diff_vs_oracle": f2f(diff_c),
                "bound": f2f(bound_b),
                "bound_legs": list(legs),
                "leg_sum": f2f(leg_sum(legs, ns)),
                "passed": bool(ok_b and ok_c),
            }
        )

    extra_values = sorted(set(native_values_raw) - set(VALUE_KEYS))
    if extra_values:
        notes.append(f"{case_id}: native values carry additional keys not compared: {extra_values}")
    oracle_only_value_keys = sorted(set(oracle_values_exact) - set(VALUE_KEYS))
    if oracle_only_value_keys:
        notes.append(
            f"{case_id}: oracle carries expected keys the native format does not emit "
            f"(not compared): {oracle_only_value_keys}"
        )

    # ---- residuals -------------------------------------------------------- #
    residual_rows = []
    for key in RESIDUAL_KEYS:
        if key not in native_residuals_raw:
            failures.append(f"{case_id}: native residuals missing key {key!r}")
            continue
        raw = native_residuals_raw[key]
        if raw is None:
            failures.append(f"{case_id}: native residual {key!r} is non-finite (printed null)")
            continue
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            failures.append(f"{case_id}: native residual {key!r} is not numeric ({raw!r})")
            continue
        value = float(raw)
        if not math.isfinite(value):
            failures.append(f"{case_id}: native residual {key!r} is non-finite ({raw!r})")
            continue
        legs = RESIDUAL_LEG_SETS[key]
        bound = BOUND_SCALE * leg_sum(legs, ns)
        magnitude = abs(to_fraction_from_binary64(value))
        ok = magnitude <= bound
        if not ok:
            failures.append(
                f"{case_id}: residual {key!r} = {value!r}, |residual| > leg-derived bound "
                f"{f2f(bound)!r} (legs {list(legs)})"
            )
        residual_rows.append(
            {
                "key": key,
                "native": value,
                "bound": f2f(bound),
                "bound_legs": list(legs),
                "leg_sum": f2f(leg_sum(legs, ns)),
                "passed": bool(ok),
            }
        )

    # ---- identity checks -------------------------------------------------- #
    identity_rows = []

    def identity(name, ok, detail, extra=None):
        row = {"check": name, "passed": bool(ok), "detail": detail}
        if extra:
            row.update(extra)
        identity_rows.append(row)
        if not ok:
            failures.append(f"{case_id}: identity {name!r} failed: {detail}")

    have = lambda k: k in native_values  # noqa: E731

    if have("nav_after_transition") and have("nav_before") and have("transition_value_bridge"):
        lhs = to_fraction_from_binary64(native_values["nav_after_transition"]) - to_fraction_from_binary64(
            native_values["nav_before"]
        )
        rhs = to_fraction_from_binary64(native_values["transition_value_bridge"])
        legs = IDENTITY_LEG_SETS["nav_bridge_identity"]
        bound = BOUND_SCALE * leg_sum(legs, ns)
        diff = abs(lhs - rhs)
        identity(
            "nav_after_transition_minus_nav_before_equals_bridge",
            diff <= bound,
            f"|{f2f(lhs)!r} - {native_values['transition_value_bridge']!r}| = {f2f(diff)!r} "
            f"vs bound {f2f(bound)!r}",
            {"bound_legs": list(legs), "abs_diff": f2f(diff), "bound": f2f(bound)},
        )
    else:
        identity("nav_after_transition_minus_nav_before_equals_bridge", False, "inputs to identity missing")

    if have("payment_nav_change"):
        legs = IDENTITY_LEG_SETS["payment_nav_invariance"]
        bound = BOUND_SCALE * leg_sum(legs, ns)
        magnitude = abs(to_fraction_from_binary64(native_values["payment_nav_change"]))
        identity(
            "payment_nav_change_within_leg_bound_of_zero",
            magnitude <= bound,
            f"|{native_values['payment_nav_change']!r}| = {f2f(magnitude)!r} vs bound {f2f(bound)!r}",
            {"bound_legs": list(legs), "bound": f2f(bound)},
        )
    else:
        identity("payment_nav_change_within_leg_bound_of_zero", False, "payment_nav_change missing")

    if have("gross_receivable") and have("gross_payable") and have("pending_signed_cash_claim"):
        claim = native_values["pending_signed_cash_claim"]
        recv = native_values["gross_receivable"]
        pay = native_values["gross_payable"]
        claim_legs = ("pending_signed_cash_claim",)
        cbound = BOUND_SCALE * leg_sum(claim_legs, ns)
        if claim > 0:
            ok = (
                abs(to_fraction_from_binary64(recv) - to_fraction_from_binary64(claim)) <= cbound
                and pay == 0.0
            )
            detail = f"claim>0: receivable must equal claim and payable must be exactly 0 (recv={recv!r}, pay={pay!r})"
        elif claim < 0:
            ok = (
                abs(to_fraction_from_binary64(pay) + to_fraction_from_binary64(claim)) <= cbound
                and recv == 0.0
            )
            detail = f"claim<0: payable must equal -claim and receivable must be exactly 0 (recv={recv!r}, pay={pay!r})"
        else:
            ok = recv == 0.0 and pay == 0.0
            detail = f"claim==0: both gross legs must be exactly 0 (recv={recv!r}, pay={pay!r})"
        identity("gross_legs_consistent_with_claim_sign", ok, detail,
                 {"claim": claim, "bound": f2f(cbound), "bound_legs": list(claim_legs)})
    else:
        identity("gross_legs_consistent_with_claim_sign", False, "claim or gross legs missing")

    if have("old_units_after"):
        value = native_values["old_units_after"]
        identity(
            "old_units_after_exactly_zero",
            value == 0.0,
            f"old_units_after = {value!r} (must be exactly 0)",
        )
    else:
        identity("old_units_after_exactly_zero", False, "old_units_after missing")

    if have("settled_cash_after_transition"):
        value = native_values["settled_cash_after_transition"]
        before = float(native_inputs_raw["settled_cash_before"])
        identity(
            "settled_cash_unchanged_by_transition_exactly",
            value == before,
            f"settled_cash_after_transition = {value!r}, settled_cash_before = {before!r} "
            f"(must be bit-identical)",
        )
    else:
        identity("settled_cash_unchanged_by_transition_exactly", False, "settled_cash_after_transition missing")

    if have("transition_settled_cash_delta"):
        value = native_values["transition_settled_cash_delta"]
        identity(
            "transition_settled_cash_delta_exactly_zero",
            value == 0.0,
            f"transition_settled_cash_delta = {value!r} (must be exactly 0)",
        )

    if have("claim_after_full_payment"):
        value = native_values["claim_after_full_payment"]
        identity(
            "claim_after_full_payment_exactly_zero",
            value == 0.0,
            f"claim_after_full_payment = {value!r} (must be exactly 0)",
        )

    result = {
        "case_id": case_id,
        "oracle_scenario_id": scenario["id"],
        "measurement_sources": record["sources"],
        "measurement_line_sha256": record["line_sha256_all"],
        "measurement_line_bytes": len(record["line"].encode("utf-8")),
        "passed": not failures,
        "failures": failures,
        "notes": notes,
        "inputs": input_rows,
        "values": value_rows,
        "residuals": residual_rows,
        "identities": identity_rows,
    }
    return result, failures


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Independent comparator: native security-transition measurements vs exact oracle."
    )
    parser.add_argument("--oracle", default=str(DEFAULT_ORACLE), help="exact oracle JSON")
    parser.add_argument(
        "--logs",
        nargs="+",
        default=[str(p) for p in DEFAULT_LOGS],
        help="log files to scan for measurement lines",
    )
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="comparison JSON to create (must not exist)")
    args = parser.parse_args(argv)

    out_path = Path(args.out)
    if out_path.exists():
        print(
            json.dumps(
                {
                    "schema": SCHEMA,
                    "status": "refused",
                    "reason": "output already exists; refusing to overwrite",
                    "out": str(out_path).replace("\\", "/"),
                },
                separators=(",", ":"),
            )
        )
        return 1

    self_path = Path(__file__).resolve()
    self_sha, self_len = sha256_and_len(self_path)

    oracle_path = Path(args.oracle)
    if not oracle_path.exists():
        print(
            json.dumps(
                {"schema": SCHEMA, "status": "refused", "reason": "oracle not found",
                 "oracle": str(oracle_path).replace("\\", "/")},
                separators=(",", ":"),
            )
        )
        return 1
    oracle_sha, oracle_len = sha256_and_len(oracle_path)
    oracle = json.loads(oracle_path.read_text(encoding="utf-8"))
    scenarios = {s["id"]: s for s in oracle["scenarios"]}

    global_failures = []

    # Convention cross-check: verify the documented arithmetic reproduces every
    # oracle scenario exactly, in rational arithmetic, before touching native data.
    convention_rows = []
    for sid, scenario in sorted(scenarios.items()):
        exact_inputs = {k: parse_exact(v) for k, v in scenario["inputs"]["exact_rationals"].items()}
        derived = recompute({k: exact_inputs[k] for k in INPUT_KEYS})
        expected = {k: parse_exact(v) for k, v in scenario["expected"]["exact_rationals"].items()}
        bad = [k for k in VALUE_KEYS if derived[k] != expected[k]]
        convention_rows.append({"scenario_id": sid, "exact_agreement": not bad, "disagreeing_keys": bad})
        if bad:
            global_failures.append(
                f"convention cross-check failed for oracle scenario {sid!r}: {bad}"
            )
        if exact_inputs.get("unrelated_signed_claims", Fraction(0)) != 0:
            global_failures.append(
                f"oracle scenario {sid!r} carries nonzero unrelated_signed_claims; the native "
                f"inputs block cannot express it"
            )

    log_paths = [Path(p) for p in args.logs]
    records, log_evidence, scan_errors = scan_logs(log_paths)
    global_failures.extend(scan_errors)

    found = set(records)
    required = set(REQUIRED_CASE_IDS)
    missing = sorted(required - found)
    extra = sorted(found - required)
    if missing:
        global_failures.append(f"required native case_ids missing from logs: {missing}")
    if extra:
        global_failures.append(f"unexpected native case_ids present in logs: {extra}")
    for cid, rec in records.items():
        if rec["conflicting"]:
            global_failures.append(f"case_id {cid!r} has conflicting duplicate measurements")

    for cid in REQUIRED_CASE_IDS:
        if cid not in scenarios:
            global_failures.append(f"oracle document has no scenario for required case_id {cid!r}")

    case_results = []
    for cid in REQUIRED_CASE_IDS:
        if cid not in records or cid not in scenarios or records.get(cid, {}).get("conflicting"):
            case_results.append(
                {
                    "case_id": cid,
                    "passed": False,
                    "failures": [
                        f"{cid}: no usable native measurement and/or no matching oracle scenario"
                    ],
                }
            )
            continue
        result, _ = compare_case(cid, records[cid], scenarios[cid])
        case_results.append(result)

    oracle_only = [
        {"id": sid, "status": "not_measured_natively"}
        for sid in (s["id"] for s in oracle["scenarios"])
        if sid not in required
    ]

    all_cases_passed = all(c.get("passed") for c in case_results) and len(case_results) == 5
    passed = all_cases_passed and not global_failures

    document = {
        "schema": SCHEMA,
        "status": "pass" if passed else "fail",
        "qualification": QUALIFICATION,
        "independence": {
            "native_cpp_source_read": False,
            "build_or_test_executed_by_this_script": False,
            "expected_values_rederived_in_exact_rational_arithmetic": True,
            "unrelated_signed_claims_assumed_zero": True,
        },
        "bound_policy": {
            "formula": "64 * 2**-52 * max( max(|actual|,|expected|), sum_i |leg_i| )",
            "ulp_multiplier": ULP_MULTIPLIER,
            "eps": 2.0 ** -52,
            "rationale": (
                "The first term is the plain relative bound with no absolute floor. The second is "
                "the cancellation-aware bound: for a single-term product/quotient the leg sum "
                "equals the quantity and the bound reduces exactly to the relative bound; for a "
                "difference of larger terms (including every exactly-zero quantity and the "
                "transition bridge) the leg sum governs. Zero is never tested relatively."
            ),
            "value_leg_sets": {k: list(v) for k, v in LEG_SETS.items()},
            "residual_leg_sets": {k: list(v) for k, v in RESIDUAL_LEG_SETS.items()},
            "identity_leg_sets": {k: list(v) for k, v in IDENTITY_LEG_SETS.items()},
            "residual_leg_set_provenance": (
                "declared from the named accounting identities; the native residual definitions "
                "were not read (no C++ source was opened)"
            ),
        },
        "evidence": {
            "comparator_script": {
                "path": str(self_path).replace("\\", "/"),
                "sha256": self_sha,
                "bytes": self_len,
            },
            "oracle": {
                "path": str(oracle_path).replace("\\", "/"),
                "sha256": oracle_sha,
                "bytes": oracle_len,
                "schema": oracle.get("schema"),
                "scenario_count": oracle.get("scenario_count"),
            },
            "logs": log_evidence,
        },
        "convention_cross_check": {
            "description": (
                "documented arithmetic re-derived here reproduces every oracle scenario exactly "
                "in Fraction arithmetic"
            ),
            "scenarios": convention_rows,
            "all_exact": all(r["exact_agreement"] for r in convention_rows),
        },
        "required_case_ids": list(REQUIRED_CASE_IDS),
        "native_case_ids_found": sorted(found),
        "global_failures": global_failures,
        "cases": case_results,
        "oracle_only_not_executed_natively": oracle_only,
        "counts": {
            "required": len(REQUIRED_CASE_IDS),
            "passed": sum(1 for c in case_results if c.get("passed")),
            "failed": sum(1 for c in case_results if not c.get("passed")),
            "oracle_only_not_measured_natively": len(oracle_only),
        },
    }

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, indent=2, allow_nan=False)
        stream.write("\n")

    summary = {
        "schema": SCHEMA,
        "status": document["status"],
        "passed_cases": document["counts"]["passed"],
        "required_cases": document["counts"]["required"],
        "oracle_only_not_measured_natively": document["counts"]["oracle_only_not_measured_natively"],
        "global_failure_count": len(global_failures),
        "case_failure_count": sum(len(c.get("failures", [])) for c in case_results),
        "comparator_sha256": self_sha,
        "oracle_sha256": oracle_sha,
        "out": str(out_path).replace("\\", "/"),
        "qualification": QUALIFICATION,
    }
    print(json.dumps(summary, separators=(",", ":")))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
