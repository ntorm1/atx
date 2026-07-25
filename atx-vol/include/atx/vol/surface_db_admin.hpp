#pragma once

// surface_db_admin — INSPECTION / VERIFICATION over an already-open `SurfaceDb`
// (surface_db.hpp), plus the ONE management action an operator performs on a
// built database. This is the library half of the management surface;
// `tools/surface_db_main.cpp` (target `atx-vol-surface-db`) is a thin shell over
// exactly these calls.
//
// Each entry point takes a `SurfaceDb` reference plus plain arguments and returns
// a plain owning struct through the repo's `Result<T>` vocabulary — NO iostream,
// NO printf, NO formatting decisions. Output shape is the CLI's business, so a
// Python/notebook/service caller can consume the same structs without re-parsing
// text. (That is the point: verifying a built database must not require the
// pybind11 wrapper.)
//
// ── QUERY / MUTATION SPLIT, and why the split moved (FIX-G) ──────────────────
//
// The five `describe_*` / `query_*` / `verify_*` calls are queries and take a
// `const SurfaceDb &`. `set_symbol_enabled` — the sixth, added last — takes a
// NON-const `SurfaceDb &` and writes the manifest, and the const-ness is the only
// marker a caller needs: a `const SurfaceDb &` cannot reach it.
//
// This header used to open "everything here is a query: nothing in this header
// creates, mutates, fits or persists anything". That sentence described the SCOPE
// this file had reached, not a safety property anyone had chosen, and the
// distinction matters because the codebase does record chosen safety properties
// when it makes them — `surface-db-build.md`'s "the library exposes an
// `overwrite_existing` escape hatch; the CLI does not — it is deliberately
// non-destructive" is what one looks like. Nothing equivalent was ever written
// for this surface. Meanwhile `AutoConfigSpec::retry_disabled` defaults to false
// specifically to protect "an operator `upsert_symbol` with `enabled = false`
// fences a symbol out of production" (surface_db_build.hpp), and
// `surface-db-build.md` names that disable as the standing remedy for a
// permanently-failing name — a remedy no shipped tool could perform. The gap was
// recorded as a gap ("the admin CLI has no enable/disable verb today"), never as
// a guarantee.
//
// So the write path is deliberately ONE FIELD WIDE. `set_symbol_enabled` flips
// `SymbolFitConfig::enabled` and touches nothing else; there is no `upsert` verb,
// no config editor, and no partition mutation here. That keeps the build CLI's
// "a stored config is never clobbered" posture intact — an operator's tuned
// config survives this call byte-for-byte — while making the one documented
// remedy reachable.
//
// ── The five questions ───────────────────────────────────────────────────────
//
//   describe_db        — "what is in this database?"      generation, symbol /
//                        partition counts, per-partition surface counts + bytes.
//   describe_partition — "what does THIS date hold?"      the symbols the
//                        partition's .atxvsa directory actually carries.
//   describe_symbol    — "how is THIS name configured?"   enabled/preset/curve
//                        + the stored provenance when present.
//   query_surface      — "what does this cell evaluate to?" iv / total variance
//                        / uid / n_slices at one (K, T), through `map_surface`.
//   verify_db          — "is the whole thing healthy?"    walk every cell; map
//                        it, verify its stored payload CHECKSUM, and evaluate one
//                        ATM-ish point to prove it produces a usable number.
//
// ── The one action ───────────────────────────────────────────────────────────
//
//   set_symbol_enabled — "stop / resume fitting THIS name."  flip the stored
//                        `SymbolFitConfig::enabled` bit and nothing else.
//                        Disabling does NOT delete what the symbol already
//                        fitted — see the call for the full argument.
//
// ── What a green `verify_db` does and does NOT prove ─────────────────────────
//
// PROVES, for every cell the spec selected: the partition file opened, the record
// framing and bounds parse, the record's bytes still match the payload CRC the
// writer computed (so no bit rot / partial copy / hand edit), and the surface
// evaluates to a finite, positive implied vol at ONE ATM-ish point.
//
// Does NOT prove: that the numbers are RIGHT (no oracle is consulted — a surface
// fitted from bad market data checksums perfectly and probes fine); that any point
// other than the probe is usable; that the database has the coverage you expect
// (that is `cells_checked` against a caller-supplied floor — `verify_db` reports
// the count, it cannot know the intended one); or anything at all about cells the
// spec excluded. It IS a byte-integrity + liveness check over a selected grid, and
// it is deliberately not more than that.
//
// ── Why `map_surface` and not `load_surface` ─────────────────────────────────
//
// `query_surface` / `verify_db` deliberately go through the ZERO-COPY
// `SurfaceDb::map_surface` path: that is the call production readers make (and
// the one the retired Python check made), so a green verify is evidence about
// the path that actually serves, not about a parallel owned-reconstruct route
// that could diverge.
//
// ── Bytes on disk ────────────────────────────────────────────────────────────
//
// Two byte counts are reported per partition and they are NOT redundant.
// `manifest_bytes` is `DbPartitionRecord::file_size` — the size the manifest
// recorded when the partition was written. `bytes_on_disk` is the file's size
// right now (0 when the file is absent). A mismatch, or `present == false`,
// means the directory was edited out from under the manifest — precisely the
// corruption an operator runs an inspector to find.
//
// ── Thread safety ────────────────────────────────────────────────────────────
//
// Every QUERY is a `const` call over `SurfaceDb`'s own thread-safe const API
// (immutable manifest snapshot + the internally-locked partition view cache), so
// any number of threads may call these concurrently on one `const SurfaceDb &`.
// They are NOT synchronized against a concurrent writer mutating the same db; a
// mid-walk `write_partition` is observed as a torn view (some cells from before,
// some after), never as a data race.
//
// `set_symbol_enabled` is a MUTATION. In-process it is as safe as any other
// manifest mutation — `SurfaceDb::upsert_symbol` serializes on the db's own mutex
// and swaps an atomically-rewritten snapshot — so a concurrent reader sees the
// manifest before or after, never torn. CROSS-PROCESS is the rule that matters
// and it is `surface_db.hpp`'s, not a new one: SINGLE WRITER, many readers. See
// the call's CONCURRENCY paragraph for the specific way a build violates it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/session.hpp"        // FitPreset
#include "atx/vol/surface_archive.hpp" // SurfaceProvenance
#include "atx/vol/surface_db.hpp"     // SurfaceDb, SymbolFitConfig
#include "atx/vol/surface_policy.hpp" // SurfacePolicy
#include "atx/vol/types.hpp"          // Result, Status
#include "atx/vol/vol_curve.hpp"      // VolCurveKind

namespace atx::vol {

// ── describe_db ─────────────────────────────────────────────────────────────

// One partition as the manifest indexes it, cross-checked against the file that
// is actually on disk. See the "Bytes on disk" note above for why both byte
// counts exist.
struct DbPartitionSummary {
  std::string key{};                 // canonical partition key (e.g. a trade date)
  std::uint32_t surface_count{0};    // surfaces the manifest recorded for it
  std::uint64_t manifest_bytes{0};   // file bytes AT WRITE TIME (manifest record)
  std::uint64_t bytes_on_disk{0};    // file bytes NOW; 0 when `present` is false
  bool present{false};               // the .atxvsa file exists under <root>/partitions
  std::int64_t created_ts_ns{0};
};

// Whole-database shape. `n_symbols_enabled` counts manifest symbols whose stored
// `SymbolFitConfig::enabled` is true — a disabled symbol is configured but
// deliberately never populated or served (fail-closed), so the two counts
// differing is normal, not a defect.
struct DbDescription {
  std::string root{};
  std::uint64_t generation{0};
  std::size_t n_symbols{0};
  std::size_t n_symbols_enabled{0};
  std::size_t n_partitions{0};
  std::size_t n_partitions_missing{0}; // manifest entries with no file on disk
  std::uint64_t total_surface_count{0};
  std::uint64_t total_manifest_bytes{0};
  std::uint64_t total_bytes_on_disk{0};
  std::vector<DbPartitionSummary> partitions{}; // manifest order (sorted by key)
};

// Summarize `db` from its manifest snapshot plus a `stat` of each partition file.
// Reads no surface record and opens no archive, so it stays cheap on a
// thousand-partition database. Err only if a manifest symbol's stored config
// fails to decode (a corrupt manifest that `open` admitted).
[[nodiscard]] Result<DbDescription> describe_db(const SurfaceDb &db);

// ── describe_partition ──────────────────────────────────────────────────────

// One surface inside a partition's archive directory. `surface_bytes` is that
// record's own byte extent within the .atxvsa file.
struct PartitionSymbolInfo {
  std::string symbol{};
  std::uint32_t uid{0};
  std::uint32_t n_slices{0};
  std::uint64_t surface_bytes{0};
};

// What a partition ACTUALLY holds — read from the .atxvsa directory, not from
// the manifest. `manifest_surface_count` vs `archive_surface_count` disagreeing
// means the manifest and the file have drifted apart.
struct PartitionDescription {
  std::string key{};
  std::uint32_t manifest_surface_count{0};
  std::uint32_t archive_surface_count{0};
  std::uint64_t manifest_bytes{0};
  std::uint64_t bytes_on_disk{0};
  std::vector<PartitionSymbolInfo> symbols{}; // archive directory order (canonical)
};

// Open partition `key` and enumerate the symbols it carries. Errors: NotFound
// (no such partition in the manifest — AND the case where the manifest lists it
// but the file is gone, which `SurfaceArchiveV2::open_file` also reports as
// NotFound), ParseError (the file is there but does not parse as a v2 archive),
// IoError (present and indexed, but unreadable), InvalidArgument (malformed key).
[[nodiscard]] Result<PartitionDescription> describe_partition(const SurfaceDb &db,
                                                              std::string_view key);

// ── describe_symbol ─────────────────────────────────────────────────────────

// A symbol's stored fit configuration, flattened to the fields an operator
// checks. `curve_kind` is only meaningful when `pin_curve` is true (otherwise
// the preset/selector picks the family at fit time).
struct SymbolDescription {
  std::string symbol{}; // canonical, as stored in the manifest
  bool enabled{false};
  FitPreset preset{FitPreset::Populate};
  bool pin_curve{false};
  VolCurveKind curve_kind{};
  double band_k{0.0};
  SurfacePolicy surface_policy{};
  bool has_provenance{false};
  SurfaceProvenance provenance{}; // meaningful only when `has_provenance`
};

// Look up `symbol`'s stored config (+ provenance when the manifest carries one).
// Case-insensitive, like every other SurfaceDb symbol lookup. Errors: NotFound
// when the symbol is not in the manifest's symbol table.
[[nodiscard]] Result<SymbolDescription> describe_symbol(const SurfaceDb &db,
                                                        std::string_view symbol);

// ── query_surface ───────────────────────────────────────────────────────────

// One evaluated point on one stored surface. `forward` is the surface's own
// interpolated forward at `T` — the anchor an ATM query should use for `K`.
struct SurfacePointQuote {
  std::string key{};
  std::string symbol{};
  double K{0.0};
  double T{0.0};
  double iv{0.0};
  double total_variance{0.0};
  double forward{0.0};
  std::uint32_t uid{0};
  std::size_t n_slices{0};
};

// Map (`key`, `symbol`) through the zero-copy `SurfaceDb::map_surface` and
// evaluate it at (`K`, `T`). Errors: InvalidArgument (non-finite or
// non-positive K/T, malformed key), NotFound (no such partition, the partition
// file is missing, or the partition does not carry that symbol), ParseError (the
// partition file is present but corrupt). A mapped surface that evaluates to an
// UNUSABLE point — non-finite total variance, or an iv that is non-finite or
// non-positive — is reported as Internal. That is the SAME usability rule
// `verify_db`'s per-cell probe applies (both go through the one `usable_iv`
// predicate), so a spot check can never print a number for a cell the walk calls
// broken.
[[nodiscard]] Result<SurfacePointQuote> query_surface(const SurfaceDb &db, std::string_view key,
                                                      std::string_view symbol, double K, double T);

// ── verify_db ───────────────────────────────────────────────────────────────

// Default ATM probe tenor: ~30 calendar days. Every stored surface is defined at
// every positive T (short-end scaling / flat long-end extrapolation), so a fixed
// probe needs no knowledge of the surface's own expiry ladder and stays
// comparable across cells.
inline constexpr double kSurfaceDbVerifyProbeT = 30.0 / 365.0;

// Default cap on the failure list `verify_db` returns. A verify over a broken
// universe must not return a million-entry vector; the cap plus
// `DbVerifyReport::n_failures_elided` keeps the answer bounded WITHOUT letting
// truncation read as "everything passed".
inline constexpr std::size_t kSurfaceDbVerifyMaxFailures = 32;

// Why one cell failed. The three kinds are three DIFFERENT questions answered in
// order, and they stay distinct so a fault names its own root cause.
enum class DbCellFailure : std::uint8_t {
  // `map_surface` did not return a surface: the partition file is missing or
  // corrupt, or the partition simply does not carry that symbol.
  Unmappable = 0,
  // The surface mapped and its bytes are intact, but the ATM probe did not
  // produce a usable number (non-finite, or a non-positive implied vol).
  NonFinite = 1,
  // The record's bytes no longer match the payload CRC the writer stored in it
  // (`SurfaceArchiveV2::validate_symbol`). Mapping only checks magic, framing and
  // bounds, so intra-record damage — bit rot, a partial copy, a hand edit — maps
  // CLEANLY and can still probe to a plausible number when the corruption misses
  // the slices the probe touches. This is the only check that reads the payload.
  ChecksumMismatch = 2,
};

// One failing (partition, symbol) cell, identified.
struct DbCellFault {
  std::string key{};
  std::string symbol{};
  DbCellFailure kind{DbCellFailure::Unmappable};
  std::string detail{}; // the mapping error's text, or the offending probe value
};

// What to verify. All fields are restrictions on an otherwise exhaustive walk.
struct DbVerifySpec {
  // Inclusive partition-key range, compared lexicographically against the
  // CANONICAL (upper-cased) key. ISO dates sort correctly under that compare, so
  // `key_lo = "2026-07-01"`, `key_hi = "2026-07-31"` is a July restriction.
  // Empty means unbounded on that side.
  std::string key_lo{};
  std::string key_hi{};
  // Symbol subset (canonicalized on entry). Empty = every symbol in the
  // manifest's symbol table, filtered by `include_disabled`.
  std::vector<std::string> symbols{};
  // false (default) => a symbol whose stored config is DISABLED is skipped. A
  // disabled symbol is never populated into a partition (fail-closed), so
  // checking it would report a "missing" cell on every partition of every
  // healthy database. true forces it into the walk.
  bool include_disabled{false};
  // Tenor for the per-cell ATM evaluation. Must be finite and > 0.
  double probe_T{kSurfaceDbVerifyProbeT};
  // Cap on `DbVerifyReport::failures`. Extra faults are counted in
  // `n_failures_elided`, never dropped silently. 0 retains no detail at all
  // (every fault is elided) — the counters still tell the truth.
  std::size_t max_reported_failures{kSurfaceDbVerifyMaxFailures};
};

// The health verdict. `cells_checked == cells_ok + cells_unmappable +
// cells_non_finite + cells_checksum` always holds.
struct DbVerifyReport {
  std::size_t n_partitions{0};       // partitions IN RANGE (the walk's rows)
  std::size_t n_partitions_in_db{0}; // partitions the manifest holds, range-independent
  std::size_t n_symbols{0};          // symbols in the walk (the walk's columns)
  std::size_t cells_checked{0};
  std::size_t cells_ok{0};
  std::size_t cells_unmappable{0};
  std::size_t cells_non_finite{0};
  std::size_t cells_checksum{0};       // mapped, but the payload CRC did not match
  std::vector<DbCellFault> failures{}; // capped at spec.max_reported_failures
  std::size_t n_failures_elided{0};    // faults NOT in `failures` (never silent)
  // The manifest symbols this walk DROPPED because their stored config is
  // disabled (canonical, sorted). Populated only on the default exhaustive walk —
  // an explicit `spec.symbols` is honoured verbatim (a named disabled symbol IS
  // walked, and reports its missing cells), and `include_disabled` drops nothing.
  //
  // Naming them is the point: the default walk silently narrows its columns, so a
  // database permanently missing a requested name reads exactly like a healthy
  // one. It does NOT change `ok()` — a fail-closed disable is a legitimate
  // production state, not a corrupt database, and flipping the verdict would fail
  // every operator script over every partially-disabled db. The verdict answers
  // "are the bytes I stored still good?"; this answers "which names did I not
  // even look at?", and an operator needs both.
  std::vector<std::string> disabled_symbols{};

  // The walk covered NOTHING over a database that has something. Not one byte of
  // any partition was read, so every counter above is zero and every failure list
  // is empty — the exact shape of a perfect result. It is reachable three ways and
  // all three are operator errors, not health:
  //   - every symbol is fail-closed DISABLED (the default walk drops the columns),
  //   - `spec.symbols` names only symbols the manifest never configured,
  //   - `spec.key_lo`/`key_hi` select no partition (wrong window, wrong db).
  //
  // A genuinely FRESH root (`n_partitions_in_db == 0`) is deliberately excluded:
  // there is nothing to be wrong about yet, a newly created db must not be an
  // error, and the caller-supplied `--min-cells` floor is the right instrument for
  // "this database should not be empty by now". The distinction this makes is
  // between "there is no data" and "there is data and you looked at none of it".
  [[nodiscard]] constexpr bool selected_no_cells() const noexcept {
    return n_partitions_in_db > 0 && cells_checked == 0;
  }

  [[nodiscard]] constexpr bool ok() const noexcept {
    return cells_unmappable == 0 && cells_non_finite == 0 && cells_checksum == 0 &&
           !selected_no_cells();
  }
};

// Walk the (partition x symbol) grid selected by `spec`. Each cell passes THREE
// gates, in this order, and stops at the first that fails:
//
//   1. map        — `SurfaceDb::map_surface`; proves magic, framing and bounds.
//   2. checksum   — `SurfaceArchiveV2::validate_symbol` over the archive the map
//                   already holds open; proves the payload BYTES are the ones the
//                   writer checksummed. Ordered before the probe because a byte
//                   fault is the root cause of any number it produces, and after
//                   the map because "this partition does not carry that symbol"
//                   must stay `Unmappable` rather than becoming a checksum fault.
//   3. probe      — evaluate one ATM-ish point (K = the surface's own
//                   `forward_at(probe_T)`, T = `probe_T`) to prove the surface
//                   yields a finite, positive implied vol rather than merely
//                   holding valid bytes.
//
// COST: gate 2 reads and checksums every selected record's payload, which is
// strictly more IO than the map-and-probe walk (which touches only the header,
// the column offsets, and the slices bracketing `probe_T`). It is the default
// anyway — a health check that skips the checksum the format already carries is
// not a health check, and it is the only gate that sees intra-record damage.
//
// A cell failure is TALLIED, never fatal — the whole point is to enumerate every
// broken cell in one pass. Err is reserved for a spec that cannot be honored:
// InvalidArgument (non-finite / non-positive `probe_T`, an empty or oversized
// entry in `spec.symbols`) or a manifest whose stored config fails to decode.
//
// The report is all-ok (`DbVerifyReport::ok()`) exactly when every selected cell
// passed all three gates AND the walk actually selected cells (see
// `selected_no_cells`). A caller wiring this into a script should branch on
// `ok()`, not on the Result.
[[nodiscard]] Result<DbVerifyReport> verify_db(const SurfaceDb &db, const DbVerifySpec &spec = {});

// ── set_symbol_enabled — the one management action ──────────────────────────

// What `set_symbol_enabled` did. `changed == false` means the symbol was ALREADY
// in the requested state and no manifest write happened at all (so `generation`
// did not move) — the call is idempotent, and a converging operator script can
// run it unconditionally.
struct SymbolEnableChange {
  std::string symbol{}; // canonical spelling, as stored in the manifest
  bool was_enabled{false};
  bool now_enabled{false};
  bool changed{false};         // false => already in this state; nothing was written
  std::uint64_t generation{0}; // manifest generation AFTER the call
};

// Set `symbol`'s stored `SymbolFitConfig::enabled` to `enabled`, leaving every
// other field of that config — and the symbol's stored provenance — untouched.
//
// This is the operator-facing form of the remedy `surface-db-build.md` has always
// named ("disable the name"), and it is intentionally the ONLY write in this
// header. It reads the stored config, flips one bool, and hands it back to
// `SurfaceDb::upsert_symbol` — the same serialized, atomically-rewritten,
// generation-bumping writer the build path uses (`generate_symbol_configs`,
// `populate_universe_streaming`). There is no second writer and no second
// encoding of a `SymbolFitConfig`.
//
// WHAT `enabled = false` MEANS, EXACTLY. STOP FITTING, not delete:
//   - the populate stops scheduling the symbol on every date, forever, until it
//     is re-enabled — this is a per-SYMBOL switch, never a per-cell one;
//   - every surface it ALREADY fitted stays in its partition, keeps loading and
//     keeps serving (nothing on the read path gates on `enabled`), and is
//     re-emitted verbatim through any later rewrite of those dates
//     (`UniversePopulateCoverage::cells_carried_disabled`). That preservation is
//     FIX-E's; before it, disabling a name silently destroyed everything it had
//     produced on the next unrelated rewrite of each of its dates. This call is
//     the reason that invariant now has an operator who can reach it, so it is
//     pinned end to end by `SurfaceDbAdmin.DisableThenRebuildPreservesStored*`
//     and the enable round trip beside it.
//
// ENABLE IS NOT `--retry-disabled`. This flips the STORED config's bit and runs
// no selection. When the disable came from the operator, that is exactly right —
// the config is theirs and it comes back untouched. When it came from a FAILED
// CONFIG SELECTION (`AutoConfigReport::n_disabled_failed`), the stored config is
// the fallback `symbol_config_from_preset(preset)` that selection fell back to,
// and re-enabling it fits that generic config rather than a chosen one.
// `atx-vol-surface-db-build --retry-disabled` is the instrument for that case: it
// re-SELECTS the symbol as if it were new.
//
// CONCURRENCY. `surface_db.hpp` documents the database as SINGLE WRITER, many
// readers, and this call is a writer. Do not run it while a build is running
// against the same root, in either order: a build holds an in-memory manifest
// snapshot for the whole run and every `write_partition` persists THAT snapshot's
// symbol table, so a disable landing mid-build is silently overwritten by the
// next partition write, and `DbConfigAttestation`'s fold would be taken over
// configs the surfaces were not fitted under. Nothing detects this — there is no
// lock file — so it is a scheduling rule, and it is the same one every other
// manifest mutation on this database has always been under.
//
// Errors: NotFound when `symbol` is not in the manifest's symbol table (this call
// never CREATES a config — an operator fencing out a name they cannot see named
// the wrong name, and inventing a default config for it would be the silent
// wrong answer); InvalidArgument on an empty canonical symbol; IoError/ParseError
// propagated from the manifest rewrite.
[[nodiscard]] Result<SymbolEnableChange> set_symbol_enabled(SurfaceDb &db, std::string_view symbol,
                                                            bool enabled);

} // namespace atx::vol
