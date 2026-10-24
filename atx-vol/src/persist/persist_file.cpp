// PersistFile (ATXVPF): builder, durable publish, open + validation and the lazy
// per-section verdicts. Spec: atx-vol/docs/storage/PERSIST_FORMAT.md; the framing and
// the validation order are ATXVFA's (src/storage/fit_archive.cpp), plus kind, schema
// fingerprint, per-section dtype / codec / raw CRC, the Lazy level and a 4096-byte
// file pad (design wave3-persist D2-D7).

#include "atx/vol/api/persist/persist_file.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "persist/persist_internal.hpp"
#include "storage/archive_util.hpp" // crc32c, align_up, temp + publish

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace atx::vol {
namespace {

static_assert(std::endian::native == std::endian::little,
              "ATXVPF stores little-endian values in place; big-endian hosts are unsupported");
static_assert(sizeof(void *) == 8, "ATXVPF offsets are 64-bit and mapped whole: LP64 only");

using persist_detail::Block64;

constexpr std::uint64_t kHeaderBytes = sizeof(PersistHeader);
constexpr std::uint64_t kEntryBytes = sizeof(PersistSectionEntry);
constexpr std::uint64_t kFooterBytes = sizeof(PersistFooter);
// Mapping admission ceiling: far above any state / segment, below address-space folly.
constexpr std::uint64_t kMaxFileBytes = 1ull << 40;

constexpr std::uint8_t kUnchecked = 0;
constexpr std::uint8_t kVerified = 1;
constexpr std::uint8_t kFailed = 2;

[[nodiscard]] std::uint32_t crc_of(std::span<const std::byte> b) noexcept {
  return detail::crc32c(b.data(), b.size());
}

template <class T> [[nodiscard]] bool all_zero(const T &bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](auto v) { return v == 0; });
}

[[nodiscard]] std::uint32_t header_crc(PersistHeader h) noexcept {
  h.header_crc32c = 0;
  return crc_of(std::as_bytes(std::span{&h, 1}));
}

[[nodiscard]] std::uint32_t footer_crc(PersistFooter f) noexcept {
  f.footer_crc32c = 0;
  return crc_of(std::as_bytes(std::span{&f, 1}));
}

[[nodiscard]] bool kind_known(std::uint32_t kind) noexcept {
  return kind >= static_cast<std::uint32_t>(PersistKind::Manifest) &&
         kind <= static_cast<std::uint32_t>(PersistKind::RiskRun);
}

[[nodiscard]] bool scalar(PersistDType d) noexcept { return persist_dtype_size(d) != 0; }

// Writer-side contract of one input (stride / dtype / codec / level).
[[nodiscard]] PersistOutcome check_input(const PersistSectionInput &s, std::size_t i) noexcept {
  const auto dtype = static_cast<std::uint8_t>(s.dtype);
  const auto codec = static_cast<std::uint8_t>(s.codec);
  const auto filter = static_cast<std::uint8_t>(s.filter);
  if (dtype >= kPersistDTypeCount || codec >= kPersistCodecCount || filter >= kPersistFilterCount ||
      s.stride == 0 || (scalar(s.dtype) && s.stride != persist_dtype_size(s.dtype)) ||
      s.bytes.size() % s.stride != 0u) {
    return persist_error(PersistStatus::InvalidArgument, s.type, i);
  }
  if (s.codec == PersistCodec::Zstd && (s.level < 1 || s.level > 19)) {
    return persist_error(PersistStatus::InvalidArgument, s.type, i);
  }
  return {};
}

[[nodiscard]] PersistOutcome check_inputs(std::span<const PersistSectionInput> sections,
                                          const PersistFileInfo &info) noexcept {
  if (sections.size() > kPersistMaxSections || !kind_known(static_cast<std::uint32_t>(info.kind)) ||
      info.writer.size() > kPersistWriterNameMax || info.ts_min_ns > info.ts_max_ns) {
    return persist_error(PersistStatus::InvalidArgument, kPersistNone, sections.size());
  }
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (PersistOutcome ok = check_input(sections[i], i); !ok) return ok;
    for (std::size_t j = 0; j < i; ++j) {
      if (sections[j].type == sections[i].type) {
        return persist_error(PersistStatus::DuplicateSection, sections[i].type, i);
      }
    }
  }
  return {};
}

[[nodiscard]] PersistHeader make_header(const PersistFileInfo &info, std::uint64_t file_size,
                                        std::uint32_t count) noexcept {
  PersistHeader h{};
  h.magic = kPersistMagic;
  h.format_major = kPersistFormatMajor;
  h.format_minor = kPersistFormatMinor;
  h.header_size = static_cast<std::uint16_t>(kHeaderBytes);
  h.section_entry_size = static_cast<std::uint16_t>(kEntryBytes);
  h.endian_tag = kPersistEndianTag;
  h.footer_size = static_cast<std::uint32_t>(kFooterBytes);
  h.file_size = file_size;
  h.section_table_offset = kHeaderBytes;
  h.section_count = count;
  h.kind = static_cast<std::uint32_t>(info.kind);
  h.schema_fingerprint = info.schema_fingerprint;
  h.source_fingerprint = info.source_fingerprint;
  h.created_unix_ns = info.created_unix_ns;
  h.ts_min_ns = info.ts_min_ns;
  h.ts_max_ns = info.ts_max_ns;
  h.writer_version = static_cast<std::uint64_t>(ATX_VOL_VERSION);
  std::size_t at = 0;
  if (!info.writer.empty()) {
    for (const char c : info.writer) h.writer_name[at++] = c;
  } else {
    for (const char c : std::string_view{"atx-vol "}) h.writer_name[at++] = c;
    for (const char c : kVersionString) {
      if (at + 1 >= h.writer_name.size()) break;
      h.writer_name[at++] = c;
    }
  }
  return h;
}

// The stored bytes of every codec section, encoded on up to `n_threads` threads into
// disjoint slots (bytes identical for any thread count).
[[nodiscard]] PersistResult<std::vector<std::vector<std::byte>>>
encode_codec_sections(std::span<const PersistSectionInput> sections, unsigned n_threads) {
  std::vector<std::vector<std::byte>> encoded(sections.size());
  std::vector<std::uint8_t> failed(sections.size(), 0);
  std::vector<std::size_t> todo;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (sections[i].codec != PersistCodec::None) todo.push_back(i);
  }
  if (todo.empty()) return encoded;
  pricing_executor().run_blocks(todo.size(), std::max(1u, n_threads), [&](std::size_t k) {
    const PersistSectionInput &s = sections[todo[k]];
    auto frame = persist_detail::encode_zstd(s.bytes, s.stride, s.filter, s.level);
    if (frame) {
      encoded[todo[k]] = std::move(*frame);
    } else {
      failed[todo[k]] = 1;
    }
  });
  for (const std::size_t i : todo) {
    if (failed[i] != 0) return persist_error(PersistStatus::CodecError, sections[i].type, i);
  }
  return encoded;
}

} // namespace

// ── Vocabulary ───────────────────────────────────────────────────────────────

const char *to_string(PersistStatus status) noexcept {
  switch (status) {
  case PersistStatus::Ok: return "ok";
  case PersistStatus::InvalidArgument: return "invalid-argument";
  case PersistStatus::NotFound: return "not-found";
  case PersistStatus::IoError: return "io-error";
  case PersistStatus::AlreadyExists: return "already-exists";
  case PersistStatus::Busy: return "busy";
  case PersistStatus::Truncated: return "truncated";
  case PersistStatus::SizeMismatch: return "size-mismatch";
  case PersistStatus::BadMagic: return "bad-magic";
  case PersistStatus::UnsupportedEndian: return "unsupported-endian";
  case PersistStatus::UnsupportedMajor: return "unsupported-major";
  case PersistStatus::WrongKind: return "wrong-kind";
  case PersistStatus::SchemaMismatch: return "schema-mismatch";
  case PersistStatus::HeaderInvalid: return "header-invalid";
  case PersistStatus::HeaderChecksum: return "header-checksum";
  case PersistStatus::FooterInvalid: return "footer-invalid";
  case PersistStatus::FooterChecksum: return "footer-checksum";
  case PersistStatus::FooterMismatch: return "footer-mismatch";
  case PersistStatus::SectionTableChecksum: return "section-table-checksum";
  case PersistStatus::SectionBounds: return "section-bounds";
  case PersistStatus::SectionOverlap: return "section-overlap";
  case PersistStatus::SectionAlignment: return "section-alignment";
  case PersistStatus::SectionStride: return "section-stride";
  case PersistStatus::DuplicateSection: return "duplicate-section";
  case PersistStatus::MissingSection: return "missing-section";
  case PersistStatus::UnsupportedSection: return "unsupported-section";
  case PersistStatus::UnsupportedSectionMajor: return "unsupported-section-major";
  case PersistStatus::SectionChecksum: return "section-checksum";
  case PersistStatus::CodecError: return "codec-error";
  case PersistStatus::RawChecksum: return "raw-checksum";
  case PersistStatus::PaddingNotZero: return "padding-not-zero";
  case PersistStatus::FileChecksum: return "file-checksum";
  case PersistStatus::RecordInvalid: return "record-invalid";
  case PersistStatus::IndexUnsorted: return "index-unsorted";
  case PersistStatus::IndexBounds: return "index-bounds";
  case PersistStatus::Misaligned: return "misaligned";
  case PersistStatus::DuplicateKey: return "duplicate-key";
  case PersistStatus::SegmentMismatch: return "segment-mismatch";
  case PersistStatus::StaleSource: return "stale-source";
  }
  return "unknown";
}

const char *to_string(PersistKind kind) noexcept {
  switch (kind) {
  case PersistKind::Manifest: return "manifest";
  case PersistKind::MarketState: return "market-state";
  case PersistKind::SurfaceTimeline: return "surface-timeline";
  case PersistKind::RiskRun: return "risk-run";
  }
  return "unknown";
}

Status to_status(const PersistError &error) {
  ErrorCode code = ErrorCode::ParseError;
  switch (error.status) {
  case PersistStatus::InvalidArgument: code = ErrorCode::InvalidArgument; break;
  case PersistStatus::NotFound: code = ErrorCode::NotFound; break;
  case PersistStatus::IoError: code = ErrorCode::IoError; break;
  case PersistStatus::AlreadyExists: code = ErrorCode::AlreadyExists; break;
  case PersistStatus::Busy: code = ErrorCode::Unavailable; break;
  default: break; // every integrity refusal is a parse error
  }
  std::string msg = std::string("persist: ") + to_string(error.status);
  if (error.section != kPersistNone) msg += " (section " + std::to_string(error.section) + ")";
  if (error.detail != 0) msg += " detail " + std::to_string(error.detail);
  return Err(code, std::move(msg));
}

std::uint32_t persist_crc32c(std::span<const std::byte> bytes) noexcept { return crc_of(bytes); }

// ── Builder ──────────────────────────────────────────────────────────────────

PersistResult<std::vector<std::byte>>
build_persist_file(std::span<const PersistSectionInput> sections, const PersistFileInfo &info,
                   unsigned n_threads) {
  if (PersistOutcome ok = check_inputs(sections, info); !ok) return tl::unexpected(ok.error());
  auto encoded = encode_codec_sections(sections, n_threads);
  if (!encoded) return tl::unexpected(encoded.error());
  const auto count = static_cast<std::uint32_t>(sections.size());
  const std::uint64_t table_end = kHeaderBytes + std::uint64_t{count} * kEntryBytes;
  std::vector<PersistSectionEntry> entries(count);
  std::vector<std::span<const std::byte>> stored(count);
  std::uint64_t cursor = detail::align_up(table_end, kPersistSectionAlign);
  for (std::uint32_t i = 0; i < count; ++i) {
    const PersistSectionInput &s = sections[i];
    const bool codec = s.codec != PersistCodec::None;
    stored[i] = codec ? std::span<const std::byte>{(*encoded)[i]} : s.bytes;
    PersistSectionEntry &e = entries[i];
    e.type = s.type;
    e.type_major = s.type_major;
    e.type_minor = s.type_minor;
    e.offset = cursor;
    e.length = stored[i].size();
    e.raw_length = s.bytes.size();
    e.element_count = s.bytes.size() / s.stride;
    e.element_stride = s.stride;
    e.flags = s.required ? kPersistSectionRequired : 0u;
    e.raw_crc32c = crc_of(s.bytes);
    e.crc32c = codec ? crc_of(stored[i]) : e.raw_crc32c;
    e.dtype = static_cast<std::uint8_t>(s.dtype);
    e.codec = static_cast<std::uint8_t>(s.codec);
    e.filter = codec ? static_cast<std::uint8_t>(s.filter) : std::uint8_t{0};
    cursor = detail::align_up(cursor + e.length, kPersistSectionAlign);
  }
  const std::uint64_t file_size = detail::align_up(cursor + kFooterBytes, kPersistFileAlign);
  const std::uint64_t footer_offset = file_size - kFooterBytes;
  std::vector<std::byte> out(static_cast<std::size_t>(file_size));
  const std::span<const std::byte> table = std::as_bytes(std::span{entries});
  if (!table.empty()) std::memcpy(out.data() + kHeaderBytes, table.data(), table.size());
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!stored[i].empty()) {
      std::memcpy(out.data() + entries[i].offset, stored[i].data(), stored[i].size());
    }
  }
  PersistHeader h = make_header(info, file_size, count);
  h.section_table_crc32c = crc_of(table);
  h.header_crc32c = header_crc(h);
  std::memcpy(out.data(), &h, sizeof h);
  PersistFooter f{};
  f.magic = kPersistFooterMagic;
  f.file_size = file_size;
  f.section_table_offset = kHeaderBytes;
  f.section_count = count;
  f.kind = h.kind;
  f.schema_fingerprint = h.schema_fingerprint;
  f.header_crc32c = h.header_crc32c;
  f.file_crc32c =
      crc_of(std::span<const std::byte>{out.data(), static_cast<std::size_t>(footer_offset)});
  f.footer_crc32c = footer_crc(f);
  std::memcpy(out.data() + footer_offset, &f, sizeof f);
  return out;
}

namespace persist_detail {

PersistOutcome replace_file(std::string_view path, std::span<const std::byte> bytes) {
  auto reserved = detail::reserve_unique_publish_temp_file(path);
  if (!reserved) return persist_error(PersistStatus::IoError);
  const std::filesystem::path tmp{*reserved};
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (os) {
      // SAFETY: std::byte -> char view for ostream::write; char may alias any object.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      os.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    }
    if (!os) {
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      return persist_error(PersistStatus::IoError);
    }
  }
  if (!detail::flush_and_publish_file(tmp.string(), path)) {
    return persist_error(PersistStatus::IoError);
  }
  return {};
}

} // namespace persist_detail

PersistOutcome publish_persist_file(std::string_view path, std::span<const std::byte> bytes) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (std::filesystem::exists(p, ec)) return persist_error(PersistStatus::AlreadyExists);
  if (!std::filesystem::is_directory(p.parent_path().empty() ? "." : p.parent_path(), ec)) {
    return persist_error(PersistStatus::IoError);
  }
  return persist_detail::replace_file(path, bytes);
}

// ── Reader ───────────────────────────────────────────────────────────────────

namespace {

// Everything an open file holds. PersistFile::Impl derives from it so the free
// validation helpers below can fill it (the nested Impl name is private).
struct FileCore {
  std::shared_ptr<const void> owner;
  std::span<const std::byte> bytes;
  PersistHeader header{};
  PersistFooter footer{};
  PersistVerify verify{PersistVerify::Full};
  std::vector<PersistSectionEntry> entries;
  std::vector<std::span<const std::byte>> served;              // per table index
  std::vector<std::pair<std::uint32_t, std::uint32_t>> by_type; // (type, table index), sorted
  // The one shared mutable datum: kUnchecked -> kVerified / kFailed, set by the first
  // touch (or the open). Racing first touches compute the same CRC of the same
  // immutable bytes and store the same verdict, so the race is benign; seq_cst.
  std::unique_ptr<std::atomic<std::uint8_t>[]> state;
  std::vector<std::vector<Block64>> decoded; // codec sections' owned buffers

  [[nodiscard]] std::size_t index_of(std::uint32_t type) const noexcept {
    const auto it = std::lower_bound(by_type.begin(), by_type.end(), type,
                                     [](const auto &p, std::uint32_t t) { return p.first < t; });
    return it == by_type.end() || it->first != type ? entries.size() : it->second;
  }
  [[nodiscard]] std::span<const std::byte> stored(std::size_t i) const noexcept {
    return bytes.subspan(static_cast<std::size_t>(entries[i].offset),
                         static_cast<std::size_t>(entries[i].length));
  }
  // Verify (once) the stored CRC of table index i; true when it matches.
  [[nodiscard]] bool verdict(std::size_t i) const noexcept {
    std::uint8_t s = state[i].load();
    if (s == kUnchecked) {
      s = crc_of(stored(i)) == entries[i].crc32c ? kVerified : kFailed;
      state[i].store(s);
    }
    return s == kVerified;
  }
};

[[nodiscard]] PersistOutcome check_header(const PersistHeader &h, std::uint64_t n) noexcept {
  using S = PersistStatus;
  if (h.magic != kPersistMagic) return persist_error(S::BadMagic);
  if (h.endian_tag != kPersistEndianTag) return persist_error(S::UnsupportedEndian);
  if (h.format_major != kPersistFormatMajor) {
    return persist_error(S::UnsupportedMajor, kPersistNone, h.format_major);
  }
  if (h.header_size != kHeaderBytes || h.section_entry_size != kEntryBytes ||
      h.footer_size != kFooterBytes) {
    return persist_error(S::HeaderInvalid);
  }
  if (header_crc(h) != h.header_crc32c) return persist_error(S::HeaderChecksum);
  if (h.flags != 0u || h.reserved0 != 0u || !all_zero(h.reserved) ||
      h.section_table_offset != kHeaderBytes || h.section_count > kPersistMaxSections ||
      h.file_size % kPersistFileAlign != 0u || h.file_size == 0u || h.ts_min_ns > h.ts_max_ns) {
    return persist_error(S::HeaderInvalid);
  }
  if (n < h.file_size) return persist_error(S::Truncated, kPersistNone, n);
  if (n > h.file_size) return persist_error(S::SizeMismatch, kPersistNone, n);
  const std::uint64_t table_end = kHeaderBytes + std::uint64_t{h.section_count} * kEntryBytes;
  if (table_end + kFooterBytes > h.file_size) return persist_error(S::HeaderInvalid);
  return {};
}

[[nodiscard]] PersistOutcome check_footer(const PersistFooter &f, const PersistHeader &h) noexcept {
  using S = PersistStatus;
  if (f.magic != kPersistFooterMagic) return persist_error(S::FooterInvalid);
  if (footer_crc(f) != f.footer_crc32c) return persist_error(S::FooterChecksum);
  if (f.reserved0 != 0u || f.reserved1 != 0u) return persist_error(S::FooterInvalid);
  if (f.file_size != h.file_size || f.section_table_offset != h.section_table_offset ||
      f.section_count != h.section_count || f.kind != h.kind ||
      f.schema_fingerprint != h.schema_fingerprint || f.header_crc32c != h.header_crc32c) {
    return persist_error(S::FooterMismatch);
  }
  return {};
}

// One table entry against the payload area [payload_begin, footer_offset) and the
// previous section's end.
[[nodiscard]] PersistOutcome check_entry(const PersistSectionEntry &e, std::size_t i,
                                         std::uint64_t payload_begin, std::uint64_t footer_offset,
                                         std::uint64_t previous_end) noexcept {
  using S = PersistStatus;
  const std::uint32_t t = e.type;
  if (e.reserved0 != 0u || e.reserved1 != 0u) return persist_error(S::HeaderInvalid, t, i);
  if (e.offset % kPersistSectionAlign != 0u) return persist_error(S::SectionAlignment, t, i);
  if (e.offset < payload_begin || e.offset > footer_offset || e.length > footer_offset - e.offset) {
    return persist_error(S::SectionBounds, t, i);
  }
  if (e.offset < previous_end) return persist_error(S::SectionOverlap, t, i);
  if (e.dtype >= kPersistDTypeCount || e.codec >= kPersistCodecCount ||
      e.filter >= kPersistFilterCount || (e.flags & ~kPersistSectionRequired) != 0u) {
    return persist_error(S::UnsupportedSection, t, i);
  }
  const bool codec = e.codec != static_cast<std::uint8_t>(PersistCodec::None);
  if (!codec && e.filter != 0u) return persist_error(S::HeaderInvalid, t, i);
  const auto dtype = static_cast<PersistDType>(e.dtype);
  if (e.element_stride == 0u || (scalar(dtype) && e.element_stride != persist_dtype_size(dtype)) ||
      e.raw_length % e.element_stride != 0u || e.element_count != e.raw_length / e.element_stride) {
    return persist_error(S::SectionStride, t, i);
  }
  if (!codec && e.length != e.raw_length) return persist_error(S::SectionStride, t, i);
  if (!codec && e.crc32c != e.raw_crc32c) return persist_error(S::HeaderInvalid, t, i);
  return {};
}

[[nodiscard]] PersistOutcome check_padding(std::span<const std::byte> bytes, std::uint64_t from,
                                           std::uint64_t to) noexcept {
  for (std::uint64_t at = from; at < to; ++at) {
    if (bytes[static_cast<std::size_t>(at)] != std::byte{0}) {
      return persist_error(PersistStatus::PaddingNotZero, kPersistNone, at);
    }
  }
  return {};
}

// Decode every codec section into an owned 64-byte aligned buffer and verify both of
// its CRCs (codec sections are always verified at open: nothing allocates later).
[[nodiscard]] PersistOutcome decode_sections(FileCore &a) {
  std::vector<std::byte> scratch;
  for (std::size_t i = 0; i < a.entries.size(); ++i) {
    const PersistSectionEntry &e = a.entries[i];
    if (e.codec == static_cast<std::uint8_t>(PersistCodec::None)) continue;
    if (crc_of(a.stored(i)) != e.crc32c) {
      return persist_error(PersistStatus::SectionChecksum, e.type, i);
    }
    if (e.raw_length > std::numeric_limits<std::size_t>::max() - 64u) {
      return persist_error(PersistStatus::CodecError, e.type, i);
    }
    auto &buffer = a.decoded.emplace_back(static_cast<std::size_t>((e.raw_length + 63u) / 64u));
    const std::span<std::byte> out =
        std::as_writable_bytes(std::span{buffer}).first(static_cast<std::size_t>(e.raw_length));
    if (PersistOutcome ok = persist_detail::decode_zstd(a.stored(i), e.element_stride,
                                                        static_cast<PersistFilter>(e.filter), out,
                                                        scratch);
        !ok) {
      return persist_error(PersistStatus::CodecError, e.type, i);
    }
    if (crc_of(out) != e.raw_crc32c) return persist_error(PersistStatus::RawChecksum, e.type, i);
    a.served[i] = out;
    a.state[i].store(kVerified);
  }
  return {};
}

// Full / Paranoid: every stored CRC and every padding byte, in file order.
[[nodiscard]] PersistOutcome verify_payloads(const FileCore &a, std::uint64_t table_end,
                                             std::uint64_t footer_offset) noexcept {
  std::uint64_t cursor = table_end;
  for (std::size_t i = 0; i < a.entries.size(); ++i) {
    const PersistSectionEntry &e = a.entries[i];
    if (!a.verdict(i)) return persist_error(PersistStatus::SectionChecksum, e.type, i);
    if (PersistOutcome ok = check_padding(a.bytes, cursor, e.offset); !ok) return ok;
    cursor = e.offset + e.length;
  }
  return check_padding(a.bytes, cursor, footer_offset);
}

// Validate `bytes` into `out` (a fresh FileCore). The first refusal wins.
[[nodiscard]] PersistOutcome validate(FileCore &out, std::span<const std::byte> bytes,
                                      std::shared_ptr<const void> owner, PersistKind kind,
                                      std::uint64_t fingerprint, PersistVerify verify) {
  using S = PersistStatus;
  // SAFETY: address arithmetic only, to check alignment.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (reinterpret_cast<std::uintptr_t>(bytes.data()) % 8u != 0u) {
    return persist_error(S::Misaligned);
  }
  const std::uint64_t n = bytes.size();
  if (n < kHeaderBytes + kFooterBytes) {
    const bool foreign = n >= kPersistMagic.size() &&
                         std::memcmp(bytes.data(), kPersistMagic.data(), kPersistMagic.size()) != 0;
    return persist_error(foreign ? S::BadMagic : S::Truncated, kPersistNone, n);
  }
  FileCore *a = &out;
  a->verify = verify;
  std::memcpy(&a->header, bytes.data(), sizeof a->header);
  const PersistHeader &h = a->header;
  if (PersistOutcome ok = check_header(h, n); !ok) return tl::unexpected(ok.error());
  if (h.kind != static_cast<std::uint32_t>(kind)) {
    return persist_error(S::WrongKind, kPersistNone, h.kind);
  }
  if (h.schema_fingerprint != fingerprint) return persist_error(S::SchemaMismatch);
  const std::uint64_t footer_offset = h.file_size - kFooterBytes;
  std::memcpy(&a->footer, bytes.data() + footer_offset, sizeof a->footer);
  if (PersistOutcome ok = check_footer(a->footer, h); !ok) return tl::unexpected(ok.error());

  const std::uint64_t table_end = kHeaderBytes + std::uint64_t{h.section_count} * kEntryBytes;
  const auto table =
      bytes.subspan(kHeaderBytes, static_cast<std::size_t>(table_end - kHeaderBytes));
  if (crc_of(table) != h.section_table_crc32c) return persist_error(S::SectionTableChecksum);
  a->entries.resize(h.section_count);
  if (!table.empty()) std::memcpy(a->entries.data(), table.data(), table.size());

  const std::uint64_t payload_begin = detail::align_up(table_end, kPersistSectionAlign);
  std::uint64_t previous_end = payload_begin;
  for (std::size_t i = 0; i < a->entries.size(); ++i) {
    const PersistSectionEntry &e = a->entries[i];
    if (PersistOutcome ok = check_entry(e, i, payload_begin, footer_offset, previous_end); !ok) {
      return tl::unexpected(ok.error());
    }
    previous_end = e.offset + e.length;
    a->by_type.emplace_back(e.type, static_cast<std::uint32_t>(i));
  }
  std::sort(a->by_type.begin(), a->by_type.end());
  for (std::size_t k = 1; k < a->by_type.size(); ++k) {
    if (a->by_type[k].first == a->by_type[k - 1].first) {
      return persist_error(S::DuplicateSection, a->by_type[k].first, a->by_type[k].second);
    }
  }
  a->bytes = bytes;
  a->owner = std::move(owner);
  a->state = std::make_unique<std::atomic<std::uint8_t>[]>(a->entries.size());
  a->served.resize(a->entries.size());
  for (std::size_t i = 0; i < a->entries.size(); ++i) a->served[i] = a->stored(i);
  if (PersistOutcome ok = decode_sections(*a); !ok) return tl::unexpected(ok.error());
  if (verify == PersistVerify::Full || verify == PersistVerify::Paranoid) {
    if (PersistOutcome ok = verify_payloads(*a, table_end, footer_offset); !ok) {
      return tl::unexpected(ok.error());
    }
  }
  if (verify == PersistVerify::Paranoid &&
      crc_of(bytes.first(static_cast<std::size_t>(footer_offset))) != a->footer.file_crc32c) {
    return persist_error(S::FileChecksum);
  }
  return {};
}

struct ReadBacking {
  std::shared_ptr<std::vector<Block64>> buffer;
  std::span<const std::byte> bytes;
};

// One sized sequential read into an owned 64-byte aligned buffer (Read mode).
[[nodiscard]] PersistResult<ReadBacking> read_whole(const std::filesystem::path &p,
                                                    std::uint64_t size) {
  ReadBacking r;
  r.buffer = std::make_shared<std::vector<Block64>>(static_cast<std::size_t>((size + 63u) / 64u));
  std::byte *dst = std::as_writable_bytes(std::span{*r.buffer}).data();
#if defined(_WIN32)
  // FILE_SHARE_DELETE: a reader never blocks an atomic republish (manifest rename).
  HANDLE h = ::CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    const bool absent = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    return persist_error(absent ? PersistStatus::NotFound : PersistStatus::IoError);
  }
  std::uint64_t done = 0;
  bool ok = true;
  while (ok && done < size) {
    const auto want = static_cast<DWORD>(std::min<std::uint64_t>(size - done, 1u << 30));
    DWORD got = 0;
    ok = ::ReadFile(h, dst + done, want, &got, nullptr) != 0 && got == want;
    done += got;
  }
  ::CloseHandle(h);
  if (!ok) return persist_error(PersistStatus::IoError);
#else
  std::ifstream is(p, std::ios::binary);
  // SAFETY: byte storage written through a char view (char may alias any object).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  is.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(size));
  if (!is || static_cast<std::uint64_t>(is.gcount()) != size) {
    return persist_error(PersistStatus::IoError);
  }
#endif
  r.bytes = std::span<const std::byte>{dst, static_cast<std::size_t>(size)};
  return r;
}

} // namespace

struct PersistFile::Impl final : FileCore {};

PersistFile::PersistFile(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
PersistFile::PersistFile(PersistFile &&) noexcept = default;
PersistFile &PersistFile::operator=(PersistFile &&) noexcept = default;
PersistFile::~PersistFile() = default;

PersistResult<PersistFile> PersistFile::open(std::string_view path, PersistKind kind,
                                             std::uint64_t fingerprint,
                                             const PersistOpenOptions &options) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) return persist_error(PersistStatus::NotFound);
  const std::uintmax_t size = std::filesystem::file_size(p, ec);
  if (ec) return persist_error(PersistStatus::IoError);
  if (options.expected_size != 0 && size != options.expected_size) {
    return persist_error(PersistStatus::SizeMismatch, kPersistNone, size);
  }
  if (size < kHeaderBytes + kFooterBytes) {
    // Too short to map usefully; read it so the refusal is the reader's (BadMagic /
    // Truncated), never an OS mapping error.
    if (size == 0) return persist_error(PersistStatus::Truncated);
  } else if (size > kMaxFileBytes) {
    return persist_error(PersistStatus::SizeMismatch, kPersistNone, size);
  }
  if (options.mapped && size >= kHeaderBytes + kFooterBytes) {
    auto mapped = atx::tsdb::Mapping::map_file_ro(p.string(), size, size);
    if (!mapped) return persist_error(PersistStatus::IoError);
    auto owner = std::make_shared<atx::tsdb::Mapping>(std::move(*mapped));
    if (options.prefetch) owner->prefetch();
    // SAFETY: the mapping's u8 bytes viewed as std::byte (same size; any object may be
    // inspected through a byte type).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte *>(owner->base()),
                                           owner->size()};
    auto impl = std::make_unique<Impl>();
    if (PersistOutcome ok =
            validate(*impl, bytes, std::move(owner), kind, fingerprint, options.verify);
        !ok) {
      return tl::unexpected(ok.error());
    }
    return PersistFile{std::move(impl)};
  }
  auto backing = read_whole(p, size);
  if (!backing) return tl::unexpected(backing.error());
  auto impl = std::make_unique<Impl>();
  if (PersistOutcome ok = validate(*impl, backing->bytes, std::move(backing->buffer), kind,
                                   fingerprint, options.verify);
      !ok) {
    return tl::unexpected(ok.error());
  }
  return PersistFile{std::move(impl)};
}

PersistResult<PersistFile> PersistFile::open_bytes(std::span<const std::byte> bytes,
                                                   std::shared_ptr<const void> owner,
                                                   PersistKind kind, std::uint64_t fingerprint,
                                                   PersistVerify verify) {
  auto impl = std::make_unique<Impl>();
  if (PersistOutcome ok = validate(*impl, bytes, std::move(owner), kind, fingerprint, verify);
      !ok) {
    return tl::unexpected(ok.error());
  }
  return PersistFile{std::move(impl)};
}

const PersistHeader &PersistFile::header() const noexcept { return impl_->header; }
const PersistFooter &PersistFile::footer() const noexcept { return impl_->footer; }
std::span<const PersistSectionEntry> PersistFile::sections() const noexcept {
  return impl_->entries;
}
std::span<const std::byte> PersistFile::bytes() const noexcept { return impl_->bytes; }
std::shared_ptr<const void> PersistFile::keepalive() const noexcept { return impl_->owner; }

const PersistSectionEntry *PersistFile::entry(std::uint32_t type) const noexcept {
  const std::size_t i = impl_->index_of(type);
  return i == impl_->entries.size() ? nullptr : &impl_->entries[i];
}

PersistResult<std::span<const std::byte>> PersistFile::section(std::uint32_t type) const noexcept {
  const Impl &a = *impl_;
  const std::size_t i = a.index_of(type);
  if (i == a.entries.size()) return persist_error(PersistStatus::MissingSection, type);
  // Framing never checks payloads on touch; Full / Paranoid verified them at open.
  if (a.verify == PersistVerify::Lazy && !a.verdict(i)) {
    return persist_error(PersistStatus::SectionChecksum, type, i);
  }
  return a.served[i];
}

PersistOutcome
PersistFile::check_schema(std::span<const PersistSectionSpec> schema) const noexcept {
  for (const PersistSectionSpec &spec : schema) {
    const PersistSectionEntry *e = entry(spec.type);
    if (e == nullptr) {
      if (spec.required) return persist_error(PersistStatus::MissingSection, spec.type);
      continue;
    }
    if (e->type_major != spec.type_major) {
      return persist_error(PersistStatus::UnsupportedSectionMajor, spec.type, e->type_major);
    }
    if (e->dtype != static_cast<std::uint8_t>(spec.dtype) || e->element_stride != spec.stride) {
      return persist_error(PersistStatus::SectionStride, spec.type, e->element_stride);
    }
  }
  for (const PersistSectionEntry &e : impl_->entries) {
    const bool known = std::any_of(schema.begin(), schema.end(),
                                   [&](const PersistSectionSpec &s) { return s.type == e.type; });
    if (!known && (e.flags & kPersistSectionRequired) != 0u) {
      return persist_error(PersistStatus::UnsupportedSection, e.type);
    }
  }
  return {};
}

PersistOutcome PersistFile::verify_all() const noexcept {
  const Impl &a = *impl_;
  for (std::size_t i = 0; i < a.entries.size(); ++i) {
    if (!a.verdict(i)) return persist_error(PersistStatus::SectionChecksum, a.entries[i].type, i);
  }
  const std::uint64_t footer_offset = a.header.file_size - kFooterBytes;
  if (crc_of(a.bytes.first(static_cast<std::size_t>(footer_offset))) != a.footer.file_crc32c) {
    return persist_error(PersistStatus::FileChecksum);
  }
  return {};
}

} // namespace atx::vol
