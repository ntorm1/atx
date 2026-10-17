// Chain-level batch American implied vol. See api/pricing/batch_chain.hpp for the
// contract; the shared validation and hoisting live in batch_chain.cpp.
//
// Both routes apply ONE refusal rule (fix round 1, 2026-10-17):
//   1. screen(): american_implied_vol's identifiable-range screen, replicated (its
//      helpers are file-local in american_iv.cpp): the price ceiling, the immediate
//      intrinsic and the zero-volatility American value, with its tolerance band.
//   2. accept(): an Ok vol is a point the cold Andersen-Lake map was evaluated at,
//      whose residual meets price_tol, whose slope (a chord of evaluated American
//      points, never the Black-76 seed slope) is above vega_floor * K, and whose
//      Newton correction |f / slope| is at most sigma_tol (Lockstep may instead
//      have both bracket ends evaluated within sigma_tol of each other).
// PerContract runs american_implied_vol and then verifies its answer on the cold map:
// a flat slope there is VegaFloor, and a root that misses the rule gets at most
// kPolishSteps secant steps on the same map before it is NoConvergence.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/batch_chain.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/batch_chain_detail.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

namespace bcd = batch_chain_detail;

// american_implied_vol's band: band_tol = 1e-9 * ceiling + 1e-12 (american_iv.cpp).
constexpr double kScalarBandRel = 1.0e-9;
constexpr double kBandAbs = 1.0e-12;
// Relative sigma step of a slope probe: a chord this wide is "resolved" (far above
// the map's price noise), so a flat one is a flat map, not noise.
constexpr double kSlopeProbeRel = 1.0e-4;
// PerContract: cold secant steps allowed after american_implied_vol's answer.
constexpr unsigned kPolishSteps = 4;

// Replica of american_iv.cpp zero_vol_american_value (file-local there; see its
// comment for the derivation): the sigma -> 0 American value, the sup over exercise
// dates of the deterministic discounted payoff, max over {0, T, t*}. NaN when the
// carry x maturity overflows the payoff.
[[nodiscard]] double zero_vol_value(double S, double K, double T, double r, double q,
                                    Side side) noexcept {
  const bool call = side == Side::Call;
  const double A = call ? S : K;
  const double a = call ? q : r;
  const double B = call ? K : S;
  const double b = call ? r : q;
  const auto payoff = [A, a, B, b](double t) noexcept {
    return A * std::exp(-a * t) - B * std::exp(-b * t);
  };
  const double at_0 = payoff(0.0);
  const double at_T = payoff(T);
  if (!std::isfinite(at_0) || !std::isfinite(at_T)) {
    return bcd::kNaN;
  }
  double best = std::fmax(at_0, at_T);
  const double t_star = std::log((a * A) / (b * B)) / (a - b);
  if (std::isfinite(t_star) && t_star > 0.0 && t_star < T) {
    const double at_star = payoff(t_star);
    if (std::isfinite(at_star)) {
      best = std::fmax(best, at_star);
    }
  }
  return std::fmax(best, 0.0);
}

// Replica of american_iv.cpp american_price_ceiling: S * max(1, e^{-qT}) for a call,
// K * max(1, e^{-rT}) for a put (negative carry lifts the naive S / K bound).
[[nodiscard]] double price_ceiling(double S, double K, double T, double r, double q,
                                   Side side) noexcept {
  return (side == Side::Call) ? S * std::fmax(1.0, std::exp(-q * T))
                              : K * std::fmax(1.0, std::exp(-r * T));
}

struct IvCall {
  double spot;
  const ChainExpiryColumns &expiries;
  const ChainContractColumns &contracts;
  std::span<const double> target;
  std::span<double> iv;
  std::span<ChainRowStatus> status;
  const ChainBatchWorkspace::Impl &w;
  const ChainIvOptions &o;
};

void fail(const IvCall &c, std::size_t i, ChainRowStatus st) noexcept {
  c.iv[i] = bcd::kNaN;
  c.status[i] = st;
}

[[nodiscard]] double price_tol_abs(const IvCall &c, std::size_t i) noexcept {
  return c.o.price_tol * std::max(1.0, c.target[i]);
}

// The identifiable-range screen both routes apply before inverting, in
// american_implied_vol's order. Ok means "invert this row".
[[nodiscard]] ChainRowStatus screen(const IvCall &c, std::size_t i) noexcept {
  if (!bcd::contract_ok(c.contracts, c.w, c.expiries.size(), i)) {
    return ChainRowStatus::InvalidInput;
  }
  const double P = c.target[i];
  if (!std::isfinite(P)) {
    return ChainRowStatus::InvalidInput;
  }
  const std::uint32_t e = c.contracts.expiry[i];
  const double S = c.spot;
  const double K = c.contracts.strike[i];
  const double T = c.expiries.T[e];
  const double r = c.expiries.rate[e];
  const double q = c.w.q_eff[e];
  const Side side = c.contracts.side[i];
  const double ceiling = price_ceiling(S, K, T, r, q, side);
  const double zero_vol = zero_vol_value(S, K, T, r, q, side);
  if (!std::isfinite(ceiling) || !std::isfinite(zero_vol)) {
    return ChainRowStatus::Unpriceable; // the zero-vol bound overflows: no usable map
  }
  // Never narrower than american_implied_vol's own band, so PerContract (which
  // runs that screen again inside the scalar) and Lockstep refuse the same rows; a
  // looser price_tol widens it (a target that close to the floor matches every
  // sigma up to it).
  const double band = std::max(std::max(c.o.intrinsic_tol, kScalarBandRel) * ceiling,
                               price_tol_abs(c, i)) +
                      kBandAbs;
  if (P > ceiling + band) {
    return ChainRowStatus::OutOfBand;
  }
  const double immediate =
      (side == Side::Call) ? std::fmax(S - K, 0.0) : std::fmax(K - S, 0.0);
  if (P < immediate - band) {
    return ChainRowStatus::OutOfBand;
  }
  // At the zero-vol value every sigma up to the exercise edge (or below kIvMin)
  // reproduces the target: no identifiable vol (ledgered trap, 2026-10-16).
  if (std::abs(P - zero_vol) <= band) {
    return ChainRowStatus::VegaFloor;
  }
  // Below the sigma -> 0 value no sigma reproduces it. Andersen-Lake reproduces the
  // analytic limit to 1.3e-13 (american_iv.cpp), so the scalar's map probe at
  // kIvMin refuses the same rows.
  if (P < zero_vol - band) {
    return ChainRowStatus::OutOfBand;
  }
  return ChainRowStatus::Ok;
}

// The shared Ok rule at an evaluated point: residual f, chord slope through it,
// `collapsed` = both bracket ends evaluated within sigma_tol (Lockstep only).
[[nodiscard]] bool accept(const IvCall &c, double f, double slope, double ptol, double vega_min,
                          bool collapsed) noexcept {
  return std::abs(f) <= ptol && slope > vega_min &&
         (std::abs(f) <= c.o.sigma_tol * slope || collapsed);
}

// Cold-map residual P(sigma) - target for row i; false when the kernel refuses.
[[nodiscard]] bool cold_residual(const IvCall &c, std::size_t i, double sigma, double &f) {
  const std::uint32_t e = c.contracts.expiry[i];
  const Result<double> p =
      andersen_lake(c.spot, c.contracts.strike[i], c.expiries.T[e], sigma, c.expiries.rate[e],
                    c.w.q_eff[e], c.contracts.side[i], c.o.al_opts);
  if (!p || !std::isfinite(*p)) {
    return false;
  }
  f = *p - c.target[i];
  return true;
}

// Verifies (and if needed polishes) american_implied_vol's answer x on the cold map.
[[nodiscard]] ChainRowStatus verify_root(const IvCall &c, std::size_t i, double &x) {
  const double ptol = price_tol_abs(c, i);
  const double vega_min = c.o.vega_floor * c.contracts.strike[i];
  double fx = 0.0;
  double fb = 0.0;
  const double xb = x * (1.0 - kSlopeProbeRel);
  if (!cold_residual(c, i, x, fx) || !cold_residual(c, i, xb, fb)) {
    return ChainRowStatus::Unpriceable;
  }
  double slope = (fx - fb) / (x - xb);
  // A resolved flat chord ending at the answer: the price does not move with sigma
  // there (the scalar stopped inside the flat exercise interval, review 2026-10-17
  // M2), so the answer identifies no vol.
  if (!(slope > vega_min)) {
    return ChainRowStatus::VegaFloor;
  }
  for (unsigned k = 0;; ++k) {
    if (accept(c, fx, slope, ptol, vega_min, false)) {
      return ChainRowStatus::Ok;
    }
    if (k == kPolishSteps) {
      return ChainRowStatus::NoConvergence;
    }
    const double xn = x - fx / slope;
    if (!(xn > kIvMin)) {
      return ChainRowStatus::VegaFloor; // the root sits at or below the vol floor
    }
    if (!(xn < kIvMax)) {
      return ChainRowStatus::OutOfBand;
    }
    if (xn == x) {
      return ChainRowStatus::NoConvergence;
    }
    double fn = 0.0;
    if (!cold_residual(c, i, xn, fn)) {
      return ChainRowStatus::Unpriceable;
    }
    const double span = std::abs(xn - x);
    slope = (fn - fx) / (xn - x);
    x = xn;
    fx = fn;
    if (!(slope > vega_min)) {
      return span >= kSlopeProbeRel * x ? ChainRowStatus::VegaFloor
                                        : ChainRowStatus::NoConvergence;
    }
  }
}

// PerContract route: american_implied_vol warm-started at the row's own sigma, then
// verify_root.
void invert_row_per_contract(const IvCall &c, std::size_t i) noexcept {
  const ChainRowStatus scr = screen(c, i);
  if (scr != ChainRowStatus::Ok) {
    fail(c, i, scr);
    return;
  }
  const std::uint32_t e = c.contracts.expiry[i];
  const Result<double> iv = american_implied_vol(
      c.target[i], c.spot, c.contracts.strike[i], c.expiries.T[e], c.expiries.rate[e],
      c.w.q_eff[e], c.contracts.side[i], AmericanMethod::AndersenLake, c.o.sigma_tol,
      c.o.max_iter, c.o.al_opts, /*correction=*/nullptr, /*warm_start=*/c.contracts.sigma[i],
      /*price_tol=*/0.0);
  if (!iv) {
    switch (iv.error().code()) {
    case ErrorCode::OutOfRange:
      fail(c, i, ChainRowStatus::OutOfBand);
      return;
    case ErrorCode::Unavailable:
      fail(c, i, ChainRowStatus::NoConvergence);
      return;
    default: // NotImplemented regimes, Internal: the kernel refused the row
      fail(c, i, ChainRowStatus::Unpriceable);
      return;
    }
  }
  double x = *iv;
  // A root at the kIvMin floor is the scalar's "not identifiable" clamp; one past
  // kIvMax (its bracket reaches 40) is outside the image the contract promises.
  if (!(x > kIvMin)) {
    fail(c, i, ChainRowStatus::VegaFloor);
    return;
  }
  if (!(x < kIvMax)) {
    fail(c, i, ChainRowStatus::OutOfBand);
    return;
  }
  ChainRowStatus st = ChainRowStatus::Unpriceable;
  try {
    st = verify_root(c, i, x);
  } catch (const std::bad_alloc &) {
    st = ChainRowStatus::Unpriceable;
  }
  if (st != ChainRowStatus::Ok) {
    fail(c, i, st);
    return;
  }
  c.iv[i] = x;
  c.status[i] = ChainRowStatus::Ok;
}

// ── Lockstep route ───────────────────────────────────────────────────────────
//
// Up to kChainBatchLanes contracts iterate together. Per iteration the pack is
// priced in ONE call of the laned AVX2 put kernel (a call C(S, K, r, q) enters as the
// internal put P(K, S, q, r)); a pack is always sent 4 wide (converged lanes keep
// their last sigma, a short pack repeats lane 0) so the kernel never takes its
// scalar tail. Then the lane arithmetic updates each active lane's bracket [lo, hi],
// slope and next iterate. P is increasing in sigma, so f = P - target > 0 moves hi
// down and f <= 0 moves lo up. lo / hi start at [kIvMin, kIvMax] unevaluated; lo_set /
// hi_set record whether an evaluated point sits on that side, which tells a converged
// root from a target outside the image of the vol range.
//
// Stepping uses the closed-form Black-76 vega on the first step only (no chord
// exists yet), the secant afterwards, bisection whenever the step leaves the
// bracket. Acceptance never uses the Black-76 slope: accept() needs a chord of two
// evaluated points. A lane whose residual is met before it has a usable chord
// takes one probe step of kSlopeProbeRel * sigma to measure one.

constexpr std::size_t kL = kChainBatchLanes;

struct Lanes {
  std::size_t m = 0;
  std::array<std::size_t, kL> row{};
  std::array<double, kL> target{};
  std::array<double, kL> ptol{};
  std::array<double, kL> vega_min{};
  // Black-76 inputs (first step only).
  std::array<double, kL> K{};
  std::array<double, kL> F{};
  std::array<double, kL> df{};
  std::array<double, kL> sqrt_t{};
  std::array<Side, kL> side{};
  // Internal-put kernel columns.
  std::array<double, kL> pS{};
  std::array<double, kL> pK{};
  std::array<double, kL> T{};
  std::array<double, kL> pr{};
  std::array<double, kL> pq{};
  // Iteration state.
  std::array<double, kL> sig{};
  std::array<double, kL> lo{};
  std::array<double, kL> hi{};
  std::array<double, kL> sig_prev{};
  std::array<double, kL> f_prev{};
  std::array<std::uint8_t, kL> have_prev{};
  std::array<std::uint8_t, kL> lo_set{};
  std::array<std::uint8_t, kL> hi_set{};
  std::array<std::uint8_t, kL> active{};
};

void load_lane(const IvCall &c, Lanes &s, std::size_t i) noexcept {
  const std::size_t l = s.m++;
  const std::uint32_t e = c.contracts.expiry[i];
  const double K = c.contracts.strike[i];
  const double r = c.expiries.rate[e];
  const double q = c.w.q_eff[e];
  const bool call = c.contracts.side[i] == Side::Call;
  s.row[l] = i;
  s.target[l] = c.target[i];
  s.ptol[l] = price_tol_abs(c, i);
  s.vega_min[l] = c.o.vega_floor * K;
  s.K[l] = K;
  s.F[l] = c.spot * c.w.fwd[e];
  s.df[l] = c.w.df[e];
  s.sqrt_t[l] = c.w.sqrt_t[e];
  s.side[l] = c.contracts.side[i];
  s.pS[l] = call ? K : c.spot;
  s.pK[l] = call ? c.spot : K;
  s.T[l] = c.expiries.T[e];
  s.pr[l] = call ? q : r;
  s.pq[l] = call ? r : q;
  s.sig[l] = std::clamp(c.contracts.sigma[i], kIvMin, kIvMax);
  s.lo[l] = kIvMin;
  s.hi[l] = kIvMax;
  s.active[l] = 1u;
}

void finish(const IvCall &c, Lanes &s, std::size_t l, ChainRowStatus st) noexcept {
  s.active[l] = 0u;
  if (st == ChainRowStatus::Ok) {
    c.iv[s.row[l]] = s.sig[l];
    c.status[s.row[l]] = ChainRowStatus::Ok;
  } else {
    fail(c, s.row[l], st);
  }
}

// One lane's update after its price P at s.sig[l].
void step_lane(const IvCall &c, Lanes &s, std::size_t l, double P) noexcept {
  if (!std::isfinite(P)) {
    finish(c, s, l, ChainRowStatus::Unpriceable);
    return;
  }
  const double sig = s.sig[l];
  const double f = P - s.target[l];
  const double vega_min = s.vega_min[l];
  const bool chord = s.have_prev[l] != 0u && sig != s.sig_prev[l];
  const double slope = chord ? (f - s.f_prev[l]) / (sig - s.sig_prev[l]) : bcd::kNaN;
  const bool resolved = chord && std::abs(sig - s.sig_prev[l]) >= kSlopeProbeRel * sig;
  if (f > 0.0) {
    s.hi[l] = sig;
    s.hi_set[l] = 1u;
  } else {
    s.lo[l] = sig;
    s.lo_set[l] = 1u;
  }
  const bool collapsed = s.lo_set[l] != 0u && s.hi_set[l] != 0u && s.hi[l] - s.lo[l] <= c.o.sigma_tol;
  const bool met = std::abs(f) <= s.ptol[l];
  if (chord && accept(c, f, slope, s.ptol[l], vega_min, collapsed)) {
    finish(c, s, l, ChainRowStatus::Ok);
    return;
  }
  // A match on a resolved flat chord: every sigma along it reproduces the target.
  if (met && resolved && !(slope > vega_min)) {
    finish(c, s, l, ChainRowStatus::VegaFloor);
    return;
  }
  if (s.hi[l] - s.lo[l] <= c.o.sigma_tol) {
    // The bracket closed on an unevaluated end: without a point above the target it
    // is beyond P(kIvMax); without one below it the root is at the floor.
    if (s.hi_set[l] == 0u) {
      finish(c, s, l, ChainRowStatus::OutOfBand);
      return;
    }
    if (s.lo_set[l] == 0u) {
      finish(c, s, l, ChainRowStatus::VegaFloor);
      return;
    }
  }
  double next = bcd::kNaN;
  if (met && !(chord && slope > vega_min)) {
    // Matched but no usable chord yet: probe one.
    const double h = kSlopeProbeRel * sig;
    next = (sig + h < s.hi[l]) ? sig + h : sig - h;
  } else {
    const double step_slope =
        chord ? slope
              : black76_value_and_vega(s.F[l], s.K[l], s.T[l], sig, s.df[l], s.side[l],
                                       s.sqrt_t[l])
                    .vega;
    if (step_slope > 0.0 && std::isfinite(step_slope)) {
      next = sig - f / step_slope;
    }
  }
  if (!(next > s.lo[l] && next < s.hi[l])) {
    next = 0.5 * (s.lo[l] + s.hi[l]);
  }
  if (next == sig) {
    finish(c, s, l, ChainRowStatus::NoConvergence); // no representable progress left
    return;
  }
  s.sig_prev[l] = sig;
  s.f_prev[l] = f;
  s.have_prev[l] = 1u;
  s.sig[l] = next;
}

void run_pack(const IvCall &c, Lanes &s) noexcept {
  // A short pack repeats lane 0 in the unused slots (their prices are ignored).
  for (std::size_t l = s.m; l < kL; ++l) {
    s.pS[l] = s.pS[0];
    s.pK[l] = s.pK[0];
    s.T[l] = s.T[0];
    s.pr[l] = s.pr[0];
    s.pq[l] = s.pq[0];
  }
  std::array<double, kL> sig_in{};
  std::array<double, kL> P{};
  bool any = true;
  for (std::uint16_t it = 0; it < c.o.max_iter && any; ++it) {
    for (std::size_t l = 0; l < kL; ++l) {
      sig_in[l] = l < s.m ? s.sig[l] : s.sig[0];
    }
    // The returned route is a dispatch statistic only.
    (void)simd::american_put_boundary_batch(s.pS.data(), s.pK.data(), s.T.data(), sig_in.data(),
                                            s.pr.data(), s.pq.data(), P.data(), kL, c.o.al_opts,
                                            simd::SimdIsa::ForceAvx2);
    any = false;
    for (std::size_t l = 0; l < s.m; ++l) {
      if (s.active[l] != 0u) {
        step_lane(c, s, l, P[l]);
        any = any || s.active[l] != 0u;
      }
    }
  }
  for (std::size_t l = 0; l < s.m; ++l) {
    if (s.active[l] != 0u) {
      finish(c, s, l, ChainRowStatus::NoConvergence);
    }
  }
}

// Packs are the chunk's screened rows in input order, so their composition depends
// on the chunk alone.
void invert_chunk_lockstep(const IvCall &c, std::size_t lo, std::size_t hi) noexcept {
  Lanes s{};
  for (std::size_t i = lo; i < hi; ++i) {
    const ChainRowStatus scr = screen(c, i);
    if (scr != ChainRowStatus::Ok) {
      fail(c, i, scr);
      continue;
    }
    load_lane(c, s, i);
    if (s.m == kL) {
      run_pack(c, s);
      s = Lanes{};
    }
  }
  if (s.m > 0u) {
    run_pack(c, s);
  }
}

[[nodiscard]] bool options_ok(const ChainIvOptions &o) noexcept {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  return (o.route == ChainIvRoute::PerContract || o.route == ChainIvRoute::Lockstep) &&
         o.sigma_tol > 0.0 && o.sigma_tol < kInf && o.max_iter > 0u && o.price_tol >= 0.0 &&
         o.price_tol < kInf && o.intrinsic_tol >= 0.0 && o.intrinsic_tol < kInf &&
         o.vega_floor >= 0.0 && o.vega_floor < kInf;
}

} // namespace

Result<std::size_t> implied_vol_chain_batch(double spot, const ChainExpiryColumns &expiries,
                                            const ChainContractColumns &contracts,
                                            std::span<const double> target_price,
                                            std::span<double> iv_out,
                                            std::span<ChainRowStatus> status_out,
                                            const ChainIvOptions &options,
                                            ChainBatchWorkspace &ws) {
  const std::size_t n = contracts.size();
  if (!(spot > 0.0 && spot < std::numeric_limits<double>::infinity()) ||
      !expiries.consistent() || !contracts.consistent() || target_price.size() != n ||
      iv_out.size() != n || status_out.size() != n || !options_ok(options)) {
    return Err(ErrorCode::InvalidArgument,
               "implied_vol_chain_batch: inconsistent columns, a span length != contract "
               "count, spot not finite > 0, or invalid options");
  }
  if (bcd::outputs_alias({std::as_bytes(iv_out), std::as_bytes(status_out)},
                         {std::as_bytes(expiries.T), std::as_bytes(expiries.rate),
                          std::as_bytes(expiries.carry), std::as_bytes(expiries.div_pv),
                          std::as_bytes(contracts.strike), std::as_bytes(contracts.expiry),
                          std::as_bytes(contracts.side), std::as_bytes(contracts.sigma),
                          std::as_bytes(target_price)})) {
    return Err(ErrorCode::InvalidArgument,
               "implied_vol_chain_batch: an output overlaps an input or another output");
  }
  if (n == 0u) {
    return Ok(n);
  }
  try {
    bcd::prepare_expiries(spot, expiries, ws);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Internal); // no message: building one would allocate again
  }
  const IvCall call{spot, expiries, contracts, target_price, iv_out, status_out, ws.impl(),
                    options};
  const bool lockstep = options.route == ChainIvRoute::Lockstep;
  bcd::for_each_chunk(n, options.executor, options.n_threads,
                      [&call, lockstep](std::size_t lo, std::size_t hi) noexcept {
                        if (lockstep) {
                          invert_chunk_lockstep(call, lo, hi);
                          return;
                        }
                        for (std::size_t i = lo; i < hi; ++i) {
                          invert_row_per_contract(call, i);
                        }
                      });
  return Ok(n);
}

} // namespace atx::vol
