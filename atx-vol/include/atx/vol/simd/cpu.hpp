#pragma once

// Runtime CPU capability query for the atx-vol SIMD kernels.
//
// The library ships a scalar baseline (always correct on any x86-64) plus AVX2
// fast paths compiled into dedicated `*_avx2.cpp` translation units. Each public
// batch entry point calls have_avx2() once and dispatches to the AVX2 kernel or
// the scalar loop. Detection is via CPUID + XGETBV (verifies the OS has enabled
// YMM state), so a binary built with AVX2 kernels still runs correctly — falling
// back to scalar — on a CPU or VM where AVX2 is unavailable.

namespace atx::vol::simd {

// True iff the host supports AVX2 + FMA and the OS has enabled AVX state.
// Result is detected once and cached; safe to call from any thread and cheap
// enough for the hot path (a relaxed load of a cached bool after first call).
[[nodiscard]] bool have_avx2() noexcept;

// ── Test/CI-forceable ISA seam (P3.1) ────────────────────────────────────
//
// A process-global override that lets a test (or a CLI flag) pin the SIMD
// dispatch onto a specific path regardless of the host CPUID result. It exists so
// P3's acceptance is testable:
//   * ForceScalar — prove the scalar path emits NO AVX2 instruction (illegal-
//     instruction-clean CI on a non-AVX2 host / VM), and that the scalar path is
//     bit-identical to the reference kernels.
//   * ForceAvx2   — exercise the AVX2 kernel on demand and assert it matches the
//     scalar reference to the accuracy gate. Only legal on an AVX2 host: callers
//     (tests) must GTEST_SKIP when !have_avx2() before forcing it.
// Auto (the default) resolves to have_avx2().
//
// NOTE (scope): only the NEW American boundary batch dispatch consults this seam
// (via use_avx2()). The existing black76/greeks/iv/essvi/pnl batch kernels still
// gate directly on have_avx2() and are intentionally NOT rewired here (T13 scope).
enum class SimdIsa { Auto, ForceScalar, ForceAvx2 };

// Install the override. Thread-safe (a relaxed atomic store); intended as coarse,
// set-once control from a test main or a startup flag, not per-call toggling.
void set_simd_isa_override(SimdIsa isa) noexcept;

// The currently-installed override (default Auto).
[[nodiscard]] SimdIsa simd_isa_override() noexcept;

// The effective dispatch decision the American boundary batch consults:
//   Auto        -> have_avx2()
//   ForceScalar -> false
//   ForceAvx2   -> true   (undefined on a non-AVX2 host; guard with have_avx2())
[[nodiscard]] bool use_avx2() noexcept;

} // namespace atx::vol::simd
