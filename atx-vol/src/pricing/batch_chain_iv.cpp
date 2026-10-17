// Chain-level batch American implied vol. See api/pricing/batch_chain.hpp for the
// contract; the shared validation and hoisting live in batch_chain.cpp.

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

// The no-arbitrage / well-posedness screen both routes apply before inverting. Ok
// means "invert this row".
[[nodiscard]] ChainRowStatus screen(const IvCall &c, std::size_t i) noexcept {
  if (!bcd::contract_ok(c.contracts, c.w, c.expiries.size(), i)) {
    return ChainRowStatus::InvalidInput;
  }
  const double P = c.target[i];
  if (!std::isfinite(P)) {
    return ChainRowStatus::InvalidInput;
  }
  const double K = c.contracts.strike[i];
  const bool call = c.contracts.side[i] == Side::Call;
  const double intrinsic = call ? std::max(c.spot - K, 0.0) : std::max(K - c.spot, 0.0);
  const double band = c.o.intrinsic_tol * K;
  if (P < intrinsic - band) {
    return ChainRowStatus::OutOfBand;
  }
  // Every sigma up to the exercise edge reproduces intrinsic, so a target there
  // has no identifiable vol (ledgered trap, 2026-10-16).
  if (P <= intrinsic + band) {
    return ChainRowStatus::VegaFloor;
  }
  if (P >= (call ? c.spot : K)) {
    return ChainRowStatus::OutOfBand;
  }
  return ChainRowStatus::Ok;
}

// PerContract route: american_implied_vol warm-started at the row's own sigma.
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
  if (iv) {
    // A root at the kIvMin floor is the scalar's "not identifiable" clamp.
    if (*iv > kIvMin) {
      c.iv[i] = *iv;
      c.status[i] = ChainRowStatus::Ok;
    } else {
      fail(c, i, ChainRowStatus::VegaFloor);
    }
    return;
  }
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

// ── Lockstep route ───────────────────────────────────────────────────────────
//
// Up to kChainBatchLanes contracts iterate together. Per iteration the pack is
// priced in ONE call of the laned AVX2 put kernel (a call C(S, K, r, q) enters as the
// internal put P(K, S, q, r)); a pack is always sent 4 wide (converged lanes keep
// their last sigma, a short pack repeats lane 0) so the kernel never takes its
// scalar tail. Then the lane arithmetic updates each active lane's bracket [lo, hi],
// slope and next iterate: the closed-form Black-76 vega at the forward on the first
// step, the secant slope afterwards, bisection whenever the step leaves the bracket.
// P is increasing in sigma, so f = P - target > 0 moves hi down and f <= 0 moves lo
// up. lo / hi start at [kIvMin, kIvMax] unevaluated; lo_set / hi_set record whether
// an evaluated point sits on that side, which tells a converged root from a target
// outside the image of the vol range.

constexpr std::size_t kL = kChainBatchLanes;

struct Lanes {
  std::size_t m = 0;
  std::array<std::size_t, kL> row{};
  std::array<double, kL> target{};
  // Black-76 inputs (first-step slope) and the original strike (vega floor).
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
  const bool secant = s.have_prev[l] != 0u && sig != s.sig_prev[l];
  const double slope = secant ? (f - s.f_prev[l]) / (sig - s.sig_prev[l])
                              : black76_value_and_vega(s.F[l], s.K[l], s.T[l], sig, s.df[l],
                                                       s.side[l], s.sqrt_t[l])
                                    .vega;
  const double vega_min = c.o.vega_floor * s.K[l];
  if (std::abs(f) <= c.o.price_tol * std::max(1.0, s.target[l])) {
    finish(c, s, l, slope > vega_min ? ChainRowStatus::Ok : ChainRowStatus::VegaFloor);
    return;
  }
  if (f > 0.0) {
    s.hi[l] = sig;
    s.hi_set[l] = 1u;
  } else {
    s.lo[l] = sig;
    s.lo_set[l] = 1u;
  }
  if (s.hi[l] - s.lo[l] <= c.o.sigma_tol) {
    // Bracket collapsed onto sig. Without an evaluated point above the target the
    // target is beyond P(kIvMax); without one below it the root is at the floor.
    ChainRowStatus st = ChainRowStatus::Ok;
    if (s.hi_set[l] == 0u) {
      st = ChainRowStatus::OutOfBand;
    } else if (s.lo_set[l] == 0u || !(slope > vega_min)) {
      st = ChainRowStatus::VegaFloor;
    }
    finish(c, s, l, st);
    return;
  }
  double next = (slope > 0.0 && std::isfinite(slope)) ? sig - f / slope : bcd::kNaN;
  if (!(next > s.lo[l] && next < s.hi[l])) {
    next = 0.5 * (s.lo[l] + s.hi[l]);
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
