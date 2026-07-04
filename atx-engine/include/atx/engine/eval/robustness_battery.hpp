#pragma once

// atx::engine::eval — RobustnessBattery (p8 Sprint 5, S5-3): the automated
// admission-time robustness subsystem that rejects the degenerate-alpha class of
// artifact — an edge that clears EVERY statistical gate (Sharpe, fitness, DSR,
// PBO, correlation) yet is actually a dimensional tilt or pure noise. The
// motivating case: a measured `1/price` "alpha" that passed every existing gate
// because none of them probe whether the edge is a DIMENSIONAL artifact or
// survives on RANDOMIZED inputs.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  Four independently-toggleable, pure, deterministic checks over a candidate's
//  ALREADY-COMPUTED baseline edge (a DSR or comparable scalar) plus a caller-
//  supplied re-evaluation callback (`Reevaluator`). The battery is ENGINE-
//  AGNOSTIC: it knows nothing about Genome/Panel/the VM. It only (a) builds the
//  perturbed `RobustnessScenario` each check needs (a sub-universe instrument
//  mask, a seeded-permuted alternate group map, a seeded-permuted "noise" input,
//  or a jittered param scale) and (b) asks the caller's `Reevaluator` to report
//  the resulting edge for that scenario — the caller owns compiling/evaluating
//  the actual alpha (typically wrapping factory::Genome + alpha::Engine +
//  extract_streams, exactly as fitness.hpp's §0.8 robust-factor re-eval does).
//
// ===========================================================================
//  The four checks
// ===========================================================================
//  sub_universe        : re-evaluate on the TOP-N-by-liquidity (ADV) restricted
//                        universe. An edge concentrated in a handful of illiquid
//                        names collapses; a broad edge survives. SURVIVAL check:
//                        PASS iff scenario_edge >= min_survival_ratio * base_edge.
//  alt_neutralization   : re-evaluate after residualizing against an ALTERNATE
//                        group map — a seeded permutation of the candidate's OWN
//                        group_id vector (same instrument set, shuffled labels).
//                        A pure sector/dimensional tilt collapses under ANY
//                        group-level residualization; an idiosyncratic edge
//                        survives. SURVIVAL check (same shape as sub_universe).
//  noise_control        : re-evaluate after replacing the candidate's INPUT
//                        values with a SEEDED PERMUTATION of themselves (a
//                        permutation trivially preserves the exact empirical
//                        marginal distribution — the "matched marginal" the plan
//                        specifies — while destroying any genuine time-series/
//                        cross-sectional STRUCTURE). NEGATIVE control: an edge
//                        that SURVIVES on noise is a dimensional artifact (the
//                        `1/price` class) -> REJECT. INVERTED polarity vs the
//                        other three checks: PASS iff the edge COLLAPSES
//                        (scenario_edge < min_survival_ratio * base_edge).
//  param_perturbation   : re-evaluate under `param_perturbation_draws` seeded
//                        multiplicative jitters of the candidate's own numeric
//                        param(s) (uniform in [1-band, 1+band]). A stable
//                        candidate's edge varies little across draws; a
//                        knife-edge candidate (edge only at one exact param
//                        value) swings wildly. PASS iff the coefficient of
//                        variation of the resulting edges is
//                        <= param_perturbation_max_cv.
//
// ===========================================================================
//  Determinism (the gate for every check)
// ===========================================================================
//  All-false BatteryConfig -> BatteryResult::ran == false, the Reevaluator is
//  NEVER called, no state touched (off-path byte-identity for the pipeline). Each
//  ENABLED check that draws randomness (noise_control's permutation,
//  alt_neutralization's permutation, param_perturbation's jitter draws) seeds an
//  INDEPENDENT atx::core::Xoshiro256pp from `cfg.seed` XORed with a check-
//  specific salt constant — NEVER thread/time — so (a) a check's result never
//  depends on whether any OTHER check is enabled (no shared-PRNG ordering
//  coupling) and (b) the whole battery is reproducible run-to-run and
//  seq-equals-parallel BY CONSTRUCTION (no internal parallel_for; every check is
//  a fixed, small, sequential reduction). sub_universe's ranking is pure sorting
//  (no RNG at all).
//
//  A check whose required CandidateInputs field is empty (no `adv` for
//  sub_universe, no `group_id` for alt_neutralization, no `input_values` for
//  noise_control), or whose Reevaluator call returns Err, is marked
//  `applicable=false` and contributes NOTHING to `overall_pass` (degrades
//  gracefully — never a hard block, never a fabricated verdict).

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "atx/core/error.hpp" // atx::core::Result
#include "atx/core/types.hpp" // atx::f64, atx::u8, atx::u32, atx::u64, atx::usize

namespace atx::engine::eval {

// ===========================================================================
//  BatteryConfig — the per-check opt-in toggles + shared thresholds.
// ===========================================================================
struct BatteryConfig {
  bool sub_universe = false;       // rerun on TOP-N-by-ADV restricted universes; edge must survive
  bool alt_neutralization = false; // rerun under an alternate group_map; edge must survive
  bool noise_control = false;      // NEGATIVE control: edge must COLLAPSE on randomized inputs
  bool param_perturbation = false; // perturb the candidate's params; edge must be stable
  atx::f64 min_survival_ratio = 0.5; // fraction of the base edge sub_universe/alt_neutralization must retain
  atx::u64 seed = 0;                 // deterministic RNG seed (NEVER thread/time)

  // sub_universe: 0 -> half the universe (floor, minimum 1); explicit -> that count
  // (clamped to the available instrument count).
  atx::usize sub_universe_top_n = 0;
  // param_perturbation: how many jittered draws, the relative jitter band (the
  // multiplicative scale is drawn uniform in [1-band, 1+band]), and the max
  // allowed coefficient of variation (population stddev / |mean|) of the
  // resulting edges before the candidate is flagged a knife-edge.
  atx::usize param_perturbation_draws = 8;
  atx::f64 param_perturbation_band = 0.10;
  atx::f64 param_perturbation_max_cv = 0.25;

  // All-false (the struct default) is the no-op: BatteryResult::ran == false and
  // the Reevaluator is never invoked.
  [[nodiscard]] bool any_enabled() const noexcept {
    return sub_universe || alt_neutralization || noise_control || param_perturbation;
  }
};

// ===========================================================================
//  RobustnessScenario — the perturbed input a check asks the caller's
//  Reevaluator to score. Exactly one variant's fields are meaningful per `kind`;
//  the others are left at their empty/identity default.
// ===========================================================================
enum class ScenarioKind : atx::u8 {
  Baseline,          // the candidate's own, unperturbed inputs (used to (re)confirm base_edge)
  SubUniverse,       // keep_instruments is the TOP-N-by-ADV index subset (ascending, deduplicated)
  AltNeutralization, // alt_group_id is a seeded permutation of the candidate's own group_id
  NoiseControl,      // noise_input is a seeded permutation of the candidate's own input_values
  ParamPerturbation, // param_scale is the multiplicative jitter to apply to the candidate's param(s)
};

struct RobustnessScenario {
  ScenarioKind kind = ScenarioKind::Baseline;
  std::vector<atx::usize> keep_instruments; // SubUniverse
  std::vector<atx::u32> alt_group_id;       // AltNeutralization (same length as the candidate's group_id)
  std::vector<atx::f64> noise_input;        // NoiseControl (same length as the candidate's input_values)
  atx::f64 param_scale = 1.0;               // ParamPerturbation
};

// The candidate re-evaluation seam (§ header doc): given a scenario, return the
// resulting edge (a DSR or any comparable higher-is-better scalar the caller
// defines consistently across scenarios). Err propagates a genuine evaluation
// failure (e.g. an empty sub-universe) — the battery treats it as "check
// inapplicable", not a crash.
using Reevaluator = std::function<atx::core::Result<atx::f64>(const RobustnessScenario &)>;

// ===========================================================================
//  CandidateInputs — the per-candidate data each check needs to build its
//  RobustnessScenario. A field left empty marks its corresponding check
//  inapplicable (graceful degradation — see the header's determinism note).
//  All spans are BORROWED for the duration of the run() call only.
// ===========================================================================
struct CandidateInputs {
  atx::f64 base_edge = 0.0;               // the candidate's OWN already-computed edge (e.g. DSR)
  std::span<const atx::f64> adv;          // per-instrument liquidity proxy (sub_universe ranking)
  std::span<const atx::u32> group_id;     // the candidate's OWN neutralization group_map (alt_neutralization)
  std::span<const atx::f64> input_values; // the raw INPUT column(s) the signal reads (noise_control)
};

// ===========================================================================
//  CheckOutcome — one check's verdict.
// ===========================================================================
struct CheckOutcome {
  bool applicable = false;       // false: the required input/reeval was unavailable (never a hard block)
  bool passed = false;           // meaningful only when applicable
  atx::f64 base_edge = 0.0;      // the edge this check compared against
  atx::f64 scenario_edge = 0.0;  // sub_universe/alt_neutralization/noise_control: the perturbed edge.
                                 // param_perturbation: the coefficient of variation across draws.
  atx::f64 survival_ratio = 0.0; // scenario_edge / base_edge (0 when base_edge <= 0 or inapplicable)
};

// ===========================================================================
//  BatteryResult — the aggregate verdict.
// ===========================================================================
struct BatteryResult {
  bool ran = false;          // false iff the all-false config no-op (nothing else is meaningful)
  bool overall_pass = true;  // AND of every ENABLED-and-APPLICABLE check's `passed`
  CheckOutcome sub_universe;
  CheckOutcome alt_neutralization;
  CheckOutcome noise_control;
  CheckOutcome param_perturbation;
};

// ===========================================================================
//  RobustnessBattery — stateless orchestrator (holds no members; a namespace
//  scoped as a struct so call sites read `RobustnessBattery::run(...)`, matching
//  the house `Factory`/`AlphaGate` static-surface convention).
// ===========================================================================
struct RobustnessBattery {
  // Run every ENABLED check in `cfg` against `inputs`, using `reeval` to score
  // each perturbed scenario. Pure: touches no external state; the only
  // randomness is the per-check seeded Xoshiro256pp streams derived from
  // `cfg.seed` (never thread/time). All-false `cfg` returns a default
  // BatteryResult{} (ran=false) WITHOUT calling `reeval` at all.
  [[nodiscard]] static BatteryResult run(const BatteryConfig &cfg, const CandidateInputs &inputs,
                                         const Reevaluator &reeval);
};

} // namespace atx::engine::eval
