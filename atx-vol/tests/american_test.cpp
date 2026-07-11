#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/greeks.hpp"
#include "support/oracle_pricer_pde.hpp"

// American pricer coverage, ported from the C ats-vol tests test_pricer_al.c,
// test_pricer_al_checkpoint.c, and test_greeks_american.c:
//   - American == European in no-early-exercise regimes,
//   - positive early-exercise premium where admissible,
//   - intrinsic floors and degenerate collapse,
//   - McDonald-Schroder put-call symmetry,
//   - the canonical ALO premium to 1e-7,
//   - Andersen-Lake vs a Crank-Nicolson PDE oracle,
//   - Gauss-Legendre quadrature constants,
//   - American Greeks vs finite differences of the cached price.

namespace {

using atx::vol::AloPricer;
using atx::vol::AlOpts;
using atx::vol::american_greeks;
using atx::vol::american_delta;
using atx::vol::american_greeks_al;
using atx::vol::american_greeks_fd;
using atx::vol::american_price;
using atx::vol::AmericanGreeks;
using atx::vol::AmericanMethod;
using atx::vol::american_vega;
using atx::vol::al_fast_opts;
using atx::vol::american_price_cached;
using atx::vol::andersen_lake;
using atx::vol::andersen_lake_call_slice;
using atx::vol::andersen_lake_put_slice;
using atx::vol::baw_american;
using atx::vol::black76_greeks;
using atx::vol::black76_price;
using atx::vol::CorrectionCache;
using atx::vol::Side;
using atx::vol::test::oracle_pde_american;

// Unwrap a Result<double> in a test, failing loudly on an unexpected error.
double value_or_fail(const atx::core::Result<double>& r) {
  EXPECT_TRUE(r.has_value())
      << (r ? std::string{} : r.error().to_string());
  return r ? *r : std::nan("");
}

double euro_put(double S, double K, double T, double sigma, double r, double q) {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T),
                       Side::Put);
}
double euro_call(double S, double K, double T, double sigma, double r, double q) {
  return black76_price(S * std::exp((r - q) * T), K, T, sigma, std::exp(-r * T),
                       Side::Call);
}

// Exact IEEE-754 bit comparison (see backtest_test.cpp): the r>0 corpus must stay
// bit-for-bit identical across this regime-guard change (Global Constraint 1).
[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// ULP distance between two NON-NEGATIVE doubles (prices are >= 0 here). For
// non-negative IEEE-754 doubles the bit pattern is monotonic in value, so the
// unsigned difference of the patterns is exactly the count of representable
// doubles between them. Used by the put-slice homogeneity-reuse spike.
[[nodiscard]] std::uint64_t ulp_distance_nonneg(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return (ba >= bb) ? (ba - bb) : (bb - ba);
}

// Independent statement of the Task-1 spec table (in the ORIGINAL option's own
// (r, q), NOT delegating to the production classifier). Put never-early <=>
// r<=0 && r<=q; call never-early <=> q<=0 && q<=r. Outside that but with the
// short-rate side <=0, a double-continuation region appears the single-boundary
// ALO scheme cannot price.
enum class Regime { European, Unsupported, American };
[[nodiscard]] Regime classify_spec(double r, double q, Side side) {
  const double rate = (side == Side::Put) ? r : q;   // internal-put short rate
  const double yield = (side == Side::Put) ? q : r;  // internal-put yield
  if (rate > 0.0) {
    return Regime::American;
  }
  return (rate <= yield) ? Regime::European : Regime::Unsupported;
}

// ── No-early-exercise short-circuits ────────────────────────────────────

TEST(AndersenLake, CallNoDividend_EqualsEuropean) {
  const double S = 100.0, K = 100.0, T = 0.5, sigma = 0.25, r = 0.04, q = 0.0;
  const double p = value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Call));
  EXPECT_LT(std::fabs(p - euro_call(S, K, T, sigma, r, q)), 1.0e-12);
}

TEST(AndersenLake, PutNegativeRate_EqualsEuropean) {
  const double S = 100.0, K = 100.0, T = 1.0, sigma = 0.30, r = -0.01, q = 0.02;
  const double p = value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Put));
  EXPECT_LT(std::fabs(p - euro_put(S, K, T, sigma, r, q)), 1.0e-12);
}

// ── Early-exercise premium is positive where admissible ─────────────────

TEST(AndersenLake, PutEarlyExercise_PremiumPositive) {
  const double S = 100.0, K = 100.0, T = 1.0, sigma = 0.25, r = 0.05, q = 0.0;
  const double p = value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Put));
  const double euro = euro_put(S, K, T, sigma, r, q);
  EXPECT_GT(p, euro);
  EXPECT_GT(p - euro, 0.001);
}

TEST(AndersenLake, CallWithDividend_PremiumPositive) {
  const double S = 100.0, K = 100.0, T = 1.0, sigma = 0.25, r = 0.02, q = 0.05;
  const double p = value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Call));
  EXPECT_GT(p, euro_call(S, K, T, sigma, r, q));
}

// ── Intrinsic floor and degenerates ─────────────────────────────────────

TEST(AndersenLake, DeepItmPut_AboveIntrinsic) {
  const double S = 50.0, K = 100.0, T = 1.0, sigma = 0.25, r = 0.05, q = 0.0;
  const double p = value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Put));
  EXPECT_GE(p, K - S);
}

TEST(AndersenLake, ZeroTime_ReturnsIntrinsic) {
  const double c = value_or_fail(
      andersen_lake(110.0, 100.0, 0.0, 0.25, 0.05, 0.0, Side::Call));
  EXPECT_LT(std::fabs(c - 10.0), 1.0e-12);
  const double p =
      value_or_fail(andersen_lake(90.0, 100.0, 0.0, 0.25, 0.05, 0.0, Side::Put));
  EXPECT_LT(std::fabs(p - 10.0), 1.0e-12);
}

TEST(AndersenLake, ZeroSigma_ReturnsFiniteNonNegative) {
  const double p =
      value_or_fail(andersen_lake(100.0, 100.0, 0.5, 0.0, 0.05, 0.0, Side::Put));
  EXPECT_TRUE(std::isfinite(p));
  EXPECT_GE(p, 0.0);
}

TEST(AndersenLake, NonPositiveSpot_IsInvalidArgument) {
  const auto r = andersen_lake(-1.0, 100.0, 0.5, 0.25, 0.05, 0.0, Side::Put);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── Degenerate-input contract: greeks vs vega asymmetry (P1-4) ──────────────
//
// Intentional, LOAD-BEARING asymmetry: `american_greeks` surfaces an error on
// degenerate input while `american_vega` returns the exact 0.0 sentinel the IV
// inverter's Newton step reads as "vega unavailable, force bisection". Pin BOTH
// so neither contract can silently drift.
TEST(AmericanDegenerateContract, GreeksErrorButVegaReturnsZeroSentinel) {
  const double K = 100.0, T = 0.5, sigma = 0.25, r = 0.05, q = 0.0;

  // Non-positive spot.
  {
    const auto g = american_greeks(-1.0, K, T, sigma, r, q, Side::Call, nullptr);
    ASSERT_FALSE(g.has_value());
    EXPECT_EQ(g.error().code(), atx::core::ErrorCode::InvalidArgument);
    EXPECT_EQ(american_vega(-1.0, K, T, sigma, r, q, Side::Call, nullptr), 0.0);
  }
  // Zero volatility.
  {
    const auto g = american_greeks(100.0, K, T, 0.0, r, q, Side::Put, nullptr);
    ASSERT_FALSE(g.has_value());
    EXPECT_EQ(g.error().code(), atx::core::ErrorCode::InvalidArgument);
    EXPECT_EQ(american_vega(100.0, K, T, 0.0, r, q, Side::Put, nullptr), 0.0);
  }
  // Non-positive time to expiry.
  {
    const auto g = american_greeks(100.0, K, 0.0, sigma, r, q, Side::Call, nullptr);
    ASSERT_FALSE(g.has_value());
    EXPECT_EQ(g.error().code(), atx::core::ErrorCode::InvalidArgument);
    EXPECT_EQ(american_vega(100.0, K, 0.0, sigma, r, q, Side::Call, nullptr), 0.0);
  }
}

// ── McDonald-Schroder put-call symmetry ─────────────────────────────────

TEST(AndersenLake, McDonaldSchroderSymmetry_CallEqualsSwappedPut) {
  const double S = 110.0, K = 100.0, T = 1.0, sigma = 0.25, r = 0.04, q = 0.06;
  const double c = value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Call));
  const double p = value_or_fail(andersen_lake(K, S, T, sigma, q, r, Side::Put));
  const double slack = std::fmax(1.0e-5, 1.0e-3 * std::fmax(c, p));
  EXPECT_LT(std::fabs(c - p), slack);
}

// ── Canonical ALO reference premium (QuantLib FP-B) to 1e-7 ──────────────

TEST(AndersenLake, CanonicalAtmZeroCarry_PremiumMatchesReference) {
  const double S = 100.0, K = 100.0, T = 1.0, sigma = 0.25, r = 0.05, q = 0.05;
  const double expected_premium = 0.1069526779971959;

  const AlOpts opts{/*n_collocation=*/32, /*n_quadrature=*/64,
                    /*max_newton_iter=*/16, /*tol=*/1.0e-13};
  const double p =
      value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Put, opts));
  const double euro = euro_put(S, K, T, sigma, r, q);
  EXPECT_LT(std::fabs((p - euro) - expected_premium), 1.0e-7);
}

// ── Andersen-Lake vs Crank-Nicolson PDE oracle ──────────────────────────

// The AL-vs-PDE agreement is a per-(S,sigma,T) property, so the oracle grid is
// pruned to its representative CORNERS — deep-ITM / ATM / deep-OTM (S) x low/high
// vol x short/long T — instead of the full 5x3x3 sweep. Each Crank-Nicolson PDE
// solve is ~0.7 s, so the corner grid keeps the ITM/ATM/OTM x short/long coverage
// at a fraction of the wall (the interior nodes add no distinct accuracy claim).
TEST(AndersenLake, VsPdeOracle_PutGrid) {
  const double Ss[] = {80.0, 100.0, 120.0};    // deep-ITM / ATM / deep-OTM
  const double sigmas[] = {0.20, 0.40};        // low / high vol
  const double Ts[] = {0.25, 1.00};            // short / long
  const double K = 100.0, r = 0.05, q = 0.02;

  double max_rel = 0.0;
  int n_compared = 0;
  for (double S : Ss)
    for (double sigma : sigmas)
      for (double T : Ts) {
        const double p_al =
            value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Put));
        const double p_pde = oracle_pde_american(S, K, T, sigma, r, q, Side::Put);
        ASSERT_TRUE(std::isfinite(p_pde));
        if (p_pde > 0.05) {
          max_rel = std::fmax(max_rel, std::fabs(p_al - p_pde) / p_pde);
          ++n_compared;
        }
      }
  EXPECT_GT(n_compared, 6);
  EXPECT_LT(max_rel, 5.0e-3);
}

TEST(AndersenLake, VsPdeOracle_CallGrid) {
  const double Ss[] = {80.0, 100.0, 120.0};    // deep-ITM / ATM / deep-OTM
  const double sigmas[] = {0.20, 0.40};        // low / high vol
  const double Ts[] = {0.25, 1.00};            // short / long
  const double K = 100.0, r = 0.03, q = 0.05;  // q > r admits early call exercise

  double max_rel = 0.0;
  int n_compared = 0;
  for (double S : Ss)
    for (double sigma : sigmas)
      for (double T : Ts) {
        const double p_al =
            value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Call));
        const double p_pde =
            oracle_pde_american(S, K, T, sigma, r, q, Side::Call);
        ASSERT_TRUE(std::isfinite(p_pde));
        if (p_pde > 0.05) {
          max_rel = std::fmax(max_rel, std::fabs(p_al - p_pde) / p_pde);
          ++n_compared;
        }
      }
  EXPECT_GT(n_compared, 6);
  EXPECT_LT(max_rel, 5.0e-3);
}

// ── Barone-Adesi-Whaley ─────────────────────────────────────────────────

TEST(Baw, PutEarlyExercise_PremiumPositive) {
  const double S = 100.0, K = 100.0, T = 1.0, sigma = 0.25, r = 0.05, q = 0.0;
  const double p = value_or_fail(baw_american(S, K, T, sigma, r, q, Side::Put));
  EXPECT_GT(p, euro_put(S, K, T, sigma, r, q));
  EXPECT_GE(p, K - S);
}

TEST(Baw, CallNoDividend_EqualsEuropean) {
  const double S = 100.0, K = 100.0, T = 0.5, sigma = 0.25, r = 0.04, q = 0.0;
  const double p = value_or_fail(baw_american(S, K, T, sigma, r, q, Side::Call));
  EXPECT_LT(std::fabs(p - euro_call(S, K, T, sigma, r, q)), 1.0e-12);
}

TEST(Baw, VsPdeOracle_WithinApproximationTolerance) {
  // BAW is a quadratic approximation; it is most accurate at moderate tenor and
  // rate. On a benign ATM 1y put it tracks the PDE oracle to a few percent
  // (short-T / high-r corners are BAW's documented 5-7% worst case, avoided
  // here). Assert BAW >= European and within a generous BAW tolerance.
  const double S = 100.0, K = 100.0, T = 1.0, sigma = 0.25, r = 0.04, q = 0.0;
  const double p_baw = value_or_fail(baw_american(S, K, T, sigma, r, q, Side::Put));
  const double p_pde = oracle_pde_american(S, K, T, sigma, r, q, Side::Put);
  ASSERT_TRUE(std::isfinite(p_pde));
  EXPECT_GE(p_baw, euro_put(S, K, T, sigma, r, q));
  EXPECT_LT(std::fabs(p_baw - p_pde) / p_pde, 5.0e-2);
}

// ── Gauss-Legendre quadrature constants ─────────────────────────────────

TEST(GaussLegendre, Weights_SumToTwo) {
  const auto gl = atx::vol::detail::gauss_legendre(8);
  ASSERT_TRUE(gl.ok);
  double wsum = 0.0;
  for (unsigned i = 0; i < gl.n; ++i) {
    wsum += gl.weights[i];
  }
  EXPECT_LT(std::fabs(wsum - 2.0), 1.0e-13);
}

TEST(GaussLegendre, IntegratesExp_MatchesClosedForm) {
  const auto gl = atx::vol::detail::gauss_legendre(8);
  ASSERT_TRUE(gl.ok);
  double integral = 0.0;
  for (unsigned i = 0; i < gl.n; ++i) {
    integral += gl.weights[i] * std::exp(gl.nodes[i]);
  }
  EXPECT_LT(std::fabs(integral - 2.35040238728760), 1.0e-12);  // e - 1/e
}

TEST(GaussLegendre, UnsupportedOrder_ReportsNotOk) {
  const auto gl = atx::vol::detail::gauss_legendre(7);
  EXPECT_FALSE(gl.ok);
}

// ── American Greeks vs finite differences of the cached price ───────────

CorrectionCache make_correction(Side side, double r, double q) {
  const AlOpts opts = atx::vol::al_default_opts();
  auto built = CorrectionCache::build(/*n_k=*/16, /*n_T=*/12, /*n_s=*/8, r, q,
                                      /*k_log_min=*/-0.4, /*k_log_max=*/0.4,
                                      /*T_min=*/0.05, /*T_max=*/1.0,
                                      /*sigma_min=*/0.10, /*sigma_max=*/0.60,
                                      side, opts);
  EXPECT_TRUE(built.has_value());
  return built ? std::move(*built) : CorrectionCache{};
}

TEST(AmericanGreeks, Delta_MatchesFd_Put) {
  const double r = 0.04, q = 0.01;
  const CorrectionCache tbl = make_correction(Side::Put, r, q);
  const double S = 100.0, K = 105.0, T = 0.5, sigma = 0.25;

  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Put, &tbl);
  ASSERT_TRUE(g.has_value());

  const double h = 1.0e-3;
  const double up = american_price_cached(S + h, K, T, sigma, r, q, Side::Put, &tbl);
  const double dn = american_price_cached(S - h, K, T, sigma, r, q, Side::Put, &tbl);
  EXPECT_LT(std::fabs(g->delta - (up - dn) / (2.0 * h)), 1.0e-5);
}

TEST(AmericanGreeks, Vega_MatchesFd_Call) {
  const double r = 0.05, q = 0.02;
  const CorrectionCache tbl = make_correction(Side::Call, r, q);
  const double S = 100.0, K = 95.0, T = 0.4, sigma = 0.30;

  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Call, &tbl);
  ASSERT_TRUE(g.has_value());

  const double h = 1.0e-5;
  const double up = american_price_cached(S, K, T, sigma + h, r, q, Side::Call, &tbl);
  const double dn = american_price_cached(S, K, T, sigma - h, r, q, Side::Call, &tbl);
  EXPECT_LT(std::fabs(g->vega - (up - dn) / (2.0 * h)), 1.0e-4);
}

TEST(AmericanGreeks, Theta_MatchesFd_Put) {
  const double r = 0.04, q = 0.0;
  const CorrectionCache tbl = make_correction(Side::Put, r, q);
  const double S = 100.0, K = 100.0, T = 0.5, sigma = 0.25;

  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Put, &tbl);
  ASSERT_TRUE(g.has_value());

  const double h = 1.0e-5;
  const double up = american_price_cached(S, K, T + h, sigma, r, q, Side::Put, &tbl);
  const double dn = american_price_cached(S, K, T - h, sigma, r, q, Side::Put, &tbl);
  const double theta_fd = -(up - dn) / (2.0 * h);
  EXPECT_LT(std::fabs(g->theta - theta_fd) / (std::fabs(theta_fd) + 1.0e-3), 1.0e-3);
}

TEST(AmericanGreeks, Rho_MatchesFd_Call) {
  const double r = 0.05, q = 0.02;
  const CorrectionCache tbl = make_correction(Side::Call, r, q);
  const double S = 100.0, K = 100.0, T = 1.0, sigma = 0.20;

  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Call, &tbl);
  ASSERT_TRUE(g.has_value());

  const double hr = 1.0e-5;
  const double up = american_price_cached(S, K, T, sigma, r + hr, q, Side::Call, &tbl);
  const double dn = american_price_cached(S, K, T, sigma, r - hr, q, Side::Call, &tbl);
  EXPECT_LT(std::fabs(g->rho - (up - dn) / (2.0 * hr)), 1.0e-4);
}

TEST(AmericanGreeks, Gamma_MatchesFd_Put) {
  const double r = 0.04, q = 0.01;
  const CorrectionCache tbl = make_correction(Side::Put, r, q);
  const double S = 100.0, K = 100.0, T = 0.5, sigma = 0.25;

  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Put, &tbl);
  ASSERT_TRUE(g.has_value());

  const double h = 0.05;  // coarse step matches the cache's noise floor
  const double mid = american_price_cached(S, K, T, sigma, r, q, Side::Put, &tbl);
  const double up = american_price_cached(S + h, K, T, sigma, r, q, Side::Put, &tbl);
  const double dn = american_price_cached(S - h, K, T, sigma, r, q, Side::Put, &tbl);
  const double gamma_fd = (up - 2.0 * mid + dn) / (h * h);
  EXPECT_LT(std::fabs(g->gamma - gamma_fd) / (std::fabs(gamma_fd) + 1.0e-3), 5.0e-2);
}

TEST(AmericanGreeks, Vanna_MatchesCrossFd_Put) {
  const double r = 0.04, q = 0.01;
  const CorrectionCache tbl = make_correction(Side::Put, r, q);
  const double S = 100.0, K = 100.0, T = 0.5, sigma = 0.25;

  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Put, &tbl);
  ASSERT_TRUE(g.has_value());

  const double hS = 0.05, hs = 1.0e-3;
  const double pp = american_price_cached(S + hS, K, T, sigma + hs, r, q, Side::Put, &tbl);
  const double pm = american_price_cached(S + hS, K, T, sigma - hs, r, q, Side::Put, &tbl);
  const double mp = american_price_cached(S - hS, K, T, sigma + hs, r, q, Side::Put, &tbl);
  const double mm = american_price_cached(S - hS, K, T, sigma - hs, r, q, Side::Put, &tbl);
  const double vanna_fd = (pp - pm - mp + mm) / (4.0 * hS * hs);
  EXPECT_LT(std::fabs(g->vanna - vanna_fd), 1.0e-2);
}

TEST(AmericanGreeks, Volga_MatchesFd_Put) {
  const double r = 0.04, q = 0.01;
  const CorrectionCache tbl = make_correction(Side::Put, r, q);
  const double S = 100.0, K = 100.0, T = 0.5, sigma = 0.25;

  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Put, &tbl);
  ASSERT_TRUE(g.has_value());

  const double h = 0.005;
  const double mid = american_price_cached(S, K, T, sigma, r, q, Side::Put, &tbl);
  const double up = american_price_cached(S, K, T, sigma + h, r, q, Side::Put, &tbl);
  const double dn = american_price_cached(S, K, T, sigma - h, r, q, Side::Put, &tbl);
  const double volga_fd = (up - 2.0 * mid + dn) / (h * h);
  EXPECT_LT(std::fabs(g->volga - volga_fd) / (std::fabs(volga_fd) + 1.0), 1.0e-1);
}

TEST(AmericanGreeks, Charm_FiniteAndBounded_Put) {
  const double r = 0.04, q = 0.01;
  const CorrectionCache tbl = make_correction(Side::Put, r, q);
  const double S = 100.0, K = 100.0, T = 0.5, sigma = 0.25;

  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Put, &tbl);
  ASSERT_TRUE(g.has_value());
  EXPECT_TRUE(std::isfinite(g->charm));

  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const auto gB = black76_greeks(F, K, T, sigma, r, df, Side::Put).greeks;
  EXPECT_LT(std::fabs(g->charm), 5.0 * (std::fabs(gB.charm) + 1.0e-3));
}

TEST(AmericanGreeks, NoCorrection_FallsBackToBlack76) {
  const double S = 100.0, K = 100.0, T = 0.5, sigma = 0.25, r = 0.04, q = 0.0;
  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Put, nullptr);
  ASSERT_TRUE(g.has_value());

  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const auto gBpk = black76_greeks(F, K, T, sigma, r, df, Side::Put);
  const auto gB = gBpk.greeks;

  EXPECT_LT(std::fabs(g->price - gBpk.price), 1.0e-12);
  const double m = std::exp((r - q) * T);
  EXPECT_LT(std::fabs(g->delta - m * gB.delta), 1.0e-12);
  EXPECT_LT(std::fabs(g->vega - gB.vega), 1.0e-12);
}

// ── FD boundary-reuse (P1a): fast greeks == 17-solve reference, bit-identical ──

// The pre-P1a algorithm: every one of the 17 stencils a full cold american_price.
// american_greeks_fd's put fast path solves each of the 7 unique (sigma,r,T)
// boundaries once and re-prices the spot stencils against it; because the boundary
// is S-independent and the solve/eval split is the same code al_solve_put runs,
// the result must reproduce this reference to the last bit.
AmericanGreeks greeks_fd_reference(double S, double K, double T, double sigma,
                                   double r, double q, Side side) {
  const double hS = 1.0e-3 * S;
  double hv = 1.0e-3;
  if (sigma - hv <= 0.0) {
    hv = 0.5 * sigma;
  }
  const double hr = 1.0e-4;
  const double hT = 1.0e-3;
  const bool near_expiry = (T - hT <= 1.0e-8);
  auto P = [&](double dS, double dsig, double dr, double dT) {
    return value_or_fail(american_price(S + dS, K, T + dT, sigma + dsig, r + dr, q,
                                        side, AmericanMethod::AndersenLake,
                                        std::nullopt));
  };
  const double p0 = P(0, 0, 0, 0);
  const double p_Sp = P(+hS, 0, 0, 0);
  const double p_Sm = P(-hS, 0, 0, 0);
  const double p_vp = P(0, +hv, 0, 0);
  const double p_vm = P(0, -hv, 0, 0);
  const double p_rp = P(0, 0, +hr, 0);
  const double p_rm = P(0, 0, -hr, 0);
  const double p_Tp = P(0, 0, 0, +hT);
  const double p_Tm = near_expiry ? p0 : P(0, 0, 0, -hT);
  const double p_SpVp = P(+hS, +hv, 0, 0);
  const double p_SpVm = P(+hS, -hv, 0, 0);
  const double p_SmVp = P(-hS, +hv, 0, 0);
  const double p_SmVm = P(-hS, -hv, 0, 0);
  const double p_SpTp = P(+hS, 0, 0, +hT);
  const double p_SmTp = P(-hS, 0, 0, +hT);
  const double p_SpTm = near_expiry ? p_Sp : P(+hS, 0, 0, -hT);
  const double p_SmTm = near_expiry ? p_Sm : P(-hS, 0, 0, -hT);
  const double dT_den = near_expiry ? hT : (2.0 * hT);
  AmericanGreeks g;
  g.price = p0;
  g.delta = (p_Sp - p_Sm) / (2.0 * hS);
  g.gamma = (p_Sp - 2.0 * p0 + p_Sm) / (hS * hS);
  g.vega = (p_vp - p_vm) / (2.0 * hv);
  g.volga = (p_vp - 2.0 * p0 + p_vm) / (hv * hv);
  g.rho = (p_rp - p_rm) / (2.0 * hr);
  g.theta = -(p_Tp - p_Tm) / dT_den;
  g.vanna = (p_SpVp - p_SpVm - p_SmVp + p_SmVm) / (4.0 * hS * hv);
  g.charm = -(p_SpTp - p_SpTm - p_SmTp + p_SmTm) / (2.0 * hS * dT_den);
  return g;
}

TEST(AmericanGreeks, FdBoundaryReuse_BitIdentical_PutGrid) {
  const double S = 100.0;
  const double r = 0.05;
  const double q = 0.03;
  int checked = 0;
  for (const double K : {70.0, 85.0, 100.0, 115.0, 130.0}) {
    for (const double T : {0.02, 0.1, 0.5, 1.0, 2.0}) {
      for (const double sigma : {0.12, 0.25, 0.45}) {
        const auto fast = american_greeks_fd(S, K, T, sigma, r, q, Side::Put);
        ASSERT_TRUE(fast.has_value())
            << "K=" << K << " T=" << T << " sigma=" << sigma;
        const AmericanGreeks ref = greeks_fd_reference(S, K, T, sigma, r, q, Side::Put);
        const std::string at =
            "K=" + std::to_string(K) + " T=" + std::to_string(T) +
            " sigma=" + std::to_string(sigma);
        EXPECT_EQ(fast->price, ref.price) << at;
        EXPECT_EQ(fast->delta, ref.delta) << at;
        EXPECT_EQ(fast->gamma, ref.gamma) << at;
        EXPECT_EQ(fast->vega, ref.vega) << at;
        EXPECT_EQ(fast->volga, ref.volga) << at;
        EXPECT_EQ(fast->rho, ref.rho) << at;
        EXPECT_EQ(fast->theta, ref.theta) << at;
        EXPECT_EQ(fast->vanna, ref.vanna) << at;
        EXPECT_EQ(fast->charm, ref.charm) << at;
        ++checked;
      }
    }
  }
  EXPECT_EQ(checked, 75);
}

// Delta-only fast path: american_delta must reproduce american_greeks_fd's delta
// BIT-IDENTICALLY on both sides (the put/AL lane shares the base boundary; the call
// lane is the same two-price central difference), at ~1-2 boundary solves instead
// of seven/seventeen. This is what makes resolve_strike_by_delta's bisection cheap
// without moving the resolved strike.
TEST(AmericanDelta, MatchesFd_PutCallGrid) {
  const double S = 100.0;
  const double r = 0.05;
  const double q = 0.03;
  int checked = 0;
  for (const Side side : {Side::Put, Side::Call}) {
    for (const double K : {70.0, 85.0, 100.0, 115.0, 130.0}) {
      for (const double T : {0.02, 0.1, 0.5, 1.0, 2.0}) {
        for (const double sigma : {0.12, 0.25, 0.45}) {
          const auto d = american_delta(S, K, T, sigma, r, q, side);
          const auto g = american_greeks_fd(S, K, T, sigma, r, q, side);
          ASSERT_TRUE(d.has_value() && g.has_value())
              << (side == Side::Put ? "put" : "call") << " K=" << K << " T=" << T
              << " sigma=" << sigma;
          EXPECT_EQ(*d, g->delta)
              << (side == Side::Put ? "put" : "call") << " K=" << K << " T=" << T
              << " sigma=" << sigma;
          ++checked;
        }
      }
    }
  }
  EXPECT_EQ(checked, 150);
}

// P1b: warm-started greeks (6 bumped boundaries seeded from the converged base)
// must reconverge to the cold FD path to ~tol — same greeks, several-fold faster.
// The base boundary (== the mark) stays cold, so the price is bit-identical.
TEST(AmericanGreeks, WarmStart_MatchesCold_PutGrid) {
  const double S = 100.0;
  const double r = 0.05;
  const double q = 0.03;
  // Absolute worst-case errors (rel is meaningless where a greek is ~0 for deep
  // ITM/OTM points). Price and delta/gamma share the cold base boundary => exact.
  double abs_price = 0.0, abs_delta = 0.0, abs_gamma = 0.0;
  double abs_vega = 0.0, abs_theta = 0.0, abs_rho = 0.0;
  // Relative error only where the cold greek is materially non-zero.
  double rel_vega = 0.0, rel_theta = 0.0, rel_rho = 0.0;
  int checked = 0;
  for (const double K : {70.0, 85.0, 100.0, 115.0, 130.0}) {
    for (const double T : {0.02, 0.1, 0.5, 1.0, 2.0}) {
      for (const double sigma : {0.12, 0.25, 0.45}) {
        const auto cold = american_greeks_fd(S, K, T, sigma, r, q, Side::Put,
                                             AmericanMethod::AndersenLake,
                                             std::nullopt, /*warm_start=*/false);
        const auto warm = american_greeks_fd(S, K, T, sigma, r, q, Side::Put,
                                             AmericanMethod::AndersenLake,
                                             std::nullopt, /*warm_start=*/true);
        ASSERT_TRUE(cold.has_value());
        ASSERT_TRUE(warm.has_value());
        // Price is the cold base boundary in both paths: bit-identical.
        EXPECT_EQ(warm->price, cold->price)
            << "K=" << K << " T=" << T << " sigma=" << sigma;
        abs_price = std::max(abs_price, std::fabs(warm->price - cold->price));
        abs_delta = std::max(abs_delta, std::fabs(warm->delta - cold->delta));
        abs_gamma = std::max(abs_gamma, std::fabs(warm->gamma - cold->gamma));
        abs_vega = std::max(abs_vega, std::fabs(warm->vega - cold->vega));
        abs_theta = std::max(abs_theta, std::fabs(warm->theta - cold->theta));
        abs_rho = std::max(abs_rho, std::fabs(warm->rho - cold->rho));
        const auto rel_if = [](double a, double b, double floor) {
          return std::fabs(b) > floor ? std::fabs(a - b) / std::fabs(b) : 0.0;
        };
        rel_vega = std::max(rel_vega, rel_if(warm->vega, cold->vega, 1.0));
        rel_theta = std::max(rel_theta, rel_if(warm->theta, cold->theta, 1.0));
        rel_rho = std::max(rel_rho, rel_if(warm->rho, cold->rho, 1.0));
        ++checked;
      }
    }
  }
  std::printf(
      "[p1b-warm-vs-cold] pts=%d abs: price=%.2e delta=%.2e gamma=%.2e vega=%.2e "
      "theta=%.2e rho=%.2e | rel(>1): vega=%.2e theta=%.2e rho=%.2e\n",
      checked, abs_price, abs_delta, abs_gamma, abs_vega, abs_theta, abs_rho,
      rel_vega, rel_theta, rel_rho);
  EXPECT_EQ(checked, 75);
  EXPECT_EQ(abs_price, 0.0);  // base boundary is cold in both => bit-identical
  EXPECT_EQ(abs_delta, 0.0);  // spot stencils reuse the cold base boundary
  EXPECT_EQ(abs_gamma, 0.0);
  // Warm bumped boundaries reconverge from the base seed to the same budget the
  // cold path uses (2 JN + 4 FP sweeps), so the sensitivities match the cold FD
  // reference to within that budget's own convergence noise (~1% on the smallest
  // rate/time sensitivities) and far inside a PnL tick in absolute terms
  // (greek_err * per-step bump << $0.01). The mark (price) stays bit-identical.
  EXPECT_LT(abs_vega, 5.0e-2);
  EXPECT_LT(abs_theta, 5.0e-2);
  EXPECT_LT(abs_rho, 5.0e-1);
  EXPECT_LT(rel_vega, 1.5e-2);
  EXPECT_LT(rel_theta, 1.5e-2);
  EXPECT_LT(rel_rho, 1.5e-2);
}

// P2: analytic (1-solve) greeks vs the 7-solve FD path across the OPRA put grid.
// Prints per-greek worst deviation so the accuracy is calibrated empirically; the
// mark (price) must be bit-identical (same base-boundary evaluation).
TEST(AmericanGreeks, Analytic_VsFd_PutGrid) {
  const double S = 100.0;
  const double r = 0.05;
  const double q = 0.03;
  struct Acc {
    double abs = 0.0, rel = 0.0;
    void add(double a, double b, double floor) {
      abs = std::max(abs, std::fabs(a - b));
      if (std::fabs(b) > floor) rel = std::max(rel, std::fabs(a - b) / std::fabs(b));
    }
  };
  Acc price, delta, gamma, vega, volga, rho, vanna, theta, charm;
  double abs_price = 0.0;
  int checked = 0;
  for (const double K : {70.0, 85.0, 100.0, 115.0, 130.0}) {
    for (const double T : {0.02, 0.1, 0.5, 1.0, 2.0}) {
      for (const double sig : {0.12, 0.25, 0.45}) {
        const auto a = american_greeks_al(S, K, T, sig, r, q, Side::Put);
        const auto f = american_greeks_fd(S, K, T, sig, r, q, Side::Put);
        ASSERT_TRUE(a.has_value());
        ASSERT_TRUE(f.has_value());
        abs_price = std::max(abs_price, std::fabs(a->price - f->price));
        price.add(a->price, f->price, 1.0);
        delta.add(a->delta, f->delta, 0.05);
        gamma.add(a->gamma, f->gamma, 1e-3);
        vega.add(a->vega, f->vega, 1.0);
        volga.add(a->volga, f->volga, 1.0);
        rho.add(a->rho, f->rho, 1.0);
        vanna.add(a->vanna, f->vanna, 1.0);
        theta.add(a->theta, f->theta, 1.0);
        charm.add(a->charm, f->charm, 1.0);
        ++checked;
      }
    }
  }
  std::printf(
      "[p2-analytic-vs-fd] pts=%d abs_price=%.2e\n"
      "  delta abs=%.2e rel=%.2e | gamma abs=%.2e rel=%.2e | vega abs=%.2e rel=%.2e\n"
      "  rho   abs=%.2e rel=%.2e | volga abs=%.2e rel=%.2e | vanna abs=%.2e rel=%.2e\n"
      "  theta abs=%.2e rel=%.2e | charm abs=%.2e rel=%.2e\n",
      checked, abs_price, delta.abs, delta.rel, gamma.abs, gamma.rel, vega.abs,
      vega.rel, rho.abs, rho.rel, volga.abs, volga.rel, vanna.abs, vanna.rel,
      theta.abs, theta.rel, charm.abs, charm.rel);
  EXPECT_EQ(checked, 75);
  // Same base + sigma+/- + r+/- boundaries => price and the six spot/vol/rate greeks
  // are bit-identical to the FD path.
  EXPECT_EQ(abs_price, 0.0);
  EXPECT_EQ(delta.abs, 0.0);
  EXPECT_EQ(gamma.abs, 0.0);
  EXPECT_EQ(vega.abs, 0.0);
  EXPECT_EQ(rho.abs, 0.0);
  EXPECT_EQ(vanna.abs, 0.0);
  EXPECT_EQ(volga.abs, 0.0);
  // theta/charm are the continuation-region PDE (exact there; no time-bump
  // truncation), agreeing with the FD reference to ~1e-3 relative.
  EXPECT_LT(theta.rel, 3.0e-3);
  EXPECT_LT(charm.rel, 5.0e-3);
}

// Controlled A/B of the isolated hot function: fast put greeks (7 boundary solves)
// vs the 17-solve reference. Same params, same process — the ratio is P1a's true
// per-call speedup, free of backtest/book noise. DISABLED (perf, not correctness):
//   run: --gtest_also_run_disabled_tests --gtest_filter=*FdBoundaryReuse_Speedup*
TEST(AmericanGreeks, DISABLED_FdBoundaryReuse_Speedup) {
  const double S = 100.0, r = 0.05, q = 0.03;
  struct Pt { double K, T, sigma; };
  std::vector<Pt> grid;
  for (const double K : {70.0, 85.0, 100.0, 115.0, 130.0}) {
    for (const double T : {0.05, 0.25, 0.75, 1.5}) {
      for (const double sigma : {0.15, 0.30}) {
        grid.push_back({K, T, sigma});
      }
    }
  }
  const int reps = 400;
  volatile double sink = 0.0;

  auto t0 = std::chrono::steady_clock::now();
  for (int rep = 0; rep < reps; ++rep) {
    for (const Pt& p : grid) {
      const auto g = american_greeks_fd(S, p.K, p.T, p.sigma, r, q, Side::Put,
                                        AmericanMethod::AndersenLake, std::nullopt,
                                        /*warm_start=*/false);
      sink += g ? g->delta + g->vega + g->gamma : 0.0;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  for (int rep = 0; rep < reps; ++rep) {
    for (const Pt& p : grid) {
      const auto g = american_greeks_fd(S, p.K, p.T, p.sigma, r, q, Side::Put,
                                        AmericanMethod::AndersenLake, std::nullopt,
                                        /*warm_start=*/true);
      sink += g ? g->delta + g->vega + g->gamma : 0.0;
    }
  }
  auto t2 = std::chrono::steady_clock::now();
  for (int rep = 0; rep < reps; ++rep) {
    for (const Pt& p : grid) {
      const auto g = american_greeks_al(S, p.K, p.T, p.sigma, r, q, Side::Put);
      sink += g ? g->delta + g->vega + g->gamma : 0.0;
    }
  }
  auto t3 = std::chrono::steady_clock::now();
  for (int rep = 0; rep < reps; ++rep) {
    for (const Pt& p : grid) {
      const auto g = greeks_fd_reference(S, p.K, p.T, p.sigma, r, q, Side::Put);
      sink += g.delta + g.vega + g.gamma;
    }
  }
  auto t4 = std::chrono::steady_clock::now();

  const double fast_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double warm_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
  const double al_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
  const double ref_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
  const long calls = static_cast<long>(reps) * static_cast<long>(grid.size());
  const double per = static_cast<double>(calls);
  std::printf(
      "[greeks-speedup] calls=%ld ref(17cold)=%.1fms (%.1fus)\n"
      "  p1a(7cold)=%.1fms (%.1fus, %.2fx) p1b(warm)=%.1fms (%.1fus, %.2fx) "
      "p2(al,5solve)=%.1fms (%.1fus, %.2fx) sink=%.3g\n",
      calls, ref_ms, 1000.0 * ref_ms / per, fast_ms, 1000.0 * fast_ms / per,
      ref_ms / fast_ms, warm_ms, 1000.0 * warm_ms / per, ref_ms / warm_ms, al_ms,
      1000.0 * al_ms / per, ref_ms / al_ms, static_cast<double>(sink));
  EXPECT_GT(ref_ms, fast_ms);
  EXPECT_GT(fast_ms, warm_ms);  // warm must beat cold-fast
  EXPECT_GT(fast_ms, al_ms);    // analytic (5 solves) must beat P1a (7 solves)
}

// ── Warm-started AloPricer (the American-IV throughput lever) ─────────────

// A fresh AloPricer's first price() is the cold-seed path, which is the SAME code
// as andersen_lake (BAW seed + scheme sweeps) — so it must reproduce it exactly.
TEST(AloPricer, ColdFirstCall_MatchesAndersenLake) {
  const double S = 100.0, r = 0.05;
  for (double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
    for (double T : {0.1, 0.5, 1.0, 2.0}) {
      for (double sigma : {0.1, 0.2, 0.4}) {
        for (double q : {0.0, 0.03}) {
          for (Side side : {Side::Call, Side::Put}) {
            AloPricer pr(S, K, T, r, q, side);
            const double warm = pr.price(sigma);
            const double cold =
                value_or_fail(andersen_lake(S, K, T, sigma, r, q, side));
            ASSERT_TRUE(std::isfinite(warm)) << "K=" << K << " T=" << T;
            EXPECT_NEAR(warm, cold, 1.0e-9 * std::fmax(1.0, cold))
                << "K=" << K << " T=" << T << " sig=" << sigma << " q=" << q;
          }
        }
      }
    }
  }
}

// Reused across a fine ascending sigma sweep (warm start engaged), price() stays
// within the scheme's convergence noise of a fresh cold solve. It is NOT required
// to be bit-identical: at hard corners the 2-JN/4-FP boundary is not fully
// converged and is thus seed-dependent (the reason the IV inverter cold-polishes).
TEST(AloPricer, WarmSweep_TracksColdWithinSchemeNoise) {
  const double S = 100.0, r = 0.05, q = 0.02;
  for (double K : {85.0, 100.0, 115.0}) {
    for (double T : {0.25, 1.0, 2.0}) {
      for (Side side : {Side::Call, Side::Put}) {
        AloPricer pr(S, K, T, r, q, side);
        for (double sigma = 0.12; sigma <= 0.45 + 1e-9; sigma += 0.01) {
          const double warm = pr.price(sigma);  // warm after the first
          const double cold =
              value_or_fail(andersen_lake(S, K, T, sigma, r, q, side));
          ASSERT_TRUE(std::isfinite(warm));
          EXPECT_NEAR(warm, cold, 3.0e-3 * std::fmax(1.0, cold) + 1.0e-6)
              << "K=" << K << " T=" << T << " sig=" << sigma;
        }
      }
    }
  }
}

// Degenerate sigma collapses to intrinsic; a no-early-exercise contract (put with
// r <= 0) collapses to the European price — mirroring andersen_lake's guards.
TEST(AloPricer, DegenerateAndEuropeanBranches) {
  {
    AloPricer pr(100.0, 110.0, 1.0, 0.05, 0.0, Side::Put);
    EXPECT_NEAR(pr.price(1.0e-12), 10.0, 1.0e-9);  // intrinsic K - S
  }
  {
    AloPricer pr(100.0, 90.0, 1.0, 0.05, 0.0, Side::Call);
    EXPECT_NEAR(pr.price(1.0e-12), 10.0, 1.0e-9);  // intrinsic S - K
  }
  {
    // Put with r < 0: no early exercise -> European put.
    const double S = 100.0, K = 110.0, T = 1.0, r = -0.01, q = 0.0, sig = 0.3;
    AloPricer pr(S, K, T, r, q, Side::Put);
    EXPECT_NEAR(pr.price(sig), euro_put(S, K, T, sig, r, q), 1.0e-10);
  }
}

// ── Cross-strike call-slice pricer (one boundary, many strikes) ──────────

TEST(AndersenLakeCallSlice, MatchesPerStrikeAndersenLakeBitIdentical) {
  const double S = 600.0, T = 0.35, sigma = 0.22, r = 0.03, q = 0.02;
  std::vector<double> strikes;
  for (double K = 480.0; K <= 720.0 + 1e-9; K += 12.0) {
    strikes.push_back(K);
  }
  std::vector<double> px(strikes.size(), 0.0);
  const atx::vol::Status st = andersen_lake_call_slice(
      S, std::span<const double>(strikes), T, sigma, r, q,
      std::span<double>(px), std::nullopt);
  ASSERT_TRUE(st.has_value()) << (st ? std::string{} : st.error().to_string());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double ref = value_or_fail(
        andersen_lake(S, strikes[i], T, sigma, r, q, Side::Call, std::nullopt));
    // One shared boundary solve must reproduce each per-strike cold solve to the
    // bit (the whole point — surface numbers must not move).
    EXPECT_EQ(px[i], ref) << "strike " << strikes[i];
  }
}

TEST(AndersenLakeCallSlice, FastPresetDegenerateEuroAndValidation) {
  const double S = 600.0, T = 0.5, r = 0.03, q = 0.02;
  std::vector<double> strikes{540.0, 600.0, 660.0};
  std::vector<double> px(3, 0.0);

  // Fast preset routes bit-identically too.
  const AlOpts fast = al_fast_opts();
  ASSERT_TRUE(andersen_lake_call_slice(S, std::span<const double>(strikes), T, 0.2,
                                       r, q, std::span<double>(px), fast)
                  .has_value());
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(px[i], value_or_fail(andersen_lake(S, strikes[i], T, 0.2, r, q,
                                                 Side::Call, fast)));
  }

  // Degenerate sigma -> intrinsic per strike.
  ASSERT_TRUE(andersen_lake_call_slice(S, std::span<const double>(strikes), T, 0.0,
                                       r, q, std::span<double>(px), std::nullopt)
                  .has_value());
  EXPECT_DOUBLE_EQ(px[0], 60.0);  // 600 - 540
  EXPECT_DOUBLE_EQ(px[1], 0.0);   // 600 - 600
  EXPECT_DOUBLE_EQ(px[2], 0.0);   // max(600 - 660, 0)

  // q <= 0: European call per strike (matches the andersen_lake short-circuit).
  ASSERT_TRUE(andersen_lake_call_slice(S, std::span<const double>(strikes), T, 0.2,
                                       r, 0.0, std::span<double>(px), std::nullopt)
                  .has_value());
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(px[i], value_or_fail(andersen_lake(S, strikes[i], T, 0.2, r, 0.0,
                                                 Side::Call, std::nullopt)));
  }

  // Length mismatch is rejected.
  std::vector<double> short_out(2, 0.0);
  EXPECT_FALSE(andersen_lake_call_slice(S, std::span<const double>(strikes), T, 0.2,
                                        r, q, std::span<double>(short_out),
                                        std::nullopt)
                   .has_value());
}

// ── Cross-strike put-slice pricer (one boundary, many strikes) ───────────
//
// Unlike the call slice (bit-identical to per-strike andersen_lake because its
// internal put has a FIXED strike Kp = S), the put slice reuses ONE boundary
// across strikes by strike homogeneity. That reuse is exact only in ℝ: the AL
// sweeps carry b.K in absolute (non-ratio) terms, so a reference-strike boundary
// reused at another strike differs from a fresh per-strike solve by a few ULP.

// STEP-1 SPIKE (kept as a regression). Measure the ULP distribution of the
// one-boundary-reused-then-rescaled put price vs a fresh per-strike andersen_lake
// over an ATM/wing/near-expiry/deep-ITM x (r,q)-corner grid, with the
// forward-normalized spot S = e^{-(r-q)T} the correction cache samples at. Assert
// the MEASURED bound (max ULP + max absolute), and prove the reference strike is
// bit-identical. This is the evidence that decides the correction-cache branch:
// a non-zero ULP gap => the cache put row STAYS on the scalar path (deferred to
// T9's normalized-boundary refactor), so no archive/pin/corpus guard moves.
TEST(AndersenLakePutSlice, StepOneReusedBoundaryUlpSpike) {
  const double rs[] = {0.01, 0.03, 0.05, 0.08};
  const double qs[] = {0.0, 0.01, 0.02, 0.04, 0.07};
  const double Ts[] = {0.02, 0.1, 0.5, 1.0, 2.0};
  const double sigmas[] = {0.1, 0.2, 0.35, 0.6};
  // Forward-normalized strike ladder: deep-OTM put (small K) -> deep-ITM (large K).
  const double Ks[] = {0.5, 0.7, 0.85, 0.95, 1.0, 1.05, 1.15, 1.3, 1.6, 2.0};

  // A "meaningful" price floor: below it, absolute gaps are sub-1e-9 but the ULP /
  // relative counts explode on tiny deep-OTM values. Correctness is gated on
  // absolute gap everywhere AND relative gap above this floor.
  constexpr double kPriceFloor = 1.0e-3;

  std::uint64_t max_ulp = 0;                 // over all points
  std::uint64_t max_ulp_meaningful = 0;      // prices >= floor
  double max_abs = 0.0;                       // over all points
  double max_rel_meaningful = 0.0;            // prices >= floor
  std::uint64_t n_pts = 0;
  std::uint64_t n_bit_identical = 0;
  std::uint64_t ref_strike_pts = 0;
  std::uint64_t ref_strike_bit_identical = 0;
  double worst_r = 0, worst_q = 0, worst_T = 0, worst_s = 0, worst_K = 0;
  double worst_slice = 0, worst_ref = 0;

  std::vector<double> strikes(std::begin(Ks), std::end(Ks));
  std::vector<double> px(strikes.size(), 0.0);

  for (double r : rs)
    for (double q : qs)
      for (double T : Ts)
        for (double sigma : sigmas) {
          const double S = std::exp(-(r - q) * T);
          const auto st = andersen_lake_put_slice(
              S, std::span<const double>(strikes), T, sigma, r, q,
              std::span<double>(px), std::nullopt);
          ASSERT_TRUE(st.has_value())
              << "r=" << r << " q=" << q << " T=" << T << " sigma=" << sigma
              << " : " << st.error().to_string();
          for (std::size_t i = 0; i < strikes.size(); ++i) {
            const double ref = value_or_fail(andersen_lake(
                S, strikes[i], T, sigma, r, q, Side::Put, std::nullopt));
            const std::uint64_t u = ulp_distance_nonneg(px[i], ref);
            const double a = std::fabs(px[i] - ref);
            if (u > max_ulp) max_ulp = u;
            if (a > max_abs) {
              max_abs = a;
              worst_r = r; worst_q = q; worst_T = T; worst_s = sigma;
              worst_K = strikes[i]; worst_slice = px[i]; worst_ref = ref;
            }
            if (ref >= kPriceFloor) {
              if (u > max_ulp_meaningful) max_ulp_meaningful = u;
              const double rel = a / ref;
              if (rel > max_rel_meaningful) max_rel_meaningful = rel;
            }
            ++n_pts;
            if (u == 0) ++n_bit_identical;
            if (i == 0) {  // strikes[0] is the reference strike => must be exact
              ++ref_strike_pts;
              if (u == 0) ++ref_strike_bit_identical;
              EXPECT_TRUE(bits_equal(px[i], ref))
                  << "reference strike not bit-identical: r=" << r << " q=" << q
                  << " T=" << T << " sigma=" << sigma;
            }
          }
        }

  std::printf(
      "[put-slice spike] points=%llu  bit-identical=%llu (%.1f%%)  "
      "max_ulp(all)=%llu  max_abs(all)=%.3e\n"
      "                  meaningful(price>=%.0e): max_ulp=%llu  max_rel=%.3e\n"
      "                  worst-abs @ r=%.3f q=%.3f T=%.3f sigma=%.3f K=%.3f "
      "slice=%.12e ref=%.12e\n"
      "                  ref-strike bit-identical=%llu/%llu\n",
      static_cast<unsigned long long>(n_pts),
      static_cast<unsigned long long>(n_bit_identical),
      100.0 * static_cast<double>(n_bit_identical) / static_cast<double>(n_pts),
      static_cast<unsigned long long>(max_ulp), max_abs, kPriceFloor,
      static_cast<unsigned long long>(max_ulp_meaningful), max_rel_meaningful,
      worst_r, worst_q, worst_T, worst_s, worst_K, worst_slice, worst_ref,
      static_cast<unsigned long long>(ref_strike_bit_identical),
      static_cast<unsigned long long>(ref_strike_pts));

  // The reference strike (strikes[0]) is ALWAYS bit-identical: same solve, same
  // clamp path as al_solve_put. This is the ONE strike whose price does not move.
  EXPECT_EQ(ref_strike_bit_identical, ref_strike_pts);

  // MEASURED bounds (nullopt/ACCURATE preset, this grid). Homogeneity boundary
  // reuse is NOT bit-exact: the reused y[] equals a fresh per-strike y[] only in
  // exact arithmetic (absolute-K terms in the sweep + finite convergence tol), so
  // the gap sits at the boundary-convergence-tolerance level, NOT machine epsilon.
  // A non-zero gap is the recorded evidence for DEFERRING the correction-cache
  // put-row collapse to T9's normalized-boundary refactor. The bounds below are
  // deterministic on this toolchain (measured max_abs ~ 3.2e-8, max_rel ~ 8e-9);
  // they gate a real blow-up while tolerating last-bit toolchain drift.
  EXPECT_LT(max_abs, 1.0e-7);
  EXPECT_LT(max_rel_meaningful, 1.0e-6);
}

// Core correctness gate: the put slice matches a per-strike andersen_lake loop
// across a rate/yield/wing/near-expiry/deep-ITM grid to the MEASURED tolerance.
// The put boundary is homogeneity-reused (one solve rescaled per strike), exact
// only in ℝ, so this is NOT bit-identical like the call slice — the gap sits at
// the boundary-convergence-tolerance level (~1e-6 relative for the ACCURATE
// preset; step-1 spike measured max_rel ~ 1.1e-7). Combined absolute+relative
// tolerance 1e-6·max(1,ref) covers both large ITM and tiny OTM prices.
TEST(AndersenLakePutSlice, MatchesPerStrikeAndersenLake) {
  const double S = 100.0, T = 0.4, sigma = 0.28;
  struct RQ { double r, q; };
  const RQ corners[] = {{0.05, 0.0}, {0.05, 0.02}, {0.03, 0.05}, {0.08, 0.07}, {0.02, 0.01}};
  std::vector<double> strikes;
  for (double K = 60.0; K <= 160.0 + 1e-9; K += 5.0) {
    strikes.push_back(K);
  }
  std::vector<double> px(strikes.size(), 0.0);
  for (const RQ& c : corners) {
    const auto st = andersen_lake_put_slice(
        S, std::span<const double>(strikes), T, sigma, c.r, c.q,
        std::span<double>(px), std::nullopt);
    ASSERT_TRUE(st.has_value())
        << "r=" << c.r << " q=" << c.q << " : " << st.error().to_string();
    for (std::size_t i = 0; i < strikes.size(); ++i) {
      const double ref = value_or_fail(andersen_lake(
          S, strikes[i], T, sigma, c.r, c.q, Side::Put, std::nullopt));
      EXPECT_LT(std::fabs(px[i] - ref), 1.0e-6 * std::fmax(1.0, ref))
          << "K=" << strikes[i] << " r=" << c.r << " q=" << c.q
          << " slice=" << px[i] << " scalar=" << ref;
    }
  }
}

// n=1 ladder equals andersen_lake bit-identically (the single strike IS the
// reference strike, so no homogeneity rescale error).
TEST(AndersenLakePutSlice, SingleStrike_EqualsAndersenLake) {
  const double S = 100.0, T = 0.75, sigma = 0.33, r = 0.06, q = 0.02;
  const double Ks[] = {70.0, 100.0, 140.0};
  for (double K : Ks) {
    const double strike[] = {K};
    double out = 0.0;
    const auto st = andersen_lake_put_slice(
        S, std::span<const double>(strike), T, sigma, r, q,
        std::span<double>(&out, 1), std::nullopt);
    ASSERT_TRUE(st.has_value()) << st.error().to_string();
    const double ref =
        value_or_fail(andersen_lake(S, K, T, sigma, r, q, Side::Put, std::nullopt));
    EXPECT_TRUE(bits_equal(out, ref)) << "K=" << K << " slice=" << out << " scalar=" << ref;
  }
}

// Fast preset also reuses one boundary across strikes to the same tolerance.
TEST(AndersenLakePutSlice, FastPresetMatchesPerStrike) {
  const double S = 100.0, T = 0.5, sigma = 0.3, r = 0.04, q = 0.01;
  const double strikes[] = {75.0, 90.0, 100.0, 110.0, 130.0};
  std::vector<double> px(std::size(strikes), 0.0);
  const AlOpts fast = al_fast_opts();
  ASSERT_TRUE(andersen_lake_put_slice(S, std::span<const double>(strikes), T, sigma,
                                      r, q, std::span<double>(px), fast)
                  .has_value());
  for (std::size_t i = 0; i < std::size(strikes); ++i) {
    const double ref = value_or_fail(
        andersen_lake(S, strikes[i], T, sigma, r, q, Side::Put, fast));
    // Fast preset (tol=1e-8, fewer sweeps): looser boundary-reuse gap than ACCURATE.
    EXPECT_LT(std::fabs(px[i] - ref), 1.0e-4 * std::fmax(1.0, ref)) << "K=" << strikes[i];
  }
}

// Degenerate sigma / T -> put intrinsic max(K_i - S, 0) per strike.
TEST(AndersenLakePutSlice, Degenerate_Intrinsic) {
  const double S = 100.0, T = 0.5, r = 0.03, q = 0.02;
  const double strikes[] = {80.0, 100.0, 130.0};
  std::vector<double> px(3, 0.0);
  ASSERT_TRUE(andersen_lake_put_slice(S, std::span<const double>(strikes), T, 0.0,
                                      r, q, std::span<double>(px), std::nullopt)
                  .has_value());
  EXPECT_DOUBLE_EQ(px[0], 0.0);   // max(80 - 100, 0)
  EXPECT_DOUBLE_EQ(px[1], 0.0);   // max(100 - 100, 0)
  EXPECT_DOUBLE_EQ(px[2], 30.0);  // 130 - 100
  // Degenerate T likewise.
  ASSERT_TRUE(andersen_lake_put_slice(S, std::span<const double>(strikes), 0.0, 0.3,
                                      r, q, std::span<double>(px), std::nullopt)
                  .has_value());
  EXPECT_DOUBLE_EQ(px[2], 30.0);
}

// European corner (r <= 0 && r <= q): Black-76 European put per strike, matching
// the andersen_lake short-circuit exactly.
TEST(AndersenLakePutSlice, European_Black76) {
  const double S = 100.0, T = 1.0, sigma = 0.3, r = -0.01, q = 0.02;
  ASSERT_EQ(classify_spec(r, q, Side::Put), Regime::European);
  const double strikes[] = {80.0, 100.0, 120.0};
  std::vector<double> px(3, 0.0);
  ASSERT_TRUE(andersen_lake_put_slice(S, std::span<const double>(strikes), T, sigma,
                                      r, q, std::span<double>(px), std::nullopt)
                  .has_value());
  for (std::size_t i = 0; i < 3; ++i) {
    const double ref = value_or_fail(
        andersen_lake(S, strikes[i], T, sigma, r, q, Side::Put, std::nullopt));
    EXPECT_TRUE(bits_equal(px[i], ref)) << "K=" << strikes[i];
    EXPECT_TRUE(bits_equal(px[i], euro_put(S, strikes[i], T, sigma, r, q)));
  }
}

// Unsupported double-continuation corner (q < r <= 0): NotImplemented, and every
// per-strike scalar solve errors the same way.
TEST(AndersenLakePutSlice, Unsupported_NotImplemented) {
  const double S = 100.0, T = 1.0, sigma = 0.3, r = -0.005, q = -0.02;
  ASSERT_EQ(classify_spec(r, q, Side::Put), Regime::Unsupported);
  const double strikes[] = {80.0, 100.0, 120.0};
  std::vector<double> px(3, 0.0);
  const auto st = andersen_lake_put_slice(S, std::span<const double>(strikes), T,
                                          sigma, r, q, std::span<double>(px),
                                          std::nullopt);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::NotImplemented);
  for (double K : strikes) {
    const auto sc = andersen_lake(S, K, T, sigma, r, q, Side::Put);
    ASSERT_FALSE(sc.has_value());
    EXPECT_EQ(sc.error().code(), atx::core::ErrorCode::NotImplemented);
  }
}

// Input-validation errors mirror the call slice.
TEST(AndersenLakePutSlice, InputValidation) {
  const double strikes[] = {90.0, 100.0, 110.0};
  std::vector<double> px(3, 0.0);
  // S <= 0
  EXPECT_FALSE(andersen_lake_put_slice(0.0, std::span<const double>(strikes), 0.5,
                                       0.2, 0.03, 0.0, std::span<double>(px))
                   .has_value());
  // negative T
  EXPECT_FALSE(andersen_lake_put_slice(100.0, std::span<const double>(strikes), -0.1,
                                       0.2, 0.03, 0.0, std::span<double>(px))
                   .has_value());
  // negative sigma
  EXPECT_FALSE(andersen_lake_put_slice(100.0, std::span<const double>(strikes), 0.5,
                                       -0.2, 0.03, 0.0, std::span<double>(px))
                   .has_value());
  // non-positive strike
  const double bad_strikes[] = {90.0, 0.0, 110.0};
  EXPECT_FALSE(andersen_lake_put_slice(100.0, std::span<const double>(bad_strikes),
                                       0.5, 0.2, 0.03, 0.0, std::span<double>(px))
                   .has_value());
  // non-finite r
  EXPECT_FALSE(andersen_lake_put_slice(100.0, std::span<const double>(strikes), 0.5,
                                       0.2, std::nan(""), 0.0, std::span<double>(px))
                   .has_value());
  // length mismatch
  std::vector<double> short_out(2, 0.0);
  EXPECT_FALSE(andersen_lake_put_slice(100.0, std::span<const double>(strikes), 0.5,
                                       0.2, 0.03, 0.0, std::span<double>(short_out))
                   .has_value());
}

// ── Negative-rate regime classification (Task 1, P0.5) ──────────────────────
//
// The American pricer's no-early-exercise short-circuit was wrong under negative
// rates: it returned a European price for options that CAN be optimally exercised
// early. The fix classifies (r, q, side) into three regimes (Healy 2021 §2.2):
// European (American == European exactly), Unsupported (a double continuation
// region the single-boundary ALO cannot price -> NotImplemented), and American.

// Rate/yield corner grid: assert each cell lands in the spec regime with the
// right behavior (European == Black-76 to 1e-12; Unsupported -> NotImplemented;
// American -> a sane, above-intrinsic Ok). Cheap (no PDE oracle) so it can sweep
// the full 5x5 x 2 sides x 3 (S/K,T,sigma) points.
TEST(AndersenLakeRegime, RateYieldCornerGrid_Classification) {
  const double rq[] = {-0.02, -0.005, 0.0, 0.005, 0.05};
  struct Pt { double sk, T, sigma; };
  const Pt pts[] = {{1.0, 1.0, 0.20}, {0.8, 1.0, 0.20}, {1.25, 0.25, 0.50}};
  const Side sides[] = {Side::Call, Side::Put};
  const double K = 100.0;
  int n_euro = 0, n_unsup = 0, n_amer = 0;
  for (double r : rq)
    for (double q : rq)
      for (Side side : sides)
        for (const Pt& pt : pts) {
          const double S = pt.sk * K;
          const auto res = andersen_lake(S, K, pt.T, pt.sigma, r, q, side);
          const Regime reg = classify_spec(r, q, side);
          const std::string where = "r=" + std::to_string(r) + " q=" +
                                     std::to_string(q) + " S=" +
                                     std::to_string(S) + " side=" +
                                     (side == Side::Call ? "C" : "P");
          if (reg == Regime::European) {
            ASSERT_TRUE(res.has_value()) << where << " : " << res.error().to_string();
            const double euro = (side == Side::Call)
                                    ? euro_call(S, K, pt.T, pt.sigma, r, q)
                                    : euro_put(S, K, pt.T, pt.sigma, r, q);
            EXPECT_LT(std::fabs(*res - euro), 1.0e-12) << where;
            ++n_euro;
          } else if (reg == Regime::Unsupported) {
            ASSERT_FALSE(res.has_value()) << where << " expected NotImplemented";
            EXPECT_EQ(res.error().code(), atx::core::ErrorCode::NotImplemented) << where;
            ++n_unsup;
          } else {
            ASSERT_TRUE(res.has_value()) << where << " : " << res.error().to_string();
            EXPECT_TRUE(std::isfinite(*res)) << where;
            const double intr = (side == Side::Call) ? (S - K) : (K - S);
            EXPECT_GE(*res, std::fmax(intr, 0.0) - 1.0e-9) << where;
            ++n_amer;
          }
        }
  EXPECT_GT(n_euro, 0);
  EXPECT_GT(n_unsup, 0);
  EXPECT_GT(n_amer, 0);
}

// A curated handful of European and American cells (both sides, mixed rate/yield
// signs) checked against the independent Crank-Nicolson PDE oracle. In the
// European regime the American PDE price equals Black-76 (early exercise never
// optimal); in the American regime AL must track the PDE to the existing
// AL-vs-PDE tolerance. Oracle calls are kept to a couple dozen (each ~a PDE solve).
TEST(AndersenLakeRegime, CornerGrid_VsPdeOracle) {
  struct Cell { double S, r, q; Side side; };
  const Cell cells[] = {
      // European (r<=0 && r<=q  put / q<=0 && q<=r call): American == European.
      {80.0, -0.02, 0.05, Side::Put},   {85.0, -0.005, 0.0, Side::Put},
      {90.0, 0.0, 0.05, Side::Put},     {120.0, 0.05, -0.02, Side::Call},
      {115.0, 0.0, -0.005, Side::Call}, {110.0, 0.05, 0.0, Side::Call},
      // American (r>0 put / q>0 call), including negative opposite-carry corners.
      {90.0, 0.05, -0.02, Side::Put},   {95.0, 0.05, 0.02, Side::Put},
      {100.0, 0.05, 0.05, Side::Put},   {105.0, -0.02, 0.05, Side::Call},
      {105.0, 0.02, 0.05, Side::Call},  {100.0, 0.05, 0.05, Side::Call},
  };
  const double K = 100.0, T = 1.0, sigma = 0.25;
  double max_rel = 0.0;
  int n_compared = 0;
  for (const Cell& c : cells) {
    const double p_al =
        value_or_fail(andersen_lake(c.S, K, T, sigma, c.r, c.q, c.side));
    const double p_pde = oracle_pde_american(c.S, K, T, sigma, c.r, c.q, c.side);
    ASSERT_TRUE(std::isfinite(p_pde));
    if (p_pde > 0.05) {
      max_rel = std::fmax(max_rel, std::fabs(p_al - p_pde) / p_pde);
      ++n_compared;
    }
  }
  EXPECT_GE(n_compared, 12);
  EXPECT_LT(max_rel, 5.0e-3);
}

// Regression: the specific deep-ITM put with q < r < 0 (double-continuation).
// New behavior is an explicit NotImplemented; the OLD guard returned the European
// price, which the PDE oracle shows was wrong by >> $0.005. This test documents
// exactly WHY the guard changed.
TEST(AndersenLakeRegime, UnsupportedPutRegression_OldEuropeanWasWrong) {
  const double S = 70.0, K = 100.0, T = 1.0, sigma = 0.30, r = -0.005, q = -0.02;
  ASSERT_EQ(classify_spec(r, q, Side::Put), Regime::Unsupported);

  const auto res = andersen_lake(S, K, T, sigma, r, q, Side::Put);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::NotImplemented);

  const double euro = euro_put(S, K, T, sigma, r, q);
  const double pde = oracle_pde_american(S, K, T, sigma, r, q, Side::Put);
  ASSERT_TRUE(std::isfinite(pde));
  EXPECT_GT(std::fabs(euro - pde), 0.005);  // the silent European answer was wrong
  EXPECT_GT(pde, euro);                     // early exercise has genuine value here
}

// Global Constraint 1: wherever the corpus lives (r>0 puts / q>0 or European
// calls), the price is BIT-FOR-BIT what the pre-change code produced. Values were
// captured from the pre-change binary and hard-coded here.
TEST(AndersenLakeRegime, PositiveRateGrid_BitIdenticalToPrechange) {
  struct Pin { double S, K, T, sigma, r, q; Side side; double expected; };
  const Pin pins[] = {
      {100.0, 100.0, 1.0, 0.25, 0.03, -0.01, Side::Put, 8.3642096679194555},
      {100.0, 100.0, 1.0, 0.25, 0.03, 0.00, Side::Put, 8.67484861703951},
      {100.0, 100.0, 1.0, 0.25, 0.03, 0.02, Side::Put, 9.3465659527747356},
      {100.0, 100.0, 1.0, 0.25, 0.03, 0.06, Side::Put, 11.013229294069999},
      {100.0, 100.0, 1.0, 0.25, 0.06, 0.02, Side::Put, 8.2133823819523322},
      {80.0, 100.0, 1.0, 0.25, 0.03, 0.02, Side::Put, 21.489057874566694},
      {100.0, 100.0, 1.0, 0.25, 0.03, -0.01, Side::Call, 11.956010735337411},
      {100.0, 100.0, 1.0, 0.25, 0.03, 0.00, Side::Call, 11.348476825143523},
      {100.0, 100.0, 1.0, 0.25, 0.03, 0.02, Side::Call, 10.200496715877067},
      {100.0, 100.0, 1.0, 0.25, 0.03, 0.06, Side::Call, 8.511813671384429},
      {100.0, 100.0, 1.0, 0.25, 0.06, 0.02, Side::Call, 11.602657346692153},
      {120.0, 100.0, 1.0, 0.25, 0.03, 0.02, Side::Call, 23.973643280589464},
  };
  for (const Pin& p : pins) {
    const double got =
        value_or_fail(andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side));
    EXPECT_TRUE(bits_equal(got, p.expected))
        << "r=" << p.r << " q=" << p.q << " side="
        << (p.side == Side::Call ? "C" : "P") << " got=" << got
        << " expected=" << p.expected;
  }
}

// The batched call slice must match the scalar andersen_lake per strike in EVERY
// regime, including returning the SAME Status classification where the scalar
// errors (Unsupported).
TEST(AndersenLakeCallSlice, MatchesScalarPerStrike_AllRegimes) {
  const double S = 100.0, T = 0.5, sigma = 0.30;
  const double strikes[] = {80.0, 90.0, 100.0, 110.0, 120.0};
  const double rq[] = {-0.02, -0.005, 0.0, 0.005, 0.05};
  std::vector<double> out(std::size(strikes), 0.0);
  for (double r : rq)
    for (double q : rq) {
      const auto st = andersen_lake_call_slice(
          S, std::span<const double>(strikes), T, sigma, r, q,
          std::span<double>(out));
      const Regime reg = classify_spec(r, q, Side::Call);
      const std::string where = "r=" + std::to_string(r) + " q=" + std::to_string(q);
      if (reg == Regime::Unsupported) {
        ASSERT_FALSE(st.has_value()) << where;
        EXPECT_EQ(st.error().code(), atx::core::ErrorCode::NotImplemented) << where;
        for (double Kk : strikes) {
          const auto sc = andersen_lake(S, Kk, T, sigma, r, q, Side::Call);
          ASSERT_FALSE(sc.has_value()) << where << " K=" << Kk;
          EXPECT_EQ(sc.error().code(), atx::core::ErrorCode::NotImplemented) << where;
        }
      } else {
        ASSERT_TRUE(st.has_value()) << where << " : " << st.error().to_string();
        for (std::size_t i = 0; i < std::size(strikes); ++i) {
          const double sc = value_or_fail(
              andersen_lake(S, strikes[i], T, sigma, r, q, Side::Call));
          EXPECT_TRUE(bits_equal(out[i], sc))
              << where << " K=" << strikes[i] << " slice=" << out[i]
              << " scalar=" << sc;
        }
      }
    }
}

// McDonald-Schroder symmetry over the corner grid: C(S,K,r,q) == P(K,S,q,r), and
// where one errors BOTH must error with the same code.
TEST(AndersenLakeRegime, CallPutSymmetry_CornerGrid) {
  const double rq[] = {-0.02, -0.005, 0.0, 0.005, 0.05};
  const double S = 110.0, K = 100.0, T = 1.0, sigma = 0.25;
  for (double r : rq)
    for (double q : rq) {
      const auto c = andersen_lake(S, K, T, sigma, r, q, Side::Call);
      const auto p = andersen_lake(K, S, T, sigma, q, r, Side::Put);
      const std::string where = "r=" + std::to_string(r) + " q=" + std::to_string(q);
      ASSERT_EQ(c.has_value(), p.has_value()) << where;
      if (c.has_value()) {
        const double slack = std::fmax(1.0e-5, 1.0e-3 * std::fmax(*c, *p));
        EXPECT_LT(std::fabs(*c - *p), slack) << where;
      } else {
        EXPECT_EQ(c.error().code(), p.error().code()) << where;
      }
    }
}

// The Greeks paths must SURFACE the NotImplemented error in the Unsupported
// regime rather than silently returning a bundle built on a wrong European price;
// the European put regime (r<=0 && r<=q) still returns a bundle (no early ex).
TEST(AmericanGreeksRegime, UnsupportedRegime_PropagatesNotImplemented) {
  const double S = 70.0, K = 100.0, T = 1.0, sigma = 0.30, r = -0.005, q = -0.02;
  ASSERT_EQ(classify_spec(r, q, Side::Put), Regime::Unsupported);

  const auto ga = american_greeks_al(S, K, T, sigma, r, q, Side::Put);
  ASSERT_FALSE(ga.has_value());
  EXPECT_EQ(ga.error().code(), atx::core::ErrorCode::NotImplemented);

  const auto gf = american_greeks_fd(S, K, T, sigma, r, q, Side::Put,
                                     AmericanMethod::AndersenLake, std::nullopt,
                                     /*warm_start=*/false);
  ASSERT_FALSE(gf.has_value());
  EXPECT_EQ(gf.error().code(), atx::core::ErrorCode::NotImplemented);

  const auto d = american_delta(S, K, T, sigma, r, q, Side::Put,
                                AmericanMethod::AndersenLake, std::nullopt);
  ASSERT_FALSE(d.has_value());
  EXPECT_EQ(d.error().code(), atx::core::ErrorCode::NotImplemented);

  // Unsupported CALL (r < q < 0) also errors through the FD routing.
  const auto gc = american_greeks_al(K, S, T, sigma, q, r, Side::Call);
  ASSERT_FALSE(gc.has_value());
  EXPECT_EQ(gc.error().code(), atx::core::ErrorCode::NotImplemented);

  // European put (r<=0 && r<=q): still a valid bundle.
  const auto ge = american_greeks_al(100.0, 100.0, 1.0, 0.30, -0.01, 0.02, Side::Put);
  ASSERT_TRUE(ge.has_value());
}

// Fix-wave 1c: the CorrectionCache Greeks route (`american_greeks`) must ALSO
// surface NotImplemented in the Unsupported regime, not a Black-76+correction
// bundle built on a wrong European price.
TEST(AmericanGreeksRegime, CachedRoute_UnsupportedNotImplemented) {
  const double S = 70.0, K = 100.0, T = 1.0, sigma = 0.30, r = -0.005, q = -0.02;
  ASSERT_EQ(classify_spec(r, q, Side::Put), Regime::Unsupported);  // q < r <= 0
  const auto g = american_greeks(S, K, T, sigma, r, q, Side::Put, nullptr);
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error().code(), atx::core::ErrorCode::NotImplemented);
  // European put regime still returns a valid bundle (no early exercise).
  const auto ge = american_greeks(100.0, 100.0, 1.0, 0.30, -0.01, 0.02, Side::Put, nullptr);
  ASSERT_TRUE(ge.has_value());
}

// Fix-wave 1b: the hot cached price must surface NaN (not a silent number) in the
// Unsupported regime. Null-cache path: it delegates to the cold andersen_lake,
// which now returns NotImplemented -> NaN.
TEST(AmericanPriceCached, UnsupportedRegime_ReturnsNaN) {
  const double S = 70.0, K = 100.0, T = 1.0, sigma = 0.30, r = -0.005, q = -0.02;
  ASSERT_EQ(classify_spec(r, q, Side::Put), Regime::Unsupported);
  EXPECT_TRUE(std::isnan(
      american_price_cached(S, K, T, sigma, r, q, Side::Put, nullptr)));
}

// Fix-wave 1a: the warm-started ALO pricer must surface NaN in the
// double-continuation regime rather than the old silent European price.
TEST(AloPricer, UnsupportedRegime_ReturnsNaN) {
  // Double-continuation put (q < r <= 0).
  {
    const double S = 70.0, K = 100.0, T = 1.0, r = -0.005, q = -0.02, sig = 0.30;
    ASSERT_EQ(classify_spec(r, q, Side::Put), Regime::Unsupported);
    AloPricer pr(S, K, T, r, q, Side::Put);
    EXPECT_TRUE(std::isnan(pr.price(sig)));
    // Degenerate sigma still collapses to intrinsic (K - S) even in this regime.
    EXPECT_NEAR(pr.price(1.0e-12), 30.0, 1.0e-9);
  }
  // Double-continuation call (r < q <= 0), via the internal-put swap.
  {
    const double S = 100.0, K = 70.0, T = 1.0, r = -0.02, q = -0.005, sig = 0.30;
    ASSERT_EQ(classify_spec(r, q, Side::Call), Regime::Unsupported);
    AloPricer pr(S, K, T, r, q, Side::Call);
    EXPECT_TRUE(std::isnan(pr.price(sig)));
  }
}

// Fix-wave 2: BAW is single-boundary, so it returns the SAME NotImplemented as
// andersen_lake in the double-continuation regime (previously untested).
TEST(Baw, UnsupportedRegime_NotImplemented) {
  const double S = 70.0, K = 100.0, T = 1.0, sigma = 0.30, r = -0.005, q = -0.02;
  ASSERT_EQ(classify_spec(r, q, Side::Put), Regime::Unsupported);  // r <= 0 && r > q
  const auto res = baw_american(S, K, T, sigma, r, q, Side::Put);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::NotImplemented);
}

// Fix-wave 3: a non-finite r/q would pass the regime classifier (NaN comparisons
// are false) and leak a NaN price through an Ok result. Every scalar/slice entry
// point must reject it as InvalidArgument. No-op for the finite-input corpus.
TEST(AndersenLake, NonFiniteRateOrYield_IsInvalidArgument) {
  const double S = 100.0, K = 100.0, T = 1.0, sigma = 0.30;
  const double inf = std::numeric_limits<double>::infinity();
  const double bad[] = {std::nan(""), inf, -inf};
  for (const double x : bad) {
    for (const Side side : {Side::Call, Side::Put}) {
      const auto a = andersen_lake(S, K, T, sigma, 0.03, x, side);  // bad q
      ASSERT_FALSE(a.has_value());
      EXPECT_EQ(a.error().code(), atx::core::ErrorCode::InvalidArgument);
      const auto b = andersen_lake(S, K, T, sigma, x, 0.01, side);  // bad r
      ASSERT_FALSE(b.has_value());
      EXPECT_EQ(b.error().code(), atx::core::ErrorCode::InvalidArgument);
    }
    const auto bw = baw_american(S, K, T, sigma, 0.03, x, Side::Put);
    ASSERT_FALSE(bw.has_value());
    EXPECT_EQ(bw.error().code(), atx::core::ErrorCode::InvalidArgument);

    std::vector<double> ks{90.0, 110.0};
    std::vector<double> px(2, 0.0);
    const auto sl = andersen_lake_call_slice(S, std::span<const double>(ks), T,
                                             sigma, 0.03, x, std::span<double>(px));
    ASSERT_FALSE(sl.has_value());
    EXPECT_EQ(sl.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

}  // namespace
