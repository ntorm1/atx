#include "atx/tsdb/segment_reader.hpp"

#include <algorithm>   // std::upper_bound
#include <optional>    // std::optional, std::nullopt
#include <span>        // std::span
#include <string>      // std::string
#include <string_view> // std::string_view
#include <utility>     // std::move

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/tsdb/checksum.hpp"
#include "atx/tsdb/mapping.hpp"
#include "atx/tsdb/segment.hpp"

namespace atx::tsdb {

using atx::core::Err;
using atx::core::ErrorCode;

namespace {

// Validate that a mapping holds a well-formed, sealed, intact segment. Returns
// an Error or std::nullopt on success.
[[nodiscard]] std::optional<atx::core::Error> validate(const Mapping &m) {
  if (m.size() < sizeof(SegmentHeader) + sizeof(SegmentFooter)) {
    return atx::core::Error{ErrorCode::InvalidArgument, "file too small for a segment"};
  }
  // SAFETY: size checked above; the first bytes are a candidate header read-only.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto &h = *reinterpret_cast<const SegmentHeader *>(m.base());
  if (h.magic != kMagic) {
    return atx::core::Error{ErrorCode::InvalidArgument, "bad magic (not an atx segment)"};
  }
  if (h.format_version != kFormatVersion) {
    return atx::core::Error{ErrorCode::InvalidArgument, "unsupported format version"};
  }
  if ((h.flags & kFlagSealed) == 0U) {
    return atx::core::Error{ErrorCode::InvalidArgument, "segment is not sealed"};
  }
  if (h.total_bytes != m.size() || h.off_footer + sizeof(SegmentFooter) != m.size()) {
    return atx::core::Error{ErrorCode::InvalidArgument, "size/offset mismatch"};
  }
  // SAFETY: off_footer + sizeof(SegmentFooter) == size (checked), so the footer
  // lies fully inside the mapping. Read-only.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto &f = *reinterpret_cast<const SegmentFooter *>(m.base() + h.off_footer);
  if (f.seal_marker != kSealMarker) {
    return atx::core::Error{ErrorCode::InvalidArgument, "missing seal marker"};
  }
  if (f.integrity_crc != crc32(m.base(), static_cast<atx::usize>(h.off_footer))) {
    return atx::core::Error{ErrorCode::Internal, "integrity crc mismatch (corrupt segment)"};
  }
  return std::nullopt;
}

} // namespace

atx::core::Result<SegmentReader> SegmentReader::attach(const std::string &path) {
  ATX_TRY(Mapping m, Mapping::map_file_ro(path));
  if (auto err = validate(m)) {
    return Err(std::move(*err));
  }
  return SegmentReader{std::move(m)};
}

std::optional<atx::u32> SegmentReader::field_index(std::string_view name) const noexcept {
  const SegmentHeader &h = header();
  // SAFETY: off_field_dir + F*sizeof(FieldEntry) <= off_footer by construction.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *dir = reinterpret_cast<const FieldEntry *>(map_.base() + h.off_field_dir);
  for (atx::u32 i = 0; i < h.field_count; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const FieldEntry &e = dir[i];
    // Name is NUL-padded in a kFieldNameLen buffer; bound the view at the NUL
    // (or the full capacity if unterminated) without an unbounded strlen.
    const std::string_view raw{e.name, kFieldNameLen};
    if (raw.substr(0, raw.find('\0')) == name) {
      return e.field_index;
    }
  }
  return std::nullopt;
}

std::string_view SegmentReader::symbol_name(atx::u32 inst) const noexcept {
  const SegmentHeader &h = header();
  ATX_ASSERT(inst < h.instrument_count);
  // SAFETY: symbol table holds N entries; inst < N (asserted). Read-only.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *tbl = reinterpret_cast<const SymbolEntry *>(map_.base() + h.off_symbol_table);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const SymbolEntry &e = tbl[inst];
  // SAFETY: (name_off, name_len) slices the string blob, both written in bounds.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *chars = reinterpret_cast<const char *>(map_.base() + h.off_string_blob + e.name_off);
  return std::string_view{chars, e.name_len};
}

std::optional<atx::u64> SegmentReader::cutoff_index(atx::i64 now_nanos) const noexcept {
  const std::span<const atx::i64> axis = times();
  // First element strictly greater than now; the one before it is the cutoff.
  const auto it = std::upper_bound(axis.begin(), axis.end(), now_nanos);
  if (it == axis.begin()) {
    return std::nullopt; // nothing visible yet
  }
  return static_cast<atx::u64>((it - axis.begin()) - 1);
}

} // namespace atx::tsdb
