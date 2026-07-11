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

} // namespace atx::vol::simd
