#pragma once

// atx::engine::eval — multiple-testing control plane: FDR / FWER p-value
// adjustment, Romano-Wolf stepdown, Hansen's SPA test and White's Reality Check.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  A research factory that screens thousands of candidates needs ONE set of
//  statistics for every admission decision. This header supplies them:
//
//   * p_adjust_{bh,by,holm,bonferroni} — the R `p.adjust` algorithms (same
//     ordering, same cumulative min/max, same clamp at 1), and the matching
//     rejection masks benjamini_hochberg / benjamini_yekutieli / holm.
//     BH controls FDR under independence / PRDS; BY controls FDR under ANY
//     dependence (the price is the harmonic factor c(m) = Σ 1/i); Holm controls
//     the FWER under any dependence.
//   * romano_wolf — Romano & Wolf (2005) bootstrap stepdown: FWER control that
//     exploits the cross-strategy dependence the bootstrap preserves, so it is
//     more powerful than Holm on correlated books. One-sided, H0_k:
//     E[x_k] <= 0, studentized statistics t_k = √T·x̄_k/σ̂_k.
//   * hansen_spa — Hansen (2005) Superior Predictive Ability test of
//     H0: max_k E[x_k − bench] <= 0, with the lower / consistent / upper
//     p-values, plus White's (2000) Reality Check p-value on the same draws.
//
//  Every bootstrap statistic uses the SAME stationary block bootstrap
//  (Politis & Romano 1994): geometric block lengths with mean `mean_block`,
//  uniform block starts, circular wrap. One index draw per replicate is shared
//  by all K series so the cross-sectional dependence survives resampling.
//
// ===========================================================================
//  Determinism + throughput (load-bearing)
// ===========================================================================
//  * The RNG is COUNTER-BASED (splitmix64 of (seed, replicate, draw)); there is
//    no generator state. Replicate b's blocks are a pure function of
//    (seed, b, T, mean_block), so results are bit-identical for any thread
//    count: floating-point reductions run over fixed-size replicate chunks that
//    are combined in chunk order.
//  * A replicate's mean of series k is a sum over its BLOCKS, each read in O(1)
//    from a per-series prefix sum, so one replicate costs O(K · T / mean_block)
//    rather than O(K · T). That is the whole SPA-at-scale budget
//    (1000 candidates × 2520 days × 1000 replicates).

#include <span>
#include <vector>

#include "atx/core/error.hpp" // atx::core::Result
#include "atx/core/types.hpp" // atx::f64, atx::u32, atx::u64, atx::usize

namespace atx::engine::eval {

// ===========================================================================
//  p-value adjustments (R p.adjust semantics). Inputs are p-values in [0, 1];
//  outputs are in input order. Empty in -> empty out.
// ===========================================================================
[[nodiscard]] std::vector<atx::f64> p_adjust_bh(std::span<const atx::f64> p);
[[nodiscard]] std::vector<atx::f64> p_adjust_by(std::span<const atx::f64> p);
[[nodiscard]] std::vector<atx::f64> p_adjust_holm(std::span<const atx::f64> p);
[[nodiscard]] std::vector<atx::f64> p_adjust_bonferroni(std::span<const atx::f64> p);

// Rejection masks: reject[i] == (adjusted p[i] <= level).
[[nodiscard]] std::vector<bool> benjamini_hochberg(std::span<const atx::f64> p, atx::f64 q);
[[nodiscard]] std::vector<bool> benjamini_yekutieli(std::span<const atx::f64> p, atx::f64 q);
[[nodiscard]] std::vector<bool> holm(std::span<const atx::f64> p, atx::f64 alpha);

// ===========================================================================
//  PnlMatrix — non-owning K × T view, STRATEGY-MAJOR: series k occupies
//  data[k*T, (k+1)*T). The caller keeps `data` alive for the call.
// ===========================================================================
struct PnlMatrix {
  std::span<const atx::f64> data;
  atx::usize n_series{};  // K
  atx::usize n_periods{}; // T

  // PRECONDITION: k < n_series and data.size() == n_series * n_periods.
  [[nodiscard]] std::span<const atx::f64> row(atx::usize k) const noexcept {
    return data.subspan(k * n_periods, n_periods);
  }
};

// ===========================================================================
//  BootstrapCfg — stationary block bootstrap settings.
//    n_boot     : replicates B (>= 1).
//    mean_block : expected block length L (>= 1; 1 == iid bootstrap).
//    seed       : counter-RNG key; same seed -> bit-identical results.
//    threads    : worker threads (0 or 1 == calling thread only). Results do not
//                 depend on this value.
// ===========================================================================
struct BootstrapCfg {
  atx::usize n_boot{1000};
  atx::f64 mean_block{10.0};
  atx::u64 seed{0x5eeda11ce5e50001ULL};
  atx::usize threads{1};
};

// One circular block of a stationary-bootstrap replicate: periods
// start, start+1, ..., start+len-1 (mod T).
struct BootstrapBlock {
  atx::u32 start{};
  atx::u32 len{};
};

// Fill `out` with replicate `b`'s blocks for a series of length T
// (1 <= T < 2^32). The block lengths sum to exactly T. Pure function of
// (T, cfg.mean_block, cfg.seed, b); `out` is cleared first (capacity reused).
void stationary_bootstrap_blocks(atx::usize T, const BootstrapCfg &cfg, atx::usize b,
                                 std::vector<BootstrapBlock> &out);

// ===========================================================================
//  Romano-Wolf stepdown (one-sided, studentized).
//
//    t_stat[k]     : √T · x̄_k / σ̂_k (σ̂ sample std; a flat series has t = 0).
//    p_adjusted[k] : stepdown-adjusted p-value, (1 + #exceed) / (1 + B), made
//                    monotone along the descending-t order.
//    reject[k]     : p_adjusted[k] <= alpha. FWER <= alpha asymptotically.
//
//  Err(InvalidArgument) when K == 0, T < 2, data.size() != K·T, n_boot == 0,
//  mean_block < 1, alpha not in (0, 1), or any non-finite input.
// ===========================================================================
struct RomanoWolfResult {
  std::vector<atx::f64> t_stat;
  std::vector<atx::f64> p_adjusted;
  std::vector<bool> reject;
  atx::f64 alpha{};
};

[[nodiscard]] atx::core::Result<RomanoWolfResult>
romano_wolf(const PnlMatrix &pnl, const BootstrapCfg &cfg, atx::f64 alpha);

// ===========================================================================
//  Hansen SPA + White Reality Check.
//
//  d_k,t = candidates_k,t − benchmark_t (performance differential; larger is
//  better). ω̂_k is the bootstrap standard deviation of √T·d̄*_k.
//    statistic    : T_SPA = max(0, max_k √T·d̄_k / ω̂_k).
//    p_lower      : recentring g = max(d̄, 0)            (liberal bound).
//    p_consistent : recentring g = d̄·1{√T·d̄/ω̂ >= −√(2 ln ln T)} (SPA_c).
//    p_upper      : recentring g = d̄                     (conservative bound).
//    rc_statistic : White's V = max_k √T·d̄_k (not studentized).
//    rc_pvalue    : #{V*_b >= V} / B.
//    best_index   : argmax_k of the studentized √T·d̄_k / ω̂_k.
//  p_lower <= p_consistent <= p_upper always. A series with ω̂ == 0 (a
//  constant differential) contributes nothing to the SPA maxima. When the
//  statistic is 0 (no candidate ahead of the benchmark, or every candidate
//  has ω̂ == 0) the three SPA p-values are 1: a max(0, ·) statistic of 0 is
//  never evidence against the null.
//
//  Err(InvalidArgument) on the romano_wolf shape rules, or when
//  benchmark.size() != T.
// ===========================================================================
struct SpaResult {
  atx::f64 statistic{};
  atx::f64 p_lower{};
  atx::f64 p_consistent{};
  atx::f64 p_upper{};
  atx::f64 rc_statistic{};
  atx::f64 rc_pvalue{};
  atx::usize best_index{};
};

[[nodiscard]] atx::core::Result<SpaResult> hansen_spa(const PnlMatrix &candidates,
                                                      std::span<const atx::f64> benchmark,
                                                      const BootstrapCfg &cfg);

} // namespace atx::engine::eval
