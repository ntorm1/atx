#include "atx/vol/surface_archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/bit.hpp" // next_pow2, is_pow2
#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"               // hash_bytes
#include "atx/tsdb/mapping.hpp"            // tsdb::Mapping (read-only mmap owner, S2)
#include "atx/vol/american.hpp"            // AlOpts, AmericanMethod
#include "atx/vol/dense_slice.hpp"         // ConvexSliceFit
#include "atx/vol/detail/archive_util.hpp" // crc32c, crc32c_update, align_up, canonicalize_symbol
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/vol_curve.hpp"   // IVolCurve, Convex/Essvi/SviCurve, CurveSurface
#include "atx/vol/vol_surface.hpp" // EssviParams, SviParams

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// Shared with surface_db.hpp (ATXVDB) so both binary formats agree bit-for-bit
// on CRC-32C and canonical-symbol bytes. See detail/archive_util.hpp.
using detail::align_up;
using detail::crc32c;
using detail::crc32c_update;

// The POD slice structs are serialized verbatim, so they must be trivially
// copyable for the memcpy round-trip to be well-defined.
static_assert(std::is_trivially_copyable_v<EssviParams>,
              "EssviParams must be trivially copyable for byte serialization");
static_assert(std::is_trivially_copyable_v<SviParams>,
              "SviParams must be trivially copyable for byte serialization");
static_assert(std::is_trivially_copyable_v<C8Params>,
              "C8Params must be trivially copyable for byte serialization");
static_assert(std::is_trivially_copyable_v<AlOpts>,
              "AlOpts must be trivially copyable for byte serialization");

// schema_hash() folds sizeof(EssviParams)/sizeof(SviParams) so a reader built
// against a different struct shape rejects the file. C8Params cannot join that
// fold: its kind postdates the v3 fingerprint, and adding it would invalidate
// every already-written archive that contains no C8 slice at all. Freeze the
// layout here instead -- same protection, paid at compile time. If you change
// C8Params, bump kV3Salt in schema_hash() and update this size.
static_assert(sizeof(C8Params) == 176,
              "C8Params layout is serialized verbatim; changing it must bump the archive "
              "schema salt (see schema_hash) so stale C8 archives are rejected, not mis-read");

namespace {

// ── Small helpers ────────────────────────────────────────────────────────
// CRC-32C, align_up, and symbol canonicalization live in
// atx::vol::detail (detail/archive_util.hpp/.cpp) so surface_db.cpp can share
// a bit-identical implementation; see the `using detail::...;` aliases above.


[[nodiscard]] std::byte *buf_at(std::vector<std::byte> &b, std::uint64_t off) noexcept {
  return b.data() + static_cast<std::size_t>(off);
}

// Bits 0..11 — includes ValidationFailure::CarryGap (1u << 11), the
// publish-with-Degraded reason: a Degraded+CarryGap provenance is a routinely
// SERVED state and must round-trip the archive, not be refused as unknown.
constexpr std::uint32_t kKnownValidationFailures = (1u << 12) - 1u;

[[nodiscard]] bool provenance_record_valid(const ArchiveSurfaceProvenanceRecord &record) noexcept {
  const bool fields_valid = record.marker == kArchiveProvenanceMarker && record.purpose <= 1u &&
                            record.quality_mode <= 2u && record.state <= 3u &&
                            record.reserved0 == 0u && record.reserved1 == 0u &&
                            (record.validation_failures & ~kKnownValidationFailures) == 0u;
  const bool healthy_is_clean = record.state != static_cast<std::uint8_t>(SurfaceState::Healthy) ||
                                record.validation_failures == 0u;
  return fields_valid && healthy_is_clean;
}

[[nodiscard]] ArchiveSurfaceProvenanceRecord
to_provenance_record(const SurfaceProvenance &provenance) noexcept {
  ArchiveSurfaceProvenanceRecord record{};
  record.marker = kArchiveProvenanceMarker;
  record.purpose = static_cast<std::uint8_t>(provenance.purpose);
  record.quality_mode = static_cast<std::uint8_t>(provenance.quality_mode);
  record.state = static_cast<std::uint8_t>(provenance.state);
  record.validation_failures = static_cast<std::uint32_t>(provenance.validation.failures);
  record.validation_id = provenance.validation.validation_id;
  record.source_generation = provenance.source_generation;
  record.served_generation = provenance.served_generation;
  return record;
}

[[nodiscard]] Result<SurfaceProvenance>
from_provenance_record(const ArchiveSurfaceProvenanceRecord &record) {
  if (record.marker == 0u) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&record);
    if (!std::all_of(bytes, bytes + sizeof record,
                     [](std::uint8_t value) { return value == 0u; })) {
      return Err(ErrorCode::ParseError, "surface archive: malformed legacy provenance bytes");
    }
    return Ok(legacy_surface_provenance());
  }
  if (!provenance_record_valid(record)) {
    return Err(ErrorCode::ParseError, "surface archive: invalid surface provenance record");
  }
  SurfaceProvenance provenance;
  provenance.purpose = static_cast<SurfacePurpose>(record.purpose);
  provenance.quality_mode = static_cast<FitQualityMode>(record.quality_mode);
  provenance.state = static_cast<SurfaceState>(record.state);
  provenance.validation.failures = static_cast<ValidationFailure>(record.validation_failures);
  provenance.validation.validation_id = record.validation_id;
  provenance.source_generation = record.source_generation;
  provenance.served_generation = record.served_generation;
  provenance.legacy_format = false;
  return Ok(provenance);
}

} // namespace

SurfaceProvenance legacy_surface_provenance() noexcept {
  SurfaceProvenance provenance;
  provenance.purpose = SurfacePurpose::MarketMark;
  provenance.quality_mode = FitQualityMode::Balanced;
  provenance.state = SurfaceState::Degraded;
  provenance.validation.failures = ValidationFailure::InsufficientData;
  provenance.legacy_format = true;
  return provenance;
}


// ═══════════════════════════════════════════════════════════════════════════
// ATXVSA2 (v2) — zero-copy mmap columnar format. Layout spec + research lineage:
// atx-vol/docs/atxvsa2-format.md. Primary sources cited there (FlatBuffers
// natural alignment; Cap'n Proto relative byte-offset segments; Apache Arrow
// contiguous typed columns + mmap-first alignment). This is a CLEAN-BREAK sibling
// to the v1 reader/writer (§0): distinct types, no dual-read. The v1 reader/writer
// was isolated out of this product TU into src/surface_archive_v1.cpp (WS-G G2) so
// atx::vol no longer links v1; only the shared legacy_surface_provenance() and the
// internal-linkage provenance/buf helpers are defined in both TUs.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

constexpr char kArchiveV2MagicBytes[8] = {'A', 'T', 'X', 'V', 'S', 'A', '2', '0'};
constexpr char kSurfaceRecordMagicBytes[8] = {'A', 'T', 'X', 'V', 'S', 'R', '2', '0'};

// v2 schema fingerprint: folds sizeof of every v2 on-disk struct + the serialized
// POD slice structs + a v2-specific salt so a v1 file, a drifted-struct v2 file,
// and a different-build v2 file are all rejected (FlatBuffers-style hard schema
// pin; we pay no vtable indirection because the schema is fixed).
[[nodiscard]] std::uint64_t schema_hash_v2() noexcept {
  constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
  // Salt 0101 (was 0100): the SplineVol payload gained mult_cap + w_offset. That
  // layout is not captured by the sizeof-fold (SplineVol has no fixed serialized
  // POD struct), so bump the salt to reject any older v2 file. Pre-release (§0).
  constexpr std::uint64_t kV2Salt = 0xA7C3'5F04'2E1F'0101ull;
  std::uint64_t h = 0x9e3779b97f4a7c15ull ^ kV2Salt;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveV2Header)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveV2LookupSlot)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveV2DirEntry)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveV2SurfaceHeader)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(EssviParams)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(SviParams)) * kFnvPrime;
  return h;
}

[[nodiscard]] std::uint32_t header_crc_v2(ArchiveV2Header h) noexcept {
  h.header_crc32c = 0;
  std::array<std::byte, sizeof(ArchiveV2Header)> bytes{};
  std::memcpy(bytes.data(), &h, sizeof h);
  return crc32c(bytes.data(), bytes.size());
}

// CRC-32C over a whole record with its own payload_crc32c field forced to 0 (the
// exact bytes the writer checksummed). Piecewise so no temp copy is needed.
[[nodiscard]] std::uint32_t record_crc_v2(const std::byte *base, std::uint64_t size) noexcept {
  constexpr std::size_t crc_off = offsetof(ArchiveV2SurfaceHeader, payload_crc32c);
  const std::uint32_t zero = 0;
  std::uint32_t c = crc32c_update(0xFFFFFFFFu, base, crc_off);
  c = crc32c_update(c, reinterpret_cast<const std::byte *>(&zero), sizeof zero);
  c = crc32c_update(c, base + crc_off + sizeof(std::uint32_t),
                    static_cast<std::size_t>(size) - crc_off - sizeof(std::uint32_t));
  return c ^ 0xFFFFFFFFu;
}

// v2 per-slice payload byte size (mirrors v1 slice_payload_size + the ConvexDense
// diagnostics that v2 folds into the payload).
[[nodiscard]] std::uint64_t v2_payload_size(VolCurveKind kind, std::uint32_t node_count) noexcept {
  switch (kind) {
  case VolCurveKind::ConvexDense:
    return 24ull + 2ull * static_cast<std::uint64_t>(node_count) * sizeof(double);
  case VolCurveKind::Essvi:
    return sizeof(EssviParams);
  case VolCurveKind::Svi:
    return sizeof(SviParams);
  case VolCurveKind::LinearVariance:
    return 2ull * static_cast<std::uint64_t>(node_count) * sizeof(double);
  case VolCurveKind::C8:
    return sizeof(C8Params);
  case VolCurveKind::SplineVol:
    // atm/z_lo/z_hi/n/pad (32) + z[n]+mult[n] (16n) + mult_cap+w_offset (16) + viol (4).
    return 52ull + 16ull * static_cast<std::uint64_t>(node_count);
  }
  return 0;
}

struct V2SlicePlan {
  VolCurveKind kind{VolCurveKind::Essvi};
  std::uint32_t node_count{0};
  std::uint64_t payload_off{0}; // record-relative
  std::uint64_t payload_size{0};
  const IVolCurve *curve{nullptr};
  SliceContext ctx{};
};

struct V2SurfacePlan {
  std::array<char, kArchiveSymbolMax> symbol{};
  std::uint16_t symbol_len{};
  std::uint64_t symbol_hash{};
  std::uint32_t uid{};
  std::uint16_t n_slices{};
  std::uint16_t kind_bits{};
  const PricedSurface *surf{nullptr};
  std::optional<SurfaceProvenance> provenance{};
  std::uint64_t file_offset{}; // absolute
  std::uint64_t record_size{};
  std::uint64_t col_kind_off{}, col_T_off{}, col_forward_off{}, col_qeff_off{}, col_df_off{},
      col_borrow_off{}, col_nused_off{}, col_ndropped_off{}, col_nodecount_off{},
      col_payload_off_off{};
  std::size_t slot_index{};
  std::vector<V2SlicePlan> slices;
};

[[nodiscard]] bool v2_plan_less(const V2SurfacePlan &a, const V2SurfacePlan &b) noexcept {
  const std::uint16_t n = std::min(a.symbol_len, b.symbol_len);
  const int c = std::memcmp(a.symbol.data(), b.symbol.data(), n);
  if (c != 0) {
    return c < 0;
  }
  return a.symbol_len < b.symbol_len;
}

} // namespace

Result<std::vector<std::byte>>
write_surface_archive_v2(std::span<const SurfaceArchiveItem> items,
                         const ArchiveV2WriteOpts &opts) {
  if (items.empty()) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: no items");
  }
  if (items.size() > 0xFFFFFFFFull) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: too many items");
  }
  if (opts.lookup_load_pct == 0 || opts.lookup_load_pct > 100) {
    return Err(ErrorCode::InvalidArgument,
               "write_surface_archive_v2: lookup_load_pct must be in (0, 100]");
  }
  const std::uint64_t surf_align =
      opts.surface_alignment != 0 ? opts.surface_alignment : kArchiveV2SurfaceAlign;
  // surf_align flows into align_up (which assumes a power of two) and must not be
  // finer than the natural column alignment, else record offsets silently corrupt.
  if (!atx::core::is_pow2(static_cast<std::uint32_t>(surf_align)) ||
      surf_align < kArchiveV2ColumnAlign || surf_align > 0xFFFFFFFFull) {
    return Err(ErrorCode::InvalidArgument,
               "write_surface_archive_v2: surface_alignment must be a power of two >= 8");
  }
  const auto n_items = static_cast<std::uint32_t>(items.size());
  constexpr std::uint64_t A = kArchiveV2ColumnAlign;

  // 1. Plan + validate.
  std::vector<V2SurfacePlan> plans;
  plans.reserve(n_items);
  for (const SurfaceArchiveItem &it : items) {
    if (it.surface == nullptr) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: null surface");
    }
    if (it.symbol.empty()) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: empty symbol");
    }
    const PricedSurface &ps = *it.surface;
    const std::size_t n = ps.n_slices();
    if (n == 0) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: surface has no slices");
    }
    if (n > 0xFFFFu) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: too many slices");
    }

    V2SurfacePlan plan;
    plan.surf = &ps;
    plan.provenance = it.provenance;
    if (plan.provenance.has_value()) {
      if (plan.provenance->legacy_format) {
        return Err(ErrorCode::InvalidArgument,
                   "write_surface_archive_v2: explicit legacy provenance is unsupported");
      }
      if (!provenance_record_valid(to_provenance_record(*plan.provenance))) {
        return Err(ErrorCode::InvalidArgument,
                   "write_surface_archive_v2: invalid surface provenance");
      }
    }
    const std::string canon_sym = detail::canonicalize_symbol(it.symbol, kArchiveSymbolMax);
    plan.symbol_len = static_cast<std::uint16_t>(canon_sym.size());
    if (plan.symbol_len == 0) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: empty canonical symbol");
    }
    std::memcpy(plan.symbol.data(), canon_sym.data(), canon_sym.size());
    plan.symbol_hash = atx::core::hash_bytes(plan.symbol.data(), plan.symbol_len);
    plan.n_slices = static_cast<std::uint16_t>(n);
    plan.uid = ps.uid();

    // Record geometry: header -> columnar SoA scalars -> variable payloads.
    const std::uint64_t nn = n;
    plan.col_kind_off = sizeof(ArchiveV2SurfaceHeader);
    plan.col_T_off = align_up(plan.col_kind_off + nn, A);
    plan.col_forward_off = plan.col_T_off + 8ull * nn;
    plan.col_qeff_off = plan.col_forward_off + 8ull * nn;
    plan.col_df_off = plan.col_qeff_off + 8ull * nn;
    plan.col_borrow_off = plan.col_df_off + 8ull * nn;
    plan.col_nused_off = plan.col_borrow_off + 8ull * nn;
    plan.col_ndropped_off = plan.col_nused_off + 8ull * nn;
    plan.col_nodecount_off = plan.col_ndropped_off + 8ull * nn;
    plan.col_payload_off_off = align_up(plan.col_nodecount_off + 4ull * nn, A);
    std::uint64_t cursor = plan.col_payload_off_off + 8ull * nn;

    const std::span<const std::unique_ptr<IVolCurve>> curves = ps.surface().slices();
    const std::span<const SliceContext> ctx = ps.context();
    plan.slices.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      const IVolCurve *const c = curves[i].get();
      V2SlicePlan sp;
      sp.kind = c->kind();
      sp.curve = c;
      sp.ctx = ctx[i];
      if (sp.kind == VolCurveKind::ConvexDense) {
        const std::size_t nc = static_cast<const ConvexDenseCurve *>(c)->fit().u.size();
        if (nc > std::numeric_limits<std::uint32_t>::max()) {
          return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: node count exceeds u32");
        }
        sp.node_count = static_cast<std::uint32_t>(nc);
      } else if (sp.kind == VolCurveKind::LinearVariance) {
        const std::size_t nc = static_cast<const LinearVarianceCurve *>(c)->k_nodes().size();
        if (nc > std::numeric_limits<std::uint32_t>::max()) {
          return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: node count exceeds u32");
        }
        sp.node_count = static_cast<std::uint32_t>(nc);
      } else if (sp.kind == VolCurveKind::SplineVol) {
        const auto *sv = static_cast<const SplineVolCurve *>(c);
        const std::size_t nc = sv->params().z.size();
        if (nc > std::numeric_limits<std::uint32_t>::max() ||
            sv->params().n_butterfly_viol > std::numeric_limits<std::uint32_t>::max()) {
          return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: node count exceeds u32");
        }
        sp.node_count = static_cast<std::uint32_t>(nc);
      }
      sp.payload_size = v2_payload_size(sp.kind, sp.node_count);
      sp.payload_off = align_up(cursor, A);
      cursor = sp.payload_off + sp.payload_size;
      plan.kind_bits |= static_cast<std::uint16_t>(1u << static_cast<unsigned>(sp.kind));
      plan.slices.push_back(sp);
    }
    plan.record_size = cursor;
    plans.push_back(std::move(plan));
  }

  // 2. Deterministic order by canonical symbol.
  std::sort(plans.begin(), plans.end(), v2_plan_less);

  // 3. File geometry.
  const std::uint64_t load = opts.lookup_load_pct;
  const std::uint64_t want_slots =
      (static_cast<std::uint64_t>(n_items) * 100ull + load - 1ull) / load;
  std::uint32_t lookup_slots = atx::core::next_pow2(static_cast<std::uint32_t>(want_slots));
  if (lookup_slots < 8u) {
    lookup_slots = 8u;
  }
  const std::uint64_t lookup_offset = align_up(sizeof(ArchiveV2Header), 64u);
  const std::uint64_t lookup_bytes =
      static_cast<std::uint64_t>(lookup_slots) * sizeof(ArchiveV2LookupSlot);
  const std::uint64_t directory_offset = lookup_offset + lookup_bytes;
  const std::uint64_t dir_bytes = static_cast<std::uint64_t>(n_items) * sizeof(ArchiveV2DirEntry);
  const std::uint64_t data_offset = align_up(directory_offset + dir_bytes, surf_align);

  std::uint64_t cursor = data_offset;
  for (V2SurfacePlan &plan : plans) {
    plan.file_offset = align_up(cursor, surf_align);
    cursor = plan.file_offset + plan.record_size;
  }
  // Page-align the FILE tail for clean mmap (one-time, not per-surface).
  const std::uint64_t file_size = align_up(cursor, kArchiveV2FilePageAlign);

  // 4. Lookup + directory.
  std::vector<ArchiveV2LookupSlot> lookup(lookup_slots);
  std::vector<ArchiveV2DirEntry> directory(n_items);
  const std::uint64_t mask = static_cast<std::uint64_t>(lookup_slots) - 1ull;
  for (std::size_t idx = 0; idx < plans.size(); ++idx) {
    V2SurfacePlan &plan = plans[idx];
    std::uint64_t i = plan.symbol_hash & mask;
    bool placed = false;
    for (std::uint32_t step = 0; step < lookup_slots; ++step) {
      ArchiveV2LookupSlot &s = lookup[static_cast<std::size_t>(i)];
      if (s.flags == kArchiveV2SlotEmpty) {
        s.symbol_hash = plan.symbol_hash;
        s.surface_offset = plan.file_offset;
        s.surface_size = plan.record_size;
        s.uid = plan.uid;
        s.symbol_len = plan.symbol_len;
        s.flags = kArchiveV2SlotOccupied;
        std::memcpy(s.symbol, plan.symbol.data(), plan.symbol_len);
        plan.slot_index = static_cast<std::size_t>(i);
        placed = true;
        break;
      }
      if (s.symbol_hash == plan.symbol_hash && s.symbol_len == plan.symbol_len &&
          std::memcmp(s.symbol, plan.symbol.data(), plan.symbol_len) == 0) {
        return Err(ErrorCode::AlreadyExists, "write_surface_archive_v2: duplicate canonical symbol");
      }
      i = (i + 1ull) & mask;
    }
    if (!placed) {
      return Err(ErrorCode::Internal, "write_surface_archive_v2: lookup table full");
    }
    ArchiveV2DirEntry &de = directory[idx];
    de.surface_offset = plan.file_offset;
    de.surface_size = plan.record_size;
    de.symbol_hash = plan.symbol_hash;
    de.uid = plan.uid;
    de.n_slices = plan.n_slices;
    de.kind_bits = plan.kind_bits;
    de.symbol_len = plan.symbol_len;
    std::memcpy(de.symbol, plan.symbol.data(), plan.symbol_len);
  }

  // 5. Materialize.
  std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));

  for (std::size_t idx = 0; idx < plans.size(); ++idx) {
    V2SurfacePlan &plan = plans[idx];
    std::byte *base = buf_at(buffer, plan.file_offset);
    const PricingContext &pc = plan.surf->pricing();

    // Header (payload_crc32c stays 0 until the record is checksummed).
    ArchiveV2SurfaceHeader sh{};
    std::memcpy(sh.magic, kSurfaceRecordMagicBytes, 8);
    sh.record_size = plan.record_size;
    sh.S = pc.S;
    sh.r = pc.r;
    sh.now_ts_ns = pc.now_ts_ns;
    sh.al_tol = pc.al_opts.tol;
    sh.col_kind_off = plan.col_kind_off;
    sh.col_T_off = plan.col_T_off;
    sh.col_forward_off = plan.col_forward_off;
    sh.col_qeff_off = plan.col_qeff_off;
    sh.col_df_off = plan.col_df_off;
    sh.col_borrow_off = plan.col_borrow_off;
    sh.col_nused_off = plan.col_nused_off;
    sh.col_ndropped_off = plan.col_ndropped_off;
    sh.col_nodecount_off = plan.col_nodecount_off;
    sh.col_payload_off_off = plan.col_payload_off_off;
    sh.uid = plan.uid;
    sh.n_slices = plan.n_slices;
    sh.kind_bits = plan.kind_bits;
    sh.method = static_cast<std::uint8_t>(pc.method);
    sh.al_n_collocation = pc.al_opts.n_collocation;
    sh.al_n_quadrature = pc.al_opts.n_quadrature;
    sh.al_max_newton_iter = pc.al_opts.max_newton_iter;
    if (plan.provenance.has_value()) {
      const ArchiveSurfaceProvenanceRecord rec = to_provenance_record(*plan.provenance);
      sh.prov_marker = rec.marker;
      sh.prov_purpose = rec.purpose;
      sh.prov_quality_mode = rec.quality_mode;
      sh.prov_state = rec.state;
      sh.prov_validation_failures = rec.validation_failures;
      sh.prov_validation_id = rec.validation_id;
      sh.prov_source_generation = rec.source_generation;
      sh.prov_served_generation = rec.served_generation;
    } // else prov_marker == 0 -> legacy provenance on read (mirrors v1 zero-fill)
    std::memcpy(base, &sh, sizeof sh);

    // Columns (SoA).
    auto *kind_col = base + plan.col_kind_off;
    for (std::size_t i = 0; i < plan.slices.size(); ++i) {
      const V2SlicePlan &sp = plan.slices[i];
      kind_col[i] = static_cast<std::byte>(static_cast<std::uint8_t>(sp.kind));
      const double T = sp.ctx.T;
      const double fwd = sp.ctx.forward;
      const double qeff = sp.ctx.q_eff;
      const double df = sp.curve->df();
      const double borrow = sp.ctx.borrow;
      const std::uint64_t nused = sp.ctx.n_used;
      const std::uint64_t ndropped = sp.ctx.n_dropped;
      std::memcpy(base + plan.col_T_off + i * 8, &T, 8);
      std::memcpy(base + plan.col_forward_off + i * 8, &fwd, 8);
      std::memcpy(base + plan.col_qeff_off + i * 8, &qeff, 8);
      std::memcpy(base + plan.col_df_off + i * 8, &df, 8);
      std::memcpy(base + plan.col_borrow_off + i * 8, &borrow, 8);
      std::memcpy(base + plan.col_nused_off + i * 8, &nused, 8);
      std::memcpy(base + plan.col_ndropped_off + i * 8, &ndropped, 8);
      std::memcpy(base + plan.col_nodecount_off + i * 4, &sp.node_count, 4);
      std::memcpy(base + plan.col_payload_off_off + i * 8, &sp.payload_off, 8);
    }

    // Variable payloads (mirror v1 byte layouts; ConvexDense diag folded in).
    for (const V2SlicePlan &sp : plan.slices) {
      std::byte *p = base + sp.payload_off;
      switch (sp.kind) {
      case VolCurveKind::ConvexDense: {
        const ConvexSliceFit &fit = static_cast<const ConvexDenseCurve *>(sp.curve)->fit();
        const std::uint64_t n_obs = fit.n_obs;
        const std::uint64_t n_active = fit.n_active;
        std::memcpy(p + 0, &fit.rmse_price, 8);
        std::memcpy(p + 8, &n_obs, 8);
        std::memcpy(p + 16, &n_active, 8);
        const std::size_t nb = static_cast<std::size_t>(sp.node_count) * sizeof(double);
        std::memcpy(p + 24, fit.u.data(), nb);
        std::memcpy(p + 24 + nb, fit.C.data(), nb);
        break;
      }
      case VolCurveKind::Essvi: {
        const EssviParams &e = static_cast<const EssviCurve *>(sp.curve)->slice();
        std::memcpy(p, &e, sizeof e);
        break;
      }
      case VolCurveKind::Svi: {
        const SviParams &vv = static_cast<const SviCurve *>(sp.curve)->slice();
        std::memcpy(p, &vv, sizeof vv);
        break;
      }
      case VolCurveKind::LinearVariance: {
        const auto *lv = static_cast<const LinearVarianceCurve *>(sp.curve);
        const std::size_t nb = static_cast<std::size_t>(sp.node_count) * sizeof(double);
        std::memcpy(p, lv->k_nodes().data(), nb);
        std::memcpy(p + nb, lv->w_nodes().data(), nb);
        break;
      }
      case VolCurveKind::C8: {
        const C8Params &c8 = static_cast<const C8Curve *>(sp.curve)->slice();
        std::memcpy(p, &c8, sizeof c8);
        break;
      }
      case VolCurveKind::SplineVol: {
        const SplineVolParams &sv = static_cast<const SplineVolCurve *>(sp.curve)->params();
        const std::uint32_t n32 = sp.node_count;
        const std::uint32_t pad = 0;
        const std::uint32_t viol = static_cast<std::uint32_t>(sv.n_butterfly_viol);
        std::memcpy(p + 0, &sv.atm_vol, 8);
        std::memcpy(p + 8, &sv.z_lo_valid, 8);
        std::memcpy(p + 16, &sv.z_hi_valid, 8);
        std::memcpy(p + 24, &n32, 4);
        std::memcpy(p + 28, &pad, 4);
        const std::size_t nb = static_cast<std::size_t>(sp.node_count) * sizeof(double);
        std::memcpy(p + 32, sv.z.data(), nb);
        std::memcpy(p + 32 + nb, sv.mult.data(), nb);
        // mult_cap + w_offset are LIVE terms of SplineVolCurve::w() (served-multiple
        // clamp + additive calendar-cone lift); serialize both so the view is
        // bit-exact (review C1).
        std::memcpy(p + 32 + 2 * nb, &sv.mult_cap, 8);
        std::memcpy(p + 40 + 2 * nb, &sv.w_offset, 8);
        std::memcpy(p + 48 + 2 * nb, &viol, 4);
        break;
      }
      }
    }

    // Record payload CRC (lazy: written, never verified on the price path).
    const std::uint32_t crc = record_crc_v2(base, plan.record_size);
    std::memcpy(base + offsetof(ArchiveV2SurfaceHeader, payload_crc32c), &crc, sizeof crc);
    // R-19 (F6): mirror this record's CRC into its directory entry so
    // `metadata_crc32c` (which covers lookup ‖ directory, computed below) is
    // sensitive to a same-length in-place payload rewrite — the content-identity
    // the SnapshotCache/SurfaceDb staleness check relies on. directory[idx] is the
    // entry for plans[idx] (built in the same order above).
    directory[idx].payload_crc32c = crc;
  }

  if (lookup_bytes > 0) {
    std::memcpy(buf_at(buffer, lookup_offset), lookup.data(),
                static_cast<std::size_t>(lookup_bytes));
  }
  if (dir_bytes > 0) {
    std::memcpy(buf_at(buffer, directory_offset), directory.data(),
                static_cast<std::size_t>(dir_bytes));
  }
  std::uint32_t meta = crc32c_update(0xFFFFFFFFu, buf_at(buffer, lookup_offset),
                                     static_cast<std::size_t>(lookup_bytes));
  meta =
      crc32c_update(meta, buf_at(buffer, directory_offset), static_cast<std::size_t>(dir_bytes)) ^
      0xFFFFFFFFu;

  // 6. Header.
  ArchiveV2Header hdr{};
  std::memcpy(hdr.magic, kArchiveV2MagicBytes, 8);
  hdr.file_size = file_size;
  // created_ts_ns: an explicit nonzero stamp is honored verbatim (unit tests pin
  // it). The 0 sentinel is filled from a DETERMINISTIC CRC-32C of the archive
  // CONTENT — the whole payload span [header, EOF): data ‖ lookup ‖ directory
  // (every per-record payload_crc32c is mirrored into the directory, so any
  // same-length rewrite changes this hash too) — folded with file_size. Two
  // identical builds therefore produce byte-identical containers (run-to-run
  // reproducibility), while two DIFFERENT builds still get distinct stamps, which
  // preserves the (file_size, created_ts_ns, header_crc32c, metadata_crc32c)
  // content-identity the SnapshotCache keys on for staleness. (A wall-clock read
  // broke the former; a constant would break the latter.) The span excludes the
  // header, so header_crc32c/created_ts_ns are not inputs to their own hash.
  if (opts.created_ts_ns != 0) {
    hdr.created_ts_ns = static_cast<std::uint64_t>(opts.created_ts_ns);
  } else {
    const std::uint32_t content_crc =
        crc32c(buf_at(buffer, sizeof(ArchiveV2Header)),
               static_cast<std::size_t>(file_size - sizeof(ArchiveV2Header)));
    const std::uint64_t derived =
        (static_cast<std::uint64_t>(content_crc) << 32) ^ static_cast<std::uint64_t>(file_size);
    hdr.created_ts_ns = derived != 0 ? derived : 1u; // never re-hit the 0 sentinel
  }
  hdr.schema_hash = schema_hash_v2();
  hdr.writer_version_hash = 0;
  hdr.lookup_offset = lookup_offset;
  hdr.directory_offset = directory_offset;
  hdr.data_offset = data_offset;
  hdr.surface_count = n_items;
  hdr.lookup_slot_count = lookup_slots;
  hdr.lookup_slot_size = static_cast<std::uint32_t>(sizeof(ArchiveV2LookupSlot));
  hdr.dir_entry_size = static_cast<std::uint32_t>(sizeof(ArchiveV2DirEntry));
  hdr.surface_header_size = static_cast<std::uint32_t>(sizeof(ArchiveV2SurfaceHeader));
  hdr.metadata_crc32c = meta;
  hdr.flags = opts.flags;
  hdr.major = kArchiveV2Major;
  hdr.minor = kArchiveV2Minor;
  hdr.header_size = static_cast<std::uint16_t>(sizeof(ArchiveV2Header));
  hdr.endian = 1;
  hdr.pointer_bits = 64;
  hdr.header_crc32c = header_crc_v2(hdr);
  std::memcpy(buf_at(buffer, 0), &hdr, sizeof hdr);

  return Ok(std::move(buffer));
}

Status write_surface_archive_v2_file(std::string_view path,
                                     std::span<const SurfaceArchiveItem> items,
                                     const ArchiveV2WriteOpts &opts) {
  auto built = write_surface_archive_v2(items, opts);
  if (!built) {
    return tl::unexpected<atx::core::Error>(std::move(built).error());
  }
  const std::vector<std::byte> &buffer = *built;
  const std::filesystem::path dst{std::string(path)};
  std::filesystem::path tmp = dst;
  tmp += ".tmp";
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) {
      return Err(ErrorCode::IoError, "write_surface_archive_v2_file: cannot open temp file");
    }
    os.write(reinterpret_cast<const char *>(buffer.data()),
             static_cast<std::streamsize>(buffer.size()));
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "write_surface_archive_v2_file: write failed");
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    std::filesystem::remove(tmp, ec2);
    return Err(ErrorCode::IoError, "write_surface_archive_v2_file: rename failed");
  }
  return Ok();
}

// ── v2 reader ────────────────────────────────────────────────────────────────

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_impl(std::span<const std::byte> bytes,
                                                     std::shared_ptr<const void> owner) {
  if (bytes.size() < sizeof(ArchiveV2Header)) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: shorter than header");
  }
  ArchiveV2Header h;
  std::memcpy(&h, bytes.data(), sizeof h);
  if (std::memcmp(h.magic, kArchiveV2MagicBytes, 8) != 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: bad magic");
  }
  if (h.major != kArchiveV2Major) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: unsupported major version");
  }
  if (h.endian != 1) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: non-little-endian archive");
  }
  if (h.pointer_bits != 64) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: unsupported pointer width");
  }
  if (h.header_size != sizeof(ArchiveV2Header) ||
      h.lookup_slot_size != sizeof(ArchiveV2LookupSlot) ||
      h.dir_entry_size != sizeof(ArchiveV2DirEntry) ||
      h.surface_header_size != sizeof(ArchiveV2SurfaceHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: record size mismatch");
  }
  if (h.schema_hash != schema_hash_v2()) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: schema hash mismatch");
  }
  if (h.file_size != bytes.size()) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: file size mismatch");
  }
  if (h.lookup_slot_count == 0 || !atx::core::is_pow2(h.lookup_slot_count)) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: lookup slot count not a power of two");
  }
  const std::uint64_t lookup_bytes =
      static_cast<std::uint64_t>(h.lookup_slot_count) * h.lookup_slot_size;
  const std::uint64_t dir_bytes = static_cast<std::uint64_t>(h.surface_count) * h.dir_entry_size;
  if (h.lookup_offset < sizeof(ArchiveV2Header)) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: lookup overlaps header");
  }
  if (h.lookup_offset > h.file_size || lookup_bytes > h.file_size - h.lookup_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: lookup out of bounds");
  }
  if (h.lookup_offset + lookup_bytes > h.directory_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: lookup overlaps directory");
  }
  if (h.directory_offset > h.file_size || dir_bytes > h.file_size - h.directory_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: directory out of bounds");
  }
  if (h.directory_offset + dir_bytes > h.data_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: directory overlaps data");
  }
  if (h.data_offset > h.file_size) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: data offset out of bounds");
  }
  if (header_crc_v2(h) != h.header_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: header checksum mismatch");
  }
  std::uint32_t meta = crc32c_update(0xFFFFFFFFu, bytes.data() + h.lookup_offset,
                                     static_cast<std::size_t>(lookup_bytes));
  meta = crc32c_update(meta, bytes.data() + h.directory_offset,
                       static_cast<std::size_t>(dir_bytes)) ^
         0xFFFFFFFFu;
  if (meta != h.metadata_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: metadata checksum mismatch");
  }

  SurfaceArchiveV2 a;
  a.bytes_ = bytes;
  a.owner_ = std::move(owner);
  a.header_ = h;
  a.lookup_.resize(h.lookup_slot_count);
  if (lookup_bytes > 0) {
    std::memcpy(a.lookup_.data(), bytes.data() + h.lookup_offset,
                static_cast<std::size_t>(lookup_bytes));
  }
  a.directory_.resize(h.surface_count);
  if (dir_bytes > 0) {
    std::memcpy(a.directory_.data(), bytes.data() + h.directory_offset,
                static_cast<std::size_t>(dir_bytes));
  }
  for (const ArchiveV2LookupSlot &slot : a.lookup_) {
    if (slot.symbol_len > kArchiveSymbolMax) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: lookup symbol length out of bounds");
    }
  }
  for (const ArchiveV2DirEntry &de : a.directory_) {
    if (de.symbol_len > kArchiveSymbolMax) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: directory symbol length out of bounds");
    }
    if (de.surface_offset < h.data_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: directory entry precedes data");
    }
    if (de.surface_offset > h.file_size || de.surface_size > h.file_size - de.surface_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: directory entry out of bounds");
    }
    if (de.surface_size < sizeof(ArchiveV2SurfaceHeader)) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: record smaller than header");
    }
    // Record start must be >= 8-B aligned in-file so the view's typed column reads
    // are aligned relative to a >= 8-B backing base (§11.3 hardening for untrusted
    // files). Records are packed on kArchiveV2SurfaceAlign (>= 8), so any entry not
    // so aligned is corrupt.
    if ((de.surface_offset % kArchiveV2ColumnAlign) != 0u) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: record offset misaligned");
    }
  }
  return Ok(std::move(a));
}

Result<SurfaceArchiveV2> SurfaceArchiveV2::open(std::vector<std::byte> bytes) {
  auto owned = std::make_shared<std::vector<std::byte>>(std::move(bytes));
  std::span<const std::byte> span{owned->data(), owned->size()};
  return open_impl(span, std::static_pointer_cast<const void>(owned));
}

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_borrowed(std::span<const std::byte> bytes,
                                                         std::shared_ptr<const void> owner) {
  return open_impl(bytes, std::move(owner));
}

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_file(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || ec) {
    return Err(ErrorCode::NotFound, "SurfaceArchiveV2::open_file: file not found");
  }
  std::ifstream is(p, std::ios::binary | std::ios::ate);
  if (!is) {
    return Err(ErrorCode::IoError, "SurfaceArchiveV2::open_file: cannot open file");
  }
  const std::streamsize size = is.tellg();
  if (size < 0) {
    return Err(ErrorCode::IoError, "SurfaceArchiveV2::open_file: cannot size file");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  is.seekg(0);
  is.read(reinterpret_cast<char *>(bytes.data()), size);
  if (is.gcount() != size) {
    return Err(ErrorCode::IoError, "SurfaceArchiveV2::open_file: short read");
  }
  return open(std::move(bytes));
}

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_copied(std::string_view path) {
  // WS-ZC1: the backing for readers that BORROW records (PricedSurfaceView) for longer
  // than the open call. They cannot hold the mapping — on Windows a file with a live
  // mapped section cannot be replaced, which would break atomic partition republish —
  // but they also must not pay `open_file`'s stream read, which measured ~210 ms over
  // the 82-session replay (~585 MB/s through ifstream) versus ~8 ms to map.
  //
  // So: MAP, one memcpy into an owned buffer, then DROP the mapping. The copy runs at
  // memory bandwidth out of pages the OS cache already holds, so this keeps nearly all
  // of the mapped open's speed while leaving no section open against the file.
  auto mapping = atx::tsdb::Mapping::map_file_ro(std::string(path));
  if (!mapping) {
    return tl::unexpected<atx::core::Error>(std::move(mapping).error());
  }
  const auto *base = reinterpret_cast<const std::byte *>(mapping->base());
  const auto size = static_cast<std::size_t>(mapping->size());
  // reserve + insert, NOT `vector<std::byte> bytes(size)`: the sized constructor
  // VALUE-INITIALIZES (zeroes) the whole buffer and the memcpy then overwrites it, so
  // the archive is walked twice. Inserting a contiguous range is a single memcpy into
  // uninitialized storage.
  std::vector<std::byte> bytes;
  bytes.reserve(size);
  bytes.insert(bytes.end(), base, base + size);
  // `mapping` is destroyed here, before `open` — the archive owns only `bytes` now.
  return open(std::move(bytes));
}

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_mapped(std::string_view path) {
  // S2 (WS-S): map the partition read-only instead of reading the whole file
  // into an owned heap buffer. `open_impl` only CRC-validates the header, lookup,
  // and directory sections (never the data records), so opening touches just the
  // metadata pages; each surface record's bytes are faulted in lazily by the OS
  // page cache when (and only when) a reader reconstructs/maps it. A subset load
  // therefore never pays for the whole file. The `Mapping` is kept alive for the
  // archive's whole lifetime via the type-erased `owner` handed to `open_borrowed`
  // — every returned `PricedSurfaceView` (and the archive's own `bytes_` span)
  // borrows into these mapped pages, so the mapping MUST outlive them.
  auto mapping = atx::tsdb::Mapping::map_file_ro(std::string(path));
  if (!mapping) {
    return tl::unexpected<atx::core::Error>(std::move(mapping).error());
  }
  auto owner = std::make_shared<atx::tsdb::Mapping>(std::move(*mapping));
  const std::span<const std::byte> span{reinterpret_cast<const std::byte *>(owner->base()),
                                        static_cast<std::size_t>(owner->size())};
  return open_borrowed(span, std::static_pointer_cast<const void>(owner));
}

const ArchiveV2LookupSlot *SurfaceArchiveV2::find_slot(std::string_view symbol) const noexcept {
  if (lookup_.empty()) {
    return nullptr;
  }
  const std::string canon_sym = detail::canonicalize_symbol(symbol, kArchiveSymbolMax);
  const auto len = static_cast<std::uint16_t>(canon_sym.size());
  if (len == 0) {
    return nullptr;
  }
  const std::uint64_t hh = atx::core::hash_bytes(canon_sym.data(), len);
  const std::uint64_t mask = static_cast<std::uint64_t>(lookup_.size()) - 1ull;
  std::uint64_t i = hh & mask;
  for (std::size_t step = 0; step < lookup_.size(); ++step) {
    const ArchiveV2LookupSlot &s = lookup_[static_cast<std::size_t>(i)];
    if (s.flags == kArchiveV2SlotEmpty) {
      return nullptr;
    }
    if (s.symbol_hash == hh && s.symbol_len == len &&
        std::memcmp(s.symbol, canon_sym.data(), len) == 0) {
      return &s;
    }
    i = (i + 1ull) & mask;
  }
  return nullptr;
}

Result<ArchiveV2DirEntry> SurfaceArchiveV2::find(std::string_view symbol) const {
  const ArchiveV2LookupSlot *s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchiveV2::find: symbol not present");
  }
  ArchiveV2DirEntry de;
  de.surface_offset = s->surface_offset;
  de.surface_size = s->surface_size;
  de.symbol_hash = s->symbol_hash;
  de.uid = s->uid;
  de.symbol_len = s->symbol_len;
  std::memcpy(de.symbol, s->symbol, s->symbol_len);
  return de;
}

namespace {
// Decode provenance from a v2 surface record header's fields (reuses the shared
// v1 record validation / legacy handling for identical semantics).
[[nodiscard]] Result<SurfaceProvenance> provenance_from_v2_header(const ArchiveV2SurfaceHeader &h) {
  ArchiveSurfaceProvenanceRecord rec{};
  rec.marker = h.prov_marker;
  rec.purpose = h.prov_purpose;
  rec.quality_mode = h.prov_quality_mode;
  rec.state = h.prov_state;
  rec.validation_failures = h.prov_validation_failures;
  rec.validation_id = h.prov_validation_id;
  rec.source_generation = h.prov_source_generation;
  rec.served_generation = h.prov_served_generation;
  return from_provenance_record(rec);
}
} // namespace

Result<SurfaceProvenance> SurfaceArchiveV2::provenance(std::string_view symbol) const {
  const ArchiveV2LookupSlot *slot = find_slot(symbol);
  if (slot == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchiveV2::provenance: symbol not present");
  }
  if (slot->surface_size < sizeof(ArchiveV2SurfaceHeader) ||
      slot->surface_offset > bytes_.size() ||
      slot->surface_size > bytes_.size() - slot->surface_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::provenance: record out of bounds");
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, bytes_.data() + slot->surface_offset, sizeof h);
  if (std::memcmp(h.magic, kSurfaceRecordMagicBytes, 8) != 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::provenance: bad record magic");
  }
  return provenance_from_v2_header(h);
}

Result<PricedSurfaceView> SurfaceArchiveV2::map_symbol(std::string_view symbol) const {
  const ArchiveV2LookupSlot *s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchiveV2::map_symbol: symbol not present");
  }
  if (s->surface_offset > bytes_.size() || s->surface_size > bytes_.size() - s->surface_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::map_symbol: record out of bounds");
  }
  // SUBSET MAP: the view borrows ONLY this record's extent; no sibling bytes read.
  return PricedSurfaceView::create_over_record(
      bytes_.subspan(static_cast<std::size_t>(s->surface_offset),
                     static_cast<std::size_t>(s->surface_size)));
}

Result<std::vector<PricedSurfaceView>> SurfaceArchiveV2::map_all() const {
  std::vector<PricedSurfaceView> out;
  out.reserve(directory_.size());
  for (const ArchiveV2DirEntry &de : directory_) {
    if (de.surface_offset > bytes_.size() || de.surface_size > bytes_.size() - de.surface_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::map_all: record out of bounds");
    }
    auto v = PricedSurfaceView::create_over_record(
        bytes_.subspan(static_cast<std::size_t>(de.surface_offset),
                       static_cast<std::size_t>(de.surface_size)));
    if (!v) {
      return tl::unexpected<atx::core::Error>(std::move(v).error());
    }
    out.push_back(std::move(*v));
  }
  return Ok(std::move(out));
}

Result<ArchivedSurfaceView> SurfaceArchiveV2::map_entry(const ArchiveV2DirEntry &e) const {
  // Exactly `reconstruct_entry`'s bounds contract, but building a BORROWED view over
  // the record extent instead of an owned PricedSurface (WS-ZC1).
  if (e.surface_size < sizeof(ArchiveV2SurfaceHeader) || e.surface_offset > bytes_.size() ||
      e.surface_size > bytes_.size() - e.surface_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::map_entry: record out of bounds");
  }
  const std::span<const std::byte> rec = bytes_.subspan(
      static_cast<std::size_t>(e.surface_offset), static_cast<std::size_t>(e.surface_size));
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, rec.data(), sizeof h);
  auto prov = provenance_from_v2_header(h);
  if (!prov) {
    return tl::unexpected<atx::core::Error>(std::move(prov).error());
  }
  auto v = PricedSurfaceView::create_over_record(rec);
  if (!v) {
    return tl::unexpected<atx::core::Error>(std::move(v).error());
  }
  return Ok(ArchivedSurfaceView{std::move(*v), std::move(*prov)});
}

Result<std::vector<ArchivedSurfaceView>> SurfaceArchiveV2::map_all_with_provenance() const {
  std::vector<ArchivedSurfaceView> out;
  out.reserve(directory_.size());
  for (const ArchiveV2DirEntry &de : directory_) {
    if (de.surface_offset > bytes_.size() || de.surface_size > bytes_.size() - de.surface_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::map_all_with_provenance: record OOB");
    }
    const std::byte *rec = bytes_.data() + de.surface_offset;
    ArchiveV2SurfaceHeader h;
    std::memcpy(&h, rec, sizeof h);
    auto prov = provenance_from_v2_header(h);
    if (!prov) {
      return tl::unexpected<atx::core::Error>(std::move(prov).error());
    }
    auto v = PricedSurfaceView::create_over_record(
        bytes_.subspan(static_cast<std::size_t>(de.surface_offset),
                       static_cast<std::size_t>(de.surface_size)));
    if (!v) {
      return tl::unexpected<atx::core::Error>(std::move(v).error());
    }
    out.push_back(ArchivedSurfaceView{std::move(*v), std::move(*prov)});
  }
  return Ok(std::move(out));
}

namespace {

// Bounds + natural-alignment check for a v2 column: `count` elems of `elem` bytes
// at record-relative `off` in a record of `rs` bytes (mirrors the view's guard).
[[nodiscard]] bool v2_column_ok(std::uint64_t off, std::uint64_t elem, std::uint64_t count,
                                std::uint64_t rs, std::uint64_t align) noexcept {
  if ((off % align) != 0u || off > rs) {
    return false;
  }
  return elem * count <= rs - off; // count bounded by n_slices (u32) -> no overflow
}

// Rebuild an OWNED PricedSurface from one v2 record's byte extent — the inverse of
// write_surface_archive_v2 and the deleted v1 `reconstruct`. Every field is read
// via memcpy (never reinterpret_cast) so it is alignment-safe regardless of the
// record base. The curve constructors + SliceContext fields + PricingContext are
// IDENTICAL to what the old v1 reconstruct built from the same source surface, so
// the result is bit-for-bit the source PricedSurface (the S2 view gate proved the
// view matches the source over the same serialized columns; this materializes the
// same data into an owned surface for the callers that still need one).
[[nodiscard]] Result<PricedSurface> reconstruct_v2_record(std::span<const std::byte> record) {
  if (record.size() < sizeof(ArchiveV2SurfaceHeader)) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: record smaller than header");
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, record.data(), sizeof h);
  if (std::memcmp(h.magic, kSurfaceRecordMagicBytes, 8) != 0) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: bad record magic");
  }
  if (h.record_size != record.size()) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: record size mismatch");
  }
  if (h.n_slices == 0) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: zero slices");
  }
  const std::uint64_t n = h.n_slices;
  const std::uint64_t rs = record.size();
  if (!(v2_column_ok(h.col_kind_off, 1, n, rs, 1) && v2_column_ok(h.col_T_off, 8, n, rs, 8) &&
        v2_column_ok(h.col_forward_off, 8, n, rs, 8) && v2_column_ok(h.col_qeff_off, 8, n, rs, 8) &&
        v2_column_ok(h.col_df_off, 8, n, rs, 8) && v2_column_ok(h.col_borrow_off, 8, n, rs, 8) &&
        v2_column_ok(h.col_nused_off, 8, n, rs, 8) &&
        v2_column_ok(h.col_ndropped_off, 8, n, rs, 8) &&
        v2_column_ok(h.col_nodecount_off, 4, n, rs, 4) &&
        v2_column_ok(h.col_payload_off_off, 8, n, rs, 8))) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: column out of bounds / misaligned");
  }

  const std::byte *base = record.data();
  const auto rd_f64 = [&](std::uint64_t col_off, std::uint64_t i) noexcept {
    double v;
    std::memcpy(&v, base + col_off + i * 8, 8);
    return v;
  };
  const auto rd_u64 = [&](std::uint64_t col_off, std::uint64_t i) noexcept {
    std::uint64_t v;
    std::memcpy(&v, base + col_off + i * 8, 8);
    return v;
  };

  PricingContext pc;
  pc.S = h.S;
  pc.r = h.r;
  pc.now_ts_ns = h.now_ts_ns;
  pc.uid = h.uid;
  pc.method = static_cast<AmericanMethod>(h.method);
  pc.al_opts.n_collocation = h.al_n_collocation;
  pc.al_opts.n_quadrature = h.al_n_quadrature;
  pc.al_opts.max_newton_iter = h.al_max_newton_iter;
  pc.al_opts.tol = h.al_tol;

  CurveSurface surface;
  std::vector<SliceContext> ctx;
  ctx.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint8_t kind_byte = 0;
    std::memcpy(&kind_byte, base + h.col_kind_off + i, 1);
    const auto kind = static_cast<VolCurveKind>(kind_byte);
    const double T = rd_f64(h.col_T_off, i);
    const double fwd = rd_f64(h.col_forward_off, i);
    const double qeff = rd_f64(h.col_qeff_off, i);
    const double df = rd_f64(h.col_df_off, i);
    const double borrow = rd_f64(h.col_borrow_off, i);
    const std::uint64_t nused = rd_u64(h.col_nused_off, i);
    const std::uint64_t ndropped = rd_u64(h.col_ndropped_off, i);
    std::uint32_t nc = 0;
    std::memcpy(&nc, base + h.col_nodecount_off + i * 4, 4);
    const std::uint64_t poff = rd_u64(h.col_payload_off_off, i);
    if ((poff % kArchiveV2ColumnAlign) != 0u || poff > rs) {
      return Err(ErrorCode::ParseError, "reconstruct_v2: slice payload misaligned/out of bounds");
    }
    const std::uint64_t avail = rs - poff;
    const std::byte *p = base + poff;
    const std::size_t nb = static_cast<std::size_t>(nc) * sizeof(double);

    std::unique_ptr<IVolCurve> curve;
    switch (kind) {
    case VolCurveKind::Essvi: {
      if (sizeof(EssviParams) > avail) {
        return Err(ErrorCode::ParseError, "reconstruct_v2: essvi payload out of bounds");
      }
      EssviParams e{};
      std::memcpy(&e, p, sizeof e);
      curve = std::make_unique<EssviCurve>(e, df);
      break;
    }
    case VolCurveKind::Svi: {
      if (sizeof(SviParams) > avail) {
        return Err(ErrorCode::ParseError, "reconstruct_v2: svi payload out of bounds");
      }
      SviParams sv{};
      std::memcpy(&sv, p, sizeof sv);
      curve = std::make_unique<SviCurve>(sv, df);
      break;
    }
    case VolCurveKind::C8: {
      if (sizeof(C8Params) > avail) {
        return Err(ErrorCode::ParseError, "reconstruct_v2: c8 payload out of bounds");
      }
      C8Params c8{};
      std::memcpy(&c8, p, sizeof c8);
      curve = std::make_unique<C8Curve>(c8, df);
      break;
    }
    case VolCurveKind::LinearVariance: {
      const std::uint64_t need = 2ull * static_cast<std::uint64_t>(nc) * sizeof(double);
      if (nc == 0 || need > avail) {
        return Err(ErrorCode::ParseError, "reconstruct_v2: linear payload out of bounds");
      }
      std::vector<double> k(nc);
      std::vector<double> w(nc);
      std::memcpy(k.data(), p, nb);
      std::memcpy(w.data(), p + nb, nb);
      curve = std::make_unique<LinearVarianceCurve>(T, fwd, df, std::move(k), std::move(w));
      break;
    }
    case VolCurveKind::ConvexDense: {
      const std::uint64_t need = 24ull + 2ull * static_cast<std::uint64_t>(nc) * sizeof(double);
      if (nc == 0 || need > avail) {
        return Err(ErrorCode::ParseError, "reconstruct_v2: convex payload out of bounds");
      }
      ConvexSliceFit fit;
      fit.T = T;
      fit.F = fwd;
      fit.df = df;
      std::memcpy(&fit.rmse_price, p + 0, 8);
      std::uint64_t n_obs = 0;
      std::uint64_t n_active = 0;
      std::memcpy(&n_obs, p + 8, 8);
      std::memcpy(&n_active, p + 16, 8);
      fit.n_obs = static_cast<std::size_t>(n_obs);
      fit.n_active = static_cast<std::size_t>(n_active);
      fit.u.resize(nc);
      fit.C.resize(nc);
      std::memcpy(fit.u.data(), p + 24, nb);
      std::memcpy(fit.C.data(), p + 24 + nb, nb);
      curve = std::make_unique<ConvexDenseCurve>(std::move(fit));
      break;
    }
    case VolCurveKind::SplineVol: {
      // atm_vol,z_lo,z_hi f64x3 | n u32 | pad u32 | z[n] | mult[n] | mult_cap f64 |
      // w_offset f64 | viol u32 (mult_cap + w_offset are LIVE in w(); review C1).
      const std::uint64_t need = 52ull + 16ull * static_cast<std::uint64_t>(nc);
      if (need > avail) {
        return Err(ErrorCode::ParseError, "reconstruct_v2: spline payload out of bounds");
      }
      SplineVolParams sp;
      std::memcpy(&sp.atm_vol, p + 0, 8);
      std::memcpy(&sp.z_lo_valid, p + 8, 8);
      std::memcpy(&sp.z_hi_valid, p + 16, 8);
      std::uint32_t n32 = 0;
      std::memcpy(&n32, p + 24, 4);
      if (n32 != nc) {
        return Err(ErrorCode::ParseError, "reconstruct_v2: spline node count mismatch");
      }
      sp.z.resize(nc);
      sp.mult.resize(nc);
      std::memcpy(sp.z.data(), p + 32, nb);
      std::memcpy(sp.mult.data(), p + 32 + nb, nb);
      std::memcpy(&sp.mult_cap, p + 32 + 2 * nb, 8);
      std::memcpy(&sp.w_offset, p + 40 + 2 * nb, 8);
      std::uint32_t viol = 0;
      std::memcpy(&viol, p + 48 + 2 * nb, 4);
      sp.n_butterfly_viol = viol;
      curve = std::make_unique<SplineVolCurve>(std::move(sp), T, fwd, df);
      break;
    }
    default:
      return Err(ErrorCode::ParseError, "reconstruct_v2: unknown curve kind");
    }
    surface.push(std::move(curve));

    SliceContext sc;
    sc.T = T;
    sc.forward = fwd;
    sc.borrow = borrow;
    sc.q_eff = qeff;
    sc.n_used = static_cast<std::size_t>(nused);
    sc.n_dropped = static_cast<std::size_t>(ndropped);
    ctx.push_back(sc);
  }

  auto ps = PricedSurface::create(std::move(surface), std::move(ctx), pc);
  if (!ps) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: reconstructed surface failed validation");
  }
  return Ok(std::move(*ps));
}

} // namespace

Result<PricedSurface> SurfaceArchiveV2::reconstruct_symbol(std::string_view symbol) const {
  const ArchiveV2LookupSlot *s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchiveV2::reconstruct_symbol: symbol not present");
  }
  if (s->surface_offset > bytes_.size() || s->surface_size > bytes_.size() - s->surface_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::reconstruct_symbol: record out of bounds");
  }
  return reconstruct_v2_record(bytes_.subspan(static_cast<std::size_t>(s->surface_offset),
                                              static_cast<std::size_t>(s->surface_size)));
}

Result<ArchivedSurface> SurfaceArchiveV2::reconstruct_entry(const ArchiveV2DirEntry &e) const {
  // S3 (WS-S): reconstruct + read provenance directly from a directory entry the
  // caller already holds (its record extent is in `e`), in ONE pass. The subset
  // load path previously called reconstruct_symbol(sym) THEN provenance(sym),
  // each re-running find_slot (hash probe + canonicalize_symbol string alloc)
  // despite `e` already carrying surface_offset/surface_size — two redundant
  // probes per referenced surface. Here there is no probe: both the surface and
  // its provenance come off the same record header/payload extent.
  if (e.surface_size < sizeof(ArchiveV2SurfaceHeader) || e.surface_offset > bytes_.size() ||
      e.surface_size > bytes_.size() - e.surface_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::reconstruct_entry: record out of bounds");
  }
  const std::span<const std::byte> rec = bytes_.subspan(
      static_cast<std::size_t>(e.surface_offset), static_cast<std::size_t>(e.surface_size));
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, rec.data(), sizeof h);
  auto prov = provenance_from_v2_header(h);
  if (!prov) {
    return tl::unexpected<atx::core::Error>(std::move(prov).error());
  }
  auto ps = reconstruct_v2_record(rec);
  if (!ps) {
    return tl::unexpected<atx::core::Error>(std::move(ps).error());
  }
  return Ok(ArchivedSurface{std::move(*ps), std::move(*prov)});
}

Result<std::vector<PricedSurface>> SurfaceArchiveV2::reconstruct_all() const {
  std::vector<PricedSurface> out;
  out.reserve(directory_.size());
  for (const ArchiveV2DirEntry &de : directory_) {
    if (de.surface_offset > bytes_.size() || de.surface_size > bytes_.size() - de.surface_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::reconstruct_all: record out of bounds");
    }
    auto ps = reconstruct_v2_record(
        bytes_.subspan(static_cast<std::size_t>(de.surface_offset),
                       static_cast<std::size_t>(de.surface_size)));
    if (!ps) {
      return tl::unexpected<atx::core::Error>(std::move(ps).error());
    }
    out.push_back(std::move(*ps));
  }
  return Ok(std::move(out));
}

Result<std::vector<ArchivedSurface>> SurfaceArchiveV2::reconstruct_all_with_provenance() const {
  std::vector<ArchivedSurface> out;
  out.reserve(directory_.size());
  for (const ArchiveV2DirEntry &de : directory_) {
    if (de.surface_offset > bytes_.size() || de.surface_size > bytes_.size() - de.surface_offset) {
      return Err(ErrorCode::ParseError,
                 "SurfaceArchiveV2::reconstruct_all_with_provenance: record OOB");
    }
    const std::span<const std::byte> rec =
        bytes_.subspan(static_cast<std::size_t>(de.surface_offset),
                       static_cast<std::size_t>(de.surface_size));
    ArchiveV2SurfaceHeader h;
    std::memcpy(&h, rec.data(), sizeof h);
    auto prov = provenance_from_v2_header(h);
    if (!prov) {
      return tl::unexpected<atx::core::Error>(std::move(prov).error());
    }
    auto ps = reconstruct_v2_record(rec);
    if (!ps) {
      return tl::unexpected<atx::core::Error>(std::move(ps).error());
    }
    out.push_back(ArchivedSurface{std::move(*ps), std::move(*prov)});
  }
  return Ok(std::move(out));
}

Status SurfaceArchiveV2::validate_record(std::uint64_t offset, std::uint64_t size) const {
  if (size < sizeof(ArchiveV2SurfaceHeader) || offset > bytes_.size() ||
      size > bytes_.size() - offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::validate: record out of bounds");
  }
  const std::byte *base = bytes_.data() + offset;
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, base, sizeof h);
  if (std::memcmp(h.magic, kSurfaceRecordMagicBytes, 8) != 0 || h.record_size != size) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::validate: bad record framing");
  }
  if (record_crc_v2(base, size) != h.payload_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::validate: record checksum mismatch");
  }
  return Ok();
}

Status SurfaceArchiveV2::validate_symbol(std::string_view symbol) const {
  const ArchiveV2LookupSlot *s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchiveV2::validate_symbol: symbol not present");
  }
  return validate_record(s->surface_offset, s->surface_size);
}

Status SurfaceArchiveV2::validate_all() const {
  for (const ArchiveV2DirEntry &de : directory_) {
    const Status st = validate_record(de.surface_offset, de.surface_size);
    if (!st) {
      return st;
    }
  }
  return Ok();
}

} // namespace atx::vol
