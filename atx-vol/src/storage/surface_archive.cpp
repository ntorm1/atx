#include "atx/vol/api/storage/surface_archive.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
#include "atx/tsdb/mapping.hpp"                // tsdb::Mapping (open_mapped)
#include "atx/vol/api/pricing/american.hpp"    // AlOpts, AmericanMethod
#include "atx/vol/api/fitting/dense_slice.hpp" // ConvexSliceFit
#include "storage/archive_util.hpp"            // crc32c, align_up, canonicalize, PositionalFile
#include "storage/surface_archive_internal.hpp" // read_archive_v2_first_now_ts
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"   // IVolCurve, Convex/Essvi/SviCurve, CurveSurface
#include "atx/vol/api/fitting/vol_surface.hpp" // EssviParams, SviParams

#include "storage/slice_payload_padding.hpp" // normalize_{svi,essvi,c8}_payload_padding

// ATXVSA2 revision 1 — the layout, the point-read algorithm and the integrity model are
// specified in atx-vol/docs/atxvsa2-format.md (section numbers below refer to it).
// Design lineage: FlatBuffers natural alignment, Cap'n Proto relative byte offsets,
// Apache Arrow contiguous typed columns (sources in the spec, §13).

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

using detail::align_up;
using detail::crc32c;
using detail::crc32c_update;

// The native slice layouts are the record payload ABI; named fields are serialized
// verbatim and their padding re-zeroed (slice_payload_padding.hpp).
static_assert(std::is_trivially_copyable_v<EssviParams>);
static_assert(std::is_trivially_copyable_v<SviParams>);
static_assert(std::is_trivially_copyable_v<C8Params>);
static_assert(std::is_trivially_copyable_v<AlOpts>);
static_assert(sizeof(C8Params) == 176, "C8Params is a record payload; changing it is a format "
                                       "change (it is folded into the schema hash, spec §7)");

namespace {

// Bits 0..12 — includes both publish-with-Degraded reasons, ValidationFailure::CarryGap
// (1u << 11) and ValidationFailure::SubstituteUnderserve (1u << 12): a Degraded
// provenance carrying either is a routinely SERVED state and must round-trip.
constexpr std::uint32_t kKnownValidationFailures = (1u << 13) - 1u;

// Schema salt (spec §7). One reader accepts exactly one schema: there is no prior-salt
// accept-list. Revision 0 used ...0100/0101/0102.
constexpr std::uint64_t kV2Salt = 0xA7C3'5F04'2E1F'0201ull;

[[nodiscard]] std::uint64_t schema_hash_v2() noexcept {
  constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
  std::uint64_t h = 0x9e3779b97f4a7c15ull ^ kV2Salt;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveV2Header)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveV2LookupSlot)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveV2DirEntry)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveV2SurfaceHeader)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(EssviParams)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(SviParams)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(C8Params)) * kFnvPrime;
  return h;
}

[[nodiscard]] std::uint32_t header_crc_v2(ArchiveV2Header h) noexcept {
  h.header_crc32c = 0;
  std::array<std::byte, sizeof(ArchiveV2Header)> bytes{};
  std::memcpy(bytes.data(), &h, sizeof h);
  return crc32c(bytes.data(), bytes.size());
}

[[nodiscard]] std::uint32_t slot_crc_v2(ArchiveV2LookupSlot s) noexcept {
  s.slot_crc32c = 0;
  std::array<std::byte, sizeof(ArchiveV2LookupSlot)> bytes{};
  std::memcpy(bytes.data(), &s, sizeof s);
  return crc32c(bytes.data(), bytes.size());
}

// CRC-32C over a whole record with its own payload_crc32c field forced to 0 (the exact
// bytes the writer checksummed). Piecewise so no temp copy is needed. Precondition:
// size >= sizeof(ArchiveV2SurfaceHeader).
[[nodiscard]] std::uint32_t record_crc_v2(const std::byte *base, std::uint64_t size) noexcept {
  constexpr std::size_t crc_off = offsetof(ArchiveV2SurfaceHeader, payload_crc32c);
  const std::uint32_t zero = 0;
  std::uint32_t c = crc32c_update(0xFFFFFFFFu, base, crc_off);
  c = crc32c_update(c, reinterpret_cast<const std::byte *>(&zero), sizeof zero);
  c = crc32c_update(c, base + crc_off + sizeof(std::uint32_t),
                    static_cast<std::size_t>(size) - crc_off - sizeof(std::uint32_t));
  return c ^ 0xFFFFFFFFu;
}

template <std::size_t N> [[nodiscard]] bool all_zero(const std::uint8_t (&b)[N]) noexcept {
  return std::all_of(std::begin(b), std::end(b), [](std::uint8_t v) { return v == 0u; });
}

// `bytes[len, N)` all zero: the canonical zero padding of a fixed symbol field.
template <std::size_t N>
[[nodiscard]] bool symbol_tail_zero(const char (&s)[N], std::size_t len) noexcept {
  return std::all_of(s + std::min(len, N), s + N, [](char c) { return c == '\0'; });
}

// The 8 magic bytes in printable form, for the refusal message.
[[nodiscard]] std::string printable_magic(const char (&magic)[8]) {
  std::string out;
  for (const char c : magic) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x20u && u < 0x7Fu) {
      out.push_back(c);
    } else {
      constexpr char kHex[] = "0123456789ABCDEF";
      out += "\\x";
      out.push_back(kHex[u >> 4u]);
      out.push_back(kHex[u & 0xFu]);
    }
  }
  return out;
}

// Spec §0: name the container we were handed when it is not ATXVSA21.
[[nodiscard]] Status check_file_magic(const char (&magic)[8], std::string_view ctx) {
  if (std::memcmp(magic, kArchiveV2Magic, sizeof kArchiveV2Magic) == 0) {
    return Ok();
  }
  const std::string found = printable_magic(magic);
  if (found == "ATXVSA20") {
    return Err(ErrorCode::ParseError,
               std::string(ctx) + ": found magic 'ATXVSA20' (ATXVSA2 revision 0); this reader "
                                  "reads only ATXVSA21 - rebuild the archive");
  }
  if (found == "ATXVSA03") {
    return Err(ErrorCode::ParseError,
               std::string(ctx) + ": found magic 'ATXVSA03' (the retired ATXVSA03 format); this "
                                  "reader reads only ATXVSA21 - rebuild the archive");
  }
  return Err(ErrorCode::ParseError,
             std::string(ctx) + ": bad magic '" + found + "' (not an ATXVSA2 archive)");
}

// Every header check of spec §4 step 3 / §5 — shared by the point read, the whole-board
// open and `SurfaceDb::session_ts`'s positional reader.
[[nodiscard]] Status validate_header_v2(const ArchiveV2Header &h, std::uint64_t real_size,
                                        std::string_view ctx) {
  const auto fail = [&](const char *what) {
    return Err(ErrorCode::ParseError, std::string(ctx) + ": " + what);
  };
  if (Status magic = check_file_magic(h.magic, ctx); !magic) {
    return magic;
  }
  if (h.major != kArchiveV2Major) {
    return fail("unsupported major version");
  }
  if (h.endian != 1u) {
    return fail("non-little-endian archive");
  }
  if (h.pointer_bits != 64u) {
    return fail("unsupported pointer width");
  }
  if (h.header_size != sizeof(ArchiveV2Header) ||
      h.lookup_slot_size != sizeof(ArchiveV2LookupSlot) ||
      h.dir_entry_size != sizeof(ArchiveV2DirEntry) ||
      h.surface_header_size != sizeof(ArchiveV2SurfaceHeader)) {
    return fail("record size mismatch");
  }
  if (h.schema_hash != schema_hash_v2()) {
    return fail("schema hash mismatch");
  }
  if (header_crc_v2(h) != h.header_crc32c) {
    return fail("header checksum mismatch");
  }
  if (h.file_size != real_size) {
    return fail("file size mismatch (truncated or appended)");
  }
  if (!all_zero(h.reserved)) {
    return fail("non-zero reserved header bytes");
  }
  if (h.surface_count > kArchiveV2MaxSurfaces) {
    return fail("surface count exceeds the format bound");
  }
  if (h.lookup_slot_count < 8u || !atx::core::is_pow2(h.lookup_slot_count) ||
      h.lookup_slot_count <= h.surface_count) {
    return fail("lookup slot count must be a power of two >= 8 with an empty slot");
  }
  const std::uint64_t lookup_bytes =
      static_cast<std::uint64_t>(h.lookup_slot_count) * sizeof(ArchiveV2LookupSlot);
  const std::uint64_t dir_bytes =
      static_cast<std::uint64_t>(h.surface_count) * sizeof(ArchiveV2DirEntry);
  // Canonical metadata placement (spec §2): every byte before data_offset is covered by
  // the header CRC or the metadata CRC. Sums below cannot wrap: slots <= 2^32, entries
  // <= 2^24.
  if (h.lookup_offset != sizeof(ArchiveV2Header) ||
      h.directory_offset != h.lookup_offset + lookup_bytes) {
    return fail("metadata placement is not canonical");
  }
  if (h.directory_offset > h.file_size || dir_bytes > h.file_size - h.directory_offset) {
    return fail("directory out of bounds");
  }
  if (h.data_offset < h.directory_offset + dir_bytes || h.data_offset > h.file_size ||
      (h.data_offset % kArchiveV2ColumnAlign) != 0u) {
    return fail("data offset out of bounds / misaligned");
  }
  return Ok();
}

// A slot's own integrity and shape (spec §2.2): CRC, flag, the all-zero empty form, and
// for an occupied slot its symbol, hash and record extent.
[[nodiscard]] Status validate_slot_v2(const ArchiveV2LookupSlot &s, const ArchiveV2Header &h,
                                      std::string_view ctx) {
  const auto fail = [&](const char *what) {
    return Err(ErrorCode::ParseError, std::string(ctx) + ": " + what);
  };
  if (slot_crc_v2(s) != s.slot_crc32c) {
    return fail("lookup slot checksum mismatch");
  }
  if (s.flags == kArchiveV2SlotEmpty) {
    ArchiveV2LookupSlot zero{};
    zero.slot_crc32c = s.slot_crc32c;
    if (std::memcmp(&zero, &s, sizeof s) != 0) {
      return fail("empty lookup slot is not all-zero");
    }
    return Ok();
  }
  if (s.flags != kArchiveV2SlotOccupied) {
    return fail("lookup slot has unknown flags");
  }
  if (s.symbol_len == 0u || s.symbol_len > kArchiveSymbolMax || !symbol_tail_zero(s.symbol, s.symbol_len) ||
      !all_zero(s.reserved)) {
    return fail("lookup slot symbol malformed");
  }
  if (s.symbol_hash != archive_v2_symbol_hash(std::string_view(s.symbol, s.symbol_len))) {
    return fail("lookup slot hash does not match its symbol");
  }
  if (s.surface_offset < h.data_offset || (s.surface_offset % kArchiveV2ColumnAlign) != 0u ||
      s.surface_size < sizeof(ArchiveV2SurfaceHeader) || s.surface_offset > h.file_size ||
      s.surface_size > h.file_size - s.surface_offset) {
    return fail("lookup slot record extent out of bounds");
  }
  return Ok();
}

[[nodiscard]] bool kind_known(std::uint8_t k) noexcept {
  return k <= static_cast<std::uint8_t>(VolCurveKind::SplineVol);
}

[[nodiscard]] bool kind_is_array(VolCurveKind k) noexcept {
  return k == VolCurveKind::ConvexDense || k == VolCurveKind::LinearVariance ||
         k == VolCurveKind::SplineVol;
}

// Per-slice payload byte size (spec §3.2 table). `node_count` is u32, so every product
// is computed in u64 and cannot wrap.
[[nodiscard]] std::uint64_t v2_payload_size(VolCurveKind kind, std::uint32_t node_count) noexcept {
  const auto nc = static_cast<std::uint64_t>(node_count);
  switch (kind) {
  case VolCurveKind::ConvexDense:
    return 24ull + 16ull * nc;
  case VolCurveKind::Essvi:
    return sizeof(EssviParams);
  case VolCurveKind::Svi:
    return sizeof(SviParams);
  case VolCurveKind::LinearVariance:
    return 16ull * nc;
  case VolCurveKind::C8:
    return sizeof(C8Params);
  case VolCurveKind::SplineVol:
    // atm/z_lo/z_hi/n/pad (32) + z[n]+mult[n] (16n) + mult_cap+w_offset (16) + viol (4).
    return 52ull + 16ull * nc;
  }
  return 0;
}

template <class T> [[nodiscard]] T load_at(std::span<const std::byte> rec, std::uint64_t off) noexcept {
  T v{};
  std::memcpy(&v, rec.data() + off, sizeof v);
  return v;
}

// The writer's canonical record geometry (spec §3.2), recomputed from n_slices and the
// kind/node-count columns. Equality rules out every aliasing class (column over header,
// column over column, payload over column, payload over payload) and pins record_size
// to the exact payload end. nullptr when canonical, else the reason.
[[nodiscard]] const char *record_geometry_error(const ArchiveV2SurfaceHeader &h,
                                                std::span<const std::byte> rec) noexcept {
  constexpr std::uint64_t A = kArchiveV2ColumnAlign;
  const std::uint64_t n = h.n_slices;
  if (n == 0u || n > 0xFFFFu) {
    return "slice count out of range";
  }
  const std::uint64_t rs = rec.size();
  const std::uint64_t kind_off = sizeof(ArchiveV2SurfaceHeader);
  const std::uint64_t t_off = align_up(kind_off + n, A);
  const std::uint64_t node_off = t_off + 7ull * 8ull * n;
  const std::uint64_t poff_off = align_up(node_off + 4ull * n, A);
  if (h.col_kind_off != kind_off || h.col_T_off != t_off || h.col_forward_off != t_off + 8ull * n ||
      h.col_qeff_off != t_off + 16ull * n || h.col_df_off != t_off + 24ull * n ||
      h.col_borrow_off != t_off + 32ull * n || h.col_nused_off != t_off + 40ull * n ||
      h.col_ndropped_off != t_off + 48ull * n || h.col_nodecount_off != node_off ||
      h.col_payload_off_off != poff_off) {
    return "column geometry is not canonical";
  }
  const std::uint64_t columns_end = poff_off + 8ull * n;
  if (columns_end > rs) {
    return "columns exceed the record";
  }
  std::uint64_t end = columns_end; // end of the previous extent (columns, then payloads)
  std::uint16_t kinds = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    const auto kind_byte = load_at<std::uint8_t>(rec, kind_off + i);
    if (!kind_known(kind_byte)) {
      return "unknown curve kind";
    }
    const auto kind = static_cast<VolCurveKind>(kind_byte);
    const auto nc = load_at<std::uint32_t>(rec, node_off + 4ull * i);
    const auto poff = load_at<std::uint64_t>(rec, poff_off + 8ull * i);
    if (poff != align_up(end, A)) {
      return "payload offsets are not canonical";
    }
    if (kind_is_array(kind) ? nc == 0u : nc != 0u) {
      return "node count does not fit the curve kind";
    }
    const std::uint64_t need = v2_payload_size(kind, nc);
    if (poff > rs || need > rs - poff) {
      return "payload exceeds the record";
    }
    if (kind == VolCurveKind::SplineVol &&
        (load_at<std::uint32_t>(rec, poff + 24u) != nc || load_at<std::uint32_t>(rec, poff + 28u) != 0u)) {
      return "spline knot count / pad disagrees with the node column";
    }
    const auto df = load_at<double>(rec, h.col_df_off + 8ull * i);
    if (!(df > 0.0) || !std::isfinite(df)) {
      return "non-positive or non-finite discount factor";
    }
    kinds = static_cast<std::uint16_t>(kinds | (1u << kind_byte));
    end = poff + need;
  }
  if (h.kind_bits != kinds) {
    return "kind_bits disagrees with the kind column";
  }
  if (end != rs) {
    return "record size is not the canonical payload end";
  }
  return nullptr;
}

[[nodiscard]] bool attested_provenance_valid(std::uint8_t purpose, std::uint8_t quality,
                                             std::uint8_t state, std::uint32_t failures) noexcept {
  return purpose <= 1u && quality <= 2u && state <= 3u &&
         (failures & ~kKnownValidationFailures) == 0u &&
         (state != static_cast<std::uint8_t>(SurfaceState::Healthy) || failures == 0u);
}

// Provenance from a record header (spec §3.1 prov_status). ParseError for status 0 /
// unknown, an invalid attested block, or an unattested block carrying any value.
[[nodiscard]] Result<SurfaceProvenance> provenance_from_header(const ArchiveV2SurfaceHeader &h) {
  switch (h.prov_status) {
  case static_cast<std::uint8_t>(ArchiveV2ProvenanceStatus::Attested): {
    if (!attested_provenance_valid(h.prov_purpose, h.prov_quality_mode, h.prov_state,
                                   h.prov_validation_failures)) {
      return Err(ErrorCode::ParseError, "surface archive: invalid surface provenance record");
    }
    SurfaceProvenance p;
    p.purpose = static_cast<SurfacePurpose>(h.prov_purpose);
    p.quality_mode = static_cast<FitQualityMode>(h.prov_quality_mode);
    p.state = static_cast<SurfaceState>(h.prov_state);
    p.validation.failures = static_cast<ValidationFailure>(h.prov_validation_failures);
    p.validation.validation_id = h.prov_validation_id;
    p.source_generation = h.prov_source_generation;
    p.served_generation = h.prov_served_generation;
    p.legacy_format = false;
    return Ok(p);
  }
  case static_cast<std::uint8_t>(ArchiveV2ProvenanceStatus::Unattested):
    if (h.prov_purpose != 0u || h.prov_quality_mode != 0u || h.prov_state != 0u ||
        h.prov_validation_failures != 0u || h.prov_validation_id != 0u ||
        h.prov_source_generation != 0u || h.prov_served_generation != 0u) {
      return Err(ErrorCode::ParseError,
                 "surface archive: unattested provenance carries field values");
    }
    return Ok(legacy_surface_provenance());
  default:
    return Err(ErrorCode::ParseError, "surface archive: unknown provenance status");
  }
}

// Framing, binding, enums and geometry of one record extent (spec §3), WITHOUT the CRC
// recomputation (the caller decides: the point read always, whole-board lazily). `canon`
// is the symbol the slot / entry that led here names; `crc_copy` its CRC copy.
[[nodiscard]] Result<ArchiveV2SurfaceHeader> check_record_v2(std::span<const std::byte> rec,
                                                             std::string_view canon,
                                                             std::uint32_t crc_copy,
                                                             std::string_view ctx) {
  const auto fail = [&](const std::string &what) {
    return Err(ErrorCode::ParseError, std::string(ctx) + ": " + what);
  };
  if (rec.size() < sizeof(ArchiveV2SurfaceHeader)) {
    return fail("record smaller than its header");
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, rec.data(), sizeof h);
  if (std::memcmp(h.magic, kArchiveV2RecordMagic, sizeof kArchiveV2RecordMagic) != 0) {
    return fail("bad record magic");
  }
  if (h.record_size != rec.size()) {
    return fail("record size mismatch");
  }
  if (h.symbol_len != canon.size() || h.symbol_len > kArchiveSymbolMax ||
      std::memcmp(h.symbol, canon.data(), canon.size()) != 0 ||
      !symbol_tail_zero(h.symbol, h.symbol_len)) {
    return fail("record does not belong to symbol '" + std::string(canon) + "'");
  }
  if (h.payload_crc32c != crc_copy) {
    return fail("record checksum disagrees with its index copy");
  }
  if (!all_zero(h.reserved)) {
    return fail("non-zero reserved record bytes");
  }
  if (h.method > static_cast<std::uint8_t>(AmericanMethod::Baw)) {
    return fail("unknown pricing method");
  }
  if (auto prov = provenance_from_header(h); !prov) {
    return tl::unexpected<atx::core::Error>(std::move(prov).error());
  }
  if (const char *why = record_geometry_error(h, rec); why != nullptr) {
    return fail(why);
  }
  return Ok(h);
}

// Is `k` a legal `std::lower_bound` key? `LinearVarianceCurve::w` brackets the node axis
// with lower_bound and indexes at `lo = hi - 1`; a NaN node drives lower_bound to index
// 0 and underflows that subscript. Non-strict ascendance is lower_bound's precondition.
[[nodiscard]] bool linear_nodes_searchable(std::span<const double> k) noexcept {
  for (std::size_t i = 0; i < k.size(); ++i) {
    if (!std::isfinite(k[i]) || (i > 0 && k[i] < k[i - 1])) {
      return false;
    }
  }
  return true;
}

// Rebuild an OWNED PricedSurface from one record extent — the inverse of the writer.
// Every field is read via memcpy (alignment-safe for any base). The curve constructors,
// SliceContext and PricingContext are identical to the source's, so the result is bit
// for bit the source surface. Keeps its own bounds checks (callers pass checked records,
// but this function's contract does not depend on it).
[[nodiscard]] Result<PricedSurface> reconstruct_v2_record(std::span<const std::byte> record) {
  if (record.size() < sizeof(ArchiveV2SurfaceHeader)) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: record smaller than header");
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, record.data(), sizeof h);
  if (std::memcmp(h.magic, kArchiveV2RecordMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: bad record magic");
  }
  if (h.record_size != record.size()) {
    return Err(ErrorCode::ParseError, "reconstruct_v2: record size mismatch");
  }
  if (const char *why = record_geometry_error(h, record); why != nullptr) {
    return Err(ErrorCode::ParseError, std::string("reconstruct_v2: ") + why);
  }
  const std::uint64_t n = h.n_slices;
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
  pc.al_opts.n_quad_price = h.al_n_quad_price; // 0 -> tied
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
    // record_geometry_error proved poff + payload_size(kind, nc) <= record.size().
    const std::byte *p = base + poff;
    const std::size_t nb = static_cast<std::size_t>(nc) * sizeof(double);

    std::unique_ptr<IVolCurve> curve;
    switch (kind) {
    case VolCurveKind::Essvi: {
      EssviParams e{};
      std::memcpy(&e, p, sizeof e);
      // rho_R/rho_scale are retired reserved-zero wire fields; an armed blend can only
      // come from a hand-written payload. Refuse it rather than serve a NaN surface.
      if (essvi_rho_blend_armed(e)) {
        return Err(ErrorCode::ParseError,
                   "reconstruct_v2: essvi payload arms the retired asymmetric-rho blend");
      }
      curve = std::make_unique<EssviCurve>(e, df);
      break;
    }
    case VolCurveKind::Svi: {
      SviParams sv{};
      std::memcpy(&sv, p, sizeof sv);
      curve = std::make_unique<SviCurve>(sv, df);
      break;
    }
    case VolCurveKind::C8: {
      C8Params c8{};
      std::memcpy(&c8, p, sizeof c8);
      curve = std::make_unique<C8Curve>(c8, df);
      break;
    }
    case VolCurveKind::LinearVariance: {
      std::vector<double> k(nc);
      std::vector<double> w(nc);
      std::memcpy(k.data(), p, nb);
      std::memcpy(w.data(), p + nb, nb);
      if (!linear_nodes_searchable(k)) {
        return Err(ErrorCode::ParseError, "reconstruct_v2: linear node k's not ascending/finite");
      }
      curve = std::make_unique<LinearVarianceCurve>(T, fwd, df, std::move(k), std::move(w));
      break;
    }
    case VolCurveKind::ConvexDense: {
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
      // w_offset f64 | viol u32 (mult_cap + w_offset are LIVE in w()).
      SplineVolParams sp;
      std::memcpy(&sp.atm_vol, p + 0, 8);
      std::memcpy(&sp.z_lo_valid, p + 8, 8);
      std::memcpy(&sp.z_hi_valid, p + 16, 8);
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
    }
    if (curve == nullptr) {
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

// ── Writer plan ─────────────────────────────────────────────────────────────

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

// Lookup-table size for `n` surfaces at `load_pct` (spec §2.1): a power of two >= 8 that
// is > n (so every probe for an absent symbol meets an empty slot). Computed in u64:
// n <= 2^24 and load >= 1 keep `want` <= 100 * 2^24 < 2^31, so next_pow2 on u32 is in
// its defined range. Revision 0 truncated `want` to u32 and could hand std::bit_ceil a
// value above 2^31 — UB — for n >= ~1.5e9 surfaces.
[[nodiscard]] std::uint32_t lookup_slot_count_for(std::uint64_t n, std::uint64_t load_pct) noexcept {
  const std::uint64_t want = std::max((n * 100ull + load_pct - 1ull) / load_pct, n + 1ull);
  const std::uint32_t slots = atx::core::next_pow2(static_cast<std::uint32_t>(want));
  return std::max<std::uint32_t>(slots, 8u);
}

// Plan one item: validate it and compute its record geometry (spec §3.2).
[[nodiscard]] Result<V2SurfacePlan> plan_item(const SurfaceArchiveItem &it) {
  if (it.surface == nullptr) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: null surface");
  }
  if (it.symbol.empty() || it.symbol.size() > kArchiveSymbolMax) {
    return Err(ErrorCode::InvalidArgument,
               "write_surface_archive_v2: symbol must be 1.." + std::to_string(kArchiveSymbolMax) +
                   " bytes (no truncation)");
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
    if (!attested_provenance_valid(static_cast<std::uint8_t>(plan.provenance->purpose),
                                   static_cast<std::uint8_t>(plan.provenance->quality_mode),
                                   static_cast<std::uint8_t>(plan.provenance->state),
                                   static_cast<std::uint32_t>(plan.provenance->validation.failures))) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: invalid surface provenance");
    }
  }
  const std::string canon_sym = detail::canonicalize_symbol(it.symbol, kArchiveSymbolMax);
  plan.symbol_len = static_cast<std::uint16_t>(canon_sym.size());
  std::memcpy(plan.symbol.data(), canon_sym.data(), canon_sym.size());
  plan.symbol_hash = archive_v2_symbol_hash(canon_sym);
  plan.n_slices = static_cast<std::uint16_t>(n);
  plan.uid = ps.uid();

  constexpr std::uint64_t A = kArchiveV2ColumnAlign;
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
    std::size_t nc = 0;
    if (sp.kind == VolCurveKind::ConvexDense) {
      nc = static_cast<const ConvexDenseCurve *>(c)->fit().u.size();
    } else if (sp.kind == VolCurveKind::LinearVariance) {
      nc = static_cast<const LinearVarianceCurve *>(c)->k_nodes().size();
    } else if (sp.kind == VolCurveKind::SplineVol) {
      const auto *sv = static_cast<const SplineVolCurve *>(c);
      nc = sv->params().z.size();
      if (sv->params().n_butterfly_viol > std::numeric_limits<std::uint32_t>::max()) {
        return Err(ErrorCode::InvalidArgument,
                   "write_surface_archive_v2: butterfly violation count exceeds u32");
      }
    }
    if (nc > std::numeric_limits<std::uint32_t>::max()) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: node count exceeds u32");
    }
    sp.node_count = static_cast<std::uint32_t>(nc);
    if (kind_is_array(sp.kind) && sp.node_count == 0u) {
      return Err(ErrorCode::InvalidArgument,
                 "write_surface_archive_v2: an array curve slice has no nodes");
    }
    sp.payload_size = v2_payload_size(sp.kind, sp.node_count);
    sp.payload_off = align_up(cursor, A);
    cursor = sp.payload_off + sp.payload_size;
    plan.kind_bits |= static_cast<std::uint16_t>(1u << static_cast<unsigned>(sp.kind));
    plan.slices.push_back(sp);
  }
  plan.record_size = cursor;
  if (plan.record_size > kArchiveV2MaxRecordBytes) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: record exceeds 4 GiB");
  }
  return Ok(std::move(plan));
}

// Materialize one planned record at `base` and return its payload CRC.
[[nodiscard]] std::uint32_t emit_record(const V2SurfacePlan &plan, std::byte *base) {
  const PricingContext &pc = plan.surf->pricing();

  ArchiveV2SurfaceHeader sh{};
  std::memcpy(sh.magic, kArchiveV2RecordMagic, 8);
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
  sh.symbol_len = plan.symbol_len;
  std::memcpy(sh.symbol, plan.symbol.data(), plan.symbol_len);
  sh.method = static_cast<std::uint8_t>(pc.method);
  sh.al_n_collocation = pc.al_opts.n_collocation;
  sh.al_n_quadrature = pc.al_opts.n_quadrature;
  sh.al_max_newton_iter = pc.al_opts.max_newton_iter;
  sh.al_n_quad_price = pc.al_opts.n_quad_price;
  if (plan.provenance.has_value()) {
    const SurfaceProvenance &p = *plan.provenance;
    sh.prov_status = static_cast<std::uint8_t>(ArchiveV2ProvenanceStatus::Attested);
    sh.prov_purpose = static_cast<std::uint8_t>(p.purpose);
    sh.prov_quality_mode = static_cast<std::uint8_t>(p.quality_mode);
    sh.prov_state = static_cast<std::uint8_t>(p.state);
    sh.prov_validation_failures = static_cast<std::uint32_t>(p.validation.failures);
    sh.prov_validation_id = p.validation.validation_id;
    sh.prov_source_generation = p.source_generation;
    sh.prov_served_generation = p.served_generation;
  } else {
    sh.prov_status = static_cast<std::uint8_t>(ArchiveV2ProvenanceStatus::Unattested);
  }
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

  // Variable payloads (spec §3.2 table; unchanged from revision 0, bit-exact).
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
    // The three POD payloads are blitted whole and their PADDING re-zeroed: the pad
    // bytes are never written by any fitter, so a verbatim blit would store thread
    // stack residue in the record and its CRC (slice_payload_padding.hpp).
    case VolCurveKind::Essvi: {
      const EssviParams &e = static_cast<const EssviCurve *>(sp.curve)->slice();
      std::memcpy(p, &e, sizeof e);
      detail::normalize_essvi_payload_padding(p);
      break;
    }
    case VolCurveKind::Svi: {
      const SviParams &vv = static_cast<const SviCurve *>(sp.curve)->slice();
      std::memcpy(p, &vv, sizeof vv);
      detail::normalize_svi_payload_padding(p);
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
      detail::normalize_c8_payload_padding(p);
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
      // mult_cap + w_offset are LIVE terms of SplineVolCurve::w(); the view is bit-exact
      // only because both are stored.
      std::memcpy(p + 32 + 2 * nb, &sv.mult_cap, 8);
      std::memcpy(p + 40 + 2 * nb, &sv.w_offset, 8);
      std::memcpy(p + 48 + 2 * nb, &viol, 4);
      break;
    }
    }
  }

  const std::uint32_t crc = record_crc_v2(base, plan.record_size);
  std::memcpy(base + offsetof(ArchiveV2SurfaceHeader, payload_crc32c), &crc, sizeof crc);
  return crc;
}

[[nodiscard]] std::byte *buf_at(std::vector<std::byte> &b, std::uint64_t off) noexcept {
  return b.data() + static_cast<std::size_t>(off);
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
// Writer
// ═══════════════════════════════════════════════════════════════════════════

Result<std::vector<std::byte>>
write_surface_archive_v2(std::span<const SurfaceArchiveItem> items,
                         const ArchiveV2WriteOpts &opts) {
  if (items.empty()) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: no items");
  }
  if (items.size() > kArchiveV2MaxSurfaces) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive_v2: too many items");
  }
  if (opts.lookup_load_pct == 0 || opts.lookup_load_pct > 100) {
    return Err(ErrorCode::InvalidArgument,
               "write_surface_archive_v2: lookup_load_pct must be in (0, 100]");
  }
  const std::uint64_t surf_align =
      opts.surface_alignment != 0 ? opts.surface_alignment : kArchiveV2SurfaceAlign;
  // surf_align flows into align_up (power of two) and must not be finer than the
  // column alignment, else record offsets silently corrupt.
  if (!atx::core::is_pow2(static_cast<std::uint32_t>(surf_align)) ||
      surf_align < kArchiveV2ColumnAlign) {
    return Err(ErrorCode::InvalidArgument,
               "write_surface_archive_v2: surface_alignment must be a power of two >= 8");
  }
  const auto n_items = static_cast<std::uint32_t>(items.size());

  // 1. Plan + validate.
  std::vector<V2SurfacePlan> plans;
  plans.reserve(n_items);
  for (const SurfaceArchiveItem &it : items) {
    auto plan = plan_item(it);
    if (!plan) {
      return tl::unexpected<atx::core::Error>(std::move(plan).error());
    }
    plans.push_back(std::move(*plan));
  }

  // 2. Deterministic order by canonical symbol (input order never reaches the bytes).
  std::sort(plans.begin(), plans.end(), v2_plan_less);

  // 3. File geometry (canonical metadata placement, spec §2).
  const std::uint32_t lookup_slots = lookup_slot_count_for(n_items, opts.lookup_load_pct);
  const std::uint64_t lookup_offset = sizeof(ArchiveV2Header);
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
  const std::uint64_t file_size = align_up(cursor, kArchiveV2FilePageAlign);

  // 4. Lookup placement (linear probing in sorted order -> deterministic) + directory.
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
        s.surface_size = static_cast<std::uint32_t>(plan.record_size);
        s.symbol_len = static_cast<std::uint8_t>(plan.symbol_len);
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

  // 5. Records; mirror each record CRC into its slot and its directory entry so the
  // metadata CRC (content identity) sees any record rewrite.
  std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));
  for (std::size_t idx = 0; idx < plans.size(); ++idx) {
    const V2SurfacePlan &plan = plans[idx];
    const std::uint32_t crc = emit_record(plan, buf_at(buffer, plan.file_offset));
    directory[idx].payload_crc32c = crc;
    lookup[plan.slot_index].payload_crc32c = crc;
  }
  for (ArchiveV2LookupSlot &s : lookup) {
    s.slot_crc32c = slot_crc_v2(s);
  }
  std::memcpy(buf_at(buffer, lookup_offset), lookup.data(), static_cast<std::size_t>(lookup_bytes));
  if (dir_bytes > 0) {
    std::memcpy(buf_at(buffer, directory_offset), directory.data(),
                static_cast<std::size_t>(dir_bytes));
  }
  const std::uint32_t meta =
      crc32c(buf_at(buffer, lookup_offset), static_cast<std::size_t>(lookup_bytes + dir_bytes));

  // 6. Header. created_ts_ns: explicit nonzero honored verbatim; the 0 sentinel is
  // filled from a deterministic CRC of [header, EOF) folded with file_size (spec §8).
  ArchiveV2Header hdr{};
  std::memcpy(hdr.magic, kArchiveV2Magic, 8);
  hdr.file_size = file_size;
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
  auto reserved = detail::reserve_unique_publish_temp_file(path);
  if (!reserved) {
    return tl::unexpected<atx::core::Error>(std::move(reserved).error());
  }
  const std::filesystem::path tmp{*reserved};
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
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
  // Durable atomic publish: fsync the temp, rename with bounded retry under a
  // reader-held destination, preserve the temp on final failure (spec §9).
  return detail::flush_and_publish_file(tmp.string(), dst.string());
}

// ═══════════════════════════════════════════════════════════════════════════
// Whole-board reader
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Strict order the writer sorts the directory in (`v2_plan_less`) and that `open_impl`
// proves holds. Heterogeneous so `std::lower_bound` can probe with a bare symbol.
[[nodiscard]] bool dir_symbol_less(const ArchiveV2DirEntry &e, std::string_view sym) noexcept {
  const std::size_t n = std::min(static_cast<std::size_t>(e.symbol_len), sym.size());
  if (n != 0) {
    const int c = std::memcmp(e.symbol, sym.data(), n);
    if (c != 0) {
      return c < 0;
    }
  }
  return static_cast<std::size_t>(e.symbol_len) < sym.size();
}

[[nodiscard]] std::string_view entry_symbol(const ArchiveV2DirEntry &e) noexcept {
  return {e.symbol, std::min<std::size_t>(e.symbol_len, kArchiveSymbolMax)};
}

} // namespace

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_impl(std::span<const std::byte> bytes,
                                                     std::shared_ptr<const void> owner) {
  constexpr std::string_view ctx = "SurfaceArchiveV2::open";
  if (bytes.size() < sizeof(ArchiveV2Header)) {
    if (bytes.size() >= 8u) {
      char magic[8];
      std::memcpy(magic, bytes.data(), 8);
      if (Status m = check_file_magic(magic, ctx); !m) {
        return tl::unexpected<atx::core::Error>(std::move(m).error());
      }
    }
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: shorter than header");
  }
  ArchiveV2Header h;
  std::memcpy(&h, bytes.data(), sizeof h);
  if (Status hs = validate_header_v2(h, bytes.size(), ctx); !hs) {
    return tl::unexpected<atx::core::Error>(std::move(hs).error());
  }
  const std::uint64_t lookup_bytes =
      static_cast<std::uint64_t>(h.lookup_slot_count) * sizeof(ArchiveV2LookupSlot);
  const std::uint64_t dir_bytes =
      static_cast<std::uint64_t>(h.surface_count) * sizeof(ArchiveV2DirEntry);
  // lookup ‖ directory is one contiguous span (canonical placement).
  if (crc32c(bytes.data() + h.lookup_offset, static_cast<std::size_t>(lookup_bytes + dir_bytes)) !=
      h.metadata_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: metadata checksum mismatch");
  }

  SurfaceArchiveV2 a;
  a.bytes_ = bytes;
  a.owner_ = std::move(owner);
  a.header_ = h;
  a.lookup_.resize(h.lookup_slot_count);
  std::memcpy(a.lookup_.data(), bytes.data() + h.lookup_offset,
              static_cast<std::size_t>(lookup_bytes));
  a.directory_.resize(h.surface_count);
  if (dir_bytes > 0) {
    std::memcpy(a.directory_.data(), bytes.data() + h.directory_offset,
                static_cast<std::size_t>(dir_bytes));
  }

  std::uint32_t occupied = 0;
  for (const ArchiveV2LookupSlot &slot : a.lookup_) {
    if (Status s = validate_slot_v2(slot, h, ctx); !s) {
      return tl::unexpected<atx::core::Error>(std::move(s).error());
    }
    occupied += slot.flags == kArchiveV2SlotOccupied ? 1u : 0u;
  }
  if (occupied != h.surface_count) {
    return Err(ErrorCode::ParseError,
               "SurfaceArchiveV2::open: occupied lookup slots != surface count");
  }
  for (const ArchiveV2DirEntry &de : a.directory_) {
    if (de.symbol_len == 0u || de.symbol_len > kArchiveSymbolMax ||
        !symbol_tail_zero(de.symbol, de.symbol_len) || de.flags != 0u) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: directory entry malformed");
    }
    if (de.symbol_hash != archive_v2_symbol_hash(entry_symbol(de))) {
      return Err(ErrorCode::ParseError,
                 "SurfaceArchiveV2::open: directory hash does not match its symbol");
    }
    if (de.surface_offset < h.data_offset || (de.surface_offset % kArchiveV2ColumnAlign) != 0u ||
        de.surface_size < sizeof(ArchiveV2SurfaceHeader) || de.surface_offset > h.file_size ||
        de.surface_size > h.file_size - de.surface_offset || de.n_slices == 0u) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: directory entry out of bounds");
    }
  }
  // (a) strictly ascending (hence unique) directory symbols; (b) occupied == count above;
  // (c) every entry resolves through the lookup to a slot that agrees on offset, size,
  // CRC copy and hash. (a) makes (c) injective, (b) makes it onto: a bijection, so no
  // occupied slot escapes validation (rev-ws-c I-1).
  for (std::size_t i = 1; i < a.directory_.size(); ++i) {
    if (!dir_symbol_less(a.directory_[i - 1], entry_symbol(a.directory_[i]))) {
      return Err(ErrorCode::ParseError,
                 "SurfaceArchiveV2::open: directory symbols not strictly ascending");
    }
  }
  for (const ArchiveV2DirEntry &de : a.directory_) {
    const ArchiveV2LookupSlot *s = a.find_slot(entry_symbol(de));
    if (s == nullptr || s->surface_offset != de.surface_offset ||
        s->surface_size != de.surface_size || s->payload_crc32c != de.payload_crc32c ||
        s->symbol_hash != de.symbol_hash) {
      return Err(ErrorCode::ParseError, "SurfaceArchiveV2::open: lookup/directory disagreement");
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

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_copied(std::string_view path) {
  // ONE positional read of the whole file into an owned, 16-B aligned buffer: no
  // ifstream (revision 0's open_file streamed it in ~4 KB chunks, ~2.8k syscalls for an
  // 11.6 MB partition), no mapping (nothing outlives the call against the file), and
  // the handle shares delete so a concurrent republish is never refused.
  std::shared_ptr<std::byte[]> owned;
  std::size_t size = 0;
  {
    auto file = detail::PositionalFile::open(path);
    if (!file) {
      return tl::unexpected<atx::core::Error>(std::move(file).error());
    }
    if (file->size() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return Err(ErrorCode::IoError, "SurfaceArchiveV2::open_copied: file too large to buffer");
    }
    size = static_cast<std::size_t>(file->size());
    owned = std::make_unique_for_overwrite<std::byte[]>(size);
    if (Status st = file->read_at(0, owned.get(), size); !st) {
      return tl::unexpected<atx::core::Error>(std::move(st).error());
    }
  } // handle closed here
  const std::span<const std::byte> span{owned.get(), size};
  return open_impl(span, std::shared_ptr<const void>(std::move(owned)));
}

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_file(std::string_view path) {
  return open_copied(path);
}

Result<SurfaceArchiveV2> SurfaceArchiveV2::open_mapped(std::string_view path) {
  // Map read-only and keep the mapping for the archive's lifetime; records fault in only
  // when a reader touches them. No whole-mapping prefetch: a subset load would fault in
  // an order of magnitude more pages than it reads (S6-T29). On Windows the live
  // mapping blocks a republish of this file — use only for read-only corpora.
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
  if (lookup_.empty() || symbol.empty() || symbol.size() > kArchiveSymbolMax) {
    return nullptr;
  }
  const std::string canon = detail::canonicalize_symbol(symbol, kArchiveSymbolMax);
  const std::uint64_t hh = archive_v2_symbol_hash(canon);
  const std::uint64_t mask = static_cast<std::uint64_t>(lookup_.size()) - 1ull;
  std::uint64_t i = hh & mask;
  for (std::size_t step = 0; step < lookup_.size(); ++step) {
    const ArchiveV2LookupSlot &s = lookup_[static_cast<std::size_t>(i)];
    if (s.flags == kArchiveV2SlotEmpty) {
      return nullptr;
    }
    if (s.symbol_hash == hh && s.symbol_len == canon.size() &&
        std::memcmp(s.symbol, canon.data(), canon.size()) == 0) {
      return &s;
    }
    i = (i + 1ull) & mask;
  }
  return nullptr;
}

const ArchiveV2DirEntry *SurfaceArchiveV2::entry_for_slot(const ArchiveV2LookupSlot &s) const noexcept {
  const std::string_view canon(s.symbol, s.symbol_len);
  const auto it = std::lower_bound(directory_.begin(), directory_.end(), canon, dir_symbol_less);
  if (it == directory_.end() || entry_symbol(*it) != canon) {
    return nullptr; // unreachable on an opened archive: open_impl proved the bijection
  }
  return &*it;
}

Result<std::span<const std::byte>> SurfaceArchiveV2::checked_record(const ArchiveV2DirEntry &e) const {
  constexpr std::string_view ctx = "SurfaceArchiveV2: record";
  if (e.surface_size < sizeof(ArchiveV2SurfaceHeader) || e.surface_offset > bytes_.size() ||
      e.surface_size > bytes_.size() - e.surface_offset || e.symbol_len > kArchiveSymbolMax) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2: record out of bounds");
  }
  const std::span<const std::byte> rec = bytes_.subspan(static_cast<std::size_t>(e.surface_offset),
                                                        static_cast<std::size_t>(e.surface_size));
  auto h = check_record_v2(rec, entry_symbol(e), e.payload_crc32c, ctx);
  if (!h) {
    return tl::unexpected<atx::core::Error>(std::move(h).error());
  }
  if (h->n_slices != e.n_slices || h->kind_bits != e.kind_bits || h->uid != e.uid) {
    return Err(ErrorCode::ParseError,
               "SurfaceArchiveV2: directory/record n_slices, kind_bits or uid mismatch");
  }
  return Ok(rec);
}

Result<ArchiveV2DirEntry> SurfaceArchiveV2::find(std::string_view symbol) const {
  const ArchiveV2LookupSlot *s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchiveV2::find: symbol not present");
  }
  const ArchiveV2DirEntry *e = entry_for_slot(*s);
  if (e == nullptr) {
    return Err(ErrorCode::Internal, "SurfaceArchiveV2::find: lookup slot has no directory entry");
  }
  return *e;
}

Result<SurfaceProvenance> SurfaceArchiveV2::provenance(std::string_view symbol) const {
  auto e = find(symbol);
  if (!e) {
    return tl::unexpected<atx::core::Error>(std::move(e).error());
  }
  auto rec = checked_record(*e);
  if (!rec) {
    return tl::unexpected<atx::core::Error>(std::move(rec).error());
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, rec->data(), sizeof h);
  return provenance_from_header(h);
}

Result<PricedSurfaceView> SurfaceArchiveV2::map_symbol(std::string_view symbol) const {
  auto e = find(symbol);
  if (!e) {
    return tl::unexpected<atx::core::Error>(std::move(e).error());
  }
  auto rec = checked_record(*e);
  if (!rec) {
    return tl::unexpected<atx::core::Error>(std::move(rec).error());
  }
  // SUBSET MAP: the view borrows ONLY this record's extent; no sibling bytes read.
  return PricedSurfaceView::create_over_record(*rec);
}

Result<std::vector<PricedSurfaceView>> SurfaceArchiveV2::map_all() const {
  std::vector<PricedSurfaceView> out;
  out.reserve(directory_.size());
  for (const ArchiveV2DirEntry &de : directory_) {
    auto rec = checked_record(de);
    if (!rec) {
      return tl::unexpected<atx::core::Error>(std::move(rec).error());
    }
    auto v = PricedSurfaceView::create_over_record(*rec);
    if (!v) {
      return tl::unexpected<atx::core::Error>(std::move(v).error());
    }
    out.push_back(std::move(*v));
  }
  return Ok(std::move(out));
}

Result<ArchivedSurfaceView> SurfaceArchiveV2::map_entry(const ArchiveV2DirEntry &e) const {
  auto rec = checked_record(e);
  if (!rec) {
    return tl::unexpected<atx::core::Error>(std::move(rec).error());
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, rec->data(), sizeof h);
  auto prov = provenance_from_header(h);
  if (!prov) {
    return tl::unexpected<atx::core::Error>(std::move(prov).error());
  }
  auto v = PricedSurfaceView::create_over_record(*rec);
  if (!v) {
    return tl::unexpected<atx::core::Error>(std::move(v).error());
  }
  return Ok(ArchivedSurfaceView{std::move(*v), std::move(*prov)});
}

Result<std::vector<ArchivedSurfaceView>> SurfaceArchiveV2::map_all_with_provenance() const {
  std::vector<ArchivedSurfaceView> out;
  out.reserve(directory_.size());
  for (const ArchiveV2DirEntry &de : directory_) {
    auto one = map_entry(de);
    if (!one) {
      return tl::unexpected<atx::core::Error>(std::move(one).error());
    }
    out.push_back(std::move(*one));
  }
  return Ok(std::move(out));
}

Result<PricedSurface> SurfaceArchiveV2::reconstruct_symbol(std::string_view symbol) const {
  auto e = find(symbol);
  if (!e) {
    return tl::unexpected<atx::core::Error>(std::move(e).error());
  }
  auto rec = checked_record(*e);
  if (!rec) {
    return tl::unexpected<atx::core::Error>(std::move(rec).error());
  }
  return reconstruct_v2_record(*rec);
}

Result<ArchivedSurface> SurfaceArchiveV2::reconstruct_entry(const ArchiveV2DirEntry &e) const {
  auto rec = checked_record(e);
  if (!rec) {
    return tl::unexpected<atx::core::Error>(std::move(rec).error());
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, rec->data(), sizeof h);
  auto prov = provenance_from_header(h);
  if (!prov) {
    return tl::unexpected<atx::core::Error>(std::move(prov).error());
  }
  auto ps = reconstruct_v2_record(*rec);
  if (!ps) {
    return tl::unexpected<atx::core::Error>(std::move(ps).error());
  }
  return Ok(ArchivedSurface{std::move(*ps), std::move(*prov)});
}

Result<std::vector<PricedSurface>> SurfaceArchiveV2::reconstruct_all() const {
  std::vector<PricedSurface> out;
  out.reserve(directory_.size());
  for (const ArchiveV2DirEntry &de : directory_) {
    auto rec = checked_record(de);
    if (!rec) {
      return tl::unexpected<atx::core::Error>(std::move(rec).error());
    }
    auto ps = reconstruct_v2_record(*rec);
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
    auto one = reconstruct_entry(de);
    if (!one) {
      return tl::unexpected<atx::core::Error>(std::move(one).error());
    }
    out.push_back(std::move(*one));
  }
  return Ok(std::move(out));
}

Status SurfaceArchiveV2::validate_record_crc(const ArchiveV2DirEntry &e) const {
  if (e.surface_size < sizeof(ArchiveV2SurfaceHeader) || e.surface_offset > bytes_.size() ||
      e.surface_size > bytes_.size() - e.surface_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::validate: record out of bounds");
  }
  const std::byte *base = bytes_.data() + e.surface_offset;
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, base, sizeof h);
  if (std::memcmp(h.magic, kArchiveV2RecordMagic, 8) != 0 || h.record_size != e.surface_size) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::validate: bad record framing");
  }
  if (record_crc_v2(base, e.surface_size) != h.payload_crc32c ||
      h.payload_crc32c != e.payload_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchiveV2::validate: record checksum mismatch");
  }
  auto rec = checked_record(e);
  if (!rec) {
    return tl::unexpected<atx::core::Error>(std::move(rec).error());
  }
  return Ok();
}

Status SurfaceArchiveV2::validate_symbol(std::string_view symbol) const {
  auto e = find(symbol);
  if (!e) {
    return tl::unexpected<atx::core::Error>(std::move(e).error());
  }
  return validate_record_crc(*e);
}

Status SurfaceArchiveV2::validate_all() const {
  for (const ArchiveV2DirEntry &de : directory_) {
    if (Status st = validate_record_crc(de); !st) {
      return st;
    }
  }
  return Ok();
}

// ═══════════════════════════════════════════════════════════════════════════
// Point read (spec §4)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

struct PointReadRecord {
  std::unique_ptr<std::byte[]> bytes;
  std::size_t size{0};
  ArchiveV2SurfaceHeader header{};
  ArchivePointReadStats stats{};
};

[[nodiscard]] Result<PointReadRecord> point_read_record(std::string_view path,
                                                        std::string_view symbol) {
  constexpr std::string_view ctx = "point_read_archive";
  if (symbol.empty() || symbol.size() > kArchiveSymbolMax) {
    return Err(ErrorCode::InvalidArgument, "point_read_archive: symbol must be 1.." +
                                               std::to_string(kArchiveSymbolMax) + " bytes");
  }
  const std::string canon = detail::canonicalize_symbol(symbol, kArchiveSymbolMax);

  auto file = detail::PositionalFile::open(path);
  if (!file) {
    return tl::unexpected<atx::core::Error>(std::move(file).error());
  }
  PointReadRecord out;
  const auto finish_stats = [&]() {
    out.stats.bytes_read = file->bytes_read();
    out.stats.read_calls = file->read_calls();
  };

  // Step 3: the header.
  const std::uint64_t real_size = file->size();
  ArchiveV2Header h{};
  const std::size_t head = static_cast<std::size_t>(std::min<std::uint64_t>(real_size, sizeof h));
  if (Status st = file->read_at(0, reinterpret_cast<std::byte *>(&h), head); !st) {
    return tl::unexpected<atx::core::Error>(std::move(st).error());
  }
  if (head < sizeof h) {
    if (head >= 8u) {
      if (Status m = check_file_magic(h.magic, ctx); !m) {
        return tl::unexpected<atx::core::Error>(std::move(m).error());
      }
    }
    return Err(ErrorCode::ParseError, "point_read_archive: shorter than header");
  }
  if (Status st = validate_header_v2(h, real_size, ctx); !st) {
    return tl::unexpected<atx::core::Error>(std::move(st).error());
  }

  // Step 4: probe, reading slots in runs that never wrap inside one read.
  const std::uint64_t hash = archive_v2_symbol_hash(canon);
  const std::uint64_t slots = h.lookup_slot_count;
  const std::uint64_t mask = slots - 1u;
  std::array<ArchiveV2LookupSlot, kArchiveV2SlotReadRun> run{};
  std::uint64_t run_start = 0;
  std::uint64_t run_len = 0;
  std::uint64_t i = hash & mask;
  std::optional<ArchiveV2LookupSlot> hit;
  for (std::uint64_t step = 0; step < slots; ++step) {
    if (i < run_start || i >= run_start + run_len) {
      run_start = i;
      run_len = std::min<std::uint64_t>(kArchiveV2SlotReadRun, slots - i);
      if (Status st = file->read_at(h.lookup_offset + i * sizeof(ArchiveV2LookupSlot),
                                    reinterpret_cast<std::byte *>(run.data()),
                                    static_cast<std::size_t>(run_len) * sizeof(ArchiveV2LookupSlot));
          !st) {
        return tl::unexpected<atx::core::Error>(std::move(st).error());
      }
    }
    const ArchiveV2LookupSlot &s = run[static_cast<std::size_t>(i - run_start)];
    ++out.stats.slots_probed;
    if (Status st = validate_slot_v2(s, h, ctx); !st) {
      return tl::unexpected<atx::core::Error>(std::move(st).error());
    }
    if (s.flags == kArchiveV2SlotEmpty) {
      break;
    }
    if (s.symbol_hash == hash && s.symbol_len == canon.size() &&
        std::memcmp(s.symbol, canon.data(), canon.size()) == 0) {
      hit = s;
      break;
    }
    i = (i + 1u) & mask;
  }
  if (!hit) {
    finish_stats();
    return Err(ErrorCode::NotFound, "point_read_archive: symbol not present");
  }

  // Steps 5-6: the record extent (bounds proven by validate_slot_v2), CRC first, then
  // binding, framing, enums and canonical geometry.
  out.size = hit->surface_size;
  out.bytes = std::make_unique_for_overwrite<std::byte[]>(out.size);
  if (Status st = file->read_at(hit->surface_offset, out.bytes.get(), out.size); !st) {
    return tl::unexpected<atx::core::Error>(std::move(st).error());
  }
  finish_stats();
  const std::span<const std::byte> rec{out.bytes.get(), out.size};
  ArchiveV2SurfaceHeader stored{};
  std::memcpy(&stored, rec.data(), sizeof stored);
  if (record_crc_v2(rec.data(), rec.size()) != stored.payload_crc32c) {
    return Err(ErrorCode::ParseError, "point_read_archive: record checksum mismatch");
  }
  auto checked = check_record_v2(rec, canon, hit->payload_crc32c, ctx);
  if (!checked) {
    return tl::unexpected<atx::core::Error>(std::move(checked).error());
  }
  out.header = *checked;
  return Ok(std::move(out));
}

} // namespace

Result<PointReadSurface> point_read_archive_surface(std::string_view path,
                                                    std::string_view symbol) {
  auto rec = point_read_record(path, symbol);
  if (!rec) {
    return tl::unexpected<atx::core::Error>(std::move(rec).error());
  }
  auto prov = provenance_from_header(rec->header);
  if (!prov) {
    return tl::unexpected<atx::core::Error>(std::move(prov).error());
  }
  std::shared_ptr<std::byte[]> owned(std::move(rec->bytes));
  auto view = PricedSurfaceView::create_over_record(std::span<const std::byte>{owned.get(), rec->size});
  if (!view) {
    return tl::unexpected<atx::core::Error>(std::move(view).error());
  }
  return Ok(PointReadSurface{std::shared_ptr<const void>(std::move(owned)), std::move(*view),
                             std::move(*prov), rec->stats});
}

Result<std::int64_t> detail::read_archive_v2_first_now_ts(std::string_view path) {
  constexpr std::string_view ctx = "SurfaceDb::session_ts";
  auto file = detail::PositionalFile::open(path);
  if (!file) {
    return tl::unexpected<atx::core::Error>(std::move(file).error());
  }
  const std::uint64_t real_size = file->size();
  if (real_size < sizeof(ArchiveV2Header)) {
    return Err(ErrorCode::ParseError, "SurfaceDb::session_ts: shorter than archive header");
  }
  ArchiveV2Header h{};
  if (Status st = file->read_at(0, reinterpret_cast<std::byte *>(&h), sizeof h); !st) {
    return tl::unexpected<atx::core::Error>(std::move(st).error());
  }
  if (Status st = validate_header_v2(h, real_size, ctx); !st) {
    return tl::unexpected<atx::core::Error>(std::move(st).error());
  }
  if (h.surface_count == 0u) {
    return Err(ErrorCode::ParseError, "SurfaceDb::session_ts: partition has no surfaces");
  }
  ArchiveV2DirEntry e{};
  if (Status st = file->read_at(h.directory_offset, reinterpret_cast<std::byte *>(&e), sizeof e);
      !st) {
    return tl::unexpected<atx::core::Error>(std::move(st).error());
  }
  if (e.symbol_len == 0u || e.symbol_len > kArchiveSymbolMax || e.surface_offset < h.data_offset ||
      (e.surface_offset % kArchiveV2ColumnAlign) != 0u ||
      e.surface_size < sizeof(ArchiveV2SurfaceHeader) || e.surface_offset > real_size ||
      e.surface_size > real_size - e.surface_offset) {
    return Err(ErrorCode::ParseError, "SurfaceDb::session_ts: surface record out of bounds");
  }
  ArchiveV2SurfaceHeader r{};
  if (Status st = file->read_at(e.surface_offset, reinterpret_cast<std::byte *>(&r), sizeof r);
      !st) {
    return tl::unexpected<atx::core::Error>(std::move(st).error());
  }
  if (std::memcmp(r.magic, kArchiveV2RecordMagic, sizeof kArchiveV2RecordMagic) != 0 ||
      r.record_size != e.surface_size || r.n_slices != e.n_slices ||
      r.symbol_len != e.symbol_len || std::memcmp(r.symbol, e.symbol, e.symbol_len) != 0) {
    return Err(ErrorCode::ParseError, "SurfaceDb::session_ts: invalid surface record header");
  }
  return Ok(r.now_ts_ns);
}

Result<PointReadOwnedSurface> point_read_archive_reconstruct(std::string_view path,
                                                             std::string_view symbol) {
  auto rec = point_read_record(path, symbol);
  if (!rec) {
    return tl::unexpected<atx::core::Error>(std::move(rec).error());
  }
  auto prov = provenance_from_header(rec->header);
  if (!prov) {
    return tl::unexpected<atx::core::Error>(std::move(prov).error());
  }
  auto ps = reconstruct_v2_record(std::span<const std::byte>{rec->bytes.get(), rec->size});
  if (!ps) {
    return tl::unexpected<atx::core::Error>(std::move(ps).error());
  }
  return Ok(PointReadOwnedSurface{ArchivedSurface{std::move(*ps), std::move(*prov)}, rec->stats});
}

} // namespace atx::vol
