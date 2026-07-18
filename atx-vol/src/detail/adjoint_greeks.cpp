// ── Adjoint (AAD) American / European greeks — WS-P P2 ────────────────────
//
// Hand-coded adjoint algorithmic differentiation of the Andersen-Lake American
// pricer with implicit-function-theorem (IFT) differentiation THROUGH the
// early-exercise boundary. See docs/adjoint_greeks_design.md for the full design
// and the primary-source citations (Giles-Glasserman 2006; Savine ch.9; Henrard/
// OpenGamma AAD+IFT 2011; Griewank-Walther cheap-gradient bound; Deussen/Naumann
// EuroAD-2015 American AAD envelope + Γ=0 trap). Class: accuracy-improving.
//
// Pure functions (no globals/statics mutation) — live == backtest bit-for-bit.

#include "atx/vol/detail/adjoint_greeks.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "atx/core/math.hpp"
#include "atx/vol/american.hpp"

#include "../american_boundary.hpp" // amer:: boundary + residual + price seams

namespace atx::vol::detail {
namespace {

using atx::core::norm_cdf;
using atx::core::norm_pdf;

// ══════════════════════════════════════════════════════════════════════════
// European (Black-Scholes-Merton spot form, continuous yield q) adjoint
// ══════════════════════════════════════════════════════════════════════════
//
// The exact American price in the no-early-exercise regime (American == European)
// AND the TDD rung-1 validation of the adjoint architecture. First order is a
// hand-coded REVERSE sweep of the price graph; the raw chain rule reproduces the
// simplified closed forms (F·φ(d1) = K·φ(d2) cancellation happens numerically).
//
// price(put)  = df·(K·Φ(-d2) - F·Φ(-d1))     F = S·e^{(r-q)T}, df = e^{-rT}
// price(call) = df·(F·Φ(d1)  - K·Φ(d2))       d1 = ln(F/K)/v + v/2, d2 = d1 - v
// with v = σ√T. Reference: Giles-Glasserman "Smoking Adjoints" (RISK 2006) — the
// reverse sweep gives the whole input gradient in one pass.

struct EuroFirstOrder {
  double price;
  double dS, dK, dT, dsig, dr, dq; // full direct-input gradient (Jacobian row)
};

[[nodiscard]] EuroFirstOrder euro_reverse(double S, double K, double T, double sigma, double r,
                                          double q, Side side) noexcept {
  // ── forward ──
  const double sqrtT = std::sqrt(T);
  const double v = sigma * sqrtT;
  const double EE = std::exp((r - q) * T); // spot→forward carry
  const double F = S * EE;
  const double df = std::exp(-r * T);
  const double lnFK = std::log(F / K);
  const double d1 = lnFK / v + 0.5 * v;
  const double d2 = d1 - v;
  const double phi1 = norm_pdf(d1);
  const double phi2 = norm_pdf(d2);

  double price = 0.0;
  double dfbar = 0.0, Fbar = 0.0, Kbar = 0.0, d1bar = 0.0, d2bar = 0.0;
  if (side == Side::Put) {
    const double Nm1 = norm_cdf(-d1);
    const double Nm2 = norm_cdf(-d2);
    price = df * (K * Nm2 - F * Nm1);
    // reverse of price = df*(K*Nm2 - F*Nm1)
    dfbar = K * Nm2 - F * Nm1;
    Kbar = df * Nm2;
    Fbar = -df * Nm1;
    const double Nm2bar = df * K;
    const double Nm1bar = -df * F;
    d1bar = Nm1bar * (-phi1); // dΦ(-d1)/dd1 = -φ(d1)
    d2bar = Nm2bar * (-phi2);
  } else {
    const double Np1 = norm_cdf(d1);
    const double Np2 = norm_cdf(d2);
    price = df * (F * Np1 - K * Np2);
    dfbar = F * Np1 - K * Np2;
    Fbar = df * Np1;
    Kbar = -df * Np2;
    const double Np1bar = df * F;
    const double Np2bar = -df * K;
    d1bar = Np1bar * phi1; // dΦ(d1)/dd1 = φ(d1)
    d2bar = Np2bar * phi2;
  }

  // ── common reverse tail ──
  // d2 = d1 - v
  d1bar += d2bar;
  double vbar = -d2bar;
  // d1 = lnFK/v + 0.5*v
  const double lnFKbar = d1bar / v;
  vbar += d1bar * (-lnFK / (v * v) + 0.5);
  // lnFK = log(F/K)
  Fbar += lnFKbar / F;
  Kbar += lnFKbar * (-1.0 / K);
  // v = sigma*sqrtT
  const double dsig = vbar * sqrtT;
  double sqrtTbar = vbar * sigma;
  // sqrtT = sqrt(T)
  double Tbar = sqrtTbar * 0.5 / sqrtT;
  // df = exp(-r*T)
  double rbar = dfbar * df * (-T);
  Tbar += dfbar * df * (-r);
  // F = S*exp((r-q)*T)
  const double dS = Fbar * EE;
  rbar += Fbar * F * T;
  const double dq = Fbar * (-F * T);
  Tbar += Fbar * F * (r - q);

  return EuroFirstOrder{price, dS, Kbar, Tbar, dsig, rbar, dq};
}

// Exact BSM second-order closed forms (continuous q). Reference: Haug,
// "Complete Guide to Option Pricing Formulas" (2nd ed.), gamma/vanna/vomma/charm.
struct EuroSecondOrder {
  double gamma, vanna, volga, charm;
};

[[nodiscard]] EuroSecondOrder euro_second_order(double S, double K, double T, double sigma,
                                                double r, double q, Side side) noexcept {
  const double sqrtT = std::sqrt(T);
  const double v = sigma * sqrtT;
  const double F = S * std::exp((r - q) * T);
  const double lnFK = std::log(F / K);
  const double d1 = lnFK / v + 0.5 * v;
  const double d2 = d1 - v;
  const double eqT = std::exp(-q * T);
  const double phi1 = norm_pdf(d1);

  const double gamma = eqT * phi1 / (S * v);
  const double vanna = -eqT * phi1 * d2 / sigma;
  const double vega = S * eqT * phi1 * sqrtT;
  const double volga = vega * d1 * d2 / sigma;
  // charm = ∂θ/∂S = -∂²P/∂S∂T (calendar-time "delta decay"); matches the codebase
  // convention used by american_greeks_fd/al.
  const double common = eqT * phi1 * (2.0 * (r - q) * T - d2 * v) / (2.0 * T * v);
  double charm;
  if (side == Side::Call) {
    charm = q * eqT * norm_cdf(d1) - common;
  } else {
    charm = -q * eqT * norm_cdf(-d1) - common;
  }
  return EuroSecondOrder{gamma, vanna, volga, charm};
}

} // namespace

AmericanGreeks european_greeks_adjoint(double S, double K, double T, double sigma, double r,
                                       double q, Side side) noexcept {
  AmericanGreeks g{};
  // Degenerate: collapse to intrinsic to avoid a v=0 division. Matches the
  // pricer's T~0/σ~0 intrinsic policy.
  if (T <= 1.0e-12 || sigma <= 1.0e-8 || S <= 0.0 || K <= 0.0) {
    const double intr = (side == Side::Put) ? (K - S) : (S - K);
    g.price = intr > 0.0 ? intr : 0.0;
    if (intr > 0.0) {
      g.delta = (side == Side::Put) ? -1.0 : 1.0;
    }
    return g;
  }
  const EuroFirstOrder fo = euro_reverse(S, K, T, sigma, r, q, side);
  const EuroSecondOrder so = euro_second_order(S, K, T, sigma, r, q, side);
  g.price = fo.price;
  g.delta = fo.dS;
  g.vega = fo.dsig;
  g.rho = fo.dr;
  g.theta = -fo.dT; // calendar-time
  g.gamma = so.gamma;
  g.vanna = so.vanna;
  g.volga = so.volga;
  g.charm = so.charm;
  return g;
}

Result<AmericanGreeks> american_greeks_adjoint(double S, double K, double T, double sigma,
                                               double r, double q, Side side,
                                               const std::optional<AlOpts> &opts) {
  // Placeholder until the IFT-adjoint arm lands (next commit): defer to the
  // untouched FD reference so the drop-in contract holds from day one.
  return american_greeks_fd(S, K, T, sigma, r, q, side, AmericanMethod::AndersenLake, opts,
                            /*warm_start=*/true);
}

} // namespace atx::vol::detail
