#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/chain.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/spy_fixture.hpp"
#include "atx/vol/surface_policy.hpp"
#include "atx/vol/vol_curve.hpp"

// V2 release qualification over a deterministic, known-truth SPY-like board.
// These are deliberately black-box product tests: every assertion reads the
// immutable SurfaceBundle or the public valuation API, never fitter internals.

namespace {

using atx::vol::AmericanGreeks;
using atx::vol::ChainValuation;
using atx::vol::FitQualityMode;
using atx::vol::OptionChain;
using atx::vol::OptionId;
using atx::vol::OutputField;
using atx::vol::PricerConfig;
using atx::vol::PricerFitter;
using atx::vol::RiskAdmission;
using atx::vol::SurfaceFallback;
using atx::vol::SurfaceOutputs;
using atx::vol::SurfacePurpose;
using atx::vol::SurfaceState;
using atx::vol::ValidationFailure;
using atx::vol::VolCurveKind;
using atx::vol::has_validation_failure;
using atx::vol::make_spy_synthetic_spec;
using atx::vol::make_synthetic_american_panel;

[[nodiscard]] bool same(double a, double b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

void expect_same_greeks(const AmericanGreeks& a, const AmericanGreeks& b) {
  EXPECT_TRUE(same(a.delta, b.delta));
  EXPECT_TRUE(same(a.gamma, b.gamma));
  EXPECT_TRUE(same(a.vega, b.vega));
  EXPECT_TRUE(same(a.theta, b.theta));
  EXPECT_TRUE(same(a.rho, b.rho));
  EXPECT_TRUE(same(a.vanna, b.vanna));
  EXPECT_TRUE(same(a.volga, b.volga));
  EXPECT_TRUE(same(a.charm, b.charm));
  EXPECT_TRUE(same(a.price, b.price));
}

void expect_deterministic(const ChainValuation& serial,
                          const ChainValuation& threaded) {
  ASSERT_EQ(serial.size(), threaded.size());
  ASSERT_EQ(serial.filled, threaded.filled);
  ASSERT_EQ(serial.model_price.size(), threaded.model_price.size());
  ASSERT_EQ(serial.model_iv.size(), threaded.model_iv.size());
  ASSERT_EQ(serial.bid_iv.size(), threaded.bid_iv.size());
  ASSERT_EQ(serial.ask_iv.size(), threaded.ask_iv.size());
  ASSERT_EQ(serial.mid_iv.size(), threaded.mid_iv.size());
  ASSERT_EQ(serial.greeks.size(), threaded.greeks.size());
  for (std::size_t i = 0; i < serial.size(); ++i) {
    EXPECT_EQ(serial.ids[i], threaded.ids[i]);
    EXPECT_TRUE(same(serial.model_price[i], threaded.model_price[i]));
    EXPECT_TRUE(same(serial.model_iv[i], threaded.model_iv[i]));
    EXPECT_TRUE(same(serial.bid_iv[i], threaded.bid_iv[i]));
    EXPECT_TRUE(same(serial.ask_iv[i], threaded.ask_iv[i]));
    EXPECT_TRUE(same(serial.mid_iv[i], threaded.mid_iv[i]));
    expect_same_greeks(serial.greeks[i], threaded.greeks[i]);
  }
}

[[nodiscard]] std::optional<OptionChain> make_known_truth_chain() {
  const auto spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  if (!panel) return std::nullopt;
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  if (!chain) return std::nullopt;
  return std::optional<OptionChain>{std::move(*chain)};
}

[[nodiscard]] PricerConfig config_for(FitQualityMode mode) {
  PricerConfig cfg;
  cfg.quality_mode = mode;
  cfg.outputs = SurfaceOutputs::MarketMarkAndRisk;
  cfg.risk_admission = RiskAdmission::Required;
  cfg.fallback = SurfaceFallback::LastKnownGood;
  return cfg;
}

class SurfaceV2Qualification
    : public ::testing::TestWithParam<FitQualityMode> {};

TEST_P(SurfaceV2Qualification,
       KnownTruthBoardPublishesSeparatedAdmittedDeterministicBundle) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  const FitQualityMode mode = GetParam();
  PricerFitter fitter{config_for(mode)};

  const auto fitted = fitter.fit(*chain);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const auto bundle = fitter.bundle();

  ASSERT_TRUE(bundle.has(SurfacePurpose::MarketMark));
  ASSERT_TRUE(bundle.has(SurfacePurpose::Risk));
  ASSERT_NE(bundle.market_mark, nullptr);
  ASSERT_NE(bundle.risk, nullptr);
  EXPECT_NE(bundle.market_mark, bundle.risk);
  EXPECT_EQ(bundle.market_mark->purpose(), SurfacePurpose::MarketMark);
  EXPECT_EQ(bundle.risk->purpose(), SurfacePurpose::Risk);
  EXPECT_EQ(bundle.market_mark->quality_mode(), mode);
  EXPECT_EQ(bundle.risk->quality_mode(), mode);
  EXPECT_EQ(bundle.market_mark->generation(), bundle.candidate_generation);
  EXPECT_EQ(bundle.risk->generation(), bundle.candidate_generation);

  EXPECT_EQ(bundle.market_mark_health.state, SurfaceState::Healthy);
  EXPECT_EQ(bundle.market_mark_health.purpose, SurfacePurpose::MarketMark);
  EXPECT_TRUE(bundle.market_mark_health.serving_candidate());
  EXPECT_EQ(bundle.risk_health.state, SurfaceState::Healthy);
  EXPECT_EQ(bundle.risk_health.purpose, SurfacePurpose::Risk);
  EXPECT_EQ(bundle.risk_health.quality_mode, mode);
  EXPECT_TRUE(bundle.risk_health.serving_candidate());
  EXPECT_FALSE(bundle.risk_health.using_fallback());

  const auto& digest = bundle.risk_health.validation;
  EXPECT_TRUE(digest.admitted());
  EXPECT_EQ(digest.failures, ValidationFailure::None);
  EXPECT_NE(digest.validation_id, 0u);
  EXPECT_GT(digest.n_slices, 0u);
  EXPECT_GT(digest.n_strike_samples, 0u);
  EXPECT_EQ(digest.n_non_finite, 0u);
  EXPECT_EQ(digest.n_price_bound_violations, 0u);
  EXPECT_EQ(digest.n_strike_monotonicity_violations, 0u);
  EXPECT_EQ(digest.n_butterfly_violations, 0u);
  EXPECT_EQ(digest.n_calendar_violations, 0u);
  EXPECT_EQ(digest.n_wing_violations, 0u);

  auto mark_priced = bundle.market_mark->session().to_priced_surface();
  auto risk_priced = bundle.risk->session().to_priced_surface();
  ASSERT_TRUE(mark_priced.has_value()) << mark_priced.error().to_string();
  ASSERT_TRUE(risk_priced.has_value()) << risk_priced.error().to_string();
  ASSERT_GT(mark_priced->n_slices(), 0u);
  ASSERT_GT(risk_priced->n_slices(), 0u);
  EXPECT_EQ(mark_priced->kind_at(0), VolCurveKind::LinearVariance);
  EXPECT_NE(risk_priced->kind_at(0), VolCurveKind::LinearVariance);

  const auto& input_diag = bundle.risk->diagnostics();
  EXPECT_TRUE(input_diag.carry_confident);
  EXPECT_TRUE(input_diag.inversion_certified);
  EXPECT_EQ(input_diag.n_iv_rejected_residual, 0u);

  const auto serial = fitter.value_chain(*chain, OutputField::All, 1);
  const auto threaded = fitter.value_chain(*chain, OutputField::All, 4);
  ASSERT_TRUE(serial.has_value()) << serial.error().to_string();
  ASSERT_TRUE(threaded.has_value()) << threaded.error().to_string();
  expect_deterministic(*serial, *threaded);
}

INSTANTIATE_TEST_SUITE_P(
    AllQualityModes, SurfaceV2Qualification,
    ::testing::Values(FitQualityMode::Latency, FitQualityMode::Balanced,
                      FitQualityMode::Accuracy),
    [](const ::testing::TestParamInfo<FitQualityMode>& info) {
      switch (info.param) {
        case FitQualityMode::Latency: return std::string{"Latency"};
        case FitQualityMode::Balanced: return std::string{"Balanced"};
        case FitQualityMode::Accuracy: return std::string{"Accuracy"};
      }
      return std::string{"Unknown"};
    });

TEST(SurfaceV2Fallback, InvalidRefreshKeepsLastAdmittedRiskGeneration) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  PricerFitter fitter{config_for(FitQualityMode::Balanced)};
  ASSERT_TRUE(fitter.fit(*chain).has_value());

  const auto before = fitter.bundle();
  ASSERT_NE(before.risk, nullptr);
  const std::uint64_t admitted_generation = before.risk->generation();
  const std::uint64_t validation_id = before.risk_health.validation.validation_id;
  const double sample_T = before.risk->session().expiries().front().T;
  const double sample_iv = before.risk->iv(chain->spot(), sample_T);
  ASSERT_TRUE(std::isfinite(sample_iv));

  const std::vector<OptionId> ids = chain->ids();
  std::vector<double> bids(ids.size(), 2.0);
  std::vector<double> asks(ids.size(), 1.0);  // crossed: every row is invalid
  ASSERT_TRUE(chain
                  ->update_quotes(std::span<const OptionId>(ids),
                                  std::span<const double>(bids),
                                  std::span<const double>(asks))
                  .has_value());

  // The refresh itself succeeds operationally because an admitted generation
  // exists to serve. The candidate is rejected and must not replace it.
  const auto refreshed = fitter.fit(*chain);
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().to_string();
  const auto after = fitter.bundle();
  ASSERT_NE(after.risk, nullptr);
  EXPECT_EQ(after.candidate_generation, before.candidate_generation + 1u);
  EXPECT_EQ(after.risk->generation(), admitted_generation);
  EXPECT_EQ(after.risk_health.state, SurfaceState::Degraded);
  EXPECT_TRUE(after.risk_health.using_fallback());
  EXPECT_FALSE(after.risk_health.serving_candidate());
  EXPECT_EQ(after.risk_health.served_generation, admitted_generation);
  EXPECT_EQ(after.risk_health.fallback_generation, admitted_generation);
  EXPECT_TRUE(has_validation_failure(after.risk_health.reasons,
                                     ValidationFailure::InsufficientData));
  EXPECT_EQ(after.risk_health.validation.validation_id, 0u);
  EXPECT_EQ(before.risk_health.validation.validation_id, validation_id);
  EXPECT_DOUBLE_EQ(after.risk->iv(chain->spot(), sample_T), sample_iv);

  ASSERT_NE(after.market_mark, nullptr);
  EXPECT_EQ(after.market_mark_health.state, SurfaceState::Stale);
  EXPECT_TRUE(after.market_mark_health.using_fallback());
}

}  // namespace
