#include "atx/vol/detail/writer_lock.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

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
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace atx::vol::detail {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::optional<std::uint64_t> parse_pid(std::string_view text) noexcept {
  // Trim the trailing whitespace/NUL padding a fixed-size read buffer leaves.
  std::size_t end = text.size();
  while (end > 0 && (text[end - 1] == '\0' || text[end - 1] == '\n' || text[end - 1] == '\r' ||
                     text[end - 1] == ' ')) {
    --end;
  }
  if (end == 0) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < end; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

// Tri-state outcome of one CREATE_NEW/O_EXCL attempt: `Err` is an unexpected
// filesystem failure; `Ok(handle)` is a fresh lock; `Ok(nullopt)` means the
// path is already taken (by a live OR stale owner -- the caller decides
// which).
#if defined(_WIN32)
[[nodiscard]] Result<std::optional<std::intptr_t>> try_create_new(const fs::path &path) {
  HANDLE h = ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    return Ok(std::optional<std::intptr_t>{reinterpret_cast<std::intptr_t>(h)});
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
    return Ok(std::optional<std::intptr_t>{std::nullopt});
  }
  return Err(ErrorCode::IoError,
             "WriterLock::acquire: cannot create lock file (win32 error=" +
                 std::to_string(error) + ")");
}

[[nodiscard]] Status write_owner_pid(std::intptr_t native_handle, std::uint64_t pid) {
  const auto h = reinterpret_cast<HANDLE>(native_handle);
  const std::string text = std::to_string(pid);
  DWORD written = 0;
  const BOOL ok =
      ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  if (ok == FALSE || written != text.size()) {
    return Err(ErrorCode::IoError, "WriterLock::acquire: cannot write owner pid to lock file");
  }
  if (::FlushFileBuffers(h) == FALSE) {
    return Err(ErrorCode::IoError, "WriterLock::acquire: cannot flush lock file");
  }
  return Ok();
}

void close_handle(std::intptr_t native_handle) noexcept {
  ::CloseHandle(reinterpret_cast<HANDLE>(native_handle));
}

// Best-effort: empty/unreadable/vanished all come back as nullopt, which the
// caller treats as "cannot determine an owner" -- i.e. NOT eligible for
// stale-lock takeover, the conservative default.
[[nodiscard]] std::optional<std::uint64_t> read_owner_pid(const fs::path &path) {
  // Must share READ + WRITE + DELETE: the live owner's own handle was opened
  // with GENERIC_READ|GENERIC_WRITE, and Windows sharing is bidirectional --
  // this open only succeeds alongside that outstanding access if it declares
  // itself willing to share all of it, even though this call only ever reads.
  HANDLE h = ::CreateFileW(path.wstring().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  char buf[32];
  DWORD read = 0;
  const BOOL ok = ::ReadFile(h, buf, static_cast<DWORD>(sizeof buf), &read, nullptr);
  ::CloseHandle(h);
  if (ok == FALSE) {
    return std::nullopt;
  }
  return parse_pid(std::string_view(buf, static_cast<std::size_t>(read)));
}

[[nodiscard]] bool process_alive(std::uint64_t pid) noexcept {
  if (pid == 0 || pid > static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())) {
    return false;
  }
  HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) {
    // ERROR_INVALID_PARAMETER: no process with this id exists right now.
    return false;
  }
  DWORD exit_code = 0;
  const BOOL got = ::GetExitCodeProcess(h, &exit_code);
  ::CloseHandle(h);
  // Indeterminate reads as "alive" -- the conservative direction: it can only
  // cause acquire() to wait/report contention on a dead owner, never to
  // delete a live one's lock.
  return (got == FALSE) || exit_code == STILL_ACTIVE;
}
#else
[[nodiscard]] Result<std::optional<std::intptr_t>> try_create_new(const fs::path &path) {
  const std::string p = path.string();
  const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd >= 0) {
    return Ok(std::optional<std::intptr_t>{fd});
  }
  if (errno == EEXIST) {
    return Ok(std::optional<std::intptr_t>{std::nullopt});
  }
  return Err(ErrorCode::IoError, "WriterLock::acquire: cannot create lock file: " + p);
}

[[nodiscard]] Status write_owner_pid(std::intptr_t native_handle, std::uint64_t pid) {
  const int fd = static_cast<int>(native_handle);
  const std::string text = std::to_string(pid);
  const ssize_t written = ::write(fd, text.data(), text.size());
  if (written < 0 || static_cast<std::size_t>(written) != text.size()) {
    return Err(ErrorCode::IoError, "WriterLock::acquire: cannot write owner pid to lock file");
  }
  if (::fsync(fd) != 0) {
    return Err(ErrorCode::IoError, "WriterLock::acquire: cannot flush lock file");
  }
  return Ok();
}

void close_handle(std::intptr_t native_handle) noexcept { ::close(static_cast<int>(native_handle)); }

[[nodiscard]] std::optional<std::uint64_t> read_owner_pid(const fs::path &path) {
  const std::string p = path.string();
  const int fd = ::open(p.c_str(), O_RDONLY);
  if (fd < 0) {
    return std::nullopt;
  }
  char buf[32];
  const ssize_t got = ::read(fd, buf, sizeof buf);
  ::close(fd);
  if (got < 0) {
    return std::nullopt;
  }
  return parse_pid(std::string_view(buf, static_cast<std::size_t>(got)));
}

[[nodiscard]] bool process_alive(std::uint64_t pid) noexcept {
  if (pid == 0) {
    return false;
  }
  const int rc = ::kill(static_cast<pid_t>(pid), 0);
  if (rc == 0) {
    return true;
  }
  // ESRCH: definitively no such process. Anything else (e.g. EPERM -- exists,
  // owned by someone else) reads conservatively as "alive".
  return errno != ESRCH;
}
#endif

// Deletes `path` iff it still holds exactly `expected_pid` -- narrows (does
// not eliminate; see the header's documented limitation) the race between
// probing a stale owner and another process already having taken it over.
// Best-effort: failures are swallowed, since the caller's retry loop is the
// real recovery path either way (either this process wins the next
// CREATE_NEW, or it discovers a fresh live owner and waits/backs off).
void try_take_over_stale_lock(const fs::path &path, std::uint64_t expected_pid) noexcept {
  const std::optional<std::uint64_t> still_there = read_owner_pid(path);
  if (still_there.has_value() && *still_there == expected_pid) {
    std::error_code ec;
    fs::remove(path, ec);
  }
}

} // namespace

Result<WriterLock> WriterLock::acquire(std::string_view lock_path,
                                       std::chrono::milliseconds timeout) {
  if (lock_path.empty()) {
    return Err(ErrorCode::InvalidArgument, "WriterLock::acquire: empty lock path");
  }
  const fs::path path{std::string(lock_path)};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  constexpr std::chrono::milliseconds kRetryInterval{20};

  for (;;) {
    auto attempt = try_create_new(path);
    if (!attempt) {
      return Err(attempt.error());
    }
    if (attempt->has_value()) {
      const std::intptr_t handle = **attempt;
      if (Status wrote = write_owner_pid(handle, current_process_id()); !wrote) {
        close_handle(handle);
        std::error_code ec;
        fs::remove(path, ec);
        return Err(wrote.error());
      }
      return Ok(WriterLock(path.string(), handle));
    }

    // Contended: `path` already exists. A determinate DEAD owner is taken
    // over immediately (no budget spent waiting on something that will never
    // release); anything else (live, or indeterminate/racing) counts against
    // `timeout`.
    const std::optional<std::uint64_t> owner_pid = read_owner_pid(path);
    const bool stale = owner_pid.has_value() && !process_alive(*owner_pid);
    if (stale) {
      try_take_over_stale_lock(path, *owner_pid);
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      return Err(ErrorCode::Unavailable,
                 "WriterLock::acquire: timed out waiting for lock: " + path.string());
    }
    if (!stale) {
      // `deadline` was just confirmed to be in the future, so this is always
      // positive (sleep_for treats a non-positive duration as a no-op
      // regardless, if scheduling delay ever made it otherwise).
      std::this_thread::sleep_for(std::min(
          kRetryInterval, std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())));
    }
    // A successful stale takeover retries CREATE_NEW immediately, no sleep.
  }
}

WriterLock::~WriterLock() { release(); }

WriterLock::WriterLock(WriterLock &&other) noexcept
    : lock_path_{std::move(other.lock_path_)}, native_handle_{other.native_handle_} {
  other.native_handle_ = kInvalidHandle;
  other.lock_path_.clear();
}

WriterLock &WriterLock::operator=(WriterLock &&other) noexcept {
  if (this != &other) {
    release();
    lock_path_ = std::move(other.lock_path_);
    native_handle_ = other.native_handle_;
    other.native_handle_ = kInvalidHandle;
    other.lock_path_.clear();
  }
  return *this;
}

void WriterLock::release() noexcept {
  if (native_handle_ == kInvalidHandle) {
    return;
  }
  close_handle(native_handle_);
  native_handle_ = kInvalidHandle;
  // Bounded best-effort retry: a concurrent liveness-probe reader can hold a
  // transient open against this file (Windows sharing), so the first delete
  // attempt losing that race is expected, not exceptional.
  std::error_code ec;
  for (int attempt = 0; attempt < 5; ++attempt) {
    fs::remove(lock_path_, ec);
    if (!ec) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  lock_path_.clear();
}

} // namespace atx::vol::detail
