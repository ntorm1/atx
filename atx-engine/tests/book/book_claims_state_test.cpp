#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "atx/engine/book/claims_state.hpp"
#include "atx/engine/book/security_transition.hpp"

namespace {
namespace book = atx::engine::book;
namespace data = atx::engine::data;

// Matches the checkpoint-12 sibling suite: 64 * f64 epsilon scaled to the
// canonical magnitudes, i.e. the planner's own published reconciliation policy.
constexpr double kValueTolerance = 1.0e-12;
constexpr double kUnitTolerance = 1.0e-15;

template <class T> std::string why(const atx::core::Result<T>& result) {
  return result.has_value() ? std::string{} : result.error().message();
}

data::TransitionDigest digest(atx::u8 value) {
  data::TransitionDigest result{};
  result.fill(value);
  return result;
}

data::TransitionEvidence evidence(atx::u8 id, atx::i64 available_at) {
  return {data::TransitionEvidenceNamespace::SyntheticFixture, digest(id), available_at};
}

// Three axis positions: 0 = predecessor, 1 = successor, 2 = an untouched name
// that must stay bit-identical across every commit and every rejection.
struct Book {
  std::array<double, 3> tri_units{};
  std::array<double, 3> marked_values{};
  std::array<atx::u8, 3> retired{};
};

struct Fixture {
  data::SecurityTransition event;
  data::TransitionBasis basis;
  book::TransitionAdmission admission;
  book::ClaimsBookState state;
  Book book;
};

// The checkpoint-12 canonical numbers: old units 2, raw 10, TRI 30, ratio 1/2,
// cash 40491/10000, successor raw 12, TRI 60, existing successor units -0.4,
// settled cash 100. Long NAV 136 -> 136.2946; short mirror 64 -> 63.7054.
Fixture make_fixture(double sign) {
  Fixture result{};
  auto& event = result.event;
  event.event_id = 1;
  event.revision = 1;
  event.sequence = 1;
  event.expected_state_version = 7;
  event.cash_claim_id = 101;
  event.predecessor = {1001, 1, 0, digest(1)};
  event.successor = {1002, 1, 1, digest(2)};
  event.stock_ratio = {1, 2};
  event.cash_per_predecessor_share = {40491, 10000};
  event.currency = data::TransitionCurrency::USD;
  event.cash_denomination = data::TransitionShareDenomination::PredecessorBeforeTransition;
  event.boundary = data::TransitionBoundary::BetweenValuations;
  event.entitlement_rule = data::TransitionEntitlementRule::SameAsConvertedPredecessor;
  event.effective_at_ns = 150;
  event.entitlement_at_ns = 160;
  event.terms_sha256 = digest(3);
  event.evidence = evidence(4, 120);

  auto& basis = result.basis;
  basis.event_id = event.event_id;
  basis.revision = event.revision;
  basis.source_artifact_sha256 = digest(5);
  basis.axes_sha256 = digest(6);
  basis.adjustment_recipe_sha256 = digest(7);
  basis.evidence = evidence(8, 200);
  basis.predecessor = {event.predecessor, 10.0, 30.0, 100};
  basis.successor = {event.successor, 12.0, 60.0, 200};
  basis.stock_coverage = data::TransitionCoverage::ExcludedFromTri;
  basis.cash_coverage = data::TransitionCoverage::ExcludedFromTri;
  basis.continuity_coverage = data::TransitionCoverage::ExcludedFromTri;
  basis.entitled_predecessor_shares = sign * 6.0;

  result.admission = {book::TransitionMode::SyntheticFixture,
                      book::TransitionUnitConvention::ResearchReinvestedShareEquivalent,
                      book::TransitionClaimValuation::FixedUsdFaceValue, 250, 64};
  result.state.state_version = event.expected_state_version;
  result.state.settled_cash = 100.0;
  result.book.tri_units = {sign * 2.0, sign * -0.4, 0.0};
  result.book.marked_values = {sign * 60.0, sign * -24.0, 0.0};
  return result;
}

book::TransitionHoldingView holding(const Fixture& f) {
  book::TransitionHoldingView view;
  view.state_version = f.state.state_version;
  view.last_applied_sequence = f.state.last_applied_sequence;
  view.predecessor = f.event.predecessor;
  view.successor = f.event.successor;
  view.source_artifact_sha256 = f.basis.source_artifact_sha256;
  view.axes_sha256 = f.basis.axes_sha256;
  view.adjustment_recipe_sha256 = f.basis.adjustment_recipe_sha256;
  view.predecessor_tri_units = f.book.tri_units[0];
  view.predecessor_accounted_value = f.book.marked_values[0];
  view.successor_tri_units = f.book.tri_units[1];
  view.successor_marked_value = f.book.marked_values[1];
  view.settled_cash = f.state.settled_cash;
  view.last_valuation_at_ns = 100;
  view.next_valuation_at_ns = 200;
  view.applied_events = book::applied_events(f.state);
  view.known_cash_claim_ids = book::known_claim_ids(f.state);
  return view;
}

book::SecurityTransitionPlan plan_for(const Fixture& f) {
  const auto planned = book::plan_security_transition(f.event, f.basis, holding(f), f.admission);
  EXPECT_TRUE(planned.has_value()) << why(planned);
  return planned.has_value() ? *planned : book::SecurityTransitionPlan{};
}

atx::core::Result<book::ClaimsCommitReceipt> commit(const book::SecurityTransitionPlan& plan,
                                                    Fixture& f) {
  return book::commit_security_transition(plan, 3, f.book.tri_units, f.book.marked_values,
                                          f.book.retired, f.state);
}

book::CashClaimPaymentPlan payment_plan(const Fixture& f,
                                        const book::SecurityTransitionPlan& plan) {
  data::CashClaimPayment payment;
  payment.payment_id = 201;
  payment.sequence = f.state.last_applied_sequence + 1;
  payment.expected_state_version = f.state.state_version;
  payment.cash_claim_id = plan.event.cash_claim_id;
  payment.event_id = plan.event.event_id;
  payment.event_revision = plan.event.revision;
  payment.currency = data::TransitionCurrency::USD;
  payment.amount = plan.signed_cash_claim;
  payment.allocated_at_ns = 240;
  payment.evidence = evidence(9, 240);

  book::CashClaimPaymentView view;
  view.state_version = f.state.state_version;
  view.last_applied_sequence = f.state.last_applied_sequence;
  view.settled_cash = f.state.settled_cash;
  view.claim = f.state.claims[0].claim;
  view.applied_payment_ids = book::applied_payments(f.state);
  const auto planned = book::plan_cash_claim_payment(payment, view, f.admission);
  EXPECT_TRUE(planned.has_value()) << why(planned);
  return planned.has_value() ? *planned : book::CashClaimPaymentPlan{};
}

template <class T> std::array<atx::u8, sizeof(T)> snapshot(const T& value) {
  std::array<atx::u8, sizeof(T)> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(T));
  return bytes;
}

template <class T> void expect_unchanged(const T& value, const std::array<atx::u8, sizeof(T)>& b) {
  EXPECT_EQ(std::memcmp(&value, b.data(), b.size()), 0);
}
} // namespace

TEST(ClaimsState, SignedTransitionAndPaymentRoundTripPreserveNavAndLedger) {
  for (const double sign : {1.0, -1.0}) {
    SCOPED_TRACE(sign);
    auto f = make_fixture(sign);
    const auto nav_before = book::compute_claims_nav(f.state, sign * 36.0);
    ASSERT_TRUE(nav_before.has_value()) << nav_before.error().message();
    EXPECT_NEAR(nav_before->nav, sign > 0.0 ? 136.0 : 64.0, kValueTolerance);
    EXPECT_DOUBLE_EQ(nav_before->signed_pending_claims, 0.0);

    const auto plan = plan_for(f);
    ASSERT_TRUE(plan.claim.has_value());
    const auto receipt = commit(plan, f);
    ASSERT_TRUE(receipt.has_value()) << receipt.error().message();

    EXPECT_EQ(f.state.state_version, 8U);
    EXPECT_EQ(f.state.last_applied_sequence, 1U);
    EXPECT_EQ(f.state.event_count, 1U);
    EXPECT_EQ(f.state.claim_count, 1U);
    EXPECT_EQ(f.state.retired_count, 1U);
    EXPECT_EQ(f.state.movement_count, 3U);
    EXPECT_EQ(receipt->first_movement, 0U);
    EXPECT_EQ(receipt->movement_count, 3U);
    EXPECT_EQ(receipt->to_state_version, 8U);
    EXPECT_DOUBLE_EQ(f.state.settled_cash, 100.0); // Claims are never spendable cash.

    EXPECT_DOUBLE_EQ(f.book.tri_units[0], 0.0);
    EXPECT_DOUBLE_EQ(f.book.marked_values[0], 0.0);
    EXPECT_NEAR(f.book.tri_units[1], sign * 0.2, kUnitTolerance);
    EXPECT_NEAR(f.book.marked_values[1], sign * 12.0, kValueTolerance);
    EXPECT_DOUBLE_EQ(f.book.marked_values[2], 0.0);
    EXPECT_EQ(f.book.retired[0], atx::u8{1});
    EXPECT_EQ(f.book.retired[1], atx::u8{0});
    EXPECT_EQ(f.book.retired[2], atx::u8{0});

    EXPECT_EQ(f.state.applied_events[0].event_id, 1U);
    EXPECT_EQ(f.state.applied_events[0].sequence, 1U);
    EXPECT_EQ(f.state.known_claim_ids[0], 101U);
    EXPECT_EQ(f.state.claims[0].status, book::ClaimSlotStatus::Pending);
    EXPECT_EQ(f.state.retired[0].instrument, 0U);
    EXPECT_EQ(f.state.retired[0].retired_at_period, 3U);
    EXPECT_EQ(f.state.retired[0].retired_at_state_version, 8U);

    EXPECT_EQ(f.state.movements[0].kind, book::MandatoryMovementKind::PredecessorRetirement);
    EXPECT_EQ(f.state.movements[0].instrument, 0U);
    EXPECT_NEAR(f.state.movements[0].value_before, sign * 60.0, kValueTolerance);
    EXPECT_DOUBLE_EQ(f.state.movements[0].value_after, 0.0);
    EXPECT_EQ(f.state.movements[1].kind, book::MandatoryMovementKind::SuccessorDelivery);
    EXPECT_EQ(f.state.movements[1].instrument, 1U);
    EXPECT_NEAR(f.state.movements[1].units_after, sign * 0.2, kUnitTolerance);
    EXPECT_NEAR(f.state.movements[1].valuation_bridge, sign * 0.2946, kValueTolerance);
    EXPECT_EQ(f.state.movements[2].kind, book::MandatoryMovementKind::ClaimRecognition);
    EXPECT_EQ(f.state.movements[2].instrument, book::kNoInstrument);
    EXPECT_NEAR(f.state.movements[2].claim_delta, sign * 24.2946, kValueTolerance);
    EXPECT_DOUBLE_EQ(f.state.movements[2].settled_cash_delta, 0.0);

    const double equities = sign * 12.0;
    EXPECT_NEAR(receipt->nav_after.nav, sign > 0.0 ? 136.2946 : 63.7054, kValueTolerance);
    EXPECT_NEAR(receipt->nav_after.marked_equities, equities, kValueTolerance);
    EXPECT_NEAR(receipt->nav_after.gross_receivable, sign > 0.0 ? 24.2946 : 0.0, kValueTolerance);
    EXPECT_NEAR(receipt->nav_after.gross_payable, sign < 0.0 ? 24.2946 : 0.0, kValueTolerance);
    EXPECT_NEAR(receipt->nav_after.nav - (sign > 0.0 ? 136.0 : 64.0), sign * 0.2946,
                kValueTolerance);

    std::array<book::PendingTransitionCashClaim, book::kMaxPendingClaims> scratch{};
    ASSERT_EQ(book::pending_claims_view(f.state, scratch).size(), 1U);
    EXPECT_EQ(book::pending_claims_view(f.state, scratch)[0].cash_claim_id, 101U);

    const auto paid = payment_plan(f, plan);
    const auto settled = book::commit_cash_claim_payment(paid, 4, f.state);
    ASSERT_TRUE(settled.has_value()) << settled.error().message();
    EXPECT_NEAR(f.state.settled_cash, sign > 0.0 ? 124.2946 : 75.7054, kValueTolerance);
    EXPECT_EQ(f.state.claims[0].status, book::ClaimSlotStatus::Settled);
    EXPECT_EQ(f.state.payment_count, 1U);
    EXPECT_EQ(f.state.applied_payments[0], 201U);
    EXPECT_EQ(f.state.movement_count, 4U);
    EXPECT_EQ(f.state.movements[3].kind, book::MandatoryMovementKind::ClaimSettlement);
    EXPECT_EQ(f.state.movements[3].payment_id, 201U);
    EXPECT_NEAR(f.state.movements[3].settled_cash_delta, sign * 24.2946, kValueTolerance);
    EXPECT_NEAR(f.state.movements[3].accounting_residual, 0.0, kValueTolerance);
    EXPECT_EQ(f.state.state_version, 9U);
    EXPECT_EQ(f.state.last_applied_sequence, 2U);
    EXPECT_EQ(book::pending_claims_view(f.state, scratch).size(), 0U);

    // Settlement moves value between NAV terms and changes NAV by nothing.
    const auto nav_after = book::compute_claims_nav(f.state, equities);
    ASSERT_TRUE(nav_after.has_value()) << nav_after.error().message();
    EXPECT_NEAR(nav_after->nav, receipt->nav_after.nav, kValueTolerance);
    EXPECT_DOUBLE_EQ(nav_after->signed_pending_claims, 0.0);
  }
}

TEST(ClaimsState, BoundedCountsAndDuplicateIdsReject) {
  const auto reject_transition = [](auto&& mutate) {
    auto f = make_fixture(1.0);
    const auto plan = plan_for(f);
    mutate(f.state);
    EXPECT_FALSE(commit(plan, f).has_value());
  };

  reject_transition([](book::ClaimsBookState& s) {
    for (atx::usize i = 0; i < book::kMaxTransitionEvents; ++i) {
      s.applied_events[i] = {static_cast<atx::u64>(1000 + i), atx::u32{1}, atx::u64{1}};
    }
    s.event_count = book::kMaxTransitionEvents;
  });
  reject_transition([](book::ClaimsBookState& s) {
    for (atx::usize i = 0; i < book::kMaxPendingClaims; ++i) {
      s.known_claim_ids[i] = static_cast<atx::u64>(2000 + i);
      s.claims[i].status = book::ClaimSlotStatus::Settled;
    }
    s.claim_count = book::kMaxPendingClaims;
  });
  reject_transition([](book::ClaimsBookState& s) {
    for (atx::usize i = 0; i < book::kMaxTransitionEvents; ++i) {
      s.retired[i].instrument = 128 + i; // Disjoint from the event's own axes.
    }
    s.retired_count = book::kMaxTransitionEvents;
  });
  reject_transition([](book::ClaimsBookState& s) {
    s.movement_count = book::kMaxMandatoryMovements - 2; // Three rows do not fit.
  });
  reject_transition([](book::ClaimsBookState& s) {
    s.applied_events[0] = {atx::u64{1}, atx::u32{1}, atx::u64{1}}; // Duplicate event ID.
    s.event_count = 1;
  });
  reject_transition([](book::ClaimsBookState& s) {
    s.known_claim_ids[0] = 101U; // Duplicate cash claim ID.
    s.claims[0].status = book::ClaimSlotStatus::Settled;
    s.claim_count = 1;
  });

  auto f = make_fixture(1.0);
  const auto plan = plan_for(f);
  ASSERT_TRUE(commit(plan, f).has_value());
  const auto paid = payment_plan(f, plan);

  auto duplicate_payment = f.state;
  duplicate_payment.applied_payments[0] = 201U;
  duplicate_payment.payment_count = 1;
  EXPECT_FALSE(book::commit_cash_claim_payment(paid, 4, duplicate_payment).has_value());

  auto payments_full = f.state;
  for (atx::usize i = 0; i < book::kMaxAppliedPayments; ++i) {
    payments_full.applied_payments[i] = static_cast<atx::u64>(3000 + i);
  }
  payments_full.payment_count = book::kMaxAppliedPayments;
  EXPECT_FALSE(book::commit_cash_claim_payment(paid, 4, payments_full).has_value());

  auto movements_full = f.state;
  movements_full.movement_count = book::kMaxMandatoryMovements;
  EXPECT_FALSE(book::commit_cash_claim_payment(paid, 4, movements_full).has_value());

  // The same payment against the untouched state still commits, so each
  // rejection above is the bound under test and not a broken fixture.
  EXPECT_TRUE(book::commit_cash_claim_payment(paid, 4, f.state).has_value());
}

TEST(ClaimsState, RejectionsLeaveStateAndSpansByteIdentical) {
  const auto reject = [](auto&& mutate) {
    auto f = make_fixture(1.0);
    const auto plan = plan_for(f);
    mutate(f);
    const auto state_before = snapshot(f.state);
    const auto book_before = snapshot(f.book);
    EXPECT_FALSE(commit(plan, f).has_value());
    expect_unchanged(f.state, state_before);
    expect_unchanged(f.book, book_before);
  };

  reject([](Fixture& f) { f.state.state_version = 9; });            // Stale version.
  reject([](Fixture& f) { f.state.last_applied_sequence = 5; });    // Wrong sequence.
  reject([](Fixture& f) { f.book.tri_units[0] = 3.0; });            // Mismatched before-units.
  reject([](Fixture& f) { f.book.marked_values[1] = 7.0; });        // Mismatched before-values.
  reject([](Fixture& f) { f.book.retired[0] = atx::u8{1}; });       // Retired predecessor.
  reject([](Fixture& f) { f.book.retired[1] = atx::u8{1}; });       // Retired successor.
  reject([](Fixture& f) { f.state.settled_cash = 101.0; });         // Cash moved under the plan.

  // Nonfinite projected NAV: an untouched name's mark is not a "before" value
  // the plan pinned, so only the NAV projection can catch it.
  reject([](Fixture& f) {
    f.book.marked_values[2] = std::numeric_limits<double>::infinity();
  });

  // Nonpositive projected NAV. The cash must be negative BEFORE planning, or the
  // plan/settled-cash agreement check would fire first and prove nothing.
  {
    auto f = make_fixture(1.0);
    f.state.settled_cash = -100.0;
    const auto plan = plan_for(f);
    const auto state_before = snapshot(f.state);
    const auto book_before = snapshot(f.book);
    EXPECT_FALSE(commit(plan, f).has_value()); // -100 + 12 + 24.2946 <= 0.
    expect_unchanged(f.state, state_before);
    expect_unchanged(f.book, book_before);
  }

  { // compute_claims_nav refuses the same two conditions on its own.
    const auto f = make_fixture(1.0);
    EXPECT_FALSE(
        book::compute_claims_nav(f.state, std::numeric_limits<double>::infinity()).has_value());
    EXPECT_FALSE(book::compute_claims_nav(f.state, -100.0).has_value()); // NAV exactly 0.
    EXPECT_TRUE(book::compute_claims_nav(f.state, 36.0).has_value());
  }

  { // Successor index outside the supplied book spans.
    auto f = make_fixture(1.0);
    const auto plan = plan_for(f);
    const auto state_before = snapshot(f.state);
    const auto book_before = snapshot(f.book);
    const auto result = book::commit_security_transition(
        plan, 3, std::span<double>(f.book.tri_units).first(1),
        std::span<double>(f.book.marked_values).first(1),
        std::span<atx::u8>(f.book.retired).first(1), f.state);
    EXPECT_FALSE(result.has_value());
    expect_unchanged(f.state, state_before);
    expect_unchanged(f.book, book_before);
  }

  auto f = make_fixture(1.0);
  const auto plan = plan_for(f);
  ASSERT_TRUE(commit(plan, f).has_value());
  const auto paid = payment_plan(f, plan);

  const auto reject_payment = [&f, &paid](auto&& mutate) {
    auto state = f.state;
    auto local = paid;
    mutate(state, local);
    const auto before = snapshot(state);
    EXPECT_FALSE(book::commit_cash_claim_payment(local, 4, state).has_value());
    expect_unchanged(state, before);
  };

  reject_payment([](book::ClaimsBookState& s, book::CashClaimPaymentPlan&) {
    s.claim_count = 0; // Unknown cash claim ID.
  });
  reject_payment([](book::ClaimsBookState& s, book::CashClaimPaymentPlan&) {
    s.applied_payments[0] = 201U; // Duplicate payment ID.
    s.payment_count = 1;
  });
  reject_payment([](book::ClaimsBookState& s, book::CashClaimPaymentPlan&) {
    s.state_version += 1; // Stale version.
  });
  reject_payment([](book::ClaimsBookState&, book::CashClaimPaymentPlan& p) {
    p.payment.amount += 1.0; // Mismatched payment amount.
  });
  reject_payment([](book::ClaimsBookState& s, book::CashClaimPaymentPlan&) {
    s.claims[0].claim.amount += 1.0; // Retained slot differs from the plan's claim.
  });
  reject_payment([](book::ClaimsBookState&, book::CashClaimPaymentPlan& p) {
    p.claim_after.event_id += 1; // Forged post-settlement claim identity.
  });
}
