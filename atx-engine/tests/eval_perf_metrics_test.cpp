#include <gtest/gtest.h>
#include <vector>
#include <span>
#include <cmath>
#include "atx/engine/eval/perf_metrics.hpp"
#include "atx/engine/combine/metrics.hpp"
using namespace atx::engine::eval;
TEST(EvalPerfMetrics, Sharpe_MatchesCombineBitForBit) {
  std::vector<double> pnl{0.0, 0.01, -0.005, 0.02, 0.0, 0.015};
  auto rm = compute_return_metrics(pnl, {});
  auto cm = atx::engine::combine::compute_metrics(pnl, {}, /*n_instruments=*/0U, /*book_size=*/1.0);
  EXPECT_EQ(rm.sharpe, cm.sharpe);
}
TEST(EvalPerfMetrics, Sortino_DownsideOnly) {
  std::vector<double> pnl{0.0, 0.01, 0.02, 0.03};
  EXPECT_EQ(compute_return_metrics(pnl, {}).sortino, 0.0);
}
TEST(EvalPerfMetrics, HitRate_CountsPositive) {
  std::vector<double> pnl{0.0, 1.0, -1.0, 1.0, 1.0};
  EXPECT_NEAR(compute_return_metrics(pnl, {}).hit_rate, 0.75, 1e-12);
}
TEST(EvalPerfMetrics, Calmar_AnnRetOverMaxDD) {
  std::vector<double> pnl{0.0, 0.1, -0.2, 0.1};
  auto rm = compute_return_metrics(pnl, {});
  EXPECT_GT(rm.max_dd, 0.0); EXPECT_TRUE(std::isfinite(rm.calmar));
}
TEST(EvalPerfMetrics, Deterministic_TwoRunsByteIdentical) {
  std::vector<double> pnl{0.0,0.01,-0.02,0.03,-0.01,0.02};
  auto a = compute_return_metrics(pnl, {}); auto b = compute_return_metrics(pnl, {});
  EXPECT_EQ(a.sharpe,b.sharpe); EXPECT_EQ(a.sortino,b.sortino); EXPECT_EQ(a.max_dd,b.max_dd);
}
TEST(EvalPerfMetrics, EmptyAndSingle_Degenerate) {
  EXPECT_NO_FATAL_FAILURE((void)compute_return_metrics(std::span<const double>{}, {}));
  std::vector<double> one{0.0}; (void)compute_return_metrics(one, {});
}
