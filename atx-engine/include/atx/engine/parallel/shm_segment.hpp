#pragma once

// atx::engine::parallel::ShmSegment — RAII named, sizable shared-memory segment
// (the cross-platform SHM seam for the multi-process executor, S7-2).
//
// A parent process create()s a NEW named segment of `bytes`, mapped ReadWrite,
// and writes inputs/slots into it. Worker handles (or a second in-process
// handle) open() the same name: ReadOnly maps the region PROT_READ /
// FILE_MAP_READ — one physical region, N viewers, and a worker WRITE FAULTS
// (the OS-enforced zero-copy-input invariant, R5). ReadWrite maps it writable
// (the per-worker output-slot region, R4/R6).
//
// Move-only (owns OS handles); the CREATOR unlinks the OS name on destruction
// (POSIX shm_unlink; Win32 paging-file mappings free when the last handle
// closes). Mirrors the structure of atx::tsdb::Mapping. No OS headers leak into
// this header — the Win32/POSIX split lives entirely in shm_segment.cpp.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::parallel {

/// Requested mapping protection for an opened segment.
/// ReadOnly maps PROT_READ / FILE_MAP_READ (a write faults); ReadWrite maps the
/// region writable.
enum class ShmAccess { ReadOnly, ReadWrite };

class ShmSegment {
public:
  /// Create a NEW named segment of `bytes`, mapped ReadWrite (the parent).
  /// Err(InvalidArgument) if `name` is empty or `bytes == 0`,
  /// Err(AlreadyExists) if a segment with that name already exists,
  /// Err(IoError) on any other OS failure.
  [[nodiscard]] static atx::core::Result<ShmSegment> create(std::string_view name, atx::usize bytes);

  /// Open an EXISTING named segment (a worker, or a second in-process handle).
  /// `access == ReadOnly` maps PROT_READ / FILE_MAP_READ (a write faults);
  /// `access == ReadWrite` maps it writable. Err(InvalidArgument) if `name` is
  /// empty, Err(NotFound) if no such segment exists, Err(IoError) otherwise.
  [[nodiscard]] static atx::core::Result<ShmSegment> open(std::string_view name, ShmAccess access);

  ShmSegment() noexcept = default;
  ~ShmSegment();

  ShmSegment(ShmSegment &&other) noexcept;
  ShmSegment &operator=(ShmSegment &&other) noexcept;
  ShmSegment(const ShmSegment &) = delete;
  ShmSegment &operator=(const ShmSegment &) = delete;

  /// The whole mapped payload as a const byte span (zero-copy input view).
  /// Empty if this segment is in the moved-from / default state.
  [[nodiscard]] std::span<const std::byte> view_ro() const noexcept;

  /// The whole mapped payload as a writable byte span. Empty if the segment was
  /// opened ReadOnly (or is moved-from / default).
  [[nodiscard]] std::span<std::byte> view_rw() noexcept;

  /// Payload size in bytes (0 when moved-from / default).
  [[nodiscard]] atx::usize size() const noexcept { return size_; }

  /// false when opened ReadOnly (or moved-from / default).
  [[nodiscard]] bool writable() const noexcept { return access_ == ShmAccess::ReadWrite; }

  /// Unmap + close handles (and, for the creator, unlink the OS name).
  /// Idempotent; safe on a moved-from / default segment.
  void reset() noexcept;

private:
  // The OS maps `header + payload`; map_base_ is the raw mapping start (the
  // address passed back to UnmapViewOfFile/munmap), base_ points at the payload
  // just past the u64 length header. Keeping both makes unmap correct while
  // view_*() expose only the payload.
  void *map_base_{nullptr};  // raw mapping start (== header), for unmap
  atx::usize map_bytes_{0};  // total mapped length (header + payload), for unmap
  std::byte *base_{nullptr}; // start of the PAYLOAD (after the length header)
  atx::usize size_{0};       // payload size in bytes
  ShmAccess access_{ShmAccess::ReadOnly};
  bool owner_{false}; // true for the creator (unlinks the OS name on reset)
  std::string name_;  // normalized OS name (for shm_unlink by the owner)
#if defined(_WIN32)
  void *mapping_handle_{nullptr}; // HANDLE from CreateFileMappingW/OpenFileMappingW
#else
  int fd_{-1}; // shm_open fd
#endif
};

} // namespace atx::engine::parallel
