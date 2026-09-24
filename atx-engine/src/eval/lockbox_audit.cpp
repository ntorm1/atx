// atx::engine::eval — FileLockboxAudit (see lockbox.hpp for the contract).
//
// Every read-modify-append of the audit log happens under an exclusive OS file
// lock (LockFileEx on Windows, flock elsewhere), which the OS drops if the
// holder dies, so a crashed opener can never wedge the log. All I/O on the log
// goes through the locked handle: Windows byte-range locks are mandatory, so a
// second stream on the same file would be refused while the lock is held.
#include "atx/engine/eval/lockbox.hpp"

#include <algorithm> // std::min
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace atx::engine::eval {

namespace {

using atx::u64;
using atx::usize;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;

constexpr std::string_view kTag = "ATXLBX2";
constexpr usize kIoChunk = usize{1} << 20U; // bounded single read/write call size

// ---------------------------------------------------------------------------
//  LockedFile — an exclusively locked read/write handle on the log (created
//  when missing). Move-only RAII: the destructor unlocks and closes.
// ---------------------------------------------------------------------------
class LockedFile {
public:
  LockedFile(const LockedFile &) = delete;
  LockedFile &operator=(const LockedFile &) = delete;
  LockedFile(LockedFile &&o) noexcept : h_{std::exchange(o.h_, kInvalid)} {}
  LockedFile &operator=(LockedFile &&) = delete;
  ~LockedFile() { release(); }

#if defined(_WIN32)
  [[nodiscard]] static Result<LockedFile> acquire(const std::filesystem::path &p) {
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot open " + p.string());
    }
    OVERLAPPED ov{};
    // Blocking exclusive lock over the whole (present and future) file.
    if (::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov) == 0) {
      ::CloseHandle(h);
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot lock " + p.string());
    }
    return LockedFile{h};
  }

  [[nodiscard]] Result<u64> size() const {
    LARGE_INTEGER sz{};
    if (::GetFileSizeEx(h_, &sz) == 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot stat the log");
    }
    return static_cast<u64>(sz.QuadPart);
  }

  [[nodiscard]] Result<std::string> read_range(u64 off, u64 len) const {
    std::string out(static_cast<usize>(len), '\0');
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(off);
    if (::SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) == 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot seek the log");
    }
    usize done = 0;
    // Bounded: each iteration reads >= 1 byte or fails.
    while (done < out.size()) {
      const DWORD want = static_cast<DWORD>(std::min(out.size() - done, kIoChunk));
      DWORD got = 0;
      if (::ReadFile(h_, out.data() + done, want, &got, nullptr) == 0 || got == 0) {
        return Err(ErrorCode::IoError, "FileLockboxAudit: cannot read the log");
      }
      done += got;
    }
    return out;
  }

  [[nodiscard]] Status truncate(u64 len) const {
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(len);
    if (::SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) == 0 || ::SetEndOfFile(h_) == 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot truncate the log");
    }
    return atx::core::Ok();
  }

  // Append at EOF and flush to stable storage.
  [[nodiscard]] Status append_durable(std::string_view bytes) const {
    LARGE_INTEGER zero{};
    if (::SetFilePointerEx(h_, zero, nullptr, FILE_END) == 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot seek the log end");
    }
    usize done = 0;
    while (done < bytes.size()) {
      const DWORD want = static_cast<DWORD>(std::min(bytes.size() - done, kIoChunk));
      DWORD put = 0;
      if (::WriteFile(h_, bytes.data() + done, want, &put, nullptr) == 0 || put == 0) {
        return Err(ErrorCode::IoError, "FileLockboxAudit: durable append failed");
      }
      done += put;
    }
    if (::FlushFileBuffers(h_) == 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: flush failed");
    }
    return atx::core::Ok();
  }

private:
  using Handle = HANDLE;
  static inline const Handle kInvalid = INVALID_HANDLE_VALUE;
  explicit LockedFile(Handle h) noexcept : h_{h} {}
  void release() noexcept {
    if (h_ != kInvalid) {
      OVERLAPPED ov{};
      ::UnlockFileEx(h_, 0, MAXDWORD, MAXDWORD, &ov);
      ::CloseHandle(h_);
      h_ = kInvalid;
    }
  }
#else
  [[nodiscard]] static Result<LockedFile> acquire(const std::filesystem::path &p) {
    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot open " + p.string());
    }
    int rc = 0;
    // Bounded by signal delivery: retry only on EINTR.
    do {
      rc = ::flock(fd, LOCK_EX);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
      ::close(fd);
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot lock " + p.string());
    }
    return LockedFile{fd};
  }

  [[nodiscard]] Result<u64> size() const {
    struct stat st{};
    if (::fstat(h_, &st) != 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot stat the log");
    }
    return static_cast<u64>(st.st_size);
  }

  [[nodiscard]] Result<std::string> read_range(u64 off, u64 len) const {
    std::string out(static_cast<usize>(len), '\0');
    usize done = 0;
    while (done < out.size()) {
      const ssize_t got = ::pread(h_, out.data() + done, std::min(out.size() - done, kIoChunk),
                                  static_cast<off_t>(off + done));
      if (got <= 0) {
        if (got < 0 && errno == EINTR) {
          continue;
        }
        return Err(ErrorCode::IoError, "FileLockboxAudit: cannot read the log");
      }
      done += static_cast<usize>(got);
    }
    return out;
  }

  [[nodiscard]] Status truncate(u64 len) const {
    if (::ftruncate(h_, static_cast<off_t>(len)) != 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot truncate the log");
    }
    return atx::core::Ok();
  }

  [[nodiscard]] Status append_durable(std::string_view bytes) const {
    if (::lseek(h_, 0, SEEK_END) < 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: cannot seek the log end");
    }
    usize done = 0;
    while (done < bytes.size()) {
      const ssize_t put =
          ::write(h_, bytes.data() + done, std::min(bytes.size() - done, kIoChunk));
      if (put <= 0) {
        if (put < 0 && errno == EINTR) {
          continue;
        }
        return Err(ErrorCode::IoError, "FileLockboxAudit: durable append failed");
      }
      done += static_cast<usize>(put);
    }
    if (::fsync(h_) != 0) {
      return Err(ErrorCode::IoError, "FileLockboxAudit: fsync failed");
    }
    return atx::core::Ok();
  }

private:
  using Handle = int;
  static constexpr Handle kInvalid = -1;
  explicit LockedFile(Handle h) noexcept : h_{h} {}
  void release() noexcept {
    if (h_ != kInvalid) {
      ::flock(h_, LOCK_UN);
      ::close(h_);
      h_ = kInvalid;
    }
  }
#endif

  Handle h_;
};

[[nodiscard]] std::string to_hex(u64 v) {
  char buf[17] = {};
  const auto res = std::to_chars(buf, buf + 16, v, 16);
  return std::string(buf, res.ptr);
}

[[nodiscard]] bool parse_hex(std::string_view s, u64 &out) {
  if (s.empty()) {
    return false;
  }
  const auto res = std::from_chars(s.data(), s.data() + s.size(), out, 16);
  return res.ec == std::errc{} && res.ptr == s.data() + s.size();
}

[[nodiscard]] std::string format_line(const LockboxReceipt &r) {
  std::string line{kTag};
  for (const u64 v : {r.sequence, r.content_address, r.candidate_hash,
                      static_cast<u64>(r.holdout_begin), static_cast<u64>(r.holdout_end),
                      r.prev_receipt_hash, r.receipt_hash}) {
    line += '\t';
    line += to_hex(v);
  }
  line += '\t';
  if (r.date_digests.empty()) {
    line += '-';
  }
  for (usize i = 0; i < r.date_digests.size(); ++i) {
    if (i > 0U) {
      line += ',';
    }
    line += to_hex(r.date_digests[i]);
  }
  line += '\t';
  line += detail::sanitize_audit_text(r.purpose);
  line += '\t';
  line += detail::sanitize_audit_text(r.requester);
  line += '\n';
  return line;
}

[[nodiscard]] std::optional<LockboxReceipt> parse_line(std::string_view line) {
  std::vector<std::string_view> f;
  usize p = 0;
  // Bounded: a line has at most line.size() + 1 tab-separated fields.
  for (usize guard = 0; guard <= line.size(); ++guard) {
    const usize tab = line.find('\t', p);
    f.push_back(line.substr(p, tab == std::string_view::npos ? line.size() - p : tab - p));
    if (tab == std::string_view::npos) {
      break;
    }
    p = tab + 1U;
  }
  if (f.size() != 11U || f[0] != kTag) {
    return std::nullopt;
  }
  u64 v[7] = {};
  for (usize i = 0; i < 7U; ++i) {
    if (!parse_hex(f[i + 1U], v[i])) {
      return std::nullopt;
    }
  }
  LockboxReceipt r;
  r.sequence = v[0];
  r.content_address = v[1];
  r.candidate_hash = v[2];
  r.holdout_begin = static_cast<usize>(v[3]);
  r.holdout_end = static_cast<usize>(v[4]);
  r.prev_receipt_hash = v[5];
  r.receipt_hash = v[6];
  const std::string_view dg = f[8];
  if (dg != "-") {
    usize q = 0;
    // Bounded: each iteration consumes one comma-separated digest.
    for (usize guard = 0; guard <= dg.size(); ++guard) {
      const usize comma = dg.find(',', q);
      const std::string_view tok =
          dg.substr(q, comma == std::string_view::npos ? dg.size() - q : comma - q);
      u64 d = 0;
      if (!parse_hex(tok, d)) {
        return std::nullopt;
      }
      r.date_digests.push_back(d);
      if (comma == std::string_view::npos) {
        break;
      }
      q = comma + 1U;
    }
  }
  r.purpose = std::string(f[9]);
  r.requester = std::string(f[10]);
  return r;
}

} // namespace

// Private-access shim: all locked I/O against a FileLockboxAudit's state.
struct FileLockboxAudit::Io {
  // Ingest every complete line in [a.synced_len_, EOF) — verifying each as the
  // next chain link — and truncate a trailing partial line. MUST be called with
  // the lock held: under the lock no writer is mid-append, so a partial line
  // can only be the torn tail of a crashed writer.
  [[nodiscard]] static Status sync(FileLockboxAudit &a, const LockedFile &f) {
    ATX_TRY(const u64 size, f.size());
    if (size < a.synced_len_) {
      return Err(ErrorCode::ParseError,
                 "FileLockboxAudit: the audit log shrank (truncated by another writer?)");
    }
    if (size == a.synced_len_) {
      return atx::core::Ok();
    }
    ATX_TRY(const std::string text, f.read_range(a.synced_len_, size - a.synced_len_));
    usize pos = 0;
    // Bounded: each iteration consumes one complete line.
    while (pos < text.size()) {
      const usize nl = text.find('\n', pos);
      if (nl == std::string::npos) {
        break; // torn tail
      }
      auto rec = parse_line(std::string_view(text).substr(pos, nl - pos));
      if (!rec.has_value() || !a.ledger_.links(*rec)) {
        return Err(ErrorCode::ParseError,
                   "FileLockboxAudit: audit chain verification failed (log edited?)");
      }
      a.ledger_.ingest(std::move(*rec));
      pos = nl + 1U;
    }
    a.synced_len_ += static_cast<u64>(pos);
    if (pos != text.size()) {
      ATX_TRY_VOID(f.truncate(a.synced_len_));
    }
    return atx::core::Ok();
  }
};

Result<FileLockboxAudit> FileLockboxAudit::open(const std::filesystem::path &path) {
  FileLockboxAudit out;
  out.path_ = path;
  ATX_TRY(LockedFile f, LockedFile::acquire(path));
  ATX_TRY_VOID(Io::sync(out, f));
  return atx::core::Ok(std::move(out));
}

Result<LockboxReceipt> FileLockboxAudit::commit(LockboxReceipt draft) {
  ATX_TRY(LockedFile f, LockedFile::acquire(path_));
  // Catch up with every receipt other handles / processes committed since we
  // last looked, then decide single-use against that fresh state.
  ATX_TRY_VOID(Io::sync(*this, f));
  ATX_TRY_VOID(ledger_.check_fresh(draft));
  LockboxReceipt r = ledger_.seal(std::move(draft));
  const std::string line = format_line(r);
  const Status st = f.append_durable(line);
  if (!st) {
    (void)f.truncate(synced_len_); // best effort: drop a partial line
    return atx::core::Err(st.error());
  }
  synced_len_ += static_cast<u64>(line.size());
  ledger_.ingest(r);
  return atx::core::Ok(std::move(r));
}

} // namespace atx::engine::eval
