// Lane 4 (l4-mtest): multiple-testing control plane — FDR / FWER p-value
// adjustments, Romano-Wolf stepdown, Hansen SPA + White Reality Check, and the
// seeded stationary block bootstrap they share.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/eval/multiple_testing.hpp"

namespace atx_test_l4_mtest_multiple_testing {

using namespace atx::engine::eval;
using atx::f64;
using atx::u64;
using atx::usize;

// Deterministic N(0,1) stream (splitmix64 + Box-Muller) — independent of the
// code under test's own counter RNG so the fixtures are not self-referential.
class Gauss {
public:
  explicit Gauss(u64 seed) : s_{seed} {}
  f64 next() {
    const f64 u1 = (static_cast<f64>(mix() >> 11U) + 0.5) * 0x1.0p-53;
    const f64 u2 = (static_cast<f64>(mix() >> 11U) + 0.5) * 0x1.0p-53;
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

private:
  u64 mix() {
    u64 z = (s_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
  }
  u64 s_;
};

// K x T strategy-major matrix of N(mu_k, sigma^2) returns.
std::vector<f64> gaussian_panel(usize k, usize t, u64 seed, const std::vector<f64> &mu,
                                f64 sigma) {
  Gauss g{seed};
  std::vector<f64> out(k * t);
  for (usize i = 0; i < k; ++i) {
    for (usize j = 0; j < t; ++j) {
      out[i * t + j] = mu[i] + sigma * g.next();
    }
  }
  return out;
}

const std::vector<f64> kP{0.01, 0.04, 0.03, 0.005, 0.2, 0.5, 0.04, 0.001, 0.7, 0.02};

// Values produced by the R p.adjust reference algorithm
// (pmin(1, cummin(n/i * p[o]))[ro] etc.) for kP.
TEST(EvalFdr, BhMatchesRPadjust) {
  const std::vector<f64> want{0.033333333333333333, 0.057142857142857148, 0.057142857142857148,
                              0.025000000000000001, 0.25,                 0.55555555555555558,
                              0.057142857142857148, 0.01,                 0.69999999999999996,
                              0.050000000000000003};
  const std::vector<f64> got = p_adjust_bh(kP);
  ASSERT_EQ(got.size(), want.size());
  for (usize i = 0; i < want.size(); ++i) {
    EXPECT_NEAR(got[i], want[i], 1e-15) << i;
  }
}

TEST(EvalFdr, ByMatchesRPadjust) {
  const std::vector<f64> want{0.097632275132275126, 0.16736961451247165, 0.16736961451247165,
                              0.073224206349206344, 0.73224206349206344, 1.0,
                              0.16736961451247165,  0.029289682539682539, 1.0,
                              0.14644841269841269};
  const std::vector<f64> got = p_adjust_by(kP);
  ASSERT_EQ(got.size(), want.size());
  for (usize i = 0; i < want.size(); ++i) {
    EXPECT_NEAR(got[i], want[i], 1e-14) << i;
  }
}

TEST(EvalFdr, HolmMatchesRPadjust) {
  const std::vector<f64> want{0.08, 0.2, 0.18, 0.045, 0.6, 1.0, 0.2, 0.01, 1.0, 0.14};
  const std::vector<f64> got = p_adjust_holm(kP);
  ASSERT_EQ(got.size(), want.size());
  for (usize i = 0; i < want.size(); ++i) {
    EXPECT_NEAR(got[i], want[i], 1e-14) << i;
  }
}

TEST(EvalFdr, RejectionMasksFollowAdjustedPValues) {
  const std::vector<bool> bh = benjamini_hochberg(kP, 0.051);
  const std::vector<bool> by = benjamini_yekutieli(kP, 0.05);
  const std::vector<bool> hm = holm(kP, 0.05);
  const std::vector<bool> bh_want{true, false, false, true, false,
                                  false, false, true, false, true};
  const std::vector<bool> by_want{false, false, false, false, false,
                                  false, false, true, false, false};
  const std::vector<bool> hm_want{false, false, false, true, false,
                                  false, false, true, false, false};
  EXPECT_EQ(bh, bh_want);
  EXPECT_EQ(by, by_want);
  EXPECT_EQ(hm, hm_want);
}

TEST(EvalFdr, EmptyInputGivesEmptyOutput) {
  EXPECT_TRUE(p_adjust_bh({}).empty());
  EXPECT_TRUE(benjamini_yekutieli({}, 0.1).empty());
}

TEST(EvalBootstrap, StationaryBlocksCoverExactlyTPeriods) {
  BootstrapCfg cfg;
  cfg.mean_block = 7.0;
  cfg.seed = 42U;
  std::vector<BootstrapBlock> blocks;
  f64 total_blocks = 0.0;
  const usize n_rep = 400U;
  for (usize b = 0; b < n_rep; ++b) {
    stationary_bootstrap_blocks(500U, cfg, b, blocks);
    usize covered = 0;
    for (const BootstrapBlock &blk : blocks) {
      ASSERT_LT(blk.start, 500U);
      ASSERT_GE(blk.len, 1U);
      covered += blk.len;
    }
    EXPECT_EQ(covered, 500U);
    total_blocks += static_cast<f64>(blocks.size());
  }
  // Geometric block lengths with mean 7 -> about 500/7 blocks per replicate.
  const f64 mean_blocks = total_blocks / static_cast<f64>(n_rep);
  EXPECT_NEAR(mean_blocks, 500.0 / 7.0, 3.0);
}

TEST(EvalRomanoWolf, DeterministicUnderFixedSeed) {
  const usize k = 8U;
  const usize t = 300U;
  const std::vector<f64> mu{0.0, 0.0, 0.2, 0.0, 0.0, 0.0, 0.0, 0.0};
  const std::vector<f64> x = gaussian_panel(k, t, 7U, mu, 1.0);
  const PnlMatrix m{x, k, t};
  BootstrapCfg cfg;
  cfg.n_boot = 300U;
  cfg.seed = 99U;
  const auto a = romano_wolf(m, cfg, 0.05);
  cfg.threads = 3U; // thread count must not change a single bit
  const auto b = romano_wolf(m, cfg, 0.05);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(a->p_adjusted, b->p_adjusted);
  EXPECT_EQ(a->t_stat, b->t_stat);
  cfg.seed = 100U;
  const auto c = romano_wolf(m, cfg, 0.05);
  ASSERT_TRUE(c.has_value());
  EXPECT_NE(a->p_adjusted, c->p_adjusted);
}

TEST(EvalRomanoWolf, DetectsTheSingleRealEdge) {
  const usize k = 20U;
  const usize t = 500U;
  std::vector<f64> mu(k, 0.0);
  mu[5] = 0.4; // E[t] ~ 0.4 * sqrt(500) ~ 8.9
  const std::vector<f64> x = gaussian_panel(k, t, 11U, mu, 1.0);
  BootstrapCfg cfg;
  cfg.n_boot = 500U;
  const auto r = romano_wolf(PnlMatrix{x, k, t}, cfg, 0.05);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->reject[5]);
  EXPECT_LT(r->p_adjusted[5], 0.01);
  for (usize i = 0; i < k; ++i) {
    EXPECT_GE(r->p_adjusted[i], 0.0);
    EXPECT_LE(r->p_adjusted[i], 1.0);
  }
}

TEST(EvalRomanoWolf, NullFwerWithinAlphaPlusMcError) {
  const usize k = 10U;
  const usize t = 200U;
  const usize n_mc = 200U;
  const f64 alpha = 0.10;
  const std::vector<f64> mu(k, 0.0);
  usize any_reject = 0;
  BootstrapCfg cfg;
  cfg.n_boot = 199U;
  cfg.mean_block = 1.0; // iid data
  for (usize r = 0; r < n_mc; ++r) {
    const std::vector<f64> x = gaussian_panel(k, t, 1000U + r, mu, 1.0);
    cfg.seed = 5000U + r;
    const auto res = romano_wolf(PnlMatrix{x, k, t}, cfg, alpha);
    ASSERT_TRUE(res.has_value());
    bool any = false;
    for (const bool rej : res->reject) {
      any = any || rej;
    }
    any_reject += any ? 1U : 0U;
  }
  const f64 fwer = static_cast<f64>(any_reject) / static_cast<f64>(n_mc);
  const f64 mc_se = std::sqrt(alpha * (1.0 - alpha) / static_cast<f64>(n_mc));
  EXPECT_LE(fwer, alpha + 3.0 * mc_se) << "FWER " << fwer;
}

TEST(EvalRomanoWolf, RejectsBadShape) {
  const std::vector<f64> x(10, 0.0);
  EXPECT_FALSE(romano_wolf(PnlMatrix{x, 3U, 4U}, BootstrapCfg{}, 0.05).has_value());
  BootstrapCfg bad;
  bad.n_boot = 0U;
  EXPECT_FALSE(romano_wolf(PnlMatrix{x, 2U, 5U}, bad, 0.05).has_value());
  EXPECT_FALSE(romano_wolf(PnlMatrix{x, 2U, 5U}, BootstrapCfg{}, 1.5).has_value());
}

TEST(EvalSpa, NullOfNoSuperiorModelIsNotRejected) {
  const usize k = 30U;
  const usize t = 400U;
  const std::vector<f64> x = gaussian_panel(k, t, 21U, std::vector<f64>(k, 0.0), 1.0);
  const std::vector<f64> bench(t, 0.0);
  BootstrapCfg cfg;
  cfg.n_boot = 500U;
  const auto r = hansen_spa(PnlMatrix{x, k, t}, bench, cfg);
  ASSERT_TRUE(r.has_value());
  EXPECT_GT(r->p_consistent, 0.05);
  EXPECT_GT(r->rc_pvalue, 0.05);
  EXPECT_LE(r->p_lower, r->p_consistent + 1e-12);
  EXPECT_LE(r->p_consistent, r->p_upper + 1e-12);
}

TEST(EvalSpa, SuperiorModelIsFoundAndSpaIsRobustToPoorModels) {
  const usize t = 500U;
  // One genuinely superior model (mean 0.2 above the benchmark) plus 60 poor
  // ones (mean -0.3). White's RC is dragged toward acceptance by the poor models
  // (they are recentred at their own negative means); the consistent SPA test
  // discards them, so its p-value must be no larger than RC's.
  const usize k = 61U;
  std::vector<f64> mu(k, -0.3);
  mu[0] = 0.2;
  const std::vector<f64> x = gaussian_panel(k, t, 31U, mu, 1.0);
  const std::vector<f64> bench(t, 0.0);
  BootstrapCfg cfg;
  cfg.n_boot = 1000U;
  const auto r = hansen_spa(PnlMatrix{x, k, t}, bench, cfg);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->best_index, 0U);
  EXPECT_LT(r->p_consistent, 0.01);
  EXPECT_LE(r->p_consistent, r->rc_pvalue + 1e-12);
  EXPECT_GT(r->statistic, 3.0);
}

TEST(EvalSpa, DeterministicAndBenchmarkLengthChecked) {
  const usize k = 5U;
  const usize t = 120U;
  const std::vector<f64> x = gaussian_panel(k, t, 41U, std::vector<f64>(k, 0.05), 1.0);
  const std::vector<f64> bench(t, 0.0);
  BootstrapCfg cfg;
  cfg.n_boot = 200U;
  const auto a = hansen_spa(PnlMatrix{x, k, t}, bench, cfg);
  cfg.threads = 4U;
  const auto b = hansen_spa(PnlMatrix{x, k, t}, bench, cfg);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(a->p_consistent, b->p_consistent);
  EXPECT_EQ(a->p_lower, b->p_lower);
  EXPECT_EQ(a->p_upper, b->p_upper);
  EXPECT_EQ(a->rc_pvalue, b->rc_pvalue);
  EXPECT_EQ(a->statistic, b->statistic);
  const std::vector<f64> short_bench(t - 1U, 0.0);
  EXPECT_FALSE(hansen_spa(PnlMatrix{x, k, t}, short_bench, cfg).has_value());
}

// Degenerate input (review finding): every candidate identical to the benchmark
// (all-zero pnl) has zero bootstrap variance, so the statistic is 0 — the null
// of no superiority must NOT be rejected (p = 1, not 0).
TEST(EvalSpa, DegenerateZeroStatisticNeverRejects) {
  const usize k = 3U;
  const usize t = 100U;
  const std::vector<f64> x(k * t, 0.0);
  const std::vector<f64> bench(t, 0.0);
  BootstrapCfg cfg;
  cfg.n_boot = 200U;
  const auto r = hansen_spa(PnlMatrix{x, k, t}, bench, cfg);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->statistic, 0.0);
  EXPECT_EQ(r->p_lower, 1.0);
  EXPECT_EQ(r->p_consistent, 1.0);
  EXPECT_EQ(r->p_upper, 1.0);
  EXPECT_EQ(r->rc_pvalue, 1.0);
  // Candidates that are the benchmark plus a constant shortfall: also statistic 0.
  std::vector<f64> y = gaussian_panel(1U, t, 51U, std::vector<f64>(1U, 0.0), 1.0);
  std::vector<f64> ys(k * t);
  for (usize j = 0; j < k; ++j) {
    for (usize s = 0; s < t; ++s) {
      ys[j * t + s] = y[s] - 0.1;
    }
  }
  const auto r2 = hansen_spa(PnlMatrix{ys, k, t}, y, cfg);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2->p_consistent, 1.0);
  EXPECT_EQ(r2->p_lower, 1.0);
}

} // namespace atx_test_l4_mtest_multiple_testing
