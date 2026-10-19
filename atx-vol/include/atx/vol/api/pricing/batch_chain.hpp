#pragma once

// Chain-level batch pricing, Greeks and implied vol (batch-pricing lane, 2026-10-17).
//
// One call prices a whole listed chain: N contracts that share one spot, and per
// expiry one rate, one carry and (optionally) one cash-dividend PV. Inputs and
// outputs are caller-owned structure-of-arrays spans; outputs are written in input
// order, one entry per contract.
//
// ## What is hoisted
//
// Per expiry, once per call and before any fan-out: expiry validity, the effective
// carry q_eff = chain_effective_carry(spot, T, carry, div_pv), the discount factor
// and the forward factor. Every contract of the expiry reads them. The
// Andersen-Lake time grids and quadrature tables are rebuilt inside every kernel
// call and are not reachable through the kernel's public API, so they are not
// shared here.
//
// ## Routes
//
//   * ChainBatchRoute::Scalar (default) makes, per contract, exactly the call a
//     per-contract loop makes at the hoisted carry: andersen_lake for a
//     price-only request, otherwise american_greeks_al(..., need_vega, need_rho,
//     need_charm = false). Its outputs are bit-identical to that loop.
//   * ChainBatchRoute::LanedAvx2 (opt-in) prices 4 same-side contracts per pack
//     through the public laned AVX2 kernels simd::american_{put,call}_greeks_batch
//     (ForceAvx2; a host without AVX2 takes their scalar route). Those kernels patch
//     ineligible lanes through scalar american_greeks_al and match it within their
//     documented economic gate: NOT bit-identical to Scalar.
//   * ChainIvRoute::PerContract (default) calls american_implied_vol per contract,
//     warm-started at the contract's own sigma, then verifies the answer on the
//     cold andersen_lake map (below).
//   * ChainIvRoute::Lockstep (opt-in) runs a safeguarded Newton-secant on 4
//     contracts at a time in lockstep: each iteration prices the whole pack in one
//     call of the public laned AVX2 put kernel simd::american_put_boundary_batch
//     (calls through the McDonald-Schroder put map, ForceAvx2), and the iterate /
//     bracket / slope updates run over the lane arrays. The map matches
//     andersen_lake within that kernel's gate: not bit-identical to PerContract.
//
// ## One implied-vol refusal rule (both routes)
//
//   1. Screen, american_implied_vol's own (replicated): with band = max(max(
//      intrinsic_tol, 1e-9) * ceiling, price_tol * max(1, target)) + 1e-12 and
//      ceiling = american_price_ceiling (S max(1, e^{-qT}) call, K max(1, e^{-rT})
//      put), a target above ceiling + band or below the immediate intrinsic - band
//      is OutOfBand; one within the band of the zero-volatility American value is
//      VegaFloor; one below that value - band is OutOfBand. At the defaults the
//      band is the scalar's 1e-9 * ceiling + 1e-12 whenever the ceiling is >= 0.1.
//   2. Ok, at a sigma the cold map was evaluated at: |P(sigma) - target| <=
//      price_tol * max(1, target), the slope there (a chord of two evaluated
//      American prices; never the Black-76 vega) is above vega_floor * K, and the
//      Newton correction |P(sigma) - target| / slope is <= sigma_tol (Lockstep: or
//      both bracket ends are evaluated within sigma_tol). A resolved flat chord at
//      a matching sigma is VegaFloor.
//   PerContract applies 2 to american_implied_vol's answer: a flat chord ending
//   there (the scalar stopped inside the flat exercise interval) is VegaFloor, and
//   an answer that misses the rule gets up to 4 secant steps on the cold map, then
//   NoConvergence. So an Ok row of either route carries the same accuracy bound,
//   and PerContract's iv equals american_implied_vol's bit for bit on every row
//   that already met it. The engines still differ: PerContract can refuse a row
//   Lockstep solves (the scalar stopped in a flat region the Lockstep bracket walks
//   out of), and either can run out of budget where the other converges.
//   3. Opt-in (ChainIvOptions::refuse_non_monotone; L1c, 2026-10-18): the cold
//      Andersen-Lake map is not monotone in sigma inside the kernel's own exercise
//      region close to the zero-vol value (american_iv.hpp, "Typed entry point with
//      the opt-in non-monotone refusal"), so a target there can have several roots
//      and the two engines return different ones. With the flag, a row whose target
//      lies within al_non_monotone_gate of its zero-vol value is NonMonotoneRegion
//      (a) when a resolved chord of two evaluated points has a slope below
//      -vega_floor * K (the slope floor: PerContract's verification chords,
//      Lockstep's iterate chords), or (b) when its Ok vol satisfies
//      al_non_monotone_region (the spot in the kernel's exercise region at that vol:
//      one more boundary solve, inside the band only). Targets above the band are
//      never refused by rule 3.
//
// ## Status, errors, allocation
//
// Each contract gets one ChainRowStatus byte; a non-Ok row holds NaN in every
// requested output column. The call returns Result<std::size_t> (the contract count)
// and reports only call-shape errors there: mismatched or missing columns, an output
// that overlaps an input or another output, a non-finite or non-positive spot. Once
// the ChainBatchWorkspace is reserved the call allocates nothing itself; a row the
// kernel refuses still builds the kernel's own Error message, and a kernel bad_alloc
// is contained as Unpriceable.
//
// ## Threads and determinism
//
// Contracts are cut into fixed chunks of kChainBatchChunk in input order. With a
// null executor the chunks run in order on the calling thread. With an executor,
// PricingExecutor::run_dynamic hands whole chunks to the calling thread and the pool
// workers (pinned by configure_pricing_executor's topology); the chunk boundaries do
// not depend on the thread count, each output slot is written by one context, and a
// row depends only on its own inputs (Scalar, PerContract) or on its fixed chunk
// (the LanedAvx2 and Lockstep packs), so outputs are bit-identical for every
// n_threads. A call issued from inside a pool dispatch runs inline (executor rule).
// The free functions are pure in their inputs; concurrent calls need disjoint
// outputs and their own workspace.
//
// Tier-B: public and includable, outside the frozen umbrella.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "atx/vol/api/core/types.hpp"       // Side, Result
#include "atx/vol/api/pricing/american.hpp" // AlOpts

namespace atx::vol {

class PricingExecutor; // pricing_executor.hpp

// Contracts per fan-out chunk (fixed: the partition never depends on thread count).
inline constexpr std::size_t kChainBatchChunk = 32;
// Contracts per laned pack (LanedAvx2 route, Lockstep IV route).
inline constexpr std::size_t kChainBatchLanes = 4;

// Per-contract outcome. One byte; the hot loop never builds a message.
enum class ChainRowStatus : std::uint8_t {
  Ok = 0,
  InvalidInput = 1,  // failed boundary validation (strike, sigma, target, expiry index or
                     // the expiry's own T/rate/carry/div_pv); never sent to the kernel
  Unpriceable = 2,   // the kernel refused the contract (e.g. the double-continuation
                     // regime), returned a non-finite requested output, or (implied vol)
                     // the zero-vol bound overflows
  VegaFloor = 3,     // implied vol: ill-posed. A target within the band of the zero-vol
                     // American value (every sigma up to the exercise edge, or below
                     // kIvMin, reproduces it), a root at or below kIvMin, or a slope at or
                     // below vega_floor * K where the target is matched (PerContract:
                     // also at american_implied_vol's answer)
  OutOfBand = 4,     // implied vol: target above the price ceiling, below the immediate
                     // intrinsic or the zero-vol value by more than the band, or beyond
                     // the image of [kIvMin, kIvMax]
  NoConvergence = 5, // implied vol: the acceptance rule was not met within the budget
  NonMonotoneRegion = 6, // implied vol, opt-in (ChainIvOptions::refuse_non_monotone): the
                         // answer sits where the cold Andersen-Lake map is not monotone
                         // in sigma (rule 3 of "One implied-vol refusal rule")
};

// One entry per expiry e. T is the year fraction (> 0), rate / carry the continuous
// r and q (q = borrow plus dividend yield), div_pv the spot-time PV of the cash
// dividends paid before the expiry (empty span = none; folded into the carry by
// chain_effective_carry).
struct ChainExpiryColumns {
  std::span<const double> T;
  std::span<const double> rate;
  std::span<const double> carry;
  std::span<const double> div_pv;

  [[nodiscard]] std::size_t size() const noexcept { return T.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = T.size();
    return rate.size() == n && carry.size() == n && (div_pv.empty() || div_pv.size() == n);
  }
};

// One entry per contract i. expiry[i] indexes ChainExpiryColumns. sigma is the
// pricing vol; for implied_vol_chain_batch it is the warm seed. Any order.
struct ChainContractColumns {
  std::span<const double> strike;
  std::span<const std::uint32_t> expiry;
  std::span<const Side> side;
  std::span<const double> sigma;

  [[nodiscard]] std::size_t size() const noexcept { return strike.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = strike.size();
    return expiry.size() == n && side.size() == n && sigma.size() == n;
  }
};

// Output columns, one entry per contract in input order. price and status are
// required; each Greek column is optional (an empty span is not computed, and an
// empty vega / rho skips that pair of boundary solves). theta is the calendar
// derivative dV/dt per year (american_greeks_al convention), vega per unit sigma,
// rho per unit rate.
struct ChainGreekColumns {
  std::span<double> price;
  std::span<double> delta;
  std::span<double> gamma;
  std::span<double> vega;
  std::span<double> theta;
  std::span<double> rho;
  std::span<ChainRowStatus> status;

  [[nodiscard]] std::size_t size() const noexcept { return price.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = price.size();
    const auto opt = [n](std::span<double> s) noexcept { return s.empty() || s.size() == n; };
    return status.size() == n && opt(delta) && opt(gamma) && opt(vega) && opt(theta) &&
           opt(rho);
  }
  [[nodiscard]] bool price_only() const noexcept {
    return delta.empty() && gamma.empty() && vega.empty() && theta.empty() && rho.empty();
  }
};

enum class ChainBatchRoute : std::uint8_t {
  Scalar = 0,    // per-contract kernel calls, bit-identical to the per-contract loop
  LanedAvx2 = 1, // opt-in: laned AVX2 Greeks kernels, within their economic gate
};

struct ChainBatchOptions {
  std::optional<AlOpts> al_opts = std::nullopt; // nullopt = the accurate preset
  ChainBatchRoute route = ChainBatchRoute::Scalar;
  PricingExecutor *executor = nullptr; // null = inline on the calling thread
  unsigned n_threads = 0;              // with an executor: 0 = caller + whole pool, else a cap
};

enum class ChainIvRoute : std::uint8_t {
  PerContract = 0, // american_implied_vol per contract, warm seed = own sigma
  Lockstep = 1,    // opt-in: 4-lane lockstep Newton-secant on the laned AVX2 map
};

struct ChainIvOptions {
  std::optional<AlOpts> al_opts = std::nullopt; // the forward map's preset (nullopt = accurate)
  ChainIvRoute route = ChainIvRoute::PerContract;
  // Both routes: the largest accepted Newton correction |f / slope| (Lockstep: or the
  // evaluated bracket width). PerContract also passes it to american_implied_vol.
  double sigma_tol = 1.0e-7;
  // PerContract: american_implied_vol's cap. Lockstep: pack iterations.
  std::uint16_t max_iter = 64;
  // Both routes: an Ok row has |P(sigma) - target| <= price_tol * max(1, target).
  double price_tol = 1.0e-10;
  // Both routes: the zero-vol band's width relative to the price ceiling (raised to
  // american_implied_vol's 1e-9 when smaller; see "One implied-vol refusal rule").
  double intrinsic_tol = 1.0e-9;
  // Both routes: a matching sigma whose American slope is <= vega_floor * K is VegaFloor.
  double vega_floor = 1.0e-8;
  PricingExecutor *executor = nullptr;
  unsigned n_threads = 0;
  // OPT-IN (L1c, 2026-10-18): rule 3 of "One implied-vol refusal rule". false keeps
  // every output bit-identical to the routes without it.
  bool refuse_non_monotone = false;
};

// q_eff = carry - log1p(-div_pv / spot) / T: the continuous carry whose forward
// spot * exp((rate - q_eff) T) equals the escrowed forward (spot - div_pv) *
// exp((rate - carry) T). Exactly `carry` when div_pv == 0. NaN when spot or T is not
// finite > 0, carry is not finite, or div_pv is not in [0, spot).
[[nodiscard]] double chain_effective_carry(double spot, double T, double carry,
                                           double div_pv) noexcept;

// Caller-owned scratch: the per-expiry hoisted constants. Single-owner (one per
// concurrent call). reserve() allocates; a call on an undersized workspace grows it
// first (on the calling thread, before any fan-out) and allocates only then. A
// moved-from workspace has expiry_capacity() 0 and stays usable: its next reserve()
// or call allocates fresh storage.
class ChainBatchWorkspace {
public:
  ChainBatchWorkspace();
  ~ChainBatchWorkspace();
  ChainBatchWorkspace(ChainBatchWorkspace &&) noexcept;
  ChainBatchWorkspace &operator=(ChainBatchWorkspace &&) noexcept;
  ChainBatchWorkspace(const ChainBatchWorkspace &) = delete;
  ChainBatchWorkspace &operator=(const ChainBatchWorkspace &) = delete;

  // Size for chains of up to `max_expiries` expiries. Idempotent; never shrinks.
  void reserve(std::size_t max_expiries);

  [[nodiscard]] std::size_t expiry_capacity() const noexcept;

  // Implementation access for the batch_chain sources; the type is opaque here.
  // Precondition: reserve() ran since construction or the last move-from (both entry
  // points reserve first).
  struct Impl;
  [[nodiscard]] Impl &impl() noexcept { return *impl_; }

private:
  std::unique_ptr<Impl> impl_;
};

// Price (and the requested Greeks of) every contract of the chain at `spot`.
//
// @return the contract count, or InvalidArgument when a column set is inconsistent,
//         the output length differs from the contract count, an output overlaps an
//         input or another output, or spot is not finite > 0 (nothing is written).
[[nodiscard]] Result<std::size_t> price_chain_batch(double spot,
                                                    const ChainExpiryColumns &expiries,
                                                    const ChainContractColumns &contracts,
                                                    const ChainGreekColumns &out,
                                                    const ChainBatchOptions &options,
                                                    ChainBatchWorkspace &ws);

// American implied vol of target_price[i] for every contract, seeded at
// contracts.sigma[i]. iv_out[i] is the vol for an Ok row and NaN otherwise.
//
// @return the contract count, or InvalidArgument as price_chain_batch (here the
//         target, iv and status spans must match the contract count).
[[nodiscard]] Result<std::size_t> implied_vol_chain_batch(double spot,
                                                          const ChainExpiryColumns &expiries,
                                                          const ChainContractColumns &contracts,
                                                          std::span<const double> target_price,
                                                          std::span<double> iv_out,
                                                          std::span<ChainRowStatus> status_out,
                                                          const ChainIvOptions &options,
                                                          ChainBatchWorkspace &ws);

} // namespace atx::vol
