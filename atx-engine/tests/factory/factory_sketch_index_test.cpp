// atx::engine::factory — SketchIndex / FarthestPointArchive tests
// (L3, suite FactorySketchIndex).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/factory/sketch_index.hpp"

namespace atx_test_l3_search_sketch_index {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::factory::FarthestPointArchive;
using atx::engine::factory::Neighbor;
using atx::engine::factory::SketchIndex;

struct Gauss {
  std::uint64_t s;
  [[nodiscard]] f64 uni() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) + 0.5) / static_cast<f64>(1ULL << 53U);
  }
  [[nodiscard]] f64 next() noexcept { // Box-Muller (one leg)
    const f64 u1 = uni();
    const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

// Alpha-zoo-shaped PnL: a market factor, 1000 style clusters, idiosyncratic noise.
struct Zoo {
  usize length;
  std::vector<std::vector<f64>> factors;
  std::vector<f64> market;
  Gauss g{0xC0FFEEULL};

  Zoo(usize len, usize clusters) : length{len}, factors(clusters, std::vector<f64>(len)),
                                   market(len) {
    for (f64 &m : market) {
      m = g.next();
    }
    for (auto &f : factors) {
      for (f64 &v : f) {
        v = g.next();
      }
    }
  }

  [[nodiscard]] std::vector<f64> member(usize cluster, f64 load, f64 noise) {
    std::vector<f64> x(length);
    for (usize t = 0; t < length; ++t) {
      x[t] = 0.4 * market[t] + load * factors[cluster][t] + noise * g.next();
    }
    return x;
  }
};

TEST(FactorySketchIndex, RecallAt10AgainstBruteForceOn10k) {
  constexpr usize kLen = 252;
  constexpr usize kClusters = 1000;
  constexpr usize kN = 10000;
  Zoo zoo{kLen, kClusters};
  SketchIndex idx{kLen};
  for (usize i = 0; i < kN; ++i) {
    // Heterogeneous loadings so within-cluster correlations are spread out.
    const f64 load = 0.6 + 0.8 * zoo.g.uni();
    idx.add(i, zoo.member(i % kClusters, load, 1.0));
  }
  ASSERT_EQ(idx.size(), kN);
  usize hit = 0;
  usize total = 0;
  for (usize q = 0; q < 200; ++q) {
    const auto query = zoo.member((q * 37) % kClusters, 1.0, 1.0);
    const std::vector<Neighbor> approx = idx.topk(query, 10);
    const std::vector<Neighbor> exact = idx.topk_exact(query, 10);
    ASSERT_EQ(approx.size(), 10U);
    ASSERT_EQ(exact.size(), 10U);
    std::set<u64> truth;
    for (const Neighbor &n : exact) {
      truth.insert(n.id);
    }
    for (const Neighbor &n : approx) {
      hit += truth.count(n.id);
    }
    total += 10;
  }
  const f64 recall = static_cast<f64>(hit) / static_cast<f64>(total);
  EXPECT_GE(recall, 0.95) << "recall@10 = " << recall;
}

TEST(FactorySketchIndex, ExactRecheckReturnsTrueCorrelation) {
  constexpr usize kLen = 64;
  SketchIndex idx{kLen, 8};
  std::vector<f64> a(kLen);
  std::vector<f64> neg(kLen);
  for (usize t = 0; t < kLen; ++t) {
    a[t] = std::sin(0.3 * static_cast<f64>(t)) + 0.01 * static_cast<f64>(t);
    neg[t] = -2.0 * a[t] + 5.0;
  }
  idx.add(1, a);
  idx.add(2, neg);
  const auto nn = idx.topk(a, 2);
  ASSERT_EQ(nn.size(), 2U);
  EXPECT_EQ(nn[0].id, 1U); // tie on |corr| == 1 -> lower insertion slot first
  EXPECT_NEAR(nn[0].corr, 1.0, 1e-5);
  EXPECT_EQ(nn[1].id, 2U);
  EXPECT_NEAR(nn[1].corr, -1.0, 1e-5);
}

TEST(FactorySketchIndex, DegenerateSeriesAreSafe) {
  SketchIndex idx{16};
  const std::vector<f64> flat(16, 3.0);
  const std::vector<f64> nan(16, std::nan(""));
  idx.add(1, flat);
  idx.add(2, nan);
  const auto nn = idx.topk(flat, 5);
  ASSERT_EQ(nn.size(), 2U);
  for (const Neighbor &n : nn) {
    EXPECT_EQ(n.corr, 0.0);
  }
  EXPECT_TRUE(SketchIndex{16}.topk(flat, 3).empty());
}

TEST(FactorySketchIndex, FarthestPointArchiveMinDistanceNeverDecreases) {
  constexpr usize kLen = 128;
  Zoo zoo{kLen, 40};
  FarthestPointArchive arch{16, kLen};
  f64 prev = 2.0;
  bool full_seen = false;
  for (usize i = 0; i < 400; ++i) {
    static_cast<void>(arch.offer(i, zoo.member(i % 40, 1.0, 0.5)));
    if (arch.size() == 16) {
      const f64 cur = arch.min_pairwise();
      if (full_seen) {
        EXPECT_GE(cur, prev - 1e-12) << "at offer " << i;
      }
      prev = cur;
      full_seen = true;
    }
  }
  EXPECT_EQ(arch.size(), 16U);
  // Re-offering a near-copy of the most redundant pair cannot lower the minimum.
  static_cast<void>(arch.offer(9999, zoo.member(0, 1.0, 0.0)));
  EXPECT_GE(arch.min_pairwise(), prev - 1e-12);
  EXPECT_FALSE(FarthestPointArchive(0, kLen).offer(1, zoo.member(0, 1.0, 0.5)));
}

} // namespace atx_test_l3_search_sketch_index
