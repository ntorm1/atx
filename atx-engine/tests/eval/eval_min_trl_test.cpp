// Lane 4 (l4-mtest): Minimum Track Record Length (Bailey & López de Prado 2012),
// the Harvey-Liu (2015) multiple-testing Sharpe haircut, and combination-level
// weight stability (jackknife dispersion + refit turnover).
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/eval/haircut.hpp"
#include "atx/engine/eval/min_trl.hpp"
#include "atx/engine/eval/multiple_testing.hpp"
#include "atx/engine/eval/trial_registry.hpp"
#include "atx/engine/eval/weight_stability.hpp"

namespace atx_test_l4_mtest_min_trl {

using namespace atx::engine::eval;
using atx::f64;
using atx::usize;

// Bailey & López de Prado, "The Sharpe Ratio Efficient Frontier" (J. Risk 2012),
// §5: annualized SR 2 vs benchmark 1 at 95% under IID Normal returns needs
// 2.73 years of daily data (Fig. 8), 2.83 years weekly (Fig. 9) and 3.24 years
// monthly (Fig. 10).
f64 years_needed(f64 periods_per_year) {
  const f64 root = std::sqrt(periods_per_year);
  const f64 obs = min_track_record_length(2.0 / root, 1.0 / root, 0.0, 0.0, 0.95);
  return obs / periods_per_year;
}

TEST(EvalMinTrl, MatchesBaileyLdpPaperExamples) {
  EXPECT_NEAR(years_needed(252.0), 2.73, 0.005);
  EXPECT_NEAR(years_needed(52.0), 2.83, 0.005);
  EXPECT_NEAR(years_needed(12.0), 3.24, 0.005);
}

TEST(EvalMinTrl, FatTailsAndNegativeSkewLengthenTheRecord) {
  const f64 base = min_track_record_length(0.1, 0.0, 0.0, 0.0, 0.95);
  EXPECT_GT(min_track_record_length(0.1, 0.0, -1.0, 0.0, 0.95), base);
  EXPECT_GT(min_track_record_length(0.1, 0.0, 0.0, 5.0, 0.95), base);
  EXPECT_GT(min_track_record_length(0.1, 0.0, 0.0, 0.0, 0.99), base);
}

TEST(EvalMinTrl, UndefinedWhenSharpeDoesNotBeatBenchmark) {
  EXPECT_TRUE(std::isnan(min_track_record_length(0.05, 0.05, 0.0, 0.0, 0.95)));
  EXPECT_TRUE(std::isnan(min_track_record_length(0.01, 0.05, 0.0, 0.0, 0.95)));
  EXPECT_TRUE(std::isnan(min_track_record_length(0.1, 0.0, 0.0, 0.0, 1.0)));
}

TEST(EvalMinTrl, PsrAtMinTrlEqualsConfidence) {
  // By construction PSR(SR*) evaluated at T = MinTRL equals the confidence.
  const f64 n = min_track_record_length(0.08, 0.02, -0.3, 2.0, 0.9);
  const f64 z = (0.08 - 0.02) * std::sqrt(n - 1.0) /
                std::sqrt(1.0 + 0.3 * 0.08 + (2.0 + 2.0) / 4.0 * 0.08 * 0.08);
  EXPECT_NEAR(0.5 * std::erfc(-z / std::sqrt(2.0)), 0.9, 1e-9);
}

// Harvey & Liu, "Backtesting" (JPM 2015): annualized SR 0.75 over 240 months,
// 200 tests. Their reported adjusted Sharpe is ~0.32 (a ~57-60% haircut; the
// paper's 60% includes rounding of the intermediate p-values).
TEST(EvalHaircut, HarveyLiuWorkedExample) {
  const f64 sr_m = 0.75 / std::sqrt(12.0);
  const HaircutResult sid = haircut_sharpe(sr_m, 240U, 200.0, HaircutMethod::Sidak);
  EXPECT_NEAR(sid.p_single, 0.00079623, 1e-7);
  EXPECT_NEAR(sid.p_multiple, 0.147268, 1e-5);
  EXPECT_NEAR(sid.sr_adjusted * std::sqrt(12.0), 0.3241, 5e-4);
  EXPECT_NEAR(sid.haircut, 0.5679, 5e-4);
  const HaircutResult bon = haircut_sharpe(sr_m, 240U, 200.0, HaircutMethod::Bonferroni);
  EXPECT_NEAR(bon.p_multiple, 0.159246, 1e-5);
  EXPECT_NEAR(bon.sr_adjusted * std::sqrt(12.0), 0.3148, 5e-4);
  EXPECT_GT(bon.haircut, sid.haircut); // Bonferroni is the more conservative
}

TEST(EvalHaircut, OneTestNoHaircutAndHopelessIsFull) {
  const HaircutResult one = haircut_sharpe(0.1, 500U, 1.0, HaircutMethod::Bonferroni);
  EXPECT_NEAR(one.haircut, 0.0, 1e-9);
  EXPECT_NEAR(one.sr_adjusted, 0.1, 1e-9);
  const HaircutResult all = haircut_sharpe(0.02, 100U, 1.0e6, HaircutMethod::Bonferroni);
  EXPECT_DOUBLE_EQ(all.sr_adjusted, 0.0);
  EXPECT_DOUBLE_EQ(all.haircut, 1.0);
  const HaircutResult neg = haircut_sharpe(-0.1, 100U, 10.0, HaircutMethod::Sidak);
  EXPECT_DOUBLE_EQ(neg.haircut, 0.0);
  EXPECT_DOUBLE_EQ(neg.sr_adjusted, -0.1);
}

TEST(EvalHaircut, RegistryEffectiveCountSoftensTheHaircut) {
  TrialSummary s;
  s.n_raw = 1000U;
  s.n_eff = 40.0;
  const f64 sr = 0.12;
  const HaircutResult fed = haircut_sharpe(sr, 750U, s, HaircutMethod::Sidak);
  const HaircutResult raw = haircut_sharpe(sr, 750U, 1000.0, HaircutMethod::Sidak);
  EXPECT_LT(fed.haircut, raw.haircut);
  EXPECT_NEAR(fed.haircut, haircut_sharpe(sr, 750U, 40.0, HaircutMethod::Sidak).haircut, 1e-15);
}

// ---- weight stability -------------------------------------------------------

// Fit: weights proportional to each series' positive mean (a toy combiner).
std::vector<f64> mean_weight_fit(const PnlMatrix &m) {
  std::vector<f64> w(m.n_series, 0.0);
  f64 tot = 0.0;
  for (usize k = 0; k < m.n_series; ++k) {
    f64 mu = 0.0;
    for (const f64 v : m.row(k)) {
      mu += v;
    }
    w[k] = std::max(mu, 0.0);
    tot += w[k];
  }
  for (f64 &x : w) {
    x = tot > 0.0 ? x / tot : 0.0;
  }
  return w;
}

TEST(EvalWeightStability, ConstantSeriesGiveZeroDispersion) {
  const usize k = 3U;
  const usize t = 60U;
  std::vector<f64> x(k * t);
  for (usize i = 0; i < k; ++i) {
    for (usize s = 0; s < t; ++s) {
      x[i * t + s] = 0.01 * static_cast<f64>(i + 1U);
    }
  }
  const auto r = jackknife_weight_stability(PnlMatrix{x, k, t}, 6U, mean_weight_fit);
  ASSERT_TRUE(r.has_value());
  for (const f64 se : r->jackknife_se) {
    EXPECT_NEAR(se, 0.0, 1e-12);
  }
  EXPECT_NEAR(r->mean_refit_turnover, 0.0, 1e-12);
  EXPECT_NEAR(r->full_weights[2], 0.5, 1e-12);
}

TEST(EvalWeightStability, RegimeDependentWeightsAreUnstable) {
  const usize k = 2U;
  const usize t = 100U;
  std::vector<f64> x(k * t, 0.0);
  // Series 0 earns only in the first half, series 1 only in the second.
  for (usize s = 0; s < t; ++s) {
    x[s] = s < 50U ? 0.02 : 0.0;
    x[t + s] = s >= 50U ? 0.02 : 0.0;
  }
  const auto r = jackknife_weight_stability(PnlMatrix{x, k, t}, 10U, mean_weight_fit);
  ASSERT_TRUE(r.has_value());
  EXPECT_GT(r->max_se, 0.05);
  EXPECT_GT(r->mean_refit_turnover, 0.01);
  EXPECT_EQ(r->full_weights.size(), 2U);
}

TEST(EvalWeightStability, RefitTurnoverIsHalfL1AndShapesChecked) {
  const std::vector<f64> a{0.5, 0.5, 0.0};
  const std::vector<f64> b{0.2, 0.5, 0.3};
  EXPECT_NEAR(refit_turnover(a, b), 0.3, 1e-15);
  const std::vector<f64> x(12, 0.0);
  EXPECT_FALSE(jackknife_weight_stability(PnlMatrix{x, 2U, 6U}, 1U, mean_weight_fit).has_value());
  EXPECT_FALSE(jackknife_weight_stability(PnlMatrix{x, 2U, 6U}, 7U, mean_weight_fit).has_value());
}

} // namespace atx_test_l4_mtest_min_trl
