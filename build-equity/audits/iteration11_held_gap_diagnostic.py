"""Read-only scalar carry/source-audit diagnosis of iteration 11's first held gap.

Uses the already verified proposal NAV anchors and explicit intents. No native
imports, archive scan, source edits, future marks, or unavailable-value estimate.
"""
import hashlib
import importlib.util
import json
import math
from pathlib import Path


ROOT = Path("C:/atx/.worktrees/equity-platform")
AUDIT = ROOT / "build-equity/audits"
RECEIPT = AUDIT / "iteration11-allocation-proposal-verification.json"
RECEIPT_SHA = "ce58228dd002a6f2d0b001f4080281cf727c8da54f1a2a6560ee600700da6be0"
DECODER = Path("C:/atx/build-equity/audits/iteration6-baseline-verify.py")
DECODER_SHA = "9a2052f35d1da6235b0fd0f11ff1222fe7f213a5a64f608cbaffbb9393064ee3"
EVALUATION = Path("C:/atx/data/equity_baseline_training_2013_20260919/evaluation.bin")
CERTIFICATES = Path("C:/atx/data/equity_book_training_2013_observed_close_20260920/allocation_certificates.json")
SOURCE_AUDIT = Path("C:/atx/data/equity_source_reconciliation_2013_20260919/manifest.json")
OUTPUT = AUDIT / "iteration11-held-gap-diagnostic.json"
PINS = {}


def read(path, expected=None):
    data = path.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    if expected is not None and sha != expected:
        raise ValueError(f"snapshot changed: {path}")
    PINS[str(path)] = sha
    return data


def main():
    if OUTPUT.exists():
        raise ValueError("refusing to overwrite diagnostic")
    read(Path(__file__).resolve())
    receipt = json.loads(read(RECEIPT, RECEIPT_SHA))
    stop = receipt["observation_stop"]
    assert stop["kind"] == "missing-held-mark" and stop["security_id"] == "146189"
    source = read(DECODER, DECODER_SHA)
    module = importlib.util.module_from_spec(importlib.util.spec_from_file_location("held_gap_panel_reader", DECODER))
    exec(compile(source, str(DECODER), "exec"), module.__dict__)
    panel = module.Panel(EVALUATION)
    for name, sha in module.INPUT_HASHES.items():
        assert receipt["input_hashes"][name] == sha
        PINS[name] = sha
    native = json.loads(read(CERTIFICATES, receipt["input_hashes"][str(CERTIFICATES)]))
    source_audit = json.loads(read(SOURCE_AUDIT))
    instrument, period = stop["instrument"], stop["period"]
    assert panel.axes["instrument_ids"][instrument] == stop["security_id"]
    quantity, history = 0.0, []
    for certificate in native["decisions"]:
        execution = certificate["execution_period"]
        assert execution < period
        price = float(panel.columns["close"][execution, instrument])
        action = certificate["proposed_intents_canonical_order"][instrument]
        previous = quantity
        if action["action"] == "close":
            quantity = 0.0
        elif action["action"] == "target-weight":
            weight = action["weight"]
            if weight != 0:
                assert math.isfinite(price) and price > 0
                quantity = (weight * certificate["pretrade_nav"]) / price
            else:
                quantity = 0.0
        else:
            assert action["action"] == "hold-current" and quantity.hex() == previous.hex()
        dollars = quantity * price if quantity else 0.0
        history.append({"decision_period": certificate["decision_period"], "execution_period": execution,
            "action": action["action"], "intent_weight_payload": action["weight"],
            "resolved_requested_weight": certificate["proposed_weights_canonical_order"][instrument],
            "validated_pretrade_nav_anchor": certificate["pretrade_nav"],
            "current_tri_mark": price, "carried_tri_units": quantity, "posttrade_marked_dollars": dollars})
    assert quantity != 0 and math.isfinite(quantity)
    last_price = float(panel.columns["close"][period - 1, instrument])
    missing_price = float(panel.columns["close"][period, instrument])
    assert math.isfinite(last_price) and last_price > 0 and (not math.isfinite(missing_price) or missing_price <= 0)
    gaps = [g for g in source_audit["gaps"] if g["required_gap"]["security_id"] == stop["security_id"]]
    current_gap = [g for g in gaps if g["required_gap"]["session_key_ns"] == stop["session_key_ns"]]
    assert len(current_gap) == 1
    groups = [g for g in source_audit["groups"] if g["security_id"] == stop["security_id"]]
    prior = [g for g in groups if g["date"] == "2013-04-30"]
    assert len(prior) == 1 and prior[0]["accepted_matches_qa_v1_original_bytes"] is True
    rows = prior[0]["original_rows"]
    assert len(rows) == 1 and rows[0]["ticker_at_observation"] == "PCS"
    result = {"schema": "atx-iteration11-held-gap-diagnostic-v1", "status": "observed-material-held-gap",
        "verification_receipt_sha256": RECEIPT_SHA,
        "instrument": instrument, "vendor_security_id": stop["security_id"], "archive_ticker_annotation": "PCS",
        "identity_scope": "ticker-to-vendor-ID mapping from archived source row; no external security-ID attestation",
        "failure_period": period, "failure_session_key_ns": stop["session_key_ns"],
        "last_observed_period": period - 1, "last_observed_session_key_ns": str(int(panel.keys[period - 1])),
        "last_observed_tri_mark": last_price, "carried_tri_units": quantity,
        "last_observed_signed_marked_dollars": quantity * last_price,
        "holding_basis": "research total-return-index units, not asserted physical shares",
        "intent_history": history, "source_audit_id": source_audit["audit_id"],
        "source_audit_current_gap": current_gap[0], "source_audit_security_gaps": gaps,
        "source_audit_previous_group": prior[0],
        "source_economic_status": "unverified", "archive_rescanned": False,
        "first_held_gap_independently_reproduced": receipt["native_failure_independently_reproduced"],
        "incomplete_interval_borrow_debited": False, "failure_mark_or_later_values_estimated": False,
        "completed_performance_result": False, "native_or_source_outputs_modified": False,
        "input_hashes": dict(sorted(PINS.items()))}
    for name, sha in PINS.items():
        assert hashlib.sha256(Path(name).read_bytes()).hexdigest() == sha
    with OUTPUT.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k not in
        {"input_hashes", "source_audit_security_gaps", "source_audit_previous_group"}}, indent=2))
    print("diagnostic_sha256", hashlib.sha256(OUTPUT.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
