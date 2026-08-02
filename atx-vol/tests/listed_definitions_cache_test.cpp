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

#include "atx/vol/research/listed_definitions_cache.hpp"

#include "atx/core/hash.hpp"              // hash_bytes (independent wire ABI fold)
#include "atx/vol/data.hpp"               // iso_to_ns
#include "atx/vol/detail/archive_util.hpp" // crc32c (independent CRC recompute)
#include "atx/vol/listed_opra.hpp"
#include "atx/vol/research/run_diagnostics.hpp"     // PhaseTimer (definitions_cache hit/miss phase, review I6)

#include <gtest/gtest.h>

#include <algorithm>
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

#if defined(_WIN32)
// clang-format off
#include <windows.h> // GetCurrentProcessId — scratch dirs must be per-process
#include <psapi.h>   // GetProcessMemoryInfo / EmptyWorkingSet (T7 fix round 1, I5 peak-RSS harness)
// clang-format on
#else
#include <unistd.h> // getpid
#endif

namespace atx::vol {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] unsigned long current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<unsigned long>(::GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

// ── Peak resident-memory measurement (T7 fix round 1, I5) ──────────────────
//
// `EmptyWorkingSet` trims the calling process's resident set to (near) zero;
// Windows then re-establishes `PeakWorkingSetSize` from whatever residency
// accumulates AFTER that trim. Calling it immediately before a step and
// reading `PeakWorkingSetSize` immediately after therefore gives an ISOLATED
// high-water mark for that step alone, not a process-lifetime maximum
// contaminated by everything measured earlier in the same test binary. Both
// calls resolve against kernel32 (PSAPI_VERSION 2 on this SDK), but the test
// target links `psapi` explicitly so this does not depend on that macro.
#if defined(_WIN32)
[[nodiscard]] std::uint64_t peak_working_set_bytes() noexcept {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)) != 0) {
    return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
  }
  return 0u;
}
void trim_working_set_and_reset_peak() noexcept { ::EmptyWorkingSet(::GetCurrentProcess()); }
#else
[[nodiscard]] std::uint64_t peak_working_set_bytes() noexcept { return 0u; }
void trim_working_set_and_reset_peak() noexcept {}
#endif

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

// Per-test, per-PROCESS private directory. The pid is load-bearing, not
// decoration: this function opens with `remove_all`, so without it two
// concurrent `atx-vol-tests.exe` runs of the same test would delete each other's
// fixture mid-test (Wave E T7 review M2 — the original comment claimed collision
// safety the path did not have).
fs::path scratch_dir(std::string_view name) {
  const fs::path dir =
      fs::temp_directory_path() /
      ("atx_defs_cache_" + std::string(name) + "_" + std::to_string(current_process_id()));
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

// The per-field enumeration below is for DIAGNOSABILITY — it names the field
// that moved. It is deliberately NOT the assertion of record, because an
// enumeration by name is structurally blind to a field that is later added to
// `ListedContractDefinition`: it would simply not be compared. The assertion of
// record is the whole-struct `operator==`, which is `= default`ed and therefore
// covers EVERY member, including one added after this line was written.
//
// (The primary defence against that scenario is the compile-time shape pin in
// listed_definitions_cache.hpp — an added field stops the build. This is the
// belt to that pin's braces, and it costs one line.)
void expect_rows_bit_identical(const ListedDefinitionTable &lhs, const ListedDefinitionTable &rhs) {
  ASSERT_EQ(lhs.definitions().size(), rhs.definitions().size());
  ASSERT_GT(lhs.definitions().size(), 0u) << "anti-vacuity: an empty table proves nothing";
  for (std::size_t i = 0; i < lhs.definitions().size(); ++i) {
    const ListedContractDefinition &a = lhs.definitions()[i];
    const ListedContractDefinition &b = rhs.definitions()[i];
    EXPECT_TRUE(a == b) << "row " << i
                        << ": whole-struct equality (covers members not enumerated below)";
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

// GATE: `abi_fold` must actually depend on the encoded wire schema. Recomputing
// the fold over a DELIBERATELY DIFFERENT schema must produce a different value —
// a fold that ignored its input would return the same number for both.
TEST(ListedDefinitionsCache, AbiFoldMovesWhenTheEncodedShapeMoves) {
  const std::uint64_t live = definitions_cache_abi_fold();
  EXPECT_NE(live, 0u);
  EXPECT_EQ(live, definitions_cache_abi_fold()) << "the fold must be stable within a process";

  // Same construction as definitions_cache_abi_fold, with ONE offset perturbed:
  // this stands in for a fixed-width wire-column reorder.
  using D = ListedDefinitionsCacheWireRowSchema;
  const std::uint64_t perturbed_shape[] = {
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
      offsetof(D, instrument_id) + 1u, // <- the reorder
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
  const std::uint64_t perturbed = atx::core::hash_bytes(
      static_cast<const void *>(perturbed_shape), sizeof perturbed_shape);
  EXPECT_NE(live, perturbed) << "abi_fold does not depend on the field offsets it claims to pin";
}

// GATE on the property the WHOLE format rests on, and which its own dependency
// disclaims. `atx/core/hash.hpp:13-15` says hash_bytes returns the same u64 for
// identical bytes "within one process — NOT stable across process restarts or
// platforms". Every one of the five key fields is compared on a cache read, so
// an unstable `content_hash` is never a bad serve — but it IS a permanent 100%
// miss that re-publishes a ~300 MB blob on every single run, forever, and since
// `abi_fold` is hash-derived too it would also change the cache FILENAME every
// run, so the directory would grow without bound. That is invisible in
// correctness terms and visible only in wall time, which is why it is pinned
// here rather than left to a header comment.
//
// PROCEDURE ACTUALLY RUN (Wave E T7 fix 1, item 2): this exact test binary was
// invoked as three SEPARATE OS process launches of
// `atx-vol-tests.exe --gtest_filter=ListedDefinitionsCache.ContentHashAndAbiFoldAreStableAcrossProcesses`,
// each its own `bin\atx-vol-tests.exe` invocation from a fresh shell command (not
// three assertions inside one run — a single process could not test cross-
// process stability). All three printed the identical
// `content_hash=0xbe2185a9042c2062 abi_fold=0x3ab6dd71e67cd631`; see the fix
// report for the three raw stdout captures. If an ankerl bump or a compiler
// change ever moves either value, this test goes RED and
// `kDefinitionsParserRevision` / the cache's viability must be revisited — that
// is the intended failure, not a nuisance.
TEST(ListedDefinitionsCache, ContentHashAndAbiFoldAreStableAcrossProcesses) {
  // A fixed byte string, not a file: the property under test is the hash, not
  // the I/O. Includes a NUL and a high byte so the whole range is exercised.
  static const std::string kProbe =
      std::string("ATX_LISTED_DEFINITIONS\t1\n", 25) + std::string(1, '\0') +
      std::string("\xff\x00\x7f zebra 2026-06-05\t101\tSPY   260717C00600000\n", 48);
  ASSERT_EQ(kProbe.size(), 74u) << "anti-vacuity: the probe is not the string this test pins";

  const ListedDefinitionsCacheKey key = definitions_cache_key(kProbe);
  std::printf("[T7 hash-stability] content_hash = 0x%016llx  abi_fold = 0x%016llx\n",
              static_cast<unsigned long long>(key.content_hash),
              static_cast<unsigned long long>(key.abi_fold));
  std::fflush(stdout);

  EXPECT_EQ(key.content_hash, 0xbe2185a9042c2062ull)
      << "hash_bytes is no longer stable across processes/builds — ATXDEFS1 would miss forever";
  EXPECT_EQ(key.abi_fold, 0x309f3081df910150ull)
      << "definitions_cache_abi_fold moved: either the wire schema changed (see the shape pin) or "
         "hash_bytes is no longer stable";

  // Negative control: a one-byte difference must move the hash, so the equality
  // above is a real assertion and not a constant that any input would satisfy.
  std::string perturbed = kProbe;
  perturbed[0] = 'a';
  EXPECT_NE(definitions_cache_key(perturbed).content_hash, key.content_hash);
}

// ── detail::read_whole_file — the shared slurp (review I2) ──────────────────
//
// `read_listed_definitions_file` and `read_listed_definitions_cached` carried
// the same eight-line `istreambuf_iterator` slurp verbatim; both now call this
// one `fread`-based helper. The two properties that must hold for that swap to
// be safe are that the BYTES are identical to what the iterator form produced,
// and that the three failure statuses map onto the same caller messages the
// ifstream form produced. Both are gated here directly, on the helper, rather
// than only through its two callers.

// GATE (the byte-identity gate — this is the one that matters). The comparison
// target is an INDEPENDENT `istreambuf_iterator` slurp of the same file taken in
// this same test, i.e. literally the code path `read_whole_file` replaced, so
// the assertion is "the new form produces the old form's bytes" and not "the new
// form agrees with a constant somebody typed".
TEST(ListedDefinitionsCache, ReadWholeFileIsByteIdenticalToTheIteratorSlurp) {
  const fs::path dir = scratch_dir("readwholefile_bytes");
  const fs::path file = dir / "payload.bin";

  // Deliberately hostile to a text-mode read and larger than the helper's 64 KiB
  // drain chunk: embedded NULs, 0xFF, a bare CR, a CRLF and a lone LF. If the
  // helper ever opened in text mode, the CRLF would collapse and this fails.
  std::string expected;
  expected.reserve(200u * 1024u);
  for (int i = 0; i < 4096; ++i) {
    expected.append("row\t", 4);
    expected.push_back(static_cast<char>(i & 0xff));
    expected.append("\0\xff\r\n", 4);
    expected.append("\rmid\n", 5);
    expected.append("SPY   260717C00600000\t100.0\n", 28);
  }
  ASSERT_EQ(expected.size(), 4096u * 42u);
  ASSERT_GT(expected.size(), 64u * 1024u) << "anti-vacuity: must exceed the helper's chunk size";
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    out.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    ASSERT_TRUE(out.good());
  }

  // The independent reference: the exact idiom that used to live in both
  // callers, run here against the same file.
  std::ifstream ref_stream(file, std::ios::binary);
  ASSERT_TRUE(ref_stream);
  const std::string reference((std::istreambuf_iterator<char>(ref_stream)),
                              std::istreambuf_iterator<char>());
  ASSERT_EQ(reference.size(), expected.size()) << "anti-vacuity: the reference slurp read nothing";

  std::string got = "STALE CONTENT THAT MUST BE CLEARED";
  ASSERT_EQ(detail::read_whole_file(file.string(), got), detail::FileReadStatus::Ok);

  EXPECT_EQ(got.size(), reference.size());
  EXPECT_TRUE(got == reference) << "read_whole_file's bytes differ from the iterator slurp's";
  // Report the first divergence rather than dumping 168 KB into the log.
  // Parenthesised so <windows.h>'s `min` macro cannot capture the call.
  const std::size_t n = (std::min)(got.size(), reference.size());
  std::size_t first_diff = n;
  for (std::size_t i = 0; i < n; ++i) {
    if (got[i] != reference[i]) {
      first_diff = i;
      break;
    }
  }
  EXPECT_EQ(first_diff, n) << "first differing byte at offset " << first_diff;

  // NEGATIVE CONTROL: the comparator above must be able to say False. Perturb one
  // byte of the reference and show the same comparison fails.
  std::string perturbed = reference;
  perturbed[perturbed.size() / 2] = static_cast<char>(perturbed[perturbed.size() / 2] ^ 0x01);
  EXPECT_FALSE(got == perturbed) << "the byte comparison cannot fail and therefore gates nothing";
}

// GATE. An empty file is `Ok` with an empty result, not a failure. The helper
// skips the sized read entirely when `file_size` is 0, so this exercises the
// drain loop as the only reader.
TEST(ListedDefinitionsCache, ReadWholeFileHandlesAnEmptyFile) {
  const fs::path dir = scratch_dir("readwholefile_empty");
  const fs::path file = dir / "empty.bin";
  { std::ofstream out(file, std::ios::binary | std::ios::trunc); }
  std::error_code ec;
  ASSERT_TRUE(fs::exists(file, ec));
  ASSERT_EQ(fs::file_size(file, ec), 0u) << "anti-vacuity: the fixture is not empty";

  std::string got = "STALE";
  EXPECT_EQ(detail::read_whole_file(file.string(), got), detail::FileReadStatus::Ok);
  EXPECT_TRUE(got.empty()) << "out was not cleared, size=" << got.size();
}

// GATE. An absent path is `NotFound` — which is what makes
// `read_listed_definitions_file` still say "listed definitions: file not found".
TEST(ListedDefinitionsCache, ReadWholeFileReportsNotFoundForAnAbsentPath) {
  const fs::path dir = scratch_dir("readwholefile_absent");
  const fs::path file = dir / "no_such_file.bin";
  std::error_code ec;
  ASSERT_FALSE(fs::exists(file, ec)) << "anti-vacuity: the fixture path exists";

  std::string got = "STALE";
  EXPECT_EQ(detail::read_whole_file(file.string(), got), detail::FileReadStatus::NotFound);
  EXPECT_TRUE(got.empty()) << "out was not cleared on failure, size=" << got.size();
}

// GATE. A DIRECTORY must be `NotFound`, not `IoError`. The helper special-cases
// it (an `fopen` of a directory succeeds on POSIX and would then fail the read,
// which would report IoError where the replaced `ifstream` form reported
// NotFound) and that special case had no coverage. Getting this wrong changes
// `read_listed_definitions_file`'s message from "file not found" to "read
// failed" — a string pinned by ~40 of Wave E Task 4's assertions.
TEST(ListedDefinitionsCache, ReadWholeFileReportsNotFoundForADirectory) {
  const fs::path dir = scratch_dir("readwholefile_dir");
  std::error_code ec;
  ASSERT_TRUE(fs::is_directory(dir, ec)) << "anti-vacuity: the fixture is not a directory";

  std::string got = "STALE";
  EXPECT_EQ(detail::read_whole_file(dir.string(), got), detail::FileReadStatus::NotFound)
      << "a directory must report NotFound, not IoError";
  EXPECT_TRUE(got.empty()) << "out was not cleared on failure, size=" << got.size();

  // And the caller-visible consequence, end to end: the pinned message.
  const auto res = read_listed_definitions_file(dir.string());
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::NotFound);
  EXPECT_EQ(res.error().message(), "listed definitions: file not found");
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
//
// The check is now OPT-IN (default Off in Release), so this test FORCES IT ON.
// That is the point of an explicit flag over a silent weakening: the guarantee
// is still provable and still proved, it is simply not purchased on every
// production read. `CacheWithTheFingerprintCheckOffAcceptsWhatOnRejects` below
// is the other half — it states, on the SAME input, exactly what Off gives up.
TEST(ListedDefinitionsCache, CacheRejectsTamperedPayloadEvenWithRepairedCrcs) {
  const fs::path dir = scratch_dir("tamper_crcfix");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, key));

  std::vector<std::byte> image = read_all(file);
  const ListedDefinitionsCacheHeader header = header_of(image);

  // NEGATIVE CONTROL 1: the untouched image reads green, with the check ON.
  ASSERT_TRUE(read_definitions_cache(file.string(), key, DefinitionsCacheFingerprintCheck::On));

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

  auto tampered = read_definitions_cache(file.string(), key, DefinitionsCacheFingerprintCheck::On);
  EXPECT_FALSE(tampered) << "a CRC-repaired payload edit was served — the table_fingerprint "
                            "check is not load-bearing";
}

// The OTHER half of the flag, stated rather than left implicit: with the check
// Off, the very image the test above rejects IS served. This is a GATE on the
// flag's semantics — it fails if `Off` silently still runs the check, and it
// makes the cost of the default visible in the suite instead of in a comment.
//
// What Off does NOT give up is asserted on the same object: `create()`'s
// unconditional semantic re-validation still ran, and the CRCs still hold.
TEST(ListedDefinitionsCache, CacheWithTheFingerprintCheckOffAcceptsWhatOnRejects) {
  const fs::path dir = scratch_dir("fp_off");
  const fs::path file = dir / "defs.atxdefs";
  const ListedDefinitionTable source = sample_table();
  const ListedDefinitionsCacheKey key = sample_key();
  ASSERT_TRUE(write_definitions_cache(file.string(), source, key));

  std::vector<std::byte> image = read_all(file);
  const ListedDefinitionsCacheHeader header = header_of(image);

  // NEGATIVE CONTROL: the untouched image reads green under BOTH settings.
  ASSERT_TRUE(read_definitions_cache(file.string(), key, DefinitionsCacheFingerprintCheck::On));
  ASSERT_TRUE(read_definitions_cache(file.string(), key, DefinitionsCacheFingerprintCheck::Off));

  // Same one-byte edit to source_fingerprint[0], both CRCs restamped.
  const std::size_t target = static_cast<std::size_t>(header.column_block_offset) +
                             3u * 8u * static_cast<std::size_t>(header.n_rows) + 7u;
  ASSERT_LT(target, image.size());
  image[target] = static_cast<std::byte>(std::to_integer<unsigned>(image[target]) ^ 0x80u);
  restamp_crcs(image);
  write_all(file, image);

  EXPECT_FALSE(read_definitions_cache(file.string(), key, DefinitionsCacheFingerprintCheck::On))
      << "control: On must still reject this image";
  auto served = read_definitions_cache(file.string(), key, DefinitionsCacheFingerprintCheck::Off);
  ASSERT_TRUE(served) << "Off is still running the fingerprint check";
  EXPECT_EQ(served->definitions().size(), source.definitions().size());
  EXPECT_NE(served->fingerprint(), header.table_fingerprint)
      << "anti-vacuity: the edit must actually have moved the table's fingerprint, or this "
         "test would pass on an unperturbed file";
}

// CONFIGURATION PIN, not coverage. Records which way this build is compiled, so
// a reader of the suite can tell whether the 1.4-3.8 s per-read fingerprint
// verification is in the shipped default. Flipping
// ATX_DEFS_CACHE_VERIFY_FINGERPRINT is a deliberate act and must move this line.
TEST(ListedDefinitionsCache, FingerprintCheckDefaultsOffInThisBuild) {
  EXPECT_EQ(kDefinitionsCacheFingerprintDefault, DefinitionsCacheFingerprintCheck::Off);
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

// POSITIVE CONTROL / regression lock. The fixed-width wire projection owns the
// portable sizeof/offsetof pins. The runtime row remains guarded separately by
// ordered designated initialization plus structured binding, so adding,
// removing, reordering or retyping a `ListedContractDefinition` member stops the
// build even though its std::string-bearing object layout is not an on-disk ABI.
TEST(ListedDefinitionsCache, WireRowAbiAndRuntimeShapeArePinned) {
  const ListedContractDefinition probe{};
  definitions_cache_payload_shape_pin(probe);
  using W = ListedDefinitionsCacheWireRowSchema;
  EXPECT_EQ(sizeof(W), 48u);
  EXPECT_EQ(alignof(W), 8u);
  EXPECT_EQ(offsetof(W, definition_ts_ns), 0u);
  EXPECT_EQ(offsetof(W, expiry_ts_ns), 8u);
  EXPECT_EQ(offsetof(W, multiplier), 16u);
  EXPECT_EQ(offsetof(W, source_fingerprint), 24u);
  EXPECT_EQ(offsetof(W, instrument_id), 32u);
  EXPECT_EQ(offsetof(W, trade_date_code), 36u);
  EXPECT_EQ(offsetof(W, raw_symbol_code), 40u);
  EXPECT_EQ(offsetof(W, flags), 44u);
  EXPECT_EQ(kDefinitionsCacheMonthlyFlag, 0x1u);
  EXPECT_EQ(kDefinitionsCacheDeliverableFlag, 0x2u);
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

// GATE (review I6, "the other half"). The seam's stderr HIT/MISS lines
// (`ca74f68`) are observability for a human tailing a log; they do not reach a
// `diagnostics` RunArchive reader, which has no other way to tell a fast run
// from a cached one. `read_listed_definitions_cached` now takes an optional
// `PhaseTimer *` and charges a `definitions_cache` phase whose `count` IS the
// hit/miss flag (1 = HIT, 0 = MISS) — the shape Task 8's brief specifies for
// this exact phase. Exercised directly on the seam rather than only through a
// CLI, since no CLI wiring exists yet (that is Task 8's job, out of scope
// here).
TEST(ListedDefinitionsCache, CachedSeamChargesADefinitionsCachePhaseWithHitMissCount) {
  const fs::path dir = scratch_dir("phase_timer");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), sample_table()));

  // First call: the cache dir does not exist yet, so this must be a MISS.
  PhaseTimer miss_timer{"definitions_cache"};
  auto miss = read_listed_definitions_cached(tsv.string(), cache.string(),
                                             kDefinitionsCacheFingerprintDefault, &miss_timer);
  ASSERT_TRUE(miss) << (miss ? std::string{} : miss.error().to_string());
  ASSERT_EQ(miss_timer.phases().size(), 1u) << "the phase must be charged exactly once";
  EXPECT_EQ(miss_timer.phases().front().name, "definitions_cache");
  EXPECT_EQ(miss_timer.phases().front().count, 0u) << "a MISS must charge count=0";

  // Second call: the first call published the cache, so this must be a HIT.
  PhaseTimer hit_timer{"definitions_cache"};
  auto hit = read_listed_definitions_cached(tsv.string(), cache.string(),
                                            kDefinitionsCacheFingerprintDefault, &hit_timer);
  ASSERT_TRUE(hit) << (hit ? std::string{} : hit.error().to_string());
  ASSERT_EQ(hit_timer.phases().size(), 1u);
  EXPECT_EQ(hit_timer.phases().front().count, 1u)
      << "a HIT must charge count=1 — this is the NEGATIVE CONTROL against the "
         "MISS assertion above: the same phase must be able to read either value";

  // NEGATIVE CONTROL: an empty `cache_dir` disables the cache entirely (per the
  // header) — it is never CONSULTED, so it must not be charged at all, not even
  // as a count=0 "miss". `add()` was never called, so `elapsed` is provably its
  // constructed zero, not merely a fast-but-real measurement.
  PhaseTimer disabled_timer{"definitions_cache"};
  auto disabled = read_listed_definitions_cached(
      tsv.string(), "", kDefinitionsCacheFingerprintDefault, &disabled_timer);
  ASSERT_TRUE(disabled);
  ASSERT_EQ(disabled_timer.phases().size(), 1u);
  EXPECT_EQ(disabled_timer.phases().front().count, 0u);
  EXPECT_EQ(disabled_timer.phases().front().elapsed, PhaseTimer::Duration::zero())
      << "a disabled cache must not be charged at all — disabled is not a miss";

  // NEGATIVE CONTROL: a null timer (the default) must not crash and must not be
  // required — it is the "economically identical to today" path the header
  // promises.
  auto no_timer = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(no_timer);
}

// ── Task 8: staleness proofs at the SEAM ────────────────────────────────────
//
// Wave E Task 7 built the format and proved the low-level reader
// (`read_definitions_cache`) rejects a mismatched key (`CacheRejects...`
// above). Task 8's brief is a STRONGER claim, made at the seam a caller
// actually invokes: each case below asserts a MISS that falls through to a
// CORRECT full parse — never an error, and never a stale serve. That is
// genuinely new coverage, not a duplicate of the low-level tests: those only
// show the reader says no; these show the seam recovers with the right data.
//
// GATE (the headline test, Task 8 Step 1/2). Content changes but
// `source_size` does NOT. A cache keyed on `(source_size, format_version)`
// alone — the weakened key Task 8's brief calls for as a deliberate RED
// check — would still resolve to the SAME filename and would still pass
// GUARD 2, so the stale (pre-mutation) table would be SERVED. Only a
// content-derived key forces a miss here. See task-8-report.md for the
// observed failure when the key was temporarily weakened to prove this test
// is a real gate (Step 2) rather than vacuously green.
TEST(ListedDefinitionsCache, CachedSeamRejectsAndReparsesASameSizeContentMutation) {
  const fs::path dir = scratch_dir("stale_samesize");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";

  auto source_table = ListedDefinitionTable::create(sample_rows());
  ASSERT_TRUE(source_table);
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), *source_table));

  auto first = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(first) << (first ? std::string{} : first.error().to_string());
  ASSERT_EQ(first->definitions().size(), 5u);

  // Mutate ONE byte of the TSV IN PLACE, preserving length EXACTLY: the last
  // character of the first data row is always a digit of `source_fingerprint`
  // (the row's last column, per kHeader's field order), so incrementing it
  // mod 10 changes the row's content without touching any delimiter, without
  // changing the row count, and without changing the file's byte length.
  const std::uintmax_t size_before = fs::file_size(tsv);
  {
    std::string bytes;
    ASSERT_EQ(detail::read_whole_file(tsv.string(), bytes), detail::FileReadStatus::Ok);
    const std::size_t first_nl = bytes.find('\n');
    ASSERT_NE(first_nl, std::string::npos) << "malformed fixture: no magic line";
    const std::size_t second_nl = bytes.find('\n', first_nl + 1);
    ASSERT_NE(second_nl, std::string::npos) << "malformed fixture: no header line";
    const std::size_t third_nl = bytes.find('\n', second_nl + 1);
    ASSERT_NE(third_nl, std::string::npos) << "malformed fixture: no first data row";
    ASSERT_GT(third_nl, second_nl + 1u);
    const std::size_t target = third_nl - 1u; // last byte of the first data row
    const char before = bytes[target];
    ASSERT_TRUE(before >= '0' && before <= '9')
        << "expected the row's last field (source_fingerprint) to end in a digit, got '" << before
        << "'";
    bytes[target] = static_cast<char>('0' + ((before - '0' + 1) % 10));
    ASSERT_NE(bytes[target], before) << "the perturbation did not change the byte";
    std::ofstream out(tsv, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good());
  }
  EXPECT_EQ(fs::file_size(tsv), size_before) << "anti-vacuity: this test's whole premise is a "
                                                "SAME-SIZE mutation";

  // Independent reference: parse the MUTATED bytes directly, bypassing the
  // cache entirely, so the assertion below is "the seam agrees with a fresh
  // parse of what is on disk NOW", not a constant this test invented.
  auto reference = read_listed_definitions_file(tsv.string());
  ASSERT_TRUE(reference) << (reference ? std::string{} : reference.error().to_string());
  EXPECT_NE(reference->fingerprint(), first->fingerprint())
      << "anti-vacuity: the mutation must actually move the table, or this test proves nothing";

  auto second = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(second) << (second ? std::string{} : second.error().to_string());
  expect_rows_bit_identical(*reference, *second);
  EXPECT_EQ(second->fingerprint(), reference->fingerprint());
  EXPECT_NE(second->fingerprint(), first->fingerprint())
      << "the STALE (pre-mutation) table was served";
}

// GATE. Content changes AND `source_size` moves with it (a row is added).
// Weaker in isolation than the same-size case above (a size-only key would
// already catch this), but it is one of the five cases the brief enumerates
// and it exercises a different code path: the appended row changes both
// `content_hash` and `source_size`, so BOTH the cache filename and GUARD 2
// move.
TEST(ListedDefinitionsCache, CachedSeamRejectsAndReparsesADifferentSizeContentChange) {
  const fs::path dir = scratch_dir("stale_diffsize");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";

  auto source_table = ListedDefinitionTable::create(sample_rows());
  ASSERT_TRUE(source_table);
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), *source_table));

  auto first = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(first) << (first ? std::string{} : first.error().to_string());
  ASSERT_EQ(first->definitions().size(), 5u);
  const std::uintmax_t size_before = fs::file_size(tsv);

  // Append one well-formed extra row (a new instrument_id, so the key stays
  // unique) by rewriting the file from an expanded row set.
  std::vector<ListedContractDefinition> expanded_rows = sample_rows();
  expanded_rows.push_back(ListedContractDefinition{
      "2026-06-09", 555, "SPY   260821C00650000", iso_to_ns("2026-06-09T12:00:00Z"),
      iso_to_ns("2026-08-21T20:00:00Z"), 100.0, false, true, 0x1111111111111111ull});
  auto expanded_table = ListedDefinitionTable::create(std::move(expanded_rows));
  ASSERT_TRUE(expanded_table);
  ASSERT_EQ(expanded_table->definitions().size(), 6u);
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), *expanded_table));
  EXPECT_GT(fs::file_size(tsv), size_before) << "anti-vacuity: this test's premise is a "
                                                "DIFFERENT-size change";

  auto reference = read_listed_definitions_file(tsv.string());
  ASSERT_TRUE(reference) << (reference ? std::string{} : reference.error().to_string());
  ASSERT_EQ(reference->definitions().size(), 6u);

  auto second = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(second) << (second ? std::string{} : second.error().to_string());
  ASSERT_EQ(second->definitions().size(), 6u) << "the stale 5-row cache was served";
  expect_rows_bit_identical(*reference, *second);
  EXPECT_NE(second->fingerprint(), first->fingerprint());
}

// GATE. Simulates the Task 4/5 scenario named explicitly in the brief: the
// bytes on disk NEVER changed, only what the current build's parser DOES with
// them did (`kDefinitionsParserRevision` moved). A blob published under the
// PRIOR revision — same content_hash/source_size/format_version/abi_fold,
// stamped at the exact path the CURRENT build's key computes — must be a
// miss, not a serve, even though every OTHER identity field agrees.
TEST(ListedDefinitionsCache, CachedSeamRejectsAndReparsesWhenParserRevisionMovedSincePublish) {
  const fs::path dir = scratch_dir("stale_parserrev");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";
  std::error_code ec;
  ASSERT_TRUE(fs::create_directories(cache, ec) || fs::is_directory(cache, ec));

  auto source_table = ListedDefinitionTable::create(sample_rows());
  ASSERT_TRUE(source_table);
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), *source_table));

  std::string bytes;
  ASSERT_EQ(detail::read_whole_file(tsv.string(), bytes), detail::FileReadStatus::Ok);
  const ListedDefinitionsCacheKey real_key = definitions_cache_key(bytes);
  ASSERT_GT(real_key.parser_revision, 0u) << "cannot construct a revision one below zero";

  ListedDefinitionsCacheKey stale_key = real_key;
  stale_key.parser_revision -= 1u;
  ASSERT_NE(stale_key, real_key);

  // A DECOY table (4 rows, not 5) stamped under the stale key, planted at the
  // exact path the CURRENT build's real key resolves to — exactly what a
  // pre-Task-4/5 cache publish would leave sitting there.
  std::vector<ListedContractDefinition> decoy_rows = sample_rows();
  decoy_rows.pop_back();
  auto decoy_table = ListedDefinitionTable::create(std::move(decoy_rows));
  ASSERT_TRUE(decoy_table);
  ASSERT_EQ(decoy_table->definitions().size(), 4u);

  const fs::path planted = cache / definitions_cache_filename(real_key);
  ASSERT_TRUE(write_definitions_cache(planted.string(), *decoy_table, stale_key));
  // NEGATIVE CONTROL: readable under its OWN (stale) key, so the rejection
  // below is attributable to the revision mismatch, not a broken plant.
  ASSERT_TRUE(read_definitions_cache(planted.string(), stale_key))
      << "the planted decoy is not even readable under its own key";

  auto served = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(served) << (served ? std::string{} : served.error().to_string());
  ASSERT_EQ(served->definitions().size(), source_table->definitions().size())
      << "the parser-revision-stale decoy (4 rows) was served instead of a fresh parse (5 rows)";
  expect_rows_bit_identical(*source_table, *served);
}

// GATE. Same shape as the parser-revision case, for `abi_fold`: a blob
// encoded by a differently-shaped `ListedContractDefinition` — same content,
// same size, same format/parser revision — must never be decoded as today's
// shape.
TEST(ListedDefinitionsCache, CachedSeamRejectsAndReparsesWhenAbiFoldMovedSincePublish) {
  const fs::path dir = scratch_dir("stale_abifold");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";
  std::error_code ec;
  ASSERT_TRUE(fs::create_directories(cache, ec) || fs::is_directory(cache, ec));

  auto source_table = ListedDefinitionTable::create(sample_rows());
  ASSERT_TRUE(source_table);
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), *source_table));

  std::string bytes;
  ASSERT_EQ(detail::read_whole_file(tsv.string(), bytes), detail::FileReadStatus::Ok);
  const ListedDefinitionsCacheKey real_key = definitions_cache_key(bytes);

  ListedDefinitionsCacheKey stale_key = real_key;
  stale_key.abi_fold ^= 1ull;
  ASSERT_NE(stale_key, real_key);

  std::vector<ListedContractDefinition> decoy_rows = sample_rows();
  decoy_rows.pop_back();
  auto decoy_table = ListedDefinitionTable::create(std::move(decoy_rows));
  ASSERT_TRUE(decoy_table);
  ASSERT_EQ(decoy_table->definitions().size(), 4u);

  const fs::path planted = cache / definitions_cache_filename(real_key);
  ASSERT_TRUE(write_definitions_cache(planted.string(), *decoy_table, stale_key));
  ASSERT_TRUE(read_definitions_cache(planted.string(), stale_key))
      << "the planted decoy is not even readable under its own key";

  auto served = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(served) << (served ? std::string{} : served.error().to_string());
  ASSERT_EQ(served->definitions().size(), source_table->definitions().size())
      << "the abi_fold-stale decoy (4 rows) was served instead of a fresh parse (5 rows)";
  expect_rows_bit_identical(*source_table, *served);
}

// GATE. A published hit whose payload CRC no longer validates (a genuine
// bit-flip corruption, CRCs left UNREPAIRED — unlike
// `CacheRejectsTamperedPayloadEvenWithRepairedCrcs`, which isolates GUARD 4)
// must be a MISS that falls through to a correct full parse, never a partial
// serve and never a propagated error.
TEST(ListedDefinitionsCache, CachedSeamRejectsAndReparsesACorruptedHit) {
  const fs::path dir = scratch_dir("stale_corrupt");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";

  auto source_table = ListedDefinitionTable::create(sample_rows());
  ASSERT_TRUE(source_table);
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), *source_table));

  auto first = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(first) << (first ? std::string{} : first.error().to_string());

  std::string bytes;
  ASSERT_EQ(detail::read_whole_file(tsv.string(), bytes), detail::FileReadStatus::Ok);
  const ListedDefinitionsCacheKey key = definitions_cache_key(bytes);
  const fs::path published = cache / definitions_cache_filename(key);
  std::error_code ec;
  ASSERT_TRUE(fs::is_regular_file(published, ec)) << "the first call did not publish a cache";

  // Flip the LAST byte of the published image WITHOUT restamping the CRCs — a
  // genuine on-disk corruption, not a self-consistent hand edit.
  std::vector<std::byte> image = read_all(published);
  ASSERT_GT(image.size(), sizeof(ListedDefinitionsCacheHeader) + 16u);
  const std::size_t target = image.size() - 1u;
  const std::byte before = image[target];
  image[target] = static_cast<std::byte>(std::to_integer<unsigned>(before) ^ 0x80u);
  ASSERT_NE(image[target], before) << "the perturbation did not change a byte";
  write_all(published, image);

  // NEGATIVE CONTROL: the low-level reader rejects the corrupted image
  // directly, so the seam-level rejection below is attributable to the same
  // guard, not a different bug.
  ASSERT_FALSE(read_definitions_cache(published.string(), key))
      << "the corruption did not actually break the CRC — the rejection below would be vacuous";

  auto second = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(second) << "a corrupted hit must be a MISS that falls through to a full parse, "
                         "never an error: "
                      << (second ? std::string{} : second.error().to_string());
  expect_rows_bit_identical(*source_table, *second);
  EXPECT_EQ(second->fingerprint(), source_table->fingerprint());
}

// POSITIVE CASE (Task 8's brief, exact name). A hit that is merely FAST and
// not IDENTICAL to what cache-disabled parsing produces is a defect — this is
// the cache's entire contract, proven field-for-field and by `fingerprint()`.
TEST(ListedDefinitionsCache, CacheHitEqualsFullParseExactly) {
  const fs::path dir = scratch_dir("hit_equals_full_parse");
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";

  auto source_table = ListedDefinitionTable::create(sample_rows());
  ASSERT_TRUE(source_table);
  ASSERT_TRUE(write_listed_definitions_file(tsv.string(), *source_table));

  // Cache DISABLED: an empty cache_dir degenerates to the direct parse.
  auto disabled = read_listed_definitions_cached(tsv.string(), "");
  ASSERT_TRUE(disabled) << (disabled ? std::string{} : disabled.error().to_string());

  // MISS (publishes), then a genuine WARM HIT.
  auto miss = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(miss) << (miss ? std::string{} : miss.error().to_string());
  auto hit = read_listed_definitions_cached(tsv.string(), cache.string());
  ASSERT_TRUE(hit) << (hit ? std::string{} : hit.error().to_string());

  expect_rows_bit_identical(*disabled, *hit);
  EXPECT_EQ(hit->fingerprint(), disabled->fingerprint());
  EXPECT_NE(hit->fingerprint(), 0u);
}

// ── Measurement harnesses (NOT gates) ───────────────────────────────────────
//
// All three below are skipped unless ATX_T7_DEFINITIONS_TSV names a real
// definitions.tsv — the committed fixtures are five rows and say nothing
// about the format's economics. Each asserts basic correctness but its
// PRINTED output, not its verdict, is the point.
//
// This section REPLACES the retired `MeasureLoadPathOnARealDefinitionsFile`,
// whose `net_ratio = parse_ms / (key_ms + read_ms)` put the ~730 MB source
// slurp in the numerator only (`parse_ms` = `read_listed_definitions_file`,
// slurp + parse) while the denominator's `key_ms`/`read_ms` ran over bytes
// ALREADY held resident — excluding the slurp the seam can never skip, since
// the key is content-derived and a hit must read the source before the cache
// can be consulted (Wave E T7 review, Critical C1). That test also computed
// `parse_ms / (key_ms + read_ms - fingerprint_ms)`, a ratio of two
// independently noisy small terms after subtraction; the reviewer's own rep 2
// printed 28.028x from that exact formula (review I4). Both defects are FIXED
// HERE by construction, not patched: the estimator is deleted, and every
// ratio below is timed end-to-end through the public seam a caller actually
// invokes, with the slurp inside every timed span that can reach it.
//
// Measurement protocol applied to all three tests (sprint fix-round 1, I3/I4):
//   * variants are INTERLEAVED within each rep — never all-of-A then all-of-B;
//   * a DISCARDED warm-up rep runs first and its numbers are PRINTED, not
//     hidden (I3 found a discarded, most-favourable rep folded silently into
//     a reported interval);
//   * sign counts and a distribution-free (min, max) interval are reported
//     over the RECORDED reps only;
//   * a +/-5% noise floor is applied SYMMETRICALLY to every ratio, tagging it
//     "no measurable difference" regardless of which direction it favours.

[[nodiscard]] std::string definitions_tsv_path_from_env() {
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
  return path;
}

// C1-corrected seam measurement. Measures the THREE quantities the fix-round
// worklist specifies, end to end, through the public functions a caller
// actually calls — not through internals timed in isolation:
//
//   t_direct = read_listed_definitions_file(path)                 (today's cost: slurp + parse)
//   t_hit    = read_listed_definitions_cached(path, published_dir) (slurp + key hash + cache read)
//   t_miss   = read_listed_definitions_cached(path, EMPTY dir)      (slurp + key hash + parse + publish)
//
// Both functions call `detail::read_whole_file` (item 1's `fread`), so neither
// side is credited an I/O win the other does not also get. Reported:
// `seam_ratio = t_direct / t_hit` and `cold_penalty = t_miss / t_direct`. Both
// are ratios of two multi-second, page-cache-warm quantities — no term here
// is small enough for the ratio to be noise-dominated the way the retired
// subtraction-based estimator was.
TEST(ListedDefinitionsCache, MeasureSeamEndToEndDirectHitAndMissOnARealDefinitionsFile) {
  const std::string path = definitions_tsv_path_from_env();
  if (path.empty()) {
    GTEST_SKIP() << "set ATX_T7_DEFINITIONS_TSV to a definitions.tsv to measure the seam";
  }
  std::error_code ec;
  ASSERT_TRUE(fs::is_regular_file(fs::path{path}, ec)) << path << " is not a file";

  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  const fs::path hit_dir = scratch_dir("seam_hit");
  const fs::path miss_dir = scratch_dir("seam_miss");

  // Untimed setup: publish `hit_dir` ONCE, before any measured rep, so every
  // `t_hit` measurement below reads a cache that is genuinely already
  // published — never the call that publishes it. This number is printed for
  // transparency even though it is not one of the three measured quantities.
  const auto setup_t0 = Clock::now();
  auto setup = read_listed_definitions_cached(path, hit_dir.string());
  const auto setup_t1 = Clock::now();
  ASSERT_TRUE(setup) << (setup ? std::string{} : setup.error().to_string());
  const std::size_t total_rows = setup->definitions().size();
  ASSERT_GT(total_rows, 0u);

  constexpr int kRecordedReps = 3;
  constexpr int kTotalReps = kRecordedReps + 1; // rep 0 is the discarded warm-up
  std::vector<double> t_direct;
  std::vector<double> t_hit;
  std::vector<double> t_miss;

  std::printf("\n[T7 seam] source                                = %s\n", path.c_str());
  std::printf("[T7 seam] rows                                   = %zu\n", total_rows);
  std::printf("[T7 seam] hit_dir prepublish (untimed setup) ms  = %.3f\n", ms(setup_t0, setup_t1));

  for (int rep = 0; rep < kTotalReps; ++rep) {
    // Rotate the order every rep so no variant is systematically first or
    // last within the sequence of reps (I3/I4: interleave, never
    // all-of-A-then-all-of-B).
    const int rot = rep % 3;
    const char *order_label = rot == 0 ? "direct,hit,miss" : rot == 1 ? "hit,miss,direct" : "miss,direct,hit";

    double d_ms = 0.0;
    double h_ms = 0.0;
    double m_ms = 0.0;
    for (int slot = 0; slot < 3; ++slot) {
      const int which = (rot + slot) % 3; // 0=direct 1=hit 2=miss
      if (which == 0) {
        const auto a = Clock::now();
        auto r = read_listed_definitions_file(path);
        const auto b = Clock::now();
        ASSERT_TRUE(r) << (r ? std::string{} : r.error().to_string());
        ASSERT_EQ(r->definitions().size(), total_rows);
        d_ms = ms(a, b);
      } else if (which == 1) {
        const auto a = Clock::now();
        auto r = read_listed_definitions_cached(path, hit_dir.string());
        const auto b = Clock::now();
        ASSERT_TRUE(r) << (r ? std::string{} : r.error().to_string());
        ASSERT_EQ(r->definitions().size(), total_rows);
        h_ms = ms(a, b);
      } else {
        // Force a MISS: wipe miss_dir's published cache immediately before
        // the timed call, so the timed call always sees an EMPTY cache dir
        // regardless of what a previous rep left behind. The wipe itself is
        // untimed — it is not part of the miss cost being measured.
        std::error_code rm_ec;
        fs::remove_all(miss_dir, rm_ec);
        fs::create_directories(miss_dir, rm_ec);
        const auto a = Clock::now();
        auto r = read_listed_definitions_cached(path, miss_dir.string());
        const auto b = Clock::now();
        ASSERT_TRUE(r) << (r ? std::string{} : r.error().to_string());
        ASSERT_EQ(r->definitions().size(), total_rows);
        m_ms = ms(a, b);
      }
    }

    const char *tag = rep == 0 ? "DISCARDED warm-up" : "recorded";
    std::printf("[T7 seam] rep%d (%-17s) order=%-15s  t_direct=%9.3f  t_hit=%9.3f  t_miss=%9.3f\n", rep,
               tag, order_label, d_ms, h_ms, m_ms);
    if (rep > 0) {
      t_direct.push_back(d_ms);
      t_hit.push_back(h_ms);
      t_miss.push_back(m_ms);
    }
  }
  std::fflush(stdout);

  ASSERT_EQ(t_direct.size(), static_cast<std::size_t>(kRecordedReps));

  // +/-5% applied SYMMETRICALLY: Step 1 of the original report found its own
  // within-run ratio agreeing to within 3.5 percentage points across reps it
  // called real, so a difference smaller than that either side of 1.0x is
  // flagged as noise for BOTH seam_ratio and cold_penalty — not just
  // whichever direction would flatter the format.
  constexpr double kNoiseFloor = 0.05;
  const auto floor_tag = [&](double ratio) {
    return (std::fabs(ratio - 1.0) < kNoiseFloor) ? " [within +/-5% noise floor]" : "";
  };

  std::vector<double> seam_ratio;
  std::vector<double> cold_penalty;
  int seam_sign_above_one = 0;
  int cold_sign_above_one = 0;
  for (int i = 0; i < kRecordedReps; ++i) {
    const double sr = t_direct[static_cast<std::size_t>(i)] / t_hit[static_cast<std::size_t>(i)];
    const double cp = t_miss[static_cast<std::size_t>(i)] / t_direct[static_cast<std::size_t>(i)];
    seam_ratio.push_back(sr);
    cold_penalty.push_back(cp);
    if (sr > 1.0) {
      ++seam_sign_above_one;
    }
    if (cp > 1.0) {
      ++cold_sign_above_one;
    }
    std::printf(
        "[T7 seam] rep%d  seam_ratio(t_direct/t_hit)=%.3fx%s   cold_penalty(t_miss/t_direct)=%.3fx%s\n",
        i + 1, sr, floor_tag(sr), cp, floor_tag(cp));
  }
  const auto [sr_min_it, sr_max_it] = std::minmax_element(seam_ratio.begin(), seam_ratio.end());
  const auto [cp_min_it, cp_max_it] = std::minmax_element(cold_penalty.begin(), cold_penalty.end());
  std::printf("[T7 seam] seam_ratio   sign %d/%d above 1.0x   distribution-free interval [%.3fx, %.3fx]\n",
             seam_sign_above_one, kRecordedReps, *sr_min_it, *sr_max_it);
  std::printf(
      "[T7 seam] cold_penalty sign %d/%d above 1.0x   distribution-free interval [%.3fx, %.3fx]\n",
      cold_sign_above_one, kRecordedReps, *cp_min_it, *cp_max_it);
  std::fflush(stdout);
}

// Item 3, "separately": what `fread` alone is worth, measured IN-PROCESS and
// INTERLEAVED against the `istreambuf_iterator` form it replaced — same file,
// both variants warm. This is the alternative P1 competes against (Wave E T7
// review Concern 3 / Critical C1): a one-function change with no new on-disk
// format and no stale-serve surface.
TEST(ListedDefinitionsCache, MeasureFreadVsIteratorThroughputOnARealDefinitionsFile) {
  const std::string path = definitions_tsv_path_from_env();
  if (path.empty()) {
    GTEST_SKIP() << "set ATX_T7_DEFINITIONS_TSV to a definitions.tsv to measure fread vs iterator";
  }
  std::error_code ec;
  ASSERT_TRUE(fs::is_regular_file(fs::path{path}, ec)) << path << " is not a file";

  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto mb_per_s = [](std::size_t bytes, double milliseconds) {
    return milliseconds > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (milliseconds / 1000.0)
                              : 0.0;
  };

  constexpr int kRecordedReps = 3;
  constexpr int kTotalReps = kRecordedReps + 1;
  std::vector<double> t_fread;
  std::vector<double> t_iter;
  std::size_t reference_size = 0;

  for (int rep = 0; rep < kTotalReps; ++rep) {
    const bool fread_first = (rep % 2) == 0; // alternate order every rep
    double fr_ms = 0.0;
    double it_ms = 0.0;
    std::size_t fr_bytes = 0;
    std::size_t it_bytes = 0;

    const auto do_fread = [&] {
      std::string out;
      const auto a = Clock::now();
      const detail::FileReadStatus st = detail::read_whole_file(path, out);
      const auto b = Clock::now();
      ASSERT_EQ(st, detail::FileReadStatus::Ok);
      fr_ms = ms(a, b);
      fr_bytes = out.size();
    };
    const auto do_iter = [&] {
      const auto a = Clock::now();
      std::ifstream in(fs::path{path}, std::ios::binary);
      ASSERT_TRUE(in);
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const auto b = Clock::now();
      it_ms = ms(a, b);
      it_bytes = bytes.size();
    };

    if (fread_first) {
      do_fread();
      do_iter();
    } else {
      do_iter();
      do_fread();
    }

    ASSERT_EQ(fr_bytes, it_bytes) << "the two slurps disagree on size";
    if (reference_size == 0u) {
      reference_size = fr_bytes;
    } else {
      ASSERT_EQ(fr_bytes, reference_size) << "the source file changed size mid-measurement";
    }

    const char *tag = rep == 0 ? "DISCARDED warm-up" : "recorded";
    std::printf(
        "[T7 throughput] rep%d (%-17s) order=%-10s  fread_ms=%8.3f (%7.1f MB/s)  iter_ms=%8.3f (%7.1f MB/s)\n",
        rep, tag, fread_first ? "fread,iter" : "iter,fread", fr_ms, mb_per_s(fr_bytes, fr_ms), it_ms,
        mb_per_s(it_bytes, it_ms));
    if (rep > 0) {
      t_fread.push_back(fr_ms);
      t_iter.push_back(it_ms);
    }
  }
  std::fflush(stdout);

  ASSERT_EQ(t_fread.size(), static_cast<std::size_t>(kRecordedReps));
  int fread_faster_count = 0;
  std::vector<double> ratio; // iter_ms / fread_ms — "fread is worth Nx"
  for (int i = 0; i < kRecordedReps; ++i) {
    const double r = t_iter[static_cast<std::size_t>(i)] / t_fread[static_cast<std::size_t>(i)];
    ratio.push_back(r);
    if (t_fread[static_cast<std::size_t>(i)] < t_iter[static_cast<std::size_t>(i)]) {
      ++fread_faster_count;
    }
  }
  const auto [r_min_it, r_max_it] = std::minmax_element(ratio.begin(), ratio.end());
  std::printf("[T7 throughput] fread-faster sign %d/%d   (iter_ms/fread_ms) distribution-free interval "
             "[%.2fx, %.2fx]\n",
             fread_faster_count, kRecordedReps, *r_min_it, *r_max_it);
  std::fflush(stdout);
}

// I5: write, and a fingerprint-VERIFIED read, each force the full
// `serialize_listed_definitions` transient Wave E T4 deliberately made lazy.
// `ca74f68` made GUARD 4 opt-in and OFF by default in Release, which removed
// the READ-side instance of this transient from the default path (confirmed
// below). The WRITE side is UNCONDITIONAL: `write_definitions_cache` always
// stamps a real `table.fingerprint()`, never a placeholder, because a header
// that did not carry a correct one would make GUARD 4 permanently unusable on
// that specific blob even for a caller who later turns the check on — the
// format's opt-in design depends on every blob being ready to be verified.
// That is a correctness requirement, not an oversight, so this harness does
// not attempt to remove the transient; it measures and reports the peak, per
// the fix-round worklist's explicit fallback ("say so plainly and report the
// measured peak").
//
// Fixed (NOT per-process) scratch path, deliberately NOT `scratch_dir()`:
// that helper suffixes the path with the calling PID specifically so
// concurrent runs cannot collide (review M2), but the three tests below need
// the OPPOSITE property — a cache PUBLISHED by one process invocation must be
// found by a SEPARATE, later invocation. That is required because of what a
// first attempt at this harness found empirically: Windows' `PeakWorkingSetSize`
// is a PROCESS-LIFETIME high-water mark, and `EmptyWorkingSet` trims CURRENT
// residency but does not reset it. Measuring three scenarios back to back in
// one process (write, then read/Off, then read/On) printed
// `before=after=3.955 GB, delta=0.000 GB` for ALL THREE, because the first
// scenario alone had already saturated the process's lifetime peak and
// nothing later in that same process needed more. That is reported in the fix
// round as raw evidence, not silently discarded, and is WHY each of the three
// tests below must be run ALONE, in its OWN process, in the stated order.
fs::path t7_transient_shared_cache_dir() {
  return fs::temp_directory_path() / "atx_t7_i5_transient_shared_cache";
}

// I5, scenario 1/3 — the WRITE side, through the REAL seam (a genuine MISS,
// exactly the path production takes on a sweep's first point). The review
// estimated seam peak ~2.8 GB on this 696 MB input by summing concurrently-
// resident buffers (source contents, parsed rows, the ~300 MB image, the
// fingerprint's ~730 MB serialize transient); this measures the process's
// actual peak WORKING SET directly. MUST be run ALONE
// (`--gtest_filter=ListedDefinitionsCache.MeasureSeamMissPeakTransient`) and
// FIRST of the three — it both measures the write-side peak and leaves a
// published cache on disk for the two HIT scenarios below.
TEST(ListedDefinitionsCache, MeasureSeamMissPeakTransient) {
  const std::string path = definitions_tsv_path_from_env();
  if (path.empty()) {
    GTEST_SKIP() << "set ATX_T7_DEFINITIONS_TSV to a definitions.tsv to measure the write-side transient";
  }
#if !defined(_WIN32)
  GTEST_SKIP() << "peak-working-set measurement is implemented for Windows only in this harness";
#else
  std::error_code ec;
  ASSERT_TRUE(fs::is_regular_file(fs::path{path}, ec)) << path << " is not a file";
  const fs::path dir = t7_transient_shared_cache_dir();
  fs::remove_all(dir, ec);

  trim_working_set_and_reset_peak();
  const std::uint64_t before = peak_working_set_bytes();
  PhaseTimer timer{"definitions_cache"};
  auto miss =
      read_listed_definitions_cached(path, dir.string(), kDefinitionsCacheFingerprintDefault, &timer);
  const std::uint64_t after = peak_working_set_bytes();
  ASSERT_TRUE(miss) << (miss ? std::string{} : miss.error().to_string());
  ASSERT_EQ(timer.phases().front().count, 0u) << "precondition broken: this call was not a MISS";

  const auto gb = [](std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  };
  std::printf("\n[T7 transient] MISS (write side, real seam)  rows=%zu\n", miss->definitions().size());
  std::printf("[T7 transient] peak_working_set: before=%.3f GB  after=%.3f GB  delta=%.3f GB\n", gb(before),
             gb(after), gb(after - before));
  std::fflush(stdout);
#endif
}

// I5, scenario 2/3 — a HIT with the fingerprint check OFF (this build's
// default, per `ca74f68`). Requires `MeasureSeamMissPeakTransient` to have
// already published the cache this reads (skips with a clear message
// otherwise) — run ALONE, in its OWN process, SECOND.
TEST(ListedDefinitionsCache, MeasureSeamHitPeakTransientCheckOff) {
  const std::string path = definitions_tsv_path_from_env();
  if (path.empty()) {
    GTEST_SKIP() << "set ATX_T7_DEFINITIONS_TSV to a definitions.tsv to measure the read-side transient";
  }
#if !defined(_WIN32)
  GTEST_SKIP() << "peak-working-set measurement is implemented for Windows only in this harness";
#else
  const fs::path dir = t7_transient_shared_cache_dir();
  std::error_code ec;
  if (!fs::is_directory(dir, ec) || fs::is_empty(dir, ec)) {
    GTEST_SKIP() << "run MeasureSeamMissPeakTransient first (alone) to publish " << dir.string();
  }
  ASSERT_TRUE(fs::is_regular_file(fs::path{path}, ec)) << path << " is not a file";

  trim_working_set_and_reset_peak();
  const std::uint64_t before = peak_working_set_bytes();
  PhaseTimer timer{"definitions_cache"};
  auto hit =
      read_listed_definitions_cached(path, dir.string(), DefinitionsCacheFingerprintCheck::Off, &timer);
  const std::uint64_t after = peak_working_set_bytes();
  ASSERT_TRUE(hit) << (hit ? std::string{} : hit.error().to_string());
  ASSERT_EQ(timer.phases().front().count, 1u)
      << "precondition broken: this call was not a HIT — run MeasureSeamMissPeakTransient first";

  const auto gb = [](std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  };
  std::printf("\n[T7 transient] HIT check=Off (this build's default)  rows=%zu\n",
             hit->definitions().size());
  std::printf("[T7 transient] peak_working_set: before=%.3f GB  after=%.3f GB  delta=%.3f GB\n", gb(before),
             gb(after), gb(after - before));
  std::fflush(stdout);
#endif
}

// I5, scenario 3/3 — a HIT with the fingerprint check explicitly ON, i.e.
// what the read side costs if `ATX_DEFS_CACHE_VERIFY_FINGERPRINT` (or an
// explicit `check = On`) is used. Same precondition as scenario 2 — run
// ALONE, in its OWN process, AFTER scenario 1 (order relative to scenario 2
// does not matter).
TEST(ListedDefinitionsCache, MeasureSeamHitPeakTransientCheckOn) {
  const std::string path = definitions_tsv_path_from_env();
  if (path.empty()) {
    GTEST_SKIP() << "set ATX_T7_DEFINITIONS_TSV to a definitions.tsv to measure the read-side transient";
  }
#if !defined(_WIN32)
  GTEST_SKIP() << "peak-working-set measurement is implemented for Windows only in this harness";
#else
  const fs::path dir = t7_transient_shared_cache_dir();
  std::error_code ec;
  if (!fs::is_directory(dir, ec) || fs::is_empty(dir, ec)) {
    GTEST_SKIP() << "run MeasureSeamMissPeakTransient first (alone) to publish " << dir.string();
  }
  ASSERT_TRUE(fs::is_regular_file(fs::path{path}, ec)) << path << " is not a file";

  trim_working_set_and_reset_peak();
  const std::uint64_t before = peak_working_set_bytes();
  PhaseTimer timer{"definitions_cache"};
  auto hit =
      read_listed_definitions_cached(path, dir.string(), DefinitionsCacheFingerprintCheck::On, &timer);
  const std::uint64_t after = peak_working_set_bytes();
  ASSERT_TRUE(hit) << (hit ? std::string{} : hit.error().to_string());
  ASSERT_EQ(timer.phases().front().count, 1u)
      << "precondition broken: this call was not a HIT — run MeasureSeamMissPeakTransient first";

  const auto gb = [](std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  };
  std::printf("\n[T7 transient] HIT check=On (fingerprint-verified)  rows=%zu\n", hit->definitions().size());
  std::printf("[T7 transient] peak_working_set: before=%.3f GB  after=%.3f GB  delta=%.3f GB\n", gb(before),
             gb(after), gb(after - before));
  std::fflush(stdout);
#endif
}

} // namespace
} // namespace atx::vol
