// Parity + round-trip gate for the vectorized (AVX2) implied-vol batch kernel.
//
// The batch entry point dispatches to the 4-lane AVX2 path when the host
// supports it (this CI/dev box does — Alder Lake). These tests assert that:
//   1. Round-trip: pricing a grid with black76_price at a known σ and inverting
//      with implied_vol_batch recovers that σ (and matches the scalar
//      implied_vol) to a tight combined abs+rel tolerance. Accepted AVX2 lanes
//      land within ~1e-9 of scalar; degenerate / deep-wing / ill-conditioned /
//      non-converged lanes patch through the exact scalar inverter, so they are
//      bit-for-bit with scalar there.
//   2. No-arb-violating prices (above the upper bound, below intrinsic) and
//      degenerate inputs are flagged ok_out == 0, exactly as scalar errors.
//   3. The scalar tail (n % 4 != 0) is handled for every residue class.
// If AVX2 is absent the batch runs the scalar loop and these become identity
// checks — still valid, just trivially exact.

#include "atx/vol/simd/iv_batch.hpp"

#include "atx/vol/black76.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// A broad, deterministic grid spanning ITM/OTM/ATM, short/long tenors, low/high
// vol, both sides. sigma_in is retained so the round-trip can check recovery.
struct Grid {
  std::vector<double> price, F, K, T, df, sigma_in;
  std::vector<Side> side;
  [[nodiscard]] std::size_t size() const { return F.size(); }
  void push(double f, double k, double t, double s, double d, Side sd) {
    const double p = black76_price(f, k, t, s, d, sd);
    price.push_back(p);
    F.push_back(f);
    K.push_back(k);
    T.push_back(t);
    df.push_back(d);
    sigma_in.push_back(s);
    side.push_back(sd);
  }
};

Grid make_grid() {
  Grid g;
  const double forwards[] = {50.0, 100.0, 250.0};
  const double moneyness[] = {0.80, 0.90, 0.95, 1.0, 1.05, 1.10, 1.20};
  const double tenors[] = {0.10, 0.25, 0.50, 1.0, 2.0};
  const double vols[] = {0.15, 0.25, 0.40};
  for (double F : forwards)
    for (double m : moneyness)
      for (double T : tenors)
        for (double v : vols) {
          const double df = std::exp(-0.03 * T);
          g.push(F, F * m, T, v, df, Side::Call);
          g.push(F, F * m, T, v, df, Side::Put);
        }
  return g;
}

// ── 2.10: the noexcept boundary is a promise, not a hint ─────────────────────
//
// simd/iv_batch.hpp declares implied_vol_batch noexcept, but each lane calls the
// scalar `implied_vol`, which is not noexcept: a failing lane composes an Error
// message longer than any SSO buffer, so it allocates. Under memory pressure the
// escaping std::bad_alloc was std::terminate. The lane now contains bad_alloc and
// reports the documented (NaN, ok == 0) failure instead.
//
// Allocation failure is not injectable from a unit test, so this pins the half
// that IS machine-checkable: the declaration. Silently "fixing" the mismatch by
// dropping noexcept — an ABI-visible change to a public entry point — breaks the
// build here rather than passing unnoticed.
static_assert(noexcept(simd::implied_vol_batch(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                               nullptr, nullptr, 0U)),
              "implied_vol_batch's noexcept is a public contract: contain the throw, do not drop "
              "the specifier");

TEST(SimdImpliedVolBatch, RoundTripMatchesInputAndScalar) {
  const Grid g = make_grid();
  const std::size_t n = g.size();
  std::vector<double> iv(n, 0.0);
  std::vector<std::uint8_t> ok(n, 0);
  simd::implied_vol_batch(g.price.data(), g.F.data(), g.K.data(), g.T.data(), g.df.data(),
                          g.side.data(), iv.data(), ok.data(), n);

  // Accepted AVX2 lanes carry the Cheb-Φ σ bias (~1e-9); patched lanes are exact
  // (~1e-12). A combined abs+rel tol of 1e-8 leaves ~10× headroom on both.
  constexpr double kAbs = 1e-8;
  constexpr double kRel = 1e-8;
  double max_vs_input = 0.0;
  double max_vs_scalar = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_EQ(ok[i], 1u) << "i=" << i;
    ASSERT_TRUE(std::isfinite(iv[i])) << "i=" << i;

    const Result<double> r = implied_vol(g.price[i], g.F[i], g.K[i], g.T[i], g.df[i], g.side[i]);
    ASSERT_TRUE(r.has_value()) << "i=" << i;

    const double e_in = std::abs(iv[i] - g.sigma_in[i]);
    const double e_sc = std::abs(iv[i] - *r);
    max_vs_input = std::max(max_vs_input, e_in);
    max_vs_scalar = std::max(max_vs_scalar, e_sc);
    EXPECT_LE(e_in, kAbs + kRel * g.sigma_in[i])
        << "i=" << i << " iv=" << iv[i] << " sigma_in=" << g.sigma_in[i];
    EXPECT_LE(e_sc, kAbs + kRel * std::abs(*r)) << "i=" << i << " iv=" << iv[i] << " scalar=" << *r;
  }
  EXPECT_LT(max_vs_input, kAbs);
  EXPECT_LT(max_vs_scalar, kAbs);
}

// Prices outside the no-arb band and degenerate inputs must be flagged ok == 0,
// with the scalar inverter as the source of truth.
TEST(SimdImpliedVolBatch, NoArbViolationsAndDegenerateFlagged) {
  struct Row {
    double price, F, K, T, df;
    Side side;
  };
  const double df = std::exp(-0.03);
  const std::vector<Row> rows = {
      {1.20 * 100.0 * df, 100.0, 95.0, 1.0, df, Side::Call}, // call > upper F·df
      {1.20 * 105.0 * df, 100.0, 105.0, 1.0, df, Side::Put}, // put  > upper K·df
      {-0.5, 100.0, 100.0, 1.0, df, Side::Call},             // negative premium
      {5.0, 100.0, 100.0, 0.0, 1.0, Side::Call},             // T == 0 degenerate
      {5.0, 100.0, 100.0, -1.0, 1.0, Side::Put},             // T < 0 degenerate
  };
  const std::size_t n = rows.size();
  std::vector<double> price(n), F(n), K(n), T(n), dfv(n);
  std::vector<Side> side(n);
  for (std::size_t i = 0; i < n; ++i) {
    price[i] = rows[i].price;
    F[i] = rows[i].F;
    K[i] = rows[i].K;
    T[i] = rows[i].T;
    dfv[i] = rows[i].df;
    side[i] = rows[i].side;
  }
  std::vector<double> iv(n, 0.0);
  std::vector<std::uint8_t> ok(n, 7);
  simd::implied_vol_batch(price.data(), F.data(), K.data(), T.data(), dfv.data(), side.data(),
                          iv.data(), ok.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(ok[i], 0u) << "i=" << i;
    // Cross-check the scalar inverter agrees this input has no valid IV.
    const Result<double> r = implied_vol(price[i], F[i], K[i], T[i], dfv[i], side[i]);
    EXPECT_FALSE(r.has_value()) << "i=" << i;
  }
}

// A price at (or below) intrinsic implies σ → 0: clamps to kIvMin, ok == 1.
// Near-intrinsic / near-upper-bound prices sit on the no-arb band edge, where
// the scalar inverter either clamps to kIvMin or rejects as OutOfRange depending
// on where the (fp round-tripped) price falls relative to the band's 1e-15
// tolerance. The batch's contract is to reproduce that scalar decision exactly
// (these lanes patch through the scalar inverter), so this asserts bit-for-bit
// parity with scalar rather than presuming a specific clamp/reject outcome.
TEST(SimdImpliedVolBatch, BandEdgePricesMatchScalar) {
  const double df = std::exp(-0.03);
  std::vector<double> price, F, K, T, dfv;
  std::vector<Side> side;
  const auto add = [&](double p, double f, double k, Side s) {
    price.push_back(p);
    F.push_back(f);
    K.push_back(k);
    T.push_back(1.0);
    dfv.push_back(df);
    side.push_back(s);
  };
  // Deep-ITM call/put at (and a hair above) discounted intrinsic, and a hair
  // below the discounted upper bound — the σ→0 and σ→∞ band corners.
  add(df * (100.0 - 60.0), 100.0, 60.0, Side::Call);        // call at intrinsic
  add(df * (100.0 - 60.0) + 1e-4, 100.0, 60.0, Side::Call); // just inside
  add(df * (90.0 - 40.0), 40.0, 90.0, Side::Put);           // put at intrinsic
  add(df * 100.0 - 1e-4, 100.0, 60.0, Side::Call);          // near upper bound
  add(df * (120.0 - 70.0), 120.0, 70.0, Side::Call);        // another ITM lane

  const std::size_t n = price.size();
  std::vector<double> iv(n, 0.0);
  std::vector<std::uint8_t> ok(n, 0);
  simd::implied_vol_batch(price.data(), F.data(), K.data(), T.data(), dfv.data(), side.data(),
                          iv.data(), ok.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const Result<double> want = implied_vol(price[i], F[i], K[i], T[i], dfv[i], side[i]);
    EXPECT_EQ(ok[i] != 0, want.has_value()) << "i=" << i;
    if (want && ok[i]) {
      EXPECT_DOUBLE_EQ(iv[i], *want) << "i=" << i; // patched ⇒ bit-exact
    }
  }
}

// The scalar tail (n % 4 != 0) must be handled for every residue class.
TEST(SimdImpliedVolBatch, HandlesEveryTailResidue) {
  const Grid g = make_grid();
  for (std::size_t n = 1; n <= 11; ++n) {
    std::vector<double> iv(n, 0.0);
    std::vector<std::uint8_t> ok(n, 0);
    simd::implied_vol_batch(g.price.data(), g.F.data(), g.K.data(), g.T.data(), g.df.data(),
                            g.side.data(), iv.data(), ok.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      EXPECT_EQ(ok[i], 1u) << "n=" << n << " i=" << i;
      const Result<double> r = implied_vol(g.price[i], g.F[i], g.K[i], g.T[i], g.df[i], g.side[i]);
      ASSERT_TRUE(r.has_value()) << "n=" << n << " i=" << i;
      EXPECT_LE(std::abs(iv[i] - *r), 1e-8 + 1e-8 * std::abs(*r)) << "n=" << n << " i=" << i;
    }
  }
}

TEST(SimdImpliedVolBatch, NonCallSideMatchesScalarAcrossBlockAndTail) {
  constexpr std::size_t kN = 5;
  const double discount = std::exp(-0.03 * 0.5);
  const Side non_call = static_cast<Side>(0xffU);
  const std::vector<double> F(kN, 100.0);
  const std::vector<double> K(kN, 105.0);
  const std::vector<double> T(kN, 0.5);
  const std::vector<double> df(kN, discount);
  const std::vector<Side> side(kN, non_call);
  std::vector<double> price(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    price[i] = black76_price(F[i], K[i], T[i], 0.25, df[i], non_call);
  }
  std::vector<double> iv(kN);
  std::vector<std::uint8_t> ok(kN);
  simd::implied_vol_batch(price.data(), F.data(), K.data(), T.data(), df.data(), side.data(),
                          iv.data(), ok.data(), kN);
  for (std::size_t i = 0; i < kN; ++i) {
    const Result<double> scalar = implied_vol(price[i], F[i], K[i], T[i], df[i], non_call);
    ASSERT_EQ(ok[i] != 0U, scalar.has_value()) << "lane " << i;
    ASSERT_TRUE(scalar.has_value()) << "lane " << i;
    EXPECT_LE(std::abs(iv[i] - *scalar), 1.0e-8 + 1.0e-8 * std::abs(*scalar)) << "lane " << i;
  }
}

TEST(SimdImpliedVolBatch, ZeroLengthIsNoOp) {
  double iv_sentinel = 42.0;
  std::uint8_t ok_sentinel = 9;
  simd::implied_vol_batch(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &iv_sentinel,
                          &ok_sentinel, 0);
  EXPECT_EQ(iv_sentinel, 42.0);
  EXPECT_EQ(ok_sentinel, 9u);
}

} // namespace
} // namespace atx::vol
