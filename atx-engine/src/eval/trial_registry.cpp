// atx::engine::eval — TrialRegistry (see trial_registry.hpp for the contract).
#include "atx/engine/eval/trial_registry.hpp"

#include <algorithm> // std::max, std::min, std::clamp
#include <array>
#include <bit>     // std::bit_cast
#include <cmath>   // std::sqrt, std::isfinite
#include <cstring> // std::memcpy
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace atx::engine::eval {

namespace {

using atx::f64;
using atx::u64;
using atx::u8;
using atx::usize;

constexpr u64 kGolden = 0x9e3779b97f4a7c15ULL;
constexpr std::array<char, 8> kMagic{'A', 'T', 'X', 'T', 'R', 'G', '0', '1'};
constexpr u64 kVersion = 1U;
constexpr usize kHeaderBytes = 48U; // magic, version, pnl_len, sketch_dim, seed, checksum
constexpr usize kRecordFixed = 40U; // id, config_hash, sharpe, kind, (sketch), checksum
constexpr usize kMinSketchDim = 8U;

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

} // namespace

TrialId trial_id(TrialKind kind, u64 config_hash) noexcept {
  std::array<unsigned char, 9> bytes{};
  bytes[0] = static_cast<unsigned char>(kind);
  std::memcpy(bytes.data() + 1, &config_hash, sizeof(config_hash));
  return TrialId{stable_digest(bytes.data(), bytes.size())};
}

// ===========================================================================
//  Impl
// ===========================================================================
struct TrialRegistry::Impl {
  TrialRegistryConfig cfg;
  bool exact{};
  usize dim{}; // sketch width actually used: T when exact, else cfg.sketch_dim
  std::vector<usize> bucket;
  std::vector<f64> sign;
  std::vector<f64> gram; // dim x dim, upper triangle (i <= j) maintained
  f64 sum_s4{};          // Σ_i ||s_i||⁴ (the Gram's diagonal-pair mass)
  u64 n{};
  f64 sr_mean{};
  f64 sr_m2{};
  f64 sr_max{};
  u64 chain{0x6a09e667f3bcc909ULL};
  std::unordered_set<u64> ids;
  std::optional<std::ofstream> log; // present iff durable
  std::vector<unsigned char> rec_buf;
  std::vector<f64> z;
  std::vector<f64> s;

  explicit Impl(const TrialRegistryConfig &c) : cfg{c} {
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
    gram.assign(dim * dim, 0.0);
    rec_buf.resize(record_bytes());
    z.resize(cfg.pnl_len);
    s.resize(dim);
  }

  [[nodiscard]] usize record_bytes() const noexcept { return kRecordFixed + 8U * dim; }

  // Fold one (already validated / replayed) trial into every aggregate.
  void apply(u64 id, f64 sharpe, const f64 *sk) {
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
    // Rank-1 update of the upper triangle; zero rows are skipped (a sketch of a
    // short-support vector is sparse).
    for (usize i = 0; i < dim; ++i) {
      const f64 si = sk[i];
      if (si == 0.0) {
        continue;
      }
      f64 *row = gram.data() + i * dim;
      for (usize j = i; j < dim; ++j) {
        row[j] += si * sk[j];
      }
    }
  }

  void encode(u64 id, u64 config_hash, f64 sharpe, TrialKind kind, const f64 *sk) {
    put_u64(rec_buf, 0U, id);
    put_u64(rec_buf, 8U, config_hash);
    put_u64(rec_buf, 16U, std::bit_cast<u64>(sharpe));
    put_u64(rec_buf, 24U, static_cast<u64>(kind));
    std::memcpy(rec_buf.data() + 32U, sk, 8U * dim);
    const usize body = 32U + 8U * dim;
    put_u64(rec_buf, body, stable_digest(rec_buf.data(), body));
  }

  [[nodiscard]] std::vector<unsigned char> header_bytes() const {
    std::vector<unsigned char> h(kHeaderBytes, 0U);
    std::memcpy(h.data(), kMagic.data(), kMagic.size());
    put_u64(h, 8U, kVersion);
    put_u64(h, 16U, static_cast<u64>(cfg.pnl_len));
    put_u64(h, 24U, static_cast<u64>(cfg.sketch_dim));
    put_u64(h, 32U, cfg.sketch_seed);
    put_u64(h, 40U, stable_digest(h.data(), 40U));
    return h;
  }
};

namespace {

atx::core::Status validate_cfg(const TrialRegistryConfig &cfg) {
  if (cfg.pnl_len < 3U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "TrialRegistry: pnl_len must be >= 3");
  }
  if (cfg.sketch_dim < kMinSketchDim) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "TrialRegistry: sketch_dim must be >= 8");
  }
  return atx::core::Ok();
}

} // namespace

TrialRegistry::TrialRegistry(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
TrialRegistry::TrialRegistry(TrialRegistry &&) noexcept = default;
TrialRegistry &TrialRegistry::operator=(TrialRegistry &&) noexcept = default;
TrialRegistry::~TrialRegistry() = default;

atx::core::Result<TrialRegistry> TrialRegistry::in_memory(const TrialRegistryConfig &cfg) {
  ATX_TRY_VOID(validate_cfg(cfg));
  return TrialRegistry{std::make_unique<Impl>(cfg)};
}

atx::core::Result<TrialRegistry> TrialRegistry::open(const std::filesystem::path &path,
                                                     const TrialRegistryConfig &cfg) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  ATX_TRY_VOID(validate_cfg(cfg));
  auto impl = std::make_unique<Impl>(cfg);
  const std::vector<unsigned char> want_header = impl->header_bytes();

  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  usize good_len = 0U;
  if (exists) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot read " + path.string());
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() >= kHeaderBytes) {
      if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0 ||
          get_u64(bytes.data() + 40U) != stable_digest(bytes.data(), 40U)) {
        return Err(ErrorCode::ParseError, "TrialRegistry: bad header in " + path.string());
      }
      if (std::memcmp(bytes.data(), want_header.data(), kHeaderBytes) != 0) {
        return Err(ErrorCode::InvalidArgument,
                   "TrialRegistry: config does not match the registry file header");
      }
      good_len = kHeaderBytes;
      const usize rb = impl->record_bytes();
      const usize body = rb - 8U;
      // Bounded replay: one iteration per complete record in the file.
      while (good_len + rb <= bytes.size()) {
        const unsigned char *r = bytes.data() + good_len;
        if (get_u64(r + body) != stable_digest(r, body) || !valid_kind(get_u64(r + 24U))) {
          break; // corrupt record: it and everything after it is a torn tail
        }
        const u64 id = get_u64(r);
        const f64 sharpe = std::bit_cast<f64>(get_u64(r + 16U));
        std::memcpy(impl->s.data(), r + 32U, 8U * impl->dim);
        if (impl->ids.find(id) == impl->ids.end()) {
          impl->apply(id, sharpe, impl->s.data());
        }
        good_len += rb;
      }
    }
    // A header shorter than kHeaderBytes is a torn creation: nothing committed.
    in.close();
    if (good_len != bytes.size()) {
      std::filesystem::resize_file(path, good_len, ec);
      if (ec) {
        return Err(ErrorCode::IoError, "TrialRegistry: cannot repair tail: " + ec.message());
      }
    }
  }

  impl->log.emplace(path, std::ios::binary | std::ios::app);
  if (!*impl->log) {
    return Err(ErrorCode::IoError, "TrialRegistry: cannot open for append " + path.string());
  }
  if (good_len == 0U) {
    impl->log->write(reinterpret_cast<const char *>(want_header.data()),
                     static_cast<std::streamsize>(want_header.size()));
    impl->log->flush();
    if (!*impl->log) {
      return Err(ErrorCode::IoError, "TrialRegistry: cannot write header");
    }
  }
  return TrialRegistry{std::move(impl)};
}

atx::core::Result<RecordOutcome> TrialRegistry::record(TrialKind kind, u64 config_hash,
                                                       std::span<const f64> oos_pnl,
                                                       f64 sharpe) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  Impl &im = *impl_;
  if (!valid_kind(static_cast<u64>(kind))) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: unknown TrialKind");
  }
  if (oos_pnl.size() != im.cfg.pnl_len) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: pnl length != pnl_len");
  }
  if (!std::isfinite(sharpe)) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: non-finite sharpe");
  }
  const TrialId id = trial_id(kind, config_hash);
  if (im.ids.find(id.value) != im.ids.end()) {
    return atx::core::Ok(RecordOutcome{id, false});
  }
  // Standardize to a unit vector: z·z' is then the Pearson correlation.
  f64 mean = 0.0;
  for (const f64 v : oos_pnl) {
    if (!std::isfinite(v)) {
      return Err(ErrorCode::InvalidArgument, "TrialRegistry: non-finite pnl");
    }
    mean += v;
  }
  mean /= static_cast<f64>(oos_pnl.size());
  f64 ss = 0.0;
  for (const f64 v : oos_pnl) {
    ss += (v - mean) * (v - mean);
  }
  if (!(ss > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "TrialRegistry: zero-variance pnl");
  }
  const f64 inv = 1.0 / std::sqrt(ss);
  for (usize t = 0; t < oos_pnl.size(); ++t) {
    im.z[t] = (oos_pnl[t] - mean) * inv;
  }
  if (im.exact) {
    std::copy(im.z.begin(), im.z.end(), im.s.begin());
  } else {
    std::fill(im.s.begin(), im.s.end(), 0.0);
    for (usize t = 0; t < oos_pnl.size(); ++t) {
      im.s[im.bucket[t]] += im.sign[t] * im.z[t];
    }
  }
  if (im.log.has_value()) {
    im.encode(id.value, config_hash, sharpe, kind, im.s.data());
    im.log->write(reinterpret_cast<const char *>(im.rec_buf.data()),
                  static_cast<std::streamsize>(im.rec_buf.size()));
    im.log->flush();
    if (!*im.log) {
      return Err(ErrorCode::IoError, "TrialRegistry: durable append failed");
    }
  }
  im.apply(id.value, sharpe, im.s.data());
  return atx::core::Ok(RecordOutcome{id, true});
}

TrialSummary TrialRegistry::summary() const {
  const Impl &im = *impl_;
  TrialSummary out;
  out.n_raw = im.n;
  out.pnl_len = im.cfg.pnl_len;
  out.registry_hash = im.chain;
  if (im.n == 0U) {
    return out;
  }
  out.mean_sr = im.sr_mean;
  out.var_sr = im.n >= 2U ? im.sr_m2 / static_cast<f64>(im.n - 1U) : 0.0;
  out.max_sr = im.sr_max;

  // ||M||_F² from the upper triangle, and tr M.
  f64 frob2 = 0.0;
  f64 trace = 0.0;
  for (usize i = 0; i < im.dim; ++i) {
    const f64 *row = im.gram.data() + i * im.dim;
    trace += row[i];
    frob2 += row[i] * row[i];
    f64 off = 0.0;
    for (usize j = i + 1U; j < im.dim; ++j) {
      off += row[j] * row[j];
    }
    frob2 += 2.0 * off;
  }
  out.n_eff_uncorrected = frob2 > 0.0 ? (trace * trace) / frob2 : 0.0;

  const f64 nf = static_cast<f64>(im.n);
  const f64 pairs = nf * (nf - 1.0);
  // E[ρ̂²] under independence: 1/(T−1) sampling noise, plus ≈ 1/d sketch noise.
  f64 c = 1.0 / static_cast<f64>(im.cfg.pnl_len - 1U);
  if (!im.exact) {
    c += 1.0 / static_cast<f64>(im.dim);
  }
  c = std::min(c, 0.5);
  const f64 s_off = std::max(0.0, frob2 - im.sum_s4);
  const f64 off_corr = std::max(0.0, (s_off - pairs * c) / (1.0 - c));
  const f64 total = nf + off_corr;
  out.n_eff = std::clamp((nf * nf) / total, 1.0, nf);
  return out;
}

u64 TrialRegistry::size() const noexcept { return impl_->n; }

bool TrialRegistry::contains(TrialId id) const {
  return impl_->ids.find(id.value) != impl_->ids.end();
}

const TrialRegistryConfig &TrialRegistry::config() const noexcept { return impl_->cfg; }

} // namespace atx::engine::eval
