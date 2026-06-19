#pragma once

// atx::engine::factory — CachedScore: the per-canon_hash fitness-cache VALUE.
//
// Extracted into its own tiny header (from search_driver.hpp) so the resumable-
// discover serialization layer (search_progress.hpp) can serialize/restore the
// fitness_cache WITHOUT a circular include against search_driver.hpp (which itself
// includes search_progress.hpp). search_driver.hpp re-includes this header so the
// public type is unchanged; CachedScore stays a trivial aggregate.

#include <array>
#include <vector>

#include "atx/core/types.hpp"             // atx::f64, atx::u8
#include "atx/engine/factory/fitness.hpp" // factory::kMaxObjectives

namespace atx::engine::factory {

// =========================================================================
//  CachedScore — the per-canon_hash fitness cache value (F6 throughput, S4.1).
//
//  Pre-S4 this cache held only the scalar `raw`. S4.1 also caches the multi-
//  objective vector so a dedup-hit reuses the SAME objectives (not just raw)
//  without a re-eval — the MultiObjective ranking is then identical for an
//  equivalent structure however many times it recurs. Trivial aggregate.
// =========================================================================
struct CachedScore {
  atx::f64 raw{0.0};
  std::array<atx::f64, kMaxObjectives> objectives{};
  atx::u8 n_objectives{0};
  // S4.2: the candidate's behavioral descriptor (OOS PnL profile). Pure function of
  // (genome, panel) -> canon-cacheable: a dedup-hit reuses this WITHOUT a re-eval.
  // The behavioral NOVELTY computed from it is population-relative and is recomputed
  // fresh each generation (NOT cached). Empty if the candidate's fitness errored.
  std::vector<atx::f64> descriptor{};
};

} // namespace atx::engine::factory
