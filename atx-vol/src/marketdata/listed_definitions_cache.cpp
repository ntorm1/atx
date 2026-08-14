#include "atx/vol/research/listed_definitions_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <ankerl/unordered_dense.h>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "storage/archive_util.hpp"
#include "core/log_emit.hpp"
#include "atx/vol/research/run_diagnostics.hpp" // PhaseTimer (optional definitions_cache hit/miss phase, review I6)

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::vol::detail::align_up;
using atx::vol::detail::crc32c;

namespace fs = std::filesystem;

constexpr std::uint16_t kLittleEndian = 1;
constexpr std::uint16_t kPointerBits = 64;

// ── Derived geometry ────────────────────────────────────────────────────────
//
// ONE function computes every offset and size in the image, and BOTH the writer
// and the reader go through it. The reader recomputes the layout from
// (n_rows, n_entries, blob_len) and requires the header's stored offsets, sizes
// and file_size to match field-for-field, so a header claiming geometry the
// payload does not have is rejected structurally, before any byte is decoded.
struct BlobLayout {
  std::uint64_t string_table_offset{};
  std::uint64_t string_table_size{};
  std::uint64_t offsets_at{}; // absolute: u64 offsets[n_entries + 1]
  std::uint64_t blob_at{};    // absolute: char blob[blob_len]
  std::uint64_t column_block_offset{};
  std::uint64_t column_block_size{};
  std::uint64_t definition_ts_at{};
  std::uint64_t expiry_ts_at{};
  std::uint64_t multiplier_at{};
  std::uint64_t source_fingerprint_at{};
  std::uint64_t instrument_id_at{};
  std::uint64_t trade_date_code_at{};
  std::uint64_t raw_symbol_code_at{};
  std::uint64_t flags_at{};
  std::uint64_t file_size{};
};

// Ceiling on the row / dict counts a layout may describe. Chosen so every
// `count * elem` product below stays far inside u64 and so a corrupt header
// cannot ask for an absurd allocation before any CRC has been checked.
constexpr std::uint64_t kMaxRows = 1ull << 40;
constexpr std::uint64_t kMaxDictEntries = 1ull << 40;
constexpr std::uint64_t kMaxBlobLen = 1ull << 44;

[[nodiscard]] bool compute_layout(std::uint64_t n_rows, std::uint64_t n_entries,
                                  std::uint64_t blob_len, BlobLayout &out) noexcept {
  if (n_rows > kMaxRows || n_entries > kMaxDictEntries || blob_len > kMaxBlobLen) {
    return false;
  }
  out = BlobLayout{};
  out.string_table_offset = sizeof(ListedDefinitionsCacheHeader);
  out.offsets_at = out.string_table_offset + 8u;
  out.blob_at = out.offsets_at + 8u * (n_entries + 1u);
  out.string_table_size = 8u + 8u * (n_entries + 1u) + blob_len;
  out.column_block_offset = align_up(out.string_table_offset + out.string_table_size,
                                     kDefinitionsCacheAlign);

  std::uint64_t at = out.column_block_offset;
  const auto place = [&](std::uint64_t &field, std::uint64_t elem) {
    field = at;
    at = align_up(at + elem * n_rows, kDefinitionsCacheAlign);
  };
  place(out.definition_ts_at, 8u);
  place(out.expiry_ts_at, 8u);
  place(out.multiplier_at, 8u);
  place(out.source_fingerprint_at, 8u);
  place(out.instrument_id_at, 4u);
  place(out.trade_date_code_at, 4u);
  place(out.raw_symbol_code_at, 4u);
  place(out.flags_at, 1u);

  out.column_block_size = at - out.column_block_offset;
  out.file_size = at;
  return true;
}

// ── Raw load/store into the image (host little-endian LP64 only) ────────────
template <typename T> void store_at(std::byte *image, std::uint64_t off, const T &value) noexcept {
  std::memcpy(image + off, &value, sizeof(T));
}

template <typename T>
[[nodiscard]] T load_at(const std::byte *image, std::uint64_t off) noexcept {
  T value{};
  std::memcpy(&value, image + off, sizeof(T));
  return value;
}

// ── Durable publish (mirrors write_run_archive_file / commit 86f2210) ───────
[[nodiscard]] std::FILE *cache_fopen_write_binary(const fs::path &p) noexcept {
#if defined(_WIN32)
  std::FILE *fp = nullptr;
  if (::_wfopen_s(&fp, p.wstring().c_str(), L"wb") != 0) {
    return nullptr;
  }
  return fp;
#else
  return std::fopen(p.string().c_str(), "wb");
#endif
}

[[nodiscard]] std::FILE *cache_fopen_read_binary(const fs::path &p) noexcept {
#if defined(_WIN32)
  std::FILE *fp = nullptr;
  if (::_wfopen_s(&fp, p.wstring().c_str(), L"rb") != 0) {
    return nullptr;
  }
  return fp;
#else
  return std::fopen(p.string().c_str(), "rb");
#endif
}

// fflush only pushes the CRT buffer into the OS page cache; _commit / fsync
// force it to the device, so the temp is durable BEFORE the rename replaces the
// previous good file.
[[nodiscard]] bool cache_fsync_stream(std::FILE *fp) noexcept {
  if (std::fflush(fp) != 0) {
    return false;
  }
#if defined(_WIN32)
  return ::_commit(::_fileno(fp)) == 0;
#else
  return ::fsync(::fileno(fp)) == 0;
#endif
}

} // namespace

// ── Key ─────────────────────────────────────────────────────────────────────

std::uint64_t definitions_cache_abi_fold() noexcept {
  // Fold the actual fixed-width column schema, not the nonportable object layout
  // of the runtime row (which contains std::string and is never copied to disk).
  using D = ListedDefinitionsCacheWireRowSchema;
  const std::uint64_t shape[] = {
      sizeof(D),
      alignof(D),
      offsetof(D, definition_ts_ns),
      sizeof(D::definition_ts_ns),
      offsetof(D, expiry_ts_ns),
      sizeof(D::expiry_ts_ns),
      offsetof(D, multiplier),
      sizeof(D::multiplier),
      offsetof(D, source_fingerprint),
      sizeof(D::source_fingerprint),
      offsetof(D, instrument_id),
      sizeof(D::instrument_id),
      offsetof(D, trade_date_code),
      sizeof(D::trade_date_code),
      offsetof(D, raw_symbol_code),
      sizeof(D::raw_symbol_code),
      offsetof(D, flags),
      sizeof(D::flags),
      kDefinitionsCacheMonthlyFlag,
      kDefinitionsCacheDeliverableFlag,
  };
  const std::uint64_t fold = atx::core::hash_bytes(static_cast<const void *>(shape), sizeof shape);
  return fold == 0u ? 1u : fold; // 0 is reserved as "unset" by convention
}

ListedDefinitionsCacheKey definitions_cache_key(std::string_view source_bytes) {
  ListedDefinitionsCacheKey key;
  key.content_hash = atx::core::hash_bytes(source_bytes.data(), source_bytes.size());
  key.source_size = static_cast<std::uint64_t>(source_bytes.size());
  key.format_version = kDefinitionsCacheFormat;
  key.parser_revision = kDefinitionsParserRevision;
  key.abi_fold = definitions_cache_abi_fold();
  return key;
}

std::string definitions_cache_filename(const ListedDefinitionsCacheKey &key) {
  // `abi_fold` is in the NAME as well as the key (review M1). Correctness never
  // depended on it — the key rejects a foreign-shaped blob either way — but two
  // builds with different encoded wire schemas and the same source
  // otherwise contend for one path and ping-pong a ~300 MB write on every run.
  char buffer[128];
  const int written = std::snprintf(
      buffer, sizeof buffer, "definitions-%016llx-%llu-%u-%u-%016llx.atxdefs",
      static_cast<unsigned long long>(key.content_hash),
      static_cast<unsigned long long>(key.source_size), key.format_version, key.parser_revision,
      static_cast<unsigned long long>(key.abi_fold));
  if (written <= 0) {
    return std::string("definitions.atxdefs");
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

// ── Writer ──────────────────────────────────────────────────────────────────

Status write_definitions_cache(std::string_view cache_path, const ListedDefinitionTable &table,
                               const ListedDefinitionsCacheKey &key) {
  const fs::path dst{std::string(cache_path)};
  if (dst.empty()) {
    return Err(ErrorCode::InvalidArgument, "definitions cache: empty output path");
  }
  const std::span<const ListedContractDefinition> rows = table.definitions();
  const std::uint64_t n_rows = static_cast<std::uint64_t>(rows.size());
  if (n_rows > kMaxRows) {
    return Err(ErrorCode::InvalidArgument, "definitions cache: table too large to encode");
  }

  // Intern `trade_date` and `raw_symbol` into ONE deduplicated table. The views
  // alias `rows`, which the caller keeps alive for the duration of this call.
  ankerl::unordered_dense::map<std::string_view, std::uint32_t> index;
  std::vector<std::string_view> dict;
  std::vector<std::uint32_t> date_codes;
  std::vector<std::uint32_t> symbol_codes;
  date_codes.reserve(rows.size());
  symbol_codes.reserve(rows.size());
  std::uint64_t blob_len = 0;
  const auto intern = [&](std::string_view value) {
    const auto [it, inserted] = index.try_emplace(value, static_cast<std::uint32_t>(dict.size()));
    if (inserted) {
      dict.push_back(value);
      blob_len += static_cast<std::uint64_t>(value.size());
    }
    return it->second;
  };
  for (const ListedContractDefinition &row : rows) {
    date_codes.push_back(intern(row.trade_date));
    symbol_codes.push_back(intern(row.raw_symbol));
  }
  const std::uint64_t n_entries = static_cast<std::uint64_t>(dict.size());

  BlobLayout layout;
  if (!compute_layout(n_rows, n_entries, blob_len, layout)) {
    return Err(ErrorCode::InvalidArgument, "definitions cache: table too large to encode");
  }

  std::vector<std::byte> image;
  try {
    image.assign(static_cast<std::size_t>(layout.file_size), std::byte{0});
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "definitions cache: cannot allocate image");
  }
  std::byte *const base = image.data();

  // String table: u64 n_entries, u64 offsets[n_entries + 1], char blob[].
  store_at<std::uint64_t>(base, layout.string_table_offset, n_entries);
  std::uint64_t blob_cursor = 0;
  for (std::uint64_t i = 0; i < n_entries; ++i) {
    store_at<std::uint64_t>(base, layout.offsets_at + 8u * i, blob_cursor);
    const std::string_view value = dict[static_cast<std::size_t>(i)];
    if (!value.empty()) {
      std::memcpy(base + layout.blob_at + blob_cursor, value.data(), value.size());
    }
    blob_cursor += static_cast<std::uint64_t>(value.size());
  }
  store_at<std::uint64_t>(base, layout.offsets_at + 8u * n_entries, blob_cursor);

  // Column arrays.
  for (std::uint64_t i = 0; i < n_rows; ++i) {
    const ListedContractDefinition &row = rows[static_cast<std::size_t>(i)];
    store_at<std::int64_t>(base, layout.definition_ts_at + 8u * i, row.definition_ts_ns);
    store_at<std::int64_t>(base, layout.expiry_ts_at + 8u * i, row.expiry_ts_ns);
    store_at<double>(base, layout.multiplier_at + 8u * i, row.multiplier);
    store_at<std::uint64_t>(base, layout.source_fingerprint_at + 8u * i, row.source_fingerprint);
    store_at<std::uint32_t>(base, layout.instrument_id_at + 4u * i, row.instrument_id);
    store_at<std::uint32_t>(base, layout.trade_date_code_at + 4u * i,
                            date_codes[static_cast<std::size_t>(i)]);
    store_at<std::uint32_t>(base, layout.raw_symbol_code_at + 4u * i,
                            symbol_codes[static_cast<std::size_t>(i)]);
    const std::uint8_t flags =
        static_cast<std::uint8_t>((row.standard_monthly ? kDefinitionsCacheMonthlyFlag : 0u) |
                                  (row.standard_deliverable
                                       ? kDefinitionsCacheDeliverableFlag
                                       : 0u));
    store_at<std::uint8_t>(base, layout.flags_at + i, flags);
  }

  // Header last: the payload CRC covers everything after it.
  ListedDefinitionsCacheHeader header{};
  std::memcpy(header.magic, kDefinitionsCacheMagic, sizeof header.magic);
  header.file_size = layout.file_size;
  header.content_hash = key.content_hash;
  header.source_size = key.source_size;
  header.abi_fold = key.abi_fold;
  // LAZY: on a table whose fingerprint has not been asked for, THIS call pays
  // for the canonical serialization. See the header's cost note.
  header.table_fingerprint = table.fingerprint();
  header.n_rows = n_rows;
  header.string_table_offset = layout.string_table_offset;
  header.string_table_size = layout.string_table_size;
  header.column_block_offset = layout.column_block_offset;
  header.column_block_size = layout.column_block_size;
  header.format_version = key.format_version;
  header.parser_revision = key.parser_revision;
  header.major = kDefinitionsCacheMajor;
  header.minor = kDefinitionsCacheMinor;
  header.header_size = static_cast<std::uint16_t>(sizeof(ListedDefinitionsCacheHeader));
  header.endian = kLittleEndian;
  header.pointer_bits = kPointerBits;
  header.payload_crc32c =
      crc32c(base + sizeof(ListedDefinitionsCacheHeader),
             static_cast<std::size_t>(layout.file_size - sizeof(ListedDefinitionsCacheHeader)));
  header.header_crc32c = 0;
  std::memcpy(base, &header, sizeof header);
  header.header_crc32c = crc32c(base, sizeof header);
  std::memcpy(base, &header, sizeof header);

  std::error_code ec;
  if (!dst.parent_path().empty()) {
    fs::create_directories(dst.parent_path(), ec);
    if (ec && !fs::is_directory(dst.parent_path())) {
      return Err(ErrorCode::IoError, "write_definitions_cache: cannot create cache directory");
    }
  }

  fs::path tmp = dst;
  tmp += ".tmp";
  {
    std::FILE *fp = cache_fopen_write_binary(tmp);
    if (fp == nullptr) {
      return Err(ErrorCode::IoError, "write_definitions_cache: cannot open temp file");
    }
    const bool wrote =
        image.empty() || std::fwrite(image.data(), 1, image.size(), fp) == image.size();
    const bool synced = wrote && cache_fsync_stream(fp);
    const bool closed = std::fclose(fp) == 0; // always close, even on prior failure
    if (!wrote || !synced || !closed) {
      ec.clear();
      fs::remove(tmp, ec);
      return Err(ErrorCode::IoError, "write_definitions_cache: write failed");
    }
  }

  // Atomic publish with bounded retry + exponential backoff: on Windows the
  // rename fails while a reader holds the destination open without
  // FILE_SHARE_DELETE. On FINAL failure the temp is PRESERVED so the freshly
  // written bytes are recoverable and the prior good destination is intact.
  constexpr int kMaxAttempts = 8;
  std::chrono::milliseconds delay{5};
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    ec.clear();
    fs::rename(tmp, dst, ec);
    if (!ec) {
      return Ok();
    }
    if (attempt + 1 < kMaxAttempts) {
      std::this_thread::sleep_for(delay);
      delay *= 2;
    }
  }
  return Err(ErrorCode::IoError,
             "write_definitions_cache: cannot publish file (rename failed after retries)");
}

// ── Reader ──────────────────────────────────────────────────────────────────

Result<ListedDefinitionTable> read_definitions_cache(std::string_view cache_path,
                                                     const ListedDefinitionsCacheKey &expected,
                                                     DefinitionsCacheFingerprintCheck check) {
  const fs::path src{std::string(cache_path)};
  std::error_code ec;
  const std::uintmax_t on_disk = fs::file_size(src, ec);
  if (ec) {
    return Err(ErrorCode::NotFound, "definitions cache: no cache file");
  }
  if (on_disk < sizeof(ListedDefinitionsCacheHeader)) {
    return Err(ErrorCode::ParseError, "definitions cache: file shorter than its header");
  }

  std::vector<std::byte> image;
  try {
    image.resize(static_cast<std::size_t>(on_disk));
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "definitions cache: cannot allocate image");
  }
  {
    std::FILE *fp = cache_fopen_read_binary(src);
    if (fp == nullptr) {
      return Err(ErrorCode::IoError, "definitions cache: cannot open cache file");
    }
    const std::size_t got = std::fread(image.data(), 1, image.size(), fp);
    const bool closed = std::fclose(fp) == 0;
    if (got != image.size() || !closed) {
      return Err(ErrorCode::IoError, "definitions cache: short read");
    }
  }
  const std::byte *const base = image.data();

  ListedDefinitionsCacheHeader header{};
  std::memcpy(&header, base, sizeof header);

  if (std::memcmp(header.magic, kDefinitionsCacheMagic, sizeof header.magic) != 0 ||
      header.major != kDefinitionsCacheMajor || header.endian != kLittleEndian ||
      header.pointer_bits != kPointerBits ||
      header.header_size != sizeof(ListedDefinitionsCacheHeader)) {
    return Err(ErrorCode::ParseError, "definitions cache: header mismatch");
  }

  // GUARD 1 — header integrity. Recompute over the header bytes with
  // `header_crc32c` treated as zero, exactly as the writer stamped it. Every
  // field read below (including the geometry and the key) is only trustworthy
  // once this passes.
  {
    ListedDefinitionsCacheHeader probe = header;
    probe.header_crc32c = 0;
    std::byte header_bytes[sizeof(ListedDefinitionsCacheHeader)];
    std::memcpy(header_bytes, &probe, sizeof header_bytes);
    if (crc32c(header_bytes, sizeof header_bytes) != header.header_crc32c) {
      return Err(ErrorCode::ParseError, "definitions cache: header CRC mismatch");
    }
  }

  // GUARD 2 — identity. All five key fields must match FIELD FOR FIELD. This is
  // the stale-serve gate: a blob written for other source bytes, by another wire
  // format, by another parser revision, or against another encoded wire schema
  // is a MISS, never a serve.
  const ListedDefinitionsCacheKey stored{header.content_hash, header.source_size,
                                         header.format_version, header.parser_revision,
                                         header.abi_fold};
  if (!(stored == expected)) {
    return Err(ErrorCode::ParseError, "definitions cache: key mismatch");
  }

  if (header.file_size != static_cast<std::uint64_t>(on_disk)) {
    return Err(ErrorCode::ParseError, "definitions cache: truncated or extended file");
  }

  // Structural geometry: recompute the layout from the counts the file carries
  // and require the header's stored offsets and sizes to agree exactly.
  // Written as a SUBTRACTION, not `offset + size > file_size`: the addition can
  // wrap for a header claiming a near-UINT64_MAX table and would then pass. The
  // subtraction cannot underflow — `file_size` was just proven equal to the real
  // on-disk length, which is at least `sizeof(header)`, and `string_table_offset`
  // is required to be exactly that.
  if (header.string_table_offset != sizeof(ListedDefinitionsCacheHeader) ||
      header.string_table_size < 16u ||
      header.string_table_size > header.file_size - header.string_table_offset) {
    return Err(ErrorCode::ParseError, "definitions cache: bad string table extent");
  }
  const std::uint64_t n_entries = load_at<std::uint64_t>(base, header.string_table_offset);
  if (n_entries > kMaxDictEntries) {
    return Err(ErrorCode::ParseError, "definitions cache: implausible dictionary size");
  }
  const std::uint64_t index_bytes = 8u + 8u * (n_entries + 1u);
  if (header.string_table_size < index_bytes) {
    return Err(ErrorCode::ParseError, "definitions cache: string index overruns its table");
  }
  const std::uint64_t blob_len = header.string_table_size - index_bytes;

  BlobLayout layout;
  if (!compute_layout(header.n_rows, n_entries, blob_len, layout)) {
    return Err(ErrorCode::ParseError, "definitions cache: implausible geometry");
  }
  // NOTE (review M4): of the four comparisons below, `string_table_size` cannot
  // fail — `compute_layout` derives it from `blob_len`, which was itself derived
  // by subtracting `index_bytes` from `header.string_table_size`. It is kept for
  // symmetry with the other three, which are real gates. Do not read it as
  // evidence that the string table's declared size was independently checked;
  // that check is the extent test above.
  if (layout.file_size != header.file_size ||
      layout.string_table_size != header.string_table_size ||
      layout.column_block_offset != header.column_block_offset ||
      layout.column_block_size != header.column_block_size) {
    return Err(ErrorCode::ParseError, "definitions cache: header geometry disagrees with payload");
  }

  // GUARD 3 — payload integrity, over [header_size, file_size). Checked BEFORE
  // any byte of the payload is interpreted, so a corrupt blob is never decoded.
  if (crc32c(base + sizeof(ListedDefinitionsCacheHeader),
             static_cast<std::size_t>(header.file_size - sizeof(ListedDefinitionsCacheHeader))) !=
      header.payload_crc32c) {
    return Err(ErrorCode::ParseError, "definitions cache: payload CRC mismatch");
  }

  // String offsets: non-decreasing, terminated at exactly blob_len.
  std::vector<std::uint64_t> offsets;
  try {
    offsets.resize(static_cast<std::size_t>(n_entries) + 1u);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "definitions cache: cannot allocate string index");
  }
  std::memcpy(offsets.data(), base + layout.offsets_at, offsets.size() * sizeof(std::uint64_t));
  for (std::size_t i = 0; i + 1u < offsets.size(); ++i) {
    if (offsets[i] > offsets[i + 1u]) {
      return Err(ErrorCode::ParseError, "definitions cache: string offsets not monotone");
    }
  }
  if (offsets.back() != blob_len) {
    return Err(ErrorCode::ParseError, "definitions cache: string blob length mismatch");
  }

  const char *const blob = reinterpret_cast<const char *>(base + layout.blob_at);
  const auto view = [&](std::uint32_t code) -> std::string_view {
    const std::size_t i = static_cast<std::size_t>(code);
    return std::string_view(blob + offsets[i], static_cast<std::size_t>(offsets[i + 1u] - offsets[i]));
  };

  const std::size_t n_rows = static_cast<std::size_t>(header.n_rows);
  std::vector<ListedContractDefinition> rows;
  try {
    rows.resize(n_rows);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "definitions cache: cannot allocate rows");
  }
  for (std::size_t i = 0; i < n_rows; ++i) {
    const std::uint32_t date_code = load_at<std::uint32_t>(base, layout.trade_date_code_at + 4u * i);
    const std::uint32_t symbol_code =
        load_at<std::uint32_t>(base, layout.raw_symbol_code_at + 4u * i);
    if (static_cast<std::uint64_t>(date_code) >= n_entries ||
        static_cast<std::uint64_t>(symbol_code) >= n_entries) {
      return Err(ErrorCode::ParseError, "definitions cache: string code out of range");
    }
    const std::uint8_t flags = load_at<std::uint8_t>(base, layout.flags_at + i);
    if ((flags & ~kDefinitionsCacheKnownFlags) != 0u) {
      return Err(ErrorCode::ParseError, "definitions cache: unknown flag bits");
    }
    ListedContractDefinition &row = rows[i];
    row.trade_date.assign(view(date_code));
    row.raw_symbol.assign(view(symbol_code));
    row.instrument_id = load_at<std::uint32_t>(base, layout.instrument_id_at + 4u * i);
    row.definition_ts_ns = load_at<std::int64_t>(base, layout.definition_ts_at + 8u * i);
    row.expiry_ts_ns = load_at<std::int64_t>(base, layout.expiry_ts_at + 8u * i);
    row.multiplier = load_at<double>(base, layout.multiplier_at + 8u * i);
    row.standard_monthly = (flags & kDefinitionsCacheMonthlyFlag) != 0u;
    row.standard_deliverable = (flags & kDefinitionsCacheDeliverableFlag) != 0u;
    row.source_fingerprint = load_at<std::uint64_t>(base, layout.source_fingerprint_at + 8u * i);
  }

  auto table = ListedDefinitionTable::create(std::move(rows));
  if (!table) {
    return Err(ErrorCode::ParseError, "definitions cache: decoded rows rejected by create()");
  }

  // GUARD 4 — OPT-IN. The reconstructed table must hash to the value the writer
  // stamped. This is the ONE check the CRCs cannot make: the CRCs prove the
  // bytes are the bytes that were written, this proves those bytes still MEAN
  // the table that was written. It catches a decoder defect and a
  // self-consistent hand-edit that a restamped CRC would wave through
  // (ListedDefinitionsCache.CacheRejectsTamperedPayloadEvenWithRepairedCrcs,
  // which pins it with `check == On`).
  //
  // It is also the single most expensive step on this path by a wide margin:
  // `fingerprint()` is lazy, so on a freshly decoded table this call pays for a
  // full `serialize_listed_definitions` — 1.4-3.8 s AND a ~730 MB transient on
  // the production fixture. `listed_definitions_cache.hpp` states in full why
  // that is not worth paying on every production read once the encoded struct's
  // shape is pinned at compile time.
  if (check == DefinitionsCacheFingerprintCheck::On &&
      table->fingerprint() != header.table_fingerprint) {
    return Err(ErrorCode::ParseError, "definitions cache: table fingerprint mismatch");
  }
  return table;
}

// ── The seam ────────────────────────────────────────────────────────────────

Result<ListedDefinitionTable> read_listed_definitions_cached(std::string_view tsv_path,
                                                             std::string_view cache_dir,
                                                             DefinitionsCacheFingerprintCheck check,
                                                             PhaseTimer *timer) {
  // The SAME function `read_listed_definitions_file` uses — not a copy of it
  // (review I2: these eight lines were duplicated verbatim, error strings and
  // all, so the `fread` change would otherwise have had to be made twice and
  // fixing only one would silently make the seam and the direct path diverge in
  // cost). The miss path stays exactly the direct path's cost plus the key hash.
  //
  // NOTE this slurp is UNAVOIDABLE on the HIT path too: the key is content-
  // derived, so the source bytes must be read before the cache can be consulted.
  // A hit can never avoid READING 730 MB; it can only avoid PARSING what it
  // read. That is the ceiling on what this format can be worth at the seam.
  std::string contents;
  switch (detail::read_whole_file(tsv_path, contents)) {
  case detail::FileReadStatus::NotFound:
    return Err(ErrorCode::NotFound, "listed definitions: file not found");
  case detail::FileReadStatus::IoError:
    return Err(ErrorCode::IoError, "listed definitions: read failed");
  case detail::FileReadStatus::Ok:
    break;
  }
  if (cache_dir.empty()) {
    return parse_listed_definitions(contents);
  }

  const ListedDefinitionsCacheKey key = definitions_cache_key(contents);
  const fs::path cache_file = fs::path{std::string(cache_dir)} / definitions_cache_filename(key);
  const auto lookup_start = PhaseTimer::now();
  auto hit = read_definitions_cache(cache_file.string(), key, check);
  if (timer) {
    // I6 — "the other half". `count` is the hit/miss flag itself (1 = HIT,
    // 0 = MISS), the shape Task 8's brief specifies, so a `diagnostics` reader
    // can tell a cached run from a fast-but-uncached one without grepping the
    // stderr line below. Timed span is deliberately JUST the lookup call: the
    // slurp above is a cost every path pays (cache or not), and a miss's
    // parse/publish below are the no-cache cost, not the cache's own cost.
    timer->add("definitions_cache", lookup_start, hit ? 1u : 0u);
  }
  if (hit) {
    // OBSERVABILITY, not decoration. `atx::core::hash_bytes` documents itself as
    // deterministic only WITHIN one process; the whole key rests on it also
    // being stable ACROSS processes (measured true on this build, but not a
    // contract). If that property is ever lost — a compiler change, an ankerl
    // bump, a platform move — this degrades to a permanent 100% miss that still
    // pays a ~300 MB write on every single run, and without these two lines
    // nothing but wall time would say so.
    detail::log_emitf(LogLevel::Info, LogStream::Stderr, "listed definitions cache: HIT %s",
                      cache_file.string().c_str());
    return hit;
  }
  detail::log_emitf(LogLevel::Info, LogStream::Stderr, "listed definitions cache: MISS (%s) %s",
                    hit.error().to_string().c_str(), cache_file.string().c_str());

  auto parsed = parse_listed_definitions(contents);
  if (!parsed) {
    return parsed;
  }
  // A publish failure is NEVER an error — it is a logged miss.
  const Status published = write_definitions_cache(cache_file.string(), *parsed, key);
  if (!published) {
    detail::log_emitf(LogLevel::Error, LogStream::Stderr,
                      "listed definitions cache: publish failed (%s); continuing uncached",
                      published.error().to_string().c_str());
  }
  return parsed;
}

} // namespace atx::vol
