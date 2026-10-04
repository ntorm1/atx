# Readiness v9 scoped independent review

Spec: Needs fixes. Quality: Needs fixes. Static metadata review only; no source,
native payload, hash, actual data job, fixture or index action.

## Important I1 — retained-file frontier contradicts the current checkpoint

`readiness-matrix-v9.json:2865` retains
`financial_retained_native_admission_files=[0,1,2]` from an earlier checkpoint,
while its current admitted count is8 and independently checked IDs are0..6.
`refresh-readiness-v9.py:70` updates the count/checked/pending fields but leaves
this current named retained-file field unchanged. The independently bound v5
inspection establishes retained admissions0..7. A consumer reading the explicit
file frontier therefore receives three retained units instead of eight, and an
impossible checked frontier outside the retained frontier. Update this current
field to the inspected0..7 frontier, or clearly move/rename it as historical
evidence while exposing an unambiguous current retained-file list. Preserve
all historical requirement observations and seven-unit audited credit.

## Checked invariants

All46 original requirement contracts/statuses are identical to v8 excluding
append-only observations; every prior observation remains an exact prefix.
The separately required P9 appendix reference and all13 IDs are unchanged.
Checkpoint acceptance remains NOT_PRODUCTION_READY; final financial/universe/
gold/golden publication, predictive measurements, source/clocks and consumer
acceptance remain unqualified/false. Checked IDs0..6 and pending7 correctly
distinguish separate native audits from retained admissions.

The update binds the stopped34458 continuation and v5 interruption evidence to
their reviewed literal metadata hashes, preserves stopped_low_commit/no final
manifest/current pointer, and authenticates completed memory-window/recovery
guard command/resources without promoting a gate. The memory window reports
minimum commit3.6207GiB/physical2.7781GiB. Exact dead-owner lock recovery status
is ORPHAN_LOCK_PRESERVED_AFTER_DEAD_OWNER_CHECK. Audit7v1 is refused_low_disk
below the unchanged40GiB floor, with no child, result or start. Creator8 and
scheduler9..15 receive no new credit. Metadata inventory is rechecked before
exclusive output creation; native source inventories are not read here.

Covering commands: one full Get-Content of refresh-readiness-v9.py (exit0,1.30s),
then repository-venv stdin JSON comparison asserting46 contracts/statuses and
observation prefixes, unchanged appendix,8 count/seven checked/pending7, refused
guard/no child/result/start and memory/recovery summaries (exit0,.95s). A named
appendix JSON read confirmed all13 IDs (exit0,.97s); named rg located the
contradictory field and update lines (exit0,.88s). No unchanged suite rerun.

## Scoped correction rereview — v10 only

Spec PASS; quality PASS for refresh-readiness-v10.py and readiness-matrix-v10.json.
Important I1 ADDRESSED in this new version. Rejected v9 remains historical and
unchanged; this verdict does not approve its stale field. V10 authenticates the
same independently pinned v5 inspection and replaces only the current retained
file frontier with0..7, while checked0..6/pending7 and all unqualified acceptance
gates remain unchanged. It preserves all46 complete requirement records,
observations and13-ID P9 appendix verbatim, records an explicitly non-promoting
correction with the prior v9 identity, rechecks both metadata inputs, and uses
exclusive creation rather than overwriting v8/v9. The inherited v8 stale frontier
is not silently rewritten or granted current authority.

Covering command read the new correction script and compared v9/v10 JSON using
the repository interpreter: exit0,1.47s; changed top-level fields are only schema,
version and checkpoint; current retained0..7, checked0..6, acceptance
NOT_PRODUCTION_READY and requirement/appendix equality assertions passed.
Static metadata only; no actual native source/payload/hash/job or fixture/index.
