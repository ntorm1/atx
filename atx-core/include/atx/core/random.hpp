#pragma once

// atx::core random — deterministic PRNG for reproducible backtests.
//
// Provides:
//   class Xoshiro256pp — xoshiro256++ generator (Blackman & Vigna, 2019).
//
// Design notes:
//   - ALL state is held inside the object; NO global state.
//   - Two instances seeded with the same value produce identical sequences.
//     This is a contract: do not break it.
//   - Seeds the 256-bit state from a single u64 via SplitMix64 expansion
//     (standard practice; avoids bad zero-state for seed==0).
//   - jump() advances the state by 2^128 steps using the published polynomial,
//     enabling non-overlapping parallel streams from copies of a seeded instance.
//   - Value semantics: copy/assign copies all state; sequences diverge after
//     copy only if jump() is called on one copy.
//
// References:
//   xoshiro256++ paper/code: https://prng.di.unimi.it/xoshiro256plusplus.c
//   SplitMix64: https://prng.di.unimi.it/splitmix64.c
//   Box-Muller transform: Box & Muller (1958), "A Note on the Generation of
//     Random Normal Deviates", Ann. Math. Statist. 29(2):610-611.
//
// Not thread-safe: each thread should own its own instance (seeded with
// different seeds or produced via jump() from a shared seeded instance).

#include <array>
#include <bit>
#include <cmath>

#include "atx/core/types.hpp"

namespace atx::core {

// ---- internal helpers -------------------------------------------------------

namespace detail {

/// Rotate-left x by k bits.
///
/// Delegates to C++20 std::rotl which is defined for all k values. Using
/// std::rotl avoids any shift-count UB and enables the compiler to emit a
/// single rotate instruction.
[[nodiscard]] constexpr u64 rotl64(u64 x, int k) noexcept {
    return std::rotl(x, k);
}

/// One step of the SplitMix64 PRNG (David Stafford's Mix13 finalizer).
///
/// Used exclusively to expand a single 64-bit seed into the 256-bit xoshiro
/// state. Advances `state` in place and returns the mixed output.
///
/// Constant 0x9E3779B97F4A7C15 is the 64-bit Weyl-sequence step (fractional
/// part of the golden ratio * 2^64). The two mixing constants are Stafford's
/// strongest MurmurHash3-style avalanche constants for 64-bit state.
[[nodiscard]] constexpr u64 splitmix64_next(u64& state) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    state += u64{0x9E3779B97F4A7C15ULL};
    u64 z = state;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    z = (z ^ (z >> 30U)) * u64{0xBF58476D1CE4E5B9ULL};
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    z = (z ^ (z >> 27U)) * u64{0x94D049BB133111EBULL};
    return z ^ (z >> 31U);
}

} // namespace detail

// ---- Xoshiro256pp -----------------------------------------------------------

/// Deterministic PRNG using the xoshiro256++ algorithm (Blackman & Vigna 2019).
///
/// Contract:
///   - Two instances constructed with the same seed produce identical sequences.
///   - All output depends solely on the 256-bit internal state (4 × u64).
///   - No global state, no thread-local state, no I/O side effects.
///   - After jump(), two copies will produce non-overlapping sequences
///     (guaranteed for up to 2^128 values before any overlap).
///
/// Thread-safety: NOT safe for concurrent access. Give each thread its own
/// instance (or produce independent streams via jump()).
class Xoshiro256pp {
public:
    // ---- construction -------------------------------------------------------

    /// Construct from a single 64-bit seed.
    ///
    /// Seeds the 256-bit state by running four steps of SplitMix64 from the
    /// given seed. This ensures the state is non-zero and well-distributed
    /// even for small or zero seeds.
    ///
    /// @param seed  Any u64 value; two instances with the same seed produce the
    ///              same sequence.
    explicit Xoshiro256pp(u64 seed) noexcept {
        // SplitMix64 expansion: four sequential steps from the same seed fill
        // all four 64-bit state words. This is the canonical seeding technique
        // for xoshiro/xoroshiro generators.
        state_[0] = detail::splitmix64_next(seed);
        state_[1] = detail::splitmix64_next(seed);
        state_[2] = detail::splitmix64_next(seed);
        state_[3] = detail::splitmix64_next(seed);
    }

    // Defaulted copy/move give value semantics: state is copied verbatim so
    // two instances with the same state produce the same subsequent sequence.
    Xoshiro256pp(const Xoshiro256pp&) noexcept = default;
    Xoshiro256pp& operator=(const Xoshiro256pp&) noexcept = default;
    Xoshiro256pp(Xoshiro256pp&&) noexcept = default;
    Xoshiro256pp& operator=(Xoshiro256pp&&) noexcept = default;
    ~Xoshiro256pp() = default;

    // ---- core generator -----------------------------------------------------

    /// Advance the state and return the next 64-bit output.
    ///
    /// Algorithm: xoshiro256++ (Blackman & Vigna 2019).
    /// Output function: rotl(s[0] + s[3], 23) + s[0]
    /// State update: linear feedback shift register using XOR and rotl.
    ///
    /// Period: 2^256 − 1. Passes all known statistical tests (PractRand, etc.).
    ///
    /// @return  A pseudo-random u64; the state is advanced by one step.
    [[nodiscard]] u64 next_u64() noexcept {
        // Verbatim xoshiro256++ reference step (public domain):
        //   uint64_t result = rotl(s[0] + s[3], 23) + s[0];
        //   uint64_t t = s[1] << 17;
        //   s[2] ^= s[0];  s[3] ^= s[1];
        //   s[1] ^= s[2];  s[0] ^= s[3];
        //   s[2] ^= t;
        //   s[3] = rotl(s[3], 45);
        //   return result;
        const u64 result = detail::rotl64(state_[0] + state_[3], 23) + state_[0];
        const u64 t = state_[1] << 17U;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = detail::rotl64(state_[3], 45);
        return result;
    }

    // ---- derived distributions ----------------------------------------------

    /// Return a uniform f64 in [0, 1).
    ///
    /// Takes the top 53 bits of the 64-bit output and divides by 2^53. This
    /// matches IEEE 754 double mantissa width exactly, producing every
    /// representable value in [0, 1) with equal probability.
    ///
    /// @return  A f64 in [0.0, 1.0).
    [[nodiscard]] f64 uniform01() noexcept {
        // Shift right by 11 to obtain the top 53 bits, then scale to [0,1).
        // SAFETY: 2^53 = 9007199254740992.0 is exactly representable as f64;
        // all 53-bit integers divide cleanly by it without rounding above 1.
        constexpr f64 k_scale = 1.0 / 9007199254740992.0; // 1 / 2^53
        return static_cast<f64>(next_u64() >> 11U) * k_scale;
    }

    /// Return a uniform f64 in [lo, hi).
    ///
    /// @pre   lo < hi and both are finite. Results are unspecified for NaN or
    ///        infinite bounds (no runtime check in release builds).
    /// @return  A f64 uniformly distributed in [lo, hi).
    [[nodiscard]] f64 uniform(f64 lo, f64 hi) noexcept {
        return lo + (hi - lo) * uniform01();
    }

    /// Return a standard normal variate (mean 0, standard deviation 1).
    ///
    /// Box-Muller transform. Each pair of uniforms yields two independent
    /// standard normals; the second is cached and returned on the next call,
    /// halving the number of std::sqrt/log/sin/cos calls.
    ///
    /// @note The cached spare is part of the object state and is copied by the
    ///       copy/move constructor. A copy taken while a spare is pending will
    ///       return that same spare on its first normal() call before drawing
    ///       any fresh uniforms. This is intentional and correct for
    ///       reproducibility: a copy continues the identical normal() sequence
    ///       as the original.
    ///
    /// @return  A f64 drawn from N(0, 1).
    [[nodiscard]] f64 normal() noexcept {
        if (has_spare_) {
            has_spare_ = false;
            return spare_;
        }

        // Box-Muller requires u1 > 0 to evaluate log(u1).
        // P(u1 == 0) ≈ 2^{-53} but we loop defensively. The loop is bounded
        // in practice: the generator has period 2^256 − 1 and never visits
        // the all-zero state, so it will produce a non-zero value within a
        // handful of iterations with overwhelming probability.
        f64 u1{};
        do {
            u1 = uniform01();
        } while (u1 == 0.0);
        const f64 u2 = uniform01();

        // z0 = sqrt(-2 ln u1) * cos(2π u2)
        // z1 = sqrt(-2 ln u1) * sin(2π u2)
        constexpr f64 k_two_pi = 6.283185307179586476925286766559; // 2π
        const f64 mag = std::sqrt(-2.0 * std::log(u1));
        const f64 angle = k_two_pi * u2;

        spare_ = mag * std::sin(angle);
        has_spare_ = true;

        return mag * std::cos(angle);
    }

    /// Return a Bernoulli sample: true with probability p.
    ///
    /// @param p  Probability of returning true.
    ///           p <= 0.0 → always false; p >= 1.0 → always true.
    /// @note     A NaN p always yields false: every IEEE-754 comparison with
    ///           NaN (including `x < NaN`) is false, so `uniform01() < p` is
    ///           false regardless of the drawn value.
    /// @return   true with probability p.
    [[nodiscard]] bool bernoulli(f64 p) noexcept {
        return uniform01() < p;
    }

    // ---- parallel streams ---------------------------------------------------

    /// Advance the state by 2^128 steps in O(1).
    ///
    /// Equivalent to calling next_u64() 2^128 times, but computed via the
    /// published xoshiro256++ jump polynomial. Use this to carve out
    /// non-overlapping sub-streams: seed one instance, copy it N times, then
    /// call jump() once per copy. Each copy produces 2^128 unique values
    /// before any overlap with its neighbours.
    ///
    /// Jump polynomial constants: canonical xoshiro256++ JUMP[] from
    /// https://prng.di.unimi.it/xoshiro256plusplus.c (public domain).
    void jump() noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
        constexpr std::array<u64, 4> k_jump = {
            0x180EC6D33CFD0ABAULL,
            0xD5A61266F0C9392CULL,
            0xA9582618E03FC9AAULL,
            0x39ABDC4529B1661CULL,
        };

        u64 s0 = 0ULL;
        u64 s1 = 0ULL;
        u64 s2 = 0ULL;
        u64 s3 = 0ULL;

        for (const u64 jump_word : k_jump) {
            for (int bit = 0; bit < 64; ++bit) {
                if ((jump_word & (u64{1ULL} << static_cast<unsigned>(bit))) != 0ULL) {
                    s0 ^= state_[0];
                    s1 ^= state_[1];
                    s2 ^= state_[2];
                    s3 ^= state_[3];
                }
                static_cast<void>(next_u64());
            }
        }

        state_[0] = s0;
        state_[1] = s1;
        state_[2] = s2;
        state_[3] = s3;
    }

private:
    // 256-bit generator state (4 × 64-bit words).
    std::array<u64, 4> state_{};

    // Box-Muller cached second variate.
    f64  spare_{};
    bool has_spare_{false};
};

} // namespace atx::core
