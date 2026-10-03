#pragma once

// atx::engine — the arithmetic-relevant build flavour of one translation unit (P9 S1, DS-1).
//
//   A cache keyed on computed bits (the IC runner's candidate-signal and IC-result caches, the
//   marginal verb's pair cache) must never serve one build flavour's entries to another: Release
//   compiles out ATX_ASSERT (NDEBUG), links another CRT whose log / exp / pow / tanh need not
//   agree bit for bit with the debug CRT's, optimises differently, and an xsimd upgrade can change
//   a kernel's reduction order. Before P9 the identities carried none of these, so a Debug and a
//   Release tree shared one cache directory (DS review §3, "cache-key completeness").
//
//   ATX_ENGINE_BUILD_FLAVOR is evaluated by the preprocessor of the translation unit that expands
//   it. The TU whose arithmetic keys a cache therefore exposes its own flavour through a function
//   defined there (factory::ic_screen_build_flavor, combine::marginal_rank_ic_build_flavor): a
//   flavour read in another TU can differ (the Debug tree gives a few TUs /O2, and a target may
//   carry other ISA flags).
//
//   fp_flavor_suffix     "_fma", "_avx2", "_fastmath": the code-generation switches the IC caches
//                        have always keyed, in that order.
//   build_flavor_token   "<opt>_<crt>_<assert>_xs<M>.<m>.<p>", e.g. "opt_md_ndebug_xs13.0.0" for
//                        the equity-rel tree. <opt>: noopt | opt | os (__OPTIMIZE__,
//                        __OPTIMIZE_SIZE__: the TU's share of the build type); <crt>: mdd | md |
//                        mtd | mt (MSVC runtime: _DLL, _DEBUG) or posix; <assert>: assert | ndebug.
//   build_flavor_suffix  "" for the legacy flavour, else "_" + the token. Legacy = assertions on,
//                        debug dynamic CRT, xsimd 13.0.0: the equity-dev Debug flavour that wrote
//                        every pre-P9 cache entry, so its identities, cache directories and
//                        summaries keep their bytes. The legacy test ignores <opt> on purpose:
//                        that tree compiles the IC TUs /O2 and the rest /Od, and strict FP without
//                        an FMA ISA rounds the same at either level (fp_flavor_suffix keys FMA).

#include <string>      // std::string, std::to_string
#include <string_view> // std::string_view

#include <xsimd/config/xsimd_config.hpp> // XSIMD_VERSION_MAJOR / _MINOR / _PATCH (macros only)

namespace atx::engine {

struct BuildFlavor {
  bool fma = false;                // __FMA__
  bool avx2 = false;               // __AVX2__
  bool fast_math = false;          // __FAST_MATH__ (banned repo-wide; keyed all the same)
  bool optimized = false;          // __OPTIMIZE__: compiled at -O1 or above
  bool optimized_for_size = false; // __OPTIMIZE_SIZE__
  bool ndebug = false;             // NDEBUG: assert and ATX_ASSERT compiled out
  std::string_view crt{};          // "mdd" | "md" | "mtd" | "mt" | "posix" (a string literal)
  int xsimd_major = 0;
  int xsimd_minor = 0;
  int xsimd_patch = 0;
};

[[nodiscard]] inline std::string fp_flavor_suffix(const BuildFlavor &flavor) {
  std::string out;
  if (flavor.fma) {
    out += "_fma";
  }
  if (flavor.avx2) {
    out += "_avx2";
  }
  if (flavor.fast_math) {
    out += "_fastmath";
  }
  return out;
}

[[nodiscard]] inline std::string build_flavor_token(const BuildFlavor &flavor) {
  const char *const opt =
      flavor.optimized_for_size ? "os" : (flavor.optimized ? "opt" : "noopt");
  return std::string(opt) + "_" + std::string(flavor.crt) + "_" +
         (flavor.ndebug ? "ndebug" : "assert") + "_xs" + std::to_string(flavor.xsimd_major) + "." +
         std::to_string(flavor.xsimd_minor) + "." + std::to_string(flavor.xsimd_patch);
}

[[nodiscard]] constexpr bool legacy_build_flavor(const BuildFlavor &flavor) noexcept {
  return !flavor.ndebug && flavor.crt == std::string_view("mdd") && flavor.xsimd_major == 13 &&
         flavor.xsimd_minor == 0 && flavor.xsimd_patch == 0;
}

[[nodiscard]] inline std::string build_flavor_suffix(const BuildFlavor &flavor) {
  return legacy_build_flavor(flavor) ? std::string{} : "_" + build_flavor_token(flavor);
}

} // namespace atx::engine

#if defined(__FMA__)
#define ATX_ENGINE_BF_FMA true
#else
#define ATX_ENGINE_BF_FMA false
#endif
#if defined(__AVX2__)
#define ATX_ENGINE_BF_AVX2 true
#else
#define ATX_ENGINE_BF_AVX2 false
#endif
#if defined(__FAST_MATH__)
#define ATX_ENGINE_BF_FAST_MATH true
#else
#define ATX_ENGINE_BF_FAST_MATH false
#endif
#if defined(__OPTIMIZE__)
#define ATX_ENGINE_BF_OPTIMIZED true
#else
#define ATX_ENGINE_BF_OPTIMIZED false
#endif
#if defined(__OPTIMIZE_SIZE__)
#define ATX_ENGINE_BF_OPTIMIZED_FOR_SIZE true
#else
#define ATX_ENGINE_BF_OPTIMIZED_FOR_SIZE false
#endif
#if defined(NDEBUG)
#define ATX_ENGINE_BF_NDEBUG true
#else
#define ATX_ENGINE_BF_NDEBUG false
#endif
// clang-cl /MDd defines _DEBUG, _MT and _DLL; /MD _MT and _DLL; /MTd _DEBUG and _MT.
#if defined(_MSC_VER) && defined(_DLL) && defined(_DEBUG)
#define ATX_ENGINE_BF_CRT "mdd"
#elif defined(_MSC_VER) && defined(_DLL)
#define ATX_ENGINE_BF_CRT "md"
#elif defined(_MSC_VER) && defined(_DEBUG)
#define ATX_ENGINE_BF_CRT "mtd"
#elif defined(_MSC_VER)
#define ATX_ENGINE_BF_CRT "mt"
#else
#define ATX_ENGINE_BF_CRT "posix"
#endif

// The expanding translation unit's flavour (see the file header).
#define ATX_ENGINE_BUILD_FLAVOR                                                                \
  ::atx::engine::BuildFlavor {                                                                 \
    .fma = ATX_ENGINE_BF_FMA, .avx2 = ATX_ENGINE_BF_AVX2,                                      \
    .fast_math = ATX_ENGINE_BF_FAST_MATH, .optimized = ATX_ENGINE_BF_OPTIMIZED,                \
    .optimized_for_size = ATX_ENGINE_BF_OPTIMIZED_FOR_SIZE, .ndebug = ATX_ENGINE_BF_NDEBUG,    \
    .crt = ::std::string_view(ATX_ENGINE_BF_CRT), .xsimd_major = XSIMD_VERSION_MAJOR,          \
    .xsimd_minor = XSIMD_VERSION_MINOR, .xsimd_patch = XSIMD_VERSION_PATCH                     \
  }
