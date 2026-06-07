#include <gtest/gtest.h>
#include <cmath>
#include <cstddef>
#include <vector>
#include "atx/engine/eval/pbo.hpp"
using namespace atx::engine::eval;
// Deterministic bounded ~zero-mean noise matrix (NO rng): m[c*T+t].
static std::vector<double> noise_matrix(std::size_t N, std::size_t T) {
  std::vector<double> m(N*T);
  for (std::size_t c=0;c<N;++c) for (std::size_t t=0;t<T;++t)
    m[c*T+t] = std::sin(0.7*double(c+1) + 0.3*double(t)) - std::sin(0.7*double(c+1));
  return m;
}
TEST(EvalPbo, SplitCount_IsCombinatorial) {
  auto r = pbo_cscv(noise_matrix(8, 16), 8, /*S=*/4);   // C(4,2) = 6
  EXPECT_EQ(r.split_logits.size(), 6U);
}
TEST(EvalPbo, PureNoise_AboutHalf) {
  auto r = pbo_cscv(noise_matrix(20, 64), 20, /*S=*/8);  // C(8,4)=70
  EXPECT_NEAR(r.pbo, 0.5, 0.20);
}
TEST(EvalPbo, OneGenuineEdge_MateriallyBelowHalf) {
  auto m = noise_matrix(20, 64);
  for (std::size_t t=0;t<64;++t) m[0*64 + t] += 0.5;   // candidate 0 persistent edge
  auto r = pbo_cscv(m, 20, 8);
  EXPECT_LT(r.pbo, 0.30);
}
TEST(EvalPbo, Deterministic_TwoRunsEqual) {
  auto a = pbo_cscv(noise_matrix(12,32),12,4); auto b = pbo_cscv(noise_matrix(12,32),12,4);
  EXPECT_EQ(a.pbo, b.pbo);
}
TEST(EvalPbo, OddSplitOrTooFew_Errors) {
  EXPECT_TRUE(pbo_cscv_checked(noise_matrix(4,16),4,/*S=*/3).is_err());   // S odd
}
