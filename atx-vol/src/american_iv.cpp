#include "atx/vol/american_iv.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/correction.hpp" // CorrectionCache, american_price_cached hot path
#include "atx/vol/counters.hpp"
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
constexpr double kSigmaHiCap = 40.0; // hard ceiling for hi expansion
constexpr unsigned kMaxExpand = 8;   // bounded hi-doubling budget

// A3 (core-review finding 3): the cold IV polish must stay near the rtsafe root.
// A polished iterate that would land more than this many× the final tolerance
// outside the sign-change bracket is a collapsed-vega artifact (pre-fix it could
// run past kSigmaHiCap and be returned as a wild IV) and is dropped in favour of
// the converged rtsafe iterate.
constexpr double kPolishMaxDriftTols = 4.0;

// American immediate-exercise value (undiscounted) and the price ceiling.
struct NoArbBand {
  double intrinsic; // max(0, S-K) call / max(0, K-S) put
  double upper;     // S call / K put
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
[[nodiscard]] std::optional<double> euro_seed(double price, double S, double K, double T, double r,
                                              double q, Side side) noexcept {
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
// `american_greeks` bundle — the inverter only needs the sigma partial. Returns 0
// when the vega is non-finite or non-positive, which the rtsafe range test reads
// as "force bisection".
[[nodiscard]] double correction_vega(double S, double K, double T, double sigma, double r, double q,
                                     Side side, const CorrectionCache *correction) noexcept {
  return american_vega(S, K, T, sigma, r, q, side, correction);
}

[[nodiscard]] double correction_vega(double S, double K, double T, double sigma, double r, double q,
                                     Side side, const CorrectionBlend *correction) noexcept {
  if (correction == nullptr) {
    return american_vega(S, K, T, sigma, r, q, side, static_cast<const CorrectionCache *>(nullptr));
  }
  return american_vega(S, K, T, sigma, r, q, side, *correction);
}

template <typename Correction>
[[nodiscard]] double newton_vega(double S, double K, double T, double sigma, double r, double q,
                                 Side side, const Correction *correction) noexcept {
  const double v = correction_vega(S, K, T, sigma, r, q, side, correction);
  return (std::isfinite(v) && v > 0.0) ? v : 0.0;
}

// True iff `correction` is usable as the forward map for `side`: non-null,
// populated, and built for the SAME side (a side-mismatched cache would apply
// the wrong early-exercise correction, so it is ignored — cold path instead).
[[nodiscard]] bool cache_usable(const CorrectionCache *correction, Side side) noexcept {
  return correction != nullptr && correction->populated() && correction->side() == side;
}

[[nodiscard]] bool cache_usable(const CorrectionBlend *correction, Side side) noexcept {
  return correction != nullptr && correction->usable(side);
}

[[nodiscard]] double cached_price(double S, double K, double T, double sigma, double r, double q,
                                  Side side, const CorrectionCache *correction) noexcept {
  return american_price_cached(S, K, T, sigma, r, q, side, correction);
}

[[nodiscard]] double cached_price(double S, double K, double T, double sigma, double r, double q,
                                  Side side, const CorrectionBlend *correction) noexcept {
  return american_price_cached(S, K, T, sigma, r, q, side, *correction);
}

} // namespace

namespace {

// A3 test/measurement seam: the cold IV polish records the sign-change bracket
// [xl, xh] captured at polish entry and whether the bracket-clamp engaged on any
// polish iterate. Production callers pass nullptr and pay nothing.
struct PolishTrace {
  double xl{0.0};
  double xh{0.0};
  bool ran{false};     // the cold-AL polish path executed
  bool clamped{false}; // a polish iterate was clamped/rejected back into [xl, xh]
};

struct ThreadAloSlot {
  std::optional<AloPricer> pricer;
  bool busy{false};
};

inline thread_local ThreadAloSlot t_alo_slot{};

// One inversion exclusively leases the retained per-thread pricer. A nested
// same-thread inversion cannot overwrite the outer boundary, so it owns a local
// fallback for the duration of the nested call.
class ScopedAloPricer final {
public:
  ScopedAloPricer(double S, double K, double T, double r, double q, Side side,
                  const std::optional<AlOpts> &opts) {
    if (!t_alo_slot.busy) {
      slot_ = &t_alo_slot;
      if (slot_->pricer) {
        slot_->pricer->reset(S, K, T, r, q, side, opts);
      } else {
        slot_->pricer.emplace(S, K, T, r, q, side, opts);
      }
      slot_->busy = true;
      pricer_ = &*slot_->pricer;
      return;
    }
    fallback_.emplace(S, K, T, r, q, side, opts);
    pricer_ = &*fallback_;
  }

  ScopedAloPricer(const ScopedAloPricer &) = delete;
  ScopedAloPricer &operator=(const ScopedAloPricer &) = delete;
  ScopedAloPricer(ScopedAloPricer &&) = delete;
  ScopedAloPricer &operator=(ScopedAloPricer &&) = delete;

  ~ScopedAloPricer() noexcept {
    if (slot_ != nullptr) {
      slot_->busy = false;
    }
  }

  [[nodiscard]] AloPricer &get() noexcept { return *pricer_; }

private:
  ThreadAloSlot *slot_{nullptr};
  std::optional<AloPricer> fallback_;
  AloPricer *pricer_{nullptr};
};

template <typename Correction>
Result<double> american_implied_vol_impl(double price, double S, double K, double T, double r,
                                         double q, Side side, AmericanMethod method, double tol,
                                         std::uint16_t max_iter, const std::optional<AlOpts> &opts,
                                         const Correction *correction, double warm_start,
                                         PolishTrace *trace = nullptr) noexcept {
  counters::lightweight::AmericanIvSample telemetry_sample;
  // Route price + vega through the cached hot path when the cache matches side.
  const bool use_cache = cache_usable(correction, side);
  const Correction *const active_correction = use_cache ? correction : nullptr;
  // ── Boundary validation ────────────────────────────────────────────────
  if (!std::isfinite(price) || !std::isfinite(S) || !std::isfinite(K) || !std::isfinite(T) ||
      !std::isfinite(r) || !std::isfinite(q)) {
    return Err(ErrorCode::OutOfRange, "american_implied_vol: non-finite input");
  }
  if (S <= 0.0 || K <= 0.0 || T <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "american_implied_vol: S/K/T must be > 0");
  }

  const NoArbBand band = american_band(S, K, side);
  const double band_tol = 1.0e-9 * band.upper + 1.0e-12;
  if (price < band.intrinsic - band_tol) {
    return Err(ErrorCode::OutOfRange, "american_implied_vol: price below intrinsic");
  }
  if (price > band.upper + band_tol) {
    return Err(ErrorCode::OutOfRange, "american_implied_vol: price above upper bound");
  }
  // A price at intrinsic implies sigma -> 0 (no finite IV above the floor);
  // mirror the European inverter and clamp to the vol floor.
  if (price <= band.intrinsic + band_tol) {
    return Ok(kIvMin);
  }

  // The retained TLS state is reset once per cold Andersen-Lake inversion and
  // holds the early-exercise boundary across its residual evaluations. Cached
  // and BAW maps bypass it entirely. Same-thread reentrancy gets an isolated
  // local owner through ScopedAloPricer.
  std::optional<ScopedAloPricer> alo;
  if (!use_cache && method == AmericanMethod::AndersenLake) {
    alo.emplace(S, K, T, r, q, side, opts);
  }

  // f(sigma) = american_price(sigma) - price, monotone increasing in sigma. On
  // the cached path the forward map is `american_price_cached` (Black-76 + the
  // Chebyshev correction), which is still monotone in sigma; a non-finite cached
  // price surfaces as an Internal error rather than an unbounded bracket.
  const auto residual = [&](double sigma) -> Result<double> {
    counters::lightweight::record_residual_evaluation();
    counters::ledger::bump(counters::ledger::Solve::IvNewtonIters); // V1 always-on: one IV
                                                                    // inversion residual/Newton step
    if (use_cache) {
      const double p = cached_price(S, K, T, sigma, r, q, side, active_correction);
      if (!std::isfinite(p)) {
        return Err(ErrorCode::Internal,
                   "american_implied_vol: cached pricer produced a non-finite price");
      }
      return Ok(p - price);
    }
    if (alo) {
      const double p = alo->get().price(sigma);
      if (!std::isfinite(p)) {
        return Err(ErrorCode::NotImplemented,
                   "american_implied_vol: ALO boundary collapsed (negative-carry corner)");
      }
      return Ok(p - price);
    }
    Result<double> p = american_price(S, K, T, sigma, r, q, side, method, opts);
    if (!p) {
      return p; // propagate the pricer's Error
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
  double xl = 0.0; // residual(xl) < 0
  double xh = 0.0; // residual(xh) >= 0
  double rts = 0.0;
  double f = 0.0;
  bool bracketed = false;

  double seed = 0.0;
  if (warm_start > kSigmaLo && warm_start < kSigmaHi) {
    seed = warm_start;
  } else if (const std::optional<double> es = euro_seed(price, S, K, T, r, q, side)) {
    if (*es > kSigmaLo && *es < kSigmaHi) {
      seed = *es;
    }
  }

  if (seed > 0.0) {
    ATX_TRY(double f_seed, residual(seed));
    if (f_seed == 0.0) {
      return Ok(seed); // seed is the root
    }
    rts = seed;
    f = f_seed;
    if (f_seed > 0.0) {
      // Root at/below the seed: step down to the sign change (each step warm). The
      // seed stays the upper bracket end (xh), so the Newton start rts = seed is
      // always in [xl, xh].
      xh = seed;
      double s_lo = seed;
      double f_lo = f_seed; // residual at the current lower probe (>= 0 until bracketed)
      for (unsigned e = 0; e < 16; ++e) {
        s_lo *= 0.93; // ~7% < the 12% AloPricer warm-reseed band
        if (s_lo <= kSigmaLo) {
          s_lo = kSigmaLo;
        }
        ATX_TRY(double f_step, residual(s_lo));
        f_lo = f_step;
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
        // [correctness R-05] The bounded step-down (16 warm ~7% steps) can halt
        // ABOVE the vol floor when the seed is large — s_lo may still be well
        // inside (kSigmaLo, seed). Evaluate the TRUE floor before clamping: only
        // when even kSigmaLo over-prices the quote (residual >= 0) is the IV
        // genuinely at/below the floor. Otherwise a real, tiny-but-in-range root
        // sits in (kSigmaLo, s_lo] and must be solved, not clamped to kIvMin
        // (which would violate the documented warm_start "result unchanged"
        // contract). Mirrors the wide-bracket fallback's floor test below.
        double f_floor = f_lo;
        if (s_lo > kSigmaLo) {
          ATX_TRY(double f_at_floor, residual(kSigmaLo));
          f_floor = f_at_floor;
        }
        if (f_floor >= 0.0) {
          return Ok(kIvMin);
        }
        xl = kSigmaLo;
        xh = s_lo; // residual(s_lo) = f_lo >= 0 (loop ended without a sign change)
        rts = s_lo;
        f = f_lo;
        bracketed = true;
      }
    } else {
      // f(seed) < 0 (seed under-estimates — rare; the bound above is >= 0). Step
      // up to the sign change; the seed stays the lower bracket end (xl).
      xl = seed;
      double s_hi = seed;
      double f_hi = f_seed; // residual at the current upper probe (< 0 until bracketed)
      for (unsigned e = 0; e < 16 && s_hi < kSigmaHiCap; ++e) {
        s_hi = std::fmin(s_hi * 1.15, kSigmaHiCap);
        ATX_TRY(double f_step, residual(s_hi));
        f_hi = f_step;
        if (f_hi >= 0.0) {
          xh = s_hi;
          bracketed = true;
          break;
        }
      }
      if (!bracketed) {
        // [correctness R-05] The bounded step-up (16 warm steps) can halt BELOW
        // kSigmaHiCap when the seed is small — s_hi may still be well inside
        // [seed, kSigmaHiCap). Evaluate the TRUE ceiling before rejecting: only
        // when even kSigmaHiCap under-prices the quote (residual < 0) is the price
        // genuinely above the max-vol price. Otherwise a real, in-range (but high)
        // root sits in [s_hi, kSigmaHiCap] and must be solved, not rejected as
        // OutOfRange (which would violate the documented warm_start "result
        // unchanged" contract). Mirrors the wide-bracket fallback's hi-expansion.
        double f_ceil = f_hi;
        if (s_hi < kSigmaHiCap) {
          ATX_TRY(double f_at_ceil, residual(kSigmaHiCap));
          f_ceil = f_at_ceil;
        }
        if (f_ceil < 0.0) {
          return Err(ErrorCode::OutOfRange, "american_implied_vol: price above max-vol price");
        }
        xl = s_hi; // residual(s_hi) = f_hi < 0 (loop ended without a sign change)
        xh = kSigmaHiCap;
        rts = s_hi;
        f = f_hi;
        bracketed = true;
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
      a = b; // f(a) < 0 preserved (the old b was still on the negative side)
      b = std::fmin(b * 2.0, kSigmaHiCap);
      ATX_TRY(double fb_next, residual(b));
      fb = fb_next;
    }
    if (fb < 0.0) {
      return Err(ErrorCode::OutOfRange, "american_implied_vol: price above max-vol price");
    }
    xl = a;
    xh = b;
    rts = 0.5 * (xl + xh);
    ATX_TRY(double f_mid, residual(rts));
    f = f_mid;
  }

  // ── Safeguarded Newton (rtsafe): oriented so f(xl) < 0 < f(xh) ─────────
  bool converged = (f == 0.0); // seed landed exactly on the root
  double dx = xh - xl;
  double dx_old = dx;
  double df = converged ? 0.0 : newton_vega(S, K, T, rts, r, q, side, active_correction);

  for (std::uint16_t iter = 0; !converged && iter < max_iter; ++iter) {
    // Bisect when the Newton iterate would leave [xl, xh] or is not shrinking
    // the step by at least half — this is what guarantees convergence.
    const bool newton_out = (((rts - xh) * df - f) * ((rts - xl) * df - f)) > 0.0;
    const bool newton_slow = std::fabs(2.0 * f) > std::fabs(dx_old * df);
    dx_old = dx;
    if (newton_out || newton_slow) {
      dx = 0.5 * (xh - xl);
      rts = xl + dx;
      if (rts == xl) {
        converged = true; // interval collapsed to a representable point
        break;
      }
    } else {
      dx = f / df;
      const double prev = rts;
      rts -= dx;
      if (rts == prev) {
        converged = true; // step below one ULP
        break;
      }
    }
    if (std::fabs(dx) < tol) {
      converged = true;
      break;
    }

    ATX_TRY(double f_next, residual(rts));
    f = f_next;
    df = newton_vega(S, K, T, rts, r, q, side, active_correction);
    if (f < 0.0) {
      xl = rts;
    } else if (f > 0.0) {
      xh = rts;
    } else {
      converged = true; // exact hit
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
    if (trace != nullptr) {
      trace->ran = true;
      trace->xl = xl;
      trace->xh = xh;
    }
    const double rts0 = rts; // the converged rtsafe root; the polish stays near it
    for (int k = 0; k < 2; ++k) {
      ATX_TRY(double pc, american_price(S, K, T, rts, r, q, side, method, opts));
      const double v = newton_vega(S, K, T, rts, r, q, side, active_correction);
      if (!(v > 0.0)) {
        break;
      }
      const double step = (pc - price) / v;
      const double prev = rts;
      const double cand = rts - step;
      if (cand >= xl && cand <= xh) {
        // In-bracket: apply the Newton step exactly as the pre-fix polish did —
        // bit-identical for every quote whose polish already stayed inside the
        // sign-change bracket.
        rts = cand;
      } else {
        // [A3 core-review finding 3] The raw Newton iterate left the bracket. On
        // the cold reference map a genuine polish moves the iterate << the
        // bracket width; a step that bolts more than a few× the final tol past
        // the rtsafe root is a collapsed-vega artifact (pre-fix this could run
        // past kSigmaHiCap and be returned as a wild IV) — drop it and keep the
        // converged iterate. Otherwise clamp to the near bracket edge so the
        // polish never leaves [xl, xh].
        if (trace != nullptr) {
          trace->clamped = true;
        }
        if (std::fabs(cand - rts0) > kPolishMaxDriftTols * tol) {
          rts = prev;
          break;
        }
        rts = (cand < xl) ? xl : xh;
      }
      if (std::fabs(step) < tol) {
        break;
      }
    }
  }
  return Ok(rts);
}

} // namespace

Result<double> american_implied_vol(double price, double S, double K, double T, double r, double q,
                                    Side side, AmericanMethod method, double tol,
                                    std::uint16_t max_iter, const std::optional<AlOpts> &opts,
                                    const CorrectionCache *correction, double warm_start) noexcept {
  return american_implied_vol_impl(price, S, K, T, r, q, side, method, tol, max_iter, opts,
                                   correction, warm_start);
}

Result<double> american_implied_vol(double price, double S, double K, double T, double r, double q,
                                    Side side, const CorrectionBlend &correction,
                                    AmericanMethod method, double tol, std::uint16_t max_iter,
                                    const std::optional<AlOpts> &opts, double warm_start) noexcept {
  return american_implied_vol_impl(price, S, K, T, r, q, side, method, tol, max_iter, opts,
                                   &correction, warm_start);
}

// Test/measurement seam (declared in american_iv_test.cpp, not the public
// header). Runs the cold Andersen-Lake inversion and additionally reports the
// polish bracket [xl, xh] captured at polish entry and whether the A3
// bracket-clamp engaged on any polish iterate, so the test can pin that the
// polished IV never leaves the sign-change bracket.
Result<double> american_implied_vol_polish_traced(double price, double S, double K, double T,
                                                  double r, double q, Side side, double tol,
                                                  std::uint16_t max_iter,
                                                  const std::optional<AlOpts> &opts,
                                                  double warm_start, double &xl_out, double &xh_out,
                                                  bool &polish_ran_out, bool &polish_clamped_out) {
  PolishTrace tr;
  Result<double> iv = american_implied_vol_impl<CorrectionCache>(
      price, S, K, T, r, q, side, AmericanMethod::AndersenLake, tol, max_iter, opts,
      static_cast<const CorrectionCache *>(nullptr), warm_start, &tr);
  xl_out = tr.xl;
  xh_out = tr.xh;
  polish_ran_out = tr.ran;
  polish_clamped_out = tr.clamped;
  return iv;
}

Status american_implied_vol_batch(std::span<const double> price, double S,
                                  std::span<const double> K, double T, double r, double q,
                                  Side side, std::span<double> iv_out, std::span<Status> status_out,
                                  AmericanMethod method, double tol, std::uint16_t max_iter,
                                  const std::optional<AlOpts> &opts,
                                  const CorrectionCache *correction) {
  const std::size_t n = price.size();
  if (K.size() != n || iv_out.size() != n || status_out.size() != n) {
    return Err(ErrorCode::InvalidArgument, "american_implied_vol_batch: span length mismatch");
  }
  for (std::size_t i = 0; i < n; ++i) {
    Result<double> iv = american_implied_vol(price[i], S, K[i], T, r, q, side, method, tol,
                                             max_iter, opts, correction);
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

} // namespace atx::vol
