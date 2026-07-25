#pragma once

// surface_db_build — build a production SurfaceDb from OPRA boards, in two
// stages that share this header:
//
//   1. `generate_symbol_configs` — auto-generate the manifest's per-symbol fit
//      configuration from a set of loaded boards (below).
//   2. `build_surface_db` — the one-call driver that chains hive load ->
//      config generation -> streaming populate into a single create-or-open
//      call, plus `write_build_report_csv` for its report (bottom of file).
//
// ── Stage 1: `generate_symbol_configs` ──────────────────────────────────────
//
// Where `surface_db_populate.hpp` FITS boards into partitions, this stage decides
// HOW each symbol should be fit and writes that decision into the manifest's
// symbol table (`SurfaceDb::upsert_symbol`) BEFORE any populate runs. For each
// symbol it takes one representative board, classifies it through the shared
// atx-vol fit-policy seam (`select_fit_policy`, optionally the full held-out
// `select_curve` search), and stores a `SymbolFitConfig` whose curve family is
// pinned to the policy's choice. A symbol whose board cannot be selected on is
// stored DISABLED (fail closed — never silently served), the top-level call
// still succeeding so one bad board never sinks a universe build.
//
// ── Ownership / thread-safety ────────────────────────────────────────────────
//
// `generate_symbol_configs` borrows `db` (mutated via its own serialized
// `upsert_symbol`) and `boards` (read-only) for the call and returns an owning
// report. It is itself single-threaded: it walks symbols in canonical order and
// upserts one at a time, so re-running it is deterministic. Not safe to call
// concurrently with other mutators on the SAME `db` (they share the manifest
// mutex, but interleaved upserts would race the skip-existing/idempotence
// bookkeeping this stage owns). `build_surface_db` is likewise single-threaded
// at the driver level (its fits fan out internally); one build per db root.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"             // CorpusBoard
#include "atx/vol/opra_hive.hpp"          // OpraHiveSpec
#include "atx/vol/surface_db.hpp"         // SurfaceDb, SymbolFitConfig, FitPreset
#include "atx/vol/surface_db_populate.hpp" // UniversePopulateCoverage, PopulateSymbolStats
#include "atx/vol/types.hpp"             // Result, Status

namespace atx::vol {

// How to auto-generate the manifest configs. Field names/defaults are
// contractual (Task 5's build driver constructs this directly).
struct AutoConfigSpec {
  // Board date used for per-symbol selection. Empty ("") => each symbol's
  // EARLIEST available board (min date string) is used. A non-empty date a given
  // symbol has NO board for falls back to that symbol's earliest board (per
  // symbol, silently — one symbol missing the requested date never fails the
  // call or skips the symbol).
  std::string config_date{};
  // Base preset captured into every symbol's config (`symbol_config_from_preset`).
  // The fit-policy decision pins the curve FAMILY; this preset is the numerical
  // tier and is retained (matching `populate_universe_streaming`'s seeding).
  FitPreset preset{FitPreset::Populate};
  // The designated index leg — pinned to the dense index recipe (shared with
  // `populate_universe_streaming` via `seed_symbol_config`), bypassing per-board
  // selection. Empty => no index leg.
  std::string index_symbol{};
  // false (default) => per-symbol selection uses `select_fit_policy` board
  // features only (O(quotes), no fit). true => additionally run the full
  // held-out `select_curve` OOS search and pin its winner; a NotFound/Unavailable
  // selector outcome falls back to the fit-policy decision.
  bool deep_selection{false};
  // false (default) => a symbol already present in the manifest is left
  // UNTOUCHED (idempotent resume; operator overrides win). true => overwrite it.
  bool overwrite_existing{false};
};

// What `generate_symbol_configs` did. The three disposition counters partition
// the distinct symbols it saw: `n_configured + n_skipped_existing +
// n_disabled_failed == n_symbols`.
struct AutoConfigReport {
  std::uint32_t n_symbols{0};          // distinct symbols in `boards`
  std::uint32_t n_configured{0};       // freshly configured (or overwritten), enabled
  std::uint32_t n_skipped_existing{0}; // already present, left untouched (not overwrite)
  std::uint32_t n_disabled_failed{0};  // selection failed -> stored disabled
  std::vector<std::string> failed_symbols; // sorted; mirrored as disabled configs in the db
};

// Auto-generate one `SymbolFitConfig` per distinct symbol in `boards` and upsert
// it into `db`'s manifest.
//
// Per symbol (canonical order): pick the config board (`spec.config_date` or the
// symbol's earliest); if already configured and not `spec.overwrite_existing`,
// SKIP it (`n_skipped_existing`); if it is `spec.index_symbol`, store the dense
// index recipe (`seed_symbol_config`); otherwise build the board's `OptionChain`
// (`OptionChain::from_frame`, the corpus_board_fit path), classify it with
// `select_fit_policy`, and store `symbol_config_from_preset(spec.preset)` with
// the policy's curve pinned (`pin_curve = true`, `curve = decision.curve`). When
// `spec.deep_selection`, `select_curve`'s held-out winner is pinned instead,
// falling back to the fit-policy decision on a NotFound/Unavailable selector
// outcome. A symbol whose board fails to build a selectable underlying — or whose
// deep selection fails with a hard (non-fallback) error, or throws — is stored
// DISABLED (`symbol_config_from_preset(spec.preset)` with `enabled = false`) and
// recorded in `failed_symbols` (`n_disabled_failed`); the call still succeeds.
//
// @return the disposition report, or an Error only on a db write failure
//         (`upsert_symbol` IoError/ParseError propagated). An empty `boards`
//         span yields an all-zero report (Ok), not an error.
[[nodiscard]] Result<AutoConfigReport>
generate_symbol_configs(SurfaceDb &db, std::span<const CorpusBoard> boards,
                        const AutoConfigSpec &spec);

// ── Stage 2: `build_surface_db` — the one-call build driver ─────────────────
//
// Everything a production build needs in one call: (1) create the db at
// `db_root` iff it has no manifest yet, else open the existing one (mirrors
// `SurfaceDb::open`'s NotFound probe — a resumed build reuses the same root);
// (2) load the `hive` window (`load_opra_hive`); (3) build one `CorpusBoard`
// per SUCCESSFULLY loaded cell (missing/corrupt cells are tallied, never fit);
// (4) `generate_symbol_configs` over those boards (`auto_config`); (5)
// `populate_universe_streaming` them (cell-aware resume, RSS O(dates in
// flight)) with an index leg / preset / worker budget from this spec.
//
// Field names/defaults are contractual (the CLI and Python bindings construct
// this directly).
struct SurfaceDbBuildSpec {
  std::string db_root;         // created if absent, else opened (resume)
  OpraHiveSpec hive;           // the hive window to load (root_dir/date span/symbols)
  AutoConfigSpec auto_config{}; // per-symbol config-generation policy (stage 1)
  // Numerical tier every symbol's fit runs at (passed to the populate);
  // `auto_config.preset` seeds the manifest, this drives the populate fallback.
  FitPreset preset{FitPreset::Populate};
  unsigned fit_workers{0}; // 0 = auto (honors ATX_VOL_FIT_WORKERS); 1 = serial
};

// The full disposition of a `build_surface_db` call: the stage-1 config report,
// the stage-2 populate coverage, and the ingest tallies. The two DATE counters
// describe the hive load in distinct dates, not cells: `n_dates_loaded` is the number
// of distinct dates that produced at least one board; `n_dates_missing` is the
// number of distinct in-range dates that produced NONE (a fully absent or
// unreadable date) — the window is enumerated as CALENDAR days, so every weekend
// and market holiday in range counts as missing (a July window always shows ~9).
//
// The last two split the loader's `n_error` cells into the two things an operator
// must NOT confuse: `n_load_errors` is the CELL count of real defects — a present
// file that is unreadable/unparseable, has the wrong schema, or whose market
// inputs quarantined the cell; `n_coverage_holes` is the CELL count of a present,
// readable date file that simply does not carry that symbol. A real hive has
// non-uniform per-date coverage, so a discover-all build reports MANY holes and
// that is healthy — it is `n_load_errors` that means "something is wrong with the
// data". Both are classified structurally by `load_opra_hive` (never inferred from
// an error code), and together they exhaust the loader's `n_error`. Neither ever
// reaches the fit.
struct SurfaceDbBuildReport {
  AutoConfigReport config;
  UniversePopulateCoverage coverage;
  std::size_t n_dates_loaded{0};
  std::size_t n_dates_missing{0};
  std::size_t n_load_errors{0};
  std::size_t n_coverage_holes{0};
};

// Did this build attempt work and get NOTHING out of it? True iff it scheduled at
// least one cell (`coverage.cells_to_fit > 0`) and not one of them fitted
// (`coverage.cells_ok == 0`) — the signature of a systematically wrong build
// input, the carry-rate mismatch (`OpraHiveSpec.r` disagreeing with the rate the
// hive's quotes were priced under) being the top suspect: every put-call-parity
// forward is then wrong and every full fit fails identically.
//
// Deliberately NARROW — the two neighbouring shapes are both healthy and must not
// be swept in:
//   - PARTIAL failure (`cells_ok > 0` with some `cells_failed`) is normal in
//     production: real hives carry unfittable boards. Not a failure.
//   - NOTHING TO DO (`cells_to_fit == 0`) is the resume path over an already
//     complete database (and the un-pulled empty window). The build's convergence
//     guarantee is exactly "a re-run fits zero", so this must stay a success.
//
// Pure predicate over the report; the CLI uses it to pick its exit code, which is
// why the decision lives here (testable) and not in `main`.
[[nodiscard]] bool is_total_fit_failure(const SurfaceDbBuildReport &r);

// Run the whole build (see `SurfaceDbBuildSpec`). Idempotent/resumable: re-running
// over an unchanged hive re-fits ZERO (configs skip-existing, the populate's
// cell-aware filter writes no date) ONCE every loaded cell has either fitted
// successfully or been config-disabled. A cell that FAILS to fit is deliberately
// retried — there is no persisted known-failed state — so it keeps its date in the
// rewrite set and that date's siblings are re-fit on every run: the price of giving
// a transient failure another chance. A grown hive fits only the new dates. An
// EMPTY window (un-pulled days) is a graceful success with all-zero coverage — the
// db is still created. Top-level Err only on a malformed hive spec (`load_opra_hive`)
// or a db config/write failure; a single unloadable/unselectable board never
// aborts the build (it is tallied and, for config, stored disabled).
[[nodiscard]] Result<SurfaceDbBuildReport> build_surface_db(const SurfaceDbBuildSpec &spec);

// Write `r` as a two-section CSV (reuses `write_populate_stats_csv`'s formatting
// discipline: an owned buffer flushed to a binary/truncating stream, IoError on
// open/write failure). Section 1 is a `key,value` table of every scalar counter
// (config.*, coverage.*, and the ingest counters n_dates_loaded / n_dates_missing
// / n_load_errors / n_coverage_holes); section 2 is a
// `symbol,n_attempted,n_ok,n_failed,n_disabled` row per `coverage.per_symbol`
// entry. The first line is always the pinned header `key,value`.
[[nodiscard]] Status write_build_report_csv(const SurfaceDbBuildReport &r, std::string_view path);

} // namespace atx::vol
