#include "fitting/slice_channel.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/vol_curve.hpp"

namespace atx::vol {
namespace {

constexpr double kYears = 0.5;

double smile(double k) { return 0.25 - 0.10 * k + 0.30 * k * k; }

// Served volatility of the fitted parameters through the production adapter.
double served(const ChannelFit &fit, double k) {
  const SplineVolCurve curve(fit.params, kYears, 100.0, 1.0);
  return curve.iv(k);
}

std::vector<ChannelObservation> tight_channels(double half_width, double mid_bias = 0.0) {
  std::vector<ChannelObservation> out;
  for (int i = -10; i <= 10; ++i) {
    const double k = 0.03 * i;
    const double s = smile(k);
    out.push_back({k, s - half_width, s + half_width, s + mid_bias, 1.0});
  }
  return out;
}

TEST(SliceChannel, RecoversAKnownSmileFromTightChannels) {
  const auto obs = tight_channels(0.002);
  const auto fit = fit_slice_channel(obs, kYears, nullptr);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_TRUE(fit->diagnostics.converged);
  EXPECT_EQ(fit->diagnostics.observations, obs.size());
  EXPECT_EQ(fit->diagnostics.mid_observations, obs.size());
  EXPECT_FALSE(fit->diagnostics.used_term_prior);
  for (const auto &o : obs) {
    EXPECT_NEAR(served(*fit, o.k), smile(o.k), 1.0e-3) << o.k;
  }
  EXPECT_EQ(fit->diagnostics.violations, 0U);
  EXPECT_NEAR(fit->params.atm_vol, smile(0.0), 2.0e-3);
}

TEST(SliceChannel, ChannelPenaltyKeepsTheCurveInsideWhenMidsAreBiased) {
  // Mids sit at the top edge of each channel; the fit may use the whole channel
  // but must not leave it materially.
  auto obs = tight_channels(0.004, 0.004);
  const auto fit = fit_slice_channel(obs, kYears, nullptr);
  ASSERT_TRUE(fit);
  for (const auto &o : obs) {
    const double s = served(*fit, o.k);
    EXPECT_LE(s, o.sigma_hi + 1.0e-4) << o.k;
    EXPECT_GE(s, o.sigma_lo - 1.0e-4) << o.k;
  }
}

TEST(SliceChannel, UpperBoundsAloneCapAPriorThatSitsAboveThem) {
  // Bidless contracts: only an ask volatility. A flat prior at 0.40 sits above
  // every upper bound (0.30), so the steep channel penalty must pull it down.
  std::vector<ChannelObservation> obs;
  for (int i = -4; i <= 4; ++i) {
    obs.push_back({0.05 * i, 0.0, 0.30});
  }
  const std::array<double, 3> z{-2.0, 0.0, 2.0};
  const std::array<double, 3> m{1.0, 1.0, 1.0};
  const ChannelPrior prior{0.40, z, m, 1.0};
  const auto fit = fit_slice_channel(obs, kYears, &prior);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_TRUE(fit->diagnostics.used_term_prior);
  for (const auto &o : obs) {
    EXPECT_LE(served(*fit, o.k), 0.30 + 0.01) << o.k;
  }
}

TEST(SliceChannel, PriorCarriesLevelAndShapeWhereChannelsAreUninformative) {
  // Lower bounds only (ask above the maximum-volatility price): the prior shape
  // decides the curve wherever it sits above the bounds.
  std::vector<ChannelObservation> obs;
  for (int i = -3; i <= 3; ++i) {
    obs.push_back({0.05 * i, 0.05, std::numeric_limits<double>::infinity()});
  }
  const std::array<double, 5> z{-2.0, -1.0, 0.0, 1.0, 2.0};
  const std::array<double, 5> m{1.2, 1.1, 1.0, 0.95, 0.92};
  const ChannelPrior prior{0.30, z, m, 2.0};
  const auto fit = fit_slice_channel(obs, kYears, &prior);
  ASSERT_TRUE(fit);
  const double axis = 0.30 * std::sqrt(kYears);
  EXPECT_NEAR(served(*fit, 0.0), 0.30, 2.0e-3);
  EXPECT_NEAR(served(*fit, -1.0 * axis), 0.33, 4.0e-3);
  EXPECT_NEAR(served(*fit, 1.0 * axis), 0.285, 4.0e-3);
}

TEST(SliceChannel, TermPriorPullsASparseSliceTowardItsNeighbours) {
  // Two wide two-sided quotes whose mids disagree with the prior level: a
  // strong prior moves the ATM level toward 0.35 but stays inside the channels.
  const std::vector<ChannelObservation> obs{{-0.02, 0.15, 0.45, 0.25, 1.0},
                                            {0.02, 0.15, 0.45, 0.25, 1.0}};
  const std::array<double, 1> z{0.0};
  const std::array<double, 1> m{1.0};
  const ChannelPrior weak{0.35, z, m, 0.01};
  const ChannelPrior strong{0.35, z, m, 100.0};
  const auto loose = fit_slice_channel(obs, kYears, &weak);
  const auto tight = fit_slice_channel(obs, kYears, &strong);
  ASSERT_TRUE(loose);
  ASSERT_TRUE(tight);
  EXPECT_LT(std::abs(served(*loose, 0.0) - 0.25), std::abs(served(*tight, 0.0) - 0.25));
  EXPECT_NEAR(served(*tight, 0.0), 0.35, 0.01);
  EXPECT_EQ(tight->diagnostics.violations, 0U);
}

TEST(SliceChannel, ServedWingsAreFlatBeyondTheOutermostKnot) {
  const auto fit = fit_slice_channel(tight_channels(0.002), kYears, nullptr);
  ASSERT_TRUE(fit);
  const double axis = fit->params.atm_vol * std::sqrt(kYears);
  const double edge = fit->params.z.back() * axis;
  EXPECT_DOUBLE_EQ(served(*fit, edge * 2.0), served(*fit, edge * 4.0));
}

TEST(SliceChannel, RefusesTooFewObservationsAndMalformedInputs) {
  const std::vector<ChannelObservation> one{{0.0, 0.2, 0.3, 0.25, 1.0}};
  const auto few = fit_slice_channel(one, kYears, nullptr);
  ASSERT_FALSE(few);
  EXPECT_EQ(few.error().code(), ErrorCode::Unavailable);
  // Uninformative channels (no lower and no upper information) are not usable.
  const std::vector<ChannelObservation> empty{
      {0.0, 0.0, std::numeric_limits<double>::infinity()},
      {0.1, 0.0, std::numeric_limits<double>::infinity()}};
  EXPECT_FALSE(fit_slice_channel(empty, kYears, nullptr));
  const auto obs = tight_channels(0.002);
  EXPECT_EQ(fit_slice_channel(obs, 0.0, nullptr).error().code(), ErrorCode::InvalidArgument);
  ChannelFitConfig bad;
  bad.prior_scale = 0.0;
  EXPECT_EQ(fit_slice_channel(obs, kYears, nullptr, bad).error().code(),
            ErrorCode::InvalidArgument);
  const std::array<double, 2> z{1.0, 0.0};
  const std::array<double, 2> m{1.0, 1.0};
  const ChannelPrior unsorted{0.3, z, m, 1.0};
  EXPECT_EQ(fit_slice_channel(obs, kYears, &unsorted).error().code(),
            ErrorCode::InvalidArgument);
}

TEST(SliceChannel, IsDeterministic) {
  const auto obs = tight_channels(0.003, 0.001);
  const auto a = fit_slice_channel(obs, kYears, nullptr);
  const auto b = fit_slice_channel(obs, kYears, nullptr);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(a->params.mult, b->params.mult);
  EXPECT_EQ(a->params.atm_vol, b->params.atm_vol);
}

} // namespace
} // namespace atx::vol
