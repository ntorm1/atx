#pragma once

// atx::engine::factory — CachedScore: the per-canon_hash fitness-cache VALUE.
//
// Extracted into its own tiny header (from search_driver.hpp) so the resumable-
// discover serialization layer (search_progress.hpp) can serialize/restore the
// fitness_cache WITHOUT a circular include against search_driver.hpp (which itself
// includes search_progress.hpp). search_driver.hpp re-includes this header so the
// public type is unchanged; CachedScore stays a trivial aggregate.

#include <array>
#include <limits>
#include <vector>

#include "atx/core/types.hpp"             // atx::f64, atx::u8
#include "atx/engine/factory/fitness.hpp" // factory::kMaxObjectives

namespace atx::engine::factory {

// =========================================================================
//  ScoreOrigin — how a CachedScore was obtained (L3).
//
//    Full                — a real full-fidelity pool_aware_fitness pass (or the
//                          legacy fitness-error default); the only origin whose
//                          genome may be EMITTED as an admitted candidate.
//    FingerprintBorrowed — output_dedup hit: raw/objectives/descriptor are an
//                          APPROXIMATE score borrowed from an earlier genome whose
//                          rank-quantized signal fingerprint collided (parsimony is
//                          recomputed for the borrowing genome). Selectable, never
//                          emitted.
//    FidelityRejected    — dropped at a low fidelity rung, never fully scored.
//                          Carries the worst-case sentinel (raw == -inf, no live
//                          objectives): ranks last in ScalarRaw, is excluded from
//                          the NSGA-II sort (placed on a trailing front), never
//                          emitted, never credited to an operator.
// =========================================================================
enum class ScoreOrigin : atx::u8 {
  Full = 0,
  FingerprintBorrowed = 1,
  FidelityRejected = 2,
  IcRejected = 3, // screened on forward-return IC; still a distinct research trial
  ResidualUnavailable = 4, // evaluated/attempted but no defined three-horizon IC score
  // platform v8 H-3, signal-fitness path only: attempted but never scored (compile or VM
  // failure, or a non-finite functor score). A trial; never selected, emitted or checkpointed.
  Unscored = 5,
};

[[nodiscard]] constexpr bool is_rejected_score(ScoreOrigin origin) noexcept {
  return origin == ScoreOrigin::FidelityRejected || origin == ScoreOrigin::IcRejected ||
      origin == ScoreOrigin::ResidualUnavailable || origin == ScoreOrigin::Unscored;
}

// Worst-case raw sentinel for a fidelity-rejected candidate. -inf (not lowest())
// so it survives the hex checkpoint round-trip and is recognisable on resume.
inline constexpr atx::f64 kRejectedRaw = -std::numeric_limits<atx::f64>::infinity();

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
  // L3: provenance of this score (see ScoreOrigin). Not serialized by the resume
  // checkpoint for legacy origins; deserialize_cache restores FidelityRejected from the -inf raw
  // sentinel, while a FingerprintBorrowed score resumes as Full (documented: runs
  // with output_dedup on are not resume byte-identical). IcRejected has an explicit
  // optional codec tag so it retains its exclusion identity after resume.
  ScoreOrigin origin{ScoreOrigin::Full};
};

// Canonical fidelity-rejected score: sentinel raw, zero live objectives, empty
// descriptor (skipped by the behavioral-novelty pass).
[[nodiscard]] inline CachedScore rejected_score() {
  CachedScore cs{};
  cs.raw = kRejectedRaw;
  cs.origin = ScoreOrigin::FidelityRejected;
  return cs;
}

[[nodiscard]] inline CachedScore ic_rejected_score() {
  CachedScore cs = rejected_score();
  cs.origin = ScoreOrigin::IcRejected;
  return cs;
}

[[nodiscard]] inline CachedScore residual_unavailable_score() {
  CachedScore cs = rejected_score();
  cs.origin = ScoreOrigin::ResidualUnavailable;
  return cs;
}

[[nodiscard]] inline CachedScore unscored_score() {
  CachedScore cs = rejected_score();
  cs.origin = ScoreOrigin::Unscored;
  return cs;
}

} // namespace atx::engine::factory
