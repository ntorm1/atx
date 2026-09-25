#pragma once

// atx::impl — shared helper: build the diagonal FactorModel from a research Panel.
//
// The model uses per-instrument population variance of daily TRI returns from the
// research "close" field, floored at 1e-4. X = M×1 zeros, F = Identity(1,1).
// This is the SAME model S5 (stage_optimize) and S6 (stage_report) both need;
// factored here to keep both callers byte-identical (DRY, no duplicate loop).
//
// p8-S1 fix-loop: two overloads, ONE implementation. diagonal_risk_model(research,
// fit_end) is the real body -- it reads ONLY panel rows in [0, fit_end) (the loop
// bound is `t < fit_end`, not `t < research.dates()`), so it is PIT-clean for any
// fit_end <= research.dates() and is the Factor path's warm-up fallback for a
// rebalance step too early to support a genuine Factor fit (stage_optimize.cpp).
// diagonal_risk_model(research) (the ORIGINAL, still-public zero-arg signature every
// pre-S1 caller uses) just forwards to it with fit_end == research.dates() -- by
// construction this is the SAME loop bound as before (`t < D` with D ==
// research.dates()), so the Diagonal-KIND path stays byte-identical, not merely
// numerically equal.

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "config.hpp"          // RunConfig (run_optimize PIT overload)
#include "dead_alpha_wire.hpp" // DeadAlphaRule (DeployPitConfig)
#include "stages.hpp"          // StageResult

namespace atx::impl {

namespace alpha = atx::engine::alpha;
namespace risk  = atx::engine::risk;

// Build a diagonal FactorModel from the per-instrument population variance of
// daily TRI returns in research.field("close"), reading ONLY rows in
// [0, fit_end) -- rows >= fit_end are never touched (PIT-clean). Variance is
// floored at 1e-4. X = M×1 zeros, F = [[1]], dvar = per-instrument variance
// vector. fit_end MUST be <= research.dates() (checked; Err(InvalidArgument)
// otherwise) and > 0 (FactorModel::create rejects an empty [0, fit_end) window).
// Returns Err(InvalidArgument) if "close" is not a field in the panel.
//
// This is the Factor path's warm-up fallback (stage_optimize.cpp): a rebalance
// step whose date is too early for a genuine Factor fit uses this PIT diagonal
// over [0, fit_end) for that step ONLY -- never the whole-panel diagonal below,
// which would reintroduce look-ahead for that step.
[[nodiscard]] inline atx::core::Result<risk::FactorModel>
diagonal_risk_model(const alpha::Panel& research, atx::usize fit_end)
{
    const atx::usize M = research.instruments();
    const atx::usize D = research.dates();
    if (fit_end > D) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "diagonal_risk_model: fit_end exceeds research.dates()");
    }

    ATX_TRY(const auto close_id, research.field_id("close"));
    const auto close = research.field_all(close_id);  // date-major D*M

    atx::core::linalg::VecX dvar(static_cast<Eigen::Index>(M));
    for (atx::usize i = 0; i < M; ++i) {
        // Population variance of daily TRI returns, NaN-aware. PIT: t ranges
        // only over [1, fit_end) -- rows >= fit_end never enter `rets`.
        std::vector<atx::f64> rets;
        atx::f64 mean = 0.0;
        atx::usize n = 0;
        for (atx::usize t = 1; t < fit_end; ++t) {
            const atx::f64 p0 = close[(t - 1) * M + i];
            const atx::f64 p1 = close[t * M + i];
            if (!std::isnan(p0) && !std::isnan(p1) && p0 != 0.0) {
                const atx::f64 r = p1 / p0 - 1.0;
                rets.push_back(r);
                mean += r;
                ++n;
            }
        }
        atx::f64 v = 1e-4;  // fallback floor for a degenerate stream
        if (n >= 2) {
            mean /= static_cast<atx::f64>(n);
            atx::f64 s = 0.0;
            for (atx::f64 r : rets) {
                s += (r - mean) * (r - mean);
            }
            v = std::max(1e-4, s / static_cast<atx::f64>(n));
        }
        dvar[static_cast<Eigen::Index>(i)] = v;
    }

    atx::core::linalg::MatX X = atx::core::linalg::MatX::Zero(
        static_cast<Eigen::Index>(M), 1);
    atx::core::linalg::MatX F = atx::core::linalg::MatX::Identity(1, 1);
    return risk::FactorModel::create(std::move(X), std::move(F), std::move(dvar), 0, fit_end);
}

// The ORIGINAL zero-arg signature (every pre-S1 caller): the whole-panel
// diagonal model. Forwards to the fit_end overload with fit_end ==
// research.dates() -- the SAME loop bound as the pre-fix body (`t < D`), so
// this path is byte-identical BY CONSTRUCTION, not by parallel-maintained
// duplicate logic.
[[nodiscard]] inline atx::core::Result<risk::FactorModel>
diagonal_risk_model(const alpha::Panel& research)
{
    return diagonal_risk_model(research, research.dates());
}

// ===========================================================================
//  W0-I0a (I-04): per-step PIT diagonal models.
// ===========================================================================
//  DiagRiskRule selects the diagonal risk lens the deploy stages (optimize,
//  metabook) apply at each rebalance step:
//    PerStepPitV2 (default): one model per step, fitted on rows [0, period+1) only.
//    WholePanelV1: the pre-W0 single whole-panel model applied to every step (an
//      early book was sized with variance estimated from later dates). Kept only
//      so frozen artifacts can be re-derived.
enum class DiagRiskRule : atx::u8 {
    PerStepPitV2 = 0,
    WholePanelV1 = 1,
};

// One diagonal FactorModel per entry of `fit_ends` (ascending, each in [1, D]),
// model k reading ONLY rows [0, fit_ends[k]). Same estimator as
// diagonal_risk_model(research, fit_end) -- NaN-aware population variance of the
// daily TRI returns, floored at 1e-4, fallback 1e-4 below two returns -- computed in
// ONE expanding pass (Welford), so the cost is O(D*M) for the whole schedule instead
// of O(D*M) per step. Welford and the two-pass form agree to rounding (~1e-15
// relative), not bit-for-bit. Err(InvalidArgument) on a non-ascending / zero /
// out-of-panel fit_end or a panel without "close".
[[nodiscard]] inline atx::core::Result<std::vector<risk::FactorModel>>
diagonal_risk_models_expanding(const alpha::Panel& research, std::span<const atx::usize> fit_ends)
{
    const atx::usize M = research.instruments();
    const atx::usize D = research.dates();
    for (atx::usize k = 0; k < fit_ends.size(); ++k) {
        if (fit_ends[k] == 0U || fit_ends[k] > D || (k > 0U && fit_ends[k] < fit_ends[k - 1U])) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "diagonal_risk_models_expanding: fit_ends must be ascending "
                                  "and in [1, dates]");
        }
    }
    ATX_TRY(const auto close_id, research.field_id("close"));
    const auto close = research.field_all(close_id); // date-major D*M

    std::vector<atx::usize> n(M, 0U);
    std::vector<atx::f64> mean(M, 0.0);
    std::vector<atx::f64> m2(M, 0.0);
    std::vector<risk::FactorModel> out;
    out.reserve(fit_ends.size());
    atx::usize next_t = 1U; // next return row to fold in (returns live at t >= 1)
    for (const atx::usize fit_end : fit_ends) {
        for (; next_t < fit_end; ++next_t) { // PIT: only rows < fit_end
            for (atx::usize i = 0; i < M; ++i) {
                const atx::f64 p0 = close[(next_t - 1U) * M + i];
                const atx::f64 p1 = close[next_t * M + i];
                if (std::isnan(p0) || std::isnan(p1) || p0 == 0.0) {
                    continue;
                }
                const atx::f64 r = p1 / p0 - 1.0;
                ++n[i];
                const atx::f64 delta = r - mean[i];
                mean[i] += delta / static_cast<atx::f64>(n[i]);
                m2[i] += delta * (r - mean[i]);
            }
        }
        atx::core::linalg::VecX dvar(static_cast<Eigen::Index>(M));
        for (atx::usize i = 0; i < M; ++i) {
            const atx::f64 v = (n[i] >= 2U) ? std::max(1e-4, m2[i] / static_cast<atx::f64>(n[i]))
                                            : 1e-4;
            dvar[static_cast<Eigen::Index>(i)] = v;
        }
        atx::core::linalg::MatX X =
            atx::core::linalg::MatX::Zero(static_cast<Eigen::Index>(M), 1);
        atx::core::linalg::MatX F = atx::core::linalg::MatX::Identity(1, 1);
        ATX_TRY(auto model,
                risk::FactorModel::create(std::move(X), std::move(F), std::move(dvar), 0, fit_end));
        out.push_back(std::move(model));
    }
    return atx::core::Ok(std::move(out));
}

// ===========================================================================
//  W0-I0a: the deploy stages' point-in-time rules (optimize + metabook).
// ===========================================================================
//  ParticipationAdvRule (R-12) selects the ADV/price reference the optimizer's
//  participation cap uses:
//    TrailingPitPerRebalanceV2 (default): at each rebalance date d, the mean volume
//      over the trailing window [d-19, d] and the close at d -- data through d only.
//      A name without a price at d (not listed yet, or delisted) gets cap 0 at that
//      date only.
//    LastDateV1: the pre-W0 reference -- the panel's LAST 20 dates and last close
//      applied to every rebalance (look-ahead; a name delisted before the end had
//      cap 0 for its whole history). Kept only to re-derive frozen artifacts.
enum class ParticipationAdvRule : atx::u8 {
    TrailingPitPerRebalanceV2 = 0,
    LastDateV1 = 1,
};

// Stage-private knobs (no RunConfig field: config.* is not this lane's to edit).
// Every default is the corrected, point-in-time behaviour.
struct DeployPitConfig {
    DiagRiskRule diag = DiagRiskRule::PerStepPitV2;                          // I-04
    DeadAlphaRule dead = DeadAlphaRule::DeadOrDecayingPerStepV2;             // I-06
    ParticipationAdvRule participation = ParticipationAdvRule::TrailingPitPerRebalanceV2; // R-12
};

// run_optimize with explicit point-in-time rules. The 1-arg and 2-arg overloads
// (stages.hpp / stage_riskmodel.hpp) forward here with DeployPitConfig{}.
[[nodiscard]] atx::core::Result<StageResult>
run_optimize(const RunConfig& cfg, const risk::RiskModelConfig& risk_cfg,
             const DeployPitConfig& pit);

} // namespace atx::impl
