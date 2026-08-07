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
// evaluates to a finite, positive implied vol at ONE ATM-ish point. And, for every
// partition in range, that the manifest's record still DESCRIBES that file (its
// surface count and byte size) — the one thing no per-cell gate can see, because a
// stale index sits beside a perfectly good file.
//
// Does NOT prove: that the numbers are RIGHT (no oracle is consulted — a surface
// fitted from bad market data checksums perfectly and probes fine); that any point
// other than the probe is usable; that the database has the coverage you expect
// (that is `cells_checked` against a caller-supplied floor — `verify_db` reports
// the count, it cannot know the intended one); or anything at all about cells the
// spec excluded. It IS a byte-integrity + liveness check over a selected grid, and
// it is deliberately not more than that.
//
// ── ABSENCE IS NOT A FAULT (FIX-H) ───────────────────────────────────────────
//
// A selected cell can be missing for two reasons that could not be more
// different, and `verify_db` used to report both as `Unmappable`. The cost was
// not cosmetic: the flagship production database — healthy, converged, re-fitting
// nothing — printed `verdict FAILED` and exited 1 on EVERY run, because 9 of its
// 867 cells are permanently unfittable.
//
//   NEVER STORED   the fit for that (partition, symbol) permanently fails, so
//                  nothing was ever written there. Expected, permanent, and not a
//                  defect. On `prod-2026-07` that is 9 cells over 8 symbols, each
//                  of which fits fine on its other 16 dates.
//   STORED, GONE   the surface WAS written and the partition no longer carries
//                  it. That is data loss.
//
// The partition's own ARCHIVE DIRECTORY separates those from the corruption case:
// it lists exactly the symbols the file holds. Absent from the directory => it
// was never stored (`cells_absent`). Present in the directory and the map fails
// => something that should be readable is not (`cells_unmappable`).
//
// WHAT THE DIRECTORY CANNOT DO is tell the two ABSENCES apart, because the format
// keeps no tombstone: a cell a whole-partition rewrite destroyed is byte-for-byte
// a cell that was never fitted (`SurfaceDbPopulate.DegradedCellLosesItsStored
// SurfaceAndPresenceIsWhatDrivesTheRetry` pins that loss as current behaviour).
// So `ok()` deliberately ignores `cells_absent` — a permanently-red verdict is
// not a signal, and one that fires on a converged database trains the operator to
// stop reading it — and the report NAMES the absent cells instead, under the same
// cap and the same never-silent elision count as the faults, so what an operator
// watches is the absent SET rather than the fact that absence exists. Same
// discipline, for the same reason, as `is_carry_masked_fit_failure`
// (surface_db_build.hpp), which took the identical decision one tool over.
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
#include <span>
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

// Why one cell FAILED. The three kinds are three DIFFERENT questions answered in
// order, and they stay distinct so a fault names its own root cause.
//
// A cell the partition never stored is NOT one of them and deliberately has no
// enumerator here (see `DbAbsentCell`): everything in this enum is a
// corruption-class fault that moves `ok()`, and absence is not. Giving absence a
// kind would have put it in `failures` beside the faults, where the counter that
// drives the verdict would have had to special-case it and a `fail` line would
// have kept saying "fail" about a cell nothing is wrong with.
enum class DbCellFailure : std::uint8_t {
  // The partition's directory says the symbol IS there and `map_surface` still
  // did not return a surface — or the partition could not be opened AT ALL
  // (file missing, framing/CRC damage at open), in which case the directory that
  // would have answered "was this ever stored?" is itself what is unreadable.
  // Either way something that should be readable is not. A symbol simply absent
  // from a partition the walk opened fine is NOT this — it is `cells_absent`.
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

// One selected (partition, symbol) cell the partition's archive directory does
// NOT list — the surface was never stored there. No `detail`, on purpose: there
// is no error to quote, because nothing failed. The cell's IDENTITY is the whole
// payload, because the operator's question is never "why is this one absent?"
// (the database cannot know) but "is this the same set that was absent last
// time?".
struct DbAbsentCell {
  std::string key{};
  std::string symbol{};
};

// One partition whose MANIFEST RECORD disagrees with the partition FILE it
// indexes — the "opens but is wrong" case, and the only inconsistency the
// per-cell gates below structurally cannot see, because they read the file and
// the file is fine.
//
// HOW IT HAPPENS. `SurfaceDb::write_partition` writes the archive FIRST and the
// manifest SECOND. That order is correct and is argued at the call: it means an
// interrupted archive write never advances the partition index. The cost of that
// choice is this window — a crash, or a `persist_locked` error, BETWEEN the two
// leaves the newly rewritten file beside a STALE record whose `surface_count`,
// `file_size`, `created_ts_ns` and carry-over `config_fingerprint` all describe
// the previous write. The database then opens cleanly and reports wrong numbers,
// which is worse than one that fails to open, because nothing looks wrong.
//
// WHAT IT CANNOT CAUSE, checked rather than assumed: a false carry. The carry
// gate recomputes its fold over the ARCHIVE's own directory (surface_db_populate),
// so a torn write moves the symbol set, moves the fold, and fails the gate closed.
// The exposure is bookkeeping — `info` / `partitions` byte and surface counts, and
// any consumer that trusts them — not stored-surface soundness.
struct DbPartitionIndexFault {
  std::string key{};
  std::uint32_t manifest_surface_count{0}; // what the manifest record claims
  std::uint32_t archive_surface_count{0};  // what the file's own directory holds
  std::uint64_t manifest_bytes{0};         // file bytes the record recorded at write time
  std::uint64_t bytes_on_disk{0};          // file bytes now
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
  //
  // It caps `DbVerifyReport::absent_cells` too, and the two budgets are
  // INDEPENDENT: this is one number spent twice, never one pool shared. Sharing
  // would have made absence able to hide corruption — a converged production
  // database carries absences permanently (9 on `prod-2026-07`) and they are
  // recorded first, so a shared 32-cell pool would elide the ONE checksum fault
  // that appeared beside 40 absences. The cap exists to bound output, not to
  // ration it between two questions of different severity.
  std::size_t max_reported_failures{kSurfaceDbVerifyMaxFailures};
};

// The health verdict. `cells_checked == cells_ok + cells_absent +
// cells_unmappable + cells_non_finite + cells_checksum` always holds.
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

  // Cells the walk selected that their partition's archive directory does NOT
  // list. NOT a fault, NOT in `failures`, and NOT read by `ok()` — see the
  // ABSENCE IS NOT A FAULT block at the top of this header for the whole
  // argument. The short form: this counter is PERMANENTLY NON-ZERO on a healthy
  // converged database (every permanently-unfittable cell lands here on every
  // run, forever), so a verdict that read it would be permanently FAILED, which
  // is the defect FIX-H exists to remove.
  //
  // It is also the only place a DESTROYED surface shows up AFTER THE FACT. A
  // present, enabled cell whose re-fit fails would lose its stored surface,
  // because a partition rewrite is whole-file — 95 surfaces were lost that way in
  // one measured run on a production-shaped copy. REV-R3 made the BUILD refuse
  // such a rewrite by default (`coverage.dates_refused_coverage_regression`, exit
  // 5) and name the cells at risk at the time, so this counter is no longer the
  // only instrument — but it remains the only one that works retroactively, and
  // the build's guard can still be waived with `--allow-coverage-regression`.
  // Preserving the prior surface with a persisted stale marker (rather than
  // refusing the write) still needs an archive format change; the mechanism is
  // pinned by
  // `SurfaceDbPopulate.DegradedCellLosesItsStoredSurfaceAndPresenceIsWhatDrives
  // TheRetry`. A destroyed cell and a never-fitted cell are byte-for-byte
  // identical on disk; nothing in the format records which happened.
  //
  // That is why this is a COUNT plus a NAMED, CAPPED LIST rather than a verdict:
  // the discriminator is not the number's existence but whether the SET CHANGED
  // since the last run, which only a reader (or an operator-supplied ceiling —
  // `atx-vol-surface-db verify --max-absent N`) can decide. Making it silent
  // would trade a false alarm for a silent data loss; making it a verdict would
  // reinstate the false alarm. It is loud and it does not judge.
  std::size_t cells_absent{0};
  std::vector<DbAbsentCell> absent_cells{}; // capped at spec.max_reported_failures
  std::size_t n_absent_elided{0};           // absences NOT in `absent_cells` (never silent)
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

  // Partitions IN RANGE whose manifest record disagrees with their file — see
  // `DbPartitionIndexFault`. CORRUPTION-CLASS: this one DOES move `ok()`, unlike
  // `cells_absent`, and the difference is that it can never be a healthy steady
  // state. A converged production database has permanent absences and ZERO index
  // mismatches; a mismatch is always either a torn write or an edited directory.
  //
  // It is per-PARTITION, so it sits OUTSIDE the `cells_checked` exhaustion
  // invariant above and is counted on its own budget (a database with many
  // absences must not be able to elide the one index fault beside them, the same
  // argument `DbVerifySpec::max_reported_failures` makes for the other two lists).
  // A partition whose file will not open at all is NOT counted here: every cell on
  // that row is already reported `unmappable`, which is the louder and more
  // precise answer, and a second fault over the same bytes would double-report it.
  std::size_t partitions_index_mismatch{0};
  std::vector<DbPartitionIndexFault> index_faults{}; // capped at spec.max_reported_failures
  std::size_t n_index_faults_elided{0};              // never silent

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

  // The walk DID read cells, and the database held NOT ONE of them. Every
  // selected cell is absent and nothing failed a gate, so `ok()` is true, every
  // fault list is empty, and the only non-zero counters are `cells_checked` and
  // `cells_absent`. `selected_no_cells` cannot see this — it asks whether the walk
  // was empty, and this walk was not — and neither can `--min-cells`, which counts
  // the GRID and is fully satisfied by a grid of pure holes.
  //
  // A WARNING, NEVER A VERDICT, and the reason is the same one that keeps
  // `cells_absent` out of `ok()`: this shape is reachable on a perfectly healthy
  // database by an ordinary, CORRECT invocation. `verify --symbols MCD
  // --from 2026-07-01 --to 2026-07-01` against the finished `prod-2026-07` — an
  // operator checking the one cell they already know is absent — is
  // `cells_checked 1, cells_ok 0, cells_absent 1`. So is `--symbols` naming a
  // ticker the manifest never configured, which `DbVerifySpec::symbols` documents
  // as a legitimate thing to assert about. Mapping this to an exit code would make
  // the narrowest, most deliberate use of the tool red on a healthy database,
  // which is the defect FIX-H removed wearing a smaller blast radius.
  //
  // WHAT IT IS FOR. On a database whose steady-state absent count is ZERO — most
  // databases, including the manual's worked-session one — "every cell I looked at
  // is a hole" is a real alarm, and before FIX-H it arrived as `unmappable` and a
  // FAILED verdict. It now arrives as `ok`, so something has to say it out loud.
  // Two readings, and this predicate makes no claim about which:
  //   - you NARROWED the walk onto cells the database legitimately does not hold
  //     (an unconfigured `--symbols` name; a window whose every cell permanently
  //     fails). The answer is correct and nothing is wrong.
  //   - the database holds nothing WHERE YOU LOOKED — never built over that
  //     window, built over the wrong one, or every surface there was destroyed.
  // The scriptable form of the second reading is `--max-absent N`; a database that
  // expects no holes pins `--max-absent 0` and gets a non-zero exit for any
  // absence at all, this shape included.
  //
  // DISJOINT FROM A CELL-FAULT VERDICT BY CONSTRUCTION, not by the order a caller
  // tests things in: `cells_absent == cells_checked` forces every CELL fault
  // counter to zero through the exhaustion invariant above, so this can never fire
  // on a run that also reports a corrupt cell. (`--min-cells` can still fail the
  // same run; that is a floor on the grid and an orthogonal statement, and both are
  // then true. So can `partitions_index_mismatch`, for the same reason — it is a
  // statement about a partition RECORD, not about a cell, so it lives outside the
  // exhaustion invariant this disjointness rests on.)
  [[nodiscard]] constexpr bool stored_no_selected_cell() const noexcept {
    return cells_checked > 0 && cells_absent == cells_checked;
  }

  // CORRUPTION-CLASS ONLY, and `cells_absent` is deliberately not in the list.
  // The verdict answers "is everything this database STORED still good?" — every
  // term here is a byte or a number that went wrong under a cell that exists, or
  // (the partition-index term) an index entry that no longer describes the file it
  // points at. A mismatch there is never a healthy steady state, which is exactly
  // why it belongs here and absence does not.
  // "Which cells does it not hold?" is a different question with a different
  // audience and no in-band answer (a never-fitted cell and a destroyed one are
  // indistinguishable on disk), so it gets its own counter and its own list and
  // leaves this alone. Adding it here would make every converged production
  // database read FAILED forever, which is precisely the state FIX-H found.
  [[nodiscard]] constexpr bool ok() const noexcept {
    return cells_unmappable == 0 && cells_non_finite == 0 && cells_checksum == 0 &&
           partitions_index_mismatch == 0 && !selected_no_cells();
  }
};

// Walk the (partition x symbol) grid selected by `spec`. Each ROW (= partition)
// is cross-checked against its manifest record first; then each cell passes three
// gates, in this order, stopping at the first that fails — preceded by one TRIAGE
// step that decides whether the cell is in the game at all:
//
//  -1. index      — per PARTITION, not per cell. Compare the manifest record's
//                   `surface_count` / `file_size` against the file's own directory
//                   count and its size on disk. A mismatch is
//                   `partitions_index_mismatch` and it DOES move the verdict; see
//                   `DbPartitionIndexFault` for the torn-write window it catches
//                   and for why no per-cell gate can see it. A partition that will
//                   not open is skipped here and left to gate 0/1, which report
//                   every one of its cells `unmappable`.
//
//   0. presence   — only when the map below fails, and only when it failed with
//                   NotFound. Re-open the cell's partition and ask its archive
//                   DIRECTORY whether the symbol is there. Not there =>
//                   `cells_absent`, and no fault is recorded: the surface was
//                   never stored, and no gate can be applied to bytes that do not
//                   exist. There => the mapping failure is real and falls through
//                   to `Unmappable`. When the partition itself cannot be opened,
//                   the directory is exactly what is missing, so the cell stays
//                   `Unmappable` (the corruption reading), and that is the case
//                   the removed-partition-file test pins. Any error OTHER than
//                   NotFound stays a fault whatever the directory says.
//   1. map        — `SurfaceDb::map_surface`; proves magic, framing and bounds.
//   2. checksum   — `SurfaceArchiveV2::validate_symbol` over the archive the map
//                   already holds open; proves the payload BYTES are the ones the
//                   writer checksummed. Ordered before the probe because a byte
//                   fault is the root cause of any number it produces.
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
// Gates -1 and 0 share ONE partition open per row, taken up front. Gate 0's use
// of it is unchanged in every observable way — the directory is still consulted
// only to CLASSIFY a mapping failure the map already produced, and the mapping
// path stays the primary gate (see "Why map_surface and not load_surface" above).
// What changed is only WHEN the open happens: gate -1 needs the file's own
// directory count on EVERY row, including rows where nothing failed, because a
// stale index record is invisible from the cells (they all map fine — the file is
// the correct one, it is the record that lies). So the open can no longer be
// lazy, and making gate 0 re-open the same file afterwards would have been
// strictly worse. The cost is O(partitions in range), not O(cells): one extra
// header+directory read per row on a walk that is about to map every cell in it.
//
// A cell failure is TALLIED, never fatal — the whole point is to enumerate every
// broken cell in one pass. Err is reserved for a spec that cannot be honored:
// InvalidArgument (non-finite / non-positive `probe_T`, an empty or oversized
// entry in `spec.symbols`) or a manifest whose stored config fails to decode.
//
// The report is all-ok (`DbVerifyReport::ok()`) exactly when every selected cell
// that the database ACTUALLY HOLDS passed all three gates AND the walk actually
// selected cells (see `selected_no_cells`). Absent cells do not move the verdict;
// they are counted and named (`cells_absent` / `absent_cells`) and a caller that
// wants to gate on them must supply its own expected ceiling, because nothing in
// the database knows what that ceiling is. A caller wiring this into a script
// should branch on `ok()`, not on the Result.
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
// STATE THE OTHER DIRECTION TOO, because it is the worse one and "your disable
// gets overwritten" reads as if the build always wins. Both `upsert_symbol` and
// `write_partition` rewrite the ENTIRE manifest — symbol table AND partition
// table — from their own in-process snapshot, with no compare-and-swap on
// `generation`. So an interleaved disable can just as easily DROP a partition
// record the build has already committed, and can regress the generation counter;
// once it has regressed, `SurfaceDb::refresh` will not pick the newer manifest up,
// because it compares generations. The loss there is a partition the database
// stops knowing about, not a setting that did not stick. Closing this properly
// means a generation compare-and-swap on the manifest rewrite, which is a real
// design change and not a doc fix.
//
// Errors: NotFound when `symbol` is not in the manifest's symbol table (this call
// never CREATES a config — an operator fencing out a name they cannot see named
// the wrong name, and inventing a default config for it would be the silent
// wrong answer); InvalidArgument on an empty canonical symbol; IoError/ParseError
// propagated from the manifest rewrite.
[[nodiscard]] Result<SymbolEnableChange> set_symbol_enabled(SurfaceDb &db, std::string_view symbol,
                                                            bool enabled);

// ── tenor_audit — Task 3 (mark-domain-robustness) ────────────────────────────
//
// A built database's build report (`surface_db_build.hpp`'s `slice_drop.*`
// section) names a drop AT BUILD TIME, over the one date being written. This is
// the AFTER-THE-FACT counterpart: walk an already-built database, symbol by
// symbol, and flag a (date, symbol) cell whose fitted tenor reach (Task 1's
// `PricedSurface::tenor_domain().max_T`) is suspiciously SHORT next to its
// neighbors in the SAME symbol's date-ordered history — the shape a stressed
// day produces when it silently truncates a long-dated expiry that every
// nearby day still reaches.
//
// AN AUDIT, NOT A GATE. Every row is reported regardless of its flag; nothing
// here refuses or quarantines anything. The exit-code opt-in lives on the CLI
// (`--fail-on-truncated`), not on this call.

// Deliberately NOT year-aware, NOT symbol-aware beyond the grouping described
// at `tenor_audit` below, and computed PER SYMBOL: mixing two symbols' max_T
// series (an index vs. a single-name, say) into one rolling window would
// compare tenors that were never comparable to begin with.
struct TenorAuditRow {
  std::string date{};   // canonical partition key
  std::string symbol{}; // canonical, as audited
  std::size_t n_slices{0};
  double min_T{0.0};
  double max_T{0.0};
  // TRUNCATED iff this row's max_T is more than 0.25 (year-fraction) below the
  // median max_T of its up to 5 nearest OTHER partitions in this SAME symbol's
  // date-ordered series (see `tenor_audit` for the exact neighbor selection).
  // Always false when there were no neighbors at all to compare against (a
  // symbol with a single partition) — there is nothing to be short RELATIVE
  // TO.
  bool truncated{false};
};

struct TenorAuditSpec {
  // Which symbols to audit, and in what order the report groups them. Empty
  // (default) = every symbol the manifest currently configures
  // (`SurfaceDb::symbols()` — canonical, sorted), REGARDLESS of its stored
  // `enabled` bit: a symbol disabled after it fitted keeps its stored
  // surfaces (nothing on the read path gates on `enabled`, `surface_db_admin
  // .hpp`'s standing rule), and those surfaces are exactly as auditable as an
  // enabled symbol's.
  //
  // An EXPLICIT (non-empty) list, unlike `DbVerifySpec::symbols`, MUST name
  // only symbols the manifest currently configures (Fix Round 1, Important
  // 2): `tenor_audit` rejects an unconfigured name with `NotFound` rather
  // than silently auditing zero rows for it. This is narrower than
  // `verify_db`'s "an unconfigured name is a legitimate thing to assert
  // about" stance, deliberately: an EMPTY tenor-audit walk and a FULLY
  // TRUNCATION-FREE one are otherwise indistinguishable by `n_truncated`
  // alone, which is exactly the shape that made `--fail-on-truncated`
  // silently pass on a typo'd `--symbol`.
  std::vector<std::string> symbols{};

  // Cap on `TenorAuditReport::skip_notes`, same discipline as `DbVerifySpec
  // ::max_reported_failures` (Fix Round 1, Important 3): a sparse production
  // universe can have O(10^5-10^6) (date, symbol) cells a symbol was never
  // written into, and every one of those is a skip note under the doc above.
  // Uncapped, that is a multi-hundred-MB vector of heap strings for a walk
  // whose per-cell TRUNCATED verdict never depended on the note text at all.
  // Extra notes are counted in `n_skip_notes_elided`, never dropped silently
  // — the same "bounded, not rationed, and never silently clean" shape as
  // `max_reported_failures`, on its OWN budget (nothing else competes for it).
  std::size_t max_skip_notes{kSurfaceDbVerifyMaxFailures};
};

struct TenorAuditReport {
  // Grouped by symbol in AUDIT order (the order `symbols` above resolved to);
  // ascending by date WITHIN a group. The rolling median in `truncated` above
  // is computed per group, never across a group boundary.
  std::vector<TenorAuditRow> rows{};
  std::size_t n_truncated{0}; // == count of `rows` with `truncated == true`
  // One free-text note per (date, symbol) this walk could NOT load — a
  // partition that does not carry the symbol, or a partition file that will
  // not open at all. Never fatal (see `tenor_audit`'s contract): the audit
  // skips the cell and keeps going, exactly the discipline `verify_db` uses
  // for its own load failures ("ABSENCE IS NOT A FAULT" at the top of this
  // header) — except an audit has no directory-vs-map distinction to make, so
  // every load failure, whatever its cause, is a skip here.
  //
  // Capped at `TenorAuditSpec::max_skip_notes` (Fix Round 1, Important 3);
  // notes beyond the cap are counted in `n_skip_notes_elided`, not dropped
  // silently. The SKIP ITSELF is never capped — every (date, symbol) cell
  // that fails to load is still excluded from `rows` and still absent from
  // the rolling-median comparison, whether or not it got a note. The cap
  // bounds the TEXT, not the accounting.
  std::vector<std::string> skip_notes{};
  std::size_t n_skip_notes_elided{0};
};

// Walk every partition (`SurfaceDb::partitions()`, already sorted ascending by
// canonical key) for each symbol in `spec.symbols` (or every manifest symbol
// when empty), loading each (date, symbol) cell with `SurfaceDb::load_surface`
// and reading its `PricedSurface::tenor_domain()` (Task 1). A load failure —
// the symbol absent from that date's partition, or the partition file itself
// unreadable — is SKIPPED, not fatal: recorded in `skip_notes` and the walk
// continues with the next cell. This is an inspector over a database that may
// legitimately have per-symbol gaps (a symbol added mid-history has no
// partitions before it existed), not a completeness check.
//
// ROLLING-MEDIAN NEIGHBOR SELECTION, exactly. For row `i` of a symbol's
// date-ordered series (length `n`, `i` zero-based), take up to 5 OTHER rows,
// nearest by INDEX DISTANCE, ties (a before-row and an after-row equidistant
// from `i`) broken toward the EARLIER date. This is the "2 before + 2 after +
// 1 more from whichever side has slack" rule stated as one sort: at an
// INTERIOR row with at least 3 rows on each side, distance-1 before,
// distance-1 after, distance-2 before, distance-2 after and distance-3 before
// sort as the nearest 5 (the tie-break sends the wildcard 5th neighbor
// earlier rather than later), giving 3 before + 2 after — one valid resolution
// of the wildcard the brief did not pin further. FEWER AT EDGES falls out of
// the same sort with no special-casing: a row near either end of the series
// simply has fewer "other" rows to draw from, so it takes however many exist
// (down to zero for a symbol with a single partition, which is never
// flagged — see `TenorAuditRow::truncated`). The comparison value is the
// MEDIAN of the selected neighbors' `max_T` (the ordinary two-middle-average
// definition on an even neighbor count); a row is TRUNCATED when its own
// `max_T` is more than 0.25 below that median.
//
// Err: `NotFound` when `spec.symbols` names a symbol the manifest does not
// currently configure (Fix Round 1, Important 2 — see `TenorAuditSpec
// ::symbols`'s doc for why this is checked up front rather than left to
// silently audit zero rows for that name). No other Err is reachable today —
// `db.symbols()`/`db.partitions()` cannot fail for an already-open
// `SurfaceDb` — but the empty-`spec.symbols` walk stays `Result` for symmetry
// with the rest of this header and so a future spec restriction has
// somewhere to put an error without changing the signature.
[[nodiscard]] Result<TenorAuditReport> tenor_audit(const SurfaceDb &db,
                                                    const TenorAuditSpec &spec = {});

// ── band_audit — full-chain quote-fidelity inspector ─────────────────────────
// AFTER-THE-FACT counterpart of the fit's own parity diagnostics, measured
// against the FULL LISTED CHAIN instead of the fit's admitted rows: for each
// (date, symbol) cell, load the stored surface AND the OPRA hive chain at the
// build snapshot, reprice every two-sided listed quote through
// PricedSurface::fair_value, and report per-expiry band statistics. This is
// the detector for the 2020-03-18 / 2025-04-10 defect class (model above the
// ask of the whole listed belly/wing) and the before/after instrument for the
// fix validation. AN AUDIT, NOT A GATE (same stance as tenor_audit): every
// row prints; the exit-code opt-in lives on the CLI.
//
// Fix round 1 (review Important 1): "the build snapshot" above is the
// ARCHIVED SURFACE's OWN stored valuation instant
// (`PricedSurface::pricing().now_ts_ns`), not a hardcoded UTC hour. A hive is
// ET-anchored (the production orchestrator's `--snap-et` pull), so the same
// wall-clock pre-close minute lands at two different UTC hours depending on
// DST; deriving the hive snapshot from the surface itself is DST-proof by
// construction instead of assuming EDT year-round. A surface with no usable
// stored timestamp falls back to a DST-aware RECONSTRUCTED suffix and says so
// via `BandAuditReport::snapshot_fallback_notes`.

struct BandAuditRow {
  std::string date{};
  std::string symbol{};
  double T{0.0};                           // expiry year-fraction at the snapshot
  std::size_t n{0};                        // scored quotes (two-sided, finite model)
  double frac_in_band{0.0};                // bid <= model <= ask
  double frac_above_ask{0.0};              // model > ask (the spike signature)
  double avg_signed_err_half_spreads{0.0}; // mean (model - mid) / half-spread
  double max_abs_err_half_spreads{0.0};    // max |model - mid| / half-spread
  bool flagged{false};                     // n > 0 && frac_in_band < floor
};

struct BandAuditSpec {
  std::vector<std::string> symbols{};      // empty = every manifest symbol
  std::string hive_root{};                 // REQUIRED: OPRA hive v2 root
  std::string date_lo{};                   // empty = first partition key
  std::string date_hi{};                   // empty = last partition key
  std::string snapshot_suffix{"T19:55:00Z"};
  double r{0.0};                           // flat fallback rate for the chain env
  double min_frac_in_band{0.30};           // per-expiry flag floor
  std::size_t max_skip_notes{kSurfaceDbVerifyMaxFailures};
};

struct BandAuditReport {
  // Date-major (the hive loads one file per date), symbol-major within a
  // date, ascending T within a cell.
  std::vector<BandAuditRow> rows{};
  std::size_t n_flagged{0};
  std::vector<std::string> skip_notes{};   // capped like TenorAuditReport
  std::size_t n_skip_notes_elided{0};
  // Fix round 1 (review Important 1): one note per (date, symbol) cell whose
  // archived surface carried no usable stored valuation timestamp
  // (`PricedSurface::pricing().now_ts_ns`), so the hive reprice fell back to a
  // DST-aware RECONSTRUCTED snapshot suffix instead of the surface's own
  // exact instant. Never fatal — the cell is still scored under `rows` — but
  // worth surfacing (the observability constraint the fallback exists under):
  // a fallback-scored row is only as precise as the DST rule's calendar-day
  // granularity, not the surface's bit-exact fit instant. Capped the same way
  // as `skip_notes`, on its own budget.
  std::vector<std::string> snapshot_fallback_notes{};
  std::size_t n_snapshot_fallback_notes_elided{0};
};

// Pure per-expiry scorer (unit-tested): a quote is scored when bid > 0,
// ask > bid and model_price is finite; counts via band_violation_stats
// (fit_metrics.hpp:192-195, which skips the same rows), half-spread stats
// over the same scored subset. date/symbol/T are left default for the caller.
[[nodiscard]] BandAuditRow score_expiry_band(std::span<const double> model_price,
                                             std::span<const double> bid,
                                             std::span<const double> ask,
                                             double min_frac_in_band);

// Err: InvalidArgument for an empty hive_root; NotFound when spec.symbols
// names an unconfigured symbol (the tenor_audit stance — checked BEFORE any
// hive IO so a typo fails loud without a hive present). A cell that fails to
// load (surface or chain) is a skip note, never fatal.
[[nodiscard]] Result<BandAuditReport> band_audit(const SurfaceDb &db,
                                                 const BandAuditSpec &spec);

} // namespace atx::vol
