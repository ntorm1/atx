#include "atx/vol/backtest_driver.hpp"

#include <chrono>
#include <memory>
#include <utility>

#include "atx/core/error.hpp" // ATX_TRY

namespace atx::vol {

namespace {

using Clk = std::chrono::steady_clock;

[[nodiscard]] double elapsed_ms(Clk::time_point t0, Clk::time_point t1) noexcept {
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// The stage-7 capture, shared by both overloads. A null cache yields a zeroed
// SnapshotCacheStats (the drivers that run without a shared cache), never a
// dereference of nullptr.
[[nodiscard]] EngineRunStats capture_stats(double wall_ms, const BacktestResult &r,
                                          const std::shared_ptr<SnapshotCache> &cache) {
  EngineRunStats stats;
  stats.wall_clock_ms = wall_ms;
  stats.n_steps = r.size();
  stats.cache = cache ? cache->stats() : SnapshotCacheStats{};
  return stats;
}

} // namespace

Result<RunOutcome> run_timed(const Clock &clock, IStrategy &strat, const RunConfig &cfg) {
  const Clk::time_point t0 = Clk::now();
  ATX_TRY(BacktestResult result, run_backtest(clock, strat, cfg));
  const Clk::time_point t1 = Clk::now(); // engine only — the fold is NOT timed
  const EngineRunStats stats = capture_stats(elapsed_ms(t0, t1), result, cfg.snapshot_cache);
  // Fold BEFORE the move: `result` must still be intact when `tearsheet` reads it.
  const TearSheet sheet = tearsheet(result);
  return RunOutcome{std::move(result), sheet, stats};
}

Result<RunOutcome> run_timed(const Clock &clock, DispersionUniverse universe,
                             const DispersionBacktestConfig &cfg) {
  const Clk::time_point t0 = Clk::now();
  ATX_TRY(BacktestResult result, run_dispersion_backtest(clock, std::move(universe), cfg));
  const Clk::time_point t1 = Clk::now(); // engine only — the fold is NOT timed
  const EngineRunStats stats = capture_stats(elapsed_ms(t0, t1), result, cfg.run.snapshot_cache);
  const TearSheet sheet = tearsheet(result);
  return RunOutcome{std::move(result), sheet, stats};
}

} // namespace atx::vol
