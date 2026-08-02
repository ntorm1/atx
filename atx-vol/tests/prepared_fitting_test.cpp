#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/arb.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/curve_fit.hpp"
#include "atx/vol/essvi_calib.hpp"
#include "atx/vol/detail/prepared_fitting.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_curve.hpp"

namespace {

using atx::vol::black76_price;
using atx::vol::CalibOpts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::ExerciseStyle;
using atx::vol::ObservationKey;
using atx::vol::ObservationRejectionReason;
using atx::vol::PreparedBoard;
using atx::vol::PreparedObservationPolicy;
using atx::vol::PreparedSlice;
using atx::vol::PreparedSliceInputs;
using atx::vol::QuoteFlag;
using atx::vol::Side;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;
using atx::vol::VolCurveKind;

constexpr double kS = 100.0;
constexpr double kF = 100.0;
constexpr double kT = 0.5;
constexpr double kR = 0.03;
constexpr double kDf = 0.9851119396030626;

[[nodiscard]] Chain make_chain() {
  Chain chain;
  chain.uid = 17u;
  chain.expiry_id = 42u;
  chain.T = kT;
  chain.strikes = {80.0, 85.0, 90.0, 95.0, 100.0, 105.0, 110.0, 115.0};
  const std::size_t n_sides = 2u * chain.strikes.size();
  chain.bids.assign(n_sides, 0.0);
  chain.asks.assign(n_sides, 0.0);
  chain.mids.assign(n_sides, 0.0);
  chain.ivs.assign(n_sides, std::numeric_limits<double>::quiet_NaN());
  chain.bid_sizes.assign(n_sides, 1u);
  chain.ask_sizes.assign(n_sides, 1u);
  chain.ts_ns.assign(n_sides, 0u);
  chain.flags.assign(n_sides, 0u);
  for (std::size_t strike_index = 0; strike_index < chain.strikes.size(); ++strike_index) {
    for (std::uint8_t side_value = 0u; side_value < 2u; ++side_value) {
      const Side side = static_cast<Side>(side_value);
      const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(strike_index), side);
      const double mid = black76_price(kF, chain.strikes[strike_index], kT, 0.24, kDf, side);
      chain.mids[quote_index] = mid;
      chain.bids[quote_index] = mid - 0.02;
      chain.asks[quote_index] = mid + 0.02;
    }
  }

  const std::size_t stale = chain_index(1u, Side::Put);
  chain.flags[stale] = static_cast<std::uint8_t>(QuoteFlag::Stale);
  const std::size_t crossed = chain_index(6u, Side::Call);
  chain.bids[crossed] = chain.asks[crossed];
  return chain;
}

// Wide American-priced board (flat vol) for the F1 shared-boundary parity gate.
// Each (strike, side) mid is the exact Andersen-Lake American premium at `sigma`;
// bid/ask straddle it. ~64 strikes gives each OTM side well over the 16-row
// shared-lane floor so the batch actually engages.
[[nodiscard]] Chain make_american_board(const std::vector<double> &strikes, double T, double r,
                                        double q, double sigma) {
  Chain chain;
  chain.uid = 71u;
  chain.expiry_id = 7u;
  chain.T = T;
  chain.strikes = strikes;
  const std::size_t n_sides = 2u * strikes.size();
  chain.bids.assign(n_sides, 0.0);
  chain.asks.assign(n_sides, 0.0);
  chain.mids.assign(n_sides, 0.0);
  chain.ivs.assign(n_sides, std::numeric_limits<double>::quiet_NaN());
  chain.bid_sizes.assign(n_sides, 10u);
  chain.ask_sizes.assign(n_sides, 10u);
  chain.ts_ns.assign(n_sides, 0);
  chain.flags.assign(n_sides, 0u);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    for (const Side side : {Side::Call, Side::Put}) {
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
      const auto price =
          atx::vol::american_price(100.0, strikes[i], T, sigma, r, q, side,
                                   atx::vol::AmericanMethod::AndersenLake, std::nullopt);
      const double mid = price.has_value() ? *price : 1.0;
      const double half = std::fmin(0.002, 0.10 * mid);
      chain.mids[idx] = mid;
      chain.bids[idx] = mid - half;
      chain.asks[idx] = mid + half;
    }
  }
  return chain;
}

[[nodiscard]] PreparedSliceInputs configured_inputs() {
  PreparedSliceInputs inputs;
  inputs.expiry_index = 3u;
  inputs.S = kS;
  inputs.r = kR;
  inputs.F = kF;
  inputs.q_eff = kR;
  inputs.df = kDf;
  inputs.calib = CalibOpts{};
  inputs.policy = PreparedObservationPolicy::Configured;
  return inputs;
}

[[nodiscard]] std::vector<ObservationKey> accepted_keys(const PreparedSlice &slice) {
  std::vector<ObservationKey> keys;
  for (const auto &observation : slice.observations()) {
    if (observation.accepted()) {
      keys.push_back(observation.key);
    }
  }
  return keys;
}

} // namespace

TEST(PreparedFitting, ConfiguredPopulationIsDeterministicAndFamilyAgnostic) {
  const Chain chain = make_chain();
  const auto first = PreparedSlice::create(chain, configured_inputs());
  const auto second = PreparedSlice::create(chain, configured_inputs());
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(second.has_value()) << second.error().to_string();

  const std::vector<ObservationKey> essvi_keys = accepted_keys(*first);
  EXPECT_TRUE(std::is_sorted(essvi_keys.begin(), essvi_keys.end()));
  EXPECT_TRUE(std::equal(first->rejections().begin(), first->rejections().end(),
                         second->rejections().begin(), second->rejections().end()));
  ASSERT_EQ(first->fit_observations().size(), essvi_keys.size());

  atx::vol::FitDiag essvi_diag;
  const auto essvi =
      atx::vol::essvi_fit_slice(first->fit_observations(), kT, kF, CalibOpts{}, &essvi_diag);
  ASSERT_TRUE(essvi.has_value()) << essvi.error().to_string();
  const std::vector<ObservationKey> after_essvi = accepted_keys(*first);

  CurveConfig generic;
  generic.kind = VolCurveKind::LinearVariance;
  const auto generic_fit =
      atx::vol::fit_slice_curve(generic, first->fit_observations(), kF, kT, kDf);
  ASSERT_TRUE(generic_fit.has_value()) << generic_fit.error().to_string();
  const std::vector<ObservationKey> after_generic = accepted_keys(*first);

  EXPECT_EQ(after_essvi, essvi_keys);
  EXPECT_EQ(after_generic, essvi_keys);
  EXPECT_EQ(accepted_keys(*second), essvi_keys);
}

TEST(PreparedFitting, EuropeanChainUsesRawBlack76ObservationsWithoutDeAmericanization) {
  Chain chain = make_chain();
  chain.exercise_style = ExerciseStyle::European;

  const auto prepared = PreparedSlice::create(chain, configured_inputs());
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  ASSERT_FALSE(prepared->fit_observations().empty());
  for (const auto &row : prepared->fit_observations()) {
    EXPECT_NEAR(row.sigma_mkt, 0.24, 1.0e-10);
    EXPECT_NEAR(row.score_sigma_mkt, 0.24, 1.0e-10);
  }
  const auto &audit = prepared->deam_audit();
  EXPECT_EQ(audit.n_deam_rows, prepared->fit_observations().size());
  EXPECT_EQ(audit.n_deam_accepted, audit.n_deam_rows);
  EXPECT_EQ(audit.accurate.n_proposed, audit.n_deam_rows);
  EXPECT_EQ(audit.accurate.n_audited, audit.n_deam_rows);
  EXPECT_EQ(audit.accurate.n_reference_reprices, audit.n_deam_rows);
  EXPECT_EQ(audit.accurate.n_accepted, audit.n_deam_rows);
  EXPECT_EQ(audit.n_rejected_residual, std::uint32_t{0});
  EXPECT_TRUE(atx::vol::deam_inversion_certified(
      audit, configured_inputs().calib.max_certified_deam_drop_fraction));
  EXPECT_EQ(prepared->provenance().exercise_style, ExerciseStyle::European);
}

TEST(PreparedFitting, LegacyPolicyAlsoBypassesDeAmericanizationForEuropeanChain) {
  Chain chain = make_chain();
  chain.exercise_style = ExerciseStyle::European;
  PreparedSliceInputs inputs = configured_inputs();
  inputs.policy = PreparedObservationPolicy::LegacyEssviCompatibility;

  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  ASSERT_FALSE(prepared->fit_observations().empty());
  for (const auto &row : prepared->fit_observations()) {
    EXPECT_NEAR(row.sigma_mkt, 0.24, 1.0e-10);
  }
  const auto &audit = prepared->deam_audit();
  EXPECT_EQ(audit.n_deam_rows, prepared->fit_observations().size());
  EXPECT_EQ(audit.n_deam_accepted, audit.n_deam_rows);
  EXPECT_TRUE(
      atx::vol::deam_inversion_certified(audit, inputs.calib.max_certified_deam_drop_fraction));
  EXPECT_EQ(prepared->provenance().policy, PreparedObservationPolicy::LegacyEssviCompatibility);
}

TEST(PreparedFitting, ConfiguredPopulationRetainsStableRejectedKeysAndReasons) {
  const Chain chain = make_chain();
  const auto prepared = PreparedSlice::create(chain, configured_inputs());
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

  const auto &rejections = prepared->rejections();
  ASSERT_EQ(rejections.size(), 2u);
  EXPECT_EQ(rejections[0].key, (ObservationKey{3u, 1u, Side::Put}));
  EXPECT_EQ(rejections[0].reason, ObservationRejectionReason::QuoteFlag);
  EXPECT_EQ(rejections[1].key, (ObservationKey{3u, 6u, Side::Call}));
  EXPECT_EQ(rejections[1].reason, ObservationRejectionReason::InvalidBidAsk);
}

TEST(PreparedFitting, LegacyCompatibilityPreservesHistoricalPermissiveRowsAndWeights) {
  const Chain chain = make_chain();
  PreparedSliceInputs inputs = configured_inputs();
  inputs.policy = PreparedObservationPolicy::LegacyEssviCompatibility;
  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

  // Historical eSSVI ignored quote flags and allowed locked bid==ask rows. The
  // explicit compatibility policy retains both; Configured rejects both.
  ASSERT_EQ(prepared->fit_observations().size(), chain.n_strikes());
  EXPECT_TRUE(prepared->rejections().empty());
  EXPECT_EQ(prepared->provenance().policy, PreparedObservationPolicy::LegacyEssviCompatibility);
  EXPECT_DOUBLE_EQ(prepared->provenance().S, kS);
  EXPECT_DOUBLE_EQ(prepared->provenance().q_eff, kR);

  const atx::vol::FitObs &row = prepared->fit_observations().front();
  const double spread =
      chain.asks[chain_index(0u, Side::Put)] - chain.bids[chain_index(0u, Side::Put)];
  const double vega =
      atx::vol::black76_value_and_vega(kF, chain.strikes.front(), kT, row.sigma_mkt, kDf, Side::Put)
          .vega;
  const double two_sigma_t = 2.0 * row.sigma_mkt * kT;
  const double expected_weight = (vega * vega) / (spread * spread * two_sigma_t * two_sigma_t);
  EXPECT_DOUBLE_EQ(row.weight_w, expected_weight);
  EXPECT_DOUBLE_EQ(row.active_weight_w, expected_weight);
}

// FT-C6 (B5b): the Legacy observation builder set noise_sigma = 0.0 for a
// vega <= 0 row, while the Configured builder (calib.cpp build_one_observation)
// uses 1.0. noise_sigma feeds the C8 spread_w (2*sigma*T*max(noise_sigma,1e-7));
// a 0.0 noise makes that ~1e4x smaller than a normal row -> ~1e8x the LM weight,
// so one dead deep-wing quote can own the objective. The two builders must agree.
// A deep low-vol board floors the de-Am IV so black76 vega underflows to exactly
// 0.0 on the far wings — a deterministic vega==0 row that still survives Legacy's
// permissive admission.
TEST(PreparedFitting, LegacyVegaZeroRowNoiseSigmaMatchesConfiguredFallback) {
  constexpr double T = 0.25, r = 0.01, q = 0.0;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const double q_eff = r - std::log(F / 100.0) / T;
  std::vector<double> strikes;
  for (int i = 0; i < 40; ++i) {
    strikes.push_back(80.0 + 15.0 * static_cast<double>(i)); // out to K=665
  }
  const Chain chain = make_american_board(strikes, T, r, q, 0.05);

  PreparedSliceInputs in;
  in.expiry_index = 3u;
  in.S = 100.0;
  in.r = r;
  in.F = F;
  in.q_eff = q_eff;
  in.df = df;
  in.policy = PreparedObservationPolicy::LegacyEssviCompatibility;
  in.calib = CalibOpts{};
  const auto prep = PreparedSlice::create(chain, in);
  ASSERT_TRUE(prep.has_value()) << prep.error().to_string();

  int n_vega_zero = 0;
  for (const auto &o : prep->fit_observations()) {
    if (o.vega == 0.0) {
      ++n_vega_zero;
      // The unified fallback (matching the Configured builder, calib.cpp:198) is
      // 1.0, NOT 0.0. Pre-fix: noise_sigma == 0.0 (RED).
      EXPECT_DOUBLE_EQ(o.noise_sigma, 1.0)
          << "Legacy vega<=0 noise_sigma must match the Configured builder's 1.0 "
             "fallback (K="
          << o.K << ")";
    }
  }
  ASSERT_GT(n_vega_zero, 0) << "fixture must produce at least one vega==0 row";
}

TEST(PreparedFitting, LegacySharedBoundaryDeamMatchesScalarWithinEconomicBound) {
  // F1 (R-01p2): the Legacy/eSSVI de-Am now routes through the shared exercise-
  // boundary batch (one boundary solve per slice-side across strikes) instead of a
  // per-row scalar american_implied_vol. This is the parity gate: the batch
  // European-equivalent IV must match the exact scalar inverter within the economic
  // bound (1e-4 vol pts), the batch must not drop in-band rows (in-band >= prior),
  // and it must not worsen the eSSVI fit (chi^2 <= prior, economic slack).
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const double q_eff = r - std::log(F / 100.0) / T;

  std::vector<double> strikes;
  strikes.reserve(64u);
  for (std::size_t i = 0; i < 64u; ++i) {
    strikes.push_back(72.0 + 56.0 * static_cast<double>(i) / 63.0);
  }
  const Chain chain = make_american_board(strikes, T, r, q, 0.24);

  PreparedSliceInputs base;
  base.expiry_index = 5u;
  base.S = 100.0;
  base.r = r;
  base.F = F;
  base.q_eff = q_eff;
  base.df = df;
  base.policy = PreparedObservationPolicy::LegacyEssviCompatibility;
  base.calib = CalibOpts{};

  PreparedSliceInputs batch_inputs = base;
  batch_inputs.calib.use_shared_boundary_deam = true; // the F1 batch route
  PreparedSliceInputs scalar_inputs = base;
  scalar_inputs.calib.use_shared_boundary_deam = false; // the exact per-row oracle

  const auto batch = PreparedSlice::create(chain, batch_inputs);
  const auto scalar = PreparedSlice::create(chain, scalar_inputs);
  ASSERT_TRUE(batch.has_value()) << batch.error().to_string();
  ASSERT_TRUE(scalar.has_value()) << scalar.error().to_string();

  // (a) Engagement + direct parity oracle. Drive the exported lane helper over the
  // same OTM candidate legs and require it to certify a real population (a side
  // certifies only at >= 12 accepted lanes), so the end-to-end parity below is not
  // vacuous. Every certified batch IV must match the exact scalar
  // european_equiv_iv within 1e-4 vol pts.
  std::vector<atx::vol::FitObs> seeds;
  seeds.reserve(strikes.size());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const Side side = (strikes[i] >= F) ? Side::Call : Side::Put;
    const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(i), side);
    atx::vol::FitObs seed;
    seed.K = strikes[i];
    seed.side = side;
    seed.mid = chain.mids[quote_index];
    seed.spread = chain.asks[quote_index] - chain.bids[quote_index];
    seeds.push_back(seed);
  }
  const std::size_t certified = atx::vol::shared_boundary_deam_batch(
      seeds, 100.0, r, F, T, df, base.calib, {}, std::nullopt, 1.0e-7, 64,
      atx::vol::AmericanMethod::AndersenLake, nullptr);
  ASSERT_GE(certified, 12u) << "shared-boundary batch must engage on this board";

  double worst_direct = 0.0;
  for (const atx::vol::FitObs &seed : seeds) {
    if (!std::isfinite(seed.score_sigma_mkt)) {
      continue; // uncertified row -> scalar fallback, nothing to compare
    }
    const auto oracle =
        atx::vol::european_equiv_iv(seed.mid, 100.0, seed.K, T, r, q_eff, seed.side);
    ASSERT_TRUE(oracle.has_value()) << oracle.error().to_string();
    worst_direct = std::max(worst_direct, std::fabs(seed.score_sigma_mkt - *oracle));
  }
  EXPECT_LE(worst_direct, 1.0e-4) << "batch vs scalar de-Am IV (direct)";

  // (b) End-to-end wiring: the two Legacy arms select the identical population
  // (selection is independent of the de-Am route), so the fit rows line up 1:1 and
  // differ only in the de-Am IV, which must hold the bound row-for-row.
  ASSERT_EQ(batch->fit_observations().size(), scalar->fit_observations().size());
  ASSERT_GE(batch->fit_observations().size(),
            scalar->fit_observations().size()); // in-band >= prior
  double worst_e2e = 0.0;
  for (std::size_t i = 0; i < batch->fit_observations().size(); ++i) {
    const atx::vol::FitObs &b = batch->fit_observations()[i];
    const atx::vol::FitObs &s = scalar->fit_observations()[i];
    ASSERT_EQ(b.source_strike_index, s.source_strike_index);
    ASSERT_EQ(b.side, s.side);
    worst_e2e = std::max(worst_e2e, std::fabs(b.sigma_mkt - s.sigma_mkt));
    EXPECT_LE(std::fabs(b.sigma_mkt - s.sigma_mkt), 1.0e-4)
        << "K=" << b.K << " side=" << static_cast<int>(b.side);
  }

  // (c) No fit regression: the eSSVI slice fitted on the batch population is no
  // worse than the scalar one beyond an economically-negligible margin.
  atx::vol::FitDiag diag_batch;
  atx::vol::FitDiag diag_scalar;
  const auto fit_batch =
      atx::vol::essvi_fit_slice(batch->fit_observations(), T, F, CalibOpts{}, &diag_batch);
  const auto fit_scalar =
      atx::vol::essvi_fit_slice(scalar->fit_observations(), T, F, CalibOpts{}, &diag_scalar);
  ASSERT_TRUE(fit_batch.has_value()) << fit_batch.error().to_string();
  ASSERT_TRUE(fit_scalar.has_value()) << fit_scalar.error().to_string();
  EXPECT_LE(diag_batch.rmse_vol_vega_weighted, diag_scalar.rmse_vol_vega_weighted + 1.0e-3);
}

TEST(PreparedFitting, SharedExpiryPreparationPreservesPolicySpecificCacheRouting) {
  const Chain chain = make_chain();
  atx::vol::CorrectionCache call_cache;
  atx::vol::CorrectionCache put_cache;
  SurfaceParityInputs inputs;
  inputs.S = kS;
  inputs.r = kR;
  inputs.deam.imply_borrow = false;
  inputs.deam.borrow_fixed = kR;
  inputs.deam.caches = atx::vol::AmericanCorrectionCaches{&call_cache, &put_cache};
  inputs.use_deam_cache_for_fit = false;

  const auto legacy = atx::vol::prepare_expiry(chain, 3u, inputs,
                                               PreparedObservationPolicy::LegacyEssviCompatibility);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  EXPECT_TRUE(legacy->slice.provenance().call_cache);
  EXPECT_TRUE(legacy->slice.provenance().put_cache);

  const auto configured_without_cache =
      atx::vol::prepare_expiry(chain, 3u, inputs, PreparedObservationPolicy::Configured);
  ASSERT_TRUE(configured_without_cache.has_value()) << configured_without_cache.error().to_string();
  EXPECT_FALSE(configured_without_cache->slice.provenance().call_cache);
  EXPECT_FALSE(configured_without_cache->slice.provenance().put_cache);

  inputs.use_deam_cache_for_fit = true;
  const auto configured_with_cache =
      atx::vol::prepare_expiry(chain, 3u, inputs, PreparedObservationPolicy::Configured);
  ASSERT_TRUE(configured_with_cache.has_value()) << configured_with_cache.error().to_string();
  EXPECT_TRUE(configured_with_cache->slice.provenance().call_cache);
  EXPECT_TRUE(configured_with_cache->slice.provenance().put_cache);
}

TEST(PreparedFitting, SharedExpiryPreparationUsesAlignedRateBeforeMaturityFallback) {
  const Chain chain = make_chain();
  SurfaceParityInputs inputs;
  inputs.S = kS;
  inputs.r = kR;
  inputs.expiry_rate_T = {kT, kT};
  inputs.expiry_rates = {0.01, 0.07};
  inputs.deam.imply_borrow = false;
  inputs.deam.borrow_fixed = 0.07;

  const auto aligned =
      atx::vol::prepare_expiry(chain, 1u, inputs, PreparedObservationPolicy::Configured);
  ASSERT_TRUE(aligned.has_value()) << aligned.error().to_string();
  EXPECT_DOUBLE_EQ(aligned->rate, 0.07);
  EXPECT_DOUBLE_EQ(aligned->slice.provenance().r, 0.07);

  // A retained/refit term vector can be shorter than the source expiry index;
  // in that shape the exact maturity fallback remains intentional.
  inputs.expiry_rate_T = {kT};
  inputs.expiry_rates = {0.01};
  inputs.deam.borrow_fixed = 0.01;
  const auto retained =
      atx::vol::prepare_expiry(chain, 9u, inputs, PreparedObservationPolicy::Configured);
  ASSERT_TRUE(retained.has_value()) << retained.error().to_string();
  EXPECT_DOUBLE_EQ(retained->rate, 0.01);
}

TEST(PreparedFitting, SharedExpiryPreparationRejectsThinLegacySliceBeforeFit) {
  Chain chain = make_chain();
  chain.strikes.resize(4u);
  const std::size_t quote_count = 2u * chain.strikes.size();
  chain.bids.resize(quote_count);
  chain.asks.resize(quote_count);
  chain.mids.resize(quote_count);
  chain.ivs.resize(quote_count);
  chain.bid_sizes.resize(quote_count);
  chain.ask_sizes.resize(quote_count);
  chain.ts_ns.resize(quote_count);
  chain.flags.resize(quote_count);

  SurfaceParityInputs inputs;
  inputs.S = kS;
  inputs.r = kR;
  inputs.deam.imply_borrow = false;
  inputs.deam.borrow_fixed = kR;
  const auto prepared = atx::vol::prepare_expiry(
      chain, 3u, inputs, PreparedObservationPolicy::LegacyEssviCompatibility);
  ASSERT_FALSE(prepared.has_value());
  EXPECT_EQ(prepared.error().code(), atx::core::ErrorCode::NotFound);

  Underlying underlying;
  underlying.uid = chain.uid;
  underlying.ticker = "THIN_LEGACY";
  underlying.spot = kS;
  underlying.chains.push_back(std::move(chain));
  const auto surface = atx::vol::run_surface_parity(underlying, inputs);
  ASSERT_FALSE(surface.has_value());
  EXPECT_EQ(surface.error().code(), atx::core::ErrorCode::NotFound);
}

TEST(PreparedFitting, ConfiguredCapAppliesIdenticallyToFitKeysAndScoringRows) {
  const Chain chain = make_chain();
  PreparedSliceInputs inputs = configured_inputs();
  inputs.calib.max_obs_per_slice = 5u;
  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

  const std::vector<ObservationKey> keys = accepted_keys(*prepared);
  ASSERT_EQ(keys.size(), 5u);
  ASSERT_EQ(prepared->fit_observations().size(), keys.size());
  ASSERT_EQ(prepared->score_keys().size(), keys.size());
  ASSERT_EQ(prepared->score_columns().strike.size(), keys.size());
  for (std::size_t index = 0; index < keys.size(); ++index) {
    EXPECT_EQ(prepared->score_keys()[index], keys[index]);
    EXPECT_EQ(prepared->fit_observations()[index].source_strike_index, keys[index].strike_index);
  }
  EXPECT_EQ(std::count_if(prepared->rejections().begin(), prepared->rejections().end(),
                          [](const atx::vol::ObservationRejection &rejection) {
                            return rejection.reason == ObservationRejectionReason::ObservationCap;
                          }),
            1);
}

TEST(PreparedFitting, DuplicateStrikesRetainDistinctSourceKeysThroughCap) {
  Chain chain = make_chain();
  chain.strikes[2] = chain.strikes[1];
  for (std::uint8_t side_value = 0u; side_value < 2u; ++side_value) {
    const Side side = static_cast<Side>(side_value);
    const std::size_t quote_index = chain_index(2u, side);
    const double mid = black76_price(kF, chain.strikes[2], kT, 0.24, kDf, side);
    chain.mids[quote_index] = mid;
    chain.bids[quote_index] = mid - 0.02;
    chain.asks[quote_index] = mid + 0.02;
  }
  chain.flags[chain_index(1u, Side::Put)] = 0u;
  PreparedSliceInputs inputs = configured_inputs();
  inputs.calib.max_obs_per_slice = 5u;
  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

  ASSERT_EQ(prepared->fit_observations().size(), 5u);
  ASSERT_EQ(prepared->score_keys().size(), 5u);
  for (std::size_t index = 0; index < prepared->fit_observations().size(); ++index) {
    EXPECT_EQ(prepared->fit_observations()[index].source_strike_index,
              prepared->score_keys()[index].strike_index);
  }
  const auto first_duplicate =
      std::find_if(prepared->observations().begin(), prepared->observations().end(),
                   [](const auto &row) { return row.key == ObservationKey{3u, 1u, Side::Put}; });
  const auto second_duplicate =
      std::find_if(prepared->observations().begin(), prepared->observations().end(),
                   [](const auto &row) { return row.key == ObservationKey{3u, 2u, Side::Put}; });
  EXPECT_NE(first_duplicate, prepared->observations().end());
  EXPECT_NE(second_duplicate, prepared->observations().end());
}

TEST(PreparedFitting, AllEqualStrikesFillCapByDeterministicSourceKeyFallback) {
  Chain chain = make_chain();
  for (std::size_t strike_index = 0; strike_index < chain.n_strikes(); ++strike_index) {
    chain.strikes[strike_index] = kF;
    for (std::uint8_t side_value = 0u; side_value < 2u; ++side_value) {
      const Side side = static_cast<Side>(side_value);
      const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(strike_index), side);
      const double mid = black76_price(kF, kF, kT, 0.24, kDf, side);
      chain.mids[quote_index] = mid;
      chain.bids[quote_index] = mid - 0.02;
      chain.asks[quote_index] = mid + 0.02;
      chain.flags[quote_index] = 0u;
    }
  }

  PreparedSliceInputs inputs = configured_inputs();
  inputs.calib.max_obs_per_slice = 5u;
  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  ASSERT_EQ(prepared->score_keys().size(), 5u);

  const std::vector<ObservationKey> expected{
      ObservationKey{3u, 0u, Side::Call}, ObservationKey{3u, 1u, Side::Call},
      ObservationKey{3u, 2u, Side::Call}, ObservationKey{3u, 3u, Side::Call},
      ObservationKey{3u, 7u, Side::Call}};
  EXPECT_TRUE(std::equal(prepared->score_keys().begin(), prepared->score_keys().end(),
                         expected.begin(), expected.end()));
}

TEST(PreparedFitting, BidOrAskAnchorScoringMarketIvStillComesFromRawMid) {
  const Chain chain = make_chain();
  for (const auto anchor : {atx::vol::CalibAnchorKind::Bid, atx::vol::CalibAnchorKind::Ask}) {
    PreparedSliceInputs inputs = configured_inputs();
    inputs.calib.anchor_kind = anchor;
    const auto prepared = PreparedSlice::create(chain, inputs);
    ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

    ASSERT_FALSE(prepared->score_keys().empty());
    const ObservationKey key = prepared->score_keys().front();
    const std::size_t quote_index =
        chain_index(static_cast<std::uint16_t>(key.strike_index), key.side);
    const auto expected = atx::vol::european_equiv_iv(
        chain.mids[quote_index], kS, chain.strikes[key.strike_index], kT, kR, kR, key.side,
        inputs.method, inputs.al_opts, nullptr, inputs.iv_tolerance, inputs.iv_max_iterations);
    ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
    EXPECT_DOUBLE_EQ(prepared->score_columns().market_iv.front(), *expected);
    EXPECT_NE(prepared->fit_observations().front().sigma_mkt,
              prepared->score_columns().market_iv.front());
  }
}

TEST(PreparedFitting, ScoringOptOutSkipsIndependentRawMidInversions) {
  const Chain chain = make_chain();
  PreparedSliceInputs without_scoring = configured_inputs();
  without_scoring.calib.anchor_kind = atx::vol::CalibAnchorKind::Bid;
  without_scoring.prepare_scoring = false;
  const auto skipped = PreparedSlice::create(chain, without_scoring);
  ASSERT_TRUE(skipped.has_value()) << skipped.error().to_string();
  EXPECT_TRUE(skipped->score_keys().empty());
  EXPECT_EQ(skipped->provenance().n_score_inversions, 0u);

  PreparedSliceInputs with_scoring = without_scoring;
  with_scoring.prepare_scoring = true;
  const auto prepared = PreparedSlice::create(chain, with_scoring);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  EXPECT_EQ(prepared->score_keys().size(), prepared->fit_observations().size());
  EXPECT_EQ(prepared->provenance().n_score_inversions, prepared->fit_observations().size());
}

TEST(PreparedFitting, MidAnchorReusesMathematicallyIdenticalFitSigmaForScoring) {
  const Chain chain = make_chain();
  PreparedSliceInputs inputs = configured_inputs();
  inputs.calib.max_obs_per_slice = 6u;
  inputs.calib.anchor_kind = atx::vol::CalibAnchorKind::Mid;
  inputs.prepare_scoring = true;

  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  EXPECT_EQ(prepared->provenance().n_score_inversions, 0u);
  ASSERT_EQ(prepared->fit_observations().size(), prepared->score_columns().market_iv.size());
  for (std::size_t index = 0; index < prepared->fit_observations().size(); ++index) {
    EXPECT_DOUBLE_EQ(prepared->score_columns().market_iv[index],
                     prepared->fit_observations()[index].sigma_mkt);
  }
}

TEST(PreparedFitting, ScoringDoesNotChangeFitObservationsOnAcceleratedPaths) {
  const Chain chain = make_chain();

  // Every fit-relevant field must match; only the score column may differ, so it
  // is deliberately excluded from the comparison.
  const auto expect_fit_rows_identical = [](const PreparedSlice &off, const PreparedSlice &on) {
    const auto a = off.fit_observations();
    const auto b = on.fit_observations();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
      EXPECT_EQ(a[i].source_strike_index, b[i].source_strike_index);
      EXPECT_EQ(a[i].side, b[i].side);
      EXPECT_DOUBLE_EQ(a[i].K, b[i].K);
      EXPECT_DOUBLE_EQ(a[i].k, b[i].k);
      EXPECT_DOUBLE_EQ(a[i].sigma_mkt, b[i].sigma_mkt);
      EXPECT_DOUBLE_EQ(a[i].w_mkt, b[i].w_mkt);
      EXPECT_DOUBLE_EQ(a[i].weight_w, b[i].weight_w);
      EXPECT_DOUBLE_EQ(a[i].active_weight_w, b[i].active_weight_w);
      EXPECT_DOUBLE_EQ(a[i].mid, b[i].mid);
      EXPECT_DOUBLE_EQ(a[i].vega, b[i].vega);
    }
  };

  // Bid anchors and the OTM premium shortcut require an independent raw-mid
  // inversion. A Mid-anchored capped path reuses the fit sigma. Scoring must not add,
  // drop, reorder, or reweight any fit row — its raw-mid inversion is scoring
  // only and can never gate a row's presence in the fit population.
  std::vector<PreparedSliceInputs> presets;
  {
    PreparedSliceInputs in = configured_inputs();
    in.calib.max_obs_per_slice = 6u;
    in.calib.anchor_kind = atx::vol::CalibAnchorKind::Bid; // fit inverts bid, score inverts mid
    presets.push_back(in);
  }
  {
    PreparedSliceInputs in = configured_inputs();
    in.calib.max_otm_shortcut_premium_spread_frac = 5.0; // OTM shortcut path
    presets.push_back(in);
  }

  for (PreparedSliceInputs preset : presets) {
    preset.prepare_scoring = false;
    const auto off = PreparedSlice::create(chain, preset);
    ASSERT_TRUE(off.has_value()) << off.error().to_string();
    preset.prepare_scoring = true;
    const auto on = PreparedSlice::create(chain, preset);
    ASSERT_TRUE(on.has_value()) << on.error().to_string();
    EXPECT_GT(on->provenance().n_score_inversions, 0u)
        << "preset expected to exercise the independent scoring inversion";
    EXPECT_EQ(off->provenance().n_score_inversions, 0u);
    expect_fit_rows_identical(*off, *on);
  }
}

TEST(PreparedFitting, CurveFitParityOptOutReportsZeroScoreInversions) {
  Underlying underlying;
  underlying.uid = 17u;
  underlying.ticker = "SCORE_INTENT";
  underlying.spot = kS;
  underlying.chains.push_back(make_chain());

  SurfaceParityInputs inputs;
  inputs.S = kS;
  inputs.r = kR;
  inputs.deam.imply_borrow = false;
  inputs.deam.borrow_fixed = kR;
  inputs.calib.anchor_kind = atx::vol::CalibAnchorKind::Bid;
  inputs.score_parity = false;
  CurveConfig curve;
  curve.kind = VolCurveKind::LinearVariance;
  const auto skipped = atx::vol::fit_curve_surface(underlying, inputs, curve);
  ASSERT_TRUE(skipped.has_value()) << skipped.error().to_string();
  EXPECT_EQ(skipped->n_score_inversions, 0u);

  inputs.score_parity = true;
  const auto prepared = atx::vol::fit_curve_surface(underlying, inputs, curve);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  EXPECT_GT(prepared->n_score_inversions, 0u);
}

TEST(PreparedFitting, InconsistentCarryOrDiscountIsRejected) {
  const Chain chain = make_chain();
  PreparedSliceInputs bad_carry = configured_inputs();
  bad_carry.q_eff += 0.01;
  EXPECT_FALSE(PreparedSlice::create(chain, bad_carry).has_value());

  PreparedSliceInputs bad_discount = configured_inputs();
  bad_discount.df *= 0.99;
  EXPECT_FALSE(PreparedSlice::create(chain, bad_discount).has_value());

  PreparedSliceInputs nonfinite = configured_inputs();
  nonfinite.F = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(PreparedSlice::create(chain, nonfinite).has_value());
}

TEST(PreparedFitting, LegacyCompatibilityRemainsWiredThroughEssviSurfaceEndToEnd) {
  Underlying underlying;
  underlying.uid = 17u;
  underlying.ticker = "LEGACY";
  underlying.spot = kS;
  underlying.chains.push_back(make_chain());

  SurfaceParityInputs inputs;
  inputs.S = kS;
  inputs.r = kR;
  inputs.deam.imply_borrow = false;
  inputs.deam.borrow_fixed = kR;
  const auto report = atx::vol::run_surface_parity(underlying, inputs);
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  ASSERT_EQ(report->n_slices, 1u);
  ASSERT_EQ(report->context.size(), 1u);
  EXPECT_EQ(report->context.front().n_used, underlying.chains.front().n_strikes());
  // Golden from the historical permissive eSSVI route on this deliberately
  // flagged/locked board. This pins the compatibility seam end to end.
  // Economic-seam pin, not a bit-identity pin (PLAN §3): the K2 Choi-L3 IV
  // seed (e34e3bb) changes the de-Am inversion's Halley path, shifting this
  // converged ATM IV by ~4.9e-11 vol pts — six orders inside the 1e-4 economic
  // gate. Tolerance is 1e-9: tight enough to catch any real regression
  // (>=1e-4-class), loose enough to tolerate iteration-seed LSB drift.
  EXPECT_NEAR(report->surface.iv_on_slice(0u, 0.0), 0.35315599514657198, 1.0e-9);
}

TEST(PreparedFitting, BoardCanonicalizesSliceOrderAndRejectsDuplicateExpiryKeys) {
  const Chain chain = make_chain();
  PreparedSliceInputs later_inputs = configured_inputs();
  later_inputs.expiry_index = 9u;
  PreparedSliceInputs earlier_inputs = configured_inputs();
  earlier_inputs.expiry_index = 2u;
  auto later = PreparedSlice::create(chain, later_inputs);
  auto earlier = PreparedSlice::create(chain, earlier_inputs);
  ASSERT_TRUE(later.has_value());
  ASSERT_TRUE(earlier.has_value());

  std::vector<PreparedSlice> slices;
  slices.push_back(std::move(*later));
  slices.push_back(std::move(*earlier));
  auto board = PreparedBoard::create(std::move(slices));
  ASSERT_TRUE(board.has_value()) << board.error().to_string();
  ASSERT_EQ(board->slices().size(), 2u);
  EXPECT_EQ(board->slices()[0].expiry_index(), 2u);
  EXPECT_EQ(board->slices()[1].expiry_index(), 9u);

  auto duplicate_a = PreparedSlice::create(chain, configured_inputs());
  auto duplicate_b = PreparedSlice::create(chain, configured_inputs());
  ASSERT_TRUE(duplicate_a.has_value());
  ASSERT_TRUE(duplicate_b.has_value());
  std::vector<PreparedSlice> duplicates;
  duplicates.push_back(std::move(*duplicate_a));
  duplicates.push_back(std::move(*duplicate_b));
  EXPECT_FALSE(PreparedBoard::create(std::move(duplicates)).has_value());
}

namespace {

[[nodiscard]] std::size_t count_selected(const std::vector<char> &mask) {
  std::size_t n = 0;
  for (char c : mask) {
    if (c != 0) {
      ++n;
    }
  }
  return n;
}

} // namespace

TEST(PreparedFitting, DeamSpreadKeepsAtmAndWingsWithinCap) {
  // 101 candidates, unique ATM at moneyness 0, symmetric wings at ±1.
  std::vector<double> moneyness(101);
  for (std::size_t i = 0; i < moneyness.size(); ++i) {
    moneyness[i] = -1.0 + 2.0 * static_cast<double>(i) / 100.0;
  }
  constexpr std::uint32_t kCap = 16u;
  const std::vector<char> mask = atx::vol::detail::select_deam_spread(moneyness, kCap);

  ASSERT_EQ(mask.size(), moneyness.size());
  // Invariant: never keep more than the cap.
  EXPECT_LE(count_selected(mask), static_cast<std::size_t>(kCap));
  // Both extreme wings are pinned (outer spline knots).
  EXPECT_NE(mask.front(), 0);
  EXPECT_NE(mask.back(), 0);
  // The unique near-ATM strike is kept, densely with its immediate neighbors.
  EXPECT_NE(mask[50], 0);
  EXPECT_NE(mask[49], 0);
  EXPECT_NE(mask[51], 0);
  EXPECT_NE(mask[48], 0);
  EXPECT_NE(mask[52], 0);
  // The subsample is meaningfully populated (not a lone endpoint pair).
  EXPECT_GE(count_selected(mask), static_cast<std::size_t>(kCap) / 2u);
}

TEST(PreparedFitting, DeamSpreadIsDeterministicAndOrderIndependent) {
  std::vector<double> moneyness(200);
  for (std::size_t i = 0; i < moneyness.size(); ++i) {
    moneyness[i] = -0.8 + 1.6 * static_cast<double>(i) / 199.0;
  }
  const std::vector<char> a = atx::vol::detail::select_deam_spread(moneyness, 32u);
  const std::vector<char> b = atx::vol::detail::select_deam_spread(moneyness, 32u);
  EXPECT_EQ(a, b);
  EXPECT_LE(count_selected(a), 32u);
}

TEST(PreparedFitting, DeamSpreadNoBindKeepsEverything) {
  std::vector<double> moneyness = {-0.3, -0.1, 0.0, 0.1, 0.3};
  // cap == 0 (unlimited) keeps all.
  const std::vector<char> none = atx::vol::detail::select_deam_spread(moneyness, 0u);
  EXPECT_EQ(count_selected(none), moneyness.size());
  // candidate count <= cap keeps all (bit-identical to the uncapped path).
  const std::vector<char> under = atx::vol::detail::select_deam_spread(moneyness, 8u);
  EXPECT_EQ(count_selected(under), moneyness.size());
  const std::vector<char> exact = atx::vol::detail::select_deam_spread(moneyness, 5u);
  EXPECT_EQ(count_selected(exact), moneyness.size());
}
