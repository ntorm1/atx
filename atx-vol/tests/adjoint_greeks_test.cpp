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

// WHERE THE ADJOINT PATH ACTUALLY RUNS, its greeks match the true derivative.
// The IFT computes the fixed-point derivative; it agrees with the actual (loose,
// budget-limited ACCURATE-preset) andersen_lake pricer only where the boundary is
// well-converged, which the ‖R(y0)‖≤1e-7 guard selects (a MINORITY of points — see
// AdjointDomainNarrowButSafe). This test gates ONLY those took==true points vs
// Richardson AND vs the validated forward-IFT spike; every fallback point is
// exercised by DeltaGammaBitIdenticalToFd / FallbackMatchesFd instead.
TEST(AdjointGreeksAmerican, AdjointPathAccuracyVsRichardson) {
  const double S = 100.0;
  int took_n = 0, spike_checked = 0;
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
            continue; // fell back to fd (see AdjointDomainNarrowButSafe) — not the adjoint
          }
          ++took_n;
          const std::string at = "K=" + std::to_string(K) + " T=" + std::to_string(T) +
                                 " sig=" + std::to_string(sigma) + " r=" + std::to_string(r);
          const double vega_ref =
              rich_d1([&](double v) { return alput(S, K, T, v, r, q); }, sigma, 1.0e-3);
          const double rho_ref =
              rich_d1([&](double rr) { return alput(S, K, T, sigma, rr, q); }, r, 1.0e-3);
          const double volga_ref =
              rich_d2([&](double v) { return alput(S, K, T, v, r, q); }, sigma, 3.0e-3);
          const auto fd = american_greeks_fd(S, K, T, sigma, r, q, Side::Put);
          ASSERT_TRUE(fd.has_value());
          // First-order σ/r sensitivities have clean Richardson references — gate
          // vega/rho tightly there. gamma/vanna are S-derivatives whose Richardson
          // reference is unreliable at near-expiry (sharp S-curvature vs a cold-solve
          // step floor), so gate them vs fd (same-family method): gamma is
          // bit-identical (spot-independent boundary), vanna within the 2nd-order
          // plateau. volga (cold-re-solve 2nd diff) is 2nd-order-accurate only.
          EXPECT_NEAR(g->vega, vega_ref, 5.0e-3 + 2.0e-3 * std::fabs(vega_ref)) << "vega " << at;
          EXPECT_NEAR(g->rho, rho_ref, 5.0e-3 + 2.0e-3 * std::fabs(rho_ref)) << "rho " << at;
          EXPECT_NEAR(g->gamma, fd->gamma, 1.0e-8) << "gamma " << at;
          EXPECT_NEAR(g->vanna, fd->vanna, 5.0e-2 + 5.0e-2 * std::fabs(fd->vanna)) << "vanna " << at;
          EXPECT_NEAR(g->volga, volga_ref, 2.0e0 + 5.0e-2 * std::fabs(volga_ref)) << "volga " << at;
          // Independent correctness anchor: the reverse-IFT adjoint must equal the
          // VALIDATED forward-IFT spike (al_implicit_diff_put_greeks) — a reference
          // that is not Richardson and not the code under test.
          double jerr = 0.0;
          const auto sp = atx::vol::detail::al_implicit_diff_put_greeks(S, K, T, sigma, r, q,
                                                                        std::nullopt, false, jerr);
          if (sp.ok) {
            EXPECT_NEAR(g->vega, sp.vega, 1.0e-3 + 1.0e-3 * std::fabs(sp.vega))
                << "vega vs forward-IFT spike " << at;
            EXPECT_NEAR(g->delta, sp.delta, 1.0e-6) << "delta vs spike " << at;
            ++spike_checked;
          }
        }
      }
    }
  }
  EXPECT_GT(took_n, 10) << "adjoint path must fire on a non-trivial number of points";
  EXPECT_GT(spike_checked, 8);
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

// Honesty gate on the DOMAIN: the IFT-adjoint fast-lane fires only where the
// budget-limited ACCURATE-preset boundary is well-converged (‖R(y0)‖≤1e-7) — a
// MINORITY of the grid — and safely hands every other point to the exact fd
// bundle. This test documents that split as a hard fact and confirms the fallback
// is loss-free (delta/gamma stay bit-identical whichever path runs).
TEST(AdjointGreeksAmerican, AdjointDomainNarrowButSafe) {
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
  // Documented fact: the adjoint fires on a minority (~1/12 with the ACCURATE
  // preset's 6-sweep budget) and falls back elsewhere. Both bounds are asserted so
  // a future change that silently widens/collapses the domain trips this test.
  EXPECT_GT(took_n, 8) << "adjoint should fire on the well-converged subset";
  EXPECT_LT(took_n, total) << "and must fall back on the under-converged majority";
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
  std::cout << "[perf sanity] american_greeks: aad=" << aad << " items/s  fd_warm=" << fdw
            << " items/s  speedup=" << (aad / fdw) << "x\n";
  SUCCEED();
}

TEST(AdjointGreeksAmerican, InvalidArgument) {
  EXPECT_FALSE(american_greeks_adjoint(-1.0, 100.0, 0.5, 0.3, 0.05, 0.0, Side::Put).has_value());
  EXPECT_FALSE(american_greeks_adjoint(100.0, 100.0, -0.5, 0.3, 0.05, 0.0, Side::Put).has_value());
  EXPECT_FALSE(american_greeks_adjoint(100.0, 100.0, 0.5, 0.0, 0.05, 0.0, Side::Put).has_value());
}

} // namespace
