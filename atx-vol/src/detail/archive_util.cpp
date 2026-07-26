#include "atx/vol/detail/archive_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <new>
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
#include <unistd.h>
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define ATX_ARCH_X86 1
#include <intrin.h>
#else
#define ATX_ARCH_X86 0
#endif

namespace atx::vol::detail {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Status;

namespace {

std::atomic<std::uint64_t> g_publish_temp_counter{0u};
std::array<std::mutex, 64> g_publish_destination_locks;

[[nodiscard]] std::uint64_t process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::string destination_lock_key(const std::filesystem::path &dst) {
  std::error_code ec;
  std::filesystem::path normalized = std::filesystem::weakly_canonical(dst, ec);
  if (ec) {
    ec.clear();
    normalized = std::filesystem::absolute(dst, ec);
    if (ec) {
      normalized = dst;
    }
  }
  std::string key = normalized.lexically_normal().generic_string();
#if defined(_WIN32)
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
    return static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
  });
#endif
  return key;
}

[[nodiscard]] std::mutex &destination_lock(const std::filesystem::path &dst) {
  const std::size_t slot =
      std::hash<std::string>{}(destination_lock_key(dst)) % g_publish_destination_locks.size();
  return g_publish_destination_locks[slot];
}

[[nodiscard]] Status fsync_parent_directory(const std::filesystem::path &dst) {
#if defined(_WIN32)
  (void)dst;
  return Ok();
#else
  std::filesystem::path parent = dst.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const std::string path = parent.string();
  int flags = O_RDONLY;
#if defined(O_DIRECTORY)
  flags |= O_DIRECTORY;
#endif
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    return Err(ErrorCode::IoError,
               "flush_and_publish_file: published but cannot open parent directory for fsync");
  }
  const int rc = ::fsync(fd);
  const int close_rc = ::close(fd);
  if (rc != 0 || close_rc != 0) {
    return Err(ErrorCode::IoError,
               "flush_and_publish_file: published but parent directory fsync failed");
  }
  return Ok();
#endif
}

// ── CRC-32C (Castagnoli, reflected poly 0x82F63B78) ──────────────────────
//
// Two implementations with bit-identical output: a table-driven fallback and a
// hardware SSE4.2 `_mm_crc32` path (8 bytes/step), runtime-dispatched by CPUID.
// The running-state semantics (no init/final XOR inside `_update`) match, so the
// one-shot `crc32c` = update(0xFFFFFFFF) ^ 0xFFFFFFFF regardless of path.

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t n = 0; n < 256; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) {
      c = ((c & 1u) != 0u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
    }
    table[n] = c;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

[[nodiscard]] std::uint32_t crc32c_update_table(std::uint32_t crc, const std::byte *p,
                                                std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    const auto b = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i]));
    crc = kCrc32cTable[(crc ^ b) & 0xFFu] ^ (crc >> 8);
  }
  return crc;
}

#if ATX_ARCH_X86
[[nodiscard]] bool detect_sse42() noexcept {
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, 1);
  return (regs[2] & (1 << 20)) != 0; // ECX bit 20 = SSE4.2 (CRC32)
}
// Resolved once at load; the dispatch below is a predictable branch.
const bool kHasSse42 = detect_sse42();

// The SSE4.2 CRC32 intrinsics require the `crc32` target feature to be emitted.
// The build compiles the TU without it (baseline x86-64), so mark just this
// function with the target attribute; it is only ever *called* behind the runtime
// CPUID gate (kHasSse42), so no unsupported instruction is executed on an old CPU.
#if defined(__clang__)
__attribute__((target("sse4.2")))
#endif
std::uint32_t
crc32c_update_hw(std::uint32_t crc, const std::byte *p, std::size_t n) noexcept {
  std::uint64_t c = crc;
  while (n >= 8) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 8);
    c = _mm_crc32_u64(c, v);
    p += 8;
    n -= 8;
  }
  auto c32 = static_cast<std::uint32_t>(c);
  while (n != 0) {
    c32 = _mm_crc32_u8(c32, std::to_integer<std::uint8_t>(*p));
    ++p;
    --n;
  }
  return c32;
}
#endif

} // namespace

// Continue a CRC-32C over [p, p+n). `crc` is the running (un-finalized) state.
[[nodiscard]] std::uint32_t crc32c_update(std::uint32_t crc, const std::byte *p,
                                          std::size_t n) noexcept {
#if ATX_ARCH_X86
  if (kHasSse42) {
    return crc32c_update_hw(crc, p, n);
  }
#endif
  return crc32c_update_table(crc, p, n);
}

// One-shot CRC-32C with the standard init/final XOR applied.
[[nodiscard]] std::uint32_t crc32c(const std::byte *p, std::size_t n) noexcept {
  return crc32c_update(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

// ASCII upper-case + truncate to max_len. Returns exactly the canonical bytes
// (no zero-pad tail): callers that need a fixed-width buffer size it to the
// returned std::string's length, matching the archive's prior fixed-array
// semantics where only the first `n` bytes were ever hashed/compared/copied.
[[nodiscard]] std::string canonicalize_symbol(std::string_view s, std::size_t max_len) {
  const std::size_t n = std::min(s.size(), max_len);
  std::string dst(n, '\0');
  for (std::size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    dst[i] = c;
  }
  return dst;
}

atx::core::Result<std::string> reserve_unique_publish_temp_file(std::string_view dst_path) {
  namespace fs = std::filesystem;
  const fs::path dst{std::string(dst_path)};
  if (dst.empty() || dst.filename().empty()) {
    return Err(ErrorCode::InvalidArgument,
               "reserve_unique_publish_temp_file: destination must name a file");
  }
  const fs::path parent = dst.has_parent_path() ? dst.parent_path() : fs::path{"."};
  const std::string prefix =
      dst.filename().string() + ".tmp." + std::to_string(process_id()) + ".";

  constexpr std::size_t kMaxAttempts = 1024u;
  for (std::size_t attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const std::uint64_t sequence =
        g_publish_temp_counter.fetch_add(1u, std::memory_order_relaxed);
    const fs::path candidate = parent / (prefix + std::to_string(sequence));
#if defined(_WIN32)
    HANDLE h = ::CreateFileW(candidate.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      ::CloseHandle(h);
      return Ok(candidate.string());
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
      continue;
    }
#else
    const std::string path = candidate.string();
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      if (::close(fd) != 0) {
        std::error_code ignored;
        fs::remove(candidate, ignored);
        return Err(ErrorCode::IoError,
                   "reserve_unique_publish_temp_file: cannot close reserved temp");
      }
      return Ok(candidate.string());
    }
    if (errno == EEXIST) {
      continue;
    }
#endif
    return Err(ErrorCode::IoError,
               "reserve_unique_publish_temp_file: cannot create same-directory temp");
  }
  return Err(ErrorCode::IoError,
             "reserve_unique_publish_temp_file: unique temp attempts exhausted");
}

namespace {

// fsync `p`'s already-written contents to stable storage. The caller wrote and
// closed the file; we re-open it to flush its cached pages (FlushFileBuffers /
// fsync operate on the file, not the writing handle).
[[nodiscard]] Status fsync_file(const std::filesystem::path &p) {
#if defined(_WIN32)
  const std::wstring wp = p.wstring();
  HANDLE h = ::CreateFileW(wp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return Err(ErrorCode::IoError, "flush_and_publish_file: cannot open temp for fsync");
  }
  const BOOL flushed = ::FlushFileBuffers(h);
  ::CloseHandle(h);
  if (flushed == FALSE) {
    return Err(ErrorCode::IoError, "flush_and_publish_file: FlushFileBuffers failed");
  }
  return Ok();
#else
  const std::string sp = p.string();
  const int fd = ::open(sp.c_str(), O_RDONLY);
  if (fd < 0) {
    return Err(ErrorCode::IoError, "flush_and_publish_file: cannot open temp for fsync");
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return Err(ErrorCode::IoError, "flush_and_publish_file: fsync failed");
  }
  return Ok();
#endif
}

} // namespace

Status flush_and_publish_file(std::string_view tmp_path, std::string_view dst_path) {
  namespace fs = std::filesystem;
  const fs::path tmp{std::string(tmp_path)};
  const fs::path dst{std::string(dst_path)};
  const fs::path tmp_parent = tmp.has_parent_path() ? tmp.parent_path() : fs::path{"."};
  const fs::path dst_parent = dst.has_parent_path() ? dst.parent_path() : fs::path{"."};
  if (tmp.empty() || dst.empty() || tmp == dst ||
      destination_lock_key(tmp_parent) != destination_lock_key(dst_parent)) {
    return Err(ErrorCode::InvalidArgument,
               "flush_and_publish_file: temp and destination must be distinct siblings");
  }

  // Serializing this interval prevents local writers from racing replacement of
  // the same live path. Exclusive unique temp creation covers temp-name safety
  // across processes; each rename remains atomic.
  std::scoped_lock publish_lock(destination_lock(dst));

  // (1) Durability: flush the temp to disk BEFORE it becomes the live file, so a
  // machine crash after the rename can never expose a correctly-named but empty /
  // garbage file (which the rename would have substituted for the prior good one).
  if (Status s = fsync_file(tmp); !s) {
    return s;
  }

  // (2) Atomic publish with bounded retry + exponential backoff. On Windows the
  // rename fails while any process holds `dst` open without FILE_SHARE_DELETE
  // (MSVC ifstream, mmap); the hold is usually brief, so back off and retry.
  // Cumulative budget ~635 ms across 8 attempts (5,10,20,40,80,160,320 ms gaps).
  constexpr int kMaxAttempts = 8;
  std::chrono::milliseconds delay{5};
  std::error_code ec;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    ec.clear();
    fs::rename(tmp, dst, ec);
    if (!ec) {
      return fsync_parent_directory(dst);
    }
    if (attempt + 1 < kMaxAttempts) {
      std::this_thread::sleep_for(delay);
      delay *= 2;
    }
  }
  // Final failure: PRESERVE the temp (do not delete) so the freshly written bytes
  // survive for recovery; the prior good `dst` is also still intact (rename never
  // succeeded). Clear IoError.
  return Err(ErrorCode::IoError,
             "flush_and_publish_file: rename failed after retries (temp preserved)");
}

// ── Whole-file slurp ────────────────────────────────────────────────────────
// See the header for why this exists and why it is `fread` rather than
// `istreambuf_iterator`. Byte-identical to the form it replaces.
[[nodiscard]] FileReadStatus read_whole_file(std::string_view path, std::string &out) {
  out.clear();
  const std::filesystem::path p{std::string(path)};

  // A directory opens successfully under `fopen` on POSIX and would then fail
  // the read, reporting IoError where the `ifstream` form reported NotFound.
  // Checked explicitly so the two forms are indistinguishable to callers.
  std::error_code ec;
  if (std::filesystem::is_directory(p, ec)) {
    return FileReadStatus::NotFound;
  }

  std::FILE *fp = nullptr;
#if defined(_WIN32)
  if (::_wfopen_s(&fp, p.wstring().c_str(), L"rb") != 0 || fp == nullptr) {
    return FileReadStatus::NotFound;
  }
#else
  fp = std::fopen(p.c_str(), "rb");
  if (fp == nullptr) {
    return FileReadStatus::NotFound;
  }
#endif

  const auto fail = [&](FileReadStatus s) {
    std::fclose(fp);
    out.clear();
    return s;
  };

  ec.clear();
  const std::uintmax_t hinted = std::filesystem::file_size(p, ec);
  if (!ec && hinted > 0u) {
    if (hinted > static_cast<std::uintmax_t>(out.max_size())) {
      return fail(FileReadStatus::IoError);
    }
    try {
      out.resize(static_cast<std::size_t>(hinted));
    } catch (const std::bad_alloc &) {
      return fail(FileReadStatus::IoError);
    }
    const std::size_t got = std::fread(out.data(), 1, out.size(), fp);
    if (got != out.size()) {
      if (std::ferror(fp) != 0) {
        return fail(FileReadStatus::IoError);
      }
      out.resize(got); // the file shrank between the stat and the read
    }
  }

  // Drain anything past the hinted size (the file grew, or `file_size` failed
  // and nothing has been read yet). Normally this is a single zero-length read.
  char chunk[64 * 1024];
  for (;;) {
    const std::size_t n = std::fread(chunk, 1, sizeof chunk, fp);
    if (n > 0u) {
      try {
        out.append(chunk, n);
      } catch (const std::bad_alloc &) {
        return fail(FileReadStatus::IoError);
      }
    }
    if (n < sizeof chunk) {
      break;
    }
  }
  if (std::ferror(fp) != 0) {
    return fail(FileReadStatus::IoError);
  }
  if (std::fclose(fp) != 0) {
    out.clear();
    return FileReadStatus::IoError;
  }
  return FileReadStatus::Ok;
}

} // namespace atx::vol::detail
