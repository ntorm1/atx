#pragma once

// Black-76 European pricing kernels — the pricing hot path.
//
// Ported from the C `ats-vol` library (ats_pricer_b76.c / ats_b76.h). The
// kernel is closed-form and allocation-free:
//
//     d1 = (ln(F/K) + ½σ²T) / (σ√T)
//     d2 = d1 - σ√T
//     C  = df · (F·Φ(d1) - K·Φ(d2))
//     P  = df · (K·Φ(-d2) - F·Φ(-d1))
//
// `df` is the discount factor exp(-rT), passed in so it is never recomputed on
// the per-tick path. Every entry is a pure function of its arguments — no
// globals, no scratch — so concurrent calls from any threads are safe.
//
// The C library also shipped AVX2 SIMD batch variants; those are deferred in
// this port (see atx-vol/README.md). The scalar kernels below are the
// numerical source of truth the batch paths mirrored.

#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

// Forward-based Black-76 European premium.
//
// @param F      forward price (> 0)
// @param K      strike (> 0)
// @param T      year-fraction to expiry; T <= 0 collapses to intrinsic
// @param sigma  annualized lognormal volatility; sigma <= 0 collapses to intrinsic
// @param df     discount factor exp(-rT)
// @param side   Call or Put
// @return       option premium (discounted). Degenerate T<=0 or sigma<=0
//               returns df·max(intrinsic, 0).
[[nodiscard]] double black76_price(double F, double K, double T, double sigma,
                                   double df, Side side) noexcept;

// Price plus the shared d1/d2/Φ(d1) intermediates, so a caller computing Greeks
// immediately afterwards need not recompute them.
struct Black76Aux {
  double price;
  double d1;
  double d2;
  double n_d1; // Φ(d1)
};

[[nodiscard]] Black76Aux black76_aux(double F, double K, double T, double sigma,
                                     double df, Side side) noexcept;

// Fused price + vega. Vega = F·df·φ(d1)·√T is the same for calls and puts.
// `sqrt_t_in >= 0` is used as-is (caller already has √T); a negative value is
// the sentinel for "compute √T internally".
struct Black76ValueVega {
  double price;
  double vega;
};

[[nodiscard]] Black76ValueVega
black76_value_and_vega(double F, double K, double T, double sigma, double df,
                       Side side, double sqrt_t_in = -1.0) noexcept;

// Black-76 from precomputed log-moneyness ln(F/K) and √T — the portfolio engine
// computes both once per bind and shares them across the surface-IV lookup and
// the price. Numerically identical to black76_price when
// ln_fk == ln(F/K) and sqrt_t == sqrt(T).
[[nodiscard]] double black76_price_from_lnfk(double F, double K, double T,
                                             double sigma, double df,
                                             double ln_fk, double sqrt_t,
                                             Side side) noexcept;

} // namespace atx::vol
