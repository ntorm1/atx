#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/detail/adjoint_greeks.hpp"

using atx::vol::american_greeks_fd;
using atx::vol::andersen_lake;
using atx::vol::detail::american_greeks_adjoint;

// WS-P P2 parity gate: the adjoint / implicit-function-theorem greeks kernel vs a
// high-quality central-difference reference. Rung 1 (European, this section) is
// the closed-form BSM adjoint; the American IFT-adjoint parity lands with the
// American arm. Design + citations: atx-vol/docs/adjoint_greeks_design.md.

namespace {

using atx::vol::AmericanGreeks;
using atx::vol::black76_price;
using atx::vol::Side;
using atx::vol::detail::european_greeks_adjoint;

// Independent European price oracle: BSM spot form via Black-76 with F = S·e^{(r-q)T},
// df = e^{-rT}. A different code path from the adjoint reverse sweep.
double euro_price(double S, double K, double T, double sigma, double r, double q, Side side) {
  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  return black76_price(F, K, T, sigma, df, side);
}

struct Pt {
  double S, K, T, sigma, r, q;
};

// A hard European grid: moneyness × maturity × vol × rate/borrow sign flips
// (negative borrow q<0), deep ITM/OTM, near-expiry.
std::vector<Pt> euro_grid() {
  std::vector<Pt> g;
  for (double K : {60.0, 85.0, 100.0, 115.0, 140.0}) {
    for (double T : {0.03, 0.25, 1.0, 2.0}) {
      for (double sigma : {0.12, 0.30, 0.60}) {
        for (double r : {-0.02, 0.0, 0.05}) {
          for (double q : {-0.03, 0.0, 0.04}) {
            g.push_back({100.0, K, T, sigma, r, q});
          }
        }
      }
    }
  }
  return g;
}

TEST(AdjointGreeksEuropean, PriceMatchesBlack76) {
  for (Side side : {Side::Put, Side::Call}) {
    for (const Pt& p : euro_grid()) {
      const AmericanGreeks g = european_greeks_adjoint(p.S, p.K, p.T, p.sigma, p.r, p.q, side);
      const double ref = euro_price(p.S, p.K, p.T, p.sigma, p.r, p.q, side);
      EXPECT_NEAR(g.price, ref, 1.0e-11)
          << "K=" << p.K << " T=" << p.T << " sig=" << p.sigma << " r=" << p.r << " q=" << p.q;
    }
  }
}

// First-order greeks: exact adjoint vs central FD of the independent oracle.
// Richardson-extrapolated central differences reach the analytic value to well
// below these tolerances; the residual is FD truncation only.
TEST(AdjointGreeksEuropean, FirstOrderMatchesCentralFd) {
  auto fd = [](auto f, double h) { return (f(h) - f(-h)) / (2.0 * h); };
  for (Side side : {Side::Put, Side::Call}) {
    for (const Pt& p : euro_grid()) {
      const AmericanGreeks g = european_greeks_adjoint(p.S, p.K, p.T, p.sigma, p.r, p.q, side);
      const std::string at = "K=" + std::to_string(p.K) + " T=" + std::to_string(p.T) +
                             " sig=" + std::to_string(p.sigma) + " r=" + std::to_string(p.r) +
                             " q=" + std::to_string(p.q) + " side=" + (side == Side::Put ? "P" : "C");
      const double delta_fd =
          fd([&](double h) { return euro_price(p.S + h, p.K, p.T, p.sigma, p.r, p.q, side); },
             1.0e-4 * p.S);
      const double vega_fd =
          fd([&](double h) { return euro_price(p.S, p.K, p.T, p.sigma + h, p.r, p.q, side); },
             1.0e-4);
      const double rho_fd =
          fd([&](double h) { return euro_price(p.S, p.K, p.T, p.sigma, p.r + h, p.q, side); },
             1.0e-5);
      // theta calendar = -∂P/∂T
      const double theta_fd =
          -fd([&](double h) { return euro_price(p.S, p.K, p.T + h, p.sigma, p.r, p.q, side); },
              1.0e-4);
      EXPECT_NEAR(g.delta, delta_fd, 1.0e-6) << "delta " << at;
      EXPECT_NEAR(g.vega, vega_fd, 1.0e-5) << "vega " << at;
      EXPECT_NEAR(g.rho, rho_fd, 1.0e-5) << "rho " << at;
      EXPECT_NEAR(g.theta, theta_fd, 1.0e-4) << "theta " << at;
    }
  }
}

TEST(AdjointGreeksEuropean, SecondOrderMatchesCentralFd) {
  for (Side side : {Side::Put, Side::Call}) {
    for (const Pt& p : euro_grid()) {
      const AmericanGreeks g = european_greeks_adjoint(p.S, p.K, p.T, p.sigma, p.r, p.q, side);
      const std::string at = "K=" + std::to_string(p.K) + " T=" + std::to_string(p.T) +
                             " sig=" + std::to_string(p.sigma) + " r=" + std::to_string(p.r) +
                             " q=" + std::to_string(p.q) + " side=" + (side == Side::Put ? "P" : "C");
      // Richardson-extrapolated central references: combine step h and h/2 to
      // cancel the O(h²) truncation (which blows up near-expiry / low-vol where
      // the surface curvature is sharp), leaving the reference at its roundoff
      // plateau. The adjoint closed forms are analytically exact, so this both
      // confirms them and keeps a tight gate.
      auto d2 = [&](auto f, double h) { // ∂²/∂x² central
        auto D = [&](double hh) { return (f(hh) - 2.0 * f(0.0) + f(-hh)) / (hh * hh); };
        return (4.0 * D(0.5 * h) - D(h)) / 3.0;
      };
      auto cross = [&](auto f, double hx, double hy) { // ∂²/∂x∂y central
        auto D = [&](double a, double b) {
          return (f(a, b) - f(-a, b) - f(a, -b) + f(-a, -b)) / (4.0 * a * b);
        };
        return (4.0 * D(0.5 * hx, 0.5 * hy) - D(hx, hy)) / 3.0;
      };
      const double hS = 1.0e-3 * p.S;
      const double hv = 1.0e-3;
      const double hT = 1.0e-3;
      const double gamma_fd =
          d2([&](double h) { return euro_price(p.S + h, p.K, p.T, p.sigma, p.r, p.q, side); }, hS);
      const double volga_fd =
          d2([&](double h) { return euro_price(p.S, p.K, p.T, p.sigma + h, p.r, p.q, side); }, hv);
      const double vanna_fd = cross(
          [&](double a, double b) { return euro_price(p.S + a, p.K, p.T, p.sigma + b, p.r, p.q, side); },
          hS, hv);
      // charm = -∂²P/∂S∂T (calendar delta decay)
      const double charm_fd = -cross(
          [&](double a, double b) { return euro_price(p.S + a, p.K, p.T + b, p.sigma, p.r, p.q, side); },
          hS, hT);
      EXPECT_NEAR(g.gamma, gamma_fd, 1.0e-6 + 1.0e-6 * std::fabs(gamma_fd)) << "gamma " << at;
      EXPECT_NEAR(g.vanna, vanna_fd, 1.0e-5 + 1.0e-5 * std::fabs(vanna_fd)) << "vanna " << at;
      EXPECT_NEAR(g.volga, volga_fd, 1.0e-3 + 1.0e-5 * std::fabs(volga_fd)) << "volga " << at;
      EXPECT_NEAR(g.charm, charm_fd, 1.0e-5 + 1.0e-5 * std::fabs(charm_fd)) << "charm " << at;
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════
// American IFT-adjoint parity (genuine early-exercise puts)
// ══════════════════════════════════════════════════════════════════════════

// The exact function the adjoint differentiates: cold Andersen-Lake, ACCURATE
// preset (nullopt). Bit-identical to the adjoint kernel's internal price path.
double alput(double S, double K, double T, double sigma, double r, double q) {
  const auto p = andersen_lake(S, K, T, sigma, r, q, Side::Put);
  return p.has_value() ? *p : std::nan("");
}

// Richardson-extrapolated central differences of alput() — the high-quality
// reference (cancels O(h²) truncation).
double rich_d1(std::function<double(double)> f, double x, double h) {
  auto D = [&](double hh) { return (f(x + hh) - f(x - hh)) / (2.0 * hh); };
  return (4.0 * D(0.5 * h) - D(h)) / 3.0;
}
double rich_d2(std::function<double(double)> f, double x, double h) {
  auto D = [&](double hh) { return (f(x + hh) - 2.0 * f(x) + f(x - hh)) / (hh * hh); };
  return (4.0 * D(0.5 * h) - D(h)) / 3.0;
}
struct APt {
  double K, T, sigma, r, q;
};

// Genuine early-exercise put grid: moneyness × maturity × vol × rate × borrow
// sign flips (negative q). S = 100 throughout. r > 0 (American regime).
std::vector<APt> amer_grid() {
  std::vector<APt> g;
  for (double K : {75.0, 90.0, 100.0, 110.0, 125.0}) {
    for (double T : {0.08, 0.5, 1.5}) {
      for (double sigma : {0.15, 0.35}) {
        for (double r : {0.04, 0.09}) {
          for (double q : {-0.02, 0.03}) {
            g.push_back({K, T, sigma, r, q});
          }
        }
      }
    }
  }
  return g;
}

TEST(AdjointGreeksAmerican, PriceMatchesAndersenLake) {
  const double S = 100.0;
  for (const APt& p : amer_grid()) {
    const auto g = american_greeks_adjoint(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    ASSERT_TRUE(g.has_value());
    const double ref = alput(S, p.K, p.T, p.sigma, p.r, p.q);
    EXPECT_NEAR(g->price, ref, 1.0e-9) << "K=" << p.K << " T=" << p.T << " sig=" << p.sigma
                                       << " r=" << p.r << " q=" << p.q;
  }
}

// delta/gamma are bit-identical to the FD bundle on the ENTIRE grid (incl.
// negative borrow): the put exercise boundary is spot-independent, so both paths
// price the same base boundary with the same hS=1e-3·S stencils. This is the
// hedge-parity guarantee B2/B3 rely on.
TEST(AdjointGreeksAmerican, DeltaGammaBitIdenticalToFd) {
  const double S = 100.0;
  for (const APt& p : amer_grid()) {
    const auto g = american_greeks_adjoint(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    const auto fd = american_greeks_fd(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    ASSERT_TRUE(g.has_value());
    ASSERT_TRUE(fd.has_value());
    const std::string at = "K=" + std::to_string(p.K) + " T=" + std::to_string(p.T) +
                           " sig=" + std::to_string(p.sigma) + " r=" + std::to_string(p.r) +
                           " q=" + std::to_string(p.q);
    EXPECT_NEAR(g->price, fd->price, 1.0e-9) << "price " << at;
    EXPECT_NEAR(g->delta, fd->delta, 1.0e-9) << "delta " << at;
    EXPECT_NEAR(g->gamma, fd->gamma, 1.0e-8) << "gamma " << at;
  }
}

// WHERE THE CHRISTIANSON ADJOINT PATH RUNS, its FIRST-ORDER greeks (the hedge-critical
// delta/vega/rho/theta) match the budget-limited MARK derivative on the WHOLE wide
// domain — because through-iterations differentiates the actual iteration the pricer
// ran, not the exact fixed point. Reference: high-quality Richardson central-difference
// of the ACCURATE-preset andersen_lake (the mark). Tolerances are two-tier: tight on the
// realistic grid (T ≤ 0.5, the backtest hot path), economic on the adversarial long-T
// low-σ corners where even Richardson vs al disagree ~5% (the mark itself is noisy).
// volga (the noisy 2nd-order greek) is gated only on the smooth subgrid.
TEST(AdjointGreeksAmerican, AdjointPathAccuracyVsMark) {
  const double S = 100.0;
  int took_n = 0;
  for (double K : {80.0, 88.0, 96.0, 100.0, 104.0, 108.0, 112.0}) {
    for (double T : {0.1, 0.4, 1.0}) {
      for (double sigma : {0.15, 0.25, 0.4}) {
        for (double r : {0.03, 0.06, 0.10}) {
          const double q = 0.0;
          bool took = false;
          const auto g =
              american_greeks_adjoint(S, K, T, sigma, r, q, Side::Put, std::nullopt, &took);
          ASSERT_TRUE(g.has_value());
          if (!took) {
            continue; // fell back to fd (loss-free: delta/gamma bit-identical) — not the adjoint
          }
          ++took_n;
          const bool realistic = (T <= 0.5); // the backtest hot path (short-to-medium T)
          const std::string at = "K=" + std::to_string(K) + " T=" + std::to_string(T) +
                                 " sig=" + std::to_string(sigma) + " r=" + std::to_string(r);
          const double vega_ref =
              rich_d1([&](double v) { return alput(S, K, T, v, r, q); }, sigma, 1.0e-3);
          const double rho_ref =
              rich_d1([&](double rr) { return alput(S, K, T, sigma, rr, q); }, r, 1.0e-3);
          const auto fd = american_greeks_fd(S, K, T, sigma, r, q, Side::Put);
          ASSERT_TRUE(fd.has_value());
          // FIRST-ORDER (hedge-critical): tight on realistic, economic on adversarial.
          const double vtol = realistic ? (5.0e-3 + 3.0e-3 * std::fabs(vega_ref))
                                        : (1.0e-2 + 3.0e-2 * std::fabs(vega_ref));
          const double rtol = realistic ? (5.0e-3 + 3.0e-3 * std::fabs(rho_ref))
                                        : (1.0e-2 + 3.0e-2 * std::fabs(rho_ref));
          EXPECT_NEAR(g->vega, vega_ref, vtol) << "vega " << at;
          EXPECT_NEAR(g->rho, rho_ref, rtol) << "rho " << at;
          EXPECT_NEAR(g->gamma, fd->gamma, 1.0e-8) << "gamma " << at; // spot-independent boundary
          // SECOND-ORDER (vanna, volga): 2nd-order-accurate only, and gated only on the
          // SMOOTH realistic grid. At the adversarial long-T low-σ corners the mark's 2nd
          // σ-derivative is genuinely noise (fd's own vanna/volga flip sign there), so these
          // greeks are not claimed — the hedge-critical first-order greeks above are.
          if (realistic) {
            EXPECT_NEAR(g->vanna, fd->vanna, 5.0e-2 + 5.0e-2 * std::fabs(fd->vanna)) << "vanna " << at;
            const double volga_ref =
                rich_d2([&](double v) { return alput(S, K, T, v, r, q); }, sigma, 3.0e-3);
            EXPECT_NEAR(g->volga, volga_ref, 2.0e0 + 1.0e-1 * std::fabs(volga_ref)) << "volga " << at;
          }
        }
      }
    }
  }
  EXPECT_GT(took_n, 100) << "Christianson adjoint fires on the DOMINANT share of the grid";
}

// Negative borrow (q < 0): the reliable greeks (price/delta/gamma/rho) match fd.
// vega/vanna/volga are NOT gated here — the low-σ/long-T/neg-carry corner is a
// boundary-solver-instability region where fd is itself unreliable; the adjoint's
// self-consistency guard hands the worst points to fd, and delta/gamma stay exact.
TEST(AdjointGreeksAmerican, NegativeBorrowReliable) {
  const double S = 100.0;
  int checked = 0;
  for (const APt& p : amer_grid()) {
    if (p.q >= 0.0) {
      continue;
    }
    const auto g = american_greeks_adjoint(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    const auto fd = american_greeks_fd(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    ASSERT_TRUE(g.has_value());
    ASSERT_TRUE(fd.has_value());
    const std::string at = "K=" + std::to_string(p.K) + " T=" + std::to_string(p.T) +
                           " sig=" + std::to_string(p.sigma) + " r=" + std::to_string(p.r) +
                           " q=" + std::to_string(p.q);
    EXPECT_NEAR(g->price, fd->price, 1.0e-9) << "price " << at;
    EXPECT_NEAR(g->delta, fd->delta, 1.0e-9) << "delta " << at;
    EXPECT_NEAR(g->gamma, fd->gamma, 1.0e-8) << "gamma " << at;
    EXPECT_NEAR(g->rho, fd->rho, 5.0e-2 + 5.0e-3 * std::fabs(fd->rho)) << "rho " << at;
    ++checked;
  }
  EXPECT_GT(checked, 20);
}

// DOMAIN-WIDTH gate (P3-pre, the primary Amdahl lever): Christianson through-iterations
// differentiates the actual budget-limited solve, so the adjoint fast-lane now fires on
// the DOMINANT share of the grid (vs the ~1/12 the P2 fixed-point IFT claimed) — that is
// what lifts the portfolio-greeks speedup off the ~1.09× Amdahl cap. Fallback stays
// loss-free (delta/gamma bit-identical whichever path runs).
TEST(AdjointGreeksAmerican, AdjointDomainWideAndSafe) {
  const double S = 100.0;
  int took_n = 0, total = 0;
  for (double K : {80.0, 88.0, 96.0, 100.0, 104.0, 108.0, 112.0}) {
    for (double T : {0.1, 0.4, 1.0}) {
      for (double sigma : {0.15, 0.25, 0.4}) {
        for (double r : {0.03, 0.06, 0.10}) {
          const double q = 0.0;
          bool took = false;
          const auto g =
              american_greeks_adjoint(S, K, T, sigma, r, q, Side::Put, std::nullopt, &took);
          const auto fd = american_greeks_fd(S, K, T, sigma, r, q, Side::Put);
          ASSERT_TRUE(g.has_value());
          ASSERT_TRUE(fd.has_value());
          ++total;
          took_n += took ? 1 : 0;
          // Loss-free regardless of path: delta/gamma bit-identical to fd.
          EXPECT_NEAR(g->delta, fd->delta, 1.0e-9);
          EXPECT_NEAR(g->gamma, fd->gamma, 1.0e-8);
        }
      }
    }
  }
  // Christianson widens the domain to the majority (the whole point). The lower bound
  // trips if a change silently re-narrows it; the fallback (< total) is kept for the
  // genuinely-unstable corners the self-consistency guard hands to fd.
  EXPECT_GT(took_n, (3 * total) / 4) << "Christianson adjoint must fire on the dominant share";
  EXPECT_LE(took_n, total);
}

// P3-pre GATE: on a REALISTIC backtest-shaped put grid (moneyness 0.85–1.15, T up to
// 6M, σ/r/q of liquid single names), the Christianson adjoint (a) fires on the DOMINANT
// share (the Amdahl lever — was ~14% for the P2 fixed-point IFT), (b) keeps the mark
// bit-identical to the production andersen_lake pricer, and (c) matches the mark's
// vega/rho to economic tolerance. This is the primary domain-widening acceptance gate.
TEST(AdjointGreeksAmerican, RealisticGridWideMarkExactParity) {
  const double S = 100.0;
  int took_n = 0, total = 0, vega_ok = 0;
  for (double mny : {0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.15}) {
    const double K = S / mny;
    for (double T : {0.05, 0.08, 0.12, 0.25, 0.5}) {
      for (double sigma : {0.15, 0.22, 0.30, 0.40}) {
        for (double r : {0.03, 0.045}) {
          for (double q : {0.0, 0.015}) {
            if (atx::vol::detail::classify_regime(r, q) != atx::vol::detail::ExerciseRegime::American)
              continue;
            bool took = false;
            const auto g = american_greeks_adjoint(S, K, T, sigma, r, q, Side::Put, std::nullopt, &took);
            const auto fd = american_greeks_fd(S, K, T, sigma, r, q, Side::Put);
            ASSERT_TRUE(g.has_value());
            ASSERT_TRUE(fd.has_value());
            ++total;
            // (b) mark bit-identical + (delta/gamma loss-free) whichever path runs.
            EXPECT_NEAR(g->price, fd->price, 1.0e-9);
            EXPECT_NEAR(g->delta, fd->delta, 1.0e-9);
            EXPECT_NEAR(g->gamma, fd->gamma, 1.0e-8);
            if (!took) continue;
            ++took_n;
            // (c) vega vs Richardson of the mark, economic.
            const double vega_ref =
                rich_d1([&](double v) { return alput(S, K, T, v, r, q); }, sigma, 1.0e-3);
            if (std::fabs(g->vega - vega_ref) <= 1.0e-2 + 5.0e-3 * std::fabs(vega_ref)) ++vega_ok;
          }
        }
      }
    }
  }
  // (a) dominant engagement — the domain-widening headline (measured ≈ 83%).
  EXPECT_GT(took_n, (3 * total) / 4) << "engagement=" << took_n << "/" << total;
  EXPECT_GT(vega_ok, (95 * took_n) / 100) << "vega economic parity on ≥95% of adjoint-path points";
}

// I-3: pin the IFT boundary correction (-λ^T R_σ) where it is MATERIAL. Near
// expiry with a high rate, the early-exercise boundary moves sharply with σ, so
// the American vega departs materially from the European (no-early-exercise) vega
// — the −λ^T R_σ correction is doing real work. Matching Richardson THERE proves
// the correction's sign and scale (a wrong correction could not match the true
// American vega). Only points the adjoint path actually claims are asserted.
TEST(AdjointGreeksAmerican, IftCorrectionMaterialAndCorrect) {
  const double S = 100.0;
  int material = 0;
  for (double K : {92.0, 96.0, 100.0, 104.0}) {
    for (double T : {0.1, 0.15, 0.2}) {
      for (double sigma : {0.15, 0.2}) {
        for (double r : {0.08, 0.12}) {
          const double q = 0.0;
          bool took = false;
          const auto g = american_greeks_adjoint(S, K, T, sigma, r, q, Side::Put, std::nullopt,
                                                 &took);
          if (!g.has_value() || !took) {
            continue; // only points the genuine adjoint path claims
          }
          const std::string at = "K=" + std::to_string(K) + " T=" + std::to_string(T) +
                                 " sig=" + std::to_string(sigma) + " r=" + std::to_string(r);
          const double vega_ref =
              rich_d1([&](double v) { return alput(S, K, T, v, r, q); }, sigma, 1.0e-3);
          const double rho_ref =
              rich_d1([&](double rr) { return alput(S, K, T, sigma, rr, q); }, r, 1.0e-3);
          const double vega_euro =
              european_greeks_adjoint(S, K, T, sigma, r, q, Side::Put).vega;
          // Materiality: the American vega must depart from the European vega
          // (the early-exercise boundary genuinely moves the vega here).
          if (std::fabs(vega_ref - vega_euro) > 0.02 * std::fabs(vega_ref)) {
            ++material;
          }
          // Correctness of the correction: adjoint == true American vega. Gated
          // to the near-boundary ITM plateau (documented, report parity table).
          EXPECT_NEAR(g->vega, vega_ref, 3.0e-3 + 2.0e-3 * std::fabs(vega_ref)) << "vega " << at;
          EXPECT_NEAR(g->rho, rho_ref, 3.0e-3 + 2.0e-3 * std::fabs(rho_ref)) << "rho " << at;
        }
      }
    }
  }
  EXPECT_GT(material, 1) << "expected points with a material boundary correction";
}

// M-1: genuine near-expiry (T≈days) coverage, so the disclosed near-expiry
// theta/charm degradation (PDE identity, ~few %) is measured in-grid, not merely
// asserted. delta/gamma stay bit-identical to fd even at T≈days.
TEST(AdjointGreeksAmerican, NearExpiryThetaCharm) {
  const double S = 100.0;
  int checked = 0;
  for (double K : {96.0, 100.0, 104.0}) {
    for (double T : {0.01, 0.02, 0.04}) { // ~2.5 / 5 / 10 trading days
      for (double sigma : {0.2, 0.4}) {
        const double r = 0.06, q = 0.0;
        bool took = false;
        const auto g = american_greeks_adjoint(S, K, T, sigma, r, q, Side::Put, std::nullopt, &took);
        const auto fd = american_greeks_fd(S, K, T, sigma, r, q, Side::Put);
        ASSERT_TRUE(g.has_value());
        ASSERT_TRUE(fd.has_value());
        if (!took) {
          continue; // handed to fd (straddle / unconverged) — trivially equal
        }
        const std::string at = "K=" + std::to_string(K) + " T=" + std::to_string(T) +
                               " sig=" + std::to_string(sigma);
        EXPECT_NEAR(g->delta, fd->delta, 1.0e-9) << "delta " << at; // still bit-identical
        EXPECT_NEAR(g->gamma, fd->gamma, 1.0e-8) << "gamma " << at;
        // theta/charm PDE identity degrades near expiry (large speed); bound it.
        EXPECT_NEAR(g->theta, fd->theta, 1.0e-1 + 5.0e-2 * std::fabs(fd->theta)) << "theta " << at;
        EXPECT_NEAR(g->charm, fd->charm, 2.0e-1 + 5.0e-2 * std::fabs(fd->charm)) << "charm " << at;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 6);
}

// Diagnostic (non-gating): print the max per-greek gap vs the FD bundle and vs
// Richardson across the grid, with argmax, to characterise the achieved tolerance.
TEST(AdjointGreeksAmerican, DiagnosticGaps) {
  const double S = 100.0;
  struct MG {
    double v = 0;
    std::string at;
  };
  MG dvega, drho, dgamma, ddelta, dvanna, dvolga;
  auto upd = [](MG& m, double d, const std::string& at) {
    if (std::fabs(d) > m.v) {
      m.v = std::fabs(d);
      m.at = at;
    }
  };
  int n_adjoint = 0, n_total = 0, n_material = 0;
  double vega_gap_R = 0.0, volga_gap_R = 0.0;
  std::string vega_at_R, volga_at_R, material_at;
  for (double K : {80.0, 88.0, 96.0, 100.0, 104.0, 108.0, 112.0}) {
    for (double T : {0.1, 0.4, 1.0}) {
      for (double sigma : {0.15, 0.25, 0.4}) {
        for (double r : {0.03, 0.06, 0.10}) {
          const double q = 0.0;
          bool took = false;
          const auto g = american_greeks_adjoint(S, K, T, sigma, r, q, Side::Put, std::nullopt,
                                                 &took);
          const auto fd = american_greeks_fd(S, K, T, sigma, r, q, Side::Put);
          if (!g.has_value() || !fd.has_value()) {
            continue;
          }
          ++n_total;
          const std::string at = "K=" + std::to_string((int)K) + " T=" + std::to_string(T) +
                                 " sig=" + std::to_string(sigma) + " r=" + std::to_string(r);
          upd(ddelta, g->delta - fd->delta, at);
          upd(dgamma, g->gamma - fd->gamma, at);
          if (!took) {
            continue;
          }
          ++n_adjoint;
          const double vega_ref = rich_d1([&](double v) { return alput(S, K, T, v, r, q); }, sigma,
                                          1.0e-3);
          const double volga_ref = rich_d2([&](double v) { return alput(S, K, T, v, r, q); }, sigma,
                                           3.0e-3);
          const double vega_euro = european_greeks_adjoint(S, K, T, sigma, r, q, Side::Put).vega;
          if (std::fabs(g->vega - vega_ref) > vega_gap_R) {
            vega_gap_R = std::fabs(g->vega - vega_ref);
            vega_at_R = at;
          }
          if (std::fabs(g->volga - volga_ref) > volga_gap_R) {
            volga_gap_R = std::fabs(g->volga - volga_ref);
            volga_at_R = at;
          }
          const double corr = std::fabs(vega_ref - vega_euro);
          if (corr > 0.02 * std::fabs(vega_ref)) {
            ++n_material;
            if (material_at.empty()) {
              material_at = at + " corr=" + std::to_string(corr);
            }
          }
        }
      }
    }
  }
  std::cout << "[adjoint coverage] took=" << n_adjoint << "/" << n_total
            << "  vega_gap_vsR=" << vega_gap_R << " (" << vega_at_R << ")"
            << "  volga_gap_vsR=" << volga_gap_R << " (" << volga_at_R << ")"
            << "  n_material_corr=" << n_material << " (" << material_at << ")\n";
  std::cout << "[gaps vs fd, full grid] delta=" << ddelta.v << " gamma=" << dgamma.v << "\n";
  SUCCEED();
}

// Drop-in contract: every regime the adjoint does not claim (calls, European-exact
// put, degenerate, negative-carry) returns exactly american_greeks_fd.
TEST(AdjointGreeksAmerican, FallbackMatchesFd) {
  const double S = 100.0;
  struct FB {
    double K, T, sigma, r, q;
    Side side;
  };
  const std::array<FB, 5> cases = {{
      {100.0, 0.5, 0.3, 0.05, 0.02, Side::Call},   // call -> FD
      {100.0, 0.5, 0.3, -0.01, 0.02, Side::Put},   // European-exact put (r<=0,r<=q)
      {100.0, 0.5, 0.3, 0.05, 0.10, Side::Put},    // American put but deep in continuation
      {110.0, 0.02, 0.25, 0.05, 0.0, Side::Call},  // near-expiry call
      {100.0, 0.5, 0.3, -0.02, -0.05, Side::Put},  // negative-carry / double-continuation corner
  }};
  for (const FB& c : cases) {
    const auto g = american_greeks_adjoint(S, c.K, c.T, c.sigma, c.r, c.q, c.side);
    const auto fd = american_greeks_fd(S, c.K, c.T, c.sigma, c.r, c.q, c.side);
    ASSERT_EQ(g.has_value(), fd.has_value());
    if (!g.has_value()) {
      continue;
    }
    // European-exact put uses the closed-form adjoint (== American == European);
    // all other fallbacks are the literal FD bundle. Both are economically equal
    // to the FD reference within its plateau.
    EXPECT_NEAR(g->price, fd->price, 1.0e-6) << "K=" << c.K;
    EXPECT_NEAR(g->delta, fd->delta, 1.0e-4);
    EXPECT_NEAR(g->vega, fd->vega, 1.0e-3);
  }
}

// Informal perf sanity (DISABLED — run with --gtest_also_run_disabled_tests on a
// rel-avx2 quiet host). NOT an official number: the sprint's headline throughput
// lands in wave-2 P5 against the measure agent's baseline. Best-of-3, genuine
// early-exercise puts (the adjoint path). Prices delta+full greeks per solve.
TEST(AdjointGreeksAmerican, DISABLED_PerfSanity) {
  const double S = 100.0;
  std::vector<APt> pts;
  for (double K : {90.0, 95.0, 100.0, 105.0}) {
    for (double T : {0.1, 0.5, 1.0}) {
      for (double sigma : {0.2, 0.35}) {
        pts.push_back({K, T, sigma, 0.05, 0.01});
      }
    }
  }
  const int reps = 200;
  auto bench = [&](auto fn) {
    double best = 1e300;
    for (int t = 0; t < 3; ++t) {
      const auto t0 = std::chrono::steady_clock::now();
      double acc = 0.0;
      for (int r = 0; r < reps; ++r) {
        for (const APt& p : pts) {
          acc += fn(p);
        }
      }
      const auto t1 = std::chrono::steady_clock::now();
      volatile double sink = acc;
      (void)sink;
      const double s = std::chrono::duration<double>(t1 - t0).count();
      best = std::min(best, s);
    }
    return static_cast<double>(reps * (int)pts.size()) / best; // items/s
  };
  const double aad = bench([&](const APt& p) {
    const auto g = american_greeks_adjoint(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    return g.has_value() ? g->vega + g->delta : 0.0;
  });
  const double fdw = bench([&](const APt& p) {
    const auto g = american_greeks_fd(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put,
                                      atx::vol::AmericanMethod::AndersenLake, std::nullopt, true);
    return g.has_value() ? g->vega + g->delta : 0.0;
  });
  // BOTH SCOPES (the PM holds the >=5x gate scope decision).
  //  * FULL 8-greek bundle: aad (all 8 in one taped solve + reverse tangent) vs
  //    fd_warm (the full FD bundle). This is the row the portfolio adjoint A/B uses.
  //  * FIRST-ORDER delta+vega: the adjoint kernel ALWAYS computes the full bundle
  //    (no first_order_only fast path this wave), so its first-order cost == aad; the
  //    FD first-order composite is the delta-only + vega-only fast paths
  //    (american_delta + american_vega_al, ~2 boundary solves each). This exposes
  //    that a delta+vega-only need is cheaper via the FD fast paths than via the full
  //    adjoint kernel — a first_order_only adjoint (skip vanna/volga) is the lever.
  const double fd_first = bench([&](const APt& p) {
    const auto d = atx::vol::american_delta(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    const auto v = atx::vol::american_vega_al(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    return (d.has_value() ? *d : 0.0) + (v.has_value() ? *v : 0.0);
  });
  std::cout << "[perf sanity] FULL 8-greek: aad=" << aad << " items/s  fd_warm=" << fdw
            << " items/s  speedup=" << (aad / fdw) << "x\n";
  std::cout << "[perf sanity] FIRST-ORDER delta+vega: aad(full kernel)=" << aad
            << " items/s  fd_first(delta+vega_al)=" << fd_first
            << " items/s  speedup=" << (aad / fd_first) << "x\n";
  SUCCEED();
}

// P3-pre DECISION DIAGNOSTIC (DISABLED). Measures, on a realistic backtest-shaped
// put grid, (a) the fixed-point-IFT-domain engagement fraction (took==true), and
// (b) how far the fixed-point IFT vega (al_implicit_diff_put_greeks — differentiates
// the EXACT fixed point y_fp) is from the budget-limited MARK vega (rich_d1 of the
// ACCURATE-preset andersen_lake — differentiates y*(θ) the solver actually returns),
// split by convergence. Answers: can we widen by simply loosening the ‖R‖ guard, or
// must we differentiate through the iterations (Christianson)?
TEST(AdjointGreeksAmerican, DISABLED_DomainRealityScan) {
  using atx::vol::detail::al_implicit_diff_put_greeks;
  const double S = 100.0;
  int n_total = 0, n_took = 0;
  // through-iters (american_greeks_adjoint) vs the two mark estimates, RELATIVE gaps.
  std::vector<double> ti_vs_al, ti_vs_rich, al_vs_rich;
  for (double m : {0.80, 0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.15, 1.20}) {
    const double K = S / m; // spot-moneyness m = S/K
    for (double T : {0.02, 0.05, 0.08, 0.12, 0.17, 0.25, 0.35, 0.5}) {
      for (double sigma : {0.12, 0.18, 0.25, 0.32, 0.45}) {
        for (double r : {0.03, 0.045, 0.05}) {
          for (double q : {0.0, 0.012, 0.025}) {
            if (atx::vol::detail::classify_regime(r, q) != atx::vol::detail::ExerciseRegime::American) {
              continue;
            }
            bool took = false;
            const auto g = american_greeks_adjoint(S, K, T, sigma, r, q, Side::Put, std::nullopt, &took);
            if (!g.has_value()) {
              continue;
            }
            ++n_total;
            n_took += took ? 1 : 0;
            const double rich = rich_d1([&](double v) { return alput(S, K, T, v, r, q); }, sigma, 1.0e-3);
            const double denom = std::fabs(rich) + 1.0;
            const auto al = atx::vol::american_greeks_al(S, K, T, sigma, r, q, Side::Put);
            if (took && al.has_value()) {
              ti_vs_al.push_back(std::fabs(g->vega - al->vega) / denom);
              ti_vs_rich.push_back(std::fabs(g->vega - rich) / denom);
              al_vs_rich.push_back(std::fabs(al->vega - rich) / denom);
            }
          }
        }
      }
    }
  }
  auto pct = [](std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * v.size()))];
  };
  std::cout << "[domain reality] realistic put grid N=" << n_total
            << "  through-iters engagement took=" << n_took << "/" << n_total << " ("
            << (100.0 * n_took / std::max(1, n_total)) << "%)\n";
  std::cout << "  through-iters vega vs al   : median=" << pct(ti_vs_al, 0.5)
            << " p90=" << pct(ti_vs_al, 0.9) << " p99=" << pct(ti_vs_al, 0.99)
            << " max=" << pct(ti_vs_al, 1.0) << " n=" << ti_vs_al.size() << "\n";
  std::cout << "  through-iters vega vs rich : median=" << pct(ti_vs_rich, 0.5)
            << " p90=" << pct(ti_vs_rich, 0.9) << " p99=" << pct(ti_vs_rich, 0.99)
            << " max=" << pct(ti_vs_rich, 1.0) << "\n";
  std::cout << "  al vega vs rich (mark self-noise): median=" << pct(al_vs_rich, 0.5)
            << " p90=" << pct(al_vs_rich, 0.9) << " max=" << pct(al_vs_rich, 1.0) << "\n";
  SUCCEED();
}

TEST(AdjointGreeksAmerican, InvalidArgument) {
  EXPECT_FALSE(american_greeks_adjoint(-1.0, 100.0, 0.5, 0.3, 0.05, 0.0, Side::Put).has_value());
  EXPECT_FALSE(american_greeks_adjoint(100.0, 100.0, -0.5, 0.3, 0.05, 0.0, Side::Put).has_value());
  EXPECT_FALSE(american_greeks_adjoint(100.0, 100.0, 0.5, 0.0, 0.05, 0.0, Side::Put).has_value());
}

} // namespace
