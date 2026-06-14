// fund_netting_test.cpp — P2-S2-4: Internal-crossing Netting.
//
// net_fund_book sums per-sleeve target deltas into ONE fund net trade. Offsetting
// sleeve flow crosses internally, so the fund trades the NET, not the gross. The
// honest crossing measurement (§4.4):
//   fund_book           = W = Σ_s c_s·w_s                  (order-fixed)
//   turnover_gross      = Σ_i Σ_s |c_s·Δw_{s,i}|           (sleeves traded SEPARATELY)
//   turnover_net        = Σ_i |Σ_s c_s·Δw_{s,i}|          ≤ gross (triangle, R3)
//   crossing_benefit_bps= (gross − net)·round_trip_cost_bps ≥ 0 (R3)
//   crossed_fraction    = (gross − net)/gross ∈ [0,1]      (the internal-cross rate)
//
// THE invariants (R3, asserted on EVERY fixture): turnover_net ≤ turnover_gross
// (the triangle inequality |Σd| ≤ Σ|d|) and crossing_benefit_bps ≥ 0. The benefit
// is priced through the SAME calibrated book::CostInputs.round_trip_cost_bps the
// sleeves/single-fund path use (one cost model). PIT-safe by construction: a same-
// timestamp aggregation of already-known sleeve targets — no realized return / future
// bar (§0.9).
//
// Suite FundNetting; names Subject_Condition_ExpectedResult.

#include <bit>     // std::bit_cast (determinism byte-pin)
#include <cmath>   // std::fabs
#include <cstdint> // std::uint64_t
#include <span>    // std::span
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/fund/netting.hpp"       // UNIT UNDER TEST
#include "atx/engine/risk/multi_period.hpp"  // book::CostInputs

namespace {

using atx::f64;
using atx::usize;
using atx::engine::book::CostInputs;
using atx::engine::fund::net_fund_book;
using atx::engine::fund::NetResult;

// span-of-spans over a vector-of-vectors (the call-site adapter).
[[nodiscard]] std::vector<std::span<const f64>>
spans_of(const std::vector<std::vector<f64>>& v) {
  std::vector<std::span<const f64>> out;
  out.reserve(v.size());
  for (const auto& row : v) {
    out.emplace_back(row);
  }
  return out;
}

// The triangle + non-negative-benefit invariants (R3) — asserted on EVERY fixture.
void expect_invariants(const NetResult& r, const CostInputs& cost) {
  EXPECT_LE(r.turnover_net, r.turnover_gross) << "triangle: |Σd| ≤ Σ|d|";
  EXPECT_GE(r.crossing_benefit_bps, 0.0) << "benefit ≥ 0 (gross ≥ net, cost ≥ 0)";
  EXPECT_GE(r.crossed_fraction, 0.0);
  EXPECT_LE(r.crossed_fraction, 1.0);
  // Benefit is priced through the SAME cost field, to bit (1e-12) — see TEST 6.
  EXPECT_NEAR(r.crossing_benefit_bps,
              (r.turnover_gross - r.turnover_net) * cost.round_trip_cost_bps, 1e-12);
}

// ===========================================================================
//  TEST 1 — Exact net book: fund_book ≈ Σ_s c_s·w_s (hand-computed).
// ===========================================================================
TEST(FundNetting, FundBook_KnownFixture_EqualsWeightedSum) {
  const std::vector<std::vector<f64>> books = {
      {1.0, -0.5, 0.3},
      {-0.4, 0.8, 0.1},
  };
  const std::vector<std::vector<f64>> prev = {
      {0.5, -0.2, 0.0},
      {-0.1, 0.4, 0.2},
  };
  const std::vector<f64> c = {0.6, 0.5};
  const CostInputs cost{0.0, 10.0, 1e9};

  auto r = net_fund_book(spans_of(books), spans_of(prev), c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  ASSERT_EQ(r->fund_book.size(), 3U);
  for (usize i = 0; i < 3; ++i) {
    const f64 w = c[0] * books[0][i] + c[1] * books[1][i];
    EXPECT_NEAR(r->fund_book[i], w, 1e-12) << "fund_book[" << i << "]";
  }
  expect_invariants(*r, cost);
}

// ===========================================================================
//  TEST 2 — Triangle + non-negative benefit (R3) on a mixed-sign fixture.
// ===========================================================================
TEST(FundNetting, MixedSign_AnyFixture_TriangleAndNonNegBenefit) {
  const std::vector<std::vector<f64>> books = {
      {1.0, -0.5, 0.3, 0.2, -1.0},
      {-0.4, 0.8, 0.1, -0.6, 0.5},
      {0.2, 0.2, -0.7, 0.9, 0.1},
  };
  const std::vector<std::vector<f64>> prev = {
      {0.1, -0.3, 0.0, 0.4, -0.5},
      {-0.2, 0.5, 0.3, -0.1, 0.2},
      {0.3, 0.0, -0.4, 0.6, 0.0},
  };
  const std::vector<f64> c = {0.6, 0.3, 0.5};
  const CostInputs cost{0.0, 7.5, 1e9};

  auto r = net_fund_book(spans_of(books), spans_of(prev), c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  expect_invariants(*r, cost);
  // Mixed-sign deltas DO offset ⇒ a strict crossing benefit here.
  EXPECT_GT(r->turnover_gross, r->turnover_net) << "mixed-sign deltas should cross";
  EXPECT_GT(r->crossing_benefit_bps, 0.0);
}

// ===========================================================================
//  TEST 3 — Two IDENTICAL sleeves (same w, same prev, equal c): same-sign deltas,
//  no offset ⇒ crossed_fraction == 0, turnover_net == turnover_gross.
// ===========================================================================
TEST(FundNetting, IdenticalSleeves_EqualCapital_NoCrossing) {
  const std::vector<std::vector<f64>> one = {{1.0, -0.5, 0.3, 0.2}};
  const std::vector<std::vector<f64>> books = {one[0], one[0]};
  const std::vector<std::vector<f64>> prev = {{0.1, 0.2, -0.1, 0.0}, {0.1, 0.2, -0.1, 0.0}};
  const std::vector<f64> c = {0.7, 0.7};
  const CostInputs cost{0.0, 5.0, 1e9};

  auto r = net_fund_book(spans_of(books), spans_of(prev), c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_EQ(r->turnover_net, r->turnover_gross) << "same-sign deltas ⇒ no offset";
  EXPECT_EQ(r->crossed_fraction, 0.0);
  EXPECT_EQ(r->crossing_benefit_bps, 0.0);
  // fund_book == Σ c_s·w (both books identical) = 2·c·w.
  for (usize i = 0; i < 4; ++i) {
    EXPECT_NEAR(r->fund_book[i], 2.0 * c[0] * books[0][i], 1e-12);
  }
  expect_invariants(*r, cost);
}

// ===========================================================================
//  TEST 4 — Two EXACTLY OPPOSITE sleeves, equal capital (w_1 = −w_0, prev symmetric):
//  fund_book ≈ 0, turnover_net ≈ 0, crossed_fraction ≈ 1, benefit ≈ full gross priced.
// ===========================================================================
TEST(FundNetting, OppositeSleeves_EqualCapital_FullInternalCross) {
  const std::vector<f64> w0 = {1.0, -0.5, 0.3, 0.2};
  const std::vector<f64> p0 = {0.4, -0.2, 0.1, -0.3};
  std::vector<f64> w1(w0.size()), p1(p0.size());
  for (usize i = 0; i < w0.size(); ++i) {
    w1[i] = -w0[i];
    p1[i] = -p0[i];
  }
  const std::vector<std::vector<f64>> books = {w0, w1};
  const std::vector<std::vector<f64>> prev = {p0, p1};
  const std::vector<f64> c = {0.8, 0.8}; // equal capital ⇒ exact cancellation
  const CostInputs cost{0.0, 12.0, 1e9};

  auto r = net_fund_book(spans_of(books), spans_of(prev), c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  for (usize i = 0; i < 4; ++i) {
    EXPECT_NEAR(r->fund_book[i], 0.0, 1e-12) << "opposite sleeves net to a flat book";
  }
  EXPECT_NEAR(r->turnover_net, 0.0, 1e-12) << "full internal cross ⇒ ~zero net trade";
  EXPECT_NEAR(r->crossed_fraction, 1.0, 1e-12) << "full internal cross";
  EXPECT_NEAR(r->crossing_benefit_bps, r->turnover_gross * cost.round_trip_cost_bps, 1e-12)
      << "the entire gross turnover is saved";
  expect_invariants(*r, cost);
}

// ===========================================================================
//  TEST 5 — Empty sleeve_prev ⇒ first move from flat: prev treated as 0;
//  deltas == c_s·w_s; turnovers computed against a flat book.
// ===========================================================================
TEST(FundNetting, EmptyPrev_FirstMove_TreatsPrevAsZero) {
  const std::vector<std::vector<f64>> books = {
      {1.0, -0.5, 0.3},
      {-0.4, 0.8, 0.1},
  };
  const std::vector<f64> c = {0.6, 0.5};
  const CostInputs cost{0.0, 10.0, 1e9};

  // Empty prev ⇒ first move from flat (all prev = 0).
  auto r = net_fund_book(spans_of(books), {}, c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  // Oracle: gross = Σ_i Σ_s |c_s·w_s|; net = Σ_i |Σ_s c_s·w_s| (prev=0).
  f64 gross = 0.0, net = 0.0;
  for (usize i = 0; i < 3; ++i) {
    f64 nd = 0.0;
    for (usize s = 0; s < 2; ++s) {
      const f64 d = c[s] * books[s][i];
      gross += std::fabs(d);
      nd += d;
    }
    net += std::fabs(nd);
  }
  EXPECT_NEAR(r->turnover_gross, gross, 1e-12);
  EXPECT_NEAR(r->turnover_net, net, 1e-12);
  // fund_book unchanged by empty-prev (it's Σ c_s·w_s either way).
  for (usize i = 0; i < 3; ++i) {
    EXPECT_NEAR(r->fund_book[i], c[0] * books[0][i] + c[1] * books[1][i], 1e-12);
  }
  expect_invariants(*r, cost);
}

// ===========================================================================
//  TEST 6 — Benefit pricing exact: benefit == (gross − net)·round_trip_cost_bps.
// ===========================================================================
TEST(FundNetting, Benefit_AnyFixture_EqualsTurnoverGapTimesCost) {
  const std::vector<std::vector<f64>> books = {
      {1.0, -0.5, 0.3, 0.2},
      {-0.4, 0.8, 0.1, -0.6},
  };
  const std::vector<std::vector<f64>> prev = {
      {0.1, -0.3, 0.0, 0.4},
      {-0.2, 0.5, 0.3, -0.1},
  };
  const std::vector<f64> c = {0.6, 0.5};
  const CostInputs cost{0.0, 9.25, 1e9};

  auto r = net_fund_book(spans_of(books), spans_of(prev), c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_NEAR(r->crossing_benefit_bps,
              (r->turnover_gross - r->turnover_net) * cost.round_trip_cost_bps, 1e-12);
  expect_invariants(*r, cost);
}

// ===========================================================================
//  TEST 7 — Determinism (R1): two calls on identical inputs ⇒ byte-identical.
// ===========================================================================
TEST(FundNetting, Determinism_IdenticalInputs_ByteIdentical) {
  const std::vector<std::vector<f64>> books = {
      {1.0, -0.5, 0.3, 0.2, -1.0},
      {-0.4, 0.8, 0.1, -0.6, 0.5},
      {0.2, 0.2, -0.7, 0.9, 0.1},
  };
  const std::vector<std::vector<f64>> prev = {
      {0.1, -0.3, 0.0, 0.4, -0.5},
      {-0.2, 0.5, 0.3, -0.1, 0.2},
      {0.3, 0.0, -0.4, 0.6, 0.0},
  };
  const std::vector<f64> c = {0.6, 0.3, 0.5};
  const CostInputs cost{0.0, 8.0, 1e9};

  auto a = net_fund_book(spans_of(books), spans_of(prev), c, cost);
  auto b = net_fund_book(spans_of(books), spans_of(prev), c, cost);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a->fund_book.size(), b->fund_book.size());
  for (usize i = 0; i < a->fund_book.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->fund_book[i]),
              std::bit_cast<std::uint64_t>(b->fund_book[i]))
        << "non-determinism in fund_book[" << i << "]";
  }
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a->turnover_gross),
            std::bit_cast<std::uint64_t>(b->turnover_gross));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a->turnover_net),
            std::bit_cast<std::uint64_t>(b->turnover_net));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a->crossing_benefit_bps),
            std::bit_cast<std::uint64_t>(b->crossing_benefit_bps));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a->crossed_fraction),
            std::bit_cast<std::uint64_t>(b->crossed_fraction));
}

// ===========================================================================
//  TEST 8 — S=1 (single sleeve): no crossing possible ⇒ net == gross,
//  crossed_fraction == 0, fund_book == c_0·w_0.
// ===========================================================================
TEST(FundNetting, SingleSleeve_NoCrossingPossible_NetEqualsGross) {
  const std::vector<std::vector<f64>> books = {{1.0, -0.5, 0.3, 0.2}};
  const std::vector<std::vector<f64>> prev = {{0.4, -0.2, 0.1, -0.3}};
  const std::vector<f64> c = {1.5};
  const CostInputs cost{0.0, 6.0, 1e9};

  auto r = net_fund_book(spans_of(books), spans_of(prev), c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_EQ(r->turnover_net, r->turnover_gross) << "S=1 ⇒ |d| == |Σd|, no crossing";
  EXPECT_EQ(r->crossed_fraction, 0.0);
  EXPECT_EQ(r->crossing_benefit_bps, 0.0);
  for (usize i = 0; i < 4; ++i) {
    EXPECT_NEAR(r->fund_book[i], c[0] * books[0][i], 1e-12);
  }
  expect_invariants(*r, cost);
}

// ===========================================================================
//  TEST 9 — Degenerate: S==0 ⇒ Ok empty fund_book, zero turnovers; M==0 ⇒ Ok empty.
// ===========================================================================
TEST(FundNetting, NoSleeves_S0_OkEmptyZero) {
  const std::vector<std::span<const f64>> empty_books;
  const std::vector<f64> c; // S == 0
  const CostInputs cost{0.0, 10.0, 1e9};

  auto r = net_fund_book(empty_books, {}, c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_TRUE(r->fund_book.empty());
  EXPECT_EQ(r->turnover_gross, 0.0);
  EXPECT_EQ(r->turnover_net, 0.0);
  EXPECT_EQ(r->crossing_benefit_bps, 0.0);
  EXPECT_EQ(r->crossed_fraction, 0.0);
  expect_invariants(*r, cost);
}

TEST(FundNetting, ZeroNames_M0_OkEmpty) {
  const std::vector<std::vector<f64>> books = {{}, {}}; // S=2, M=0
  const std::vector<f64> c = {0.6, 0.5};
  const CostInputs cost{0.0, 10.0, 1e9};

  auto r = net_fund_book(spans_of(books), {}, c, cost);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_TRUE(r->fund_book.empty());
  EXPECT_EQ(r->turnover_gross, 0.0);
  EXPECT_EQ(r->turnover_net, 0.0);
  EXPECT_EQ(r->crossing_benefit_bps, 0.0);
  EXPECT_EQ(r->crossed_fraction, 0.0);
  expect_invariants(*r, cost);
}

// ===========================================================================
//  TEST 10 — Errors: shape mismatches ⇒ Err(InvalidArgument).
// ===========================================================================
TEST(FundNetting, ShapeMismatch_ReturnsErr) {
  const std::vector<std::vector<f64>> books = {
      {1.0, -0.5, 0.3},
      {-0.4, 0.8, 0.1},
  };
  const CostInputs cost{0.0, 10.0, 1e9};

  // sleeve_books.size() != c.size() (2 books, 3 weights).
  {
    const std::vector<f64> c = {0.6, 0.3, 0.5};
    auto r = net_fund_book(spans_of(books), {}, c, cost);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  // a sleeve_books[s] of wrong length (second book truncated to M-1).
  {
    std::vector<std::vector<f64>> bad = books;
    bad[1].pop_back();
    const std::vector<f64> c = {0.6, 0.5};
    auto r = net_fund_book(spans_of(bad), {}, c, cost);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  // sleeve_prev.size() (non-empty) != S (1 prev, 2 sleeves).
  {
    const std::vector<std::vector<f64>> prev1 = {{0.0, 0.0, 0.0}};
    const std::vector<f64> c = {0.6, 0.5};
    auto r = net_fund_book(spans_of(books), spans_of(prev1), c, cost);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  // a sleeve_prev[s] of wrong length (second prev truncated to M-1).
  {
    std::vector<std::vector<f64>> prev = {{0.1, 0.2, 0.3}, {0.4, 0.5}};
    const std::vector<f64> c = {0.6, 0.5};
    auto r = net_fund_book(spans_of(books), spans_of(prev), c, cost);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

} // namespace
