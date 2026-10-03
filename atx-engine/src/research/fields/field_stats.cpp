#include "atx/engine/research/fields/field_stats.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <system_error>

namespace atx::engine::research::fields {

namespace {
constexpr usize kUnroll = 8;          // numpy's eight accumulators
constexpr usize kPairwiseBlock = 128; // numpy PW_BLOCKSIZE
} // namespace

f64 numpy_pairwise_sum(std::span<const f64> values) noexcept {
  const usize n = values.size();
  if (n < kUnroll) {
    f64 res = 0.0;
    for (const f64 x : values) {
      res += x;
    }
    return res;
  }
  if (n <= kPairwiseBlock) {
    std::array<f64, kUnroll> r{};
    for (usize j = 0; j < kUnroll; ++j) {
      r[j] = values[j];
    }
    usize i = kUnroll;
    for (; i < n - (n % kUnroll); i += kUnroll) {
      for (usize j = 0; j < kUnroll; ++j) {
        r[j] += values[i + j];
      }
    }
    const f64 left = (r[0] + r[1]) + (r[2] + r[3]);
    const f64 right = (r[4] + r[5]) + (r[6] + r[7]);
    f64 res = left + right;
    for (; i < n; ++i) {
      res += values[i];
    }
    return res;
  }
  // Recursion depth is at most log2(n) (each level at least halves n), so it is bounded by 64.
  usize n2 = n / 2;
  n2 -= n2 % kUnroll;
  const f64 head = numpy_pairwise_sum(values.first(n2));
  const f64 tail = numpy_pairwise_sum(values.subspan(n2));
  return head + tail;
}

f64 numpy_linear_quantile(std::span<const f64> sorted, f64 q) noexcept {
  const usize n = sorted.size();
  const f64 last = static_cast<f64>(n - 1);
  const f64 virtual_index = last * q;
  usize lo = 0;
  usize hi = 0;
  f64 previous = 0.0;
  if (virtual_index >= last) {
    lo = n - 1;
    hi = n - 1;
    previous = -1.0; // numpy marks the above-bounds index -1 before it computes gamma
  } else if (virtual_index >= 0.0) {
    const f64 floor_index = std::floor(virtual_index);
    lo = static_cast<usize>(floor_index);
    hi = lo + 1;
    previous = floor_index;
  }
  const f64 gamma = virtual_index - previous;
  const f64 a = sorted[lo];
  const f64 b = sorted[hi];
  const f64 diff = b - a;
  if (gamma >= 0.5) {
    const f64 complement = 1.0 - gamma;
    const f64 step = diff * complement;
    return b - step;
  }
  const f64 step = diff * gamma;
  return a + step;
}

std::optional<f64> rounded_fraction(u64 finite, u64 member) {
  if (member == 0) {
    return std::nullopt;
  }
  const f64 x = static_cast<f64>(finite) / static_cast<f64>(member);
  std::array<char, 64> buf{};
  const auto printed =
      std::to_chars(buf.data(), buf.data() + buf.size(), x, std::chars_format::fixed, 6);
  if (printed.ec != std::errc{}) {
    return std::nullopt;
  }
  f64 out = 0.0;
  const auto parsed = std::from_chars(buf.data(), printed.ptr, out);
  if (parsed.ec != std::errc{}) {
    return std::nullopt;
  }
  return out;
}

} // namespace atx::engine::research::fields
