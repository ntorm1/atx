#pragma once

// atx::engine::parallel — determinism digest oracle (S2-1).
//
// signal_set_digest(SignalSet) -> u64 is the BIT-EXACT oracle every later
// parallel unit checks parallel==sequential against. It hashes the SignalSet in
// fixed canonical order — shape (dates, instruments), root count, then each
// alpha (in its stored root order) by NAME bytes and RAW f64 VALUE bytes.
//
// WHY RAW f64 BYTES:
//   The digest hashes the underlying bytes of each f64 via hash_bytes, NOT a
//   normalized numeric form. Consequences (all INTENTIONAL — this is a bit-
//   IDENTITY oracle, not a numeric-equality check):
//     * a 1-ULP change flips the digest (the whole point — catches a result that
//       drifted by a single bit under a different worker count or build);
//     * +0.0 and -0.0 produce DIFFERENT digests (distinct bit patterns);
//     * two NaNs with different payload/sign bits produce DIFFERENT digests.
//   Determinism caveat: hash_bytes is deterministic only WITHIN a process (its
//   wyhash seeds are compile-time constants and endianness is not normalized).
//   That is exactly the contract these checks need — same process, same run,
//   parallel path vs sequential path must agree bit-for-bit.
//
// CANONICAL ORDER is load-bearing: alphas are hashed in their stored vector
// order (which is canonical root order per the Program), and within each alpha
// name precedes values. Swapping two roots, renaming a root, reshaping with the
// same value count, or perturbing one value all flip the digest.
//
// result_table_digest: added in S2-3 (needs FoldResult).
//
// Header-only; the single free function is inline.

#include "atx/core/hash.hpp"  // atx::core::hash_bytes / hash_combine
#include "atx/core/types.hpp" // atx::u64 / usize / f64

#include "atx/engine/alpha/panel.hpp"  // alpha::SignalSet
#include "atx/engine/parallel/fwd.hpp" // parallel-layer fwd decls (consistency)

namespace atx::engine::parallel {

// Canonical-order wyhash digest of a SignalSet. Equal iff the two SignalSets are
// BIT-IDENTICAL in shape, root order, names, and every value's raw f64 bits.
// See the file header for the raw-byte / signed-zero / NaN-payload semantics.
[[nodiscard]] inline atx::u64 signal_set_digest(const atx::engine::alpha::SignalSet& ss) noexcept {
  // Seed with the shape and the root count so a reshape (same value count,
  // different dates*instruments split) or a differing alpha count flips it.
  std::size_t seed = atx::core::hash_combine(std::size_t{0}, ss.dates, ss.instruments,
                                             ss.alphas.size());

  // Fold each alpha in fixed (canonical) root order: name bytes, then raw value
  // bytes. Hashing the name and value digests via hash_combine is order-
  // sensitive, so (name, values) per root and root-to-root order both matter.
  for (const atx::engine::alpha::SignalSet::Alpha& alpha : ss.alphas) {
    const atx::u64 name_digest = atx::core::hash_bytes(alpha.name.data(), alpha.name.size());
    const atx::u64 values_digest =
        atx::core::hash_bytes(alpha.values.data(), alpha.values.size() * sizeof(atx::f64));
    seed = atx::core::hash_combine(seed, name_digest, values_digest);
  }

  return static_cast<atx::u64>(seed);
}

} // namespace atx::engine::parallel
