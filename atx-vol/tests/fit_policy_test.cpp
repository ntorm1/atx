#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "atx/vol/fit_policy.hpp"
#include "atx/vol/universe.hpp"

namespace {

using namespace atx::vol;

Chain make_chain(double T, std::size_t n_strikes, double spot, double spread_fraction) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = T;
  c.strikes.resize(n_strikes);
  const std::size_t n2 = 2u * n_strikes;
  c.bids.resize(n2);
  c.asks.resize(n2);
  c.mids.resize(n2);
  c.bid_sizes.assign(n2, 10);
  c.ask_sizes.assign(n2, 10);
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, 0u);
  for (std::size_t i = 0; i < n_strikes; ++i) {
    c.strikes[i] = spot * (0.5 + static_cast<double>(i + 1u) / static_cast<double>(n_strikes + 1u));
    for (Side side : {Side::Call, Side::Put}) {
      const std::size_t j = chain_index(static_cast<std::uint16_t>(i), side);
      const double mid = 1.0 + 0.001 * static_cast<double>(i);
      c.bids[j] = mid * (1.0 - 0.5 * spread_fraction);
      c.asks[j] = mid * (1.0 + 0.5 * spread_fraction);
      c.mids[j] = mid;
    }
  }
  return c;
}

Underlying make_underlier(std::string ticker, std::size_t n_strikes, double spread_fraction) {
  Underlying u;
  u.uid = 1u;
  u.ticker = std::move(ticker);
  u.spot = 100.0;
  u.chains.push_back(make_chain(0.05, n_strikes, u.spot, spread_fraction));
  return u;
}

TEST(FitPolicy, SpyTickerRoutesDirectlyToHftLinearVariance) {
  const Underlying u = make_underlier("SPY", 2500, 0.01);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_EQ(d.profile.kind, ProfileKind::IndexEtfUltraLiquid);
  EXPECT_EQ(d.preset, FitPreset::Hft);
  EXPECT_EQ(d.curve.kind, VolCurveKind::LinearVariance);
  EXPECT_FALSE(d.needs_cross_validation);
  EXPECT_EQ(d.source, FitDecisionSource::TickerPrior);
}

TEST(FitPolicy, SparseUnknownBoardUsesParsimoniousGuard) {
  const Underlying u = make_underlier("ZZZZ", 20, 0.60);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_EQ(d.profile.kind, ProfileKind::IlliquidSmallCap);
  EXPECT_EQ(d.curve.kind, VolCurveKind::Svi);
  EXPECT_FALSE(d.needs_cross_validation);
  EXPECT_EQ(d.source, FitDecisionSource::SparseGuard);
}

TEST(FitPolicy, ExplicitEventWindowSelectsC8) {
  const Underlying u = make_underlier("AAPL", 200, 0.04);
  FitContext context;
  context.event_phase = EventPhase::PreAnnouncement;
  FitPolicyConfig config;
  config.sparse_validation_floor = 0;
  const FitDecision d = select_fit_policy(u, u.ticker, context, config);
  EXPECT_EQ(d.profile.kind, ProfileKind::MegaCapEvent);
  EXPECT_EQ(d.curve.kind, VolCurveKind::C8);
  EXPECT_FALSE(d.needs_cross_validation);
}

TEST(FitPolicy, OpeningSnapshotSuppressesHighDofEventFit) {
  const Underlying u = make_underlier("AAPL", 200, 0.04);
  FitContext context;
  context.event_phase = EventPhase::PostAnnouncement;
  context.session_phase = MarketSessionPhase::Opening;
  FitPolicyConfig config;
  config.sparse_validation_floor = 0;
  const FitDecision d = select_fit_policy(u, u.ticker, context, config);
  EXPECT_EQ(d.curve.kind, VolCurveKind::Essvi);
}

// C8 spends eight free parameters on one slice. A ticker prior says WHICH
// underlier this is, not that today's snapshot carries enough quotes to identify
// them, so a thin event board must still drop to the five-parameter backbone.
// (Contrast ExplicitEventWindowSelectsC8, which lowers the floor to 0.)
TEST(FitPolicy, ThinEventBoardWillNotSpendEightDegreesOfFreedom) {
  const Underlying u = make_underlier("AAPL", 200, 0.04); // 400 live legs < 600 floor
  FitContext context;
  context.event_phase = EventPhase::PreAnnouncement;
  const FitDecision d = select_fit_policy(u, u.ticker); // default sparse_validation_floor
  EXPECT_LT(d.features.n_live_quotes, FitPolicyConfig{}.sparse_validation_floor);

  const FitDecision thin = select_fit_policy(u, u.ticker, context);
  EXPECT_EQ(thin.profile.kind, ProfileKind::MegaCapEvent);
  EXPECT_EQ(thin.source, FitDecisionSource::TickerPrior);
  EXPECT_EQ(thin.curve.kind, VolCurveKind::Essvi);
}

// An unseeded ticker keeps board provenance. This is the seam that used to test
// `verdict.confidence == 0.95`; a board vote must never impersonate a seed.
TEST(FitPolicy, UnseededTickerRetainsBoardProvenance) {
  const Underlying u = make_underlier("NOTATICKER", 2500, 0.01);
  const FitDecision d = select_fit_policy(u, u.ticker);
  EXPECT_EQ(d.source, FitDecisionSource::BoardFeatures);
  EXPECT_NE(d.profile.confidence, kTickerSeedConfidence);
}

// A board with no two-sided quote has no spread to measure; a 0.0 median would be
// the TIGHTEST possible value and would vote the most liquid tier. Assert on the
// classifier itself -- select_fit_policy's sparse guard would mask the inversion.
TEST(FitPolicy, EmptyBoardDoesNotVoteUltraLiquidIndex) {
  Underlying u;
  u.uid = 1u;
  u.ticker = "NOTATICKER";
  u.spot = 100.0;

  const ClassifierInputs features = classifier_inputs_from_underlier(u);
  EXPECT_EQ(features.n_live_quotes, 0u);
  EXPECT_GT(features.median_spread_pct, 0.40);
  EXPECT_NE(classify_profile(features).kind, ProfileKind::IndexEtfUltraLiquid);
}

TEST(FitPolicy, ForcedCrossValidationOverridesDirectTickerPrior) {
  const Underlying u = make_underlier("SPY", 2500, 0.01);
  FitPolicyConfig config;
  config.mode = FitSelectionMode::CrossValidated;
  const FitDecision d = select_fit_policy(u, u.ticker, {}, config);
  EXPECT_TRUE(d.needs_cross_validation);
  EXPECT_EQ(d.source, FitDecisionSource::CrossValidation);
}

} // namespace
