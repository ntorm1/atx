#include "atx/vol/api/pricing/american_iv.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new> // std::bad_alloc (the noexcept-boundary containment below)
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "pricing/al_probe.hpp" // env-gated AL hot-path zone timers (Perf 2b step 1)
#include "pricing/american_family_error.hpp" // K3 envelope + root acceptance
#include "pricing/american_iv_batch.hpp"  // K2: the family-native lane
#include "pricing/american_iv_premium.hpp"
#include "pricing/american_iv_search.hpp"
#include "pricing/american_iv_stream.hpp" // K2: the bench's recorded request stream
#include "atx/core/error.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_region.hpp" // C1 classifier (K4 native route)
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/fitting/correction.hpp" // CorrectionCache, american_price_cached hot path
#include "fitting/counters.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp" // K4b: the batched confirm prices
#include "atx/vol/api/simd/cpu.hpp"                     // K4b: simd::use_avx2()

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

// The retained ALO pricer is C1's AloRegionPricer (american_region.hpp; K4 wiring of the
// fourth KernelFloor site): under AlExerciseRegion::KernelFloor with no class output it is
// AloPricer operation for operation (AloRegionPricerKernelFloorIsAloPricerBitForBit), so the
// Search route moves no bits; the native fit route never reaches this residual map.
struct ThreadAloSlot {
  std::optional<AloRegionPricer> pricer;
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

  [[nodiscard]] AloRegionPricer &get() noexcept { return *pricer_; }

private:
  ThreadAloSlot *slot_{nullptr};
  std::optional<AloRegionPricer> fallback_;
  AloRegionPricer *pricer_{nullptr};
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
      const double p = alo->get().price(sigma, AlExerciseRegion::KernelFloor);
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

double iv_premium_edge_band_top(double S, double K, double T, double r, double q, Side side,
                                const std::optional<AlOpts> &opts) noexcept {
  const double ceiling = american_price_ceiling(S, K, T, r, q, side);
  const double zero_vol = zero_vol_american_value(S, K, T, r, q, side);
  const double band_tol = 1.0e-9 * ceiling + 1.0e-12; // premium_root's own arithmetic
  return zero_vol + std::fmax(al_non_monotone_gate(K, T, opts), band_tol); // NaN propagates
}

bool iv_premium_native_window(double price, double S, double K, double T, double r, double q,
                              Side side, const std::optional<AlOpts> &opts) noexcept {
  if (!std::isfinite(price) || !std::isfinite(S) || !std::isfinite(K) || !std::isfinite(T) ||
      !std::isfinite(r) || !std::isfinite(q) || !(S > 0.0) || !(K > 0.0) || !(T > 0.0)) {
    return false;
  }
  // premium_root's own arithmetic (its RangeEdge test and the native route's typed edges).
  const double ceiling = american_price_ceiling(S, K, T, r, q, side);
  const double zero_vol = zero_vol_american_value(S, K, T, r, q, side);
  if (!std::isfinite(ceiling) || !std::isfinite(zero_vol)) return false;
  const double band_tol = 1.0e-9 * ceiling + 1.0e-12;
  return price < ceiling - band_tol &&
         price - zero_vol > std::fmax(al_non_monotone_gate(K, T, opts), band_tol);
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
                                    double q, Side side, const IvOpts &opts,
                                    std::optional<double> *rejected_candidate = nullptr) {
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
    if (rejected_candidate != nullptr) *rejected_candidate = *iv;
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

// K4b (r6): charges its lifetime to one inverse sub-clock of the request
// (IvPremiumStats::sub_ns) when the request is timed; one branch and no clock read otherwise.
class SubTimer {
  using Steady = std::chrono::steady_clock;

public:
  SubTimer(IvPremiumStats &stats, IvPremiumSubClock clock) noexcept
      : slot_(stats.timed ? &stats.sub_ns[static_cast<std::size_t>(clock)] : nullptr),
        start_(slot_ != nullptr ? Steady::now() : Steady::time_point{}) {}
  ~SubTimer() {
    if (slot_ == nullptr) return;
    const auto ns = (Steady::now() - start_) / std::chrono::nanoseconds{1};
    *slot_ += ns > 0 ? static_cast<std::uint64_t>(ns) : 0u;
  }
  SubTimer(const SubTimer &) = delete;
  SubTimer &operator=(const SubTimer &) = delete;
  SubTimer(SubTimer &&) = delete;
  SubTimer &operator=(SubTimer &&) = delete;

private:
  std::uint64_t *slot_;
  Steady::time_point start_;
};

// K4b (r6): the seed-leg route's sub-clock - its lifetime less the confirm solves charged to
// IvPremiumSubClock::Confirm meanwhile (the leg's own Newton confirms).
class LegTimer {
  using Steady = std::chrono::steady_clock;

public:
  explicit LegTimer(IvPremiumStats &stats) noexcept
      : stats_(stats.timed ? &stats : nullptr),
        confirm0_(stats.sub_ns[static_cast<std::size_t>(IvPremiumSubClock::Confirm)]),
        start_(stats_ != nullptr ? Steady::now() : Steady::time_point{}) {}
  ~LegTimer() {
    if (stats_ == nullptr) return;
    const auto ns = (Steady::now() - start_) / std::chrono::nanoseconds{1};
    const std::uint64_t confirm =
        stats_->sub_ns[static_cast<std::size_t>(IvPremiumSubClock::Confirm)] - confirm0_;
    const std::uint64_t total = ns > 0 ? static_cast<std::uint64_t>(ns) : 0u;
    stats_->sub_ns[static_cast<std::size_t>(IvPremiumSubClock::Leg)] +=
        total > confirm ? total - confirm : 0u;
  }
  LegTimer(const LegTimer &) = delete;
  LegTimer &operator=(const LegTimer &) = delete;
  LegTimer(LegTimer &&) = delete;
  LegTimer &operator=(LegTimer &&) = delete;

private:
  IvPremiumStats *stats_;
  std::uint64_t confirm0_;
  Steady::time_point start_;
};

// K4b (r6): the prepared values of the request premium_root is running (IvPremiumGate::
// prepared), installed for its duration by PreparedScope; the lane, the enclosure and the
// first confirm read them and consume one only on bit-identical inputs.
thread_local const IvPremiumPrepared *t_prepared = nullptr;

class PreparedScope {
public:
  explicit PreparedScope(const IvPremiumPrepared *prepared) noexcept : prev_(t_prepared) {
    t_prepared = prepared;
  }
  ~PreparedScope() { t_prepared = prev_; }
  PreparedScope(const PreparedScope &) = delete;
  PreparedScope &operator=(const PreparedScope &) = delete;
  PreparedScope(PreparedScope &&) = delete;
  PreparedScope &operator=(PreparedScope &&) = delete;

private:
  const IvPremiumPrepared *prev_;
};

// Bit equality (a NaN equals the same NaN): what "the same inputs" means for a prepared value.
[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] bool same_opts(const std::optional<AlOpts> &a, const std::optional<AlOpts> &b) noexcept {
  if (a.has_value() != b.has_value()) return false;
  if (!a) return true;
  return a->n_collocation == b->n_collocation && a->n_quadrature == b->n_quadrature &&
         a->n_quad_price == b->n_quad_price && a->max_newton_iter == b->max_newton_iter &&
         same_bits(a->tol, b->tol) && a->profile == b->profile;
}

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
  // K4: what the lane learned, accepted or not (the native fit route's reference hint):
  // its best iterate, the last evaluated point with its fused vega, and the family-map
  // bracket at exit (NaN: an unevaluated end). Not certified on the target map.
  double sigma = std::numeric_limits<double>::quiet_NaN();
  double last_sigma = std::numeric_limits<double>::quiet_NaN();
  double last_price = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();
  double lo = std::numeric_limits<double>::quiet_NaN();
  double hi = std::numeric_limits<double>::quiet_NaN();
};

static_assert(kIvPremiumNativeEvaluations == iv_batch::kInverseMaxEvaluations,
              "the native attempt's budget is the batch seam's lane cap");

// The seed and retained bracket a family-native lane starts from (K2): the contract's native
// carry - the evaluated points of an earlier attempt on this contract (the quote's mid behind
// its bid / ask) bracket the price on the side their prices are on, and the last one with its
// vega gives a Newton seed - else the Black-76 IV of the price at the contract's forward (an
// upper bound of the American IV). One function for the one-lane route and the batched route
// (K4b), so a prepared lane's inputs are the route's own. Not noexcept: the Black-76
// inversion's Error path builds a message string.
struct LaneStart {
  double seed = std::numeric_limits<double>::quiet_NaN();
  double blo = std::numeric_limits<double>::quiet_NaN();
  double bhi = std::numeric_limits<double>::quiet_NaN();
  double plo = std::numeric_limits<double>::quiet_NaN();
  double phi = std::numeric_limits<double>::quiet_NaN();
  bool carried = false;
};

[[nodiscard]] LaneStart lane_start(double price, double S, double K, double T, double r, double q,
                                   Side side, const IvPremiumNativeCarry *carry) {
  LaneStart st;
  if (carry != nullptr && carry->valid) {
    const IvPremiumNativeCarry &c = *carry;
    st.carried = true;
    const auto take = [&](double s, double p) {
      if (!std::isfinite(s) || !std::isfinite(p)) return;
      if (p < price) {
        if (!(st.blo >= s)) {
          st.blo = s;
          st.plo = p;
        }
      } else if (!(st.bhi <= s)) {
        st.bhi = s;
        st.phi = p;
      }
    };
    take(c.lo, c.p_lo);
    take(c.hi, c.p_hi);
    take(c.sigma, c.price);
    if (std::isfinite(c.vega) && c.vega > iv_batch::kInverseVegaFloorRel * K) {
      st.seed = c.sigma + (price - c.price) / c.vega;
    }
  }
  if (!std::isfinite(st.seed)) {
    const double F = S * std::exp((r - q) * T);
    const Result<double> eu = implied_vol(price, F, K, T, std::exp(-r * T), side);
    if (eu && std::isfinite(*eu)) st.seed = *eu;
  }
  return st;
}

// K4b: whether a prepared lane was computed from exactly these lane inputs.
[[nodiscard]] bool prepared_lane_matches(const IvPreparedLane &p, const AlSigmaQFamily &f,
                                         double S, double K, double price, double q, double tol,
                                         const LaneStart &st) noexcept {
  return p.valid && p.family == &f && same_bits(p.S, S) && same_bits(p.K, K) &&
         same_bits(p.target, price) && same_bits(p.q, q) && same_bits(p.tol, tol) &&
         same_bits(p.seed, st.seed) && same_bits(p.bracket_lo, st.blo) &&
         same_bits(p.bracket_hi, st.bhi) && same_bits(p.price_lo, st.plo) &&
         same_bits(p.price_hi, st.phi);
}

// The family-native iterates (SPY1s Phase 3 lever 1) through the K2 batch seam: ONE lane of
// iv_batch::invert_many on the (sigma, q) family's own map at the strike's own q, a bounded
// bracketed Newton on the fused price + vega (no seed leg, no chord), started as lane_start
// says; a failed or out-of-box seed starts from the bracket (the box top is priced AT the
// box top, mn5). K4b: a lane the batched route prepared from exactly these inputs is taken
// instead of the one-lane call. Every attempt rewrites the carry, accepted or not. Not
// noexcept: the Black-76 inversion's Error path builds a message string.
[[nodiscard]] NativeRoot family_native_root(double price, double S, double K, double T, double r,
                                            double q, Side side, double tol,
                                            const AlSigmaQFamily &f, IvPremiumMemory *memory,
                                            IvPremiumStats &stats, std::uint32_t stream_id) {
  namespace ib = iv_batch;
  NativeRoot out;
  const LaneStart start =
      lane_start(price, S, K, T, r, q, side, memory != nullptr ? &memory->carry : nullptr);
  if (start.carried) stats.native_carried = true;
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
  const IvPreparedLane *prepared =
      t_prepared != nullptr && t_prepared->lane.valid ? &t_prepared->lane : nullptr;
  if (prepared != nullptr && prepared_lane_matches(*prepared, f, S, K, price, q, tol, start)) {
    // K4b: the batched route's lane for exactly these inputs (iv_native_prepare_lanes).
    sigma = prepared->sigma;
    lo = prepared->lo;
    hi = prepared->hi;
    p_lo = prepared->p_lo;
    p_hi = prepared->p_hi;
    res = prepared->residual;
    vega = prepared->vega;
    last = prepared->last;
    evals = prepared->evaluations;
    status = prepared->status;
    flags = prepared->flags;
    stats.batch_lane_hit = true;
  } else {
    if (prepared != nullptr) stats.batch_lane_miss = true;
    const std::uint8_t is_call = side == Side::Call ? 1u : 0u;
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
    in.seed_sigma = std::span<const double>(&start.seed, 1);
    in.bracket_lo = std::span<const double>(&start.blo, 1);
    in.bracket_hi = std::span<const double>(&start.bhi, 1);
    in.price_lo = std::span<const double>(&start.plo, 1);
    in.price_hi = std::span<const double>(&start.phi, 1);
    const ib::InverseBatchOut o{{&sigma, 1}, {&lo, 1},     {&hi, 1},    {&p_lo, 1},
                                {&p_hi, 1},  {&res, 1},    {&vega, 1},  {&last, 1},
                                {&evals, 1}, {&status, 1}, {&flags, 1}, {&acc, 1}};
    // One scratch per thread (built once; the one-lane call then allocates nothing).
    thread_local ib::InverseScratch scratch;
    ib::InverseBatchError batch_error = ib::InverseBatchError::None;
    {
      const SubTimer lane_clock(stats, IvPremiumSubClock::Lane);
      batch_error = ib::invert_many(in, o, scratch);
    }
    if (batch_error != ib::InverseBatchError::None) {
      stats.native_status = ib::InverseStatus::InvalidInput;
      return out;
    }
  }
  stats.native_status = status;
  stats.native_evaluations = evals;
  stats.native_flags = flags;
  stats.family_prices += evals;
  out.sigma = sigma;
  out.last_sigma = last;
  out.vega = vega;
  out.lo = lo;
  out.hi = hi;
  const double last_price = std::isfinite(res) ? price + res : res;
  out.last_price = last_price;
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

// ── Root acceptance of the (sigma, q) family route (lane K3; american_family_error.hpp) ──
//
// THE RULE. A is the target map (the family's cold accurate kernel at the strike's own q),
// C the confirm kernel (|C - A| <= eps_C, al_profile_price_bound: 0 on Reference, which
// is A). On the domain premium_root admits (strictly inside the identifiable range, above
// the non-monotone band) A is continuous and increasing, so a bracket [a, b] whose ends
// carry CERTIFIED sides, A(a) <= price <= A(b), contains A's root. A side is certified
//   by the family: P_family -/+ eps on the far side of the price, eps the validated
//                  envelope's per-point bound (|P_family - A| <= eps);
//   by C:          C -/+ eps_C on the far side of the price.
//   Skipped:   both ends by the family (two family prices, no confirm solve), b - a <= W;
//   Confirmed: at least one end by C (confirm_bracket), b - a <= W;
// W = min(tol, kIvRootRequirement) (less one part in 2^20), so the served root, clamped
// into [a, b], lies within W of A's root. No step length and no secant slope is a bound:
// a local chord only PLACES an end and the served point.

// A far root's bracket (confirm_bracket): centred on the chord's Newton point, half-width
// this fraction of W (the estimate's own error must stay inside the remaining 0.05 W).
constexpr double kFarRootHalfWidth = 0.45;

// The enclosure's upper end (family_enclosure) moves toward the lower one by at most this
// many ulps so that the bracket's COMPUTED width meets b - a <= w (lane K5): one step within
// a binade, two where b sits on a power of two.
constexpr int kEnclosureWidthSteps = 4;

// The bracket width the acceptance closes.
[[nodiscard]] double acceptance_width(double tol) noexcept {
  return std::fmin(tol, kIvRootRequirement) * (1.0 - 0x1p-20);
}

} // namespace

// Encloses A's root in [x - w/2, x + w/2] (the upper end stepped toward the lower one where
// rounding widens the computed bracket past w; below) from two family prices with their
// bounds, each certified at the end it serves; a second, re-centred attempt (on the local
// chord's root) when the first missed only by its centre. Without a covering envelope it
// measures the local chord only. Declared in american_iv_premium.hpp (FamilyEnclosure) so its
// tests can place x off the root.
FamilyEnclosure family_enclosure(double price, double S, double K, double q, double x, double w,
                                 const AlSigmaQFamily &f, const ValidatedErrorEnvelope *env,
                                 IvPremiumStats &stats) noexcept {
  static const ValidatedErrorEnvelope kNoEnvelope{};
  const ValidatedErrorEnvelope &e = env != nullptr ? *env : kNoEnvelope;
  FamilyEnclosure out;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const double a = x - 0.5 * w;
    // The two roundings leave b - a one ulp of x above w exactly when frac(w / (2 ulp(x))) >
    // 1/2, a per-binade condition (both ends in x's binade). At w = acceptance_width(1e-7) on
    // [0.005, 5]: x in [0.005, 2^-7), [2^-6, 2^-2), [1, 2) and [4, 5]; not [2^-7, 2^-6),
    // [0.25, 1) or [2, 4). b steps toward a until the bracket meets the width rule below AS
    // COMPUTED; both ends are priced and certified after the step (the served ones). The rule
    // is unchanged (b - a is exact here: w << a, Sterbenz).
    double b = x + 0.5 * w;
    for (int step = 0; step < kEnclosureWidthSteps && !(b - a <= w); ++step) {
      b = std::nextafter(b, a);
    }
    const FamilyBoundedPrice pa = family_price_bounded(f, e, S, K, a, q);
    const FamilyBoundedPrice pb = family_price_bounded(f, e, S, K, b, q);
    stats.family_prices += 2u;
    if (pa.status != AlSigmaQFamilyStatus::Ok || pb.status != AlSigmaQFamilyStatus::Ok) {
      return out;
    }
    out.slope = (pb.price - pa.price) / (b - a);
    if (!e.covered() || !(b - a <= w)) {
      return out;
    }
    if (pa.price + pa.bound_usd <= price && price <= pb.price - pb.bound_usd) {
      const double gap = pb.price - pa.price;
      const double frac = gap > 0.0 ? (price - pa.price) / gap : 0.5;
      out.enclosed = true;
      out.lo = a;
      out.hi = b;
      out.root = std::clamp(a + frac * (b - a), a, b);
      return out;
    }
    const bool fits = (pb.price - pb.bound_usd) - (pa.price + pa.bound_usd) > 0.0;
    const bool off_centre = price < pa.price || price > pb.price;
    if (!(fits && off_centre && out.slope > 0.0)) {
      return out;
    }
    x = a + (price - pa.price) / out.slope;
  }
  return out;
}

namespace {

// One solve of the confirm kernel at sigma (charged as a confirming solve).
[[nodiscard]] std::optional<double> confirm_price(double S, double K, double T, double sigma,
                                                  double r, double q, Side side,
                                                  const std::optional<AlOpts> &confirm_opts,
                                                  IvPremiumStats &stats) {
  counters::ledger::bump(counters::ledger::Solve::IvNewtonIters);
  // K4b: the batched route's first confirm price for exactly this solve (decisive only:
  // iv_native_prepare_acceptance) is this solve's answer; still one confirming solve.
  if (t_prepared != nullptr && t_prepared->confirm.valid) {
    const IvPreparedConfirm &pc = t_prepared->confirm;
    if (same_bits(pc.S, S) && same_bits(pc.K, K) && same_bits(pc.T, T) &&
        same_bits(pc.sigma, sigma) && same_bits(pc.r, r) && same_bits(pc.q, q) &&
        pc.side == side && same_opts(pc.opts, confirm_opts)) {
      ++stats.premium_solves;
      ++stats.confirm_solves;
      stats.batch_confirm_hit = true;
      return pc.price;
    }
  }
  const SubTimer confirm_clock(stats, IvPremiumSubClock::Confirm);
  const Result<double> pc = andersen_lake(S, K, T, sigma, r, q, side, confirm_opts);
  ++stats.premium_solves;
  ++stats.confirm_solves;
  if (!pc || !std::isfinite(*pc)) {
    return std::nullopt;
  }
  return *pc;
}

// Which side of the target A's root sigma sits on, from one confirm price c there: +1 when
// A(sigma) >= price is certified (c - eps_c >= price), -1 when A(sigma) <= price is (c +
// eps_c <= price), 0 when C's tested distance eps_c to A leaves it open.
[[nodiscard]] int confirm_side(double c, double price, double eps_c) noexcept {
  if (c - eps_c >= price) return 1;
  if (c + eps_c <= price) return -1;
  return 0;
}

// The same from the family: the side of A at sigma that the envelope certifies (0 when
// there is no covering envelope, the family refuses, or the bound leaves it open).
[[nodiscard]] int family_side(double price, double S, double K, double q, double sigma,
                              const AlSigmaQFamily &f, const ValidatedErrorEnvelope *env,
                              IvPremiumStats &stats) noexcept {
  if (env == nullptr || !env->covered()) {
    return 0;
  }
  const SubTimer family_clock(stats, IvPremiumSubClock::FamilyAccept);
  const FamilyBoundedPrice fp = family_price_bounded(f, *env, S, K, sigma, q);
  ++stats.family_prices;
  if (fp.status != AlSigmaQFamilyStatus::Ok) {
    return 0;
  }
  if (fp.price - fp.bound_usd >= price) return 1;
  if (fp.price + fp.bound_usd <= price) return -1;
  return 0;
}

// The confirm of a family-native root x (lane K3; replaces the secant-slope Newton distance
// of R1-A MJ1). Every end of the accepted bracket carries a CERTIFIED side of A's root: a
// confirm price beyond C's tested distance eps_c (confirm_side), or a family price beyond
// its envelope bound (family_side); A is continuous and increasing on the domain, so its
// root lies between two ends of opposite sides, and an accepted bracket is no wider than w.
// At most three confirm solves: C at x, then the far end (the family first, C second)
// within w of x; for a far root (the chord's Newton step beyond w / 4) both ends around
// the Newton point at -/+ kFarRootHalfWidth w (the family first, C second); when C leaves
// x's side open, both ends at x -/+ w/2 (the family first, then one C solve).
// `slope` (a local chord: the enclosure's span <= w, or the family's kAlPackSigmaBump chord)
// only PLACES the far end and the served point; it bounds nothing. nullopt with
// stats.acceptance PriceOnly (low vega: slope w <= 2 eps_c, no bracket of width w can be
// certified by C), Interval (a certified bracket wider than w) or None (no bracket) and
// stats.fallback set: the caller then runs Search.
[[nodiscard]] std::optional<double> confirm_bracket(double price, double S, double K, double T,
                                                    double r, double q, Side side, double x,
                                                    double slope, double w,
                                                    const AlSigmaQFamily &f,
                                                    const ValidatedErrorEnvelope *env,
                                                    const std::optional<AlOpts> &confirm_opts,
                                                    IvPremiumStats &stats) {
  RootAcceptance &acc = stats.acceptance;
  acc.error_class = env != nullptr && env->covered() ? env->error_class
                                                     : FamilyErrorClass::Uncovered;
  const auto fail = [&](IvPremiumFallback why, RootAcceptanceKind kind,
                        RootAcceptanceReason reason) -> std::optional<double> {
    acc.kind = kind;
    acc.reason = reason;
    stats.fallback = why;
    return std::nullopt;
  };
  // An accepted bracket [lo, hi] (A's side certified -1 at lo, +1 at hi), the served point
  // the falsi of the two prices (or the chord placement) clamped into it.
  const auto accept = [&](double root, double lo, double hi,
                          RootAcceptanceReason reason) -> std::optional<double> {
    acc.kind = RootAcceptanceKind::Confirmed;
    acc.reason = reason;
    acc.lo = lo;
    acc.hi = hi;
    acc.bound_iv = hi - lo;
    stats.fallback = IvPremiumFallback::None;
    return std::clamp(root, lo, hi);
  };
  const auto interval = [&](double lo, double hi) -> std::optional<double> {
    acc.lo = lo;
    acc.hi = hi;
    acc.bound_iv = hi - lo;
    return fail(IvPremiumFallback::NoConvergence, RootAcceptanceKind::Interval,
                RootAcceptanceReason::BudgetExhausted);
  };
  const auto inside = [](double s) { return s > kSigmaLo && s < kSigmaHi; };
  const AlProfile profile = confirm_opts ? confirm_opts->profile : AlProfile::Reference;
  stats.confirm_profile = profile;
  const double eps_c = al_profile_price_bound(profile, price, T);
  if (!(std::isfinite(slope) && slope > 0.0) || !(slope * w > 2.0 * eps_c)) {
    return fail(IvPremiumFallback::NotIncreasing, RootAcceptanceKind::PriceOnly,
                RootAcceptanceReason::LowVega);
  }
  const std::optional<double> c0 = confirm_price(S, K, T, x, r, q, side, confirm_opts, stats);
  if (!c0) {
    return fail(IvPremiumFallback::PricerFailure, RootAcceptanceKind::None,
                RootAcceptanceReason::None);
  }
  stats.last_residual_usd = std::fabs(*c0 - price);
  const int s0 = confirm_side(*c0, price, eps_c);
  if (s0 == 0) {
    // C cannot place x: x sits within eps_c / slope of the root. The ends x -/+ w/2, each
    // certified by the family first, else by one more confirm solve.
    const double lo = x - 0.5 * w;
    const double hi = x + 0.5 * w;
    if (!inside(lo) || !inside(hi)) {
      return fail(IvPremiumFallback::VolBound, RootAcceptanceKind::None,
                  RootAcceptanceReason::BudgetExhausted);
    }
    int s_lo = family_side(price, S, K, q, lo, f, env, stats);
    int s_hi = family_side(price, S, K, q, hi, f, env, stats);
    const bool family_end = s_lo == -1 || s_hi == 1;
    double c_lo = std::numeric_limits<double>::quiet_NaN();
    double c_hi = std::numeric_limits<double>::quiet_NaN();
    for (const bool upper : {false, true}) {
      int &end_side = upper ? s_hi : s_lo;
      const int want = upper ? 1 : -1;
      if (end_side == want) continue;
      const std::optional<double> ce =
          confirm_price(S, K, T, upper ? hi : lo, r, q, side, confirm_opts, stats);
      if (!ce) {
        return fail(IvPremiumFallback::PricerFailure, RootAcceptanceKind::None,
                    RootAcceptanceReason::None);
      }
      stats.last_residual_usd = std::fabs(*ce - price);
      (upper ? c_hi : c_lo) = *ce;
      end_side = confirm_side(*ce, price, eps_c);
      if (end_side != want) {
        return fail(IvPremiumFallback::NoConvergence, RootAcceptanceKind::None,
                    RootAcceptanceReason::BudgetExhausted);
      }
    }
    const double gap = c_hi - c_lo;
    const double root = std::isfinite(gap) && gap != 0.0 ? lo + (price - c_lo) / gap * w : x;
    return accept(root, lo, hi,
                  family_end ? RootAcceptanceReason::ConfirmAndFamily
                             : RootAcceptanceReason::ConfirmBothEnds);
  }
  const int dir = -s0; // A increasing: the root lies on this side of x
  const double delta = (price - *c0) / slope;
  if (4.0 * std::fabs(delta) > w) {
    // A far root: the chord puts the confirm map's root more than w / 4 from x, so no
    // bracket with x as an end fits in w. The bracket is placed around the chord's Newton
    // point instead, each end certified by the family first, else by C (two more solves).
    const double centre = x + delta;
    const double near_end = centre - dir * kFarRootHalfWidth * w; // x's side: needs s0
    const double far_end = centre + dir * kFarRootHalfWidth * w;  // needs -s0
    if (!inside(near_end) || !inside(far_end)) {
      return fail(IvPremiumFallback::VolBound, RootAcceptanceKind::None,
                  RootAcceptanceReason::BudgetExhausted);
    }
    int s_near = family_side(price, S, K, q, near_end, f, env, stats);
    int s_far = family_side(price, S, K, q, far_end, f, env, stats);
    const bool family_end = s_near == s0 || s_far == -s0;
    double c_near = std::numeric_limits<double>::quiet_NaN();
    double c_far = std::numeric_limits<double>::quiet_NaN();
    if (s_near != s0) {
      const std::optional<double> cn =
          confirm_price(S, K, T, near_end, r, q, side, confirm_opts, stats);
      if (!cn) {
        return fail(IvPremiumFallback::PricerFailure, RootAcceptanceKind::None,
                    RootAcceptanceReason::None);
      }
      c_near = *cn;
      stats.last_residual_usd = std::fabs(c_near - price);
      s_near = confirm_side(c_near, price, eps_c);
      if (s_near == -s0) {
        // The root lies between x and the near end: certified, but wider than w.
        return interval(std::fmin(x, near_end), std::fmax(x, near_end));
      }
      if (s_near != s0) {
        return fail(IvPremiumFallback::NoConvergence, RootAcceptanceKind::None,
                    RootAcceptanceReason::BudgetExhausted);
      }
    }
    if (s_far != -s0) {
      const std::optional<double> cf =
          confirm_price(S, K, T, far_end, r, q, side, confirm_opts, stats);
      if (!cf) {
        return fail(IvPremiumFallback::PricerFailure, RootAcceptanceKind::None,
                    RootAcceptanceReason::None);
      }
      c_far = *cf;
      stats.last_residual_usd = std::fabs(c_far - price);
      s_far = confirm_side(c_far, price, eps_c);
      if (s_far != -s0) {
        return fail(IvPremiumFallback::NoConvergence, RootAcceptanceKind::None,
                    RootAcceptanceReason::BudgetExhausted);
      }
    }
    const double lo = std::fmin(near_end, far_end);
    const double hi = std::fmax(near_end, far_end);
    const double gap = c_far - c_near;
    const double root = std::isfinite(gap) && gap != 0.0
                            ? near_end + (price - c_near) / gap * (far_end - near_end)
                            : centre;
    return accept(root, lo, hi,
                  family_end ? RootAcceptanceReason::ConfirmAndFamily
                             : RootAcceptanceReason::ConfirmBothEnds);
  }
  const double far = x + dir * w;
  if (inside(far) && family_side(price, S, K, q, far, f, env, stats) == -s0) {
    return accept(x + delta, std::fmin(x, far), std::fmax(x, far),
                  RootAcceptanceReason::ConfirmAndFamily);
  }
  // The second confirm solve past the chord's Newton point (4 x its step, <= w here).
  const double step = std::fmax(4.0 * std::fabs(delta), 1.0e-3 * w);
  const double e = x + dir * step;
  if (!inside(e)) {
    return fail(IvPremiumFallback::VolBound, RootAcceptanceKind::None,
                RootAcceptanceReason::BudgetExhausted);
  }
  const std::optional<double> ce = confirm_price(S, K, T, e, r, q, side, confirm_opts, stats);
  if (!ce) {
    return fail(IvPremiumFallback::PricerFailure, RootAcceptanceKind::None,
                RootAcceptanceReason::None);
  }
  stats.last_residual_usd = std::fabs(*ce - price);
  const int se = confirm_side(*ce, price, eps_c);
  const double lo = std::fmin(x, e);
  const double hi = std::fmax(x, e);
  if (se == -s0) {
    if (hi - lo > w) {
      return interval(lo, hi);
    }
    const double gap = *ce - *c0;
    const double root = gap != 0.0 ? x + (price - *c0) / gap * (e - x) : 0.5 * (lo + hi);
    return accept(root, lo, hi, RootAcceptanceReason::ConfirmBothEnds);
  }
  if (se == s0) {
    // Still on x's side: the family may certify the other side within w past e.
    const double far_e = e + dir * w;
    if (inside(far_e) && family_side(price, S, K, q, far_e, f, env, stats) == -s0) {
      return accept(e + (price - *ce) / slope, std::fmin(e, far_e), std::fmax(e, far_e),
                    RootAcceptanceReason::ConfirmAndFamily);
    }
  }
  return fail(IvPremiumFallback::NoConvergence, RootAcceptanceKind::None,
              RootAcceptanceReason::BudgetExhausted);
}

// K3 acceptance of a family-native root (IvPremiumGate::qfamily_native): Skipped when the
// envelope encloses it, else confirm_bracket with the enclosure's local chord (or, where a
// price refused, the family's own kAlPackSigmaBump chord at the root).
// K4 (native fit route, `lane_vega`): when the enclosure measured no chord (a family price
// refused), the confirm's placement slope is the lane's fused vega at its last point, already
// in hand, instead of the family's kAlPackSigmaBump chord (two more family prices). The slope
// places only; the bracket rule is unchanged. K4b (review-K4 m3): the hook reaches the
// enclosure step - without a covering envelope the enclosure cannot skip, so its two family
// prices would only measure that placement chord; the fused vega replaces them.
[[nodiscard]] std::optional<double> accept_family_native(const NativeRoot &native, double price,
                                                         double S, double K, double T, double r,
                                                         double q, Side side, double tol,
                                                         const AlSigmaQFamily &f,
                                                         const std::optional<AlOpts> &confirm_opts,
                                                         IvPremiumStats &stats,
                                                         bool lane_vega = false) {
  const double x = native.root;
  stats.last_residual_usd = native.residual_usd;
  // W2c-g: K2's batch-seam lane accepts on the fused vega at its last point - a tangent,
  // no secant partner - so the span is 0 (K3's secant span is gone with the secant).
  stats.native_partner_span = 0.0;
  if (!(x > kSigmaLo && x < kSigmaHi)) {
    stats.fallback = IvPremiumFallback::VolBound;
    return std::nullopt;
  }
  const ValidatedErrorEnvelope *env = current_family_envelope(&f);
  const double w = acceptance_width(tol);
  if (lane_vega && (env == nullptr || !env->covered()) && std::isfinite(native.slope) &&
      native.slope > 0.0) {
    stats.lane_vega_used = true;
    return confirm_bracket(price, S, K, T, r, q, side, x, native.slope, w, f, env, confirm_opts,
                           stats);
  }
  const FamilyEnclosure enc = [&] {
    // K4b: the batched route's enclosure for exactly these inputs, with what it spent.
    if (t_prepared != nullptr && t_prepared->enclosure.valid) {
      const IvPreparedEnclosure &pe = t_prepared->enclosure;
      if (pe.family == &f && pe.env == env && same_bits(pe.price, price) &&
          same_bits(pe.S, S) && same_bits(pe.K, K) && same_bits(pe.q, q) &&
          same_bits(pe.x, x) && same_bits(pe.w, w)) {
        stats.family_prices += pe.family_prices;
        return pe.enclosure;
      }
    }
    const SubTimer family_clock(stats, IvPremiumSubClock::FamilyAccept);
    return family_enclosure(price, S, K, q, x, w, f, env, stats);
  }();
  if (enc.enclosed) {
    stats.acceptance = RootAcceptance{RootAcceptanceKind::Skipped,
                                      RootAcceptanceReason::FamilyEnclosure,
                                      FamilyErrorClass::RefinementValidated, enc.hi - enc.lo,
                                      enc.lo, enc.hi};
    stats.confirm_skipped = true;
    stats.gate_bound_vol = enc.hi - enc.lo;
    stats.fallback = IvPremiumFallback::None;
    return enc.root;
  }
  double slope = enc.slope;
  if (!(std::isfinite(slope) && slope > 0.0) && lane_vega) {
    slope = native.slope; // > the lane's vega floor: family_native_root accepted on it
    stats.lane_vega_used = true;
  }
  if (!(std::isfinite(slope) && slope > 0.0)) {
    const SubTimer family_clock(stats, IvPremiumSubClock::FamilyAccept);
    const AlSigmaQFamilyChord fc = f.price_and_chord(S, K, x, q);
    stats.family_prices += 2u;
    if (fc.status == AlSigmaQFamilyStatus::Ok) {
      slope = (fc.price_sigma_bumped - fc.price) / kAlPackSigmaBump;
    }
  }
  return confirm_bracket(price, S, K, T, r, q, side, x, slope, w, f, env, confirm_opts, stats);
}

// ── The bounded reference route of the native fit route (lane K4, r6) ──
//
// THE MAP. C_I(sigma) = andersen_lake_classified(..., confirm kernel, Intrinsic): the confirm
// kernel C of the K3 acceptance (eps_c = its tested bound against the target map A,
// al_profile_price_bound) under C1's exact plateau rule. Outside the exercise plateau C_I is
// C bit for bit; on the plateau (the spot on the exercise side of B(T)) it is the intrinsic,
// flat in sigma, so the in-region roots of the kernel floor's dip (L1c) cannot be returned.
// Every evaluation is classified (american_region.hpp), so the plateau is decided by the
// classifier, never by a family's own exercise flag.
//
// THE RULE (K3's bracket rule on C_I): a side of the root at sigma is certified when
// C_I -/+ eps_c lies beyond the price; a root is served only from a bracket [lo, hi] whose
// ends carry opposite certified sides, hi - lo <= W = acceptance_width(tol), the served point
// the falsi of the end prices clamped into it. Typed outcomes otherwise (IvRouteOutcome):
// an evaluation on the plateau whose intrinsic is not below the price (the price sits on the
// plateau): PriceOnlyPlateau; C_I(kIvMin) not below the price outside the plateau:
// PriceOnlyFloor; C_I(kSigmaHiCap) - Search's own expansion ceiling - not above it:
// AboveRange; a point C_I cannot place whose two straddling points 0.45 W away it cannot
// place either (the slope cannot clear 2 eps_c across W): PriceOnlyLowVega; the per-request
// cap with a certified bracket: Interval, without one (or a kernel refusal): Unresolved; the
// chain's budget spent: BudgetExhausted. Low vega is decided only there, at the root: a chord
// across a flat tail far from it (a deep out-of-the-money price near zero) says nothing.
//
// THE PATH. From the hint (the native attempt's retained bracket, best iterate and fused
// vega, or the Black-76 IV): safeguarded Newton / secant steps kept inside the certified
// bracket (bisection, geometric on a wide one, otherwise; also from a point on the exercise
// plateau, flat in sigma, and after two Newton steps that did not halve); once a step is
// within W / 4 the next point straddles the root estimate by 0.45 W so one evaluation closes
// the bracket.
struct ReferenceHint {
  double seed = std::numeric_limits<double>::quiet_NaN();
  double slope = std::numeric_limits<double>::quiet_NaN();
  bool floor_first = false; // evaluate kIvMin first (an edge-band price)
};

struct ReferencePoint {
  double sigma = std::numeric_limits<double>::quiet_NaN();
  double price = std::numeric_limits<double>::quiet_NaN();
  int side = 0;
  AlExerciseClass cls = AlExerciseClass::Continuation;
};

// The geometric (or arithmetic, on a narrow bracket) midpoint of (lo, hi).
[[nodiscard]] double reference_bisect(double lo, double hi) noexcept {
  return hi > 4.0 * lo && lo > 0.0 ? std::sqrt(lo * hi) : 0.5 * (lo + hi);
}

[[nodiscard]] std::optional<double> reference_root(double price, double S, double K, double T,
                                                   double r, double q, Side side, double tol,
                                                   const std::optional<AlOpts> &kernel,
                                                   const ReferenceHint &hint,
                                                   IvReferenceBudget *budget,
                                                   IvPremiumStats &stats) {
  const SubTimer reference_clock(stats, IvPremiumSubClock::Reference);
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  const auto typed = [&stats](IvRouteOutcome outcome, double lo,
                              double hi) -> std::optional<double> {
    stats.route = outcome;
    stats.route_lo = lo;
    stats.route_hi = hi;
    stats.fallback = IvPremiumFallback::None;
    return std::nullopt;
  };
  // K4b: the chain's reference WALL budget (IvReferenceBudget::max_wall_ns; 0: none), on top
  // of the count budget: this request's wall is charged to it on every exit, and the budget is
  // checked at entry and before every evaluation.
  using Steady = std::chrono::steady_clock;
  const bool wall_budget = budget != nullptr && budget->max_wall_ns > 0u;
  const Steady::time_point wall_t0 = wall_budget ? Steady::now() : Steady::time_point{};
  const auto wall_now = [&]() noexcept -> std::uint64_t {
    const auto ns = (Steady::now() - wall_t0) / std::chrono::nanoseconds{1};
    return ns > 0 ? static_cast<std::uint64_t>(ns) : 0u;
  };
  struct WallCharge {
    IvReferenceBudget *budget;
    const decltype(wall_now) &now;
    ~WallCharge() {
      if (budget != nullptr) budget->wall_ns += now();
    }
  };
  const WallCharge wall_charge{wall_budget ? budget : nullptr, wall_now};
  if (budget != nullptr) {
    const bool wall_spent = wall_budget && budget->wall_ns >= budget->max_wall_ns;
    if (budget->calls >= budget->max_calls || budget->evaluations >= budget->max_evaluations ||
        wall_spent) {
      ++budget->refused;
      if (wall_spent) ++budget->refused_wall;
      return typed(IvRouteOutcome::BudgetExhausted, kNaN, kNaN);
    }
    ++budget->calls;
  }
  const AlProfile profile = kernel ? kernel->profile : AlProfile::Reference;
  const double eps = al_profile_price_bound(profile, price, T);
  const double w = acceptance_width(tol);
  bool spent = false; // the chain's budget ran out mid-request
  bool spent_wall = false; // ... its wall budget (K4b)
  bool refused = false;
  const auto eval = [&](double s) -> std::optional<ReferencePoint> {
    if (stats.reference_evaluations >= kIvReferenceMaxEvaluations) return std::nullopt;
    if (budget != nullptr) {
      if (budget->evaluations >= budget->max_evaluations) {
        spent = true;
        return std::nullopt;
      }
      if (wall_budget && budget->wall_ns + wall_now() >= budget->max_wall_ns) {
        spent = true;
        spent_wall = true;
        return std::nullopt;
      }
      ++budget->evaluations;
    }
    ++stats.reference_evaluations;
    ++stats.premium_solves;
    counters::ledger::bump(counters::ledger::Solve::IvNewtonIters);
    const Result<AlRegionPrice> rp =
        andersen_lake_classified(S, K, T, s, r, q, side, kernel, AlExerciseRegion::Intrinsic);
    if (!rp || !rp->priced() || !std::isfinite(rp->price)) {
      refused = true;
      return std::nullopt;
    }
    stats.reference_class = static_cast<std::uint8_t>(rp->cls);
    return ReferencePoint{s, rp->price, confirm_side(rp->price, price, eps), rp->cls};
  };
  // The certified bracket (NaN price: that end is not certified yet).
  double lo = kSigmaLo;
  double hi = kSigmaHiCap;
  double p_lo = kNaN;
  double p_hi = kNaN;
  // review-K4 m2: a certified end that would cross the other certified end (possible only
  // if the confirm kernel's tested distance eps_c were violated) makes the bracket
  // inconsistent: no root is served from it (Unresolved).
  bool crossed = false;
  // K4b (PM item A17; K5's dev200b audit): whether the certified low end is an
  // ExercisePlateau point. The classified map is the intrinsic exactly up to the plateau's
  // edge sigma* and the kernel's euro + premium past it, so it JUMPS at sigma* by the
  // kernel-floor map's own premium there (2.4e-6 on the BABO 3 call, 42 % carry); a price
  // inside that jump is bracketed by a plateau end and a continuation end with no root
  // between them. A served root's plateau end is therefore re-decided on the kernel-floor
  // map (andersen_lake, the accurate map K3's audit re-bisects); a disagreement types the
  // price price-only (plateau): it lies within the plateau edge's jump.
  bool lo_plateau = false;
  const auto stop = [&]() -> std::optional<double> {
    if (spent) {
      ++budget->refused;
      if (spent_wall) ++budget->refused_wall;
      return typed(IvRouteOutcome::BudgetExhausted, std::isfinite(p_lo) ? lo : kNaN,
                   std::isfinite(p_hi) ? hi : kNaN);
    }
    if (crossed) {
      return typed(IvRouteOutcome::Unresolved, kNaN, kNaN);
    }
    if (!refused && std::isfinite(p_lo) && std::isfinite(p_hi)) {
      return typed(IvRouteOutcome::Interval, lo, hi);
    }
    return typed(IvRouteOutcome::Unresolved, std::isfinite(p_lo) ? lo : kNaN,
                 std::isfinite(p_hi) ? hi : kNaN);
  };
  // Records a point; a typed outcome when the point itself decides one.
  const auto take = [&](const ReferencePoint &e) -> std::optional<IvRouteOutcome> {
    if (e.cls == AlExerciseClass::ExercisePlateau && e.side >= 0) {
      return IvRouteOutcome::PriceOnlyPlateau; // the price sits on the plateau's intrinsic
    }
    // review-K4 m2: an end is accepted only strictly inside the other CERTIFIED end.
    const IvReferenceTake t = iv_reference_take(lo, hi, p_lo, p_hi, e.sigma, e.price, e.side);
    if (t == IvReferenceTake::Crossed) {
      crossed = true;
      return IvRouteOutcome::Unresolved;
    }
    if (t == IvReferenceTake::Lower) lo_plateau = e.cls == AlExerciseClass::ExercisePlateau;
    if (e.sigma <= kSigmaLo && e.side >= 0) return IvRouteOutcome::PriceOnlyFloor;
    if (e.sigma >= kSigmaHiCap && e.side <= 0) return IvRouteOutcome::AboveRange;
    return std::nullopt;
  };
  const auto decided = [&](IvRouteOutcome o) -> std::optional<double> {
    if (o == IvRouteOutcome::Unresolved) return stop(); // a crossing (m2)
    if (o == IvRouteOutcome::PriceOnlyPlateau) return typed(o, 0.0, kNaN);
    if (o == IvRouteOutcome::PriceOnlyFloor) return typed(o, 0.0, kSigmaLo);
    return typed(o, kSigmaHiCap, kNaN); // AboveRange
  };
  std::optional<ReferencePoint> prev;
  double last_step = std::numeric_limits<double>::infinity(); // the last Newton step
  int slow = 0;                                                 // steps not halving
  double intrinsic = kNaN; // a plateau point's price (the intrinsic), once one is evaluated
  double slope = std::isfinite(hint.slope) && hint.slope > 0.0 ? hint.slope : kNaN;
  if (hint.floor_first) {
    const std::optional<ReferencePoint> e = eval(kSigmaLo);
    if (!e) return stop();
    if (const auto o = take(*e)) return decided(*o);
    if (e->cls == AlExerciseClass::ExercisePlateau) {
      intrinsic = e->price;
    } else {
      prev = e;
    }
  }
  double x = std::isfinite(hint.seed) && hint.seed > lo && hint.seed < hi
                 ? hint.seed
                 : reference_bisect(lo, hi);
  for (;;) {
    std::optional<ReferencePoint> e = eval(x);
    if (!e) return stop();
    if (const auto o = take(*e)) return decided(*o);
    if (e->side == 0) {
      // C_I cannot place x: the root lies within eps_c / slope of it. Straddle x by 0.45 W
      // (a bracket of 0.9 W: no rounding of the two ends can widen it past W).
      for (const double end : {x - 0.45 * w, x + 0.45 * w}) {
        const std::optional<ReferencePoint> s = eval(end);
        if (!s) return stop();
        if (const auto o = take(*s)) return decided(*o);
        // review-K4 m1: only a CERTIFIED end bounds the price-only outcome (an uncertified
        // range end - kIvMin / kSigmaHiCap - carries no information).
        if (s->side == 0) {
          return typed(IvRouteOutcome::PriceOnlyLowVega, std::isfinite(p_lo) ? lo : kNaN,
                       std::isfinite(p_hi) ? hi : kNaN);
        }
      }
    }
    if (std::isfinite(p_lo) && std::isfinite(p_hi) && lo <= hi && hi - lo <= w) {
      if (lo_plateau) {
        // K4b (A17): the plateau end's side on the kernel-floor map (one evaluation, charged
        // to the budget like every other; past the per-request cap by at most this one).
        if (budget != nullptr) {
          if (budget->evaluations >= budget->max_evaluations ||
              (wall_budget && budget->wall_ns + wall_now() >= budget->max_wall_ns)) {
            spent = true;
            spent_wall = budget->evaluations < budget->max_evaluations;
            return stop();
          }
          ++budget->evaluations;
        }
        ++stats.reference_evaluations;
        ++stats.premium_solves;
        counters::ledger::bump(counters::ledger::Solve::IvNewtonIters);
        const Result<double> a = andersen_lake(S, K, T, lo, r, q, side, kernel);
        if (!a || !std::isfinite(*a)) {
          refused = true;
          return stop();
        }
        if (confirm_side(*a, price, eps) >= 0) {
          return typed(IvRouteOutcome::PriceOnlyPlateau, 0.0, kNaN);
        }
      }
      const double gap = p_hi - p_lo;
      const double root = gap > 0.0 ? lo + (price - p_lo) / gap * (hi - lo) : 0.5 * (lo + hi);
      stats.acceptance = RootAcceptance{RootAcceptanceKind::Confirmed,
                                        RootAcceptanceReason::ConfirmBothEnds,
                                        FamilyErrorClass::Uncovered, hi - lo, lo, hi};
      stats.last_residual_usd = std::fmin(std::fabs(p_lo - price), std::fabs(p_hi - price));
      stats.route = IvRouteOutcome::ReferenceRoot;
      stats.route_lo = lo;
      stats.route_hi = hi;
      stats.fallback = IvPremiumFallback::None;
      return std::clamp(root, lo, hi);
    }
    // A plateau point is flat in sigma (its chord is no local slope): the root lies past the
    // plateau's edge, so the bracket is bisected and the point never seeds a chord.
    const bool plateau = e->cls == AlExerciseClass::ExercisePlateau;
    if (!plateau && prev && prev->sigma != e->sigma) {
      const double chord = (e->price - prev->price) / (e->sigma - prev->sigma);
      if (std::isfinite(chord) && chord > 0.0) slope = chord;
    }
    double next = kNaN;
    if (plateau) {
      intrinsic = e->price;
      next = reference_bisect(lo, hi);
    } else if (std::isfinite(slope)) {
      double newton = e->sigma - (e->price - price) / slope;
      // Past the plateau's edge sigma* the premium P - I grows like (sigma - sigma*)^2 (smooth
      // pasting), where Newton on P only halves its distance per step: with the intrinsic I
      // known (a plateau point), the step is taken on g = sqrt(P - I), linear there.
      if (std::isfinite(intrinsic) && e->price > intrinsic && price > intrinsic) {
        const double target = std::sqrt(price - intrinsic);
        const double ge = std::sqrt(e->price - intrinsic) - target;
        double gslope = slope / (2.0 * std::sqrt(e->price - intrinsic));
        if (prev && prev->price > intrinsic && prev->sigma != e->sigma) {
          const double gp = std::sqrt(prev->price - intrinsic) - target;
          const double chord = (ge - gp) / (e->sigma - prev->sigma);
          if (std::isfinite(chord) && chord > 0.0) gslope = chord;
        }
        if (std::isfinite(gslope) && gslope > 0.0) newton = e->sigma - ge / gslope;
      }
      // A step that has not halved twice running is not converging: bisect instead.
      const double step = std::fabs(newton - e->sigma);
      slow = step > 0.5 * last_step ? slow + 1 : 0;
      last_step = step;
      // Close to the root: straddle the estimate so the next evaluation closes the bracket.
      const double dir = e->side < 0 ? 1.0 : -1.0;
      next = step <= 0.25 * w ? newton + dir * 0.45 * w : newton;
      if (slow >= 2) {
        next = reference_bisect(lo, hi);
        slow = 0;
      }
    } else if (e->side != 0) {
      next = e->sigma + (e->side < 0 ? w : -w); // a second point for the first chord
    }
    if (!(next > lo && next < hi)) {
      // Outside the certified bracket: bisect; an uncertified box end is evaluated at the
      // box end itself once the bracket has narrowed onto it (the floor / range decision).
      next = reference_bisect(lo, hi);
      if (!std::isfinite(p_hi) && next > 0.75 * kSigmaHiCap) next = kSigmaHiCap;
      if (!std::isfinite(p_lo) && next < 2.0 * kSigmaLo) next = kSigmaLo;
    }
    if (!plateau) prev = e;
    x = next;
  }
}

// The Black-76 IV of `price` at the contract's forward (an upper bound of the American IV), the
// reference route's seed where no family point is known; NaN when there is none. Not
// noexcept: the Black-76 inversion's Error path builds a message string.
[[nodiscard]] double black76_seed(double price, double S, double K, double T, double r, double q,
                                  Side side) {
  const double F = S * std::exp((r - q) * T);
  const Result<double> eu = implied_vol(price, F, K, T, std::exp(-r * T), side);
  return eu && std::isfinite(*eu) ? *eu : std::numeric_limits<double>::quiet_NaN();
}

// The family's Newton estimate from the lane's last evaluated point (NaN without one).
[[nodiscard]] double lane_newton(const NativeRoot &native, double price) noexcept {
  if (!std::isfinite(native.last_price) || !(native.vega > 0.0) || !std::isfinite(native.vega)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return native.last_sigma + (price - native.last_price) / native.vega;
}

// K4 native fit route: a family-native root the K3 acceptance did not close. PriceOnly (the
// confirm kernel cannot resolve the width at the root's slope) and Interval (a certified
// bracket wider than the requirement) are typed outcomes - C0's low_vega / iv_interval - not
// re-solved; a bracket that did not close (kind None) goes to the reference route from the
// root with the lane's fused vega.
[[nodiscard]] std::optional<double>
native_unaccepted(const NativeRoot &native, double price, double S, double K, double T, double r,
                  double q, Side side, double tol, const std::optional<AlOpts> &kernel,
                  IvReferenceBudget *budget, IvPremiumStats &stats) {
  const RootAcceptance &acc = stats.acceptance;
  stats.fallback = IvPremiumFallback::None;
  const IvRouteOutcome typed = native_unaccepted_outcome(acc.kind);
  if (typed != IvRouteOutcome::None) {
    stats.route = typed;
    if (typed == IvRouteOutcome::Interval) {
      stats.route_lo = acc.lo; // a certified bracket of the target map's root
      stats.route_hi = acc.hi;
    }
    return std::nullopt;
  }
  ReferenceHint hint;
  hint.seed = native.root;
  hint.slope = native.slope;
  return reference_root(price, S, K, T, r, q, side, tol, kernel, hint, budget, stats);
}

// K4 native fit route: a refused family-native attempt, typed by its InverseStatus and never
// handed to the seed leg. LowVega (a slope at or below the lane's floor off the plateau) is
// price-only; every other status goes to the reference route, started from what the lane
// learned: the family's Newton estimate from its last point (a box miss: the box edge it
// evaluated), its best iterate (nonconverged, converged without a slope, exercise-flat -
// whose plateau the reference route's classifier decides, not the family's own flag), else
// the Black-76 IV (no evaluation: q outside the box, invalid input, unsupported ISA).
[[nodiscard]] std::optional<double>
native_refused(const NativeRoot &native, double price, double S, double K, double T, double r,
               double q, Side side, double tol, const std::optional<AlOpts> &kernel,
               IvReferenceBudget *budget, IvPremiumStats &stats) {
  namespace ib = iv_batch;
  ReferenceHint hint;
  hint.slope = native.vega;
  switch (stats.native_status) {
  case ib::InverseStatus::LowVega:
    // The lane's bracket is the family map's, not certified on the target map: no interval.
    stats.fallback = IvPremiumFallback::None;
    stats.route = IvRouteOutcome::PriceOnlyLowVega;
    return std::nullopt;
  case ib::InverseStatus::SigmaBelowBox:
  case ib::InverseStatus::SigmaAboveBox:
    hint.seed = lane_newton(native, price);
    break;
  case ib::InverseStatus::NonConverged:
  case ib::InverseStatus::ConvergedNoSlope:
  case ib::InverseStatus::ExerciseFlat:
  case ib::InverseStatus::Converged: // converged without a usable vega (accepted otherwise)
    hint.seed = native.sigma;
    break;
  case ib::InverseStatus::Inactive:
  case ib::InverseStatus::QOutsideBox:
  case ib::InverseStatus::InvalidInput:
  case ib::InverseStatus::UnsupportedIsa:
    break;
  }
  if (!std::isfinite(hint.seed)) hint.seed = black76_seed(price, S, K, T, r, q, side);
  return reference_root(price, S, K, T, r, q, side, tol, kernel, hint, budget, stats);
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
  // K4b: the batched route's values prepared for this request, for its whole duration.
  const PreparedScope prepared_scope(gate != nullptr ? gate->prepared : nullptr);
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
  // Both sides are in vol units (the brief's statement read on the vol scale). Lane K3: a
  // request that carries a (sigma, q) family never reaches it (accept below), so eps_A is
  // always the seed tier's TESTED bound, never a family's sampled estimate.
  const auto band_gate_holds = [&](double slope, const ColdPoint &last,
                                   const ColdPoint *partner) -> bool {
    if (gate == nullptr || !(gate->half_spread_usd > 0.0) || !(gate->k > 0.0)) {
      return false;
    }
    const AlProfile seed_profile = seed_opts ? seed_opts->profile : AlProfile::Reference;
    const double eps_a = al_profile_price_bound(seed_profile, price, T);
    const double eps_r = std::fabs(last.price - price);
    const double h =
        last.sigma + kAlPackSigmaBump < kSigmaHi ? kAlPackSigmaBump : -kAlPackSigmaBump;
    double bumped = std::numeric_limits<double>::quiet_NaN();
    const Result<double> pb = andersen_lake(S, K, T, last.sigma + h, r, q, side, seed_opts);
    ++stats.premium_solves;
    ++stats.gate_solves;
    if (pb) bumped = *pb;
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
  // unless the gate's policy skips the confirm: Never; Band on the (sigma, q) family route
  // only when the family's validated envelope encloses the root (lane K3, the rule above
  // accept_family_native); Band on a request that carried no family when the band gate's
  // statement holds (the legacy Screen arm). A request that carried a (sigma, q) family
  // never reaches the band gate: a root after a family refusal is confirmed.
  const auto accept = [&](double root, double slope, const ColdPoint &last,
                          const ColdPoint *partner) -> std::optional<double> {
    if (!(root > kSigmaLo && root < kSigmaHi)) {
      return fail(IvPremiumFallback::VolBound);
    }
    if (confirm) {
      const IvConfirmPolicy policy = gate != nullptr ? gate->policy : IvConfirmPolicy::Always;
      if (policy == IvConfirmPolicy::Band && on_qfamily) {
        const FamilyEnclosure enc = [&] {
          const SubTimer family_clock(stats, IvPremiumSubClock::FamilyAccept);
          return family_enclosure(price, S, K, q, root, acceptance_width(tol), *qfamily,
                                  current_family_envelope(qfamily), stats);
        }();
        if (enc.enclosed) {
          stats.acceptance = RootAcceptance{RootAcceptanceKind::Skipped,
                                            RootAcceptanceReason::FamilyEnclosure,
                                            FamilyErrorClass::RefinementValidated,
                                            enc.hi - enc.lo, enc.lo, enc.hi};
          stats.confirm_skipped = true;
          stats.gate_bound_vol = enc.hi - enc.lo;
          stats.fallback = IvPremiumFallback::None;
          return enc.root;
        }
      }
      const bool legacy_band = policy == IvConfirmPolicy::Band && gate->qfamily == nullptr &&
                               band_gate_holds(slope, last, partner);
      if (policy == IvConfirmPolicy::Never || legacy_band) {
        stats.acceptance.kind = RootAcceptanceKind::Skipped;
        stats.acceptance.reason = legacy_band ? RootAcceptanceReason::LegacyBandGate
                                              : RootAcceptanceReason::LegacyNever;
        stats.acceptance.bound_iv = legacy_band ? stats.gate_bound_vol
                                                : std::numeric_limits<double>::quiet_NaN();
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
        const Result<double> pc = [&] {
          const SubTimer confirm_clock(stats, IvPremiumSubClock::Confirm);
          return andersen_lake(S, K, T, root, r, q, side, confirm_opts);
        }();
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
      // The established contract (the keys-OFF confirm): a Newton distance on the confirm
      // kernel, no bracket.
      stats.acceptance.kind = RootAcceptanceKind::Confirmed;
      stats.acceptance.reason = RootAcceptanceReason::NewtonContract;
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
  // K4 native fit route: the typed edges. At or above the range's top there is no IV; a
  // price inside the edge band is classified before any root attempt - the reference route
  // evaluates kIvMin first, classified (a price on the exercise plateau is price-only, at or
  // below the floor price outside it too), then finds the continuation root, if any.
  const bool native_route = gate != nullptr && gate->native_route;
  const std::optional<AlOpts> &reference_kernel =
      gate != nullptr && gate->confirm_opts ? gate->confirm_opts : opts;
  if (native_route && !(price < ceiling - band_tol)) {
    stats.route = IvRouteOutcome::AboveRange;
    stats.fallback = IvPremiumFallback::None;
    return std::nullopt;
  }
  if (native_route && !(price - zero_vol > std::fmax(accurate_gate, band_tol))) {
    ReferenceHint hint;
    hint.floor_first = true;
    hint.seed = black76_seed(price, S, K, T, r, q, side);
    return reference_root(price, S, K, T, r, q, side, tol, reference_kernel, hint,
                          gate->reference_budget, stats);
  }
  if (!(price < ceiling - band_tol) || !(price - zero_vol > std::fmax(accurate_gate, band_tol))) {
    return fail(IvPremiumFallback::RangeEdge);
  }
  // K4: the seed tier's routing gate protects seed-tier iterates; a family request on the
  // native route never iterates on the seed tier (its family is built on the accurate kernel).
  const bool native_family_request = native_route && gate->qfamily != nullptr &&
                                     gate->qfamily->side() == side && gate->qfamily_native;
  if (native_route && gate->no_family_reference && !native_family_request) {
    // K4b (review-K4 m5): no family of this side - the bounded reference route answers, from
    // the Black-76 IV; no seed leg and no Barone-Adesi-Whaley model on the route.
    ReferenceHint hint;
    hint.seed = black76_seed(price, S, K, T, r, q, side);
    return reference_root(price, S, K, T, r, q, side, tol, reference_kernel, hint,
                          gate->reference_budget, stats);
  }
  if (confirm && !native_family_request) {
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
      const std::optional<AlOpts> &confirm_opts = gate->confirm_opts ? gate->confirm_opts : opts;
      const std::optional<double> root = accept_family_native(
          native, price, S, K, T, r, q, side, tol, *qfamily, confirm_opts, stats, native_route);
      if (root || !native_route) {
        if (root && native_route) stats.route = IvRouteOutcome::NativeRoot;
        return root;
      }
      return native_unaccepted(native, price, S, K, T, r, q, side, tol, reference_kernel,
                               gate->reference_budget, stats);
    }
    stats.family_refused = true;
    if (native_route) {
      return native_refused(native, price, S, K, T, r, q, side, tol, reference_kernel,
                            gate->reference_budget, stats);
    }
    on_qfamily = false;
  }

  // K4b: from here on the request is on the seed-leg route (its sub-clock).
  const LegTimer leg_clock(stats);
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

// The IvStatus of a native-route outcome that serves no IV (IvPremiumStats::route carries
// the typed reason; the caller reads it, the status is the coarse public class).
[[nodiscard]] IvStatus route_status(IvRouteOutcome route) noexcept {
  switch (route) {
  case IvRouteOutcome::PriceOnlyPlateau:
    return IvStatus::NonMonotoneRegion; // the exercise plateau: the opt-in refusal's class
  case IvRouteOutcome::PriceOnlyFloor:
  case IvRouteOutcome::AboveRange:
    return IvStatus::OutOfRange;
  case IvRouteOutcome::None:
  case IvRouteOutcome::NativeRoot:
  case IvRouteOutcome::ReferenceRoot:
  case IvRouteOutcome::LegRoot:
  case IvRouteOutcome::PriceOnlyLowVega:
  case IvRouteOutcome::Interval:
  case IvRouteOutcome::BudgetExhausted:
  case IvRouteOutcome::Unresolved:
    break;
  }
  return IvStatus::NoConvergence;
}

// The Premium strategy: its root when accepted (then the same opt-in refusal gate as
// Search), else the Search route unchanged. `gate` governs a seed-tier root's confirm
// (nullptr: Always on al_opts).
[[nodiscard]] IvResult premium_typed(double price, double S, double K, double T, double r,
                                     double q, Side side, const IvOpts &opts,
                                     IvPremiumMemory *memory, IvPremiumStats &stats,
                                     const IvPremiumGate *gate) {
  stats = IvPremiumStats{};
  // K4b: the sub-clocks run only under an armed chain timer (quote-bench --timing-detail).
  stats.timed = counters::fit_timing::current_counts() != nullptr;
  if (premium_route(opts, side)) {
    const std::optional<AlOpts> seed_opts = premium_seed_opts(opts);
    const AlProfile own = opts.al_opts ? opts.al_opts->profile : AlProfile::Reference;
    const bool confirm = opts.seed_profile.has_value() && *opts.seed_profile != own;
    const IvPremiumGate *const active = confirm ? gate : nullptr;
    std::optional<double> root = premium_root(price, S, K, T, r, q, side, opts.tol, opts.al_opts,
                                              seed_opts, confirm, active, memory, stats);
    if (!root && active != nullptr && active->native_route) {
      if (stats.route == IvRouteOutcome::None) {
        // K4: a request with no family of its side left the seed-leg route (stats.fallback):
        // the bounded reference route answers it, never Search.
        stats.fallback = IvPremiumFallback::None;
        ReferenceHint hint;
        hint.seed = black76_seed(price, S, K, T, r, q, side);
        const std::optional<AlOpts> &kernel = active->confirm_opts ? active->confirm_opts
                                                                   : opts.al_opts;
        root = reference_root(price, S, K, T, r, q, side, opts.tol, kernel, hint,
                              active->reference_budget, stats);
      }
      if (!root) {
        return IvResult{std::numeric_limits<double>::quiet_NaN(), route_status(stats.route)};
      }
    }
    if (root) {
      if (active != nullptr && active->native_route && stats.route == IvRouteOutcome::None) {
        stats.route = IvRouteOutcome::LegRoot;
      }
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

IvSearchResult american_implied_vol_search(double price, double S, double K, double T,
                                          double r, double q, Side side,
                                          const IvOpts &opts) noexcept {
  try {
    IvSearchResult result;
    result.answer = search_typed(price, S, K, T, r, q, side, opts, &result.rejected_candidate);
    return result;
  } catch (const std::bad_alloc &) {
    return {{std::numeric_limits<double>::quiet_NaN(), IvStatus::Unpriceable}, std::nullopt};
  }
}

std::optional<double> american_implied_vol_price_candidate(
    double price, double S, double K, double T, double r, double q, Side side,
    const IvOpts &opts, double max_price_error) noexcept {
  if (!(max_price_error > 0.0) || !std::isfinite(max_price_error) ||
      opts.method != AmericanMethod::AndersenLake || opts.correction != nullptr ||
      opts.warm_start != 0.0 || opts.price_tol != 0.0) return std::nullopt;
  try {
    IvOpts seed = opts;
    seed.al_opts = al_fast_profile(opts.al_opts);
    seed.refuse_non_monotone = false;
    const IvResult candidate = search_typed(price, S, K, T, r, q, side, seed);
    if (candidate.status != IvStatus::Ok || !(candidate.iv > kIvMin) ||
        !std::isfinite(candidate.iv)) return std::nullopt;
    // Fast is only a seed. This cold target solve supplies both the price and
    // exercise classification, avoiding a separate boundary solve for the gate.
    const auto cold = andersen_lake_classified(S, K, T, candidate.iv, r, q, side,
                                              opts.al_opts, AlExerciseRegion::KernelFloor);
    if (!cold || !cold->priced() || !std::isfinite(cold->price) ||
        cold->cls == AlExerciseClass::ExercisePlateau ||
        std::abs(cold->price - price) > max_price_error) return std::nullopt;
    if (in_band(price, S, K, T, r, q, side, opts.al_opts)) {
      // A monetary budget does not make a flat IV identifiable. Retain the
      // existing positive cold-chord floor; only the residual budget changes.
      const double below = candidate.iv * (1.0 - kCertProbeRel);
      const auto p_below = andersen_lake(S, K, T, below, r, q, side, opts.al_opts);
      if (!p_below || !std::isfinite(*p_below) ||
          !((cold->price - *p_below) / (candidate.iv - below) > kCertSlopeFloorRel * K)) {
        return std::nullopt;
      }
    }
    return candidate.iv;
  } catch (const std::bad_alloc &) {
    return std::nullopt;
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

// ── K4b (r6): the batched native route (american_iv_premium.hpp) ───────────────────────

IvReferenceTake iv_reference_take(double &lo, double &hi, double &p_lo, double &p_hi,
                                  double sigma, double price, int side) noexcept {
  if (side < 0 && sigma >= lo) {
    if (std::isfinite(p_hi) && !(sigma < hi)) return IvReferenceTake::Crossed;
    lo = sigma;
    p_lo = price;
    return IvReferenceTake::Lower;
  }
  if (side > 0 && sigma <= hi) {
    if (std::isfinite(p_lo) && !(sigma > lo)) return IvReferenceTake::Crossed;
    hi = sigma;
    p_hi = price;
    return IvReferenceTake::Upper;
  }
  return IvReferenceTake::None;
}

namespace {

// The batch's structure-of-arrays columns, reused per thread (grown, never shrunk: a warm
// thread prepares a work group without allocating).
struct BatchColumns {
  std::vector<std::size_t> index;
  std::vector<double> K, target, q, seed, blo, bhi, plo, phi;
  std::vector<std::uint8_t> is_call;
  std::vector<double> sigma, lo, hi, p_lo, p_hi, res, vega, last;
  std::vector<std::uint16_t> evals;
  std::vector<iv_batch::InverseStatus> status;
  std::vector<std::uint8_t> flags;
  std::vector<iv_batch::LaneAcceptance> acc;
  // The confirm batch (internal-put coordinates, McDonald-Schroder for a call).
  std::vector<double> cS, cK, cT, csigma, cr, cq, cprice, cslope, ceps, cw;
  std::vector<std::size_t> cindex;
  std::vector<FamilyBoundedRequest> erequest;
  std::vector<FamilyBoundedPrice> eprice;
  std::vector<std::size_t> eindex;

  void resize(std::size_t n) {
    for (auto *v : {&K, &target, &q, &seed, &blo, &bhi, &plo, &phi, &sigma, &lo, &hi, &p_lo, &p_hi,
                    &res, &vega, &last}) {
      if (v->size() < n) v->resize(n);
    }
    if (index.size() < n) index.resize(n);
    if (is_call.size() < n) is_call.resize(n);
    if (evals.size() < n) evals.resize(n);
    if (status.size() < n) status.resize(n);
    if (flags.size() < n) flags.resize(n);
    if (acc.size() < n) acc.resize(n);
  }
  void resize_confirms(std::size_t n) {
    for (auto *v : {&cS, &cK, &cT, &csigma, &cr, &cq, &cprice, &cslope, &ceps, &cw}) {
      if (v->size() < n) v->resize(n);
    }
    if (cindex.size() < n) cindex.resize(n);
  }
  void resize_enclosures(std::size_t n) {
    if (erequest.size() < 2u * n) erequest.resize(2u * n);
    if (eprice.size() < 2u * n) eprice.resize(2u * n);
    if (eindex.size() < n) eindex.resize(n);
  }
};

thread_local BatchColumns t_columns;
thread_local iv_batch::InverseScratch t_batch_scratch;

[[nodiscard]] std::uint64_t ns_since(std::chrono::steady_clock::time_point t0) noexcept {
  const auto ns = (std::chrono::steady_clock::now() - t0) / std::chrono::nanoseconds{1};
  return ns > 0 ? static_cast<std::uint64_t>(ns) : 0u;
}

} // namespace

void iv_native_prepare_lanes(const AlSigmaQFamily &f,
                             const sigma_q_family_detail::PremiumTable *table,
                             std::span<const IvNativeBatchRequest> requests,
                             std::span<IvPremiumPrepared> out, bool timed,
                             IvNativeBatchWork &work) {
  namespace ib = iv_batch;
  const std::size_t n = requests.size();
  if (n == 0 || out.size() != n) return;
  // One spot and one tolerance per call (a chain's); a request that differs is not prepared.
  const double S = requests[0].S;
  const double tol = requests[0].tol;
  BatchColumns &c = t_columns;
  c.resize(n);
  std::size_t m = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const IvNativeBatchRequest &rq = requests[i];
    out[i].lane = IvPreparedLane{};
    if (!same_bits(rq.S, S) || !same_bits(rq.tol, tol) || rq.side != f.side()) continue;
    const LaneStart st = lane_start(rq.price, rq.S, rq.K, rq.T, rq.r, rq.q, rq.side, rq.carry);
    c.index[m] = i;
    c.K[m] = rq.K;
    c.target[m] = rq.price;
    c.q[m] = rq.q;
    c.is_call[m] = rq.side == Side::Call ? 1u : 0u;
    c.seed[m] = st.seed;
    c.blo[m] = st.blo;
    c.bhi[m] = st.bhi;
    c.plo[m] = st.plo;
    c.phi[m] = st.phi;
    ++m;
  }
  if (m == 0) return;
  // The AVX2 kernel on K1's snapshot when the host runs it; otherwise the family's own scalar
  // map - the one-lane route's source, so every lane is that route's bit for bit.
  const bool avx2 = table != nullptr && table->ok && !table->fast && simd::use_avx2();
  ib::InverseBatchIn in;
  if (avx2) {
    in.table = table;
    in.isa = ib::InverseIsa::Avx2;
  } else {
    in.family = &f;
    in.isa = ib::InverseIsa::Scalar;
  }
  in.S = S;
  in.tol = tol;
  in.max_evaluations = static_cast<std::uint16_t>(kIvPremiumNativeEvaluations);
  in.K = std::span<const double>(c.K.data(), m);
  in.target = std::span<const double>(c.target.data(), m);
  in.q = std::span<const double>(c.q.data(), m);
  in.is_call = std::span<const std::uint8_t>(c.is_call.data(), m);
  in.seed_sigma = std::span<const double>(c.seed.data(), m);
  in.bracket_lo = std::span<const double>(c.blo.data(), m);
  in.bracket_hi = std::span<const double>(c.bhi.data(), m);
  in.price_lo = std::span<const double>(c.plo.data(), m);
  in.price_hi = std::span<const double>(c.phi.data(), m);
  const ib::InverseBatchOut o{{c.sigma.data(), m}, {c.lo.data(), m},     {c.hi.data(), m},
                              {c.p_lo.data(), m},  {c.p_hi.data(), m},   {c.res.data(), m},
                              {c.vega.data(), m},  {c.last.data(), m},   {c.evals.data(), m},
                              {c.status.data(), m}, {c.flags.data(), m}, {c.acc.data(), m}};
  ib::InverseBatchWork bw;
  const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const ib::InverseBatchError error = ib::invert_many(in, o, t_batch_scratch, &bw);
  if (timed) work.lane_ns += ns_since(t0);
  ++work.calls;
  work.lanes += m;
  if (bw.avx2) ++work.avx2_calls;
  if (error != ib::InverseBatchError::None) return;
  for (std::size_t j = 0; j < m; ++j) {
    const IvNativeBatchRequest &rq = requests[c.index[j]];
    IvPreparedLane &lane = out[c.index[j]].lane;
    lane.valid = true;
    lane.family = &f;
    lane.S = S;
    lane.K = rq.K;
    lane.target = rq.price;
    lane.q = rq.q;
    lane.tol = tol;
    lane.seed = c.seed[j];
    lane.bracket_lo = c.blo[j];
    lane.bracket_hi = c.bhi[j];
    lane.price_lo = c.plo[j];
    lane.price_hi = c.phi[j];
    lane.sigma = c.sigma[j];
    lane.lo = c.lo[j];
    lane.hi = c.hi[j];
    lane.p_lo = c.p_lo[j];
    lane.p_hi = c.p_hi[j];
    lane.residual = c.res[j];
    lane.vega = c.vega[j];
    lane.last = c.last[j];
    lane.evaluations = c.evals[j];
    lane.status = c.status[j];
    lane.flags = c.flags[j];
  }
}

IvPremiumNativeCarry iv_native_lane_carry(const IvPreparedLane &lane, double price) noexcept {
  if (!lane.valid) return IvPremiumNativeCarry{};
  // family_native_root's own carry write (the last point's price is target + residual).
  const double last_price = std::isfinite(lane.residual) ? price + lane.residual : lane.residual;
  return IvPremiumNativeCarry{true,      lane.lo,         lane.hi,   lane.p_lo,
                              lane.p_hi, lane.last,       last_price, lane.vega,
                              iv_batch::kNoStreamId};
}

void iv_native_prepare_acceptance(const AlSigmaQFamily &f, const ValidatedErrorEnvelope *env,
                                  std::span<const IvNativeBatchRequest> requests,
                                  std::span<IvPremiumPrepared> out,
                                  const std::optional<AlOpts> &confirm_opts, bool avx2,
                                  bool timed, IvNativeBatchWork &work) {
  const std::size_t n = requests.size();
  if (out.size() != n) return;
  const AlProfile profile = confirm_opts ? confirm_opts->profile : AlProfile::Reference;
  BatchColumns &c = t_columns;
  c.resize_confirms(n);
  for (auto &p : out) {
    p.enclosure = IvPreparedEnclosure{};
    p.confirm = IvPreparedConfirm{};
  }
  if (avx2 && env != nullptr && env->covered()) {
    c.resize_enclosures(n);
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const auto &lane = out[i].lane;
      const auto &rq = requests[i];
      if (!lane.valid || lane.status != iv_batch::InverseStatus::Converged ||
          !std::isfinite(lane.vega) || !(lane.vega > iv_batch::kInverseVegaFloorRel * rq.K) ||
          !(lane.sigma > kSigmaLo && lane.sigma < kSigmaHi)) continue;
      const double w = acceptance_width(rq.tol);
      const double a = lane.sigma - 0.5 * w;
      double b = lane.sigma + 0.5 * w;
      for (int step = 0; step < kEnclosureWidthSteps && !(b - a <= w); ++step) {
        b = std::nextafter(b, a);
      }
      c.eindex[count] = i;
      c.erequest[2u * count] = {rq.S, rq.K, a, rq.q};
      c.erequest[2u * count + 1u] = {rq.S, rq.K, b, rq.q};
      ++count;
    }
    const auto t0 =
        timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    static_cast<void>(family_prices_bounded(f, *env,
        std::span<const FamilyBoundedRequest>(c.erequest.data(), 2u * count),
        std::span<FamilyBoundedPrice>(c.eprice.data(), 2u * count)));
    if (timed) work.family_ns += ns_since(t0);
    work.family_prices += static_cast<std::uint32_t>(2u * count);
    work.family_batch_prices += static_cast<std::uint32_t>(2u * count);
    for (std::size_t j = 0; j < count; ++j) {
      const std::size_t i = c.eindex[j];
      const auto &rq = requests[i];
      auto &p = out[i];
      p.enclosure.family_prices = 2u;
      const double w = acceptance_width(rq.tol);
      const double a = c.erequest[2u * j].sigma;
      const double b = c.erequest[2u * j + 1u].sigma;
      const auto &pa = c.eprice[2u * j];
      const auto &pb = c.eprice[2u * j + 1u];
      // The batch bound is no tighter than scalar's, and the price bits match.
      // A closed bracket is therefore scalar's first bracket with the same root.
      // Every open/invalid bracket takes the original scalar path, including its
      // optional recenter, so this optimization cannot change an acceptance.
      if (pa.status != AlSigmaQFamilyStatus::Ok || pb.status != AlSigmaQFamilyStatus::Ok ||
          !(b - a <= w) || !(pa.price + pa.bound_usd <= rq.price &&
                             rq.price <= pb.price - pb.bound_usd)) continue;
      const double gap = pb.price - pa.price;
      const double frac = gap > 0.0 ? (rq.price - pa.price) / gap : 0.5;
      FamilyEnclosure enc;
      enc.enclosed = true;
      enc.slope = gap / (b - a);
      enc.lo = a;
      enc.hi = b;
      enc.root = std::clamp(a + frac * (b - a), a, b);
      p.enclosure = IvPreparedEnclosure{true, &f, env, rq.price, rq.S, rq.K, rq.q,
                                        p.lane.sigma, w, enc, 2u};
    }
  }
  std::size_t m = 0;
  for (std::size_t i = 0; i < n; ++i) {
    IvPremiumPrepared &p = out[i];
    const IvPreparedLane &lane = p.lane;
    const IvNativeBatchRequest &rq = requests[i];
    // family_native_root accepts exactly these lanes (its root = the lane's sigma), and
    // accept_family_native runs the enclosure only inside the seed box.
    if (!lane.valid || lane.status != iv_batch::InverseStatus::Converged ||
        !std::isfinite(lane.vega) || !(lane.vega > iv_batch::kInverseVegaFloorRel * rq.K)) {
      continue;
    }
    const double x = lane.sigma;
    if (!(x > kSigmaLo && x < kSigmaHi)) continue;
    const double w = acceptance_width(rq.tol);
    // accept_family_native on the native route: no covering envelope - no enclosure, the
    // lane's fused vega places the confirm (the lane-vega hook); otherwise the enclosure.
    double slope = lane.vega;
    if (env != nullptr && env->covered()) {
      ++work.enclosures;
      if (p.enclosure.valid) continue;
      IvPremiumStats spent; // untimed: only family_prices is read
      const auto t0 =
          timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      const FamilyEnclosure enc =
          family_enclosure(rq.price, rq.S, rq.K, rq.q, x, w, f, env, spent);
      if (timed) work.family_ns += ns_since(t0);
      work.family_prices += spent.family_prices;
      const std::uint32_t batch_spent = p.enclosure.family_prices;
      if (batch_spent != 0) work.family_retry_prices += spent.family_prices;
      p.enclosure = IvPreparedEnclosure{true, &f,   env, rq.price, rq.S, rq.K, rq.q,
                                        x,    w,    enc, batch_spent + spent.family_prices};
      if (enc.enclosed) continue;
      if (std::isfinite(enc.slope) && enc.slope > 0.0) slope = enc.slope;
    }
    // The AVX2 boundary batch prices a lane in about the scalar FAST solve's time (72-84 us
    // vs 61-79 us per lane on the SPY shape; it is ~2x faster than the scalar ACCURATE scheme
    // only, 77-84 vs 151-178 us: K4b E\diag\probe-boundary-batch-cost-1.txt), so the first
    // confirms are batched on the accurate confirm kernel only; Fast confirms stay scalar.
    if (!avx2 || profile != AlProfile::Reference) continue;
    const double eps_c = al_profile_price_bound(profile, rq.price, rq.T);
    if (!(std::isfinite(slope) && slope > 0.0) || !(slope * w > 2.0 * eps_c)) continue;
    const bool call = rq.side == Side::Call;
    c.cindex[m] = i;
    c.cS[m] = call ? rq.K : rq.S; // McDonald-Schroder: C(S, K, r, q) = P(K, S, q, r)
    c.cK[m] = call ? rq.S : rq.K;
    c.cr[m] = call ? rq.q : rq.r;
    c.cq[m] = call ? rq.r : rq.q;
    c.cT[m] = rq.T;
    c.csigma[m] = x;
    c.cslope[m] = slope;
    c.ceps[m] = eps_c;
    c.cw[m] = w;
    ++m;
  }
  if (m == 0) return;
  const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  static_cast<void>(simd::american_put_boundary_batch(c.cS.data(), c.cK.data(), c.cT.data(),
                                                      c.csigma.data(), c.cr.data(), c.cq.data(),
                                                      c.cprice.data(), m, confirm_opts,
                                                      simd::SimdIsa::ForceAvx2));
  if (timed) work.confirm_ns += ns_since(t0);
  work.confirms += m;
  for (std::size_t j = 0; j < m; ++j) {
    const std::size_t i = c.cindex[j];
    const IvNativeBatchRequest &rq = requests[i];
    const double price = c.cprice[j];
    if (!std::isfinite(price)) continue;
    // Decisive only: the side decision (confirm_side) and the far-root placement test
    // (4 |delta| > w) are the same for every price within the batch kernel's tested class
    // of this one, so they are the scalar confirm kernel's.
    const double margin = kIvBatchConfirmMarginRel * std::fmax(rq.price, 1.0);
    const double d = std::fabs(price - rq.price);
    const bool side_decisive = d > c.ceps[j] + margin || d < c.ceps[j] - margin;
    const double delta = (rq.price - price) / c.cslope[j];
    const bool far_decisive =
        std::fabs(4.0 * std::fabs(delta) - c.cw[j]) > 4.0 * margin / c.cslope[j];
    if (!side_decisive || !far_decisive) continue;
    ++work.decisive;
    out[i].confirm = IvPreparedConfirm{true,  rq.S,    rq.K, rq.T,         c.csigma[j],
                                       rq.r,  rq.q,    rq.side, confirm_opts, price};
  }
}

} // namespace atx::vol
