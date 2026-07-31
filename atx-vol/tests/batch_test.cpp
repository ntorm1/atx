#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/batch.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/surface.hpp"
#include "atx/vol/types.hpp"

// Public batch API parity tests, ported from the C ats-vol test_pricer_simd.c.
// Interior lanes may take the runtime-dispatched AVX2 kernels, whose vector
// transcendental approximations are economically immaterial but not bit
// identical to the scalar source of truth. Degenerate lanes retain exact scalar
// patch-through; deep-wing lanes are machine-accurate on the Cody-erfc vector Φ
// (K2). Length and alias guards cover the public boundary — exact in==out
// identity aliasing is permitted in place (R-23), staggered overlap rejected.

namespace {

using atx::vol::black76_greeks;
using atx::vol::black76_greeks_batch;
using atx::vol::black76_price;
using atx::vol::black76_price_batch;
using atx::vol::black76_price_from_lnfk;
using atx::vol::black76_price_from_lnfk_batch;
using atx::vol::black76_value_and_vega;
using atx::vol::black76_value_and_vega_batch;
using atx::vol::Black76Greeks;
using atx::vol::Black76ValueVega;
using atx::vol::ErrorCode;
using atx::vol::essvi_w;
using atx::vol::essvi_w_batch;
using atx::vol::EssviSlice;
using atx::vol::Greeks;
using atx::vol::implied_vol;
using atx::vol::implied_vol_batch;
using atx::vol::Result;
using atx::vol::Side;
using atx::vol::Status;

// ── Reproducible SoA grid (mirrors test_pricer_simd.c's xorshift seed) ────

struct Grid {
  std::vector<double> F, K, T, sigma, df;
  std::vector<Side> side;
};

Grid make_grid(std::size_t n) {
  Grid g;
  g.F.resize(n);
  g.K.resize(n);
  g.T.resize(n);
  g.sigma.resize(n);
  g.df.resize(n);
  g.side.resize(n);

  std::uint64_t s = 0xa5a5a5a5deadbeefULL;
  auto next_u = [&s]() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<double>(s & 0xFFFFFFFFULL) / 4294967296.0;
  };

  for (std::size_t i = 0; i < n; ++i) {
    const double u1 = next_u();
    const double u2 = next_u();
    const double u3 = next_u();
    const double u4 = next_u();
    g.F[i] = 80.0 + 40.0 * u1; // F in [80, 120]
    g.K[i] = 80.0 + 40.0 * u2;
    g.T[i] = 30.0 / 365.25 + 1.5 * u3; // T in ~[0.08, 1.6] y
    g.sigma[i] = 0.10 + 0.60 * u4;     // sigma in [0.10, 0.70]
    g.df[i] = std::exp(-0.05 * g.T[i]);
    g.side[i] = (i & 1U) ? Side::Put : Side::Call;
  }
  return g;
}

constexpr std::size_t kN = 1024;

void expect_close(double got, double want, double abs_tol, double rel_tol, std::size_t lane) {
  EXPECT_LE(std::abs(got - want), abs_tol + rel_tol * std::abs(want))
      << "lane " << lane << " got=" << got << " want=" << want;
}

void expect_scalar_semantics(double got, double want, double abs_tol, double rel_tol,
                             std::size_t lane) {
  if (std::isnan(want)) {
    EXPECT_TRUE(std::isnan(got)) << "lane " << lane << " got=" << got;
    return;
  }
  if (std::isinf(want)) {
    EXPECT_EQ(got, want) << "lane " << lane;
    return;
  }
  expect_close(got, want, abs_tol, rel_tol, lane);
}

void inject_invalid_domains(Grid &g) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  g.F[0] = nan;
  g.F[1] = inf;
  g.F[2] = -1.0;
  g.K[3] = nan;
  g.K[4] = inf;
  g.K[5] = -1.0;
  g.T[6] = nan;
  g.T[7] = inf;
  g.sigma[8] = nan;
  g.sigma[9] = inf;
  g.df[10] = nan;
  g.df[11] = inf;
}

// ── Price batch == scalar over the random grid ───────────────────────────

TEST(Batch, Price_MatchesScalar_RandomGrid) {
  const Grid g = make_grid(kN);
  std::vector<double> batch(kN);
  const Result<std::size_t> st =
      black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(batch));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < kN; ++i) {
    const double scalar = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
    expect_close(batch[i], scalar, 1.0e-6, 1.0e-7, i);
  }
}

// ── Degenerate lanes (T = 0, sigma = 0) fall through to intrinsic ─────────

TEST(Batch, Price_DegenerateLanes_MatchScalar) {
  const std::vector<double> F = {100.0, 100.0, 100.0, 110.0, 90.0};
  const std::vector<double> K = {100.0, 100.0, 105.0, 105.0, 95.0};
  const std::vector<double> T = {0.5, 0.0, 0.5, 0.5, 0.5};
  const std::vector<double> sigma = {0.20, 0.20, 0.0, 0.25, 0.30};
  const std::vector<double> df = {1.0, 1.0, 1.0, 1.0, 1.0};
  const std::vector<Side> side = {Side::Call, Side::Call, Side::Put, Side::Call, Side::Put};
  std::vector<double> batch(F.size());
  const Result<std::size_t> st =
      black76_price_batch(F, K, T, sigma, df, side, std::span<double>(batch));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < F.size(); ++i) {
    const double scalar = black76_price(F[i], K[i], T[i], sigma[i], df[i], side[i]);
    if (T[i] <= 0.0 || sigma[i] <= 0.0) {
      EXPECT_DOUBLE_EQ(batch[i], scalar);
    } else {
      expect_close(batch[i], scalar, 1.0e-6, 1.0e-7, i);
    }
  }
  // Sanity anchors from the C test: T=0 ATM call -> 0; sigma=0 K=105 put -> 5.
  EXPECT_DOUBLE_EQ(batch[1], 0.0);
  EXPECT_DOUBLE_EQ(batch[2], 5.0);
}

TEST(Batch, Price_ShortBatchRetainsExactScalarSemantics) {
  const Grid g = make_grid(3);
  std::vector<double> batch(3);
  const Result<std::size_t> st =
      black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(batch));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < batch.size(); ++i) {
    EXPECT_DOUBLE_EQ(batch[i],
                     black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]));
  }
}

TEST(Batch, Price_LengthMismatch_InvalidArgument) {
  const Grid g = make_grid(8);
  std::vector<double> out(7); // one short
  const Result<std::size_t> st =
      black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(out));
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

// R-23: exact in==out identity aliasing is PERMITTED at the public boundary and
// computes in place, bit-for-bit with the disjoint-output result. The kernel
// loads each 4-lane block (and the scalar tail element) fully before its
// same-index store, so an output that exactly coincides with an input reads
// every value before it is overwritten. 70 rows exercise the AVX2 blocks and the
// 2-lane scalar tail.
TEST(Batch, Price_ExactInPlaceAliasing_MatchesDisjoint) {
  const Grid g = make_grid(70);
  std::vector<double> ref(g.F.size());
  ASSERT_TRUE(
      black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(ref)).has_value());
  std::vector<double> f = g.F; // serves as both the F input and the price output
  const Result<std::size_t> st = black76_price_batch(std::span<const double>(f), g.K, g.T, g.sigma,
                                                     g.df, g.side, std::span<double>(f));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    EXPECT_DOUBLE_EQ(f[i], ref[i]) << "lane " << i;
  }
}

// A staggered (non-identity) overlap of output onto an input is NOT safe in
// place and must still be rejected: shift price_out by one element so it shares
// storage with F without exactly coinciding.
TEST(Batch, Price_PartialOverlapInput_InvalidArgument) {
  const Grid g = make_grid(8);
  std::vector<double> buf(g.F.size() + 1);
  std::copy(g.F.begin(), g.F.end(), buf.begin());
  const std::span<const double> F_in(buf.data(), g.F.size());    // buf[0 .. n-1]
  const std::span<double> price_out(buf.data() + 1, g.F.size()); // buf[1 .. n]  (staggered)
  const Result<std::size_t> st =
      black76_price_batch(F_in, g.K, g.T, g.sigma, g.df, g.side, price_out);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

// ── from-lnFK batch == scalar (T, sqrt_t, df shared per slice) ────────────

TEST(Batch, FromLnfk_MatchesScalar_RandomGrid) {
  const Grid g = make_grid(kN);
  const double T = 0.35; // shared expiry slice
  const double sqrt_t = std::sqrt(T);
  const double df = std::exp(-0.05 * T);
  std::vector<double> ln_fk(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    ln_fk[i] = std::log(g.F[i] / g.K[i]);
  }
  std::vector<double> batch(kN);
  const Result<std::size_t> st = black76_price_from_lnfk_batch(
      g.F, g.K, T, sqrt_t, g.sigma, df, ln_fk, g.side, std::span<double>(batch));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < kN; ++i) {
    const double scalar =
        black76_price_from_lnfk(g.F[i], g.K[i], T, g.sigma[i], df, ln_fk[i], sqrt_t, g.side[i]);
    EXPECT_DOUBLE_EQ(batch[i], scalar) << "lane " << i;
  }
}

TEST(Batch, FromLnfk_LengthMismatch_InvalidArgument) {
  const Grid g = make_grid(8);
  std::vector<double> ln_fk(8, 0.0);
  std::vector<double> out(8);
  // sigma one short.
  const Result<std::size_t> st = black76_price_from_lnfk_batch(
      g.F, g.K, 0.35, std::sqrt(0.35), std::span<const double>(g.sigma.data(), 7),
      std::exp(-0.05 * 0.35), ln_fk, g.side, std::span<double>(out));
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(Batch, FromLnfk_ExactInPlaceAliasing_MatchesDisjoint) {
  const Grid g = make_grid(70);
  const double T = 0.35;
  const double sqrt_t = std::sqrt(T);
  const double df = std::exp(-0.05 * T);
  std::vector<double> ln_fk(g.F.size());
  for (std::size_t i = 0; i < g.F.size(); ++i) {
    ln_fk[i] = std::log(g.F[i] / g.K[i]);
  }
  std::vector<double> ref(g.F.size());
  ASSERT_TRUE(black76_price_from_lnfk_batch(g.F, g.K, T, sqrt_t, g.sigma, df, ln_fk, g.side,
                                            std::span<double>(ref))
                  .has_value());
  std::vector<double> f = g.F; // both the F input and the price output
  const Result<std::size_t> st = black76_price_from_lnfk_batch(
      std::span<const double>(f), g.K, T, sqrt_t, g.sigma, df, ln_fk, g.side, std::span<double>(f));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    EXPECT_DOUBLE_EQ(f[i], ref[i]) << "lane " << i;
  }
}

// ── value+vega batch == scalar (T shared; sentinel and supplied sqrt_t) ───

TEST(Batch, ValueAndVega_MatchesScalar_RandomGrid) {
  const Grid g = make_grid(kN);
  const double T = 0.5;
  std::vector<double> value(kN);
  std::vector<double> vega(kN);
  const Result<std::size_t> st = black76_value_and_vega_batch(g.F, g.K, T, g.sigma, g.df, g.side,
                                                 std::span<double>(value), std::span<double>(vega));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < kN; ++i) {
    const Black76ValueVega vv =
        black76_value_and_vega(g.F[i], g.K[i], T, g.sigma[i], g.df[i], g.side[i]);
    expect_close(value[i], vv.price, 1.0e-6, 1.0e-7, i);
    expect_close(vega[i], vv.vega, 1.0e-5, 1.0e-7, i);
  }
}

TEST(Batch, ValueAndVega_SuppliedSqrtT_MatchesScalar) {
  const Grid g = make_grid(64);
  const double T = 0.6;
  // A deliberately non-canonical supplied root proves the SIMD route preserves
  // the public API's "use as-is" contract rather than recomputing sqrt(T).
  const double sqrt_t = 0.7;
  std::vector<double> value(64);
  std::vector<double> vega(64);
  const Result<std::size_t> st =
      black76_value_and_vega_batch(g.F, g.K, T, g.sigma, g.df, g.side, std::span<double>(value),
                                   std::span<double>(vega), sqrt_t);
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < 64; ++i) {
    const Black76ValueVega vv =
        black76_value_and_vega(g.F[i], g.K[i], T, g.sigma[i], g.df[i], g.side[i], sqrt_t);
    expect_close(value[i], vv.price, 1.0e-6, 1.0e-7, i);
    expect_close(vega[i], vv.vega, 1.0e-5, 1.0e-7, i);
  }
}

TEST(Batch, ValueAndVega_LengthMismatch_InvalidArgument) {
  const Grid g = make_grid(8);
  std::vector<double> value(8);
  std::vector<double> vega(7); // one short
  const Result<std::size_t> st = black76_value_and_vega_batch(g.F, g.K, 0.5, g.sigma, g.df, g.side,
                                                 std::span<double>(value), std::span<double>(vega));
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

// Two OUTPUTS sharing storage is always a caller bug — rejected even at exact
// identity (unlike output↔input aliasing, which is permitted in place).
TEST(Batch, ValueAndVega_OutputsAlias_InvalidArgument) {
  const Grid g = make_grid(8);
  std::vector<double> out(8);
  const Result<std::size_t> st = black76_value_and_vega_batch(g.F, g.K, 0.5, g.sigma, g.df, g.side,
                                                 std::span<double>(out), std::span<double>(out));
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(Batch, ValueAndVega_ValueOutExactInPlaceAliasing_MatchesDisjoint) {
  const Grid g = make_grid(70);
  const double T = 0.5;
  std::vector<double> ref_value(g.F.size());
  std::vector<double> ref_vega(g.F.size());
  ASSERT_TRUE(black76_value_and_vega_batch(g.F, g.K, T, g.sigma, g.df, g.side,
                                           std::span<double>(ref_value), std::span<double>(ref_vega))
                  .has_value());
  // value_out aliases the F input exactly; vega_out is a disjoint buffer.
  std::vector<double> f = g.F;
  std::vector<double> vega(g.F.size());
  const Result<std::size_t> st =
      black76_value_and_vega_batch(std::span<const double>(f), g.K, T, g.sigma, g.df, g.side,
                                   std::span<double>(f), std::span<double>(vega));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < ref_value.size(); ++i) {
    EXPECT_DOUBLE_EQ(f[i], ref_value[i]) << "value lane " << i;
    EXPECT_DOUBLE_EQ(vega[i], ref_vega[i]) << "vega lane " << i;
  }
}

// ── IV batch == scalar over round-tripped prices; parallel status ─────────

TEST(Batch, ImpliedVol_MatchesScalar_RandomGrid) {
  const Grid g = make_grid(kN);
  // Price each lane, then invert the batch and the scalar reference.
  std::vector<double> price(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    price[i] = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
  }
  std::vector<double> iv(kN);
  std::vector<Status> status(kN);
  const Result<std::size_t> st = implied_vol_batch(price, g.F, g.K, g.T, g.df, g.side,
                                                   std::span<double>(iv),
                                                   std::span<Status>(status));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < kN; ++i) {
    const auto scalar = implied_vol(price[i], g.F[i], g.K[i], g.T[i], g.df[i], g.side[i]);
    ASSERT_EQ(status[i].has_value(), scalar.has_value()) << "lane " << i;
    if (scalar.has_value()) {
      expect_close(iv[i], *scalar, 5.0e-8, 1.0e-8, i);
    }
  }
}

TEST(Batch, ImpliedVol_FailureLane_NaNAndErrorStatus) {
  // Lane 1 gets an out-of-band price (above the F*df upper bound) -> the scalar
  // solver rejects it; the batch must write NaN + a non-Ok status there.
  const std::vector<double> F = {100.0, 100.0};
  const std::vector<double> K = {100.0, 100.0};
  const std::vector<double> T = {0.5, 0.5};
  const std::vector<double> df = {1.0, 1.0};
  const std::vector<Side> side = {Side::Call, Side::Call};
  std::vector<double> price = {0.0, 0.0};
  price[0] = black76_price(F[0], K[0], T[0], 0.25, df[0], side[0]); // valid
  price[1] = 2.0 * F[1] * df[1];                                    // impossible premium

  std::vector<double> iv(2);
  std::vector<Status> status(2);
  const Result<std::size_t> st =
      implied_vol_batch(price, F, K, T, df, side, std::span<double>(iv), std::span<Status>(status));
  ASSERT_TRUE(st.has_value()); // arguments were well-formed
  EXPECT_TRUE(status[0].has_value());
  EXPECT_FALSE(std::isnan(iv[0]));
  EXPECT_FALSE(status[1].has_value());
  EXPECT_TRUE(std::isnan(iv[1]));
  // Batch failure status equals what the scalar returns for the same lane.
  const auto scalar = implied_vol(price[1], F[1], K[1], T[1], df[1], side[1]);
  ASSERT_FALSE(scalar.has_value());
  EXPECT_EQ(status[1].error().code(), scalar.error().code());
}

TEST(Batch, ImpliedVol_FailureLanesPreserveScalarErrorCodes) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  // Eight lanes force two full SIMD blocks. The failures cover OutOfRange and
  // InvalidArgument, proving the compact AVX2 ok mask is expanded back into
  // each scalar lane's specific error rather than a generic batch failure.
  const std::vector<double> price = {200.0, 5.0, nan, 200.0, 5.0, nan, -1.0, 200.0};
  const std::vector<double> F(8, 100.0);
  const std::vector<double> K(8, 100.0);
  const std::vector<double> T = {0.5, 0.0, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5};
  const std::vector<double> df(8, 1.0);
  const std::vector<Side> side(8, Side::Call);
  std::vector<double> iv(8);
  std::vector<Status> status(8);

  const Result<std::size_t> st =
      implied_vol_batch(price, F, K, T, df, side, std::span<double>(iv), std::span<Status>(status));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < price.size(); ++i) {
    const auto scalar = implied_vol(price[i], F[i], K[i], T[i], df[i], side[i]);
    ASSERT_FALSE(scalar.has_value()) << "lane " << i;
    ASSERT_FALSE(status[i].has_value()) << "lane " << i;
    EXPECT_EQ(status[i].error().code(), scalar.error().code()) << "lane " << i;
    EXPECT_TRUE(std::isnan(iv[i])) << "lane " << i;
  }
}

TEST(Batch, ImpliedVol_LengthMismatch_InvalidArgument) {
  const std::vector<double> price(8, 1.0), F(8, 100.0), K(8, 100.0), T(8, 0.5), df(8, 1.0);
  const std::vector<Side> side(8, Side::Call);
  std::vector<double> iv(8);
  std::vector<Status> status(7); // one short
  const Result<std::size_t> st =
      implied_vol_batch(price, F, K, T, df, side, std::span<double>(iv), std::span<Status>(status));
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(Batch, ImpliedVol_ExactInPlaceAliasing_MatchesDisjoint) {
  const Grid g = make_grid(70);
  std::vector<double> price(g.F.size());
  for (std::size_t i = 0; i < g.F.size(); ++i) {
    price[i] = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
  }
  std::vector<double> ref_iv(g.F.size());
  std::vector<Status> ref_status(g.F.size());
  ASSERT_TRUE(implied_vol_batch(price, g.F, g.K, g.T, g.df, g.side, std::span<double>(ref_iv),
                                std::span<Status>(ref_status))
                  .has_value());
  // iv_out aliases the price input exactly; status_out is a disjoint buffer.
  std::vector<double> p = price;
  std::vector<Status> status(g.F.size());
  const Result<std::size_t> st =
      implied_vol_batch(std::span<const double>(p), g.F, g.K, g.T, g.df, g.side,
                        std::span<double>(p), std::span<Status>(status));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < ref_iv.size(); ++i) {
    ASSERT_EQ(status[i].has_value(), ref_status[i].has_value()) << "lane " << i;
    if (ref_status[i].has_value()) {
      EXPECT_DOUBLE_EQ(p[i], ref_iv[i]) << "lane " << i;
    } else {
      EXPECT_TRUE(std::isnan(p[i])) << "lane " << i;
    }
  }
}

// ── Greeks batch == scalar (all eight + price; and price-skipped) ─────────

TEST(Batch, Greeks_MatchesScalar_RandomGrid) {
  const Grid g = make_grid(kN);
  std::vector<double> r(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    r[i] = -std::log(g.df[i]) / g.T[i]; // recover r from df = exp(-rT)
  }
  std::vector<Greeks> greeks(kN);
  std::vector<double> price(kN);
  const Result<std::size_t> st = black76_greeks_batch(g.F, g.K, g.T, g.sigma, r, g.df, g.side,
                                         std::span<Greeks>(greeks), std::span<double>(price));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < kN; ++i) {
    const Black76Greeks s =
        black76_greeks(g.F[i], g.K[i], g.T[i], g.sigma[i], r[i], g.df[i], g.side[i]);
    expect_close(greeks[i].delta, s.greeks.delta, 1.0e-6, 1.0e-7, i);
    expect_close(greeks[i].gamma, s.greeks.gamma, 1.0e-6, 1.0e-7, i);
    expect_close(greeks[i].vega, s.greeks.vega, 1.0e-6, 1.0e-7, i);
    expect_close(greeks[i].theta, s.greeks.theta, 1.0e-6, 1.0e-7, i);
    expect_close(greeks[i].rho, s.greeks.rho, 1.0e-6, 1.0e-7, i);
    expect_close(greeks[i].vanna, s.greeks.vanna, 1.0e-6, 1.0e-7, i);
    expect_close(greeks[i].volga, s.greeks.volga, 1.0e-6, 1.0e-7, i);
    expect_close(greeks[i].charm, s.greeks.charm, 1.0e-6, 1.0e-7, i);
    expect_close(price[i], s.price, 1.0e-6, 1.0e-7, i);
  }
}

TEST(Batch, Greeks_PriceSkipped_EmptySpan) {
  const Grid g = make_grid(16);
  std::vector<double> r(16, 0.03);
  std::vector<Greeks> greeks(16);
  // Empty price span -> premium skipped, greeks still written.
  const Result<std::size_t> st =
      black76_greeks_batch(g.F, g.K, g.T, g.sigma, r, g.df, g.side, std::span<Greeks>(greeks));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < 16; ++i) {
    const Black76Greeks s =
        black76_greeks(g.F[i], g.K[i], g.T[i], g.sigma[i], r[i], g.df[i], g.side[i]);
    expect_close(greeks[i].delta, s.greeks.delta, 1.0e-6, 1.0e-7, i);
    expect_close(greeks[i].vega, s.greeks.vega, 1.0e-6, 1.0e-7, i);
  }
}

TEST(Batch, Greeks_LengthMismatch_InvalidArgument) {
  const Grid g = make_grid(8);
  std::vector<double> r(8, 0.03);
  std::vector<Greeks> greeks(7); // one short
  const Result<std::size_t> st =
      black76_greeks_batch(g.F, g.K, g.T, g.sigma, r, g.df, g.side, std::span<Greeks>(greeks));
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(Batch, Greeks_PriceOutMismatch_InvalidArgument) {
  const Grid g = make_grid(8);
  std::vector<double> r(8, 0.03);
  std::vector<Greeks> greeks(8);
  std::vector<double> price(7); // non-empty but wrong length
  const Result<std::size_t> st = black76_greeks_batch(g.F, g.K, g.T, g.sigma, r, g.df, g.side,
                                         std::span<Greeks>(greeks), std::span<double>(price));
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(Batch, Greeks_PriceExactInPlaceAliasing_MatchesDisjoint) {
  const Grid g = make_grid(70);
  std::vector<double> r(g.F.size());
  for (std::size_t i = 0; i < g.F.size(); ++i) {
    r[i] = -std::log(g.df[i]) / g.T[i];
  }
  std::vector<Greeks> ref_greeks(g.F.size());
  std::vector<double> ref_price(g.F.size());
  ASSERT_TRUE(black76_greeks_batch(g.F, g.K, g.T, g.sigma, r, g.df, g.side,
                                   std::span<Greeks>(ref_greeks), std::span<double>(ref_price))
                  .has_value());
  // price_out aliases the F input exactly; greeks_out is a disjoint buffer.
  std::vector<double> f = g.F;
  std::vector<Greeks> greeks(g.F.size());
  const Result<std::size_t> st =
      black76_greeks_batch(std::span<const double>(f), g.K, g.T, g.sigma, r, g.df, g.side,
                           std::span<Greeks>(greeks), std::span<double>(f));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < ref_price.size(); ++i) {
    EXPECT_DOUBLE_EQ(f[i], ref_price[i]) << "price lane " << i;
    EXPECT_DOUBLE_EQ(greeks[i].delta, ref_greeks[i].delta) << "delta lane " << i;
    EXPECT_DOUBLE_EQ(greeks[i].vega, ref_greeks[i].vega) << "vega lane " << i;
    EXPECT_DOUBLE_EQ(greeks[i].gamma, ref_greeks[i].gamma) << "gamma lane " << i;
  }
}

// ── eSSVI total-variance batch == scalar over a k-grid ────────────────────

TEST(Batch, EssviW_MatchesScalar_KGrid) {
  const EssviSlice slice{0.04, 1.2, -0.3, 0.5};
  std::vector<double> k_log(129);
  for (std::size_t i = 0; i < k_log.size(); ++i) {
    k_log[i] = -0.8 + 1.6 * static_cast<double>(i) / static_cast<double>(k_log.size() - 1);
  }
  std::vector<double> w(k_log.size());
  const Result<std::size_t> st = essvi_w_batch(slice, k_log, std::span<double>(w));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < k_log.size(); ++i) {
    expect_close(w[i], essvi_w(slice, k_log[i]), 1.0e-12, 1.0e-12, i);
  }
}

TEST(Batch, EssviW_LengthMismatch_InvalidArgument) {
  const EssviSlice slice{0.04, 1.2, -0.3, 0.5};
  std::vector<double> k_log(8, 0.0);
  std::vector<double> w(7); // one short
  const Result<std::size_t> st = essvi_w_batch(slice, k_log, std::span<double>(w));
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(Batch, EssviW_ExactInPlaceAliasing_MatchesDisjoint) {
  const EssviSlice slice{0.04, 1.2, -0.3, 0.5};
  std::vector<double> k_log(129);
  for (std::size_t i = 0; i < k_log.size(); ++i) {
    k_log[i] = -0.8 + 1.6 * static_cast<double>(i) / static_cast<double>(k_log.size() - 1);
  }
  std::vector<double> ref(k_log.size());
  ASSERT_TRUE(essvi_w_batch(slice, k_log, std::span<double>(ref)).has_value());
  std::vector<double> k = k_log; // both the k_log input and the w output
  const Result<std::size_t> st =
      essvi_w_batch(slice, std::span<const double>(k), std::span<double>(k));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    EXPECT_DOUBLE_EQ(k[i], ref[i]) << "lane " << i;
  }
}

TEST(Batch, Price_NonfiniteAndInvalidDomainLanesMatchScalarSemantics) {
  Grid g = make_grid(12);
  inject_invalid_domains(g);
  std::vector<double> price(g.F.size());
  const Result<std::size_t> st =
      black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(price));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < g.F.size(); ++i) {
    const double scalar = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
    expect_scalar_semantics(price[i], scalar, 1.0e-6, 1.0e-7, i);
  }
}

TEST(Batch, ValueAndVega_NonfiniteDomainsAndSharedRootMatchScalarSemantics) {
  Grid g = make_grid(12);
  inject_invalid_domains(g);
  std::vector<double> value(g.F.size());
  std::vector<double> vega(g.F.size());
  constexpr double kT = 0.5;
  Result<std::size_t> st = black76_value_and_vega_batch(g.F, g.K, kT, g.sigma, g.df, g.side,
                                           std::span<double>(value), std::span<double>(vega));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < g.F.size(); ++i) {
    const Black76ValueVega scalar =
        black76_value_and_vega(g.F[i], g.K[i], kT, g.sigma[i], g.df[i], g.side[i]);
    expect_scalar_semantics(value[i], scalar.price, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(vega[i], scalar.vega, 1.0e-5, 1.0e-7, i);
  }

  const Grid valid = make_grid(8);
  const double infinite_root = std::numeric_limits<double>::infinity();
  st = black76_value_and_vega_batch(valid.F, valid.K, kT, valid.sigma, valid.df, valid.side,
                                    std::span<double>(value.data(), valid.F.size()),
                                    std::span<double>(vega.data(), valid.F.size()), infinite_root);
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < valid.F.size(); ++i) {
    const Black76ValueVega scalar = black76_value_and_vega(
        valid.F[i], valid.K[i], kT, valid.sigma[i], valid.df[i], valid.side[i], infinite_root);
    expect_scalar_semantics(value[i], scalar.price, 0.0, 0.0, i);
    expect_scalar_semantics(vega[i], scalar.vega, 0.0, 0.0, i);
  }
}

TEST(Batch, Greeks_NonfiniteAndInvalidDomainLanesMatchScalarSemantics) {
  Grid g = make_grid(14);
  inject_invalid_domains(g);
  std::vector<double> r(g.F.size(), 0.03);
  r[12] = std::numeric_limits<double>::quiet_NaN();
  r[13] = std::numeric_limits<double>::infinity();
  std::vector<Greeks> greeks(g.F.size());
  std::vector<double> price(g.F.size());
  const Result<std::size_t> st = black76_greeks_batch(g.F, g.K, g.T, g.sigma, r, g.df, g.side,
                                         std::span<Greeks>(greeks), std::span<double>(price));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < g.F.size(); ++i) {
    const Black76Greeks scalar =
        black76_greeks(g.F[i], g.K[i], g.T[i], g.sigma[i], r[i], g.df[i], g.side[i]);
    expect_scalar_semantics(price[i], scalar.price, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].delta, scalar.greeks.delta, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].gamma, scalar.greeks.gamma, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].vega, scalar.greeks.vega, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].theta, scalar.greeks.theta, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].rho, scalar.greeks.rho, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].vanna, scalar.greeks.vanna, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].volga, scalar.greeks.volga, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].charm, scalar.greeks.charm, 1.0e-6, 1.0e-7, i);
  }
}

TEST(Batch, EssviW_NonfiniteGridAndInvalidSliceMatchScalarSemantics) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<double> k_log(17, 0.1);
  for (std::size_t i = 0; i < k_log.size(); ++i) {
    k_log[i] = (i % 3u == 0u) ? nan : ((i % 3u == 1u) ? inf : -inf);
  }
  const EssviSlice slice{0.04, 1.2, -0.3, 0.5};
  std::vector<double> w(k_log.size());
  Result<std::size_t> st = essvi_w_batch(slice, k_log, std::span<double>(w));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < k_log.size(); ++i) {
    expect_scalar_semantics(w[i], essvi_w(slice, k_log[i]), 0.0, 0.0, i);
  }

  const EssviSlice invalid_slice{0.04, 1.2, nan, 0.5};
  const std::vector<double> finite_k(k_log.size(), 0.1);
  st = essvi_w_batch(invalid_slice, finite_k, std::span<double>(w));
  ASSERT_TRUE(st.has_value());
  for (std::size_t i = 0; i < finite_k.size(); ++i) {
    expect_scalar_semantics(w[i], essvi_w(invalid_slice, finite_k[i]), 0.0, 0.0, i);
  }
}

TEST(Batch, NonCallSideSemanticsAreInvariantAcrossVectorBlocksAndTail) {
  constexpr std::size_t kRows = 9;
  const std::vector<double> F(kRows, 100.0);
  const std::vector<double> K(kRows, 105.0);
  const std::vector<double> T(kRows, 0.5);
  const std::vector<double> sigma(kRows, 0.25);
  const std::vector<double> r(kRows, 0.03);
  const std::vector<double> df(kRows, std::exp(-0.03 * T.front()));
  const Side non_call = static_cast<Side>(0xffU);
  const std::vector<Side> side(kRows, non_call);
  std::vector<double> price(kRows);
  std::vector<double> vega(kRows);
  std::vector<Greeks> greeks(kRows);

  ASSERT_TRUE(black76_price_batch(F, K, T, sigma, df, side, std::span<double>(price)));
  for (std::size_t i = 0; i < kRows; ++i) {
    expect_scalar_semantics(price[i], black76_price(F[i], K[i], T[i], sigma[i], df[i], non_call),
                            1.0e-6, 1.0e-7, i);
  }

  ASSERT_TRUE(black76_value_and_vega_batch(F, K, T.front(), sigma, df, side,
                                           std::span<double>(price), std::span<double>(vega)));
  ASSERT_TRUE(black76_greeks_batch(F, K, T, sigma, r, df, side, std::span<Greeks>(greeks)));
  for (std::size_t i = 0; i < kRows; ++i) {
    const Black76ValueVega vv = black76_value_and_vega(F[i], K[i], T[i], sigma[i], df[i], non_call);
    const Black76Greeks gg = black76_greeks(F[i], K[i], T[i], sigma[i], r[i], df[i], non_call);
    expect_scalar_semantics(price[i], vv.price, 1.0e-6, 1.0e-7, i);
    expect_scalar_semantics(vega[i], vv.vega, 1.0e-5, 1.0e-7, i);
    expect_scalar_semantics(greeks[i].delta, gg.greeks.delta, 1.0e-6, 1.0e-7, i);
  }
}

// ── 4.3 error model: every entry reports through Result<std::size_t> ──────
//
// The six entries above used to return `Status` and leave the caller to
// recompute "how many lanes did that write" from an input span. The failure
// branch of that shape was reachable only as "not Ok" — no test ever read the
// message, so a boundary rejection could have been renamed or merged into a
// neighbour's without anything failing. These two tests pin BOTH halves of the
// converted shape: the exact Error each rejection carries, and the lane count a
// success carries.

TEST(BatchErrorModel, EveryEntryCarriesItsExactRejectionThroughResult) {
  const Grid g = make_grid(8);
  const EssviSlice slice{0.04, 1.2, -0.3, 0.5};
  const std::vector<double> r(8, 0.03);
  const std::vector<double> ln_fk(8, 0.0);
  std::vector<double> price(8), value(8), vega(8), iv(8), w(8);
  std::vector<Greeks> greeks(8);
  std::vector<Status> lane_status(8);

  // Each call is one span short somewhere, which is the shared "span length
  // mismatch" rejection; the message names the entry that refused.
  const Result<std::size_t> px = black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side,
                                                     std::span<double>(price.data(), 7));
  ASSERT_FALSE(px.has_value());
  EXPECT_EQ(px.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(px.error().message(), "black76_price_batch: span length mismatch");

  const Result<std::size_t> lnfk = black76_price_from_lnfk_batch(
      g.F, g.K, 0.35, std::sqrt(0.35), std::span<const double>(g.sigma.data(), 7),
      std::exp(-0.05 * 0.35), ln_fk, g.side, std::span<double>(price));
  ASSERT_FALSE(lnfk.has_value());
  EXPECT_EQ(lnfk.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(lnfk.error().message(), "black76_price_from_lnfk_batch: span length mismatch");

  const Result<std::size_t> vv =
      black76_value_and_vega_batch(g.F, g.K, 0.5, g.sigma, g.df, g.side, std::span<double>(value),
                                   std::span<double>(vega.data(), 7));
  ASSERT_FALSE(vv.has_value());
  EXPECT_EQ(vv.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(vv.error().message(), "black76_value_and_vega_batch: span length mismatch");

  const Result<std::size_t> ivr =
      implied_vol_batch(price, g.F, g.K, g.T, g.df, g.side, std::span<double>(iv),
                        std::span<Status>(lane_status.data(), 7));
  ASSERT_FALSE(ivr.has_value());
  EXPECT_EQ(ivr.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(ivr.error().message(), "implied_vol_batch: span length mismatch");

  const Result<std::size_t> gk =
      black76_greeks_batch(g.F, g.K, g.T, g.sigma, r, g.df, g.side,
                           std::span<Greeks>(greeks.data(), 7), std::span<double>(price));
  ASSERT_FALSE(gk.has_value());
  EXPECT_EQ(gk.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(gk.error().message(), "black76_greeks_batch: span length mismatch");

  const Result<std::size_t> ew =
      essvi_w_batch(slice, g.F, std::span<double>(w.data(), 7));
  ASSERT_FALSE(ew.has_value());
  EXPECT_EQ(ew.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(ew.error().message(), "essvi_w_batch: span length mismatch");

  // The second rejection class (staggered output↔input overlap) keeps its own
  // distinct message, so the two are not collapsible.
  std::vector<double> buf(g.F.size() + 1);
  std::copy(g.F.begin(), g.F.end(), buf.begin());
  const Result<std::size_t> alias = black76_price_batch(
      std::span<const double>(buf.data(), g.F.size()), g.K, g.T, g.sigma, g.df, g.side,
      std::span<double>(buf.data() + 1, g.F.size()));
  ASSERT_FALSE(alias.has_value());
  EXPECT_EQ(alias.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(alias.error().message(),
            "black76_price_batch: output overlaps an input (only exact in-place aliasing is "
            "permitted)");
}

TEST(BatchErrorModel, EveryEntryCarriesTheLaneCountItWrote) {
  constexpr std::size_t kRows = 16;
  const Grid g = make_grid(kRows);
  const EssviSlice slice{0.04, 1.2, -0.3, 0.5};
  const std::vector<double> r(kRows, 0.03);
  const std::vector<double> ln_fk(kRows, 0.0);
  std::vector<double> price(kRows), value(kRows), vega(kRows), iv(kRows), w(kRows);
  std::vector<Greeks> greeks(kRows);
  std::vector<Status> lane_status(kRows);

  const Result<std::size_t> px =
      black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(price));
  ASSERT_TRUE(px.has_value());
  EXPECT_EQ(*px, kRows);

  const Result<std::size_t> lnfk =
      black76_price_from_lnfk_batch(g.F, g.K, 0.35, std::sqrt(0.35), g.sigma,
                                    std::exp(-0.05 * 0.35), ln_fk, g.side,
                                    std::span<double>(value));
  ASSERT_TRUE(lnfk.has_value());
  EXPECT_EQ(*lnfk, kRows);

  const Result<std::size_t> vv = black76_value_and_vega_batch(
      g.F, g.K, 0.5, g.sigma, g.df, g.side, std::span<double>(value), std::span<double>(vega));
  ASSERT_TRUE(vv.has_value());
  EXPECT_EQ(*vv, kRows);

  const Result<std::size_t> ivr = implied_vol_batch(
      price, g.F, g.K, g.T, g.df, g.side, std::span<double>(iv), std::span<Status>(lane_status));
  ASSERT_TRUE(ivr.has_value());
  EXPECT_EQ(*ivr, kRows);

  const Result<std::size_t> gk = black76_greeks_batch(g.F, g.K, g.T, g.sigma, r, g.df, g.side,
                                                      std::span<Greeks>(greeks));
  ASSERT_TRUE(gk.has_value());
  EXPECT_EQ(*gk, kRows);

  const Result<std::size_t> ew = essvi_w_batch(slice, g.F, std::span<double>(w));
  ASSERT_TRUE(ew.has_value());
  EXPECT_EQ(*ew, kRows);

  // An empty batch is a well-formed zero-lane call, not a rejection.
  const Result<std::size_t> empty =
      essvi_w_batch(slice, std::span<const double>{}, std::span<double>{});
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(*empty, 0u);
}

} // namespace
