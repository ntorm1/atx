#pragma once

// Golden 82-session SPY economics tripwire (Task D1, Step 5). One place both
// the (currently corpus-gated -- see tests/track_key_test.cpp) golden-replay
// test and a future CI job read: the pinned NAV this exact
// `kBacktestEconomicsRev` must reproduce.
//
// Provenance: `src/dispersion_run.cpp`'s "surface-only projected backtest
// complete" log line (dispersion_run.cpp:2937), 82-session SPY corpus,
// default run_spec (frictionless, no risk limits, multiplier 100 -- the
// golden's own value). Before this task the literal existed only in sprint /
// CHANGELOG prose (e.g. 2026-07-19-atx-vol-fit-backtest-sota-sprint.md) --
// nowhere as an executable assertion. This header plus
// TrackKeyGoldenReplay.* in track_key_test.cpp are the first one.
//
// MECHANICAL ENFORCEMENT, two halves:
//
//   1. COMPILE TIME (this header): kGolden82SessionEconomicsRev must equal
//      kBacktestEconomicsRev. Bump the rev for an economics change and this
//      header stops compiling until the golden literal below is updated
//      alongside it -- the pairing cannot be forgotten silently.
//
//   2. RUN TIME (track_key_test.cpp, corpus-gated): replays the pinned 82
//      sessions and compares final_nav against the literal below bit-for-bit.
//      Skips cleanly with a named reason when the corpus is absent, which is
//      the case in every worktree today. Wiring an actual corpus + run_spec
//      into CI, and completing the replay call, is Task D6's "golden replay +
//      economics tripwire (D1)" gate (sprints/2026-08-05-backtest-production-
//      lakehouse-sprint.md, Task D6 Step 2) -- this header and the test's
//      skip-cleanly contract are the mechanical piece D1 owns.

#include "atx/vol/research/track_key.hpp" // kBacktestEconomicsRev

namespace atx::vol {

// dispersion_run.cpp:2937, "surface-only projected backtest complete:
// dates=82 final_nav=...". Compared bit-for-bit, not with a tolerance: the
// tripwire is meant to catch drift down to the last ULP.
inline constexpr double kGolden82SessionFinalNav = 247.4065016443293;

// The kBacktestEconomicsRev this literal was pinned at. See the compile-time
// check below -- this is what turns "NAV changed without a rev bump" into a
// mechanical, two-layer gate instead of a hand-remembered convention.
inline constexpr int kGolden82SessionEconomicsRev = 1;

static_assert(
    kGolden82SessionEconomicsRev == kBacktestEconomicsRev,
    "kBacktestEconomicsRev moved without updating the golden 82-session NAV "
    "pin (kGolden82SessionFinalNav) alongside it. Re-measure the golden "
    "replay, update BOTH constants in golden_pin.hpp together, and record "
    "the before/after NAV in the sprint's results doc.");

} // namespace atx::vol
