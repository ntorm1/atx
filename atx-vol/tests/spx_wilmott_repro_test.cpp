#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/black76.hpp"
#include "spx_wilmott_repro_support.hpp"

namespace atx::vol::spx_wilmott {
namespace {

class FlatCurve final : public IVolCurve {
public:
  FlatCurve(double sigma, double T, double F, double df) noexcept
      : IVolCurve(T, F, df), sigma_(sigma) {}

  [[nodiscard]] double w(double /*k_log*/) const noexcept override { return sigma_ * sigma_ * T_; }
  [[nodiscard]] double iv(double /*k_log*/) const noexcept override { return sigma_; }
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::Essvi; }
  [[nodiscard]] std::size_t dof() const noexcept override { return 1u; }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<FlatCurve>(sigma_, T_, F_, df_);
  }

private:
  double sigma_{0.0};
};

void add_pair(Chain &chain, double strike, double forward_at_strike, double T, double r) {
  const std::uint16_t strike_index = static_cast<std::uint16_t>(chain.strikes.size());
  chain.strikes.push_back(strike);
  chain.bids.resize(chain.strikes.size() * 2u);
  chain.asks.resize(chain.strikes.size() * 2u);
  chain.mids.resize(chain.strikes.size() * 2u);
  chain.flags.resize(chain.strikes.size() * 2u);

  constexpr double kPutMid = 20.0;
  const double call_mid = kPutMid + (forward_at_strike - strike) * std::exp(-r * T);
  for (const Side side : {Side::Call, Side::Put}) {
    const std::size_t index = chain_index(strike_index, side);
    const double mid = side == Side::Call ? call_mid : kPutMid;
    chain.bids[index] = mid - 0.05;
    chain.asks[index] = mid + 0.05;
    chain.mids[index] = mid;
    chain.flags[index] = 0u;
  }
}

TEST(SpxWilmottForward, SelectsNearAtmPairsAndUsesMedian) {
  constexpr double kT = 0.25;
  constexpr double kR = 0.02;
  Chain chain;
  add_pair(chain, 90.0, 101.0, kT, kR);
  add_pair(chain, 95.0, 100.5, kT, kR);
  add_pair(chain, 100.0, 100.0, kT, kR);
  add_pair(chain, 105.0, 99.5, kT, kR);
  add_pair(chain, 110.0, 99.0, kT, kR);

  const auto estimate = estimate_european_forward(chain, kT, kR, 3u);

  ASSERT_TRUE(estimate.has_value()) << estimate.error().message();
  EXPECT_NEAR(estimate->forward, 100.0, 1.0e-12);
  EXPECT_NEAR(estimate->mad, 0.5, 1.0e-12);
  EXPECT_EQ(estimate->n_pairs, 3u);
}

TEST(SpxWilmottForward, RejectsEmptyPairBudget) {
  Chain chain;
  const auto estimate = estimate_european_forward(chain, 0.25, 0.02, 0u);
  ASSERT_FALSE(estimate.has_value());
  EXPECT_EQ(estimate.error().code(), ErrorCode::InvalidArgument);
}

TEST(SpxWilmottNormalization, InterpolatesAtmSigmaInLogMoneyness) {
  std::vector<FitObs> observations(2);
  observations[0].k = -0.1;
  observations[0].sigma_mkt = 0.30;
  observations[1].k = 0.1;
  observations[1].sigma_mkt = 0.20;

  EXPECT_NEAR(interpolate_atm_sigma(observations, 0.25), 0.25, 1.0e-14);
  EXPECT_TRUE(std::isnan(interpolate_atm_sigma({}, 0.25)));
}

TEST(SpxWilmottVisualObservations, IncludesZeroBidOtmQuoteAtLowWeight) {
  constexpr double kF = 100.0;
  constexpr double kK = 110.0;
  constexpr double kT = 0.25;
  constexpr double kDf = 0.99;
  constexpr double kSigma0 = 0.20;
  Chain chain;
  chain.strikes = {kK};
  chain.bids = {0.0, 1.0};
  chain.asks = {2.0 * black76_price(kF, kK, kT, 0.30, kDf, Side::Call), 2.0};
  chain.mids = {0.5 * chain.asks[0], 1.5};
  chain.flags = {0u, 0u};

  const auto visual = build_figure_reproduction_observations(chain, kF, kT, kDf, kSigma0);

  ASSERT_TRUE(visual.has_value()) << visual.error().message();
  ASSERT_EQ(visual->obs.size(), 1u);
  EXPECT_EQ(visual->n_zero_bid, 1u);
  EXPECT_EQ(visual->n_rejected, 0u);
  EXPECT_GT(visual->obs[0].weight_w, 0.0);
  EXPECT_LT(visual->obs[0].active_weight_w, 1.0e3);
  EXPECT_EQ(visual->obs[0].side, Side::Call);
}

TEST(SpxWilmottVisualObservations, RejectsCrossedPreferredQuote) {
  Chain chain;
  chain.strikes = {110.0};
  chain.bids = {2.0, 1.0};
  chain.asks = {1.0, 2.0};
  chain.mids = {1.5, 1.5};
  chain.flags = {0u, 0u};

  const auto visual = build_figure_reproduction_observations(chain, 100.0, 0.25, 0.99, 0.20);

  ASSERT_TRUE(visual.has_value()) << visual.error().message();
  EXPECT_TRUE(visual->obs.empty());
  EXPECT_EQ(visual->n_zero_bid, 0u);
  EXPECT_EQ(visual->n_rejected, 1u);
}

TEST(SpxWilmottPriceCone, AcceptsConvexBoundedCallNodes) {
  ConvexSliceFit fit;
  fit.T = 0.25;
  fit.F = 100.0;
  fit.df = 1.0;
  fit.u = {80.0, 100.0, 120.0};
  fit.C = {21.0, 8.0, 3.0};

  const PriceConeScore score = check_convex_price_cone(fit);

  EXPECT_TRUE(score.clean());
  EXPECT_DOUBLE_EQ(score.max_slope_decrease, 0.0);
}

TEST(SpxWilmottPriceCone, FlagsDecreasingAdjacentSlope) {
  ConvexSliceFit fit;
  fit.T = 0.25;
  fit.F = 100.0;
  fit.df = 1.0;
  fit.u = {80.0, 100.0, 120.0};
  fit.C = {21.0, 8.0, -7.0};

  const PriceConeScore score = check_convex_price_cone(fit);

  EXPECT_GT(score.price_bound_violations, 0u);
  EXPECT_EQ(score.convexity_violations, 1u);
  EXPECT_NEAR(score.max_slope_decrease, 0.10, 1.0e-14);
}

TEST(SpxWilmottScore, ReportsIvErrorAndBlack76BandHit) {
  constexpr double kF = 100.0;
  constexpr double kK = 100.0;
  constexpr double kT = 0.25;
  constexpr double kDf = 0.99;
  const double bid = black76_price(kF, kK, kT, 0.18, kDf, Side::Call);
  const double ask = black76_price(kF, kK, kT, 0.22, kDf, Side::Call);

  FitObs observation;
  observation.k = 0.0;
  observation.sigma_mkt = 0.20;
  observation.K = kK;
  observation.F = kF;
  observation.df = kDf;
  observation.mid = 0.5 * (bid + ask);
  observation.spread = ask - bid;
  observation.side = Side::Call;
  const FlatCurve curve(0.20, kT, kF, kDf);

  const FitScore score = score_curve(std::span<const FitObs>(&observation, 1u), curve);

  EXPECT_EQ(score.n_scored, 1u);
  EXPECT_EQ(score.n_band_scored, 1u);
  EXPECT_EQ(score.n_in_band, 1u);
  EXPECT_DOUBLE_EQ(score.rmse_iv, 0.0);
  EXPECT_DOUBLE_EQ(score.max_abs_iv_error, 0.0);
  EXPECT_DOUBLE_EQ(score.in_band_percent, 100.0);
}

} // namespace
} // namespace atx::vol::spx_wilmott
