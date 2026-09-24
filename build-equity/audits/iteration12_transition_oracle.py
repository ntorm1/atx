"""Exact synthetic oracle for signed stock-plus-cash transition and full payment.

Fraction algebra is independent of engine code. Decimal(70) is used only for
human-readable values. No real event admission, archive scan, native execution,
claim settlement evidence, or physical-share assertion is performed here.
"""
from decimal import Decimal, localcontext
from fractions import Fraction as Q
import hashlib
import json
from pathlib import Path


ROOT = Path("C:/atx/.worktrees/equity-platform")
OUTPUT = ROOT / "build-equity/audits/iteration12-transition-oracle.json"
DESIGN = ROOT / "atx-engine/reviews/2026-09-20-iteration12-security-transition-design.md"
PRECISION = 70


def require(condition, message):
    if not condition:
        raise ValueError(message)


def exact(value):
    return f"{value.numerator}/{value.denominator}"


def decimal_text(value):
    with localcontext() as context:
        context.prec = PRECISION
        return str(Decimal(value.numerator) / Decimal(value.denominator))


def pack(values):
    return {"exact_rationals": {k: exact(v) for k, v in values.items()},
            "nearest_binary64_references": {k: float(v) for k, v in values.items()}}


def transition(inputs):
    old_units, old_raw, old_tri = (inputs[k] for k in ("old_units", "old_raw", "old_tri"))
    new_before, new_raw, new_tri = (inputs[k] for k in ("successor_units_before", "successor_raw", "successor_tri"))
    ratio, cash_rate = inputs["successor_shares_per_old_share"], inputs["cash_per_old_share"]
    cash, unrelated_claims = inputs["settled_cash_before"], inputs["unrelated_signed_claims"]
    require(old_raw > 0 and old_tri > 0 and new_raw > 0 and new_tri > 0 and ratio > 0 and cash_rate >= 0,
            "unsupported nonpositive mark/ratio or negative cash rate")
    old_shares = old_units * old_tri / old_raw
    entitled_shares = inputs["entitled_pre_split_share_equivalents"]
    require(entitled_shares == old_shares, "synthetic entitlement and converted quantity disagree")
    successor_shares = old_shares * ratio
    added_units = successor_shares * new_raw / new_tri
    after_units = new_before + added_units
    old_value, before_value, after_value = old_units * old_tri, new_before * new_tri, after_units * new_tri
    added_value = after_value - before_value
    claim = entitled_shares * cash_rate
    before_nav = cash + old_value + before_value + unrelated_claims
    after_nav = cash + after_value + unrelated_claims + claim
    bridge = added_value + claim - old_value
    payment_cash = cash + claim
    payment_remaining_claim = Q(0)
    payment_nav = payment_cash + after_value + unrelated_claims + payment_remaining_claim
    require(added_value == successor_shares * new_raw, "stock value reconciliation failed")
    require(after_nav - before_nav == bridge, "transition NAV bridge failed")
    require(payment_nav == after_nav, "full payment changed NAV")
    require(claim + (-claim) == 0, "nominal payment transfer does not cancel")
    return {
        "old_share_equivalents": old_shares, "entitled_share_equivalents": entitled_shares,
        "old_units_after": Q(0), "old_units_delta": -old_units, "removed_old_marked_value": old_value,
        "successor_share_equivalents_delivered": successor_shares, "successor_units_added": added_units,
        "successor_units_after": after_units, "successor_value_before": before_value,
        "successor_value_after": after_value, "incremental_successor_value": added_value,
        "pending_signed_cash_claim": claim, "gross_receivable": max(Q(0), claim),
        "gross_payable": max(Q(0), -claim), "settled_cash_after_transition": cash,
        "transition_settled_cash_delta": Q(0), "nav_before": before_nav, "nav_after_transition": after_nav,
        "transition_value_bridge": bridge, "exchange_trade_dollars": Q(0), "trade_fee": Q(0),
        "payment_cash_delta": claim, "payment_claim_delta": -claim,
        "settled_cash_after_payment": payment_cash, "claim_after_full_payment": payment_remaining_claim,
        "nav_after_payment": payment_nav, "payment_nav_change": payment_nav - after_nav,
    }


def make_inputs(sign, old_scale=Q(1), new_scale=Q(1), exact_netting=False):
    # Existing successor -0.4 TRI at 60 is -2 research share equivalents.
    # For exact netting, existing -0.6 TRI offsets all 3 delivered equivalents.
    existing = Q(-3, 5) if exact_netting else Q(-2, 5)
    return {"old_units": sign * Q(2) / old_scale, "old_raw": Q(10), "old_tri": Q(30) * old_scale,
        "successor_units_before": sign * existing / new_scale, "successor_raw": Q(12),
        "successor_tri": Q(60) * new_scale, "successor_shares_per_old_share": Q(1, 2),
        "cash_per_old_share": Q(40491, 10000), "entitled_pre_split_share_equivalents": sign * Q(6),
        "settled_cash_before": Q(100), "unrelated_signed_claims": Q(0)}


def run():
    # Explicit hand-derived rational checkpoints bind the algebra to the agreed
    # fixture; no output from an engine helper enters these expected values.
    long = transition(make_inputs(Q(1)))
    short = transition(make_inputs(Q(-1)))
    hand_long = {"old_share_equivalents": Q(6), "successor_share_equivalents_delivered": Q(3),
        "successor_units_added": Q(3, 5), "successor_units_after": Q(1, 5),
        "incremental_successor_value": Q(36), "pending_signed_cash_claim": Q(121473, 5000),
        "nav_before": Q(136), "nav_after_transition": Q(681473, 5000),
        "transition_value_bridge": Q(1473, 5000), "settled_cash_after_payment": Q(621473, 5000)}
    hand_short = {"old_share_equivalents": Q(-6), "successor_share_equivalents_delivered": Q(-3),
        "successor_units_added": Q(-3, 5), "successor_units_after": Q(-1, 5),
        "incremental_successor_value": Q(-36), "pending_signed_cash_claim": Q(-121473, 5000),
        "nav_before": Q(64), "nav_after_transition": Q(318527, 5000),
        "transition_value_bridge": Q(-1473, 5000), "settled_cash_after_payment": Q(378527, 5000)}
    require(all(long[k] == v for k, v in hand_long.items()), "long manual checkpoint failed")
    require(all(short[k] == v for k, v in hand_short.items()), "short manual checkpoint failed")
    scenarios = []
    scale_cases = [("canonical", Q(1), Q(1)), ("old_scale_8", Q(8), Q(1)),
                   ("successor_scale_4", Q(1), Q(4)), ("independent_scales_8_4", Q(8), Q(4)),
                   ("independent_scales_7_11", Q(7), Q(11)),
                   ("fractional_scales_1_8_and_3_2", Q(1, 8), Q(3, 2))]
    unit_fields = {"old_units_after", "old_units_delta", "successor_units_added", "successor_units_after"}
    for sign, label, base in ((Q(1), "long", long), (Q(-1), "short", short)):
        for name, old_scale, new_scale in scale_cases:
            inputs = make_inputs(sign, old_scale, new_scale)
            values = transition(inputs)
            require(all(values[k] == v for k, v in base.items() if k not in unit_fields),
                    f"economic basis invariance failed: {label}/{name}")
            require(values["old_units_delta"] * old_scale == base["old_units_delta"] and
                    values["successor_units_added"] * new_scale == base["successor_units_added"] and
                    values["successor_units_after"] * new_scale == base["successor_units_after"],
                    "inverse quantity rescaling failed")
            scenarios.append({"id": f"{label}-{name}", "old_tri_scale": exact(old_scale),
                "successor_tri_scale": exact(new_scale), "inputs": pack(inputs), "expected": pack(values),
                "economic_invariance_exact": True, "payment_nav_invariance_exact": True})
        inputs = make_inputs(sign, exact_netting=True)
        values = transition(inputs)
        require(values["successor_units_after"] == 0 and values["successor_units_added"] != 0 and
                values["incremental_successor_value"] == sign * Q(36), "legitimate exact netting failed")
        scenarios.append({"id": f"{label}-exact-successor-netting", "inputs": pack(inputs),
            "expected": pack(values), "legitimate_final_zero_preserves_stock_leg_attribution": True,
            "payment_nav_invariance_exact": True})
    wrong_denomination = long["successor_share_equivalents_delivered"] * Q(40491, 10000)
    naive_stock_units = Q(2) * Q(1, 2)
    document = {
        "schema": "atx-iteration12-transition-oracle-v1", "status": "exact-synthetic-math-verified",
        "convention": "synthetic signed reinvested-share-equivalent research accounting",
        "native_comparison_status": "pending-coordinator-build-and-measurement",
        "native_code_imported": False, "real_event_admitted": False, "replay_integrated": False,
        "physical_share_delivery_attested": False, "stock_loan_discharge_attested": False,
        "completed_performance_result": False, "source_panels_or_native_outputs_modified": False,
        "cash_denomination": "per entitled predecessor pre-split share equivalent",
        "component_coverage_assumption": "stock conversion, cash distribution and cross-security continuity explicitly excluded from both certified TRI bases",
        "entitlement_assumption": "synthetic entitled predecessor equivalents equal converted equivalents; not inferred real record ownership",
        "claim_valuation_assumption": "unconditional signed USD pending claim carried at nominal face value; not spendable settled cash",
        "value_bridge_qualification": "incremental successor value plus signed claim minus prior predecessor value; not labelled instantaneous corporate-action profit",
        "full_payment_qualification": "synthetic identified full allocation transfers the exact signed claim to settled cash; operational admission and duplicate guards are native responsibilities",
        "proofs": {
            "old_basis": "(q_old/k_old)*(k_old*I_old)/P_old = q_old*I_old/P_old",
            "successor_basis": "q_added' = s_successor*P_new/(k_new*I_new) = q_added/k_new; existing successor units scale identically",
            "value_invariance": "(q_new_before/k_new + q_added/k_new)*(k_new*I_new) = (q_new_before+q_added)*I_new",
            "payment": "delta_NAV = delta_cash + delta_claim = R-R = 0, including R<0",
            "short_signs": "negative converted and entitled quantities produce negative successor exposure and a payable; payment debits cash",
            "netting": "zero final successor units may be legitimate exact cancellation; actual incremental successor value must still be retained"},
        "canonical_long_decimal": {k: decimal_text(long[k]) for k in
            ("pending_signed_cash_claim", "nav_before", "nav_after_transition", "transition_value_bridge", "settled_cash_after_payment")},
        "canonical_short_decimal": {k: decimal_text(short[k]) for k in
            ("pending_signed_cash_claim", "nav_before", "nav_after_transition", "transition_value_bridge", "settled_cash_after_payment")},
        "negative_controls_not_admitted": {
            "cash_on_successor_quantity": {"incorrect_claim": exact(wrong_denomination),
                "correct_claim": exact(long["pending_signed_cash_claim"]),
                "lost_entitlement": exact(long["pending_signed_cash_claim"] - wrong_denomination)},
            "copy_old_tri_units_times_stock_ratio": {"incorrect_added_tri_units": exact(naive_stock_units),
                "correct_added_tri_units": exact(long["successor_units_added"]),
                "incorrect_extra_stock_value": exact((naive_stock_units - long["successor_units_added"]) * Q(60))}},
        "native_measurement_guidance": "Compare native numeric outputs to the exact rational/nearest binary64 references using the declared floating-point residual policy; do not demand byte equality to exact rational arithmetic.",
        "scenario_count": len(scenarios), "decimal_display_precision": PRECISION, "scenarios": scenarios,
        "downloads_discovery": {"scope": "filename-only rg --files --iglob '*ticker*' C:/Users/natha/Downloads",
            "matches": ["C:/Users/natha/Downloads/tbltickerhistory3_10y.zip"],
            "separate_master_filename_found_within_pattern": False, "archives_opened_or_scanned": False,
            "interpretation": "bounded name-pattern discovery only; no claim that differently named master data do not exist"},
        "input_hashes": {str(Path(__file__).resolve()): hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                         str(DESIGN): hashlib.sha256(DESIGN.read_bytes()).hexdigest()},
    }
    return document


def main():
    require(not OUTPUT.exists(), f"refusing to overwrite oracle: {OUTPUT}")
    result = run()
    with OUTPUT.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"status": result["status"], "scenario_count": result["scenario_count"],
        "canonical_long_decimal": result["canonical_long_decimal"],
        "canonical_short_decimal": result["canonical_short_decimal"],
        "oracle_sha256": hashlib.sha256(OUTPUT.read_bytes()).hexdigest()}, indent=2))


if __name__ == "__main__":
    main()
