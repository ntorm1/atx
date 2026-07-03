#pragma once

// atx::impl — stage_combine: p8-S3 direct-call overloads.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  run_combine(const RunConfig&) (declared in stages.hpp, the S5-CLI-hub-owned
//  public entry point) parses cfg.method into a combine::CombineMethod and
//  forwards, with an inert-default combine::CombinerConfig, to the
//  CombinerConfig-parameterized overload below — mirroring the p8-S1-2 seam
//  (`run_optimize(const RunConfig&, const risk::RiskModelConfig&)`,
//  stage_riskmodel.hpp) exactly. This is how S3's Stack/RegimeStack methods
//  and their stacking/regime knobs (combine::CombinerConfig's S3-0 fields) are
//  exercised WITHOUT threading a new field onto RunConfig (config.hpp/.cpp are
//  Sprint-5-owned hub files S3 must not edit) — the caller constructs a
//  CombinerConfig with method=Stack/RegimeStack + the desired knobs directly.
//  CLI flag threading (`--method stack`, `--stack-*`) is Sprint 5's job; S3
//  proves the engine path via these direct-call overloads (the run_all/CLI
//  seam, recorded in the sprint-3 ledger).
//
//  The three-argument overload additionally threads a risk::RiskModelConfig
//  (S3-4): behind kind==Factor, the ShrinkageMv weight-fit and the breadth-
//  instrumentation covariance consume the S1-shipped `data::cleaned_alpha_cov`
//  accessor instead of the raw `combine::detail::mle_covariance`; kind==Diagonal
//  (the default, reached by every existing caller via the zero/one-arg
//  forwards) is byte-identical to pre-S3.

#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/learn/ensemble.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "config.hpp"
#include "stages.hpp"

namespace atx::impl {

// S3-1/S3-3: the method + stacking/regime knobs come from `combiner_cfg`
// directly (cfg.method is IGNORED by this overload — only the zero-arg
// `run_combine(const RunConfig&)` reads it). risk_model defaults to
// kind==Diagonal (S3-4 inert default).
[[nodiscard]] atx::core::Result<StageResult>
run_combine(const RunConfig& cfg, const atx::engine::combine::CombinerConfig& combiner_cfg);

// S3-4: additionally threads the covariance-source selector.
[[nodiscard]] atx::core::Result<StageResult>
run_combine(const RunConfig& cfg, const atx::engine::combine::CombinerConfig& combiner_cfg,
            const atx::engine::risk::RiskModelConfig& risk_cfg);

// ===========================================================================
//  fit_stack_combo (S3-1/S3-2/S3-3) — the Stack/RegimeStack producer + honest-
//  gate, exposed for direct-call testing against a HAND-BUILT pool (bypassing
//  the DSL/VM entirely — the same technique ensemble_test.cpp uses for
//  fit_stack itself), so the admit/reject fixtures can construct an exact
//  interaction/linear meta without depending on a real alpha DSL expression
//  happening to produce that structure. See the definition in
//  stage_combine.cpp for the full admit-vs-fallback contract.
//
//  `close_all` is the research panel's date-major close field (length
//  n_dates*ni); `pool` must already share that same n_periods()/n_instruments()
//  shape. `with_regime` selects Stack (false — S3-1/S3-2, flat nonlinear arm +
//  flat linear fallback) vs RegimeStack (true — S3-3: fits a PIT HMM on
//  learn::ensemble_detail::regime_observable(meta) internally, so the
//  nonlinear arm is regime-conditional and the fallback is the regime-
//  conditional linear combine::fit_regime_combiner blend). n_regimes==1
//  (HmmCfg.n_states==1, combiner_cfg.regime_n_states) makes the RegimeStack
//  path byte-identical to the corresponding Stack call — the critical
//  single-state fallback guard (regime_combiner.hpp's own documented
//  contract, composed with fit_stack's identical flat-vs-1-regime reduction).
// ===========================================================================
struct StackFitResult {
  atx::engine::combine::Combination combo;
  atx::engine::learn::StackingVerdict verdict;
};

[[nodiscard]] atx::core::Result<StackFitResult>
fit_stack_combo(const atx::engine::combine::AlphaStore& pool, std::span<const atx::f64> close_all,
                atx::usize ni, atx::usize fit_begin, atx::usize fit_end,
                const atx::engine::combine::CombinerConfig& combiner_cfg,
                bool with_regime);

} // namespace atx::impl
