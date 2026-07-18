#pragma once

// Per-ISA golden-pin tolerance (WS-0 / M4).
//
// Global constraint (plan §3, §11.5): *bit-identity is a telltale, not the gate*
// — the economic bound governs. FMA-contracting builds (the rel-avx2 acceptance
// preset, /arch:AVX2 → -mfma) fuse `a*b + c` into a single rounding, so a scalar
// hot path can land 1-few ULP away from the SSE2/Debug source-of-truth. That is
// contraction, NOT a regression: every such value stays byte-exact on the SSE2
// reference ISA and drifts only in the last places under FMA.
//
// A golden value-pin that hard-asserts bit-identity therefore *fails on rel-avx2
// while passing on Debug* — which blocks rel-avx2 from being a green acceptance
// gate. This header gives those pins a per-ISA tolerance band: ZERO (byte-exact)
// on the reference ISA, a machine-precision-class band under FMA. A real
// regression (>= ~1e-12, ~10 orders of magnitude past this band) still trips.
//
// Detection is `__FMA__` (defined by /arch:AVX2 or -mfma; absent on the SSE2
// default), so the SSE2 gate keeps its byte-exact guarantee untouched.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace atx::vol::test {

#if defined(__FMA__)
inline constexpr bool kFmaContraction = true;
#else
inline constexpr bool kFmaContraction = false;
#endif

// Golden-pin tolerance for `expected`: 0 on the reference ISA; 32 ULP (scaled by
// max(1,|expected|), mirroring the tests' local rounding_tolerance() at 4 ULP)
// under FMA. The observed FMA drift on the M4 golden pins peaks at 3.0e-15 abs
// (~13 ULP); 32 ULP clears it with >=2x margin while staying ~11 orders of
// magnitude inside any economic bound.
[[nodiscard]] inline double golden_isa_tol(double expected) noexcept {
  if constexpr (!kFmaContraction) {
    return 0.0;
  } else {
    return 32.0 * std::numeric_limits<double>::epsilon() * std::fmax(1.0, std::fabs(expected));
  }
}

// Byte-exact on the reference ISA (matches the tests' bits_equal / EXPECT_EQ
// intent, incl. NaN discrimination); within golden_isa_tol() under FMA.
[[nodiscard]] inline bool golden_close(double got, double expected) noexcept {
  if constexpr (!kFmaContraction) {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::memcpy(&a, &got, sizeof a);
    std::memcpy(&b, &expected, sizeof b);
    return a == b;
  } else {
    if (std::isnan(got) || std::isnan(expected)) {
      return std::isnan(got) && std::isnan(expected);
    }
    return std::fabs(got - expected) <= golden_isa_tol(expected);
  }
}

} // namespace atx::vol::test
