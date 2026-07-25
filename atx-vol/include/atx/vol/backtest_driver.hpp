#pragma once

// atx-vol backtest driver spine — the one stage sequence every example driver
// shares: TIME the engine call, FOLD the result into a `TearSheet`, CAPTURE the
// run's `EngineRunStats`. Nothing else. The drivers keep their own arg parsing,
// clock construction, strategy authoring, `RunConfig` overlay, output writers,
// console summaries and exit codes — those are 0/5 to 2/5 shared and four
// mutually-incompatible output shapes, so there is no spine there to extract.
//
// ## Contract (the byte-stability guarantee the migrated drivers depend on)
//
//   * `RunOutcome::result` is the engine's return value MOVED, with NO
//     transformation of any kind — same rows, same columns, same signals, same
//     bits. Every driver's emitted series is written from it.
//   * The timed interval brackets ONLY the engine call, matching what the four
//     timing drivers measure today (`mag7:201-203`, `spy_dispersion_pnl:459-461`,
//     `spy_strangle_backtest:457-459`, `dispersion_backtest:155-159`). It does
//     NOT include the tearsheet fold — widening it would silently change the
//     `wall_clock_ms` (and derived `steps_per_s`) semantics of
//     `engine_metrics.csv` and of the drivers' meta headers.
//   * `stats.n_steps == result.size()`; `stats.cache` is
//     `cfg.snapshot_cache->stats()` when a cache was supplied and a zeroed
//     `SnapshotCacheStats{}` otherwise (mirroring
//     `spy_strangle_backtest.cpp:468-469`'s ternary — two of the five drivers run
//     with no shared cache, so the null path is a supported, non-crashing route).
//   * On engine `Err` the error propagates VERBATIM: each driver keeps its own
//     `fprintf` text and exit code, so no console output moves.
//
// `EngineRunStats` deliberately stays in `run_report.hpp` (which is where its
// `engine_metrics()` emitter lives); this header includes it rather than
// relocating it.

#include "atx/vol/backtest.hpp"            // Clock, IStrategy, RunConfig, BacktestResult
#include "atx/vol/dispersion.hpp"          // DispersionUniverse
#include "atx/vol/dispersion_backtest.hpp" // DispersionBacktestConfig, run_dispersion_backtest
#include "atx/vol/run_report.hpp"          // EngineRunStats (NOT relocated — included)
#include "atx/vol/tearsheet.hpp"           // TearSheet, tearsheet
#include "atx/vol/types.hpp"               // Result

namespace atx::vol {

// One timed engine run: the untouched engine output, its tearsheet, its stats.
struct RunOutcome {
  BacktestResult result; // exactly what the engine returned — never post-processed
  TearSheet sheet;       // tearsheet(result)
  EngineRunStats stats;  // wall_clock_ms over the engine call ONLY
};

// Engine slot A — the `IStrategy` engine (mag7_dispersion_backtest,
// spy_dispersion_pnl, spy_strangle_backtest, strategy_examples).
[[nodiscard]] Result<RunOutcome> run_timed(const Clock &clock, IStrategy &strat,
                                           const RunConfig &cfg = {});

// Engine slot B — the composed surface-only dispersion driver
// (examples/dispersion_backtest; a future consumer is spy_dispersion_backtest's
// run_surface_backtest_command). Cache stats come from `cfg.run.snapshot_cache`.
[[nodiscard]] Result<RunOutcome> run_timed(const Clock &clock, DispersionUniverse universe,
                                           const DispersionBacktestConfig &cfg = {});

} // namespace atx::vol
