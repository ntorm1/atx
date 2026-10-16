#pragma once

// Tick-path chain refresh (pricing hot-path audit 2026-10-16, items 1-5).
//
// After a spot tick a market maker re-marks a whole listed chain: price plus
// delta / gamma / vega / theta for every contract, inside a few hundred
// microseconds on one core. This header is the API for that path.
//
// ## Execution contract (every entry point in this header)
//
//   * INLINE on the calling thread. Nothing here dispatches to the pricing pool:
//     a PricingExecutor wake costs ~124 us (pricing_executor.cpp), 41% of the
//     300 us chain budget, so the tick path must never touch it. Pin the calling
//     thread to an isolated P-core instead (the bench does, audit item 5a).
//   * Caller-owned structure-of-arrays outputs, sized once and reused per tick.
//   * One `TickStatus` byte per contract instead of a per-contract
//     Result<..., std::string>; `Status` reports only call-shape errors, at the
//     boundary.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.
//
// Thread-safety: the free functions are pure in their inputs and write only the
// caller's output spans, so concurrent calls with disjoint outputs are safe.

#include <cstddef>
#include <cstdint>
#include <span>

#include "atx/vol/api/core/types.hpp"         // Side, Status
#include "atx/vol/api/fitting/correction.hpp" // AmericanCorrectionCaches

namespace atx::vol {

// Per-contract outcome of one tick refresh. A single byte so a 2,000-contract
// status column is 2 KB and the hot loop never builds an error string.
enum class TickStatus : std::uint8_t {
  Ok = 0,          // served by the route's continuation-region model
  Exercised = 1,   // spot is in the early-exercise region: intrinsic value,
                   // delta = +/-1 and gamma = vega = theta = 0 (the cold
                   // reference american_greeks_al reports theta = 0 there too)
  OutOfWindow = 2, // record route: spot left this contract's certified window
                   // and no fallback was allowed; outputs are NaN
  Fallback = 3,    // served by the route's documented fallback (e.g. the cached
                   // route with no cache for this side serves the Black-76 leg)
  Unpriceable = 4, // the model refused this contract; outputs are NaN
};

// A listed chain as caller-owned columns, one entry per contract. `expiry` is
// the year fraction T, `rate`/`carry` the continuous r and q (carry = the
// expiry's effective q, i.e. borrow plus dividend yield). Contracts may be in
// any order; routes that group by expiry do so at build time.
struct ChainContractsView {
  std::span<const double> strike;
  std::span<const double> expiry;
  std::span<const double> sigma;
  std::span<const double> rate;
  std::span<const double> carry;
  std::span<const Side> side;

  [[nodiscard]] std::size_t size() const noexcept { return strike.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = strike.size();
    return expiry.size() == n && sigma.size() == n && rate.size() == n &&
           carry.size() == n && side.size() == n;
  }
};

// Caller-owned output columns, one entry per contract, in the input order.
// theta is the calendar derivative dV/dt per year (the american_greeks_al
// convention), vega is per unit of sigma.
struct ChainTickOutputs {
  std::span<double> price;
  std::span<double> delta;
  std::span<double> gamma;
  std::span<double> vega;
  std::span<double> theta;
  std::span<TickStatus> status;

  [[nodiscard]] std::size_t size() const noexcept { return price.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = price.size();
    return delta.size() == n && gamma.size() == n && vega.size() == n &&
           theta.size() == n && status.size() == n;
  }
};

// Baseline tick route (audit §4 `cached_scalar`): every contract is re-marked
// with the cached-correction Greek bundle, american_greeks(S, K, T, sigma, r, q,
// side, cache), inline. The same numbers a per-contract loop over that function
// produces (bit-identical), with TickStatus::Fallback where `caches` has no
// cache for the contract's side (the Black-76 leg is then served, exactly as
// american_greeks documents) and TickStatus::Unpriceable + NaN where it refuses.
// This route still pays a full 3-D second-order correction sweep per contract;
// it exists as the measured baseline the record routes are compared against.
//
// @return InvalidArgument when the columns disagree in length or `spot` is not
//         a finite positive number; nothing is written then.
[[nodiscard]] Status refresh_chain_cached(double spot, const ChainContractsView &chain,
                                          const AmericanCorrectionCaches &caches,
                                          const ChainTickOutputs &out);

} // namespace atx::vol
