#include "atx/engine/parallel/shm_segment.hpp"

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <cerrno>
#endif

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner) — windows.h is the Win32 umbrella; the
// individual sub-headers (memoryapi.h, stringapiset.h, winbase.h, …) are NOT
// self-contained on all Windows SDK versions, so we must include the umbrella.
// All Win32 symbol warnings in the block below are suppressed for this reason.
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace atx::engine::parallel {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;

namespace {

// The mapping carries an internal length header so an opener can recover the
// payload size on every backend without an out-of-band size argument (Win32
// does not expose the size to OpenFileMapping callers; POSIX could fstat but we
// keep ONE uniform codepath). The payload begins immediately after the header.
constexpr atx::usize kHeaderBytes = sizeof(atx::u64);

// SECURITY: the shared-memory name lives in a cooperative OS namespace — any
// local process can pre-create or squat a name. We do NOT defend against a
// malicious peer choosing the same name; we DO refuse to silently rewrite a
// caller's name into a different OS object (which would alias two distinct
// logical segments onto one, causing spurious AlreadyExists or wrong-segment
// opens). Validation is therefore strict-reject, not normalize-and-hope.
//
// POSIX shm names must start with '/' and contain no other '/' and fit a
// portable length bound (NAME_MAX is 255 on most systems; we leave headroom for
// the leading '/'). The ONLY normalization we perform is prepending the leading
// '/'; an interior '/' or an over-bound name is rejected with InvalidArgument so
// the OS object is an unambiguous 1:1 image of the caller's name.
constexpr std::size_t kMaxNameLen = 250; // includes the leading '/'

[[nodiscard]] Result<std::string> normalize_name(std::string_view name) {
  if (name.find('/') != std::string_view::npos) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment: name must not contain '/'");
  }
  if (name.size() + 1U > kMaxNameLen) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment: name too long");
  }
  std::string out;
  out.reserve(name.size() + 1U);
  out.push_back('/');
  out.append(name);
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Platform-independent members (no OS calls): move ops, views, accessors.
// ---------------------------------------------------------------------------

ShmSegment::~ShmSegment() { reset(); }

ShmSegment::ShmSegment(ShmSegment &&other) noexcept
    : map_base_{other.map_base_}, map_bytes_{other.map_bytes_}, base_{other.base_},
      size_{other.size_}, access_{other.access_}, owner_{other.owner_}, name_{std::move(other.name_)}
#if defined(_WIN32)
      ,
      mapping_handle_{other.mapping_handle_}
#else
      ,
      fd_{other.fd_}
#endif
{
  other.map_base_ = nullptr;
  other.map_bytes_ = 0;
  other.base_ = nullptr;
  other.size_ = 0;
  other.access_ = ShmAccess::ReadOnly;
  other.owner_ = false;
  other.name_.clear();
#if defined(_WIN32)
  other.mapping_handle_ = nullptr;
#else
  other.fd_ = -1;
#endif
}

ShmSegment &ShmSegment::operator=(ShmSegment &&other) noexcept {
  if (this != &other) {
    reset();
    map_base_ = other.map_base_;
    map_bytes_ = other.map_bytes_;
    base_ = other.base_;
    size_ = other.size_;
    access_ = other.access_;
    owner_ = other.owner_;
    name_ = std::move(other.name_);
#if defined(_WIN32)
    mapping_handle_ = other.mapping_handle_;
    other.mapping_handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.map_base_ = nullptr;
    other.map_bytes_ = 0;
    other.base_ = nullptr;
    other.size_ = 0;
    other.access_ = ShmAccess::ReadOnly;
    other.owner_ = false;
    other.name_.clear();
  }
  return *this;
}

std::span<const std::byte> ShmSegment::view_ro() const noexcept {
  if (base_ == nullptr) {
    return {};
  }
  return {base_, size_};
}

std::span<std::byte> ShmSegment::view_rw() noexcept {
  if (base_ == nullptr || access_ != ShmAccess::ReadWrite) {
    return {};
  }
  return {base_, size_};
}

// ---------------------------------------------------------------------------
// Win32 backend
// ---------------------------------------------------------------------------
#if defined(_WIN32)

void ShmSegment::reset() noexcept {
  if (map_base_ != nullptr) {
    UnmapViewOfFile(map_base_);
    map_base_ = nullptr;
  }
  if (mapping_handle_ != nullptr) {
    CloseHandle(mapping_handle_);
    mapping_handle_ = nullptr;
  }
  // Win32 paging-file-backed mappings free automatically when the last handle
  // closes — there is no explicit unlink (owner_ carries no extra work here).
  map_bytes_ = 0;
  base_ = nullptr;
  size_ = 0;
  access_ = ShmAccess::ReadOnly;
  owner_ = false;
  name_.clear();
}

namespace {

// UTF-8 -> UTF-16 (the mapping.cpp pattern). Empty result signals bad UTF-8.
[[nodiscard]] std::wstring widen(const std::string &utf8) {
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (wlen <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), wlen);
  return wide;
}

} // namespace

Result<ShmSegment> ShmSegment::create(std::string_view name, atx::usize bytes) {
  if (name.empty()) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::create: empty name");
  }
  if (bytes == 0) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::create: zero bytes");
  }
  if (bytes > (static_cast<atx::usize>(-1) - kHeaderBytes)) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::create: bytes + header overflows usize");
  }
  ATX_TRY(const std::string norm, normalize_name(name));
  const std::wstring wname = widen(norm);
  if (wname.empty()) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::create: name not valid UTF-8");
  }
  const atx::u64 total = static_cast<atx::u64>(bytes) + kHeaderBytes;
  const DWORD hi = static_cast<DWORD>(total >> 32U);
  const DWORD lo = static_cast<DWORD>(total & 0xFFFFFFFFU);

  HANDLE handle =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, wname.c_str());
  if (handle == nullptr) {
    return Err(ErrorCode::IoError, "ShmSegment::create: CreateFileMappingW failed");
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(handle);
    return Err(ErrorCode::AlreadyExists, "ShmSegment::create: name already exists");
  }
  // Least privilege: FILE_MAP_WRITE implies read; FILE_MAP_ALL_ACCESS would add
  // execute-class rights a data segment never needs.
  void *view = MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, 0);
  if (view == nullptr) {
    CloseHandle(handle);
    return Err(ErrorCode::IoError, "ShmSegment::create: MapViewOfFile failed");
  }
  // SAFETY: view is the address returned by MapViewOfFile; the first
  // sizeof(u64) bytes are our length header. Writing the size lets openers
  // recover it. reinterpret_cast from void* to the byte cursor is required.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto *raw = reinterpret_cast<std::byte *>(view);
  const auto stored = static_cast<atx::u64>(bytes);
  std::memcpy(raw, &stored, sizeof(stored));

  ShmSegment seg;
  seg.map_base_ = view;
  seg.map_bytes_ = static_cast<atx::usize>(total);
  seg.base_ = raw + kHeaderBytes;
  seg.size_ = bytes;
  seg.access_ = ShmAccess::ReadWrite;
  seg.owner_ = true;
  seg.name_ = norm;
  seg.mapping_handle_ = handle;
  return seg;
}

Result<ShmSegment> ShmSegment::open(std::string_view name, ShmAccess access) {
  if (name.empty()) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::open: empty name");
  }
  ATX_TRY(const std::string norm, normalize_name(name));
  const std::wstring wname = widen(norm);
  if (wname.empty()) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::open: name not valid UTF-8");
  }
  const DWORD map_access =
      (access == ShmAccess::ReadWrite) ? FILE_MAP_WRITE : static_cast<DWORD>(FILE_MAP_READ);

  HANDLE handle = OpenFileMappingW(map_access, FALSE, wname.c_str());
  if (handle == nullptr) {
    return Err(ErrorCode::NotFound, "ShmSegment::open: OpenFileMappingW failed (no such segment)");
  }
  void *view = MapViewOfFile(handle, map_access, 0, 0, 0);
  if (view == nullptr) {
    CloseHandle(handle);
    return Err(ErrorCode::IoError, "ShmSegment::open: MapViewOfFile failed");
  }
  // Query the ACTUAL mapped region size; OpenFileMapping exposes no size, so the
  // on-segment length header is untrusted until validated against the real
  // mapping. NOTE: guards an untrusted/hostile-creator scenario — a peer process
  // sharing the OS name namespace could pre-create a segment with a bogus header
  // (not reachable through this API's own writers, which always write a correct
  // header). Without this, an oversized `stored` would yield an OOB view_* span.
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0) {
    UnmapViewOfFile(view);
    CloseHandle(handle);
    return Err(ErrorCode::IoError, "ShmSegment::open: VirtualQuery failed");
  }
  const auto region = static_cast<atx::usize>(mbi.RegionSize);
  // SAFETY: view is the MapViewOfFile address; the first sizeof(u64) bytes are
  // the length header written by create(). reinterpret to a byte cursor is
  // required to read it and to offset to the payload.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto *raw = reinterpret_cast<std::byte *>(view);
  atx::u64 stored = 0;
  std::memcpy(&stored, raw, sizeof(stored));
  if (region < kHeaderBytes || stored > static_cast<atx::u64>(region - kHeaderBytes)) {
    UnmapViewOfFile(view);
    CloseHandle(handle);
    return Err(ErrorCode::InvalidArgument, "ShmSegment::open: shm header length exceeds mapping");
  }

  ShmSegment seg;
  seg.map_base_ = view;
  seg.map_bytes_ = region; // the ACTUAL mapped length, not stored + header
  seg.size_ = static_cast<atx::usize>(stored);
  seg.base_ = raw + kHeaderBytes;
  seg.access_ = access;
  seg.owner_ = false;
  seg.name_ = norm;
  seg.mapping_handle_ = handle;
  return seg;
}

// NOLINTEND(misc-include-cleaner)

// ---------------------------------------------------------------------------
// POSIX backend
// ---------------------------------------------------------------------------
#else

void ShmSegment::reset() noexcept {
  if (map_base_ != nullptr) {
    // Unmap exactly what we mapped (map_bytes_), never a size derived from the
    // header — a corrupt/externally-truncated segment must not feed munmap a
    // wrong length (UB on POSIX).
    ::munmap(map_base_, map_bytes_);
    map_base_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (owner_ && !name_.empty()) {
    ::shm_unlink(name_.c_str()); // creator removes the OS name
  }
  map_bytes_ = 0;
  base_ = nullptr;
  size_ = 0;
  access_ = ShmAccess::ReadOnly;
  owner_ = false;
  name_.clear();
}

Result<ShmSegment> ShmSegment::create(std::string_view name, atx::usize bytes) {
  if (name.empty()) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::create: empty name");
  }
  if (bytes == 0) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::create: zero bytes");
  }
  if (bytes > (static_cast<atx::usize>(-1) - kHeaderBytes)) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::create: bytes + header overflows usize");
  }
  ATX_TRY(const std::string norm, normalize_name(name));
  const atx::usize total = bytes + kHeaderBytes;

  const int fd = ::shm_open(norm.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    if (errno == EEXIST) {
      return Err(ErrorCode::AlreadyExists, "ShmSegment::create: name already exists");
    }
    return Err(ErrorCode::IoError, "ShmSegment::create: shm_open failed");
  }
  if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
    ::close(fd);
    ::shm_unlink(norm.c_str());
    return Err(ErrorCode::IoError, "ShmSegment::create: ftruncate failed");
  }
  void *addr = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
    ::close(fd);
    ::shm_unlink(norm.c_str());
    return Err(ErrorCode::IoError, "ShmSegment::create: mmap failed");
  }
  // SAFETY: addr is the mapped region; the first sizeof(u64) bytes are the
  // length header. reinterpret to a byte cursor is required.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto *raw = reinterpret_cast<std::byte *>(addr);
  const auto stored = static_cast<atx::u64>(bytes);
  std::memcpy(raw, &stored, sizeof(stored));

  ShmSegment seg;
  seg.map_base_ = addr;
  seg.map_bytes_ = total;
  seg.base_ = raw + kHeaderBytes;
  seg.size_ = bytes;
  seg.access_ = ShmAccess::ReadWrite;
  seg.owner_ = true;
  seg.name_ = norm;
  seg.fd_ = fd;
  return seg;
}

Result<ShmSegment> ShmSegment::open(std::string_view name, ShmAccess access) {
  if (name.empty()) {
    return Err(ErrorCode::InvalidArgument, "ShmSegment::open: empty name");
  }
  ATX_TRY(const std::string norm, normalize_name(name));
  const bool rw = (access == ShmAccess::ReadWrite);
  const int oflag = rw ? O_RDWR : O_RDONLY;
  const int prot = rw ? (PROT_READ | PROT_WRITE) : PROT_READ;

  const int fd = ::shm_open(norm.c_str(), oflag, 0600);
  if (fd < 0) {
    if (errno == ENOENT) {
      return Err(ErrorCode::NotFound, "ShmSegment::open: no such segment");
    }
    return Err(ErrorCode::IoError, "ShmSegment::open: shm_open failed");
  }
  // Recover the mapped length: stat the fd, then map header + payload.
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < 0 ||
      static_cast<atx::usize>(st.st_size) < kHeaderBytes) {
    ::close(fd);
    return Err(ErrorCode::IoError, "ShmSegment::open: fstat failed or segment too small");
  }
  const auto total = static_cast<atx::usize>(st.st_size);
  void *addr = ::mmap(nullptr, total, prot, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
    ::close(fd);
    return Err(ErrorCode::IoError, "ShmSegment::open: mmap failed");
  }
  // SAFETY: addr is the mapped region; read the length header written by
  // create(). reinterpret to a byte cursor is required.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto *raw = reinterpret_cast<std::byte *>(addr);
  atx::u64 stored = 0;
  std::memcpy(&stored, raw, sizeof(stored));
  // Validate the untrusted on-disk header against the REAL mapped size (`total`,
  // from fstat) before exposing a payload span. NOTE: guards an untrusted/
  // hostile-creator scenario — a peer process sharing the shm namespace could
  // pre-create a segment with a bogus header (not reachable through this API's
  // own writers). Without this, an oversized `stored` would yield an OOB span.
  if (stored > static_cast<atx::u64>(total - kHeaderBytes)) {
    ::munmap(addr, total);
    ::close(fd);
    return Err(ErrorCode::InvalidArgument, "ShmSegment::open: shm header length exceeds mapping");
  }

  ShmSegment seg;
  seg.map_base_ = addr;
  seg.map_bytes_ = total;
  seg.base_ = raw + kHeaderBytes;
  seg.size_ = static_cast<atx::usize>(stored);
  seg.access_ = access;
  seg.owner_ = false;
  seg.name_ = norm;
  seg.fd_ = fd;
  return seg;
}

#endif

} // namespace atx::engine::parallel
