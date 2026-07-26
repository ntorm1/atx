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
  // LIFECYCLE IS A PARAMETER, not a constant. `make_dispersion_backtest_strategy`
  // used to hardcode EveryNDays/RollAtHorizon, which made this "reusable
  // orchestration" able to express exactly ONE of the shapes the engine supports:
  // a single book rolled at the horizon. The overlapping-clip shape — a fresh
  // cohort entered every step and held to its own expiry — was reachable only by
  // hand-rolling a StrategySpec around DeclarativeStrategy, so the dispersion
  // sizing in `build_dispersion_book` could not be used with it at all.
  //
  // Both fields default to the previous hardcoded values, so an existing caller
  // that does not set them is byte-for-byte unchanged.
  //
  // The two shapes differ in what they stress, which is why both are benchmarked:
  // RollAtHorizon holds one clip (~2*(n_names+1) lots) no matter how long the run
  // is, while HoldToExpiry accumulates one clip per entry until the oldest expires,
  // so the priced book grows to roughly (tenor in steps) * clip size and the
  // per-step pricing cost grows with it.
  LifecycleSpec::Entry entry{LifecycleSpec::Entry::EveryNDays};
  LifecycleSpec::Holding holding{LifecycleSpec::Holding::RollAtHorizon};
  RunConfig run{};
};

// The overlapping-clip ("ladder") shape: enter a fresh dispersion clip EVERY step
// and hold each to its own expiry, so the book ladders up to one live cohort per
// step of the tenor. `target_dte_days` is the clip tenor (90 => a 3-month ladder).
//
// `roll_dte_days` is deliberately not a parameter: under HoldToExpiry nothing is
// closed at a horizon, so `LifecycleSpec::roll_at_T` is unread, and accepting it
// would imply a control this shape does not have.
[[nodiscard]] DispersionBacktestConfig
make_dispersion_ladder_config(double target_dte_days, double gross_index_vega,
                              std::size_t min_names);

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
