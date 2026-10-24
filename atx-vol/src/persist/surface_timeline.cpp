// SurfaceTimeline (kind SurfaceTimeline, `.atxstl`): verbatim ATXVSA2 surface records
// behind a (uid, ts) index, one PersistFile per period (design wave3-persist D12; layout
// spec docs/storage/PERSIST_HISTORY_FORMAT.md).
//
// Sections (all type_major 1, all required):
//   1 Meta          TimelineMeta, 1 row (64 B)
//   2 UidDirectory  TimelineUid, one row per uid, ascending uid (24 B)
//   3 Strings       the tickers, concatenated (no terminators)
//   4 Entries       TimelineEntry, sorted by (uid, ts_ns), strictly ascending (48 B)
//   5 Records       the records, each at a 64-byte multiple, zero padded between
//
// The records are copied byte for byte from the source partitions, so a view built
// here runs PricedSurfaceView::create_over_record over the same bytes the partition's
// map_symbol does: bit-exact by construction, never by tolerance. The timeline's
// integrity unit is the RECORD: under the default Lazy open, the four index sections
// are CRC-verified on their first touch (at open) and the Records section never as a
// whole; every view() verifies its own record's ATXVSA2 payload CRC instead, so opening
// a year of history costs the index, not the payload.

#include "atx/vol/api/persist/surface_timeline.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"
#include "atx/vol/api/storage/surface_db.hpp"
#include "storage/archive_util.hpp"

namespace atx::vol {
namespace {

static_assert(std::endian::native == std::endian::little,
              "SurfaceTimeline is little-endian only (persist D3)");

namespace fs = std::filesystem;
using S = PersistStatus;

constexpr auto kMetaSection = static_cast<std::uint32_t>(SurfaceTimelineSection::Meta);
constexpr auto kUidSection = static_cast<std::uint32_t>(SurfaceTimelineSection::UidDirectory);
constexpr auto kStringsSection = static_cast<std::uint32_t>(SurfaceTimelineSection::Strings);
constexpr auto kEntriesSection = static_cast<std::uint32_t>(SurfaceTimelineSection::Entries);
constexpr auto kRecordsSection = static_cast<std::uint32_t>(SurfaceTimelineSection::Records);

// Records start on a cache line inside the Records section (itself 64-byte aligned in
// the file), so every column of a record keeps the 8-byte alignment the view needs.
constexpr std::uint64_t kRecordAlign = 64;

// ── On-disk records private to this kind (TimelineEntry is public) ─────────────

struct TimelineMeta {
  std::uint64_t n_entries{};
  std::uint64_t records_bytes{};
  std::int64_t ts_min_ns{};
  std::int64_t ts_max_ns{};
  std::uint32_t n_uids{};
  std::uint32_t n_sources{};      // partitions folded in (informational)
  std::uint32_t flags{};          // 0 in v1
  std::uint32_t reserved0{};
  std::uint64_t source_fingerprint{}; // == header().source_fingerprint
  std::uint64_t reserved1{};
};
static_assert(kPersistPod<TimelineMeta> && sizeof(TimelineMeta) == 64);

struct TimelineUid {
  std::uint32_t uid{};
  std::uint32_t entry_count{};    // >= 1
  std::uint64_t entry_begin{};    // index into Entries; rows tile [0, n_entries) in uid order
  std::uint32_t ticker_offset{};  // into Strings
  std::uint32_t ticker_len{};
};
static_assert(kPersistPod<TimelineUid> && sizeof(TimelineUid) == 24);

constexpr PersistFieldSpec kMetaFields[] = {
    {"n_entries", offsetof(TimelineMeta, n_entries), 8},
    {"records_bytes", offsetof(TimelineMeta, records_bytes), 8},
    {"ts_min_ns", offsetof(TimelineMeta, ts_min_ns), 8},
    {"ts_max_ns", offsetof(TimelineMeta, ts_max_ns), 8},
    {"n_uids", offsetof(TimelineMeta, n_uids), 4},
    {"n_sources", offsetof(TimelineMeta, n_sources), 4},
    {"flags", offsetof(TimelineMeta, flags), 4},
    {"reserved0", offsetof(TimelineMeta, reserved0), 4},
    {"source_fingerprint", offsetof(TimelineMeta, source_fingerprint), 8},
    {"reserved1", offsetof(TimelineMeta, reserved1), 8},
};
static_assert(persist_fields_tile(kMetaFields, sizeof(TimelineMeta)));

constexpr PersistFieldSpec kUidFields[] = {
    {"uid", offsetof(TimelineUid, uid), 4},
    {"entry_count", offsetof(TimelineUid, entry_count), 4},
    {"entry_begin", offsetof(TimelineUid, entry_begin), 8},
    {"ticker_offset", offsetof(TimelineUid, ticker_offset), 4},
    {"ticker_len", offsetof(TimelineUid, ticker_len), 4},
};
static_assert(persist_fields_tile(kUidFields, sizeof(TimelineUid)));

constexpr PersistFieldSpec kEntryFields[] = {
    {"uid", offsetof(TimelineEntry, uid), 4},
    {"n_slices", offsetof(TimelineEntry, n_slices), 2},
    {"flags", offsetof(TimelineEntry, flags), 2},
    {"ts_ns", offsetof(TimelineEntry, ts_ns), 8},
    {"record_offset", offsetof(TimelineEntry, record_offset), 8},
    {"record_size", offsetof(TimelineEntry, record_size), 4},
    {"record_crc32c", offsetof(TimelineEntry, record_crc32c), 4},
    {"source_fingerprint", offsetof(TimelineEntry, source_fingerprint), 8},
    {"reserved", offsetof(TimelineEntry, reserved), 8},
};
static_assert(persist_fields_tile(kEntryFields, sizeof(TimelineEntry)));

constexpr PersistSectionSpec kSchema[] = {
    {kMetaSection, 1, PersistDType::Pod, sizeof(TimelineMeta), "meta", kMetaFields, true},
    {kUidSection, 1, PersistDType::Pod, sizeof(TimelineUid), "uid-directory", kUidFields, true},
    {kStringsSection, 1, PersistDType::Bytes, 1, "strings", {}, true},
    {kEntriesSection, 1, PersistDType::Pod, sizeof(TimelineEntry), "entries", kEntryFields, true},
    {kRecordsSection, 1, PersistDType::Bytes, 1, "records", {}, true},
};
constexpr std::uint64_t kFingerprint =
    persist_fingerprint(persist_salt(kSurfaceTimelineSalt), kSchema);

// ── Small helpers ─────────────────────────────────────────────────────────────

// House Result errors (SurfaceArchiveV2 / SurfaceDb) in the persist currency.
[[nodiscard]] tl::unexpected<PersistError> from_core(const atx::core::Error &e,
                                                     std::uint32_t section) noexcept {
  const ErrorCode c = e.code();
  if (c == ErrorCode::NotFound) return persist_error(S::NotFound, section);
  if (c == ErrorCode::IoError) return persist_error(S::IoError, section);
  if (c == ErrorCode::InvalidArgument) return persist_error(S::InvalidArgument, section);
  if (c == ErrorCode::AlreadyExists) return persist_error(S::AlreadyExists, section);
  return persist_error(S::RecordInvalid, section);
}

// ATXVSA2's record checksum: CRC-32C over the whole record with its own
// payload_crc32c field read as zero (surface_archive.cpp record_crc_v2; spec
// docs/atxvsa2-format.md §3.1). Piecewise, no copy. Precondition: the record is at
// least one ArchiveV2SurfaceHeader long.
[[nodiscard]] std::uint32_t record_crc(std::span<const std::byte> rec) noexcept {
  constexpr std::size_t at = offsetof(ArchiveV2SurfaceHeader, payload_crc32c);
  constexpr std::array<std::byte, sizeof(std::uint32_t)> kZero{};
  std::uint32_t c = detail::crc32c_update(0xFFFFFFFFu, rec.data(), at);
  c = detail::crc32c_update(c, kZero.data(), kZero.size());
  c = detail::crc32c_update(c, rec.data() + at + kZero.size(), rec.size() - at - kZero.size());
  return c ^ 0xFFFFFFFFu;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
  return persist_detail::fold_le(h, v, 8);
}

// A partition's content identity as one nonzero u64 (0 means "no source").
[[nodiscard]] std::uint64_t identity_fingerprint(const ArchiveContentIdentity &id) noexcept {
  std::uint64_t h = persist_detail::kFnvOffset;
  h = mix(h, id.file_size);
  h = mix(h, id.created_ts_ns);
  h = mix(h, id.header_crc32c);
  h = mix(h, id.metadata_crc32c);
  h = persist_detail::fmix64(h);
  return h == 0 ? 1 : h;
}

// "YYYY-MM" of a UTC instant (the period a snapshot routes to). Days from civil
// (H. Hinnant's algorithm), floor division so pre-1970 instants route correctly.
[[nodiscard]] PersistResult<std::string> period_key(std::int64_t ts_ns) {
  constexpr std::int64_t kDayNs = 86'400'000'000'000;
  std::int64_t days = ts_ns / kDayNs;
  if (ts_ns % kDayNs < 0) --days;
  days += 719'468;
  const std::int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
  const std::int64_t doe = days - era * 146'097;
  const std::int64_t yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  if (year < 0 || year > 9'999) return persist_error(S::InvalidArgument, kPersistNone, 0);
  std::array<char, 16> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%04lld-%02lld",
                              static_cast<long long>(year), static_cast<long long>(month));
  if (n != 7) return persist_error(S::InvalidArgument);
  return std::string(buf.data(), 7);
}

// ── Build side ────────────────────────────────────────────────────────────────

// One source partition, read whole into an owned 8-byte aligned buffer. A positional
// read (FILE_SHARE_DELETE) rather than a mapping: a mapped partition cannot be
// replaced on Windows, and a timeline build must never block a SurfaceDb republish.
struct LoadedPartition {
  std::vector<std::uint64_t> buffer;
  std::size_t size{0};
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return std::as_bytes(std::span<const std::uint64_t>{buffer}).first(size);
  }
};

[[nodiscard]] PersistResult<LoadedPartition> load_partition(std::string_view path) {
  auto f = detail::PositionalFile::open(path);
  if (!f) return from_core(f.error(), kRecordsSection);
  LoadedPartition p;
  if (f->size() == 0 || f->size() > std::numeric_limits<std::size_t>::max() - 8u) {
    return persist_error(S::RecordInvalid, kRecordsSection, f->size());
  }
  p.size = static_cast<std::size_t>(f->size());
  p.buffer.resize((p.size + 7u) / 8u);
  std::byte *dst = std::as_writable_bytes(std::span<std::uint64_t>{p.buffer}).data();
  if (atx::core::Status st = f->read_at(0, dst, p.size); !st) {
    return from_core(st.error(), kRecordsSection);
  }
  return p;
}

// One record to fold into a period file. Spans borrow a LoadedPartition or an open
// TimelineCore that the builder keeps alive until the image is framed.
struct RecordItem {
  std::uint32_t uid{};
  std::int64_t ts_ns{};
  std::uint16_t n_slices{};
  std::uint16_t kind_bits{};
  std::string_view ticker{};
  std::span<const std::byte> record{};
  std::uint32_t crc{};
  std::uint64_t source_fingerprint{};
};

// Every record of one partition, fully verified (metadata at open, every record's CRC,
// framing and symbol binding through validate_all). `ts_override == 0` keeps each
// record's own PricingContext instant.
[[nodiscard]] PersistResult<std::uint64_t> append_partition(std::span<const std::byte> bytes,
                                                            std::int64_t ts_override,
                                                            std::vector<RecordItem> &out) {
  auto arch = SurfaceArchiveV2::open_borrowed(bytes, nullptr);
  if (!arch) return from_core(arch.error(), kRecordsSection);
  if (atx::core::Status st = arch->validate_all(); !st) {
    return from_core(st.error(), kRecordsSection);
  }
  const std::uint64_t fp = identity_fingerprint(arch->identity());
  for (const ArchiveV2DirEntry &e : arch->entries()) {
    // validate_all proved this extent lies inside `bytes` and frames one record.
    const auto rec = bytes.subspan(static_cast<std::size_t>(e.surface_offset),
                                   static_cast<std::size_t>(e.surface_size));
    ArchiveV2SurfaceHeader h;
    std::memcpy(&h, rec.data(), sizeof h);
    if (h.n_slices > std::numeric_limits<std::uint16_t>::max() ||
        h.symbol_len > kArchiveSymbolMax) {
      return persist_error(S::RecordInvalid, kRecordsSection, e.surface_offset);
    }
    RecordItem it;
    it.uid = h.uid;
    it.ts_ns = ts_override != 0 ? ts_override : h.now_ts_ns;
    it.n_slices = static_cast<std::uint16_t>(h.n_slices);
    it.kind_bits = h.kind_bits;
    // The ticker is read from the record itself, so it lives as long as the bytes.
    // SAFETY: `symbol` is a char array inside the record's header extent.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    it.ticker = std::string_view{reinterpret_cast<const char *>(rec.data()) +
                                     offsetof(ArchiveV2SurfaceHeader, symbol),
                                 h.symbol_len};
    it.record = rec;
    it.crc = h.payload_crc32c;
    it.source_fingerprint = fp;
    out.push_back(it);
  }
  return fp;
}

struct ImageInfo {
  std::uint32_t n_sources{0};
  std::uint64_t source_fingerprint{0};
  std::int64_t created_unix_ns{0};
};

// The index of one period: entries in (uid, ts) order, the uid directory, the ticker
// strings and the Records section length.
struct TimelineIndex {
  std::vector<TimelineEntry> entries;
  std::vector<TimelineUid> dir;
  std::string strings;
  std::uint64_t records_bytes{0};
};

// Sort `items` by (uid, ts) and lay out the index: (uid, ts) unique, ONE ticker per uid,
// records on the 64-byte grid in entry order (so a uid's history is contiguous on disk).
[[nodiscard]] PersistResult<TimelineIndex> index_items(std::vector<RecordItem> &items) {
  std::sort(items.begin(), items.end(), [](const RecordItem &a, const RecordItem &b) {
    return a.uid != b.uid ? a.uid < b.uid : a.ts_ns < b.ts_ns;
  });
  TimelineIndex ix;
  ix.entries.resize(items.size());
  std::uint64_t at = 0;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const RecordItem &it = items[i];
    if (i > 0 && items[i - 1].uid == it.uid && items[i - 1].ts_ns == it.ts_ns) {
      return persist_error(S::DuplicateKey, kEntriesSection, it.uid);
    }
    if (ix.dir.empty() || ix.dir.back().uid != it.uid) {
      if (ix.strings.size() + it.ticker.size() > std::numeric_limits<std::uint32_t>::max()) {
        return persist_error(S::InvalidArgument, kStringsSection);
      }
      ix.dir.push_back(TimelineUid{it.uid, 0, i, static_cast<std::uint32_t>(ix.strings.size()),
                                   static_cast<std::uint32_t>(it.ticker.size())});
      ix.strings.append(it.ticker);
    } else if (std::string_view{ix.strings}.substr(ix.dir.back().ticker_offset,
                                                   ix.dir.back().ticker_len) != it.ticker) {
      // One uid, two tickers: the uid space of the sources disagrees, and a history
      // keyed on uid would silently splice two underliers together.
      return persist_error(S::RecordInvalid, kStringsSection, it.uid);
    }
    ++ix.dir.back().entry_count;
    at = detail::align_up(at, kRecordAlign);
    TimelineEntry &e = ix.entries[i];
    e.uid = it.uid;
    e.n_slices = it.n_slices;
    e.flags = it.kind_bits;
    e.ts_ns = it.ts_ns;
    e.record_offset = at;
    e.record_size = static_cast<std::uint32_t>(it.record.size()); // ATXVSA2 caps records < 4 GiB
    e.record_crc32c = it.crc;
    e.source_fingerprint = it.source_fingerprint;
    at += it.record.size();
  }
  if (at > std::numeric_limits<std::size_t>::max()) return persist_error(S::InvalidArgument);
  ix.records_bytes = at;
  return ix;
}

// Frame `items` into one period image.
[[nodiscard]] PersistResult<std::vector<std::byte>> build_image(std::vector<RecordItem> items,
                                                                const ImageInfo &info) {
  if (items.empty()) return persist_error(S::InvalidArgument);
  auto index = index_items(items);
  if (!index) return tl::unexpected(index.error());
  const TimelineIndex &ix = *index;
  std::vector<std::byte> records(static_cast<std::size_t>(ix.records_bytes));
  for (std::size_t i = 0; i < items.size(); ++i) {
    std::memcpy(records.data() + ix.entries[i].record_offset, items[i].record.data(),
                items[i].record.size());
  }
  TimelineMeta meta;
  meta.n_entries = ix.entries.size();
  meta.records_bytes = ix.records_bytes;
  meta.ts_min_ns = std::numeric_limits<std::int64_t>::max();
  meta.ts_max_ns = std::numeric_limits<std::int64_t>::min();
  for (const TimelineEntry &e : ix.entries) {
    meta.ts_min_ns = std::min(meta.ts_min_ns, e.ts_ns);
    meta.ts_max_ns = std::max(meta.ts_max_ns, e.ts_ns);
  }
  meta.n_uids = static_cast<std::uint32_t>(ix.dir.size());
  meta.n_sources = info.n_sources;
  meta.source_fingerprint = info.source_fingerprint;

  const auto pod = [](std::uint32_t type, std::uint32_t stride, std::span<const std::byte> b) {
    return PersistSectionInput{type, 1, 0, PersistDType::Pod, stride, true,
                               PersistCodec::None, PersistFilter::None, 3, b};
  };
  const auto blob = [](std::uint32_t type, std::span<const std::byte> b) {
    return PersistSectionInput{type, 1, 0, PersistDType::Bytes, 1, true,
                               PersistCodec::None, PersistFilter::None, 3, b};
  };
  const PersistSectionInput inputs[] = {
      pod(kMetaSection, static_cast<std::uint32_t>(sizeof(TimelineMeta)),
          std::as_bytes(std::span{&meta, 1})),
      pod(kUidSection, static_cast<std::uint32_t>(sizeof(TimelineUid)),
          std::as_bytes(std::span{ix.dir})),
      blob(kStringsSection, std::as_bytes(std::span{ix.strings.data(), ix.strings.size()})),
      pod(kEntriesSection, static_cast<std::uint32_t>(sizeof(TimelineEntry)),
          std::as_bytes(std::span{ix.entries})),
      blob(kRecordsSection, std::span<const std::byte>{records}),
  };
  PersistFileInfo fi;
  fi.kind = PersistKind::SurfaceTimeline;
  fi.schema_fingerprint = kFingerprint;
  fi.created_unix_ns = info.created_unix_ns;
  fi.ts_min_ns = meta.ts_min_ns;
  fi.ts_max_ns = meta.ts_max_ns;
  fi.source_fingerprint = info.source_fingerprint;
  return build_persist_file(inputs, fi);
}

// ── Read side ─────────────────────────────────────────────────────────────────

// A validated period file: the container plus typed spans over its index sections.
// Movable (spans point into the PersistFile's backing, which a move does not touch).
struct TimelineCore {
  explicit TimelineCore(PersistFile f) noexcept : file{std::move(f)} {}

  PersistFile file;
  TimelineMeta meta{};
  std::span<const TimelineUid> dir{};
  std::span<const char> strings{};
  std::span<const TimelineEntry> entries{};
  std::span<const std::byte> records{};
  std::vector<std::uint32_t> uids{}; // dir[i].uid, dense for the binary search

  // The bounds- and grid-checked extent of `e` (it may come from anywhere).
  [[nodiscard]] PersistResult<std::span<const std::byte>>
  record(const TimelineEntry &e) const noexcept {
    if (e.record_offset % kRecordAlign != 0u || e.record_size < sizeof(ArchiveV2SurfaceHeader) ||
        e.record_offset > records.size() || e.record_size > records.size() - e.record_offset) {
      return persist_error(S::IndexBounds, kRecordsSection, e.record_offset);
    }
    return records.subspan(static_cast<std::size_t>(e.record_offset), e.record_size);
  }
  [[nodiscard]] std::string_view ticker(const TimelineUid &d) const noexcept {
    return std::string_view{strings.data() + d.ticker_offset, d.ticker_len};
  }
};

template <class T>
[[nodiscard]] PersistResult<std::span<const T>> typed_section(const PersistFile &f,
                                                              std::uint32_t type) noexcept {
  return f.template typed<T>(type);
}

// The Records payload WITHOUT a whole-section CRC (the timeline verifies per record):
// the stored extent when uncompressed (bounds proven by the framing pass), else the
// decoded buffer (decoded and verified at open by the container).
[[nodiscard]] PersistResult<std::span<const std::byte>> records_of(const PersistFile &f) noexcept {
  const PersistSectionEntry *e = f.entry(kRecordsSection);
  if (e == nullptr) return persist_error(S::MissingSection, kRecordsSection);
  if (e->codec != static_cast<std::uint8_t>(PersistCodec::None)) return f.section(kRecordsSection);
  return f.bytes().subspan(static_cast<std::size_t>(e->offset),
                           static_cast<std::size_t>(e->length));
}

// Structural pass over the index: O(n_entries), touches no record byte.
[[nodiscard]] PersistOutcome check_index(const TimelineCore &c) noexcept {
  std::uint64_t next = 0;
  for (std::size_t i = 0; i < c.dir.size(); ++i) {
    const TimelineUid &d = c.dir[i];
    if (i > 0 && c.dir[i - 1].uid >= d.uid) return persist_error(S::IndexUnsorted, kUidSection, i);
    if (d.entry_begin != next || d.entry_count == 0 ||
        d.entry_count > c.entries.size() - d.entry_begin ||
        d.ticker_offset > c.strings.size() || d.ticker_len > c.strings.size() - d.ticker_offset) {
      return persist_error(S::IndexBounds, kUidSection, i);
    }
    next += d.entry_count;
  }
  if (next != c.entries.size()) return persist_error(S::IndexBounds, kUidSection, next);
  std::uint64_t record_end = 0;
  for (const TimelineUid &d : c.dir) {
    const auto rows = c.entries.subspan(static_cast<std::size_t>(d.entry_begin), d.entry_count);
    for (std::size_t k = 0; k < rows.size(); ++k) {
      const TimelineEntry &e = rows[k];
      const std::uint64_t at = d.entry_begin + k;
      if (e.uid != d.uid || e.reserved != 0u || e.ts_ns < c.meta.ts_min_ns ||
          e.ts_ns > c.meta.ts_max_ns) {
        return persist_error(S::RecordInvalid, kEntriesSection, at);
      }
      if (k > 0 && rows[k - 1].ts_ns >= e.ts_ns) {
        return persist_error(S::IndexUnsorted, kEntriesSection, at);
      }
      if (!c.record(e) || e.record_offset < record_end) {
        return persist_error(S::IndexBounds, kEntriesSection, at);
      }
      record_end = e.record_offset + e.record_size;
    }
  }
  return {};
}

[[nodiscard]] PersistResult<TimelineCore> open_core(std::string_view path,
                                                    const PersistOpenOptions &options) {
  auto file = PersistFile::open(path, PersistKind::SurfaceTimeline, kFingerprint, options);
  if (!file) return tl::unexpected(file.error());
  TimelineCore c{std::move(*file)};
  if (PersistOutcome ok = c.file.check_schema(kSchema); !ok) return tl::unexpected(ok.error());
  auto meta = typed_section<TimelineMeta>(c.file, kMetaSection);
  if (!meta) return tl::unexpected(meta.error());
  if (meta->size() != 1u) return persist_error(S::SectionStride, kMetaSection, meta->size());
  c.meta = meta->front();
  auto dir = typed_section<TimelineUid>(c.file, kUidSection);
  if (!dir) return tl::unexpected(dir.error());
  auto strings = typed_section<char>(c.file, kStringsSection);
  if (!strings) return tl::unexpected(strings.error());
  auto entries = typed_section<TimelineEntry>(c.file, kEntriesSection);
  if (!entries) return tl::unexpected(entries.error());
  auto records = records_of(c.file);
  if (!records) return tl::unexpected(records.error());
  c.dir = *dir;
  c.strings = *strings;
  c.entries = *entries;
  c.records = *records;
  const PersistHeader &h = c.file.header();
  if (c.meta.flags != 0u || c.meta.reserved0 != 0u || c.meta.reserved1 != 0u ||
      c.meta.n_uids != c.dir.size() || c.meta.n_entries != c.entries.size() ||
      c.meta.records_bytes != c.records.size() || c.meta.ts_min_ns != h.ts_min_ns ||
      c.meta.ts_max_ns != h.ts_max_ns || c.meta.source_fingerprint != h.source_fingerprint) {
    return persist_error(S::RecordInvalid, kMetaSection);
  }
  if (PersistOutcome ok = check_index(c); !ok) return tl::unexpected(ok.error());
  c.uids.reserve(c.dir.size());
  for (const TimelineUid &d : c.dir) c.uids.push_back(d.uid);
  return c;
}

// The entries of `uid` with from <= ts <= to (ascending), or empty.
[[nodiscard]] std::span<const TimelineEntry> range_of(const TimelineCore &c, std::uint32_t uid,
                                                      std::int64_t from, std::int64_t to) noexcept {
  if (from > to) return {};
  const auto it = std::lower_bound(c.uids.begin(), c.uids.end(), uid);
  if (it == c.uids.end() || *it != uid) return {};
  const TimelineUid &d = c.dir[static_cast<std::size_t>(it - c.uids.begin())];
  const auto all = c.entries.subspan(static_cast<std::size_t>(d.entry_begin), d.entry_count);
  const auto before = [](const TimelineEntry &e, std::int64_t v) { return e.ts_ns < v; };
  const auto after = [](std::int64_t v, const TimelineEntry &e) { return v < e.ts_ns; };
  const auto lo = std::lower_bound(all.begin(), all.end(), from, before);
  const auto hi = std::upper_bound(lo, all.end(), to, after);
  return all.subspan(static_cast<std::size_t>(lo - all.begin()), static_cast<std::size_t>(hi - lo));
}

// The checked record of `e`: bounds, the ATXVSA2 payload CRC against both copies, the
// framing the CRC cannot speak for (magic, size, uid binding).
[[nodiscard]] PersistResult<std::span<const std::byte>> verified_record(const TimelineCore &c,
                                                                        const TimelineEntry &e) {
  auto rec = c.record(e);
  if (!rec) return rec;
  const std::uint32_t crc = record_crc(*rec);
  if (crc != e.record_crc32c) {
    return persist_error(S::SectionChecksum, kRecordsSection, e.record_offset);
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, rec->data(), sizeof h);
  if (std::memcmp(h.magic, kArchiveV2RecordMagic, sizeof h.magic) != 0 ||
      h.record_size != rec->size() || h.payload_crc32c != crc || h.uid != e.uid ||
      h.n_slices != e.n_slices) {
    return persist_error(S::RecordInvalid, kRecordsSection, e.record_offset);
  }
  return rec;
}

} // namespace

std::uint64_t surface_timeline_fingerprint() noexcept { return kFingerprint; }

PersistResult<std::vector<std::byte>>
encode_surface_timeline(std::span<const TimelineSource> sources, std::int64_t created_unix_ns) {
  if (sources.empty() || sources.size() > std::numeric_limits<std::uint32_t>::max()) {
    return persist_error(S::InvalidArgument);
  }
  std::vector<LoadedPartition> loaded;
  loaded.reserve(sources.size()); // the items borrow these buffers
  std::vector<RecordItem> items;
  ImageInfo info;
  info.n_sources = static_cast<std::uint32_t>(sources.size());
  info.created_unix_ns = created_unix_ns;
  std::uint64_t h = persist_detail::kFnvOffset;
  for (const TimelineSource &s : sources) {
    auto p = load_partition(s.archive_path);
    if (!p) return tl::unexpected(p.error());
    loaded.push_back(std::move(*p));
    auto fp = append_partition(loaded.back().bytes(), s.ts_ns, items);
    if (!fp) return tl::unexpected(fp.error());
    h = mix(h, *fp);
  }
  info.source_fingerprint = persist_detail::fmix64(h);
  return build_image(std::move(items), info);
}

// ── SurfaceTimeline ───────────────────────────────────────────────────────────

struct SurfaceTimeline::Impl {
  TimelineCore core;
};

SurfaceTimeline::SurfaceTimeline(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
SurfaceTimeline::SurfaceTimeline(SurfaceTimeline &&) noexcept = default;
SurfaceTimeline &SurfaceTimeline::operator=(SurfaceTimeline &&) noexcept = default;
SurfaceTimeline::~SurfaceTimeline() = default;

PersistResult<SurfaceTimeline> SurfaceTimeline::open(std::string_view path,
                                                     const PersistOpenOptions &options) {
  auto core = open_core(path, options);
  if (!core) return tl::unexpected(core.error());
  return SurfaceTimeline{std::make_unique<Impl>(Impl{std::move(*core)})};
}

std::span<const std::uint32_t> SurfaceTimeline::uids() const noexcept {
  if (!impl_) return {};
  return impl_->core.uids;
}

std::span<const TimelineEntry> SurfaceTimeline::range(std::uint32_t uid, std::int64_t from_ns,
                                                      std::int64_t to_ns) const noexcept {
  if (!impl_) return {};
  return range_of(impl_->core, uid, from_ns, to_ns);
}

PersistResult<PricedSurfaceView> SurfaceTimeline::view(const TimelineEntry &entry) const {
  if (!impl_) return persist_error(S::InvalidArgument);
  auto rec = verified_record(impl_->core, entry);
  if (!rec) return tl::unexpected(rec.error());
  auto v = PricedSurfaceView::create_over_record(*rec);
  if (!v) return persist_error(S::RecordInvalid, kRecordsSection, entry.record_offset);
  return std::move(*v);
}

// ── SurfaceTimelineStore ──────────────────────────────────────────────────────

namespace {

struct Period {
  std::string key;
  std::int64_t ts_min_ns{0};
  std::int64_t ts_max_ns{0};
  SurfaceTimeline timeline;
};

// The partition file of a SurfaceDb key: the db's public layout
// (<root>/partitions/<CANONICAL KEY>.atxvsa, surface_db.hpp kSurfaceDbPartition*), for
// a key its manifest lists.
struct PlannedSource {
  std::string period;
  TimelineSource source;
};

[[nodiscard]] PersistResult<PlannedSource> plan_key(const SurfaceDb &db,
                                                    std::span<const DbPartitionInfo> parts,
                                                    std::string_view key) {
  std::string canon(key);
  std::transform(canon.begin(), canon.end(), canon.begin(), [](char ch) {
    return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
  });
  const bool listed = std::any_of(parts.begin(), parts.end(),
                                  [&](const DbPartitionInfo &p) { return p.key == canon; });
  if (!listed) return persist_error(S::NotFound);
  auto ts = db.session_ts(key);
  if (!ts) return from_core(ts.error(), kRecordsSection);
  auto period = period_key(*ts);
  if (!period) return tl::unexpected(period.error());
  const fs::path path = fs::path{db.root()} / std::string(kSurfaceDbPartitionDir) /
                        (canon + std::string(kSurfaceDbPartitionExt));
  return PlannedSource{std::move(*period), TimelineSource{path.string(), *ts}};
}

// Rebuild one period: the existing generation's records (fully verified) whose instant
// no new source replaces, plus every new source. Returns the entries published.
[[nodiscard]] PersistResult<std::size_t> rebuild_period(std::string_view root,
                                                        const std::string &period,
                                                        std::span<const TimelineSource> sources,
                                                        std::int64_t created_unix_ns) {
  std::vector<RecordItem> items;
  ImageInfo info;
  info.created_unix_ns = created_unix_ns;
  std::uint64_t h = persist_detail::kFnvOffset;
  std::vector<std::int64_t> fresh;
  for (const TimelineSource &s : sources) fresh.push_back(s.ts_ns);
  std::sort(fresh.begin(), fresh.end());

  std::vector<TimelineCore> previous; // keeps the carried records alive
  auto dir = PersistDirectory::open(root);
  if (!dir && dir.error().status != S::NotFound) return tl::unexpected(dir.error());
  if (dir) {
    if (const PersistManifestEntry *e = dir->find(period); e != nullptr) {
      auto old = open_core(dir->path(*e), PersistOpenOptions{false, PersistVerify::Full, false,
                                                             e->file_size});
      if (!old) return tl::unexpected(old.error());
      previous.push_back(std::move(*old));
      const TimelineCore &c = previous.back();
      info.n_sources = c.meta.n_sources;
      h = mix(h, c.meta.source_fingerprint);
      for (const TimelineUid &d : c.dir) {
        for (const TimelineEntry &e2 : c.entries.subspan(static_cast<std::size_t>(d.entry_begin),
                                                         d.entry_count)) {
          if (std::binary_search(fresh.begin(), fresh.end(), e2.ts_ns)) continue;
          auto rec = verified_record(c, e2);
          if (!rec) return tl::unexpected(rec.error());
          items.push_back(RecordItem{e2.uid, e2.ts_ns, e2.n_slices, e2.flags, c.ticker(d), *rec,
                                     e2.record_crc32c, e2.source_fingerprint});
        }
      }
    }
  }
  std::vector<LoadedPartition> loaded;
  loaded.reserve(sources.size());
  for (const TimelineSource &s : sources) {
    auto p = load_partition(s.archive_path);
    if (!p) return tl::unexpected(p.error());
    loaded.push_back(std::move(*p));
    auto fp = append_partition(loaded.back().bytes(), s.ts_ns, items);
    if (!fp) return tl::unexpected(fp.error());
    h = mix(h, *fp);
  }
  info.n_sources += static_cast<std::uint32_t>(sources.size());
  info.source_fingerprint = persist_detail::fmix64(h);
  const std::size_t n = items.size();
  auto image = build_image(std::move(items), info);
  if (!image) return tl::unexpected(image.error());
  auto published = PersistDirectory::publish(root, period, *image, static_cast<std::uint32_t>(n));
  if (!published) return tl::unexpected(published.error());
  return n;
}

} // namespace

struct SurfaceTimelineStore::Impl {
  std::string root;
  std::vector<Period> periods; // ascending, disjoint ts ranges
};

SurfaceTimelineStore::SurfaceTimelineStore(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}
SurfaceTimelineStore::SurfaceTimelineStore(SurfaceTimelineStore &&) noexcept = default;
SurfaceTimelineStore &SurfaceTimelineStore::operator=(SurfaceTimelineStore &&) noexcept = default;
SurfaceTimelineStore::~SurfaceTimelineStore() = default;

PersistResult<SurfaceTimelineStore> SurfaceTimelineStore::open(std::string_view root) {
  auto dir = PersistDirectory::open(root);
  if (!dir) return tl::unexpected(dir.error());
  auto impl = std::make_unique<Impl>();
  impl->root = std::string(root);
  for (const PersistManifestEntry &e : dir->entries()) {
    if (e.kind != static_cast<std::uint32_t>(PersistKind::SurfaceTimeline)) {
      return persist_error(S::WrongKind, kPersistNone, e.seq);
    }
    PersistOpenOptions o;
    o.expected_size = e.file_size;
    auto tl = SurfaceTimeline::open(dir->path(e), o);
    if (!tl) return tl::unexpected(tl.error());
    impl->periods.push_back(Period{std::string(e.key.data()), e.ts_min_ns, e.ts_max_ns,
                                   std::move(*tl)});
  }
  std::sort(impl->periods.begin(), impl->periods.end(),
            [](const Period &a, const Period &b) { return a.ts_min_ns < b.ts_min_ns; });
  for (std::size_t i = 1; i < impl->periods.size(); ++i) {
    // collect() concatenates periods in order, so their instants must not interleave.
    if (impl->periods[i - 1].ts_max_ns >= impl->periods[i].ts_min_ns) {
      return persist_error(S::IndexUnsorted, kPersistNone, i);
    }
  }
  return SurfaceTimelineStore{std::move(impl)};
}

PersistResult<std::size_t> SurfaceTimelineStore::build_from_surface_db(
    const SurfaceDb &db, std::span<const std::string> keys, std::string_view root,
    std::int64_t created_unix_ns) {
  if (keys.empty()) return persist_error(S::InvalidArgument);
  const std::vector<DbPartitionInfo> parts = db.partitions();
  // Resolve every key before publishing anything.
  std::map<std::string, std::vector<TimelineSource>> by_period;
  for (const std::string &key : keys) {
    auto planned = plan_key(db, parts, key);
    if (!planned) return tl::unexpected(planned.error());
    by_period[planned->period].push_back(std::move(planned->source));
  }
  std::size_t total = 0;
  for (const auto &[period, sources] : by_period) {
    auto n = rebuild_period(root, period, sources, created_unix_ns);
    if (!n) return tl::unexpected(n.error());
    total += *n;
  }
  return total;
}

PersistResult<std::size_t> SurfaceTimelineStore::collect(std::uint32_t uid, std::int64_t from_ns,
                                                         std::int64_t to_ns,
                                                         std::vector<PricedSurfaceView> &views,
                                                         std::vector<SurfaceRef> &refs,
                                                         std::vector<std::int64_t> &ts) const {
  if (!impl_) return persist_error(S::InvalidArgument);
  const auto overlaps = [&](const Period &p) {
    return p.ts_max_ns >= from_ns && p.ts_min_ns <= to_ns;
  };
  std::size_t n = 0;
  for (const Period &p : impl_->periods) {
    if (overlaps(p)) n += p.timeline.range(uid, from_ns, to_ns).size();
  }
  if (n == 0) return std::size_t{0};

  const std::size_t views0 = views.size();
  if (views.capacity() - views0 < n) {
    // The reserve below moves the views: re-point every ref that names one of them
    // (by index, recovered with integer arithmetic and confirmed by equality, so no
    // pointer into a freed buffer is ever formed) so earlier refs stay valid.
    std::vector<std::pair<std::size_t, std::size_t>> rebase;
    const auto base = std::bit_cast<std::uintptr_t>(views.data());
    for (std::size_t j = 0; j < refs.size(); ++j) {
      const auto at = std::bit_cast<std::uintptr_t>(refs[j].view());
      if (refs[j].view() == nullptr || at < base) continue;
      const std::size_t idx = (at - base) / sizeof(PricedSurfaceView);
      if (idx < views0 && &views[idx] == refs[j].view()) rebase.emplace_back(j, idx);
    }
    views.reserve(views0 + n);
    for (const auto &[j, idx] : rebase) refs[j] = SurfaceRef{&views[idx]};
  }
  const std::size_t refs0 = refs.size();
  const std::size_t ts0 = ts.size();
  refs.reserve(refs0 + n);
  ts.reserve(ts0 + n);
  for (const Period &p : impl_->periods) {
    if (!overlaps(p)) continue;
    for (const TimelineEntry &e : p.timeline.range(uid, from_ns, to_ns)) {
      auto v = p.timeline.view(e);
      if (!v) {
        views.erase(views.begin() + static_cast<std::ptrdiff_t>(views0), views.end());
        refs.resize(refs0);
        ts.resize(ts0);
        return tl::unexpected(v.error());
      }
      views.push_back(std::move(*v));
      refs.emplace_back(&views.back());
      ts.push_back(e.ts_ns);
    }
  }
  return n;
}

} // namespace atx::vol
