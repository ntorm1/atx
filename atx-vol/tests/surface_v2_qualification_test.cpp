#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/american.hpp" // al_default_opts (accurate reference AL preset)
#include "atx/vol/chain.hpp"
#include "atx/vol/data.hpp" // year_fraction
#include "atx/vol/panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/s3.hpp" // S3Params (crossed-wing fixture truth)
#include "atx/vol/session.hpp"
#include "support/spy_fixture.hpp"
#include "atx/vol/surface_policy.hpp"
#include "atx/vol/vol_curve.hpp"

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

// Review finding I6: when the validation-rejection ladder admits a different
// family, the served provenance must name that family — used_fallback set,
// primary_curve preserving the rejected policy choice, and decision()->curve
// equal to the model actually served.
TEST(SurfaceV2Provenance, ValidationFallbackAdmissionRecordsTheServedFamily) {
  auto chain = make_calendar_arb_dense_reject_chain();
  ASSERT_TRUE(chain.has_value());
  PricerConfig config = config_for(FitQualityMode::Balanced);
  // Opt into the strict risk-serving admission contract (min_expiry_coverage=1.0).
  // This is the realistic policy for a risk consumer AND the source of a ROBUST,
  // DISCRETE rejection: the ConvexDense dense model cannot node-wise repair the
  // board's genuine 3m>4m calendar inversion, so it drops the 4m expiry and this
  // gate rejects the 2-of-3 surface (InsufficientExpiryCoverage). The parametric
  // SVI fallback repairs the calendar and serves all three -> admitted. See
  // make_calendar_arb_dense_reject_chain for why a discrete expiry-count failure
  // is used instead of a geometric convexity margin (the latter is a sub-1e-8
  // seam artifact that A1/P2 kept flipping).
  config.admission = atx::vol::risk_admission_policy();
  // Deterministic direct route: the SPY ticker prior pins the primary to the
  // rewritten ConvexDense with no out-of-sample cross-validation pass.
  config.policy.sparse_validation_floor = 0;
  PricerFitter fitter{config};
  const auto fitted = fitter.fit(*chain);
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();

  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.risk, nullptr);
  EXPECT_EQ(bundle.risk_health.state, SurfaceState::Healthy);
  EXPECT_TRUE(bundle.risk_health.validation.admitted());

  ASSERT_TRUE(fitter.decision().has_value());
  const FitDecision &decision = *fitter.decision();
  EXPECT_TRUE(decision.used_fallback);
  EXPECT_EQ(decision.primary_curve.kind, VolCurveKind::ConvexDense);
  EXPECT_NE(decision.curve.kind, VolCurveKind::ConvexDense);

  // The fallback fired because the ConvexDense primary GENUINELY failed
  // independent admission (not because the fallback was preferred). Assert the
  // primary attempt is recorded, ConvexDense, and rejected for the SPECIFIC,
  // drift-proof reason — dropping the arb-conflicting expiry — so a future
  // fit/de-Am change that re-admits the primary fails this test LOUDLY instead of
  // silently re-greening it.
  ASSERT_TRUE(fitter.published_report().has_value());
  ASSERT_GT(fitter.published_report()->attempts.size(), 1u);
  const auto &primary_attempt = fitter.published_report()->attempts.front();
  EXPECT_EQ(primary_attempt.curve.kind, VolCurveKind::ConvexDense);
  EXPECT_FALSE(primary_attempt.admission.admitted);
  EXPECT_TRUE(atx::vol::has_admission_failure(
      primary_attempt.admission, atx::vol::SurfaceAdmissionReason::InsufficientExpiryCoverage));
  // The discrete margin: the dense primary served strictly fewer expiries than the
  // board attempted (one WHOLE expiry short of the 1.0 floor), while the served
  // fallback covers all of them.
  EXPECT_LT(primary_attempt.evidence.fitted_expiries,
            primary_attempt.evidence.attempted_expiries);
  const auto &served_attempt = fitter.published_report()->attempts.back();
  EXPECT_TRUE(served_attempt.admission.admitted);
  EXPECT_EQ(served_attempt.evidence.fitted_expiries, served_attempt.evidence.attempted_expiries);

  // Persisted provenance must match the served model family.
  auto priced = bundle.risk->session().to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  ASSERT_GT(priced->n_slices(), 0u);
  EXPECT_EQ(priced->kind_at(0), decision.curve.kind);
}

} // namespace
