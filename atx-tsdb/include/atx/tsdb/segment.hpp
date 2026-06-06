#pragma once

// atx::tsdb segment format — THE on-disk contract shared by builder and reader.
//
// One contiguous file. Every internal reference is a byte OFFSET from the base
// of the mapping (never a raw pointer), so a reader can map the file at any
// virtual address. Sections, in order:
//   SegmentHeader | FieldDirectory | SymbolTable | StringBlob | TimeAxis |
//   FieldBlocks (F x [T][N] f64, row-major) | PresentBitmap | SegmentFooter.
//
// Endianness: the raw f64/i64 grids are written/read in native byte order. Both
// supported targets are little-endian x86_64; the static_assert below pins that
// assumption so a big-endian port fails loudly rather than silently corrupting.

#include <bit>
#include <span>

#include "atx/core/types.hpp"

namespace atx::tsdb {

static_assert(std::endian::native == std::endian::little,
              "atx-tsdb segment format assumes a little-endian target");

/// Pack an 8-char tag into a u64 (little-endian: s[0] is the low byte).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
[[nodiscard]] consteval atx::u64 tag8(const char (&s)[9]) noexcept {
  atx::u64 v = 0;
  for (atx::u32 i = 0; i < 8U; ++i) {
    v |= static_cast<atx::u64>(static_cast<atx::u8>(s[i])) << (8U * i);
  }
  return v;
}

inline constexpr atx::u64 kMagic = tag8("ATXSEG01");
inline constexpr atx::u64 kSealMarker = tag8("SEALED!!");
inline constexpr atx::u32 kFormatVersion = 2U; // v2: 64B-aligned field blocks
inline constexpr atx::u32 kFlagSealed = 1U << 0U;
inline constexpr atx::u32 kFieldNameLen = 16U; // NUL-padded field name capacity
inline constexpr atx::u64 kBlockAlign =
    64U; // field-block byte alignment in v2 (cache line / AVX-512)

/// True iff this reader can interpret an on-disk `version`. v1 (packed blocks)
/// and v2 (64B-padded blocks) are both addressable via header offsets.
[[nodiscard]] constexpr bool is_supported_version(atx::u32 version) noexcept {
  return version >= 1U && version <= kFormatVersion;
}

// ---------------------------------------------------------------------------
//  POD records (trivially copyable; memcpy'd to/from the file verbatim).
// ---------------------------------------------------------------------------

/// Fixed header at file offset 0. All section offsets are bytes from base.
struct SegmentHeader {
  atx::u64 magic;            // == kMagic
  atx::u32 format_version;   // == kFormatVersion
  atx::u32 flags;            // bit0 = kFlagSealed
  atx::u64 total_bytes;      // full file size
  atx::u32 field_count;      // F
  atx::u32 instrument_count; // N
  atx::u64 time_count;       // T
  atx::i64 created_at_nanos; // wall clock at build (supplied by caller)
  atx::u64 content_hash;     // crc32 over data sections (zero-extended)
  atx::u64 off_field_dir;
  atx::u64 off_symbol_table;
  atx::u64 off_string_blob;
  atx::u64 off_time_axis;
  atx::u64 off_field_blocks;
  atx::u64 off_present_bitmap;
  atx::u64 off_footer;
};

/// One field-directory entry: NUL-padded name + its block index.
struct FieldEntry {
  // SAFETY: fixed-size on-disk POD field name (NUL-padded), memcpy'd verbatim;
  // a C array pins the ABI layout. std::array<char,16> is layout-compatible but
  // not adopted to keep the aggregate-init/memcpy sites simple.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  char name[kFieldNameLen]; // e.g. "close"
  atx::u32 field_index;
  atx::u32 reserved; // pad to 8-byte alignment; written 0
};

/// One symbol-table entry: slice into the string blob (offsets from blob start).
struct SymbolEntry {
  atx::u32 name_off; // byte offset within StringBlob
  atx::u32 name_len; // byte length
};

/// Trailer: seal marker + integrity CRC over [header .. present-bitmap end].
struct SegmentFooter {
  atx::u64 seal_marker;   // == kSealMarker
  atx::u32 integrity_crc; // crc32 of everything before the footer
  atx::u32 reserved;      // written 0
};

static_assert(sizeof(SegmentHeader) == 112, "SegmentHeader layout drift");
static_assert(std::is_trivially_copyable_v<SegmentHeader>);
static_assert(std::is_trivially_copyable_v<FieldEntry>);
static_assert(std::is_trivially_copyable_v<SymbolEntry>);
static_assert(std::is_trivially_copyable_v<SegmentFooter>);

// ---------------------------------------------------------------------------
//  Geometry + addressing (pure functions over a header and a mapping base).
// ---------------------------------------------------------------------------

/// u64 words needed to hold an N-bit present-bitmap row.
[[nodiscard]] constexpr atx::u64 mask_words(atx::u32 n_inst) noexcept {
  return (static_cast<atx::u64>(n_inst) + 63U) / 64U;
}

/// Bytes from one field block's start to the next. v1 packs blocks (stride ==
/// T*N*8); v2 pads each block up to a 64-byte boundary so the block start is
/// SIMD-aligned in the mapping. The block's live data is always the first T*N
/// f64; the padding is trailing and never read.
[[nodiscard]] constexpr atx::u64 field_block_stride_bytes(const SegmentHeader &h) noexcept {
  const atx::u64 packed =
      h.time_count * static_cast<atx::u64>(h.instrument_count) * sizeof(atx::f64);
  if (h.format_version >= 2U) {
    return (packed + (kBlockAlign - 1U)) & ~(kBlockAlign - 1U);
  }
  return packed;
}

/// Pointer to the first f64 of field block `field`.
[[nodiscard]] inline const atx::f64 *field_block(const atx::u8 *base, const SegmentHeader &h,
                                                 atx::u32 field) noexcept {
  const atx::u64 byte_off =
      h.off_field_blocks + static_cast<atx::u64>(field) * field_block_stride_bytes(h);
  // SAFETY: byte_off < total_bytes by construction (validated at attach). The
  // section is laid out f64-aligned (64B-aligned in v2), so the reinterpret is
  // well-defined.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return reinterpret_cast<const atx::f64 *>(base + byte_off);
}

/// Non-owning view of field block `field` (length T*N, padding excluded).
[[nodiscard]] inline std::span<const atx::f64>
field_block_view(const atx::u8 *base, const SegmentHeader &h, atx::u32 field) noexcept {
  return {field_block(base, h, field), static_cast<atx::usize>(h.time_count * h.instrument_count)};
}

/// Value at (field, t, inst). No bounds check (caller guarantees in range).
[[nodiscard]] inline atx::f64 cell_value(const atx::u8 *base, const SegmentHeader &h,
                                         atx::u32 field, atx::u64 t, atx::u32 inst) noexcept {
  // SAFETY: index bounded by caller contract (field<F, t<T, inst<N); flat field-block ring.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return field_block(base, h, field)[t * h.instrument_count + inst];
}

/// True iff cell (t, inst) was present at build time.
[[nodiscard]] inline bool cell_present(const atx::u8 *base, const SegmentHeader &h, atx::u64 t,
                                       atx::u32 inst) noexcept {
  const atx::u64 mw = mask_words(h.instrument_count);
  // SAFETY: bitmap section is u64-aligned and sized T*mw; index is in range by
  // caller contract. Read-only mapped bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *bm = reinterpret_cast<const atx::u64 *>(base + h.off_present_bitmap);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const atx::u64 word = bm[t * mw + (inst >> 6U)];
  return ((word >> (inst & 63U)) & 1ULL) != 0ULL;
}

/// The time axis (T ascending unix-nanos) as a non-owning span.
[[nodiscard]] inline std::span<const atx::i64> time_axis(const atx::u8 *base,
                                                         const SegmentHeader &h) noexcept {
  // SAFETY: axis section is i64-aligned, sized T. Read-only mapped bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *p = reinterpret_cast<const atx::i64 *>(base + h.off_time_axis);
  return {p, static_cast<atx::usize>(h.time_count)};
}

} // namespace atx::tsdb
