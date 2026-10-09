#pragma once

// Implied-volatility inversion.
//
// Ported from the C `ats-vol` library (ats_pricer_iv.c / ats_iv.h). The hot path
// is a closed-form seed followed by Halley (order-3) iterations, clamped into
// [kIvMin, kIvMax] after every step.
//
// The seed is the Choi–Kim–Kwak (2023) L₃ tighter LOWER bound on the total
// volatility σ·√T (implied_vol.cpp `seed_choi_l3`), NOT the Stefanica–Radoicic
// (2017) quadratic this banner used to advertise. SR-2017 survives only as the
// degenerate-corner fallback for the wings where its quadratic collapses
// (γ < |y|); L₃ is exact at the money and, being a bound rather than an
// approximation, is measured 18–32% LOW at |ln F/K| ≈ 0.10–0.14 in this envelope
// — the "uniformly within ~2% of the true σ" the banner used to claim was never
// true of either seed. What IS measured and gated is the STEP COUNT it buys:
// mean 2.94, max 4 Halley steps over the 904-point iv_seed_test corpus, against
// the old seed's mean 4.71 / max 12.
//
//   Choi, Kim & Kwak, "Tighter uniform bounds for Black–Scholes implied
//   volatility", arXiv:2302.08758 (Corollary 5.2 — the L₃ lower bound).
//   Stefanica & Radoicic, "An Explicit Implied Volatility Formula",
//   IJTAF 20(7), 2017. https://papers.ssrn.com/abstract=2908494
//
// Stateless and pure — safe to call concurrently from any threads.
//
// The C library shipped AVX2 batch variants; those are deferred in this port
// (see atx-vol/README.md). This scalar entry is the numerical source of truth.

#include "atx/vol/api/core/types.hpp"

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
//                 OutOfRange      — non-finite input; price outside the
//                                   no-arbitrage band [intrinsic, upper]; or an
//                                   implied vol ABOVE kIvMax, where the answer
//                                   is out of range rather than unfound
//                 Unavailable     — solver did not converge (deep-wing /
//                                   near-expiry vega collapse)
//               A price at (or below) intrinsic clamps to kIvMin and succeeds.
//               Likewise a quote whose true IV lies below kIvMin reports kIvMin
//               and succeeds: kIvMin is the unified reported floor (types.hpp),
//               so no result ever sits below it. The ceiling is DELIBERATELY
//               asymmetric — kIvMin is a documented reported floor, kIvMax is
//               only a search bound.
[[nodiscard]] Result<double> implied_vol(double price, double F, double K,
                                         double T, double df, Side side);

} // namespace atx::vol
