#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
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
using atx::vol::american_vega;
using atx::vol::al_fast_opts;
using atx::vol::american_price_cached;
using atx::vol::andersen_lake;
using atx::vol::andersen_lake_call_slice;
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

TEST(AndersenLake, VsPdeOracle_PutGrid) {
  const double Ss[] = {80.0, 90.0, 100.0, 110.0, 120.0};
  const double sigmas[] = {0.20, 0.30, 0.40};
  const double Ts[] = {0.25, 0.50, 1.00};
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
  EXPECT_GT(n_compared, 30);
  EXPECT_LT(max_rel, 5.0e-3);
}

TEST(AndersenLake, VsPdeOracle_CallGrid) {
  const double Ss[] = {80.0, 90.0, 100.0, 110.0, 120.0};
  const double sigmas[] = {0.20, 0.30, 0.40};
  const double Ts[] = {0.25, 0.50, 1.00};
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
  EXPECT_GT(n_compared, 30);
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

}  // namespace
