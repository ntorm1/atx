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
//     scalar reference to the accuracy gate. FIX-5/I2: this is a REQUEST, capped by
//     the host — on a machine without AVX2 it resolves to the scalar path rather
//     than to an illegal instruction. Tests that need the vector numbers should
//     still GTEST_SKIP on !have_avx2(), because there is nothing to compare.
// Auto (the default) resolves to have_avx2().
enum class SimdIsa { Auto, ForceScalar, ForceAvx2 };

// Install the override. Thread-safe (a relaxed atomic store); intended as coarse,
// set-once control from a test main or a startup flag, not per-call toggling.
void set_simd_isa_override(SimdIsa isa) noexcept;

// The currently-installed override (default Auto).
[[nodiscard]] SimdIsa simd_isa_override() noexcept;

// The pure decision behind use_avx2(), factored out so the non-AVX2-host arm is
// testable ON an AVX2 host (FIX-5/I2: the defect it pins cannot be observed
// through use_avx2() here, because have_avx2() is true and the two answers
// coincide). `host_has_avx2` is what have_avx2() would return.
[[nodiscard]] constexpr bool resolve_use_avx2(SimdIsa isa, bool host_has_avx2) noexcept {
    switch (isa) {
        case SimdIsa::ForceScalar:
            return false;
        case SimdIsa::ForceAvx2:
            // FIX-5/I2: ForceAvx2 REQUESTS the vector path, it does not assert the
            // host can execute it. ~15 dispatch sites gate AVX2 intrinsics on
            // use_avx2() alone (src/batch.cpp, simd/{black76,essvi,greeks,pnl}_batch.cpp)
            // and SIGILL on a host without AVX2 (see simd/vector_math_probe.hpp:12).
            // Since ATX_SIMD_ISA made this reachable from the environment of a
            // SHIPPING process (initial_isa_from_env, 371889a), the request must be
            // capped by capability — exactly as avx2_greeks_selected() already does
            // (american_boundary_batch.cpp:180-181). Behaviour-neutral on an AVX2 host.
            return host_has_avx2;
        case SimdIsa::Auto:
            break;
    }
    return host_has_avx2;
}

// The effective dispatch decision every batch dispatcher consults:
//   Auto        -> have_avx2()
//   ForceScalar -> false
//   ForceAvx2   -> have_avx2()   (a request for the vector path, capped by the host)
[[nodiscard]] bool use_avx2() noexcept;

} // namespace atx::vol::simd
