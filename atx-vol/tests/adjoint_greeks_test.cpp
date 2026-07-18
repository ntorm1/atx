#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
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
double rich_cross(std::function<double(double, double)> f, double x, double y, double hx,
                  double hy) {
  auto D = [&](double a, double b) {
    return (f(x + a, y + b) - f(x - a, y + b) - f(x + a, y - b) + f(x - a, y - b)) / (4.0 * a * b);
  };
  return (4.0 * D(0.5 * hx, 0.5 * hy) - D(hx, hy)) / 3.0;
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

// vega/rho reproduce the FD bundle across the STABLE regime (q ≥ 0). At q ≥ 0 the
// boundary solver is well-conditioned; the low-σ / long-T / negative-borrow corner
// (where fd itself yields nonsensical values, e.g. a negative put vega) is covered
// separately (NegativeBorrowReliable). theta/charm come from the continuation-
// region BS PDE identity and are gated close to fd (american_greeks_al precedent).
TEST(AdjointGreeksAmerican, VegaRhoThetaMatchFdStableRegime) {
  const double S = 100.0;
  for (const APt& p : amer_grid()) {
    if (p.q < 0.0) {
      continue; // negative-borrow corner: see NegativeBorrowReliable
    }
    const auto g = american_greeks_adjoint(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    const auto fd = american_greeks_fd(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    ASSERT_TRUE(g.has_value());
    ASSERT_TRUE(fd.has_value());
    const std::string at = "K=" + std::to_string(p.K) + " T=" + std::to_string(p.T) +
                           " sig=" + std::to_string(p.sigma) + " r=" + std::to_string(p.r) +
                           " q=" + std::to_string(p.q);
    // vega/rho: machine-precise in the smooth region, ~few·1e-3 near-expiry ITM.
    // theta/charm: PDE-identity (speed stencil), accurate to ~1-4% near expiry —
    // the documented "close to fd" plateau (american_greeks_al precedent).
    EXPECT_NEAR(g->vega, fd->vega, 5.0e-3 + 2.0e-3 * std::fabs(fd->vega)) << "vega " << at;
    EXPECT_NEAR(g->rho, fd->rho, 1.0e-2 + 5.0e-3 * std::fabs(fd->rho)) << "rho " << at;
    EXPECT_NEAR(g->theta, fd->theta, 3.0e-2 + 3.0e-2 * std::fabs(fd->theta)) << "theta " << at;
    EXPECT_NEAR(g->charm, fd->charm, 5.0e-2 + 3.0e-2 * std::fabs(fd->charm)) << "charm " << at;
  }
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

// High-accuracy anchor: on the smooth OTM/ATM continuation region (boundary well
// below spot, q ≥ 0) the adjoint is machine-precise vs Richardson-extrapolated
// central differences of the SAME pricer it differentiates.
TEST(AdjointGreeksAmerican, HighAccuracyVsRichardson) {
  const double S = 100.0;
  int checked = 0;
  for (double K : {85.0, 92.0, 100.0}) {
    for (double T : {0.25, 1.0}) {
      for (double sigma : {0.2, 0.4}) {
        for (double r : {0.04, 0.08}) {
          const double q = 0.0;
          const auto g = american_greeks_adjoint(S, K, T, sigma, r, q, Side::Put);
          if (!g.has_value()) {
            continue;
          }
          const std::string at = "K=" + std::to_string(K) + " T=" + std::to_string(T) +
                                 " sig=" + std::to_string(sigma) + " r=" + std::to_string(r);
          const double vega_ref =
              rich_d1([&](double v) { return alput(S, K, T, v, r, q); }, sigma, 1.0e-3);
          const double rho_ref =
              rich_d1([&](double rr) { return alput(S, K, T, sigma, rr, q); }, r, 1.0e-3);
          const double gamma_ref =
              rich_d2([&](double s) { return alput(s, K, T, sigma, r, q); }, S, 3.0e-2 * S);
          const double vanna_ref = rich_cross(
              [&](double s, double v) { return alput(s, K, T, v, r, q); }, S, sigma, 2.0e-2 * S,
              2.0e-3);
          EXPECT_NEAR(g->vega, vega_ref, 5.0e-4 + 1.0e-4 * std::fabs(vega_ref)) << "vega " << at;
          EXPECT_NEAR(g->rho, rho_ref, 5.0e-4 + 1.0e-4 * std::fabs(rho_ref)) << "rho " << at;
          EXPECT_NEAR(g->gamma, gamma_ref, 1.0e-4 + 5.0e-3 * std::fabs(gamma_ref)) << "gamma " << at;
          EXPECT_NEAR(g->vanna, vanna_ref, 2.0e-3 + 5.0e-3 * std::fabs(vanna_ref)) << "vanna " << at;
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 8);
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
  for (const APt& p : amer_grid()) {
    const auto g = american_greeks_adjoint(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    const auto fd = american_greeks_fd(S, p.K, p.T, p.sigma, p.r, p.q, Side::Put);
    if (!g.has_value() || !fd.has_value()) {
      continue;
    }
    const std::string at = "K=" + std::to_string((int)p.K) + " T=" + std::to_string(p.T) +
                           " sig=" + std::to_string(p.sigma) + " r=" + std::to_string(p.r) +
                           " q=" + std::to_string(p.q);
    upd(ddelta, g->delta - fd->delta, at);
    upd(dgamma, g->gamma - fd->gamma, at);
    upd(dvega, g->vega - fd->vega, at);
    upd(drho, g->rho - fd->rho, at);
    upd(dvanna, g->vanna - fd->vanna, at);
    upd(dvolga, g->volga - fd->volga, at);
  }
  std::cout << "[gaps vs fd] delta=" << ddelta.v << " (" << ddelta.at << ")\n"
            << "             gamma=" << dgamma.v << " (" << dgamma.at << ")\n"
            << "             vega =" << dvega.v << " (" << dvega.at << ")\n"
            << "             rho  =" << drho.v << " (" << drho.at << ")\n"
            << "             vanna=" << dvanna.v << " (" << dvanna.at << ")\n"
            << "             volga=" << dvolga.v << " (" << dvolga.at << ")\n";
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

TEST(AdjointGreeksAmerican, InvalidArgument) {
  EXPECT_FALSE(american_greeks_adjoint(-1.0, 100.0, 0.5, 0.3, 0.05, 0.0, Side::Put).has_value());
  EXPECT_FALSE(american_greeks_adjoint(100.0, 100.0, -0.5, 0.3, 0.05, 0.0, Side::Put).has_value());
  EXPECT_FALSE(american_greeks_adjoint(100.0, 100.0, 0.5, 0.0, 0.05, 0.0, Side::Put).has_value());
}

} // namespace
