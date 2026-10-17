// ATXVFA framing: builder, atomic publish, open + framing validation and the
// zero-copy lookups. Record-level validation (the part that knows the library's
// enums and the ExpiryRecord layout) is FitArchive::validate_records, defined in
// fit_archive_records.cpp. Spec: atx-vol/docs/storage/ARCHIVE_FORMAT.md.

#include "atx/vol/api/storage/fit_archive.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/tsdb/mapping.hpp"
#include "atx/vol/api/core/version.hpp"
#include "storage/archive_util.hpp" // crc32c, align_up, atomic publish

namespace atx::vol {
namespace {

static_assert(std::endian::native == std::endian::little,
              "ATXVFA stores little-endian values in place; big-endian hosts are unsupported");

using Type = FitArchiveSectionType;

struct SectionSpec {
  Type type;
  const char *name;
  std::uint32_t stride;     // this reader's element size
  std::uint32_t min_stride; // smallest admissible stored stride (the minor-0 size)
  std::uint32_t align;
  bool evolvable; // fixed-stride record type whose stride may grow by minor
};

constexpr std::array<SectionSpec, 25> kSpecs{{
    {Type::Symbols, "symbols", 48, 48, 8, false},
    {Type::Strings, "strings", 1, 1, 1, false},
    {Type::Provenance, "provenance", 1, 1, 1, false},
    {Type::SurfaceRecords, "surface-records", 192, 192, 8, true},
    {Type::SurfaceIndex, "surface-index", 24, 24, 8, false},
    {Type::FitRecords, "fit-records", 896, 896, 8, true},
    {Type::FitIndex, "fit-index", 24, 24, 8, false},
    {Type::PriceNodes, "price-nodes", 24, 24, 8, false},
    {Type::QuoteRows, "quote-rows", 48, 48, 8, true},
    {Type::MovedNodes, "moved-nodes", 1, 1, 1, false},
    {Type::Dividends, "dividends", 16, 16, 8, false},
    {Type::CarryPoints, "carry-points", 16, 16, 8, false},
    {Type::CurveWords, "curve-words", 8, 8, 8, false},
    {Type::ExpiryMeta, "expiry-meta", 128, 128, 8, true},
    {Type::ExpiryData, "expiry-data", kFitArchiveExpiryRecordBytes,
     kFitArchiveExpiryRecordBytes, 8, false},
    {Type::ExpiryIndex, "expiry-index", 24, 24, 8, false},
    {Type::Ladders, "ladders", 192, 192, 8, true},
    {Type::LadderIndex, "ladder-index", 24, 24, 8, false},
    {Type::LadderContracts, "ladder-contracts", 48, 48, 8, true},
    {Type::MarkovRecords, "markov-records", 192, 192, 8, true},
    {Type::MarkovIndex, "markov-index", 24, 24, 8, false},
    {Type::MarkovIntervals, "markov-intervals", 56, 56, 8, true},
    {Type::MarkovLocalVol, "markov-local-vol", 16, 16, 8, false},
    {Type::MarkovWords, "markov-words", 8, 8, 8, false},
    {Type::SeriesSegments, "series-segments", 128, 128, 8, true},
}};

[[nodiscard]] const SectionSpec *spec_of(std::uint32_t type) noexcept {
  for (const SectionSpec &s : kSpecs) {
    if (static_cast<std::uint32_t>(s.type) == type) return &s;
  }
  return nullptr;
}

constexpr std::uint64_t kHeaderBytes = sizeof(FitArchiveHeader);
constexpr std::uint64_t kEntryBytes = sizeof(FitArchiveSectionEntry);
constexpr std::uint64_t kFooterBytes = sizeof(FitArchiveFooter);

[[nodiscard]] std::uint32_t crc_of(std::span<const std::byte> b) noexcept {
  return detail::crc32c(b.data(), b.size());
}

template <class T> [[nodiscard]] bool all_zero(const T &bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](auto v) { return v == 0; });
}

[[nodiscard]] std::uint32_t header_crc(FitArchiveHeader h) noexcept {
  h.header_crc32c = 0;
  return crc_of(std::as_bytes(std::span{&h, 1}));
}

[[nodiscard]] std::uint32_t footer_crc(FitArchiveFooter f) noexcept {
  f.footer_crc32c = 0;
  return crc_of(std::as_bytes(std::span{&f, 1}));
}

[[nodiscard]] FitArchiveHeader make_header(const FitArchiveFileInfo &info, std::uint64_t file_size,
                                           std::uint32_t section_count) noexcept {
  FitArchiveHeader h{};
  h.magic = kFitArchiveMagic;
  h.format_major = kFitArchiveFormatMajor;
  h.format_minor = kFitArchiveFormatMinor;
  h.header_size = static_cast<std::uint16_t>(kHeaderBytes);
  h.section_entry_size = static_cast<std::uint16_t>(kEntryBytes);
  h.endian_tag = kFitArchiveEndianTag;
  h.footer_size = static_cast<std::uint32_t>(kFooterBytes);
  h.file_size = file_size;
  h.section_table_offset = kHeaderBytes;
  h.section_count = section_count;
  h.created_unix_ns = info.created_unix_ns;
  h.writer_version = static_cast<std::uint64_t>(ATX_VOL_VERSION);
  h.archive_id = info.archive_id;
  h.segment_sequence = info.segment_sequence;
  constexpr std::string_view kPrefix{"atx-vol "};
  std::size_t at = 0;
  for (const char c : kPrefix) h.writer_name[at++] = c;
  for (const char c : kVersionString) {
    if (at + 1 >= h.writer_name.size()) break;
    h.writer_name[at++] = c;
  }
  return h;
}

[[nodiscard]] ArchiveOutcome validate_inputs(std::span<const FitArchiveSectionInput> sections) noexcept {
  if (sections.size() > kFitArchiveMaxSections) {
    return archive_error(ArchiveStatus::InvalidArgument, kFitArchiveNone, sections.size());
  }
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const FitArchiveSectionInput &s = sections[i];
    const auto type = static_cast<std::uint32_t>(s.type);
    const bool align_ok = s.element_align == 1u || s.element_align == 8u;
    // Pool references are u32 element indices, so a section holds < 2^32 elements.
    if (s.element_stride == 0u || !align_ok || s.element_stride % s.element_align != 0u ||
        s.bytes.size() % s.element_stride != 0u ||
        s.bytes.size() / s.element_stride > std::numeric_limits<std::uint32_t>::max()) {
      return archive_error(ArchiveStatus::InvalidArgument, type, i);
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (sections[j].type == s.type) return archive_error(ArchiveStatus::DuplicateSection, type, i);
    }
  }
  return {};
}

} // namespace

const char *to_string(FitArchiveSectionType type) noexcept {
  const SectionSpec *s = spec_of(static_cast<std::uint32_t>(type));
  return s == nullptr ? "unknown" : s->name;
}

const char *to_string(ArchiveStatus status) noexcept {
  switch (status) {
  case ArchiveStatus::Ok: return "ok";
  case ArchiveStatus::InvalidArgument: return "invalid-argument";
  case ArchiveStatus::NotFound: return "not-found";
  case ArchiveStatus::IoError: return "io-error";
  case ArchiveStatus::Truncated: return "truncated";
  case ArchiveStatus::SizeMismatch: return "size-mismatch";
  case ArchiveStatus::BadMagic: return "bad-magic";
  case ArchiveStatus::UnsupportedEndian: return "unsupported-endian";
  case ArchiveStatus::UnsupportedMajor: return "unsupported-major";
  case ArchiveStatus::HeaderInvalid: return "header-invalid";
  case ArchiveStatus::HeaderChecksum: return "header-checksum";
  case ArchiveStatus::FooterInvalid: return "footer-invalid";
  case ArchiveStatus::FooterChecksum: return "footer-checksum";
  case ArchiveStatus::FooterMismatch: return "footer-mismatch";
  case ArchiveStatus::SectionTableChecksum: return "section-table-checksum";
  case ArchiveStatus::SectionBounds: return "section-bounds";
  case ArchiveStatus::SectionOverlap: return "section-overlap";
  case ArchiveStatus::SectionAlignment: return "section-alignment";
  case ArchiveStatus::SectionStride: return "section-stride";
  case ArchiveStatus::DuplicateSection: return "duplicate-section";
  case ArchiveStatus::MissingSection: return "missing-section";
  case ArchiveStatus::UnsupportedSection: return "unsupported-section";
  case ArchiveStatus::UnsupportedRecordVersion: return "unsupported-record-version";
  case ArchiveStatus::SectionChecksum: return "section-checksum";
  case ArchiveStatus::PaddingNotZero: return "padding-not-zero";
  case ArchiveStatus::FileChecksum: return "file-checksum";
  case ArchiveStatus::IndexUnsorted: return "index-unsorted";
  case ArchiveStatus::IndexBounds: return "index-bounds";
  case ArchiveStatus::RecordBounds: return "record-bounds";
  case ArchiveStatus::RecordInvalid: return "record-invalid";
  case ArchiveStatus::LayoutMismatch: return "layout-mismatch";
  case ArchiveStatus::Misaligned: return "misaligned";
  case ArchiveStatus::DuplicateKey: return "duplicate-key";
  case ArchiveStatus::SegmentMismatch: return "segment-mismatch";
  case ArchiveStatus::Busy: return "busy";
  }
  return "unknown";
}

std::uint32_t fit_archive_crc32c(std::span<const std::byte> bytes) noexcept {
  return crc_of(bytes);
}

ArchiveResult<std::vector<std::byte>>
build_fit_archive(std::span<const FitArchiveSectionInput> sections, const FitArchiveFileInfo &info) {
  if (ArchiveOutcome ok = validate_inputs(sections); !ok) return tl::unexpected(ok.error());
  const auto count = static_cast<std::uint32_t>(sections.size());
  const std::uint64_t table_end = kHeaderBytes + count * kEntryBytes;
  std::vector<FitArchiveSectionEntry> entries(count);
  std::uint64_t cursor = detail::align_up(table_end, kFitArchiveSectionAlign);
  for (std::uint32_t i = 0; i < count; ++i) {
    const FitArchiveSectionInput &s = sections[i];
    FitArchiveSectionEntry &e = entries[i];
    e.type = static_cast<std::uint32_t>(s.type);
    e.type_major = s.type_major;
    e.type_minor = s.type_minor;
    e.offset = cursor;
    e.length = s.bytes.size();
    e.element_count = s.bytes.size() / s.element_stride;
    e.element_stride = s.element_stride;
    e.element_align = s.element_align;
    e.flags = s.flags;
    e.crc32c = crc_of(s.bytes);
    cursor = detail::align_up(cursor + e.length, kFitArchiveSectionAlign);
  }
  const std::uint64_t footer_offset = cursor;
  const std::uint64_t file_size = footer_offset + kFooterBytes;
  std::vector<std::byte> out(static_cast<std::size_t>(file_size));
  const std::span<const std::byte> table = std::as_bytes(std::span{entries});
  if (!table.empty()) std::memcpy(out.data() + kHeaderBytes, table.data(), table.size());
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!sections[i].bytes.empty()) {
      std::memcpy(out.data() + entries[i].offset, sections[i].bytes.data(), sections[i].bytes.size());
    }
  }
  FitArchiveHeader h = make_header(info, file_size, count);
  h.section_table_crc32c = crc_of(table);
  h.header_crc32c = header_crc(h);
  std::memcpy(out.data(), &h, sizeof h);
  FitArchiveFooter f{};
  f.magic = kFitArchiveFooterMagic;
  f.header_offset = 0;
  f.file_size = file_size;
  f.section_table_offset = kHeaderBytes;
  f.section_count = count;
  f.header_crc32c = h.header_crc32c;
  f.file_crc32c = crc_of(std::span<const std::byte>{out.data(), static_cast<std::size_t>(footer_offset)});
  f.segment_sequence = info.segment_sequence;
  f.footer_crc32c = footer_crc(f);
  std::memcpy(out.data() + footer_offset, &f, sizeof f);
  return out;
}

ArchiveOutcome publish_fit_archive(std::string_view path, std::span<const std::byte> bytes) {
  auto reserved = detail::reserve_unique_publish_temp_file(path);
  if (!reserved) return archive_error(ArchiveStatus::IoError);
  const std::filesystem::path tmp{*reserved};
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (os) {
      // SAFETY: std::byte -> char view for ostream::write; char may alias any object.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      os.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!os) {
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      return archive_error(ArchiveStatus::IoError);
    }
  }
  if (!detail::flush_and_publish_file(tmp.string(), path)) return archive_error(ArchiveStatus::IoError);
  return {};
}

// ── Open ─────────────────────────────────────────────────────────────────────

ArchiveResult<FitArchive> FitArchive::open(std::string_view path, const FitArchiveOpenOptions &options) {
  const std::string p{path};
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) return archive_error(ArchiveStatus::NotFound);
  if (options.mode == FitArchiveOpenMode::Mapped) {
    auto mapped = atx::tsdb::Mapping::map_file_ro(p);
    if (!mapped) {
      const bool empty = mapped.error().code() == atx::core::ErrorCode::InvalidArgument;
      return archive_error(empty ? ArchiveStatus::Truncated : ArchiveStatus::IoError);
    }
    auto owner = std::make_shared<atx::tsdb::Mapping>(std::move(*mapped));
    // SAFETY: the mapping's u8 bytes viewed as std::byte (same size, any object
    // may be inspected through a byte type).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte *>(owner->base()),
                                           owner->size()};
    return open_impl(bytes, std::move(owner), options.verify);
  }
  const std::uintmax_t size = std::filesystem::file_size(p, ec);
  if (ec) return archive_error(ArchiveStatus::IoError);
  if (size == 0) return archive_error(ArchiveStatus::Truncated);
  auto buffer = std::make_shared<std::vector<std::uint64_t>>(static_cast<std::size_t>((size + 7u) / 8u));
  std::ifstream is(p, std::ios::binary);
  // SAFETY: u64 storage written through a char view (char may alias any object).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  is.read(reinterpret_cast<char *>(buffer->data()), static_cast<std::streamsize>(size));
  if (!is || static_cast<std::uintmax_t>(is.gcount()) != size) return archive_error(ArchiveStatus::IoError);
  const std::span<const std::byte> bytes =
      std::as_bytes(std::span<const std::uint64_t>{*buffer}).first(static_cast<std::size_t>(size));
  return open_impl(bytes, std::move(buffer), options.verify);
}

ArchiveResult<FitArchive> FitArchive::open_bytes(std::span<const std::byte> bytes,
                                                 std::shared_ptr<const void> owner,
                                                 FitArchiveVerify verify) {
  return open_impl(bytes, std::move(owner), verify);
}

ArchiveResult<FitArchive> FitArchive::open_copy(std::span<const std::byte> bytes, FitArchiveVerify verify) {
  auto buffer = std::make_shared<std::vector<std::uint64_t>>((bytes.size() + 7u) / 8u);
  if (!bytes.empty()) std::memcpy(buffer->data(), bytes.data(), bytes.size());
  const std::span<const std::byte> copy =
      std::as_bytes(std::span<const std::uint64_t>{*buffer}).first(bytes.size());
  return open_impl(copy, std::move(buffer), verify);
}

namespace {

[[nodiscard]] ArchiveOutcome check_header(const FitArchiveHeader &h, std::uint64_t n) noexcept {
  if (h.magic != kFitArchiveMagic) return archive_error(ArchiveStatus::BadMagic);
  if (h.endian_tag != kFitArchiveEndianTag) return archive_error(ArchiveStatus::UnsupportedEndian);
  if (h.format_major != kFitArchiveFormatMajor) {
    return archive_error(ArchiveStatus::UnsupportedMajor, kFitArchiveNone, h.format_major);
  }
  if (h.header_size != kHeaderBytes || h.section_entry_size != kEntryBytes ||
      h.footer_size != kFooterBytes) {
    return archive_error(ArchiveStatus::HeaderInvalid);
  }
  if (header_crc(h) != h.header_crc32c) return archive_error(ArchiveStatus::HeaderChecksum);
  if (h.flags != 0u || !all_zero(h.reserved) || h.section_table_offset != kHeaderBytes ||
      h.section_count > kFitArchiveMaxSections || h.file_size % kFitArchiveSectionAlign != 0u) {
    return archive_error(ArchiveStatus::HeaderInvalid);
  }
  if (n < h.file_size) return archive_error(ArchiveStatus::Truncated, kFitArchiveNone, n);
  if (n > h.file_size) return archive_error(ArchiveStatus::SizeMismatch, kFitArchiveNone, n);
  const std::uint64_t table_end = kHeaderBytes + std::uint64_t{h.section_count} * kEntryBytes;
  if (table_end + kFooterBytes > h.file_size) return archive_error(ArchiveStatus::HeaderInvalid);
  return {};
}

[[nodiscard]] ArchiveOutcome check_footer(const FitArchiveFooter &f, const FitArchiveHeader &h) noexcept {
  if (f.magic != kFitArchiveFooterMagic) return archive_error(ArchiveStatus::FooterInvalid);
  if (footer_crc(f) != f.footer_crc32c) return archive_error(ArchiveStatus::FooterChecksum);
  if (!all_zero(f.reserved)) return archive_error(ArchiveStatus::FooterInvalid);
  if (f.header_offset != 0u || f.file_size != h.file_size ||
      f.section_table_offset != h.section_table_offset || f.section_count != h.section_count ||
      f.header_crc32c != h.header_crc32c || f.segment_sequence != h.segment_sequence) {
    return archive_error(ArchiveStatus::FooterMismatch);
  }
  return {};
}

// One table entry against the payload area [payload_begin, footer_offset) and
// the previous section's end. Also enforces the known-type contract.
[[nodiscard]] ArchiveOutcome check_entry(const FitArchiveSectionEntry &e, std::size_t i,
                                         std::uint64_t payload_begin, std::uint64_t footer_offset,
                                         std::uint64_t previous_end) noexcept {
  const std::uint32_t t = e.type;
  if (!all_zero(e.reserved)) return archive_error(ArchiveStatus::HeaderInvalid, t, i);
  if (e.offset % kFitArchiveSectionAlign != 0u) return archive_error(ArchiveStatus::SectionAlignment, t, i);
  if (e.offset < payload_begin || e.offset > footer_offset || e.length > footer_offset - e.offset) {
    return archive_error(ArchiveStatus::SectionBounds, t, i);
  }
  if (e.offset < previous_end) return archive_error(ArchiveStatus::SectionOverlap, t, i);
  const bool align_ok = e.element_align == 1u || e.element_align == 8u;
  if (e.element_stride == 0u || !align_ok || e.element_stride % e.element_align != 0u) {
    return archive_error(ArchiveStatus::SectionAlignment, t, i);
  }
  if (e.length % e.element_stride != 0u || e.element_count != e.length / e.element_stride) {
    return archive_error(ArchiveStatus::SectionStride, t, i);
  }
  if ((e.flags & ~kFitArchiveSectionRequired) != 0u) {
    return archive_error(ArchiveStatus::UnsupportedSection, t, i);
  }
  const SectionSpec *spec = spec_of(t);
  if (spec == nullptr) {
    if ((e.flags & kFitArchiveSectionRequired) != 0u) {
      return archive_error(ArchiveStatus::UnsupportedSection, t, i);
    }
    return {};
  }
  if (e.type_major != 1u) return archive_error(ArchiveStatus::UnsupportedRecordVersion, t, e.type_major);
  if (e.element_align != spec->align) return archive_error(ArchiveStatus::SectionAlignment, t, i);
  if (e.element_stride != spec->stride) {
    if (spec->type == Type::ExpiryData) return archive_error(ArchiveStatus::LayoutMismatch, t, i);
    if (!spec->evolvable || e.element_stride < spec->min_stride) {
      return archive_error(ArchiveStatus::SectionStride, t, i);
    }
  }
  return {};
}

[[nodiscard]] ArchiveOutcome check_padding(std::span<const std::byte> bytes, std::uint64_t from,
                                           std::uint64_t to) noexcept {
  for (std::uint64_t at = from; at < to; ++at) {
    if (bytes[static_cast<std::size_t>(at)] != std::byte{0}) {
      return archive_error(ArchiveStatus::PaddingNotZero, kFitArchiveNone, at);
    }
  }
  return {};
}

} // namespace

ArchiveResult<FitArchive> FitArchive::open_impl(std::span<const std::byte> bytes,
                                                std::shared_ptr<const void> owner,
                                                FitArchiveVerify verify) {
  // SAFETY: address arithmetic only, to check alignment.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (reinterpret_cast<std::uintptr_t>(bytes.data()) % 8u != 0u) {
    return archive_error(ArchiveStatus::Misaligned);
  }
  const std::uint64_t n = bytes.size();
  if (n < kHeaderBytes + kFooterBytes) {
    const bool foreign = n >= kFitArchiveMagic.size() &&
                         std::memcmp(bytes.data(), kFitArchiveMagic.data(), kFitArchiveMagic.size()) != 0;
    return archive_error(foreign ? ArchiveStatus::BadMagic : ArchiveStatus::Truncated, kFitArchiveNone, n);
  }
  FitArchive a;
  std::memcpy(&a.header_, bytes.data(), sizeof a.header_);
  const FitArchiveHeader &h = a.header_;
  if (ArchiveOutcome ok = check_header(h, n); !ok) return tl::unexpected(ok.error());
  const std::uint64_t footer_offset = h.file_size - kFooterBytes;
  std::memcpy(&a.footer_, bytes.data() + footer_offset, sizeof a.footer_);
  if (ArchiveOutcome ok = check_footer(a.footer_, h); !ok) return tl::unexpected(ok.error());

  const std::uint64_t table_end = kHeaderBytes + std::uint64_t{h.section_count} * kEntryBytes;
  const std::span<const std::byte> table = bytes.subspan(kHeaderBytes, static_cast<std::size_t>(table_end - kHeaderBytes));
  if (crc_of(table) != h.section_table_crc32c) return archive_error(ArchiveStatus::SectionTableChecksum);
  a.sections_.resize(h.section_count);
  if (!table.empty()) std::memcpy(a.sections_.data(), table.data(), table.size());

  const std::uint64_t payload_begin = detail::align_up(table_end, kFitArchiveSectionAlign);
  std::uint64_t previous_end = payload_begin;
  for (std::size_t i = 0; i < a.sections_.size(); ++i) {
    const FitArchiveSectionEntry &e = a.sections_[i];
    if (ArchiveOutcome ok = check_entry(e, i, payload_begin, footer_offset, previous_end); !ok) {
      return tl::unexpected(ok.error());
    }
    previous_end = e.offset + e.length;
    if (spec_of(e.type) == nullptr) continue;
    Served &slot = a.served_[e.type];
    if (slot.table_index != kFitArchiveNone) return archive_error(ArchiveStatus::DuplicateSection, e.type, i);
    slot.table_index = static_cast<std::uint32_t>(i);
  }

  if (verify != FitArchiveVerify::Framing) {
    std::uint64_t cursor = table_end;
    for (std::size_t i = 0; i < a.sections_.size(); ++i) {
      const FitArchiveSectionEntry &e = a.sections_[i];
      const auto payload = bytes.subspan(static_cast<std::size_t>(e.offset), static_cast<std::size_t>(e.length));
      if (crc_of(payload) != e.crc32c) return archive_error(ArchiveStatus::SectionChecksum, e.type, i);
      if (ArchiveOutcome ok = check_padding(bytes, cursor, e.offset); !ok) return tl::unexpected(ok.error());
      cursor = e.offset + e.length;
    }
    if (ArchiveOutcome ok = check_padding(bytes, cursor, footer_offset); !ok) return tl::unexpected(ok.error());
  }
  if (verify == FitArchiveVerify::Paranoid &&
      crc_of(bytes.first(static_cast<std::size_t>(footer_offset))) != a.footer_.file_crc32c) {
    return archive_error(ArchiveStatus::FileChecksum);
  }

  for (std::size_t t = 0; t < kSlots; ++t) {
    Served &slot = a.served_[t];
    if (slot.table_index == kFitArchiveNone) continue;
    const FitArchiveSectionEntry &e = a.sections_[slot.table_index];
    const SectionSpec &spec = *spec_of(e.type);
    const auto stored = bytes.subspan(static_cast<std::size_t>(e.offset), static_cast<std::size_t>(e.length));
    slot.count = static_cast<std::size_t>(e.element_count);
    slot.stride = spec.stride;
    if (e.element_stride == spec.stride) {
      slot.bytes = stored;
      continue;
    }
    // Minor evolution: copy each element's known prefix into a zeroed element of
    // this reader's size (older minor: missing tail reads as zero; newer: ignored).
    const std::uint64_t served_bytes = e.element_count * spec.stride;
    auto &copy = a.upgraded_.emplace_back(static_cast<std::size_t>((served_bytes + 7u) / 8u), 0u);
    // SAFETY: u64 storage written through a byte view; the copy is only ever read
    // back through view<T>() for a trivially copyable T of stride spec.stride.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto *dst = reinterpret_cast<std::byte *>(copy.data());
    const std::size_t keep = std::min<std::size_t>(e.element_stride, spec.stride);
    for (std::uint64_t k = 0; k < e.element_count; ++k) {
      std::memcpy(dst + k * spec.stride, stored.data() + k * e.element_stride, keep);
    }
    slot.bytes = std::span<const std::byte>{dst, static_cast<std::size_t>(served_bytes)};
  }
  a.bytes_ = bytes;
  a.owner_ = std::move(owner);
  if (ArchiveOutcome ok = a.validate_records(); !ok) return tl::unexpected(ok.error());
  return a;
}

// ── Queries ──────────────────────────────────────────────────────────────────

std::size_t FitArchive::slot_of(FitArchiveSectionType type) const noexcept {
  const auto t = static_cast<std::size_t>(type);
  if (t >= kSlots || served_[t].table_index == kFitArchiveNone) return kNoSlot;
  return t;
}

const FitArchiveSectionEntry *FitArchive::section(FitArchiveSectionType type) const noexcept {
  const std::size_t slot = slot_of(type);
  return slot == kNoSlot ? nullptr : &sections_[served_[slot].table_index];
}

std::span<const std::byte> FitArchive::section_bytes(FitArchiveSectionType type) const noexcept {
  const std::size_t slot = slot_of(type);
  return slot == kNoSlot ? std::span<const std::byte>{} : served_[slot].bytes;
}

ArchiveOutcome FitArchive::verify_section(std::size_t table_index) const noexcept {
  if (table_index >= sections_.size()) return archive_error(ArchiveStatus::InvalidArgument);
  const FitArchiveSectionEntry &e = sections_[table_index];
  const auto payload = bytes_.subspan(static_cast<std::size_t>(e.offset), static_cast<std::size_t>(e.length));
  if (crc_of(payload) != e.crc32c) return archive_error(ArchiveStatus::SectionChecksum, e.type, table_index);
  return {};
}

ArchiveOutcome FitArchive::verify_file_checksum() const noexcept {
  const auto body = bytes_.first(static_cast<std::size_t>(header_.file_size - kFooterBytes));
  if (crc_of(body) != footer_.file_crc32c) return archive_error(ArchiveStatus::FileChecksum);
  return {};
}

namespace {

[[nodiscard]] bool symbol_less(const FitArchiveSymbol &s, std::string_view name) noexcept {
  const std::size_t n = std::min<std::size_t>(s.length, name.size());
  const int c = n == 0 ? 0 : std::memcmp(s.name.data(), name.data(), n);
  return c != 0 ? c < 0 : s.length < name.size();
}

template <class T> [[nodiscard]] std::span<const T> slice_of(std::span<const T> pool, FitArchiveArrayRef ref) noexcept {
  if (ref.first > pool.size() || ref.count > pool.size() - ref.first) return {};
  return pool.subspan(ref.first, ref.count);
}

[[nodiscard]] Type index_type_of(Type records) noexcept {
  switch (records) {
  case Type::SurfaceRecords: return Type::SurfaceIndex;
  case Type::FitRecords: return Type::FitIndex;
  case Type::ExpiryMeta: return Type::ExpiryIndex;
  case Type::Ladders: return Type::LadderIndex;
  case Type::MarkovRecords: return Type::MarkovIndex;
  default: return records;
  }
}

} // namespace

std::optional<std::uint32_t> FitArchive::find_underlier(std::string_view name) const noexcept {
  const auto syms = symbols();
  const auto it = std::lower_bound(syms.begin(), syms.end(), name, symbol_less);
  if (it == syms.end() || it->length != name.size() ||
      (!name.empty() && std::memcmp(it->name.data(), name.data(), name.size()) != 0)) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(it - syms.begin());
}

std::string_view FitArchive::underlier_name(std::uint32_t id) const noexcept {
  const auto syms = symbols();
  if (id >= syms.size()) return {};
  return std::string_view{syms[id].name.data(), std::min<std::size_t>(syms[id].length, kFitArchiveSymbolMax)};
}

std::string_view FitArchive::string(FitArchiveStringRef ref) const noexcept {
  const auto blob = section_bytes(FitArchiveSectionType::Strings);
  if (ref.offset > blob.size() || ref.length > blob.size() - ref.offset) return {};
  // SAFETY: std::byte -> char view of the string blob (char may alias any object).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return std::string_view{reinterpret_cast<const char *>(blob.data()) + ref.offset, ref.length};
}

std::span<const FitArchiveIndexEntry> FitArchive::index_of(FitArchiveSectionType records) const noexcept {
  const Type index = index_type_of(records);
  if (index == records) return {};
  return view<FitArchiveIndexEntry>(index);
}

template <class R>
const R *FitArchive::find_keyed(FitArchiveSectionType records, std::span<const R> rows,
                                const FitArchiveKey &key) const noexcept {
  const auto idx = index_of(records);
  const auto it = std::lower_bound(idx.begin(), idx.end(), key, [](const FitArchiveIndexEntry &e,
                                                                    const FitArchiveKey &k) {
    return fit_archive_key_less(e, k);
  });
  if (it == idx.end() || !fit_archive_key_equal(*it, key) || it->record >= rows.size()) return nullptr;
  return &rows[it->record];
}

const FitArchiveSurfaceRecord *FitArchive::find_surface(std::uint32_t underlier,
                                                        std::int64_t snapshot_ns) const noexcept {
  FitArchiveKey key{};
  key.underlier = underlier;
  key.snapshot_ns = snapshot_ns;
  return find_keyed(Type::SurfaceRecords, surfaces(), key);
}

const FitArchiveFitRecord *FitArchive::find_fit(const FitArchiveKey &key) const noexcept {
  return find_keyed(Type::FitRecords, fits(), key);
}

const FitArchiveExpiryMeta *FitArchive::find_expiry(const FitArchiveKey &key) const noexcept {
  return find_keyed(Type::ExpiryMeta, expiry_meta(), key);
}

const FitArchiveLadderRecord *FitArchive::find_ladder(const FitArchiveKey &key) const noexcept {
  return find_keyed(Type::Ladders, ladders(), key);
}

const FitArchiveMarkovRecord *FitArchive::find_markov(const FitArchiveKey &key) const noexcept {
  return find_keyed(Type::MarkovRecords, markov(), key);
}

std::span<const FitArchiveFitRecord> FitArchive::fits_of(const FitArchiveSurfaceRecord &s) const noexcept {
  return slice_of(fits(), FitArchiveArrayRef{s.first_fit, s.fit_count});
}

std::span<const FitArchiveQuoteRow> FitArchive::quote_rows(const FitArchiveFitRecord &fit) const noexcept {
  return slice_of(view<FitArchiveQuoteRow>(Type::QuoteRows), fit.rows);
}

std::span<const std::uint8_t> FitArchive::moved_nodes(const FitArchiveFitRecord &fit) const noexcept {
  return slice_of(view<std::uint8_t>(Type::MovedNodes), fit.moved_nodes);
}

std::span<const double> FitArchive::curve_payload(const FitArchiveFitRecord &fit) const noexcept {
  return slice_of(view<double>(Type::CurveWords), fit.curve.payload);
}

std::span<const FitArchiveLadderContract>
FitArchive::ladder_contracts(const FitArchiveLadderRecord &ladder) const noexcept {
  return slice_of(view<FitArchiveLadderContract>(Type::LadderContracts), ladder.contracts);
}

std::span<const FitArchiveExpiryMeta>
FitArchive::ladder_records(const FitArchiveLadderRecord &ladder) const noexcept {
  return slice_of(expiry_meta(), FitArchiveArrayRef{ladder.first_record, ladder.record_count});
}

std::span<const FitArchiveMarkovInterval>
FitArchive::markov_intervals(const FitArchiveMarkovRecord &record) const noexcept {
  return slice_of(view<FitArchiveMarkovInterval>(Type::MarkovIntervals), record.intervals);
}

std::span<const double> FitArchive::markov_words(FitArchiveArrayRef ref) const noexcept {
  return slice_of(view<double>(Type::MarkovWords), ref);
}

} // namespace atx::vol
