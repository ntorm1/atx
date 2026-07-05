#include "atx/vol/american_iv.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/correction.hpp"  // CorrectionCache, american_price_cached hot path
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Volatility search bracket. The spec's [1e-4, 5]; `hi` is expanded
// geometrically for the rare quote implying a vol above 500%.
constexpr double kSigmaLo = 1.0e-4;
constexpr double kSigmaHi = 5.0;
constexpr double kSigmaHiCap = 40.0;  // hard ceiling for hi expansion
constexpr unsigned kMaxExpand = 8;    // bounded hi-doubling budget

// American immediate-exercise value (undiscounted) and the price ceiling.
struct NoArbBand {
  double intrinsic;  // max(0, S-K) call / max(0, K-S) put
  double upper;      // S call / K put
};

[[nodiscard]] NoArbBand american_band(double S, double K, Side side) noexcept {
  if (side == Side::Call) {
    return NoArbBand{(S > K) ? (S - K) : 0.0, S};
  }
  return NoArbBand{(K > S) ? (K - S) : 0.0, K};
}

// European implied vol of `price` read as a Black-76 premium. The American
// price >= the European at equal vol, so this is an upper-biased but
// well-placed seed; it silently yields nullopt when the American price sits
// above the European no-arb band (common for deep-ITM American puts), leaving
// the caller to fall back to the bracket midpoint.
[[nodiscard]] std::optional<double> euro_seed(double price, double S, double K,
                                              double T, double r, double q,
                                              Side side) noexcept {
  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const Result<double> iv = implied_vol(price, F, K, T, df, side);
  if (iv && std::isfinite(*iv)) {
    return *iv;
  }
  return std::nullopt;
}

// American vega for the Newton step. `american_vega` (a null correction gives the
// Black-76 European-leg vega, which shares the sign and scale of the American
// vega; the bracket keeps a mis-scaled step safe) instead of the full
// `american_greeks` bundle — the inverter only needs vega, and the bundle's
// second-order FD terms are ~6 extra cache evaluations per Newton step. Returns 0
// when the vega is non-finite or non-positive, which the rtsafe range test reads
// as "force bisection".
[[nodiscard]] double newton_vega(double S, double K, double T, double sigma,
                                 double r, double q, Side side,
                                 const CorrectionCache* correction) noexcept {
  const double v = american_vega(S, K, T, sigma, r, q, side, correction);
  return (std::isfinite(v) && v > 0.0) ? v : 0.0;
}

// True iff `correction` is usable as the forward map for `side`: non-null,
// populated, and built for the SAME side (a side-mismatched cache would apply
// the wrong early-exercise correction, so it is ignored — cold path instead).
[[nodiscard]] bool cache_usable(const CorrectionCache* correction,
                                Side side) noexcept {
  return correction != nullptr && correction->populated() &&
         correction->side() == side;
}

}  // namespace

Result<double> american_implied_vol(double price, double S, double K, double T,
                                    double r, double q, Side side,
                                    AmericanMethod method, double tol,
                                    std::uint16_t max_iter,
                                    const std::optional<AlOpts>& opts,
                                    const CorrectionCache* correction,
                                    double warm_start) noexcept {
  // Route price + vega through the cached hot path when the cache matches side.
  const bool use_cache = cache_usable(correction, side);
  // ── Boundary validation ────────────────────────────────────────────────
  if (!std::isfinite(price) || !std::isfinite(S) || !std::isfinite(K) ||
      !std::isfinite(T) || !std::isfinite(r) || !std::isfinite(q)) {
    return Err(ErrorCode::OutOfRange, "american_implied_vol: non-finite input");
  }
  if (S <= 0.0 || K <= 0.0 || T <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "american_implied_vol: S/K/T must be > 0");
  }

  const NoArbBand band = american_band(S, K, side);
  const double band_tol = 1.0e-9 * band.upper + 1.0e-12;
  if (price < band.intrinsic - band_tol) {
    return Err(ErrorCode::OutOfRange,
               "american_implied_vol: price below intrinsic");
  }
  if (price > band.upper + band_tol) {
    return Err(ErrorCode::OutOfRange,
               "american_implied_vol: price above upper bound");
  }
  // A price at intrinsic implies sigma -> 0 (no finite IV above the floor);
  // mirror the European inverter and clamp to the vol floor.
  if (price <= band.intrinsic + band_tol) {
    return Ok(kIvMin);
  }

  // Warm-started ALO forward map for the cold Andersen-Lake path: one pricer per
  // inversion holds the early-exercise boundary across residual evaluations, so
  // each sigma re-solve reuses the previous boundary (1-2 sweeps) instead of a
  // cold seed (12 Barone-Adesi-Whaley root-finds + ~6 sweeps). Identical output to
  // repeated `andersen_lake(...)` calls, at a fraction of the cost. Only built for
  // the un-cached AndersenLake path; the cached hot path and BAW keep their maps.
  std::optional<AloPricer> alo;
  if (!use_cache && method == AmericanMethod::AndersenLake) {
    alo.emplace(S, K, T, r, q, side, opts);
  }

  // f(sigma) = american_price(sigma) - price, monotone increasing in sigma. On
  // the cached path the forward map is `american_price_cached` (Black-76 + the
  // Chebyshev correction), which is still monotone in sigma; a non-finite cached
  // price surfaces as an Internal error rather than an unbounded bracket.
  const auto residual = [&](double sigma) -> Result<double> {
    if (use_cache) {
      const double p =
          american_price_cached(S, K, T, sigma, r, q, side, correction);
      if (!std::isfinite(p)) {
        return Err(ErrorCode::Internal,
                   "american_implied_vol: cached pricer produced a non-finite price");
      }
      return Ok(p - price);
    }
    if (alo) {
      const double p = alo->price(sigma);
      if (!std::isfinite(p)) {
        return Err(ErrorCode::NotImplemented,
                   "american_implied_vol: ALO boundary collapsed (negative-carry corner)");
      }
      return Ok(p - price);
    }
    Result<double> p = american_price(S, K, T, sigma, r, q, side, method, opts);
    if (!p) {
      return p;  // propagate the pricer's Error
    }
    return Ok(*p - price);
  };

  // ── Bracket the root so f(xl) < 0 <= f(xh) ──────────────────────────────
  //
  // The European implied vol of the American price bounds the American IV from
  // ABOVE: the American price >= the European at equal sigma, and by construction
  // euro_price(seed) = price, so f(seed) = american_price(seed) - price >= 0. So
  // when the seed exists we bracket AROUND it — one cold solve at the seed, then
  // small (~7%) steps DOWN to the sign change — never pricing the far extremes
  // sigma in {1e-4, 5}, which cost two cold ALO solves of pure bracket overhead.
  // The small steps stay inside the AloPricer warm-reseed band, so for OTM options
  // (American ~ European, root within a few percent of the seed) the entire
  // bracket-and-polish is warm. A caller warm_start (a prior nearby sigma) is used
  // as the seed when valid; the safeguarded Newton below makes any seed safe.
  double xl = 0.0;  // residual(xl) < 0
  double xh = 0.0;  // residual(xh) >= 0
  double rts = 0.0;
  double f = 0.0;
  bool bracketed = false;

  double seed = 0.0;
  if (warm_start > kSigmaLo && warm_start < kSigmaHi) {
    seed = warm_start;
  } else if (const std::optional<double> es =
                 euro_seed(price, S, K, T, r, q, side)) {
    if (*es > kSigmaLo && *es < kSigmaHi) {
      seed = *es;
    }
  }

  if (seed > 0.0) {
    ATX_TRY(double f_seed, residual(seed));
    if (f_seed == 0.0) {
      return Ok(seed);  // seed is the root
    }
    rts = seed;
    f = f_seed;
    if (f_seed > 0.0) {
      // Root at/below the seed: step down to the sign change (each step warm). The
      // seed stays the upper bracket end (xh), so the Newton start rts = seed is
      // always in [xl, xh].
      xh = seed;
      double s_lo = seed;
      for (unsigned e = 0; e < 16; ++e) {
        s_lo *= 0.93;  // ~7% < the 12% AloPricer warm-reseed band
        if (s_lo <= kSigmaLo) {
          s_lo = kSigmaLo;
        }
        ATX_TRY(double f_lo, residual(s_lo));
        if (f_lo < 0.0) {
          xl = s_lo;
          bracketed = true;
          break;
        }
        if (s_lo <= kSigmaLo) {
          break;
        }
      }
      if (!bracketed) {
        // Even the vol floor over-prices the quote -> IV is at/below the floor.
        return Ok(kIvMin);
      }
    } else {
      // f(seed) < 0 (seed under-estimates — rare; the bound above is >= 0). Step
      // up to the sign change; the seed stays the lower bracket end (xl).
      xl = seed;
      double s_hi = seed;
      for (unsigned e = 0; e < 16 && s_hi < kSigmaHiCap; ++e) {
        s_hi = std::fmin(s_hi * 1.15, kSigmaHiCap);
        ATX_TRY(double f_hi, residual(s_hi));
        if (f_hi >= 0.0) {
          xh = s_hi;
          bracketed = true;
          break;
        }
      }
      if (!bracketed) {
        return Err(ErrorCode::OutOfRange,
                   "american_implied_vol: price above max-vol price");
      }
    }
  }

  if (!bracketed) {
    // Fallback wide bracket [1e-4, 5] with geometric hi-expansion: deep-ITM where
    // the European seed is unavailable (American price above the European band), or
    // a pathological quote.
    double a = kSigmaLo;
    ATX_TRY(double fa0, residual(a));
    if (fa0 >= 0.0) {
      return Ok(kIvMin);
    }
    double b = kSigmaHi;
    ATX_TRY(double fb, residual(b));
    for (unsigned e = 0; fb < 0.0 && b < kSigmaHiCap && e < kMaxExpand; ++e) {
      a = b;  // f(a) < 0 preserved (the old b was still on the negative side)
      b = std::fmin(b * 2.0, kSigmaHiCap);
      ATX_TRY(double fb_next, residual(b));
      fb = fb_next;
    }
    if (fb < 0.0) {
      return Err(ErrorCode::OutOfRange,
                 "american_implied_vol: price above max-vol price");
    }
    xl = a;
    xh = b;
    rts = 0.5 * (xl + xh);
    ATX_TRY(double f_mid, residual(rts));
    f = f_mid;
  }

  // ── Safeguarded Newton (rtsafe): oriented so f(xl) < 0 < f(xh) ─────────
  bool converged = (f == 0.0);  // seed landed exactly on the root
  double dx = xh - xl;
  double dx_old = dx;
  double df = converged ? 0.0 : newton_vega(S, K, T, rts, r, q, side, correction);

  for (std::uint16_t iter = 0; !converged && iter < max_iter; ++iter) {
    // Bisect when the Newton iterate would leave [xl, xh] or is not shrinking
    // the step by at least half — this is what guarantees convergence.
    const bool newton_out =
        (((rts - xh) * df - f) * ((rts - xl) * df - f)) > 0.0;
    const bool newton_slow = std::fabs(2.0 * f) > std::fabs(dx_old * df);
    dx_old = dx;
    if (newton_out || newton_slow) {
      dx = 0.5 * (xh - xl);
      rts = xl + dx;
      if (rts == xl) {
        converged = true;  // interval collapsed to a representable point
        break;
      }
    } else {
      dx = f / df;
      const double prev = rts;
      rts -= dx;
      if (rts == prev) {
        converged = true;  // step below one ULP
        break;
      }
    }
    if (std::fabs(dx) < tol) {
      converged = true;
      break;
    }

    ATX_TRY(double f_next, residual(rts));
    f = f_next;
    df = newton_vega(S, K, T, rts, r, q, side, correction);
    if (f < 0.0) {
      xl = rts;
    } else if (f > 0.0) {
      xh = rts;
    } else {
      converged = true;  // exact hit
      break;
    }
  }

  if (!converged) {
    return Err(ErrorCode::Unavailable, "american_implied_vol: no convergence");
  }

  // Cold polish (un-cached Andersen-Lake path only). The warm AloPricer forward
  // map used for the search reproduces andersen_lake exactly when its boundary is
  // fully converged, but at hard corners (long-dated, low-vol) the 2-JN/4-FP
  // boundary is not fully converged and is thus SEED-dependent, so the warm map
  // differs from the reference cold andersen_lake by up to the scheme's noise
  // (~1e-3 price there). One or two Newton steps on the COLD reference map lock
  // *iv to andersen_lake(*iv) = price, so the inversion is self-consistent with
  // re-pricing. The cached and BAW paths already reprice through their own search
  // map, so they need no polish. The scheme's max_dy convergence flag cannot
  // reliably predict warm==cold (a damped sweep step under-reports), so this runs
  // unconditionally; it is at most two cold solves, and the warm seed-centric
  // bracket (one cold seed, not two far extremes) keeps the inversion well below
  // the cold-per-residual baseline.
  if (alo) {
    for (int k = 0; k < 2; ++k) {
      ATX_TRY(double pc, american_price(S, K, T, rts, r, q, side, method, opts));
      const double v = newton_vega(S, K, T, rts, r, q, side, correction);
      if (!(v > 0.0)) {
        break;
      }
      const double step = (pc - price) / v;
      const double prev = rts;
      rts -= step;
      if (!(rts > 0.0)) {
        rts = prev;  // keep the last valid iterate
        break;
      }
      if (std::fabs(step) < tol) {
        break;
      }
    }
  }
  return Ok(rts);
}

Status american_implied_vol_batch(std::span<const double> price, double S,
                                  std::span<const double> K, double T, double r,
                                  double q, Side side, std::span<double> iv_out,
                                  std::span<Status> status_out,
                                  AmericanMethod method, double tol,
                                  std::uint16_t max_iter,
                                  const std::optional<AlOpts>& opts,
                                  const CorrectionCache* correction) {
  const std::size_t n = price.size();
  if (K.size() != n || iv_out.size() != n || status_out.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "american_implied_vol_batch: span length mismatch");
  }
  for (std::size_t i = 0; i < n; ++i) {
    Result<double> iv = american_implied_vol(price[i], S, K[i], T, r, q, side,
                                             method, tol, max_iter, opts,
                                             correction);
    if (iv) {
      iv_out[i] = *iv;
      status_out[i] = Ok();
    } else {
      // Match the library batch convention: NaN value slot + parallel status.
      iv_out[i] = std::numeric_limits<double>::quiet_NaN();
      status_out[i] = Err(iv.error());
    }
  }
  return Ok();
}

}  // namespace atx::vol
