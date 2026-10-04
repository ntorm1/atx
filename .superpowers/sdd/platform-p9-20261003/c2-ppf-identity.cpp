// P9 C2 M1c-RED identity evidence (scratch, not committed): the committed contraction-free
// norm_ppf against the pre-fix one (oldinc/, wrapped in namespace old_ns), both evaluated at RUN
// TIME on every u the norm-score-v1 kernel can form for n <= 4000 names (k / (2 (n + 1)), k in
// [2, 2n]) and on 4M pseudo-random p in (0, 1) including both tails. Prints the mismatch counts.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>
#include "atx/core/types.hpp"
#include "atx/engine/eval/stats_ext.hpp"

namespace old_ns::atx {
using ::atx::f64;
using ::atx::usize;
} // namespace old_ns::atx
namespace old_ns {
#include "oldinc/atx/engine/eval/stats_ext.hpp"
} // namespace old_ns

int main(int argc, char**) {
  std::uint64_t evaluated = 0, mismatched = 0;
  const auto check = [&](double p) {
    const double a = ::atx::engine::eval::norm_ppf(p);
    const double b = old_ns::atx::engine::eval::norm_ppf(p);
    ++evaluated;
    if (std::bit_cast<std::uint64_t>(a) != std::bit_cast<std::uint64_t>(b)) {
      if (mismatched < 5) std::printf("mismatch p=%.17g new=%.17g old=%.17g\n", p, a, b);
      ++mismatched;
    }
  };
  // argc keeps every argument a run-time value (never folded).
  const double one = static_cast<double>(argc);
  for (std::uint64_t n = 2; n <= 4000; ++n)
    for (std::uint64_t k = 2; k <= 2 * n; ++k)
      check(one * static_cast<double>(k) / (2.0 * (static_cast<double>(n) + 1.0)));
  std::uint64_t state = 0x9e3779b97f4a7c15ULL * static_cast<std::uint64_t>(argc);
  for (int i = 0; i < 4000000; ++i) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    double p = static_cast<double>(state >> 11) * 0x1.0p-53;
    if (i % 4 == 1) p *= 0.02425;       // lower tail
    if (i % 4 == 2) p = 1.0 - p * 0.02425; // upper tail
    if (p > 0.0 && p < 1.0) check(p);
  }
  std::printf("evaluated %llu mismatched %llu\n", static_cast<unsigned long long>(evaluated),
              static_cast<unsigned long long>(mismatched));
  return mismatched == 0 ? 0 : 1;
}
