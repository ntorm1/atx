#include "atx/vol/simd/cpu.hpp"

#include "atx/vol/simd/math_mode.hpp"

#include <atomic>
#include <cstddef> // std::size_t
#include <cstdlib> // std::getenv / getenv_s (ATX_SIMD_ISA leg)
#include <cstring> // std::strcmp

#if defined(_M_X64) || defined(__x86_64__)
#  include <intrin.h> // __cpuid, __cpuidex, _xgetbv (clang-cl / MSVC)
#endif

namespace atx::vol::simd {

namespace {

[[nodiscard]] bool detect_avx2_fma() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7) {
        return false; // CPUID leaf 7 not available
    }

    __cpuidex(info, 1, 0);
    const bool fma = (info[2] & (1 << 12)) != 0;     // ECX.FMA
    const bool osxsave = (info[2] & (1 << 27)) != 0; // ECX.OSXSAVE
    const bool avx = (info[2] & (1 << 28)) != 0;     // ECX.AVX
    if (!(fma && osxsave && avx)) {
        return false;
    }

    // OSXSAVE set ⇒ XGETBV is legal. Require the OS to preserve XMM (bit 1) and
    // YMM (bit 2) state across context switches, else AVX instructions #UD/#GP.
    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6ULL) != 0x6ULL) {
        return false;
    }

    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0; // EBX.AVX2
#else
    return false;
#endif
}

} // namespace

bool have_avx2() noexcept {
    // Magic-static: CPUID runs exactly once; subsequent calls read the cache.
    static const bool kHas = detect_avx2_fma();
    return kHas;
}

namespace {

// PR-C2 non-AVX2 test leg: seed the process-global ISA override from the
// ATX_SIMD_ISA environment variable at load time. This is the knob a CI/test leg
// sets to run the WHOLE process on one ISA — chiefly ForceScalar, to keep the
// scalar patch-paths (the degenerate/tail/non-finite lanes the AVX2 kernels fall
// back through, and the ForceScalar reproducibility contract) honest against the
// Auto-ON AVX2 marks. Unset / unrecognized => Auto (unchanged behavior). An
// explicit programmatic set_simd_isa_override() (e.g. the SIMD tests that pin a
// path in-process) still wins over this default.
[[nodiscard]] int initial_isa_from_env() noexcept {
#if defined(_WIN32)
    std::size_t sz = 0;
    char buf[16] = {};
    if (getenv_s(&sz, buf, sizeof(buf), "ATX_SIMD_ISA") != 0 || sz == 0) {
        return static_cast<int>(SimdIsa::Auto);
    }
    const char* e = buf;
#else
    const char* e = std::getenv("ATX_SIMD_ISA");
    if (e == nullptr) {
        return static_cast<int>(SimdIsa::Auto);
    }
#endif
    if (std::strcmp(e, "ForceScalar") == 0) {
        return static_cast<int>(SimdIsa::ForceScalar);
    }
    if (std::strcmp(e, "ForceAvx2") == 0) {
        return static_cast<int>(SimdIsa::ForceAvx2);
    }
    return static_cast<int>(SimdIsa::Auto);
}

// Process-global ISA override. Relaxed atomic: coarse set-once control, never on
// the per-lane hot path. Stored as the underlying int so the load is trivially
// lock-free on every target. Initialized from ATX_SIMD_ISA (above) at load.
std::atomic<int> g_isa_override{initial_isa_from_env()};

} // namespace

void set_simd_isa_override(SimdIsa isa) noexcept {
    g_isa_override.store(static_cast<int>(isa), std::memory_order_relaxed);
}

SimdIsa simd_isa_override() noexcept {
    return static_cast<SimdIsa>(g_isa_override.load(std::memory_order_relaxed));
}

bool use_avx2() noexcept {
    // FIX-5/I2: the decision itself lives in the header as a pure function so the
    // non-AVX2-host arm is testable on this (AVX2) host.
    return resolve_use_avx2(simd_isa_override(), have_avx2());
}

// ── P3.3 math modes (a name over the existing ISA seam; no new dispatch) ──────

const char* math_mode_name(MathMode m) noexcept {
    switch (m) {
        case MathMode::Reference:
            return "Reference";
        case MathMode::FastDeterministic:
            return "FastDeterministic";
    }
    return "Reference";
}

SimdIsa isa_for_math_mode(MathMode m) noexcept {
    switch (m) {
        case MathMode::Reference:
            return SimdIsa::ForceScalar;
        case MathMode::FastDeterministic:
            // The only vector ISA is AVX2; without it FastDeterministic degrades to
            // the scalar libm ordering (still deterministic for this host).
            return have_avx2() ? SimdIsa::ForceAvx2 : SimdIsa::ForceScalar;
    }
    return SimdIsa::ForceScalar;
}

void set_math_mode(MathMode m) noexcept {
    set_simd_isa_override(isa_for_math_mode(m));
}

MathMode active_math_mode() noexcept {
    switch (simd_isa_override()) {
        case SimdIsa::ForceScalar:
            return MathMode::Reference;
        case SimdIsa::ForceAvx2:
            return MathMode::FastDeterministic;
        case SimdIsa::Auto:
            break;
    }
    return have_avx2() ? MathMode::FastDeterministic : MathMode::Reference;
}

} // namespace atx::vol::simd
