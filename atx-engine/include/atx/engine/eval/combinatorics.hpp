#pragma once

#include <span>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::eval {
namespace detail {
// C(n,k), exact multiplicative recurrence when every multiply-before-divide
// temporary fits usize. Callers must bound that temporary, not just C(n,k).
// PBO uses n<=16: even 8*C(16,8)=102960 fits. Other consumers own their bounds.
// Returns zero for k>n; symmetry reduces the number of recurrence steps.
[[nodiscard]] inline atx::usize binomial(atx::usize n, atx::usize k) noexcept {
  if (k > n) {
    return 0U;
  }
  const atx::usize kk = (k > n - k) ? (n - k) : k; // smaller arm, by symmetry
  atx::usize result = 1U;
  for (atx::usize i = 0U; i < kk; ++i) {
    [[maybe_unused]] const atx::usize prev = result;
    // result *= (n - kk + 1 + i); result /= (i + 1) — exact at each step because
    // the running product is always a binomial coefficient (an integer).
    result = result * (n - kk + 1U + i) / (i + 1U);
    // C(m, j) is non-decreasing as the walk grows the coefficient toward C(n, kk);
    // a decrease would signal a wraparound (overflow), which the S<=16 guard rules out.
    ATX_ASSERT(result >= prev);
  }
  return result;
}

// ---------------------------------------------------------------------------
//  next_combination — advance a strictly-ascending k-subset of [0, n) to the
//  lexicographically next one. Returns false when `comb` is the final (largest)
//  combination, leaving it unchanged. Deterministic; no allocation; no RNG.
//
//  Standard index walk: find the rightmost element that can be incremented
//  (comb[i] < n - k + i), bump it, then reset every element to its right to the
//  minimal increasing run. `comb` must hold k strictly-ascending indices < n.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool next_combination(std::span<atx::usize> comb, atx::usize n) noexcept {
  const atx::usize k = comb.size();
  if (k == 0U) {
    return false; // the empty subset is its own only combination
  }
  // Walk from the right to the first index that has headroom to increment.
  atx::usize i = k; // one past the last; loop decrements before use
  while (i > 0U) {
    --i;
    const atx::usize ceiling = n - k + i; // max legal value at position i
    if (comb[i] < ceiling) {
      ++comb[i];
      // Reset the suffix to the minimal ascending run after comb[i].
      for (atx::usize j = i + 1U; j < k; ++j) {
        comb[j] = comb[j - 1U] + 1U;
      }
      return true;
    }
  }
  return false; // already the last combination
}

} // namespace detail
} // namespace atx::engine::eval
