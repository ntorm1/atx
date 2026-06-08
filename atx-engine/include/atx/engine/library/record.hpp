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

#include <array>
#include <bit>     // std::bit_cast, std::endian
#include <cstring> // std::memcpy
#include <optional>
#include <span>
#include <string>
#include <utility> // std::move
#include <vector>

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
inline constexpr atx::u32 kLibFormatVersion = 1U;
inline constexpr atx::u32 kLibFlagSealed = 1U << 0U;

/// True iff this reader can interpret an on-disk library-segment `version`.
[[nodiscard]] constexpr bool is_supported_version(atx::u32 version) noexcept {
  return version >= 1U && version <= kLibFormatVersion;
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
  if (off + sizeof(T) > src.size()) {
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
  if (!detail::get_le(src, off, expr_len) || off + expr_len > src.size()) {
    return p;
  }
  p.expr_source.assign(reinterpret_cast<const char *>(src.data() + off),
                       static_cast<atx::usize>(expr_len));
  off += static_cast<atx::usize>(expr_len);
  atx::u64 n_parents = 0;
  if (!detail::get_le(src, off, n_parents)) {
    return p;
  }
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

// ===========================================================================
//  Header constructors / geometry.
// ===========================================================================

/// Compute section offsets + a fully-populated header for a segment of the given
/// shape. content_hash is filled by the writer (left 0 here); offsets are exact.
[[nodiscard]] inline SegmentHeader make_header(atx::u32 n_alphas, atx::u32 n_instruments,
                                               atx::u64 n_periods, atx::u64 base_alpha_id) noexcept {
  SegmentHeader h{};
  h.magic = kLibMagic;
  h.format_version = kLibFormatVersion;
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
  // 2) Finalize offsets now that the blob length is known.
  h.off_footer = h.off_prov + static_cast<atx::u64>(blob.size());
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
    return atx::core::Result<SegmentReaderLite>{std::move(r)};
  }

  SegmentReaderLite() = default;
  SegmentReaderLite(SegmentReaderLite &&) noexcept = default;
  SegmentReaderLite &operator=(SegmentReaderLite &&) noexcept = default;
  SegmentReaderLite(const SegmentReaderLite &) = delete;
  SegmentReaderLite &operator=(const SegmentReaderLite &) = delete;

  [[nodiscard]] atx::u32 n_alphas() const noexcept { return header().n_alphas; }
  [[nodiscard]] atx::u32 n_instruments() const noexcept { return header().n_instruments; }
  [[nodiscard]] atx::u64 n_periods() const noexcept { return header().n_periods; }
  [[nodiscard]] atx::u64 base_alpha_id() const noexcept { return header().base_alpha_id; }
  [[nodiscard]] atx::u64 content_hash() const noexcept { return header().content_hash; }
  [[nodiscard]] atx::u32 integrity_crc() const noexcept { return footer().integrity_crc; }

  /// Alpha `local`'s PnL row (length n_periods), alpha-major.
  /// SAFETY: aliases the mapping/owned bytes; dangles when this reader dies.
  [[nodiscard]] std::span<const atx::f64> pnl_row(atx::u32 local) const noexcept {
    const SegmentHeader &h = header();
    const atx::u64 off = h.off_pnl + static_cast<atx::u64>(local) * h.n_periods * sizeof(atx::f64);
    return {f64_at(off), static_cast<atx::usize>(h.n_periods)};
  }

  /// Alpha `local`'s position cross-section at `period` (length n_instruments).
  /// SAFETY: aliases the mapping/owned bytes; dangles when this reader dies.
  [[nodiscard]] std::span<const atx::f64> pos_row(atx::u32 local, atx::u64 period) const noexcept {
    const SegmentHeader &h = header();
    const atx::u64 cells = (static_cast<atx::u64>(local) * h.n_periods + period) * h.n_instruments;
    const atx::u64 off = h.off_pos + cells * sizeof(atx::f64);
    return {f64_at(off), static_cast<atx::usize>(h.n_instruments)};
  }

  /// Directory entry for local row `local`.
  /// SAFETY: aliases the mapping/owned bytes; dangles when this reader dies.
  [[nodiscard]] const AlphaDirEntry &dir_entry(atx::u32 local) const noexcept {
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
    return deserialize_provenance(slice);
  }

private:
  [[nodiscard]] const SegmentHeader &header() const noexcept {
    // SAFETY: validate() confirmed size >= sizeof(SegmentHeader) + magic.
    return *reinterpret_cast<const SegmentHeader *>(base_);
  }
  [[nodiscard]] const SegmentFooter &footer() const noexcept {
    // SAFETY: validate() confirmed off_footer + sizeof(SegmentFooter) <= size.
    return *reinterpret_cast<const SegmentFooter *>(base_ + header().off_footer);
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
        h.off_footer + sizeof(SegmentFooter) > bytes.size() || h.off_footer < sizeof(SegmentHeader)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library segment: section offsets out of range");
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
    return atx::core::Ok();
  }

  atx::tsdb::Mapping map_;           // owned mapping (attach() path); empty for attach_bytes
  std::vector<std::byte> owned_;     // owned copy (attach_bytes() path); empty for attach()
  const std::byte *base_{nullptr};   // -> validated segment bytes (in map_ or owned_)
};

} // namespace atx::engine::library
