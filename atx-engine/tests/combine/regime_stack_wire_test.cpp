// regime_stack_wire_test.cpp — p8 S3-3: the PIT HMM posterior wired end-to-end
// through combine::fit_regime_combiner + RegimeCombiner::blend (the linear
// regime-conditional path RegimeStack's fallback arm reuses — see
// atx-impl/src/stage_combine.cpp's fit_stack_combo).
//
// combine_regime_combiner_test.cpp already proves: (1) the n_regimes==1
// byte-identical fallback, (2) the blend arithmetic in isolation, (3) that
// hand-supplied regime labels segment history the way a caller expects, and
// (4) regime_posterior_at's own PIT truncation-invariance. What none of those
// tests exercise is the FULL composition this sprint adds: a REAL baum_welch
// fit -> PIT-argmax labels -> fit_regime_combiner -> per-date
// regime_posterior_at -> RegimeCombiner::blend, scored OUT-OF-SAMPLE against
// a single flat AlphaCombiner fit on the SAME data. That composition is what
// "the PIT-posterior-blended book tracks the regime-appropriate combo per
// date, and its OOS score beats a single flat fit" (the sprint's own accept
// criterion) actually claims.
//
// Suite: RegimeStackWire

#include <cmath>
#include <iostream>
#include <span>
#include <vector>

#include <Eigen/Dense>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/regime_combiner.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/learn/hmm.hpp"

namespace atxtest_regime_stack_wire {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
namespace combine = atx::engine::combine;
namespace learn = atx::engine::learn;

// The mean/pop-std of a span (a minimal local Sharpe-like scorer — this test
// does not need the engine's full eval::compute_return_metrics machinery).
struct MeanStd {
  f64 mean;
  f64 std;
};
[[nodiscard]] static MeanStd mean_std(std::span<const f64> xs) {
  f64 sum = 0.0;
  for (const f64 x : xs) sum += x;
  const f64 mean = sum / static_cast<f64>(xs.size());
  f64 ss = 0.0;
  for (const f64 x : xs) ss += (x - mean) * (x - mean);
  return MeanStd{mean, std::sqrt(ss / static_cast<f64>(xs.size()))};
}

TEST(RegimeStackWire, PitPosteriorBlendBeatsFlatFitOutOfSample) {
  // T=120: 20-period runs alternating regime 0/1 -> [0-19]=0,[20-39]=1,
  // [40-59]=0,[60-79]=1 (TRAIN, 2 runs each), [80-99]=0,[100-119]=1 (TEST, one
  // run each — both regimes represented OOS). Deterministic ±1 zigzag (period
  // 2) gives each alpha real within-regime variance (a well-defined
  // window-sharpe, not a degenerate zero-variance stream).
  constexpr usize T = 120U;
  constexpr usize kRun = 20U;
  constexpr usize kTrainEnd = 80U;
  const auto regime_of = [](usize t) -> u32 { return static_cast<u32>((t / kRun) % 2U); };

  std::vector<f64> a0(T), a1(T);
  for (usize t = 0; t < T; ++t) {
    const f64 zig = ((t / 2U) % 2U == 0U) ? 1.0 : -1.0;
    if (regime_of(t) == 0U) {
      a0[t] = 0.0050 + 0.0010 * zig; // alpha 0 wins in regime 0
      a1[t] = 0.0010 * zig;          // alpha 1 oscillates around 0
    } else {
      a0[t] = 0.0010 * zig;
      a1[t] = 0.0050 + 0.0010 * zig; // alpha 1 wins in regime 1
    }
  }
  combine::AlphaStore pool;
  const std::vector<f64> dummy_pos(T, 0.0); // 1 instrument, unused by the fit
  ASSERT_TRUE(pool.insert(nullptr, a0, dummy_pos, combine::AlphaMetrics{}).has_value());
  ASSERT_TRUE(pool.insert(nullptr, a1, dummy_pos, combine::AlphaMetrics{}).has_value());

  // The regime marker observable (SEPARATE from the alphas — a clean regime
  // indicator, mirroring PosteriorPath_TruncationInvariant's fixture): high
  // (3.0) in regime 1, low (0.0) in regime 0, over the FULL 120 dates.
  MatX obs_full(static_cast<Eigen::Index>(T), 1);
  for (usize t = 0; t < T; ++t) {
    obs_full(static_cast<Eigen::Index>(t), 0) = (regime_of(t) == 1U) ? 3.0 : 0.0;
  }
  // Fit the HMM on TRAIN ONLY (PIT: the model itself never sees test rows).
  MatX obs_train(static_cast<Eigen::Index>(kTrainEnd), 1);
  for (usize t = 0; t < kTrainEnd; ++t) obs_train(static_cast<Eigen::Index>(t), 0) = obs_full(static_cast<Eigen::Index>(t), 0);
  learn::HmmCfg hcfg;
  hcfg.n_states = 2U;
  hcfg.max_iter = 50U;
  hcfg.master_seed = 42ULL;
  const learn::Hmm hmm = learn::baum_welch(obs_train, hcfg);

  // PIT-argmax labels over TRAIN (the window fit_regime_combiner is fit on).
  std::vector<u32> labels(T, 0U);
  for (usize d = 0; d < kTrainEnd; ++d) {
    const VecX post = learn::regime_posterior_at(hmm, obs_full, d);
    f64 best = post(0);
    u32 arg = 0U;
    for (Eigen::Index s = 1; s < post.size(); ++s) {
      if (post(s) > best) { best = post(s); arg = static_cast<u32>(s); }
    }
    labels[d] = arg;
  }

  combine::CombinerConfig cfg{};
  cfg.method = combine::CombineMethod::IcWeighted; // legible per-regime tilt (window-sharpe based)
  const auto rc_r = combine::fit_regime_combiner(pool, std::span<const u32>{labels}, /*n_regimes=*/2U,
                                                 /*fit_begin=*/0U, /*fit_end=*/kTrainEnd, cfg);
  ASSERT_TRUE(rc_r.has_value()) << rc_r.error().message();

  combine::AlphaCombiner flat_combiner;
  flat_combiner.cfg = cfg;
  const auto flat_r = flat_combiner.fit(pool, 0U, kTrainEnd);
  ASSERT_TRUE(flat_r.has_value()) << flat_r.error().message();

  // Score OOS [kTrainEnd, T): per-date PIT posterior blend vs the static flat combo.
  std::vector<f64> pnl_regime, pnl_flat;
  pnl_regime.reserve(T - kTrainEnd);
  pnl_flat.reserve(T - kTrainEnd);
  for (usize d = kTrainEnd; d < T; ++d) {
    const VecX post = learn::regime_posterior_at(hmm, obs_full, d); // PIT: reads obs[0..d] only
    std::vector<f64> post_vec(static_cast<usize>(post.size()));
    for (Eigen::Index s = 0; s < post.size(); ++s) post_vec[static_cast<usize>(s)] = post(s);
    const std::vector<f64> blended = rc_r->blend(std::span<const f64>{post_vec});

    const f64 a0d = pool.pnl(combine::AlphaId{0U})[d];
    const f64 a1d = pool.pnl(combine::AlphaId{1U})[d];
    pnl_regime.push_back(blended[0] * a0d + blended[1] * a1d);
    pnl_flat.push_back(flat_r->weights[0] * a0d + flat_r->weights[1] * a1d);
  }

  const MeanStd ms_regime = mean_std(std::span<const f64>{pnl_regime});
  const MeanStd ms_flat = mean_std(std::span<const f64>{pnl_flat});
  const f64 sharpe_regime = (ms_regime.std > 0.0) ? ms_regime.mean / ms_regime.std : 0.0;
  const f64 sharpe_flat = (ms_flat.std > 0.0) ? ms_flat.mean / ms_flat.std : 0.0;

  std::cout << "[RegimeStackWire] oos_mean_regime=" << ms_regime.mean
            << " oos_mean_flat=" << ms_flat.mean << " oos_sharpe_regime=" << sharpe_regime
            << " oos_sharpe_flat=" << sharpe_flat << "\n";

  EXPECT_GT(ms_regime.mean, ms_flat.mean)
      << "the PIT-posterior-blended OOS mean return must beat the flat fit: "
      << "regime_mean=" << ms_regime.mean << " flat_mean=" << ms_flat.mean;
  EXPECT_GT(sharpe_regime, sharpe_flat)
      << "regime_sharpe=" << sharpe_regime << " flat_sharpe=" << sharpe_flat;
}

} // namespace atxtest_regime_stack_wire
