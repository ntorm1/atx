# Private batched candidate proof component

This component implements the first numerical step of the approved
[complete-capture plan](cold-price-candidate-capture-review-20261024.md).
It adds no fitter dispatch and changes no public/default policy. The ordinary
candidate function still executes scalar Screen and Fast proposals/proofs in
the original order. Its Screen proposal and complete Fast continuation are
factored into shared bodies used by the private batch helper.

`american_implied_vol_price_candidate_batch` takes synchronous, disjoint
request/output spans and one exact `IvOpts` value. Requests retain every
target/model/budget field; the caller owns the spans for the complete call.
There is no retained prepared state or borrowed numerical owner after return.
Only the original Screen proof stage can use the already reviewed explicit
`VectorReferenceEmpirical` seed evaluator. Its negative-carry exclusions,
boundary margin, empirical allowance, exact scalar floor and full scalar
fallbacks are unchanged. No vector value is labeled a scalar Reference proof.

The helper acquires three scratch arrays before executing any inverse, runs
the unchanged Screen proposal per eligible request, then batches its actual
finite seeds. Each declined/missing Screen proof resumes the shared original
Fast chain with the same controls and retained Screen statistics. That chain
still includes the early Fast proof, complete Fast retry and final scalar
proof. A Screen proposal is never repeated to perform the continuation.
The separate `screen_proof` result retains its explicit origin/retry/evidence;
the final sigma is published only after a complete successful proof.

Scratch allocation failure occurs before any proposal and runs the complete
original scalar function once for every request. It is separately counted as
`complete_scalar_fallbacks`; staged proposal/proof counters do not pretend to
observe its internal work. Escaping allocation failure during a staged Screen
or Fast continuation refuses only that request, preserving original candidate
semantics. Contained optional-floor allocation failures retain their existing
cold scalar fallback and are not mislabeled as escaping failures.

The inclusive optional clock starts before output reset and scratch setup and
ends after scratch and floor-owner destruction. Setup, Screen proposals,
the existing proof component and Fast continuations have separate subclocks.
Fresh proof and continuation floor preparations/hits are separate. Caller
request/output construction is outside this helper and must be included in the
finalizer's owning clock when wiring is implemented. No latency result follows
from these counters or from the targeted correctness runs.

Three refusal-stub tests failed at runtime before implementation. The final
focused pricing run passes 37 tests: four new batch controls, five trace,
four lifetime capture, five existing seed-batch and 19 non-monotone controls.
New cases cover both sides, negative carry, tails, exact sigma/statistics,
failed Screen proofs, resumed Fast acceptance, full retries and refusal,
explicit nondefault Reference options, invalid inputs/shape, and unchanged
inverse-iteration counts. The existing trace tests keep scalar decisions,
phase arguments, work and floor state observable after factoring.

Five isolated allocation controls pass, including two new groups. Failure at
each of the three scratch allocations preserves every original result through
the complete scalar fallback. Fail-once allocation at actual Screen and
resumed Fast error construction preserves one attempted call, its refusal and
which continuation was reached. Three owning source/test files compile with
PCH disabled (the shared pricing test has two owning object targets).
Evidence prefix:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/candidate-proof-batch-*`.

Still required: independent source review, typed completed/pending finalizer
prefix and shared Search tail, explicit default-off build provenance, all
700/684/16 and direct70/61 work counts, bitwise whole-fit price/status/IV parity,
the 13-gate scalar actual-API audit and an independently archived ThinLTO
comparison. This component alone establishes none of those whole-fit results.
