// P3.4 gates for the public SoA American batch APIs (american_batch.hpp).
//
// american_price_batch groups the genuine early-exercise lanes into a homogeneous
// internal-put pack and dispatches T13's REAL simd::american_put_boundary_batch;
// degenerate / European / double-continuation / invalid lanes patch to the exact
// scalar andersen_lake. These tests assert:
//   * ForceScalar: every lane is BIT-IDENTICAL to per-contract andersen_lake, all
//     routes Scalar, fallback rate 100% (the T13 scalar default the batch inherits);
//   * ForceAvx2 (AVX2 hosts): Scalar-route lanes stay bit-identical, Avx2-route
//     lanes match the reference to T13's stress gate, fallback rate < 100%;
//   * per-lane status/route + preserved output order + tail residues + empty no-op.
//
// american_greeks_batch is the SoA surface over the EXISTING scalar T9 Greek route
// (american_greeks_fd), so the batch is bit-identical to a per-contract loop; the
// GreekFieldMask writes only the requested columns; every lane's route is Scalar
// (there is no vectorized Greek stencil — documented in the header).

#include "atx/vol/american_batch.hpp"

#include "atx/vol/american.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// A mixed book: American puts (r>0), American calls (q>0), European put/call
// corners, a double-continuation (Unsupported) put, and degenerate lanes.
struct Book {
  std::vector<double> S, K, T, sigma, r, q;
  std::vector<Side> side;
  [[nodiscard]] std::size_t size() const { return S.size(); }
  void push(double s, double k, double t, double v, double rr, double qq,
            Side sd) {
    S.push_back(s); K.push_back(k); T.push_back(t);
    sigma.push_back(v); r.push_back(rr); q.push_back(qq); side.push_back(sd);
  }
  [[nodiscard]] AmericanBatchInput view() const {
    return AmericanBatchInput{S, K, T, sigma, r, q, side};
  }
};

Book make_book() {
  Book b;
  const double S = 100.0;
  // American puts (r>0) across moneyness/tenor/vol.
  for (double m : {0.8, 0.95, 1.0, 1.1, 1.25}) {
    for (double t : {0.1, 0.5, 1.0}) {
      for (double v : {0.15, 0.30}) {
        b.push(S, S * m, t, v, 0.05, 0.01, Side::Put);
      }
    }
  }
  // American calls (q>0 -> early-exercise regime American).
  for (double m : {0.85, 1.0, 1.15}) {
    for (double v : {0.20, 0.40}) {
      b.push(S, S * m, 1.0, v, 0.02, 0.06, Side::Call);
    }
  }
  // European put corner (r<=0 && r<=q).
  b.push(S, 105.0, 1.0, 0.25, -0.01, 0.03, Side::Put);
  // European call corner (q<=0 && q<=r).
  b.push(S, 95.0, 1.0, 0.25, 0.03, -0.01, Side::Call);
  // Double-continuation / Unsupported put (yield < rate <= 0).
  b.push(S, 100.0, 1.0, 0.25, -0.01, -0.05, Side::Put);
  // Degenerate lanes.
  b.push(S, 105.0, 0.0, 0.20, 0.05, 0.0, Side::Put);   // T = 0
  b.push(S, 95.0, 1.0, 0.0, 0.05, 0.0, Side::Call);    // sigma = 0
  return b;
}

double ref_price(const Book& b, std::size_t i) {
  const Result<double> r =
      andersen_lake(b.S[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.q[i], b.side[i]);
  return r.has_value() ? *r : std::numeric_limits<double>::quiet_NaN();
}

// Reset the process ISA override after a test twiddles it via a kernel.
struct IsaGuard {
  ~IsaGuard() { simd::set_simd_isa_override(simd::SimdIsa::Auto); }
};

// ── ForceScalar: bit-identical to per-contract andersen_lake ──────────────
TEST(AmericanPriceBatch, MatchesScalarBitIdentical) {
  IsaGuard g;
  const Book b = make_book();
  const std::size_t n = b.size();

  PricingKernel kernel;
  kernel.isa = simd::SimdIsa::ForceScalar;
  PricingWorkspace ws;
  PriceBatchOutput out;
  ASSERT_TRUE(american_price_batch(b.view(), out, kernel, ws).has_value());
  ASSERT_EQ(out.size(), n);

  for (std::size_t i = 0; i < n; ++i) {
    const double want = ref_price(b, i);
    EXPECT_EQ(out.route[i], simd::SimdRoute::Scalar) << "i=" << i;
    if (std::isnan(want)) {
      EXPECT_TRUE(std::isnan(out.price[i])) << "i=" << i;
      EXPECT_EQ(out.status[i], LaneStatus::Unsupported) << "i=" << i;
    } else {
      EXPECT_EQ(out.price[i], want) << "i=" << i; // bit-for-bit
      EXPECT_EQ(out.status[i], LaneStatus::Ok) << "i=" << i;
    }
  }
  // Every lane ran scalar -> the batch inherits T13's scalar default.
  EXPECT_EQ(out.scalar_fallback_rate(), 1.0);
}

// ── ForceAvx2: Scalar lanes bit-exact, Avx2 lanes within T13's stress gate ─
TEST(AmericanPriceBatch, ForceAvx2MatchesReference) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  IsaGuard g;
  const Book b = make_book();
  const std::size_t n = b.size();

  PricingKernel kernel;
  kernel.isa = simd::SimdIsa::ForceAvx2;
  PricingWorkspace ws;
  PriceBatchOutput out;
  ASSERT_TRUE(american_price_batch(b.view(), out, kernel, ws).has_value());

  std::size_t n_avx2 = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double want = ref_price(b, i);
    if (out.route[i] == simd::SimdRoute::Avx2) {
      ++n_avx2;
      ASSERT_FALSE(std::isnan(want)) << "i=" << i;
      EXPECT_LE(std::abs(out.price[i] - want), 1e-3) << "i=" << i; // T13 stress gate
      EXPECT_EQ(out.status[i], LaneStatus::Ok) << "i=" << i;
    } else {
      // Patched lanes go through the exact scalar andersen_lake.
      if (std::isnan(want)) {
        EXPECT_TRUE(std::isnan(out.price[i])) << "i=" << i;
      } else {
        EXPECT_EQ(out.price[i], want) << "i=" << i;
      }
    }
  }
  EXPECT_GT(n_avx2, 0u); // genuine American lanes exercised the vector kernel
  EXPECT_LT(out.scalar_fallback_rate(), 1.0);
}

// ── Output order preserved + every n%4 tail residue handled ───────────────
TEST(AmericanPriceBatch, TailResiduesPreserveOrder) {
  IsaGuard g;
  const Book b = make_book();
  PricingKernel kernel;
  kernel.isa = simd::SimdIsa::ForceScalar;
  PricingWorkspace ws;
  for (std::size_t n = 1; n <= 13; ++n) {
    AmericanBatchInput in;
    in.S = {b.S.data(), n};
    in.K = {b.K.data(), n};
    in.T = {b.T.data(), n};
    in.sigma = {b.sigma.data(), n};
    in.r = {b.r.data(), n};
    in.q = {b.q.data(), n};
    in.side = {b.side.data(), n};

    PriceBatchOutput out;
    ASSERT_TRUE(american_price_batch(in, out, kernel, ws).has_value());
    ASSERT_EQ(out.size(), n);
    for (std::size_t i = 0; i < n; ++i) {
      const double want = ref_price(b, i);
      if (std::isnan(want)) {
        EXPECT_TRUE(std::isnan(out.price[i])) << "n=" << n << " i=" << i;
      } else {
        EXPECT_EQ(out.price[i], want) << "n=" << n << " i=" << i;
      }
    }
  }
}

TEST(AmericanPriceBatch, EmptyIsNoOp) {
  IsaGuard g;
  AmericanBatchInput in; // all empty spans
  PricingKernel kernel;
  PricingWorkspace ws;
  PriceBatchOutput out;
  ASSERT_TRUE(american_price_batch(in, out, kernel, ws).has_value());
  EXPECT_EQ(out.size(), 0u);
  EXPECT_EQ(out.scalar_fallback_rate(), 0.0);
}

TEST(AmericanPriceBatch, InconsistentSpansRejected) {
  IsaGuard g;
  std::vector<double> a(4, 1.0), sh(3, 1.0);
  std::vector<Side> sd(4, Side::Put);
  AmericanBatchInput in{a, sh, a, a, a, a, sd}; // K shorter
  PricingKernel kernel;
  PricingWorkspace ws;
  PriceBatchOutput out;
  EXPECT_FALSE(american_price_batch(in, out, kernel, ws).has_value());
}

// ── Greeks batch: bit-identical to per-contract american_greeks_fd ────────
Book make_american_greeks_book() {
  Book b;
  const double S = 100.0;
  for (double m : {0.85, 0.95, 1.0, 1.05, 1.2}) {
    for (double t : {0.25, 1.0}) {
      for (double v : {0.20, 0.35}) {
        b.push(S, S * m, t, v, 0.05, 0.0, Side::Put);
        b.push(S, S * m, t, v, 0.02, 0.06, Side::Call); // American call (q>0)
      }
    }
  }
  return b;
}

TEST(AmericanGreeksBatch, MatchesScalarFd) {
  const Book b = make_american_greeks_book();
  const std::size_t n = b.size();

  std::vector<double> dl(n), gm(n), vg(n), th(n), rh(n), vn(n), vl(n), cm(n),
      px(n);
  simd::GreeksBatchSoA out{dl.data(), gm.data(), vg.data(), th.data(),
                           rh.data(), vn.data(), vl.data(), cm.data(),
                           px.data()};
  PricingKernel kernel; // fd route (default)
  PricingWorkspace ws;
  ASSERT_TRUE(
      american_greeks_batch(b.view(), GreekFieldMask::All, out, kernel, ws)
          .has_value());

  for (std::size_t i = 0; i < n; ++i) {
    const Result<AmericanGreeks> ref = american_greeks_fd(
        b.S[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.q[i], b.side[i]);
    ASSERT_TRUE(ref.has_value()) << "i=" << i;
    EXPECT_EQ(dl[i], ref->delta) << "i=" << i;
    EXPECT_EQ(gm[i], ref->gamma) << "i=" << i;
    EXPECT_EQ(vg[i], ref->vega) << "i=" << i;
    EXPECT_EQ(th[i], ref->theta) << "i=" << i;
    EXPECT_EQ(rh[i], ref->rho) << "i=" << i;
    EXPECT_EQ(vn[i], ref->vanna) << "i=" << i;
    EXPECT_EQ(vl[i], ref->volga) << "i=" << i;
    EXPECT_EQ(cm[i], ref->charm) << "i=" << i;
    EXPECT_EQ(px[i], ref->price) << "i=" << i;
    EXPECT_EQ(ws.lane_status_view()[i], LaneStatus::Ok) << "i=" << i;
    EXPECT_EQ(ws.lane_route_view()[i], simd::SimdRoute::Scalar) << "i=" << i;
  }
}

TEST(AmericanGreeksBatch, FieldMaskWritesOnlyRequested) {
  const Book b = make_american_greeks_book();
  const std::size_t n = b.size();
  constexpr double kSentinel = -987654.0;
  std::vector<double> dl(n, kSentinel), gm(n, kSentinel), vg(n, kSentinel),
      th(n, kSentinel), rh(n, kSentinel), vn(n, kSentinel), vl(n, kSentinel),
      cm(n, kSentinel), px(n, kSentinel);
  simd::GreeksBatchSoA out{dl.data(), gm.data(), vg.data(), th.data(),
                           rh.data(), vn.data(), vl.data(), cm.data(),
                           px.data()};
  PricingKernel kernel;
  PricingWorkspace ws;
  const GreekFieldMask fields =
      GreekFieldMask::Delta | GreekFieldMask::Vega | GreekFieldMask::Price;
  ASSERT_TRUE(
      american_greeks_batch(b.view(), fields, out, kernel, ws).has_value());

  for (std::size_t i = 0; i < n; ++i) {
    const Result<AmericanGreeks> ref = american_greeks_fd(
        b.S[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.q[i], b.side[i]);
    ASSERT_TRUE(ref.has_value()) << "i=" << i;
    EXPECT_EQ(dl[i], ref->delta) << "i=" << i;
    EXPECT_EQ(vg[i], ref->vega) << "i=" << i;
    EXPECT_EQ(px[i], ref->price) << "i=" << i;
    // Unrequested columns untouched.
    EXPECT_EQ(gm[i], kSentinel) << "i=" << i;
    EXPECT_EQ(th[i], kSentinel) << "i=" << i;
    EXPECT_EQ(rh[i], kSentinel) << "i=" << i;
    EXPECT_EQ(vn[i], kSentinel) << "i=" << i;
    EXPECT_EQ(vl[i], kSentinel) << "i=" << i;
    EXPECT_EQ(cm[i], kSentinel) << "i=" << i;
  }
}

TEST(AmericanGreeksBatch, UnsupportedLaneReportsStatus) {
  Book b;
  // Double-continuation put -> american_greeks_fd propagates NotImplemented.
  b.push(100.0, 100.0, 1.0, 0.25, -0.01, -0.05, Side::Put);
  const std::size_t n = b.size();
  std::vector<double> px(n, 0.0);
  simd::GreeksBatchSoA out;
  out.price = px.data();
  PricingKernel kernel;
  PricingWorkspace ws;
  ASSERT_TRUE(
      american_greeks_batch(b.view(), GreekFieldMask::Price, out, kernel, ws)
          .has_value());
  EXPECT_EQ(ws.lane_status_view()[0], LaneStatus::Unsupported);
  EXPECT_TRUE(std::isnan(px[0]));
}

TEST(AmericanGreeksBatch, EmptyIsNoOp) {
  AmericanBatchInput in;
  simd::GreeksBatchSoA out;
  PricingKernel kernel;
  PricingWorkspace ws;
  ASSERT_TRUE(
      american_greeks_batch(in, GreekFieldMask::All, out, kernel, ws)
          .has_value());
}

} // namespace
} // namespace atx::vol
