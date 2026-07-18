#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/detail/adjoint_greeks.hpp"

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

} // namespace
