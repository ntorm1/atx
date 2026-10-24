#include "atx/vol/api/storage/surface_db.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "fitting/essvi_anchored.hpp" // apply_anchored_env_policy (opt-in)
#include "storage/archive_util.hpp"  // crc32c, align_up, canonicalize_symbol, PositionalFile
#include "storage/surface_archive_internal.hpp" // read_archive_v2_first_now_ts
#include "storage/writer_lock.hpp"   // Task D3: cross-process manifest writer lock

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

// Bits 0..12 — includes both publish-with-Degraded reasons, ValidationFailure::CarryGap
// (1u << 11) and ValidationFailure::SubstituteUnderserve (1u << 12): a Degraded
// provenance carrying either is a routinely SERVED state and must round-trip.
constexpr std::uint32_t kKnownFailures = (1u << 13) - 1u;

// SurfacePolicy + provenance into the record's named fields (ATXVDB02).
void encode_surface_policy(DbSymbolRecord &rec, const SurfacePolicy &policy,
                           const std::optional<SurfaceProvenance> &provenance) noexcept {
  rec.policy_quality_mode = static_cast<std::uint8_t>(policy.quality_mode);
  rec.policy_outputs = static_cast<std::uint8_t>(policy.outputs);
  rec.policy_risk_admission = static_cast<std::uint8_t>(policy.risk_admission);
  rec.policy_fallback = static_cast<std::uint8_t>(policy.fallback);
  if (provenance.has_value()) {
    rec.prov_present = 1u;
    rec.prov_purpose = static_cast<std::uint8_t>(provenance->purpose);
    rec.prov_state = static_cast<std::uint8_t>(provenance->state);
    rec.prov_quality_mode = static_cast<std::uint8_t>(provenance->quality_mode);
    rec.prov_validation_failures = static_cast<std::uint32_t>(provenance->validation.failures);
    rec.prov_validation_id = provenance->validation.validation_id;
    rec.prov_source_generation = provenance->source_generation;
    rec.prov_served_generation = provenance->served_generation;
  }
}

// The policy/provenance half of a symbol record. There is no all-zero "legacy"
// policy: `policy_outputs` must be 1..3, and an absent provenance is all-zero.
[[nodiscard]] bool surface_policy_fields_valid(const DbSymbolRecord &r) noexcept {
  const bool policy_valid = r.policy_quality_mode <= 2u && r.policy_outputs >= 1u &&
                            r.policy_outputs <= 3u && r.policy_risk_admission <= 1u &&
                            r.policy_fallback <= 1u;
  const bool requests_risk =
      (r.policy_outputs & static_cast<std::uint8_t>(SurfaceOutputs::Risk)) != 0u;
  const bool mandatory_risk_admission =
      !requests_risk || r.policy_risk_admission == static_cast<std::uint8_t>(RiskAdmission::Required);
  bool provenance_valid = false;
  if (r.prov_present == 0u) {
    provenance_valid = r.prov_purpose == 0u && r.prov_state == 0u && r.prov_quality_mode == 0u &&
                       r.prov_validation_failures == 0u && r.prov_validation_id == 0u &&
                       r.prov_source_generation == 0u && r.prov_served_generation == 0u;
  } else if (r.prov_present == 1u) {
    provenance_valid = r.prov_purpose <= 1u && r.prov_state <= 3u && r.prov_quality_mode <= 2u &&
                       (r.prov_validation_failures & ~kKnownFailures) == 0u &&
                       (r.prov_state != static_cast<std::uint8_t>(SurfaceState::Healthy) ||
                        r.prov_validation_failures == 0u);
  }
  return policy_valid && mandatory_risk_admission && provenance_valid && r.pad0 == 0u;
}

[[nodiscard]] SurfacePolicy decode_surface_policy(const DbSymbolRecord &r) noexcept {
  SurfacePolicy policy;
  policy.quality_mode = static_cast<FitQualityMode>(r.policy_quality_mode);
  policy.outputs = static_cast<SurfaceOutputs>(r.policy_outputs);
  policy.risk_admission = static_cast<RiskAdmission>(r.policy_risk_admission);
  policy.fallback = static_cast<SurfaceFallback>(r.policy_fallback);
  return policy;
}

template <std::size_t N>
[[nodiscard]] bool key_tail_zero(const char (&s)[N], std::size_t len) noexcept {
  return std::all_of(s + std::min(len, N), s + N, [](char c) { return c == '\0'; });
}

template <std::size_t N> [[nodiscard]] bool bytes_zero(const std::uint8_t (&b)[N]) noexcept {
  return std::all_of(std::begin(b), std::end(b), [](std::uint8_t v) { return v == 0u; });
}

[[nodiscard]] std::byte *buf_at(std::vector<std::byte> &b, std::uint64_t off) noexcept {
  return b.data() + static_cast<std::size_t>(off);
}

// The FNV-1a/64 prime, shared by every FNV fold in this TU.
//
// FIX-D fix-1 (I4). It lives here, once, because it did NOT used to: db_schema_hash
// below spelled it correctly while `fnv1a_fold` (the carry-over fingerprint) had a
// literal with one hex digit too many -- 0x1000'0000'01b3, which is not FNV's prime
// and whose avalanche nobody had analysed. Any odd multiplier is deterministic, so
// the bug was invisible; the failure it risks is a fingerprint COLLISION, i.e.
// silently carrying a stale surface, the outcome this design ranks worst. One named
// constant plus the identity check below makes the same typo impossible to repeat.
inline constexpr std::uint64_t kFnv1a64Prime = 0x100000001b3ull;
static_assert(kFnv1a64Prime == (1ull << 40) + (1ull << 8) + 0xb3ull,
              "kFnv1a64Prime is not FNV-1a/64's prime (2^40 + 2^8 + 0xb3)");
// The matching offset basis, used to seed a fold that starts from nothing.
inline constexpr std::uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325ull;

// Compile-time fingerprint of the on-disk layout. Folds the sizeof of every
// serialized record + a v1 format salt so a reader built against a different
// struct shape rejects the file instead of mis-reading bytes.
[[nodiscard]] std::uint64_t db_schema_hash() noexcept {
  constexpr std::uint64_t kFnvPrime = kFnv1a64Prime;
  // ATXVDB02 salt (ATXVDB01 was ...0001) -- distinct from the archive's salt so a
  // manifest never aliases into an archive schema hash (or vice versa) even though
  // both fold sizeof()s with the same FNV pattern.
  constexpr std::uint64_t kDbSalt = 0xB19E'55DB'2A17'0002ull;
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
               "surface_db: partition key length must be 1.." + std::to_string(kSurfaceDbKeyMax));
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
                                                  const SymbolFitConfig &cfg,
                                                  const std::optional<SurfaceProvenance>
                                                      &provenance) noexcept {
  DbSymbolRecord rec{};
  const auto len = static_cast<std::uint16_t>(std::min(canon.size(), kSurfaceDbKeyMax));
  std::memcpy(rec.symbol, canon.data(), len);
  rec.symbol_len = len;

  std::uint16_t flags = 0;
  if (cfg.enabled)
    flags |= kDbSymEnabled;
  if (cfg.pin_curve)
    flags |= kDbSymPinCurve;
  if (cfg.al_override)
    flags |= kDbSymAlOverride;
  if (cfg.use_correction_cache)
    flags |= kDbSymUseCorrectionCache;
  if (cfg.score_parity)
    flags |= kDbSymScoreParity;
  if (cfg.enforce_calendar_floor)
    flags |= kDbSymEnforceCalendarFloor;
  if (cfg.use_deam_cache_for_fit)
    flags |= kDbSymUseDeamCacheForFit;
  if (cfg.curve.convex.bound_slope_below)
    flags |= kDbSymConvexBoundSlopeBelow;
  if (cfg.curve.parametric.lee_bound_project)
    flags |= kDbSymLeeBoundProject;
  if (cfg.curve.parametric.morozov_stop)
    flags |= kDbSymMorozovStop;
  if (cfg.curve.parametric.validate_no_arb)
    flags |= kDbSymValidateNoArb;
  if (cfg.curve.parametric.residual_disable)
    flags |= kDbSymResidualDisable;
  if (cfg.curve.parametric.essvi_asymmetric_rho)
    flags |= kDbSymEssviAsymmetricRho;
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
  rec.al_n_quad_price = cfg.al.n_quad_price; // C2 (SE-P1-2): decoupled premium order

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
  encode_surface_policy(rec, cfg.surface_policy, provenance);
  return rec;
}

// FNV-1a/64 over raw bytes. Deliberately NOT atx::core::hash_bytes (wyhash):
// that is documented as "not stable across process restarts or platforms", and
// this digest is PERSISTED and compared across runs. FNV-1a is fixed arithmetic,
// so the same bytes fold to the same value on every process and host.
[[nodiscard]] std::uint64_t fnv1a_fold(std::uint64_t h, const void *data,
                                       std::size_t len) noexcept {
  // FIX-D fix-1 (I4): the shared, identity-checked prime above. This line used to
  // carry its own literal, and that literal was wrong (see kFnv1a64Prime).
  const auto *p = static_cast<const std::uint8_t *>(data);
  for (std::size_t i = 0; i < len; ++i) {
    h ^= static_cast<std::uint64_t>(p[i]);
    h *= kFnv1a64Prime;
  }
  return h;
}

// Wire-range validation for every enum stored as uint8. A manifest written by
// a future writer (wider enum) must be rejected here, not aliased into a
// different-but-valid-looking enumerator.
//
// curve_kind <= 4 deliberately excludes VolCurveKind::SplineVol (= 5):
// SplineVol has no ATXVDB v1 wire format (mirrors surface_archive.cpp's
// "SplineVol serialization not supported" rejection), so a config pinning it
// must fail upsert cleanly rather than persist a record no reader can
// reconstruct. Raise this cap only alongside a new wire version that defines
// SplineVol's on-disk payload.
[[nodiscard]] bool symbol_record_enums_valid(const DbSymbolRecord &rec) noexcept {
  return rec.preset <= static_cast<std::uint8_t>(FitPreset::Bulk) && // Perf 2b: Bulk=5 is new max
         rec.curve_kind <= 4 && rec.calendar_repair <= 2 &&
         rec.convex_loss <= 1 && rec.essvi_rho_mode <= 2 && rec.optimization_level <= 4 &&
         rec.residual_basis_kind <= 5 && rec.loss_kind <= 1 && rec.anchor_kind <= 2 &&
         surface_policy_fields_valid(rec);
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
  cfg.al.n_quad_price = rec.al_n_quad_price; // C2 (SE-P1-2); 0 on pre-C2 manifests -> tied
  cfg.al.tol = rec.al_tol;
  cfg.band_k = rec.band_k;
  cfg.surface_policy = decode_surface_policy(rec);

  return cfg;
}

// Fold the fit CONFIG of each named symbol into one stable digest. `canon` must
// already be canonicalized; it is sorted here so caller order cannot change the
// result. Each symbol contributes the sizeof(DbSymbolRecord) bytes of its record
// re-encoded with provenance = nullopt: that zeroes the named `prov_*` fields
// (which `write_partition` rewrites on every write, and which are not part of the
// config) while keeping every genuine config field, the `policy_*` fields
// included. Returns 0 — the "unknown, never reuse" sentinel — if
// any symbol has no manifest entry.
[[nodiscard]] std::uint64_t fold_symbol_configs(std::span<const DbSymbolRecord> recs,
                                                std::vector<std::string> canon) {
  std::sort(canon.begin(), canon.end());
  std::uint64_t h =
      fnv1a_fold(kFnv1a64OffsetBasis, &kSurfaceDbCarryOverFitSalt, sizeof kSurfaceDbCarryOverFitSalt);
  for (const std::string &sym : canon) {
    // PRECONDITION: `recs` is sorted strictly ascending by `cmp_key` on the
    // canonical symbol. Every DbManifest comes from `DbManifest::open`, which
    // REJECTS a manifest that is not (see the "symbols not strictly ascending"
    // check), so this is enforced, not assumed -- and the writer sorts with the
    // same comparator. Given that, bisect exactly as `DbManifest::find_symbol`
    // does: a linear scan here is O(partition symbols x manifest symbols) per
    // date -- ~1M string compares on a 1030-name universe, in a fix whose entire
    // premise is that the cost multiplier is the universe width. That the bisect
    // agrees with the scan it replaced is pinned by
    // SurfaceDbConfigFingerprint.FoldFindsTheSameRecordAsALinearScan, which
    // includes the prefix/length cases where `cmp_key`'s ordering is the only
    // thing keeping the two the same.
    const auto len = static_cast<std::uint16_t>(sym.size());
    const auto it = std::lower_bound(recs.begin(), recs.end(), sym,
                                     [len](const DbSymbolRecord &rec, const std::string &key) {
                                       return cmp_key(rec.symbol, rec.symbol_len, key.data(),
                                                      len) < 0;
                                     });
    if (it == recs.end() || cmp_key(it->symbol, it->symbol_len, sym.data(), len) != 0) {
      return 0u; // a symbol we have no config for -> unknown -> never reuse
    }
    const DbSymbolRecord clean =
        encode_symbol_record(sym, decode_symbol_record(*it), std::nullopt);
    h = fnv1a_fold(h, &clean, sizeof clean);
  }
  // Never hand back the sentinel for a successful fold.
  return h == 0u ? 1u : h;
}

// ── Fitting-pipeline binding ────────────────────────────────────────────────

void apply_symbol_config(const SymbolFitConfig &cfg, SessionInputs &in) {
  apply_fit_preset(in, cfg.preset);
  if (cfg.pin_curve) {
    in.curve = cfg.curve;
    in.calib = cfg.curve.parametric;
    // The STORED parametric block predates the anchored flags, so this
    // assignment would silently clear what `apply_fit_preset` just armed.
    // Re-arm from the same seam.
    apply_anchored_env_policy(in.calib);
  }
  if (cfg.al_override) {
    in.deam.al_opts = cfg.al;
  }
  in.band_k = cfg.band_k;
  in.calendar_repair = cfg.calendar_repair;
  in.use_correction_cache = cfg.use_correction_cache;
  in.score_parity = cfg.score_parity;
  in.enforce_calendar_floor = cfg.enforce_calendar_floor;
  in.use_deam_cache_for_fit = cfg.use_deam_cache_for_fit;
}

void apply_symbol_config(const SymbolFitConfig &cfg, SessionInputs &in, SurfacePolicy &policy) {
  apply_symbol_config(cfg, in);
  // Unconditional, like every other SymbolFitConfig field this function binds
  // (band_k, calendar_repair, ...): the stored surface_policy is always the
  // final word — see the header comment for why it has no preset-deferring
  // gate the way pin_curve/al_override do.
  policy = cfg.surface_policy;
}

SymbolFitConfig symbol_config_from_preset(FitPreset preset) {
  SessionInputs tmp;
  apply_fit_preset(tmp, preset);
  SymbolFitConfig cfg;
  cfg.preset = preset;
  cfg.pin_curve = false;
  cfg.curve = tmp.curve;
  cfg.al_override = tmp.deam.al_opts.has_value();
  cfg.al = tmp.deam.al_opts.value_or(AlOpts{});
  cfg.band_k = tmp.band_k;
  cfg.calendar_repair = tmp.calendar_repair;
  cfg.use_correction_cache = tmp.use_correction_cache;
  cfg.score_parity = tmp.score_parity;
  cfg.enforce_calendar_floor = tmp.enforce_calendar_floor;
  cfg.use_deam_cache_for_fit = tmp.use_deam_cache_for_fit;
  const LegacyPresetMapping legacy = map_legacy_fit_preset(preset);
  cfg.surface_policy.quality_mode = legacy.quality_mode;
  // R1 (2026-08-23 top-of-book fit-refusal diagnosis, §4). A Risk-purpose
  // preset asks for BOTH outputs, not Risk alone.
  //
  // WHY. `SurfaceOutputs` is a bitmask and `PricerFitter::fit` engages the mark
  // arm only when the MarketMark bit is set (pricer_fitter.cpp: `if
  // (has_output(requested_outputs, SurfacePurpose::MarketMark))`). With Risk
  // alone, `mark_future` is never launched and `finalize_mark()` returns
  // immediately, so a board whose risk candidate the independent geometry
  // oracle refuses produced NOTHING — even though the same board fits perfectly
  // well as a market mark (measured: 25 ok / 1 fail as marks against 6 ok / 20
  // fail as risk candidates, same boards, same date, same preset, every mark
  // `mm_state = healthy`). The pipeline needs fair values, fair vols and greeks,
  // which is a mark-grade need; refusing to BUILD the mark threw that away to
  // protect a risk contract that is not weakened by building it.
  //
  // WHAT IS NOT WEAKENED. `risk_admission` stays `Required` below, so a Risk
  // request is still served only through the independent oracle. Every
  // fail-closed read seam is untouched: `PricerFitter::surface()` and the
  // default-purpose `value_chain` overloads route to `risk_surface_` whenever
  // the request carries the Risk bit and answer `nullptr`/`Unavailable` while
  // risk is unserved — the mark is never silently substituted for risk. What
  // changes is only that the mark now EXISTS to be asked for by name, and
  // `fit_board` serves it explicitly tagged `SurfacePurpose::MarketMark`
  // (corpus_board_fit.cpp).
  //
  // ON-DISK: `outputs` is persisted in the named `DbSymbolRecord::policy_outputs`
  // field (ATXVDB02, surface_db.hpp), and `surface_policy_fields_valid` admits
  // `outputs` in 1..3 — `MarketMarkAndRisk` is 3. The record BYTES change, so
  // `fold_symbol_configs` yields a different carry-over fingerprint and a
  // partition fitted under the old contract is re-fitted rather than carried —
  // which is exactly the intended behaviour for a changed fit config.
  //
  // A manifest that already stores `outputs = Risk` keeps it: the populate
  // seeder leaves an existing symbol config untouched by design (an operator
  // override wins). Re-configuring the symbol is what adopts the wider request.
  cfg.surface_policy.outputs = legacy.purpose == SurfacePurpose::MarketMark
                                   ? SurfaceOutputs::MarketMark
                                   : SurfaceOutputs::MarketMarkAndRisk;
  cfg.surface_policy.risk_admission = legacy.purpose == SurfacePurpose::Risk
                                          ? RiskAdmission::Required
                                          : RiskAdmission::NotApplicable;
  cfg.surface_policy.fallback = legacy.purpose == SurfacePurpose::Risk
                                    ? SurfaceFallback::LastKnownGood
                                    : SurfaceFallback::None;
  return cfg;
}

std::optional<SurfaceProvenance>
decode_symbol_provenance(const DbSymbolRecord &rec) noexcept {
  if (rec.prov_present == 0u) {
    return std::nullopt;
  }
  SurfaceProvenance provenance;
  provenance.purpose = static_cast<SurfacePurpose>(rec.prov_purpose);
  provenance.quality_mode = static_cast<FitQualityMode>(rec.prov_quality_mode);
  provenance.state = static_cast<SurfaceState>(rec.prov_state);
  provenance.validation.failures = static_cast<ValidationFailure>(rec.prov_validation_failures);
  provenance.validation.validation_id = rec.prov_validation_id;
  provenance.source_generation = rec.prov_source_generation;
  provenance.served_generation = rec.prov_served_generation;
  provenance.legacy_format = false;
  return provenance;
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
    p.rec = encode_symbol_record(canon, e.config, e.provenance);
    // Validate wire-range BEFORE this record ever reaches disk: the exact
    // same check DbManifest::open enforces on read (symbol_record_enums_valid),
    // so a config carrying an out-of-range enum (e.g. an invalid
    // static_cast<FitPreset>) is rejected here instead of round-tripping into
    // a manifest the parser would go on to refuse -- see persist_locked's
    // parse-before-rename ordering for the other half of this guard.
    if (!symbol_record_enums_valid(p.rec)) {
      return Err(ErrorCode::InvalidArgument,
                 "write_db_manifest: symbol config has out-of-range enum value for " + canon);
    }
    sym_plans.push_back(p);
  }

  // 2. Deterministic order by canonical symbol; reject duplicates.
  std::sort(sym_plans.begin(), sym_plans.end(), [](const SymPlan &a, const SymPlan &b) {
    return cmp_key(a.rec.symbol, a.rec.symbol_len, b.rec.symbol, b.rec.symbol_len) < 0;
  });
  for (std::size_t i = 1; i < sym_plans.size(); ++i) {
    if (cmp_key(sym_plans[i - 1].rec.symbol, sym_plans[i - 1].rec.symbol_len,
                sym_plans[i].rec.symbol, sym_plans[i].rec.symbol_len) == 0) {
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
    // FIX-D. Symmetric with decode_partitions below -- the decode half is NOT
    // optional, because every manifest mutation round-trips the untouched partitions
    // through DbPartitionInfo, so a write-only field would be wiped from every other
    // partition on the next upsert_symbol.
    p.rec.config_fingerprint = info.config_fingerprint;
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
  const std::uint64_t symbols_offset = align_up(sizeof(DbManifestHeader), kSurfaceDbSectionAlign);
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
  std::memcpy(hdr.magic, kSurfaceDbMagic, 8);
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

  if (std::memcmp(h.magic, kSurfaceDbMagic, 8) != 0) {
    if (std::memcmp(h.magic, "ATXVDB01", 8) == 0) {
      return Err(ErrorCode::ParseError,
                 "DbManifest::open: found magic 'ATXVDB01' (the previous manifest format); this "
                 "reader reads only ATXVDB02 - rebuild the surface db");
    }
    return Err(ErrorCode::ParseError, "DbManifest::open: bad magic (not an ATXVDB02 manifest)");
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
  if (h.header_size != sizeof(DbManifestHeader) || h.symbol_record_size != sizeof(DbSymbolRecord) ||
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
  if (h.pad0 != 0u || !bytes_zero(h.reserved)) {
    return Err(ErrorCode::ParseError, "DbManifest::open: non-zero reserved header bytes");
  }

  const std::uint64_t payload_end = h.partitions_offset + partitions_bytes;
  // Canonical placement: symbols right after the header, partitions at the next 64-B
  // boundary, nothing after them -- so every byte of the file is under a CRC.
  if (h.symbols_offset != align_up(sizeof(DbManifestHeader), kSurfaceDbSectionAlign) ||
      h.partitions_offset != align_up(h.symbols_offset + symbols_bytes, kSurfaceDbSectionAlign) ||
      payload_end != h.file_size) {
    return Err(ErrorCode::ParseError, "DbManifest::open: section placement is not canonical");
  }
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
    if (rec.symbol_len == 0 || rec.symbol_len > kSurfaceDbKeyMax ||
        !key_tail_zero(rec.symbol, rec.symbol_len)) {
      return Err(ErrorCode::ParseError, "DbManifest::open: invalid symbol_len");
    }
    if (!symbol_record_enums_valid(rec)) {
      return Err(ErrorCode::ParseError, "DbManifest::open: bad enum wire value for symbol " +
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
    if (rec.key_len == 0 || rec.key_len > kSurfaceDbKeyMax || !key_tail_zero(rec.key, rec.key_len) ||
        rec.flags != 0u) {
      return Err(ErrorCode::ParseError, "DbManifest::open: invalid partition record");
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

Result<std::optional<SurfaceProvenance>>
DbManifest::find_symbol_provenance(std::string_view symbol) const {
  const std::string canon = detail::canonicalize_symbol(symbol, kSurfaceDbKeyMax);
  if (!canon.empty()) {
    const auto len = static_cast<std::uint16_t>(canon.size());
    const auto it =
        std::lower_bound(symbols_.begin(), symbols_.end(), canon,
                         [len](const DbSymbolRecord &rec, const std::string &key) {
                           return cmp_key(rec.symbol, rec.symbol_len, key.data(), len) < 0;
                         });
    if (it != symbols_.end() &&
        cmp_key(it->symbol, it->symbol_len, canon.data(), len) == 0) {
      return Ok(decode_symbol_provenance(*it));
    }
  }
  return Err(ErrorCode::NotFound,
             "DbManifest::find_symbol_provenance: symbol not present");
}

const DbPartitionRecord *DbManifest::find_partition(std::string_view key) const noexcept {
  const std::string canon = detail::canonicalize_symbol(key, kSurfaceDbKeyMax);
  if (canon.empty()) {
    return nullptr;
  }
  const auto len = static_cast<std::uint16_t>(canon.size());
  const auto it = std::lower_bound(partitions_.begin(), partitions_.end(), canon,
                                   [len](const DbPartitionRecord &rec, const std::string &key2) {
                                     return cmp_key(rec.key, rec.key_len, key2.data(), len) < 0;
                                   });
  if (it != partitions_.end() && cmp_key(it->key, it->key_len, canon.data(), len) == 0) {
    return &*it;
  }
  return nullptr;
}

// ── SurfaceDb: file IO helpers ─────────────────────────────────────────────

namespace {

// Atomic manifest write: serialize to an exclusively reserved unique
// same-directory temp, then durably rename over `dst`.
// Mirrors write_surface_archive_v2_file's discipline (surface_archive.cpp) --
// including tmp cleanup on failure -- so both binary formats fail the same
// way under a crash mid-write.
[[nodiscard]] Status write_manifest_file_atomic(const std::filesystem::path &dst,
                                                const std::vector<std::byte> &bytes) {
  auto reserved = detail::reserve_unique_publish_temp_file(dst.string());
  if (!reserved) {
    return tl::unexpected<atx::core::Error>(std::move(reserved).error());
  }
  const std::filesystem::path tmp{*reserved};
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "SurfaceDb: cannot open manifest temp file");
    }
    os.write(reinterpret_cast<const char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "SurfaceDb: manifest write failed");
    }
  }
  // Durable atomic publish: fsync the temp before the rename, retry under a
  // reader-held manifest, preserve the temp on final failure (C3 / SE-P2-1,
  // SE-P2-2). Shared primitive with the v1 + v2 archive writers.
  return detail::flush_and_publish_file(tmp.string(), dst.string());
}

// Read the manifest fully with ONE positional read (a delete-sharing handle, so a
// concurrent manifest publish is never refused because of this reader). NotFound iff
// the open reports the file absent; IoError on any other failure.
[[nodiscard]] Result<std::vector<std::byte>> read_file_fully(const std::filesystem::path &p) {
  auto file = detail::PositionalFile::open(p.string());
  if (!file) {
    if (file.error().code() == ErrorCode::NotFound) {
      return Err(ErrorCode::NotFound, "SurfaceDb: manifest not found");
    }
    return Err(file.error());
  }
  if (file->size() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return Err(ErrorCode::IoError, "SurfaceDb: manifest too large to buffer");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(file->size()));
  if (Status st = file->read_at(0, bytes.data(), bytes.size()); !st) {
    return Err(st.error());
  }
  return Ok(std::move(bytes));
}

// Decode a manifest's partition records back into writer-input form -- shared
// by the public partitions() query and every mutation (which must round-trip
// the untouched partitions through persist_locked alongside the symbol edit).
[[nodiscard]] std::vector<DbPartitionInfo>
decode_partitions(std::span<const DbPartitionRecord> recs) {
  std::vector<DbPartitionInfo> out;
  out.reserve(recs.size());
  for (const DbPartitionRecord &rec : recs) {
    DbPartitionInfo info;
    info.key.assign(rec.key, rec.key_len);
    info.surface_count = rec.surface_count;
    info.file_size = rec.file_size;
    info.created_ts_ns = rec.created_ts_ns;
    info.config_fingerprint = rec.config_fingerprint; // FIX-D; 0 = unknown
    out.push_back(std::move(info));
  }
  return out;
}

// Decode a manifest's symbol records back into writer-input form -- shared by
// every partition mutation, which must round-trip the untouched symbol table
// through persist_locked alongside the partition edit (partitions() and the
// symbol table are orthogonal namespaces; see surface_db.hpp).
[[nodiscard]] std::vector<DbSymbolEntry>
decode_symbol_entries(std::span<const DbSymbolRecord> recs) {
  std::vector<DbSymbolEntry> out;
  out.reserve(recs.size());
  for (const DbSymbolRecord &rec : recs) {
    out.push_back(DbSymbolEntry{std::string_view(rec.symbol, rec.symbol_len),
                                decode_symbol_record(rec),
                                decode_symbol_provenance(rec)});
  }
  return out;
}

} // namespace

// ── SurfaceDb: create/open ──────────────────────────────────────────────

std::string SurfaceDb::manifest_path() const {
  return (std::filesystem::path(root_) / std::string(kSurfaceDbManifestName)).string();
}

Result<SurfaceDb> SurfaceDb::create(std::string_view root, const SurfaceDbCreateOpts &opts) {
  const std::filesystem::path root_path{std::string(root)};
  const std::filesystem::path manifest_file = root_path / std::string(kSurfaceDbManifestName);

  std::error_code exists_ec;
  const bool manifest_present = std::filesystem::exists(manifest_file, exists_ec);
  if (exists_ec) {
    return Err(ErrorCode::IoError, "SurfaceDb::create: failed to stat root");
  }
  if (manifest_present) {
    return Err(ErrorCode::AlreadyExists, "SurfaceDb::create: manifest already exists at root");
  }

  std::error_code mkdir_ec;
  std::filesystem::create_directories(root_path / std::string(kSurfaceDbPartitionDir), mkdir_ec);
  if (mkdir_ec) {
    return Err(ErrorCode::IoError, "SurfaceDb::create: failed to create partitions directory");
  }

  auto bytes = write_db_manifest({}, {}, {.generation = 1, .created_ts_ns = opts.created_ts_ns});
  if (!bytes) {
    return Err(bytes.error());
  }
  auto wrote = write_manifest_file_atomic(manifest_file, *bytes);
  if (!wrote) {
    return Err(wrote.error());
  }

  return open(root, SurfaceDbOpenOpts{});
}

Result<SurfaceDb> SurfaceDb::open(std::string_view root, const SurfaceDbOpenOpts & /*opts*/) {
  SurfaceDb db;
  db.root_ = std::string(root);
  db.mu_ = std::make_unique<std::mutex>();

  auto bytes = read_file_fully(db.manifest_path());
  if (!bytes) {
    return Err(bytes.error());
  }
  auto parsed = DbManifest::open(std::move(*bytes));
  if (!parsed) {
    return Err(parsed.error());
  }
  db.snapshot_ = std::make_shared<const DbManifest>(std::move(*parsed));
  return Ok(std::move(db));
}

// ── SurfaceDb: manifest snapshot queries ──────────────────────────────────

std::shared_ptr<const DbManifest> SurfaceDb::manifest() const {
  std::lock_guard<std::mutex> lock(*mu_);
  return snapshot_;
}

std::uint64_t SurfaceDb::generation() const { return manifest()->generation(); }

std::vector<std::string> SurfaceDb::symbols() const {
  const std::shared_ptr<const DbManifest> snap = manifest();
  std::vector<std::string> out;
  out.reserve(snap->symbols().size());
  for (const DbSymbolRecord &rec : snap->symbols()) {
    out.emplace_back(rec.symbol, rec.symbol_len);
  }
  return out;
}

Result<SymbolFitConfig> SurfaceDb::symbol_config(std::string_view symbol) const {
  return manifest()->find_symbol(symbol);
}

Result<std::optional<SurfaceProvenance>>
SurfaceDb::surface_provenance(std::string_view symbol) const {
  return manifest()->find_symbol_provenance(symbol);
}

std::vector<DbPartitionInfo> SurfaceDb::partitions() const {
  return decode_partitions(manifest()->partitions());
}

std::uint64_t SurfaceDb::config_fingerprint(std::span<const std::string> symbols) const {
  std::vector<std::string> canon;
  canon.reserve(symbols.size());
  for (const std::string &sym : symbols) {
    canon.push_back(detail::canonicalize_symbol(sym, kSurfaceDbKeyMax));
  }
  return fold_symbol_configs(manifest()->symbols(), std::move(canon));
}

std::uint64_t SurfaceDb::partition_config_fingerprint(std::string_view key) const {
  // ONE FULL-EXPRESSION, deliberately (REV-R5, review M-9). `manifest()` returns a
  // `shared_ptr<const DbManifest>` BY VALUE; written as two statements
  //
  //     const DbPartitionRecord *rec = manifest()->find_partition(key);
  //     return rec != nullptr ? rec->config_fingerprint : 0u;
  //
  // the temporary shared_ptr is destroyed at the semicolon of the FIRST line and
  // `rec` — which points into the manifest's mapped bytes — is dereferenced on the
  // second with no owner held. It survives today only because `snapshot_` holds a
  // second reference and this path is single-threaded through the build; a
  // concurrent `refresh`/`persist` swapping `snapshot_` between the two lines
  // would drop the last owner and leave `rec` dangling.
  //
  // The fix is a NAMED OWNER spanning both the lookup and the load — the same
  // shape `SurfaceDb::symbols()` above already uses, and this file's idiom for any
  // query that touches manifest bytes across a statement boundary.
  // `partition_listed` below solves it the other way and is equally correct: it
  // consumes the pointer (`!= nullptr`) inside the very full-expression that
  // produced the owner, so the temporary outlives the use. This one DEREFERENCES,
  // so it needs the owner past the semicolon.
  const std::shared_ptr<const DbManifest> m = manifest();
  const DbPartitionRecord *rec = m->find_partition(key);
  return rec != nullptr ? rec->config_fingerprint : 0u;
}

// ── SurfaceDb: manifest mutation ──────────────────────────────────────────

namespace {

// Task D3: every public mutator acquires this BEFORE `std::lock_guard
// lock(*mu_)` -- see detail/writer_lock.hpp's "LOCK ORDERING" doc comment
// for why that order is load-bearing (a cross-process wait taken under `mu_`
// would stall every in-process reader for up to WriterLock::kDefaultTimeout,
// not just other writers). `persist_locked` itself no longer acquires this
// lock -- it is a precondition that the caller already holds it (alongside
// `mu_`) for the whole read-modify-publish window.
[[nodiscard]] Result<detail::WriterLock> acquire_manifest_writer_lock(const std::string &manifest) {
  return detail::WriterLock::acquire(manifest + ".lock");
}

} // namespace

// PRECONDITION (Task D3): the caller already holds a `WriterLock` on this
// manifest's `<path>.lock` (acquired via `acquire_manifest_writer_lock`,
// BEFORE `mu_`) for the duration of this call, in addition to `mu_` itself.
// This function does not acquire or check either lock -- it trusts the
// caller, exactly as it already trusted the caller to hold `mu_` before this
// task existed. `mu_` above only serializes mutations through THIS SurfaceDb
// instance -- a second handle on the same root (another process, or another
// handle in this one) does not share it, so without the WriterLock two
// writers could both read generation N, both compute N+1, and both publish:
// the LAST atomic rename wins and the other writer's update vanishes with no
// error.
Status SurfaceDb::persist_locked(std::vector<DbSymbolEntry> symbols,
                                 std::vector<DbPartitionInfo> partitions) {
  const std::string manifest = manifest_path();

  // Re-read the on-disk manifest UNDER the lock: `symbols`/`partitions` were
  // computed by the caller (e.g. upsert_symbol) against the OLD `snapshot_`,
  // so if another writer advanced the generation since then, publishing them
  // now would silently overwrite whatever that writer just committed with
  // content that never accounted for it. Fail cleanly instead -- Unavailable,
  // not a corrupted or lost write -- and resync `snapshot_` so the caller's
  // very next retry decides against fresh state.
  auto current_bytes = read_file_fully(manifest);
  if (!current_bytes) {
    return Err(current_bytes.error());
  }
  auto current = DbManifest::open(std::move(*current_bytes));
  if (!current) {
    return Err(current.error());
  }
  if (current->generation() != snapshot_->generation()) {
    snapshot_ = std::make_shared<const DbManifest>(std::move(*current));
    return Err(ErrorCode::Unavailable,
               "SurfaceDb::persist_locked: manifest was concurrently advanced by another "
               "writer; retry against the refreshed snapshot");
  }

  SurfaceDbManifestWriteOpts write_opts;
  write_opts.generation = snapshot_->generation() + 1;
  write_opts.created_ts_ns = snapshot_->header().created_ts_ns;
  write_opts.updated_ts_ns = 0; // now
  write_opts.flags = snapshot_->header().flags;

  auto bytes = write_db_manifest(symbols, partitions, write_opts);
  if (!bytes) {
    return Err(bytes.error());
  }
  // Parse-validate the freshly serialized bytes IN MEMORY before this
  // mutation ever touches disk. `DbManifest::open` takes its argument by
  // value, so this is a deliberate COPY of `*bytes` -- it leaves the
  // original intact for the atomic write below and means the writer and the
  // parser can never quietly disagree about wire validity: any future
  // writer/reader asymmetry (e.g. an enum the writer forgot to range-check)
  // fails the mutation cleanly right here with the original manifest file
  // still on disk, instead of atomically renaming bytes the parser would go
  // on to reject and permanently bricking every subsequent
  // SurfaceDb::open/refresh in every process.
  auto parsed = DbManifest::open(*bytes);
  if (!parsed) {
    return Err(parsed.error());
  }
  auto wrote = write_manifest_file_atomic(manifest_path(), *bytes);
  if (!wrote) {
    return Err(wrote.error());
  }
  // Swap in the already-parsed manifest -- exactly what a fresh reader would
  // parse from the bytes just written, with no second parse needed.
  snapshot_ = std::make_shared<const DbManifest>(std::move(*parsed));
  return Ok();
}

Status SurfaceDb::upsert_symbol(std::string_view symbol, const SymbolFitConfig &cfg,
                                std::optional<SurfaceProvenance> provenance) {
  const std::string canon = detail::canonicalize_symbol(symbol, kSurfaceDbKeyMax);
  if (canon.empty()) {
    return Err(ErrorCode::InvalidArgument, "SurfaceDb::upsert_symbol: empty canonical symbol");
  }

  // Lock order (detail/writer_lock.hpp): WriterLock BEFORE mu_.
  auto file_lock = acquire_manifest_writer_lock(manifest_path());
  if (!file_lock) {
    return Err(file_lock.error());
  }
  std::lock_guard<std::mutex> lock(*mu_);
  const std::shared_ptr<const DbManifest> snap = snapshot_;

  std::vector<DbSymbolEntry> entries;
  entries.reserve(snap->symbols().size() + 1);
  bool replaced = false;
  for (const DbSymbolRecord &rec : snap->symbols()) {
    const std::string_view rec_sym(rec.symbol, rec.symbol_len);
    if (rec_sym == canon) {
      entries.push_back(DbSymbolEntry{
          canon, cfg, provenance.has_value() ? provenance
                                             : decode_symbol_provenance(rec)});
      replaced = true;
    } else {
      entries.push_back(DbSymbolEntry{rec_sym, decode_symbol_record(rec),
                                      decode_symbol_provenance(rec)});
    }
  }
  if (!replaced) {
    entries.push_back(DbSymbolEntry{canon, cfg, provenance});
  }

  return persist_locked(std::move(entries), decode_partitions(snap->partitions()));
}

Status SurfaceDb::upsert_symbols(std::span<const DbSymbolEntry> upserts) {
  if (upserts.empty()) {
    return Ok(); // vacuous batch: nothing to validate, nothing to persist
  }
  // Canonicalize + name-validate every entry up front (before the lock), and
  // resolve the batch's LAST-WINS rule: `last_of[canon]` is the index of the
  // entry that survives for that name.
  std::vector<std::string> canon(upserts.size());
  std::map<std::string, std::size_t> last_of;
  for (std::size_t i = 0; i < upserts.size(); ++i) {
    canon[i] = detail::canonicalize_symbol(upserts[i].symbol, kSurfaceDbKeyMax);
    if (canon[i].empty()) {
      return Err(ErrorCode::InvalidArgument, "SurfaceDb::upsert_symbols: empty canonical symbol");
    }
    last_of[canon[i]] = i;
  }

  // Lock order (detail/writer_lock.hpp): WriterLock BEFORE mu_ -- acquired
  // even though the batch may turn out `!changed` (a pure no-op write)
  // below, since that can only be known after `mu_` is taken.
  auto file_lock = acquire_manifest_writer_lock(manifest_path());
  if (!file_lock) {
    return Err(file_lock.error());
  }
  std::lock_guard<std::mutex> lock(*mu_);
  const std::shared_ptr<const DbManifest> snap = snapshot_;

  // Walk the stored records once: an untouched record round-trips verbatim; a
  // record named in the batch is replaced in place (nullopt provenance keeps the
  // stored one — upsert_symbol semantics). `changed` tracks whether ANY replaced
  // record's ENCODED bytes actually moved: the re-seed path hands this function
  // the exact configs already stored, and rewriting a byte-identical manifest N
  // symbols wide on every resume is the O(N^2) cost this batch API exists to
  // delete. DbSymbolRecord is a fixed-layout, zero-initialized POD, so memcmp is
  // the precise "would this write change the file" predicate.
  std::vector<DbSymbolEntry> entries;
  entries.reserve(snap->symbols().size() + last_of.size());
  bool changed = false;
  std::size_t n_replaced = 0;
  for (const DbSymbolRecord &rec : snap->symbols()) {
    const std::string_view rec_sym(rec.symbol, rec.symbol_len);
    const auto it = last_of.find(std::string(rec_sym));
    if (it == last_of.end()) {
      entries.push_back(
          DbSymbolEntry{rec_sym, decode_symbol_record(rec), decode_symbol_provenance(rec)});
      continue;
    }
    const DbSymbolEntry &up = upserts[it->second];
    std::optional<SurfaceProvenance> prov =
        up.provenance.has_value() ? up.provenance : decode_symbol_provenance(rec);
    const DbSymbolRecord next = encode_symbol_record(rec_sym, up.config, prov);
    if (std::memcmp(&next, &rec, sizeof rec) != 0) {
      changed = true;
    }
    entries.push_back(DbSymbolEntry{rec_sym, up.config, std::move(prov)});
    ++n_replaced;
    it->second = static_cast<std::size_t>(-1); // consumed by the replace walk
  }
  // Append genuinely new symbols in batch order (each name's surviving entry).
  if (n_replaced < last_of.size()) {
    for (std::size_t i = 0; i < upserts.size(); ++i) {
      const auto it = last_of.find(canon[i]);
      if (it->second != i) {
        continue; // superseded within the batch, or already consumed above
      }
      entries.push_back(DbSymbolEntry{canon[i], upserts[i].config, upserts[i].provenance});
      changed = true;
    }
  }
  if (!changed) {
    return Ok(); // every entry byte-identical to its stored record: zero-write
  }
  return persist_locked(std::move(entries), decode_partitions(snap->partitions()));
}

Status SurfaceDb::remove_symbol(std::string_view symbol) {
  const std::string canon = detail::canonicalize_symbol(symbol, kSurfaceDbKeyMax);
  if (canon.empty()) {
    return Err(ErrorCode::InvalidArgument, "SurfaceDb::remove_symbol: empty canonical symbol");
  }

  // Lock order (detail/writer_lock.hpp): WriterLock BEFORE mu_.
  auto file_lock = acquire_manifest_writer_lock(manifest_path());
  if (!file_lock) {
    return Err(file_lock.error());
  }
  std::lock_guard<std::mutex> lock(*mu_);
  const std::shared_ptr<const DbManifest> snap = snapshot_;

  std::vector<DbSymbolEntry> entries;
  entries.reserve(snap->symbols().size());
  bool found = false;
  for (const DbSymbolRecord &rec : snap->symbols()) {
    const std::string_view rec_sym(rec.symbol, rec.symbol_len);
    if (rec_sym == canon) {
      found = true;
      continue;
    }
    entries.push_back(DbSymbolEntry{rec_sym, decode_symbol_record(rec),
                                    decode_symbol_provenance(rec)});
  }
  if (!found) {
    return Err(ErrorCode::NotFound, "SurfaceDb::remove_symbol: symbol not present");
  }

  return persist_locked(std::move(entries), decode_partitions(snap->partitions()));
}

Status SurfaceDb::refresh() {
  std::lock_guard<std::mutex> lock(*mu_);

  // The generation probe reads the header only, through a delete-sharing handle that
  // is closed before the full read. Torn reads are impossible: every manifest version
  // is published by an atomic rename, the probe only decides WHETHER to reload, and the
  // reload parses (and CRC-checks) whichever complete version it opens.
  const std::filesystem::path path = manifest_path();
  DbManifestHeader hdr{};
  {
    auto file = detail::PositionalFile::open(path.string());
    if (!file) {
      return Err(ErrorCode::IoError, "SurfaceDb::refresh: cannot open manifest file");
    }
    if (file->size() < sizeof hdr) {
      return Err(ErrorCode::ParseError, "SurfaceDb::refresh: short read of manifest header");
    }
    if (Status st = file->read_at(0, reinterpret_cast<std::byte *>(&hdr), sizeof hdr); !st) {
      return Err(st.error());
    }
  }

  if (hdr.generation <= snapshot_->generation()) {
    return Ok();
  }

  auto bytes = read_file_fully(path);
  if (!bytes) {
    return Err(bytes.error());
  }
  auto parsed = DbManifest::open(std::move(*bytes));
  if (!parsed) {
    return Err(parsed.error());
  }
  snapshot_ = std::make_shared<const DbManifest>(std::move(*parsed));
  return Ok();
}

// ── SurfaceDb: partition IO ────────────────────────────────────────────────

std::string SurfaceDb::partition_path(std::string_view canonical_key) const {
  return (std::filesystem::path(root_) / std::string(kSurfaceDbPartitionDir) /
          (std::string(canonical_key) + std::string(kSurfaceDbPartitionExt)))
      .string();
}

Status SurfaceDb::write_partition(std::string_view key, std::span<const SurfaceArchiveItem> items,
                                  const ArchiveV2WriteOpts &opts, DbConfigAttestation attest) {
  auto canon = canonicalize_key(key);
  if (!canon) {
    return Err(canon.error());
  }
  const std::string path = partition_path(*canon);

  // The archive write is itself atomic (tmp+rename, see write_surface_archive_v2_file);
  // do it BEFORE touching the manifest so a failed/interrupted archive write
  // (e.g. empty `items` -> InvalidArgument) never advances the partition index.
  auto wrote = write_surface_archive_v2_file(path, items, opts);
  if (!wrote) {
    return Err(wrote.error());
  }

  std::error_code size_ec;
  const std::uintmax_t file_size = std::filesystem::file_size(path, size_ec);
  if (size_ec) {
    return Err(ErrorCode::IoError, "SurfaceDb::write_partition: cannot stat partition file");
  }

  // Lock order (detail/writer_lock.hpp): WriterLock BEFORE mu_. (The archive
  // file write above is keyed by `key`, not by manifest generation, so it
  // does not itself need the cross-process lock's protection -- only the
  // manifest read-modify-publish below does.)
  auto file_lock = acquire_manifest_writer_lock(manifest_path());
  if (!file_lock) {
    return Err(file_lock.error());
  }
  std::lock_guard<std::mutex> lock(*mu_);
  const std::shared_ptr<const DbManifest> snap = snapshot_;

  std::vector<DbPartitionInfo> parts = decode_partitions(snap->partitions());
  const auto it = std::find_if(parts.begin(), parts.end(),
                               [&](const DbPartitionInfo &p) { return p.key == *canon; });
  // FIX-D: record the configs these surfaces were produced under, so a later
  // resume can tell whether reusing them is still sound. Computed from the
  // manifest snapshot (which already reflects any upsert_symbol) over exactly the
  // symbols going into this partition; the provenance this call is about to write
  // back is excluded from the fold, so the value is stable across rewrites that
  // change nothing but provenance.
  //
  // FIX-D fix-1 (I5): only when the caller ATTESTS that it produced these
  // surfaces under those configs. Without the attestation the fingerprint stays 0
  // -- "unknown", never carried -- because this function cannot check the claim
  // and a wrong one silently reuses a stale surface. Note the fold is skipped
  // entirely, not computed and discarded: an unattested write must not pay the
  // manifest walk either.
  std::uint64_t fingerprint = 0u;
  if (attest == DbConfigAttestation::FitterProduced) {
    std::vector<std::string> written_symbols;
    written_symbols.reserve(items.size());
    for (const SurfaceArchiveItem &item : items) {
      written_symbols.push_back(detail::canonicalize_symbol(item.symbol, kSurfaceDbKeyMax));
    }
    fingerprint = fold_symbol_configs(snap->symbols(), std::move(written_symbols));
  }
  DbPartitionInfo info{*canon, static_cast<std::uint32_t>(items.size()),
                       static_cast<std::uint64_t>(file_size), wall_clock_ns(), fingerprint};
  if (it != parts.end()) {
    *it = info; // rewrite: overwriting an existing key is allowed.
  } else {
    parts.push_back(std::move(info));
  }

  std::vector<DbSymbolEntry> symbol_entries = decode_symbol_entries(snap->symbols());
  for (const SurfaceArchiveItem &item : items) {
    if (!item.provenance.has_value()) {
      continue;
    }
    const std::string symbol =
        detail::canonicalize_symbol(item.symbol, kSurfaceDbKeyMax);
    for (DbSymbolEntry &entry : symbol_entries) {
      if (entry.symbol == symbol) {
        entry.provenance = item.provenance;
        break;
      }
    }
  }

  return persist_locked(std::move(symbol_entries), std::move(parts));
}

Result<SurfaceArchiveV2> SurfaceDb::open_partition(std::string_view key) const {
  auto canon = canonicalize_key(key);
  if (!canon) {
    return Err(canon.error());
  }
  const std::shared_ptr<const DbManifest> snap = manifest();
  if (snap->find_partition(*canon) == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceDb::open_partition: partition not present");
  }
  // Whole-board callers: one positional read into an owned buffer (no stream, no
  // mapping left against the file).
  return SurfaceArchiveV2::open_copied(partition_path(*canon));
}

// REV-R3 fix-1 (review I-1). The same open with the manifest gate REMOVED — see
// the header for why this is a separate entry point rather than a widening of
// `open_partition`. The one thing this function is for is keeping "there is no
// file" apart from "there is a file I could not use", because its caller turns
// the first into a whole-file overwrite.
//
// The split is the open's own error code (`detail::PositionalFile::open`):
// NotFound only for ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND (ENOENT/ENOTDIR),
// IoError for every other refusal — denied ACL, sharing violation, a directory, a
// volume or SMB fault. Asking the open that also reads the bytes removes revision
// 0's separate `exists` probe and with it the window between the probe and the read.
// Armed with a real ERROR_ACCESS_DENIED by
// `SurfaceDbPartition.OpenPartitionFileDoesNotReportAnUnanswerableProbeAsAbsent`.
Result<SurfaceArchiveV2> SurfaceDb::open_partition_file(std::string_view key) const {
  auto canon = canonicalize_key(key);
  if (!canon) {
    return Err(canon.error());
  }
  return SurfaceArchiveV2::open_copied(partition_path(*canon));
}

// REV-R3 fix-2 (review N-3). Does the MANIFEST list `key`? Pure snapshot lookup:
// no file I/O, no partition cache, no archive open. It is the other half of the
// question `open_partition` fuses — `open_partition` = listed AND openable — and
// the coverage guard needs the halves separately so its refusal banner can say
// WHICH disagreement it hit. `false` for a key that will not canonicalize: an
// unrepresentable key cannot be listed under any spelling.
bool SurfaceDb::partition_listed(std::string_view key) const {
  auto canon = canonicalize_key(key);
  if (!canon) {
    return false;
  }
  return manifest()->find_partition(*canon) != nullptr;
}

Result<std::string> SurfaceDb::listed_partition_path(std::string_view key,
                                                     std::string_view caller) const {
  auto canon = canonicalize_key(key);
  if (!canon) {
    return Err(canon.error());
  }
  if (manifest()->find_partition(*canon) == nullptr) {
    return Err(ErrorCode::NotFound, std::string(caller) + ": partition not present");
  }
  return Ok(partition_path(*canon));
}

Result<std::int64_t> SurfaceDb::session_ts(std::string_view key) const {
  auto path = listed_partition_path(key, "SurfaceDb::session_ts");
  if (!path) {
    return Err(path.error());
  }
  return detail::read_archive_v2_first_now_ts(*path);
}

// Both single-surface reads are POINT READS (docs/atxvsa2-format.md §4, §10): the
// archive header, the probed lookup slot(s) and the one record — O(record) I/O
// whatever the partition's size — through one delete-sharing handle that is closed
// before the call returns. Each call therefore reads ONE consistent file version, and
// the revision-0 partition cache (whole-file buffers keyed by a content identity read
// outside its lock, kept stale when the identity was unreadable) has no successor.
Result<PricedSurface> SurfaceDb::load_surface(std::string_view key, std::string_view symbol) const {
  auto path = listed_partition_path(key, "SurfaceDb::load_surface");
  if (!path) {
    return Err(path.error());
  }
  auto got = point_read_archive_reconstruct(*path, symbol);
  if (!got) {
    return Err(got.error());
  }
  return Ok(std::move(got->archived.surface));
}

Result<LoadedSurface> SurfaceDb::map_surface(std::string_view key, std::string_view symbol) const {
  auto path = listed_partition_path(key, "SurfaceDb::map_surface");
  if (!path) {
    return Err(path.error());
  }
  auto got = point_read_archive_surface(*path, symbol);
  if (!got) {
    return Err(got.error());
  }
  return LoadedSurface{std::move(got->backing), std::move(got->view)};
}

Status SurfaceDb::drop_partition(std::string_view key) {
  auto canon = canonicalize_key(key);
  if (!canon) {
    return Err(canon.error());
  }
  const std::string path = partition_path(*canon);

  // Lock order (detail/writer_lock.hpp): WriterLock BEFORE mu_ -- acquired
  // even though the key may turn out absent (NotFound below), since that can
  // only be known after `mu_` is taken.
  auto file_lock = acquire_manifest_writer_lock(manifest_path());
  if (!file_lock) {
    return Err(file_lock.error());
  }
  std::lock_guard<std::mutex> lock(*mu_);
  const std::shared_ptr<const DbManifest> snap = snapshot_;
  if (snap->find_partition(*canon) == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceDb::drop_partition: partition not present");
  }

  std::vector<DbPartitionInfo> parts = decode_partitions(snap->partitions());
  parts.erase(std::remove_if(parts.begin(), parts.end(),
                             [&](const DbPartitionInfo &p) { return p.key == *canon; }),
              parts.end());

  auto persisted = persist_locked(decode_symbol_entries(snap->symbols()), std::move(parts));
  if (!persisted) {
    return Err(persisted.error());
  }

  // Manifest-first ordering is deliberate, not incidental: the index entry is
  // already gone (and generation already bumped) by the time we get here, so
  // a crash right at this line leaves an orphaned .atxvsa file under
  // partitions/ that no READER ever sees (the manifest no longer lists it, so
  // open_partition/load_surface correctly report NotFound). The reverse order
  // -- unlink the file, then edit the manifest -- would risk a crash between
  // the two steps that leaves a manifest entry pointing at a now-missing file:
  // every later open_partition/load_surface for that key would then surface a
  // confusing IoError/NotFound-on-open instead of a clean "no such partition."
  // The unlink stays under `*mu_` (rather than after releasing it) so an
  // in-process write_partition on the SAME key racing on another thread cannot
  // land its archive write and manifest entry inside the gap between this
  // manifest commit and the unlink; see surface_db.hpp's thread-safety note for
  // the residual ordering this narrows but does not fully close
  // (write_partition's own archive write still happens before it takes the
  // lock).
  //
  // REV-R3 fix-2 (review N-2). THE ORPHAN IS NO LONGER HARMLESS, which is what
  // this comment used to say. It is still invisible to readers and a direct
  // `write_partition` for the same key still overwrites it silently -- but the
  // BUILD path, which is the only thing that writes partitions in production,
  // now runs a coverage guard that opens the partition FILE without asking the
  // manifest (`open_partition_file`, added for exactly this class of
  // manifest/file disagreement). That guard reads the orphan, sees symbols the
  // new candidate lacks, and REFUSES to rewrite the date -- on every run, until
  // someone deletes the file or rebuilds over the full board set. So an
  // interrupted drop no longer costs nothing; it costs that date's rebuilds.
  //
  // Which is why the unlink error is now returned instead of discarded: a
  // failed unlink produces the same orphan with no crash at all, and on Windows
  // it is not exotic (a live mapping in another process blocks it). Reporting
  // it is a PARTIAL SUCCESS -- the manifest commit above already happened and
  // is not rolled back, the key really is dropped from the index, and a caller
  // that retries gets `NotFound`. The message says so, because "drop_partition
  // failed" would otherwise read as "nothing happened". `remove` on an already
  // absent path returns false with `ec` CLEARED, so a double-unlink is not an
  // error here.
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return Err(ErrorCode::IoError,
               "SurfaceDb::drop_partition: the manifest entry for '" + *canon +
                   "' was removed, but its partition file could not be unlinked (" + ec.message() +
                   "); the file is now an orphan that the build's coverage guard will read and "
                   "defend -- delete " + path + " by hand");
  }
  return Ok();
}

} // namespace atx::vol
