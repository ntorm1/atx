#pragma once

// atx::engine::library — on-disk library-segment record schema (S4-1).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  THE wire contract for one immutable library segment: the POD framing
//  (SegmentHeader / AlphaDirEntry / SegmentFooter), the variable-length
//  Provenance (de)serialization, a one-pass sealed-segment WRITER, and a
//  read-only attach path (SegmentReaderLite) that validates magic / version /
//  seal / integrity-crc BEFORE exposing a single byte.
//
//  The framing DISCIPLINE is cloned from atx::tsdb's segment format (tag8 magic,
//  format_version + is_supported_version guard, sections addressed as byte
//  OFFSETS from base(), a SEALED footer marker, an integrity crc over
//  [header..footer), and validate-before-expose at attach). The bar-grid LAYOUT
//  is NOT reused — the data sections here are AlphaDirEntry + alpha-major PnL +
//  alpha->period->instrument positions + a concatenated Provenance blob.
//
// ===========================================================================
//  File layout — one contiguous file; every ref is a byte OFFSET from base().
// ===========================================================================
//    SegmentHeader | AlphaDirectory | PnlBlock | PosBlock | ProvenanceBlob | SegmentFooter
//
//  * AlphaDirectory: n_alphas x AlphaDirEntry (fixed-width => O(1) addressable).
//  * PnlBlock:  n_alphas * n_periods f64, ALPHA-MAJOR (a*T + t) — mirrors
//               combine::AlphaStore::pnl_.
//  * PosBlock:  n_alphas * n_periods * n_instruments f64, alpha->period->
//               instrument — mirrors combine::AlphaStore::pos_.
//  * ProvenanceBlob: concatenated serialized Provenance records (var-length);
//               each AlphaDirEntry carries {prov_off, prov_len} into this blob.
//
//  NaN cells are stored VERBATIM (bit-identical round-trip — the f64 grids are
//  memcpy'd, never coerced). Endianness: the f64/integer fields are written/read
//  in native byte order. Both supported targets are little-endian x86_64; the
//  static_assert below pins that assumption so a big-endian port fails loudly
//  rather than silently corrupting (matching atx::tsdb's discipline).

// V2 replaces dense positions with bounded int16 sketches and f64 PnL with f32.
// See AlphaMetadata/write_compressed_segment_bytes below. V1 writer bytes remain
// unchanged; version-specific readers validate geometry before exposing data.

#include <algorithm>
#include <cmath>
#include <limits>
#include <array>
#include <bit>     // std::bit_cast, std::endian
#include <cstring> // std::memcpy
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility> // std::move
#include <vector>

#include "atx/core/macro.hpp"
#include "atx/core/error.hpp" // Result, Err, ErrorCode
#include "atx/core/types.hpp" // u8, u16, u32, u64, f64, usize, byte

#include "atx/tsdb/checksum.hpp" // tsdb::crc32
#include "atx/tsdb/mapping.hpp"  // tsdb::Mapping (read-only mmap RAII)
#include "atx/tsdb/segment.hpp"  // tsdb::tag8 (consteval 8-char tag -> u64)

#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics

namespace atx::engine::library {

static_assert(std::endian::native == std::endian::little,
              "atx-engine library segment format assumes a little-endian target");

// ===========================================================================
//  Magic / version / flags / seal constants.
// ===========================================================================
inline constexpr atx::u64 kLibMagic = atx::tsdb::tag8("ATXALIB1");
inline constexpr atx::u64 kLibSealMarker = atx::tsdb::tag8("LIBSEAL!");
inline constexpr atx::u32 kLibFormatVersion = 2U;
inline constexpr atx::u32 kLibFlagSealed = 1U << 0U;

/// True iff this reader can interpret an on-disk library-segment `version`.
[[nodiscard]] constexpr bool is_supported_version(atx::u32 version) noexcept {
  return version >= 1U && version <= kLibFormatVersion;
}

/// Round `value` up to the next multiple of `align` (align must be a power of 2).
[[nodiscard]] constexpr atx::u64 align_up(atx::u64 value, atx::u64 align) noexcept {
  return (value + (align - 1U)) & ~(align - 1U);
}

// ===========================================================================
//  POD records (trivially copyable; memcpy'd to/from the file verbatim).
//
//  Field order is chosen so every member is naturally aligned with NO implicit
//  padding — the layout is fully explicit and the size static_asserts pin it.
// ===========================================================================

/// Fixed header at file offset 0. All section offsets are bytes from base.
struct SegmentHeader {
  atx::u64 magic;          // == kLibMagic
  atx::u64 total_bytes;    // full file size
  atx::u64 n_periods;      // T
  atx::u64 base_alpha_id;  // global AlphaId of local row 0
  atx::u64 content_hash;   // crc32 over the data sections (zero-extended u64)
  atx::u64 off_dir;        // -> AlphaDirectory
  atx::u64 off_pnl;        // -> PnlBlock
  atx::u64 off_pos;        // -> PosBlock
  atx::u64 off_prov;       // -> ProvenanceBlob
  atx::u64 off_footer;     // -> SegmentFooter
  atx::u32 format_version; // == kLibFormatVersion
  atx::u32 flags;          // bit0 = kLibFlagSealed
  atx::u32 n_alphas;       // rows in THIS segment
  atx::u32 n_instruments;  // N
};

/// One alpha's directory entry — fixed-width => O(1) addressable.
struct AlphaDirEntry {
  atx::u64 alpha_id;          // global; == base_alpha_id + local row
  atx::u64 canon_hash;        // the S3 stable cross-run dedup key
  combine::AlphaMetrics metrics; // 7 x f64, copied verbatim
  atx::u64 prov_off;          // byte offset of this alpha's Provenance in the blob
  atx::u64 prov_len;          // byte length of this alpha's serialized Provenance
  atx::u32 lifecycle_at_seal; // lifecycle state captured at seal time
  atx::u32 pad_;              // explicit pad to 8-byte multiple; written 0
};

/// Trailer: seal marker + integrity CRC over [header .. footer-start).
struct SegmentFooter {
  atx::u64 seal_marker;        // == kLibSealMarker
  atx::u32 integrity_crc;      // crc32 of everything before the footer
  atx::u32 reserved;           // written 0
};

static_assert(std::is_trivially_copyable_v<SegmentHeader>);
static_assert(std::is_trivially_copyable_v<AlphaDirEntry>);
static_assert(std::is_trivially_copyable_v<SegmentFooter>);
static_assert(sizeof(SegmentHeader) == 96, "SegmentHeader layout drift");
static_assert(sizeof(AlphaDirEntry) == 96, "AlphaDirEntry layout drift"); // 16 + 56 + 16 + 8
static_assert(sizeof(SegmentFooter) == 16, "SegmentFooter layout drift");
static_assert(sizeof(combine::AlphaMetrics) == 56, "AlphaMetrics is 7 x f64");

// ===========================================================================
//  Provenance — expression source + lineage + scalars (variable length).
// ===========================================================================

/// Per-alpha provenance: DSL expression text, parent canonical hashes, the
/// mutation operator id, and the RNG seed. Serialized length-prefixed.
struct Provenance {
  std::string expr_source;            // S3 DSL expression text
  std::vector<atx::u64> parent_hashes; // lineage (parent canonical hashes)
  atx::u16 mutation_op{0};            // factory mutation operator id
  atx::u64 seed{0};                   // RNG seed
};

namespace detail {

/// Append `value` to `out` as little-endian bytes (T must be trivially copyable).
template <class T> inline void put_le(std::vector<std::byte> &out, const T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::array<std::byte, sizeof(T)> tmp{};
  std::memcpy(tmp.data(), &value, sizeof(T));
  out.insert(out.end(), tmp.begin(), tmp.end());
}

/// Read a trivially-copyable T from `src` at `off`, advancing `off`. Returns
/// false (without advancing) if there are not sizeof(T) bytes remaining.
template <class T>
[[nodiscard]] inline bool get_le(std::span<const std::byte> src, atx::usize &off, T &out) noexcept {
  if (off > src.size() || sizeof(T) > src.size() - off) {
    return false;
  }
  std::memcpy(&out, src.data() + off, sizeof(T));
  off += sizeof(T);
  return true;
}

} // namespace detail

/// Serialize `p` into `out` (APPENDS): u64 expr_len + expr bytes + u64
/// parent_count + parent_count x u64 + u16 mutation_op + u64 seed.
inline void serialize(const Provenance &p, std::vector<std::byte> &out) {
  detail::put_le<atx::u64>(out, static_cast<atx::u64>(p.expr_source.size()));
  const auto *bytes = reinterpret_cast<const std::byte *>(p.expr_source.data());
  out.insert(out.end(), bytes, bytes + p.expr_source.size());
  detail::put_le<atx::u64>(out, static_cast<atx::u64>(p.parent_hashes.size()));
  for (const atx::u64 h : p.parent_hashes) {
    detail::put_le<atx::u64>(out, h);
  }
  detail::put_le<atx::u16>(out, p.mutation_op);
  detail::put_le<atx::u64>(out, p.seed);
}

/// Deserialize a Provenance from `src` (the whole span is one record). A short
/// / malformed buffer yields a default-constructed Provenance (no UB) — callers
/// that need strict validation use the crc'd attach path, which guarantees the
/// blob slices are well-formed.
[[nodiscard]] inline Provenance deserialize_provenance(std::span<const std::byte> src) {
  Provenance p;
  atx::usize off = 0;
  atx::u64 expr_len = 0;
  if (!detail::get_le(src, off, expr_len) || expr_len > src.size() - off) {
    return p;
  }
  p.expr_source.assign(reinterpret_cast<const char *>(src.data() + off),
                       static_cast<atx::usize>(expr_len));
  off += static_cast<atx::usize>(expr_len);
  atx::u64 n_parents = 0;
  if (!detail::get_le(src, off, n_parents)) {
    return p;
  }
  if (n_parents > (src.size() - off) / sizeof(atx::u64)) return p;
  p.parent_hashes.reserve(static_cast<atx::usize>(n_parents));
  for (atx::u64 i = 0; i < n_parents; ++i) {
    atx::u64 h = 0;
    if (!detail::get_le(src, off, h)) {
      return p;
    }
    p.parent_hashes.push_back(h);
  }
  (void)detail::get_le(src, off, p.mutation_op);
  (void)detail::get_le(src, off, p.seed);
  return p;
}

// V2 keeps the 96-byte framing/directory. off_pos points to fixed 256*i16
// sketch slots (not holdings); pad_ is the populated sketch length. PnL is f32.
// The provenance slice is [u64 legacy length, legacy bytes, metadata].
// Missing sketches are explicit length zero, never synthesized from PnL.
enum class LibraryStorageRule : atx::u8 { ExistingOrLegacyV1 = 0, LegacyDenseV1 = 1, CompressedV2 = 2 };
inline constexpr atx::usize kSignalSketchCapacity = 256;

struct AlphaMetadata {
  std::string theme;
  std::string family;
  atx::u32 horizon{0}; // sessions; 0 means not supplied
  atx::f64 tau{0.0};  // sessions; 0 means not supplied
  std::string residualization;
  atx::u64 context_hash{0};
  atx::u64 position_recipe_hash{0};
  atx::u64 sketch_recipe_hash{0};
  atx::f64 sketch_scale{1.0};
  friend bool operator==(const AlphaMetadata&, const AlphaMetadata&) = default;
};

struct CompressedAlphaRecord {
  combine::AlphaMetrics metrics{};
  atx::u64 canon_hash{0};
  Provenance provenance;
  AlphaMetadata metadata;
  std::vector<atx::f32> pnl;
  std::vector<atx::i16> signal_sketch;
};

namespace detail {
[[nodiscard]] inline bool checked_add(atx::u64 a, atx::u64 b, atx::u64& out) noexcept {
  if (b > std::numeric_limits<atx::u64>::max() - a) return false;
  out = a + b; return true;
}
[[nodiscard]] inline bool checked_mul(atx::u64 a, atx::u64 b, atx::u64& out) noexcept {
  if (a != 0U && b > std::numeric_limits<atx::u64>::max() / a) return false;
  out = a * b; return true;
}
inline void put_string(std::vector<std::byte>& out, const std::string& text) {
  put_le(out, static_cast<atx::u64>(text.size()));
  const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
  out.insert(out.end(), bytes.begin(), bytes.end());
}
[[nodiscard]] inline bool get_string(std::span<const std::byte> in, atx::usize& off,
                                      std::string* text = nullptr) {
  atx::u64 size = 0;
  if (!get_le(in, off, size) || size > in.size() - off) return false;
  if (text) text->assign(reinterpret_cast<const char*>(in.data() + off),
                         static_cast<atx::usize>(size));
  off += static_cast<atx::usize>(size); return true;
}
[[nodiscard]] inline bool valid_provenance(std::span<const std::byte> in) {
  atx::usize off = 0;
  atx::u64 parents = 0;
  if (!get_string(in, off) || !get_le(in, off, parents) ||
      parents > (in.size() - off) / sizeof(atx::u64)) return false;
  off += static_cast<atx::usize>(parents) * sizeof(atx::u64);
  return in.size() - off == sizeof(atx::u16) + sizeof(atx::u64);
}
inline void put_metadata(std::vector<std::byte>& out, const AlphaMetadata& m) {
  put_string(out, m.theme); put_string(out, m.family);
  put_le(out, m.horizon); put_le(out, m.tau); put_string(out, m.residualization);
  put_le(out, m.context_hash); put_le(out, m.position_recipe_hash);
  put_le(out, m.sketch_recipe_hash); put_le(out, m.sketch_scale);
}
[[nodiscard]] inline bool get_metadata(std::span<const std::byte> in, atx::usize& off,
                                        AlphaMetadata& m) {
  return get_string(in, off, &m.theme) && get_string(in, off, &m.family) &&
      get_le(in, off, m.horizon) && get_le(in, off, m.tau) &&
      get_string(in, off, &m.residualization) && get_le(in, off, m.context_hash) &&
      get_le(in, off, m.position_recipe_hash) && get_le(in, off, m.sketch_recipe_hash) &&
      get_le(in, off, m.sketch_scale) && off == in.size() &&
      std::isfinite(m.tau) && m.tau >= 0.0 &&
      std::isfinite(m.sketch_scale) && m.sketch_scale > 0.0;
}
} // namespace detail

[[nodiscard]] inline atx::core::Result<std::vector<std::byte>>
write_compressed_segment_bytes(atx::u32 instruments, atx::u64 periods, atx::u64 base,
                               std::span<const CompressedAlphaRecord> records) {
  using atx::core::Err; using atx::core::ErrorCode;
  if (records.empty() || periods == 0U || records.size() > std::numeric_limits<atx::u32>::max() ||
      base > std::numeric_limits<atx::u32>::max() - records.size())
    return Err(ErrorCode::InvalidArgument, "library V2: invalid record geometry");
  SegmentHeader h{};
  h.magic = kLibMagic; h.flags = kLibFlagSealed; h.format_version = 2U;
  h.n_alphas = static_cast<atx::u32>(records.size()); h.n_instruments = instruments;
  h.n_periods = periods; h.base_alpha_id = base; h.off_dir = sizeof(SegmentHeader);
  atx::u64 count = 0, bytes = 0;
  if (!detail::checked_mul(records.size(), sizeof(AlphaDirEntry), bytes) ||
      !detail::checked_add(h.off_dir, bytes, h.off_pnl) ||
      !detail::checked_mul(records.size(), periods, count) ||
      !detail::checked_mul(count, sizeof(atx::f32), bytes) ||
      !detail::checked_add(h.off_pnl, bytes, h.off_pos) ||
      !detail::checked_mul(records.size(), kSignalSketchCapacity * sizeof(atx::i16), bytes) ||
      !detail::checked_add(h.off_pos, bytes, h.off_prov))
    return Err(ErrorCode::InvalidArgument, "library V2: size overflow");
  std::vector<AlphaDirEntry> dir(records.size());
  std::vector<std::byte> blob;
  for (atx::usize a = 0; a < records.size(); ++a) {
    const auto& r = records[a];
    if (r.pnl.size() != periods || r.signal_sketch.size() > kSignalSketchCapacity ||
        !std::isfinite(r.metadata.tau) || r.metadata.tau < 0.0 ||
        !std::isfinite(r.metadata.sketch_scale) || r.metadata.sketch_scale <= 0.0 ||
        std::any_of(r.pnl.begin(), r.pnl.end(), [](auto x) { return std::isinf(x); }))
      return Err(ErrorCode::InvalidArgument, "library V2: invalid record payload");
    std::vector<std::byte> prov;
    serialize(r.provenance, prov);
    const auto start = blob.size();
    detail::put_le(blob, static_cast<atx::u64>(prov.size()));
    blob.insert(blob.end(), prov.begin(), prov.end());
    detail::put_metadata(blob, r.metadata);
    dir[a] = AlphaDirEntry{base + a, r.canon_hash, r.metrics, start, blob.size() - start,
                          0U, static_cast<atx::u32>(r.signal_sketch.size())};
  }
  if (!detail::checked_add(h.off_prov, blob.size(), bytes) ||
      !detail::checked_add(bytes, 7U, bytes))
    return Err(ErrorCode::InvalidArgument, "library V2: metadata size overflow");
  h.off_footer = bytes & ~atx::u64{7};
  if (!detail::checked_add(h.off_footer, sizeof(SegmentFooter), h.total_bytes) ||
      h.total_bytes > std::numeric_limits<atx::usize>::max())
    return Err(ErrorCode::InvalidArgument, "library V2: total size overflow");
  std::vector<std::byte> out(static_cast<atx::usize>(h.total_bytes));
  std::memcpy(out.data() + h.off_dir, dir.data(), dir.size() * sizeof(AlphaDirEntry));
  for (atx::usize a = 0; a < records.size(); ++a) {
    const auto& r = records[a];
    std::memcpy(out.data() + h.off_pnl + a * periods * sizeof(atx::f32),
                r.pnl.data(), r.pnl.size() * sizeof(atx::f32));
    if (!r.signal_sketch.empty())
      std::memcpy(out.data() + h.off_pos + a * kSignalSketchCapacity * sizeof(atx::i16),
                  r.signal_sketch.data(), r.signal_sketch.size() * sizeof(atx::i16));
  }
  std::memcpy(out.data() + h.off_prov, blob.data(), blob.size());
  h.content_hash = atx::tsdb::crc32(out.data() + h.off_dir,
                                   static_cast<atx::usize>(h.off_footer - h.off_dir));
  std::memcpy(out.data(), &h, sizeof(h));
  const SegmentFooter footer{kLibSealMarker,
      atx::tsdb::crc32(out.data(), static_cast<atx::usize>(h.off_footer)), 0U};
  std::memcpy(out.data() + h.off_footer, &footer, sizeof(footer));
  return atx::core::Ok(std::move(out));
}

// ===========================================================================
//  Header constructors / geometry.
// ===========================================================================

/// Compute section offsets + a fully-populated header for a segment of the given
/// shape. content_hash is filled by the writer (left 0 here); offsets are exact.
[[nodiscard]] inline SegmentHeader make_header(atx::u32 n_alphas, atx::u32 n_instruments,
                                               atx::u64 n_periods, atx::u64 base_alpha_id) noexcept {
  SegmentHeader h{};
  h.magic = kLibMagic;
  h.format_version = 1U; // frozen dense V1 writer
  h.flags = kLibFlagSealed;
  h.n_alphas = n_alphas;
  h.n_instruments = n_instruments;
  h.n_periods = n_periods;
  h.base_alpha_id = base_alpha_id;
  h.content_hash = 0;
  const atx::u64 dir_bytes = static_cast<atx::u64>(n_alphas) * sizeof(AlphaDirEntry);
  const atx::u64 pnl_bytes = static_cast<atx::u64>(n_alphas) * n_periods * sizeof(atx::f64);
  const atx::u64 pos_bytes =
      static_cast<atx::u64>(n_alphas) * n_periods * n_instruments * sizeof(atx::f64);
  h.off_dir = sizeof(SegmentHeader);
  h.off_pnl = h.off_dir + dir_bytes;
  h.off_pos = h.off_pnl + pnl_bytes;
  h.off_prov = h.off_pos + pos_bytes;
  // off_footer + total_bytes are finalized by the writer once the (variable-
  // length) provenance blob size is known.
  h.off_footer = h.off_prov;
  h.total_bytes = h.off_prov + sizeof(SegmentFooter);
  return h;
}

// ===========================================================================
//  One-pass sealed-segment writer (in-memory).
//
//  Lays out every section, serializes the provenance blob, fills the directory
//  offsets, computes content_hash (crc32 over the data sections) and the footer
//  integrity_crc (crc32 over [header..footer-start)), sets SEALED, and returns
//  the whole sealed file as bytes — mirroring the atx::tsdb builder's single
//  write pass. The store's flush() writes these bytes to a file in one shot.
// ===========================================================================
[[nodiscard]] inline std::vector<std::byte>
write_segment_bytes(atx::u32 n_alphas, atx::u32 n_instruments, atx::u64 n_periods,
                    atx::u64 base_alpha_id, std::span<const atx::f64> pnl,
                    std::span<const atx::f64> pos, std::span<const combine::AlphaMetrics> metrics,
                    std::span<const atx::u64> canon_hashes,
                    std::span<const Provenance> provenance) {
  SegmentHeader h = make_header(n_alphas, n_instruments, n_periods, base_alpha_id);

  // 1) Serialize the provenance blob and build the directory (with blob slices).
  std::vector<std::byte> blob;
  std::vector<AlphaDirEntry> dir(n_alphas);
  for (atx::u32 a = 0; a < n_alphas; ++a) {
    const atx::u64 prov_off = static_cast<atx::u64>(blob.size());
    serialize(provenance[a], blob);
    AlphaDirEntry &e = dir[a];
    e.alpha_id = base_alpha_id + a;
    e.canon_hash = canon_hashes[a];
    e.metrics = metrics[a];
    e.prov_off = prov_off;
    e.prov_len = static_cast<atx::u64>(blob.size()) - prov_off;
    e.lifecycle_at_seal = 0;
    e.pad_ = 0;
  }
  // 2) Finalize offsets now that the blob length is known. The ProvenanceBlob
  //    is variable-length and NOT a multiple of 8, so pad off_footer up to an
  //    8-byte boundary: SegmentFooter has alignof 8, and keeping it naturally
  //    aligned future-proofs the format (the pad gap is part of the crc'd
  //    region but stays deterministic because `out` is zero-initialized).
  const atx::u64 footer_off = align_up(h.off_prov + static_cast<atx::u64>(blob.size()),
                                       alignof(SegmentFooter));
  h.off_footer = footer_off;
  h.total_bytes = h.off_footer + sizeof(SegmentFooter);

  std::vector<std::byte> out(static_cast<atx::usize>(h.total_bytes));
  auto write_at = [&out](atx::u64 off, const void *src, atx::usize n) {
    std::memcpy(out.data() + off, src, n);
  };

  // 3) content_hash = crc32 over the data sections [off_dir .. off_footer).
  //    Compute it over the assembled data BEFORE the header is written so the
  //    header's content_hash field is excluded from its own digest.
  write_at(h.off_dir, dir.data(), dir.size() * sizeof(AlphaDirEntry));
  if (!pnl.empty()) {
    write_at(h.off_pnl, pnl.data(), pnl.size() * sizeof(atx::f64));
  }
  if (!pos.empty()) {
    write_at(h.off_pos, pos.data(), pos.size() * sizeof(atx::f64));
  }
  if (!blob.empty()) {
    write_at(h.off_prov, blob.data(), blob.size());
  }
  const atx::usize data_len = static_cast<atx::usize>(h.off_footer - h.off_dir);
  h.content_hash = atx::tsdb::crc32(out.data() + h.off_dir, data_len);

  // 4) Write the (now-complete) header, then the footer (integrity crc over
  //    everything before the footer — i.e. [0 .. off_footer)).
  write_at(0, &h, sizeof(h));
  SegmentFooter f{};
  f.seal_marker = kLibSealMarker;
  f.reserved = 0;
  f.integrity_crc = atx::tsdb::crc32(out.data(), static_cast<atx::usize>(h.off_footer));
  write_at(h.off_footer, &f, sizeof(f));
  return out;
}

// ===========================================================================
//  SegmentReaderLite — read-only attach + O(1) addressing into a sealed segment.
//
//  attach(path): map the file read-only via atx::tsdb::Mapping (RAII), validate
//  magic / version / seal / integrity-crc, and only then expose accessors.
//  attach_bytes(span): the same validation over an in-memory byte span (the unit-
//  test path, no filesystem). A bad file/buffer returns Err — never UB.
//
//  SAFETY: every span/ref accessor aliases the underlying mapping (or the
//  attach_bytes copy this reader owns). It DANGLES when this reader is destroyed
//  or move-assigned. Copy out before the reader dies.
// ===========================================================================
class SegmentReaderLite {
public:
  /// Map + validate `path`. Err(IoError) if it cannot be mapped (from Mapping);
  /// Err(InvalidArgument) on short file / bad magic / unsupported version /
  /// missing seal; Err(Internal) on integrity-crc mismatch.
  [[nodiscard]] static atx::core::Result<SegmentReaderLite> attach(const std::string &path) {
    auto mapped = atx::tsdb::Mapping::map_file_ro(path);
    if (!mapped) {
      return atx::core::Err(std::move(mapped).error());
    }
    SegmentReaderLite r;
    r.map_ = std::move(*mapped);
    // SAFETY: the bytes live in the mapping for r.map_'s lifetime; this span is
    // only used to validate + as the addressing base, never escapes the reader.
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte *>(r.map_.base()), r.map_.size()};
    if (auto st = r.validate(bytes); !st) {
      return atx::core::Err(std::move(st).error());
    }
    r.base_ = reinterpret_cast<const std::byte *>(r.map_.base());
    r.decoded_.resize(r.n_alphas());
    return atx::core::Result<SegmentReaderLite>{std::move(r)};
  }

  /// Validate an in-memory sealed segment. On success the reader OWNS a copy of
  /// the bytes (so the accessors stay valid for the reader's lifetime). Same
  /// error contract as attach().
  [[nodiscard]] static atx::core::Result<SegmentReaderLite>
  attach_bytes(std::span<const std::byte> bytes) {
    SegmentReaderLite r;
    r.owned_.assign(bytes.begin(), bytes.end());
    if (auto st = r.validate(r.owned_); !st) {
      return atx::core::Err(std::move(st).error());
    }
    r.base_ = r.owned_.data();
    r.decoded_.resize(r.n_alphas());
    return atx::core::Result<SegmentReaderLite>{std::move(r)};
  }

  SegmentReaderLite() = default;
  SegmentReaderLite(SegmentReaderLite &&) noexcept = default;
  SegmentReaderLite &operator=(SegmentReaderLite &&) noexcept = default;
  SegmentReaderLite(const SegmentReaderLite &) = delete;
  SegmentReaderLite &operator=(const SegmentReaderLite &) = delete;

  [[nodiscard]] atx::u32 format_version() const noexcept { return header().format_version; }
  [[nodiscard]] atx::u32 n_alphas() const noexcept { return header().n_alphas; }
  [[nodiscard]] atx::u32 n_instruments() const noexcept { return header().n_instruments; }
  [[nodiscard]] atx::u64 n_periods() const noexcept { return header().n_periods; }
  [[nodiscard]] atx::u64 base_alpha_id() const noexcept { return header().base_alpha_id; }
  [[nodiscard]] atx::u64 content_hash() const noexcept { return header().content_hash; }
  [[nodiscard]] atx::u32 integrity_crc() const noexcept { return footer().integrity_crc; }

  /// Alpha `local`'s PnL row (length n_periods), alpha-major.
  /// SAFETY: aliases the mapping/owned bytes; dangles when this reader dies.
  [[nodiscard]] std::span<const atx::f64> pnl_row(atx::u32 local) const {
    ATX_CHECK(local < n_alphas());
    const SegmentHeader &h = header();
    if (h.format_version == 2U) {
      auto& row = decoded_[local];
      if (row.empty()) {
        const auto* first = reinterpret_cast<const atx::f32*>(
            base_ + h.off_pnl + local * h.n_periods * sizeof(atx::f32));
        row.assign(first, first + static_cast<atx::usize>(h.n_periods));
      }
      return row;
    }
    const atx::u64 off = h.off_pnl + static_cast<atx::u64>(local) * h.n_periods * sizeof(atx::f64);
    return {f64_at(off), static_cast<atx::usize>(h.n_periods)};
  }

  /// Alpha `local`'s position cross-section at `period` (length n_instruments).
  /// SAFETY: aliases the mapping/owned bytes; dangles when this reader dies.
  [[nodiscard]] std::span<const atx::f64> pos_row(atx::u32 local, atx::u64 period) const noexcept {
    ATX_CHECK(format_version() == 1U && local < n_alphas() && period < n_periods());
    const SegmentHeader &h = header();
    const atx::u64 cells = (static_cast<atx::u64>(local) * h.n_periods + period) * h.n_instruments;
    const atx::u64 off = h.off_pos + cells * sizeof(atx::f64);
    return {f64_at(off), static_cast<atx::usize>(h.n_instruments)};
  }

  /// Directory entry for local row `local`.
  /// SAFETY: aliases the mapping/owned bytes; dangles when this reader dies.
  [[nodiscard]] const AlphaDirEntry &dir_entry(atx::u32 local) const noexcept {
    ATX_CHECK(local < n_alphas());
    const atx::u64 off = header().off_dir + static_cast<atx::u64>(local) * sizeof(AlphaDirEntry);
    // SAFETY: off in range by attach-time validation; AlphaDirEntry is POD.
    return *reinterpret_cast<const AlphaDirEntry *>(base_ + off);
  }

  /// Decode local row `local`'s Provenance from the blob slice in its dir entry.
  /// Returns a fresh owning copy (independent of the mapping lifetime).
  [[nodiscard]] Provenance provenance(atx::u32 local) const {
    const AlphaDirEntry &e = dir_entry(local);
    const std::span<const std::byte> slice{base_ + header().off_prov + e.prov_off,
                                           static_cast<atx::usize>(e.prov_len)};
    if (format_version() == 1U) return deserialize_provenance(slice);
    atx::usize off = 0; atx::u64 size = 0;
    const bool ok = detail::get_le(slice, off, size); ATX_CHECK(ok);
    return deserialize_provenance(slice.subspan(off, static_cast<atx::usize>(size)));
  }

  [[nodiscard]] std::span<const atx::f32> pnl_f32_row(atx::u32 local) const noexcept {
    ATX_CHECK(format_version() == 2U && local < n_alphas());
    const auto& h = header();
    return {reinterpret_cast<const atx::f32*>(
        base_ + h.off_pnl + local * h.n_periods * sizeof(atx::f32)),
        static_cast<atx::usize>(h.n_periods)};
  }
  // Cold append invalidates prior PnL spans just like stage/flush; release old
  // compatibility copies before composing the newly extended rows.
  void clear_pnl_cache() const {
    for (auto& row : decoded_) std::vector<atx::f64>{}.swap(row);
  }
  [[nodiscard]] AlphaMetadata metadata(atx::u32 local) const {
    if (format_version() == 1U) return {};
    const auto& e = dir_entry(local);
    const std::span<const std::byte> slice{base_ + header().off_prov + e.prov_off,
                                         static_cast<atx::usize>(e.prov_len)};
    atx::usize off = 0; atx::u64 size = 0;
    const bool size_ok = detail::get_le(slice, off, size); ATX_CHECK(size_ok);
    off += static_cast<atx::usize>(size);
    AlphaMetadata result;
    const bool ok = detail::get_metadata(slice, off, result); ATX_CHECK(ok);
    return result;
  }
  [[nodiscard]] std::span<const atx::i16> signal_sketch(atx::u32 local) const noexcept {
    if (format_version() == 1U) return {};
    const auto& e = dir_entry(local);
    const auto off = header().off_pos + local * kSignalSketchCapacity * sizeof(atx::i16);
    return {reinterpret_cast<const atx::i16*>(base_ + off), e.pad_};
  }

private:
  [[nodiscard]] const SegmentHeader &header() const noexcept {
    // SAFETY: validate() confirmed size >= sizeof(SegmentHeader) + magic.
    return *reinterpret_cast<const SegmentHeader *>(base_);
  }
  [[nodiscard]] SegmentFooter footer() const noexcept {
    // Read by value via memcpy (NOT a reinterpret_cast deref): although the
    // writer now 8-aligns off_footer (so the footer IS naturally aligned), the
    // ProvenanceBlob is variable-length, so reading through a typed pointer is
    // alignment-fragile if the format ever changes. memcpy is alignment-agnostic
    // and well-defined for any off_footer. validate() confirmed
    // off_footer + sizeof(SegmentFooter) <= size, so the read is in-bounds.
    SegmentFooter f{};
    std::memcpy(&f, base_ + header().off_footer, sizeof(f));
    return f;
  }
  [[nodiscard]] const atx::f64 *f64_at(atx::u64 off) const noexcept {
    // SAFETY: section offsets validated at attach; f64 grids are 8-byte aligned
    // because every preceding section is a multiple of 8 bytes.
    return reinterpret_cast<const atx::f64 *>(base_ + off);
  }

  /// Validate a candidate segment buffer end-to-end before any byte is exposed.
  [[nodiscard]] static atx::core::Status validate(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(SegmentHeader)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library segment: shorter than header");
    }
    SegmentHeader h{};
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (h.magic != kLibMagic) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library segment: bad magic");
    }
    if (!is_supported_version(h.format_version)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library segment: unsupported format_version");
    }
    if ((h.flags & kLibFlagSealed) == 0U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library segment: not sealed");
    }
    if (h.total_bytes != bytes.size() ||
        h.off_footer > bytes.size() || bytes.size() - h.off_footer != sizeof(SegmentFooter) ||
        h.off_footer < sizeof(SegmentHeader)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library segment: section offsets out of range");
    }
    atx::u64 dir_bytes = 0, cells = 0, pnl_bytes = 0, pos_cells = 0, pos_bytes = 0;
    const bool compressed = h.format_version == 2U;
    if ((compressed && (h.n_alphas == 0U || h.n_periods == 0U)) ||
        !detail::checked_mul(h.n_alphas, sizeof(AlphaDirEntry), dir_bytes) ||
        !detail::checked_mul(h.n_alphas, h.n_periods, cells) ||
        !detail::checked_mul(cells, compressed ? sizeof(atx::f32) : sizeof(atx::f64), pnl_bytes) ||
        !detail::checked_mul(compressed ? h.n_alphas : cells,
                             compressed ? kSignalSketchCapacity : h.n_instruments, pos_cells) ||
        !detail::checked_mul(pos_cells, compressed ? sizeof(atx::i16) : sizeof(atx::f64), pos_bytes) ||
        h.off_dir != sizeof(SegmentHeader) || h.off_pnl < h.off_dir ||
        h.off_pos < h.off_pnl || h.off_prov < h.off_pos || h.off_footer < h.off_prov ||
        h.off_pnl - h.off_dir != dir_bytes || h.off_pos - h.off_pnl != pnl_bytes ||
        h.off_prov - h.off_pos != pos_bytes || h.n_periods > std::numeric_limits<atx::usize>::max() ||
        h.base_alpha_id > std::numeric_limits<atx::u32>::max() - static_cast<atx::u64>(h.n_alphas))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library segment: invalid section geometry");
    for (atx::u32 a = 0; a < h.n_alphas; ++a) {
      AlphaDirEntry e{};
      std::memcpy(&e, bytes.data() + h.off_dir + a * sizeof(e), sizeof(e));
      if (e.alpha_id != h.base_alpha_id + a || e.prov_off > h.off_footer - h.off_prov ||
          e.prov_len > h.off_footer - h.off_prov - e.prov_off ||
          (compressed && e.pad_ > kSignalSketchCapacity))
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "library segment: invalid directory entry");
      const auto slice = bytes.subspan(static_cast<atx::usize>(h.off_prov + e.prov_off),
                                        static_cast<atx::usize>(e.prov_len));
      bool valid = false;
      if (!compressed) valid = detail::valid_provenance(slice);
      else {
        atx::usize off = 0; atx::u64 size = 0; AlphaMetadata metadata;
        if (detail::get_le(slice, off, size) && size <= slice.size() - off &&
            detail::valid_provenance(slice.subspan(off, static_cast<atx::usize>(size)))) {
          off += static_cast<atx::usize>(size);
          valid = detail::get_metadata(slice, off, metadata);
        }
      }
      if (!valid) return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                       "library segment: invalid provenance/metadata");
    }
    SegmentFooter f{};
    std::memcpy(&f, bytes.data() + h.off_footer, sizeof(f));
    if (f.seal_marker != kLibSealMarker) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library segment: bad seal");
    }
    const atx::u32 want = atx::tsdb::crc32(bytes.data(), static_cast<atx::usize>(h.off_footer));
    if (want != f.integrity_crc) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "library segment: integrity crc mismatch");
    }
    if (compressed) {
      for (atx::u64 i = 0; i < cells; ++i) {
        atx::f32 value{};
        std::memcpy(&value, bytes.data() + h.off_pnl + i * sizeof(value), sizeof(value));
        if (std::isinf(value))
          return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                "library V2: infinite PnL payload");
      }
    }
    return atx::core::Ok();
  }

  mutable std::vector<std::vector<atx::f64>> decoded_; // lazy V2 compatibility rows, never positions
  atx::tsdb::Mapping map_;           // owned mapping (attach() path); empty for attach_bytes
  std::vector<std::byte> owned_;     // owned copy (attach_bytes() path); empty for attach()
  const std::byte *base_{nullptr};   // -> validated segment bytes (in map_ or owned_)
};

} // namespace atx::engine::library
