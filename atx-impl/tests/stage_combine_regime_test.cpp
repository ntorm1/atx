// stage_combine_regime_test.cpp — p8 S3-3: PIT HMM regime posterior ->
// CombineMethod::RegimeStack, the critical single-state fallback guard.
//
// fit_stack_combo(..., with_regime=true) is exercised directly against
// hand-built pools (the same technique stage_combine_stack_gate_test.cpp
// uses), so the admit/reject outcome and the regime_n_states knob are both
// under precise control.
//
// Suite: StageCombineRegime

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"

#include "stage_combine.hpp"

namespace atxtest_stage_combine_regime {

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

// Identical fixture-building technique to stage_combine_stack_gate_test.cpp:
// n_features hand-set position columns + a close series whose 1-period
// forward return recovers label_fn(cols, noise) exactly.
template <typename ColFn, typename LabelFn>
static void build_fixture(usize n_dates, usize n_inst, usize n_features, u64 seed, ColFn col_fn,
                          LabelFn label_fn, combine::AlphaStore& pool_out, std::vector<f64>& close_out) {
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
    std::vector<f64> pnl(n_dates, 0.0);
    for (usize d = 0; d < n_dates; ++d) {
      pnl[d] = 0.001 * cols_per_alpha[f][d * n_inst]; // non-degenerate PnL proxy (see gate test)
    }
    const auto r = pool_out.insert(nullptr, pnl, cols_per_alpha[f], combine::AlphaMetrics{});
    ASSERT_TRUE(r.has_value());
  }
}

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
  build_fixture(48U, 14U, 4U, seed, cols, label, pool, close_out);
  return pool;
}

static combine::AlphaStore linearly_combinable_pool(u64 seed, std::vector<f64>& close_out) {
  combine::AlphaStore pool;
  const auto cols = [](usize, usize, std::vector<f64>& c, Lcg& rng) {
    for (f64& v : c) v = rng.next();
  };
  const auto label = [](const std::vector<f64>& c, f64 noise) -> f64 {
    return 0.6 * c[0] + 0.4 * c[1] + 0.05 * noise;
  };
  build_fixture(48U, 14U, 4U, seed, cols, label, pool, close_out);
  return pool;
}

static combine::CombinerConfig base_cfg(u64 seed) {
  combine::CombinerConfig c{};
  c.method = combine::CombineMethod::RegimeStack;
  c.stack_master_seed = seed;
  c.stack_cpcv_groups = 5;
  c.stack_cpcv_test_groups = 1;
  c.stack_cpcv_embargo = 0.0;
  c.stack_horizon = 1;
  c.regime_n_states = 1; // the single-state fallback guard under test
  return c;
}

// ===========================================================================
//  regime_stack_single_state_byte_identical — the critical fallback guard.
//  n_regimes==1 must make BOTH the admit path (interaction fixture) and the
//  fallback path (linear fixture) byte-identical between with_regime=true
//  (RegimeStack) and with_regime=false (Stack) on the SAME pool/cfg.
// ===========================================================================
TEST(StageCombineRegime, SingleStateByteIdenticalOnAdmitPath) {
  std::vector<f64> close;
  const combine::AlphaStore pool = nonlinear_interaction_pool(/*seed=*/7ULL, close);
  const combine::CombinerConfig cfg = base_cfg(/*seed=*/17ULL);

  const auto r_stack = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                                  0U, pool.n_periods(), cfg, /*with_regime=*/false);
  const auto r_regime = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                                    0U, pool.n_periods(), cfg, /*with_regime=*/true);
  ASSERT_TRUE(r_stack.has_value()) << r_stack.error().message();
  ASSERT_TRUE(r_regime.has_value()) << r_regime.error().message();

  ASSERT_TRUE(r_stack->verdict.admitted) << "fixture must exercise the ADMIT path for this test to be meaningful";
  EXPECT_EQ(r_stack->verdict.admitted, r_regime->verdict.admitted);
  EXPECT_EQ(r_stack->verdict.verdict_hash, r_regime->verdict.verdict_hash)
      << "n_regimes==1 -> fit_regime_nonlinear's single partition == fit_flat_nonlinear (identical verdict)";
  ASSERT_EQ(r_stack->combo.weights.size(), r_regime->combo.weights.size());
  for (usize a = 0; a < r_stack->combo.weights.size(); ++a) {
    EXPECT_EQ(r_stack->combo.weights[a], r_regime->combo.weights[a])
        << "alpha " << a << ": deploy_nonlinear never reads regime -- admit-path weights must be bit-exact";
  }
}

TEST(StageCombineRegime, SingleStateByteIdenticalOnFallbackPath) {
  std::vector<f64> close;
  const combine::AlphaStore pool = linearly_combinable_pool(/*seed=*/1ULL, close);
  const combine::CombinerConfig cfg = base_cfg(/*seed=*/17ULL);

  const auto r_stack = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                                  0U, pool.n_periods(), cfg, /*with_regime=*/false);
  const auto r_regime = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                                    0U, pool.n_periods(), cfg, /*with_regime=*/true);
  ASSERT_TRUE(r_stack.has_value()) << r_stack.error().message();
  ASSERT_TRUE(r_regime.has_value()) << r_regime.error().message();

  ASSERT_FALSE(r_stack->verdict.admitted) << "fixture must exercise the FALLBACK path for this test to be meaningful";
  EXPECT_EQ(r_stack->verdict.admitted, r_regime->verdict.admitted);
  ASSERT_EQ(r_stack->combo.weights.size(), r_regime->combo.weights.size());
  for (usize a = 0; a < r_stack->combo.weights.size(); ++a) {
    EXPECT_EQ(r_stack->combo.weights[a], r_regime->combo.weights[a])
        << "alpha " << a << ": n_regimes==1 fit_regime_combiner+blend == plain AlphaCombiner::fit "
        << "(regime_combiner.hpp's own documented reduction) -- fallback-path weights must be bit-exact";
  }
  EXPECT_EQ(r_stack->combo.fit_begin, r_regime->combo.fit_begin);
  EXPECT_EQ(r_stack->combo.fit_end, r_regime->combo.fit_end);
}

// ===========================================================================
//  Twice-run determinism on the RegimeStack path (both arms: the HMM fit,
//  the nonlinear gate, and (on this fixture) the regime-conditional fallback).
// ===========================================================================
TEST(StageCombineRegime, TwiceRunByteIdentical) {
  std::vector<f64> close;
  const combine::AlphaStore pool = linearly_combinable_pool(/*seed=*/1ULL, close);
  combine::CombinerConfig cfg = base_cfg(/*seed=*/99ULL);
  cfg.regime_n_states = 2; // exercise a genuine >1-state fit, not the trivial reduction

  const auto r1 = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                             0U, pool.n_periods(), cfg, /*with_regime=*/true);
  const auto r2 = atx::impl::fit_stack_combo(pool, std::span<const f64>{close}, pool.n_instruments(),
                                             0U, pool.n_periods(), cfg, /*with_regime=*/true);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_EQ(r1->verdict.verdict_hash, r2->verdict.verdict_hash);
  ASSERT_EQ(r1->combo.weights.size(), r2->combo.weights.size());
  for (usize a = 0; a < r1->combo.weights.size(); ++a) {
    EXPECT_EQ(r1->combo.weights[a], r2->combo.weights[a]) << "alpha " << a;
  }
}

} // namespace atxtest_stage_combine_regime
