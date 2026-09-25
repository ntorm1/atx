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

// ===========================================================================
//  fit_shrinkage_mv_cleaned_cov (S3-4) — the ShrinkageMv weight fit gated on
//  risk_cfg.kind==Factor, exposed for direct-call testing against a HAND-BUILT
//  pool (the same rationale as fit_stack_combo above: precise control over an
//  N>=T fixture where the raw complete-case MLE covariance is singular/near-
//  singular and cleaned_alpha_cov's shrink+eigen-clip pipeline is expected to
//  produce a materially more diversified weight fit). See the definition in
//  stage_combine.cpp for the full "why not edit combiner.hpp" rationale.
// ===========================================================================
[[nodiscard]] atx::core::Result<atx::engine::combine::Combination>
fit_shrinkage_mv_cleaned_cov(const atx::engine::combine::AlphaStore& pool,
                             atx::usize fit_begin, atx::usize fit_end);

// ===========================================================================
//  W0-I0a — combine-stage point-in-time rules (I-01, I-02, I-03, I-08).
// ===========================================================================
//  Every numeric change keeps the pre-W0 behaviour reachable behind a versioned
//  enumerator (the *V1 values); the defaults are the corrected behaviour. None of
//  these is a RunConfig field (config.* is not this lane's to edit): run_combine(cfg)
//  and the two existing overloads forward CombinePitConfig{}.
//
//  ConvictionWindowRule (I-02): the conviction DSR/stability score of each alpha is
//    measured on the fit window [fit_begin, fit_end) (FitWindowV2) instead of the whole
//    stream including the holdout (FullStreamV1).
//  CapacityRule (I-03): the per-alpha capacity AUM behind --capacity-floor uses the fit
//    window only -- mean realized PnL over [fit_begin, fit_end) as the edge, and the
//    square-root impact of the TRADES (|w_t - w_{t-1}|) at each date t, sized with the
//    trailing 20-day dollar ADV and 60-day volatility ending at t (TrailingPitTradesV2).
//    FullPeriodHoldingsV1 used the full-period mean PnL, the LAST book's holdings and
//    the last-date ADV (look-ahead, and biased against low-turnover alphas).
//  WalkForwardRule (I-08): walk-forward folds live inside [fit_begin, fit_end), refit
//    through the SAME weight dispatch as the shipped book (method + stacking/regime +
//    cleaned covariance + conviction + Kelly + crowding/capacity), and each fold's test
//    window starts h + delay dates after its train window ends (h = stack_horizon for
//    Stack/RegimeStack, else 1). LinearNoEmbargoV1 is the pre-W0 fold loop (plain
//    AlphaCombiner with only the method copied -- which errs for Stack/RegimeStack -- no
//    embargo, folds running into the holdout).
//  HoldoutGuardRule (I-01): with a library source and a final test [test_begin, n), the
//    test must not overlap any discover train/holdout range recorded in the library's
//    split-range ledger (dead_alpha_wire.hpp); the test range is then recorded so a
//    later discover run refuses to read it. NoCheckV1 skips both.
enum class ConvictionWindowRule : atx::u8 { FitWindowV2 = 0, FullStreamV1 = 1 };
enum class CapacityRule : atx::u8 { TrailingPitTradesV2 = 0, FullPeriodHoldingsV1 = 1 };
enum class WalkForwardRule : atx::u8 { ShippedFitEmbargoedV2 = 0, LinearNoEmbargoV1 = 1 };
enum class HoldoutGuardRule : atx::u8 { RefuseOverlapV2 = 0, NoCheckV1 = 1 };

struct CombinePitConfig {
  ConvictionWindowRule conviction = ConvictionWindowRule::FitWindowV2;
  CapacityRule capacity = CapacityRule::TrailingPitTradesV2;
  WalkForwardRule walk_forward = WalkForwardRule::ShippedFitEmbargoedV2;
  HoldoutGuardRule holdout_guard = HoldoutGuardRule::RefuseOverlapV2;
  // First date of the final test. 0 = fit_end (the test starts right after the fit
  // window). A nested split sets it to fit_end + embargo; must lie in [fit_end, n].
  atx::usize test_begin = 0;
  // The "delay" of the walk-forward h + delay embargo (execution delay in dates).
  atx::usize execution_delay = 1;
};

[[nodiscard]] atx::core::Result<StageResult>
run_combine(const RunConfig& cfg, const atx::engine::combine::CombinerConfig& combiner_cfg,
            const atx::engine::risk::RiskModelConfig& risk_cfg, const CombinePitConfig& pit);

// cfg.method / cfg.risk_model parsed exactly as run_combine(cfg), plus explicit PIT rules.
[[nodiscard]] atx::core::Result<StageResult> run_combine(const RunConfig& cfg,
                                                         const CombinePitConfig& pit);

// ===========================================================================
//  W0-I0a (I-01) — nested splits: discover < combine fit < final test.
// ===========================================================================
//  Carves n_dates into three consecutive windows separated by `embargo` dates:
//    discover  [0, discover_end)          -- search + admission (its own lockbox inside)
//    fit       [fit_begin, fit_end)       -- the combiner weights (fresh to discover)
//    test      [test_begin, n_dates)      -- the final out-of-sample test
//  test_begin = n - floor(test_frac*n); fit_end = test_begin - embargo;
//  fit_begin = fit_end - floor(combine_frac*n); discover_end = fit_begin - embargo.
//  Err(InvalidArgument) when a fraction is outside (0, 1), their sum is >= 1, or any
//  window would hold fewer than 2 dates.
struct NestedSplitConfig {
  atx::f64 test_frac = 0.25;
  atx::f64 combine_frac = 0.25;
  atx::usize embargo = 2; // h + delay (1-day labels, 1-day execution delay)
};

struct NestedSplit {
  atx::usize n_dates = 0;
  atx::usize discover_end = 0;
  atx::usize fit_begin = 0;
  atx::usize fit_end = 0;
  atx::usize test_begin = 0;
};

[[nodiscard]] atx::core::Result<NestedSplit> resolve_nested_split(atx::usize n_dates,
                                                                  const NestedSplitConfig& cfg);

// run_discover restricted to the panel prefix [0, discover_end) (defined in
// stage_discover.cpp). Every discover read -- search, admission lockbox, capacity screen,
// robustness panels -- sees only that prefix, and the gated library path records the
// train/holdout ranges it used in the library's split-range ledger. discover_end == 0
// means the whole panel (== run_discover(cfg)).
[[nodiscard]] atx::core::Result<StageResult> run_discover_window(const RunConfig& cfg,
                                                                 atx::usize discover_end);

} // namespace atx::impl
