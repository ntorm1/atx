// atx::engine::factory — BehavioralArchive farthest-point eviction (L3, suite
// FactoryArchiveL3). Fifo stays the default (legacy ring); FarthestPoint keeps
// the archive's minimum pairwise behavioural distance non-decreasing.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/factory/behavior.hpp"

namespace atx_test_l3_search_archive {

using atx::f64;
using atx::usize;
using atx::engine::factory::ArchiveEviction;
using atx::engine::factory::behavioral_distance;
using atx::engine::factory::BehavioralArchive;

[[nodiscard]] f64 min_pairwise(const BehavioralArchive &a) {
  f64 m = 2.0;
  const auto &e = a.entries();
  for (usize i = 0; i < e.size(); ++i) {
    for (usize j = i + 1; j < e.size(); ++j) {
      m = std::min(m, behavioral_distance(e[i], e[j]));
    }
  }
  return m;
}

// Descriptor family: `cluster` picks one of 6 base waves, `noise` perturbs it.
[[nodiscard]] std::vector<f64> profile(usize cluster, usize k) {
  std::vector<f64> x(64);
  std::uint64_t s = 0x9E37ULL + 131U * k;
  for (usize t = 0; t < x.size(); ++t) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const f64 u = static_cast<f64>(s >> 11U) / static_cast<f64>(1ULL << 53U) - 0.5;
    x[t] = std::sin(0.1 * static_cast<f64>((cluster + 1) * t) + static_cast<f64>(cluster)) + 0.3 * u;
  }
  return x;
}

TEST(FactoryArchiveL3, FifoRemainsTheDefault) {
  BehavioralArchive a{2};
  const auto p0 = profile(0, 0);
  const auto p1 = profile(1, 1);
  const auto p2 = profile(0, 2);
  a.insert(p0);
  a.insert(p1);
  a.insert(p2);
  ASSERT_EQ(a.size(), 2U);
  EXPECT_EQ(a.entries()[0], p1); // oldest evicted
  EXPECT_EQ(a.entries()[1], p2);
}

TEST(FactoryArchiveL3, FarthestPointMinDistanceNeverDecreases) {
  BehavioralArchive a{6, ArchiveEviction::FarthestPoint};
  f64 prev = -1.0;
  for (usize k = 0; k < 120; ++k) {
    a.insert(profile(k % 6 == 0 ? 0 : (k * 7) % 6, k));
    if (a.size() == 6) {
      const f64 cur = min_pairwise(a);
      EXPECT_GE(cur, prev - 1e-12) << "insert " << k;
      prev = cur;
    }
  }
  // A FIFO archive fed the same stream ends less spread out.
  BehavioralArchive fifo{6};
  for (usize k = 0; k < 120; ++k) {
    fifo.insert(profile(k % 6 == 0 ? 0 : (k * 7) % 6, k));
  }
  EXPECT_GE(min_pairwise(a), min_pairwise(fifo));
}

TEST(FactoryArchiveL3, FarthestPointRejectsDuplicatesWhenFull) {
  BehavioralArchive a{3, ArchiveEviction::FarthestPoint};
  const auto p0 = profile(0, 0);
  const auto p1 = profile(2, 1);
  const auto p2 = profile(4, 2);
  a.insert(p0);
  a.insert(p1);
  a.insert(p2);
  const auto before = a.entries();
  a.insert(p1); // an exact copy is distance 0 from p1 -> never admitted
  EXPECT_EQ(a.entries(), before);
}

} // namespace atx_test_l3_search_archive
