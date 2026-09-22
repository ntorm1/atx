#pragma once

#include <array>
#include <limits>
#include <span>
#include <type_traits>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/security_transition.hpp"
#include "atx/engine/data/security_transition.hpp"

namespace atx::engine::book {

// Fixture-sized bounds (iteration-13 design §11 ruling 6). These are explicit
// placeholders for the synthetic event calendar, NOT a measured production
// capacity: a real event calendar requires a deliberate constant bump plus
// re-measurement. Exhausting any of them rejects the commit; nothing grows.
inline constexpr atx::usize kMaxTransitionEvents = 64;
inline constexpr atx::usize kMaxPendingClaims = 64;
inline constexpr atx::usize kMaxAppliedPayments = 64;
inline constexpr atx::usize kMaxMandatoryMovements = 256; // <= 4 rows per event/payment
inline constexpr atx::usize kMaxEventsPerObservation = 8;
inline constexpr atx::usize kMaxPaymentsPerObservation = 8;

// Sentinel for a movement row that touches no instrument axis position (a claim
// recognition or settlement). It doubles as the "no carried mark period"
// sentinel on `MandatoryMovement::carried_mark_period`, which T2 fills.
inline constexpr atx::usize kNoInstrument = std::numeric_limits<atx::usize>::max();

enum class ClaimSlotStatus : atx::u8 { Empty, Pending, Settled };

enum class MandatoryMovementKind : atx::u8 {
  Unknown, PredecessorRetirement, SuccessorDelivery, ClaimRecognition, ClaimSettlement
};

// One immutable ledger row. Never a trade: it contributes zero turnover and
// zero trade fee. Event-level figures (`valuation_bridge`, `accounting_residual`)
// are reported exactly once per commit, on the SuccessorDelivery row for a
// transition and on the ClaimSettlement row for a payment, so summing a column
// over the ledger never double counts.
struct MandatoryMovement {
  MandatoryMovementKind kind{MandatoryMovementKind::Unknown};
  atx::usize period{};
  atx::usize instrument{kNoInstrument};
  atx::u64 event_id{};
  atx::u64 cash_claim_id{};
  atx::u64 payment_id{};
  atx::u64 sequence{};
  atx::u64 from_state_version{};
  atx::u64 to_state_version{};
  atx::f64 units_before{};
  atx::f64 units_after{};
  atx::f64 value_before{};
  atx::f64 value_after{};
  atx::f64 settled_cash_delta{};
  atx::f64 claim_delta{};
  atx::f64 valuation_bridge{};
  atx::f64 accounting_residual{};
  atx::f64 carried_mark{}; // 0 unless CarryLastAccountedValueV1 applied (T2).
  atx::usize carried_mark_period{kNoInstrument};
};

struct PendingClaimSlot {
  PendingTransitionCashClaim claim{};
  ClaimSlotStatus status{ClaimSlotStatus::Empty};
};

struct RetiredRepresentation {
  atx::usize instrument{kNoInstrument};
  atx::u64 identity_epoch{};
  atx::u64 event_id{};
  atx::u64 retired_at_state_version{};
  atx::usize retired_at_period{};
};

// O(events + claims) retained state. No event-by-date index, no per-universe
// history. `known_claim_ids[i]` and `claims[i]` are maintained in lockstep and
// in ascending claim-ID order, so a retained slot's guard ID and its record
// always share an index; `applied_events` and `applied_payments` are likewise
// ascending and unique, which is exactly what the checkpoint-12 planners
// require of the guard spans they borrow.
struct ClaimsBookState {
  atx::u64 state_version{1};
  atx::u64 last_applied_sequence{};
  atx::f64 settled_cash{}; // SETTLED only. Claims never live here.
  atx::usize event_count{};
  atx::usize claim_count{};
  atx::usize payment_count{};
  atx::usize movement_count{};
  atx::usize retired_count{};
  std::array<AppliedSecurityTransition, kMaxTransitionEvents> applied_events{};
  std::array<atx::u64, kMaxPendingClaims> known_claim_ids{};
  std::array<PendingClaimSlot, kMaxPendingClaims> claims{};
  std::array<atx::u64, kMaxAppliedPayments> applied_payments{};
  std::array<RetiredRepresentation, kMaxTransitionEvents> retired{};
  std::array<MandatoryMovement, kMaxMandatoryMovements> movements{};
};

// Trivial copyability is load-bearing: it is what lets a caller (and the tests)
// take a byte snapshot of the whole ledger to prove a rejection changed nothing.
static_assert(std::is_trivially_copyable_v<ClaimsBookState>);
static_assert(std::is_trivially_copyable_v<MandatoryMovement>);

struct ClaimsNav {
  atx::f64 settled_cash{};
  atx::f64 marked_equities{};
  atx::f64 signed_pending_claims{};
  atx::f64 gross_receivable{};
  atx::f64 gross_payable{};
  atx::f64 nav{};
};

// nav = settled_cash + marked_equities + signed_pending_claims. Rejects
// nonfinite terms and a nonpositive NAV. The gross legs are retained separately
// and are never netted into settled cash or against each other.
[[nodiscard]] atx::core::Result<ClaimsNav>
compute_claims_nav(const ClaimsBookState& state, atx::f64 marked_equities);

// `nav_after.marked_equities` is the marked-value span the commit observed, so
// the payment commit — which sees no equity book — reports 0.0 there and its
// `nav` is then the settled-cash plus signed-claims subtotal only. The identity
// a payment certifies is that this subtotal is unchanged by settlement.
struct ClaimsCommitReceipt {
  atx::u64 from_state_version{};
  atx::u64 to_state_version{};
  atx::u64 sequence{};
  atx::usize first_movement{};
  atx::usize movement_count{};
  ClaimsNav nav_after{};
};

// ATOMIC COMMIT CONTRACT.
// Preconditions: `plan` came from `plan_security_transition` under the same
// admission; the three spans share the panel's canonical length; `period` is the
// observation whose valuation produced the successor mark.
// Commits ONLY if `plan.from_state_version == state.state_version`,
// `plan.to_state_version == state.state_version + 1`, the sequence is exactly
// `state.last_applied_sequence + 1`, the event ID is unapplied, the claim ID is
// unknown, both instrument indices are distinct and in range, neither is
// retired, the spans still hold the plan's "before" units and values, the plan's
// cash matches `state.settled_cash`, and bounded counts admit the new records.
// It ALSO rejects a projected claims-aware NAV that is nonfinite or nonpositive:
// the design puts that gate on `compute_claims_nav` alone, but a commit is the
// only place the post-event NAV can still be refused, so the projection is built
// from the staged values and checked before any store. A commit therefore never
// publishes a book whose NAV the caller would immediately have to reject.
// Two-phase: every fallible check writes a stack-local staged POD; the tail
// applies it with stores that cannot fail. On ANY error, `state`, `tri_units`,
// `marked_values` and `retired_mask` are byte-identical to entry and no
// movement, claim or receipt is published. Error-message allocation may throw;
// either error or exception leaves all four untouched. No I/O, no dynamic
// allocation on the success path, no exceptions across the boundary.
// Movement rows: PredecessorRetirement, SuccessorDelivery, and — only when the
// plan carries a nonzero claim — ClaimRecognition. A zero-claim event therefore
// occupies no claim slot and reserves no claim ID; `plan_security_transition`
// omits zero claims entirely and the event-ID guard already prevents replaying
// the same event, so no identity is lost.
[[nodiscard]] atx::core::Result<ClaimsCommitReceipt> commit_security_transition(
    const SecurityTransitionPlan& plan, atx::usize period, std::span<atx::f64> tri_units,
    std::span<atx::f64> marked_values, std::span<atx::u8> retired_mask, ClaimsBookState& state);

// Same all-or-nothing contract. settled_cash += amount, claim -> Settled, zero
// accounting P&L, one ClaimSettlement row. Rejects a stale version, a wrong
// sequence, a duplicate payment ID, an unknown or already-settled claim, a
// payment whose reconstructed claim differs in ANY field from the retained slot,
// a `claim_after` whose identity fields differ from the retained slot, and
// exhausted payment or movement capacity. The settled slot and the ledger row
// are DERIVED from the retained claim (amount 0.0, settled true), never copied
// out of the plan, so no plan can rewrite which claim the state says it settled.
[[nodiscard]] atx::core::Result<ClaimsCommitReceipt> commit_cash_claim_payment(
    const CashClaimPaymentPlan& plan, atx::usize period, ClaimsBookState& state);

// Borrowed, ascending, bounded views for the planners' guard spans and for the
// policy seam. Each aliases `state` and must not outlive it.
[[nodiscard]] std::span<const AppliedSecurityTransition> applied_events(
    const ClaimsBookState& state);
[[nodiscard]] std::span<const atx::u64> known_claim_ids(const ClaimsBookState& state);
[[nodiscard]] std::span<const atx::u64> applied_payments(const ClaimsBookState& state);

// Copies the still-pending claims into `scratch` in ascending claim-ID order and
// returns the filled prefix. Settled slots are omitted; the returned span
// aliases `scratch`, not `state`.
[[nodiscard]] std::span<const PendingTransitionCashClaim> pending_claims_view(
    const ClaimsBookState& state,
    std::span<PendingTransitionCashClaim, kMaxPendingClaims> scratch);

} // namespace atx::engine::book
