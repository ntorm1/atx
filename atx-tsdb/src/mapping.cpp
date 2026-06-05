#include "atx/tsdb/mapping.hpp"

#include <string>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner) — windows.h is the Win32 umbrella; the
// individual sub-headers (memoryapi.h, stringapiset.h, fileapi.h, …) are NOT
// self-contained on all Windows SDK versions, so we must include the umbrella.
// All Win32 symbol warnings in the block below are suppressed for this reason.
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace atx::tsdb {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;

Mapping::~Mapping() { reset(); }

Mapping::Mapping(Mapping &&other) noexcept
    : base_{other.base_}, size_{other.size_}
#if defined(_WIN32)
      ,
      file_handle_{other.file_handle_}, mapping_handle_{other.mapping_handle_}
#else
      ,
      fd_{other.fd_}
#endif
{
  other.base_ = nullptr;
  other.size_ = 0;
#if defined(_WIN32)
  other.file_handle_ = nullptr;
  other.mapping_handle_ = nullptr;
#else
  other.fd_ = -1;
#endif
}

Mapping &Mapping::operator=(Mapping &&other) noexcept {
  if (this != &other) {
    reset();
    base_ = other.base_;
    size_ = other.size_;
#if defined(_WIN32)
    file_handle_ = other.file_handle_;
    mapping_handle_ = other.mapping_handle_;
    other.file_handle_ = nullptr;
    other.mapping_handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.base_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

#if defined(_WIN32)

void Mapping::reset() noexcept {
  if (base_ != nullptr) {
    // SAFETY: base_ was obtained from MapViewOfFile; const_cast back to
    // non-const void* for the API is safe — we never mutate through it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    UnmapViewOfFile(const_cast<atx::u8 *>(base_));
    base_ = nullptr;
  }
  if (mapping_handle_ != nullptr) {
    CloseHandle(mapping_handle_);
    mapping_handle_ = nullptr;
  }
  if (file_handle_ != nullptr) {
    CloseHandle(file_handle_);
    file_handle_ = nullptr;
  }
  size_ = 0;
}

Result<Mapping> Mapping::map_file_ro(const std::string &path) {
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (wlen <= 0) {
    return Err(ErrorCode::InvalidArgument, "path is not valid UTF-8: " + path);
  }
  // wlen counts the NUL terminator; size the string to wlen-1 visible chars.
  // std::wstring still allocates room for and guarantees a NUL at data()[size()],
  // so the conversion below can safely write the terminator.
  std::wstring wpath(static_cast<std::size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

  HANDLE file = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Err(ErrorCode::IoError, "CreateFileW failed: " + path);
  }
  LARGE_INTEGER fsize{};
  if (GetFileSizeEx(file, &fsize) == 0 || fsize.QuadPart == 0) {
    CloseHandle(file);
    return Err(ErrorCode::InvalidArgument, "empty or unsizable file: " + path);
  }
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping == nullptr) {
    CloseHandle(file);
    return Err(ErrorCode::IoError, "CreateFileMappingW failed: " + path);
  }
  void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (view == nullptr) {
    CloseHandle(mapping);
    CloseHandle(file);
    return Err(ErrorCode::IoError, "MapViewOfFile failed: " + path);
  }
  Mapping m;
  // SAFETY: view is the address returned by MapViewOfFile; treating it as a
  // read-only byte array via u8* is correct. reinterpret_cast is required here.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  m.base_ = reinterpret_cast<const atx::u8 *>(view);
  // SAFETY: fsize.QuadPart > 0 (checked above); fits in usize on 64-bit targets.
  m.size_ = static_cast<atx::usize>(fsize.QuadPart);
  m.file_handle_ = file;
  m.mapping_handle_ = mapping;
  return m;
}

void Mapping::prefetch() const noexcept {
  if (base_ == nullptr) {
    return;
  }
  WIN32_MEMORY_RANGE_ENTRY range{};
  // SAFETY: PrefetchVirtualMemory requires non-const VirtualAddress; the
  // mapping is read-only to us but the API predates const correctness.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  range.VirtualAddress = const_cast<atx::u8 *>(base_);
  range.NumberOfBytes = size_;
  // Best-effort: ignore return value — failure doesn't affect correctness.
  static_cast<void>(PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0));
}

// NOLINTEND(misc-include-cleaner)

#else // POSIX

void Mapping::reset() noexcept {
  if (base_ != nullptr) {
    // SAFETY: munmap requires a non-const pointer; base_ was obtained from mmap.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    ::munmap(const_cast<atx::u8 *>(base_), size_);
    base_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  size_ = 0;
}

Result<Mapping> Mapping::map_file_ro(const std::string &path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Err(ErrorCode::IoError, "open failed: " + path);
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size == 0) {
    ::close(fd);
    return Err(ErrorCode::InvalidArgument, "empty or unstatable file: " + path);
  }
  const auto bytes = static_cast<atx::usize>(st.st_size);
  void *addr = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
    ::close(fd);
    return Err(ErrorCode::IoError, "mmap failed: " + path);
  }
  Mapping m;
  // SAFETY: addr is the mapped region returned by mmap; reinterpret to u8* is safe.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  m.base_ = reinterpret_cast<const atx::u8 *>(addr);
  m.size_ = bytes;
  m.fd_ = fd;
  return m;
}

void Mapping::prefetch() const noexcept {
  if (base_ != nullptr) {
    // SAFETY: madvise takes non-const; base_ is our own mapped region.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    ::madvise(const_cast<atx::u8 *>(base_), size_, MADV_WILLNEED);
  }
}

#endif

} // namespace atx::tsdb
