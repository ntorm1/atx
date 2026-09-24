# FQ2 whole-branch review repair

Candidate: 727e6b90. Work on feat/tier1-parity in the shared tree.
The independent whole-branch review found an Important defect: the selected
forward-label evidence digest omits is_stitched, although it participates in
validity for delisted observations. Altering it can change counts/spreads while
leaving the stored evidence hash unchanged.

Own only src/atx_db/fundamental_signal_evaluation.py and its focused test file
under atx-db, plus this task's report. Inspect the selected-label query, validity
predicate, canonical digest and version contract. Include every operand that
determines selected-label validity or numeric evaluation in the versioned
evidence. Preserve deterministic streaming and resource bounds. Treat an
evidence-format change explicitly; do not silently imply old hashes have the
new coverage. Add a small focused tamper case for is_stitched (and any other
concrete omitted operand discovered). Avoid broader evaluation changes.

Fresh Codex implementer only, no Claude/API spend. Static edits only: no Python,
tests, database access or heavy tools; root owns guarded runtime. Do not edit
migrations/registry/jobs/activation, commit, stash/reset/restore/checkout--/clean,
or touch others' files. Graph tools unavailable; targeted rg/git reads allowed.
Preserve original line endings. This implements an existing independent review
finding; Important fix is accepted on your report and focused execution, with
no repeated review unless a Critical defect is found.

Write .superpowers/sdd/tier1-parity/fq2-label-evidence-fix-report.md explaining
the exact defect, field audit, version compatibility, changed files and focused
commands for root. Claim no executed tests or production evaluation.
