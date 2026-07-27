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
// members that bound them, so they follow a field's move automatically; what
// they cannot see is a NEW gap introduced by a new member. `surface_archive.cpp`
// folds `sizeof(EssviParams)`/`sizeof(SviParams)` into `schema_hash()` and pins
// `sizeof(C8Params)` with a static_assert, so any layout change already forces a
// deliberate edit there — revisit this file in the same change.
//
// Private, src/-only header: NOT installed, NOT part of the public atx/vol/ API
// surface. Both translation units that need it (`surface_archive.cpp`,
// `surface_archive_v1.cpp`) live inside the atx-vol library target.

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
