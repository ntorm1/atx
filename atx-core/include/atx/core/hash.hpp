#pragma once

// atx::core hash — fast, composable hashing helpers built on wyhash.
//
// Provides:
//   hash_bytes(data, len)         — raw byte digest via wyhash
//   hash_combine(seed, xs...)     — fold one or more values into a seed using
//                                   a 64-bit golden-ratio mixer
//
// hash_bytes contract:
//   - data may be nullptr iff len == 0 (wyhash internally branches on len==0
//     before touching the pointer).
//   - Returns the same u64 for identical (data, len) pairs across calls within
//     one process — NOT stable across process restarts or platforms (wyhash
//     seeds are compile-time constants; endianness is not normalised by design).
//   - noexcept: no dynamic allocation; pure arithmetic.
//
// hash_combine contract:
//   - Order-sensitive: hash_combine(0, a, b) != hash_combine(0, b, a)
//     for most (a, b) pairs.
//   - Left-associative fold: hash_combine(seed, a, b, c) is equivalent to
//     hash_combine(hash_combine(hash_combine(seed, a), b), c).
//   - noexcept: delegates to std::hash<T> specialisations; well-defined for
//     all standard scalar types and any user type providing a noexcept
//     std::hash specialisation.
//
// SAFETY: hash_bytes uses ankerl::unordered_dense::detail::wyhash::hash —
//   a detail namespace not part of the library's public API surface. It is
//   used here because: (1) the public ankerl::unordered_dense::hash<T>
//   specialisation for string-like types delegates to this exact symbol; (2)
//   we need a raw (void*, size_t) overload without constructing a temporary
//   std::string/string_view; (3) the implementation is stable across the
//   pinned version (v4.4.x). If the library is upgraded, verify the symbol
//   still exists before bumping the pin. The deviation is isolated to this
//   file; callers see only the clean atx::core::hash_bytes interface.

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <functional>

#include "atx/core/types.hpp"

namespace atx::core {

// ---- hash_bytes -------------------------------------------------------------

/// Hash a raw byte buffer using wyhash.
///
/// @param data  Pointer to the first byte. May be nullptr iff len == 0.
/// @param len   Number of bytes to hash.
/// @return      A u64 digest. Deterministic within a process; not stable
///              across process restarts or platforms.
///
/// @note noexcept — pure arithmetic, no allocation.
[[nodiscard]] inline u64 hash_bytes(const void* data, usize len) noexcept {
    // SAFETY: ankerl::unordered_dense::detail::wyhash is a private namespace.
    // See file-level comment for the full rationale and upgrade protocol.
    // The function signature is:
    //   [[nodiscard]] inline auto hash(void const* key, size_t len) -> uint64_t
    // It handles len==0 (returns mix(secret[1]^0, mix(0^secret[1], 0^seed)))
    // without dereferencing `key`, so nullptr is safe when len==0.
    return ankerl::unordered_dense::detail::wyhash::hash(data, len);
}

// ---- hash_combine -----------------------------------------------------------

namespace detail {

/// Mix a single value into a running hash seed using a 64-bit golden-ratio
/// constant (Boost-style). The shift amounts break symmetry and make the
/// operation non-commutative.
///
/// The constant 0x9e3779b97f4a7c15 is the 64-bit fractional part of the
/// golden ratio φ = (√5−1)/2, used as a Weyl sequence step. It has excellent
/// avalanche properties and is widely used in hash combiners (Boost, Abseil,
/// xxHash, …).
///
/// @param seed  Running state; updated in place.
/// @param val   Hash of a single element (from std::hash or compatible).
inline void mix_one(std::size_t& seed, std::size_t val) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    seed ^= val + std::size_t{0x9e3779b97f4a7c15ULL} + (seed << 6U) + (seed >> 2U);
}

} // namespace detail

/// Concept: T is hashable via std::hash<T>.
template <class T>
concept Hashable = requires(const T& t) {
    { std::hash<T>{}(t) } -> std::convertible_to<std::size_t>;
};

/// Combine zero or more values into a seed using a golden-ratio mixer.
///
/// Called with zero extra arguments, returns `seed` unchanged.
/// Called with one or more arguments, folds each via std::hash<T> and the
/// mix_one helper — left to right, order-sensitive.
///
/// @param seed  Starting seed value (typically 0 or a previous combine result).
/// @param xs    Values to fold in (each must satisfy Hashable).
/// @return      Updated seed after mixing all values.
///
/// @note noexcept — conditional on std::hash<Ts> being noexcept.
template <Hashable... Ts>
[[nodiscard]] std::size_t hash_combine(std::size_t seed, const Ts&... xs) noexcept {
    // Fold left over parameter pack: each element is hashed then mixed in.
    // The comma-expression inside the initialiser-list ensures left-to-right
    // evaluation order (guaranteed by [dcl.init.list] for braced-init-lists).
    (detail::mix_one(seed, std::hash<Ts>{}(xs)), ...);
    return seed;
}

} // namespace atx::core
