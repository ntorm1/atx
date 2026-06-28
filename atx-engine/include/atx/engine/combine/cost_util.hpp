#pragma once

// atx::engine::combine — cost_util: shared cost-adjustment primitives (S4-1).
//
// ===========================================================================
//  Why this header exists (circular-include resolution)
// ===========================================================================
//  cost/cost_aware.hpp already #includes combine/gate.hpp and combine/metrics.hpp,
//  so gate.hpp CANNOT include cost_aware.hpp (circular). This leaf header is the
//  single source of truth for the 0.1 fitness-penalty scale and the
//  cost_adjusted_fitness formula. It depends ONLY on atx/core/types.hpp.
//
//  Consumers:
//    - combine/gate.hpp       (AlphaGate::admit fitness floor)
//    - library/library.hpp   (Library::verdict_for fitness floor)
//    - cost/cost_aware.hpp   (cost::cost_adjusted_fitness delegates here)

#include "atx/core/types.hpp" // atx::f64

namespace atx::engine::combine {

// Fitness-penalty scale: cost_penalty = turnover * rt_cost_bps * kFitnessCostScale,
// in raw-fitness units. Single source of truth shared by the gate, library, and
// cost_aware. See cost_aware.hpp (kFitnessScale) for derivation rationale.
inline constexpr atx::f64 kFitnessCostScale = 0.1;

// Net fitness = raw fitness - turnover-proportional cost penalty (fitness units).
// rt_cost_bps == 0 => returns raw_fitness unchanged (byte-identical inert default).
// Pure arithmetic: no allocation, no state, no branches on sign.
[[nodiscard]] inline atx::f64 cost_adjusted_fitness(atx::f64 raw_fitness, atx::f64 turnover,
                                                    atx::f64 rt_cost_bps) noexcept {
  return raw_fitness - turnover * rt_cost_bps * kFitnessCostScale;
}

} // namespace atx::engine::combine
