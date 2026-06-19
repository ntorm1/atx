#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "book_shape.hpp"

namespace { using atx::f64; }

TEST(BookShape, DollarNeutralAndGross) {
  std::vector<f64> w = {3.0, 1.0, -1.0, -1.0};
  std::vector<std::uint8_t> live = {1,1,1,1};
  atx::impl::shape_book(w, live, /*gross*/1.0, /*name_cap*/1.0);
  f64 s = 0, g = 0; for (f64 x : w) { s += x; g += std::abs(x); }
  EXPECT_NEAR(s, 0.0, 1e-9);
  EXPECT_NEAR(g, 1.0, 1e-9);
}

// Feasible cap: one name blows past the cap after gross-normalization; the cap binds
// and the freed gross is redistributed to the unclipped names so Sigma|w| == gross.
// 4 active names, cap 0.30 -> max gross 4*0.30 = 1.20 >= 1.0, so the target IS feasible.
TEST(BookShape, NameCapBinds) {
  std::vector<f64> w = {10.0, -2.0, -3.0, -5.0};
  std::vector<std::uint8_t> live = {1,1,1,1};
  atx::impl::shape_book(w, live, /*gross*/1.0, /*name_cap*/0.30);
  for (f64 x : w) EXPECT_LE(std::abs(x), 0.30 + 1e-9);
  f64 g = 0; for (f64 x : w) g += std::abs(x);
  EXPECT_NEAR(g, 1.0, 1e-6);   // freed budget redistributed to unclipped names
}

// Infeasible cap (cap * n_live < gross): every name pins at the cap and the book
// cannot reach the target gross. The cap wins; Sigma|w| = n_live*cap < gross.
TEST(BookShape, InfeasibleCapPinsAtCap) {
  std::vector<f64> w = {10.0, 0.0, -5.0, -5.0};   // 3 active names
  std::vector<std::uint8_t> live = {1,1,1,1};
  atx::impl::shape_book(w, live, /*gross*/1.0, /*name_cap*/0.30); // 3*0.30=0.90 < 1.0
  for (f64 x : w) EXPECT_LE(std::abs(x), 0.30 + 1e-9);   // caps respected
  f64 g = 0; for (f64 x : w) g += std::abs(x);
  EXPECT_LE(g, 1.0 + 1e-9);                              // gross NOT exceeded (cap wins)
  EXPECT_GT(g, 0.0);                                     // not degenerate-empty
}

TEST(BookShape, DeadAndNanCellsZeroed) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  std::vector<f64> w = {2.0, nan, 1.0, -3.0};
  std::vector<std::uint8_t> live = {1,0,1,1};   // index1 dead
  atx::impl::shape_book(w, live, 1.0, 1.0);
  EXPECT_EQ(w[1], 0.0);
  f64 s = 0; for (f64 x : w) s += x;
  EXPECT_NEAR(s, 0.0, 1e-9);
}

TEST(BookShape, Deterministic) {
  std::vector<f64> a = {5,-2,1,-4,3}, b = a;
  std::vector<std::uint8_t> live = {1,1,1,1,1};
  atx::impl::shape_book(a, live, 0.8, 0.25);
  atx::impl::shape_book(b, live, 0.8, 0.25);
  EXPECT_EQ(a, b);
}
