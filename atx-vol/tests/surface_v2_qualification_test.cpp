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
#include "atx/vol/spy_fixture.hpp"
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
using atx::vol::has_validation_failure;
using atx::vol::make_spy_synthetic_spec;
using atx::vol::make_synthetic_american_panel;
using atx::vol::year_fraction;

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

  const SessionInputs& inputs = bundle.risk->session().inputs();
  const std::size_t expected_n_atm = mode == FitQualityMode::Latency    ? 3u
                                     : mode == FitQualityMode::Balanced ? 8u
                                                                        : 12u;
  const std::size_t expected_max_pairs =
      mode == FitQualityMode::Latency ? 6u : 12u;
  EXPECT_EQ(inputs.deam.n_atm, expected_n_atm);
  EXPECT_EQ(inputs.deam.max_borrow_pairs, expected_max_pairs);
  ASSERT_TRUE(inputs.deam.al_opts.has_value());
  const AlOpts accurate = al_default_opts();
  EXPECT_EQ(inputs.deam.al_opts->n_collocation, accurate.n_collocation);
  EXPECT_EQ(inputs.deam.al_opts->n_quadrature, accurate.n_quadrature);
  EXPECT_EQ(inputs.deam.al_opts->max_newton_iter, accurate.max_newton_iter);
  EXPECT_DOUBLE_EQ(inputs.deam.al_opts->tol, accurate.tol);
  const double expected_iv_tol =
      mode == FitQualityMode::Latency ? 1.0e-5 : 1.0e-7;
  EXPECT_DOUBLE_EQ(inputs.deam.iv_tol, expected_iv_tol);
  EXPECT_TRUE(inputs.deam.require_carry_confidence);

  // And the carry strip actually resolved multi-pair at (or above) the
  // admission gate's confidence floor.
  const auto& diagnostics = bundle.risk->diagnostics();
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
                  ->update_quotes(std::span<const OptionId>(ids),
                                  std::span<const double>(bids),
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
void strip_put_bids_to_two_pairs(OptionChain& chain) {
  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : chain.ids()) {
    const auto option = chain.at(id);
    ASSERT_TRUE(option.has_value());
    if (option->side != Side::Put) continue;
    if (option->strike == 595.0 || option->strike == 605.0) continue;
    ids.push_back(id);
    bids.push_back(0.0);  // one-sided quote: kills the co-terminal carry pair
    asks.push_back(option->ask);
  }
  ASSERT_FALSE(ids.empty());
  ASSERT_TRUE(chain
                  .update_quotes(std::span<const OptionId>(ids),
                                 std::span<const double>(bids),
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

  PricerConfig config = config_for(FitQualityMode::Balanced);
  config.fallback = SurfaceFallback::None;
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  config.curve = essvi;  // pinned: one deterministic risk build, no ladder
  PricerFitter fitter{config};
  EXPECT_FALSE(fitter.fit(*chain).has_value());

  const auto bundle = fitter.bundle();
  ASSERT_NE(bundle.market_mark, nullptr);
  EXPECT_EQ(bundle.market_mark_health.state, SurfaceState::Healthy);
  EXPECT_EQ(bundle.risk, nullptr);
  EXPECT_EQ(bundle.risk_health.state, SurfaceState::Rejected);

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
  config.outputs = SurfaceOutputs::MarketMark;  // explicit mark-only request
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
  EXPECT_EQ(bundle.risk, nullptr);  // never an implicit risk request

  ASSERT_NE(fitter.surface(), nullptr);  // mark-only request serves its mark
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
  config.enforce_calendar_floor = false;  // disabling a mandatory risk gate
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

  EXPECT_TRUE(
      fitter.value_chain(*chain, OutputField::ModelIV, SurfacePurpose::MarketMark).has_value());
  EXPECT_FALSE(fitter.value_chain(*chain, OutputField::ModelIV).has_value());
}

// The legacy risk presets keep their §9 work budgets through the shared
// mapping table (map_legacy_fit_preset): Fast => Latency, Accurate => Accuracy.
TEST(SurfaceV2LegacyCompat, LegacyRiskPresetsRouteThroughTheSharedMappingTable) {
  auto chain = make_known_truth_chain();
  ASSERT_TRUE(chain.has_value());
  {
    PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast}};
    ASSERT_TRUE(fitter.fit(*chain).has_value());
    ASSERT_NE(fitter.bundle().risk, nullptr);
    EXPECT_EQ(fitter.bundle().risk->quality_mode(), FitQualityMode::Latency);
  }
  {
    PricerFitter fitter{PricerConfig{.preset = FitPreset::Accurate}};
    ASSERT_TRUE(fitter.fit(*chain).has_value());
    ASSERT_NE(fitter.bundle().risk, nullptr);
    EXPECT_EQ(fitter.bundle().risk->quality_mode(), FitQualityMode::Accuracy);
  }
}

// A board whose quoted ±10% strikes are calendar-clean everywhere, but whose 3m
// slice carries a much steeper put wing than the flat 4m smile above it: the
// served ConvexDense slices extend total variance linearly beyond the last
// quoted strike, so the term structure crosses INSIDE the ±0.5 validation band
// while the fit's own shared-k calendar lattice (NaN beyond the inversion
// boundary) cannot see it. The primary therefore builds, fails independent
// admission, and a parametric fallback rung is admitted in its place.
[[nodiscard]] std::optional<OptionChain> make_extension_crossed_chain() {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  spec.expiries.clear();
  struct Row {
    const char* iso;
    double sigma0;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-07-17", 0.10, -0.40, 0.10},  // ~1m calm base
      {"2026-09-18", 0.16, -0.55, 0.15},  // ~3m steep put wing
      {"2026-10-16", 0.19, -0.03, 0.02},  // ~4m flat smile hugging the 3m wing
  };
  for (const Row& row : rows) {
    SynthExpiry expiry;
    expiry.expiry_iso = row.iso;
    expiry.T = year_fraction(spec.snapshot_iso, row.iso);
    const double s2 = 2.0 * std::sqrt(expiry.T) * row.skew_k;
    expiry.truth = S3Params{row.sigma0, s2, row.c2};
    spec.expiries.push_back(expiry);
  }
  auto panel = make_synthetic_american_panel(spec);
  if (!panel) return std::nullopt;
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  if (!chain) return std::nullopt;
  return std::optional<OptionChain>{std::move(*chain)};
}

// Review finding I6: when the validation-rejection ladder admits a different
// family, the served provenance must name that family — used_fallback set,
// primary_curve preserving the rejected policy choice, and decision()->curve
// equal to the model actually served.
TEST(SurfaceV2Provenance, ValidationFallbackAdmissionRecordsTheServedFamily) {
  auto chain = make_extension_crossed_chain();
  ASSERT_TRUE(chain.has_value());
  PricerConfig config = config_for(FitQualityMode::Balanced);
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
  const FitDecision& decision = *fitter.decision();
  EXPECT_TRUE(decision.used_fallback);
  EXPECT_EQ(decision.primary_curve.kind, VolCurveKind::ConvexDense);
  EXPECT_NE(decision.curve.kind, VolCurveKind::ConvexDense);

  // Persisted provenance must match the served model family.
  auto priced = bundle.risk->session().to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  ASSERT_GT(priced->n_slices(), 0u);
  EXPECT_EQ(priced->kind_at(0), decision.curve.kind);
}

}  // namespace
