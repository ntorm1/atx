// W0-E0b (trial accounting): ONC-style trial clustering, the Monte-Carlo
// max-Sharpe null under the estimated correlation, and the cluster-N Deflated
// Sharpe rules (findings E-01). Suites EvalTrialClusters_*.
//
// Acceptance items proved here (plan §7 W0-E0b):
//   * EvalTrialClusters_Mc.EquicorrelatedNullFalsePositiveRateIsFivePercent —
//     an equicorrelated null (rho = 0.5, N = 2000) gives a Monte-Carlo false-
//     positive rate of 5% +- 1%.
//   * EvalTrialClusters_Onc.GBlockModelGivesNWithinTenPercentOfG — a G-block
//     model gives N = G +- 10%.
// Measured numbers are printed with a "[w0e0b]" prefix for the lane report.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/eval/deflated_sharpe.hpp"
#include "atx/engine/eval/trial_clusters.hpp"
#include "atx/engine/eval/trial_registry.hpp"

namespace atx_test_w0_e0b_trial_clusters {

using namespace atx::engine::eval;
using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;

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

std::vector<f64> noise(usize t, Gauss &g, f64 scale = 0.01) {
  std::vector<f64> v(t);
  for (f64 &x : v) {
    x = scale * g.next();
  }
  return v;
}

f64 sharpe_of(std::span<const f64> x) {
  f64 m = 0.0;
  for (const f64 v : x) {
    m += v;
  }
  m /= static_cast<f64>(x.size());
  f64 ss = 0.0;
  for (const f64 v : x) {
    ss += (v - m) * (v - m);
  }
  return m / std::sqrt(ss / static_cast<f64>(x.size() - 1U));
}

TrialRegistry must_mem(usize t, usize d) {
  TrialRegistryConfig cfg;
  cfg.pnl_len = t;
  cfg.sketch_dim = d;
  auto r = TrialRegistry::in_memory(cfg);
  EXPECT_TRUE(r.has_value());
  return std::move(*r);
}

// G blocks of unequal size m + (g % 3) - 1; within-block correlation `a`,
// zero across blocks: x = sqrt(a)·f_g + sqrt(1-a)·e.
usize record_blocks(TrialRegistry &reg, usize g_blocks, usize m, f64 a, usize t, u64 seed) {
  Gauss g{seed};
  usize n = 0U;
  for (usize b = 0; b < g_blocks; ++b) {
    const std::vector<f64> f = noise(t, g);
    const usize size = m + (b % 3U) - 1U;
    for (usize k = 0; k < size; ++k) {
      const std::vector<f64> e = noise(t, g);
      std::vector<f64> x(t);
      for (usize s = 0; s < t; ++s) {
        x[s] = std::sqrt(a) * f[s] + std::sqrt(1.0 - a) * e[s];
      }
      EXPECT_TRUE(reg.record(TrialKind::MinerExpr, n, x, sharpe_of(x)).has_value());
      ++n;
    }
  }
  return n;
}

// Equicorrelated trials: x_i = sqrt(rho)·f + sqrt(1-rho)·e_i.
void record_equicorrelated(TrialRegistry &reg, usize n, f64 rho, usize t, u64 seed) {
  Gauss g{seed};
  const std::vector<f64> f = noise(t, g);
  for (usize i = 0; i < n; ++i) {
    const std::vector<f64> e = noise(t, g);
    std::vector<f64> x(t);
    for (usize s = 0; s < t; ++s) {
      x[s] = std::sqrt(rho) * f[s] + std::sqrt(1.0 - rho) * e[s];
    }
    ASSERT_TRUE(reg.record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
  }
}

// ---------------------------------------------------------------------------
//  Acceptance: a G-block model gives N = G +- 10%.
// ---------------------------------------------------------------------------
TEST(EvalTrialClusters_Onc, GBlockModelGivesNWithinTenPercentOfG) {
  const usize t = 252U;
  struct Case {
    usize g;
    usize m;
    f64 a;
  };
  const Case cases[] = {{4U, 15U, 0.6}, {10U, 12U, 0.5}, {20U, 10U, 0.5}};
  for (const Case &c : cases) {
    TrialRegistry reg = must_mem(t, 256U);
    const usize n = record_blocks(reg, c.g, c.m, c.a, t, 1000U + c.g);
    auto acct = reg.accounting(TrialAccountingConfig{});
    ASSERT_TRUE(acct.has_value()) << acct.error().to_string();
    const f64 k = static_cast<f64>(acct->clusters.n_clusters);
    const f64 g = static_cast<f64>(c.g);
    std::printf("[w0e0b] G-block: G=%zu n=%zu rho_within=%.2f -> N_clusters=%zu "
                "(n_eff=%.2f, mean silhouette=%.3f)\n",
                c.g, n, c.a, acct->clusters.n_clusters, acct->n_eff,
                acct->clusters.mean_silhouette);
    EXPECT_EQ(acct->clusters.shape, ClusterShape::Blocks);
    EXPECT_LE(std::abs(k - g), 0.1 * g) << "G=" << c.g << " N=" << k;
    // Every block lands in one cluster (the partition is the block model).
    usize first = 0U;
    for (usize b = 0; b < c.g; ++b) {
      const usize size = c.m + (b % 3U) - 1U;
      for (usize i = first; i < first + size; ++i) {
        EXPECT_EQ(acct->clusters.labels[i], acct->clusters.labels[first]) << "block " << b;
      }
      first += size;
    }
  }
}

TEST(EvalTrialClusters_Onc, DuplicatesFormOneCluster) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t, 256U);
  Gauss g{11U};
  const std::vector<f64> base = noise(t, g);
  for (usize i = 0; i < 30U; ++i) {
    const std::vector<f64> eps = noise(t, g);
    std::vector<f64> x(t);
    for (usize s = 0; s < t; ++s) {
      x[s] = base[s] + 0.05 * eps[s]; // rho ~ 0.9975
    }
    ASSERT_TRUE(reg.record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
  }
  auto acct = reg.accounting(TrialAccountingConfig{});
  ASSERT_TRUE(acct.has_value());
  EXPECT_EQ(acct->clusters.shape, ClusterShape::OneCluster);
  EXPECT_EQ(acct->clusters.n_clusters, 1U);
  EXPECT_EQ(acct->cluster_sharpes.size(), 1U);
  EXPECT_EQ(acct->var_sr_clusters, 0.0);
}

TEST(EvalTrialClusters_Onc, IndependentTrialsAreSingletons) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t, 256U);
  Gauss g{12U};
  for (usize i = 0; i < 60U; ++i) {
    const std::vector<f64> x = noise(t, g);
    ASSERT_TRUE(reg.record(TrialKind::CombinerHyper, i, x, sharpe_of(x)).has_value());
  }
  auto acct = reg.accounting(TrialAccountingConfig{});
  ASSERT_TRUE(acct.has_value());
  EXPECT_EQ(acct->clusters.shape, ClusterShape::Singletons);
  EXPECT_EQ(acct->clusters.n_clusters, 60U);
  // Singletons: the representatives are the trials themselves.
  for (usize i = 0; i < 60U; ++i) {
    EXPECT_DOUBLE_EQ(acct->cluster_sharpes[acct->clusters.labels[i]], reg.trials()[i].sharpe);
  }
  EXPECT_NEAR(acct->var_sr_clusters, acct->var_sr, 1e-15);
}

TEST(EvalTrialClusters_Onc, EquicorrelatedTrialsHaveNoBlockStructure) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t, 256U);
  record_equicorrelated(reg, 300U, 0.5, t, 13U);
  auto acct = reg.accounting(TrialAccountingConfig{});
  ASSERT_TRUE(acct.has_value());
  std::printf("[w0e0b] equicorrelated rho=0.5 n=300: shape=%d N_clusters=%zu\n",
              static_cast<int>(acct->clusters.shape), acct->clusters.n_clusters);
  EXPECT_EQ(acct->clusters.shape, ClusterShape::Singletons);
  EXPECT_EQ(acct->clusters.n_clusters, 300U);
}

TEST(EvalTrialClusters_Onc, TwoTightClustersWithCanonicalLabels) {
  const usize t = 400U;
  TrialRegistry reg = must_mem(t, 512U);
  Gauss g{14U};
  const std::vector<f64> a = noise(t, g);
  const std::vector<f64> b = noise(t, g);
  for (usize i = 0; i < 40U; ++i) {
    const std::vector<f64> eps = noise(t, g);
    std::vector<f64> x(t);
    const std::vector<f64> &c = (i % 2U == 0U) ? a : b;
    for (usize s = 0; s < t; ++s) {
      x[s] = c[s] + 0.05 * eps[s];
    }
    ASSERT_TRUE(reg.record(TrialKind::StackHyper, i, x, sharpe_of(x)).has_value());
  }
  auto acct = reg.accounting(TrialAccountingConfig{});
  ASSERT_TRUE(acct.has_value());
  EXPECT_EQ(acct->clusters.shape, ClusterShape::Blocks);
  ASSERT_EQ(acct->clusters.n_clusters, 2U);
  for (usize i = 0; i < 40U; ++i) {
    EXPECT_EQ(acct->clusters.labels[i], static_cast<u32>(i % 2U)); // first-appearance order
  }
  EXPECT_GT(acct->clusters.mean_silhouette, 0.9);
}

TEST(EvalTrialClusters_Onc, IsDeterministicAndValidatesInput) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t, 256U);
  (void)record_blocks(reg, 6U, 8U, 0.5, t, 15U);
  auto corr = reg.correlation();
  ASSERT_TRUE(corr.has_value());
  const usize n = static_cast<usize>(reg.size());
  const auto a = onc_cluster(*corr, n, OncConfig{});
  const auto b = onc_cluster(*corr, n, OncConfig{});
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_EQ(a->labels, b->labels);
  EXPECT_EQ(a->n_clusters, b->n_clusters);
  EXPECT_EQ(a->quality, b->quality);

  const std::vector<f64> bad_shape(n * n - 1U, 0.0);
  EXPECT_FALSE(onc_cluster(bad_shape, n, OncConfig{}).has_value());
  std::vector<f64> with_nan = *corr;
  with_nan[1] = std::nan("");
  EXPECT_FALSE(onc_cluster(with_nan, n, OncConfig{}).has_value());
  OncConfig c0;
  c0.n_init = 0U;
  EXPECT_FALSE(onc_cluster(*corr, n, c0).has_value());
  OncConfig c1;
  c1.max_k = 1U;
  EXPECT_FALSE(onc_cluster(*corr, n, c1).has_value());
  OncConfig c2;
  c2.max_iter = 0U;
  EXPECT_FALSE(onc_cluster(*corr, n, c2).has_value());
  const auto empty = onc_cluster(std::vector<f64>{}, 0U, OncConfig{});
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->n_clusters, 0U);
  EXPECT_EQ(empty->shape, ClusterShape::Empty);
}

// SR_c = mean(SR_i) / sqrt(max(mean_ij R_ij, 1/m)) — checked by hand.
TEST(EvalTrialClusters_Rep, RepresentativeIsTheEqualRiskPortfolioSharpe) {
  // Trials 0,1 in cluster 0 (rho 0.5); trials 2,3 in cluster 1 (rho -0.9).
  const std::vector<f64> corr{1.0, 0.5, 0.0, 0.0, //
                              0.5, 1.0, 0.0, 0.0, //
                              0.0, 0.0, 1.0, -0.9, //
                              0.0, 0.0, -0.9, 1.0};
  TrialClusters cl;
  cl.n_clusters = 2U;
  cl.labels = {0U, 0U, 1U, 1U};
  const std::vector<f64> sr{0.10, 0.20, 0.30, -0.10};
  const auto rep = cluster_representative_sharpes(corr, 4U, sr, cl);
  ASSERT_TRUE(rep.has_value());
  ASSERT_EQ(rep->size(), 2U);
  // Cluster 0: mean SR 0.15, mean rho (1+0.5+0.5+1)/4 = 0.75.
  EXPECT_NEAR((*rep)[0], 0.15 / std::sqrt(0.75), 1e-15);
  // Cluster 1: mean SR 0.10, mean rho 0.05 floored at 1/m = 0.5.
  EXPECT_NEAR((*rep)[1], 0.10 / std::sqrt(0.5), 1e-15);
  TrialClusters bad = cl;
  bad.labels[3] = 5U;
  EXPECT_FALSE(cluster_representative_sharpes(corr, 4U, sr, bad).has_value());
}

// ---------------------------------------------------------------------------
//  Monte-Carlo null
// ---------------------------------------------------------------------------
TEST(EvalTrialClusters_Mc, IndependentTrialsMatchTheClosedFormExpectedMax) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t, 256U);
  Gauss g{21U};
  for (usize i = 0; i < 200U; ++i) {
    const std::vector<f64> x = noise(t, g);
    ASSERT_TRUE(reg.record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
  }
  auto mc = reg.mc_max_null(4000U, 7U);
  ASSERT_TRUE(mc.has_value());
  const f64 closed = expected_max_sharpe(200U, 1.0 / static_cast<f64>(t));
  std::printf("[w0e0b] independent n=200: MC E[max]=%.5f closed-form SR*=%.5f ratio=%.4f\n",
              mc->mean, closed, mc->mean / closed);
  EXPECT_NEAR(mc->mean / closed, 1.0, 0.05);
  // Quantile / CDF semantics on the sorted draws.
  EXPECT_EQ(mc->sorted_max.size(), 4000U);
  EXPECT_DOUBLE_EQ(mc->quantile(1.0), mc->sorted_max.back());
  EXPECT_DOUBLE_EQ(mc->quantile(0.0), mc->sorted_max.front());
  EXPECT_DOUBLE_EQ(mc->cdf(mc->sorted_max.back()), 1.0);
  EXPECT_NEAR(mc->cdf(mc->quantile(0.95)), 0.95, 1e-12);
  EXPECT_TRUE(std::isnan(mc->quantile(1.5)));
}

TEST(EvalTrialClusters_Mc, IdenticalTrialsHaveNoSelectionPremium) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t, 256U);
  Gauss g{22U};
  const std::vector<f64> base = noise(t, g);
  for (usize i = 0; i < 50U; ++i) {
    std::vector<f64> y(t);
    for (usize s = 0; s < t; ++s) {
      y[s] = base[s] * (1.0 + 0.01 * static_cast<f64>(i));
    }
    ASSERT_TRUE(reg.record(TrialKind::MinerExpr, i, y, sharpe_of(y)).has_value());
  }
  auto mc = reg.mc_max_null(4000U, 8U);
  ASSERT_TRUE(mc.has_value());
  // One effective draw: max = Z / sqrt(T), mean 0, sd 1/sqrt(T).
  const f64 sd = 1.0 / std::sqrt(static_cast<f64>(t));
  EXPECT_NEAR(mc->mean, 0.0, 4.0 * sd / std::sqrt(4000.0));
  EXPECT_NEAR(mc->sd, sd, 0.05 * sd);
}

TEST(EvalTrialClusters_Mc, RejectsBadInput) {
  const std::vector<f64> rows{1.0, 0.0, 0.0, 1.0};
  const std::vector<f64> sd{0.1, 0.1};
  EXPECT_TRUE(mc_max_sharpe_null(rows, 2U, 2U, sd, 10U, 1U).has_value());
  EXPECT_FALSE(mc_max_sharpe_null(rows, 2U, 2U, sd, 0U, 1U).has_value());
  EXPECT_FALSE(mc_max_sharpe_null(rows, 2U, 3U, sd, 10U, 1U).has_value());
  const std::vector<f64> bad_sd{0.1, 0.0};
  EXPECT_FALSE(mc_max_sharpe_null(rows, 2U, 2U, bad_sd, 10U, 1U).has_value());
  const TrialRegistry empty = must_mem(10U, 16U);
  EXPECT_FALSE(empty.mc_max_null(10U, 1U).has_value());
  EXPECT_FALSE(empty.accounting(TrialAccountingConfig{}).has_value());
  TrialRegistryConfig lean;
  lean.pnl_len = 10U;
  lean.keep_sketches = false;
  auto no_sketch = TrialRegistry::in_memory(lean);
  ASSERT_TRUE(no_sketch.has_value());
  Gauss g{23U};
  ASSERT_TRUE(no_sketch->record(TrialKind::MinerExpr, 1U, noise(10U, g), 0.1).has_value());
  EXPECT_FALSE(no_sketch->correlation().has_value());
  EXPECT_FALSE(no_sketch->mc_max_null(10U, 1U).has_value());
}

// ---------------------------------------------------------------------------
//  Acceptance: an equicorrelated null (rho = 0.5, N = 2000) gives a Monte-Carlo
//  false-positive rate of 5% +- 1%.
//
//  Each of 12 calibration samples draws N = 2000 null trials (true Sharpe 0,
//  pairwise rho = 0.5) over T = 252 periods, and the Monte-Carlo null of the
//  maximum Sharpe is estimated under THAT sample's correlation (the unit rows
//  are the standardized pnl, i.e. the registry's exact-mode sketches). Then
//  1000 fresh null experiments per calibration draw the 2000 trial Sharpes from
//  the TRUE model (SR_i = (sqrt(rho)·F + sqrt(1-rho)·e_i)/sqrt(T)), keep the
//  maximum, and reject when the registry-fed DSR exceeds 0.95. Calibration 0
//  also runs through a TrialRegistry, whose Monte-Carlo null must be the same
//  draws bit-for-bit (the registry path in debug builds is dominated by the
//  Eigen Gram update, so the other calibrations use the rows directly). The
//  same experiments measure the summary rules: the pre-W0 rule (V1, E-01;
//  n_eff from calibration 0) and the corrected default (V2).
// ---------------------------------------------------------------------------
// The registry's exact-mode sketch of a full-window pnl (same operation order).
std::vector<f64> unit_row(std::span<const f64> x) {
  f64 mean = 0.0;
  for (const f64 v : x) {
    mean += v;
  }
  mean /= static_cast<f64>(x.size());
  f64 ss = 0.0;
  for (const f64 v : x) {
    ss += (v - mean) * (v - mean);
  }
  const f64 inv = 1.0 / std::sqrt(ss);
  std::vector<f64> z(x.size());
  for (usize k = 0; k < x.size(); ++k) {
    z[k] = (x[k] - mean) * inv;
  }
  return z;
}

TEST(EvalTrialClusters_Mc, EquicorrelatedNullFalsePositiveRateIsFivePercent) {
  const usize n = 2000U;
  const usize t = 252U; // one year of daily pnl per trial
  const f64 rho = 0.5;
  const usize calibrations = 12U;
  const usize reps = 1000U;
  const usize draws = 2000U;
  const f64 root_t = std::sqrt(static_cast<f64>(t));
  const std::vector<f64> null_sd(n, 1.0 / std::sqrt(static_cast<f64>(t)));
  usize fp_mc = 0U;
  usize fp_v1 = 0U;
  usize fp_v2 = 0U;
  usize fp_floor = 0U; // PSR(SR*_mc): an upper bound on the default rule's FPR
  f64 n_eff0 = 0.0;
  for (usize c = 0; c < calibrations; ++c) {
    Gauss g{5000U + c};
    const std::vector<f64> f = noise(t, g);
    std::vector<f64> rows;
    rows.reserve(n * t);
    std::vector<f64> sharpes(n, 0.0);
    std::optional<TrialRegistry> reg;
    if (c == 0U) {
      reg.emplace(must_mem(t, 256U)); // exact correlation (d >= T)
    }
    for (usize i = 0; i < n; ++i) {
      const std::vector<f64> e = noise(t, g);
      std::vector<f64> x(t);
      for (usize s = 0; s < t; ++s) {
        x[s] = std::sqrt(rho) * f[s] + std::sqrt(1.0 - rho) * e[s];
      }
      sharpes[i] = sharpe_of(x);
      const std::vector<f64> z = unit_row(x);
      rows.insert(rows.end(), z.begin(), z.end());
      if (reg.has_value()) {
        ASSERT_TRUE(reg->record(TrialKind::MinerExpr, i, x, sharpes[i]).has_value());
      }
    }
    auto mc = mc_max_sharpe_null(rows, n, t, null_sd, draws, 900U + c);
    ASSERT_TRUE(mc.has_value());
    if (reg.has_value()) {
      auto via_registry = reg->mc_max_null(draws, 900U + c);
      ASSERT_TRUE(via_registry.has_value());
      EXPECT_EQ(via_registry->sorted_max, mc->sorted_max);
      n_eff0 = reg->summary().n_eff;
    }
    TrialAccounting acct;
    acct.n_raw = n;
    acct.mc = std::move(*mc);
    TrialSummary sum;
    sum.n_raw = n;
    sum.n_eff = n_eff0;
    f64 m = 0.0;
    for (const f64 v : sharpes) {
      m += v;
    }
    m /= static_cast<f64>(n);
    for (const f64 v : sharpes) {
      sum.var_sr += (v - m) * (v - m);
    }
    sum.var_sr /= static_cast<f64>(n - 1U);
    std::printf("[w0e0b] calibration %zu: MC E[max]=%.4f q95=%.4f (sigma units %.3f / %.3f); "
                "var_sr*T=%.3f\n",
                c, acct.mc.mean, acct.mc.quantile(0.95), acct.mc.mean * root_t,
                acct.mc.quantile(0.95) * root_t, sum.var_sr * static_cast<f64>(t));
    Gauss outer{77000U + c};
    for (usize r = 0; r < reps; ++r) {
      const f64 common = outer.next();
      f64 mx = -1e300;
      for (usize i = 0; i < n; ++i) {
        const f64 sr = (std::sqrt(rho) * common + std::sqrt(1.0 - rho) * outer.next()) / root_t;
        mx = std::max(mx, sr);
      }
      const RegistryDsr d =
          deflated_sharpe(mx, acct, t, 0.0, 0.0, AccountingDsrRule::MonteCarloMaxV2);
      fp_mc += d.result.dsr > 0.95 ? 1U : 0U;
      // `acct` has no clusters (SR*_cluster = 0), so the default rule's SR*
      // is exactly SR*_mc here. Any real partition can only raise SR* above
      // SR*_mc, and PSR falls as SR* rises, so this rate bounds the default
      // rule's FPR from above for every clustering outcome.
      const RegistryDsr fl = deflated_sharpe(mx, acct, t, 0.0, 0.0);
      EXPECT_EQ(fl.result.sr_star, acct.mc.mean);
      fp_floor += fl.result.dsr > 0.95 ? 1U : 0U;
      const DsrResult v1 = deflated_sharpe(mx, sum, t, 0.0, 0.0, SummaryDsrRule::NEffCrossVarV1);
      const DsrResult v2 = deflated_sharpe(mx, sum, t, 0.0, 0.0, SummaryDsrRule::RawNCrossVarV2);
      fp_v1 += v1.dsr > 0.95 ? 1U : 0U;
      fp_v2 += v2.dsr > 0.95 ? 1U : 0U;
    }
  }
  const f64 total = static_cast<f64>(calibrations * reps);
  const f64 fpr_mc = static_cast<f64>(fp_mc) / total;
  const f64 fpr_v1 = static_cast<f64>(fp_v1) / total;
  const f64 fpr_v2 = static_cast<f64>(fp_v2) / total;
  const f64 fpr_floor = static_cast<f64>(fp_floor) / total;
  std::printf("[w0e0b] equicorrelated null rho=0.5 N=2000 (n_eff=%.2f), %zu experiments: FPR "
              "MonteCarloMaxV2=%.4f, summary NEffCrossVarV1 (pre-W0)=%.4f, summary "
              "RawNCrossVarV2=%.4f\n",
              n_eff0, calibrations * reps, fpr_mc, fpr_v1, fpr_v2);
  std::printf("[w0e0b] same null: FPR bound of the default ClusterMcFloorV2 "
              "(PSR at SR*_mc)=%.4f\n",
              fpr_floor);
  EXPECT_NEAR(fpr_mc, 0.05, 0.01);
  // E-01: the pre-W0 rule double-discounts correlation and over-rejects.
  EXPECT_GT(fpr_v1, 0.25);
  // The corrected summary rule never over-rejects (PSR around E[max]).
  EXPECT_LE(fpr_v2, 0.05);
  // The default accounting rule is PSR-based: conservative, not calibrated to
  // alpha (a calibrated alpha-level selection gate uses MonteCarloMaxV2).
  EXPECT_LE(fpr_floor, 0.05);
}

// ---------------------------------------------------------------------------
//  Cluster-N DSR rules
// ---------------------------------------------------------------------------
TEST(EvalTrialClusters_Dsr, ClusterRuleUsesClusterCountAndRepresentativeVariance) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t, 256U);
  (void)record_blocks(reg, 20U, 6U, 0.95, t, 31U); // tight blocks: the cluster model holds
  auto acct = reg.accounting(TrialAccountingConfig{});
  ASSERT_TRUE(acct.has_value());
  ASSERT_EQ(acct->clusters.n_clusters, 20U);
  const f64 sr = reg.summary().max_sr;
  const RegistryDsr c = deflated_sharpe(sr, *acct, t, 0.0, 0.0, AccountingDsrRule::ClusterV2);
  EXPECT_EQ(c.n_used, 20.0);
  EXPECT_EQ(c.v_used, acct->var_sr_clusters);
  EXPECT_DOUBLE_EQ(c.result.sr_star, expected_max_sharpe(20U, acct->var_sr_clusters));
  EXPECT_DOUBLE_EQ(c.result.dsr, probabilistic_sharpe(sr, c.result.sr_star, t, 0.0, 0.0));
  // Cross-check: where the cluster model holds, the two benchmarks agree.
  const f64 ratio = c.sr_star_cluster / c.sr_star_mc;
  std::printf("[w0e0b] tight 20-block: SR*_cluster=%.5f SR*_mc=%.5f ratio=%.3f; "
              "V1 SR*=%.5f\n",
              c.sr_star_cluster, c.sr_star_mc, ratio,
              deflated_sharpe(sr, reg.summary(), t, 0.0, 0.0, SummaryDsrRule::NEffCrossVarV1)
                  .sr_star);
  EXPECT_GT(ratio, 0.65);
  EXPECT_LT(ratio, 1.35);
  // Default = cluster benchmark floored by the Monte-Carlo E[max].
  const RegistryDsr d = deflated_sharpe(sr, *acct, t, 0.0, 0.0);
  EXPECT_EQ(d.rule, AccountingDsrRule::ClusterMcFloorV2);
  EXPECT_DOUBLE_EQ(d.result.sr_star, std::max(c.sr_star_cluster, c.sr_star_mc));
  EXPECT_DOUBLE_EQ(d.result.haircut_sharpe, std::max(0.0, sr - d.result.sr_star));
}

TEST(EvalTrialClusters_Dsr, FloorTakesOverWhereClustersCannotExpressSelection) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t, 256U);
  // Tight duplicates of one series plus weakly related variants: ONC sees one
  // cluster (N = 1, SR*_cluster = 0) though selection happened over 40 trials.
  Gauss g{41U};
  const std::vector<f64> base = noise(t, g);
  for (usize i = 0; i < 40U; ++i) {
    const std::vector<f64> eps = noise(t, g);
    std::vector<f64> x(t);
    for (usize s = 0; s < t; ++s) {
      x[s] = base[s] + 0.08 * eps[s]; // rho ~ 0.994: one ONC cluster
    }
    ASSERT_TRUE(reg.record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
  }
  auto acct = reg.accounting(TrialAccountingConfig{});
  ASSERT_TRUE(acct.has_value());
  ASSERT_EQ(acct->clusters.n_clusters, 1U);
  const f64 sr = reg.summary().max_sr;
  const RegistryDsr only = deflated_sharpe(sr, *acct, t, 0.0, 0.0, AccountingDsrRule::ClusterV2);
  const RegistryDsr floor = deflated_sharpe(sr, *acct, t, 0.0, 0.0);
  EXPECT_EQ(only.result.sr_star, 0.0);
  EXPECT_GT(floor.result.sr_star, 0.0);
  EXPECT_DOUBLE_EQ(floor.result.sr_star, acct->mc.mean);
  EXPECT_LE(floor.result.dsr, only.result.dsr);
  EXPECT_EQ(floor.n_used, 40.0);
  EXPECT_TRUE(std::isnan(floor.v_used));
  // The Monte-Carlo rule's dsr is the null CDF of the maximum.
  const RegistryDsr m = deflated_sharpe(sr, *acct, t, 0.0, 0.0, AccountingDsrRule::MonteCarloMaxV2);
  EXPECT_DOUBLE_EQ(m.result.dsr, acct->mc.cdf(sr));
  EXPECT_DOUBLE_EQ(m.result.sr_star, acct->mc.mean);
}

// The ONC base stage searches k <= max_k (default min(n - 1, 64)). With more
// genuine families (G) than the cap, the partition it returns depends on how
// far G exceeds the cap (scaled down here to G = 8 so it runs in the debug
// preset):
//   * cap just below G (7): the base stage merges blocks and still passes
//     min_silhouette, so N = cap < G — the under-count. The default depth-2
//     refinement splits the merged cluster again (N = G).
//   * cap far below G (4): every capped partition mixes blocks, fails
//     min_silhouette, and ONC falls back to singletons (N = n_raw, the
//     conservative direction).
//   * raising onc.max_k to >= G recovers N = G without the refinement.
// The default rule is floored by SR*_mc whatever the partition.
TEST(EvalTrialClusters_Dsr, BaseStageCapBoundsClusterCountUntilRaised) {
  const usize t = 252U;
  const usize g_blocks = 8U;
  TrialRegistry reg = must_mem(t, 256U);
  const usize n = record_blocks(reg, g_blocks, 10U, 0.5, t, 51U);
  const f64 sr = reg.summary().max_sr;
  struct Run {
    usize max_k;
    usize max_depth;
  };
  const Run runs[] = {{7U, 0U}, {7U, 2U}, {4U, 0U}, {4U, 2U}, {16U, 0U}};
  std::vector<TrialAccounting> out;
  for (const Run &r : runs) {
    TrialAccountingConfig cfg;
    cfg.onc.max_k = r.max_k;
    cfg.onc.max_depth = r.max_depth;
    auto acct = reg.accounting(cfg);
    ASSERT_TRUE(acct.has_value()) << acct.error().to_string();
    const RegistryDsr c = deflated_sharpe(sr, *acct, t, 0.0, 0.0, AccountingDsrRule::ClusterV2);
    const RegistryDsr d = deflated_sharpe(sr, *acct, t, 0.0, 0.0);
    std::printf("[w0e0b] ONC cap: G=%zu n=%zu max_k=%zu depth=%zu -> N=%zu (shape %d) "
                "SR*_cluster=%.5f SR*_mc=%.5f default SR*=%.5f\n",
                g_blocks, n, r.max_k, r.max_depth, acct->clusters.n_clusters,
                static_cast<int>(acct->clusters.shape), c.sr_star_cluster, c.sr_star_mc,
                d.result.sr_star);
    EXPECT_GE(d.result.sr_star, c.sr_star_mc); // the floor binds for every partition
    EXPECT_GE(d.result.sr_star, c.sr_star_cluster);
    out.push_back(std::move(*acct));
  }
  // Cap just below G: base stage under-counts; the default refinement recovers.
  EXPECT_EQ(out[0].clusters.shape, ClusterShape::Blocks);
  EXPECT_LE(out[0].clusters.n_clusters, 7U);
  EXPECT_EQ(out[1].clusters.n_clusters, g_blocks);
  // Cap far below G: no capped partition passes min_silhouette -> singletons.
  EXPECT_EQ(out[2].clusters.shape, ClusterShape::Singletons);
  EXPECT_EQ(out[2].clusters.n_clusters, n);
  EXPECT_EQ(out[3].clusters.n_clusters, n);
  // Raised cap: N = G from the base stage alone.
  EXPECT_EQ(out[4].clusters.n_clusters, g_blocks);
}

} // namespace atx_test_w0_e0b_trial_clusters
