#pragma once

// Reusable surface-only dispersion backtest orchestration. The example CLI is
// intentionally limited to parsing files and writing artifacts; strategy,
// lifecycle, hedge, and engine defaults live here for library callers.

#include <cstddef>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_workflow.hpp" // UniverseRow (point-in-time schedule)
#include "atx/vol/strategy.hpp"

namespace atx::vol {

struct DispersionBacktestConfig {
  double target_dte_days{30.0};
  double roll_dte_days{7.0};
  double gross_index_vega{10'000.0};
  double delta_band{0.0};
  std::size_t min_names{2};
  unsigned entry_every_n{21};
  bool project_to_calendar_expiry{true};
  bool record_diagnostics{false};
  RunConfig run{};
};

// Construct the canonical surface-only dispersion strategy used by research
// and production backtests. The authored universe is rebound by symbol on each
// snapshot by DispersionStrategy. This overload FREEZES the passed membership for
// the whole run (no point-in-time reconstitution); prefer the schedule overload
// below for a multi-block universe.
[[nodiscard]] DispersionStrategy
make_dispersion_backtest_strategy(DispersionUniverse universe,
                                  const DispersionBacktestConfig &config = {});

// C1 point-in-time overload. The strategy re-resolves its basket from `schedule`
// on every step (via `make_pit_universe_resolver`), so a mid-window
// reconstitution — including a name being REMOVED (C3) — is honored at the next
// roll instead of freezing day-1 membership. `index_symbol` (default "SPY") is
// the never-a-constituent index leg. With a single-block schedule this is
// behaviourally identical to the frozen overload.
[[nodiscard]] DispersionStrategy
make_dispersion_backtest_strategy(std::vector<UniverseRow> schedule,
                                  const DispersionBacktestConfig &config = {},
                                  std::string_view index_symbol = "SPY");

// Run the canonical strategy over an already-qualified Clock. Callers retain
// control of corpus construction and artifact persistence.
[[nodiscard]] Result<BacktestResult>
run_dispersion_backtest(const Clock &clock, DispersionUniverse universe,
                        const DispersionBacktestConfig &config = {});

// C1 point-in-time overload: same as above but re-resolves the basket per step
// from `schedule`. This is the flagship surface-path entry that honors mid-window
// reconstitution/removals.
[[nodiscard]] Result<BacktestResult>
run_dispersion_backtest(const Clock &clock, std::vector<UniverseRow> schedule,
                        const DispersionBacktestConfig &config = {},
                        std::string_view index_symbol = "SPY");

} // namespace atx::vol
