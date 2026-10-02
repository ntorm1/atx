#pragma once
// atx::impl::strategy — a pinned payload held open for reading with writers denied (lane
// MINE-JOIN; the mining verb's pool members, strategy_mine_pool.hpp).
//
// On Windows the handle is CreateFileW(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING), the share
// mode atx-tsdb opens its read-only inputs with (Mapping::map_file_ro, atx-tsdb/src/mapping.cpp).
// While the handle is open, any other open that asks to write, append, truncate, rename or
// delete the file fails with a sharing violation; and the open itself fails while another handle
// may write or delete it. So bytes verified through the handle once are the bytes every later
// read returns, for as long as it stays open (kPinnedReadDeniesWriters). What the share check
// cannot see -- raw volume writes, a kernel filter -- is outside this guarantee.
//
// POSIX has no such share mode (flock is advisory): there kPinnedReadDeniesWriters is false and
// a caller re-verifies what it reads. Reads are positional (no shared file position), so a const
// handle serves every pass. Move-only RAII, as TrialRegistry's LogFile (trial_registry.cpp).
#include <cstddef>
#include <filesystem>
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {

#if defined(_WIN32)
inline constexpr bool kPinnedReadDeniesWriters = true;
#else
inline constexpr bool kPinnedReadDeniesWriters = false;
#endif

class PinnedReadFile {
public:
  // Err(IoError) when the file cannot be opened for reading with writers denied: absent,
  // unreadable, or (Windows) another handle may write or delete it.
  [[nodiscard]] static atx::core::Result<PinnedReadFile> open(const std::filesystem::path &path);

  PinnedReadFile() noexcept = default;
  ~PinnedReadFile();
  PinnedReadFile(PinnedReadFile &&other) noexcept;
  PinnedReadFile &operator=(PinnedReadFile &&other) noexcept;
  PinnedReadFile(const PinnedReadFile &) = delete;
  PinnedReadFile &operator=(const PinnedReadFile &) = delete;

  [[nodiscard]] bool is_open() const noexcept;
  // The file's extent in bytes now.
  [[nodiscard]] atx::core::Result<atx::u64> size() const;
  // Exactly out.size() bytes from byte `offset`; Err(IoError) on a short read or a closed file.
  [[nodiscard]] atx::core::Status read_at(atx::u64 offset, std::span<std::byte> out) const;

private:
  void close() noexcept;
#if defined(_WIN32)
  void *handle_{nullptr}; // HANDLE from CreateFileW; null when closed
#else
  int fd_{-1};
#endif
};

} // namespace atx::impl::strategy
