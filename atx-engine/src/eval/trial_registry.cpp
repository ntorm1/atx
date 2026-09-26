// atx::engine::eval — TrialRegistry and trial clustering (see trial_registry.hpp
// and trial_clusters.hpp for the contracts).
//
// Every durable operation on the log runs under an exclusive OS file lock
// (LockFileEx on Windows, flock elsewhere) taken on ONE persistent handle, so
// all I/O on the log goes through that handle (Windows byte-range locks are
// mandatory: a second handle cannot read or write the locked range).
#include "atx/engine/eval/trial_registry.hpp"

#include <algorithm> // std::max, std::min, std::clamp, std::sort, std::fill
#include <array>
#include <bit>     // std::bit_cast
#include <charconv>
#include <cmath>   // std::sqrt, std::isfinite, std::log, std::cos
#include <cstring> // std::memcpy, std::memcmp
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <Eigen/Core> // batched SYRK Gram update

#include <ankerl/unordered_dense.h> // flat id set for 10^6-trial registries

#include "atx/engine/eval/trial_clusters.hpp"

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

using atx::f64;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;

constexpr u64 kGolden = 0x9e3779b97f4a7c15ULL;
constexpr std::array<char, 8> kMagicV1{'A', 'T', 'X', 'T', 'R', 'G', '0', '1'};
constexpr std::array<char, 8> kMagicV2{'A', 'T', 'X', 'T', 'R', 'G', '0', '2'};
constexpr std::array<char, 8> kMagicV3{'A', 'T', 'X', 'T', 'R', 'G', '0', '3'};
constexpr usize kMagicStem = 7U;    // "ATXTRG0" — the version digit follows
constexpr usize kHeaderBytes = 48U; // magic, version, pnl_len, sketch_dim, seed, checksum
constexpr usize kRecordFixedV1 = 40U; // id, config_hash, sharpe, kind, (sketch), checksum
// id, config_hash, sharpe, kind|fidelity|sample, window_start, window_end,
// family_tag, theme_tag, (sketch), checksum
constexpr usize kRecordFixedV2 = 72U;
constexpr usize kRecordFixedV3 = 88U; // length, observation, identity/meta, SR, sketch, checksum
constexpr usize kScreenRecordV3 = 96U; // identity/meta, recipe/reason tags; NO SR or sketch
constexpr usize kMinSketchDim = 8U;
constexpr usize kMinWindow = 3U;
constexpr u64 kLegacyChainSeed = 0x6a09e667f3bcc909ULL;  // registry_hash (V1 semantics)
constexpr u64 kContentChainSeed = 0xbb67ae8584caa73bULL; // tamper-evident chain head
constexpr usize kIoChunk = usize{1} << 20U; // bounded single read / write call size
constexpr std::string_view kHeadTag = "ATXTRGH1";
// Sketches are folded into the Gram in batches of this many columns with one
// blocked, vectorized symmetric rank-k update (SYRK) instead of per-record
// scalar rank-1 updates. Batch boundaries depend only on insertion order, so
// the Gram is a pure function of the registry history.
constexpr Eigen::Index kGramBatch = 32;
// Silhouette-sd floor for ONC quality (a perfectly uniform partition has sd 0).
constexpr f64 kQualitySdFloor = 1e-9;
constexpr usize kMaxMcDraws = 10'000'000U;

[[nodiscard]] constexpr u64 splitmix64(u64 x) noexcept {
  u64 z = x + kGolden;
  z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31U);
}

// Stable byte digest: FNV-1a 64 with a splitmix64 finalizer (avalanche).
[[nodiscard]] u64 stable_digest(const unsigned char *p, usize n) noexcept {
  u64 h = 0xcbf29ce484222325ULL;
  for (usize i = 0; i < n; ++i) {
    h ^= static_cast<u64>(p[i]);
    h *= 0x100000001b3ULL;
  }
  return splitmix64(h);
}

void put_u64(std::vector<unsigned char> &buf, usize off, u64 v) noexcept {
  std::memcpy(buf.data() + off, &v, sizeof(v)); // host order; x64 is little-endian
}

[[nodiscard]] u64 get_u64(const unsigned char *p) noexcept {
  u64 v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

[[nodiscard]] bool valid_kind(u64 k) noexcept {
  return k <= static_cast<u64>(TrialKind::OptimizerHyper);
}

[[nodiscard]] bool valid_sample(u64 s) noexcept {
  return s <= static_cast<u64>(TrialSample::OutOfSample);
}

// Deterministic generator: splitmix64 stream, (0,1) uniforms, Box-Muller normals.
class SplitMixRng {
public:
  explicit SplitMixRng(u64 seed) noexcept : s_{seed} {}
  [[nodiscard]] u64 next() noexcept {
    s_ += kGolden;
    u64 z = s_;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
  }
  [[nodiscard]] f64 uniform() noexcept { // strictly inside (0, 1)
    return (static_cast<f64>(next() >> 11U) + 0.5) * 0x1.0p-53;
  }
  [[nodiscard]] f64 normal() noexcept {
    if (have_) {
      have_ = false;
      return cached_;
    }
    const f64 r = std::sqrt(-2.0 * std::log(uniform()));
    const f64 th = 6.283185307179586 * uniform();
    cached_ = r * std::sin(th);
    have_ = true;
    return r * std::cos(th);
  }

private:
  u64 s_;
  bool have_{false};
  f64 cached_{0.0};
};

// ---------------------------------------------------------------------------
//  LogFile — one persistent read/write handle on the log (created when
//  missing). Move-only RAII. lock()/unlock() take / drop an exclusive OS lock
//  over the whole (present and future) file.
// ---------------------------------------------------------------------------
class LogFile {
public:
  LogFile(const LogFile &) = delete;
  LogFile &operator=(const LogFile &) = delete;
#if defined(_WIN32)
  LogFile(LogFile &&o) noexcept : h_{std::exchange(o.h_, INVALID_HANDLE_VALUE)} {}
  LogFile &operator=(LogFile &&o) noexcept {
    if (this != &o) {
      close();
      h_ = std::exchange(o.h_, INVALID_HANDLE_VALUE);
    }
    return *this;
  }
  ~LogFile() { close(); }

  [[nodiscard]] static Result<LogFile> open(const std::filesystem::path &p) {
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot open " + p.string());
    }
    return LogFile{h};
  }

  [[nodiscard]] Status lock() const {
    OVERLAPPED ov{};
    // Blocking exclusive lock over the whole (present and future) file.
    if (::LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov) == 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot lock the log");
    }
    return atx::core::Ok();
  }
  void unlock() const noexcept {
    OVERLAPPED ov{};
    (void)::UnlockFileEx(h_, 0, MAXDWORD, MAXDWORD, &ov);
  }

  [[nodiscard]] Result<u64> size() const {
    LARGE_INTEGER sz{};
    if (::GetFileSizeEx(h_, &sz) == 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot stat the log");
    }
    return static_cast<u64>(sz.QuadPart);
  }

  [[nodiscard]] Status read_at(u64 off, unsigned char *dst, usize len) const {
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(off);
    if (::SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) == 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot seek the log");
    }
    usize done = 0;
    // Bounded: each iteration reads >= 1 byte or fails.
    while (done < len) {
      const DWORD want = static_cast<DWORD>(std::min(len - done, kIoChunk));
      DWORD got = 0;
      if (::ReadFile(h_, dst + done, want, &got, nullptr) == 0 || got == 0) {
        return Err(ErrorCode::IoError, "TrialRegistry: cannot read the log");
      }
      done += got;
    }
    return atx::core::Ok();
  }

  [[nodiscard]] Status truncate(u64 len) const {
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(len);
    if (::SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) == 0 || ::SetEndOfFile(h_) == 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot truncate the log");
    }
    return atx::core::Ok();
  }

  // Append at EOF (handed to the OS before returning; survives a process crash).
  [[nodiscard]] Status append(const unsigned char *p, usize len) const {
    LARGE_INTEGER zero{};
    if (::SetFilePointerEx(h_, zero, nullptr, FILE_END) == 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot seek the log end");
    }
    usize done = 0;
    while (done < len) {
      const DWORD want = static_cast<DWORD>(std::min(len - done, kIoChunk));
      DWORD put = 0;
      if (::WriteFile(h_, p + done, want, &put, nullptr) == 0 || put == 0) {
        return Err(ErrorCode::IoError, "TrialRegistry: durable append failed");
      }
      done += put;
    }
    return atx::core::Ok();
  }

private:
  explicit LogFile(HANDLE h) noexcept : h_{h} {}
  void close() noexcept {
    if (h_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(h_);
      h_ = INVALID_HANDLE_VALUE;
    }
  }
  HANDLE h_;
#else
  LogFile(LogFile &&o) noexcept : h_{std::exchange(o.h_, -1)} {}
  LogFile &operator=(LogFile &&o) noexcept {
    if (this != &o) {
      close();
      h_ = std::exchange(o.h_, -1);
    }
    return *this;
  }
  ~LogFile() { close(); }

  [[nodiscard]] static Result<LogFile> open(const std::filesystem::path &p) {
    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot open " + p.string());
    }
    return LogFile{fd};
  }

  [[nodiscard]] Status lock() const {
    int rc = 0;
    // Bounded by signal delivery: retry only on EINTR.
    do {
      rc = ::flock(h_, LOCK_EX);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot lock the log");
    }
    return atx::core::Ok();
  }
  void unlock() const noexcept { (void)::flock(h_, LOCK_UN); }

  [[nodiscard]] Result<u64> size() const {
    struct stat st{};
    if (::fstat(h_, &st) != 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot stat the log");
    }
    return static_cast<u64>(st.st_size);
  }

  [[nodiscard]] Status read_at(u64 off, unsigned char *dst, usize len) const {
    usize done = 0;
    while (done < len) {
      const ssize_t got =
          ::pread(h_, dst + done, std::min(len - done, kIoChunk), static_cast<off_t>(off + done));
      if (got <= 0) {
        if (got < 0 && errno == EINTR) {
          continue;
        }
        return Err(ErrorCode::IoError, "TrialRegistry: cannot read the log");
      }
      done += static_cast<usize>(got);
    }
    return atx::core::Ok();
  }

  [[nodiscard]] Status truncate(u64 len) const {
    if (::ftruncate(h_, static_cast<off_t>(len)) != 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot truncate the log");
    }
    return atx::core::Ok();
  }

  [[nodiscard]] Status append(const unsigned char *p, usize len) const {
    if (::lseek(h_, 0, SEEK_END) < 0) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot seek the log end");
    }
    usize done = 0;
    while (done < len) {
      const ssize_t put = ::write(h_, p + done, std::min(len - done, kIoChunk));
      if (put <= 0) {
        if (put < 0 && errno == EINTR) {
          continue;
        }
        return Err(ErrorCode::IoError, "TrialRegistry: durable append failed");
      }
      done += static_cast<usize>(put);
    }
    return atx::core::Ok();
  }

private:
  explicit LogFile(int fd) noexcept : h_{fd} {}
  void close() noexcept {
    if (h_ >= 0) {
      ::close(h_);
      h_ = -1;
    }
  }
  int h_;
#endif
};

// Scoped exclusive lock on a LogFile (move-only; unlocks on destruction).
class LogLock {
public:
  [[nodiscard]] static Result<LogLock> acquire(const LogFile &f) {
    ATX_TRY_VOID(f.lock());
    return LogLock{&f};
  }
  LogLock(LogLock &&o) noexcept : f_{std::exchange(o.f_, nullptr)} {}
  LogLock &operator=(LogLock &&) = delete;
  LogLock(const LogLock &) = delete;
  LogLock &operator=(const LogLock &) = delete;
  ~LogLock() {
    if (f_ != nullptr) {
      f_->unlock();
    }
  }

private:
  explicit LogLock(const LogFile *f) noexcept : f_{f} {}
  const LogFile *f_;
};

[[nodiscard]] std::string to_hex(u64 v) {
  std::array<char, 17> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + 16, v, 16);
  return std::string(buf.data(), res.ptr);
}

[[nodiscard]] bool parse_hex(std::string_view s, u64 &out) {
  if (s.empty()) {
    return false;
  }
  const auto res = std::from_chars(s.data(), s.data() + s.size(), out, 16);
  return res.ec == std::errc{} && res.ptr == s.data() + s.size();
}

[[nodiscard]] u64 head_check(const TrialChainHead &h) noexcept {
  return splitmix64(h.records ^ splitmix64(h.head ^ 0x4854524f4c415254ULL));
}

// Dot product of two length-n rows (raw pointers: the hot loop of the
// correlation and Monte-Carlo passes). Four independent accumulators break
// the add dependency chain (the compiler may not reassociate FP sums itself);
// the summation order is fixed, so results are deterministic.
[[nodiscard]] f64 dot(const f64 *a, const f64 *b, usize n) noexcept {
  f64 acc0 = 0.0;
  f64 acc1 = 0.0;
  f64 acc2 = 0.0;
  f64 acc3 = 0.0;
  usize i = 0;
  for (; i + 4U <= n; i += 4U) {
    acc0 += a[i] * b[i];
    acc1 += a[i + 1U] * b[i + 1U];
    acc2 += a[i + 2U] * b[i + 2U];
    acc3 += a[i + 3U] * b[i + 3U];
  }
  for (; i < n; ++i) {
    acc0 += a[i] * b[i];
  }
  return (acc0 + acc1) + (acc2 + acc3);
}

} // namespace

TrialId trial_id(TrialKind kind, u64 config_hash) noexcept {
  std::array<unsigned char, 9> bytes{};
  bytes[0] = static_cast<unsigned char>(kind);
  std::memcpy(bytes.data() + 1, &config_hash, sizeof(config_hash));
  return TrialId{stable_digest(bytes.data(), bytes.size())};
}

u64 trial_tag(std::string_view name) noexcept {
  if (name.empty()) {
    return 0U;
  }
  std::vector<unsigned char> bytes(name.size() + 8U);
  std::memcpy(bytes.data(), "ATXTAG01", 8U);
  std::memcpy(bytes.data() + 8U, name.data(), name.size());
  const u64 d = stable_digest(bytes.data(), bytes.size());
  return d == 0U ? 1U : d;
}

// ===========================================================================
//  Impl
// ===========================================================================
struct TrialRegistry::Impl {
  TrialRegistryConfig cfg;
  TrialLogFormat fmt{};
  bool exact{};
  usize dim{}; // sketch width actually used: C when exact, else cfg.sketch_dim
  std::vector<usize> bucket;
  std::vector<f64> sign;
  Eigen::MatrixXd gram;    // dim x dim, upper triangle (i <= j) maintained
  Eigen::MatrixXd pending; // dim x kGramBatch sketches not yet folded into gram
  Eigen::Index n_pending{};
  f64 sum_s4{}; // Σ_i ||s_i||⁴ (the Gram's diagonal-pair mass)
  u64 n{};
  u64 n_screened{};
  f64 sr_mean{};
  f64 sr_m2{};
  f64 sr_max{};
  u64 chain{kLegacyChainSeed};    // registry_hash: (id, sharpe) of distinct trials
  u64 content{kContentChainSeed}; // chain head: digest of every log record's bytes
  u64 n_log{};                    // records covered by `content`
  std::array<u64, 3> by_sample{};
  // Partial-window bias bookkeeping (E-16): w_t = Σ_i u_i·1[t in W_i] as a
  // difference array, u_i = 1/sqrt(n_i(n_i-1)); sum_inv_nm1 = Σ_i 1/(n_i-1).
  bool all_full{true};
  std::vector<f64> wdiff;
  f64 sum_inv_nm1{};
  ankerl::unordered_dense::set<u64> ids;
  std::vector<TrialInfo> infos;
  std::vector<f64> sketches; // n x dim unit sketches (keep_sketches)
  std::optional<LogFile> file;
  u64 synced_len{}; // bytes of the log already verified + ingested
  std::vector<unsigned char> rec_buf;
  std::vector<unsigned char> io_buf;
  std::vector<f64> z;
  std::vector<f64> s;

  explicit Impl(const TrialRegistryConfig &c) : cfg{c}, fmt{c.format} {
    exact = cfg.sketch_dim >= cfg.pnl_len;
    dim = exact ? cfg.pnl_len : cfg.sketch_dim;
    if (!exact) {
      bucket.resize(cfg.pnl_len);
      sign.resize(cfg.pnl_len);
      for (usize t = 0; t < cfg.pnl_len; ++t) {
        const u64 h = splitmix64(cfg.sketch_seed ^ splitmix64(static_cast<u64>(t)));
        bucket[t] = static_cast<usize>(h % static_cast<u64>(dim));
        sign[t] = ((h >> 63U) != 0U) ? -1.0 : 1.0;
      }
    }
    const auto d = static_cast<Eigen::Index>(dim);
    gram = Eigen::MatrixXd::Zero(d, d);
    pending = Eigen::MatrixXd::Zero(d, kGramBatch);
    rec_buf.resize(record_bytes());
    wdiff.assign(cfg.pnl_len + 1U, 0.0);
    z.resize(cfg.pnl_len);
    s.resize(dim);
  }

  [[nodiscard]] usize record_bytes() const noexcept {
    return (fmt == TrialLogFormat::V1 ? kRecordFixedV1
            : fmt == TrialLogFormat::V2 ? kRecordFixedV2 : kRecordFixedV3) + 8U * dim;
  }
  void set_format(TrialLogFormat f) {
    fmt = f;
    rec_buf.resize(record_bytes());
  }

  [[nodiscard]] TrialMeta legacy_meta() const noexcept {
    TrialMeta m;
    m.window_start = 0U;
    m.window_end = static_cast<u64>(cfg.pnl_len) - 1U;
    return m;
  }

  [[nodiscard]] Status validate_meta(const TrialMeta &m) const {
    if (!valid_sample(static_cast<u64>(m.sample))) {
      return Err(ErrorCode::InvalidArgument, "TrialRegistry: unknown TrialSample");
    }
    if (m.window_start > m.window_end || m.window_end >= static_cast<u64>(cfg.pnl_len)) {
      return Err(ErrorCode::InvalidArgument, "TrialRegistry: window outside the calendar");
    }
    if (m.window_end - m.window_start + 1U < kMinWindow) {
      return Err(ErrorCode::InvalidArgument, "TrialRegistry: window shorter than 3 periods");
    }
    if (fmt == TrialLogFormat::V1 && !(m == legacy_meta())) {
      return Err(ErrorCode::InvalidArgument,
                 "TrialRegistry: a V1 log records only full-calendar trials without metadata");
    }
    return atx::core::Ok();
  }

  // Standardize `pnl` over its own window to a unit vector z and project it
  // onto the calendar sketch `s`. Err on non-finite values or zero variance.
  [[nodiscard]] Status make_sketch(const TrialMeta &m, std::span<const f64> pnl) {
    f64 mean = 0.0;
    for (const f64 v : pnl) {
      if (!std::isfinite(v)) {
        return Err(ErrorCode::InvalidArgument, "TrialRegistry: non-finite pnl");
      }
      mean += v;
    }
    mean /= static_cast<f64>(pnl.size());
    f64 ss = 0.0;
    for (const f64 v : pnl) {
      ss += (v - mean) * (v - mean);
    }
    if (!(ss > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "TrialRegistry: zero-variance pnl");
    }
    const f64 inv = 1.0 / std::sqrt(ss);
    for (usize k = 0; k < pnl.size(); ++k) {
      z[k] = (pnl[k] - mean) * inv;
    }
    const auto start = static_cast<usize>(m.window_start);
    std::fill(s.begin(), s.end(), 0.0);
    if (exact) {
      for (usize k = 0; k < pnl.size(); ++k) {
        s[start + k] = z[k];
      }
      return atx::core::Ok();
    }
    for (usize k = 0; k < pnl.size(); ++k) {
      s[bucket[start + k]] += sign[start + k] * z[k];
    }
    // Renormalize: a count-sketch preserves norms only to 1 ± O(sqrt(2/d)), so
    // unnormalized s_i·s_j is ρ times a random norm error that the additive
    // 1/d correction cannot remove (identical trials would read as partially
    // independent). The unit sketch estimates the cosine, i.e. ρ, directly.
    f64 s2 = 0.0;
    for (const f64 v : s) {
      s2 += v * v;
    }
    if (s2 > 0.0) {
      const f64 sinv = 1.0 / std::sqrt(s2);
      for (f64 &v : s) {
        v *= sinv;
      }
    }
    return atx::core::Ok();
  }

  // Fold one (already validated / replayed) trial into every aggregate.
  void apply_trial(const TrialInfo &info, const f64 *sk) {
    const u64 id = info.id.value;
    const f64 sharpe = info.sharpe;
    ids.insert(id);
    ++n;
    const f64 delta = sharpe - sr_mean;
    sr_mean += delta / static_cast<f64>(n);
    sr_m2 += delta * (sharpe - sr_mean);
    sr_max = (n == 1U) ? sharpe : std::max(sr_max, sharpe);
    chain = splitmix64(chain ^ id);
    chain = splitmix64(chain ^ std::bit_cast<u64>(sharpe));
    f64 nrm2 = 0.0;
    for (usize i = 0; i < dim; ++i) {
      nrm2 += sk[i] * sk[i];
    }
    sum_s4 += nrm2 * nrm2;
    std::copy(sk, sk + dim, pending.col(n_pending).data());
    ++n_pending;
    if (n_pending == kGramBatch) {
      gram.selfadjointView<Eigen::Upper>().rankUpdate(pending);
      n_pending = 0;
    }
    const auto ws = static_cast<usize>(info.meta.window_start);
    const auto we = static_cast<usize>(info.meta.window_end);
    const f64 len = static_cast<f64>(we - ws + 1U);
    if (ws != 0U || we + 1U != cfg.pnl_len) {
      all_full = false;
    }
    const f64 u = 1.0 / std::sqrt(len * (len - 1.0));
    wdiff[ws] += u;
    wdiff[we + 1U] -= u;
    sum_inv_nm1 += 1.0 / (len - 1.0);
    ++by_sample[static_cast<usize>(info.meta.sample)];
    infos.push_back(info);
    if (cfg.keep_sketches) {
      sketches.insert(sketches.end(), sk, sk + dim);
    }
  }

  void apply_screened(const TrialInfo &info) {
    ids.insert(info.id.value);
    ++n_screened;
    ++by_sample[static_cast<usize>(info.meta.sample)];
    infos.push_back(info);
    chain = splitmix64(chain ^ info.id.value);
    chain = splitmix64(chain ^ static_cast<u64>(TrialObservation::IcScreened));
    chain = splitmix64(chain ^ info.screen_rule_tag);
    chain = splitmix64(chain ^ info.screen_reason_tag);
  }

  void chain_record(const unsigned char *rec, usize len) noexcept {
    content = splitmix64(content ^ stable_digest(rec, len));
    ++n_log;
  }

  // The full Gram including the not-yet-folded batch (upper triangle valid).
  [[nodiscard]] Eigen::MatrixXd current_gram() const {
    Eigen::MatrixXd g = gram;
    if (n_pending > 0) {
      g.selfadjointView<Eigen::Upper>().rankUpdate(pending.leftCols(n_pending));
    }
    return g;
  }

  void encode(const TrialInfo &info, const f64 *sk) {
    if (fmt == TrialLogFormat::V3) {
      const bool screened = info.observation == TrialObservation::IcScreened;
      rec_buf.resize(screened ? kScreenRecordV3 : record_bytes());
      put_u64(rec_buf, 0U, static_cast<u64>(rec_buf.size()));
      put_u64(rec_buf, 8U, static_cast<u64>(info.observation));
      put_u64(rec_buf, 16U, info.id.value);
      put_u64(rec_buf, 24U, info.config_hash);
      put_u64(rec_buf, 32U, static_cast<u64>(info.kind) |
          (static_cast<u64>(info.meta.fidelity) << 8U) |
          (static_cast<u64>(info.meta.sample) << 16U));
      put_u64(rec_buf, 40U, info.meta.window_start);
      put_u64(rec_buf, 48U, info.meta.window_end);
      put_u64(rec_buf, 56U, info.meta.family_tag);
      put_u64(rec_buf, 64U, info.meta.theme_tag);
      if (screened) {
        put_u64(rec_buf, 72U, info.screen_rule_tag);
        put_u64(rec_buf, 80U, info.screen_reason_tag);
      } else {
        put_u64(rec_buf, 72U, std::bit_cast<u64>(info.sharpe));
        std::memcpy(rec_buf.data() + 80U, sk, 8U * dim);
      }
      const usize body = rec_buf.size() - 8U;
      put_u64(rec_buf, body, stable_digest(rec_buf.data(), body));
      return;
    }
    put_u64(rec_buf, 0U, info.id.value);
    put_u64(rec_buf, 8U, info.config_hash);
    put_u64(rec_buf, 16U, std::bit_cast<u64>(info.sharpe));
    usize off = 0U;
    if (fmt == TrialLogFormat::V1) {
      put_u64(rec_buf, 24U, static_cast<u64>(info.kind));
      off = 32U;
    } else {
      const u64 packed = static_cast<u64>(info.kind) |
                         (static_cast<u64>(info.meta.fidelity) << 8U) |
                         (static_cast<u64>(info.meta.sample) << 16U);
      put_u64(rec_buf, 24U, packed);
      put_u64(rec_buf, 32U, info.meta.window_start);
      put_u64(rec_buf, 40U, info.meta.window_end);
      put_u64(rec_buf, 48U, info.meta.family_tag);
      put_u64(rec_buf, 56U, info.meta.theme_tag);
      off = 64U;
    }
    std::memcpy(rec_buf.data() + off, sk, 8U * dim);
    const usize body = off + 8U * dim;
    put_u64(rec_buf, body, stable_digest(rec_buf.data(), body));
  }

  // Validate one complete record and decode it (sketch into `s`). False on a
  // bad checksum or any field outside its domain.
  [[nodiscard]] bool decode(const unsigned char *r, TrialInfo &out) {
    const usize rb = record_bytes();
    const usize body = rb - 8U;
    if (get_u64(r + body) != stable_digest(r, body)) {
      return false;
    }
    out.id = TrialId{get_u64(r)};
    out.config_hash = get_u64(r + 8U);
    out.sharpe = std::bit_cast<f64>(get_u64(r + 16U));
    const u64 packed = get_u64(r + 24U);
    usize off = 0U;
    if (fmt == TrialLogFormat::V1) {
      if (!valid_kind(packed)) {
        return false;
      }
      out.kind = static_cast<TrialKind>(packed);
      out.meta = legacy_meta();
      off = 32U;
    } else {
      const u64 kind = packed & 0xffU;
      const u64 sample = (packed >> 16U) & 0xffU;
      if ((packed >> 24U) != 0U || !valid_kind(kind) || !valid_sample(sample)) {
        return false;
      }
      out.kind = static_cast<TrialKind>(kind);
      out.meta.fidelity = static_cast<u8>((packed >> 8U) & 0xffU);
      out.meta.sample = static_cast<TrialSample>(sample);
      out.meta.window_start = get_u64(r + 32U);
      out.meta.window_end = get_u64(r + 40U);
      out.meta.family_tag = get_u64(r + 48U);
      out.meta.theme_tag = get_u64(r + 56U);
      const TrialMeta &m = out.meta;
      if (m.window_start > m.window_end || m.window_end >= static_cast<u64>(cfg.pnl_len) ||
          m.window_end - m.window_start + 1U < kMinWindow) {
        return false;
      }
      off = 64U;
    }
    if (!std::isfinite(out.sharpe) || !(trial_id(out.kind, out.config_hash) == out.id)) {
      return false;
    }
    std::memcpy(s.data(), r + off, 8U * dim);
    return true;
  }

  [[nodiscard]] bool decode_v3(const unsigned char *r, usize bytes, TrialInfo &out) {
    const u64 observation = get_u64(r + 8U);
    if (observation > static_cast<u64>(TrialObservation::IcScreened) ||
        get_u64(r) != static_cast<u64>(bytes) ||
        bytes != (observation == 1U ? kScreenRecordV3 : record_bytes()) ||
        get_u64(r + bytes - 8U) != stable_digest(r, bytes - 8U)) return false;
    out.id = TrialId{get_u64(r + 16U)};
    out.config_hash = get_u64(r + 24U);
    const u64 packed = get_u64(r + 32U);
    const u64 kind = packed & 0xffU;
    const u64 sample = (packed >> 16U) & 0xffU;
    if ((packed >> 24U) != 0U || !valid_kind(kind) || !valid_sample(sample)) return false;
    out.kind = static_cast<TrialKind>(kind);
    out.meta.fidelity = static_cast<u8>((packed >> 8U) & 0xffU);
    out.meta.sample = static_cast<TrialSample>(sample);
    out.meta.window_start = get_u64(r + 40U);
    out.meta.window_end = get_u64(r + 48U);
    out.meta.family_tag = get_u64(r + 56U);
    out.meta.theme_tag = get_u64(r + 64U);
    if (!validate_meta(out.meta) || !(trial_id(out.kind, out.config_hash) == out.id)) return false;
    out.observation = static_cast<TrialObservation>(observation);
    if (out.observation == TrialObservation::IcScreened) {
      out.sharpe = std::numeric_limits<f64>::quiet_NaN();
      out.screen_rule_tag = get_u64(r + 72U);
      out.screen_reason_tag = get_u64(r + 80U);
      return out.screen_rule_tag != 0U && out.screen_reason_tag != 0U;
    }
    out.sharpe = std::bit_cast<f64>(get_u64(r + 72U));
    if (!std::isfinite(out.sharpe)) return false;
    std::memcpy(s.data(), r + 80U, 8U * dim);
    return std::all_of(s.begin(), s.end(), [](f64 x) { return std::isfinite(x); });
  }

  [[nodiscard]] std::vector<unsigned char> header_bytes() const {
    std::vector<unsigned char> h(kHeaderBytes, 0U);
    const auto &magic = fmt == TrialLogFormat::V1 ? kMagicV1
                         : fmt == TrialLogFormat::V2 ? kMagicV2 : kMagicV3;
    std::memcpy(h.data(), magic.data(), magic.size());
    put_u64(h, 8U, static_cast<u64>(fmt));
    put_u64(h, 16U, static_cast<u64>(cfg.pnl_len));
    put_u64(h, 24U, static_cast<u64>(cfg.sketch_dim));
    put_u64(h, 32U, cfg.sketch_seed);
    put_u64(h, 40U, stable_digest(h.data(), 40U));
    return h;
  }

  // Parse + validate an existing header; sets the log format.
  [[nodiscard]] Status adopt_header(const unsigned char *h) {
    if (std::memcmp(h, kMagicV1.data(), kMagicStem) != 0 ||
        get_u64(h + 40U) != stable_digest(h, 40U)) {
      return Err(ErrorCode::ParseError, "TrialRegistry: bad header");
    }
    const char digit = static_cast<char>(h[kMagicStem]);
    const u64 version = get_u64(h + 8U);
    TrialLogFormat f{};
    if (digit == '1' && version == 1U) {
      f = TrialLogFormat::V1;
    } else if (digit == '2' && version == 2U) {
      f = TrialLogFormat::V2;
    } else if (digit == '3' && version == 3U) {
      f = TrialLogFormat::V3;
    } else {
      return Err(ErrorCode::ParseError, "TrialRegistry: unknown log version");
    }
    if (get_u64(h + 16U) != static_cast<u64>(cfg.pnl_len) ||
        get_u64(h + 24U) != static_cast<u64>(cfg.sketch_dim) ||
        get_u64(h + 32U) != cfg.sketch_seed) {
      return Err(ErrorCode::InvalidArgument,
                 "TrialRegistry: config does not match the registry file header");
    }
    set_format(f);
    return atx::core::Ok();
  }

  // V3 frames carry their exact byte length, allowing a metadata-only record to
  // contain no Sharpe or sketch. Existing V1/V2 fixed-record replay is untouched.
  [[nodiscard]] Status sync_v3(u64 size, const TrialChainHead *anchor) {
    const LogFile &f = *file;
    bool captured = anchor != nullptr && n_log == anchor->records;
    u64 anchor_value = captured ? content : 0U;
    while (size - synced_len >= 8U) {
      std::array<unsigned char, 8> prefix{};
      ATX_TRY_VOID(f.read_at(synced_len, prefix.data(), prefix.size()));
      const u64 length = get_u64(prefix.data());
      if (length != kScreenRecordV3 && length != record_bytes()) {
        return Err(ErrorCode::ParseError, "TrialRegistry: invalid V3 frame length");
      }
      if (length > size - synced_len) break; // torn final append
      const auto bytes = static_cast<usize>(length);
      io_buf.resize(bytes);
      ATX_TRY_VOID(f.read_at(synced_len, io_buf.data(), bytes));
      TrialInfo info;
      if (!decode_v3(io_buf.data(), bytes, info)) {
        if (length < size - synced_len) {
          return Err(ErrorCode::ParseError, "TrialRegistry: corrupt V3 record mid-log");
        }
        break; // complete-size torn tail; verify anchor before truncating
      }
      if (!ids.contains(info.id.value)) {
        if (info.observation == TrialObservation::IcScreened) apply_screened(info);
        else apply_trial(info, s.data());
      } else {
        const auto prior = std::find_if(infos.begin(), infos.end(), [&](const TrialInfo &v) {
          return v.id == info.id;
        });
        if (prior->observation != info.observation ||
            prior->screen_rule_tag != info.screen_rule_tag ||
            prior->screen_reason_tag != info.screen_reason_tag) {
          return Err(ErrorCode::ParseError, "TrialRegistry: conflicting V3 observation identity");
        }
      }
      chain_record(io_buf.data(), bytes);
      synced_len += length;
      if (anchor != nullptr && !captured && n_log == anchor->records) {
        captured = true;
        anchor_value = content;
      }
    }
    if (anchor != nullptr && (!captured || anchor_value != anchor->head)) {
      return Err(ErrorCode::ParseError, "TrialRegistry: V3 log does not match its anchored chain head");
    }
    if (synced_len != size) ATX_TRY_VOID(f.truncate(synced_len));
    return atx::core::Ok();
  }

  // Ingest every complete record in [synced_len, EOF), verifying each, and
  // truncate a torn tail. MUST be called with the log lock held: under the
  // lock no writer is mid-append, so a partial / bad LAST record can only be
  // the torn tail of a crashed writer. `anchor` (open() only) is verified
  // BEFORE anything is truncated.
  [[nodiscard]] Status sync(const TrialChainHead *anchor) {
    const LogFile &f = *file;
    ATX_TRY(const u64 size, f.size());
    if (size < synced_len) {
      return Err(ErrorCode::ParseError,
                 "TrialRegistry: the log shrank (truncated by another writer?)");
    }
    if (synced_len == 0U) {
      if (size < kHeaderBytes) {
        // Empty, or a torn creation: nothing was ever committed.
        if (anchor != nullptr && (anchor->records != 0U || anchor->head != content)) {
          return Err(ErrorCode::ParseError,
                     "TrialRegistry: the log holds fewer records than its anchored chain head");
        }
        if (size > 0U) {
          ATX_TRY_VOID(f.truncate(0U));
        }
        set_format(cfg.format);
        const std::vector<unsigned char> h = header_bytes();
        ATX_TRY_VOID(f.append(h.data(), h.size()));
        synced_len = kHeaderBytes;
        return atx::core::Ok();
      }
      std::array<unsigned char, kHeaderBytes> head{};
      ATX_TRY_VOID(f.read_at(0U, head.data(), head.size()));
      ATX_TRY_VOID(adopt_header(head.data()));
      synced_len = kHeaderBytes;
    }
    if (fmt == TrialLogFormat::V3) return sync_v3(size, anchor);
    const usize rb = record_bytes();
    const u64 chunk_records = std::max<u64>(1U, static_cast<u64>(kIoChunk / rb));
    bool captured = anchor != nullptr && n_log == anchor->records;
    u64 anchor_value = captured ? content : 0U;
    bool torn = false;
    // Bounded: every outer iteration consumes >= 1 complete record or stops.
    while (!torn && synced_len + rb <= size) {
      const u64 take = std::min<u64>((size - synced_len) / rb, chunk_records);
      io_buf.resize(static_cast<usize>(take) * rb);
      ATX_TRY_VOID(f.read_at(synced_len, io_buf.data(), io_buf.size()));
      for (u64 k = 0; k < take; ++k) {
        const unsigned char *r = io_buf.data() + static_cast<usize>(k) * rb;
        TrialInfo info;
        if (!decode(r, info)) {
          // A crash can only tear the LAST append. A bad complete record that
          // is the final bytes of the file is that torn tail (dropped below); a
          // bad record with more data after it is mid-log corruption, and
          // silently truncating would delete acknowledged trials and undercount
          // N — so refuse and leave the file untouched.
          if (synced_len + rb < size) {
            return Err(ErrorCode::ParseError,
                       "TrialRegistry: corrupt record mid-log (refusing to truncate "
                       "acknowledged trials)");
          }
          torn = true;
          break;
        }
        if (ids.find(info.id.value) == ids.end()) {
          apply_trial(info, s.data());
        }
        chain_record(r, rb);
        synced_len += rb;
        if (anchor != nullptr && !captured && n_log == anchor->records) {
          captured = true;
          anchor_value = content;
        }
      }
    }
    if (anchor != nullptr) {
      if (!captured) {
        return Err(ErrorCode::ParseError,
                   "TrialRegistry: the log holds fewer records than its anchored chain head "
                   "(records were removed)");
      }
      if (anchor_value != anchor->head) {
        return Err(ErrorCode::ParseError,
                   "TrialRegistry: the log does not match its anchored chain head (a record "
                   "was edited)");
      }
    }
    if (synced_len != size) {
      ATX_TRY_VOID(f.truncate(synced_len));
    }
    return atx::core::Ok();
  }

  // Register a validated trial (lock held + synced when durable).
  [[nodiscard]] Result<RecordOutcome> insert(TrialId id, TrialKind kind, u64 config_hash,
                                             const TrialMeta &meta, std::span<const f64> pnl,
                                             f64 sharpe) {
    if (ids.find(id.value) != ids.end()) {
      if (fmt == TrialLogFormat::V3) {
        const auto prior = std::find_if(infos.begin(), infos.end(), [&](const TrialInfo &v) {
          return v.id == id;
        });
        if (prior->observation != TrialObservation::FullPnl)
          return Err(ErrorCode::InvalidArgument, "TrialRegistry: identity already belongs to a screened trial");
      }
      return atx::core::Ok(RecordOutcome{id, false});
    }
    ATX_TRY_VOID(make_sketch(meta, pnl));
    TrialInfo info;
    info.id = id;
    info.kind = kind;
    info.config_hash = config_hash;
    info.sharpe = sharpe;
    info.meta = meta;
    encode(info, s.data());
    if (file.has_value()) {
      const Status st = file->append(rec_buf.data(), rec_buf.size());
      if (!st) {
        (void)file->truncate(synced_len); // best effort: drop a partial record
        return Err(ErrorCode::IoError, "TrialRegistry: durable append failed");
      }
      synced_len += rec_buf.size();
    }
    apply_trial(info, s.data());
    chain_record(rec_buf.data(), rec_buf.size());
    return atx::core::Ok(RecordOutcome{id, true});
  }

  [[nodiscard]] Result<RecordOutcome> insert_screened(TrialKind kind, u64 config_hash,
                                                      const TrialMeta &meta, u64 rule, u64 reason) {
    if (fmt != TrialLogFormat::V3) {
      return Err(ErrorCode::InvalidArgument, "TrialRegistry: screened observations require explicit V3");
    }
    const TrialId id = trial_id(kind, config_hash);
    if (ids.contains(id.value)) {
      const auto prior = std::find_if(infos.begin(), infos.end(), [&](const TrialInfo &v) {
        return v.id == id;
      });
      if (prior->observation != TrialObservation::IcScreened ||
          prior->screen_rule_tag != rule || prior->screen_reason_tag != reason)
        return Err(ErrorCode::InvalidArgument,
                   "TrialRegistry: changed screening recipe/reason requires a distinct config hash");
      return atx::core::Ok(RecordOutcome{id, false});
    }
    TrialInfo info;
    info.id = id;
    info.kind = kind;
    info.config_hash = config_hash;
    info.sharpe = std::numeric_limits<f64>::quiet_NaN();
    info.meta = meta;
    info.observation = TrialObservation::IcScreened;
    info.screen_rule_tag = rule;
    info.screen_reason_tag = reason;
    encode(info, nullptr);
    if (file.has_value()) {
      if (!file->append(rec_buf.data(), rec_buf.size())) {
        (void)file->truncate(synced_len);
        return Err(ErrorCode::IoError, "TrialRegistry: screened durable append failed");
      }
      synced_len += rec_buf.size();
    }
    apply_screened(info);
    chain_record(rec_buf.data(), rec_buf.size());
    return atx::core::Ok(RecordOutcome{id, true});
  }
};

namespace {

atx::core::Status validate_cfg(const TrialRegistryConfig &cfg) {
  if (cfg.pnl_len < kMinWindow) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: pnl_len must be >= 3");
  }
  if (cfg.sketch_dim < kMinSketchDim) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: sketch_dim must be >= 8");
  }
  if (cfg.format != TrialLogFormat::V1 && cfg.format != TrialLogFormat::V2 &&
      cfg.format != TrialLogFormat::V3) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: unknown TrialLogFormat");
  }
  return atx::core::Ok();
}

} // namespace

TrialRegistry::TrialRegistry(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
TrialRegistry::TrialRegistry(TrialRegistry &&) noexcept = default;
TrialRegistry &TrialRegistry::operator=(TrialRegistry &&) noexcept = default;
TrialRegistry::~TrialRegistry() = default;

Result<TrialRegistry> TrialRegistry::in_memory(const TrialRegistryConfig &cfg) {
  ATX_TRY_VOID(validate_cfg(cfg));
  return TrialRegistry{std::make_unique<Impl>(cfg)};
}

Result<TrialRegistry> TrialRegistry::open(const std::filesystem::path &path,
                                          const TrialRegistryConfig &cfg) {
  return open_impl(path, cfg, nullptr);
}

Result<TrialRegistry> TrialRegistry::open(const std::filesystem::path &path,
                                          const TrialRegistryConfig &cfg,
                                          const TrialChainHead &anchor) {
  return open_impl(path, cfg, &anchor);
}

Result<TrialRegistry> TrialRegistry::open_impl(const std::filesystem::path &path,
                                               const TrialRegistryConfig &cfg,
                                               const TrialChainHead *anchor) {
  ATX_TRY_VOID(validate_cfg(cfg));
  auto impl = std::make_unique<Impl>(cfg);
  ATX_TRY(LogFile f, LogFile::open(path));
  impl->file.emplace(std::move(f));
  {
    ATX_TRY(const LogLock lock, LogLock::acquire(*impl->file));
    ATX_TRY_VOID(impl->sync(anchor));
  }
  return TrialRegistry{std::move(impl)};
}

Result<RecordOutcome> TrialRegistry::record(TrialKind kind, u64 config_hash,
                                            std::span<const f64> pnl, f64 sharpe) {
  if (pnl.size() != impl_->cfg.pnl_len) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: pnl length != pnl_len");
  }
  return record(kind, config_hash, impl_->legacy_meta(), pnl, sharpe);
}

Result<RecordOutcome> TrialRegistry::record(TrialKind kind, u64 config_hash, const TrialMeta &meta,
                                            std::span<const f64> pnl, f64 sharpe) {
  Impl &im = *impl_;
  if (!valid_kind(static_cast<u64>(kind))) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: unknown TrialKind");
  }
  ATX_TRY_VOID(im.validate_meta(meta));
  if (pnl.size() != static_cast<usize>(meta.window_end - meta.window_start + 1U)) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: pnl length != window length");
  }
  if (!std::isfinite(sharpe)) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: non-finite sharpe");
  }
  const TrialId id = trial_id(kind, config_hash);
  if (!im.file.has_value()) {
    return im.insert(id, kind, config_hash, meta, pnl, sharpe);
  }
  ATX_TRY(const LogLock lock, LogLock::acquire(*im.file));
  // Catch up with every record other handles appended, then decide the
  // content address against that fresh state.
  ATX_TRY_VOID(im.sync(nullptr));
  return im.insert(id, kind, config_hash, meta, pnl, sharpe);
}

Result<RecordOutcome> TrialRegistry::record_screened(TrialKind kind, u64 config_hash,
                                                     const TrialMeta &meta, u64 rule_tag,
                                                     u64 reason_tag) {
  Impl &im = *impl_;
  if (!valid_kind(static_cast<u64>(kind)) || rule_tag == 0U || reason_tag == 0U) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: invalid screened observation identity");
  }
  ATX_TRY_VOID(im.validate_meta(meta));
  if (!im.file.has_value()) return im.insert_screened(kind, config_hash, meta, rule_tag, reason_tag);
  ATX_TRY(const LogLock lock, LogLock::acquire(*im.file));
  ATX_TRY_VOID(im.sync(nullptr));
  return im.insert_screened(kind, config_hash, meta, rule_tag, reason_tag);
}

Result<u64> TrialRegistry::refresh() {
  Impl &im = *impl_;
  if (!im.file.has_value()) {
    return atx::core::Ok(u64{0});
  }
  const u64 before = im.n_log;
  ATX_TRY(const LogLock lock, LogLock::acquire(*im.file));
  ATX_TRY_VOID(im.sync(nullptr));
  return atx::core::Ok(im.n_log - before);
}

TrialSummary TrialRegistry::summary() const {
  const Impl &im = *impl_;
  TrialSummary out;
  out.n_raw = im.n + im.n_screened;
  out.n_full_pnl = im.n;
  out.n_screened = im.n_screened;
  out.pnl_statistics_complete = im.n_screened == 0U;
  if (!out.pnl_statistics_complete) {
    out.n_eff = std::numeric_limits<f64>::quiet_NaN();
    out.n_eff_uncorrected = std::numeric_limits<f64>::quiet_NaN();
  }
  out.pnl_len = im.cfg.pnl_len;
  out.registry_hash = im.chain;
  out.n_unspecified = im.by_sample[static_cast<usize>(TrialSample::Unspecified)];
  out.n_in_sample = im.by_sample[static_cast<usize>(TrialSample::InSample)];
  out.n_out_of_sample = im.by_sample[static_cast<usize>(TrialSample::OutOfSample)];
  if (im.n == 0U) {
    if (im.n_screened != 0U) {
      out.mean_sr = out.var_sr = out.max_sr = std::numeric_limits<f64>::quiet_NaN();
    }
    return out;
  }
  out.mean_sr = im.sr_mean;
  out.var_sr = im.n >= 2U ? im.sr_m2 / static_cast<f64>(im.n - 1U) : 0.0;
  out.max_sr = im.sr_max;

  // ||M||_F² from the upper triangle, and tr M.
  const Eigen::MatrixXd g = im.current_gram();
  f64 frob2 = 0.0;
  f64 trace = 0.0;
  const auto d = static_cast<Eigen::Index>(im.dim);
  for (Eigen::Index j = 0; j < d; ++j) {
    f64 off = 0.0;
    for (Eigen::Index i = 0; i < j; ++i) {
      off += g(i, j) * g(i, j);
    }
    trace += g(j, j);
    frob2 += g(j, j) * g(j, j) + 2.0 * off;
  }
  out.n_eff_uncorrected = frob2 > 0.0 ? (trace * trace) / frob2 : 0.0;

  const f64 nf = static_cast<f64>(im.n);
  const f64 pairs = nf * (nf - 1.0);
  // E[ρ̂²] under independence: sampling noise, plus ≈ 1/d sketch noise. Full
  // windows: the legacy 1/(C−1). Partial windows (E-16): the pair average of
  // L_ij / sqrt(n_i(n_i−1) n_j(n_j−1)) = (Σ_t w_t² − Σ_i 1/(n_i−1)) / pairs.
  f64 c = 1.0 / static_cast<f64>(im.cfg.pnl_len - 1U);
  if (!im.all_full) {
    f64 run = 0.0;
    f64 sw2 = 0.0;
    for (usize t = 0; t < im.cfg.pnl_len; ++t) {
      run += im.wdiff[t];
      sw2 += run * run;
    }
    c = pairs > 0.0 ? std::max(0.0, sw2 - im.sum_inv_nm1) / pairs : 0.0;
  }
  if (!im.exact) {
    c += 1.0 / static_cast<f64>(im.dim);
  }
  c = std::min(c, 0.5);
  const f64 s_off = std::max(0.0, frob2 - im.sum_s4);
  const f64 off_corr = std::max(0.0, (s_off - pairs * c) / (1.0 - c));
  const f64 total = nf + off_corr;
  out.n_eff = std::clamp((nf * nf) / total, 1.0, nf);
  out.n_eff_full_pnl = out.n_eff;
  out.n_eff_uncorrected_full_pnl = out.n_eff_uncorrected;
  if (!out.pnl_statistics_complete) {
    out.n_eff = std::numeric_limits<f64>::quiet_NaN();
    out.n_eff_uncorrected = std::numeric_limits<f64>::quiet_NaN();
  }
  return out;
}

u64 TrialRegistry::size() const noexcept { return impl_->n + impl_->n_screened; }

bool TrialRegistry::contains(TrialId id) const {
  return impl_->ids.find(id.value) != impl_->ids.end();
}

const TrialRegistryConfig &TrialRegistry::config() const noexcept { return impl_->cfg; }

TrialLogFormat TrialRegistry::format() const noexcept { return impl_->fmt; }

const std::vector<TrialInfo> &TrialRegistry::trials() const noexcept { return impl_->infos; }

TrialChainHead TrialRegistry::chain_head() const noexcept {
  return TrialChainHead{impl_->n_log, impl_->content};
}

Result<std::vector<f64>> TrialRegistry::correlation() const {
  const Impl &im = *impl_;
  if (im.n_screened != 0U) {
    return Err(ErrorCode::InvalidArgument,
               "TrialRegistry: correlation unavailable; screened trials have no observed P&L");
  }
  if (!im.cfg.keep_sketches) {
    return Err(ErrorCode::InvalidArgument,
               "TrialRegistry: correlation needs keep_sketches (sketches were not retained)");
  }
  const auto n = static_cast<usize>(im.n);
  std::vector<f64> out(n * n, 0.0);
  const f64 *base = im.sketches.data();
  for (usize i = 0; i < n; ++i) {
    const f64 *ri = base + i * im.dim;
    out[i * n + i] = 1.0; // unit sketches: 1 up to rounding, pinned exactly
    for (usize j = 0; j < i; ++j) {
      const f64 r = std::clamp(dot(ri, base + j * im.dim, im.dim), -1.0, 1.0);
      out[i * n + j] = r;
      out[j * n + i] = r;
    }
  }
  return out;
}

Result<McMaxNull> TrialRegistry::mc_max_null(usize draws, u64 seed) const {
  const Impl &im = *impl_;
  if (im.n_screened != 0U) {
    return Err(ErrorCode::InvalidArgument,
               "TrialRegistry: MC unavailable; screened trials have no observed P&L");
  }
  if (!im.cfg.keep_sketches) {
    return Err(ErrorCode::InvalidArgument,
               "TrialRegistry: mc_max_null needs keep_sketches (sketches were not retained)");
  }
  if (im.n == 0U) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: mc_max_null on an empty registry");
  }
  std::vector<f64> null_sd;
  null_sd.reserve(im.infos.size());
  for (const TrialInfo &t : im.infos) {
    const f64 len = static_cast<f64>(t.meta.window_end - t.meta.window_start + 1U);
    null_sd.push_back(1.0 / std::sqrt(len));
  }
  return mc_max_sharpe_null(im.sketches, static_cast<usize>(im.n), im.dim, null_sd, draws, seed);
}

Result<TrialAccounting> TrialRegistry::accounting(const TrialAccountingConfig &cfg) const {
  const Impl &im = *impl_;
  if (im.n_screened != 0U) {
    return Err(ErrorCode::InvalidArgument,
               "TrialRegistry: cluster/MC accounting unavailable; screened trial correlation is unknown");
  }
  if (im.n == 0U) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: accounting on an empty registry");
  }
  if (im.n > static_cast<u64>(cfg.max_trials)) {
    return Err(ErrorCode::InvalidArgument,
               "TrialRegistry: accounting past max_trials (the n x n correlation is O(n^2))");
  }
  ATX_TRY(const std::vector<f64> corr, correlation());
  const auto n = static_cast<usize>(im.n);
  TrialAccounting out;
  out.n_raw = im.n;
  ATX_TRY(out.clusters, onc_cluster(corr, n, cfg.onc));
  std::vector<f64> sharpes;
  sharpes.reserve(n);
  for (const TrialInfo &t : im.infos) {
    sharpes.push_back(t.sharpe);
  }
  ATX_TRY(out.cluster_sharpes, cluster_representative_sharpes(corr, n, sharpes, out.clusters));
  const usize k = out.cluster_sharpes.size();
  if (k >= 2U) {
    f64 m = 0.0;
    for (const f64 v : out.cluster_sharpes) {
      m += v;
    }
    m /= static_cast<f64>(k);
    f64 ss = 0.0;
    for (const f64 v : out.cluster_sharpes) {
      ss += (v - m) * (v - m);
    }
    out.var_sr_clusters = ss / static_cast<f64>(k - 1U);
  }
  const TrialSummary sum = summary();
  out.var_sr = sum.var_sr;
  out.n_eff = sum.n_eff;
  ATX_TRY(out.mc, mc_max_null(cfg.mc_draws, cfg.mc_seed));
  return out;
}

// ===========================================================================
//  Chain-head sidecar codec
// ===========================================================================
atx::core::Status write_chain_head(const std::filesystem::path &path, const TrialChainHead &head) {
  std::string line{kHeadTag};
  line += ' ';
  line += to_hex(head.records);
  line += ' ';
  line += to_hex(head.head);
  line += ' ';
  line += to_hex(head_check(head));
  line += '\n';
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.flush();
    if (!out) {
      return Err(ErrorCode::IoError, "write_chain_head: cannot write " + tmp.string());
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "write_chain_head: cannot publish " + path.string());
  }
  return atx::core::Ok();
}

Result<TrialChainHead> read_chain_head(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Err(ErrorCode::IoError, "read_chain_head: cannot read " + path.string());
  }
  std::string line;
  std::getline(in, line);
  std::array<std::string_view, 4> tok{};
  std::string_view rest{line};
  for (usize i = 0; i < tok.size(); ++i) {
    const usize sp = rest.find(' ');
    tok[i] = rest.substr(0, sp);
    rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1U);
  }
  TrialChainHead h;
  u64 check = 0;
  if (tok[0] != kHeadTag || !rest.empty() || !parse_hex(tok[1], h.records) ||
      !parse_hex(tok[2], h.head) || !parse_hex(tok[3], check) || check != head_check(h)) {
    return Err(ErrorCode::ParseError, "read_chain_head: malformed chain head " + path.string());
  }
  return h;
}

// ===========================================================================
//  ONC-style clustering (trial_clusters.hpp)
// ===========================================================================
namespace {

// ONC correlation distance.
[[nodiscard]] f64 onc_distance(f64 r) noexcept { return std::sqrt(std::max(0.0, 0.5 * (1.0 - r))); }

// Squared Euclidean distance between unit vectors with correlation r.
[[nodiscard]] f64 unit_d2(f64 r) noexcept { return std::max(0.0, 2.0 - 2.0 * r); }

struct Partition {
  std::vector<u32> labels;
  usize k{};
  f64 sse{};
};

// Draw an index with probability ∝ w (w >= 0, total > 0); w.size() when none.
[[nodiscard]] usize draw_weighted(const std::vector<f64> &w, f64 total, SplitMixRng &rng) {
  const f64 u = rng.uniform() * total;
  f64 cum = 0.0;
  usize pick = w.size();
  for (usize i = 0; i < w.size(); ++i) {
    if (w[i] > 0.0) {
      cum += w[i];
      pick = i;
      if (cum >= u) {
        break;
      }
    }
  }
  return pick;
}

// Seeding of kernel k-means.
//  * FarthestFirst: a random first center, then repeatedly the point farthest
//    from every chosen center (ties -> lowest index). On a block model it
//    places one seed per block whenever blocks are tighter than they are far
//    apart, which plain k-means++ does with probability << 1 (at within /
//    across d² = 1 : 2 and G = 10 blocks of 12, about 1%).
//  * GreedyPlusPlus: k-means++ with 2 + floor(ln k) local candidates per step,
//    keeping the one that lowers the potential most (Arthur & Vassilvitskii;
//    the scikit-learn default).
enum class Seeding : u8 { FarthestFirst = 0, GreedyPlusPlus = 1 };

// Kernel k-means (Lloyd) on unit vectors given only their Gram R (n x n):
// ||x_i − μ_c||² = 1 − 2·A_ic/m_c + B_c/m_c², A_ic = Σ_{j∈c} R_ij,
// B_c = Σ_{i,j∈c} R_ij. Seeding on d²(i,j) = 2 − 2R_ij (see Seeding). Every
// cluster is kept non-empty. O(n² + n·k) per iteration, <= max_iter iterations.
[[nodiscard]] Partition kernel_kmeans(const f64 *R, usize n, usize k, usize max_iter,
                                      SplitMixRng &rng, Seeding seeding) {
  std::vector<usize> centers;
  centers.reserve(k);
  std::vector<f64> mind2(n, 0.0);
  centers.push_back(static_cast<usize>(rng.next() % static_cast<u64>(n)));
  for (usize i = 0; i < n; ++i) {
    mind2[i] = unit_d2(R[i * n + centers[0]]);
  }
  std::vector<unsigned char> is_center(n, 0U);
  is_center[centers[0]] = 1U;
  const usize n_local = 2U + static_cast<usize>(std::log(static_cast<f64>(k)));
  for (usize c = 1; c < k; ++c) {
    f64 total = 0.0;
    for (const f64 v : mind2) {
      total += v;
    }
    usize pick = n;
    if (total > 0.0 && seeding == Seeding::FarthestFirst) {
      f64 best = 0.0;
      for (usize i = 0; i < n; ++i) {
        if (mind2[i] > best) {
          best = mind2[i];
          pick = i;
        }
      }
    } else if (total > 0.0) {
      f64 best_pot = std::numeric_limits<f64>::infinity();
      for (usize t = 0; t < n_local; ++t) {
        const usize cand = draw_weighted(mind2, total, rng);
        if (cand == n) {
          break;
        }
        f64 pot = 0.0;
        for (usize i = 0; i < n; ++i) {
          pot += std::min(mind2[i], unit_d2(R[i * n + cand]));
        }
        if (pot < best_pot) {
          best_pot = pot;
          pick = cand;
        }
      }
    }
    if (pick == n) { // every point coincides with a center: take the next free index
      for (usize i = 0; i < n; ++i) {
        if (is_center[i] == 0U) {
          pick = i;
          break;
        }
      }
    }
    centers.push_back(pick);
    is_center[pick] = 1U;
    for (usize i = 0; i < n; ++i) {
      mind2[i] = std::min(mind2[i], unit_d2(R[i * n + pick]));
    }
  }
  Partition p;
  p.k = k;
  p.labels.assign(n, 0U);
  for (usize i = 0; i < n; ++i) {
    f64 best = std::numeric_limits<f64>::infinity();
    for (usize c = 0; c < k; ++c) {
      const f64 d2 = unit_d2(R[i * n + centers[c]]);
      if (d2 < best) {
        best = d2;
        p.labels[i] = static_cast<u32>(c);
      }
    }
  }
  std::vector<f64> A(n * k, 0.0);
  std::vector<f64> B(k, 0.0);
  std::vector<usize> cnt(k, 0U);
  std::vector<u32> next(n, 0U);
  std::vector<f64> bestd(n, 0.0);
  const u32 *lab = p.labels.data();
  for (usize iter = 0; iter < max_iter; ++iter) {
    std::fill(A.begin(), A.end(), 0.0);
    std::fill(B.begin(), B.end(), 0.0);
    std::fill(cnt.begin(), cnt.end(), usize{0});
    for (usize j = 0; j < n; ++j) {
      ++cnt[lab[j]];
    }
    for (usize i = 0; i < n; ++i) {
      const f64 *row = R + i * n;
      f64 *a = A.data() + i * k;
      for (usize j = 0; j < n; ++j) {
        a[lab[j]] += row[j];
      }
    }
    for (usize i = 0; i < n; ++i) {
      B[lab[i]] += A[i * k + lab[i]];
    }
    bool changed = false;
    f64 sse = 0.0;
    for (usize i = 0; i < n; ++i) {
      f64 best = std::numeric_limits<f64>::infinity();
      u32 bc = lab[i];
      for (usize c = 0; c < k; ++c) {
        if (cnt[c] == 0U) {
          continue;
        }
        const f64 m = static_cast<f64>(cnt[c]);
        const f64 d2 = std::max(0.0, 1.0 - 2.0 * A[i * k + c] / m + B[c] / (m * m));
        if (d2 < best) {
          best = d2;
          bc = static_cast<u32>(c);
        }
      }
      next[i] = bc;
      bestd[i] = best;
      sse += best;
      changed = changed || bc != lab[i];
    }
    // Keep every cluster non-empty: an empty cluster takes the point farthest
    // from its centroid among clusters that can spare one (bounded by k).
    std::vector<usize> ncnt(k, 0U);
    for (usize i = 0; i < n; ++i) {
      ++ncnt[next[i]];
    }
    for (usize c = 0; c < k; ++c) {
      if (ncnt[c] != 0U) {
        continue;
      }
      usize victim = n;
      f64 victim_d = -1.0;
      for (usize i = 0; i < n; ++i) {
        if (ncnt[next[i]] > 1U && bestd[i] > victim_d) {
          victim_d = bestd[i];
          victim = i;
        }
      }
      if (victim == n) {
        break; // k > distinct points cannot happen (k <= n - 1), defensive
      }
      sse -= bestd[victim];
      --ncnt[next[victim]];
      next[victim] = static_cast<u32>(c);
      ncnt[c] = 1U;
      bestd[victim] = 0.0;
      changed = true;
    }
    p.sse = sse;
    std::copy(next.begin(), next.end(), p.labels.begin());
    if (!changed) {
      break;
    }
  }
  return p;
}

// Silhouette of every point under ONC distance (singleton cluster -> 0).
[[nodiscard]] std::vector<f64> silhouettes(const f64 *R, usize n, const std::vector<u32> &lab,
                                           usize k) {
  std::vector<usize> cnt(k, 0U);
  for (const u32 l : lab) {
    ++cnt[l];
  }
  std::vector<f64> out(n, 0.0);
  std::vector<f64> srow(k, 0.0);
  for (usize i = 0; i < n; ++i) {
    std::fill(srow.begin(), srow.end(), 0.0);
    const f64 *row = R + i * n;
    for (usize j = 0; j < n; ++j) {
      if (j != i) {
        srow[lab[j]] += onc_distance(row[j]);
      }
    }
    const u32 own = lab[i];
    if (cnt[own] <= 1U) {
      continue; // singleton: 0
    }
    const f64 a = srow[own] / static_cast<f64>(cnt[own] - 1U);
    f64 b = std::numeric_limits<f64>::infinity();
    for (usize c = 0; c < k; ++c) {
      if (c != own && cnt[c] > 0U) {
        b = std::min(b, srow[c] / static_cast<f64>(cnt[c]));
      }
    }
    if (!std::isfinite(b)) {
      continue; // one cluster: 0
    }
    const f64 m = std::max(a, b);
    out[i] = m > 0.0 ? (b - a) / m : 0.0;
  }
  return out;
}

struct MeanSd {
  f64 mean{};
  f64 sd{};
};

[[nodiscard]] MeanSd mean_sd(std::span<const f64> v) noexcept {
  MeanSd r;
  if (v.empty()) {
    return r;
  }
  for (const f64 x : v) {
    r.mean += x;
  }
  r.mean /= static_cast<f64>(v.size());
  f64 ss = 0.0;
  for (const f64 x : v) {
    ss += (x - r.mean) * (x - r.mean);
  }
  r.sd = std::sqrt(ss / static_cast<f64>(v.size()));
  return r;
}

// ONC quality q = mean / sd (sd floored so a uniform partition scores finitely).
[[nodiscard]] f64 onc_quality(std::span<const f64> sil) noexcept {
  const MeanSd ms = mean_sd(sil);
  return ms.mean / std::max(ms.sd, kQualitySdFloor);
}

// Per-cluster ONC t-stat: mean / sd of the members' silhouettes.
[[nodiscard]] std::vector<f64> cluster_tstats(const std::vector<f64> &sil,
                                              const std::vector<u32> &lab, usize k) {
  std::vector<std::vector<f64>> by(k);
  for (usize i = 0; i < lab.size(); ++i) {
    by[lab[i]].push_back(sil[i]);
  }
  std::vector<f64> t(k, 0.0);
  for (usize c = 0; c < k; ++c) {
    t[c] = onc_quality(by[c]);
  }
  return t;
}

struct OncOut {
  std::vector<u32> labels;
  usize k{};
  ClusterShape shape{ClusterShape::Empty};
  std::vector<f64> sil;
};

[[nodiscard]] OncOut singletons(usize n) {
  OncOut o;
  o.shape = ClusterShape::Singletons;
  o.k = n;
  o.labels.resize(n);
  for (usize i = 0; i < n; ++i) {
    o.labels[i] = static_cast<u32>(i);
  }
  o.sil.assign(n, 0.0);
  return o;
}

[[nodiscard]] OncOut one_cluster(usize n) {
  OncOut o;
  o.shape = ClusterShape::OneCluster;
  o.k = 1U;
  o.labels.assign(n, 0U);
  o.sil.assign(n, 0.0);
  return o;
}

// ONC base stage (see trial_clusters.hpp).
[[nodiscard]] OncOut onc_base(const f64 *R, usize n, const OncConfig &cfg, u64 salt) {
  if (n == 0U) {
    return OncOut{};
  }
  if (n == 1U) {
    return one_cluster(1U);
  }
  f64 max_d = 0.0;
  for (usize i = 0; i < n; ++i) {
    for (usize j = 0; j < i; ++j) {
      max_d = std::max(max_d, onc_distance(R[i * n + j]));
    }
  }
  if (max_d <= cfg.tight_distance) {
    return one_cluster(n);
  }
  if (n == 2U) {
    return singletons(n);
  }
  const usize cap = cfg.max_k == 0U ? kOncDefaultMaxK : cfg.max_k;
  const usize kmax = std::min(n - 1U, cap);
  OncOut best;
  f64 best_q = -std::numeric_limits<f64>::infinity();
  for (usize k = 2; k <= kmax; ++k) {
    Partition bp;
    bool have = false;
    for (usize init = 0; init < cfg.n_init; ++init) {
      SplitMixRng rng{splitmix64(cfg.seed ^ splitmix64(salt ^ splitmix64(k * 1000003U + init)))};
      const Seeding seeding = init == 0U ? Seeding::FarthestFirst : Seeding::GreedyPlusPlus;
      Partition p = kernel_kmeans(R, n, k, cfg.max_iter, rng, seeding);
      if (!have || p.sse < bp.sse) {
        bp = std::move(p);
        have = true;
      }
    }
    std::vector<f64> sil = silhouettes(R, n, bp.labels, k);
    const f64 q = onc_quality(sil);
    if (q > best_q) {
      best_q = q;
      best.labels = std::move(bp.labels);
      best.k = k;
      best.sil = std::move(sil);
      best.shape = ClusterShape::Blocks;
    }
  }
  if (best.shape != ClusterShape::Blocks || mean_sd(best.sil).mean < cfg.min_silhouette) {
    return singletons(n); // no block structure
  }
  return best;
}

// ONC top stage: recursive refinement of below-average clusters (LdP 2019).
[[nodiscard]] OncOut onc_top(const f64 *R, usize n, const OncConfig &cfg, usize depth, u64 salt) {
  OncOut base = onc_base(R, n, cfg, salt);
  if (base.shape != ClusterShape::Blocks || depth >= cfg.max_depth) {
    return base;
  }
  const std::vector<f64> t = cluster_tstats(base.sil, base.labels, base.k);
  f64 t_mean = 0.0;
  for (const f64 v : t) {
    t_mean += v;
  }
  t_mean /= static_cast<f64>(base.k);
  std::vector<unsigned char> redo(base.k, 0U);
  usize n_redo = 0U;
  f64 redo_mean = 0.0;
  for (usize c = 0; c < base.k; ++c) {
    if (t[c] < t_mean) {
      redo[c] = 1U;
      ++n_redo;
      redo_mean += t[c];
    }
  }
  if (n_redo <= 1U) {
    return base;
  }
  redo_mean /= static_cast<f64>(n_redo);
  std::vector<usize> members;
  for (usize i = 0; i < n; ++i) {
    if (redo[base.labels[i]] != 0U) {
      members.push_back(i);
    }
  }
  const usize m = members.size();
  if (m < 3U) {
    return base;
  }
  std::vector<f64> sub(m * m);
  for (usize a = 0; a < m; ++a) {
    for (usize b = 0; b < m; ++b) {
      sub[a * m + b] = R[members[a] * n + members[b]];
    }
  }
  const OncOut inner = onc_top(sub.data(), m, cfg, depth + 1U, splitmix64(salt ^ (depth + 1U)));
  // Kept clusters keep their relative order, relabeled 0..K_keep-1; the
  // re-clustered members follow.
  std::vector<u32> remap(base.k, 0U);
  usize kept = 0U;
  for (usize c = 0; c < base.k; ++c) {
    if (redo[c] == 0U) {
      remap[c] = static_cast<u32>(kept++);
    }
  }
  OncOut out;
  out.shape = ClusterShape::Blocks;
  out.k = kept + inner.k;
  out.labels.resize(n);
  for (usize i = 0; i < n; ++i) {
    out.labels[i] = remap[base.labels[i]];
  }
  for (usize a = 0; a < m; ++a) {
    out.labels[members[a]] = static_cast<u32>(kept + inner.labels[a]);
  }
  out.sil = silhouettes(R, n, out.labels, out.k);
  const std::vector<f64> t_new = cluster_tstats(out.sil, out.labels, out.k);
  f64 new_mean = 0.0;
  for (const f64 v : t_new) {
    new_mean += v;
  }
  new_mean /= static_cast<f64>(out.k);
  if (!(new_mean > redo_mean)) {
    return base;
  }
  return out;
}

} // namespace

Result<TrialClusters> onc_cluster(std::span<const f64> corr, usize n, const OncConfig &cfg) {
  if (n != 0U && (n > std::numeric_limits<usize>::max() / n || corr.size() != n * n)) {
    return Err(ErrorCode::InvalidArgument, "onc_cluster: corr must be n x n");
  }
  if (n == 0U && !corr.empty()) {
    return Err(ErrorCode::InvalidArgument, "onc_cluster: corr must be n x n");
  }
  if (cfg.n_init == 0U || cfg.max_iter == 0U || cfg.max_k == 1U ||
      !std::isfinite(cfg.tight_distance) || !std::isfinite(cfg.min_silhouette)) {
    return Err(ErrorCode::InvalidArgument, "onc_cluster: invalid OncConfig");
  }
  for (const f64 v : corr) {
    if (!std::isfinite(v)) {
      return Err(ErrorCode::InvalidArgument, "onc_cluster: non-finite correlation");
    }
  }
  OncOut o = onc_top(corr.data(), n, cfg, 0U, 0U);
  // Canonical labels: numbered by first appearance in input order.
  std::vector<u32> canon(o.k, std::numeric_limits<u32>::max());
  u32 next = 0U;
  TrialClusters out;
  out.labels.resize(n);
  for (usize i = 0; i < n; ++i) {
    u32 &c = canon[o.labels[i]];
    if (c == std::numeric_limits<u32>::max()) {
      c = next++;
    }
    out.labels[i] = c;
  }
  out.n_clusters = static_cast<usize>(next);
  out.shape = o.shape;
  if (o.shape == ClusterShape::Blocks) {
    const MeanSd ms = mean_sd(o.sil);
    out.mean_silhouette = ms.mean;
    out.quality = ms.mean / std::max(ms.sd, kQualitySdFloor);
  }
  return out;
}

Result<std::vector<f64>> cluster_representative_sharpes(std::span<const f64> corr, usize n,
                                                        std::span<const f64> sharpes,
                                                        const TrialClusters &clusters) {
  const usize k = clusters.n_clusters;
  if (corr.size() != n * n || sharpes.size() != n || clusters.labels.size() != n) {
    return Err(ErrorCode::InvalidArgument, "cluster_representative_sharpes: shape mismatch");
  }
  for (const u32 l : clusters.labels) {
    if (static_cast<usize>(l) >= k) {
      return Err(ErrorCode::InvalidArgument, "cluster_representative_sharpes: bad label");
    }
  }
  std::vector<f64> sum_sr(k, 0.0);
  std::vector<f64> sum_r(k, 0.0);
  std::vector<usize> cnt(k, 0U);
  for (usize i = 0; i < n; ++i) {
    const u32 c = clusters.labels[i];
    sum_sr[c] += sharpes[i];
    ++cnt[c];
    const f64 *row = corr.data() + i * n;
    for (usize j = 0; j < n; ++j) {
      if (clusters.labels[j] == c) {
        sum_r[c] += row[j];
      }
    }
  }
  std::vector<f64> out(k, 0.0);
  for (usize c = 0; c < k; ++c) {
    if (cnt[c] == 0U) {
      return Err(ErrorCode::InvalidArgument, "cluster_representative_sharpes: empty cluster");
    }
    const f64 m = static_cast<f64>(cnt[c]);
    const f64 mean_r = std::max(sum_r[c] / (m * m), 1.0 / m);
    out[c] = (sum_sr[c] / m) / std::sqrt(mean_r);
  }
  return out;
}

Result<McMaxNull> mc_max_sharpe_null(std::span<const f64> unit_rows, usize n, usize d,
                                     std::span<const f64> null_sd, usize draws, u64 seed) {
  if (n == 0U || d == 0U || n > std::numeric_limits<usize>::max() / d ||
      unit_rows.size() != n * d || null_sd.size() != n) {
    return Err(ErrorCode::InvalidArgument, "mc_max_sharpe_null: shape mismatch");
  }
  if (draws == 0U || draws > kMaxMcDraws) {
    return Err(ErrorCode::InvalidArgument, "mc_max_sharpe_null: draws must be in [1, 1e7]");
  }
  for (const f64 v : unit_rows) {
    if (!std::isfinite(v)) {
      return Err(ErrorCode::InvalidArgument, "mc_max_sharpe_null: non-finite row");
    }
  }
  for (const f64 v : null_sd) {
    if (!std::isfinite(v) || !(v > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "mc_max_sharpe_null: null sd must be finite, > 0");
    }
  }
  SplitMixRng rng{splitmix64(seed ^ 0x4d434d4158ULL)};
  std::vector<f64> g(d, 0.0);
  McMaxNull out;
  out.sorted_max.resize(draws);
  const f64 *rows = unit_rows.data();
  const f64 *sd = null_sd.data();
  for (usize b = 0; b < draws; ++b) {
    for (f64 &x : g) {
      x = rng.normal();
    }
    f64 mx = -std::numeric_limits<f64>::infinity();
    for (usize i = 0; i < n; ++i) {
      mx = std::max(mx, sd[i] * dot(rows + i * d, g.data(), d));
    }
    out.sorted_max[b] = mx;
  }
  std::sort(out.sorted_max.begin(), out.sorted_max.end());
  const MeanSd ms = mean_sd(out.sorted_max);
  out.mean = ms.mean;
  out.sd = draws > 1U ? ms.sd * std::sqrt(static_cast<f64>(draws) /
                                          static_cast<f64>(draws - 1U))
                      : 0.0;
  return out;
}

} // namespace atx::engine::eval
