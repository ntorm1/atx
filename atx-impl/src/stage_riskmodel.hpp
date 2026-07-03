#pragma once

// atx::impl — stage_riskmodel: the p8-S1-1 covariance-source PRODUCER.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  build_risk_model(research, cfg) is the single entry point that turns a
//  research Panel into a FactorModelArtifact, dispatching on
//  RiskModelConfig::kind:
//
//    kind == Diagonal (the inert default) -> DELEGATES to diag_risk.hpp's
//      diagonal_risk_model, then lowers the resulting FactorModel's (X, F, D)
//      straight into an artifact. Byte-identical to today's stage_optimize /
//      stage_report path — this is a drop-in, not a reinterpretation.
//
//    kind == Factor (opt-in) -> assembles a Barra-style exposure matrix X from
//      PIT panel-derived columns (momentum / volatility / beta / a log-dollar-
//      ADV size proxy — see the field-mapping note below) plus industry dummies
//      (from a caller-supplied group_map), then calls the FROZEN
//      risk::FactorModelBuilder::build_components to estimate (X, F, D). S1
//      does NOT reimplement or duplicate any regression/shrinkage math here —
//      it only assembles the exposure inputs the existing estimator consumes.
//
// ===========================================================================
//  Field-mapping note (style_size -> StyleFactor::Liquidity)
// ===========================================================================
//  atx::engine::alpha::Panel (atx-impl's research panel) carries OHLCV only —
//  there is no market-cap field (risk/exposures.hpp's own header note confirms
//  this: "There is NO `cap`... in the panel"). risk::StyleFactor::Size requires
//  an external market_cap span this pipeline cannot supply. The sprint brief's
//  architecture note names the size factor as "log(dollar-ADV) OR log-cap
//  proxy" — dollar-ADV IS exactly risk::StyleFactor::Liquidity's definition
//  (ln(mean(close*volume, 20))). So RiskModelConfig::style_size here toggles
//  emitting StyleFactor::Liquidity (the panel-derivable size proxy), NOT
//  StyleFactor::Size (which build_exposures always omits anyway when the
//  market_cap span is empty, regardless of style_mask — so this mapping is
//  the only way style_size has any effect on this panel shape).
//
// ===========================================================================
//  PanelView bridge (Panel -> PanelView)
// ===========================================================================
//  risk::FactorModelBuilder::build_components consumes atx::engine::PanelView —
//  a NON-OWNING, newest-first zero-copy ring-buffer accessor over RollingPanel
//  storage (loop/panel_types.hpp) — not atx::engine::alpha::Panel (this
//  pipeline's flat, date-major, file-backed research panel). No existing
//  adapter bridges the two (RollingPanel is a live backtest-loop construct).
//  build_risk_model owns a small per-fit-window (fields_, mask_) buffer sized
//  to [fit_end - lookback, fit_end), fills it by REVERSING alpha::Panel's
//  date-major order into PanelView's newest-first convention (row 0 = fit_end-1,
//  the newest date in the window), and constructs a PanelView over that owned
//  buffer — the same pattern risk_factor_builder_test.cpp's PanelFixture uses.
//  This is a documented COLD-path allocation (once per fit-window), matching
//  the existing estimator's own "COLD path" contract.
//
// ===========================================================================
//  PIT (point-in-time)
// ===========================================================================
//  The owned buffer is filled ONLY from panel rows in [fit_end - lookback,
//  fit_end) — rows at or after fit_end are never read, so no future bar can
//  enter X (guarded by the S1-1 PIT test, which perturbs rows >= fit_end and
//  asserts the artifact is unchanged).
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG. The exposure columns are built in canonical ascending instrument
//  order (matching risk/exposures.hpp's own contract); the estimator
//  (build_components) is itself order-fixed. Same (research, cfg) -> the same
//  FactorModelArtifact bytes, run to run and process to process.

#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/store.hpp"     // combine::AlphaId (dead_ids element type)
#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/library/fwd.hpp"       // library::Library fwd decl (S1-4, optional dead-alpha source)
#include "atx/engine/risk/factor_model.hpp" // RiskModelConfig, FactorModel, FactorModelBuilder

#include "config.hpp" // RunConfig
#include "stages.hpp" // StageResult

namespace atx::impl {

// Build a FactorModelArtifact from `research` as of the LAST date in the panel
// (fit_end == research.dates()), per `cfg`:
//
//   cfg.kind == Diagonal -> lowers diagonal_risk_model(research)'s (X, F, D)
//     into the artifact. Byte-identical to the pre-S1 path (X=Mx1 zeros,
//     F=Identity(1,1), D=per-instrument TRI-return variance).
//
//   cfg.kind == Factor -> assembles the Barra-style exposure block over
//     [research.dates() - cfg.fit_lookback_days, research.dates()) (clamped to
//     [0, research.dates())) using research's "close"/"volume" fields, optional
//     `group_id` industry dummies (cfg.industry — pass an empty span to omit),
//     and calls FactorModelBuilder::build_components to estimate (X, F, D).
//
// group_id, when non-empty, MUST have length == research.instruments()
// (mirrors risk::build_exposures's own contract) -> Err(InvalidArgument)
// otherwise. Err(InvalidArgument) if research has no "close" field, or if the
// resolved fit window has fewer than 2 usable rows (Factor path only — the
// Diagonal path's own requirements are diagonal_risk_model's, unchanged).
//
// S1-4 (dead-alpha crowding factors, opt-in via cfg.dead_alpha_factors):
// after the base (X, F, D) is assembled (Diagonal OR Factor — augmentation
// composes with either kind), when cfg.dead_alpha_factors is true AND `dead_lib`
// is non-null, the holdings of every AlphaId in `dead_ids` are read from
// `dead_lib` at `dead_as_of` via risk::extract_dead_factors (Kakushadze-Yu
// holdings-overlap eigen-extraction) and folded in via
// risk::augment_factor_model — a PURE transform on the already-built
// FactorModel; the style/industry block is NOT re-estimated. This raises the
// variance the optimizer assigns to a dead alpha's crowded holdings
// direction, steering the book off it (R6).
//
// FAIL-OPEN GUARD (per the sprint's risk table): cfg.dead_alpha_factors=true
// with `dead_lib == nullptr` (the deploy panel has no library available) is a
// documented NO-OP — the base model is returned UNAUGMENTED, not an Err. This
// is intentional: dead-factor augmentation is a risk-reduction ENHANCEMENT,
// and a caller without a library should not have its entire risk-model build
// fail. `dead_ids` empty is likewise a no-op (extract_dead_factors' own
// documented boundary: k_dead == 0 -> augment_factor_model is a passthrough).
[[nodiscard]] atx::core::Result<atx::engine::data::FactorModelArtifact>
build_risk_model(const atx::engine::alpha::Panel& research,
                  const atx::engine::risk::RiskModelConfig& cfg,
                  std::span<const atx::u32> group_id = {},
                  const atx::engine::library::Library* dead_lib = nullptr,
                  std::span<const atx::engine::combine::AlphaId> dead_ids = {},
                  atx::usize dead_as_of = 0);

// ===========================================================================
//  S1-2: run_optimize's covariance-source overload.
// ===========================================================================
//  The p8-S1-2 seam. `run_optimize(const RunConfig&)` (declared in stages.hpp,
//  the S5-CLI-hub-owned public entry point) is UNCHANGED and forwards to this
//  overload with a default-constructed RiskModelConfig{} (kind==Diagonal,
//  inert) — so the no-flag / CLI path is byte-identical to pre-S1 by
//  construction (same code, same inert input), not by parallel maintenance of
//  two implementations. RiskModelConfig is NOT threaded onto RunConfig here:
//  that CLI/config-file wiring is Sprint 5's hub job (sprint-1's "Out of
//  scope"). This overload is how S1's tests exercise the Factor path directly
//  ("a direct-call integration test, not the CLI" per the sprint brief).
//
//  kind == Diagonal: identical to today's stage_optimize body — builds
//  diagonal_risk_model(research) once, applies it to every period.
//  kind == Factor: builds ONE FactorModelArtifact via build_risk_model
//  (fit over the whole research panel, fit_end == research.dates()) and
//  applies THAT model to every period — the "piecewise-constant... matching
//  the existing single-model cadence" the brief specifies (S1 fits once per
//  run_optimize call, exactly mirroring the Diagonal path's own cadence; a
//  true per-rebalance-window re-fit is not required by S1's accept criteria
//  and is left to a future sprint if the cadence needs to tighten).
[[nodiscard]] atx::core::Result<StageResult>
run_optimize(const RunConfig& cfg, const atx::engine::risk::RiskModelConfig& risk_cfg);

} // namespace atx::impl
