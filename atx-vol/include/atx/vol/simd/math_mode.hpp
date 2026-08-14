#pragma once

// P3.3 math mode: the EXPLICIT, documented accuracy contract for the transcendental
// implementation the atx-vol SIMD batch kernels evaluate. Two modes only:
//
//   Reference          — the scalar libm ordering (std::erfc / std::log / std::exp,
//                        i.e. atx::core::norm_cdf). Bit-for-bit the cold scalar path;
//                        the numerical source of truth every fast path is graded on.
//   FastDeterministic  — ONE fixed vector approximation per ISA: the hand-tuned AVX2
//                        transcendentals in detail/vector_math.hpp (Cody-Waite log/exp
//                        + a full-range Cody rational-erfc Φ, accurate across the whole
//                        real line, no wing patch). Deterministic for a given ISA (no
//                        /fp:fast, no reassociation knobs), with a STATED, TESTED bound
//                        vs Reference (below).
//
// This is a *formalization* of the seam T13 already shipped, NOT a new dispatch: the
// process-global SimdIsa override (cpu.hpp) is exactly the knob that chooses scalar
// libm vs the AVX2 vector_math.hpp path, so a MathMode is just its accuracy-facing
// name. `set_math_mode` installs the corresponding SimdIsa override; the American
// boundary batch (the T13 kernel that consults use_avx2()) honors it directly, and
// the bound test / bakeoff bench drive the vector transcendentals through it.
//
// BAKEOFF OUTCOME: vector_math.hpp is the
// chosen FastDeterministic implementation. SLEEF was DECLINED — it documents parity
// with SVML (not a speedup over libm), the 4-lane win is already captured by
// vector_math.hpp, and the one kernel that could benefit (the boundary solve) is
// capped by its scalar per-lane BAW seed, not by Φ. Vendoring it would add a build
// dependency + a -fno-math-errno TU for no measured throughput gain.

#include "atx/vol/simd/cpu.hpp" // SimdIsa, have_avx2()

namespace atx::vol::simd {

// Which transcendental implementation a batch kernel runs.
enum class MathMode {
    Reference,         // scalar libm — bit-for-bit
    FastDeterministic, // AVX2 vector_math.hpp — bounded vs Reference (below)
};

// ── Measured FastDeterministic bounds vs Reference (asserted by the gate
//    VectorMath_FastDeterministic_BoundedVsReference; see the T14 report) ──────
//
// These are the CONTRACT the FastDeterministic mode promises and the tests enforce.

// Max |Φ_fast(x) − Φ_ref(x)| across the FULL real line. The Cody rational-erfc Φ is
// full double precision everywhere — there is no wing exemption: the vector Cody erfc
// and the scalar libm erfc (atx::core::norm_cdf) differ by only a few ULP of Φ.
// MEASURED ≈1.7e-15 over [-40,40] (vs the retired 48-term Chebyshev-Φ's ≈3e-11 interior
// error, which additionally needed a scalar wing patch). Asserted with headroom.
inline constexpr double kFastDeterministicPhiBound = 1e-14;

// Max relative error of log_pd/exp_pd vs std::log/std::exp on positive normals /
// the exp domain — the Cody-Waite kernels are a few ULP, comfortably inside this.
inline constexpr double kFastDeterministicLogExpRelBound = 1e-13;

// Max |price_fast − price_ref| for the American boundary batch on the NORMAL grid
// (the FastDeterministic Φ/log/exp compounded through the iterative sweep). Matches
// the T13 kNormalGate immateriality threshold; measured ≈6.4e-7 there.
inline constexpr double kFastDeterministicPriceBound = 1e-6;

// Human-readable mode name (logging / bench labels).
[[nodiscard]] const char* math_mode_name(MathMode m) noexcept;

// Map a MathMode onto the EXISTING T13 ISA override (does NOT rebuild dispatch):
//   Reference         -> SimdIsa::ForceScalar (scalar libm)
//   FastDeterministic -> SimdIsa::ForceAvx2 when have_avx2(), else ForceScalar —
//                        i.e. on a non-AVX2 host FastDeterministic *is* Reference
//                        (the deterministic degrade; there is no other vector ISA).
[[nodiscard]] SimdIsa isa_for_math_mode(MathMode m) noexcept;

// Install the ISA override that realizes `m`. Coarse, set-once (a relaxed atomic
// store, same as set_simd_isa_override); intended for tests / a startup flag.
void set_math_mode(MathMode m) noexcept;

// The mode the current SimdIsa override resolves to on this host:
//   ForceScalar -> Reference
//   ForceAvx2   -> FastDeterministic
//   Auto        -> have_avx2() ? FastDeterministic : Reference
[[nodiscard]] MathMode active_math_mode() noexcept;

} // namespace atx::vol::simd
