# Read-only stock-transition inventory and pre-role consistency review

Reviewed source at pool4 `47696031` and runner `5e1fa849`; corporate transition/claims files are unchanged versus root integration `d52ed04b53e000d7c376876f194e47226e4ca774` (targeted git diff empty). No implementation, build, test or real payload access in this task.

## Pre-role cash event edge: closed

Pool5 confirms its prior finding was specifically old known-completion clocks with `recognition_mark_ns` later than the initial role mark. Core `1ef2524d`, included in final `47696031`, rejects that contradiction. The seventh fixture pins refusal. Runner `5e1fa849` forms its owned historical VM/blend mask using `effective_by_ns < decision_time_ns && available_at_ns < decision_time_ns`; it no longer waits for valuation recognition to change decision eligibility. The execution kernel uses the same information boundary and additionally reserves known locked positive equity from newly formed target capital. Claim accounting still waits for the first strictly known mark. Pool5 confirmed no further warmup inconsistency. No speculative semantic change is needed before the focused build.

## Existing stock conversion: useful arithmetic, not real admission

The corporate-action substrate is checkpoint12/13 (`SECURITY_TRANSITIONS.md`), distinct from the current DAG's D4 PIT-classification lane. It already represents a positive rational predecessor-to-successor stock conversion with optional nonnegative USD cash per predecessor share. Setting the cash numerator to0 gives a pure stock conversion. Long and short signs survive; existing successor holdings net, including exact offset, while attributed delivery remains separate.

- `data/security_transition.hpp:42`: `SecurityTransition` binds predecessor/successor vendor IDs, identity epochs and mapping hashes, ratio, entitlement, terms, state version, sequence and evidence clocks. `TransitionBasis` binds source/axes/adjustment hashes, each raw/TRI mark and explicit excluded-from-TRI component coverage.
- `src/book/security_transition.cpp:355`: `plan_security_transition` converts predecessor TRI units to research share equivalents, applies stock ratio, converts to successor TRI units using its independent raw/TRI basis, then computes the signed optional cash claim and valuation bridge. It guards absorbed additions, overflow/underflow and signed leg reconciliation. Cash stays unchanged.
- `book/claims_state.hpp:134` and `src/book/claims_state.cpp:514`: atomic commit validates original state/units, records event/claim IDs, retires predecessor and delivers successor. `compute_claims_nav` includes signed pending claims. Separate full-payment planning/commit is NAV-neutral and requires allocation evidence.
- `book/replay.hpp:488-602`: `replay_scheduled_intents_with_events` adds bounded event batches to scheduled replay. Only admitted predecessor marks may be carried into the mandatory bridge; all other required marks retain ordinary validation. Delivery is not an execution trade. Retirement prevents reopening. The callback is structurally absent at period0 and final valuation.

The blockers are explicit in executable source, not merely documentation:

1. `security_transition.cpp:85-103` accepts ONLY SyntheticFixture admission and SyntheticFixture evidence. StrictAsOf and RetrospectiveReconstruction enum names are reserved and rejected. Research units only; no physical-share claim. Replay repeats the synthetic admission guard at `replay.cpp:646-656`.
2. It assumes one distinct successor already on the ordered axis, observed positive raw/TRI successor marks, and affirmative stock/cash/vendor-continuity exclusion evidence. It cannot infer these from a missing predecessor quote or merger headline.
3. Replay's old short policy explicitly does not prove loan transfer or discharge. Pending-payable financing is `NoneDisclosedV1`, which explicitly understates financing; the new execution cash-claim reserve/carry policy must not be silently replaced by it.
4. No application caller uses this planner or event-aware replay; the source caller search finds only the book replay implementation. The actual DSL runner uses `factory/execution_objective.cpp`, a different marked-dollar book and delayed-order schedule.

The historical checkpoint13 receipt records synthetic T1/T2 minimal closure, 62/62 then-current book tests, and defers scenarios B/C, application allocation certification and equity-book wiring. Those are historical receipts, not a fresh run here or real-event qualification. Owning existing fixtures are `tests/book/security_transition_test.cpp`, `book_claims_state_test.cpp` and `book_claims_replay_test.cpp`.

## Smallest credible next bridge, only when an actual event requires it

Reuse the arithmetic/atomic invariants as a checked compiled primitive; do not pass real evidence through SyntheticFixture or merely enable a reserved enum. An explicit reconstructed-publication version must bind both historical identities, completion/availability, ratio/cash terms, raw share basis, observed successor price and adjustment-component coverage. If the successor is absent from the loaded price axis, refuse and revise the bounded input artifact with explicit extra held-asset support; never substitute another ticker or future universe membership.

At an admitted current mark, first mark the already-held successor, remove the accounted predecessor, deliver signed successor dollars `(old_dollars / old_raw) * stock_ratio * current_successor_raw`, and recognize any signed nonspendable cash claim. Add delivery to the existing successor position, retaining attribution and a separate mandatory value bridge. Cancel predecessor orders; do not cancel unrelated successor orders. Subsequent queued target orders net against actual post-delivery holdings and incur costs only on their actual fills. Successor observation/presence and source clocks remain required, including when it is outside the decision universe. Short obligations continue explicitly; no inferred locate, loan discharge, cash payment or liquidation.

This would require a new typed event/context variant and small private execution integration, plus root-owned evidence/runner plumbing; it is not already runnable. Keep the current cash-only gate intact while qualifying it. No stock bridge implementation or widening of old admission was made by this review.
