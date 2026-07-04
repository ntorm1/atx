#pragma once

// atx::engine::factory — fitness_cost_selection (S4-4 [B7]): charge the
// sqrt-law temp+perm impact cost in the SEARCH SELECTION scalar (ScalarRaw
// `raw`), not only the NSGA objective vector (`objectives[4]`, S4.3/pre-S4).
//
// ===========================================================================
//  Why this is a STANDALONE function, not a `finish_report` call site
// ===========================================================================
//  The natural wire is a call inside `detail::finish_report` (fitness.cpp)
//  right after `raw` is assembled, reading a `CostSelectionConfig` field off
//  `FitnessCfg`. But `FitnessCfg` (factory/fitness.hpp) is SPRINT-5-OWNED (the
//  hub sprint that threads config fields through to the CLI) — S4 must not
//  edit fitness.hpp, so `finish_report`'s SIGNATURE cannot gain a
//  `CostSelectionConfig` parameter this sprint. This header therefore ships
//  the pure, testable half of the fix — `apply_selection_cost` — as a
//  free function S4's own tests call directly on hand-built (raw, cost_bps)
//  pairs. It is `#include`-only from `fitness.cpp` (via a NEW, S4-owned
//  header) so no forbidden file changes shape or signature.
//
//  SEAM (binding, for Sprint 5 — recorded in sprint-4-progress.md): add
//  `cost::CostSelectionConfig cost_selection{};` to `FitnessCfg` (alongside
//  `target_aum`/`cost`) and ONE call site in `detail::finish_report`,
//  immediately after `raw` is assembled:
//
//    if (cfg.cost_selection.impact_in_selection) {
//      const atx::f64 sel_cost_bps =
//          book_cost_bps(strm, panel, cfg.cost, cfg.cost_selection.selection_aum);
//      raw = apply_selection_cost(raw, sel_cost_bps, cfg.cost_selection);
//    }
//
//  (finish_report does not currently receive `strm`/`panel` either — that
//  plumbing is also part of the S5 wire, since `fitness_core`'s caller already
//  has both in scope.) Until landed, S4-4 is exercised ONLY via direct calls
//  to `apply_selection_cost` — see `fitness_cost_selection_test.cpp`.
//
// ===========================================================================
//  The formula — ONE fitness-cost scale, shared with the p6-S4 admission gate
// ===========================================================================
//  `combine::cost_adjusted_fitness(raw, turnover, rt_cost_bps)` already defines
//  the house conversion from a round-trip bps figure to a raw-fitness-unit
//  penalty (`combine::kFitnessCostScale = 0.1`), used today by the p6-S4
//  admission gate (`combine/gate.hpp`) and the library verdict floor
//  (`library/library.hpp`). S4-4 reuses it verbatim with `turnover` pinned to
//  1.0 — `cost_bps_at_selection_aum` is already a BOOK-LEVEL aggregate (from
//  `book_cost_bps`, post-S4-1 dimensionally correct), not a per-unit-turnover
//  rate, so no second turnover multiplier belongs here; `cfg.cost_weight`
//  scales the penalty before it enters the shared formula. S4 does NOT invent
//  a second cost-to-fitness scale.
//
// ===========================================================================
//  Inert-default contract (opt-in, byte-identity contract A)
// ===========================================================================
//  `cfg.impact_in_selection == false` OR `cfg.selection_aum <= 0.0` ⇒ `raw` is
//  returned UNCHANGED (byte-identical) — mirrors `CostSelectionConfig`'s own
//  documented "0 ⇒ off regardless of impact_in_selection" contract. Pure
//  function of its three inputs: no allocation, no state, no RNG — trivially
//  reproducible across repeated calls and concurrent callers.

#include "atx/core/types.hpp" // atx::f64

#include "atx/engine/combine/cost_util.hpp"       // combine::cost_adjusted_fitness
#include "atx/engine/cost/cost_selection_config.hpp" // cost::CostSelectionConfig

namespace atx::engine::factory {

// Net-of-cost selection scalar. `raw` is the gross ScalarRaw fitness
// (`wq * diversify * robust`, pre-cost); `cost_bps_at_selection_aum` is the
// book round-trip cost (bps) already priced at `cfg.selection_aum` (the
// caller's job — typically `book_cost_bps(strm, panel, cost, cfg.selection_aum)`).
// Returns `raw` unchanged when the config is off; otherwise
// `combine::cost_adjusted_fitness(raw, 1.0, cfg.cost_weight * cost_bps_at_selection_aum)`.
[[nodiscard]] inline atx::f64
apply_selection_cost(atx::f64 raw, atx::f64 cost_bps_at_selection_aum,
                     const cost::CostSelectionConfig &cfg) noexcept {
  if (!cfg.impact_in_selection || cfg.selection_aum <= 0.0) {
    return raw; // inert default -> byte-identical (contract A)
  }
  return combine::cost_adjusted_fitness(raw, /*turnover=*/1.0,
                                        cfg.cost_weight * cost_bps_at_selection_aum);
}

} // namespace atx::engine::factory
