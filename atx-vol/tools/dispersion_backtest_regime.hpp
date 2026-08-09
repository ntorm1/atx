#pragma once

// dispersion_backtest_regime.hpp — E1 fix round (backtest-lakehouse sprint,
// task review). B1's `FrictionRegime` (the engine-classified execution
// assumption a `BacktestResult` carries -- see backtest.hpp) has no
// stringifier anywhere in the library. `tools/spy_dispersion_backtest.cpp`'s
// `run-backtest` / `run-projected-backtest` subcommands need one to name the
// regime + D1's `kBacktestEconomicsRev` in their RunArchive `meta` section and
// console summary, matching the convention `dispersion_run.cpp` already
// established for its own (differently-shaped) `DispersionFrictionRegime`.
//
// Pulled into its own header -- rather than staying an anonymous-namespace
// helper inside spy_dispersion_backtest.cpp -- purely so a gtest can reach it
// without spawning the (ATX_BUILD_TOOLS-gated) binary; mirrors the seam
// `tools/surface_db_build_cli.hpp` already established for the same reason
// (see tests/dispersion_backtest_regime_test.cpp).

#include <string_view>

#include "atx/vol/backtest.hpp" // FrictionRegime

namespace atx::vol {

[[nodiscard]] inline std::string_view friction_regime_text(FrictionRegime regime) noexcept {
  switch (regime) {
  case FrictionRegime::Frictionless:
    return "frictionless";
  case FrictionRegime::Modeled:
    return "modeled";
  case FrictionRegime::QuoteSide:
    return "quote_side";
  }
  return "unknown";
}

} // namespace atx::vol
