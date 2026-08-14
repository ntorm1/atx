#pragma once

// Corpus build internals split out of the public corpus.hpp API surface (Task 6,
// atx-vol API restructure): CorpusFitTestHooks is used only by corpus.cpp (the
// T1 inner-worker reclaim's observation seam) and by corpus_test.cpp's direct
// test-hook installation, never by any other production TU.

#include <cstddef>
#include <functional>

namespace atx::vol::detail {

// ── Test-only observation seam for the T1 inner-worker reclaim ──────────────
//
// The reclaim's whole point is WHEN a board's inner fit-worker budget is
// resolved, and the process-global `CorpusPhaseTimings` counters are sums — they
// cannot tell "one board offered 4 once" from "one board offered 1 four times".
// A gate for the drain regime has to see the individual offers, in order, with
// the live unfinished-task count each was resolved against.
//
// Same shape and same contract as `PopulateTestHooks`
// (surface_db_populate.hpp): a raw, defaulted-null pointer on the config, never
// dereferenced unless a test installs it, and never consulted on any path that
// can reach an output byte. Production leaves it null.
struct CorpusFitTestHooks {
  // Once per INNER FAN-OUT of `boards[board_index]` on the across-board parallel
  // arm — i.e. every time that board's inner fit-worker budget is resolved, not
  // once per board. `unfinished` is the live outer unfinished-task count the
  // budget was resolved against; `workers` is the budget offered.
  //
  // Called ON THE BOARD'S OWN FIT THREAD, so a test may block here to hold a
  // board in flight while its siblings drain.
  std::function<void(std::size_t board_index, std::size_t unfinished, unsigned workers)>
      on_inner_fit_workers{};
  // After `boards[board_index]`'s fit has completed and the outer unfinished
  // counter has been decremented; `unfinished` is the post-decrement value, so
  // `unfinished == 1` means this board's completion left exactly one task
  // standing. Lets a test wait for a deterministic drain state instead of
  // sleeping.
  std::function<void(std::size_t board_index, std::size_t unfinished)> on_board_fit_done{};
};

} // namespace atx::vol::detail
