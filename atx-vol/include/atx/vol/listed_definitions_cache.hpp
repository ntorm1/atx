#pragma once

// ATXDEFS1 — a pre-parsed on-disk cache for the listed contract-definition
// table (`ListedDefinitionTable`).
//
// WHY. `definitions_parse` — the single call to `read_listed_definitions_file`
// at the head of both `build-schedule` and `run-backtest` — is the dominant
// phase of the listed-dispersion backtest. Measured on the shared Wave E fixture
// at commit fd52934 (a 730,526,177-byte `definitions.tsv`), it is 85-89% of the
// two subcommands' combined wall time even after the Wave E parse passes. The
// parsed table is a pure function of the definitions bytes, so it can be
// materialised once and reloaded.
//
// THE FAILURE MODE THIS FORMAT EXISTS TO PREVENT is serving a table that does
// not correspond to the file on disk. Every read is therefore FAIL-CLOSED: any
// header mismatch, key mismatch, CRC failure, short read or truncation returns
// `Err` and the caller falls through to a full parse. There is no partial serve
// and no "probably fine" path. A cache MISS is always correct; a cache HIT is
// only ever produced when five independent identity fields and two CRCs agree.
//
// ── The cache key ───────────────────────────────────────────────────────────
//
// `ListedDefinitionsCacheKey` folds five things:
//
//   (a) content_hash    — `atx::core::hash_bytes` over the FULL byte content of
//                         the source `definitions.tsv`.
//   (b) source_size     — that content's byte length.
//   (c) format_version  — `kDefinitionsCacheFormat`, the ATXDEFS1 wire version.
//   (d) parser_revision — `kDefinitionsParserRevision` (see below).
//   (e) abi_fold        — a sizeof/offsetof fold over `ListedContractDefinition`,
//                         the struct the blob encodes, so a field addition or
//                         reorder can never be misread. This is the same
//                         protection `RunArchiveHeader::schema_hash` gives the
//                         RunArchive.
//
// WHAT THE KEY DELIBERATELY EXCLUDES, AND WHY. It contains NO `run_spec.tsv`, no
// `universe_schedule.tsv` and no swept knob. This is not an oversight — it is
// the reason this cache does not lean on `RunDir::run_identity_hash`. That hash
// folds `run_spec.tsv`, and the swept knobs of a parameter sweep live in exactly
// that file, so a key built on it would miss at EVERY sweep point while the
// definitions bytes never changed. `run_identity_hash` is simultaneously too
// wide (it folds inputs the parsed table does not depend on) and too narrow (it
// deliberately does not fold `definitions.tsv`'s content at all — see
// `RunDir::run_identity_hash` and the pin
// `RunDir.RunIdentityIsDeliberatelyBlindToDefinitionsContent`). Widening it was
// still worth doing for the merge-write guard; that is a different job.
//
// NOTE for a possible follow-up: `definitions_cache_key(...).content_hash` is
// exactly the value `run_identity_hash` would need in order to close its
// documented `definitions.tsv` gap, and on the read path it is computed from
// bytes that are already resident. Folding it there would close that gap at no
// extra I/O. That is a separate change and is NOT done here.
//
// ── kDefinitionsParserRevision ──────────────────────────────────────────────
//
// A hand-bumped integer. ANY semantic change to `parse_listed_definitions` or
// `ListedDefinitionTable::create` MUST increment it, because such a change makes
// previously-written blobs describe a table the current code would no longer
// produce from the same bytes — the one way this cache could serve a stale
// answer without any byte on disk having changed. The content hash cannot see
// this: the source bytes are identical, only their meaning moved.
//
// Two changes already in this branch are exactly that class, and each would have
// required a bump had this cache predated them:
//   * Wave E Task 4 — `ListedDefinitionTable::create` replaced a per-row
//     `end_of_day_ns` recomputation with a compare-then-refresh `trade_end` memo
//     and made `fingerprint()` lazy (commit ba06428 and its parents).
//   * Wave E Task 5 — `parse_listed_definitions` replaced `split(line, '\t')`
//     with a single forward scan over nine field boundaries and dropped the
//     materialised line index (commit 18ee3cb, guard fix 2d5b74f).
// Both were behaviour-preserving as it happens; the revision does not ask
// whether a change WAS observable, it asks whether it COULD be. Bump on any
// touch to either function's semantics.
//
// ── Cross-process stability of the key ──────────────────────────────────────
//
// `atx::core::hash_bytes`'s header comment is conservative about stability
// across process restarts. Measured on this build, two separate processes hash
// the fixture's 730,526,177-byte `definitions.tsv` to the same
// 0xf3a3de5a76bec10e, and the codebase already depends on that property:
// `RunDir::run_identity_hash` persists a fold of `fingerprint_text` values into
// `run.atxrun` and compares them across invocations. Were it ever NOT stable,
// the consequence here is a permanent MISS — never a bad serve — because the
// reader recomputes the hash from the current bytes with the current binary and
// requires field-for-field equality.
//
// ── Format ──────────────────────────────────────────────────────────────────
//
// ATXDEFS1 is its OWN FILE, not a RunArchive section: the RunArchive schema is
// frozen (`ra_schema_hash() == 0xdcce47781ac8390d`, `kRaMinor == 0`) and this
// format has nothing to do with a run's result set.
//
//   [0, 128)                                  ListedDefinitionsCacheHeader
//   [string_table_offset, +string_table_size) string table
//   [column_block_offset, +column_block_size) typed column arrays, 8-B aligned
//
// String table:  u64 n_entries, u64 offsets[n_entries + 1], char blob[].
// `offsets` are blob-relative, non-decreasing, and `offsets[n_entries]` is the
// blob length. `trade_date` and `raw_symbol` share ONE deduplicated table and
// are stored per row as u32 codes into it — on the fixture the ~6.5M rows carry
// only ~49 distinct trade dates and a few hundred thousand distinct symbols.
//
// Column block, in descending-alignment order, each array 8-B aligned:
//   i64 definition_ts_ns[n]   i64 expiry_ts_ns[n]   f64 multiplier[n]
//   u64 source_fingerprint[n] u32 instrument_id[n]  u32 trade_date_code[n]
//   u32 raw_symbol_code[n]    u8  flags[n]   (bit0 monthly, bit1 deliverable)
//
// Struct discipline mirrors `run_archive.hpp`: fields ordered by descending
// alignment so there is no internal padding, trivially copyable + standard
// layout, and sizeof PLUS every load-bearing offsetof pinned by static_assert.
// This is an on-disk ABI — a field reorder that preserved sizeof would still
// silently corrupt readers. Host little-endian LP64 only.

#include "atx/vol/listed_opra.hpp" // ListedDefinitionTable, ListedContractDefinition
#include "atx/vol/types.hpp"       // Result / Status

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace atx::vol {

// Forward-declared so the seam can accept the CLI's phase timer by pointer
// without this header pulling in run_diagnostics.hpp. Defined in
// atx/vol/run_diagnostics.hpp; the .cpp includes it for the definition. Same
// convention as `build_listed_dispersion_schedule`'s optional `PhaseTimer *`.
class PhaseTimer;

// ── The encoded payload struct's ABI ────────────────────────────────────────
//
// `ListedContractDefinition` is what ATXDEFS1 encodes, field for field, so it is
// an ON-DISK ABI and the sprint constraint ("On-disk structs are an ABI:
// `static_assert` on `sizeof` AND `offsetof`") binds it exactly as it binds
// `ListedDefinitionsCacheHeader` below. The header was pinned; this struct was
// not, which is Wave E T7 review finding I1.
//
// WHAT THESE PINS EXIST TO CATCH, concretely. A field is added to
// `ListedContractDefinition`. `parse_listed_definitions` and
// `serialize_listed_definitions` are updated, because the TSV round trip forces
// it. The ATXDEFS1 encoder in `listed_definitions_cache.cpp` is NOT. The new
// writer then emits a blob that silently drops the field and the new reader
// default-initialises it: both CRCs pass (the bytes are the bytes written), the
// content hash passes (the source file did not change), and
// `CacheRoundTripReconstructsTableExactly` passes too, because its fixture uses
// positional aggregate initialisation and the field is default on BOTH sides.
// These asserts turn that whole scenario into a BUILD ERROR at zero runtime
// cost. That is what makes the runtime fingerprint guard below safe to leave
// opt-in.
//
// WHEN ONE OF THESE FIRES the fix is NOT to update the number. It is to teach
// the ATXDEFS1 encoder AND decoder about the new shape, bump
// `kDefinitionsParserRevision` (and `kDefinitionsCacheFormat` if the wire layout
// moved), and only then re-pin.
//
// Offsets are written relative to `sizeof(std::string)` so the pin is exact on
// any LP64 host rather than only on the one whose `std::string` happens to be 40
// bytes. On MSVC (`sizeof(std::string) == 40`) they evaluate to
//     0, 40, 48, 88, 96, 104, 112, 113, 120   with `sizeof` == 128.
namespace detail {
inline constexpr std::size_t kDefsStrSize = sizeof(std::string);
} // namespace detail

static_assert(std::is_standard_layout_v<ListedContractDefinition>,
              "ATXDEFS1 pins ListedContractDefinition's offsets; it must be standard layout");
static_assert(alignof(ListedContractDefinition) == 8);
static_assert(sizeof(ListedContractDefinition) == 2 * detail::kDefsStrSize + 48,
              "ListedContractDefinition layout drift — read the note above before touching this");
static_assert(offsetof(ListedContractDefinition, trade_date) == 0);
static_assert(offsetof(ListedContractDefinition, instrument_id) == detail::kDefsStrSize);
static_assert(offsetof(ListedContractDefinition, raw_symbol) == detail::kDefsStrSize + 8);
static_assert(offsetof(ListedContractDefinition, definition_ts_ns) == 2 * detail::kDefsStrSize + 8);
static_assert(offsetof(ListedContractDefinition, expiry_ts_ns) == 2 * detail::kDefsStrSize + 16);
static_assert(offsetof(ListedContractDefinition, multiplier) == 2 * detail::kDefsStrSize + 24);
static_assert(offsetof(ListedContractDefinition, standard_monthly) ==
              2 * detail::kDefsStrSize + 32);
static_assert(offsetof(ListedContractDefinition, standard_deliverable) ==
              2 * detail::kDefsStrSize + 33);
static_assert(offsetof(ListedContractDefinition, source_fingerprint) ==
              2 * detail::kDefsStrSize + 40);

// `sizeof` + `offsetof` alone are NOT sufficient, and neither is the runtime
// `abi_fold`, which folds exactly the same twenty numbers: a `bool` inserted
// between `standard_deliverable` and `source_fingerprint` lands in the six bytes
// of existing tail padding, so every assert above still holds, `sizeof` is still
// 128 and the fold does not move. That is a real hole in both, and this is what
// closes it — a structured binding NAMES every member, so a field added,
// removed, reordered or retyped stops this translation unit compiling.
inline void definitions_cache_payload_shape_pin(const ListedContractDefinition &row) noexcept {
  const auto &[trade_date, instrument_id, raw_symbol, definition_ts_ns, expiry_ts_ns, multiplier,
               standard_monthly, standard_deliverable, source_fingerprint] = row;
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(trade_date)>, std::string>);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(instrument_id)>, std::uint32_t>);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(raw_symbol)>, std::string>);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(definition_ts_ns)>, std::int64_t>);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(expiry_ts_ns)>, std::int64_t>);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(multiplier)>, double>);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(standard_monthly)>, bool>);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(standard_deliverable)>, bool>);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(source_fingerprint)>, std::uint64_t>);
  static_cast<void>(std::tie(trade_date, instrument_id, raw_symbol, definition_ts_ns, expiry_ts_ns,
                             multiplier, standard_monthly, standard_deliverable,
                             source_fingerprint));
}

// File magic, no NUL.
inline constexpr char kDefinitionsCacheMagic[8] = {'A', 'T', 'X', 'D', 'E', 'F', 'S', '1'};

inline constexpr std::uint16_t kDefinitionsCacheMajor = 1;

// RESERVED, and deliberately inert (review M3). It is stamped into the header
// and pinned by `WrittenHeaderCarriesTheDeclaredIdentity`, but the reader does
// NOT check it and it is NOT folded into the key — a minor bump is by
// definition backward-compatible, so rejecting on it would turn a compatible
// blob into a miss. `major` is the field that gates compatibility. If a future
// minor ever carries meaning, it must be added to the key at the same time.
inline constexpr std::uint16_t kDefinitionsCacheMinor = 0;

// Wire version, folded into the key. Bump on ANY layout change.
inline constexpr std::uint32_t kDefinitionsCacheFormat = 1;

// Semantic revision of `parse_listed_definitions` + `ListedDefinitionTable::
// create`. See the file-level note: bump on ANY semantic touch to either.
inline constexpr std::uint32_t kDefinitionsParserRevision = 1;

// Column-array alignment inside the payload.
inline constexpr std::uint64_t kDefinitionsCacheAlign = 8;

// ── The key ─────────────────────────────────────────────────────────────────
struct ListedDefinitionsCacheKey {
  std::uint64_t content_hash{};
  std::uint64_t source_size{};
  std::uint32_t format_version{};
  std::uint32_t parser_revision{};
  std::uint64_t abi_fold{};

  [[nodiscard]] bool operator==(const ListedDefinitionsCacheKey &) const = default;
};

// sizeof/offsetof fold over `ListedContractDefinition`. Deterministic for a
// given build; a field addition, removal, reorder or type change moves it, so a
// blob written by a differently-shaped build can never be decoded as the current
// shape. Not constexpr only because `hash_bytes` is not.
[[nodiscard]] std::uint64_t definitions_cache_abi_fold() noexcept;

// The key for a source `definitions.tsv` whose full byte content is
// `source_bytes`. The caller is expected to already hold those bytes (the parse
// path reads the whole file before parsing), so this costs one memory-bandwidth
// pass, not a second I/O. Measured on the fixture: ~58 ms over 730 MB (~12 GB/s)
// against an 8-15 s parse.
[[nodiscard]] ListedDefinitionsCacheKey definitions_cache_key(std::string_view source_bytes);

// ── On-disk header (offset 0, 128 B, all fields naturally aligned) ──────────
struct ListedDefinitionsCacheHeader {
  char magic[8]{};                      //   0  "ATXDEFS1", no NUL
  std::uint64_t file_size{};            //   8  total bytes, must equal the real size
  std::uint64_t content_hash{};         //  16  key (a)
  std::uint64_t source_size{};          //  24  key (b)
  std::uint64_t abi_fold{};             //  32  key (e)
  std::uint64_t table_fingerprint{};    //  40  ListedDefinitionTable::fingerprint()
  std::uint64_t n_rows{};               //  48
  std::uint64_t string_table_offset{};  //  56
  std::uint64_t string_table_size{};    //  64
  std::uint64_t column_block_offset{};  //  72
  std::uint64_t column_block_size{};    //  80
  std::uint32_t format_version{};       //  88  key (c)
  std::uint32_t parser_revision{};      //  92  key (d)
  std::uint32_t header_crc32c{};        //  96  over the header with THIS field = 0
  std::uint32_t payload_crc32c{};       // 100  over [header_size, file_size)
  std::uint16_t major{};                // 104  kDefinitionsCacheMajor
  std::uint16_t minor{};                // 106  kDefinitionsCacheMinor
  std::uint16_t header_size{};          // 108  sizeof(ListedDefinitionsCacheHeader)
  std::uint16_t endian{};               // 110  1 = little
  std::uint16_t pointer_bits{};         // 112  64
  std::uint16_t reserved_u16{};         // 114
  std::uint8_t reserved[12]{};          // 116  pad to 128; covered by header_crc32c
};
static_assert(sizeof(ListedDefinitionsCacheHeader) == 128,
              "ListedDefinitionsCacheHeader layout drift");
static_assert(alignof(ListedDefinitionsCacheHeader) == 8);
static_assert(std::is_trivially_copyable_v<ListedDefinitionsCacheHeader>);
static_assert(std::is_standard_layout_v<ListedDefinitionsCacheHeader>);

// Per-field offsets pinned explicitly, not just sizeof: a reorder that preserved
// sizeof would still silently corrupt readers.
static_assert(offsetof(ListedDefinitionsCacheHeader, magic) == 0);
static_assert(offsetof(ListedDefinitionsCacheHeader, file_size) == 8);
static_assert(offsetof(ListedDefinitionsCacheHeader, content_hash) == 16);
static_assert(offsetof(ListedDefinitionsCacheHeader, source_size) == 24);
static_assert(offsetof(ListedDefinitionsCacheHeader, abi_fold) == 32);
static_assert(offsetof(ListedDefinitionsCacheHeader, table_fingerprint) == 40);
static_assert(offsetof(ListedDefinitionsCacheHeader, n_rows) == 48);
static_assert(offsetof(ListedDefinitionsCacheHeader, string_table_offset) == 56);
static_assert(offsetof(ListedDefinitionsCacheHeader, string_table_size) == 64);
static_assert(offsetof(ListedDefinitionsCacheHeader, column_block_offset) == 72);
static_assert(offsetof(ListedDefinitionsCacheHeader, column_block_size) == 80);
static_assert(offsetof(ListedDefinitionsCacheHeader, format_version) == 88);
static_assert(offsetof(ListedDefinitionsCacheHeader, parser_revision) == 92);
static_assert(offsetof(ListedDefinitionsCacheHeader, header_crc32c) == 96);
static_assert(offsetof(ListedDefinitionsCacheHeader, payload_crc32c) == 100);
static_assert(offsetof(ListedDefinitionsCacheHeader, major) == 104);
static_assert(offsetof(ListedDefinitionsCacheHeader, minor) == 106);
static_assert(offsetof(ListedDefinitionsCacheHeader, header_size) == 108);
static_assert(offsetof(ListedDefinitionsCacheHeader, endian) == 110);
static_assert(offsetof(ListedDefinitionsCacheHeader, pointer_bits) == 112);
static_assert(offsetof(ListedDefinitionsCacheHeader, reserved_u16) == 114);
static_assert(offsetof(ListedDefinitionsCacheHeader, reserved) == 116);

// ── The reconstructed-table fingerprint check ───────────────────────────────
//
// GUARD 4 of the reader recomputes `ListedDefinitionTable::fingerprint()` on the
// decoded table and requires it to equal the value the writer stamped. It is by
// far the most expensive step on the read path — `fingerprint()` is lazy, so on
// a freshly decoded table it pays for a full `serialize_listed_definitions`:
// 1.4-3.8 s AND a ~730 MB transient allocation on the production fixture, on top
// of the 300 MB image and the ~1 GB row vector.
//
// It is DEFAULT OFF in Release and FORCED ON in the tests that isolate it.
// The three arguments that decide this:
//
//  1. The one failure mode it genuinely catches — an encoder that silently drops
//     a field added to `ListedContractDefinition` — is now a BUILD ERROR (see
//     the shape pins at the top of this header). Runtime detection of a defect
//     the compiler refuses to build is the wrong instrument, and it was the only
//     mode CRC + abi_fold + content-hash + the round-trip test did not already
//     cover.
//  2. The anti-tamper argument does not survive contact. `fingerprint()` is a
//     PUBLIC, UNKEYED, deterministic function, so anyone able to rewrite the
//     payload and restamp the two CRCs can restamp the fingerprint too. The
//     threat model it encodes — an adversary who repairs exactly two of three
//     recomputable hashes — is arbitrary.
//  3. The reader is not left bare. `ListedDefinitionTable::create` runs
//     UNCONDITIONALLY on every decoded row set and re-validates non-empty
//     trade_date, nonzero instrument_id, non-empty raw_symbol,
//     definition_ts_ns > 0, expiry_ts_ns > definition_ts_ns, finite positive
//     multiplier, nonzero source_fingerprint, definition_ts_ns <= trade_end and
//     the absence of duplicate keys — a substantial semantic re-validation,
//     independent of the CRCs, on every read.
//
// The check is NOT deleted, because it remains the strongest available proof
// that a decoded blob still MEANS the table that was written, and
// `CacheRejectsTamperedPayloadEvenWithRepairedCrcs` pins it with the flag On.
// Define `ATX_DEFS_CACHE_VERIFY_FINGERPRINT=1` to make it the build-wide
// default, or pass `On` explicitly.
enum class DefinitionsCacheFingerprintCheck : std::uint8_t { Off = 0, On = 1 };

inline constexpr DefinitionsCacheFingerprintCheck kDefinitionsCacheFingerprintDefault =
#if defined(ATX_DEFS_CACHE_VERIFY_FINGERPRINT) && (ATX_DEFS_CACHE_VERIFY_FINGERPRINT != 0)
    DefinitionsCacheFingerprintCheck::On;
#else
    DefinitionsCacheFingerprintCheck::Off;
#endif

// ── Writer / reader ─────────────────────────────────────────────────────────

// Build the whole ATXDEFS1 image in memory, write it to `<cache_path>.tmp`,
// fsync THAT to stable storage, then rename over `cache_path` with bounded
// retry. Mirrors `write_run_archive_file` (src/run_archive.cpp), which itself
// mirrors commit 86f2210: without the fsync a crash after the rename could leave
// a correctly-named cache file holding garbage while the rename had already
// destroyed the previous good one.
//
// COST NOTE: this stamps `table.fingerprint()`, which is LAZY — on a table whose
// fingerprint has not been asked for yet this call is what pays for the
// canonical serialization. That cost belongs to the write path, not to the
// format.
[[nodiscard]] Status write_definitions_cache(std::string_view cache_path,
                                             const ListedDefinitionTable &table,
                                             const ListedDefinitionsCacheKey &key);

// Load a table from `cache_path`, or `Err` — FAIL-CLOSED, never a partial serve.
// A hit requires ALL of:
//   * the file is at least `sizeof(ListedDefinitionsCacheHeader)` bytes;
//   * magic / major / endian / pointer_bits / header_size agree;
//   * `header_crc32c` validates over the header with that field zeroed;
//   * every one of the five key fields equals `expected` field-for-field;
//   * `file_size` equals the file's real length and every offset/size pair lies
//     inside it, with the string offsets non-decreasing and terminated;
//   * `payload_crc32c` validates over [header_size, file_size);
//   * the decoded rows survive `ListedDefinitionTable::create`;
//   * IF `check == On`, the reconstructed table's own `fingerprint()` equals
//     `table_fingerprint` (see the note above for why that is not the default).
// Any failure is an `Err` the caller treats as a miss.
[[nodiscard]] Result<ListedDefinitionTable>
read_definitions_cache(std::string_view cache_path, const ListedDefinitionsCacheKey &expected,
                       DefinitionsCacheFingerprintCheck check = kDefinitionsCacheFingerprintDefault);

// Cache file name for a key, inside `cache_dir`. Content-addressed, so two
// different `definitions.tsv` bodies never contend for one path.
[[nodiscard]] std::string definitions_cache_filename(const ListedDefinitionsCacheKey &key);

// The seam: read `tsv_path`'s bytes ONCE, derive the key from them, try the
// cache in `cache_dir`, and on any miss parse those same bytes and (best-effort)
// publish the result.
//
// A PUBLISH FAILURE IS NEVER AN ERROR — a read-only or full cache directory is a
// logged miss, not a failed run. The returned table is byte-for-byte the table
// `read_listed_definitions_file(tsv_path)` would have returned, on both the hit
// and the miss path.
//
// An EMPTY `cache_dir` disables the cache entirely and this degenerates to
// `read_listed_definitions_file`. When disabled, the cache is never consulted,
// so `timer` receives no `definitions_cache` charge at all — "disabled" is not
// a miss.
//
// Every call where `cache_dir` is non-empty logs exactly one HIT or MISS line
// to stderr, with the rejecting guard's message on a miss. Without it a key
// that stopped matching — the documented consequence of `hash_bytes` not being
// stable across processes — would be a permanent 100% miss that still writes a
// ~300 MB blob every run, and nothing but wall time would say so (review I6).
//
// `timer` (optional, review I6 — "the other half"): when non-null and the
// cache is consulted, charges a `definitions_cache` phase covering exactly the
// `read_definitions_cache` lookup call — the guard chain on a hit, or the
// rejecting guard on a miss. `count` is 1 for a HIT, 0 for a MISS: the shape
// Task 8's brief specifies, so a reader of the `diagnostics` RunArchive section
// can tell a cached run from a fast-but-uncached one without grepping stderr.
// The slurp (shared with the direct path) and a miss's subsequent parse/publish
// are deliberately NOT included in this phase's wall time — they are costs a
// no-cache run pays too, and blurring them in here would make "the cache's own
// cost" unreadable. A null timer (the default) is economically identical to
// today.
[[nodiscard]] Result<ListedDefinitionTable> read_listed_definitions_cached(
    std::string_view tsv_path, std::string_view cache_dir,
    DefinitionsCacheFingerprintCheck check = kDefinitionsCacheFingerprintDefault,
    PhaseTimer *timer = nullptr);

} // namespace atx::vol
