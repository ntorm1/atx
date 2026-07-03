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

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/risk/factor_model.hpp" // RiskModelConfig, FactorModel, FactorModelBuilder

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
[[nodiscard]] atx::core::Result<atx::engine::data::FactorModelArtifact>
build_risk_model(const atx::engine::alpha::Panel& research,
                  const atx::engine::risk::RiskModelConfig& cfg,
                  std::span<const atx::u32> group_id = {});

} // namespace atx::impl
