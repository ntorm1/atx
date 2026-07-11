#include "atx/vol/surface_db.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/detail/archive_util.hpp" // crc32c, align_up, canonicalize_symbol

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// Shared with surface_archive.cpp (ATXVSA) so both binary formats agree
// bit-for-bit on CRC-32C and canonical-symbol bytes. See detail/archive_util.hpp.
using detail::align_up;
using detail::crc32c;

namespace {

// ── Small helpers ────────────────────────────────────────────────────────

constexpr char kDbMagic[8] = {'A', 'T', 'X', 'V', 'D', 'B', '0', '1'};

[[nodiscard]] std::byte *buf_at(std::vector<std::byte> &b, std::uint64_t off) noexcept {
  return b.data() + static_cast<std::size_t>(off);
}

// Compile-time fingerprint of the on-disk layout. Folds the sizeof of every
// serialized record + a v1 format salt so a reader built against a different
// struct shape rejects the file instead of mis-reading bytes.
[[nodiscard]] std::uint64_t db_schema_hash() noexcept {
  constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
  // Salt derived from "atx-vol-surface-db-v1" -- distinct from the archive's
  // kV3Salt so a manifest never aliases into an archive schema hash (or vice
  // versa) even though both fold sizeof()s with the same FNV pattern.
  constexpr std::uint64_t kDbSalt = 0xB19E'55DB'2A17'0001ull;
  std::uint64_t h = 0x9e3779b97f4a7c15ull ^ kDbSalt;
  h ^= static_cast<std::uint64_t>(sizeof(DbManifestHeader)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(DbSymbolRecord)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(DbPartitionRecord)) * kFnvPrime;
  return h;
}

// CRC-32C over a header with its own checksum field zeroed.
[[nodiscard]] std::uint32_t header_crc(DbManifestHeader h) noexcept {
  h.header_crc32c = 0;
  std::array<std::byte, sizeof(DbManifestHeader)> bytes{};
  std::memcpy(bytes.data(), &h, sizeof h);
  return crc32c(bytes.data(), bytes.size());
}

[[nodiscard]] std::int64_t wall_clock_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Canonical-key comparator: memcmp of the shorter prefix, then length --
// deterministic layout independent of caller order. Returns <0/0/>0 like
// memcmp/strcmp so callers can both sort and equality-test with it.
[[nodiscard]] int cmp_key(const char *a, std::uint16_t alen, const char *b,
                          std::uint16_t blen) noexcept {
  const std::uint16_t n = std::min(alen, blen);
  const int c = n != 0 ? std::memcmp(a, b, n) : 0;
  if (c != 0) {
    return c;
  }
  if (alen != blen) {
    return alen < blen ? -1 : 1;
  }
  return 0;
}

// Partition-key validation (writer side): length 1..32, charset
// [A-Za-z0-9._-], no ".." substring; upper-cased on success.
[[nodiscard]] Result<std::string> canonicalize_key(std::string_view k) {
  if (k.empty() || k.size() > kSurfaceDbKeyMax) {
    return Err(ErrorCode::InvalidArgument,
               "surface_db: partition key length must be 1.." +
                   std::to_string(kSurfaceDbKeyMax));
  }
  std::string out(k.size(), '\0');
  for (std::size_t i = 0; i < k.size(); ++i) {
    char c = k[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) {
      return Err(ErrorCode::InvalidArgument, "surface_db: partition key has invalid character");
    }
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    out[i] = c;
  }
  if (out.find("..") != std::string::npos) {
    return Err(ErrorCode::InvalidArgument, "surface_db: partition key contains \"..\"");
  }
  return Ok(std::move(out));
}

// Field-for-field mirror of SymbolFitConfig -> DbSymbolRecord (writer side).
// Exact inverse of decode_symbol_record.
[[nodiscard]] DbSymbolRecord encode_symbol_record(std::string_view canon,
                                                   const SymbolFitConfig &cfg) noexcept {
  DbSymbolRecord rec{};
  const auto len = static_cast<std::uint16_t>(std::min(canon.size(), kSurfaceDbKeyMax));
  std::memcpy(rec.symbol, canon.data(), len);
  rec.symbol_len = len;

  std::uint16_t flags = 0;
  if (cfg.enabled) flags |= kDbSymEnabled;
  if (cfg.pin_curve) flags |= kDbSymPinCurve;
  if (cfg.al_override) flags |= kDbSymAlOverride;
  if (cfg.use_correction_cache) flags |= kDbSymUseCorrectionCache;
  if (cfg.score_parity) flags |= kDbSymScoreParity;
  if (cfg.enforce_calendar_floor) flags |= kDbSymEnforceCalendarFloor;
  if (cfg.use_deam_cache_for_fit) flags |= kDbSymUseDeamCacheForFit;
  if (cfg.curve.convex.bound_slope_below) flags |= kDbSymConvexBoundSlopeBelow;
  if (cfg.curve.parametric.lee_bound_project) flags |= kDbSymLeeBoundProject;
  if (cfg.curve.parametric.morozov_stop) flags |= kDbSymMorozovStop;
  if (cfg.curve.parametric.validate_no_arb) flags |= kDbSymValidateNoArb;
  if (cfg.curve.parametric.residual_disable) flags |= kDbSymResidualDisable;
  if (cfg.curve.parametric.essvi_asymmetric_rho) flags |= kDbSymEssviAsymmetricRho;
  rec.flags = flags;

  rec.preset = static_cast<std::uint8_t>(cfg.preset);
  rec.curve_kind = static_cast<std::uint8_t>(cfg.curve.kind);
  rec.calendar_repair = static_cast<std::uint8_t>(cfg.calendar_repair);
  rec.convex_loss = static_cast<std::uint8_t>(cfg.curve.convex.loss);
  rec.essvi_rho_mode = static_cast<std::uint8_t>(cfg.curve.parametric.essvi_rho_mode);
  rec.optimization_level = static_cast<std::uint8_t>(cfg.curve.parametric.optimization_level);
  rec.residual_basis_kind = static_cast<std::uint8_t>(cfg.curve.parametric.residual_basis_kind);
  rec.residual_n_basis_terms = cfg.curve.parametric.residual_n_basis_terms;
  rec.loss_kind = static_cast<std::uint8_t>(cfg.curve.parametric.loss_kind);
  rec.anchor_kind = static_cast<std::uint8_t>(cfg.curve.parametric.anchor_kind);

  rec.max_outer_iter = cfg.curve.parametric.max_outer_iter;
  rec.max_inner_iter = cfg.curve.parametric.max_inner_iter;
  rec.max_iter_quick_mark = cfg.curve.parametric.max_iter_quick_mark;
  rec.max_iter_trading = cfg.curve.parametric.max_iter_trading;
  rec.max_iter_risk = cfg.curve.parametric.max_iter_risk;
  rec.max_iter_reference = cfg.curve.parametric.max_iter_reference;
  rec.max_iter_cold_fast = cfg.curve.parametric.max_iter_cold_fast;
  rec.al_n_collocation = cfg.al.n_collocation;
  rec.al_n_quadrature = cfg.al.n_quadrature;
  rec.al_max_newton_iter = cfg.al.max_newton_iter;

  rec.convex_node_cap = static_cast<std::int32_t>(cfg.curve.convex.node_cap);
  rec.convex_max_iter = static_cast<std::int32_t>(cfg.curve.convex.max_iter);
  rec.max_obs_per_slice = cfg.curve.parametric.max_obs_per_slice;
  rec.n_butterfly_grid = cfg.curve.parametric.n_butterfly_grid;
  rec.min_obs_per_slice = cfg.curve.parametric.min_obs_per_slice;

  rec.convex_lambda = cfg.curve.convex.lambda;
  rec.tol_param = cfg.curve.parametric.tol_param;
  rec.tol_residual = cfg.curve.parametric.tol_residual;
  rec.huber_k = cfg.curve.parametric.huber_k;
  rec.min_vega_weight = cfg.curve.parametric.min_vega_weight;
  rec.max_spread_vol = cfg.curve.parametric.max_spread_vol;
  rec.max_weight = cfg.curve.parametric.max_weight;
  rec.max_otm_shortcut_premium_spread_frac =
      cfg.curve.parametric.max_otm_shortcut_premium_spread_frac;
  rec.prior_strength = cfg.curve.parametric.prior_strength;
  rec.essvi_fallback_rmse_threshold = cfg.curve.parametric.essvi_fallback_rmse_threshold;
  rec.wing_floor_alpha = cfg.curve.parametric.wing_floor_alpha;
  rec.morozov_tau = cfg.curve.parametric.morozov_tau;
  rec.residual_ridge_factor = cfg.curve.parametric.residual_ridge_factor;
  rec.max_post_fit_sigma = cfg.curve.parametric.max_post_fit_sigma;
  rec.max_spread_to_mid_pct = cfg.curve.parametric.max_spread_to_mid_pct;
  rec.al_tol = cfg.al.tol;
  rec.band_k = cfg.band_k;

  return rec;
}

// Wire-range validation for every enum stored as uint8. A manifest written by
// a future writer (wider enum) must be rejected here, not aliased into a
// different-but-valid-looking enumerator.
[[nodiscard]] bool symbol_record_enums_valid(const DbSymbolRecord &rec) noexcept {
  return rec.preset <= 3 && rec.curve_kind <= 4 && rec.calendar_repair <= 2 &&
         rec.convex_loss <= 1 && rec.essvi_rho_mode <= 2 && rec.optimization_level <= 4 &&
         rec.residual_basis_kind <= 5 && rec.loss_kind <= 1 && rec.anchor_kind <= 2;
}

} // namespace

// ── decode (public: exact inverse of encode_symbol_record) ────────────────

SymbolFitConfig decode_symbol_record(const DbSymbolRecord &rec) {
  SymbolFitConfig cfg;
  cfg.enabled = (rec.flags & kDbSymEnabled) != 0;
  cfg.pin_curve = (rec.flags & kDbSymPinCurve) != 0;
  cfg.al_override = (rec.flags & kDbSymAlOverride) != 0;
  cfg.use_correction_cache = (rec.flags & kDbSymUseCorrectionCache) != 0;
  cfg.score_parity = (rec.flags & kDbSymScoreParity) != 0;
  cfg.enforce_calendar_floor = (rec.flags & kDbSymEnforceCalendarFloor) != 0;
  cfg.use_deam_cache_for_fit = (rec.flags & kDbSymUseDeamCacheForFit) != 0;

  cfg.preset = static_cast<FitPreset>(rec.preset);
  cfg.curve.kind = static_cast<VolCurveKind>(rec.curve_kind);
  cfg.calendar_repair = static_cast<CalendarRepair>(rec.calendar_repair);

  cfg.curve.convex.bound_slope_below = (rec.flags & kDbSymConvexBoundSlopeBelow) != 0;
  cfg.curve.convex.loss = static_cast<CalibLossKind>(rec.convex_loss);
  cfg.curve.convex.node_cap = static_cast<int>(rec.convex_node_cap);
  cfg.curve.convex.max_iter = static_cast<int>(rec.convex_max_iter);
  cfg.curve.convex.lambda = rec.convex_lambda;

  auto &p = cfg.curve.parametric;
  p.max_outer_iter = rec.max_outer_iter;
  p.max_inner_iter = rec.max_inner_iter;
  p.tol_param = rec.tol_param;
  p.tol_residual = rec.tol_residual;
  p.huber_k = rec.huber_k;
  p.min_vega_weight = rec.min_vega_weight;
  p.max_spread_vol = rec.max_spread_vol;
  p.max_weight = rec.max_weight;
  p.max_obs_per_slice = rec.max_obs_per_slice;
  p.max_otm_shortcut_premium_spread_frac = rec.max_otm_shortcut_premium_spread_frac;
  p.prior_strength = rec.prior_strength;
  p.essvi_rho_mode = static_cast<EssviRhoMode>(rec.essvi_rho_mode);
  p.optimization_level = static_cast<OptimizationLevel>(rec.optimization_level);
  p.essvi_fallback_rmse_threshold = rec.essvi_fallback_rmse_threshold;
  p.n_butterfly_grid = rec.n_butterfly_grid;
  p.max_iter_quick_mark = rec.max_iter_quick_mark;
  p.max_iter_trading = rec.max_iter_trading;
  p.max_iter_risk = rec.max_iter_risk;
  p.max_iter_reference = rec.max_iter_reference;
  p.max_iter_cold_fast = rec.max_iter_cold_fast;
  p.wing_floor_alpha = rec.wing_floor_alpha;
  p.lee_bound_project = (rec.flags & kDbSymLeeBoundProject) != 0;
  p.morozov_stop = (rec.flags & kDbSymMorozovStop) != 0;
  p.morozov_tau = rec.morozov_tau;
  p.validate_no_arb = (rec.flags & kDbSymValidateNoArb) != 0;
  p.residual_disable = (rec.flags & kDbSymResidualDisable) != 0;
  p.residual_basis_kind = static_cast<ResidualBasisKind>(rec.residual_basis_kind);
  p.residual_n_basis_terms = rec.residual_n_basis_terms;
  p.residual_ridge_factor = rec.residual_ridge_factor;
  p.loss_kind = static_cast<CalibLossKind>(rec.loss_kind);
  p.anchor_kind = static_cast<CalibAnchorKind>(rec.anchor_kind);
  p.essvi_asymmetric_rho = (rec.flags & kDbSymEssviAsymmetricRho) != 0;
  p.min_obs_per_slice = rec.min_obs_per_slice;
  p.max_post_fit_sigma = rec.max_post_fit_sigma;
  p.max_spread_to_mid_pct = rec.max_spread_to_mid_pct;

  cfg.al.n_collocation = rec.al_n_collocation;
  cfg.al.n_quadrature = rec.al_n_quadrature;
  cfg.al.max_newton_iter = rec.al_max_newton_iter;
  cfg.al.tol = rec.al_tol;
  cfg.band_k = rec.band_k;

  return cfg;
}

// ── Writer ───────────────────────────────────────────────────────────────

Result<std::vector<std::byte>> write_db_manifest(std::span<const DbSymbolEntry> symbols,
                                                  std::span<const DbPartitionInfo> partitions,
                                                  const SurfaceDbManifestWriteOpts &opts) {
  if (symbols.size() > 0xFFFFFFFFull) {
    return Err(ErrorCode::InvalidArgument, "write_db_manifest: too many symbols");
  }
  if (partitions.size() > 0xFFFFFFFFull) {
    return Err(ErrorCode::InvalidArgument, "write_db_manifest: too many partitions");
  }

  // 1. Canonicalize + encode symbols.
  struct SymPlan {
    DbSymbolRecord rec{};
  };
  std::vector<SymPlan> sym_plans;
  sym_plans.reserve(symbols.size());
  for (const DbSymbolEntry &e : symbols) {
    const std::string canon = detail::canonicalize_symbol(e.symbol, kSurfaceDbKeyMax);
    if (canon.empty()) {
      return Err(ErrorCode::InvalidArgument, "write_db_manifest: empty canonical symbol");
    }
    SymPlan p;
    p.rec = encode_symbol_record(canon, e.config);
    sym_plans.push_back(p);
  }

  // 2. Deterministic order by canonical symbol; reject duplicates.
  std::sort(sym_plans.begin(), sym_plans.end(), [](const SymPlan &a, const SymPlan &b) {
    return cmp_key(a.rec.symbol, a.rec.symbol_len, b.rec.symbol, b.rec.symbol_len) < 0;
  });
  for (std::size_t i = 1; i < sym_plans.size(); ++i) {
    if (cmp_key(sym_plans[i - 1].rec.symbol, sym_plans[i - 1].rec.symbol_len, sym_plans[i].rec.symbol,
                sym_plans[i].rec.symbol_len) == 0) {
      return Err(ErrorCode::AlreadyExists, "write_db_manifest: duplicate canonical symbol");
    }
  }

  // 3. Canonicalize + encode partitions.
  struct PartPlan {
    DbPartitionRecord rec{};
  };
  std::vector<PartPlan> part_plans;
  part_plans.reserve(partitions.size());
  for (const DbPartitionInfo &info : partitions) {
    auto canon = canonicalize_key(info.key);
    if (!canon) {
      return Err(canon.error());
    }
    PartPlan p;
    const std::string &ck = *canon;
    const auto len = static_cast<std::uint16_t>(ck.size());
    std::memcpy(p.rec.key, ck.data(), len);
    p.rec.key_len = len;
    p.rec.surface_count = info.surface_count;
    p.rec.file_size = info.file_size;
    p.rec.created_ts_ns = info.created_ts_ns;
    part_plans.push_back(p);
  }

  // 4. Deterministic order by canonical key; reject duplicates.
  std::sort(part_plans.begin(), part_plans.end(), [](const PartPlan &a, const PartPlan &b) {
    return cmp_key(a.rec.key, a.rec.key_len, b.rec.key, b.rec.key_len) < 0;
  });
  for (std::size_t i = 1; i < part_plans.size(); ++i) {
    if (cmp_key(part_plans[i - 1].rec.key, part_plans[i - 1].rec.key_len, part_plans[i].rec.key,
                part_plans[i].rec.key_len) == 0) {
      return Err(ErrorCode::AlreadyExists, "write_db_manifest: duplicate canonical partition key");
    }
  }

  // 5. Layout: header @ 0, symbols @ align_up(header, 64), partitions @
  // align_up(symbols_end, 64).
  const std::uint64_t symbols_offset =
      align_up(sizeof(DbManifestHeader), kSurfaceDbSectionAlign);
  const std::uint64_t symbols_bytes =
      static_cast<std::uint64_t>(sym_plans.size()) * sizeof(DbSymbolRecord);
  const std::uint64_t partitions_offset =
      align_up(symbols_offset + symbols_bytes, kSurfaceDbSectionAlign);
  const std::uint64_t partitions_bytes =
      static_cast<std::uint64_t>(part_plans.size()) * sizeof(DbPartitionRecord);
  const std::uint64_t file_size = partitions_offset + partitions_bytes;

  // 6. Materialize the buffer (zero-initialized, so inter-section alignment
  // padding is deterministic and covered by payload_crc32c).
  std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));
  for (std::size_t i = 0; i < sym_plans.size(); ++i) {
    std::memcpy(buf_at(buffer, symbols_offset) + i * sizeof(DbSymbolRecord), &sym_plans[i].rec,
                sizeof(DbSymbolRecord));
  }
  for (std::size_t i = 0; i < part_plans.size(); ++i) {
    std::memcpy(buf_at(buffer, partitions_offset) + i * sizeof(DbPartitionRecord),
                &part_plans[i].rec, sizeof(DbPartitionRecord));
  }

  // payload_crc32c: the contiguous [symbols_offset, end-of-partitions) span.
  const std::uint32_t payload_crc =
      crc32c(buf_at(buffer, symbols_offset), static_cast<std::size_t>(file_size - symbols_offset));

  // 7. Header (payload_crc32c filled before header_crc32c, computed last).
  DbManifestHeader hdr{};
  std::memcpy(hdr.magic, kDbMagic, 8);
  hdr.major = kSurfaceDbMajor;
  hdr.minor = kSurfaceDbMinor;
  hdr.header_size = static_cast<std::uint16_t>(sizeof(DbManifestHeader));
  hdr.endian = 1;
  hdr.pointer_bits = 64;
  hdr.flags = opts.flags;
  hdr.file_size = file_size;
  hdr.created_ts_ns = opts.created_ts_ns != 0 ? opts.created_ts_ns : wall_clock_ns();
  hdr.updated_ts_ns = opts.updated_ts_ns != 0 ? opts.updated_ts_ns : wall_clock_ns();
  hdr.generation = opts.generation;
  hdr.schema_hash = db_schema_hash();
  hdr.symbol_count = static_cast<std::uint32_t>(sym_plans.size());
  hdr.partition_count = static_cast<std::uint32_t>(part_plans.size());
  hdr.symbols_offset = symbols_offset;
  hdr.partitions_offset = partitions_offset;
  hdr.symbol_record_size = static_cast<std::uint32_t>(sizeof(DbSymbolRecord));
  hdr.partition_record_size = static_cast<std::uint32_t>(sizeof(DbPartitionRecord));
  hdr.payload_crc32c = payload_crc;
  hdr.header_crc32c = header_crc(hdr);
  std::memcpy(buf_at(buffer, 0), &hdr, sizeof hdr);

  return Ok(std::move(buffer));
}

// ── Reader ───────────────────────────────────────────────────────────────

Result<DbManifest> DbManifest::open(std::vector<std::byte> bytes) {
  if (bytes.size() < sizeof(DbManifestHeader)) {
    return Err(ErrorCode::ParseError, "DbManifest::open: shorter than header");
  }

  DbManifestHeader h;
  std::memcpy(&h, bytes.data(), sizeof h);

  if (std::memcmp(h.magic, kDbMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "DbManifest::open: bad magic");
  }
  if (h.major != kSurfaceDbMajor) {
    return Err(ErrorCode::ParseError, "DbManifest::open: unsupported major version");
  }
  if (h.endian != 1) {
    return Err(ErrorCode::ParseError, "DbManifest::open: non-little-endian manifest");
  }
  if (h.pointer_bits != 64) {
    return Err(ErrorCode::ParseError, "DbManifest::open: unsupported pointer width");
  }
  if (h.header_size != sizeof(DbManifestHeader) ||
      h.symbol_record_size != sizeof(DbSymbolRecord) ||
      h.partition_record_size != sizeof(DbPartitionRecord)) {
    return Err(ErrorCode::ParseError, "DbManifest::open: record size mismatch");
  }
  if (h.schema_hash != db_schema_hash()) {
    return Err(ErrorCode::ParseError, "DbManifest::open: schema hash mismatch");
  }
  if (h.file_size != bytes.size()) {
    return Err(ErrorCode::ParseError, "DbManifest::open: file size mismatch");
  }

  const std::uint64_t symbols_bytes =
      static_cast<std::uint64_t>(h.symbol_count) * h.symbol_record_size;
  const std::uint64_t partitions_bytes =
      static_cast<std::uint64_t>(h.partition_count) * h.partition_record_size;

  if (h.symbols_offset < sizeof(DbManifestHeader)) {
    return Err(ErrorCode::ParseError, "DbManifest::open: symbols overlap header");
  }
  if (h.symbols_offset > h.file_size || symbols_bytes > h.file_size - h.symbols_offset) {
    return Err(ErrorCode::ParseError, "DbManifest::open: symbols out of bounds");
  }
  if (h.symbols_offset + symbols_bytes > h.partitions_offset) {
    return Err(ErrorCode::ParseError, "DbManifest::open: symbols overlap partitions");
  }
  if (h.partitions_offset > h.file_size || partitions_bytes > h.file_size - h.partitions_offset) {
    return Err(ErrorCode::ParseError, "DbManifest::open: partitions out of bounds");
  }

  if (header_crc(h) != h.header_crc32c) {
    return Err(ErrorCode::ParseError, "DbManifest::open: header checksum mismatch");
  }

  const std::uint64_t payload_end = h.partitions_offset + partitions_bytes;
  const std::uint32_t payload_crc = crc32c(
      buf_at(bytes, h.symbols_offset), static_cast<std::size_t>(payload_end - h.symbols_offset));
  if (payload_crc != h.payload_crc32c) {
    return Err(ErrorCode::ParseError, "DbManifest::open: payload checksum mismatch");
  }

  DbManifest m;
  m.header_ = h;
  m.symbols_.resize(h.symbol_count);
  if (symbols_bytes > 0) {
    std::memcpy(m.symbols_.data(), buf_at(bytes, h.symbols_offset),
                static_cast<std::size_t>(symbols_bytes));
  }
  m.partitions_.resize(h.partition_count);
  if (partitions_bytes > 0) {
    std::memcpy(m.partitions_.data(), buf_at(bytes, h.partitions_offset),
                static_cast<std::size_t>(partitions_bytes));
  }

  // Validate every record eagerly (once, here) so find_symbol/find_partition
  // stay cheap lookups with no per-query re-validation.
  for (const DbSymbolRecord &rec : m.symbols_) {
    if (rec.symbol_len == 0 || rec.symbol_len > kSurfaceDbKeyMax) {
      return Err(ErrorCode::ParseError, "DbManifest::open: invalid symbol_len");
    }
    if (!symbol_record_enums_valid(rec)) {
      return Err(ErrorCode::ParseError,
                 "DbManifest::open: bad enum wire value for symbol " +
                     std::string(rec.symbol, rec.symbol_len));
    }
  }
  for (std::size_t i = 1; i < m.symbols_.size(); ++i) {
    if (cmp_key(m.symbols_[i - 1].symbol, m.symbols_[i - 1].symbol_len, m.symbols_[i].symbol,
                m.symbols_[i].symbol_len) >= 0) {
      return Err(ErrorCode::ParseError, "DbManifest::open: symbols not strictly ascending");
    }
  }

  for (const DbPartitionRecord &rec : m.partitions_) {
    if (rec.key_len == 0 || rec.key_len > kSurfaceDbKeyMax) {
      return Err(ErrorCode::ParseError, "DbManifest::open: invalid partition key_len");
    }
  }
  for (std::size_t i = 1; i < m.partitions_.size(); ++i) {
    if (cmp_key(m.partitions_[i - 1].key, m.partitions_[i - 1].key_len, m.partitions_[i].key,
                m.partitions_[i].key_len) >= 0) {
      return Err(ErrorCode::ParseError, "DbManifest::open: partitions not strictly ascending");
    }
  }

  return Ok(std::move(m));
}

Result<SymbolFitConfig> DbManifest::find_symbol(std::string_view symbol) const {
  const std::string canon = detail::canonicalize_symbol(symbol, kSurfaceDbKeyMax);
  if (!canon.empty()) {
    const auto len = static_cast<std::uint16_t>(canon.size());
    const auto it =
        std::lower_bound(symbols_.begin(), symbols_.end(), canon,
                         [len](const DbSymbolRecord &rec, const std::string &key) {
                           return cmp_key(rec.symbol, rec.symbol_len, key.data(), len) < 0;
                         });
    if (it != symbols_.end() && cmp_key(it->symbol, it->symbol_len, canon.data(), len) == 0) {
      return decode_symbol_record(*it);
    }
  }
  return Err(ErrorCode::NotFound, "DbManifest::find_symbol: symbol not present");
}

const DbPartitionRecord *DbManifest::find_partition(std::string_view key) const noexcept {
  const std::string canon = detail::canonicalize_symbol(key, kSurfaceDbKeyMax);
  if (canon.empty()) {
    return nullptr;
  }
  const auto len = static_cast<std::uint16_t>(canon.size());
  const auto it =
      std::lower_bound(partitions_.begin(), partitions_.end(), canon,
                       [len](const DbPartitionRecord &rec, const std::string &key2) {
                         return cmp_key(rec.key, rec.key_len, key2.data(), len) < 0;
                       });
  if (it != partitions_.end() && cmp_key(it->key, it->key_len, canon.data(), len) == 0) {
    return &*it;
  }
  return nullptr;
}

} // namespace atx::vol
