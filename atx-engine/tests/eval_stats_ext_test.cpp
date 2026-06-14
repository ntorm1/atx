#include <gtest/gtest.h>
#include <vector>
#include "atx/engine/eval/stats_ext.hpp"

namespace atxtest_eval_stats_ext_test {

using namespace atx::engine::eval;
TEST(EvalStatsExt, NormCdf_KnownPoints) {
  EXPECT_NEAR(norm_cdf(0.0), 0.5, 1e-12);
  EXPECT_NEAR(norm_cdf(1.959963984540054), 0.975, 1e-9);
  EXPECT_NEAR(norm_cdf(-1.959963984540054), 0.025, 1e-9);
}
TEST(EvalStatsExt, NormPpf_InvertsNormCdf) {
  for (double p : {0.01, 0.25, 0.5, 0.75, 0.975, 0.999}) EXPECT_NEAR(norm_cdf(norm_ppf(p)), p, 1e-9);
  EXPECT_NEAR(norm_ppf(0.975), 1.959963984540054, 1e-7);
}
TEST(EvalStatsExt, SkewKurt_SymmetricIsZero) {
  std::vector<double> s{-2,-1,0,1,2};
  EXPECT_NEAR(skewness(s), 0.0, 1e-12);
}
TEST(EvalStatsExt, ExcessKurtosis_KnownValue) {
  std::vector<double> s{-2,-1,0,1,2};   // pop: mu=0, var=2, 4th std moment=1.7 -> excess = -1.3
  EXPECT_NEAR(excess_kurtosis(s), -1.3, 1e-12);
}
TEST(EvalStatsExt, Median_EvenAndOdd) {
  std::vector<double> a{3,1,2}; std::vector<double> b{4,1,3,2};
  EXPECT_DOUBLE_EQ(median(a), 2.0); EXPECT_DOUBLE_EQ(median(b), 2.5);
}
TEST(EvalStatsExt, ReturnsFromEquity_SimpleSteps) {
  std::vector<double> eq{100,110,99}; auto r = returns_from_equity(eq);
  ASSERT_EQ(r.size(), 2U); EXPECT_NEAR(r[0], 0.10, 1e-12); EXPECT_NEAR(r[1], -0.10, 1e-12);
}


}  // namespace atxtest_eval_stats_ext_test
