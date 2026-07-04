#pragma once

// atx::engine::cost — CostSelectionConfig (S4-0): the inert-default toggle that
// charges the sqrt-law temp+perm impact cost in the SEARCH SELECTION scalar
// (factory ScalarRaw `raw`), not only the NSGA objective vector (objectives[4]).
//
// ===========================================================================
//  Why this struct exists, and why it is NOT a FitnessCfg field yet
// ===========================================================================
//  S4 fixes the cost/capacity/execution correctness bugs (B1-B6) and wires the
//  impact cost into the SELECTION objective (B7, S4-4) so a high-turnover
//  in-sample winner ranks BELOW a low-turnover one net-of-cost. The toggle that
//  gates this is CostSelectionConfig -- but `factory::FitnessCfg`
//  (factory/fitness.hpp) is Sprint-5-owned (the hub sprint that threads config
//  fields through to the CLI), so S4 must not edit fitness.hpp. This struct
//  therefore lives in the S4-owned cost config surface; S4's own on-path tests
//  construct it directly and call the S4-owned selection-objective function
//  (factory::apply_selection_cost, factory/fitness_cost_selection.hpp) rather
//  than routing through FitnessCfg.
//
//  SEAM (ledger row, sprint-4-progress.md): Sprint 5 adds
//  `cost::CostSelectionConfig cost_selection{};` to FitnessCfg (fitness.hpp,
//  alongside `target_aum`/`cost`) and one call in `detail::finish_report`
//  (fitness.cpp) forwarding `cfg.cost_selection` + a `book_cost_bps` re-priced
//  at `cfg.cost_selection.selection_aum` into `apply_selection_cost`.
//
// ===========================================================================
//  Inert-default contract (byte-identity, opt-in contract A)
// ===========================================================================
//  impact_in_selection = false ⇒ the consuming branch (apply_selection_cost)
//  is never entered ⇒ every ScalarRaw/NSGA golden computed today stays
//  byte-identical. Fields are append-only at struct end so adding one never
//  breaks an existing aggregate-init call site.

#include "atx/core/types.hpp" // atx::f64

namespace atx::engine::cost {

struct CostSelectionConfig {
  // Charge the sqrt-law temp+perm impact in the SEARCH SELECTION scalar
  // (ScalarRaw `raw`), not only the NSGA objective vector. false (default) ⇒
  // selection is byte-identical to today (gross ranking).
  bool impact_in_selection = false;

  // AUM at which the selection cost is priced (the target_aum book_cost_bps
  // is evaluated at, post-S4-1 dimensionally-correct participation). 0 ⇒ off
  // regardless of impact_in_selection (a zero AUM has no meaningful cost).
  atx::f64 selection_aum = 0.0;

  // Multiplier on the net-of-cost penalty when on. 1.0 ⇒ the unscaled
  // combine::kFitnessCostScale convention (the ONE fitness-cost scale, shared
  // with the p6-S4 admission-gate penalty -- see fitness_cost_selection.hpp).
  atx::f64 cost_weight = 1.0;
};

} // namespace atx::engine::cost
