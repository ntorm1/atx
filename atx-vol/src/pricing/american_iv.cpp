#include "atx/vol/api/pricing/american_iv.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new> // std::bad_alloc (the noexcept-boundary containment below)
#include <optional>
#include <span>
#include <utility>

#include "pricing/al_probe.hpp" // env-gated AL hot-path zone timers (Perf 2b step 1)
#include "pricing/american_iv_batch.hpp"  // K2: the family-native lane
#include "pricing/american_iv_premium.hpp"
#include "pricing/american_iv_stream.hpp" // K2: the bench's recorded request stream
#include "atx/core/error.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/fitting/correction.hpp" // CorrectionCache, american_price_cached hot path
#include "fitting/counters.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"
#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Volatility search bracket. `hi` is expanded geometrically for the rare quote
// implying a vol above 500%.
//
// A6 (core-review finding 6): the bracket floor IS the reported floor, kIvMin.
// Previously the bracket floored at 1e-4 while quotes were reported no lower than
// kIvMin=0.005, so a root found in (1e-4, kIvMin) was returned as-is (below the
// documented floor) and a root below 1e-4 snapped up to kIvMin — a sub-floor
// band plus a 50x discontinuity as a quote decayed toward intrinsic. Unifying the
// two removes both: no representable IV sits below kIvMin, and a quote whose true
// vol is at/below the floor is reported as kIvMin with the existing at-floor
// status. Interacts with the A3 polish clamp, whose bracket lo (xl) is now kIvMin.
constexpr double kSigmaLo = kIvMin;
constexpr double kSigmaHi = 5.0;
constexpr double kSigmaHiCap = 40.0; // hard ceiling for hi expansion
constexpr unsigned kMaxExpand = 8;   // bounded hi-doubling budget

// A3 (core-review finding 3): the cold IV polish must stay near the rtsafe root.
// A polished iterate that would land more than this many× the final tolerance
// outside the sign-change bracket is a collapsed-vega artifact (pre-fix it could
// run past kSigmaHiCap and be returned as a wild IV) and is dropped in favour of
// the converged rtsafe iterate.
constexpr double kPolishMaxDriftTols = 4.0;

// The American price ceiling: the sup over exercise dates of the DISCOUNTED
// payoff ceiling. A call's payoff never exceeds S_t, worth e^{-rt}*S_t =
// S*e^{-qt} if exercised at t; a put's never exceeds K, worth K*e^{-rt}. Both
// are monotone in t, so the sup over [0, T] sits at an endpoint.
//
// [2026-08-23] The naive "S for a call, K for a put" holds only under
// NON-NEGATIVE carry. With q < 0 an American call is worth MORE than spot
// (measured: S=100, K=50, T=1, r=0.05, q=-0.5 prices at 117.31 against a spot of
// 100) and with r < 0 a put is worth more than its strike (S=K=100, T=20,
// r=-0.05 prices at 197.56 against a strike of 100). This is reachable in-house,
// not hypothetical: american_iv.hpp advertises "any sign" for r and q, and
// `imply_borrow_european_pcp_from_base` brackets borrow from -0.5
// (dividend.hpp), and q_eff IS the borrow. Under the naive ceiling the upper
// screen and the zero-vol lower screen CROSS, and an empty admissible interval
// rejects every price — a silent total refusal, which is why the caller asserts
// the two cannot cross.
[[nodiscard]] double american_price_ceiling(double S, double K, double T, double r, double q,
                                            Side side) noexcept {
  return (side == Side::Call) ? S * std::fmax(1.0, std::exp(-q * T))
                              : K * std::fmax(1.0, std::exp(-r * T));
}

// The ZERO-VOLATILITY American value: the sigma -> 0 limit of the premium, and
// therefore the true lower end of the identifiable price range.
//
// [2026-08-23, LEDGER.md:98] This screen used to compare the quote against
// the IMMEDIATE intrinsic max(0, S-K) alone. That is only the t = 0 corner of
// the limit. As sigma -> 0 the underlying follows the deterministic path
// S*e^{(r-q)t}, so the option is worth the sup of the discounted payoff over
// every exercise date:
//
//   sup_{t in [0,T]} e^{-rt}*(S*e^{(r-q)t} - K)^+  =  max(0, max_t f(t)),
//   f(t) = S*e^{-qt} - K*e^{-rt}                                    (call)
//   f(t) = K*e^{-rt} - S*e^{-qt}                                    (put)
//
// The t = T corner is the discounted forward intrinsic df*(F-K)^+, and for a
// CALL with r > q it is the LARGER of the two — the everyday case. A quote
// between the two carries no volatility at all (no sigma > 0 reproduces it), yet
// it cleared the immediate-intrinsic screen, tripped residual(kSigmaLo) >= 0
// inside the bracket, and came back as Ok(kIvMin): a SUCCESS publishing 0.005 as
// a MEASURED volatility.
//
// f(t) = A*e^{-a t} - B*e^{-b t} has AT MOST ONE stationary point, so the sup
// over the closed interval is the max over {0, T, t*}: f'(t) = 0 gives
// e^{(a-b)t} = aA / bB, i.e. t* = ln(aA / bB) / (a - b). Deep ITM and long dated
// with 0 < q < r that interior optimum clears both endpoints by whole price
// points, so the endpoints alone are not enough.
//
// The maximum-vs-minimum condition (f''(t*) = (a-b)*aA*e^{-a t*}, so a maximum
// iff a < b) is deliberately NOT tested, and the code must not be "improved" to
// test it: `fmax` over the three candidates is correct whether t* is a maximum
// or a minimum, because a minimum only contributes a value the max discards.
// That is exactly what makes the negative-rate sign flip harmless. Note t* is
// NOT always non-finite in the degenerate corners — with BOTH rates negative
// aA/bB > 0 and a != b, so t* is finite and may be a minimum; the invariant
// above, not a finiteness accident, is what keeps that correct.
[[nodiscard]] double zero_vol_american_value(double S, double K, double T, double r, double q,
                                             Side side) noexcept {
  const double A = (side == Side::Call) ? S : K;
  const double a = (side == Side::Call) ? q : r;
  const double B = (side == Side::Call) ? K : S;
  const double b = (side == Side::Call) ? r : q;
  const auto payoff = [A, a, B, b](double t) noexcept {
    return A * std::exp(-a * t) - B * std::exp(-b * t);
  };
  const double at_0 = payoff(0.0);
  const double at_T = payoff(T);
  if (!std::isfinite(at_0) || !std::isfinite(at_T)) {
    // A carry x maturity that overflows the deterministic payoff (q = -0.05 with
    // T = 1000 already reaches S*e^50). Surface it as NaN so the caller refuses
    // the quote instead of comparing against an infinity that rejects every
    // price — and so `inf - inf = NaN` cannot be silently swallowed by fmax,
    // which returns the non-NaN operand.
    return std::numeric_limits<double>::quiet_NaN();
  }
  double best = std::fmax(at_0, at_T);
  // SAFETY: t_star is formed from IEEE division and std::log, both DEFINED (not
  // UB) on every degenerate input reachable here — a == b divides by zero
  // (+/-inf), b*B == 0 divides by zero inside the log argument (log of +/-inf),
  // and a ratio of opposite-signed rates makes log return NaN. Each of those
  // fails the isfinite guard below and the sup falls back to the endpoints,
  // which is correct: a stationary point outside (0, T) cannot be the maximum.
  const double t_star = std::log((a * A) / (b * B)) / (a - b);
  if (std::isfinite(t_star) && t_star > 0.0 && t_star < T) {
    const double at_star = payoff(t_star);
    if (std::isfinite(at_star)) {
      best = std::fmax(best, at_star);
    }
  }
  return std::fmax(best, 0.0);
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

// [2026-08-23, LEDGER.md:273] Secant slope of the residual over two evaluated
// points, i.e. the derivative of the map ACTUALLY being solved.
//
// The Newton step above differentiates the wrong function on the cold/BAW route:
// the residual there is the American map (Andersen-Lake or BAW), but
// `american_vega` with a null correction is the pure Black-76 EUROPEAN vega
// (american.cpp). The sign and scale agree, so the maintained sign bracket keeps
// the search correct, but docs/LEDGER.md measured the cost: the proxy overstates
// the true AL slope 2-3x deep ITM, every Newton step understeps, the rtsafe range
// test demotes it to bisection, and convergence collapses to linear exactly where
// the proxy is worst. Two consecutive residual evaluations already bracket a
// chord of the true map, so the slope is free — no extra pricer call — and it
// tightens onto the true derivative as the iterates close in.
//
// Returns 0 (the documented "force bisection" value) whenever the chord is not a
// usable increasing slope: coincident abscissae, or a non-positive/non-finite
// difference, which at the cold pricer's own noise floor means the two residuals
// carry no resolvable slope information.
[[nodiscard]] double secant_slope(double x0, double f0, double x1, double f1) noexcept {
  const double dxs = x1 - x0;
  if (dxs == 0.0) {
    return 0.0;
  }
  const double slope = (f1 - f0) / dxs;
  return (std::isfinite(slope) && slope > 0.0) ? slope : 0.0;
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

// Fused cached price + vega for the Newton step (perf F1): ONE shared correction
// traversal feeds both the residual and the vega, replacing the separate
// cached_price (1 traversal) + american_vega (2 traversals) pair.
[[nodiscard]] AmericanPriceVega cached_price_and_vega(double S, double K, double T, double sigma,
                                                      double r, double q, Side side,
                                                      const CorrectionCache *correction) noexcept {
  return american_price_and_vega_cached(S, K, T, sigma, r, q, side, correction);
}

[[nodiscard]] AmericanPriceVega cached_price_and_vega(double S, double K, double T, double sigma,
                                                      double r, double q, Side side,
                                                      const CorrectionBlend *correction) noexcept {
  return american_price_and_vega_cached(S, K, T, sigma, r, q, side, *correction);
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
  bool bolted{false};  // price-unit extension evaluated a beyond-drift-cap iterate
  bool bolt_reverted{false}; // ... and rejected one that failed to reduce the residual
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

// NOT noexcept, deliberately: this body allocates. A cold Andersen-Lake
// inversion constructs an AloPricer, whose ~46 KB `State` is a PIMPL behind
// make_unique, and every `Err(code, "…")` below builds a std::string message
// longer than any SSO buffer. Both throw std::bad_alloc under memory pressure.
// The `noexcept` the PUBLIC entry points below must keep (american_iv.hpp) is
// made TRUE by containing that throw at those boundaries, not by an unprovable
// claim here.
template <typename Correction>
Result<double> american_implied_vol_impl(double price, double S, double K, double T, double r,
                                         double q, Side side, AmericanMethod method, double tol,
                                         std::uint16_t max_iter, const std::optional<AlOpts> &opts,
                                         const Correction *correction, double warm_start,
                                         double price_tol = 0.0, PolishTrace *trace = nullptr) {
  const alprobe::Scope probe_zone(alprobe::Zone::AmericanIv);
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

  // The identifiable range is [zero-vol value, ceiling]. The zero-vol value is a
  // sup over exercise dates, NOT the immediate intrinsic — see
  // zero_vol_american_value.
  const double ceiling = american_price_ceiling(S, K, T, r, q, side);
  const double zero_vol = zero_vol_american_value(S, K, T, r, q, side);
  if (!std::isfinite(ceiling) || !std::isfinite(zero_vol)) {
    return Err(ErrorCode::OutOfRange,
               "american_implied_vol: carry x maturity overflows the zero-vol bound");
  }
  // Both ends are sups of the same integrand over the same interval — the
  // ceiling drops the subtracted (non-negative) leg — so ceiling >= zero_vol
  // holds by construction, not by luck. An empty interval would reject EVERY
  // price silently, so make it unrepresentable rather than merely unlikely.
  assert(ceiling >= zero_vol && "american_implied_vol: empty admissible price interval");
  const double band_tol = 1.0e-9 * ceiling + 1.0e-12;
  if (price > ceiling + band_tol) {
    return Err(ErrorCode::OutOfRange, "american_implied_vol: price above upper bound");
  }
  // Immediate intrinsic is the one lower bound EVERY American map respects
  // (early exercise is always available), so an arbitrage-violating quote is
  // refused here for free — no pricer call. The zero-vol screen proper needs the
  // forward map and therefore runs below, after it exists.
  const double immediate =
      (side == Side::Call) ? std::fmax(S - K, 0.0) : std::fmax(K - S, 0.0);
  if (price < immediate - band_tol) {
    return Err(ErrorCode::OutOfRange, "american_implied_vol: price below intrinsic");
  }
  // A price AT the zero-vol value implies sigma -> 0 (no finite IV above the
  // floor); mirror the European inverter and clamp to the vol floor. A price
  // between it and the price at kIvMin is a genuine sub-floor root and still
  // reports kIvMin through the bracket, per the unified-floor contract. Kept
  // here, ahead of the map, because a deep-ITM quote sitting at intrinsic is the
  // COMMON case and must not pay a cold solve to be recognised.
  if (price >= zero_vol - band_tol && price <= zero_vol + band_tol) {
    return Ok(kIvMin);
  }
  const bool below_zero_vol = price < zero_vol - band_tol;

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

  // ── Zero-vol admissibility, adjudicated by the MAP that will be inverted ──
  //
  // The analytic sup above is the sigma -> 0 limit of the EXACT American value,
  // and Andersen-Lake reproduces it to 1.3e-13 across a 2,688-point carry x
  // moneyness x maturity scan. But this entry point also serves BAW and the
  // Chebyshev-corrected cached map, which are APPROXIMATIONS: nothing guarantees
  // their own sigma -> 0 limit sits at the exact one, and refusing a price the
  // caller's own selected pricer produced would break the round trip.
  //
  // So the analytic bound PROPOSES the refusal and the map DISPOSES: one
  // evaluation of the actual residual at the vol floor, paid only on this path.
  // If the map prices at or below the quote there, a root exists at/above kIvMin
  // under this map and the search runs normally; only when the map's own floor
  // price also exceeds the quote is the quote genuinely unidentifiable. The same
  // scan finds ZERO BAW cases that this rescues today (every one the analytic
  // bound would newly refuse was already below immediate intrinsic, hence
  // refused before this change too), so the probe buys no coverage now — it buys
  // the INVARIANT that no approximate forward map can ever be refused a price it
  // itself produced, which a finite scan cannot promise on its own.
  if (below_zero_vol) {
    ATX_TRY(const double f_floor, residual(kSigmaLo));
    if (f_floor > band_tol) {
      return Err(ErrorCode::OutOfRange,
                 "american_implied_vol: price below the zero-vol American bound");
    }
  }

  // Fused residual + vega for the safeguarded-Newton refinement (perf F1). On the
  // CACHED path a single american_price_and_vega_cached call shares ONE correction
  // tensor traversal between the residual and the vega — the exact 3->2 (stage a),
  // ~3->1 (stage b) cut this task banks. On the cold/BAW path it is the previous
  // residual() + american_vega() pair, unchanged. Counter accounting matches the
  // residual lambda: one residual/Newton-step bump per call.
  const auto residual_and_vega = [&](double sigma) -> Result<std::pair<double, double>> {
    if (use_cache) {
      counters::lightweight::record_residual_evaluation();
      counters::ledger::bump(counters::ledger::Solve::IvNewtonIters);
      const AmericanPriceVega pv =
          cached_price_and_vega(S, K, T, sigma, r, q, side, active_correction);
      if (!std::isfinite(pv.price)) {
        return Err(ErrorCode::Internal,
                   "american_implied_vol: cached pricer produced a non-finite price");
      }
      const double v = (std::isfinite(pv.vega) && pv.vega > 0.0) ? pv.vega : 0.0;
      return Ok(std::pair<double, double>{pv.price - price, v});
    }
    ATX_TRY(double f_cold, residual(sigma)); // bumps the counters on the cold/BAW path
    const double v = newton_vega(S, K, T, sigma, r, q, side, active_correction);
    return Ok(std::pair<double, double>{f_cold, v});
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
  // The OTHER evaluated point of the completed bracket — the second half of
  // the initial secant chord, so the first Newton step already differentiates the
  // forward map instead of the Black-76 proxy. Every branch below closes the
  // bracket having priced exactly two points; this records the one that is not
  // `rts`.
  double x_sec = 0.0;
  double f_sec = 0.0;
  bool have_sec = false;

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
          x_sec = s_lo;
          f_sec = f_lo;
          have_sec = true;
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
        x_sec = kSigmaLo;
        f_sec = f_floor;
        have_sec = true;
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
          x_sec = s_hi;
          f_sec = f_hi;
          have_sec = true;
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
        x_sec = kSigmaHiCap;
        f_sec = f_ceil;
        have_sec = true;
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
    x_sec = b;
    f_sec = fb;
    have_sec = true;
  }

  // ── Safeguarded Newton (rtsafe): oriented so f(xl) < 0 < f(xh) ─────────
  bool converged = (f == 0.0); // seed landed exactly on the root
  double dx = xh - xl;
  double dx_old = dx;
  // Prefer the bracket's own chord on the cold/BAW route, where the analytic
  // vega differentiates a DIFFERENT function than the residual (see
  // secant_slope). On the cached route `american_price_and_vega_cached` returns
  // the true sigma partial of the very map being solved AND shares one correction
  // traversal with the residual (perf F1), so that path keeps its analytic vega.
  double df = 0.0;
  if (!converged) {
    const double slope0 = (use_cache || !have_sec) ? 0.0 : secant_slope(x_sec, f_sec, rts, f);
    df = (slope0 > 0.0) ? slope0 : newton_vega(S, K, T, rts, r, q, side, active_correction);
  }

  for (std::uint16_t iter = 0; !converged && iter < max_iter; ++iter) {
    // Bisect when the Newton iterate would leave [xl, xh] or is not shrinking
    // the step by at least half — this is what guarantees convergence.
    const bool newton_out = (((rts - xh) * df - f) * ((rts - xl) * df - f)) > 0.0;
    const bool newton_slow = std::fabs(2.0 * f) > std::fabs(dx_old * df);
    dx_old = dx;
    // The point whose residual `f` currently is — the near half of the next
    // secant chord. Captured before the step moves `rts`.
    const double x_prev = rts;
    const double f_prev = f;
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

    // Fused residual + vega: one shared correction traversal per Newton step on
    // the cached path (perf F1). The residual is bit-identical to residual(rts).
    ATX_TRY(auto fv, residual_and_vega(rts));
    // The two most recent evaluations are a chord of the map being solved;
    // the analytic vega on the cold/BAW route is a chord of a different one. Fall
    // back to the vega only where the chord carries no usable slope.
    const double slope = use_cache ? 0.0 : secant_slope(x_prev, f_prev, rts, fv.first);
    f = fv.first;
    df = (slope > 0.0) ? slope : fv.second;
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
  // unconditionally; it is at most two cold solves (four when the caller states a
  // price_tol — see the price-unit extension below), and the warm seed-centric
  // bracket (one cold seed, not two far extremes) keeps the inversion well below
  // the cold-per-residual baseline.
  if (alo) {
    const alprobe::Scope probe_polish(alprobe::Zone::IvColdPolish);
    if (trace != nullptr) {
      trace->ran = true;
      trace->xl = xl;
      trace->xh = xh;
    }
    const double rts0 = rts; // the converged rtsafe root; the polish stays near it
    // The price-unit extension below shares one bounded cold-solve budget with
    // this sigma-unit loop. The counter is dead on the price_tol == 0 path and
    // changes no arithmetic there.
    int polish_solves = 0;
    for (int k = 0; k < 2; ++k) {
      ATX_TRY(double pc, american_price(S, K, T, rts, r, q, side, method, opts));
      ++polish_solves;
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

    // ── Price-unit polish extension (price_tol > 0; 2026-08-23 diagnosis) ──
    //
    // The sigma-unit polish above has a hard authority ceiling: every accepted
    // move is bounded by the bracket / the kPolishMaxDriftTols cap, i.e. a few×
    // tol of the rtsafe root. But the rtsafe search ran on the WARM AloPricer
    // forward map, whose 2-JN/4-FP boundary is seed-dependent at the hard
    // corners (long-dated deep-ITM puts): the warm root sits 10-62× tol away
    // from the COLD reference root (measured 1e-5..6.2e-5 sigma). A caller that
    // re-prices through the cold map then misses by vega × gap — beyond any
    // budget wherever dollar vega is large — and no sigma-unit step is allowed
    // to close it. When the caller states a PRICE budget, verify the polished
    // iterate on the cold map and, only if it misses, continue the same cold
    // Newton unconstrained by the sigma cap. Each extension solve is
    // american_greeks_al — the exact map Mode B re-prices with (its price is
    // bit-identical to american_price on this route) — because ONE cold solve
    // yields both the residual and the TRUE cold-map vega. The Black-76-proxy
    // newton_vega the search uses is ~2-3× the real slope at the deep-ITM
    // corners (early exercise kills vega), which under-steps Newton exactly
    // where this extension is needed. The cap's collapsed-vega protection is
    // preserved in price units: a candidate is kept ONLY if it strictly
    // REDUCES the cold-map residual; the first non-reducing candidate is
    // rejected and the best verified iterate is returned (so the result is
    // never worse on the cold map than the sigma-polish result). Bounded: the
    // sigma loop and this extension share one kPolishColdSolveBudget.
    if (price_tol > 0.0) {
      constexpr int kPolishColdSolveBudget = 4;
      double cur = rts;
      double cur_pc = 0.0;
      double cur_vega = 0.0;
      bool have_cur = false;
      if (polish_solves < kPolishColdSolveBudget) {
        const Result<AmericanGreeks> g0 = american_greeks_al(S, K, T, cur, r, q, side, opts);
        ++polish_solves;
        if (g0.has_value() && std::isfinite(g0->price)) {
          cur_pc = g0->price;
          cur_vega = g0->vega;
          have_cur = true;
        }
      }
      // Verification unavailable (budget spent or the pricer refused): keep
      // the sigma-polish iterate rather than surfacing a new error class.
      if (have_cur) {
        double best = cur;
        double best_resid = std::fabs(cur_pc - price);
        while (best_resid > price_tol && polish_solves < kPolishColdSolveBudget) {
          if (!(std::isfinite(cur_vega) && cur_vega > 0.0)) {
            break;
          }
          double cand = cur - (cur_pc - price) / cur_vega;
          if (!std::isfinite(cand)) {
            break;
          }
          cand = std::fmin(std::fmax(cand, kSigmaLo), kSigmaHiCap);
          if (cand == cur) {
            break; // step vanished at this precision
          }
          const bool past_cap = std::fabs(cand - rts0) > kPolishMaxDriftTols * tol;
          if (past_cap && trace != nullptr) {
            trace->bolted = true;
          }
          const Result<AmericanGreeks> g = american_greeks_al(S, K, T, cand, r, q, side, opts);
          ++polish_solves;
          if (!g.has_value() || !std::isfinite(g->price)) {
            break; // keep the best verified iterate
          }
          const double cand_resid = std::fabs(g->price - price);
          if (!(cand_resid < best_resid)) {
            // Failed to reduce the cold-map residual: the drift cap's
            // collapsed-vega protection stands — reject the iterate and stop.
            if (past_cap && trace != nullptr) {
              trace->bolt_reverted = true;
            }
            break;
          }
          best = cand;
          best_resid = cand_resid;
          cur = cand;
          cur_pc = g->price;
          cur_vega = g->vega;
        }
        rts = best;
      }
    }
  }
  return Ok(rts);
}

} // namespace

// SAFETY: `noexcept` is pinned by american_iv.hpp, but the inversion allocates
// (see american_implied_vol_impl). Contain the resulting std::bad_alloc here so
// the promise holds and memory pressure degrades to the declared Result error
// model instead of std::terminate. The Error carries NO message: building one is
// itself an allocation, which is exactly what just failed. Only bad_alloc is
// caught — any other exception would mean a contract violation deeper in the
// call tree, and staying loud (terminate) is the right answer for that.
Result<double> american_implied_vol(double price, double S, double K, double T, double r, double q,
                                    Side side, AmericanMethod method, double tol,
                                    std::uint16_t max_iter, const std::optional<AlOpts> &opts,
                                    const CorrectionCache *correction, double warm_start,
                                    double price_tol) noexcept {
  try {
    return american_implied_vol_impl(price, S, K, T, r, q, side, method, tol, max_iter, opts,
                                     correction, warm_start, price_tol);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Internal);
  }
}

Result<double> american_implied_vol(double price, double S, double K, double T, double r, double q,
                                    Side side, const CorrectionBlend &correction,
                                    AmericanMethod method, double tol, std::uint16_t max_iter,
                                    const std::optional<AlOpts> &opts, double warm_start,
                                    double price_tol) noexcept {
  try {
    return american_implied_vol_impl(price, S, K, T, r, q, side, method, tol, max_iter, opts,
                                     &correction, warm_start, price_tol);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Internal);
  }
}

// ── Opt-in non-monotone refusal (L1c, 2026-10-18; contract in american_iv.hpp) ──

namespace {

// Gate kappa (american_iv.hpp al_non_monotone_gate): ~4x the largest exercise-region
// artifact the region map measured per scheme class, relative to K * max(1, T)^2.
constexpr double kNonMonotoneGateAccurate = 5.0e-6;
constexpr double kNonMonotoneGateWide = 4.0e-4;
// Scalar certificate inside the band: slope probe width (relative), slope floor
// (relative to K, the chain batch's default vega_floor) and the largest accepted
// Newton correction on the cold map, in vol units.
constexpr double kCertProbeRel = 1.0e-4;
constexpr double kCertSlopeFloorRel = 1.0e-8;
constexpr double kCertSigmaTol = 1.0e-4;

// The accurate scheme: nullopt, or its explicit equivalent under either profile.
[[nodiscard]] bool accurate_scheme(const std::optional<AlOpts> &opts) noexcept {
  if (!opts) {
    return true;
  }
  const AlOpts a = al_fast_profile(std::nullopt);
  return opts->n_collocation == a.n_collocation && opts->n_quadrature == a.n_quadrature &&
         opts->n_quad_price == a.n_quad_price && opts->max_newton_iter == a.max_newton_iter &&
         opts->tol == a.tol;
}

// Spot on the exercise side of the kernel's own boundary at sigma. Not noexcept: the
// kernel's Error path builds a message string.
[[nodiscard]] bool in_exercise_region(double S, double K, double T, double r, double q,
                                      Side side, double sigma,
                                      const std::optional<AlOpts> &opts) {
  const Result<double> b = exercise_boundary(K, T, sigma, r, q, side, opts);
  if (!b || !std::isfinite(*b)) {
    return false; // European regime (no boundary) or a regime the kernel refuses
  }
  return (side == Side::Put) ? (S <= *b) : (S >= *b);
}

[[nodiscard]] bool in_band(double price, double S, double K, double T, double r, double q,
                           Side side, const std::optional<AlOpts> &opts) noexcept {
  const double zv = zero_vol_american_value(S, K, T, r, q, side);
  return price - zv <= al_non_monotone_gate(K, T, opts); // NaN anywhere -> false
}

// The scalar's answer x, refused (false) inside the band when x lies in the exercise
// region, or (x above kIvMin) when the cold map does not certify it as its root.
[[nodiscard]] bool scalar_answer_certified(double price, double S, double K, double T, double r,
                                           double q, Side side, double x,
                                           const std::optional<AlOpts> &opts) {
  if (!in_band(price, S, K, T, r, q, side, opts)) {
    return true;
  }
  if (in_exercise_region(S, K, T, r, q, side, x, opts)) {
    return false;
  }
  if (!(x > kIvMin)) {
    return true; // the documented floor clamp, outside the exercise region
  }
  const double xb = x * (1.0 - kCertProbeRel);
  const Result<double> px = andersen_lake(S, K, T, x, r, q, side, opts);
  const Result<double> pb = andersen_lake(S, K, T, xb, r, q, side, opts);
  if (!px || !pb || !std::isfinite(*px) || !std::isfinite(*pb)) {
    return false;
  }
  const double slope = (*px - *pb) / (x - xb);
  if (!(slope > kCertSlopeFloorRel * K)) {
    return false;
  }
  return std::fabs(*px - price) <= kCertSigmaTol * slope;
}

[[nodiscard]] IvStatus iv_status_of(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::InvalidArgument:
    return IvStatus::InvalidArgument;
  case ErrorCode::OutOfRange:
    return IvStatus::OutOfRange;
  case ErrorCode::Unavailable:
    return IvStatus::NoConvergence;
  default: // NotImplemented regimes, Internal (allocation), anything the pricer raised
    return IvStatus::Unpriceable;
  }
}

} // namespace

double al_non_monotone_gate(double K, double T, const std::optional<AlOpts> &opts) noexcept {
  if (!(K > 0.0) || !(T > 0.0) || !std::isfinite(K) || !std::isfinite(T)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double kappa = accurate_scheme(opts) ? kNonMonotoneGateAccurate : kNonMonotoneGateWide;
  return kappa * K * std::fmax(1.0, T * T);
}

double iv_premium_seed_gate(double K, double T, const std::optional<AlOpts> &seed_opts,
                            const IvPremiumGate *gate) noexcept {
  if (gate == nullptr || !(gate->seed_gate_kappa > 0.0)) {
    return al_non_monotone_gate(K, T, seed_opts);
  }
  if (!(K > 0.0) || !(T > 0.0) || !std::isfinite(K) || !std::isfinite(T)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return gate->seed_gate_kappa * K * std::fmax(1.0, T * T);
}

bool al_non_monotone_region(double price, double S, double K, double T, double r, double q,
                            Side side, double sigma, const std::optional<AlOpts> &opts) noexcept {
  if (!in_band(price, S, K, T, r, q, side, opts)) {
    return false;
  }
  try {
    return in_exercise_region(S, K, T, r, q, side, sigma, opts);
  } catch (const std::bad_alloc &) {
    return true; // refuse is the safe side
  }
}

namespace {

// The Search route of the typed entry point (also every Premium fallback). Not
// noexcept: the inversion allocates; the public entry points contain bad_alloc.
[[nodiscard]] IvResult search_typed(double price, double S, double K, double T, double r,
                                    double q, Side side, const IvOpts &opts) {
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  const Result<double> iv =
      american_implied_vol_impl(price, S, K, T, r, q, side, opts.method, opts.tol, opts.max_iter,
                                opts.al_opts, opts.correction, opts.warm_start, opts.price_tol);
  if (!iv) {
    return IvResult{kNaN, iv_status_of(iv.error().code())};
  }
  const bool cold_al =
      opts.method == AmericanMethod::AndersenLake && !cache_usable(opts.correction, side);
  if (opts.refuse_non_monotone && cold_al &&
      !scalar_answer_certified(price, S, K, T, r, q, side, *iv, opts.al_opts)) {
    return IvResult{kNaN, IvStatus::NonMonotoneRegion};
  }
  return IvResult{*iv, IvStatus::Ok};
}

// ── Premium strategy (L2e; contract in american_iv.hpp IvStrategy) ──

// The approximation leg A of the decomposition P_am(sigma) = A(sigma) + D(sigma). Where
// the kernel has an exercise region it is the Barone-Adesi-Whaley price: its quadratic
// approximation already carries most of the early-exercise premium, so the remainder
// D = P_am - A is nearly flat in sigma even deep in the money (where the premium
// itself is strongly curved: on a 3-year deep-ITM put the Black-76 remainder moved the
// iterates linearly). Elsewhere it is Black-76 on the kernel's forward, where the
// American map IS Black-76 and D = 0. A is only ever a seed model: every accepted root
// is confirmed on the cold Andersen-Lake map.
class Leg {
public:
  Leg(double S, double K, double T, double r, double q, Side side) noexcept
      : S_(S), K_(K), T_(T), r_(r), q_(q), side_(side), F_(S * std::exp((r - q) * T)),
        df_(std::exp(-r * T)),
        baw_((side == Side::Put) ? r > 0.0 : q > 0.0) {}

  [[nodiscard]] bool valid() const noexcept {
    return F_ > 0.0 && std::isfinite(F_) && df_ > 0.0 && std::isfinite(df_);
  }

  // A(sigma); nullopt where the BAW critical-price search fails.
  [[nodiscard]] std::optional<double> price(double sigma) const {
    if (!baw_) {
      return black76_price(F_, K_, T_, sigma, df_, side_);
    }
    const Result<double> p = baw_american(S_, K_, T_, sigma, r_, q_, side_, kBawIter, kBawTol);
    if (!p || !std::isfinite(*p)) {
      return std::nullopt;
    }
    return *p;
  }

  // dA/dsigma: Black-76 vega, or a central difference of the BAW price.
  [[nodiscard]] std::optional<double> slope(double sigma) const {
    if (!baw_) {
      return black76_value_and_vega(F_, K_, T_, sigma, df_, side_).vega;
    }
    const double h = kBawSlopeStep * sigma;
    const std::optional<double> up = price(sigma + h);
    const std::optional<double> dn = price(sigma - h);
    if (!up || !dn) {
      return std::nullopt;
    }
    return (*up - *dn) / (2.0 * h);
  }

  // Solves A(sigma) + d0 + e * (sigma - s0) = target from `start`: Newton kept inside the
  // sign-change bracket it learns (bisection otherwise), bounded. nullopt when the
  // model has no increasing root inside (kIvMin, kSigmaHi).
  [[nodiscard]] std::optional<double> solve(double target, double d0, double s0, double e,
                                            double start) const {
    constexpr int kMaxModelSteps = 48;
    double lo = kSigmaLo;
    double hi = kSigmaHi;
    double x = std::fmin(std::fmax(start, kSigmaLo * 1.001), kSigmaHi * 0.999);
    for (int i = 0; i < kMaxModelSteps; ++i) {
      const std::optional<double> a = price(x);
      const std::optional<double> da = slope(x);
      if (!a || !da) {
        return std::nullopt;
      }
      const double g = *a + d0 + e * (x - s0) - target;
      const double dg = *da + e;
      if (!std::isfinite(g) || !std::isfinite(dg)) {
        return std::nullopt;
      }
      if (g == 0.0) {
        return x;
      }
      (g > 0.0 ? hi : lo) = x;
      double next = (dg > 0.0) ? x - g / dg : 0.5 * (lo + hi);
      if (!(next > lo && next < hi)) {
        next = 0.5 * (lo + hi);
      }
      if (std::fabs(next - x) <= 1.0e-15 + 1.0e-13 * x) {
        return (next > kSigmaLo && next < kSigmaHi) ? std::optional<double>{next} : std::nullopt;
      }
      x = next;
    }
    return std::nullopt;
  }

  // The Black-76 IV of `target` (the American IV's upper bound: P_am >= P_eu).
  [[nodiscard]] std::optional<double> euro_iv(double target) const {
    const Result<double> v = implied_vol(target, F_, K_, T_, df_, side_);
    if (v && std::isfinite(*v) && *v > kSigmaLo && *v < kSigmaHi) {
      return *v;
    }
    return std::nullopt;
  }

private:
  static constexpr std::uint16_t kBawIter = 64;
  static constexpr double kBawTol = 1.0e-12;
  static constexpr double kBawSlopeStep = 1.0e-5; // relative central-difference step
  double S_, K_, T_, r_, q_;
  Side side_;
  double F_, df_;
  bool baw_; // the kernel has an exercise region: put r > 0, call q > 0
};

// One cold point of the contract: sigma, its American price and its remainder D.
struct ColdPoint {
  double sigma;
  double price;
  double rest;
};

// A family-native attempt's outcome (IvPremiumGate::qfamily_native): an accepted root with
// the Newton correction taken and the fused vega at its last point (the confirm's LOCAL
// slope), the last point; not accepted = hand the price to the seed-leg path, the typed
// reason in IvPremiumStats::native_status.
struct NativeRoot {
  bool accepted = false;
  double root = 0.0;
  double slope = 0.0;
  double residual_usd = 0.0;
  ColdPoint last{0.0, 0.0, 0.0};
};

static_assert(kIvPremiumNativeEvaluations == iv_batch::kInverseMaxEvaluations,
              "the native attempt's budget is the batch seam's lane cap");

// The family-native iterates (SPY1s Phase 3 lever 1) through the K2 batch seam: ONE lane of
// iv_batch::invert_many on the (sigma, q) family's own map at the strike's own q, a bounded
// bracketed Newton on the fused price + vega (no seed leg, no chord). The lane starts from
// the contract's native carry - the evaluated points of an earlier attempt on this contract
// (the quote's mid behind its bid / ask) bracket the price on the side their prices are on
// and the last one with its vega gives a Newton seed - else from the Black-76 IV of the
// price at the contract's forward (an upper bound of the American IV); a failed or
// out-of-box seed starts from the bracket (the box top is priced AT the box top, mn5).
// Every attempt rewrites the carry, accepted or not. Not noexcept: the Black-76 inversion's
// Error path builds a message string.
[[nodiscard]] NativeRoot family_native_root(double price, double S, double K, double T, double r,
                                            double q, Side side, double tol,
                                            const AlSigmaQFamily &f, IvPremiumMemory *memory,
                                            IvPremiumStats &stats, std::uint32_t stream_id) {
  namespace ib = iv_batch;
  NativeRoot out;
  double seed = std::numeric_limits<double>::quiet_NaN();
  double blo = seed;
  double bhi = seed;
  double plo = seed;
  double phi = seed;
  if (memory != nullptr && memory->carry.valid) {
    const IvPremiumNativeCarry &c = memory->carry;
    stats.native_carried = true;
    const auto take = [&](double s, double p) {
      if (!std::isfinite(s) || !std::isfinite(p)) return;
      if (p < price) {
        if (!(blo >= s)) {
          blo = s;
          plo = p;
        }
      } else if (!(bhi <= s)) {
        bhi = s;
        phi = p;
      }
    };
    take(c.lo, c.p_lo);
    take(c.hi, c.p_hi);
    take(c.sigma, c.price);
    if (std::isfinite(c.vega) && c.vega > ib::kInverseVegaFloorRel * K) {
      seed = c.sigma + (price - c.price) / c.vega;
    }
  }
  if (!std::isfinite(seed)) {
    const double F = S * std::exp((r - q) * T);
    const Result<double> eu = implied_vol(price, F, K, T, std::exp(-r * T), side);
    if (eu && std::isfinite(*eu)) seed = *eu;
  }
  const std::uint8_t is_call = side == Side::Call ? 1u : 0u;
  double sigma = 0.0;
  double lo = 0.0;
  double hi = 0.0;
  double p_lo = 0.0;
  double p_hi = 0.0;
  double res = 0.0;
  double vega = 0.0;
  double last = 0.0;
  std::uint16_t evals = 0;
  ib::InverseStatus status = ib::InverseStatus::Inactive;
  std::uint8_t flags = 0;
  ib::LaneAcceptance acc;
  ib::InverseBatchIn in;
  in.family = &f;
  in.S = S;
  in.tol = tol;
  in.max_evaluations = static_cast<std::uint16_t>(kIvPremiumNativeEvaluations);
  in.isa = ib::InverseIsa::Scalar;
  in.K = std::span<const double>(&K, 1);
  in.target = std::span<const double>(&price, 1);
  in.q = std::span<const double>(&q, 1);
  in.is_call = std::span<const std::uint8_t>(&is_call, 1);
  in.seed_sigma = std::span<const double>(&seed, 1);
  in.bracket_lo = std::span<const double>(&blo, 1);
  in.bracket_hi = std::span<const double>(&bhi, 1);
  in.price_lo = std::span<const double>(&plo, 1);
  in.price_hi = std::span<const double>(&phi, 1);
  const ib::InverseBatchOut o{{&sigma, 1}, {&lo, 1},     {&hi, 1},    {&p_lo, 1},
                              {&p_hi, 1},  {&res, 1},    {&vega, 1},  {&last, 1},
                              {&evals, 1}, {&status, 1}, {&flags, 1}, {&acc, 1}};
  // One scratch per thread (built once; the one-lane call then allocates nothing).
  thread_local ib::InverseScratch scratch;
  if (ib::invert_many(in, o, scratch) != ib::InverseBatchError::None) {
    stats.native_status = ib::InverseStatus::InvalidInput;
    return out;
  }
  stats.native_status = status;
  stats.native_evaluations = evals;
  stats.native_flags = flags;
  stats.family_prices += evals;
  const double last_price = std::isfinite(res) ? price + res : res;
  if (memory != nullptr) {
    memory->carry = IvPremiumNativeCarry{true, lo, hi, p_lo, p_hi, last, last_price, vega,
                                         stream_id};
  }
  ib::inverse_stream_annotate(stream_id, status, evals, sigma);
  if (status != ib::InverseStatus::Converged || !std::isfinite(vega) ||
      !(vega > ib::kInverseVegaFloorRel * K)) {
    return out;
  }
  out.accepted = true;
  out.root = sigma;
  out.slope = vega;
  out.residual_usd = std::fabs(res);
  out.last = ColdPoint{last, last_price, last_price};
  if (memory != nullptr) {
    // The accepted point as the memory's one native point (the leg path skips native
    // points; the de-Am cache seeds the band edges off a memory with points).
    memory->native = true;
    memory->points = 1;
    memory->sigma[0] = last;
    memory->rest[0] = last_price;
  }
  return out;
}

// The Premium root, or nullopt with stats.fallback set (the caller then runs Search).
// Not noexcept: andersen_lake's Error path builds a message string.
// `seed_opts` / `confirm` are taken by value: a price inside the seed tier's own
// non-monotone gate re-routes the iterates to `opts` (the accurate kernel) below.
[[nodiscard]] std::optional<double> premium_root(double price, double S, double K, double T,
                                                 double r, double q, Side side, double tol,
                                                 const std::optional<AlOpts> &opts,
                                                 std::optional<AlOpts> seed_opts, bool confirm,
                                                 const IvPremiumGate *gate, IvPremiumMemory *memory,
                                                 IvPremiumStats &stats) {
  const auto fail = [&stats](IvPremiumFallback why) {
    stats.fallback = why;
    return std::optional<double>{};
  };
  // K2 (r5): the bench's recorded request stream (american_iv_stream.hpp; disarmed: one
  // atomic load) - every Premium request, refusals included, with the family-native family
  // when the request is native-eligible and the request whose carry it starts from.
  std::uint32_t stream_id = iv_batch::kNoStreamId;
  if (iv_batch::inverse_stream_armed() != nullptr) {
    const bool eligible = gate != nullptr && gate->qfamily != nullptr && gate->qfamily_native &&
                          gate->policy == IvConfirmPolicy::Always && confirm &&
                          gate->qfamily->side() == side;
    iv_batch::InverseStreamRequest rq;
    rq.price = price;
    rq.S = S;
    rq.K = K;
    rq.T = T;
    rq.r = r;
    rq.q = q;
    rq.tol = tol;
    rq.side = side;
    const std::uint32_t carried = memory != nullptr && memory->carry.valid
                                      ? memory->carry.stream_id
                                      : iv_batch::kNoStreamId;
    stream_id = iv_batch::inverse_stream_record(rq, eligible ? gate->qfamily : nullptr, carried);
  }
  // (sigma, q) family iterates (IvPremiumGate::qfamily; set below): the family, and whether
  // the attempt's points are still the family's (a refusal clears it).
  const AlSigmaQFamily *qfamily = nullptr;
  bool on_qfamily = false;
  // The band gate (IvPremiumGate, policy Band; gap report 5a): the seed-tier root may
  // skip its confirm only when its vol-unit error bound (eps_A + eps_r) / v_min is under
  // the quote's half-spread in vol divided by k. eps_A = the seed tier's tested price
  // bound at this contract, eps_r = the root's residual on the seed map, v_min = the seed
  // map's chord at the root (one more seed-tier solve a kAlPackSigmaBump away) less its
  // chord bound h |volga| + 1e-8 (volga from the chord to the previous cold point; a 2x
  // margin when there is none), floored at min_relative_slope max(price, 1); the
  // half-spread is converted with the chord PLUS that margin (the conservative side).
  // Both sides are in vol units (the brief's statement read on the vol scale).
  const auto band_gate_holds = [&](double slope, const ColdPoint &last,
                                   const ColdPoint *partner) -> bool {
    if (gate == nullptr || !(gate->half_spread_usd > 0.0) || !(gate->k > 0.0)) {
      return false;
    }
    const AlProfile seed_profile = seed_opts ? seed_opts->profile : AlProfile::Reference;
    double eps_a = al_profile_price_bound(seed_profile, price, T);
    const double eps_r = std::fabs(last.price - price);
    double h = last.sigma + kAlPackSigmaBump < kSigmaHi ? kAlPackSigmaBump : -kAlPackSigmaBump;
    double bumped = std::numeric_limits<double>::quiet_NaN();
    if (on_qfamily) {
      // B-family: the family's measured bound and its own chord (no Andersen-Lake solve).
      eps_a = gate->qfamily_bound * std::fmax(price, 1.0);
      h = kAlPackSigmaBump;
      const AlSigmaQFamilyChord fc = qfamily->price_and_chord(S, K, last.sigma, q);
      ++stats.family_prices;
      if (fc.status == AlSigmaQFamilyStatus::Ok) bumped = fc.price_sigma_bumped;
    } else {
      const Result<double> pb = andersen_lake(S, K, T, last.sigma + h, r, q, side, seed_opts);
      ++stats.premium_solves;
      ++stats.gate_solves;
      if (pb) bumped = *pb;
    }
    if (!std::isfinite(bumped) || !std::isfinite(eps_a)) {
      return false;
    }
    const double chord = (bumped - last.price) / h;
    double margin = 0.5 * std::fabs(chord) + 1.0e-8;
    if (partner != nullptr && partner->sigma != last.sigma) {
      const double back = (last.price - partner->price) / (last.sigma - partner->sigma);
      const double span = 0.5 * (std::fabs(h) + std::fabs(last.sigma - partner->sigma));
      const double volga = std::fabs(chord - back) / span;
      margin = std::fabs(h) * volga + 1.0e-8;
    }
    const double v_min = std::fmin(chord, slope) - margin;
    const double v_max = std::fmax(chord, slope) + margin;
    const double floor = gate->min_relative_slope * std::fmax(price, 1.0);
    if (!(v_min > 0.0) || !(v_min > floor) || !std::isfinite(v_max)) {
      return false;
    }
    const double bound_vol = (eps_a + eps_r) / v_min;
    stats.gate_bound_vol = bound_vol;
    return bound_vol < gate->half_spread_usd / (v_max * gate->k);
  };
  // An accepted iterate's root; with a seed kernel other than the accurate one it is
  // confirmed by one solve on the confirm kernel (Newton distance <= tol on that map)
  // unless the gate's policy skips the confirm (Never, or Band when the statement holds).
  const auto accept = [&](double root, double slope, const ColdPoint &last,
                          const ColdPoint *partner) -> std::optional<double> {
    if (!(root > kSigmaLo && root < kSigmaHi)) {
      return fail(IvPremiumFallback::VolBound);
    }
    if (confirm) {
      const IvConfirmPolicy policy = gate != nullptr ? gate->policy : IvConfirmPolicy::Always;
      const bool skip = policy == IvConfirmPolicy::Never ||
                        (policy == IvConfirmPolicy::Band && band_gate_holds(slope, last, partner));
      if (skip) {
        stats.confirm_skipped = true;
        stats.fallback = IvPremiumFallback::None;
        return root;
      }
      const std::optional<AlOpts> &confirm_opts =
          gate != nullptr && gate->confirm_opts ? gate->confirm_opts : opts;
      stats.confirm_profile = confirm_opts ? confirm_opts->profile : AlProfile::Reference;
      // A seed-tier root sits up to the tier's price bound off the confirm map (Screen:
      // 7.8e-4 max(P, 1), often > tol in vol), so the confirm is a bounded Newton on the
      // confirm map: a solve whose distance is <= tol accepts (then corrects the root); a
      // larger one takes the step and confirms once more; kIvPremiumConfirmSolves solves
      // at most, else the Search fallback.
      for (std::uint32_t attempt = 0;; ++attempt) {
        counters::ledger::bump(counters::ledger::Solve::IvNewtonIters);
        const Result<double> pc = andersen_lake(S, K, T, root, r, q, side, confirm_opts);
        ++stats.premium_solves;
        ++stats.confirm_solves;
        if (!pc || !std::isfinite(*pc)) {
          return fail(IvPremiumFallback::PricerFailure);
        }
        const double dc = (*pc - price) / slope;
        stats.last_residual_usd = std::fabs(*pc - price);
        if (!std::isfinite(dc)) {
          return fail(IvPremiumFallback::NotIncreasing);
        }
        root -= dc;
        if (!(root > kSigmaLo && root < kSigmaHi)) {
          return fail(IvPremiumFallback::VolBound);
        }
        if (std::fabs(dc) <= tol) {
          break;
        }
        if (attempt + 1u >= kIvPremiumConfirmSolves) {
          return fail(IvPremiumFallback::NoConvergence);
        }
      }
    }
    stats.fallback = IvPremiumFallback::None;
    return root;
  };
  if (!std::isfinite(price) || !std::isfinite(S) || !std::isfinite(K) || !std::isfinite(T) ||
      !std::isfinite(r) || !std::isfinite(q) || !(S > 0.0) || !(K > 0.0) || !(T > 0.0) ||
      !(tol > 0.0)) {
    return fail(IvPremiumFallback::NotApplicable);
  }
  // The Search route's identifiable range [zero_vol, ceiling]. Premium stays strictly
  // inside it and above the non-monotone band, where P_am(sigma) is one increasing
  // branch; at the edges the Search route's own clamps decide.
  const double ceiling = american_price_ceiling(S, K, T, r, q, side);
  const double zero_vol = zero_vol_american_value(S, K, T, r, q, side);
  if (!std::isfinite(ceiling) || !std::isfinite(zero_vol)) {
    return fail(IvPremiumFallback::NotApplicable);
  }
  const double band_tol = 1.0e-9 * ceiling + 1.0e-12;
  const double accurate_gate = al_non_monotone_gate(K, T, opts);
  if (!(price < ceiling - band_tol) || !(price - zero_vol > std::fmax(accurate_gate, band_tol))) {
    return fail(IvPremiumFallback::RangeEdge);
  }
  if (confirm) {
    // Seed-tier routing (L2f note section 4): inside the seed tier's own, wider
    // non-monotone gate its map may dip where the accurate map does not (Screen: 4e-4 K
    // max(1, T)^2, dips at r = q), so the iterates run on the accurate kernel instead -
    // no confirm is then needed, and a seed-tier refusal never reaches the quote. The
    // gate's seed_gate_kappa (opt-in) narrows that band to a multiple of the measured dip.
    if (!(price - zero_vol > iv_premium_seed_gate(K, T, seed_opts, gate))) {
      seed_opts = opts;
      confirm = false;
      stats.seed_routed = true;
    }
  }
  stats.seed_nodes = seed_opts ? seed_opts->n_collocation : kAlAccurateCollocation;
  const Leg leg(S, K, T, r, q, side);
  if (!leg.valid()) {
    return fail(IvPremiumFallback::NotApplicable);
  }
  // Family iterates (IvPremiumGate::family, SPY1s Phase 2): seed-tier iterates only (a
  // routed price iterates on the accurate kernel); the leg at the family's yield gives the
  // remainder D the family carries.
  const AlSigmaFamily *family = confirm && gate != nullptr && gate->family != nullptr &&
                                        gate->policy == IvConfirmPolicy::Always &&
                                        gate->family->side() == side &&
                                        std::isfinite(gate->family_q)
                                    ? gate->family
                                    : nullptr;
  const Leg family_leg(S, K, T, r, family != nullptr ? gate->family_q : q, side);
  if (family != nullptr && !family_leg.valid()) {
    family = nullptr;
  }
  // (sigma, q) family iterates (IvPremiumGate::qfamily, step 5): the strike's own q, no
  // D-model; policy Always or Band (the B-family gate), never Never.
  if (confirm && gate != nullptr && gate->qfamily != nullptr && gate->qfamily->side() == side &&
      gate->policy != IvConfirmPolicy::Never) {
    qfamily = gate->qfamily;
    on_qfamily = true;
    family = nullptr;
  }
  // Family-native iterates (IvPremiumGate::qfamily_native, SPY1s Phase 3 lever 1; K2 r5:
  // one batch-seam lane): the family's own map without the seed leg. An unaccepted attempt
  // hands the price, with its typed outcome in stats.native_status and its bracket kept in
  // the memory's carry, to the seed-leg path on the seed kernel, which keeps the contract's
  // leg memory (native points are never leg points: that path skips them).
  if (on_qfamily && gate->qfamily_native && gate->policy == IvConfirmPolicy::Always) {
    const NativeRoot native = family_native_root(price, S, K, T, r, q, side, tol, *qfamily,
                                                 memory, stats, stream_id);
    if (native.accepted) {
      stats.last_residual_usd = native.residual_usd;
      return accept(native.root, native.slope, native.last, nullptr);
    }
    on_qfamily = false;
    stats.family_refused = true;
  }

  // Up to two cold points, most recent first: the memory's, then this attempt's.
  std::array<ColdPoint, 2> pts{};
  std::size_t n = 0;
  if (memory != nullptr && !memory->native) {
    for (std::size_t i = 0; i < memory->points && i < 2; ++i) {
      const std::optional<double> a = leg.price(memory->sigma[i]);
      if (!a) {
        return fail(IvPremiumFallback::NoEuropeanSeed);
      }
      pts[n++] = ColdPoint{memory->sigma[i], *a + memory->rest[i], memory->rest[i]};
    }
  }
  // The remainder chord through two points with distinct sigmas (nullopt: none).
  const auto chord = [](const ColdPoint &a, const ColdPoint &b) -> std::optional<double> {
    if (a.sigma == b.sigma) {
      return std::nullopt;
    }
    const double e = (a.rest - b.rest) / (a.sigma - b.sigma);
    return std::isfinite(e) ? std::optional<double>{e} : std::nullopt;
  };

  // Seed: the leg inverted at the quote less the remembered remainder (constant, or its
  // chord), started from the European IV (an upper bound) or the remembered sigma.
  std::optional<double> x;
  if (n == 0) {
    if (const std::optional<double> eu = leg.euro_iv(price)) {
      x = leg.solve(price, 0.0, 0.0, 0.0, *eu);
    }
  } else {
    const std::optional<double> e = n >= 2 ? chord(pts[0], pts[1]) : std::nullopt;
    x = leg.solve(price, pts[0].rest, pts[0].sigma, e.value_or(0.0), pts[0].sigma);
  }
  if (!x) {
    return fail(IvPremiumFallback::NoEuropeanSeed);
  }

  const double slope_floor = kCertSlopeFloorRel * K;
  // Newton distances of the last two iterates: a solve beyond the budget is spent only
  // while they shrink geometrically (kIvPremiumShrink per solve).
  double abs_d = std::numeric_limits<double>::infinity();
  double prev_abs_d = std::numeric_limits<double>::infinity();
  for (std::uint32_t solve = 0; solve < kIvPremiumMaxSolves + kIvPremiumExtraSolves; ++solve) {
    if (solve >= kIvPremiumMaxSolves && !(abs_d <= kIvPremiumShrink * prev_abs_d)) {
      return fail(IvPremiumFallback::NoConvergence);
    }
    const double s = *x;
    if (!(s > kSigmaLo && s < kSigmaHi)) {
      return fail(IvPremiumFallback::VolBound);
    }
    counters::ledger::bump(counters::ledger::Solve::IvNewtonIters); // one inversion iterate
    std::optional<double> pa;
    std::optional<double> a;
    if (family != nullptr) {
      // The D-model point A(s; q) + P_family(s; q_family) - A(s; q_family); any refusal
      // sends this and every later iterate of the attempt to the seed kernel.
      a = leg.price(s);
      const AlSigmaFamilyPrice fp = family->price(S, K, s);
      const std::optional<double> af =
          a && fp.status == AlSigmaFamilyStatus::Ok ? family_leg.price(s) : std::nullopt;
      const double model = af ? *a + (fp.price - *af) : std::numeric_limits<double>::quiet_NaN();
      if (std::isfinite(model)) {
        pa = model;
        ++stats.family_prices;
      } else {
        // The D-model points (this attempt's and the memory's) never meet a seed-kernel
        // point in one chord: the attempt restarts its remainder model from this solve.
        family = nullptr;
        stats.family_refused = true;
        n = 0;
        if (memory != nullptr) memory->points = 0;
      }
    } else if (on_qfamily) {
      // The (sigma, q) family's price at the strike's own q (the accurate map to its
      // measured bound); a refusal restarts the attempt's points on the seed kernel.
      const AlSigmaQFamilyPrice fp = qfamily->price(S, K, s, q);
      a = leg.price(s);
      if (fp.status == AlSigmaQFamilyStatus::Ok && std::isfinite(fp.price)) {
        pa = fp.price;
        ++stats.family_prices;
      } else {
        on_qfamily = false;
        stats.family_refused = true;
        n = 0;
        if (memory != nullptr) memory->points = 0;
      }
    }
    if (!pa) {
      const Result<double> cold = andersen_lake(S, K, T, s, r, q, side, seed_opts);
      ++stats.premium_solves;
      if (!a) {
        a = leg.price(s);
      }
      if (!cold || !std::isfinite(*cold)) {
        return fail(IvPremiumFallback::PricerFailure);
      }
      pa = *cold;
    }
    if (!a) {
      return fail(IvPremiumFallback::NoEuropeanSeed);
    }
    const ColdPoint cur{s, *pa, *pa - *a};
    const double res = cur.price - price;
    // Slope of the cold map at the new point: the leg's own slope plus the remainder
    // chord to the most recent point at another sigma (only the nearly flat remainder
    // is approximated by a chord). With no such point, the leg slope alone where the
    // whole remainder is within tol of vol (its chord then moves the Newton step by
    // |D'| / A' times a step that is itself <= tol; exact where D is 0). The raw
    // cold-map chord between the two points must itself rise: one increasing branch.
    std::optional<double> slope;
    const ColdPoint *partner = nullptr;
    for (std::size_t i = 0; i < n; ++i) {
      if (pts[i].sigma != s) {
        partner = &pts[i];
        break;
      }
    }
    const std::optional<double> leg_slope = leg.slope(s);
    if (!leg_slope) {
      return fail(IvPremiumFallback::NoEuropeanSeed);
    }
    if (partner != nullptr) {
      const double raw = (cur.price - partner->price) / (s - partner->sigma);
      if (!(std::isfinite(raw) && raw > 0.0)) {
        return fail(IvPremiumFallback::NotIncreasing);
      }
      slope = *leg_slope + (cur.rest - partner->rest) / (s - partner->sigma);
    } else if (std::fabs(cur.rest) <= tol * *leg_slope) {
      slope = *leg_slope;
    }
    if (memory != nullptr) {
      if (memory->native) memory->points = 0; // never mix family-native and leg points
      memory->native = false;
      memory->sigma[1] = memory->sigma[0];
      memory->rest[1] = memory->rest[0];
      memory->sigma[0] = s;
      memory->rest[0] = cur.rest;
      memory->points = static_cast<std::uint8_t>(std::min<std::size_t>(memory->points + 1u, 2u));
    }
    const std::optional<ColdPoint> prev =
        partner != nullptr ? std::optional<ColdPoint>{*partner} : std::nullopt;
    pts[1] = pts[0];
    pts[0] = cur;
    n = std::min<std::size_t>(n + 1, 2);
    if (!slope) {
      // First cold point with a remainder above tol: the remainder-constant step.
      x = leg.solve(price, cur.rest, s, 0.0, s);
      if (!x) {
        return fail(IvPremiumFallback::NoEuropeanSeed);
      }
      continue;
    }
    if (!(std::isfinite(*slope) && *slope > slope_floor)) {
      return fail(IvPremiumFallback::NotIncreasing);
    }
    const double d = res / *slope;
    prev_abs_d = abs_d;
    abs_d = std::fabs(d);
    if (abs_d <= tol) {
      stats.last_residual_usd = std::fabs(res);
      return accept(s - d, *slope, cur, prev ? &*prev : nullptr);
    }
    // Next iterate: the leg plus the remainder chord through the two latest cold points,
    // solved exactly; the plain Newton step on the cold map if that model has no root.
    std::optional<double> next;
    if (prev) {
      if (const auto e = chord(cur, *prev)) {
        next = leg.solve(price, cur.rest, s, *e, s - d);
      }
    }
    x = next ? *next : s - d;
    if (*x == s) {
      return fail(IvPremiumFallback::NoConvergence);
    }
  }
  return fail(IvPremiumFallback::NoConvergence);
}

[[nodiscard]] bool premium_route(const IvOpts &opts, Side side) noexcept {
  return opts.method == AmericanMethod::AndersenLake && !cache_usable(opts.correction, side) &&
         opts.warm_start == 0.0 && opts.price_tol == 0.0;
}

// The kernel the Premium iterates run on (IvOpts::seed_profile): al_opts itself when
// the profile is unset or is al_opts' own profile (then every solve is the accurate
// confirming solve); AlProfile::Screen selects the measured tier al_screen_opts() (the
// only Screen scheme with a tested bound; american_iv.hpp); any other profile al_opts'
// scheme on that profile.
[[nodiscard]] std::optional<AlOpts> premium_seed_opts(const IvOpts &opts) noexcept {
  const AlProfile own = opts.al_opts ? opts.al_opts->profile : AlProfile::Reference;
  if (!opts.seed_profile || *opts.seed_profile == own) {
    return opts.al_opts;
  }
  if (*opts.seed_profile == AlProfile::Screen) {
    return al_screen_opts();
  }
  AlOpts seed = al_fast_profile(opts.al_opts); // the explicit scheme of al_opts
  seed.profile = *opts.seed_profile;
  return seed;
}

// The Premium strategy: its root when accepted (then the same opt-in refusal gate as
// Search), else the Search route unchanged. `gate` governs a seed-tier root's confirm
// (nullptr: Always on al_opts).
[[nodiscard]] IvResult premium_typed(double price, double S, double K, double T, double r,
                                     double q, Side side, const IvOpts &opts,
                                     IvPremiumMemory *memory, IvPremiumStats &stats,
                                     const IvPremiumGate *gate) {
  stats = IvPremiumStats{};
  if (premium_route(opts, side)) {
    const std::optional<AlOpts> seed_opts = premium_seed_opts(opts);
    const AlProfile own = opts.al_opts ? opts.al_opts->profile : AlProfile::Reference;
    const bool confirm = opts.seed_profile.has_value() && *opts.seed_profile != own;
    if (const std::optional<double> root =
            premium_root(price, S, K, T, r, q, side, opts.tol, opts.al_opts, seed_opts, confirm,
                         confirm ? gate : nullptr, memory, stats)) {
      if (opts.refuse_non_monotone &&
          !scalar_answer_certified(price, S, K, T, r, q, side, *root, opts.al_opts)) {
        return IvResult{std::numeric_limits<double>::quiet_NaN(), IvStatus::NonMonotoneRegion};
      }
      return IvResult{*root, IvStatus::Ok};
    }
  }
  return search_typed(price, S, K, T, r, q, side, opts);
}

} // namespace

IvResult american_implied_vol(double price, double S, double K, double T, double r, double q,
                              Side side, const IvOpts &opts) noexcept {
  try {
    if (opts.strategy == IvStrategy::Premium) {
      IvPremiumStats stats;
      return premium_typed(price, S, K, T, r, q, side, opts, nullptr, stats, nullptr);
    }
    return search_typed(price, S, K, T, r, q, side, opts);
  } catch (const std::bad_alloc &) {
    return IvResult{std::numeric_limits<double>::quiet_NaN(), IvStatus::Unpriceable};
  }
}

IvResult american_implied_vol_premium(double price, double S, double K, double T, double r,
                                      double q, Side side, const IvOpts &opts,
                                      IvPremiumMemory *memory, IvPremiumStats &stats,
                                      const IvPremiumGate *gate) noexcept {
  try {
    return premium_typed(price, S, K, T, r, q, side, opts, memory, stats, gate);
  } catch (const std::bad_alloc &) {
    return IvResult{std::numeric_limits<double>::quiet_NaN(), IvStatus::Unpriceable};
  }
}

// Test/measurement seam (declared in american_iv_test.cpp, not the public
// header). Runs the cold Andersen-Lake inversion and additionally reports the
// polish bracket [xl, xh] captured at polish entry, whether the A3
// bracket-clamp engaged on any polish iterate, and — for price_tol > 0 —
// whether the price-unit extension evaluated a beyond-drift-cap iterate
// (`bolted_out`) and whether it rejected one that failed to reduce the
// cold-map residual (`bolt_reverted_out`), so the tests can pin both the
// bracket invariant and the preserved collapsed-vega protection.
Result<double> american_implied_vol_polish_traced(double price, double S, double K, double T,
                                                  double r, double q, Side side, double tol,
                                                  std::uint16_t max_iter,
                                                  const std::optional<AlOpts> &opts,
                                                  double warm_start, double &xl_out, double &xh_out,
                                                  bool &polish_ran_out, bool &polish_clamped_out,
                                                  double price_tol, bool *bolted_out,
                                                  bool *bolt_reverted_out) {
  PolishTrace tr;
  Result<double> iv = american_implied_vol_impl<CorrectionCache>(
      price, S, K, T, r, q, side, AmericanMethod::AndersenLake, tol, max_iter, opts,
      static_cast<const CorrectionCache *>(nullptr), warm_start, price_tol, &tr);
  xl_out = tr.xl;
  xh_out = tr.xh;
  polish_ran_out = tr.ran;
  polish_clamped_out = tr.clamped;
  if (bolted_out != nullptr) {
    *bolted_out = tr.bolted;
  }
  if (bolt_reverted_out != nullptr) {
    *bolt_reverted_out = tr.bolt_reverted;
  }
  return iv;
}

Result<std::size_t> american_implied_vol_batch(std::span<const double> price, double S,
                                               std::span<const double> K, double T, double r,
                                               double q, Side side, std::span<double> iv_out,
                                               std::span<Status> status_out, AmericanMethod method,
                                               double tol, std::uint16_t max_iter,
                                               const std::optional<AlOpts> &opts,
                                               const CorrectionCache *correction,
                                               bool warm_start_chain) {
  const std::size_t n = price.size();
  if (K.size() != n || iv_out.size() != n || status_out.size() != n) {
    return Err(ErrorCode::InvalidArgument, "american_implied_vol_batch: span length mismatch");
  }
  // P6 (perf F9): adjacent-lane warm-start chaining. All lanes share one side and
  // (S, T, r, q); only (price, K) vary. On a strike-sorted batch the previous
  // lane's converged root is a near-exact seed for the next (adjacent-strike IVs
  // are near-equal). The seed is used only when the previous lane converged AND
  // its strike is within one documented log-moneyness step (guards an unsorted or
  // gapped batch); otherwise the per-quote European seed is used. The seed only
  // shifts the Newton path, so iv_out is economic-parity to the cold path (bounded
  // by the inverter's warm_start invariance: < 1e-9 cached, ~1e-6 cold-AL).
  constexpr double kWarmChainMaxMoneynessStep = 0.15; // ln(K/K_prev) band; a smile step this
                                                      // wide keeps the prior root in the
                                                      // pricer's warm-reseed band
  double warm = 0.0;     // previous lane's converged root (0 = cold seed)
  double warm_lnK = 0.0; // its ln(K)
  bool have_warm = false;
  for (std::size_t i = 0; i < n; ++i) {
    double warm_seed = 0.0;
    if (warm_start_chain && have_warm && K[i] > 0.0 &&
        std::fabs(std::log(K[i]) - warm_lnK) <= kWarmChainMaxMoneynessStep) {
      warm_seed = warm;
    }
    Result<double> iv = american_implied_vol(price[i], S, K[i], T, r, q, side, method, tol,
                                             max_iter, opts, correction, warm_seed);
    if (iv) {
      iv_out[i] = *iv;
      status_out[i] = Ok();
      if (warm_start_chain && K[i] > 0.0) {
        warm = *iv;
        warm_lnK = std::log(K[i]);
        have_warm = true;
      }
    } else {
      // Match the library batch convention: NaN value slot + parallel status.
      iv_out[i] = std::numeric_limits<double>::quiet_NaN();
      status_out[i] = Err(iv.error());
    }
  }
  return Ok(n);
}

} // namespace atx::vol
