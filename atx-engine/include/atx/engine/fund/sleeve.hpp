#pragma once

// atx::engine::fund — Sleeve: a thin owning identity that wraps an S1
// risk::MultiHorizonOptimizer over a SUBSET of the alpha library (P2-S2-1). The FIRST
// file of the new fund/ layer and the structural foundation of the sprint's load-
// bearing R7 "boundary pin".
//
// ===========================================================================
//  What a Sleeve is
// ===========================================================================
//  A Sleeve bundles four things and nothing more:
//    * its MEMBERSHIP — the library AlphaId id-list it owns (Library itself cannot
//      slice, so the sleeve carries the subset explicitly);
//    * its TAGS — free-form universe × family labels (e.g. {"US","momentum"}) the
//      later meta-allocator groups by;
//    * its CAPACITY ceiling — the per-sleeve gross box the meta-allocator honors;
//    * a wrapped S1 MultiHorizonConfig — the proven constrained multi-horizon driver.
//
//  It produces the per-period sleeve book series by PURE DELEGATION to that S1
//  optimizer — it composes the optimizer, it never re-implements it.
//
// ===========================================================================
//  The R7 boundary pin is STRUCTURAL (why run() MUST stay a transparent one-liner)
// ===========================================================================
//  Sleeve::run IS MultiHorizonOptimizer{cfg.mh}.run(...) — byte-for-byte. Because the
//  wrapper adds no arithmetic, a later one-sleeve meta-book that capital-weights this
//  sleeve 1.0 and skips netting/vol-target reduces to MultiHorizonOptimizer.run BY
//  CONSTRUCTION. Keeping run() inline and delegating preserves that pin at the source
//  level — there is no numeric kernel here to drift, so there is no .cpp for this unit.

#include <functional> // std::function (the run() callbacks)
#include <string>     // std::string (tag labels)
#include <vector>     // std::vector (membership id-list)

#include "atx/core/error.hpp" // atx::core::Result
#include "atx/core/types.hpp" // atx::f64, atx::usize

#include "atx/engine/library/library.hpp"   // library::AlphaId (the membership id type)
#include "atx/engine/risk/factor_model.hpp" // risk::FactorModel (the model_at callback return)
#include "atx/engine/risk/multi_horizon.hpp" // risk::MultiHorizonOptimizer/Config/Result, HorizonSources
#include "atx/engine/risk/multi_period.hpp"  // risk::RebalanceSchedule, book::CostInputs

namespace atx::engine::fund {

// ===========================================================================
//  SleeveTag — free-form membership labels the meta-allocator groups sleeves by.
// ===========================================================================
struct SleeveTag {
  std::string universe; // e.g. "US", "EU", "crypto"
  std::string family;   // e.g. "momentum", "reversal", "carry"
};

// ===========================================================================
//  SleeveConfig — the sleeve's full identity (pure configuration).
// ===========================================================================
struct SleeveConfig {
  risk::MultiHorizonConfig mh;            // the wrapped S1 optimizer config (incl SignalHorizon/source)
  std::vector<library::AlphaId> members; // the library SUBSET this sleeve owns (Library cannot slice)
  SleeveTag tag{};                        // universe × family membership labels
  atx::f64 capacity_gross = 1e9;          // per-sleeve capacity ceiling → the meta-allocator's box
};

// ===========================================================================
//  Sleeve — a thin owning wrapper over a MultiHorizonOptimizer (Rule of Zero).
// ===========================================================================
class Sleeve {
public:
  SleeveConfig cfg;

  // PURE DELEGATION — this IS MultiHorizonOptimizer{cfg.mh}.run(...). Compose, never
  // rewrite. The R7 boundary pin is STRUCTURAL: a one-sleeve meta-book that capital-
  // weights this sleeve 1.0 and skips netting/vol-target is byte-identical to S1 by
  // construction, because this wrapper adds no arithmetic. Stays inline to preserve
  // that pin at the source level.
  [[nodiscard]] atx::core::Result<risk::MultiHorizonResult>
  run(const risk::RebalanceSchedule &sched,
      const std::function<risk::HorizonSources(atx::usize)> &sources_at,
      const std::function<const risk::FactorModel &(atx::usize)> &model_at,
      const book::CostInputs &cost) const {
    return risk::MultiHorizonOptimizer{cfg.mh}.run(sched, sources_at, model_at, cost);
  }

  // The size of the membership id-list (the library subset this sleeve owns).
  [[nodiscard]] atx::usize n_members() const noexcept { return cfg.members.size(); }
};

} // namespace atx::engine::fund
