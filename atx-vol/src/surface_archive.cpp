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

constexpr char kArchiveMagic[8] = {'A', 'T', 'X', 'V', 'S', 'A', '0', '3'};
constexpr char kBlobMagic[8] = {'A', 'T', 'X', 'V', 'S', 'B', '0', '3'};

[[nodiscard]] std::byte *buf_at(std::vector<std::byte> &b, std::uint64_t off) noexcept {
  return b.data() + static_cast<std::size_t>(off);
}
[[nodiscard]] const std::byte *buf_at(const std::vector<std::byte> &b, std::uint64_t off) noexcept {
  return b.data() + static_cast<std::size_t>(off);
}

// Compile-time fingerprint of the on-disk layout. Folds the sizeof of every
// serialized record + a v3 format salt so a reader built against a different
// struct shape (or the v2 format) rejects the file instead of mis-reading bytes.
[[nodiscard]] std::uint64_t schema_hash() noexcept {
  constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
  constexpr std::uint64_t kV3Salt = 0xA7C3'5F03'1D9E'0003ull;
  std::uint64_t h = 0x9e3779b97f4a7c15ull ^ kV3Salt;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveHeader)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveIndexSlot)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveDirEntry)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(SurfaceBlobHeader)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchivePricingRecord)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveSliceHeader)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(EssviParams)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(SviParams)) * kFnvPrime;
  return h;
}

// CRC-32C over a header with its own checksum field zeroed.
[[nodiscard]] std::uint32_t header_crc(ArchiveHeader h) noexcept {
  h.header_crc32c = 0;
  std::array<std::byte, sizeof(ArchiveHeader)> bytes{};
  std::memcpy(bytes.data(), &h, sizeof h);
  return crc32c(bytes.data(), bytes.size());
}

[[nodiscard]] std::int64_t wall_clock_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Payload bytes for one slice of `kind` with `node_count` convex nodes.
[[nodiscard]] std::uint32_t slice_payload_size(VolCurveKind kind,
                                               std::uint32_t node_count) noexcept {
  switch (kind) {
  case VolCurveKind::ConvexDense:
    return static_cast<std::uint32_t>(2ull * node_count * sizeof(double));
  case VolCurveKind::Essvi:
    return static_cast<std::uint32_t>(sizeof(EssviParams));
  case VolCurveKind::Svi:
    return static_cast<std::uint32_t>(sizeof(SviParams));
  case VolCurveKind::LinearVariance:
    return static_cast<std::uint32_t>(2ull * node_count * sizeof(double));
  case VolCurveKind::C8:
    return static_cast<std::uint32_t>(sizeof(C8Params));
  case VolCurveKind::SplineVol:
    // Additive ATXVSA payload (Task I5): atm_vol / z_lo_valid / z_hi_valid
    // (f64 x3, 24 bytes) + active-knot count `n` (u32, 4 bytes -- mirrors AND
    // cross-checks the header's `node_count`) + z[n] / mult[n] (f64 arrays,
    // 16 bytes/knot) + n_butterfly_viol (u32, 4 bytes) = 32 fixed bytes +
    // 16*n. No fixed POD struct is memcpy'd here (SplineVolParams owns
    // std::vector members, the same shape as LinearVarianceCurve's k_/w_), so
    // this join does NOT touch schema_hash -- it mirrors the C8Params
    // precedent noted above: a new *kind byte value*, not a changed layout of
    // an existing serialized kind. An old reader built before this task hits
    // the `default:` "unknown curve kind" ParseError on kind byte 5 instead
    // of misreading it; an old archive (which cannot contain kind 5, by
    // construction of the prior reject) parses identically under this build.
    return static_cast<std::uint32_t>(32ull + 16ull * node_count);
  }
  return 0;
}

// One planned slice within a blob (source curve + its carry context).
struct SlicePlan {
  VolCurveKind kind{VolCurveKind::Essvi};
  std::uint32_t node_count{0};
  std::uint32_t payload_size{0};
  std::uint64_t rec_size{0}; // header + payload, padded to array_align
  const IVolCurve *curve{nullptr};
  SliceContext ctx{};
};

// One planned surface blob.
struct BlobPlan {
  std::array<char, kArchiveSymbolMax> symbol{};
  std::uint16_t symbol_len{};
  std::uint64_t symbol_hash{};
  std::uint16_t n_slices{};
  std::uint16_t kind_bits{};
  std::uint32_t uid{};
  const PricedSurface *surf{nullptr};
  std::optional<SurfaceProvenance> provenance{};
  std::uint64_t symbol_offset{};
  std::uint64_t symbol_size{};
  std::uint64_t pricing_offset{};
  std::uint64_t pricing_size{};
  std::uint64_t slices_offset{};
  std::uint64_t slices_size{};
  std::uint64_t blob_size{};
  std::uint64_t file_offset{};
  std::uint32_t crc32c{};
  std::size_t slot_index{};
  std::vector<SlicePlan> slices;
};

// Canonical-symbol comparator (memcmp of the shorter prefix, then length) —
// deterministic layout independent of caller order.
[[nodiscard]] bool plan_less(const BlobPlan &a, const BlobPlan &b) noexcept {
  const std::uint16_t n = std::min(a.symbol_len, b.symbol_len);
  const int c = std::memcmp(a.symbol.data(), b.symbol.data(), n);
  if (c != 0) {
    return c < 0;
  }
  return a.symbol_len < b.symbol_len;
}

// Fill an ArchivePricingRecord from a PricedSurface's PricingContext.
[[nodiscard]] ArchivePricingRecord to_pricing_record(const PricingContext &pc) noexcept {
  ArchivePricingRecord pr{};
  pr.S = pc.S;
  pr.r = pc.r;
  pr.now_ts_ns = pc.now_ts_ns;
  pr.uid = pc.uid;
  pr.method = static_cast<std::uint8_t>(pc.method);
  pr.al_n_collocation = pc.al_opts.n_collocation;
  pr.al_n_quadrature = pc.al_opts.n_quadrature;
  pr.al_max_newton_iter = pc.al_opts.max_newton_iter;
  pr.al_tol = pc.al_opts.tol;
  return pr;
}

[[nodiscard]] PricingContext from_pricing_record(const ArchivePricingRecord &pr) noexcept {
  PricingContext pc;
  pc.S = pr.S;
  pc.r = pr.r;
  pc.now_ts_ns = pr.now_ts_ns;
  pc.uid = pr.uid;
  pc.method = static_cast<AmericanMethod>(pr.method);
  pc.al_opts.n_collocation = pr.al_n_collocation;
  pc.al_opts.n_quadrature = pr.al_n_quadrature;
  pc.al_opts.max_newton_iter = pr.al_max_newton_iter;
  pc.al_opts.tol = pr.al_tol;
  return pc;
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

[[nodiscard]] const ArchiveIndexSlot *
find_directory_slot(std::span<const ArchiveIndexSlot> lookup,
                    const ArchiveDirEntry &directory) noexcept {
  if (lookup.empty() || directory.symbol_len > kArchiveSymbolMax) {
    return nullptr;
  }
  const std::uint64_t mask = static_cast<std::uint64_t>(lookup.size()) - 1ull;
  std::uint64_t index = directory.symbol_hash & mask;
  for (std::size_t step = 0; step < lookup.size(); ++step) {
    const ArchiveIndexSlot &slot = lookup[static_cast<std::size_t>(index)];
    if (slot.flags == kArchiveSlotEmpty) {
      return nullptr;
    }
    if (slot.symbol_len <= kArchiveSymbolMax && slot.symbol_hash == directory.symbol_hash &&
        slot.symbol_len == directory.symbol_len &&
        std::memcmp(slot.symbol, directory.symbol, directory.symbol_len) == 0) {
      return &slot;
    }
    index = (index + 1ull) & mask;
  }
  return nullptr;
}

[[nodiscard]] bool directory_identity_matches(const ArchiveDirEntry &directory,
                                              const ArchiveIndexSlot &slot) noexcept {
  return directory.surface_offset == slot.surface_offset &&
         directory.surface_size == slot.surface_size && directory.uid == slot.uid;
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

// ── Writer ───────────────────────────────────────────────────────────────

Result<std::vector<std::byte>> write_surface_archive(std::span<const SurfaceArchiveItem> items,
                                                     const SurfaceArchiveWriteOpts &opts) {
  if (items.empty()) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive: no items");
  }
  if (items.size() > 0xFFFFFFFFull) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive: too many items");
  }
  if (opts.lookup_load_pct == 0 || opts.lookup_load_pct > 100) {
    return Err(ErrorCode::InvalidArgument,
               "write_surface_archive: lookup_load_pct must be in (0, 100]");
  }
  const std::uint64_t array_align =
      opts.array_alignment != 0 ? opts.array_alignment : kArchiveArrayAlign;
  const std::uint64_t blob_align =
      opts.blob_alignment != 0 ? opts.blob_alignment : kArchiveBlobAlign;

  const auto n_items = static_cast<std::uint32_t>(items.size());

  // 1. Plan + validate every item.
  std::vector<BlobPlan> plans;
  plans.reserve(n_items);
  for (const SurfaceArchiveItem &it : items) {
    if (it.surface == nullptr) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: null surface");
    }
    if (it.symbol.empty()) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: empty symbol");
    }
    const PricedSurface &ps = *it.surface;
    const std::size_t n = ps.n_slices();
    if (n == 0) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: surface has no slices");
    }
    if (n > 0xFFFFu) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: too many slices");
    }

    BlobPlan plan;
    plan.surf = &ps;
    plan.provenance = it.provenance;
    if (plan.provenance.has_value()) {
      if (plan.provenance->legacy_format) {
        return Err(ErrorCode::InvalidArgument,
                   "write_surface_archive: explicit legacy provenance is unsupported");
      }
      const ArchiveSurfaceProvenanceRecord record = to_provenance_record(*plan.provenance);
      if (!provenance_record_valid(record)) {
        return Err(ErrorCode::InvalidArgument, "write_surface_archive: invalid surface provenance");
      }
    }
    // plan.symbol{} is zero-initialized (BlobPlan's default member init), so
    // bytes past canon_sym.size() stay zero -- matching the old canonicalize()
    // zero-pad tail without needing it explicitly here.
    const std::string canon_sym = detail::canonicalize_symbol(it.symbol, kArchiveSymbolMax);
    plan.symbol_len = static_cast<std::uint16_t>(canon_sym.size());
    std::memcpy(plan.symbol.data(), canon_sym.data(), canon_sym.size());
    if (plan.symbol_len == 0) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: empty canonical symbol");
    }
    plan.symbol_hash = atx::core::hash_bytes(plan.symbol.data(), plan.symbol_len);
    plan.n_slices = static_cast<std::uint16_t>(n);
    plan.uid = ps.uid();

    // Blob geometry: header -> symbol -> pricing -> sequential slice records.
    std::uint64_t cur = sizeof(SurfaceBlobHeader);
    plan.symbol_offset = cur;
    plan.symbol_size = plan.symbol_len;
    cur = align_up(cur + plan.symbol_size, array_align);
    plan.pricing_offset = cur;
    plan.pricing_size = sizeof(ArchivePricingRecord);
    cur = align_up(cur + plan.pricing_size, array_align);
    plan.slices_offset = cur;

    const std::span<const std::unique_ptr<IVolCurve>> curves = ps.surface().slices();
    const std::span<const SliceContext> ctx = ps.context();
    plan.slices.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      const IVolCurve *const c = curves[i].get();
      SlicePlan sp;
      sp.kind = c->kind();
      sp.curve = c;
      sp.ctx = ctx[i];
      if (sp.kind == VolCurveKind::ConvexDense) {
        const auto *cd = static_cast<const ConvexDenseCurve *>(c);
        // `node_count` is a uint32 field on disk. Guard the narrowing so an
        // (implausibly large) slice cannot silently wrap the count instead of
        // being reported — matches the write-path validation above (>0xFFFF
        // slices, empty surface). Defensive: a >UINT32_MAX-node curve is not
        // reachable in practice.
        const std::size_t node_count = cd->fit().u.size();
        if (node_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
          return Err(ErrorCode::InvalidArgument,
                     "write_surface_archive: slice node count exceeds uint32");
        }
        sp.node_count = static_cast<std::uint32_t>(node_count);
      } else if (sp.kind == VolCurveKind::LinearVariance) {
        const auto *lv = static_cast<const LinearVarianceCurve *>(c);
        const std::size_t node_count = lv->k_nodes().size();
        if (node_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
          return Err(ErrorCode::InvalidArgument,
                     "write_surface_archive: slice node count exceeds uint32");
        }
        sp.node_count = static_cast<std::uint32_t>(node_count);
      } else if (sp.kind == VolCurveKind::SplineVol) {
        const auto *sv = static_cast<const SplineVolCurve *>(c);
        const std::size_t node_count = sv->params().z.size();
        if (node_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
          return Err(ErrorCode::InvalidArgument,
                     "write_surface_archive: slice node count exceeds uint32");
        }
        if (sv->params().n_butterfly_viol >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
          return Err(ErrorCode::InvalidArgument,
                     "write_surface_archive: butterfly violation count exceeds uint32");
        }
        sp.node_count = static_cast<std::uint32_t>(node_count);
      }
      sp.payload_size = slice_payload_size(sp.kind, sp.node_count);
      sp.rec_size = align_up(sizeof(ArchiveSliceHeader) + sp.payload_size, array_align);
      plan.kind_bits |= static_cast<std::uint16_t>(1u << static_cast<unsigned>(sp.kind));
      cur += sp.rec_size;
      plan.slices.push_back(sp);
    }
    plan.slices_size = cur - plan.slices_offset;
    plan.blob_size = align_up(cur, array_align);
    plans.push_back(std::move(plan));
  }

  // 2. Deterministic order by canonical symbol.
  std::sort(plans.begin(), plans.end(), plan_less);

  // 3. Geometry.
  const std::uint64_t load = opts.lookup_load_pct;
  const std::uint64_t want_slots =
      (static_cast<std::uint64_t>(n_items) * 100ull + load - 1ull) / load;
  std::uint32_t lookup_slots = atx::core::next_pow2(static_cast<std::uint32_t>(want_slots));
  if (lookup_slots < 8u) {
    lookup_slots = 8u;
  }
  const std::uint64_t lookup_offset = align_up(sizeof(ArchiveHeader), 64u);
  const std::uint64_t lookup_bytes =
      static_cast<std::uint64_t>(lookup_slots) * sizeof(ArchiveIndexSlot);
  const std::uint64_t directory_offset = lookup_offset + lookup_bytes;
  const std::uint64_t dir_bytes = static_cast<std::uint64_t>(n_items) * sizeof(ArchiveDirEntry);
  const std::uint64_t data_offset = align_up(directory_offset + dir_bytes, blob_align);

  std::uint64_t cursor = data_offset;
  for (BlobPlan &plan : plans) {
    plan.file_offset = align_up(cursor, blob_align);
    cursor = plan.file_offset + plan.blob_size;
  }
  const std::uint64_t file_size = cursor;

  // 4. Lookup table (open-addressed) + directory.
  std::vector<ArchiveIndexSlot> lookup(lookup_slots);
  std::vector<ArchiveDirEntry> directory(n_items);
  const std::uint64_t mask = static_cast<std::uint64_t>(lookup_slots) - 1ull;
  for (std::size_t idx = 0; idx < plans.size(); ++idx) {
    BlobPlan &plan = plans[idx];
    std::uint64_t i = plan.symbol_hash & mask;
    bool placed = false;
    for (std::uint32_t step = 0; step < lookup_slots; ++step) {
      ArchiveIndexSlot &s = lookup[static_cast<std::size_t>(i)];
      if (s.flags == kArchiveSlotEmpty) {
        s.symbol_hash = plan.symbol_hash;
        s.surface_offset = plan.file_offset;
        s.surface_size = plan.blob_size;
        s.surface_crc32c = 0; // patched after the blob is materialized
        s.uid = plan.uid;
        s.symbol_len = plan.symbol_len;
        s.flags = kArchiveSlotOccupied;
        std::memcpy(s.symbol, plan.symbol.data(), plan.symbol_len);
        plan.slot_index = static_cast<std::size_t>(i);
        placed = true;
        break;
      }
      if (s.symbol_hash == plan.symbol_hash && s.symbol_len == plan.symbol_len &&
          std::memcmp(s.symbol, plan.symbol.data(), plan.symbol_len) == 0) {
        return Err(ErrorCode::AlreadyExists, "write_surface_archive: duplicate canonical symbol");
      }
      i = (i + 1ull) & mask;
    }
    if (!placed) {
      return Err(ErrorCode::Internal, "write_surface_archive: lookup table full");
    }

    ArchiveDirEntry &de = directory[idx];
    de.surface_offset = plan.file_offset;
    de.surface_size = plan.blob_size;
    de.symbol_hash = plan.symbol_hash;
    de.uid = plan.uid;
    de.symbol_len = plan.symbol_len;
    de.kind_bits = plan.kind_bits;
    de.n_slices = plan.n_slices;
    std::memcpy(de.symbol, plan.symbol.data(), plan.symbol_len);
  }

  // 5. Materialize the buffer.
  std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));

  for (BlobPlan &plan : plans) {
    std::byte *base = buf_at(buffer, plan.file_offset);

    // Symbol bytes.
    std::memcpy(base + static_cast<std::size_t>(plan.symbol_offset), plan.symbol.data(),
                plan.symbol_len);

    // Pricing record.
    const ArchivePricingRecord pr = to_pricing_record(plan.surf->pricing());
    std::memcpy(base + static_cast<std::size_t>(plan.pricing_offset), &pr, sizeof pr);

    // Sequential slice records.
    std::uint64_t off = plan.slices_offset;
    for (const SlicePlan &sp : plan.slices) {
      ArchiveSliceHeader sh{};
      sh.kind = static_cast<std::uint8_t>(sp.kind);
      sh.rec_size = static_cast<std::uint32_t>(sp.rec_size);
      sh.node_count = sp.node_count;
      sh.payload_size = sp.payload_size;
      sh.T = sp.ctx.T;
      sh.forward = sp.ctx.forward;
      sh.borrow = sp.ctx.borrow;
      sh.q_eff = sp.ctx.q_eff;
      sh.df = sp.curve->df();
      sh.n_used = sp.ctx.n_used;
      sh.n_dropped = sp.ctx.n_dropped;

      std::byte *rec = base + static_cast<std::size_t>(off);
      std::byte *payload = rec + sizeof(ArchiveSliceHeader);
      switch (sp.kind) {
      case VolCurveKind::ConvexDense: {
        const ConvexSliceFit &fit = static_cast<const ConvexDenseCurve *>(sp.curve)->fit();
        sh.conv_rmse_price = fit.rmse_price;
        sh.conv_n_obs = fit.n_obs;
        sh.conv_n_active = fit.n_active;
        const std::size_t nb = static_cast<std::size_t>(sp.node_count) * sizeof(double);
        std::memcpy(payload, fit.u.data(), nb);
        std::memcpy(payload + nb, fit.C.data(), nb);
        break;
      }
      case VolCurveKind::Essvi: {
        const EssviParams &e = static_cast<const EssviCurve *>(sp.curve)->slice();
        std::memcpy(payload, &e, sizeof e);
        break;
      }
      case VolCurveKind::Svi: {
        const SviParams &v = static_cast<const SviCurve *>(sp.curve)->slice();
        std::memcpy(payload, &v, sizeof v);
        break;
      }
      case VolCurveKind::LinearVariance: {
        const auto *lv = static_cast<const LinearVarianceCurve *>(sp.curve);
        const std::size_t nb = static_cast<std::size_t>(sp.node_count) * sizeof(double);
        std::memcpy(payload, lv->k_nodes().data(), nb);
        std::memcpy(payload + nb, lv->w_nodes().data(), nb);
        break;
      }
      case VolCurveKind::C8: {
        const C8Params &c8 = static_cast<const C8Curve *>(sp.curve)->slice();
        std::memcpy(payload, &c8, sizeof c8);
        break;
      }
      case VolCurveKind::SplineVol: {
        // Payload layout (see slice_payload_size's SplineVol case for the
        // full versioning rationale): atm_vol, z_lo_valid, z_hi_valid (f64 x3,
        // offsets 0/8/16), n (u32, offset 24), z[n] (offset 28), mult[n]
        // (offset 28 + 8n), n_butterfly_viol (u32, offset 28 + 16n).
        const auto *sv = static_cast<const SplineVolCurve *>(sp.curve);
        const SplineVolParams &p = sv->params();
        std::size_t poff = 0;
        std::memcpy(payload + poff, &p.atm_vol, sizeof(double));
        poff += sizeof(double);
        std::memcpy(payload + poff, &p.z_lo_valid, sizeof(double));
        poff += sizeof(double);
        std::memcpy(payload + poff, &p.z_hi_valid, sizeof(double));
        poff += sizeof(double);
        const auto n32 = static_cast<std::uint32_t>(p.z.size());
        std::memcpy(payload + poff, &n32, sizeof(std::uint32_t));
        poff += sizeof(std::uint32_t);
        const std::size_t nb = p.z.size() * sizeof(double);
        std::memcpy(payload + poff, p.z.data(), nb);
        poff += nb;
        std::memcpy(payload + poff, p.mult.data(), nb);
        poff += nb;
        const auto viol32 = static_cast<std::uint32_t>(p.n_butterfly_viol);
        std::memcpy(payload + poff, &viol32, sizeof(std::uint32_t));
        break;
      }
      }
      std::memcpy(rec, &sh, sizeof sh); // header last (fields now complete)
      off += sp.rec_size;
    }

    // Blob header (written after the payload so payload_crc32c is well-defined).
    SurfaceBlobHeader bh{};
    std::memcpy(bh.magic, kBlobMagic, 8);
    bh.major = kArchiveMajor;
    bh.minor = kArchiveMinor;
    bh.n_slices = plan.n_slices;
    bh.uid = plan.uid;
    bh.blob_header_size = static_cast<std::uint32_t>(sizeof(SurfaceBlobHeader));
    bh.blob_size = plan.blob_size;
    bh.symbol_offset = plan.symbol_offset;
    bh.symbol_size = plan.symbol_size;
    bh.pricing_offset = plan.pricing_offset;
    bh.pricing_size = plan.pricing_size;
    bh.slices_offset = plan.slices_offset;
    bh.slices_size = plan.slices_size;
    bh.payload_crc32c =
        crc32c(base + sizeof(SurfaceBlobHeader),
               static_cast<std::size_t>(plan.blob_size - sizeof(SurfaceBlobHeader)));
    if (plan.provenance.has_value()) {
      const ArchiveSurfaceProvenanceRecord record = to_provenance_record(*plan.provenance);
      std::memcpy(bh.reserved, &record, sizeof record);
    }
    std::memcpy(base, &bh, sizeof bh);

    // Whole-blob CRC into the owning lookup slot.
    plan.crc32c = crc32c(base, static_cast<std::size_t>(plan.blob_size));
    lookup[plan.slot_index].surface_crc32c = plan.crc32c;
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
  ArchiveHeader hdr;
  std::memcpy(hdr.magic, kArchiveMagic, 8);
  hdr.major = kArchiveMajor;
  hdr.minor = kArchiveMinor;
  hdr.header_size = static_cast<std::uint16_t>(sizeof(ArchiveHeader));
  hdr.endian = 1;
  hdr.pointer_bits = 64;
  hdr.alignment_log2 = 12;
  hdr.flags = opts.flags;
  hdr.file_size = file_size;
  hdr.created_ts_ns = opts.created_ts_ns != 0 ? static_cast<std::uint64_t>(opts.created_ts_ns)
                                              : static_cast<std::uint64_t>(wall_clock_ns());
  hdr.schema_hash = schema_hash();
  hdr.writer_version_hash = 0;
  hdr.surface_count = n_items;
  hdr.lookup_slot_count = lookup_slots;
  hdr.lookup_offset = lookup_offset;
  hdr.directory_offset = directory_offset;
  hdr.data_offset = data_offset;
  hdr.index_slot_size = static_cast<std::uint32_t>(sizeof(ArchiveIndexSlot));
  hdr.dir_entry_size = static_cast<std::uint32_t>(sizeof(ArchiveDirEntry));
  hdr.surface_blob_header_size = static_cast<std::uint32_t>(sizeof(SurfaceBlobHeader));
  hdr.slice_header_size = static_cast<std::uint32_t>(sizeof(ArchiveSliceHeader));
  hdr.metadata_crc32c = meta;
  hdr.header_crc32c = header_crc(hdr);
  std::memcpy(buf_at(buffer, 0), &hdr, sizeof hdr);

  return Ok(std::move(buffer));
}

Status write_surface_archive_file(std::string_view path, std::span<const SurfaceArchiveItem> items,
                                  const SurfaceArchiveWriteOpts &opts) {
  auto built = write_surface_archive(items, opts);
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
      return Err(ErrorCode::IoError, "write_surface_archive_file: cannot open temp file");
    }
    os.write(reinterpret_cast<const char *>(buffer.data()),
             static_cast<std::streamsize>(buffer.size()));
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "write_surface_archive_file: write failed");
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    std::filesystem::remove(tmp, ec2);
    return Err(ErrorCode::IoError, "write_surface_archive_file: rename failed");
  }
  return Ok();
}

// ── Reader ───────────────────────────────────────────────────────────────

Result<SurfaceArchive> SurfaceArchive::open(std::vector<std::byte> bytes) {
  if (bytes.size() < sizeof(ArchiveHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: shorter than header");
  }

  SurfaceArchive a;
  a.buffer_ = std::move(bytes);
  const std::vector<std::byte> &buf = a.buffer_;

  ArchiveHeader h;
  std::memcpy(&h, buf.data(), sizeof h);

  if (std::memcmp(h.magic, kArchiveMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: bad magic");
  }
  if (h.major != kArchiveMajor) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: unsupported major version");
  }
  if (h.endian != 1) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: non-little-endian archive");
  }
  if (h.pointer_bits != 64) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: unsupported pointer width");
  }
  if (h.header_size != sizeof(ArchiveHeader) || h.index_slot_size != sizeof(ArchiveIndexSlot) ||
      h.dir_entry_size != sizeof(ArchiveDirEntry) ||
      h.surface_blob_header_size != sizeof(SurfaceBlobHeader) ||
      h.slice_header_size != sizeof(ArchiveSliceHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: record size mismatch");
  }
  if (h.schema_hash != schema_hash()) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: schema hash mismatch");
  }
  if (h.file_size != buf.size()) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: file size mismatch");
  }
  if (h.lookup_slot_count == 0 || !atx::core::is_pow2(h.lookup_slot_count)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup slot count not a power of two");
  }

  const std::uint64_t lookup_bytes =
      static_cast<std::uint64_t>(h.lookup_slot_count) * h.index_slot_size;
  const std::uint64_t dir_bytes = static_cast<std::uint64_t>(h.surface_count) * h.dir_entry_size;
  if (h.lookup_offset < sizeof(ArchiveHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup overlaps header");
  }
  if (h.lookup_offset > h.file_size || lookup_bytes > h.file_size - h.lookup_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup out of bounds");
  }
  if (h.lookup_offset + lookup_bytes > h.directory_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup overlaps directory");
  }
  if (h.directory_offset > h.file_size || dir_bytes > h.file_size - h.directory_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: directory out of bounds");
  }
  if (h.directory_offset + dir_bytes > h.data_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: directory overlaps data");
  }
  if (h.data_offset > h.file_size) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: data offset out of bounds");
  }
  if (header_crc(h) != h.header_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: header checksum mismatch");
  }

  std::uint32_t meta = crc32c_update(0xFFFFFFFFu, buf_at(buf, h.lookup_offset),
                                     static_cast<std::size_t>(lookup_bytes));
  meta = crc32c_update(meta, buf_at(buf, h.directory_offset), static_cast<std::size_t>(dir_bytes)) ^
         0xFFFFFFFFu;
  if (meta != h.metadata_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: metadata checksum mismatch");
  }

  a.header_ = h;
  a.lookup_.resize(h.lookup_slot_count);
  if (lookup_bytes > 0) {
    std::memcpy(a.lookup_.data(), buf_at(buf, h.lookup_offset),
                static_cast<std::size_t>(lookup_bytes));
  }
  a.directory_.resize(h.surface_count);
  if (dir_bytes > 0) {
    std::memcpy(a.directory_.data(), buf_at(buf, h.directory_offset),
                static_cast<std::size_t>(dir_bytes));
  }

  for (const ArchiveIndexSlot &slot : a.lookup_) {
    if (slot.symbol_len > kArchiveSymbolMax) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup symbol length out of bounds");
    }
  }
  for (const ArchiveDirEntry &de : a.directory_) {
    if (de.symbol_len > kArchiveSymbolMax) {
      return Err(ErrorCode::ParseError,
                 "SurfaceArchive::open: directory symbol length out of bounds");
    }
    if (de.surface_offset < h.data_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::open: directory entry precedes data");
    }
    if (de.surface_offset > h.file_size || de.surface_size > h.file_size - de.surface_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::open: directory entry out of bounds");
    }
  }

  return Ok(std::move(a));
}

Result<SurfaceArchive> SurfaceArchive::open_file(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || ec) {
    return Err(ErrorCode::NotFound, "SurfaceArchive::open_file: file not found");
  }
  std::ifstream is(p, std::ios::binary | std::ios::ate);
  if (!is) {
    return Err(ErrorCode::IoError, "SurfaceArchive::open_file: cannot open file");
  }
  const std::streamsize size = is.tellg();
  if (size < 0) {
    return Err(ErrorCode::IoError, "SurfaceArchive::open_file: cannot size file");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  is.seekg(0);
  is.read(reinterpret_cast<char *>(bytes.data()), size);
  if (is.gcount() != size) {
    return Err(ErrorCode::IoError, "SurfaceArchive::open_file: short read");
  }
  return open(std::move(bytes));
}

const ArchiveIndexSlot *SurfaceArchive::find_slot(std::string_view symbol) const noexcept {
  if (lookup_.empty()) {
    return nullptr;
  }
  const std::string canon_sym = detail::canonicalize_symbol(symbol, kArchiveSymbolMax);
  const auto len = static_cast<std::uint16_t>(canon_sym.size());
  if (len == 0) {
    return nullptr;
  }
  const std::uint64_t h = atx::core::hash_bytes(canon_sym.data(), len);
  const std::uint64_t mask = static_cast<std::uint64_t>(lookup_.size()) - 1ull;
  std::uint64_t i = h & mask;
  for (std::size_t step = 0; step < lookup_.size(); ++step) {
    const ArchiveIndexSlot &s = lookup_[static_cast<std::size_t>(i)];
    if (s.flags == kArchiveSlotEmpty) {
      return nullptr;
    }
    if (s.symbol_hash == h && s.symbol_len == len &&
        std::memcmp(s.symbol, canon_sym.data(), len) == 0) {
      return &s;
    }
    i = (i + 1ull) & mask;
  }
  return nullptr;
}

Result<ArchiveDirEntry> SurfaceArchive::find(std::string_view symbol) const {
  const ArchiveIndexSlot *s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchive::find: symbol not present");
  }
  ArchiveDirEntry de;
  de.surface_offset = s->surface_offset;
  de.surface_size = s->surface_size;
  de.symbol_hash = s->symbol_hash;
  de.uid = s->uid;
  de.symbol_len = s->symbol_len;
  std::memcpy(de.symbol, s->symbol, s->symbol_len);
  return de;
}

Result<SurfaceProvenance> SurfaceArchive::read_provenance(std::uint64_t offset, std::uint64_t size,
                                                          std::uint32_t expected_crc) const {
  if (size < sizeof(SurfaceBlobHeader) || offset > buffer_.size() ||
      size > buffer_.size() - offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::provenance: blob out of bounds");
  }
  const std::byte *base = buf_at(buffer_, offset);
  if (crc32c(base, static_cast<std::size_t>(size)) != expected_crc) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::provenance: blob checksum mismatch");
  }
  SurfaceBlobHeader header{};
  std::memcpy(&header, base, sizeof header);
  if (std::memcmp(header.magic, kBlobMagic, 8) != 0 || header.major != kArchiveMajor ||
      header.blob_size != size || header.blob_header_size != sizeof(SurfaceBlobHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::provenance: invalid blob header");
  }
  ArchiveSurfaceProvenanceRecord record{};
  std::memcpy(&record, header.reserved, sizeof record);
  return from_provenance_record(record);
}

Result<SurfaceProvenance> SurfaceArchive::provenance(std::string_view symbol) const {
  const ArchiveIndexSlot *slot = find_slot(symbol);
  if (slot == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchive::provenance: symbol not present");
  }
  return read_provenance(slot->surface_offset, slot->surface_size, slot->surface_crc32c);
}

Result<ArchivedSurface> SurfaceArchive::reconstruct(const ArchiveIndexSlot &slot,
                                                    const ArchiveDirEntry *directory) const {
  const std::uint64_t offset = slot.surface_offset;
  const std::uint64_t size = slot.surface_size;
  if (size < sizeof(SurfaceBlobHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob smaller than header");
  }
  if (offset > buffer_.size() || size > buffer_.size() - offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob out of bounds");
  }
  const std::byte *base = buf_at(buffer_, offset);

  if (crc32c(base, static_cast<std::size_t>(size)) != slot.surface_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob checksum mismatch");
  }

  SurfaceBlobHeader bh;
  std::memcpy(&bh, base, sizeof bh);
  if (std::memcmp(bh.magic, kBlobMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: bad blob magic");
  }
  if (bh.major != kArchiveMajor) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: unsupported blob version");
  }
  if (bh.blob_size != size || bh.blob_header_size != sizeof(SurfaceBlobHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob size mismatch");
  }
  if (bh.uid != slot.uid) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob uid mismatch");
  }
  if (bh.symbol_offset < sizeof(SurfaceBlobHeader) || bh.symbol_offset > size ||
      bh.symbol_size > size - bh.symbol_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: symbol section out of bounds");
  }
  if (bh.pricing_offset < sizeof(SurfaceBlobHeader) ||
      bh.pricing_size < sizeof(ArchivePricingRecord) || bh.pricing_offset > size ||
      bh.pricing_size > size - bh.pricing_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: pricing section out of bounds");
  }
  if (bh.slices_offset < sizeof(SurfaceBlobHeader) || bh.slices_offset > size ||
      bh.slices_size > size - bh.slices_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: slices section out of bounds");
  }
  if ((bh.symbol_offset % kArchiveArrayAlign) != 0u ||
      (bh.pricing_offset % kArchiveArrayAlign) != 0u ||
      (bh.slices_offset % kArchiveArrayAlign) != 0u) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: section alignment mismatch");
  }
  const std::uint64_t symbol_end = bh.symbol_offset + bh.symbol_size;
  const std::uint64_t pricing_end = bh.pricing_offset + bh.pricing_size;
  if (symbol_end > bh.pricing_offset || pricing_end > bh.slices_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: section topology mismatch");
  }
  if (bh.symbol_size != slot.symbol_len ||
      std::memcmp(base + static_cast<std::size_t>(bh.symbol_offset), slot.symbol,
                  slot.symbol_len) != 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: symbol identity mismatch");
  }
  if (directory != nullptr && bh.n_slices != directory->n_slices) {
    return Err(ErrorCode::ParseError,
               "SurfaceArchive::reconstruct: directory slice count mismatch");
  }
  ArchiveSurfaceProvenanceRecord provenance_record{};
  std::memcpy(&provenance_record, bh.reserved, sizeof provenance_record);
  auto provenance = from_provenance_record(provenance_record);
  if (!provenance.has_value()) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: invalid surface provenance");
  }
  // NOTE: the whole-blob `surface_crc32c` verified above STRICTLY SUBSUMES the
  // payload CRC (payload bytes are a subspan of the blob), so the hot path does NOT
  // re-CRC the payload — one pass, not two. `payload_crc32c` is still written (a
  // self-describing field for external tools / partial verification) but not
  // re-checked here; verifying it would double the per-surface CRC cost for no
  // added integrity.

  const std::size_t n = bh.n_slices;
  if (n == 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: zero slices");
  }

  ArchivePricingRecord pr;
  std::memcpy(&pr, base + static_cast<std::size_t>(bh.pricing_offset), sizeof pr);
  if (pr.uid != slot.uid) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: pricing uid mismatch");
  }
  const PricingContext pc = from_pricing_record(pr);

  CurveSurface surface;
  std::vector<SliceContext> ctx;
  ctx.reserve(n);

  std::uint64_t off = bh.slices_offset;
  const std::uint64_t slices_end = bh.slices_offset + bh.slices_size;
  std::uint16_t kind_bits = 0u;
  for (std::size_t i = 0; i < n; ++i) {
    if (off > slices_end || sizeof(ArchiveSliceHeader) > slices_end - off) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: slice header out of bounds");
    }
    ArchiveSliceHeader sh;
    std::memcpy(&sh, base + static_cast<std::size_t>(off), sizeof sh);
    if (sh.rec_size < sizeof(ArchiveSliceHeader) || sh.rec_size > slices_end - off) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: slice record out of bounds");
    }
    if ((sh.rec_size % kArchiveArrayAlign) != 0u) {
      return Err(ErrorCode::ParseError,
                 "SurfaceArchive::reconstruct: slice record alignment mismatch");
    }
    if (sh.payload_size > sh.rec_size - sizeof(ArchiveSliceHeader)) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: slice payload out of bounds");
    }
    const std::byte *payload = base + static_cast<std::size_t>(off + sizeof(ArchiveSliceHeader));
    const auto kind = static_cast<VolCurveKind>(sh.kind);

    std::unique_ptr<IVolCurve> curve;
    switch (kind) {
    case VolCurveKind::ConvexDense: {
      const std::uint64_t need = 2ull * static_cast<std::uint64_t>(sh.node_count) * sizeof(double);
      if (sh.node_count == 0 || need != sh.payload_size) {
        return Err(ErrorCode::ParseError,
                   "SurfaceArchive::reconstruct: convex node payload size mismatch");
      }
      ConvexSliceFit fit;
      fit.T = sh.T;
      fit.F = sh.forward;
      fit.df = sh.df;
      fit.rmse_price = sh.conv_rmse_price;
      fit.n_obs = static_cast<std::size_t>(sh.conv_n_obs);
      fit.n_active = static_cast<std::size_t>(sh.conv_n_active);
      fit.u.resize(sh.node_count);
      fit.C.resize(sh.node_count);
      const std::size_t nb = static_cast<std::size_t>(sh.node_count) * sizeof(double);
      std::memcpy(fit.u.data(), payload, nb);
      std::memcpy(fit.C.data(), payload + nb, nb);
      curve = std::make_unique<ConvexDenseCurve>(std::move(fit));
      break;
    }
    case VolCurveKind::Essvi: {
      if (sh.payload_size < sizeof(EssviParams)) {
        return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: essvi payload too small");
      }
      EssviParams e;
      std::memcpy(&e, payload, sizeof e);
      curve = std::make_unique<EssviCurve>(e, sh.df);
      break;
    }
    case VolCurveKind::Svi: {
      if (sh.payload_size < sizeof(SviParams)) {
        return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: svi payload too small");
      }
      SviParams v;
      std::memcpy(&v, payload, sizeof v);
      curve = std::make_unique<SviCurve>(v, sh.df);
      break;
    }
    case VolCurveKind::LinearVariance: {
      const std::uint64_t need = 2ull * static_cast<std::uint64_t>(sh.node_count) * sizeof(double);
      if (sh.node_count < 2 || need != sh.payload_size) {
        return Err(ErrorCode::ParseError,
                   "SurfaceArchive::reconstruct: linear node payload size mismatch");
      }
      std::vector<double> k(sh.node_count);
      std::vector<double> w(sh.node_count);
      const std::size_t nb = static_cast<std::size_t>(sh.node_count) * sizeof(double);
      std::memcpy(k.data(), payload, nb);
      std::memcpy(w.data(), payload + nb, nb);
      curve = std::make_unique<LinearVarianceCurve>(sh.T, sh.forward, sh.df, std::move(k),
                                                    std::move(w));
      break;
    }
    case VolCurveKind::C8: {
      if (sh.payload_size < sizeof(C8Params)) {
        return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: C8 payload too small");
      }
      C8Params c8;
      std::memcpy(&c8, payload, sizeof c8);
      curve = std::make_unique<C8Curve>(c8, sh.df);
      break;
    }
    case VolCurveKind::SplineVol: {
      // Additive ATXVSA payload (Task I5) -- see slice_payload_size's
      // SplineVol case (surface_archive.cpp) for the full wire-format
      // description + versioning rationale.
      constexpr std::uint64_t kFixedBytes = 32; // atm_vol+z_lo+z_hi+n+n_butterfly_viol
      if (sh.payload_size < kFixedBytes) {
        return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: spline payload too small");
      }
      std::uint32_t n = 0;
      std::memcpy(&n, payload + 24, sizeof n);
      const std::uint64_t need = kFixedBytes + 16ull * static_cast<std::uint64_t>(n);
      if (n != sh.node_count || need != sh.payload_size) {
        return Err(ErrorCode::ParseError,
                   "SurfaceArchive::reconstruct: spline node payload size mismatch");
      }
      SplineVolParams p;
      std::memcpy(&p.atm_vol, payload + 0, sizeof(double));
      std::memcpy(&p.z_lo_valid, payload + 8, sizeof(double));
      std::memcpy(&p.z_hi_valid, payload + 16, sizeof(double));
      p.z.resize(n);
      p.mult.resize(n);
      const std::size_t nb = static_cast<std::size_t>(n) * sizeof(double);
      std::memcpy(p.z.data(), payload + 28, nb);
      std::memcpy(p.mult.data(), payload + 28 + nb, nb);
      std::uint32_t viol = 0;
      std::memcpy(&viol, payload + 28 + 2 * nb, sizeof viol);
      p.n_butterfly_viol = viol;
      curve = std::make_unique<SplineVolCurve>(std::move(p), sh.T, sh.forward, sh.df);
      break;
    }
    default:
      return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: unknown curve kind");
    }
    kind_bits |= static_cast<std::uint16_t>(1u << static_cast<unsigned>(kind));
    surface.push(std::move(curve));

    SliceContext sc;
    sc.T = sh.T;
    sc.forward = sh.forward;
    sc.borrow = sh.borrow;
    sc.q_eff = sh.q_eff;
    sc.n_used = static_cast<std::size_t>(sh.n_used);
    sc.n_dropped = static_cast<std::size_t>(sh.n_dropped);
    ctx.push_back(sc);

    off += sh.rec_size;
  }
  if (off != slices_end) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: incomplete slice consumption");
  }
  if (directory != nullptr && kind_bits != directory->kind_bits) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: directory kind bits mismatch");
  }

  auto ps = PricedSurface::create(std::move(surface), std::move(ctx), pc);
  if (!ps) {
    return Err(ErrorCode::ParseError,
               "SurfaceArchive::reconstruct: reconstructed surface failed validation");
  }
  return Ok(ArchivedSurface{std::move(*ps), std::move(*provenance)});
}

Result<PricedSurface> SurfaceArchive::map_symbol(std::string_view symbol) const {
  const ArchiveIndexSlot *s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchive::map_symbol: symbol not present");
  }
  auto archived = reconstruct(*s, nullptr);
  if (!archived.has_value()) {
    return tl::unexpected<atx::core::Error>(std::move(archived).error());
  }
  return Ok(std::move(archived->surface));
}

Result<std::vector<PricedSurface>> SurfaceArchive::map_all() const {
  std::vector<PricedSurface> out;
  out.reserve(directory_.size());
  for (const ArchiveDirEntry &de : directory_) {
    const ArchiveIndexSlot *slot = find_directory_slot(lookup_, de);
    if (slot == nullptr || !directory_identity_matches(de, *slot)) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::map_all: directory/lookup mismatch");
    }
    auto res = reconstruct(*slot, &de);
    if (!res) {
      return tl::unexpected<atx::core::Error>(std::move(res).error());
    }
    out.push_back(std::move(res->surface));
  }
  return Ok(std::move(out));
}

Result<std::vector<ArchivedSurface>> SurfaceArchive::map_all_with_provenance() const {
  std::vector<ArchivedSurface> out;
  out.reserve(directory_.size());
  for (const ArchiveDirEntry &de : directory_) {
    const ArchiveIndexSlot *slot = find_directory_slot(lookup_, de);
    if (slot == nullptr || !directory_identity_matches(de, *slot)) {
      return Err(ErrorCode::ParseError,
                 "SurfaceArchive::map_all_with_provenance: directory/lookup mismatch");
    }
    auto res = reconstruct(*slot, &de);
    if (!res.has_value()) {
      return tl::unexpected<atx::core::Error>(std::move(res).error());
    }
    out.push_back(std::move(*res));
  }
  return Ok(std::move(out));
}

Result<std::size_t>
SurfaceArchive::map_all_into(std::span<std::optional<PricedSurface>> out) const {
  if (out.size() < directory_.size()) {
    return Err(ErrorCode::OutOfRange, "SurfaceArchive::map_all_into: output too small");
  }
  auto all = map_all();
  if (!all) {
    return tl::unexpected<atx::core::Error>(std::move(all).error());
  }
  std::vector<PricedSurface> surfaces = *std::move(all);
  const std::size_t count = surfaces.size();
  for (std::size_t i = 0; i < count; ++i) {
    out[i].emplace(std::move(surfaces[i]));
  }
  return Ok(count);
}

// ═══════════════════════════════════════════════════════════════════════════
// ATXVSA2 (v2) — zero-copy mmap columnar format. Layout spec + research lineage:
// atx-vol/docs/atxvsa2-format.md. Primary sources cited there (FlatBuffers
// natural alignment; Cap'n Proto relative byte-offset segments; Apache Arrow
// contiguous typed columns + mmap-first alignment). This is a CLEAN-BREAK sibling
// to the v1 reader/writer above (§0): distinct types, no dual-read.
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

  for (V2SurfacePlan &plan : plans) {
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
  hdr.created_ts_ns = opts.created_ts_ns != 0 ? static_cast<std::uint64_t>(opts.created_ts_ns)
                                              : static_cast<std::uint64_t>(wall_clock_ns());
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
