// Parity between the legacy one-period weight report (book::detail::accumulate_period,
// the arithmetic under accumulate_report) and the policy replay's cash/holdings
// accounting, on a one-period fixture where both conventions describe the same
// trade: start in cash, buy the target at t0 with no delay, mark at t1.
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/book/report.hpp"

namespace atx_test_l8_e2e_report_parity {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

struct Fixture {
  std::vector<atx::f64> close{40.0, 25.0, 80.0, 10.0, 41.0, 24.5, 78.0, 10.4};
  std::vector<atx::f64> target{0.3, -0.2, 0.25, -0.35};
  Panel panel;
  Fixture() : panel(make()) {}
  Panel make() const {
    std::vector<atx::f64> ret(close.size(), std::nan(""));
    for (atx::usize i = 0; i < 4; ++i) ret[i] = close[4 + i] / close[i] - 1.0;
    return Panel::create(2, 4, {"close", "ret"}, {close, ret}, {}).value();
  }
};


TEST(ReplayReportParity, OnePeriodPolicyReplayEqualsLegacyReport) {
  Fixture f;
  constexpr atx::f64 kTradeBps = 7.5;
  atx::f64 turnover = 0.0;
  for (const auto w : f.target) turnover += std::abs(w);
  const auto ret_field = f.panel.field_id("ret").value();
  const auto legacy = book::detail::accumulate_period(
    f.target, f.panel.field_cross_section(ret_field, 0), f.panel, 0, turnover * kTradeBps);
  const auto legacy_net = legacy.pnl_gross - legacy.pnl_cost - legacy.pnl_borrow;

  book::ReplayConfig cfg;
  cfg.initial_nav = 1.0;
  cfg.execution_delay_periods = 0;
  cfg.trade_bps = kTradeBps;
  const book::ReplayAllocationPolicy identity = [](const book::ReplayAllocationState &s)
    -> atx::core::Result<std::vector<atx::f64>> {
    return atx::core::Ok(
      std::vector<atx::f64>(s.preference_weights.begin(), s.preference_weights.end()));
  };
  const std::vector<atx::i64> keys{0, kDay};
  const std::vector<atx::usize> decisions{0};
  const auto replay =
    book::replay_scheduled_targets(f.panel, keys, decisions, f.target, identity, cfg);
  ASSERT_TRUE(replay.has_value()) << replay.error().message();
  ASSERT_EQ(replay->replay.intervals.size(), 1U);
  const auto &row = replay->replay.intervals[0];
  EXPECT_NEAR(row.gross_pnl / row.pretrade_nav, legacy.pnl_gross, 1e-15);
  EXPECT_NEAR(row.trade_cost / row.pretrade_nav, legacy.pnl_cost, 1e-15);
  EXPECT_NEAR(row.net_return, legacy_net, 1e-15);
  EXPECT_NEAR(row.start_gross / row.pretrade_nav, legacy.gross, 1e-15);
}

TEST(ReplayReportParity, OnePeriodParityHoldsThroughAFlatCostModel) {
  Fixture f;
  const auto flat = book::FlatBpsCost::create(7.5).value();
  book::ReplayConfig cfg;
  cfg.initial_nav = 1.0;
  cfg.execution_delay_periods = 0;
  cfg.cost_model = &flat;
  const std::vector<atx::i64> keys{0, kDay};
  const std::vector<atx::usize> decisions{0};
  const auto replay = book::replay_scheduled_targets(f.panel, keys, decisions, f.target, cfg);
  ASSERT_TRUE(replay.has_value()) << replay.error().message();
  atx::f64 turnover = 0.0;
  for (const auto w : f.target) turnover += std::abs(w);
  const auto ret_field = f.panel.field_id("ret").value();
  const auto legacy = book::detail::accumulate_period(
    f.target, f.panel.field_cross_section(ret_field, 0), f.panel, 0, turnover * 7.5);
  EXPECT_NEAR(replay->intervals[0].net_return,
        legacy.pnl_gross - legacy.pnl_cost - legacy.pnl_borrow, 1e-15);
}

} // namespace atx_test_l8_e2e_report_parity
