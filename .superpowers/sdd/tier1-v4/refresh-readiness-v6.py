"""Append verified progress to v5 without changing any acceptance contract."""
import copy
import json
from pathlib import Path

BASE = Path(__file__).resolve().parent
original = json.loads((BASE / "readiness-matrix-v5.json").read_text(encoding="utf-8"))
result = copy.deepcopy(original)
result.update(schema="atx.tier1-v4.readiness/v6", version=6, client_date="2026-10-04")
result["checkpoint"] = {
    "controller_commit": "8c53f70b",
    "acceptance_status": "NOT_PRODUCTION_READY",
    "qualification": "Existing acceptance contracts and statuses are unchanged. Completed inventory traversal and one independently checked native financial unit are operational progress, not source-economic or release acceptance.",
    "universe_completed_years": [2018, 2019, 2020, 2021, 2022],
    "universe_final_publication": False,
    "fundamental_extract_batches_admitted": 85,
    "financial_batches_admitted": 1,
    "financial_batches_independently_checked": [0],
    "financial_retained_native_admission_files": [0, 1, 2],
    "financial_additional_admissions_pending_current_replay": [1, 2],
    "financial_final_publication": False,
    "submissions_inventory_main_members": 985667,
    "submissions_inventory_unique_cik_accessions": 278228,
    "submissions_inventory_terminal_exhaustion": True,
    "submissions_inventory_open_oversize_exceptions": [19617, 1665650],
    "submissions_source_and_clocks_qualified": False,
    "gold_publication": False,
    "golden_panel_publication": False,
    "actual_predictive_measurements": False,
    "consumer_acceptance": False,
    "actual_data_jobs": "HELD_AFTER_CONTINUATION_HOST_LOW_COMMIT_STOP; NO_ROOT_JOB_LIVE; NO_ADDITIONAL_UNIT_COUNTED",
    "engineering": "Task15 repair1 dab239f5 independently approved; actual source/coverage remains open. Task17 R26 repair2, including helper75efb231, remains in implementation and awaits combined independent review.",
    "sec_http": "HELD_PENDING_CLOSED_SHARED_ALL_ATTEMPT_ACCOUNTING",
}
entries = [
    (["D01", "D06", "O01", "O03", "O04"], "t17-interruption-inspection-v4.json",
     "Continuation stopped for host low commit after573.5s, peak617504768bytes, no cap. Two-pass read-only inspection retained native admission files0,1,2; only batch0 has the prior independent root audit. Batches1/2 require current full native source/typed replay before additional admission credit. No final manifest/pointer. Dead-owner lock10544 remains preserved; heavy work held."),
    (["D01", "D06", "D07", "C03", "C04", "O01", "O03", "O04"],
     "t17-financial-progress-audit-v2.json",
     "Native batch0 completed full independent typed replay before admission. Root independently checked the complete frozen sources, runtime proof, snapshot, receipt, all four payloads/footers and before/after identities. Native192.3s/583569408peak bytes; audit26.0s/151269376bytes. One of85 units is admitted; no final financial publication or original source-economic/coverage gate passes."),
    (["C11", "C12", "O01", "O03"], "task11-submissions-progress-v4.json",
     "All99 bounded partitions reach independently enumerated archive terminal exhaustion:985667main,5251history traversed,278228distinctCIK-accessions,0duplicates. Audit16.0s/84922368bytes authenticates source/index/code/output/receipts twice. Two oversize main files remain excluded; directory contains5374history files. Full history/exception, body, public-clock and ownership qualification remain open."),
    (["C16", "D01", "O03", "O04"], "review-T15-text-native-r1-findings.md",
     "Task15 repair1 dab239f5 independently passes scoped spec/quality review, all seven findings addressed. Eleven distinct focused checks support lineage/cohort, exact activation, bounded dependency reads, original accession/owner clocks, PIT identity and working-set repairs. Original actual95%strict peers/90%text and complete yearly source populations remain unmeasured."),
    (["O01"], "logs/host-headroom-window-v4.json",
     "Original five-minute31sample scheduling condition passed: minimum sampled commit3.5228GiB/physical2.2586GiB; continuous guard separately observed commit2.9519GiB. This enabled the successful native pilot under unchanged0.8GiB/heavy/40GiBdisk guards. No source/product acceptance follows from scheduling evidence."),
    (["C10", "C11", "C12", "C16", "O01"], "sec-command-metadata-audit-v1.json",
     "Small named execution-metadata audit read1402173bytes including recheck across1551entries. Two command matches are pytest/ruff, not HTTP children; the retained text loop script lacks complete execution intervals. No historical all-attempt closure or new SEC budget grant follows."),
    (["I01", "I02", "I03", "I04", "I05", "O05", "O06"], "task-26-delivery-integration-ruling.md",
     "R26 routes eight golden-delivery and four native/catalog findings to existing Task17 repair2/5. Corrections and complete family mappings are still active; helper75efb231 is committed but awaits independent combined review. No actual gold panel, separate labels, predictive measurements or atx-impl acceptance is supplied by this engineering checkpoint."),
]
supporting = {
    "t17-interruption-inspection-v4.json": ["logs/t17-runtime-b0001-b0004-v1.receipt.json", "logs/t17-interruption-inspection-v4.receipt.json"],
    "t17-financial-progress-audit-v2.json": ["logs/t17-financial-progress-audit-v2.receipt.json"],
    "task11-submissions-progress-v4.json": ["logs/task11-submissions-progress-v4.receipt.json"],
    "logs/host-headroom-window-v4.json": ["logs/host-headroom-observe-v4.receipt.json"],
}
by_id = {row["id"]: row for row in result["requirements"]}
assert len(by_id) == 46
for identifiers, relative, observation in entries:
    assert (BASE / relative).is_file(), relative
    for identifier in identifiers:
        observation_record = {
            "artifact": f".superpowers/sdd/tier1-v4/{relative}", "result": observation,
            "checkpoint_version": 6, "gate_promotion": False,
        }
        if relative in supporting:
            assert all((BASE / path).is_file() for path in supporting[relative])
            observation_record["supporting_artifacts"] = [
                f".superpowers/sdd/tier1-v4/{path}" for path in supporting[relative]
            ]
        by_id[identifier].setdefault("observations", []).append(observation_record)
for before, after in zip(original["requirements"], result["requirements"], strict=True):
    expected = copy.deepcopy(after)
    old = before.get("observations")
    if old is None:
        expected.pop("observations", None)
    else:
        expected["observations"] = expected.get("observations", [])[:len(old)]
    assert expected == before, before["id"]
with (BASE / "readiness-matrix-v6.json").open("x", encoding="utf-8", newline="\n") as sink:
    json.dump(result, sink, indent=2, ensure_ascii=False)
    sink.write("\n")
print("Created v6; all46 original contracts/statuses and all prior observations preserved; no gate promoted.")
