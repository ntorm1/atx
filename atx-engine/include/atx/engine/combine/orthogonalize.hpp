#pragma once

// atx::engine::combine — signal orthogonalization + marginal IC (Lane 5).
//
//   residualize_signal   per date t, the weighted-least-squares residual of s_t on
//                        [B_t | pool_1,t | … | pool_P,t] with W = diag(1/σ²_spec):
//                        s̃ = (I − X(XᵀWX)⁺XᵀW)s. The residual is W-orthogonal to every
//                        risk exposure and every pool signal (to ~1e-12). Rank-deficient
//                        X (e.g. a full industry-dummy set plus a market column) is
//                        handled by a complete orthogonal decomposition (min-norm LS),
//                        so the projection is still exact.
//   lowdin_orthogonalize Löwdin symmetric orthogonalization Φ' = Φ G^{-1/2} of K signal
//                        panels (G = pooled cell-level Gram / n): the orthonormal set
//                        closest to the originals in Frobenius norm, and — unlike
//                        Gram-Schmidt — independent of input order.
//   marginal_ic          mean over dates of the IC between the candidate's residual on
//                        the pool (cross-sectional OLS with intercept) and the forward
//                        return: the information the candidate adds beyond the pool.
//
//  Panels are date-major T×N spans (the SignalStore layout); NaN = missing. A cell
//  whose inputs are not all finite yields NaN in the output. Dates with too few usable
//  cells to identify the regression come out all-NaN.

#include <span>   // std::span
#include <vector> // std::vector

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, usize
#include "atx/engine/eval/hac.hpp"

namespace atx::engine::combine {

// Non-owning date-major T×N panel.
struct PanelView {
  std::span<const atx::f64> data;
  atx::usize n_dates = 0U;
  atx::usize n_instruments = 0U;
};

// Non-owning exposure cube, layout [t][i][k]. n_dates == 1 broadcasts one static
// exposure matrix to every date. n_factors == 0 means "no risk factors".
struct ExposureView {
  std::span<const atx::f64> data;
  atx::usize n_dates = 0U;
  atx::usize n_instruments = 0U;
  atx::usize n_factors = 0U;
};

// Residual of `s` on the exposures and the pool (see header). `spec_var` is either
// empty (unit weights), size N (static per-instrument specific variance) or size T·N.
// Non-positive / non-finite spec_var marks the cell unusable. Err on any shape
// mismatch.
[[nodiscard]] atx::core::Result<std::vector<atx::f64>>
residualize_signal(PanelView s, ExposureView b, std::span<const atx::f64> spec_var,
                   std::span<const PanelView> pool);

// Löwdin orthogonalization of K >= 1 same-shaped panels. Output k is Σ_j Φ_j (G^{-1/2})_jk
// on cells where every input is finite (NaN elsewhere); the pooled Gram of the
// outputs is the identity. Err on shape mismatch, < 2 complete cells, or a
// numerically singular Gram (smallest eigenvalue <= 1e-12 · largest).
[[nodiscard]] atx::core::Result<std::vector<std::vector<atx::f64>>>
lowdin_orthogonalize(std::span<const PanelView> panels);

struct MarginalIc {
  atx::f64 mean_ic = 0.0; // mean over usable dates of IC(residual_t, fwd_t)
  atx::f64 tstat = 0.0;   // versioned HAC t; 0 when inference is undefined
  atx::usize n_dates = 0U;
};

// Marginal IC of `cand` beyond `pool` against forward returns `fwd` over date rows
// [begin, end) (end == 0 → all dates). Empty pool → plain mean IC. Err on shape
// mismatch, an empty date range, or zero label_horizon. Declare the overlap horizon
// in stream dates; IidV1 restores the historical mean/(sd/sqrt(n)) statistic.
[[nodiscard]] atx::core::Result<MarginalIc> marginal_ic(PanelView cand,
                                                        std::span<const PanelView> pool,
                                                        PanelView fwd, atx::usize begin = 0U,
                                                        atx::usize end = 0U,
                         eval::hac::TStatRule tstat_rule = eval::hac::TStatRule::HorizonAwareV3,
                         atx::usize label_horizon = 1U);

} // namespace atx::engine::combine
