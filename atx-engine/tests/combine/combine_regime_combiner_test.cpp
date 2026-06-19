// atx::engine::combine — RegimeCombiner tests (S10-3, suite RegimeCombine).
//
// Proves the regime-conditioned combination contract end-to-end:
//
//   1. SingleRegime_ByteIdenticalToAlphaCombiner (CRITICAL) — the n_regimes==1
//      fallback combo equals AlphaCombiner::fit element-wise EXACT, so routing
//      through regimes never perturbs the legacy static-combo path.
//   2. Blend_ConvexHandComputed — blend({0.3,0.7})[i] == 0.3*w0[i] + 0.7*w1[i].
//   3. Blend_PosteriorExtremes — blend({1,0}) == w0, blend({0,1}) == w1, exactly.
//   4. Blend_PreservesDimension — output length == per-regime weight length.
//   5. RegimeConditioning_SegmentsHistory — masking really splits the history: the
//      regime whose periods favor alpha 0 tilts toward alpha 0, and vice versa.
//   6. PosteriorPath_TruncationInvariant — regime_posterior_at at d reads only obs
//      rows [0..d] (no look-ahead), the PIT property the blend rides on.
//   7. TwoRuns_ByteIdentical — fit + blend are deterministic (no RNG).
//   8. UnderPopulatedRegime_FallsBackToGlobal — a 0/1-period regime slot equals the
//      global AlphaCombiner combo over the full window.
//   9. RegimeCombineDeathTest.Blend_PosteriorSizeMismatch_Aborts — a misshaped
//      posterior trips the ATX_ASSERT (programmer-error precondition).
//
// Helpers build a deterministic K-alpha x T-period AlphaStore with dummy positions
// and zero AlphaMetrics (fit ignores both). Naming: Subject_Condition_Expected.

#include <span>
#include <vector>

#include <Eigen/Dense> // MatX (the HMM PIT-posterior fixture)

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, u32, usize

#include "atx/engine/combine/combiner.hpp"        // AlphaCombiner, Combination, CombinerConfig
#include "atx/engine/combine/metrics.hpp"         // AlphaMetrics
#include "atx/engine/combine/regime_combiner.hpp" // RegimeCombiner, fit_regime_combiner
#include "atx/engine/combine/store.hpp"           // AlphaStore, AlphaId
#include "atx/engine/learn/hmm.hpp"               // baum_welch, regime_posterior_at (test only)

namespace atxtest_combine_regime_combiner_test {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::combine::AlphaCombiner;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::CombinerConfig;
using atx::engine::combine::Combination;
using atx::engine::combine::CombineMethod;
using atx::engine::combine::fit_regime_combiner;
using atx::engine::combine::RegimeCombiner;
namespace learn = atx::engine::learn;

// Build a K-alpha x T-period AlphaStore. `pnl_rows[a]` is alpha a's PnL stream
// (length T); each alpha gets an all-zero dummy position cross-section (1 inst) and
// a zero AlphaMetrics{} — the combiner fit reads neither. Aborts the test on any
// insert error (a malformed fixture is a test bug).
[[nodiscard]] AlphaStore make_pool(const std::vector<std::vector<f64>> &pnl_rows) {
  AlphaStore pool;
  const usize T = pnl_rows.empty() ? 0U : pnl_rows.front().size();
  const std::vector<f64> dummy_positions(T, 0.0); // n_inst == 1, all zeros
  for (const std::vector<f64> &row : pnl_rows) {
    EXPECT_EQ(row.size(), T) << "fixture rows must share one period count";
    const auto r = pool.insert(nullptr, row, dummy_positions, AlphaMetrics{});
    EXPECT_TRUE(r.has_value()) << "fixture insert must succeed";
  }
  return pool;
}

// A deterministic 2-alpha x T pool: alpha 0 ramps up, alpha 1 ramps down — distinct,
// non-degenerate streams so the combiner produces a non-uniform fit.
[[nodiscard]] AlphaStore two_alpha_pool(usize T) {
  std::vector<f64> a0(T);
  std::vector<f64> a1(T);
  for (usize t = 0; t < T; ++t) {
    const f64 ft = static_cast<f64>(t);
    a0[t] = 0.001 + 0.0001 * ft;        // small positive, rising
    a1[t] = 0.002 - 0.00005 * ft;       // larger positive, falling
  }
  return make_pool({a0, a1});
}

// A known Combination with the given weights (window fields irrelevant to blend).
[[nodiscard]] Combination combo_with(std::vector<f64> w) {
  return Combination{std::move(w), /*fit_begin=*/0U, /*fit_end=*/1U};
}

// ---- 1: CRITICAL — single-regime fallback is byte-identical to AlphaCombiner -----

TEST(RegimeCombine, SingleRegime_ByteIdenticalToAlphaCombiner) {
  const usize T = 40U;
  const AlphaStore pool = two_alpha_pool(T);
  const CombinerConfig cfg{}; // default ShrinkageMv (no begin_is_zero dependence)
  const std::vector<u32> labels(T, 0U); // all regime 0 -> n_regimes == 1

  const auto rc = fit_regime_combiner(pool, labels, /*n_regimes=*/1U, /*fit_begin=*/0U,
                                      /*fit_end=*/T, cfg);
  ASSERT_TRUE(rc.has_value()) << "single-regime fit must succeed";
  ASSERT_EQ(rc->per_regime.size(), 1U);

  const auto direct = AlphaCombiner{cfg}.fit(pool, 0U, T);
  ASSERT_TRUE(direct.has_value());

  // The masked sub-pool over the full [0,T) window holds the SAME PnL rows the
  // direct fit reads, so the weights match bit-for-bit.
  const std::vector<f64> blended = rc->blend(std::vector<f64>{1.0});
  ASSERT_EQ(blended.size(), direct->weights.size());
  for (usize i = 0; i < blended.size(); ++i) {
    EXPECT_EQ(blended[i], direct->weights[i])
        << "single-regime blend must equal AlphaCombiner element-wise at i=" << i;
  }
}

// ---- 2: blend is a hand-checkable convex combination --------------------------

TEST(RegimeCombine, Blend_ConvexHandComputed) {
  RegimeCombiner rc;
  const std::vector<f64> w0{0.10, 0.30, 0.60};
  const std::vector<f64> w1{0.70, 0.20, 0.10};
  rc.per_regime = {combo_with(w0), combo_with(w1)};

  const std::vector<f64> out = rc.blend(std::vector<f64>{0.3, 0.7});
  ASSERT_EQ(out.size(), w0.size());
  for (usize i = 0; i < out.size(); ++i) {
    EXPECT_NEAR(out[i], 0.3 * w0[i] + 0.7 * w1[i], 1e-12) << "at i=" << i;
  }
}

// ---- 3: posterior extremes pick out one regime exactly ------------------------

TEST(RegimeCombine, Blend_PosteriorExtremes) {
  RegimeCombiner rc;
  const std::vector<f64> w0{0.10, 0.30, 0.60};
  const std::vector<f64> w1{0.70, 0.20, 0.10};
  rc.per_regime = {combo_with(w0), combo_with(w1)};

  const std::vector<f64> out0 = rc.blend(std::vector<f64>{1.0, 0.0});
  const std::vector<f64> out1 = rc.blend(std::vector<f64>{0.0, 1.0});
  ASSERT_EQ(out0.size(), w0.size());
  ASSERT_EQ(out1.size(), w1.size());
  for (usize i = 0; i < w0.size(); ++i) {
    EXPECT_EQ(out0[i], w0[i]) << "blend({1,0}) must equal w0 at i=" << i;
    EXPECT_EQ(out1[i], w1[i]) << "blend({0,1}) must equal w1 at i=" << i;
  }
}

// ---- 4: blend preserves the weight dimension ----------------------------------

TEST(RegimeCombine, Blend_PreservesDimension) {
  RegimeCombiner rc;
  const std::vector<f64> w0{0.25, 0.25, 0.25, 0.25};
  const std::vector<f64> w1{0.40, 0.30, 0.20, 0.10};
  rc.per_regime = {combo_with(w0), combo_with(w1)};

  const std::vector<f64> out = rc.blend(std::vector<f64>{0.5, 0.5});
  EXPECT_EQ(out.size(), w0.size()) << "output length must equal the per-regime weight length";
}

// ---- 5: regime-conditioning is REAL — masking segments the history ------------

TEST(RegimeCombine, RegimeConditioning_SegmentsHistory) {
  // Two alphas, T periods. In regime-0 periods alpha 0 earns a strong positive
  // mean while alpha 1 oscillates around ~0; in regime-1 periods the reverse. With
  // IcWeighted the per-regime tilt is legible (w_i ∝ max(window-sharpe_i, 0)) — a
  // high-mean, low-variance stream earns a high sharpe and dominates its regime's
  // combo. The loser oscillates (mean ≈ 0, sharpe ≈ 0 -> near-zero weight). A
  // deterministic ±1 zig-zag gives each alpha a controlled within-regime variance
  // so the window-sharpe is well-defined (not a zero-variance degenerate window).
  const usize T = 60U;
  std::vector<f64> a0(T, 0.0);
  std::vector<f64> a1(T, 0.0);
  std::vector<u32> labels(T, 0U);
  for (usize t = 0; t < T; ++t) {
    const bool reg1 = (t % 2U) == 1U;             // alternate so each regime has 30 periods
    const f64 zig = ((t / 2U) % 2U == 0U) ? 1.0 : -1.0; // ±1 within-regime wiggle
    labels[t] = reg1 ? 1U : 0U;
    if (reg1) {
      a0[t] = 0.0010 * zig;          // alpha 0 oscillates around 0 in regime 1 (loser)
      a1[t] = 0.0050 + 0.0010 * zig; // alpha 1 strong positive mean in regime 1 (winner)
    } else {
      a0[t] = 0.0050 + 0.0010 * zig; // alpha 0 strong positive mean in regime 0 (winner)
      a1[t] = 0.0010 * zig;          // alpha 1 oscillates around 0 in regime 0 (loser)
    }
  }
  const AlphaStore pool = make_pool({a0, a1});

  CombinerConfig cfg;
  cfg.method = CombineMethod::IcWeighted;
  const auto rc = fit_regime_combiner(pool, labels, /*n_regimes=*/2U, /*fit_begin=*/0U,
                                      /*fit_end=*/T, cfg);
  ASSERT_TRUE(rc.has_value());
  ASSERT_EQ(rc->per_regime.size(), 2U);

  const std::vector<f64> &w0 = rc->per_regime[0].weights; // regime 0: alpha 0 winner
  const std::vector<f64> &w1 = rc->per_regime[1].weights; // regime 1: alpha 1 winner
  ASSERT_EQ(w0.size(), 2U);
  ASSERT_EQ(w1.size(), 2U);
  EXPECT_GT(w0[0], w0[1]) << "regime-0 combo must tilt toward alpha 0 (its winner)";
  EXPECT_GT(w1[1], w1[0]) << "regime-1 combo must tilt toward alpha 1 (its winner)";
}

// ---- 6: PIT — the posterior path is truncation-invariant (no look-ahead) -------

TEST(RegimeCombine, PosteriorPath_TruncationInvariant) {
  // A tiny planted 2-regime, 1-dim series: alternating runs of mean 0 and mean +3.
  const usize T = 120U;
  MatX obs(static_cast<Eigen::Index>(T), 1);
  for (usize t = 0; t < T; ++t) {
    const bool high = ((t / 20U) % 2U) == 1U;
    obs(static_cast<Eigen::Index>(t), 0) = high ? 3.0 : 0.0;
  }
  learn::HmmCfg hcfg;
  hcfg.n_states = 2U;
  hcfg.max_iter = 50U;
  hcfg.master_seed = 42ULL;
  const learn::Hmm fitted = learn::baum_welch(obs, hcfg);

  // Truncated copy with exactly d+1 rows.
  const usize d = 79U;
  MatX truncated(static_cast<Eigen::Index>(d + 1), obs.cols());
  for (usize t = 0; t <= d; ++t) {
    for (Eigen::Index j = 0; j < obs.cols(); ++j) {
      truncated(static_cast<Eigen::Index>(t), j) = obs(static_cast<Eigen::Index>(t), j);
    }
  }
  const VecX p_full = learn::regime_posterior_at(fitted, obs, d);
  const VecX p_trunc = learn::regime_posterior_at(fitted, truncated, d);

  ASSERT_EQ(p_full.size(), p_trunc.size());
  // Bit-for-bit: the PIT posterior at d depends ONLY on obs rows [0..d], so the
  // 80..119 rows cannot change a single bit — no look-ahead in the blend's input.
  for (Eigen::Index s = 0; s < p_full.size(); ++s) {
    EXPECT_EQ(p_full(s), p_trunc(s)) << "PIT posterior must be invariant to obs rows > d at s=" << s;
  }
}

// ---- 7: determinism — two identical fits/blends are byte-identical -------------

TEST(RegimeCombine, TwoRuns_ByteIdentical) {
  const usize T = 50U;
  const AlphaStore pool = two_alpha_pool(T);
  std::vector<u32> labels(T, 0U);
  for (usize t = 0; t < T; ++t) {
    labels[t] = (t < T / 2U) ? 0U : 1U; // two contiguous regimes, each >= 2 periods
  }
  const CombinerConfig cfg{};

  const auto rc_a = fit_regime_combiner(pool, labels, 2U, 0U, T, cfg);
  const auto rc_b = fit_regime_combiner(pool, labels, 2U, 0U, T, cfg);
  ASSERT_TRUE(rc_a.has_value());
  ASSERT_TRUE(rc_b.has_value());
  ASSERT_EQ(rc_a->per_regime.size(), rc_b->per_regime.size());
  for (usize r = 0; r < rc_a->per_regime.size(); ++r) {
    const std::vector<f64> &wa = rc_a->per_regime[r].weights;
    const std::vector<f64> &wb = rc_b->per_regime[r].weights;
    ASSERT_EQ(wa.size(), wb.size());
    for (usize i = 0; i < wa.size(); ++i) {
      EXPECT_EQ(wa[i], wb[i]) << "fit must be byte-identical at r=" << r << " i=" << i;
    }
  }

  const std::vector<f64> post{0.4, 0.6};
  const std::vector<f64> b_a = rc_a->blend(post);
  const std::vector<f64> b_b = rc_b->blend(post);
  ASSERT_EQ(b_a.size(), b_b.size());
  for (usize i = 0; i < b_a.size(); ++i) {
    EXPECT_EQ(b_a[i], b_b[i]) << "blend must be byte-identical at i=" << i;
  }
}

// ---- 8: under-populated regime falls back to the global combo ------------------

TEST(RegimeCombine, UnderPopulatedRegime_FallsBackToGlobal) {
  // Three regimes, but regime 2 gets exactly ONE period (< 2) so it must fall back
  // to the global AlphaCombiner combo over the full window.
  const usize T = 30U;
  const AlphaStore pool = two_alpha_pool(T);
  std::vector<u32> labels(T, 0U);
  for (usize t = 0; t < T; ++t) {
    labels[t] = (t < T / 2U) ? 0U : 1U;
  }
  labels[T - 1U] = 2U; // exactly one period in regime 2

  const CombinerConfig cfg{};
  const auto rc = fit_regime_combiner(pool, labels, /*n_regimes=*/3U, /*fit_begin=*/0U,
                                      /*fit_end=*/T, cfg);
  ASSERT_TRUE(rc.has_value());
  ASSERT_EQ(rc->per_regime.size(), 3U);

  const auto global = AlphaCombiner{cfg}.fit(pool, 0U, T);
  ASSERT_TRUE(global.has_value());
  const std::vector<f64> &w2 = rc->per_regime[2].weights;
  ASSERT_EQ(w2.size(), global->weights.size());
  for (usize i = 0; i < w2.size(); ++i) {
    EXPECT_EQ(w2[i], global->weights[i])
        << "under-populated regime must equal the global combo at i=" << i;
  }
}

// ---- 9: death test — misshaped posterior aborts (programmer-error precondition) -

TEST(RegimeCombineDeathTest, Blend_PosteriorSizeMismatch_Aborts) {
  RegimeCombiner rc;
  rc.per_regime = {combo_with({0.5, 0.5}), combo_with({0.3, 0.7})};
  const std::vector<f64> wrong_size{1.0}; // size 1 != per_regime.size() 2
  EXPECT_DEATH((void)rc.blend(wrong_size), ".*");
}


}  // namespace atxtest_combine_regime_combiner_test
