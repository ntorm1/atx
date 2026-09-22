#include "atx/engine/book/security_transition.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>

namespace atx::engine::book {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

constexpr atx::u64 kLargestExactInteger = atx::u64{1} << 53;
constexpr atx::f64 kRelativeToleranceV1 = 64.0 * std::numeric_limits<atx::f64>::epsilon();

bool present(const data::TransitionDigest& digest) noexcept {
  return std::any_of(digest.begin(), digest.end(), [](atx::u8 byte) { return byte != 0; });
}

Status invalid(const char* message) {
  return Err(ErrorCode::InvalidArgument, std::string("security transition: ") + message);
}

Status finite(atx::f64 value, const char* message) {
  if (!std::isfinite(value)) {
    return Err(ErrorCode::OutOfRange, std::string("security transition: ") + message);
  }
  return Ok();
}

// The same relative policy measured against a caller-supplied scale, for
// identities whose operands are much larger than the difference they cancel to.
// A zero scale admits only an exact match, so an exactly netted leg is never
// bounded relative to zero and no absolute floor is introduced.
Status reconciled_at_scale(atx::f64 actual, atx::f64 expected, atx::f64 scale,
                           const char* message) {
  ATX_TRY_VOID(finite(actual, message));
  ATX_TRY_VOID(finite(expected, message));
  if (std::abs(actual - expected) > kRelativeToleranceV1 * scale) {
    return Err(ErrorCode::OutOfRange, std::string("security transition: ") + message);
  }
  return Ok();
}

Status reconciled(atx::f64 actual, atx::f64 expected, const char* message) {
  return reconciled_at_scale(actual, expected, std::max(std::abs(actual), std::abs(expected)),
                             message);
}

Result<atx::f64> product(atx::f64 left, atx::f64 right, const char* message) {
  const auto result = left * right;
  ATX_TRY_VOID(finite(result, message));
  if (left != 0.0 && right != 0.0 && result == 0.0) {
    return Err(ErrorCode::OutOfRange, std::string("security transition: ") + message);
  }
  return Ok(result == 0.0 ? 0.0 : result);
}

// Mirrors product's zero guard for the additive legs, and is symmetric for the
// same reason product's is: whichever operand is nonzero yet leaves the sum
// equal to the other has been dusted away, which the representation contract
// forbids regardless of which side of the addition it sits on.
Result<atx::f64> added(atx::f64 left, atx::f64 right, const char* message) {
  const auto result = left + right;
  ATX_TRY_VOID(finite(result, message));
  if ((right != 0.0 && result == left) || (left != 0.0 && result == right)) {
    return Err(ErrorCode::OutOfRange, std::string("security transition: ") + message);
  }
  return Ok(result);
}

Result<atx::f64> ratio(const data::TransitionRatio& value, bool positive) {
  if (value.denominator == 0 || value.denominator > kLargestExactInteger ||
      value.numerator > kLargestExactInteger || (positive && value.numerator == 0)) {
    return Err(ErrorCode::InvalidArgument, "security transition: unsupported rational amount");
  }
  return Ok(static_cast<atx::f64>(value.numerator) /
            static_cast<atx::f64>(value.denominator));
}

Status validate_admission(const TransitionAdmission& admission) {
  if (admission.mode != TransitionMode::SyntheticFixture ||
      admission.units != TransitionUnitConvention::ResearchReinvestedShareEquivalent ||
      admission.claim_valuation != TransitionClaimValuation::FixedUsdFaceValue ||
      !admission.knowledge_cutoff_ns || admission.max_guard_records == 0) {
    return invalid("only explicit synthetic research-unit/face-claim admission is supported");
  }
  return Ok();
}

Status validate_evidence(const data::TransitionEvidence& evidence,
                         const TransitionAdmission& admission) {
  if (evidence.name_space != data::TransitionEvidenceNamespace::SyntheticFixture ||
      !present(evidence.content_sha256) || !evidence.available_at_ns ||
      *evidence.available_at_ns > *admission.knowledge_cutoff_ns) {
    return invalid("missing synthetic evidence or evidence after knowledge cutoff");
  }
  return Ok();
}

Status validate_version(atx::u64 expected, atx::u64 actual, atx::u64 sequence,
                        atx::u64 last_sequence) {
  if (expected != actual || actual == std::numeric_limits<atx::u64>::max() ||
      last_sequence == std::numeric_limits<atx::u64>::max() ||
      sequence != last_sequence + 1) {
    return invalid("stale state, invalid sequence or version/sequence exhaustion");
  }
  return Ok();
}

Status validate_security(const data::TransitionSecurity& security) {
  if (security.vendor_security_id == 0 || security.identity_epoch == 0 ||
      !present(security.mapping_sha256)) {
    return invalid("missing mapped synthetic security identity");
  }
  return Ok();
}

Status validate_ids(std::span<const atx::u64> ids, atx::u64 candidate) {
  atx::u64 previous = 0;
  for (const auto id : ids) {
    if (id <= previous) return invalid("guard IDs must be nonzero, unique and ascending");
    if (id == candidate) {
      return Err(ErrorCode::AlreadyExists, "security transition: duplicate claim/payment ID");
    }
    previous = id;
  }
  return Ok();
}

Status validate_event_guards(const data::SecurityTransition& event,
                             const TransitionHoldingView& state,
                             const TransitionAdmission& admission) {
  if (state.applied_events.size() > admission.max_guard_records ||
      state.known_cash_claim_ids.size() >
          admission.max_guard_records - state.applied_events.size()) {
    return invalid("borrowed guard records exceed admission limit");
  }
  atx::u64 previous_id = 0;
  for (const auto& applied : state.applied_events) {
    if (applied.event_id <= previous_id || applied.revision == 0 || applied.sequence == 0 ||
        applied.sequence > state.last_applied_sequence) {
      return invalid("invalid applied-event guard history");
    }
    if (applied.event_id == event.event_id) {
      return Err(ErrorCode::AlreadyExists, "security transition: event already applied");
    }
    previous_id = applied.event_id;
  }
  return validate_ids(state.known_cash_claim_ids, event.cash_claim_id);
}

Status validate_clock(const data::SecurityTransition& event, const data::TransitionBasis& basis,
                      const TransitionHoldingView& state, const TransitionAdmission& admission) {
  if (!state.last_valuation_at_ns || !state.next_valuation_at_ns || !event.effective_at_ns ||
      !event.entitlement_at_ns || !basis.predecessor.observed_at_ns ||
      !basis.successor.observed_at_ns ||
      *state.last_valuation_at_ns >= *event.effective_at_ns ||
      *event.effective_at_ns > *event.entitlement_at_ns ||
      *event.entitlement_at_ns > *state.next_valuation_at_ns ||
      *state.next_valuation_at_ns > *admission.knowledge_cutoff_ns ||
      *basis.evidence.available_at_ns < *basis.successor.observed_at_ns ||
      basis.predecessor.observed_at_ns != state.last_valuation_at_ns ||
      basis.successor.observed_at_ns != state.next_valuation_at_ns) {
    return invalid("missing, inconsistent supplied, or out-of-order event/valuation clocks");
  }
  return Ok();
}

Status validate_contract(const data::SecurityTransition& event, const data::TransitionBasis& basis,
                         const TransitionHoldingView& state,
                         const TransitionAdmission& admission) {
  ATX_TRY_VOID(validate_admission(admission));
  ATX_TRY_VOID(validate_evidence(event.evidence, admission));
  ATX_TRY_VOID(validate_evidence(basis.evidence, admission));
  ATX_TRY_VOID(validate_version(event.expected_state_version, state.state_version,
                                event.sequence, state.last_applied_sequence));
  if (event.event_id == 0 || event.revision == 0 || event.cash_claim_id == 0 ||
      !present(event.terms_sha256) || event.currency != data::TransitionCurrency::USD ||
      event.cash_denomination != data::TransitionShareDenomination::PredecessorBeforeTransition ||
      event.boundary != data::TransitionBoundary::BetweenValuations ||
      event.entitlement_rule != data::TransitionEntitlementRule::SameAsConvertedPredecessor ||
      state.predecessor_retired) {
    return invalid("unsupported, incomplete or retired transition contract");
  }
  ATX_TRY_VOID(validate_security(event.predecessor));
  ATX_TRY_VOID(validate_security(event.successor));
  if (event.predecessor.vendor_security_id == event.successor.vendor_security_id ||
      event.predecessor.instrument == event.successor.instrument ||
      event.predecessor != state.predecessor || event.successor != state.successor ||
      basis.predecessor.security != event.predecessor ||
      basis.successor.security != event.successor || basis.event_id != event.event_id ||
      basis.revision != event.revision) {
    return invalid("event, basis and held identities disagree or use the same axis/security");
  }
  if (!present(basis.source_artifact_sha256) || !present(basis.axes_sha256) ||
      !present(basis.adjustment_recipe_sha256) ||
      basis.source_artifact_sha256 != state.source_artifact_sha256 ||
      basis.axes_sha256 != state.axes_sha256 ||
      basis.adjustment_recipe_sha256 != state.adjustment_recipe_sha256 ||
      basis.stock_coverage != data::TransitionCoverage::ExcludedFromTri ||
      basis.cash_coverage != data::TransitionCoverage::ExcludedFromTri ||
      basis.continuity_coverage != data::TransitionCoverage::ExcludedFromTri) {
    return invalid("unbound basis or already embedded/unknown event component coverage");
  }
  ATX_TRY_VOID(validate_clock(event, basis, state, admission));
  return validate_event_guards(event, state, admission);
}

Result<atx::f64> price_ratio(atx::f64 numerator, atx::f64 denominator) {
  if (!std::isfinite(numerator) || !std::isfinite(denominator) || numerator <= 0.0 ||
      denominator <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "security transition: missing/nonpositive basis mark");
  }
  const auto result = numerator / denominator;
  if (!std::isfinite(result) || result <= 0.0) {
    return Err(ErrorCode::OutOfRange, "security transition: basis ratio overflow/underflow");
  }
  return Ok(result);
}

// Returns the recomputed existing successor marked value. The plan takes both
// its before and after successor values from this one product, so a supplied
// mark that is admitted within tolerance cannot leak its discrepancy into the
// increment, whose own tolerance is scaled to the (possibly far smaller)
// delivered leg.
Result<atx::f64> validate_values(const data::TransitionBasis& basis,
                                 const TransitionHoldingView& state) {
  ATX_TRY_VOID(finite(state.predecessor_tri_units, "nonfinite predecessor units"));
  ATX_TRY_VOID(finite(state.successor_tri_units, "nonfinite successor units"));
  ATX_TRY_VOID(finite(state.settled_cash, "nonfinite cash"));
  ATX_TRY_VOID(finite(basis.entitled_predecessor_shares, "nonfinite entitlement"));
  ATX_TRY(const auto old_value, product(state.predecessor_tri_units, basis.predecessor.tri_price,
                                       "predecessor marked value overflow/underflow"));
  ATX_TRY(const auto new_value, product(state.successor_tri_units, basis.successor.tri_price,
                                       "existing successor marked value overflow/underflow"));
  ATX_TRY_VOID(reconciled(state.predecessor_accounted_value, old_value,
                          "predecessor accounted value does not match held units/basis"));
  ATX_TRY_VOID(reconciled(state.successor_marked_value, new_value,
                          "successor current value does not match held units/basis"));
  return Ok(new_value);
}

Status size_successor(SecurityTransitionPlan& plan, const TransitionHoldingView& state,
                      atx::f64 inverse_successor_factor) {
  ATX_TRY(plan.successor_units_added,
          product(plan.successor_share_equivalents, inverse_successor_factor,
                  "successor units overflow/underflow"));
  plan.successor_units_after = state.successor_tri_units + plan.successor_units_added;
  ATX_TRY_VOID(finite(plan.successor_units_after, "successor units addition overflow"));
  if (plan.successor_units_added != 0.0 &&
      plan.successor_units_after == state.successor_tri_units) {
    return Err(ErrorCode::OutOfRange, "security transition: successor increment was absorbed");
  }
  // An exact offset is a valid zero holding. It is distinct from an absorbed leg.
  if (plan.successor_units_after == 0.0) plan.successor_units_after = 0.0;
  ATX_TRY_VOID(reconciled(plan.successor_units_after - state.successor_tri_units,
                          plan.successor_units_added, "successor unit increment does not reconcile"));
  ATX_TRY(plan.successor_value_after,
          product(plan.successor_units_after, plan.basis.successor.tri_price,
                  "successor post-transition value overflow/underflow"));
  ATX_TRY(plan.expected_successor_value_added,
          product(plan.successor_share_equivalents, plan.basis.successor.raw_price,
                  "expected successor leg overflow/underflow"));
  plan.actual_successor_value_added = plan.successor_value_after - plan.successor_value_before;
  ATX_TRY_VOID(reconciled(plan.actual_successor_value_added, plan.expected_successor_value_added,
                          "successor economic increment does not reconcile"));
  plan.representation_residual =
      plan.actual_successor_value_added - plan.expected_successor_value_added;
  return Ok();
}

Status finish_transition(SecurityTransitionPlan& plan, const TransitionHoldingView& state) {
  ATX_TRY(const auto proceeds,
          added(plan.actual_successor_value_added, plan.signed_cash_claim,
                "cash claim absorbed into the successor value increment"));
  plan.valuation_bridge = proceeds - plan.removed_predecessor_value;
  ATX_TRY_VOID(finite(plan.valuation_bridge, "valuation bridge overflow"));
  ATX_TRY(const auto before,
          added(plan.removed_predecessor_value, plan.successor_value_before,
                "existing successor value absorbed into the removed predecessor value"));
  ATX_TRY(const auto after,
          added(plan.successor_value_after, plan.signed_cash_claim,
                "cash claim absorbed into the post-transition successor value"));
  // The reported bridge is restated from the four retained magnitudes rather
  // than from itself, and is bounded by the largest of them, so a removed leg
  // much larger than the bridge cannot force a false rejection.
  const auto scale = std::max({std::abs(plan.successor_value_after),
                               std::abs(plan.signed_cash_claim),
                               std::abs(plan.removed_predecessor_value),
                               std::abs(plan.successor_value_before)});
  const auto identity_bridge = after - before;
  ATX_TRY_VOID(reconciled_at_scale(identity_bridge, plan.valuation_bridge, scale,
                                   "transition accounting does not reconcile"));
  plan.accounting_residual = identity_bridge - plan.valuation_bridge;
  if (plan.signed_cash_claim != 0.0) {
    PendingTransitionCashClaim claim;
    claim.cash_claim_id = plan.event.cash_claim_id;
    claim.event_id = plan.event.event_id;
    claim.event_revision = plan.event.revision;
    claim.created_state_version = plan.to_state_version;
    claim.created_sequence = plan.event.sequence;
    claim.currency = plan.event.currency;
    claim.amount = plan.signed_cash_claim;
    claim.entitlement_at_ns = plan.event.entitlement_at_ns;
    claim.recognized_at_ns = state.next_valuation_at_ns;
    claim.evidence = plan.event.evidence;
    plan.claim = claim;
  }
  return Ok();
}

Status validate_payment(const data::CashClaimPayment& payment, const CashClaimPaymentView& state,
                        const TransitionAdmission& admission) {
  ATX_TRY_VOID(validate_admission(admission));
  ATX_TRY_VOID(validate_evidence(payment.evidence, admission));
  ATX_TRY_VOID(validate_evidence(state.claim.evidence, admission));
  ATX_TRY_VOID(validate_version(payment.expected_state_version, state.state_version,
                                payment.sequence, state.last_applied_sequence));
  const auto& claim = state.claim;
  if (payment.payment_id == 0 || claim.cash_claim_id == 0 || claim.event_id == 0 ||
      claim.event_revision == 0 || claim.created_state_version == 0 ||
      claim.created_state_version > state.state_version || claim.created_sequence == 0 ||
      claim.created_sequence > state.last_applied_sequence || claim.settled ||
      payment.cash_claim_id != claim.cash_claim_id || payment.event_id != claim.event_id ||
      payment.event_revision != claim.event_revision ||
      payment.currency != data::TransitionCurrency::USD || payment.currency != claim.currency ||
      !std::isfinite(claim.amount) || claim.amount == 0.0 || payment.amount != claim.amount) {
    return invalid("payment does not exactly identify a full outstanding signed synthetic claim");
  }
  if (!payment.allocated_at_ns || !claim.entitlement_at_ns || !claim.recognized_at_ns ||
      *claim.entitlement_at_ns > *claim.recognized_at_ns ||
      *claim.recognized_at_ns > *payment.allocated_at_ns ||
      *payment.evidence.available_at_ns < *payment.allocated_at_ns ||
      *payment.allocated_at_ns > *admission.knowledge_cutoff_ns) {
    return invalid("missing payment/claim clocks or allocation before recognition/after cutoff");
  }
  if (state.applied_payment_ids.size() > admission.max_guard_records) {
    return invalid("borrowed payment guard records exceed admission limit");
  }
  ATX_TRY_VOID(validate_ids(state.applied_payment_ids, payment.payment_id));
  return finite(state.settled_cash, "nonfinite payment cash");
}

} // namespace

static_assert(std::is_trivially_copyable_v<SecurityTransitionPlan>);
static_assert(std::is_trivially_copyable_v<CashClaimPaymentPlan>);

Result<SecurityTransitionPlan> plan_security_transition(
    const data::SecurityTransition& event, const data::TransitionBasis& basis,
    const TransitionHoldingView& state, const TransitionAdmission& admission) {
  ATX_TRY_VOID(validate_contract(event, basis, state, admission));
  ATX_TRY(const auto stock_ratio, ratio(event.stock_ratio, true));
  ATX_TRY(const auto cash_ratio, ratio(event.cash_per_predecessor_share, false));
  ATX_TRY(const auto predecessor_factor,
          price_ratio(basis.predecessor.tri_price, basis.predecessor.raw_price));
  ATX_TRY(const auto inverse_successor_factor,
          price_ratio(basis.successor.raw_price, basis.successor.tri_price));
  ATX_TRY(const auto successor_value_before, validate_values(basis, state));

  SecurityTransitionPlan plan;
  plan.event = event;
  plan.basis = basis;
  plan.admission = admission;
  plan.from_state_version = state.state_version;
  plan.to_state_version = state.state_version + 1;
  plan.predecessor_units_before = state.predecessor_tri_units;
  plan.successor_units_before = state.successor_tri_units;
  plan.removed_predecessor_value = state.predecessor_accounted_value;
  // Recomputed, not the supplied mark: see validate_values.
  plan.successor_value_before = successor_value_before;
  plan.cash_before = state.settled_cash;
  plan.cash_after = state.settled_cash;
  plan.retire_predecessor = true;
  ATX_TRY(plan.converted_predecessor_shares,
          product(state.predecessor_tri_units, predecessor_factor,
                  "converted predecessor shares overflow/underflow"));
  ATX_TRY_VOID(reconciled(basis.entitled_predecessor_shares, plan.converted_predecessor_shares,
                          "entitlement differs from converted pre-transition share basis"));
  ATX_TRY(plan.successor_share_equivalents,
          product(plan.converted_predecessor_shares, stock_ratio,
                  "successor share equivalents overflow/underflow"));
  ATX_TRY(plan.signed_cash_claim,
          product(basis.entitled_predecessor_shares, cash_ratio, "cash claim overflow/underflow"));
  plan.gross_cash_receivable = std::max(plan.signed_cash_claim, 0.0);
  plan.gross_cash_payable = std::max(-plan.signed_cash_claim, 0.0);
  ATX_TRY_VOID(size_successor(plan, state, inverse_successor_factor));
  ATX_TRY_VOID(finish_transition(plan, state));
  return Ok(plan);
}

Result<CashClaimPaymentPlan> plan_cash_claim_payment(
    const data::CashClaimPayment& payment, const CashClaimPaymentView& state,
    const TransitionAdmission& admission) {
  ATX_TRY_VOID(validate_payment(payment, state, admission));
  CashClaimPaymentPlan plan;
  plan.payment = payment;
  plan.admission = admission;
  plan.claim_before = state.claim;
  plan.claim_after = state.claim;
  plan.claim_after.amount = 0.0;
  plan.claim_after.settled = true;
  plan.from_state_version = state.state_version;
  plan.to_state_version = state.state_version + 1;
  plan.cash_before = state.settled_cash;
  plan.cash_after = state.settled_cash + payment.amount;
  ATX_TRY_VOID(finite(plan.cash_after, "payment cash addition overflow"));
  if (plan.cash_after == plan.cash_before) {
    return Err(ErrorCode::OutOfRange, "security transition: payment cash increment was absorbed");
  }
  plan.actual_cash_delta = plan.cash_after - plan.cash_before;
  ATX_TRY_VOID(reconciled(plan.actual_cash_delta, payment.amount,
                          "payment cash increment does not preserve the signed claim"));
  plan.claim_delta = -state.claim.amount;
  plan.accounting_residual = plan.actual_cash_delta + plan.claim_delta;
  return Ok(plan);
}

} // namespace atx::engine::book
