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

#include "atx/vol/track_key.hpp" // kBacktestEconomicsRev

namespace atx::vol {

// dispersion_run.cpp:2937, "surface-only projected backtest complete:
// dates=82 final_nav=...". Compared bit-for-bit, not with a tolerance: the
// tripwire is meant to catch drift down to the last ULP.
//
// Task E3 CORRECTION (backtest-production-lakehouse sprint). The literal that
// shipped here from Task D1 (247.4065016443293) was WRONG -- not because
// economics moved during this sprint, but because it was copied from stale
// sprint/CHANGELOG prose that predates a re-pin event that had ALREADY
// happened, and already been corrected on `main`, before this sprint even
// started. Full chain, reconstructed from git history + a fresh measurement:
//
//   1. `247.4065016443293` is an M2-era (2026-07-21) figure. It was already
//      superseded once, in-repo, by commit 2de65c7 (2026-07-25, "chore(vol):
//      re-pin the 82-session dispersion golden", an ancestor of this sprint's
//      base) -- that commit's own message states outright: "those [doc] sites
//      carried 247.4065016443293, which was already stale by one generation
//      -- the M2 re-pin moved the value to 247.40624124981315 and the prose
//      was never updated."
//   2. The SAME commit then applied WS-E's E1 sizing migration (dispersion
//      book size becomes exactly 100x larger: dollars-per-vol-point instead
//      of dollars-per-100-vol-points, `kVegaPerVolPoint`), moving that already
//      corrected 247.40624124981315 -> 24740.624124981368 -- confirmed an
//      EXACT x100 rescale by 2de65c7's own control experiment (reverting
//      `gross_index_vega` to the pre-E1 value reproduced 247.40624124981315
//      to within 1.8e-16 relative). This is the CHANGELOG.md 1.1.0-predecessor
//      entry "Golden replay pins ... move as a direct consequence".
//   3. Task D1 (this sprint, commit ca74938) introduced this header and typed
//      in `247.4065016443293` -- the doubly-stale, pre-(1)-AND-pre-(2) prose
//      figure -- never having reproduced it against the actual corpus. That
//      is the ~100x-and-then-some discrepancy Task A3 flagged (measured
//      24740.624124996561 on the real 82-session corpus, both before and
//      after A3's own change, bit-for-bit identical) and Task E2 disclosed
//      but left open (docs/backtest-lakehouse.md).
//   4. Task E3 re-measured directly: rebuilt `atxvol_spy_dispersion_backtest`
//      at this exact worktree tip (commit 39419bb + this task's own,
//      non-economics changes), ran `run-surface-backtest` against a SCRATCH
//      COPY of the real corpus (C:/atx-data/spy-dispersion/runs/
//      bt-sota-baseline -- the exact recipe A3 used; the original was never
//      touched), with `emit_tsv_diagnostics=true` for full-precision output.
//      Result, `surface_backtest.tsv`'s last row (`2026-04-30`), `nav` column:
//      `24740.624124996561` -- BIT-FOR-BIT IDENTICAL to A3's independently
//      measured figure, reproduced on a different day, different commit,
//      same worktree lineage. Console line also printed `economics_rev=1`,
//      matching this file's `kBacktestEconomicsRev` pairing below.
//
//   The residual between step (2)'s 24740.624124981368 and step (4)'s
//   24740.624124996561 is 1.52e-8 absolute / ~6.1e-13 relative -- seven
//   orders of magnitude below a one-cent tick, the same "not economically
//   meaningful" class of ordinary pricing-path drift 2de65c7 itself already
//   catalogued as a separate, non-economic phenomenon ("wave-1 pricing/
//   greeks drift ... remains visible on the listed route, re-measured
//   separately"). It is NOT attributable to any change landed during THIS
//   sprint. Every sprint commit that could plausibly touch this corpus's
//   economics is accounted for:
//
//     - A3 (RunConfig.reconcile_nav/book_entry_fill_slippage default flips)
//       proved its OWN change bit-identical before/after on this exact
//       corpus (real data, same CLI, same regime line, byte-for-byte).
//     - E1 (5292cae, `apply_to_financing` -> `FinancingConfig::flat_r`
//       routing fix) is inert here: `bt-sota-baseline/run_spec.tsv` never
//       sets `rate_applies_to_financing` (confirmed by inspection), matching
//       5292cae's own commit message's claim.
//     - A1 (a381df7, swap fixing-cadence guard) and A2 (9fc8e7a, RollAtHorizon
//       no-trade-step close) both landed BEFORE A3's own baseline measurement
//       (commit 76411e0, the "before" tip A3 measured against) -- so A3's
//       bit-identity check never independently covered them, and they need
//       their own argument, not a ride on A3's. Verified by reading the code
//       rather than assuming: A1 touches `strategy.hpp`/`strategy.cpp`/
//       `backtest.cpp`'s swap-fixing accrual loop, which only executes for a
//       lot carrying a populated `StrategySpec::swap_legs` -- and
//       `swap_legs` is populated and consumed nowhere but `strategy.cpp`'s
//       generic `DeclarativeStrategy`/`StrategySpec` interpreter (grep:
//       `swap_legs` appears in exactly one .cpp in the whole tree). A2
//       touches `strategy.cpp`'s `LifecycleSpec::Holding::RollAtHorizon`
//       branch, that same generic interpreter. The golden 82-session replay
//       does not go through either: `dispersion_run.cpp` (the surface-only
//       projected backtest's own orchestrator, line ~1114) constructs a
//       `DispersionStrategy` directly (`dispersion_strategy.cpp`) -- a
//       bespoke `IStrategy` built straight on `build_dispersion_book`, which
//       never references `StrategySpec`, `LifecycleSpec::Holding`, or
//       `swap_legs` at all (its one incidental mention of the string
//       "RollAtHorizon" is a doc comment about an unrelated synthetic-expiry
//       invariant, not a call into A2's code). A1's and A2's touched surface
//       is therefore structurally unreachable from this corpus's code path,
//       independent of whatever A3's bit-identity check did or didn't cover.
//
//   CONCLUSION: this is a literal-transcription correction (D1 typed in the
//   wrong, already-superseded number), NOT a sprint economics change --
//   `kBacktestEconomicsRev`/`kGolden82SessionEconomicsRev` stay at 1
//   unbumped. Bumping the rev here would be actively wrong: it is folded into
//   `make_engine_id()` -> `TrackKey`, so bumping it invalidates every cached
//   BacktestDb/TrackStore/lakehouse entry system-wide for a change that never
//   touched economics semantics -- see CHANGELOG.md's own precedent for the
//   5292cae fix ("No kBacktestEconomicsRev bump was needed for this fix, and
//   the reasoning is structural, not usage-odds"). See CHANGELOG.md's E3
//   entry for the migration note this correction still owes a reader (per
//   the sprint's "golden pin changes only via economics-rev bump + CHANGELOG"
//   discipline -- honored here via CHANGELOG, deliberately not via a rev bump
//   that would misrepresent what actually happened).
inline constexpr double kGolden82SessionFinalNav = 24740.624124996561;

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
