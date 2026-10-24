#pragma once

// Test-only byte surgery on ATXVSA2 revision 1 archives (docs/atxvsa2-format.md).
//
// A forged archive must reach the check under test, not an earlier checksum. Every
// helper here mirrors the writer's checksum rules exactly:
//   slot_crc32c     CRC-32C of the 64-byte slot with that field zeroed       (§2.2)
//   payload_crc32c  CRC-32C of the whole record with that field zeroed        (§3.1)
//                   — copied into the record's lookup slot and directory entry
//   metadata_crc32c CRC-32C of lookup ‖ directory (one contiguous span)       (§2)
//   header_crc32c   CRC-32C of the 128-byte header with that field zeroed     (§2.1)
// Shared by the adversarial, point-read and SurfaceDb admin suites so a format
// change edits ONE restamp implementation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/api/storage/surface_archive.hpp"
#include "storage/archive_util.hpp" // crc32c

namespace atxvsa_patch {

using atx::vol::ArchiveV2DirEntry;
using atx::vol::ArchiveV2Header;
using atx::vol::ArchiveV2LookupSlot;
using atx::vol::ArchiveV2SurfaceHeader;

template <class T> [[nodiscard]] T read_pod(std::span<const std::byte> b, std::uint64_t off) {
  T v{};
  std::memcpy(&v, b.data() + off, sizeof v);
  return v;
}
template <class T> void write_pod(std::span<std::byte> b, std::uint64_t off, const T &v) {
  std::memcpy(b.data() + off, &v, sizeof v);
}

[[nodiscard]] inline ArchiveV2Header read_header(std::span<const std::byte> b) {
  return read_pod<ArchiveV2Header>(b, 0);
}

[[nodiscard]] inline std::uint64_t slot_offset(const ArchiveV2Header &h, std::size_t i) {
  return h.lookup_offset + static_cast<std::uint64_t>(i) * sizeof(ArchiveV2LookupSlot);
}
[[nodiscard]] inline std::uint64_t dir_offset(const ArchiveV2Header &h, std::size_t i) {
  return h.directory_offset + static_cast<std::uint64_t>(i) * sizeof(ArchiveV2DirEntry);
}

[[nodiscard]] inline std::uint32_t slot_crc(ArchiveV2LookupSlot s) {
  s.slot_crc32c = 0;
  std::array<std::byte, sizeof s> raw{};
  std::memcpy(raw.data(), &s, sizeof s);
  return atx::vol::detail::crc32c(raw.data(), raw.size());
}

[[nodiscard]] inline std::uint32_t record_crc(std::span<const std::byte> rec) {
  std::vector<std::byte> copy(rec.begin(), rec.end());
  const std::uint32_t zero = 0;
  std::memcpy(copy.data() + offsetof(ArchiveV2SurfaceHeader, payload_crc32c), &zero, sizeof zero);
  return atx::vol::detail::crc32c(copy.data(), copy.size());
}

[[nodiscard]] inline ArchiveV2LookupSlot read_slot(std::span<const std::byte> b,
                                                   const ArchiveV2Header &h, std::size_t i) {
  return read_pod<ArchiveV2LookupSlot>(b, slot_offset(h, i));
}
// Write `s` verbatim (its slot_crc32c as given) — for forging a bad slot CRC.
inline void write_slot_raw(std::span<std::byte> b, const ArchiveV2Header &h, std::size_t i,
                           const ArchiveV2LookupSlot &s) {
  write_pod(b, slot_offset(h, i), s);
}
// Write `s` with a freshly computed slot_crc32c (the slot itself is self-consistent).
inline void write_slot(std::span<std::byte> b, const ArchiveV2Header &h, std::size_t i,
                       ArchiveV2LookupSlot s) {
  s.slot_crc32c = slot_crc(s);
  write_slot_raw(b, h, i, s);
}

[[nodiscard]] inline ArchiveV2DirEntry read_dir(std::span<const std::byte> b,
                                                const ArchiveV2Header &h, std::size_t i) {
  return read_pod<ArchiveV2DirEntry>(b, dir_offset(h, i));
}
inline void write_dir(std::span<std::byte> b, const ArchiveV2Header &h, std::size_t i,
                      const ArchiveV2DirEntry &e) {
  write_pod(b, dir_offset(h, i), e);
}

// Recompute metadata_crc32c then header_crc32c — the two fields any lookup, directory
// or header edit invalidates.
inline void restamp_header(std::span<std::byte> b) {
  ArchiveV2Header h = read_header(b);
  const std::size_t span_bytes =
      static_cast<std::size_t>(h.lookup_slot_count) * sizeof(ArchiveV2LookupSlot) +
      static_cast<std::size_t>(h.surface_count) * sizeof(ArchiveV2DirEntry);
  h.metadata_crc32c = atx::vol::detail::crc32c(b.data() + h.lookup_offset, span_bytes);
  h.header_crc32c = 0;
  std::array<std::byte, sizeof h> raw{};
  std::memcpy(raw.data(), &h, sizeof h);
  h.header_crc32c = atx::vol::detail::crc32c(raw.data(), raw.size());
  write_pod(b, 0, h);
}

// Index of the OCCUPIED slot naming `canon`, or -1.
[[nodiscard]] inline std::ptrdiff_t slot_index_for(std::span<const std::byte> b,
                                                   std::string_view canon) {
  const ArchiveV2Header h = read_header(b);
  for (std::size_t i = 0; i < h.lookup_slot_count; ++i) {
    const ArchiveV2LookupSlot s = read_slot(b, h, i);
    if (s.flags == atx::vol::kArchiveV2SlotOccupied && s.symbol_len == canon.size() &&
        std::memcmp(s.symbol, canon.data(), canon.size()) == 0) {
      return static_cast<std::ptrdiff_t>(i);
    }
  }
  return -1;
}

// Index of the directory entry naming `canon`, or -1.
[[nodiscard]] inline std::ptrdiff_t dir_index_for(std::span<const std::byte> b,
                                                  std::string_view canon) {
  const ArchiveV2Header h = read_header(b);
  for (std::size_t i = 0; i < h.surface_count; ++i) {
    const ArchiveV2DirEntry e = read_dir(b, h, i);
    if (e.symbol_len == canon.size() && std::memcmp(e.symbol, canon.data(), canon.size()) == 0) {
      return static_cast<std::ptrdiff_t>(i);
    }
  }
  return -1;
}

// Copy the record-at-`record_offset`'s CURRENT payload_crc32c field into its slot and
// directory entry, then restamp the slot, metadata and header CRCs. The record's own
// CRC is not recomputed (use `restamp_record` for that).
inline void propagate_record_crc(std::span<std::byte> b, std::uint64_t record_offset) {
  const ArchiveV2Header h = read_header(b);
  const auto rec_h = read_pod<ArchiveV2SurfaceHeader>(b, record_offset);
  for (std::size_t i = 0; i < h.surface_count; ++i) {
    ArchiveV2DirEntry e = read_dir(b, h, i);
    if (e.surface_offset == record_offset) {
      e.payload_crc32c = rec_h.payload_crc32c;
      write_dir(b, h, i, e);
    }
  }
  for (std::size_t i = 0; i < h.lookup_slot_count; ++i) {
    ArchiveV2LookupSlot s = read_slot(b, h, i);
    if (s.flags == atx::vol::kArchiveV2SlotOccupied && s.surface_offset == record_offset) {
      s.payload_crc32c = rec_h.payload_crc32c;
      write_slot(b, h, i, s);
    }
  }
  restamp_header(b);
}

// Recompute the record's payload_crc32c over its (edited) bytes, store it in the
// record, and propagate it to the index (a fully self-consistent forgery: only the
// SEMANTIC check under test can refuse it).
inline void restamp_record(std::span<std::byte> b, std::uint64_t record_offset) {
  const auto rec_h = read_pod<ArchiveV2SurfaceHeader>(b, record_offset);
  const std::span<const std::byte> rec{b.data() + record_offset,
                                       static_cast<std::size_t>(rec_h.record_size)};
  const std::uint32_t crc = record_crc(rec);
  write_pod(b, record_offset + offsetof(ArchiveV2SurfaceHeader, payload_crc32c), crc);
  propagate_record_crc(b, record_offset);
}

} // namespace atxvsa_patch
