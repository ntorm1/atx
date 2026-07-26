#pragma once

// Implied-volatility inversion.
//
// Ported from the C `ats-vol` library (ats_pricer_iv.c / ats_iv.h). The hot
// path is a Stefanica–Radoicic (2017) closed-form seed followed by 1–2 Halley
// (order-3) iterations to machine precision:
//
//   Reference: D. Stefanica & R. Radoicic, "An Explicit Implied Volatility
//   Formula", IJTAF 20(7), 2017. https://papers.ssrn.com/abstract=2908494
//
// The seed is uniformly within ~2% of the true σ across moneyness / maturity /
// vol, so the Halley refinement converges in 1–2 steps. Stateless and pure —
// safe to call concurrently from any threads.
//
// The C library shipped AVX2 batch variants; those are deferred in this port
// (see atx-vol/README.md). This scalar entry is the numerical source of truth.

#include "atx/vol/types.hpp"

namespace atx::vol {

// Invert a Black-76 price for the implied volatility.
//
// @param price  observed option premium (discounted)
// @param F      forward (> 0)
// @param K      strike (> 0)
// @param T      year-fraction to expiry (> 0)
// @param df     discount factor exp(-rT) (> 0)
// @param side   Call or Put
// @return       the implied volatility, or an Error:
//                 InvalidArgument — F/K/T/df <= 0
//                 OutOfRange      — non-finite input, or price outside the
//                                   no-arbitrage band [intrinsic, upper]
//                 Unavailable     — solver did not converge (deep-wing /
//                                   near-expiry vega collapse)
//               A price at (or below) intrinsic clamps to kIvMin and succeeds.
//               Likewise a quote whose true IV lies below kIvMin reports kIvMin
//               and succeeds: kIvMin is the unified reported floor (types.hpp),
//               so no result ever sits below it.
[[nodiscard]] Result<double> implied_vol(double price, double F, double K,
                                         double T, double df, Side side);

} // namespace atx::vol
