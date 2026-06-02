#pragma once

// atx::core platform — compile-time platform/perf constants and intrinsics.
//
// Provides:
//   kCacheLineSize        — cache-line width in bytes (compile-time constant)
//   ATX_CACHE_ALIGNED     — alignas wrapper for cache-line alignment
//   ATX_RESTRICT          — pointer-aliasing hint (__restrict / empty fallback)
//   atx::core::prefetch() — non-temporal prefetch hint, inline, noexcept
//
// Intentional scope: this header is intentionally minimal. ATX_FORCE_INLINE is
// NOT redefined here; include atx/core/macro.hpp if you need it.

#include "atx/core/types.hpp"

// =====================================================================
//  Compiler / arch detection helpers (translation-unit private)
// =====================================================================
#if defined(_MSC_VER) || defined(__clang__)
#  define ATX_PLATFORM_HAS_RESTRICT 1
#else
#  define ATX_PLATFORM_HAS_RESTRICT 0
#endif

// x86-64: both MSVC and clang-cl expose <xmmintrin.h>; GCC/Clang on x86 too.
#if (defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)) && \
    (defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__))
#  define ATX_PLATFORM_HAS_MM_PREFETCH 1
#  include <xmmintrin.h>
#else
#  define ATX_PLATFORM_HAS_MM_PREFETCH 0
#endif

// __builtin_prefetch: GCC and Clang (non-MSVC) on any arch.
#if (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
#  define ATX_PLATFORM_HAS_BUILTIN_PREFETCH 1
#else
#  define ATX_PLATFORM_HAS_BUILTIN_PREFETCH 0
#endif

namespace atx::core {

// =====================================================================
//  Cache-line size
//
//  x86-64 (Intel/AMD) cache lines are uniformly 64 bytes.
//  This is a compile-time constant, not queried at runtime, because:
//    1. No x86-64 part has shipped with a different L1 cache line since P4.
//    2. Runtime CPUID query is too expensive for a constant used in alignas.
// =====================================================================
inline constexpr usize kCacheLineSize = 64U;

} // namespace atx::core

// =====================================================================
//  ATX_CACHE_ALIGNED
//
//  Usage:   struct ATX_CACHE_ALIGNED MyStruct { ... };
//           ATX_CACHE_ALIGNED int hot_counter;
//
//  Expands to alignas(::atx::core::kCacheLineSize).
// =====================================================================
#define ATX_CACHE_ALIGNED alignas(::atx::core::kCacheLineSize)

// =====================================================================
//  ATX_RESTRICT
//
//  Informs the compiler that the pointer does not alias any other pointer
//  in the current scope, enabling better vectorisation and load/store
//  elimination.
//
//  SAFETY: The caller guarantees no aliasing. Violating this is UB (C
//          standard; compilers rely on it for codegen). Only attach this
//          qualifier when you can prove non-aliasing at the call site.
//
//  Fallback: empty on compilers that don't support __restrict so the
//  code compiles correctly (just without the hint).
// =====================================================================
#if ATX_PLATFORM_HAS_RESTRICT
#  define ATX_RESTRICT __restrict
#else
#  define ATX_RESTRICT /* no restrict on this compiler */
#endif

namespace atx::core {

// =====================================================================
//  prefetch(p)
//
//  Issues a non-temporal / L1-level prefetch hint for the cache line
//  containing *p. This is a performance hint only — it has no observable
//  effect on program semantics, and implementations that ignore it are
//  conforming.
//
//  @param p  Address whose cache line should be brought closer to the CPU.
//            May be nullptr on platforms where the intrinsic treats it as
//            a pure hint (no load performed). See SAFETY note below.
//
//  SAFETY: _mm_prefetch/_MM_HINT_T0 and __builtin_prefetch are both
//          defined by their respective ABIs to be pure hints: no memory
//          access takes place, so passing an unmapped or null address is
//          not undefined behaviour under those ABIs. The (void)p fallback
//          is trivially safe. Callers on other platforms should prefer
//          valid addresses.
// =====================================================================
inline void prefetch(const void* const p) noexcept {
#if ATX_PLATFORM_HAS_MM_PREFETCH
    // SAFETY: _mm_prefetch is a cache-hint intrinsic; the CPU may or may
    //         not issue a prefetch load — no fault is raised on bad addresses
    //         (Intel SDM Vol.2B, PREFETCHT0 description).
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#elif ATX_PLATFORM_HAS_BUILTIN_PREFETCH
    // SAFETY: __builtin_prefetch expands to a prefetch instruction where
    //         available, or a no-op otherwise. GCC/Clang explicitly document
    //         it as hint-only with no memory access (GCC manual §6.62).
    __builtin_prefetch(p, /*rw=*/0, /*locality=*/3);
#else
    // Portable no-op fallback — silence unused-parameter warning.
    (void)p;
#endif
}

} // namespace atx::core
