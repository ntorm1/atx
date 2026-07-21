// ── ATXVSA (v1) archive read/write — ISOLATED to bench/migrator/test TUs (WS-G G2) ──
//
// This TU holds the LEGACY v1 SurfaceArchive reader + write_surface_archive[_file]
// writer, split out of src/surface_archive.cpp so the PRODUCT library atx::vol no
// longer links v1 read/write (parent adjudication A). No product src/ TU calls v1
// (verified: only the v2 SurfaceArchiveV2/map_symbol/reconstruct_symbol path is on
// the hot path); the sole v1 consumers are the migrator (tools/migrate_atxvsa_v1_to_v2),
// a handful of round-trip/format tests, and two archive benches. Those targets link
// the small `atx-vol-archive-v1` static library (which links atx::vol for the shared
// curve/priced-surface/detail symbols — dependency direction v1 -> vol, never the
// reverse, so vol.lib stays v1-free and there is no link cycle).
//
// The anonymous-namespace helpers below are copied VERBATIM from surface_archive.cpp;
// the ones also needed by the v2 side (buf_at / wall_clock_ns / the provenance-record
// converters / kKnownValidationFailures) stay defined in BOTH TUs — legal because they
// have internal linkage (no ODR hazard, no duplicate symbol). legacy_surface_provenance()
// is the one shared EXTERNAL symbol: it stays defined in atx::vol and is referenced here
// (declared in surface_archive.hpp), resolved at final link.

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
  // created_ts_ns: an explicit nonzero stamp is honored verbatim. The 0 sentinel
  // is filled from a DETERMINISTIC CRC-32C of the archive CONTENT — the whole
  // payload span [header, EOF): data ‖ lookup ‖ directory — folded with file_size,
  // so two identical builds produce byte-identical containers while two DIFFERENT
  // builds still get distinct stamps (preserving the SnapshotCache content-identity
  // that keys on created_ts_ns). Mirrors the v2 writer; the span excludes the
  // header, so header_crc32c/created_ts_ns are not inputs to their own hash.
  if (opts.created_ts_ns != 0) {
    hdr.created_ts_ns = static_cast<std::uint64_t>(opts.created_ts_ns);
  } else {
    const std::uint32_t content_crc =
        crc32c(buf_at(buffer, sizeof(ArchiveHeader)),
               static_cast<std::size_t>(file_size - sizeof(ArchiveHeader)));
    const std::uint64_t derived =
        (static_cast<std::uint64_t>(content_crc) << 32) ^ static_cast<std::uint64_t>(file_size);
    hdr.created_ts_ns = derived != 0 ? derived : 1u; // never re-hit the 0 sentinel
  }
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

} // namespace atx::vol
