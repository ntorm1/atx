#include "strategy_mine_pinned_file.hpp"

#include <algorithm>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
// The largest single read (ReadFile counts in a DWORD).
constexpr usize kReadChunk = usize{1} << 30;
} // namespace

PinnedReadFile::~PinnedReadFile() { close(); }

#if defined(_WIN32)
PinnedReadFile::PinnedReadFile(PinnedReadFile &&other) noexcept
    : handle_{std::exchange(other.handle_, nullptr)} {}

PinnedReadFile &PinnedReadFile::operator=(PinnedReadFile &&other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

co::Result<PinnedReadFile> PinnedReadFile::open(const std::filesystem::path &path) {
  // FILE_SHARE_READ only (atx-tsdb Mapping::map_file_ro): no other handle may write or delete.
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return co::Err(co::ErrorCode::IoError,
                   "cannot open " + path.string() + " for reading with writers denied");
  PinnedReadFile out;
  out.handle_ = handle;
  return co::Ok(std::move(out));
}

bool PinnedReadFile::is_open() const noexcept { return handle_ != nullptr; }

co::Result<u64> PinnedReadFile::size() const {
  LARGE_INTEGER bytes{};
  if (handle_ == nullptr || ::GetFileSizeEx(handle_, &bytes) == 0 || bytes.QuadPart < 0)
    return co::Err(co::ErrorCode::IoError, "cannot size a pinned file");
  return co::Ok(static_cast<u64>(bytes.QuadPart));
}

co::Status PinnedReadFile::read_at(u64 offset, std::span<std::byte> out) const {
  usize done = 0;
  // Bounded: each iteration reads at least one byte or fails.
  while (done < out.size()) {
    const u64 at = offset + done;
    OVERLAPPED position{}; // a synchronous handle reads at this offset and waits
    position.Offset = static_cast<DWORD>(at & 0xffffffffULL);
    position.OffsetHigh = static_cast<DWORD>(at >> 32U);
    const DWORD want = static_cast<DWORD>(std::min(out.size() - done, kReadChunk));
    DWORD got = 0;
    if (handle_ == nullptr ||
        ::ReadFile(handle_, out.data() + done, want, &got, &position) == 0 || got == 0)
      return co::Err(co::ErrorCode::IoError, "short read of a pinned file");
    done += got;
  }
  return co::Ok();
}

void PinnedReadFile::close() noexcept {
  if (handle_ != nullptr) {
    static_cast<void>(::CloseHandle(handle_));
    handle_ = nullptr;
  }
}
#else
PinnedReadFile::PinnedReadFile(PinnedReadFile &&other) noexcept
    : fd_{std::exchange(other.fd_, -1)} {}

PinnedReadFile &PinnedReadFile::operator=(PinnedReadFile &&other) noexcept {
  if (this != &other) {
    close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

co::Result<PinnedReadFile> PinnedReadFile::open(const std::filesystem::path &path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return co::Err(co::ErrorCode::IoError, "cannot open " + path.string());
  PinnedReadFile out;
  out.fd_ = fd;
  return co::Ok(std::move(out));
}

bool PinnedReadFile::is_open() const noexcept { return fd_ >= 0; }

co::Result<u64> PinnedReadFile::size() const {
  struct stat st {};
  if (fd_ < 0 || ::fstat(fd_, &st) != 0 || st.st_size < 0)
    return co::Err(co::ErrorCode::IoError, "cannot size a pinned file");
  return co::Ok(static_cast<u64>(st.st_size));
}

co::Status PinnedReadFile::read_at(u64 offset, std::span<std::byte> out) const {
  usize done = 0;
  // Bounded: each iteration reads at least one byte, retries an interrupted read, or fails.
  while (done < out.size()) {
    if (fd_ < 0) return co::Err(co::ErrorCode::IoError, "short read of a pinned file");
    const ssize_t got = ::pread(fd_, out.data() + done, std::min(out.size() - done, kReadChunk),
                                static_cast<off_t>(offset + done));
    if (got <= 0) {
      if (got < 0 && errno == EINTR) continue;
      return co::Err(co::ErrorCode::IoError, "short read of a pinned file");
    }
    done += static_cast<usize>(got);
  }
  return co::Ok();
}

void PinnedReadFile::close() noexcept {
  if (fd_ >= 0) {
    static_cast<void>(::close(fd_));
    fd_ = -1;
  }
}
#endif

} // namespace atx::impl::strategy
