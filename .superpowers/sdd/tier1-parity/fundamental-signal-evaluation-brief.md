# FQ2: fundamental forward-return decile evaluation

User priority is a usable professional quant platform while memory blocks
backfill. Read fundamental-evaluation-contract.md and FQ1's final result.
Implement actual run-scoped warehouse evaluation tables and a bounded evaluator,
including the small canonical-label provenance prerequisite below. Do not stop
at a permanent unverified-label stub. Do not claim alpha before live measurement.

## Ownership and serialization

Own new src/atx_db/fundamental_signal_evaluation.py,
scripts/evaluate_fundamental_signals.py, tests/test_fundamental_signal_evaluation.py,
docs/FUNDAMENTAL_SIGNAL_EVALUATION.md, migration bodies_0325.py and only its
registration/schema lines AFTER FQ1 commits0324 and releases registry.
For the label prerequisite own minimal edits to _forward_return_publication.py,
the affected label focused test, and only if necessary delisting.py's publisher
handoff. Do not alter its legacy pure pandas API. No FQ1 owned-file, cli,
activation/jobs edits. Coordinate any shared digest helper with root/FQ1 first.
No Python/import/test/DB/network until root grants the sole guarded runtime.
Codex implementer and one fresh Codex review; pathspec-only task commit.

## Canonical forward-label basis

Current rows have no persisted price basis. Add nullable price_basis and
calculation_version columns to forward_returns_survivorship_safe in0325.
Old rows remain NULL/unverified; never infer basis from source name/run_id.
The canonical bounded publisher must insert the actual validated price_basis
used to select its price column and a fixed version of that calculation path
into every newly computed row, in the same stage/shadow/atomic publication.
No synthetic backfill of legacy provenance. Preserve unselected sources and
their NULL provenance. Preserve public legacy projections and numeric values.
Document the code version and exact field semantics in catalog metadata.

This records what the publisher actually calculated. It is not a claim that
vendor adjustments or original historical delivery vintages are certified.
A new full input-hash manifest is not required for this bounded prerequisite.
FQ2 accepts only recognized adjusted_close calculation versions; latest visible
NULL/unsupported basis labels remain excluded and cannot reveal older labels.
Capture selected label IDs/basis/version and an economic-row digest in the
evaluation evidence so the actual evaluated sample is pinned.

## Interface and frozen experiment

evaluate_fundamental_signals(store, options) consumes an explicit complete
FQ1 build_run_id and unique evaluation run_id, evaluation as_of_date and UTC
run_at, explicit label source. Script requires existing migrated DB, bounded
date range inherited from build, memory-limit256MB and one thread by default.
No implicit migration/source refresh/activation. Maximum32 frozen signals.

Revalidate FQ1's exact canonical digest recipe, spec/definition hashes and
logical grains before outcome reads. Reject changed/noncomplete panels.
Do not duplicate a diverging digest recipe: coordinate a public validation
helper with FQ1 owner or pin the same ordered queries in a clearly versioned
reader. Verify observed entry/calendar agreement using the frozen build
calendar recipe; endpoint sessions use the same source and explicit cutoff.
The evaluator must record all predeclared signals, including zero-coverage
signals. Freeze configuration before joins; never choose direction/horizon
from observed performance. Primary21 sessions, secondary5/63.

Add uniquely named fundamental_signal_evaluation_runs, deciles, summaries and
bounded provenance/evidence tables as needed. Completed runs are immutable;
partial chunks require status building, failures remain diagnosable, only
complete manifests consumable. Seal spec/build/config/sample/result digests,
source/cutoffs, counts and blockers. production_eligible=false always.

## Decision cohort, ranking and outcomes

Default cohort is FQ1's proved dated US-common issuer-qualified cohort. Do not
claim it is tradable or capacity-qualified: FQ1 contains no price/ADV screen.
Persist this scope and the missing liquidity/borrow/capacity filters as blockers.
A separate predecision liquidity refinement can follow measured coverage;
do not add an unverified CF1/current-directory join here.

For each decision session and signal, freeze finite eligible scores and decile
memberships BEFORE joining labels. Require200names and20perdecile. Deterministic
security-ID tie allocation is acceptable, but report ties crossing boundaries
and reject a constant cohort; no economic separation claim for equal scores.
Do not re-rank after label attrition or zero-fill missing outcomes.

Entry is next observed session close. Join latest visible label revision on
exact source/security/entry_date/horizon, rank revisions BEFORE status/basis
validity filters, never is_latest_revision. Require finite return>=-1, recognized
adjusted basis/version, exact expected endpoint, valid terminal source
(observed/policy), and outcome available by evaluation cutoff. Keep missing,
invalid, unsupported-basis, observed-terminal and policy-terminal counts by
date/decile. Preserve every requested session/horizon, including empty,
not-matured, insufficient, embargo and purged outcomes.

Chronology predeclared: train<=2020, validation2021..2023, holdout>=2024.
Purge labels crossing split ends and embargo the first63 observed market
sessions of validation/holdout. Use calendar indices without compressing gaps.
All large membership/rank/label joins stay in bounded date partitions in SQL;
only date-level spreads and summary rows cross into Python.

## Inference and evidence

Daily equal-weight Q10-Q1 horizon return spread; reuse pure
custom_features.calendar_hac_statistics (Bartlett/Newey-West lag horizon-1).
General Holm correction over the ENTIRE frozen primary signal family per
split, including missing/untestable hypotheses as p=1 but outputNULL for them.
Secondary horizons never qualify a primary hypothesis. Do not reuse fixed
holm_eight for a variable family. Report local-family correction; repeated
research searches are not globally corrected or certified by this procedure.

Report gross/CI/HAC/adjustedp,10/25/50bp-per-side scenarios (two legs times
entry/exit =>4*cost), annual/date counts, label coverage and terminal policy
shares. Statistical candidate criteria match predeclared CF1 policy:
primary positive spread, Holm<=.05, positive net25bp scenario,252spread dates,
at least2annual buckets each>=60dates and positive,>=99%labelcoverage.
Candidate status is not production eligibility or realized portfolio PnL;
overlapping-horizon spreads are not daily trading returns/Sharpe.

## Focused validation

Use tiny isolated schemas, no full323+ migration bootstrap in new inner-loop
fixtures. Cover adjusted vs raw/legacy label basis, unchanged label arithmetic
and atomic publication, preserve other source, migration replay; frozen panel
tamper/duplicate detection; deciles fixed before missing-label join; entry vs
decision alignment; latest invalid-label suppression; split purge/embargo;
tie/constant/small/empty diagnostics; terminal policy attrition; variable Holm
including missing hypotheses; calendar-gap HAC; immutable/failed run behavior.
Add one end-to-end temporary warehouse evaluation with sufficient synthetic
names to exercise actual INSERT/seal/summary SQL. No fake significant alpha.
Focused tests + scopedRuff only, one fresh review; fullsuite stays at gate.
Write fundamental-signal-evaluation-result.md with exact schema/API/limits.
