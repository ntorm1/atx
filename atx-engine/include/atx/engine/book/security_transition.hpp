#pragma once

#include <optional>
#include <span>

#include "atx/core/error.hpp"
#include "atx/engine/data/security_transition.hpp"

namespace atx::engine::book {

enum class TransitionMode : atx::u8 {
  Unknown, SyntheticFixture, StrictAsOf, RetrospectiveReconstruction
};
enum class TransitionUnitConvention : atx::u8 {
  Unknown, ResearchReinvestedShareEquivalent, PhysicalShares
};
enum class TransitionClaimValuation : atx::u8 { Unknown, FixedUsdFaceValue };

struct TransitionAdmission {
  TransitionMode mode{TransitionMode::Unknown};
  TransitionUnitConvention units{TransitionUnitConvention::Unknown};
  TransitionClaimValuation claim_valuation{TransitionClaimValuation::Unknown};
  std::optional<atx::i64> knowledge_cutoff_ns;
  // Combined number of borrowed applied-event/claim IDs (or payment IDs).
  // Zero admits no plan. No copies of these spans are retained by the result.
  atx::usize max_guard_records{};
};

struct AppliedSecurityTransition {
  atx::u64 event_id{};
  atx::u32 revision{};
  atx::u64 sequence{};
};

struct TransitionHoldingView {
  atx::u64 state_version{};
  atx::u64 last_applied_sequence{};
  data::TransitionSecurity predecessor;
  data::TransitionSecurity successor;
  data::TransitionDigest source_artifact_sha256{};
  data::TransitionDigest axes_sha256{};
  data::TransitionDigest adjustment_recipe_sha256{};
  atx::f64 predecessor_tri_units{};
  atx::f64 predecessor_accounted_value{};
  atx::f64 successor_tri_units{};
  atx::f64 successor_marked_value{};
  atx::f64 settled_cash{};
  bool predecessor_retired{};
  // Caller supplies adjacent required valuation boundaries. The planner checks
  // their ordering, not an external exchange calendar or omitted observations.
  std::optional<atx::i64> last_valuation_at_ns;
  std::optional<atx::i64> next_valuation_at_ns;
  // Guard spans contain unique nonzero IDs in ascending ID order. Event
  // sequences must be nonzero and no later than last_applied_sequence.
  std::span<const AppliedSecurityTransition> applied_events;
  std::span<const atx::u64> known_cash_claim_ids;
};

struct PendingTransitionCashClaim {
  atx::u64 cash_claim_id{};
  atx::u64 event_id{};
  atx::u32 event_revision{};
  atx::u64 created_state_version{};
  atx::u64 created_sequence{};
  data::TransitionCurrency currency{data::TransitionCurrency::Unknown};
  atx::f64 amount{};
  std::optional<atx::i64> entitlement_at_ns;
  std::optional<atx::i64> recognized_at_ns;
  data::TransitionEvidence evidence;
  bool settled{};
};

// Fixed-size owned output. Terms, basis and admission are retained verbatim so
// callers can bind provenance without keeping borrowed arrays or price panels.
struct SecurityTransitionPlan {
  data::SecurityTransition event;
  data::TransitionBasis basis;
  TransitionAdmission admission;
  atx::u64 from_state_version{};
  atx::u64 to_state_version{};
  atx::f64 predecessor_units_before{};
  atx::f64 predecessor_units_after{};
  atx::f64 successor_units_before{};
  atx::f64 successor_units_added{};
  atx::f64 successor_units_after{};
  atx::f64 converted_predecessor_shares{};
  atx::f64 successor_share_equivalents{};
  atx::f64 removed_predecessor_value{};
  atx::f64 successor_value_before{};
  atx::f64 successor_value_after{};
  atx::f64 expected_successor_value_added{};
  atx::f64 actual_successor_value_added{};
  atx::f64 representation_residual{};
  atx::f64 signed_cash_claim{};
  atx::f64 gross_cash_receivable{};
  atx::f64 gross_cash_payable{};
  atx::f64 cash_before{};
  atx::f64 cash_after{};
  atx::f64 valuation_bridge{};
  atx::f64 accounting_residual{};
  bool retire_predecessor{};
  std::optional<PendingTransitionCashClaim> claim;
};

struct CashClaimPaymentView {
  atx::u64 state_version{};
  atx::u64 last_applied_sequence{};
  atx::f64 settled_cash{};
  PendingTransitionCashClaim claim;
  std::span<const atx::u64> applied_payment_ids; // Unique ascending nonzero IDs.
};

struct CashClaimPaymentPlan {
  data::CashClaimPayment payment;
  TransitionAdmission admission;
  PendingTransitionCashClaim claim_before;
  PendingTransitionCashClaim claim_after;
  atx::u64 from_state_version{};
  atx::u64 to_state_version{};
  atx::f64 cash_before{};
  atx::f64 cash_after{};
  atx::f64 actual_cash_delta{};
  atx::f64 claim_delta{};
  atx::f64 accounting_residual{};
};

// Pure planners: immutable inputs, no I/O, no mutation, no successful-result
// dynamic allocation. Unknown/default contracts and every real-data admission
// mode reject. Synthetic results do not authenticate external evidence, physical
// entitlement, historical availability, loan transfer, fill or spendable cash.
//
// Arithmetic V1: old q*(old TRI/raw), ratio, entitled shares*cash ratio,
// successor shares*(new raw/TRI), then add to existing successor units and mark.
// Each nonzero leg must survive representation with its sign. Reconciliations
// allow 64*f64-epsilon*max(abs(actual),abs(expected)), without an absolute floor
// or any residual cash plug. This intentionally rejects poorly conditioned
// additions, including payment into cash too large to preserve the claim leg.
// Pure plans carry no trade, fee or borrow movement. A caller must atomically
// commit every delta and record its IDs/sequence only if from_state_version still
// matches; no result authorizes replay to ignore earlier missing valuations.
// Calls are independent/thread-safe if callers keep borrowed inputs immutable.
// Error-message allocation may throw; either error or exception leaves inputs
// untouched. Sequence is exactly last_applied_sequence + 1; versions cannot wrap.
[[nodiscard]] atx::core::Result<SecurityTransitionPlan> plan_security_transition(
    const data::SecurityTransition& event, const data::TransitionBasis& basis,
    const TransitionHoldingView& state, const TransitionAdmission& admission);

// Requires a full signed amount identical to the outstanding claim, allocation
// at/after recognition and a strictly later sequence. Zero claims are omitted by
// the transition planner and cannot be paid. Unknown payments remain pending.
[[nodiscard]] atx::core::Result<CashClaimPaymentPlan> plan_cash_claim_payment(
    const data::CashClaimPayment& payment, const CashClaimPaymentView& state,
    const TransitionAdmission& admission);

} // namespace atx::engine::book
