# IQ2 whole-branch review repair

Candidate: 727e6b90. Work on feat/tier1-parity in the shared tree.
The independent whole-branch review found a Moderate regression in
issuer_derived_asof: with eligible owner IDs but zero visible derived rows,
result_columns remains empty and the returned DataFrame loses its schema.
The prior SQL df() preserved column names even for zero rows. A caller indexing
derived_value_id now gets KeyError. The fetched-but-all-rejected case was already
fixed; cover the zero-candidate case too.

Own only atx-db/src/atx_db/asof/fundamentals.py and
atx-db/tests/test_issuer_selected_lineage.py, plus the report. Capture the actual
query result columns before the empty-page exit, retaining existing byte bounds,
qualification, cutoff and paging behavior. Add one narrow empty-page fixture;
avoid widening scope or changing the separate no-eligible-owner contract.

Fresh Codex implementer only, no Claude/API spend. Static edits only: no Python,
tests, database access or heavy tools; root owns guarded runtime. Do not edit
migrations/registry/jobs/activation, commit, stash/reset/restore/checkout--/clean,
or touch others' files. Graph tools unavailable; targeted rg/git reads allowed.
Preserve original line endings. This implements an existing independent review
finding; fix accepted on report and focused execution, no repeated review unless
a Critical defect is found.

Write .superpowers/sdd/tier1-parity/iq2-empty-schema-fix-report.md with the exact
change, changed files and focused commands. Claim no executed tests/live query.
