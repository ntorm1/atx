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

#include "atx/core/error.hpp"
#include "atx/engine/combine/combiner.hpp"
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

} // namespace atx::impl
