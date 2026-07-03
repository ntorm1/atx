// stage_combine_stack_gate_test.cpp — p8 S3-2: the MANDATORY admit-vs-fallback
// gate around fit_stack_combo (stage_combine.hpp/.cpp).
//
// fit_stack_combo is exercised directly against a HAND-BUILT combine::AlphaStore
// pool (bypassing the DSL/VM entirely — the same technique
// atx-engine/tests/learn/ensemble_test.cpp uses for fit_stack itself), so the
// interaction/linear structure is exact and independent of whether a real
// alpha DSL expression happens to produce it. The pool's per-alpha POSITION
// columns are hand-set; the forward-return LABEL is encoded directly into a
// synthetic close-price series (horizon=1: close[d+1] = close[d]*(1+Y), so
// build_forward_returns_window's close[d+1]/close[d]-1 recovers Y exactly).
//
// Suite: StageCombineStackGate

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"

#include "stage_combine.hpp"

namespace atxtest_stage_combine_stack_gate {

using atx::f64;
using atx::u64;
using atx::usize;
namespace combine = atx::engine::combine;

struct Lcg {
    u64 s;
    [[nodiscard]] f64 next() noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const u64 hi = s >> 11U;
        return 2.0 * (static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U)) - 1.0;
    }
};

// Fixture: n_features hand-set alpha position columns + a close-price series
// whose 1-period forward return at (d,inst) is EXACTLY label_fn(cols, noise)
// (up to float round-trip noise from the close multiply/divide). `col_fn`
// fills the n_features cells for (d,inst); `label_fn` derives Y from them + a
// per-cell noise draw. Mirrors ensemble_test.cpp's make_meta/label fixtures,
// adapted to a combine::AlphaStore pool + a real close-price array (rather
// than a hand-built FeatureMatrix), so fit_stack_combo's FULL wiring (windowed
// pool, build_forward_returns_window, the gate, the projection) is exercised.
template <typename ColFn, typename LabelFn>
static void build_gate_fixture(usize n_dates, usize n_inst, usize n_features, u64 seed,
                               ColFn col_fn, LabelFn label_fn,
                               combine::AlphaStore& pool_out, std::vector<f64>& close_out) {
  Lcg rng{seed};
  std::vector<std::vector<f64>> cols_per_alpha(n_features, std::vector<f64>(n_dates * n_inst, 0.0));
  close_out.assign(n_dates * n_inst, 100.0);
  for (usize d = 0; d < n_dates; ++d) {
    for (usize i = 0; i < n_inst; ++i) {
      std::vector<f64> cols(n_features, 0.0);
      col_fn(d, i, cols, rng);
      for (usize f = 0; f < n_features; ++f) {
        cols_per_alpha[f][d * n_inst + i] = cols[f];
      }
      const f64 noise = rng.next();
      if (d + 1U < n_dates) {
        const f64 y = label_fn(cols, noise);
        close_out[(d + 1U) * n_inst + i] = close_out[d * n_inst + i] * (1.0 + y);
      }
    }
  }
  for (usize f = 0; f < n_features; ++f) {
    // The stack path (windowed_pool/meta_features_from_pool/the projection)
    // reads ONLY positions, never pnl -- but fit_stack_combo's REJECT-path
    // fallback calls AlphaCombiner::fit, which fits ShrinkageMv on the PNL
    // covariance. An all-zero (degenerate) pnl stream makes that covariance
    // exactly singular (Ledoit-Wolf cannot rescue an already-zero matrix), so
    // the fallback -- and hence this fixture's REJECT-direction test -- would
    // fail with "not positive-definite". A small-amplitude, non-degenerate
    // per-alpha pnl proxy (instrument 0's own column value, scaled down) gives
    // ShrinkageMv a genuinely-varying covariance to shrink/solve.
    std::vector<f64> pnl(n_dates, 0.0);
    for (usize d = 0; d < n_dates; ++d) {
      pnl[d] = 0.001 * cols_per_alpha[f][d * n_inst];
    }
    const auto r = pool_out.insert(nullptr, pnl, cols_per_alpha[f], combine::AlphaMetrics{});
    ASSERT_TRUE(r.has_value()) << "pool.insert must succeed for alpha " << f;
  }
}

// A meta whose BEST predictor of Y is a LINEAR blend of the position columns:
// Y = 0.6*c0 + 0.4*c1 + 0.05*noise (no interaction term) — mirrors
// ensemble_test.cpp's linearly_combinable_meta exactly.
static combine::AlphaStore linearly_combinable_pool(u64 seed, std::vector<f64>& close_out) {
  combine::AlphaStore pool;
  const auto cols = [](usize, usize, std::vector<f64>& c, Lcg& rng) {
    for (f64& v : c) v = rng.next();
  };
  const auto label = [](const std::vector<f64>& c, f64 noise) -> f64 {
    return 0.6 * c[0] + 0.4 * c[1] + 0.05 * noise;
  };
  build_gate_fixture(/*n_dates=*/48U, /*n_inst=*/14U, /*n_features=*/4U, seed, cols, label, pool, close_out);
  return pool;
}

// A meta whose Y depends on the PRODUCT of two columns with NO marginal linear
// signal: Y = sign(c0)*sign(c1) + 0.10*noise — mirrors ensemble_test.cpp's
// nonlinear_interaction_meta exactly.
static combine::AlphaStore nonlinear_interaction_pool(u64 seed, std::vector<f64>& close_out) {
  combine::AlphaStore pool;
  const auto cols = [](usize, usize, std::vector<f64>& c, Lcg& rng) {
    for (f64& v : c) v = rng.next();
  };
  const auto label = [](const std::vector<f64>& c, f64 noise) -> f64 {
    const f64 s0 = (c[0] >= 0.0) ? 1.0 : -1.0;
    const f64 s1 = (c[1] >= 0.0) ? 1.0 : -1.0;
    return s0 * s1 + 0.10 * noise;
  };
  build_gate_fixture(/*n_dates=*/48U, /*n_inst=*/14U, /*n_features=*/4U, seed, cols, label, pool, close_out);
  return pool;
}

static combine::CombinerConfig gate_cfg(u64 seed) {
  combine::CombinerConfig c{};
  c.method = combine::CombineMethod::Stack;
  c.stack_master_seed = seed;
  c.stack_cpcv_groups = 5;
  c.stack_cpcv_test_groups = 1;
  c.stack_cpcv_embargo = 0.0;
  c.stack_horizon = 1;
  return c;
}

// ===========================================================================
//  stack_admits_on_interaction_fixture — the S3-2 MANDATORY admit direction.
// ===========================================================================
TEST(StageCombineStackGate, AdmitsOnInteractionFixture) {
  std::vector<f64> close;
  const combine::AlphaStore pool = nonlinear_interaction_pool(/*seed=*/7ULL, close);
  const combine::CombinerConfig cfg = gate_cfg(/*seed=*/17ULL);

  const auto r = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                            /*fit_begin=*/0U, /*fit_end=*/pool.n_periods(), cfg,
                                            /*regime=*/nullptr);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  EXPECT_TRUE(r->verdict.admitted)
      << "a genuine cross-alpha interaction must beat linear OOS -> admit. "
      << "nl_ic=" << r->verdict.oos_ic_nonlinear << " lin_ic=" << r->verdict.oos_ic_linear
      << " nl_dsr=" << r->verdict.oos_dsr_nonlinear;
  EXPECT_GT(r->verdict.oos_ic_nonlinear, r->verdict.oos_ic_linear);
  EXPECT_GT(r->verdict.oos_dsr_nonlinear, 0.0);

  // The shipped combo must be the STACK's projected weights (not the linear
  // fallback): well-formed (finite, Sigma|w|=1, one per pool alpha).
  ASSERT_EQ(r->combo.weights.size(), pool.n_alphas());
  f64 gross = 0.0;
  for (const f64 w : r->combo.weights) {
    EXPECT_TRUE(std::isfinite(w));
    gross += std::fabs(w);
  }
  EXPECT_NEAR(gross, 1.0, 1e-9);
}

// ===========================================================================
//  stack_rejects_on_linear_fixture — the S3-2 MANDATORY fallback direction.
//  Proves the fallback FIRES and is BYTE-IDENTICAL to the plain
//  --method shrinkage-mv (ShrinkageMv default) combo on the SAME pool/window.
// ===========================================================================
TEST(StageCombineStackGate, RejectsOnLinearFixtureAndFallsBackByteIdenticalToShrinkageMv) {
  std::vector<f64> close;
  const combine::AlphaStore pool = linearly_combinable_pool(/*seed=*/1ULL, close);
  const combine::CombinerConfig cfg = gate_cfg(/*seed=*/17ULL);

  const auto r = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                            /*fit_begin=*/0U, /*fit_end=*/pool.n_periods(), cfg,
                                            /*regime=*/nullptr);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  EXPECT_FALSE(r->verdict.admitted)
      << "a linearly-combinable pool must buy the GBT no OOS edge over linear -> reject. "
      << "nl_ic=" << r->verdict.oos_ic_nonlinear << " lin_ic=" << r->verdict.oos_ic_linear
      << " nl_dsr=" << r->verdict.oos_dsr_nonlinear;
  EXPECT_EQ(r->verdict.reason, atx::engine::learn::AdmitKind::RejectFitness);

  // The fallback combo must be BYTE-IDENTICAL to a plain ShrinkageMv fit on
  // the SAME pool/window (the fallback fires, not a differently-parameterized
  // stand-in).
  combine::AlphaCombiner plain; // default cfg: method == ShrinkageMv
  const auto expected = plain.fit(pool, 0U, pool.n_periods());
  ASSERT_TRUE(expected.has_value()) << expected.error().message();

  ASSERT_EQ(r->combo.weights.size(), expected->weights.size());
  for (usize a = 0; a < expected->weights.size(); ++a) {
    EXPECT_EQ(r->combo.weights[a], expected->weights[a]) << "alpha " << a;
  }
  EXPECT_EQ(r->combo.fit_begin, expected->fit_begin);
  EXPECT_EQ(r->combo.fit_end, expected->fit_end);
}

// ===========================================================================
//  StackCpcvConfigIsThreadedFromCombinerConfig — the wiring half of
//  "stack_gate_purges_cpcv": the CPCV correctness (purge/embargo) itself is
//  eval::cpcv_folds's own frozen-math contract (already exhaustively tested
//  in atx-engine/tests/eval/eval_cpcv_test.cpp and exercised for real by every
//  fit_stack call in ensemble_test.cpp). What S3 additionally owns is the
//  WIRING risk: that CombinerConfig's stack_cpcv_* fields actually reach
//  StackingCfg.cpcv (not silently defaulted/ignored). Proof: an embargo large
//  enough to purge every fold's train set down to near-nothing must change
//  the verdict's decided numerics vs embargo=0 on the SAME interaction fixture
//  (a no-op wire would leave both runs identical).
// ===========================================================================
TEST(StageCombineStackGate, StackCpcvConfigIsThreadedFromCombinerConfig) {
  std::vector<f64> close;
  const combine::AlphaStore pool = nonlinear_interaction_pool(/*seed=*/7ULL, close);

  combine::CombinerConfig low_embargo = gate_cfg(/*seed=*/17ULL);
  low_embargo.stack_cpcv_embargo = 0.0;
  combine::CombinerConfig high_embargo = gate_cfg(/*seed=*/17ULL);
  high_embargo.stack_cpcv_embargo = 0.9; // purges nearly the whole train set

  const auto r_low = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                                0U, pool.n_periods(), low_embargo, nullptr);
  const auto r_high = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                                 0U, pool.n_periods(), high_embargo, nullptr);
  ASSERT_TRUE(r_low.has_value()) << r_low.error().message();
  ASSERT_TRUE(r_high.has_value()) << r_high.error().message();

  EXPECT_NE(r_low->verdict.verdict_hash, r_high->verdict.verdict_hash)
      << "stack_cpcv_embargo must genuinely reach StackingCfg.cpcv -- a silently-ignored "
      << "config would leave the verdict identical regardless of embargo";
}

} // namespace atxtest_stage_combine_stack_gate
