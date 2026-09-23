#pragma once

// atx::engine::eval — combination-level weight stability.
//
// A combiner whose weights swing when a small slice of history is removed is
// fitting noise, however good its in-sample Sharpe. Two diagnostics:
//
//  * Delete-a-block jackknife (Quenouille/Tukey; blocks respect serial
//    dependence): split the T periods into G contiguous blocks, refit with
//    block g removed to get w^(g), and report per-weight
//        SE_k = √( (G−1)/G · Σ_g (w_k^(g) − w̄_k)² ).
//  * Refit turnover: ½·||w^(g) − w_full||₁, the one-way turnover a refit on
//    slightly different data would trade. Averaged over g.
//
// `fit` is any callable `std::vector<f64>(const PnlMatrix &)` returning one
// weight per series (the combiner under test). Cold path: G + 1 fits and one
// K×T scratch copy per fit.

#include <cmath>      // std::abs, std::sqrt
#include <concepts>   // std::invocable
#include <functional> // std::invoke
#include <span>
#include <vector>

#include "atx/core/error.hpp"                   // atx::core::Result
#include "atx/core/types.hpp"                   // atx::f64, atx::usize
#include "atx/engine/eval/multiple_testing.hpp" // PnlMatrix

namespace atx::engine::eval {

struct WeightStability {
  std::vector<atx::f64> full_weights; // fit on all T periods
  std::vector<atx::f64> jackknife_se; // per-weight jackknife standard error
  atx::f64 mean_se{};
  atx::f64 max_se{};
  atx::f64 mean_refit_turnover{}; // mean_g ½·||w^(g) − w_full||₁
};

// One-way turnover between two weight vectors: ½·Σ|a − b|. PRECONDITION:
// equal sizes (the shorter length is used otherwise).
[[nodiscard]] inline atx::f64 refit_turnover(std::span<const atx::f64> a,
                                             std::span<const atx::f64> b) noexcept {
  const atx::usize n = a.size() < b.size() ? a.size() : b.size();
  atx::f64 s = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    s += std::abs(a[i] - b[i]);
  }
  return 0.5 * s;
}

template <class Fit>
  requires std::invocable<Fit &, const PnlMatrix &>
[[nodiscard]] atx::core::Result<WeightStability>
jackknife_weight_stability(const PnlMatrix &pnl, atx::usize n_blocks, Fit &&fit) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  const atx::usize K = pnl.n_series;
  const atx::usize T = pnl.n_periods;
  if (K == 0U || pnl.data.size() != K * T) {
    return Err(ErrorCode::InvalidArgument, "weight_stability: data.size() != K * T");
  }
  if (n_blocks < 2U || n_blocks > T) {
    return Err(ErrorCode::InvalidArgument, "weight_stability: need 2 <= n_blocks <= T");
  }
  WeightStability out;
  out.full_weights = std::invoke(fit, pnl);
  if (out.full_weights.size() != K) {
    return Err(ErrorCode::InvalidArgument, "weight_stability: fit returned wrong size");
  }

  std::vector<std::vector<atx::f64>> ws;
  ws.reserve(n_blocks);
  std::vector<atx::f64> scratch;
  for (atx::usize g = 0; g < n_blocks; ++g) {
    // Block g = periods [g*T/G, (g+1)*T/G) (balanced integer split).
    const atx::usize b0 = g * T / n_blocks;
    const atx::usize b1 = (g + 1U) * T / n_blocks;
    const atx::usize t_keep = T - (b1 - b0);
    scratch.assign(K * t_keep, 0.0);
    for (atx::usize k = 0; k < K; ++k) {
      const std::span<const atx::f64> row = pnl.row(k);
      atx::f64 *dst = scratch.data() + k * t_keep;
      atx::usize w = 0;
      for (atx::usize t = 0; t < T; ++t) {
        if (t < b0 || t >= b1) {
          dst[w++] = row[t];
        }
      }
    }
    std::vector<atx::f64> wg = std::invoke(fit, PnlMatrix{scratch, K, t_keep});
    if (wg.size() != K) {
      return Err(ErrorCode::InvalidArgument, "weight_stability: fit returned wrong size");
    }
    out.mean_refit_turnover += refit_turnover(wg, out.full_weights);
    ws.push_back(std::move(wg));
  }
  const atx::f64 gf = static_cast<atx::f64>(n_blocks);
  out.mean_refit_turnover /= gf;

  out.jackknife_se.assign(K, 0.0);
  for (atx::usize k = 0; k < K; ++k) {
    atx::f64 mean = 0.0;
    for (const auto &wg : ws) {
      mean += wg[k];
    }
    mean /= gf;
    atx::f64 ss = 0.0;
    for (const auto &wg : ws) {
      ss += (wg[k] - mean) * (wg[k] - mean);
    }
    const atx::f64 se = std::sqrt((gf - 1.0) / gf * ss);
    out.jackknife_se[k] = se;
    out.mean_se += se;
    out.max_se = se > out.max_se ? se : out.max_se;
  }
  out.mean_se /= static_cast<atx::f64>(K);
  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::eval
