#pragma once

// slice_payload_padding — canonicalize the PADDING of the fixed-layout POD slice
// payloads (`EssviParams`, `SviParams`, `C8Params`) that both surface-archive
// writers blit into a record verbatim.
//
// THE DEFECT THIS CLOSES. Those structs are serialized as their OBJECT
// representation, which is WIDER than the bytes their members occupy:
// `SviParams` ends with a `std::uint16_t expiry_id` followed by 6 bytes of tail
// padding, `EssviParams` has a gap before `resid_coef` and a tail gap, and
// `C8Params` has a gap between `expiry_id` and `v` plus a tail gap. NOTHING ever
// writes those pad bytes: a fitter builds its result as `SviParams out{};` and
// assigns members, and clang-cl does not materialize the zero-initialization of
// padding bits that `{}` nominally implies. The pad therefore holds whatever the
// producing thread's stack last left at that address.
//
// A blind `std::memcpy(payload, &params, sizeof params)` copies that stack
// residue into the record — and thus into `payload_crc32c`, into the directory
// entry that mirrors it, and into `metadata_crc32c`, i.e. into the archive's
// CONTENT IDENTITY. The consequence is that the SAME fitted slice stored
// DIFFERENT bytes depending on which thread fitted it, hence on the fit worker
// count (observed end to end by
// `SurfaceDbPopulate.CarryOverIsByteIdenticalAcrossWorkerCounts`, which fits the
// same board under 1 and 8 workers and byte-compares the stored records).
//
// THE CONTRACT. A record's bytes are a pure function of the surface's VALUES.
// After the blit, the writer calls the matching `normalize_*_payload_padding`
// below, which re-zeroes exactly the gaps BETWEEN members. No value byte is
// touched, and the on-disk layout is unchanged — every archive written before
// this fix still reads back identically, because readers `memcpy` the whole
// struct back and never look at the pad.
//
// MAINTENANCE. The gap lists are expressed with `offsetof`/`sizeof` on the
// members that bound them, so a field that MOVES is followed automatically. What
// they cannot see is a member ADDED or REMOVED — and that case is dangerous in
// exactly one direction: a small member appended into existing tail slack lands
// INSIDE a zeroed range. `std::uint8_t new_flag` appended to `SviParams` sits at
// offset 66 and leaves `sizeof` at 72; appended to `C8Params` it sits at 169 and
// leaves `sizeof` at 176. The writer would then zero that member in every record
// and compute `payload_crc32c` AFTER zeroing, so the loss is self-consistent and
// invisible on read-back: the field reads 0 forever, with no diagnostic.
//
// Neither of the pre-existing layout guards catches that. `schema_hash()`'s
// `sizeof(EssviParams)`/`sizeof(SviParams)` fold and
// `static_assert(sizeof(C8Params) == 176)` only fire on a SIZE change, and they
// only make older readers reject newer files — they never force anyone back to
// this gap list. (Even a size-CHANGING addition is silently clobbered: append a
// `double` to `SviParams` and `sizeof` goes 72 -> 80, after which the tail range
// `[66, sizeof)` eats it.)
//
// The guard that does hold is the structured-binding ARITY TRIPWIRE below: one
// per struct, naming every member exactly once. Adding or removing ANY member is
// then a hard compile error in this header, which is the prompt to revisit the
// gap list in the same change. Known residual hole: a pure REORDER (same member
// count, same size) still compiles, and `zero_payload_gap` no-ops rather than
// failing when a gap list is made wrong, so the nondeterminism would return
// silently. Reordering an on-disk POD is already a format break that the
// `offsetof` static_asserts in `surface_archive.hpp` police; the tripwire is not
// a substitute for reading this file when you touch these structs.
//
// Private, src/-only header: NOT installed, NOT part of the public atx/vol/ API
// surface. The one translation unit that needs it (`surface_archive.cpp`) lives
// inside the atx-vol library target.

#include <cstddef>
#include <cstring>
#include <type_traits>

#include "atx/vol/c8.hpp"          // C8Params
#include "atx/vol/vol_surface.hpp" // EssviParams, SviParams

namespace atx::vol::detail {

// `offsetof` is only well-defined on standard-layout types.
static_assert(std::is_standard_layout_v<EssviParams>,
              "EssviParams must be standard-layout for offsetof-based padding normalization");
static_assert(std::is_standard_layout_v<SviParams>,
              "SviParams must be standard-layout for offsetof-based padding normalization");
static_assert(std::is_standard_layout_v<C8Params>,
              "C8Params must be standard-layout for offsetof-based padding normalization");

// ── Layout tripwires ────────────────────────────────────────────────────────
//
// A structured binding must name EVERY non-static data member, exactly once, in
// declaration order. So these three functions are compile-time arity assertions:
// add or remove a member of any of the three structs and the matching binding
// stops compiling, right here, next to the gap list that has to be revisited.
// They are never called — the bodies are checked because the functions are
// defined, not because anyone uses them. See MAINTENANCE above for the failure
// they exist to prevent.

inline void svi_layout_tripwire(const SviParams &s) noexcept {
  [[maybe_unused]] const auto &[a, b, rho, m, sigma, T, F, expiry_ns, expiry_id] = s;
}

inline void essvi_layout_tripwire(const EssviParams &s) noexcept {
  [[maybe_unused]] const auto &[theta, phi, rho, rho_R, rho_scale, psi, p, lambda, lambda_R, T, F,
                                expiry_ns, expiry_id, resid_coef, resid_scale, resid_basis_kind,
                                resid_n_basis] = s;
}

inline void c8_layout_tripwire(const C8Params &s) noexcept {
  [[maybe_unused]] const auto &[T, F, expiry_ns, expiry_id, v, psi, p, c, v_min, kappa, q_L, q_R,
                                h_atm, k_L, h_L, k_R, h_R, arb_damping_factor, rmse_price, rmse_vol,
                                n_lm_iters, n_irls_iters, bumps_active] = s;
}

// Zero the byte range `[from, to)` of the payload at `p`. `to <= from` (a layout
// with no gap there) is a no-op, so the call sites need no per-platform guards.
inline void zero_payload_gap(std::byte *p, std::size_t from, std::size_t to) noexcept {
  if (to > from) {
    std::memset(p + from, 0, to - from);
  }
}

// Tail gap after `expiry_id`.
inline void normalize_svi_payload_padding(std::byte *p) noexcept {
  zero_payload_gap(p, offsetof(SviParams, expiry_id) + sizeof(SviParams::expiry_id),
                   sizeof(SviParams));
}

// Gap between `expiry_id` and the 8-aligned `resid_coef`, plus the tail gap
// after `resid_n_basis`.
inline void normalize_essvi_payload_padding(std::byte *p) noexcept {
  zero_payload_gap(p, offsetof(EssviParams, expiry_id) + sizeof(EssviParams::expiry_id),
                   offsetof(EssviParams, resid_coef));
  zero_payload_gap(p, offsetof(EssviParams, resid_n_basis) + sizeof(EssviParams::resid_n_basis),
                   sizeof(EssviParams));
}

// Gap between `expiry_id` and the 8-aligned `v`, plus the tail gap after
// `bumps_active`.
inline void normalize_c8_payload_padding(std::byte *p) noexcept {
  zero_payload_gap(p, offsetof(C8Params, expiry_id) + sizeof(C8Params::expiry_id),
                   offsetof(C8Params, v));
  zero_payload_gap(p, offsetof(C8Params, bumps_active) + sizeof(C8Params::bumps_active),
                   sizeof(C8Params));
}

} // namespace atx::vol::detail
