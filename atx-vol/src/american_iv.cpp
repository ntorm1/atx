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

// American vega for the Newton step. `american_greeks` with a null correction
// cache returns the Black-76 (European-leg) vega, which shares the sign and
// scale of the American vega; the bracket keeps a mis-scaled step safe. Returns
// 0 when the Greeks evaluation fails or the vega is non-positive, which the
// rtsafe range test naturally reads as "force bisection".
[[nodiscard]] double newton_vega(double S, double K, double T, double sigma,
                                 double r, double q, Side side,
                                 const CorrectionCache* correction) noexcept {
  const Result<AmericanGreeks> g =
      american_greeks(S, K, T, sigma, r, q, side, correction);
  return (g && std::isfinite(g->vega) && g->vega > 0.0) ? g->vega : 0.0;
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
    Result<double> p = american_price(S, K, T, sigma, r, q, side, method, opts);
    if (!p) {
      return p;  // propagate the pricer's Error
    }
    return Ok(*p - price);
  };

  // ── Bracket the root so that f(a) < 0 <= f(b) ──────────────────────────
  double a = kSigmaLo;
  ATX_TRY(double fa0, residual(a));
  if (fa0 >= 0.0) {
    // Even the vol floor over-prices the quote -> IV is at/below the floor.
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

  // ── Safeguarded Newton (rtsafe): oriented so f(xl) < 0 < f(xh) ─────────
  double xl = a;  // residual(xl) < 0
  double xh = b;  // residual(xh) >= 0

  double rts = 0.5 * (xl + xh);
  // A caller-supplied warm start (a prior nearby sigma) takes priority over the
  // European seed when it is a valid in-bracket vol; otherwise fall back to the
  // European implied vol of the American price. Both only set the initial Newton
  // iterate — the safeguarded bracket makes any seed safe.
  if (warm_start > 0.0 && warm_start > xl && warm_start < xh) {
    rts = warm_start;
  } else if (const std::optional<double> s =
                 euro_seed(price, S, K, T, r, q, side)) {
    if (*s > xl && *s < xh) {
      rts = *s;
    }
  }

  double dx = xh - xl;
  double dx_old = dx;
  ATX_TRY(double f, residual(rts));
  if (f == 0.0) {
    return Ok(rts);  // seed landed exactly on the root
  }
  double df = newton_vega(S, K, T, rts, r, q, side, correction);

  for (std::uint16_t iter = 0; iter < max_iter; ++iter) {
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
        return Ok(rts);  // interval collapsed to a representable point
      }
    } else {
      dx = f / df;
      const double prev = rts;
      rts -= dx;
      if (rts == prev) {
        return Ok(rts);  // step below one ULP
      }
    }
    if (std::fabs(dx) < tol) {
      return Ok(rts);
    }

    ATX_TRY(double f_next, residual(rts));
    f = f_next;
    df = newton_vega(S, K, T, rts, r, q, side, correction);
    if (f < 0.0) {
      xl = rts;
    } else if (f > 0.0) {
      xh = rts;
    } else {
      return Ok(rts);  // exact hit
    }
  }

  return Err(ErrorCode::Unavailable, "american_implied_vol: no convergence");
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
