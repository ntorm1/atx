# Independent controller-only financial unit validation review

2026-10-04. **Spec PASS; quality PASS** for the proposed incremental retained-unit
audit in `t17-financial-unit-validate-v1.py`. No Critical/Important/Minor finding.
This is static review, not a run, unit admission, financial/source gate pass or
permission to overlap the active native writer. No source/hash/metadata job,
fixture, index or producer change was performed. Only this report is written.

The controller is an evidence wrapper around unchanged `fund_batch_release`
and the separately pinned R23 runtime. It never calls compute_new, merge,
publication or recovery, and grants no semantic credit to unnamed frontier
units. The existing native admission must already exist. Exact independent
physical and canonical snapshot pins remain distinct; complete85 extraction
admission, execution contract and actual adapter-code identities are literal
external expectations. Runtime connection verifies actual file-backed256MB,
one thread, UTC, external guard identity and disk floor before and after.

Checked the controller once, then the named native functions `recheck`,
`require_snapshot`, `payloads`, `admit_unit`, runtime `check_proof`, `connection`
and `_recheck_proof`. No broad diff, actual candidate inspection or source read.

Checked invariants:

- Original writer lock prevents concurrent native admission changes. The wrapper
  refuses an activated/published candidate or pointer transaction; it does not
  reclaim the known dead lock or modify prior failed attempts.
- Native recheck(full=False) preserves the full current frozen source/code/
  preparation identity protocol and independently admitted85 extraction reuse.
  It does not replace the named unit's mandatory typed replay. Complete snapshot
  contract, code/product sets and144 preparation units remain enforced.
- Native admit_unit reconstructs the named unit from pinned originals, compares
  complete physical schemas and populations and original failure/counter receipt
  semantics, and checks exact original attempt/start/native payload identities.
  This is the unchanged original validator, not hash-only or abbreviated replay.
- Wrapper physical pins cover admission, original start, native receipt and all
  four payloads before/after. Canonical/physical snapshot, R23 proof, full source
  identities, controller bytes, immutable start and entire admitted frontier are
  rechecked. Unexpected unit paths, links/junctions and changed frontier refuse.
- Start and result use immutable, unique attempt-tag paths. Interrupted attempts
  are preserved. No generation inventory or native admission is altered.
- The result explicitly withholds publication, source qualification and gate
  promotion. Final complete85 generation validation, merge/evidence, original
  economic/science gates and publication protocol remain unchanged and mandatory.

The JSON result is deliberately conditional: it is written before the external
guard process has terminated, and the R23 context's final resource check also
occurs after that write. Root must count it only with the exact invocation's
matching successful external guard receipt (completed, exit0, no cap or stop),
and verify the recorded controller/start/unit/source/runtime identities against
the independently reviewed invocation. A result surviving a later host stop or
failed context exit is retained failed-attempt evidence, not a checked unit.
The controller states this requirement explicitly; no guard result is inferred
from the mere ATX_GUARD_JOB environment marker or result-file presence.

This approval concerns the controller wrapper and its evidence boundary only.
It does not self-review this author's separate Task17 product implementation.

## Adjacent PowerShell orchestration review

Static review of `t17-financial-unit-validation-controller-v1.ps1` and the named
original guard receipt-construction lines622-630/787-789 only; no execution.
**Spec Needs fixes; quality Needs fixes** for this adjacent receipt acceptance
boundary. The Python wrapper's preceding PASS is unchanged.

**Important I1 — exact guard invocation is not authenticated at acceptance.**
`t17-financial-unit-validation-controller-v1.ps1:18` checks status/exit/cap/stop
and the result's self-declared receipt path, but never compares the guard's
`command`, `receipt`, `job_limit_gb`, `heavy` or `guard_version` to the actual
independently reviewed invocation. A completed/exit0 receipt for another command
copied to the selected receipt filename would pass when paired with a surviving
named result. Missing `cap_hit` also acts false in this predicate. This does not
meet the Python result's explicit *matching successful exact invocation* boundary.
Require the original receipt's mandatory typed fields and exact interpreter/
script/batch/attempt command, receipt path,0.8 limit, heavy flag and supported
guard version before counting. Also require immutable result semantics
new_units_created=0/publication=false/source_quality_certified=false/
gate_promotion=false. Do not count a merely present or loosely matching JSON.

Other orchestration invariants checked: four sequential batches3..6; original
0.8GiB heavy/40GiB disk-floor invocation; unique retained output/log/receipt names;
immediate fail/throw on external exit or native-result mismatch; no parallel
audit, native writer creation, source alteration or producer/runtime edit.
Guard success may omit stop_reason under the original schema; its absence alone
is not a defect, while any present stop reason must refuse. Root should repair
this narrowly and request scoped static re-review before execution.

## Scoped I1 correction review

**Spec PASS; quality PASS. Important I1 ADDRESSED.** Static rereview of the
corrected adjacent PowerShell acceptance block only; the Python wrapper is
unchanged and its preceding PASS remains in force. No controller execution,
source inspection, source hashing or fixture was performed by this reviewer.

The correction requires mandatory guard fields, exact command array (interpreter,
script, batch and attempt), receipt identity and guard version; typed successful
exit/cap/heavy fields; the original 0.8GiB page-aligned cap,40GiB disk floor and
absence of test overrides; mandatory typed non-promoting result semantics; and
controller byte identity across prelaunch, native result and postrun. Missing,
mismatched or stopped guard outcomes now refuse before a unit is counted. Four
sequential unique retained attempts and fail-closed termination remain intact.
The original successful guard schema may omit stop_reason; any present stop
reason still refuses. This resolves the specific receipt substitution and
missing-cap-field acceptance defect without altering native admission, the
complete85 final protocol, or source/economic gates.

Root separately reports an in-memory acceptance-block check: one positive
accepted and17 concrete mutations refused, chunk96e8cd,2.0s. That is controller
reported supplemental evidence; this verdict rests on the scoped static review.
