#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/pricing/american.hpp" // al_default_opts (accurate reference AL preset)
#include "atx/vol/api/core/chain.hpp"
#include "atx/vol/api/marketdata/data.hpp" // year_fraction
#include "atx/vol/api/backtest/panel.hpp"
#include "atx/vol/api/fitting/pricer_fitter.hpp"
#include "atx/vol/api/fitting/s3.hpp" // S3Params (crossed-wing fixture truth)
#include "atx/vol/api/fitting/session.hpp"
#include "fitting/spy_fixture.hpp"
#include "atx/vol/api/fitting/surface_policy.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"

// V2 release qualification over a deterministic, known-truth SPY-like board.
// These are deliberately black-box product tests: every assertion reads the
// immutable SurfaceBundle or the public valuation API, never fitter internals.

namespace {

using atx::vol::al_default_opts;
using atx::vol::AlOpts;
using atx::vol::AmericanGreeks;
using atx::vol::ChainValuation;
using atx::vol::CurveConfig;
using atx::vol::ErrorCode;
using atx::vol::FitDecision;
using atx::vol::FitPreset;
using atx::vol::FitQualityMode;
using atx::vol::has_validation_failure;
using atx::vol::make_spy_synthetic_spec;
using atx::vol::make_synthetic_american_panel;
using atx::vol::OptionChain;
using atx::vol::OptionId;
using atx::vol::OutputField;
using atx::vol::PricerConfig;
using atx::vol::PricerFitter;
using atx::vol::RiskAdmission;
using atx::vol::S3Params;
using atx::vol::SessionInputs;
using atx::vol::Side;
using atx::vol::SurfaceFallback;
using atx::vol::SurfaceOutputs;
using atx::vol::SurfacePurpose;
using atx::vol::SurfaceState;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::ValidationFailure;
using atx::vol::VolCurveKind;
using atx::vol::year_fraction;

[[nodiscard]] bool same(double a, double b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

void expect_same_greeks(const AmericanGreeks &a, const AmericanGreeks &b) {
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

void expect_deterministic(const ChainValuation &serial, const ChainValuation &threaded) {
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
  if (!panel)
    return std::nullopt;
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  if (!chain)
    return std::nullopt;
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

class SurfaceV2Qualification : public ::testing::TestWithParam<FitQualityMode> {};

TEST_P(SurfaceV2Qualification, KnownTruthBoardPublishesSeparatedAdmittedDeterministicBundle) {
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
  ASSERT_TRUE(fitter.published_provenance(SurfacePurpose::MarketMark).has_value());
  ASSERT_TRUE(fitter.published_provenance(SurfacePurpose::Risk).has_value());
  EXPECT_EQ(fitter.published_provenance(SurfacePurpose::MarketMark)->chain_instance_id,
            chain->instance_id());
  EXPECT_EQ(fitter.published_provenance(SurfacePurpose::Risk)->chain_instance_id,
            chain->instance_id());
  EXPECT_EQ(fitter.published_provenance()->chain_instance_id, chain->instance_id());

  EXPECT_EQ(bundle.market_mark_health.state, SurfaceState::Healthy);
  EXPECT_EQ(bundle.market_mark_health.purpose, SurfacePurpose::MarketMark);
  EXPECT_TRUE(bundle.market_mark_health.serving_candidate());
  EXPECT_EQ(bundle.risk_health.state, SurfaceState::Healthy);
  EXPECT_EQ(bundle.risk_health.purpose, SurfacePurpose::Risk);
  EXPECT_EQ(bundle.risk_health.quality_mode, mode);
  EXPECT_TRUE(bundle.risk_health.serving_candidate());
  EXPECT_FALSE(bundle.risk_health.using_fallback());

  const auto &digest = bundle.risk_health.validation;
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

  const auto &input_diag = bundle.risk->diagnostics();
  EXPECT_TRUE(input_diag.carry_confident);
  EXPECT_TRUE(input_diag.inversion_certified);
  EXPECT_EQ(input_diag.n_iv_rejected_residual, 0u);

  const auto serial = fitter.value_chain(*chain, OutputField::All, 1);
  const auto threaded = fitter.value_chain(*chain, OutputField::All, 4);
  ASSERT_TRUE(serial.has_value()) << serial.error().to_string();
  ASSERT_TRUE(threaded.has_value()) << threaded.error().to_string();
  expect_deterministic(*serial, *threaded);
}

INSTANTIATE_TEST_SUITE_P(AllQualityModes, SurfaceV2Qualification,
                         ::testing::Values(FitQualityMode::Latency, FitQualityMode::Balanced,
                                           FitQualityMode::Accuracy),
                         [](const ::testing::TestParamInfo<FitQualityMode> &info) {
                           switch (info.param) {
                           case FitQualityMode::Latency:
                             return std::string{"Latency"};
                           case FitQualityMode::Balanced:
                             return std::string{"Balanced"};
                           case FitQualityMode::Accuracy:
                             return std::string{"Accuracy"};
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
  std::vector<double> asks(ids.size(), 1.0); // crossed: every row is invalid
  ASSERT_TRUE(chain
                  ->update_quotes(std::span<const OptionId>(ids), std::span<const double>(bids),
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
  EXPECT_TRUE(
      has_validation_failure(after.risk_health.reasons, ValidationFailure::InsufficientData));
  EXPECT_EQ(after.risk_health.validation.validation_id, 0u);
  EXPECT_EQ(before.risk_health.validation.validation_id, validation_id);
  EXPECT_DOUBLE_EQ(after.risk->iv(chain->spot(), sample_T), sample_iv);
  // SurfaceBundle owns immutable generation leases; retaining `before` across
  // publication cannot dangle even after the fitter replaces other generations.
  EXPECT_DOUBLE_EQ(before.risk->iv(chain->spot(), sample_T), sample_iv);

  ASSERT_NE(after.market_mark, nullptr);
  EXPECT_EQ(after.market_mark_health.state, SurfaceState::Stale);
  EXPECT_TRUE(after.market_mark_health.using_fallback());
}

// The per-mode risk-policy budgets must reach the session build unclobbered.
// apply_risk_policy pins the ACCURATE Andersen-Lake reference preset expressly;
// an unset al_opts would let VolaSession::build substitute the fast preset,
// loosen iv_tol to 1e-5, and force a SINGLE-pair carry floor — silently
// reverting every floor asserted below (review finding I1/C3).
TEST_P(SurfaceV2Qualification, RiskBuildRunsTheModeCarryAndInversionBudgets) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  const FitQualityMode mode = GetParam();
  PricerFitter fitter{config_for(mode)};
  const auto fitted = fitter.fit(*chain);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.risk, nullptr);

  const SessionInputs &inputs = bundle.risk->session().inputs();
  const std::size_t expected_n_atm = mode == FitQualityMode::Latency    ? 3u
                                     : mode == FitQualityMode::Balanced ? 8u
                                                                        : 12u;
  // max_borrow_pairs re-pin (WS-M M3). Standing authorization: e7d5ebb "perf(vol):
  // halve real OPRA de-Americanization time" (2026-07-15) capped Latency 6->5 and
  // Balanced 12->5 at the evidence-based five-pair carry cap (deamer.hpp:302 "real-
  // OPRA accuracy plateau"; matching apply_fit_preset in session.cpp and its
  // deamer/session tests). That commit was NOT economically material: it only trims
  // redundant borrow solves past the plateau — carry inference is a ~1e-4 input
  // (deamer.hpp:295) and multi-pair coverage is unchanged (min_carry_effective_pairs
  // >= 3.0, asserted below, above the 3-pair confidence floor). The perf commit
  // updated the code + deamer/session tests but missed this budget (pinned 6:12 by
  // 4b769dd ~1.5h earlier the same day), leaving it stale. Accuracy is the reference-
  // fidelity contract and deliberately retains 12 (pricer_fitter.cpp:1131).
  const std::size_t expected_max_pairs = mode == FitQualityMode::Accuracy ? 12u : 5u;
  EXPECT_EQ(inputs.deam.n_atm, expected_n_atm);
  EXPECT_EQ(inputs.deam.max_borrow_pairs, expected_max_pairs);
  ASSERT_TRUE(inputs.deam.al_opts.has_value());
  const AlOpts accurate = al_default_opts();
  EXPECT_EQ(inputs.deam.al_opts->n_collocation, accurate.n_collocation);
  EXPECT_EQ(inputs.deam.al_opts->n_quadrature, accurate.n_quadrature);
  EXPECT_EQ(inputs.deam.al_opts->max_newton_iter, accurate.max_newton_iter);
  EXPECT_DOUBLE_EQ(inputs.deam.al_opts->tol, accurate.tol);
  const double expected_iv_tol = mode == FitQualityMode::Latency ? 1.0e-5 : 1.0e-7;
  EXPECT_DOUBLE_EQ(inputs.deam.iv_tol, expected_iv_tol);
  EXPECT_TRUE(inputs.deam.require_carry_confidence);

  // And the carry strip actually resolved multi-pair at (or above) the
  // admission gate's confidence floor.
  const auto &diagnostics = bundle.risk->diagnostics();
  EXPECT_TRUE(diagnostics.carry_confident);
  EXPECT_GE(diagnostics.min_carry_effective_pairs, 3.0);
}

TEST(SurfaceV2Fallback, DisabledFallbackMakesRejectedGenerationUnserviceable) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  PricerConfig config = config_for(FitQualityMode::Balanced);
  config.fallback = SurfaceFallback::None;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value());
  ASSERT_NE(fitter.risk_surface(), nullptr);

  const std::vector<OptionId> ids = chain->ids();
  std::vector<double> bids(ids.size(), 2.0);
  std::vector<double> asks(ids.size(), 1.0);
  ASSERT_TRUE(chain
                  ->update_quotes(std::span<const OptionId>(ids), std::span<const double>(bids),
                                  std::span<const double>(asks))
                  .has_value());

  EXPECT_FALSE(fitter.fit(*chain).has_value());
  const auto rejected = fitter.bundle();
  EXPECT_EQ(rejected.risk, nullptr);
  EXPECT_EQ(rejected.risk_health.state, SurfaceState::Rejected);
  EXPECT_EQ(rejected.risk_health.served_generation, 0u);
  EXPECT_FALSE(fitter.value_chain(*chain, OutputField::ModelIV, SurfacePurpose::Risk).has_value());
}

// Strip every expiry's put bids at all but two near-ATM strikes. At most two
// both-sided co-terminal pairs survive per slice — under
// min_confident_borrow_pairs — so the risk build (require_carry_confidence)
// drops every slice and fails outright, while the Hft LinearVariance mark —
// single-pair borrow, no confidence gate — still publishes Healthy.
void strip_put_bids_to_two_pairs(OptionChain &chain) {
  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : chain.ids()) {
    const auto option = chain.at(id);
    ASSERT_TRUE(option.has_value());
    if (option->side != Side::Put)
      continue;
    if (option->strike == 595.0 || option->strike == 605.0)
      continue;
    ids.push_back(id);
    bids.push_back(0.0); // one-sided quote: kills the co-terminal carry pair
    asks.push_back(option->ask);
  }
  ASSERT_FALSE(ids.empty());
  ASSERT_TRUE(chain
                  .update_quotes(std::span<const OptionId>(ids), std::span<const double>(bids),
                                 std::span<const double>(asks))
                  .has_value());
}

// Charter §3/§5.6/§12: after a risk rejection with fallback=None the fitter
// must fail closed — the healthy market mark exists but may only be served
// through the EXPLICIT MarketMark purpose, never through the purpose-less
// accessors of a config that requested a risk output.
TEST(SurfaceV2FailClosed, UnservedRiskIsNeverAnsweredByTheMarketMark) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  strip_put_bids_to_two_pairs(*chain);

  // MERGE: a pinned curve with default (Balanced) v2 fields is main's
  // single-surface transactional shape (essvi_config in pricer_fitter_test).
  // This branch test wants the v2 dual pipeline with a pinned deterministic
  // risk build, so name an explicit v2 quality to route through it (the
  // fail-closed serving contract under test is quality-agnostic).
  PricerConfig config = config_for(FitQualityMode::Latency);
  config.fallback = SurfaceFallback::None;
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  config.curve = essvi; // pinned: one deterministic risk build, no ladder
  PricerFitter fitter{config};
  EXPECT_FALSE(fitter.fit(*chain).has_value());

  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.market_mark, nullptr);
  EXPECT_EQ(bundle.market_mark_health.state, SurfaceState::Healthy);
  EXPECT_EQ(bundle.risk, nullptr);
  EXPECT_EQ(bundle.risk_health.state, SurfaceState::Rejected);
  ASSERT_TRUE(fitter.published_provenance(SurfacePurpose::MarketMark).has_value());
  EXPECT_EQ(fitter.published_provenance(SurfacePurpose::MarketMark)->chain_instance_id,
            chain->instance_id());
  EXPECT_FALSE(fitter.published_provenance(SurfacePurpose::Risk).has_value());
  // The no-argument accessor follows the requested default purpose and stays
  // fail-closed even though explicit mark provenance exists.
  EXPECT_FALSE(fitter.published_provenance().has_value());

  // Fail-closed purpose-less serving: no silent mark-for-risk substitution.
  EXPECT_FALSE(fitter.fitted());
  EXPECT_EQ(fitter.surface(), nullptr);
  const auto default_purpose = fitter.value_chain(*chain, OutputField::ModelIV);
  ASSERT_FALSE(default_purpose.has_value());
  EXPECT_EQ(default_purpose.error().code(), ErrorCode::Unavailable);

  // The mark remains reachable, but only by naming it.
  const auto explicit_mark =
      fitter.value_chain(*chain, OutputField::ModelIV, SurfacePurpose::MarketMark);
  EXPECT_TRUE(explicit_mark.has_value())
      << (explicit_mark ? "" : explicit_mark.error().to_string());
}

TEST(SurfaceV2FailClosed, MarkOnlyConfigServesTheMarkByDefault) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  PricerConfig config;
  config.outputs = SurfaceOutputs::MarketMark; // explicit mark-only request
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value());

  EXPECT_TRUE(fitter.fitted());
  ASSERT_NE(fitter.surface(), nullptr);
  EXPECT_EQ(fitter.surface()->purpose(), SurfacePurpose::MarketMark);
  const auto valued = fitter.value_chain(*chain, OutputField::ModelIV);
  EXPECT_TRUE(valued.has_value()) << (valued ? "" : valued.error().to_string());
}

// Charter §9: legacy `FitPreset::Hft + curve=LinearVariance` — the documented
// explicit spelling of the legacy dense mark route — maps to Latency +
// MarketMark. Never an implicit risk request, and never a hard error.
TEST(SurfaceV2LegacyCompat, HftWithPinnedLinearVarianceIsAMarkOnlyRequest) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  PricerConfig config;
  config.preset = FitPreset::Hft;
  CurveConfig linear;
  linear.kind = VolCurveKind::LinearVariance;
  config.curve = linear;
  PricerFitter fitter{config};
  const auto fitted = fitter.fit(*chain);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();

  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.market_mark, nullptr);
  EXPECT_EQ(bundle.market_mark_health.state, SurfaceState::Healthy);
  EXPECT_EQ(bundle.market_mark->quality_mode(), FitQualityMode::Latency);
  EXPECT_EQ(bundle.risk, nullptr); // never an implicit risk request

  ASSERT_NE(fitter.surface(), nullptr); // mark-only request serves its mark
  EXPECT_EQ(fitter.surface()->purpose(), SurfacePurpose::MarketMark);
  auto priced = fitter.surface()->session().to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  ASSERT_GT(priced->n_slices(), 0u);
  EXPECT_EQ(priced->kind_at(0), VolCurveKind::LinearVariance);
}

// Charter §5.6: a policy-rejected risk request still attempts the requested
// MarketMark build and publishes it on its own contract. The caller keeps the
// InvalidArgument error, the mark is available, and the risk health honestly
// reports the rejection against the new candidate generation.
TEST(SurfaceV2LegacyCompat, InvalidRiskPolicyStillPublishesTheRequestedMark) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  PricerConfig config = config_for(FitQualityMode::Balanced);
  config.enforce_calendar_floor = false; // disabling a mandatory risk gate
  PricerFitter fitter{config};
  const auto fitted = fitter.fit(*chain);
  ASSERT_FALSE(fitted.has_value());
  EXPECT_EQ(fitted.error().code(), ErrorCode::InvalidArgument);

  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.market_mark, nullptr);
  EXPECT_EQ(bundle.market_mark_health.state, SurfaceState::Healthy);
  EXPECT_TRUE(bundle.market_mark_health.serving_candidate());
  EXPECT_EQ(bundle.risk, nullptr);
  EXPECT_EQ(bundle.risk_health.state, SurfaceState::Rejected);
  EXPECT_EQ(bundle.risk_health.candidate_generation, bundle.candidate_generation);
  ASSERT_TRUE(fitter.published_provenance(SurfacePurpose::MarketMark).has_value());
  EXPECT_EQ(fitter.published_provenance(SurfacePurpose::MarketMark)->board_revision,
            chain->quote_revision());
  EXPECT_FALSE(fitter.published_provenance(SurfacePurpose::Risk).has_value());
  EXPECT_FALSE(fitter.published_provenance().has_value());

  EXPECT_TRUE(
      fitter.value_chain(*chain, OutputField::ModelIV, SurfacePurpose::MarketMark).has_value());
  EXPECT_FALSE(fitter.value_chain(*chain, OutputField::ModelIV).has_value());
}

// The legacy risk presets keep their §9 work budgets through the shared
// mapping table (map_legacy_fit_preset): Fast => Latency, Accurate => Accuracy.
//
// MERGE (routing call): the preset supplies the BUDGET of a v2 request, it does
// not CREATE one. A bare `PricerConfig{.preset = Fast}` is the legacy
// single-surface mark request (da718f7/WP12: strict risk is opted into, never
// inherited — corpus/populate/dispersion all arrive with exactly that shape and
// must keep serving their mark). So name the dual output explicitly and leave
// `quality_mode` unnamed: that is precisely the path this test exists to pin —
// the unnamed field resolving through map_legacy_fit_preset.
TEST(SurfaceV2LegacyCompat, LegacyRiskPresetsRouteThroughTheSharedMappingTable) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  const auto dual_with_preset = [](FitPreset preset) {
    PricerConfig cfg;
    cfg.preset = preset;
    cfg.outputs = SurfaceOutputs::MarketMarkAndRisk; // v2 request; quality unnamed
    return cfg;
  };
  {
    PricerFitter fitter{dual_with_preset(FitPreset::Fast)};
    ASSERT_TRUE(fitter.fit(*chain).has_value());
    ASSERT_NE(fitter.bundle().risk, nullptr);
    EXPECT_EQ(fitter.bundle().risk->quality_mode(), FitQualityMode::Latency);
  }
  {
    PricerFitter fitter{dual_with_preset(FitPreset::Accurate)};
    ASSERT_TRUE(fitter.fit(*chain).has_value());
    ASSERT_NE(fitter.bundle().risk, nullptr);
    EXPECT_EQ(fitter.bundle().risk->quality_mode(), FitQualityMode::Accuracy);
  }
  // And the legacy shape itself: no v2 field named => one mark-grade surface,
  // no risk build, no fail-closed serving.
  PricerFitter legacy{PricerConfig{.preset = FitPreset::Fast}};
  ASSERT_TRUE(legacy.fit(*chain).has_value());
  EXPECT_EQ(legacy.bundle().risk, nullptr);
  ASSERT_NE(legacy.surface(), nullptr);
  EXPECT_EQ(legacy.surface()->purpose(), SurfacePurpose::MarketMark);
  EXPECT_EQ(legacy.surface()->quality_mode(), FitQualityMode::Latency);
}

// An adversarial board carrying a GENUINE calendar arbitrage that the served
// ConvexDense dense model structurally cannot repair — so it drops an expiry and
// fails independent admission, while the parametric SVI fallback repairs the arb
// and is admitted. This forces the validation-rejection ladder on a DISCRETE,
// drift-proof failure (an expiry either fits or it does not), NOT on a geometric
// convexity/calendar margin: a validly-built ConvexDense on smooth truth is
// arb-free by construction (QP + calendar floor + Roger-Lee clamp), so any
// convexity/butterfly/calendar violation it leaks is a sub-1e-8 seam artifact in
// the near-zero-density deep wing that flips on any ~1e-6 pricing nudge — the
// exact fragility A1 (BAW sign fix) and P2 (warm-start carry) kept re-flipping.
//
// Mechanism. Truth total variance theta = sigma0^2 * T is NON-monotone across
// tenor: the 3m slice (sigma0=0.28) carries ~4x the ATM variance of the 4m slice
// above it (sigma0=0.12) — a real 3m>4m calendar inversion. ConvexDense's
// node-wise QP calendar floor (fit_slice_curve) must lift the ENTIRE 4m slice
// >=4x onto the 3m to serve it arb-free; that lift is QP-infeasible against the
// 4m's own convex price shape, so the 4m slice fit fails and the expiry is
// dropped. Strict risk admission (min_expiry_coverage=1.0) then rejects the
// 2-of-3 surface with SurfaceAdmissionReason::InsufficientExpiryCoverage. SVI
// fits each slice parametrically and repairs the calendar (arb_project_calendar_
// svi_pair), serving all three expiries -> admitted Healthy.
//
// MARGIN / ROBUSTNESS (round-2 budget). The failure is a DISCRETE expiry count:
// ConvexDense serves fitted_expiries=2, the floor requires 3 (one WHOLE expiry
// short) — not a numeric margin. Verified deterministic 5/5 and stable across the
// +-14%..+-18% strike band and >=0.01-vol parameter jitter; the drop/no-drop
// boundary sits ~0.01 in vol (~1% in price) = ~10^4 x the ~1e-6 pricing-drift
// scale that flipped the old convexity gate. Served SVI quote-coverage is 0.60 vs
// the 0.50 floor (structural deep-wing drops, stable under drift). If a future
// de-Am/fit change disarms the drop, the explicit reason assertion below fails
// LOUDLY rather than silently re-greening.
[[nodiscard]] std::optional<OptionChain> make_calendar_arb_dense_reject_chain() {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  spec.expiries.clear();
  struct Row {
    const char *iso;
    double sigma0;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-07-17", 0.10, -0.40, 0.10}, // ~1m calm base
      {"2026-09-18", 0.28, -0.25, 0.08}, // ~3m variance PEAK (calendar-arb source)
      {"2026-10-16", 0.12, -0.25, 0.08}, // ~4m low-variance slice BELOW the 3m
  };
  for (const Row &row : rows) {
    SynthExpiry expiry;
    expiry.expiry_iso = row.iso;
    expiry.T = year_fraction(spec.snapshot_iso, row.iso);
    const double s2 = 2.0 * std::sqrt(expiry.T) * row.skew_k;
    expiry.truth = S3Params{row.sigma0, s2, row.c2};
    spec.expiries.push_back(expiry);
  }
  // +-16% strike ladder (504..694 on a 600 spot): wide enough that the dense QP
  // sees the full arb and drops the 4m; the drop is stable across +-14%..+-18%.
  spec.strikes.clear();
  for (double K = 504.0; K <= 696.0 + 1e-9; K += 5.0)
    spec.strikes.push_back(K);
  auto panel = make_synthetic_american_panel(spec);
  if (!panel)
    return std::nullopt;
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  if (!chain)
    return std::nullopt;
  return std::optional<OptionChain>{std::move(*chain)};
}

// A deterministic board whose auto-routed PRIMARY is rejected by independent
// risk admission while a fallback rung is admitted — the validation-rejection
// ladder's adoption seam. Mirrors corpus_test's sparse AAPL event board: eleven
// near-money strikes on a 200 spot with a hot (0.42 ATM), steeply-skewed smile
// and a MegaCapEvent pre-announcement context routing the C8 event family as
// the primary; C8's extra curvature freedom on this sparse ladder is exactly
// what independent risk admission rejects, and the parsimonious eSSVI backbone
// rung serves the same board admitted (the corpus qualification pipeline pins
// the same rejected-C8 -> admitted-eSSVI transition on this board).
[[nodiscard]] std::optional<OptionChain> make_event_c8_reject_chain() {
  SynthPanelSpec spec;
  spec.uid = "AAPL";
  spec.snapshot_iso = "2026-06-15";
  spec.spot = 200.0;
  spec.r = 0.043;
  for (const std::string_view expiry_iso :
       {"2026-07-17", "2026-08-21", "2026-09-18", "2026-10-16", "2026-11-20", "2026-12-18"}) {
    const double T = year_fraction(spec.snapshot_iso, expiry_iso);
    spec.expiries.push_back(
        SynthExpiry{std::string(expiry_iso), T, S3Params{0.42, -0.9 * std::sqrt(T), 0.8}});
  }
  spec.strikes = {160.0, 168.0, 176.0, 184.0, 192.0, 200.0,
                  208.0, 216.0, 224.0, 232.0, 240.0};
  spec.half_spread_frac = 0.03;
  spec.min_half_spread = 0.05;
  auto panel = make_synthetic_american_panel(spec);
  if (!panel)
    return std::nullopt;
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  if (!chain)
    return std::nullopt;
  return std::optional<OptionChain>{std::move(*chain)};
}

// Review finding I6: when the validation-rejection ladder admits a different
// family, the served provenance must name that family — used_fallback set,
// primary_curve preserving the rejected policy choice, and decision()->curve
// equal to the model actually served.
//
// FIXTURE HISTORY (T7a). This test originally drove the ladder with
// make_calendar_arb_dense_reject_chain: a ConvexDense primary dropping the
// arb-conflicting expiry (2/3, InsufficientExpiryCoverage) with a parametric SVI
// fallback expected to REPAIR the ~5x calendar inversion and serve 3/3. The
// calendar-repair fidelity budget (arb.hpp) has since refused exactly that
// fabrication: SVI now honestly drops the same expiry (2/3) and eSSVI refuses
// to build ("needs a cumulative ATM level scale of 4.998, beyond the fidelity
// budget 1.100"), so NO family can serve that board at min_expiry_coverage=1.0
// and the fit fails loudly — the correct outcome, asserted by
// UnservableCalendarArbBoardFailsLoudWithFullAttemptTrail below. The ladder
// provenance contract this test guards is instead exercised on the sparse
// event board above, whose C8 primary is rejected and whose eSSVI rung is
// admitted — asserted through the v2 dual mark/risk request down to the
// persisted priced-surface family.
TEST(SurfaceV2Provenance, ValidationFallbackAdmissionRecordsTheServedFamily) {
  auto chain = make_event_c8_reject_chain();
  ASSERT_TRUE(chain.has_value());
  PricerConfig config = config_for(FitQualityMode::Balanced);
  // Opt into the strict risk-serving admission contract (the mark-grade default
  // admits the primary directly and would never fall back).
  config.admission = atx::vol::risk_admission_policy();
  // The event routing context that makes C8 the primary (corpus_test's AAPL
  // event cell), pinned rather than inferred so the route is deterministic.
  config.context.profile_override = atx::vol::ProfileKind::MegaCapEvent;
  config.context.event_phase = atx::vol::EventPhase::PreAnnouncement;
  config.context.event_distance_days = 2u;
  // Deterministic direct route: no out-of-sample cross-validation pass.
  config.policy.sparse_validation_floor = 0;
  PricerFitter fitter{config};
  const auto fitted = fitter.fit(*chain);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();

  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.risk, nullptr);
  EXPECT_TRUE(bundle.risk_health.validation.admitted());

  ASSERT_TRUE(fitter.decision().has_value());
  const FitDecision &decision = *fitter.decision();
  EXPECT_TRUE(decision.used_fallback);
  EXPECT_EQ(decision.primary_curve.kind, VolCurveKind::C8);
  EXPECT_NE(decision.curve.kind, VolCurveKind::C8);

  // The fallback fired because the primary GENUINELY failed independent
  // admission (not because the fallback was preferred): the primary attempt is
  // recorded, C8, and rejected, and the served attempt is a different family
  // that passed the complete admission contract.
  ASSERT_TRUE(fitter.published_report().has_value());
  ASSERT_GT(fitter.published_report()->attempts.size(), 1u);
  const auto &primary_attempt = fitter.published_report()->attempts.front();
  EXPECT_EQ(primary_attempt.curve.kind, VolCurveKind::C8);
  EXPECT_FALSE(primary_attempt.admission.admitted);
  const auto &served_attempt = fitter.published_report()->attempts.back();
  EXPECT_TRUE(served_attempt.admission.admitted);
  EXPECT_EQ(served_attempt.curve.kind, decision.curve.kind);

  // Persisted provenance must match the served model family.
  auto priced = bundle.risk->session().to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  ASSERT_GT(priced->n_slices(), 0u);
  EXPECT_EQ(priced->kind_at(0), decision.curve.kind);
}

// T7a Stage 1: adopting a substitute after independent admission rejected the
// BUILT primary must publish the two candidates' comparison on a COMMON quote
// population (FallbackComparisonRecord) — the seam previously served the
// substitute with no comparison ever made, while downstream admission scores
// the gameable own-support min (worst_frac_within_bidask). Instrumentation
// only: the record rides the build report; the same fixture's serving
// assertions live in ValidationFallbackAdmissionRecordsTheServedFamily above.
TEST(SurfaceV2Provenance, ValidationFallbackAdoptionPublishesCommonSupportComparison) {
  auto chain = make_event_c8_reject_chain();
  ASSERT_TRUE(chain.has_value());
  // Make every term of the population screen load-bearing: cross one deep-wing
  // quote (bid > ask) and one-side another (bid == 0). Both must fall OUT of
  // the common-support population — the independent recomputation below drops
  // them, so a production screen that silently admits either drifts the counts
  // and fails the population pin.
  {
    const std::vector<OptionId> ids = chain->ids();
    ASSERT_GE(ids.size(), 2u);
    const std::array<OptionId, 2> touched{ids.front(), ids.back()};
    const std::array<double, 2> bids{2.0, 0.0};
    const std::array<double, 2> asks{1.0, 0.05};
    ASSERT_TRUE(chain
                    ->update_quotes(std::span<const OptionId>(touched),
                                    std::span<const double>(bids), std::span<const double>(asks))
                    .has_value());
  }
  PricerConfig config = config_for(FitQualityMode::Balanced);
  config.admission = atx::vol::risk_admission_policy();
  config.context.profile_override = atx::vol::ProfileKind::MegaCapEvent;
  config.context.event_phase = atx::vol::EventPhase::PreAnnouncement;
  config.context.event_distance_days = 2u;
  config.policy.sparse_validation_floor = 0;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value());
  ASSERT_TRUE(fitter.published_report().has_value());
  const auto &report = *fitter.published_report();
  ASSERT_TRUE(report.validation_fallback_comparison.has_value());
  const atx::vol::FallbackComparisonRecord &record = *report.validation_fallback_comparison;

  EXPECT_EQ(record.path, atx::vol::FallbackAdoptionPath::ValidationLadder);
  EXPECT_EQ(record.primary_curve.kind, VolCurveKind::C8);
  ASSERT_TRUE(fitter.decision().has_value());
  EXPECT_EQ(record.substitute_curve.kind, fitter.decision()->curve.kind);
  EXPECT_TRUE(record.substitute_served);
  // The oracle verdict that killed the primary is carried, not defaulted.
  EXPECT_NE(record.primary_failures, ValidationFailure::None);
  EXPECT_FALSE(record.primary_admission.admitted);

  // The record scored the primary's support: one slice row per fitted primary
  // expiry, each row's own-support obs matching the primary attempt's census.
  const auto &primary_attempt = report.attempts.front();
  ASSERT_EQ(record.slices.size(), primary_attempt.evidence.fitted_expiries);
  ASSERT_GT(record.slices.size(), 0u);

  // Population pin: on each primary-fitted slice the common support is EXACTLY
  // the chain's well-formed two-sided quotes (finite strike/bid/ask, bid > 0,
  // ask > 0, uncrossed), recomputed here independently from the installed
  // underlying. Both candidates are scored on that same denominator.
  const atx::vol::Underlying &under = chain->underlying();
  std::size_t total_scored = 0u;
  std::size_t total_primary_within = 0u;
  std::size_t total_substitute_within = 0u;
  double worst_primary = 1.0;
  double worst_substitute = 1.0;
  for (const atx::vol::FallbackComparisonSlice &slice : record.slices) {
    const auto matching =
        std::find_if(under.chains.begin(), under.chains.end(),
                     [&](const atx::vol::Chain &expiry) { return expiry.T == slice.maturity; });
    ASSERT_NE(matching, under.chains.end());
    std::size_t expected = 0u;
    for (std::size_t strike_idx = 0u; strike_idx < matching->n_strikes(); ++strike_idx) {
      if (!std::isfinite(matching->strikes[strike_idx]) || !(matching->strikes[strike_idx] > 0.0))
        continue;
      for (const Side side : {Side::Call, Side::Put}) {
        const std::size_t quote_idx =
            atx::vol::chain_index(static_cast<std::uint16_t>(strike_idx), side);
        const double bid = matching->bids[quote_idx];
        const double ask = matching->asks[quote_idx];
        if (std::isfinite(bid) && std::isfinite(ask) && bid > 0.0 && ask > 0.0 && ask >= bid)
          ++expected;
      }
    }
    EXPECT_EQ(slice.n_scored, expected) << "maturity " << slice.maturity;
    ASSERT_GT(slice.n_scored, 0u);
    EXPECT_LE(slice.primary_within, slice.n_scored);
    EXPECT_LE(slice.substitute_within, slice.n_scored);
    EXPECT_GT(slice.primary_obs, 0u);
    total_scored += slice.n_scored;
    total_primary_within += slice.primary_within;
    total_substitute_within += slice.substitute_within;
    const double dn = static_cast<double>(slice.n_scored);
    worst_primary = std::min(worst_primary, static_cast<double>(slice.primary_within) / dn);
    worst_substitute =
        std::min(worst_substitute, static_cast<double>(slice.substitute_within) / dn);
  }
  EXPECT_EQ(record.n_scored, total_scored);
  EXPECT_EQ(record.primary_within, total_primary_within);
  EXPECT_EQ(record.substitute_within, total_substitute_within);
  EXPECT_DOUBLE_EQ(record.primary_worst_slice_frac, worst_primary);
  EXPECT_DOUBLE_EQ(record.substitute_worst_slice_frac, worst_substitute);
  EXPECT_GT(record.primary_obs_total, 0u);
  EXPECT_GT(record.substitute_obs_total, 0u);
  // T7a stage 3, no-false-positive direction: on THIS board the adopted eSSVI
  // substitute genuinely out-serves the rejected C8 primary on common support
  // (measured 108/108 vs 86/108), so the demote-on-loss rule must NOT fire —
  // no SubstituteUnderserve reason, and the served state stays whatever the
  // substitute's own digest earned. (The fires-when-worse direction is pinned
  // by SubstituteUnderservePredicateBoundaries + RiskSurfaceAdmission.
  // SubstituteUnderserveOnlyPublishesDegradedWithReasonRetained, and end to
  // end by the pre-registered corpus control: 143/179 production substitutes
  // gain exactly this reason bit and nothing else changes.)
  EXPECT_GT(record.substitute_within, record.primary_within);
  EXPECT_FALSE(atx::vol::substitute_underserves(record));
  EXPECT_FALSE(has_validation_failure(fitter.bundle().risk_health.reasons,
                                      ValidationFailure::SubstituteUnderserve));
}

// T7a stage 3: the demote-on-loss trigger, at its boundaries. Fail-open on an
// unmeasured comparison (W3-A), inactive on ties, active only on a strict
// in-band deficit.
TEST(SurfaceV2Provenance, SubstituteUnderservePredicateBoundaries) {
  atx::vol::FallbackComparisonRecord record;
  EXPECT_FALSE(atx::vol::substitute_underserves(record)); // measured nothing
  record.n_scored = 10u;
  record.primary_within = 5u;
  record.substitute_within = 5u;
  EXPECT_FALSE(atx::vol::substitute_underserves(record)); // tie is not a loss
  record.substitute_within = 6u;
  EXPECT_FALSE(atx::vol::substitute_underserves(record)); // substitute better
  record.substitute_within = 4u;
  EXPECT_TRUE(atx::vol::substitute_underserves(record)); // strict deficit
  record.n_scored = 0u;
  EXPECT_FALSE(atx::vol::substitute_underserves(record)); // W3-A guard wins
}

// W3-A: a fit that never substituted must publish NO comparison record —
// absence is structural, never a zeroed record masquerading as measurement.
TEST(SurfaceV2Provenance, NoSubstitutionPublishesNoComparisonRecord) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  PricerFitter fitter{config_for(FitQualityMode::Balanced)};
  ASSERT_TRUE(fitter.fit(*chain).has_value());
  ASSERT_TRUE(fitter.published_report().has_value());
  EXPECT_FALSE(fitter.published_report()->validation_fallback_comparison.has_value());
  ASSERT_TRUE(fitter.decision().has_value());
  EXPECT_FALSE(fitter.decision()->used_fallback);
}

// T7a companion to the fixture history above: the calendar-arb board whose
// genuine 3m>4m inversion needs a ~5x ATM variance fabrication to "repair" is
// UNSERVABLE under min_expiry_coverage=1.0 now that the fidelity budget refuses
// that fabrication — every family honestly drops or refuses the arb expiry, and
// the fit fails LOUDLY with the complete attempt trail naming each family's own
// verdict. If a future repair/fit change quietly re-serves this board 3/3, this
// test fails and the fabrication question must be re-argued in the open.
TEST(SurfaceV2Provenance, UnservableCalendarArbBoardFailsLoudWithFullAttemptTrail) {
  auto chain = make_calendar_arb_dense_reject_chain();
  ASSERT_TRUE(chain.has_value());
  PricerConfig config = config_for(FitQualityMode::Balanced);
  config.admission = atx::vol::risk_admission_policy();
  // Deterministic direct route: the SPY ticker prior pins the primary to
  // ConvexDense with no out-of-sample cross-validation pass.
  config.policy.sparse_validation_floor = 0;
  PricerFitter fitter{config};
  const auto fitted = fitter.fit(*chain);
  ASSERT_FALSE(fitted.has_value())
      << "the arb-carrying board was served; the fidelity budget's refusal to fabricate "
         "~5x ATM variance has been bypassed";
  EXPECT_NE(fitted.error().to_string().find("InsufficientExpiryCoverage"), std::string::npos)
      << fitted.error().to_string();

  // The complete attempt trail: dense primary drops the arb expiry (2/3), the
  // SVI rung honestly drops the same expiry instead of fabricating the repair,
  // and the eSSVI rung refuses to build against the fidelity budget.
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  const auto &attempts = fitter.last_attempt_report()->attempts;
  ASSERT_EQ(attempts.size(), 3u);
  EXPECT_EQ(attempts[0].curve.kind, VolCurveKind::ConvexDense);
  EXPECT_TRUE(attempts[0].build_succeeded);
  EXPECT_FALSE(attempts[0].admission.admitted);
  EXPECT_TRUE(atx::vol::has_admission_failure(
      attempts[0].admission, atx::vol::SurfaceAdmissionReason::InsufficientExpiryCoverage));
  EXPECT_LT(attempts[0].evidence.fitted_expiries, attempts[0].evidence.attempted_expiries);
  EXPECT_EQ(attempts[1].curve.kind, VolCurveKind::Svi);
  EXPECT_TRUE(attempts[1].build_succeeded);
  EXPECT_FALSE(attempts[1].admission.admitted);
  EXPECT_LT(attempts[1].evidence.fitted_expiries, attempts[1].evidence.attempted_expiries);
  EXPECT_EQ(attempts[2].curve.kind, VolCurveKind::Essvi);
  EXPECT_FALSE(attempts[2].build_succeeded);
  ASSERT_TRUE(attempts[2].failure.has_value());
  EXPECT_NE(attempts[2].failure->to_string().find("fidelity budget"), std::string::npos)
      << attempts[2].failure->to_string();

  // Nothing was published: a fresh fitter has no last-known-good to retain.
  EXPECT_FALSE(fitter.published_report().has_value());
  EXPECT_EQ(fitter.bundle().risk, nullptr);
}

// ── T1: the market mark is measured, and says what it measured ───────────────
//
// The mark arm pins LinearVariance -- raw quote interpolation in total variance
// -- and published `Healthy` with a DEFAULT-CONSTRUCTED ValidationDigest: every
// count zero and `admitted()` true, for a surface no oracle had ever looked at.
// The digest is now the mark's own butterfly tally, and the COUNTS are what
// changed. `n_slices` counts the slices the audit MEASURED, so `n_slices > 0`
// proves it ran rather than defaulting.
//
// `admitted()` is still TRUE, and this test pins that deliberately. The archive
// refuses a Healthy record carrying any failure bit (`provenance_record_valid`,
// src/storage/surface_archive.cpp:93), so a Healthy mark with a Butterfly bit is
// not a representable persisted state -- making `admitted()` false here would
// make the mark unpersistable, not honest. The geometry lives in the counts.
//
// Measured at this SHA on the repo's known-truth synthetic SPY panel: 6 served
// slices, 11 butterfly violations. The worst kink slope drop (0.20) is pinned in
// curve_selector_test.cpp against the tally directly, where its units -- a
// total-variance slope in log-moneyness -- are unambiguous; it is deliberately
// NOT published into `max_butterfly_slack`, which means a price slope in strike
// space (see MarkButterflyAudit in src/fitting/pricer_fitter.cpp).
TEST(MarkButterflyHonesty, MarkDigestCarriesItsOwnButterflyTally) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());

  PricerConfig config;
  config.quality_mode = FitQualityMode::Latency;
  config.outputs = SurfaceOutputs::MarketMark;
  config.risk_admission = RiskAdmission::NotApplicable;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value());

  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.market_mark, nullptr);
  const auto &digest = bundle.market_mark_health.validation;
  // The audit ran over the served slices -- an assertion a defaulted,
  // never-populated digest cannot satisfy. Pinned to the measured value, not
  // `> 0`: every slice of this mark IS LinearVariance, so measured == served,
  // and a regression that started skipping slices would still pass `> 0`.
  EXPECT_EQ(digest.n_slices, 6u);
  // Every LinearVariance slice splices a flat wing onto a sloped segment, so the
  // family is structurally never butterfly-arb-free and the tally says so.
  EXPECT_EQ(digest.n_butterfly_violations, 11u);
  // Zero BY DESIGN, not unpopulated: these three fields mean a price slope in
  // strike space, and this audit measures a total-variance slope in
  // log-moneyness. Publishing the latter under the former's name would put two
  // incommensurable quantities in one field -- `format_digest` prints the mark's
  // and the risk arm's side by side.
  EXPECT_EQ(digest.max_butterfly_slack, 0.0);
  EXPECT_EQ(digest.first_butterfly_slope_left, 0.0);
  EXPECT_EQ(digest.first_butterfly_slope_right, 0.0);
  // `first_butterfly_k` IS shared honestly -- log-moneyness in both writers.
  EXPECT_TRUE(std::isfinite(digest.first_butterfly_k));
  EXPECT_LT(digest.first_butterfly_slice, digest.n_slices);
  // `failures` is the admission VERDICT, not the measurement, and stays None
  // while the state is Healthy -- the archive record refuses any other pairing
  // (`provenance_record_valid`, src/storage/surface_archive.cpp). The honest
  // reading is the COUNT, which no longer defaults to zero for an unlooked-at
  // surface; the top-level verdict moves under `demote_mark_on_butterfly`.
  EXPECT_EQ(digest.failures, ValidationFailure::None);
  // The mark is still SERVED, and by default its state is unchanged -- gating or
  // demoting it by default would stop serving SPY/QQQ/IWM (and would arm the
  // `state == Healthy` mark-substitution gate in corpus_board_fit.cpp).
  EXPECT_EQ(bundle.market_mark_health.state, SurfaceState::Healthy);
  EXPECT_EQ(bundle.market_mark_health.reasons, ValidationFailure::None);

  // T6, pinned here because this is the one place a freshly published health is
  // in hand: `SurfaceHealth::surface_age_ns` is DECLARED AND NEVER ASSIGNED by
  // any first-party code, so it reads 0 -- "built just now" -- whatever the
  // surface's actual age. A caller must not mistake that silence for a
  // measurement; freshness lives in the generation counters and SurfaceState.
  EXPECT_EQ(bundle.market_mark_health.surface_age_ns, 0);
}

// The honest state a caller can ASK for: with `demote_mark_on_butterfly` the
// same board publishes Degraded + Butterfly. Still served -- refusing the mark
// would publish nothing for the most liquid names -- but no longer claiming to
// be verified clean at the top level either.
TEST(MarkButterflyHonesty, PolicyCanDemoteAButterflyCarryingMarkToDegraded) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());

  PricerConfig config;
  config.quality_mode = FitQualityMode::Latency;
  config.outputs = SurfaceOutputs::MarketMark;
  config.risk_admission = RiskAdmission::NotApplicable;
  config.demote_mark_on_butterfly = true;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value()) << "the mark must still SERVE, only not claim clean";

  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.market_mark, nullptr);
  EXPECT_EQ(bundle.market_mark_health.state, SurfaceState::Degraded);
  EXPECT_TRUE(
      has_validation_failure(bundle.market_mark_health.reasons, ValidationFailure::Butterfly));
  EXPECT_TRUE(bundle.market_mark_health.serving_candidate());
  EXPECT_GT(bundle.market_mark_health.validation.n_butterfly_violations, 0u);
  // Demoted state and digest verdict move TOGETHER -- the pairing the archive
  // record enforces at write time.
  EXPECT_TRUE(has_validation_failure(bundle.market_mark_health.validation.failures,
                                     ValidationFailure::Butterfly));
  EXPECT_FALSE(bundle.market_mark_health.validation.admitted());
}

} // namespace
