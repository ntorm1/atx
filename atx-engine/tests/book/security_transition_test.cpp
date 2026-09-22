#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <locale>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "atx/engine/book/security_transition.hpp"

namespace {
namespace book = atx::engine::book;
namespace data = atx::engine::data;
constexpr double kValueTolerance = 1.0e-12;

data::TransitionDigest digest(atx::u8 value) {
  data::TransitionDigest result{};
  result.fill(value);
  return result;
}

data::TransitionEvidence evidence(atx::u8 id, atx::i64 available_at) {
  return {data::TransitionEvidenceNamespace::SyntheticFixture, digest(id), available_at};
}

struct TransitionFixture {
  data::SecurityTransition event;
  data::TransitionBasis basis;
  book::TransitionHoldingView state;
  book::TransitionAdmission admission;
};

TransitionFixture fixture(double sign = 1.0) {
  TransitionFixture result;
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

  auto& state = result.state;
  state.state_version = event.expected_state_version;
  state.predecessor = event.predecessor;
  state.successor = event.successor;
  state.source_artifact_sha256 = basis.source_artifact_sha256;
  state.axes_sha256 = basis.axes_sha256;
  state.adjustment_recipe_sha256 = basis.adjustment_recipe_sha256;
  state.predecessor_tri_units = sign * 2.0;
  state.predecessor_accounted_value = sign * 60.0;
  state.successor_tri_units = sign * -0.4; // Opposite two research share equivalents.
  state.successor_marked_value = sign * -24.0;
  state.settled_cash = 100.0;
  state.last_valuation_at_ns = 100;
  state.next_valuation_at_ns = 200;
  result.admission = {book::TransitionMode::SyntheticFixture,
                      book::TransitionUnitConvention::ResearchReinvestedShareEquivalent,
                      book::TransitionClaimValuation::FixedUsdFaceValue, 250, 64};
  return result;
}

// Unit raw/TRI prices on both sides, so units equal values and every leg is
// exactly representable. Each numerical case below then isolates one addition
// or one admitted mark discrepancy instead of fighting the canonical rounding.
TransitionFixture unit_price_fixture(double predecessor_units, double successor_units) {
  auto result = fixture();
  result.basis.predecessor.raw_price = 1.0;
  result.basis.predecessor.tri_price = 1.0;
  result.basis.successor.raw_price = 1.0;
  result.basis.successor.tri_price = 1.0;
  result.state.predecessor_tri_units = predecessor_units;
  result.state.predecessor_accounted_value = predecessor_units;
  result.basis.entitled_predecessor_shares = predecessor_units;
  result.state.successor_tri_units = successor_units;
  result.state.successor_marked_value = successor_units;
  return result;
}

auto state_numbers(const book::TransitionHoldingView& state) {
  return std::array<atx::u64, 8>{
      state.state_version, state.last_applied_sequence,
      std::bit_cast<atx::u64>(state.predecessor_tri_units),
      std::bit_cast<atx::u64>(state.predecessor_accounted_value),
      std::bit_cast<atx::u64>(state.successor_tri_units),
      std::bit_cast<atx::u64>(state.successor_marked_value),
      std::bit_cast<atx::u64>(state.settled_cash),
      static_cast<atx::u64>(state.predecessor_retired)};
}

void expect_rejected_without_state_change(const TransitionFixture& input) {
  const auto before = state_numbers(input.state);
  const auto result = book::plan_security_transition(
      input.event, input.basis, input.state, input.admission);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(state_numbers(input.state), before);
}

struct PaymentFixture {
  data::CashClaimPayment payment;
  book::CashClaimPaymentView state;
};

PaymentFixture payment_fixture(const book::SecurityTransitionPlan& plan) {
  PaymentFixture result;
  result.payment.payment_id = 201;
  result.payment.sequence = 2;
  result.payment.expected_state_version = plan.to_state_version;
  result.payment.cash_claim_id = plan.event.cash_claim_id;
  result.payment.event_id = plan.event.event_id;
  result.payment.event_revision = plan.event.revision;
  result.payment.currency = data::TransitionCurrency::USD;
  result.payment.amount = plan.signed_cash_claim;
  result.payment.allocated_at_ns = 240;
  result.payment.evidence = evidence(9, 240);
  result.state.state_version = plan.to_state_version;
  result.state.last_applied_sequence = plan.event.sequence;
  result.state.settled_cash = plan.cash_after;
  result.state.claim = plan.claim.value(); // Callers first assert a nonzero claim.
  return result;
}

void expect_payment_rejected(const PaymentFixture& input,
                             const book::TransitionAdmission& admission) {
  const auto cash_before = std::bit_cast<atx::u64>(input.state.settled_cash);
  const auto claim_before = std::bit_cast<atx::u64>(input.state.claim.amount);
  const auto settled_before = input.state.claim.settled;
  const auto state_before = input.state.state_version;
  const auto result = book::plan_cash_claim_payment(input.payment, input.state, admission);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(std::bit_cast<atx::u64>(input.state.settled_cash), cash_before);
  EXPECT_EQ(std::bit_cast<atx::u64>(input.state.claim.amount), claim_before);
  EXPECT_EQ(input.state.claim.settled, settled_before);
  EXPECT_EQ(input.state.state_version, state_before);
}

void append_numbers(std::ostringstream& out,
                    std::initializer_list<std::pair<std::string_view, double>> numbers) {
  out << '{';
  bool first = true;
  for (const auto& [name, value] : numbers) {
    if (!first) out << ',';
    first = false;
    out << '"' << name << "\":";
    EXPECT_TRUE(std::isfinite(value)) << name;
    if (std::isfinite(value)) out << value;
    else out << "null";
  }
  out << '}';
}

// Coordinator-captured stdout is the only measurement sink. No files, JSON
// dependency, environment variables, or separate native executable are needed.
void emit_measurement(std::string_view case_id, const book::SecurityTransitionPlan& plan,
                      const book::CashClaimPaymentPlan& paid) {
  const auto& basis = plan.basis;
  const double nav_before = plan.cash_before + plan.removed_predecessor_value +
                             plan.successor_value_before;
  const double nav_after = plan.cash_after + plan.successor_value_after + plan.signed_cash_claim;
  const double nav_paid = paid.cash_after + plan.successor_value_after + paid.claim_after.amount;
  const double stock_ratio = static_cast<double>(plan.event.stock_ratio.numerator) /
                              static_cast<double>(plan.event.stock_ratio.denominator);
  const double cash_ratio = static_cast<double>(plan.event.cash_per_predecessor_share.numerator) /
      static_cast<double>(plan.event.cash_per_predecessor_share.denominator);
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<double>::max_digits10)
      << "SECURITY_TRANSITION_MEASUREMENT {\"schema\":\"atx-security-transition-measurement-v1\","
      << "\"case_id\":\"" << case_id << "\",\"inputs\":";
  append_numbers(out, {
      {"old_units", plan.predecessor_units_before}, {"old_raw", basis.predecessor.raw_price},
      {"old_tri", basis.predecessor.tri_price},
      {"successor_units_before", plan.successor_units_before},
      {"successor_raw", basis.successor.raw_price}, {"successor_tri", basis.successor.tri_price},
      {"successor_shares_per_old_share", stock_ratio}, {"cash_per_old_share", cash_ratio},
      {"entitled_pre_split_share_equivalents", basis.entitled_predecessor_shares},
      {"settled_cash_before", plan.cash_before}});
  out << ",\"values\":";
  append_numbers(out, {
      {"old_share_equivalents", plan.converted_predecessor_shares},
      {"entitled_share_equivalents", basis.entitled_predecessor_shares},
      {"old_units_after", plan.predecessor_units_after},
      {"old_units_delta", plan.predecessor_units_after - plan.predecessor_units_before},
      {"removed_old_marked_value", plan.removed_predecessor_value},
      {"successor_share_equivalents_delivered", plan.successor_share_equivalents},
      {"successor_units_added", plan.successor_units_added},
      {"successor_units_after", plan.successor_units_after},
      {"successor_value_before", plan.successor_value_before},
      {"successor_value_after", plan.successor_value_after},
      {"incremental_successor_value", plan.actual_successor_value_added},
      {"pending_signed_cash_claim", plan.signed_cash_claim},
      {"gross_receivable", plan.gross_cash_receivable}, {"gross_payable", plan.gross_cash_payable},
      {"settled_cash_after_transition", plan.cash_after},
      {"transition_settled_cash_delta", plan.cash_after - plan.cash_before},
      {"nav_before", nav_before}, {"nav_after_transition", nav_after},
      {"transition_value_bridge", plan.valuation_bridge},
      {"payment_cash_delta", paid.actual_cash_delta}, {"payment_claim_delta", paid.claim_delta},
      {"settled_cash_after_payment", paid.cash_after},
      {"claim_after_full_payment", paid.claim_after.amount}, {"nav_after_payment", nav_paid},
      {"payment_nav_change", nav_paid - nav_after}});
  out << ",\"residuals\":";
  append_numbers(out, {{"representation_residual", plan.representation_residual},
                        {"transition_accounting_residual", plan.accounting_residual},
                        {"payment_accounting_residual", paid.accounting_residual}});
  out << '}';
  std::cout << out.str() << '\n';
}
} // namespace

TEST(SecurityTransition, SignedStockCashAndFullPaymentPreserveAttributedExposureAndNav) {
  for (const double sign : {1.0, -1.0}) {
    SCOPED_TRACE(sign);
    const auto input = fixture(sign);
    const auto before = state_numbers(input.state);
    const auto result = book::plan_security_transition(
        input.event, input.basis, input.state, input.admission);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_TRUE(result->claim.has_value());
    EXPECT_EQ(state_numbers(input.state), before);
    EXPECT_EQ(result->from_state_version, 7U);
    EXPECT_EQ(result->to_state_version, 8U);
    EXPECT_TRUE(result->retire_predecessor);
    EXPECT_DOUBLE_EQ(result->predecessor_units_after, 0.0);
    EXPECT_DOUBLE_EQ(result->converted_predecessor_shares, sign * 6.0);
    EXPECT_DOUBLE_EQ(result->successor_share_equivalents, sign * 3.0);
    EXPECT_NEAR(result->successor_units_added, sign * 0.6, 1.0e-15);
    EXPECT_NEAR(result->successor_units_after, sign * 0.2, 1.0e-15);
    EXPECT_NEAR(result->actual_successor_value_added, sign * 36.0, kValueTolerance);
    EXPECT_NEAR(result->successor_value_after, sign * 12.0, kValueTolerance);
    EXPECT_NEAR(result->signed_cash_claim, sign * 24.2946, kValueTolerance);
    EXPECT_DOUBLE_EQ(result->cash_before, 100.0);
    EXPECT_DOUBLE_EQ(result->cash_after, 100.0); // Pending claims are not spendable cash.
    EXPECT_NEAR(result->valuation_bridge, sign * 0.2946, kValueTolerance);
    EXPECT_NEAR(result->gross_cash_receivable, sign > 0.0 ? 24.2946 : 0.0, kValueTolerance);
    EXPECT_NEAR(result->gross_cash_payable, sign < 0.0 ? 24.2946 : 0.0, kValueTolerance);
    EXPECT_FALSE(result->claim->settled);
    const double nav_before = sign > 0.0 ? 136.0 : 64.0;
    const double nav_after = result->cash_after + result->successor_value_after +
                             result->signed_cash_claim;
    EXPECT_NEAR(nav_after, sign > 0.0 ? 136.2946 : 63.7054, kValueTolerance);
    EXPECT_NEAR(nav_after - nav_before, result->valuation_bridge, kValueTolerance);

    const auto payment = payment_fixture(*result);
    const auto paid = book::plan_cash_claim_payment(payment.payment, payment.state,
                                                    input.admission);
    ASSERT_TRUE(paid.has_value()) << paid.error().message();
    EXPECT_NEAR(paid->cash_after, sign > 0.0 ? 124.2946 : 75.7054, kValueTolerance);
    EXPECT_NEAR(paid->actual_cash_delta, sign * 24.2946, kValueTolerance);
    EXPECT_NEAR(paid->claim_delta, sign * -24.2946, kValueTolerance);
    EXPECT_DOUBLE_EQ(paid->claim_after.amount, 0.0);
    EXPECT_TRUE(paid->claim_after.settled);
    EXPECT_NEAR(paid->cash_after + result->successor_value_after, nav_after, kValueTolerance);
    EXPECT_NEAR(paid->accounting_residual, 0.0, kValueTolerance);
    EXPECT_DOUBLE_EQ(payment.state.settled_cash, 100.0);
    EXPECT_FALSE(payment.state.claim.settled);
    emit_measurement(sign > 0.0 ? "long-canonical" : "short-canonical", *result, *paid);
  }

  // Exact cancellation is genuine netting, not an absorbed stock component.
  auto input = fixture();
  input.state.predecessor_tri_units = 1.0;
  input.state.predecessor_accounted_value = 1.0;
  input.basis.predecessor.raw_price = 1.0;
  input.basis.predecessor.tri_price = 1.0;
  input.basis.entitled_predecessor_shares = 1.0;
  input.basis.successor.raw_price = 10.0;
  input.basis.successor.tri_price = 10.0;
  input.state.successor_tri_units = -0.5;
  input.state.successor_marked_value = -5.0;
  const auto netted = book::plan_security_transition(
      input.event, input.basis, input.state, input.admission);
  ASSERT_TRUE(netted.has_value()) << netted.error().message();
  EXPECT_DOUBLE_EQ(netted->successor_units_after, 0.0);
  EXPECT_DOUBLE_EQ(netted->actual_successor_value_added, 5.0);
  EXPECT_NEAR(netted->signed_cash_claim, 4.0491, kValueTolerance);
}

TEST(SecurityTransition, IndependentTriScalesPreserveEconomicsAndUnprovedBasisRejects) {
  for (const auto scales : {std::array<double, 2>{8.0, 1.0},
                            std::array<double, 2>{1.0, 4.0},
                            std::array<double, 2>{8.0, 4.0}}) {
    SCOPED_TRACE(::testing::Message() << scales[0] << "," << scales[1]);
    auto input = fixture();
    input.basis.predecessor.tri_price *= scales[0];
    input.state.predecessor_tri_units /= scales[0];
    input.basis.successor.tri_price *= scales[1];
    input.state.successor_tri_units /= scales[1];
    const auto result = book::plan_security_transition(
        input.event, input.basis, input.state, input.admission);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_DOUBLE_EQ(result->converted_predecessor_shares, 6.0);
    EXPECT_DOUBLE_EQ(result->successor_share_equivalents, 3.0);
    EXPECT_NEAR(result->successor_units_added * scales[1], 0.6, 1.0e-15);
    EXPECT_NEAR(result->successor_units_after * scales[1], 0.2, 1.0e-15);
    EXPECT_NEAR(result->actual_successor_value_added, 36.0, kValueTolerance);
    EXPECT_NEAR(result->signed_cash_claim, 24.2946, kValueTolerance);
    EXPECT_NEAR(result->valuation_bridge, 0.2946, kValueTolerance);
    ASSERT_TRUE(result->claim.has_value());
    const auto payment = payment_fixture(*result);
    const auto paid = book::plan_cash_claim_payment(payment.payment, payment.state,
                                                    input.admission);
    ASSERT_TRUE(paid.has_value()) << paid.error().message();
    const std::string_view case_id = scales[0] == 1.0 ? "long-successor_scale_4" :
        (scales[1] == 1.0 ? "long-old_scale_8" : "long-independent_scales_8_4");
    emit_measurement(case_id, *result, *paid);
  }

  for (const auto coverage : {data::TransitionCoverage::Unknown,
                              data::TransitionCoverage::EmbeddedInTri}) {
    for (const auto component : {0, 1, 2}) {
      SCOPED_TRACE(::testing::Message() << static_cast<int>(coverage) << "," << component);
      auto input = fixture();
      if (component == 0) input.basis.stock_coverage = coverage;
      if (component == 1) input.basis.cash_coverage = coverage;
      if (component == 2) input.basis.continuity_coverage = coverage;
      expect_rejected_without_state_change(input);
    }
  }
  for (const double entitlement : {3.0, 12.0}) {
    auto input = fixture();
    input.basis.entitled_predecessor_shares = entitlement;
    expect_rejected_without_state_change(input);
  }
  auto input = fixture();
  input.event.cash_denomination = data::TransitionShareDenomination::Unknown;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.admission.units = book::TransitionUnitConvention::PhysicalShares;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.basis.successor.tri_price = std::numeric_limits<double>::quiet_NaN();
  expect_rejected_without_state_change(input);
  input = fixture();
  input.state.successor_tri_units = 1.0e300;
  input.state.successor_marked_value = 6.0e301;
  expect_rejected_without_state_change(input); // Nonzero incoming stock cannot disappear.
  input = fixture();
  input.basis.predecessor.raw_price = 1.0;
  input.basis.predecessor.tri_price = 1.0;
  input.state.predecessor_tri_units = std::numeric_limits<double>::denorm_min();
  input.state.predecessor_accounted_value = std::numeric_limits<double>::denorm_min();
  input.basis.entitled_predecessor_shares = std::numeric_limits<double>::denorm_min();
  expect_rejected_without_state_change(input); // The half-share leg underflows to zero.

  const auto canonical = fixture();
  const auto plan = book::plan_security_transition(
      canonical.event, canonical.basis, canonical.state, canonical.admission);
  ASSERT_TRUE(plan.has_value()) << plan.error().message();
  ASSERT_TRUE(plan->claim.has_value());
  auto payment = payment_fixture(*plan);
  payment.state.settled_cash = 1.0e300;
  expect_payment_rejected(payment, canonical.admission);
}

TEST(SecurityTransition, OrderingIdentityAndAdmissionFailuresAreAtomic) {
  const std::array<book::AppliedSecurityTransition, 1> already_applied{{{1, 1, 1}}};
  const std::array<book::AppliedSecurityTransition, 1> conflicting_revision{{{1, 2, 1}}};
  const std::array<atx::u64, 1> known_claims{101};
  auto input = fixture();
  input.state.last_applied_sequence = 1;
  input.event.sequence = 2;
  input.state.applied_events = already_applied;
  expect_rejected_without_state_change(input);
  input.state.applied_events = conflicting_revision;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.state.known_cash_claim_ids = known_claims;
  expect_rejected_without_state_change(input);
  input = fixture();
  ++input.state.state_version;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.event.sequence = 2;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.state.predecessor_retired = true;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.event.evidence.available_at_ns.reset();
  expect_rejected_without_state_change(input);
  input = fixture();
  input.basis.evidence.available_at_ns = 251;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.basis.evidence.available_at_ns = 199; // Cannot know the successor mark before observation.
  expect_rejected_without_state_change(input);
  input = fixture();
  input.event.effective_at_ns = 100; // Cannot reach backward over an earlier valuation.
  expect_rejected_without_state_change(input);
  input = fixture();
  input.basis.successor.observed_at_ns = 201;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.event.successor.instrument = input.event.predecessor.instrument;
  input.basis.successor.security = input.event.successor;
  input.state.successor = input.event.successor;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.event.predecessor.mapping_sha256 = {};
  input.basis.predecessor.security = input.event.predecessor;
  input.state.predecessor = input.event.predecessor;
  expect_rejected_without_state_change(input);
  input = fixture();
  input.basis.axes_sha256 = digest(99);
  expect_rejected_without_state_change(input);
  input = fixture();
  input.admission.max_guard_records = 0;
  expect_rejected_without_state_change(input);
  input = fixture();
  const std::array<book::AppliedSecurityTransition, 1> unrelated_events{{{99, 1, 1}}};
  const std::array<atx::u64, 1> unrelated_claims{999};
  input.state.applied_events = unrelated_events;
  input.state.known_cash_claim_ids = unrelated_claims;
  input.state.last_applied_sequence = 1;
  input.event.sequence = 2;
  input.admission.max_guard_records = 2;
  const auto at_limit = book::plan_security_transition(
      input.event, input.basis, input.state, input.admission);
  ASSERT_TRUE(at_limit.has_value()) << at_limit.error().message();
  input.admission.max_guard_records = 1;
  expect_rejected_without_state_change(input);
  for (const auto mode : {book::TransitionMode::Unknown, book::TransitionMode::StrictAsOf,
                          book::TransitionMode::RetrospectiveReconstruction}) {
    input = fixture();
    input.admission.mode = mode;
    expect_rejected_without_state_change(input);
  }

  input = fixture();
  const auto plan = book::plan_security_transition(
      input.event, input.basis, input.state, input.admission);
  ASSERT_TRUE(plan.has_value()) << plan.error().message();
  ASSERT_TRUE(plan->claim.has_value());
  const std::array<atx::u64, 1> paid_ids{201};
  auto payment = payment_fixture(*plan);
  payment.state.applied_payment_ids = paid_ids;
  expect_payment_rejected(payment, input.admission);
  payment = payment_fixture(*plan);
  ++payment.state.state_version;
  expect_payment_rejected(payment, input.admission);
  payment = payment_fixture(*plan);
  payment.payment.amount *= 0.5;
  expect_payment_rejected(payment, input.admission);
  payment = payment_fixture(*plan);
  payment.payment.allocated_at_ns = 199; // Before the claim was recognized.
  expect_payment_rejected(payment, input.admission);
  payment = payment_fixture(*plan);
  payment.payment.evidence.available_at_ns = 239; // Cannot attest an allocation before it occurs.
  expect_payment_rejected(payment, input.admission);
  payment = payment_fixture(*plan);
  ++payment.payment.event_revision;
  expect_payment_rejected(payment, input.admission);
  payment = payment_fixture(*plan);
  payment.state.claim.settled = true;
  expect_payment_rejected(payment, input.admission);
  EXPECT_EQ(already_applied[0].event_id, 1U);
  EXPECT_EQ(conflicting_revision[0].revision, 2U);
  EXPECT_EQ(known_claims[0], 101U);
  EXPECT_EQ(paid_ids[0], 201U);
}

// Regression cover for the three numerical-contract defects found in review:
// a supplied-versus-recomputed successor value mix, a self-restating accounting
// identity, and unguarded additions in the reconciliation chain.
TEST(SecurityTransition, AdmittedMarkToleranceAndAbsorbedAdditionsAreEnforced) {
  constexpr double kEps = std::numeric_limits<double>::epsilon();

  // A correct-but-not-bit-identical successor mark is admitted, and the small
  // delivered increment is still measured against the recomputed value. Before
  // the fix the admitted 1e-9 drift on a 1e6 position swamped the increment's
  // own 64*eps*1.0 tolerance and rejected a valid plan.
  auto input = unit_price_fixture(2.0, 1.0e6);
  input.state.successor_marked_value = 1.0e6 + 1.0e-9; // Inside 64*eps*1e6.
  const auto drifted = book::plan_security_transition(input.event, input.basis, input.state,
                                                      input.admission);
  ASSERT_TRUE(drifted.has_value()) << drifted.error().message();
  EXPECT_DOUBLE_EQ(drifted->successor_value_before, 1.0e6);
  EXPECT_DOUBLE_EQ(drifted->successor_value_after, 1000001.0);
  EXPECT_DOUBLE_EQ(drifted->actual_successor_value_added, 1.0);
  EXPECT_DOUBLE_EQ(drifted->expected_successor_value_added, 1.0);
  input = unit_price_fixture(2.0, 1.0e6);
  input.state.successor_marked_value = 1000000.1; // Outside 64*eps*1e6.
  expect_rejected_without_state_change(input);

  // The reported bridge must be reproduced by the four retained magnitudes,
  // bounded by the largest of them rather than by their cancelled difference.
  const auto canonical = fixture();
  const auto plan = book::plan_security_transition(canonical.event, canonical.basis,
                                                   canonical.state, canonical.admission);
  ASSERT_TRUE(plan.has_value()) << plan.error().message();
  const double identity = (plan->successor_value_after + plan->signed_cash_claim) -
                          (plan->removed_predecessor_value + plan->successor_value_before);
  const double scale = std::max({std::abs(plan->successor_value_after),
                                 std::abs(plan->signed_cash_claim),
                                 std::abs(plan->removed_predecessor_value),
                                 std::abs(plan->successor_value_before)});
  EXPECT_LE(std::abs(identity - plan->valuation_bridge), 64.0 * kEps * scale);
  input = fixture();
  input.state.predecessor_accounted_value = 61.0; // Two units at TRI 30 is 60.
  expect_rejected_without_state_change(input);

  // A legitimate valuation-interval bridge dominated by the removed leg. The old
  // check restated the bridge as before + bridge and bounded the restatement by
  // the tiny post-transition value, so cancellation error in 1e8 - 99999999.99999999
  // rejected a correct plan; the scaled bound is 64*eps*1e8 instead.
  input = unit_price_fixture(1.0e8, 0.0);
  input.event.stock_ratio = {1, atx::u64{1} << 53};
  input.event.cash_per_predecessor_share = {0, 1}; // No claim at all.
  const auto wide_bridge = book::plan_security_transition(input.event, input.basis, input.state,
                                                          input.admission);
  ASSERT_TRUE(wide_bridge.has_value()) << wide_bridge.error().message();
  EXPECT_FALSE(wide_bridge->claim.has_value());
  EXPECT_NEAR(wide_bridge->valuation_bridge, -99999999.99999999, 64.0 * kEps * 1.0e8);
  EXPECT_DOUBLE_EQ(wide_bridge->accounting_residual, 0.0);

  // Absorbed additions reject at each reconciliation site. Claim below half an
  // ulp of the 36.0 stock proceeds.
  input = fixture();
  input.event.cash_per_predecessor_share = {1, atx::u64{1} << 53};
  expect_rejected_without_state_change(input);
  // Existing successor value 1.0 below half an ulp of the removed 1e18. With a
  // symmetric guard the huge claim swallows the stock leg at the proceeds site
  // first; the zero-claim variant below isolates the removed-value site.
  input = unit_price_fixture(1.0e18, 1.0);
  input.event.stock_ratio = {1, atx::u64{1} << 53};
  expect_rejected_without_state_change(input);
  input.event.cash_per_predecessor_share = {0, 1};
  expect_rejected_without_state_change(input);
  // A nonzero stock leg swallowed by a huge claim: the mirrored proceeds case,
  // which a one-sided absorbed-addition guard admits. Existing successor value
  // -1e10 survives against the claim, so only the proceeds site can catch it.
  input = unit_price_fixture(9007199254740992.0, -1.0e10);
  input.event.stock_ratio = {1, atx::u64{1} << 53}; // Delivers exactly 1.0 unit.
  input.event.cash_per_predecessor_share = {11102, 1};
  expect_rejected_without_state_change(input);
  // Claim of exactly 1.0 below half an ulp of the 1e18 post-transition value.
  input = unit_price_fixture(9007199254740992.0, 1.0e18);
  input.event.stock_ratio = {256, atx::u64{1} << 53};
  input.event.cash_per_predecessor_share = {1, atx::u64{1} << 53};
  expect_rejected_without_state_change(input);

  // Payment into settled cash too large to preserve the claim leg also rejects.
  auto paid_input = unit_price_fixture(2.0, 0.0);
  paid_input.event.cash_per_predecessor_share = {1, 2}; // A claim of exactly 1.0.
  const auto paid_plan = book::plan_security_transition(
      paid_input.event, paid_input.basis, paid_input.state, paid_input.admission);
  ASSERT_TRUE(paid_plan.has_value()) << paid_plan.error().message();
  ASSERT_TRUE(paid_plan->claim.has_value());
  EXPECT_DOUBLE_EQ(paid_plan->signed_cash_claim, 1.0);
  auto payment = payment_fixture(*paid_plan);
  payment.state.settled_cash = 1.0e17;
  expect_payment_rejected(payment, paid_input.admission);
}
