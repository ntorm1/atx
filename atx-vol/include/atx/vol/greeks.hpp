#pragma once

// Analytic Black-76 Greeks.
//
// Ported from the C `ats-vol` library (ats_greeks_b76 in ats_pricer_b76.c).
// All eight sensitivities share the d1/d2 math with the pricer, so they are
// computed together. Conventions (forward-based leg):
//
//   delta = ∂P/∂F        gamma = ∂²P/∂F²      vega  = ∂P/∂σ
//   theta = ∂P/∂t        rho   = ∂P/∂r        vanna = ∂²P/∂F∂σ
//   volga = ∂²P/∂σ²      charm = ∂²P/∂F∂t
//
// theta uses calendar-time convention: ∂P/∂t = -∂P/∂T.
//
// Stateless and pure — safe to call concurrently from any threads.

#include "atx/vol/types.hpp"

namespace atx::vol {

// The eight analytic Black-76 sensitivities.
struct Greeks {
  double delta;
  double gamma;
  double vega;
  double theta;
  double rho;
  double vanna;
  double volga;
  double charm;
};

// Greeks plus the premium (computed in the same pass, sharing d1/d2).
struct Black76Greeks {
  Greeks greeks;
  double price;
};

// Analytic Black-76 Greeks + price.
//
// @param F      forward price
// @param K      strike
// @param T      year-fraction to expiry; T <= 0 or sigma <= 0 yields a
//               degenerate result (intrinsic-step delta, other Greeks 0)
// @param sigma  annualized lognormal volatility
// @param r      continuously-compounded rate (used by theta/rho/charm)
// @param df     discount factor exp(-rT)
// @param side   Call or Put
[[nodiscard]] Black76Greeks black76_greeks(double F, double K, double T,
                                           double sigma, double r, double df,
                                           Side side) noexcept;

} // namespace atx::vol
