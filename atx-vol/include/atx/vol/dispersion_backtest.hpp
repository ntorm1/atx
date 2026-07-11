#pragma once

// Reusable surface-only dispersion backtest orchestration. The example CLI is
// intentionally limited to parsing files and writing artifacts; strategy,
// lifecycle, hedge, and engine defaults live here for library callers.

#include <cstddef>

#include "atx/vol/backtest.hpp"
#include "atx/vol/dispersion.hpp"
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
// snapshot by DispersionStrategy.
[[nodiscard]] DispersionStrategy
make_dispersion_backtest_strategy(DispersionUniverse universe,
                                  const DispersionBacktestConfig &config = {});

// Run the canonical strategy over an already-qualified Clock. Callers retain
// control of corpus construction and artifact persistence.
[[nodiscard]] Result<BacktestResult>
run_dispersion_backtest(const Clock &clock, DispersionUniverse universe,
                        const DispersionBacktestConfig &config = {});

} // namespace atx::vol
