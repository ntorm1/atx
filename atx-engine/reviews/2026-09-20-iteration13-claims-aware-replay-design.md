# Iteration 13: claims-aware state ownership and replay/accounting

Design only. No implementation, build, test or native run was performed. Checkpoint 12
delivered pure planners (`plan_security_transition`, `plan_cash_claim_payment`) that own
fixed-size plans and mutate nothing. This design supplies the missing owner: a bounded,
versioned claims/state ledger, an atomic commit, and the replay/allocation ordering that
keeps pending claims in NAV, out of settled cash, and out of turnover and trade fees.

Authored by a read-only design subagent; transcribed verbatim by the parent session, which
appended §11 (controller rulings on the design's open decisions).

## 1. Current state, with citations

**Replay owns exactly cash plus marked equities.**

- `atx-engine/src/book/replay.cpp:277-290` — the entire book state is `result.final_cash`
  (one `f64`), `result.final_tri_units` (N doubles) and the two N-double scratch arrays
  `start_values`/`end_values`. There is no claim, event, receivable or state-version field.
- `atx-engine/src/book/replay.cpp:291-294` — per observation `d`, `mark_holdings` at `d`,
  then `pretrade_nav = result.final_cash + start.assets`, then `require_nav`. This is the
  NAV identity: **settled cash + marked equities, nothing else**.
- `atx-engine/src/book/replay.cpp:137-163` — `required_price` / `mark_holdings`: every
  instrument with nonzero units requires a finite positive close at `d`; unused missing
  marks are ignored (`replay.cpp:153`). All held marks are validated **before** any callback.
- `atx-engine/src/book/replay.cpp:297-336` — the decision seam fires when
  `decision_periods[next] + execution_delay == d`; `ReplayAllocationState` is constructed at
  `replay.cpp:303-306` from cash, pretrade NAV, units, current marks, marked dollars,
  eligibility and preferences.
- `atx-engine/src/book/replay.cpp:209-211` — post-trade gate inside `apply_target`:
  `require_nav(cash + assets)` and `require_reconciled(cash + assets, nav - trade_cost)`.
- `atx-engine/src/book/replay.cpp:337-338` — `elapsed_days(session_keys[d], session_keys[d+1])`
  then `borrow_charge(start.shorts, days, config)`: **post-trade short dollars for the whole
  observed interval at actual calendar duration**.
- `atx-engine/src/book/replay.cpp:215-223` — `borrow_charge` = `short_dollars * (rate/basis) * days`,
  simple, no compounding, no loan identity.
- `atx-engine/src/book/replay.cpp:345-356` — `final_cash -= borrow`;
  `final_nav = final_cash + end.assets`; reconcile against
  `pretrade_nav + gross_pnl - trade_cost - borrow`; push `ReplayInterval`.
- The loop is `for (d = 0; d + 1 < dates; ++d)` (`replay.cpp:291`), so the **final observation
  is valuation-only**: no trade, fee, borrow or liquidation.
- `atx-engine/src/book/replay.cpp:404-414` — `HoldCurrent` rejects when `held && !decision_eligible`.
  Hold is already not a bypass; this design must keep it that way for *delivered* holdings.
- `atx-engine/include/atx/engine/book/replay.hpp:29-44` (`ReplayInterval`), `:71-84`
  (`ReplayAllocationState`), `:132-150` (`ReplayAllocation`) — none has a claims field.

**Allocation certifies the same two-term identity.**

- `atx-impl/src/equity_allocation.cpp:138-141` — `computed_nav = e.cash + assets` must reconcile
  with `e.pretrade_nav`. This is the identity that must become claims-aware.
- `atx-impl/src/equity_allocation.cpp:126-129` — a *held, eligible* name lacking 63 valid
  adjacent returns rejects allocation outright.
- `atx-impl/src/equity_allocation.cpp:145-174` — `identify_required_zeros` assigns
  `DecisionIneligible` / `RiskUnready` / `ExecutionCloseUnavailable` bits.
- `atx-impl/src/equity_allocation.cpp:246-352` — `certify` independently reproduces sizing,
  cash debits, fees and post-fee NAV; `:311-319` computes `posttrade_nav` and reconciles it with
  `pretrade_nav - trade_cost`; `:314` defines `actual_turnover = traded_dollars / pretrade_nav`.
- `atx-impl/src/equity_allocation.cpp:532-536` — mandatory exits that alone exceed the hard
  turnover budget fail the run. Delivered successors that must be closed inherit this gate.
- `atx-impl/src/equity_allocation.hpp:80-90` — `EquityAllocationExecution` carries `cash`,
  `pretrade_nav` and three N-spans; no claims, no retired flags.
- `atx-impl/src/stage_equity_book.cpp:489-491` — the stage builds `EquityAllocationExecution`
  directly from `ReplayAllocationState`; `:459-545` is the whole intent callback.
- `atx-impl/src/replay_report.cpp:428-441` selects the replay entry point;
  `:466-476` writes `ledger.csv`, `trades.csv`, `final_tri_units.csv`, `allocations.csv`.

**Checkpoint 12 planners, and the contract they hand us.**

- `atx-engine/include/atx/engine/book/security_transition.hpp:127-143` — "A caller must
  atomically commit every delta and record its IDs/sequence only if `from_state_version` still
  matches; no result authorizes replay to ignore earlier missing valuations."
- `:51-53` — the caller supplies `last_valuation_at_ns` / `next_valuation_at_ns`; the planner
  checks ordering only, never a calendar.
- `atx-engine/src/book/security_transition.cpp:205-218` — `validate_values` requires
  `predecessor_accounted_value == predecessor_tri_units * basis.predecessor.tri_price` and
  `successor_marked_value == successor_tri_units * basis.successor.tri_price`. The commit
  layer must therefore bind those basis prices to actual panel closes, bitwise.
- `atx-engine/src/book/security_transition.cpp:249-275` — bridge, accounting residual, and
  claim creation with `recognized_at_ns = state.next_valuation_at_ns`.
- `atx-engine/src/book/security_transition.cpp:330-338` — `to_state_version = from + 1`;
  `:347-350` — signed claim plus separate gross receivable/payable.
- `atx-engine/src/book/security_transition.cpp:356-381` — payment: `cash += amount`,
  `claim -> 0/settled`, `accounting_residual = cash_delta + claim_delta`.
- `atx-engine/src/book/security_transition.cpp:311-312` — both plans are trivially copyable.

## 2. The load-bearing decision: terminal predecessor marks

The current replay requires a valid mark at `d` for **every** held instrument before anything
else happens (`replay.cpp:292`). A terminal predecessor stops printing precisely at the event,
which is exactly how iteration 11 died at period 19. If valuation strictly precedes event
application, no real terminal event is ever replayable; if the event silently substitutes for
the mark, we have invented a price.

The design makes this an explicit, rejectable admission value, never a default:

```cpp
enum class TransitionPredecessorMark : atx::u8 {
  Unknown,                      // rejects
  RequireMarkAtEvent,           // predecessor must still print at d; ordinary valuation applies
  CarryLastAccountedValueV1     // predecessor keeps its completed d-1 accounted value
};
```

Under `CarryLastAccountedValueV1` the predecessor is exempt from the required-mark rule **at
`d` only**, **only** if it is the predecessor of an admitted event in this observation's batch,
and its accounted value is exactly the value produced by the *completed* valuation at `d-1`
(`end_values` from the previous loop iteration — no new N-array). The interval `(d-1, d]` price
move of that name is then not market P&L we observed; it lands in the mandatory valuation
bridge, reported separately, exactly as the iteration-12 design requires. No forward fill, no
synthesized quote, no options-basket substitute close. Every other held instrument still
requires a valid mark at `d`, and a predecessor with no admitted event still fails.

## 3. Proposed types and API

New pure-engine unit: `atx-engine/include/atx/engine/book/claims_state.hpp` +
`atx-engine/src/book/claims_state.cpp`. Fixed-size owned state; no dynamic allocation on any
success path; `Result`/`Status` and `ATX_TRY` exactly as the rest of `book/`.

```cpp
namespace atx::engine::book {

inline constexpr atx::usize kMaxTransitionEvents        = 64;
inline constexpr atx::usize kMaxPendingClaims           = 64;
inline constexpr atx::usize kMaxAppliedPayments         = 64;
inline constexpr atx::usize kMaxMandatoryMovements      = 256;  // <= 4 rows per event/payment
inline constexpr atx::usize kMaxEventsPerObservation    = 8;
inline constexpr atx::usize kMaxPaymentsPerObservation  = 8;
inline constexpr atx::usize kNoInstrument = std::numeric_limits<atx::usize>::max();

enum class ClaimSlotStatus : atx::u8 { Empty, Pending, Settled };

enum class MandatoryMovementKind : atx::u8 {
  Unknown, PredecessorRetirement, SuccessorDelivery, ClaimRecognition, ClaimSettlement
};

// One immutable ledger row. Never a trade: it contributes zero turnover and zero trade fee.
struct MandatoryMovement {
  MandatoryMovementKind kind{MandatoryMovementKind::Unknown};
  atx::usize period{};
  atx::usize instrument{kNoInstrument};
  atx::u64 event_id{}, cash_claim_id{}, payment_id{}, sequence{};
  atx::u64 from_state_version{}, to_state_version{};
  atx::f64 units_before{}, units_after{}, value_before{}, value_after{};
  atx::f64 settled_cash_delta{}, claim_delta{}, valuation_bridge{}, accounting_residual{};
  atx::f64 carried_mark{};                 // 0 unless CarryLastAccountedValueV1 applied
  atx::usize carried_mark_period{kNoInstrument};
};

struct PendingClaimSlot {
  PendingTransitionCashClaim claim{};
  ClaimSlotStatus status{ClaimSlotStatus::Empty};
};

struct RetiredRepresentation {
  atx::usize instrument{kNoInstrument};
  atx::u64 identity_epoch{}, event_id{}, retired_at_state_version{};
  atx::usize retired_at_period{};
};

// O(events + claims) retained state. No event-by-date, no per-universe history.
// Trivially copyable, so a whole-state stack snapshot is the atomicity mechanism.
struct ClaimsBookState {
  atx::u64 state_version{1};
  atx::u64 last_applied_sequence{};
  atx::f64 settled_cash{};                 // SETTLED only. Claims never live here.
  atx::usize event_count{}, claim_count{}, payment_count{}, movement_count{}, retired_count{};
  std::array<AppliedSecurityTransition, kMaxTransitionEvents> applied_events{};
  std::array<atx::u64,           kMaxPendingClaims>      known_claim_ids{};  // ascending
  std::array<PendingClaimSlot,   kMaxPendingClaims>      claims{};
  std::array<atx::u64,           kMaxAppliedPayments>    applied_payments{}; // ascending
  std::array<RetiredRepresentation, kMaxTransitionEvents> retired{};
  std::array<MandatoryMovement,  kMaxMandatoryMovements> movements{};
};

struct ClaimsNav {
  atx::f64 settled_cash{}, marked_equities{}, signed_pending_claims{};
  atx::f64 gross_receivable{}, gross_payable{}, nav{};
};

// nav = settled_cash + marked_equities + signed_pending_claims. Rejects nonfinite terms and
// nonpositive NAV. Gross legs are retained separately and never netted into cash.
[[nodiscard]] atx::core::Result<ClaimsNav>
compute_claims_nav(const ClaimsBookState& state, atx::f64 marked_equities);

struct ClaimsCommitReceipt {
  atx::u64 from_state_version{}, to_state_version{}, sequence{};
  atx::usize first_movement{}, movement_count{};
  ClaimsNav nav_after{};
};

// ATOMIC COMMIT CONTRACT.
// Preconditions: plan came from plan_security_transition with the same admission; spans have
// the panel's canonical length; `period` is the observation whose valuation produced the
// successor mark. Commits ONLY if plan.from_state_version == state.state_version, the event
// ID is unapplied, the claim ID is unknown, the predecessor is not already retired, the
// successor is not retired, bounded counts admit the new records, and every staged delta is
// finite. Two-phase: all fallible work writes a stack-local staged POD; the tail applies it
// with stores that cannot fail. On ANY error, `state`, `tri_units` and `marked_values` are
// byte-identical to entry and no movement/claim/receipt is published. Error-message
// allocation may throw; either error or exception leaves all four untouched. No I/O, no
// dynamic allocation on success, no exceptions across the boundary.
[[nodiscard]] atx::core::Result<ClaimsCommitReceipt> commit_security_transition(
    const SecurityTransitionPlan& plan, atx::usize period,
    std::span<atx::f64> tri_units, std::span<atx::f64> marked_values,
    std::span<atx::u8> retired_mask, ClaimsBookState& state);

// Same all-or-nothing contract. cash += amount, claim -> settled, zero accounting P&L.
// Rejects a stale version, a duplicate payment ID, an unknown/settled claim, and a payment
// whose reconstructed claim differs in ANY field from the retained slot.
[[nodiscard]] atx::core::Result<ClaimsCommitReceipt> commit_cash_claim_payment(
    const CashClaimPaymentPlan& plan, atx::usize period, ClaimsBookState& state);

// Borrowed, ascending, bounded views for the planners' guard spans and for the policy seam.
[[nodiscard]] std::span<const AppliedSecurityTransition> applied_events(const ClaimsBookState&);
[[nodiscard]] std::span<const atx::u64> known_claim_ids(const ClaimsBookState&);
[[nodiscard]] std::span<const PendingTransitionCashClaim> pending_claims_view(
    const ClaimsBookState&, std::span<PendingTransitionCashClaim, kMaxPendingClaims> scratch);

} // namespace atx::engine::book
```

`retired_mask` is one replay-owned `N`-byte array (the same order of magnitude as the existing
`PolicyCapture::eligibility`, `replay.cpp:286`) giving O(1) retired lookup in the hot path;
the authoritative record stays the bounded `retired[]` list, so retained state is O(E + C).

### Replay additions (opt-in; existing three entry points unchanged)

```cpp
enum class TransitionLoanTreatment : atx::u8 { Unknown, NoDischargeSuccessorContinuesV1 };
enum class ClaimFinancing        : atx::u8 { Unknown, NoneDisclosedV1 };
enum class BorrowBase            : atx::u8 { Unknown, MarkedShortEquityDollarsV1 };

struct ReplayClaimsConfig {
  TransitionAdmission admission{};                 // SyntheticFixture only, for now
  TransitionPredecessorMark predecessor_mark{TransitionPredecessorMark::Unknown};
  TransitionLoanTreatment loan{TransitionLoanTreatment::Unknown};
  ClaimFinancing claim_financing{ClaimFinancing::Unknown};
  BorrowBase borrow_base{BorrowBase::Unknown};
};

struct ReplayTransitionRequest { data::SecurityTransition event; data::TransitionBasis basis; };

struct ReplayEventBatch {                          // fixed size, owns no borrowed storage
  atx::usize transition_count{}, payment_count{};
  std::array<ReplayTransitionRequest, kMaxEventsPerObservation> transitions{};
  std::array<data::CashClaimPayment,  kMaxPaymentsPerObservation> payments{};
};

struct ReplayEventContext {                        // borrowed for the call only
  atx::usize period{};
  atx::i64 session_key{}, previous_session_key{};
  atx::u64 state_version{}, last_applied_sequence{};
  atx::f64 settled_cash{};
  std::span<const atx::f64> tri_units, previous_marked_dollars, current_marks;
  std::span<const PendingTransitionCashClaim> pending_claims;
  std::span<const atx::u8> retired_representation;
};

using ReplayMandatoryEventPolicy =
    std::function<atx::core::Result<ReplayEventBatch>(const ReplayEventContext&)>;

struct ReplayClaimsResult {
  ReplayPolicyResult policy;                       // replay + allocations, as today
  std::vector<MandatoryMovement> movements;        // reserved to kMaxMandatoryMovements
  ClaimsBookState final_state{};
};

// Same accounting, validation and all-or-error publication as replay_scheduled_intents.
// Events are requested at every observation with d >= 1, INCLUDING the final valuation-only
// observation. With an empty batch at every observation the results are bit-identical to
// replay_scheduled_intents; that equivalence is a required regression test.
[[nodiscard]] atx::core::Result<ReplayClaimsResult> replay_scheduled_intents_with_events(
    const alpha::Panel& research, std::span<const atx::i64> session_keys,
    std::span<const atx::usize> decision_periods, std::span<const atx::f64> preference_weights,
    const ReplayIntentPolicy& policy, const ReplayMandatoryEventPolicy& events,
    const ReplayConfig& config, const ReplayClaimsConfig& claims);
```

`ReplayAllocationState` gains (`replay.hpp:71-84`): `signed_pending_claims`,
`gross_receivable`, `gross_payable`, `std::span<const PendingTransitionCashClaim> pending_claims`,
`std::span<const atx::u8> retired_representation`. `cash` keeps its name and becomes
**settled cash**, documented; `pretrade_nav` becomes claims-aware. With no events both are
numerically unchanged.

`ReplayInterval` gains `settled_cash`, `signed_pending_claims`, `gross_receivable`,
`gross_payable`, `mandatory_value_bridge`, `mandatory_settled_cash`, `claim_recognized`,
`claim_settled`. `nav` becomes claims-aware; `cash` keeps its name with changed documentation.

## 4. Replay integration sequence, per observation `d`

Order is fixed. Each step names what it rejects.

**0. Event batch request (query only, no commit).** For `d >= 1`, call the event policy once
with `ReplayEventContext`. Rejects: `transition_count > kMaxEventsPerObservation`,
`payment_count > kMaxPaymentsPerObservation`, any event at `d == 0` (no completed previous
valuation), duplicate IDs inside the batch, any event whose predecessor/successor instrument
is outside the canonical axis, any admission mode other than the configured one. The batch is
**fetched before valuation only so that valuation knows which predecessors are exempt**; it is
committed in step 2.

**1. Valuation at `d`.** `mark_holdings` over the pre-event book, with the
`CarryLastAccountedValueV1` exemption of §2 applied to admitted predecessors only. Rejects:
any other held instrument with a missing/nonpositive/nonfinite close (unchanged
`replay.cpp:137-163`); a predecessor exemption requested under `RequireMarkAtEvent`; an
exemption for an instrument not in the batch; `Unknown` predecessor-mark policy. This step is
where "earlier unresolved valuations still fail" is enforced — an event never rescues a
missing mark for a name it does not retire.

**2. Mandatory transition application.** For each transition in batch order, build
`TransitionHoldingView` with `state_version`/`last_applied_sequence` from `ClaimsBookState`,
`predecessor_tri_units`/`predecessor_accounted_value` from the carried or current valuation,
`successor_tri_units`/`successor_marked_value` from the current valuation,
`settled_cash = state.settled_cash`, `last_valuation_at_ns = session_keys[d-1]`,
`next_valuation_at_ns = session_keys[d]`, and guard spans from `applied_events()` /
`known_claim_ids()`. Cross-check **bitwise** that `basis.predecessor.tri_price` equals the
close used for the carried/current predecessor valuation and `basis.successor.tri_price`
equals `close[d*N + successor]`, finite and positive. Then `plan_security_transition` →
`commit_security_transition`. Rejects: missing/nonpositive successor mark; basis price not
equal to the admitted panel close; stale state version; already-applied event; already-known
claim ID; retired predecessor or retired successor; sequence not exactly
`last_applied_sequence + 1`; bounded counts exhausted; any planner rejection (embedded/unknown
TRI coverage, entitlement mismatch, absorbed leg, unreconciled residual). After commit,
re-mark exactly the two touched coordinates from the panel close at `d` and require equality
with `plan.successor_value_after` / exact `0.0` predecessor value.

**3. Claim recognition.** The commit already inserted the signed claim with
`recognized_at_ns = session_keys[d]`. Recompute `signed_pending_claims`, `gross_receivable`,
`gross_payable`. Rejects: nonfinite claim amount; a claim whose currency is not USD; claim
slots exhausted. Receivables and payables are **never** netted into `settled_cash`.

**4. Claims-aware pretrade NAV.** `compute_claims_nav(state, marked_equities)`;
`pretrade_nav = settled_cash + marked_equities + signed_pending_claims`; `require_nav`.
Rejects: nonfinite or nonpositive NAV.

**5. Allocation**, only when `d` is a delayed execution period and `d + 1 < dates`. Build
`ReplayAllocationState` with the claims fields and `retired_representation`. Rejects, in
addition to everything today: a nonzero `TargetWeight` or a `HoldCurrent` on a **retired**
instrument, regardless of that instrument's original decision eligibility — a delayed
predecessor target may not reopen a retired representation; a `HoldCurrent` on a delivered
successor that is not decision-eligible (already `replay.cpp:404-414`, explicitly reaffirmed);
an allocation whose cash arithmetic assumes claims are spendable.

**6. Trades.** `apply_target` unchanged, except that the post-trade gate at
`replay.cpp:209-211` becomes
`settled_cash + marked_equities + signed_pending_claims == pretrade_nav - trade_cost`.
Claims are constant across the trade by construction. Rejects: unchanged.

**7. Payment settlement.** For each payment in the batch: build `CashClaimPaymentView` from the
retained slot and `applied_payments`, `plan_cash_claim_payment` → `commit_cash_claim_payment`.
Rejects: stale version; duplicate payment ID; partial or mismatched amount; allocation before
recognition or after the knowledge cutoff; settled claim. Because settlement follows the trade,
**cash recognized and paid at `d` is first spendable at `d+1`** — the conservative choice, and
it must be stated in the recipe. NAV is unchanged across settlement; assert it.

**8. Borrow.** `elapsed_days(session_keys[d], session_keys[d+1])` and
`borrow_charge(start.shorts, days, config)` unchanged, with `start.shorts` now the
**post-trade, post-settlement marked short equity dollars** (`replay.cpp:128-135`). Claims,
receivable or payable, contribute nothing to the borrow base. §5 states the event-time choice.

Interval reconciliation becomes
`nav_{d+1} - nav_d = gross_pnl + mandatory_value_bridge - trade_cost - borrow_cost`,
with claim settlement P&L-neutral, checked by the existing `require_reconciled` tolerance.

## 5. Explicit event-time borrow treatment

The current convention charges post-trade short dollars for the whole observed interval at
actual calendar duration (`replay.cpp:215-223`, `:337-338`). Across a transition at `d`:

- Interval `(d-1, d]` was already charged at `d-1` on the **predecessor** short dollars. The
  conversion at `d` does not retroactively shorten that interval, so no gap and no double count.
- Interval `(d, d+1]` is charged at `d` on the **successor** short dollars.

What the design refuses to infer: `TransitionLoanTreatment::NoDischargeSuccessorContinuesV1`
is the only admitted value and asserts, for accounting only, that the short obligation
continues in the successor representation. It does **not** assert a loan transfer, recall,
termination, re-rate, locate or delivery. If the stock ratio shrinks the short (PCS: one-for-two),
the borrow base falls at `d`; that is a representation effect of the admitted terms, **not**
evidence of a cheaper or partially discharged loan, and the movement ledger records the
pre- and post-event short dollars so the change is auditable rather than silent. A punitive
"floor the base at the predecessor level" variant is deliberately **not** implemented: it would
require loan-term evidence we do not have.

`ClaimFinancing::NoneDisclosedV1` charges nothing on a pending payable and declares that this
**understates** financing cost rather than asserting the cost is zero. SIFMA MSLA §8 and the
IBKR payment-in-lieu description both keep the short's distribution obligation alive, and the
OIC/OCC cash-merger FAQ shows the obligation converting into a fixed cash deliverable rather
than evaporating — so the payable is retained at face, signed, in NAV, and never netted against
short equity dollars (`BorrowBase::MarkedShortEquityDollarsV1`). All four enums reject `Unknown`
whenever the event policy is non-empty.

## 6. Ledger conventions

| Ledger | Contents | Explicitly excluded |
| --- | --- | --- |
| Turnover (`traded_dollars`, `actual_turnover`) | Only `apply_target` dollar deltas from step 6 | Transition deliveries, retirements, claim recognition, claim settlement — all contribute exactly `0.0` |
| Trade fee (`trade_cost`) | Only `traded_dollars * trade_bps` | No fee on any mandatory movement; real transfer taxes/agent fees are declared **absent**, not zero-by-evidence |
| Financing (`borrow_cost`) | Post-trade marked short **equity** dollars × rate × actual days | Pending claims of either sign; payable financing declared absent and disclosed |
| Mandatory movement (`movements.csv`) | One row per retirement / delivery / recognition / settlement: IDs, revision, sequence, from/to state version, instrument indices, unit and value before/after, valuation bridge, accounting residual, carried mark and its period | Never appears in `trades.csv`; never in turnover or fees |
| Claim (`claims.csv`) | One row per claim lifecycle edge: signed amount, gross receivable, gross payable, currency, entitlement/recognition/allocation clocks, evidence digest, event and payment IDs, state versions | Never merged into `ledger.csv` cash |
| Interval (`ledger.csv`) | Adds `settled_cash`, `signed_pending_claims`, `gross_receivable`, `gross_payable`, `mandatory_value_bridge`, `mandatory_settled_cash` | `nav` becomes claims-aware; `cash` documented as settled |
| Allocation certificate | `pretrade_nav` becomes claims-aware; new `pretrade_settled_cash`, `pretrade_signed_claims`, `retired_target_rejections` | Turnover denominator is the claims-aware NAV |

## 7. Synthetic fixture end-to-end test plan

`atx-engine/tests/book/book_claims_replay_test.cpp`, three grouped `TEST`s matching the
checkpoint-12 style. Three instruments — `0 = PRED`, `1 = SUCC`, `2 = OTHER`; six observations
`t0..t5`, one calendar day apart; `SyntheticFixture` admission throughout. `PRED` prints
`t0..t2` and is **absent at `t3..t5`** (the real terminal shape). `SUCC` is absent `t0..t2` and
prints `t3..t5` — no invented history. `OTHER` prints everywhere. One transition at `t3`
(stock ratio `1/2`, cash `40491/10000` per predecessor share), one payment at `t4`.
`initial_nav = 1000`, `trade_bps = 0`, `annual_borrow_bps = 0` except where noted.

**A. Long receivable.** Decision at `t1` executes at `t2` buying `PRED` and `OTHER`. At `t3`:
- `PRED` valuation is exempt and carries its `t2` accounted value; `OTHER` still requires `t3`.
- Assert `pretrade_nav == settled_cash + marked_equities + signed_pending_claims` exactly by
  the replay tolerance, and that it equals the pre-event NAV plus `plan.valuation_bridge`.
- Assert `interval[t3].traded_dollars == 0.0` and `trade_cost == 0.0`; `trades` contains no row
  at `t3`; `movements` contains exactly two rows (retirement, delivery) plus one recognition.
- Assert `final_tri_units[PRED] == 0.0` bitwise and `retired_mask[PRED] == 1`.
- At `t4` settlement: `settled_cash` rises by exactly `claim.amount`; `signed_pending_claims`
  becomes `0.0`; NAV immediately before and after settlement is identical; turnover unchanged.
- At `t5` (valuation-only): `final_nav == settled_cash + marked_equities + signed_pending_claims`,
  and `sum(gross_pnl + mandatory_value_bridge - trade_cost - borrow_cost) == final_nav - initial_nav`.

**B. Short payable with borrow.** Same fixture with the `PRED` position negated and
`annual_borrow_bps = 365`. Assert the claim is negative, `gross_payable > 0`,
`gross_receivable == 0`, and that settlement **debits** cash. Assert both
`interval[t2].borrow_cost > 0` (predecessor short over `(t2,t3]`) and
`interval[t3].borrow_cost > 0` (successor short over `(t3,t4]`) — the conversion neither
discharges nor gaps the loan. Assert the payable contributed nothing to either charge.

**C. Rejections are atomic.** Each sub-case returns an error and publishes no partial state,
no movement and no claim: missing `SUCC` mark at `t3`; `basis.successor.tri_price` not equal to
the panel close; `OTHER` missing at `t3` (an event does not rescue an unrelated valuation);
`PRED` missing at `t3` with **no** admitted event; a stale `expected_state_version`; a repeated
`event_id`; a repeated `payment_id`; a partial payment amount; an event at `t0`; a batch of
`kMaxEventsPerObservation + 1`; a `t4` allocation returning a nonzero `TargetWeight` for the
retired `PRED`; a `t4` allocation returning `HoldCurrent` for a decision-ineligible `SUCC`;
`Unknown` in any of the four convention enums. Plus the equivalence regression: an always-empty
batch reproduces `replay_scheduled_intents` bit-for-bit on the same fixture.

## 8. What real PCS admission would still require (unchanged)

`SyntheticFixture` is the only admitted mode; the fixture path cannot be pointed at the archived
2013 panel. A real 2013-05-01 MetroPCS→TMUS admission still requires, at minimum:

1. A reviewed, time-bounded vendor→issuer mapping on **both** sides — archive ID `146189` to the
   old class and an admitted canonical axis position for the successor — with source and axis
   hashes, plus identity-epoch handling if the vendor reuses an ID.
2. Adjustment-factor coverage proving the stock, cash and vendor-continuity components are each
   `ExcludedFromTri` at the exact boundary; embedded, partial or unknown coverage rejects.
3. A predecessor basis anchored before the applied components and covering intervening actions,
   with any retrospective revision identified.
4. An observed successor mark at the resolving valuation, from the same admitted panel, matching
   the basis bitwise. No options-basket quote, forward fill or synthesized close.
5. Entitlement in pre-split physical shares, with holder aggregation, fractional-share
   treatment, rounding and withholding — the research reinvested-share-equivalent convention
   cannot certify any of these.
6. Effective-time normalization that preserves the certificate's "4:01 p.m. Eastern Standard
   Time on April 30, 2013" wording rather than a guessed UTC instant, plus a decided boundary
   phase relative to the observation grid.
7. Historical source availability, publication and ingestion times sufficient for `StrictAsOf`;
   otherwise `RetrospectiveReconstruction` with a disclosed hindsight statement that still must
   not feed later event knowledge into earlier preferences, eligibility or risk.
8. For the measured short: evidenced stock-loan terms — transfer, recall, termination, rate —
   and the actual cash-distribution obligation with allocation dates and amounts.

**PCS admission remains REJECTED.** Nothing in this design supplies a price, a proceed, a
continuity factor, a fill or a mark. The failed iteration-11 run, its window, panels and
receipts remain unchanged.

## 9. Research citations

- [Zipline `zipline/finance/ledger.py`](https://github.com/quantopian/zipline/blob/master/zipline/finance/ledger.py)
  — declared dividends are *earned* on ex-date into `_unpaid_dividends` / `_unpaid_stock_dividends`
  keyed by **pay date**, and only `pay_dividends` converts them to cash on that date
  ("Store the earned dividends so that they can be paid on the dividends' pay_dates";
  "Mark these dividends as paid by dropping them from our unpaid"). A production simulator keeps
  the receivable structurally separate from cash — this design's claim ledger is the same shape.
- [Cvxportfolio market simulator](https://www.cvxportfolio.com/en/stable/simulator.html)
  — a single cash account whose "returns are the risk-free rate"; there is no receivable,
  claim or corporate-action settlement concept. Our nearest accounting reference does **not**
  model claims, so the claim ledger must be documented as our own convention, not assumed.
- [Cvxportfolio holding costs](https://www.cvxportfolio.com/en/1.3.1/costs.html)
  — financing depends on actual time held including weekends, and dividends must not be counted
  twice when returns already include them. Supports both the actual-calendar borrow and the
  TRI-coverage admission gate that rejects `EmbeddedInTri` components.
- [17 CFR 270.2a-4](https://www.law.cornell.edu/cfr/text/17/270.2a-4)
  — "Dividends receivable shall be included to date of calculation either at ex-dividend dates
  or record dates, as appropriate", and "Changes in holdings of portfolio securities shall be
  reflected no later than in the first calculation on the first business day following the
  trade date." Receivables belong in NAV at entitlement, not at cash receipt — exactly the
  pending-claim rule, and authority for recognizing the claim at the event, not at payment.
- [SIFMA MSLA guidance notes, section 8](https://www.sifma.org/wp-content/uploads/2024/06/Master_Securities_Loan_Agreement_MSLA-Guidance_Notes_2000_Version.pdf)
  — cash distributions on loaned securities are transferred to the lender and noncash
  distributions are added to the loan. A short's obligation survives the corporate action;
  numerical position netting is not loan discharge.
- [OIC/OCC "Splits, Mergers, Spinoffs & Bankruptcies"](https://www.optionseducation.org/referencelibrary/faq/splits-mergers-spinoffs-bankruptcies)
  — "When an underlying security is converted into a right to receive a fixed amount of cash,
  options on that security will generally be adjusted to require the delivery upon exercise of
  a fixed amount of cash", with the short side then owing that cash and OCC accelerating
  expiration for all-cash deliverables (memo #30047). The obligation follows the transition
  terms into cash; it does not vanish when the ticker stops printing. (Options clearing
  mechanics corroborate the accounting direction; they are not stock-loan terms.)
- [IBKR payment in lieu of dividends](https://www.interactivebrokers.com/campus/glossary-terms/payment-in-lieu-of-dividends/)
  — the short holder owes the distribution. Retained from the checkpoint-12 documentation.

## 10. Implementation tasks, in dependency order

**T1 — pure engine: bounded claims state and atomic commit.** New
`atx-engine/include/atx/engine/book/claims_state.hpp`, new `atx-engine/src/book/claims_state.cpp`,
new `atx-engine/tests/book/book_claims_state_test.cpp`; `atx-engine/CMakeLists.txt` only if the
source list is explicit. Delivers `ClaimsBookState`, `MandatoryMovement`, `ClaimsNav`,
`compute_claims_nav`, `commit_security_transition`, `commit_cash_claim_payment`, the guard-span
views and retired-mask maintenance. Grouped tests: signed commit + payment round trip; bounded
count and duplicate-ID exhaustion; byte-identical state after every rejection. **No atx-impl.**

**T2 — pure engine: claims-aware replay seam.** `atx-engine/include/atx/engine/book/replay.hpp`,
`atx-engine/src/book/replay.cpp`, new `atx-engine/tests/book/book_claims_replay_test.cpp`.
Adds `ReplayClaimsConfig`, the event-policy seam, the §4 ordering including the predecessor-mark
exemption, claims-aware NAV and `ReplayAllocationState`, retired-target rejection, the §5 borrow
conventions and `ReplayClaimsResult`. Includes the empty-batch bit-identity regression.

**T3 — engine fixture end-to-end and documentation.** Completes scenarios A/B/C in
`book_claims_replay_test.cpp`; updates `atx-engine/docs/SECURITY_TRANSITIONS.md` (replay adapter
section) and adds a checkpoint-13 section to `atx-engine/docs/PLATFORM_PROGRESS.md`. Still no
atx-impl; PCS admission restated as rejected.

**T4 — atx-impl: claims-aware allocation certification.** `atx-impl/src/equity_allocation.hpp`,
`atx-impl/src/equity_allocation.cpp`, `atx-impl/tests/`. `EquityAllocationExecution` gains
`settled_cash`, `signed_pending_claims`, `gross_receivable`, `gross_payable` and
`retired_representation`; `validate_execution_and_count` (`equity_allocation.cpp:138-141`)
checks the three-term identity; `identify_required_zeros` gains a `RetiredRepresentation` reason
bit; `certify` holds claims constant across the trade, rejects any nonzero target on a retired
name, and keeps the claims-aware NAV as the turnover and exposure denominator.

**T5 — atx-impl: `equity-book` wiring and report.** `atx-impl/src/stage_equity_book.cpp`,
`atx-impl/src/replay_report.hpp`, `atx-impl/src/replay_report.cpp`,
`atx-impl/docs/EQUITY_BOOK_CONSTRAINED.md`. Adds an opt-in `--transitions <fixture.json>` flag
(synthetic namespace only; every real-data mode rejects at parse), the event-policy adapter,
`movements.csv` and `claims.csv` writers, new `ledger.csv` columns, and recipe/certificate
fields naming all four conventions. With the flag absent the stage must be byte-identical to
today's output.

## 11. Controller rulings on the design's open decisions (parent session, 2026-09-20)

1. **Payment settlement after trades** (cash recognized and paid at `d` first spendable at
   `d+1`): accepted as the conservative first-slice convention. Must be named in the recipe.
   Cost if wrong: one observation of understated deployable cash on payment days.
2. **`CarryLastAccountedValueV1`** as an explicit, rejectable admission value (never default):
   accepted. Cost if wrong: one interval of stale predecessor mark in NAV, disclosed in the
   mandatory bridge and the movement ledger.
3. **Payable financing `NoneDisclosedV1`**: accepted; disclosure of understatement is mandatory
   in the recipe. Cost if wrong: understated financing on short payables until rate evidence exists.
4. **`ReplayInterval.cash` keeps its name**, documented as settled cash; `ledger.csv` header
   unchanged, new columns appended. Cost if wrong: reader confusion, no numerical effect.
5. **Transitions at the final valuation-only observation are REJECTED** in this slice
   (`d == dates - 1` returns an error). The design text in §3 saying events are requested
   "INCLUDING the final valuation-only observation" is overridden by this ruling; the batch is
   requested for `1 <= d < dates - 1` only. Cost if wrong: a fixture ending on an event day
   must extend by one observation.
6. **Bounded maxima** (`kMaxTransitionEvents = 64` etc.) accepted as fixture-sized placeholders;
   document as such. Cost if wrong: a later real event calendar requires a constant bump and
   re-measurement.
