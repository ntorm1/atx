#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/claims_state.hpp"
#include "atx/engine/book/borrow_schedule.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/data/security_transition.hpp"

namespace {
namespace book = atx::engine::book;
namespace data = atx::engine::data;
using atx::engine::alpha::Panel;

constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::f64 kTolerance = 1.0e-12;

Panel prices(atx::usize dates, atx::usize instruments, std::vector<atx::f64> close) {
  return Panel::create(dates, instruments, {"close"}, {std::move(close)}, {}).value();
}

std::vector<atx::i64> days(atx::usize count) {
  std::vector<atx::i64> times;
  for (atx::usize i = 0; i < count; ++i) times.push_back(static_cast<atx::i64>(i) * kDay);
  return times;
}

atx::u64 bits(atx::f64 value) { return std::bit_cast<atx::u64>(value); }

// Every numeric field of one interval as a bit pattern, so the empty-batch
// equivalence is an identity rather than a near-comparison.
std::array<atx::u64, 11> numeric(const book::ReplayInterval &row) {
  return {bits(row.pretrade_nav),  bits(row.cash),        bits(row.assets),
          bits(row.nav),           bits(row.gross_pnl),   bits(row.trade_cost),
          bits(row.borrow_cost),   bits(row.net_return),  bits(row.traded_dollars),
          bits(row.start_gross),   bits(row.end_gross)};
}

// Every scalar of one allocation certificate as a bit pattern. The vector
// payloads are compared element-wise beside it, so the whole certificate — not
// just its count — is an identity on the empty-batch path.
std::array<atx::u64, 6> numeric(const book::ReplayAllocation &row) {
  return {bits(row.pretrade_cash),   bits(row.pretrade_nav),
          bits(row.posttrade_cash),  bits(row.posttrade_nav),
          bits(row.traded_dollars),  bits(row.trade_cost)};
}

std::vector<book::ReplayTargetIntent> as_targets(std::span<const atx::f64> weights) {
  std::vector<book::ReplayTargetIntent> out;
  for (const auto weight : weights) {
    out.push_back({book::ReplayTargetAction::TargetWeight, weight});
  }
  return out;
}

book::ReplayClaimsConfig claims_config(atx::i64 cutoff) {
  book::ReplayClaimsConfig config;
  config.admission = {book::TransitionMode::SyntheticFixture,
                      book::TransitionUnitConvention::ResearchReinvestedShareEquivalent,
                      book::TransitionClaimValuation::FixedUsdFaceValue, cutoff, 64};
  config.predecessor_mark = book::TransitionPredecessorMark::CarryLastAccountedValueV1;
  config.loan = book::TransitionLoanTreatment::NoDischargeSuccessorContinuesV1;
  config.claim_financing = book::ClaimFinancing::NoneDisclosedV1;
  config.borrow_base = book::BorrowBase::MarkedShortEquityDollarsV1;
  return config;
}

data::TransitionDigest digest(atx::u8 value) {
  data::TransitionDigest result{};
  result.fill(value);
  return result;
}

data::TransitionEvidence evidence(atx::u8 id, atx::i64 available_at) {
  return {data::TransitionEvidenceNamespace::SyntheticFixture, digest(id), available_at};
}

// Design §7 scenario A panel. 0 = PRED (prints t0..t2, absent t3..t5),
// 1 = SUCC (absent t0..t2), 2 = OTHER. SUCC deliberately moves 60 -> 61 over
// (t4, t5]: the settlement observation must carry real market P&L so that the
// NAV assertions around the payment cannot pass merely because prices are flat.
Panel scenario_a_panel() {
  return prices(6, 3, {32.0, kNaN, 100.0, 32.0, kNaN, 100.0, 32.0, kNaN, 100.0,
                       kNaN, 60.0, 100.0, kNaN, 60.0, 100.0, kNaN, 61.0, 100.0});
}

book::ReplayConfig scenario_a_config() {
  book::ReplayConfig config;
  config.initial_nav = 1000.0;
  config.execution_delay_periods = 1;
  return config;
}

// Design §7 scenario A: one stock-plus-cash transition at t3 with stock ratio
// 1/2 and 40491/10000 cash per predecessor share. Clocks, versions and the
// entitlement are read off the observation the replay itself reports, which is
// how a real adapter would bind them and what makes the entitlement reconcile
// bit-for-bit against the held book.
book::ReplayTransitionRequest scenario_a_transition(const book::ReplayEventContext &ctx) {
  book::ReplayTransitionRequest request;
  auto &event = request.event;
  event.event_id = 1;
  event.revision = 1;
  event.sequence = ctx.last_applied_sequence + 1;
  event.expected_state_version = ctx.state_version;
  event.cash_claim_id = 101;
  event.predecessor = {1001, 1, 0, digest(1)};
  event.successor = {1002, 1, 1, digest(2)};
  event.stock_ratio = {1, 2};
  event.cash_per_predecessor_share = {40491, 10000};
  event.currency = data::TransitionCurrency::USD;
  event.cash_denomination = data::TransitionShareDenomination::PredecessorBeforeTransition;
  event.boundary = data::TransitionBoundary::BetweenValuations;
  event.entitlement_rule = data::TransitionEntitlementRule::SameAsConvertedPredecessor;
  event.effective_at_ns = ctx.previous_session_key + 1;
  event.entitlement_at_ns = ctx.previous_session_key + 2;
  event.terms_sha256 = digest(3);
  event.evidence = evidence(4, ctx.previous_session_key + 1);

  auto &basis = request.basis;
  basis.event_id = event.event_id;
  basis.revision = event.revision;
  basis.source_artifact_sha256 = digest(5);
  basis.axes_sha256 = digest(6);
  basis.adjustment_recipe_sha256 = digest(7);
  basis.evidence = evidence(8, ctx.session_key);
  // PRED's TRI price is its LAST printed close (t2 = 32), which is exactly the
  // mark CarryLastAccountedValueV1 values it at; SUCC's is its t3 close.
  basis.predecessor = {event.predecessor, 8.0, 32.0, ctx.previous_session_key};
  basis.successor = {event.successor, 15.0, 60.0, ctx.session_key};
  basis.stock_coverage = data::TransitionCoverage::ExcludedFromTri;
  basis.cash_coverage = data::TransitionCoverage::ExcludedFromTri;
  basis.continuity_coverage = data::TransitionCoverage::ExcludedFromTri;
  basis.entitled_predecessor_shares = ctx.tri_units[0] * (32.0 / 8.0);
  return request;
}

data::CashClaimPayment scenario_a_payment(const book::ReplayEventContext &ctx) {
  const auto &claim = ctx.pending_claims[0];
  data::CashClaimPayment payment;
  payment.payment_id = 201;
  payment.sequence = ctx.last_applied_sequence + 1;
  payment.expected_state_version = ctx.state_version;
  payment.cash_claim_id = claim.cash_claim_id;
  payment.event_id = claim.event_id;
  payment.event_revision = claim.event_revision;
  payment.currency = data::TransitionCurrency::USD;
  payment.amount = claim.amount;
  payment.allocated_at_ns = ctx.session_key;
  payment.evidence = evidence(9, ctx.session_key);
  return payment;
}
} // namespace

TEST(ClaimsReplay, EmptyEventBatchIsBitIdenticalToScheduledIntents) {
  const auto panel = prices(5, 2, {100, 200, 101, 190, 103, 191, 102, 185, 108, 186});
  const std::vector<atx::i64> times{0, kDay, 4 * kDay, 5 * kDay, 6 * kDay};
  const std::vector<atx::usize> schedule{0, 2};
  const std::vector<atx::f64> preferences{0.4, -0.4, 0.1, -0.1};
  book::ReplayConfig cfg;
  cfg.initial_nav = 100.0;
  cfg.execution_delay_periods = 1;
  cfg.trade_bps = 5.0;
  cfg.annual_borrow_bps = 365.0;
  const book::ReplayIntentPolicy policy = [](const book::ReplayAllocationState &state) {
    return atx::core::Ok(as_targets(state.preference_weights));
  };
  atx::usize queries = 0;
  const book::ReplayMandatoryEventPolicy empty_batch =
      [&](const book::ReplayEventContext &) {
        ++queries;
        return atx::core::Ok(book::ReplayEventBatch{});
      };

  const auto baseline =
      book::replay_scheduled_intents(panel, times, schedule, preferences, policy, cfg);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().message();
  const auto claimed = book::replay_scheduled_intents_with_events(
      panel, times, schedule, preferences, policy, empty_batch, cfg, claims_config(6 * kDay));
  ASSERT_TRUE(claimed.has_value()) << claimed.error().message();
  EXPECT_EQ(queries, 3U); // Periods 1..dates-2; never period 0 or the terminal one.

  const auto &expected = baseline->replay;
  const auto &actual = claimed->policy.replay;
  ASSERT_EQ(actual.intervals.size(), expected.intervals.size());
  for (atx::usize i = 0; i < actual.intervals.size(); ++i) {
    const auto &row = actual.intervals[i];
    EXPECT_EQ(numeric(row), numeric(expected.intervals[i])) << "interval " << i;
    EXPECT_EQ(row.start_period, expected.intervals[i].start_period);
    EXPECT_EQ(row.end_period, expected.intervals[i].end_period);
    EXPECT_EQ(row.decision_period.has_value(), expected.intervals[i].decision_period.has_value());
    EXPECT_EQ(row.decision_period.value_or(0), expected.intervals[i].decision_period.value_or(0));
    // The claims columns exist but carry nothing on an event-free replay, and
    // `settled_cash` is the same number `cash` already reported.
    EXPECT_EQ(bits(row.settled_cash), bits(row.cash));
    EXPECT_EQ(bits(row.signed_pending_claims), atx::u64{0});
    EXPECT_EQ(bits(row.gross_receivable), atx::u64{0});
    EXPECT_EQ(bits(row.gross_payable), atx::u64{0});
    EXPECT_EQ(bits(row.mandatory_value_bridge), atx::u64{0});
    EXPECT_EQ(bits(row.mandatory_settled_cash), atx::u64{0});
    EXPECT_EQ(bits(row.claim_recognized), atx::u64{0});
    EXPECT_EQ(bits(row.claim_settled), atx::u64{0});
  }
  ASSERT_EQ(actual.final_tri_units.size(), expected.final_tri_units.size());
  for (atx::usize i = 0; i < actual.final_tri_units.size(); ++i) {
    EXPECT_EQ(bits(actual.final_tri_units[i]), bits(expected.final_tri_units[i]));
  }
  ASSERT_EQ(actual.trades.size(), expected.trades.size());
  for (atx::usize i = 0; i < actual.trades.size(); ++i) {
    EXPECT_EQ(actual.trades[i].period, expected.trades[i].period);
    EXPECT_EQ(actual.trades[i].instrument, expected.trades[i].instrument);
    EXPECT_EQ(bits(actual.trades[i].dollar_delta), bits(expected.trades[i].dollar_delta));
  }
  EXPECT_EQ(bits(actual.final_cash), bits(expected.final_cash));
  EXPECT_EQ(bits(actual.final_assets), bits(expected.final_assets));
  EXPECT_EQ(bits(actual.final_nav), bits(expected.final_nav));
  EXPECT_EQ(bits(actual.initial_nav), bits(expected.initial_nav));
  EXPECT_EQ(actual.effective_rebalances, expected.effective_rebalances);
  EXPECT_EQ(actual.unexecuted_decisions, expected.unexecuted_decisions);
  ASSERT_EQ(claimed->policy.allocations.size(), baseline->allocations.size());
  const auto instruments = panel.instruments();
  for (atx::usize i = 0; i < claimed->policy.allocations.size(); ++i) {
    const auto &row = claimed->policy.allocations[i];
    const auto &want = baseline->allocations[i];
    EXPECT_EQ(numeric(row), numeric(want)) << "allocation " << i;
    EXPECT_EQ(row.schedule_index, want.schedule_index);
    EXPECT_EQ(row.decision_period, want.decision_period);
    EXPECT_EQ(row.execution_period, want.execution_period);
    EXPECT_EQ(row.decision_session_key, want.decision_session_key);
    EXPECT_EQ(row.execution_session_key, want.execution_session_key);
    ASSERT_EQ(row.target_weights.size(), instruments);
    ASSERT_EQ(want.target_weights.size(), instruments);
    ASSERT_EQ(row.pretrade_marked_dollars.size(), instruments);
    ASSERT_EQ(want.pretrade_marked_dollars.size(), instruments);
    ASSERT_EQ(row.posttrade_marked_dollars.size(), instruments);
    ASSERT_EQ(want.posttrade_marked_dollars.size(), instruments);
    ASSERT_EQ(row.target_intents.size(), instruments);
    ASSERT_EQ(want.target_intents.size(), instruments);
    for (atx::usize j = 0; j < instruments; ++j) {
      EXPECT_EQ(bits(row.target_weights[j]), bits(want.target_weights[j]))
          << "allocation " << i << " weight " << j;
      EXPECT_EQ(bits(row.pretrade_marked_dollars[j]), bits(want.pretrade_marked_dollars[j]));
      EXPECT_EQ(bits(row.posttrade_marked_dollars[j]), bits(want.posttrade_marked_dollars[j]));
      EXPECT_EQ(row.target_intents[j].action, want.target_intents[j].action);
      EXPECT_EQ(bits(row.target_intents[j].weight), bits(want.target_intents[j].weight));
    }
  }
  EXPECT_TRUE(claimed->movements.empty());
  EXPECT_EQ(claimed->final_state.state_version, 1U);
  EXPECT_EQ(claimed->final_state.event_count, 0U);
  EXPECT_EQ(claimed->final_state.claim_count, 0U);
  EXPECT_EQ(claimed->final_state.movement_count, 0U);
  EXPECT_EQ(bits(claimed->final_state.settled_cash), bits(expected.final_cash));
}

TEST(ClaimsReplay, LongReceivableTransitionAndPaymentPreserveNavAndLedgers) {
  const auto panel = scenario_a_panel();
  const std::vector<atx::usize> schedule{1};
  const std::vector<atx::f64> preferences{0.0625, 0.0, 0.2};
  const auto cfg = scenario_a_config();

  constexpr atx::f64 kPredUnits = 1.953125;   // 0.0625 * 1000 / 32
  constexpr atx::f64 kPredValue = 62.5;
  constexpr atx::f64 kOtherValue = 200.0;
  constexpr atx::f64 kCash = 1000.0 - kPredValue - kOtherValue;
  constexpr atx::f64 kSuccUnits = 0.9765625;  // 7.8125 entitled * 1/2 * (15/60)
  constexpr atx::f64 kSuccValue = 58.59375;
  constexpr atx::f64 kClaim = 7.8125 * 4.0491;
  constexpr atx::f64 kBridge = kSuccValue + kClaim - kPredValue;
  constexpr atx::f64 kFinalPnl = kSuccUnits * (61.0 - 60.0); // SUCC's (t4, t5] move.

  atx::usize intent_calls = 0;
  const book::ReplayIntentPolicy policy = [&](const book::ReplayAllocationState &state) {
    ++intent_calls;
    EXPECT_EQ(state.execution_period, 2U);
    EXPECT_TRUE(state.pending_claims.empty());
    EXPECT_EQ(state.retired_representation.size(), 3U);
    return atx::core::Ok(as_targets(state.preference_weights));
  };
  std::vector<atx::usize> queried;
  const book::ReplayMandatoryEventPolicy events = [&](const book::ReplayEventContext &ctx) {
    queried.push_back(ctx.period);
    book::ReplayEventBatch batch{};
    if (ctx.period == 3) {
      batch.transition_count = 1;
      batch.transitions[0] = scenario_a_transition(ctx);
    } else if (ctx.period == 4 && !ctx.pending_claims.empty()) {
      batch.payment_count = 1;
      batch.payments[0] = scenario_a_payment(ctx);
    }
    return atx::core::Ok(batch);
  };

  const auto result = book::replay_scheduled_intents_with_events(
      panel, days(6), schedule, preferences, policy, events, cfg, claims_config(5 * kDay));
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(intent_calls, 1U);
  EXPECT_EQ(queried, (std::vector<atx::usize>{1, 2, 3, 4}));

  const auto &replay = result->policy.replay;
  ASSERT_EQ(replay.intervals.size(), 5U);
  ASSERT_EQ(replay.trades.size(), 2U);
  for (const auto &trade : replay.trades) EXPECT_EQ(trade.period, 2U);
  // PRED stops printing at t3, so the t3 valuation carries its completed t2
  // accounted value and contributes zero market P&L over (t2, t3].
  EXPECT_NEAR(replay.intervals[2].nav, 1000.0, kTolerance);
  EXPECT_NEAR(replay.intervals[2].cash, kCash, kTolerance);
  EXPECT_NEAR(replay.intervals[2].assets, kPredValue + kOtherValue, kTolerance);
  EXPECT_EQ(bits(replay.intervals[2].gross_pnl), atx::u64{0});

  const auto &event_row = replay.intervals[3];
  EXPECT_NEAR(event_row.mandatory_value_bridge, kBridge, kTolerance);
  EXPECT_NEAR(event_row.pretrade_nav,
              replay.intervals[2].nav + event_row.mandatory_value_bridge, kTolerance);
  EXPECT_EQ(bits(event_row.traded_dollars), atx::u64{0}); // Never a trade, never a fee.
  EXPECT_EQ(bits(event_row.trade_cost), atx::u64{0});
  EXPECT_EQ(bits(event_row.mandatory_settled_cash), atx::u64{0});
  EXPECT_NEAR(event_row.claim_recognized, kClaim, kTolerance);
  EXPECT_NEAR(event_row.signed_pending_claims, kClaim, kTolerance);
  EXPECT_NEAR(event_row.gross_receivable, kClaim, kTolerance);
  EXPECT_EQ(bits(event_row.gross_payable), atx::u64{0});
  EXPECT_NEAR(event_row.cash, kCash, kTolerance);
  EXPECT_NEAR(event_row.assets, kSuccValue + kOtherValue, kTolerance);

  // Settlement at t4 moves exactly the claim out of the claim column and into
  // settled cash, leaving turnover and the valuation bridge untouched. SUCC
  // moves over (t4, t5], so nothing below can pass on flat prices: the NAV
  // neutrality of the payment is pinned across the t4 boundary, and the row's
  // own published identity — not a comparison with the previous row — carries
  // the interval.
  const auto &settle_row = replay.intervals[4];
  EXPECT_NEAR(settle_row.mandatory_settled_cash, kClaim, kTolerance);
  EXPECT_NEAR(settle_row.claim_settled, kClaim, kTolerance);
  EXPECT_EQ(bits(settle_row.signed_pending_claims), atx::u64{0});
  EXPECT_EQ(bits(settle_row.gross_receivable), atx::u64{0});
  EXPECT_NEAR(settle_row.cash, kCash + kClaim, kTolerance);
  EXPECT_NEAR(settle_row.cash - event_row.cash, kClaim, kTolerance);
  EXPECT_NEAR(settle_row.pretrade_nav, event_row.nav, kTolerance);
  EXPECT_NEAR(settle_row.gross_pnl, kFinalPnl, kTolerance);
  EXPECT_NEAR(settle_row.nav,
              settle_row.pretrade_nav + settle_row.gross_pnl - settle_row.trade_cost -
                  settle_row.borrow_cost,
              kTolerance);
  EXPECT_EQ(bits(settle_row.traded_dollars), atx::u64{0});
  EXPECT_EQ(bits(settle_row.mandatory_value_bridge), atx::u64{0});

  atx::f64 accumulated = 0.0;
  for (const auto &row : replay.intervals) {
    EXPECT_NEAR(row.nav, row.cash + row.assets + row.signed_pending_claims, kTolerance);
    EXPECT_EQ(bits(row.settled_cash), bits(row.cash));
    EXPECT_EQ(bits(row.borrow_cost), atx::u64{0});
    accumulated += row.gross_pnl + row.mandatory_value_bridge - row.trade_cost - row.borrow_cost;
  }
  EXPECT_NEAR(accumulated, replay.final_nav - replay.initial_nav, kTolerance);
  EXPECT_NEAR(replay.final_nav, 1000.0 + kBridge + kFinalPnl, kTolerance);
  EXPECT_NEAR(replay.final_cash, kCash + kClaim, kTolerance);
  EXPECT_EQ(bits(replay.final_tri_units[0]), atx::u64{0});
  EXPECT_NEAR(replay.final_tri_units[1], kSuccUnits, 1.0e-15);

  ASSERT_EQ(result->movements.size(), 4U);
  const auto &rows = result->movements;
  EXPECT_EQ(rows[0].kind, book::MandatoryMovementKind::PredecessorRetirement);
  EXPECT_EQ(rows[0].instrument, 0U);
  EXPECT_EQ(rows[0].period, 3U);
  EXPECT_NEAR(rows[0].units_before, kPredUnits, 1.0e-15);
  EXPECT_EQ(bits(rows[0].units_after), atx::u64{0});
  EXPECT_NEAR(rows[0].value_before, kPredValue, kTolerance);
  EXPECT_EQ(bits(rows[0].carried_mark), bits(32.0)); // Disclosed, with its period.
  EXPECT_EQ(rows[0].carried_mark_period, 2U);
  EXPECT_EQ(rows[1].kind, book::MandatoryMovementKind::SuccessorDelivery);
  EXPECT_EQ(rows[1].instrument, 1U);
  EXPECT_NEAR(rows[1].units_after, kSuccUnits, 1.0e-15);
  EXPECT_NEAR(rows[1].value_after, kSuccValue, kTolerance);
  EXPECT_NEAR(rows[1].valuation_bridge, kBridge, kTolerance);
  EXPECT_EQ(rows[2].kind, book::MandatoryMovementKind::ClaimRecognition);
  EXPECT_NEAR(rows[2].claim_delta, kClaim, kTolerance);
  EXPECT_EQ(rows[3].kind, book::MandatoryMovementKind::ClaimSettlement);
  EXPECT_EQ(rows[3].period, 4U);
  EXPECT_NEAR(rows[3].settled_cash_delta, kClaim, kTolerance);
  EXPECT_NEAR(rows[3].claim_delta, -kClaim, kTolerance);

  const auto &state = result->final_state;
  EXPECT_EQ(state.state_version, 3U);
  EXPECT_EQ(state.last_applied_sequence, 2U);
  EXPECT_EQ(state.event_count, 1U);
  EXPECT_EQ(state.claim_count, 1U);
  EXPECT_EQ(state.payment_count, 1U);
  EXPECT_EQ(state.movement_count, 4U);
  ASSERT_EQ(state.retired_count, 1U);
  EXPECT_EQ(state.retired[0].instrument, 0U);
  EXPECT_EQ(state.retired[0].retired_at_period, 3U);
  EXPECT_EQ(state.claims[0].status, book::ClaimSlotStatus::Settled);
  EXPECT_NEAR(state.settled_cash, kCash + kClaim, kTolerance);
}

// Design §4 step 5's retired-target gate. Scenario A's panel, with a second
// decision at t3 executing at t4 — one observation AFTER the t3 transition
// retires PRED. PRED then holds zero units and has no close at t4, so
// resolve_replay_target would accept a HoldCurrent on it (`held == false`) and
// a nonzero TargetWeight would silently reopen a retired representation: this
// gate is the only thing that refuses them. Both sub-cases must reject, and
// because the whole replay is all-or-error neither may publish a row, a
// movement or a ledger, nor disturb the frozen inputs.
TEST(ClaimsReplay, RetiredPredecessorTargetsAreRejectedAtomically) {
  const auto panel = scenario_a_panel();
  const std::vector<atx::usize> schedule{1, 3}; // Executes at t2 and at t4.
  const std::vector<atx::f64> preferences{0.0625, 0.0, 0.2, 0.0, 0.0, 0.2};
  const auto cfg = scenario_a_config();
  const auto close = panel.field_all(panel.field_id("close").value());
  const std::vector<atx::f64> close_before(close.begin(), close.end());
  const auto preferences_before = preferences;

  const book::ReplayMandatoryEventPolicy events = [](const book::ReplayEventContext &ctx) {
    book::ReplayEventBatch batch{};
    if (ctx.period == 3) {
      batch.transition_count = 1;
      batch.transitions[0] = scenario_a_transition(ctx);
    } else if (ctx.period == 4 && !ctx.pending_claims.empty()) {
      batch.payment_count = 1;
      batch.payments[0] = scenario_a_payment(ctx);
    }
    return atx::core::Ok(batch);
  };

  const std::array<book::ReplayTargetIntent, 2> reopening{
      book::ReplayTargetIntent{book::ReplayTargetAction::HoldCurrent, 0.0},
      book::ReplayTargetIntent{book::ReplayTargetAction::TargetWeight, 0.05}};
  for (const auto &reopen : reopening) {
    atx::usize intent_calls = 0;
    const book::ReplayIntentPolicy policy = [&](const book::ReplayAllocationState &state) {
      ++intent_calls;
      auto intents = as_targets(state.preference_weights);
      if (state.execution_period == 4) {
        EXPECT_EQ(intents.size(), 3U);
        EXPECT_EQ(state.retired_representation.size(), 3U);
        if (intents.size() == 3U && state.retired_representation.size() == 3U) {
          EXPECT_EQ(state.retired_representation[0], atx::u8{1}); // PRED is retired.
          intents[0] = reopen;
        }
      }
      return atx::core::Ok(std::move(intents));
    };

    const auto result = book::replay_scheduled_intents_with_events(
        panel, days(6), schedule, preferences, policy, events, cfg, claims_config(5 * kDay));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
    EXPECT_NE(result.error().message().find("retired representation"), std::string::npos)
        << result.error().message();
    EXPECT_NE(result.error().message().find("at period=4 instrument=0"), std::string::npos)
        << result.error().message();
    // t2 allocated normally; t4 is the rejected one, so the gate runs after the
    // policy call and before any trade of that observation.
    EXPECT_EQ(intent_calls, 2U);
  }

  const auto close_after = panel.field_all(panel.field_id("close").value());
  ASSERT_EQ(close_after.size(), close_before.size());
  for (atx::usize i = 0; i < close_before.size(); ++i) {
    EXPECT_EQ(bits(close_after[i]), bits(close_before[i])) << "close cell " << i;
  }
  EXPECT_EQ(preferences, preferences_before);
}

TEST(ClaimsReplay, CostedResidualsAndScheduledFinancingReconcileShortTransitionAndPayment) {
  // All three orders partially fill at t2. At t3 the short predecessor becomes
  // successor stock plus a payable, netting against an existing successor long.
  // The two changed goals must cancel, while OTHER continues filling with an
  // outstanding claim at t3 and before that payable is settled at t4.
  const auto panel = prices(6, 3, {32, 60, 100, 32, 60, 100, 32, 60, 100,
                                   kNaN, 60, 100, kNaN, 60, 100, kNaN, 61, 100});
  const std::vector<atx::usize> schedule{1};
  const std::vector<atx::f64> preferences{-0.2, 0.1, 0.3};
  auto cfg = scenario_a_config();
  cfg.borrow_day_basis = book::ReplayDayBasis::D360;
  const auto model = book::SqrtImpactCost::create({0.25, 0.5}, 0.1, 1.0).value();
  std::vector<book::LiquidityRow> liquidity;
  book::BorrowSchedule borrow;
  borrow.dates = 6;
  borrow.instruments = 3;
  borrow.cash_bps = 72.0;
  borrow.quote_kind = book::BorrowQuoteKind::FeeV2;
  for (atx::usize d = 0; d < 6; ++d) {
    liquidity.insert(liquidity.end(), {{640, 0.02, 2}, {150, 0.02, 2}, {500, 0.02, 2}});
    borrow.fee_bps.insert(borrow.fee_bps.end(), {360, 7200, 0});
    // No successor locate: mandatory stock delivery is carried, not forcibly
    // covered. The predecessor locate cannot become a successor locate.
    borrow.locate_dollars.insert(borrow.locate_dollars.end(), {1000, 0, 1000});
  }
  cfg.cost_model = &model;
  cfg.liquidity = liquidity;
  cfg.borrow_schedule = &borrow;
  const book::ReplayIntentPolicy policy = [](const book::ReplayAllocationState &state) {
    return atx::core::Ok(as_targets(state.preference_weights));
  };
  const book::ReplayMandatoryEventPolicy events = [](const book::ReplayEventContext &ctx) {
    book::ReplayEventBatch batch{};
    if (ctx.period == 3) {
      batch.transition_count = 1;
      batch.transitions[0] = scenario_a_transition(ctx);
    } else if (ctx.period == 4 && !ctx.pending_claims.empty()) {
      batch.payment_count = 1;
      batch.payments[0] = scenario_a_payment(ctx);
    }
    return atx::core::Ok(batch);
  };
  const auto result = book::replay_scheduled_intents_with_events(
      panel, days(6), schedule, preferences, policy, events, cfg, claims_config(5 * kDay));
  ASSERT_TRUE(result.has_value()) << result.error().message();
  const auto &replay = result->policy.replay;
  ASSERT_EQ(replay.intervals.size(), 5U);
  ASSERT_EQ(replay.trades.size(), 5U);
  ASSERT_EQ(result->order_cancellations.size(), 2U);
  EXPECT_EQ(result->order_cancellations[0].instrument, 0U);
  EXPECT_EQ(result->order_cancellations[1].instrument, 1U);
  for (const auto &cancellation : result->order_cancellations) {
    EXPECT_EQ(cancellation.period, 3U);
    EXPECT_EQ(cancellation.event_id, 1U);
  }
  for (const auto &trade : replay.trades) {
    if (trade.period >= 3) EXPECT_EQ(trade.instrument, 2U);
  }
  EXPECT_EQ(replay.open_working_orders, 1U);
  EXPECT_DOUBLE_EQ(replay.final_tri_units[0], 0.0);
  EXPECT_DOUBLE_EQ(replay.final_tri_units[1], -0.75); // +0.25 long -1.0 delivery.
  EXPECT_DOUBLE_EQ(replay.final_tri_units[2], 1.5);

  constexpr auto claim = -8.0 * 4.0491;
  constexpr auto bridge = -45.0 + claim + 64.0;
  EXPECT_NEAR(replay.intervals[3].claim_recognized, claim, kTolerance);
  EXPECT_NEAR(replay.intervals[3].signed_pending_claims, claim, kTolerance);
  EXPECT_NEAR(replay.intervals[3].gross_payable, -claim, kTolerance);
  EXPECT_NEAR(replay.intervals[3].mandatory_value_bridge, bridge, kTolerance);
  EXPECT_NEAR(replay.intervals[4].mandatory_settled_cash, claim, kTolerance);
  EXPECT_DOUBLE_EQ(replay.intervals[4].signed_pending_claims, 0.0);

  // Independent cash roll-forward: real capped model fees, financing at the
  // predecessor rate before the event and the net successor rate afterwards.
  // Claims enter cash only on payment, and never enter the equity borrow base.
  const auto fill_cost = [](atx::f64 dollars) {
    return dollars * (3.0e-4 + 0.25 * 0.02 * std::sqrt(0.1));
  };
  atx::f64 expected_cash = 1000.0;
  atx::f64 accumulated = 0.0;
  for (atx::usize d = 0; d < replay.intervals.size(); ++d) {
    const auto &row = replay.intervals[d];
    atx::f64 cost = 0.0;
    atx::f64 fee_dollars_bps = 0.0;
    if (d == 2) {
      cost = fill_cost(129.0);
      expected_cash += 64.0 - 15.0 - 50.0 - cost;
      fee_dollars_bps = 64.0 * 360.0;
    } else if (d >= 3) {
      cost = fill_cost(50.0);
      expected_cash -= 50.0 + cost;
      if (d == 4) expected_cash += claim;
      fee_dollars_bps = 45.0 * 7200.0;
    }
    const auto charge = (fee_dollars_bps - expected_cash * 72.0) * (1e-4 / 360.0);
    expected_cash -= charge;
    EXPECT_NEAR(row.trade_cost, cost, kTolerance) << d;
    EXPECT_NEAR(row.borrow_cost, charge, kTolerance) << d;
    EXPECT_NEAR(row.cash, expected_cash, kTolerance) << d;
    EXPECT_NEAR(row.nav, row.cash + row.assets + row.signed_pending_claims, kTolerance);
    EXPECT_NEAR(row.nav, row.pretrade_nav + row.gross_pnl - cost - charge, kTolerance);
    accumulated += row.mandatory_value_bridge + row.gross_pnl - cost - charge;
  }
  EXPECT_NEAR(replay.intervals[4].gross_pnl, -0.75, kTolerance);
  EXPECT_NEAR(replay.final_nav, expected_cash - 0.75 * 61.0 + 150.0, kTolerance);
  EXPECT_NEAR(replay.final_nav - 1000.0, accumulated, kTolerance);
  EXPECT_NEAR(result->final_state.settled_cash, expected_cash, kTolerance);
  EXPECT_EQ(result->final_state.claims[0].status, book::ClaimSlotStatus::Settled);
  EXPECT_EQ(result->movements.size(), 4U);

  auto unsupported = claims_config(5 * kDay);
  unsupported.admission.mode = book::TransitionMode::StrictAsOf;
  EXPECT_FALSE(book::replay_scheduled_intents_with_events(
      panel, days(6), schedule, preferences, policy, events, cfg, unsupported));
}

TEST(ClaimsReplay, SuccessorLocateLimitsDiscretionaryGrowthAfterMandatoryShortDelivery) {
  const auto panel = scenario_a_panel();
  const std::vector<atx::usize> schedule{1, 2, 3};
  const std::vector<atx::f64> preferences{-0.0625, 0, 0, 0, -0.5, 0, 0, 0, 0};
  auto cfg = scenario_a_config();
  const auto model = book::FlatBpsCost::create(5).value();
  book::BorrowSchedule borrow;
  borrow.dates = 6;
  borrow.instruments = 3;
  borrow.quote_kind = book::BorrowQuoteKind::NetRebateV2; // Explicit zero rebate.
  borrow.rebate_bps = 0.0;
  for (atx::usize d = 0; d < 6; ++d) {
    borrow.locate_dollars.insert(borrow.locate_dollars.end(), {1000, 0, 0});
  }
  cfg.cost_model = &model;
  cfg.borrow_schedule = &borrow;
  const book::ReplayIntentPolicy policy = [](const book::ReplayAllocationState &state) {
    return atx::core::Ok(as_targets(state.preference_weights));
  };
  const book::ReplayMandatoryEventPolicy events = [](const book::ReplayEventContext &ctx) {
    book::ReplayEventBatch batch{};
    if (ctx.period == 3) {
      batch.transition_count = 1;
      batch.transitions[0] = scenario_a_transition(ctx);
      batch.transitions[0].event.stock_ratio = {2, 1}; // Successor short grows on delivery.
    } else if (ctx.period == 4 && !ctx.pending_claims.empty()) {
      batch.payment_count = 1;
      batch.payments[0] = scenario_a_payment(ctx);
    }
    return atx::core::Ok(batch);
  };
  const auto clipped = book::replay_scheduled_intents_with_events(
      panel, days(6), schedule, preferences, policy, events, cfg, claims_config(5 * kDay));
  ASSERT_TRUE(clipped.has_value()) << clipped.error().message();
  const auto &replay = clipped->policy.replay;
  ASSERT_EQ(replay.locate_clips.size(), 1U);
  EXPECT_EQ(replay.locate_clips[0].period, 3U);
  EXPECT_EQ(replay.locate_clips[0].instrument, 1U);
  EXPECT_DOUBLE_EQ(replay.locate_clips[0].locate, 0.0);
  EXPECT_DOUBLE_EQ(replay.intervals[3].traded_dollars, 0.0);
  EXPECT_DOUBLE_EQ(replay.intervals[3].trade_cost, 0.0);
  ASSERT_EQ(clipped->policy.allocations.size(), 3U);
  EXPECT_DOUBLE_EQ(clipped->policy.allocations[1].posttrade_marked_dollars[1], -234.375);
  EXPECT_NEAR(replay.intervals[4].traded_dollars, 234.375, kTolerance);
  EXPECT_NEAR(replay.intervals[4].trade_cost, 234.375 * 5e-4, kTolerance);
  EXPECT_DOUBLE_EQ(replay.final_assets, 0.0); // Cover succeeds despite zero locate.
  EXPECT_DOUBLE_EQ(replay.final_tri_units[0], 0.0);
  EXPECT_DOUBLE_EQ(replay.final_tri_units[1], 0.0);
  cfg.locate_breach = book::LocateBreach::AbortV1;
  const auto rejected = book::replay_scheduled_intents_with_events(
      panel, days(6), schedule, preferences, policy, events, cfg, claims_config(5 * kDay));
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().message().find("short exceeds locate at period=3 instrument=1"),
            std::string::npos) << rejected.error().message();
}
