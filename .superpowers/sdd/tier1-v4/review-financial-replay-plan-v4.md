# Independent financial replay/recovery plan v4 review

2026-10-04. **Spec NEEDS CHANGES; quality NEEDS CHANGES for the proposed
audit evidence chain.** One Important finding; one Minor hardening point.
The frozen replay-only mechanism is correct. This is controller-script review,
not a Task17 product repair round or source/product approval.

Reviewed t17-preserve-orphan-lock-v4.ps1, t17-financial-progress-audit-v4.py,
retained interruption-inspection-v4/stopped-guard declarations and relevant
unchanged R23 fund_release_runtime/fund_batch_release source. No actual jobs,
hashes, archive/warehouse payload reads, HTTP, fixtures, producer edits or index
actions. Only this report is owned. Root execution should wait for the small
controller evidence-chain correction below.

## Confirmed recovery/replay semantics

Lock script restricts generationr17_20261004_002 and exact five-byte owner10544,
known lock SHA5173a948..., stopped guard8860/child4624/exit137/statuslow_commit,
known guard SHA20f5006c..., original snapshot SHA and retained admissions0..2.
It refuses owners10544/8860/4624 or matching module writers, pointer/final
manifest, unexpected resolved/reparse paths, existing destination/output and
changed named inspected unit members. Owners and lock bytes are checked again
immediately before Move-Item. The sole move is the original lock to its named
_interrupted_locks archive; no unit, source, failed attempt or partial payload is
deleted/moved. Guarded execution and root serialization remain necessary; this
is not an automatic stale-lock removal path.

Frozen `fund_release_runtime.build:110-173` enters unchanged writer_lock and
runtime connection, freezes complete original source/binding/extraction
admission, verifies the independently expected snapshot and existing runtime
proof, then visits **every REQUIRED batch**. It calls N.admit_unit before the
max_new_batches condition. Therefore max_new_batches0 suppresses compute_new,
but does not suppress existing unit replay. `fund_batch_release.admit_unit:
571-594` authenticates start/native receipt/member inventory, reconstructs each
existing unit into a temporary directory using _replay, compares original typed
schemas/full rows via EXCEPT ALL, native counters/receipt semantics and recorded
actual payload identities. It preserves original unit/partial evidence; only
verification scratch and separate runtime-attempt evidence are created.

For this frontier, a successful replay-only build returns partial_cache_only,
published=false, missing3..84. It creates no new unit or final pointer/manifest.
It still rechecks source/proof/runtime after all units. This validates retained
units0..2 against the frozen native source protocol, not original85unit release,
financial economics or downstream source/coverage/statistical acceptance.

## Findings and smallest corrections

1. **Important — audit lacks an independent unit binding across replay.**
   t17-financial-progress-audit-v4.py requires successful exact replay command
   and expected missing-batch stdout, then reads the current three admissions
   and compares their current payload/start/receipt hashes to those current
   admissions. Its R.connection block calls N.recheck for sources and runtime
   proof, but never calls N.admit_unit. It binds only batch0files to the prior
   root audit. A native unit/admission coherently replaced after successful
   replay can satisfy its current adjacent hashes/footers without being the
   bytes that actually passed typed replay. The claim “all three units are
   checked here” is therefore unsupported by that script alone.

   Smallest nonduplicating correction: independently pin the exact original
   interruption-inspection-v4 document and exact preserved-lock recovery
   evidence. Recovery must compare every inspected unit/start/receipt/payload/
   admission to those original identities before replay. After successful replay,
   audit must require its current three admission dictionaries and every named
   inspected native member identity to equal that same original inspection,
   including batches1/2and retained failed/partial attempts. Recheck those
   independent evidence identities before/after audit and retain them in output.
   Authenticate successful guard/stdout and unchanged runtime/source contract
   as already planned. This makes typed replay of the independently fixed bytes
   the evidence for admission, rather than local hashes creating new admission.
   Alternatively call N.admit_unit for0..2 inside the audit's unchanged runtime
   connection, with source/proof and output checks before/after; that is a fresh
   expensive typed replay and must be separately guarded/authorized.

   Also check the **actual complete admission frontier** before/after audit is
   exactly0..2, instead of copying missing3..84 solely from prior stdout. Reject
   active/intervening writers or unexpected new admissions. Preserve all partial
   evidence; do not rewrite units/admissions or weaken frozen native validators.
   The output should say current identity checks authenticate the exact units
   that passed the successful independently bound replay; no new unit is built,
   and only a later readiness checkpoint may credit the newly current-checked
   retained1/2. Original source-economic/full85release gates remain open.

2. **Minor — pin the inspection itself during lock preservation.**
   The PowerShell script independently pins guard/snapshot/lock but reads the
   inspection's unit-member inventory without an independently expected full
   inspection identity, and hashes the inspection only when writing recovery
   output. Its exact five-byte move remains safe and narrowly bounded; this is
   an evidence-authentication gap, not deletion/producer modification. Bind the
   prepared original inspection SHA/bytes before use and recheck unchanged after
   the member checks and before preservation. Record that exact pin. This also
   supplies the original pre-replay binding needed for finding1; root can supply
   the known guarded metadata identity without a new warehouse/source scan.

## Verification boundary

No source hashes were computed here. Known identities/statuses are read from
prepared scripts and retained small reports only. Exact code reads confirmed
admit_unit precedes compute_new gating and source-backed typed comparisons occur
for every existing batch. No unchanged suites or actual replay were run.
The recommendations change only root controller scripts/evidence, not frozen
R23 producers or Task17 R26 product ownership/repair count. This reviewer remains
independent for the later combined R26 review. A corrected controller plan needs
a narrow rereview before root execution; this report grants no actual run.

## Narrow corrected-plan rereview — 2026-10-04

**Spec PASS; quality PASS for the corrected controller plan.** Prior Important
and Minor findings are addressed. Static source review only; neither script or
native replay was executed by this reviewer and no hashes/jobs were launched.

Recovery now checks the independently declared15890byte inspection identity
945ca1845869df47904a1ed24bd4371bac078a155d23c7fd2817398c46759746 before
parsing it, then rechecks it and exact stopped-owner guard immediately before
preserving the original10544lock. Its original checks of all named retained
native/failed/partial members remain intact. The moved object remains solely
the authenticated five-byte lock; source/attempt/unit evidence is preserved.

Audit now independently pins that same original inspection and7402byte prior
batch0audit9cef7b23958163d6ad2e23167929625687c86c6d544ac5dac7ea3065a3444322.
For each admitted0..2unit it first matches admission bytes and complete admission
object to the original inspection, then matches start/native receipt and all
four payload identities to original inspected pins. Guard/stdout identities are
captured before parsing and retained for final recheck. Exact successful recovery
guard/command/result binds the inspected frontier and lock history. Full actual
admission frontier is checked before and after source checks, with unexpected
unit names/symlink/junction paths refused. All used evidence/source/runtime pins
are rechecked afterward and unexpected final manifest/pointer still fails.

These checks close the prior coherent-after-replay replacement gap: original
independently fixed units checked before recovery must remain the same bytes
after the exact successful frozen replay-only command. Typed replay itself still
comes from unchanged native admit_unit; the audit explicitly says it does not
duplicate that replay and hashes alone grant no new admission. No new unit
creation/full85release/source-economic gate is claimed.

This PASS is conditional plan engineering under root's existing exclusive
controller/guard sequencing, not evidence that recovery or replay has run or
succeeded. Root must retain actual terminal recovery/replay/audit receipts and
reconcile any failure before accounting current units. A later readiness update
may credit retained1/2 only after that complete successful chain; all original
actual/source/coverage/consumer gates remain open. No product repair counter,
frozen producer or Task17 R26 independence is changed by this rereview.
