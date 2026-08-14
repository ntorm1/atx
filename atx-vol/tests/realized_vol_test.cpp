// atx-vol/tests/realized_vol_test.cpp
#include "analytics/realized_vol.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>

namespace atx::vol {
namespace {

// Deterministic GBM daily bars: drift-free, sigma annualized, 252 steps/yr.
std::vector<OhlcBar> synth_gbm_bars(double sigma, std::size_t n, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> z(0.0, 1.0);
  const double dt = 1.0 / 252.0, sq = sigma * std::sqrt(dt);
  std::vector<OhlcBar> bars;
  bars.reserve(n);
  double s = 100.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double open = s;
    const double close = open * std::exp(-0.5 * sq * sq + sq * z(rng));
    // Intraday extremes: bracket open/close with a half-range excursion.
    const double ex = 0.5 * sq * std::abs(z(rng));
    const double hi = std::max(open, close) * std::exp(ex);
    const double lo = std::min(open, close) * std::exp(-ex);
    bars.push_back(OhlcBar{static_cast<std::int64_t>(i) * 86'400'000'000'000LL,
                           open, hi, lo, close});
    s = close;
  }
  return bars;
}

TEST(RealizedVol, CloseToCloseRecoversGbmSigmaWithinSamplingError) {
  const auto bars = synth_gbm_bars(0.20, 5000, 42u);
  const auto rv = realized_vol(bars, RvEstimator::CloseToClose);
  ASSERT_TRUE(rv.has_value());
  EXPECT_NEAR(*rv, 0.20, 0.01);  // ~2*sigma/sqrt(2n) sampling band
}

TEST(RealizedVol, AllEstimatorsAgreeOnGapFreeGbmWithinTolerance) {
  const auto bars = synth_gbm_bars(0.30, 5000, 7u);
  for (auto est : {RvEstimator::Parkinson, RvEstimator::GarmanKlass,
                   RvEstimator::RogersSatchell, RvEstimator::YangZhang}) {
    const auto rv = realized_vol(bars, est);
    ASSERT_TRUE(rv.has_value());
    EXPECT_NEAR(*rv, 0.30, 0.05) << static_cast<int>(est);
  }
}

TEST(RealizedVol, TwoBarMinimumEnforced) {
  const auto bars = synth_gbm_bars(0.20, 1, 1u);
  EXPECT_FALSE(realized_vol(bars, RvEstimator::CloseToClose).has_value());
}

TEST(RealizedVol, NonPositiveOhlcRejected) {
  std::vector<OhlcBar> bars = synth_gbm_bars(0.20, 10, 3u);
  bars[4].low = 0.0;
  EXPECT_FALSE(realized_vol(bars, RvEstimator::YangZhang).has_value());
}

TEST(RealizedVol, PanelWindowsAreTrailingAndOrdered) {
  const auto bars = synth_gbm_bars(0.25, 300, 9u);
  const auto p = realized_vol_panel(bars);
  ASSERT_TRUE(p.has_value());
  for (double v : p->vol) EXPECT_TRUE(std::isfinite(v) && v > 0.05 && v < 0.60);
}

// Item 1 / M2: a 1-bar history can't form a return term for ANY window (all
// four are >= 2), so every slot degrades to the per-slot NaN flag rather than
// the whole panel failing -- CloseToClose, the estimator the shipped backtest
// example actually drives this with (TheoEdgeSignalStrategy::signals(), on
// every step starting at bars_.size() == 1).
TEST(RealizedVol, PanelOneBarHistoryIsAllNaNSlots) {
  const auto bars = synth_gbm_bars(0.20, 1, 11u);
  const auto p = realized_vol_panel(bars, RvEstimator::CloseToClose);
  ASSERT_TRUE(p.has_value());
  for (double v : p->vol)
    EXPECT_TRUE(std::isnan(v));
}

// Item 1 / M2: a 3-bar history is >= 2 for every window, so no slot flags
// NaN -- but every window (5, 21, 63, 252) exceeds 3 bars, so every slot's
// trailing slice silently falls back to the SAME whole 3-bar span. All four
// vols come out identical even though window[1]/[2] are still labelled 21d/
// 63d -- the undisclosed mislabeling the review named (a short real history
// blends against a 2-3 day realized vol labelled 21-day).
TEST(RealizedVol, PanelThreeBarHistoryFallsBackToWholeSpanForEveryWindow) {
  const auto bars = synth_gbm_bars(0.20, 3, 12u);
  const auto p = realized_vol_panel(bars, RvEstimator::CloseToClose);
  ASSERT_TRUE(p.has_value());
  const auto direct = realized_vol(bars, RvEstimator::CloseToClose);
  ASSERT_TRUE(direct.has_value());
  for (std::size_t i = 0; i < p->vol.size(); ++i) {
    EXPECT_TRUE(std::isfinite(p->vol[i])) << "slot " << i;
    EXPECT_DOUBLE_EQ(p->vol[i], *direct) << "slot " << i;
  }
}

}  // namespace
}  // namespace atx::vol
