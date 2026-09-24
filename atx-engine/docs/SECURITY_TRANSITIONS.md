# Signed security transitions

The book transition API plans one mandatory predecessor-to-successor stock
conversion and an optional signed USD cash claim. It also plans a separate,
identified full payment of that claim. These pure functions do not modify holdings
or run a replay. The initial admission mode is `SyntheticFixture` with continuous
research share equivalents; historical and physical-share modes reject.

Use the terms and evidence types in
[`data/security_transition.hpp`](../include/atx/engine/data/security_transition.hpp)
and the planners in
[`book/security_transition.hpp`](../include/atx/engine/book/security_transition.hpp).
Inputs bind security identities, source axes, adjustment recipes, evidence clocks,
state versions and applied IDs. Nonzero digests are references, not authentication
of external facts. Successful plans own fixed-size copies of terms and basis;
their borrowed input spans do not escape.

For predecessor units `q`, raw price `P` and TRI price `I`, the supported research
share equivalent is `q * (I / P)`. Apply the stock ratio to that quantity, then
convert to successor TRI units using its own `P / I`. Apply cash per predecessor
share to the separately supplied pre-transition entitlement. Stock, cash and
vendor-continuity components must each be explicitly excluded from the supplied
TRI bases to avoid counting them twice.

Pending claims retain their sign. A short produces a payable; full payment
decreases cash and removes that payable with zero accounting P&L. A receivable
contributes to NAV but does not become settled cash before payment. Existing
successor holdings may net with delivered units, including exact cancellation;
the plan still retains the attributed delivery and its value. The interval value
bridge includes any movement between the supplied marks.

Each nonzero economic leg must survive floating-point representation. The
published arithmetic order reconciles legs within `64 * epsilon` relative to
their magnitude, without an absolute floor or cash correction. Absorbed additions,
overflow, underflow and poorly conditioned payment into cash reject.

A caller must atomically apply all deltas and record event/claim/payment IDs only
while the starting state version still matches. The replay adapter and its owned
claim ledger are subsequent work. They must include claims in NAV and allocation
certificates, keep claims out of settled cash, reject retired-target reopening,
preserve required valuation boundaries and report mandatory movements separately
from external trades. The existing historical replay remains unchanged.

For real short books, numerical position netting does not establish loan
termination. The [SIFMA lending guidance, section 8](https://www.sifma.org/wp-content/uploads/2024/06/Master_Securities_Loan_Agreement_MSLA-Guidance_Notes_2000_Version.pdf)
distinguishes cash distribution transfers and noncash loaned-security additions;
[IBKR's payment-in-lieu description](https://www.interactivebrokers.com/campus/glossary-terms/payment-in-lieu-of-dividends/)
also describes the short holder's distribution liability. These support retaining
separate obligations in the design, not assuming particular historical loan terms
or payment dates.

The failed 2013 PCS book attempt remains frozen. Actual event admission still
requires vendor mapping, adjustment coverage, timing, entitlement, successor
valuation and loan/payment evidence. See the
[transition design](../reviews/2026-09-20-iteration12-security-transition-design.md)
for those boundaries.

## Validation status

The [checkpoint 12 receipt](../reviews/2026-09-20-security-transition-validation.json)
pins native measurement (attempt 4: clang-cl 18.1.8, `equity-dev`, static
Debug; CTest `^SecurityTransition\.` 4/4) against an independent
Fraction-arithmetic comparator on five of fourteen oracle scenarios, all
passing with zero residual. Independent review clarified two numerical
contracts: successor value-before is now derived from the same product as
`validate_values`, so a correct-but-not-bit-identical mark within tolerance is
accepted rather than falsely rejected; and the bridge/accounting
reconciliation is restated from the four retained legs with tolerance scaled
to the maximum leg magnitude. A third fix made the absorbed-addition guard
`added(a,b)` symmetric at all three addition sites. Replay integration
remains later work, tracked in the checkpoint 13
[claims-aware replay design](../reviews/2026-09-20-iteration13-claims-aware-replay-design.md).

### Replay seam (checkpoint 13)

Checkpoint 13 was MINIMAL-CLOSED on 2026-09-20 covering T1 and T2 only: a
bounded `ClaimsBookState` with two-phase atomic `commit_security_transition`
and `commit_cash_claim_payment`, plus `compute_claims_nav`; and the
claims-aware replay seam `replay_scheduled_intents_with_events` with
`ReplayClaimsConfig`, `ReplayEventBatch` and `ReplayMandatoryEventPolicy`.
Ten book suites pass 62/62 natively (59 pre-existing plus 3 new). The
empty-event-batch path is pinned bit-identical to `replay_scheduled_intents`
at both interval and per-allocation level, and scenario A (a long
receivable transition plus a cash claim payment) preserves NAV and ledgers;
retired predecessor targets are rejected atomically. T3-T5 (engine
scenarios B/C, `atx-impl` allocation certification, and `atx-impl`
equity-book wiring) are deferred: the seam is not wired into `atx-impl`,
and equity-book output is unchanged. See the
[claims-aware replay design](../reviews/2026-09-20-iteration13-claims-aware-replay-design.md)
and
[checkpoint receipt](../reviews/2026-09-20-claims-aware-replay-validation.json)
(SHA-256 982edfc422961ee3bb4cd9c697712aae905aee81dd0340a66ec50beab7475072,
status `minimal_close_t1_t2_only`).
