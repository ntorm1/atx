#pragma once

// atx::tsdb::Mapping — RAII read-only file mapping (the cross-platform seam).
//
// map_file_ro(path) maps an existing file PROT_READ / FILE_MAP_READ and shares
// its pages via the OS page cache: every process that maps the same file sees
// one physical copy. Move-only (owns OS handles). Read-only by construction, so
// a buggy reader cannot corrupt the dataset for sibling processes.

#include <string>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::tsdb {

class Mapping {
public:
  /// Map `path` read-only. Err(IoError) if the file cannot be opened/mapped,
  /// Err(InvalidArgument) if it is empty (an empty mapping is never valid here).
  [[nodiscard]] static atx::core::Result<Mapping> map_file_ro(const std::string &path);

  Mapping() noexcept = default;
  ~Mapping();

  Mapping(Mapping &&other) noexcept;
  Mapping &operator=(Mapping &&other) noexcept;
  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;

  [[nodiscard]] const atx::u8 *base() const noexcept { return base_; }
  [[nodiscard]] atx::usize size() const noexcept { return size_; }

  /// Hint the OS to make the mapping resident (madvise WILLNEED /
  /// PrefetchVirtualMemory). Best-effort; never fails the caller.
  void prefetch() const noexcept;

private:
  void reset() noexcept; // unmap + close handles; idempotent

  const atx::u8 *base_{nullptr};
  atx::usize size_{0};
#if defined(_WIN32)
  void *file_handle_{nullptr};    // HANDLE from CreateFileW
  void *mapping_handle_{nullptr}; // HANDLE from CreateFileMappingW
#else
  int fd_{-1};
#endif
};

} // namespace atx::tsdb
