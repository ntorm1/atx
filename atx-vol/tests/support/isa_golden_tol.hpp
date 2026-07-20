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

// Tolerance for *accumulated* economic scalars — a backtest's running cash /
// turnover, a portfolio's column-sum totals — that were pinned with
// EXPECT_DOUBLE_EQ or bit-identity. Unlike a single evaluation (golden_isa_tol,
// ~13 ULP peak drift), an accumulator sums per-evaluation drift over thousands of
// grid evaluations, so its *absolute* delta compounds well past the 32-ULP
// single-value band.
//
// TWO independent drift sources now feed this band, and only one of them is a
// property of how the TEST was compiled:
//
//  1. FMA contraction (compile-time, __FMA__). The rel-avx2 preset fuses a*b+c
//     into one rounding, so a scalar hot path lands 1-few ULP off the SSE2
//     source-of-truth. Observed 2.4e-12 on an O(1) running cash total (~5.5e-13
//     relative). This is what the original band was written for.
//
//  2. The laned analytic-greeks AVX2 kernel (RUNTIME, WS-P1a). Shipping under
//     Auto ISA (kShipAvx2Greeks), american_greeks_avx2.cpp is selected by CPU
//     capability at run time, NOT by how this TU was compiled. Its AVX2
//     transcendentals differ from the scalar libm oracle by ~1e-13 in price
//     (economic-gate documented at american_boundary_batch.cpp), and the FD
//     denominators of the bundle amplify that into the greeks.
//
// Source 2 is why this band no longer keys on __FMA__. A test built for the SSE2
// reference ISA still routes its greeks through the AVX2 kernel on an AVX2 host,
// so the old "reference ISA is byte-exact, keep the 4-ULP gate" premise is void:
// the 4-ULP base branch was failing on the shipping configuration it is meant to
// gate.
//
// MEASURED WORST CASE on this band's call sites (dev preset, laned greeks on),
// from backtest_test's running `cash` accumulator in
// Backtest.DailyTwoLegRollReusesExactPnlTargetMarksWithoutChangingEconomics:
//   step 0  expected -3.0734556197676284   |delta| 2.2737367544323206e-13  7.40e-14 rel
//   step 1  expected -3.548979869780851    |delta| 2.2737367544323206e-13  6.41e-14 rel
//   step 2  expected -4.0009109642776366   |delta| 2.2737367544323206e-13  5.68e-14 rel
//   step 3  expected -4.4307747789998757   |delta| 2.2737367544323206e-12  5.13e-13 rel
// The last step is the compounded worst. `turnover_notional` peaks far lower
// (9.0949470177292824e-13 abs on an O(4.4e3) total, ~2.0e-16 relative), and
// Backtest.PriceBpsRollCloseReusesPnlMarkWithoutASecondSurfaceSolve's cash[1]
// runs 2.2737367544323206e-13 abs on -69.997585204798639 (~3.2e-15 relative).
// Measured by forcing this function to return 0 and reading the EXPECT_NEAR
// differences gtest prints.
//
// BAND: 1e-11 relative — ~20x over the 5.13e-13 measured worst, so normal
// run-to-run and host-to-host variation of the same magnitude stays green, while
// remaining ~2 orders TIGHTER than the FMA branch's old 1e-9 and ~7 orders inside
// any P&L-material move. A genuine economic regression still trips it.
//
// SCOPE OF THE TIGHTENING — deliberately limited to what has been MEASURED.
// This band is tightened because TWO independent measurements of these call
// sites agree: 5.13e-13 relative on the dev preset (laned greeks, measured
// above) and ~5.5e-13 relative on rel-avx2 (the FMA-contraction figure the
// original WS-0 comment recorded). 1e-11 is ~20x and ~18x those respectively.
//
// golden_accum_close() below used to route through this function on its __FMA__
// branch, which would have silently inherited the same 100x tightening. It does
// NOT any more — it now has its own kAccumCloseRelBandFma, left at the original
// 1e-9. Reason: its only caller is lifecycle_integration_test.cpp, whose totals
// have NO measurement on either preset, and on the dev preset that branch is not
// even taken (it returns bit-equality), so no gate reachable from here can
// exercise it. Tightening an unexercised path on the strength of "probably fine"
// is how a band detonates two sprints later. If someone measures those totals on
// rel-avx2, fold the two constants back together and say so.
inline constexpr double kAccumRelBand = 1.0e-11;

[[nodiscard]] inline double golden_isa_accum_tol(double expected) noexcept {
  const double scale = std::fmax(1.0, std::fabs(expected));
  const double base = 4.0 * std::numeric_limits<double>::epsilon() * scale;
  return std::fmax(base, kAccumRelBand * scale);
}

// ── Laned-greeks route parity (WS-P1a) ──────────────────────────────────────
//
// A distinct band from the two above, for a distinct claim. Several tests assert
// that a value produced by the BATCHED pricing path equals the same value
// re-queried through a SINGLE-contract call (PricedSurface::greeks_analytic), or
// that the analytic greek route matches the FD route. These were `bits_equal` /
// `EXPECT_EQ` on doubles: a ROUTE-PARITY claim, not a golden value pin.
//
// Since WS-P1a ships the laned analytic-greeks kernel under Auto ISA, the batched
// path routes through american_greeks_avx2.cpp while a single-contract re-query
// can land on the scalar american_greeks_al oracle. The two differ by the AVX2
// transcendentals (~1e-13 in price), amplified by the bundle's FD denominators —
// exactly the divergence the WS-K ship gate measured and accepted as economically
// negligible. Bit-equality between the two routes is therefore no longer the
// correct statement of the invariant; relative agreement is.
//
// PURE relative (scale = max(|a|,|b|)), unlike golden_isa_accum_tol's
// max(1,|expected|): these are per-contract greeks whose magnitudes span orders
// (a gamma of 0.0159 next to a vega of 27.5), so a max(...,1) scale would make
// the band meaninglessly loose on the small columns.
//
// MEASURED WORST CASE across every call site below (dev preset, laned greeks on):
//   gamma   0.015895175603652941 vs 0.015895175605784569   1.3410535750888835e-10
//   theta -10.391444231650004   vs -10.391444232928901     1.2307211198398948e-10
//   vega   27.582757396789503   vs 27.582757396793944      1.610024710225965e-13
//   vega   27.531219351482505   vs 27.531219351486058      1.290430922598688e-13
//   price  10.172058875992983   vs 10.172058875992976      6.985240101559459e-16
// (Measured by instrumenting this predicate to print every comparison it makes.)
// BAND: 1e-9 — ~7.5x over the 1.3410535750888835e-10 worst. This is also the
// sprint's declared STOP threshold: a divergence past 1e-9 relative is NOT
// laned-greeks drift and must be escalated as a real regression rather than
// absorbed here.
inline constexpr double kLanedGreeksRelBand = 1.0e-9;

[[nodiscard]] inline bool laned_greeks_close(double got, double expected) noexcept {
  if (std::isnan(got) || std::isnan(expected)) {
    return std::isnan(got) && std::isnan(expected); // preserve bits_equal's NaN discrimination
  }
  const double scale = std::fmax(std::fabs(got), std::fabs(expected));
  if (scale == 0.0) {
    // Reachable only when BOTH values are exactly zero. This is deliberately NOT
    // a re-imposed bit-equality gate: under IEEE-754 `+0.0 == -0.0` is true, so
    // this branch ACCEPTS the mixed-sign-zero pair that the old bits_equal
    // rejected (verified: bits_equal(+0,-0) = false, (+0 == -0) = true). It is
    // therefore strictly more permissive than the predicate it replaced.
    //
    // It is also exactly equivalent to the general path below, which would yield
    // |0 - 0| = 0 <= 1e-9 * 0 = 0, i.e. true. Kept as an explicit, self-documenting
    // case so the zero handling does not silently depend on the band being purely
    // multiplicative — were the band ever given an additive floor, this branch
    // would still say the right thing.
    return got == expected;
  }
  return std::fabs(got - expected) <= kLanedGreeksRelBand * scale;
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

// Band for golden_accum_close's FMA branch. Held at the ORIGINAL 1e-9 on purpose
// — see "SCOPE OF THE TIGHTENING" above. This is the UNMEASURED path: its only
// caller (lifecycle_integration_test.cpp) has no recorded drift figure on either
// preset, and the dev preset takes the bit-equality branch instead, so nothing
// reachable from the dev gate exercises it. It is a separate constant purely so
// that tightening kAccumRelBand cannot silently drag this along with it.
inline constexpr double kAccumCloseRelBandFma = 1.0e-9;

// Like golden_close (byte-exact on the reference ISA — the SSE2 gate is untouched)
// but the FMA band is a RELATIVE accumulator band instead of the 32-ULP
// single-value one. For reduction *totals* (e.g. a portfolio's column-sum totals
// vs a serial re-sum) whose per-element marks stay byte-exact but whose FMA
// reduction *order* drifts more than 32 ULP.
[[nodiscard]] inline bool golden_accum_close(double got, double expected) noexcept {
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
    const double scale = std::fmax(1.0, std::fabs(expected));
    const double base = 4.0 * std::numeric_limits<double>::epsilon() * scale;
    return std::fabs(got - expected) <= std::fmax(base, kAccumCloseRelBandFma * scale);
  }
}

} // namespace atx::vol::test
