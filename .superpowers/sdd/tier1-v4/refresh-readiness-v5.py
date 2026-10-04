"""Refresh evidence references; preserve every v4 acceptance contract and status.

This reads existing controller reports/configuration only. It does not inspect
warehouse values, run a measurement, or qualify a source/product.
"""
import copy
import json
from pathlib import Path

BASE = Path(__file__).resolve().parent
original = json.loads((BASE / "readiness-matrix-v4.json").read_text(encoding="utf-8"))
result = copy.deepcopy(original)
result.update(schema="atx.tier1-v4.readiness/v5", version=5, client_date="2026-10-04")
result["checkpoint"] = {
    "controller_commit": "59dd11cb",
    "acceptance_status": "NOT_PRODUCTION_READY",
    "qualification": "Existing acceptance statuses and thresholds remain unchanged. Added evidence separates reviewed engineering, retained bounded units, source extraction admission, actual financial admission, and final release acceptance.",
    "universe_completed_years": [2018, 2019, 2020, 2021, 2022],
    "universe_final_publication": False,
    "fundamental_extract_batches_admitted": 85,
    "financial_batches_admitted": 0,
    "gold_publication": False,
    "golden_panel_publication": False,
    "actual_predictive_measurements": False,
    "consumer_acceptance": False,
    "actual_data_jobs": "HELD_AFTER_HOST_LOW_COMMIT_STOPS",
    "engineering": "Task15 native text/TNIC, Task16 golden delivery and Task17 full catalog/native readers await completion/independent review; their synthetic fixtures are not actual acceptance.",
    "sec_http": "HELD_PENDING_CLOSED_SHARED_ALL_ATTEMPT_ACCOUNTING",
}

entries = [
    (["D01", "D03", "C01", "I01", "O04"], "t16-universe-progress-audit-v2.json",
     "Root independently rechecked complete current inputs and retained 2018-2022 bounded universe units, including two-pass payload/footer/receipt identities. 2023-2026 and final publication remain missing; membership/source-economic/French gates are not promoted."),
    (["D01", "D06", "D07", "D09", "C03", "C04"], "task16-extract-admission-r19-v1.audit.json",
     "All85 source extraction batches independently admitted, covering120,117,237rows/20,390members. Extraction admission is not financial construction, financial-unit admission or any original economic/coverage gate."),
    (["D01", "D06", "O01", "O03", "O04"], "t17-interruption-inspection-v3.json",
     "Financial pilotv3 stopped for host low commit; snapshot unchanged, no batch0admission/finalmanifest/current pointer, retained attempts stable. Exact orphan lock7732 remains preserved for qualified recovery. No retained diagnostic payload is admitted by this report."),
    (["C11", "C12", "O01"], "logs/t11-submissions-inventory-part0001-v1.receipt.json",
     "Small cached13F inventory part0001 stopped_low_commit after24.3s, peak49,414,144bytes, no cap. Only partial output exists; no completed partition/native receipt, so the complete inventory remains open. This is not the intentional exit2 of completed part0000."),
    (["C08", "P05"], "review-T6-T7-r21-findings.md",
     "R21 construction updatecb114231 independently approved. Historical return-based construction may extend beyond2022; all evaluation and complete forward-label windows remain strictly before2023. Actual source/construction/economic gates remain open."),
    (["P05"], "review-T8-native-r2-findings.md",
     "Task8 native replication engineering097f7dbb independently approved after repair2/5. Original actual replication gates remain0PASS/4FAIL-unmeasured; no real statistic is supplied by synthetic tests."),
    (["C10"], "review-T10-native-r1-findings.md",
     "Task10 native guidance312c42f6 independently approved after repair1/5; actual seven gates and manual0/300 remain unmeasured/open. Source-supported comparable EPS surprise follows R22; review does not establish actual coverage/precision."),
    (["C13", "D12"], "review-T12-native-r2-findings.md",
     "Task12 native terminal-event82bad1ae independently approved after repair2/5. Original2267event denominator/80%coverage/200manualsample requirements remain unmeasured. R21 permits individual later source-qualified DLRET values, not later forward windows or evaluation."),
    (["C14", "D12"], "review-T13-native-qualified-r5-findings.md",
     "Task13 native distributionsab81c710 independently approved at exhausted whole-task repair5/5. Actual0PASS/3FAIL, manual0/200 and original completeness/cancellation/carry/vendor source proofs remain open."),
    (["O01", "O03", "O04"], "review-T17-runtime-r1-findings.md",
     "Separate R23 runtime6bab983f/docsd847a7ce independently approved at Task17 repair1/5. Native256MB diagnostic succeeded within0.8GiBguard, but subsequent actual pilots stopped for host headroom; runtime approval is not financial admission or full Task17 catalog completion."),
    (["C10", "C11", "C12", "C16", "O01"], "sec-budget-reconciliation-v2.md",
     "Two-pass audit of20knownledgers verified current text38,433receipts/38,678declaredrequests and source-specific chronology. Baseline interval, hidden/pre-receipt attempts and all-store completeness remain unresolved; no fresh SEC allocation or source admission follows."),
    (["I01", "I02", "I03", "I04", "I05", "O05", "O06"], "gold-readiness-preflight-v2.json",
     "Latest completed six-root readiness inspection: gold/golden_panel/characteristics/char_monthly/labels_goldready absent; partial panel_goldready has no completion manifest. No actual final consumer acceptance exists. Later authors have not published actual data."),
    (["C16"], "task-24-tnic-method-ruling.md",
     "Original reproduced TNIC prior-yearDF25/SIC3-calibrated strict-peer method/full denominator remains binding. Historical SIC and corpus/vintage clocks must be genuine; official external TNIC import is optional. Task15 native implementation/source coverage remain pending."),
]
by_id = {row["id"]: row for row in result["requirements"]}
assert len(by_id) == 46
for identifiers, relative, observation in entries:
    assert (BASE / relative).is_file(), relative
    for identifier in identifiers:
        by_id[identifier].setdefault("observations", []).append({
            "artifact": f".superpowers/sdd/tier1-v4/{relative}",
            "result": observation,
            "checkpoint_version": 5,
            "gate_promotion": False,
        })

# Check the complete original row contracts, including commands, denominators,
# thresholds and every prior observation. This deliberately does not promote
# historical status labels merely because newer engineering code exists.
for before, after in zip(original["requirements"], result["requirements"], strict=True):
    expected = copy.deepcopy(after)
    old_observations = before.get("observations")
    if old_observations is None:
        expected.pop("observations", None)
    else:
        expected["observations"] = expected.get("observations", [])[:len(old_observations)]
    assert expected == before, before["id"]
destination = BASE / "readiness-matrix-v5.json"
with destination.open("x", encoding="utf-8", newline="\n") as handle:
    json.dump(result, handle, indent=2, ensure_ascii=False)
    handle.write("\n")
print("Created readiness-matrix-v5.json; all46 original contracts/statuses preserved; no gate promoted.")
