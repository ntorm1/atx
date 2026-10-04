# Readiness12 precision clarification review

Spec verdict: NEEDS CHANGES for the current appendix byte binding; complete
request/requirement/cutoff/status preservation otherwise PASS. Quality verdict:
NEEDS CHANGES. This metadata-only review approves no producer, source, A0 arm,
D0 result, financial unit or actual release gate.

1. **Important I1 ? refresh-readiness-v12.py:47 and readiness-matrix-v12.json
additional_required_readiness: current appendix SHA binding is dropped.**
Readiness11 bound its actual appendix2 bytes with SHA256
50941216df88ad011ee0747a641a008d21b9a7089cd89f5ba9be14380e26958c.
The new dictionary at47?51 retains that old binding only under
prior_appendix_preserved, but supplies no SHA256 for the new appendix3 path.
The independently checked source request SHA is not the appendix artifact SHA.
Thus current appendix3 may change while readiness12 remains unchanged; its
23 IDs alone cannot authenticate the binding interpretation/content actually
reviewed. Preserve the current artifact byte-pin contract by hashing the exact
serialized appendix3 bytes and binding that SHA in the main metadata before
publication, with the original outputs/history retained. No data/test run is
needed for this correction.

Checked invariants

The full v2->v3 request diff has exactly three changed passages: seal counts
versus moved links; per-module raw/LF hashes plus one git_head; and missing
producing-code identity for Stage I/backfill/name. Source v3 is59756 bytes with
SHA8f9916d59eb7f3b6f5c7b72768c3e4246c0d189dfbd42344330b57689f1d3321.
Clarification ruling and goal8.2 preserve full D0/row differentials, B1=A and
B12=B2 rehearsal reuse, diagnostic-only S4 and all original source guards. The
saved complete goal snapshot equals the actual attachment byte-for-byte after
text decoding. Neither labels/input hashes nor a named commit are asserted to
prove the missing producing code.

Canonical complete original46 records match11->12 exactly; complete23 appendix
records match2->3 exactly. Main changed keys are only schema/version/supersedes/
revision_reason/review/prior_review_record_preserved/additional_required_readiness.
Appendix changed keys are only schema/version/supersedes/interpretation/source/
binding_interpretation_clarifications. Original cutoffs, release statuses, trial
bars and every observation/checkpoint remain unchanged. Main is
NOT_PRODUCTION_READY; appendix is NOT_READY. Previous source/review/appendix
records remain nested/preserved. No threshold, cap, source proof or result
status was promoted.

Generator prechecks three bounded metadata input identities, copies complete
records, refuses existing destinations, rechecks held inputs before publication
and writes exclusive new outputs. Root refresh-readiness-v12 receipt is exact
completed0/1s,26206208B peak/no cap; stdout METADATA_CLARIFICATION_ONLY reports
46/23 preserved and gate_promotionFalse; stderr empty. No reviewer rerun or
fixture/data/source scan/HTTP/index/cleanup action occurred.

Covering read-only commands/results

- Full generator/ruling/goal snapshot tail and JSON headings: independent
PowerShell reads completed; the initial rg --files log search returned no
ignored log matches (exit1), not a product failure.
- rg --files --hidden --no-ignore logs identified refresh-readiness-v12 files;
exact request S4/D0/provenance passages read, exit0/1.66s.
- Python stdlib difflib comparison of full v2/v3 snapshots, canonical JSON
46/23 equality, exact changed-key sets, current/prior appendix bindings and
bounded source SHA checks; named guard stdout/stderr/receipt reads: exit0/1.42s.
- Named generator47?51 source lines plus actual/saved goal comparison: exit0/1.66s.

Review is static and metadata-only. Product integration R26 repair2/5 and all
actual source/statistical/consumer acceptance remain pending.
