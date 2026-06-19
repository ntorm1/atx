// atx::engine::combine — crowding / capacity de-correlation tests (S10-4, suite
// Crowding). Proves the decorrelate_weights contract end-to-end:
//
//   1. TwoPerfectCopies_Halved (CRITICAL calibration) — two IDENTICAL non-constant
//      PnL streams (|corr| = 1) with corr_penalty = 1 ⇒ each out == w/2, and the
//      pair sums back to w (n copies ≈ one signal's worth).
//   2. ThreePerfectCopies_Thirded — three identical streams ⇒ each out == w/3.
//   3. UncorrelatedSignals_Unchanged — exactly-orthogonal streams (|corr| = 0) ⇒
//      out == input weights (crowding == 0).
//   4. Passthrough_PenaltyZeroCapZero (CRITICAL) — corr_penalty == 0 AND
//      capacity_floor == 0 ⇒ out EQUALS the input element-wise EXACT, even for
//      correlated streams.
//   5. CapacityFloor_ScalesName — capacity_floor = 1, capacity {0.5, 2.0},
//      penalty 0 ⇒ out0 == 0.5·w0 (scale 0.5) and out1 == w1 (>= floor ⇒ scale 1).
//   6. CapacityZero_ZeroesWeight — capacity[i] == 0 with a positive floor ⇒ out_i == 0.
//   7. TwoRuns_ByteIdentical — same inputs ⇒ identical output (no RNG).
//   8. CrowdingDeathTest.SizeMismatch_Aborts — a capacity/weights/pool length
//      mismatch trips the ATX_ASSERT (programmer-error precondition).
//
// Helpers build a deterministic K-alpha x T-period AlphaStore with dummy positions
// and zero AlphaMetrics (decorrelate_weights reads neither). Naming: Subject_Expected.

#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/combine/crowding.hpp" // CrowdingConfig, decorrelate_weights
#include "atx/engine/combine/metrics.hpp"  // AlphaMetrics
#include "atx/engine/combine/store.hpp"    // AlphaStore, AlphaId

namespace atxtest_combine_crowding_test {

using atx::f64;
using atx::usize;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::CrowdingConfig;
using atx::engine::combine::decorrelate_weights;

// Build a K-alpha x T-period AlphaStore. `pnl_rows[a]` is alpha a's PnL stream
// (length T); each alpha gets an all-zero dummy position cross-section (1 inst) and
// a zero AlphaMetrics{} — decorrelate_weights reads neither. Aborts the test on any
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

// A deterministic non-constant stream (rising ramp): mean-shifted so |corr| against
// a copy of itself is exactly 1 (non-degenerate variance). Length T.
[[nodiscard]] std::vector<f64> ramp(usize T) {
  std::vector<f64> s(T);
  for (usize t = 0; t < T; ++t) {
    s[t] = 0.001 + 0.0001 * static_cast<f64>(t);
  }
  return s;
}

// ---- 1: CRITICAL — two perfect copies are HALVED (n copies -> one signal) -------

TEST(Crowding, TwoPerfectCopies_Halved) {
  const usize T = 32U;
  const std::vector<f64> s = ramp(T);
  const AlphaStore pool = make_pool({s, s}); // identical streams -> |corr| = 1

  const f64 w = 0.40;
  const std::vector<f64> weights{w, w};
  const std::vector<f64> capacity{1.0, 1.0}; // irrelevant: floor disabled
  CrowdingConfig cfg;
  cfg.corr_penalty = 1.0;
  cfg.capacity_floor = 0.0;

  const std::vector<f64> out = decorrelate_weights(weights, pool, 0U, T, capacity, cfg);
  ASSERT_EQ(out.size(), 2U);
  // crowding_i = |corr| = 1, so out_i = w / (1 + 1·1) = w/2.
  EXPECT_NEAR(out[0], w / 2.0, 1e-12);
  EXPECT_NEAR(out[1], w / 2.0, 1e-12);
  // The two copies together contribute ~one signal's worth (== w, not 2w).
  EXPECT_NEAR(out[0] + out[1], w, 1e-12);
}

// ---- 2: three perfect copies are THIRDED --------------------------------------

TEST(Crowding, ThreePerfectCopies_Thirded) {
  const usize T = 32U;
  const std::vector<f64> s = ramp(T);
  const AlphaStore pool = make_pool({s, s, s}); // three identical streams

  const f64 w = 0.30;
  const std::vector<f64> weights{w, w, w};
  const std::vector<f64> capacity{1.0, 1.0, 1.0};
  CrowdingConfig cfg;
  cfg.corr_penalty = 1.0;
  cfg.capacity_floor = 0.0;

  const std::vector<f64> out = decorrelate_weights(weights, pool, 0U, T, capacity, cfg);
  ASSERT_EQ(out.size(), 3U);
  // crowding_i = |corr| to the other 2 copies = 2, so out_i = w / (1 + 1·2) = w/3.
  for (usize i = 0; i < 3U; ++i) {
    EXPECT_NEAR(out[i], w / 3.0, 1e-12) << "at i=" << i;
  }
  // Three copies collapse to ~one signal's weight.
  EXPECT_NEAR(out[0] + out[1] + out[2], w, 1e-12);
}

// ---- 3: uncorrelated signals are left unchanged -------------------------------

TEST(Crowding, UncorrelatedSignals_Unchanged) {
  // Two exactly-orthogonal mean-zero streams (T = 4): a = (+1,-1,+1,-1),
  // b = (+1,+1,-1,-1). Both mean 0; Σab = 1-1-1+1 = 0 ⇒ covariance 0 ⇒ corr EXACTLY
  // 0. Both have non-zero variance (not a degenerate window), so corr is the real 0,
  // not the degenerate-pair fallback. crowding_i == 0 ⇒ out_i == w_i.
  const std::vector<f64> a{1.0, -1.0, 1.0, -1.0};
  const std::vector<f64> b{1.0, 1.0, -1.0, -1.0};
  const AlphaStore pool = make_pool({a, b});

  const std::vector<f64> weights{0.6, 0.4};
  const std::vector<f64> capacity{1.0, 1.0};
  CrowdingConfig cfg;
  cfg.corr_penalty = 1.0;
  cfg.capacity_floor = 0.0;

  const std::vector<f64> out = decorrelate_weights(weights, pool, 0U, /*fit_end=*/4U, capacity, cfg);
  ASSERT_EQ(out.size(), 2U);
  // Exactly-zero sample correlation ⇒ crowding 0 ⇒ unchanged (within fp slack).
  EXPECT_NEAR(out[0], weights[0], 1e-12);
  EXPECT_NEAR(out[1], weights[1], 1e-12);
}

// ---- 4: CRITICAL — passthrough guard at penalty 0 AND cap floor 0 --------------

TEST(Crowding, Passthrough_PenaltyZeroCapZero) {
  const usize T = 16U;
  const std::vector<f64> s = ramp(T);
  const AlphaStore pool = make_pool({s, s}); // correlated -> would shrink if penalty > 0

  const std::vector<f64> weights{0.37, 0.91};
  const std::vector<f64> capacity{0.1, 100.0}; // arbitrary: floor disabled -> ignored
  CrowdingConfig cfg;
  cfg.corr_penalty = 0.0;   // de-correlation OFF
  cfg.capacity_floor = 0.0; // capacity scaling OFF

  const std::vector<f64> out = decorrelate_weights(weights, pool, 0U, T, capacity, cfg);
  ASSERT_EQ(out.size(), weights.size());
  // EXACT element-wise equality: no arithmetic perturbs the weight on the guard rail.
  for (usize i = 0; i < weights.size(); ++i) {
    EXPECT_EQ(out[i], weights[i]) << "passthrough must be bit-exact at i=" << i;
  }
}

// ---- 5: capacity floor scales a capacity-limited name -------------------------

TEST(Crowding, CapacityFloor_ScalesName) {
  // Orthogonal streams so crowding == 0 — the ONLY effect under test is capacity.
  const std::vector<f64> a{1.0, -1.0, 1.0, -1.0};
  const std::vector<f64> b{1.0, 1.0, -1.0, -1.0};
  const AlphaStore pool = make_pool({a, b});

  const std::vector<f64> weights{0.5, 0.5};
  const std::vector<f64> capacity{0.5, 2.0}; // name 0 below floor; name 1 above
  CrowdingConfig cfg;
  cfg.corr_penalty = 0.0;   // isolate capacity (no crowding shrink)
  cfg.capacity_floor = 1.0; // fade in over [0, 1]

  const std::vector<f64> out = decorrelate_weights(weights, pool, 0U, /*fit_end=*/4U, capacity, cfg);
  ASSERT_EQ(out.size(), 2U);
  // cap_scale_0 = clamp(0.5/1.0) = 0.5 ⇒ out0 = 0.5·w0. cap_scale_1 = clamp(2.0/1.0)=1.
  EXPECT_NEAR(out[0], 0.5 * weights[0], 1e-12);
  EXPECT_NEAR(out[1], weights[1], 1e-12);
}

// ---- 6: zero remaining capacity zeroes the weight -----------------------------

TEST(Crowding, CapacityZero_ZeroesWeight) {
  const std::vector<f64> a{1.0, -1.0, 1.0, -1.0};
  const std::vector<f64> b{1.0, 1.0, -1.0, -1.0};
  const AlphaStore pool = make_pool({a, b});

  const std::vector<f64> weights{0.5, 0.5};
  const std::vector<f64> capacity{0.0, 1.0}; // name 0 fully consumed
  CrowdingConfig cfg;
  cfg.corr_penalty = 0.0;
  cfg.capacity_floor = 1.0;

  const std::vector<f64> out = decorrelate_weights(weights, pool, 0U, /*fit_end=*/4U, capacity, cfg);
  ASSERT_EQ(out.size(), 2U);
  // cap_scale_0 = clamp(0/1) = 0 ⇒ out0 == 0 EXACTLY (dropped from the book).
  EXPECT_EQ(out[0], 0.0);
  EXPECT_NEAR(out[1], weights[1], 1e-12);
}

// ---- 7: determinism — two identical runs are byte-identical -------------------

TEST(Crowding, TwoRuns_ByteIdentical) {
  const usize T = 24U;
  const std::vector<f64> s0 = ramp(T);
  std::vector<f64> s1(T);
  for (usize t = 0; t < T; ++t) {
    s1[t] = 0.002 - 0.00005 * static_cast<f64>(t); // a falling, partly-correlated stream
  }
  const AlphaStore pool = make_pool({s0, s1});

  const std::vector<f64> weights{0.55, 0.45};
  const std::vector<f64> capacity{0.8, 1.5};
  CrowdingConfig cfg;
  cfg.corr_penalty = 1.0;
  cfg.capacity_floor = 1.0;

  const std::vector<f64> a = decorrelate_weights(weights, pool, 0U, T, capacity, cfg);
  const std::vector<f64> b = decorrelate_weights(weights, pool, 0U, T, capacity, cfg);
  ASSERT_EQ(a.size(), b.size());
  for (usize i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i], b[i]) << "decorrelate must be byte-identical at i=" << i;
  }
}

// ---- 8: death test — a size mismatch aborts (programmer-error precondition) ----

TEST(CrowdingDeathTest, SizeMismatch_Aborts) {
  const usize T = 8U;
  const std::vector<f64> s = ramp(T);
  const AlphaStore pool = make_pool({s, s}); // pool.size() == 2

  const std::vector<f64> weights{0.5, 0.5}; // correct length 2
  const std::vector<f64> capacity{1.0};     // WRONG length 1 != 2
  const CrowdingConfig cfg{};
  EXPECT_DEATH((void)decorrelate_weights(weights, pool, 0U, T, capacity, cfg), ".*");
}


}  // namespace atxtest_combine_crowding_test
