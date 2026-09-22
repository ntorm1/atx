#include "atx/engine/book/claims_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace atx::engine::book {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

constexpr atx::u64 kExhaustedCounter = std::numeric_limits<atx::u64>::max();

Status invalid(const char* message) {
  return Err(ErrorCode::InvalidArgument, std::string("claims state: ") + message);
}

Status exhausted(const char* message) {
  return Err(ErrorCode::OutOfRange, std::string("claims state: ") + message);
}

Status duplicated(const char* message) {
  return Err(ErrorCode::AlreadyExists, std::string("claims state: ") + message);
}

Status require_finite(atx::f64 value, const char* message) {
  if (!std::isfinite(value)) return exhausted(message);
  return Ok();
}

// Every entry point validates the retained counters before trusting any array
// prefix, so a corrupted or hand-built state rejects instead of reading past
// the bounded storage it claims to own.
Status validate_counts(const ClaimsBookState& state) {
  if (state.event_count > kMaxTransitionEvents || state.claim_count > kMaxPendingClaims ||
      state.payment_count > kMaxAppliedPayments ||
      state.movement_count > kMaxMandatoryMovements ||
      state.retired_count > kMaxTransitionEvents) {
    return invalid("retained counters exceed their bounded storage");
  }
  return Ok();
}

struct ClaimTotals {
  atx::f64 signed_total{};
  atx::f64 gross_receivable{};
  atx::f64 gross_payable{};
};

Status accumulate(ClaimTotals& totals, atx::f64 amount) {
  ATX_TRY_VOID(require_finite(amount, "nonfinite pending claim amount"));
  totals.signed_total += amount;
  totals.gross_receivable += std::max(amount, 0.0);
  totals.gross_payable += std::max(-amount, 0.0);
  return require_finite(totals.signed_total, "pending claim total overflow");
}

// `exclude` drops one slot from the total, which is how a payment projects the
// post-settlement claim book without writing to it first.
Result<ClaimTotals> pending_totals(const ClaimsBookState& state,
                                   atx::usize exclude = kNoInstrument) {
  ClaimTotals totals;
  for (atx::usize i = 0; i < state.claim_count; ++i) {
    if (i == exclude || state.claims[i].status != ClaimSlotStatus::Pending) continue;
    ATX_TRY_VOID(accumulate(totals, state.claims[i].claim.amount));
  }
  return Ok(totals);
}

ClaimsNav make_nav(atx::f64 settled_cash, atx::f64 marked_equities, const ClaimTotals& totals) {
  ClaimsNav nav;
  nav.settled_cash = settled_cash;
  nav.marked_equities = marked_equities;
  nav.signed_pending_claims = totals.signed_total;
  nav.gross_receivable = totals.gross_receivable;
  nav.gross_payable = totals.gross_payable;
  nav.nav = settled_cash + marked_equities + totals.signed_total;
  return nav;
}

Status require_positive_nav(const ClaimsNav& nav) {
  ATX_TRY_VOID(require_finite(nav.nav, "nonfinite claims-aware NAV"));
  if (nav.nav <= 0.0) return exhausted("nonpositive claims-aware NAV");
  return Ok();
}

Status validate_versions(atx::u64 from, atx::u64 to, atx::u64 sequence,
                         const ClaimsBookState& state) {
  if (state.state_version == kExhaustedCounter ||
      state.last_applied_sequence == kExhaustedCounter) {
    return exhausted("state version or applied sequence exhausted");
  }
  if (from != state.state_version || to != state.state_version + 1 ||
      sequence != state.last_applied_sequence + 1) {
    return invalid("stale state version or non-consecutive sequence");
  }
  return Ok();
}

Status require_unapplied_event(const ClaimsBookState& state, atx::u64 event_id) {
  if (event_id == 0) return invalid("event ID must be nonzero");
  for (atx::usize i = 0; i < state.event_count; ++i) {
    if (state.applied_events[i].event_id == event_id) return duplicated("event already applied");
  }
  return Ok();
}

Status require_unknown_id(std::span<const atx::u64> ids, atx::u64 id, const char* message) {
  if (id == 0) return invalid("guard ID must be nonzero");
  if (std::find(ids.begin(), ids.end(), id) != ids.end()) return duplicated(message);
  return Ok();
}

Status require_live(const ClaimsBookState& state, std::span<const atx::u8> retired_mask,
                    atx::usize instrument, const char* message) {
  if (retired_mask[instrument] != 0) return invalid(message);
  for (atx::usize i = 0; i < state.retired_count; ++i) {
    if (state.retired[i].instrument == instrument) return invalid(message);
  }
  return Ok();
}

// Everything a transition commit needs, sized on the stack. The tail that
// applies it performs only stores that cannot fail.
struct StagedTransition {
  atx::usize predecessor{};
  atx::usize successor{};
  bool has_claim{};
  PendingTransitionCashClaim claim{};
  RetiredRepresentation retired{};
  AppliedSecurityTransition applied{};
  atx::usize movement_count{};
  std::array<MandatoryMovement, 3> movements{};
  ClaimsNav nav{};
};

Status validate_spans(const SecurityTransitionPlan& plan, std::span<const atx::f64> tri_units,
                      std::span<const atx::f64> marked_values,
                      std::span<const atx::u8> retired_mask, StagedTransition& staged) {
  if (tri_units.empty() || tri_units.size() != marked_values.size() ||
      tri_units.size() != retired_mask.size()) {
    return invalid("book spans must be nonempty and share the canonical length");
  }
  staged.predecessor = plan.event.predecessor.instrument;
  staged.successor = plan.event.successor.instrument;
  if (staged.predecessor == staged.successor || staged.predecessor >= tri_units.size() ||
      staged.successor >= tri_units.size()) {
    return invalid("transition instruments must be distinct and inside the book");
  }
  return Ok();
}

// The plan was built from these four numbers, so anything but equality means the
// book moved under the plan and the commit is no longer the one that was priced.
Status require_before_values(const SecurityTransitionPlan& plan,
                             std::span<const atx::f64> tri_units,
                             std::span<const atx::f64> marked_values,
                             const StagedTransition& staged) {
  const std::array<atx::f64, 4> held{tri_units[staged.predecessor],
                                     marked_values[staged.predecessor],
                                     tri_units[staged.successor],
                                     marked_values[staged.successor]};
  const std::array<atx::f64, 4> expected{plan.predecessor_units_before,
                                         plan.removed_predecessor_value,
                                         plan.successor_units_before,
                                         plan.successor_value_before};
  for (atx::usize i = 0; i < held.size(); ++i) {
    ATX_TRY_VOID(require_finite(held[i], "nonfinite held units or marked value"));
    if (held[i] != expected[i]) {
      return invalid("book units/values differ from the plan's pre-transition values");
    }
  }
  return Ok();
}

Status require_capacity(const ClaimsBookState& state, atx::usize movements, bool needs_claim) {
  if (state.event_count >= kMaxTransitionEvents) return exhausted("applied event capacity");
  if (state.retired_count >= kMaxTransitionEvents) {
    return exhausted("retired representation capacity");
  }
  if (needs_claim && state.claim_count >= kMaxPendingClaims) {
    return exhausted("pending claim capacity");
  }
  if (movements > kMaxMandatoryMovements - state.movement_count) {
    return exhausted("mandatory movement capacity");
  }
  return Ok();
}

MandatoryMovement transition_row(const SecurityTransitionPlan& plan, atx::usize period,
                                 MandatoryMovementKind kind) {
  MandatoryMovement row;
  row.kind = kind;
  row.period = period;
  row.event_id = plan.event.event_id;
  row.cash_claim_id = plan.event.cash_claim_id;
  row.sequence = plan.event.sequence;
  row.from_state_version = plan.from_state_version;
  row.to_state_version = plan.to_state_version;
  return row;
}

void stage_movements(const SecurityTransitionPlan& plan, atx::usize period,
                     StagedTransition& staged) {
  staged.movements[0] = transition_row(plan, period,
                                       MandatoryMovementKind::PredecessorRetirement);
  MandatoryMovement& retire = staged.movements[0];
  retire.instrument = staged.predecessor;
  retire.units_before = plan.predecessor_units_before;
  retire.units_after = plan.predecessor_units_after;
  retire.value_before = plan.removed_predecessor_value;
  retire.value_after = 0.0;

  staged.movements[1] = transition_row(plan, period, MandatoryMovementKind::SuccessorDelivery);
  MandatoryMovement& deliver = staged.movements[1];
  deliver.instrument = staged.successor;
  deliver.units_before = plan.successor_units_before;
  deliver.units_after = plan.successor_units_after;
  deliver.value_before = plan.successor_value_before;
  deliver.value_after = plan.successor_value_after;
  deliver.valuation_bridge = plan.valuation_bridge;
  deliver.accounting_residual = plan.accounting_residual;
  staged.movement_count = 2;
  if (!staged.has_claim) return;

  // A zero claim creates no record, so it gets no recognition row either.
  staged.movements[2] = transition_row(plan, period, MandatoryMovementKind::ClaimRecognition);
  MandatoryMovement& recognize = staged.movements[2];
  recognize.value_after = plan.signed_cash_claim;
  recognize.claim_delta = plan.signed_cash_claim;
  staged.movement_count = 3;
}

Result<atx::f64> projected_equities(std::span<const atx::f64> marked_values,
                                    const StagedTransition& staged, atx::f64 successor_after) {
  atx::f64 total = 0.0;
  for (atx::usize i = 0; i < marked_values.size(); ++i) {
    if (i == staged.predecessor) continue;
    total += (i == staged.successor) ? successor_after : marked_values[i];
  }
  ATX_TRY_VOID(require_finite(total, "nonfinite projected marked equities"));
  return Ok(total);
}

Result<ClaimsNav> projected_nav(const ClaimsBookState& state, const StagedTransition& staged,
                                atx::f64 marked_equities, atx::f64 settled_cash) {
  ATX_TRY(auto totals, pending_totals(state));
  if (staged.has_claim) {
    ATX_TRY_VOID(accumulate(totals, staged.claim.amount));
  }
  const ClaimsNav nav = make_nav(settled_cash, marked_equities, totals);
  ATX_TRY_VOID(require_positive_nav(nav));
  return Ok(nav);
}

Status stage_claim(const SecurityTransitionPlan& plan, StagedTransition& staged) {
  staged.has_claim = plan.claim.has_value();
  if (!staged.has_claim) {
    if (plan.signed_cash_claim != 0.0) return invalid("nonzero cash claim without a record");
    return Ok();
  }
  staged.claim = *plan.claim;
  if (staged.claim.cash_claim_id != plan.event.cash_claim_id || staged.claim.settled ||
      staged.claim.amount != plan.signed_cash_claim || staged.claim.amount == 0.0 ||
      staged.claim.currency != data::TransitionCurrency::USD) {
    return invalid("plan claim disagrees with its event or carries no outstanding amount");
  }
  return require_finite(staged.claim.amount, "nonfinite recognized claim amount");
}

Status stage_transition(const SecurityTransitionPlan& plan, atx::usize period,
                        std::span<const atx::f64> tri_units,
                        std::span<const atx::f64> marked_values,
                        std::span<const atx::u8> retired_mask, const ClaimsBookState& state,
                        StagedTransition& staged) {
  ATX_TRY_VOID(validate_counts(state));
  ATX_TRY_VOID(validate_spans(plan, tri_units, marked_values, retired_mask, staged));
  if (!plan.retire_predecessor) return invalid("plan does not retire its predecessor");
  ATX_TRY_VOID(validate_versions(plan.from_state_version, plan.to_state_version,
                                 plan.event.sequence, state));
  ATX_TRY_VOID(require_unapplied_event(state, plan.event.event_id));
  ATX_TRY_VOID(require_unknown_id(known_claim_ids(state), plan.event.cash_claim_id,
                                  "cash claim ID already known"));
  ATX_TRY_VOID(require_live(state, retired_mask, staged.predecessor,
                            "predecessor representation is already retired"));
  ATX_TRY_VOID(require_live(state, retired_mask, staged.successor,
                            "successor representation is retired"));
  ATX_TRY_VOID(require_before_values(plan, tri_units, marked_values, staged));
  if (plan.cash_before != state.settled_cash || plan.cash_after != plan.cash_before) {
    return invalid("plan cash disagrees with settled cash or moves settled cash");
  }
  if (plan.predecessor_units_after != 0.0) {
    return invalid("a retired predecessor must hold exactly zero units");
  }
  ATX_TRY_VOID(require_finite(plan.successor_units_after, "nonfinite successor units"));
  ATX_TRY_VOID(require_finite(plan.successor_value_after, "nonfinite successor value"));
  ATX_TRY_VOID(stage_claim(plan, staged));
  stage_movements(plan, period, staged);
  ATX_TRY_VOID(require_capacity(state, staged.movement_count, staged.has_claim));
  ATX_TRY(const auto equities,
          projected_equities(marked_values, staged, plan.successor_value_after));
  ATX_TRY(staged.nav, projected_nav(state, staged, equities, plan.cash_after));
  staged.applied = {plan.event.event_id, plan.event.revision, plan.event.sequence};
  staged.retired = {staged.predecessor, plan.event.predecessor.identity_epoch,
                    plan.event.event_id, plan.to_state_version, period};
  return Ok();
}

void insert_applied_event(ClaimsBookState& state,
                          const AppliedSecurityTransition& applied) noexcept {
  atx::usize index = state.event_count;
  while (index > 0 && state.applied_events[index - 1].event_id > applied.event_id) {
    state.applied_events[index] = state.applied_events[index - 1];
    --index;
  }
  state.applied_events[index] = applied;
  ++state.event_count;
}

// `known_claim_ids` and `claims` are shifted together so that index i always
// names the same claim in both arrays.
void insert_claim(ClaimsBookState& state, const PendingTransitionCashClaim& claim) noexcept {
  atx::usize index = state.claim_count;
  while (index > 0 && state.known_claim_ids[index - 1] > claim.cash_claim_id) {
    state.known_claim_ids[index] = state.known_claim_ids[index - 1];
    state.claims[index] = state.claims[index - 1];
    --index;
  }
  state.known_claim_ids[index] = claim.cash_claim_id;
  state.claims[index].claim = claim;
  state.claims[index].status = ClaimSlotStatus::Pending;
  ++state.claim_count;
}

void insert_payment_id(ClaimsBookState& state, atx::u64 payment_id) noexcept {
  atx::usize index = state.payment_count;
  while (index > 0 && state.applied_payments[index - 1] > payment_id) {
    state.applied_payments[index] = state.applied_payments[index - 1];
    --index;
  }
  state.applied_payments[index] = payment_id;
  ++state.payment_count;
}

void apply_transition(const SecurityTransitionPlan& plan, const StagedTransition& staged,
                      std::span<atx::f64> tri_units, std::span<atx::f64> marked_values,
                      std::span<atx::u8> retired_mask, ClaimsBookState& state) noexcept {
  tri_units[staged.predecessor] = plan.predecessor_units_after;
  marked_values[staged.predecessor] = 0.0;
  tri_units[staged.successor] = plan.successor_units_after;
  marked_values[staged.successor] = plan.successor_value_after;
  retired_mask[staged.predecessor] = atx::u8{1};
  state.retired[state.retired_count] = staged.retired;
  ++state.retired_count;
  insert_applied_event(state, staged.applied);
  if (staged.has_claim) insert_claim(state, staged.claim);
  for (atx::usize i = 0; i < staged.movement_count; ++i) {
    state.movements[state.movement_count + i] = staged.movements[i];
  }
  state.movement_count += staged.movement_count;
  state.state_version = plan.to_state_version;
  state.last_applied_sequence = plan.event.sequence;
}

struct StagedPayment {
  atx::usize slot{};
  PendingTransitionCashClaim claim_after{};
  MandatoryMovement movement{};
  ClaimsNav nav{};
};

bool same_evidence(const data::TransitionEvidence& left,
                   const data::TransitionEvidence& right) noexcept {
  return left.name_space == right.name_space && left.content_sha256 == right.content_sha256 &&
         left.available_at_ns == right.available_at_ns;
}

// Everything that identifies a claim, i.e. every field except the two the
// settlement itself changes.
bool same_claim_identity(const PendingTransitionCashClaim& left,
                         const PendingTransitionCashClaim& right) noexcept {
  return left.cash_claim_id == right.cash_claim_id && left.event_id == right.event_id &&
         left.event_revision == right.event_revision &&
         left.created_state_version == right.created_state_version &&
         left.created_sequence == right.created_sequence && left.currency == right.currency &&
         left.entitlement_at_ns == right.entitlement_at_ns &&
         left.recognized_at_ns == right.recognized_at_ns &&
         same_evidence(left.evidence, right.evidence);
}

bool same_claim(const PendingTransitionCashClaim& left,
                const PendingTransitionCashClaim& right) noexcept {
  return same_claim_identity(left, right) && left.amount == right.amount &&
         left.settled == right.settled;
}

// The settled slot is DERIVED from the retained claim, never copied out of the
// plan: a plan whose `claim_after` carried a different identity would otherwise
// rewrite the ledger's record of which claim was settled.
PendingTransitionCashClaim settled_from(const PendingTransitionCashClaim& retained) noexcept {
  PendingTransitionCashClaim result = retained;
  result.amount = 0.0;
  result.settled = true;
  return result;
}

Result<atx::usize> find_pending_slot(const ClaimsBookState& state, atx::u64 claim_id) {
  for (atx::usize i = 0; i < state.claim_count; ++i) {
    if (state.known_claim_ids[i] != claim_id) continue;
    if (state.claims[i].status != ClaimSlotStatus::Pending) {
      return Err(ErrorCode::InvalidArgument, "claims state: cash claim is not pending");
    }
    return Ok(i);
  }
  return Err(ErrorCode::NotFound, "claims state: unknown cash claim ID");
}

// `retained` is the slot's own claim, so the ledger row names the claim the
// state actually holds rather than the one the plan asserts.
void stage_payment_movement(const CashClaimPaymentPlan& plan, atx::usize period,
                            const PendingTransitionCashClaim& retained,
                            StagedPayment& staged) noexcept {
  MandatoryMovement& row = staged.movement;
  row.kind = MandatoryMovementKind::ClaimSettlement;
  row.period = period;
  row.event_id = retained.event_id;
  row.cash_claim_id = retained.cash_claim_id;
  row.payment_id = plan.payment.payment_id;
  row.sequence = plan.payment.sequence;
  row.from_state_version = plan.from_state_version;
  row.to_state_version = plan.to_state_version;
  row.value_before = retained.amount;
  row.value_after = staged.claim_after.amount;
  row.settled_cash_delta = plan.actual_cash_delta;
  row.claim_delta = plan.claim_delta;
  row.accounting_residual = plan.accounting_residual;
}

Status validate_payment_plan(const CashClaimPaymentPlan& plan, const ClaimsBookState& state) {
  if (plan.cash_before != state.settled_cash) {
    return invalid("payment plan cash disagrees with settled cash");
  }
  if (plan.claim_after.amount != 0.0 || !plan.claim_after.settled ||
      plan.payment.amount != plan.claim_before.amount ||
      plan.cash_after != plan.cash_before + plan.payment.amount ||
      plan.claim_delta != -plan.claim_before.amount) {
    return invalid("payment plan does not fully settle its claim into settled cash");
  }
  return require_finite(plan.cash_after, "nonfinite post-payment settled cash");
}

Status stage_payment(const CashClaimPaymentPlan& plan, atx::usize period,
                     const ClaimsBookState& state, StagedPayment& staged) {
  ATX_TRY_VOID(validate_counts(state));
  ATX_TRY_VOID(validate_versions(plan.from_state_version, plan.to_state_version,
                                 plan.payment.sequence, state));
  ATX_TRY(staged.slot, find_pending_slot(state, plan.payment.cash_claim_id));
  const PendingTransitionCashClaim& retained = state.claims[staged.slot].claim;
  if (!same_claim(plan.claim_before, retained)) {
    return invalid("payment plan's claim differs from the retained pending slot");
  }
  if (!same_claim_identity(plan.claim_after, retained)) {
    return invalid("payment plan's settled claim forges the retained claim identity");
  }
  ATX_TRY_VOID(require_unknown_id(applied_payments(state), plan.payment.payment_id,
                                  "payment ID already applied"));
  if (state.payment_count >= kMaxAppliedPayments) return exhausted("applied payment capacity");
  if (state.movement_count >= kMaxMandatoryMovements) {
    return exhausted("mandatory movement capacity");
  }
  ATX_TRY_VOID(validate_payment_plan(plan, state));
  staged.claim_after = settled_from(retained);
  stage_payment_movement(plan, period, retained, staged);
  // Settlement moves value between two NAV terms; the equity book is not
  // observed here, so the reported NAV is the cash-and-claims subtotal.
  ATX_TRY(const auto totals, pending_totals(state, staged.slot));
  staged.nav = make_nav(plan.cash_after, 0.0, totals);
  return require_finite(staged.nav.nav, "nonfinite post-payment claims subtotal");
}

void apply_payment(const CashClaimPaymentPlan& plan, const StagedPayment& staged,
                   ClaimsBookState& state) noexcept {
  state.settled_cash = plan.cash_after;
  state.claims[staged.slot].claim = staged.claim_after;
  state.claims[staged.slot].status = ClaimSlotStatus::Settled;
  insert_payment_id(state, plan.payment.payment_id);
  state.movements[state.movement_count] = staged.movement;
  ++state.movement_count;
  state.state_version = plan.to_state_version;
  state.last_applied_sequence = plan.payment.sequence;
}

} // namespace

Result<ClaimsNav> compute_claims_nav(const ClaimsBookState& state, atx::f64 marked_equities) {
  ATX_TRY_VOID(validate_counts(state));
  ATX_TRY_VOID(require_finite(state.settled_cash, "nonfinite settled cash"));
  ATX_TRY_VOID(require_finite(marked_equities, "nonfinite marked equities"));
  ATX_TRY(const auto totals, pending_totals(state));
  const ClaimsNav nav = make_nav(state.settled_cash, marked_equities, totals);
  ATX_TRY_VOID(require_positive_nav(nav));
  return Ok(nav);
}

Result<ClaimsCommitReceipt> commit_security_transition(
    const SecurityTransitionPlan& plan, atx::usize period, std::span<atx::f64> tri_units,
    std::span<atx::f64> marked_values, std::span<atx::u8> retired_mask, ClaimsBookState& state) {
  StagedTransition staged{};
  ATX_TRY_VOID(
      stage_transition(plan, period, tri_units, marked_values, retired_mask, state, staged));
  ClaimsCommitReceipt receipt;
  receipt.from_state_version = plan.from_state_version;
  receipt.to_state_version = plan.to_state_version;
  receipt.sequence = plan.event.sequence;
  receipt.first_movement = state.movement_count;
  receipt.movement_count = staged.movement_count;
  receipt.nav_after = staged.nav;
  apply_transition(plan, staged, tri_units, marked_values, retired_mask, state);
  return Ok(receipt);
}

Result<ClaimsCommitReceipt> commit_cash_claim_payment(const CashClaimPaymentPlan& plan,
                                                      atx::usize period,
                                                      ClaimsBookState& state) {
  StagedPayment staged{};
  ATX_TRY_VOID(stage_payment(plan, period, state, staged));
  ClaimsCommitReceipt receipt;
  receipt.from_state_version = plan.from_state_version;
  receipt.to_state_version = plan.to_state_version;
  receipt.sequence = plan.payment.sequence;
  receipt.first_movement = state.movement_count;
  receipt.movement_count = 1;
  receipt.nav_after = staged.nav;
  apply_payment(plan, staged, state);
  return Ok(receipt);
}

std::span<const AppliedSecurityTransition> applied_events(const ClaimsBookState& state) {
  return {state.applied_events.data(), std::min(state.event_count, kMaxTransitionEvents)};
}

std::span<const atx::u64> known_claim_ids(const ClaimsBookState& state) {
  return {state.known_claim_ids.data(), std::min(state.claim_count, kMaxPendingClaims)};
}

std::span<const atx::u64> applied_payments(const ClaimsBookState& state) {
  return {state.applied_payments.data(), std::min(state.payment_count, kMaxAppliedPayments)};
}

std::span<const PendingTransitionCashClaim> pending_claims_view(
    const ClaimsBookState& state,
    std::span<PendingTransitionCashClaim, kMaxPendingClaims> scratch) {
  atx::usize filled = 0;
  const atx::usize count = std::min(state.claim_count, kMaxPendingClaims);
  for (atx::usize i = 0; i < count; ++i) {
    if (state.claims[i].status != ClaimSlotStatus::Pending) continue;
    scratch[filled] = state.claims[i].claim;
    ++filled;
  }
  return {scratch.data(), filled};
}

} // namespace atx::engine::book
