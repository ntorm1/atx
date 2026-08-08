// atx-vol/tests/realized_vol_test.cpp
#include "atx/vol/realized_vol.hpp"
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

}  // namespace
}  // namespace atx::vol
