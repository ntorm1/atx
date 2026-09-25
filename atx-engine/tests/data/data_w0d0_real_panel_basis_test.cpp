// W0-D0 — real_panel candle prices on close's TRI basis (D-03) and the real-panel
// level-basis tags.
//
// Suite: DataLevelBasis_W0d0 (real-panel half)
//
// build_real_panel set close = TRI while open/high/low/vwap stayed raw, so one panel
// mixed two price bases. The candle is now restated cell by cell with
// restate_on_tri_basis (price x TRI / raw_close), which build_real_panel applies to
// open/high/low/vwap. The full build reads a databento hive whose volume column is
// UInt64, which the in-process parquet writer cannot produce, so the build itself
// is exercised by the orchestrator's G0 smoke re-run; here the restatement is
// driven through the real adjust_total_return on a synthetic dividend + split series.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/data/adjust.hpp"
#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/data/real_panel.hpp"

namespace atx_test_w0_d0_real_panel_basis {

using atx::engine::data::adjust_total_return;
using atx::engine::data::AdjustedSeries;
using atx::engine::data::LevelBasis;
using atx::engine::data::real_panel_field_level_basis;
using atx::engine::data::RealDataConfig;
using atx::engine::data::RealPanelPriceBasis;
using atx::engine::data::restate_on_tri_basis;

namespace {
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
} // namespace

// Candle-to-close ratios after the restatement equal the raw bar's ratios on every
// session, across dividends and a split; the mixed legacy basis does not.
TEST(DataLevelBasis_W0d0, RealPanelCandleSharesTheTriBasis) {
  // 5 years of a flat 100 payer (0.75 a quarter), a 2:1 split near the end, and a
  // raw candle open = 0.99 x close, high = 1.02 x close, low = 0.97 x close.
  const atx::usize n = 5 * 252;
  std::vector<atx::f64> close(n, 100.0);
  std::vector<atx::f64> factor(n, 1.0);
  std::vector<atx::f64> div(n, 0.0);
  for (atx::usize t = 63; t < n; t += 63) {
    div[t] = 0.75;
  }
  for (atx::usize t = 0; t < n - 10; ++t) {
    factor[t] = 0.5; // split 10 sessions before the end: raw 100 -> 50
  }
  for (atx::usize t = n - 10; t < n; ++t) {
    close[t] = 50.0;
  }
  close[n / 2] = kNaN; // one missing session
  std::vector<atx::f64> open(n);
  std::vector<atx::f64> high(n);
  std::vector<atx::f64> low(n);
  for (atx::usize t = 0; t < n; ++t) {
    open[t] = close[t] * 0.99;
    high[t] = close[t] * 1.02;
    low[t] = close[t] * 0.97;
  }
  const AdjustedSeries adj = adjust_total_return(close, factor, div);
  const auto r_open = restate_on_tri_basis(open, adj.total_return_index, close);
  const auto r_high = restate_on_tri_basis(high, adj.total_return_index, close);
  const auto r_low = restate_on_tri_basis(low, adj.total_return_index, close);
  ASSERT_EQ(r_open.size(), n);

  atx::f64 worst_v2 = 0.0;
  atx::f64 worst_v1 = 0.0;
  for (atx::usize t = 0; t < n; ++t) {
    const atx::f64 tri = adj.total_return_index[t];
    if (std::isnan(close[t])) {
      EXPECT_TRUE(std::isnan(r_open[t]) && std::isnan(r_high[t]) && std::isnan(r_low[t]));
      continue;
    }
    worst_v2 = std::max(worst_v2, std::abs(r_open[t] / tri - 0.99));
    worst_v2 = std::max(worst_v2, std::abs(r_high[t] / tri - 1.02));
    worst_v2 = std::max(worst_v2, std::abs(r_low[t] / tri - 0.97));
    worst_v1 = std::max(worst_v1, std::abs(open[t] / tri - 0.99)); // raw open vs TRI close
  }
  EXPECT_LT(worst_v2, 1e-14) << "restated candle left close's basis";
  EXPECT_GT(worst_v1, 0.5) << "the mixed basis should be far off after 5 years + a split";
  std::printf("[real-candle] max |open/close - raw ratio|: TriScaledV2=%.3g MixedV1=%.3g\n",
              worst_v2, worst_v1);
}

// Missing / non-positive TRI or raw close gives NaN; length mismatch gives empty.
TEST(DataLevelBasis_W0d0, RestateNeverFabricatesAPrice) {
  const std::vector<atx::f64> price = {10.0, 10.0, 10.0, 10.0, kNaN};
  const std::vector<atx::f64> tri = {20.0, kNaN, 20.0, -1.0, 20.0};
  const std::vector<atx::f64> raw = {10.0, 10.0, 0.0, 10.0, 10.0};
  const auto out = restate_on_tri_basis(price, tri, raw);
  ASSERT_EQ(out.size(), 5U);
  EXPECT_DOUBLE_EQ(out[0], 20.0);
  for (atx::usize k = 1; k < 5; ++k) {
    EXPECT_TRUE(std::isnan(out[k])) << k;
  }
  EXPECT_TRUE(restate_on_tri_basis(price, tri, std::vector<atx::f64>(4, 1.0)).empty());
}

// Real-panel tags: the candle is adjusted_level under the default, raw under the
// legacy mixed basis; liquidity stays raw; the config defaults are the V2 rules.
TEST(DataLevelBasis_W0d0, RealPanelFieldTagsFollowThePriceBasis) {
  for (const auto name : {"close", "open", "high", "low", "vwap"}) {
    EXPECT_EQ(real_panel_field_level_basis(name), LevelBasis::AdjustedLevel) << name;
  }
  for (const auto name : {"open", "high", "low", "vwap"}) {
    EXPECT_EQ(real_panel_field_level_basis(name, RealPanelPriceBasis::MixedV1), LevelBasis::Raw)
        << name;
  }
  EXPECT_EQ(real_panel_field_level_basis("close", RealPanelPriceBasis::MixedV1),
            LevelBasis::AdjustedLevel);
  for (const auto name : {"raw_close", "volume", "dollar_volume", "adv21", "market_cap", "sector",
                          "regime_vix"}) {
    EXPECT_EQ(real_panel_field_level_basis(name), LevelBasis::Raw) << name;
  }
  const RealDataConfig cfg{};
  EXPECT_EQ(cfg.price_basis, RealPanelPriceBasis::TriScaledV2);
  EXPECT_EQ(cfg.corp_align, atx::engine::data::CorpAlignRule::EventOnceCappedV2);
  EXPECT_EQ(cfg.tri_gap_rule, atx::engine::data::TriGapRule::RatioChainV2);
}

} // namespace atx_test_w0_d0_real_panel_basis
