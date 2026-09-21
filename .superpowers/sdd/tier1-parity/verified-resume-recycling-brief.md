# VR1 — bounded recycling for verified CompanyFacts resume

Read verified-resume-prefix-audit.md first. The observed archive6 verified
prefix took about15minutes after raw proof, with6554verified loaded members,
1107empty replays and no new raw rows. Source proves verified candidate-only
transactions consume the same ten-member recycle counter as raw replacements.
That distinction justifies reducing unnecessary lifecycle work; the fraction
of elapsed time caused by reopening is unmeasured.

## Scope and constraints

Draft only while archive7 owns the sole runtime. Use
verified-resume-recycling-draft/ for source/test copies and an integration patch.
Do not modify live source, run Python/imports/tests/lint/DB/profilers, commit,
or interrupt the current job. Root integrates after the writer is terminal.
Codex only; no LLM API spend. No stash/checkout/reset/restore/clean. No edits
to registry.py, jobs.py, activation.py or historical migrations.

Own only the narrow SecCompanyFactsDataset.load counter/cadence logic in
atx-db/src/atx_db/fundamentals.py and focused cases in an existing suitable
CompanyFacts test file (prefer test_companyfacts_archive_repair.py). No new
public module or schema surface. Preserve all unrelated code exactly.

## Required behavior

1. Keep raw fact/point replacement recycling at ten successfully committed
   issuer replacements. Empty replacements still count, because they must
   remove stale facts/points/candidates atomically and retain receipt behavior.
2. Verified loaded members retain their per-issuer candidate reconciliation
   transaction but use a separate bounded counter, recycling after at most
   100verified members since the last reopen. This is a conservative bounded
   alternative to eliminating their recycling entirely. A reopen caused by
   either counter resets both, since it frees the same connection state.
3. Preserve the explicit post-proof reopen and final partial-work reopen when
   either counter is nonzero. Preserve configured-persistent eligibility and
   existing session/temp-relation protections. No active cursor crosses reopen.
4. Unavailable placeholders retain existing preservation/receipt semantics;
   failed transactions never count as committed work and errors remain loud.
5. Preserve archive hash/identity, receipt admission/lineage, full multiset
   fingerprints, scope, row ownership, raw receipts, conservative clocks,
   per-issuer atomicity and source completeness. Do not batch or omit candidate
   cleanup, skip empty replay or weaken existing assertions.

## Focused evidence to prepare

- More than ten verified members do not consume the raw ten-replacement
  cadence. One hundred verified members still force a bounded reopen.
- Mixed verified/raw/empty sequences demonstrate independent counters and
  reset-on-either behavior; preserve post-proof/final partial handling.
- Candidate rows recover/clear exactly as before; verified raw fact/point and
  original receipt contents/ownership are unchanged. Reuse existing real
  resume fixture semantics, not a fake verifier that sidesteps correctness.
- Retain existing raw-write/session/temp checks. Cover an actual candidate
  reconciliation error so rollback/failure behavior is not weakened.

Keep test breadth proportionate to this narrow change. Root runs the covering
focused selectors once when runtime is free; full suite remains at the gate.
One independent Codex review, re-review only for Critical findings. Important
fixes are accepted on the implementer's report after focused verification.

## Deliverables

Write verified-resume-recycling-report.md with exact changed paths, focused
selectors, source/patch hashes, invariant reasoning and integration instructions.
Package a patch that applies without replacing unrelated whole files. No live
performance or memory-capacity claim: those require a future necessary guarded
resume, not an extra full-universe rewrite solely to benchmark this change.
Do not expand this into generic recovery tooling or loader redesign.
