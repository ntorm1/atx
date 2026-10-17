#pragma once

// Shared internals of batch_chain.cpp (price / Greeks) and batch_chain_iv.cpp
// (implied vol). Not installed; see api/pricing/batch_chain.hpp for the contract.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/api/pricing/batch_chain.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"

namespace atx::vol {

// Per-expiry hoisted constants (index e), sized to the capacity; a call reads only
// its first n_expiries entries.
struct ChainBatchWorkspace::Impl {
  std::vector<double> q_eff;  // chain_effective_carry(spot, T, carry, div_pv)
  std::vector<double> df;     // exp(-rate T)
  std::vector<double> fwd;    // exp((rate - q_eff) T): forward = spot * fwd
  std::vector<double> sqrt_t; // sqrt(T)
  std::vector<std::uint8_t> expiry_ok;
};

namespace batch_chain_detail {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// True iff an output byte range intersects an input range or another output range
// (empty spans never intersect). Quadratic in the small fixed span count.
[[nodiscard]] bool outputs_alias(std::initializer_list<std::span<const std::byte>> outs,
                                 std::initializer_list<std::span<const std::byte>> ins) noexcept;

// Grows `ws` to cover the call (allocates only when it is undersized), then writes
// the per-expiry constants for `spot`. An expiry with a non-finite / non-positive T,
// a non-finite rate or carry, or a div_pv outside [0, spot) gets expiry_ok = 0.
void prepare_expiries(double spot, const ChainExpiryColumns &expiries, ChainBatchWorkspace &ws);

// Contract i is priceable: a valid expiry index of a valid expiry, a finite
// positive strike and sigma, and a Call / Put side.
[[nodiscard]] inline bool contract_ok(const ChainContractColumns &c,
                                      const ChainBatchWorkspace::Impl &w, std::size_t n_expiries,
                                      std::size_t i) noexcept {
  const std::uint32_t e = c.expiry[i];
  if (e >= n_expiries || w.expiry_ok[e] == 0u) {
    return false;
  }
  const double K = c.strike[i];
  const double s = c.sigma[i];
  const Side sd = c.side[i];
  return K > 0.0 && K < std::numeric_limits<double>::infinity() && s > 0.0 &&
         s < std::numeric_limits<double>::infinity() && (sd == Side::Call || sd == Side::Put);
}

// Runs fn(lo, hi) for every fixed chunk [lo, hi) of kChainBatchChunk rows (the last
// one shorter). Inline in chunk order when `ex` is null or there is one chunk;
// otherwise PricingExecutor::run_dynamic claims whole chunks for the calling thread
// and the pool workers. The partition depends only on n; fn must write only the
// rows of its own chunk.
template <class Fn>
void for_each_chunk(std::size_t n, PricingExecutor *ex, unsigned n_threads, Fn &&fn) {
  const std::size_t n_chunks = (n + kChainBatchChunk - 1u) / kChainBatchChunk;
  // Not const: run_dynamic type-erases the closure through a void *.
  auto body = [&fn, n](std::size_t c, unsigned /*worker_id*/) noexcept {
    const std::size_t lo = c * kChainBatchChunk;
    fn(lo, std::min(n, lo + kChainBatchChunk));
  };
  if (ex == nullptr || n_chunks < 2u) {
    for (std::size_t c = 0; c < n_chunks; ++c) {
      body(c, 0u);
    }
    return;
  }
  ex->run_dynamic(n_chunks, n_threads, body);
}

} // namespace batch_chain_detail
} // namespace atx::vol
