#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "atx/engine/eval/pbo.hpp"
using namespace atx::engine::eval;
// Deterministic zero-edge noise matrix (NO rng): m[c*T+t] in [-0.5,0.5) via a
// SplitMix64 hash of (c,t). No per-candidate persistent mean, so a correct CSCV
// yields PBO~0.5 (the earlier sin()-based fixture imposed a constant per-candidate
// level edge that drove PBO->0 for any S — a defective "noise" generator).
static std::vector<double> noise_matrix(std::size_t N, std::size_t T) {
  std::vector<double> m(N*T);
  for (std::size_t c=0;c<N;++c) for (std::size_t t=0;t<T;++t) {
    std::uint64_t z = ((static_cast<std::uint64_t>(c) << 32) ^ static_cast<std::uint64_t>(t))
                      + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z =  z ^ (z >> 31);
    m[c*T+t] = (static_cast<double>(z >> 11) * (1.0/9007199254740992.0)) - 0.5; // [-0.5,0.5)
  }
  return m;
}
TEST(EvalPbo, SplitCount_IsCombinatorial) {
  auto r = pbo_cscv(noise_matrix(8, 16), 8, /*S=*/4);   // C(4,2) = 6
  EXPECT_EQ(r.split_logits.size(), 6U);
}
TEST(EvalPbo, PureNoise_AboutHalf) {
  auto r = pbo_cscv(noise_matrix(20, 64), 20, /*S=*/8);  // C(8,4)=70
  EXPECT_EQ(r.split_logits.size(), 70U);
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
  EXPECT_FALSE(pbo_cscv_checked(noise_matrix(4,16),4,/*S=*/3).has_value());   // S odd -> Err
}
