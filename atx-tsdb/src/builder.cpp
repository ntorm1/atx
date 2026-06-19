#include "atx/tsdb/builder.hpp"

#include <cstring> // std::memcpy
#include <fstream>
#include <ios>     // std::ios, std::streamsize
#include <limits>  // std::numeric_limits
#include <string>  // std::string
#include <utility> // std::move
#include <vector>  // std::vector

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"
#include "atx/tsdb/checksum.hpp"
#include "atx/tsdb/segment.hpp"

namespace atx::tsdb {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Status;

namespace {

[[nodiscard]] atx::u64 align_up(atx::u64 v, atx::u64 a) noexcept {
  return (v + a - 1U) & ~(a - 1U);
}

} // namespace

SegmentBuilder::SegmentBuilder(std::vector<std::string> field_names,
                               std::vector<std::string> symbols, std::vector<atx::i64> time_axis)
    : field_names_{std::move(field_names)}, symbols_{std::move(symbols)},
      time_axis_{std::move(time_axis)} {
  const atx::u64 f = field_names_.size();
  const atx::u64 n = symbols_.size();
  const atx::u64 t = time_axis_.size();
  // NaN-fill every cell so an unset cell reads as "absent" even if its present
  // bit were ever misread; present_ is the authoritative absence signal.
  blocks_.assign(static_cast<atx::usize>(f * t * n), std::numeric_limits<atx::f64>::quiet_NaN());
  present_.assign(static_cast<atx::usize>(t * mask_words(static_cast<atx::u32>(n))), 0ULL);
}

void SegmentBuilder::set(atx::u32 field, atx::u64 t, atx::u32 inst, atx::f64 value) noexcept {
  [[maybe_unused]] const atx::u64 f = field_names_.size();
  const atx::u64 n = symbols_.size();
  const atx::u64 nt = time_axis_.size();
  ATX_ASSERT(static_cast<atx::u64>(field) < f);
  ATX_ASSERT(t < nt);
  ATX_ASSERT(static_cast<atx::u64>(inst) < n);
  blocks_[static_cast<atx::usize>(static_cast<atx::u64>(field) * nt * n + t * n +
                                  static_cast<atx::u64>(inst))] = value;
  const atx::u64 mw = mask_words(static_cast<atx::u32>(n));
  present_[static_cast<atx::usize>(t * mw + (static_cast<atx::u64>(inst) >> 6U))] |=
      (1ULL << (inst & 63U));
}

Status SegmentBuilder::write(const std::string &path, atx::i64 created_at_nanos) const {
  const auto f = static_cast<atx::u32>(field_names_.size());
  const auto n = static_cast<atx::u32>(symbols_.size());
  const atx::u64 t = time_axis_.size();
  const atx::u64 mw = mask_words(n);

  // --- string blob: concatenate symbol names; record (off,len) per symbol ---
  std::vector<SymbolEntry> sym_entries(n);
  std::string blob;
  for (atx::u32 i = 0; i < n; ++i) {
    sym_entries[i].name_off = static_cast<atx::u32>(blob.size());
    sym_entries[i].name_len = static_cast<atx::u32>(symbols_[i].size());
    blob += symbols_[i];
  }

  // --- section offsets -------------------------------------------------------
  atx::u64 off = sizeof(SegmentHeader);

  const atx::u64 off_field_dir = off;
  off += static_cast<atx::u64>(f) * sizeof(FieldEntry);

  const atx::u64 off_symbol_table = off;
  off += static_cast<atx::u64>(n) * sizeof(SymbolEntry);

  const atx::u64 off_string_blob = off;
  off += static_cast<atx::u64>(blob.size());
  off = align_up(off, 8U); // i64 time axis needs 8B alignment

  const atx::u64 off_time_axis = off;
  off += t * sizeof(atx::i64);

  // v2: 64-byte-align the field-block section AND pad each block to a 64-byte
  // stride, so every block start is cache-line / AVX-512 aligned in the mapping.
  off = align_up(off, kBlockAlign);
  const atx::u64 off_field_blocks = off;
  const atx::u64 block_packed = t * static_cast<atx::u64>(n) * sizeof(atx::f64);
  const atx::u64 block_stride = align_up(block_packed, kBlockAlign);
  off += static_cast<atx::u64>(f) * block_stride;

  const atx::u64 off_present_bitmap = off;
  off += t * mw * sizeof(atx::u64);

  const atx::u64 off_footer = off;
  const atx::u64 total = off_footer + sizeof(SegmentFooter);

  // --- assemble the whole file in one buffer ---------------------------------
  std::vector<atx::u8> buf(static_cast<atx::usize>(total), atx::u8{0});

  // SAFETY: all `at` values are < total (computed above); source regions are
  // valid, non-overlapping, and within their respective owners' lifetimes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto put = [&buf](atx::u64 at, const void *src, atx::usize len) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::memcpy(buf.data() + at, src, len);
  };

  // Build header (content_hash filled after data sections are written).
  SegmentHeader h{};
  h.magic = kMagic;
  h.format_version = kFormatVersion;
  h.flags = kFlagSealed;
  h.total_bytes = total;
  h.field_count = f;
  h.instrument_count = n;
  h.time_count = t;
  h.created_at_nanos = created_at_nanos;
  h.off_field_dir = off_field_dir;
  h.off_symbol_table = off_symbol_table;
  h.off_string_blob = off_string_blob;
  h.off_time_axis = off_time_axis;
  h.off_field_blocks = off_field_blocks;
  h.off_present_bitmap = off_present_bitmap;
  h.off_footer = off_footer;

  // Field directory.
  for (atx::u32 i = 0; i < f; ++i) {
    FieldEntry e{};
    const std::string &name = field_names_[i];
    // SAFETY: e.name is a kFieldNameLen-byte C array (on-disk ABI); memcpy is
    // the correct way to fill it while leaving the remainder NUL-padded.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const atx::usize copy_len = name.size() < static_cast<atx::usize>(kFieldNameLen - 1U)
                                    ? name.size()
                                    : static_cast<atx::usize>(kFieldNameLen - 1U);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    std::memcpy(e.name, name.data(), copy_len);
    e.field_index = i;
    // e.reserved stays 0 (value-initialised above).
    put(off_field_dir + static_cast<atx::u64>(i) * sizeof(FieldEntry), &e, sizeof(e));
  }

  // Symbol table + string blob.
  if (n > 0U) {
    put(off_symbol_table, sym_entries.data(), sym_entries.size() * sizeof(SymbolEntry));
  }
  if (!blob.empty()) {
    put(off_string_blob, blob.data(), blob.size());
  }

  // Time axis.
  if (t > 0U) {
    put(off_time_axis, time_axis_.data(), t * sizeof(atx::i64));
  }

  // Field blocks: copy each field's packed T*N f64 to its 64B-padded slot. The
  // inter-block padding stays zero (buf is value-initialised), so content_hash
  // is deterministic.
  for (atx::u32 i = 0; i < f; ++i) {
    const atx::u64 src_elems = t * static_cast<atx::u64>(n);
    if (src_elems == 0U) {
      break; // no data to write (T or N is zero)
    }
    // SAFETY: src is blocks_[i*T*N .. +T*N) (packed in RAM); dst is the i-th
    // 64B-padded block in buf. Both ranges are in bounds by construction.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    put(off_field_blocks + static_cast<atx::u64>(i) * block_stride,
        blocks_.data() + static_cast<atx::usize>(static_cast<atx::u64>(i) * src_elems),
        static_cast<atx::usize>(src_elems) * sizeof(atx::f64));
  }

  // Present bitmap.
  if (!present_.empty()) {
    put(off_present_bitmap, present_.data(), present_.size() * sizeof(atx::u64));
  }

  // content_hash: crc over data sections [off_time_axis, off_footer).
  // SAFETY: [off_time_axis, off_footer) is a sub-range of buf (both < total).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const atx::u8 *data_begin = buf.data() + off_time_axis;
  h.content_hash =
      static_cast<atx::u64>(crc32(data_begin, static_cast<atx::usize>(off_footer - off_time_axis)));
  // Header is now final; write it.
  put(0, &h, sizeof(h));

  // integrity_crc: crc over everything before the footer.
  SegmentFooter footer{};
  footer.seal_marker = kSealMarker;
  footer.integrity_crc = crc32(buf.data(), static_cast<atx::usize>(off_footer));
  // footer.reserved stays 0.
  put(off_footer, &footer, sizeof(footer));

  // Write the complete buffer in one shot.
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot open for write: " + path);
  }
  // SAFETY: reinterpret u8* as char* for ofstream; both are single-byte types
  // and the cast is the standard idiom for binary I/O.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
  if (!out) {
    return Err(ErrorCode::IoError, "write failed: " + path);
  }
  return Ok();
}

} // namespace atx::tsdb
