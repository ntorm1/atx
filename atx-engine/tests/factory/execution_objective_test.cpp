#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/cost/cost_surface.hpp"
#include "atx/engine/factory/execution_objective.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace {
using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;
using namespace atx::engine::factory;
namespace cost = atx::engine::cost;
constexpr usize dates = 8, names = 2;
constexpr atx::i64 day_ns = 86'400'000'000'000;

// Tiny synthetic input builder. All expected ledger values below are explicit
// dollar calculations, independent of the production chronological loop.
struct Inputs {
  ExecutionObjectiveConfig cfg;
  atx::engine::WeightPolicy policy;
  cost::CostSurfaceRecipe recipe;
  std::array<cost::CostSurfaceRow, names> rows{};
  std::array<atx::u64, names> ids{101, 202};
  std::vector<f64> close = std::vector<f64>(dates * names, 100.0);
  std::vector<f64> signal = std::vector<f64>(dates * names);
  std::vector<atx::u8> present = std::vector<atx::u8>(dates * names, 1);
  std::vector<atx::u8> member = std::vector<atx::u8>(dates * names, 1);
  std::vector<atx::u32> guard = std::vector<atx::u32>(dates * names, 0);
  std::vector<atx::i64> marks = std::vector<atx::i64>(dates);
  std::vector<atx::i64> decisions = std::vector<atx::i64>(dates);

  Inputs() {
    cfg.rule = ExecutionObjectiveRule::DelayedSurfaceV2;
    cfg.initial_nav = 1000;
    cfg.window_end = dates;
    cfg.maturity_end = dates;
    cfg.borrow = ExecutionBorrowRule::DisabledExplicitV1;
    policy.winsorize_limit = 0;
    recipe.rule = cost::CostSurfaceRule::ModeledInputsV2;
    recipe.impact_y = 0;
    recipe.commission_bps = 0;
    for (usize d = 0; d < dates; ++d) {
      marks[d] = (100 + static_cast<atx::i64>(d)) * day_ns;
      decisions[d] = marks[d] + 1;
      signal[d * names] = -1;
      signal[d * names + 1] = 1;
    }
    for (usize i = 0; i < names; ++i) {
      rows[i].instrument_id = ids[i];
      rows[i].state = cost::CostInputState::Available;
      rows[i].available_at_ns = 1;
      rows[i].adv_dollars = 1000;
      rows[i].borrow_state = cost::CostInputState::Available;
      rows[i].borrow_available_at_ns = 1;
    }
  }

  atx::core::Result<Panel> panel() const {
    return Panel::create(dates, names, {"close"}, {close}, present);
  }
  atx::core::Result<std::vector<cost::CostSurface>> surfaces() const {
    const auto end = std::min(cfg.window_end, cfg.maturity_end - cfg.delay - 1);
    std::vector<cost::CostSurface> out;
    for (usize d = cfg.window_begin; d < end; ++d) {
      cost::CostSurfaceIdentity id{decisions[d], std::string(64, 'a'),
                                  "synthetic-prior-liquidity-v1", "explicit-test-model"};
      ATX_TRY(auto surface, cost::CostSurface::create(recipe, id, rows));
      out.push_back(std::move(surface));
    }
    return atx::core::Ok(std::move(out));
  }
  atx::core::Result<ExecutionObjectiveContext> context(const Panel& p) const {
    ATX_TRY(auto snapshots, surfaces());
    return prepare_execution_objective(p, policy, cfg, snapshots, marks, decisions, ids,
        {std::string(64, 'b'), "synthetic-training-role", "synthetic-total-return-close-v1"},
        member, guard);
  }
};

TEST(ExecutionObjective, DelayedCashLedgerUsesMarkedDollarCostsAndCalendarSlots) {
  Inputs f;
  f.recipe.commission_bps = 10;
  auto p = f.panel(); ASSERT_TRUE(p);
  auto c = f.context(*p); ASSERT_TRUE(c);
  const auto out = extract_execution_signal(f.signal, *c); ASSERT_TRUE(out);
  ASSERT_EQ(out->n_periods(), dates);
  EXPECT_EQ(out->first_realization_, 2U);
  EXPECT_EQ(out->realization_end_, dates);
  for (usize d = 0; d < 2; ++d) {
    EXPECT_EQ(out->valid_flat[d], 0);
    EXPECT_TRUE(std::isnan(out->pnl_flat[d]));
  }
  // d0 fixes -$500/+$500; entry1 debits $1. Short proceeds stay in
  // cash. d1 therefore fixes -$499.50/+$499.50, and entry2 trades $1 total.
  EXPECT_DOUBLE_EQ(out->pretrade_nav_flat[2], 1000);
  EXPECT_DOUBLE_EQ(out->end_nav_flat[2], 999);
  EXPECT_DOUBLE_EQ(out->positions(0, 2)[0], -0.5);
  EXPECT_DOUBLE_EQ(out->positions(0, 2)[1], 0.5);
  EXPECT_DOUBLE_EQ(out->turnover_flat[2], 1);
  EXPECT_DOUBLE_EQ(out->execution_cost_flat[2], 0.001);
  EXPECT_NEAR(out->pnl_flat[2], -0.001, 1e-15);
  EXPECT_DOUBLE_EQ(out->pretrade_nav_flat[3], 999);
  EXPECT_NEAR(out->end_nav_flat[3], 998.999, 1e-12);
  EXPECT_NEAR(out->turnover_flat[3], 1.0 / 999.0, 1e-15);
  EXPECT_NEAR(out->execution_cost_flat[3], 0.001 / 999.0, 1e-15);
  EXPECT_NEAR(out->pnl_flat[3], -0.001 / 999.0, 1e-15);
  auto snapshots = f.surfaces(); ASSERT_TRUE(snapshots);
  const std::array<cost::CostPathTrade, 4> trades{{
      {0, 0, f.decisions[0], 1000, -500}, {0, 1, f.decisions[0], 1000, 500},
      {1, 0, f.decisions[1], 999, 0.5}, {1, 1, f.decisions[1], 999, -0.5}}};
  const auto priced = cost::price_trade_path(*snapshots, trades); ASSERT_TRUE(priced);
  EXPECT_NEAR(priced->cost_return,
              out->execution_cost_flat[2] + out->execution_cost_flat[3], 1e-15);
  EXPECT_NEAR(priced->one_way_turnover,
              out->turnover_flat[2] + out->turnover_flat[3], 1e-15);
  for (usize d = 2; d < dates; ++d) EXPECT_EQ(out->valid_flat[d], 1);
}

TEST(ExecutionObjective, DelayTwoKeepsDecisionDollarsAfterEntryNavChanges) {
  Inputs f;
  f.cfg.delay = 2;
  for (usize d = 3; d < dates; ++d) f.close[d * names + 1] = 120;
  auto p = f.panel(); ASSERT_TRUE(p);
  auto c = f.context(*p); ASSERT_TRUE(c);
  const auto out = extract_execution_signal(f.signal, *c); ASSERT_TRUE(out);
  // d0 enters at2; the long earns $100 at3. d1's already fixed $500 target
  // sells precisely that $100 gain at3, rather than sizing to future NAV1100.
  EXPECT_EQ(out->first_realization_, 3U);
  EXPECT_NEAR(out->gross_flat[3], 0.1, 1e-15);
  EXPECT_DOUBLE_EQ(out->end_nav_flat[3], 1100);
  EXPECT_DOUBLE_EQ(out->pretrade_nav_flat[4], 1100);
  EXPECT_NEAR(out->positions(0, 4)[0], -500.0 / 1100.0, 1e-15);
  EXPECT_NEAR(out->positions(0, 4)[1], 500.0 / 1100.0, 1e-15);
  EXPECT_NEAR(out->turnover_flat[4], 100.0 / 1100.0, 1e-15);
  EXPECT_DOUBLE_EQ(out->pnl_flat[4], 0);
}

TEST(ExecutionObjective, ParticipationCapChangesActualHoldingsAndTurnover) {
  Inputs f;
  f.recipe.max_participation = 0.1; // $100/name/decision against $500 targets.
  auto p = f.panel(); ASSERT_TRUE(p);
  auto c = f.context(*p); ASSERT_TRUE(c);
  const auto out = extract_execution_signal(f.signal, *c); ASSERT_TRUE(out);
  EXPECT_NEAR(out->positions(0, 2)[0], -0.1, 1e-15);
  EXPECT_NEAR(out->positions(0, 2)[1], 0.1, 1e-15);
  EXPECT_NEAR(out->positions(0, 3)[0], -0.2, 1e-15);
  EXPECT_NEAR(out->positions(0, 3)[1], 0.2, 1e-15);
  EXPECT_NEAR(out->turnover_flat[2], 0.2, 1e-15);
  EXPECT_NEAR(out->turnover_flat[3], 0.2, 1e-15);
  EXPECT_EQ(out->capped_names_flat[2], 2U);
  EXPECT_EQ(out->capped_names_flat[3], 2U);
}

TEST(ExecutionObjective, CalendarBorrowAndInverseSignalHaveDifferentCashLedgers) {
  Inputs f;
  f.cfg.borrow = ExecutionBorrowRule::RequireModeledV2;
  f.rows[0].borrow_annual_fraction = 0.365;
  for (usize d = 2; d < dates; ++d) {
    f.marks[d] += 2 * day_ns; // entry1 -> endpoint2 spans three calendar days.
    f.decisions[d] = f.marks[d] + 1;
  }
  auto p = f.panel(); ASSERT_TRUE(p);
  auto c = f.context(*p); ASSERT_TRUE(c);
  const auto positive = extract_execution_signal(f.signal, *c, 1); ASSERT_TRUE(positive);
  const auto negative = extract_execution_signal(f.signal, *c, -1); ASSERT_TRUE(negative);
  // $500 short * 36.5% annual * 3/365 = $1.50. The inverse shorts the
  // independently modeled zero-borrow second asset: its PnL is not +$1.50.
  EXPECT_NEAR(positive->borrow_cost_flat[2], 0.0015, 1e-15);
  EXPECT_NEAR(positive->pnl_flat[2], -0.0015, 1e-15);
  EXPECT_DOUBLE_EQ(positive->end_nav_flat[2], 998.5);
  EXPECT_DOUBLE_EQ(negative->borrow_cost_flat[2], 0);
  EXPECT_DOUBLE_EQ(negative->pnl_flat[2], 0);
  EXPECT_DOUBLE_EQ(negative->positions(0, 2)[0], 0.5);
}

TEST(ExecutionObjective, AbsentFiniteEntryOrHeldMarkCannotBecomeRealizedPnl) {
  for (const usize missing_date : {usize{1}, usize{2}}) {
    SCOPED_TRACE(missing_date);
    Inputs f;
    f.present[missing_date * names] = 0;
    f.close[missing_date * names] = 777; // external membership remains one.
    auto p = f.panel(); ASSERT_TRUE(p);
    auto c = f.context(*p); ASSERT_TRUE(c);
    EXPECT_FALSE(extract_execution_signal(f.signal, *c));
  }
}

TEST(ExecutionObjective, RoleMaturityAndFutureMutationPreserveRealizedPrefix) {
  Inputs f;
  f.cfg.maturity_end = 6;
  auto p = f.panel(); ASSERT_TRUE(p);
  auto c = f.context(*p); ASSERT_TRUE(c);
  const auto before = extract_execution_signal(f.signal, *c); ASSERT_TRUE(before);
  Inputs changed = f;
  for (usize d = 4; d < dates; ++d) {
    changed.signal[d * names] = 50;
    changed.signal[d * names + 1] = -50;
  }
  for (usize d = 6; d < dates; ++d) {
    changed.close[d * names] = 777;
    changed.close[d * names + 1] = 0.001;
    changed.present[d * names] = 0;
  }
  auto q = changed.panel(); ASSERT_TRUE(q);
  auto other = changed.context(*q); ASSERT_TRUE(other);
  const auto after = extract_execution_signal(changed.signal, *other); ASSERT_TRUE(after);
  EXPECT_EQ(c->decision_end(), 4U);
  EXPECT_EQ(before->realization_end_, 6U);
  for (usize d = 2; d < 6; ++d) {
    EXPECT_DOUBLE_EQ(before->pnl_flat[d], after->pnl_flat[d]);
    EXPECT_DOUBLE_EQ(before->end_nav_flat[d], after->end_nav_flat[d]);
    for (usize i = 0; i < names; ++i)
      EXPECT_DOUBLE_EQ(before->positions(0, d)[i], after->positions(0, d)[i]);
  }
  for (usize d = 6; d < dates; ++d) {
    EXPECT_EQ(after->valid_flat[d], 0);
    EXPECT_TRUE(std::isnan(after->pnl_flat[d]));
  }
}

TEST(ExecutionObjective, ClocksIdentitySupportAndBudgetAreExplicitAdmission) {
  Inputs f;
  auto p = f.panel(); ASSERT_TRUE(p);
  auto c = f.context(*p); ASSERT_TRUE(c);
  EXPECT_TRUE(execution_objective_matches(*c, *p, f.policy, f.cfg));
  EXPECT_TRUE(execution_support_matches(*c, f.member, f.guard));
  f.member[0] = 0;
  EXPECT_FALSE(execution_support_matches(*c, f.member, f.guard));
  f.member[0] = 1;
  f.marks[0] = f.decisions[0];
  EXPECT_FALSE(f.context(*p)); // equality is not prior publication.
  f.marks[0] -= 1;
  std::swap(f.ids[0], f.ids[1]);
  EXPECT_FALSE(f.context(*p)); // rows are valid, but in another instrument order.
  std::swap(f.ids[0], f.ids[1]);
  f.cfg.max_working_bytes = c->bytes() + c->per_signal_working_bytes() - 1;
  EXPECT_FALSE(f.context(*p)); // retained + one output/scratch must fit together.
  EXPECT_FALSE(extract_execution_signal(f.signal, ExecutionObjectiveContext{}));
}

TEST(ExecutionObjective, MissingCostBorrowAndGuardedHeldReturnNeverBecomeZeroCost) {
  Inputs f;
  auto p = f.panel(); ASSERT_TRUE(p);
  f.rows[0].state = cost::CostInputState::Unavailable;
  auto no_cost = f.context(*p); ASSERT_TRUE(no_cost);
  EXPECT_FALSE(extract_execution_signal(f.signal, *no_cost));
  f.rows[0].state = cost::CostInputState::Available;
  f.cfg.borrow = ExecutionBorrowRule::RequireModeledV2;
  f.rows[0].borrow_state = cost::CostInputState::Unavailable;
  auto no_borrow = f.context(*p); ASSERT_TRUE(no_borrow);
  EXPECT_FALSE(extract_execution_signal(f.signal, *no_borrow));
  f.cfg.borrow = ExecutionBorrowRule::DisabledExplicitV1;
  for (usize d = 2; d < dates; ++d) f.guard[d * names] = 1;
  auto guarded = f.context(*p); ASSERT_TRUE(guarded);
  EXPECT_FALSE(extract_execution_signal(f.signal, *guarded));
}

TEST(ExecutionObjective, UnfundedLongAndAsymmetricallyCappedNeutralBooksAreRefused) {
  Inputs long_only;
  long_only.policy.transform = atx::engine::Transform::Raw;
  long_only.policy.dollar_neutral = false;
  long_only.policy.gross_leverage = 2;
  std::fill(long_only.signal.begin(), long_only.signal.end(), 1.0);
  auto p = long_only.panel(); ASSERT_TRUE(p);
  auto leveraged = long_only.context(*p); ASSERT_TRUE(leveraged);
  const auto borrowed = extract_execution_signal(long_only.signal, *leveraged);
  ASSERT_FALSE(borrowed); // $2,000 long assets would require $1,000 cash funding.
  EXPECT_NE(borrowed.error().to_string().find("cash financing unsupported"), std::string::npos);

  Inputs capped;
  capped.policy.gross_leverage = 3; // neutral targets alone do not guarantee funded fills.
  capped.recipe.max_participation = 0.1;
  capped.rows[0].adv_dollars = 1000;   // short proceeds capped at $100.
  capped.rows[1].adv_dollars = 20000;  // all $1,500 long dollars fill.
  auto q = capped.panel(); ASSERT_TRUE(q);
  auto asymmetric = capped.context(*q); ASSERT_TRUE(asymmetric);
  const auto unfunded = extract_execution_signal(capped.signal, *asymmetric);
  ASSERT_FALSE(unfunded); // cash1000 + short100 - long1500 = -400.
  EXPECT_NE(unfunded.error().to_string().find("cash financing unsupported"), std::string::npos);

  // With both sides executable, same-batch short proceeds fund the purchase.
  // Buy the first asset before selling the second to exercise intra-batch order.
  capped.rows[0].adv_dollars = 20000;
  auto funded = capped.context(*q); ASSERT_TRUE(funded);
  const auto both = extract_execution_signal(capped.signal, *funded, -1);
  ASSERT_TRUE(both);
  EXPECT_DOUBLE_EQ(both->pretrade_nav_flat[2], 1000);
  EXPECT_DOUBLE_EQ(both->end_nav_flat[2], 1000);
  EXPECT_DOUBLE_EQ(both->turnover_flat[2], 3);
}
} // namespace
