# Independent cash-claim engine review

2026-09-26. Reviewer: `g0_evidence`, pool-3. **Source approved** through production `c0be402e102e1fbd3ad21a1578b1ae4290e31857`, `1ef2524da051c8ba4b9938f7f9ee3de8b601e0e6`, and `440d538f778bdb7e893ad4aec5514a74399361f6`; fixtures `25ff0a88cc486878cd58369c92c075b9cbb80257`, `3d1ef20ab08a5e96bc690e506740291bf575617f`, and fixture/documentation follow-up `4769603148ca91e3a642854d6578cfdb43325e05`.

No compiler, runtime test, market-payload read, or numerical strategy run was performed for this review. This is approval for focused qualification, not runtime, event-coverage, settlement, or alpha-performance acceptance.

## Reviewed contract

- `execution_cash_claim.hpp`, owning streams, private conversion/validation, and the actual chronological `execution_objective.cpp` integration were read together. Exact source/identity/basis pins remain caller-supplied evidence assertions. In-role admission additionally compares the immediately previous observed raw and adjusted marks exactly against the Panel.
- Completion and publication must strictly precede recognition. Recognition is the first strictly later role mark. Pre-role extinction blocks formation without an invented opening entitlement; delayed pre-role recognition refuses. Outside-axis and after-role events remain explicit in identity/use diagnostics. Between-mark publication blocks formation at the already-informed decision.
- Conversion is signed held dollars divided by the prior raw close, multiplied by cash per predecessor raw share. The bridge changes NAV; settled cash does not receive the consideration. Positive receivables are excluded from fresh target capital. Negative claims reduce NAV, reserve cash, and retain the last admitted modeled annual short rate; recognition-interval equity borrow and subsequent claim borrow are separate.
- Recognition clears all pending targets for that instrument before fills and removes its equity holding. Other instruments retain ordinary strict source-presence/return-guard checks. No settlement date, stock conversion, loan termination, or cash receipt is inferred.
- Event count/string bounds precede copying. Retained event/name indexing, per-signal claim arrays/records, and scratch are admitted with the existing checked budget. Conversion, aggregate claims/borrow, target capital, and financing paths check nonfinite/overflow cases. These are declared allocation bounds, not measured RSS.
- The empty event route omits the claim identity suffix and bypasses claim arithmetic. Existing source functions require the explicit owning claim result for nonempty contexts, preventing silent loss of diagnostics. `AlphaStreams` was not expanded.

## Finding and closure

The initial between-mark-and-decision fixture intentionally sized fresh targets from full pre-recognition NAV even after completion was public. That allowed already-known locked positive equity to contribute fresh target capital until the next mark. The reviewer raised this policy gap; `440d538f` now subtracts its current accounted positive holding at that informed decision, without recognizing a future gain or cash receipt. Previously queued targets remain fixed. The fixture now requires the independent remaining-capital result (250 rather than 500 in its toy book). The new sum has an explicit overflow check.

The runner's separate initial retirement mask in `80ebf709` still used recognition-mark timing. Its owner was notified to align VM/rank/blend support to the same strict decision clocks. That caller correction is outside this engine approval and must be reviewed separately.

## Focused fixture coverage

Seven `ExecutionCashClaim.*` cases cover raw-versus-adjusted share conversion; signed liability/cash reserve/calendar borrow; delay-3 pending orders; nonspendable targets; strict clocks, source and raw basis; future-event prefix invariance; unrelated missing held marks; pre-role/outside-axis/after-role use; between-mark publication; and empty-route parity. The final parity fixture compares all eight floating arrays, validity/name/cap arrays, context hash and realization bounds. These fixtures have been inspected, not executed by this reviewer.

Exact Git blob anchors: private conversion `d685c0308f5b9c72578ce6aed574370ecca7e3ed`; final integrated execution source `b1886bc3a0f09bc2fcf678971080f0e6fa35a94c`; final fixture `c046db5473d836ca35481fc3bdd73cdf69b48526`.

The independently sourced four-event evidence is separate in `four-cash-event-primary-evidence.md` and `four-cash-event-prior-source-records.json` (commit `7ae76e91`). All four payment/usable-cash dates remain unknown.
