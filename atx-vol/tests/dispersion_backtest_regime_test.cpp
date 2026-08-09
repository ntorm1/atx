// dispersion_backtest_regime_test.cpp — E1 fix round (backtest-lakehouse
// sprint, task review, Finding 1).
//
// Covers `tools/dispersion_backtest_regime.hpp`'s `friction_regime_text`, the
// stringifier `tools/spy_dispersion_backtest.cpp`'s `run-backtest` /
// `run-projected-backtest` subcommands now use to name B1's `FrictionRegime`
// in the RunArchive `meta` section + console summary of their emitted
// artifacts (previously neither carried `friction_regime` nor
// `economics_rev` at all -- review finding).
//
// This is the unit-testable half of that fix: `run_backtest_command` /
// `run_projected_backtest_command` are file-local functions with no header
// declaration (only reachable via the ATX_BUILD_TOOLS-gated binary), so this
// pins the one piece of NEW logic those call sites added -- the enum->string
// mapping -- directly, the same way `dispersion_realism_flags_test.cpp` pins
// the examples/ CLIs' identical mapping.

#include <gtest/gtest.h>

#include "atx/vol/backtest.hpp"
#include "dispersion_backtest_regime.hpp"

using namespace atx::vol;

TEST(DispersionBacktestRegime, TextCoversEveryFrictionRegimeEnumerator) {
  EXPECT_EQ(friction_regime_text(FrictionRegime::Frictionless), "frictionless");
  EXPECT_EQ(friction_regime_text(FrictionRegime::Modeled), "modeled");
  EXPECT_EQ(friction_regime_text(FrictionRegime::QuoteSide), "quote_side");
}
