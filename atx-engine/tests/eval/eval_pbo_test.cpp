#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>
#include "atx/engine/eval/pbo.hpp"

namespace atxtest_eval_pbo_test {

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

TEST(EvalPbo, RejectsRaggedCandidatesButKeepsDocumentedPeriodTrimming) {
  auto ragged = noise_matrix(4, 16);
  ragged.push_back(0.125);
  const auto invalid = pbo_cscv_checked(ragged, 4, 4);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error().code(), atx::core::ErrorCode::InvalidArgument);
  const auto trimmed = pbo_cscv_checked(noise_matrix(4, 17), 4, 4);
  const auto reference = pbo_cscv_checked(noise_matrix(4, 16), 4, 4);
  ASSERT_TRUE(trimmed.has_value()); ASSERT_TRUE(reference.has_value());
  EXPECT_EQ(trimmed->split_logits, reference->split_logits);
}

TEST(EvalPbo, BoundsExhaustiveSplitEnumerationBeforeAllocation) {
  const auto boundary = pbo_cscv_checked(noise_matrix(2, 16), 2, 16);
  ASSERT_TRUE(boundary.has_value());
  EXPECT_EQ(boundary->split_logits.size(), 12870U);
  const auto oversized = pbo_cscv_checked(noise_matrix(2, 18), 2, 18);
  ASSERT_FALSE(oversized.has_value());
  EXPECT_EQ(oversized.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// Independent frozen oracle: original pbo.hpp at f9171a0b and the ordered
// mean/std formula from stats_ext.hpp. It calls no production PBO/moment/rank
// helper. Recursive ascending subsets give the original lexicographic order.
static PboResult frozen_gather_oracle(std::span<const double> perf, std::size_t n,
                                    std::size_t splits) {
  const std::size_t periods = perf.size() / n, width = periods / splits;
  std::vector<std::size_t> selected;
  PboResult out{0.0, {}, 0.0};
  std::size_t nonpositive = 0;
  auto visit = [&](auto&& self, std::size_t first) -> void {
    if (selected.size() != splits / 2) {
      for (std::size_t block = first; block < splits; ++block) {
        selected.push_back(block);
        self(self, block + 1);
        selected.pop_back();
      }
      return;
    }
    auto sharpe = [&](std::size_t candidate, bool in_sample) {
      std::vector<double> values;
      for (std::size_t block = 0; block < splits; ++block) {
        const bool chosen = std::find(selected.begin(), selected.end(), block) != selected.end();
        if (chosen != in_sample) continue;
        for (std::size_t t = 0; t < width; ++t)
          values.push_back(perf[candidate * periods + block * width + t]);
      }
      double sum = 0.0;
      for (const double v : values) sum += v;
      const double mean = sum / static_cast<double>(values.size());
      double square = 0.0;
      for (const double v : values) { const double d = v - mean; square += d * d; }
      const double deviation = std::sqrt(square / static_cast<double>(values.size()));
      return deviation == 0.0 ? 0.0 : mean / deviation;
    };
    std::vector<double> oos(n);
    double maximum = 0.0;
    std::size_t winner = 0;
    for (std::size_t c = 0; c < n; ++c) {
      const double in = sharpe(c, true);
      oos[c] = sharpe(c, false);
      if (c == 0 || in > maximum) { maximum = in; winner = c; }
    }
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return oos[a] < oos[b]; });
    const auto rank = std::find(order.begin(), order.end(), winner) - order.begin();
    const double w = (static_cast<double>(rank) + 1.0) / static_cast<double>(n + 1);
    const double logit = std::log(w / (1.0 - w));
    out.split_logits.push_back(logit);
    out.mean_logit += logit;
    if (logit <= 0.0) ++nonpositive;
  };
  visit(visit, 0);
  out.pbo = static_cast<double>(nonpositive) / static_cast<double>(out.split_logits.size());
  out.mean_logit /= static_cast<double>(out.split_logits.size());
  return out;
}

static void expect_same_bits(const PboResult& actual, const PboResult& oracle) {
  ASSERT_EQ(actual.split_logits.size(), oracle.split_logits.size());
  EXPECT_EQ(std::bit_cast<std::uint64_t>(actual.pbo), std::bit_cast<std::uint64_t>(oracle.pbo));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(actual.mean_logit),
            std::bit_cast<std::uint64_t>(oracle.mean_logit));
  for (std::size_t i = 0; i < actual.split_logits.size(); ++i)
    EXPECT_EQ(std::bit_cast<std::uint64_t>(actual.split_logits[i]),
              std::bit_cast<std::uint64_t>(oracle.split_logits[i])) << "split " << i;
}

TEST(EvalPbo, VersionedRulesMatchIndependentFrozenOracleAndUseCache) {
  for (const std::size_t splits : {4U, 8U}) {
    const auto matrix = noise_matrix(9, 67); // nonempty trimmed tail, original row stride
    const auto oracle = frozen_gather_oracle(matrix, 9, splits);
    const auto legacy = pbo_cscv_checked(matrix, 9, splits, PboRule::LegacyGatherV1);
    const auto cached = pbo_cscv_checked(matrix, 9, splits); // deliberate V2 default
    ASSERT_TRUE(legacy.has_value()); ASSERT_TRUE(cached.has_value());
    expect_same_bits(*legacy, oracle); expect_same_bits(*cached, oracle);
    EXPECT_EQ(legacy->rule, PboRule::LegacyGatherV1);
    EXPECT_EQ(cached->rule, PboRule::CachedMomentsV2);
    const auto evaluations = 2U * 9U * oracle.split_logits.size();
    EXPECT_EQ(legacy->cached_evaluations, 0U);
    EXPECT_EQ(legacy->reference_evaluations, evaluations);
    EXPECT_EQ(cached->cached_evaluations, evaluations);
    EXPECT_LT(cached->reference_evaluations, evaluations);
  }
}

TEST(EvalPbo, NearTieWinnerAndOosRankBoundariesUseReference) {
  const double e = std::ldexp(1.0, -48);
  // Split 0: near-tied IS winner, widely different OOS ranks.
  // Split 1: clear IS winner, near-tied OOS winner rank. Both comparisons matter.
  const std::vector<double> matrix{
      0, 2, 0, 2,              4, 6, 4, 6,
      e, 2 + e, e, 2 + e,     -4, -2, -4, -2,
      -4, -2, -4, -2,         0, 2, 0, 2};
  const auto oracle = frozen_gather_oracle(matrix, 3, 2);
  const auto cached = pbo_cscv_checked(matrix, 3, 2, PboRule::CachedMomentsV2);
  ASSERT_TRUE(cached.has_value());
  expect_same_bits(*cached, oracle);
  EXPECT_GE(cached->ambiguous_comparisons, 2U);
  EXPECT_GE(cached->reference_evaluations, 4U);

  // Exact score ties still choose the first IS maximum and ascending OOS index.
  std::vector<double> ties(3 * 16);
  for (std::size_t c = 0; c < 3; ++c)
    for (std::size_t t = 0; t < 16; ++t) ties[c * 16 + t] = (t % 2 == 0) ? -1.0 : 1.0;
  const auto tied = pbo_cscv_checked(ties, 3, 4);
  ASSERT_TRUE(tied.has_value());
  expect_same_bits(*tied, frozen_gather_oracle(ties, 3, 4));
  EXPECT_GT(tied->ambiguous_comparisons, 0U);
}

TEST(EvalPbo, UnstableMomentsFallbackPreservesValidExtremeAndDegenerateScores) {
  std::vector<std::vector<double>> fixtures;
  auto offset = noise_matrix(3, 16);
  for (auto& value : offset) value = 1e12 + value * 0.01;
  fixtures.push_back(std::move(offset));
  std::vector<double> huge(3 * 16), constant(3 * 16);
  for (std::size_t c = 0; c < 3; ++c)
    for (std::size_t t = 0; t < 16; ++t) {
      // Ordered reference variance overflows, but mean/std remains finite zero.
      huge[c * 16 + t] = (t % 2 == 0) ? -1e200 : 1e200;
      constant[c * 16 + t] = static_cast<double>(c + 1);
    }
  fixtures.push_back(std::move(huge)); fixtures.push_back(std::move(constant));
  std::vector<double> subnormal_variance(3 * 16), subnormal_error_bound(3 * 16);
  for (std::size_t c = 0; c < 3; ++c)
    for (std::size_t t = 0; t < 16; ++t) {
      // Normal raw scale does not prevent centered variance from being subnormal.
      const double step = static_cast<double>(t % 4 + c);
      subnormal_variance[c * 16 + t] = 1e-153 + step * 1e-161;
      // Variance is normal here; its relative-error envelope is subnormal.
      subnormal_error_bound[c * 16 + t] = 1e-150 + step * 1e-150;
    }
  fixtures.push_back(std::move(subnormal_variance));
  fixtures.push_back(std::move(subnormal_error_bound));
  for (const auto& matrix : fixtures) {
    const auto oracle = frozen_gather_oracle(matrix, 3, 4);
    const auto legacy = pbo_cscv_checked(matrix, 3, 4, PboRule::LegacyGatherV1);
    const auto cached = pbo_cscv_checked(matrix, 3, 4, PboRule::CachedMomentsV2);
    ASSERT_TRUE(legacy.has_value()); ASSERT_TRUE(cached.has_value());
    expect_same_bits(*legacy, oracle); expect_same_bits(*cached, oracle);
    EXPECT_EQ(cached->reference_evaluations, 2U * 3U * oracle.split_logits.size());
  }
}

TEST(EvalPbo, RejectsNonfiniteUsedValuesAndUndefinedScoresButIgnoresTrimmedTail) {
  const auto finite = noise_matrix(3, 16);
  auto trimmed = noise_matrix(3, 17);
  trimmed[16] = std::numeric_limits<double>::quiet_NaN();
  trimmed[33] = std::numeric_limits<double>::infinity();
  trimmed[50] = -std::numeric_limits<double>::infinity();
  for (const auto rule : {PboRule::LegacyGatherV1, PboRule::CachedMomentsV2}) {
    const auto valid = pbo_cscv_checked(trimmed, 3, 4, rule);
    ASSERT_TRUE(valid.has_value());
    expect_same_bits(*valid, frozen_gather_oracle(finite, 3, 4));
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()}) {
      auto used = finite; used[7] = bad;
      const auto invalid = pbo_cscv_checked(used, 3, 4, rule);
      ASSERT_FALSE(invalid.has_value());
      EXPECT_EQ(invalid.error().code(), atx::core::ErrorCode::InvalidArgument);
    }
    const std::vector<double> overflowing_mean(3 * 16, 1e308);
    const auto undefined = pbo_cscv_checked(overflowing_mean, 3, 4, rule);
    ASSERT_FALSE(undefined.has_value());
    EXPECT_EQ(undefined.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  EXPECT_FALSE(pbo_cscv_checked(finite, 3, 4, static_cast<PboRule>(99)).has_value());
}


}  // namespace atxtest_eval_pbo_test
