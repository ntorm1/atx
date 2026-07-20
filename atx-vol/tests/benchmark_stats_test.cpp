// WS-X-B / X5 — benchmark-relative statistics (IR, alpha, beta, tracking error).
//
// EVERY EXPECTED VALUE IN THIS FILE IS COMPUTED BY HAND and written into the
// test as a literal, with the arithmetic shown in the comment above it. Nothing
// here compares a formula against its own implementation: the series are small
// enough that beta, alpha, the active series' sample standard deviation and the
// information ratio can all be worked out on paper, so a sign slip, an n-vs-n-1
// denominator mistake, or a missing annualization would fail the test rather
// than be reproduced by it.
//
// Coverage:
//   1. HandComputed_TwoSeries      — beta/alpha/TE/IR/corr against paper arithmetic
//   2. PerfectTracking             — rs == rb => beta 1, alpha 0, TE 0, IR 0
//   3. Antiphase                   — rs == -rb => beta -1, correlation -1
//   4. Annualization               — ppy enters alpha/active linearly and TE by sqrt
//   5. Guards                      — empty / single / constant-benchmark degeneracies
//   6. TearsheetSuperset           — an absent benchmark changes nothing
//   7. BenchmarkAlignsToReturns    — the sheet folds the same series `tearsheet` does

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/backtest.hpp"  // BacktestResult
#include "atx/vol/tearsheet.hpp" // BenchmarkStats, benchmark_stats, tearsheet

using namespace atx::vol;

namespace {

// A 5-observation pair, chosen so every intermediate is an exact short decimal.
//
//   rs = { 4, -2,  6,  0,  2 }   mean_s = 10 / 5 = 2
//   rb = { 2, -1,  3,  1,  0 }   mean_b =  5 / 5 = 1
//
// deviations   ds = { 2, -4,  4, -2,  0 }
//              db = { 1, -2,  2,  0, -1 }
//
//   Sum ds*db = 2 + 8 + 8 + 0 + 0 = 18   => cov    = 18 / 4 = 4.5
//   Sum db*db = 1 + 4 + 4 + 0 + 1 = 10   => var_b  = 10 / 4 = 2.5
//   Sum ds*ds = 4 + 16 + 16 + 4 + 0 = 40 => var_s  = 40 / 4 = 10
//
//   beta = cov / var_b = 4.5 / 2.5 = 1.8
//   corr = cov / (sd_s * sd_b) = 4.5 / (sqrt(10) * sqrt(2.5)) = 4.5 / 5 = 0.9
//
// active series ra = rs - rb = { 2, -1, 3, -1, 2 }   mean_a = 5 / 5 = 1
//   da = { 1, -2, 2, -2, 1 }
//   Sum da^2 = 1 + 4 + 4 + 4 + 1 = 14   => var_a = 14 / 4 = 3.5
//   sd_a = sqrt(3.5)
constexpr double kStrategy[] = {4.0, -2.0, 6.0, 0.0, 2.0};
constexpr double kBenchmark[] = {2.0, -1.0, 3.0, 1.0, 0.0};

[[nodiscard]] std::span<const double> strategy_span() {
  return std::span<const double>(kStrategy, 5);
}
[[nodiscard]] std::span<const double> benchmark_span() {
  return std::span<const double>(kBenchmark, 5);
}

// A 6-row BacktestResult whose return series (rows 1..5) is exactly `kStrategy`.
// EVERY SoA column `tearsheet` folds is populated -- the struct is a parallel
// column set with no internal length invariant, so a partially-filled result
// indexes out of range rather than reporting a shape error.
[[nodiscard]] BacktestResult make_track() {
  BacktestResult r;
  constexpr std::size_t n = 6;
  const double pnl[n] = {0.0, 4.0, -2.0, 6.0, 0.0, 2.0};
  for (std::size_t i = 0; i < n; ++i) {
    r.date.push_back("2026-01-0" + std::to_string(i + 1));
    r.ts_ns.push_back(static_cast<std::int64_t>(i));
    r.pnl_total.push_back(pnl[i]);
    r.nav.push_back(i == 0 ? 0.0 : r.nav.back() + pnl[i]);
  }
  const std::vector<double> zeros(n, 0.0);
  for (std::vector<double> *column :
       {&r.pnl_delta, &r.pnl_gamma, &r.pnl_vega, &r.pnl_vanna, &r.pnl_volga, &r.pnl_theta,
        &r.pnl_rho, &r.pnl_charm, &r.pnl_unexplained, &r.pnl_settlement, &r.pnl_shares,
        &r.financing, &r.cost, &r.cash, &r.gross_delta, &r.gross_gamma, &r.gross_theta,
        &r.turnover_notional, &r.turnover_vega, &r.n_open_lots}) {
    *column = zeros;
  }
  r.gross_vega = std::vector<double>(n, 1000.0); // nonzero so the vega-scaled block is live
  return r;
}

} // namespace

// ── 1. Every statistic against paper arithmetic ─────────────────────────────
TEST(BenchmarkStats, HandComputed_TwoSeries) {
  // ppy = 1 isolates the estimators from the annualization (checked in test 4).
  const BenchmarkStats s = benchmark_stats(strategy_span(), benchmark_span(), 1.0);

  EXPECT_TRUE(s.has_benchmark);
  EXPECT_EQ(s.n_obs, 5u);

  // beta = 4.5 / 2.5 = 1.8 exactly.
  EXPECT_NEAR(s.beta, 1.8, 1e-14);

  // alpha = (mean_s - beta * mean_b) * ppy = (2 - 1.8 * 1) * 1 = 0.2
  EXPECT_NEAR(s.alpha, 0.2, 1e-14);

  // active_return = mean(rs - rb) * ppy = 1 * 1 = 1
  EXPECT_NEAR(s.active_return, 1.0, 1e-14);

  // tracking_error = sqrt(3.5) * sqrt(1) = 1.8708286933869707
  EXPECT_NEAR(s.tracking_error, std::sqrt(3.5), 1e-14);
  EXPECT_NEAR(s.tracking_error, 1.8708286933869707, 1e-14);

  // information_ratio = active_return / tracking_error = 1 / sqrt(3.5)
  //                   = 0.5345224838248488
  EXPECT_NEAR(s.information_ratio, 1.0 / std::sqrt(3.5), 1e-14);
  EXPECT_NEAR(s.information_ratio, 0.5345224838248488, 1e-14);

  // correlation = 4.5 / (sqrt(10) * sqrt(2.5)) = 4.5 / 5 = 0.9 exactly.
  EXPECT_NEAR(s.correlation, 0.9, 1e-14);
}

// ── 2. Perfect tracking: the degenerate case every IR implementation must get ─
TEST(BenchmarkStats, PerfectTracking_ZeroActiveRiskAndZeroAlpha) {
  const BenchmarkStats s = benchmark_stats(benchmark_span(), benchmark_span(), 252.0);
  EXPECT_TRUE(s.has_benchmark);
  EXPECT_NEAR(s.beta, 1.0, 1e-14);         // cov == var_b
  EXPECT_NEAR(s.alpha, 0.0, 1e-12);        // mean_s - 1 * mean_b == 0
  EXPECT_NEAR(s.active_return, 0.0, 1e-12);
  EXPECT_NEAR(s.tracking_error, 0.0, 1e-14);
  // IR is 0/0; the guard must yield exactly 0, never a NaN.
  EXPECT_EQ(s.information_ratio, 0.0);
  EXPECT_FALSE(std::isnan(s.information_ratio));
  EXPECT_NEAR(s.correlation, 1.0, 1e-14);
}

// ── 3. Antiphase: beta and correlation must carry the sign ──────────────────
TEST(BenchmarkStats, Antiphase_NegativeBetaAndCorrelation) {
  std::vector<double> negated;
  for (const double x : kBenchmark) {
    negated.push_back(-x);
  }
  const BenchmarkStats s = benchmark_stats(negated, benchmark_span(), 1.0);
  EXPECT_NEAR(s.beta, -1.0, 1e-14);
  EXPECT_NEAR(s.correlation, -1.0, 1e-14);
  // alpha = (mean_s - beta*mean_b) = (-1 - (-1 * 1)) = 0
  EXPECT_NEAR(s.alpha, 0.0, 1e-12);
  // active = mean(-rb - rb) = -2 * mean_b = -2
  EXPECT_NEAR(s.active_return, -2.0, 1e-14);
}

// ── 4. Annualization enters alpha linearly and tracking error by sqrt ───────
TEST(BenchmarkStats, Annualization_ScalesAlphaLinearlyAndTeBySqrt) {
  const BenchmarkStats unit = benchmark_stats(strategy_span(), benchmark_span(), 1.0);
  const BenchmarkStats annual = benchmark_stats(strategy_span(), benchmark_span(), 252.0);

  // beta and correlation are scale-free — annualization must NOT touch them.
  EXPECT_NEAR(annual.beta, unit.beta, 1e-14);
  EXPECT_NEAR(annual.correlation, unit.correlation, 1e-14);

  // alpha and active return scale by ppy; tracking error by sqrt(ppy).
  EXPECT_NEAR(annual.alpha, unit.alpha * 252.0, 1e-12);
  EXPECT_NEAR(annual.active_return, unit.active_return * 252.0, 1e-12);
  EXPECT_NEAR(annual.tracking_error, unit.tracking_error * std::sqrt(252.0), 1e-12);

  // Therefore IR scales by sqrt(ppy) — the "IR is annualized by sqrt(T)" law.
  EXPECT_NEAR(annual.information_ratio, unit.information_ratio * std::sqrt(252.0), 1e-12);
  // 1/sqrt(3.5) * sqrt(252) = 8.48528137423857
  EXPECT_NEAR(annual.information_ratio, 8.48528137423857, 1e-10);
}

// ── 5. Degenerate inputs are guarded, never NaN ─────────────────────────────
TEST(BenchmarkStats, Guards_EmptySingleAndConstantBenchmark) {
  // No benchmark at all => nothing is claimed.
  const BenchmarkStats none = benchmark_stats(strategy_span(), {}, 252.0);
  EXPECT_FALSE(none.has_benchmark);
  EXPECT_EQ(none.n_obs, 0u);
  EXPECT_EQ(none.beta, 0.0);
  EXPECT_EQ(none.information_ratio, 0.0);

  // One paired observation: a sample variance needs two.
  const double one_s[] = {1.0};
  const double one_b[] = {1.0};
  const BenchmarkStats single = benchmark_stats(one_s, one_b, 252.0);
  EXPECT_TRUE(single.has_benchmark);
  EXPECT_EQ(single.n_obs, 1u);
  EXPECT_EQ(single.beta, 0.0);
  EXPECT_FALSE(std::isnan(single.alpha));

  // A constant benchmark has zero variance => beta is undefined, guarded to 0,
  // and alpha collapses to the strategy's own mean return.
  const double flat[] = {3.0, 3.0, 3.0, 3.0, 3.0};
  const BenchmarkStats constant = benchmark_stats(strategy_span(), flat, 1.0);
  EXPECT_EQ(constant.beta, 0.0);
  EXPECT_FALSE(std::isnan(constant.beta));
  EXPECT_NEAR(constant.alpha, 2.0, 1e-14); // mean_s - 0 * 3
  EXPECT_EQ(constant.correlation, 0.0);
  EXPECT_FALSE(std::isnan(constant.correlation));

  // Mismatched lengths truncate to the common prefix rather than reading past.
  const double short_b[] = {2.0, -1.0, 3.0};
  const BenchmarkStats truncated = benchmark_stats(strategy_span(), short_b, 1.0);
  EXPECT_EQ(truncated.n_obs, 3u);
}

// ── 6. The benchmark block is a strict superset of the absolute sheet ───────
TEST(BenchmarkStats, TearsheetWithBenchmark_IsAStrictSuperset) {
  const BacktestResult r = make_track();

  const TearSheet plain = tearsheet(r, 252.0);
  const TearSheet no_bench = tearsheet_with_benchmark(r, {}, 252.0);
  const TearSheet with_bench = tearsheet_with_benchmark(r, benchmark_span(), 252.0);

  // An absent benchmark leaves the sheet EXACTLY as `tearsheet` built it.
  EXPECT_FALSE(plain.benchmark.has_benchmark);
  EXPECT_FALSE(no_bench.benchmark.has_benchmark);
  EXPECT_EQ(no_bench.total_return, plain.total_return);
  EXPECT_EQ(no_bench.sharpe, plain.sharpe);
  EXPECT_EQ(no_bench.max_drawdown, plain.max_drawdown);

  // A present benchmark adds the block and perturbs NO absolute statistic.
  EXPECT_TRUE(with_bench.benchmark.has_benchmark);
  EXPECT_EQ(with_bench.total_return, plain.total_return);
  EXPECT_EQ(with_bench.sharpe, plain.sharpe);
  EXPECT_EQ(with_bench.ann_vol, plain.ann_vol);
  EXPECT_EQ(with_bench.max_drawdown, plain.max_drawdown);
}

// ── 7. The benchmark aligns to the SAME return series the sheet folds ───────
TEST(BenchmarkStats, BenchmarkAlignsToTheTearsheetReturnSeries) {
  const BacktestResult r = make_track();

  // Row 0 is inception and carries NO return, so the folded series is rows 1..5
  // — which is exactly `kStrategy`. If the implementation accidentally included
  // row 0, n_obs would be 6 and every statistic would shift.
  const std::vector<double> returns = backtest_return_series(r);
  ASSERT_EQ(returns.size(), 5u);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(returns[i], kStrategy[i]) << "return series element " << i;
  }

  // And the sheet's block equals the direct call on that series — proving the
  // alignment rather than assuming it.
  const TearSheet sheet = tearsheet_with_benchmark(r, benchmark_span(), 1.0);
  const BenchmarkStats direct = benchmark_stats(returns, benchmark_span(), 1.0);
  EXPECT_EQ(sheet.benchmark.n_obs, 5u);
  EXPECT_NEAR(sheet.benchmark.beta, direct.beta, 1e-15);
  EXPECT_NEAR(sheet.benchmark.beta, 1.8, 1e-14); // the hand-computed value
  EXPECT_NEAR(sheet.benchmark.information_ratio, direct.information_ratio, 1e-15);
}
