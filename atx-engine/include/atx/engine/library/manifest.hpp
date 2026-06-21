#pragma once

// atx::engine::library — LibraryManifest: content-addressed versioned snapshot (S4-5).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The deterministic, content-ADDRESSED snapshot of an entire library's catalog:
//  an alpha_id-ordered list of ManifestEntry rows (one per admitted alpha) plus
//  the master seeds, content-addressed by a crc32 over a FIXED-byte-layout
//  serialization of (entries ++ seeds) => `version_id`.
//
//  The version_id is the load-bearing INVARIANT (S4-5 dominant-risk #4 — "a
//  snapshot that is not byte-identical fails manifest integrity silently"):
//    * Same library content + same seeds  => SAME version_id   (determinism, L7).
//    * One more alpha (or any field change) => DIFFERENT version_id (the address
//      genuinely depends on the content, not a vacuous constant).
//
//  Because the version_id folds a STABLE, byte-explicit, endian-independent
//  crc32 (atx::tsdb::crc32 — the same primitive the segment footer uses) over a
//  FIXED field layout serialized in alpha_id order, two independent builds of the
//  same fixture produce the same address on any platform/process.
//
// ===========================================================================
//  ManifestEntry.segment_crc — the per-segment integrity crc
// ===========================================================================
//  Each entry carries the integrity_crc of the SEALED SEGMENT that holds its
//  alpha — i.e. SegmentFooter::integrity_crc (the crc32 over [header..footer),
//  exposed by SegmentReaderLite::integrity_crc()). This is the SAME stable
//  per-segment witness LibraryStore writes into the catalog and the S4-1
//  determinism test (TwoBuildsByteIdentical) compares. It is chosen over the
//  header content_hash because the footer integrity_crc covers the header too
//  (a strictly stronger, whole-segment digest) and is the catalog's crc column.
//  Every alpha in one segment shares that segment's integrity_crc.
//
// ===========================================================================
//  Persistence — a sidecar file
// ===========================================================================
//  write_manifest / read_manifest serialize the manifest to a small binary
//  sidecar (`<magic><version_id><n_seeds><seeds...><n_entries><entries...>`),
//  fixed-layout and little-endian (the put_le/get_le primitives from record.hpp),
//  so a written manifest reads back field-for-field with no loss. read_manifest
//  RE-DERIVES the version_id from the round-tripped (entries ++ seeds) and checks
//  it equals the stored version_id (an integrity check on the sidecar).
//
//  Threading: a value type; the (de)serialization helpers are pure functions.

#include <algorithm> // std::min (bound speculative reserve against bytes remaining)
#include <fstream>   // sidecar read/write
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp" // Result, Status, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // u8, u32, u64, usize, byte

#include "atx/tsdb/checksum.hpp" // tsdb::crc32 (stable content-address)
#include "atx/tsdb/segment.hpp"  // tsdb::tag8 (sidecar magic)

#include "atx/engine/library/record.hpp" // detail::put_le / get_le (fixed-layout LE)

namespace atx::engine::library {

// ===========================================================================
//  Magic / version for the manifest SIDECAR file framing.
//  ATXMANI2: bumped from ATXMANI1 to add the cumulative_trials u64 field (R1).
//  An old ATXMANI1 sidecar is cleanly rejected ("bad magic") rather than
//  silently mis-parsed as a truncated or corrupt ATXMANI2 file.
// ===========================================================================
inline constexpr atx::u64 kManifestMagic = atx::tsdb::tag8("ATXMANI2");

// ===========================================================================
//  ManifestEntry — one alpha's content-address row (POD value).
//
//  alpha_id              global AlphaId (entries are emitted in ascending order).
//  canon_hash            the S3 cross-run-stable dedup key for this alpha.
//  lifecycle_at_snapshot the LifecycleState (0..5) as of the snapshot's `now`.
//  segment_crc           the integrity_crc of the segment holding this alpha.
// ===========================================================================
struct ManifestEntry {
  atx::u64 alpha_id;
  atx::u64 canon_hash;
  atx::u8 lifecycle_at_snapshot;
  atx::u32 segment_crc;
};

// The ON-DISK width of one serialized ManifestEntry: the pinned field-by-field
// layout put_entry() writes (alpha_id u64 + canon_hash u64 + lifecycle u8 +
// segment_crc u32), with NO struct padding folded in. Used to bound the speculative
// read_manifest reserve against the bytes actually remaining (NOT sizeof(ManifestEntry),
// which would include compiler padding the wire format does not have).
inline constexpr atx::usize kManifestEntryWireBytes =
    sizeof(atx::u64) + sizeof(atx::u64) + sizeof(atx::u8) + sizeof(atx::u32);

// ===========================================================================
//  LibraryManifest — the versioned, content-addressed snapshot.
// ===========================================================================
struct LibraryManifest {
  atx::u64 version_id{0};               // crc32 over a fixed-layout (entries ++ seeds ++ cumulative_trials)
  std::vector<atx::u64> master_seeds;   // the SRP / search seeds (index rebuilds identically)
  std::vector<ManifestEntry> entries;   // ordered by alpha_id (L7)
  atx::u64 cumulative_trials{0};        // R1: cross-run cumulative trial count for DSR deflation
};

namespace detail {

// Append one entry's fields to `out` in a PINNED little-endian byte order. The
// order/width is part of the content-address contract: alpha_id (u64), canon_hash
// (u64), lifecycle (u8), segment_crc (u32). No struct padding is folded (we serialize
// field-by-field, so the digest is identical regardless of compiler layout).
inline void put_entry(std::vector<std::byte> &out, const ManifestEntry &e) {
  put_le<atx::u64>(out, e.alpha_id);
  put_le<atx::u64>(out, e.canon_hash);
  put_le<atx::u8>(out, e.lifecycle_at_snapshot);
  put_le<atx::u32>(out, e.segment_crc);
}

// The canonical content buffer the version_id is the crc32 of: n_entries, then
// every entry in alpha_id order, then n_seeds, then every seed, then
// cumulative_trials (u64, appended last — append-only extension, R1). Counts are
// folded so a {1 entry, 0 seeds} library can never alias a {0 entries, 1 seed}
// library.
[[nodiscard]] inline std::vector<std::byte>
serialize_for_address(std::span<const ManifestEntry> entries,
                      std::span<const atx::u64> seeds,
                      atx::u64 cumulative_trials) {
  std::vector<std::byte> buf;
  put_le<atx::u64>(buf, static_cast<atx::u64>(entries.size()));
  for (const ManifestEntry &e : entries) {
    put_entry(buf, e);
  }
  put_le<atx::u64>(buf, static_cast<atx::u64>(seeds.size()));
  for (const atx::u64 s : seeds) {
    put_le<atx::u64>(buf, s);
  }
  // R1: fold cumulative_trials into the content address (append-only, last field).
  put_le<atx::u64>(buf, cumulative_trials);
  return buf;
}

} // namespace detail

// ===========================================================================
//  compute_version_id — the deterministic content-address.
//
//  crc32 (zero-extended to u64) over the fixed-layout (n_entries, entries...,
//  n_seeds, seeds..., cumulative_trials) buffer. PRECONDITION (caller's): `entries`
//  are in ascending alpha_id order (the snapshot builder guarantees this).
//  R1: cumulative_trials is folded in as the last field so the address changes when
//  the cross-run trial counter changes (a fresh/single-run library has it == 0,
//  which is byte-identical to the old layout with the 8-byte zero appended).
// ===========================================================================
[[nodiscard]] inline atx::u64 compute_version_id(std::span<const ManifestEntry> entries,
                                                 std::span<const atx::u64> seeds,
                                                 atx::u64 cumulative_trials) {
  const std::vector<std::byte> buf =
      detail::serialize_for_address(entries, seeds, cumulative_trials);
  return static_cast<atx::u64>(atx::tsdb::crc32(buf.data(), buf.size()));
}

// Recompute + assign `m.version_id` from its current entries, seeds, and
// cumulative_trials. Returns the computed id. (The snapshot builder calls this
// once entries are populated — R1: also after cumulative_trials is set.)
inline atx::u64 finalize_version_id(LibraryManifest &m) {
  m.version_id = compute_version_id(m.entries, m.master_seeds, m.cumulative_trials);
  return m.version_id;
}

// ===========================================================================
//  Sidecar (de)serialization — write_manifest / read_manifest.
// ===========================================================================

/// Serialize `m` to a binary sidecar at `path` (truncate-create). Layout (ATXMANI2):
///   u64 magic | u64 version_id | u64 n_seeds | n_seeds*u64 |
///   u64 n_entries | n_entries * {u64 alpha_id, u64 canon_hash, u8 life, u32 crc} |
///   u64 cumulative_trials.
/// R1: cumulative_trials appended last (append-only extension).
/// Err(IoError) on a write fault.
[[nodiscard]] inline atx::core::Status write_manifest(const LibraryManifest &m,
                                                      const std::string &path) {
  std::vector<std::byte> buf;
  detail::put_le<atx::u64>(buf, kManifestMagic);
  detail::put_le<atx::u64>(buf, m.version_id);
  detail::put_le<atx::u64>(buf, static_cast<atx::u64>(m.master_seeds.size()));
  for (const atx::u64 s : m.master_seeds) {
    detail::put_le<atx::u64>(buf, s);
  }
  detail::put_le<atx::u64>(buf, static_cast<atx::u64>(m.entries.size()));
  for (const ManifestEntry &e : m.entries) {
    detail::put_entry(buf, e);
  }
  // R1: cumulative_trials is the last field (append-only).
  detail::put_le<atx::u64>(buf, m.cumulative_trials);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          "LibraryManifest: cannot open sidecar for write");
  }
  out.write(reinterpret_cast<const char *>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
  if (!out) {
    return atx::core::Err(atx::core::ErrorCode::IoError, "LibraryManifest: sidecar write failed");
  }
  return atx::core::Ok();
}

/// Read a manifest sidecar from `path`. Validates the magic, decodes every field
/// in the pinned layout, and RE-DERIVES the version_id from the round-tripped
/// (entries ++ seeds) to confirm it matches the stored version_id (an integrity
/// check). Err(IoError) if unreadable; Err(InvalidArgument) on bad magic / a
/// short/truncated buffer; Err(Internal) on a version_id mismatch.
[[nodiscard]] inline atx::core::Result<LibraryManifest> read_manifest(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return atx::core::Err(atx::core::ErrorCode::IoError, "LibraryManifest: cannot open sidecar");
  }
  const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
  const std::span<const std::byte> src{reinterpret_cast<const std::byte *>(raw.data()),
                                       raw.size()};
  atx::usize off = 0;
  atx::u64 magic = 0;
  if (!detail::get_le(src, off, magic) || magic != kManifestMagic) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "LibraryManifest: bad magic");
  }
  LibraryManifest m;
  atx::u64 stored_version = 0;
  atx::u64 n_seeds = 0;
  if (!detail::get_le(src, off, stored_version) || !detail::get_le(src, off, n_seeds)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "LibraryManifest: truncated header");
  }
  // Bound the speculative reserve against the bytes ACTUALLY remaining: a garbage
  // huge n_seeds from a corrupt sidecar must NOT throw length_error/bad_alloc before
  // the per-element get_le bounds check can return the intended Err(InvalidArgument).
  // The loop below still rejects a truncated buffer; this only caps the allocation.
  {
    const atx::usize rem = src.size() - off;
    m.master_seeds.reserve(
        std::min<atx::usize>(static_cast<atx::usize>(n_seeds), rem / sizeof(atx::u64)));
  }
  for (atx::u64 i = 0; i < n_seeds; ++i) {
    atx::u64 s = 0;
    if (!detail::get_le(src, off, s)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "LibraryManifest: truncated seeds");
    }
    m.master_seeds.push_back(s);
  }
  atx::u64 n_entries = 0;
  if (!detail::get_le(src, off, n_entries)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "LibraryManifest: truncated entry count");
  }
  // Same bound for entries: cap the reserve at (bytes remaining / on-disk entry
  // width) so a corrupt n_entries cannot trigger a huge speculative allocation
  // before the per-element get_le loop rejects the truncated buffer.
  {
    const atx::usize rem = src.size() - off;
    m.entries.reserve(
        std::min<atx::usize>(static_cast<atx::usize>(n_entries), rem / kManifestEntryWireBytes));
  }
  for (atx::u64 i = 0; i < n_entries; ++i) {
    ManifestEntry e{};
    if (!detail::get_le(src, off, e.alpha_id) || !detail::get_le(src, off, e.canon_hash) ||
        !detail::get_le(src, off, e.lifecycle_at_snapshot) ||
        !detail::get_le(src, off, e.segment_crc)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "LibraryManifest: truncated entry");
    }
    m.entries.push_back(e);
  }
  // R1: decode cumulative_trials (last field, ATXMANI2).
  atx::u64 cumulative_trials = 0;
  if (!detail::get_le(src, off, cumulative_trials)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "LibraryManifest: truncated cumulative_trials");
  }
  m.cumulative_trials = cumulative_trials;
  m.version_id = stored_version;
  // Integrity: the re-derived address MUST match the stored one.
  if (compute_version_id(m.entries, m.master_seeds, m.cumulative_trials) != stored_version) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "LibraryManifest: version_id mismatch (corrupt sidecar)");
  }
  return atx::core::Result<LibraryManifest>{std::move(m)};
}

} // namespace atx::engine::library
