#pragma once

// surface_db_populate — the "fit + store" pipeline stage: fit OPRA boards
// (CorpusBoard, corpus.hpp) into a SurfaceDb, honoring each symbol's
// SymbolFitConfig from the db manifest, and store one partition per date.
//
// This is deliberately NOT `build_corpus` pointed at a SurfaceDb: the ONLY
// fit-policy difference from a plain corpus build is that EVERY board's fit
// inputs are overlaid with the symbol's manifest config (`apply_symbol_config`)
// before the fit runs, so a per-symbol operator override (a pinned curve, a
// tighter band_k, ...) reaches the actual fit — see populate_surface_db's
// doc-comment below. The board -> PricedSurface fit itself reuses the exact
// same blessed path `build_corpus` runs (src/corpus_board_fit.{hpp,cpp});
// this header/impl adds only the per-symbol config resolution, date grouping,
// uid stamping, and SurfaceDb partition writes on top.

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"     // CorpusBoard
#include "atx/vol/run_report.hpp" // MetaKv
#include "atx/vol/surface_db.hpp" // SurfaceDb, SymbolFitConfig
#include "atx/vol/types.hpp"      // Result, Status

namespace atx::vol {

struct SurfaceDbPopulateConfig {
  // Base fit inputs (preset etc). Per-symbol SymbolFitConfig from the db
  // manifest is overlaid via apply_symbol_config; a symbol absent from the
  // manifest uses `fallback` unchanged.
  SymbolFitConfig fallback{};
  unsigned n_threads{0};    // 0 = serial; determinism must hold regardless
  bool skip_existing{true}; // date key already in db.partitions() -> skip whole date
  // C4 wave-2 (perf, finding 13): cap the outer worker budget at the physical
  // P-cores and pin outer workers to them (the compute-bound fit path regresses
  // past the P-core count on a hybrid P/E host). Default ON. Set false to measure
  // the unpinned/uncapped scaling curve or on hosts where the pin is unwanted;
  // results are byte-identical either way (pinning only steers scheduling).
  bool pin_outer_workers{true};
};

struct PopulateSymbolStats {
  std::string symbol;
  std::uint32_t n_attempted{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_disabled{0}; // skipped because manifest enabled=false
  // Mean fit-quality score over successful fits, when the shared corpus fit
  // path yields one (oos_in_band from curve selection; see corpus.cpp's
  // CorpusEntry.oos_in_band recording). NaN when unavailable (e.g. the
  // pinned-curve path has no OOS score — mirrors corpus.cpp).
  double mean_oos_in_band{std::numeric_limits<double>::quiet_NaN()};
};

// One (date, symbol) cell whose fit FAILED, carrying the reason the fit itself
// produced. `n_failed` answers HOW MANY cells died; this answers WHICH and WHY.
//
// `detail` is the fit `Error`'s own message, verbatim — for the risk pipeline
// that is PricerFitter's formatted rejection string naming the failing gate, the
// offending slice, the log-moneyness and the slack. It is deliberately NOT a
// re-derivation: an operator must be able to root-cause a lost cell from the run
// report alone, which is exactly what a bare `cells_failed` count could not do.
//
// Purely diagnostic. Recording a failure does NOT make it sticky: there is no
// persisted known-failed state (see `surface_db_build.hpp`'s ruling), so a cell
// listed here is retried on the next run exactly as it was before.
struct FailedCell {
  std::string date;   // partition key (the board's date)
  std::string symbol; // the board's symbol
  // The fit `Error`, split into its two halves; `detail` is "" when the Error
  // carried no message.
  ErrorCode code{ErrorCode::Unknown};
  std::string detail;
};

struct SurfaceDbPopulateStats {
  std::uint32_t n_boards{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_dates_written{0};
  std::uint32_t n_dates_skipped_existing{0};
  std::vector<PopulateSymbolStats> per_symbol; // sorted by symbol
  // One entry per cell counted in `n_failed`, ascending by (date, symbol).
  //
  // DETERMINISM (a repo invariant: identical output for any thread count):
  // entries are appended by the SINGLE drain thread as it walks dates in
  // ascending order and, within a date, boards in ascending (date, symbol) sort
  // order — never by a fit worker. Completion order therefore cannot reach this
  // list, and it is byte-identical for any `SurfaceDbPopulateConfig::n_threads`.
  std::vector<FailedCell> failed_cells;
};

// Deterministic test seam for the streaming/per-date-release path. Production
// callers pass nullptr (the default). Both callbacks default-empty; when set
// they MUST be thread-safe: `before_board_fit` runs on a fit worker thread
// immediately before each board's fit begins, `after_partition_write` runs on
// the draining thread immediately after a date's partition is written. The sole
// use is the streaming test, which blocks a later date's board until an earlier
// date's partition lands to prove writes are streamed (not deferred to a single
// global join) — see SurfaceDbPopulate.StreamsPartitionsBeforeGlobalJoin.
struct PopulateTestHooks {
  std::function<void(const std::string &date, const std::string &symbol)> before_board_fit{};
  std::function<void(const std::string &date)> after_partition_write{};
  // U4 (R-14): observe the per-board inner fit-worker budget resolved from the
  // shared pool (budget / min(budget, n_boards); 0 = auto sizing, the
  // outer-serial mode). Called once on the caller thread before the fit
  // fan-out, so the test needs no synchronization. The sole use is the
  // shared-worker-budget test asserting a small book splits the pool across
  // boards instead of pinning each board to one worker.
  std::function<void(unsigned inner_fit_workers)> on_inner_fit_workers{};
};

// Fit every board and store one partition per distinct board date (key =
// date). Eligible boards share one bounded dynamic queue across all dates; the
// fits stream: each date's partition is aggregated, written, and RELEASED in
// ascending date order as soon as that date's fits complete, while later dates
// are still being fit — so peak RSS is O(dates in flight), not O(all dates).
// Results (surfaces + stats) are byte-identical to a launch-then-join-then-write
// populate: every board's fit is independent/deterministic and the drain visits
// dates and boards in the same deterministic date/symbol order. A board whose
// symbol's manifest config has
// enabled=false is skipped (n_disabled). A board whose fit fails records
// n_failed and does NOT abort the date (document per-name failures, don't
// silently drop). A date with zero successful fits writes NO partition.
// Partition write uses SurfaceArchiveItem{symbol, &surface} with owning
// symbol-string storage kept alive across the call.
// Top-level Err only on: empty boards span, db write errors, or a date key
// the db rejects.
[[nodiscard]] Result<SurfaceDbPopulateStats>
populate_surface_db(SurfaceDb &db, std::span<const CorpusBoard> boards,
                    const SurfaceDbPopulateConfig &cfg = {},
                    const PopulateTestHooks *test_hooks = nullptr);

// ── F-c: universe-scale, cell-aware resumable populate ──────────────────────
//
// The universe populate driver's testable core. Wraps populate_surface_db with
// (1) per-symbol manifest-config seeding (an index leg pinned to the dense recipe,
// every other symbol left on the preset's auto-selector) and (2) CELL-AWARE
// idempotent resume: a partition (= date) is (re)written only when a loaded board
// adds a symbol the partition does not already carry, so re-running as the OPRA
// pull dribbles in new (symbol,date) cells fits only the new work and a re-run over
// unchanged data fits ZERO once every loaded cell has either fitted successfully or
// been config-disabled (a disabled cell is excluded from the pending tally; a cell
// that FAILS to fit is not, so it is retried — see build_surface_db's contract).
// Uses the fused streaming populate_surface_db underneath
// (per-date fit->serialize->release on the executor pool), so RSS stays
// O(dates in flight). The caller loads the hive (load_opra_daterange ->
// corpus_board_from_opra) and hands the available boards in; this function owns the
// config seeding, the resume filtering, the populate call, and the coverage report.
//
// Determinism: byte-identical across fit_workers (populate_surface_db invariant);
// the resume filter is a deterministic date/symbol grouping.
//
// SAFETY: a date is skipped (never rewritten) if its partition already holds a
// symbol NOT present in this run's loaded set — a whole-partition rewrite would
// drop it. This cannot happen on the intended workflow (the pull only grows, the
// full run has a fixed symbol set) but guards a narrower-symbol re-run from data loss.
struct UniversePopulateSpec {
  std::string index_symbol{};        // pinned to the dense index recipe; empty = none
  FitPreset preset{FitPreset::Fast}; // non-index symbols use this preset's auto-selector
  unsigned fit_workers{0};           // 0 = auto (honors ATX_VOL_FIT_WORKERS); 1 = serial
};

struct UniversePopulateCoverage {
  std::uint32_t cells_loaded{0};             // boards handed in (available parquet cells)
  // NEW (symbol,date) cells scheduled this run. A cell whose resolved config is
  // DISABLED is never counted: it can never be added to a partition, so treating
  // it as pending work would keep its date in the rewrite set forever.
  std::uint32_t cells_to_fit{0};
  std::uint32_t cells_refit{0};              // already-present cells re-fit by a same-date rewrite
  std::uint32_t cells_already_present{0};    // skipped: symbol already in its date partition
  std::uint32_t cells_ok{0};                 // populate n_ok over the (re)written dates
  std::uint32_t cells_failed{0};             // populate n_failed over the (re)written dates
  std::uint32_t dates_total{0};              // distinct dates among the loaded boards
  std::uint32_t dates_written{0};            // dates that needed a (re)write this run
  // Dates with NOTHING left to add: every loaded cell is either already in the
  // partition or config-DISABLED (a disabled cell can never be added, so a date
  // whose only gap is disabled symbols is complete, not pending).
  std::uint32_t dates_skipped_complete{0};
  std::uint32_t dates_skipped_would_drop{0}; // dates skipped to avoid dropping an existing symbol
  std::vector<PopulateSymbolStats> per_symbol; // from the underlying populate (written dates only)
  // WHY each of `cells_failed` failed, ascending by (date, symbol) — the fit
  // stage's answer to `AutoConfigReport::failed_symbols`, which names the symbols
  // the CONFIG stage is not serving (as of FIX-C-2 that is the STANDING disabled
  // set — this run's refusals plus the ones already stored disabled — not just
  // this run's). Carried straight through from the underlying populate (same
  // determinism guarantee), so `failed_cells.size() == cells_failed` over the
  // dates this run wrote.
  std::vector<FailedCell> failed_cells;
};
// NOTE: the cell counters do NOT reconcile against `cells_loaded`. A DISABLED cell
// that is absent from its partition on a skipped-complete date is in none of
// `cells_to_fit` / `cells_refit` / `cells_already_present`, and
// `PopulateSymbolStats::n_disabled` only covers the dates this run WROTE — so
// `cells_loaded` is the input count, not the sum of a partition. Treat it as
// such: there is deliberately no disabled-cell counter here.

// Seed per-symbol configs (idempotent) then cell-aware-resume-populate the given
// boards into `db`. Empty `boards` is a graceful no-op (all-zero coverage), NOT an
// error — an un-pulled window legitimately yields no boards. Top-level Err only on a
// db config/write failure.
[[nodiscard]] Result<UniversePopulateCoverage>
populate_universe_streaming(SurfaceDb &db, std::span<const CorpusBoard> boards,
                            const UniversePopulateSpec &spec,
                            const PopulateTestHooks *test_hooks = nullptr);

// Stats file for the report: meta (caller's, plus n_boards/n_ok/n_failed/
// n_dates_written appended), header
// "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band",
// one row per symbol (success_rate = n_ok / max(1, n_attempted - n_disabled),
// %.10g; mean_oos_in_band prints "nan" when NaN).
[[nodiscard]] Status write_populate_stats_csv(const SurfaceDbPopulateStats &s, const MetaKv &meta,
                                              std::string_view path);

} // namespace atx::vol
