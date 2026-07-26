// ATXDEFS1 — the pre-parsed listed-definitions cache (listed_definitions_cache.hpp).
//
// The format exists to make `definitions_parse` cheap; it earns its place only
// if it can NEVER serve a table that does not correspond to the file on disk.
// These tests are therefore weighted towards the rejection paths, and each
// rejection test carries its own NEGATIVE CONTROL: it first asserts that the
// UNPERTURBED file reads back green, so a green rejection can never be an
// artefact of the fixture failing to read at all.
//
// Test-class labels used below, per the sprint's test-quality rules:
//   GATE             — the assertion fails against unguarded code.
//   POSITIVE CONTROL — the assertion holds by construction; it locks a value in
//                      place but cannot demonstrate the guard can fail.

#include "atx/vol/listed_definitions_cache.hpp"

#include "atx/core/hash.hpp"              // hash_bytes (independent abi_fold recompute)
#include "atx/vol/data.hpp"               // iso_to_ns
#include "atx/vol/detail/archive_util.hpp" // crc32c (independent CRC recompute)
#include "atx/vol/listed_opra.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace atx::vol {
namespace {

namespace fs = std::filesystem;

// ── Fixture ─────────────────────────────────────────────────────────────────

// Three trade dates x three contracts. Deliberately includes: two distinct
// dates that share a symbol, two distinct symbols that share a date, both
// boolean flags in both states, and multipliers that are NOT exactly
// representable round numbers so an EXPECT_EQ on the raw double is a real
// bit-for-bit assertion rather than a coincidence of small integers.
std::vector<ListedContractDefinition> sample_rows() {
  const std::int64_t d1 = iso_to_ns("2026-06-05T12:00:00Z");
  const std::int64_t d2 = iso_to_ns("2026-06-08T12:00:00Z");
  const std::int64_t d3 = iso_to_ns("2026-06-09T12:00:00Z");
  const std::int64_t x1 = iso_to_ns("2026-07-17T20:00:00Z");
  const std::int64_t x2 = iso_to_ns("2026-08-21T20:00:00Z");
  return {
      ListedContractDefinition{"2026-06-05", 101, "SPY   260717C00600000", d1, x1, 100.0, true, true,
                               0x0123456789abcdefull},
      ListedContractDefinition{"2026-06-05", 102, "SPY   260717P00600000", d1, x1, 99.7500000001,
                               true, false, 0xfedcba9876543210ull},
      ListedContractDefinition{"2026-06-08", 101, "SPY   260717C00600000", d2, x1, 100.0, false,
                               true, 0x00000000000000ffull},
      ListedContractDefinition{"2026-06-08", 307, "AAPL  260821C00250000", d2, x2, 100.25, false,
                               false, 0x8000000000000001ull},
      ListedContractDefinition{"2026-06-09", 999, "MSFT  260821P00400000", d3, x2,
                               0.1000000000000000055511151231257827, true, true, 7u},
  };
}

ListedDefinitionTable sample_table() {
  auto table = ListedDefinitionTable::create(sample_rows());
  EXPECT_TRUE(table) << (table ? std::string{} : table.error().to_string());
  return table ? std::move(*table) : ListedDefinitionTable{};
}

ListedDefinitionsCacheKey sample_key() {
  return definitions_cache_key("ATX_LISTED_DEFINITIONS\t1\nheader...\nrow...\n");
}

// Per-test private directory so a concurrent suite run cannot collide.
fs::path scratch_dir(std::string_view name) {
  const fs::path dir = fs::temp_directory_path() / ("atx_defs_cache_" + std::string(name));
  std::error_code ec;
  fs::remove_all(dir, ec);
  ec.clear();
  fs::create_directories(dir, ec);
  EXPECT_FALSE(ec) << "cannot create scratch dir " << dir.string();
  return dir;
}

std::vector<std::byte> read_all(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "cannot open " << path.string();
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(bytes.size());
  if (!bytes.empty()) {
    std::memcpy(out.data(), bytes.data(), bytes.size());
  }
  return out;
}

void write_all(const fs::path &path, const std::vector<std::byte> &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out) << "cannot open " << path.string() << " for writing";
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  ASSERT_TRUE(out.good());
}

ListedDefinitionsCacheHeader header_of(const std::vector<std::byte> &image) {
  ListedDefinitionsCacheHeader header{};
  EXPECT_GE(image.size(), sizeof header);
  std::memcpy(&header, image.data(), sizeof header);
  return header;
}

// Restamp both CRCs so an image whose payload was edited is once again
// self-consistent. Used ONLY by the test that isolates the table-fingerprint
// guard from the CRC guards.
void restamp_crcs(std::vector<std::byte> &image) {
  ListedDefinitionsCacheHeader header = header_of(image);
  header.payload_crc32c = detail::crc32c(image.data() + sizeof header, image.size() - sizeof header);
  header.header_crc32c = 0;
  std::memcpy(image.data(), &header, sizeof header);
  header.header_crc32c = detail::crc32c(image.data(), sizeof header);
  std::memcpy(image.data(), &header, sizeof header);
}

void expect_rows_bit_identical(const ListedDefinitionTable &lhs, const ListedDefinitionTable &rhs) {
  ASSERT_EQ(lhs.definitions().size(), rhs.definitions().size());
  ASSERT_GT(lhs.definitions().size(), 0u) << "anti-vacuity: an empty table proves nothing";
  for (std::size_t i = 0; i < lhs.definitions().size(); ++i) {
    const ListedContractDefinition &a = lhs.definitions()[i];
    const ListedContractDefinition &b = rhs.definitions()[i];
    EXPECT_EQ(a.trade_date, b.trade_date) << "row " << i;
    EXPECT_EQ(a.instrument_id, b.instrument_id) << "row " << i;
    EXPECT_EQ(a.raw_symbol, b.raw_symbol) << "row " << i;
    EXPECT_EQ(a.definition_ts_ns, b.definition_ts_ns) << "row " << i;
    EXPECT_EQ(a.expiry_ts_ns, b.expiry_ts_ns) << "row " << i;
    EXPECT_EQ(a.multiplier, b.multiplier) << "row " << i; // raw double, not NEAR
    EXPECT_EQ(a.standard_monthly, b.standard_monthly) << "row " << i;
    EXPECT_EQ(a.standard_deliverable, b.standard_deliverable) << "row " << i;
    EXPECT_EQ(a.source_fingerprint, b.source_fingerprint) << "row " << i;
  }
}

// ── Round trip ──────────────────────────────────────────────────────────────

// GATE for the encoder/decoder: every field of every row must survive the
// column-major blob, including the raw double bits and both flag bits.
TEST(ListedDefinitionsCache, CacheRoundTripReconstructsTableExactly) {
  const fs::path dir = scratch_dir("roundtrip");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_EQ(source.definitions().size(), 5u);

  ASSERT_TRUE(write_definitions_cache(file.string(), source, key))
      << "writer must publish the cache file";
  std::error_code ec;
  ASSERT_TRUE(fs::is_regular_file(file, ec)) << "cache file was not published";
  ASSERT_GT(fs::file_size(file, ec), sizeof(ListedDefinitionsCacheHeader))
      << "anti-vacuity: the published cache must carry a payload, not just a header";

  auto loaded = read_definitions_cache(file.string(), key);
  ASSERT_TRUE(loaded) << (loaded ? std::string{} : loaded.error().to_string());
  expect_rows_bit_identical(source, *loaded);
  EXPECT_EQ(loaded->fingerprint(), source.fingerprint());
  EXPECT_NE(loaded->fingerprint(), 0u);

  // The stamped fingerprint in the header must be the table's, not a placeholder.
  const ListedDefinitionsCacheHeader header = header_of(read_all(file));
  EXPECT_EQ(header.table_fingerprint, source.fingerprint());
  EXPECT_EQ(header.n_rows, source.definitions().size());
}

// GATE: `find` must still resolve on the reconstructed table — the round trip
// has to preserve the canonical sort order the lookup binary-searches, not just
// the field values.
TEST(ListedDefinitionsCache, ReconstructedTableStillResolvesLookups) {
  const fs::path dir = scratch_dir("lookup");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, key));
  auto loaded = read_definitions_cache(file.string(), key);
  ASSERT_TRUE(loaded) << (loaded ? std::string{} : loaded.error().to_string());

  for (const ListedContractDefinition &row : source.definitions()) {
    const ListedContractDefinition *found =
        loaded->find(row.trade_date, row.instrument_id, row.raw_symbol);
    ASSERT_NE(found, nullptr) << row.trade_date << " " << row.raw_symbol;
    EXPECT_EQ(found->expiry_ts_ns, row.expiry_ts_ns);
    EXPECT_EQ(found->multiplier, row.multiplier);
  }
  // Negative control: a key that is NOT in the table must miss.
  EXPECT_EQ(loaded->find("2026-06-05", 424242, "SPY   260717C00600000"), nullptr);
}

// ── Key ─────────────────────────────────────────────────────────────────────

// GATE: the key is derived from the source CONTENT, so two byte streams that
// differ anywhere must key differently and identical streams must key
// identically.
TEST(ListedDefinitionsCache, CacheKeyIsContentDerived) {
  const std::string a = "trade_date\tinstrument_id\n2026-06-05\t101\n";
  std::string b = a;
  b[b.size() - 2u] = '2'; // one byte, deep inside the stream

  const ListedDefinitionsCacheKey ka = definitions_cache_key(a);
  const ListedDefinitionsCacheKey kb = definitions_cache_key(b);
  const ListedDefinitionsCacheKey ka2 = definitions_cache_key(a);

  ASSERT_EQ(a.size(), b.size()) << "the perturbation must isolate CONTENT, not length";
  EXPECT_NE(ka.content_hash, kb.content_hash);
  EXPECT_NE(ka, kb);
  EXPECT_EQ(ka, ka2);
  EXPECT_EQ(ka.content_hash, ka2.content_hash);

  // A length-only difference must also move the key.
  const ListedDefinitionsCacheKey kc = definitions_cache_key(a + "\n");
  EXPECT_NE(ka.source_size, kc.source_size);
  EXPECT_NE(ka, kc);

  // The three non-content fields are populated, not left zero.
  EXPECT_EQ(ka.format_version, kDefinitionsCacheFormat);
  EXPECT_EQ(ka.parser_revision, kDefinitionsParserRevision);
  EXPECT_NE(ka.abi_fold, 0u);
  EXPECT_EQ(ka.abi_fold, definitions_cache_abi_fold());
  EXPECT_EQ(ka.source_size, a.size());
}

// GATE: `abi_fold` must actually depend on the struct's shape. Recomputing the
// fold over a DELIBERATELY DIFFERENT shape must produce a different value — a
// fold that ignored its input would return the same number for both.
TEST(ListedDefinitionsCache, AbiFoldMovesWhenTheEncodedShapeMoves) {
  const std::uint64_t live = definitions_cache_abi_fold();
  EXPECT_NE(live, 0u);
  EXPECT_EQ(live, definitions_cache_abi_fold()) << "the fold must be stable within a process";

  // Same construction as definitions_cache_abi_fold, with ONE offset perturbed:
  // this stands in for a field reorder in ListedContractDefinition.
  using D = ListedContractDefinition;
  const std::uint64_t perturbed_shape[] = {
      sizeof(D),
      alignof(D),
      offsetof(D, trade_date),
      sizeof(D::trade_date),
      offsetof(D, instrument_id) + 1u, // <- the reorder
      sizeof(D::instrument_id),
      offsetof(D, raw_symbol),
      sizeof(D::raw_symbol),
      offsetof(D, definition_ts_ns),
      sizeof(D::definition_ts_ns),
      offsetof(D, expiry_ts_ns),
      sizeof(D::expiry_ts_ns),
      offsetof(D, multiplier),
      sizeof(D::multiplier),
      offsetof(D, standard_monthly),
      sizeof(D::standard_monthly),
      offsetof(D, standard_deliverable),
      sizeof(D::standard_deliverable),
      offsetof(D, source_fingerprint),
      sizeof(D::source_fingerprint),
  };
  const std::uint64_t perturbed = atx::core::hash_bytes(
      static_cast<const void *>(perturbed_shape), sizeof perturbed_shape);
  EXPECT_NE(live, perturbed) << "abi_fold does not depend on the field offsets it claims to pin";
}

// ── Rejection paths ─────────────────────────────────────────────────────────

// GATE (the stale-serve gate). A cache written for one source must NEVER be
// served for a different source. This is the failure mode the whole key exists
// to prevent.
TEST(ListedDefinitionsCache, CacheRejectsAKeyFromDifferentSourceBytes) {
  const fs::path dir = scratch_dir("keymismatch");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey written = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, written));

  // NEGATIVE CONTROL: with the key it was written for, the file reads green.
  auto control = read_definitions_cache(file.string(), written);
  ASSERT_TRUE(control) << "control read must succeed, else the rejection below is vacuous";
  ASSERT_EQ(control->definitions().size(), 5u);

  ListedDefinitionsCacheKey other = written;
  other.content_hash ^= 1ull; // one bit of the source's content hash
  EXPECT_FALSE(read_definitions_cache(file.string(), other))
      << "a cache written for other bytes was served";

  other = written;
  other.source_size += 1u;
  EXPECT_FALSE(read_definitions_cache(file.string(), other)) << "source_size not enforced";

  other = written;
  other.format_version += 1u;
  EXPECT_FALSE(read_definitions_cache(file.string(), other)) << "format_version not enforced";

  other = written;
  other.parser_revision += 1u;
  EXPECT_FALSE(read_definitions_cache(file.string(), other)) << "parser_revision not enforced";

  other = written;
  other.abi_fold ^= 1ull;
  EXPECT_FALSE(read_definitions_cache(file.string(), other)) << "abi_fold not enforced";
}

// GATE: a single flipped payload byte must be rejected, not decoded.
TEST(ListedDefinitionsCache, CacheRejectsTamperedPayload) {
  const fs::path dir = scratch_dir("tamper");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, key));

  std::vector<std::byte> image = read_all(file);
  const ListedDefinitionsCacheHeader header = header_of(image);
  ASSERT_GT(header.column_block_size, 0u);

  // NEGATIVE CONTROL: the untouched image reads green.
  ASSERT_TRUE(read_definitions_cache(file.string(), key))
      << "control read must succeed, else the rejection below is vacuous";

  // Flip the top bit of the first `source_fingerprint`. Chosen because ANY
  // non-zero fingerprint is accepted by ListedDefinitionTable::create, so a
  // reader that skipped its CRC would happily return a table here — the
  // rejection has to come from the format, not from downstream validation.
  const std::size_t target = static_cast<std::size_t>(header.column_block_offset) +
                             3u * 8u * static_cast<std::size_t>(header.n_rows) + 7u;
  ASSERT_LT(target, image.size());
  const std::byte before = image[target];
  image[target] = static_cast<std::byte>(std::to_integer<unsigned>(before) ^ 0x80u);
  ASSERT_NE(image[target], before) << "the perturbation did not change a byte";
  write_all(file, image);

  auto tampered = read_definitions_cache(file.string(), key);
  EXPECT_FALSE(tampered) << "a payload-tampered cache was served";
}

// GATE for the table-fingerprint check SPECIFICALLY. Same one-byte payload
// edit as above, but both CRCs are restamped so the image is internally
// self-consistent. Only the fingerprint stamped by the writer can catch it —
// this is the assertion that fails if the fingerprint verification is removed.
TEST(ListedDefinitionsCache, CacheRejectsTamperedPayloadEvenWithRepairedCrcs) {
  const fs::path dir = scratch_dir("tamper_crcfix");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, key));

  std::vector<std::byte> image = read_all(file);
  const ListedDefinitionsCacheHeader header = header_of(image);

  // NEGATIVE CONTROL 1: the untouched image reads green.
  ASSERT_TRUE(read_definitions_cache(file.string(), key));

  const std::size_t target = static_cast<std::size_t>(header.column_block_offset) +
                             3u * 8u * static_cast<std::size_t>(header.n_rows) + 7u;
  ASSERT_LT(target, image.size());
  image[target] = static_cast<std::byte>(std::to_integer<unsigned>(image[target]) ^ 0x80u);
  restamp_crcs(image);
  write_all(file, image);

  // NEGATIVE CONTROL 2: prove the restamp really did repair the CRCs, so the
  // rejection below is attributable to the fingerprint and not to a CRC the
  // test failed to fix.
  const std::vector<std::byte> repaired = read_all(file);
  const ListedDefinitionsCacheHeader fixed = header_of(repaired);
  ListedDefinitionsCacheHeader zeroed = fixed;
  zeroed.header_crc32c = 0;
  std::vector<std::byte> probe = repaired;
  std::memcpy(probe.data(), &zeroed, sizeof zeroed);
  EXPECT_EQ(fixed.header_crc32c, detail::crc32c(probe.data(), sizeof zeroed))
      << "the test failed to repair header_crc32c";
  EXPECT_EQ(fixed.payload_crc32c, detail::crc32c(repaired.data() + sizeof fixed,
                                                 repaired.size() - sizeof fixed))
      << "the test failed to repair payload_crc32c";

  auto tampered = read_definitions_cache(file.string(), key);
  EXPECT_FALSE(tampered) << "a CRC-repaired payload edit was served — the table_fingerprint "
                            "check is not load-bearing";
}

// GATE: a header byte edit must be rejected by header_crc32c.
TEST(ListedDefinitionsCache, CacheRejectsTamperedHeader) {
  const fs::path dir = scratch_dir("hdrtamper");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, key));
  ASSERT_TRUE(read_definitions_cache(file.string(), key)) << "control read must succeed";

  std::vector<std::byte> image = read_all(file);
  // `reserved` is covered by header_crc32c and read by nothing else, so only the
  // CRC can reject this.
  const std::size_t target = offsetof(ListedDefinitionsCacheHeader, reserved);
  image[target] = static_cast<std::byte>(std::to_integer<unsigned>(image[target]) ^ 0x01u);
  write_all(file, image);
  EXPECT_FALSE(read_definitions_cache(file.string(), key))
      << "a header-tampered cache was served";
}

// GATE: truncation at every scale must be rejected.
TEST(ListedDefinitionsCache, CacheRejectsTruncatedFile) {
  const fs::path dir = scratch_dir("truncate");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, key));
  ASSERT_TRUE(read_definitions_cache(file.string(), key)) << "control read must succeed";

  const std::vector<std::byte> full = read_all(file);
  ASSERT_GT(full.size(), sizeof(ListedDefinitionsCacheHeader) + 16u);

  for (const std::size_t keep : {std::size_t{0}, std::size_t{17},
                                 sizeof(ListedDefinitionsCacheHeader),
                                 sizeof(ListedDefinitionsCacheHeader) + 8u, full.size() - 1u}) {
    std::vector<std::byte> cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(keep));
    write_all(file, cut);
    EXPECT_FALSE(read_definitions_cache(file.string(), key))
        << "a file truncated to " << keep << " bytes was served";
  }

  // A file EXTENDED past its declared file_size is equally invalid.
  std::vector<std::byte> extended = full;
  extended.push_back(std::byte{0});
  write_all(file, extended);
  EXPECT_FALSE(read_definitions_cache(file.string(), key)) << "an extended file was served";
}

// GATE: a foreign or absent file is a miss, never a crash and never a serve.
TEST(ListedDefinitionsCache, CacheRejectsForeignAndAbsentFiles) {
  const fs::path dir = scratch_dir("foreign");
  const ListedDefinitionsCacheKey key = sample_key();

  EXPECT_FALSE(read_definitions_cache((dir / "does_not_exist.atxdefs").string(), key));

  const fs::path foreign = dir / "foreign.atxdefs";
  {
    std::ofstream out(foreign, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    const std::string junk(4096, 'Z');
    out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  }
  EXPECT_FALSE(read_definitions_cache(foreign.string(), key)) << "a foreign file was served";

  // A RunArchive is the most plausible wrong-file-type confusion.
  const fs::path pretend = dir / "run.atxrun";
  {
    std::ofstream out(pretend, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    const std::string magic = "ATXRUN01";
    std::string body(1024, '\0');
    out.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
  }
  EXPECT_FALSE(read_definitions_cache(pretend.string(), key)) << "a RunArchive was served";
}

// ── The header ABI ──────────────────────────────────────────────────────────

// POSITIVE CONTROL / regression lock. The compile-time pins live in the header
// (static_assert on sizeof AND every offsetof); this echoes them at runtime so a
// reader of a failing suite sees the numbers. It cannot fail independently of
// the header's own static_asserts and is NOT counted as coverage.
TEST(ListedDefinitionsCache, CacheHeaderAbiIsPinned) {
  EXPECT_EQ(sizeof(ListedDefinitionsCacheHeader), 128u);
  EXPECT_EQ(alignof(ListedDefinitionsCacheHeader), 8u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, magic), 0u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, file_size), 8u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, content_hash), 16u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, source_size), 24u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, abi_fold), 32u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, table_fingerprint), 40u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, n_rows), 48u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, string_table_offset), 56u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, string_table_size), 64u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, column_block_offset), 72u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, column_block_size), 80u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, format_version), 88u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, parser_revision), 92u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, header_crc32c), 96u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, payload_crc32c), 100u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, major), 104u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, minor), 106u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, header_size), 108u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, endian), 110u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, pointer_bits), 112u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, reserved_u16), 114u);
  EXPECT_EQ(offsetof(ListedDefinitionsCacheHeader, reserved), 116u);
}

// GATE: the stamped header fields must carry the declared constants, so a
// blob written by a future incompatible build is recognisable as such.
TEST(ListedDefinitionsCache, WrittenHeaderCarriesTheDeclaredIdentity) {
  const fs::path dir = scratch_dir("hdrid");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, key));

  const std::vector<std::byte> image = read_all(file);
  const ListedDefinitionsCacheHeader header = header_of(image);
  EXPECT_EQ(std::memcmp(header.magic, kDefinitionsCacheMagic, 8), 0);
  EXPECT_EQ(header.major, kDefinitionsCacheMajor);
  EXPECT_EQ(header.minor, kDefinitionsCacheMinor);
  EXPECT_EQ(header.header_size, sizeof(ListedDefinitionsCacheHeader));
  EXPECT_EQ(header.endian, 1u);
  EXPECT_EQ(header.pointer_bits, 64u);
  EXPECT_EQ(header.file_size, image.size());
  EXPECT_EQ(header.content_hash, key.content_hash);
  EXPECT_EQ(header.source_size, key.source_size);
  EXPECT_EQ(header.format_version, key.format_version);
  EXPECT_EQ(header.parser_revision, key.parser_revision);
  EXPECT_EQ(header.abi_fold, key.abi_fold);
  EXPECT_GE(header.column_block_offset, header.string_table_offset + header.string_table_size);
  EXPECT_EQ(header.column_block_offset % kDefinitionsCacheAlign, 0u);
}

// ── The seam ────────────────────────────────────────────────────────────────

// GATE: the cached seam must return exactly what the uncached read returns, on
// BOTH the miss (first call, no cache present) and the hit (second call).
TEST(ListedDefinitionsCache, CachedSeamAgreesWithTheDirectParseOnMissAndHit) {
  const fs::path dir = scratch_dir("seam");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";
  const ListedDefinitionTable source = sample_table();
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), source));

  auto direct = read_listed_definitions_file(tsv.string());
  ASSERT_TRUE(direct) << (direct ? std::string{} : direct.error().to_string());

  std::error_code ec;
  ASSERT_FALSE(fs::exists(cache, ec)) << "the cache dir must not exist before the miss";

  auto miss = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(miss) << (miss ? std::string{} : miss.error().to_string());
  expect_rows_bit_identical(*direct, *miss);

  // The miss must have PUBLISHED something — otherwise the "hit" below is just
  // a second miss and this test proves nothing about the cache.
  const ListedDefinitionsCacheKey key = [&] {
    std::ifstream in(tsv, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    return definitions_cache_key(bytes);
  }();
  const fs::path published = cache / definitions_cache_filename(key);
  ASSERT_TRUE(fs::is_regular_file(published, ec))
      << "the miss path did not publish " << published.string();
  ASSERT_TRUE(read_definitions_cache(published.string(), key))
      << "the published file is not readable as a cache — the hit below would be a miss";

  auto hit = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(hit) << (hit ? std::string{} : hit.error().to_string());
  expect_rows_bit_identical(*direct, *hit);
  EXPECT_EQ(hit->fingerprint(), direct->fingerprint());
}

// GATE: mutating the source TSV must NOT be served the stale cache — and the
// gate that stops it must be the KEY INSIDE the file, not merely the fact that
// the cache filename is content-addressed. So this test deliberately DEFEATS
// the filename defence: it copies the stale blob onto the path the new content
// hashes to, which is exactly what a filename collision (or a hand-edited cache
// directory) would produce. An unguarded reader serves five rows here.
TEST(ListedDefinitionsCache, CachedSeamDoesNotServeAStaleCacheAfterTheSourceChanges) {
  const fs::path dir = scratch_dir("stale");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), sample_table()));
  auto first = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(first);
  ASSERT_EQ(first->definitions().size(), 5u);

  const auto key_of_source = [&] {
    std::ifstream in(tsv, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    return definitions_cache_key(bytes);
  };
  const fs::path stale_blob = cache / definitions_cache_filename(key_of_source());
  std::error_code ec;
  ASSERT_TRUE(fs::is_regular_file(stale_blob, ec)) << "the first call published nothing";

  // Rewrite the source with ONE row dropped and one multiplier changed.
  std::vector<ListedContractDefinition> mutated = sample_rows();
  mutated.pop_back();
  mutated[0].multiplier = 50.0;
  auto mutated_table = ListedDefinitionTable::create(mutated);
  ASSERT_TRUE(mutated_table);
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), *mutated_table));

  // Plant the STALE blob at the NEW content's cache path.
  const fs::path collided = cache / definitions_cache_filename(key_of_source());
  ASSERT_NE(collided, stale_blob) << "the two sources must hash to different names";
  fs::copy_file(stale_blob, collided, fs::copy_options::overwrite_existing, ec);
  ASSERT_FALSE(ec) << "could not plant the stale blob: " << ec.message();
  ASSERT_TRUE(fs::is_regular_file(collided, ec));

  auto second = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(second) << (second ? std::string{} : second.error().to_string());
  ASSERT_EQ(second->definitions().size(), 4u) << "the stale 5-row cache was served";
  expect_rows_bit_identical(*mutated_table, *second);
  EXPECT_NE(second->fingerprint(), first->fingerprint());

  // And the third call must serve the NEW content from cache, still correctly.
  auto third = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(third);
  expect_rows_bit_identical(*mutated_table, *third);
}

// GATE: an unusable cache directory is a logged miss, never a failed read.
TEST(ListedDefinitionsCache, CachedSeamSurvivesAnUnusableCacheDirectory) {
  const fs::path dir = scratch_dir("badcache");
  const fs::path tsv = dir / "definitions.tsv";
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), sample_table()));

  auto direct = read_listed_definitions_file(tsv.string());
  ASSERT_TRUE(direct);

  // A FILE where the cache directory should be: create_directories cannot make
  // it, so the publish must fail — and the read must still succeed.
  const fs::path blocked = dir / "blocked";
  {
    std::ofstream out(blocked, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    out << "not a directory\n";
  }
  std::error_code ec;
  ASSERT_TRUE(fs::is_regular_file(blocked, ec));

  auto through = read_listed_definitions_cached(tsv.string(), blocked.string());
  ASSERT_TRUE(through) << "a publish failure must be a logged miss, not an error";
  expect_rows_bit_identical(*direct, *through);

  // An EMPTY cache_dir disables the cache and degenerates to the direct read.
  auto uncached = read_listed_definitions_cached(tsv.string(), "");
  ASSERT_TRUE(uncached);
  expect_rows_bit_identical(*direct, *uncached);
}

// ── Measurement harness (NOT a gate) ────────────────────────────────────────
//
// Skipped unless ATX_T7_DEFINITIONS_TSV names a real definitions.tsv. It exists
// so the load path can be measured on a PRODUCTION-SIZED file — the committed
// fixtures are five rows and say nothing about the economics of the format.
// It asserts correctness (the cached table must equal the parsed one) but its
// output, not its verdict, is the point.
TEST(ListedDefinitionsCache, MeasureLoadPathOnARealDefinitionsFile) {
  std::string path;
#if defined(_WIN32)
  char *raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, "ATX_T7_DEFINITIONS_TSV") == 0 && raw != nullptr) {
    path.assign(raw);
    std::free(raw);
  }
#else
  if (const char *raw = std::getenv("ATX_T7_DEFINITIONS_TSV")) {
    path.assign(raw);
  }
#endif
  if (path.empty()) {
    GTEST_SKIP() << "set ATX_T7_DEFINITIONS_TSV to a definitions.tsv to measure the load path";
  }
  std::error_code ec;
  ASSERT_TRUE(fs::is_regular_file(fs::path{path}, ec)) << path << " is not a file";

  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  // 1. Today's path: whole-file read + parse.
  const auto t0 = Clock::now();
  auto parsed = read_listed_definitions_file(path);
  const auto t1 = Clock::now();
  ASSERT_TRUE(parsed) << (parsed ? std::string{} : parsed.error().to_string());
  ASSERT_GT(parsed->definitions().size(), 0u);

  // 2. The key's own price over the same bytes, held resident as the parse path
  //    already holds them.
  std::string bytes;
  {
    std::ifstream in(fs::path{path}, std::ios::binary);
    ASSERT_TRUE(in);
    bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  const auto t2 = Clock::now();
  const ListedDefinitionsCacheKey key = definitions_cache_key(bytes);
  const auto t3 = Clock::now();

  // 3. fingerprint() in isolation: LAZY, so this call is what pays for the
  //    canonical serialization the writer stamps and the reader verifies.
  const auto t4 = Clock::now();
  const std::uint64_t fp = parsed->fingerprint();
  const auto t5 = Clock::now();
  EXPECT_NE(fp, 0u);

  const fs::path dir = scratch_dir("measure");
  const fs::path file = dir / definitions_cache_filename(key);

  // 4. Write.
  const auto t6 = Clock::now();
  const Status wrote = write_definitions_cache(file.string(), *parsed, key);
  const auto t7 = Clock::now();
  ASSERT_TRUE(wrote) << wrote.error().to_string();

  // 5. Read back (cold-ish first, then a warmed repeat).
  const auto t8 = Clock::now();
  auto loaded1 = read_definitions_cache(file.string(), key);
  const auto t9 = Clock::now();
  ASSERT_TRUE(loaded1) << (loaded1 ? std::string{} : loaded1.error().to_string());
  const auto t10 = Clock::now();
  auto loaded2 = read_definitions_cache(file.string(), key);
  const auto t11 = Clock::now();
  ASSERT_TRUE(loaded2) << (loaded2 ? std::string{} : loaded2.error().to_string());

  ASSERT_EQ(loaded2->definitions().size(), parsed->definitions().size());
  EXPECT_EQ(loaded2->fingerprint(), fp);

  const double parse_ms = ms(t0, t1);
  const double key_ms = ms(t2, t3);
  const double fingerprint_ms = ms(t4, t5);
  const double write_ms = ms(t6, t7);
  const double read_cold_ms = ms(t8, t9);
  const double read_warm_ms = ms(t10, t11);
  std::printf("\n[T7 measure] source            = %s\n", path.c_str());
  std::printf("[T7 measure] source_bytes      = %zu\n", bytes.size());
  std::printf("[T7 measure] rows              = %zu\n", parsed->definitions().size());
  std::printf("[T7 measure] cache_bytes       = %llu\n",
              static_cast<unsigned long long>(fs::file_size(file, ec)));
  std::printf("[T7 measure] parse_ms          = %.3f\n", parse_ms);
  std::printf("[T7 measure] key_hash_ms       = %.3f\n", key_ms);
  std::printf("[T7 measure] fingerprint_ms    = %.3f\n", fingerprint_ms);
  std::printf("[T7 measure] cache_write_ms    = %.3f\n", write_ms);
  std::printf("[T7 measure] cache_read_ms(1)  = %.3f\n", read_cold_ms);
  std::printf("[T7 measure] cache_read_ms(2)  = %.3f\n", read_warm_ms);
  std::printf("[T7 measure] net_ratio(1)      = %.3fx\n", parse_ms / (key_ms + read_cold_ms));
  std::printf("[T7 measure] net_ratio(2)      = %.3fx\n", parse_ms / (key_ms + read_warm_ms));
  std::printf("[T7 measure] net_ratio_no_fp(2)= %.3fx\n",
              parse_ms / (key_ms + read_warm_ms - fingerprint_ms));
  std::fflush(stdout);
}

} // namespace
} // namespace atx::vol
