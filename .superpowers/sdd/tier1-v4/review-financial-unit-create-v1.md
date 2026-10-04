# Independent one-unit financial creator review

Scope: static review of t17-financial-unit-create-v1.py (NOTRUN), with named
unchanged fund_batch_release.compute_new:597-628, recheck:475-487,
require_snapshot:490-497 and merged_queries:645 onwards. No actual source/data,
hash, controller execution, fixture, index or producer edit was performed.

Spec PASS. Quality PASS. No Critical/Important/Minor findings in this scope.

The requested unit is independent of earlier computed outputs: unchanged
compute_new consumes the pinned snapshot and the selected original extract,
performs its own three full source rechecks, native computation, exact typed/
schema/population checks and second independent original derivation with native
receipt/failure-counter comparison, and seals admission only afterward. Earlier
unit outputs are combined only in the later native merge. The wrapper calls that
function directly without replacing any default checks or resource globals.

Checked wrapper invariants:

- Physical snapshot bytes and independently known canonical digest are distinct
  and both mandatory; original85 extraction admission, all144 preparation/native
  source identities and actual R23 execution/adapter code are rechecked.
- The reviewed file-backed256MB/one-thread R23 connection and original generation
  writer lock are used. Complete runtime proof identity is checked before/after.
- Only the next missing fixed0..84 unit is accepted, with exact prefix frontier.
  Existing admission/start/native receipt/four payload bytes are recorded before
  and compared after; source/start/controller identities are rechecked.
- Native compute_new creates a fresh unique attempt and refuses an already
  admitted unit. Incomplete attempts are retained; the wrapper has no deletion,
  move, merge, pointer or publication operation.
- Immutable unique external start/result records preserve interrupted evidence.
  The result explicitly credits only the new unit and says earlier units were
  neither replayed nor newly qualified by this invocation.
- An active pointer transaction/current publication or candidate final manifest
  refuses. Final original R23 build must still replay every prior/new unit and
  complete whole85 merge, evidence, public validation and atomic publication.

Evidence boundary: the result is written before final runtime context exit and
external guard completion. Credit requires the exact matching successful guard
command/code/start/unit identities, completed/exit0/no cap/no stop and successful
final context exit. A surviving native admission/result after host stop is
retained provisional/failed-attempt evidence until independently checked; result
presence or ATX_GUARD_JOB alone cannot grant credit.

Prior byte records are preservation evidence, not independent admission proofs.
This wrapper does not discharge any outstanding audit of retained7 or earlier
units. Root's planned separate successful native audit7, dead-owner lock
preservation and resource-window admission remain prerequisites for the intended
batch8 invocation. No full-generation, financial quality, economic, scientific
or consumer-acceptance gate is promoted by this review.

This review is independent controller-only work, disjoint from this author's
Task17 product implementation, which still requires its separate scoped review.

## Scoped orphan-lock v5 preservation review

Static review of t17-preserve-orphan-lock-v5.ps1 (NOTRUN). Spec Needs fixes;
quality Needs fixes for the path boundary below. Earlier creator PASS is unchanged.

Important I1: Assert-NoReparse uses `$item = $item.Parent` for a Get-Item result.
A FileInfo has no Parent property, so lock/member file checks stop after the file
and never inspect its parent directories. When archive is absent its check also
returns without checking existing ancestors. A junction in the stage/unit/archive
parent chain therefore bypasses the stated reparse protection; lexical full-path
and Resolve-Path equality do not establish that the native members or moved lock
remain within the intended physical directory. Walk FileInfo.Directory versus
DirectoryInfo.Parent, and for absent paths walk their nearest existing ancestor,
then inspect all existing ancestors before preservation. Keep exact literal
owner/destination/pins, no-delete behavior and original guard/source checks.

Named outside-script type inspection only (no files/metadata/source read):
`$syntheticFile = [IO.FileInfo]::new('C:\synthetic\member.bin'); [pscustomobject]@{ FileInfoHasParent = ($syntheticFile.PSObject.Properties.Name -contains 'Parent'); DirectoryInfoHasParent = ([IO.DirectoryInfo]::new('C:\synthetic').PSObject.Properties.Name -contains 'Parent') } | ConvertTo-Json -Compress`
returned FileInfoHasParent=false/DirectoryInfoHasParent=true,exit0,1.93s.
No recovery/source/hash/controller job was executed.

Other checked boundaries: exact pinned failed guard/inspection/lock/snapshot;
dead owner/guard/child plus native-writer checks repeated before move; exact
retained0..7/all85 admission frontier; every independently inspected native member
byte check; archive under original stage; unique no-overwrite archive/evidence;
no deletion or new native admission and preservation of batch8 partial attempts.
Root's qualifying host window, reviewed audit7 and exact successful guard remain
separate prerequisites; this lock action cannot grant any source/financial gate.

Scoped v5 recovery correction rereview: Spec PASS; quality PASS. Important I1
ADDRESSED. The corrected Assert-NoReparse walks the nearest existing ancestor
for absent leaves, then explicitly uses FileInfo.Directory versus
DirectoryInfo.Parent to inspect every ancestor to root; unsupported types refuse.
Checks now cover lock, archive, destination, guard, inspection and each pinned
native member. Literal destination containment, repeated dead-owner/byte checks,
no-delete unique archive semantics and all earlier gate boundaries remain intact.
Static function/call-site rereview only; no recovery, source/hash or fixture job.
Root's successful qualifying host window and separate audit7 remain mandatory;
this approval grants no native admission/publication/source quality.

## Scoped sequential creation controller review

Spec PASS; quality PASS. No new findings in
`t17-financial-unit-creation-controller-v1.ps1` (static review only, NOTRUN).
The fixed sequence is batches9..15, with unique no-overwrite attempt paths,
the original .8GiB/heavy/40GiB guard, and immediate refusal after any failed
guard or mismatched outcome; there is no retry or unchecked continuation.
Acceptance binds the exact interpreter/script/batch/tag command, original guard
version and receipt, typed completed/zero/no-cap outcome, resource settings and
absence of test overrides. The Decimal-or-Double predicate retains exact .8
equality and rejects strings. Result/start/frontier bindings identify exactly
one new native double-replay-admitted unit, unchanged creator bytes before and
after, and explicitly false publication/source certification/gate promotion and
prior-unit semantic credit. Actual original compute/checks remain in the
separately reviewed Python creator; the scheduler does not replace them.

Root must first authenticate successful audit7 and creator8 guard/results as
the stated operational prerequisites. The unchanged creator independently
requires the exact next-unit prefix, so this controller cannot skip a missing
unit. Scheduler success does not assert separate admit_unit replay of newly
created units, prior-unit qualification, or final whole85 validation; original
whole85 replay/merge/evidence/publication checks remain required.

Covering command: `Get-Content .superpowers/sdd/tier1-v4/t17-financial-unit-creation-controller-v1.ps1`;
exit0, 1.50s. No source/data/hash/metadata/controller/fixture job or index action
was performed. Approval is confined to this static orchestration boundary.
