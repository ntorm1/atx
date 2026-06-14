// book_allocation_test.cpp — S7-4: capacity-bounded fractional-Kelly allocation.
//
// book::size_book maps a book Sharpe + book vol to a gross-leverage target via a
// fractional-Kelly rule L = c·SR/σ, HARD-CLIPPED at min(capacity_gross, max_gross).
// The load-bearing proof (R7) is that the allocated gross NEVER exceeds the
// capacity ceiling: when Kelly wants more than capacity, capacity wins.
//
// book::effective_breadth is the participation ratio of the alpha-return
// covariance eigenspectrum (Σλ)²/Σλ² — Grinold-Kahn breadth: many weakly
// correlated alphas ⇒ BR≈N, a collinear pile ⇒ BR≈1.
//
// Coverage:
//   * KellyLeverageFromSharpe — uncapped Kelly matches c·SR/σ.
//   * CapacityClipsKelly (R7) — capacity below Kelly wins.
//   * EffectiveBreadthParticipationRatio — equal eigs ⇒ N; one dominant ⇒ ≈1.
//   * Boundaries — σ<=0 ⇒ 0; negative Kelly clamps to 0; max_gross caps.

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp" // VecX
#include "atx/core/types.hpp"         // f64

#include "atx/engine/book/allocation.hpp" // the unit under test

namespace atxtest_book_allocation_test {

using atx::f64;
using atx::core::linalg::VecX;
using atx::engine::book::AllocationConfig;
using atx::engine::book::effective_breadth;
using atx::engine::book::size_book;

// Half-Kelly default config (c = 0.5, max_gross = 4).
[[nodiscard]] AllocationConfig half_kelly() { return AllocationConfig{}; }

// N equal positive eigenvalues ⇒ participation ratio == N (uncorrelated basis).
[[nodiscard]] VecX equal_eigs(int n) {
  VecX v(n);
  for (int i = 0; i < n; ++i) {
    v[i] = 1.0;
  }
  return v;
}

// One dominant eigenvalue + a tail of tiny ones ⇒ participation ratio ≈ 1
// (collinear: nearly all variance in one direction).
[[nodiscard]] VecX one_dominant_eig(int n) {
  VecX v(n);
  v[0] = 1000.0;
  for (int i = 1; i < n; ++i) {
    v[i] = 1e-6;
  }
  return v;
}

// ===========================================================================
//  Kelly sizing
// ===========================================================================
TEST(BookAllocation, KellyLeverageFromSharpe) {
  // Uncapped (capacity huge): L = c·SR/σ = 0.5·2.0/0.1 = 10.0 — but max_gross=4
  // would cap it; bump max_gross so the raw Kelly is observable.
  AllocationConfig cfg = half_kelly();
  cfg.max_gross = 100.0;
  EXPECT_NEAR(size_book(2.0, 0.1, 1e9, cfg), 0.5 * 2.0 / 0.1, 1e-12);
}

TEST(BookAllocation, CapacityClipsKelly) {
  // R7: Kelly wants 0.5·3.0/0.1 = 15.0, capacity_gross = 0.7 ⇒ allocated == 0.7.
  EXPECT_EQ(size_book(3.0, 0.1, 0.7, half_kelly()), 0.7);
}

TEST(BookAllocation, MaxGrossCapsWhenBothExceed) {
  // Kelly = 15.0, capacity = 10.0, max_gross = 4.0 ⇒ the min ceiling (4.0) wins.
  EXPECT_EQ(size_book(3.0, 0.1, 10.0, half_kelly()), 4.0);
}

TEST(BookAllocation, ZeroVolGivesZero) {
  EXPECT_EQ(size_book(2.0, 0.0, 1.0, half_kelly()), 0.0);
  EXPECT_EQ(size_book(2.0, -0.5, 1.0, half_kelly()), 0.0); // σ<=0 guarded
}

TEST(BookAllocation, NegativeKellyClampsToZero) {
  // Negative book Sharpe ⇒ negative Kelly ⇒ clamped to 0 (no short-the-book).
  EXPECT_EQ(size_book(-2.0, 0.1, 1.0, half_kelly()), 0.0);
}

// ===========================================================================
//  Effective breadth (participation ratio)
// ===========================================================================
TEST(BookAllocation, EffectiveBreadthParticipationRatio) {
  EXPECT_NEAR(effective_breadth(equal_eigs(10)), 10.0, 1e-9);       // uncorrelated ⇒ BR≈N
  EXPECT_NEAR(effective_breadth(one_dominant_eig(10)), 1.0, 0.5);   // collinear ⇒ BR≈1
}

TEST(BookAllocation, EffectiveBreadthIgnoresNonPositiveEigs) {
  // Negative / zero eigenvalues are clamped to 0 in both sums, so a spectrum of
  // {1, 1, 0, -3} contributes the same as two unit eigs ⇒ BR == 2.
  VecX v(4);
  v << 1.0, 1.0, 0.0, -3.0;
  EXPECT_NEAR(effective_breadth(v), 2.0, 1e-9);
}

TEST(BookAllocation, EffectiveBreadthAllNonPositiveIsZero) {
  VecX v(3);
  v << 0.0, -1.0, -2.0;
  EXPECT_EQ(effective_breadth(v), 0.0); // s2 <= 0 ⇒ 0 (no division by zero)
}


}  // namespace atxtest_book_allocation_test
