#pragma once

// atx::impl — stage_regime: p8-S3-3 direct-call HMM extension.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  run_regime (stages.hpp / stage_regime.cpp) is a MACRO-SERIES LOADER
//  (FRED/CBOE CSVs -> a regime-history artifact) — it never fits an HMM and is
//  UNTOUCHED by S3 (byte-identical; a genuinely separate function). fit_regime_hmm
//  below is the append-only HMM path the sprint's architecture note describes:
//  a thin, directly-testable wrapper around the frozen learn::baum_welch, so
//  stage_combine.cpp's RegimeStack branch (S3-3) has an S3-owned call site to
//  fit the guarded regime overlay instead of reaching into learn:: directly
//  from a DIFFERENT owned file.
//
//  Observable choice (binding, not a free parameter): the Hmm passed to
//  learn::fit_stack's regime-conditional arm MUST be fit on
//  learn::ensemble_detail::regime_observable(meta) — the cross-sectional mean
//  of the meta's LAST feature column — because fit_regime_nonlinear
//  RE-DERIVES that exact observable internally from `meta` (it does not take
//  the caller's obs as an input). Fitting this Hmm on a DIFFERENT series (e.g.
//  a panel close-return proxy, or a loaded macro series) would still compile
//  and run, but would apply a fitted emission model to an unrelated series
//  inside fit_regime_nonlinear — statistically meaningless. So
//  fit_regime_hmm is deliberately OBSERVABLE-AGNOSTIC (it just wraps
//  baum_welch): stage_combine.cpp is responsible for building `obs` via
//  ensemble_detail::regime_observable(meta) before calling this. A future
//  sprint that wants to fit on the LOADED MACRO SERIES instead (vix/move/...)
//  can do so by assembling that observable and calling this same function —
//  the seam stays open without touching this file again.

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/learn/hmm.hpp"

namespace atx::impl {

// Fit a PIT HMM (Baum-Welch EM, seeded/order-fixed — hmm.hpp M1) on a
// caller-assembled observable matrix (T x d). Pure passthrough to
// learn::baum_welch; exists as an S3-owned call site (see the header note).
[[nodiscard]] atx::engine::learn::Hmm
fit_regime_hmm(const atx::core::linalg::MatX& obs, const atx::engine::learn::HmmCfg& cfg);

} // namespace atx::impl
