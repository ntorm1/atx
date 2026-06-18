#pragma once

// atx::impl — shared helper: build the diagonal FactorModel from a research Panel.
//
// The model uses per-instrument population variance of daily TRI returns from the
// research "close" field, floored at 1e-4. X = M×1 zeros, F = Identity(1,1).
// This is the SAME model S5 (stage_optimize) and S6 (stage_report) both need;
// factored here to keep both callers byte-identical (DRY, no duplicate loop).

#include <cmath>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atx::impl {

namespace alpha = atx::engine::alpha;
namespace risk  = atx::engine::risk;

// Build a diagonal FactorModel from the per-instrument population variance of
// daily TRI returns in research.field("close"). Variance is floored at 1e-4.
// X = M×1 zeros, F = [[1]], dvar = per-instrument variance vector.
// Returns Err(InvalidArgument) if "close" is not a field in the panel.
[[nodiscard]] inline atx::core::Result<risk::FactorModel>
diagonal_risk_model(const alpha::Panel& research)
{
    const atx::usize M = research.instruments();
    const atx::usize D = research.dates();

    ATX_TRY(const auto close_id, research.field_id("close"));
    const auto close = research.field_all(close_id);  // date-major D*M

    atx::core::linalg::VecX dvar(static_cast<Eigen::Index>(M));
    for (atx::usize i = 0; i < M; ++i) {
        // Population variance of daily TRI returns, NaN-aware.
        std::vector<atx::f64> rets;
        atx::f64 mean = 0.0;
        atx::usize n = 0;
        for (atx::usize t = 1; t < D; ++t) {
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
    return risk::FactorModel::create(std::move(X), std::move(F), std::move(dvar), 0, D);
}

} // namespace atx::impl
