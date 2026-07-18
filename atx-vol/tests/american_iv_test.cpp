#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/types.hpp"

// American implied-vol inverter coverage.
//   - price->sigma round-trip across a moneyness/maturity/vol grid for both
//     the Andersen-Lake and BAW forward pricers (self-consistent by design),
//   - monotonicity of the forward map the inverter relies on,
//   - deep ITM/OTM behaviour,
//   - no-arbitrage guards (sub-intrinsic, above upper bound, non-finite),
//   - the at-intrinsic clamp to the vol floor,
//   - the strike-axis batch helper and its parallel-status failure convention.
//
// Strict 1e-5 sigma recovery is asserted only where the Black-76 vega is large
// enough that a 1e-5 vol change is resolvable above the cold pricer's noise
// floor; at the deep-wing / short-maturity corners vega collapses and sigma is
// not identifiable, so only the price round-trip is checked there.

namespace {

using atx::vol::AlOpts;
using atx::vol::american_implied_vol;
using atx::vol::american_implied_vol_batch;
using atx::vol::american_price;
using atx::vol::american_price_cached;
using atx::vol::AmericanMethod;
using atx::vol::CorrectionBlend;
using atx::vol::CorrectionCache;
using atx::vol::Side;

// Unwrap a Result<double>, flagging (non-fatally) an unexpected error.
double value_or_fail(const atx::core::Result<double> &r) {
  EXPECT_TRUE(r.has_value()) << (r ? std::string{} : r.error().to_string());
  return r ? *r : std::nan("");
}

// American finite-difference vega — the TRUE sensitivity of the American
// premium to sigma, and the correct conditioning proxy for strict recovery.
// A deep-ITM American option optimally exercised now sits at intrinsic and is
// insensitive to sigma (time value ~ 0), so sigma is unidentifiable there even
// though the European vega is large; only the FD American vega captures that.
double american_vega_fd(double S, double K, double T, double sigma, double r, double q, Side side,
                        AmericanMethod method) {
  const double h = 1.0e-4;
  const double pu = value_or_fail(american_price(S, K, T, sigma + h, r, q, side, method));
  const double pd = value_or_fail(american_price(S, K, T, sigma - h, r, q, side, method));
  return (pu - pd) / (2.0 * h);
}

const char *side_tag(Side side) { return side == Side::Call ? "C" : "P"; }

CorrectionCache make_iv_correction(double r, double q) {
  auto built = CorrectionCache::build(
      /*n_k=*/16, /*n_T=*/8, /*n_s=*/12, r, q,
      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
      /*T_min=*/0.05, /*T_max=*/2.0,
      /*sigma_min=*/0.10, /*sigma_max=*/0.80, Side::Put);
  EXPECT_TRUE(built.has_value());
  return built ? std::move(*built) : CorrectionCache{};
}

// Price a point, invert it, and assert the round-trip. Price recovery is always
// checked; strict sigma recovery only where vega is well above the noise floor.
void check_round_trip(double S, double K, double T, double sigma, double r, double q, Side side,
                      AmericanMethod method) {
  const double p = value_or_fail(american_price(S, K, T, sigma, r, q, side, method));
  if (!std::isfinite(p)) {
    return; // forward pricer already flagged the failure above
  }

  const atx::core::Result<double> iv = american_implied_vol(p, S, K, T, r, q, side, method);
  ASSERT_TRUE(iv.has_value()) << iv.error().to_string() << " [K=" << K << " T=" << T
                              << " sig=" << sigma << " q=" << q << " " << side_tag(side) << "]";

  const double reprice = value_or_fail(american_price(S, K, T, *iv, r, q, side, method));
  EXPECT_NEAR(reprice, p, 1.0e-5 * std::fmax(1.0, p))
      << "price round-trip [K=" << K << " T=" << T << " sig=" << sigma << "]";

  if (american_vega_fd(S, K, T, sigma, r, q, side, method) > 0.5) {
    // BAW is a documented 3-4 significant-figure approximation, so its
    // self-consistent inversion inherits that coarseness at ITM corners;
    // Andersen-Lake is ~1e-7 accurate and holds the strict tolerance.
    const double sig_tol = (method == AmericanMethod::Baw) ? 1.0e-3 : 1.0e-5;
    EXPECT_NEAR(*iv, sigma, sig_tol)
        << "sigma round-trip [K=" << K << " T=" << T << " sig=" << sigma << " q=" << q << " "
        << side_tag(side) << "]";
  }
}

void grid_round_trip(AmericanMethod method) {
  const double S = 100.0, r = 0.05;
  for (double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
    for (double T : {0.1, 0.5, 1.0, 2.0}) {
      for (double sigma : {0.1, 0.2, 0.4}) {
        for (double q : {0.0, 0.03}) {
          for (Side side : {Side::Call, Side::Put}) {
            check_round_trip(S, K, T, sigma, r, q, side, method);
          }
        }
      }
    }
  }
}

} // namespace

// ── Round-trip over the full grid, both pricers ──────────────────────────

TEST(AmericanIv, RoundTrip_GridAndersenLake_RecoversSigma) {
  grid_round_trip(AmericanMethod::AndersenLake);
}

TEST(AmericanIv, RoundTrip_GridBaw_RecoversSigma) { grid_round_trip(AmericanMethod::Baw); }

// ── Monotonicity of the forward map the inverter assumes ─────────────────

TEST(AmericanIvBlend, EndpointMatchesSingleCacheExactly) {
  const CorrectionCache lower = make_iv_correction(0.04, 0.00);
  const CorrectionCache upper = make_iv_correction(0.06, 0.03);
  const CorrectionBlend endpoint = CorrectionBlend::single(&lower);
  constexpr double S = 100.0;
  constexpr double K = 102.0;
  constexpr double T = 0.5;
  constexpr double sigma = 0.27;
  constexpr double r = 0.04;
  constexpr double q = 0.00;
  const double price = american_price_cached(S, K, T, sigma, r, q, Side::Put, &lower);
  const auto single =
      american_implied_vol(price, S, K, T, r, q, Side::Put, AmericanMethod::AndersenLake, 1.0e-7,
                           64, std::nullopt, &lower);
  const auto blended = american_implied_vol(price, S, K, T, r, q, Side::Put, endpoint);
  ASSERT_TRUE(single.has_value());
  ASSERT_TRUE(blended.has_value());
  EXPECT_EQ(*blended, *single);

  const CorrectionBlend upper_endpoint{&lower, &upper, 1.0};
  const double upper_price = american_price_cached(S, K, T, sigma, 0.06, 0.03, Side::Put, &upper);
  const auto upper_single =
      american_implied_vol(upper_price, S, K, T, 0.06, 0.03, Side::Put,
                           AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt, &upper);
  const auto upper_blended =
      american_implied_vol(upper_price, S, K, T, 0.06, 0.03, Side::Put, upper_endpoint);
  ASSERT_TRUE(upper_single.has_value());
  ASSERT_TRUE(upper_blended.has_value());
  EXPECT_EQ(*upper_blended, *upper_single);
}

TEST(AmericanIvBlend, InteriorRoundTripRecoversSigma) {
  const CorrectionCache lower = make_iv_correction(0.04, 0.00);
  const CorrectionCache upper = make_iv_correction(0.06, 0.03);
  const CorrectionBlend blend{&lower, &upper, 0.4};
  constexpr double S = 100.0;
  constexpr double K = 102.0;
  constexpr double T = 0.5;
  constexpr double sigma = 0.27;
  constexpr double r = 0.048;
  constexpr double q = 0.012;
  const double price = american_price_cached(S, K, T, sigma, r, q, Side::Put, blend);
  const auto iv = american_implied_vol(price, S, K, T, r, q, Side::Put, blend);
  ASSERT_TRUE(iv.has_value()) << iv.error().to_string();
  EXPECT_NEAR(*iv, sigma, 1.0e-6);
  EXPECT_NEAR(american_price_cached(S, K, T, *iv, r, q, Side::Put, blend), price, 1.0e-7);
}

TEST(AmericanIv, ForwardPrice_IncreasingInSigma_Monotone) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  for (Side side : {Side::Call, Side::Put}) {
    double prev = -1.0;
    for (double sigma : {0.05, 0.1, 0.2, 0.3, 0.5, 0.8, 1.2}) {
      const double p =
          value_or_fail(american_price(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake));
      EXPECT_GT(p, prev) << "sigma=" << sigma << " " << side_tag(side);
      prev = p;
    }
  }
}

// ── Deep ITM / OTM at a well-conditioned vol ─────────────────────────────

TEST(AmericanIv, DeepItmOtm_ModerateVol_RecoversSigma) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.02, sigma = 0.3;
  for (double K : {60.0, 160.0}) {
    for (Side side : {Side::Call, Side::Put}) {
      const double p =
          value_or_fail(american_price(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake));
      const double iv = value_or_fail(american_implied_vol(p, S, K, T, r, q, side));
      // Deep-ITM American options optimally exercised early sit at intrinsic
      // and are sigma-insensitive (time value ~ 0), so sigma is only
      // identifiable on the well-conditioned (OTM) side.
      if (american_vega_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake) > 0.5) {
        EXPECT_NEAR(iv, sigma, 1.0e-5) << "K=" << K << " " << side_tag(side);
      }
    }
  }
}

// ── BAW method, single point ─────────────────────────────────────────────

TEST(AmericanIv, RoundTrip_BawPut_RecoversSigma) {
  const double S = 100.0, K = 105.0, T = 0.5, sigma = 0.25, r = 0.05, q = 0.0;
  const double p =
      value_or_fail(american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::Baw));
  const double iv =
      value_or_fail(american_implied_vol(p, S, K, T, r, q, Side::Put, AmericanMethod::Baw));
  EXPECT_NEAR(iv, sigma, 1.0e-5);
}

// ── No-arbitrage guards ──────────────────────────────────────────────────

TEST(AmericanIv, SubIntrinsicPrice_Call_ReturnsOutOfRange) {
  const double S = 100.0, K = 80.0, T = 1.0, r = 0.05, q = 0.0; // intrinsic = 20
  const auto iv = american_implied_vol(10.0, S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, SubIntrinsicPrice_Put_ReturnsOutOfRange) {
  const double S = 100.0, K = 120.0, T = 1.0, r = 0.05, q = 0.0; // intrinsic = 20
  const auto iv = american_implied_vol(5.0, S, K, T, r, q, Side::Put);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, PriceAboveUpperBound_Call_ReturnsOutOfRange) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0; // upper = S = 100
  const auto iv = american_implied_vol(101.0, S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, NonFinitePrice_ReturnsOutOfRange) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const auto iv = american_implied_vol(std::nan(""), S, K, T, r, q, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(AmericanIv, NonPositiveStrike_ReturnsInvalidArgument) {
  const auto iv = american_implied_vol(5.0, 100.0, 0.0, 1.0, 0.05, 0.0, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

// ── At-intrinsic clamp ───────────────────────────────────────────────────

TEST(AmericanIv, PriceAtIntrinsic_ClampsToFloor) {
  const double S = 100.0, K = 140.0, T = 1.0, r = 0.05, q = 0.0; // put intrinsic = 40
  const auto iv = american_implied_vol(40.0, S, K, T, r, q, Side::Put);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_DOUBLE_EQ(*iv, atx::vol::kIvMin);
}

// ── R-05: seeded fast path evaluates the floor/ceiling BEFORE clamping/rejecting
//
// The seeded bracket takes bounded (16-step) geometric steps from the warm/euro
// seed. A seed far from a genuine in-range root can exhaust those steps without
// reaching the true vol floor (step-down) or ceiling (step-up). Before this fix
// the fast path then clamped to kIvMin / returned OutOfRange spuriously — even
// though a real, identifiable IV sits inside the bracket — violating the
// documented warm_start contract ("the result is unchanged; only the iteration
// count differs"). The floor/ceiling is now priced first, matching the
// wide-bracket fallback, so the genuine root is solved.

TEST(AmericanIv, SeededStepDown_TinyInRangeIv_NotClampedToFloor) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const double true_sigma = 0.05; // small but far above kIvMin (1e-4)
  const double p = value_or_fail(
      american_price(S, K, T, true_sigma, r, q, Side::Call, AmericanMethod::AndersenLake));
  // warm_start = 2.0 >> true_sigma: the ~7%/step step-down halts near 0.62,
  // never reaching kSigmaLo, so the pre-fix fast path returned kIvMin.
  const auto iv =
      american_implied_vol(p, S, K, T, r, q, Side::Call, AmericanMethod::AndersenLake, 1.0e-7, 64,
                           std::nullopt, /*correction=*/nullptr, /*warm_start=*/2.0);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  // true_sigma (0.05) is a full 10x the vol floor (kIvMin = 0.005); the pre-fix
  // fast path returned exactly kIvMin here, so any value clearly above the floor
  // proves the spurious clamp is gone.
  EXPECT_GT(*iv, atx::vol::kIvMin * 2.0) << "genuine tiny IV must not be clamped to the floor";
  EXPECT_NEAR(*iv, true_sigma, 1.0e-5);
}

TEST(AmericanIv, SeededStepUp_HighInRangeIv_NotRejectedAsOutOfRange) {
  const double S = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const double true_sigma = 1.2; // high but well below kSigmaHiCap (40)
  const double p = value_or_fail(
      american_price(S, K, T, true_sigma, r, q, Side::Call, AmericanMethod::AndersenLake));
  // warm_start = 0.1 << true_sigma: the *1.15/step step-up halts near 0.81,
  // never reaching kSigmaHiCap, so the pre-fix fast path returned OutOfRange.
  const auto iv =
      american_implied_vol(p, S, K, T, r, q, Side::Call, AmericanMethod::AndersenLake, 1.0e-7, 64,
                           std::nullopt, /*correction=*/nullptr, /*warm_start=*/0.1);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  const double reprice =
      value_or_fail(american_price(S, K, T, *iv, r, q, Side::Call, AmericanMethod::AndersenLake));
  EXPECT_NEAR(reprice, p, 1.0e-5 * std::fmax(1.0, p));
  EXPECT_NEAR(*iv, true_sigma, 1.0e-3);
}

// The seeded fast path must agree with the no-warm-start (euro-seeded) path for
// the SAME quote: a warm_start only changes the search trajectory, never the
// returned IV. This pins the R-05 invariant across both branches at once.
TEST(AmericanIv, WarmStartResultInvariantToSeed) {
  const double S = 100.0, K = 105.0, T = 0.75, r = 0.04, q = 0.01;
  const double true_sigma = 0.22;
  const double p = value_or_fail(
      american_price(S, K, T, true_sigma, r, q, Side::Put, AmericanMethod::AndersenLake));
  const auto base = american_implied_vol(p, S, K, T, r, q, Side::Put);
  ASSERT_TRUE(base.has_value()) << (base ? std::string{} : base.error().to_string());
  for (double ws : {0.02, 0.1, 0.5, 2.0, 4.5}) {
    const auto iv =
        american_implied_vol(p, S, K, T, r, q, Side::Put, AmericanMethod::AndersenLake, 1.0e-7, 64,
                             std::nullopt, /*correction=*/nullptr, /*warm_start=*/ws);
    ASSERT_TRUE(iv.has_value()) << "warm_start=" << ws << ": "
                                << (iv ? std::string{} : iv.error().to_string());
    EXPECT_NEAR(*iv, *base, 1.0e-6) << "warm_start=" << ws;
  }
}

// ── Batch helper over a strike axis ──────────────────────────────────────

TEST(AmericanIv, Batch_StrikeAxis_MatchesScalar) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.0, sigma = 0.25;
  const Side side = Side::Call;
  const std::array<double, 4> K{80.0, 95.0, 105.0, 120.0};
  std::array<double, 4> price{};
  for (std::size_t i = 0; i < K.size(); ++i) {
    price[i] =
        value_or_fail(american_price(S, K[i], T, sigma, r, q, side, AmericanMethod::AndersenLake));
  }

  std::array<double, 4> iv_out{};
  std::array<atx::vol::Status, 4> status{};
  const auto s = american_implied_vol_batch(price, S, K, T, r, q, side, iv_out, status);
  ASSERT_TRUE(s.has_value()) << (s ? std::string{} : s.error().to_string());
  for (std::size_t i = 0; i < K.size(); ++i) {
    EXPECT_TRUE(status[i].has_value()) << "K=" << K[i];
    EXPECT_NEAR(iv_out[i], sigma, 1.0e-5) << "K=" << K[i];
  }
}

TEST(AmericanIv, Batch_SubIntrinsicLane_NanValueAndErrorStatus) {
  const double S = 100.0, T = 1.0, r = 0.05, q = 0.0;
  const Side side = Side::Call;
  const std::array<double, 2> K{90.0, 90.0}; // intrinsic = 10
  const double good =
      value_or_fail(american_price(S, 90.0, T, 0.2, r, q, side, AmericanMethod::AndersenLake));
  const std::array<double, 2> price{5.0, good}; // lane 0 is sub-intrinsic

  std::array<double, 2> iv_out{};
  std::array<atx::vol::Status, 2> status{};
  const auto s = american_implied_vol_batch(price, S, K, T, r, q, side, iv_out, status);
  ASSERT_TRUE(s.has_value());
  EXPECT_FALSE(status[0].has_value());
  EXPECT_TRUE(std::isnan(iv_out[0]));
  EXPECT_TRUE(status[1].has_value());
  EXPECT_NEAR(iv_out[1], 0.2, 1.0e-5);
}

TEST(AmericanIv, Batch_SpanLengthMismatch_ReturnsInvalidArgument) {
  const std::array<double, 3> price{1.0, 2.0, 3.0};
  const std::array<double, 2> K{90.0, 100.0}; // length mismatch vs price
  std::array<double, 3> iv_out{};
  std::array<atx::vol::Status, 3> status{};
  const auto s =
      american_implied_vol_batch(price, 100.0, K, 1.0, 0.05, 0.0, Side::Call, iv_out, status);
  ASSERT_FALSE(s.has_value());
  EXPECT_EQ(s.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(AmericanIv, LightweightTelemetryMeasuresCompleteInversionKernel) {
  namespace lw = atx::vol::counters::lightweight;
  constexpr double S = 100.0;
  constexpr double K = 105.0;
  constexpr double T = 0.75;
  constexpr double sigma = 0.24;
  constexpr double r = 0.04;
  constexpr double q = 0.01;
  const double price =
      value_or_fail(american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::AndersenLake));

  lw::reset();
  for (std::uint32_t i = 0; i < lw::kSamplePeriod; ++i) {
    const auto iv = american_implied_vol(price, S, K, T, r, q, Side::Put);
    ASSERT_TRUE(iv.has_value());
  }
  const lw::Snapshot measured = lw::snapshot();
  EXPECT_EQ(measured.american_iv_samples, 1u);
  EXPECT_GT(measured.residual_evaluations_in_sampled_iv, 0u);
  EXPECT_GT(measured.boundary_solves_in_sampled_iv, 0u);
  EXPECT_GT(measured.exp_calls_in_sampled_iv, 0u);
}

TEST(AmericanIv, RetainedThreadLocalPricerAllocatesOnlyOnFirstColdInversion) {
  using atx::vol::counters::Counter;
  if constexpr (!atx::vol::counters::counters_enabled()) {
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  }

  const AlOpts fast = atx::vol::al_fast_opts();
  constexpr double sigma_a = 0.24;
  constexpr double sigma_b = 0.34;
  const double price_a = value_or_fail(american_price(100.0, 105.0, 0.75, sigma_a, 0.04, 0.01,
                                                      Side::Put, AmericanMethod::AndersenLake));
  const double price_b = value_or_fail(american_price(180.0, 155.0, 0.35, sigma_b, 0.025, 0.065,
                                                      Side::Call, AmericanMethod::AndersenLake,
                                                      fast));

  bool inversion_ok = true;
  std::uint64_t first_allocations = 0u;
  std::uint64_t reuse_allocations = 0u;
  atx::vol::counters::reset();
  std::jthread worker([&] {
    const auto first = american_implied_vol(price_a, 100.0, 105.0, 0.75, 0.04, 0.01, Side::Put);
    inversion_ok = first.has_value() && std::fabs(*first - sigma_a) < 1.0e-5;
    first_allocations = atx::vol::counters::snapshot().get(Counter::AloStateAllocations);

    atx::vol::counters::reset();
    for (int i = 0; i < 8; ++i) {
      const bool use_fast = (i & 1) != 0;
      const auto iv = use_fast
                          ? american_implied_vol(price_b, 180.0, 155.0, 0.35, 0.025, 0.065,
                                                 Side::Call, AmericanMethod::AndersenLake, 1.0e-7,
                                                 64, fast)
                          : american_implied_vol(price_a, 100.0, 105.0, 0.75, 0.04, 0.01,
                                                 Side::Put);
      inversion_ok = inversion_ok && iv.has_value() &&
                     std::fabs(*iv - (use_fast ? sigma_b : sigma_a)) < 1.0e-5;
    }
    reuse_allocations = atx::vol::counters::snapshot().get(Counter::AloStateAllocations);
  });
  worker.join();

  EXPECT_TRUE(inversion_ok);
  EXPECT_EQ(first_allocations, 1u);
  EXPECT_EQ(reuse_allocations, 0u);
}

TEST(AmericanIv, BawAndCachedMapsBypassThreadLocalAloState) {
  using atx::vol::counters::Counter;
  if constexpr (!atx::vol::counters::counters_enabled()) {
    GTEST_SKIP() << "ATX_VOL_COUNTERS off: rebuild with -DATX_VOL_COUNTERS=ON";
  }

  constexpr double S = 100.0;
  constexpr double K = 102.0;
  constexpr double T = 0.5;
  constexpr double sigma = 0.27;
  constexpr double r = 0.04;
  constexpr double q = 0.0;
  const double baw_price =
      value_or_fail(american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::Baw));
  const CorrectionCache cache = make_iv_correction(r, q);
  const double cache_price = american_price_cached(S, K, T, sigma, r, q, Side::Put, &cache);

  bool baw_ok = false;
  bool cache_ok = false;
  atx::vol::counters::reset();
  std::jthread worker([&] {
    const auto baw_iv =
        american_implied_vol(baw_price, S, K, T, r, q, Side::Put, AmericanMethod::Baw);
    const auto cache_iv = american_implied_vol(cache_price, S, K, T, r, q, Side::Put,
                                               AmericanMethod::AndersenLake, 1.0e-7, 64,
                                               std::nullopt, &cache);
    baw_ok = baw_iv.has_value() && std::fabs(*baw_iv - sigma) < 1.0e-3;
    cache_ok = cache_iv.has_value() && std::fabs(*cache_iv - sigma) < 1.0e-6;
  });
  worker.join();

  EXPECT_TRUE(baw_ok);
  EXPECT_TRUE(cache_ok);
  EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::AloStateAllocations), 0u);
}
